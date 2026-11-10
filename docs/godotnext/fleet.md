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

Recorded on 2026-11-08 at base commit `17dd0086` (`4.8.beta.custom_build.17dd00861`). The RTX 5090 machine gets its rows through #227 before PQ (§23).

### Editor builds, doctests, and dev tools

| Host | OS | GPU | Compiler | Build command | Editor binary | Wall time | `--headless --test --force-colors` | Vulkan SDK and SPIR-V dev tools |
|---|---|---|---|---|---|---|---|---|
| Apple M5 Max | macOS 27.0.1 (26A434) | Apple M5 Max, 40 GPU cores, Metal 4 (`system_profiler SPDisplaysDataType`); Vulkan RT compiled out (MoltenVK), Metal HWRT active | Apple clang 21.0.0 (`clang-2100.4.1.1`) | `scons platform=macos target=editor dev_mode=yes arch=arm64 use_volk=yes -j8` | `bin/godot.macos.editor.arm64` | 10.82 s incremental (warm 2.5 GB SCons cache) | 1735 passed, 0 failed (3 pre-existing platform skips) | No LunarG SDK (`use_volk=yes`); `spirv-val`: `SPIRV-Tools v2026.4` (2026-09-10); `spirv-cross`: built 2026-09-16 (`/opt/homebrew/bin`) |
| the RTX 4080 SUPER machine | Windows 11 Pro (10.0.26100) | NVIDIA GeForce RTX 4080 SUPER (plus Intel UHD 770, unused in v1) | MSVC 19.44.35228 for x64 (VS 14.3, Windows SDK 10.0.26100.0) | `python misc/scripts/install_d3d12_sdk_windows.py`, then `scons platform=windows target=editor dev_mode=yes d3d12=yes -j16` | `bin\godot.windows.editor.x86_64.console.exe` | 9m 27s (567.2 s) cold; 34.20 s incremental | 1728 passed, 0 failed (3 pre-existing platform skips) | LunarG Vulkan SDK `1.4.363.0` (`VK_LAYER_KHRONOS_validation` 1.4.363); `spirv-val`: `SPIRV-Tools v2026.4 v2026.4.rc2-0-gef96ed76`; `spirv-cross`: `vulkan-sdk-1.4.357.0-39-gf11ba9f0` (2026-09-27) |
| the Linux VM | Linux 7.2.5-3 x86_64 (Arch-based, 4 vCPU, 15 GiB RAM) | None (`llvmpipe`, LLVM 22.1.8, 256 bits, Mesa 26.2.2) | `g++ (GCC) 16.2.1 20260810` | `scons platform=linuxbsd target=editor dev_mode=yes -j4` | `bin/godot.linuxbsd.editor.x86_64` | 25m 39s (1538.7 s) cold; 11.67 s incremental | 1734 passed, 0 failed (3 pre-existing platform skips) | System Vulkan loader `1.4.357` (`VK_LAYER_KHRONOS_validation` 1.4.357); `spirv-val`: `SPIRV-Tools v2026.3 vulkan-sdk-1.4.357.0-0-g9a49b0883` |

The dev tools (`spirv-val`, `spirv-cross`, LunarG Vulkan SDK) are local dependencies and are never vendored (§13.1).

### Device mapping on the RTX 4080 SUPER machine (Session 1 vs Session 0)

SSH Session 0 cannot create desktop swapchains. In the interactive desktop session (Session 1), `VK_LAYER_NV_optimus` on Vulkan and DXGI on D3D12 enumerate the discrete NVIDIA GPU first (`#0`), whereas headless Session-0 `vulkaninfo --summary` lists the integrated Intel UHD 770 at `GPU0` and the RTX 4080 SUPER at `GPU1`. Harnesses (#237) resolve indices by device name from this table rather than hardcoding an ordinal across tools:

| Device name | v1 role | Godot Session-1 Vulkan `--gpu-index` | Godot Session-1 D3D12 `--gpu-index` | `nvidia-smi -i` | Session-0 `vulkaninfo --json=<n>` |
|---|---|---|---|---|---|
| `NVIDIA GeForce RTX 4080 SUPER` | Primary NVIDIA build & gate device | `0` | `0` | `0` | `1` |
| `Intel(R) UHD Graphics 770` | Not a test device (§23) | `1` | `1` | n/a | `0` |
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

Recorded per plan v2.4 without downloading any NVIDIA SDK; #100 logs the actual runtime NGX requirement list:

| Requirement | Value | Query |
|---|---|---|
| Vulkan `apiVersion` (`>= 1.1`) | `1.4.357` — yes (`>= 1.1`) | `.capabilities.device.properties.VkPhysicalDeviceProperties.apiVersion` |
| NVIDIA driver version | `621.01` | `nvidia-smi --query-gpu=driver_version --format=csv,noheader` |
| `VK_NVX_binary_import` | yes (rev `2`) | `.capabilities.device.extensions["VK_NVX_binary_import"]` |
| `VK_NVX_image_view_handle` | yes (rev `4`) | `.capabilities.device.extensions["VK_NVX_image_view_handle"]` |
| `VK_KHR_push_descriptor` | yes (rev `2`) | `.capabilities.device.extensions["VK_KHR_push_descriptor"]` |

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
| RTX 4080 SUPER machine | pending (#22) | pending (#22) | pending (#22) | pending (#22) | pending (#22) |
| Linux VM | pending (#22) | pending (#22) | pending (#22) | pending (#22) | pending (#22) |

Apple M5 Max MSL line from the PR check script, SPIRV-Cross 1.4.363.0 (timestamp 2026-09-16), `spirv-cross --msl --msl-version 30000 --stage comp`, exit 0. On this machine `PATH` resolves `spirv-val` and `spirv-cross` to `/opt/homebrew/bin`.

```
NextGIHit nextgi_trace_opaque(thread const float3& origin, thread const float& tmin, thread const float3& dir, thread const float& tmax, thread const uint& mask, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rq, const raytracing::acceleration_structure<raytracing::instancing> nextgi_tlas)
```

The headless doctest saw the same `NO_RAY_QUERY` JSON, because `--headless --test` has no `RenderingDevice`. The windowed Metal run is the device-0 result in the table. Both paths return before `NextgiRqSelftestShaderRD` is constructed.

G0-1 (0 `spirv-val` errors on every dumped ray-query module, on this machine and on the RTX 4080 SUPER machine) is a PQ measurement under #24. The offline module on this machine was clean. The RTX 4080 SUPER dump, driver 621.01, `--gpu-validation`, and the Linux VM row move to #22.
