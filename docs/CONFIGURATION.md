# Runtime configuration

Settings the Windows remaster actually reads. Local `cfg.ini` files, logs, and user directories are not committed.

## Graphics API

The renderer is chosen **once**, when the window is created.

| Source (highest first) | Values |
|---|---|
| Environment `ARX_RENDERER` | `d3d12`, `dx12`, `Direct3D 12`, `DirectX 12`, `d3d9`, `dx9`, `Direct3D 9`, `DirectX 9` |
| `cfg.ini` `[video] renderer=` | `auto` (Direct3D 12), `Direct3D 12` / `DirectX 12`, `Direct3D 9` / `DirectX 9` / `D3D9` |
| In-game **Options → Video** | DirectX 12 or DirectX 9. Saved immediately. Applied on the next launch |

The Video Options page also shows the API of the **current** session, for example `Graphics API: DirectX 12`. That line does not change until you restart.

**Apply** on the Video page saves resolution and fullscreen and may resize the window. It never switches Direct3D 9 ↔ Direct3D 12 in the running process.

Launch scripts set `ARX_RENDERER` for that process only. They do not rewrite `cfg.ini`.

## Directories

| Path | Purpose |
|---|---|
| `--data-dir` | Arx Fatalis install (`data.pak`). Detected from Steam / GOG when using the scripts |
| `--user-dir` | Saves, `cfg.ini`, `arx.log`. The scripts use `runtime/user/` under the repo |
| `data.dirs` next to `arx.exe` | Generated at build time so overlay localisation is found. Do not commit it |

`--list-dirs` prints the data directories in priority order.

## Other video keys

The rest of `[video]` is the Arx Libertatis set: resolution, fullscreen, vsync, FOV, LOD, fog, antialiasing, anisotropic filtering, and so on. See the in-game Video and Render pages.

## What not to commit

- `runtime/user/cfg.ini`, `runtime/user/arx.log`, save games
- `data.dirs` (absolute build path)
- Machine paths, user profile folders, personal GPU launchers
