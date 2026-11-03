# SDF import set

The sign set for gate GS-5 (#181): 12 closed and 4 open meshes that the
mesh distance field importer (#184) and its sign code are checked
against. Everything here is generated; never edit an `.obj` or `.json`
file in this folder by hand.

## Contents

- `*.obj` — one mesh per file, meters, triangles counter-clockwise
  seen from outside. Sizes are 1–2 m. Closed: `box`, `sphere`
  (icosphere), `cylinder`, `cone`, `torus`, `capsule`, `tetrahedron`,
  `octahedron`, `icosahedron`, `hexagonal_prism`, `l_prism` (concave),
  `wedge`. Open: `plane_sheet`, `open_box` (no top), `open_tube` (no
  caps), `hemisphere_bowl` (no rim cap).
- `manifest.json` — per mesh: vertex and triangle counts, boundary and
  non-manifold edge counts, signed volume, AABB, and the SHA-256 of
  the OBJ text. A closed mesh has 0 boundary edges and a positive
  signed volume; `scenes/swrt_content_check.gd` verifies the hashes.
- `sign_reference.json` — the 512-ray parity reference: for each mesh,
  a padded voxel grid (16 voxels on the longest axis) with an
  inside/outside string (`dims`, `grid_origin`, `voxel_size_m`,
  x-fastest). GS-5 compares the importer's sign against this on the
  closed meshes. Open meshes are included so their degenerate signs
  are visible, but no gate scores them.
- `*.obj.import` — the importer settings Godot writes for each OBJ.
  They are the plain OBJ-importer defaults at this point: the plan's
  `meshes/generate_distance_fields` import option (plan §21.2 L2)
  does not exist yet because #184 builds it. When it lands, enable it
  on these files and re-import; nothing else about the set changes.

## Regenerate

From the repository root:

```
godot --headless --path godotnext/testbed --script res://generators/build_sdf_assets.gd
godot --headless --path godotnext/testbed --script res://generators/sdf_sign_reference.gd
godot --headless --path godotnext/testbed --import
```

The first two are byte-deterministic (the sign reference takes a few
minutes: 512 BVH rays per voxel center). The mesh data lives in
`generators/sdf_mesh_library.gd`, which the display scene in
`scenes/sdf_import_set/` shares, so the assets, the reference, and
the scene can never describe different geometry.

Checks: `res://scenes/swrt_content_check.gd` (headless) verifies the
census contract, the manifest hashes, and analytic inside/outside
samples of the reference (including the torus hole and the `l_prism`
missing quadrant).
