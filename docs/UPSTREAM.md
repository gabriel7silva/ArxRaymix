# Relationship to Arx Libertatis

This repository is a fork of [Arx Libertatis](https://github.com/arx/ArxLibertatis), combined with the [ArxWindows](https://github.com/arx/ArxWindows) dependency tree so it builds from one clone.

| | |
|---|---|
| Base version | Arx Libertatis **1.3-dev** |
| Base commit | `5b95e4c5ca9d583f1b11c085326979772645e0f3` |
| `git describe` | `1.2-2756-g5b95e4c5c` |

Upstream history is not carried here — this repository starts at its own initial commit. To diff against Arx Libertatis, clone it and compare the `arx/` directory:

```bash
git clone https://github.com/arx/ArxLibertatis.git
git -C ArxLibertatis checkout 5b95e4c5ca9d583f1b11c085326979772645e0f3
```

Upstream licensing, authorship and history files are kept intact: [`arx/LICENSE`](../arx/LICENSE), [`arx/COPYING`](../arx/COPYING), [`arx/AUTHORS`](../arx/AUTHORS), [`arx/CHANGELOG`](../arx/CHANGELOG).

## New files (not upstream)

```
arx/src/graphics/d3d12/
arx/src/graphics/dxr/
arx/src/graphics/d3d9/
```

Leftover experiment (not compiled into the product path):

```
arx/src/graphics/remix/
arx/third_party/rtx-remix/
```

NVIDIA Streamline is fetched into `arx/third_party/streamline/` by `scripts/fetch-streamline.ps1` and is not an Arx Libertatis file.

## Modified files

The Windows remaster also touches window creation, configuration, Video / Render / Ray tracing menus, `GlobalFog`, draw helpers, and localisation overlays. The files most likely to conflict on an upstream rebase are `SDL2Window.cpp`, `MainMenu.cpp`, `Config.{h,cpp}` and `GlobalFog.cpp`.

Crash fixes that are not renderer-specific and would apply upstream:

| File | Change |
|---|---|
| `src/game/Entity.cpp` | Validates the owner pointer before dereferencing it during teardown |
| `src/game/EntityManager.cpp` | Drops ownership links while every entity is still alive |

### Build and version

| File | Change |
|---|---|
| `CMakeLists.txt` | Direct3D 12 and Direct3D 9 sources on Windows |
| `src/Configure.h.in` | `ARX_HAVE_D3D12`, `ARX_HAVE_D3D9` |
| `VERSION` | Product name / version for this fork |

## Rebasing onto a newer upstream

The engine changes are additive and mostly guarded by `#if ARX_HAVE_D3D12` or `#if ARX_HAVE_D3D9`. Expect mechanical conflicts in `SDL2Window.cpp` and `MainMenu.cpp`.
