/*
 * Arx Raymix — Option A RTAO. World-space SMY_VERTEX → TLAS → AO → multiply.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/dxr/D3D12Rtao.h"

#if ARX_HAVE_D3D12

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>

#include <windows.h>

#include <glm/gtc/type_ptr.hpp>

#include "graphics/Math.h"
#include "math/Vector.h"
#include "io/log/Logger.h"
#include "platform/Platform.h"

namespace {

constexpr size_t kMaxDynTriangles = 80000;
constexpr size_t kMaxRoomTriangles = 120000;
constexpr size_t kMaxWaterTriangles = 20000;
constexpr size_t kMaxMetalTriangles = 30000;
constexpr UINT kIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
constexpr UINT kShaderRecord = 64;
// Heap: [0..14] RT SRVs t0-t14, [15..19] RT UAVs u0-u4,
// [20..27] composite SRVs (color, ao, shadow, depth, gi, spec, waterMask, metalMask).
constexpr UINT kRtSrvCount = 15;
constexpr UINT kRtUavBase = 15;
constexpr UINT kRtUavCount = 5;
constexpr UINT kCompositeBase = 20;
constexpr UINT kCompositeSrvCount = 8;
constexpr UINT kHeapCount = kCompositeBase + kCompositeSrvCount;
static_assert(kHeapCount == D3D12Rtao::kHeapDescriptors,
              "INV-02: the block the renderer reserves must match what this file lays out");
// Size of the Params (b0) constant buffer. It used to be a count of root constants; those cost a
// DWORD each and filled 60 of the root signature's 64, which is why every later value had to be
// smuggled into ViewParams (b1). It is a root CBV now, so the size is only a size.
constexpr UINT kParamsBytes = 240;
// A root CBV is read by the GPU when the dispatch runs, not copied into the command list when it
// is recorded, so one slot would be overwritten while an earlier frame still reads it. The
// renderer keeps kFrameCount frames in flight and waits only on the frame before that.
constexpr UINT kParamsSlots = 3;
constexpr UINT kParamsStride = 256; // SetComputeRootConstantBufferView requires a 256-byte address
static_assert(kRtUavBase == kRtSrvCount, "INV-02: UAV range must follow the RT SRVs");
static_assert(kCompositeBase == kRtUavBase + kRtUavCount, "INV-02: composite SRVs follow the UAVs");
constexpr UINT kMaskRtvWater = 0;
constexpr UINT kMaskRtvWaterDepth = 1;
constexpr UINT kMaskRtvMetal = 2;
// History blend for the current frame. Low 0.15 ≈ 14 frames to 90%; High 0.08 ≈ 28.
constexpr float kTemporalAlphaLow = 0.15f;
constexpr float kTemporalAlphaHigh = 0.08f;

constexpr int kMaxRtQuality = 3;
constexpr int kMaxShadowDenoise = 1;
constexpr int kMaxGiDenoise = 2;
static constexpr float aoRadius[] = { 0.f, 24.f, 32.f, 48.f };
static constexpr UINT aoRayCount[] = { 0u, 8u, 16u, 24u };
static constexpr UINT shadowRays[] = { 0u, 2u, 8u, 16u };
static constexpr UINT giRayCount[] = { 0u, 4u, 8u, 16u };
static constexpr UINT transRays[] = { 0u, 1u, 1u, 2u };
static constexpr UINT metalRayCount[] = { 0u, 1u, 2u, 2u };
// How reflective water looks head-on, per quality level. Physical water is 0.04, which reads as
// no reflection at all on an indoor pool seen from above, so the levels trade physical accuracy
// for a visible effect. Grazing angles still reach 1.0 at every level.
static constexpr float waterFacing[] = { 0.f, 0.18f, 0.30f, 0.45f };
// Brightness of the light-only guess used when a reflected hit is off screen and has no colour to
// sample. It carries no albedo, so it can only ever be a tinted blob; the shader now also fades
// the reflection down wherever this is all it had, and these numbers stay modest for the same
// reason. Raising them is what turned the missing data into a white shape sliding over the water.
static constexpr float reflectFill[] = { 0.f, 0.45f, 0.60f, 0.80f };
static constexpr float shadowAlpha[] = { kTemporalAlphaLow, kTemporalAlphaHigh };
static constexpr float giAlpha[] = { 0.20f, 0.12f, 0.08f };
static_assert(std::size(aoRadius) == kMaxRtQuality + 1);
static_assert(std::size(aoRayCount) == kMaxRtQuality + 1);
static_assert(std::size(shadowRays) == kMaxRtQuality + 1);
static_assert(std::size(giRayCount) == kMaxRtQuality + 1);
static_assert(std::size(transRays) == kMaxRtQuality + 1);
static_assert(std::size(metalRayCount) == kMaxRtQuality + 1);
static_assert(std::size(waterFacing) == kMaxRtQuality + 1);
static_assert(std::size(reflectFill) == kMaxRtQuality + 1);
static_assert(std::size(shadowAlpha) == kMaxShadowDenoise + 1);
static_assert(std::size(giAlpha) == kMaxGiDenoise + 1);
static_assert(aoRayCount[0] == 0u && shadowRays[0] == 0u && giRayCount[0] == 0u
              && transRays[0] == 0u && metalRayCount[0] == 0u && aoRadius[0] == 0.f,
              "Off (index 0) must launch no rays");

struct DxrConstants {
	float invViewProj[16];
	float cameraPos[3];
	float radius;
	UINT aoRays;
	float pixelWorld;
	UINT width;
	UINT height;
	UINT lightCount;
	UINT shadowsOn;
	UINT pad0;
	UINT pad1;
	float prevViewProj[16];
	UINT penumbraRays;
	UINT giOn;
	UINT giWidth;
	UINT giHeight;
	UINT giRays;
	float temporalAlpha;
	float projA;
	float projB;
	UINT specRays;
	UINT specHalfRes;
	UINT contactOn;
	UINT metalRays;
	float specAlpha;
	float contactTMax;
	float giTemporalAlpha;
	UINT playerVertBase;
};
static_assert(sizeof(DxrConstants) == kParamsBytes, "Params (b0) must match the HLSL cbuffer");
static_assert(sizeof(DxrConstants) % 16 == 0,
              "INV-01: the cbuffer has to end on a complete float4 row");
static_assert(offsetof(DxrConstants, invViewProj) == 0);
static_assert(offsetof(DxrConstants, cameraPos) == 64);
static_assert(offsetof(DxrConstants, prevViewProj) == 112,
              "prevViewProj must stay on a float4 boundary");
static_assert(offsetof(DxrConstants, penumbraRays) == 176);
static_assert(offsetof(DxrConstants, temporalAlpha) == 196);
static_assert(offsetof(DxrConstants, specRays) == 208);
static_assert(offsetof(DxrConstants, metalRays) == 220);
static_assert(offsetof(DxrConstants, playerVertBase) == 236);

struct DxrViewCbuf {
	float viewProj[16];
	float specTMax;
	float giTMax;
	float rtRange;
	float waterFacing;
	float waterFill;
	float metalFill;
	float pad0;
	float pad1;
};
static_assert(offsetof(DxrViewCbuf, viewProj) == 0);
static_assert(offsetof(DxrViewCbuf, specTMax) == 64);
static_assert(offsetof(DxrViewCbuf, waterFill) == 80,
              "ViewParams must stay float4-aligned to match the HLSL cbuffer");
static_assert(sizeof(DxrViewCbuf) == 96);

glm::mat4x4 jitteredProjection(const glm::mat4x4 & proj, float jitterNdcX, float jitterNdcY) {
	glm::mat4x4 jp = proj;
	for(int i = 0; i < 4; ++i) {
		jp[i][0] += jitterNdcX * jp[i][3];
		jp[i][1] += jitterNdcY * jp[i][3];
	}
	return jp;
}

constexpr UINT kLightFloat4s = 3;
static_assert(sizeof(D3D12Rtao::GpuLight) == kLightFloat4s * 16, "DXR light record is three float4s");

const char * kRayLib = R"(
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<float4> g_verts : register(t1);
StructuredBuffer<float4> g_lights : register(t2);
Texture2D<float> g_depth : register(t3);
Texture2D g_color : register(t4);
StructuredBuffer<float4> g_roomVerts : register(t5);
Texture2D<float> g_shadowPrev : register(t6);
Texture2D<float> g_aoPrev : register(t7);
Texture2D<float> g_depthPrev : register(t8);
Texture2D<float4> g_giPrev : register(t9);
Texture2D<float> g_waterMask : register(t10);
Texture2D<float> g_waterDepth : register(t11);
Texture2D<float4> g_specPrev : register(t12);
Texture2D<float> g_metalMask : register(t13);
StructuredBuffer<float4> g_waterVerts : register(t14);
RWTexture2D<float> g_ao : register(u0);
RWTexture2D<float> g_shadow : register(u1);
RWTexture2D<float4> g_gi : register(u2);
RWTexture2D<float> g_depthOut : register(u3);
RWTexture2D<float4> g_spec : register(u4);

// pad0/pad1 keep prevViewProj on a float4 boundary; the CPU struct mirrors this.
cbuffer Params : register(b0) {
	float4x4 invViewProj;
	float3 cameraPos;
	float radius;
	uint aoRays;
	float pixelWorld; // world units covered by one pixel at view depth 1
	uint width;
	uint height;
	uint lightCount;
	uint shadowsOn;
	uint pad0;
	uint pad1;
	float4x4 prevViewProj;
	uint penumbraRays;
	uint giOn;
	uint giWidth;
	uint giHeight;
	uint giRays;
	float temporalAlpha;
	float projA;
	float projB;
	uint specRays;
	uint specHalfRes;
	uint contactOn;
	uint metalRays;
	float specAlpha;
	float contactTMax;
	float giTemporalAlpha;
	uint playerVertBase;
};

cbuffer ViewParams : register(b1) {
	float4x4 viewProj;
	// These lived here because Params (b0) used to be 60 root constants and the signature was
	// full at 64 DWORDs. Params is a root CBV now and the cap is no longer the reason, but they
	// stay: this buffer is the natural home for per-view values, and moving them back would
	// churn two cbuffer layouts for nothing.
	float specTMax;
	float giTMax;
	float rtRange;
	// Reflection strength per quality level, so Off / Low / Medium / High actually look
	// different instead of only changing the ray count.
	float waterFacing;
	float waterFill;
	float metalFill;
	float padView0;
	float padView1;
};

// Cleared D24_UNORM depth is exactly 1.0 (INV-04). A value just below 1.0 is a
// finite world distance and used to skip AO / shadows past that plane.
static const float SKY_Z = 1.0;

struct RayPayload {
	float t;
	float3 n;
};

// Interleaved gradient noise: a per-pixel rotation that is fixed in screen space
// (no frame index), so the fixed sample sets dither spatially instead of banding.
// The composite's bilateral and the temporal history average it out.
float pixelRotation(uint2 p) {
	return 6.2831853 * frac(52.9829189 * frac(0.06711056 * float(p.x) + 0.00583715 * float(p.y)));
}

float2 rotate2(float2 p, float rot) {
	float cs = cos(rot);
	float sn = sin(rot);
	return float2(p.x * cs - p.y * sn, p.x * sn + p.y * cs);
}

float3 diskOffsetFixed(float3 dir, uint s, float rad, float rot) {
	float2 o[8] = {
		// Half-unit disk on purpose: rad is the light radius, so a full-unit ring would sample a
		// source twice the intended size and widen every penumbra to match. Thin casters — leaves,
		// bars, table legs — have a shadow no wider than the penumbra, so they wash out first.
		float2(0.00, 0.50), float2(0.43, 0.25), float2(0.43, -0.25), float2(0.00, -0.50),
		float2(-0.43, -0.25), float2(-0.43, 0.25), float2(0.22, 0.00), float2(-0.22, 0.00)
	};
	// Batches of 8: each further batch is rotated 22.5° and alternates a 0.72 ring
	// so 16 samples are 16 distinct disk points, not the same 8 twice.
	float2 p = rotate2(o[s & 7u], rot + float(s >> 3) * 0.3927) * ((s & 8u) ? 0.72 : 1.0);
	float3 t = normalize(abs(dir.z) < 0.999 ? cross(dir, float3(0, 0, 1)) : cross(dir, float3(1, 0, 0)));
	float3 b = cross(dir, t);
	return (t * p.x + b * p.y) * rad;
}

float3 hemisphereFixed(float3 n, uint s, float rot) {
	uint k = s & 7u;
	float z = sqrt(1.0 - (float(k) + 0.5) / 8.0);
	float r = sqrt(max(1.0 - z * z, 0.0));
	float a = float(k) * 2.399963229728653;
	float3 l = float3(cos(a) * r, sin(a) * r, z);
	l.xy = rotate2(l.xy, rot);
	float3 t = normalize(abs(n.z) < 0.999 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
	float3 b = cross(n, t);
	return normalize(t * l.x + b * l.y + n * l.z);
}

// Returns rgb plus a confidence in .a: 1 when the hit was resolved from the colour buffer,
// low when only the light-only fallback was available. The caller fades the reflection by that
// confidence, because an untextured milky blob where the geometry is off screen reads far worse
// than no reflection at all.
float4 shadeReflectionHit(float3 origin, float3 dir, float tmin, float tmax, uint mask,
                          float lightScale, float lightCap) {
	RayDesc rd;
	rd.Origin = origin;
	rd.Direction = dir;
	rd.TMin = tmin;
	rd.TMax = tmax;
	RayPayload rp;
	rp.t = 1e7;
	rp.n = float3(0, 0, 0);
	TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
	         mask, 0, 1, 0, rd, rp);
	if(rp.t >= tmax) {
		// Nothing within range: the reflection genuinely shows nothing here.
		return float4(0, 0, 0, 0);
	}
	float3 hit = rd.Origin + rd.Direction * rp.t;
	float3 hn = rp.n;
	if(dot(hn, hn) < 1e-6) {
		hn = -rd.Direction;
	}
	float4 hc = mul(viewProj, float4(hit, 1.0));
	float3 hitCol = float3(0, 0, 0);
	float resolved = 0.0;
	if(hc.w > 1.0) {
		float2 hu = float2(hc.x / hc.w * 0.5 + 0.5, 0.5 - hc.y / hc.w * 0.5);
		if(hu.x > 0.0 && hu.x < 1.0 && hu.y > 0.0 && hu.y < 1.0) {
			int2 hp = int2(hu * float2(width, height));
			float hz = g_depth.Load(int3(hp, 0)).r;
			if(hz > 0.0 && hz < SKY_Z) {
				float hw = projB / min(hz - projA, -1e-4);
				if(abs(hw - hc.w) < max(0.04 * hc.w, 8.0)) {
					hitCol = g_color.Load(int3(hp, 0)).rgb;
					resolved = 1.0;
				}
			}
		}
	}
	if(dot(hitCol, hitCol) < 1e-6) {
		for(uint i = 0; i < lightCount; ++i) {
			float4 a = g_lights[i * 3 + 0];
			float4 b = g_lights[i * 3 + 1];
			float4 col = g_lights[i * 3 + 2];
			float3 toL = a.xyz - hit;
			float d = length(toL);
			if(d < 1.0) {
				continue;
			}
			float ndotl = saturate(dot(hn, toL / d));
			float span = max(b.y - b.x, 1e-3);
			float fall = saturate((b.y - d) / span);
			hitCol += col.rgb * min(a.w * fall * ndotl * b.w * lightScale, lightCap);
		}
		// kFallbackTrust: how much of a reflection to draw when all we have is a light-only
		// guess with no surface colour. Keep it low; this is the term that used to paint a
		// white shape that slid across the water as the camera turned.
		const float kFallbackTrust = 0.30;
		return float4(hitCol, kFallbackTrust);
	}
	return float4(hitCol, resolved);
}


[shader("raygeneration")]
void RayGen() {
	uint2 pixel = DispatchRaysIndex().xy;
	if(pixel.x >= width || pixel.y >= height) {
		return;
	}
	float z = g_depth.Load(int3(pixel, 0)).r;
	if(z <= 0.0 || z >= SKY_Z) {
		g_ao[pixel] = 1.0;
		g_shadow[pixel] = 1.0;
		g_depthOut[pixel] = z;
		g_spec[pixel] = float4(0, 0, 0, 0);
		if((pixel.x & 1u) == 0u && (pixel.y & 1u) == 0u) {
			uint2 gp = pixel / 2;
			if(gp.x < giWidth && gp.y < giHeight) {
				g_gi[gp] = float4(0, 0, 0, 0);
			}
		}
		return;
	}
	float2 uv = (float2(pixel) + 0.5) / float2(width, height);
	float ndcX = uv.x * 2.0 - 1.0;
	float ndcY = 1.0 - uv.y * 2.0;
	float4 worldH = mul(invViewProj, float4(ndcX, ndcY, z, 1.0));
	float3 pos = worldH.xyz / max(worldH.w, 1e-6);
	float wC = projB / (z - projA);
	if(rtRange > 8.0 && wC > rtRange) {
		g_ao[pixel] = 1.0;
		g_shadow[pixel] = 1.0;
		g_depthOut[pixel] = z;
		g_spec[pixel] = float4(0, 0, 0, 0);
		if((pixel.x & 1u) == 0u && (pixel.y & 1u) == 0u) {
			uint2 gp = pixel / 2;
			if(gp.x < giWidth && gp.y < giHeight) {
				g_gi[gp] = float4(0, 0, 0, 0);
			}
		}
		return;
	}
	// Normal from depth. Per axis, take whichever neighbour is closer in linear
	// depth (so a silhouette on one side does not skew the normal), and call the
	// pixel a discontinuity when even that step is far bigger than a surface at a
	// steep grazing angle could produce (measured in pixel footprints, not raw z:
	// the old 0.04 raw-z test never fired, so decal / wall and bar / wall edges
	// got normals tilted up to ~80 degrees and their rays started inside geometry).
	float footprint = max(wC * pixelWorld, 0.05);
	float zR = g_depth.Load(int3(int(pixel.x) + 1, int(pixel.y), 0)).r;
	float zL = g_depth.Load(int3(max(int(pixel.x) - 1, 0), int(pixel.y), 0)).r;
	float zD = g_depth.Load(int3(int(pixel.x), int(pixel.y) + 1, 0)).r;
	float zU = g_depth.Load(int3(int(pixel.x), max(int(pixel.y) - 1, 0), 0)).r;
	float dR = (zR <= 0.0 || zR >= SKY_Z) ? 1e9 : abs(projB / (zR - projA) - wC);
	float dL = (zL <= 0.0 || zL >= SKY_Z) ? 1e9 : abs(projB / (zL - projA) - wC);
	float dD = (zD <= 0.0 || zD >= SKY_Z) ? 1e9 : abs(projB / (zD - projA) - wC);
	float dU = (zU <= 0.0 || zU >= SKY_Z) ? 1e9 : abs(projB / (zU - projA) - wC);
	const bool useL = dL < dR;
	const bool useU = dU < dD;
	const float zH = useL ? zL : zR;
	const float zV = useU ? zU : zD;
	const float dxH = useL ? -1.0 : 1.0;
	const float dyV = useU ? -1.0 : 1.0;
	const float dH = useL ? dL : dR;
	const float dV = useU ? dU : dD;
	const float discTol = max(4.0 * footprint, 1.0);
	const bool disc = (dH > discTol || dV > discTol);
	float3 n;
	if(disc) {
		n = normalize(cameraPos - pos);
	} else {
		float2 uvH = (float2(pixel) + float2(0.5 + dxH, 0.5)) / float2(width, height);
		float2 uvV = (float2(pixel) + float2(0.5, 0.5 + dyV)) / float2(width, height);
		float4 wH = mul(invViewProj, float4(uvH.x * 2.0 - 1.0, 1.0 - uvH.y * 2.0, zH, 1.0));
		float4 wV = mul(invViewProj, float4(uvV.x * 2.0 - 1.0, 1.0 - uvV.y * 2.0, zV, 1.0));
		n = normalize(cross(wH.xyz / max(wH.w, 1e-6) - pos, wV.xyz / max(wV.w, 1e-6) - pos));
		if(dot(n, cameraPos - pos) < 0.0) {
			n = -n;
		}
	}
	// Ray origins step off the surface by a bias that grows with the pixel
	// footprint, and TMin skips the receiver's own thickness (bars, decals).
	const float aoBias = 8.0 + 2.0 * footprint;
	const float shBias = 4.0 + 2.0 * footprint;
	const float rot = pixelRotation(pixel);
	float aoCur = 1.0;
	if(aoRays == 0) {
		aoCur = 1.0;
	} else {
		// Discontinuity pixels trace too (with the camera-facing normal): forcing
		// AO = 1 there left bright specks on bars, legs and every silhouette.
		float occ = 0.0;
		for(uint i = 0; i < aoRays; ++i) {
			RayDesc ao;
			ao.Origin = pos + n * aoBias;
			ao.Direction = hemisphereFixed(n, i, rot + float(i >> 3) * 0.2618);
			ao.TMin = aoBias;
			ao.TMax = radius;
			RayPayload aop;
			aop.t = radius + 1.0;
			aop.n = 0.xxx;
			TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
			         0x01, 0, 1, 0, ao, aop);
			if(aop.t >= 8.0 && aop.t < radius) {
				occ += 1.0 - aop.t / radius;
			}
		}
		// The ray set above is a proper cosine hemisphere (14° to 75° off the normal), not the
		// 25° cone this used to sample. That is the correct distribution, but inside a closed
		// stone room almost every pixel then occludes past the 0.45 floor, so ambient occlusion
		// turns into a flat darkening with no gradient — and a flat darkening is exactly what
		// stops ray traced shadows from reading. Scale the occlusion so the floor is reached
		// only by geometry that really is enclosed, and the gradient survives.
		const float kAoStrength = 0.6;
		aoCur = max(1.0 - occ / float(aoRays) * kAoStrength, 0.45);
	}

	float shCur = 1.0;
	if(shadowsOn == 0 || lightCount == 0) {
		shCur = 1.0;
	} else {
		float sumVis = 0.0;
		float sumAttn = 0.0;
		uint samples = min(max(penumbraRays, 1u), 16u);
		for(uint i = 0; i < lightCount; ++i) {
			float4 a = g_lights[i * 3 + 0];
			float4 b = g_lights[i * 3 + 1];
			float3 lpos = a.xyz;
			float intensity = a.w;
			float fallstart = b.x;
			float fallend = b.y;
			float lrad = b.z;
			float presence = b.w;
			float3 toL = lpos - pos;
			float d = length(toL);
			if(d < 1e-3) {
				continue;
			}
			float3 ldir = toL / d;
			float ndotl = saturate(dot(n, ldir));
			// Shadows reach further than the raster falloff, and the light selector in
			// D3D12Renderer::fillShadowLights ranks candidates by this same fallend * 1.35.
			// Drop the factor here and the selector keeps handing shadow slots to lights this
			// loop then evaluates as attn == 0, which removes their shadow entirely.
			float shadowEnd = fallend * 1.35;
			float span = max(shadowEnd - fallstart, 1e-3);
			float fall = saturate((shadowEnd - d) / span);
			float attn = intensity * fall * ndotl * presence;
			if(attn <= 0.0) {
				continue;
			}
			float3 origin = pos + n * shBias;
			float vis = 0.0;
			for(uint s = 0; s < samples; ++s) {
				float3 samplePos = (samples == 1u) ? lpos : (lpos + diskOffsetFixed(ldir, s, lrad, rot));
				float3 toRay = samplePos - origin;
				float tmax = length(toRay);
				RayDesc sh;
				sh.Origin = origin;
				sh.Direction = toRay / max(tmax, 1e-3);
				sh.TMin = shBias;
				sh.TMax = max(tmax, shBias + 0.1);
				RayPayload shp;
				shp.t = 1e7;
				shp.n = 0.xxx;
				TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
				         0x01, 0, 1, 0, sh, shp);
				vis += (shp.t >= tmax) ? 1.0 : 0.0;
			}
			if(contactOn != 0u && disc) {
				RayDesc cs;
				cs.Origin = origin;
				cs.Direction = ldir;
				cs.TMin = shBias;
				cs.TMax = max(contactTMax, shBias + 1.0);
				RayPayload cp;
				cp.t = 1e7;
				cp.n = float3(0, 0, 0);
				TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
				         0x01, 0, 1, 0, cs, cp);
				if(cp.t < cs.TMax) {
					vis *= 0.35;
				}
			}
			sumVis += (vis / float(samples)) * attn;
			sumAttn += attn;
		}
		// Ambient floor: only the direct share of the light is shadowed. Without
		// it a blocked, weak, far light dragged an already dark room down to the
		// full umbra even though the raster light there was mostly ambient.
		// 0.12 against a typical torch attn of 0.8-1.6 keeps that umbra nearly
		// full; 0.3 washed it out to half strength.
		shCur = (sumVis + 0.04) / (sumAttn + 0.04);
	}

	// Temporal history. The player camera never rests (view_attach follows the
	// idle animation), so a single frame's hard shadow edge flickers; blend with
	// last frame's value at the reprojected pixel when its depth matches.
	float aoOut = aoCur;
	float shOut = shCur;
	bool histOk = false;
	int2 histPix = int2(0, 0);
	if(temporalAlpha > 0.0) {
		float4 pc = mul(prevViewProj, float4(pos, 1.0));
		if(pc.w > 1.0) {
			float2 pn = pc.xy / pc.w;
			float2 puv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
			// Bilinear 2x2 history fetch with per-tap depth validation. A nearest
			// fetch jittered by half a pixel every frame while walking, and a flat
			// depth tolerance rejected the history of floors seen at a grazing
			// angle (one pixel over differs by more than a few percent of w), so
			// those pixels fell back to the raw dithered value and sparkled.
			float2 pf = puv * float2(width, height) - 0.5;
			int2 p0 = int2(floor(pf));
			float2 fr = pf - float2(p0);
			// Discontinuity pixels keep a generous tolerance instead of none: a pixel
			// with no history shows the raw dithered value, which sparkles.
			float slope = disc ? discTol : max(dH, dV);
			float tol = max(0.02 * pc.w, 2.0) + 2.0 * slope;
			float wsum = 0.0;
			float shH = 0.0;
			float aoH = 0.0;
			[unroll] for(int j = 0; j < 2; ++j) {
				[unroll] for(int i = 0; i < 2; ++i) {
					int2 pp = p0 + int2(i, j);
					if(pp.x < 0 || pp.y < 0 || pp.x >= int(width) || pp.y >= int(height)) {
						continue;
					}
					float zp = g_depthPrev.Load(int3(pp, 0));
					if(zp <= 0.0 || zp >= SKY_Z) {
						continue;
					}
					float wPrev = projB / (zp - projA);
					if(abs(wPrev - pc.w) >= tol) {
						continue;
					}
					float wgt = ((i == 0) ? (1.0 - fr.x) : fr.x) * ((j == 0) ? (1.0 - fr.y) : fr.y);
					shH += g_shadowPrev.Load(int3(pp, 0)) * wgt;
					aoH += g_aoPrev.Load(int3(pp, 0)) * wgt;
					wsum += wgt;
				}
			}
			if(wsum <= 0.05) {
				// Fallback: nearest tap with twice the tolerance, still better than raw.
				int2 pn2 = int2(round(pf));
				if(pn2.x >= 0 && pn2.y >= 0 && pn2.x < int(width) && pn2.y < int(height)) {
					float zp = g_depthPrev.Load(int3(pn2, 0));
					if(zp > 0.0 && zp < SKY_Z && abs(projB / (zp - projA) - pc.w) < tol * 2.0) {
						shH = g_shadowPrev.Load(int3(pn2, 0));
						aoH = g_aoPrev.Load(int3(pn2, 0));
						wsum = 1.0;
					}
				}
			}
			if(wsum > 0.05) {
				shOut = lerp(shH / wsum, shCur, temporalAlpha);
				aoOut = lerp(aoH / wsum, aoCur, temporalAlpha);
				histOk = true;
				histPix = int2(round(pf));
			}
		}
	}
	g_ao[pixel] = aoOut;
	g_shadow[pixel] = shOut;
	g_depthOut[pixel] = z;

	float3 specRgb = float3(0, 0, 0);
	float specF = 0.0;
	// Metal reflects from the primary surface, so the shared history reprojection above is right
	// for it. Water does not: its reflection is computed on the water plane while pos, and every
	// pixel derived from it, is the pool bottom seen through the water. Reprojecting the water
	// reflection by the bottom's position drags the history sideways by the depth between the two
	// every time the camera turns, and at specAlpha 0.10 that error survives dozens of frames —
	// the smear that reads as ghosting. Water overwrites these below with its own reprojection.
	bool specHistOk = histOk;
	int2 specHistPix = histPix;
	const bool doSpec = (specRays + metalRays) > 0u
		&& (specHalfRes == 0u || ((pixel.x | pixel.y) & 1u) == 0u);
	if(doSpec) {
		float wm = g_waterMask.Load(int3(pixel, 0)).r;
		float mm = g_metalMask.Load(int3(pixel, 0)).r;
		if(wm > 0.5 && specRays > 0u) {
			float zw = g_waterDepth.Load(int3(pixel, 0)).r;
			// Water quads span rooms. Raster water is depth-tested; the mask
			// must be too. A leftover 1 behind a wall would reflect from the
			// plane (grazing F≈1 → a bright water strip on the pillar).
			if(zw > 0.0 && zw < SKY_Z && z + 0.001 < zw) {
				wm = 0.0;
			}
			if(wm > 0.5 && zw > 0.0 && zw < SKY_Z) {
				float4 wposH = mul(invViewProj, float4(ndcX, ndcY, zw, 1.0));
				float3 wpos = wposH.xyz / max(wposH.w, 1e-6);
				// Reproject the reflection history by the water plane itself, not by the bottom.
				specHistOk = false;
				float4 wpc = mul(prevViewProj, float4(wpos, 1.0));
				if(wpc.w > 1.0) {
					float2 wpn = wpc.xy / wpc.w;
					float2 wuv = float2(wpn.x * 0.5 + 0.5, 0.5 - wpn.y * 0.5);
					if(wuv.x > 0.0 && wuv.x < 1.0 && wuv.y > 0.0 && wuv.y < 1.0) {
						int2 wp = int2(wuv * float2(width, height));
						// Only reuse history from a pixel that was also water at a matching
						// depth; otherwise the reflection inherits the shore or the bottom.
						float zwPrev = g_waterDepth.Load(int3(wp, 0)).r;
						if(zwPrev > 0.0 && zwPrev < SKY_Z
						   && abs(projB / min(zwPrev - projA, -1e-4) - wpc.w) < max(0.05 * wpc.w, 12.0)) {
							specHistPix = wp;
							specHistOk = true;
						}
					}
				}
				float zWR = g_waterDepth.Load(int3(int(pixel.x) + 1, int(pixel.y), 0)).r;
				float zWD = g_waterDepth.Load(int3(int(pixel.x), int(pixel.y) + 1, 0)).r;
				float3 nW = n;
				if(zWR > 0.0 && zWR < SKY_Z && zWD > 0.0 && zWD < SKY_Z) {
					float2 uvHR = (float2(pixel) + float2(1.5, 0.5)) / float2(width, height);
					float2 uvVR = (float2(pixel) + float2(0.5, 1.5)) / float2(width, height);
					float4 wHR = mul(invViewProj, float4(uvHR.x * 2.0 - 1.0, 1.0 - uvHR.y * 2.0, zWR, 1.0));
					float4 wVR = mul(invViewProj, float4(uvVR.x * 2.0 - 1.0, 1.0 - uvVR.y * 2.0, zWD, 1.0));
					nW = normalize(cross(wHR.xyz / max(wHR.w, 1e-6) - wpos,
					                     wVR.xyz / max(wVR.w, 1e-6) - wpos));
					if(dot(nW, cameraPos - wpos) < 0.0) {
						nW = -nW;
					}
				}
				float3 V = normalize(cameraPos - wpos);
				float ndv = saturate(dot(nW, V));
				// Physically water reflects about 4 % head-on, which on an indoor pool seen from
				// above reads as no reflection at all — the whole effect only showed at grazing
				// angles. Keep the Fresnel curve, but lift its head-on end so the reflection is
				// visible from the angle players actually look at water. waterFacing comes from
				// the Transparent reflections setting: 0.04 would be physical, 1.0 a mirror.
				float F0 = 0.04;
				float fresnel = F0 + (1.0 - F0) * pow(1.0 - ndv, 5.0);
				specF = saturate(lerp(waterFacing, 1.0, fresnel));
				uint nSpec = min(max(specRays, 1u), 2u);
				float4 acc = float4(0, 0, 0, 0);
				for(uint s = 0; s < nSpec; ++s) {
					float3 R = reflect(-V, nW);
					if(s > 0u) {
						R = normalize(R + hemisphereFixed(nW, s, rot) * 0.04);
					}
					acc += shadeReflectionHit(wpos + nW * 6.0, R, 6.0, specTMax, 0x05,
					                          0.35 * waterFill, 0.6 * waterFill);
				}
				acc /= float(nSpec);
				specRgb = acc.rgb;
				// Fade the whole reflection by how much of it was real data.
				specF *= acc.a;
			}
		} else if(mm > 0.5 && metalRays > 0u) {
			float3 V = normalize(cameraPos - pos);
			float ndv = saturate(dot(n, V));
			// Dirty iron, not chrome. 0.56 replaced the plate albedo with 2-ray
			// static (the elevator rope face).
			float F0 = 0.18;
			specF = F0 + (1.0 - F0) * pow(1.0 - ndv, 5.0);
			uint nSpec = min(max(metalRays, 1u), 2u);
			float4 acc = float4(0, 0, 0, 0);
			for(uint s = 0; s < nSpec; ++s) {
				float3 R = reflect(-V, n);
				R = normalize(R + hemisphereFixed(n, s, rot) * 0.06);
				acc += shadeReflectionHit(pos + n * shBias, R, shBias, specTMax * 0.625, 0x07,
					                          0.30 * metalFill, 0.55 * metalFill);
			}
			acc /= float(nSpec);
			specRgb = acc.rgb;
			specF *= acc.a;
		}
		if(specF > 0.0 && specAlpha > 0.0 && specHistOk) {
			int2 hp = specHalfRes ? (specHistPix & int2(~1, ~1)) : specHistPix;
			float4 prevS = g_specPrev.Load(int3(hp, 0));
			if(prevS.a >= 1e-4) {
				specRgb = lerp(prevS.rgb, specRgb, specAlpha);
				specF = lerp(prevS.a, specF, specAlpha);
			}
		}
	}
	g_spec[pixel] = float4(specRgb, specF);

	if((pixel.x & 1u) == 0u && (pixel.y & 1u) == 0u) {
		uint2 gp = pixel / 2;
		if(gp.x >= giWidth || gp.y >= giHeight) {
			return;
		}
		if(giOn == 0) {
			g_gi[gp] = float4(0, 0, 0, 0);
			return;
		}
		float3 gi = float3(0, 0, 0);
		uint nGi = max(min(giRays, 16u), 1u);
		for(uint h = 0; h < nGi; ++h) {
			RayDesc bounce;
			bounce.Origin = pos + n * aoBias;
			bounce.Direction = hemisphereFixed(n, h, rot + float(h >> 3) * 0.2618);
			bounce.TMin = aoBias;
			bounce.TMax = giTMax;
			RayPayload bp;
			bp.t = giTMax + 1.0;
			bp.n = float3(0, 0, 0);
			TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
			         0x01, 0, 1, 0, bounce, bp);
			if(bp.t < 8.0 || bp.t >= giTMax) {
				continue;
			}
			float3 hit = bounce.Origin + bounce.Direction * bp.t;
			float3 hn = bp.n;
			if(dot(hn, hn) < 1e-6) {
				hn = -bounce.Direction;
			}
			float3 fill = float3(0, 0, 0);
			for(uint i = 0; i < lightCount; ++i) {
				float4 a = g_lights[i * 3 + 0];
				float4 b = g_lights[i * 3 + 1];
				float4 col = g_lights[i * 3 + 2];
				float3 toL = a.xyz - hit;
				float d = max(length(toL), 1.0);
				// A bounce point sitting almost on a torch reports a near-maximum fill and floods
				// the umbra with that torch's colour. Bounce light is what escapes the direct
				// falloff, not a second copy of it — and this sum is added before the shadow
				// multiply, so anything spilled here lands squarely in the shadow. Ramp the
				// contribution in over the first 40 units instead of trusting the falloff at
				// point-blank range.
				float nearFade = saturate((d - 8.0) / 32.0);
				if(nearFade <= 0.0) {
					continue;
				}
				float ndotl = saturate(dot(hn, toL / d));
				float span = max(b.y - b.x, 1e-3);
				float fall = saturate((b.y - d) / span);
				// Bounce carries the light's hue; grey bounce read as a white haze
				// on the ceiling above the torch.
				fill += col.rgb * min(a.w * fall * ndotl * b.w * 0.20, 0.25) * nearFade;
			}
			// Per-ray clamp, then average over rays *launched*: dividing by the
			// rays that hit let one lucky ray next to a lamp light the whole texel
			// (the white GI sparks).
			float fl = dot(fill, float3(0.30, 0.59, 0.11));
			gi += fill * (min(fl, 0.3) / max(fl, 1e-4));
		}
		gi /= float(nGi);
		if(histOk) {
			gi = lerp(g_giPrev.Load(int3(histPix / 2, 0)).rgb, gi, giTemporalAlpha);
		}
		g_gi[gp] = float4(gi, 1.0);
	}
}

[shader("closesthit")]
void ClosestHit(inout RayPayload p, BuiltInTriangleIntersectionAttributes /* attr */) {
	p.t = RayTCurrent();
	uint prim = PrimitiveIndex();
	float3 v0, v1, v2;
	if(InstanceID() == 0) {
		v0 = g_roomVerts[prim * 3 + 0].xyz;
		v1 = g_roomVerts[prim * 3 + 1].xyz;
		v2 = g_roomVerts[prim * 3 + 2].xyz;
	} else if(InstanceID() == 2) {
		v0 = g_waterVerts[prim * 3 + 0].xyz;
		v1 = g_waterVerts[prim * 3 + 1].xyz;
		v2 = g_waterVerts[prim * 3 + 2].xyz;
	} else {
		uint base = (InstanceID() == 3) ? playerVertBase : 0u;
		v0 = g_verts[base + prim * 3 + 0].xyz;
		v1 = g_verts[base + prim * 3 + 1].xyz;
		v2 = g_verts[base + prim * 3 + 2].xyz;
	}
	float3 n = normalize(cross(v1 - v0, v2 - v0));
	if(dot(n, -WorldRayDirection()) < 0.0) {
		n = -n;
	}
	p.n = n;
}

[shader("miss")]
void Miss(inout RayPayload p) {
	p.t = 1e7;
	p.n = 0.xxx;
}
)";

const char * kComposite = R"(
Texture2D colorTex : register(t0);
Texture2D aoTex : register(t1);
Texture2D shadowTex : register(t2);
Texture2D<float> depthTex : register(t3);
Texture2D giTex : register(t4);
Texture2D specTex : register(t5);
Texture2D<float> waterMaskTex : register(t6);
Texture2D<float> metalMaskTex : register(t7);
SamplerState samp : register(s0);
cbuffer CompParams : register(b0) {
	int shadowRadius;
	int aoRadius;
	int specHalf;
	int padC;
	float projA;
	float projB;
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	float2 uv = float2((id << 1) & 2, id & 2);
	VSOut o;
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
float bilateral(Texture2D tex, int2 pix, int radius, float zCenter) {
	if(radius <= 0) {
		return tex.Load(int3(pix, 0)).r;
	}
	float s = 0.0;
	float wsum = 0.0;
	const float wCenter = projB / min(zCenter - projA, -1e-4);
	const float zTol = max(0.02 * wCenter, 2.0);
	const float r2 = float(radius * radius);
	for(int y = -radius; y <= radius; ++y) {
		for(int x = -radius; x <= radius; ++x) {
			int2 p = pix + int2(x, y);
			float z = depthTex.Load(int3(p, 0)).r;
			if(z <= 0.0 || z >= 0.99999) {
				continue;
			}
			float wLin = projB / min(z - projA, -1e-4);
			float dz = abs(wLin - wCenter);
			if(dz >= zTol) {
				continue;
			}
			float spatial = 1.0 - float(x * x + y * y) / (r2 * 2.0 + 1.0);
			float w = (1.0 - dz / zTol) * spatial;
			s += tex.Load(int3(p, 0)).r * w;
			wsum += w;
		}
	}
	if(wsum < 1e-5) {
		return tex.Load(int3(pix, 0)).r;
	}
	return s / wsum;
}
float3 giUpsample(int2 pix, float zCenter) {
	int2 gp = pix / 2;
	float3 s = float3(0, 0, 0);
	float wsum = 0.0;
	[unroll] for(int y = -1; y <= 1; ++y) {
		[unroll] for(int x = -1; x <= 1; ++x) {
			int2 p = gp + int2(x, y);
			int2 full = p * 2;
			float z = depthTex.Load(int3(full, 0)).r;
			if(abs(z - zCenter) >= 0.002) {
				continue;
			}
			float3 g = giTex.Load(int3(p, 0)).rgb;
			s += g;
			wsum += 1.0;
		}
	}
	if(wsum < 1e-5) {
		return float3(0, 0, 0);
	}
	return s / wsum;
}
float4 PSMain(VSOut i) : SV_Target {
	float3 c = colorTex.SampleLevel(samp, i.uv, 0).rgb;
	int2 pix = int2(i.pos.xy);
	float zCenter = depthTex.Load(int3(pix, 0)).r;
	// Radius 3: the old 13x13 window erased bar shadows a few pixels wide once
	// the camera stepped back; per-pixel sample rotation plus temporal history
	// now handle the smoothing a wide blur used to do.
	// Strength tuned by the user ("triplica os efeitos"): AO up to 55 % dark,
	// umbra keeps 10 % of the light, GI up to about +0.33 on lit stone.
	// AO is low frequency: a wider blur than the shadow's hides the dither.
	// Second dial for ambient occlusion, on the presentation side: 1.0 applies the buffer as
	// traced, 0.0 removes it entirely. Kept at 1.0 because the strength is set where the rays are
	// accumulated; turn it down here to weaken ambient occlusion without touching the history.
	const float kAoComposite = 1.0;
	float ao = lerp(1.0, bilateral(aoTex, pix, aoRadius, zCenter), kAoComposite);
	float sh = lerp(0.10, 1.0, bilateral(shadowTex, pix, shadowRadius, zCenter));
	// Bounce light scaled by the surface's own raster color (plus a small floor
	// for the darkest stone) so it reads as light on the material, not grey haze.
	// The lit raster colour stands in for albedo, so cap it (a surface already
	// blown out by a spell light must not bounce even brighter) and add in
	// screen fashion so the sum never saturates to white.
	float3 bounce = giUpsample(pix, zCenter) * (0.15 + min(c, 0.45)) * 2.0;
	c = c + bounce * (1.0 - c);
	float water = waterMaskTex.Load(int3(pix, 0)).r;
	float metal = metalMaskTex.Load(int3(pix, 0)).r;
	float4 specS = specTex.SampleLevel(samp, i.uv, 0);
	if(specHalf != 0 && specS.a < 1e-4) {
		specS = specTex.Load(int3(pix.x & ~1, pix.y & ~1, 0));
	}
	// Water keeps the raster enviro and is not multiplied by the floor's ao * sh.
	if(water > 0.5) {
		return float4(saturate(lerp(colorTex.SampleLevel(samp, i.uv, 0).rgb, specS.rgb, specS.a)), 1);
	}
	c = saturate(c) * max(ao * sh, 0.08);
	if(metal > 0.5) {
		// Coat, not a replace: the elevator iron must keep its raster texture.
		c = saturate(c * (1.0 - specS.a * 0.35) + specS.rgb * specS.a * 0.45);
	}
	return float4(saturate(c), 1);
}
)";

const char * kMask = R"(
cbuffer MaskParams : register(b0) {
	float4x4 viewProj;
	float maskValue;
	float jitterX;
	float jitterY;
	float pad2;
};
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(float4 p : POSITION) {
	VSOut o;
	o.pos = mul(viewProj, float4(p.xyz, 1));
	o.pos.xy += float2(jitterX, jitterY) * o.pos.w;
	return o;
}
float PSWater(VSOut i) : SV_Target {
	return maskValue;
}
float PSWaterZ(VSOut i) : SV_Target {
	return i.pos.z;
}
float PSMetal(VSOut i) : SV_Target {
	return maskValue;
}
)";

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

void uavBarrier(ID3D12GraphicsCommandList * list, ID3D12Resource * res) {
	D3D12_RESOURCE_BARRIER b {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	b.UAV.pResource = res;
	list->ResourceBarrier(1, &b);
}

bool createBuffer(ID3D12Device * device, UINT64 bytes, D3D12_HEAP_TYPE heap,
                  D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                  ID3D12Resource ** out) {
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	D3D12_HEAP_PROPERTIES hp {};
	hp.Type = heap;
	return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
	                                                 state, nullptr, IID_PPV_ARGS(out)));
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
		if(i >= nindices || indices[i] >= nverts) {
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

struct DxcCache {
	HMODULE compiler = nullptr;
	HMODULE dxil = nullptr;
};

DxcCache & dxcCache() {
	static DxcCache cache;
	return cache;
}

void retainDxil(HMODULE handle) {
	if(!handle) {
		return;
	}
	DxcCache & cache = dxcCache();
	if(!cache.dxil) {
		cache.dxil = handle;
	} else if(handle != cache.dxil) {
		FreeLibrary(handle);
	}
}

void releaseDxcompiler() {
	DxcCache & cache = dxcCache();
	if(cache.compiler) {
		FreeLibrary(cache.compiler);
		cache.compiler = nullptr;
	}
}

void loadDxilBeside(const std::wstring & dxcompilerPath) {
	const size_t slash = dxcompilerPath.find_last_of(L"\\/");
	if(slash == std::wstring::npos) {
		return;
	}
	const std::wstring dxil = dxcompilerPath.substr(0, slash + 1) + L"dxil.dll";
	retainDxil(LoadLibraryExW(dxil.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
}

HMODULE tryLoadDxcompilerFile(const std::wstring & path) {
	if(path.empty() || path.find_first_of(L"\\/") == std::wstring::npos) {
		return nullptr;
	}
	loadDxilBeside(path);
	return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

HMODULE loadDxcompiler() {
	DxcCache & cache = dxcCache();
	if(cache.compiler) {
		return cache.compiler;
	}
	wchar_t exe[MAX_PATH] {};
	if(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
		std::wstring dir(exe);
		const size_t slash = dir.find_last_of(L"\\/");
		if(slash != std::wstring::npos) {
			if(HMODULE lib = tryLoadDxcompilerFile(dir.substr(0, slash + 1) + L"dxcompiler.dll")) {
				cache.compiler = lib;
				return lib;
			}
		}
	}
	
	wchar_t env[MAX_PATH] {};
	if(GetEnvironmentVariableW(L"WindowsSdkVerBinPath", env, MAX_PATH) > 0) {
		std::wstring path(env);
		if(!path.empty() && path.back() != L'\\' && path.back() != L'/') {
			path += L'\\';
		}
		if(HMODULE lib = tryLoadDxcompilerFile(path + L"x64\\dxcompiler.dll")) {
			cache.compiler = lib;
			return lib;
		}
	}
	
	WIN32_FIND_DATAW fd {};
	HANDLE find = FindFirstFileW(L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\*", &fd);
	if(find != INVALID_HANDLE_VALUE) {
		std::wstring newest;
		do {
			if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				continue;
			}
			if(fd.cFileName[0] == L'.') {
				continue;
			}
			std::wstring candidate = std::wstring(L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\")
				+ fd.cFileName + L"\\x64\\dxcompiler.dll";
			if(GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
				newest = candidate;
			}
		} while(FindNextFileW(find, &fd));
		FindClose(find);
		if(HMODULE lib = tryLoadDxcompilerFile(newest)) {
			cache.compiler = lib;
			return lib;
		}
	}
	return nullptr;
}

} // namespace

D3D12Rtao::D3D12Rtao() = default;

D3D12Rtao::~D3D12Rtao() {
	shutdown();
}

void D3D12Rtao::shutdown() {
	m_ready = false;
	m_supported = false;
	m_ao.reset();
	m_shadow.reset();
	m_gi.reset();
	m_colorCopy.reset();
	m_aoPrev.reset();
	m_shadowPrev.reset();
	m_depthCur.reset();
	m_depthPrev.reset();
	m_giPrev.reset();
	m_spec.reset();
	m_specPrev.reset();
	m_waterMask.reset();
	m_waterDepth.reset();
	m_metalMask.reset();
	m_histValid = false;
	m_lights.reset();
	if(m_paramsCbuf && m_paramsMapped) {
		m_paramsCbuf->Unmap(0, nullptr);
	}
	m_paramsMapped = nullptr;
	m_paramsCbuf.reset();
	m_viewCbuf.reset();
	m_shaderTable.reset();
	m_instances.reset();
	m_scratch.reset();
	m_tlas.reset();
	m_blas.reset();
	m_vertDefault.reset();
	m_vertUpload.reset();
	m_roomDefault.reset();
	m_roomUpload.reset();
	m_waterDefault.reset();
	m_waterUpload.reset();
	m_metalUpload.reset();
	m_roomMetalUpload.reset();
	m_roomBlas.reset();
	m_waterBlas.reset();
	m_playerBlas.reset();
	m_heap = nullptr; // not ours to release
	m_rtaoBase = 0;
	m_rtvHeap.reset();
	m_dsvHeap.reset();
	m_compositePso.reset();
	m_waterMaskPso.reset();
	m_waterDepthPso.reset();
	m_metalMaskPso.reset();
	m_rtState.reset();
	m_compositeRoot.reset();
	m_maskRoot.reset();
	m_rtRoot.reset();
	m_dxil.clear();
	m_device5.reset();
	m_device = nullptr;
	m_positions.clear();
	m_roomPositions.clear();
	m_waterPositions.clear();
	m_metalPositions.clear();
	m_roomMetalPositions.clear();
	m_roomsDirty = true;
	m_colorIsShader = false;
	m_maskIsSrv = false;
	m_targetsFailed = false;
	m_failedW = 0;
	m_failedH = 0;
}

bool D3D12Rtao::init(ID3D12Device * device, ID3D12DescriptorHeap * sharedHeap,
                     unsigned baseIndex) {
	shutdown();
	m_device = device;
	if(!device) {
		LogInfo << "RaytracingTier=NOT_SUPPORTED";
		return false;
	}
	
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt {};
	if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt, sizeof(opt)))
	   || opt.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
		LogInfo << "RaytracingTier=NOT_SUPPORTED";
		return true;
	}
	LogInfo << "RaytracingTier=" << int(opt.RaytracingTier);
	
	if(FAILED(device->QueryInterface(IID_PPV_ARGS(m_device5.put())))) {
		LogInfo << "RaytracingTier=NOT_SUPPORTED (Device5 QI failed)";
		return true;
	}
	
	m_supported = true;
	m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	
	if(!compileRayLib() || !createPipeline()) {
		LogError << "RTAO: DXR pipeline failed — raster only";
		m_ready = false;
		return true;
	}
	if(!createMaskPipeline()) {
		LogWarning << "RTAO: mask pipeline failed — reflections off, AO/shadows/GI still on";
	}
	
	// The heap belongs to the renderer. A ray dispatch and a raster draw cannot bind different
	// CBV_SRV_UAV heaps, and the hit shader has to reach the game's textures, so this module
	// works inside a reserved block of the renderer's heap instead of owning one.
	if(!sharedHeap) {
		LogError << "RTAO: no shared descriptor heap";
		return true;
	}
	m_heap = sharedHeap;
	m_rtaoBase = baseIndex;
	if(!createBuffer(device, UINT64(kMaxShadowLights * sizeof(GpuLight)), D3D12_HEAP_TYPE_UPLOAD,
	                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, m_lights.put())) {
		LogError << "RTAO: light buffer failed";
		return true;
	}
	if(!createBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
	                 D3D12_RESOURCE_FLAG_NONE, m_viewCbuf.put())) {
		LogError << "RTAO: view cbuffer failed";
		return true;
	}
	if(!createBuffer(device, UINT64(kParamsSlots) * kParamsStride, D3D12_HEAP_TYPE_UPLOAD,
	                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
	                 m_paramsCbuf.put())) {
		LogError << "RTAO: params cbuffer failed";
		return true;
	}
	// Upload heaps may stay mapped for their whole life, so map once here rather than around
	// every dispatch.
	if(FAILED(m_paramsCbuf->Map(0, nullptr, &m_paramsMapped)) || !m_paramsMapped) {
		LogError << "RTAO: params cbuffer map failed";
		m_paramsMapped = nullptr;
		return true;
	}
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeap {};
	rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeap.NumDescriptors = 3;
	if(FAILED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(m_rtvHeap.put())))) {
		LogError << "RTAO: mask RTV heap failed";
		return true;
	}
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeap {};
	dsvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeap.NumDescriptors = 1;
	if(FAILED(device->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(m_dsvHeap.put())))) {
		LogError << "RTAO: mask DSV heap failed";
		return true;
	}
	
	m_ready = true;
	LogInfo << "RTAO ready";
	return true;
}

bool D3D12Rtao::compileRayLib() {
	HMODULE dxc = loadDxcompiler();
	if(!dxc) {
		LogError << "RTAO: dxcompiler.dll not found";
		return false;
	}
	struct CompilerGuard {
		~CompilerGuard() { releaseDxcompiler(); }
	} unloadCompiler;
	using DxcCreateInstanceFn = HRESULT (WINAPI *)(REFCLSID, REFIID, LPVOID *);
	auto create = reinterpret_cast<DxcCreateInstanceFn>(GetProcAddress(dxc, "DxcCreateInstance"));
	if(!create) {
		LogError << "RTAO: DxcCreateInstance missing";
		return false;
	}
	
	IDxcLibrary * library = nullptr;
	IDxcCompiler * compiler = nullptr;
	if(FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) || !library) {
		LogError << "RTAO: IDxcLibrary failed";
		return false;
	}
	if(FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler) {
		library->Release();
		LogError << "RTAO: IDxcCompiler failed";
		return false;
	}
	
	IDxcBlobEncoding * source = nullptr;
	const HRESULT blobHr = library->CreateBlobWithEncodingFromPinned(
		kRayLib, UINT(std::strlen(kRayLib)), CP_UTF8, &source);
	if(FAILED(blobHr) || !source) {
		compiler->Release();
		library->Release();
		LogError << "RTAO: source blob failed";
		return false;
	}
	
	IDxcOperationResult * result = nullptr;
	const HRESULT compileHr = compiler->Compile(source, L"rtao.hlsl", L"", L"lib_6_3",
	                                            nullptr, 0, nullptr, 0, nullptr, &result);
	source->Release();
	compiler->Release();
	library->Release();
	if(FAILED(compileHr) || !result) {
		LogError << "RTAO: dxc Compile failed";
		return false;
	}
	
	HRESULT status = E_FAIL;
	result->GetStatus(&status);
	if(FAILED(status)) {
		IDxcBlobEncoding * errors = nullptr;
		result->GetErrorBuffer(&errors);
		if(errors && errors->GetBufferPointer()) {
			LogError << "RTAO: " << static_cast<const char *>(errors->GetBufferPointer());
		} else {
			LogError << "RTAO: lib_6_3 compile failed";
		}
		if(errors) {
			errors->Release();
		}
		result->Release();
		return false;
	}
	
	IDxcBlob * dxil = nullptr;
	if(FAILED(result->GetResult(&dxil)) || !dxil) {
		result->Release();
		return false;
	}
	result->Release();
	const size_t n = dxil->GetBufferSize();
	m_dxil.resize(n);
	if(n) {
		std::memcpy(m_dxil.data(), dxil->GetBufferPointer(), n);
	}
	dxil->Release();
	return true;
}

bool D3D12Rtao::createPipeline() {
	D3D12_DESCRIPTOR_RANGE srv {};
	srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv.NumDescriptors = kRtSrvCount;
	srv.BaseShaderRegister = 0;
	D3D12_DESCRIPTOR_RANGE uav {};
	uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uav.NumDescriptors = kRtUavCount;
	uav.BaseShaderRegister = 0;
	D3D12_ROOT_PARAMETER params[4] {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srv;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &uav;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[3].Descriptor.ShaderRegister = 1;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rs {};
	rs.NumParameters = 4;
	rs.pParameters = params;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> err;
	if(FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put()))) {
		if(err && err->GetBufferPointer()) {
			LogError << "RTAO: RT root signature failed — "
			         << static_cast<const char *>(err->GetBufferPointer());
		} else {
			LogError << "RTAO: RT root signature failed";
		}
		return false;
	}
	if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                        IID_PPV_ARGS(m_rtRoot.put())))) {
		LogError << "RTAO: CreateRootSignature failed";
		return false;
	}
	
	D3D12_EXPORT_DESC exports[3] {};
	exports[0].Name = L"RayGen";
	exports[1].Name = L"ClosestHit";
	exports[2].Name = L"Miss";
	D3D12_DXIL_LIBRARY_DESC lib {};
	lib.DXILLibrary.pShaderBytecode = m_dxil.data();
	lib.DXILLibrary.BytecodeLength = m_dxil.size();
	lib.NumExports = 3;
	lib.pExports = exports;
	D3D12_HIT_GROUP_DESC hit {};
	hit.HitGroupExport = L"HitGroup";
	hit.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
	hit.ClosestHitShaderImport = L"ClosestHit";
	D3D12_RAYTRACING_SHADER_CONFIG sc {};
	sc.MaxPayloadSizeInBytes = 32;
	sc.MaxAttributeSizeInBytes = 8;
	D3D12_RAYTRACING_PIPELINE_CONFIG pc {};
	pc.MaxTraceRecursionDepth = 2;
	D3D12_GLOBAL_ROOT_SIGNATURE grs {};
	grs.pGlobalRootSignature = m_rtRoot.Get();
	D3D12_STATE_SUBOBJECT subs[5] {};
	subs[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib };
	subs[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit };
	subs[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc };
	subs[3] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc };
	subs[4] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs };
	D3D12_STATE_OBJECT_DESC sod {};
	sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	sod.NumSubobjects = 5;
	sod.pSubobjects = subs;
	if(FAILED(m_device5->CreateStateObject(&sod, IID_PPV_ARGS(m_rtState.put())))) {
		LogError << "RTAO: CreateStateObject failed";
		return false;
	}
	
	ID3D12StateObjectProperties * props = nullptr;
	if(FAILED(m_rtState->QueryInterface(IID_PPV_ARGS(&props))) || !props) {
		return false;
	}
	if(!createBuffer(m_device, kShaderRecord * 3, D3D12_HEAP_TYPE_UPLOAD,
	                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
	                 m_shaderTable.put())) {
		props->Release();
		return false;
	}
	char * table = nullptr;
	if(FAILED(m_shaderTable->Map(0, nullptr, reinterpret_cast<void **>(&table))) || !table) {
		props->Release();
		return false;
	}
	std::memset(table, 0, kShaderRecord * 3);
	std::memcpy(table + 0, props->GetShaderIdentifier(L"RayGen"), kIdentifierSize);
	std::memcpy(table + kShaderRecord, props->GetShaderIdentifier(L"Miss"), kIdentifierSize);
	std::memcpy(table + kShaderRecord * 2, props->GetShaderIdentifier(L"HitGroup"), kIdentifierSize);
	m_shaderTable->Unmap(0, nullptr);
	props->Release();
	
	D3D12_DESCRIPTOR_RANGE colorSrv {};
	colorSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	colorSrv.NumDescriptors = kCompositeSrvCount;
	colorSrv.BaseShaderRegister = 0;
	D3D12_ROOT_PARAMETER cparams[2] {};
	cparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	cparams[0].Constants.Num32BitValues = 6;
	cparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	cparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	cparams[1].DescriptorTable.NumDescriptorRanges = 1;
	cparams[1].DescriptorTable.pDescriptorRanges = &colorSrv;
	cparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC samp {};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC crs {};
	crs.NumParameters = 2;
	crs.pParameters = cparams;
	crs.NumStaticSamplers = 1;
	crs.pStaticSamplers = &samp;
	blob.reset();
	err.reset();
	if(FAILED(D3D12SerializeRootSignature(&crs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put()))) {
		return false;
	}
	if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                        IID_PPV_ARGS(m_compositeRoot.put())))) {
		return false;
	}
	
	ComPtr<ID3DBlob> vs;
	ComPtr<ID3DBlob> ps;
	ComPtr<ID3DBlob> cerr;
	if(FAILED(D3DCompile(kComposite, std::strlen(kComposite), "rtao_composite", nullptr, nullptr,
	                     "VSMain", "vs_5_0", 0, 0, vs.put(), cerr.put()))) {
		LogError << "RTAO: composite VS failed"
		         << (cerr && cerr->GetBufferPointer()
		             ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
		return false;
	}
	cerr.reset();
	if(FAILED(D3DCompile(kComposite, std::strlen(kComposite), "rtao_composite", nullptr, nullptr,
	                     "PSMain", "ps_5_0", 0, 0, ps.put(), cerr.put()))) {
		LogError << "RTAO: composite PS failed"
		         << (cerr && cerr->GetBufferPointer()
		             ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
		return false;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
	pd.pRootSignature = m_compositeRoot.Get();
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.DepthStencilState.DepthEnable = FALSE;
	pd.SampleMask = 0xffffffff;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.SampleDesc.Count = 1;
	if(FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(m_compositePso.put())))) {
		LogError << "RTAO: composite PSO failed";
		return false;
	}
	return true;
}

bool D3D12Rtao::createMaskPipeline() {
	D3D12_ROOT_PARAMETER mp {};
	mp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	mp.Constants.Num32BitValues = 20; // float4x4 + maskValue + jitter + pad
	mp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC mrs {};
	mrs.NumParameters = 1;
	mrs.pParameters = &mp;
	mrs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> err;
	if(FAILED(D3D12SerializeRootSignature(&mrs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put()))) {
		LogError << "RTAO: mask root signature failed";
		return false;
	}
	if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                        IID_PPV_ARGS(m_maskRoot.put())))) {
		return false;
	}
	ComPtr<ID3DBlob> vs;
	ComPtr<ID3DBlob> psW;
	ComPtr<ID3DBlob> psZ;
	ComPtr<ID3DBlob> psM;
	ComPtr<ID3DBlob> cerr;
	auto compile = [&](const char * entry, ComPtr<ID3DBlob> & out, const char * fail) {
		cerr.reset();
		if(FAILED(D3DCompile(kMask, std::strlen(kMask), "rtao_mask", nullptr, nullptr,
		                     entry, "ps_5_0", 0, 0, out.put(), cerr.put()))) {
			LogError << fail
			         << (cerr && cerr->GetBufferPointer()
			             ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
			return false;
		}
		return true;
	};
	if(FAILED(D3DCompile(kMask, std::strlen(kMask), "rtao_mask", nullptr, nullptr,
	                     "VSMain", "vs_5_0", 0, 0, vs.put(), cerr.put()))) {
		LogError << "RTAO: mask VS failed"
		         << (cerr && cerr->GetBufferPointer()
		             ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
		return false;
	}
	if(!compile("PSWater", psW, "RTAO: water mask PS failed")
	   || !compile("PSWaterZ", psZ, "RTAO: water depth PS failed")
	   || !compile("PSMetal", psM, "RTAO: metal mask PS failed")) {
		return false;
	}
	D3D12_INPUT_ELEMENT_DESC elem {};
	elem.SemanticName = "POSITION";
	elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	elem.InputSlot = 0;
	elem.AlignedByteOffset = 0;
	elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
	elem.InstanceDataStepRate = 0;
	auto fill = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC & pd, ID3DBlob * ps, DXGI_FORMAT format) {
		pd = {};
		pd.pRootSignature = m_maskRoot.Get();
		pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		// R8 / R32 reject COLOR_WRITE_ENABLE_ALL (0xf).
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		// Same pull-forward as RenderWater's depthOffset(8): water sits on the
		// floor that wrote the scene depth and must still mark those pixels.
		pd.RasterizerState.DepthBias = -8;
		pd.RasterizerState.SlopeScaledDepthBias = -8.f;
		pd.DepthStencilState.DepthEnable = TRUE;
		pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		pd.SampleMask = 0xffffffff;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = format;
		pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		pd.SampleDesc.Count = 1;
		pd.InputLayout.pInputElementDescs = &elem;
		pd.InputLayout.NumElements = 1;
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC water {};
	fill(water, psW.Get(), DXGI_FORMAT_R8_UNORM);
	HRESULT hrW = m_device->CreateGraphicsPipelineState(&water, IID_PPV_ARGS(m_waterMaskPso.put()));
	if(FAILED(hrW)) {
		LogError << "RTAO: water mask PSO failed (hr=" << long(hrW) << ")";
		return false;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC waterZ {};
	fill(waterZ, psZ.Get(), DXGI_FORMAT_R32_FLOAT);
	HRESULT hrZ = m_device->CreateGraphicsPipelineState(&waterZ, IID_PPV_ARGS(m_waterDepthPso.put()));
	if(FAILED(hrZ)) {
		LogError << "RTAO: water depth PSO failed (hr=" << long(hrZ) << ")";
		return false;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC metal {};
	fill(metal, psM.Get(), DXGI_FORMAT_R8_UNORM);
	HRESULT hrM = m_device->CreateGraphicsPipelineState(&metal, IID_PPV_ARGS(m_metalMaskPso.put()));
	if(FAILED(hrM)) {
		LogError << "RTAO: metal mask PSO failed (hr=" << long(hrM) << ")";
		return false;
	}
	LogInfo << "RTAO: mask pipeline ready";
	return true;
}

void D3D12Rtao::releaseTargets() {
	m_ao.reset();
	m_shadow.reset();
	m_gi.reset();
	m_colorCopy.reset();
	m_aoPrev.reset();
	m_shadowPrev.reset();
	m_depthCur.reset();
	m_depthPrev.reset();
	m_giPrev.reset();
	m_spec.reset();
	m_specPrev.reset();
	m_waterMask.reset();
	m_waterDepth.reset();
	m_metalMask.reset();
	m_histValid = false;
	m_maskIsSrv = false;
	m_width = 0;
	m_height = 0;
}

bool D3D12Rtao::needsResize(int width, int height) const {
	if(m_targetsFailed && width == m_failedW && height == m_failedH) {
		return false;
	}
	return !(width == m_width && height == m_height && m_ao && m_shadow && m_gi && m_depthPrev
	         && m_spec && m_waterMask && m_metalMask);
}

void D3D12Rtao::resize(int width, int height) {
	if(!needsResize(width, height)) {
		return;
	}
	m_targetsFailed = false;
	releaseTargets();
	if(!m_device || width <= 0 || height <= 0) {
		return;
	}
	
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC ao {};
	ao.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	ao.Width = UINT(width);
	ao.Height = UINT(height);
	ao.DepthOrArraySize = 1;
	ao.MipLevels = 1;
	ao.Format = DXGI_FORMAT_R8_UNORM;
	ao.SampleDesc.Count = 1;
	ao.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	auto failAlloc = [&]() {
		releaseTargets();
		m_targetsFailed = true;
		m_failedW = width;
		m_failedH = height;
	};
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ao,
	                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
	                                            IID_PPV_ARGS(m_ao.put())))) {
		LogError << "RTAO: AO target failed";
		failAlloc();
		return;
	}
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ao,
	                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
	                                            IID_PPV_ARGS(m_shadow.put())))) {
		LogError << "RTAO: shadow target failed";
		failAlloc();
		return;
	}
	D3D12_RESOURCE_DESC gi {};
	gi.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	gi.Width = UINT((std::max)(width / 2, 1));
	gi.Height = UINT((std::max)(height / 2, 1));
	gi.DepthOrArraySize = 1;
	gi.MipLevels = 1;
	gi.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	gi.SampleDesc.Count = 1;
	gi.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &gi,
	                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
	                                            IID_PPV_ARGS(m_gi.put())))) {
		LogError << "RTAO: GI target failed";
		failAlloc();
		return;
	}
	D3D12_RESOURCE_DESC color {};
	color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	color.Width = UINT(width);
	color.Height = UINT(height);
	color.DepthOrArraySize = 1;
	color.MipLevels = 1;
	color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	color.SampleDesc.Count = 1;
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &color,
	                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	                                            IID_PPV_ARGS(m_colorCopy.put())))) {
		LogError << "RTAO: color copy failed";
		failAlloc();
		return;
	}
	// Temporal history targets. History textures start as copy destinations that
	// the RT pass reads; m_depthCur is the raw depth RayGen writes each frame.
	auto makeTex = [&](const D3D12_RESOURCE_DESC & base, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
	                   D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> & out) {
		D3D12_RESOURCE_DESC d = base;
		d.Format = format;
		d.Flags = flags;
		return SUCCEEDED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state,
		                                                   nullptr, IID_PPV_ARGS(out.put())));
	};
	const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	if(!makeTex(ao, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, srvState, m_aoPrev)
	   || !makeTex(ao, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, srvState, m_shadowPrev)
	   || !makeTex(ao, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_depthCur)
	   || !makeTex(ao, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, srvState, m_depthPrev)
	   || !makeTex(gi, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srvState, m_giPrev)
	   || !makeTex(color, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_spec)
	   || !makeTex(color, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srvState, m_specPrev)) {
		LogError << "RTAO: history targets failed";
		failAlloc();
		return;
	}
	D3D12_RESOURCE_DESC mask = ao;
	mask.Format = DXGI_FORMAT_R8_UNORM;
	mask.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE maskClear {};
	maskClear.Format = DXGI_FORMAT_R8_UNORM;
	D3D12_RESOURCE_DESC wdepth = ao;
	wdepth.Format = DXGI_FORMAT_R32_FLOAT;
	wdepth.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE depthClear {};
	depthClear.Format = DXGI_FORMAT_R32_FLOAT;
	depthClear.Color[0] = 1.f;
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &mask,
	                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &maskClear,
	                                            IID_PPV_ARGS(m_waterMask.put())))
	   || FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &wdepth,
	                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &depthClear,
	                                               IID_PPV_ARGS(m_waterDepth.put())))
	   || FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &mask,
	                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &maskClear,
	                                               IID_PPV_ARGS(m_metalMask.put())))) {
		LogError << "RTAO: reflection mask targets failed";
		failAlloc();
		return;
	}
	if(m_rtvHeap && m_device) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		D3D12_RENDER_TARGET_VIEW_DESC r8 {};
		r8.Format = DXGI_FORMAT_R8_UNORM;
		r8.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		D3D12_RENDER_TARGET_VIEW_DESC r32 {};
		r32.Format = DXGI_FORMAT_R32_FLOAT;
		r32.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		m_device->CreateRenderTargetView(m_waterMask.Get(), &r8, rtv);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = rtv;
		rtv1.ptr += m_rtvSize;
		m_device->CreateRenderTargetView(m_waterDepth.Get(), &r32, rtv1);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv2 = rtv;
		rtv2.ptr += SIZE_T(2) * m_rtvSize;
		m_device->CreateRenderTargetView(m_metalMask.Get(), &r8, rtv2);
	}
	m_maskIsSrv = false;
	m_masksRasterized = false;
	m_width = width;
	m_height = height;
	m_aoIsUav = true;
	m_colorIsShader = false;
	m_histValid = false;
	if(m_heap) {
		updateDescriptors();
	}
}

bool D3D12Rtao::ensureTargets(int width, int height) {
	if(m_targetsFailed && width == m_failedW && height == m_failedH) {
		return false;
	}
	if(width != m_width || height != m_height || !m_ao) {
		resize(width, height);
	}
	return m_ao && m_shadow && m_gi && m_colorCopy
	       && m_aoPrev && m_shadowPrev && m_depthCur && m_depthPrev && m_giPrev
	       && m_spec && m_specPrev && m_waterMask && m_waterDepth && m_metalMask;
}

void D3D12Rtao::updateDescriptors() {
	if(!m_device || !m_heap) {
		return;
	}
	// Every slot(i) below is relative to this module's reserved block, so the base is folded in
	// here once instead of at each of the twenty call sites.
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += SIZE_T(m_rtaoBase) * m_descriptorSize;
	auto slot = [&](UINT i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
		h.ptr += SIZE_T(i) * m_descriptorSize;
		return h;
	};
	D3D12_SHADER_RESOURCE_VIEW_DESC nullBuf {};
	nullBuf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	nullBuf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	nullBuf.Format = DXGI_FORMAT_UNKNOWN;
	nullBuf.Buffer.FirstElement = 0;
	nullBuf.Buffer.NumElements = 1;
	nullBuf.Buffer.StructureByteStride = 16;
	for(UINT i = 0; i < kRtSrvCount; ++i) {
		m_device->CreateShaderResourceView(nullptr, &nullBuf, slot(i));
	}
	if(m_tlas) {
		D3D12_SHADER_RESOURCE_VIEW_DESC tlas {};
		tlas.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
		tlas.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		tlas.RaytracingAccelerationStructure.Location = m_tlas->GetGPUVirtualAddress();
		m_device->CreateShaderResourceView(nullptr, &tlas, slot(0));
	}
	auto bindVerts = [&](ID3D12Resource * res, size_t n, UINT slotIndex) {
		if(!res || n == 0) {
			return;
		}
		D3D12_SHADER_RESOURCE_VIEW_DESC vb {};
		vb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		vb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		vb.Buffer.FirstElement = 0;
		vb.Buffer.NumElements = UINT(n);
		vb.Buffer.StructureByteStride = sizeof(Pos);
		m_device->CreateShaderResourceView(res, &vb, slot(slotIndex));
	};
	if(m_vertDefault && !m_positions.empty()) {
		bindVerts(m_vertDefault.Get(), m_positions.size(), 1);
	}
	if(m_roomDefault && !m_roomPositions.empty()) {
		bindVerts(m_roomDefault.Get(), m_roomPositions.size(), 5);
	}
	if(m_waterDefault && !m_waterPositions.empty()) {
		bindVerts(m_waterDefault.Get(), m_waterPositions.size(), 14);
	}
	if(m_lights) {
		D3D12_SHADER_RESOURCE_VIEW_DESC lb {};
		lb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		lb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		lb.Buffer.FirstElement = 0;
		lb.Buffer.NumElements = UINT(kMaxShadowLights * kLightFloat4s);
		lb.Buffer.StructureByteStride = sizeof(float) * 4;
		m_device->CreateShaderResourceView(m_lights.Get(), &lb, slot(2));
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav8 {};
	uav8.Format = DXGI_FORMAT_R8_UNORM;
	uav8.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_SHADER_RESOURCE_VIEW_DESC factorSrv {};
	factorSrv.Format = DXGI_FORMAT_R8_UNORM;
	factorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	factorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	factorSrv.Texture2D.MipLevels = 1;
	if(m_ao) {
		m_device->CreateUnorderedAccessView(m_ao.Get(), nullptr, &uav8, slot(kRtUavBase + 0));
		m_device->CreateShaderResourceView(m_ao.Get(), &factorSrv, slot(kCompositeBase + 1));
	}
	if(m_shadow) {
		m_device->CreateUnorderedAccessView(m_shadow.Get(), nullptr, &uav8, slot(kRtUavBase + 1));
		m_device->CreateShaderResourceView(m_shadow.Get(), &factorSrv, slot(kCompositeBase + 2));
	}
	if(m_shadowPrev) {
		m_device->CreateShaderResourceView(m_shadowPrev.Get(), &factorSrv, slot(6));
	}
	if(m_aoPrev) {
		m_device->CreateShaderResourceView(m_aoPrev.Get(), &factorSrv, slot(7));
	}
	if(m_depthPrev) {
		D3D12_SHADER_RESOURCE_VIEW_DESC dSrv {};
		dSrv.Format = DXGI_FORMAT_R32_FLOAT;
		dSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		dSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		dSrv.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(m_depthPrev.Get(), &dSrv, slot(8));
	}
	if(m_depthCur) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC dUav {};
		dUav.Format = DXGI_FORMAT_R32_FLOAT;
		dUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		m_device->CreateUnorderedAccessView(m_depthCur.Get(), nullptr, &dUav, slot(kRtUavBase + 3));
	}
	if(m_gi) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavGi {};
		uavGi.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		uavGi.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		D3D12_SHADER_RESOURCE_VIEW_DESC giSrv {};
		giSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		giSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		giSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		giSrv.Texture2D.MipLevels = 1;
		m_device->CreateUnorderedAccessView(m_gi.Get(), nullptr, &uavGi, slot(kRtUavBase + 2));
		m_device->CreateShaderResourceView(m_gi.Get(), &giSrv, slot(kCompositeBase + 4));
		if(m_giPrev) {
			m_device->CreateShaderResourceView(m_giPrev.Get(), &giSrv, slot(9));
		}
	}
	if(m_spec) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavS {};
		uavS.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		uavS.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		D3D12_SHADER_RESOURCE_VIEW_DESC specSrv {};
		specSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		specSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		specSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		specSrv.Texture2D.MipLevels = 1;
		m_device->CreateUnorderedAccessView(m_spec.Get(), nullptr, &uavS, slot(kRtUavBase + 4));
		m_device->CreateShaderResourceView(m_spec.Get(), &specSrv, slot(kCompositeBase + 5));
		if(m_specPrev) {
			m_device->CreateShaderResourceView(m_specPrev.Get(), &specSrv, slot(12));
		}
	}
	if(m_waterMask) {
		m_device->CreateShaderResourceView(m_waterMask.Get(), &factorSrv, slot(10));
		m_device->CreateShaderResourceView(m_waterMask.Get(), &factorSrv, slot(kCompositeBase + 6));
	}
	if(m_waterDepth) {
		D3D12_SHADER_RESOURCE_VIEW_DESC dSrv {};
		dSrv.Format = DXGI_FORMAT_R32_FLOAT;
		dSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		dSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		dSrv.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(m_waterDepth.Get(), &dSrv, slot(11));
	}
	if(m_metalMask) {
		m_device->CreateShaderResourceView(m_metalMask.Get(), &factorSrv, slot(13));
		m_device->CreateShaderResourceView(m_metalMask.Get(), &factorSrv, slot(kCompositeBase + 7));
	}
	if(m_colorCopy) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(m_colorCopy.Get(), &srv, slot(4));
		m_device->CreateShaderResourceView(m_colorCopy.Get(), &srv, slot(kCompositeBase + 0));
	}
}

void D3D12Rtao::beginWorldFrame() {
	m_positions.clear();
	m_waterPositions.clear();
	m_metalPositions.clear();
	m_reflectOnlyStart = SIZE_MAX;
	m_masksRasterized = false;
}

void D3D12Rtao::markReflectOnlyStart() {
	m_reflectOnlyStart = m_positions.size();
}

void D3D12Rtao::clearRooms() {
	m_roomPositions.clear();
	m_roomMetalPositions.clear();
	m_roomsDirty = true;
}

template <typename Vertex>
void D3D12Rtao::addTris(std::vector<Pos> & dst, size_t cap, Renderer::Primitive primitive,
                        const Vertex * vertices, size_t nvertices,
                        const unsigned short * indices, size_t nindices) {
	if(!m_supported || !vertices || nvertices == 0) {
		return;
	}
	const size_t n = indices ? nindices : nvertices;
	forEachTriangleIndices(primitive, n, [&](size_t i0, size_t i1, size_t i2) {
		if(dst.size() / 3 >= cap) {
			if(!m_loggedCap) {
				LogInfo << "DXR: triangle cap " << cap;
				m_loggedCap = true;
			}
			return;
		}
		size_t a = 0, b = 0, c = 0;
		if(!resolveIndex(indices, nindices, nvertices, i0, a)
		   || !resolveIndex(indices, nindices, nvertices, i1, b)
		   || !resolveIndex(indices, nindices, nvertices, i2, c)) {
			return;
		}
		const Vec3f & pa = vertices[a].p;
		const Vec3f & pb = vertices[b].p;
		const Vec3f & pc = vertices[c].p;
		const Vec3f e0 = pb - pa;
		const Vec3f e1 = pc - pa;
		const Vec3f nrm = glm::cross(e0, e1);
		if(glm::dot(nrm, nrm) < 1e-4f) {
			return;
		}
		dst.push_back({ pa.x, pa.y, pa.z, 1.f });
		dst.push_back({ pb.x, pb.y, pb.z, 1.f });
		dst.push_back({ pc.x, pc.y, pc.z, 1.f });
	});
}

void D3D12Rtao::addRoom(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                        const unsigned short * indices, size_t nindices) {
	addTris(m_roomPositions, kMaxRoomTriangles, primitive, vertices, nvertices, indices, nindices);
	m_roomsDirty = true;
}

void D3D12Rtao::addWorld(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                         const unsigned short * indices, size_t nindices) {
	addTris(m_positions, kMaxDynTriangles, primitive, vertices, nvertices, indices, nindices);
}

void D3D12Rtao::addPosTri(std::vector<Pos> & dst, size_t cap, const Vec3f & a, const Vec3f & b, const Vec3f & c) {
	if(dst.size() / 3 >= cap) {
		if(!m_loggedCap) {
			LogInfo << "DXR: triangle cap " << cap;
			m_loggedCap = true;
		}
		return;
	}
	const Vec3f nrm = glm::cross(b - a, c - a);
	if(glm::dot(nrm, nrm) < 1e-4f) {
		return;
	}
	dst.push_back({ a.x, a.y, a.z, 1.f });
	dst.push_back({ b.x, b.y, b.z, 1.f });
	dst.push_back({ c.x, c.y, c.z, 1.f });
}

void D3D12Rtao::addWater(const Vec3f & a, const Vec3f & b, const Vec3f & c) {
	if(!m_supported) {
		return;
	}
	addPosTri(m_waterPositions, kMaxWaterTriangles, a, b, c);
}

void D3D12Rtao::addMetal(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                         const unsigned short * indices, size_t nindices) {
	addTris(m_metalPositions, kMaxMetalTriangles, primitive, vertices, nvertices, indices, nindices);
}

void D3D12Rtao::addRoomMetal(Renderer::Primitive primitive, const SMY_VERTEX * vertices, size_t nvertices,
                             const unsigned short * indices, size_t nindices) {
	addTris(m_roomMetalPositions, kMaxMetalTriangles, primitive, vertices, nvertices, indices, nindices);
}

void D3D12Rtao::addWorld(Renderer::Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
                         const unsigned short * indices, size_t nindices) {
	addTris(m_positions, kMaxDynTriangles, primitive, vertices, nvertices, indices, nindices);
}

template void D3D12Rtao::addTris<SMY_VERTEX>(std::vector<Pos> &, size_t, Renderer::Primitive,
                                             const SMY_VERTEX *, size_t, const unsigned short *, size_t);
template void D3D12Rtao::addTris<SMY_VERTEX3>(std::vector<Pos> &, size_t, Renderer::Primitive,
                                              const SMY_VERTEX3 *, size_t, const unsigned short *, size_t);

bool D3D12Rtao::ensureGeometryBuffers(ID3D12GraphicsCommandList * list) {
	if(!m_device || (m_positions.empty() && m_roomPositions.empty() && m_waterPositions.empty())) {
		return false;
	}
	auto upload = [&](const std::vector<Pos> & src, ComPtr<ID3D12Resource> & up,
	                  ComPtr<ID3D12Resource> & def, bool & srvFlag) -> bool {
		if(src.empty()) {
			return true;
		}
		const UINT64 bytes = UINT64(src.size() * sizeof(Pos));
		if(!up || up->GetDesc().Width < bytes) {
			up.reset();
			def.reset();
			if(!createBuffer(m_device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
			                 D3D12_RESOURCE_FLAG_NONE, up.put())) {
				return false;
			}
			if(!createBuffer(m_device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
			                 D3D12_RESOURCE_FLAG_NONE, def.put())) {
				return false;
			}
			srvFlag = false;
		}
		void * mapped = nullptr;
		if(FAILED(up->Map(0, nullptr, &mapped)) || !mapped) {
			return false;
		}
		std::memcpy(mapped, src.data(), size_t(bytes));
		up->Unmap(0, nullptr);
		if(srvFlag) {
			transition(list, def.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			           D3D12_RESOURCE_STATE_COPY_DEST);
			srvFlag = false;
		}
		list->CopyBufferRegion(def.Get(), 0, up.Get(), 0, bytes);
		transition(list, def.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
		           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		srvFlag = true;
		return true;
	};
	if(!upload(m_positions, m_vertUpload, m_vertDefault, m_vertsAreSrv)) {
		return false;
	}
	if(m_roomsDirty && !upload(m_roomPositions, m_roomUpload, m_roomDefault, m_roomVertsAreSrv)) {
		return false;
	}
	if(!upload(m_waterPositions, m_waterUpload, m_waterDefault, m_waterVertsAreSrv)) {
		return false;
	}
	auto uploadMetal = [&](std::vector<Pos> & src, ComPtr<ID3D12Resource> & buf) {
		if(src.empty()) {
			return true;
		}
		const UINT64 bytes = UINT64(src.size() * sizeof(Pos));
		if(!buf || buf->GetDesc().Width < bytes) {
			buf.reset();
			if(!createBuffer(m_device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
			                 D3D12_RESOURCE_FLAG_NONE, buf.put())) {
				return false;
			}
		}
		void * mapped = nullptr;
		if(FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) {
			src.clear();
			return false;
		}
		std::memcpy(mapped, src.data(), size_t(bytes));
		buf->Unmap(0, nullptr);
		return true;
	};
	if(!uploadMetal(m_metalPositions, m_metalUpload)) {
		return false;
	}
	if(m_roomsDirty && !uploadMetal(m_roomMetalPositions, m_roomMetalUpload)) {
		return false;
	}
	return true;
}

bool D3D12Rtao::buildAcceleration(ID3D12GraphicsCommandList * list) {
	ID3D12GraphicsCommandList4 * list4 = nullptr;
	if(FAILED(list->QueryInterface(IID_PPV_ARGS(&list4))) || !list4) {
		LogError << "RTAO: CommandList4 QI failed";
		return false;
	}
	
	const bool haveDyn = !m_positions.empty() && bool(m_vertDefault);
	const bool haveRooms = !m_roomPositions.empty() && bool(m_roomDefault);
	const bool haveWater = !m_waterPositions.empty() && bool(m_waterDefault);
	if(!haveDyn && !haveRooms && !haveWater) {
		list4->Release();
		return false;
	}
	
	auto fillGeo = [](D3D12_RAYTRACING_GEOMETRY_DESC & geo, ID3D12Resource * vb,
	                  size_t vertOffset, size_t nverts) {
		geo = {};
		geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geo.Triangles.VertexCount = UINT(nverts);
		geo.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress()
		                                          + vertOffset * sizeof(Pos);
		geo.Triangles.VertexBuffer.StrideInBytes = sizeof(Pos);
	};
	
	const size_t entityVertCount = (m_reflectOnlyStart == SIZE_MAX)
		? m_positions.size() : m_reflectOnlyStart;
	const size_t playerVertCount = (m_reflectOnlyStart == SIZE_MAX
	                                || m_positions.size() <= m_reflectOnlyStart)
		? 0 : (m_positions.size() - m_reflectOnlyStart);
	const bool havePlayer = playerVertCount >= 3 && bool(m_vertDefault);
	const bool haveEntity = entityVertCount >= 3 && bool(m_vertDefault);
	
	D3D12_RAYTRACING_GEOMETRY_DESC dynGeo {};
	D3D12_RAYTRACING_GEOMETRY_DESC playerGeo {};
	D3D12_RAYTRACING_GEOMETRY_DESC roomGeo {};
	D3D12_RAYTRACING_GEOMETRY_DESC waterGeo {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS dynIn {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS playerIn {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS roomIn {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS waterIn {};
	UINT64 scratchBytes = 0;
	UINT64 dynBlasBytes = 0;
	UINT64 playerBlasBytes = 0;
	UINT64 roomBlasBytes = 0;
	UINT64 waterBlasBytes = 0;
	
	if(haveEntity) {
		fillGeo(dynGeo, m_vertDefault.Get(), 0, entityVertCount);
		dynIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		dynIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		dynIn.NumDescs = 1;
		dynIn.pGeometryDescs = &dynGeo;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info {};
		m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&dynIn, &info);
		dynBlasBytes = info.ResultDataMaxSizeInBytes;
		scratchBytes = (std::max)(scratchBytes, info.ScratchDataSizeInBytes);
	}
	if(havePlayer) {
		fillGeo(playerGeo, m_vertDefault.Get(), entityVertCount, playerVertCount);
		playerIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		playerIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		playerIn.NumDescs = 1;
		playerIn.pGeometryDescs = &playerGeo;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info {};
		m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&playerIn, &info);
		playerBlasBytes = info.ResultDataMaxSizeInBytes;
		scratchBytes = (std::max)(scratchBytes, info.ScratchDataSizeInBytes);
	}
	if(haveRooms && m_roomsDirty) {
		fillGeo(roomGeo, m_roomDefault.Get(), 0, m_roomPositions.size());
		roomIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		roomIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		roomIn.NumDescs = 1;
		roomIn.pGeometryDescs = &roomGeo;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info {};
		m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&roomIn, &info);
		roomBlasBytes = info.ResultDataMaxSizeInBytes;
		scratchBytes = (std::max)(scratchBytes, info.ScratchDataSizeInBytes);
	}
	if(haveWater) {
		fillGeo(waterGeo, m_waterDefault.Get(), 0, m_waterPositions.size());
		waterIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		waterIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		waterIn.NumDescs = 1;
		waterIn.pGeometryDescs = &waterGeo;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info {};
		m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&waterIn, &info);
		waterBlasBytes = info.ResultDataMaxSizeInBytes;
		scratchBytes = (std::max)(scratchBytes, info.ScratchDataSizeInBytes);
	}
	
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn {};
	tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	tlasIn.NumDescs = 4;
	tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo {};
	m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);
	scratchBytes = (std::max)(scratchBytes, tlasInfo.ScratchDataSizeInBytes);
	
	if(!m_scratch || m_scratch->GetDesc().Width < scratchBytes) {
		m_scratch.reset();
		if(!createBuffer(m_device, scratchBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_scratch.put())) {
			list4->Release();
			return false;
		}
	}
	if(haveEntity && (!m_blas || m_blas->GetDesc().Width < dynBlasBytes)) {
		m_blas.reset();
		if(!createBuffer(m_device, dynBlasBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_blas.put())) {
			list4->Release();
			return false;
		}
	}
	if(havePlayer && (!m_playerBlas || m_playerBlas->GetDesc().Width < playerBlasBytes)) {
		m_playerBlas.reset();
		if(!createBuffer(m_device, playerBlasBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_playerBlas.put())) {
			list4->Release();
			return false;
		}
	}
	if(haveRooms && m_roomsDirty && (!m_roomBlas || m_roomBlas->GetDesc().Width < roomBlasBytes)) {
		m_roomBlas.reset();
		if(!createBuffer(m_device, roomBlasBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_roomBlas.put())) {
			list4->Release();
			return false;
		}
	}
	if(haveWater && (!m_waterBlas || m_waterBlas->GetDesc().Width < waterBlasBytes)) {
		m_waterBlas.reset();
		if(!createBuffer(m_device, waterBlasBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_waterBlas.put())) {
			list4->Release();
			return false;
		}
	}
	if(!m_tlas || m_tlas->GetDesc().Width < tlasInfo.ResultDataMaxSizeInBytes) {
		m_tlas.reset();
		if(!createBuffer(m_device, tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_tlas.put())) {
			list4->Release();
			return false;
		}
	}
	const UINT64 instBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * 4;
	if(!m_instances || m_instances->GetDesc().Width < instBytes) {
		m_instances.reset();
		if(!createBuffer(m_device, instBytes, D3D12_HEAP_TYPE_UPLOAD,
		                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
		                 m_instances.put())) {
			list4->Release();
			return false;
		}
	}
	
	if(haveRooms && m_roomsDirty) {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC roomBlas {};
		roomBlas.DestAccelerationStructureData = m_roomBlas->GetGPUVirtualAddress();
		roomBlas.Inputs = roomIn;
		roomBlas.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
		list4->BuildRaytracingAccelerationStructure(&roomBlas, 0, nullptr);
		uavBarrier(list, m_roomBlas.Get());
		uavBarrier(list, m_scratch.Get());
		m_roomsDirty = false;
	}
	if(haveEntity) {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC dynBlas {};
		dynBlas.DestAccelerationStructureData = m_blas->GetGPUVirtualAddress();
		dynBlas.Inputs = dynIn;
		dynBlas.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
		list4->BuildRaytracingAccelerationStructure(&dynBlas, 0, nullptr);
		uavBarrier(list, m_blas.Get());
		uavBarrier(list, m_scratch.Get());
	}
	if(havePlayer) {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC playerBlas {};
		playerBlas.DestAccelerationStructureData = m_playerBlas->GetGPUVirtualAddress();
		playerBlas.Inputs = playerIn;
		playerBlas.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
		list4->BuildRaytracingAccelerationStructure(&playerBlas, 0, nullptr);
		uavBarrier(list, m_playerBlas.Get());
		uavBarrier(list, m_scratch.Get());
	}
	if(haveWater) {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC waterBlas {};
		waterBlas.DestAccelerationStructureData = m_waterBlas->GetGPUVirtualAddress();
		waterBlas.Inputs = waterIn;
		waterBlas.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
		list4->BuildRaytracingAccelerationStructure(&waterBlas, 0, nullptr);
		uavBarrier(list, m_waterBlas.Get());
		uavBarrier(list, m_scratch.Get());
	}
	
	D3D12_RAYTRACING_INSTANCE_DESC * inst = nullptr;
	if(FAILED(m_instances->Map(0, nullptr, reinterpret_cast<void **>(&inst))) || !inst) {
		list4->Release();
		return false;
	}
	std::memset(inst, 0, size_t(instBytes));
	UINT nInst = 0;
	auto writeInst = [&](ID3D12Resource * blas, UINT id, UINT mask) {
		inst[nInst].Transform[0][0] = 1.f;
		inst[nInst].Transform[1][1] = 1.f;
		inst[nInst].Transform[2][2] = 1.f;
		inst[nInst].InstanceID = id;
		inst[nInst].InstanceMask = mask;
		inst[nInst].AccelerationStructure = blas->GetGPUVirtualAddress();
		nInst++;
	};
	if(haveRooms && m_roomBlas) {
		writeInst(m_roomBlas.Get(), 0, 0x1);
	}
	if(haveEntity && m_blas) {
		writeInst(m_blas.Get(), 1, 0x1);
	}
	if(haveWater && m_waterBlas) {
		writeInst(m_waterBlas.Get(), 2, 0x2);
	}
	if(havePlayer && m_playerBlas) {
		writeInst(m_playerBlas.Get(), 3, 0x4);
	}
	m_instances->Unmap(0, nullptr);
	if(nInst == 0) {
		list4->Release();
		return false;
	}
	
	tlasIn.NumDescs = nInst;
	tlasIn.InstanceDescs = m_instances->GetGPUVirtualAddress();
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas {};
	tlas.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
	tlas.Inputs = tlasIn;
	tlas.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
	list4->BuildRaytracingAccelerationStructure(&tlas, 0, nullptr);
	uavBarrier(list, m_tlas.Get());
	uavBarrier(list, m_scratch.Get());
	list4->Release();
	updateDescriptors();
	return true;
}

void D3D12Rtao::rasterizeMasks(ID3D12GraphicsCommandList * list, const glm::mat4x4 & viewProj,
                               ID3D12Resource * depth, float jitterNdcX, float jitterNdcY) {
	if(!list || !m_rtvHeap || !m_waterMask || !m_waterDepth || !m_metalMask) {
		return;
	}
	const bool haveMaskPso = m_waterMaskPso && m_waterDepthPso && m_metalMaskPso && m_maskRoot;
	if(m_maskIsSrv) {
		transition(list, m_waterMask.Get(),
		           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_RENDER_TARGET);
		transition(list, m_waterDepth.Get(),
		           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_RENDER_TARGET);
		transition(list, m_metalMask.Get(),
		           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_maskIsSrv = false;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = rtv0;
	rtv1.ptr += m_rtvSize;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv2 = rtv0;
	rtv2.ptr += SIZE_T(2) * m_rtvSize;
	const float z0[4] = { 0.f, 0.f, 0.f, 0.f };
	const float z1[4] = { 1.f, 0.f, 0.f, 0.f };
	list->ClearRenderTargetView(rtv0, z0, 0, nullptr);
	list->ClearRenderTargetView(rtv1, z1, 0, nullptr);
	list->ClearRenderTargetView(rtv2, z0, 0, nullptr);
	D3D12_VIEWPORT vp {};
	vp.Width = float(m_width);
	vp.Height = float(m_height);
	vp.MaxDepth = 1.f;
	D3D12_RECT sc { 0, 0, LONG(m_width), LONG(m_height) };
	list->RSSetViewports(1, &vp);
	list->RSSetScissorRects(1, &sc);
	if(haveMaskPso) {
		list->SetGraphicsRootSignature(m_maskRoot.Get());
		struct MaskCb {
			float viewProj[16];
			float maskValue;
			float jitterX;
			float jitterY;
			float pad2;
		} cb {};
		static_assert(sizeof(MaskCb) == 20 * 4, "mask root constants");
		std::memcpy(cb.viewProj, glm::value_ptr(viewProj), sizeof(cb.viewProj));
		cb.maskValue = 1.f;
		cb.jitterX = jitterNdcX;
		cb.jitterY = jitterNdcY;
		list->SetGraphicsRoot32BitConstants(0, UINT(sizeof(cb) / 4), &cb, 0);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
	const D3D12_CPU_DESCRIPTOR_HANDLE * dsvPtr = nullptr;
	if(depth && m_dsvHeap) {
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
		dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		m_device->CreateDepthStencilView(depth, &dsvDesc, dsv);
		dsvPtr = &dsv;
	}
	if(haveMaskPso && !m_waterPositions.empty() && m_waterUpload) {
		D3D12_VERTEX_BUFFER_VIEW vb {};
		vb.BufferLocation = m_waterUpload->GetGPUVirtualAddress();
		vb.SizeInBytes = UINT(m_waterPositions.size() * sizeof(Pos));
		vb.StrideInBytes = sizeof(Pos);
		list->IASetVertexBuffers(0, 1, &vb);
		list->OMSetRenderTargets(1, &rtv0, FALSE, dsvPtr);
		list->SetPipelineState(m_waterMaskPso.Get());
		list->DrawInstanced(UINT(m_waterPositions.size()), 1, 0, 0);
		list->OMSetRenderTargets(1, &rtv1, FALSE, dsvPtr);
		list->SetPipelineState(m_waterDepthPso.Get());
		list->DrawInstanced(UINT(m_waterPositions.size()), 1, 0, 0);
	}
	if(haveMaskPso && !m_metalPositions.empty() && m_metalUpload) {
		list->OMSetRenderTargets(1, &rtv2, FALSE, dsvPtr);
		list->SetPipelineState(m_metalMaskPso.Get());
		D3D12_VERTEX_BUFFER_VIEW vb {};
		vb.BufferLocation = m_metalUpload->GetGPUVirtualAddress();
		vb.SizeInBytes = UINT(m_metalPositions.size() * sizeof(Pos));
		vb.StrideInBytes = sizeof(Pos);
		list->IASetVertexBuffers(0, 1, &vb);
		list->DrawInstanced(UINT(m_metalPositions.size()), 1, 0, 0);
	}
	if(haveMaskPso && !m_roomMetalPositions.empty() && m_roomMetalUpload) {
		list->OMSetRenderTargets(1, &rtv2, FALSE, dsvPtr);
		list->SetPipelineState(m_metalMaskPso.Get());
		D3D12_VERTEX_BUFFER_VIEW vb {};
		vb.BufferLocation = m_roomMetalUpload->GetGPUVirtualAddress();
		vb.SizeInBytes = UINT(m_roomMetalPositions.size() * sizeof(Pos));
		vb.StrideInBytes = sizeof(Pos);
		list->IASetVertexBuffers(0, 1, &vb);
		list->DrawInstanced(UINT(m_roomMetalPositions.size()), 1, 0, 0);
	}
	transition(list, m_waterMask.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_waterDepth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_metalMask.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_maskIsSrv = true;
	m_masksRasterized = true;
}

bool D3D12Rtao::apply(ID3D12GraphicsCommandList * list, ID3D12Resource * backbuffer,
                      ID3D12Resource * depth,
                      const glm::mat4x4 & view, const glm::mat4x4 & proj,
                      int width, int height, const Settings & settings,
                      const GpuLight * lights, size_t lightCount, std::uint64_t rtvPtr) {
	if(!m_ready || !list || !backbuffer || !depth) {
		return false;
	}
	const int aoQuality = (std::max)(0, (std::min)(settings.aoQuality, kMaxRtQuality));
	const int shadowQuality = (std::max)(0, (std::min)(settings.shadowQuality, kMaxRtQuality));
	const int giQuality = (std::max)(0, (std::min)(settings.giQuality, kMaxRtQuality));
	const int transRefl = (std::max)(0, (std::min)(settings.transRefl, kMaxRtQuality));
	const int metalRefl = (std::max)(0, (std::min)(settings.metalRefl, kMaxRtQuality));
	const int shadowDenoise = (std::max)(0, (std::min)(settings.shadowDenoise, kMaxShadowDenoise));
	const int giDenoise = (std::max)(0, (std::min)(settings.giDenoise, kMaxGiDenoise));
	const int contact = settings.contact ? 1 : 0;
	const bool skipTemporal = settings.skipTemporal;
	if(aoQuality <= 0 && shadowQuality <= 0 && giQuality <= 0 && transRefl <= 0 && metalRefl <= 0
	   && contact <= 0) {
		return false;
	}
	if(!ensureTargets(width, height) || !m_lights) {
		return false;
	}
	const glm::mat4x4 invView = glm::inverse(view);
	const glm::vec3 cam(invView[3]);
	if(m_positions.empty() && m_roomPositions.empty() && m_waterPositions.empty()) {
		return false;
	}
	ID3D12GraphicsCommandList4 * list4 = nullptr;
	if(FAILED(list->QueryInterface(IID_PPV_ARGS(&list4))) || !list4) {
		return false;
	}
	if(!ensureGeometryBuffers(list) || !buildAcceleration(list)) {
		list4->Release();
		return false;
	}
	const glm::mat4x4 viewProjEarly = proj * view;
	rasterizeMasks(list, viewProjEarly, depth, settings.jitterNdcX, settings.jitterNdcY);
	D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv {};
	depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSrv.Texture2D.MipLevels = 1;
	// These two write into the reserved block like slot() does, so they carry the same base.
	const SIZE_T heapStart = m_heap->GetCPUDescriptorHandleForHeapStart().ptr
	                         + SIZE_T(m_rtaoBase) * m_descriptorSize;
	D3D12_CPU_DESCRIPTOR_HANDLE depthCpu { heapStart + SIZE_T(3) * m_descriptorSize };
	m_device->CreateShaderResourceView(depth, &depthSrv, depthCpu);
	D3D12_CPU_DESCRIPTOR_HANDLE depthPs {
		heapStart + SIZE_T(kCompositeBase + 3) * m_descriptorSize
	};
	m_device->CreateShaderResourceView(depth, &depthSrv, depthPs);
	transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	
	const D3D12_RESOURCE_STATES colorShader =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	transition(list, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_colorCopy.Get(),
	           m_colorIsShader ? colorShader : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	           D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(m_colorCopy.Get(), backbuffer);
	transition(list, m_colorCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, colorShader);
	transition(list, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_colorIsShader = true;
	
	if(!m_aoIsUav) {
		transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		transition(list, m_gi.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		transition(list, m_spec.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_aoIsUav = true;
	}
	
	GpuLight packed[kMaxShadowLights] {};
	const size_t nlights = (std::min)(lightCount, kMaxShadowLights);
	if(lights && nlights) {
		std::memcpy(packed, lights, nlights * sizeof(GpuLight));
	}
	void * mappedLights = nullptr;
	if(SUCCEEDED(m_lights->Map(0, nullptr, &mappedLights)) && mappedLights) {
		std::memcpy(mappedLights, packed, sizeof(packed));
		m_lights->Unmap(0, nullptr);
	}
	
	const glm::mat4x4 jitteredProj = jitteredProjection(proj, settings.jitterNdcX, settings.jitterNdcY);
	const glm::mat4x4 viewProj = jitteredProj * view;
	const glm::mat4x4 inv = glm::inverse(viewProj);
	
	DxrConstants cb {};
	std::memcpy(cb.invViewProj, glm::value_ptr(inv), sizeof(cb.invViewProj));
	cb.cameraPos[0] = cam.x;
	cb.cameraPos[1] = cam.y;
	cb.cameraPos[2] = cam.z;
	cb.radius = aoRadius[aoQuality];
	cb.aoRays = aoRayCount[aoQuality];
	// One pixel spans 2 / width in NDC; clip.x = proj[0][0] * xView, so at view
	// depth w a pixel covers 2 * w / (width * proj[0][0]) world units.
	cb.pixelWorld = 2.f / ((std::max)(float(m_width), 1.f) * (std::max)(proj[0][0], 1e-4f));
	cb.width = UINT(m_width);
	cb.height = UINT(m_height);
	cb.lightCount = UINT(nlights);
	cb.shadowsOn = (shadowQuality > 0) ? 1u : 0u;
	const glm::mat4x4 & prevViewProj = m_histValid ? m_prevViewProj : viewProj;
	std::memcpy(cb.prevViewProj, glm::value_ptr(prevViewProj), sizeof(cb.prevViewProj));
	cb.penumbraRays = shadowRays[shadowQuality];
	cb.giOn = (giQuality > 0) ? 1u : 0u;
	cb.giWidth = UINT((std::max)(m_width / 2, 1));
	cb.giHeight = UINT((std::max)(m_height / 2, 1));
	cb.giRays = giRayCount[giQuality];
	cb.temporalAlpha = (m_histValid && !skipTemporal) ? shadowAlpha[shadowDenoise] : 0.f;
	// Camera.cpp: clip.z = Q * zView - Q * near, clip.w = zView → zView = B / (z - A).
	cb.projA = proj[2][2];
	cb.projB = proj[3][2];
	cb.specRays = transRays[transRefl];
	cb.specHalfRes = (transRefl == 1) ? 1u : 0u;
	cb.contactOn = (contact && shadowQuality > 0) ? 1u : 0u;
	cb.metalRays = metalRayCount[metalRefl];
	// 0.10 kept 90 % of last frame's reflection, so any reprojection error took dozens of frames
	// to wash out and read as a smear trailing the camera. 0.16 leans back towards steadying the
	// 1-2 ray reflection now that water reprojects against its own surface and the trail is gone;
	// lower it further only if the smear comes back, raise it if the sparkle does.
	cb.specAlpha = (m_histValid && !skipTemporal) ? 0.16f : 0.f;
	cb.contactTMax = 60.f;
	cb.giTemporalAlpha = (m_histValid && !skipTemporal) ? giAlpha[giDenoise] : 0.f;
	cb.playerVertBase = (m_reflectOnlyStart == SIZE_MAX) ? 0u : UINT(m_reflectOnlyStart);
	const DistancePreset dist = distancePreset(settings.distance);

	ID3D12DescriptorHeap * heaps[] = { m_heap };
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(m_rtRoot.Get());
	if(m_paramsMapped) {
		m_paramsSlot = (m_paramsSlot + 1) % kParamsSlots;
		std::memcpy(static_cast<char *>(m_paramsMapped) + m_paramsSlot * kParamsStride,
		            &cb, sizeof(cb));
		list->SetComputeRootConstantBufferView(0, m_paramsCbuf->GetGPUVirtualAddress()
		                                          + m_paramsSlot * kParamsStride);
	}
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += SIZE_T(m_rtaoBase) * m_descriptorSize;
	list->SetComputeRootDescriptorTable(1, gpu);
	D3D12_GPU_DESCRIPTOR_HANDLE uav = gpu;
	uav.ptr += SIZE_T(kRtUavBase) * m_descriptorSize;
	list->SetComputeRootDescriptorTable(2, uav);
	if(m_viewCbuf) {
		void * mappedView = nullptr;
		if(SUCCEEDED(m_viewCbuf->Map(0, nullptr, &mappedView)) && mappedView) {
			DxrViewCbuf viewCb {};
			std::memcpy(viewCb.viewProj, glm::value_ptr(viewProj), 64);
			viewCb.specTMax = dist.specTMax;
			viewCb.giTMax = dist.giTMax;
			viewCb.rtRange = (settings.range > 8.f) ? settings.range : dist.caster;
			viewCb.waterFacing = waterFacing[transRefl];
			viewCb.waterFill = reflectFill[transRefl];
			viewCb.metalFill = reflectFill[metalRefl];
			std::memcpy(mappedView, &viewCb, sizeof(viewCb));
			m_viewCbuf->Unmap(0, nullptr);
		}
		list->SetComputeRootConstantBufferView(3, m_viewCbuf->GetGPUVirtualAddress());
	}
	
	D3D12_DISPATCH_RAYS_DESC rays {};
	const D3D12_GPU_VIRTUAL_ADDRESS table = m_shaderTable->GetGPUVirtualAddress();
	rays.RayGenerationShaderRecord.StartAddress = table;
	rays.RayGenerationShaderRecord.SizeInBytes = kShaderRecord;
	rays.MissShaderTable.StartAddress = table + kShaderRecord;
	rays.MissShaderTable.SizeInBytes = kShaderRecord;
	rays.MissShaderTable.StrideInBytes = kShaderRecord;
	rays.HitGroupTable.StartAddress = table + kShaderRecord * 2;
	rays.HitGroupTable.SizeInBytes = kShaderRecord;
	rays.HitGroupTable.StrideInBytes = kShaderRecord;
	rays.Width = UINT(m_width);
	rays.Height = UINT(m_height);
	rays.Depth = 1;
	list4->SetPipelineState1(m_rtState.Get());
	list4->DispatchRays(&rays);
	list4->Release();
	uavBarrier(list, m_ao.Get());
	uavBarrier(list, m_shadow.Get());
	uavBarrier(list, m_gi.Get());
	uavBarrier(list, m_depthCur.Get());
	uavBarrier(list, m_spec.Get());

	transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_gi.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_spec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_aoIsUav = false;
	
	D3D12_CPU_DESCRIPTOR_HANDLE rtv {};
	rtv.ptr = SIZE_T(rtvPtr);
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	list->SetGraphicsRootSignature(m_compositeRoot.Get());
	list->SetPipelineState(m_compositePso.Get());
	const int shadowRadius = skipTemporal ? 0 : ((shadowDenoise > 0) ? 3 : 2);
	const int aoBlur = skipTemporal ? 0 : 5;
	struct CompCb {
		int shadowRadius;
		int aoRadius;
		int specHalf;
		int padC;
		float projA;
		float projB;
	} compCb { shadowRadius, aoBlur, int(cb.specHalfRes), 0, cb.projA, cb.projB };
	list->SetGraphicsRoot32BitConstants(0, 6, &compCb, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE color = gpu;
	color.ptr += SIZE_T(kCompositeBase) * m_descriptorSize;
	list->SetGraphicsRootDescriptorTable(1, color);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->DrawInstanced(3, 1, 0, 0);
	transition(list, depth,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	           D3D12_RESOURCE_STATE_DEPTH_WRITE);

	// Keep this frame's accumulated AO / shadow and raw depth as next frame's history.
	const D3D12_RESOURCE_STATES histRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_depthCur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_gi.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_spec.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_aoPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_shadowPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_depthPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_giPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_specPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(m_aoPrev.Get(), m_ao.Get());
	list->CopyResource(m_shadowPrev.Get(), m_shadow.Get());
	list->CopyResource(m_depthPrev.Get(), m_depthCur.Get());
	list->CopyResource(m_giPrev.Get(), m_gi.Get());
	list->CopyResource(m_specPrev.Get(), m_spec.Get());
	transition(list, m_aoPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_shadowPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_depthPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_giPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_specPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_gi.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_spec.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_depthCur.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	m_prevViewProj = viewProj;
	m_histValid = true;
	return true;
}

#endif // ARX_HAVE_D3D12
