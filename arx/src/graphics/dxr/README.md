# DirectX Raytracing

This directory is **Option A** (RTAO), **Phase 2 DXR shadows**, and **Phase 3 penumbra + 1-bounce GI**. The D3D12 raster stays in [`../d3d12/`](../d3d12/). Do not put raster, D3D9, or Remix sources here.

## Phases

```text
    world raster  →  Phase 1 RTAO  →  Phase 2/3 shadows + GI  →  HUD / Present
```

AO/shadow composite uses a depth-aware bilateral blur (no box mix across edges). Short AO hits and depth discontinuities do not write black specks. GI is additive half-res, then `* ao * sh`.

### Phase 1 — Option A (RTAO)

Ray-traced ambient occlusion on top of the existing D3D12 raster. Dark corners and crevices only. Toggled from **Options → Ray tracing** (`cfg.ini` `[video] rtao=`).

### Phase 2 — DXR shadows

Trace shadows for map lights (lit `g_staticLights` plus non-ignition dynamic lights, Arx space, no `toRemix`). The world stays raster. Toggled from **Options → Ray tracing** (`cfg.ini` `[video] dxr_shadows=`). Receivers come from raster depth. Casters are nearby rooms (rebuilt when the player room changes or the camera moves 1800 units) plus in-scene entities; draw-call geometry is not copied into the TLAS.

Light selection (`fillShadowLights`): up to 16 lights ranked by `intensity * fallend / (outside + 200)²`, where `outside` is how far the camera sits beyond the light's shadow reach (`fallend * 1.35`). A light already in the set gets a 1.35 bonus and a wider distance slack (1600 vs 900). There is no near cut beyond 8 units: an 80-unit one made the cell torch flap in and out of the set as the idle animation bobbed the camera, and 40 still dropped it when the player walked up to it. Ranking by raw `intensity * fallend` let bright far lights evict it as soon as the player stepped back. Every entry / exit is a presence fade (`GpuLight::presence`, 10 frames) so a set change is never a one-frame pop.

Shadow weight per pixel is `(Σ vis·attn + 0.04) / (Σ attn + 0.04)`: the ambient floor means only the direct share of a light is shadowed, so a blocked weak far light does not drag an already dark room to the full umbra (0.3 here washed a torch umbra to half strength). The composite applies `lerp(0.10, 1, sh)`. AO mixes at 1.0 with a 0.45 floor (up to 55 % dark). GI carries the light's hue (`GpuLight` is three float4s: position/intensity, falloff/radius/presence, colour), is clamped to luma 0.35 in RayGen, rejected above 0.6 in the upsample, and added as `bounce = gi * (0.15 + min(color, 0.45)) * 2.0; color += bounce * (1 - color)`: the lit raster colour stands in for albedo, so it is capped and the add is screen-like, otherwise a table already blown out by a spell light bounced itself to pure white; grey bounce with a 0.25 floor read as a white haze on the ceiling above the torch; the original flat `* 0.15` on a 0.12 clamp was at most +1.8 % and invisible. These strengths are the user's call ("triplica os efeitos"); quality levels only change ray counts.

### Phase 3 — Penumbra and one bounce

Each light is a disk (`radius = clamp(max(ex_radius, fallstart * 0.08), 8, 36)`). Shadow rays are a fixed Poisson disk (2 / 8 / 16 on Low / Medium / High; batches of 8 rotated 22.5° on a 0.72 ring) rotated per pixel by interleaved gradient noise (screen-space, no frame index: it dithers instead of banding and never crawls). AO traces 8 / 16 / 24 rays and GI 4 / 8 / 16 at half resolution, each batch of 8 rotated 15°. Softness is the sample average, not a fake 12-unit TMax fade. One-bounce diffuse GI at half resolution, analytic only (no color reprojection). Toggle `cfg.ini` `[video] dxr_gi=`.

### Temporal history

The player camera never rests: `view_attach` follows the idle animation, so with `view_bobbing` the camera bobs a few units even when standing still and every single-frame hard edge flickers. RayGen keeps last frame's AO, shadow and raw depth (`m_aoPrev` / `m_shadowPrev` / `m_depthPrev`, copied at the end of `apply()`), reprojects the current world position through `prevViewProj`, fetches the history bilinearly (2x2, each tap validated, nearest tap at twice the tolerance as a fallback) and blends `lerp(history, current, 0.08)`. Discontinuity pixels keep a generous tolerance; a pixel with no history shows the raw dithered value, which sparkles at full strength. A tap is valid when its stored linear depth (`projB / (z - projA)`, from `Camera.cpp`'s projection) is within `max(2 %, 2 units) + 2 * slope` of the expected one, `slope` being the per-pixel linear-depth step already known from the normal reconstruction; a flat tolerance rejected the history of floors seen at a grazing angle and those pixels sparkled while walking. History is dropped on resize and on the first frame. `prevViewProj` lives where the unused `viewProj` was, so `kRootConstants` stays 52 and `pad0` / `pad1` keep it float4-aligned.

GI has the same history (`m_giPrev`, half-res, nearest texel at the reprojected pixel / 2, blended only when the full-res validation passed). Each bounce ray's fill is clamped to 0.3 and the sum is divided by rays *launched*; dividing by rays that hit let one lucky ray next to a lamp light the whole 2x2 texel, the white GI sparks that came back at full strength. GI rays use the per-pixel rotation too.

Sky is only the cleared depth (`SKY_Z = 0.99999`). The previous `0.999` reached at `w ≈ 865` units (near 1, far 6400), so AO and shadows silently stopped beyond that and the boundary flickered as the camera bobbed across it.

### Self-shadow acne on thin geometry

At full strength, gate bars and cutout decals (roots, webs) went black. Causes and fixes:

- Normal from depth used a raw-z discontinuity test (`0.04`) that never fired, so at any silhouette the normal tilted up to ~80° and rays started inside the receiver. Now each axis takes the closer neighbour in linear depth and a pixel is a discontinuity when the step exceeds `4 * footprint` (`footprint = w * pixelWorld`, `pixelWorld = 2 / (width * proj[0][0])`, sent in the old `frameIndex` slot).
- Ray origins step off by `4 + 2 * footprint` (shadow) / `8 + 2 * footprint` (AO, GI) and `TMin` equals that bias, so the receiver's own thickness is skipped.
- Cutout textures (`Texture::hasAlpha()`, color-keyed too) are opaque quads to the BLAS since rays never see the texture; those polygons and entity faces are not casters at all until any-hit alpha testing exists. `DXR room casters rooms=… alphaSkipped=…` says how many were dropped; if that is most of the room, the criterion is catching real walls.
- The composite floors `ao * sh` at 0.08 so the two never compound to black.

The composite bilateral is 7x7 (`radius 3`) for shadows and 11x11 (`radius 5`) for AO. The old 13x13 on shadows erased bar shadows a few pixels wide once the camera stepped back; the per-pixel rotation plus history do the smoothing now. Discontinuity pixels trace AO too, with the camera-facing normal, instead of being forced to 1 (bright specks on thin geometry).

## Later (not this directory yet)

- Phase 4 — specular reflections (water / metal)
- Phase 5 — DLSS / Ray Reconstruction / Streamline

## Out of scope

- RTX Remix, `SetupCamera`, `CreateLight`, `DrawInstance`
- Path tracing / multi-bounce / irradiance volumes
- Porting HUD, menus, or 2D cinematics to DXR
- OpenGL dual-boot

## Hook

`Renderer::applyWorldRayEffects()` runs right after `ARX_SCENE_Render()` (opaque world, entities, transparent polys, water, halos) and **before** particles, magic flares, spell effects, light flares and HUD, from [`ArxGame::renderLevel`](../../../core/ArxGame.cpp). Composited later, a spell's additive flare was multiplied by the shading of the surface behind it and showed the table's shadow through itself. `showFrame` is too late (HUD is already in the backbuffer). 2D already uses `ZFUNC ALWAYS`. Do not apply RTAO to HUD or cinematics (`isInCinematic()`).

`createDevice` asks for `D3D_FEATURE_LEVEL_11_0` and queries `D3D12_FEATURE_D3D12_OPTIONS5` / `RaytracingTier`. If the tier is `NOT_SUPPORTED`, raster only.

## Menu

**Options → Ray tracing**: ambient occlusion, shadows, and indirect light each have Off / Low / Medium / High (runtime, no restart). Widgets are disabled on D3D9 or when DXR is missing.

## Verification

One symptom at a time. Prove it in `runtime/user/arx.log`:

1. `Using D3D12 renderer`
2. `RaytracingTier=` … or `NOT_SUPPORTED` / `Ray tracing unavailable (D3D9)`
3. `RTAO disabled` or `RTAO enabled quality=…`
4. `DXR shadows disabled` or `DXR shadows enabled quality=… lights=…`
5. `DXR GI disabled` or `DXR GI enabled quality=…`

Flicker diagnostics (rate limited, always on):

- `DXR lights set changed #n n=… cands=… added=… removed=…` followed by one `DXR light +/-` line per light (static index or dyn pointer, pos, dist, intensity, falloff). Standing still this must not repeat; if it does, a light is straddling a cut.
- `DXR lights params changed` — same set, different pos / falloff / intensity. Should stay silent.
- `DXR entity movers #n n=… tris=…: names` — entities whose caster vertices changed since last frame. NPCs and rats are legitimate; a static prop here is a bug.
- `DXR room cache tris=… lights=…` — once per room change, never every frame.

`arx.exe --loadslot 0` loads the newest save without the menu; quit through the **menu** (or a window close, which also shuts down cleanly).
