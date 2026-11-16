# RT pipelines source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the
outside inputs used for RT pipelines that evaluate materials in hit shaders (#179, plan §21.1 C5) on
RenderingDevice and Vulkan: what each is, where it came from, its license, and how it was used.

## RT pipelines for hit-shader material evaluation (#179)

RenderingDevice gains `RDHitGroup.record_data` (a shader record of at most 32 bytes after each hit
group's handle in the hit shader binding table), the `flags` argument of `raytracing_pipeline_create`
with `RAYTRACING_PIPELINE_FLAG_LIBRARY` and `RAYTRACING_PIPELINE_FLAG_REORDER`,
`raytracing_pipeline_link`, `raytracing_list_trace_rays_indirect`, the features
`SUPPORTS_RAYTRACING_REORDER`, `SUPPORTS_RAYTRACING_PIPELINE_LIBRARY` and
`SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT`, and the limit `LIMIT_RAYTRACING_REORDER_EXTENSIONS`. The Vulkan driver
implements libraries with `VK_KHR_pipeline_library`, the indirect trace with `vkCmdTraceRaysIndirectKHR`
and shader execution reordering with `VK_EXT_ray_tracing_invocation_reorder` or
`VK_NV_ray_tracing_invocation_reorder`. Direct3D 12 (#121) and Metal (#172) keep reporting the features
false and keep the driver defaults that fail.

### Vulkan specification, ray tracing pipelines and libraries

| Field | Value |
|---|---|
| What | `VkRayTracingPipelineCreateInfoKHR`: `VUID-...-flags-03465` (a library needs `pLibraryInterface`), `VUID-...-pLibraryInfo-03590` (so does a pipeline linked from libraries), `VUID-...-pLibraries-03591` (every library has the pipeline's `maxPipelineRayRecursionDepth`), `VUID-...-pLibraryInfo-03592` (every library has a layout compatible with the pipeline's), `VUID-...-pLibraryInfo-03593` (every library has the pipeline's `maxPipelineRayPayloadSize` and `maxPipelineRayHitAttributeSize`), `VUID-...-pLibraryInfo-07999` (`stageCount` may be 0 only when libraries are given), `VUID-...-stage-03425` (a pipeline that is not a library has a ray generation stage, counting the libraries' stages). The groups of the libraries are appended after `pGroups`, in `pLibraries` order and in the order each library defined them. |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VkRayTracingPipelineCreateInfoKHR.html |
| Version | Vulkan reference pages as published on 2026-11-16. The driver compiles against the vendored `thirdparty/vulkan` headers (`VK_HEADER_VERSION` 335) |
| License | The reference pages show no license notice of their own and are generated from the specification sources (CC-BY-4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-16, for issue #179 |
| Consulted by | The #179 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_raytracing_pipeline_create`, `raytracing_pipeline_library_create`, `raytracing_pipeline_link`) and `servers/rendering/rendering_device.cpp` (`raytracing_pipeline_link`) |
| Copied files | None |

How it was used: a library is created with `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR` and a
`VkRayTracingPipelineInterfaceCreateInfoKHR`; a link is a pipeline with no stages or groups of its own,
every library in `VkPipelineLibraryCreateInfoKHR`, and the same interface. Because every library and the
linked pipeline must declare the same payload and hit attribute sizes, they are one value for all of
them: a 64-byte payload (`RDRTPipeline::LIBRARY_MAX_PAYLOAD_SIZE`) and the device's
`maxRayHitAttributeSize`. RenderingDevice checks the recursion depth and the layout (the same uniform set
formats, immutable samplers and push constant size) before the driver is called, so a mismatch is one
named error, not a validation error. Because groups follow `pLibraries` order, the base library (ray generation and miss)
goes first and libraries of hit groups only follow; a pipeline's hit group indices then stay valid in a
pipeline linked from it with more libraries, which is what `hit_sbt_set_pipeline` needs.

### Vulkan specification, library interface and layout compatibility

| Field | Value |
|---|---|
| What | `VkRayTracingPipelineInterfaceCreateInfoKHR`: `maxPipelineRayPayloadSize` is the largest block declared in the `RayPayloadKHR` or `IncomingRayPayloadKHR` storage classes, sized as if every scalar had the alignment of the block's largest scalar; `VUID-...-maxPipelineRayHitAttributeSize-03605`. Pipeline Layout Compatibility ("compatible for set N": identically defined descriptor set layouts for sets 0 through N) and the glossary's Identically Defined Objects (creation arguments that are the same handles or identical values). |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VkRayTracingPipelineInterfaceCreateInfoKHR.html, https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html, https://docs.vulkan.org/spec/latest/appendices/glossary.html |
| Version | Vulkan reference pages and specification as published on 2026-11-16 |
| License | As above. Consulted, not copied |
| Read on | 2026-11-16, for issue #179 (review fixes) |
| Consulted by | The #179 implementer |
| Used in | `servers/rendering/rendering_device_rt_pipeline.cpp` (`get_spirv_payload_size`, `get_link_library_status`), `servers/rendering/rendering_device.cpp` (`shader_create_from_bytecode_with_samplers`, `raytracing_pipeline_create`) and `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`shader_create_from_container`, `shader_get_raytracing_payload_size`) |
| Copied files | None |

How it was used: the Vulkan driver measures each ray tracing stage's payload from its SPIR-V when it
creates the shader, by that rule, and RenderingDevice refuses a library with a stage over the 64 bytes
libraries declare, with one named error, instead of creating a pipeline whose shaders break its declared
interface. An array sized by a specialization constant has no fixed size, so a library refuses it too.
Uniform set formats do not include immutable samplers, but the Vulkan driver puts them into the
descriptor set layouts, and two layouts with different sampler handles are not identically defined. So
RenderingDevice keeps a hash of the immutable samplers each shader's layout gets (the sampler RIDs, which
name the handles) and a link compares it like the set formats.

### Vulkan specification, indirect trace

| Field | Value |
|---|---|
| What | `vkCmdTraceRaysIndirectKHR`: `VUID-...-indirectDeviceAddress-03633` (the address comes from a buffer with `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`), `-03634` (a multiple of 4), `-03636` (the whole `VkTraceRaysIndirectCommandKHR` in the same buffer), `VUID-...-rayTracingPipelineTraceRaysIndirect-03637` (the feature is enabled), and the hit table rules `-03689` (base alignment), `-03690` (stride a multiple of `shaderGroupHandleAlignment`) and `-04035` (stride at most `maxShaderGroupStride`). |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdTraceRaysIndirectKHR.html |
| Version | Vulkan reference pages as published on 2026-11-16 |
| License | As above. Consulted, not copied |
| Read on | 2026-11-16, for issue #179 |
| Consulted by | The #179 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_check_device_capabilities`, `_initialize_device`, `command_trace_rays_indirect`), `servers/rendering/rendering_device_rt_pipeline.cpp` (`get_indirect_status`, `get_hit_group_stride`) and `servers/rendering/rendering_device_graph.cpp` (`add_raytracing_list_trace_rays_indirect`) |
| Copied files | None |

How it was used: the device enables `rayTracingPipelineTraceRaysIndirect` when it reports it, and
RenderingDevice requires the indirect buffer to have the indirect and device-address usages, a 4-byte
aligned offset and 12 bytes in range, each with its own error. The render graph runs the list at the
draw-indirect stage as well, where indirect command data is read, so a compute pass that writes the
launch size gets a barrier to it (a run without that stage hit `VUID-vkCmdPipelineBarrier-dstAccessMask-02816`).
A buffer has one usage per raytracing list, so, as for #177's count draws, the graph refuses an indirect
trace whose launch-size buffer the list already reads from a uniform set, or that a bindless table bound in
the list has not read since its last write; when the table is bound after the trace, the list's end names
the conflict and keeps the buffer unready for the table's next bind.
A record makes the hit stride the handle plus the record, aligned to the handle alignment; at most
32 + 32 bytes on 32-byte handles, far below the 4,096 bytes Vulkan guarantees for `maxShaderGroupStride`.

### Vulkan specification, shader execution reordering

| Field | Value |
|---|---|
| What | `VK_EXT_ray_tracing_invocation_reorder`: requires `VK_KHR_ray_tracing_pipeline`; SPIR-V dependency `SPV_EXT_shader_invocation_reorder`; API support for `GL_EXT_shader_invocation_reorder`; `VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT` and `...PropertiesEXT`; forked from the NV extension. |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_ray_tracing_invocation_reorder.html |
| Version | Vulkan reference pages as published on 2026-11-16 |
| License | As above. Consulted, not copied |
| Read on | 2026-11-16, for issue #179 |
| Consulted by | The #179 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_initialize_device_extensions`, `_check_device_capabilities`, `_initialize_device`, `has_feature`, `limit_get`) and `servers/rendering/rendering_device_rt_pipeline.cpp` (`get_spirv_reorder_usage`) |
| Copied files | None |

How it was used: both the EXT and the NV extension are requested as optional; each feature structure is
chained only when its extension is enabled, because validation layers older than the EXT structure
report it as unknown. `SUPPORTS_RAYTRACING_REORDER` is true with either, and
`LIMIT_RAYTRACING_REORDER_EXTENSIONS` says which GLSL extension shaders may use, because the two declare
different SPIR-V capabilities (`ShaderInvocationReorderEXT` and `ShaderInvocationReorderNV`, from the
vendored `thirdparty/spirv-headers`). RenderingDevice scans the capabilities of every shader it creates,
and of shader caches through the container's reflection, and fails a shader that needs an extension the
device lacks with one named error. Both paths ran on NVIDIA L4 GPUs: the NV extension on Linux (driver
580.178, which exposes only the NV extension) and the EXT extension on Windows (driver 621.14, which
exposes both).
