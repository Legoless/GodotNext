# RD ray-query smoke test (#18, #49)

A testbed GDScript on a local `RenderingDevice` builds BLASes and a
ray-query-only TLAS (`hit_sbt_range` 0, no RT pipeline) and dispatches
`GL_EXT_ray_query` compute kernels. It reads back t, instance custom index,
primitive ID, barycentrics and front face and compares them with
double-precision analytic values (plan §13.4, Q17). Devices without ray query
assert the fallback; they never skip.

| Fixture | Dump name | Scene | Rays | Pass rule |
|---|---|---|---|---|
| A | `tri` | One triangle at z = −5, instance id 7 | Forward and reverse | t = 5, u = v = 0.25; the two rays disagree on front face |
| B | `rays` | 64 instances (ids 1000–1063) of a 1 m quad and an 8-triangle bipyramid on a centered 10 m grid, seeded rotations | 4096, seed `0x4E455854` | IDs, primitive, front face and committed exact; t within 1e-5 relative; barycentrics within 1e-5 absolute |
| C | `edges` | Same scene; geometry built with `NO_DUPLICATE_ANY_HIT_INVOCATION` | 1024 at shared edges | `candidate_count == 1` and the primitive is one of the two adjacent triangles |
| D | `winding` | One triangle in six instances (ids 2000–2005): plain, `FLIP_FACING`, mirrored (x scale −1), mirrored + `FLIP_FACING`, turned 180°, `FACING_CULL_DISABLE` | 36, from both sides | As A, with the front face of the facing rule below |

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
  getters (the D5 rules of `nextgi_rt_inc.glsl`). Fixtures A, B and D.
- `rq_edges.glsl` — fixture C only: `gl_RayFlagsNoOpaqueEXT` with a candidate
  loop, so double hits and cracks on shared edges are visible.
- `run_rd_smoke.sh` — the M5 Max runner (Metal validation plus the Q17 tool).

## Arguments and output

User arguments go after `--`:

- `--out=<dir>`: the dump folder. The default is `user://rd_smoke`.
- `--expect-reason=<name>`: the device must lack ray query with this reason
  (`NO_RAY_QUERY`, `D3D12_NO_DXR`, `METAL_SYNC_MODE` or
  `METAL_NO_ARGUMENT_BUFFERS`). Without it, a device without ray query must
  still report some unsupported reason.

The dump folder is the Q17 tool's input (`harness/metrics`, #49):

- `meta.json`: the schema, the driver and adapter names, the seed and the
  record count of each fixture;
- `<name>.bin` for `tri`, `rays`, `edges` and `winding`: the 32-byte Q17
  records shared with #14 (`{float t; uint custom_index; uint primitive;
  float u; float v; uint front_face; uint committed; uint candidate_count;}`);
- `<name>_analytic.json`: the analytic expectations. Edge rows carry only
  `custom_index`, `adjacent_primitives`, `committed` and `candidate_count`;
- `kernel.spv` and `edges.spv`: the compiled modules.

The test prints one `GODOTNEXT_RESULT {"test":"rd_smoke",...}` line with the
driver, adapter and dump folder, and exits 0 on pass, 1 on failure. A device
without ray query prints `"fallback":true` and the reason after asserting that
`blas_create` returns an invalid RID. That call logs one expected `ERROR:`
line, so fallback jobs on the RTX 4080 SUPER use kind `run`, not `validation`.

## Front face and winding

`rayQueryGetIntersectionFrontFaceEXT` follows the Vulkan facing rule: facing
is decided in the instance's space, and a triangle whose vertices wind
counter-clockwise as seen from the ray origin is front-facing. Two things
invert it: the instance's `ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT`,
and an instance transform with a negative determinant (a mirror). Both
together cancel. Godot's acceleration-structure front face is
counter-clockwise; on Metal the driver selects the counter-clockwise option
only when `FLIP_FACING` is set, which gives the same result.

Fixture A pins the plain rule: the forward ray sees the triangle
counter-clockwise (front face 1) and the reverse ray sees it clockwise (front
face 0), with identical t and barycentrics. Fixture D adds the inversions:
`FLIP_FACING`, a mirrored transform, both, a 180° turn (which must not
invert) and `FACING_CULL_DISABLE` (which must not change facing). A failure
lists the cases whose front face is wrong.

The Metal driver's front-face mapping was inverted when #47 landed; fixture A
caught it, and the driver now maps `FLIP_FACING` directly to Metal's
counter-clockwise option, as the Vulkan driver passes it through.

## Run commands

The Apple M5 Max (Metal):

```sh
sh godotnext/testbed/tests/rd_smoke/run_rd_smoke.sh
```

or directly:

```sh
GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 \
  ./bin/godot.macos.editor.arm64 --path godotnext/testbed \
  --rendering-driver metal res://tests/rd_smoke/rd_smoke.tscn -- --out=/tmp/rd_smoke_metal
```

The Metal fallback modes run on the same binary:

```sh
GODOT_MTL_SYNC_MODE=none ... res://tests/rd_smoke/rd_smoke.tscn -- --expect-reason=METAL_SYNC_MODE
GODOT_MTL_DISABLE_ARGUMENT_BUFFERS=1 ... res://tests/rd_smoke/rd_smoke.tscn -- --expect-reason=METAL_NO_ARGUMENT_BUFFERS
```

The RTX 4080 SUPER machine (Vulkan), through the Session-1 runner from the M5
Max. The runner adds `--rendering-driver` and `--gpu-index`, and a `validation`
job adds `NV_ALLOW_RAYTRACING_VALIDATION=1` and
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`:

```json
{"name": "rd-smoke", "device": "NVIDIA GeForce RTX 4080 SUPER", "driver": "vulkan", "kind": "validation",
 "args": ["--path", "godotnext/testbed", "--gpu-validation", "res://tests/rd_smoke/rd_smoke.tscn", "--", "--out={out}/dump"],
 "out": "godotnext/testbed/captures/remote/rd-smoke", "timeout_minutes": 10}
```

```sh
godotnext/testbed/harness/runners/run_remote.sh job rd_smoke.json /tmp/rd_smoke_remote
```

The dump comes back in `/tmp/rd_smoke_remote/rd-smoke/dump`. With
`--rendering-driver d3d12` the test takes the fallback (`D3D12_NO_DXR`) until
PD. The Linux VM runs the full test if lavapipe exposes ray query and the
fallback otherwise; record which ran.

## Q17 metric tool and SPIR-V validation

The #14 metric tool recomputes Q17 from a dump folder, against the analytic
values or against another device's dump (G2-1, measured in PQ):

```sh
cd godotnext/testbed/harness/metrics
python3 -m godotnext_metrics q17 --in /tmp/rd_smoke_metal --json
python3 -m godotnext_metrics q17 --in /tmp/rd_smoke_remote/rd-smoke/dump --ref /tmp/rd_smoke_metal --json
```

Validate the saved modules (reported at PR time, gated as G0-1 in PQ):

```sh
spirv-val --target-env vulkan1.1 <dump>/kernel.spv
spirv-val --target-env vulkan1.1 <dump>/edges.spv
```
