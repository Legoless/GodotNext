# UE comparison export (schema 1)

`ue_export.gd` writes a testbed scene in a form the UE 5.9 comparison ports import (#235, plan §23.7). The UE projects, their import scripts and everything UE makes stay in the private UE comparison storage on the RTX 4080 SUPER machine. This folder holds only the GodotNext side: the exporter (MIT), this format and its check.

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

Other user arguments (for example `--instances`) reach the scene as in a harness run.

The scene runs as in a harness timing run. Every `_ready` runs first, so seeded generators spawn their content and the export carries their output, not their code. Then the exporter publishes frame index N through `Harness.drive_frame(N)` and samples the camera after the scene has processed that frame: row N is the pose a timing run shows at measured row N.

The format carries static content only for now. The export stops with exit code 1, and names what it found, when the scene has content the format does not carry yet:

- geometry or lights that move or change while the camera path runs;
- skinned meshes, blend shapes, or non-triangle surfaces;
- textured materials, ShaderMaterials, next passes or material overlays;
- materials with vertex colors as albedo, transparency (including alpha scissor and alpha hash), a diffuse mode other than Lambert, or front-face culling (the UE import builds opaque, default-lit constant materials);
- MultiMeshes with per-instance colors;
- other visual nodes, such as particles, decals, probes, fog volumes or GI nodes;
- sky materials other than `ProceduralSkyMaterial`, a procedural sky with a sky cover texture, or an `Environment` on the camera.

Exit codes: 0 written, 1 content not carried yet, 2 bad arguments.

## Files

| File | Content |
| --- | --- |
| `library.glb` | Every unique mesh once, as one glTF node named by its mesh id (`m0000`, …) at the origin with the identity transform. Surfaces keep their order. The glTF materials are informative only; the UE side builds its materials from `scene.json`. |
| `axes.glb` | The calibration fixture: four 0.1 m boxes whose vertices sit at the origin (`axis_o`) and 1 m along +X (`axis_x`), +Y (`axis_y`) and +Z (`axis_z`). The UE import reads the importer's axis and unit conversion from their bounds instead of assuming it. |
| `scene.json` | Everything else, described below. |
| `camera_track.csv` | `frame,px,py,pz,qx,qy,qz,qw,fov_deg,near,far`: one row per frame index from 0 to `frames - 1`, in Godot world space (meters, camera looking down -Z). |
| `sky.hdr` | Written when the environment's sky is a `ProceduralSkyMaterial`: the sky radiance as a 512x256 Radiance HDR panorama (linear, run-length encoded RGBE), in Godot's equirectangular convention (`u = atan2(x, -z) / 2π`, `v = acos(y) / π`, the convention `PanoramaSkyMaterial` samples). It is computed on the CPU with the formula of Godot's procedural sky shader, including the glow of each directional light that lights the sky, without the debanding noise. |
| `export_manifest.json` | Schema, tool, engine version and hash, `source_commit`, the scene path and its SHA-256, the user arguments, and the SHA-256 of every file above. |

Two exports of the same scene with the same binary are byte-identical, apart from `export_manifest.json` when the arguments differ.

## scene.json

All positions are in meters in Godot world space: +Y up, right-handed, cameras and lights look down -Z. A transform is 12 numbers, the 3x4 matrix in row-major order (`world = M * [x, y, z, 1]`). Colors in `properties` are as Godot stores them (sRGB for material and light colors); the `*_linear` fields are already converted.

| Key | Content |
| --- | --- |
| `schema` | 1 |
| `scene` | The scene's `res://` path |
| `units` | `{"length": "m", "up": "+Y", "handedness": "right", "camera_forward": "-Z", "light_forward": "-Z"}` |
| `fps`, `warmup`, `frames` | From the arguments |
| `camera` | `node`, `track` (the CSV), `fov_deg`, `fov_axis` (`vertical` for `KEEP_HEIGHT`, otherwise `horizontal`), `near`, `far`, `projection` (`perspective`), `attributes` |
| `meshes` | Per unique mesh: `id`, `gltf_node`, `class`, `name`, `source` (the resource path when the mesh has its own file), `surfaces` (`triangles` and `vertices` each), `triangles`, `vertices`, `aabb` (`position`, `size`), `nextgeo` (the mesh carries NextGeo virtual geometry) |
| `materials` | Per material: `id` (`mat0000`, …, or `default` for surfaces without a material), `name`, `class`, `albedo_linear`, `emission_linear` (color × energy multiplier, zero when emission is off), `properties` (every stored property) |
| `instances` | One entry per `MeshInstance3D` (`kind: "node"`, its world `transform`) and per `MultiMeshInstance3D` (`kind: "multimesh"`, the node's `transform`, `count`, and `instances`: each instance's world transform). Both carry `node`, `mesh`, `materials` (one id per surface), `cast_shadow`, `gi_mode` and `visibility_range`; MultiMeshes also `use_colors` and `use_custom_data`. |
| `lights` | Per directional, omni or spot light: `node`, `type`, world `transform`, `direction` (world -Z), `color_linear`, `properties` (every stored property) |
| `environment` | The `WorldEnvironment`: `node`, `properties`, `sky` (`properties`, `material` with `class` and `properties`, and for a procedural sky `panorama`, `panorama_size` and `panorama_projection`) and `camera_attributes`; `null` without one |
| `rendering` | `project`: the project settings under `rendering/global_illumination/`, `rendering/nextgeo/`, `rendering/anti_aliasing/`, `rendering/scaling_3d/`, `rendering/lights_and_shadows/`, `rendering/environment/`, the rendering method and driver, and the default viewport size. `record_rows`: for the settings the UE settings record maps, the row of `docs/godotnext/ue-bar/settings-map.md` that maps them. |
| `checks` | Values the UE import reproduces and checks: `instances_total`, `node_instances`, `multimesh_instances`, `multimesh_nodes`, `meshes`, `materials`, `lights`, `triangles_total` (summed over placements), `aabb` (`min`, `max`), `instances_sha256` (every placement's 12 numbers as float32, in file order), `positions_sha256` (their origins as a `PackedVector3Array`), `camera_rows` |

The project settings are the ones in effect for the export run, including a per-run `override.cfg`. The comparison job decides the settings of a timing run; the UE side takes its settings from the settings record, not from this block.

## The check

`godotnext/testbed/tests/ue_export/run_ue_export_check.sh <godot> [godot arguments]` refuses a headless export, makes two rendered exports of the overlap-heavy kitbash and runs `check_ue_export.gd` on them. The check covers:

- The two exports are byte-identical, and the manifest checksums match.
- Every `scene.json` key above is present with its type, and the `checks` agree with the instance rows.
- The 2,400 rock placements equal the seeded layout's plan, with the MultiMesh half included, and hash to the scene's `positions_sha256`.
- `library.glb` re-imports in Godot with each mesh's triangle count and bounds, and `axes.glb` with its marker positions.
- Every camera row is `CaptureCam`'s pose and field of view.
- `sky.hdr` holds the procedural sky within RGBE precision.
