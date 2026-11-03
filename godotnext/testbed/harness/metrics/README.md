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

`hdr_*.exr`, `frame*.png`, `timing.csv`, `summary.json` and the patch JSON are the harness and scene formats. Direct-only renders, masks, counters, velocity and ray records are sidecar files named in `docs/godotnext/metrics.md`. The engine's doctests do not import this package.
