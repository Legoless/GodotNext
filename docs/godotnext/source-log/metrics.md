# Metrics source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used to write the §13.4 metric tools: what it is, where
it came from, its license, and how it was used.

## Approved inputs

### NVIDIA FLIP, Python package `flip-evaluator` 1.7

| Field | Value |
|---|---|
| What | LDR FLIP image error, called as a library |
| Upstream | https://github.com/NVlabs/flip |
| Version | 1.7, PyPI package `flip-evaluator==1.7` |
| License | BSD-3-Clause. Copyright 2020-2025 NVIDIA Corporation & Affiliates. |
| Read on | 2026-11-03, for issue #14 |
| Consulted by | The #14 implementer |
| Used in | `godotnext/testbed/harness/metrics/godotnext_metrics/flip_eval.py` |
| Copied files | None. The package is not vendored. |

How it was used: `flip_evaluator.evaluate(reference, test, "LDR", inputsRGB=True, applyMagma=False, computeMeanError=True)`. The public Python API was consulted. The C++ and CUDA sources were not read, and no engine integration of FLIP was consulted. Q7, Q9 and Q19 call this function. Mean and p99 are taken from the scalar error map. p99 uses the harness percentile in `manifest.gd`, which is code in this repository.

### OpenEXR Python bindings 3.5.3

| Field | Value |
|---|---|
| What | `OpenEXR.File` read and write of scanline images |
| Upstream | https://github.com/AcademySoftwareFoundation/openexr |
| Version | 3.5.3, PyPI package `OpenEXR==3.5.3` |
| License | BSD-3-Clause |
| Read on | 2026-11-03, for issue #14 |
| Consulted by | The #14 implementer |
| Used in | `godotnext/testbed/harness/metrics/godotnext_metrics/images.py` |
| Copied files | None |

How it was used: write and read R, G, B and A planes so a half-float channel stays half and a float channel stays float. Q21 compares the sentinel against the stored channel type.

The percentile function is a port of `godotnext/testbed/harness/manifest.gd` in this repository. It is not an outside source.
