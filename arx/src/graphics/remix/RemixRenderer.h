/*
 * Arx Remaster — Native RTX Remix Renderer (elimination of OpenGL backend)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXRENDERER_H
#define ARX_GRAPHICS_REMIX_REMIXRENDERER_H

#include "Configure.h"

#if ARX_HAVE_RTX_REMIX

#include <memory>
#include <vector>
#include <map>

#include "graphics/Renderer.h"
#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureStage.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Vertex.h"
#include "math/Rectangle.h"
#include <remix/remix_c.h>

namespace remix {

struct UI2DDrawCall {
	Texture * texture = nullptr;
	std::vector<remixapi_HardcodedVertex> vertices;
	std::vector<uint32_t> indices;
	bool particle = false;
	bool opaque = false;
};


class RemixTexture final : public Texture {
public:
	RemixTexture();
	~RemixTexture() override;

	void upload() override;
	void destroy() override;

protected:
	bool create() override;
};

class RemixTextureStage final : public TextureStage {
public:
	explicit RemixTextureStage(unsigned stage);
	~RemixTextureStage() override = default;

	Texture * getTexture() const override { return m_texture; }
	void setTexture(Texture * pTexture) override { m_texture = pTexture; }
	void resetTexture() override { m_texture = nullptr; }

	void setColorOp(TextureOp textureOp) override { m_colorOp = textureOp; }
	void setAlphaOp(TextureOp textureOp) override { m_alphaOp = textureOp; }
	void setMipMapLODBias(float bias) override { m_lodBias = bias; }

private:
	Texture * m_texture = nullptr;
	TextureOp m_colorOp = OpModulate;
	TextureOp m_alphaOp = OpSelectArg1;
	float m_lodBias = 0.f;
};

class RemixRenderer final : public Renderer {
public:
	RemixRenderer();
	~RemixRenderer() override;

	void initialize() override;
	void beforeResize(bool wasOrIsFullscreen) override;
	void afterResize() override;

	// Matrices
	void SetViewMatrix(const glm::mat4x4 & matView) override;
	void SetProjectionMatrix(const glm::mat4x4 & matProj) override;

	// Texture management
	void ReleaseAllTextures() override;
	void RestoreAllTextures() override;
	void reloadColorKeyTextures() override;

	// Factory
	[[nodiscard]] Texture * createTexture() override;

	// Viewport & Scissor
	void SetViewport(const Rect & viewport) override;
	void SetScissor(const Rect & rect) override;

	// Render Target
	void Clear(BufferFlags bufferFlags, Color clearColor = Color(), float clearDepth = 1.f,
	           size_t nrects = 0, Rect * rect = 0) override;

	// Fog
	void SetFogColor(Color color) override;
	void SetFogParams(float fogStart, float fogEnd) override;

	// Rasterizer
	void SetAntialiasing(bool enable) override;
	void SetFillMode(FillMode mode) override;

	[[nodiscard]] float getMaxSupportedAnisotropy() const override { return 16.f; }
	void setMaxAnisotropy(float value) override { m_anisotropy = value; }

	[[nodiscard]] AlphaCutoutAntialising getMaxSupportedAlphaCutoutAntialiasing() const override {
		return CrispAlphaCutoutAA;
	}

	[[nodiscard]] std::unique_ptr<VertexBuffer<TexturedVertex>> createVertexBufferTL(size_t capacity, BufferUsage usage) override;
	[[nodiscard]] std::unique_ptr<VertexBuffer<SMY_VERTEX>> createVertexBuffer(size_t capacity, BufferUsage usage) override;
	[[nodiscard]] std::unique_ptr<VertexBuffer<SMY_VERTEX3>> createVertexBuffer3(size_t capacity, BufferUsage usage) override;

	void drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
	                 unsigned short * indices, size_t nindices) override;

	bool getSnapshot(Image & image) override;
	bool getSnapshot(Image & image, size_t width, size_t height) override;

	// Present frame to Remix Vulkan swapchain
	void showFrame() override;

	// 2D Draw Queue for Menus, HUD, Text
	void queue2DVertices(Primitive primitive, const TexturedVertex * vertices, size_t count);

private:
	Rect m_viewport;
	Rect m_scissor;
	glm::mat4x4 m_view = glm::mat4x4(1.f);
	glm::mat4x4 m_proj = glm::mat4x4(1.f);
	Color m_fogColor;
	float m_fogStart = 0.f;
	float m_fogEnd = 1.f;
	float m_anisotropy = 1.f;
	bool m_antialiasing = true;
	FillMode m_fillMode = FillSolid;

	std::vector<UI2DDrawCall> m_uiBatches;
	//! Meshes built for the current frame, released right after Present.
	std::vector<remixapi_MeshHandle> m_frameUiMeshes;
	unsigned m_frame = 0;
};

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXRENDERER_H
