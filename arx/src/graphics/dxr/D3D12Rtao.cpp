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
constexpr UINT kHeapCount = 10;
constexpr UINT kRootConstants = 26;

static_assert(sizeof(D3D12Rtao::GpuLight) == 32, "DXR light record is two float4s");

const char * kRayLib = R"(
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<float4> g_verts : register(t1);
StructuredBuffer<float4> g_lights : register(t2);
Texture2D<float> g_depth : register(t3);
RWTexture2D<float> g_ao : register(u0);
RWTexture2D<float> g_shadow : register(u1);

cbuffer Params : register(b0) {
	float4x4 invViewProj;
	float3 cameraPos;
	float radius;
	uint aoRays;
	uint frameIndex;
	uint width;
	uint height;
	uint lightCount;
	uint shadowsOn;
};

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

[shader("raygeneration")]
void RayGen() {
	uint2 pixel = DispatchRaysIndex().xy;
	if(pixel.x >= width || pixel.y >= height) {
		return;
	}
	float z = g_depth.Load(int3(pixel, 0)).r;
	if(z <= 0.0 || z >= 0.999) {
		g_ao[pixel] = 1.0;
		g_shadow[pixel] = 1.0;
		return;
	}
	float2 uv = (float2(pixel) + 0.5) / float2(width, height);
	float ndcX = uv.x * 2.0 - 1.0;
	float ndcY = 1.0 - uv.y * 2.0;
	float4 worldH = mul(invViewProj, float4(ndcX, ndcY, z, 1.0));
	float3 pos = worldH.xyz / max(worldH.w, 1e-6);
	float zR = g_depth.Load(int3(int(pixel.x) + 1, int(pixel.y), 0)).r;
	float zD = g_depth.Load(int3(int(pixel.x), int(pixel.y) + 1, 0)).r;
	const bool disc = (zR <= 0.0 || zR >= 0.999 || zD <= 0.0 || zD >= 0.999
	                   || abs(zR - z) > 0.04 || abs(zD - z) > 0.04);
	float3 n;
	if(disc) {
		n = normalize(cameraPos - pos);
	} else {
		float2 uvR = (float2(pixel) + float2(1.5, 0.5)) / float2(width, height);
		float2 uvD = (float2(pixel) + float2(0.5, 1.5)) / float2(width, height);
		float4 wR = mul(invViewProj, float4(uvR.x * 2.0 - 1.0, 1.0 - uvR.y * 2.0, zR, 1.0));
		float4 wD = mul(invViewProj, float4(uvD.x * 2.0 - 1.0, 1.0 - uvD.y * 2.0, zD, 1.0));
		n = normalize(cross(wR.xyz / max(wR.w, 1e-6) - pos, wD.xyz / max(wD.w, 1e-6) - pos));
		if(dot(n, cameraPos - pos) < 0.0) {
			n = -n;
		}
	}
	if(aoRays == 0 || disc) {
		g_ao[pixel] = 1.0;
	} else {
		float occ = 0.0;
		uint base = pixel.x + pixel.y * width + frameIndex * 1973u;
		for(uint i = 0; i < aoRays; ++i) {
			RayDesc ao;
			ao.Origin = pos + n * 8.0;
			ao.Direction = hemisphere(n, base + i * 1013u);
			ao.TMin = 8.0;
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
		g_ao[pixel] = max(1.0 - occ / float(aoRays), 0.55);
	}

	if(shadowsOn == 0 || lightCount == 0) {
		g_shadow[pixel] = 1.0;
		return;
	}
	float sumVis = 0.0;
	float sumAttn = 0.0;
	for(uint i = 0; i < lightCount; ++i) {
		float4 a = g_lights[i * 2 + 0];
		float4 b = g_lights[i * 2 + 1];
		float3 lpos = a.xyz;
		float intensity = a.w;
		float fallstart = b.x;
		float fallend = b.y;
		float3 toL = lpos - pos;
		float d = length(toL);
		if(d < 1e-3) {
			continue;
		}
		float3 ldir = toL / d;
		float ndotl = saturate(dot(n, ldir));
		float span = max(fallend - fallstart, 1e-3);
		float fall = saturate((fallend - d) / span);
		float attn = intensity * fall * ndotl;
		if(attn <= 0.0) {
			continue;
		}
		float3 origin = pos + n * 4.0;
		float3 toRay = lpos - origin;
		float tmax = length(toRay);
		RayDesc sh;
		sh.Origin = origin;
		sh.Direction = toRay / max(tmax, 1e-3);
		sh.TMin = 2.0;
		sh.TMax = max(tmax - 12.0, 2.1);
		RayPayload shp;
		shp.t = 1e7;
		shp.n = 0.xxx;
		TraceRay(g_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
		         0xff, 0, 1, 0, sh, shp);
		float vis = saturate((shp.t - (sh.TMax - 12.0)) / 12.0);
		sumVis += vis * attn;
		sumAttn += attn;
	}
	g_shadow[pixel] = (sumAttn > 1e-5) ? (sumVis / sumAttn) : 1.0;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload p, BuiltInTriangleIntersectionAttributes /* attr */) {
	p.t = RayTCurrent();
	p.n = 0.xxx;
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
	[unroll] for(int y = -4; y <= 4; ++y) {
		[unroll] for(int x = -4; x <= 4; ++x) {
			if(abs(x) > radius || abs(y) > radius) {
				continue;
			}
			int2 p = pix + int2(x, y);
			float z = depthTex.Load(int3(p, 0)).r;
			if(abs(z - zCenter) < 0.002) {
				s += tex.Load(int3(p, 0)).r;
				wsum += 1.0;
			}
		}
	}
	if(wsum < 1e-5) {
		return tex.Load(int3(pix, 0)).r;
	}
	return s / wsum;
}
float4 PSMain(VSOut i) : SV_Target {
	float3 c = colorTex.SampleLevel(samp, i.uv, 0).rgb;
	int2 pix = int2(i.pos.xy);
	float zCenter = depthTex.Load(int3(pix, 0)).r;
	float ao = lerp(1.0, bilateral(aoTex, pix, 2, zCenter), 0.35);
	float sh = lerp(0.50, 1.0, bilateral(shadowTex, pix, 4, zCenter));
	return float4(c * ao * sh, 1);
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
	m_colorCopy.reset();
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
	srv.NumDescriptors = 4;
	srv.BaseShaderRegister = 0;
	D3D12_DESCRIPTOR_RANGE uav {};
	uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uav.NumDescriptors = 2;
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
	colorSrv.NumDescriptors = 4;
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
		LogError << "RTAO: composite VS failed";
		return false;
	}
	if(FAILED(D3DCompile(kComposite, std::strlen(kComposite), "rtao_composite", nullptr, nullptr,
	                     "PSMain", "ps_5_0", 0, 0, ps.put(), cerr.put()))) {
		LogError << "RTAO: composite PS failed";
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
	if(width == m_width && height == m_height && m_ao && m_shadow) {
		return;
	}
	m_ao.reset();
	m_shadow.reset();
	m_colorCopy.reset();
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
		return;
	}
	m_width = width;
	m_height = height;
	m_aoIsUav = true;
	if(m_heap) {
		updateDescriptors();
	}
}

bool D3D12Rtao::ensureTargets(int width, int height) {
	if(width != m_width || height != m_height || !m_ao) {
		resize(width, height);
	}
	return m_ao && m_shadow && m_colorCopy;
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
	ID3D12Resource * verts = nullptr;
	UINT nverts = 0;
	if(m_vertDefault && !m_positions.empty()) {
		verts = m_vertDefault.Get();
		nverts = UINT(m_positions.size());
	} else if(m_roomDefault && !m_roomPositions.empty()) {
		verts = m_roomDefault.Get();
		nverts = UINT(m_roomPositions.size());
	}
	if(verts) {
		D3D12_SHADER_RESOURCE_VIEW_DESC vb {};
		vb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		vb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		vb.Buffer.FirstElement = 0;
		vb.Buffer.NumElements = nverts;
		vb.Buffer.StructureByteStride = sizeof(Pos);
		m_device->CreateShaderResourceView(verts, &vb, slot(1));
	}
	if(m_lights) {
		D3D12_SHADER_RESOURCE_VIEW_DESC lb {};
		lb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		lb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		lb.Buffer.FirstElement = 0;
		lb.Buffer.NumElements = UINT(kMaxShadowLights * 2);
		lb.Buffer.StructureByteStride = sizeof(float) * 4;
		m_device->CreateShaderResourceView(m_lights.Get(), &lb, slot(2));
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
	uav.Format = DXGI_FORMAT_R8_UNORM;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_SHADER_RESOURCE_VIEW_DESC factorSrv {};
	factorSrv.Format = DXGI_FORMAT_R8_UNORM;
	factorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	factorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	factorSrv.Texture2D.MipLevels = 1;
	if(m_ao) {
		m_device->CreateUnorderedAccessView(m_ao.Get(), nullptr, &uav, slot(4));
		m_device->CreateShaderResourceView(m_ao.Get(), &factorSrv, slot(7));
	}
	if(m_shadow) {
		m_device->CreateUnorderedAccessView(m_shadow.Get(), nullptr, &uav, slot(5));
		m_device->CreateShaderResourceView(m_shadow.Get(), &factorSrv, slot(8));
	}
	if(m_colorCopy) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(m_colorCopy.Get(), &srv, slot(6));
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
	auto writeInst = [&](ID3D12Resource * blas) {
		inst[nInst].Transform[0][0] = 1.f;
		inst[nInst].Transform[1][1] = 1.f;
		inst[nInst].Transform[2][2] = 1.f;
		inst[nInst].InstanceMask = 0xff;
		inst[nInst].AccelerationStructure = blas->GetGPUVirtualAddress();
		nInst++;
	};
	if(haveRooms && m_roomBlas) {
		writeInst(m_roomBlas.Get());
	}
	if(haveDyn && m_blas) {
		writeInst(m_blas.Get());
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
                      int width, int height, int quality, bool shadows,
                      const GpuLight * lights, size_t lightCount, std::uint64_t rtvPtr) {
	if(!m_ready || !list || !backbuffer || !depth) {
		return false;
	}
	if(quality <= 0 && !shadows) {
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
	depthPs.ptr += SIZE_T(9) * m_descriptorSize;
	m_device->CreateShaderResourceView(depth, &depthSrv, depthPs);
	transition(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if(!m_aoIsUav) {
		transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
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
	
	const glm::mat4x4 inv = glm::inverse(proj * view);
	
	struct Constants {
		float invViewProj[16];
		float cameraPos[3];
		float radius;
		UINT aoRays;
		UINT frameIndex;
		UINT width;
		UINT height;
		UINT lightCount;
		UINT shadowsOn;
	} cb {};
	std::memcpy(cb.invViewProj, glm::value_ptr(inv), sizeof(cb.invViewProj));
	cb.cameraPos[0] = cam.x;
	cb.cameraPos[1] = cam.y;
	cb.cameraPos[2] = cam.z;
	cb.radius = (quality >= 2) ? 48.f : 32.f;
	cb.aoRays = (quality <= 0) ? 0u : ((quality >= 2) ? 16u : 8u);
	cb.frameIndex = m_frameIndex++;
	cb.width = UINT(m_width);
	cb.height = UINT(m_height);
	cb.lightCount = UINT(nlights);
	cb.shadowsOn = shadows ? 1u : 0u;
	
	ID3D12DescriptorHeap * heaps[] = { m_heap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(m_rtRoot.Get());
	list->SetComputeRoot32BitConstants(0, kRootConstants, &cb, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
	list->SetComputeRootDescriptorTable(1, gpu);
	D3D12_GPU_DESCRIPTOR_HANDLE uav = gpu;
	uav.ptr += SIZE_T(4) * m_descriptorSize;
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
	
	transition(list, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(list, m_colorCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	           D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(m_colorCopy.Get(), backbuffer);
	transition(list, m_colorCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	transition(list, m_ao.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	transition(list, m_shadow.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_aoIsUav = false;
	
	D3D12_CPU_DESCRIPTOR_HANDLE rtv {};
	rtv.ptr = SIZE_T(rtvPtr);
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	list->SetGraphicsRootSignature(m_compositeRoot.Get());
	list->SetPipelineState(m_compositePso.Get());
	D3D12_GPU_DESCRIPTOR_HANDLE color = gpu;
	color.ptr += SIZE_T(6) * m_descriptorSize;
	list->SetGraphicsRootDescriptorTable(0, color);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->DrawInstanced(3, 1, 0, 0);
	transition(list, depth,
	           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	           D3D12_RESOURCE_STATE_DEPTH_WRITE);
	return true;
}

#endif // ARX_HAVE_D3D12
