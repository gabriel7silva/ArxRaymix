# Runtime configuration

Every Remix option this project sets, transcribed from `RemixScene.cpp`. Values here are what the
code writes, not what a config file on disk happens to contain.

## Where settings come from, and who wins

Three channels, in increasing order of authority at any given moment:

1. **`rtx.conf`** in the working directory — read by the runtime at startup.
2. **`user.conf`**, next to it — written by the runtime whenever you change something in the
   developer menu (`Alt+X`), and read back on the next start. **It beats `rtx.conf`.**
3. **`SetConfigVariable`** from the game — applied after `dxvk_RegisterD3D9Device`, and again
   whenever a video option changes or the game crosses between menu and gameplay.

Two things worth knowing before debugging a setting:

- A run that comes up black with no explanation is usually `user.conf`. One accidental toggle that
  saved `rtx.enableRaytracing = False` persists across every later launch. Read the file on disk
  before suspecting the renderer.
- `SetConfigVariable` may only take effect at the end of a frame, and the log echoing a key back is
  not proof the runtime understood it — a deliberately misspelled key echoes the same way.

## Applied once, after device registration

`applyPreviewConfig()` — pinned so a stale `user.conf` cannot change them.

| Option | Value | Why |
|---|---|---|
| `rtx.enableRaytracing` | `True` | |
| `rtx.fallbackLightMode` | `0` | A camera-attached fallback light is visible in frame and wrecks autoexposure |
| `rtx.volumetrics.enable` | `False` | |
| `rtx.usePostFilter` | `False` | |
| `rtx.maxAnisotropySamples` | `16` | Floors are seen at a grazing angle, where isotropic mip selection dissolves the slabs |
| `rtx.ignoreAllVertexColorBakedLighting` | `True` | Arx's 2002 lightmaps must not multiply on top of traced light |
| `rtx.vertexColorIsBakedLighting` | `False` | |
| `rtx.vertexColorStrength` | `0` | |
| `rtx.autoExposure.enabled` | `True` | Overridden below when the Remix DLL is hooked |
| `rtx.tonemap.exposureBias` | `-0.6` | |
| `rtx.integrateIndirectMode` | `2` | Pins importance sampling; NRC failed to initialise on the development GPU |

Additionally, when the Remix DLL is hooked — that is, on the path the game actually runs:

| Option | Value | Why |
|---|---|---|
| `rtx.leftHandedCoordinateSystem` | `True` | D3D9 is left-handed and Arx is Y-down |
| `rtx.zUp` | `False` | |
| `rtx.sceneScale` | `1.0` | One Arx unit is about one centimetre |
| `rtx.legacyMaterial.roughnessConstant` | `0.35` | No USD replacements, so every surface uses this. `0.7` reads as matte plaster; `0.35` lets torchlight specular on stone |
| `rtx.legacyMaterial.metallicConstant` | `0.12` | |
| `rtx.legacyMaterial.emissiveIntensity` | `0.0` | |
| `rtx.camera.correctProjectionYFlip` | `False` | The engine already flips Y from NDC to screen; correcting again renders the world upside down |
| `rtx.showUI` | `2` | Developer menu forced on while the `Alt+X` window hook is unreliable |
| `rtx.showUICursor` | `True` | |

`rtx.showUI = 0` hides the menu, `1` is the simple view, `2` the advanced one. For a release build
this is the line to change.

## Applied from the in-game options

**Video options → Raymix** writes `config.video.remix` and calls `applyUserGfxConfig()` immediately,
so every toggle takes effect without a restart.

| Menu item | Config field | Effect |
|---|---|---|
| Path tracing | `pathTracing` | On the hooked path, `Off` sets `rtx.enableRaytracing = False`, which rasterises the game's own D3D9 draw calls rather than presenting black |
| Quality | `quality` | Low / Medium / High / Ultra → `rtx.pathMaxBounces` 1/2/4/6 and `rtx.di.initialSampleCount` 4/8/16/32 |
| DLSS | `dlss` | `rtx.upscalerType` `0` when on, `3` when off |
| Ray Reconstruction | `rayReconstruction` | `rtx.enableRayReconstruction` |
| Denoiser | `denoiser` | `rtx.useDenoiser` |
| Bloom | `bloom` | `rtx.bloom.enable` |

With path tracing on and the DLL hooked, exposure is pinned (`autoExposure.enabled = False`,
`exposureBias = 0.0`): a torch in frame otherwise meters the exposure off the flame and crushes the
dungeon to black.

## Menus, logos and loading screens

`applyRenderModeConfig(false)` switches to a flat profile whenever the game is not in a level: no
denoiser, no bloom, no post-processing, no upscaler, no ray reconstruction, no frame generation,
fixed exposure, `rtx.pathMaxBounces = 0`.

The tonemapper is deliberately left alone. Emissive radiance is not in display units and the
tonemapper is what lifts it there — both bypassing it and switching to the global curve left menus
black.

## Debug bitmask

`--remix-debug <mask>` (`RemixConvert.h`). Most bits drive the C API path and do nothing in a normal
game; these are the ones that still bite:

| Bit | Effect |
|---|---|
| `32` | Fixed exposure — takes autoexposure out of play while judging brightness |
| `128` | Forces the Remix developer UI on |
| `512` | Restores the `VK_COMPARE_OP_NEVER` alpha-test bug, on purpose |
| `4096` | World-space entities without skinning |
| `131072` | Builds sprites — fire, magic, sparks, light flares — in world space so the path tracer sees them. Off by default and unverified; see [CONTRIBUTING.md](CONTRIBUTING.md) |

The full list is in the option's help text: run `arx.exe --help`.
