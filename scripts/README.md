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

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run-d3d12.ps1
powershell -ExecutionPolicy Bypass -File scripts/run-d3d9.ps1
```

Optional arguments: `-DataDir '<path to Arx Fatalis>'`, `-LoadLevel <n>`, `-Config RelWithDebInfo`.

`ARX_RENDERER` is set for that process only. It does not rewrite `cfg.ini`. Change the saved API in **Options → Video** (restart required).
