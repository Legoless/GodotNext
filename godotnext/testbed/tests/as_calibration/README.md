# AS calibration benchmark (#9, G0-6)

`as_calibration.tscn` measures acceleration-structure costs on a local
`RenderingDevice` in the RT-arena input format: float32 positions, uint32
indices, storage-buffer inputs with `DEVICE_ADDRESS` and
`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY`, and opaque geometry.
`godotnext_metrics.as_calibration` (in `harness/metrics`) turns its results into
the first measurement of plan §2.1 lines 1a and 1b.

| Measure | What runs |
| --- | --- |
| BLAS throughput | 64 seeded grid-displaced meshes from 1k to 64k triangles (2,137,242 in total), built with `PREFER_FAST_TRACE` and with `PREFER_FAST_BUILD`. Each build has its own timestamps. Triangles per GPU ms per flag and per size bucket (1k–8k, 8k–32k, 32k–64k). |
| TLAS build | `tlas_create(N)` and `tlas_build` at N = 10k, 50k and 100k instances of the FAST_TRACE BLASes, seeded transforms within 200 m, `hit_sbt_range` 0, mask 0xFF, with each flag. GPU ms, and the CPU ms of the `tlas_build` call. |
| Refit | Through RD's UPDATE mode (#45): a `PREFER_FAST_TRACE` + `ALLOW_UPDATE` BLAS of 10k to 200k triangles. Every iteration moves every vertex, then refits. |
| Memory | `MEMORY_ACCELERATION_STRUCTURES` for each configuration (#22), read right after the structures are created. RD frees structures only when a frame slot cycles, so a later read would also subtract what an earlier point freed. |

Each point runs `--warmup` untimed and `--iters` timed iterations and keeps p50
and p95. `--runs` repeats the whole measurement. A device without ray query
prints a fallback `GODOTNEXT_RESULT` with its reason and exits 0.

The standalone Vulkan refit tool (`tools/as_refit_bench`) measures refit outside
RD. It times the same 100k-instance TLAS and the same FAST_TRACE BLAS set as a
cross-check. The derivation logs both refit figures. It uses the tool's figure
for line 1a only when both cross-check figures are within 15% of RD's.

## Arguments

After `--`:

- `--warmup=20 --iters=200 --runs=1` (the defaults);
- `--tlas=10000,50000,100000 --refit=10000,50000,100000,200000`;
- `--out=<dir>` also writes `<dir>/as_calibration.json`.

## Runs

PR time (the field check and a validation run; the numbers are PQ's), on the
RTX 4080 SUPER machine through the Session-1 runner from the M5 Max:

```json
{"name": "as-calibration", "device": "NVIDIA GeForce RTX 4080 SUPER", "driver": "vulkan", "kind": "run",
 "args": ["--path", "godotnext/testbed", "res://tests/as_calibration/as_calibration.tscn", "--", "--warmup=2", "--iters=5", "--out={out}"],
 "out": "godotnext/testbed/captures/remote/as-calibration", "timeout_minutes": 20}
```

The validation run is the same job with kind `validation`, `--gpu-validation`
before the scene, and `--warmup=1 --iters=2`.

Then run the refit tool on the same machine (`tools/as_refit_bench/README.md`
explains how), and the derivation over both results:

```sh
godotnext/testbed/harness/runners/run_remote.sh job as_calibration.json /tmp/asc
cd godotnext/testbed/harness/metrics
python3 -m godotnext_metrics.as_calibration /tmp/asc/as-calibration/as_calibration.json \
    --refit-tool /tmp/asc/tool/as_refit_bench.json --out /tmp/asc/derived
```

The derivation exits 1 when a field is missing in either result, or when a tool
result matches no card. It writes `as_calibration.json` (lines 1a and 1b, the
tool cross-check, both refit figures, spreads, memory) and `as_calibration.md`
(the Appendix A table, one column per card).

PQ (G0-6, with #24): kind `timing` (clocks locked), the defaults and `--runs=3`
on the RTX 4080 SUPER and the RTX 5090 machine, and the tool with `--runs 3` on
each. Then run the derivation over both cards' results, with one `--refit-tool`
per card (matched by adapter). Its output goes to `docs/godotnext/baselines/g0/`.

The mocked-capability fallback check runs headless anywhere:

```sh
godot --headless --path godotnext/testbed --script res://tests/as_calibration/as_calibration_fallback_check.gd
```
