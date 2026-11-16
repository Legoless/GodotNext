# Bindless tables source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the
outside inputs used for bindless tables through RenderingDevice (#176, plan §21.1 C4) on Vulkan,
Direct3D 12 and Metal: what each is, where it came from, its license, and how it was used.

## Bindless tables (#176)

RenderingDevice gains `bindless_table_create`, `bindless_table_allocate`, `bindless_table_release`,
`bindless_table_get_memory_usage`, the uniform type `UNIFORM_TYPE_BINDLESS_TABLE`, the feature
`SUPPORTS_BINDLESS` and the limits `LIMIT_MAX_BINDLESS_TEXTURES`, `LIMIT_MAX_BINDLESS_SAMPLERS` and
`LIMIT_MAX_BINDLESS_STORAGE_BUFFERS`. A table is a runtime array of read-only textures, samplers or
storage buffers, bound as the only binding of a uniform set. A released slot is reused only after
`frames.size()` frames, and freeing a resource that a slot holds is deferred until its last slot is
released. The render graph keeps a dirty list per table, so binding a table adds barriers only for the
resident resources a command left unready. A runtime array in SPIR-V reflects as a bindless table.

### Vulkan specification, descriptor indexing

| Field | Value |
|---|---|
| What | `VkPhysicalDeviceDescriptorIndexingFeatures` (`runtimeDescriptorArray`; `shaderSampledImageArrayNonUniformIndexing`, which covers samplers too; `shaderStorageBufferArrayNonUniformIndexing`; `descriptorBindingPartiallyBound`; `descriptorBindingUpdateUnusedWhilePending`; `descriptorBindingSampledImageUpdateAfterBind`, which covers sampler descriptors too; `descriptorBindingStorageBufferUpdateAfterBind`; `descriptorBindingVariableDescriptorCount`), a structure that extends `VkDeviceCreateInfo`. `VkDescriptorBindingFlagBits`: with update after bind a submission uses the most recently written descriptors; with update unused while pending, descriptors that pending work does not use may be written (dynamically used ones, with partially bound); with partially bound, descriptors that are not dynamically used need not be valid; a variable descriptor count is allowed on the last binding only, and `descriptorCount` is then an upper bound. `VkPhysicalDeviceDescriptorIndexingProperties`: the `maxPerStageDescriptorUpdateAfterBind*` and `maxDescriptorSetUpdateAfterBind*` limits count descriptors of sets with or without the update-after-bind flag; `maxUpdateAfterBindDescriptorsInAllPools` caps the descriptors of all update-after-bind pools; `maxPerStageUpdateAfterBindResources` caps all resources of a stage. `VkPhysicalDeviceDescriptorBufferPropertiesEXT`: `samplerDescriptorSize`, `sampledImageDescriptorSize` and `storageBufferDescriptorSize`, the size in bytes of one descriptor of each type. `VUID-VkDeviceCreateInfo-pNext-02830`: a chain with `VkPhysicalDeviceVulkan12Features` must not also hold `VkPhysicalDeviceDescriptorIndexingFeatures`, among others. |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingFeatures.html, https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorBindingFlagBits.html, https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingProperties.html, https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorBufferPropertiesEXT.html and https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html |
| Version | Vulkan reference pages as published on 2026-11-15. The driver compiles against the vendored `thirdparty/vulkan` headers (`VK_HEADER_VERSION` 335) |
| License | The reference pages show no license notice of their own and are generated from the specification sources (CC-BY-4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-15 and 2026-11-16, for issue #176 |
| Consulted by | The #176 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_initialize_device_extensions`, `_check_device_capabilities`, `_initialize_device`, `shader_create_from_container`, `uniform_set_create`, the bindless table functions, `limit_get`, `has_feature`) |
| Copied files | None |

How it was used: `VK_EXT_descriptor_indexing` is requested as optional. The eight features are read
from the Vulkan 1.2 structure on 1.2 devices (the extension's structure otherwise) and, when all eight
are present, enabled through `VkPhysicalDeviceDescriptorIndexingFeatures` of its own, never
`VkPhysicalDeviceVulkan12Features` (plan v3.9.4, C2; the VUID above). A table is one descriptor set
from a pool of its own, with one partially bound, update-after-bind, variable-count runtime array at
binding 0, visible to every stage. Shaders lay out a bindless set the same way, so the table's set
binds where the shader expects it. The capacity limits are half of the update-after-bind limits, so a
shader can bind two full tables of a type beside its other sets (those count against the same limits).
The memory a table reports is its capacity times the descriptor size `VK_EXT_descriptor_buffer` gives
(the extension is only found, not enabled), or 64 bytes per descriptor when the device does not expose
it. A released slot keeps its descriptor: the frame ring guarantees no submitted work reads it when it
is written again.

### Direct3D 12 descriptor tables and resource binding

| Field | Value |
|---|---|
| What | `D3D12_DESCRIPTOR_RANGE_FLAGS`: `DESCRIPTORS_VOLATILE` makes descriptors volatile, and combined with `DATA_VOLATILE` both descriptors and data are volatile, as in root signature 1.0. Resource binding in HLSL: an unbounded array in a shader matches a fixed `numDescriptors` in the descriptor table; declared register ranges of one type must not overlap, unbounded ones included, while ranges in different register spaces never overlap. Hardware tiers: tier 1 allows 128 SRVs in all descriptor tables per stage, tier 2 the full heap; a shader-visible sampler heap holds at most 2,048 samplers; on tier 2, CBV and UAV entries covered by descriptor tables must be populated when the shader runs, SRVs and samplers need not be. |
| Upstream | https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_descriptor_range_flags, https://learn.microsoft.com/en-us/windows/win32/direct3d12/resource-binding-in-hlsl and https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-support |
| Version | Microsoft Learn as published on 2026-11-15. The driver compiles against the vendored `thirdparty/directx_headers` |
| License | Microsoft Learn documentation (CC-BY 4.0). Consulted, not copied |
| Read on | 2026-11-15 and 2026-11-16, for issue #176 |
| Consulted by | The #176 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_get_device_limits`, `_initialize_frames`, `uniform_set_create`, the bindless table functions, `limit_get`, `has_feature`) and `drivers/d3d12/rendering_shader_container_d3d12.cpp` (the root signature) |
| Copied files | None |

How it was used: Direct3D 12 reports `SUPPORTS_BINDLESS` with resource binding tier 2 (a
65,536-descriptor SRV range) and enhanced barriers (the graph places the tables' barriers; the legacy
resource-state path would have to track every slot), on every vendor's GPU (see the Mesa section). The
shader-visible heaps then grow by a reservation for tables: two resource tables of 65,536 descriptors and
one sampler table of 1,024. It is made only when both fit: the sampler heap under its 2,048 cap, and the
CBV/SRV/UAV heap under the maximum the runtime reports in `D3D12_FEATURE_DATA_D3D12_OPTIONS19`'s
`MaxViewDescriptorHeapSize`, or else `D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2` (1,000,000),
both from the vendored headers. The reservation is one allocation per heap, made when the heaps are
created, and each table takes one of its fixed ranges, so uniform sets cannot use it and tables cannot
outgrow it. A table's set binds a descriptor table with one range of the table's size, flagged
`DESCRIPTORS_VOLATILE` (and `DATA_VOLATILE` for SRVs), at register 0 in the register space of the
table's set, so it overlaps no other range. Each table starts its range with null descriptors.

### Mesa: NIR to DXIL for runtime descriptor arrays

| Field | Value |
|---|---|
| What | How Mesa's NIR to DXIL declares resource arrays. `nir_to_dxil.c` declares a texture or sampler variable only when `glsl_type_get_texture_count` or `glsl_type_get_sampler_count` is above 0, and those count `glsl_get_length` elements (`glsl_types.c`, `glsl_type_count`), which is 0 for a runtime array. A read-only storage-buffer array is declared with `glsl_get_length` elements, so a runtime array gets a range size of 0 in the resource metadata. `emit_deref` and `emit_load_vulkan_descriptor` find the resource through `emit_createhandle_call_dynamic`, which ends in `UNREACHABLE("Resource access for undeclared range")` when no declared range covers the register; `util/macros.h` makes that `__assume(0)` or `__builtin_unreachable()` in a release build. Both pass `false` as `createHandle`'s non-uniform index flag. |
| Upstream | Mesa, https://gitlab.freedesktop.org/mesa/mesa: `src/microsoft/compiler/nir_to_dxil.c`, `src/microsoft/spirv_to_dxil/dxil_spirv_nir.c`, `src/compiler/glsl_types.c`, `src/compiler/nir/nir.h` and `src/util/macros.h`; godot-nir-static's `godot-patches/01_godot_nir_goodies.patch` (https://github.com/godotengine/godot-nir-static) |
| Version | Mesa 25.3.1 and godot-nir-static release `25.3.1-3`, the release `misc/scripts/install_d3d12_sdk_windows.py` installs |
| License | MIT. Each Mesa file read carries the MIT permission notice; godot-nir-static is MIT. Consulted, not copied |
| Read on | 2026-11-16, for issue #176 |
| Consulted by | The #176 implementer |
| Used in | `drivers/d3d12/rendering_shader_container_d3d12.cpp` (`_remap_bindless_tables`, `_generate_root_signature`, `_nir_report_resource`), `drivers/d3d12/rendering_shader_container_d3d12.h` (the bindless register constants), `tests/servers/rendering/test_d3d12_shader_container.cpp` (the bindless cases) |
| Copied files | None |

How it was used: it explains why shaders with runtime descriptor arrays failed on Direct3D 12. The
texture table was never declared, so translating its reads was undefined behavior: on the NVIDIA L4 the
process crashed while creating the shader, and an earlier build's `CreateComputePipelineState` failure
with `E_OUTOFMEMORY` is consistent with the same cause. The register number was not the cause: giving
each array a size made the fixture pass with the table still at Mesa's register in space 0, and moving
the table to register 0 of a space of its own without a size still crashed. `_remap_bindless_tables` runs after `dxil_spirv_nir_passes`. It gives
each table's runtime array the size of its root-signature range and moves it to register 0 in its set's
space, changing the variable, its derefs and each `vulkan_resource_index` together. The Windows SDK's
`dxc.exe -dumpbin` (10.0.26100) showed the result, for example `textures texture f32 2d T0 t0,space3
65536`, and that every `createHandle` has `nonUniformIndex` false. Reads at non-uniform indices
therefore rely on the hardware: HLSL requires the mark for an index that varies across a wave, and a
GPU may treat an unmarked index as uniform and give the whole wave one lane's descriptor. Direct3D 12
reports `SUPPORTS_BINDLESS` on every vendor's GPU anyway (the lead's decision of 2026-11-16 removed an
NVIDIA-only gate). The rd_parity/bindless fixture, whose compute shader reads a different slot in every
invocation, matches its CPU reference on the NVIDIA L4 and on WARP (the Microsoft Basic Render Driver).
WARP is a software adapter, and no other vendor's GPU has run it, so these runs do not show that such
GPUs read the right entry. The GodotNext Mesa library (25.3.1-gn2, #123) patches `emit_srv`, `emit_sampler`,
`emit_createhandle_call_dynamic` and `emit_deref`; these notes describe the upstream `25.3.1-3`
library, and the container's table layout was checked with both.

### Metal argument buffers

| Field | Value |
|---|---|
| What | Writing argument buffers without an argument encoder on Metal 3 (tier 2): texture and sampler entries are `MTLResourceID`s (`gpuResourceID`), buffer entries GPU addresses (`gpuAddress`), and the resources an argument buffer names are made resident with `useResource(s)` or `useHeap(s)`. |
| Upstream | No outside page: Apple's documentation pages are rendered by script and could not be read with the tools available on 2026-11-16. The behavior is the one Godot's own Metal driver already relies on, in this repository: `RenderingDeviceDriverMetal::uniform_set_create` (`drivers/metal/rendering_device_driver_metal.cpp`) writes `gpuResourceID` and `gpuAddress` entries into argument buffers, and `MDCommandBuffer` (`drivers/metal/metal3_objects.cpp`) makes their resources resident through its resource tracker |
| Version | This repository at 8b340f4edb |
| License | MIT (this repository) |
| Read on | 2026-11-16, for issue #176 |
| Consulted by | The #176 implementer |
| Used in | `drivers/metal/rendering_device_driver_metal.cpp` (`uniform_set_create`, the bindless table functions, `limit_get`, `has_feature`), `drivers/metal/metal_objects_shared.cpp` (`MDBindlessTable`), `drivers/metal/metal3_objects.cpp` (binding a table's set) |
| Copied files | None |

How it was used: a table is a shared-storage buffer of one 8-byte entry per slot, bound as its set's
argument buffer. Its resident resources join the set's resource usage, so the encoder makes them
resident as it does for any argument buffer. Encoders are built when the render graph replays a frame,
after the frame's releases, so a released slot's resource stays resident until the driver's next
segment begins. There is no `MTL::ResidencySet` path (the Metal 3 driver keeps no residency set of its
own yet). This code has not been compiled yet, so the driver reports `SUPPORTS_BINDLESS` false until
an M5 Max run.

### SPIRV-Cross runtime arrays in argument buffers (vendored, unchanged)

| Field | Value |
|---|---|
| What | How SPIRV-Cross lays out a runtime descriptor array in an argument buffer: the array must be in a device-storage argument buffer (`set_argument_buffer_device_address_space`; otherwise it throws "Runtime sized variables must be in device storage argument buffers."), and the shader reads it through `spvDescriptorArray`, one `spvDescriptor<T>` per element from the member's offset. In an argument buffer, `get_resource_array_size` takes the array size from the resource binding's `count`, not from the shader, so a runtime array needs a count of 0. Buffer entries are plain 8-byte pointers unless `runtime_array_rich_descriptor` is set, which Godot does not set. |
| Upstream | `thirdparty/spirv-cross` (https://github.com/KhronosGroup/SPIRV-Cross), as vendored in this repository |
| Version | The vendored copy (see `thirdparty/README.md`) |
| License | Apache-2.0 (vendored, already recorded in `thirdparty/README.md`) |
| Read on | 2026-11-15, for issue #176 |
| Consulted by | The #176 implementer |
| Used in | `drivers/metal/rendering_shader_container_metal.cpp` |
| Copied files | None; SPIRV-Cross is not modified |

### SPIRV-Reflect runtime arrays (vendored, unchanged)

| Field | Value |
|---|---|
| What | SPIRV-Reflect reports a runtime descriptor array (`OpTypeRuntimeArray`) as a binding with one array dimension of `SPV_REFLECT_ARRAY_DIM_RUNTIME` (0). |
| Upstream | `thirdparty/spirv-reflect` (https://github.com/KhronosGroup/SPIRV-Reflect), as vendored in this repository |
| Version | The vendored copy (see `thirdparty/README.md`) |
| License | Apache-2.0 (vendored, already recorded in `thirdparty/README.md`) |
| Read on | 2026-11-15, for issue #176 |
| Consulted by | The #176 implementer |
| Used in | `servers/rendering/rendering_shader_container.cpp` (`reflect_spirv`) |
| Copied files | None; SPIRV-Reflect is not modified |
