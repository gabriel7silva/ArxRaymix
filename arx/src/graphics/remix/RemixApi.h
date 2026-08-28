/*
 * Arx Remaster — RTX Remix SDK wrapper (M1 probe)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXAPI_H
#define ARX_GRAPHICS_REMIX_REMIXAPI_H

#include <string>
#include <vector>

#include "Configure.h"

#if ARX_HAVE_RTX_REMIX

#include <windows.h>

#include <remix/remix_c.h>

class RemixApi {
public:
	
	RemixApi() = default;
	~RemixApi();
	
	RemixApi(const RemixApi &) = delete;
	RemixApi & operator=(const RemixApi &) = delete;
	
	[[nodiscard]] remixapi_ErrorCode load(const wchar_t * dllPath);
	[[nodiscard]] remixapi_ErrorCode startup(HWND hwnd, bool editorMode = false);
	//! Release the Remix renderer; keep d3d9.dll mapped until unloadLibrary().
	void shutdownRenderer();
	//! Unmap d3d9.dll. Call only after the SDL HWND is destroyed.
	void unloadLibrary();
	//! shutdownRenderer() followed by unloadLibrary().
	void shutdown();
	
	[[nodiscard]] bool isLoaded() const { return m_dll != nullptr; }
	[[nodiscard]] bool isStarted() const { return m_started; }
	[[nodiscard]] HMODULE module() const { return m_dll; }
	[[nodiscard]] remixapi_Interface & iface() { return m_api; }
	[[nodiscard]] const remixapi_Interface & iface() const { return m_api; }
	
	[[nodiscard]] static const char * errorString(remixapi_ErrorCode code);
	[[nodiscard]] static std::string headerVersionString();
	static void setDllPathOverride(const std::string & utf8Path);
	//! `--remix-dll` or `ARX_REMIX_DLL` only. Empty if the user did not ask, or the file is missing.
	[[nodiscard]] static std::wstring requestedRuntimeDll();
	[[nodiscard]] static std::wstring findRuntimeDll();
	
private:
	
	remixapi_Interface m_api {};
	HMODULE m_dll = nullptr;
	bool m_started = false;
};

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXAPI_H
