# Architecture

How the Windows raster backends are wired, and how the ray tracing layer sits on top of one of them. Where a comment in the source and this document disagree, read the source.

This is the overview. [`docs/maintenance/`](maintenance/README.md) is the working reference for people changing the code.

## One window, two backends

On Windows, `SDL2Window` owns a single HWND and constructs **one** renderer for the process:

| Backend | Sources | When it is used |
|---|---|---|
| Direct3D 12 / DirectX 12 | `arx/src/graphics/d3d12/` | Default (`auto`, `Direct3D 12`, `DirectX 12`, `d3d12`, `dx12`) |
| Direct3D 9 / DirectX 9 | `arx/src/graphics/d3d9/` | Saved or requested D3D9, or D3D12 `createDevice` failure |

```text
ARX_RENDERER  →  env wins for this process
cfg.ini [video] renderer=  →  otherwise
"auto"  →  Direct3D 12
```

The device is created at startup. Changing the API in **Options → Video** writes `config.video.renderer` and saves `cfg.ini`. It does **not** destroy or recreate the device. The next launch reads the saved value.

`Renderer::getGraphicsApiName()` is the session name shown in the menu (`DirectX 12` or `DirectX 9`).

## Construction

`SDL2Window` (Windows):

1. Choose `D3D12Renderer` or `D3D9Renderer` from the env / config rules above.
2. Create an SDL window and obtain the HWND.
3. Call `createDevice` on the chosen backend.
4. If D3D12 fails and D3D9 is compiled in, adopt listeners onto a new `D3D9Renderer` and create that device instead.

OpenGL is the non-Windows path. It is not a runtime choice on Windows.

## What each backend draws

Both backends implement the existing `Renderer` interface. The engine keeps submitting the same textured vertices, texture stages, and render states it always did.

D3D12 specifics that matter for correctness (not style):

- Flip-model swapchain barriers (`COMMON → RENDER_TARGET → COMMON`).
- Per-texture SRVs; stage 0 can be vertex RGB + texture alpha.
- Clip-space `w` reconstructed from `rhw` so world UVs do not smear. HUD uses `rhw = 1` and is not frustum-clipped.
- Half-pixel correction in the D3D12 vertex shader only (`needsHalfPixelOffset()` is false).
- Upload pitch from `GetCopyableFootprints`. GPU format `DXGI_FORMAT_B8G8R8A8_UNORM`.
- Alpha cutout matches D3D9 (`discard` when `A <= ref`).

D3D9 is the historical Windows raster path, including CPU frustum clipping for `XYZRHW` on the system raster.

## Persistence

| Store | Role |
|---|---|
| `cfg.ini` → `[video] renderer=` | Saved choice (`Direct3D 12` or `Direct3D 9`) |
| `ARX_RENDERER` | Per-process override used by `scripts/run-d3d12.ps1` and `scripts/run-d3d9.ps1` |
| Video Options slider | Writes and saves the choice; shows a restart notice when it differs from the live API |

`runtime/user/` is a local user directory. It is not part of the public tree.

## The ray tracing layer

`arx/src/graphics/dxr/` adds ray traced ambient occlusion, shadows, one bounce of indirect light, and water / metal reflections on top of the finished D3D12 raster. NVIDIA Streamline (`D3D12Streamline`) adds DLSS Super Resolution, Frame Generation, and experimental Ray Reconstruction. It is a layer, not a replacement: the world is rastered first, and switching every effect off leaves the raster untouched. Multi-bounce path tracing is archived.

It is compiled with the D3D12 backend and is reached through one optional method on the renderer interface, which defaults to doing nothing, so the other backends need no knowledge of it. The call sits after the world is drawn and before particles, flares and the HUD, so additive effects are not multiplied by the world's shading.

| | |
|---|---|
| Receivers | The depth buffer, one per screen pixel |
| Casters | Nearby rooms, cached and rebuilt on room changes, plus in-scene entities every frame |
| Lights | A bounded set chosen per frame from the level's lit lights, faded in and out so the set changing is never a visible pop |
| Stability | Each result is denoised and blended with the previous frame, reprojected through the previous camera |
| Settings | `rtao`, `dxr_shadows`, `dxr_gi`, reflections, denoise, `dxr_distance` in `cfg.ini`, applied immediately. DLSS / FG / RR are `dxr_dlss`, `dxr_fg`, `dxr_rr` |

Every stage degrades to plain raster rather than to a broken frame: unsupported hardware, a missing shader compiler, a shader that fails to build, or a scene with nothing to trace each stop the pass and leave the rastered image alone. The log always says which stage stopped.

Phases 1–4 are done. Phase 5 is DLSS + Frame Generation; Ray Reconstruction is experimental (homemade denoise stays if NGX create fails). Phase 6 (path-traced multi-bounce) is **archived**.

For why a given value is what it is, read [`arx/src/graphics/dxr/README.md`](../arx/src/graphics/dxr/README.md). For where things live and what breaks when you change them, read [`docs/maintenance/`](maintenance/README.md). Keys are listed in [`CONFIGURATION.md`](CONFIGURATION.md).

## Leftover Remix sources

`arx/src/graphics/remix/` and `arx/third_party/rtx-remix/` still exist from an earlier experiment. `ARX_HAVE_RTX_REMIX` is not enabled. Those files are not how the game is launched.
