/*
 * Arx Remaster — present the current room to RTX Remix on the single SDL window.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/remix/RemixScene.h"

#if ARX_HAVE_RTX_REMIX

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

#include <glm/glm.hpp>

#include "cinematic/CinematicController.h"
#include "core/Config.h"
#include "core/Core.h"
#include "gui/Menu.h"
#include "game/Camera.h"
#include "game/EntityManager.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/remix/RemixApi.h"
#include "graphics/remix/RemixConvert.h"
#include "graphics/remix/RemixEntities.h"
#include "graphics/remix/RemixTextures.h"
#include "io/log/Logger.h"
#include "math/Rectangle.h"
#include "platform/Platform.h"
#include "platform/ProgramOptions.h"
#include "platform/WindowsUtils.h"
#include "scene/Light.h"
#include "scene/Tiles.h"

namespace remix {
namespace {

bool g_sceneRequested = false;
bool g_failed = false;
bool g_started = false;
bool g_dllHooked = false;
SDL_Window * g_window = nullptr;
RemixApi g_api;
remixapi_MeshHandle g_mesh = nullptr;
constexpr size_t kMaxLevelLights = 64;

/*
 * Lights are keyed by the EERIE_LIGHT they came from, not by their rank in the
 * distance-sorted list. With a rank key, walking across the room silently made
 * light 3 a different physical light, and Remix - which matches lights across
 * frames by hash - threw away its temporal history every time the order shifted.
 */
struct CachedLight {
	remixapi_LightHandle handle = nullptr;
	Vec3f pos = Vec3f(0.f);
	remixapi_Float3D radiance {};
	float radius = 0.f;
	unsigned frame = 0;
};
std::map<const EERIE_LIGHT *, CachedLight> g_levelLights;

Vec2s g_meshTile(-1, -1);
unsigned g_frames = 0;

//! Set by notifyLevelRendered(), cleared by presentFrame(). See RemixScene.h.
bool g_levelRendered = false;

//! Set by notifyOverlayFrame(), cleared by presentFrame(). See RemixScene.h.
bool g_overlayFrame = false;

bool overlayFrameActive() {
	// Logos never change ARXmenu off Mode_InGame. Credits / CC / main menu do.
	return g_overlayFrame
	       || ARXmenu.mode() == Mode_MainMenu
	       || ARXmenu.mode() == Mode_Credits
	       || ARXmenu.mode() == Mode_CharacterCreation;
}

/*!
 * True for a gameplay frame, false for menus, loading, logos and cinematics.
 *
 * Both the camera the UI quads are built against (queue2DVertices) and the
 * camera handed to Remix (setupScene) have to agree on this, or the UI ends up
 * positioned relative to one camera and rendered through another.
 */
bool sceneIs3D() {
	return !overlayFrameActive() && g_levelRendered && g_tiles && !isInCinematic()
	       && ARXmenu.mode() == Mode_InGame;
}

//! -1 = not yet applied; 0 = overlay 2D; 1 = in-game user gfx.
int g_renderModeApplied = -1;

// --remix-debug scratch state: the probe triangle drawn in front of the camera
// and the first exported vertex, both only used to make the log verifiable.
remixapi_MeshHandle g_debugMesh = nullptr;
remixapi_LightHandle g_debugLight = nullptr;
Vec3f g_meshFirstVertex(0.f);

constexpr int kPreviewFallbackWidth = 1600;
constexpr int kPreviewFallbackHeight = 900;
int g_previewWidth = kPreviewFallbackWidth;
int g_previewHeight = kPreviewFallbackHeight;
constexpr size_t kMaxTriangles = 80000;

void remixCameraBasis(const Camera & cam, Vec3f & position, Vec3f & forward, Vec3f & up, Vec3f & right) {
	
	position = toRemix(cam.m_pos);
	
	forward = toRemixDir(angleToVector(cam.angle));
	const float forwardLen = glm::length(forward);
	if(forwardLen > 1e-6f) {
		forward /= forwardLen;
	} else {
		forward = Vec3f(0.f, 0.f, 1.f);
	}
	
	// Ceiling direction in Remix space (Arx world-up is -Y).
	const Vec3f worldUp(0.f, 1.f, 0.f);
	right = glm::cross(worldUp, forward);
	float rightLen2 = glm::dot(right, right);
	if(rightLen2 < 1e-8f) {
		right = glm::cross(Vec3f(1.f, 0.f, 0.f), forward);
		rightLen2 = glm::dot(right, right);
		if(rightLen2 < 1e-8f) {
			right = Vec3f(1.f, 0.f, 0.f);
			rightLen2 = 1.f;
		}
	}
	right /= std::sqrt(rightLen2);
	up = glm::normalize(glm::cross(forward, right));
}

/*!
 * Same basis as remixCameraBasis, but in Arx units / Y-down — matching the
 * D3DFVF_XYZ verts the hook submits. toRemix() here would put SetupCamera 100x
 * away from the captured geometry.
 */
void arxHookCameraBasis(const Camera & cam, Vec3f & position, Vec3f & forward, Vec3f & up, Vec3f & right) {
	
	position = cam.m_pos;
	
	forward = angleToVector(cam.angle);
	const float forwardLen = glm::length(forward);
	if(forwardLen > 1e-6f) {
		forward /= forwardLen;
	} else {
		forward = Vec3f(0.f, 0.f, 1.f);
	}
	
	const Vec3f worldUp(0.f, -1.f, 0.f);
	right = glm::cross(worldUp, forward);
	float rightLen2 = glm::dot(right, right);
	if(rightLen2 < 1e-8f) {
		right = glm::cross(Vec3f(1.f, 0.f, 0.f), forward);
		rightLen2 = glm::dot(right, right);
		if(rightLen2 < 1e-8f) {
			right = Vec3f(1.f, 0.f, 0.f);
			rightLen2 = 1.f;
		}
	}
	right /= std::sqrt(rightLen2);
	up = glm::normalize(glm::cross(forward, right));
}

void setupDllHookCamera() {
	
	if(!g_api.iface().SetupCamera) {
		return;
	}
	
	const Camera & view = (g_camera != nullptr) ? *g_camera : g_playerCamera;
	Vec3f position, forward, up, right;
	arxHookCameraBasis(view, position, forward, up, right);
	
	int w = g_size.width();
	int h = g_size.height();
	if(g_window) {
		SDL_GetWindowSize(g_window, &w, &h);
	}
	w = std::max(w, 1);
	h = std::max(h, 1);
	
	remixapi_CameraInfo camera {};
	camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
	
	remixapi_CameraInfoParameterizedEXT params {};
	params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
	params.position = { position.x, position.y, position.z };
	params.forward = { forward.x, forward.y, forward.z };
	params.up = { up.x, up.y, up.z };
	params.right = { right.x, right.y, right.z };
	float fovY = view.fov();
	if(!std::isfinite(fovY) || fovY < 0.05f || view.focal < 10.f) {
		fovY = glm::radians(60.f);
	}
	params.fovYInDegrees = glm::degrees(fovY);
	params.aspect = float(w) / float(h);
	params.nearPlane = 1.f;
	params.farPlane = std::max(view.cdepth > 1.f ? view.cdepth : 8000.f, 1.f);
	camera.pNext = &params;
	
	const remixapi_ErrorCode status = g_api.iface().SetupCamera(&camera);
	static remixapi_ErrorCode s_logged = REMIXAPI_ERROR_CODE_SUCCESS;
	static bool s_ok = false;
	if(!s_ok || status != s_logged) {
		s_logged = status;
		if(status == REMIXAPI_ERROR_CODE_SUCCESS) {
			s_ok = true;
			LogInfo << "Remix dll hook: SetupCamera Arx-space pos=("
			        << position.x << ',' << position.y << ',' << position.z
			        << ") fovY=" << params.fovYInDegrees << " aspect=" << params.aspect;
		} else {
			LogError << "Remix dll hook: SetupCamera failed ("
			         << RemixApi::errorString(status) << ')';
		}
	}
}

void enableScene() {
	g_sceneRequested = true;
	LogWarning << "--remix-scene is ignored: C API play is off. Pass --remix-dll so D3D9Renderer loads Remix.";
}
ARX_PROGRAM_OPTION("remix-scene", "", "Ignored: C API play is off. Use --remix-dll instead.", &enableScene)

HWND nativeHwnd(SDL_Window * window) {
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if(!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
		return nullptr;
	}
	return info.info.win.window;
}

remixapi_HardcodedVertex toVertex(const Vec3f & p, const Vec3f & n, Vec2f uv, ColorRGBA /*color*/) {
	remixapi_HardcodedVertex v {};
	const Vec3f rp = toRemix(p);
	v.position[0] = rp.x;
	v.position[1] = rp.y;
	v.position[2] = rp.z;
	const Vec3f rn = toRemixDir(n);
	const float len = std::sqrt(rn.x * rn.x + rn.y * rn.y + rn.z * rn.z);
	if(len > 1e-6f) {
		v.normal[0] = rn.x / len;
		v.normal[1] = rn.y / len;
		v.normal[2] = rn.z / len;
	} else {
		v.normal[1] = 1.f;
	}
	v.texcoord[0] = uv.x;
	v.texcoord[1] = uv.y;
	// Arx vertex color is baked lighting (near-black in the cell). Remix would
	// use it as albedo and the path tracer would light a black room.
	v.color = 0xFFFFFFFFu;
	return v;
}

void appendTri(std::vector<remixapi_HardcodedVertex> & out, const EERIEPOLY & poly, int a, int b, int c) {
	// Y-flip is a reflection, so reverse winding (a, c, b) to keep front faces.
	out.push_back(toVertex(poly.v[a].p, poly.nrml[a], poly.v[a].uv, poly.v[a].color));
	out.push_back(toVertex(poly.v[c].p, poly.nrml[c], poly.v[c].uv, poly.v[c].color));
	out.push_back(toVertex(poly.v[b].p, poly.nrml[b], poly.v[b].uv, poly.v[b].color));
}

bool skipPoly(const EERIEPOLY & poly) {
	if(poly.type & (POLY_NODRAW | POLY_HIDE | POLY_IGNORE | POLY_WATER)) {
		return true;
	}
	if(poly.type & POLY_LAVA) {
		return false;
	}
	if(poly.type & POLY_TRANS) {
		return !isCutoutTexture(poly.tex);
	}
	return false;
}

void destroyMesh() {
	if(g_mesh && g_api.iface().DestroyMesh) {
		g_api.iface().DestroyMesh(g_mesh);
	}
	g_mesh = nullptr;
	g_meshTile = Vec2s(-1, -1);
}

void destroyLightHandle(remixapi_LightHandle & light) {
	if(light && g_api.iface().DestroyLight) {
		g_api.iface().DestroyLight(light);
	}
	light = nullptr;
}

void destroyLevelLights() {
	for(auto & entry : g_levelLights) {
		destroyLightHandle(entry.second.handle);
	}
	g_levelLights.clear();
}

size_t drawLevelLights() {
	size_t drawn = 0;
	for(auto & entry : g_levelLights) {
		if(entry.second.handle) {
			g_api.iface().DrawLightInstance(entry.second.handle);
			++drawn;
		}
	}
	return drawn;
}

void destroyDebugScene() {
	if(g_debugMesh && g_api.iface().DestroyMesh) {
		g_api.iface().DestroyMesh(g_debugMesh);
	}
	g_debugMesh = nullptr;
	destroyLightHandle(g_debugLight);
}

bool createSphereLight(remixapi_LightHandle & out, uint64_t hash, const Vec3f & pos, float radius,
                       remixapi_Float3D radiance) {
	
	destroyLightHandle(out);
	
	remixapi_LightInfoSphereEXT sphere {};
	sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
	sphere.position = { pos.x, pos.y, pos.z };
	sphere.radius = radius;
	sphere.shaping_hasvalue = FALSE;
	sphere.volumetricRadianceScale = 1.f;
	
	remixapi_LightInfo info {};
	info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
	info.pNext = &sphere;
	info.hash = hash;
	info.radiance = radiance;
	
	const remixapi_ErrorCode status = g_api.iface().CreateLight(&info, &out);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		static remixapi_ErrorCode s_last = REMIXAPI_ERROR_CODE_SUCCESS;
		static unsigned s_fails = 0;
		++s_fails;
		if(s_fails <= 3 || status != s_last || (g_frames % 120) == 0) {
			LogError << "Remix scene: CreateLight failed (" << RemixApi::errorString(status)
			         << ") x" << s_fails;
		}
		s_last = status;
		out = nullptr;
		return false;
	}
	return true;
}

void applyPreviewConfigImpl() {
	
	if(!g_api.iface().SetConfigVariable) {
		return;
	}

	/*
	 * Structural first, and unconditionally: these describe the geometry we
	 * submit, not how it should look. Getting them wrong renders the world
	 * upside down or at the wrong scale, so they are not up for negotiation
	 * even when the developer menu is allowed to win below.
	 */
	if(g_dllHooked) {
		// D3D9 is left-handed, Y-down. Remix defaults assume a right-handed Z-up
		// capture game; the VIEW matrix still drives the camera, but GI/specular
		// sampling follows this. 1 Arx unit ≈ 1 cm.
		g_api.iface().SetConfigVariable("rtx.leftHandedCoordinateSystem", "True");
		g_api.iface().SetConfigVariable("rtx.zUp", "False");
		g_api.iface().SetConfigVariable("rtx.sceneScale", "1.0");
		/*
		 * Off, not on.
		 *
		 * This was set to True to stop Remix logging "not detecting a valid
		 * camera" back when nothing was being traced at all. That symptom
		 * belonged to dxvk_CreateD3D9, which never fed the path tracer; on the
		 * capture path the camera is accepted without it.
		 *
		 * Camera.cpp already flips Y on the way from NDC to screen, so
		 * correcting the flip a second time renders the world upside down.
		 */
		g_api.iface().SetConfigVariable("rtx.camera.correctProjectionYFlip", "False");
	}

	if(debugEnabled(DebugFreeConfig)) {
		LogInfo << "Remix scene: free config - materials, exposure and lighting left to "
		           "rtx.conf, user.conf and the developer menu";
		return;
	}

	// The Remix runtime persists whatever the developer menu was left on into
	// user.conf in the working directory, and one stray toggle there (notably
	// rtx.enableRaytracing = False) turns the preview black on every later run.
	// Pin the settings the preview depends on instead of trusting that file.
	g_api.iface().SetConfigVariable("rtx.enableRaytracing", "True");
	// Never: a camera-attached fallback sphere is visible in frame and blows autoexposure.
	g_api.iface().SetConfigVariable("rtx.fallbackLightMode", "0");
	g_api.iface().SetConfigVariable("rtx.volumetrics.enable", "False");
	g_api.iface().SetConfigVariable("rtx.usePostFilter", "False");
	// Floors are seen at a grazing angle, where isotropic mip selection picks a
	// level far too low and the slabs dissolve. Verified on its own: this single
	// option brings the floor joints back.
	g_api.iface().SetConfigVariable("rtx.maxAnisotropySamples", "16");
	if(!debugEnabled(DebugNoBakedLighting)) {
		// Arx's bake carries every level light, including the ones that never fit
		// through fixed-function D3D9. Treated as baked lighting rather than as
		// albedo, so the runtime knows it is light and not paint on the texture.
		g_api.iface().SetConfigVariable("rtx.ignoreAllVertexColorBakedLighting", "False");
		g_api.iface().SetConfigVariable("rtx.vertexColorIsBakedLighting", "True");
		g_api.iface().SetConfigVariable("rtx.vertexColorStrength", "1.0");
		LogInfo << "Remix scene: Arx baked vertex lighting kept";
	} else {
		g_api.iface().SetConfigVariable("rtx.ignoreAllVertexColorBakedLighting", "True");
		g_api.iface().SetConfigVariable("rtx.vertexColorIsBakedLighting", "False");
		g_api.iface().SetConfigVariable("rtx.vertexColorStrength", "0");
	}
	g_api.iface().SetConfigVariable("rtx.autoExposure.enabled", "True");
	g_api.iface().SetConfigVariable("rtx.tonemap.exposureBias", "-0.6");
	// NRC fails to init on the 4050 6 GB (every session). Pin importance sampling
	// so we do not pay the failed-init path and a surprise lighting mode switch.
	g_api.iface().SetConfigVariable("rtx.integrateIndirectMode", "2");
	if(g_dllHooked) {
		/*
		 * No USD replacements, so one roughness and one metallic describe every
		 * surface in the game.
		 *
		 * These were 0.35 and 0.12, chosen while the rasterised image was still
		 * what reached the screen and a visible specular was the only sign the
		 * tracer was doing anything. With path tracing actually presenting, that
		 * same specular is what makes wet-looking stone read as moulded plastic:
		 * a broad glossy highlight sliding across a flat wall, and a metallic
		 * tint on top of it. Dungeon stone is neither wet nor metal.
		 */
		g_api.iface().SetConfigVariable("rtx.legacyMaterial.roughnessConstant", "0.75");
		g_api.iface().SetConfigVariable("rtx.legacyMaterial.metallicConstant", "0.0");
		g_api.iface().SetConfigVariable("rtx.legacyMaterial.emissiveIntensity", "0.0");
		/*
		 * Legacy D3D9 lights are converted to Remix lights with an intensity
		 * factor applied. Ours are already in Arx units - rgb in 0..1 times the
		 * light's own intensity - so the factor is pure gain on top, and the cell
		 * comes out flat and washed rather than lit from torches.
		 *
		 * Halving it is a starting point, not a measurement. --remix-debug 262144
		 * leaves this to the developer menu so the value can be found on screen.
		 */
		g_api.iface().SetConfigVariable("rtx.lightConversionIntensityFactor", "0.5");
		/*
		 * Additively blended draws become emissive surfaces.
		 *
		 * This is how the rest of the level's lighting gets in. Only a handful
		 * of map lights fit through fixed-function D3D9 at once, out of several
		 * hundred in a level, so a room lit only by those reads inconsistently -
		 * bright where a slot happened to land, flat everywhere else. Flames,
		 * flares and magic are additive quads, and since they are now built in
		 * world space they can light what is around them, the way the baked
		 * original did.
		 */
		g_api.iface().SetConfigVariable("rtx.enableEmissiveBlendModeTranslation", "True");
		// WindowProc was not bound to the swapchain; force the developer menu
		// so we do not depend on Alt+X until that hook sticks.
		g_api.iface().SetConfigVariable("rtx.showUI", "2");
		g_api.iface().SetConfigVariable("rtx.showUICursor", "True");
		LogInfo << "Remix dll hook: matte legacy material, developer UI forced on";
	}
	LogInfo << "Remix scene: fallback light off, ignore baked vertex color, realistic dark dungeon tone";
	
	if(debugEnabled(DebugFixedExposure)) {
		// A bright emitter close to the camera drags autoexposure down until
		// everything else is crushed to black. Pin it to take that out of play.
		g_api.iface().SetConfigVariable("rtx.autoExposure.enabled", "False");
		g_api.iface().SetConfigVariable("rtx.tonemap.exposureBias", "0.0");
		LogInfo << "Remix scene: autoexposure off, exposure bias pinned";
	}
	
	if(g_debugFlags != 0) {
		// SetConfigVariable may only take effect at end of frame. If a setting
		// looks ignored, drop these next to the Remix d3d9.dll as rtx.conf.
		LogInfo << "Remix scene: debug mask " << g_debugFlags
		        << "; if config vars look ignored, put them in rtx.conf next to d3d9.dll";
	}
}

/*!
 * Menus and loading screens are 2D, so nothing there wants a path tracer.
 *
 * rtx.enableRaytracing = False is not the answer: that only falls back to
 * rasterizing the game's own D3D9 draw calls, and we issue none - the scene is
 * built entirely from remixapi instances, so the window just goes black (this
 * already happened once through a stray rtx.conf; see docs/CONFIGURATION.md).
 *
 * The equivalent is to strip every stage that makes a frame look ray traced.
 * The UI quads are emissive, and emission is resolved on the primary hit, so
 * with no bounces and no denoiser each pixel lands on its texel value:
 *
 *  - useDenoiser        temporal accumulation is the grey smear around the text
 *  - tonemappingMode 0  the local tonemapper adapts per region -> grey cloud
 *  - bloom              halos around every glyph
 *  - postfx             vignette, chromatic aberration, motion blur
 *  - autoExposure       meters off the UI itself and pumps brightness
 *  - pathMaxBounces 0   nothing in a menu to bounce light off
 */
void applyUserGfxConfigImpl() {
	
	if(debugEnabled(DebugFreeConfig)) {
		// The video options page would otherwise overwrite whatever was just
		// dialled in through the developer menu, one keystroke later.
		return;
	}
	
	auto set = [](const char * key, const char * value) {
		g_api.iface().SetConfigVariable(key, value);
	};
	
	const auto & gfx = config.video.remix;
	if(g_dllHooked) {
		// D3D9 draws exist: False is the raster of those draws, not a black Present.
		set("rtx.enableRaytracing", gfx.pathTracing ? "True" : "False");
		set("rtx.postfx.enable", "False");
		if(!gfx.pathTracing) {
			set("rtx.useDenoiser", "False");
			set("rtx.upscalerType", "3");
			set("rtx.enableRayReconstruction", "False");
			set("rtx.bloom.enable", "False");
			set("rtx.dlssfg.enable", "False");
			LogInfo << "Remix dll hook: path tracing off (D3D9 raster)";
			return;
		}
	} else {
		// Never False on the C API path: that rasterizes D3D9 draws we do not issue.
		set("rtx.enableRaytracing", "True");
		set("rtx.postfx.enable", "False");
		if(!gfx.pathTracing) {
			set("rtx.pathMaxBounces", "0");
			set("rtx.di.initialSampleCount", "4");
			set("rtx.useDenoiser", "False");
			set("rtx.upscalerType", "3");
			set("rtx.enableRayReconstruction", "False");
			set("rtx.bloom.enable", "False");
			if(debugEnabled(DebugFixedExposure)) {
				set("rtx.autoExposure.enabled", "False");
				set("rtx.tonemap.exposureBias", "0.0");
			} else {
				set("rtx.autoExposure.enabled", "True");
				set("rtx.tonemap.exposureBias", "-0.6");
			}
			LogInfo << "Remix scene: path tracing off (primary hit, no GI)";
			return;
		}
	}
	
	const char * bounces = "4";
	const char * samples = "16";
	switch(gfx.quality) {
		case 0:  bounces = "1"; samples = "4";  break;
		case 1:  bounces = "2"; samples = "8";  break;
		case 2:  bounces = "4"; samples = "16"; break;
		default: bounces = "6"; samples = "32"; break;
	}
	set("rtx.pathMaxBounces", bounces);
	set("rtx.di.initialSampleCount", samples);
	set("rtx.useDenoiser", gfx.denoiser ? "True" : "False");
	set("rtx.upscalerType", gfx.dlss ? "0" : "3");
	set("rtx.enableRayReconstruction", gfx.rayReconstruction ? "True" : "False");
	set("rtx.bloom.enable", gfx.bloom ? "True" : "False");
	if(g_dllHooked || debugEnabled(DebugFixedExposure)) {
		// A torch sphere in frame meters autoexposure off the disc and crushes
		// the dungeon to black (cell bars 28 Ago). Pin exposure for hooked PT.
		set("rtx.autoExposure.enabled", "False");
		set("rtx.tonemap.exposureBias", "0.0");
	} else {
		set("rtx.autoExposure.enabled", "True");
		set("rtx.tonemap.exposureBias", "-0.6");
	}
	if(g_dllHooked) {
		set("rtx.leftHandedCoordinateSystem", "True");
		set("rtx.zUp", "False");
		set("rtx.sceneScale", "1.0");
		// Matte stone, no metal, and legacy lights at half gain. Same values as
		// applyPreviewConfigImpl(); changing one without the other gives a look
		// that shifts the first time the player opens the video options.
		set("rtx.legacyMaterial.roughnessConstant", "0.75");
		set("rtx.legacyMaterial.metallicConstant", "0.0");
		set("rtx.legacyMaterial.emissiveIntensity", "0.0");
		set("rtx.lightConversionIntensityFactor", "0.5");
	}
	LogInfo << "Remix scene: path traced profile quality=" << gfx.quality
	        << " dlss=" << gfx.dlss << " rr=" << gfx.rayReconstruction
	        << " denoiser=" << gfx.denoiser
	        << (g_dllHooked ? " autoexposure=0 legacyRough=0.75" : "");
}

void applyRenderModeConfig(bool inGame) {
	
	if(!g_api.iface().SetConfigVariable) {
		return;
	}

	if(debugEnabled(DebugFreeConfig)) {
		// Crossing between a menu and the level would otherwise reset the look
		// mid-session, which is the other half of "the developer menu does not
		// stick". The flat 2D menu profile is a look, not a correctness fix.
		return;
	}

	// SetConfigVariable is not free and may only land at end of frame, so only
	// touch it when the mode actually flips.
	const int wanted = inGame ? 1 : 0;
	if(g_renderModeApplied == wanted) {
		return;
	}
	g_renderModeApplied = wanted;
	
	auto set = [](const char * key, const char * value) {
		g_api.iface().SetConfigVariable(key, value);
	};
	
	if(inGame) {
		applyUserGfxConfigImpl();
	} else {
		/*
		 * Deliberately leaves the tonemapper alone. Emissive radiance is not in
		 * display units, and the tonemapper is what lifts it there: switching to
		 * the global curve, or bypassing tonemapping outright, both left the menu
		 * black. Only the stages that visibly betray a path tracer come off.
		 * Overlay must not inherit in-game DLSS / RR.
		 */
		if(g_dllHooked) {
			set("rtx.enableRaytracing", config.video.remix.pathTracing ? "True" : "False");
		} else {
			set("rtx.enableRaytracing", "True");
		}
		set("rtx.useDenoiser", "False");
		set("rtx.bloom.enable", "False");
		set("rtx.postfx.enable", "False");
		set("rtx.upscalerType", "3");
		set("rtx.enableRayReconstruction", "False");
		set("rtx.dlssfg.enable", "False");
		set("rtx.autoExposure.enabled", "False");
		set("rtx.tonemap.exposureBias", "0.0");
		set("rtx.pathMaxBounces", "0");
		LogInfo << "Remix scene: flat 2D profile for menus, logos and loading";
	}
}

struct Candidate {
	const EERIE_LIGHT * light = nullptr;
	float dist = 0.f;
	Vec3f pos = Vec3f(0.f);
	bool heldTorch = false;
};

/*!
 * Pick the map lights that should light this frame, nearest first.
 *
 * Shared by the C API light path and by the D3D9 one: the selection rules are
 * about Arx (the held torch sits on the camera, SEMIDYNAMIC statics already
 * have a flickering dynamic copy, cdepth bounds visibility), not about how the
 * lights are then handed to the runtime.
 */
void collectLightCandidates(const Vec3f & center, std::vector<Candidate> & nearby) {
	
	nearby.clear();
	
	const Camera & view = (g_camera != nullptr) ? *g_camera : g_playerCamera;
	const float camDepth = std::max(view.cdepth, 1.f);
	
	auto consider = [&](const EERIE_LIGHT & light, bool heldTorch) {
		if(!light.m_exists || light.intensity <= 0.f || (light.extras & EXTRAS_OFF)) {
			return;
		}
		if(light.rgb.r + light.rgb.g + light.rgb.b <= 0.02f) {
			return;
		}
		Vec3f pos = light.pos;
		if(heldTorch) {
			// torchLightHandle sits on the camera (ManageTorch). A sphere there is
			// the white globe the user rejected. Place it at roughly hand height,
			// not 50u into the next room (that punched a disc through the bars).
			Vec3f fwd = angleToVector(view.angle);
			const float fwdLen = glm::length(fwd);
			if(fwdLen > 1e-6f) {
				fwd /= fwdLen;
			}
			pos = center + fwd * 18.f;
			pos.y = center.y + 12.f;
		}
		const float dist = glm::distance(pos, center);
		// Same visibility as PrecalcDynamicLighting: lights that can reach the frustum.
		if(dist >= camDepth + std::max(light.fallend, 0.f)) {
			return;
		}
		if(!heldTorch && dist < 8.f) {
			return;
		}
		nearby.push_back({ &light, dist, pos, heldTorch });
	};
	if(EERIE_LIGHT * torch = lightHandleGet(torchLightHandle)) {
		consider(*torch, true);
	}
	for(LightHandle handle : g_dynamicLights.handles(1)) {
		consider(g_dynamicLights[handle], false);
	}
	for(const EERIE_LIGHT & light : g_staticLights) {
		if(!light.m_ignitionStatus) {
			continue;
		}
		// SEMIDYNAMIC torches already have a flickering dyn copy — do not double them.
		if((light.extras & EXTRAS_SEMIDYNAMIC) && lightHandleGet(light.m_ignitionLightHandle)) {
			continue;
		}
		consider(light, false);
	}
	
}

bool updateLights(const Vec3f & center) {
	
	std::vector<Candidate> nearby;
	collectLightCandidates(center, nearby);
	
	std::vector<Candidate> unique = nearby;
	std::sort(unique.begin(), unique.end(), [](const Candidate & a, const Candidate & b) {
		return a.dist < b.dist;
	});
	
	if(unique.empty()) {
		destroyLevelLights();
		if(g_frames == 0 || (g_frames % 120) == 0) {
			LogInfo << "Remix scene: no map lights in range of the player";
		}
		return false;
	}
	
	const size_t count = std::min(unique.size(), kMaxLevelLights);
	bool any = false;
	for(size_t i = 0; i < count; ++i) {
		const EERIE_LIGHT & light = *unique[i].light;
		const Vec3f pos = g_dllHooked ? unique[i].pos : toRemix(unique[i].pos);
		const float fall = std::max(light.fallstart, 40.f);
		const float worldRadius = glm::clamp(fall * 0.1f, 4.f, 50.f);
		const float radius = g_dllHooked ? worldRadius : worldRadius * kWorldScale;
		// C API lights live in toRemix() space (×0.01). Hooked D3D9 verts are Arx
		// units, so inverse-square is 10000× weaker unless radiance is scaled too.
		// Log 28 Ago: rad≈0.8 at dist 410 left the cell torch visible (the sphere)
		// and every wall black — PT was on, energy was not.
		const float unitScale = g_dllHooked ? (1.f / kWorldScale) : 1.f;
		const float boost = 3.2f * std::max(light.intensity, 0.4f) * unitScale * unitScale;
		const remixapi_Float3D radiance {
			light.rgb.r * boost, light.rgb.g * boost, light.rgb.b * boost
		};
		
		CachedLight & cached = g_levelLights[&light];
		const bool moved = !cached.handle || glm::distance(cached.pos, pos) > 1e-4f;
		const bool recoloured = !cached.handle
		                        || std::abs(cached.radiance.x - radiance.x) > 0.01f
		                        || std::abs(cached.radiance.y - radiance.y) > 0.01f
		                        || std::abs(cached.radiance.z - radiance.z) > 0.01f;
		const bool resized = !cached.handle || std::abs(cached.radius - radius) > 0.05f;
		if(moved || recoloured || resized) {
			// Remix has no update-light call, so a change still means destroy and
			// create - but the hash stays tied to this EERIE_LIGHT, so the runtime
			// still recognises it as the same light across frames.
			const uint64_t hash = 0xA117E0ull ^ uint64_t(reinterpret_cast<uintptr_t>(&light));
			if(!createSphereLight(cached.handle, hash, pos, radius, radiance)) {
				g_levelLights.erase(&light);
				continue;
			}
			cached.pos = pos;
			cached.radiance = radiance;
			cached.radius = radius;
		}
		cached.frame = g_frames;
		any = true;
	}
	
	if(g_frames == 0 || (g_frames % 120) == 0) {
		LogInfo << "Remix scene: submitting " << count << " of " << unique.size()
		        << " scene lights (C API path)";
		for(size_t i = 0; i < std::min(count, size_t(4)); ++i) {
			const auto found = g_levelLights.find(unique[i].light);
			if(found == g_levelLights.end()) {
				continue;
			}
			const CachedLight & cached = found->second;
			LogInfo << "Remix scene: path-traced light " << i
			        << " at " << unique[i].pos.x << ',' << unique[i].pos.y << ',' << unique[i].pos.z
			        << " dist=" << unique[i].dist
			        << " fallend=" << unique[i].light->fallend
			        << " rad=(" << cached.radiance.x << ',' << cached.radiance.y << ','
			        << cached.radiance.z << ')';
		}
	}
	
	// Drop lights that fell out of range this frame.
	for(auto it = g_levelLights.begin(); it != g_levelLights.end(); ) {
		if(it->second.frame != g_frames) {
			destroyLightHandle(it->second.handle);
			it = g_levelLights.erase(it);
		} else {
			++it;
		}
	}
	
	return any;
}

/*
 * --remix-debug helpers.
 *
 * The preview window has been black while the log reported a healthy frame, so
 * these exist to make a claim falsifiable on screen rather than in the log.
 */

//! Row-major store with an optional transpose, since the API does not document
//! whether remixapi_CameraInfo::view is row- or column-major.
void storeMatrix(float out[4][4], const float in[4][4]) {
	const bool transpose = debugEnabled(DebugTransposeMatrices);
	for(int r = 0; r < 4; ++r) {
		for(int c = 0; c < 4; ++c) {
			out[r][c] = transpose ? in[c][r] : in[r][c];
		}
	}
}

//! Left-handed look-at, D3D layout: basis in the columns, translation last row.
void fillViewMatrix(float out[4][4], const Vec3f & pos, const Vec3f & fwd,
                    const Vec3f & up, const Vec3f & right) {
	const float m[4][4] = {
		{ right.x, up.x, fwd.x, 0.f },
		{ right.y, up.y, fwd.y, 0.f },
		{ right.z, up.z, fwd.z, 0.f },
		{ -glm::dot(right, pos), -glm::dot(up, pos), -glm::dot(fwd, pos), 1.f }
	};
	storeMatrix(out, m);
}

//! Left-handed perspective with a [0, 1] depth range, matching the probe's FOV setup.
void fillProjectionMatrix(float out[4][4], float fovY, float aspect, float nearZ, float farZ) {
	const float h = 1.f / std::tan(fovY * 0.5f);
	const float w = aspect > 1e-6f ? h / aspect : h;
	const float q = farZ / (farZ - nearZ);
	const float m[4][4] = {
		{ w, 0.f, 0.f, 0.f },
		{ 0.f, h, 0.f, 0.f },
		{ 0.f, 0.f, q, 1.f },
		{ 0.f, 0.f, -nearZ * q, 0.f }
	};
	storeMatrix(out, m);
}

remixapi_HardcodedVertex debugVertex(const Vec3f & p, const Vec3f & n) {
	// Already in Remix space: do not run this through toRemix().
	remixapi_HardcodedVertex v {};
	v.position[0] = p.x;
	v.position[1] = p.y;
	v.position[2] = p.z;
	v.normal[0] = n.x;
	v.normal[1] = n.y;
	v.normal[2] = n.z;
	v.color = 0xFFFFFFFFu;
	return v;
}

/*!
 * RemixProbe's triangle, in camera object space so the mesh is created once
 * and moved with the instance transform.
 */
void drawDebugProbe(const Vec3f & pos, const Vec3f & fwd, const Vec3f & up, const Vec3f & right) {
	
	if(!g_api.iface().CreateMesh) {
		return;
	}
	
	const remixapi_HardcodedVertex verts[] = {
		debugVertex(Vec3f(5.f, -5.f, 10.f), Vec3f(0.f, 0.f, -1.f)),
		debugVertex(Vec3f(0.f, 5.f, 10.f), Vec3f(0.f, 0.f, -1.f)),
		debugVertex(Vec3f(-5.f, -5.f, 10.f), Vec3f(0.f, 0.f, -1.f))
	};
	
	if(!g_debugMesh) {
		remixapi_MeshInfoSurfaceTriangles triangles {};
		triangles.vertices_values = verts;
		triangles.vertices_count = std::size(verts);
		triangles.skinning_hasvalue = FALSE;
		triangles.material = nullptr;
		
		remixapi_MeshInfo meshInfo {};
		meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		meshInfo.hash = 0xDEB0Aull;
		meshInfo.surfaces_values = &triangles;
		meshInfo.surfaces_count = 1;
		
		if(g_api.iface().CreateMesh(&meshInfo, &g_debugMesh) != REMIXAPI_ERROR_CODE_SUCCESS) {
			g_debugMesh = nullptr;
			return;
		}
	}
	
	remixapi_InstanceInfo instance {};
	instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.mesh = g_debugMesh;
	instance.doubleSided = TRUE;
	instance.transform = { {
		{ right.x, up.x, fwd.x, pos.x },
		{ right.y, up.y, fwd.y, pos.y },
		{ right.z, up.z, fwd.z, pos.z },
	} };
	g_api.iface().DrawInstance(&instance);
	
	if(createSphereLight(g_debugLight, 0xDEB0Bull, pos - up, 0.1f, { 100.f, 200.f, 100.f })) {
		g_api.iface().DrawLightInstance(g_debugLight);
	}
}

bool rebuildMesh(const Vec3f & center) {
	
	if(!g_tiles) {
		return false;
	}
	
	const auto origin = g_tiles->getTile(center);
	if(!origin.valid()) {
		return false;
	}
	
	// Rebasing has to happen before a single vertex is converted: toRemix()
	// subtracts g_worldOrigin, so every cached mesh built under the previous
	// origin - the room and every entity - is stale the moment it moves.
	const Vec3f wantOrigin = debugEnabled(DebugRebaseOrigin) ? snapToOriginGrid(center) : Vec3f(0.f);
	if(wantOrigin != g_worldOrigin) {
		LogInfo << "Remix scene: world origin " << wantOrigin.x << ',' << wantOrigin.y << ',' << wantOrigin.z
		        << " (was " << g_worldOrigin.x << ',' << g_worldOrigin.y << ',' << g_worldOrigin.z << ')';
		g_worldOrigin = wantOrigin;
		destroyMesh();
		destroyEntityMeshes(g_api);
	}
	
	if(origin.index() == g_meshTile && g_mesh) {
		return true;
	}
	
	std::map<TextureContainer *, std::vector<remixapi_HardcodedVertex>> groups;
	
	const short x0 = short(std::max(int(origin.x) - kTileRadius, 0));
	const short z0 = short(std::max(int(origin.y) - kTileRadius, 0));
	const short x1 = short(std::min(int(origin.x) + kTileRadius, int(TileData::m_size.x) - 1));
	const short z1 = short(std::min(int(origin.y) + kTileRadius, int(TileData::m_size.y) - 1));
	
	size_t totalTris = 0;
	bool cap = false;
	for(short x = x0; x <= x1 && !cap; ++x) {
		for(short z = z0; z <= z1 && !cap; ++z) {
			const auto tile = g_tiles->get(Vec2s(x, z));
			if(!tile.valid()) {
				continue;
			}
			for(const EERIEPOLY & poly : tile.polygons()) {
				if(skipPoly(poly)) {
					continue;
				}
				if(totalTris >= kMaxTriangles) {
					cap = true;
					break;
				}
				auto & verts = groups[poly.tex];
				appendTri(verts, poly, 0, 1, 2);
				++totalTris;
				if(poly.type & POLY_QUAD) {
					appendTri(verts, poly, 1, 3, 2);
					++totalTris;
				}
			}
		}
	}
	
	if(totalTris == 0) {
		LogWarning << "Remix scene: no static triangles near the camera";
		return false;
	}
	
	destroyMesh();
	
	std::vector<std::vector<uint32_t>> indexLists;
	indexLists.reserve(groups.size());
	std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
	surfaces.reserve(groups.size());
	
	Vec3f aabbMin(0.f);
	Vec3f aabbMax(0.f);
	bool aabbInit = false;
	size_t texturedSurfaces = 0;
	
	for(auto & group : groups) {
		auto & verts = group.second;
		if(verts.size() < 3) {
			continue;
		}
		
		indexLists.emplace_back(verts.size());
		auto & indices = indexLists.back();
		for(uint32_t i = 0; i < uint32_t(indices.size()); ++i) {
			indices[i] = i;
		}
		
		remixapi_MeshInfoSurfaceTriangles surface {};
		surface.vertices_values = verts.data();
		surface.vertices_count = verts.size();
		surface.indices_values = indices.data();
		surface.indices_count = indices.size();
		surface.skinning_hasvalue = FALSE;
		surface.material = debugEnabled(DebugNoMaterials) ? nullptr : materialFor(group.first, g_api);
		if(surface.material) {
			++texturedSurfaces;
		}
		surfaces.push_back(surface);
		
		for(const remixapi_HardcodedVertex & v : verts) {
			const Vec3f p(v.position[0], v.position[1], v.position[2]);
			if(!aabbInit) {
				aabbMin = aabbMax = p;
				g_meshFirstVertex = p;
				aabbInit = true;
			} else {
				aabbMin = glm::min(aabbMin, p);
				aabbMax = glm::max(aabbMax, p);
			}
		}
	}
	
	if(surfaces.empty()) {
		return false;
	}
	
	remixapi_MeshInfo meshInfo {};
	meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash = 0xA1000000ull ^ (u64(origin.x) << 16) ^ u64(origin.y);
	meshInfo.surfaces_values = surfaces.data();
	meshInfo.surfaces_count = uint32_t(surfaces.size());
	
	const remixapi_ErrorCode status = g_api.iface().CreateMesh(&meshInfo, &g_mesh);
	if(status != REMIXAPI_ERROR_CODE_SUCCESS) {
		LogError << "Remix scene: CreateMesh failed (" << RemixApi::errorString(status) << ')';
		g_mesh = nullptr;
		return false;
	}
	
	g_meshTile = origin.index();
	LogInfo << "Remix scene: static mesh " << totalTris << " triangles, " << surfaces.size()
	        << " surfaces (" << texturedSurfaces << " textured) around tile "
	        << origin.x << ',' << origin.y
	        << " aabb=(" << aabbMin.x << ',' << aabbMin.y << ',' << aabbMin.z
	        << ")-(" << aabbMax.x << ',' << aabbMax.y << ',' << aabbMax.z << ')';
	return true;
}

} // namespace

bool startBridge(SDL_Window * window) {
	
	if(g_started) {
		return true;
	}
	
	SDL_Window * target = window ? window : g_window;
	const std::wstring dll = RemixApi::findRuntimeDll();
	if(dll.empty()) {
		LogError << "Remix scene: x64 .trex/d3d9.dll not found. Set ARX_REMIX_DLL or --remix-dll.";
		return false;
	}
	
	if(!target) {
		LogError << "Remix scene: no native window; startBridge requires the game HWND.";
		return false;
	}
	
	const HWND hwnd = nativeHwnd(target);
	if(!hwnd) {
		LogError << "Remix scene: could not obtain HWND";
		return false;
	}
	
	int shownX = 0, shownY = 0;
	SDL_GetWindowPosition(target, &shownX, &shownY);
	SDL_GetWindowSize(target, &g_previewWidth, &g_previewHeight);
	LogInfo << "Remix scene: native window at " << shownX << ',' << shownY
	        << " (" << g_previewWidth << 'x' << g_previewHeight << ')';
	LogInfo << "Remix scene: loading " << platform::WideString::toUTF8(dll.c_str());
	if(g_api.load(dll.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS) {
		return false;
	}
	if(g_api.startup(hwnd, debugEnabled(DebugEditorMode)) != REMIXAPI_ERROR_CODE_SUCCESS) {
		return false;
	}
	if(debugEnabled(DebugEditorMode)) {
		LogInfo << "Remix scene: editor mode on - press Alt+X in the preview window for Debug View";
	}
	
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
	SDL_RaiseWindow(target);
#if SDL_VERSION_ATLEAST(2, 0, 5)
	SDL_SetWindowInputFocus(target);
#endif

	applyPreviewConfigImpl();
	g_window = target;
	g_started = true;
	
	LogInfo << "Remix scene: Startup succeeded (header API " << RemixApi::headerVersionString()
	        << ", world scale " << kWorldScale << ", tile radius " << kTileRadius << ')';
	return true;
}

/*!
 * Camera, room mesh and lights. Split out of presentFrame() so the renderer can
 * submit the UI meshes after SetupCamera but before Present - drawing them
 * first made every UI instance use the previous frame's camera.
 */
void setupScene() {
	
	if(!g_started || !g_api.iface().SetupCamera) {
		return;
	}
	
	int w = g_previewWidth;
	int h = g_previewHeight;
	if(g_window) {
		SDL_GetWindowSize(g_window, &w, &h);
	}
	w = std::max(w, 1);
	h = std::max(h, 1);
	
	const bool inGame = sceneIs3D();
	applyRenderModeConfig(inGame);
	
	if(inGame) {
		const Camera & view = (g_camera != nullptr) ? *g_camera : g_playerCamera;
		const Vec3f meshCenter = view.m_pos;
		const bool haveMesh = rebuildMesh(meshCenter) && g_mesh;
		if(!haveMesh && (g_frames == 0 || (g_frames % 120) == 0)) {
			LogWarning << "Remix scene: no mesh at camera pos ("
			           << meshCenter.x << ',' << meshCenter.y << ',' << meshCenter.z << ')';
		}
		
		Vec3f position, forward, up, right;
		remixCameraBasis(view, position, forward, up, right);
		const bool haveLight = updateLights(meshCenter);
		ARX_UNUSED(haveLight);
		
		const float fovY = view.fov();
		const float aspect = float(w) / float(h);
		const float nearPlane = 0.1f;
		const float farPlane = std::max(view.cdepth * kWorldScale, 500.f);
		
		remixapi_CameraInfo camera {};
		camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
		camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
		
		remixapi_CameraInfoParameterizedEXT params {};
		params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
		params.position = { position.x, position.y, position.z };
		params.forward = { forward.x, forward.y, forward.z };
		params.up = { up.x, up.y, up.z };
		params.right = { right.x, right.y, right.z };
		params.fovYInDegrees = glm::degrees(fovY);
		params.aspect = aspect;
		params.nearPlane = nearPlane;
		params.farPlane = farPlane;
		
		if(debugEnabled(DebugExplicitMatrices)) {
			fillViewMatrix(camera.view, position, forward, up, right);
			fillProjectionMatrix(camera.projection, fovY, aspect, nearPlane, farPlane);
		} else {
			camera.pNext = &params;
		}
		g_api.iface().SetupCamera(&camera);
		
		if(haveMesh) {
			remixapi_InstanceInfoBlendEXT blend {};
			remixapi_InstanceInfo instance {};
			fillOpaqueInstance(instance, blend, g_mesh);
			g_api.iface().DrawInstance(&instance);
		}
		drawEntities(g_api, meshCenter, g_frames);
		const size_t lightsDrawn = drawLevelLights();
		ARX_UNUSED(lightsDrawn);
		
		if(debugEnabled(DebugProbeTriangle)) {
			drawDebugProbe(position, forward, up, right);
		}
	} else {
		// Menus and 2D mode camera
		remixapi_CameraInfo camera {};
		camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
		camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
		
		remixapi_CameraInfoParameterizedEXT params {};
		params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
		params.position = { 0.f, 0.f, 0.f };
		params.forward = { 0.f, 0.f, 1.f };
		params.up = { 0.f, 1.f, 0.f };
		params.right = { 1.f, 0.f, 0.f };
		params.fovYInDegrees = 60.f;
		params.aspect = float(w) / float(h);
		params.nearPlane = 0.1f;
		params.farPlane = 100.f;
		
		camera.pNext = &params;
		g_api.iface().SetupCamera(&camera);
		
		if(g_frames == 0 || (g_frames % 300) == 0) {
			LogInfo << "Remix scene: overlay camera " << w << 'x' << h
			        << " aspect=" << params.aspect << " fovY=" << params.fovYInDegrees
			        << (overlayFrameActive() ? " (overlay)" : " (2D)");
		}
	}
}

void presentFrame() {
	
	if(!g_started || !g_api.iface().Present) {
		return;
	}
	
	remixapi_PresentInfo present {};
	present.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
	const remixapi_ErrorCode presentStatus = g_api.iface().Present(&present);
	if(presentStatus != REMIXAPI_ERROR_CODE_SUCCESS && (g_frames == 0 || (g_frames % 120) == 0)) {
		LogError << "Remix scene: Present failed (" << RemixApi::errorString(presentStatus) << ')';
	}
	
	g_levelRendered = false;
	g_overlayFrame = false;
	++g_frames;
}

bool sceneRequested() {
	return g_sceneRequested;
}

bool isRemixWindowId(unsigned windowId) {
	return g_window != nullptr && windowId != 0 && SDL_GetWindowID(g_window) == windowId;
}

bool isRemixPreviewActive() {
	return g_window != nullptr && g_started;
}

bool isRemixDllHooked() {
	return g_dllHooked;
}

void setRemixDllHooked(bool hooked) {
	g_dllHooked = hooked;
	if(!hooked) {
		g_renderModeApplied = -1;
	}
}

void applyPreviewConfig() {
	applyPreviewConfigImpl();
}

size_t collectSceneLights(SceneLight * out, size_t maxLights) {
	
	if(!out || maxLights == 0) {
		return 0;
	}
	
	const Camera & view = (g_camera != nullptr) ? *g_camera : g_playerCamera;
	std::vector<Candidate> nearby;
	collectLightCandidates(view.m_pos, nearby);
	std::sort(nearby.begin(), nearby.end(), [](const Candidate & a, const Candidate & b) {
		return a.dist < b.dist;
	});
	
	const size_t count = std::min(nearby.size(), maxLights);
	for(size_t i = 0; i < count; i++) {
		const EERIE_LIGHT & light = *nearby[i].light;
		out[i].pos = nearby[i].pos;
		out[i].rgb = light.rgb;
		out[i].intensity = light.intensity;
		out[i].fallstart = light.fallstart;
		out[i].fallend = light.fallend;
	}
	
	if(g_frames == 0 || (g_frames % 120) == 0) {
		LogInfo << "Remix scene: " << count << " of " << nearby.size()
		        << " map lights handed to D3D9";
	}
	
	return count;
}

bool g_cApiDeviceRegistered = false;

void setCApiDeviceRegistered(bool registered) {
	g_cApiDeviceRegistered = registered;
}

bool hasCApiDevice() {
	return g_cApiDeviceRegistered;
}

void onDllHookReady() {
	g_dllHooked = true;
	g_renderModeApplied = -1;
	if(g_sceneRequested) {
		LogWarning << "--remix-scene is ignored: C API play is off. D3D9Renderer already loaded Remix via --remix-dll.";
	}
	applyPreviewConfigImpl();
	LogInfo << "Remix dll hook: SetConfigVariable ready (no Startup / DrawInstance)";
}

void tickDllHookFrame() {
	if(!g_dllHooked) {
		return;
	}
	const bool inGame = sceneIs3D();
	applyRenderModeConfig(inGame);
	/*
	 * No CreateLight / DrawLightInstance here any more. Those belong to the C
	 * API's own frame, delimited by remixapi_Present, and we present through
	 * D3D9 - so they never reached the scene. The runtime's Light Statistics
	 * panel said "Total Lights: 0" with 17 submitted every frame (28 Aug).
	 * D3D9Renderer now feeds the same lights in as D3DLIGHT9 instead.
	 */
	if(inGame) {
		destroyLevelLights();
	}
	++g_frames;
	g_levelRendered = false;
	g_overlayFrame = false;
}

SDL_Window * previewSdlWindow() {
	return g_window;
}

RemixApi & remixApi() {
	return g_api;
}

void getCurrentCameraBasis(Vec3f & position, Vec3f & forward, Vec3f & up, Vec3f & right, float & fovY) {
	if(sceneIs3D()) {
		const Camera & view = (g_camera != nullptr) ? *g_camera : g_playerCamera;
		remixCameraBasis(view, position, forward, up, right);
		fovY = view.fov();
	} else {
		position = Vec3f(0.f, 0.f, 0.f);
		forward = Vec3f(0.f, 0.f, 1.f);
		up = Vec3f(0.f, 1.f, 0.f);
		right = Vec3f(1.f, 0.f, 0.f);
		fovY = glm::radians(60.f);
	}
}

void tick() {
	
	if(!g_started) {
		return;
	}
	
	// Present happens in RemixRenderer::showFrame() after UI batches are submitted.
}

void ensureStarted() {
	tick();
}

void notifyLevelRendered() {
	g_levelRendered = true;
}

void notifyOverlayFrame() {
	g_overlayFrame = true;
}

bool isOverlayFrame() {
	return overlayFrameActive();
}

void onDllHookBeginScene() {
	
	if(!g_cApiDeviceRegistered) {
		// Remix reads the camera from D3DTS_VIEW/D3DTS_PROJECTION on the capture
		// path; the C API SetupCamera would only fail, once per frame.
		return;
	}

	if(!isRemixDllHooked()) {
		return;
	}
	setupDllHookCamera();
}

void getArxHookCameraBasis(Vec3f & position, Vec3f & forward, Vec3f & up, Vec3f & right) {
	const Camera & view = (g_camera != nullptr) ? *g_camera : g_playerCamera;
	arxHookCameraBasis(view, position, forward, up, right);
}

void applyUserGfxConfig() {
	g_renderModeApplied = -1;
}

void releaseLevelResources() {
	if(!g_started) {
		return;
	}
	destroyMesh();
	destroyEntityMeshes(g_api);
	destroyLevelLights();
	destroyDebugScene();
	destroyMaterials(g_api);
}

[[noreturn]] void exitWithoutDestroyingHwnd(int code) {
	Logger::quickShutdown();
	_exit(code);
}

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX
