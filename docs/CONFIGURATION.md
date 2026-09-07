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
| `data.dirs` next to `arx.exe` | Generated at build time so overlay localisation and `speech/portugues/` are found. Do not commit it |

`--list-dirs` prints the data directories in priority order.

## Render distance

`cfg.ini` `[video] fog=` is still the key (0–10). In this fork it is the **far plane**, not a fog-density slider. Options → Render → Render distance. Slider 0 is about one cell plus a fog wall (~1600 Arx units); 10 is the old max (~28000). World pixels fade into the zone fog colour from 40 % to 92 % of that distance. Fog follows `Renderer` state (`getFog()`); it must not depend on the DLSS `worldPass` flag. HUD stays unfogged.

## Ray tracing and Streamline

Applied immediately. Gray in the menu, and ignored from `cfg.ini`, when the hardware cannot run them.

| Key | Menu | Values |
|---|---|---|
| `dxr_preset` | Options → Ray tracing → Preset | 0 Off, 1 Low, 2 Medium, 3 High, 4 Custom |
| `rtao` | Ambient occlusion | 0–3 Off / Low / Medium / High |
| `dxr_shadows` | Direct lighting (DXR shadows) | 0–3 |
| `dxr_shadow_denoise` | Direct denoise | 0 Low, 1 High |
| `dxr_contact` | Contact shadows | 0 Off, 1 On |
| `dxr_gi` | Indirect lighting (one bounce) | 0–3 |
| `dxr_gi_denoise` | Indirect denoise | 0–2 |
| `dxr_reflections` | Metal reflections | 0–3 |
| `dxr_trans_reflections` | Water / transparent reflections | 0–3 |
| `dxr_transparency` | Cutout casters | 0 Off, 1 Low, 2 High |
| `dxr_debris` | Small props in the TLAS | 0 Off, 1 On |
| `dxr_distance` | Ray tracing distance | 0 Low, 1 Medium, 2 High, 3 Ultra |
| `dxr_dlss` | Options → Video → Upscaling / DLSS Mode | 0 Off, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance |
| `dxr_fg` | Options → Video → Frame Generation | 0 Off, 1 On (2×). Only this toggle may turn FG on or off |
| `dxr_rr` | Options → Ray tracing → Ray Reconstruction (experimental) | 0 Off, 1 On. Homemade denoise stays if NGX create fails |

RT presets do not write `dxr_dlss`, `dxr_fg`, or `dxr_distance`. They set `dxr_rr` to 0. `dxr_dlss` uses schema `dlss_schema=1` (old Auto / DLAA values migrate on load).

Indirect lighting is one analytic bounce. There is no bounce-count key and no path-tracing key. Phase 6 is archived.

## Other video keys

The rest of `[video]` is the Arx Libertatis set: resolution, fullscreen, vsync, FOV, LOD, antialiasing, anisotropic filtering, and so on. See the in-game Video and Render pages.

## What not to commit

- `runtime/user/cfg.ini`, `runtime/user/arx.log`, save games
- `data.dirs` (absolute build path)
- Machine paths, user profile folders, personal GPU launchers
