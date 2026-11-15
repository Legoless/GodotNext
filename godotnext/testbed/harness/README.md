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

The runner passes a job file in `GODOTNEXT_HARNESS_JOB`. That is not a user argument. It carries the host label, the clock record, and the power record, and `resolution` (`[width, height]`) when the Godot arguments include `--resolution WxH`. The harness does not call `pmset` itself.

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

`output_size` is `[width, height]` from `get_viewport().get_visible_rect().size`. A capture or timing run fails when it differs from the requested output size (#270), with a message that names both sizes, so a window the desktop clamped cannot pass as the size it asked for. The engine consumes `--resolution`, so the harness takes the requested size from the runner job's `resolution`. A capture without one uses the size of the first PNG movie frame: the movie writer writes every frame at `--resolution` (or at the project window size) and stretches the viewport into it when the two differ. A timing run without a runner job is not checked, and says so. `scaling_3d_mode` and `scaling_3d_scale` come from the viewport after the first captured or measured frame.

`runner.clock` is `locked` and `detail`. The M5 Max and Linux VM runners record `locked: false`. The RTX 4080 SUPER runner locks the clocks for timing jobs and also records `gpu_mhz`, `memory_mhz` and `nvidia_smi_index`.

`runner.power` is `on_ac`, `powermode` (`2` is High Power), `powermode_name`, `thermal`, `before`, and `after`. Each of `before` and `after` holds the filtered `pmset -g`, `pmset -g batt`, and `pmset -g therm` text. Lines that contain a filesystem path or a "prevented by" process list are dropped before anything is written. Host labels are `Apple M5 Max`, `Linux VM`, or the OS name when no job file is present. Machine host names are not recorded.

`dlss`, when present, requires `dll_path`, `dll_version`, `dll_sha256`, `dll_sha_matches_manifest`, `preset`, `allow_ota_updates`, `nvidia_driver`, `override_checklist` (`screenshot`, `state`), and `ngx_log`. Empty strings are allowed. P0 does not fill these. The upscaler fields above are the P0 record. The DLSS module fills the `dlss` object later.

## Runners

`runners/macos_local.sh` records power state, writes `runner_job.json` (with `resolution` from `--resolution`), launches the command after `--`, then patches `runner.power.after` into `manifest.json`. `--check` prints `on_ac`, `powermode`, `powermode_name`, and `thermal`, and exits without launching. Exit code 1 means the machine is not on AC power in High Power mode. `--fail-on-validation` (before `--`) also exits 1 when the run's output has any `ERROR:`, `Invalid Metal usage` or `Error detected in` line, and prints their counts on a `GODOTNEXT_RUNNER_VALIDATION` line. It is opt-in because the legacy captures share the runner. Run the checks that go through it, such as `tests/lumen_lite`, `tests/lumen_lite_hwrt` and `tests/swrt_trace`, with it.

`run_selftest.sh` and `runners/macos_local.sh` export `GODOTNEXT_BACKGROUND=1` unless it is already set, so harness runs on the M5 Max draw on Metal without taking the desktop's focus (`docs/godotnext/fleet.md`). Set `GODOTNEXT_BACKGROUND=0` to watch a run in a normal window.

`runners/linux_vm.sh` has `doctest` and `functional`. `functional` adds `--headless`, writes a job whose host is `Linux VM` when `--out` is set, and rejects timing mode. It does not install or vendor lavapipe.

`runners/win_session1.ps1 -JobFile job.json` runs one Godot launch on the RTX 4080 SUPER machine in the logged-on console session (Session 1), where Godot can create a swapchain, through a one-time scheduled task (`schtasks /IT /RL HIGHEST`). It works from an SSH login (Session 0) and returns Godot's exit code, or 2 for a bad job, 3 for a timeout and 4 when the task could not start. The job file is JSON: `name`, `device` (a device name from the `docs/godotnext/fleet.md` device table), `driver` (`vulkan` or `d3d12`), `kind` (`run`, `capture`, `timing` or `validation`), `args` (Godot arguments without `--rendering-driver` and `--gpu-index`; `{out}` becomes the absolute output directory), `out` (absolute, or relative to the repository root), and optional `godot`, `timeout_minutes`, `fail_on_validation`, `lock_clocks` and `env` (extra environment variables as `{"NAME": "value"}`, recorded in the runner job; `{out}` in a value becomes the output directory, for example `"GODOTNEXT_SPIRV_DUMP_DIR": "{out}/spv"`).
- The runner adds `--rendering-driver` and `--gpu-index`, resolved by device name from `fleet.md`. A job that names an unknown device fails with the list of known names.
- `timing` jobs need the High performance power plan and lock the GPU and memory clocks to the `fleet.md` values (`nvidia-smi --lock-gpu-clocks` and `--lock-memory-clocks`, read back until they hold), and always reset them (`-rgc`, `-rmc`) when the launch ends.
- `validation` jobs set `NV_ALLOW_RAYTRACING_VALIDATION=1` and `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`, and exit 1 when Godot exits 0 but logged an `ERROR:` or `Validation Error` line (`fail_on_validation`; case-sensitive, because the Vulkan loader logs lower-case "error:" lines that are not errors).
- Every job gets `GODOTNEXT_HARNESS_JOB` (host, device, clock, power, override, and `resolution` from a `--resolution WxH` argument). `runner.power` records the active power plan (`powermode` 2 for High performance or Ultimate Performance) and the GPU temperature and throttle reasons; `after` is patched into `manifest.json` when the launch ends. JSON is written as UTF-8 without a byte-order mark.
- Every run of the testbed project on Windows, harness or not, opens a borderless window (`display/window/size/borderless.windows=true` in `project.godot`, #270); the editor's window is unaffected. A decorated window cannot have a client area as large as the desktop: on the RTX 4080 SUPER's 3840×2160 desktop, `--resolution 3840x2160` gave 3840×2135, which the movie writer stretched to 3840×2160.
- The output directory gets `run.log` (stdout, then stderr), `done.json` (exit code, Session id, the locked clocks read back) and `runner_job.json`. The last line printed is `GODOTNEXT_RUNNER {...}` with the device, the Session id and the error counts.

`selftest/run_selftest.ps1` is the Windows twin of `run_selftest.sh`, with the same parameters plus `-Device` and `-Driver`; every launch goes through `win_session1.ps1`, and the manifest check also requires `runner.clock.locked` (and the locked clocks) on timing runs. `runners/run_remote.sh` runs a job or the self-test there from the M5 Max and copies the output back; it needs PowerShell as the machine's OpenSSH default shell, and passes the remote exit code through. SSH carries only the low byte of that code, so a Windows crash status such as `0x80000003` arrives as 3, the same number as the runner's timeout. `done.json` and the `GODOTNEXT_RUNNER` line carry the full code.

The three runners (and therefore both self-tests) apply a per-scene `override.cfg` (#240; `runners/scene_override.sh`, and the same rule in `win_session1.ps1`): when the folder holding `--scene` contains an `override.cfg`, the runner copies it to the testbed root before launching, records it in the runner job (and so in the manifest), and removes the root copy when the launch ends, including on failure. The testbed root is the `--path` argument when given, else `godotnext/testbed`. An existing root `override.cfg` is never overwritten: the runner refuses to launch, as `tests/tier/run_tier_check.sh` does. A scene at the testbed root already has its `override.cfg` in place, so nothing is copied or removed, but the record still names it. `linux_vm.sh doctest` runs no scene, so it never applies one.

## Self-test scene

`selftest/selftest.tscn` builds 100 gray cubes from a fixed `RandomNumberGenerator` seed, one shadowed directional light, a camera named `CaptureCam`, and a WorldEnvironment with SDFGI, a procedural sky, and a linear tonemap. SDFGI bounce feedback is 0, convergence is 5 frames, and dynamic lights update every frame, so the probe history reaches a fixed point and two same-seed captures hash the same. Output goes under `godotnext/testbed/captures/`, which is gitignored. Do not commit captures. The raw power dumps are local run output.

## GT-2 masks, the static segment and the GT-6 load step (#190)

Upscaler Torture carries two GT-2 additions, sized from the static camera at 1440p internal and checked without a GPU by `scenes/upscaler_torture/check_gt_content.gd`: two grilles of vertical bars low on either side, receding at 21–37° to the view ray, with a projected period from 1 to 4 px (moiré layer, bit 17), and 12 cables of 0.3–1.0 px projected width, six against the sky and six against the checker backdrop (thin-wire layer, bit 18).

- `--mask=moire` and `--mask=thin_wire` are capture runs like the #95 masks, but the rest of the scene is drawn black instead of hidden, so a mask pixel is one where that content is visible. The thin-wire mask grows each cable by one 1440p pixel a side in screen space (1.5 px at 2160p), where its anti-aliased footprint lies, so it covers the same share of the frame at both output sizes. Every mask run (#95's too) rewrites its kept frames as single-channel 8-bit PNGs, the R8 mask, beside the run's manifest.
- `--tsr-quality=low|medium|high|epic|cinematic` sets `Viewport.tsr_quality` for `--upscaler=tsr`.
- `--variant=static` holds the static camera and freezes the scene (fan, character, occluders, particles after their seeded preprocess, the scrolling emissive) for the whole run. It is captured as `--warmup=64 --frames=300`; Q8 and Q19's static form read their frozen 64-frame window inside it.
- `runners/upscaler_jobs.sh` adds the `upscaler_torture_static` scene, FSR2 Quality (`fsr2_quality`), TSR Epic at 1.0 and High at 0.667 and 0.5, and the `mask_moire` and `mask_thin_wire` runs. `metrics/upscaler_compare.py` reads that tree.

`tests/gpu_load_step.tscn` is GT-6's load step: the Dense City flight (the Slow IO Flight stand-in, or the generated city with `--with-city=1`) with a compute burner whose cost is calibrated at startup, back to back against the burner-off baseline while the camera sweeps the 30 flight frames before the switch, so the GPU time doubles; it switches on at flight frame 600. The burner's cost scales with the internal pixel count, so dynamic resolution can win it back. `--out=<dir>` writes `gpu_load_step.json` and `gpu_load_step.csv`. Calibration runs with dynamic resolution off; afterwards the project setting comes back, or `--dynamic-resolution=1` turns it on. `--with-city`, `--flight-frames` and `--dynamic-resolution` are standalone flags: the harness rejects flags it does not know, so a timing run takes dynamic resolution from the project settings (an `override.cfg`) and the upscaler from `--upscaler`. In a timing run the calibration has to finish inside `--warmup` (900 frames is enough), and timing runs lock the GPU clock. The doubling within ±5% is measured in PQ (`metrics/`, `python3 -m godotnext_metrics.load_step`, which takes the harness `timing.csv` when there is one).

```bash
./bin/godot.linuxbsd.editor.x86_64 --path godotnext/testbed --rendering-driver vulkan \
  --resolution 2560x1440 --disable-vsync --gpu-profile --quit-after 2400 res://tests/gpu_load_step.tscn -- \
  --mode=timing --scene=res://tests/gpu_load_step.tscn --out="$OUT" --warmup=900 --frames=700
```

Headless checks:

```bash
./bin/godot.macos.editor.arm64 --headless --path godotnext/testbed --script res://scenes/upscaler_torture/check_gt_content.gd
./bin/godot.macos.editor.arm64 --headless --path godotnext/testbed --script res://harness/tests/test_load_step.gd
```

## The scrolling emissive strip (#271)

Upscaler Torture's scrolling emissive strip is unshaded and writes its stripes to `ALBEDO` at ×3; an unshaded material ignores `EMISSION`, so until #271 it rendered black. On the default timeline the stripes scroll 1/30 UV a frame and look the same every 15 frames; `--variant=static` stops them. `python3 -m godotnext_metrics.scrolling_emissive` checks three captures from the static camera: inside the `scrolling_emissive` mask, the linear mean luminance of every frame is at least 0.6 (×1 brightness gives about 0.3), adjacent frames of the default timeline differ by at least 0.1 per pixel on average, and the static variant's frames are identical. The strip spans whole stripe periods, so its mean luminance barely moves as it scrolls; the check reports that change as `mean_change` but tests the per-pixel change. Exit 0 is a pass, 1 a fail and 2 a missing input.

```bash
UT=res://scenes/upscaler_torture/upscaler_torture.tscn
G="./bin/godot.linuxbsd.editor.x86_64 --path godotnext/testbed --rendering-driver vulkan --resolution 2560x1440 --fixed-fps 60 --quit-after 1054"
$G --write-movie "$OUT/default/frame.png" $UT -- --mode=capture --scene=$UT --camera=Camera3D --out="$OUT/default" --frames=4 --capture-frames=0,1,2,3
$G --write-movie "$OUT/static/frame.png" $UT -- --mode=capture --scene=$UT --camera=Camera3D --out="$OUT/static" --frames=4 --capture-frames=0,1,2,3 --variant=static
$G --write-movie "$OUT/mask_scrolling_emissive/frame.png" $UT -- --mode=capture --scene=$UT --camera=Camera3D --out="$OUT/mask_scrolling_emissive" --frames=4 --mask=scrolling_emissive
python3 -m godotnext_metrics.scrolling_emissive --default "$OUT/default" --static "$OUT/static"
```

## GU+ content and metrics (#202)

The static-UI scene, the GPU-bound run and the displayed-frame tools for GU+ (plan §21.5). The measurements are PQ work (§22). PresentMon, Nsight Systems and `dxcam` are installed on the test host and never vendored; the parsers are tested on fixtures written from their documentation and on one recorded PresentMon 2.6.0 CSV (`metrics/tests/fixtures/fg`). Every command below runs from the repository root. The Python modules need `godotnext/testbed/harness/metrics` on the module path: `export PYTHONPATH=godotnext/testbed/harness/metrics` in bash, `set PYTHONPATH=godotnext\testbed\harness\metrics` in the Windows command prompt the `^` lines are written for.

**Static-UI scene.** `scenes/static_ui/static_ui.tscn` instances Upscaler Torture (#95), so it has the same seeded camera spline and 600-frame timeline, under a fixed CanvasLayer HUD laid out in pixels from the viewport's edges and center: labels at 12, 18 and 32 px, a 50%-alpha panel carrying the 18 px label, a 1-px crosshair through the center pixel and a 256-px minimap. `static_ui_3d_only.tscn` is its 3D-only twin, the same file with the HUD hidden. `generators/static_ui.gd` writes both and `minimap.res`; `generators/build_all.gd` runs it after Upscaler Torture. It is the PU+ phase smoke scene (§22.3). With an absolute `--out` (a harness run, or `-- --out=<dir>` standalone) the HUD-visible scene writes the UI mask `cmask_ui.png` there once per scene instance: a single-channel PNG at the viewport's size, white where the HUD's own alpha is at least 0.99, drawn alone in a transparent SubViewport. A window the desktop clamps gives a smaller viewport than the movie frames (a 1920x1080 window on a smaller desktop gave a 1284x781 mask), and a harness capture fails then (#270). It is a content mask like #190's (`docs/godotnext/metrics.md`). The movie frames are unchanged, and the self-test drivers count only `frame*.png`.

```bash
mkdir -p "$OUT"
./bin/godot.linuxbsd.editor.x86_64 --path godotnext/testbed --rendering-driver vulkan --audio-driver Dummy \
  --resolution 1920x1080 --write-movie "$OUT/frame.png" --fixed-fps 60 --quit-after 1650 -- \
  --mode=capture --scene=res://scenes/static_ui/static_ui.tscn --camera=Camera3D --out="$OUT" \
  --seed=1 --frames=600 --warmup=0
./bin/godot.linuxbsd.editor.x86_64 --headless --path godotnext/testbed --script res://scenes/static_ui/check_static_ui.gd
```

The build check (#202): two FG-off captures of each scene give FLIP 0 (`python3 -m godotnext_metrics q7 --in <run_b> --ref <run_a>`; byte-identical frames on Vulkan on the L4), and the scene and its twin differ only inside the HUD's rectangles. `run_selftest.sh` / `run_selftest.ps1 -Scene res://scenes/static_ui/static_ui.tscn -Camera Camera3D -Mode capture -Runs 2` makes and hashes such a pair. A 2160p capture on a 4K desktop needs a borderless window, which the testbed opens on Windows (#270).

**GPU-bound run** (GU+-5, GU+-9 and F1; #236 ports it to UE 5.9). The generated Dense City (`res://generated/dense_city/dense_city.tscn`, from the #63 generator) on its street camera `cam_main`, 3840x2160 borderless fullscreen with VSync off, as a harness timing run on D3D12 through `runners/win_session1.ps1` (`kind` `timing` locks the clocks). The FG and Reflex settings of each configuration go in an `override.cfg` beside the scene, which the runner applies for the launch (#240). A run counts only when PresentMon's GPU busy is at least 95% of the application frame interval; in a comparison that rule is the baseline's (FG off, Reflex Off).

```json
{"name": "gu_plus_fg_off", "device": "NVIDIA GeForce RTX 4080 SUPER", "driver": "d3d12", "kind": "timing",
 "out": "godotnext/testbed/captures/gu_plus/fg_off",
 "args": ["--path", "godotnext/testbed", "--fullscreen", "--resolution", "3840x2160", "--disable-vsync",
          "--gpu-profile", "--audio-driver", "Dummy", "--quit-after", "4650", "--",
          "--mode=timing", "--scene=res://generated/dense_city/dense_city.tscn", "--camera=cam_main",
          "--out={out}", "--seed=1", "--warmup=600", "--frames=3000"]}
```

`--quit-after` is the harness cap, `900 + 30 + 120 + warmup + frames`. PresentMon 2.6.0 (the release's `PresentMon-2.6.0-x64.exe`; `record` refuses any other file) records the testbed process around that job, from an elevated shell (ETW tracing works from an SSH login). `--delay` has to cover the city's load, so the 120 warm-up displayed frames are scene frames; the first PQ run confirms the value. `--process_name` records every process of that name: if another one presents meanwhile (the editor, another job), the CSV holds several process / swap chain pairs, and `--swap-chain` takes the run's `SwapChainAddress`, or `auto` for the pair with the most displayed frames. `metrics/godotnext_metrics/presentmon_metrics.py` then reports the displayed frame time (p50, p99, p99 / median), the displayed rate, PC latency and the GPU-bound check, and compares and spreads runs:

```
python -m godotnext_metrics.presentmon_metrics record --presentmon <PresentMon-2.6.0-x64.exe> ^
  --process-name godot.windows.editor.x86_64.exe --out <dir> --delay 20 -- ^
  powershell -NoProfile -ExecutionPolicy Bypass -File godotnext\testbed\harness\runners\win_session1.ps1 -JobFile <job.json>
python -m godotnext_metrics.presentmon_metrics compare --kind fg --test <fg_2x>\presentmon.csv --base <fg_off>\presentmon.csv
```

**Displayed-UI capture** (GU+-6). `display_capture capture` grabs 240 displayed frames of the static-UI scene through Desktop Duplication (`dxcam`, `metrics/requirements-fg.txt`) and keeps only the pixels inside `cmask_ui.png` (from a capture of the scene at the same output size). Desktop Duplication needs the interactive desktop session, so it runs there, not from an SSH login. `compare` scores FG on against FG off (≤ 0.005):

```
python -m godotnext_metrics.display_capture capture --mask <capture>\cmask_ui.png --out <fg_off> --delay 15 -- ^
  bin\godot.windows.editor.x86_64.exe --path godotnext\testbed --rendering-driver d3d12 --gpu-index 0 ^
  --fullscreen --resolution 3840x2160 res://scenes/static_ui/static_ui.tscn
python -m godotnext_metrics.display_capture compare --test <fg_on> --ref <fg_off>
```

**F1 trace method.** DLSS-G runs inside the Streamline Present on its own queue, so no RD timestamp brackets it. Record 600 frames after the warm-up at FG 2x and 2160p with Nsight Systems, export to SQLite, and read the p95 of the workloads Streamline names `nv.sl.dlss_g*`:

```
nsys profile --trace=dx12 --dx12-gpu-workload=individual --output=<report> <launch command>
nsys export --type=sqlite --output=<report>.sqlite <report>.nsys-rep
python -m godotnext_metrics.nsys_dlssg --sqlite <report>.sqlite
```

**Fallback checker** (GU+-7). `harness/fg_status_probe.tscn` shows the static-UI scene for 120 frames and prints `RenderingServer.get_frame_generation_status()` (#203) as one `GODOTNEXT_FG_STATUS` line. `fallback_log_check` runs each hardware configuration with its `override.cfg` (Vulkan or Metal FG, `API_UNSUPPORTED`; MFG 4x on Ada, `MFG_REQUIRES_RTX50` running FG 2x), checks the reason and exactly one log line naming it, and compares an all-PU+-off capture byte for byte with one from a build without `modules/dlss`. Until #203 lands the status method is absent and every configuration fails. The mocked cases are doctests (#200). The probe finds the reason's constant name in ClassDB on whichever class declares `FrameGenerationReason` (RenderingServer, FrameGenerationExtension or another); `harness/tests/test_fg_status_probe.gd` checks that lookup headless on enums the engine has today.

```
./bin/godot.linuxbsd.editor.x86_64 --headless --path godotnext/testbed --script res://harness/tests/test_fg_status_probe.gd
python3 -m godotnext_metrics.fallback_log_check list
python3 -m godotnext_metrics.fallback_log_check run --godot <editor> --testbed godotnext/testbed --config vulkan_fg --out <dir> -- --gpu-index 0
python3 -m godotnext_metrics.fallback_log_check analyze --config mfg_on_ada --log <runner job>/run.log
python3 -m godotnext_metrics.fallback_log_check identical --in <PU+ off capture> --ref <no-dlss build capture>
```

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
