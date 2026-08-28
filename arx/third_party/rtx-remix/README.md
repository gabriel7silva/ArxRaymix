# RTX Remix SDK (header only)

This directory vendors the **Remix C API header** used by Arx Raymix. Runtime binaries (`d3d9.dll`
and its dependencies) are **not** stored in Git.

## Pinned revision

| Field | Value |
|---|---|
| Upstream repo | [NVIDIAGameWorks/dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix) |
| Header path upstream | `public/include/remix/remix_c.h` |
| Header API version | **0.6.4** (`REMIXAPI_VERSION_MAJOR/MINOR/PATCH` in the header) |
| Matching runtime | **remix-1.5.2** ([release tag](https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.5.2)) |
| Header license | MIT (banner at the top of `remix_c.h`) |

To upgrade, re-fetch the header from the repository root:

```powershell
Invoke-WebRequest `
  -Uri 'https://raw.githubusercontent.com/NVIDIAGameWorks/dxvk-remix/main/public/include/remix/remix_c.h' `
  -OutFile 'arx/third_party/rtx-remix/include/remix/remix_c.h'
```

Then compare `REMIXAPI_VERSION_*` in the new header against the runtime release notes before
building. A header the loaded runtime cannot serve comes back from `RemixApi::load()` as
`incompatible API version` (`REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION`), logged at startup.

## Obtaining the runtime

The runtime is downloaded per machine and never committed — `runtime/` is gitignored.

1. Download `remix-<version>-release.zip` from the
   [RTX Remix releases](https://github.com/NVIDIAGameWorks/rtx-remix/releases).
2. Extract it into `runtime/remix-extract/` at the repository root.
3. Use the **x64 renderer** at `runtime/remix-extract/.trex/d3d9.dll`.

   The `d3d9.dll` at the root of the zip is the 32-bit bridge interposer and is not what this
   integration loads.

Official SDK notes:
[RemixSDK.md](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/documentation/RemixSDK.md)

## Build flag

```powershell
cmake -S . -B build -DWITH_RTX_REMIX=ON
```

The option defaults to `OFF` (`arx/CMakeLists.txt`). When it is on, CMake checks that
`third_party/rtx-remix/include/remix/remix_c.h` exists and fails the configure step if it does not,
and defines both `ARX_HAVE_RTX_REMIX` and `ARX_HAVE_D3D9`.

## Probe

The probe renders through the runtime without any game state, which is the way to tell a broken
integration apart from a broken runtime or driver:

```powershell
arx.exe --remix-probe --remix-dll runtime\remix-extract\.trex\d3d9.dll
```

`--remix-probe=3` caps it to three frames, for use in CI.

Without `--remix-dll`, `RemixApi::findRuntimeDll()` searches `ARX_REMIX_DLL` in the environment,
then `.trex\d3d9.dll` and `d3d9.dll` next to the executable, then a few paths relative to the
working directory.
