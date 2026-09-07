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

UINT presentInterval(int vsync) {
	return (vsync == 0) ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
}

D3DPRESENT_PARAMETERS makePresentParams(HWND hwnd, int width, int height, int vsync) {
	D3DPRESENT_PARAMETERS pp {};
	pp.BackBufferWidth = UINT(width);
	pp.BackBufferHeight = UINT(height);
	// Windowed 32-bit backbuffer. UNKNOWN made the swapchain unusable.
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferCount = 1;
	pp.MultiSampleType = D3DMULTISAMPLE_NONE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = hwnd;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	pp.PresentationInterval = presentInterval(vsync);
	return pp;
}

bool createHalDevice(IDirect3D9 * d3d, HWND hwnd, int width, int height, int vsync,
                     IDirect3DDevice9 ** outDevice) {
	
	D3DPRESENT_PARAMETERS pp = makePresentParams(hwnd, width, height, vsync);
	HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
	                               D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
	                               &pp, outDevice);
	if(FAILED(hr)) {
		pp = makePresentParams(hwnd, width, height, vsync);
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

const DWORD kTLFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;
const DWORD kTL3FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3;
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

template <int NUv>
struct HVert {
	float x, y, z, w;
	D3DCOLOR color;
	D3DCOLOR specular;
	float u[NUv];
	float v[NUv];
};

template <typename Src>
struct ClipTraits;

template <>
struct ClipTraits<SMY_VERTEX> {
	using H = HVert<1>;
	using Out = TLVertex;
};

template <>
struct ClipTraits<SMY_VERTEX3> {
	using H = HVert<3>;
	using Out = TLVertex3;
};

void assignUv(HVert<1> & h, const SMY_VERTEX & v) {
	h.u[0] = v.uv.x;
	h.v[0] = v.uv.y;
}

void assignUv(HVert<3> & h, const SMY_VERTEX3 & v) {
	for(int i = 0; i < 3; ++i) {
		h.u[i] = v.uv[i].x;
		h.v[i] = v.uv[i].y;
	}
}

void assignUv(HVert<1> & h, const TexturedVertex & v) {
	h.u[0] = v.uv.x;
	h.v[0] = v.uv.y;
}

void writeUv(const HVert<1> & h, TLVertex & o) {
	o.u = h.u[0];
	o.v = h.v[0];
}

void writeUv(const HVert<3> & h, TLVertex3 & o) {
	o.u0 = h.u[0];
	o.v0 = h.v[0];
	o.u1 = h.u[1];
	o.v1 = h.v[1];
	o.u2 = h.u[2];
	o.v2 = h.v[2];
}

void writeUv(const SMY_VERTEX & v, TLVertex & o) {
	o.u = v.uv.x;
	o.v = v.uv.y;
}

void writeUv(const SMY_VERTEX3 & v, TLVertex3 & o) {
	o.u0 = v.uv[0].x;
	o.v0 = v.uv[0].y;
	o.u1 = v.uv[1].x;
	o.v1 = v.uv[1].y;
	o.u2 = v.uv[2].x;
	o.v2 = v.uv[2].y;
}

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

template <int NUv>
HVert<NUv> lerpH(const HVert<NUv> & a, const HVert<NUv> & b, float t) {
	HVert<NUv> o;
	o.x = a.x + (b.x - a.x) * t;
	o.y = a.y + (b.y - a.y) * t;
	o.z = a.z + (b.z - a.z) * t;
	o.w = a.w + (b.w - a.w) * t;
	o.color = lerpColor(a.color, b.color, t);
	o.specular = lerpFogSpecular(a.specular, b.specular, t);
	for(int i = 0; i < NUv; ++i) {
		o.u[i] = a.u[i] + (b.u[i] - a.u[i]) * t;
		o.v[i] = a.v[i] + (b.v[i] - a.v[i]) * t;
	}
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

template <typename Vert, typename LerpFn, typename DistL, typename DistR, typename DistB, typename DistT>
int clipFrustum(const Vert & a, const Vert & b, const Vert & c, Vert * out, LerpFn lerpFn,
                DistL distL, DistR distR, DistB distB, DistT distT) {
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
	if(!pass(distL) || !pass(distR) || !pass(distB) || !pass(distT)) {
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
int clipHomogeneousFrustum(const Vert & a, const Vert & b, const Vert & c, Vert * out, LerpFn lerpFn) {
	return clipFrustum(a, b, c, out, lerpFn,
	                   [](const Vert & v) { return v.w + v.x; },
	                   [](const Vert & v) { return v.w - v.x; },
	                   [](const Vert & v) { return v.w + v.y; },
	                   [](const Vert & v) { return v.w - v.y; });
}

template <typename Vert, typename LerpFn>
int clipScreenFrustum(const Vert & a, const Vert & b, const Vert & c, Vert * out,
                      float xMin, float xMax, float yMin, float yMax, LerpFn lerpFn) {
	return clipFrustum(a, b, c, out, lerpFn,
	                   [&](const Vert & v) { return v.x - xMin * v.w; },
	                   [&](const Vert & v) { return xMax * v.w - v.x; },
	                   [&](const Vert & v) { return v.y - yMin * v.w; },
	                   [&](const Vert & v) { return yMax * v.w - v.y; });
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

bool g_collectStats = true;

void logDrawSkip(const char * entry, const char * reason) {
	static unsigned seen = 0;
	unsigned bit = 0;
	if(std::strcmp(reason, "null or empty") == 0) {
		bit = 1u;
	} else if(std::strcmp(reason, "no primitives") == 0) {
		bit = 2u;
	} else if(std::strcmp(reason, "BeginScene failed") == 0) {
		bit = 4u;
	} else {
		bit = 8u;
	}
	if(std::strcmp(entry, "drawIndexed") == 0) {
		bit <<= 4;
	} else if(std::strcmp(entry, "drawTextured") == 0) {
		bit <<= 8;
	} else if(std::strcmp(entry, "drawWorldVertices") == 0) {
		bit <<= 12;
	} else if(std::strcmp(entry, "drawWorldIndexed") == 0) {
		bit <<= 16;
	}
	if(seen & bit) {
		return;
	}
	seen |= bit;
	LogWarning << "D3D9 " << entry << " skipped (" << reason << ")";
}

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

template <typename Out, int NUv>
void projectWorldH(const HVert<NUv> & h, const Rect & vp, Out & o) {
	projectClipToTL(Vec4f(h.x, h.y, h.z, h.w), vp, o.x, o.y, o.z, o.rhw);
	o.color = h.color;
	o.specular = h.specular;
	writeUv(h, o);
}

template <typename Out, int NUv>
void projectTLH(const HVert<NUv> & h, const Rect & vp, bool addViewportOrigin, bool affineRhw, Out & o) {
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
	writeUv(h, o);
}

template <typename Out, int NUv>
void emitProjectedFan(const HVert<NUv> * poly, int n, const Rect & vp, bool worldSpace,
                      bool addViewportOrigin, bool affineRhw, std::vector<Out> & out) {
	if(n < 3) {
		return;
	}
	Out projected[kMaxClipVerts];
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

// hudSpace means the TL vertices are already window pixels with w == 1 (menus, HUD,
// fades). Everything else is a projected 3D position and must be frustum-clipped:
// near-only clip leaves ndc x/y unbounded, and D3D's ±1e8 guard band rasterizes
// those as a smear over the whole screen.
template <typename Out, int NUv>
void emitClippedTriangle(const HVert<NUv> & a, const HVert<NUv> & b, const HVert<NUv> & c, const Rect & vp,
                         bool worldSpace, bool hudSpace, std::vector<Out> & out) {
	if(hudSpace) {
		HVert<NUv> tri[3] = { a, b, c };
		emitProjectedFan(tri, 3, vp, false, false, true, out);
		return;
	}
	if(worldSpace && g_collectStats) {
		g_worldStats.triTotal++;
	}
	HVert<NUv> clipped[kMaxClipVerts];
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
			n = clipHomogeneousFrustum(a, b, c, clipped, lerpH<NUv>);
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
			n = clipScreenFrustum(a, b, c, clipped, xMin, xMax, yMin, yMax, lerpH<NUv>);
		}
	}
	if(n < 3) {
		if(worldSpace && g_collectStats) {
			g_worldStats.triDropped++;
		}
		return;
	}
	if(worldSpace && clippedAny && g_collectStats) {
		g_worldStats.triClipped++;
	}
	emitProjectedFan(clipped, n, vp, worldSpace, !worldSpace, false, out);
}

template <typename Src>
typename ClipTraits<Src>::H fromWorld(const Src & v, const glm::mat4x4 & view, const glm::mat4x4 & proj,
                                     const VertexFog & fog) {
	const Vec4f clip = proj * view * Vec4f(v.p, 1.f);
	typename ClipTraits<Src>::H h;
	h.x = clip.x;
	h.y = clip.y;
	h.z = clip.z;
	h.w = clip.w;
	h.color = toD3DColor(v.color);
	h.specular = fogFactorSpecular(clip.w, fog);
	assignUv(h, v);
	return h;
}

HVert<1> fromTL(const TexturedVertex & v, const VertexFog & fog) {
	HVert<1> h;
	h.x = v.p.x;
	h.y = v.p.y;
	h.z = v.p.z;
	h.w = v.w;
	h.color = toD3DColor(v.color);
	h.specular = fogFactorSpecular(std::abs(v.w), fog);
	assignUv(h, v);
	return h;
}

template <typename Src>
void clipWorldTriangles(Renderer::Primitive primitive, const Src * verts, size_t nverts,
                        const unsigned short * indices, size_t nindices, const glm::mat4x4 & view,
                        const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                        std::vector<typename ClipTraits<Src>::Out> & out) {
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

template <typename Src, typename Out>
void convertWorld(const Src * src, size_t count, const glm::mat4x4 & view,
                  const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                  std::vector<Out> & dst) {
	dst.resize(count);
	for(size_t i = 0; i < count; ++i) {
		const Src & v = src[i];
		const Vec4f clip = proj * view * Vec4f(v.p, 1.f);
		Out & o = dst[i];
		projectClipToTL(clip, vp, o.x, o.y, o.z, o.rhw);
		o.color = toD3DColor(v.color);
		o.specular = fogFactorSpecular(clip.w, fog);
		writeUv(v, o);
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

template <typename Vertex>
void accumulateTLStats(const Vertex * verts, size_t count, const Rect & vp) {
	if(!g_collectStats || !verts || count == 0) {
		return;
	}
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
	if(!g_collectStats || !verts || count == 0) {
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
{
	if(m_renderer) {
		m_renderer->registerTexture(this);
	}
}

D3D9Texture::~D3D9Texture() {
	destroy();
	if(m_renderer) {
		m_renderer->unregisterTexture(this);
		m_renderer = nullptr;
	}
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
	
	const bool wantMips = hasMipmaps() && m_storedSize == m_size;
	const UINT levels = wantMips ? 0u : 1u;
	HRESULT hr = m_renderer->device()->CreateTexture(UINT(size.x), UINT(size.y), levels, 0,
	                                                 D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_texture, nullptr);
	if(FAILED(hr) || !m_texture) {
		LogError << "D3D9: CreateTexture " << size.x << "x" << size.y << " failed (hr=" << long(hr) << ")";
		m_texture = nullptr;
		return false;
	}
	
	m_gpuSize = size;
	m_hasMips = wantMips && m_texture->GetLevelCount() > 1;
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
	
	Image levelImage;
	const Image * levelSrc = src;
	const DWORD levelCount = m_texture->GetLevelCount();
	for(DWORD level = 0; level < levelCount; ++level) {
		D3DSURFACE_DESC desc {};
		if(FAILED(m_texture->GetLevelDesc(level, &desc))) {
			break;
		}
		if(level > 0) {
			Image next;
			next.resizeFrom(*levelSrc, desc.Width, desc.Height);
			levelImage = std::move(next);
			levelSrc = &levelImage;
		}
		D3DLOCKED_RECT locked {};
		HRESULT hr = m_texture->LockRect(level, &locked, nullptr, 0);
		if(FAILED(hr)) {
			LogError << "D3D9: LockRect failed (hr=" << long(hr) << ")";
			return;
		}
		uploadImageToLockedRect(*levelSrc, locked);
		m_texture->UnlockRect(level);
	}
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
	m_hasMips = false;
}

D3D9TextureStage::D3D9TextureStage(D3D9Renderer * renderer, unsigned stage)
	: TextureStage(stage)
	, m_renderer(renderer)
{
	if(stage == 0) {
		m_colorOp = OpModulate;
		m_alphaOp = OpSelectArg1;
	} else {
		m_colorOp = OpDisable;
		m_alphaOp = OpDisable;
	}
}

void D3D9TextureStage::invalidateApplied() {
	m_applied = Applied();
}

void D3D9TextureStage::apply(IDirect3DDevice9 * device) {
	
	if(!device) {
		return;
	}
	
	auto * tex = static_cast<D3D9Texture *>(m_texture);
	IDirect3DTexture9 * handle = (tex) ? tex->handle() : nullptr;
	TextureStage::WrapMode wrap = getWrapMode();
	if(tex && tex->isNPOT()) {
		wrap = TextureStage::WrapClamp;
	}
	const float anisotropy = m_renderer ? m_renderer->anisotropy() : 1.f;
	
	if(m_applied.valid
	   && m_applied.handle == handle
	   && m_applied.colorOp == m_colorOp
	   && m_applied.alphaOp == m_alphaOp
	   && m_applied.wrap == wrap
	   && m_applied.minFilter == getMinFilter()
	   && m_applied.magFilter == getMagFilter()
	   && m_applied.lodBias == m_lodBias
	   && m_applied.anisotropy == anisotropy) {
		return;
	}
	
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
		m_applied = Applied();
		m_applied.valid = true;
		return;
	}
	
	device->SetTextureStageState(mStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(mStage, D3DTSS_COLORARG2, D3DTA_CURRENT);
	device->SetTextureStageState(mStage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(mStage, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
	device->SetTextureStageState(mStage, D3DTSS_COLOROP, toD3DTextureOp(m_colorOp, mStage));
	device->SetTextureStageState(mStage, D3DTSS_ALPHAOP, toD3DTextureOp(m_alphaOp, mStage));
	device->SetTextureStageState(mStage, D3DTSS_TEXCOORDINDEX, mStage);
	
	D3DTEXTUREADDRESS address = D3DTADDRESS_WRAP;
	if(wrap == TextureStage::WrapMirror) {
		address = D3DTADDRESS_MIRROR;
	} else if(wrap == TextureStage::WrapClamp) {
		address = D3DTADDRESS_CLAMP;
	}
	device->SetSamplerState(mStage, D3DSAMP_ADDRESSU, address);
	device->SetSamplerState(mStage, D3DSAMP_ADDRESSV, address);
	
	const bool useAniso = anisotropy > 1.f && getMinFilter() == FilterLinear;
	const D3DTEXTUREFILTERTYPE minFilter = useAniso ? D3DTEXF_ANISOTROPIC
	                                 : ((getMinFilter() == FilterLinear) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	const D3DTEXTUREFILTERTYPE magFilter = (getMagFilter() == FilterLinear) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	device->SetSamplerState(mStage, D3DSAMP_MINFILTER, minFilter);
	device->SetSamplerState(mStage, D3DSAMP_MAGFILTER, magFilter);
	if(useAniso) {
		device->SetSamplerState(mStage, D3DSAMP_MAXANISOTROPY, DWORD((std::max)(1.f, anisotropy)));
	}
	device->SetSamplerState(mStage, D3DSAMP_MIPFILTER, tex->hasGpuMips() ? D3DTEXF_LINEAR : D3DTEXF_NONE);
	DWORD lodBits = 0;
	float bias = m_lodBias;
	std::memcpy(&lodBits, &bias, sizeof(lodBits));
	device->SetSamplerState(mStage, D3DSAMP_MIPMAPLODBIAS, lodBits);
	
	m_applied.handle = handle;
	m_applied.colorOp = m_colorOp;
	m_applied.alphaOp = m_alphaOp;
	m_applied.wrap = wrap;
	m_applied.minFilter = getMinFilter();
	m_applied.magFilter = getMagFilter();
	m_applied.lodBias = m_lodBias;
	m_applied.anisotropy = anisotropy;
	m_applied.valid = true;
}

D3D9Renderer::D3D9Renderer() {
	for(unsigned i = 0; i < 4; ++i) {
		m_TextureStages.push_back(std::make_unique<D3D9TextureStage>(this, i));
	}
}

D3D9Renderer::~D3D9Renderer() {
	if(m_initialized) {
		onRendererShutdown();
		m_initialized = false;
	}
	for(D3D9Texture * texture : m_liveTextures) {
		texture->detachRenderer();
	}
	m_liveTextures.clear();
	releaseDevice();
}

void D3D9Renderer::releaseDevice() {
	if(m_inScene && m_device) {
		m_device->EndScene();
		m_inScene = false;
	}
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
	
	m_d3d = Direct3DCreate9(D3D_SDK_VERSION);
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
		const bool clipsTLVertices = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_CLIPTLVERTS) != 0;
		if(caps.MaxAnisotropy > 0) {
			m_maxAnisotropyCap = float(caps.MaxAnisotropy);
		}
		LogInfo << "D3D9 caps: maxActiveLights=" << caps.MaxActiveLights
		        << " clipTLVerts=" << (clipsTLVertices ? 1 : 0)
		        << " maxAniso=" << m_maxAnisotropyCap
		        << " guardBand=(" << caps.GuardBandLeft << ", " << caps.GuardBandTop
		        << ", " << caps.GuardBandRight << ", " << caps.GuardBandBottom << ")"
		        << " maxTexture=" << m_maxTextureSize
		        << " pow2=" << (m_requirePow2Textures ? 1 : 0);
	}
	
	if(!createHalDevice(m_d3d, hwnd, width, height, m_vsync, &m_device)) {
		releaseDevice();
		return false;
	}
	
	m_hwnd = hwnd;
	m_width = width;
	m_height = height;
	applyDefaultStates();
	LogInfo << "Using D3D9 renderer " << width << "x" << height
	        << " (system d3d9.dll, 2D+3D raster, one HWND, hwFog=0)";
	LogInfo << "Ray tracing unavailable (D3D9)";
	return true;
}

void D3D9Renderer::applyDefaultStates() {
	if(!m_device) {
		return;
	}
	// Arx bakes lighting into vertex colours; fixed-function lights stay off.
	m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	m_device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	m_device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	m_device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
	m_device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
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
	invalidateTextureStages();
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
		invalidateTextureStages();
	}
}

bool D3D9Renderer::resetDevice(int width, int height) {
	if(!m_device || !m_hwnd || width < 1 || height < 1) {
		return false;
	}
	if(m_inScene) {
		m_device->EndScene();
		m_inScene = false;
	}
	for(DWORD i = 0; i < 4; ++i) {
		m_device->SetTexture(i, nullptr);
	}
	invalidateTextureStages();
	D3DPRESENT_PARAMETERS pp = makePresentParams(static_cast<HWND>(m_hwnd), width, height, m_vsync);
	const HRESULT hr = m_device->Reset(&pp);
	if(FAILED(hr)) {
		LogWarning << "D3D9: Reset failed (hr=" << long(hr) << ") size=" << width << "x" << height;
		m_deviceLost = true;
		return false;
	}
	m_width = width;
	m_height = height;
	m_appliedStateValid = false;
	m_deviceLost = false;
	applyDefaultStates();
	SetViewport(Rect(0, 0, m_width, m_height));
	LogInfo << "D3D9: Reset device " << width << "x" << height
	        << " vsync=" << m_vsync << " hwnd=" << m_hwnd;
	return true;
}

bool D3D9Renderer::recoverDeviceIfNeeded() {
	if(!m_device) {
		return false;
	}
	const HRESULT coop = m_device->TestCooperativeLevel();
	if(coop == D3D_OK) {
		m_deviceLost = false;
		return true;
	}
	if(coop == D3DERR_DEVICELOST) {
		m_deviceLost = true;
		return false;
	}
	if(coop == D3DERR_DEVICENOTRESET) {
		return resetDevice(m_width, m_height);
	}
	return false;
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
	// CreateDevice already used this size; a same-size Reset is a no-op.
	if(width == m_width && height == m_height) {
		return;
	}
	(void)resetDevice(width, height);
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

void D3D9Renderer::registerTexture(D3D9Texture * texture) {
	if(texture) {
		m_liveTextures.push_back(texture);
	}
}

void D3D9Renderer::unregisterTexture(D3D9Texture * texture) {
	m_liveTextures.erase(std::remove(m_liveTextures.begin(), m_liveTextures.end(), texture),
	                     m_liveTextures.end());
}

void D3D9Renderer::invalidateTextureStages() {
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		static_cast<D3D9TextureStage *>(m_TextureStages[i].get())->invalidateApplied();
	}
}

void D3D9Renderer::setMaxAnisotropy(float value) {
	m_anisotropy = (std::min)((std::max)(value, 1.f), m_maxAnisotropyCap);
	invalidateTextureStages();
}

void D3D9Renderer::setPresentVSync(int vsync) {
	if(m_vsync == vsync) {
		return;
	}
	m_vsync = vsync;
	if(m_device && m_hwnd) {
		(void)resetDevice(m_width, m_height);
	}
}

void D3D9Renderer::ReleaseAllTextures() {
	for(D3D9Texture * texture : m_liveTextures) {
		texture->destroy();
	}
}

void D3D9Renderer::RestoreAllTextures() {
	for(D3D9Texture * texture : m_liveTextures) {
		texture->restore();
	}
}

void D3D9Renderer::reloadColorKeyTextures() {
	for(D3D9Texture * texture : m_liveTextures) {
		if(texture->hasColorKey()) {
			texture->restore();
		}
	}
}

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
	if(!m_device || !recoverDeviceIfNeeded()) {
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
	return true;
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
		// Specular alpha carries the per-vertex fog factor. FOGVERTEXMODE /
		// FOGTABLEMODE NONE tells D3D to consume that factor instead of a table.
		const bool fog = m_state.getFog();
		m_device->SetRenderState(D3DRS_FOGENABLE, fog ? TRUE : FALSE);
		m_device->SetRenderState(D3DRS_SPECULARENABLE, fog ? TRUE : FALSE);
		m_device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
		m_device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
		
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
		logDrawSkip("drawTextured", "null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0) {
		logDrawSkip("drawTextured", "no primitives");
		return;
	}
	
	if(!beginSceneIfNeeded()) {
		logDrawSkip("drawTextured", "BeginScene failed");
		return;
	}
	flushDeviceState();
	
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
		logDrawSkip("drawWorldVertices", "null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0) {
		logDrawSkip("drawWorldVertices", "no primitives");
		return;
	}
	if(!beginWorldDraw()) {
		logDrawSkip("drawWorldVertices", "BeginScene failed");
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
		logDrawSkip("drawWorldVertices", "null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, count);
	if(prims == 0) {
		logDrawSkip("drawWorldVertices", "no primitives");
		return;
	}
	if(!beginWorldDraw()) {
		logDrawSkip("drawWorldVertices", "BeginScene failed");
		return;
	}
	
	thread_local std::vector<TLVertex3> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles(primitive, vertices, count, nullptr, 0, m_view, m_proj, m_viewport, fog,
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
	convertWorld(vertices, count, m_view, m_proj, m_viewport, fog, converted);
	g_worldStats.draws++;
	accumulateTLStats(converted.data(), converted.size(), m_viewport);
	m_device->SetFVF(kTL3FVF);
	m_device->DrawPrimitiveUP(toD3DPrimitive(primitive), prims, converted.data(), sizeof(TLVertex3));
}

void D3D9Renderer::drawWorldIndexed(Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                                    const unsigned short * indices, size_t nindices) {
	
	if(!m_device || !vertices || !indices || nvertices == 0 || nindices == 0) {
		logDrawSkip("drawWorldIndexed", "null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, nindices);
	if(prims == 0) {
		logDrawSkip("drawWorldIndexed", "no primitives");
		return;
	}
	if(!beginWorldDraw()) {
		logDrawSkip("drawWorldIndexed", "BeginScene failed");
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
		logDrawSkip("drawWorldIndexed", "null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, nindices);
	if(prims == 0) {
		logDrawSkip("drawWorldIndexed", "no primitives");
		return;
	}
	if(!beginWorldDraw()) {
		logDrawSkip("drawWorldIndexed", "BeginScene failed");
		return;
	}
	
	thread_local std::vector<TLVertex3> converted;
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd, m_fogColor };
	
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles(primitive, vertices, nvertices, indices, nindices, m_view, m_proj,
		                   m_viewport, fog, converted);
		g_worldStats.draws++;
		accumulateTLStats(converted.data(), converted.size(), m_viewport);
		drawDeindexed(m_device, kTL3FVF, sizeof(TLVertex3), converted.data(), converted.size());
		return;
	}
	
	convertWorld(vertices, nvertices, m_view, m_proj, m_viewport, fog, converted);
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
	if(!m_device || !vertices || !indices || nvertices == 0 || nindices == 0) {
		logDrawSkip("drawIndexed", "null or empty");
		return;
	}
	
	const UINT prims = primitiveCount(primitive, nindices);
	if(prims == 0) {
		logDrawSkip("drawIndexed", "no primitives");
		return;
	}
	
	if(!beginSceneIfNeeded()) {
		logDrawSkip("drawIndexed", "BeginScene failed");
		return;
	}
	flushDeviceState();
	
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
	g_collectStats = (worldSamples < 24) || (tlSamples < 24) || (cineLikeSamples < 12);
	if(m_inScene) {
		m_device->EndScene();
		m_inScene = false;
	}
	HWND presentWindow = static_cast<HWND>(m_hwnd);
	const HRESULT presentHr = m_device->Present(nullptr, nullptr, presentWindow, nullptr);
	if(presentHr == D3DERR_DEVICELOST || presentHr == D3DERR_DEVICENOTRESET) {
		m_deviceLost = true;
		(void)recoverDeviceIfNeeded();
	}
}

#endif // ARX_HAVE_D3D9
