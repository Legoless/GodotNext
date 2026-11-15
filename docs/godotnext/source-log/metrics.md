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

### PresentMon console application 2.6.0 (#202)

| Field | Value |
|---|---|
| What | The CSV the PresentMon console application writes, and its command line |
| Upstream | https://github.com/GameTechDev/PresentMon (`README-ConsoleApplication.md` at tags v2.6.0 and v2.3.0; the v2.6.0 release's asset list) |
| Version | 2.6.0 (released 2026-09-21), the pin; the 2.3.0 README for the column names `--v2_metrics` writes |
| License | MIT. Copyright (C) 2017-2024 Intel Corporation. |
| Read on | 2026-11-15, for issue #202 |
| Consulted by | The #202 implementer |
| Used in | `godotnext/testbed/harness/metrics/godotnext_metrics/presentmon_metrics.py`, the hand-written `harness/metrics/tests/fixtures/fg/presentmon_2_6_0_fg_off.csv` and `presentmon_v2_metrics.csv` |
| Copied files | None. The tool is installed on the test host and not vendored; no PresentMon source was read. `presentmon_2_6_0_recorded.csv` is the tool's output for a testbed run, not a copied file. |

How it was used: the README's column descriptions define a displayed frame (`DisplayedTime` not `NA`), the displayed frame time (`MsBetweenDisplayChange`), GPU busy (`MsGPUBusy`), the application frame interval (`MsBetweenAppStart`, `FrameTime` in 2.3.0) and PC latency (`MsPCLatency`), and its option tables give the command line (`--process_name`, `--output_file`, `--track_pc_latency`, `--terminate_on_proc_exit`, `--stop_existing_session`, `--session_name`, `--no_console_stats`, `--delay`). The two fixture CSVs use those headers with invented rows. The v2.6.0 release lists the console executable `PresentMon-2.6.0-x64.exe` with the SHA-256 `b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af`; `record` refuses any other file, since the README documents no version option; the executable carries no version resource either (checked on the cloud Windows VM, 2026-11-15). A CSV recorded there with that executable has no `DisplayedTime` or `DisplayLatency` column, although the README lists both, so the parser treats `DisplayedTime` as optional.

### dxcam 0.3.0 (#202)

| Field | Value |
|---|---|
| What | Desktop Duplication screen capture from Python on Windows |
| Upstream | https://github.com/ra1nty/DXcam, PyPI package `dxcam` |
| Version | 0.3.0, `dxcam==0.3.0` in `harness/metrics/requirements-fg.txt` |
| License | MIT |
| Read on | 2026-11-15, for issue #202 |
| Consulted by | The #202 implementer |
| Used in | `godotnext/testbed/harness/metrics/godotnext_metrics/display_capture.py` |
| Copied files | None. The package is not vendored, and its source was not read. |

How it was used: the PyPI description's public API only: `dxcam.create(device_idx, output_idx, output_color="BGRA")`, `grab()` returning a NumPy frame or `None` when nothing new was presented, and `release()`.

### Nsight Systems documentation (#202)

| Field | Value |
|---|---|
| What | The `nsys profile` and `nsys export` command lines, the SQLite export schema, and how Streamline names its D3D12 objects |
| Upstream | https://docs.nvidia.com/nsight-systems/UserGuide/index.html and https://docs.nvidia.com/nsight-systems/AnalysisGuide/index.html |
| Version | The public pages as published on 2026-11-15 (Nsight Systems 2026.x) |
| License | NVIDIA documentation, read only |
| Read on | 2026-11-15, for issue #202 |
| Consulted by | The #202 implementer |
| Used in | `godotnext/testbed/harness/metrics/godotnext_metrics/nsys_dlssg.py`, `harness/metrics/tests/fixtures/fg/nsys_dx12_workload.sql` |
| Copied files | None. The fixture's two tables use the documented table and column names for the five columns the tool reads, written for the fixture; the schema itself is not copied. No tool binary or SDK file. |

How it was used: `--trace=dx12` with `--dx12-gpu-workload=individual` traces each D3D12 workload's GPU activity; `nsys export --type=sqlite` writes `DX12_WORKLOAD` (`start`, `end` in ns, `frameId`, `objectNameId`, `textId`) and `StringIds`; the User Guide states that Streamline gives the D3D12 objects it creates names starting with `nv.sl.`, with `nv.sl.dlss_g.cmdQ.dlssg` as its queue example. Not read: the Streamline or DLSS SDKs, their headers or programming guides, the DLSS sample or Donut, or any Unreal Engine file.
