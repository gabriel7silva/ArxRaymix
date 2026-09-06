# Graph map

Four diagrams, each answering one question. Nodes are files, functions and resources — never values — which is what keeps them true while the numbers underneath change.

## How to read these

| Shape | Means |
|---|---|
| `["a name"]` | A module, directory or file |
| `("a name")` | A function or entry point |
| `[("a name")]` | A resource with contents: a texture, a buffer, a file on disk |
| `[["a name"]]` | An external boundary — not our code |
| `{"a question"}` | A run-time branch |

| Arrow | Means |
|---|---|
| `-->` | Calls, or runs before |
| `-.->` | Reads or writes data; the label names what flows |
| `==>` | The hot path a normal frame actually takes |
| Dashed outline | Dead code. Never revive |

```mermaid
graph LR
  Mod["a module or file"]
  Fn("a function")
  Res[("a resource")]
  Ext[["an external API"]]
  Dead["dead: never revive"]
  Mod --> Fn
  Fn -.->|"writes"| Res
  Fn ==> Ext
  classDef dead stroke-dasharray:4 3,opacity:0.55
  class Dead dead
```

## Graph 1 — Modules and ownership

Which pieces exist, and which way the dependencies point.

```mermaid
graph TD
  subgraph Game["arx/src — game"]
    Core("ArxGame::renderLevel")
    Scene("ARX_SCENE_Render")
    Anim["animation/"]
    Light["scene/Light"]
    Gui["gui/MainMenu"]
    Cfg["core/Config"]
  end

  subgraph Gfx["arx/src/graphics — renderer"]
    Iface["Renderer.h"]
    D12["d3d12/"]
    Dxr["dxr/"]
    D9["d3d9/"]
    GL["opengl/"]
    Remix["remix/ — dead"]
  end

  Win["window/SDL2Window"]
  Api[["Direct3D 12 and DXR"]]

  Core ==> Scene
  Core ==> Iface
  Scene --> Anim
  Scene --> Light
  Gui -.->|"writes settings"| Cfg
  Win -->|"creates one backend"| Iface
  Iface --- D12
  Iface --- D9
  Iface --- GL
  D12 --> Dxr
  Dxr --> Api
  D12 --> Api
  Cfg -.->|"quality levels"| D12

  classDef dead stroke-dasharray:4 3,opacity:0.55
  class Remix dead
```

What this asserts, and you could disprove:

- One backend is created per process, by the window layer. There is no path that runs two.
- `dxr/` depends on `d3d12/`, never the reverse. Ray tracing is an addition to the raster, so removing it leaves a working renderer.
- `remix/` has no inbound edges at all.
- The menu writes settings; it never talks to the renderer.

Re-derive it by reading the backend construction in `arx/src/window/SDL2Window.cpp`.

## Graph 2 — One frame, in order

When your code runs, relative to everything else.

```mermaid
flowchart TD
  Clear("Clear colour and depth")
  SceneR("ARX_SCENE_Render")
  Dbg("drawDebugRender")
  Cine{"in a cinematic?"}
  Ray("applyWorldRayEffects")
  Part("particles")
  Flares("magical flares")
  Spells("ARX_SPELLS_Update")
  Batch("g_renderBatcher.render")
  LFlare("renderLightFlares")
  Hud("HUD, notes, minimap, cursor")

  subgraph Inside["inside ARX_SCENE_Render"]
    Opaque("rooms, opaque")
    Inter("entities, then the player")
    Trans("transparency")
    Water("water and lava")
    Halo("halos")
  end

  Clear ==> SceneR
  SceneR --- Inside
  Opaque --> Inter --> Trans --> Water --> Halo
  SceneR ==> Dbg ==> Cine
  Cine -->|"no"| Ray
  Cine -->|"yes, skipped"| Part
  Ray ==> Part ==> Flares ==> Spells ==> Batch ==> LFlare ==> Hud
```

What this asserts:

- The ray tracing composite runs **after** the whole world is rastered and **before** particles, flares and the HUD. Additive effects must not be multiplied by the world's shading — see [INV-06](INVARIANTS.md#inv-06).
- It is skipped entirely during cinematics.
- Within the scene pass, opaque geometry precedes transparency, which precedes water.

Re-derive it by reading `ArxGame::renderLevel` in `arx/src/core/ArxGame.cpp` top to bottom, then `ARX_SCENE_Render` in `arx/src/scene/Scene.cpp`.

## Graph 3 — Inside the ray tracing pass

What feeds what, within one call.

```mermaid
flowchart LR
  Feed("fillShadowLights, collectRoomCasters, collectEntityCasters")
  Apply("D3D12Rtao::apply")
  Geo("ensureGeometryBuffers")
  Accel("buildAcceleration")
  Copy("copy backbuffer")
  Rays("DispatchRays — RayGen")
  Comp("composite draw")
  Save("copy to history")

  Lights[("light buffer")]
  Blas[("room and dynamic BLAS")]
  Tlas[("TLAS")]
  Colour[("colour copy")]
  Depth[("scene depth")]
  Out[("ao, shadow, gi, depth")]
  Hist[("previous ao, shadow, gi, depth")]
  Back[("backbuffer")]

  Feed -.->|"lights"| Lights
  Feed -.->|"triangles"| Apply
  Apply ==> Geo ==> Accel ==> Copy ==> Rays ==> Comp ==> Save
  Accel -.-> Blas -.-> Tlas
  Copy -.-> Colour
  Depth -.->|"receivers"| Rays
  Tlas -.->|"casters"| Rays
  Lights -.-> Rays
  Hist -.->|"read: last frame"| Rays
  Rays -.->|"write"| Out
  Out -.-> Comp
  Colour -.-> Comp
  Comp -.->|"shaded image"| Back
  Out -.->|"written at the end"| Save
  Save -.-> Hist
```

What this asserts:

- History is copied at the **end** of the pass, so every history read inside a frame is last frame's data. There is no read-after-write hazard, and none of the history is valid on the first frame after a resize.
- Receivers come from the depth buffer, one per screen pixel. Casters come from the acceleration structure. They are different inputs and a bug in one does not look like a bug in the other.
- The backbuffer is copied before tracing, because the composite needs the unshaded image while it is also the render target.

Re-derive it by reading `D3D12Rtao::apply` in `arx/src/graphics/dxr/D3D12Rtao.cpp` top to bottom.

## Graph 4 — A setting's journey

Where a value goes after someone moves a slider. This is the diagram that explains why adding one option is five edits.

```mermaid
flowchart LR
  Ini[("cfg.ini")]
  Init("Config::init — read and clamp")
  Cfg[("config.video")]
  Menu("RayTracingOptionsMenuPage")
  Save("Config::save — write")
  Use("applyWorldRayEffects")
  Arrays("quality arrays in D3D12Rtao::apply")
  Cb[("root constants")]
  Hlsl["kRayLib and kComposite"]

  Ini -.-> Init -.-> Cfg
  Menu -.->|"on change"| Cfg
  Menu --> Save -.-> Ini
  Cfg -.->|"read once per frame"| Use
  Use --> Arrays
  Arrays -.->|"ray counts"| Cb
  Cb -.-> Hlsl
```

What this asserts:

- The menu writes the setting and saves it; it never reaches the renderer directly.
- The renderer reads the setting once per frame and turns it into ray counts through a per-quality array.
- Nothing on this path is skippable, which is why the recipe in [WHERE-TO-EDIT.md](WHERE-TO-EDIT.md) has five steps and not one.
- The clamp in `Config::init` and the length of the quality arrays are the same fact stated twice — see [INV-11](INVARIANTS.md#inv-11).

Re-derive it by following one setting name through the tree:

```
grep -rn "rtao" arx/src/core/Config.h arx/src/core/Config.cpp
```
