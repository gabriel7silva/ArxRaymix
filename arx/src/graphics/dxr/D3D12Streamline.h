/*
 * Arx Raymix — NVIDIA Streamline (DLSS / DLSS-G / Reflex) on the D3D12 path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_DXR_D3D12STREAMLINE_H
#define ARX_GRAPHICS_DXR_D3D12STREAMLINE_H

#include "Configure.h"

#if ARX_HAVE_D3D12

#include <glm/glm.hpp>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter1;

class D3D12Streamline {
public:
	struct Frame {
		ID3D12GraphicsCommandList * list = nullptr;
		ID3D12Resource * color = nullptr;
		ID3D12Resource * colorOut = nullptr;
		ID3D12Resource * depth = nullptr;
		//! Pre-DXR raster (colorCopy). Lit beauty — not tagged as RR albedo.
		ID3D12Resource * albedoSrc = nullptr;
		ID3D12Resource * waterMask = nullptr;
		ID3D12Resource * metalMask = nullptr;
		ID3D12Resource * waterDepth = nullptr;
		//! True only if D3D12Rtao rasterised masks this frame and left them as SRVs.
		bool masksReady = false;
		glm::mat4x4 view = glm::mat4x4(1.f);
		glm::mat4x4 proj = glm::mat4x4(1.f);
		glm::vec3 cameraPos {};
		float cameraNear = 1.f;
		float cameraFar = 6400.f;
		float cameraFov = 1.f;
		int width = 0;
		int height = 0;
		int outputWidth = 0;
		int outputHeight = 0;
		int dlssMode = 0;
		float jitterX = 0.f;
		float jitterY = 0.f;
		bool reset = false;
		bool wantRr = false;
		bool wantDlss = false;
		bool wantFg = false;
	};
	
	D3D12Streamline();
	~D3D12Streamline();
	
	//! slInit. Call before CreateDXGIFactory / D3D12CreateDevice.
	bool init();
	//! Wrap a native DXGI/D3D interface so Present hits sl.common presentCommon().
	bool upgradeInterface(void ** iface);
	//! slSetD3DDevice + feature query. Call immediately after the device exists.
	bool setDevice(ID3D12Device * device, IDXGIAdapter1 * adapter);
	void shutdown();
	
	[[nodiscard]] bool ready() const { return m_ready; }
	[[nodiscard]] bool supportsDlss() const { return m_dlss; }
	[[nodiscard]] bool supportsRr() const { return m_rr; }
	//! DLSS Frame Generation needs both the FG plugin and Reflex.
	[[nodiscard]] bool supportsFg() const { return m_fg && m_reflex; }
	//! True only after a successful DLSS-RR evaluate. Homemade denoise stays
	//! on until this is true — a failed NGX create must not show raw dither.
	[[nodiscard]] bool rrLive() const { return m_rrLive; }
	
	//! cfg dxr_dlss (0 Off / 1 DLAA / 2–5 quality) → Streamline DLSSMode as int (0 = Off).
	static int resolveDlssMode(int setting, int outputHeight);
	//! True when Ultra Performance was demoted because outputH is below 1440p.
	static bool wasDowngraded(int setting, int outputHeight);
	static const char * dlssModeName(int slMode);
	static int dlaaMode();
	//! Optimal internal render size for a Streamline DLSSMode. Falls back to scale tables.
	bool queryOptimalSize(int slMode, int outputW, int outputH, int & renderW, int & renderH) const;
	//! Scene colour → colorOut when evaluate() cannot upscale.
	bool blitSceneToOutput(const Frame & frame);
	
	//! After world raster + DXR, before particles / HUD. Restores nothing — caller
	//! must restoreRasterBind() after this returns.
	bool evaluate(const Frame & frame);
	
	//! Reflex sleep + frame token. Call once at the start of a 3D level frame.
	void beginFrame();
	//! Menu toggle only. RT / DLSS / untagged Presents do not change FG.
	void syncFrameGen(bool enabled);
	//! Copy HUD-less color, tag depth / mvec. Does not turn FG off on failure.
	void prepareFrameGen(const Frame & frame);
	//! PCL markers only — never slDLSSGSetOptions(eOff).
	void onPresent();
	void afterPresent();
	
	void resize(int width, int height);
	
private:
	enum TargetBits : unsigned {
		kTargetMvec = 1u,
		kTargetHudless = 2u,
		kTargetHdrOut = 4u,
		kTargetGbuffer = 8u
	};
	static constexpr unsigned kTargetFg = kTargetMvec | kTargetHudless;
	static constexpr unsigned kTargetDlss = kTargetFg | kTargetHdrOut;
	static constexpr unsigned kTargetRr = kTargetDlss | kTargetGbuffer;
	
	bool loadLibrary();
	bool ensureTargets(int inputW, int inputH, int outputW, int outputH, unsigned needed);
	bool rasterGbuffers(const Frame & frame);
	bool clearMotionVectors(const Frame & frame);
	bool setConstants(const Frame & frame, void * token);
	void commitCamera(const Frame & frame);
	void resetState();
	bool evaluateRr(const Frame & frame, void * token);
	bool evaluateDlss(const Frame & frame, void * token);
	//! linearSource: the Ray Reconstruction path hands over linear radiance and needs gamma
	//! encoding on the way out; plain DLSS hands over gamma already and must not be encoded twice.
	//! True when this frame's Ray Reconstruction ran against a linearised input, so its output
	//! is linear and has to be encoded on the way to the display.
	bool m_rrLinearIn = false;
	bool tonemapToBackbuffer(const Frame & frame, bool linearSource);
	//! Scene colour (gamma) to linear fp16, the input Ray Reconstruction actually asks for.
	bool linearizeSceneColour(const Frame & frame);
	void * frameToken();
	void setDlssg(bool on);
	void setReflex(bool on);
	void pclMarker(int marker);
	bool copyHudless(const Frame & frame);
	void releaseTargets();
	
	void * m_dll = nullptr;
	ID3D12Device * m_device = nullptr;
	bool m_inited = false;
	bool m_ready = false;
	bool m_dlss = false;
	bool m_rr = false;
	bool m_fg = false;
	bool m_reflex = false;
	bool m_fgOn = false;
	bool m_reflexOn = false;
	bool m_fgTagged = false;
	bool m_loggedOn = false;
	bool m_loggedOff = false;
	bool m_loggedRrSkip = false;
	bool m_rrLive = false;
	bool m_rrFailed = false;
	bool m_constantsSet = false;
	bool m_firstFrame = true;
	bool m_loggedFg = false;
	bool m_loggedUntagged = false;
	bool m_targetsFailed = false;
	void * m_token = nullptr;
	int m_inW = 0;
	int m_inH = 0;
	int m_outW = 0;
	int m_outH = 0;
	unsigned m_targetSet = 0;
	mutable int m_optMode = 0;
	mutable int m_optOutW = 0;
	mutable int m_optOutH = 0;
	mutable int m_optInW = 0;
	mutable int m_optInH = 0;
	mutable bool m_optValid = false;
	glm::mat4x4 m_prevViewProj = glm::mat4x4(1.f);
	glm::mat4x4 m_invPrevViewProj = glm::mat4x4(1.f);
	glm::mat4x4 m_viewProj = glm::mat4x4(1.f);
	glm::mat4x4 m_invViewProj = glm::mat4x4(1.f);
	
	struct Gpu;
	Gpu * m_gpu = nullptr;
};

#endif // ARX_HAVE_D3D12

#endif // ARX_GRAPHICS_DXR_D3D12STREAMLINE_H
