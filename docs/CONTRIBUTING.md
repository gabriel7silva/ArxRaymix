# Contributing

The Remix work is small and concentrated. Almost everything is in two directories.

## Where things live

| Path | What it is |
|---|---|
| `arx/src/graphics/d3d9/D3D9Renderer.cpp` | The renderer. Device creation, vertex buffers, world-space submission, smooth normals, light conversion. Large and load-bearing |
| `arx/src/graphics/remix/RemixApi.cpp` | Finds and loads the Remix DLL, wraps `Startup`/`Shutdown`, maps error codes to strings |
| `arx/src/graphics/remix/RemixScene.cpp` | Runtime configuration and light selection |
| `arx/src/graphics/remix/RemixProbe.cpp` | `--remix-probe` and the `--remix-dll` option |
| `arx/src/graphics/remix/RemixConvert.h` | The `--remix-debug` bits |
| `arx/src/graphics/remix/RemixTextures.cpp`, `RemixEntities.cpp`, `RemixRenderer.cpp` | The C API path. Not what the game runs — see [ARCHITECTURE.md](ARCHITECTURE.md) |
| `arx/src/animation/AnimationRender.cpp` | Entity conversion to world space with posed normals |
| `arx/src/graphics/Renderer.h` | `wantsWorldSpaceEntities()`, how a backend declares the space it wants |
| `arx/src/gui/MainMenu.cpp` | The Raymix video options page |
| `arx/third_party/rtx-remix/` | Vendored Remix C API header (MIT) |

Everything else under `arx/` is Arx Libertatis; [UPSTREAM.md](UPSTREAM.md) lists exactly what was
changed there.

## Building and running

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DWITH_RTX_REMIX=ON
```

```bash
cmake --build build --config RelWithDebInfo --target arx --parallel
```

```bash
powershell -ExecutionPolicy Bypass -File scripts
un-remix-scene.ps1 -LoadLevel 1
```

`-LoadLevel 1` drops straight into the first cell, which is small, enclosed and torch-lit — a fast
loop for anything about light or geometry.

## Debugging, and what counts as evidence

This integration punishes reasoning from logs. Some hard-won rules:

- **Judge by the screen.** The Remix log does not write per frame, so silence in it means nothing.
  The developer menu (`Alt+X`) and its Debug View are the real instruments.
- **Read both logs.** `runtime/user/arx.log` and `rtx-remix/logs/remix-dxvk.log` say different
  things about the same frame.
- **Check `user.conf` first** when a run misbehaves for no reason. It is written by the developer
  menu and outranks everything else.
- **`arx.exe --list-dirs`** settles any argument about which data directory is in use.
- **Change one thing per run.** Most of the bugs already found were invisible individually and
  obvious in isolation.
- **`--remix-probe`** renders through the runtime with no game state. If the probe works and the game
  does not, the problem is ours.

## Good places to start

These are the claims in the README that the code implements but nobody has confirmed by looking:

- **Path tracing off.** The options page sets `rtx.enableRaytracing = False`, which should rasterise
  the game's own draw calls. Whether the result is a correct image is unverified.
- **The Raymix options page as a whole** — quality tiers, DLSS, ray reconstruction, denoiser, bloom.
- **Linked items**, weapons and torches carried by NPCs.
- **Smooth normal seams** where one surface spans two draw calls, since normals are accumulated per
  batch.

And the milestone in front of everything else:

- **World-space billboards.** Fire, magic and particles are screen-space overlays by default, so the
  tracer never sees them and a torch flame casts no light. `EERIECreateSprite()` can build the quads
  in world space instead under **`--remix-debug 131072`**, which is off by default because an
  earlier unconditional version left the game on a black screen with no level geometry and no crash
  in either log. Behind the bit it can be A/B'd between two runs of the same build:

  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts/run-remix-scene.ps1 -LoadLevel 1
  powershell -ExecutionPolicy Bypass -File scripts/run-remix-scene.ps1 -LoadLevel 1 -DebugMask 131072
  ```

  If the second one loads the level and the flames light the walls, the bit becomes the default. If
  it goes black again, the difference between the two runs is now one flag rather than one build.

## Style

Match the surrounding code: tabs, the existing brace style, and comments that explain *why* rather
than restating the call. The comments in `D3D9Renderer.cpp` and `RemixScene.cpp` carry a lot of
history about failed approaches — please keep that habit, since it is what stops the same wrong turn
being taken twice.
