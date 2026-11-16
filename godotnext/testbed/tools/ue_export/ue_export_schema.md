# UE comparison export (schema 2)

`ue_export.gd` writes a testbed scene in a form the UE 5.9 comparison ports import (#235, #233, plan §23.7). The UE projects, their import scripts and everything UE makes stay in the private UE comparison storage on the RTX 4080 SUPER machine. This folder holds only the GodotNext side: the exporter (MIT), this format and its check.

## Running it

Run it rendered, not with `--headless`. The dummy renderer keeps no MultiMesh instance transforms, so a headless export would lose every MultiMesh placement; the exporter refuses `--headless` with exit code 2. On a Linux machine without a desktop, point `DISPLAY` at an X server (the cloud L4 VMs use `DISPLAY=:0`).

```bash
godot --path godotnext/testbed --rendering-driver vulkan \
  --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/swrt_kitbash/swrt_kitbash.tscn --camera=CaptureCam \
  --warmup=120 --frames=600 --out=/absolute/output/dir \
  --source-commit="$(git rev-parse HEAD)"
```

| Flag | Required | Meaning |
| --- | --- | --- |
| `--scene=res://…` | yes | The scene to export |
| `--camera=<name or path>` | yes | The camera whose path is sampled |
| `--frames=<int>` | yes | Rows of the camera path: the timing run's measured window |
| `--warmup=<int>` | no, 0 | The timing run's warm-up, recorded only: a timing run holds frame 0 through it |
| `--fps=<int>` | no, 60 | The frame rate the UE Level Sequence plays the rows at |
| `--out=<absolute dir>` | yes | Output folder |
| `--source-commit=<sha>` | no | Recorded in the manifest |
| `--approximate=<id>[,<id>]` | no | Exports the named approximations (below) instead of refusing them |

Other user arguments (for example `--instances` or `--lights`) reach the scene as in a harness run. RT Shadow Yard and MegaLights Hall export with their `cam_main` (#233):

```bash
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/rt_shadow_yard/rt_shadow_yard.tscn --camera=cam_main \
  --warmup=120 --frames=600 --approximate=diffuse_mode,shadow_caster_mask,translucent_shadow --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/mega_lights_hall/mega_lights_hall.tscn --camera=cam_main --lights=1024 \
  --warmup=120 --frames=600 --approximate=diffuse_mode,light_cull_mask,light_projector,translucent_shadow \
  --out=...
```

The scene runs as in a harness timing run. Every `_ready` runs first, so seeded generators spawn their content and the export carries their output, not their code. Then the exporter publishes frame index N through `Harness.drive_frame(N)` and samples the camera after the scene has processed that frame: row N is the pose a timing run shows at measured row N.

The format carries static content only for now. The UE import builds default-lit constant materials: opaque, masked by the albedo alpha (alpha scissor) or alpha-blended, each with an optional albedo texture. The export stops with exit code 1, and names what it found, when the scene has content the format does not carry yet:

- geometry or lights that move or change while the camera path runs;
- skinned meshes, blend shapes, or non-triangle surfaces;
- ShaderMaterials, next passes or material overlays;
- materials with vertex colors as albedo, alpha hash, alpha with a depth pre-pass, alpha antialiasing, a blend mode other than mix, a shading mode other than per-pixel, a specular mode other than Schlick-GGX, front-face culling, an enabled material feature other than emission (normal mapping, rim, clearcoat, subsurface scattering and so on), a texture in a slot other than albedo, or a UV1 transform or triplanar mapping on a textured material;
- materials with a depth draw mode other than opaque only, no depth test, a render priority other than 0, proximity or distance fade, a billboard mode, grow, shadow receiving, ambient light or fog disabled, or shadow to opacity;
- MultiMeshes with per-instance colors;
- a light cull mask that lights some of the placed layers but not all of them (a mask that lights none of them is the `light_cull_mask` approximation below);
- other visual nodes, such as particles, decals, probes, fog volumes or GI nodes;
- sky materials other than `ProceduralSkyMaterial`, a procedural sky with a sky cover texture, or an `Environment` on the camera.

Some content the UE import can only approximate. The export refuses it too, unless `--approximate` names its id; `scene.json` then lists it under `approximations` with the items it touches, the UE import acknowledges it, and the settings record lists it for the lead's sign-off (§23.4). A scene exported with an approximation is not a matched port until the lead accepts it.

| Id | Content | What the UE import builds |
| --- | --- | --- |
| `diffuse_mode` | A diffuse mode other than Lambert (Godot's Burley default) | UE's default-lit diffuse |
| `shadow_caster_mask` | A shadowed light whose `shadow_caster_mask` leaves out placed layers | A light without a caster mask: every caster shadows it. UE lights have no caster mask. |
| `light_projector` | A light projector texture | The light without its projector: Epic's light functions page (UE 5.9) does not say how a light function maps a texture onto a spot light's cone |
| `light_cull_mask` | A light whose cull mask lights none of the placed layers | The light on its own lighting channel. Epic's lighting channels page (UE 5.9) says channels do not work with Static-mobility mesh actors, and the import places its meshes Static, so in UE the light lights the placed geometry. |
| `translucent_shadow` | An alpha-blended surface on an instance that casts shadows. Godot's alpha blending casts no shadow. | The instance with Cast Shadow on, so the surface shadows UE's lights |

A scene that quits the tree itself before the export is written (its own `--frames` limit, for example) ends the run with exit code 1.

Exit codes: 0 written, 1 content not carried yet or the run ended early, 2 bad arguments.

## Files

| File | Content |
| --- | --- |
| `library.glb` | Every unique mesh once, as one glTF node named by its mesh id (`m0000`, …) at the origin with the identity transform. Surfaces keep their order. The glTF materials are informative only; the UE side builds its materials from `scene.json`. |
| `axes.glb` | The calibration fixture: four 0.1 m boxes whose vertices sit at the origin (`axis_o`) and 1 m along +X (`axis_x`), +Y (`axis_y`) and +Z (`axis_z`). The UE import reads the importer's axis and unit conversion from their bounds instead of assuming it. |
| `scene.json` | Everything else, described below. |
| `camera_track.csv` | `frame,px,py,pz,qx,qy,qz,qw,fov_deg,near,far`: one row per frame index from 0 to `frames - 1`, in Godot world space (meters, camera looking down -Z). |
| `textures/<id>.png` | Every texture the export references, once per texture resource: material albedo textures, rect-light textures and light projectors, as RGBA8 PNGs of the pixels as stored (8-bit, sRGB for albedo). |
| `sky.hdr` | Written when the environment's sky is a `ProceduralSkyMaterial`: the sky radiance as a 512x256 Radiance HDR panorama (linear, run-length encoded RGBE), in Godot's equirectangular convention (`u = atan2(x, -z) / 2π`, `v = acos(y) / π`, the convention `PanoramaSkyMaterial` samples). It is computed on the CPU with the formula of Godot's procedural sky shader, including the glow of each directional light that lights the sky, without the debanding noise. |
| `export_manifest.json` | Schema, tool, engine version and hash, `source_commit`, the scene path and its SHA-256, the user arguments, and the SHA-256 of every file above. |

Two exports of the same scene with the same binary are byte-identical, apart from `export_manifest.json` when the arguments differ.

## scene.json

All positions are in meters in Godot world space: +Y up, right-handed, cameras and lights look down -Z. A transform is 12 numbers, the 3x4 matrix in row-major order (`world = M * [x, y, z, 1]`). Colors in `properties` are as Godot stores them (sRGB for material and light colors); the `*_linear` fields are already converted.

| Key | Content |
| --- | --- |
| `schema` | 2 |
| `scene` | The scene's `res://` path |
| `units` | `{"length": "m", "up": "+Y", "handedness": "right", "camera_forward": "-Z", "light_forward": "-Z"}` |
| `fps`, `warmup`, `frames` | From the arguments |
| `camera` | `node`, `track` (the CSV), `fov_deg`, `fov_axis` (`vertical` for `KEEP_HEIGHT`, otherwise `horizontal`), `near`, `far`, `projection` (`perspective`), `attributes` |
| `meshes` | Per unique mesh: `id`, `gltf_node`, `class`, `name`, `source` (the resource path when the mesh has its own file), `surfaces` (`triangles` and `vertices` each), `triangles`, `vertices`, `aabb` (`position`, `size`), `nextgeo` (the mesh carries NextGeo virtual geometry) |
| `materials` | Per material: `id` (`mat0000`, …, or `default` for surfaces without a material), `name`, `class`, `blend` (`opaque`, `masked` for alpha scissor, `translucent` for alpha blending), `alpha_scissor_threshold`, `two_sided` (culling disabled), `albedo_linear` (with the alpha the masked and translucent blends use), `albedo_texture` (a texture id or `null`), `emission_linear` (color × energy multiplier, zero when emission is off), `properties` (every stored property) |
| `textures` | Per texture: `id` (`tex0000`, …), `file`, `class`, `name`, `size`, `usages` (`albedo`, `area_light`, `light_projector`) |
| `instances` | One entry per `MeshInstance3D` (`kind: "node"`, its world `transform`) and per `MultiMeshInstance3D` (`kind: "multimesh"`, the node's `transform`, `count`, and `instances`: each instance's world transform). Both carry `node`, `mesh`, `materials` (one id per surface), `cast_shadow`, `gi_mode`, `visibility_range` and `layers`; MultiMeshes also `use_colors` and `use_custom_data`. |
| `lights` | Per directional, omni, spot or area light: `node`, `type` (`directional`, `omni`, `spot`, `area`), world `transform`, `direction` (world -Z, the direction an area light emits too), `color_linear`, `projector` (a texture id or `null`), `cull_mask_effect` (`all` or `none` of the placed layers), `caster_mask_effect` (`all`, `some` or `none`; `all` for a light without shadows), for area lights `area` (`size` in meters, `texture`, `normalize_energy`), and `properties` (every stored property) |
| `approximations` | The approximations this export carries: `id`, `description`, `items` (the material names, or the instance or light node paths). Empty without `--approximate`. |
| `environment` | The `WorldEnvironment`: `node`, `properties`, `sky` (`properties`, `material` with `class` and `properties`, and for a procedural sky `panorama`, `panorama_size` and `panorama_projection`) and `camera_attributes`; `null` without one |
| `rendering` | `project`: the project settings under `rendering/global_illumination/`, `rendering/nextgeo/`, `rendering/anti_aliasing/`, `rendering/scaling_3d/`, `rendering/lights_and_shadows/`, `rendering/environment/`, the rendering method and driver, and the default viewport size. `record_rows`: for the settings the UE settings record maps, the row of `docs/godotnext/ue-bar/settings-map.md` that maps them. |
| `checks` | Values the UE import reproduces and checks: `instances_total`, `node_instances`, `multimesh_instances`, `multimesh_nodes`, `meshes`, `materials`, `lights`, `light_types` (count per type), `textures`, `triangles_total` (summed over placements), `aabb` (`min`, `max`), `instances_sha256` (every placement's 12 numbers as float32, in file order), `positions_sha256` (their origins as a `PackedVector3Array`), `camera_rows` |

The project settings are the ones in effect for the export run, including a per-run `override.cfg`. The comparison job decides the settings of a timing run; the UE side takes its settings from the settings record, not from this block.

## The check

`godotnext/testbed/tests/ue_export/run_ue_export_check.sh <godot> [godot arguments]` refuses a headless export, makes two rendered exports each of the overlap-heavy kitbash, RT Shadow Yard (with `--approximate=diffuse_mode,shadow_caster_mask,translucent_shadow`) and MegaLights Hall at 1,024 lights (with `--approximate=diffuse_mode,light_cull_mask,light_projector,translucent_shadow`), checks that the yard without `--approximate` refuses and names its three ids, that the hall without `light_cull_mask` refuses and names it, that an unknown id exits 2 and that a run ended after 3 frames exits 1 and writes nothing, and runs `check_ue_export.gd` on each pair. The check covers:

- The material refusals (one unit check per refused flag) and approximations, the light-mask effects, and the texture PNGs: a non-uniform RGBA texture with mipmaps and a hand-made DXT1 block come back as their known texels.
- The two exports of a scene are byte-identical, and the manifest checksums match.
- Every `scene.json` key above is present with its type, and the `checks` agree with the instance rows.
- The 2,400 rock placements equal the seeded layout's plan, with the MultiMesh half included, and hash to the scene's `positions_sha256`.
- `library.glb` re-imports in Godot with each mesh's triangle count and bounds, and `axes.glb` with its marker positions.
- Every camera row is the scene camera's pose and field of view.
- Every texture is a PNG of its stated size, byte-identical to the PNG of the scene's own texture, and every texture id was compared.
- `sky.hdr` holds the procedural sky within RGBE precision.
- Every light that lights no placed layer is named by `light_cull_mask`, and every shadow-casting instance with an alpha-blended surface by `translucent_shadow`.
- RT Shadow Yard: the sun and 64 local lights by type, the 34 placed meshes, each occluder on its own layer, the 20 matrix lights' caster masks (`some`) and the approximations that name them, the masked foliage (textured, two-sided, clipped at 0.5) and the translucent glass (opacity 0.35), whose 3 panes `translucent_shadow` names.
- MegaLights Hall at 1,024 lights: the sun and 1,024 local lights by type (1,004 omnis, 12 spots, 8 rect lights), the four textured rect lights sharing one texture, the four projector spots named by `light_projector`, the two lighting-channel omnis lighting no placed layer and named by `light_cull_mask`, the 4 glass panes named by `translucent_shadow`, the volumetric fog, and the material blends.
