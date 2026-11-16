# Indirect-count draws source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the
outside inputs used for GPU-count indirect draws through RenderingDevice (#177) on Vulkan,
Direct3D 12 and Metal: what each is, where it came from, its license, and how it was used.

## Indirect-count draws (#177)

RenderingDevice gains `draw_list_draw_indirect_count()`, `SUPPORTS_DRAW_INDIRECT_COUNT` and
`SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE`, and a shader compile option, `base_instance_native`, that
makes `gl_InstanceIndex` include the first instance on Direct3D 12. The Vulkan driver enables
`VK_KHR_draw_indirect_count` and `shaderDrawParameters`; the Direct3D 12 driver keeps one command
signature per argument type and stride; the Metal driver clamps the records with a compute pass
before the render pass.

### Vulkan specification, draw-indirect-count and draw parameters

| Field | Value |
|---|---|
| What | `VK_KHR_draw_indirect_count` (`vkCmdDrawIndirectCountKHR`, `vkCmdDrawIndexedIndirectCountKHR`), promoted to Vulkan 1.2, where the commands are optional when the extension is not supported. The features chapter's extension feature aliases table, which lists `VK_KHR_draw_indirect_count` as an alias of the `drawIndirectCount` feature ("the extension itself acts as an alias for the feature"); `drawIndirectCount` has no feature structure of its own, only a member of `VkPhysicalDeviceVulkan12Features`. `VkPhysicalDeviceShaderDrawParametersFeatures` and the `shaderDrawParameters` member of `VkPhysicalDeviceVulkan11Features`. `drawIndirectFirstInstance` in `VkPhysicalDeviceFeatures`. |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_draw_indirect_count.html and https://docs.vulkan.org/spec/latest/chapters/features.html |
| Version | As published on 2026-11-16. The driver compiles against the vendored `thirdparty/vulkan` headers |
| License | Generated from the specification sources (CC-BY-4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-16, for issue #177 |
| Consulted by | The #177 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_initialize_device_extensions`, `_check_device_capabilities`, `_initialize_device`, `command_render_draw_*indirect_count`, `has_feature`) |
| Copied files | None |

How it was used: the extension is requested as optional and its entry points are loaded with `vkGetDeviceProcAddr`. Because the extension is an alias of `drawIndirectCount`, enabling it enables the feature without `VkPhysicalDeviceVulkan12Features`, which must not share the device chain with the individual structures it already carries (plan §21.1 C2). `shaderDrawParameters` is queried with its Vulkan 1.1 structure and enabled through `VkPhysicalDeviceVulkan11Features` on Vulkan 1.2 and later, or the individual structure on 1.1.

### DirectX specifications, shader model 6.8 extended command information

| Field | Value |
|---|---|
| What | `SV_StartVertexLocation` (the `BaseVertexLocation` of indexed draws, the `StartVertexLocation` of others) and `SV_StartInstanceLocation`, required in shader model 6.8. No draw index system value exists. `D3D12_FEATURE_DATA_D3D12_OPTIONS21::ExtendedCommandInfoSupported` from the vendored `thirdparty/directx_headers` |
| Upstream | https://github.com/microsoft/DirectX-Specs/blob/master/d3d/HLSL_ShaderModel6_8.md |
| Version | As published on 2026-11-16 |
| License | Creative Commons Attribution 4.0 and MIT (per the repository). Consulted, not copied |
| Read on | 2026-11-16, for issue #177 |
| Consulted by | The #177 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_check_capabilities`, `has_feature`), `drivers/d3d12/rendering_shader_container_d3d12.cpp` |
| Copied files | None |

How it was used: `SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE` needs shader model 6.8 and `ExtendedCommandInfoSupported`, and only shaders compiled with `base_instance_native` read the start locations. `gl_DrawID` stays unavailable on Direct3D 12 at every shader model.

### Mesa SPIR-V to DXIL, system value modes

| Field | Value |
|---|---|
| What | `dxil_spirv_runtime_conf::first_vertex_and_base_instance_mode` (`DXIL_SPIRV_SYSVAL_TYPE_ZERO`, `_NATIVE`, `_RUNTIME_DATA`), which system values `dxil_spirv_nir` reads from the runtime data in each mode, how SPIR-V to NIR maps the Vulkan `BaseVertex` built-in (to the first vertex system value, outside OpenGL), and how `nir_to_dxil` emits `dx.op.startVertexLocation` and `dx.op.startInstanceLocation` (shader model 6.8, SFI0 bit 32) |
| Upstream | https://gitlab.freedesktop.org/mesa/mesa, `src/microsoft/spirv_to_dxil/` and `src/microsoft/compiler/nir_to_dxil.c` |
| Version | Mesa 25.3.1, the release the upstream NIR library is built from |
| License | MIT. Consulted, not copied |
| Read on | 2026-11-16, for issue #177 |
| Consulted by | The #177 implementer |
| Used in | `drivers/d3d12/rendering_shader_container_d3d12.cpp`, `tests/servers/rendering/test_d3d12_shader_container.cpp` |
| Copied files | None |

How it was used: `base_instance_native` selects the native mode and raises the shader model to 6.8. `lower_base_vertex` stays off, as before: from Vulkan SPIR-V, `gl_BaseVertex` is already the first vertex, so in native mode it reads `SV_StartVertexLocation` and no runtime data (only `gl_DrawID` does, which the driver never sets for draws). The doctest checks the DXIL program version, the SFI0 bit, and that `gl_BaseVertex` and `gl_BaseInstance` need no runtime data.

### Direct3D 12 and Metal indirect argument layouts

| Field | Value |
|---|---|
| What | `D3D12_DRAW_ARGUMENTS`, `D3D12_DRAW_INDEXED_ARGUMENTS`, `D3D12_COMMAND_SIGNATURE_DESC::ByteStride` and `ExecuteIndirect` with a count buffer, from the vendored `thirdparty/directx_headers`; `MTL::DrawPrimitivesIndirectArguments` and `MTL::DrawIndexedPrimitivesIndirectArguments` and the indirect draw calls, from the vendored `thirdparty/metal-cpp` |
| Upstream | https://github.com/microsoft/DirectX-Headers, https://developer.apple.com/metal/cpp/ |
| Version | The vendored copies in this repository |
| License | MIT (DirectX-Headers); Apache 2.0 (metal-cpp) |
| Read on | 2026-11-16, for issue #177 |
| Consulted by | The #177 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp`, `drivers/metal/metal3_objects.cpp`, `drivers/metal/metal_objects_shared.cpp` |
| Copied files | None |

How it was used: the Direct3D 12 signature cache makes one signature per argument type and stride. The Metal clamp kernel copies 4 or 5 words per record, matching both APIs' layouts, multiplies the instance count by the pass's view count as the driver's direct draws do for multiview, and the render pass then issues one indirect draw per record from the copy.
