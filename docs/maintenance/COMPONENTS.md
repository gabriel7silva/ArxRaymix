# Components

What each part of the project is responsible for. For the picture, see [MAP.md](MAP.md). For where to make a specific change, see [WHERE-TO-EDIT.md](WHERE-TO-EDIT.md).

## Ownership tiers

Every directory falls into one of four tiers, and the tier tells you how freely you may change it.

| Tier | Meaning | Rule |
|---|---|---|
| **fork** | Written for Arx Raymix | Change freely |
| **upstream** | Arx Libertatis, carried unchanged or nearly so | Change only when you must; every edit is a future rebase conflict |
| **vendored** | Third-party code copied in | Do not edit; re-vendor instead |
| **dead** | Present but not built and not to be revived | Do not touch, do not extend, do not cite |

## The tree

### Top level

| Path | Responsible for | Tier |
|---|---|---|
| `CMakeLists.txt` | Superproject: points at the vendored libraries, adds `arx/`, copies runtime DLLs next to the executable | fork |
| `arx/` | The engine itself | mixed |
| `libs/` | Vendored Windows dependencies: Boost, FreeType, GLM, libepoxy, OpenAL, SDL, zlib | vendored |
| `docs/` | Public documentation, with a Portuguese mirror under `docs/pt-BR/` | fork |
| `scripts/` | PowerShell launchers and the Arx Fatalis install lookup | fork |
| `data/` | Upstream data-file documentation and icons | upstream |

Two directories are absent from this table on purpose: `build/` and `runtime/` are generated and both are gitignored.

### Inside `arx/`

| Path | Responsible for | Tier |
|---|---|---|
| `arx/CMakeLists.txt` | The real build. Every target is defined here, including `arx` itself | upstream, fork-modified |
| `arx/src/` | Engine sources | mixed |
| `arx/cmake/` | Build helpers, including the style-check target | upstream |
| `arx/data/` | Localisation and speech data shipped by the fork | mixed |
| `arx/tests/` | Unit tests, not built by default here | upstream |
| `arx/tools/` | Side tools: `arxunpak`, `arxsavetool`, crash reporter, profiler | upstream |
| `arx/.github/workflows/` | Upstream CI. Does not run for this fork — see the note at the end | upstream |
| `arx/third_party/`, `arx/thirdparty/` | Vendored: Streamline SDK (fetched), an RTX Remix SDK header leftover, a float parser | vendored |

### Engine subsystems, `arx/src/`

| Path | Responsible for | Tier |
|---|---|---|
| `core/` | Application lifecycle, the frame loop (`ArxGame`), settings (`Config`), localisation, save games, startup | upstream, fork-modified |
| `graphics/` | The renderer interface and every backend — see below | mixed |
| `scene/` | Level state: rooms and portals, level load and save, non-ray-traced lighting, the world render pass | upstream |
| `animation/` | Skeletal animation and the entity render pass; the source of world-space entity vertices | upstream |
| `gui/` | Menus and heads-up display, including the Ray tracing options page | upstream, fork-modified |
| `game/` | Gameplay objects: entities, camera, damage, the player | upstream |
| `script/` | The Arx script virtual machine | upstream |
| `io/` | File and stream access, INI reading and writing, logging | upstream |
| `platform/` | Operating system and compiler abstraction, crash handling, command-line option registration | upstream |
| `window/` | Window creation and renderer selection | upstream, fork-modified |
| `input/`, `audio/`, `physics/`, `ai/`, `math/`, `util/`, `cinematic/`, `lib/` | As named | upstream |

### Graphics backends, `arx/src/graphics/`

| Path | Responsible for | Tier |
|---|---|---|
| `Renderer.h`, `Renderer.cpp` | The interface every backend implements, and the render-state types | upstream, fork-extended |
| `d3d12/` | The Direct3D 12 raster backend. The fork's default on Windows | fork |
| `dxr/` | Hybrid DXR (AO, shadows, one bounce, water / metal reflections) and Streamline (DLSS, FG, experimental RR), layered on the D3D12 raster. Phase 6 path tracing is archived — do not start it here | fork |
| `d3d9/` | The Direct3D 9 raster backend, kept as a fallback and as a reference for comparing correctness | fork |
| `opengl/` | The upstream renderer. The non-Windows path | upstream |
| `data/`, `effects/`, `font/`, `image/`, `particle/`, `spells/`, `texture/` | Upstream graphics subsystems: mesh and level data, screen effects, fonts, image codecs, particles, spell effects, texture management | upstream |

## Telling fork code from upstream

Three signals, in order of reliability.

1. **The directory.** `graphics/d3d12/`, `graphics/dxr/` and `graphics/d3d9/` are entirely the fork's.
2. **The build guard.** Fork-only compilation is gated on `ARX_HAVE_D3D12` or `ARX_HAVE_D3D9`, declared in `arx/src/Configure.h.in`.

   ```
   grep -rn "ARX_HAVE_D3D12" arx/src
   ```
3. **`docs/UPSTREAM.md`** is the authority on the diff against Arx Libertatis, including the base commit. Read it before a rebase.

## Boundaries that are enforced

These are not style preferences. Each one has a reason that will bite.

- **Raster stays in `graphics/d3d12/`; ray work stays in `graphics/dxr/`.** The split is what lets the ray tracing be switched off entirely and leave a working renderer. Fog constants for the far plane live in `graphics/GlobalFog.{h,cpp}` (upstream file, fork-tuned).
- **`graphics/dxr/` must not include Direct3D 9 headers.** It is a Direct3D 12 consumer only. Streamline lives in `D3D12Streamline.{h,cpp}` in this directory.
- **The Direct3D 12 sources compile outside the unity blob.** `d3d9.h` defines `interface` as a macro, which breaks unrelated translation units when they are concatenated together. The exclusion is in `arx/CMakeLists.txt`; adding a new D3D12 source means adding it to the same list, not to the general source list.

  ```
  grep -n "ARX_D3D12_SOURCES" arx/CMakeLists.txt
  ```
- **The project builds without run-time type information.** `arx/CMakeLists.txt` compiles with `-fno-rtti` and defines `BOOST_NO_RTTI`. This is why `D3D12Rtao` carries its own small `ComPtr` instead of using the Windows Runtime one: that header requires facilities this build does not have.
- **One window, one device, one renderer per process.** There is no path that runs two backends at once, and adding one to compare output is not a debugging technique available here.

## Dead code

**The RTX Remix layer was deleted.** `arx/src/graphics/remix/` and `arx/third_party/rtx-remix/` are gone. Do not recreate them, and do not introduce `toRemix`, `remix::`, or `ARX_HAVE_RTX_REMIX`.

This grep must return no hits in compiled sources (the DXR README may still name `toRemix` in order to forbid it):

```
grep -rn --include=*.cpp --include=*.h -e "ARX_HAVE_RTX_REMIX" -e "graphics/remix" -e "remix::" arx/src
```

Any hit is leftover coupling and should be removed. The history of what Remix did is in `docs/UPSTREAM.md`. It is deliberately not repeated here, because a description of what it achieved is an invitation to revive it.

Two related prohibitions, for the same reason — they waste days and answer nothing:

- **Never dual-boot the OpenGL renderer to answer a Direct3D question.** The backends differ in coordinate conventions and state handling, so a difference between them tells you nothing about which is wrong.
- **Never compare against the Direct3D 9 backend without saying so.** It is a legitimate correctness reference, but it is a different implementation, not ground truth.

## Runtime artifacts

None of these are committed. `runtime/` is gitignored in full.

| Artifact | Written by | Located by |
|---|---|---|
| `cfg.ini` | `Config::save`, on every settings change | `fs::getConfigDir` in `arx/src/io/fs/SystemPaths.cpp` |
| `arx.log` | The file log backend, attached at startup | `arx/src/core/Startup.cpp` |
| `save/gsave.sav` | The save system | `arx/src/core/SaveGame.cpp` |
| `crashes/` | The crash handler | `arx/src/core/Startup.cpp` |
| `data.dirs` | The build, next to the executable | `arx/CMakeLists.txt` |

The user directory holding them is chosen by `fs::initSystemPaths` and can be overridden with `--user-dir`:

```
grep -n "setUserDir" arx/src/io/fs/SystemPaths.cpp
```

## A gap worth knowing about

Upstream continuous integration lives in `arx/.github/workflows/ci.yml`. It triggers on a branch this fork does not use, and its only jobs are Linux and macOS. **The fork's entire Windows, Direct3D 12 and ray tracing surface has no automated coverage.** Nothing will catch a regression except running the game and reading `runtime/user/arx.log`. That is why [WHERE-TO-EDIT.md](WHERE-TO-EDIT.md) asks for a log transcript in every pull request.
