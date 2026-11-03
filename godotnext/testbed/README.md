# GodotNext testbed

The P0 testbed project (plan §13.3, §22.3). All scenes are generated;
never edit a `.tscn` or `.json` here by hand.

## Regenerate

From the repository root:

```
godot --headless --path godotnext/testbed --script res://generators/build_all.gd
```

Seeded with `0x6E657874`; two runs produce byte-identical output. The
script asserts the content-spec checks (patch counts, line of sight,
off-screen light frustum, 40 m mesh bounds) and exits non-zero on failure.
A scene file that already carries bake data is built and verified in
memory but kept as-is (see below), so a plain regenerate never drops a
bake.

## Re-bake after regenerating

Four scenes carry editor bakes (#238): `cbr_b_lightmap.tscn`,
`legacy_lightmap.tscn` and `feature_parity_room.tscn` (LightmapGI) and
`legacy_voxelgi.tscn` (VoxelGI). The generators cannot bake, but a
plain run leaves the baked `.tscn` files alone: when a target scene
already has a LightmapGI `light_data` or VoxelGI `data` reference, the
generator still builds and asserts it in memory, prints one
`kept baked <scene>` line, and still rewrites its patch JSONs.

For the deliberate regenerate-then-re-bake cycle, pass
`-- --replace-baked` (to `build_all.gd`, or to
`build_feature_parity_room.gd` for the parity room). That overwrites
the baked `.tscn` without the bake: the `light_data` reference and the
BoxMesh `add_uv2` flags go away, and the `.lmbake`/`.exr` files and
the VoxelGIData are left unreferenced. Then re-bake from the
repository root with the editor tool scene in `tests/editor_bakes/`:

```
GODOTNEXT_BACKGROUND=1 GODOTNEXT_BAKE_SCENES=1 <godot-editor> --path godotnext/testbed --rendering-driver metal --editor res://tests/editor_bakes/bake_scenes.tscn
```

The baker checks `add_uv2` on the BoxMesh resources, bakes each scene
with its recorded settings, and saves it back over its `.tscn`. Set
`GODOTNEXT_BAKE_SCENES_FILTER` to a path substring to bake one scene.
The editor rewrites `project.godot` while the project is open; restore
it afterwards (`git checkout -- godotnext/testbed/project.godot`).
Record the new bake's host, commit and date in `PROVENANCE.md`.

## Layout

- `generators/` — SceneTree generator scripts. `scaffold.gd` holds the
  shared seed plumbing, the chunked `SurfaceTool` and text writers, the
  deterministic JSON writer and the byte-stable scene saver that the
  later large-scene generators reuse.
- `generators/dense_city/` — the Dense City Blocks generator (#63), a
  separate on-demand entry point (not part of `build_all.gd`); its
  output goes to the gitignored `generated/dense_city/`. See its
  README.
- `scenes/color_box_room/` — sealed lit/dark room pairs: `cbr_a.tscn`
  (0.10 m shared wall), `cbr_b.tscn` (0.20 m), `cbr_offscreen.tscn`
  (spot behind the capture camera), `cbr_b_lightmap.tscn` (B plus a
  baked `LightmapGI`), `cbr_single_40m.tscn` (one 42.4 m mesh, 7 rooms,
  0.10 m interior walls). Lit room: red north wall, green south wall,
  0.75 gray elsewhere (linear values, stored sRGB-encoded).
- `scenes/concave_courtyard/` — single-mesh courtyard with a roofed
  colonnade, sun at 35° elevation and a procedural sky.
- `scenes/legacy/` — NextGI-off baseline halls for SDFGI, VoxelGI and
  LightmapGI. The VoxelGI and LightmapGI halls are baked (#238).
- `tests/editor_bakes/` — the editor tool scene that bakes the scenes
  above (#238). See "Re-bake after regenerating".
- `tests/tier/` — the NextGI tier check (#15). `run_tier_check.sh`
  enables NextGI for one run through `override.cfg` and runs
  `tier_check.tscn`; the project itself keeps NextGI off.
- `scenes/moving_light_door/` — two rooms, a 1 m door, and a moving
  omni light. Variants: `door_open`, `light_toggle`, `door_teleport`.
- `scenes/time_of_day/` — courtyard plus interior. Variants: `sweep`
  (90° over 10 s) and `sun_jump` (+15° at frame 0).
- `scenes/nextgi_perf/` — timing shell. Instances are spawned at
  runtime from seed 1234 (50000 by default, 100000 with
  `--instances=100000`) and are not stored in the scene.
- `scenes/feature_parity_room/` — the D7 forward-feature parity room
  (#64): decals, boxed reflection probe, textured area light, depth
  fog, an SH-probe receiver and three `fallback_*` meshes, from
  `generators/build_feature_parity_room.gd` (a separate entry point,
  not part of `build_all.gd`). See its README.
- `scenes/swrt_leak_room_10cm/` and `scenes/swrt_leak_room_20cm/` —
  sealed single rooms (#181, GS-1) whose every wall is 0.10 m or
  0.20 m thick (`wall_thickness_m` metadata), a sun outside on a
  ground slab and one emissive ceiling panel inside as the only
  interior source. Extra camera: `ExteriorCam`.
- `scenes/swrt_kitbash/` — overlap-heavy kitbash shell (#181, GS-9).
  The 2,400 rock instances (half `MeshInstance3D`, half `MultiMesh`)
  are spawned at runtime from the seeded layout (piles within 30 m)
  and are not stored in the scene.
- `scenes/swrt_runtime_mesh/` — runtime procedural mesh room (#181,
  GS-6). The scene script builds a 20,000-triangle torus `ArrayMesh`
  at frame 60 (t = 1 s) and moves it at frame 180 (t = 3 s);
  frame-driven like the other dynamic scenes, `--hold-frame` works.
- `assets/sdf_import_set/` — the GS-5 sign set (#181): 12 closed and
  4 open OBJ meshes plus `manifest.json` and the 512-ray parity
  `sign_reference.json`, from `generators/build_sdf_assets.gd` and
  `generators/sdf_sign_reference.gd` (separate entry points, not part
  of `build_all.gd`). See its README.
- `scenes/sdf_import_set/` — display scene for the sign set: the same
  16 meshes in two rows, closed gray and open orange, with the mesh
  name, `sdf_closed` flag and triangle count as node metadata.
- `scenes/swrt_content_check.gd` — headless contract checks for the
  four #181 scenes and the SDF assets (run it directly as a script).
- `scenes/exclusion_matrix/` — Exclusion Matrix (#74). Generated by
  `generators/build_exclusion_matrix.gd` (stream 74). See its README.
- `scenes/slow_io_flight/` — Slow IO Flight (#74). Generated by
  `generators/build_slow_io_flight.gd` (stream 75). See its README.
- `scenes/feature_parity_room_streaming/` — the #64 room with texture
  streaming enabled in this folder's `override.cfg` only (#74).

## Conventions

- Units are meters. Room interiors and wall thicknesses are also stored
  as node metadata (`shared_wall_thickness_m`,
  `interior_wall_thickness_m`) for external checkers.
- Cameras: `CovCam0`–`CovCam3` (Q2 coverage), `CaptureCam`, plus
  `LeakCamLit`/`LeakCamDark` in the Color Box Room scenes.
- Patch JSON sits next to its scene as `<scene>_patches_q1.json` /
  `<scene>_patches_q3.json`: `{"version", "scene", "seed", "count",
  "patches": [{"id", "pos", "normal", "radius", "node", "camera"}]}`.
  Q1 patches are discs of radius 0.25 m, at least 0.5 m apart and 0.3 m
  from surface edges, each verified by a physics ray from its camera.
  Q3 patches add `room` ("lit"/"dark"); dark patches add `mirror_of`.
- Materials are constant-parameter `StandardMaterial3D` only (Lambert,
  no textures). Quality-scene walls: roughness 1.0, metallic 0,
  `metallic_specular` 0.
- `PROVENANCE.md` lists every asset. Keep the tracked total ≤ 50 MB.
- `harness/` belongs to #12. `tests/rd_api/` belongs to #17.
  `tests/tier/` is the #15 check above; its scene is a hand-written runner,
  not generated content.
