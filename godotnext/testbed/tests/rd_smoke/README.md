# RD ray-query smoke test (#18)

A testbed GDScript on a local `RenderingDevice` builds BLASes and a
ray-query-only TLAS (`hit_sbt_range` 0, no RT pipeline) and dispatches
`GL_EXT_ray_query` compute kernels. It reads back t, instance custom index,
primitive ID, barycentrics and front face and compares them with
double-precision analytic values (plan §13.4, Q17). Devices without ray query
assert the fallback; they never skip.

| Fixture | Scene | Rays | Pass rule |
|---|---|---|---|
| A | One triangle at z = −5, instance id 7 | Forward and reverse | t = 5, u = v = 0.25; the two rays disagree on front face |
| B | 64 instances (ids 1000–1063) of a 1 m quad and an 8-triangle bipyramid on a centered 10 m grid, seeded rotations | 4096, seed `0x4E455854` | IDs, primitive, front face and committed exact; t within 1e-5 relative; barycentrics within 1e-5 absolute |
| C | Same scene; geometry built with `NO_DUPLICATE_ANY_HIT_INVOCATION` | 1024 at shared edges | `candidate_count == 1` and the primitive is one of the two adjacent triangles |

Every fixture runs with BLAS inputs in vertex/index buffers and again in
storage buffers; the result records must be bit-identical. The analytic side
intersects the exact float32 values the GPU receives (float32 vertices,
float32 instance transforms, float32-quantized rays) against every world
triangle in double precision, and rejects any fixture B ray whose
second-nearest hit is within 1e-3 m or whose target is within 1e-3
barycentric distance of an edge.

## Files

- `rd_smoke.tscn` / `rd_smoke.gd` — the fixtures and the comparison.
- `rq_kernel.glsl` — opaque traversal, one `rayQueryProceedEXT`, committed
  getters (the D5 rules of `nextgi_rt_inc.glsl`).
- `rq_edges.glsl` — fixture C only: `gl_RayFlagsNoOpaqueEXT` with a candidate
  loop, so double hits and cracks on shared edges are visible.
- `run_rd_smoke.sh` — the M5 Max runner (Metal validation plus the Q17 tool).

Results use the 32-byte Q17 record shared with #14
(`{float t; uint custom_index; uint primitive; float u; float v; uint
front_face; uint committed; uint candidate_count;}`). Each run writes
`user://rd_smoke_kernel.spv`, `user://rd_smoke_edges.spv`,
`user://rd_smoke_rays.bin` (fixture B records), `user://rd_smoke_edges.bin`
(fixture C records) and `user://rd_smoke_analytic.json` (fixture B analytic
records). It prints one `GODOTNEXT_RESULT {"test":"rd_smoke",...}` line and
exits 0 on pass, 1 on failure. A device without ray query prints
`"fallback":true` after asserting that `blas_create` returns an invalid RID
(and, once bound, that the unsupported reason is `NO_RAY_QUERY` or
`D3D12_NO_DXR`).

## Front face and winding

`rayQueryGetIntersectionFrontFaceEXT` follows the Vulkan facing rule: a
triangle whose vertices wind counter-clockwise as seen from the ray origin is
front-facing, unless the instance sets
`ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT` (or the instance
transform has a negative determinant). Godot's acceleration-structure front
face is counter-clockwise; on Metal, #47 selects the counter-clockwise option
to match. Fixture A pins the rule: the forward ray sees the triangle
counter-clockwise (front face 1) and the reverse ray sees it clockwise
(front face 0), with identical t and barycentrics.

The Metal driver's front-face mapping was inverted when #47 landed; this fixture caught it,
and the driver now maps `FLIP_FACING` directly to Metal's counter-clockwise option, as the
Vulkan driver passes it through. The Vulkan run in P2 (#22) is the cross-check.

## Run commands

The Apple M5 Max (Metal, full test since #47):

```sh
sh godotnext/testbed/tests/rd_smoke/run_rd_smoke.sh
```

or directly:

```sh
GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  ./bin/godot.macos.editor.arm64 --path godotnext/testbed \
  --rendering-driver metal res://tests/rd_smoke/rd_smoke.tscn
```

The RTX 4080 SUPER machine (Vulkan; through `harness/runners/win_session1.ps1`
once it lands, with `NV_ALLOW_RAYTRACING_VALIDATION=1` and
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`):

```bat
godot.windows.editor.x86_64.console.exe --path godotnext/testbed ^
  --rendering-driver vulkan --gpu-index 1 --gpu-validation --gpu-abort ^
  res://tests/rd_smoke/rd_smoke.tscn
```

The fallback path runs with `--gpu-index 0` on the RTX 4080 SUPER machine and
with `--rendering-driver d3d12`. The Linux VM runs the full test if lavapipe
exposes ray query and the fallback otherwise; record which ran.

## Q17 metric tool and SPIR-V validation

The #14 metric tools recompute Q17 from the dumped records:

```sh
mkdir -p /tmp/rd_smoke_q17
cp "<user dir>/rd_smoke_rays.bin" /tmp/rd_smoke_q17/rays.bin
cp "<user dir>/rd_smoke_analytic.json" /tmp/rd_smoke_q17/analytic.json
cd godotnext/testbed/harness/metrics
python3 -m godotnext_metrics q17 --in /tmp/rd_smoke_q17 --json
```

Validate the saved modules (reported at PR time, gated as G0-1 in PQ):

```sh
spirv-val --target-env vulkan1.1 "<user dir>/rd_smoke_kernel.spv"
spirv-val --target-env vulkan1.1 "<user dir>/rd_smoke_edges.spv"
```

The user dir is `~/Library/Application Support/Godot/app_userdata/GodotNext
Testbed/` on macOS and `%APPDATA%\Godot\app_userdata\GodotNext Testbed\` on
Windows.
