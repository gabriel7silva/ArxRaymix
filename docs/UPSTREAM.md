# Relationship to Arx Libertatis

This repository is a fork of [Arx Libertatis](https://github.com/arx/ArxLibertatis), combined with
the [ArxWindows](https://github.com/arx/ArxWindows) dependency tree so it builds from one clone.

| | |
|---|---|
| Base version | Arx Libertatis **1.3-dev** |
| Base commit | `5b95e4c5ca9d583f1b11c085326979772645e0f3` |
| `git describe` | `1.2-2756-g5b95e4c5c` |

Upstream history is not carried here — this repository starts at its own initial commit. To diff
against Arx Libertatis, clone it and compare the `arx/` directory:

```bash
git clone https://github.com/arx/ArxLibertatis.git
```

```bash
git -C ArxLibertatis checkout 5b95e4c5ca9d583f1b11c085326979772645e0f3
```

Upstream licensing, authorship and history files are kept intact: [`arx/LICENSE`](../arx/LICENSE),
[`arx/COPYING`](../arx/COPYING), [`arx/AUTHORS`](../arx/AUTHORS), [`arx/CHANGELOG`](../arx/CHANGELOG).

## New files

None of these exist upstream; all of the Remix work lives here.

```
arx/src/graphics/d3d9/D3D9Renderer.{h,cpp}
arx/src/graphics/remix/RemixApi.{h,cpp}
arx/src/graphics/remix/RemixConvert.{h,cpp}
arx/src/graphics/remix/RemixEntities.{h,cpp}
arx/src/graphics/remix/RemixProbe.{h,cpp}
arx/src/graphics/remix/RemixRenderer.{h,cpp}
arx/src/graphics/remix/RemixScene.{h,cpp}
arx/src/graphics/remix/RemixTextures.{h,cpp}
arx/third_party/rtx-remix/                      (vendored Remix C API header, MIT)
```

## Modified files

Twenty-six files differ from the base commit.

### The rendering path

| File | Change | +/− |
|---|---|---|
| `src/graphics/Renderer.h` | `wantsWorldSpaceEntities()`, so a backend can declare the space it wants | +16 / −0 |
| `src/graphics/Renderer.cpp` | `adoptListeners()`, to move listeners between renderers | +5 / −0 |
| `src/graphics/Vertex.h` | `TexturedVertex` gained a normal | +11 / −0 |
| `src/animation/AnimationRender.cpp` | Entities emitted in world space with posed normals when the backend asks | +47 / −5 |
| `src/graphics/Draw.cpp` | Camera include | +2 / −0 |
| `src/window/SDL2Window.{h,cpp}` | Creates `D3D9Renderer`, hosts the Remix device, exits without destroying the HWND | +104 / −5 |
| `src/scene/LoadLevel.cpp` | Releases Remix level resources on unload | +9 / −0 |

### Input, because Remix owns the window

| File | Change | +/− |
|---|---|---|
| `src/input/SDL2InputBackend.cpp` | Remix subclasses the HWND and swallows `WM_KEYDOWN`/`WM_KEYUP`; the OS key state is polled instead. Deliberately not merged with SDL's view, or a keydown without its keyup sticks forever | +157 / −1 |
| `src/core/Core.h` | `REQUEST_SPEECH_SKIP` shared with that path | +1 / −0 |

### Presentation at Remix window sizes

| File | Change | +/− |
|---|---|---|
| `src/gui/Logo.cpp` | The 640×480 studio logos are a postage stamp on a 1080p window; scaled uniformly to fill it | +14 / −1 |
| `src/gui/LoadLevelScreen.cpp` | Same treatment for the loading screen | +17 / −0 |
| `src/cinematic/Cinematic.cpp` | Diagnostics sampled across the fade-in instead of a one-shot log that only ever saw a black frame | +77 / −1 |
| `src/gui/Text.cpp` | A missing icon font is a warning, not an error | +1 / −1 |

### Options and configuration

| File | Change | +/− |
|---|---|---|
| `src/gui/MainMenu.cpp` | The Raymix video options page, and the renderer selector | +124 / −0 |
| `src/core/Config.{h,cpp}` | `video.remix` settings and their persistence | +38 / −3 |
| `src/gui/widget/Widget.h` | Supporting change for that page | +1 / −0 |
| `src/core/Startup.cpp` | Runs `--remix-probe` before the game starts | +22 / −0 |
| `src/core/ArxGame.cpp` | Remix lifecycle and render-mode switching | +71 / −5 |

### Crash fixes found along the way

| File | Change | +/− |
|---|---|---|
| `src/game/Entity.cpp` | Validates the owner pointer before dereferencing it during teardown | +4 / −2 |
| `src/game/EntityManager.cpp` | Drops ownership links while every entity is still alive, so `~Entity` cannot touch a freed owner | +8 / −0 |

These two are not Remix-specific and would apply upstream.

### Build and version

| File | Change | +/− |
|---|---|---|
| `CMakeLists.txt` | `WITH_RTX_REMIX`, the D3D9 and Remix sources, and a git-directory fallback for the build id | +53 / −5 |
| `src/Configure.h.in` | `ARX_HAVE_RTX_REMIX`, `ARX_HAVE_D3D9` | +2 / −0 |
| `VERSION` | `Arx Raymix 1.4` | +1 / −1 |
| `cmake/VersionScript.cmake` | Build id rendered as `build <hash>` | +1 / −1 |

## Rebasing onto a newer upstream

The engine changes are additive and mostly guarded by `#if ARX_HAVE_RTX_REMIX` or
`#if ARX_HAVE_D3D9`, so a rebase is usually mechanical. The two files most likely to conflict are
`SDL2Window.cpp` and `MainMenu.cpp`, which touch code upstream also edits.
