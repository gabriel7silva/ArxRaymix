/*
 * Arx Remaster — Native RTX Remix Renderer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixRenderer.h"

#if ARX_HAVE_RTX_REMIX

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/Core.h"
#include "graphics/Renderer.h"
#include "graphics/remix/RemixApi.h"
#include "graphics/remix/RemixConvert.h"
#include "graphics/remix/RemixScene.h"
#include "graphics/remix/RemixTextures.h"
#include "io/log/Logger.h"

namespace remix {
namespace {

/*!
 * FNV-1a over the whole batch payload.
 *
 * Remix pairs meshes across frames by hash, so this has to be stable while the
 * UI does not change and distinct between batches. Keying on the texture plus
 * the vertex count (the first attempt) collided constantly: a menu page is
 * mostly 6-vertex quads sharing one atlas, so unrelated batches hashed equal
 * and Remix kept one geometry for all of them.
 *
 * remixapi_HardcodedVertex is 9 tightly packed 4-byte fields, all of which we
 * always assign, so there are no padding bytes to make this nondeterministic.
 */
uint64_t uiBatchHash(const UI2DDrawCall & batch) {
	
	uint64_t hash = 0xcbf29ce484222325ull;
	
	auto mix = [&hash](const void * data, size_t bytes) {
		const unsigned char * bytePtr = static_cast<const unsigned char *>(data);
		for(size_t i = 0; i < bytes; ++i) {
			hash ^= bytePtr[i];
			hash *= 0x100000001b3ull;
		}
	};
	
	const uintptr_t texture = reinterpret_cast<uintptr_t>(batch.texture);
	mix(&texture, sizeof(texture));
	mix(batch.vertices.data(), batch.vertices.size() * sizeof(remixapi_HardcodedVertex));
	mix(batch.indices.data(), batch.indices.size() * sizeof(uint32_t));
	
	return hash;
}

} // namespace

// -----------------------------------------------------------------------------
// RemixTexture
// -----------------------------------------------------------------------------

RemixTexture::RemixTexture() = default;
RemixTexture::~RemixTexture() = default;

bool RemixTexture::create() {
	m_storedSize = m_size;
	return m_image.isValid();
}

void RemixTexture::upload() {
	/*
	 * There is no GPU copy to refresh, but this is the only notification that
	 * the pixels changed. PackedTexture only calls it on a dirty atlas, so it is
	 * exactly the signal needed to rebuild the material: without it the font
	 * atlas keeps growing as new glyphs are used while Remix still points at the
	 * dump taken when the first page was drawn, and later menu pages come up
	 * with missing or wrong glyphs.
	 */
	invalidateUiMaterial(this);
}

void RemixTexture::destroy() {
	// Keep m_image in CPU RAM so texture data is preserved across restore cycles
}

// -----------------------------------------------------------------------------
// RemixTextureStage
// -----------------------------------------------------------------------------

RemixTextureStage::RemixTextureStage(unsigned stage)
	: TextureStage(stage)
{ }

// -----------------------------------------------------------------------------
// RemixVertexBuffer
// -----------------------------------------------------------------------------

template <class Vertex>
class RemixVertexBuffer final : public VertexBuffer<Vertex> {
public:
	explicit RemixVertexBuffer(size_t capacity, RemixRenderer * renderer)
		: VertexBuffer<Vertex>(capacity)
		, m_renderer(renderer)
		, m_data(capacity)
	{ }

	void setData(const Vertex * vertices, size_t count, size_t offset = 0, BufferFlags flags = 0) override {
		ARX_UNUSED(flags);
		if(offset + count <= m_data.size()) {
			std::copy(vertices, vertices + count, m_data.begin() + offset);
		}
	}

	Vertex * lock(BufferFlags flags = 0, size_t offset = 0, size_t count = size_t(-1)) override {
		ARX_UNUSED(flags);
		ARX_UNUSED(count);
		if(offset < m_data.size()) {
			return m_data.data() + offset;
		}
		return m_data.data();
	}

	void unlock() override { }

	void draw(Renderer::Primitive primitive, size_t count, size_t offset = 0) const override;
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override;

private:
	RemixRenderer * m_renderer;
	mutable std::vector<Vertex> m_data;
};

template <>
void RemixVertexBuffer<TexturedVertex>::draw(Renderer::Primitive primitive, size_t count, size_t offset) const {
	if(m_renderer && offset + count <= m_data.size()) {
		m_renderer->queue2DVertices(primitive, m_data.data() + offset, count);
	}
}

template <>
void RemixVertexBuffer<TexturedVertex>::drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
                                                   const unsigned short * indices, size_t nbindices) const {
	ARX_UNUSED(count);
	ARX_UNUSED(offset);
	if(!m_renderer || !indices || nbindices == 0) {
		return;
	}
	std::vector<TexturedVertex> expanded(nbindices);
	for(size_t i = 0; i < nbindices; ++i) {
		const unsigned short idx = indices[i];
		if(idx < m_data.size()) {
			expanded[i] = m_data[idx];
		}
	}
	m_renderer->queue2DVertices(primitive, expanded.data(), expanded.size());
}

template <class Vertex>
void RemixVertexBuffer<Vertex>::draw(Renderer::Primitive primitive, size_t count, size_t offset) const {
	// World geometry is exported from g_tiles in RemixScene, not from these
	// room vertex buffers — the two cover the same polys.
	ARX_UNUSED(primitive);
	ARX_UNUSED(count);
	ARX_UNUSED(offset);
}

template <class Vertex>
void RemixVertexBuffer<Vertex>::drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
                                            const unsigned short * indices, size_t nbindices) const {
	ARX_UNUSED(primitive);
	ARX_UNUSED(count);
	ARX_UNUSED(offset);
	ARX_UNUSED(indices);
	ARX_UNUSED(nbindices);
}

// -----------------------------------------------------------------------------
// RemixRenderer
// -----------------------------------------------------------------------------

RemixRenderer::RemixRenderer() {
	// Provide 4 texture stages so engine checks (>= 3 stages) pass cleanly
	for(unsigned i = 0; i < 4; ++i) {
		m_TextureStages.push_back(std::make_unique<RemixTextureStage>(i));
	}
}

RemixRenderer::~RemixRenderer() = default;

void RemixRenderer::initialize() {
	LogInfo << "Using Raymix (RTX Remix C API) renderer";
	m_initialized = true;
	onRendererInit();
}

void RemixRenderer::beforeResize(bool wasOrIsFullscreen) {
	ARX_UNUSED(wasOrIsFullscreen);
}

void RemixRenderer::afterResize() {
	m_initialized = true;
}

void RemixRenderer::SetViewMatrix(const glm::mat4x4 & matView) {
	m_view = matView;
}

void RemixRenderer::SetProjectionMatrix(const glm::mat4x4 & matProj) {
	m_proj = matProj;
}

void RemixRenderer::ReleaseAllTextures() { }
void RemixRenderer::RestoreAllTextures() { }
void RemixRenderer::reloadColorKeyTextures() { }

Texture * RemixRenderer::createTexture() {
	return new RemixTexture();
}

void RemixRenderer::SetViewport(const Rect & viewport) {
	m_viewport = viewport;
}

void RemixRenderer::SetScissor(const Rect & rect) {
	m_scissor = rect;
}

void RemixRenderer::Clear(BufferFlags bufferFlags, Color clearColor, float clearDepth,
                           size_t nrects, Rect * rect) {
	ARX_UNUSED(bufferFlags);
	ARX_UNUSED(clearColor);
	ARX_UNUSED(clearDepth);
	ARX_UNUSED(nrects);
	ARX_UNUSED(rect);
}

void RemixRenderer::SetFogColor(Color color) {
	m_fogColor = color;
}

void RemixRenderer::SetFogParams(float fogStart, float fogEnd) {
	m_fogStart = fogStart;
	m_fogEnd = fogEnd;
}

void RemixRenderer::SetAntialiasing(bool enable) {
	m_antialiasing = enable;
}

void RemixRenderer::SetFillMode(FillMode mode) {
	m_fillMode = mode;
}

std::unique_ptr<VertexBuffer<TexturedVertex>> RemixRenderer::createVertexBufferTL(size_t capacity, BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<RemixVertexBuffer<TexturedVertex>>(capacity, this);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX>> RemixRenderer::createVertexBuffer(size_t capacity, BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<RemixVertexBuffer<SMY_VERTEX>>(capacity, this);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX3>> RemixRenderer::createVertexBuffer3(size_t capacity, BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<RemixVertexBuffer<SMY_VERTEX3>>(capacity, this);
}

void RemixRenderer::drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
                                 unsigned short * indices, size_t nindices) {
	ARX_UNUSED(nvertices);
	if(!indices || nindices == 0) {
		return;
	}
	std::vector<TexturedVertex> expanded(nindices);
	for(size_t i = 0; i < nindices; ++i) {
		expanded[i] = vertices[indices[i]];
	}
	queue2DVertices(primitive, expanded.data(), expanded.size());
}

bool RemixRenderer::getSnapshot(Image & image) {
	ARX_UNUSED(image);
	return false;
}

bool RemixRenderer::getSnapshot(Image & image, size_t width, size_t height) {
	ARX_UNUSED(image);
	ARX_UNUSED(width);
	ARX_UNUSED(height);
	return false;
}

void RemixRenderer::queue2DVertices(Primitive primitive, const TexturedVertex * vertices, size_t count) {
	if(!vertices || count < 3) {
		return;
	}

	int winW = 1600, winH = 900;
	if(SDL_Window * win = previewSdlWindow()) {
		SDL_GetWindowSize(win, &winW, &winH);
	}
	winW = std::max(winW, 1);
	winH = std::max(winH, 1);

	const float screenW = float(g_size.width() > 0 ? g_size.width() : winW);
	const float screenH = float(g_size.height() > 0 ? g_size.height() : winH);
	const float aspect = screenW / std::max(screenH, 1.f);

	Vec3f camPos(0.f), fwd(0.f, 0.f, 1.f), up(0.f, 1.f, 0.f), right(1.f, 0.f, 0.f);
	float fovY = glm::radians(60.f);
	getCurrentCameraBasis(camPos, fwd, up, right, fovY);

	const float dist = 0.2f;
	const float tanHalf = std::tan(fovY * 0.5f);

	UI2DDrawCall call;
	call.texture = GetTexture(0);
	// HUD bitmaps use w=1. Sprites store p*w with w≠1 and additive blend.
	// Tagging a HUD batch as PARTICLE+CUTOUT is what smeared the interface.
	bool additive = false;
	bool noBlend = false;
	if(GRenderer) {
		const RenderState rs = GRenderer->getRenderState();
		additive = rs.isBlendEnabled() && rs.getBlendDst() == BlendOne;
		noBlend = !rs.isBlendEnabled();
	}
	const bool looksSprite = (vertices[0].w > 1.05f)
	                         || (vertices[0].w > 1e-6f && vertices[0].w < 0.95f);
	call.particle = additive && looksSprite;
	call.opaque = noBlend;

	auto toRemixUI = [&](const TexturedVertex & v) -> remixapi_HardcodedVertex {
		// Arx TL sprites store homogeneous screen coords (p.xy * w). HUD uses w=1.
		const float invW = (v.w > 1e-6f) ? (1.f / v.w) : 1.f;
		const float sx = v.p.x * invW;
		const float sy = v.p.y * invW;
		const float sz = v.p.z * invW;
		const float nx = (sx / screenW) * 2.0f - 1.0f;
		const float ny = 1.0f - (sy / screenH) * 2.0f;
		const float zFraction = std::clamp(sz, 0.f, 1.f);
		// In Arx, z=0 is foreground (text/cursor) and z=1 is background (wallpaper).
		// We place z=1 further from camera (dist * 1.08) and z=0 closer (dist * 1.00).
		const float vertexDist = dist * (1.0f + zFraction * 0.08f);
		const float vHalfH = vertexDist * tanHalf;
		const float vHalfW = vHalfH * aspect;
		const Vec3f p = camPos + fwd * vertexDist + right * (nx * vHalfW) + up * (ny * vHalfH);

		remixapi_HardcodedVertex hv {};
		hv.position[0] = p.x;
		hv.position[1] = p.y;
		hv.position[2] = p.z;
		hv.normal[0] = -fwd.x;
		hv.normal[1] = -fwd.y;
		hv.normal[2] = -fwd.z;
		hv.texcoord[0] = v.uv.x;
		hv.texcoord[1] = v.uv.y;
		hv.color = v.color.t ? v.color.t : 0xFFFFFFFFu;
		return hv;
	};

	if(primitive == TriangleList) {
		for(size_t i = 0; i + 2 < count; i += 3) {
			const uint32_t base = uint32_t(call.vertices.size());
			call.vertices.push_back(toRemixUI(vertices[i]));
			call.vertices.push_back(toRemixUI(vertices[i + 1]));
			call.vertices.push_back(toRemixUI(vertices[i + 2]));
			call.indices.push_back(base);
			call.indices.push_back(base + 1);
			call.indices.push_back(base + 2);
		}
	} else if(primitive == TriangleFan) {
		for(size_t i = 1; i + 1 < count; ++i) {
			const uint32_t base = uint32_t(call.vertices.size());
			call.vertices.push_back(toRemixUI(vertices[0]));
			call.vertices.push_back(toRemixUI(vertices[i]));
			call.vertices.push_back(toRemixUI(vertices[i + 1]));
			call.indices.push_back(base);
			call.indices.push_back(base + 1);
			call.indices.push_back(base + 2);
		}
	} else if(primitive == TriangleStrip) {
		for(size_t i = 0; i + 2 < count; ++i) {
			const uint32_t base = uint32_t(call.vertices.size());
			if(i & 1) {
				call.vertices.push_back(toRemixUI(vertices[i + 1]));
				call.vertices.push_back(toRemixUI(vertices[i]));
				call.vertices.push_back(toRemixUI(vertices[i + 2]));
			} else {
				call.vertices.push_back(toRemixUI(vertices[i]));
				call.vertices.push_back(toRemixUI(vertices[i + 1]));
				call.vertices.push_back(toRemixUI(vertices[i + 2]));
			}
			call.indices.push_back(base);
			call.indices.push_back(base + 1);
			call.indices.push_back(base + 2);
		}
	}

	if(call.vertices.empty()) {
		return;
	}

	/*
	 * Diagnostics for the UI placement, reset every time the camera flips between
	 * gameplay and the menu so a menu frame is always captured.
	 */
	static bool s_wasMenu = false;
	static unsigned s_logged = 0;
	const bool isMenu = (camPos == Vec3f(0.f) && fwd.z > 0.99f);
	if(isMenu != s_wasMenu) {
		s_wasMenu = isMenu;
		s_logged = 0;
	}
	if(s_logged < 4) {
		s_logged++;
		Vec3f lo = Vec3f(std::numeric_limits<float>::max());
		Vec3f hi = Vec3f(-std::numeric_limits<float>::max());
		for(const remixapi_HardcodedVertex & hv : call.vertices) {
			lo = glm::min(lo, Vec3f(hv.position[0], hv.position[1], hv.position[2]));
			hi = glm::max(hi, Vec3f(hv.position[0], hv.position[1], hv.position[2]));
		}
		LogInfo << "Remix UI batch: " << (isMenu ? "menu" : "game")
		        << " screen=" << screenW << 'x' << screenH << " win=" << winW << 'x' << winH
		        << " aspect=" << aspect << " fovY=" << glm::degrees(fovY)
		        << " verts=" << call.vertices.size()
		        << " world=(" << lo.x << ',' << lo.y << ',' << lo.z
		        << ")-(" << hi.x << ',' << hi.y << ',' << hi.z << ')';
	}

	m_uiBatches.push_back(std::move(call));
}

void RemixRenderer::showFrame() {
	ensureStarted();
	
	// Camera, room and lights first: UI instances must be submitted against this
	// frame's camera, not the previous one.
	remix::setupScene();
	
	RemixApi & api = remixApi();
	
	/*
	 * Repainted textures, mostly font atlases that grew a glyph. Safe to free the
	 * material here because no UI mesh outlives its frame, so nothing still holds
	 * it on a surface.
	 */
	for(Texture * dirty : takeDirtyUiTextures()) {
		dropUiMaterial(dirty, api);
	}
	
	if(api.iface().CreateMesh && api.iface().DrawInstance) {
		for(const auto & batch : m_uiBatches) {
			if(batch.vertices.size() < 3 || batch.indices.empty()) {
				continue;
			}

			/*
			 * Built fresh every frame on purpose: caching the handles and
			 * replaying them made Remix composite the menu into a small strip at
			 * the edge of the window, even though the vertices were measured to
			 * match the frustum exactly. Creating per frame is cheap here, a menu
			 * being a handful of quads.
			 */
			remixapi_MeshInfoSurfaceTriangles surface {};
			surface.vertices_values = batch.vertices.data();
			surface.vertices_count = uint32_t(batch.vertices.size());
			surface.indices_values = batch.indices.data();
			surface.indices_count = uint32_t(batch.indices.size());
			surface.skinning_hasvalue = FALSE;
			surface.material = batch.texture ? materialForTexture(batch.texture, api, batch.opaque)
			                                 : nullptr;

			remixapi_MeshInfo meshInfo {};
			meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			// Content derived, so a still menu keeps the same hash frame to frame
			// and Remix can still match it temporally.
			meshInfo.hash = uiBatchHash(batch);
			meshInfo.surfaces_values = &surface;
			meshInfo.surfaces_count = 1;

			remixapi_MeshHandle mesh = nullptr;
			if(api.iface().CreateMesh(&meshInfo, &mesh) == REMIXAPI_ERROR_CODE_SUCCESS && mesh) {
				remixapi_InstanceInfo instance {};
				if(batch.particle) {
					fillParticleInstance(instance, mesh);
				} else {
					fillUIInstance(instance, mesh);
				}
				api.iface().DrawInstance(&instance);
				m_frameUiMeshes.push_back(mesh);
			}
		}
	}

	remix::presentFrame();

	if(api.iface().DestroyMesh) {
		for(remixapi_MeshHandle mesh : m_frameUiMeshes) {
			api.iface().DestroyMesh(mesh);
		}
	}
	m_frameUiMeshes.clear();
	
	m_uiBatches.clear();
	++m_frame;
}

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX
