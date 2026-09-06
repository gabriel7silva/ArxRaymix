/*
 * Arx Raymix — Option A RTAO. World-space SMY_VERTEX → TLAS → AO → multiply.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/dxr/D3D12Rtao.h"

#if ARX_HAVE_D3D12

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>

#include <windows.h>

#include <glm/gtc/type_ptr.hpp>

#include "graphics/Math.h"
#include "io/log/Logger.h"
#include "platform/Platform.h"

namespace {

constexpr size_t kMaxDynTriangles = 80000;
constexpr size_t kMaxRoomTriangles = 120000;
constexpr UINT kIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
constexpr UINT kShaderRecord = 64;
// Heap layout: [0..9] RT SRVs (t0..t9), [10..13] RT UAVs (u0..u3),
// [14..18] composite SRVs (color, ao, shadow, depth, gi).
constexpr UINT kHeapCount = 19;
constexpr UINT kRtSrvCount = 10;
constexpr UINT kRtUavBase = 10;
constexpr UINT kRtUavCount = 4;
constexpr UINT kCompositeBase = 14;
constexpr UINT kRootConstants = 52;
// History blend weight for the current frame. 0.12 → ~90% converged after 18 frames.
constexpr float kTemporalAlpha = 0.08f;

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
RWTexture2D<float> g_ao : register(u0);
RWTexture2D<float> g_shadow : register(u1);
RWTexture2D<float4> g_gi : register(u2);
RWTexture2D<float> g_depthOut : register(u3);

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
};

// Only the cleared depth (1.0) is sky. Camera.cpp writes z = Q * (1 - near / w)
// with near = 1, far = 6400, so z reaches 0.999 at w ≈ 865 units: an older
// 0.999 cutoff silently dropped AO / shadows on everything farther than that
// and made the 865-unit boundary flicker as the camera bobbed across it.
static const float SKY_Z = 0.99999;

struct RayPayload {
	float t;
	float3 n;
};

float hash(uint n) {
	n = (n << 13u) ^ n;
	n = n * (n * n * 15731u + 789221u) + 1376312589u;
	return float(n & 0x00ffffffu) / 16777216.0;
}

float3 hemisphere(float3 n, uint seed) {
	float u = hash(seed);
	float v = hash(seed * 747796405u + 2891336453u);
	float a = 6.2831853 * u;
	float z = v;
	float r = sqrt(max(0.0, 1.0 - z * z));
	float3 t = normalize(abs(n.z) < 0.999 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
	float3 b = cross(n, t);
	return normalize(t * (cos(a) * r) + b * (sin(a) * r) + n * z);
}

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
	float3 o[8] = {
		float3(0.00, 0.00, 1.00), float3(0.40, 0.00, 0.92),
		float3(-0.20, 0.35, 0.92), float3(-0.20, -0.35, 0.92),
		float3(0.30, 0.30, 0.90), float3(-0.35, 0.15, 0.92),
		float3(0.15, -0.40, 0.90), float3(-0.10, 0.10, 0.99)
	};
	float3 l = normalize(o[s & 7u]);
	l.xy = rotate2(l.xy, rot);
	float3 t = normalize(abs(n.z) < 0.999 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
	float3 b = cross(n, t);
	return normalize(t * l.x + b * l.y + n * l.z);
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
	// Normal from depth. Per axis, take whichever neighbour is closer in linear
	// depth (so a silhouette on one side does not skew the normal), and call the
	// pixel a discontinuity when even that step is far bigger than a surface at a
	// steep grazing angle could produce (measured in pixel footprints, not raw z:
	// the old 0.04 raw-z test never fired, so decal / wall and bar / wall edges
	// got normals tilted up to ~80 degrees and their rays started inside geometry).
	float wC = projB / (z - projA);
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
			         0xff, 0, 1, 0, ao, aop);
			if(aop.t >= 8.0 && aop.t < radius) {
				occ += 1.0 - aop.t / radius;
			}
		}
		aoCur = max(1.0 - occ / float(aoRays), 0.45);
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
				         0xff, 0, 1, 0, sh, shp);
				vis += (shp.t >= tmax) ? 1.0 : 0.0;
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
			bounce.TMax = 800.0;
			RayPayload bp;
			bp.t = 801.0;
			bp.n = float3(0, 0, 0);
			TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
			         0xff, 0, 1, 0, bounce, bp);
			if(bp.t < 8.0 || bp.t >= 800.0) {
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
				float d = length(toL);
				if(d < 40.0) {
					continue;
				}
				float ndotl = saturate(dot(hn, toL / d));
				float span = max(b.y - b.x, 1e-3);
				float fall = saturate((b.y - d) / span);
				// Bounce carries the light's hue; grey bounce read as a white haze
				// on the ceiling above the torch.
				fill += col.rgb * min(a.w * fall * ndotl * b.w * 0.20, 0.25);
			}
			// Per-ray clamp, then average over rays *launched*: dividing by the
			// rays that hit let one lucky ray next to a lamp light the whole texel
			// (the white GI sparks).
			float fl = dot(fill, float3(0.30, 0.59, 0.11));
			gi += fill * (min(fl, 0.3) / max(fl, 1e-4));
		}
		gi /= float(nGi);
		float lum = dot(gi, float3(0.30, 0.59, 0.11));
		if(lum > 0.35) {
			gi *= 0.35 / lum;
		}
		if(histOk) {
			gi = lerp(g_giPrev.Load(int3(histPix / 2, 0)).rgb, gi, temporalAlpha);
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
	} else {
		v0 = g_verts[prim * 3 + 0].xyz;
		v1 = g_verts[prim * 3 + 1].xyz;
		v2 = g_verts[prim * 3 + 2].xyz;
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
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	float2 uv = float2((id << 1) & 2, id & 2);
	VSOut o;
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
float bilateral(Texture2D tex, int2 pix, int radius, float zCenter) {
	float s = 0.0;
	float wsum = 0.0;
	const float zTol = 0.012;
	const float r2 = float(radius * radius);
	for(int y = -6; y <= 6; ++y) {
		for(int x = -6; x <= 6; ++x) {
			if(abs(x) > radius || abs(y) > radius) {
				continue;
			}
			int2 p = pix + int2(x, y);
			float z = depthTex.Load(int3(p, 0)).r;
			float dz = abs(z - zCenter);
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
			// Must stay above RayGen's 0.35 luma clamp or every sample is rejected.
			if(dot(g, float3(0.30, 0.59, 0.11)) > 0.6) {
				continue;
			}
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
	float ao = lerp(1.0, bilateral(aoTex, pix, 5, zCenter), 1.0);
	float sh = lerp(0.10, 1.0, bilateral(shadowTex, pix, 3, zCenter));
	// Bounce light scaled by the surface's own raster color (plus a small floor
	// for the darkest stone) so it reads as light on the material, not grey haze.
	// The lit raster colour stands in for albedo, so cap it (a surface already
	// blown out by a spell light must not bounce even brighter) and add in
	// screen fashion so the sum never saturates to white.
	float3 bounce = giUpsample(pix, zCenter) * (0.15 + min(c, 0.45)) * 2.0;
	c = c + bounce * (1.0 - c);
	// AO and shadow compound; keep a floor so nothing goes fully black.
	return float4(saturate(c) * max(ao * sh, 0.08), 1);
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

void loadDxilBeside(const std::wstring & dxcompilerPath) {
	const size_t slash = dxcompilerPath.find_last_of(L"\\/");
	if(slash == std::wstring::npos) {
		LoadLibraryW(L"dxil.dll");
		return;
	}
	LoadLibraryW((dxcompilerPath.substr(0, slash + 1) + L"dxil.dll").c_str());
}

HMODULE tryLoadDxcompilerFile(const std::wstring & path) {
	if(path.empty()) {
		return nullptr;
	}
	loadDxilBeside(path);
	return LoadLibraryW(path.c_str());
}

HMODULE loadDxcompiler() {
	if(HMODULE lib = tryLoadDxcompilerFile(L"dxcompiler.dll")) {
		return lib;
	}
	
	wchar_t exe[MAX_PATH] {};
	if(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
		std::wstring dir(exe);
		const size_t slash = dir.find_last_of(L"\\/");
		if(slash != std::wstring::npos) {
			if(HMODULE lib = tryLoadDxcompilerFile(dir.substr(0, slash + 1) + L"dxcompiler.dll")) {
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
	m_histValid = false;
	m_lights.reset();
	m_shaderTable.reset();
	m_instances.reset();
	m_scratch.reset();
	m_tlas.reset();
	m_blas.reset();
	m_vertDefault.reset();
	m_vertUpload.reset();
	m_roomDefault.reset();
	m_roomUpload.reset();
	m_roomBlas.reset();
	m_heap.reset();
	m_compositePso.reset();
	m_rtState.reset();
	m_compositeRoot.reset();
	m_rtRoot.reset();
	m_dxil.clear();
	m_device5.reset();
	m_device = nullptr;
	m_positions.clear();
	m_roomPositions.clear();
	m_roomsDirty = true;
	m_colorIsShader = false;
}

bool D3D12Rtao::init(ID3D12Device * device) {
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
	
	if(!compileRayLib() || !createPipeline()) {
		LogError << "RTAO: DXR pipeline failed — raster only";
		m_ready = false;
		return true;
	}
	
	D3D12_DESCRIPTOR_HEAP_DESC heap {};
	heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap.NumDescriptors = kHeapCount;
	heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(m_heap.put())))) {
		LogError << "RTAO: descriptor heap failed";
		return true;
	}
	if(!createBuffer(device, UINT64(kMaxShadowLights * sizeof(GpuLight)), D3D12_HEAP_TYPE_UPLOAD,
	                 D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, m_lights.put())) {
		LogError << "RTAO: light buffer failed";
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
	D3D12_ROOT_PARAMETER params[3] {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = kRootConstants;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srv;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &uav;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rs {};
	rs.NumParameters = 3;
	rs.pParameters = params;
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> err;
	if(FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put()))) {
		LogError << "RTAO: RT root signature failed";
		return false;
	}
	if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                        IID_PPV_ARGS(m_rtRoot.put())))) {
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
	colorSrv.NumDescriptors = 5;
	colorSrv.BaseShaderRegister = 0;
	D3D12_ROOT_PARAMETER cparams[1] {};
	cparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	cparams[0].DescriptorTable.NumDescriptorRanges = 1;
	cparams[0].DescriptorTable.pDescriptorRanges = &colorSrv;
	cparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC samp {};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC crs {};
	crs.NumParameters = 1;
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

void D3D12Rtao::resize(int width, int height) {
	if(width == m_width && height == m_height && m_ao && m_shadow && m_gi && m_depthPrev) {
		return;
	}
	m_ao.reset();
	m_shadow.reset();
	m_gi.reset();
	m_colorCopy.reset();
	m_aoPrev.reset();
	m_shadowPrev.reset();
	m_depthCur.reset();
	m_depthPrev.reset();
	m_giPrev.reset();
	m_histValid = false;
	m_width = 0;
	m_height = 0;
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
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ao,
	                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
	                                            IID_PPV_ARGS(m_ao.put())))) {
		LogError << "RTAO: AO target failed";
		return;
	}
	if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ao,
	                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
	                                            IID_PPV_ARGS(m_shadow.put())))) {
		LogError << "RTAO: shadow target failed";
		m_ao.reset();
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
		m_ao.reset();
		m_shadow.reset();
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
		m_ao.reset();
		m_shadow.reset();
		m_gi.reset();
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
	   || !makeTex(gi, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srvState, m_giPrev)) {
		LogError << "RTAO: history targets failed";
		m_ao.reset();
		m_shadow.reset();
		m_gi.reset();
		m_colorCopy.reset();
		m_aoPrev.reset();
		m_shadowPrev.reset();
		m_depthCur.reset();
		m_depthPrev.reset();
		m_giPrev.reset();
		return;
	}
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
	if(width != m_width || height != m_height || !m_ao) {
		resize(width, height);
	}
	return m_ao && m_shadow && m_gi && m_colorCopy
	       && m_aoPrev && m_shadowPrev && m_depthCur && m_depthPrev && m_giPrev;
}

void D3D12Rtao::updateDescriptors() {
	if(!m_device || !m_heap) {
		return;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
	auto slot = [&](UINT i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
		h.ptr += SIZE_T(i) * m_descriptorSize;
		return h;
	};
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
}

void D3D12Rtao::clearRooms() {
	m_roomPositions.clear();
	m_roomsDirty = true;
}

void D3D12Rtao::addTris(std::vector<Pos> & dst, size_t cap, Renderer::Primitive primitive,
                        const SMY_VERTEX * vertices, size_t nvertices,
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

void D3D12Rtao::addWorld(Renderer::Primitive primitive, const SMY_VERTEX3 * vertices, size_t nvertices,
                         const unsigned short * indices, size_t nindices) {
	if(!m_supported || !vertices || nvertices == 0) {
		return;
	}
	const size_t n = indices ? nindices : nvertices;
	forEachTriangleIndices(primitive, n, [&](size_t i0, size_t i1, size_t i2) {
		if(m_positions.size() / 3 >= kMaxDynTriangles) {
			if(!m_loggedCap) {
				LogInfo << "DXR: triangle cap " << kMaxDynTriangles;
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
		m_positions.push_back({ pa.x, pa.y, pa.z, 1.f });
		m_positions.push_back({ pb.x, pb.y, pb.z, 1.f });
		m_positions.push_back({ pc.x, pc.y, pc.z, 1.f });
	});
}

bool D3D12Rtao::ensureGeometryBuffers(ID3D12GraphicsCommandList * list) {
	if(!m_device || (m_positions.empty() && m_roomPositions.empty())) {
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
	if(!haveDyn && !haveRooms) {
		list4->Release();
		return false;
	}
	
	auto fillGeo = [](D3D12_RAYTRACING_GEOMETRY_DESC & geo, ID3D12Resource * vb, size_t nverts) {
		geo = {};
		geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		geo.Triangles.VertexCount = UINT(nverts);
		geo.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
		geo.Triangles.VertexBuffer.StrideInBytes = sizeof(Pos);
	};
	
	D3D12_RAYTRACING_GEOMETRY_DESC dynGeo {};
	D3D12_RAYTRACING_GEOMETRY_DESC roomGeo {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS dynIn {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS roomIn {};
	UINT64 scratchBytes = 0;
	UINT64 dynBlasBytes = 0;
	UINT64 roomBlasBytes = 0;
	
	if(haveDyn) {
		fillGeo(dynGeo, m_vertDefault.Get(), m_positions.size());
		dynIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		dynIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
		dynIn.NumDescs = 1;
		dynIn.pGeometryDescs = &dynGeo;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info {};
		m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&dynIn, &info);
		dynBlasBytes = info.ResultDataMaxSizeInBytes;
		scratchBytes = (std::max)(scratchBytes, info.ScratchDataSizeInBytes);
	}
	if(haveRooms && m_roomsDirty) {
		fillGeo(roomGeo, m_roomDefault.Get(), m_roomPositions.size());
		roomIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		roomIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		roomIn.NumDescs = 1;
		roomIn.pGeometryDescs = &roomGeo;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info {};
		m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&roomIn, &info);
		roomBlasBytes = info.ResultDataMaxSizeInBytes;
		scratchBytes = (std::max)(scratchBytes, info.ScratchDataSizeInBytes);
	}
	
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn {};
	tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	tlasIn.NumDescs = 2;
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
	if(haveDyn && (!m_blas || m_blas->GetDesc().Width < dynBlasBytes)) {
		m_blas.reset();
		if(!createBuffer(m_device, dynBlasBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_blas.put())) {
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
	if(!m_tlas || m_tlas->GetDesc().Width < tlasInfo.ResultDataMaxSizeInBytes) {
		m_tlas.reset();
		if(!createBuffer(m_device, tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
		                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_tlas.put())) {
			list4->Release();
			return false;
		}
	}
	const UINT64 instBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * 2;
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
		m_roomsDirty = false;
	}
	if(haveDyn) {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC dynBlas {};
		dynBlas.DestAccelerationStructureData = m_blas->GetGPUVirtualAddress();
		dynBlas.Inputs = dynIn;
		dynBlas.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
		list4->BuildRaytracingAccelerationStructure(&dynBlas, 0, nullptr);
		uavBarrier(list, m_blas.Get());
	}
	
	D3D12_RAYTRACING_INSTANCE_DESC * inst = nullptr;
	if(FAILED(m_instances->Map(0, nullptr, reinterpret_cast<void **>(&inst))) || !inst) {
		list4->Release();
		return false;
	}
	std::memset(inst, 0, size_t(instBytes));
	UINT nInst = 0;
	auto writeInst = [&](ID3D12Resource * blas, UINT id) {
		inst[nInst].Transform[0][0] = 1.f;
		inst[nInst].Transform[1][1] = 1.f;
		inst[nInst].Transform[2][2] = 1.f;
		inst[nInst].InstanceID = id;
		inst[nInst].InstanceMask = 0xff;
		inst[nInst].AccelerationStructure = blas->GetGPUVirtualAddress();
		nInst++;
	};
	if(haveRooms && m_roomBlas) {
		writeInst(m_roomBlas.Get(), 0);
	}
	if(haveDyn && m_blas) {
		writeInst(m_blas.Get(), 1);
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
	list4->Release();
	updateDescriptors();
	return true;
}

bool D3D12Rtao::apply(ID3D12GraphicsCommandList * list, ID3D12Resource * backbuffer,
                      ID3D12Resource * depth,
                      const glm::mat4x4 & view, const glm::mat4x4 & proj,
                      int width, int height, int aoQuality, int shadowQuality, int giQuality,
                      const GpuLight * lights, size_t lightCount, std::uint64_t rtvPtr) {
	if(!m_ready || !list || !backbuffer || !depth) {
		return false;
	}
	aoQuality = (std::max)(0, (std::min)(aoQuality, 3));
	shadowQuality = (std::max)(0, (std::min)(shadowQuality, 3));
	giQuality = (std::max)(0, (std::min)(giQuality, 3));
	if(aoQuality <= 0 && shadowQuality <= 0 && giQuality <= 0) {
		return false;
	}
	if(!ensureTargets(width, height) || !m_lights) {
		return false;
	}
	const glm::mat4x4 invView = glm::inverse(view);
	const glm::vec3 cam(invView[3]);
	if(m_positions.empty() && m_roomPositions.empty()) {
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
	D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv {};
	depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSrv.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE depthCpu = m_heap->GetCPUDescriptorHandleForHeapStart();
	depthCpu.ptr += SIZE_T(3) * m_descriptorSize;
	m_device->CreateShaderResourceView(depth, &depthSrv, depthCpu);
	D3D12_CPU_DESCRIPTOR_HANDLE depthPs = m_heap->GetCPUDescriptorHandleForHeapStart();
	depthPs.ptr += SIZE_T(kCompositeBase + 3) * m_descriptorSize;
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
	
	const glm::mat4x4 viewProj = proj * view;
	const glm::mat4x4 inv = glm::inverse(viewProj);
	
	struct Constants {
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
	} cb {};
	static_assert(sizeof(cb) == kRootConstants * 4, "DXR root constants must match HLSL cbuffer");
	std::memcpy(cb.invViewProj, glm::value_ptr(inv), sizeof(cb.invViewProj));
	cb.cameraPos[0] = cam.x;
	cb.cameraPos[1] = cam.y;
	cb.cameraPos[2] = cam.z;
	const float aoRadius[] = { 0.f, 24.f, 32.f, 48.f };
	// Ray counts per quality (Off / Low / Medium / High). The user asked for a
	// lot: High is ~70 rays per pixel with three lights reaching it.
	const UINT aoRayCount[] = { 0u, 8u, 16u, 24u };
	const UINT shadowRays[] = { 0u, 2u, 8u, 16u };
	const UINT giRayCount[] = { 0u, 4u, 8u, 16u };
	cb.radius = aoRadius[aoQuality];
	cb.aoRays = aoRayCount[aoQuality];
	m_frameIndex++;
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
	cb.temporalAlpha = m_histValid ? kTemporalAlpha : 0.f;
	// Camera.cpp: clip.z = Q * zView - Q * near, clip.w = zView → zView = B / (z - A).
	cb.projA = proj[2][2];
	cb.projB = proj[3][2];

	ID3D12DescriptorHeap * heaps[] = { m_heap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(m_rtRoot.Get());
	list->SetComputeRoot32BitConstants(0, kRootConstants, &cb, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
	list->SetComputeRootDescriptorTable(1, gpu);
	D3D12_GPU_DESCRIPTOR_HANDLE uav = gpu;
	uav.ptr += SIZE_T(kRtUavBase) * m_descriptorSize;
	list->SetComputeRootDescriptorTable(2, uav);
	
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

	transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_gi.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_aoIsUav = false;
	
	D3D12_CPU_DESCRIPTOR_HANDLE rtv {};
	rtv.ptr = SIZE_T(rtvPtr);
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	list->SetGraphicsRootSignature(m_compositeRoot.Get());
	list->SetPipelineState(m_compositePso.Get());
	D3D12_GPU_DESCRIPTOR_HANDLE color = gpu;
	color.ptr += SIZE_T(kCompositeBase) * m_descriptorSize;
	list->SetGraphicsRootDescriptorTable(0, color);
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
	transition(list, m_aoPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_shadowPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_depthPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	transition(list, m_giPrev.Get(), histRead, D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(m_aoPrev.Get(), m_ao.Get());
	list->CopyResource(m_shadowPrev.Get(), m_shadow.Get());
	list->CopyResource(m_depthPrev.Get(), m_depthCur.Get());
	list->CopyResource(m_giPrev.Get(), m_gi.Get());
	transition(list, m_aoPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_shadowPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_depthPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_giPrev.Get(), D3D12_RESOURCE_STATE_COPY_DEST, histRead);
	transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_gi.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_depthCur.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	m_prevViewProj = viewProj;
	m_histValid = true;
	return true;
}

#endif // ARX_HAVE_D3D12
