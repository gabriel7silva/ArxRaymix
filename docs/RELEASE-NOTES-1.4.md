# Arx Raymix 1.4

<!-- TODO Gabriel: release date. Built from main at 42e55fdb (#25) plus the v1.4 work still in progress (launcher, update notice, F7, performance overlay, language prompt). Re-check the "New in 1.4" list against what is merged before publishing. -->

The first public release of Arx Raymix: *Arx Fatalis* on the Arx Libertatis engine, with a Direct3D 12 renderer, DXR ray tracing, DLSS, DLSS Frame Generation and Reflex.

You need your own copy of Arx Fatalis (Steam or GOG). Nothing else: no patch, no other mod. Install: extract the zip to its own folder and run `ArxRaymix.exe`. Full guide: [INSTALL.md](INSTALL.md).

## New in 1.4

- **Auto-update.** `ArxRaymix.exe` checks the latest GitHub Release when it starts, asks "Update now?", checks the download's SHA-256, replaces only the mod's files (keeping a backup) and starts the game. It never touches `cfg.ini`, saves or the Arx Fatalis folder. <!-- TODO Gabriel: confirm once merged -->
- **"New version available" notice** in the main menu when you start `arx.exe` directly. It can be turned off. <!-- TODO Gabriel: confirm once merged; where to turn it off -->
- **F7** switches ray tracing on and off instantly, for comparisons and screenshots. A short notice shows the state; saved settings are not changed.
- **Performance overlay** in Options → Interface: Off, FPS, or FPS + GPU time.
- **Language prompt on first run** (*Language / Idioma*), English listed first. Speech follows the choice only where voice files exist.
- **Ray tracing preset picked on first run** from your video memory (Low under 5 GiB, High from 9 GiB, Medium in between), with DLSS Quality where supported. Any saved preset is left alone.
- The Ray Reconstruction option now warns that it costs several ms per frame (about 5 ms more than DLSS alone on an RTX 4050 Laptop). It stays off in every preset.
- **Bug reports** from the crash dialog point to the Arx Raymix issue tracker. <!-- TODO Gabriel: confirm once merged -->

## What Arx Raymix adds to Arx Libertatis

### Renderer

- **Direct3D 12** is the default renderer on Windows. If it cannot start, the game uses **Direct3D 9** for that launch.
- **Options → Video** chooses DirectX 12 or DirectX 9 for the next launch and shows which API the current session uses.
- `Arx Raymix - DirectX 9.bat` starts Direct3D 9 for one session without changing your settings.

### Ray tracing (DXR)

Rays are traced on top of the normal image, so switching ray tracing off leaves the full game renderer.

- **Ambient occlusion**: darker corners and crevices.
- **Shadows** from the level's own lights and your torch, with soft edges.
- **Indirect light**: one bounce of coloured light.
- **Reflections** on water and metal, showing the real textures of what they reflect.
- **Contact shadows**, **debris** (small props as shadow casters) and **transparency**: grates, roots and webs cast shaped shadows instead of solid blocks.
- Each effect is Off / Low / Medium / High; presets Off / Low / Medium / High / Custom; **Ray tracing distance** Low / Medium / High / Ultra. All apply immediately.
- Results are denoised and kept steady between frames, so they do not shimmer while you stand or walk.

### NVIDIA DLSS

- **DLSS Super Resolution**: DLAA, Quality, Balanced, Performance, Ultra Performance. Below 1440p, Ultra Performance renders as Performance.
- **DLSS Frame Generation** (2×) on RTX 40 series and newer, with **Reflex** low latency on while it runs. The interface is not interpolated, so the HUD does not ghost.
- **DLSS Ray Reconstruction**, experimental.

### Other

- **Render distance** (Options → Render) sets how far you can see, fading into each zone's fog.
- **Brazilian Portuguese text**: menus and in-game text, chosen in Options → Language settings. English stays the default. Voices stay in English; the Portuguese voice files are not included.
- **F11 overlay** shows FPS, the DLSS mode and GPU time split into ray tracing (build / trace) and upscaling.

## Fixes and performance since development began

- Fixed a freeze with ray tracing on, and several image regressions. (#18)
- Fixed geometry corruption when a frame uploaded more vertices than the per-frame buffer held. (#21)
- Fixed textures being freed while the GPU was still using them. (#21)
- Fixed ray traced shadows flickering and dropping out while turning the camera with DLSS on: DLSS, Ray Reconstruction and Frame Generation now get real camera motion. (#21)
- Your torch can no longer be pushed out of the shadow lights in levels with many lights, and lights in connected rooms are no longer missed at Ultra distance. (#21)
- Shadows no longer change with where you look: the torch light no longer swings with the mouse, and floors seen at a shallow angle keep their shadows. (#22)
- Moving characters' shadows are cheaper to update (acceleration structures are refitted instead of rebuilt every frame). (#23)
- The GPU starts on each frame's raster work earlier, instead of waiting idle before the ray tracing pass. (#24)
- Frame Generation no longer turns itself off; render and ray tracing distance clip correctly. (#4)
- Antialiasing preference is kept while DLSS is on. (#15)
- Fog culling is back to the original range and honours each zone's far clip. (#14)
- A cinematic with a missing image no longer leaves its resources behind. (#13)
- Brazilian Portuguese: missing lines filled in, and untranslated lines fall back to English. (#12, #23, #24)

## Known issues

- **Ray Reconstruction** is experimental. On some GPUs (seen on an RTX 4050 Laptop) NVIDIA's feature cannot start; the option stays On and the built-in denoiser keeps running.
- Changing the graphics API (DirectX 12 / 9) needs a restart.
- Portuguese voice acting is not included (Arkane's audio, not redistributable).
- <!-- TODO Gabriel: add anything found in release testing -->

## Download

- Nexus Mods: [NEXUS_URL] <!-- TODO Gabriel -->
- GitHub: <https://github.com/gabriel7silva/ArxRaymix/releases/tag/v1.4> <!-- TODO Gabriel: confirm the tag name -->
- SHA-256 of `ArxRaymix-1.4.zip`: `<SHA256>` <!-- TODO Gabriel: paste the hash printed by scripts/package.ps1 -->

Arx Raymix is GPLv3, built on Arx Libertatis. Arx Fatalis is © Arkane Studios / ZeniMax Media / Bethesda Softworks. NVIDIA DLSS / Streamline are under NVIDIA's license. Not affiliated with or endorsed by Arkane Studios, ZeniMax or NVIDIA.
