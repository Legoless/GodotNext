# Dense City Blocks generator

The seeded Dense City Blocks generator (#63; plan §13.3, §2 row 1, gates
G4-5/G5-1 measured in PQ). It is a separate entry point from
`generators/build_all.gd`: the P0 scenes are small and tracked, while this
generator writes gigabytes of generated output to the gitignored
`res://generated/dense_city/` folder, so it runs on demand.

## Generate

From the repository root, one variant per run (each run is seeded,
asserts its scale counters, and updates `expected.json`):

```
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=virtual
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=lod_baseline
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=sweep_10m
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=sweep_100m
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=sweep_1b
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=kit --materials=8
godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=stress_10m
godot --headless --path godotnext/testbed --import
```

Arguments after `--`: `--seed=<int>` (default 1), `--variant=...` (above),
`--materials=<n>` (kit variant surface/material count, default 8, for
#71), `--out=res://generated/dense_city`. Targets (instance and triangle
minima, the provisional 128 MiB geometry-cache budget, sweep totals) live
in `variants.cfg`.

Outputs under `generated/dense_city/`:

| Path | Content |
|---|---|
| `dense_city.tscn` | The virtual-geometry city: 5,040 building/prop instances of the `kit/` imports, sun with 4 PSSM splits, 4 shadowed omni + 4 shadowed spot street lights, `cam_main` (street level, current), `cam_aerial`, `FlightPath` |
| `kit/*.glb` + `.glb.import` | 9 kit meshes (10.28M unique triangles total), imported with `meshes/generate_lods=false`, `meshes/create_shadow_meshes=false`, `meshes/generate_virtual_geometry=true` |
| `dense_city_lod_baseline.tscn` + `kit_lod/` | The same scene and byte-identical instance transforms, on separate copies of the kit imported with discrete LODs and shadow meshes on, virtual geometry off |
| `dense_city_sweep_10m.tscn`, `..._100m.tscn`, `..._1b.tscn` | The same kit instanced to 10M / 100M / 1B instanced triangles (within 5%; measured: within 0.01%) |
| `kit_asset/kit_facade.glb` | The multi-material kit asset alone (G4-2 / #71): one surface per material, neighbors sharing coincident seam loops |
| `stress/stress_10m.glb`, `stress/stress_10m.obj` | One single-surface 10M-triangle mesh (the G4-2 import stress asset) |

Only this folder's scripts, `variants.cfg`, this README and
`expected.json` are tracked (under 1 MB); generated output is created on
demand. That keeps the testbed's 50 MB tracked budget regardless of the
multi-gigabyte sweep/stress outputs (decision recorded in
`godotnext/testbed/PROVENANCE.md`).

## Determinism

Two generator runs with the same seed on the same machine give identical
SHA-256 for every output; `expected.json` records the reference hashes
(Apple M5 Max), per-variant instance/triangle/vertex counts, unique
geometry bytes, the reachable-cluster lower bound, per-mesh AABBs and —
for the city variants — a SHA-256 fingerprint of all instance transforms.
The virtual and `lod_baseline` fingerprints are identical by construction.

Caveat: the recorded hashes are of the files as the generator writes
them. The first `--import` rewrites every `.glb.import` in place (it adds
the uid/remap/deps sections and drops unknown keys). The generator
pre-seeds `meshes/generate_virtual_geometry=true` (the #67 option); until
#67 lands the importer drops that key on rewrite, so re-run the generator
before a virtual-geometry import once #67 is in. The importer keeps
`meshes/generate_lods` and `meshes/create_shadow_meshes` as written.

The stress asset is deliberately not part of a casual import: importing
its 10M-triangle `.obj` is the measured G4-2 run in PQ. Until then, move
`generated/dense_city/stress/` aside before `--import` if you only need
the city scenes.

## Validate

After generating and importing:

```
godot --headless --path godotnext/testbed --script res://generators/dense_city/validate_dense_city.gd -- --variant=virtual
```

One run per variant. It recounts everything from the imported meshes
(MeshInstance3D, lights, cameras, unique/instanced triangles, cluster
bound, kit seams, sweep totals, baseline LOD presence versus virtual LOD
absence), byte-diffs the baseline's instance transforms against the
virtual scene's, and hash-checks the outputs against `expected.json`.
Scene root nodes also carry `seed`, `variant`, `instance_count` and
`instanced_triangles` metadata for external checkers.

## How the geometry is built

Kit meshes are subdivided `PlaneMesh` facades displaced in one vertex
loop per surface (window insets, floor ledges, low-amplitude noise; zero
displacement on panel borders so seams stay coincident within 0.1 mm),
assembled into `ArrayMesh` buildings and written as `.glb` through
`GLTFDocument`. Facade normals stay planar — the detail is in the
geometry, which is what the NextGeo scale gates consume. Buildings sit on
a seeded shuffled lot grid around a main street along +X (`cam_main`
looks down it); rooftop props bring the instance count to 5,040. No
`MultiMesh` or `GridMap`: every placement is one scene instance whose
imported scene holds a `MeshInstance3D` (plan §21.4, Q14 fallback
accounting).

Unique bytes (the working-set counter) count position (12 B) + normal
(12 B) + UV (8 B) per unique vertex plus 4 B per unique index:
288,841,248 B against the provisional 128 MiB budget (2.15x). Reachable
clusters are the finest-level lower bound, sum over instances of
ceil(mesh triangles / 128) = 1,182,644; the `NextGeoDebug.build_stats`
confirmation runs in PQ with G4.
