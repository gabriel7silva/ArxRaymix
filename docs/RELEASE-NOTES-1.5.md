# Arx Raymix 1.5

Released 2026-10-13.

Ray tracing no longer needs an RTX card: any GPU with a Direct3D 12 driver can now turn it on. This release also adds volumetric fog and an OpenGL option, and fixes two Direct3D 9 bugs.

You need your own copy of Arx Fatalis (Steam or GOG). Nothing else. If you already have 1.4, `ArxRaymix.exe` offers this update when it starts. New install: run `ArxRaymix-v1.5-setup.exe`, or extract the zip to its own folder and run `ArxRaymix.exe`.

Arx Raymix is still **not digitally signed**: Windows SmartScreen shows "Windows protected your PC" the first time. Click **More info → Run anyway**, and compare the download with its `.sha256` if you want to be sure. Full guide: [INSTALL.md](https://github.com/gabriel7silva/ArxRaymix/blob/main/docs/INSTALL.md).

## New in 1.5

- **Ray tracing on GPUs without DXR.** On a GPU without hardware ray tracing (older GeForce GTX, AMD Radeon RX 5000 and older, older Intel), the game now traces the same effects in software: ambient occlusion, shadows, indirect light, reflections and volumetric fog. It uses the same shaders, run as a compute shader over the game's own acceleration structure. Software rays are slower than RT cores, so each effect is held at Low there, and ray tracing starts Off: turn it on in **Options → Ray tracing** (a preset you already saved is kept). The game picks the mode by itself: GPUs with DXR (GeForce RTX, Radeon RX 6000 and newer, Intel Arc) keep hardware ray tracing with every effect up to High. `arx.log` says which mode runs ("Ray tracing mode: software"). Grates, roots and webs keep their shaped shadows in both modes.
- **Volumetric fog.** Light shafts in the fog, cut by walls, pillars and the rest of the level, because every step of the fog traces a shadow ray toward the light. It is in **Options → Ray tracing → Volumetric fog** (Off / Low / Medium / High), part of the presets, and switched by **F7** with the other effects. It is based on the haze of Parricidium's [ArxCoop](https://github.com/Parricidium/ArxCoop) (GPLv3).
- **OpenGL** as a third choice in **Options → Video → Graphics API** (DirectX 12 / DirectX 9 / OpenGL, applies on the next launch), or for one launch with the environment variable `ARX_RENDERER=opengl`. It is the Arx Libertatis renderer, for drivers whose Direct3D misbehaves. OpenGL has no ray tracing and no DLSS.

## Fixes

- **Direct3D 9: F10 screenshots and save-game thumbnails were empty.** They now capture the image, as on Direct3D 12.
- **Direct3D 9: `arx.log` filled up** with a "D3D9 TL frame" line every 15 frames for the whole session. That diagnostic now stops after its budget, as intended.

## Known issues

- Software ray tracing was tested by forcing it on an RTX 4050 Laptop (`ARX_DXR_SOFTWARE=1`); it has not been tested on a GPU without DXR yet. If it is slow on your GPU, set the ray tracing preset to Off or turn individual effects off, and please report your GPU and the F11 numbers.
- **Ray Reconstruction** is still experimental and stays off in every preset.
- Changing the graphics API (DirectX 12 / 9 / OpenGL) needs a restart.
- Portuguese voice acting is not included (Arkane's audio, not redistributable).

## Download

- Nexus Mods: <https://www.nexusmods.com/arxfatalis/mods/37>
- GitHub: <https://github.com/gabriel7silva/ArxRaymix/releases/tag/v1.5>
- `ArxRaymix-v1.5-setup.exe`: the Windows installer (recommended).
- `ArxRaymix-v1.5.zip`: the same files, to extract by hand.
- SHA-256: in the `.sha256` file next to each download on the release page.

Earlier changes: [release notes 1.4](RELEASE-NOTES-1.4.md).

Arx Raymix is GPLv3, built on Arx Libertatis. Arx Fatalis is © Arkane Studios / ZeniMax Media / Bethesda Softworks. NVIDIA DLSS / Streamline are under NVIDIA's license. Not affiliated with or endorsed by Arkane Studios, ZeniMax or NVIDIA.
