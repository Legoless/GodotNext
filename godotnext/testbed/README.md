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
  (spot behind the capture camera), `cbr_b_lightmap.tscn` (B plus an
  unbaked `LightmapGI`), `cbr_single_40m.tscn` (one 42.4 m mesh, 7 rooms,
  0.10 m interior walls). Lit room: red north wall, green south wall,
  0.75 gray elsewhere (linear values, stored sRGB-encoded).
- `scenes/concave_courtyard/` — single-mesh courtyard with a roofed
  colonnade, sun at 35° elevation and a procedural sky.
- `scenes/legacy/` — NextGI-off baseline halls for SDFGI, VoxelGI and
  LightmapGI.

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
- `harness/` belongs to #12 and `tests/` to #17; neither lives here yet.
