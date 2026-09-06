# DirectX Raytracing

This directory is **Option A** (RTAO) and **Phase 2 DXR shadows**. The D3D12 raster stays in [`../d3d12/`](../d3d12/). Do not put raster, D3D9, or Remix sources here.

## Phases

```text
    world raster  →  Phase 1 RTAO  →  Phase 2 DXR shadows  →  HUD / Present
```

AO/shadow composite uses a depth-aware bilateral blur (no box mix across edges). Short AO hits and depth discontinuities do not write black specks.

### Phase 1 — Option A (RTAO)

Ray-traced ambient occlusion on top of the existing D3D12 raster. Dark corners and crevices only. No global illumination and no path tracing. Toggled from **Options → Ray tracing** (`cfg.ini` `[video] rtao=`).

### Phase 2 — DXR shadows

Trace shadows for map lights (`g_culledDynamicLights`, Arx space, no `toRemix`). The world stays raster. Toggled from **Options → Ray tracing** (`cfg.ini` `[video] dxr_shadows=`). Receivers come from raster depth. Casters are nearby rooms (rebuilt when the player room changes) plus in-scene entities; draw-call geometry is not copied into the TLAS. Up to 8 lights (4 nearest + 4 brightest).

## Out of scope

- RTX Remix, `SetupCamera`, `CreateLight`, `DrawInstance`
- Path tracing or full GI
- Porting HUD, menus, or 2D cinematics to DXR
- OpenGL dual-boot

## Hook

`Renderer::applyWorldRayEffects()` runs after the **world** is rasterized and **before** HUD, from [`ArxGame::renderLevel`](../../../core/ArxGame.cpp). `showFrame` is too late (HUD is already in the backbuffer). 2D already uses `ZFUNC ALWAYS`. Do not apply RTAO to HUD or cinematics (`isInCinematic()`).

`createDevice` asks for `D3D_FEATURE_LEVEL_11_0` and queries `D3D12_FEATURE_D3D12_OPTIONS5` / `RaytracingTier`. If the tier is `NOT_SUPPORTED`, raster only.

## Menu

**Options → Ray tracing**: ambient occlusion Off / Low / High and shadows Off / On (runtime, no restart). Widgets are disabled on D3D9 or when DXR is missing.

## Verification

One symptom at a time. Prove it in `runtime/user/arx.log`:

1. `Using D3D12 renderer`
2. `RaytracingTier=` … or `NOT_SUPPORTED` / `Ray tracing unavailable (D3D9)`
3. `RTAO disabled` or `RTAO enabled quality=…`
4. `DXR shadows disabled` or `DXR shadows enabled lights=…`

Quit through the **menu**.
