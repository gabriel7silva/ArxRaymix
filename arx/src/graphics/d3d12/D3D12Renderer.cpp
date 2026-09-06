/*
 * Arx Raymix — D3D12 raster (one HWND). Same CPU clip as D3D9, no RT.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/d3d12/D3D12Renderer.h"

#if ARX_HAVE_D3D12

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include "graphics/Color.h"
#include "graphics/Math.h"
#include "graphics/Vertex.h"
#include "graphics/image/Image.h"
#include "io/log/Logger.h"
#include "io/resource/ResourcePath.h"
#include "platform/Platform.h"

#include <glm/gtc/type_ptr.hpp>

template <class T>
class DxPtr {
	T * p = nullptr;
public:
	DxPtr() = default;
	~DxPtr() { reset(); }
	DxPtr(const DxPtr &) = delete;
	DxPtr & operator=(const DxPtr &) = delete;
	DxPtr(DxPtr && o) noexcept : p(o.p) { o.p = nullptr; }
	DxPtr & operator=(DxPtr && o) noexcept {
		if(this != &o) {
			reset();
			p = o.p;
			o.p = nullptr;
		}
		return *this;
	}
	void reset(T * next = nullptr) {
		if(p) {
			p->Release();
		}
		p = next;
	}
	T * Get() const { return p; }
	T ** put() { reset(); return &p; }
	T * operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

namespace {

constexpr UINT kFrameCount = 2;
constexpr UINT kSrvHeapSize = 4096;
constexpr UINT kUploadBytes = 16 * 1024 * 1024;
constexpr float kNearW = 1.f;
constexpr float kTLClipPad = 64.f;
constexpr int kMaxClipVerts = 16;

u32 toPacked(ColorRGBA rgba) {
	return Color::fromRGBA(rgba).toBGRA().t;
}

u32 fogFactorSpecular(float depth, bool enable, float start, float end) {
	if(!enable) {
		return 0xff000000u;
	}
	const float range = end - start;
	if(range <= 1e-5f) {
		return 0xff000000u;
	}
	const float factor = glm::clamp((end - depth) / range, 0.f, 1.f);
	return (u32(factor * 255.f + 0.5f) << 24);
}

struct GpuVert {
	float x, y, z, rhw;
	u32 color;
	u32 specular;
	float u0, v0, u1, v1, u2, v2;
};

static_assert(sizeof(GpuVert) == 48, "D3D12 projected vertex is 48 bytes");
static_assert(offsetof(GpuVert, rhw) == 12, "D3D12 projected rhw offset changed");
static_assert(offsetof(GpuVert, color) == 16, "D3D12 projected color offset changed");
static_assert(offsetof(GpuVert, specular) == 20, "D3D12 projected specular offset changed");
static_assert(offsetof(GpuVert, u0) == 24, "D3D12 projected UV offset changed");
static_assert(offsetof(GpuVert, u1) == 32, "D3D12 projected UV1 offset changed");
static_assert(offsetof(GpuVert, u2) == 40, "D3D12 projected UV2 offset changed");

struct VertexFog {
	bool enable = false;
	float start = 0.f;
	float end = 1.f;
};

struct HVert {
	float x, y, z, w;
	u32 color;
	u32 specular;
	float u, v;
};

u32 lerpColor(u32 a, u32 b, float t) {
	auto ch = [t](u32 ca, u32 cb, int shift) -> u8 {
		const float fa = float((ca >> shift) & 0xff);
		const float fb = float((cb >> shift) & 0xff);
		return u8(fa + (fb - fa) * t + 0.5f);
	};
	return (u32(ch(a, b, 24)) << 24) | (u32(ch(a, b, 16)) << 16) | (u32(ch(a, b, 8)) << 8) | u32(ch(a, b, 0));
}

HVert lerpH(const HVert & a, const HVert & b, float t) {
	HVert o;
	o.x = a.x + (b.x - a.x) * t;
	o.y = a.y + (b.y - a.y) * t;
	o.z = a.z + (b.z - a.z) * t;
	o.w = a.w + (b.w - a.w) * t;
	o.color = lerpColor(a.color, b.color, t);
	o.specular = lerpColor(a.specular, b.specular, t);
	o.u = a.u + (b.u - a.u) * t;
	o.v = a.v + (b.v - a.v) * t;
	return o;
}

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
		std::swap(src, dst);
		return n >= 3;
	};
	if(!pass([](const Vert & v) { return v.w - kNearW; })) return 0;
	if(!pass([](const Vert & v) { return v.w + v.x; })) return 0;
	if(!pass([](const Vert & v) { return v.w - v.x; })) return 0;
	if(!pass([](const Vert & v) { return v.w + v.y; })) return 0;
	if(!pass([](const Vert & v) { return v.w - v.y; })) return 0;
	if(!pass([](const Vert & v) { return v.z; })) return 0;
	if(!pass([](const Vert & v) { return v.w - v.z; })) return 0;
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
		std::swap(src, dst);
		return n >= 3;
	};
	if(!pass([](const Vert & v) { return v.w - kNearW; })) return 0;
	if(!pass([&](const Vert & v) { return v.x - xMin * v.w; })) return 0;
	if(!pass([&](const Vert & v) { return xMax * v.w - v.x; })) return 0;
	if(!pass([&](const Vert & v) { return v.y - yMin * v.w; })) return 0;
	if(!pass([&](const Vert & v) { return yMax * v.w - v.y; })) return 0;
	if(!pass([](const Vert & v) { return v.z; })) return 0;
	if(!pass([](const Vert & v) { return v.w - v.z; })) return 0;
	for(int i = 0; i < n; ++i) {
		out[i] = src[i];
	}
	return n;
}

bool finite4(float x, float y, float z, float w) {
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
}

bool insideWorldFrustum(float x, float y, float z, float w) {
	return finite4(x, y, z, w) && w >= kNearW && std::abs(x) <= w && std::abs(y) <= w
	       && z >= 0.f && z <= w;
}

bool insideTLFrustum(float x, float y, float z, float w, float xMin, float xMax, float yMin, float yMax) {
	if(!finite4(x, y, z, w) || w < kNearW) {
		return false;
	}
	return x >= xMin * w && x <= xMax * w && y >= yMin * w && y <= yMax * w && z >= 0.f && z <= w;
}

void projectToScreen(float x, float y, float z, float w, float & ox, float & oy, float & oz,
                     float & orhw) {
	if(w == 0.f) {
		w = 1.f;
	}
	ox = x / w;
	oy = y / w;
	oz = z / w;
	orhw = 1.f / std::abs(w);
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
	const float vpW = float((std::max)(vp.width(), s32(1)));
	const float vpH = float((std::max)(vp.height(), s32(1)));
	x = float(vp.left) + (ndcX + 1.f) * vpW * 0.5f;
	y = float(vp.top) + (1.f - ndcY) * vpH * 0.5f;
	z = glm::clamp(ndcZ, 0.f, 1.f);
	rhw = (w > 0.f) ? invW : 1.f / std::abs(w);
}

bool isHudW(float w) {
	return std::abs(w - 1.f) <= 1e-5f;
}

bool isTrianglePrimitive(Renderer::Primitive primitive) {
	return primitive == Renderer::TriangleList
	       || primitive == Renderer::TriangleStrip
	       || primitive == Renderer::TriangleFan;
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

bool resolveIndex(const unsigned short * indices, size_t nindices, size_t nverts, size_t i, size_t & out) {
	if(indices) {
		if(i >= nindices) {
			return false;
		}
		if(indices[i] >= nverts) {
			return false;
		}
		out = indices[i];
		return true;
	}
	if(i >= nverts) {
		return false;
	}
	out = i;
	return true;
}

GpuVert toGpu(float x, float y, float z, float rhw, u32 color, u32 specular, float u, float v) {
	return GpuVert{ x, y, z, rhw, color, specular, u, v, 0.f, 0.f, 0.f, 0.f };
}

void emitProjectedFan(const HVert * poly, int n, const Rect & vp, bool worldSpace, bool addOrigin,
                      bool affine, std::vector<GpuVert> & out) {
	if(n < 3) {
		return;
	}
	GpuVert projected[kMaxClipVerts];
	const int count = (std::min)(n, kMaxClipVerts);
	for(int i = 0; i < count; ++i) {
		float x, y, z, rhw;
		if(worldSpace) {
			projectClipToTL(Vec4f(poly[i].x, poly[i].y, poly[i].z, poly[i].w), vp, x, y, z, rhw);
		} else {
			projectToScreen(poly[i].x, poly[i].y, poly[i].z, poly[i].w, x, y, z, rhw);
			if(addOrigin) {
				x += float(vp.left);
				y += float(vp.top);
			}
			z = glm::clamp(z, 0.f, 1.f);
			if(affine) {
				rhw = 1.f;
			}
		}
		if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(rhw)
		   || rhw <= 0.f) {
			return;
		}
		projected[i] = toGpu(x, y, z, rhw, poly[i].color, poly[i].specular, poly[i].u, poly[i].v);
	}
	if(worldSpace) {
		static int rhwLogs = 3;
		if(rhwLogs > 0) {
			--rhwLogs;
			LogInfo << "D3D12 world rhw sample w=" << poly[0].w << " rhw=" << projected[0].rhw
			        << " xy=(" << projected[0].x << ", " << projected[0].y << ")";
		}
	} else if(affine) {
		static int hudLogs = 3;
		if(hudLogs > 0) {
			--hudLogs;
			LogInfo << "D3D12 hud sample xy=(" << projected[0].x << ", " << projected[0].y
			        << ") uv=(" << projected[0].u0 << ", " << projected[0].v0 << ")";
		}
	}
	for(int i = 1; i + 1 < count; ++i) {
		out.push_back(projected[0]);
		out.push_back(projected[i]);
		out.push_back(projected[i + 1]);
	}
}

void emitClippedTriangle(const HVert & a, const HVert & b, const HVert & c, const Rect & vp,
                         bool worldSpace, bool hudSpace, std::vector<GpuVert> & out) {
	if(hudSpace) {
		HVert tri[3] = { a, b, c };
		emitProjectedFan(tri, 3, vp, false, false, true, out);
		return;
	}
	HVert clipped[kMaxClipVerts];
	int n = 0;
	if(worldSpace) {
		if(insideWorldFrustum(a.x, a.y, a.z, a.w) && insideWorldFrustum(b.x, b.y, b.z, b.w)
		   && insideWorldFrustum(c.x, c.y, c.z, c.w)) {
			clipped[0] = a;
			clipped[1] = b;
			clipped[2] = c;
			n = 3;
		} else {
			n = clipHomogeneousFrustum(a, b, c, clipped, lerpH);
		}
	} else {
		const float xMin = -kTLClipPad;
		const float yMin = -kTLClipPad;
		const float xMax = float((std::max)(vp.width(), s32(1))) + kTLClipPad;
		const float yMax = float((std::max)(vp.height(), s32(1))) + kTLClipPad;
		if(insideTLFrustum(a.x, a.y, a.z, a.w, xMin, xMax, yMin, yMax)
		   && insideTLFrustum(b.x, b.y, b.z, b.w, xMin, xMax, yMin, yMax)
		   && insideTLFrustum(c.x, c.y, c.z, c.w, xMin, xMax, yMin, yMax)) {
			clipped[0] = a;
			clipped[1] = b;
			clipped[2] = c;
			n = 3;
		} else {
			n = clipScreenFrustum(a, b, c, clipped, xMin, xMax, yMin, yMax, lerpH);
		}
	}
	if(n >= 3) {
		emitProjectedFan(clipped, n, vp, worldSpace, !worldSpace, false, out);
	}
}

HVert fromWorld(const SMY_VERTEX & v, const glm::mat4x4 & view, const glm::mat4x4 & proj, const VertexFog & fog) {
	const Vec4f clip = proj * view * Vec4f(v.p, 1.f);
	HVert h;
	h.x = clip.x;
	h.y = clip.y;
	h.z = clip.z;
	h.w = clip.w;
	h.color = toPacked(v.color);
	h.specular = fogFactorSpecular(clip.w, fog.enable, fog.start, fog.end);
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
	h.color = toPacked(v.color);
	h.specular = fogFactorSpecular(std::abs(v.w), fog.enable, fog.start, fog.end);
	h.u = v.uv.x;
	h.v = v.uv.y;
	return h;
}

void clipTLTriangles(Renderer::Primitive primitive, const TexturedVertex * verts, size_t nverts,
                     const unsigned short * indices, size_t nindices, const Rect & vp,
                     const VertexFog & fog, std::vector<GpuVert> & out) {
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
		const bool hud = isHudW(verts[a].w) && isHudW(verts[b].w) && isHudW(verts[c].w);
		emitClippedTriangle(fromTL(verts[a], fog), fromTL(verts[b], fog), fromTL(verts[c], fog),
		                    vp, false, hud, out);
	});
}

void clipWorldTriangles(Renderer::Primitive primitive, const SMY_VERTEX * verts, size_t nverts,
                        const unsigned short * indices, size_t nindices, const glm::mat4x4 & view,
                        const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                        std::vector<GpuVert> & out) {
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

struct HVert3 {
	float x, y, z, w;
	u32 color;
	u32 specular;
	float u[3], v[3];
};

HVert3 lerpH3(const HVert3 & a, const HVert3 & b, float t) {
	HVert3 o;
	o.x = a.x + (b.x - a.x) * t;
	o.y = a.y + (b.y - a.y) * t;
	o.z = a.z + (b.z - a.z) * t;
	o.w = a.w + (b.w - a.w) * t;
	o.color = lerpColor(a.color, b.color, t);
	o.specular = lerpColor(a.specular, b.specular, t);
	for(int i = 0; i < 3; ++i) {
		o.u[i] = a.u[i] + (b.u[i] - a.u[i]) * t;
		o.v[i] = a.v[i] + (b.v[i] - a.v[i]) * t;
	}
	return o;
}

void emitProjectedFan3(const HVert3 * poly, int n, const Rect & vp, std::vector<GpuVert> & out) {
	if(n < 3) {
		return;
	}
	GpuVert projected[kMaxClipVerts];
	const int count = (std::min)(n, kMaxClipVerts);
	for(int i = 0; i < count; ++i) {
		float x, y, z, rhw;
		projectClipToTL(Vec4f(poly[i].x, poly[i].y, poly[i].z, poly[i].w), vp, x, y, z, rhw);
		if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(rhw)
		   || rhw <= 0.f) {
			return;
		}
		projected[i] = GpuVert{ x, y, z, rhw, poly[i].color, poly[i].specular,
		                        poly[i].u[0], poly[i].v[0], poly[i].u[1], poly[i].v[1],
		                        poly[i].u[2], poly[i].v[2] };
	}
	for(int i = 1; i + 1 < count; ++i) {
		out.push_back(projected[0]);
		out.push_back(projected[i]);
		out.push_back(projected[i + 1]);
	}
}

void clipWorldTriangles3(Renderer::Primitive primitive, const SMY_VERTEX3 * verts, size_t nverts,
                         const unsigned short * indices, size_t nindices, const glm::mat4x4 & view,
                         const glm::mat4x4 & proj, const Rect & vp, const VertexFog & fog,
                         std::vector<GpuVert> & out) {
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
		auto from = [&](const SMY_VERTEX3 & src) {
			const Vec4f clip = proj * view * Vec4f(src.p, 1.f);
			HVert3 h;
			h.x = clip.x;
			h.y = clip.y;
			h.z = clip.z;
			h.w = clip.w;
			h.color = toPacked(src.color);
			h.specular = fogFactorSpecular(clip.w, fog.enable, fog.start, fog.end);
			for(int i = 0; i < 3; ++i) {
				h.u[i] = src.uv[i].x;
				h.v[i] = src.uv[i].y;
			}
			return h;
		};
		const HVert3 a = from(verts[ia]);
		const HVert3 b = from(verts[ib]);
		const HVert3 c = from(verts[ic]);
		HVert3 clipped[kMaxClipVerts];
		int cn = 0;
		if(insideWorldFrustum(a.x, a.y, a.z, a.w) && insideWorldFrustum(b.x, b.y, b.z, b.w)
		   && insideWorldFrustum(c.x, c.y, c.z, c.w)) {
			clipped[0] = a;
			clipped[1] = b;
			clipped[2] = c;
			cn = 3;
		} else {
			cn = clipHomogeneousFrustum(a, b, c, clipped, lerpH3);
		}
		if(cn >= 3) {
			emitProjectedFan3(clipped, cn, vp, out);
		}
	});
}

void convertTL(const TexturedVertex * src, size_t count, const Rect & vp, const VertexFog & fog,
               std::vector<GpuVert> & dst) {
	dst.resize(count);
	for(size_t i = 0; i < count; ++i) {
		const TexturedVertex & v = src[i];
		float x, y, z, rhw;
		projectToScreen(v.p.x, v.p.y, v.p.z, v.w, x, y, z, rhw);
		if(std::abs(v.w - 1.f) > 1e-5f) {
			x += float(vp.left);
			y += float(vp.top);
			z = glm::clamp(z, 0.f, 1.f);
		} else {
			rhw = 1.f;
		}
		dst[i] = toGpu(x, y, z, rhw, toPacked(v.color),
		               fogFactorSpecular(v.w, fog.enable, fog.start, fog.end), v.uv.x, v.uv.y);
	}
}

D3D12_BLEND toBlend(BlendingFactor factor) {
	static const D3D12_BLEND table[] = {
		D3D12_BLEND_ZERO,
		D3D12_BLEND_ONE,
		D3D12_BLEND_SRC_COLOR,
		D3D12_BLEND_SRC_ALPHA,
		D3D12_BLEND_INV_SRC_COLOR,
		D3D12_BLEND_INV_SRC_ALPHA,
		D3D12_BLEND_SRC_ALPHA_SAT,
		D3D12_BLEND_DEST_COLOR,
		D3D12_BLEND_DEST_ALPHA,
		D3D12_BLEND_INV_DEST_COLOR,
		D3D12_BLEND_INV_DEST_ALPHA
	};
	return table[factor];
}

void transition(ID3D12GraphicsCommandList * list, ID3D12Resource * res,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER b {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}

const char * kShader = R"(
cbuffer Root : register(b0) {
	float rtW;
	float rtH;
	float fogEnable;
	float alphaRef;
	float3 fogColor;
	float colorMode;
	float texMask;
	float alphaMul;
	float2 pad;
};
Texture2D T0 : register(t0);
Texture2D T1 : register(t1);
Texture2D T2 : register(t2);
SamplerState S0 : register(s0);
SamplerState S1 : register(s1);
SamplerState S2 : register(s2);

struct VSIn {
	float3 pos : POSITION;
	float rhw : TEXCOORD3;
	float4 color : COLOR0;
	float4 specular : COLOR1;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
};
struct VSOut {
	float4 pos : SV_Position;
	float4 color : COLOR0;
	float fog : COLOR1;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
};

VSOut VSMain(VSIn i) {
	VSOut o;
	float rhw = (i.rhw > 0.0) ? i.rhw : 1.0;
	float clipW = 1.0 / rhw;
	// D3D10+ pixel centers sit at +0.5. Draw.cpp skips its D3D9 −0.5 when
	// needsHalfPixelOffset() is false, so fonts and bitmaps share this shift.
	float ndcX = ((i.pos.x - 0.5) / rtW) * 2.0 - 1.0;
	float ndcY = 1.0 - ((i.pos.y - 0.5) / rtH) * 2.0;
	o.pos = float4(ndcX * clipW, ndcY * clipW, i.pos.z * clipW, clipW);
	o.color = i.color;
	o.fog = i.specular.a;
	o.uv0 = i.uv0;
	o.uv1 = i.uv1;
	o.uv2 = i.uv2;
	return o;
}

float4 PSMain(VSOut i) : SV_Target {
	float4 c = i.color;
	if(texMask > 0.5) {
		float4 t = T0.Sample(S0, i.uv0);
		// colorMode: 0 modulate, 1 select tex, 2 disable (vertex RGB), 3/4 modulate 2×/4×.
		// Alpha follows D3D9 stage0: SelectArg1 = texture A (cutout tests that).
		// OpDisable (fonts) keeps vertex.a * tex.a.
		if(colorMode < 0.5) {
			c.rgb *= t.rgb;
		} else if(colorMode < 1.5) {
			c.rgb = t.rgb;
		} else if(colorMode < 2.5) {
			c.rgb = i.color.rgb;
		} else if(colorMode < 3.5) {
			c.rgb *= t.rgb * 2.0;
		} else {
			c.rgb *= t.rgb * 4.0;
		}
		if(colorMode > 1.5 && colorMode < 2.5) {
			c.a = i.color.a * t.a;
		} else if(alphaMul > 0.5) {
			c.a = i.color.a * t.a;
		} else {
			c.a = t.a;
		}
	}
	if(texMask > 1.5) {
		c *= T1.Sample(S1, i.uv1);
	}
	if(texMask > 2.5) {
		c *= T2.Sample(S2, i.uv2);
	}
	// D3D9 ALPHAFUNC GREATER: discard when alpha is not greater than the ref.
	// Blended cutout uses ref 0 — the old `alphaRef > 0` guard never discarded.
	if(alphaRef >= 0.0 && c.a <= alphaRef) {
		discard;
	}
	if(fogEnable > 0.5) {
		c.rgb = lerp(fogColor, c.rgb, i.fog);
	}
	return c;
}
)";

} // namespace

struct D3D12Renderer::Impl {
	HWND hwnd = nullptr;
	DxPtr<ID3D12Device> device;
	DxPtr<ID3D12CommandQueue> queue;
	DxPtr<IDXGISwapChain3> swapchain;
	DxPtr<ID3D12DescriptorHeap> rtvHeap;
	DxPtr<ID3D12DescriptorHeap> dsvHeap;
	DxPtr<ID3D12DescriptorHeap> srvHeap;
	DxPtr<ID3D12DescriptorHeap> samplerHeap;
	DxPtr<ID3D12Resource> backbuffers[kFrameCount];
	DxPtr<ID3D12Resource> depth;
	DxPtr<ID3D12CommandAllocator> allocators[kFrameCount];
	DxPtr<ID3D12GraphicsCommandList> list;
	DxPtr<ID3D12Fence> fence;
	DxPtr<ID3D12RootSignature> root;
	DxPtr<ID3D12Resource> upload;
	DxPtr<ID3D12Resource> white;
	std::vector<DxPtr<ID3D12Resource>> inflight;
	HANDLE fenceEvent = nullptr;
	UINT64 fenceValue = 0;
	UINT64 frameFence[kFrameCount] {};
	UINT frame = 0;
	UINT rtvSize = 0;
	UINT srvSize = 0;
	UINT sampSize = 0;
	UINT nextSrv = 1;
	UINT uploadOffset = 0;
	void * uploadMapped = nullptr;
	bool recording = false;
	bool inPresentState = true;
	std::unordered_map<u32, DxPtr<ID3D12PipelineState>> psos;
	DxPtr<ID3DBlob> vs;
	DxPtr<ID3DBlob> ps;
};

ID3D12Device * D3D12Renderer::device() const {
	return m ? m->device.Get() : nullptr;
}

D3D12TextureStage::D3D12TextureStage(unsigned stage)
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

D3D12Texture::D3D12Texture(D3D12Renderer * renderer)
	: m_renderer(renderer)
{ }

D3D12Texture::~D3D12Texture() {
	destroy();
}

bool D3D12Texture::create() {
	if(m_size.x <= 0 || m_size.y <= 0) {
		return false;
	}
	m_storedSize = m_size;
	return createGpuTexture();
}

bool D3D12Texture::createGpuTexture() {
	if(!m_renderer || !m_renderer->device()) {
		return false;
	}
	destroy();
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = UINT(m_storedSize.x);
	desc.Height = UINT(m_storedSize.y);
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	ID3D12Resource * res = nullptr;
	if(FAILED(m_renderer->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
	                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                                        IID_PPV_ARGS(&res)))) {
		LogError << "D3D12: CreateTexture " << m_storedSize.x << "x" << m_storedSize.y << " failed";
		return false;
	}
	m_resource = res;
	m_srvIndex = m_renderer->allocateSrv();
	m_renderer->createTextureSrv(m_resource, m_srvIndex);
	m_gpuSize = m_storedSize;
	m_onGpu = false;
	return true;
}

void D3D12Texture::upload() {
	if(!m_image.isValid() || !m_renderer || !m_resource) {
		if(m_image.isValid() && !m_resource) {
			if(!createGpuTexture()) {
				return;
			}
		} else {
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
	const UINT w = UINT(src->getWidth());
	const UINT h = UINT(src->getHeight());
	std::vector<u32> bgra(w * h);
	const size_t channels = Image::getNumChannels(src->getFormat());
	const unsigned char * data = src->getData();
	for(UINT y = 0; y < h; ++y) {
		const unsigned char * row = data + y * w * channels;
		for(UINT x = 0; x < w; ++x) {
			const unsigned char * p = row + x * channels;
			u8 r = 255, g = 255, b = 255, a = 255;
			switch(src->getFormat()) {
				case Image::Format_L8: r = g = b = p[0]; break;
				case Image::Format_A8: a = p[0]; break;
				case Image::Format_L8A8: r = g = b = p[0]; a = p[1]; break;
				case Image::Format_R8G8B8: r = p[0]; g = p[1]; b = p[2]; break;
				case Image::Format_B8G8R8: b = p[0]; g = p[1]; r = p[2]; break;
				case Image::Format_R8G8B8A8: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
				case Image::Format_B8G8R8A8: b = p[0]; g = p[1]; r = p[2]; a = p[3]; break;
				default: break;
			}
			bgra[y * w + x] = (u32(a) << 24) | (u32(r) << 16) | (u32(g) << 8) | u32(b);
		}
	}
	if(m_renderer->uploadTextureData(m_resource, bgra.data(), w, h, m_onGpu)) {
		m_onGpu = true;
	}
}

void D3D12Texture::destroy() {
	if(m_renderer) {
		for(size_t i = 0; i < m_renderer->getTextureStageCount(); ++i) {
			if(m_renderer->GetTextureStage(i)->getTexture() == this) {
				m_renderer->ResetTexture(unsigned(i));
			}
		}
		if(m_resource) {
			m_renderer->freeTexture(m_resource);
			m_resource = nullptr;
		}
	}
	m_gpuSize = Vec2i(0);
	m_onGpu = false;
}

namespace {

template <class Vertex>
class D3D12VertexBufferTL final : public VertexBuffer<TexturedVertex> {
public:
	D3D12VertexBufferTL(D3D12Renderer * renderer, size_t capacity)
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
		return (offset < m_data.size()) ? m_data.data() + offset : m_data.data();
	}
	
	void unlock() override { }
	
	void draw(Renderer::Primitive primitive, size_t count, size_t offset = 0) const override {
		if(m_renderer && count && offset + count <= m_data.size()) {
			m_renderer->drawTextured(primitive, m_data.data() + offset, count);
		}
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
		if(m_renderer && count && offset + count <= m_data.size()) {
			m_renderer->drawIndexed(primitive, m_data.data() + offset, count,
			                        const_cast<unsigned short *>(indices), nbindices);
		}
	}
	
private:
	D3D12Renderer * m_renderer;
	std::vector<TexturedVertex> m_data;
};

template <class Vertex>
class D3D12VertexBufferWorld final : public VertexBuffer<Vertex> {
public:
	D3D12VertexBufferWorld(D3D12Renderer * renderer, size_t capacity)
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
		return (offset < m_data.size()) ? m_data.data() + offset : m_data.data();
	}
	
	void unlock() override { }
	
	void draw(Renderer::Primitive primitive, size_t count, size_t offset = 0) const override {
		if(m_renderer && count && offset + count <= m_data.size()) {
			m_renderer->drawWorldVertices(primitive, m_data.data() + offset, count);
		}
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
		if(m_renderer && count && offset + count <= m_data.size() && indices && nbindices) {
			m_renderer->drawWorldIndexed(primitive, m_data.data() + offset, count, indices, nbindices);
		}
	}
	
private:
	D3D12Renderer * m_renderer;
	std::vector<Vertex> m_data;
};

} // namespace

D3D12Renderer::D3D12Renderer()
	: m(new Impl)
{
	for(unsigned i = 0; i < 4; ++i) {
		m_TextureStages.push_back(std::make_unique<D3D12TextureStage>(i));
	}
}

D3D12Renderer::~D3D12Renderer() {
	if(m_initialized) {
		onRendererShutdown();
		m_initialized = false;
	}
	releaseDevice();
	delete m;
	m = nullptr;
}

unsigned D3D12Renderer::allocateSrv() {
	if(m->nextSrv >= kSrvHeapSize) {
		LogError << "D3D12: SRV heap exhausted";
		return 0;
	}
	return m->nextSrv++;
}

void D3D12Renderer::createTextureSrv(ID3D12Resource * resource, unsigned index) {
	if(!m->device || !resource) {
		return;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
	srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m->srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += SIZE_T(index) * m->srvSize;
	m->device->CreateShaderResourceView(resource, &srv, cpu);
}

void D3D12Renderer::freeTexture(ID3D12Resource * resource) {
	if(resource) {
		waitGpu();
		resource->Release();
	}
}

void D3D12Renderer::waitGpu() {
	if(!m->queue || !m->fence) {
		return;
	}
	const UINT64 value = ++m->fenceValue;
	if(FAILED(m->queue->Signal(m->fence.Get(), value))) {
		return;
	}
	if(m->fence->GetCompletedValue() < value) {
		m->fence->SetEventOnCompletion(value, m->fenceEvent);
		WaitForSingleObject(m->fenceEvent, INFINITE);
	}
	m->inflight.clear();
}

bool D3D12Renderer::uploadTextureData(ID3D12Resource * dest, const void * bgra, unsigned width,
                                      unsigned height, bool alreadyOnGpu) {
	if(!m->device || !dest || !bgra || width == 0 || height == 0) {
		return false;
	}
	D3D12_RESOURCE_DESC destDesc = dest->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
	UINT numRows = 0;
	UINT64 rowSizeInBytes = 0;
	UINT64 uploadSize = 0;
	m->device->GetCopyableFootprints(&destDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes,
	                                 &uploadSize);
	if(uploadSize == 0 || footprint.Footprint.RowPitch == 0) {
		return false;
	}
	const UINT srcPitch = width * 4;
	const UINT rowPitch = footprint.Footprint.RowPitch;
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = uploadSize;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	DxPtr<ID3D12Resource> staging;
	if(FAILED(m->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
	                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
	                                             IID_PPV_ARGS(staging.put())))) {
		return false;
	}
	void * mapped = nullptr;
	if(FAILED(staging->Map(0, nullptr, &mapped)) || !mapped) {
		return false;
	}
	const char * srcRows = static_cast<const char *>(bgra);
	char * dstRows = static_cast<char *>(mapped) + footprint.Offset;
	const UINT copyPitch = UINT((std::min)(UINT64(srcPitch), rowSizeInBytes));
	for(UINT y = 0; y < numRows && y < height; ++y) {
		std::memcpy(dstRows + UINT64(y) * rowPitch, srcRows + UINT64(y) * srcPitch, copyPitch);
	}
	staging->Unmap(0, nullptr);
	
	const bool wasRecording = m->recording;
	if(!ensureCommandList()) {
		return false;
	}
	if(alreadyOnGpu) {
		transition(m->list.Get(), dest, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_COPY_DEST);
	}
	D3D12_TEXTURE_COPY_LOCATION dst {};
	dst.pResource = dest;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src {};
	src.pResource = staging.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = footprint;
	m->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	transition(m->list.Get(), dest, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m->inflight.push_back(std::move(staging));
	if(!wasRecording) {
		m->list->Close();
		ID3D12CommandList * lists[] = { m->list.Get() };
		m->queue->ExecuteCommandLists(1, lists);
		waitGpu();
		m->recording = false;
	}
	return true;
}

bool D3D12Renderer::createPipeline() {
	DxPtr<ID3DBlob> err;
	if(FAILED(D3DCompile(kShader, std::strlen(kShader), "d3d12", nullptr, nullptr, "VSMain",
	                     "vs_5_0", 0, 0, m->vs.put(), err.put()))) {
		LogError << "D3D12: VS compile failed: "
		         << (err ? static_cast<const char *>(err->GetBufferPointer()) : "");
		return false;
	}
	if(FAILED(D3DCompile(kShader, std::strlen(kShader), "d3d12", nullptr, nullptr, "PSMain",
	                     "ps_5_0", 0, 0, m->ps.put(), err.put()))) {
		LogError << "D3D12: PS compile failed: "
		         << (err ? static_cast<const char *>(err->GetBufferPointer()) : "");
		return false;
	}
	
	// One SRV/sampler table per stage so each draw binds the texture's own
	// GPU handle. Copying into 3 shared pack slots made every draw sample
	// the last texture of the frame (white menu, tiled HUD icon in-world).
	D3D12_DESCRIPTOR_RANGE srvRanges[3] {};
	D3D12_DESCRIPTOR_RANGE sampRanges[3] {};
	D3D12_ROOT_PARAMETER params[7] {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = 12;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	for(UINT i = 0; i < 3; ++i) {
		srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRanges[i].NumDescriptors = 1;
		srvRanges[i].BaseShaderRegister = i;
		sampRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		sampRanges[i].NumDescriptors = 1;
		sampRanges[i].BaseShaderRegister = i;
		params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
		params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
		params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[4 + i].DescriptorTable.NumDescriptorRanges = 1;
		params[4 + i].DescriptorTable.pDescriptorRanges = &sampRanges[i];
		params[4 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	
	D3D12_ROOT_SIGNATURE_DESC rs {};
	rs.NumParameters = 7;
	rs.pParameters = params;
	rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	DxPtr<ID3DBlob> blob;
	if(FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put()))) {
		LogError << "D3D12: root signature failed";
		return false;
	}
	if(FAILED(m->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                         IID_PPV_ARGS(m->root.put())))) {
		return false;
	}
	return true;
}

bool D3D12Renderer::createFrameResources() {
	m->rtvSize = m->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m->srvSize = m->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m->sampSize = m->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	
	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc {};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = kFrameCount;
	if(FAILED(m->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(m->rtvHeap.put())))) {
		return false;
	}
	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc {};
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = 1;
	if(FAILED(m->device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(m->dsvHeap.put())))) {
		return false;
	}
	D3D12_DESCRIPTOR_HEAP_DESC srvDesc {};
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = kSrvHeapSize;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(m->device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(m->srvHeap.put())))) {
		return false;
	}
	D3D12_DESCRIPTOR_HEAP_DESC sampDesc {};
	sampDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	sampDesc.NumDescriptors = 6;
	sampDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(m->device->CreateDescriptorHeap(&sampDesc, IID_PPV_ARGS(m->samplerHeap.put())))) {
		return false;
	}
	
	// wrap/clamp/mirror × linear/point — created once, never overwritten mid-frame.
	const D3D12_TEXTURE_ADDRESS_MODE wraps[3] = {
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR
	};
	D3D12_CPU_DESCRIPTOR_HANDLE sampCpu = m->samplerHeap->GetCPUDescriptorHandleForHeapStart();
	for(UINT filter = 0; filter < 2; ++filter) {
		for(UINT wrap = 0; wrap < 3; ++wrap) {
			D3D12_SAMPLER_DESC samp {};
			samp.Filter = filter ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samp.AddressU = wraps[wrap];
			samp.AddressV = wraps[wrap];
			samp.AddressW = wraps[wrap];
			samp.MaxLOD = 0.f;
			samp.MaxAnisotropy = 1;
			D3D12_CPU_DESCRIPTOR_HANDLE h = sampCpu;
			h.ptr += SIZE_T(wrap + filter * 3) * m->sampSize;
			m->device->CreateSampler(&samp, h);
		}
	}
	
	for(UINT i = 0; i < kFrameCount; ++i) {
		if(FAILED(m->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                            IID_PPV_ARGS(m->allocators[i].put())))) {
			return false;
		}
	}
	if(FAILED(m->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m->allocators[0].Get(),
	                                       nullptr, IID_PPV_ARGS(m->list.put())))) {
		return false;
	}
	m->list->Close();
	
	if(FAILED(m->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m->fence.put())))) {
		return false;
	}
	m->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	
	D3D12_RESOURCE_DESC ub {};
	ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	ub.Width = kUploadBytes;
	ub.Height = 1;
	ub.DepthOrArraySize = 1;
	ub.MipLevels = 1;
	ub.SampleDesc.Count = 1;
	ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES uploadHeap {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	if(FAILED(m->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ub,
	                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
	                                             IID_PPV_ARGS(m->upload.put())))) {
		return false;
	}
	m->upload->Map(0, nullptr, &m->uploadMapped);
	
	D3D12_RESOURCE_DESC whiteDesc {};
	whiteDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	whiteDesc.Width = 1;
	whiteDesc.Height = 1;
	whiteDesc.DepthOrArraySize = 1;
	whiteDesc.MipLevels = 1;
	whiteDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	whiteDesc.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES defHeap {};
	defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if(FAILED(m->device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &whiteDesc,
	                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                             IID_PPV_ARGS(m->white.put())))) {
		return false;
	}
	createTextureSrv(m->white.Get(), 0);
	const u32 whitePx = 0xffffffffu;
	uploadTextureData(m->white.Get(), &whitePx, 1, 1, false);
	return true;
}

void D3D12Renderer::resizeSwapchain(int width, int height) {
	if(!m->swapchain || width <= 0 || height <= 0) {
		return;
	}
	waitGpu();
	for(UINT i = 0; i < kFrameCount; ++i) {
		m->backbuffers[i].reset();
	}
	m->depth.reset();
	if(FAILED(m->swapchain->ResizeBuffers(kFrameCount, UINT(width), UINT(height),
	                                      DXGI_FORMAT_R8G8B8A8_UNORM, 0))) {
		LogError << "D3D12: ResizeBuffers failed";
		return;
	}
	m_width = width;
	m_height = height;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for(UINT i = 0; i < kFrameCount; ++i) {
		m->swapchain->GetBuffer(i, IID_PPV_ARGS(m->backbuffers[i].put()));
		m->device->CreateRenderTargetView(m->backbuffers[i].Get(), nullptr, rtv);
		rtv.ptr += m->rtvSize;
	}
	D3D12_RESOURCE_DESC depthDesc {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = UINT(width);
	depthDesc.Height = UINT(height);
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE clear {};
	clear.Format = depthDesc.Format;
	clear.DepthStencil.Depth = 1.f;
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if(FAILED(m->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
	                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
	                                             IID_PPV_ARGS(m->depth.put())))) {
		LogError << "D3D12: depth buffer failed";
		return;
	}
	m->device->CreateDepthStencilView(m->depth.Get(), nullptr,
	                                  m->dsvHeap->GetCPUDescriptorHandleForHeapStart());
	m->frame = m->swapchain->GetCurrentBackBufferIndex();
	m->inPresentState = true;
}

bool D3D12Renderer::createDevice(void * nativeHwnd, int width, int height) {
	HWND hwnd = static_cast<HWND>(nativeHwnd);
	if(!hwnd || width <= 0 || height <= 0) {
		LogError << "D3D12: invalid window";
		return false;
	}
	releaseDevice();
	m->hwnd = hwnd;
	
	DxPtr<IDXGIFactory6> factory;
	if(FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())))) {
		LogError << "D3D12: CreateDXGIFactory2 failed";
		return false;
	}
	DxPtr<IDXGIAdapter1> adapter;
	if(FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
	                                              IID_PPV_ARGS(adapter.put())))) {
		factory->EnumAdapters1(0, adapter.put());
	}
	DXGI_ADAPTER_DESC1 ad {};
	if(adapter) {
		adapter->GetDesc1(&ad);
	}
	if(FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m->device.put())))) {
		LogError << "D3D12: D3D12CreateDevice failed";
		return false;
	}
	
	D3D12_COMMAND_QUEUE_DESC qd {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if(FAILED(m->device->CreateCommandQueue(&qd, IID_PPV_ARGS(m->queue.put())))) {
		return false;
	}
	
	DXGI_SWAP_CHAIN_DESC1 sd {};
	sd.Width = UINT(width);
	sd.Height = UINT(height);
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = kFrameCount;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling = DXGI_SCALING_STRETCH;
	DxPtr<IDXGISwapChain1> sc1;
	if(FAILED(factory->CreateSwapChainForHwnd(m->queue.Get(), hwnd, &sd, nullptr, nullptr, sc1.put()))) {
		LogError << "D3D12: CreateSwapChainForHwnd failed";
		return false;
	}
	factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
	if(FAILED(sc1->QueryInterface(IID_PPV_ARGS(m->swapchain.put())))) {
		LogError << "D3D12: IDXGISwapChain3 QI failed";
		return false;
	}
	
	if(!createPipeline() || !createFrameResources()) {
		releaseDevice();
		return false;
	}
	resizeSwapchain(width, height);
	
	char name[128] {};
	WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, int(sizeof(name)), nullptr, nullptr);
	LogInfo << "Using D3D12 renderer " << width << "x" << height
	        << " (raster only, adapter=" << name << ")";
	return true;
}

void D3D12Renderer::releaseDevice() {
	if(!m) {
		return;
	}
	waitGpu();
	if(m->uploadMapped && m->upload) {
		m->upload->Unmap(0, nullptr);
		m->uploadMapped = nullptr;
	}
	if(m->fenceEvent) {
		CloseHandle(m->fenceEvent);
		m->fenceEvent = nullptr;
	}
	m->psos.clear();
	*m = Impl();
}

void D3D12Renderer::initialize() {
	if(!m->device) {
		LogError << "D3D12: initialize() without a device";
		return;
	}
	SetViewport(Rect(0, 0, m_width, m_height));
	m_initialized = true;
	onRendererInit();
}

void D3D12Renderer::beforeResize(bool wasOrIsFullscreen) {
	ARX_UNUSED(wasOrIsFullscreen);
}

void D3D12Renderer::afterResize() {
	if(!m->hwnd) {
		return;
	}
	RECT client {};
	GetClientRect(m->hwnd, &client);
	const int w = int(client.right - client.left);
	const int h = int(client.bottom - client.top);
	if(w > 0 && h > 0 && (w != m_width || h != m_height)) {
		resizeSwapchain(w, h);
	}
	m_initialized = static_cast<bool>(m->device);
}

void D3D12Renderer::SetViewMatrix(const glm::mat4x4 & matView) {
	m_view = matView;
}

void D3D12Renderer::SetProjectionMatrix(const glm::mat4x4 & matProj) {
	m_proj = matProj;
}

void D3D12Renderer::ReleaseAllTextures() { }
void D3D12Renderer::RestoreAllTextures() { }
void D3D12Renderer::reloadColorKeyTextures() { }

Texture * D3D12Renderer::createTexture() {
	return new D3D12Texture(this);
}

void D3D12Renderer::SetViewport(const Rect & viewport) {
	m_viewport = viewport;
}

void D3D12Renderer::SetScissor(const Rect & rect) {
	m_scissor = rect;
}

void D3D12Renderer::SetFogColor(Color color) {
	m_fogColor = color;
}

void D3D12Renderer::SetFogParams(float fogStart, float fogEnd) {
	m_fogStart = fogStart;
	m_fogEnd = fogEnd;
}

void D3D12Renderer::SetAntialiasing(bool enable) {
	ARX_UNUSED(enable);
}

void D3D12Renderer::SetFillMode(FillMode mode) {
	m_fillMode = mode;
}

bool D3D12Renderer::ensureCommandList() {
	if(!m->device || !m->list) {
		return false;
	}
	if(m->recording) {
		return true;
	}
	if(FAILED(m->allocators[m->frame]->Reset())) {
		return false;
	}
	if(FAILED(m->list->Reset(m->allocators[m->frame].Get(), nullptr))) {
		return false;
	}
	m->recording = true;
	m->uploadOffset = 0;
	return true;
}

bool D3D12Renderer::beginRecording() {
	if(!ensureCommandList()) {
		return false;
	}
	if(!m->backbuffers[m->frame]) {
		return false;
	}
	if(m->inPresentState) {
		// FLIP_DISCARD leaves buffers in COMMON after Present (and at creation).
		// PRESENT→RT here discarded the whole command list: black window, no menu.
		transition(m->list.Get(), m->backbuffers[m->frame].Get(),
		           D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
		m->inPresentState = false;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += SIZE_T(m->frame) * m->rtvSize;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m->dsvHeap->GetCPUDescriptorHandleForHeapStart();
	m->list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	D3D12_VIEWPORT vp {};
	vp.Width = float(m_width);
	vp.Height = float(m_height);
	vp.MaxDepth = 1.f;
	m->list->RSSetViewports(1, &vp);
	D3D12_RECT sc { 0, 0, LONG(m_width), LONG(m_height) };
	if(m_scissor.isValid()) {
		sc.left = LONG(m_scissor.left);
		sc.top = LONG(m_scissor.top);
		sc.right = LONG(m_scissor.right);
		sc.bottom = LONG(m_scissor.bottom);
	}
	m->list->RSSetScissorRects(1, &sc);
	ID3D12DescriptorHeap * heaps[] = { m->srvHeap.Get(), m->samplerHeap.Get() };
	m->list->SetDescriptorHeaps(2, heaps);
	m->list->SetGraphicsRootSignature(m->root.Get());
	return true;
}

void D3D12Renderer::Clear(BufferFlags bufferFlags, Color clearColor, float clearDepth,
                          size_t nrects, Rect * rect) {
	if(!beginRecording()) {
		return;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += SIZE_T(m->frame) * m->rtvSize;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m->dsvHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_RECT rects[8];
	UINT n = 0;
	if(nrects > 0 && rect) {
		n = UINT((std::min)(nrects, size_t(8)));
		for(UINT i = 0; i < n; ++i) {
			rects[i].left = LONG(rect[i].left);
			rects[i].top = LONG(rect[i].top);
			rects[i].right = LONG(rect[i].right);
			rects[i].bottom = LONG(rect[i].bottom);
		}
	}
	if(bufferFlags & ColorBuffer) {
		const float c[4] = {
			float(clearColor.r) / 255.f, float(clearColor.g) / 255.f,
			float(clearColor.b) / 255.f, float(clearColor.a) / 255.f
		};
		m->list->ClearRenderTargetView(rtv, c, n, n ? rects : nullptr);
	}
	if(bufferFlags & DepthBuffer) {
		m->list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clearDepth, 0, n, n ? rects : nullptr);
	}
}

void D3D12Renderer::bindDrawState(Primitive primitive) {
	BlendingFactor blendSrc = m_state.getBlendSrc();
	BlendingFactor blendDst = m_state.getBlendDst();
	// D3D9 always alphatests when AlphaCutout. Opaque uses ref 128; blended uses
	// ref 0 (GREATER → drop A==0). Send -1 when cutout is off so the PS skips.
	float alphaRef = -1.f;
	if(m_state.getAlphaCutout()) {
		if(blendSrc == BlendOne && blendDst == BlendZero) {
			alphaRef = 128.f / 255.f;
		} else {
			alphaRef = 0.f;
			if(blendSrc == BlendOne && blendDst == BlendOne) {
				blendSrc = BlendSrcAlpha;
			}
		}
	}
	u32 key = u32(blendSrc) | (u32(blendDst) << 4);
	if(m_state.getDepthTest()) key |= 1u << 8;
	if(m_state.getDepthWrite()) key |= 1u << 9;
	if(m_state.getCull()) key |= 1u << 10;
	if(m_state.getAlphaCutout()) key |= 1u << 11;
	if(m_fillMode == FillWireframe) key |= 1u << 12;
	if(primitive == LineList || primitive == LineStrip) key |= 1u << 13;
	key |= (m_state.getDepthOffset() & 31u) << 14;
	
	DxPtr<ID3D12PipelineState> & pso = m->psos[key];
	if(!pso) {
		D3D12_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
		pd.pRootSignature = m->root.Get();
		pd.VS = { m->vs->GetBufferPointer(), m->vs->GetBufferSize() };
		pd.PS = { m->ps->GetBufferPointer(), m->ps->GetBufferSize() };
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		const bool blend = (blendSrc != BlendOne || blendDst != BlendZero);
		pd.BlendState.RenderTarget[0].BlendEnable = blend ? TRUE : FALSE;
		pd.BlendState.RenderTarget[0].SrcBlend = toBlend(blendSrc);
		pd.BlendState.RenderTarget[0].DestBlend = toBlend(blendDst);
		pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		pd.SampleMask = 0xffffffff;
		pd.RasterizerState.FillMode = (m_fillMode == FillWireframe) ? D3D12_FILL_MODE_WIREFRAME
		                                                            : D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = m_state.getCull() ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
		pd.RasterizerState.FrontCounterClockwise = FALSE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		const float depthOffset = -float(m_state.getDepthOffset());
		pd.RasterizerState.SlopeScaledDepthBias = depthOffset;
		pd.RasterizerState.DepthBias = (depthOffset == 0.f) ? 0 : int(depthOffset);
		pd.DepthStencilState.DepthEnable = TRUE;
		pd.DepthStencilState.DepthWriteMask = m_state.getDepthWrite() ? D3D12_DEPTH_WRITE_MASK_ALL
		                                                              : D3D12_DEPTH_WRITE_MASK_ZERO;
		pd.DepthStencilState.DepthFunc = m_state.getDepthTest() ? D3D12_COMPARISON_FUNC_LESS_EQUAL
		                                                        : D3D12_COMPARISON_FUNC_ALWAYS;
		pd.InputLayout = { layout, 7 };
		pd.PrimitiveTopologyType = (key & (1u << 13)) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
		                                              : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		pd.SampleDesc.Count = 1;
		if(FAILED(m->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pso.put())))) {
			LogError << "D3D12: CreateGraphicsPipelineState failed";
			return;
		}
	}
	m->list->SetPipelineState(pso.Get());
	
	float constants[12] = {
		float((std::max)(m_width, 1)),
		float((std::max)(m_height, 1)),
		// D3D9 forces FOGENABLE FALSE: XYZRHW + fog painted the cell fog-black.
		// Specular-A fog is unused while SPECULARENABLE is off. Match that.
		0.f,
		alphaRef,
		float(m_fogColor.r) / 255.f,
		float(m_fogColor.g) / 255.f,
		float(m_fogColor.b) / 255.f,
		0.f,
		1.f,
		0.f, 0.f, 0.f
	};
	auto * stage0 = static_cast<D3D12TextureStage *>(GetTextureStage(0));
	auto * stage1 = static_cast<D3D12TextureStage *>(GetTextureStage(1));
	auto * stage2 = static_cast<D3D12TextureStage *>(GetTextureStage(2));
	auto * tex0 = stage0 ? static_cast<D3D12Texture *>(stage0->getTexture()) : nullptr;
	auto * tex1 = stage1 ? static_cast<D3D12Texture *>(stage1->getTexture()) : nullptr;
	auto * tex2 = stage2 ? static_cast<D3D12Texture *>(stage2->getTexture()) : nullptr;
	if(tex1 && stage1 && stage1->getColorOp() == TextureStage::OpDisable) {
		tex1 = nullptr;
	}
	if(tex2 && stage2 && stage2->getColorOp() == TextureStage::OpDisable) {
		tex2 = nullptr;
	}
	if(!tex0) {
		constants[8] = 0.f;
		constants[7] = 2.f;
	} else if(stage0->getColorOp() == TextureStage::OpSelectArg1) {
		constants[7] = 1.f;
	} else if(stage0->getColorOp() == TextureStage::OpDisable) {
		constants[7] = 2.f;
	} else if(stage0->getColorOp() == TextureStage::OpModulate2X) {
		constants[7] = 3.f;
	} else if(stage0->getColorOp() == TextureStage::OpModulate4X) {
		constants[7] = 4.f;
	} else {
		constants[7] = 0.f;
	}
	if(stage0 && (stage0->getAlphaOp() == TextureStage::OpModulate
	              || stage0->getAlphaOp() == TextureStage::OpModulate2X
	              || stage0->getAlphaOp() == TextureStage::OpModulate4X)) {
		constants[9] = 1.f;
	}
	if(tex1) {
		constants[8] = 2.f;
	}
	if(tex2) {
		constants[8] = 3.f;
	}
	m->list->SetGraphicsRoot32BitConstants(0, 12, constants, 0);
	
	const unsigned indices[3] = {
		tex0 ? tex0->srvIndex() : 0,
		tex1 ? tex1->srvIndex() : 0,
		tex2 ? tex2->srvIndex() : 0
	};
	D3D12_GPU_DESCRIPTOR_HANDLE srvStart = m->srvHeap->GetGPUDescriptorHandleForHeapStart();
	for(unsigned i = 0; i < 3; ++i) {
		D3D12_GPU_DESCRIPTOR_HANDLE h = srvStart;
		h.ptr += UINT64(indices[i]) * m->srvSize;
		m->list->SetGraphicsRootDescriptorTable(1 + i, h);
	}
	
	D3D12Texture * texs[3] = { tex0, tex1, tex2 };
	D3D12TextureStage * stages[3] = { stage0, stage1, stage2 };
	D3D12_GPU_DESCRIPTOR_HANDLE sampStart = m->samplerHeap->GetGPUDescriptorHandleForHeapStart();
	for(unsigned i = 0; i < 3; ++i) {
		TextureStage::WrapMode wrap = stages[i] ? stages[i]->getWrapMode() : TextureStage::WrapRepeat;
		if(texs[i]) {
			const int tw = texs[i]->getSize().x;
			const int th = texs[i]->getSize().y;
			if(tw <= 0 || th <= 0 || (tw & (tw - 1)) || (th & (th - 1))) {
				wrap = TextureStage::WrapClamp;
			}
		}
		TextureStage::FilterMode minF = stages[i] ? stages[i]->getMinFilter() : TextureStage::FilterLinear;
		TextureStage::FilterMode magF = stages[i] ? stages[i]->getMagFilter() : TextureStage::FilterLinear;
		const UINT wrapI = (wrap == TextureStage::WrapClamp) ? 1u
		                 : (wrap == TextureStage::WrapMirror) ? 2u : 0u;
		const UINT filterI = (minF == TextureStage::FilterLinear && magF == TextureStage::FilterLinear) ? 0u : 1u;
		D3D12_GPU_DESCRIPTOR_HANDLE h = sampStart;
		h.ptr += UINT64(wrapI + filterI * 3) * m->sampSize;
		m->list->SetGraphicsRootDescriptorTable(4 + i, h);
	}
	
	static int texStateLogs = 8;
	if(texStateLogs > 0 && (m_state.getAlphaCutout()
	                        || (blendSrc == BlendZero && blendDst == BlendInvSrcColor))) {
		--texStateLogs;
		const res::path * name = (tex0 && !tex0->getFileName().empty()) ? &tex0->getFileName() : nullptr;
		LogInfo << "D3D12 tex state cutout=" << m_state.getAlphaCutout()
		        << " alphaRef=" << alphaRef
		        << " blend=" << int(blendSrc) << "," << int(blendDst)
		        << " hasAlpha=" << (tex0 && tex0->hasAlpha())
		        << " size=" << (tex0 ? tex0->getSize().x : 0) << "x" << (tex0 ? tex0->getSize().y : 0)
		        << " tex=" << (name ? name->string() : (tex0 ? "(unnamed)" : "(none)"));
	}
}

void D3D12Renderer::drawGpuVerts(Primitive primitive, const void * verts, size_t stride, size_t count) {
	if(!verts || count == 0 || !beginRecording()) {
		return;
	}
	const UINT bytes = UINT(stride * count);
	if(m->uploadOffset + bytes > kUploadBytes) {
		m->uploadOffset = 0;
	}
	const UINT aligned = (m->uploadOffset + 255u) & ~255u;
	if(aligned + bytes > kUploadBytes) {
		LogWarning << "D3D12: upload buffer full, dropping draw";
		return;
	}
	m->uploadOffset = aligned;
	std::memcpy(static_cast<char *>(m->uploadMapped) + m->uploadOffset, verts, bytes);
	bindDrawState(primitive);
	D3D12_VERTEX_BUFFER_VIEW vbv {};
	vbv.BufferLocation = m->upload->GetGPUVirtualAddress() + m->uploadOffset;
	vbv.SizeInBytes = bytes;
	vbv.StrideInBytes = UINT(stride);
	m->list->IASetVertexBuffers(0, 1, &vbv);
	switch(primitive) {
		case LineList: m->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST); break;
		case LineStrip: m->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP); break;
		default: m->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); break;
	}
	m->list->DrawInstanced(UINT(count), 1, 0, 0);
	m->uploadOffset += bytes;
}

void D3D12Renderer::drawTextured(Primitive primitive, const TexturedVertex * vertices, size_t count) {
	if(!vertices || count == 0) {
		return;
	}
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd };
	thread_local std::vector<GpuVert> converted;
	if(isTrianglePrimitive(primitive)) {
		clipTLTriangles(primitive, vertices, count, nullptr, 0, m_viewport, fog, converted);
		drawGpuVerts(Renderer::TriangleList, converted.data(), sizeof(GpuVert), converted.size());
		return;
	}
	convertTL(vertices, count, m_viewport, fog, converted);
	drawGpuVerts(primitive, converted.data(), sizeof(GpuVert), converted.size());
}

void D3D12Renderer::drawWorldVertices(Primitive primitive, const SMY_VERTEX * vertices, size_t count) {
	if(!vertices || count == 0) {
		return;
	}
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd };
	thread_local std::vector<GpuVert> converted;
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles(primitive, vertices, count, nullptr, 0, m_view, m_proj, m_viewport, fog, converted);
		drawGpuVerts(Renderer::TriangleList, converted.data(), sizeof(GpuVert), converted.size());
		return;
	}
}

void D3D12Renderer::drawWorldVertices(Primitive primitive, const SMY_VERTEX3 * vertices, size_t count) {
	if(!vertices || count == 0) {
		return;
	}
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd };
	thread_local std::vector<GpuVert> converted;
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles3(primitive, vertices, count, nullptr, 0, m_view, m_proj, m_viewport, fog, converted);
		drawGpuVerts(Renderer::TriangleList, converted.data(), sizeof(GpuVert), converted.size());
	}
}

void D3D12Renderer::drawWorldIndexed(Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                                     const unsigned short * indices, size_t nindices) {
	if(!vertices || !indices || nvertices == 0 || nindices == 0) {
		return;
	}
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd };
	thread_local std::vector<GpuVert> converted;
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles(primitive, vertices, nvertices, indices, nindices, m_view, m_proj,
		                   m_viewport, fog, converted);
		drawGpuVerts(Renderer::TriangleList, converted.data(), sizeof(GpuVert), converted.size());
	}
}

void D3D12Renderer::drawWorldIndexed(Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
                                     const unsigned short * indices, size_t nindices) {
	if(!vertices || !indices || nvertices == 0 || nindices == 0) {
		return;
	}
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd };
	thread_local std::vector<GpuVert> converted;
	if(isTrianglePrimitive(primitive)) {
		clipWorldTriangles3(primitive, vertices, nvertices, indices, nindices, m_view, m_proj,
		                    m_viewport, fog, converted);
		drawGpuVerts(Renderer::TriangleList, converted.data(), sizeof(GpuVert), converted.size());
	}
}

void D3D12Renderer::drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
                                unsigned short * indices, size_t nindices) {
	if(!vertices || !indices || nvertices == 0 || nindices == 0) {
		return;
	}
	const VertexFog fog{ m_state.getFog(), m_fogStart, m_fogEnd };
	thread_local std::vector<GpuVert> converted;
	if(isTrianglePrimitive(primitive)) {
		clipTLTriangles(primitive, vertices, nvertices, indices, nindices, m_viewport, fog, converted);
		drawGpuVerts(Renderer::TriangleList, converted.data(), sizeof(GpuVert), converted.size());
	}
}

std::unique_ptr<VertexBuffer<TexturedVertex>> D3D12Renderer::createVertexBufferTL(size_t capacity,
                                                                                BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<D3D12VertexBufferTL<TexturedVertex>>(this, capacity);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX>> D3D12Renderer::createVertexBuffer(size_t capacity,
                                                                           BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<D3D12VertexBufferWorld<SMY_VERTEX>>(this, capacity);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX3>> D3D12Renderer::createVertexBuffer3(size_t capacity,
                                                                             BufferUsage usage) {
	ARX_UNUSED(usage);
	return std::make_unique<D3D12VertexBufferWorld<SMY_VERTEX3>>(this, capacity);
}

bool D3D12Renderer::getSnapshot(Image & image) {
	ARX_UNUSED(image);
	return false;
}

bool D3D12Renderer::getSnapshot(Image & image, size_t width, size_t height) {
	ARX_UNUSED(image);
	ARX_UNUSED(width);
	ARX_UNUSED(height);
	return false;
}

void D3D12Renderer::showFrame() {
	if(!m->swapchain || !m->queue) {
		return;
	}
	if(!m->recording) {
		if(!beginRecording()) {
			return;
		}
	}
	if(!m->inPresentState && m->backbuffers[m->frame]) {
		transition(m->list.Get(), m->backbuffers[m->frame].Get(),
		           D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
		m->inPresentState = true;
	}
	if(FAILED(m->list->Close())) {
		LogError << "D3D12: Close command list failed";
		m->recording = false;
		return;
	}
	ID3D12CommandList * lists[] = { m->list.Get() };
	m->queue->ExecuteCommandLists(1, lists);
	m->swapchain->Present(0, 0);
	waitGpu();
	m->recording = false;
	m->frame = m->swapchain->GetCurrentBackBufferIndex();
}

#endif // ARX_HAVE_D3D12
