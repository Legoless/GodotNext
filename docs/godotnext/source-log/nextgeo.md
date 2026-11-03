# NextGeo source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every input used to write NextGeo code: what it is, where it came from, its
license, and how it was used.

## Approved inputs

### meshoptimizer `demo/clusterlod.h`

| Field | Value |
|---|---|
| What | `clusterlod`, a small cluster-LOD (DAG) builder example on top of meshoptimizer |
| Upstream | https://github.com/zeux/meshoptimizer/blob/9d9890c73011d75920af614485296d1e03e95448/demo/clusterlod.h |
| Commit | `9d9890c73011d75920af614485296d1e03e95448`, the same commit as the vendored `thirdparty/meshoptimizer` (v1.2, see `thirdparty/README.md`) |
| File SHA-256 | `143461d2863a8abb979faca1b702c971498d9627c9f8f83c4d62c0aa4bc2a271` |
| License | MIT, Copyright (c) 2016-2026 Arseny Kapoulkine. The same license and author as `thirdparty/meshoptimizer`, already listed in `COPYRIGHT.txt`. |
| Read on | 2026-11-03, for issue #65 (NextGeo offline DAG builder) |
| Consulted by | The #65 implementer, while writing the builder |
| Used in | `modules/nextgeo/nextgeo_dag_builder.cpp` (the file header names the source) |
| Copied files | None. The file is not vendored. |

How it was used: the issue (#65, step 4) names this file as the reference for the DAG
build. `nextgeo_dag_builder.cpp` is a new implementation in Godot style that follows its
structure:

- position-only remap for cluster connectivity (`meshopt_generatePositionRemap`);
- `meshopt_partitionClusters` over remapped indices, with positions;
- locking vertex positions shared by more than one group before simplifying;
- `meshopt_simplifyWithAttributes` with `meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute`,
  a 50% target and the 85% "stuck" threshold that makes a group terminal;
- group LOD bounds merged from the members' bounds with `meshopt_computeSphereBounds`,
  never from the simplified geometry, with error = max(simplification error, members' errors);
- terminal groups with error `FLT_MAX`.

GodotNext's own additions, not taken from the file: one merged stream over several
surfaces with single-surface re-clusterization, permanent locks against ineligible
sibling surfaces, locks against clusters of earlier terminal groups, page-size splitting
of groups, deterministic levels on `WorkerThreadPool`, exact sphere enclosure in double
precision, the page assignment, the BVH8 layout, the 80-byte cluster record and the CPU
cut reference and checker.

The file's own config defaults (permissive simplification, sloppy fallback, 16-cluster
partitions, flexible cluster sizes) were not adopted; the issue's parameters were used.

### meshoptimizer library (vendored)

`thirdparty/meshoptimizer` (v1.2, commit `9d9890c73011d75920af614485296d1e03e95448`,
MIT) is already vendored. #65 adds `meshletcodec.cpp` and `meshletutils.cpp`, both
unmodified upstream files that were vendored but not compiled, to
`modules/meshoptimizer/SCsub` (decision D3).

## Forbidden inputs

No Unreal Engine source or shaders, UE plugin source, NvRTX, DLSS sample or Donut
source, or any NVIDIA SDK file was read or used for NextGeo.
