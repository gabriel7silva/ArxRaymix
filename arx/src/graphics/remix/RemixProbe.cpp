/*
 * Arx Remaster — RTX Remix M1 probe (SDL window + path-traced test triangle)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixProbe.h"

#if ARX_HAVE_RTX_REMIX

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "Configure.h"

#include "graphics/remix/RemixApi.h"
#include "graphics/remix/RemixConvert.h"
#include "io/log/Logger.h"
#include "platform/Environment.h"
#include "platform/ProgramOptions.h"
#include "platform/WindowsUtils.h"
#include "util/cmdline/Optional.h"

namespace remix {
namespace {

bool g_probeRequested = false;
unsigned g_probeFrames = 0; // 0 = keep presenting until the window is closed
platform::WideString g_dllOverride;

static remixapi_HardcodedVertex makeVertex(float x, float y, float z) {
	remixapi_HardcodedVertex v {};
	v.position[0] = x;
	v.position[1] = y;
	v.position[2] = z;
	v.normal[0] = 0.f;
	v.normal[1] = 0.f;
	v.normal[2] = -1.f;
	v.color = 0xFFFFFFFFu;
	return v;
}

static void enableProbe(const util::cmdline::optional<std::string> & frames) {
	g_probeRequested = true;
	g_probeFrames = 0;
	if(frames) {
		const char * begin = frames->c_str();
		char * end = nullptr;
		const unsigned long parsed = std::strtoul(begin, &end, 10);
		if(end == begin || (end && *end != '\0') || parsed == 0) {
			throw util::cmdline::error(util::cmdline::error::invalid_cmd_syntax,
			                           "invalid frame count \"" + *frames + "\"");
		}
		g_probeFrames = unsigned(parsed);
	}
}

static void setRemixDll(const std::string & path) {
	g_dllOverride = path;
	RemixApi::setDllPathOverride(path);
}

ARX_PROGRAM_OPTION_ARG("remix-probe", "", "Run RTX Remix SDK probe until the window is closed (optional frame cap for CI)", &enableProbe, "FRAMES")
ARX_PROGRAM_OPTION_ARG("remix-dll", "", "Path to the Remix d3d9.dll to render through", &setRemixDll, "PATH")

static platform::WideString widePath(const wchar_t * path) {
	platform::WideString result;
	if(path) {
		result = path;
	}
	return result;
}

static platform::WideString pathExists(const platform::WideString & path) {
	if(path.size() == 0) {
		return {};
	}
	const DWORD attrs = GetFileAttributesW(path);
	if(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		return path;
	}
	return {};
}

static platform::WideString findRemixDll() {
	
	if(g_dllOverride.size() > 0) {
		return pathExists(g_dllOverride);
	}
	
	const wchar_t * env = _wgetenv(L"ARX_REMIX_DLL");
	if(env && env[0]) {
		const platform::WideString fromEnv = pathExists(widePath(env));
		if(fromEnv.size() > 0) {
			return fromEnv;
		}
		LogWarning << "Remix: ARX_REMIX_DLL is set but file not found: " << platform::WideString::toUTF8(env);
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
	candidates.push_back(widePath(L"runtime\\remix\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"..\\runtime\\remix\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"..\\..\\runtime\\remix\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"runtime\\remix-extract\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"..\\runtime\\remix-extract\\.trex\\d3d9.dll"));
	candidates.push_back(widePath(L"..\\..\\runtime\\remix-extract\\.trex\\d3d9.dll"));
	
	for(const platform::WideString & candidate : candidates) {
		const platform::WideString resolved = pathExists(candidate);
		if(resolved.size() > 0) {
			return resolved;
		}
	}
	
	return {};
}

static HWND getSdlNativeWindow(SDL_Window * window) {
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if(!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
		return nullptr;
	}
	return info.info.win.window;
}

static remixapi_ErrorCode createTestScene(RemixApi & api, remixapi_MeshHandle & mesh, remixapi_LightHandle & light) {
	
	remixapi_LightInfoSphereEXT sphereLight {};
	sphereLight.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphereLight.position = { 0.f, -1.f, 0.f };
	sphereLight.radius = 0.1f;
	sphereLight.shaping_hasvalue = FALSE;
	
	remixapi_LightInfo lightInfo {};
	lightInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	lightInfo.pNext = &sphereLight;
	lightInfo.hash = 0x3;
	lightInfo.radiance = { 100.f, 200.f, 100.f };
	
	remixapi_ErrorCode status = api.iface().CreateLight(&lightInfo, &light);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogError << "Remix: CreateLight failed (" << RemixApi::errorString(status) << ')';
		return status;
	}
	
	const remixapi_HardcodedVertex verts[] = {
		makeVertex( 5.f, -5.f, 10.f),
		makeVertex( 0.f,  5.f, 10.f),
		makeVertex(-5.f, -5.f, 10.f),
	};
	
	remixapi_MeshInfoSurfaceTriangles triangles {};
	triangles.vertices_values = verts;
	triangles.vertices_count = std::size(verts);
	triangles.skinning_hasvalue = FALSE;
	triangles.material = nullptr;
	
	remixapi_MeshInfo meshInfo {};
	meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash = 0x1;
	meshInfo.surfaces_values = &triangles;
	meshInfo.surfaces_count = 1;
	
	status = api.iface().CreateMesh(&meshInfo, &mesh);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogError << "Remix: CreateMesh failed (" << RemixApi::errorString(status) << ')';
		api.iface().DestroyLight(light);
		light = nullptr;
		return status;
	}
	
	return REMIXAPI_ERROR_CODE_SUCCESS;
}

static remixapi_ErrorCode renderTestFrame(RemixApi & api, remixapi_MeshHandle mesh, remixapi_LightHandle light,
                                          unsigned width, unsigned height) {
	
	remixapi_CameraInfoParameterizedEXT cameraParams {};
	cameraParams.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
	cameraParams.position = { 0.f, 0.f, 0.f };
	cameraParams.forward = { 0.f, 0.f, 1.f };
	cameraParams.up = { 0.f, 1.f, 0.f };
	cameraParams.right = { 1.f, 0.f, 0.f };
	cameraParams.fovYInDegrees = 70.f;
	cameraParams.aspect = width > 0 ? float(width) / float(height) : 1.f;
	cameraParams.nearPlane = 0.1f;
	cameraParams.farPlane = 1000.f;
	
	remixapi_CameraInfo cameraInfo {};
	cameraInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	cameraInfo.pNext = &cameraParams;
	
	remixapi_ErrorCode status = api.iface().SetupCamera(&cameraInfo);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		return status;
	}
	
	remixapi_InstanceInfo instance {};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.mesh = mesh;
	instance.doubleSided = TRUE;
	instance.transform = { {
		{ 1.f, 0.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.f, 0.f },
		{ 0.f, 0.f, 1.f, 0.f },
	} };
	
	status = api.iface().DrawInstance(&instance);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		return status;
	}
	
	status = api.iface().DrawLightInstance(light);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		return status;
	}
	
	remixapi_PresentInfo presentInfo {};
	presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
	return api.iface().Present(&presentInfo);
}

static void exitProbe(int code) {
	// Remix hooks the SDL HWND; orderly SDL/Remix teardown crashes in d3d9.dll.
	// For the M1 probe, flush logs and terminate the process immediately.
	Logger::quickShutdown();
	_exit(code);
}

static void cleanupProbeWindow(SDL_Window * window, RemixApi * remix, bool rendererStarted) {
	(void)window;
	if(remix && rendererStarted) {
		exitProbe(EXIT_FAILURE);
	}
	if(remix) {
		remix->unloadLibrary();
	}
	SDL_Quit();
}

} // namespace

bool probeRequested() {
	return g_probeRequested;
}

ExitStatus runProbe() {
	
	LogInfo << "Remix probe: renderer=remix, header API " << RemixApi::headerVersionString();
	
	const platform::WideString dllPath = findRemixDll();
	if(dllPath.size() == 0) {
		LogError << "Remix probe: x64 .trex/d3d9.dll not found.";
		LogError << "Extract RTX Remix 1.5.2 runtime to runtime/remix-extract/ (see arx/third_party/rtx-remix/README.md)";
		LogError << "Use the renderer DLL at .trex/d3d9.dll (not the x86 bridge at the package root).";
		LogError << "Or pass --remix-dll=PATH or set ARX_REMIX_DLL.";
		return ExitFailure;
	}
	
	LogInfo << "Remix probe: loading " << dllPath.toUTF8();
	
	if(SDL_Init(SDL_INIT_VIDEO) != 0) {
		LogError << "Remix probe: SDL_Init failed: " << SDL_GetError();
		return ExitFailure;
	}
	
	const int windowWidth = 1280;
	const int windowHeight = 720;
	
	SDL_Window * window = SDL_CreateWindow(
		"Arx Remaster — Remix probe",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		windowWidth, windowHeight,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
	);
	
	if(!window) {
		LogError << "Remix probe: SDL_CreateWindow failed: " << SDL_GetError();
		SDL_Quit();
		return ExitFailure;
	}
	
	const HWND hwnd = getSdlNativeWindow(window);
	if(!hwnd) {
		LogError << "Remix probe: could not obtain native HWND from SDL window";
		SDL_Quit();
		return ExitFailure;
	}
	
	LogInfo << "Remix probe: SDL window " << windowWidth << 'x' << windowHeight << ", HWND=" << hwnd;
	
	RemixApi remix;
	remixapi_ErrorCode status = remix.load(dllPath.c_str());
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		SDL_Quit();
		remix.unloadLibrary();
		return ExitFailure;
	}
	
	status = remix.startup(hwnd, debugEnabled(DebugEditorMode));
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		cleanupProbeWindow(window, &remix, false);
		return ExitFailure;
	}
	
	LogInfo << "Remix probe: Startup succeeded";
	if(g_probeFrames == 0) {
		LogInfo << "Remix probe: presenting until the window is closed (Esc or close button)";
	} else {
		LogInfo << "Remix probe: presenting " << g_probeFrames << " frame(s) then exiting";
	}
	
	remixapi_MeshHandle mesh = nullptr;
	remixapi_LightHandle light = nullptr;
	status = createTestScene(remix, mesh, light);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		cleanupProbeWindow(window, &remix, true);
		return ExitFailure;
	}
	
	unsigned framesPresented = 0;
	bool quit = false;
	while(!quit) {
		int w = windowWidth;
		int h = windowHeight;
		SDL_GetWindowSize(window, &w, &h);
		const unsigned uw = w > 0 ? unsigned(w) : 1u;
		const unsigned uh = h > 0 ? unsigned(h) : 1u;
		
		status = renderTestFrame(remix, mesh, light, uw, uh);
		if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
			LogError << "Remix probe: frame " << framesPresented << " failed ("
			         << RemixApi::errorString(status) << ')';
			break;
		}
		++framesPresented;
		if(framesPresented == 1 || (framesPresented % 60) == 0) {
			LogInfo << "Remix probe: presented frame " << framesPresented << " (" << uw << 'x' << uh << ')';
		}
		
		SDL_Event event {};
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_QUIT) {
				quit = true;
			} else if(event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
				quit = true;
			} else if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
				quit = true;
			}
		}
		
		if(g_probeFrames > 0 && framesPresented >= g_probeFrames) {
			quit = true;
		}
		
		SDL_Delay(16);
	}
	
	if(mesh) {
		remix.iface().DestroyMesh(mesh);
	}
	if(light) {
		remix.iface().DestroyLight(light);
	}
	
	if(framesPresented > 0 && status == REMIXAPI_ERROR_CODE_SUCCESS) {
		LogInfo << "Remix probe: finished successfully (" << framesPresented << " frame(s) presented)";
		exitProbe(EXIT_SUCCESS);
	}
	
	LogError << "Remix probe: no frame presented or render error";
	exitProbe(EXIT_FAILURE);
}

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX
