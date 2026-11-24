# Mesh shaders source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the
outside inputs used for task and mesh shader stages through RenderingDevice (#178) on Vulkan,
Metal and Direct3D 12: what each is, where it came from, its license, and how it was used.

## Task and mesh shader stages (#178)

RenderingDevice gains the `SHADER_STAGE_TASK` and `SHADER_STAGE_MESH` stages, render pipelines
whose pre-raster stages are a task (optional) and a mesh shader, `draw_list_draw_mesh_tasks()`,
`draw_list_draw_mesh_tasks_indirect()` and `draw_list_draw_mesh_tasks_indirect_count()`, and the
features `SUPPORTS_MESH_SHADER` and `SUPPORTS_MESH_SHADER_INDIRECT`. Shader containers go to
version 6 with the task and mesh workgroup sizes. The Vulkan driver uses `VK_EXT_mesh_shader`; the
Metal driver uses object and mesh functions; the Direct3D 12 driver builds amplification and mesh
pipelines and records `DispatchMesh`, with the feature off until the shader compiler translates the
stages (#122).

### Vulkan specification, VK_EXT_mesh_shader

| Field | Value |
|---|---|
| What | `VK_EXT_mesh_shader`: `VkPhysicalDeviceMeshShaderFeaturesEXT` (`taskShader`, `meshShader`), `vkCmdDrawMeshTasksEXT`, `vkCmdDrawMeshTasksIndirectEXT`, `vkCmdDrawMeshTasksIndirectCountEXT` (which needs `drawIndirectCount`), `VkDrawMeshTasksIndirectCommandEXT`, `VkPhysicalDeviceMeshShaderPropertiesEXT` (`maxTaskWorkGroupCount`, `maxTaskWorkGroupTotalCount`, `maxMeshWorkGroupCount`, `maxMeshWorkGroupTotalCount`), the task and mesh shader and pipeline stage bits, and the graphics pipeline rule that a pipeline with a mesh stage ignores its vertex input and input assembly states. The extension needs SPIR-V 1.4 (Vulkan 1.2 or `VK_KHR_spirv_1_4`) |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html |
| Version | The vendored `thirdparty/vulkan` headers (read on 2026-11-16); the specification as published |
| License | Headers: Apache-2.0 (Khronos). Specification: CC-BY-4.0. Consulted, not copied |
| Read on | 2026-11-16, for issue #178: the vendored files; the upstream page as prior knowledge, not re-read |
| Consulted by | The #178 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_initialize_device_extensions`, `_check_device_capabilities`, `_initialize_device`, `render_pipeline_create`, `command_render_draw_mesh_tasks*`, `has_feature`), `servers/rendering/rendering_device_driver.h` (`PIPELINE_STAGE_TASK_SHADER_BIT`, `PIPELINE_STAGE_MESH_SHADER_BIT`) |
| Copied files | None |

How it was used: the extension is requested as optional, and its feature structure is chained on
its own (never through `VkPhysicalDeviceVulkan12Features`, plan §21.1 C2) with only `taskShader` and
`meshShader` enabled; the extension's other features need features the device does not enable.
The draw commands are loaded with `vkGetDeviceProcAddr`. The pipeline stage bit values were checked
against the vendored header with `static_assert`s. The group-count properties feed the
`LIMIT_MAX_TASK_WORKGROUP_*` and `LIMIT_MAX_MESH_WORKGROUP_*` limits, which `draw_list_draw_mesh_tasks()`
checks the way `compute_list_dispatch()` checks the compute counts. The device enables no
`multiviewMeshShader`, so RenderingDevice refuses a mesh pipeline for a multiview framebuffer format.

### GLSL extension GL_EXT_mesh_shader and glslang

| Field | Value |
|---|---|
| What | `GL_EXT_mesh_shader` (`EmitMeshTasksEXT`, `SetMeshOutputsEXT`, `taskPayloadSharedEXT`, `perprimitiveEXT`, `gl_MeshVerticesEXT`, `gl_PrimitiveTriangleIndicesEXT`), and glslang's `EShLangTask` and `EShLangMesh` stages, which refuse SPIR-V targets below 1.4 for this extension |
| Upstream | https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GLSL_EXT_mesh_shader.txt; the vendored `thirdparty/glslang` |
| Version | The vendored glslang (read on 2026-11-16) |
| License | GLSL extension text: Khronos; glslang: BSD-3-Clause and others (see `thirdparty/README.md`). Consulted, not copied |
| Read on | 2026-11-16, for issue #178: the vendored files; the upstream page as prior knowledge, not re-read |
| Consulted by | The #178 implementer |
| Used in | `modules/glslang/register_types.cpp`, the container formats' `get_shader_spirv_version()`, `tests/servers/rendering/test_rendering_shader_container.cpp`, `godotnext/testbed/tests/rd_parity/meshlets.gd` |
| Copied files | None |

How it was used: the two stages map to glslang's stages and compile for SPIR-V 1.4. A fragment
shader that reads per-primitive values enables the extension too, so a stage whose `#extension`
directive enables it (`require`, `enable` or `warn`) is compiled for SPIR-V 1.4 even when its
container format asks for 1.3; a comment or `disable` that names it does not count.

### Metal, object and mesh functions

| Field | Value |
|---|---|
| What | `MTLMeshRenderPipelineDescriptor` (object, mesh and fragment functions, `payloadMemoryLength`, buffer mutability), `drawMeshThreadgroups` (direct and indirect, with the object and mesh threadgroup sizes), `setObjectBuffer`, `setMeshBuffer`, `setObjectBytes`, `setMeshBytes`, `MTLRenderStageObject`, `MTLRenderStageMesh`, `MTLDispatchThreadgroupsIndirectArguments`, through the vendored metal-cpp headers. The Apple7 floor for mesh shaders and the Apple9 floor for indirect mesh draws are plan §21.1 rules |
| Upstream | https://developer.apple.com/documentation/metal/mtlmeshrenderpipelinedescriptor; the vendored `thirdparty/metal-cpp` |
| Version | The vendored metal-cpp (read on 2026-11-16) |
| License | metal-cpp: Apache-2.0. Apple documentation: consulted, not copied |
| Read on | 2026-11-16, for issue #178: the vendored files; the upstream page as prior knowledge, not re-read |
| Consulted by | The #178 implementer |
| Used in | `drivers/metal/rendering_device_driver_metal.cpp`, `drivers/metal/metal3_objects.cpp`, `drivers/metal/rendering_shader_container_metal.cpp`, `servers/rendering/rendering_device_driver.cpp` (`metal_mesh_shader_caps`) |
| Copied files | None |

How it was used: a mesh pipeline copies the attachment, blend and raster settings of the render
pipeline descriptor into a mesh pipeline descriptor. The draws pass the threadgroup sizes the shader
container stores. Indirect count draws reuse #177's clamp pass with three-word records.

### Metal Feature Set Tables

| Field | Value |
|---|---|
| What | The mesh shading rows: mesh shading from Apple7, indirect mesh draw arguments from Apple9; "Maximum threadgroups per object shader grid": no limit (Apple7 and later); "Maximum threadgroups per mesh shader grid": 1024 on Apple7 and Apple8, 1,048,575 on Apple9, 4,194,303 on Apple10 and Apple11; "Maximum payload in mesh shader pipeline": 16,384 bytes |
| Upstream | https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf |
| Version | The edition dated October 23, 2026 |
| License | Apple documentation. Consulted, not copied |
| Read on | 2026-11-16, for issue #178 (the review fixes) |
| Consulted by | The #178 implementer |
| Used in | `servers/rendering/rendering_device_driver.cpp` (`metal_mesh_shader_limits`), `drivers/metal/rendering_device_driver_metal.cpp` (`limit_get`) |
| Copied files | None |

How it was used: the Metal driver reports the mesh grid row as the mesh workgroup limits (each
dimension and the total), and `UINT32_MAX` for the task workgroup limits, which the tables do not
limit; a draw's group counts are 32-bit.

### SPIRV-Cross, MSL object and mesh functions

| Field | Value |
|---|---|
| What | SPIRV-Cross's MSL output for `ExecutionModelTaskEXT` (`[[object]]`, `mesh_grid_properties`, the `object_data` payload, declared as a plain struct) and `ExecutionModelMeshEXT` (`[[mesh]]`, with `flip_vert_y` applied to the mesh vertices' positions), and the `SPIRType` reflection the payload size is computed from |
| Upstream | https://github.com/KhronosGroup/SPIRV-Cross; the vendored `thirdparty/spirv-cross` |
| Version | The vendored SPIRV-Cross (read on 2026-11-16) |
| License | Apache-2.0. Consulted, not copied |
| Read on | 2026-11-16, for issue #178: the vendored files; the upstream page as prior knowledge, not re-read |
| Consulted by | The #178 implementer |
| Used in | `drivers/metal/rendering_shader_container_metal.cpp` |
| Copied files | None |

How it was used: the payload struct carries no `Offset` decorations (glslang lays out only explicit
blocks), so `Compiler::get_declared_struct_size()` cannot size it. The container walks the payload
type in MSL's natural layout instead (a 3-component vector sized and aligned like a 4-component one,
a struct aligned to its largest member) and names any type it cannot size.

### DirectX specifications, mesh shaders

| Field | Value |
|---|---|
| What | `D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier`, the pipeline state stream with amplification and mesh shader subobjects (`CD3DX12_PIPELINE_STATE_STREAM2`), `ID3D12GraphicsCommandList6::DispatchMesh`, `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH` with `D3D12_DISPATCH_MESH_ARGUMENTS`, and the amplification and mesh shader visibilities, through the vendored `thirdparty/directx_headers` |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html; the vendored `thirdparty/directx_headers` |
| Version | The vendored headers (read on 2026-11-16) |
| License | DirectX-Headers: MIT. Specification: consulted, not copied |
| Read on | 2026-11-16, for issue #178: the vendored files; the upstream specification re-read for the review fixes (the `DispatchMesh` group-count limits) |
| Consulted by | The #178 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp`, `drivers/d3d12/rendering_shader_container_d3d12.cpp`, `servers/rendering/rendering_device_driver.cpp` (`d3d12_mesh_shader_limits`) |
| Copied files | None |

How it was used: the driver plumbing is in place, and once the build links the GodotNext Mesa NIR
library (`25.3.1-gn4` or later, #122), `SUPPORTS_MESH_SHADER` and `SUPPORTS_MESH_SHADER_INDIRECT` turn
on when `OPTIONS7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1`; tier-0 devices and builds with an older
library report `false` and print one line with the device's mesh shader tier. The group-count limits
come from the specification's `DispatchMesh` rules (each count below 64k, their product at most 2^22,
from the command list and from an amplification shader alike, re-read on 2026-11-16) and are reported
only once the feature is on; until then every mesh limit is 0.

## DXIL mesh and amplification shaders (#122)

### Microsoft DirectX-Specs, Mesh Shader functional specification, and public DXIL specification

| Field | Value |
|---|---|
| What | Shader Model 6.5 amplification (`as_6_5`, shader kind 14) and mesh (`ms_6_5`, shader kind 13) DXIL contracts: `dx.op.setMeshOutputCounts` (opcode 168), `dx.op.emitIndices` (169), `dx.op.getMeshPayload` (170, `readonly`), `dx.op.storeVertexOutput` (171), `dx.op.storePrimitiveOutput` (172) and `dx.op.dispatchMesh` (173, taking a `groupshared` address-space 3 payload struct pointer and dominating all exits of the amplification entry point); entry-point shader property tags `MSState` (tag 9: numthreads, max vertex count, max primitive count, output topology 1 = line / 2 = triangle, payload size in bytes) and `ASState` (tag 10: numthreads, payload size in bytes); `PSV0` `PSVRuntimeInfo` mesh/amplification fields (`MS` / `MS1` / `AS` and `PSV2` thread counts); `PSG1` primitive output signature part and `SV_CullPrimitive` (`CULLPRIM`, `i1` signature component stored as 0/1 `i32` with mask `N/A`); DXIL type rules forbidding vector and multidimensional fields in payload structs; shader feature flags (`array_layer_from_vs_or_ds` and `uavs_at_every_stage` apply to VS/HS/DS/GS, not MS/AS); and black-box `dxc.exe` disassembly output (Windows SDK 10.0.26100.0, DXC 1.9.2602.17, no DXC compiler source read) on small original `as_6_5`, `ms_6_5` and `ps_6_5` test shaders |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html (`d3d/MeshShader.md`) and https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst |
| Version | DirectX-Specs and `DXIL.rst` as published on 2026-11-16; Windows SDK 10.0.26100.0 `dxc.exe` output |
| License | DirectX-Specs: CC-BY 4.0; `DXIL.rst`: LLVM/NCSA/MIT documentation. Consulted, not copied |
| Read on | 2026-11-16, for issue #122 |
| Consulted by | The #122 implementer |
| Used in | The private GodotNext Mesa NIR library `25.3.1-gn4` (`docs/godotnext/clean-room.md`), `drivers/d3d12/SCsub`, `drivers/d3d12/rendering_shader_container_d3d12.{h,cpp}`, `drivers/d3d12/rendering_device_driver_d3d12.cpp` and `tests/servers/rendering/test_rendering_shader_container.cpp` |
| Copied files | None |

How it was used: the sixth Mesa patch in `25.3.1-gn4` translates SPIR-V `SPV_EXT_mesh_shader` task and
mesh stages to `as_6_5` and `ms_6_5` DXIL, lowers task payloads to a single-dimensional word-array
struct in LLVM address space 3, normalizes task mesh launches to one dominating `dispatchMesh`, links
task-to-mesh payload sizes and mesh-to-fragment per-vertex and per-primitive varyings, and flips mesh
`SV_Position.y` like vertex shaders. On the engine side, `RenderingShaderContainerD3D12` links
fragment against mesh and mesh against task, raises mesh pipelines to Shader Model 6.5, binds compute
runtime data for `gl_NumWorkGroups` in direct `DispatchMesh` draws, shares the vertex and compute
specialization-constant bit-offset slots with mesh and task, and reports `is_mesh_shading_supported()` so
`SUPPORTS_MESH_SHADER`, `SUPPORTS_MESH_SHADER_INDIRECT` and `ShaderBakerExportPlugin` turn on with
`25.3.1-gn4`. HLSL has no system value for the group counts, so two limits follow: a mesh shader behind a
task shader cannot read `gl_NumWorkGroups` (its counts are the task shader's, known only on the GPU; the
container refuses it with one error), and indirect mesh draws do not set it (the class reference says so).
Two more refusals follow from the same sources, each with one error: `gl_DrawID` in a task or mesh stage
(SPIR-V `BuiltIn DrawIndex`, 4426: Mesa reads it from the vertex layout of the runtime data, these stages
get the compute layout, and `D3D12_DISPATCH_MESH_ARGUMENTS` records carry no draw index), and a mesh
stage with `layout(points) out` (SPIR-V `ExecutionMode OutputPoints`, 27: the specification's
`outputtopology` is `"line"` or `"triangle"` only). SPIR-V enumerant values are from the public SPIR-V
specification (https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html). The GPU check of the runtime
data and the specialization constants is `godotnext/testbed/tests/rd_parity/meshlets.gd`'s grid cases:
`gl_NumWorkGroups` in a mesh shader without a task stage and in a task shader (direct draws), constants
read by the task, mesh and fragment stages, and `gl_CullPrimitiveEXT`, each against a CPU image.

## Task/mesh workgroup invocation, output and payload limits (#154)

Issue #154 completes the remaining mesh shader limits left by #178 on `RenderingDeviceCommons::Limit`
(`LIMIT_MAX_TASK_WORKGROUP_INVOCATIONS`, `LIMIT_MAX_TASK_WORKGROUP_SIZE_{X,Y,Z}`,
`LIMIT_MAX_MESH_WORKGROUP_INVOCATIONS`, `LIMIT_MAX_MESH_WORKGROUP_SIZE_{X,Y,Z}`,
`LIMIT_MAX_MESH_OUTPUT_VERTICES`, `LIMIT_MAX_MESH_OUTPUT_PRIMITIVES`, `LIMIT_MAX_TASK_PAYLOAD_SIZE`)
and validates every task and mesh shader's SPIR-V against those limits at shader creation time with one
named error.

| Field | Value |
|---|---|
| What | Vulkan `VK_EXT_mesh_shader` (`VkPhysicalDeviceMeshShaderPropertiesEXT`: `maxTaskWorkGroupInvocations`, `maxTaskWorkGroupSize[3]`, `maxMeshWorkGroupInvocations`, `maxMeshWorkGroupSize[3]`, `maxMeshOutputVertices`, `maxMeshOutputPrimitives`, `maxTaskPayloadSize`); DirectX Mesh Shader specification (128 max threads per AS/MS threadgroup, 256 max vertices, 256 max primitives, 16,384 B max payload); Metal Feature Set Tables / MSL 3.x (1024 max threads per object/mesh threadgroup, 256 max vertices, 512 max primitives, 16,384 B max payload); public SPIR-V 1.4 specification opcodes (`OpExecutionMode` `LocalSize` / `OutputVertices` / `OutputPrimitivesEXT`, `OpVariable` `StorageClass TaskPayloadWorkgroupEXT`, `OpTypeArray` / `OpTypeStruct` / `OpTypeVector` / `OpTypeMatrix`) |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMeshShaderPropertiesEXT.html; https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html; https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf; https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html; vendored `thirdparty/vulkan` headers |
| Version | Vendored `thirdparty/vulkan` headers; public specifications as cited above |
| License | Vulkan headers: Apache-2.0; Vulkan/DirectX/SPIR-V specs: CC-BY-4.0 / Khronos; Apple documentation: consulted, not copied |
| Read on | 2026-11-23, for issue #154 |
| Consulted by | The #154 implementer |
| Used in | `servers/rendering/rendering_device_commons.h`, `servers/rendering/rendering_device_driver.{h,cpp}`, `servers/rendering/rendering_device_mesh_shader.{h,cpp}`, `servers/rendering/rendering_device.{h,cpp}`, `drivers/vulkan/rendering_device_driver_vulkan.cpp`, `doc/classes/RenderingDevice.xml`, `tests/servers/rendering/test_rd_mesh_shaders.cpp` |
| Copied files | None |

How it was used: Vulkan copies the 11 fields directly from `VkPhysicalDeviceMeshShaderPropertiesEXT`;
Direct3D 12 and Metal populate the specification constants above when `SUPPORTS_MESH_SHADER` is true and
report 0 otherwise. `RDMeshShader::inspect_spirv` parses the SPIR-V word stream directly using the public
SPIR-V binary encoding to extract task/mesh local sizes, output vertex/primitive caps, and task payload
byte size before rejecting any shader that exceeds the device's limits with one named error.
