# Where to edit

One recipe per common change. Each has the same four parts:

- **Touch** — the files, in order, and what changes in each
- **Then** — how to build it and what proves it worked
- **Watch out** — the invariants that apply
- **Re-derive** — a grep that finds every existing example, so you can copy a working one rather than trust this page

The fourth part matters most. If a recipe here drifts out of date, "look at how the last five were done" does not.

## Index

| I want to… | Recipe |
|---|---|
| Add a setting that persists in `cfg.ini` | [Add a config option](#add-a-config-option) |
| Put it in the menu | [Add a menu widget](#add-a-menu-widget) |
| Add text the player sees | [Add localised text](#add-localised-text) |
| Add a renderer capability | [Add a renderer entry point](#add-a-renderer-entry-point) |
| Add a command-line flag | [Add a command-line option](#add-a-command-line-option) |
| Change lighting that is not ray traced | [Change non-ray-traced lighting](#change-non-ray-traced-lighting) |
| Give the ray tracer more geometry | [Feed new geometry to the ray tracer](#feed-new-geometry-to-the-ray-tracer) |
| Add a texture or buffer a shader reads | [Add a shader resource](#add-a-shader-resource) |
| Add something to the log | [Add a diagnostic](#add-a-diagnostic) |
| Build and run it | [Build and run](#build-and-run) |
| Open a pull request | [Before you open a pull request](#before-you-open-a-pull-request) |

## <a id="add-a-config-option"></a>Add a config option

A setting is the same name written in five places. Miss one and it silently does not persist, or does not load, or is never clamped.

**Touch**

1. `arx/src/core/Config.h` — add the field to the right section struct.
2. `arx/src/core/Config.cpp`, `Default` namespace — its default value.
3. `arx/src/core/Config.cpp`, `Key` namespace — the string used as the `cfg.ini` key.
4. `arx/src/core/Config.cpp`, `Config::save` — write it, inside the matching section block.
5. `arx/src/core/Config.cpp`, `Config::init` — read it, and clamp it to its valid range.

**Then** build, run, change the value in the menu or by hand in `cfg.ini`, quit through the menu, and confirm the file kept it. A setting that survives a restart is a setting that is wired up.

**Watch out** if the setting indexes an array in the renderer, its clamp and that array's length are the same fact in two files — [INV-11](INVARIANTS.md#inv-11).

**Re-derive**

```
grep -n "rtao" arx/src/core/Config.h arx/src/core/Config.cpp
```

Five hits, one per step. That is the shape you are copying.

## <a id="add-a-menu-widget"></a>Add a menu widget

**Touch**

1. `arx/src/gui/widget/Widget.h` — if you need a new page, add its enum value.
2. `arx/src/gui/MainMenu.cpp` — the page class. For ray tracing this is `RayTracingOptionsMenuPage`, which builds its rows from a small local helper rather than one widget at a time.
3. The same file — register the page where the other pages are added to the menu window.
4. The same file — if the page needs an entry point, add a text widget on the parent page that targets it.

The widget pattern is: construct it, assign its change callback, add its entries, set its current value, then add it to the page. The callback writes the setting and saves the configuration; it does not talk to the renderer.

**Then** build, open the page, change the value, back out, quit through the menu, and confirm `cfg.ini` kept it.

**Watch out** a page that exposes hardware-dependent features should ask the renderer whether they are available and show a message instead of dead controls when they are not. `supportsRayTracing` is the existing example.

**Re-derive**

```
grep -n "class RayTracingOptionsMenuPage" -A 60 arx/src/gui/MainMenu.cpp
```

## <a id="add-localised-text"></a>Add localised text

The convention surprises people: **the INI section name is the lookup key**, and the text is a `string` entry inside it.

**Touch**

1. `arx/data/core/localisation/xtext_default_001_arxlibertatis.ini` — add the section and its English text. This file is the fallback for every language, so **this step is not optional**: a key missing here renders as the raw key name for anyone whose language file lacks it.
2. `arx/data/core/localisation/xtext_<language>_001_arxlibertatis.ini` — the translation, for each language you can supply.
3. Your code — look the key up by name.

Keys are lower case with underscores, grouped by where they appear, for example `system_menus_options_raytracing_*`.

**Then** run in English and confirm the text appears; run in another language and confirm you get either the translation or the English fallback, never the raw key.

**Watch out** the default file is loaded first and the language file is merged over it, so a language file only needs the keys it actually translates. Getting this backwards produces raw keys in every language but English.

**Re-derive**

```
grep -n "system_menus_options_raytracing" arx/data/core/localisation/xtext_default_001_arxlibertatis.ini
grep -rn "system_menus_options_raytracing" arx/src/gui/MainMenu.cpp
```

## <a id="add-a-renderer-entry-point"></a>Add a renderer entry point

**Touch**

1. `arx/src/graphics/Renderer.h` — declare the method on the base class **with a default body that does nothing**, so no other backend has to change.
2. Your backend — override it.
3. The caller — call it through the global renderer pointer.

The no-op default is the rule here, not a style preference. There are three backends and two of them are not the point of the fork; a pure virtual would force edits in code nobody is testing.

**Then** build, and confirm the other backends still compile. If the entry point does visible work, check that switching to Direct3D 9 still renders.

**Watch out** capability queries follow the same shape: default to reporting the feature as absent, and override to report it present.

**Re-derive**

```
grep -n "applyWorldRayEffects\|supportsRayTracing" arx/src/graphics/Renderer.h
```

## <a id="add-a-command-line-option"></a>Add a command-line option

**Touch** one file, wherever the option belongs. Write a free function that does the work, then register it immediately below with the registration macro. Options taking an argument use the argument-taking macro and name the argument.

**Then** run the game with `--help` and confirm the option is listed.

**Re-derive**

```
grep -n "ARX_PROGRAM_OPTION" arx/src/core/ArxGame.cpp
```

## <a id="change-non-ray-traced-lighting"></a>Change non-ray-traced lighting

**This is not where ray traced shadows come from.** This is the engine's own lighting, which computes vertex and polygon colour. The ray tracer reads the same light list but is otherwise independent, so changing one does not change the other, and a bug in one can look exactly like a bug in the other.

**Touch** `arx/src/scene/Light.h` and `arx/src/scene/Light.cpp`. Static level lights and dynamic lights live in separate containers. The per-frame update that copies lit static lights into dynamic ones, and the culling pass that builds the visible set, both run before the scene is drawn.

**Then** build and compare a lit room against the Direct3D 9 backend, which is a useful second implementation for correctness questions.

**Watch out** the ray tracer chooses its own subset of these lights each frame and fades membership changes. If shadows change when you did not expect them to, check the light selection diagnostics before suspecting the lighting itself — [INV-10](INVARIANTS.md#inv-10).

**Re-derive**

```
grep -n "g_staticLights\|g_dynamicLights" arx/src/scene/Light.h
```

## <a id="feed-new-geometry-to-the-ray-tracer"></a>Feed new geometry to the ray tracer

**Touch**

1. `arx/src/graphics/d3d12/D3D12Renderer.cpp` — gather it. Cached level geometry goes through the room path; anything that moves goes through the per-frame path. Decide which, because the cached path is only rebuilt on room changes and long moves.
2. `arx/src/graphics/dxr/D3D12Rtao.cpp` — if it is a genuinely new category rather than more of an existing one, it needs its own bottom-level structure, its own instance, its own vertex buffer, and a branch in the hit shader that maps the instance back to the buffer.

**Then** build, run, and check the log: the room caster line reports how much was gathered and how much cutout was excluded, and the triangle cap line tells you if you blew a budget.

**Watch out** three things, in order of how quietly they fail:

- A new category is [INV-08](INVARIANTS.md#inv-08), and its vertex buffer is [INV-02](INVARIANTS.md#inv-02). Both fail silently.
- Cutout geometry must not become a caster — [INV-07](INVARIANTS.md#inv-07).
- Budgets drop triangles arbitrarily once hit. See [TUNING.md](TUNING.md#triangle-budgets).

**Re-derive**

```
grep -n "addRoom\|addWorld" arx/src/graphics/d3d12/D3D12Renderer.cpp
```

## <a id="add-a-shader-resource"></a>Add a shader resource

The single most error-prone change in the ray tracing code, because every part of it fails silently. Read [INV-02](INVARIANTS.md#inv-02) first.

**Touch**, in `arx/src/graphics/dxr/D3D12Rtao.cpp`, all of these or none:

1. Create the resource, and release it wherever the others are released.
2. Create its view at the next free descriptor slot in `updateDescriptors`.
3. Raise the count constant for the range it belongs to.
4. Raise every constant for the ranges that come after it, by the same amount.
5. Raise the root signature's range size for that range.
6. Declare it in the shader at the register matching its offset within its range.

**Then** build and run **with the Direct3D 12 debug layer enabled**. There is no in-game symptom that tells you the slots are wrong.

**Watch out** [INV-02](INVARIANTS.md#inv-02). Also make sure the resource is in the right state before the pass reads it; the existing code transitions each one explicitly. Do **not** add DXR root constants to pass a new float — the signature is at the 64-DWORD cap ([INV-13](INVARIANTS.md#inv-13)); put it in `ViewParams`.

**Re-derive**

```
grep -n "slot(" arx/src/graphics/dxr/D3D12Rtao.cpp
grep -n ": register(" arx/src/graphics/dxr/D3D12Rtao.cpp
```

Read those two lists side by side. The order must match.

## <a id="add-a-diagnostic"></a>Add a diagnostic

**Touch** the code path itself, using the logging macros. Anything that can happen every frame **must be rate limited** — a log line per frame makes the file useless for the thing it exists for.

The existing ray tracing diagnostics rate limit by only logging when something changes, plus a counter, which is far more useful than a periodic dump: a line that appears while you are standing still is itself the bug report.

**Then** run, reproduce the situation, and read `runtime/user/arx.log`.

**Watch out** a new diagnostic is only worth adding if you can say what it being present or frequent would mean. Add its row to the diagnostics table in [INVARIANTS.md](INVARIANTS.md#diagnostics), including the "bad if" clause. A log line nobody can interpret is noise.

**Re-derive**

```
grep -n "DXR lights set changed\|DXR entity movers" arx/src/graphics/d3d12/D3D12Renderer.cpp
```

## <a id="build-and-run"></a>Build and run

**Configure** once:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

**Build** the game:

```
cmake --build build --config RelWithDebInfo --target arx --parallel
```

CMake may not be on your `PATH`. If it is not, call it by its full path from the Visual Studio installation rather than adding it globally.

**Run** through the launch scripts, which locate the Arx Fatalis installation from the Steam and GOG registry entries and point the game at the repository's own user directory:

```
scripts/run-d3d12.ps1
scripts/run-d3d9.ps1
```

Both scripts accept `-Config`, `-DataDir` and `-LoadLevel`. Other flags (`--loadslot`, `--user-dir`, …) require invoking `arx.exe` directly.

**Quit through the menu.** It shuts down cleanly and finishes the log.

**Read the log** at `runtime/user/arx.log`. It is overwritten each run, and it is gitignored, so copy anything you want to keep.

**Style check**, which runs the upstream linter:

```
cmake --build build --target style
```

## <a id="before-you-open-a-pull-request"></a>Before you open a pull request

**Nothing will catch a regression for you.** Upstream continuous integration lives in `arx/.github/workflows/ci.yml`, triggers on a branch this fork does not use, and has only Linux and macOS jobs. The Windows, Direct3D 12 and ray tracing surface — that is, everything this fork is — has no automated coverage whatsoever.

So the evidence in your pull request is the only evidence that exists. Include:

1. **What you changed and why**, in the commit message, in prose.
2. **The log lines that prove it.** Quote the relevant lines from `runtime/user/arx.log`. For a rendering change, the lines proving the feature initialised and ran at the level you expected.
3. **What you actually tested**: which level, which settings, standing still and moving. "Builds clean" is not a test of a renderer.
4. **What you did not test.** An honest gap is worth more than an implied guarantee.

Before pushing:

- `cmake --build build --config RelWithDebInfo --target arx --parallel` succeeds.
- The game runs and quits through the menu.
- No absolute paths, user directories or anything under `runtime/` is in the diff — see `docs/CONTRIBUTING.md`.
- If you changed anything this guide describes, `scripts/Check-MaintenanceDocs.ps1` still passes.
