# Invariants

Rules that must hold, what breaks when they do not, and how to tell. Every entry here was paid for once already.

Ids are permanent. Other documents link them, so they are append-only: a rule that stops applying is struck through in place, never renumbered and never deleted.

The worst entries below are the ones whose **Detect** field says *none — silent*. Those break without a compiler error, without a log line, and without an obvious symptom. They are why this file exists.

## Symptom index

Start here if something is already wrong.

| Symptom | Look at |
|---|---|
| A tuning value behaves as if it were enormous | [INV-01](#inv-01) |
| The ray passes produce garbage, or a black or white screen, or the device is removed | [INV-02](#inv-02) |
| The world renders as plain raster with no ray tracing, and nothing looks broken | [INV-03](#inv-03), [INV-12](#inv-12) |
| Shadows and occlusion stop at a fixed distance, and the boundary shimmers | [INV-04](#inv-04) |
| Thin geometry goes black: gate bars, roots, cobwebs | [INV-05](#inv-05), [INV-07](#inv-07) |
| Small objects look detached, as if floating | [INV-05](#inv-05) |
| A spell or torch flare is darkened, or shows the shadow of what is behind it | [INV-06](#inv-06) |
| Bright single pixels or blocks that flicker as the camera moves | [INV-09](#inv-09) |
| Shadows pop in or out in one frame while walking | [INV-10](#inv-10) |
| A quality level reads as Off in the menu but the effect is on, or the reverse | [INV-11](#inv-11) |
| Hit geometry is lit or shaped wrongly, but only for one category of object | [INV-08](#inv-08) |

## Build-time and layout invariants

### <a id="inv-01"></a>INV-01 — A `float4x4` in the ray constant buffer must begin on a four-float boundary

**Break it by** adding, removing or reordering a scalar in the `Constants` struct in `D3D12Rtao::apply` without keeping the padding fields that hold the following matrix aligned.

**Symptom** every field declared *after* the matrix reads another field's bits. Historically a ray-count field picked up the screen width and traced roughly a thousand shadow rays per light. The game did not crash; it became unplayably slow, which reads as a performance regression rather than a layout bug.

**Mechanism** HLSL starts a `float4x4` at the next four-float boundary. C++ starts it at the next four-byte offset. Any run of scalars before the matrix whose count is not a multiple of four shifts every later field by one to three slots. The `static_assert` compares the *total* size, so shifting fields inside a struct of unchanged size passes it.

**Detect** *none — silent.* Count the scalars before each matrix in both declarations and confirm the count is a multiple of four.

```
grep -n "float4x4\|pad0\|pad1\|kRootConstants" arx/src/graphics/dxr/D3D12Rtao.cpp
```

### <a id="inv-02"></a>INV-02 — The descriptor heap is one equation, not five constants

**Break it by** editing any one of the heap constants on its own, or adding a shader resource without moving the ones after it.

**Symptom** no build error, no log line. A shader reads a descriptor that was never written: garbage, a fully black or fully white pass, or a removed device on some drivers and not others.

**Mechanism** the constants are five projections of a single layout. Shader resources occupy the low slots, unordered access views follow them, the composite's resources follow those, and the heap has to be at least as large as the end of the last range. Each range is also declared once in a root signature and once as a register in HLSL, and those must agree.

The relationships, which stay true no matter what the numbers become:

```
highest literal SRV slot used in updateDescriptors  <  kRtSrvCount
kRtUavBase      == kRtSrvCount
kCompositeBase  == kRtUavBase + kRtUavCount
kHeapCount      >= kCompositeBase + (composite SRV range size)
```

And a fourth leg no arithmetic can check: each resource's HLSL register must equal its offset within its own range. The ray library's `t` registers count from the start of the shader-resource range, its `u` registers from the start of the unordered range, and the composite's `t` registers from `kCompositeBase`.

**Detect** *none in game.* Enable the Direct3D 12 debug layer and look for descriptor range or heap bounds warnings.

```
grep -n "kRtSrvCount\|kRtUavBase\|kRtUavCount\|kCompositeBase\|kHeapCount" arx/src/graphics/dxr/D3D12Rtao.cpp
grep -n "slot(" arx/src/graphics/dxr/D3D12Rtao.cpp
grep -n ": register(" arx/src/graphics/dxr/D3D12Rtao.cpp
```

Adding one shader resource is six edits, all or nothing. The recipe is in [WHERE-TO-EDIT.md](WHERE-TO-EDIT.md#add-a-shader-resource).

### <a id="inv-03"></a>INV-03 — The composite shader and the ray library are compiled by different compilers

**Break it by** writing a construct one accepts into the other. They are adjacent string literals in one source file, which makes editing the wrong one easy.

**Symptom** the ray tracing silently does nothing. The world renders as plain raster, which reads as "the feature is off" rather than "a shader failed to build".

**Mechanism** the ray library is compiled to a shader-model-6 library through the newer compiler. The composite is compiled to shader-model-5 vertex and pixel shaders through the legacy compiler, which rejects syntax the newer one accepts. When either fails, initialisation reports the failure and the renderer continues without the pass.

**Detect**

```
grep -n "lib_6_3\|vs_5_0\|ps_5_0" arx/src/graphics/dxr/D3D12Rtao.cpp
```

In the log, look for `RTAO: composite VS failed`, `RTAO: composite PS failed`, or `RTAO: lib_6_3 compile failed`.

### <a id="inv-11"></a>INV-11 — A setting's clamp and its quality array are the same fact

**Break it by** adding a quality level to the array without widening the clamp in `Config::init`, or widening the clamp without extending the array.

**Symptom** either the top quality level is unreachable no matter what the menu shows, or the setting indexes past the end of the array, which is undefined behaviour that usually reads as an absurd ray count.

**Mechanism** the setting is clamped when it is read from disk, then used directly as an index into a per-quality array in the renderer. The clamp's upper bound and the array's last index must be the same number, stated in two files that do not include each other.

**Detect** *none — silent* in the direction that under-clamps. Compare the two:

```
grep -n "glm::clamp(reader.getKey(Section::Video" arx/src/core/Config.cpp
grep -n "aoRayCount\|shadowRays\|giRayCount\|aoRadius" arx/src/graphics/dxr/D3D12Rtao.cpp
```

Every array's first entry must also be zero, because the menu's Off state is the zero index and a non-zero entry would leave the effect running while the menu says it is off.

## Run-time invariants

### <a id="inv-04"></a>INV-04 — The sky test must use the depth clear value, not an approximation of it

**Break it by** rounding it, or by changing the depth clear without changing it.

**Symptom** ambient occlusion and shadows stop at a fixed distance from the camera, and the boundary shimmers as the idle animation moves the camera.

**Mechanism** the test asks "is this pixel sky, and therefore not worth tracing". Depth is not linear, so a value slightly below the clear value is reached at a finite distance in the world. Every pixel beyond it is misclassified as sky and skipped. The camera is never perfectly still, so pixels cross the threshold every frame and a static cutoff becomes a flicker.

**Detect** no log line. Walk toward a distant wall and watch for shadows appearing at a fixed distance.

```
grep -n "SKY_Z" arx/src/graphics/dxr/D3D12Rtao.cpp
```

### <a id="inv-05"></a>INV-05 — Ray origin offset and minimum distance must scale with the pixel footprint, and must match each other

**Break it by** replacing either with a constant, or changing one without the other.

**Symptom** thin geometry shadows itself and goes black — gate bars, roots, cobwebs. Biased the other way, contact shadows detach and small objects appear to float.

**Mechanism** one pixel covers more world space the further away it is. An offset large enough to clear a surface up close is inside that surface at distance. The ray's minimum distance must equal the offset, or the ray immediately re-enters the surface it started on.

**Detect** visual, at a gate, walking toward it and away. Confirm both derive from the footprint:

```
grep -n "footprint\|aoBias\|shBias" arx/src/graphics/dxr/D3D12Rtao.cpp
```

### <a id="inv-07"></a>INV-07 — Cutout geometry must not be a caster

**Break it by** feeding alpha-tested polygons into the acceleration structure.

**Symptom** foliage, cobwebs and grates cast solid rectangular shadows and shade themselves black.

**Mechanism** a ray cannot see a texture. To the acceleration structure a cutout quad is a solid quad, so the holes that make it read as foliage do not exist. The fix is to exclude those polygons from the caster set entirely, which trades their shadow for the absence of a wrong one.

**Detect** the log reports how many were excluded per room rebuild:

```
grep -n "alphaSkipped" arx/src/graphics/d3d12/D3D12Renderer.cpp
```

If that count is most of the room, the exclusion test is catching real walls and you have a different bug.

### <a id="inv-08"></a>INV-08 — Every geometry category needs an instance id, a vertex buffer and a branch that agrees

**Break it by** adding a bottom-level structure to the top-level one without extending the branch in the closest-hit shader that maps instance id to vertex buffer.

**Symptom** hit geometry is shaded using another category's vertices. Surface normals come out wrong for one class of object only, which reads as lighting being broken for, say, entities but not walls.

**Mechanism** the hit shader receives a primitive index and an instance id. It has no other way to know which buffer the triangle came from, so it branches on the instance id to choose. A new category is three coordinated changes: the instance written into the top-level structure, a shader resource holding its vertices, and a branch reading it. The shader resource part is [INV-02](#inv-02).

**Detect** *none — silent.*

```
grep -n "InstanceID\|writeInst" arx/src/graphics/dxr/D3D12Rtao.cpp
```

### <a id="inv-09"></a>INV-09 — Averaged ray results divide by rays launched, never by rays that hit

**Break it by** counting hits and dividing by that count.

**Symptom** isolated bright pixels or blocks that flicker as the camera moves. In the bounce pass they appear near light sources.

**Mechanism** dividing by hits turns "one ray out of many found something bright" into "everything found something bright". A single lucky sample then represents the whole pixel. Dividing by rays launched treats a miss as the zero contribution it is. Clamping each ray's contribution before summing bounds the damage a single sample can do.

**Detect** visual. Confirm the divisor is the launched count:

```
grep -n "nGi\|gi /= float" arx/src/graphics/dxr/D3D12Rtao.cpp
```

### <a id="inv-10"></a>INV-10 — A light entering or leaving the shadow set must fade, never pop

**Break it by** adding or removing a light from the set with full weight in one frame.

**Symptom** shadows appear or vanish in a single frame while walking, which reads as flickering.

**Mechanism** the set of lights that cast shadows is bounded, so walking changes its membership. Each light carries a weight that ramps in over several frames when it joins and ramps out before it is dropped, so a membership change becomes a short fade. The weight multiplies the light's contribution in the shader; a light at zero weight is still in the buffer but contributes nothing.

**Detect** membership changes are logged. Standing still they should not repeat.

```
grep -n "presence" arx/src/graphics/d3d12/D3D12Renderer.cpp arx/src/graphics/dxr/D3D12Rtao.cpp
```

## Ordering invariants

### <a id="inv-06"></a>INV-06 — The composite must run after the world and before particles, flares and the HUD

**Break it by** moving the hook later in the frame.

**Symptom** additive effects are darkened and carry the shading of whatever is behind them. A spell flare over a table shows the table's shadow through itself.

**Mechanism** the composite multiplies the whole render target by the traced occlusion and shadow terms. Anything already drawn is multiplied too. Additive effects have no business being occluded by the surface behind them, and the depth buffer at those pixels belongs to that surface, not to the effect.

**Detect** visual, by casting a spell near a shadowed surface. Confirm the position of the call:

```
grep -n "applyWorldRayEffects" arx/src/core/ArxGame.cpp
```

### <a id="inv-12"></a>INV-12 — Ray tracing must degrade to raster, never to a broken frame

**Break it by** assuming a resource, a capability or a compile succeeded.

**Symptom** when this holds, the game runs as a plain raster and says why in the log. When it is broken, the frame is wrong instead.

**Mechanism** every stage can fail on a real machine: the hardware may not support ray tracing, the shader compiler may be absent, a shader may fail to build, a target may fail to allocate, and there may be no geometry to trace. Each failure returns early and leaves the rastered frame untouched.

**Detect** the log always says which stage stopped. Absence of `RTAO ready` means initialisation did not finish.

```
grep -n "RaytracingTier=\|RTAO ready\|raster only" arx/src/graphics/dxr/D3D12Rtao.cpp
```

## Process invariants

No detection, because these are about how you work rather than what the code does. Each one exists because breaking it has already cost days.

- **One symptom at a time.** Fix one thing, prove it, then move on. Stacking hypotheses makes it impossible to tell which change did what.
- **Prove it in `runtime/user/arx.log`.** A screenshot shows that something changed. The log shows what the code decided. Both belong in a pull request; the log is the one that settles arguments.
- **Quit through the menu when testing.** It produces a clean shutdown and a complete log.
- **Never revive the Remix path**, and never introduce `toRemix`. See [COMPONENTS.md](COMPONENTS.md#dead-code).
- **Never dual-boot the OpenGL renderer** to answer a Direct3D question.
- **Do not weaken an effect to hide an artefact.** Turning a shadow down until the flicker stops leaves both a weak shadow and the bug.

## Diagnostics

Every ray tracing line the game writes to `runtime/user/arx.log`, and what each proves. The **Bad if** column is the point: most of these are only meaningful by their frequency.

| Log line begins with | Written by | Proves | Bad if |
|---|---|---|---|
| `Using D3D12 renderer` | `D3D12Renderer` init | Which backend and adapter is live | Absent when you expected Direct3D 12 |
| `RaytracingTier=` | `D3D12Rtao::init` | The hardware's support level | Reads `NOT_SUPPORTED` on hardware that should support it |
| `RTAO ready` | `D3D12Rtao::init` | Pipeline, heap and light buffer all built | Absent — everything after it is disabled |
| `RTAO: ... failed`, `raster only` | `D3D12Rtao` init paths | A named stage failed and the game fell back | Present at all |
| `RTAO enabled quality=` | `applyWorldRayEffects` | Occlusion is running, at which level | Level does not match the menu |
| `DXR shadows enabled quality=` | `applyWorldRayEffects` | Shadows are running, with how many lights | Light count is zero in a lit room |
| `DXR GI enabled quality=` | `applyWorldRayEffects` | Bounce light is running | Level does not match the menu |
| `RTAO disabled`, `DXR shadows disabled`, `DXR GI disabled` | `applyWorldRayEffects` | The effect is off by setting, not by failure | Present when the menu says otherwise |
| `DXR room cache tris=` | `applyWorldRayEffects` | Cached room geometry was rebuilt | **Every frame.** Expect it on room changes and long moves only |
| `DXR room casters rooms=` … `alphaSkipped=` | `collectRoomCasters` | How much geometry was gathered, and how much cutout was excluded | `alphaSkipped` is most of the room — see [INV-07](#inv-07) |
| `DXR lights set changed` | `fillShadowLights` | Which lights joined or left, with their distances | Repeats while standing still — see [INV-10](#inv-10) |
| `DXR lights params changed` | `fillShadowLights` | Same lights, different values | Repeats while standing still |
| `DXR entity movers` | `collectEntityCasters` | Which entities changed shape this frame | A static prop appears in the list |
| `DXR: triangle cap` | `D3D12Rtao::addTris` | A geometry budget was hit and triangles were dropped | Present in a normal room — see [TUNING.md](TUNING.md) |

No sample output is quoted here on purpose: it would fix a moment in time and start rotting immediately. Grep for the prefix and read what your own run produced.
