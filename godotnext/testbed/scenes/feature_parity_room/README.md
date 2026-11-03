# Feature Parity Room (#64)

A sealed room, 8 × 6 × 3 m (X × Z × Y), built from `MeshInstance3D`
nodes, viewed from three fixed cameras. It exercises the forward
features the D7 material-bucket resolve must match by construction,
plus materials that are expected to fall back. Gates G4-6 (FLIP mean
≤0.01 at the finest cut) and G5-5 (FLIP ≤0.02 at the default cut) use
it in PQ.

The scene is generated; never edit `feature_parity_room.tscn` by hand.

```
godot --headless --path godotnext/testbed --script res://generators/build_feature_parity_room.gd
godot --headless --path godotnext/testbed --script res://scenes/feature_parity_room/check_features.gd
```

## Features and their nodes

| Feature | Nodes |
|---|---|
| Decals with albedo, normal, ORM and emission maps, on a wall and the floor | `DecalWall`, `DecalFloor`; `DecalCulled` excludes render layer 2 via `cull_mask` and sits over `Layer2Block` (layer 2 only), which therefore receives no decal |
| Box-projected reflection probe over a rough metallic panel | `ReflectionProbeMetal` (`box_projection`, update ONCE) over `MetalPanel` (metallic 1.0, roughness 0.45) |
| Textured area light | `AreaLight` (`area_texture`, `area_size` 2.4 × 1.2 m) on the ceiling |
| Projector spot and layer-culled omni | `SpotProjector` (`light_projector`), `OmniCulled` (`light_cull_mask` excludes layer 2) |
| Depth fog with height density and aerial perspective under a sky | `WorldEnvironment`: `fog_mode = FOG_MODE_DEPTH`, `fog_height_density = 0.05`, `fog_aerial_perspective = 0.4`, procedural sky |
| SH-probe receiver next to a lightmap | `LightmapGI` (settings recorded, **unbaked**: baking is editor-only; the `.lmbake`, at most 4 MB, is produced in the editor and logged in `PROVENANCE.md`) and `SHReceiver` (`gi_mode = GI_MODE_DYNAMIC`, not lightmapped) |
| Multi-bounce ambient occlusion | `override.cfg` (see below) plus AO/ORM textures on the textured materials |

Eligible materials, one mesh each: the textured floor/north-wall
material (albedo, normal, ORM and AO textures, `uv1_scale`,
`uv1_offset`), `TriplanarBlock` (triplanar UV1), `AnisotropyBlock`,
`ClearcoatBlock`, `RimBlock`, and `ShaderBlock` (a `ShaderMaterial`
whose `vertex()` never writes `VERTEX`).

Every texture is a procedural `FastNoiseLite` or gradient resource
created by the generator, so the scene tracks no binary assets.

## Expected fallbacks

| Mesh | Setup | Fallback reason |
|---|---|---|
| `fallback_per_vertex` | `shading_mode = SHADING_MODE_PER_VERTEX` | `VERTEX_LIGHTING` |
| `fallback_next_pass` | surface material carries a `next_pass` shell | `NEXT_PASS_OR_OVERLAY` |
| `fallback_overlay` | `MeshInstance3D.material_overlay` set | `NEXT_PASS_OR_OVERLAY` |

The reason is also stored per mesh as `fallback_reason` metadata and
asserted by `check_features.gd`.

## Required override

`rendering/lights_and_shadows/multi_bounce_occlusion/enabled` is a
restart-only, project-wide setting. It lives in this folder's
`override.cfg`; the harness copies it to `res://override.cfg` only for
this scene and removes it afterwards. It must never go into
`project.godot`, because the NextGI baselines depend on it staying off.
For a manual run, copy the file before launching and delete it after.

## Cameras and capture

Fixed `Camera3D` nodes `cam_0`, `cam_1` and `cam_2` all sit on the
south side looking at the north wall, so every feature above is
visible from every camera. The scene root script selects the camera
from `--camera=cam_N` and forces TAA, FXAA (screen-space AA) and MSAA
off; the environment uses the linear tonemapper at exposure 1.0. The
generator is seeded (`0x6E657874`), and the scene contains no animated
content, so captures are deterministic.

On the RTX 4080 SUPER machine (Vulkan), one camera:

```
<godot> --path godotnext/testbed --rendering-driver vulkan --gpu-index 1 --gpu-validation --resolution 1920x1080 --write-movie /tmp/parity/cam0.png --fixed-fps 60 --quit-after 130 res://scenes/feature_parity_room/feature_parity_room.tscn -- --camera=cam_0
```

Repeat per camera and at `--resolution 2560x1440`. On the Apple M5 Max
use `--rendering-driver metal` with `MTL_DEBUG_LAYER=1
MTL_SHADER_VALIDATION=1` and `GODOTNEXT_BACKGROUND=1`.

## Forward baselines (PQ)

The forward baseline images at 1920×1080 and 2560×1440 on the RTX
4080 SUPER machine (Vulkan) and the Apple M5 Max (Metal) are captured
in PQ on the code-complete build. Their SHA-256 values are recorded
here then; the images themselves are harness artifacts kept outside
the repository.
