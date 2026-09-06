/*
 * Arx Raymix — D3D9 raster renderer (one HWND, system d3d9.dll).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/d3d9/D3D9Renderer.h"

#if ARX_HAVE_D3D9

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <d3d9.h>

#include "graphics/Color.h"
#include "graphics/Math.h"
#include "graphics/Vertex.h"
#include "graphics/image/Image.h"
#include "io/log/Logger.h"
#include "platform/Platform.h"

#if ARX_HAVE_RTX_REMIX
#include "graphics/remix/RemixApi.h"
#include "graphics/remix/RemixScene.h"
#include "platform/WindowsUtils.h"
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {

D3DCOLOR toD3DColor(Color color) {
	return D3DCOLOR_ARGB(color.a, color.r, color.g, color.b);
}

D3DCOLOR toD3DColor(ColorRGBA rgba) {
	return Color::fromRGBA(rgba).toBGRA().t;
}

struct VertexFog {
	bool enable = false;
	float start = 0.f;
	float end = 1.f;
	Color color;
};

// D3D cannot compute vertex fog for XYZRHW, so the factor travels in the specular
// alpha and the rasterizer blends it in per pixel after texturing, which is where
// OpenGL applies GL_FOG too. Folding fog into the diffuse instead yields
// texture * fogColour and turns fogged surfaces black in dark levels.
D3DCOLOR fogFactorSpecular(float depth, const VertexFog & fog) {
	if(!fog.enable) {
		return 0xff000000u;
	}
	const float range = fog.end - fog.start;
	if(range <= 1e-5f) {
		return 0xff000000u;
	}
	const float factor = glm::clamp((fog.end - depth) / range, 0.f, 1.f);
	return D3DCOLOR_ARGB(u8(factor * 255.f + 0.5f), 0, 0, 0);
}

D3DPRESENT_PARAMETERS makePresentParams(HWND hwnd, int width, int height) {
	D3DPRESENT_PARAMETERS pp {};
	pp.BackBufferWidth = UINT(width);
	pp.BackBufferHeight = UINT(height);
	// UNKNOWN left Remix logging D3D9Format::Unknown and
	// "[D3D9WindowProc] Swapchain handle is invalid" — Alt+X never attached
	// (remix-dxvk.log 28 Ago). Windowed 32-bit backbuffer is X8R8G8B8.
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferCount = 1;
	pp.MultiSampleType = D3DMULTISAMPLE_NONE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = hwnd;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	return pp;
}

bool createHalDevice(IDirect3D9 * d3d, HWND hwnd, int width, int height, IDirect3DDevice9 ** outDevice) {
	
	D3DPRESENT_PARAMETERS pp = makePresentParams(hwnd, width, height);
	HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
	                               D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
	                               &pp, outDevice);
	if(FAILED(hr)) {
		pp = makePresentParams(hwnd, width, height);
		hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
		                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
		                       &pp, outDevice);
	}
	if(FAILED(hr) || !outDevice || !*outDevice) {
		LogError << "D3D9: CreateDevice failed (hr=" << long(hr) << ")";
		if(outDevice) {
			*outDevice = nullptr;
		}
		return false;
	}
	return true;
}

#if ARX_HAVE_RTX_REMIX
bool createHalDeviceEx(IDirect3D9Ex * d3d, HWND hwnd, int width, int height, IDirect3DDevice9Ex ** outDevice) {
	
	if(!d3d || !outDevice) {
		return false;
	}
	*outDevice = nullptr;
	D3DPRESENT_PARAMETERS pp = makePresentParams(hwnd, width, height);
	HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
	                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
	                                 &pp, nullptr, outDevice);
	if(FAILED(hr)) {
		pp = makePresentParams(hwnd, width, height);
		hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
		                         D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
		                         &pp, nullptr, outDevice);
	}
	if(FAILED(hr) || !*outDevice) {
		LogError << "D3D9: CreateDeviceEx failed (hr=" << long(hr) << ")";
		*outDevice = nullptr;
		return false;
	}
	return true;
}
#endif

D3DCOLOR lerpFogSpecular(D3DCOLOR a, D3DCOLOR b, float t) {
	const float fa = float(a >> 24);
	const float fb = float(b >> 24);
	return D3DCOLOR_ARGB(u8(fa + (fb - fa) * t + 0.5f), 0, 0, 0);
}

struct TLVertex {
	float x, y, z, rhw;
	D3DCOLOR color;
	D3DCOLOR specular;
	float u, v;
};

struct TLVertex3 {
	float x, y, z, rhw;
	D3DCOLOR color;
	D3DCOLOR specular;
	float u0, v0, u1, v1, u2, v2;
};

static_assert(sizeof(TLVertex) == 32, "XYZRHW+DIFFUSE+SPECULAR+TEX1 is 32 bytes");
static_assert(sizeof(TLVertex3) == 48, "XYZRHW+DIFFUSE+SPECULAR+TEX3 is 48 bytes");

struct WorldVertex {
	float x, y, z;
	// D3DFVF component order is fixed: position, normal, diffuse, texcoords.
	float nx, ny, nz;
	D3DCOLOR color;
	float u, v;
};

struct WorldVertex3 {
	float x, y, z;
	float nx, ny, nz;
	D3DCOLOR color;
	float u0, v0, u1, v1, u2, v2;
};

static_assert(sizeof(WorldVertex) == 36, "XYZ+NORMAL+DIFFUSE+TEX1 is 36 bytes");
static_assert(sizeof(WorldVertex3) == 52, "XYZ+NORMAL+DIFFUSE+TEX3 is 52 bytes");

const DWORD kTLFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
const DWORD kTL3FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3;
/*
 * Upper bound on map lights handed to the runtime in one frame, whatever the
 * device claims it can take. Each one is a real light in the traced scene, so
 * this trades cost against how much of the level's lighting survives.
 */
constexpr unsigned kMaxSceneLights = 32;

const DWORD kWorldFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
const DWORD kWorld3FVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX3;

DWORD floatBits(float f) {
	DWORD bits = 0;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

void projectToScreen(float x, float y, float z, float w, float & ox, float & oy, float & oz, float & orhw) {
	if(w == 0.f) {
		w = 1.f;
	}
	ox = x / w;
	oy = y / w;
	oz = z / w;
	orhw = 1.f / std::abs(w);
}

// Match Camera.cpp nearDist. clip.w == view.z with the D3D-style projection.
const float kNearW = 1.f;

struct HVert {
	float x, y, z, w;
	D3DCOLOR color;
	D3DCOLOR specular;
	float u, v;
};

template <typename Vertex>
bool isFiniteTL(const Vertex & v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.rhw);
}

D3DCOLOR lerpColor(D3DCOLOR a, D3DCOLOR b, float t) {
	auto channel = [t](D3DCOLOR ca, D3DCOLOR cb, int shift) -> u8 {
		const float fa = float((ca >> shift) & 0xff);
		const float fb = float((cb >> shift) & 0xff);
		return u8(fa + (fb - fa) * t + 0.5f);
	};
	return D3DCOLOR_ARGB(channel(a, b, 24), channel(a, b, 16), channel(a, b, 8), channel(a, b, 0));
}

HVert lerpH(const HVert & a, const HVert & b, float t) {
	HVert o;
	o.x = a.x + (b.x - a.x) * t;
	o.y = a.y + (b.y - a.y) * t;
	o.z = a.z + (b.z - a.z) * t;
	o.w = a.w + (b.w - a.w) * t;
	o.color = lerpColor(a.color, b.color, t);
	o.specular = lerpFogSpecular(a.specular, b.specular, t);
	o.u = a.u + (b.u - a.u) * t;
	o.v = a.v + (b.v - a.v) * t;
	return o;
}

// Triangle + 6 frustum planes <= 9 verts; keep headroom.
const int kMaxClipVerts = 16;
// XYZRHW is not clipped to the viewport when the guard band is huge (±1e8).
// Clip a little past the edges so legitimate overflow still rasterizes.
const float kTLClipPad = 64.f;

template <typename Vert, typename DistFn, typename LerpFn>
void clipAgainstPlane(const Vert * in, int nIn, Vert * out, int & nOut, DistFn dist, LerpFn lerpFn) {
	nOut = 0;
	if(nIn < 2) {
		return;
	}
	for(int i = 0; i < nIn; ++i) {
		const Vert & prev = in[(i + nIn - 1) % nIn];
		const Vert & curr = in[i];
		const float dp = dist(prev);
		const float dc = dist(curr);
		const bool prevIn = dp >= 0.f;
		const bool currIn = dc >= 0.f;
		if(currIn) {
			if(!prevIn) {
				const float denom = dc - dp;
				if(denom != 0.f && nOut < kMaxClipVerts) {
					out[nOut++] = lerpFn(prev, curr, (-dp) / denom);
				}
			}
			if(nOut < kMaxClipVerts) {
				out[nOut++] = curr;
			}
		} else if(prevIn) {
			const float denom = dc - dp;
			if(denom != 0.f && nOut < kMaxClipVerts) {
				out[nOut++] = lerpFn(prev, curr, (-dp) / denom);
			}
		}
	}
}

template <typename Vert, typename LerpFn>
int clipHomogeneousFrustum(const Vert & a, const Vert & b, const Vert & c, Vert * out, LerpFn lerpFn) {
	Vert buf0[kMaxClipVerts];
	Vert buf1[kMaxClipVerts];
	buf0[0] = a;
	buf0[1] = b;
	buf0[2] = c;
	int n = 3;
	Vert * src = buf0;
	Vert * dst = buf1;
	auto pass = [&](auto dist) -> bool {
		int nOut = 0;
		clipAgainstPlane(src, n, dst, nOut, dist, lerpFn);
		n = nOut;
		Vert * tmp = src;
		src = dst;
		dst = tmp;
		return n >= 3;
	};
	if(!pass([](const Vert & v) { return v.w - kNearW; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.w + v.x; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.w - v.x; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.w + v.y; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.w - v.y; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.z; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.w - v.z; })) {
		return 0;
	}
	for(int i = 0; i < n; ++i) {
		out[i] = src[i];
	}
	return n;
}

template <typename Vert, typename LerpFn>
int clipScreenFrustum(const Vert & a, const Vert & b, const Vert & c, Vert * out,
                      float xMin, float xMax, float yMin, float yMax, LerpFn lerpFn) {
	Vert buf0[kMaxClipVerts];
	Vert buf1[kMaxClipVerts];
	buf0[0] = a;
	buf0[1] = b;
	buf0[2] = c;
	int n = 3;
	Vert * src = buf0;
	Vert * dst = buf1;
	auto pass = [&](auto dist) -> bool {
		int nOut = 0;
		clipAgainstPlane(src, n, dst, nOut, dist, lerpFn);
		n = nOut;
		Vert * tmp = src;
		src = dst;
		dst = tmp;
		return n >= 3;
	};
	if(!pass([](const Vert & v) { return v.w - kNearW; })) {
		return 0;
	}
	if(!pass([&](const Vert & v) { return v.x - xMin * v.w; })) {
		return 0;
	}
	if(!pass([&](const Vert & v) { return xMax * v.w - v.x; })) {
		return 0;
	}
	if(!pass([&](const Vert & v) { return v.y - yMin * v.w; })) {
		return 0;
	}
	if(!pass([&](const Vert & v) { return yMax * v.w - v.y; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.z; })) {
		return 0;
	}
	if(!pass([](const Vert & v) { return v.w - v.z; })) {
		return 0;
	}
	for(int i = 0; i < n; ++i) {
		out[i] = src[i];
	}
	return n;
}

bool finiteHomogeneous(float x, float y, float z, float w) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
}

bool insideWorldFrustum(float x, float y, float z, float w) {
	return finiteHomogeneous(x, y, z, w)
	       && w >= kNearW
	       && std::abs(x) <= w
	       && std::abs(y) <= w
	       && z >= 0.f && z <= w;
}

void tlClipBounds(const Rect & vp, float & xMin, float & xMax, float & yMin, float & yMax) {
	xMin = -kTLClipPad;
	yMin = -kTLClipPad;
	xMax = float((std::max)(vp.width(), s32(1))) + kTLClipPad;
	yMax = float((std::max)(vp.height(), s32(1))) + kTLClipPad;
}

bool insideTLFrustum(float x, float y, float z, float w, float xMin, float xMax, float yMin, float yMax) {
	if(!finiteHomogeneous(x, y, z, w) || w < kNearW) {
		return false;
	}
	return x >= xMin * w && x <= xMax * w
	       && y >= yMin * w && y <= yMax * w
	       && z >= 0.f && z <= w;
}

template <typename Emit>
void forEachTriangleIndices(Renderer::Primitive primitive, size_t n, Emit && emit) {
	switch(primitive) {
		case Renderer::TriangleList:
			for(size_t i = 0; i + 2 < n; i += 3) {
				emit(i, i + 1, i + 2);
			}
			break;
		case Renderer::TriangleStrip:
			for(size_t i = 0; i + 2 < n; ++i) {
				if(i & 1) {
					emit(i + 1, i, i + 2);
				} else {
					emit(i, i + 1, i + 2);
				}
			}
			break;
		case Renderer::TriangleFan:
			for(size_t i = 1; i + 1 < n; ++i) {
				emit(size_t(0), i, i + 1);
			}
			break;
		default:
			break;
	}
}

bool resolveIndex(const unsigned short * indices, size_t nindices, size_t nverts, size_t i,
                  size_t & out) {
	if(indices) {
		if(i >= nindices) {
			return false;
		}
		const unsigned short idx = indices[i];
		if(idx >= nverts) {
			return false;
		}
		out = idx;
		return true;
	}
	if(i >= nverts) {
		return false;
	}
	out = i;
	return true;
}

void convertTL(const TexturedVertex * src, size_t count, const Rect & vp, const VertexFog & fog,
               std::vector<TLVertex> & dst) {
	dst.resize(count);
	for(size_t i = 0; i < count; ++i) {
		const TexturedVertex & v = src[i];
		TLVertex & o = dst[i];
		projectToScreen(v.p.x, v.p.y, v.p.z, v.w, o.x, o.y, o.z, o.rhw);
		// HUD bitmaps already store window pixels with w=1.
		// Entities/cinematics store viewport-local homogeneous coords (see Book.cpp
		// bbox2D += bookPos). D3D XYZRHW is window space, so add the viewport origin.
		if(std::abs(v.w - 1.f) > 1e-5f) {
			o.x += float(vp.left);
			o.y += float(vp.top);
			if(o.z < 0.f) {
				o.z = 0.f;
			} else if(o.z > 1.f) {
				o.z = 1.f;
			}
		}
		o.color = toD3DColor(v.color);
		o.specular = fogFactorSpecular(v.w, fog);
		o.u = v.uv.x;
		o.v = v.uv.y;
	}
}

struct WorldDrawStats {
	int draws = 0;
	size_t emitted = 0;
	size_t inView = 0;
	float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
	float clipW = 0.f, ndcX = 0.f, ndcY = 0.f, ndcZ = 0.f, sx = 0.f, sy = 0.f, sz = 0.f;
	size_t triTotal = 0;
	size_t triClipped = 0;
	size_t triDropped = 0;
	unsigned lumaMin = 255;
	unsigned lumaMax = 0;
	unsigned lumaInViewMin = 255;
	unsigned lumaInViewMax = 0;
	float wInViewMin = 1e9f;
	float wInViewMax = -1e9f;
	bool fogEnable = false;
	float fogStart = 0.f;
	float fogEnd = 0.f;
	unsigned fogColor = 0;
	bool haveSample = false;
};

// Non-HUD TL draws (cinematics, characters, particles) — union of the frame.
struct TLFrameStats {
	int draws = 0;
	size_t emitted = 0;
	float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
	unsigned rmin = 255;
	unsigned rmax = 0;
	unsigned amin = 255;
	unsigned amax = 0;
};

TLFrameStats g_tlFrameStats;

WorldDrawStats g_worldStats;

void projectClipToTL(const Vec4f & clip, const Rect & vp, float & x, float & y, float & z, float & rhw) {
	float w = clip.w;
	if(w == 0.f) {
		w = 1.f;
	}
	const float invW = 1.f / w;
	const float ndcX = clip.x * invW;
	const float ndcY = clip.y * invW;
	float ndcZ = clip.z * invW;
	const float vpX = float(vp.left);
	const float vpY = float(vp.top);
	const float vpW = float((std::max)(vp.width(), s32(1)));
	const float vpH = float((std::max)(vp.height(), s32(1)));
	x = vpX + (ndcX + 1.f) * vpW * 0.5f;
	y = vpY + (1.f - ndcY) * vpH * 0.5f;
	if(ndcZ < 0.f) {
		ndcZ = 0.f;
	} else if(ndcZ > 1.f) {
		ndcZ = 1.f;
	}
	z = ndcZ;
	rhw = (w > 0.f) ? invW : 1.f / std::abs(w);
}

void projectWorldH(const HVert & h, const Rect & vp, TLVertex & o) {
	projectClipToTL(Vec4f(h.x, h.y, h.z, h.w), vp, o.x, o.y, o.z, o.rhw);
	o.color = h.color;
	o.specular = h.specular;
	o.u = h.u;
	o.v = h.v;
}

void projectTLH(const HVert & h, const Rect & vp, bool addViewportOrigin, bool affineRhw, TLVertex & o) {
	projectToScreen(h.x, h.y, h.z, h.w, o.x, o.y, o.z, o.rhw);
	if(addViewportOrigin) {
		o.x += float(vp.left);
		o.y += float(vp.top);
	}
	if(o.z < 0.f) {
		o.z = 0.f;
	} else if(o.z > 1.f) {
		o.z = 1.f;
	}
	if(affineRhw) {
		o.rhw = 1.f;
	}
	o.color = h.color;
	o.specular = h.specular;
	o.u = h.u;
	o.v = h.v;
}

void emitProjectedFan(const HVert * poly, int n, const Rect & vp, bool worldSpace, bool addViewportOrigin,
                      bool affineRhw, std::vector<TLVertex> & out) {
	if(n < 3) {
		return;
	}
	TLVertex projected[kMaxClipVerts];
	const int count = (std::min)(n, kMaxClipVerts);
	for(int i = 0; i < count; ++i) {
		if(worldSpace) {
			projectWorldH(poly[i], vp, projected[i]);
		} else {
			projectTLH(poly[i], vp, addViewportOrigin, affineRhw, projected[i]);
		}
		if(!isFiniteTL(projected[i]) || projected[i].rhw <= 0.f) {
			return;
		}
	}
	for(int i = 1; i + 1 < count; ++i) {
		out.push_back(projected[0]);
		out.push_back(projected[i]);
		out.push_back(projected[i + 1]);
	}
}

bool isHudW(float w) {
	return std::abs(w - 1.f) <= 1e-5f;
}

//! w == 0 means TexturedVertex::p is already a world position, not a projected
//! one. See Renderer::wantsWorldSpaceEntities().
bool isWorldSpaceW(float w) {
	return std::abs(w) <= 1e-6f;
}

bool submitWorldVerts(IDirect3DDevice9 * device, Renderer::Primitive primitive,
                      const std::vector<WorldVertex> & verts,
                      const unsigned short * indices, size_t nindices, bool direct);

// hudSpace means the TL vertices are already window pixels with w == 1 (menus, HUD,
// fades). Everything else is a projected 3D position and must be frustum-clipped:
// near-only clip leaves ndc x/y unbounded, and D3D's ±1e8 guard band rasterizes
// those as a smear over the whole screen.
void emitClippedTriangle(const HVert & a, const HVert & b, const HVert & c, const Rect & vp,
                         bool worldSpace, bool hudSpace, std::vector<TLVertex> & out) {
	if(hudSpace) {
		HVert tri[3] = { a, b, c };
		emitProjectedFan(tri, 3, vp, false, false, true, out);
		return;
	}
	if(worldSpace) {
		g_worldStats.triTotal++;
	}
	HVert clipped[kMaxClipVerts];
	int n = 0;
	bool clippedAny = false;
	if(worldSpace) {
		if(insideWorldFrustum(a.x, a.y, a.z, a.w)
		   && insideWorldFrustum(b.x, b.y, b.z, b.w)
		   && insideWorldFrustum(c.x, c.y, c.z, c.w)) {
			clipped[0] = a;
			clipped[1] = b;
			clipped[2] = c;
			n = 3;
		} else {
			clippedAny = true;
			n = clipHomogeneousFrustum(a, b, c, clipped, lerpH);
		}
	} else {
		float xMin, xMax, yMin, yMax;
		tlClipBounds(vp, xMin, xMax, yMin, yMax);
		if(insideTLFrustum(a.x, a.y, a.z, a.w, xMin, xMax, yMin, yMax)
		   && insideTLFrustum(b.x, b.y, b.z, b.w, xMin, xMax, yMin, yMax)
		   && insideTLFrustum(c.x, c.y, c.z, c.w, xMin, xMax, yMin, yMax)) {
			clipped[0] = a;
			clipped[1] = b;
			clipped[2] = c;
			n = 3;
		} else {
			clippedAny = true;
			n = clipScreenFrustum(a, b, c, clipped, xMin, xMax, yMin, yMax, lerpH);
		}
	}
	if(n < 3) {
		if(worldSpace) {
			g_worldStats.triDropped++;
		}
		return;
	}
	if(worldSpace && clippedAny) {
		g_worldStats.triClipped++;
	}
	emitProjectedFan(clipped, n, vp, worldSpace, !worldSpace, false, out);
}

HVert fromWorld(const SMY_VERTEX & v, const glm::mat4x4 & view, const glm::mat4x4 & proj,
                const VertexFog & fog) {
	const Vec4f clip = proj * view * Vec4f(v.p, 1.f);
	HVert h;
	h.x = clip.x;
	h.y = clip.y;
	h.z = clip.z;
	h.w = clip.w;
	h.color = toD3DColor(v.color);
	h.specular = fogFactorSpecular(clip.w, fog);
	h.u = v.uv.x;
	h.v = v.uv.y;
	return h;
}

HVert fromTL(const TexturedVertex & v, const VertexFog & fog) {
	HVert h;
	h.x = v.p.x;
	h.y = v.p.y;
	h.z = v.p.z;
	h.w = v.w;
	h.color = toD3DColor(v.color);
	h.specular = fogFactorSpecular(std::abs(v.w), fog);
	h.u = v.uv.x;
	h.v = v.uv.y;
	return h;
}

void clipWorldTriangles(Renderer::Primitive primitive, const SMY_VERTEX * verts, size_t nverts,
                        const unsigned short * indices, size_t nindices, const glm::mat4x4 & view,
                        const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                        std::vector<TLVertex> & out) {
	out.clear();
	if(!verts || nverts == 0) {
		return;
	}
	const size_t n = indices ? nindices : nverts;
	out.reserve(n);
	forEachTriangleIndices(primitive, n, [&](size_t i0, size_t i1, size_t i2) {
		size_t a = 0, b = 0, c = 0;
		if(!resolveIndex(indices, nindices, nverts, i0, a)
		   || !resolveIndex(indices, nindices, nverts, i1, b)
		   || !resolveIndex(indices, nindices, nverts, i2, c)) {
			return;
		}
		emitClippedTriangle(fromWorld(verts[a], view, proj, fog),
		                    fromWorld(verts[b], view, proj, fog),
		                    fromWorld(verts[c], view, proj, fog),
		                    vp, true, false, out);
	});
}

void clipTLTriangles(Renderer::Primitive primitive, const TexturedVertex * verts, size_t nverts,
                     const unsigned short * indices, size_t nindices, const Rect & vp,
                     const VertexFog & fog, std::vector<TLVertex> & out) {
	out.clear();
	if(!verts || nverts == 0) {
		return;
	}
	const size_t n = indices ? nindices : nverts;
	out.reserve(n);
	forEachTriangleIndices(primitive, n, [&](size_t i0, size_t i1, size_t i2) {
		size_t ia = 0, ib = 0, ic = 0;
		if(!resolveIndex(indices, nindices, nverts, i0, ia)
		   || !resolveIndex(indices, nindices, nverts, i1, ib)
		   || !resolveIndex(indices, nindices, nverts, i2, ic)) {
			return;
		}
		const TexturedVertex & a = verts[ia];
		const TexturedVertex & b = verts[ib];
		const TexturedVertex & c = verts[ic];
		const bool hudSpace = isHudW(a.w) && isHudW(b.w) && isHudW(c.w);
		emitClippedTriangle(fromTL(a, fog), fromTL(b, fog), fromTL(c, fog),
		                    vp, false, hudSpace, out);
	});
}

void convertWorld(const SMY_VERTEX * src, size_t count, const glm::mat4x4 & view,
                  const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                  std::vector<TLVertex> & dst) {
	dst.resize(count);
	for(size_t i = 0; i < count; ++i) {
		const SMY_VERTEX & v = src[i];
		const Vec4f clip = proj * view * Vec4f(v.p, 1.f);
		TLVertex & o = dst[i];
		projectClipToTL(clip, vp, o.x, o.y, o.z, o.rhw);
		o.color = toD3DColor(v.color);
		o.specular = fogFactorSpecular(clip.w, fog);
		o.u = v.uv.x;
		o.v = v.uv.y;
		if(!g_worldStats.haveSample) {
			g_worldStats.haveSample = true;
			g_worldStats.clipW = clip.w;
			g_worldStats.ndcX = (clip.w != 0.f) ? clip.x / clip.w : 0.f;
			g_worldStats.ndcY = (clip.w != 0.f) ? clip.y / clip.w : 0.f;
			g_worldStats.ndcZ = (clip.w != 0.f) ? clip.z / clip.w : 0.f;
			g_worldStats.sx = o.x;
			g_worldStats.sy = o.y;
			g_worldStats.sz = o.z;
		}
	}
}

void convertWorld3(const SMY_VERTEX3 * src, size_t count, const glm::mat4x4 & view,
                   const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                   std::vector<TLVertex3> & dst) {
	dst.resize(count);
	for(size_t i = 0; i < count; ++i) {
		const SMY_VERTEX3 & v = src[i];
		const Vec4f clip = proj * view * Vec4f(v.p, 1.f);
		TLVertex3 & o = dst[i];
		projectClipToTL(clip, vp, o.x, o.y, o.z, o.rhw);
		o.color = toD3DColor(v.color);
		o.specular = fogFactorSpecular(clip.w, fog);
		o.u0 = v.uv[0].x;
		o.v0 = v.uv[0].y;
		o.u1 = v.uv[1].x;
		o.v1 = v.uv[1].y;
		o.u2 = v.uv[2].x;
		o.v2 = v.uv[2].y;
	}
}

// Same frustum clip as the SMY_VERTEX path, for the multi-texture tile batches.
struct HVert3 {
	float x, y, z, w;
	D3DCOLOR color;
	D3DCOLOR specular;
	float u[3], v[3];
};

HVert3 lerpH3(const HVert3 & a, const HVert3 & b, float t) {
	HVert3 o;
	o.x = a.x + (b.x - a.x) * t;
	o.y = a.y + (b.y - a.y) * t;
	o.z = a.z + (b.z - a.z) * t;
	o.w = a.w + (b.w - a.w) * t;
	o.color = lerpColor(a.color, b.color, t);
	o.specular = lerpFogSpecular(a.specular, b.specular, t);
	for(int i = 0; i < 3; ++i) {
		o.u[i] = a.u[i] + (b.u[i] - a.u[i]) * t;
		o.v[i] = a.v[i] + (b.v[i] - a.v[i]) * t;
	}
	return o;
}

void projectWorldH3(const HVert3 & h, const Rect & vp, TLVertex3 & o) {
	projectClipToTL(Vec4f(h.x, h.y, h.z, h.w), vp, o.x, o.y, o.z, o.rhw);
	o.color = h.color;
	o.specular = h.specular;
	o.u0 = h.u[0];
	o.v0 = h.v[0];
	o.u1 = h.u[1];
	o.v1 = h.v[1];
	o.u2 = h.u[2];
	o.v2 = h.v[2];
}

void emitProjectedFan3(const HVert3 * poly, int n, const Rect & vp, std::vector<TLVertex3> & out) {
	if(n < 3) {
		return;
	}
	TLVertex3 projected[kMaxClipVerts];
	const int count = (std::min)(n, kMaxClipVerts);
	for(int i = 0; i < count; ++i) {
		projectWorldH3(poly[i], vp, projected[i]);
		if(!isFiniteTL(projected[i]) || projected[i].rhw <= 0.f) {
			return;
		}
	}
	for(int i = 1; i + 1 < count; ++i) {
		out.push_back(projected[0]);
		out.push_back(projected[i]);
		out.push_back(projected[i + 1]);
	}
}

void emitClippedTriangle3(const HVert3 & a, const HVert3 & b, const HVert3 & c, const Rect & vp,
                          std::vector<TLVertex3> & out) {
	g_worldStats.triTotal++;
	HVert3 clipped[kMaxClipVerts];
	int n = 0;
	bool clippedAny = false;
	if(insideWorldFrustum(a.x, a.y, a.z, a.w)
	   && insideWorldFrustum(b.x, b.y, b.z, b.w)
	   && insideWorldFrustum(c.x, c.y, c.z, c.w)) {
		clipped[0] = a;
		clipped[1] = b;
		clipped[2] = c;
		n = 3;
	} else {
		clippedAny = true;
		n = clipHomogeneousFrustum(a, b, c, clipped, lerpH3);
	}
	if(n < 3) {
		g_worldStats.triDropped++;
		return;
	}
	if(clippedAny) {
		g_worldStats.triClipped++;
	}
	emitProjectedFan3(clipped, n, vp, out);
}

HVert3 fromWorld3(const SMY_VERTEX3 & src, const glm::mat4x4 & view, const glm::mat4x4 & proj,
                  const VertexFog & fog) {
	const Vec4f clip = proj * view * Vec4f(src.p, 1.f);
	HVert3 h;
	h.x = clip.x;
	h.y = clip.y;
	h.z = clip.z;
	h.w = clip.w;
	h.color = toD3DColor(src.color);
	h.specular = fogFactorSpecular(clip.w, fog);
	for(int i = 0; i < 3; ++i) {
		h.u[i] = src.uv[i].x;
		h.v[i] = src.uv[i].y;
	}
	return h;
}

void clipWorldTriangles3(Renderer::Primitive primitive, const SMY_VERTEX3 * verts, size_t nverts,
                         const unsigned short * indices, size_t nindices, const glm::mat4x4 & view,
                         const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                         std::vector<TLVertex3> & out) {
	out.clear();
	if(!verts || nverts == 0) {
		return;
	}
	const size_t n = indices ? nindices : nverts;
	out.reserve(n);
	forEachTriangleIndices(primitive, n, [&](size_t i0, size_t i1, size_t i2) {
		size_t a = 0, b = 0, c = 0;
		if(!resolveIndex(indices, nindices, nverts, i0, a)
		   || !resolveIndex(indices, nindices, nverts, i1, b)
		   || !resolveIndex(indices, nindices, nverts, i2, c)) {
			return;
		}
		emitClippedTriangle3(fromWorld3(verts[a], view, proj, fog),
		                     fromWorld3(verts[b], view, proj, fog),
		                     fromWorld3(verts[c], view, proj, fog),
		                     vp, out);
	});
}

template <typename Vertex>
void accumulateTLStats(const Vertex * verts, size_t count, const Rect & vp) {
	const float left = float(vp.left);
	const float top = float(vp.top);
	const float right = float(vp.right);
	const float bottom = float(vp.bottom);
	for(size_t i = 0; i < count; ++i) {
		const Vertex & v = verts[i];
		g_worldStats.emitted++;
		g_worldStats.xmin = (std::min)(g_worldStats.xmin, v.x);
		g_worldStats.xmax = (std::max)(g_worldStats.xmax, v.x);
		g_worldStats.ymin = (std::min)(g_worldStats.ymin, v.y);
		g_worldStats.ymax = (std::max)(g_worldStats.ymax, v.y);
		const unsigned luma = (std::max)((std::max)((v.color >> 16) & 0xff, (v.color >> 8) & 0xff),
		                                 v.color & 0xff);
		g_worldStats.lumaMin = (std::min)(g_worldStats.lumaMin, luma);
		g_worldStats.lumaMax = (std::max)(g_worldStats.lumaMax, luma);
		if(v.rhw > 0.f && v.x >= left && v.x < right && v.y >= top && v.y < bottom) {
			g_worldStats.inView++;
			g_worldStats.lumaInViewMin = (std::min)(g_worldStats.lumaInViewMin, luma);
			g_worldStats.lumaInViewMax = (std::max)(g_worldStats.lumaInViewMax, luma);
			const float w = 1.f / v.rhw;
			g_worldStats.wInViewMin = (std::min)(g_worldStats.wInViewMin, w);
			g_worldStats.wInViewMax = (std::max)(g_worldStats.wInViewMax, w);
		}
	}
}

void accumulateTLFrameStats(const TLVertex * verts, size_t count) {
	if(!verts || count == 0) {
		return;
	}
	g_tlFrameStats.draws++;
	for(size_t i = 0; i < count; ++i) {
		const TLVertex & v = verts[i];
		g_tlFrameStats.emitted++;
		g_tlFrameStats.xmin = (std::min)(g_tlFrameStats.xmin, v.x);
		g_tlFrameStats.xmax = (std::max)(g_tlFrameStats.xmax, v.x);
		g_tlFrameStats.ymin = (std::min)(g_tlFrameStats.ymin, v.y);
		g_tlFrameStats.ymax = (std::max)(g_tlFrameStats.ymax, v.y);
		const unsigned r = (v.color >> 16) & 0xff;
		const unsigned a = (v.color >> 24) & 0xff;
		g_tlFrameStats.rmin = (std::min)(g_tlFrameStats.rmin, r);
		g_tlFrameStats.rmax = (std::max)(g_tlFrameStats.rmax, r);
		g_tlFrameStats.amin = (std::min)(g_tlFrameStats.amin, a);
		g_tlFrameStats.amax = (std::max)(g_tlFrameStats.amax, a);
	}
}

bool isTrianglePrimitive(Renderer::Primitive primitive) {
	return primitive == Renderer::TriangleList
	       || primitive == Renderer::TriangleStrip
	       || primitive == Renderer::TriangleFan;
}

void logFirstIndexedDraw(const char * tag, size_t nverts, size_t nindices, size_t emitted,
                         bool hasSample, float x, float y, float z, float rhw, HRESULT hr) {
	static bool logged = false;
	if(logged) {
		return;
	}
	logged = true;
	if(hasSample) {
		LogInfo << "D3D9 first indexed draw (" << tag << "): nverts=" << nverts
		        << " nidx=" << nindices << " emitted=" << emitted
		        << " v0=(" << x << ", " << y << ", " << z << ", rhw=" << rhw << ") hr=" << long(hr);
	} else {
		LogInfo << "D3D9 first indexed draw (" << tag << "): nverts=" << nverts
		        << " nidx=" << nindices << " emitted=" << emitted
		        << " (no sample vertex) hr=" << long(hr);
	}
}

HRESULT drawDeindexed(IDirect3DDevice9 * device, DWORD fvf, UINT stride,
                      const void * vertices, size_t vertexCount) {
	if(!device || !vertices || vertexCount < 3) {
		return D3D_OK;
	}
	device->SetFVF(fvf);
	return device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(vertexCount / 3), vertices, stride);
}

bool remixWantsWorldSpace() {
#if ARX_HAVE_RTX_REMIX
	return remix::isRemixDllHooked();
#else
	return false;
#endif
}

void logWorldSpaceSubmit(size_t count, bool indexed) {
	static bool logged = false;
	if(logged) {
		return;
	}
	logged = true;
	LogInfo << "D3D9 world: untransformed XYZ for Remix PT (indexed=" << (indexed ? 1 : 0)
	        << " n=" << count << ", DLF baked diffuse)";
}

/*
 * There used to be an unprojectClipToWorld() here, rebuilding world positions
 * from the engine's screen-space TL vertices so they could be path traced.
 * It is gone on purpose. Two things it could never get right:
 *
 *  - precision: the reconstruction is unstable wherever w is small, so meshes
 *    visibly trembled frame to frame
 *  - billboards: a camera-facing quad built in screen space has no world
 *    orientation left in it to recover
 *
 * Geometry that needs to be in the path-traced scene is converted at the source
 * instead - see Renderer::wantsWorldSpaceEntities(). One trap worth keeping in
 * mind if anyone tries again: worldToClipSpace() writes m_worldToScreen * pos,
 * not clip, so the inverse needs ndcToScreen * proj * view, not just proj*view.
 * Inverting only proj*view put NPCs thousands of units off-camera.
 */

/*!
 * Give the world mesh smooth per-vertex normals.
 *
 * SMY_VERTEX has no normal - Arx never needed one, because the software
 * pipeline lit the vertices itself and handed the renderer a finished colour.
 * A path tracer without normals falls back to each triangle's geometric normal,
 * which is what made the walls read as flat facets.
 *
 * Face normals are area-weighted by construction (the cross product is not
 * normalised before accumulating), which is the usual cheap approximation and
 * behaves well on the long thin triangles Arx rooms are full of.
 *
 * Smoothing only reaches across one draw call, so a hard edge between batches
 * stays hard. That matches the geometry: batches are split by texture.
 */
template <typename V>
void computeSmoothNormals(V * verts, size_t nverts, const unsigned short * indices, size_t nindices) {
	
	if(!verts || nverts == 0) {
		return;
	}
	
	for(size_t i = 0; i < nverts; i++) {
		verts[i].nx = 0.f;
		verts[i].ny = 0.f;
		verts[i].nz = 0.f;
	}
	
	const size_t triangles = (indices ? nindices : nverts) / 3;
	for(size_t t = 0; t < triangles; t++) {
		size_t i0 = t * 3, i1 = t * 3 + 1, i2 = t * 3 + 2;
		if(indices) {
			i0 = indices[i0];
			i1 = indices[i1];
			i2 = indices[i2];
		}
		if(i0 >= nverts || i1 >= nverts || i2 >= nverts) {
			continue;
		}
		const V & a = verts[i0];
		const V & b = verts[i1];
		const V & c = verts[i2];
		const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
		const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
		const float nx = e1y * e2z - e1z * e2y;
		const float ny = e1z * e2x - e1x * e2z;
		const float nz = e1x * e2y - e1y * e2x;
		for(size_t idx : { i0, i1, i2 }) {
			verts[idx].nx += nx;
			verts[idx].ny += ny;
			verts[idx].nz += nz;
		}
	}
	
	for(size_t i = 0; i < nverts; i++) {
		V & v = verts[i];
		const float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
		if(len > 1e-12f) {
			v.nx /= len;
			v.ny /= len;
			v.nz /= len;
		} else {
			v.ny = 1.f;
		}
	}
}

WorldVertex toWorldVertex(const SMY_VERTEX & v) {
	WorldVertex o {};
	o.x = v.p.x;
	o.y = v.p.y;
	o.z = v.p.z;
	// Filled by computeSmoothNormals() once the whole batch is in.
	o.nx = 0.f;
	o.ny = 1.f;
	o.nz = 0.f;
	/*
	 * SMY_VERTEX.color is the DLF light bake. Which value belongs here depends
	 * on who is lighting the frame, and right now that is nobody:
	 *
	 *   path tracing on screen -> white, so Remix relights from scratch and the
	 *                             baked torch blob does not double up
	 *   rasterising            -> the bake, because D3DRS_LIGHTING is FALSE and
	 *                             there is no other light source at all
	 *
	 * White with the raster on screen means texture at full brightness and no
	 * shading anywhere - the "sun inside the dungeon" look. Until the Remix
	 * present is fixed we are looking at the raster, so send the bake and let
	 * the room look like Arx. Flip this back to 0xFFFFFFFF the moment the path
	 * tracer is what reaches the window.
	 */
	o.color = toD3DColor(v.color);
	o.u = v.uv.x;
	o.v = v.uv.y;
	return o;
}

WorldVertex3 toWorldVertex3(const SMY_VERTEX3 & v) {
	WorldVertex3 o {};
	o.x = v.p.x;
	o.y = v.p.y;
	o.z = v.p.z;
	// Filled by computeSmoothNormals() once the whole batch is in.
	o.nx = 0.f;
	o.ny = 1.f;
	o.nz = 0.f;
	// Same reasoning as toWorldVertex(): the bake is the only lighting there is
	// while the raster is on screen.
	o.color = toD3DColor(v.color);
	o.u0 = v.uv[0].x;
	o.v0 = v.uv[0].y;
	o.u1 = v.uv[1].x;
	o.v1 = v.uv[1].y;
	o.u2 = v.uv[2].x;
	o.v2 = v.uv[2].y;
	return o;
}

D3DPRIMITIVETYPE toD3DPrimitive(Renderer::Primitive primitive) {
	switch(primitive) {
		case Renderer::TriangleList:  return D3DPT_TRIANGLELIST;
		case Renderer::TriangleStrip: return D3DPT_TRIANGLESTRIP;
		case Renderer::TriangleFan:   return D3DPT_TRIANGLEFAN;
		case Renderer::LineList:      return D3DPT_LINELIST;
		case Renderer::LineStrip:     return D3DPT_LINESTRIP;
	}
	return D3DPT_TRIANGLELIST;
}

UINT primitiveCount(Renderer::Primitive primitive, size_t vertexOrIndexCount) {
	switch(primitive) {
		case Renderer::TriangleList:  return UINT(vertexOrIndexCount / 3);
		case Renderer::TriangleStrip:
		case Renderer::TriangleFan:   return (vertexOrIndexCount >= 2) ? UINT(vertexOrIndexCount - 2) : 0;
		case Renderer::LineList:      return UINT(vertexOrIndexCount / 2);
		case Renderer::LineStrip:     return (vertexOrIndexCount >= 1) ? UINT(vertexOrIndexCount - 1) : 0;
	}
	return 0;
}

D3DBLEND toD3DBlend(BlendingFactor factor) {
	static const D3DBLEND table[] = {
		D3DBLEND_ZERO,          // BlendZero
		D3DBLEND_ONE,           // BlendOne
		D3DBLEND_SRCCOLOR,      // BlendSrcColor
		D3DBLEND_SRCALPHA,      // BlendSrcAlpha
		D3DBLEND_INVSRCCOLOR,   // BlendInvSrcColor
		D3DBLEND_INVSRCALPHA,   // BlendInvSrcAlpha
		D3DBLEND_SRCALPHASAT,   // BlendSrcAlphaSaturate
		D3DBLEND_DESTCOLOR,     // BlendDstColor
		D3DBLEND_DESTALPHA,     // BlendDstAlpha
		D3DBLEND_INVDESTCOLOR,  // BlendInvDstColor
		D3DBLEND_INVDESTALPHA   // BlendInvDstAlpha
	};
	return table[factor];
}

D3DTEXTUREOP toD3DTextureOp(TextureStage::TextureOp op, unsigned stage) {
	switch(op) {
		case TextureStage::OpDisable:
			// Stage 0 cannot be D3DTOP_DISABLE; skip the texture and keep vertex color.
			return (stage == 0) ? D3DTOP_SELECTARG2 : D3DTOP_DISABLE;
		case TextureStage::OpSelectArg1: return D3DTOP_SELECTARG1;
		case TextureStage::OpModulate:   return D3DTOP_MODULATE;
		case TextureStage::OpModulate2X: return D3DTOP_MODULATE2X;
		case TextureStage::OpModulate4X: return D3DTOP_MODULATE4X;
	}
	return D3DTOP_MODULATE;
}

D3DCOLOR pixelToD3D(Image::Format format, const unsigned char * p) {
	u8 r = 255, g = 255, b = 255, a = 255;
	switch(format) {
		case Image::Format_L8:
			r = g = b = p[0];
			break;
		case Image::Format_A8:
			a = p[0];
			break;
		case Image::Format_L8A8:
			r = g = b = p[0];
			a = p[1];
			break;
		case Image::Format_R8G8B8:
			r = p[0]; g = p[1]; b = p[2];
			break;
		case Image::Format_B8G8R8:
			b = p[0]; g = p[1]; r = p[2];
			break;
		case Image::Format_R8G8B8A8:
			r = p[0]; g = p[1]; b = p[2]; a = p[3];
			break;
		case Image::Format_B8G8R8A8:
			b = p[0]; g = p[1]; r = p[2]; a = p[3];
			break;
		default:
			break;
	}
	return D3DCOLOR_ARGB(a, r, g, b);
}

void uploadImageToLockedRect(const Image & image, const D3DLOCKED_RECT & locked) {
	const size_t width = image.getWidth();
	const size_t height = image.getHeight();
	const Image::Format format = image.getFormat();
	const size_t channels = Image::getNumChannels(format);
	const unsigned char * src = image.getData();
	auto * dstBase = static_cast<unsigned char *>(locked.pBits);
	for(size_t y = 0; y < height; ++y) {
		DWORD * dst = reinterpret_cast<DWORD *>(dstBase + y * locked.Pitch);
		const unsigned char * row = src + y * width * channels;
		for(size_t x = 0; x < width; ++x) {
			dst[x] = pixelToD3D(format, row + x * channels);
		}
	}
}

class D3D9VertexBufferTL final : public VertexBuffer<TexturedVertex> {
public:
	D3D9VertexBufferTL(D3D9Renderer * renderer, size_t capacity)
		: VertexBuffer<TexturedVertex>(capacity)
		, m_renderer(renderer)
		, m_data(capacity)
	{ }
	
	void setData(const TexturedVertex * vertices, size_t count, size_t offset = 0, BufferFlags flags = 0) override {
		ARX_UNUSED(flags);
		if(offset + count <= m_data.size()) {
			std::copy(vertices, vertices + count, m_data.begin() + offset);
		}
	}
	
	TexturedVertex * lock(BufferFlags flags = 0, size_t offset = 0, size_t count = size_t(-1)) override {
		ARX_UNUSED(flags);
		ARX_UNUSED(count);
		if(offset < m_data.size()) {
			return m_data.data() + offset;
		}
		return m_data.data();
	}
	
	void unlock() override { }
	
	void draw(Renderer::Primitive primitive, size_t count, size_t offset = 0) const override {
		if(!m_renderer || count == 0 || offset + count > m_data.size()) {
			return;
		}
		m_renderer->drawTextured(primitive, m_data.data() + offset, count);
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
		if(!m_renderer || count == 0 || offset + count > m_data.size()) {
			return;
		}
		m_renderer->drawIndexed(primitive, m_data.data() + offset, count,
		                        const_cast<unsigned short *>(indices), nbindices);
	}
	
private:
	D3D9Renderer * m_renderer;
	std::vector<TexturedVertex> m_data;
};

template <class Vertex>
class D3D9VertexBufferWorld final : public VertexBuffer<Vertex> {
public:
	D3D9VertexBufferWorld(D3D9Renderer * renderer, size_t capacity)
		: VertexBuffer<Vertex>(capacity)
		, m_renderer(renderer)
		, m_data(capacity)
	{ }
	
	void setData(const Vertex * vertices, size_t count, size_t offset = 0, BufferFlags flags = 0) override {
		ARX_UNUSED(flags);
		if(vertices && offset + count <= m_data.size()) {
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
	
	void draw(Renderer::Primitive primitive, size_t count, size_t offset = 0) const override {
		if(!m_renderer || count == 0 || offset + count > m_data.size()) {
			return;
		}
		m_renderer->drawWorldVertices(primitive, m_data.data() + offset, count);
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
		if(!m_renderer || count == 0 || offset + count > m_data.size() || !indices || nbindices == 0) {
			return;
		}
		m_renderer->drawWorldIndexed(primitive, m_data.data() + offset, count, indices, nbindices);
	}
	
private:
	D3D9Renderer * m_renderer;
	std::vector<Vertex> m_data;
};

} // namespace

D3D9Texture::D3D9Texture(D3D9Renderer * renderer)
	: m_renderer(renderer)
{ }

D3D9Texture::~D3D9Texture() {
	destroy();
}

bool D3D9Texture::createGpuTexture() {
	
	if(!m_renderer || !m_renderer->device()) {
		return false;
	}
	
	if(m_texture) {
		m_texture->Release();
		m_texture = nullptr;
	}
	
	Vec2i size = m_storedSize;
	if(size.x <= 0 || size.y <= 0) {
		return false;
	}
	if(unsigned(size.x) > m_renderer->maxTextureSize() || unsigned(size.y) > m_renderer->maxTextureSize()) {
		LogError << "D3D9: texture " << size.x << "x" << size.y << " exceeds device max "
		         << m_renderer->maxTextureSize();
		return false;
	}
	
	HRESULT hr = m_renderer->device()->CreateTexture(UINT(size.x), UINT(size.y), 1, 0,
	                                                 D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_texture, nullptr);
	if(FAILED(hr) || !m_texture) {
		LogError << "D3D9: CreateTexture " << size.x << "x" << size.y << " failed (hr=" << long(hr) << ")";
		m_texture = nullptr;
		return false;
	}
	
	m_gpuSize = size;
	return true;
}

bool D3D9Texture::create() {
	
	if(m_size.x <= 0 || m_size.y <= 0) {
		return false;
	}
	
	Vec2i nextPowerOfTwo(s32(GetNextPowerOf2(unsigned(m_size.x))), s32(GetNextPowerOf2(unsigned(m_size.y))));
	if(m_renderer && m_renderer->requirePow2Textures()) {
		m_storedSize = nextPowerOfTwo;
	} else {
		m_storedSize = m_size;
	}
	m_npot = (m_storedSize != nextPowerOfTwo);
	
	return createGpuTexture();
}

void D3D9Texture::upload() {
	
	if(!m_image.isValid() || !m_renderer || !m_renderer->device()) {
		return;
	}
	
	if(!m_texture || m_gpuSize != m_storedSize) {
		if(!createGpuTexture()) {
			return;
		}
	}
	
	const Image * src = &m_image;
	Image padded;
	if(m_storedSize != Vec2i(s32(m_image.getWidth()), s32(m_image.getHeight()))) {
		padded.create(size_t(m_storedSize.x), size_t(m_storedSize.y), m_image.getFormat());
		padded.extendClampToEdgeBorder(m_image);
		src = &padded;
	}
	
	D3DLOCKED_RECT locked {};
	HRESULT hr = m_texture->LockRect(0, &locked, nullptr, 0);
	if(FAILED(hr)) {
		LogError << "D3D9: LockRect failed (hr=" << long(hr) << ")";
		return;
	}
	uploadImageToLockedRect(*src, locked);
	m_texture->UnlockRect(0);
}

void D3D9Texture::destroy() {
	
	if(m_renderer) {
		for(size_t i = 0; i < m_renderer->getTextureStageCount(); ++i) {
			if(m_renderer->GetTextureStage(i)->getTexture() == this) {
				m_renderer->ResetTexture(unsigned(i));
			}
		}
	}
	
	if(m_texture) {
		m_texture->Release();
		m_texture = nullptr;
	}
	m_gpuSize = Vec2i(0);
}

D3D9TextureStage::D3D9TextureStage(unsigned stage)
	: TextureStage(stage)
{
	if(stage == 0) {
		m_colorOp = OpModulate;
		m_alphaOp = OpSelectArg1;
	} else {
		m_colorOp = OpDisable;
		m_alphaOp = OpDisable;
	}
}

void D3D9TextureStage::apply(IDirect3DDevice9 * device) const {
	
	if(!device) {
		return;
	}
	
	auto * tex = static_cast<D3D9Texture *>(m_texture);
	IDirect3DTexture9 * handle = (tex) ? tex->handle() : nullptr;
	device->SetTexture(mStage, handle);
	
	if(!handle) {
		if(mStage == 0) {
			device->SetTextureStageState(mStage, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			device->SetTextureStageState(mStage, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			device->SetTextureStageState(mStage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			device->SetTextureStageState(mStage, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		} else {
			device->SetTextureStageState(mStage, D3DTSS_COLOROP, D3DTOP_DISABLE);
			device->SetTextureStageState(mStage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}
		return;
	}
	
	device->SetTextureStageState(mStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(mStage, D3DTSS_COLORARG2, D3DTA_CURRENT);
	device->SetTextureStageState(mStage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(mStage, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
	device->SetTextureStageState(mStage, D3DTSS_COLOROP, toD3DTextureOp(m_colorOp, mStage));
	device->SetTextureStageState(mStage, D3DTSS_ALPHAOP, toD3DTextureOp(m_alphaOp, mStage));
	device->SetTextureStageState(mStage, D3DTSS_TEXCOORDINDEX, mStage);
	
	TextureStage::WrapMode wrap = getWrapMode();
	if(tex->isNPOT()) {
		wrap = TextureStage::WrapClamp;
	}
	D3DTEXTUREADDRESS address = D3DTADDRESS_WRAP;
	if(wrap == TextureStage::WrapMirror) {
		address = D3DTADDRESS_MIRROR;
	} else if(wrap == TextureStage::WrapClamp) {
		address = D3DTADDRESS_CLAMP;
	}
	device->SetSamplerState(mStage, D3DSAMP_ADDRESSU, address);
	device->SetSamplerState(mStage, D3DSAMP_ADDRESSV, address);
	
	const D3DTEXTUREFILTERTYPE minFilter = (getMinFilter() == FilterLinear) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	const D3DTEXTUREFILTERTYPE magFilter = (getMagFilter() == FilterLinear) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	device->SetSamplerState(mStage, D3DSAMP_MINFILTER, minFilter);
	device->SetSamplerState(mStage, D3DSAMP_MAGFILTER, magFilter);
	device->SetSamplerState(mStage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	DWORD lodBits = 0;
	float bias = m_lodBias;
	std::memcpy(&lodBits, &bias, sizeof(lodBits));
	device->SetSamplerState(mStage, D3DSAMP_MIPMAPLODBIAS, lodBits);
}

D3D9Renderer::D3D9Renderer() {
	for(unsigned i = 0; i < 4; ++i) {
		m_TextureStages.push_back(std::make_unique<D3D9TextureStage>(i));
	}
}

D3D9Renderer::~D3D9Renderer() {
	releaseDevice();
}

void D3D9Renderer::releaseDevice() {
	if(m_inScene && m_device) {
		m_device->EndScene();
		m_inScene = false;
	}
	releaseRemixBuffers();
	if(m_device) {
		m_device->Release();
		m_device = nullptr;
	}
	if(m_d3d) {
		m_d3d->Release();
		m_d3d = nullptr;
	}
	m_hwnd = nullptr;
	m_appliedStateValid = false;
}

bool D3D9Renderer::createDevice(void * nativeHwnd, int width, int height) {
	
	HWND hwnd = static_cast<HWND>(nativeHwnd);
	if(!hwnd || width <= 0 || height <= 0) {
		LogError << "D3D9: invalid window for CreateDevice";
		return false;
	}
	
	releaseDevice();
	
	bool usedRemix = false;
#if ARX_HAVE_RTX_REMIX
	IDirect3D9Ex * remixD3dEx = nullptr;
	IDirect3DDevice9Ex * remixDeviceEx = nullptr;
	remix::setRemixDllHooked(false);
	const std::wstring requested = RemixApi::requestedRuntimeDll();
	if(remix::sceneRequested() && requested.empty()) {
		LogWarning << "--remix-scene is ignored: C API play is off. Pass --remix-dll so D3D9Renderer loads Remix.";
	}
	if(!requested.empty()) {
		RemixApi & api = remix::remixApi();
		const std::string pathUtf8 = platform::WideString::toUTF8(requested.c_str());
		LogInfo << "D3D9: loading Remix " << pathUtf8;
		if(api.load(requested.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS) {
			LogError << "D3D9: Remix load failed, falling back to system d3d9.dll";
		} else if(api.iface().dxvk_CreateD3D9) {
			// CreateLight / DrawLightInstance require a device from this IDirect3D9Ex.
			/*
			 * Direct3DCreate9 is deliberately preferred over dxvk_CreateD3D9.
			 *
			 * dxvk_CreateD3D9 was chosen originally because Direct3DCreate9 on
			 * this DLL yields a non-Ex object, QI for IDirect3DDevice9Ex fails,
			 * and the C API's CreateLight then returns
			 * REMIX_DEVICE_WAS_NOT_REGISTERED. That constraint is gone: the C API
			 * light path never reached the scene at all (Light Statistics read
			 * "Total Lights: 0" with 17 lights submitted per frame), so the
			 * lights now go in as D3DLIGHT9 and nothing needs the Ex object.
			 *
			 * What remained was a scene Remix presented but never traced - no
			 * lights, capture wrote no USD, toggling ray tracing changed no
			 * pixel - with real vertex buffers making no difference. The one
			 * assumption never tested was that dxvk_CreateD3D9 hands back a
			 * device whose draw calls feed the path tracer. Direct3DCreate9 is
			 * the entry point a d3d9.dll hijack uses, which is how every other
			 * Remix title is captured.
			 */
			typedef IDirect3D9 * (WINAPI * Direct3DCreate9Proc)(UINT);
			const Direct3DCreate9Proc createRemix =
				reinterpret_cast<Direct3DCreate9Proc>(GetProcAddress(api.module(), "Direct3DCreate9"));
			if(createRemix) {
				m_d3d = createRemix(D3D_SDK_VERSION);
				if(m_d3d) {
					usedRemix = true;
					LogInfo << "D3D9: Remix Direct3DCreate9 (capture path, no C API device)";
				}
			}
			if(!m_d3d) {
				LogWarning << "D3D9: Remix Direct3DCreate9 unavailable, trying dxvk_CreateD3D9";
				const remixapi_ErrorCode created = api.iface().dxvk_CreateD3D9(FALSE, &remixD3dEx);
				if(created != REMIXAPI_ERROR_CODE_SUCCESS || !remixD3dEx) {
					LogError << "D3D9: dxvk_CreateD3D9 failed (" << RemixApi::errorString(created) << ')';
				} else {
					m_d3d = remixD3dEx;
					usedRemix = true;
					LogInfo << "D3D9: dxvk_CreateD3D9 returned IDirect3D9Ex";
				}
			}
		}
		if(!m_d3d && api.isLoaded()) {
			typedef IDirect3D9 * (WINAPI * Direct3DCreate9Proc)(UINT);
			const Direct3DCreate9Proc create =
				reinterpret_cast<Direct3DCreate9Proc>(GetProcAddress(api.module(), "Direct3DCreate9"));
			if(!create) {
				LogError << "D3D9: Remix d3d9.dll has no Direct3DCreate9, falling back to system";
				api.unloadLibrary();
			} else {
				m_d3d = create(D3D_SDK_VERSION);
				if(!m_d3d) {
					LogError << "D3D9: Remix Direct3DCreate9 failed, falling back to system d3d9.dll";
					api.unloadLibrary();
				} else {
					usedRemix = true;
				}
			}
		}
	}
#endif
	if(!m_d3d) {
		m_d3d = Direct3DCreate9(D3D_SDK_VERSION);
		usedRemix = false;
	}
	if(!m_d3d) {
		LogError << "D3D9: Direct3DCreate9 failed";
		return false;
	}
	
	D3DCAPS9 caps {};
	if(SUCCEEDED(m_d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) {
		m_maxTextureSize = (std::min)(caps.MaxTextureWidth, caps.MaxTextureHeight);
		const bool pow2Required = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0;
		const bool conditionalNPOT = (caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0;
		m_requirePow2Textures = pow2Required && !conditionalNPOT;
		// If the device does not clip pre-transformed vertices we have to bound the
		// projected coordinates ourselves instead of relying on the guard band.
		m_clipsTLVertices = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_CLIPTLVERTS) != 0;
		/*
		 * How many map lights can reach the path tracer at once. Fixed-function
		 * D3D9 usually answers 8, which is why a room with 500 lights is lit by
		 * the nearest handful and looks nothing like the baked original. The
		 * runtime is dxvk-remix rather than a 2002 driver, so ask instead of
		 * assuming, and log the answer either way.
		 */
		if(caps.MaxActiveLights > 0) {
			m_maxLights = (std::min)(unsigned(caps.MaxActiveLights), kMaxSceneLights);
		}
		LogInfo << "D3D9 caps: maxActiveLights=" << caps.MaxActiveLights
		        << " using=" << m_maxLights
		        << " clipTLVerts=" << (m_clipsTLVertices ? 1 : 0)
		        << " guardBand=(" << caps.GuardBandLeft << ", " << caps.GuardBandTop
		        << ", " << caps.GuardBandRight << ", " << caps.GuardBandBottom << ")"
		        << " maxTexture=" << m_maxTextureSize
		        << " pow2=" << (m_requirePow2Textures ? 1 : 0);
	}
	
	bool createdDevice = false;
#if ARX_HAVE_RTX_REMIX
	if(remixD3dEx) {
		createdDevice = createHalDeviceEx(remixD3dEx, hwnd, width, height, &remixDeviceEx);
		if(createdDevice) {
			m_device = remixDeviceEx;
		} else {
			LogWarning << "D3D9: Remix CreateDeviceEx failed, trying CreateDevice on IDirect3D9Ex";
			createdDevice = createHalDevice(m_d3d, hwnd, width, height, &m_device);
		}
	}
#endif
	if(!createdDevice && !createHalDevice(m_d3d, hwnd, width, height, &m_device)) {
#if ARX_HAVE_RTX_REMIX
		if(usedRemix) {
			LogError << "D3D9: Remix CreateDevice failed, falling back to system d3d9.dll";
			releaseDevice();
			remix::remixApi().unloadLibrary();
			usedRemix = false;
			remixD3dEx = nullptr;
			remixDeviceEx = nullptr;
			m_d3d = Direct3DCreate9(D3D_SDK_VERSION);
			if(!m_d3d) {
				LogError << "D3D9: Direct3DCreate9 failed";
				return false;
			}
			if(!createHalDevice(m_d3d, hwnd, width, height, &m_device)) {
				releaseDevice();
				return false;
			}
		} else
#endif
		{
			releaseDevice();
			return false;
		}
	}
	
	m_hwnd = hwnd;
	m_width = width;
	m_height = height;
	applyDefaultStates();
#if ARX_HAVE_RTX_REMIX
	if(usedRemix) {
		remix::onDllHookReady();
		RemixApi & api = remix::remixApi();
		IDirect3DDevice9Ex * ex = remixDeviceEx;
		bool releaseEx = false;
		if(!ex && m_device) {
			if(SUCCEEDED(m_device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
			                                      reinterpret_cast<void **>(&ex))) && ex) {
				releaseEx = true;
			} else if(remixD3dEx) {
				// NVIDIA: Register needs the 9Ex from dxvk_CreateD3D9 even if QI
				// does not advertise it.
				ex = static_cast<IDirect3DDevice9Ex *>(m_device);
			}
		}
		if(ex && api.iface().dxvk_RegisterD3D9Device) {
			const remixapi_ErrorCode st = api.iface().dxvk_RegisterD3D9Device(ex);
			LogInfo << "D3D9: dxvk_RegisterD3D9Device " << RemixApi::errorString(st);
			remix::setCApiDeviceRegistered(st == REMIXAPI_ERROR_CODE_SUCCESS);
			if(st == REMIXAPI_ERROR_CODE_SUCCESS) {
				// onDllHookReady() already tried this, but that runs before the
				// device is registered and the runtime drops it on the floor.
				LogInfo << "D3D9: re-applying Remix config now that the device is registered";
				remix::applyPreviewConfig();
			}
		} else {
			remix::setCApiDeviceRegistered(false);
			LogInfo << "D3D9: no C API device (capture path). Lights go in as D3DLIGHT9.";
		}
		if(releaseEx && ex) {
			ex->Release();
		}
		LogInfo << "Using D3D9 renderer " << width << "x" << height
		        << " (remix " << platform::WideString::toUTF8(requested.c_str())
		        << ", 2D+3D, one HWND, hwFog=0)";
		return true;
	}
#endif
	LogInfo << "Using D3D9 renderer " << width << "x" << height
	        << " (system d3d9.dll, 2D+3D raster, one HWND, hwFog=0)";
	LogInfo << "Ray tracing unavailable (D3D9)";
	return true;
}

void D3D9Renderer::applyDefaultStates() {
	if(!m_device) {
		return;
	}
	/*
	 * Fixed-function lighting stays off for a plain raster build - Arx bakes its
	 * lighting into vertex colours. Under Remix it has to be on: SetLight() and
	 * LightEnable() were being called for 8 map lights every frame and the
	 * runtime's Light Statistics panel still read "Total Lights: 0", because a
	 * light that the game itself is not using is a light Remix has no reason to
	 * carry into the scene.
	 */
	m_device->SetRenderState(D3DRS_LIGHTING, remixWantsWorldSpace() ? TRUE : FALSE);
	m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	m_device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	m_device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	m_device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
	m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
	m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
	m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	m_device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
	m_device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	m_device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	m_device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, floatBits(0.f));
	m_device->SetRenderState(D3DRS_DEPTHBIAS, floatBits(0.f));
	m_device->SetFVF(kTLFVF);
	D3DMATRIX identity {};
	identity._11 = identity._22 = identity._33 = identity._44 = 1.f;
	m_device->SetTransform(D3DTS_WORLD, &identity);
	applyTransforms();
	applyFog();
	m_appliedStateValid = false;
}

void D3D9Renderer::initialize() {
	if(!m_device) {
		LogError << "D3D9: initialize() without a device";
		return;
	}
	applyDefaultStates();
	SetViewport(Rect(0, 0, m_width, m_height));
	m_initialized = true;
	onRendererInit();
}

void D3D9Renderer::beforeResize(bool wasOrIsFullscreen) {
	ARX_UNUSED(wasOrIsFullscreen);
	if(m_inScene && m_device) {
		m_device->EndScene();
		m_inScene = false;
	}
	if(m_device) {
		for(DWORD i = 0; i < 4; ++i) {
			m_device->SetTexture(i, nullptr);
		}
	}
}

void D3D9Renderer::resetSwapchain() {
	if(!m_device || !m_hwnd) {
		return;
	}
	if(m_inScene) {
		m_device->EndScene();
		m_inScene = false;
	}
	for(DWORD i = 0; i < 4; ++i) {
		m_device->SetTexture(i, nullptr);
	}
	RECT client {};
	GetClientRect(static_cast<HWND>(m_hwnd), &client);
	int width = int(client.right - client.left);
	int height = int(client.bottom - client.top);
	if(width < 1 || height < 1) {
		width = m_width;
		height = m_height;
	}
	if(width < 1 || height < 1) {
		return;
	}
	// CreateDevice already used this size. A same-size ResetEx is what Remix
	// logs as "[D3D9WindowProc] Swapchain handle is invalid" (28 Ago 02:02).
	if(width == m_width && height == m_height) {
		return;
	}
	// D3DPOOL_DEFAULT resources have to be gone before a Reset.
	releaseRemixBuffers();
	D3DPRESENT_PARAMETERS pp = makePresentParams(static_cast<HWND>(m_hwnd), width, height);
	HRESULT hr = E_FAIL;
#if ARX_HAVE_RTX_REMIX
	IDirect3DDevice9Ex * ex = nullptr;
	if(SUCCEEDED(m_device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
	                                      reinterpret_cast<void **>(&ex))) && ex) {
		hr = ex->ResetEx(&pp, nullptr);
		ex->Release();
	} else
#endif
	{
		hr = m_device->Reset(&pp);
	}
	if(FAILED(hr)) {
		LogWarning << "D3D9: Reset swapchain failed (hr=" << long(hr)
		           << ") size=" << width << "x" << height;
		return;
	}
	m_width = width;
	m_height = height;
	m_appliedStateValid = false;
	m_remixDummyThisScene = false;
	applyDefaultStates();
	SetViewport(Rect(0, 0, m_width, m_height));
#if ARX_HAVE_RTX_REMIX
	if(remix::isRemixDllHooked() && remix::remixApi().iface().dxvk_RegisterD3D9Device) {
		IDirect3DDevice9Ex * registered = nullptr;
		if(SUCCEEDED(m_device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
		                                      reinterpret_cast<void **>(&registered))) && registered) {
			const remixapi_ErrorCode st = remix::remixApi().iface().dxvk_RegisterD3D9Device(registered);
			if(st != REMIXAPI_ERROR_CODE_SUCCESS) {
				LogWarning << "D3D9: dxvk_RegisterD3D9Device after Reset "
				           << RemixApi::errorString(st);
			} else {
				// A Reset re-registers the device, so the settings need pushing again.
				remix::applyPreviewConfig();
			}
			registered->Release();
		}
	}
#endif
	LogInfo << "D3D9: Reset swapchain " << width << "x" << height
	        << " format=X8R8G8B8 hwnd=" << m_hwnd;
}

void D3D9Renderer::afterResize() {
	resetSwapchain();
	m_initialized = (m_device != nullptr);
}

void D3D9Renderer::applyTransforms() {
	if(!m_device) {
		return;
	}
	D3DMATRIX view {};
	D3DMATRIX proj {};
	std::memcpy(&view, glm::value_ptr(m_view), sizeof(view));
	std::memcpy(&proj, glm::value_ptr(m_proj), sizeof(proj));
	m_device->SetTransform(D3DTS_VIEW, &view);
	m_device->SetTransform(D3DTS_PROJECTION, &proj);
}

void D3D9Renderer::applyFog() {
	if(!m_device) {
		return;
	}
	m_device->SetRenderState(D3DRS_FOGCOLOR, toD3DColor(m_fogColor));
	m_device->SetRenderState(D3DRS_FOGSTART, floatBits(m_fogStart));
	m_device->SetRenderState(D3DRS_FOGEND, floatBits(m_fogEnd));
}

bool D3D9Renderer::beginWorldDraw() {
	if(!beginSceneIfNeeded()) {
		return false;
	}
	flushDeviceState();
	g_worldStats.fogEnable = m_state.getFog();
	g_worldStats.fogStart = m_fogStart;
	g_worldStats.fogEnd = m_fogEnd;
	g_worldStats.fogColor = toD3DColor(m_fogColor);
	return true;
}

void D3D9Renderer::SetViewMatrix(const glm::mat4x4 & matView) {
	m_view = matView;
	if(m_device) {
		D3DMATRIX view {};
		std::memcpy(&view, glm::value_ptr(m_view), sizeof(view));
		m_device->SetTransform(D3DTS_VIEW, &view);
	}
}

void D3D9Renderer::SetProjectionMatrix(const glm::mat4x4 & matProj) {
	m_proj = matProj;
	if(m_device) {
		D3DMATRIX proj {};
		std::memcpy(&proj, glm::value_ptr(m_proj), sizeof(proj));
		m_device->SetTransform(D3DTS_PROJECTION, &proj);
	}
}

void D3D9Renderer::ReleaseAllTextures() { }
void D3D9Renderer::RestoreAllTextures() { }
void D3D9Renderer::reloadColorKeyTextures() { }

Texture * D3D9Renderer::createTexture() {
	return new D3D9Texture(this);
}

void D3D9Renderer::SetViewport(const Rect & viewport) {
	m_viewport = viewport;
	if(!m_device) {
		return;
	}
	D3DVIEWPORT9 vp {};
	vp.X = DWORD((std::max)(viewport.left, s32(0)));
	vp.Y = DWORD((std::max)(viewport.top, s32(0)));
	vp.Width = DWORD((std::max)(viewport.width(), s32(0)));
	vp.Height = DWORD((std::max)(viewport.height(), s32(0)));
	vp.MinZ = 0.f;
	vp.MaxZ = 1.f;
	m_device->SetViewport(&vp);
}

void D3D9Renderer::SetScissor(const Rect & rect) {
	m_scissor = rect;
	if(!m_device) {
		return;
	}
	if(rect.isValid()) {
		RECT r;
		r.left = LONG(rect.left);
		r.top = LONG(rect.top);
		r.right = LONG(rect.right);
		r.bottom = LONG(rect.bottom);
		m_device->SetScissorRect(&r);
		m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	} else {
		m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	}
}

bool D3D9Renderer::beginSceneIfNeeded() {
	if(!m_device) {
		return false;
	}
	if(m_inScene) {
		return true;
	}
	HRESULT hr = m_device->BeginScene();
	if(FAILED(hr)) {
		return false;
	}
	m_inScene = true;
	m_remixDummyThisScene = false;
#if ARX_HAVE_RTX_REMIX
	if(remixWantsWorldSpace()) {
		remix::onDllHookBeginScene();
		// Lights before geometry: Remix reads the fixed-function light state as
		// it captures each draw call.
		applyRemixLights();
		submitRemixInjectionDummy();
	}
#endif
	return true;
}

void D3D9Renderer::submitRemixInjectionDummy() {
#if ARX_HAVE_RTX_REMIX
	if(!m_device || m_remixDummyThisScene || !remixWantsWorldSpace()) {
		return;
	}
	if(!remix::hasCApiDevice()) {
		// Existed to make the C API camera attach before the first HUD draw. On
		// the capture path it is real traced geometry: a white untextured
		// triangle parked in front of the player.
		return;
	}
	m_remixDummyThisScene = true;
	
	D3DMATRIX savedView {};
	D3DMATRIX savedProj {};
	m_device->GetTransform(D3DTS_VIEW, &savedView);
	m_device->GetTransform(D3DTS_PROJECTION, &savedProj);
	
	const bool haveProj = std::abs(m_proj[2][3]) > 0.5f;
	WorldVertex tri[3] {};
	if(haveProj) {
		Vec3f pos, forward, up, right;
		remix::getArxHookCameraBasis(pos, forward, up, right);
		const Vec3f origin = pos + forward * 2.f;
		const Vec3f a = origin;
		const Vec3f b = origin + right * 0.25f + up * 0.15f;
		const Vec3f c = origin - right * 0.25f + up * 0.15f;
		tri[0] = { a.x, a.y, a.z, 0.f, 0.f, -1.f, 0xFFFFFFFFu, 0.f, 0.f };
		tri[1] = { b.x, b.y, b.z, 0.f, 0.f, -1.f, 0xFFFFFFFFu, 1.f, 0.f };
		tri[2] = { c.x, c.y, c.z, 0.f, 0.f, -1.f, 0xFFFFFFFFu, 0.f, 1.f };
	} else {
		tri[0] = {  0.0f,  0.1f, 2.f, 0.f, 0.f, -1.f, 0xFFFFFFFFu, 0.f, 0.f };
		tri[1] = {  0.2f, -0.1f, 2.f, 0.f, 0.f, -1.f, 0xFFFFFFFFu, 1.f, 0.f };
		tri[2] = { -0.2f, -0.1f, 2.f, 0.f, 0.f, -1.f, 0xFFFFFFFFu, 0.f, 1.f };
		const float aspect = (m_height > 0) ? float(m_width) / float((std::max)(m_height, 1)) : (16.f / 9.f);
		const glm::mat4 view(1.f);
		const glm::mat4 proj = glm::perspectiveLH_ZO(glm::radians(60.f), aspect, 1.f, 100.f);
		D3DMATRIX d3dView {};
		D3DMATRIX d3dProj {};
		std::memcpy(&d3dView, glm::value_ptr(view), sizeof(d3dView));
		std::memcpy(&d3dProj, glm::value_ptr(proj), sizeof(d3dProj));
		m_device->SetTransform(D3DTS_VIEW, &d3dView);
		m_device->SetTransform(D3DTS_PROJECTION, &d3dProj);
	}
	
	D3DMATRIX identity {};
	identity._11 = identity._22 = identity._33 = identity._44 = 1.f;
	m_device->SetTransform(D3DTS_WORLD, &identity);
	for(DWORD i = 0; i < 4; ++i) {
		m_device->SetTexture(i, nullptr);
	}
	m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
	m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	m_device->SetFVF(kWorldFVF);
	const HRESULT hr = m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(WorldVertex));
	
	if(!haveProj) {
		m_device->SetTransform(D3DTS_VIEW, &savedView);
		m_device->SetTransform(D3DTS_PROJECTION, &savedProj);
	}
	m_appliedStateValid = false;
	
	static bool logged = false;
	if(!logged) {
		logged = true;
		LogInfo << "D3D9 world: Remix injection dummy before HUD (haveProj="
		        << (haveProj ? 1 : 0) << " hr=" << long(hr) << ")";
	}
#else
	ARX_UNUSED(this);
#endif
}

bool D3D9Renderer::drawUnprojectedTL(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
                                    const unsigned short * indices, size_t nindices) {
	
	if(!m_device || !vertices || nvertices == 0) {
		return false;
	}
	
	/*
	 * Fast, exact path: the engine already handed us world positions because
	 * wantsWorldSpaceEntities() said so. Nothing to invert, so nothing to
	 * jitter - this is what stopped NPCs and props from trembling.
	 */
	if(isWorldSpaceW(vertices[0].w)) {
		thread_local std::vector<WorldVertex> direct;
		direct.resize(nvertices);
		for(size_t i = 0; i < nvertices; ++i) {
			const TexturedVertex & v = vertices[i];
			WorldVertex & o = direct[i];
			o.x = v.p.x;
			o.y = v.p.y;
			o.z = v.p.z;
			// Arx's own smooth per-vertex normal, carried through on the
			// world-space path. Without it the path tracer shades each triangle
			// flat and low-poly models come out visibly faceted.
			const float len = std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y
			                            + v.normal.z * v.normal.z);
			if(len > 1e-6f) {
				o.nx = v.normal.x / len;
				o.ny = v.normal.y / len;
				o.nz = v.normal.z / len;
			} else {
				o.nx = 0.f;
				o.ny = 1.f;
				o.nz = 0.f;
			}
			o.color = toD3DColor(v.color);
			o.u = v.uv.x;
			o.v = v.uv.y;
		}
		return submitWorldVerts(m_device, primitive, direct, indices, nindices, true);
	}
	
	// Not world space: the caller is responsible for keeping it 2D.
	return false;
}


namespace {

bool submitWorldVerts(IDirect3DDevice9 * device, Renderer::Primitive primitive,
                      const std::vector<WorldVertex> & verts,
                      const unsigned short * indices, size_t nindices, bool direct) {
	
	const size_t nvertices = verts.size();
	const size_t count = (indices && nindices > 0) ? nindices : nvertices;
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0) {
		return false;
	}
	
	device->SetFVF(kWorldFVF);
	HRESULT hr = D3D_OK;
	if(indices && nindices > 0) {
		hr = device->DrawIndexedPrimitiveUP(toD3DPrimitive(primitive), 0, UINT(nvertices), prims,
		                                    indices, D3DFMT_INDEX16, verts.data(), sizeof(WorldVertex));
	} else {
		hr = device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, verts.data(), sizeof(WorldVertex));
	}
	
	static bool loggedDirect = false;
	static bool loggedUnprojected = false;
	bool & logged = direct ? loggedDirect : loggedUnprojected;
	if(!logged) {
		logged = true;
		LogInfo << "D3D9 world: " << (direct ? "engine world-space" : "unprojected")
		        << " TL 3d for Remix PT nverts=" << nvertices
		        << " nidx=" << nindices << " prims=" << prims
		        << " world0=(" << verts[0].x << "," << verts[0].y << "," << verts[0].z
		        << ") hr=" << long(hr);
	}
	if(isTrianglePrimitive(primitive)) {
		g_tlFrameStats.draws++;
		g_tlFrameStats.emitted += count;
	}
	return SUCCEEDED(hr);
}

} // namespace


bool D3D9Renderer::wantsWorldSpaceEntities() const {
	return remixWantsWorldSpace();
}

/*!
 * Feed the map lights to Remix the way every other D3D9 game does.
 *
 * The C API route (CreateLight + DrawLightInstance) looked right in our logs -
 * 17 lights submitted every frame - but Remix's own Light Statistics panel read
 * "Total Lights: 0". Those calls live in the C API's frame, delimited by
 * remixapi_Present, and we present through D3D9, so nothing ever landed.
 *
 * Positions stay in Arx units, matching the world geometry we submit as XYZ.
 * Range comes from the light's own fallend, so the falloff is the game's rather
 * than a magic constant: the old path had ended up multiplying radiance by
 * 10000 to compensate for lights that were not there at all.
 */
void D3D9Renderer::releaseRemixBuffers() {
	if(m_remixVB) {
		m_remixVB->Release();
		m_remixVB = nullptr;
	}
	if(m_remixIB) {
		m_remixIB->Release();
		m_remixIB = nullptr;
	}
	m_remixVBBytes = 0;
	m_remixIBBytes = 0;
}

bool D3D9Renderer::ensureRemixBuffers(unsigned int vertexBytes, unsigned int indexBytes) {
	
	if(!m_device || vertexBytes == 0 || indexBytes == 0) {
		return false;
	}
	
	// Grow with headroom so a room that creeps up in size does not reallocate
	// every frame.
	if(!m_remixVB || m_remixVBBytes < vertexBytes) {
		if(m_remixVB) {
			m_remixVB->Release();
			m_remixVB = nullptr;
		}
		const UINT bytes = std::max(vertexBytes * 2, UINT(64 * 1024));
		if(FAILED(m_device->CreateVertexBuffer(bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		                                       kWorldFVF, D3DPOOL_DEFAULT, &m_remixVB, nullptr))) {
			m_remixVB = nullptr;
			return false;
		}
		m_remixVBBytes = bytes;
	}
	
	if(!m_remixIB || m_remixIBBytes < indexBytes) {
		if(m_remixIB) {
			m_remixIB->Release();
			m_remixIB = nullptr;
		}
		const UINT bytes = std::max(indexBytes * 2, UINT(16 * 1024));
		if(FAILED(m_device->CreateIndexBuffer(bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		                                      D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_remixIB, nullptr))) {
			m_remixIB = nullptr;
			return false;
		}
		m_remixIBBytes = bytes;
	}
	
	return true;
}

void D3D9Renderer::applyRemixLights() {
	
#if ARX_HAVE_RTX_REMIX
	if(!m_device || !remix::isRemixDllHooked()) {
		return;
	}
	
	remix::SceneLight lights[kMaxSceneLights];
	const size_t limit = (std::min)(size_t(m_maxLights), size_t(kMaxSceneLights));
	const size_t count = remix::collectSceneLights(lights, limit);
	
	for(size_t i = 0; i < count; i++) {
		const remix::SceneLight & src = lights[i];
		const float scale = std::max(src.intensity, 0.f);
		
		D3DLIGHT9 light {};
		light.Type = D3DLIGHT_POINT;
		light.Diffuse.r = src.rgb.r * scale;
		light.Diffuse.g = src.rgb.g * scale;
		light.Diffuse.b = src.rgb.b * scale;
		light.Diffuse.a = 1.f;
		light.Specular = light.Diffuse;
		light.Position.x = src.pos.x;
		light.Position.y = src.pos.y;
		light.Position.z = src.pos.z;
		light.Range = std::max(src.fallend, 1.f);
		// Arx fades between fallstart and fallend; a quadratic term over that
		// span is the closest fixed-function equivalent.
		const float fade = std::max(src.fallend - src.fallstart, 1.f);
		light.Attenuation0 = 0.f;
		light.Attenuation1 = 0.f;
		light.Attenuation2 = 1.f / (fade * fade);
		
		m_device->SetLight(DWORD(i), &light);
		m_device->LightEnable(DWORD(i), TRUE);
	}
	
	for(size_t i = count; i < limit; i++) {
		m_device->LightEnable(DWORD(i), FALSE);
	}
#endif
}

void D3D9Renderer::Clear(BufferFlags bufferFlags, Color clearColor, float clearDepth,
                         size_t nrects, Rect * rect) {
	if(!m_device) {
		return;
	}
	DWORD flags = 0;
	if(bufferFlags & ColorBuffer) {
		flags |= D3DCLEAR_TARGET;
	}
	if(bufferFlags & DepthBuffer) {
		flags |= D3DCLEAR_ZBUFFER;
	}
	if(flags == 0) {
		return;
	}
	if(!beginSceneIfNeeded()) {
		return;
	}
	// Letterbox bars call Clear with 2 rects and depth 0. Ignoring those rects
	// zeroed the whole Z buffer, so the 3D scene failed the depth test and only
	// the HUD (ZFUNC ALWAYS) remained visible.
	if(nrects > 0 && rect) {
		D3DRECT d3dRects[8];
		const UINT count = UINT((std::min)(nrects, size_t(8)));
		for(UINT i = 0; i < count; ++i) {
			d3dRects[i].x1 = LONG(rect[i].left);
			d3dRects[i].y1 = LONG(rect[i].top);
			d3dRects[i].x2 = LONG(rect[i].right);
			d3dRects[i].y2 = LONG(rect[i].bottom);
		}
		m_device->Clear(count, d3dRects, flags, toD3DColor(clearColor), clearDepth, 0);
		return;
	}
	m_device->Clear(0, nullptr, flags, toD3DColor(clearColor), clearDepth, 0);
}

void D3D9Renderer::SetFogColor(Color color) {
	m_fogColor = color;
	applyFog();
}

void D3D9Renderer::SetFogParams(float fogStart, float fogEnd) {
	m_fogStart = fogStart;
	m_fogEnd = fogEnd;
	applyFog();
}

void D3D9Renderer::SetAntialiasing(bool enable) {
	m_antialiasing = enable;
}

void D3D9Renderer::SetFillMode(FillMode mode) {
	m_fillMode = mode;
	if(m_device) {
		m_device->SetRenderState(D3DRS_FILLMODE, (mode == FillWireframe) ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
	}
}

void D3D9Renderer::flushDeviceState() {
	
	if(!m_device) {
		return;
	}
	
	if(!m_appliedStateValid || m_appliedState != m_state) {
		
		m_device->SetRenderState(D3DRS_CULLMODE, m_state.getCull() ? D3DCULL_CW : D3DCULL_NONE);
		// XYZRHW + FOGENABLE with FOGVERTEXMODE/FOGTABLEMODE NONE paints every 3D
		// pixel as fog colour (black in the cell). Specular-alpha fog is unused
		// while SPECULARENABLE is off, so the hardware factor is 0. Keep D3D fog
		// off; the cell is inside fog start (~1920) anyway. OpenGL still fogs
		// correctly from clip.w after texturing.
		m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
		
		BlendingFactor blendSrc = m_state.getBlendSrc();
		BlendingFactor blendDst = m_state.getBlendDst();
		DWORD alphaRef = 0;
		bool alphaTest = false;
		if(m_state.getAlphaCutout()) {
			alphaTest = true;
			if(blendSrc == BlendOne && blendDst == BlendZero) {
				alphaRef = 128;
			} else if(blendSrc == BlendOne && blendDst == BlendOne) {
				blendSrc = BlendSrcAlpha;
			}
		}
		m_device->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTest ? TRUE : FALSE);
		if(alphaTest) {
			m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
			m_device->SetRenderState(D3DRS_ALPHAREF, alphaRef);
		}
		
		const bool blend = (blendSrc != BlendOne || blendDst != BlendZero);
		m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
		if(blend) {
			m_device->SetRenderState(D3DRS_SRCBLEND, toD3DBlend(blendSrc));
			m_device->SetRenderState(D3DRS_DESTBLEND, toD3DBlend(blendDst));
		}
		
		m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
		m_device->SetRenderState(D3DRS_ZFUNC, m_state.getDepthTest() ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
		m_device->SetRenderState(D3DRS_ZWRITEENABLE, m_state.getDepthWrite() ? TRUE : FALSE);
		m_device->SetRenderState(D3DRS_FILLMODE, (m_fillMode == FillWireframe) ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
		
		const float depthOffset = -float(m_state.getDepthOffset());
		m_device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, floatBits(depthOffset));
		m_device->SetRenderState(D3DRS_DEPTHBIAS, floatBits(depthOffset * 1e-5f));
		
		m_appliedState = m_state;
		m_appliedStateValid = true;
	}
	
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		static_cast<D3D9TextureStage *>(m_TextureStages[i].get())->apply(m_device);
	}
}

void D3D9Renderer::drawTextured(Primitive primitive, const TexturedVertex * vertices, size_t count) {
	
	if(!m_device || !vertices || count == 0) {
		return;
	}
	
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0) {
		return;
	}
	
	if(!beginSceneIfNeeded()) {
		return;
	}
	flushDeviceState();
	
	/*
	 * Only take the world path for geometry the engine converted for us. What is
	 * left in screen space - sprites, sparks, flames from EERIECreateSprite() -
	 * is a camera-facing quad built from a projected position and a pixel size.
	 * There is no world orientation in it to recover here, so it stays a 2D
	 * overlay rather than being reconstructed and made to shimmer.
	 *
	 * EERIECreateSprite() can build those quads in world space instead, where
	 * the world position is still in hand, under --remix-debug 131072. Then they
	 * arrive marked and take this path like anything else.
	 */
	if(remixWantsWorldSpace() && isWorldSpaceW(vertices[0].w)) {
		if(drawUnprojectedTL(primitive, vertices, count, nullptr, 0)) {
			return;
		}
	}
	
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	
	if(isTrianglePrimitive(primitive)) {
		thread_local std::vector<TLVertex> clipped;
		clipTLTriangles(primitive, vertices, count, nullptr, 0, m_viewport, fog, clipped);
		if(clipped.size() < 3) {
			return;
		}
		{
			static bool loggedHud = false;
			static bool logged3d = false;
			const bool is3d = !isHudW(vertices[0].w);
			if(is3d) {
				accumulateTLFrameStats(clipped.data(), clipped.size());
			}
			if((is3d && !logged3d) || (!is3d && !loggedHud)) {
				if(is3d) {
					logged3d = true;
				} else {
					loggedHud = true;
				}
				LogInfo << "D3D9 TL " << (is3d ? "3d" : "hud") << ": w=" << vertices[0].w
				        << " fog=" << fog.enable << " depth=" << m_state.getDepthTest()
				        << " in=" << count << " emitted=" << clipped.size()
				        << " xy=(" << clipped[0].x << ", " << clipped[0].y << ")";
			}
		}
		m_device->SetFVF(kTLFVF);
		m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(clipped.size() / 3),
		                          clipped.data(), sizeof(TLVertex));
		return;
	}
	
	thread_local std::vector<TLVertex> converted;
	convertTL(vertices, count, m_viewport, fog, converted);
	m_device->SetFVF(kTLFVF);
	m_device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, converted.data(), sizeof(TLVertex));
}

std::unique_ptr<VertexBuffer<TexturedVertex>> D3D9Renderer::createVertexBufferTL(size_t capacity, BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<D3D9VertexBufferTL>(this, capacity);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX>> D3D9Renderer::createVertexBuffer(size_t capacity, BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<D3D9VertexBufferWorld<SMY_VERTEX>>(this, capacity);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX3>> D3D9Renderer::createVertexBuffer3(size_t capacity, BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<D3D9VertexBufferWorld<SMY_VERTEX3>>(this, capacity);
}

void D3D9Renderer::drawWorldVertices(Primitive primitive, const SMY_VERTEX * vertices, size_t count) {
	
	if(!m_device || !vertices || count == 0) {
		return;
	}
	
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0 || !beginWorldDraw()) {
		return;
	}
	
	if(remixWantsWorldSpace()) {
		thread_local std::vector<WorldVertex> worldVerts;
		worldVerts.resize(count);
		for(size_t i = 0; i < count; ++i) {
			worldVerts[i] = toWorldVertex(vertices[i]);
		}
		if(isTrianglePrimitive(primitive)) {
			computeSmoothNormals(worldVerts.data(), count, nullptr, 0);
		}
		logWorldSpaceSubmit(count, false);
		g_worldStats.draws++;
		g_worldStats.emitted += count;
		m_device->SetFVF(kWorldFVF);
		m_device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, worldVerts.data(), sizeof(WorldVertex));
		return;
	}
	
	thread_local std::vector<TLVertex> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles(primitive, vertices, count, nullptr, 0, m_view, m_proj, m_viewport, fog,
		                   converted);
		if(converted.size() < 3) {
			return;
		}
		g_worldStats.draws++;
		accumulateTLStats(converted.data(), converted.size(), m_viewport);
		m_device->SetFVF(kTLFVF);
		m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(converted.size() / 3),
		                          converted.data(), sizeof(TLVertex));
		return;
	}
	convertWorld(vertices, count, m_view, m_proj, m_viewport, fog, converted);
	g_worldStats.draws++;
	accumulateTLStats(converted.data(), converted.size(), m_viewport);
	m_device->SetFVF(kTLFVF);
	m_device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, converted.data(), sizeof(TLVertex));
}

void D3D9Renderer::drawWorldVertices(Primitive primitive, const SMY_VERTEX3 * vertices, size_t count) {
	
	if(!m_device || !vertices || count == 0) {
		return;
	}
	
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0 || !beginWorldDraw()) {
		return;
	}
	
	if(remixWantsWorldSpace()) {
		thread_local std::vector<WorldVertex3> worldVerts;
		worldVerts.resize(count);
		for(size_t i = 0; i < count; ++i) {
			worldVerts[i] = toWorldVertex3(vertices[i]);
		}
		if(isTrianglePrimitive(primitive)) {
			computeSmoothNormals(worldVerts.data(), count, nullptr, 0);
		}
		logWorldSpaceSubmit(count, false);
		g_worldStats.draws++;
		g_worldStats.emitted += count;
		m_device->SetFVF(kWorld3FVF);
		m_device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, worldVerts.data(), sizeof(WorldVertex3));
		return;
	}
	
	thread_local std::vector<TLVertex3> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles3(primitive, vertices, count, nullptr, 0, m_view, m_proj, m_viewport, fog,
		                    converted);
		if(converted.size() < 3) {
			return;
		}
		g_worldStats.draws++;
		accumulateTLStats(converted.data(), converted.size(), m_viewport);
		m_device->SetFVF(kTL3FVF);
		m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(converted.size() / 3),
		                          converted.data(), sizeof(TLVertex3));
		return;
	}
	convertWorld3(vertices, count, m_view, m_proj, m_viewport, fog, converted);
	g_worldStats.draws++;
	accumulateTLStats(converted.data(), converted.size(), m_viewport);
	m_device->SetFVF(kTL3FVF);
	m_device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, converted.data(), sizeof(TLVertex3));
}

void D3D9Renderer::drawWorldIndexed(Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                                    const unsigned short * indices, size_t nindices) {
	
	if(!m_device || !vertices || !indices || nvertices == 0 || nindices == 0) {
		return;
	}
	
	const UINT prims = primitiveCount(primitive, nindices);
	if(prims == 0 || !beginWorldDraw()) {
		return;
	}
	
	if(remixWantsWorldSpace()) {
		const UINT vertexBytes = UINT(nvertices * sizeof(WorldVertex));
		const UINT indexBytes = UINT(nindices * sizeof(unsigned short));
		/*
		 * Through real buffers, not DrawIndexedPrimitiveUP. Remix's geometry
		 * capture appears to only see draws with bound buffers: with UP draws
		 * its scene stayed empty - Total Lights 0, capture wrote no USD, and
		 * toggling ray tracing changed nothing - even though the bound textures
		 * were registered and the camera was accepted.
		 */
		if(ensureRemixBuffers(vertexBytes, indexBytes)) {
			void * dst = nullptr;
			bool filled = false;
			if(SUCCEEDED(m_remixVB->Lock(0, vertexBytes, &dst, D3DLOCK_DISCARD)) && dst) {
				WorldVertex * out = static_cast<WorldVertex *>(dst);
				for(size_t i = 0; i < nvertices; ++i) {
					out[i] = toWorldVertex(vertices[i]);
				}
				if(isTrianglePrimitive(primitive)) {
					computeSmoothNormals(out, nvertices, indices, nindices);
				}
				m_remixVB->Unlock();
				filled = true;
			}
			if(filled && SUCCEEDED(m_remixIB->Lock(0, indexBytes, &dst, D3DLOCK_DISCARD)) && dst) {
				std::memcpy(dst, indices, indexBytes);
				m_remixIB->Unlock();
				logWorldSpaceSubmit(nvertices, true);
				g_worldStats.draws++;
				g_worldStats.emitted += nindices;
				m_device->SetFVF(kWorldFVF);
				m_device->SetStreamSource(0, m_remixVB, 0, sizeof(WorldVertex));
				m_device->SetIndices(m_remixIB);
				m_device->DrawIndexedPrimitive(toD3DPrimitive(primitive), 0, 0, UINT(nvertices),
				                               0, prims);
				return;
			}
		}
		// Buffers unavailable: fall back to the user-pointer draw so the room
		// still renders, even if Remix will not trace it.
		thread_local std::vector<WorldVertex> worldVerts;
		worldVerts.resize(nvertices);
		for(size_t i = 0; i < nvertices; ++i) {
			worldVerts[i] = toWorldVertex(vertices[i]);
		}
		if(isTrianglePrimitive(primitive)) {
			computeSmoothNormals(worldVerts.data(), nvertices, indices, nindices);
		}
		logWorldSpaceSubmit(nvertices, true);
		g_worldStats.draws++;
		g_worldStats.emitted += nindices;
		m_device->SetFVF(kWorldFVF);
		m_device->DrawIndexedPrimitiveUP(toD3DPrimitive(primitive), 0, UINT(nvertices), prims,
		                                 indices, D3DFMT_INDEX16, worldVerts.data(), sizeof(WorldVertex));
		return;
	}
	
	thread_local std::vector<TLVertex> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles(primitive, vertices, nvertices, indices, nindices, m_view, m_proj,
		                   m_viewport, fog, converted);
		g_worldStats.draws++;
		accumulateTLStats(converted.data(), converted.size(), m_viewport);
		drawDeindexed(m_device, kTLFVF, sizeof(TLVertex), converted.data(), converted.size());
		return;
	}
	convertWorld(vertices, nvertices, m_view, m_proj, m_viewport, fog, converted);
	m_device->SetFVF(kTLFVF);
	m_device->DrawIndexedPrimitiveUP(toD3DPrimitive(primitive), 0, UINT(nvertices), prims,
	                                 indices, D3DFMT_INDEX16, converted.data(), sizeof(TLVertex));
}

void D3D9Renderer::drawWorldIndexed(Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
                                    const unsigned short * indices, size_t nindices) {
	
	if(!m_device || !vertices || !indices || nvertices == 0 || nindices == 0) {
		return;
	}
	
	const UINT prims = primitiveCount(primitive, nindices);
	if(prims == 0 || !beginWorldDraw()) {
		return;
	}
	
	if(remixWantsWorldSpace()) {
		thread_local std::vector<WorldVertex3> worldVerts;
		worldVerts.resize(nvertices);
		for(size_t i = 0; i < nvertices; ++i) {
			worldVerts[i] = toWorldVertex3(vertices[i]);
		}
		if(isTrianglePrimitive(primitive)) {
			computeSmoothNormals(worldVerts.data(), nvertices, indices, nindices);
		}
		logWorldSpaceSubmit(nvertices, true);
		g_worldStats.draws++;
		g_worldStats.emitted += nindices;
		m_device->SetFVF(kWorld3FVF);
		m_device->DrawIndexedPrimitiveUP(toD3DPrimitive(primitive), 0, UINT(nvertices), prims,
		                                 indices, D3DFMT_INDEX16, worldVerts.data(), sizeof(WorldVertex3));
		return;
	}
	
	thread_local std::vector<TLVertex3> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles3(primitive, vertices, nvertices, indices, nindices, m_view, m_proj,
		                    m_viewport, fog, converted);
		g_worldStats.draws++;
		accumulateTLStats(converted.data(), converted.size(), m_viewport);
		drawDeindexed(m_device, kTL3FVF, sizeof(TLVertex3), converted.data(), converted.size());
		return;
	}
	
	convertWorld3(vertices, nvertices, m_view, m_proj, m_viewport, fog, converted);
	g_worldStats.draws++;
	accumulateTLStats(converted.data(), converted.size(), m_viewport);
	m_device->SetFVF(kTL3FVF);
	m_device->DrawIndexedPrimitiveUP(toD3DPrimitive(primitive), 0, UINT(nvertices), prims,
	                                 indices, D3DFMT_INDEX16, converted.data(), sizeof(TLVertex3));
}

void D3D9Renderer::drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
                               unsigned short * indices, size_t nindices) {
	
	// Every one of these used to bail silently, which is why an entire cinematic could
	// go missing without a single line in the log.
	static bool loggedSkip = false;
	auto logSkip = [&](const char * reason) {
		if(!loggedSkip) {
			loggedSkip = true;
			LogWarning << "D3D9 drawIndexed skipped (" << reason << "): nverts=" << nvertices
			           << " nidx=" << nindices << " indices=" << (indices ? 1 : 0)
			           << " verts=" << (vertices ? 1 : 0) << " prim=" << int(primitive);
		}
	};
	
	if(!m_device || !vertices || !indices || nvertices == 0 || nindices == 0) {
		logSkip("null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, nindices);
	if(prims == 0) {
		logSkip("no primitives");
		return;
	}
	
	if(!beginSceneIfNeeded()) {
		logSkip("BeginScene failed");
		return;
	}
	flushDeviceState();
	
	/*
	 * Only take the world path for geometry the engine converted for us. What is
	 * left in screen space - sprites, sparks, flames from EERIECreateSprite() -
	 * is a camera-facing quad built from a projected position and a pixel size.
	 * There is no world orientation in it to recover here, so it stays a 2D
	 * overlay rather than being reconstructed and made to shimmer.
	 *
	 * EERIECreateSprite() can build those quads in world space instead, where
	 * the world position is still in hand, under --remix-debug 131072. Then they
	 * arrive marked and take this path like anything else.
	 */
	if(remixWantsWorldSpace() && isWorldSpaceW(vertices[0].w)) {
		if(drawUnprojectedTL(primitive, vertices, nvertices, indices, nindices)) {
			return;
		}
	}
	
	thread_local std::vector<TLVertex> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	
	HRESULT hr = D3D_OK;
	if(isTrianglePrimitive(primitive)) {
		clipTLTriangles(primitive, vertices, nvertices, indices, nindices, m_viewport, fog, converted);
		hr = drawDeindexed(m_device, kTLFVF, sizeof(TLVertex), converted.data(), converted.size());
		const bool hasSample = !converted.empty();
		logFirstIndexedDraw("tl", nvertices, nindices, converted.size(),
		                    hasSample, hasSample ? converted[0].x : 0.f,
		                    hasSample ? converted[0].y : 0.f,
		                    hasSample ? converted[0].z : 0.f,
		                    hasSample ? converted[0].rhw : 0.f, hr);
		const bool is3d = nvertices && !isHudW(vertices[0].w);
		if(is3d) {
			accumulateTLFrameStats(converted.data(), converted.size());
		}
		{
			static bool loggedCine = false;
			if(!loggedCine && is3d) {
				loggedCine = true;
				float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
				for(const TLVertex & v : converted) {
					xmin = (std::min)(xmin, v.x);
					xmax = (std::max)(xmax, v.x);
					ymin = (std::min)(ymin, v.y);
					ymax = (std::max)(ymax, v.y);
				}
				auto * stage = static_cast<D3D9TextureStage *>(GetTextureStage(0));
				auto * stageTex = static_cast<D3D9Texture *>(stage ? stage->getTexture() : nullptr);
				LogInfo << "D3D9 cinematic/2D indexed: nverts=" << nvertices
				        << " nidx=" << nindices << " emitted=" << converted.size()
				        << " v0w=" << vertices[0].w
				        << " v0p=(" << vertices[0].p.x << ", " << vertices[0].p.y << ")"
				        << " bbox=(" << xmin << ".." << xmax << ", " << ymin << ".." << ymax << ")"
				        << " vp=" << m_viewport.width() << "x" << m_viewport.height()
				        << " stageTex=" << (stageTex ? 1 : 0)
				        << " d3dTex=" << ((stageTex && stageTex->handle()) ? 1 : 0)
				        << " stored=" << (stageTex ? stageTex->getStoredSize().x : -1)
				        << "x" << (stageTex ? stageTex->getStoredSize().y : -1)
				        << " colorOp=" << int(stage ? stage->getColorOp() : -1)
				        << " c0=argb(" << (converted.empty() ? 0u : (converted[0].color >> 24) & 0xff)
				        << "," << (converted.empty() ? 0u : (converted[0].color >> 16) & 0xff)
				        << "," << (converted.empty() ? 0u : (converted[0].color >> 8) & 0xff)
				        << "," << (converted.empty() ? 0u : converted[0].color & 0xff) << ")"
				        << " uv0=(" << vertices[0].uv.x << ", " << vertices[0].uv.y << ")"
				        << " hr=" << long(hr);
			}
		}
	} else {
		convertTL(vertices, nvertices, m_viewport, fog, converted);
		m_device->SetFVF(kTLFVF);
		hr = m_device->DrawIndexedPrimitiveUP(toD3DPrimitive(primitive), 0, UINT(nvertices), prims,
		                                      indices, D3DFMT_INDEX16, converted.data(), sizeof(TLVertex));
		const bool hasSample = !converted.empty();
		logFirstIndexedDraw("tl-strip", nvertices, nindices, 0,
		                    hasSample, hasSample ? converted[0].x : 0.f,
		                    hasSample ? converted[0].y : 0.f,
		                    hasSample ? converted[0].z : 0.f,
		                    hasSample ? converted[0].rhw : 0.f, hr);
	}
}

bool D3D9Renderer::getSnapshot(Image & image) {
	ARX_UNUSED(image);
	return false;
}

bool D3D9Renderer::getSnapshot(Image & image, size_t width, size_t height) {
	ARX_UNUSED(image);
	ARX_UNUSED(width);
	ARX_UNUSED(height);
	return false;
}

void D3D9Renderer::showFrame() {
	if(!m_device) {
		return;
	}
	// Sampled per frame rather than once, so a black cutscene shows up as a frame
	// that drew nothing instead of hiding behind the very first frame's numbers.
	static int frameIndex = 0;
	static int worldSamples = 0;
	static int tlSamples = 0;
	static int cineLikeSamples = 0;
	frameIndex++;
	if(g_worldStats.draws > 0 && worldSamples < 24 && (frameIndex <= 2 || frameIndex % 120 == 0)) {
		worldSamples++;
		LogInfo << "D3D9 world frame f" << frameIndex << ": draws=" << g_worldStats.draws
		        << " emitted=" << g_worldStats.emitted
		        << " inView=" << g_worldStats.inView
		        << " tris=" << g_worldStats.triTotal
		        << " clipped=" << g_worldStats.triClipped
		        << " dropped=" << g_worldStats.triDropped
		        << " luma=" << g_worldStats.lumaMin << ".." << g_worldStats.lumaMax
		        << " lumaInView=" << g_worldStats.lumaInViewMin << ".." << g_worldStats.lumaInViewMax
		        << " wInView=" << g_worldStats.wInViewMin << ".." << g_worldStats.wInViewMax
		        << " fog=" << (g_worldStats.fogEnable ? 1 : 0)
		        << "[" << g_worldStats.fogStart << ".." << g_worldStats.fogEnd << "]"
		        << " fogCol=" << ((g_worldStats.fogColor >> 16) & 0xff)
		        << "," << ((g_worldStats.fogColor >> 8) & 0xff)
		        << "," << (g_worldStats.fogColor & 0xff)
		        << " bbox=(" << g_worldStats.xmin << ".." << g_worldStats.xmax
		        << ", " << g_worldStats.ymin << ".." << g_worldStats.ymax << ")"
		        << " v0 ndc=(" << g_worldStats.ndcX << ", " << g_worldStats.ndcY
		        << ", " << g_worldStats.ndcZ << ") w=" << g_worldStats.clipW
		        << " xy=(" << g_worldStats.sx << ", " << g_worldStats.sy << ")";
	}
	g_worldStats = WorldDrawStats();
	if(g_tlFrameStats.draws > 0) {
		const bool cineLike = g_tlFrameStats.draws >= 8;
		const bool periodic = tlSamples < 24 && (frameIndex <= 2 || frameIndex % 15 == 0);
		if((cineLike && cineLikeSamples < 12) || periodic) {
			if(cineLike) {
				cineLikeSamples++;
			} else {
				tlSamples++;
			}
			LogInfo << "D3D9 TL frame f" << frameIndex << ": draws=" << g_tlFrameStats.draws
			        << " emitted=" << g_tlFrameStats.emitted
			        << " bbox=(" << g_tlFrameStats.xmin << ".." << g_tlFrameStats.xmax
			        << ", " << g_tlFrameStats.ymin << ".." << g_tlFrameStats.ymax << ")"
			        << " r=" << g_tlFrameStats.rmin << ".." << g_tlFrameStats.rmax
			        << " a=" << g_tlFrameStats.amin << ".." << g_tlFrameStats.amax
			        << " vp=" << m_viewport.width() << "x" << m_viewport.height();
		}
	}
	g_tlFrameStats = TLFrameStats();
#if ARX_HAVE_RTX_REMIX
	remix::tickDllHookFrame();
#endif
	if(m_inScene) {
		m_device->EndScene();
		m_inScene = false;
	}
	HWND presentWindow = static_cast<HWND>(m_hwnd);
	m_device->Present(nullptr, nullptr, presentWindow, nullptr);
}

#endif // ARX_HAVE_D3D9
