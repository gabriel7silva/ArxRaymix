/*
 * Arx Raymix — Streamline manual hook (DLSS / DLSS-G / Reflex, no silent fallback).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/dxr/D3D12Streamline.h"

#if ARX_HAVE_D3D12

#include <algorithm>
#include <cstring>
#include <string>
#include <cwchar>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#ifdef interface
#undef interface
#endif

#include <windows.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "io/log/Logger.h"
#include "platform/Platform.h"

#if ARX_HAVE_STREAMLINE

#include <sl_struct.h>
#include <sl_consts.h>
#include <sl_core_types.h>
#include <sl_result.h>
#include <sl_version.h>
#define SL_FEATURE_FUN_IMPORT(feature, func)
#define SL_FEATURE_FUN_IMPORT_STATIC(feature, func) \
	static PFun_##func * s_##func = nullptr; \
	if(!s_##func) return sl::Result::eErrorNotInitialized;
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>

#endif

namespace {

#if ARX_HAVE_STREAMLINE

using PFun_slInit = sl::Result(const sl::Preferences &, uint64_t);
using PFun_slShutdown = sl::Result();
using PFun_slSetD3DDevice = sl::Result(void *);
using PFun_slIsFeatureSupported = sl::Result(sl::Feature, const sl::AdapterInfo &);
using PFun_slEvaluateFeature = sl::Result(sl::Feature, const sl::FrameToken &,
                                          const sl::BaseStructure **, uint32_t, sl::CommandBuffer *);
using PFun_slSetTagForFrame = sl::Result(const sl::FrameToken &, const sl::ViewportHandle &,
                                         const sl::ResourceTag *, uint32_t, sl::CommandBuffer *);
using PFun_slSetConstants = sl::Result(const sl::Constants &, const sl::FrameToken &,
                                       const sl::ViewportHandle &);
using PFun_slGetNewFrameToken = sl::Result(sl::FrameToken *&, const uint32_t *);
using PFun_slGetFeatureFunction = sl::Result(sl::Feature, const char *, void *&);
using PFun_slFreeResources = sl::Result(sl::Feature, const sl::ViewportHandle &);
using PFun_slUpgradeInterface = sl::Result(void **);

PFun_slInit * pslInit = nullptr;
PFun_slShutdown * pslShutdown = nullptr;
PFun_slSetD3DDevice * pslSetD3DDevice = nullptr;
PFun_slIsFeatureSupported * pslIsFeatureSupported = nullptr;
PFun_slEvaluateFeature * pslEvaluateFeature = nullptr;
PFun_slSetTagForFrame * pslSetTagForFrame = nullptr;
PFun_slSetConstants * pslSetConstants = nullptr;
PFun_slGetNewFrameToken * pslGetNewFrameToken = nullptr;
PFun_slGetFeatureFunction * pslGetFeatureFunction = nullptr;
PFun_slFreeResources * pslFreeResources = nullptr;
PFun_slUpgradeInterface * pslUpgradeInterface = nullptr;

const char * resultName(sl::Result r) {
	switch(r) {
		case sl::Result::eOk: return "ok";
		case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
		case sl::Result::eErrorOSOutOfDate: return "OS out of date";
		case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter";
		case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
		case sl::Result::eErrorNoPlugins: return "no plugins";
		case sl::Result::eErrorNGXFailed: return "NGX failed";
		case sl::Result::eErrorFeatureNotSupported: return "feature not supported";
		case sl::Result::eErrorFeatureFailedToLoad: return "feature failed to load";
		case sl::Result::eErrorFeatureMissingDependency: return "missing dependency";
		case sl::Result::eErrorNotInitialized: return "not initialized";
		case sl::Result::eErrorMissingInputParameter: return "missing input";
		case sl::Result::eErrorMissingConstants: return "missing constants";
		default: return "error";
	}
}

void slLog(sl::LogType type, const char * msg) {
	if(!msg) {
		return;
	}
	if(type == sl::LogType::eError) {
		LogError << "Streamline: " << msg;
	} else if(type == sl::LogType::eWarn) {
		LogWarning << "Streamline: " << msg;
	}
}

void toSl(sl::float4x4 & out, const glm::mat4x4 & m) {
	// glm m[i] is column i, which is row i of the row-vector matrix Streamline wants.
	for(int i = 0; i < 4; ++i) {
		out.setRow(uint32_t(i), sl::float4(m[i][0], m[i][1], m[i][2], m[i][3]));
	}
}

void slTransition(ID3D12GraphicsCommandList * list, ID3D12Resource * res,
                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER b {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}

bool makeTex(ID3D12Device * device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
             D3D12_RESOURCE_STATES state, ID3D12Resource ** out) {
	D3D12_RESOURCE_DESC d {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = format;
	d.SampleDesc.Count = 1;
	d.Flags = flags;
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state,
	                                                 nullptr, IID_PPV_ARGS(out)));
}

const char * kGbuffer = R"(
cbuffer Cb : register(b0) {
	float4x4 invViewProj;
	float4x4 prevViewProj;
	float4x4 view;
	float2 renderSize;
	float2 jitterNdc;
	float3 cameraPos;
	float hasMasks;
};
Texture2D colorTex : register(t0);
Texture2D depthTex : register(t1);
Texture2D albedoTex : register(t2);
Texture2D waterTex : register(t3);
Texture2D metalTex : register(t4);
Texture2D waterDepthTex : register(t5);
struct PSOut {
	float2 mvec : SV_Target0;
	float4 nrmR : SV_Target1;
	float4 albedo : SV_Target2;
	float4 specA : SV_Target3;
	float hit : SV_Target4;
};
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 reconstructN(float3 world, float2 uv) {
	int2 pix = int2(uv * renderSize);
	float zL = depthTex.Load(int3(pix + int2(-1, 0), 0)).r;
	float zR = depthTex.Load(int3(pix + int2(1, 0), 0)).r;
	float zU = depthTex.Load(int3(pix + int2(0, -1), 0)).r;
	float zD = depthTex.Load(int3(pix + int2(0, 1), 0)).r;
	float zC = depthTex.Load(int3(pix, 0)).r;
	int2 dH = (abs(zL - zC) < abs(zR - zC)) ? int2(-1, 0) : int2(1, 0);
	int2 dV = (abs(zU - zC) < abs(zD - zC)) ? int2(0, -1) : int2(0, 1);
	float zH = depthTex.Load(int3(pix + dH, 0)).r;
	float zV = depthTex.Load(int3(pix + dV, 0)).r;
	float2 uvH = uv + float2(dH) / renderSize;
	float2 uvV = uv + float2(dV) / renderSize;
	float4 wH = mul(invViewProj, float4(uvH.x * 2.0 - 1.0 - jitterNdc.x,
	                                    1.0 - uvH.y * 2.0 - jitterNdc.y, zH, 1.0));
	float4 wV = mul(invViewProj, float4(uvV.x * 2.0 - 1.0 - jitterNdc.x,
	                                    1.0 - uvV.y * 2.0 - jitterNdc.y, zV, 1.0));
	float3 n = normalize(cross(wH.xyz / max(wH.w, 1e-4) - world,
	                           wV.xyz / max(wV.w, 1e-4) - world));
	if(dot(n, cameraPos - world) < 0.0) {
		n = -n;
	}
	return n;
}
PSOut PSMain(float4 pos : SV_Position) {
	int2 pix = int2(pos.xy);
	float z = depthTex.Load(int3(pix, 0)).r;
	float2 uv = (pos.xy + float2(0.5, 0.5)) / renderSize;
	float ndcX = uv.x * 2.0 - 1.0 - jitterNdc.x;
	float ndcY = 1.0 - uv.y * 2.0 - jitterNdc.y;
	float4 worldH = mul(invViewProj, float4(ndcX, ndcY, z, 1.0));
	float3 world = worldH.xyz / max(worldH.w, 1e-4);
	float3 base = albedoTex.Load(int3(pix, 0)).rgb;
	float water = 0.0;
	float metal = 0.0;
	if(hasMasks > 0.5) {
		water = waterTex.Load(int3(pix, 0)).r;
		metal = metalTex.Load(int3(pix, 0)).r;
		if(hasMasks > 1.5) {
			float zw = waterDepthTex.Load(int3(pix, 0)).r;
			if(zw > 0.0 && zw < 0.99999 && z + 0.001 < zw) {
				water = 0.0;
			}
		}
	}
	float3 n = reconstructN(world, uv);
	float3 nView = mul((float3x3)view, n);
	float rough = 0.55;
	float3 f0 = float3(0.04, 0.04, 0.04);
	if(water > 0.5) {
		rough = 0.08;
		f0 = float3(0.04, 0.04, 0.04);
	} else if(metal > 0.5) {
		rough = 0.28;
		f0 = float3(0.18, 0.18, 0.18);
	}
	float4 prevH = mul(prevViewProj, float4(world, 1.0));
	float2 prevNdc = prevH.xy / max(prevH.w, 1e-4);
	PSOut o;
	o.mvec = (float2(ndcX, ndcY) - prevNdc) * 0.5;
	o.nrmR = float4(nView, rough);
	o.albedo = float4(base, 1.0);
	o.specA = float4(f0, 1.0);
	o.hit = 0.0;
	if(z <= 0.0 || z >= 0.99999) {
		o.mvec = float2(0, 0);
		o.nrmR = float4(0, 0, 1, 1);
		o.albedo = float4(0, 0, 0, 1);
		o.specA = float4(0, 0, 0, 1);
		o.hit = 0.0;
	}
	return o;
}
)";

// sRGB transfer, both directions. The raster writes gamma-encoded colour, which is what the art
// was authored in and what the game has always displayed. Ray Reconstruction is the one consumer
// that needs linear radiance instead, so the conversion happens on its path and is undone on the
// way out. Nothing else in the renderer changes colour space.
// Gamma scene colour to linear fp16, for Ray Reconstruction only. Eight bits of gamma carry more
// detail in the darks than eight bits of linear would, so nothing is lost going up to float here.
const char * kLinearize = R"(
Texture2D srcTex : register(t0);
float3 srgbToLinear(float3 c) {
	// step and lerp, not select: these are compiled by FXC as ps_5_0, which predates HLSL 2021.
	// See INV-03 — the wrong dialect here makes the whole pass fail to build and vanish.
	float3 lo = c / 12.92;
	float3 hi = pow(max(c + 0.055, 1e-5) / 1.055, 2.4);
	return lerp(hi, lo, step(c, 0.04045));
}
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
	return float4(srgbToLinear(saturate(srcTex.Load(int3(pos.xy, 0)).rgb)), 1);
}
)";

const char * kTonemap = R"(
cbuffer Cb : register(b0) { uint encodeGamma; uint pad0; uint pad1; uint pad2; };
Texture2D hdrTex : register(t0);
float3 linearToSrgb(float3 c) {
	c = max(c, 0.0);
	float3 lo = c * 12.92;
	float3 hi = 1.055 * pow(max(c, 1e-8), 1.0 / 2.4) - 0.055;
	return lerp(hi, lo, step(c, 0.0031308));
}
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
	float3 h = hdrTex.Load(int3(pos.xy, 0)).rgb;
	// Only the Ray Reconstruction path hands over linear data. Encoding unconditionally would
	// gamma the plain DLSS output a second time and wash the whole image out.
	if(encodeGamma != 0u) {
		h = linearToSrgb(h);
	}
	return float4(saturate(h), 1);
}
)";

const char * kBlit = R"(
cbuffer Cb : register(b0) { float2 srcSize; float2 dstSize; };
Texture2D src : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
	float2 uv = pos.xy / max(dstSize, float2(1, 1));
	int2 p = int2(uv * srcSize);
	return float4(src.Load(int3(p, 0)).rgb, 1);
}
)";

#endif // ARX_HAVE_STREAMLINE

} // namespace

struct D3D12Streamline::Gpu {
#if ARX_HAVE_STREAMLINE
	ID3D12Resource * mvec = nullptr;
	ID3D12Resource * nrm = nullptr;
	ID3D12Resource * albedo = nullptr;
	ID3D12Resource * specA = nullptr;
	ID3D12Resource * hdrOut = nullptr;
	// Scene colour converted to linear, the only buffer in this file that is not gamma-encoded.
	ID3D12Resource * linearIn = nullptr;
	ID3D12Resource * hit = nullptr;
	ID3D12Resource * hudless = nullptr;
	ID3D12DescriptorHeap * rtvHeap = nullptr;
	ID3D12DescriptorHeap * srvHeap = nullptr;
	ID3D12PipelineState * gbufferPso = nullptr;
	ID3D12PipelineState * tonemapPso = nullptr;
	ID3D12PipelineState * linearPso = nullptr;
	ID3D12PipelineState * blitPso = nullptr;
	ID3D12RootSignature * gbufferRoot = nullptr;
	ID3D12RootSignature * tonemapRoot = nullptr;
	ID3D12RootSignature * blitRoot = nullptr;
	UINT rtvSize = 0;
	UINT srvSize = 0;
	
	void release() {
		auto drop = [](IUnknown * p) {
			if(p) {
				p->Release();
			}
		};
		drop(mvec); mvec = nullptr;
		drop(nrm); nrm = nullptr;
		drop(albedo); albedo = nullptr;
		drop(specA); specA = nullptr;
		drop(hdrOut); hdrOut = nullptr;
		drop(linearIn); linearIn = nullptr;
		drop(hit); hit = nullptr;
		drop(hudless); hudless = nullptr;
		drop(rtvHeap); rtvHeap = nullptr;
		drop(srvHeap); srvHeap = nullptr;
		drop(gbufferPso); gbufferPso = nullptr;
		drop(tonemapPso); tonemapPso = nullptr;
		drop(linearPso); linearPso = nullptr;
		drop(blitPso); blitPso = nullptr;
		drop(gbufferRoot); gbufferRoot = nullptr;
		drop(tonemapRoot); tonemapRoot = nullptr;
		drop(blitRoot); blitRoot = nullptr;
	}
#endif
};

D3D12Streamline::D3D12Streamline() = default;

D3D12Streamline::~D3D12Streamline() {
	shutdown();
}

bool D3D12Streamline::loadLibrary() {
#if !ARX_HAVE_STREAMLINE
	return false;
#else
	if(m_dll) {
		return true;
	}
	wchar_t exe[MAX_PATH] {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir(exe);
	const size_t slash = dir.find_last_of(L"\\/");
	if(slash != std::wstring::npos) {
		dir.resize(slash + 1);
	}
	const std::wstring path = dir + L"sl.interposer.dll";
	m_dll = LoadLibraryW(path.c_str());
	if(!m_dll) {
		LogInfo << "Streamline: sl.interposer.dll not next to arx.exe — RR stays off";
		return false;
	}
	auto proc = [&](const char * name) {
		return GetProcAddress(static_cast<HMODULE>(m_dll), name);
	};
	pslInit = reinterpret_cast<PFun_slInit *>(proc("slInit"));
	pslShutdown = reinterpret_cast<PFun_slShutdown *>(proc("slShutdown"));
	pslSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice *>(proc("slSetD3DDevice"));
	pslIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported *>(proc("slIsFeatureSupported"));
	pslEvaluateFeature = reinterpret_cast<PFun_slEvaluateFeature *>(proc("slEvaluateFeature"));
	pslSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame *>(proc("slSetTagForFrame"));
	pslSetConstants = reinterpret_cast<PFun_slSetConstants *>(proc("slSetConstants"));
	pslGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken *>(proc("slGetNewFrameToken"));
	pslGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction *>(proc("slGetFeatureFunction"));
	pslFreeResources = reinterpret_cast<PFun_slFreeResources *>(proc("slFreeResources"));
	pslUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface *>(proc("slUpgradeInterface"));
	if(!pslInit || !pslShutdown || !pslSetD3DDevice || !pslIsFeatureSupported || !pslEvaluateFeature
	   || !pslSetTagForFrame || !pslSetConstants || !pslGetNewFrameToken || !pslGetFeatureFunction
	   || !pslUpgradeInterface) {
		LogError << "Streamline: sl.interposer.dll is missing exports";
		FreeLibrary(static_cast<HMODULE>(m_dll));
		m_dll = nullptr;
		return false;
	}
	return true;
#endif
}

bool D3D12Streamline::init() {
#if !ARX_HAVE_STREAMLINE
	return false;
#else
	if(m_inited) {
		return true;
	}
	if(!loadLibrary()) {
		return false;
	}
	static sl::Feature features[] = {
		sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G,
		sl::kFeatureReflex, sl::kFeaturePCL
	};
	static wchar_t pluginDir[MAX_PATH] {};
	GetModuleFileNameW(nullptr, pluginDir, MAX_PATH);
	if(wchar_t * slash = wcsrchr(pluginDir, L'\\')) {
		slash[1] = 0;
	}
	static const wchar_t * paths[] = { pluginDir };
	static wchar_t logDir[MAX_PATH] {};
	GetModuleFileNameW(nullptr, logDir, MAX_PATH);
	if(wchar_t * slash = wcsrchr(logDir, L'\\')) {
		slash[1] = 0;
	}
	sl::Preferences pref {};
	pref.showConsole = false;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.pathsToPlugins = paths;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = logDir;
	pref.logMessageCallback = slLog;
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
	             | sl::PreferenceFlags::eUseManualHooking
	             | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = 5;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.4";
	pref.projectId = "a7b3c91e-4d2f-4e18-9c6a-0c4035c19df0";
	pref.renderAPI = sl::RenderAPI::eD3D12;
	const sl::Result r = pslInit(pref, sl::kSDKVersion);
	if(r != sl::Result::eOk) {
		LogError << "Streamline: slInit failed (" << resultName(r) << ")";
		return false;
	}
	m_inited = true;
	LogInfo << "Streamline: slInit ok (manual hook, plugins next to exe)";
	return true;
#endif
}

bool D3D12Streamline::upgradeInterface(void ** iface) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(iface);
	return false;
#else
	if(!m_inited || !pslUpgradeInterface || !iface || !*iface) {
		return false;
	}
	const sl::Result r = pslUpgradeInterface(iface);
	if(r != sl::Result::eOk) {
		LogError << "Streamline: slUpgradeInterface failed (" << resultName(r) << ")";
		return false;
	}
	return true;
#endif
}

bool D3D12Streamline::setDevice(ID3D12Device * device, IDXGIAdapter1 * adapter) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(device);
	ARX_UNUSED(adapter);
	return false;
#else
	if(!m_inited || !device || !pslSetD3DDevice) {
		return false;
	}
	m_device = device;
	const sl::Result set = pslSetD3DDevice(device);
	if(set != sl::Result::eOk) {
		LogError << "Streamline: slSetD3DDevice failed (" << resultName(set) << ")";
		return false;
	}
	sl::AdapterInfo info {};
	DXGI_ADAPTER_DESC1 desc {};
	if(adapter && SUCCEEDED(adapter->GetDesc1(&desc))) {
		info.deviceLUID = reinterpret_cast<uint8_t *>(&desc.AdapterLuid);
		info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
	}
	const sl::Result dlss = pslIsFeatureSupported(sl::kFeatureDLSS, info);
	const sl::Result rr = pslIsFeatureSupported(sl::kFeatureDLSS_RR, info);
	const sl::Result fg = pslIsFeatureSupported(sl::kFeatureDLSS_G, info);
	const sl::Result reflex = pslIsFeatureSupported(sl::kFeatureReflex, info);
	m_dlss = dlss == sl::Result::eOk;
	m_rr = rr == sl::Result::eOk;
	m_fg = fg == sl::Result::eOk;
	m_reflex = reflex == sl::Result::eOk;
	LogInfo << "Streamline: DLSS=" << (m_dlss ? "yes" : resultName(dlss))
	        << " DLSS-RR=" << (m_rr ? "yes" : resultName(rr))
	        << " DLSS-G=" << (m_fg ? "yes" : resultName(fg))
	        << " Reflex=" << (m_reflex ? "yes" : resultName(reflex));
	m_ready = m_dlss || m_rr || (m_fg && m_reflex);
	m_optValid = false;
	if(!m_gpu) {
		m_gpu = new Gpu();
	}
	return m_ready;
#endif
}

void D3D12Streamline::resetState() {
	m_ready = false;
	m_dlss = false;
	m_rr = false;
	m_fg = false;
	m_reflex = false;
	m_fgOn = false;
	m_reflexOn = false;
	m_fgTagged = false;
	m_loggedOn = false;
	m_loggedOff = false;
	m_loggedRrSkip = false;
	m_rrLive = false;
	m_rrFailed = false;
	m_constantsSet = false;
	m_firstFrame = true;
	m_loggedFg = false;
	m_loggedUntagged = false;
	m_targetsFailed = false;
	m_token = nullptr;
	m_inW = 0;
	m_inH = 0;
	m_outW = 0;
	m_outH = 0;
	m_targetSet = 0;
	m_optValid = false;
	m_prevViewProj = glm::mat4x4(1.f);
	m_invPrevViewProj = glm::mat4x4(1.f);
	m_viewProj = glm::mat4x4(1.f);
	m_invViewProj = glm::mat4x4(1.f);
}

void D3D12Streamline::shutdown() {
#if ARX_HAVE_STREAMLINE
	if(m_inited && pslFreeResources) {
		const sl::ViewportHandle vp(0);
		if(m_rr) {
			pslFreeResources(sl::kFeatureDLSS_RR, vp);
		}
		if(m_dlss) {
			pslFreeResources(sl::kFeatureDLSS, vp);
		}
		if(m_fg) {
			pslFreeResources(sl::kFeatureDLSS_G, vp);
		}
	}
	releaseTargets();
	delete m_gpu;
	m_gpu = nullptr;
	if(m_inited && pslShutdown) {
		pslShutdown();
	}
	m_inited = false;
	m_device = nullptr;
	resetState();
	if(m_dll) {
		FreeLibrary(static_cast<HMODULE>(m_dll));
		m_dll = nullptr;
	}
	pslInit = nullptr;
	pslShutdown = nullptr;
	pslSetD3DDevice = nullptr;
	pslIsFeatureSupported = nullptr;
	pslEvaluateFeature = nullptr;
	pslSetTagForFrame = nullptr;
	pslSetConstants = nullptr;
	pslGetNewFrameToken = nullptr;
	pslGetFeatureFunction = nullptr;
	pslFreeResources = nullptr;
	pslUpgradeInterface = nullptr;
#endif
}

void D3D12Streamline::resize(int width, int height) {
	ARX_UNUSED(width);
	ARX_UNUSED(height);
	releaseTargets();
	m_firstFrame = true;
	m_rrLive = false;
	m_targetsFailed = false;
	m_optValid = false;
}

void D3D12Streamline::releaseTargets() {
#if ARX_HAVE_STREAMLINE
	if(m_gpu) {
		m_gpu->release();
	}
#endif
	m_inW = 0;
	m_inH = 0;
	m_outW = 0;
	m_outH = 0;
	m_targetSet = 0;
}

int D3D12Streamline::resolveDlssMode(int setting, int outputHeight) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(setting);
	ARX_UNUSED(outputHeight);
	return 0;
#else
	switch(setting) {
		case 1: return int(sl::DLSSMode::eDLAA);
		case 2: return int(sl::DLSSMode::eMaxQuality);
		case 3: return int(sl::DLSSMode::eBalanced);
		case 4: return int(sl::DLSSMode::eMaxPerformance);
		case 5:
			// SDK Ultra is ~33%. At 1080p that is 360p — oil-paint on this art.
			if(wasDowngraded(setting, outputHeight)) {
				return int(sl::DLSSMode::eMaxPerformance);
			}
			return int(sl::DLSSMode::eUltraPerformance);
		default: return int(sl::DLSSMode::eOff);
	}
#endif
}

bool D3D12Streamline::wasDowngraded(int setting, int outputHeight) {
	constexpr int kUltraMinHeight = 1440;
	return setting == 5 && outputHeight > 0 && outputHeight < kUltraMinHeight;
}

const char * D3D12Streamline::dlssModeName(int slMode) {
#if ARX_HAVE_STREAMLINE
	switch(sl::DLSSMode(slMode)) {
		case sl::DLSSMode::eMaxPerformance:   return "Performance";
		case sl::DLSSMode::eBalanced:         return "Balanced";
		case sl::DLSSMode::eMaxQuality:       return "Quality";
		case sl::DLSSMode::eUltraPerformance: return "UltraPerformance";
		case sl::DLSSMode::eUltraQuality:     return "UltraQuality";
		case sl::DLSSMode::eDLAA:             return "DLAA";
		default: break;
	}
#else
	ARX_UNUSED(slMode);
#endif
	return "Off";
}

int D3D12Streamline::dlaaMode() {
#if ARX_HAVE_STREAMLINE
	static_assert(int(sl::DLSSMode::eOff) == 0, "sl::DLSSMode::eOff");
	static_assert(int(sl::DLSSMode::eMaxPerformance) == 1, "sl::DLSSMode::eMaxPerformance");
	static_assert(int(sl::DLSSMode::eDLAA) == 6, "sl::DLSSMode::eDLAA");
	return int(sl::DLSSMode::eDLAA);
#else
	return 6;
#endif
}

bool D3D12Streamline::queryOptimalSize(int slMode, int outputW, int outputH, int & renderW, int & renderH) const {
	renderW = outputW;
	renderH = outputH;
	if(slMode <= 0 || outputW <= 0 || outputH <= 0) {
		return false;
	}
	if(m_optValid && m_optMode == slMode && m_optOutW == outputW && m_optOutH == outputH) {
		renderW = m_optInW;
		renderH = m_optInH;
		return true;
	}
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(slMode);
	return false;
#else
	PFun_slDLSSGetOptimalSettings * getOpt = nullptr;
	if(m_ready && pslGetFeatureFunction
	   && pslGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings",
	                            reinterpret_cast<void *&>(getOpt)) == sl::Result::eOk && getOpt) {
		sl::DLSSOptions opt {};
		opt.mode = sl::DLSSMode(slMode);
		opt.outputWidth = uint32_t(outputW);
		opt.outputHeight = uint32_t(outputH);
		sl::DLSSOptimalSettings settings {};
		if(getOpt(opt, settings) == sl::Result::eOk
		   && settings.optimalRenderWidth > 0 && settings.optimalRenderHeight > 0) {
			renderW = int(settings.optimalRenderWidth);
			renderH = int(settings.optimalRenderHeight);
			m_optMode = slMode;
			m_optOutW = outputW;
			m_optOutH = outputH;
			m_optInW = renderW;
			m_optInH = renderH;
			m_optValid = true;
			return true;
		}
	}
	float scale = 1.f;
	switch(sl::DLSSMode(slMode)) {
		case sl::DLSSMode::eUltraPerformance: scale = 0.33f; break;
		case sl::DLSSMode::eMaxPerformance:   scale = 0.50f; break;
		case sl::DLSSMode::eBalanced:         scale = 0.58f; break;
		case sl::DLSSMode::eMaxQuality:       scale = 0.67f; break;
		case sl::DLSSMode::eUltraQuality:     scale = 0.77f; break;
		default: break;
	}
	renderW = (std::max)(8, int(float(outputW) * scale + 0.5f));
	renderH = (std::max)(8, int(float(outputH) * scale + 0.5f));
	m_optMode = slMode;
	m_optOutW = outputW;
	m_optOutH = outputH;
	m_optInW = renderW;
	m_optInH = renderH;
	m_optValid = true;
	return true;
#endif
}

bool D3D12Streamline::ensureTargets(int inputW, int inputH, int outputW, int outputH, unsigned needed) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(inputW);
	ARX_UNUSED(inputH);
	ARX_UNUSED(outputW);
	ARX_UNUSED(outputH);
	ARX_UNUSED(needed);
	return false;
#else
	if(m_targetsFailed) {
		return false;
	}
	if(!m_device || !m_gpu || inputW <= 0 || inputH <= 0 || outputW <= 0 || outputH <= 0) {
		return false;
	}
	if(needed == 0) {
		needed = kTargetRr;
	}
	if(inputW == m_inW && inputH == m_inH && outputW == m_outW && outputH == m_outH
	   && (m_targetSet & needed) == needed && m_gpu->mvec) {
		return true;
	}
	releaseTargets();
	// Name the resource. "targets failed" on its own turns every allocation in this function into
	// a suspect, and the failure latches until the next resize, so it is not easy to catch twice.
	auto fail = [&](const char * what) -> bool {
		LogError << "Streamline: target creation failed: " << what
		         << " (" << inputW << "x" << inputH << " -> " << outputW << "x" << outputH
		         << ", needed=" << needed << ")";
		releaseTargets();
		m_targetsFailed = true;
		return false;
	};
	m_gpu->rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_gpu->srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_DESCRIPTOR_HEAP_DESC rtv {};
	rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv.NumDescriptors = 8;
	if(FAILED(m_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_gpu->rtvHeap)))) {
		return fail("target setup");
	}
	D3D12_DESCRIPTOR_HEAP_DESC srv {};
	srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	// 0-5 G-buffer, 6 blit, 7 tonemap, 8 the linearise pass. The tonemap used to sit on 5 and
	// collide with the G-buffer's water depth, which resolved at execution to whichever was
	// written last.
	srv.NumDescriptors = 10;
	srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(m_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_gpu->srvHeap)))) {
		return fail("target setup");
	}
	const UINT w = UINT(inputW);
	const UINT h = UINT(inputH);
	const UINT ow = UINT(outputW);
	const UINT oh = UINT(outputH);
	const D3D12_RESOURCE_FLAGS rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
	                                 | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const bool needGbuffer = (needed & kTargetGbuffer) != 0;
	const bool needHdrOut = (needed & kTargetHdrOut) != 0;
	const bool needHudless = (needed & kTargetHudless) != 0;
	if(!makeTex(m_device, w, h, DXGI_FORMAT_R16G16_FLOAT, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->mvec)) {
		return fail("mvec");
	}
	if(needGbuffer
	   && (!makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	                D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->nrm)
	       || !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	                   D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->albedo)
	       || !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	                   D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->specA)
	       || !makeTex(m_device, w, h, DXGI_FORMAT_R16_FLOAT, rt,
	                   D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->hit))) {
		return fail("gbuffer");
	}
	if(needHdrOut
	   && !makeTex(m_device, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, uav,
	               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &m_gpu->hdrOut)) {
		return fail("hdrOut");
	}
	// Input resolution, not output: this is what Ray Reconstruction reads, before it upscales.
	// It is created in the state it rests in — a shader resource — because linearizeSceneColour
	// runs every Ray Reconstruction frame and has to leave the resource the way it found it.
	if(needHdrOut
	   && !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &m_gpu->linearIn)) {
		return fail("linearIn");
	}
	if(needHudless
	   && !makeTex(m_device, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
	               D3D12_RESOURCE_STATE_COPY_DEST, &m_gpu->hudless)) {
		return fail("target setup");
	}
	D3D12_CPU_DESCRIPTOR_HANDLE r0 = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	auto rtvAt = [&](UINT i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = r0;
		h.ptr += SIZE_T(i) * m_gpu->rtvSize;
		return h;
	};
	m_device->CreateRenderTargetView(m_gpu->mvec, nullptr, rtvAt(0));
	if(needGbuffer) {
		m_device->CreateRenderTargetView(m_gpu->nrm, nullptr, rtvAt(1));
		m_device->CreateRenderTargetView(m_gpu->albedo, nullptr, rtvAt(2));
		m_device->CreateRenderTargetView(m_gpu->specA, nullptr, rtvAt(3));
		m_device->CreateRenderTargetView(m_gpu->hit, nullptr, rtvAt(4));
	}
	
	if(!needGbuffer && !needHdrOut) {
		m_inW = inputW;
		m_inH = inputH;
		m_outW = outputW;
		m_outH = outputH;
		m_targetSet = needed;
		return true;
	}
	
	D3D12_DESCRIPTOR_RANGE ranges[2] {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 6;
	ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER rp[2] {};
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[0].Constants.Num32BitValues = 56;
	rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 1;
	rp[1].DescriptorTable.pDescriptorRanges = ranges;
	rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC samp {};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rs {};
	rs.NumParameters = 2;
	rs.pParameters = rp;
	rs.NumStaticSamplers = 1;
	rs.pStaticSamplers = &samp;
	ID3DBlob * blob = nullptr;
	ID3DBlob * err = nullptr;
	ID3DBlob * vs = nullptr;
	ID3DBlob * ps = nullptr;
	ID3DBlob * cerr = nullptr;
	auto dropBlob = [](ID3DBlob *& b) {
		if(b) {
			b->Release();
			b = nullptr;
		}
	};
	if(needGbuffer) {
		if(FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
			dropBlob(err);
			return fail("target setup");
		}
		dropBlob(err);
		if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		                                        IID_PPV_ARGS(&m_gpu->gbufferRoot)))) {
			dropBlob(blob);
			return fail("target setup");
		}
		dropBlob(blob);
		if(FAILED(D3DCompile(kGbuffer, std::strlen(kGbuffer), "sl_gbuffer", nullptr, nullptr,
		                     "VSMain", "vs_5_0", 0, 0, &vs, &cerr))) {
			LogError << "Streamline: G-buffer VS failed"
			         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
			dropBlob(cerr);
			return fail("target setup");
		}
		dropBlob(cerr);
		if(FAILED(D3DCompile(kGbuffer, std::strlen(kGbuffer), "sl_gbuffer", nullptr, nullptr,
		                     "PSMain", "ps_5_0", 0, 0, &ps, &cerr))) {
			LogError << "Streamline: G-buffer PS failed"
			         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
			dropBlob(vs);
			dropBlob(cerr);
			return fail("target setup");
		}
		dropBlob(cerr);
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
		pd.pRootSignature = m_gpu->gbufferRoot;
		pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		pd.BlendState.IndependentBlendEnable = TRUE;
		for(UINT i = 0; i < 5; ++i) {
			pd.BlendState.RenderTarget[i].RenderTargetWriteMask = (i == 0)
				? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN
				: D3D12_COLOR_WRITE_ENABLE_ALL;
		}
		pd.BlendState.RenderTarget[4].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.DepthStencilState.DepthEnable = FALSE;
		pd.SampleMask = 0xffffffff;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 5;
		pd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
		pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		pd.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		pd.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		pd.RTVFormats[4] = DXGI_FORMAT_R16_FLOAT;
		pd.SampleDesc.Count = 1;
		if(FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_gpu->gbufferPso)))) {
			LogError << "Streamline: G-buffer PSO failed";
			dropBlob(vs);
			dropBlob(ps);
			return fail("target setup");
		}
		dropBlob(vs);
		dropBlob(ps);
	}
	
	if(needHdrOut) {
		D3D12_DESCRIPTOR_RANGE tr {};
		tr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		tr.NumDescriptors = 1;
		tr.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		D3D12_ROOT_PARAMETER tp {};
		tp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		tp.DescriptorTable.NumDescriptorRanges = 1;
		tp.DescriptorTable.pDescriptorRanges = &tr;
		tp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_PARAMETER tparams[2] {};
		tparams[0] = tp;
		tparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		tparams[1].Constants.Num32BitValues = 4;
		tparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC trs {};
		trs.NumParameters = 2;
		trs.pParameters = tparams;
		if(FAILED(D3D12SerializeRootSignature(&trs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
			dropBlob(err);
			return fail("target setup");
		}
		dropBlob(err);
		if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		                                        IID_PPV_ARGS(&m_gpu->tonemapRoot)))) {
			dropBlob(blob);
			return fail("target setup");
		}
		dropBlob(blob);
		if(FAILED(D3DCompile(kTonemap, std::strlen(kTonemap), "sl_tonemap", nullptr, nullptr,
		                     "VSMain", "vs_5_0", 0, 0, &vs, &cerr))) {
			LogError << "Streamline: tonemap VS failed: "
			         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
			dropBlob(cerr);
			return fail("tonemap VS");
		}
		dropBlob(cerr);
		if(FAILED(D3DCompile(kTonemap, std::strlen(kTonemap), "sl_tonemap", nullptr, nullptr,
		                     "PSMain", "ps_5_0", 0, 0, &ps, &cerr))) {
			LogError << "Streamline: tonemap PS failed: "
			         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
			dropBlob(vs);
			dropBlob(cerr);
			return fail("tonemap PS");
		}
		dropBlob(cerr);
		D3D12_GRAPHICS_PIPELINE_STATE_DESC td {};
		td.pRootSignature = m_gpu->tonemapRoot;
		td.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		td.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		td.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		td.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		td.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		td.RasterizerState.DepthClipEnable = TRUE;
		td.DepthStencilState.DepthEnable = FALSE;
		td.SampleMask = 0xffffffff;
		td.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		td.NumRenderTargets = 1;
		td.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		{
			// Same root signature and same full-screen triangle; only the pixel shader and the
			// target format differ.
			ID3DBlob * lvs = nullptr;
			ID3DBlob * lps = nullptr;
			if(SUCCEEDED(D3DCompile(kLinearize, std::strlen(kLinearize), "sl_linearize", nullptr,
			                        nullptr, "VSMain", "vs_5_0", 0, 0, &lvs, &cerr))) {
				dropBlob(cerr);
				if(SUCCEEDED(D3DCompile(kLinearize, std::strlen(kLinearize), "sl_linearize", nullptr,
				                        nullptr, "PSMain", "ps_5_0", 0, 0, &lps, &cerr))) {
					D3D12_GRAPHICS_PIPELINE_STATE_DESC ld = td;
					ld.VS = { lvs->GetBufferPointer(), lvs->GetBufferSize() };
					ld.PS = { lps->GetBufferPointer(), lps->GetBufferSize() };
					ld.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
					if(FAILED(m_device->CreateGraphicsPipelineState(&ld,
					                                               IID_PPV_ARGS(&m_gpu->linearPso)))) {
						LogError << "Streamline: linearize PSO failed — RR keeps gamma input";
					}
					dropBlob(lps);
				}
				dropBlob(cerr);
				dropBlob(lvs);
			} else {
				dropBlob(cerr);
			}
		}
		if(FAILED(m_device->CreateGraphicsPipelineState(&td, IID_PPV_ARGS(&m_gpu->tonemapPso)))) {
			LogError << "Streamline: tonemap PSO failed";
			dropBlob(vs);
			dropBlob(ps);
			return fail("target setup");
		}
		dropBlob(vs);
		dropBlob(ps);
		
		D3D12_DESCRIPTOR_RANGE br {};
		br.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		br.NumDescriptors = 1;
		br.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		D3D12_ROOT_PARAMETER bp[2] {};
		bp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		bp[0].Constants.Num32BitValues = 4;
		bp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		bp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		bp[1].DescriptorTable.NumDescriptorRanges = 1;
		bp[1].DescriptorTable.pDescriptorRanges = &br;
		bp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC brs {};
		brs.NumParameters = 2;
		brs.pParameters = bp;
		if(FAILED(D3D12SerializeRootSignature(&brs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
			dropBlob(err);
			return fail("target setup");
		}
		dropBlob(err);
		if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		                                        IID_PPV_ARGS(&m_gpu->blitRoot)))) {
			dropBlob(blob);
			return fail("target setup");
		}
		dropBlob(blob);
		if(FAILED(D3DCompile(kBlit, std::strlen(kBlit), "sl_blit", nullptr, nullptr,
		                     "VSMain", "vs_5_0", 0, 0, &vs, &cerr))) {
			dropBlob(cerr);
			return fail("target setup");
		}
		dropBlob(cerr);
		if(FAILED(D3DCompile(kBlit, std::strlen(kBlit), "sl_blit", nullptr, nullptr,
		                     "PSMain", "ps_5_0", 0, 0, &ps, &cerr))) {
			LogError << "Streamline: tonemap PS failed: "
			         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
			dropBlob(vs);
			dropBlob(cerr);
			return fail("tonemap PS");
		}
		dropBlob(cerr);
		D3D12_GRAPHICS_PIPELINE_STATE_DESC bd = td;
		bd.pRootSignature = m_gpu->blitRoot;
		bd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		bd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		if(FAILED(m_device->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&m_gpu->blitPso)))) {
			LogError << "Streamline: blit PSO failed";
			dropBlob(vs);
			dropBlob(ps);
			return fail("target setup");
		}
		dropBlob(vs);
		dropBlob(ps);
	}
	m_inW = inputW;
	m_inH = inputH;
	m_outW = outputW;
	m_outH = outputH;
	m_targetSet = needed;
	return true;
#endif
}

bool D3D12Streamline::rasterGbuffers(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	return false;
#else
	if(!frame.list || !m_gpu || !m_gpu->gbufferPso || !frame.color || !frame.depth
	   || !frame.albedoSrc) {
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv {};
	colorSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	colorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	colorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	colorSrv.Texture2D.MipLevels = 1;
	D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv {};
	depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSrv.Texture2D.MipLevels = 1;
	D3D12_SHADER_RESOURCE_VIEW_DESC maskSrv {};
	maskSrv.Format = DXGI_FORMAT_R8_UNORM;
	maskSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	maskSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	maskSrv.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_gpu->srvHeap->GetCPUDescriptorHandleForHeapStart();
	auto srvAt = [&](UINT i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
		h.ptr += SIZE_T(i) * m_gpu->srvSize;
		return h;
	};
	m_device->CreateShaderResourceView(frame.color, &colorSrv, srvAt(0));
	m_device->CreateShaderResourceView(frame.depth, &depthSrv, srvAt(1));
	m_device->CreateShaderResourceView(frame.albedoSrc, &colorSrv, srvAt(2));
	const bool masksOk = frame.masksReady && frame.waterMask && frame.metalMask;
	ID3D12Resource * water = masksOk ? frame.waterMask : frame.albedoSrc;
	ID3D12Resource * metal = masksOk ? frame.metalMask : frame.albedoSrc;
	m_device->CreateShaderResourceView(water, masksOk ? &maskSrv : &colorSrv, srvAt(3));
	m_device->CreateShaderResourceView(metal, masksOk ? &maskSrv : &colorSrv, srvAt(4));
	D3D12_SHADER_RESOURCE_VIEW_DESC depthMaskSrv {};
	depthMaskSrv.Format = DXGI_FORMAT_R32_FLOAT;
	depthMaskSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthMaskSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthMaskSrv.Texture2D.MipLevels = 1;
	const bool waterDepthOk = masksOk && frame.waterDepth;
	m_device->CreateShaderResourceView(waterDepthOk ? frame.waterDepth : frame.albedoSrc,
	                                   waterDepthOk ? &depthMaskSrv : &colorSrv, srvAt(5));
	
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	slTransition(frame.list, frame.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	
	D3D12_CPU_DESCRIPTOR_HANDLE rts[5];
	D3D12_CPU_DESCRIPTOR_HANDLE r0 = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for(UINT i = 0; i < 5; ++i) {
		rts[i] = r0;
		rts[i].ptr += SIZE_T(i) * m_gpu->rtvSize;
	}
	frame.list->OMSetRenderTargets(5, rts, FALSE, nullptr);
	D3D12_VIEWPORT vp {};
	vp.Width = float(frame.width);
	vp.Height = float(frame.height);
	vp.MaxDepth = 1.f;
	D3D12_RECT sc { 0, 0, LONG(frame.width), LONG(frame.height) };
	frame.list->RSSetViewports(1, &vp);
	frame.list->RSSetScissorRects(1, &sc);
	frame.list->SetGraphicsRootSignature(m_gpu->gbufferRoot);
	frame.list->SetPipelineState(m_gpu->gbufferPso);
	frame.list->SetDescriptorHeaps(1, &m_gpu->srvHeap);
	frame.list->SetGraphicsRootDescriptorTable(1, m_gpu->srvHeap->GetGPUDescriptorHandleForHeapStart());
	m_viewProj = frame.proj * frame.view;
	m_invViewProj = glm::inverse(m_viewProj);
	float cb[56] {};
	std::memcpy(cb, glm::value_ptr(m_invViewProj), 64);
	std::memcpy(cb + 16, glm::value_ptr(m_prevViewProj), 64);
	std::memcpy(cb + 32, glm::value_ptr(frame.view), 64);
	cb[48] = float((std::max)(frame.width, 1));
	cb[49] = float((std::max)(frame.height, 1));
	cb[50] = frame.jitterX * 2.f / float((std::max)(frame.width, 1));
	cb[51] = -frame.jitterY * 2.f / float((std::max)(frame.height, 1));
	cb[52] = frame.cameraPos.x;
	cb[53] = frame.cameraPos.y;
	cb[54] = frame.cameraPos.z;
	cb[55] = !masksOk ? 0.f : (waterDepthOk ? 2.f : 1.f);
	frame.list->SetGraphicsRoot32BitConstants(0, 56, cb, 0);
	frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	frame.list->DrawInstanced(3, 1, 0, 0);
	
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_RENDER_TARGET);
	slTransition(frame.list, frame.depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_DEPTH_WRITE);
	return true;
#endif
}

bool D3D12Streamline::clearMotionVectors(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	return false;
#else
	if(!frame.list || !m_gpu || !m_gpu->mvec || !m_gpu->rtvHeap) {
		return false;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
	frame.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
	return true;
#endif
}

bool D3D12Streamline::setConstants(const Frame & frame, void * token) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	ARX_UNUSED(token);
	return false;
#else
	sl::Constants c {};
	m_viewProj = frame.proj * frame.view;
	m_invViewProj = glm::inverse(m_viewProj);
	const glm::mat4x4 invProj = glm::inverse(frame.proj);
	toSl(c.cameraViewToClip, frame.proj);
	toSl(c.clipToCameraView, invProj);
	toSl(c.clipToLensClip, glm::mat4x4(1.f));
	toSl(c.clipToPrevClip, m_prevViewProj * m_invViewProj);
	toSl(c.prevClipToClip, m_viewProj * m_invPrevViewProj);
	c.jitterOffset = sl::float2(frame.jitterX, frame.jitterY);
	c.mvecScale = sl::float2(1.f, 1.f);
	c.motionVectorsInvalidValue = 1024.f;
	c.cameraPinholeOffset = sl::float2(0.f, 0.f);
	c.cameraPos = sl::float3(frame.cameraPos.x, frame.cameraPos.y, frame.cameraPos.z);
	c.cameraRight = sl::float3(frame.view[0][0], frame.view[1][0], frame.view[2][0]);
	c.cameraUp = sl::float3(frame.view[0][1], frame.view[1][1], frame.view[2][1]);
	c.cameraFwd = sl::float3(frame.view[0][2], frame.view[1][2], frame.view[2][2]);
	c.cameraNear = frame.cameraNear;
	c.cameraFar = frame.cameraFar;
	c.cameraFOV = frame.cameraFov;
	const int aspectH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	const int aspectW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	c.cameraAspectRatio = float(aspectW) / float((std::max)(aspectH, 1));
	c.depthInverted = sl::Boolean::eFalse;
	c.cameraMotionIncluded = sl::Boolean::eTrue;
	c.motionVectors3D = sl::Boolean::eFalse;
	c.reset = (m_firstFrame || frame.reset) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.orthographicProjection = sl::Boolean::eFalse;
	c.motionVectorsDilated = sl::Boolean::eFalse;
	c.motionVectorsJittered = sl::Boolean::eFalse;
	const sl::ViewportHandle vp(0);
	const sl::Result r = pslSetConstants(c, *static_cast<sl::FrameToken *>(token), vp);
	if(r != sl::Result::eOk) {
		LogError << "Streamline: slSetConstants failed (" << resultName(r) << ")";
		return false;
	}
	commitCamera(frame);
	return true;
#endif
}

void D3D12Streamline::commitCamera(const Frame & frame) {
	m_prevViewProj = frame.proj * frame.view;
	m_invPrevViewProj = glm::inverse(m_prevViewProj);
	m_firstFrame = false;
}

bool D3D12Streamline::evaluateRr(const Frame & frame, void * token) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	ARX_UNUSED(token);
	return false;
#else
	PFun_slDLSSDSetOptions * setOptions = nullptr;
	if(pslGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions",
	                         reinterpret_cast<void *&>(setOptions)) != sl::Result::eOk || !setOptions) {
		return false;
	}
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	sl::DLSSDOptions opt {};
	opt.mode = sl::DLSSMode(frame.dlssMode > 0 ? frame.dlssMode : int(sl::DLSSMode::eDLAA));
	opt.outputWidth = uint32_t(outW);
	opt.outputHeight = uint32_t(outH);
	// Ray Reconstruction refuses to create otherwise: sl.log says "HDR Color required" and NGX
	// returns InvalidParameter. The SDK's own default is eTrue and its guide calls it mandatory,
	// because the denoiser works in linear radiance. Plain DLSS below is different and keeps
	// eFalse legitimately.
	//
	// Caveat worth knowing: the scene colour handed to it is R8G8B8A8_UNORM, so RR reads eight
	// bits of tone-mapped output as linear radiance. It runs, but a linear fp16 scene colour is
	// what it actually wants.
	opt.colorBuffersHDR = sl::Boolean::eTrue;
	opt.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
	opt.dlaaPreset = sl::DLSSDPreset::ePresetD;
	opt.qualityPreset = sl::DLSSDPreset::ePresetD;
	opt.balancedPreset = sl::DLSSDPreset::ePresetD;
	opt.performancePreset = sl::DLSSDPreset::ePresetD;
	opt.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;
	toSl(opt.worldToCameraView, frame.view);
	toSl(opt.cameraViewToWorld, glm::inverse(frame.view));
	const sl::ViewportHandle vp(0);
	if(setOptions(vp, opt) != sl::Result::eOk) {
		return false;
	}
	sl::Extent inExt { 0, 0, uint32_t(frame.width), uint32_t(frame.height) };
	sl::Extent outExt { 0, 0, uint32_t(outW), uint32_t(outH) };
	// Ray Reconstruction denoises in linear radiance. The raster writes gamma-encoded colour, so
	// hand it a converted copy rather than eight bits of gamma relabelled as linear.
	const bool linear = linearizeSceneColour(frame);
	ID3D12Resource * rrIn = linear ? m_gpu->linearIn : frame.color;
	const uint32_t rrInState = linear
		? uint32_t(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		: uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET);
	sl::Resource colorIn { sl::ResourceType::eTex2d, rrIn, rrInState };
	sl::Resource colorOut { sl::ResourceType::eTex2d, m_gpu->hdrOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
	sl::Resource depth { sl::ResourceType::eTex2d, frame.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE };
	sl::Resource mvec { sl::ResourceType::eTex2d, m_gpu->mvec, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource nrm { sl::ResourceType::eTex2d, m_gpu->nrm, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource albedo { sl::ResourceType::eTex2d, m_gpu->albedo,
	                      D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource specA { sl::ResourceType::eTex2d, m_gpu->specA,
	                     D3D12_RESOURCE_STATE_RENDER_TARGET };
	// Albedo and specular albedo are required, not optional: the plugin rejects an evaluate
	// without them. What we can hand it is lit raster rather than unlit albedo, which is an
	// approximation — but the choice is an approximate albedo or no Ray Reconstruction at all.
	// Specular hit distance genuinely is optional, and the G-buffer writes a constant zero
	// there, so tagging it would be telling the denoiser something false.
	sl::ResourceTag tags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outExt },
		{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inExt },
		{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &nrm, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &albedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &specA, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
	};
	auto * ft = static_cast<sl::FrameToken *>(token);
	if(pslSetTagForFrame(*ft, vp, tags, UINT(_countof(tags)), frame.list) != sl::Result::eOk) {
		return false;
	}
	const sl::BaseStructure * inputs[] = { &vp };
	const sl::Result r = pslEvaluateFeature(sl::kFeatureDLSS_RR, *ft, inputs, 1, frame.list);
	if(r != sl::Result::eOk) {
		LogWarning << "Streamline: DLSS-RR evaluate failed (" << resultName(r) << ")";
		return false;
	}
	// Only now, past every way this can fail. A failure here drops the frame to ordinary DLSS,
	// whose output is gamma-encoded; claiming a linear source would have the tonemap encode it
	// a second time and the fallback frame would come out visibly washed.
	m_rrLinearIn = linear;
	return true;
#endif
}

bool D3D12Streamline::evaluateDlss(const Frame & frame, void * token) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	ARX_UNUSED(token);
	return false;
#else
	PFun_slDLSSSetOptions * setOptions = nullptr;
	if(pslGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions",
	                         reinterpret_cast<void *&>(setOptions)) != sl::Result::eOk || !setOptions) {
		return false;
	}
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	sl::DLSSOptions opt {};
	opt.mode = sl::DLSSMode(frame.dlssMode > 0 ? frame.dlssMode : int(sl::DLSSMode::eDLAA));
	opt.outputWidth = uint32_t(outW);
	opt.outputHeight = uint32_t(outH);
	opt.colorBuffersHDR = sl::Boolean::eFalse;
	opt.useAutoExposure = sl::Boolean::eFalse;
	opt.preExposure = 1.f;
	opt.exposureScale = 1.f;
	// Preset L (Ultra Performance default) is the expensive transformer.
	// Same K model for every mode so lowering the preset actually gets cheaper.
	opt.dlaaPreset = sl::DLSSPreset::ePresetK;
	opt.qualityPreset = sl::DLSSPreset::ePresetK;
	opt.balancedPreset = sl::DLSSPreset::ePresetK;
	opt.performancePreset = sl::DLSSPreset::ePresetK;
	opt.ultraPerformancePreset = sl::DLSSPreset::ePresetK;
	const sl::ViewportHandle vp(0);
	if(setOptions(vp, opt) != sl::Result::eOk) {
		return false;
	}
	sl::Extent inExt { 0, 0, uint32_t(frame.width), uint32_t(frame.height) };
	sl::Extent outExt { 0, 0, uint32_t(outW), uint32_t(outH) };
	sl::Resource colorIn { sl::ResourceType::eTex2d, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource colorOut { sl::ResourceType::eTex2d, m_gpu->hdrOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
	sl::Resource depth { sl::ResourceType::eTex2d, frame.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE };
	sl::Resource mvec { sl::ResourceType::eTex2d, m_gpu->mvec, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::ResourceTag tags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outExt },
		{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inExt },
		{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
	};
	auto * ft = static_cast<sl::FrameToken *>(token);
	if(pslSetTagForFrame(*ft, vp, tags, UINT(_countof(tags)), frame.list) != sl::Result::eOk) {
		return false;
	}
	const sl::BaseStructure * inputs[] = { &vp };
	const sl::Result r = pslEvaluateFeature(sl::kFeatureDLSS, *ft, inputs, 1, frame.list);
	if(r != sl::Result::eOk) {
		LogWarning << "Streamline: DLSS evaluate failed (" << resultName(r) << ")";
		return false;
	}
	return true;
#endif
}

bool D3D12Streamline::linearizeSceneColour(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	return false;
#else
	if(!m_gpu || !m_gpu->linearIn || !m_gpu->linearPso || !m_gpu->tonemapRoot) {
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_gpu->srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += SIZE_T(8) * m_gpu->srvSize;
	m_device->CreateShaderResourceView(frame.color, &srv, cpu);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_gpu->srvHeap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += SIZE_T(8) * m_gpu->srvSize;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += SIZE_T(5) * m_gpu->rtvSize;
	m_device->CreateRenderTargetView(m_gpu->linearIn, nullptr, rtv);
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	slTransition(frame.list, m_gpu->linearIn, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_RENDER_TARGET);
	frame.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	D3D12_VIEWPORT vp {};
	vp.Width = float(frame.width);
	vp.Height = float(frame.height);
	vp.MaxDepth = 1.f;
	D3D12_RECT sc { 0, 0, LONG(frame.width), LONG(frame.height) };
	frame.list->RSSetViewports(1, &vp);
	frame.list->RSSetScissorRects(1, &sc);
	frame.list->SetGraphicsRootSignature(m_gpu->tonemapRoot);
	frame.list->SetPipelineState(m_gpu->linearPso);
	frame.list->SetDescriptorHeaps(1, &m_gpu->srvHeap);
	frame.list->SetGraphicsRootDescriptorTable(0, gpu);
	const UINT unused[4] = { 0u, 0u, 0u, 0u };
	frame.list->SetGraphicsRoot32BitConstants(1, 4, unused, 0);
	frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	frame.list->DrawInstanced(3, 1, 0, 0);
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_RENDER_TARGET);
	slTransition(frame.list, m_gpu->linearIn, D3D12_RESOURCE_STATE_RENDER_TARGET,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
#endif
}

bool D3D12Streamline::tonemapToBackbuffer(const Frame & frame, bool linearSource) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	ARX_UNUSED(linearSource);
	return false;
#else
	slTransition(frame.list, m_gpu->hdrOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
	srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_gpu->srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += SIZE_T(7) * m_gpu->srvSize;
	m_device->CreateShaderResourceView(m_gpu->hdrOut, &srv, cpu);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_gpu->srvHeap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += SIZE_T(7) * m_gpu->srvSize;
	
	D3D12_CPU_DESCRIPTOR_HANDLE dummyRtv = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	dummyRtv.ptr += SIZE_T(6) * m_gpu->rtvSize;
	ID3D12Resource * dest = frame.colorOut ? frame.colorOut : frame.color;
	m_device->CreateRenderTargetView(dest, nullptr, dummyRtv);
	frame.list->OMSetRenderTargets(1, &dummyRtv, FALSE, nullptr);
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	D3D12_VIEWPORT vp {};
	vp.Width = float(outW);
	vp.Height = float(outH);
	vp.MaxDepth = 1.f;
	D3D12_RECT sc { 0, 0, LONG(outW), LONG(outH) };
	frame.list->RSSetViewports(1, &vp);
	frame.list->RSSetScissorRects(1, &sc);
	frame.list->SetGraphicsRootSignature(m_gpu->tonemapRoot);
	frame.list->SetPipelineState(m_gpu->tonemapPso);
	frame.list->SetDescriptorHeaps(1, &m_gpu->srvHeap);
	frame.list->SetGraphicsRootDescriptorTable(0, gpu);
	const UINT encode[4] = { linearSource ? 1u : 0u, 0u, 0u, 0u };
	frame.list->SetGraphicsRoot32BitConstants(1, 4, encode, 0);
	frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	frame.list->DrawInstanced(3, 1, 0, 0);
	slTransition(frame.list, m_gpu->hdrOut, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	return true;
#endif
}

bool D3D12Streamline::blitSceneToOutput(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	return false;
#else
	if(!frame.list || !frame.color || !frame.colorOut) {
		return false;
	}
	if(frame.color == frame.colorOut) {
		return true;
	}
	const D3D12_RESOURCE_DESC src = frame.color->GetDesc();
	const D3D12_RESOURCE_DESC dst = frame.colorOut->GetDesc();
	if(src.Width == dst.Width && src.Height == dst.Height) {
		slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET,
		             D3D12_RESOURCE_STATE_COPY_SOURCE);
		slTransition(frame.list, frame.colorOut, D3D12_RESOURCE_STATE_RENDER_TARGET,
		             D3D12_RESOURCE_STATE_COPY_DEST);
		frame.list->CopyResource(frame.colorOut, frame.color);
		slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_COPY_SOURCE,
		             D3D12_RESOURCE_STATE_RENDER_TARGET);
		slTransition(frame.list, frame.colorOut, D3D12_RESOURCE_STATE_COPY_DEST,
		             D3D12_RESOURCE_STATE_RENDER_TARGET);
		return true;
	}
	if(!m_device || !m_gpu || !m_gpu->blitPso || !m_gpu->blitRoot || !m_gpu->srvHeap || !m_gpu->rtvHeap) {
		return false;
	}
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_gpu->srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += SIZE_T(6) * m_gpu->srvSize;
	m_device->CreateShaderResourceView(frame.color, &srv, cpu);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_gpu->srvHeap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += SIZE_T(6) * m_gpu->srvSize;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += SIZE_T(6) * m_gpu->rtvSize;
	m_device->CreateRenderTargetView(frame.colorOut, nullptr, rtv);
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : int(dst.Width);
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : int(dst.Height);
	D3D12_VIEWPORT vp {};
	vp.Width = float(outW);
	vp.Height = float(outH);
	vp.MaxDepth = 1.f;
	D3D12_RECT sc { 0, 0, LONG(outW), LONG(outH) };
	frame.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	frame.list->RSSetViewports(1, &vp);
	frame.list->RSSetScissorRects(1, &sc);
	frame.list->SetGraphicsRootSignature(m_gpu->blitRoot);
	frame.list->SetPipelineState(m_gpu->blitPso);
	frame.list->SetDescriptorHeaps(1, &m_gpu->srvHeap);
	const float cb[4] = {
		float((std::max)(frame.width, 1)),
		float((std::max)(frame.height, 1)),
		float((std::max)(outW, 1)),
		float((std::max)(outH, 1))
	};
	frame.list->SetGraphicsRoot32BitConstants(0, 4, cb, 0);
	frame.list->SetGraphicsRootDescriptorTable(1, gpu);
	frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	frame.list->DrawInstanced(3, 1, 0, 0);
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
#endif
}

void * D3D12Streamline::frameToken() {
#if !ARX_HAVE_STREAMLINE
	return nullptr;
#else
	if(m_token) {
		return m_token;
	}
	sl::FrameToken * token = nullptr;
	if(!pslGetNewFrameToken || pslGetNewFrameToken(token, nullptr) != sl::Result::eOk || !token) {
		return nullptr;
	}
	m_token = token;
	return m_token;
#endif
}

void D3D12Streamline::setDlssg(bool on) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(on);
#else
	if(!m_fg || !pslGetFeatureFunction || on == m_fgOn) {
		return;
	}
	PFun_slDLSSGSetOptions * set = nullptr;
	if(pslGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions",
	                         reinterpret_cast<void *&>(set)) != sl::Result::eOk || !set) {
		return;
	}
	sl::DLSSGOptions opt {};
	opt.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	opt.numFramesToGenerate = 1;
	opt.numBackBuffers = 2;
	const sl::ViewportHandle vp(0);
	if(set(vp, opt) != sl::Result::eOk) {
		LogWarning << "Streamline: slDLSSGSetOptions failed";
		return;
	}
	m_fgOn = on;
	if(on && !m_loggedFg) {
		LogInfo << "Streamline: DLSS-G on (2x, Reflex, HUD-less before HUD)";
		m_loggedFg = true;
	}
	if(!on) {
		LogInfo << "Streamline: DLSS-G off";
		m_loggedFg = false;
	}
#endif
}

void D3D12Streamline::setReflex(bool on) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(on);
#else
	if(!m_reflex || !pslGetFeatureFunction || on == m_reflexOn) {
		return;
	}
	PFun_slReflexSetOptions * set = nullptr;
	if(pslGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions",
	                         reinterpret_cast<void *&>(set)) != sl::Result::eOk || !set) {
		return;
	}
	sl::ReflexOptions opt {};
	opt.mode = on ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
	if(set(opt) != sl::Result::eOk) {
		LogWarning << "Streamline: slReflexSetOptions failed";
		return;
	}
	m_reflexOn = on;
#endif
}

void D3D12Streamline::pclMarker(int marker) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(marker);
#else
	if(!m_token || !pslGetFeatureFunction) {
		return;
	}
	PFun_slPCLSetMarker * set = nullptr;
	if(pslGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker",
	                         reinterpret_cast<void *&>(set)) != sl::Result::eOk || !set) {
		return;
	}
	set(sl::PCLMarker(marker), *static_cast<sl::FrameToken *>(m_token));
#endif
}

bool D3D12Streamline::copyHudless(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	return false;
#else
	ID3D12Resource * src = frame.colorOut ? frame.colorOut : frame.color;
	if(!frame.list || !src || !m_gpu || !m_gpu->hudless) {
		return false;
	}
	slTransition(frame.list, src, D3D12_RESOURCE_STATE_RENDER_TARGET,
	             D3D12_RESOURCE_STATE_COPY_SOURCE);
	frame.list->CopyResource(m_gpu->hudless, src);
	slTransition(frame.list, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
	             D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
#endif
}

void D3D12Streamline::beginFrame() {
#if ARX_HAVE_STREAMLINE
	if(!m_ready || !supportsFg()) {
		return;
	}
	if(!frameToken()) {
		return;
	}
	setReflex(true);
	PFun_slReflexSleep * sleep = nullptr;
	if(pslGetFeatureFunction && pslGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep",
	                         reinterpret_cast<void *&>(sleep)) == sl::Result::eOk && sleep) {
		sleep(*static_cast<sl::FrameToken *>(m_token));
	}
	pclMarker(int(sl::PCLMarker::eRenderSubmitStart));
#endif
}

void D3D12Streamline::syncFrameGen(bool enabled) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(enabled);
#else
	if(!supportsFg()) {
		return;
	}
	if(enabled) {
		setReflex(true);
		setDlssg(true);
	} else {
		setDlssg(false);
		setReflex(false);
	}
#endif
}

void D3D12Streamline::prepareFrameGen(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
#else
	if(!supportsFg() || !frame.wantFg) {
		return;
	}
	if(!frame.list || !frame.depth) {
		return;
	}
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	if(!ensureTargets(frame.width, frame.height, outW, outH,
	                  (frame.wantDlss || frame.wantRr) ? kTargetDlss : kTargetFg)) {
		return;
	}
	void * token = frameToken();
	if(!token) {
		return;
	}
	if(!m_constantsSet) {
		if(!frame.wantDlss && !frame.wantRr && !clearMotionVectors(frame)) {
			return;
		}
		if(!setConstants(frame, token)) {
			return;
		}
		m_constantsSet = true;
	}
	if(!copyHudless(frame)) {
		return;
	}
	sl::Extent inExt { 0, 0, uint32_t(frame.width), uint32_t(frame.height) };
	sl::Extent outExt { 0, 0, uint32_t(outW), uint32_t(outH) };
	sl::Resource depth { sl::ResourceType::eTex2d, frame.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE };
	sl::Resource mvec { sl::ResourceType::eTex2d, m_gpu->mvec, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource hud { sl::ResourceType::eTex2d, m_gpu->hudless, D3D12_RESOURCE_STATE_COPY_DEST };
	sl::ResourceTag tags[] = {
		{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inExt },
		{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inExt },
		{ &hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &outExt },
	};
	const sl::ViewportHandle vp(0);
	if(pslSetTagForFrame(*static_cast<sl::FrameToken *>(token), vp, tags, UINT(_countof(tags)),
	                     frame.list) != sl::Result::eOk) {
		LogWarning << "Streamline: DLSS-G tag failed";
		return;
	}
	m_fgTagged = true;
#endif
}

void D3D12Streamline::onPresent() {
#if ARX_HAVE_STREAMLINE
	if(m_fgOn && !m_fgTagged && !m_loggedUntagged) {
		LogWarning << "Streamline: Present without DLSS-G tags";
		m_loggedUntagged = true;
	}
	pclMarker(int(sl::PCLMarker::eRenderSubmitEnd));
	pclMarker(int(sl::PCLMarker::ePresentStart));
#endif
}

void D3D12Streamline::afterPresent() {
#if ARX_HAVE_STREAMLINE
	pclMarker(int(sl::PCLMarker::ePresentEnd));
	m_fgTagged = false;
	m_token = nullptr;
	m_constantsSet = false;
#endif
}

bool D3D12Streamline::evaluate(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
	return false;
#else
	m_rrLive = false;
	auto fail = [&]() -> bool {
		blitSceneToOutput(frame);
		return false;
	};
	if(!m_ready || !frame.list || !frame.color || !frame.depth) {
		return fail();
	}
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	if(!ensureTargets(frame.width, frame.height, outW, outH,
	                  frame.wantRr ? kTargetRr : kTargetDlss)) {
		return fail();
	}
	if(!clearMotionVectors(frame)) {
		return fail();
	}
	void * token = frameToken();
	if(!token) {
		LogError << "Streamline: slGetNewFrameToken failed";
		return fail();
	}
	if(!setConstants(frame, token)) {
		return fail();
	}
	m_constantsSet = true;
	bool ok = false;
	const char * path = nullptr;
	if(frame.wantRr && m_rr && !m_rrFailed) {
		if(rasterGbuffers(frame) && evaluateRr(frame, token)) {
			ok = true;
			path = "DLSS-RR";
			m_rrLive = true;
		} else {
			m_rrFailed = true;
			if(!m_loggedRrSkip) {
				LogWarning << "Streamline: DLSS-RR NGX create failed — homemade denoise stays on";
				m_loggedRrSkip = true;
			}
		}
	} else if(!frame.wantRr) {
		m_rrFailed = false;
		m_loggedRrSkip = false;
	}
	if(!ok && m_dlss && frame.wantDlss) {
		ok = evaluateDlss(frame, token);
		path = "DLSS";
	}
	if(!ok) {
		if(!m_loggedOff) {
			LogWarning << "Streamline: evaluate failed — blitting scene colour";
			m_loggedOff = true;
		}
		m_loggedOn = false;
		return fail();
	}
	// Only the Ray Reconstruction path produced linear output; encoding the plain DLSS result
	// would gamma it twice.
	tonemapToBackbuffer(frame, m_rrLinearIn);
	m_rrLinearIn = false;
	if(!m_loggedOn || frame.reset) {
		LogInfo << "Streamline: " << path << " evaluate ok " << frame.width << "x" << frame.height
		        << " -> " << outW << "x" << outH
		        << (std::strcmp(path, "DLSS-RR") == 0
		            ? " (HDR flag, preset D, albedo=lit raster)"
		            : " (LDR, preset K, jitter on)");
		m_loggedOn = true;
		m_loggedOff = false;
	}
	return true;
#endif
}

#endif // ARX_HAVE_D3D12
