# GodotNext testbed harness

Capture mode and timing mode for the P0 testbed, plus the runners for the Apple M5 Max, the Linux VM and the RTX 4080 SUPER machine (Session 1, clock locks, #237). The G0-4 measurement runs in PQ.

The harness autoload stays idle unless the arguments after `--` include `--mode`. Other testbed scenes are not affected.

`godotnext/testbed/project.godot` in this change is a minimal stub: issue 10 step 1 (Forward+, Windows RenderingDevice driver `vulkan`, debanding off, physical light units off) plus the `Harness` autoload, a fixed 1280x720 window, HDR 2D, and the self-test as the main scene. Issue 10 owns the rest of the project. The reviewer merges the two versions.

## Commands

Apple M5 Max, capture. The runner refuses to start unless the machine is on AC power in High Power mode. It only reads `pmset`. It does not change power settings.

```bash
godotnext/testbed/harness/runners/macos_local.sh -- \
  ./bin/godot.macos.editor.arm64 --path godotnext/testbed \
  --rendering-driver metal \
  --write-movie "$OUT/frame.png" --fixed-fps 60 --quit-after 1062 -- \
  --mode=capture --scene=res://harness/selftest/selftest.tscn \
  --camera=CaptureCam --out="$OUT" --seed=1 --frames=8 --warmup=4 \
  --capture-frames=4,7
```

Apple M5 Max, timing. No movie writer. VSync is disabled on the command line and again by the harness.

```bash
godotnext/testbed/harness/runners/macos_local.sh -- \
  ./bin/godot.macos.editor.arm64 --path godotnext/testbed \
  --rendering-driver metal --disable-vsync --gpu-profile --quit-after 1770 -- \
  --mode=timing --scene=res://harness/selftest/selftest.tscn \
  --camera=CaptureCam --out="$OUT" --seed=1 --warmup=120 --frames=600
```

Linux VM, doctest only:

```bash
godotnext/testbed/harness/runners/linux_vm.sh doctest
```

Linux VM, functional capture (lavapipe). Point Vulkan at lavapipe before launching, for example with `VK_DRIVER_FILES`. The script rejects `--mode=timing`.

```bash
VK_DRIVER_FILES=/path/to/lvp_icd.json \
  godotnext/testbed/harness/runners/linux_vm.sh functional -- \
  --path godotnext/testbed --write-movie "$OUT/frame.png" --fixed-fps 60 \
  --quit-after 1062 -- \
  --mode=capture --scene=res://harness/selftest/selftest.tscn \
  --camera=CaptureCam --out="$OUT" --seed=1 --frames=8 --warmup=4 \
  --capture-frames=4,7
```

The RTX 4080 SUPER machine (Windows 11), from the M5 Max. `runners/run_remote.sh` reads the machine's SSH host alias and checkout path from `GODOTNEXT_RTX_HOST` and `GODOTNEXT_RTX_REPO`, or from the git-ignored `local/fleet.env`, and does not sync code: put the checkout there at the commit you mean to test first. Capture pair and timing (clocks locked) self-tests, with the results copied back:

```bash
godotnext/testbed/harness/runners/run_remote.sh selftest -Mode capture -Runs 2 --fetch "$OUT"
godotnext/testbed/harness/runners/run_remote.sh selftest -Mode timing -Runs 3
```

Any scene as a job (see "Runners" for the job file):

```bash
godotnext/testbed/harness/runners/run_remote.sh job job.json "$OUT"
```

On the machine itself, from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File godotnext\testbed\harness\selftest\run_selftest.ps1 -Mode capture -Runs 2
powershell -NoProfile -ExecutionPolicy Bypass -File godotnext\testbed\harness\runners\win_session1.ps1 -JobFile job.json
```

Self-test. Capture defaults are warmup 4, frames 8, two runs. Timing defaults are warmup 120, frames 600, three runs. `-Frames` and `-Warmup` override both. On macOS every launch goes through `macos_local.sh`. `-Scene`, `-Camera`, and `-Label` point the same driver at another scene; output then lands in `captures/<label>` (the scene file name when no label is given).

```bash
godotnext/testbed/harness/run_selftest.sh -Mode capture -Runs 2
godotnext/testbed/harness/run_selftest.sh -Mode timing -Runs 1 -Warmup 2 -Frames 8
godotnext/testbed/harness/run_selftest.sh -Mode capture -Runs 2 \
  -Scene res://scenes/feature_parity_room/feature_parity_room.tscn -Camera cam_0
```

The last form is the Feature Parity Room capture-identity pair (#240): two same-seed captures with the room's fog on, compared by SHA-256, plus the manifest override check below.

Argument and override checks (no GPU):

```bash
./bin/godot.macos.editor.arm64 --headless --path godotnext/testbed \
  --script res://harness/tests/test_args.gd
godotnext/testbed/harness/tests/test_override.sh
```

## Arguments

After `--`:

| Flag | Required | Default |
| --- | --- | --- |
| `--mode=capture` or `--mode=timing` | yes | |
| `--scene=res://…` | yes | |
| `--camera=<name or NodePath>` | no | empty |
| `--out=<absolute directory>` | yes | |
| `--seed=<int>` | no | 1 |
| `--frames=<int>` | yes | |
| `--warmup=<int>` | no | 0 |
| `--capture-frames=<list>` | no | empty (no EXR) |
| `--variant=<id>` | no | empty; the scene's own default |
| `--hold-frame=<int>` | no | unset (`-1`); freezes the scene's frame index |
| `--instances=<int>` | no | unset; NextGI Perf uses 50000 |

`--out` has to be an absolute filesystem path. `res://` and `user://` are rejected. Values can be `--flag=value` or `--flag value`.

The runner passes a job file in `GODOTNEXT_HARNESS_JOB`. That is not a user argument. It carries the host label, the clock record, and the power record. The harness does not call `pmset` itself.

## What a capture run does

1. Seed the global RNG and every integer `seed` property.
2. Turn HDR 2D on, debanding off, TAA off, MSAA off, and screen-space AA off. WorldEnvironment tonemap is linear, exposure is 1, and glow, adjustments, SSAO, SSIL, and SSR are off. Fog and volumetric fog stay as the scene set them (#240). Depth fog is evaluated per frame, so it is deterministic under `--fixed-fps` already. Volumetric fog temporal reprojection is the one history-carrying part: its accumulated history would depend on how many settle frames ran before the captured window. Capture mode pins `volumetric_fog_temporal_reprojection_enabled` off, so every captured frame starts from the fixed state of no history. The same settings are reapplied to the reloaded instance in step 4, so two same-seed captures stay byte-identical with fog on. SDFGI is left as the scene set it.
3. Wait until the canvas, mesh, surface, draw, and specialization pipeline-compilation counters have each stayed unchanged for 30 frames. Specialization compiles in the background and can keep moving. If the other four have been flat for 30 frames and specialization has not, within 900 draws, the run settles without specialization and says so. If the counters still have not settled by 1020 draws, the run fails.
4. Reload the scene and seed again. The captured window starts on the next presented frame of the new instance, one slot per `frame_post_draw`. Reload drops the settle frames so both runs start at the same place.
5. The captured window is `--warmup` frames followed by `--frames` frames. `--capture-frames` indexes that whole window, so with warmup 4 the first measured frame is index 4. Each listed frame is saved with `save_exr(path, false, false, -1.0)` as `hdr_%05d.exr` (linear viewport image, negatives kept, no clamp).
6. On the way out, pre-window PNGs and the `.wav` are removed. The kept PNGs are renamed to `frame00000000.png` upward, warmup frames first. `--quit-after` on the command line is only a safety cap. The movie writer counts every drawn frame, including settle, so the cap is `900 + 30 + 120 + warmup + frames`. The harness calls `quit()` when the window ends. The issue's `warmup + frames` figure is the length of the kept window, not the process lifetime.

## What a timing run does

Scoped to G0-4 and G0-6. There is no settle window and no movie writer. After the scene is current, the next `--warmup` process frames are discarded and the following `--frames` process frames are recorded.

`timing.csv` columns are `frame,wall_ms,<pass>_ms...`. `frame` is the measurement index starting at 0. Wall time is the `Time.get_ticks_usec()` delta between `_process` calls, in milliseconds. On `frame_post_draw`, RenderingDevice timestamps are read with `get_captured_timestamps_count`, `get_captured_timestamps_frame`, `get_captured_timestamp_name`, and `get_captured_timestamp_gpu_time`. Godot leaves those queries empty unless timestamp capture is on, so timing launches pass `--gpu-profile`. That flag can also print a `GPU PROFILE` block after about a second. That print is not a measurement. A pass is the GPU-time delta to the next timestamp, named from the earlier stamp, converted from nanoseconds to milliseconds. Blank cells are passes that frame did not report. The same timestamp frame is not recorded twice.

`summary.json` stays schema 1. It holds `schema`, `frames`, `warmup`, and `columns`, plus the §2.1 fields below. Each column has `p50`, `p95`, `p99`, and `n`. Percentiles use linear interpolation: rank is `(percent / 100) * (n - 1)`. Empty input is 0. A single sample is that sample. Blank GPU cells are skipped, so a pass can have `n` below `frames`. `timing.csv` is unchanged: the new fields are not columns.

While a capture or timing run is active, the harness publishes the frame index (`is_driving_animation`, `get_animation_frame`). Capture uses the window index, warmup included. Timing holds frame 0 through the entry frame and the warmup, then uses the measured row index. `--hold-frame` on the scene wins over that index. Scenes that are not in a harness run count frames themselves, still from the frame index, never from wall time.

The extra summary object:

- `lines` maps `1a`, `1b`, `2a`, `2b`, `3`, `4`, `5`, and `7` to a name plus `p50`, `p95`, `p99`, and `n`. Names are AS updates, AS builds, SRC capture, SRC relight, WRC update, Screen traces, HWRT diffuse gather, and Gather integration. A timestamp column maps onto a line by its sanitized name (`nextgi_as_updates_ms`, `as_updates_ms`, `tlas_rebuild_ms`, and `skinned_refit_ms` are line 1a; `blas_build_ms`, `compaction_copy_ms`, `line_1b_ms`, `as_updates_amortized_ms`, and `1b_ms` are line 1b; the same pattern covers 2a, 2b, 3, 4, 5, and 7). The last sample of a column wins inside a row, then the mapped columns add. A missing pass is 0. Lines other than 1b use the steady frames only. Line 1b uses the tagged frames only.
- `diffuse_path` is the percentile of the per-steady-frame sum of lines 1a, 2a, 2b, 3, 4, 5, and 7.
- A frame is tagged line 1b when `counters.blas_builds` or `counters.compaction_copies` (or the `blas_build_count` / `compaction_copy_count` aliases) is above zero, or when the summed line-1b timestamps are above zero. `line_1b_frames` lists those indices and the p99 of their full GPU pass sum (not `wall_ms`). `high_total_ms`, `line_1b_budget_ms`, and `limit_ms` are filled when the host or the adapter names the device: Apple M5 Max is 4.00 + 0.25 = 4.25 ms, the RTX 4080 SUPER is 1.60 + 0.10 = 1.70 ms. The RTX 5090 machine has a device name and null budgets. Any other host leaves the device empty and the budgets null. `within_limit` is null when either side is null. A budget miss does not fail the run. These millisecond totals are report-only.
- `counters.source` is `nextgi_get_counters` when `RenderingServer.nextgi_get_counters` exists or a row carried a counter object, and `absent` otherwise. The keys `relight_texels`, `shadow_rays`, `q18`, `tlas_fill_ms`, and `capture_cpu_ms` are always present. Absent counters are null. Present counters are the same percentiles over every measured frame (`src_relight_texels`, `shadow_rays_traced`, and `rt_coverage` are accepted aliases).

Q11 is unchanged. It still drops line 1b by column name inside every frame, then takes p95. The frame tag above is an extra field, not a change to that definition. There is no Ada 3060 ratio and no mid-range device key.

The spread across runs is printed by `run_selftest.sh` and does not fail the script. The ≤2% p95 spread is a PQ gate (G0-4), not a PR-basics failure. Do not treat a timing printout as a measurement result.

## Manifest

`manifest.json` is schema 1. `dlss` is omitted unless the runner job supplies it. A manifest without `dlss` is valid. A partial `dlss` object is not. `override` follows the same rule (#240): the runners always supply it, a manifest without it (a direct launch with no runner job) is valid, and a partial record is not.

Top-level fields: `schema`, `host`, `adapter` (`name`, `api_version`), `commit` (engine version hash), `command_line` (`engine`, `user`), `seed`, `mode`, `scene`, `camera`, `frames`, `warmup`, `capture_frames`, `runner`, `scaling_3d_mode`, `scaling_3d_scale`, `output_size`, and `override` when a runner supplied it.

`override` is `applied` (bool), `source` (the `res://` path of the per-scene file that was copied to `res://override.cfg` for the launch), and `sha256` (hex digest of that file). A scene with no per-scene `override.cfg` records `applied: false` with empty `source` and `sha256`.

`output_size` is `[width, height]` from `get_viewport().get_visible_rect().size`. `scaling_3d_mode` and `scaling_3d_scale` come from the viewport after the first captured or measured frame.

`runner.clock` is `locked` and `detail`. The M5 Max and Linux VM runners record `locked: false`. The RTX 4080 SUPER runner locks the clocks for timing jobs and also records `gpu_mhz`, `memory_mhz` and `nvidia_smi_index`.

`runner.power` is `on_ac`, `powermode` (`2` is High Power), `powermode_name`, `thermal`, `before`, and `after`. Each of `before` and `after` holds the filtered `pmset -g`, `pmset -g batt`, and `pmset -g therm` text. Lines that contain a filesystem path or a "prevented by" process list are dropped before anything is written. Host labels are `Apple M5 Max`, `Linux VM`, or the OS name when no job file is present. Machine host names are not recorded.

`dlss`, when present, requires `dll_path`, `dll_version`, `dll_sha256`, `dll_sha_matches_manifest`, `preset`, `allow_ota_updates`, `nvidia_driver`, `override_checklist` (`screenshot`, `state`), and `ngx_log`. Empty strings are allowed. P0 does not fill these. The upscaler fields above are the P0 record. The DLSS module fills the `dlss` object later.

## Runners

`runners/macos_local.sh` records power state, writes `runner_job.json`, launches the command after `--`, then patches `runner.power.after` into `manifest.json`. `--check` prints `on_ac`, `powermode`, `powermode_name`, and `thermal`, and exits without launching. Exit code 1 means the machine is not on AC power in High Power mode. `--fail-on-validation` (before `--`) also exits 1 when the run's output has any `ERROR:`, `Invalid Metal usage` or `Error detected in` line, and prints their counts on a `GODOTNEXT_RUNNER_VALIDATION` line. It is opt-in because the legacy captures share the runner. Run the checks that go through it, such as `tests/lumen_lite`, `tests/lumen_lite_hwrt` and `tests/swrt_trace`, with it.

`run_selftest.sh` and `runners/macos_local.sh` export `GODOTNEXT_BACKGROUND=1` unless it is already set, so harness runs on the M5 Max draw on Metal without taking the desktop's focus (`docs/godotnext/fleet.md`). Set `GODOTNEXT_BACKGROUND=0` to watch a run in a normal window.

`runners/linux_vm.sh` has `doctest` and `functional`. `functional` adds `--headless`, writes a job whose host is `Linux VM` when `--out` is set, and rejects timing mode. It does not install or vendor lavapipe.

`runners/win_session1.ps1 -JobFile job.json` runs one Godot launch on the RTX 4080 SUPER machine in the logged-on console session (Session 1), where Godot can create a swapchain, through a one-time scheduled task (`schtasks /IT /RL HIGHEST`). It works from an SSH login (Session 0) and returns Godot's exit code, or 2 for a bad job, 3 for a timeout and 4 when the task could not start. The job file is JSON: `name`, `device` (a device name from the `docs/godotnext/fleet.md` device table), `driver` (`vulkan` or `d3d12`), `kind` (`run`, `capture`, `timing` or `validation`), `args` (Godot arguments without `--rendering-driver` and `--gpu-index`; `{out}` becomes the absolute output directory), `out` (absolute, or relative to the repository root), and optional `godot`, `timeout_minutes`, `fail_on_validation` and `lock_clocks`.
- The runner adds `--rendering-driver` and `--gpu-index`, resolved by device name from `fleet.md`. A job that names an unknown device fails with the list of known names.
- `timing` jobs need the High performance power plan and lock the GPU and memory clocks to the `fleet.md` values (`nvidia-smi --lock-gpu-clocks` and `--lock-memory-clocks`, read back until they hold), and always reset them (`-rgc`, `-rmc`) when the launch ends.
- `validation` jobs set `NV_ALLOW_RAYTRACING_VALIDATION=1` and `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`, and exit 1 when Godot exits 0 but logged an `ERROR:` or `Validation Error` line (`fail_on_validation`; case-sensitive, because the Vulkan loader logs lower-case "error:" lines that are not errors).
- Every job gets `GODOTNEXT_HARNESS_JOB` (host, device, clock, power, override). `runner.power` records the active power plan (`powermode` 2 for High performance or Ultimate Performance) and the GPU temperature and throttle reasons; `after` is patched into `manifest.json` when the launch ends. JSON is written as UTF-8 without a byte-order mark.
- The output directory gets `run.log` (stdout, then stderr), `done.json` (exit code, Session id, the locked clocks read back) and `runner_job.json`. The last line printed is `GODOTNEXT_RUNNER {...}` with the device, the Session id and the error counts.

`selftest/run_selftest.ps1` is the Windows twin of `run_selftest.sh`, with the same parameters plus `-Device` and `-Driver`; every launch goes through `win_session1.ps1`, and the manifest check also requires `runner.clock.locked` (and the locked clocks) on timing runs. `runners/run_remote.sh` runs a job or the self-test there from the M5 Max and copies the output back; it needs PowerShell as the machine's OpenSSH default shell, and passes the remote exit code through.

The three runners (and therefore both self-tests) apply a per-scene `override.cfg` (#240; `runners/scene_override.sh`, and the same rule in `win_session1.ps1`): when the folder holding `--scene` contains an `override.cfg`, the runner copies it to the testbed root before launching, records it in the runner job (and so in the manifest), and removes the root copy when the launch ends, including on failure. The testbed root is the `--path` argument when given, else `godotnext/testbed`. An existing root `override.cfg` is never overwritten: the runner refuses to launch, as `tests/tier/run_tier_check.sh` does. A scene at the testbed root already has its `override.cfg` in place, so nothing is copied or removed, but the record still names it. `linux_vm.sh doctest` runs no scene, so it never applies one.

## Self-test scene

`selftest/selftest.tscn` builds 100 gray cubes from a fixed `RandomNumberGenerator` seed, one shadowed directional light, a camera named `CaptureCam`, and a WorldEnvironment with SDFGI, a procedural sky, and a linear tonemap. SDFGI bounce feedback is 0, convergence is 5 frames, and dynamic lights update every frame, so the probe history reaches a fixed point and two same-seed captures hash the same. Output goes under `godotnext/testbed/captures/`, which is gitignored. Do not commit captures. The raw power dumps are local run output.

## Streaming reports (#74)

Exclusion Matrix, Slow IO Flight and the Feature Parity Room streaming variant are scenes, not harness modes. Their reports are a side CLI, schema 1, and they do not add keys that Q11 reads and do not add entries to the frozen q1–q21 command set:

```
python3 -m godotnext_metrics.streaming_reports {q15,q14,slow-io,exclusion} --in <run> [--out <dir>] [--markdown]
```

`--out` writes `geometry_memory.json`, `fallback_coverage.json`, `slow_io_flight.json` or `exclusion_matrix.json`, plus a Markdown twin. Exit 0 when the report object was produced, including `source: absent` and a failed gate. Exit 2 when `--in` is missing or a sidecar is not valid JSON. Byte counts, page-upload samples and fallback debug stats stay null until #78 and #79 add the rendering-server methods. The Slow IO p99 gate stays `pass: null` (PQ).

Slow IO Flight records `rendering/nextgeo/streaming/debug_io_delay_ms` as a wall-time parameter (uniform 50–250 ms per request, or 0 for the nodelay variant). The scene does not sleep. Issue 33's summary fields are unchanged.

## Left for later

Per-pass spread stays a print from `run_selftest.sh`. Timing numbers from these runs are not measurements. Slow IO frame-time percentiles are filled when a `timing.csv` is present and are not a measurement.

G0-4 as a measurement runs in PQ, on the RTX 4080 SUPER machine through `run_selftest.ps1 -Mode timing -Runs 3` and the capture pair. G0-6 calibration scenes are not part of this harness. The timing file format is what those runs will use.
