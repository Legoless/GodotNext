# §13.4 metric tools

Offline checks for Q1–Q21. The definitions and the operational choices are in `docs/godotnext/metrics.md`.

Python >= 3.11 is required. `numpy` 2.3 and `OpenEXR` 3.5 both require it. `flip-evaluator` 1.7 publishes wheels through 3.13 and builds from the 1.7 sdist on 3.14.

```
python3 -m pip install -r godotnext/testbed/harness/metrics/requirements.txt
python3 -m pytest -q godotnext/testbed/harness/metrics/tests
```

Run a metric from this directory so `godotnext_metrics` is on the module path:

```
python3 -m godotnext_metrics q1 --in <capture> --ref <reference> --json
```

The process prints one JSON object with `metric`, `value`, `threshold` and `pass`. Exit 0 when the result was computed, including a failed gate. Exit 2 when the metric name is unknown or an input is missing, still printing JSON.

Streaming reports (#74) are a separate module so this command set stays q1–q21:

```
python3 -m godotnext_metrics.streaming_reports q15 --in <run> --out <dir> --markdown
```

`q15`, `q14`, `slow-io` and `exclusion` read `memory_report.json`, `routing_frames.json`, `slow_io_params.json` plus optional `timing.csv`, and `cells.json` / `exclusion_observed.json`. A missing sidecar is `source: absent` and exit 0. Malformed JSON is exit 2.

Q8 and Q19 take a content mask (#190; `docs/godotnext/metrics.md`, "Content masks"). With no mask the output is the frozen metric's, byte for byte:

```
python3 -m godotnext_metrics q8 --in <capture> --mask moire
python3 -m godotnext_metrics q8 --in <capture> --mask frame
python3 q19_upscaler_flip.py --test <capture> --reference <ssaa_msaa8> --msaa4 <ssaa_msaa4> --mask thin_wire
python3 -m godotnext_metrics.masks coverage --in <mask_thin_wire run>
```

GT-2's comparison report reads a `harness/runners/upscaler_jobs.sh` tree and writes Q19, thin-wire Q19, static and moiré Q8, and Q10 per scene, resolution and configuration (TAA, FSR2, TSR; DLSS report-only):

```
python3 upscaler_compare.py --in <jobs root> --out <dir>
```

GT-6's load step check (`tests/gpu_load_step.tscn`, PQ) compares the GPU frame time after flight frame 600 with the time before it, from `gpu_load_step.csv` or a harness `timing.csv`:

```
python3 -m godotnext_metrics.load_step --in <run>
```

`hdr_*.exr`, `frame*.png`, `timing.csv`, `summary.json` and the patch JSON are the harness and scene formats. Direct-only renders, masks, counters, velocity and ray records are sidecar files named in `docs/godotnext/metrics.md`. The engine's doctests do not import this package.
