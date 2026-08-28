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

### Why the developer menu does not stick

Because channel 3 keeps winning. The game re-applies its own values when a device registers, when a
video option changes, and every time the render mode flips between a menu and the level — so a
slider dragged in the developer menu is overwritten shortly after, and saving it to `user.conf`
changes nothing on the next run either.

That is protection against a stale `user.conf`, and it is in the way while tuning. Run with
**`--remix-debug 262144`** to turn it off: the four structural options are still forced, everything
about the look is left to `rtx.conf`, `user.conf` and the menu. Tune it live, save it, then read the
values back out of `user.conf` — that is how a number gets into the table below.

## Applied once, after device registration

`applyPreviewConfig()` — pinned so a stale `user.conf` cannot change them.

| Option | Value | Why |
|---|---|---|
| `rtx.enableRaytracing` | `True` | |
| `rtx.fallbackLightMode` | `0` | A camera-attached fallback light is visible in frame and wrecks autoexposure |
| `rtx.volumetrics.enable` | `False` | |
| `rtx.usePostFilter` | `False` | |
| `rtx.maxAnisotropySamples` | `16` | Floors are seen at a grazing angle, where isotropic mip selection dissolves the slabs |
| `rtx.ignoreAllVertexColorBakedLighting` | `True` | Arx's 2002 lightmaps must not multiply on top of traced light. Inverted by `--remix-debug 524288` |
| `rtx.vertexColorIsBakedLighting` | `False` | `True` under that bit |
| `rtx.vertexColorStrength` | `0` | `1.0` under that bit |
| `rtx.autoExposure.enabled` | `True` | Overridden below when the Remix DLL is hooked |
| `rtx.tonemap.exposureBias` | `-0.6` | |
| `rtx.integrateIndirectMode` | `2` | Pins importance sampling; NRC failed to initialise on the development GPU |

When the Remix DLL is hooked — the path the game actually runs — four options are **structural**
and are applied before anything else, because they describe the geometry being submitted rather than
how it should look. Nothing overrides these, not even the developer menu:

| Option | Value | Why |
|---|---|---|
| `rtx.leftHandedCoordinateSystem` | `True` | D3D9 is left-handed and Arx is Y-down |
| `rtx.zUp` | `False` | |
| `rtx.sceneScale` | `1.0` | One Arx unit is about one centimetre |
| `rtx.camera.correctProjectionYFlip` | `False` | The engine already flips Y from NDC to screen; correcting again renders the world upside down |

The rest describe the look, and can be handed over to the developer menu with
`--remix-debug 262144`:

| Option | Value | Why |
|---|---|---|
| `rtx.legacyMaterial.roughnessConstant` | `0.75` | No USD replacements, so one value describes every surface. Was `0.35`, which read as wet plastic once path tracing was actually presenting |
| `rtx.legacyMaterial.metallicConstant` | `0.0` | Was `0.12`. Dungeon stone is not metal, and the tint it added was part of the plastic look |
| `rtx.legacyMaterial.emissiveIntensity` | `0.0` | |
| `rtx.lightConversionIntensityFactor` | `0.5` | Gain applied when D3D9 lights become Remix lights. Ours already carry Arx intensity, so the default washed the cell out. A starting point, not a measurement |
| `rtx.enableEmissiveBlendModeTranslation` | `True` | Turns additively blended draws — flames, flares, magic — into emissive surfaces. This is how light from the hundreds of level lights that cannot fit through fixed-function D3D9 gets back in |
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
| `131072` | Builds sprites — fire, magic, sparks, light flares — in world space so the path tracer sees them |
| `262144` | Stops the game re-applying the look options, so the developer menu and the `user.conf` it writes are what decide |
| `524288` | Keeps Arx's baked vertex lighting instead of discarding it. Vanilla's light distribution is the bake, so this matches the original at the cost of the shadows being baked too |

The full list is in the option's help text: run `arx.exe --help`.
