/*
 * Arx Raymix — NVIDIA Streamline (DLSS / DLSS-RR) on the D3D12 path.
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
	};
	
	D3D12Streamline();
	~D3D12Streamline();
	
	//! slInit. Call before CreateDXGIFactory / D3D12CreateDevice.
	bool init();
	//! slSetD3DDevice + feature query. Call immediately after the device exists.
	bool setDevice(ID3D12Device * device, IDXGIAdapter1 * adapter);
	void shutdown();
	
	[[nodiscard]] bool ready() const { return m_ready; }
	[[nodiscard]] bool supportsDlss() const { return m_dlss; }
	[[nodiscard]] bool supportsRr() const { return m_rr; }
	
	//! cfg dxr_dlss (0 Off / 1 DLAA / 2–5 quality) → Streamline DLSSMode as int (0 = Off).
	static int resolveDlssMode(int setting, int outputHeight);
	//! Optimal internal render size for a Streamline DLSSMode. Falls back to scale tables.
	bool queryOptimalSize(int slMode, int outputW, int outputH, int & renderW, int & renderH) const;
	
	//! After world raster + DXR, before particles / HUD. Restores nothing — caller
	//! must restoreRasterBind() after this returns.
	bool evaluate(const Frame & frame);
	
	void resize(int width, int height);
	
private:
	bool loadLibrary();
	bool ensureTargets(int inputW, int inputH, int outputW, int outputH);
	bool rasterGbuffers(const Frame & frame);
	bool setConstants(const Frame & frame, void * token);
	bool evaluateRr(const Frame & frame, void * token);
	bool evaluateDlss(const Frame & frame, void * token);
	bool tonemapToBackbuffer(const Frame & frame);
	void releaseTargets();
	
	void * m_dll = nullptr;
	ID3D12Device * m_device = nullptr;
	bool m_inited = false;
	bool m_ready = false;
	bool m_dlss = false;
	bool m_rr = false;
	bool m_loggedOn = false;
	bool m_loggedOff = false;
	bool m_firstFrame = true;
	int m_inW = 0;
	int m_inH = 0;
	int m_outW = 0;
	int m_outH = 0;
	glm::mat4x4 m_prevViewProj = glm::mat4x4(1.f);
	
	struct Gpu;
	Gpu * m_gpu = nullptr;
};

#endif // ARX_HAVE_D3D12

#endif // ARX_GRAPHICS_DXR_D3D12STREAMLINE_H
