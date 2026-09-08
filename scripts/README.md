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
| `sweep-plausible-levels.ps1` | Load each level for a few seconds (`--benchmark`) and score Pdxr-5 / F5 from the log; it reports F4 as undecidable |

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-d3d12.ps1
powershell -ExecutionPolicy Bypass -File scripts/run-d3d9.ps1
```

Optional arguments: `-DataDir '<path to Arx Fatalis>'`, `-LoadLevel <n>`, `-Config RelWithDebInfo`.

`-DxrDebug <1-4>` replaces the ray traced reflection with a raw value, because a ray pass cannot
print: 1 which instance was hit (room red, entity green, water blue, player yellow), 2 the hit
normal, 3 the hit distance, 4 the texture sampled at the hit. Look at water or metal.

Plausible load sweep (loads levels, does not play them):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1 -DryRun
powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1 -Levels 1
powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1
```

Report: `runtime/user-sweep/sweep-report.txt`. Extra flags (`--benchmark`, `--skipcinematic`) go to `arx.exe` from that script, not from `run-d3d12.ps1`.

`ARX_RENDERER` is set for that process only. It does not rewrite `cfg.ini`. Change the saved API in **Options → Video** (restart required).
