# UE comparison export (schema 4)

`ue_export.gd` writes a testbed scene in a form the UE 5.9 comparison ports import (#235, #233, #232, #230, #234, plan §23.7). The UE projects, their import scripts and everything UE makes stay in the private UE comparison storage on the RTX 4080 SUPER machine. This folder holds only the GodotNext side: the exporter (MIT), this format and its check.

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
| `--camera-cuts=<frame>[,<frame>]` | no | Frames at which the scene resets the temporal history (`Viewport.reset_temporal_history()`), each from 1 to `--frames - 1`. The export cannot see the reset, so the frames are named here; the UE Level Sequence cuts there. |

Other user arguments (for example `--instances`, `--lights` or `--variant`) reach the scene as in a harness run. RT Shadow Yard and MegaLights Hall export with their `cam_main` (#233), Upscaler Torture with the camera on its path and the reset cut at frame 400 (#232), Glass & Water, Character Walk (the walk's 240 frames) and NextGI Perf with its 4 skinned characters (`--variant=p3`) with their `CaptureCam` (#230), and the PN scenes Foliage Field, Displacement Cliff and Character Crowd with their `cam_main` (#234; `generators/build_pn.gd` writes them into the gitignored `res://generated/pn_content/` first). None of the PN scenes has a camera that follows its `FlightPath`, so their exports hold the still `cam_main` a timing run renders. Their meshes are procedural ArrayMeshes without NextGeo virtual geometry, so every `nextgeo` flag is false; the #234 UE port forces Nanite on for every mesh anyway, because the NextGeo lines compare against Nanite. The settings record maps the per-mesh NextGeo opt-in to Enable Nanite Support per mesh, so that departs from the record until the lead decides (see the harness README).

```bash
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/rt_shadow_yard/rt_shadow_yard.tscn --camera=cam_main \
  --warmup=120 --frames=600 --approximate=diffuse_mode,shadow_caster_mask,translucent_shadow --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/mega_lights_hall/mega_lights_hall.tscn --camera=cam_main --lights=1024 \
  --warmup=120 --frames=600 --approximate=diffuse_mode,light_cull_mask,light_projector,translucent_shadow \
  --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/upscaler_torture/upscaler_torture.tscn --camera=CameraPath/CameraFollow/Camera3D \
  --warmup=120 --frames=600 --camera-cuts=400 \
  --approximate=alpha_antialiasing,diffuse_mode,particles,rigid_skin,shader_material --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/glass_water/glass_water.tscn --camera=CaptureCam \
  --warmup=120 --frames=600 --approximate=translucent_shadow --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/character_walk/character_walk.tscn --camera=CaptureCam --warmup=120 --frames=240 --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://scenes/nextgi_perf/nextgi_perf.tscn --camera=CaptureCam --variant=p3 --warmup=120 --frames=600 \
  --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://generated/pn_content/foliage_field.tscn --camera=cam_main --warmup=120 --frames=600 \
  --approximate=diffuse_mode --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://generated/pn_content/displacement_cliff.tscn --camera=cam_main --warmup=120 --frames=600 --out=...
godot ... --script res://tools/ue_export/ue_export.gd -- \
  --scene=res://generated/pn_content/character_crowd.tscn --camera=cam_main --warmup=120 --frames=600 --out=...
```

The scene runs as in a harness timing run. Every `_ready` runs first, so seeded generators spawn their content and the export carries their output, not their code. Then the exporter publishes frame index N through `Harness.drive_frame(N)` and samples the camera and every mesh instance after the scene has processed that frame: row N is the pose a timing run shows at measured row N. A mesh instance whose world transform changes on any row is a mover: scene.json places it at its row-0 transform and names its track file, which holds its transform on every row.

A skinned mesh is exported as a skeletal mesh (#230): its mesh, skin and skeleton go once per asset into `skinned/<id>.glb`, and each instance adds a clip there that keys every bone's pose (position, rotation and scale) on every row at row / fps seconds, with linear interpolation, so a player that samples the row times gets the rows exactly. The same numbers are in the instance's pose track, and its world transform is a mover track when it moves. The mesh must be an ArrayMesh of triangle lists with bones and weights, at most 4 weights per vertex and no blend shapes, and its Skeleton3D must sit at the mesh instance's transform. The export reads each bone's pose, so a skeleton that hands the renderer something else is refused (below). `--approximate=rigid_skin` exports it as rigid parts instead (below), as the #232 port imports Upscaler Torture's character. A `GridMap`'s cells are exported as drawn (`GridMap.get_meshes`: each cell's transform times its item's mesh transform), one entry per mesh library mesh; the GridMap's transform, visibility and drawn cells are compared with row 0 on every row like the other geometry nodes. The items' cast shadow setting is the entry's, and an alpha-blended item material on a shadow-casting item is `translucent_shadow`, as on a mesh instance.

The UE import builds default-lit constant materials: opaque, masked by the albedo alpha (alpha scissor) or alpha-blended, each with an optional albedo texture, its UV1 scale and offset and its filtering. The export stops with exit code 1, and names what it found, when the scene has content the format does not carry yet:

- lights, MultiMeshes (their node, or their instance data) or other geometry nodes that move or change while the camera path runs, a mesh instance that appears, disappears or changes visibility, or a light whose energy changes (only mesh instance transforms are exported as tracks). Each is compared with row 0 on every row, so a change that is undone before the last row is refused too;
- skinned meshes with blend shapes, non-triangle surfaces or 8 bone weights, a skinned mesh whose Skeleton3D is not at its transform, and with `rigid_skin` one whose vertices blend bones or whose triangles span bones; blend shapes and non-triangle surfaces on other meshes too;
- a skinned mesh without a `Skin` (the renderer skins it with one it makes from the skeleton's rest transforms), and a skeleton that shows the rest pose only, has a disabled bone, a bone with a skin scale, or a `SkeletonModifier3D` child: each makes the renderer skin something other than the bones' poses, which is what the export carries (`Lib.skeleton_refusals`);
- next passes or material overlays, ShaderMaterials other than spatial ones (see `shader_material` below);
- materials with vertex colors as albedo, alpha hash, alpha with a depth pre-pass, a blend mode other than mix, a shading mode other than per-pixel, a specular mode other than Schlick-GGX, front-face culling, an enabled material feature other than emission (normal mapping, rim, clearcoat, subsurface scattering and so on), or a texture in a slot other than albedo; and on a textured material, triplanar mapping, an MSDF texture, texture repeat disabled, or a texture filter other than nearest, linear or linear with mipmaps;
- materials with a depth draw mode other than opaque only, no depth test, a render priority other than 0, proximity or distance fade, a billboard mode, grow, shadow receiving, ambient light or fog disabled, or shadow to opacity;
- MultiMeshes with per-instance colors;
- a light cull mask that lights some of the placed layers but not all of them (a mask that lights none of them is the `light_cull_mask` approximation below);
- other visual nodes, such as decals, probes, fog volumes or GI nodes (particles are the `particles` approximation below);
- sky materials other than `ProceduralSkyMaterial`, a procedural sky with a sky cover texture, an `Environment` on the camera, or a camera cull mask that hides some placed layers.

Some content the UE import can only approximate. The export refuses it too, unless `--approximate` names its id; `scene.json` then lists it under `approximations` with the items it touches, the UE import acknowledges it, and the settings record lists it for the lead's sign-off (§23.4). A scene exported with an approximation is not a matched port until the lead accepts it.

| Id | Content | What the UE import builds |
| --- | --- | --- |
| `diffuse_mode` | A diffuse mode other than Lambert (Godot's Burley default) | UE's default-lit diffuse |
| `shadow_caster_mask` | A shadowed light whose `shadow_caster_mask` leaves out placed layers | A light without a caster mask: every caster shadows it. UE lights have no caster mask. |
| `light_projector` | A light projector texture | The light without its projector: Epic's light functions page (UE 5.9) does not say how a light function maps a texture onto a spot light's cone |
| `light_cull_mask` | A light whose cull mask lights none of the placed layers | The light on its own lighting channel. Epic's lighting channels page (UE 5.9) says channels do not work with Static-mobility mesh actors, and the import places its meshes Static, so in UE the light lights the placed geometry. |
| `translucent_shadow` | An alpha-blended surface on an instance that casts shadows. Godot's alpha blending casts no shadow. | The instance with Cast Shadow on, so the surface shadows UE's lights |
| `alpha_antialiasing` | Alpha antialiasing (alpha to coverage) on an alpha-scissor material | A Masked material clipped at the threshold, without alpha to coverage |
| `rigid_skin` | A skinned mesh whose every vertex follows one bone at full weight, and whose Skeleton3D sits at the mesh instance's transform, exported as rigid parts instead of the default skeletal mesh | One mesh per skin bind (the triangles bound to it), placed and moved with its bone's per-frame transform (`mesh instance world × bone global pose × bind pose`), instead of a skeletal mesh. The export compares every part with the renderer's own skinning of the same pose on every row (`MeshInstance3D.bake_mesh_from_current_skeleton_pose`) and refuses the mesh when they differ by more than 0.1 mm. The poses are the same; UE draws rigid meshes, not a skinned one. |
| `shader_material` | A spatial ShaderMaterial | The material's code, the code's SHA-256 and its uniform values are carried; the UE import builds a hand-written UE material only for a shader it knows by that SHA-256 and refuses any other |
| `particles` | GPU or CPU particles | Nothing: no public UE import path carries a Godot particle system, so the UE scene draws no particles |
| `camera_far` | Geometry that reaches past the camera's far plane inside the view on some row, which Godot clips (the view shows the sky there). The view is the camera's field of view at the project's viewport aspect; a placement counts when its bounds (its mesh's at its transform on that row) are not wholly outside a side plane and reach more than 1 mm past the far plane. | Nothing that clips it: the import sets only the camera's near plane, so UE draws that geometry and Lumen lights it. `checks.camera_far_rows` counts the rows. |

A scene that quits the tree itself before the export is written (its own `--frames` limit, for example) ends the run with exit code 1.

Exit codes: 0 written, 1 content not carried yet or the run ended early, 2 bad arguments.

## Files

| File | Content |
| --- | --- |
| `library.glb` | Every unique mesh once, as one glTF node named by its mesh id (`m0000`, …) at the origin with the identity transform. Surfaces keep their order. The glTF materials are informative only; the UE side builds its materials from `scene.json`. |
| `axes.glb` | The calibration fixture: four 0.1 m boxes whose vertices sit at the origin (`axis_o`) and 1 m along +X (`axis_x`), +Y (`axis_y`) and +Z (`axis_z`). The UE import reads the importer's axis and unit conversion from their bounds instead of assuming it. |
| `scene.json` | Everything else, described below. |
| `camera_track.csv` | `frame,px,py,pz,qx,qy,qz,qw,fov_deg,near,far`: one row per frame index from 0 to `frames - 1`, in Godot world space (meters, camera looking down -Z). |
| `tracks/t<NNNN>.csv` | One per mover: `frame,m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23`, its world transform on every row (the 3x4 matrix in the same order as scene.json transforms). Row 0 equals the instance's `transform`. |
| `skinned/<id>.glb` | One per skeletal mesh asset: a node named by the id holding the skeleton (one joint per bone, at its rest), the skinned mesh (named `<id>_mesh`) and one animation per instance named by its clip (`c0000`, …) that keys every bone's position, rotation and scale on every row at `row / fps` seconds, linearly. The glTF materials are informative only. |
| `tracks/p<NNNN>.csv` | One per skeletal instance: `frame`, then for each bone (by its index in the asset's `bones`) `b<i>_px,b<i>_py,b<i>_pz,b<i>_qx,b<i>_qy,b<i>_qz,b<i>_qw,b<i>_sx,b<i>_sy,b<i>_sz`: its local pose on every row, the numbers the clip keys. |
| `textures/<id>.png` | Every texture the export references, once per texture resource: material albedo textures, ShaderMaterial texture uniforms, rect-light textures and light projectors, as RGBA8 PNGs of the base level's pixels as stored (8-bit, sRGB for albedo). |
| `sky.hdr` | Written when the environment's sky is a `ProceduralSkyMaterial`: the sky radiance as a 512x256 Radiance HDR panorama (linear, run-length encoded RGBE), in Godot's equirectangular convention (`u = atan2(x, -z) / 2π`, `v = acos(y) / π`, the convention `PanoramaSkyMaterial` samples). It is computed on the CPU with the formula of Godot's procedural sky shader, including the glow of each directional light that lights the sky, without the debanding noise. |
| `export_manifest.json` | Schema, tool, engine version and hash, `source_commit`, the scene path and its SHA-256, the user arguments, and the SHA-256 of every file above. |

Two exports of the same scene with the same binary are byte-identical, apart from `export_manifest.json` when the arguments differ.

## scene.json

All positions are in meters in Godot world space: +Y up, right-handed, cameras and lights look down -Z. A transform is 12 numbers, the 3x4 matrix in row-major order (`world = M * [x, y, z, 1]`). Colors in `properties` are as Godot stores them (sRGB for material and light colors); the `*_linear` fields are already converted.

| Key | Content |
| --- | --- |
| `schema` | 4 (2 added blend modes, textures, area lights, layers, light masks and approximations; 3 added tracks, camera cuts, UV1 transforms, texture filtering and mipmaps, rigid skin parts, ShaderMaterials and particles; 4 added skeletal meshes and GridMap cells, and makes rigid parts opt-in) |
| `scene` | The scene's `res://` path |
| `units` | `{"length": "m", "up": "+Y", "handedness": "right", "camera_forward": "-Z", "light_forward": "-Z"}` |
| `fps`, `warmup`, `frames` | From the arguments |
| `camera` | `node`, `track` (the CSV), `fov_deg`, `fov_axis` (`vertical` for `KEEP_HEIGHT`, otherwise `horizontal`), `near`, `far`, `projection` (`perspective`), `attributes`, `cuts` (the `--camera-cuts` frames, ascending; empty without them) |
| `meshes` | Per unique mesh: `id`, `gltf_node`, `class`, `name`, `source` (the resource path when the mesh has its own file), `surfaces` (`triangles` and `vertices` each), `triangles`, `vertices`, `aabb` (`position`, `size`), `nextgeo` (the mesh carries NextGeo virtual geometry) |
| `skeletal_meshes` | Per skeletal mesh asset (one per mesh, skin and bone layout): `id` (`s0000`, …), `file` (`skinned/<id>.glb`), `name`, `surfaces`, `triangles`, `vertices`, `aabb`, `bones` (per bone `name`, `parent` index or -1, `rest` as 12 numbers), `binds` (per skin bind `bone` name and `pose` as 12 numbers) and `clips` (the clip names its instances play) |
| `materials` | Per material: `id` (`mat0000`, …, or `default` for surfaces without a material), `name`, `class`, `blend` (`opaque`, `masked` for alpha scissor, `translucent` for alpha blending, `shader` for a ShaderMaterial), and `properties` (every stored property). A `BaseMaterial3D` also has `alpha_scissor_threshold`, `two_sided` (culling disabled), `albedo_linear` (with the alpha the masked and translucent blends use), `albedo_texture` (a texture id or `null`), `uv1_scale` and `uv1_offset` (the texture coordinate is `UV × scale + offset`), `texture_filter` (`nearest`, `linear` or `linear_mipmaps`; empty on a material without an albedo texture whose filter the format does not carry, which nothing samples), `texture_repeat`, `emission_linear` (color × energy multiplier, zero when emission is off). A ShaderMaterial has `shader`: `code`, `sha256` (of the code) and `uniforms` (per uniform: `type`, `set` (false when the material leaves it at the shader's default, which is then the value), and `value`, or `texture` with a texture id). |
| `textures` | Per texture: `id` (`tex0000`, …), `file`, `class`, `name`, `size`, `mipmaps` (the texture has mipmaps in GodotNext), `usages` (`albedo`, `shader`, `area_light`, `light_projector`) |
| `instances` | One entry per `MeshInstance3D` (`kind: "node"`, its world `transform` on row 0), per `MultiMeshInstance3D` (`kind: "multimesh"`, the node's `transform`, `count`, and `instances`: each instance's world transform), per mesh a `GridMap` draws (`kind: "gridmap"`, the GridMap's `transform`, `items` (the mesh library items that show the mesh), `octant_size`, `count` and `instances` as for a MultiMesh; `layers` 1 and `gi_mode` static, as the GridMap's render instances get) and per skinned `MeshInstance3D` (`kind: "skeletal"`, `skeletal` (the asset id), `clip`, `pose_track`, and its world `transform` on row 0). All carry `node`, `materials` (one id per surface), `cast_shadow`, `gi_mode`, `visibility_range` and `layers`; all but skeletal instances `mesh`; node and skeletal instances also `track` (the mover's track file, or `null`), MultiMeshes `use_colors` and `use_custom_data`. A skinned mesh exported with `rigid_skin` gives one node instance per part with `skin_part`: `bind`, `bone` (its name) and `surfaces` (the mesh surfaces its `materials` belong to). |
| `lights` | Per directional, omni, spot or area light: `node`, `type` (`directional`, `omni`, `spot`, `area`), world `transform`, `direction` (world -Z, the direction an area light emits too), `color_linear`, `projector` (a texture id or `null`), `cull_mask_effect` (`all` or `none` of the placed layers), `caster_mask_effect` (`all`, `some` or `none`; `all` for a light without shadows), for area lights `area` (`size` in meters, `texture`, `normalize_energy`), and `properties` (every stored property) |
| `approximations` | The approximations this export carries: `id`, `description`, `items` (the material names, or the instance or light node paths). Empty without `--approximate`. |
| `environment` | The `WorldEnvironment`: `node`, `properties`, `sky` (`properties`, `material` with `class` and `properties`, and for a procedural sky `panorama`, `panorama_size` and `panorama_projection`) and `camera_attributes`; `null` without one |
| `rendering` | `project`: the project settings under `rendering/global_illumination/`, `rendering/nextgeo/`, `rendering/anti_aliasing/`, `rendering/scaling_3d/`, `rendering/lights_and_shadows/`, `rendering/environment/`, the rendering method and driver, and the default viewport size. `record_rows`: for the settings the UE settings record maps, the row of `docs/godotnext/ue-bar/settings-map.md` that maps them. |
| `checks` | Values the UE import reproduces and checks: `instances_total` (node, MultiMesh, GridMap and skeletal placements), `node_instances`, `multimesh_instances`, `multimesh_nodes`, `gridmap_cells`, `gridmap_entries`, `skeletal_instances`, `skeletal_meshes`, `meshes`, `materials`, `lights`, `light_types` (count per type), `textures`, `triangles_total` (summed over placements), `aabb` (`min`, `max`), `instances_sha256` (every placement's 12 numbers as float32, in file order), `positions_sha256` (their origins as a `PackedVector3Array`), `camera_rows`, `movers` (instances with a track), `rigid_skin_max_error_m` (the largest distance between a rigid part's vertex and the renderer's skinning of it, over every row; 0 without skinned meshes), `rigid_skin_vertices_compared` (how many part vertices that comparison measured, summed over the rows), `camera_far_rows` (the camera rows on which a placement reaches past the far plane inside the view; `camera_far` is listed exactly when it is not 0) |

The project settings are the ones in effect for the export run, including a per-run `override.cfg`. The comparison job decides the settings of a timing run; the UE side takes its settings from the settings record, not from this block.

## The check

`godotnext/testbed/tests/ue_export/run_ue_export_check.sh <godot> [godot arguments]` refuses a headless export, makes two rendered exports each of the overlap-heavy kitbash, RT Shadow Yard (with `--approximate=diffuse_mode,shadow_caster_mask,translucent_shadow`), MegaLights Hall at 1,024 lights (with `--approximate=diffuse_mode,light_cull_mask,light_projector,translucent_shadow`), Upscaler Torture (600 frames, `--camera-cuts=400`, with `--approximate=alpha_antialiasing,diffuse_mode,particles,rigid_skin,shader_material`), Glass & Water (60 frames, with `--approximate=translucent_shadow`), Character Walk (the walk's 240 frames), NextGI Perf (`--instances=180 --variant=p3`, 120 frames) and, after generating them with `generators/build_pn.gd`, Foliage Field (with `--approximate=diffuse_mode`), Displacement Cliff and Character Crowd (60 frames each), checks that the yard without `--approximate` refuses and names its three ids, that the hall without `light_cull_mask` refuses and names it, that the torture without `--approximate` refuses and names its four ids other than `rigid_skin` (its character is a skeletal mesh by default), that Glass & Water without `--approximate` refuses and names `translucent_shadow`, that Foliage Field without `--approximate` refuses and names `diffuse_mode`, that an unknown id and a cut outside the path exit 2 and that a run ended after 3 frames exits 1 and writes nothing, that each torture export log resets the temporal history once, at the `--camera-cuts` frame, and runs `check_ue_export.gd` on each pair. On `tests/ue_export/motion_fixture.tscn` it checks the per-row refusals: the still fixture exports, and a light that moves and returns, a mesh hidden for one frame and a MultiMesh instance that moves and returns each refuse with the node and row 8 named, as does a GridMap cell that moves for one frame, as do a Skeleton3D beside its skinned mesh (as rigid parts and as a skeletal mesh), a camera cull mask that hides a placed layer and a bone skin scale; without `rigid_skin` the still fixture exports its skinned triangle as one skeletal instance, and refuses a skinned mesh without a Skin, a bone skin scale, show_rest_only, a disabled bone, a SkeletonModifier3D, an alpha-blended GridMap item that casts shadows (naming `translucent_shadow`), two GridMap items sharing a mesh with different shadow settings, and a 5 m far plane (naming `camera_far`, with which it exports). Two still fixture exports go through `check_ue_export.gd fixture`, and two with a box that moves past a 20 m far plane on one row (`mover_far`, with `camera_far`). The check covers:

- The material refusals (one unit check per refused flag) and approximations, the carried UV1 transforms and texture filters (each filter carried or refused), the light-mask effects, the rigid-part split (two bones split into two parts that keep their vertices; blended weights, triangles across bones, blend shapes, a triangle strip and a mesh other than an ArrayMesh refused) and its comparison with skinned positions (exact part transforms match; a bind pose 1 cm off, a missing part or a missing bake does not), an untextured material's filter left empty, and the texture PNGs: a non-uniform RGBA texture with mipmaps and a hand-made DXT1 block come back as their known texels.
- The two exports of a scene are byte-identical, and the manifest checksums match.
- Every `scene.json` key above is present with its type, a shader material's SHA-256 is its code's, and the `checks` agree with the instance rows (including `movers` and the camera cuts).
- The 2,400 rock placements equal the seeded layout's plan, with the MultiMesh half included, and hash to the scene's `positions_sha256`.
- `library.glb` re-imports in Godot with each mesh's triangle count and bounds, and `axes.glb` with its marker positions.
- Every camera row is the scene camera's pose and field of view.
- Every texture is a PNG of its stated size, byte-identical to the PNG of the scene's own texture (the albedo of a mesh instance, skeletal instance or MultiMesh material, a shader uniform, a rect light or a projector; a `NoiseTexture2D` once it has made its image), and every texture id was compared.
- `sky.hdr` holds the procedural sky within RGBE precision; a scene without a sky has none.
- Every light that lights no placed layer is named by `light_cull_mask`, and every shadow-casting instance with an alpha-blended surface by `translucent_shadow`.
- RT Shadow Yard: the sun and 64 local lights by type, the 34 placed meshes, each occluder on its own layer, the 20 matrix lights' caster masks (`some`) and the approximations that name them, the masked foliage (textured, two-sided, clipped at 0.5) and the translucent glass (opacity 0.35), whose 3 panes `translucent_shadow` names.
- MegaLights Hall at 1,024 lights: the sun and 1,024 local lights by type (1,004 omnis, 12 spots, 8 rect lights), the four textured rect lights sharing one texture, the four projector spots named by `light_projector`, the two lighting-channel omnis lighting no placed layer and named by `light_cull_mask`, the 4 glass panes named by `translucent_shadow`, the volumetric fog, and the material blends.
- `Lib.skeletal_refusals`: a skinned triangle list is carried; a mesh other than an ArrayMesh, blend shapes, a triangle strip, 8 bone weights and a mesh without bones or weights are each refused with their own message. `Lib.skeleton_refusals`: a plain skeleton is carried; show_rest_only, a disabled bone, a skin scale and a SkeletonModifier3D child are each refused.
- `Lib.far_planes` and `Lib.box_past_far`: a box past or across the far plane inside the view counts; a nearer box, a box behind the camera, a box past the far depth outside the side planes and one within the 1 mm tolerance do not; the side planes follow the keep-aspect mode and the camera's orientation.
- `checks.camera_far_rows` equals the count recomputed from the export's own files (the camera rows of `camera_track.csv`, and every placement's mesh bounds at its transform or at each row of its track), and `camera_far` names the camera exactly when the count is not 0.
- Every skeletal mesh: `skinned/<id>.glb` re-imports with the asset's bones (names, parents, rests), its skin's binds and the scene mesh's triangles, positions, bone indices and weights corner by corner, and each instance's clip, sampled at `row / fps`, holds its pose track on every row (rotations within 1e-4, tracks the importer drops as constant at the bone's rest).
- Glass & Water: 13 placed meshes (the hidden opaque fence left out), the sun and the omni at the energy its script sets from frame 0 (8; the scene stores 0) with its shadow, the plaster, glass (opacity 0.35), water (0.6) and scissor fence (clipped at 0.5) materials, nothing moving, and `translucent_shadow` naming the two panes and the water.
- Character Walk: the floor, the wall and the walker as one skeletal instance, the only mover; its world track equal to the walk's path (x 3.6 to 0.4 over 240 frames) and its pose track equal to the walk clip at `frame / 60` s wrapped at the 1 s cycle, recomputed from the scene's own animation.
- The still motion fixture: the box, the three MultiMesh boxes and the skeletal triangle; both GridMaps' cells equal to transforms written out from each GridMap's settings (the GridMap's transform, the cell's orientation scaled by the cell scale at cell × cell size plus half a cell on each centered axis, and the item's mesh transform), where `GridTurned` has a rotated node, non-cubic cells centered on Y and Z only, a 0.8 cell scale, an item mesh transform and four orientations; with `mover_far`, `camera_far` on exactly the one row the moving box reaches past the far plane.
- NextGI Perf: no approximation and no row past CaptureCam's 150 m far plane; every seeded placement equal to the plan for the export's `--instances` count, in its feed (MeshInstance3D nodes with the 0.4 m box, the 7 MultiMeshes in plan order, and the GridMap cells, which draw the mesh library's 0.8 m box), the 12 room boxes, the sun, 16 omnis and the off-screen spot, every camera row on the layout's spline, and with `--variant=p3` the 4 characters as skeletal instances whose world tracks follow the spline (`perf_position`, facing the travel) and whose pose tracks are the walk clip at their phases.
- Foliage Field: the ground and the two MultiMeshes, 100,000 trees and 100,000 grass cards, every placement equal to the generator's seeded field transforms; the trees' two surfaces both drawn with the node's masked leaf material override (textured, two-sided, clipped at 0.5), the grass masked and two-sided without a texture, the 64 x 64 leaf texture, nothing moving, and `diffuse_mode` naming the two materials.
- Displacement Cliff: the heightfield's 65,536 triangles and the floor, the opaque rock with the seamless 512 x 512 noise texture (mipmapped) tiled 24 times and filtered linearly with mipmaps, nothing moving and no approximation.
- Character Crowd: the ground and the 100 walkers as skeletal instances of one asset (at least 500,000 triangles, one clip per walker) at the generator's grid places and seeded facings, none moving, each pose track the walk cycle at the row's frame plus the walker's phase (index x 3 frames).
- Upscaler Torture: 55 placed meshes, the 10 movers (the fan's blades and hub, the character's 5 rigid parts) and nothing else moving, each track row equal at frames 0 to 599 (12 of them) to the pose recomputed from the scene's nodes, the occluder slide and fan rotation of the timeline and, for the character, the animation's rotation keys on the skeleton's stored poses; the rigid parts within 0.1 mm of the renderer's skinning, compared for every part vertex on every row; every camera row equal to the pose recomputed from the camera curve at the timeline's progress and look-at target, still through frames 0 to 199, with the only jump at the cut (frame 400); the fence, checker and moiré materials' blends, UV1 scales, filters and mipmaps; the scrolling emissive's shader code, SHA-256 and uniforms; and the five approximations with their items.
