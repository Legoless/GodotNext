# DLSS developer and distributor duties

Companion to `dlss-license.md` (plan §19). This page is for game developers who
build DLSS (and optionally Streamline) in with their own NVIDIA SDKs and for
anyone distributing such a build. It is not legal advice.

## Official builds

Official GodotNext editor and export-template binaries exclude `modules/dlss`
and contain no NVIDIA object code. They are 100% MIT and DLSS/Streamline-free.
Do not add NVIDIA files to the repository: the CI guard
(`misc/scripts/check_proprietary_sdk_files.py`) rejects them, and
`misc/scripts/check_vendor_bytes.py` enforces zero `NVSDK_NGX`, `nvngx_dlss`,
`nvngx_dlssd`, `nvngx_dlssg`, `sl.interposer`, and `sl.dlss_g` bytes (ASCII and
UTF-16LE) in default and mock builds.

## Building with DLSS and Streamline (developers only)

`modules/dlss` is disabled by default (`is_enabled() == False`). Developers who
accept NVIDIA's RTX SDKs License directly can fetch the pinned SDKs outside any
git work tree using the helper script:

```
python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license
python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license --streamline
```

- DLSS SDK pinned tag: `v310.9.16` (default `~/.godotnext/dlss-sdk/v310.9.16`).
- Streamline SDK pinned tag: `v2.14.16` (default `~/.godotnext/streamline-sdk/v2.14.16`, Windows D3D12 only; `sl_nvperf.*` is excluded).

### Windows (MSVC or clang-cl, release CRT only, Vulkan + D3D12)

```
scons platform=windows target=editor dev_mode=yes d3d12=yes \
  module_dlss_enabled=yes dlss_backend=ngx \
  dlss_sdk_path=<their DLSS SDK> \
  streamline_sdk_path=<their Streamline SDK> \
  dlss_notices_path=<their §9.5 notices file>
```

- `use_mingw=yes` and `debug_crt=yes` (`/MDd`) are rejected at configure time
  because the NVIDIA DLSS SDK ships MSVC release-CRT static libraries
  (`nvsdk_ngx_s.lib` for `/MT` when `use_static_cpp=yes`, otherwise
  `nvsdk_ngx_d.lib` for `/MD`).
- Build outputs get the `.dlss` module suffix (e.g.
  `godot.windows.editor.x86_64.dlss.exe`), unmodified `rel/` runtime DLLs
  (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, and when `streamline_sdk_path` is set:
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`,
  `sl.pcl.dll`, `nvngx_dlssg.dll`) are copied into `bin/`, and
  `bin/dlss_runtime.json` records `{sdk_tag, pinned_tag, variant, sha256, files, license, notices, interposer}`.
- `sl.interposer.dll` is delay-loaded (`/DELAYLOAD:sl.interposer.dll`) so runs
  with Frame Generation and Reflex both off never load Streamline.
- `dlss_notices_path` supplies the developer's NVIDIA DLSS Programming Guide §9.5
  notices file outside the repository; it is never committed and is required
  when exporting DLSS games.
- Pass `dlss_dev_runtime=yes` only for local debugging (copies `dev/` runtimes
  and emits a startup warning; exports from `dev/` builds are vetoed).

### Linux (native x86_64 Vulkan)

```
scons platform=linuxbsd target=editor dev_mode=yes \
  module_dlss_enabled=yes dlss_backend=ngx \
  dlss_sdk_path=<their DLSS SDK> \
  dlss_notices_path=<their §9.5 notices file>
```

- Links against `<sdk>/lib/Linux_x86_64/libnvsdk_ngx.a` (or `libsdk_nvngx.a`)
  and copies `libnvidia-ngx-dlss.so.<ver>` and `libnvidia-ngx-dlssd.so.<ver>`
  alongside `bin/dlss_runtime.json`.

### Mock backend (CI and local tests without the SDK)

```
scons platform=linuxbsd target=editor dev_mode=yes \
  module_dlss_enabled=yes dlss_backend=mock
./bin/godot.linuxbsd.editor.x86_64.dlss --headless --test --tc="*[DLSS]*"
```

`dlss_backend=mock` compiles and tests without any SDK and contains zero
proprietary vendor bytes. CI builds and tests only the default and mock
backends. DLSS-enabled `ngx` builds are allowed only on owned NVIDIA RTX
machines, never on rented/cloud GPUs (Supplement §3; the five questions in
`dlss-license.md`).

## Settings, presets, reasons, and diagnostics (#100, #119, #204, #206)

When `modules/dlss` is enabled, `DLSSUpscaler` registers as
`NVIDIA DLSS (Temporal - NVIDIA RTX only)` for `VIEWPORT_SCALING_3D_MODE_DLSS`
on both Vulkan and D3D12 (single view only; multiview reports `VIEW_UNSUPPORTED`).

- **Project settings (`rendering/scaling_3d/dlss/*`):**
  - `project_id`: RFC 4122 GUID generated automatically on first open in a
    DLSS-enabled editor (`DLSSEditorPlugin`) and passed to
    `NVSDK_NGX_*_Init_with_ProjectID` (an empty `project_id` and zero
    `application_id` reports `PROJECT_ID_MISSING` without invoking NGX).
  - `application_id`: optional NVIDIA-assigned integer ID.
  - `preset`: `Default` (0), `J` (1), `K` (2, transformer), `L` (3), `M` (4).
  - `allow_ota_updates`: default `false`; `NVSDK_NGX_UpdateFeature` is called
    only when enabled.
  - `ray_reconstruction`: default `false` (hint: `Beta on Vulkan`). Activates
    only when DLSS is active, NextGI runs HWRT at explicit **Epic**
    (`quality = auto` never enables RR even when auto resolves to Epic),
    `view_count == 1`, and MSAA is off. Otherwise falls back to DLSS SR + D8
    with `RR_REQUIRES_NEXTGI_EPIC` or `RR_UNAVAILABLE`.
  - `rr_preset`: `Default` (0), `D` (1), `E` (2), `F` (3).
- **Ray Reconstruction contract (#206, #204):**
  - Replaces NextGI line 8 (`DenoiserBypass` skips `nextgi_line8` with zero
    line-8 timestamps and bypasses registered PL/PM denoisers such as
    `megalights_denoise`, while keeping line 7 and `probe_temporal_max_history = 16`).
  - Uses `>= 32` Halton jitter phases at every scale.
  - Reports maturity label `"beta"` on Vulkan and `""` (no label) on D3D12.
- **D3D12 barrier paths (#119, #204):**
  - Supports both Enhanced Barriers and legacy resource-state transitions;
    debug builds can force the legacy path via `GODOT_D3D12_FORCE_LEGACY_BARRIERS=1`.
  - Invalidates cached command-list state (`command_buffer_invalidate_cached_state`)
    after every NGX call.
- **Scale snapping:** scales in `[0.30, 1.0]` map to Ultra Performance,
  Performance, Balanced, Quality, and DLAA, with render dimensions resolved from
  NGX's optimal-settings query, mip-bias offset `-1.0`, and dense velocity mode.
- **Memory report:** NGX SR/RR VRAM is reported as the `NGX` row in
  `RenderingContextDriver::get_driver_and_device_memory_report()`.
- **Validation allowlist (`docs/godotnext/dlss-validation-allowlist.md`):**
  Any Vulkan validation message referencing a GodotNext-created object always
  fails (`--gpu-abort`). Messages referencing only NGX-internal objects skip
  `--gpu-abort` only when an exact `(VUID, DLL SHA-256, driver)` row is present.

## Streamline, NVIDIA Reflex, Frame Generation, and MFG (Windows D3D12, #209, #200, #201)

As in NVIDIA's UE plugin, SR and RR stay on NGX directly while Reflex, DLSS
Frame Generation (DLSS-G 2x), Multi Frame Generation (3x–6x), and Dynamic MFG
run through Streamline on Windows D3D12 (game builds only; never in the editor,
secondary windows, XR, or composition/per-pixel-transparency swap chains).

### Latency marker map (#209, U5)

| Core `LatencyMarker` | Engine site | Streamline call |
|---|---|---|
| `LATENCY_MARKER_SLEEP` | `Main::iteration` (before `SIM_START`) | `slReflexSleep(frame_token)` |
| `LATENCY_MARKER_SIMULATION_START` | `Main::iteration` | `slPCLSetMarker(eSimulationStart, token)` |
| `LATENCY_MARKER_INPUT_SAMPLE` | `Input::flush_buffered_events` (once per frame) | `slPCLSetMarker(eControllerInputSample, token)` |
| `LATENCY_MARKER_SIMULATION_END` | `Main::iteration` (before `draw`) | `slPCLSetMarker(eSimulationEnd, token)` |
| `LATENCY_MARKER_RENDER_SUBMIT_START` | `RenderingServerDefault::draw` | `slPCLSetMarker(eRenderSubmitStart, token)` |
| `LATENCY_MARKER_RENDER_SUBMIT_END` | `RenderingServerDefault::_draw` (end) | `slPCLSetMarker(eRenderSubmitEnd, token)` |
| `LATENCY_MARKER_PRESENT_START` | `RenderingDevice::swap_buffers` (before present) | `slPCLSetMarker(ePresentStart, token)` |
| `LATENCY_MARKER_PRESENT_END` | `RenderingDevice::swap_buffers` (after present) | `slPCLSetMarker(ePresentEnd, token)` |

- **Reflex (`rendering/latency/reflex_mode`):** `0` Off (default), `1` On, `2`
  On + Boost. Forced On while DLSS-G or Dynamic MFG is active (1 log line). When
  `application/run/max_fps > 0`, Reflex owns the frame limiter (`frameLimitUs =
  round(1e6 / max_fps)`) and sets `RenderingDevice::_set_max_fps(0)`. Vulkan and
  Metal report `API_UNSUPPORTED`; adapters without low-latency support report
  `REFLEX_UNAVAILABLE` via `RenderingServer.get_latency_status()`.

### DLSS-G, MFG (3x–6x), and Dynamic MFG (#200, #201)

- **Settings (`rendering/frame_generation/*`):** `provider = ""` (off by
  default; set `"DLSS-G"` to enable), `multiplier = 2` (`2..6`, no maturity
  label), `dynamic = false`, `dynamic_target_fps = 0` (`0` = auto-detect
  display refresh rate).
- **Hardware & driver requirements:** FG 2x requires RTX 40-series or newer with
  Hardware-Accelerated GPU Scheduling enabled; MFG (3x–6x) and Dynamic MFG
  require RTX 50-series (Blackwell) and a driver meeting the pinned DLSS-FG
  310.7.0 guide floor (R570+ / R595+). Capabilities come strictly from
  `slDLSSGGetState` (`numFramesToGenerateMax`, `bIsDynamicMFGSupported`), never
  from a hard-coded GPU list.
- **Ada / non-RTX-50 fallback:** Requesting 3x–6x or Dynamic MFG on an Ada
  (RTX 40, `numFramesToGenerateMax == 1`) adapter clamps to FG 2x and reports
  `MFG_REQUIRES_RTX50` (1 log line). Requesting Dynamic MFG when unsupported on
  an MFG-capable adapter falls back to the fixed multiplier with
  `DYNAMIC_MFG_UNSUPPORTED`.
- **GU+-9 validation state:** MFG 3x–6x and Dynamic MFG are enabled in v1;
  formal GU+-9 pacing comparison against UE 5.9 runs in PQ on the RTX 5090 test
  machine.

## Distributor duties (§19(f)) and Export (#97, #208)

A build that links NGX or Streamline contains NVIDIA object code. Distributing
it requires:

- NVIDIA-compatible terms for the NVIDIA portions: license §2(c) (terms at
  least as protective), §2(d) (report non-compliant use), §4(b) and §4(e);
- the EULA terms referenced by §2(c);
- the notices in Supplement §1, §3, §4, §6.1–6.2 and §9.5 of the guides;
- the binding NVIDIA Marks attribution in Supplement §7.1(b), with the
  §7.2(c) approval for any mark use;
- the notification before commercial release (`https://developer.nvidia.com/sw-notification`, Supplement §4).

The About line in a DLSS-enabled editor (`EditorAbout::add_attribution_line`)
requires NVIDIA's §7.2(c) approval before that editor is distributed. RR and
Streamline runtime redistribution follows RR guide §4.2 and U6: only `rel`
files listed in `dlss_runtime.json` are byte-copied by `DLSSEditorExportPlugin`
(keeping NVIDIA's Authenticode signatures intact, even with `codesign/enable`
on) after SHA-256 verification. Any `dev` runtime, SHA-256 mismatch, missing
license or notices file, or excluded `sl_nvperf` entry vetoes the export via
`_blocks_export()` before any byte is written to the output directory.
