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
	const glm::mat4x4 r = glm::transpose(m);
	for(int i = 0; i < 4; ++i) {
		out.setRow(uint32_t(i), sl::float4(r[i][0], r[i][1], r[i][2], r[i][3]));
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
struct PSOut {
	float2 mvec : SV_Target0;
	float4 nrmR : SV_Target1;
	float4 albedo : SV_Target2;
	float4 specA : SV_Target3;
	float4 hdr : SV_Target4;
	float hit : SV_Target5;
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
	float3 beauty = colorTex.Load(int3(pix, 0)).rgb;
	float3 base = albedoTex.Load(int3(pix, 0)).rgb;
	float water = 0.0;
	float metal = 0.0;
	if(hasMasks > 0.5) {
		water = waterTex.Load(int3(pix, 0)).r;
		metal = metalTex.Load(int3(pix, 0)).r;
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
	PSOut o;
	o.mvec = float2(0, 0);
	o.nrmR = float4(nView, rough);
	o.albedo = float4(base, 1.0);
	o.specA = float4(f0, 1.0);
	o.hdr = float4(beauty, 1.0);
	o.hit = length(cameraPos - world);
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

const char * kTonemap = R"(
Texture2D hdrTex : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
	float3 h = hdrTex.Load(int3(pos.xy, 0)).rgb;
	return float4(saturate(h), 1);
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
	ID3D12Resource * hdrIn = nullptr;
	ID3D12Resource * hdrOut = nullptr;
	ID3D12Resource * hit = nullptr;
	ID3D12Resource * hudless = nullptr;
	ID3D12DescriptorHeap * rtvHeap = nullptr;
	ID3D12DescriptorHeap * srvHeap = nullptr;
	ID3D12PipelineState * gbufferPso = nullptr;
	ID3D12PipelineState * tonemapPso = nullptr;
	ID3D12RootSignature * gbufferRoot = nullptr;
	ID3D12RootSignature * tonemapRoot = nullptr;
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
		drop(hdrIn); hdrIn = nullptr;
		drop(hdrOut); hdrOut = nullptr;
		drop(hit); hit = nullptr;
		drop(hudless); hudless = nullptr;
		drop(rtvHeap); rtvHeap = nullptr;
		drop(srvHeap); srvHeap = nullptr;
		drop(gbufferPso); gbufferPso = nullptr;
		drop(tonemapPso); tonemapPso = nullptr;
		drop(gbufferRoot); gbufferRoot = nullptr;
		drop(tonemapRoot); tonemapRoot = nullptr;
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
	if(!m_gpu) {
		m_gpu = new Gpu();
	}
	return m_ready;
#endif
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
	m_ready = false;
	m_dlss = false;
	m_rr = false;
	m_fg = false;
	m_reflex = false;
	m_fgOn = false;
	m_reflexOn = false;
	m_fgTagged = false;
	m_loggedFg = false;
	m_token = nullptr;
	m_loggedRrSkip = false;
	m_device = nullptr;
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
			// Keep real Ultra at 1440p+ (~33% of 1440 is 480p; 4K is 720p).
			if(outputHeight > 0 && outputHeight < 1440) {
				return int(sl::DLSSMode::eMaxPerformance);
			}
			return int(sl::DLSSMode::eUltraPerformance);
		default: return int(sl::DLSSMode::eOff);
	}
#endif
}

bool D3D12Streamline::queryOptimalSize(int slMode, int outputW, int outputH, int & renderW, int & renderH) const {
	renderW = outputW;
	renderH = outputH;
	if(slMode <= 0 || outputW <= 0 || outputH <= 0) {
		return false;
	}
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(slMode);
	return false;
#else
	if(sl::DLSSMode(slMode) == sl::DLSSMode::eUltraPerformance && outputH < 1440) {
		slMode = int(sl::DLSSMode::eMaxPerformance);
	}
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
	return true;
#endif
}

bool D3D12Streamline::ensureTargets(int inputW, int inputH, int outputW, int outputH) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(inputW);
	ARX_UNUSED(inputH);
	ARX_UNUSED(outputW);
	ARX_UNUSED(outputH);
	return false;
#else
	if(!m_device || !m_gpu || inputW <= 0 || inputH <= 0 || outputW <= 0 || outputH <= 0) {
		return false;
	}
	if(inputW == m_inW && inputH == m_inH && outputW == m_outW && outputH == m_outH && m_gpu->hdrOut) {
		return true;
	}
	releaseTargets();
	m_gpu->rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_gpu->srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_DESCRIPTOR_HEAP_DESC rtv {};
	rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv.NumDescriptors = 8;
	if(FAILED(m_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_gpu->rtvHeap)))) {
		return false;
	}
	D3D12_DESCRIPTOR_HEAP_DESC srv {};
	srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srv.NumDescriptors = 8;
	srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(m_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_gpu->srvHeap)))) {
		return false;
	}
	const UINT w = UINT(inputW);
	const UINT h = UINT(inputH);
	const UINT ow = UINT(outputW);
	const UINT oh = UINT(outputH);
	const D3D12_RESOURCE_FLAGS rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
	                                 | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if(!makeTex(m_device, w, h, DXGI_FORMAT_R16G16_FLOAT, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->mvec)
	   || !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	               D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->nrm)
	   || !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	               D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->albedo)
	   || !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	               D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->specA)
	   || !makeTex(m_device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt,
	               D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->hdrIn)
	   || !makeTex(m_device, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, uav,
	               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &m_gpu->hdrOut)
	   || !makeTex(m_device, w, h, DXGI_FORMAT_R16_FLOAT, rt,
	               D3D12_RESOURCE_STATE_RENDER_TARGET, &m_gpu->hit)
	   || !makeTex(m_device, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
	               D3D12_RESOURCE_STATE_COPY_DEST, &m_gpu->hudless)) {
		LogError << "Streamline: G-buffer targets failed";
		releaseTargets();
		return false;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE r0 = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	auto rtvAt = [&](UINT i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = r0;
		h.ptr += SIZE_T(i) * m_gpu->rtvSize;
		return h;
	};
	m_device->CreateRenderTargetView(m_gpu->mvec, nullptr, rtvAt(0));
	m_device->CreateRenderTargetView(m_gpu->nrm, nullptr, rtvAt(1));
	m_device->CreateRenderTargetView(m_gpu->albedo, nullptr, rtvAt(2));
	m_device->CreateRenderTargetView(m_gpu->specA, nullptr, rtvAt(3));
	m_device->CreateRenderTargetView(m_gpu->hdrIn, nullptr, rtvAt(4));
	m_device->CreateRenderTargetView(m_gpu->hit, nullptr, rtvAt(5));
	
	D3D12_DESCRIPTOR_RANGE ranges[2] {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 5;
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
	if(FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
		if(err) {
			err->Release();
		}
		return false;
	}
	if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                        IID_PPV_ARGS(&m_gpu->gbufferRoot)))) {
		blob->Release();
		return false;
	}
	blob->Release();
	
	D3D12_DESCRIPTOR_RANGE tr {};
	tr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	tr.NumDescriptors = 1;
	tr.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER tp {};
	tp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	tp.DescriptorTable.NumDescriptorRanges = 1;
	tp.DescriptorTable.pDescriptorRanges = &tr;
	tp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC trs {};
	trs.NumParameters = 1;
	trs.pParameters = &tp;
	if(FAILED(D3D12SerializeRootSignature(&trs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
		if(err) {
			err->Release();
		}
		return false;
	}
	if(FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                        IID_PPV_ARGS(&m_gpu->tonemapRoot)))) {
		blob->Release();
		return false;
	}
	blob->Release();
	
	ID3DBlob * vs = nullptr;
	ID3DBlob * ps = nullptr;
	ID3DBlob * cerr = nullptr;
	if(FAILED(D3DCompile(kGbuffer, std::strlen(kGbuffer), "sl_gbuffer", nullptr, nullptr,
	                     "VSMain", "vs_5_0", 0, 0, &vs, &cerr))) {
		LogError << "Streamline: G-buffer VS failed"
		         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
		if(cerr) {
			cerr->Release();
		}
		return false;
	}
	if(cerr) {
		cerr->Release();
		cerr = nullptr;
	}
	if(FAILED(D3DCompile(kGbuffer, std::strlen(kGbuffer), "sl_gbuffer", nullptr, nullptr,
	                     "PSMain", "ps_5_0", 0, 0, &ps, &cerr))) {
		LogError << "Streamline: G-buffer PS failed"
		         << (cerr ? static_cast<const char *>(cerr->GetBufferPointer()) : "");
		vs->Release();
		if(cerr) {
			cerr->Release();
		}
		return false;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
	pd.pRootSignature = m_gpu->gbufferRoot;
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.BlendState.IndependentBlendEnable = TRUE;
	for(UINT i = 0; i < 6; ++i) {
		pd.BlendState.RenderTarget[i].RenderTargetWriteMask = (i == 0 || i == 5)
			? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN
			: D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	pd.BlendState.RenderTarget[5].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.DepthStencilState.DepthEnable = FALSE;
	pd.SampleMask = 0xffffffff;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 6;
	pd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
	pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	pd.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	pd.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	pd.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	pd.RTVFormats[5] = DXGI_FORMAT_R16_FLOAT;
	pd.SampleDesc.Count = 1;
	if(FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_gpu->gbufferPso)))) {
		LogError << "Streamline: G-buffer PSO failed";
		vs->Release();
		ps->Release();
		return false;
	}
	vs->Release();
	ps->Release();
	
	if(FAILED(D3DCompile(kTonemap, std::strlen(kTonemap), "sl_tonemap", nullptr, nullptr,
	                     "VSMain", "vs_5_0", 0, 0, &vs, &cerr))) {
		if(cerr) {
			cerr->Release();
		}
		return false;
	}
	if(cerr) {
		cerr->Release();
		cerr = nullptr;
	}
	if(FAILED(D3DCompile(kTonemap, std::strlen(kTonemap), "sl_tonemap", nullptr, nullptr,
	                     "PSMain", "ps_5_0", 0, 0, &ps, &cerr))) {
		vs->Release();
		if(cerr) {
			cerr->Release();
		}
		return false;
	}
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
	if(FAILED(m_device->CreateGraphicsPipelineState(&td, IID_PPV_ARGS(&m_gpu->tonemapPso)))) {
		LogError << "Streamline: tonemap PSO failed";
		vs->Release();
		ps->Release();
		return false;
	}
	vs->Release();
	ps->Release();
	m_inW = inputW;
	m_inH = inputH;
	m_outW = outputW;
	m_outH = outputH;
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
	ID3D12Resource * water = frame.waterMask ? frame.waterMask : frame.albedoSrc;
	ID3D12Resource * metal = frame.metalMask ? frame.metalMask : frame.albedoSrc;
	m_device->CreateShaderResourceView(water, frame.waterMask ? &maskSrv : &colorSrv, srvAt(3));
	m_device->CreateShaderResourceView(metal, frame.metalMask ? &maskSrv : &colorSrv, srvAt(4));
	
	slTransition(frame.list, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	slTransition(frame.list, frame.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
	             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	
	D3D12_CPU_DESCRIPTOR_HANDLE rts[6];
	D3D12_CPU_DESCRIPTOR_HANDLE r0 = m_gpu->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for(UINT i = 0; i < 6; ++i) {
		rts[i] = r0;
		rts[i].ptr += SIZE_T(i) * m_gpu->rtvSize;
	}
	frame.list->OMSetRenderTargets(6, rts, FALSE, nullptr);
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
	const glm::mat4x4 viewProj = frame.proj * frame.view;
	const glm::mat4x4 inv = glm::inverse(viewProj);
	float cb[56] {};
	std::memcpy(cb, glm::value_ptr(inv), 64);
	std::memcpy(cb + 16, glm::value_ptr(m_prevViewProj), 64);
	std::memcpy(cb + 32, glm::value_ptr(frame.view), 64);
	cb[48] = float((std::max)(frame.width, 1));
	cb[49] = float((std::max)(frame.height, 1));
	cb[50] = frame.jitterX * 2.f / float((std::max)(frame.width, 1));
	cb[51] = -frame.jitterY * 2.f / float((std::max)(frame.height, 1));
	cb[52] = frame.cameraPos.x;
	cb[53] = frame.cameraPos.y;
	cb[54] = frame.cameraPos.z;
	cb[55] = (frame.waterMask && frame.metalMask) ? 1.f : 0.f;
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
	const glm::mat4x4 viewProj = frame.proj * frame.view;
	const glm::mat4x4 invView = glm::inverse(frame.view);
	const glm::mat4x4 invProj = glm::inverse(frame.proj);
	toSl(c.cameraViewToClip, frame.proj);
	toSl(c.clipToCameraView, invProj);
	toSl(c.clipToLensClip, glm::mat4x4(1.f));
	toSl(c.clipToPrevClip, m_prevViewProj * glm::inverse(viewProj));
	toSl(c.prevClipToClip, viewProj * glm::inverse(m_prevViewProj));
	c.jitterOffset = sl::float2(frame.jitterX, frame.jitterY);
	c.mvecScale = sl::float2(1.f, 1.f);
	// Required when cameraMotionIncluded is false — default INVALID_FLOAT
	// makes Streamline skip camera synthesis (blur + extra work).
	c.motionVectorsInvalidValue = 1024.f;
	c.cameraPinholeOffset = sl::float2(0.f, 0.f);
	c.cameraPos = sl::float3(frame.cameraPos.x, frame.cameraPos.y, frame.cameraPos.z);
	c.cameraRight = sl::float3(frame.view[0][0], frame.view[1][0], frame.view[2][0]);
	c.cameraUp = sl::float3(frame.view[0][1], frame.view[1][1], frame.view[2][1]);
	c.cameraFwd = sl::float3(-frame.view[0][2], -frame.view[1][2], -frame.view[2][2]);
	c.cameraNear = frame.cameraNear;
	c.cameraFar = frame.cameraFar;
	c.cameraFOV = frame.cameraFov;
	const int aspectH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	const int aspectW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	c.cameraAspectRatio = float(aspectW) / float((std::max)(aspectH, 1));
	c.depthInverted = sl::Boolean::eFalse;
	c.cameraMotionIncluded = sl::Boolean::eFalse;
	c.motionVectors3D = sl::Boolean::eFalse;
	c.reset = (m_firstFrame || frame.reset) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.orthographicProjection = sl::Boolean::eFalse;
	c.motionVectorsDilated = sl::Boolean::eFalse;
	c.motionVectorsJittered = sl::Boolean::eFalse;
	ARX_UNUSED(invView);
	const sl::ViewportHandle vp(0);
	const sl::Result r = pslSetConstants(c, *static_cast<sl::FrameToken *>(token), vp);
	if(r != sl::Result::eOk) {
		LogError << "Streamline: slSetConstants failed (" << resultName(r) << ")";
		return false;
	}
	return true;
#endif
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
	opt.colorBuffersHDR = sl::Boolean::eFalse;
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
	sl::Resource colorIn { sl::ResourceType::eTex2d, frame.color, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource colorOut { sl::ResourceType::eTex2d, m_gpu->hdrOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
	sl::Resource depth { sl::ResourceType::eTex2d, frame.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE };
	sl::Resource mvec { sl::ResourceType::eTex2d, m_gpu->mvec, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource albedo { sl::ResourceType::eTex2d, m_gpu->albedo, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource specA { sl::ResourceType::eTex2d, m_gpu->specA, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource nrm { sl::ResourceType::eTex2d, m_gpu->nrm, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::Resource hit { sl::ResourceType::eTex2d, m_gpu->hit, D3D12_RESOURCE_STATE_RENDER_TARGET };
	sl::ResourceTag tags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outExt },
		{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inExt },
		{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &albedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &specA, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &nrm, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
		{ &hit, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eOnlyValidNow, &inExt },
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

bool D3D12Streamline::tonemapToBackbuffer(const Frame & frame) {
#if !ARX_HAVE_STREAMLINE
	ARX_UNUSED(frame);
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
	cpu.ptr += SIZE_T(2) * m_gpu->srvSize;
	m_device->CreateShaderResourceView(m_gpu->hdrOut, &srv, cpu);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_gpu->srvHeap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += SIZE_T(2) * m_gpu->srvSize;
	
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
	frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	frame.list->DrawInstanced(3, 1, 0, 0);
	slTransition(frame.list, m_gpu->hdrOut, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
	if(!ensureTargets(frame.width, frame.height, outW, outH)) {
		return;
	}
	void * token = frameToken();
	if(!token) {
		return;
	}
	if(!frame.wantDlss) {
		if(!clearMotionVectors(frame)) {
			return;
		}
		if(!m_constantsSet && !setConstants(frame, token)) {
			return;
		}
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
	if(!m_ready || !frame.list || !frame.color || !frame.depth) {
		return false;
	}
	const int outW = frame.outputWidth > 0 ? frame.outputWidth : frame.width;
	const int outH = frame.outputHeight > 0 ? frame.outputHeight : frame.height;
	if(!ensureTargets(frame.width, frame.height, outW, outH)) {
		return false;
	}
	if(!clearMotionVectors(frame)) {
		return false;
	}
	void * token = frameToken();
	if(!token) {
		LogError << "Streamline: slGetNewFrameToken failed";
		return false;
	}
	if(!setConstants(frame, token)) {
		return false;
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
			m_rrLive = false;
			if(!m_loggedRrSkip) {
				LogWarning << "Streamline: DLSS-RR NGX create failed — homemade denoise stays on";
				m_loggedRrSkip = true;
			}
		}
	} else if(!frame.wantRr) {
		m_rrLive = false;
		m_rrFailed = false;
		m_loggedRrSkip = false;
	}
	if(!ok && m_dlss && frame.wantDlss) {
		ok = evaluateDlss(frame, token);
		path = "DLSS";
	}
	if(!ok) {
		if(!m_loggedOff) {
			LogWarning << "Streamline: evaluate failed — raster unchanged";
			m_loggedOff = true;
		}
		m_loggedOn = false;
		m_prevViewProj = frame.proj * frame.view;
		m_firstFrame = false;
		return false;
	}
	tonemapToBackbuffer(frame);
	if(!m_loggedOn || frame.reset) {
		LogInfo << "Streamline: " << path << " evaluate ok " << frame.width << "x" << frame.height
		        << " -> " << outW << "x" << outH
		        << (std::strcmp(path, "DLSS-RR") == 0
		            ? " (LDR, preset D, albedo=pre-DXR)"
		            : " (LDR, preset K, jitter on)");
		m_loggedOn = true;
		m_loggedOff = false;
	}
	m_prevViewProj = frame.proj * frame.view;
	m_firstFrame = false;
	return true;
#endif
}

#endif // ARX_HAVE_D3D12
