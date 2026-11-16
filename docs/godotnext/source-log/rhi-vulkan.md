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

### PCI ID database, Linux and Mesa GPU tables, and Intel pages: the RT-core denylist (#283)

| Field | Value |
|---|---|
| What | Device-ID facts for the RT-core check behind `SUPPORTS_RAY_QUERY_HARDWARE` (Vulkan and D3D12): which NVIDIA, AMD and Intel device IDs belong to parts without hardware ray-tracing units, which AMD device IDs are integrated GPUs (APUs), and which integrated Intel GPUs have RT units |
| Upstream | The PCI ID database, https://pci-ids.ucw.cz/v2.2/pci.ids. The Vulkan specification's `VkPhysicalDeviceProperties` (https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceProperties.html). A Jetson TX2 vulkaninfo excerpt posted in NVIDIA's developer forum on 2024-02-19 (https://forums.developer.nvidia.com/t/vulkaninfo-output/283155). The Linux kernel (https://github.com/torvalds/linux): `include/drm/intel/pciids.h`, `include/drm/drm_pciids.h` (radeon) and `drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c`. Mesa (https://gitlab.freedesktop.org/mesa/mesa): `include/pci_ids/iris_pci_ids.h`, `include/pci_ids/crocus_pci_ids.h` and `src/intel/dev/intel_device_info.c` (the per-platform `has_ray_tracing` flag). Intel: the Intel Core Ultra Processors media presentation (https://download.intel.com/newsroom/2023/ai/ai-everywhere-2023/Intel-Core-Ultra-Processors-Media-Presentation.pdf), the Intel Core Ultra Desktop Processors launch briefing of 2024-10-09 (linked from Intel's newsroom article "Intel Launches First AI PC Intel Core Ultra Desktop Processors") and Intel's newsroom article "New Core Ultra Processors Deliver Breakthrough Performance, Efficiency for the AI PC Age" (2024-09-03) |
| Version | pci.ids version 2026.11.16; Linux master at aebb72744764 (2026-11-15); Mesa main at 4fac8318e4a5 (2026-11-16); the Intel documents, the Vulkan reference page and the forum post as published on 2026-11-16 |
| License | pci.ids: GPL-2.0-or-later or BSD-3-Clause (its header), a compilation of factual data. Kernel files: MIT-style permission notices (`pciids.h`, `amdgpu_drv.c`) and `SPDX-License-Identifier: MIT` (`drm_pciids.h`). Mesa: MIT. Intel documents: Intel's copyright. Vulkan reference page: CC-BY-4.0. Forum post: its author's. Only facts (device IDs, family names, whether a GPU has RT units or is integrated) were taken; no code or text was copied |
| Read on | 2026-11-16, for issue #283 |
| Consulted by | The #283 implementer |
| Used in | `servers/rendering/rendering_context_driver.{h,cpp}` (`RAY_QUERY_HARDWARE_DENYLIST`, `check_ray_query_hardware_device`), `tests/servers/rendering/test_nextgi.cpp` |
| Copied files | None |

How it was used:

- NVIDIA. The PCI ID database lists NVIDIA's dies in ID order, each in its own block: nothing below TU102's first ID (0x1E02) is an RTX part, and the Turing dies without RT cores (TU116 0x2180-0x21FF, TU117 0x1F80-0x1FFF) and the datacenter compute dies (GA100, GH100, GB100, GB102 and GB110, named there for A100, H100, B200 and B300 products) have blocks of their own. Single parts without RT cores on RT dies (GeForce GTX 1660 SUPER and GTX 1650 on TU106, the MX570 on GA107, the CMP mining cards) are rows of their own, as the #22 allowlist already split them out. Not on the denylist, because no source states that they lack RT cores: GB10B (Jetson Thor, 0x2B00 and 0x3400), the GB20B "RTX Spark" GPUs, T256, and the unreleased GR100, GR102, GB112 and GB120.
- AMD. Every AMD device ID in 0x1000-0x1FFF that the PCI ID database lists as a GPU is an APU (Kaveri to Krackan, the console APUs), the kernel's amdgpu table flags all 37 of its entries there `AMD_IS_APU` and the radeon table flags its 22 `RADEON_IS_IGP`. That block, and the API's integrated device type for later APUs, is the AMD integrated rule (lead decision 2026-11-16: UE names only discrete AMD RX 6000 series or newer). Every discrete AMD GPU below Navi 21's block (0x73A0) is before RDNA2 in both the PCI ID database and the amdgpu table (RDNA1 Navi 1x, Vega, Polaris, CDNA1 Arcturus and older). CDNA2-4 (Instinct MI200, MI300, MI350) have blocks of their own, and the TeraScale and chipset-graphics IDs (0x78xx-0x79xx, 0x94xx-0x99xx) are rows that cover each page's listed IDs.
- Intel. Mesa's `has_ray_tracing` flag is true for DG2, ATS-M, Meteor Lake, Arrow Lake (Xe-LPG), Lunar Lake, Battlemage (Xe2), Panther Lake, Nova Lake U/H (Xe3) and Nova Lake P (Xe3P), and false for every earlier platform, Wildcat Lake and Nova Lake S/HX/UL. The rows list the non-RT IDs of Mesa's tables, the kernel's table (Cannon Lake, the early chipsets) and the PCI ID database's graphics entries missing from both (Lakefield, Moorestown, Poulsbo, Crystal Well and others), merged across unlisted IDs only inside a 0x100 block that holds no RT part. The database's Cedarview entries 0x0BE0-0x0BEF are left out: the kernel lists 0x0BE0, 0x0BE1 and 0x0BE5 as Ponte Vecchio, which has RT units. Intel's pages confirm RT units in the integrated GPUs: Meteor Lake's Xe-LPG has a "Full feature set with HW ray tracing", with Arc branding on select H-series configurations and "Intel Graphics" on the others (same device IDs, so both count); Arrow Lake S lists "4 Ray tracing Units"; Lunar Lake's Arc GPU has "eight enhanced ray tracing units".
- Checked with a script over these tables (in the implementer's lane folder, not committed): every Mesa RT ID passes, every Mesa and kernel non-RT ID is denylisted, and against the #22 allowlist the only GPUs that change are the RDNA2-and-later APUs (now off HWRT) and IDs the allowlist did not list.
- Device IDs above 0xFFFF. The rows are PCI device IDs, and the Vulkan specification puts a PCI device ID in the low 16 bits of `deviceID` with "the remaining bits" zero; a device not "driven primarily by a PCI device" returns a value chosen by "operating system or platform policies". The forum's Jetson TX2 output shows one: vendor 0x10DE, device 0x93BA03D7, `INTEGRATED_GPU`, "NVIDIA Tegra X2 (nvgpu)". "Every ID below the first RT die" therefore holds only for 16-bit IDs, so an NVIDIA, AMD or Intel device ID above 0xFFFF does not count, as under the #22 allowlist, which had no such row.
- The doctests restate every row's first and last ID and test the RT parts just outside the rows (TITAN RTX after the pre-Turing row, Navi 21 after the pre-RDNA2 row, Nova Lake U and H beside Nova Lake S and HX, and others), and every AMD APU ID the #22 allowlist listed.

### Vulkan driver properties and Mesa's RADV driver string: Linux driver minimums (#285)

| Field | Value |
|---|---|
| What | `VkPhysicalDeviceDriverProperties` (`driverID`, `driverInfo`) and the `VkDriverId` values `VK_DRIVER_ID_MESA_RADV` (3) and `VK_DRIVER_ID_NVIDIA_PROPRIETARY` (4); RADV's `driverInfo`, which is "Mesa " followed by Mesa's version and an optional git suffix; UE 5.9's Linux minimums, "Vulkan: AMD (RADV minimum 24.2.8+, recommended 25.0.0+) and NVIDIA (570+)", as plan §26.1 quotes Epic's Linux Development Requirements page |
| Upstream | The Vulkan specification and `vulkan/vulkan_core.h` (as for #9 above); Mesa (https://gitlab.freedesktop.org/mesa/mesa) `src/amd/vulkan/radv_physical_device.c`; plan §26.1, source [L] |
| Version | Vulkan 1.4 specification and the SDK header already used; Mesa main at 4fac8318e4a5 (2026-11-16) |
| License | Vulkan specification CC-BY-4.0 and header Apache-2.0 OR MIT (above); Mesa MIT. Consulted, not copied |
| Read on | 2026-11-16, for issue #285 |
| Consulted by | The #285 implementer |
| Used in | `servers/rendering/rendering_device_driver.{h,cpp}` (`vulkan_linux_driver_minimum_check`), `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_get_device_properties`, Linux only), `tests/servers/rendering/test_nextgi.cpp` |
| Copied files | None |

How it was used: on Linux, the Vulkan driver passes the driver ID and `driverInfo` it already reads for its workarounds to a pure helper. NVIDIA's proprietary driver reports its version as `driverInfo` ("580.178.04" on the cloud L4 Linux VM); the helper compares its major number with 570. RADV's string starts with "Mesa "; the helper compares major, minor and patch with 24.2.8. Below a minimum it logs one warning that names the driver, the version it read and the minimum, and the tier and its reason do not change (lead decision 2026-11-16: a warning, not a forced fallback). A version that does not parse, and every other driver, gives no warning. No Windows or macOS check: UE names no driver numbers there.
