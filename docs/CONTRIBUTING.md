# Contributing

English is the default for file names, folders, comments, and documentation. Portuguese lives under [`pt-BR/`](pt-BR/).

## Where things live

| Path | What it is |
|---|---|
| `arx/src/graphics/d3d12/` | Direct3D 12 raster backend (default on Windows) |
| `arx/src/graphics/dxr/` | Hybrid DXR (AO, shadows, GI, reflections) and Streamline (DLSS, FG, RR) |
| `arx/src/graphics/d3d9/` | Direct3D 9 raster backend (fallback) |
| `arx/src/graphics/GlobalFog.{h,cpp}` | Far plane from the Render distance slider (`fog=`) |
| `arx/src/window/SDL2Window.cpp` | Chooses the backend, creates the HWND and the device |
| `arx/src/gui/MainMenu.cpp` | Video, Render, and Ray tracing option pages |
| `arx/src/core/Config.{h,cpp}` | `config.video.renderer` and the `dxr_*` keys |
| `arx/src/graphics/Renderer.h` | `getGraphicsApiName()`, `needsHalfPixelOffset()`, `applyWorldRayEffects()` |
| `scripts/run-d3d12.ps1` | Launch DirectX 12 |
| `scripts/run-d3d9.ps1` | Launch DirectX 9 |
| `scripts/fetch-streamline.ps1` | Fetch the NVIDIA Streamline SDK once |
| `scripts/Find-ArxFatalis.ps1` | Steam / GOG lookup, no hard-coded paths |

The RTX Remix layer was deleted. Do not revive it. Do not start path-traced multi-bounce (Phase 6 is archived).

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

## Tests

Unit tests live in `arx/tests/` (CppUnit). CppUnit is not vendored, so the Windows build leaves them off; CI builds and runs them on Linux for every pull request, next to an MSVC build of the game ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)). To run them locally on Linux:

```bash
cmake -S arx -B build-tests -G Ninja -DBUILD_TESTS=ON -DICON_TYPE=none
cmake --build build-tests --target arxtest
./build-tests/arxtest
```

Logic that can be tested without a GPU goes in a header of its own so the test does not drag in the renderer: `core/DxrPreset.h`, `core/LocalisationEncoding.h`, `graphics/PrimitiveIndices.h`, `graphics/dxr/DxrJitter.h`.

## Debugging

- **One symptom at a time.** Prove it in `runtime/user/arx.log` before stacking hypotheses.
- **`arx.exe --list-dirs`** settles which data directory is in use.
- Do not dual-boot OpenGL on Windows unless that is the question.
- New DXR values go in the `Params` (`b0`) or `ViewParams` (`b1`) constant buffers, keeping the C++ struct and the HLSL `cbuffer` in step ([INV-01](maintenance/INVARIANTS.md#inv-01)). Both are root CBVs, so the root signature has room; `Params` is a fixed 256 bytes (`kParamsBytes`).

## Privacy

This repository is public. Never commit:

- User profile paths
- Local clone drive letters
- `runtime/user/` (config, logs, saves)
- Generated `data.dirs`
- Personal scripts that point at a GPU kit or Downloads folder

## Style

Match the surrounding code: tabs, the existing brace style, and comments that explain *why* rather than narrate the next line. D3D12 sources are compiled outside the unity blob (`d3d9.h` defines `interface`). No RTTI (`/GR-`); use `static_cast`, not WRL `ComPtr`.
