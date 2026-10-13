# Launch scripts

English is the default. Portuguese: [docs/pt-BR/README.md](../docs/pt-BR/README.md).

These scripts start a built `arx.exe` against your own Arx Fatalis install. They never embed a user profile, drive letter, or machine path.

| Script | Graphics API |
|---|---|
| `run-d3d12.ps1` / `run-dx12.ps1` | Direct3D 12 / DirectX 12 (default renderer) |
| `run-d3d9.ps1` / `run-dx9.ps1` | Direct3D 9 / DirectX 9 (fallback) |
| `Find-ArxFatalis.ps1` | Shared Steam / GOG lookup (dot-sourced, not launched) |
| `fetch-streamline.ps1` | Fetch the NVIDIA Streamline SDK once into `arx/third_party/streamline/` |
| `Check-MaintenanceDocs.ps1` | Fail if a grep in `docs/maintenance/` matches nothing |
| `import-speech-portugues.ps1` | Copy PT-BR speech WAVs into `arx/data/core/speech/portugues/` |
| `check-speech-portugues.py` | Report broken or missing PT-BR speech WAVs and subtitles too long for their line (`--english-dir` also compares with the English voices) |
| `check-shaders.py` | Compile the HLSL embedded in the D3D12 / DXR sources with the Windows SDK's DXC / FXC (CI runs it) |
| `package.ps1` | Zip a built `arx.exe`, its DLLs and `arx/data/core` into `dist\ArxRaymix-<version>.zip` for players |
| `build-installer.ps1` | Compile `installer/ArxRaymix.nsi` (NSIS 3) from that staged folder into `dist\ArxRaymix-<version>-setup.exe` |
| `sweep-plausible-levels.ps1` | Load each level for a few seconds (`--benchmark`) and score Pdxr-5 / F5 from the log; it reports F4 as undecidable |

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-d3d12.ps1
powershell -ExecutionPolicy Bypass -File scripts/run-d3d9.ps1
```

Optional arguments: `-DataDir '<path to Arx Fatalis>'`, `-LoadLevel <n>`, `-Config RelWithDebInfo`.

`-DxrDebug <1-4>` replaces the ray traced reflection with a raw value, because a ray pass cannot
print: 1 which instance was hit (room red, entity green, water blue, player yellow), 2 the hit
normal, 3 the hit distance, 4 the texture sampled at the hit. Look at water or metal.

`ARX_DXR_FIRSTHIT=1` puts reflection rays back on any-hit traversal instead of nearest-hit.
Nearest is correct and costs a full traversal per ray; this is the escape hatch if that is too
slow on a given machine.

`ARX_DXR_NO_TEXREFL=1` stops off-screen reflected hits from using their own texture and goes
back to the light-only approximation.

`ARX_DXR_NO_REFIT=1` rebuilds the entity and player acceleration structures from scratch every
frame instead of refitting them; use it to rule refitting out when a moving shadow looks wrong.

`ARX_DXR_STATIC_NOISE=1` keeps the penumbra / AO / GI noise fixed in screen space instead of
stepping it every frame under the temporal history.

`ARX_D3D12_NO_EARLY_SUBMIT=1` keeps each frame in one command list. By default the raster work
is submitted before the ray tracing pass waits for the previous frame, so the GPU is not idle
during that wait; use the switch to rule the split out if something renders wrong.

`ARX_SL_NO_VERIFY=1` loads an `sl.interposer.dll` that is not signed by NVIDIA (a development
build of the Streamline SDK). Release DLLs are signed and need nothing.

Package for players (needs a RelWithDebInfo build; run `fetch-streamline.ps1` first to include
DLSS / Frame Generation):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Version v1.4
```

The zip holds `arx.exe`, the launcher `ArxRaymix.exe`, the DLLs CMake staged next to it, `data\`
(from `arx/data/core`), the licence, a `README.txt`, `Arx Raymix - DirectX 9.bat` and
`package.txt` (the version, then every file in the zip). `<zip>.sha256` is written next to it. The
zip never holds game data, PDBs (unless `-IncludePdb`), `data.dirs` or the imported PT-BR speech
WAVs, which are game audio.

### Releases and updates

1. Set the first line of `arx/VERSION` to the new version (`Arx Raymix 1.5`) and merge it.
2. Tag that commit and push the tag: `git tag v1.5` then `git push origin v1.5`.
3. `.github/workflows/release.yml` builds, runs `package.ps1 -Version v1.5` and
   `build-installer.ps1 -Version v1.5`, and publishes a GitHub Release with
   `ArxRaymix-v1.5-setup.exe`, `ArxRaymix-v1.5.zip` and a `.sha256` for each. It stops if the tag
   and `arx/VERSION` disagree. Upload the same zip (and, if you like, the setup) to Nexus Mods by hand.

The installer (`installer/ArxRaymix.nsi`) installs per user into `%LOCALAPPDATA%\Programs\ArxRaymix`
without an administrator prompt, so `ArxRaymix.exe` can update that folder later. It adds Start menu
shortcuts, an optional desktop shortcut and a Settings → Apps entry, whose version the launcher keeps
current after each update. The uninstaller deletes the files `package.txt` lists, `backup\`,
`update\` and the launcher's own files, and never `cfg.ini`, saves or the Arx Fatalis folder.
Locally: `winget install NSIS.NSIS`, then `scripts/build-installer.ps1 -Version <tag>` after
`package.ps1`.

Players who start `ArxRaymix.exe` are offered the new version: it downloads the zip, checks the
SHA-256, moves the files listed in the old `package.txt` to `backup\` and puts the new ones in
place. `ArxRaymix.exe --rollback` restores `backup\`; `ArxRaymix.exe --install <zip>` installs a
zip downloaded by hand (from Nexus, say), checked against `<zip>.sha256` when it is next to it;
`--no-update` skips the check. The launcher writes `launcher.log` next to itself.

The game itself asks the same endpoint once per launch (skipped when the launcher already did) and
shows "New version available" in the main menu. **Options → Interface → Check for updates**
(`check_for_updates` in `[misc]`) and `ARX_NO_UPDATE_CHECK=1` turn it off. `ARX_UPDATE_API=<https
URL>` points both at another repository's `releases/latest`, for testing.

Plausible load sweep (loads levels, does not play them):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1 -DryRun
powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1 -Levels 1
powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1
```

Report: `runtime/user-sweep/sweep-report.txt`. Extra flags (`--benchmark`, `--skipcinematic`) go to `arx.exe` from that script, not from `run-d3d12.ps1`.

`ARX_RENDERER` is set for that process only. It does not rewrite `cfg.ini`. Change the saved API in **Options → Video** (restart required).
