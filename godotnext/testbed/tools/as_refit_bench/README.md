# AS refit tool (#9, G0-6)

`as_refit_bench` is a minimal standalone Vulkan program that measures BLAS
refit throughput with `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR`, source
equal to destination. It is one C++ file and a `CMakeLists.txt` against the
Vulkan SDK. It is not part of the engine build, and the `.gdignore` keeps Godot
out of this folder.

It exists because RD had no BUILD/UPDATE mode until #45. The tool was the
smallest way to measure refit for G0-6 without pulling plan §9 item 8 forward.
RD now has the UPDATE mode, so the RD benchmark (`tests/as_calibration`) measures
refit too, and PQ logs both figures side by side.

## What it measures

Every measurement mirrors the RD benchmark, so the two can be compared directly:

- **Formats:** float32 positions and uint32 indices, in device-local storage
  buffers with a device address and `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY`.
  The TLAS instances sit in host-visible memory, as RD keeps them.
- **Timing:** one submit and wait per iteration, a barrier after every build,
  and timestamps from `vkCmdWriteTimestamp2`. Each point keeps nearest-rank p50
  and p95 over `--iters` timed iterations, after `--warmup` untimed ones.

| Measure | What runs |
| --- | --- |
| Refit | A `PREFER_FAST_TRACE` + `ALLOW_UPDATE` BLAS of 10k to 200k triangles (the RD benchmark's refit meshes). Every iteration copies in the other of two vertex sets before the first timestamp, so every refit moves every vertex. Triangles per GPU ms at p50. |
| Cross-check: BLAS | The RD benchmark's 64 `PREFER_FAST_TRACE` meshes (1k to 64k triangles, 2,137,242 in total), each build with its own scratch range and timestamps. Total triangles over the sum of per-mesh p50. |
| Cross-check: TLAS | 100,000 instances of those BLASes, seeded transforms within 200 m, mask 0xFF, SBT offset 0, `PREFER_FAST_TRACE`. p95 GPU ms. |

The meshes have the RD benchmark's triangle counts exactly. Their displacement
values come from a different seeded generator, so the geometry is equivalent but
not bit-identical.

`godotnext_metrics.as_calibration` compares the cross-check figures with RD's
(`--refit-tool`). It uses the tool's refit figure for line 1a only when both are
within 15% of RD's. Otherwise it uses RD's figure and records why: investigate
first (#9).

## Build

You need the Vulkan SDK (1.3 or newer, with `VULKAN_SDK` set), CMake 3.20 or
newer and a C++17 compiler. Build outside the repository. On Windows, run this in
a Visual Studio developer prompt (`VsDevCmd.bat -arch=amd64`), whose CMake and
Ninja are enough:

```sh
cmake -S godotnext/testbed/tools/as_refit_bench -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build <build dir>
ctest --test-dir <build dir> --output-on-failure
```

`ctest` runs `as_refit_bench --self-test`, the CPU-only checks: the mesh counts
against the RD benchmark, percentiles, option parsing and the result keys the
derivation reads. It needs no GPU.

## Run

The tool needs no window or swapchain, so it runs over plain SSH (Session 0 on
Windows). Pick the GPU by name: on the RTX 4080 SUPER machine, Session 0 lists
the Intel UHD 770 first.

- `--device <name>`: a case-insensitive substring of the device name. Or use
  `--gpu-index <n>`, the enumeration index. Without either, the tool picks the
  first discrete GPU with acceleration structures. `--list-devices` lists them.
- `--warmup 20 --iters 200 --runs 1` are the defaults.
- `--refit 10000,50000,100000,200000` sets the refit sizes.
- `--out <dir>` also writes `<dir>/as_refit_bench.json`.
- `--validation` turns on the Khronos validation layer with synchronization
  validation, plus `VK_NV_ray_tracing_validation` where the driver offers it
  (the tool sets `NV_ALLOW_RAYTRACING_VALIDATION=1` itself). Any validation error
  fails the run. Without the layer, the tool exits 1 instead of passing
  unchecked. Timing runs leave this off.

It prints one `GODOTNEXT_RESULT <json>` line and exits 0 on a pass. It exits 1 on
a failure or any validation error, and 2 on a usage error. A device without
`VK_KHR_acceleration_structure`, or without Vulkan 1.3, prints a fallback result
with its reason and exits 0. The derivation reads these keys:

- `adapter`;
- `blas_fast_trace_triangles_per_ms` and `tlas_100k_p95_ms`, the cross-check
  figures;
- `refit`, per size, and `refit_triangles_per_ms`, taken at 200k triangles (the
  line 1a skinned budget), else at the largest size.

With `--runs` above 1, each of these is the median over runs, and `runs` keeps
every run. The result also reports AS memory per configuration and the
validation counts.

PR time (one sample run and a separate validation run; the numbers are PQ's):

```sh
as_refit_bench --device "RTX 4080 SUPER" --warmup 2 --iters 5 --out <dir>
as_refit_bench --device "RTX 4080 SUPER" --validation --warmup 1 --iters 2
cd godotnext/testbed/harness/metrics
python3 -m godotnext_metrics.as_calibration <rd dir>/as_calibration.json --refit-tool <dir>/as_refit_bench.json --out <derived dir>
```

PQ (G0-6, with #24): `as_refit_bench --device "RTX 4080 SUPER" --runs 3`, with
the GPU clocks locked to the `fleet.md` values as the timing runner locks them.
Run the same on the RTX 5090 machine with `--device "RTX 5090"`. Then run the
derivation over both cards' RD results, with one `--refit-tool` per card. Tool
results are matched to cards by adapter name.
