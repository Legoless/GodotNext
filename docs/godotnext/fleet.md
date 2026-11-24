# Fleet

Hardware is named by what it is. This file does not record host names.

## Background runs on the Apple M5 Max

The M5 Max is also someone's desktop. A rendered (non-headless) Godot run activates itself and takes the keyboard focus, so every unattended run there sets `GODOTNEXT_BACKGROUND=1`: agents, the harness, smoke scenes and scene checks. With it, Godot runs as an accessory app (no Dock icon or menu bar), never activates itself, and shows its windows fully transparent and click-through. The windows stay on screen, so Metal keeps drawing every frame. Captures read the viewport texture, so they are unchanged.

```
GODOTNEXT_BACKGROUND=1 ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal --quit-after 60 <scene>
```

`misc/scripts/godotnext_background_check.sh` checks it: a lit scene renders on Metal while the frontmost app stays the same. Don't switch apps while it runs. The mode lives in `platform/macos/godotnext_background.h` and the `background_mode` hooks.

A transparent window is still a window: it shows as a blank tile in Mission Control, and the editor's main window spans the whole screen during `--export-pack` (the editor ignores `--position` and `--resolution`). Export runs never present a frame, so they also set `GODOTNEXT_BACKGROUND_HIDDEN=1`, which keeps the window ordered out. The shader baker still compiles on the RenderingDevice.

```
GODOTNEXT_BACKGROUND=1 GODOTNEXT_BACKGROUND_HIDDEN=1 ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal --export-pack "macOS (baker)" <out.pck>
```

## Metal shader validation

PR basics on the Apple M5 Max count Metal API and shader validation, not only Godot `ERROR:` lines. Shader-validation reports stay off stderr unless `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` is set. With the variable unset, a scene can render black while stdout looks clean, because validation zeros the stores it rejects.

```
GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal --quit-after 60 <scene>
```

A pass is zero lines containing `Invalid Metal usage`, zero lines containing `Error detected in`, and zero lines starting with `ERROR:`. `useHeaps` marks placement-heap resources read-only. Argument-buffer writes need `useResource` with the shader's usage, or the log reports an invalid store (`resident:Read Only`) or a texture that is not resident.

## Fleet editor builds and Vulkan capability archive (#11)

Recorded on 2026-11-08 at base commit `17dd0086` (`4.8.beta.custom_build.17dd00861`). The RTX 5090 machine's Linux rows are in its own section below (2026-11-23); its Windows rows follow through #227.

### Editor builds, doctests, and dev tools

| Host | OS | GPU | Compiler | Build command | Editor binary | Wall time | `--headless --test --force-colors` | Vulkan SDK and SPIR-V dev tools |
|---|---|---|---|---|---|---|---|---|
| Apple M5 Max | macOS 27.0.1 (26A434) | Apple M5 Max, 40 GPU cores, Metal 4 (`system_profiler SPDisplaysDataType`); Vulkan RT compiled out (MoltenVK), Metal HWRT active | Apple clang 21.0.0 (`clang-2100.4.1.1`) | `scons platform=macos target=editor dev_mode=yes arch=arm64 use_volk=yes -j8` | `bin/godot.macos.editor.arm64` | 10.82 s incremental (warm 2.5 GB SCons cache) | 1735 passed, 0 failed (3 pre-existing platform skips) | No LunarG SDK (`use_volk=yes`); `spirv-val`: `SPIRV-Tools v2026.4` (2026-09-10); `spirv-cross`: built 2026-09-16 (`/opt/homebrew/bin`) |
| the RTX 4080 SUPER machine | Windows 11 Pro (10.0.26100) | NVIDIA GeForce RTX 4080 SUPER (plus Intel UHD 770, not a test device: plan §26.4) | MSVC 19.44.35228 for x64 (VS 14.3, Windows SDK 10.0.26100.0) | `python misc/scripts/install_d3d12_sdk_windows.py`, then `scons platform=windows target=editor dev_mode=yes d3d12=yes -j16` | `bin\godot.windows.editor.x86_64.console.exe` | 9m 27s (567.2 s) cold; 34.20 s incremental | 1728 passed, 0 failed (3 pre-existing platform skips) | LunarG Vulkan SDK `1.4.363.0` (`VK_LAYER_KHRONOS_validation` 1.4.363); `spirv-val`: `SPIRV-Tools v2026.4 v2026.4.rc2-0-gef96ed76`; `spirv-cross`: `vulkan-sdk-1.4.357.0-39-gf11ba9f0` (2026-09-27) |
| the Linux VM | Linux 7.2.5-3 x86_64 (Arch-based, 4 vCPU, 15 GiB RAM) | None (`llvmpipe`, LLVM 22.1.8, 256 bits, Mesa 26.2.2) | `g++ (GCC) 16.2.1 20260810` | `scons platform=linuxbsd target=editor dev_mode=yes -j4` | `bin/godot.linuxbsd.editor.x86_64` | 25m 39s (1538.7 s) cold; 11.67 s incremental | 1734 passed, 0 failed (3 pre-existing platform skips) | System Vulkan loader `1.4.357` (`VK_LAYER_KHRONOS_validation` 1.4.357); `spirv-val`: `SPIRV-Tools v2026.3 vulkan-sdk-1.4.357.0-0-g9a49b0883` |

The dev tools (`spirv-val`, `spirv-cross`, LunarG Vulkan SDK) are local dependencies and are never vendored (§13.1).

### Device mapping on the RTX 4080 SUPER machine (Session 1 vs Session 0)

SSH Session 0 cannot create desktop swapchains. In the interactive desktop session (Session 1), `VK_LAYER_NV_optimus` on Vulkan and DXGI on D3D12 enumerate the discrete NVIDIA GPU first (`#0`), whereas headless Session-0 `vulkaninfo --summary` lists the integrated Intel UHD 770 at `GPU0` and the RTX 4080 SUPER at `GPU1`. Harnesses (#237) resolve indices by device name from this table rather than hardcoding an ordinal across tools:

| Device name | v1 role | Godot Session-1 Vulkan `--gpu-index` | Godot Session-1 D3D12 `--gpu-index` | `nvidia-smi -i` | Session-0 `vulkaninfo --json=<n>` |
|---|---|---|---|---|---|
| `NVIDIA GeForce RTX 4080 SUPER` | Primary NVIDIA build & gate device | `0` | `0` | `0` | `1` |
| `Intel(R) UHD Graphics 770` | Not a test device (plan §23.2, §26.4); one-off informational runs only | `1` | `1` | n/a | `0` |
| `Microsoft Basic Render Driver` | Software WARP adapter (unused) | n/a | `2` | n/a | n/a |

Session-1 Vulkan `--verbose` excerpt (`bin\godot.windows.editor.x86_64.console.exe --verbose --rendering-driver vulkan --gpu-index 0 --quit-after 3`):

```
Devices:
  #0: NVIDIA NVIDIA GeForce RTX 4080 SUPER - Supported, Discrete
  #1: Intel Intel(R) UHD Graphics 770 - Supported, Integrated
...
- Vulkan Acceleration Structure supported
  min acceleration structure scratch offset alignment: 128
- Vulkan Raytracing supported
  shader group handle size: 32
  shader group handle alignment: 32
  shader group handle size aligned: 32
  shader group base alignment: 64
Vulkan 1.4.357 - Forward+ - Using Device #0: NVIDIA - NVIDIA GeForce RTX 4080 SUPER
```

Session-1 D3D12 `--verbose` excerpt (`bin\godot.windows.editor.x86_64.console.exe --verbose --rendering-driver d3d12 --gpu-index 0 --quit-after 3`):

```
Devices:
  #0: NVIDIA NVIDIA GeForce RTX 4080 SUPER - Supported, Discrete
  #1: Intel Intel(R) UHD Graphics 770 - Supported, Discrete
  #2: Microsoft Microsoft Basic Render Driver - Supported, CPU
...
D3D12 12_0 - Forward+ - Using Device #0: NVIDIA - NVIDIA GeForce RTX 4080 SUPER
```

### The RTX 5090 machine (#227, plan v3.9.20)

The second test machine of the v1 fleet (plan §23.2) is the lead's Threadripper workstation, in use from 2026-11-23 (lead decision, plan v3.9.20). For now it runs Linux Vulkan only. Its Windows 11 lines (D3D12, DLSS, frame generation, MFG, Reflex, PresentMon and the UE 5.9 comparison) come later in a Windows 11 VM on the same machine with the RTX 5090 passed through (#227). It is the lead's workstation and hosts other work: GodotNext runs stay in their own folders, one GPU job at a time, and its name, address and helper scripts are in the git-ignored `local/` folder.

| Host | OS | GPU | Compiler | Build command | Editor binary | Wall time | `--headless --test` | Vulkan validation and tools |
|---|---|---|---|---|---|---|---|---|
| the RTX 5090 machine | Ubuntu 26.04.1 LTS, kernel 7.0.0-44 (AMD Ryzen Threadripper PRO 9985WX, 64 cores, 128 GB) | NVIDIA GeForce RTX 5090, 32 GB, driver 595.99.02 (Vulkan 1.4.329), plus llvmpipe | GCC 15.2.0 | `scons platform=linuxbsd target=editor dev_mode=yes -j56` (SCons 4.11.1, Python 3.14) | `bin/godot.linuxbsd.editor.x86_64` | 1m 35s cold (empty SCons cache) | 2223 passed, 0 failed (3 pre-existing skips), master `d1f126ee` | Khronos validation layer 1.4.363.1 and `vulkaninfo` 1.4.363.1 from the LunarG SDK, loaded through `VK_LAYER_PATH` (no system SDK) |

Recorded on 2026-11-23 at `d1f126ee`.

- **Device:** `vendorID` 0x10DE, `deviceID` 0x2B85, `rayQuery` true, `maxInstanceCount` 16,777,215, mesh and task shaders. Archive: `fleet/vulkaninfo/g0/the RTX 5090 machine-rtx5090.json` (`vulkaninfo --json=0`).
- **PCIe:** Gen 5 x16 maximum (`nvidia-smi --query-gpu=pcie.link.gen.max,pcie.link.width.max`); the link drops to Gen 1 at idle.
- **Clocks and power:** maximum graphics 3105 MHz, memory 14001 MHz, power limit 600 W, VBIOS 98.02.2E.80.39. Clock locks for timing runs are set in PQ.
- **Display:** rendered runs use a private Xvfb display (1920x1080x24, no TCP) that each run starts and stops; NVIDIA's Vulkan driver presents to it. Pass `--audio-driver Dummy`.
- **Smoke (2026-11-23, master `d1f126ee`, Vulkan with `--gpu-validation`):** `scenes/nextgi_perf/nextgi_perf.tscn` (NextGI on, `tier=HWRT reason=NONE quality=HIGH`) and `scenes/vsm_flythrough/vsm_flythrough.tscn`, 300 frames each, 0 errors and 0 validation messages. The first headless `--import` of a fresh checkout logs the known editor-thread `is_visible_in_tree()` error.

| Device name | v1 role | Godot Vulkan `--gpu-index` | `nvidia-smi -i` | `vulkaninfo --json=<n>` |
|---|---|---|---|---|
| `NVIDIA GeForce RTX 5090` | Second fleet device: Linux Vulkan now, Windows 11 VM later; every NVIDIA line in PQ | `0` | `0` | `0` |
| `llvmpipe (LLVM 21.1.8, 256 bits)` | Software rasterizer, not a test device | `1` | n/a | `1` |

### Cloud L4 VMs (v3.8, build-time only)

Compute Engine `g2-standard-16` VMs (16 vCPU, 64 GB) with one NVIDIA L4 each run PR basics during the build (plan §23.2, v3.8). There are two images, Linux and Windows; since plan v3.9 the build runs as many VMs of each as it has parallel work for. They run no gate, timing or UE-comparison line and no DLSS-enabled build. Their project, names, addresses and SSH key are in the git-ignored `local/fleet.env`; the fleet skill says how to start, stop and reach them.

| VM | System | GPU and driver | Compiler | Build |
|---|---|---|---|---|
| Cloud Linux VM | Ubuntu 24.04, kernel 7.0 (gcp) | NVIDIA L4, driver 580.178.04 (Vulkan 1.4.312), plus llvmpipe; headless Xorg on the L4 at `DISPLAY=:0` | GCC 13 | `scons platform=linuxbsd target=editor dev_mode=yes -j16`, 14 min cold |
| Cloud Windows VM | Windows Server 2025 Datacenter | NVIDIA L4, GRID driver 621.14 in WDDM mode; Vulkan SDK 1.4.363.1 | MSVC 17.14 | `scons platform=windows target=editor dev_mode=yes d3d12=yes -j16`, 20 min cold |

Neither VM has an audio device: pass `--audio-driver Dummy`, or ALSA (Linux) and WASAPI (Windows) log `ERROR:` lines that fail the validation bar. The Windows VM has the Graphics Tools optional feature for the D3D12 debug layer (`--gpu-validation` on D3D12); D3D12 HWRT reports `D3D12_NO_DXR` until PD lands, as on the RTX 4080 SUPER.

**Vulkan synchronization validation with async compute (#193, plan v3.9.15).** The Linux VM image's system Vulkan validation layer, 1.3.275 (Ubuntu 24.04's package), reports false hazards when `rendering/rendering_device/async_compute` is on: `RACING` hazards between command buffers that the render graph's semaphores order, and a write-after-write hazard after each readback that stalls the frame. Layer 1.4.363.1 reports none on the same runs. Judge async compute runs on layer 1.4.363.1 or later: the Windows VM's Vulkan SDK has it; on a Linux VM, load a 1.4.363.1 build of the layer through `VK_LAYER_PATH`. Set `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` (`validation` jobs set it) and keep the layer's submit-time validation on (`syncval_full_validation`, on by default in 1.4.363.1; `VK_KHRONOS_VALIDATION_SYNCVAL_FULL_VALIDATION=true` sets it explicitly), which reports hazards between command buffers and between queues when they are submitted. From layer 1.4.x a hazard reads `<command>(): <KIND> hazard detected`, with no `SYNC-HAZARD` or `Validation Error` text; it is still logged as an `ERROR:` line, and `hazard detected` counts it. A 0-hazard async run counts only beside a run of the committed positive control `tests/async_compute/cross_queue_hazard_probe.gd` on the same layer and settings, with async compute on and `--gpu-validation`: it must report `vkQueueSubmit()` `WRITE_RACING_WRITE` hazards that name two different queues. None means submit-time validation is off. The probe's header gives the command line and its expected output.

D3D12 GPU-based validation runs, on either Windows machine, add `rendering_device/d3d12/gpu_based_validation=true` under `[rendering]` in the run's `override.cfg` to `--gpu-validation` (#272). A run counts only when its log has both `D3D12: GPU-based validation is on.` and `D3D12: Debug layer messages go to the log.`. Include `res://tests/rd_parity/storage_atomics.tscn` in every such run. Under GPU-based validation, atomics whose result is unused are lost (#277), so the fixture, `tests/tier/tier_check.gd` and the runner's `GODOTNEXT_RUNNER` line report the results that depend on them as `not_checked`; the run checks the validation messages. A build that links the GodotNext Mesa NIR library 25.3.1-gn3 or later keeps those atomics (#279): its log also has `D3D12: GPU-based validation keeps atomics whose result is unused`, the runner line has no `not_checked` note, and the fixture and `tier_check.gd` must pass outright: the runner fails such a run, with exit 1 and `"not_checked_with_kept_atomics":true`, when a result still reports `"not_checked"`.

On the Windows VM the user signs in automatically, so rendered runs go through `runners/win_session1.ps1` in Session 1 as on the RTX 4080 SUPER machine; its OpenSSH default shell is PowerShell. `run_remote.sh` reaches it with `GODOTNEXT_RTX_HOST`, `GODOTNEXT_RTX_REPO` and `GODOTNEXT_RTX_SSH_OPTS` (the key) set for the run. The L4 is its only GPU apart from the WARP software adapter:

| Device name | v1 role | Godot Session-1 Vulkan `--gpu-index` | Godot Session-1 D3D12 `--gpu-index` | `nvidia-smi -i` | Session-0 `vulkaninfo --json=<n>` |
|---|---|---|---|---|---|
| `NVIDIA L4` | Cloud build-time device (no PQ lines) | `0` | `0` | `0` | `0` |

### Cloud test machine (plan v3.9.7, build-time only)

Until the RTX 5090 machine runs Windows 11 with its card (#227, plan v3.9.20), a Compute Engine Windows VM with one NVIDIA RTX PRO 6000 (Blackwell, the RTX 5090's chip) is the second test machine (lead decision 2026-11-16, plan §23.2). It runs once its GPU quota is granted; until then, the Windows L4 VMs above stand in for it. When it is created it gets a build row and a device-mapping table like the L4 VM's, and its project, name, zone and address go in the git-ignored `local/fleet.env`, as for the L4 VMs.

- A landing check that says it renders on the RTX 4080 SUPER may run on the RTX 4080 SUPER machine or on the cloud test machine. The issue comment that reports it names the machine.
- Like the L4 VMs, it runs no gate line, gives no judged timing or UE-comparison verdict and never runs a DLSS-enabled build, and it is stopped when idle.
- PQ still runs on the RTX 5090 machine (#227); the PQ fleet is unchanged.

### Functional-coverage machines (plan v3.9.9)

v1 supports the GPU vendors, hardware generations, operating systems and API versions that UE 5.9 supports (lead decision 2026-11-16, plan §26). Outside the PQ fleet these classes get functional coverage only: the build, doctests, 0 validation errors, the phase smoke scenes and the expected NextGI tier and fallback reason. They run no gate line, timing line or UE comparison (plan §26.3). The runs are #281; the machine setup is #280.

| Machine | Class it covers | System and APIs | Status |
|---|---|---|---|
| Cloud NVIDIA T4 VM (the cloud T4 test machine) | NVIDIA Turing, UE's minimum hardware ray tracing class ("NVIDIA RTX-2000 series or newer") | Windows, D3D12 and Vulkan | Ran 2026-11-16 to 2026-11-19, deleted 2026-11-20; device mapping below; build row and `vulkaninfo` to add here when it is recreated (#280) |
| Cloud AMD Radeon PRO V710 VM | AMD RDNA3, UE's "AMD RX-6000 series or newer" class | Windows, D3D12 and Vulkan | Being set up by the lead; build row, device mapping and `vulkaninfo` to add here (#280) |
| Cloud Linux VM (above) | NVIDIA on Linux (UE's Linux floor: NVIDIA 570 or newer) | Ubuntu 24.04, Vulkan | In use |

- They follow the cloud VM rules above: no DLSS-enabled build, stopped when idle, and their names and addresses only in the git-ignored `local/fleet.env`.
- UE's minimum OS versions get one functional run each in PQ: Windows 10 22H2 and Rocky Linux 8, on a fleet machine that can boot them (#282). macOS 14.5 is a known gap: the M5 Max cannot run it.
- These runs and the machines' runs are reported, not blocking (lead decision 2026-11-16, plan v3.9.11): a machine or OS that is not available when PQ runs, for example no Windows 10 22H2 host, is reported as a coverage gap, and MQ does not wait on it. A real fault a run finds (a crash, a validation error, a wrong tier or a wrong image) holds MQ: its fix is a v1 bug.
- During the build, an issue box that needs one of these machines does not hold the issue (plan v3.9.11): the issue can close on the main machines plus WARP or DXIL checks, the run on the vendor machine is reported in the issue when the machine is available, and a failure then reopens the issue.
- Supported, but with no fleet device (known gaps, not hardware to buy, plan §26.5): Intel Arc (discrete, and integrated with RT units), AMD integrated GPUs (which take the software tier, plan §26.2), AMD and Intel on Linux, AMD GCN and RDNA1, NVIDIA Ampere, Pascal and Maxwell, and Apple M1 to M4.
- The Intel UHD 770 in the RTX 4080 SUPER machine is not a test device (plan §26.4). A one-off informational run on it, such as #176's bindless check, is allowed and is not coverage.

On the cloud T4 test machine the user signs in automatically, so rendered runs go through `runners/win_session1.ps1` in Session 1, as on the Windows L4 VM. Godot names the GPU `Tesla T4` on Vulkan and `NVIDIA Tesla T4` on D3D12, so a job names the device the way its driver reports it; the other driver's column is `n/a`, so a job that pairs a name with the wrong driver fails. D3D12 also lists two `Microsoft Basic Render Driver` adapters (WARP), at `1` and `2`; Vulkan lists only the T4:

| Device name | v1 role | Godot Session-1 Vulkan `--gpu-index` | Godot Session-1 D3D12 `--gpu-index` | `nvidia-smi -i` | Session-0 `vulkaninfo --json=<n>` |
|---|---|---|---|---|---|
| `Tesla T4` | Functional-coverage device, NVIDIA Turing (TU104, PCI `0x10DE:0x1EB8`); no PQ lines (plan §26.5); the name Vulkan reports | `0` | n/a | `0` | to record (#280) |
| `NVIDIA Tesla T4` | The same T4, under the name D3D12 reports | n/a | `0` | `0` | to record (#280) |
| `Microsoft Basic Render Driver` | Software WARP adapters (unused); D3D12 lists two, at `1` and `2` | n/a | `1` | n/a | n/a |

### Vulkan capability archive (`docs/godotnext/fleet/vulkaninfo/g0/`)

Raw `vulkaninfo` JSON snapshots (both well under 5 MB):

- `docs/godotnext/fleet/vulkaninfo/g0/the RTX 4080 SUPER machine-rtx4080super.json` (441 KiB, captured via `vulkaninfo --json=1`)
- `docs/godotnext/fleet/vulkaninfo/g0/the Linux VM-llvmpipe.json` (193 KiB, captured via `vulkaninfo --json=0`)

| Field | `the RTX 4080 SUPER machine` (`rtx4080super`) | `the Linux VM` (`llvmpipe`) |
|---|---|---|
| `deviceName` | `NVIDIA GeForce RTX 4080 SUPER` | `llvmpipe (LLVM 22.1.8, 256 bits)` |
| `vendorID` / `deviceID` | `0x10de` (`4318`) / `0x2702` (`9986`) | `0x10005` (`65541`) / `0x0000` (`0`) |
| `deviceType` | `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU` | `VK_PHYSICAL_DEVICE_TYPE_CPU` |
| `driverName` / `driverInfo` | `NVIDIA` / `621.01` | `llvmpipe` / `Mesa 26.2.2-arch1.1 (LLVM 22.1.8)` |
| `apiVersion` | `1.4.357` (`4211045`) | `1.4.354` (`4211042`) |
| NVIDIA driver (`nvidia-smi`) | `621.01` | n/a |
| `VK_KHR_acceleration_structure` | yes (rev `13`) | yes (rev `13`) |
| `VK_KHR_ray_query` | yes (rev `1`) | yes (rev `1`) |
| `VK_KHR_ray_tracing_pipeline` | yes (rev `1`) | yes (rev `1`) |
| `VK_KHR_spirv_1_4` | yes (rev `1`) | yes (rev `1`) |
| `VK_KHR_deferred_host_operations` | yes (rev `4`) | yes (rev `4`) |
| `VK_NV_ray_tracing_validation` | no by default; yes (rev `1`) when `NV_ALLOW_RAYTRACING_VALIDATION=1` is set | no |
| `VK_EXT_device_fault` | yes (rev `2`) | no |
| `bufferDeviceAddress` | `true` | `true` |
| `accelerationStructure` | `true` | `true` |
| `rayQuery` | `true` | `true` |
| `maxGeometryCount` | `16777215` | `16777215` |
| `maxInstanceCount` | `16777215` (`>= 100000`) | `16777215` |
| `maxPrimitiveCount` | `536870911` | `536870911` |
| `maxPerStageDescriptorAccelerationStructures` | `1048576` | `1015808` |
| `minAccelerationStructureScratchOffsetAlignment` | `128` | `8` |
| `VK_FORMAT_R16G16B16A16_UNORM` `VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR` in `bufferFeatures` | yes | yes |

`jq` paths used against `docs/godotnext/fleet/vulkaninfo/g0/<host>-<device>.json`:

```
.capabilities.device.properties.VkPhysicalDeviceProperties | {deviceName, vendorID, deviceID, deviceType, apiVersion}
.capabilities.device.properties.VkPhysicalDeviceVulkan12Properties | {driverName, driverInfo}
.capabilities.device.extensions["VK_KHR_acceleration_structure"]   # and the other extension names above
.capabilities.device.features.VkPhysicalDeviceVulkan12Features.bufferDeviceAddress
.capabilities.device.features.VkPhysicalDeviceAccelerationStructureFeaturesKHR.accelerationStructure
.capabilities.device.features.VkPhysicalDeviceRayQueryFeaturesKHR.rayQuery
.capabilities.device.properties.VkPhysicalDeviceAccelerationStructurePropertiesKHR | {maxGeometryCount, maxInstanceCount, maxPrimitiveCount, maxPerStageDescriptorAccelerationStructures, minAccelerationStructureScratchOffsetAlignment}
.capabilities.device.formats.VK_FORMAT_R16G16B16A16_UNORM.VkFormatProperties.bufferFeatures | index("VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR") != null
```

### Expected NGX requirements (unverified) — the RTX 4080 SUPER

Recorded per plan v2.4 without downloading any NVIDIA SDK; the RTX 5090 Linux runtime check (#100) is recorded right below:

| Requirement | Value | Query |
|---|---|---|
| Vulkan `apiVersion` (`>= 1.1`) | `1.4.357` — yes (`>= 1.1`) | `.capabilities.device.properties.VkPhysicalDeviceProperties.apiVersion` |
| NVIDIA driver version | `621.01` | `nvidia-smi --query-gpu=driver_version --format=csv,noheader` |
| `VK_NVX_binary_import` | yes (rev `2`) | `.capabilities.device.extensions["VK_NVX_binary_import"]` |
| `VK_NVX_image_view_handle` | yes (rev `4`) | `.capabilities.device.extensions["VK_NVX_image_view_handle"]` |
| `VK_KHR_push_descriptor` | yes (rev `2`) | `.capabilities.device.extensions["VK_KHR_push_descriptor"]` |

### Verified NGX Vulkan extension requirements (#100) — the RTX 5090 machine

Recorded on 2026-11-24 with NVIDIA DLSS SDK `v310.9.16` (`libnvidia-ngx-dlss.so.310.9.16`, SHA-256 `30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6`), NVIDIA Linux driver `595.99.02` (Vulkan `1.4.329`), and `VK_LAYER_KHRONOS_validation` `1.4.363.1` with SyncVal + `--gpu-abort`:

- **Required instance extensions (1):** `VK_KHR_get_physical_device_properties2`.
- **Required device extensions reported by NGX (4 raw; 3 merged on Vulkan 1.2+):**
  - `VK_NVX_binary_import` (**required**; settled by #100)
  - `VK_NVX_image_view_handle` (**required**)
  - `VK_KHR_push_descriptor` (**required**)
  - `VK_EXT_buffer_device_address` (reported by `NVSDK_NGX_VULKAN_RequiredExtensions`; filtered out when `VK_KHR_buffer_device_address` is supported and enabled, since Vulkan forbids enabling both under `VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328`).
- **Validation result:** 0 validation errors and 0 SyncVal hazards on `upscaler_torture` (Preset K Quality 0.67, default and `GODOT_RG_FULL_BARRIERS=1 GODOT_RG_REORDER=0`) and `nextgi_perf` (NextGI HWRT + DLSS DLAA 1.0 Preset K), with `--gpu-validation --gpu-abort`.

### Timing clock locks — the RTX 4080 SUPER machine

From `nvidia-smi -q -d CLOCK,SUPPORTED_CLOCKS` on `NVIDIA GeForce RTX 4080 SUPER` (`nvidia-smi` index `0`):

| Setting | Value / Command |
|---|---|
| `nvidia-smi` GPU index | `0` (`NVIDIA GeForce RTX 4080 SUPER`) |
| Base graphics clock | `2295` MHz (`nvidia-smi -i 0 --lock-gpu-clocks=2295,2295`) |
| Base memory clock | `11501` MHz (`nvidia-smi -i 0 --lock-memory-clocks=11501,11501`) |
| Reset after timing job | `nvidia-smi -i 0 -rgc` and `nvidia-smi -i 0 -rmc` |

### Plan §18.2 capability answers

- **Does the Linux VM's Mesa 26.2.2 lavapipe (`llvmpipe`) expose `VK_KHR_ray_query`?** **Yes** (`VK_KHR_ray_query` rev 1, `VK_KHR_acceleration_structure` rev 13, `rayQuery = true`, `accelerationStructure = true`, `bufferDeviceAddress = true`). Under `hardware_ray_tracing = Auto`, `VK_PHYSICAL_DEVICE_TYPE_CPU` selects the software tier (SWRT) with `reason=CPU_DEVICE`; under `Force`, functional ray-query tests can run on the Linux VM.
- **Is a GTX device available?** **No**; GTX ray-query exposure stays unverified.

### Refresh at each gate (PQ checklist)

In PQ (§22), before recording each gate report (`g0`, `g1`, `g2`, `g3`, `g4`, `gs`, `gm`, `gp`, `gt`, `gu`, `gu+`, `gd`) on the RTX 4080 SUPER machine, the Linux VM, and the RTX 5090 machine (#227):

1. Capture fresh `vulkaninfo --json=<n>` files into `docs/godotnext/fleet/vulkaninfo/<gate>/` using the same `<host>-<device>.json` filenames (resolving `<n>` with `vulkaninfo --summary`).
2. Re-run the `jq` queries above and `nvidia-smi --query-gpu=driver_version --format=csv,noheader`.
3. Diff `driverInfo`, NVIDIA driver version, `apiVersion`, and the ray-tracing / expected NGX extension revisions against the previous gate directory, and attach the refreshed files to the gate report.

## Ray-query toolchain (G0)

This section is the issue #20 record. Keep it separate from the #11 section.

The check proves two things. Godot's glslang compiles the `#version 460` `GL_EXT_ray_query` self-test to SPIR-V 1.3, `spirv-val --target-env vulkan1.1` accepts that module, and SPIRV-Cross turns `rayQuery` into `intersection_query`. On a device without ray query, `RendererRD::NextGI::run_toolchain_selftest` returns `{"pass":false,"reason":"NO_RAY_QUERY"}` and does not construct `NextgiRqSelftestShaderRD`. Metal ray query lands with #48 and #47, so the Apple M5 Max takes that path today. NVIDIA acceptance of the SPIR-V 1.3 module is #22.

### Re-run

CI runs `--test` on Linux and Windows runners that do not have `spirv-val` or `spirv-cross`. The doctest is that in-engine proof. It expands `nextgi_rq_selftest.glsl` through `ShaderRD` (`NextGI::build_toolchain_selftest_source`, which includes `nextgi_rt_inc.glsl`), compiles that source with `compile_glslang_shader` at Vulkan 1.1 / SPIR-V 1.3 (the Vulkan compute target in `rendering_shader_container_vulkan.cpp`), and checks the SPIR-V magic `0x07230203`, the version word `0x00010300`, one `rayQueryProceedEXT`, no candidate loop, and the absence of `rayQueryGetIntersectionTriangleVertexPositionsEXT`, `ShaderBindingTableRecordOffset`, and `ray_tracing_position_fetch`. A second case checks the `NO_RAY_QUERY` JSON when the device has no ray query. The doctest does not call either external tool. It writes the compiled module only when `GODOTNEXT_RQ_SPIRV_OUT` is set, and writes nothing otherwise.

The PR check on the Apple M5 Max is `misc/scripts/godotnext_rq_toolchain_check.sh`. It runs the compile doctest with `GODOTNEXT_RQ_SPIRV_OUT` set, then:

- `spirv-val --target-env vulkan1.1 <module.spv>`
- `spirv-cross <module.spv> --msl --msl-version 30000 --stage comp`

The script fails if either tool fails or the MSL does not contain `intersection_query`. The binary path is the first argument and defaults to `bin/godot.macos.editor.arm64` from the repository root. `GODOTNEXT_SPIRV_VAL` and `GODOTNEXT_SPIRV_CROSS` override the tool paths. Otherwise the script uses `spirv-val` and `spirv-cross` from `PATH`.

```
misc/scripts/godotnext_rq_toolchain_check.sh
```

Runtime hook, after `gi.init` in `RendererSceneRenderRD::init`:

```
GODOTNEXT_RQ_SELFTEST=1 GODOTNEXT_SPIRV_DUMP_DIR=<dir> <bin> --path <project> --rendering-driver metal --rendering-method forward_plus --gpu-index 0 --quit-after 3
```

Set `rendering/shader_compiler/shader_cache/enabled` to false in that project. A shader-cache hit loads bytecode in `ShaderRD::_load_from_cache` and never calls `_compile_variant`, so the dump is skipped. The dump writes every `ShaderRD` variant that compiles, as `<dir>/<name>.v<variant>.<stage>.spv`. The self-test signal is the presence or absence of `NextgiRqSelftest*.spv`.

Since #15 the self-test is a ray-query ShaderRD like any other, so it initializes only under the NextGI HWRT tier (plan §3.1). On a device with ray query, the project also needs `rendering/global_illumination/nextgi/enabled = true` and `hardware_ray_tracing` other than Off. Otherwise the report names the tier reason (for example `{"pass":false,"reason":"SETTING_OFF"}`) or `DISABLED`. A device without ray query still reports `NO_RAY_QUERY`. For a one-off run, put the setting in the project's `override.cfg`, as `godotnext/testbed/tests/tier/run_tier_check.sh` does.

### D5 source lint

`rg` searches comments, so the forbidden tokens must not appear in the `.glsl` text at all. The pattern is:

```
rg -n "rayQueryGetIntersectionTriangleVertexPositionsEXT|accelerationStructureEXT\s*\(|ShaderBindingTableRecordOffset|ray_tracing_position_fetch" servers/rendering/renderer_rd/shaders godotnext/testbed --glob '*.glsl'
```

`accelerationStructureEXT` followed by `(` is the 64-bit address constructor (`OpConvertUToAccelerationStructureKHR`). An `accelerationStructureEXT` uniform is allowed. The header of `nextgi_rt_inc.glsl` states the same rule: opaque-only (`gl_RayFlagsOpaqueEXT`), one `rayQueryProceedEXT`, no candidate loop, shared memory at or under 8 KiB, and none of the three constructs SPIRV-Cross MSL cannot translate.

`godotnext/testbed` is not in the tree yet (the testbed is a later issue). On 2026-11-03 the command above exits 2 because that directory is missing. The same pattern limited to `servers/rendering/renderer_rd/shaders` exits 1, which is ripgrep's "no matches" status. When the testbed lands, the full command covers its kernels too.

### Recorded on 2026-11-03

| Device | Driver | SPIR-V | spirv-val | Pipeline | Validation-layer errors |
| --- | --- | --- | --- | --- | --- |
| Apple M5 Max (40-core GPU, macOS 27.0.1) | Metal 4.0, device 0: Apple M5 Max (Apple9). `--gpu-index 0`, Forward+. | Offline module is 1.3, version word `0x00010300` (magic `0x07230203`). The runtime path does not compile the kernel. | SPIRV-Tools v2026.4, 2026-09-10. PR check `misc/scripts/godotnext_rq_toolchain_check.sh`: `spirv-val --target-env vulkan1.1` from `PATH`, exit 0, empty output. No `NextgiRqSelftest*.spv` was dumped (280 other engine shaders were), so there is no runtime dump to validate. | Not created. `GODOTNEXT_RESULT {"pass":false,"reason":"NO_RAY_QUERY"}`. Process exit 0. | Not run. The kernel is not submitted. Metal ray query is #48 and #47. |
| RTX 4080 SUPER machine | Recorded on 2026-11-13 under #50; see [NextGI on Vulkan/NVIDIA (#50)](#nextgi-on-vulkannvidia-50). | 1.3, version word `0x00010300` on all 18 ray-query modules. | 0 failures. | Created; `GODOTNEXT_RESULT` `pass:true`. | 0 in every G1 scene. |
| Linux VM | pending (#22) | pending (#22) | pending (#22) | pending (#22) | pending (#22) |

Apple M5 Max MSL line from the PR check script, SPIRV-Cross 1.4.363.0 (timestamp 2026-09-16), `spirv-cross --msl --msl-version 30000 --stage comp`, exit 0. On this machine `PATH` resolves `spirv-val` and `spirv-cross` to `/opt/homebrew/bin`.

```
NextGIHit nextgi_trace_opaque(thread const float3& origin, thread const float& tmin, thread const float3& dir, thread const float& tmax, thread const uint& mask, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rq, const raytracing::acceleration_structure<raytracing::instancing> nextgi_tlas)
```

The headless doctest saw the same `NO_RAY_QUERY` JSON, because `--headless --test` has no `RenderingDevice`. The windowed Metal run is the device-0 result in the table. Both paths return before `NextgiRqSelftestShaderRD` is constructed.

G0-1 (0 `spirv-val` errors on every dumped ray-query module, on this machine and on the RTX 4080 SUPER machine) is a PQ measurement under #24. The offline module on this machine was clean. The RTX 4080 SUPER row was recorded under #50, with driver 621.14 and `--gpu-validation` (see [NextGI on Vulkan/NVIDIA (#50)](#nextgi-on-vulkannvidia-50)). The Linux VM row is still pending.

## NextGI on Vulkan/NVIDIA (#50)

Recorded on 2026-11-13 on the RTX 4080 SUPER machine, with:

- NVIDIA driver 621.14, which is newer than the 621.01 in the G0 archive;
- Vulkan 1.4.357 and `VK_LAYER_KHRONOS_validation` 1.4.363;
- the Windows editor built with `dev_mode=yes d3d12=yes`;
- `--gpu-index 0` in Session 1, run through `runners/win_session1.ps1`.

The operator scripts are git-ignored local copies. Every run below goes through `run_remote.sh job`.

### G1 scenes under the validation layers

Each G1 scene runs for 300 frames as a kind `validation` job. That means `--gpu-validation`, `NV_ALLOW_RAYTRACING_VALIDATION=1` and `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`.

The NextGI scenes run with a temporary root `override.cfg`:

- `nextgi/enabled=true`;
- `hardware_ray_tracing=0` (Auto);
- `quality` set to the column's value.

Every NextGI launch printed `tier=HWRT reason=NONE`. The legacy scenes run with NextGI off.

Each cell gives the run's `ERROR:` lines, then its `Validation Error` lines.

The High column comes from the final build. The Medium and Epic columns come from the build just before it. That build lacked only two changes: the relight fallback fill value and the shared variant-define helpers.

| Scene | Medium | High | Epic |
| --- | --- | --- | --- |
| `color_box_room/cbr_a` | 0 / 0 | 0 / 0 | 0 / 0 |
| `color_box_room/cbr_b` | 0 / 0 | 0 / 0 | 0 / 0 |
| `color_box_room/cbr_offscreen` | 0 / 0 | 0 / 0 | 0 / 0 |
| `color_box_room/cbr_single_40m` | 0 / 0 | 0 / 0 | 0 / 0 |
| `concave_courtyard/courtyard` | 0 / 0 | 0 / 0 | 0 / 0 |
| `moving_light_door/moving_light_door` | 0 / 0 | 0 / 0 | 0 / 0 |
| `time_of_day/time_of_day` | 0 / 0 | 0 / 0 | 0 / 0 |
| `nextgi_perf/nextgi_perf` | 0 / 0 | 0 / 0 | 0 / 0 |
| `legacy/legacy_lightmap` (NextGI off) | 0 / 0 | 0 / 0 | 0 / 0 |
| `legacy/legacy_sdfgi` (NextGI off) | 0 / 0 | 0 / 0 | 0 / 0 |
| `legacy/legacy_voxelgi` (NextGI off) | 0 / 0 | 0 / 0 | 0 / 0 |

The software tier ran the same scenes on the final build with `hardware_ray_tracing=1` (Off), at High and at Medium (Lumen Lite). It reported `tier=SWRT reason=SETTING_OFF`, and all 22 runs had 0 `ERROR:` and 0 `Validation Error` lines.

The first pass found the following, and #50 fixed each one:

- **`nextgi_perf`, Auto (Medium), 23 errors.** NextGI relight's fallback shadow texture is `D32_SFLOAT`, but it had no depth-stencil usage, so Vulkan gave it a colour-aspect view. It is now a depth attachment filled with 0, the reverse-Z far value. Against 0, relight's `GREATER` shadow compare reads as lit.
- **`nextgi_perf`, High, 10 errors.** Gather set 0 bound the SRC indirect texture twice: as a sampled texture at binding 5, which nothing read, and as the storage image it writes at binding 17. The render graph keeps a texture's first usage in a compute list, so the store ran with the image in `SHADER_READ_ONLY_OPTIMAL`. Binding 5 is removed.
- **`legacy_voxelgi`, 10 errors.** `GI::process_gi`, upstream code, had its sampler test inverted. It paired the linear sampler with the `R8G8_UINT` VoxelGI buffer, which no driver can filter linearly. The fix is a named hook.
- **`VK_EXT_device_fault`.** The driver enabled the extension but never queried or enabled the `deviceFault` feature, so a crash-latch report would call `vkGetDeviceFaultInfoEXT` on a device without it. The feature is now queried and enabled. The G0 archive shows the 4080 SUPER reports `deviceFault` and `deviceFaultVendorBinary`. No device loss was forced.
- **re-spirv noise.** re-spirv, the driver's spec-constant optimizer, cannot parse ray-query modules. It printed `OpTypeRayQueryKHR is not supported yet.` for each one, then kept the original module. The driver now skips it for modules that declare `RayQueryKHR`.

The startup 64-ray self-test passed on every NextGI launch (`tier=HWRT`).

Auto picked Medium on this card. Its benchmark was the whole self-test, about 11 to 125 ms, which includes first-dispatch setup, and the rule in `nextgi_auto_quality_from_usec` picks High only at 5 ms or less. Since #260 the benchmark is a warm second run of the self-test's rays: 0.37 ms on this card and 0.41 ms on the M5 Max, so Auto picks High on both. PQ re-checks the threshold.

On the software tier (#259), Auto times the WRC update sibling in the benchmark's fixture room, four frames of 512 probes of 64 rays in one dispatch, and picks High at 1.25 ms or less per frame. That is the 5 ms rule's ray rate, 26.2 rays per microsecond, carried from the earlier 2048-probe kernel. One frame alone (the first version) read 0.00 to 0.21 ms on the M5 Max, inside the submit-and-readback round trip's noise. Start values: 0.24 to 0.41 ms per frame over seven runs on the M5 Max (40 compute units), and 0.11 ms on the RTX 4080 SUPER in both of two runs, with 47% of the probe rays hitting the room. PQ re-checks the threshold.

### Velocity under TAA and FSR2, and the HWRT fault latch

**Prepass velocity (#37).** NextGI Perf (spline camera) and Moving Light Door (moving instance) ran at High as `validation` jobs, once with `anti_aliasing/quality/use_taa=true` and once with FSR2 (`scaling_3d/mode=2`, scale 0.77). All four runs had 0 `ERROR:` and 0 `Validation Error` lines. The velocity values themselves are covered by #37's doctests.

**Fault latch (#36).** The seven steps of `tests/tier/run_hwrt_safety.sh` ran as runner jobs on Vulkan, with a private user directory:

1. Auto selects HWRT and logs the self-test.
2. Off reports `SETTING_OFF` and runs no self-test.
3. `GODOTNEXT_NEXTGI_SELFTEST_CORRUPT=1` gives `SELFTEST_FAILED` without writing the latch.
4. `GODOTNEXT_NEXTGI_SIMULATE_DEVICE_LOST=1` crashes with the simulated-loss message and writes the latch.
5. The next launch reports `HWRT_FAULTED` and runs no self-test.
6. Force runs HWRT and keeps the file.
7. A changed `driver_key` deletes the latch and selects HWRT again.

Every step passed, and every launch except the crash had 0 errors. The simulated loss calls NextGI's latch callback directly, so the `VK_EXT_device_fault` report only runs on a real device loss, which was not forced.

### spirv-val on every ray-query module

The modules come from `GODOTNEXT_SPIRV_DUMP_DIR` runs of `nextgi_perf`, each 30 frames with `shader_compiler/shader_cache/enabled=false`:

- one run each at Medium, High and Epic;
- one more at High with `debug/reference_mode=1` and `GODOTNEXT_RQ_SELFTEST=1`.

The dumps hold 1541 modules, 390 of them distinct. 18 distinct modules declare `RayQueryKHR`:

- `NextgiGatherShaderRD` v0–v6;
- `NextgiWrcUpdateShaderRD` v0–v5;
- `NextgiReferenceShaderRD` v0–v1;
- `NextgiSrcRelightShaderRD`, `NextgiSelftestShaderRD` and `NextgiRqSelftestShaderRD`.

`spirv-val --target-env vulkan1.1` (SPIRV-Tools v2026.4) reports 0 failures on all 1541 modules. Every ray-query module carries version word `0x00010300`. Medium runs the irradiance field rather than the per-pixel gather, so the gather modules come from the High and Epic runs. The toolchain self-test printed `GODOTNEXT_RESULT {"pass":true,"spirv":"1.3","spirv_word":"0x00010300","pipeline":true,"shader":"NextgiRqSelftestShaderRD"}`.

### Shader baker

Before #50, the shader baker baked none of NextGI's ray-query versions. Its log still read `skipped 0`, because it only counted skips. The baker walks the ShaderRD versions that already exist in the editor process, and NextGI creates its ray-query ShaderRDs only after a NextGI frame on the HWRT tier. An export never renders one.

The baker now also walks `RendererRD::NextGIBakeShaders`. Those are bake-only instances of:

- the gather;
- the SRC relight, in both radiance formats;
- the WRC update;
- the reference tracer.

Each is initialized with its runtime twin's `get_shader_variant_defines()` and compiles nothing locally. The log line now reads `baked N and skipped M ray-query shader version(s)`.

| Preset | Exported on | Errors | Baker line | NextGI files in the pack |
| --- | --- | --- | --- | --- |
| Windows Vulkan (baker) | RTX 4080 SUPER machine, NextGI on | 0 | `driver 'vulkan', baked 5 and skipped 0` | 5 `.vulkan.cache` |
| macOS (baker) | Apple M5 Max, NextGI off | 0 | `driver 'metal', baked 5 and skipped 0` | 5 `.metal.cache` |

On the RTX 4080 SUPER machine, the baked paths for the gather, relight and WRC update match the ones the runtime wrote to the user shader cache during the survey. The reference tracer ran with the cache off, so it left nothing to compare.

Since #261 the bake also holds the software-tier siblings (the gather without its reference and far-field variants, both relight formats, the WRC update) and the startup self-test: 10 NextGI cache files on Windows Vulkan, 0 errors. After NextGI Perf ran on both tiers on the RTX 4080 SUPER, every NextGI path the runtime wrote (gather, relight and WRC on both tiers, and the self-test) is one the pack stores.

A Windows editor has no Metal baker platform, because `METAL_ENABLED` is Apple-only. Exporting macOS (baker) there therefore bakes nothing and prints no baker line.

`godotnext/testbed/captures/` now has a tracked `.gdignore`. Before it, an `all_resources` export imported harness output left in the folder, and the self-test's `timing.csv` files failed to load as translations, which logged 9 errors.

## Reference path tracer and ray-query parity on Vulkan (#49)

Recorded on 2026-11-13. The RTX 4080 SUPER ran Vulkan, driver 621.14, as runner jobs. The Apple M5 Max ran Metal.

**RD smoke test.** All four fixtures pass on both devices:

- `tri`, 2 rays;
- `rays`, 4096 rays;
- `edges`, 1024 rays;
- `winding`, 36 rays: the new `FLIP_FACING`, mirrored and cull-disable cases.

On Vulkan the run used `--gpu-validation` and logged 0 errors. Each device's dump also passes the Q17 tool's analytic mode.

Across APIs, `q17 --in <vulkan dump> --ref <metal dump>` gives:

- 0 failures;
- t at most 9.3e-6 relative;
- barycentrics at most 8.1e-6 absolute;
- front face equal on all 36 winding rays.

This is a report on the tooling. G2-1 itself is measured in PQ.

The Metal fallback modes assert their reasons and pass:

- `GODOT_MTL_SYNC_MODE=none` gives `METAL_SYNC_MODE`;
- `GODOT_MTL_DISABLE_ARGUMENT_BUFFERS=1` gives `METAL_NO_ARGUMENT_BUFFERS`.

The D3D12 fallback did not run, because D3D12 work waits for a go-ahead.

**v0 reference path tracer (#19).** Every gated v0 run passes on Vulkan:

- the furnace at 131072 spp: mean 4.9999841, with every pixel within 1 percent of 5;
- `sealed_a` and `sealed_b` at 65536 spp: Q3 is 0;
- the same-seed runs: identical SHA-256.

The runs used kind `run`. A 256-spp furnace under `--gpu-validation` logged 0 errors. `tests/reference/results/` keeps the Metal and Vulkan outputs under the same names. Its README records the deltas: the furnace differs by at most 4.3e-5 relative per pixel.

**Scene path (#27).** On Vulkan the furnace check first returned 1.0 everywhere, not 5. Since the reflection-only mode (#60), `reference_mode` had reached the shader unmapped. Full ran as direct-only, Direct Only as reflections-only, and Reflections Only as direct-only. The setting is now mapped to the shader's mode word (`nextgi_reference_shader_mode`, with a doctest).

All three checks now pass on both backends:

- the furnace: mean 5.0382 on Metal and on Vulkan;
- the leak check;
- the diagnostic, at High: Auto picks Medium on the RTX 4080 SUPER, and Medium does not run the per-pixel gather.

On Vulkan the leak check also logs 2 sync-validation hazards when it switches scenes (#262). Nothing writes the scene references that G1 scores against yet (#263).

## AS calibration on the RTX 4080 SUPER (#9, PR time)

`tests/as_calibration` ran on the RTX 4080 SUPER (Vulkan) and the M5 Max (Metal) with `--warmup=2 --iters=5`. A `--gpu-validation` run on Vulkan logged 0 errors. `godotnext_metrics.as_calibration` found every field it needs in both results. These short runs only check the fields; G0-6 measures the values in PQ with clocks locked.

The standalone Vulkan refit tool (`tools/as_refit_bench`) builds with MSVC and the Vulkan SDK on the RTX 4080 SUPER machine and the Windows L4 VM, and its CTest self-test passes on both. It runs over plain SSH. A `--validation` run, with synchronization validation and `VK_NV_ray_tracing_validation` on, logged 0 errors on both cards. A deliberately broken build of the tool (a missing barrier and a leaked buffer) logged both errors and exited 1, so the 0 is a real result. On the RTX 4080 SUPER machine, the issue's PQ command (`--device "RTX 4080 SUPER" --runs 3`, at the defaults) ran through and its JSON passed the derivation, a two-run validation run logged 0 errors, and `--device "UHD Graphics"` printed the fallback result (`NO_ACCELERATION_STRUCTURE`) and exited 0. The tool and the RD scene then ran on the same machine with `--warmup 2 --iters 5`, and the derivation took both. These are samples with unlocked clocks, not PQ figures:

| Card | Tool vs RD, TLAS p95 at 100k instances | Tool vs RD, FAST_TRACE BLAS throughput | Refit, RD / tool (200k triangles) | Refit used for line 1a |
|---|---|---|---|---|
| RTX 4080 SUPER | +6.4% | +0.3% | 1.036 | tool |
| L4 (build-time only) | +3.4% | +0.0% | 0.970 | tool |

The BLAS-set and TLAS memory are byte-identical between the two. Getting that match needed a fix in the RD scene: it now reads `MEMORY_ACCELERATION_STRUCTURES` right after creating the structures. Before, the 10k-instance TLAS memory came out negative, because RD frees the FAST_BUILD BLAS set only when a frame slot cycles.
