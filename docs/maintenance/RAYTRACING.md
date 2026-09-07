# Ray tracing: where it lives

Locations and order. **Why** any value is what it is belongs in `arx/src/graphics/dxr/README.md`; this file does not repeat it. See [What this file does not cover](#what-this-file-does-not-cover).

For the picture, see [MAP.md](MAP.md). For turning things up or down, [TUNING.md](TUNING.md). For what breaks, [INVARIANTS.md](INVARIANTS.md).

## What is where

| Concern | File | Entry symbol |
|---|---|---|
| The hook every backend may implement | `arx/src/graphics/Renderer.h` | `Renderer::applyWorldRayEffects`, default does nothing |
| Where the hook is called in the frame | `arx/src/core/ArxGame.cpp` | `ArxGame::renderLevel` |
| The Direct3D 12 implementation of the hook | `arx/src/graphics/d3d12/D3D12Renderer.cpp` | `D3D12Renderer::applyWorldRayEffects` |
| Capability reporting to the menu | `arx/src/graphics/d3d12/D3D12Renderer.cpp` | `D3D12Renderer::supportsRayTracing` |
| The ray tracer itself | `arx/src/graphics/dxr/D3D12Rtao.cpp`, `.h` | `D3D12Rtao` |
| Streamline (DLSS, FG, RR) | `arx/src/graphics/dxr/D3D12Streamline.cpp`, `.h` | `D3D12Streamline` |
| Far plane / fog wall | `arx/src/graphics/GlobalFog.cpp`, `.h` | `ARX_GLOBALMODS_Apply` |
| DXR collect / trace range | `arx/src/graphics/dxr/D3D12Rtao.h` | `D3D12Rtao::distancePreset` |
| Choosing which lights cast shadows | `arx/src/graphics/d3d12/D3D12Renderer.cpp` | `fillShadowLights`, `emitShadowLight` |
| Logging which lights joined or left | `arx/src/graphics/d3d12/D3D12Renderer.cpp` | `describeShadowLight` |
| Gathering level geometry as casters | `arx/src/graphics/d3d12/D3D12Renderer.cpp` | `collectRoomCasters` |
| Gathering entities as casters | `arx/src/graphics/d3d12/D3D12Renderer.cpp` | `collectEntityCasters` |
| The shaders | `arx/src/graphics/dxr/D3D12Rtao.cpp` | the `kRayLib` and `kComposite` string literals |
| The settings | `arx/src/core/Config.h` | the video section of `Config` |
| The menu page | `arx/src/gui/MainMenu.cpp` | `RayTracingOptionsMenuPage` |

Two things are deliberately **not** in that table because people look for them and they do not exist:

- There is no separate shader file. Both shaders are string literals inside `D3D12Rtao.cpp`, compiled at start-up.
- There is no ray tracing pass over the draw calls. The ray tracer builds its own caster list from the level and the entities; geometry submitted for rasterisation is never copied into the acceleration structure.

## The order of a frame

`ArxGame::renderLevel`, in order. The ray tracing slot is marked.

1. `Clear`
2. `cinematicBorder.render`
3. `ARX_SCENE_Render` — the whole world, see below
4. `drawDebugRender`
5. **`applyWorldRayEffects`** — skipped during cinematics
6. particles
7. magical flares
8. spell effects
9. `g_renderBatcher.render`
10. `renderLightFlares`
11. heads-up display, notes, minimap, cursor

`ARX_SCENE_Render`, in order:

1. opaque room geometry, per visible room
2. `ARXDRAW_DrawInterShadows`
3. `ARX_THROWN_OBJECT_Render`
4. `RenderInter` — entities
5. the player, drawn last so it is never clipped by walls in first person
6. `eyeball.render`
7. `PolyBoomDraw`
8. transparency
9. transparent room geometry
10. `RenderWater`, `RenderLava`
11. `Halo_Render`

The position of step 5 in the outer list is load-bearing, not incidental. See [INV-06](INVARIANTS.md#inv-06).

## What the pass does, in stages

`D3D12Rtao::apply`, in order. Each stage names the method that implements it.

1. **Check** the effect settings, the targets and the light buffer. Any failure returns early and leaves the rastered frame alone — [INV-12](INVARIANTS.md#inv-12).
2. **Upload geometry** — `ensureGeometryBuffers`. Room vertices are uploaded only when the room cache was rebuilt; dynamic vertices every frame.
3. **Build acceleration structures** — `buildAcceleration`. One bottom-level structure per geometry category, then one top-level structure holding an instance for each.
4. **Copy the backbuffer** into a texture the shaders can read. The composite needs the unshaded image while also drawing to it.
5. **Write the light buffer and the root constants**. The constant layout is [INV-01](INVARIANTS.md#inv-01).
6. **Dispatch rays** — one thread per pixel, running `RayGen` in `kRayLib`. This is where occlusion, shadows, bounce light and the temporal blend all happen.
7. **Composite** — a full-screen triangle running `kComposite`, which denoises the traced buffers and combines them into the backbuffer.
8. **Copy to history** — this frame's occlusion, shadow, bounce and depth become next frame's history, and the current camera matrix is stored for reprojection.

Stage 8 is at the end, so every history read in stage 6 is last frame's data. History is invalid on the first frame and after a resize, and the code handles that by blending nothing until it is valid.

### Instances

| Instance | Geometry | Mask | Rebuilt |
|---|---|---|---|
| 0 | Cached level geometry from nearby rooms | `0x1` | On room change, `dxr_distance` change, or a long camera move |
| 1 | Entities and other moving geometry | `0x1` | Every frame |
| 2 | Water planes | `0x2` | When water triangles are submitted |
| 3 | Player mesh and held torch / weapon | `0x4` | Every frame. First-person body must not be in mask `0x1` |

AO / shadow / GI rays use mask `0x1`. Water specular uses `0x5` (opaque + player). Metal uses `0x7`. The hit shader reads the instance id to know which vertex buffer the triangle came from. Adding another category is three coordinated edits — [INV-08](INVARIANTS.md#inv-08).

## The two shader dialects

`kRayLib` and `kComposite` are adjacent string literals in one file, and they are compiled by **different compilers with different accepted syntax**. Editing the wrong one is the easiest mistake in this file to make.

- `kRayLib` is compiled as a shader-model-6 library through the newer compiler, loaded at run time. It contains the ray generation, closest hit and miss shaders.
- `kComposite` is compiled as shader-model-5 vertex and pixel shaders through the legacy compiler. It contains the full-screen pass.

The legacy compiler rejects syntax the newer one accepts. If the composite fails to build, the game runs as plain raster and says so in the log rather than crashing. See [INV-03](INVARIANTS.md#inv-03).

```
grep -n "kRayLib\|kComposite\|lib_6_3\|vs_5_0\|ps_5_0" arx/src/graphics/dxr/D3D12Rtao.cpp
```

## Feeding the acceleration structures

Three inputs, gathered on the renderer side and handed over through `D3D12Rtao`'s `addRoom` and `addWorld`.

**Lights** — `fillShadowLights` picks a bounded set from the level's lit static lights and the non-ignition dynamic ones, ranked by how much of the visible scene each can actually affect. Membership is deliberately sticky: a light already chosen is harder to displace than a newcomer is to admit, and every join or leave is faded rather than switched. See [INV-10](INVARIANTS.md#inv-10). `emitShadowLight` converts one engine light into the packed form the shader reads.

**Level geometry** — `collectRoomCasters` walks out from the camera's room through portals, bounded in both room count and portal hops by `D3D12Rtao::distancePreset(dxr_distance)`, and adds the polygons of each room it reaches. The result is **cached**. It is rebuilt when the camera changes room, when `dxr_distance` changes, or when the camera has moved far enough that the old set no longer describes its surroundings. Cutout polygons are excluded unless Transparency is High — [INV-07](INVARIANTS.md#inv-07).

**Entities** — `collectEntityCasters` runs every frame, because entities animate. It skips entities marked as casting no shadow, and the player. It reads world-space vertex positions from the entity's mesh when the animation system has produced them, and otherwise transforms the model-space vertices by the entity's own rotation, position and scale. It returns a hash of everything it emitted, which is what the `DXR entity movers` diagnostic reports; that is how you tell whether something is genuinely moving or whether the geometry is being rebuilt for no reason.

Both caster gatherers are bounded by a triangle budget. Hitting it drops triangles silently apart from one log line — see [TUNING.md](TUNING.md).

## What this file does not cover

`arx/src/graphics/dxr/README.md` is the source of truth for **why**, and it is maintained alongside the ray tracing code rather than alongside this guide. Go there for:

- why a shadow's ambient floor is what it is, and what a different value looked like
- why the sample directions are rotated per pixel instead of per frame
- why the bounce light is modulated by the surface colour
- why the light ranking formula weighs reach the way it does
- the phase-by-phase history of how this arrived (Phases 1–5; Phase 6 archived)

If you find yourself wanting to write a sentence here that contains "because" followed by a number, it belongs there instead.
