# D3D12 RHI source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used for D3D12 ray tracing, starting with DXR 1.1 ray query
(#118): what each input is, where it came from, its license, and how it was used. The
Mesa NIR library that compiles ray query to DXIL (#123) is recorded in
`docs/godotnext/clean-room.md`.

The D3D12 driver implements the same RenderingDevice acceleration-structure API as the
Vulkan and Metal drivers in this repository, which are not outside sources. Godot PR
#118654, which the issue names as the AS and ray-query half to port, was not an input:
it was not read for this work.

## DXR 1.1 ray query (#118)

### DirectX Raytracing (DXR) functional specification

| Field | Value |
|---|---|
| What | Acceleration-structure rules: the destination buffer stays in `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`, needs `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`, lives in a default heap and is never transitioned; scratch is in `UNORDERED_ACCESS`; vertex, index, transform and instance inputs are in `NON_PIXEL_SHADER_RESOURCE`. Alignment: 256 bytes for acceleration structures and scratch, 16 bytes per instance, index data to its format, vertex data to its component size, postbuild output to its largest member (8 bytes for the compacted size). UAV barriers on the buffers that hold acceleration structures order builds, copies and their readers. In-place update: the source may equal the destination, and only vertex positions may change. Prebuild info (`ResultDataMaxSizeInBytes`, `ScratchDataSizeInBytes`, `UpdateScratchDataSizeInBytes`, 0 without `ALLOW_UPDATE`). `EmitRaytracingAccelerationStructurePostbuildInfo` with `COMPACTED_SIZE` writes one `UINT64` per structure into a destination in `UNORDERED_ACCESS`; `CopyRaytracingAccelerationStructure` in `COMPACT` mode needs `ALLOW_COMPACTION` on the source. The acceleration-structure SRV: `D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE` with `RaytracingAccelerationStructure.Location` and a null resource. `D3D12_RAYTRACING_INSTANCE_DESC`: row-major 3x4 transform, 24-bit `InstanceID`, 8-bit `InstanceMask` (0 never included; the ray's mask is ANDed with it), 24-bit hit-group contribution, 8-bit flags, 256-byte-aligned BLAS address. Facing: front faces are clockwise from the ray origin in object space (left-handed); `TRIANGLE_FRONT_COUNTERCLOCKWISE` reverses it; an instance transform with a negative determinant does not change facing. Vertex formats for tier 1.0 and the ones tier 1.1 adds. `RayQuery` from shader model 6.5, used in pixel and compute shaders in the specification's examples. |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html |
| Version | DirectX-Specs as published on 2026-11-15 |
| License | Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-15, for issue #118 |
| Consulted by | The #118 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`blas_create`, `tlas_create`, `acceleration_structure_instance_write`, `command_build_blas`, `command_build_tlas`, the compaction functions, `uniform_set_create`, `buffer_create`), `servers/rendering/rendering_device_driver.cpp` (`d3d12_ray_query_caps`) |
| Copied files | None |

How it was used: the driver creates each acceleration structure in a buffer made for it and leaves it in its state, transitions inputs and scratch on the legacy barrier path, builds with the prebuild sizes, writes instances in the documented layout and names every structure by GPU virtual address. RenderingDevice's build, geometry and instance flags already have the D3D12 values, so the driver passes them through behind `static_assert`s. The facing rule and the instance-mask rule are the ones the Vulkan driver relies on, which rd_smoke fixtures A, D and E check against Vulkan.

### Direct3D 12 Enhanced Barriers specification

| Field | Value |
|---|---|
| What | With enhanced barriers buffers start in layout `UNDEFINED` and have no state, so a resource flag marks acceleration-structure storage; the legacy `RAYTRACING_ACCELERATION_STRUCTURE` state corresponds to the acceleration-structure accesses. The legacy UAV barrier on acceleration structures corresponds to the build, copy and postbuild-info sync scopes with the UAV and acceleration-structure read and write accesses. `RAYTRACING_ACCELERATION_STRUCTURE_READ` and `_WRITE` allow the `ALL`, `COMPUTE_SHADING`, `RAYTRACING`, `ALL_SHADING`, `BUILD_RAYTRACING_ACCELERATION_STRUCTURE` and `COPY_RAYTRACING_ACCELERATION_STRUCTURE` scopes, and reads also `EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO`. `SHADER_RESOURCE` access allows the build scope; `UNORDERED_ACCESS` allows the postbuild-info and raytracing scopes. |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html |
| Version | DirectX-Specs as published on 2026-11-15 |
| License | Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-15, for issue #118 |
| Consulted by | The #118 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`buffer_create`, `_rd_access_to_d3d12_and_mask`, `_rd_stages_to_d3d12`, `_rd_stages_and_access_to_d3d12`, `command_pipeline_barrier`, `command_acceleration_structure_write_compacted_size`) |
| Copied files | None |

How it was used: acceleration-structure buffers get `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE` when the device uses enhanced barriers. RenderingDevice's acceleration-structure build stage maps to the build, copy and postbuild-info scopes, its acceleration-structure accesses to the two acceleration-structure access bits with only the scopes the tables allow, and ray queries in vertex or pixel shaders to `ALL_SHADING`. The graph's acceleration-structure barriers become buffer barriers on the structure's buffer.

### Direct3D 12 Tight Placed Resource Alignment specification

| Field | Value |
|---|---|
| What | With `D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT` (Agility SDK 1.618.1 and later) a placed buffer may be aligned to as little as 8 bytes and at most 256 bytes. From Agility SDK 1.619.4 a non-zero power-of-two `D3D12_RESOURCE_DESC::Alignment` is a floor the runtime honors; the 1.618 runtime rejects it with `E_INVALIDARG`. Without a floor a buffer bound in a way that needs a stricter alignment gives a debug-layer error or, on some drivers, device removal. The listed floors include 16 bytes for acceleration-structure build inputs and 256 bytes for constant-buffer views. |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/D3D12TightPlacedResourceAlignment.html |
| Version | DirectX-Specs as published on 2026-11-15 |
| License | Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-15, for issue #118 |
| Consulted by | The #118 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_acceleration_structure_buffer_create`, `_acceleration_structure_scratch_address`, `acceleration_structure_get_scratch_size_bytes`, `command_build_tlas`, `_initialize_allocator`) |
| Copied files | None |

How it was used: the vendored D3D12MA adds the tight-alignment flag to every buffer when the device supports it, and the engine loads Agility SDK 1.618, which takes no floor. On such a device the driver gives each acceleration-structure buffer and each build's scratch region 256 bytes more and starts the structure or the scratch at the region's first 256-byte boundary, the DXR alignment. Instance descriptions cannot be moved by the driver, so a top-level build whose instances are off a 16-byte boundary is refused with an error.

### DirectX-Headers (`d3d12.h`)

| Field | Value |
|---|---|
| What | The declarations the driver compiles against: `ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo`, `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure`, `EmitRaytracingAccelerationStructurePostbuildInfo` and `CopyRaytracingAccelerationStructure`, `D3D12_FEATURE_D3D12_OPTIONS5` (`RaytracingTier`, with `D3D12_RAYTRACING_TIER_1_1` = 11), and the constants `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT` (256), `D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT` (16), `D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE` and `D3D12_RAYTRACING_MAX_GEOMETRIES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE` (2^24), `D3D12_RAYTRACING_MAX_PRIMITIVES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE` (2^29) |
| Upstream | https://github.com/microsoft/DirectX-Headers, vendored as `thirdparty/directx_headers` (recorded in `thirdparty/README.md`) |
| Version | The vendored commit `25411c74bb9cc7c416b2ff01b3ad8a306811dfdd`. The same constants were cross-checked in the Agility SDK 1.618.5 `d3d12.h` installed on the Windows build VM, without copying from it. |
| License | MIT (`thirdparty/README.md`) |
| Read on | 2026-11-15, for issue #118 |
| Consulted by | The #118 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.{h,cpp}` |
| Copied files | None; the header was already vendored |

How it was used: the driver calls the public entry points and reports the header's limits through `limit_get`.

### DXGI adapter description and driver version

| Field | Value |
|---|---|
| What | `DXGI_ADAPTER_DESC::VendorId` and `DeviceId` (the PCI IDs the RT-core allowlist uses), `DXGI_ADAPTER_FLAG_SOFTWARE` (WARP, already mapped to `DEVICE_TYPE_CPU`), and `IDXGIAdapter::CheckInterfaceSupport(__uuidof(IDXGIDevice), ...)`, which returns the user-mode driver version as a `LARGE_INTEGER` of four 16-bit parts |
| Upstream | https://learn.microsoft.com/windows/win32/api/dxgi/ns-dxgi-dxgi_adapter_desc and https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-checkinterfacesupport |
| Version | Microsoft Learn as published on 2026-11-15 |
| License | Microsoft Learn documentation (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-15, for issue #118 |
| Consulted by | The #118 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_check_capabilities`, `get_driver_version_key`), `servers/rendering/rendering_device_driver.cpp` (`format_d3d12_driver_version_key`) |
| Copied files | None |

How it was used: the hardware flag comes from the adapter's PCI IDs, as on Vulkan, and the HWRT fault latch's driver key includes the user-mode driver version, so a driver update clears the latch (G1-6).

### D3D12 adapter architecture: UMA for the AMD integrated rule (#283)

| Field | Value |
|---|---|
| What | `D3D12_FEATURE_ARCHITECTURE1` and `D3D12_FEATURE_DATA_ARCHITECTURE1::UMA`, which the runtime sets to TRUE when "the hardware and driver support UMA"; the structure, "introduced in Windows 10, version 1703", supersedes `D3D12_FEATURE_DATA_ARCHITECTURE`, and the pages' remarks pair UMA with "integrated/ UMA adapters" and `UMA` false with discrete adapters |
| Upstream | https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture1 and https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture |
| Version | Microsoft Learn as published on 2026-11-16 (both pages updated 2024-07-12) |
| License | Microsoft Learn documentation (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-16, for issue #283 |
| Consulted by | The #283 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_check_capabilities`) |
| Copied files | None; the header was already vendored |

How it was used: the RT-core check needs to know whether an AMD adapter is an integrated GPU (lead decision 2026-11-16: AMD APUs take the software tier, as UE names only discrete AMD RX 6000 series or newer). The context driver's DXGI device type comes from `DedicatedVideoMemory`, which an APU's carve-out can make non-zero, so the D3D12 driver asks `CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1)` for node 0 and passes `UMA` as the integrated flag (issue #283 names `ARCHITECTURE1`; for node 0 its `UMA` is the same field as in `D3D12_FEATURE_DATA_ARCHITECTURE`, and Windows 10 1703 is older than any DXR runtime). The PCI IDs of every current AMD APU are on the RT-core check's APU block as well (source log rhi-vulkan.md, #283).
