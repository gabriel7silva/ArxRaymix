# Glossary

Words this project uses in a way you cannot look up elsewhere. Engine terms inherited from Arx Libertatis are marked *(upstream)*; the rest are the fork's.

## World and level

**Arx space** *(upstream)*
The game's own world coordinate system and units. Distances in the renderer are in these units, not metres: a torch reaches a couple of hundred of them, a room is a few thousand across. Every distance in the ray tracing code is Arx units unless it says otherwise. Nothing converts to or from another space; a conversion appearing in new code is a bug.

**Room** *(upstream)*
A convex-ish chunk of level geometry used for visibility. The level is partitioned into rooms joined by portals, and the engine draws only the rooms reachable from the camera's room. The ray tracer reuses this partition to decide which geometry to feed the acceleration structure.

**Portal** *(upstream)*
The opening between two rooms. Walking through one changes which room the camera is in, which is one of the two triggers that rebuild the cached ray tracing geometry.

**Tile** *(upstream)*
The background geometry grid a room's polygons are stored in. Room polygons are reached as a tile plus an index, not as a flat list.

**Entity** *(upstream)*
Anything in the level that is not background: a creature, an item, a door, the player. Entities have meshes that animate; background geometry does not.

## Rendering

**The raster**
The ordinary triangle rendering of the world, done by the Direct3D 12 backend. Everything the ray tracer does is layered on top of a finished raster image. The fork never replaces the raster with tracing.

**Hybrid**
The arrangement this fork uses: raster the world, then trace rays for ambient occlusion, shadows, bounce light and reflections, then combine the two. The opposite would be path tracing. Path tracing is archived (Phase 6) and is not the product.

**Composite**
The full-screen pass at the end of the ray tracing work that takes the rastered image and multiplies or adds the traced results into it. When someone says "the composite", they mean this pass and the pixel shader that implements it.

**Backend**
One implementation of the renderer interface: Direct3D 12, Direct3D 9, OpenGL. Exactly one is live per process.

## Ray tracing

**BLAS / TLAS**
Bottom- and top-level acceleration structures, the two-tier index rays are traced against. A BLAS holds triangles. The TLAS holds *instances*, each pointing at a BLAS. This fork builds one BLAS for cached room geometry and one for per-frame dynamic geometry, then a TLAS containing both.

**Instance**
One entry in the TLAS. Each carries an **instance id**, which the hit shader reads to know which vertex buffer the triangle it hit came from, and an **instance mask**, which lets a ray choose to ignore whole categories of geometry.

**Caster**
Geometry that blocks a ray, put into the acceleration structure. Not everything drawn is a caster. Cutout foliage is deliberately excluded.

**Receiver**
The surface a traced result is written for. Receivers come from the depth buffer, one per screen pixel, not from geometry.

**Cutout**
A texture whose transparency is a hard on/off mask rather than a blend: roots, cobwebs, grates. Rays cannot see a texture, so a cutout quad would block light as though it were solid. See [INV-07](INVARIANTS.md#inv-07).

**Footprint**
How much world space one screen pixel covers at a given depth. Far pixels have large footprints. Several ray tracing decisions scale with it rather than using a fixed distance, because a fixed distance that is correct up close is wrong far away.

**Penumbra**
The soft edge of a shadow. Produced by treating a light as a disk and tracing several rays to different points on it, rather than one ray to a point.

**Presence fade**
The per-light weight that ramps up when a light joins the shadow set and down when it leaves, so the set changing never causes a one-frame pop. See [INV-10](INVARIANTS.md#inv-10).

**History / reprojection**
Last frame's traced result, reused this frame. The current world position is projected through last frame's camera to find which pixel held the same surface, and the two are blended. This is what stops the traced results from shimmering while the camera moves.

**Dither**
Deliberately varying the ray directions per pixel so that too-few-rays shows up as fine noise rather than as banding. Combined with history and the blur, it reads as smooth.

**Quality level**
The four-way Off / Low / Medium / High setting each effect has. Quality levels change ray counts and denoiser strength. They do not change how strong an effect looks — that is fixed in the shader.

**Render distance**
The Options → Render slider persisted as `fog=` (0–10). In this fork it is the far plane. The world fades into zone fog; it is not a density knob and it is not an unload of the map.

**Ray tracing distance**
The Options → Ray tracing slider persisted as `dxr_distance=` (Low / Medium / High / Ultra). It scales how far casters are collected and how far reflection / GI rays travel. Independent of the quality preset.

**Streamline**
NVIDIA's hook SDK used for DLSS Super Resolution, Frame Generation and Ray Reconstruction. Implementation: `D3D12Streamline`. Fetch with `scripts/fetch-streamline.ps1`.

**Ray Reconstruction / `rrLive()`**
DLSS Ray Reconstruction. Experimental. `slIsFeatureSupported` can be true while NGX create still fails (`0xBAD00005` on some laptops). Homemade denoise stays until `rrLive()` is true after a successful evaluate. Do not skip temporal history just because the slider is On.

## Process

**Option A**
The original name for the ambient occlusion work, still used in `arx/src/graphics/dxr/README.md` and in some log lines. It means the hybrid approach: keep the raster, add ray traced ambient occlusion on top.

**Phase**
A numbered stage of the ray tracing roadmap, tracked in `arx/src/graphics/dxr/README.md`. Phases 1–4 are done. Phase 5 is DLSS + Frame Generation (RR experimental). Phase 6 (path-traced multi-bounce) is archived. Phases are historical labels, not code structure.

**Remix**
An abandoned experiment that routed the game through the NVIDIA RTX Remix runtime. Dead. See the dead code section of [COMPONENTS.md](COMPONENTS.md#dead-code).
