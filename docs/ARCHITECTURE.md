# Architecture

How the Windows raster backends are wired. Where a comment in the source and this document disagree, read the source.

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

Ray-traced AO (Option A) and later DXR shadows live under [`arx/src/graphics/dxr/`](../arx/src/graphics/dxr/README.md). RTAO is compiled with D3D12 and toggled from **Options → Ray tracing**.

## Leftover Remix sources

`arx/src/graphics/remix/` and `arx/third_party/rtx-remix/` still exist from an earlier experiment. `ARX_HAVE_RTX_REMIX` is not enabled. Those files are not how the game is launched.
