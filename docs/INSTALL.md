# Installing Arx Raymix

For players. Portuguese: [README.pt-BR.md](README.pt-BR.md). Building from source: [BUILDING.md](BUILDING.md).

## Before you start

You need:

- Windows 10 or 11, 64-bit.
- Your own copy of **Arx Fatalis** from **Steam** or **GOG**, installed.
- The Arx Raymix zip from [Nexus Mods][nexus] or [GitHub Releases][releases].

You do not need anything else: no game patch, no Arx Libertatis install, no other mod. Arx Raymix never changes the Arx Fatalis folder; it only reads the game data from it.

GPU requirements are in the [README](../README.md#requirements). In short: any Direct3D 12 GPU runs the game, ray tracing needs DXR support, DLSS needs an NVIDIA RTX GPU, and Frame Generation needs an RTX 40 series GPU or newer.

## What is in the zip

| File | What it is |
|---|---|
| `ArxRaymix.exe` | Launcher: checks for updates, then starts `arx.exe` (new in v1.4) |
| `arx.exe` | The game |
| `*.dll` | SDL2, OpenAL, the DirectX Shader Compiler, and the NVIDIA Streamline / DLSS DLLs |
| `data\` | Arx Raymix's own text (English and Brazilian Portuguese menus), fonts |
| `Arx Raymix - DirectX 9.bat` | Starts the Direct3D 9 fallback for one session |
| `README.txt`, `LICENSE`, `AUTHORS` | Short instructions, the GPLv3, the Arx Libertatis authors |

No Arx Fatalis game data is in the zip.

## Install

### 1. Install Arx Fatalis

**Steam**: install *Arx Fatalis* from your library. To find its folder: right-click the game → *Manage* → *Browse local files*.

**GOG**: install *Arx Fatalis* with GOG Galaxy or the offline installer. In GOG Galaxy, the folder is under the game's settings button → *Manage installation* → *Show folder*.

The folder you want is the one that contains `data.pak`. Note it down in case you need it in step 4.

Running the original game once first is recommended (the zip's `README.txt` asks for it too).

### 2. Install Arx Raymix

**With the installer (recommended).** Run `ArxRaymix-vX.Y-setup.exe` from the [GitHub release][releases]. It installs for your Windows user only, without an administrator prompt, into `%LOCALAPPDATA%\Programs\ArxRaymix` (you can choose another folder), adds *Arx Raymix*, *Arx Raymix (DirectX 9)* and *Uninstall* to the Start menu, optionally a desktop shortcut, and an entry in **Settings → Apps**. It refuses the Arx Fatalis folder. The setup is in English or Brazilian Portuguese, following your Windows language.

**With the zip.** Extract the zip to a folder of its own, for example `C:\Games\ArxRaymix`.

- Do **not** extract it into the Arx Fatalis folder.
- Avoid `C:\Program Files`: the launcher needs to write to the mod folder when it updates.

### 3. Run

Double-click `ArxRaymix.exe`. It checks for a new version and starts the game. You can also start `arx.exe` directly.

**Windows SmartScreen.** Arx Raymix is not digitally signed yet, so the first time you run the setup or `ArxRaymix.exe`, Windows may show *"Windows protected your PC"*. Click **More info → Run anyway**. Every file is built by GitHub Actions from the public source; to be sure, compare the file's SHA-256 (`Get-FileHash <file>` in PowerShell) with the `.sha256` on the release page. Code signing is planned and will remove this warning.

### 4. If the game data is not found

`arx.exe` looks for Arx Fatalis in every Steam library folder and in the GOG install recorded in the registry, so a normal Steam or GOG install is found on its own. If the game still reports that it cannot find the Arx Fatalis data (a copied or unusual install), tell it where the folder with `data.pak` is. Create a shortcut to `arx.exe`, open its *Properties*, and add `--data-dir` to the *Target* field:

```text
"C:\Games\ArxRaymix\arx.exe" --data-dir "C:\Program Files (x86)\Steam\steamapps\common\Arx Fatalis"
```

Use your own paths. Start the game from that shortcut.

As an alternative, the engine also reads the data folder from the registry value `DataDir` under `HKEY_CURRENT_USER\Software\ArxLibertatis`. Arx Libertatis uses the same value.


## First run

1. **Language.** The first time, a *Language / Idioma* screen lists the available text languages, English first. Pick one. You can change it later in **Options → Language settings**. Brazilian Portuguese covers the menus and the in-game text; any line without a translation shows in English. Voices stay in English: the Portuguese voice files are Arkane's audio and are not included.
2. **Graphics.** With a DXR GPU, the game picks a ray tracing preset from your video memory: Low under 5 GiB, High from 9 GiB, Medium in between. Where your GPU supports it, DLSS Quality is turned on too. Without DXR nothing is changed.
3. **Play.** New game or load a save. Saves from Arx Libertatis show up too, because both use the same user folder.

## Settings worth knowing

| Where | What |
|---|---|
| Options → Ray tracing | **Preset** Off / Low / Medium / High / Custom, each effect separately, **Ray tracing distance**, **DLSS Ray Reconstruction (Experimental)**. Applies immediately |
| Options → Video | **Upscaling** (DLSS mode), **Frame Generation**, and the graphics API (DirectX 12 / 9, applies on the next launch) |
| Options → Render | **Render distance**; antialiasing is locked off while DLSS is on |
| Options → Interface | **Performance overlay**: Off / FPS / FPS + GPU time (new in v1.4) |
| Options → Language settings | Text and audio language |

| Key | Action |
|---|---|
| F7 | Ray tracing on / off for this session; saved settings are not changed (new in v1.4) |
| F10 | Screenshot, saved as `snapshot_<n>.bmp` in your user folder |
| F11 | Cycle the performance / debug overlay |

If the game runs slowly, in this order: lower the DLSS mode (Quality → Balanced → Performance), lower the ray tracing preset, lower **Ray tracing distance**, turn Ray Reconstruction off. Use the GPU time in the overlay to compare, not only FPS: Frame Generation and V-Sync hide changes in FPS.

## Updating

### Automatic (v1.4 and later)

When `ArxRaymix.exe` starts, it checks the latest release on GitHub. If there is a newer version:

1. It asks whether to update: **Update now**, **Not now**, or **Skip this version**.
2. On *Update now*, it downloads the release zip with a progress bar and checks its SHA-256. A download that does not match is refused and nothing changes.
3. It replaces only the files listed in `package.txt` and moves the old ones to the `backup` folder next to it. `ArxRaymix.exe --rollback` puts the previous version back.
4. It starts `arx.exe`.

It never touches `cfg.ini`, your saves, or the Arx Fatalis folder. Without an internet connection, or if anything fails, it starts the installed version as usual. `ArxRaymix.exe --no-update` skips the check.

Downloaded the new zip from Nexus Mods instead? `ArxRaymix.exe --install <zip>` installs it the same way, with the same backup. When the `.sha256` file from the GitHub release sits next to the zip, it is checked too.

If you start `arx.exe` directly, the main menu shows a **new version available** notice instead. Clicking it opens the release page. Turn it off in **Options → Interface → Check for updates** (`check_for_updates` in `cfg.ini`).

### Manual

1. Download the new zip.
2. Quit the game.
3. Extract the new zip over the old folder (replace files), or into a new folder and delete the old one.

Your settings and saves are in the user folder, not the mod folder, so they carry over.

## Uninstall

1. Installed with the setup: **Settings → Apps → Arx Raymix → Uninstall**, or *Uninstall Arx Raymix* in the Start menu. It removes the mod's files, its `backup` folder and the shortcuts. Installed from the zip: delete the Arx Raymix folder.
2. Optional: delete your user folder (`%USERPROFILE%\Saved Games\Arx Libertatis`, see below) to remove settings and **saves**. Arx Libertatis uses the same folder.
3. If you added the `DataDir` registry value in step 4 of the install, delete it as well.

Arx Fatalis itself was not modified, so there is nothing to restore.

## Troubleshooting

### Where are my files?

| File | Location |
|---|---|
| Saves, `cfg.ini`, `arx.log`, screenshots | `%USERPROFILE%\Saved Games\Arx Libertatis`, or `Documents\My Games\Arx Libertatis` if that folder already exists |

Run `arx.exe --list-dirs` from a command prompt in the mod folder to print the folders actually in use. To keep Arx Raymix's settings and saves separate from Arx Libertatis, start it with `--user-dir "<folder>"`.

### Reporting a bug

Attach `arx.log`. It is rewritten on every launch, so copy it right after the problem. Also give your GPU, driver version, Windows version and the settings you used (preset, DLSS mode, Frame Generation). Report on [GitHub Issues](https://github.com/gabriel7silva/ArxRaymix/issues) <!-- TODO Gabriel: or the Nexus Bugs tab --> .

### The game starts on DirectX 9

The D3D12 device could not be created, so the game fell back to Direct3D 9 for that launch. `arx.log` says `D3D12 createDevice failed, falling back to D3D9`. Update your GPU driver. Also check **Options → Video**: if the saved API is DirectX 9, switch it back to DirectX 12 and restart.

### The Ray tracing page is grayed out

It says *Requires DirectX 12 and a GPU with DXR*. Either the game runs on Direct3D 9, or the GPU / driver reports no DirectX Raytracing support. `arx.log` shows `RaytracingTier=…` or `NOT_SUPPORTED`.

### DLSS or Frame Generation is grayed out

DLSS needs an NVIDIA RTX GPU and a current driver; Frame Generation needs an RTX 40 series GPU or newer. The menu shows the reason. The NVIDIA DLLs must sit next to `arx.exe`; do not delete them.

### DLSS Ultra Performance looks like Performance

Below 1440p, Ultra Performance is rendered as Performance, because the lower internal resolution is unusable at 1080p. At 1440p and 4K it is real Ultra Performance.

### Ray Reconstruction does not seem to do anything

It is experimental. On some GPUs (seen on an RTX 4050 Laptop) NVIDIA's feature cannot be created; the setting then stays On but the built-in denoiser keeps running, and `arx.log` says so once. It also costs noticeably more GPU time than DLSS alone (about 5 ms per frame on an RTX 4050 Laptop).

### I want to try Direct3D 9 once

Run `Arx Raymix - DirectX 9.bat`. It starts the D3D9 fallback for that session only and does not change `cfg.ini`.

### The game is in the wrong language

**Options → Language settings**. To see the first-run language screen again, delete the `[language]` section from `cfg.ini`.

## FAQ

**Do I need patch 1.21 or any other download?**
No. The Steam and GOG releases are enough.

**Does it change my Arx Fatalis install?**
No. It reads the game data and writes only to its own folder and the user folder.

**Can I keep playing my Arx Libertatis saves?**
Arx Raymix uses the same user folder as Arx Libertatis, so those saves appear in the load menu. <!-- TODO Gabriel: confirm you have tested loading an Arx Libertatis save -->

**Does it work on AMD or Intel GPUs?**
The Direct3D 12 renderer and ray tracing use standard DirectX, so any GPU with Direct3D 12 and DXR support can run them. DLSS, Frame Generation, Ray Reconstruction and Reflex are NVIDIA only. <!-- TODO Gabriel: say which non-NVIDIA GPUs have actually been tested, if any -->

**Is there Portuguese voice acting?**
Not in the download. The Portuguese voice files are Arkane's audio and cannot be redistributed. Text is translated; voices are English.

**Linux, macOS, Steam Deck?**
Arx Raymix is a Windows build. For other systems, use [Arx Libertatis](https://arx-libertatis.org/).

**Is it free? Can I see the source?**
Yes. The engine is GPLv3 and the source is at <https://github.com/gabriel7silva/ArxRaymix>.

[nexus]: NEXUS_URL "TODO Gabriel: Nexus page URL"
[releases]: https://github.com/gabriel7silva/ArxRaymix/releases
