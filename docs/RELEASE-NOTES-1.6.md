# Arx Raymix 1.6

Released 2026-10-17.

A maintenance release: a full code review of 1.5 and the fixes it called for, a frame-time graph in the F11 overlay, volumetric fog that stays where the level has fog, and a package without the self-updating launcher.

You need your own copy of Arx Fatalis (Steam or GOG). Nothing else. If you already have 1.4 or 1.5, `ArxRaymix.exe` offers this update when it starts. New install: run `ArxRaymix-v1.6-setup.exe`, or extract the zip to its own folder and run `ArxRaymix.exe`.

Arx Raymix is still **not digitally signed**: Windows SmartScreen shows "Windows protected your PC" the first time. Click **More info → Run anyway**, and compare the download with its `.sha256` if you want to be sure. Full guide: [INSTALL.md](https://github.com/gabriel7silva/ArxRaymix/blob/main/docs/INSTALL.md).

## New in 1.6

- **Frame-time graph in F11.** The performance overlay now draws CPU frame time (white), GPU time (green) and ray tracing time (orange), with 120 / 60 / 30 FPS guide lines and average / worst / current in the legend.
- **The new-version notice** in the main menu opens the Nexus Mods page.
- **Standalone package.** `ArxRaymix-v1.6-standalone.zip` and `ArxRaymix-v1.6-standalone-setup.exe` are the same game without `ArxRaymix.exe`: you run `arx.exe`, and nothing in the folder contacts the network. They do not update themselves; download the next version by hand.

## Fixes

- **Volumetric fog only where there is fog.** In 1.5 the fog had the same density everywhere: it filled every room (the prison cell at the start was hazed over), washed over the cutscenes and glowed over their black bars. It now exists only around the level's own fog emitters, the ones that already spawn the smoke, and is off in cutscenes. Rooms without fog cost nothing.
- **Ray tracing:** half-resolution indirect light and fog no longer read past the edge of the image on odd resolutions; a broken vertex can no longer crash the acceleration structure build; leaving a level frees all of its ray tracing data.
- **Direct3D 12:** textures freed during play are reused only once the GPU is done with them; F10 screenshots capture the frame actually shown, in the right colours; DLSS Ray Reconstruction gets its inputs before the ray pass changes them; resizing the window with DLSS on no longer risks a crash.
- **Direct3D 9:** resizing or alt-tabbing keeps the new window size and drawing area.
- **Menus and settings:** an unknown `ARX_RENDERER` value falls back to your configured API instead of Direct3D 9; when Direct3D 12 cannot start, the Video menu says so; the first-run ray tracing preset is no longer lost. Portuguese: the mouse wheel down binding has its label again.
- **Packages built on Windows:** the `data` folder (fonts, Portuguese text, translation overlays) was empty when the package was built with Windows PowerShell, so the game ran without its icons font and extra text. It is always complete now.
- **Launcher and installer:** the launcher's working folders are now `.arxraymix-backup` and `.arxraymix-update`, so your own `backup` or `update` folder is never touched; only one launcher runs at a time; `check_for_updates=0` or `ARX_NO_UPDATE_CHECK` also turns off the launcher's check; damaged or malformed update zips are rejected. Setup refuses to install into a folder holding something other than Arx Raymix, and the uninstaller cleans up every folder it created.

## Known issues

- Software ray tracing (GPUs without DXR) has still only been tested by forcing it on an RTX card. If it is slow on your GPU, set the ray tracing preset to Off and please report your GPU and the F11 numbers.
- **Ray Reconstruction** is still experimental and stays off in every preset.
- Changing the graphics API (DirectX 12 / 9 / OpenGL) needs a restart.
- Portuguese voice acting is not included (Arkane's audio, not redistributable).

## Download

- Nexus Mods: <https://www.nexusmods.com/arxfatalis/mods/37>
- GitHub: <https://github.com/gabriel7silva/ArxRaymix/releases/tag/v1.6>
- `ArxRaymix-v1.6-setup.exe`: the Windows installer (recommended).
- `ArxRaymix-v1.6.zip`: the same files, to extract by hand.
- `ArxRaymix-v1.6-standalone.zip` / `-standalone-setup.exe`: without the self-updating launcher.
- SHA-256: in the `.sha256` file next to each download on the release page.

Earlier changes: [release notes 1.5](RELEASE-NOTES-1.5.md), [release notes 1.4](RELEASE-NOTES-1.4.md).

Arx Raymix is GPLv3, built on Arx Libertatis. Arx Fatalis is © Arkane Studios / ZeniMax Media / Bethesda Softworks. NVIDIA DLSS / Streamline are under NVIDIA's license. Not affiliated with or endorsed by Arkane Studios, ZeniMax or NVIDIA.
