# Tunable parameters

Every knob in the renderer: where it is, how to read its current value, how far it can move, and what moving it does.

## How to use this

Per the rules in [README.md](README.md), **this file does not print the current value of a setting.** It gives you the grep that prints it. A value copied into prose is wrong the next time someone tunes it, and a confidently wrong number is worse than no number. Values are quoted only where the value is a bound imposed from outside, or an equation between named symbols.

Run every grep from the repository root. On Windows, `grep` comes with Git; `Select-String` works too if you prefer.

Each entry has the same fields:

- **Symbol** — the identifier as written in the source
- **Lives in** — the file and the enclosing function or namespace
- **Kind** — `setting`, `array[quality]`, `budget`, `bound` or `derived`
- **Read it** — the grep that prints the current value
- **Safe range** — where it stops, and why it stops there
- **Raise it / Lower it** — what you will see, not what the code does
- **Breaks** — the invariant to read first

## Index

| Parameter | Kind | Controls |
|---|---|---|
| [`rtao`, `dxrShadows`, `dxrGi`](#quality-settings) | setting | Which effects run and at what level |
| [`dxrDistance`](#ray-tracing-distance) | setting | How far DXR collects and traces |
| [`fogDistance`](#render-distance) | setting | Far plane (`fog=` in cfg.ini) |
| [`aoRayCount`, `shadowRays`, `giRayCount`](#ray-counts-per-quality) | array[quality] | Rays per pixel per effect |
| [`aoRadius`](#occlusion-radius) | array[quality] | How far occlusion looks |
| [`kMaxShadowLights`](#shadow-light-budget) | budget | How many lights can cast at once |
| [`kMaxRoomTriangles`, `kMaxDynTriangles`](#triangle-budgets) | budget | Acceleration structure size limits |
| [room cache bounds](#room-cache-bounds) | budget | How much level geometry is gathered |
| [ray offsets](#ray-offsets-and-minimum-distance) | derived | Self-shadowing versus detachment |
| [`kTemporalAlpha`](#temporal-blend) | setting | How fast traced results settle |
| [blur radii and `zTol`](#spatial-blur) | setting | How much the denoiser smooths |
| [effect strengths](#effect-strengths) | setting | How dark or bright each effect reads |
| [derived constants](#things-that-look-tunable-and-are-not) | derived | Nothing. Do not edit alone |

## Quality settings

### <a id="quality-settings"></a>`rtao`, `dxrShadows`, `dxrGi`

**Symbol** the video settings of the same names.
**Lives in** `arx/src/core/Config.h`; read and clamped in `Config::init`, written in `Config::save`, in `arx/src/core/Config.cpp`.
**Kind** `setting`, persisted to `cfg.ini` and driven by the menu.
**Read it**

```
grep -n "rtao\|dxrShadows\|dxrGi" arx/src/core/Config.cpp
```

**Safe range** zero to the clamp's upper bound in `Config::init`. That bound and the length of the quality arrays are the same fact written in two files — [INV-11](INVARIANTS.md#inv-11). Zero always means off.
**Raise it** more rays and stronger denoising, so a cleaner and more stable image at a higher cost.
**Lower it** more visible dither and more temporal lag; at the lowest level the effect is a hint rather than a result.
**Breaks** [INV-11](INVARIANTS.md#inv-11).

Players also reach reflections, denoise, contact, debris, transparency, `dxr_distance`, DLSS, Frame Generation and experimental Ray Reconstruction. Those keys are listed in `docs/CONFIGURATION.md`. Everything else on this page is a source edit.

### <a id="ray-tracing-distance"></a>`dxrDistance`

**Symbol** `config.video.dxrDistance` / `dxr_distance`.
**Lives in** `arx/src/core/Config.h`; applied through `D3D12Rtao::distancePreset`.
**Kind** `setting`, 0–3 (Low / Medium / High / Ultra). Independent of the RT preset.
**Read it**

```
grep -n "distancePreset\|dxrDistance" arx/src/graphics/dxr/D3D12Rtao.h arx/src/core/Config.cpp
```

**Safe range** the clamp in `Config::init` (0–3). Each step writes caster range, room hops, light hops, reflection `TMax` and GI `TMax` into `ViewParams` — not into extra root constants ([INV-13](INVARIANTS.md#inv-13)). The live range is also clamped to the render-distance fog end.
**Raise it** more distant rooms cast and more of the bounce / reflection rays travel; bigger TLAS rebuilds.
**Lower it** far rooms drop out of the acceleration structure and far pixels skip DXR (`rtRange`).
**Breaks** [INV-13](INVARIANTS.md#inv-13).

### <a id="render-distance"></a>`fogDistance`

**Symbol** `config.video.fogDistance`, persisted as `fog=`.
**Lives in** `arx/src/core/Config.h`; converted to `cdepth` in `arx/src/graphics/GlobalFog.cpp`.
**Kind** `setting`, slider 0–10. This fork treats it as the far plane, not fog density.
**Read it**

```
grep -n "fogDistance\|fZFogStart\|fZFogEnd\|fZFogRamp" arx/src/graphics/GlobalFog.cpp arx/src/graphics/GlobalFog.h
```

**Safe range** 0–10. Fog colour comes from the zone; near-black zone colour falls back to a haze so the clip is mist, not a hole.
**Raise it** more of the map stays in the far plane.
**Lower it** the world fades into fog sooner.
**Breaks** [INV-14](INVARIANTS.md#inv-14) if D3D12 fog enable is wired to the DLSS `worldPass` flag instead of `getFog()`.

### <a id="ray-counts-per-quality"></a>`aoRayCount`, `shadowRays`, `giRayCount`

**Symbol** the arrays of the same names.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, in `D3D12Rtao::apply`.
**Kind** `array[quality]`, indexed by the matching setting.
**Read it**

```
grep -n "aoRayCount\|shadowRays\|giRayCount" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Shape** one entry per quality level. The first entry is the off state and must be zero. `shadowRays` is per light, so its cost multiplies by however many lights reach the pixel; the other two are per pixel.
**Safe range** at least one ray for any enabled level. The upper limit is where the frame budget on your target hardware runs out, which is not a number this file can give you — raise it, run the game, watch. The ray generation shader also clamps each loop, so raising an array past its clamp does nothing at all:

```
grep -n "min(giRays\|min(max(penumbraRays\|aoRays == 0" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Raise it** smoother results, less dither, more stability while standing still. Cost grows roughly linearly, and for shadows also with the number of lights in range.
**Lower it** dither becomes visible as fine noise, and the temporal history has to do more of the work, which shows as smearing behind moving objects.
**Breaks** [INV-11](INVARIANTS.md#inv-11).

### <a id="occlusion-radius"></a>`aoRadius`

**Symbol** `aoRadius`.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, in `D3D12Rtao::apply`.
**Kind** `array[quality]`, in Arx units.
**Read it**

```
grep -n "aoRadius" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Safe range** bounded below by the ray offset, since a radius smaller than the offset finds nothing. Bounded above by the size of a room: beyond that, occlusion stops describing corners and starts darkening whole surfaces because the opposite wall is in range.
**Raise it** contact darkening spreads further from corners; large rooms get generally darker.
**Lower it** occlusion tightens into creases and eventually disappears.
**Breaks** —

## Budgets

These bound work rather than describe an effect. They exist so a pathological scene cannot stall the frame.

### <a id="shadow-light-budget"></a>`kMaxShadowLights`

**Symbol** `kMaxShadowLights`.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.h`, on `D3D12Rtao`.
**Kind** `budget`.
**Read it**

```
grep -n "kMaxShadowLights" arx/src/graphics/dxr/D3D12Rtao.h
```

**Safe range** at least one. Above that, the cost is the real limit: every light in the set costs its shadow rays for every pixel it reaches, so raising this multiplies the most expensive pass. It also sizes the light buffer and the shader resource describing it, so it cannot be changed in the shader alone.
**Raise it** more lights cast at once, so fewer visible membership changes in busy areas.
**Lower it** the selection has to drop lights sooner, which makes membership changes more frequent and more noticeable even with the fade.
**Breaks** [INV-10](INVARIANTS.md#inv-10).

### <a id="triangle-budgets"></a>`kMaxRoomTriangles`, `kMaxDynTriangles`

**Symbol** the constants of the same names.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, file-scope namespace.
**Kind** `budget`.
**Read it**

```
grep -n "kMaxRoomTriangles\|kMaxDynTriangles" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Safe range** high enough that no ordinary room reaches them. They are a backstop, not a tuning target.
**Raise it** more memory per structure and slower builds; the dynamic budget is rebuilt every frame, so it is the more expensive of the two to raise.
**Lower it** triangles are dropped once the cap is hit, and the geometry that is lost is whatever happened to be gathered last — which is arbitrary, so the result is holes in shadowing that move as you walk.
**Breaks** — but watch the log: `DXR: triangle cap` means a budget was hit. In a normal room it should never appear.

### <a id="room-cache-bounds"></a>Room cache bounds

**Symbol** `DistancePreset::maxRooms`, `roomHops`, and the camera-movement threshold that forces a rebuild.
**Lives in** `D3D12Rtao::distancePreset`; applied from `collectRoomCasters` and `D3D12Renderer::applyWorldRayEffects`.
**Kind** `budget`.
**Read it**

```
grep -n "distancePreset\|maxRooms\|roomHops" arx/src/graphics/dxr/D3D12Rtao.h
grep -n "s_roomCam" arx/src/graphics/d3d12/D3D12Renderer.cpp
```

**Safe range** the two room bounds should cover what a light can reach through open portals. The movement threshold is a compromise: too small rebuilds constantly, too large lets you walk far enough that the cached set no longer describes your surroundings.
**Raise it** more of the level is available as casters, at the cost of a bigger structure and a slower rebuild when it does happen.
**Lower it** shadows from adjacent rooms disappear, most visibly through doorways.
**Breaks** — but watch the log: `DXR room cache` should appear on room changes and long moves, never every frame.

## Geometry and bias

### <a id="ray-offsets-and-minimum-distance"></a>Ray offsets and minimum distance

**Symbol** `aoBias`, `shBias`, both derived from `footprint`; and `discTol`, which decides when neighbouring pixels are too far apart to share a surface.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, in the ray generation shader inside the `kRayLib` literal.
**Kind** `derived` — each is computed per pixel from the pixel's footprint, not chosen once.
**Read it**

```
grep -n "footprint\|aoBias\|shBias\|discTol" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Safe range** the constant part must clear the thickness of the thinnest geometry you care about; the scaling part must grow at least as fast as the pixel footprint does with distance. Replacing either with a fixed number breaks one end of the distance range or the other.
**Raise it** thin geometry stops shadowing itself, but contact shadows detach and small objects start to look like they are floating.
**Lower it** contact shadows tighten, and past a point gate bars and foliage begin to shade themselves black.
**Breaks** [INV-05](INVARIANTS.md#inv-05).

## Denoise and history

### <a id="temporal-blend"></a>`kTemporalAlpha`

**Symbol** `kTemporalAlpha`.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, file-scope namespace.
**Kind** `setting` — the weight given to the current frame when blending with history.
**Read it**

```
grep -n "kTemporalAlpha\|temporalAlpha" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Safe range** strictly between zero and one. Zero freezes the image on the first frame and never updates; one disables history entirely and brings back every artefact history exists to hide.
**Raise it** the image reacts faster to change, so less smearing behind moving objects, but more of the raw per-frame noise survives and the result sparkles while walking.
**Lower it** smoother and steadier, at the cost of a visible lag: a shadow takes longer to catch up with the thing casting it, most obviously behind moving creatures.
**Breaks** — but note that history is only blended where reprojection found a valid match, so lowering this does not help pixels that have no history at all.

### <a id="spatial-blur"></a>Blur radii and depth tolerance

**Symbol** the radius arguments passed to `bilateral` in the composite, and `zTol` inside it.
**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, in the `kComposite` literal.
**Kind** `setting`.
**Read it**

```
grep -n "bilateral(\|zTol" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Safe range** the radii are bounded above by the size of the smallest feature you need to keep. Occlusion is low-frequency and tolerates a wide blur; shadows are not, and a wide blur erases narrow shadows entirely — a gate's bars stop casting once the blur is wider than the gaps between them. `zTol` decides how different two pixels' depths may be before the blur refuses to mix them; too large and it smears across silhouettes, too small and it rejects almost every neighbour on a surface seen at a grazing angle, leaving that surface noisy.
**Raise it** smoother, at the cost of detail and of bleeding across edges.
**Lower it** sharper, at the cost of visible dither that the temporal blend then has to absorb.
**Breaks** —

The radii are edited in the composite shader, which is compiled by the legacy compiler. Mind [INV-03](INVARIANTS.md#inv-03).

## Effect strengths

**How dark occlusion goes, how dark an umbra goes, how bright the bounce is** — the floors, mixes and multipliers in the ray generation and composite shaders.

**Lives in** `arx/src/graphics/dxr/D3D12Rtao.cpp`, in both shader literals.
**Kind** `setting`.
**Read it**

```
grep -n "occ / float\|sumVis\|lerp(1.0, bilateral\|lerp(0.10\|giUpsample" arx/src/graphics/dxr/D3D12Rtao.cpp
```

**Every one of these values is explained in `arx/src/graphics/dxr/README.md`**, including what previous values looked like on screen and why they were rejected. That file is the source of truth for them. Read it before changing one, and record the change there rather than here.

Two things this file will tell you that are worth knowing before you touch anything:

- Strength is deliberately independent of quality level. The quality setting changes ray counts and denoising, never how strong the result looks. A player lowering quality should get a noisier version of the same image, not a weaker effect.
- Do not weaken an effect to hide an artefact. It leaves you with both a weak effect and the artefact, and it hides the evidence you would have needed to find the real cause.

**Breaks** [INV-03](INVARIANTS.md#inv-03) if you edit the composite, and [INV-09](INVARIANTS.md#inv-09) if you touch how bounce samples are averaged.

## Things that look tunable and are not

These are constants in the same file, next to the ones above. They are derived or contractual. Changing one alone breaks something silently.

| Looks like a knob | Actually | See |
|---|---|---|
| `kRootConstants` | The size of the constant buffer in words. It must equal the size of the matching struct, and the hardware root-signature cap is 64 DWORDs (already full). A `static_assert` catches a size mismatch but not a field reordered inside the same size | [INV-01](INVARIANTS.md#inv-01), [INV-13](INVARIANTS.md#inv-13) |
| `kRtSrvCount`, `kRtUavBase`, `kRtUavCount`, `kCompositeBase`, `kHeapCount` | Five projections of one descriptor layout, tied by an equation, and each also stated as a register in the shaders | [INV-02](INVARIANTS.md#inv-02) |
| `SKY_Z` | The depth value that means "nothing was drawn here". It must equal the clear value exactly | [INV-04](INVARIANTS.md#inv-04) |
| `kLightFloat4s` | How many four-float groups one light occupies. It must match both the packed structure and the shader's indexing of the light buffer, and a `static_assert` ties the first two | [INV-08](INVARIANTS.md#inv-08) |
| `kShaderRecord`, `kIdentifierSize` | Shader table record size, fixed by the hardware interface | — |
| The instance ids | A contract between the structure builder and the hit shader, not an ordering preference | [INV-08](INVARIANTS.md#inv-08) |

None of these has a safe range, because none of them has a range. They have a correct value determined by something else.
