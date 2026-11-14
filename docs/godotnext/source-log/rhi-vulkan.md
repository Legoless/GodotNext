# Vulkan RHI source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used for Vulkan ray-query work, starting with the AS
refit tool of #9: what each input is, where it came from, its license, and how it
was used.

## Approved inputs

### Khronos Vulkan specification and headers, AS refit tool (#9)

| Field | Value |
|---|---|
| What | Public Vulkan API: `VK_KHR_acceleration_structure` (build sizes, `vkCmdBuildAccelerationStructuresKHR`, BUILD and UPDATE modes, in-place update with source equal to destination, scratch alignment, `VkAccelerationStructureInstanceKHR`), `VK_KHR_deferred_host_operations` as its required extension, Vulkan 1.2 buffer device address, Vulkan 1.3 synchronization2 (`vkCmdPipelineBarrier2`, `vkCmdWriteTimestamp2`), timestamp queries (`timestampPeriod`, `timestampValidBits`), and the access rules for build inputs (shader read in the acceleration-structure build stage) |
| Upstream | The Vulkan specification, https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html, and the SDK header `vulkan/vulkan_core.h` |
| Version | Vulkan 1.4 specification; Vulkan SDK 1.4.363.1 (`VK_HEADER_VERSION` 363) on the Windows L4 VM and 1.4.363.0 on the RTX 4080 SUPER machine |
| License | Specification source documents: CC-BY-4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc. Header: `SPDX-License-Identifier: Apache-2.0 OR MIT`, read in the installed `vulkan_core.h`. Consulted, not copied. No sample code was copied, and no header is vendored: the tool compiles against the installed SDK |
| Read on | 2026-11-14, for issue #9 |
| Consulted by | The #9 implementer |
| Used in | `godotnext/testbed/tools/as_refit_bench/as_refit_bench.cpp` |
| Copied files | None |

How it was used: the tool was written against the published API and compiled against the SDK header. It calls the public entry points directly, and loads the extension and Vulkan 1.3 device functions through `vkGetDeviceProcAddr`. A refit is a build with `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR`, with `srcAccelerationStructure` equal to `dstAccelerationStructure`, on a structure created with `ALLOW_UPDATE`, using scratch sized from `updateScratchSize`. Scratch addresses are aligned to `minAccelerationStructureScratchOffsetAlignment`. Timestamps are written at `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`, and their differences are masked to the queue's `timestampValidBits` and scaled by `timestampPeriod`. The measurement shape (one submit per iteration, a barrier after every build, instances in host-visible memory) copies this repository's RD benchmark and Vulkan driver, which are not outside sources.

### Khronos validation layer settings

| Field | Value |
|---|---|
| What | `VK_EXT_layer_settings` (`VkLayerSettingsCreateInfoEXT` in the instance `pNext` chain), the validation layer's `validate_sync` setting (synchronization validation) and its environment variable `VK_VALIDATION_VALIDATE_SYNC`, `VK_EXT_debug_utils` messengers, and `VK_NV_ray_tracing_validation` (`VkPhysicalDeviceRayTracingValidationFeaturesNV`) as a registered Vulkan extension |
| Upstream | https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/syncval_usage.md, https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_layer_settings.html and https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_ray_tracing_validation.html |
| Version | Vulkan-ValidationLayers `main` and the Vulkan reference pages as published on 2026-11-14; the layer itself is the one in Vulkan SDK 1.4.363 |
| License | Vulkan-ValidationLayers: Apache-2.0 for most files (its `LICENSE.txt`); `syncval_usage.md` carries a LunarG copyright line. The reference pages show no license notice of their own and are generated from the specification sources (CC-BY-4.0, above). Consulted, not copied |
| Read on | 2026-11-14, for issue #9 |
| Consulted by | The #9 implementer |
| Used in | `godotnext/testbed/tools/as_refit_bench/as_refit_bench.cpp` (`--validation`) |
| Copied files | None |

How it was used: `--validation` enables `VK_LAYER_KHRONOS_validation` and turns on `validate_sync` through `VkLayerSettingsCreateInfoEXT`, as `syncval_usage.md` shows. A deliberately broken build of the tool reported `SYNC-HAZARD-READ-AFTER-WRITE` this way. The tool counts error-severity validation messages through a debug-utils messenger, and fails the run on any. Where the driver exposes `VK_NV_ray_tracing_validation`, it also enables that extension with `rayTracingValidation`. The tool sets `NV_ALLOW_RAYTRACING_VALIDATION=1` first, as the Session-1 runner's validation jobs (`harness/runners/win_session1.ps1`) do; the reference page names no environment variable. No NVIDIA SDK file was read or used.
