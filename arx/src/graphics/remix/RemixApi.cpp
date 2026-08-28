/*
 * Arx Remaster — RTX Remix SDK wrapper (M1 probe)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixApi.h"

#if ARX_HAVE_RTX_REMIX

#include <sstream>
#include <string>
#include <vector>

#include "io/log/Logger.h"
#include "platform/WindowsUtils.h"

namespace {

platform::WideString g_dllOverride;

platform::WideString widePath(const wchar_t * path) {
	platform::WideString result;
	if(path) {
		result = path;
	}
	return result;
}

platform::WideString pathIfFile(const platform::WideString & path) {
	if(path.size() == 0) {
		return {};
	}
	const DWORD attrs = GetFileAttributesW(path);
	if(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		return path;
	}
	return {};
}

} // namespace

RemixApi::~RemixApi() {
	shutdownRenderer();
	unloadLibrary();
}

remixapi_ErrorCode RemixApi::load(const wchar_t * dllPath) {
	
	if(!dllPath || !dllPath[0]) {
		return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
	}
	
	shutdown();
	
	const remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(dllPath, &m_api, &m_dll);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogError << "Remix: failed to load d3d9.dll (" << errorString(status) << ", code " << int(status) << ')';
		m_api = {};
		m_dll = nullptr;
	}
	
	return status;
}

remixapi_ErrorCode RemixApi::startup(HWND hwnd, bool editorMode) {
	
	if(!m_dll || !m_api.Startup) {
		return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
	}
	
	remixapi_StartupInfo startInfo {};
	startInfo.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
	startInfo.hwnd = hwnd;
	startInfo.disableSrgbConversionForOutput = FALSE;
	startInfo.forceNoVkSwapchain = FALSE;
	// Editor mode gives the Remix developer menu (Alt+X) and its Debug View
	// inside the preview window. combineGuiInFinalColor keeps that UI in the
	// presented image instead of a separate buffer we would have to fetch.
	startInfo.editorModeEnabled = editorMode ? TRUE : FALSE;
	startInfo.combineGuiInFinalColor = TRUE;
	
	const remixapi_ErrorCode status = m_api.Startup(&startInfo);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogError << "Remix: Startup failed (" << errorString(status) << ", code " << int(status) << ')';
		return status;
	}
	
	m_started = true;
	return REMIXAPI_ERROR_CODE_SUCCESS;
}

void RemixApi::shutdownRenderer() {
	
	if(m_started && m_api.Shutdown) {
		const remixapi_ErrorCode status = m_api.Shutdown();
		if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
			LogWarning << "Remix: Shutdown returned " << errorString(status) << " (code " << int(status) << ')';
		}
		m_started = false;
	}
}

void RemixApi::unloadLibrary() {
	
	if(m_dll) {
		FreeLibrary(m_dll);
		m_dll = nullptr;
	}
	
	m_api = {};
}

void RemixApi::shutdown() {
	shutdownRenderer();
	unloadLibrary();
}

const char * RemixApi::errorString(remixapi_ErrorCode code) {
	switch(code) {
		case REMIXAPI_ERROR_CODE_SUCCESS: return "success";
		case REMIXAPI_ERROR_CODE_GENERAL_FAILURE: return "general failure";
		case REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE: return "LoadLibrary failed";
		case REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS: return "invalid arguments";
		case REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE: return "GetProcAddress failed";
		case REMIXAPI_ERROR_CODE_ALREADY_EXISTS: return "already exists";
		case REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE: return "non-Remix D3D9 device";
		case REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED: return "D3D9 device not registered";
		case REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION: return "incompatible API version";
		case REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE: return "SetDllDirectory failed";
		case REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE: return "GetFullPathName failed";
		case REMIXAPI_ERROR_CODE_NOT_INITIALIZED: return "not initialized";
		case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES: return "GPU lacks RTX features";
		case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM: return "driver below minimum";
		case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL: return "DXVK instance extension fail";
		case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL: return "VkCreateInstance failed";
		case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return "VkCreateDevice failed";
		case REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING: return "graphics queue family missing";
		default: return "unknown error";
	}
}

std::string RemixApi::headerVersionString() {
	std::ostringstream oss;
	oss << REMIXAPI_VERSION_MAJOR << '.' << REMIXAPI_VERSION_MINOR << '.' << REMIXAPI_VERSION_PATCH;
	return oss.str();
}

void RemixApi::setDllPathOverride(const std::string & utf8Path) {
	g_dllOverride = utf8Path;
}

std::wstring RemixApi::requestedRuntimeDll() {
	
	if(g_dllOverride.size() > 0) {
		const platform::WideString resolved = pathIfFile(g_dllOverride);
		if(resolved.size() > 0) {
			return resolved.c_str();
		}
		LogWarning << "Remix: --remix-dll is set but file not found: " << g_dllOverride.toUTF8();
		return {};
	}
	
	const wchar_t * env = _wgetenv(L"ARX_REMIX_DLL");
	if(env && env[0]) {
		const platform::WideString fromEnv = pathIfFile(widePath(env));
		if(fromEnv.size() > 0) {
			return fromEnv.c_str();
		}
		LogWarning << "Remix: ARX_REMIX_DLL is set but file not found: "
		           << platform::WideString::toUTF8(env);
	}
	
	return {};
}

std::wstring RemixApi::findRuntimeDll() {
	
	const std::wstring requested = requestedRuntimeDll();
	if(!requested.empty()) {
		return requested;
	}
	
	std::vector<platform::WideString> candidates;
	
	const platform::WideString exeDir = platform::getModuleFileName(nullptr);
	const std::wstring exeDirW(exeDir.c_str());
	if(exeDirW.size() > 0) {
		const size_t slash = exeDirW.find_last_of(L"\\/");
		if(slash != std::wstring::npos) {
			const std::wstring base = exeDirW.substr(0, slash + 1);
			candidates.push_back(widePath((base + L".trex\\d3d9.dll").c_str()));
			candidates.push_back(widePath((base + L"d3d9.dll").c_str()));
		}
	}
	
	candidates.push_back(widePath(L".trex\\d3d9.dll"));
	candidates.push_back(widePath(L"runtime\\remix-extract\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"..\\runtime\\remix-extract\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"..\\..\\runtime\\remix-extract\\.trex\\d3d9.dll"));
	
	for(const platform::WideString & candidate : candidates) {
		const platform::WideString resolved = pathIfFile(candidate);
		if(resolved.size() > 0) {
			return resolved.c_str();
		}
	}
	
	return {};
}

#endif // ARX_HAVE_RTX_REMIX
