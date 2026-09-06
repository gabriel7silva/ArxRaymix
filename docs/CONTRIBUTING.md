# Contributing

English is the default for file names, folders, comments, and documentation. Portuguese lives under [`pt-BR/`](pt-BR/).

## Where things live

| Path | What it is |
|---|---|
| `arx/src/graphics/d3d12/` | Direct3D 12 raster backend (default on Windows) |
| `arx/src/graphics/d3d9/` | Direct3D 9 raster backend (fallback) |
| `arx/src/window/SDL2Window.cpp` | Chooses the backend, creates the HWND and the device |
| `arx/src/gui/MainMenu.cpp` | Video Options: current API, slider, restart notice |
| `arx/src/core/Config.{h,cpp}` | `config.video.renderer` persistence |
| `arx/src/graphics/Renderer.h` | `getGraphicsApiName()`, `needsHalfPixelOffset()` |
| `scripts/run-d3d12.ps1` | Launch DirectX 12 |
| `scripts/run-d3d9.ps1` | Launch DirectX 9 |
| `scripts/Find-ArxFatalis.ps1` | Steam / GOG lookup, no hard-coded paths |

`arx/src/graphics/remix/` is leftover and is not compiled in. Do not revive it in the product docs or the default launch path.

Everything else under `arx/` is Arx Libertatis; [UPSTREAM.md](UPSTREAM.md) lists what diverged.

## Building and running

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config RelWithDebInfo --target arx --parallel
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-d3d12.ps1
powershell -ExecutionPolicy Bypass -File scripts/run-d3d9.ps1
```

Pass `-DataDir '<path to Arx Fatalis>'` if Steam / GOG detection cannot see the install. Logs: `runtime/user/arx.log`. Quit from the menu.

## Debugging

- **One symptom at a time.** Prove it in `runtime/user/arx.log` before stacking hypotheses.
- **`arx.exe --list-dirs`** settles which data directory is in use.
- Do not dual-boot OpenGL on Windows unless that is the question.

## Privacy

This repository is public. Never commit:

- User profile paths
- Local clone drive letters
- `runtime/user/` (config, logs, saves)
- Generated `data.dirs`
- Personal scripts that point at a GPU kit or Downloads folder

## Style

Match the surrounding code: tabs, the existing brace style, and comments that explain *why* rather than narrate the next line. D3D12 sources are compiled outside the unity blob (`d3d9.h` defines `interface`). No RTTI (`/GR-`); use `static_cast`, not WRL `ComPtr`.
