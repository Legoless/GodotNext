# D3D12 RHI source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used for D3D12 ray tracing, starting with DXR 1.1 ray query
(#118): what each input is, where it came from, its license, and how it was used. The
Mesa NIR library that compiles ray query to DXIL (#123) is recorded in
`docs/godotnext/clean-room.md`.

The D3D12 driver implements the same RenderingDevice acceleration-structure API as the
Vulkan and Metal drivers in this repository, which are not outside sources. Godot PR
#118654, which the issue names as the AS and ray-query half to port, was not an input:
it was not read for this work. Its RT-pipeline half was read for the RT pipelines
(#121); see that section.

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

## RT pipelines (#121)

### Godot pull request #118654, "Implement raytracing in D3D12 driver"

| Field | Value |
|---|---|
| What | The RT-pipeline half of the draft: `raytracing_pipeline_create` as a `D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE` with one DXIL library subobject per stage that exports `main` under a new name, one hit group subobject per hit group (triangles, or procedural with an intersection shader), one shader config with the largest payload and attribute sizes, one pipeline config, and the layout-defining shader's root signature as the global root signature; identifiers from `ID3D12StateObjectProperties::GetShaderIdentifier`; `SetPipelineState1` plus `SetComputeRootSignature` to bind; uniform sets bound as compute sets; `DispatchRays` with the three shader tables; shader identifier size and the table and record alignments as API traits. The description names the two open problems: payload and attribute definitions that differ between stages (undefined on D3D12, and GLSL allows stages that declare none), and unused resources in the DXIL libraries, which fail validation. |
| Upstream | https://github.com/godotengine/godot/pull/118654 (draft by Skyth; its public diff and description) |
| Version | The pull request as published on 2026-11-16 (last updated 2026-04-16) |
| License | MIT (Godot Engine contributions). The approach was followed; the code here is written for this fork's driver. |
| Read on | 2026-11-16, for issue #121, which names the pull request as the base of its RT-pipeline half |
| Consulted by | The #121 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_raytracing_state_object_create`, `command_bind_raytracing_pipeline`, `command_bind_raytracing_uniform_set`, `command_trace_rays`, `api_trait_get`) |
| Copied files | None |

How it was used: the state object follows the pull request's layout. What the pull request lacks was
added here: export names unique in the process (so collections combine), shader records through a local
root signature, collections and `AddToStateObject` for libraries, the indirect trace, and both open
problems. For the first, each stage without a payload or attributes gets a stand-in of the type another
stage of its shader declares, and a pipeline whose hit group still mixes payload or attribute sizes fails
with one error. For the second, the D3D12 shader container removes the resource variables a stage does
not use before the DXIL is made (`RenderingShaderContainerD3D12::_remove_unused_resources`), while the
root signature keeps every binding.

### Mesa SPIR-V to DXIL, with the ray-tracing patch

| Field | Value |
|---|---|
| What | How the translation the D3D12 shader container links treats the ray-tracing stages: each stage is its own DXIL library whose entry function exports `main` under a mangled name; any-hit and closest-hit entry functions take a ray payload and hit attributes and miss entry functions a payload, and translation fails when the stage has no such variable; the payload and attribute variables are wrapped in one struct each, and an incoming payload is SPIR-V `IncomingRayPayloadKHR` while outgoing ones are temporaries passed to `traceRayEXT`; a `ShaderRecordBufferKHR` block is not translated; uniform and storage buffers, root constants and acceleration structures are read through resource indices that name the set and binding (register), while textures and samplers are read through their variables; unused uniform-mode variables other than acceleration structures are removed, and unused buffer variables stay declared; a uniform buffer becomes a constant buffer of whole 16-byte rows. |
| Upstream | Mesa `mesa-25.3.1` (https://gitlab.freedesktop.org/mesa/mesa) and the public ray-tracing commit `99b60c908aeff7fd342739d8c403a110e3bba1cb` of https://github.com/blueskythlikesclouds/mesa, as `docs/godotnext/clean-room.md` records them |
| Version | As pinned in `docs/godotnext/clean-room.md` |
| License | MIT (Mesa's). Read, not copied. |
| Read on | 2026-11-16, for issue #121 |
| Consulted by | The #121 implementer |
| Used in | `drivers/d3d12/rendering_shader_container_d3d12.cpp` (`_add_raytracing_stand_ins`, `_remove_unused_resources`, `_remap_shader_record`, `_convert_spirv_to_nir`) and `servers/rendering/rendering_device_rt_pipeline.cpp` (`rewrite_spirv_shader_record_as_uniform_buffer`) |
| Copied files | None |

How it was used: the container hands Mesa a copy of each module whose shader record is a uniform buffer, then
moves that buffer to the local root signature's register; gives a stage without a payload or attributes a
variable of the type another stage declares before Mesa's passes run; and after them removes the resource
variables that no resource index names and no instruction reads. The local root signature covers the
record's constant buffer rounded to 16 bytes.

### DirectX Raytracing (DXR) functional specification, RT pipelines

| Field | Value |
|---|---|
| What | State objects and subobjects; collections and the configuration they need to compile alone; `D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS` on collections and on pipelines that `AddToStateObject` grows, which takes a self-contained addition whose shader exports are new and keeps the parent's shader identifiers valid; default and explicit subobject associations, and that a hit group's component shaders must share their local root signature; local root signatures (`D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE`, registers apart from the global root signature's, arguments right after the 32-byte identifier, root constants packed as DWORDs); the shader record stride (a multiple of 32 bytes, at least the largest record) and the 64-byte table start; `D3D12_EXPORT_DESC` renaming an export (`ExportToRename` may be unmangled); `D3D12_RAYTRACING_SHADER_CONFIG` (payload and attribute sizes, attributes at most 32 bytes) and that every collection's config must match; the payload rule (every shader a trace reaches must declare the trace's payload type, also when it does not use it); triangle hits' `BuiltInTriangleIntersectionAttributes` (two floats); `D3D12_DISPATCH_RAYS_DESC` and the `DISPATCH_RAYS` indirect argument, which holds the whole description. |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html (source: the `d3d/Raytracing.md` file of the DirectX-Specs repository) |
| Version | DirectX-Specs as published on 2026-11-16 |
| License | Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-16, for issue #121 |
| Consulted by | The #121 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_raytracing_state_object_create`, `raytracing_pipeline_link`, `raytracing_pipeline_link_add`, `_get_local_root_signature`, `raytracing_pipeline_get_create_error`, `raytracing_pipeline_get_hit_group_record_size`, `command_trace_rays_indirect`), `drivers/d3d12/rendering_shader_container_d3d12.cpp` (`_add_raytracing_stand_ins`), `servers/rendering/rendering_device_rt_pipeline.cpp` (`get_hit_group_interface_error`, `get_d3d12_stage_error`), `servers/rendering/rendering_device.cpp` (`raytracing_pipeline_create`) |
| Copied files | None |

How it was used: a library is a collection with its own configs and root signatures; a link is a
pipeline of existing collections only, and a link onto a linked pipeline adds just the new collections
with `AddToStateObject`. Every state object gives all its hit groups one local root signature, of the
largest record its hit shaders read, so no component shader gets two. Because the local root arguments
follow each identifier, the hit table's stride holds them also where no hit group has `record_data`
(the driver reports their size, and RenderingDevice makes every record at least that big, with zeros).
By the payload rule, a miss stage that uses no payload gets the one payload type its shader's ray
generation stage traces, when no stage of the shader receives one. The indirect trace writes the
shader-table part of a `D3D12_DISPATCH_RAYS_DESC` per trace and copies the launch size in after it.

### Direct3D 12 tight placed-resource alignment specification

| Field | Value |
|---|---|
| What | With `D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT`, placed buffers may be aligned to as little as 8 bytes, while committed buffers keep a 4 KiB alignment. |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/D3D12TightPlacedResourceAlignment.html |
| Version | DirectX-Specs as published on 2026-11-16 |
| License | Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-16, for issue #121 |
| Consulted by | The #121 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`buffer_create`) |
| Copied files | None |

How it was used: shader binding table buffers are committed resources, so each table starts on the
64-byte boundary DXR needs on devices with tight alignment too.
