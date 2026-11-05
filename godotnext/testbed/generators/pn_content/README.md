# PN content scenes

The PN gate scenes (#148, plan §21.4.3). Original procedural content only;
every row's licence is MIT in `../../PROVENANCE.md`. The heavy output is
generated on demand into the gitignored `res://generated/pn_content/`
folder, like Dense City, so the testbed stays small in git.

## Generate

From the repository root:

```
godot --headless --path godotnext/testbed --script res://generators/build_pn.gd
godot --headless --path godotnext/testbed --import
godot --headless --path godotnext/testbed --script res://generators/validate_pn_content.gd
```

Micro Detail comes from the Dense City generator (variant `micro_detail`):

```
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --variant=micro_detail
godot --headless --path godotnext/testbed --import
godot --headless --path godotnext/testbed --script res://generators/dense_city/validate_dense_city.gd -- --variant=micro_detail
```

Three runs of either generator with the same seed give byte-identical
`.tscn`, `.res` and `.glb` output on the M5 Max.

## Scenes

| Scene | Content | Cameras |
|---|---|---|
| `generated/dense_city/micro_detail.tscn` | Dense City with 4× the facade triangle count: 40.98M unique, 587.47M instanced triangles; the deterministic 1080p estimate puts 99.4% of visible triangles under 1 px² | `cam_main`, `cam_aerial`, `FlightPath` |
| `generated/pn_content/foliage_field.tscn` | 100k trees + 100k grass cards, masked leaf material, one MultiMesh per part | `cam_main`, `cam_canopy`, `cam_ground`, `cam_far`, `FlightPath` |
| `generated/pn_content/foliage_field_assemblies.tscn` | The same field with each tree split into trunk and leaf MultiMeshInstances | the same |
| `generated/pn_content/foliage_field_voxels.tscn` | The plain field tagged `voxel_lod=true` for the GN-7 comparison | the same |
| `generated/pn_content/character_crowd.tscn` | 100 instances of the P3 CC0 walker on a 10×10 grid, sharing one 561,152-triangle mesh, seeded walk phases | `cam_main`, `cam_side`, `cam_high`, `cam_close`, `FlightPath` |
| `generated/pn_content/displacement_cliff.tscn` | 200 m heightfield cliff (65,536 triangles) with a tiling noise height texture | `cam_main`, `cam_detail`, `cam_wide`, `cam_ground`, `FlightPath` |
| `generated/pn_content/spline_road.tscn` | 2 km road and guard rails along a stored `Curve3D` | `cam_main`, `cam_mid`, `cam_low`, `cam_end`, `FlightPath` |

## Harness

The harness takes a scene path at run time; there is no registry file to
edit. Capture mode:

```
godotnext/testbed/harness/runners/macos_local.sh -- \
  ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal \
  --write-movie "$OUT/frame.png" --fixed-fps 60 --quit-after 1062 -- \
  --mode=capture --scene=res://generated/pn_content/foliage_field.tscn \
  --camera=cam_main --out="$OUT" --seed=1 --frames=8 --warmup=4 --capture-frames=4,7
```

Timing mode uses the same `--scene` and `--camera` with `--mode=timing`.
Micro Detail is the PN phase smoke scene (§22.3): render it with Metal
validation before landing any PN change.
