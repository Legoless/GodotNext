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

### Programmable raster bins (#155)

The programmable raster bins (#155, plan §21.4 N3, N4) run a material's raster program in the
hybrid raster's software and hardware paths.

| Field | Value |
|---|---|
| What | The Vulkan specification's valid-usage rule VUID-RuntimeSpirv-NonWritable-06341 (appendix "Vulkan Environment for SPIR-V"): without the `vertexPipelineStoresAndAtomics` feature, every storage image, storage texel buffer and storage buffer variable in the vertex, tessellation and geometry stages must be decorated NonWritable. |
| Upstream | https://docs.vulkan.org/spec/latest/appendices/spirvenv.html |
| Version | The "latest" Vulkan specification page, which shows no revision or date, as read on 2026-11-16 |
| License | The Vulkan specification (CC-BY 4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-16, for issue #155 |
| Consulted by | The #155 implementer |
| Used in | `servers/rendering/renderer_rd/shaders/nextgeo/raster_material_inc.glsl` |
| Copied files | None |

How it was used: the hardware form's vertex stage declares the visibility buffer only in the
pixel stage, so the vertex stage holds no writable storage resource. Everything else comes from
the plan (§21.4 N3, N4) and from GodotNext's own code: the raster target reuses #80's
visibility-resolve output and its analytic derivatives, and #162's triangle setup, edge rules,
winding and visibility packing. The bin key, the per-(instance, surface) bin table, the cull
append with a per-bin prefix and scatter, and the per-bin dispatch and draw are this
implementation's own design. No Unreal Engine documentation beyond what the plan already cites
was read for them.

### Material shading bins (#158)

No outside input. The material shading bins (#158, plan §21.4 N7, GN-11, N3) were written from
the plan and the issue: that Unreal Engine shades Nanite pixels through shading bins and compute
is the plan's parity row (§21.4), which cites UE's public documentation; no UE page is recorded as
read for this issue. The per-bucket bin words read through the v2.4 bucket table, the per-8x8-tile
counts in shared memory, the exclusive prefix sum, the scatter of packed 16+16-bit pixel coordinates,
the indirect arguments of ceil(count / 64) folded into balanced rows (65,535 workgroups per
dimension is the minimum `maxComputeWorkGroupCount` Vulkan requires and Direct3D 12's
per-dimension dispatch limit, values known to the implementer, not looked up), and the compute form of the Stage A/B resolve, which
compiles the Forward+ fragment template itself (`#include_stage`) from the code the scene shader's
version already holds, are this implementation's own design on GodotNext's #80 visibility resolve,
following #155's pattern of a separate ShaderRD for a material's compute form. Copied files: none.

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

## Mesh-shader HW rasterizer (#154)

No outside input beyond the public graphics API specifications recorded in
`docs/godotnext/source-log/rhi-mesh-shaders.md`. The task/mesh HW raster path for NextGeo (#154, plan
§21.4 contract N9, GN-10) — one 32-thread task workgroup per 32 compacted HW clusters with a 128-byte
payload, one 128-thread mesh workgroup per cluster capped at 128 vertices and 128 primitives with
per-primitive visibility IDs (`visibility_raster_mesh.glsl`, `raster_material_hw_mesh.glsl`, and the VSM
variant `vsm_raster_nextgeo_mesh.glsl`), indirect-count task dispatch per bin, and
`rendering/nextgeo/hw_raster_path` (`auto` | `vertex` | `mesh`, keeping VS intact until GN-10 passes per
driver in PQ) — is an original implementation built directly on GodotNext's #178 RHI mesh shaders, #162
hybrid visibility raster, #155 programmable raster bins, and #217 VSM x Nanite pass. Copied files: none.

## Pixel depth offset (#149)

No outside input beyond the public graphics API specifications for conservative fragment depth:
GLSL 4.50 (`layout(depth_less) out float gl_FragDepth`), SPIR-V (`ExecutionMode DepthLess`),
Metal Shading Language (`[[depth(less)]]`, emitted automatically by vendored SPIRV-Cross from
`ExecutionModeDepthLess`), and Direct3D 12 / DXIL (`SV_DepthLessEqual`, emitted by Mesa's
`spirv_to_nir` / `nir_to_dxil` from `ExecutionModeDepthLess`). The `DEPTH_OFFSET` spatial fragment
built-in (view-space meters clamped `>= 0.0`, mutually exclusive with absolute `DEPTH`), its
conservative-depth epilogue across Forward+ and Forward Mobile, plain `gl_FragDepth` epilogue in
Compatibility (GLES3), NextGeo programmable raster-bin integration (`RASTER_FEATURE_DEPTH_OFFSET`
in SW compute, HW vertex-pulling, and HW mesh-shader rasterizers), and VSM light-view-space offset
(both NextGeo VSM programmable bins and Forward+ non-Nanite `MODE_VSM`, with `#212` dynamic-layer
classification) are original implementations built on GodotNext's #155, #153, #154, #217, and #218
infrastructure. Copied files: none.

## Vertex offset (WPO) on Nanite (#164)

No outside input. NextGeo vertex offset (#164, plan §21.4 contract N5, GN-3, GN-4, GN-12) —
`Material.vertex_offset_max` with `rendering/nextgeo/vertex_offset_max_default` inheritance, default
admission of `VERTEX`-writing spatial shaders into the `VERTEX_OFFSET` programmable raster bin while
keeping `POSITION`/`MODELVIEW_MATRIX`/`PROJECTION_MATRIX` writes on legacy raster, instance/cluster
bounds inflation by `vertex_offset_max * instance_max_scale` and per-vertex offset clamping with
`#277`-safe clamping counters across SW/HW/mesh/VSM/resolve paths, `GeometryInstance3D.vertex_offset_disable_distance`
demotion to the fixed-function bin in main and VSM views, Stage A visibility-resolve motion vectors
evaluating `vertex()` at previous `TIME` and previous model transform (following Godot's own
`scene_forward_clustered.glsl` motion-pass precedent), VSM dynamic invalidation and VSM `VERTEX_OFFSET`
bins, and opt-in `GeometryInstance3D.rt_evaluate_vertex_offset` proxy compute evaluation
(`rt_vertex_offset.glsl`) with budgeted BLAS `UPDATE` refit (`#45`) — is an original implementation
built directly on GodotNext's #155 programmable raster bins, #153 masked/two-sided distance cutoff,
#154 mesh-shader HW raster, #212/#217 VSM x Nanite, #80/#158 visibility resolve, and #45/#52 NextGI
RT scene. Copied files: none.

## Spline meshes (#161)

No outside input. `SplineMeshInstance3D` (`scene/3d/spline_mesh_instance_3d.{h,cpp}`), the
`RenderingServer.instance_geometry_set_spline_deform()` API, the shared cubic Hermite frame and
16-sample swept-AABB reference (`servers/rendering/storage/spline_deform.h` and
`servers/rendering/renderer_rd/shaders/spline_deform_inc.glsl`), the zero-variant legacy Forward+
SSBO path, NextGeo cull/SW/HW-VS/HW-mesh/resolve integration (`RASTER_FEATURE_SPLINE`), VSM shadow
integration (Nanite and non-Nanite), and per-instance deformed NextGI RT proxy BLAS rebuilds were
implemented directly from GodotNext's plan (§21.4 contract N12, GN-8, GN-12, N8) and standard
cubic Hermite spline mathematics. Copied files: none.

## Forbidden inputs

No Unreal Engine source or shaders, UE plugin source, NvRTX, DLSS sample or Donut
source, or any NVIDIA SDK file was read or used for NextGeo.
