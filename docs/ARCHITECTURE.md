# Architecture

How the Remix path is wired, written from the code. Where a comment in the source and this document
disagree, read the source — every claim here points at the file it came from.

## Two integrations, one of them live

There are two ways to talk to Remix, and both exist in this tree.

**The capture path — what the game runs.** `D3D9Renderer` creates its device by calling
`Direct3DCreate9` resolved from the Remix `d3d9.dll` (`D3D9Renderer.cpp`, `createDevice`). Every
draw call the engine issues then goes through the runtime, which is what puts it in front of the
path tracer. Nothing about the game's rendering is special-cased for Remix beyond the geometry
format described below.

**The C API path — the probe.** `remixapi_*` calls that build a scene explicitly:
`CreateMesh`, `DrawInstance`, `CreateLight`, `remixapi_Present`. `--remix-probe` uses it to render a
triangle with no game state, which separates a broken integration from a broken runtime or driver.
`RemixEntities.cpp`, `RemixRenderer.cpp` and the DDS exporter in `RemixTextures.cpp` belong to this
path. **The game does not run it** — `remix::tick()` does nothing without `Startup`.

That distinction matters when reading the code: material classification, DDS export and per-texture
roughness live in `RemixTextures.cpp` and have no effect on the game today. On the capture path,
materials come from the D3D9 textures plus the `rtx.legacyMaterial.*` constants in
[`CONFIGURATION.md`](CONFIGURATION.md).

### Which renderer is compiled in

```cpp
#if ARX_HAVE_D3D9
    m_renderer = new D3D9Renderer;
#elif ARX_HAVE_RTX_REMIX
    m_renderer = new remix::RemixRenderer;
#else
    m_renderer = new OpenGLRenderer;
#endif
```
`SDL2Window.cpp`

`WITH_RTX_REMIX=ON` defines **both** `ARX_HAVE_RTX_REMIX` and `ARX_HAVE_D3D9` (`arx/CMakeLists.txt`),
so the D3D9 renderer wins and OpenGL is not in the binary at all. It is a build-time choice, not a
runtime one.

### Device creation order

`createDevice` tries, in order:

1. `Direct3DCreate9` from the Remix module — the capture path, and the one that works
2. `dxvk_CreateD3D9`, which returns an `IDirect3D9Ex`
3. `Direct3DCreate9` from the system `d3d9.dll`

The order matters more than it looks. `dxvk_CreateD3D9` hands back a perfectly working device —
geometry renders, no error is logged, and none of its draw calls reach the path tracer. The comment
at that branch records it, because nothing on screen or in any log distinguishes the two.

Once a device exists and it is an `IDirect3D9Ex`, `dxvk_RegisterD3D9Device` binds it, and only
**after** that does `remix::applyPreviewConfig()` run. Options set before registration are dropped.

## The world-space contract

Remix rasterises `D3DFVF_XYZRHW` and only traces untransformed geometry, so anything that should be
lit has to arrive in world space.

A backend declares what it wants:

```cpp
[[nodiscard]] virtual bool wantsWorldSpaceEntities() const { return false; }
```
`Renderer.h` — `D3D9Renderer` returns `remix::isRemixDllHooked()`.

The engine then converts at the source instead of trying to undo the projection later
(`AnimationRender.cpp`):

```cpp
if(worldSpace) {
    tvList[n].p = eobj->vertexWorldPositions[face.vid[n]].v;   // world
    tvList[n].w = 0.f;                                          // the marker
} else {
    tvList[n].p = Vec3f(eobj->vertexClipPositions[face.vid[n]]);
    tvList[n].w = eobj->vertexClipPositions[face.vid[n]].w;
}
```

`w == 0` means "this position is already world space", recognised by `isWorldSpaceW()` in the
renderer. It travels **with the vertex** deliberately: between filling a batch and drawing it there
is a deferred flush, and a global flag would not survive that trip.

An earlier version reconstructed world positions by unprojecting screen-space vertices. It was
deleted, and the comment where it stood explains why — two independent reasons, one of which is not
fixable: the reconstruction is unstable wherever `w` is small (visible as trembling geometry), and a
camera-facing billboard built in screen space has no world orientation inside it to recover.

## Vertex formats

```cpp
struct WorldVertex {
    float x, y, z;
    // D3DFVF component order is fixed: position, normal, diffuse, texcoords.
    float nx, ny, nz;
    D3DCOLOR color;
    float u, v;
};
static_assert(sizeof(WorldVertex) == 36, "XYZ+NORMAL+DIFFUSE+TEX1 is 36 bytes");
```

Four formats are in play, `kTLFVF` and `kTL3FVF` for screen-space overlays, `kWorldFVF` and
`kWorld3FVF` for traced geometry. The static asserts are load-bearing: the FVF declares the layout
and D3D reads the struct by offset, so a field in the wrong order is silent corruption.

Normals for level geometry are area-weighted smooth normals accumulated per vertex from face cross
products (`computeSmoothNormals`). They are computed per batch, so a surface split across two draw
calls can show a seam at the boundary.

Entity normals come from skinning instead: each vertex takes its rest-pose normal rotated by the
animated quaternion of the bone that owns it.

```cpp
posedNormals[index] = quat * eobj->vertexlist[vertex].norm;
```
`AnimationRender.cpp` — Arx uses rigid skinning, one bone per vertex, so a single rotation is exact.

## Lights

`applyRemixLights()` converts the level's lights each frame:

- `D3DLIGHT_POINT`, colour multiplied by the light's intensity
- `Range` from `fallend`
- quadratic attenuation over the `fallend - fallstart` fade, which is the closest fixed-function
  equivalent to the engine's own falloff
- **as many as the device allows**, read from `D3DCAPS9::MaxActiveLights` and clamped to
  `kMaxSceneLights` (32). Fixed-function D3D9 traditionally answers 8, and a level holds several
  hundred lights, so most of them never reach the tracer at all — the single biggest reason the
  result reads as inconsistent next to the baked original. Unused slots are disabled every frame
  so a stale light cannot linger

`D3DRS_LIGHTING` follows `remixWantsWorldSpace()`: on when the Remix DLL is hooked, off otherwise.

The camera needs no special handling. `SetViewMatrix` and `SetProjectionMatrix` copy the engine's
own matrices straight into `D3DTS_VIEW` and `D3DTS_PROJECTION`, and the runtime derives the camera
from them.

## Coordinate conventions

Arx is Y-down and left-handed, roughly one unit per centimetre, so the runtime is told
`rtx.leftHandedCoordinateSystem = True`, `rtx.zUp = False`, `rtx.sceneScale = 1.0`.

`rtx.camera.correctProjectionYFlip` is deliberately **False**. The engine already flips Y on the way
from NDC to screen (`Camera.cpp`, `scale(1, -1, 1)` in `ndcToScreen`); correcting it a second time
renders the world upside down. It was set to `True` at one point to silence a "not detecting a valid
camera" warning, but that symptom belonged to `dxvk_CreateD3D9` and disappeared with it.

## Shutdown

`SDL2Window`'s destructor calls `remix::exitWithoutDestroyingHwnd()`, which flushes the log and calls
`_exit()`. Orderly teardown — `Shutdown` followed by destroying the HWND — crashes inside `d3d9.dll`.
This is a known shortcut, not an oversight: the process exits hard rather than unwinding.

## Traps already paid for

Each of these looked like a different problem first.

- **`alphaTestType = 0` is `VK_COMPARE_OP_NEVER`.** A zeroed C struct field is the zero enum member,
  not a sane default; every textured texel was discarded and the window was black. `7` is
  `VK_COMPARE_OP_ALWAYS`. Bit `512` of `--remix-debug` restores the bug on purpose.
- **`dxvk_CreateD3D9` renders without tracing.** See the device creation order above.
- **Config before `dxvk_RegisterD3D9Device` is lost.**
- **`user.conf` beats `rtx.conf`.** The runtime writes it whenever the developer menu is touched and
  reads it back at startup. One stray toggle saving `rtx.enableRaytracing = False` makes every later
  run black for no visible reason.
- **Silence in the Remix log proves nothing.** It does not write per frame. Evidence is on screen:
  the developer menu, or Debug View changing pixels.
- **An echoed config key is not a validated one.** A deliberately misspelled key echoes the same way.
