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

### Rasterization rules for the software rasterizer (#162)

The hybrid HW/SW rasterizer (#162, plan §21.4 N2) rasterizes small triangles in a compute
shader, so its coverage, facing and depth must agree with the hardware rasterizer's.

| Field | Value |
|---|---|
| What | Direct3D's triangle rasterization rules: pixel centers are sampled, and a center on an edge is inside under the top-left rule (a top edge is exactly horizontal and above the other edges; a left edge is not horizontal and on the triangle's left side). The Vulkan specification's rasterization chapter: single-sample rasterization samples the fragment center; a sample on an edge shared by two polygons is covered by exactly one of them; the front face is chosen by the sign of the polygon's framebuffer-space area (`VK_FRONT_FACE_CLOCKWISE`: negative area is front-facing); polygon depth is interpolated linearly in screen space. |
| Upstream | https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules and https://docs.vulkan.org/spec/latest/chapters/primsrast.html |
| Version | The Microsoft Learn page dated 2026-09-08; the "latest" Vulkan specification page, which shows no revision or date, as read on 2026-11-16 |
| License | Microsoft Learn documentation (CC-BY 4.0); the Vulkan specification (CC-BY 4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-16, for issue #162 |
| Consulted by | The #162 implementer |
| Used in | `servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h`, `servers/rendering/renderer_rd/shaders/nextgeo/sw_raster_setup_inc.glsl`, `tests/servers/rendering/test_nextgeo_sw_raster.cpp` |
| Copied files | None |

How it was used: the software setup samples pixel centers, keeps a center on an edge
when the edge is a top or left edge (framebuffer y points down), keeps the faces the
v2.4 pipeline keeps (cull back, front face clockwise) and interpolates depth linearly
from the integer edge functions. The test's reference rasterizer restates the top-left
rule as "the sample moved by an infinitely small (e, e^2) is inside" and checks the
shared-edge property on watertight meshes. Vertex snapping to 1/256 pixel and the
coarser grid for large triangles are this implementation's own choice. The hybrid
design itself (the 64-bit depth and ID buffer written with atomic max by both paths,
the per-cluster triangle-size switch and the export pass) comes from plan §21.4 N1 to
N3; no Unreal Engine documentation beyond what the plan already cites was read for it.

## Native Nanite ray tracing (#156)

No outside input. `rendering/nextgeo/ray_tracing_mode`, the per-band cut buckets and
their shared set pool, the extraction pass `rt_cut_extract.glsl`, the N7 rebuild cap,
the MN4 memory cap (inside the AS memory line) with its whole-set admission, reclaim
and release, the per-triangle Stage-2 card choice and the exclusions were written from
the plan (§21.4 N13, N7, MN4, GN-9 and its parity rows) and the issue. That Unreal Engine
keeps native Nanite ray tracing Experimental and off, with the fallback mesh as the
default, is the plan's parity row (§21.4), which cites UE's public documentation; no UE
page is recorded as read for this issue. The card choice follows GodotNext's own #77
card layers (`modules/nextgeo/nextgeo_rt_proxy.cpp`). Copied files: none.

## Forbidden inputs

No Unreal Engine source or shaders, UE plugin source, NvRTX, DLSS sample or Donut
source, or any NVIDIA SDK file was read or used for NextGeo.
