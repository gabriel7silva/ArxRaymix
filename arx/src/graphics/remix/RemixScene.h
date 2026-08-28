/*
 * Arx Remaster — Native RTX Remix scene & pipeline exporter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_GRAPHICS_REMIX_REMIXSCENE_H
#define ARX_GRAPHICS_REMIX_REMIXSCENE_H

#include "Configure.h"

#if ARX_HAVE_RTX_REMIX

#include "graphics/Color.h"
#include "math/Vector.h"

struct SDL_Window;
class RemixApi;

namespace remix {

[[nodiscard]] bool sceneRequested();
bool startBridge(SDL_Window * window = nullptr);
void ensureStarted();

/*!
 * Tell the Remix scene that this frame drew the 3D level.
 *
 * Loading screens, splash screens and cinematics call RenderWindow::showFrame()
 * straight from their own loops, without going through ArxGame::renderLevel(),
 * so the menu mode alone cannot tell a 2D frame from a gameplay frame.
 */
void notifyLevelRendered();

/*!
 * This frame is logos, loading, main menu, credits or character creation.
 *
 * Logos keep ARXmenu in Mode_InGame (the default), so menu mode cannot mark
 * them. Call before the 2D bitmaps are queued: queue2DVertices reads the
 * camera in the same frame.
 */
void notifyOverlayFrame();

//! Logos, loading, menu, credits or character creation — no world mesh or lights.
[[nodiscard]] bool isOverlayFrame();

void setupScene();
void presentFrame();
void tick();

//! Apply Options → Remix (path tracing, quality, DLSS, RR, denoiser, bloom).
void applyUserGfxConfig();
[[nodiscard]] bool isRemixWindowId(unsigned windowId);
[[nodiscard]] bool isRemixPreviewActive();
//! D3D9 `CreateDevice` used Remix's `d3d9.dll`. Not the C API presenter.
[[nodiscard]] bool isRemixDllHooked();
void setRemixDllHooked(bool hooked);
//! Pin rtx.conf and apply the user gfx profile. No `Startup` / `DrawInstance`.
void onDllHookReady();

/*!
 * Push our RtxOptions into the Remix runtime.
 *
 * Must run *after* dxvk_RegisterD3D9Device(): until the runtime knows which
 * D3D9 device is ours, SetConfigVariable has nothing to apply to and the
 * settings are silently dropped. That is why forcing rtx.showUI had no effect
 * while the same key in a config file worked (28 Aug).
 */
void applyPreviewConfig();
/*!
 * A map light chosen for this frame, in Arx world units.
 *
 * The C API light path (CreateLight + DrawLightInstance) does not reach the
 * scene while Remix is driven as a d3d9.dll hook: those calls belong to the C
 * API's own frame, which is delimited by remixapi_Present, and we present
 * through D3D9 instead. The runtime's own Light Statistics panel read
 * "Total Lights: 0" with 17 lights submitted every frame (28 Aug).
 *
 * So the renderer turns these into D3DLIGHT9 and lets Remix pick them up the
 * way it does for any other D3D9 game.
 */
struct SceneLight {
	Vec3f pos = Vec3f(0.f);
	Color3f rgb = Color3f::black;
	float intensity = 1.f;
	float fallstart = 0.f;
	float fallend = 0.f;
};

//! Fill `out` with up to `maxLights` map lights near the camera, nearest first.
size_t collectSceneLights(SceneLight * out, size_t maxLights);

//! Overlay vs in-game `SetConfigVariable`. Clears per-frame overlay/level flags.
void tickDllHookFrame();
/*!
 * Whether the C API has a registered D3D9 device.
 *
 * False on the capture path: Remix's own Direct3DCreate9 hands back a non-Ex
 * object, so dxvk_RegisterD3D9Device cannot bind it. Nothing needs it any more
 * - lights go in as D3DLIGHT9 and the camera comes from the D3D9 transforms -
 * but SetupCamera and the injection dummy have to stop firing, or they spam
 * errors every frame and leave a stray untextured triangle in the traced scene.
 */
void setCApiDeviceRegistered(bool registered);
[[nodiscard]] bool hasCApiDevice();

//! `SetupCamera` at BeginScene, before any XYZRHW (loading HUD included).
void onDllHookBeginScene();
//! Arx-space basis matching hooked `D3DFVF_XYZ` verts (not `toRemix`).
void getArxHookCameraBasis(Vec3f & position, Vec3f & forward, Vec3f & up, Vec3f & right);
[[nodiscard]] SDL_Window * previewSdlWindow();

//! Free Remix meshes/materials/lights that belong to the current level.
void releaseLevelResources();

/*!
 * Remix hooks the HWND; Shutdown + DestroyWindow crashes in d3d9.dll.
 * Flush logs and terminate without tearing the window down.
 */
[[noreturn]] void exitWithoutDestroyingHwnd(int code);

[[nodiscard]] RemixApi & remixApi();
void getCurrentCameraBasis(Vec3f & position, Vec3f & forward, Vec3f & up, Vec3f & right, float & fovY);

} // namespace remix

#endif // ARX_HAVE_RTX_REMIX

#endif // ARX_GRAPHICS_REMIX_REMIXSCENE_H
