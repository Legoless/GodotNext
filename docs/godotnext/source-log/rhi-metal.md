# Metal RHI source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for the Metal ray-query driver: what it is, where
it came from, its license, and how it was used.

## Approved inputs

### SPIRV-Cross commit df0828ca, MSL AABB-opaque fix

| Field | Value |
|---|---|
| What | One-line MSL fix: `OpRayQueryGetIntersectionCandidateAABBOpaqueKHR` must invert `is_candidate_non_opaque_bounding_box()` |
| Upstream | https://github.com/KhronosGroup/SPIRV-Cross/commit/df0828ca34d6e94ce23ccf211abe6967591dfdda |
| Version | df0828ca34d6e94ce23ccf211abe6967591dfdda (2026-09-01), applied onto vendored snapshot fb0c1a307cca4b4a9d891837bf4c44d17fe2d324 |
| License | Apache-2.0 |
| Read on | 2026-11-03, for issue #48 |
| Consulted by | The #48 implementer |
| Used in | `thirdparty/spirv-cross/spirv_msl.cpp`, recorded as `thirdparty/spirv-cross/patches/0001-msl-aabb-opaque.patch` |
| Copied files | The `spirv_msl.cpp` hunk only. That commit also updates two reference `.comp` shaders; those files are not part of the vendored snapshot and were not copied. |

How it was used: the public commit was fetched and the `spirv_msl.cpp` change was applied verbatim. The vendored file's line number differs from upstream (10396 here, 10542 in that commit) because this tree is snapshot fb0c1a30. `0002-msl-argument-buffer-padding-as.patch` is local and is not an outside source. It treats `AccelerationStructure` as a buffer in `add_msl_resource_binding` and in the argument-buffer padding switch, which this snapshot does not do.

### Apple Metal acceleration structure documentation

| Field | Value |
|---|---|
| What | Public Metal acceleration-structure API: primitive and indirect-instance descriptors, instance descriptor layout, usage bits, the 64-byte instance-buffer offset rule, heap placement of acceleration structures, and the acceleration-structure command encoder |
| Upstream | https://developer.apple.com/documentation/metal/mtlaccelerationstructure and the macOS SDK header `Metal.framework/Headers/MTLAccelerationStructure.h` |
| Version | MacOSX27.2.sdk on the Apple M5 Max, consulted 2026-11-03. WWDC21 session 10149, https://developer.apple.com/videos/play/wwdc2021/10149/, describes `ExtendedLimits` as raising primitive and instance limits and requiring the MSL `extended_limits` tag |
| License | Apple Developer Documentation and SDK headers. Consulted, not copied. No sample code was copied. metal-cpp is already vendored under Apache-2.0 |
| Read on | 2026-11-03, for issue #47 |
| Consulted by | The #47 implementer |
| Used in | `drivers/metal/metal_acceleration_structure.cpp`, `drivers/metal/metal_allocator.cpp`, `drivers/metal/metal3_objects.cpp` |
| Copied files | None |

How it was used: the descriptor fields, usage bits, column-major `PackedFloat4x3` instance transform, 72-byte indirect instance descriptor, and the rules that `instanceDescriptorBufferOffset` is a multiple of 64 and `instanceCountBufferOffset` is a multiple of 4 come from that header. `ExtendedLimits` is never set, because SPIRV-Cross emits `intersection_query<instancing, triangle_data>` without `extended_limits`. The header and the WWDC session do not publish a numeric non-extended instance cap. The driver reports and enforces 2^24, which is above the plan's 100,000 floor, and the testbed checks that a 100,000-instance top-level structure can be created. Metal's default triangle winding is clockwise; Godot's instance flags follow Vulkan's counter-clockwise default, so the counter-clockwise instance option is set when `FLIP_FACING` is clear.

### Apple Metal acceleration-structure refit

| Field | Value |
|---|---|
| What | Public Metal refit API: `refitAccelerationStructure`, `MTLAccelerationStructureRefitOptionVertexData`, in-place refit (source and destination may be the same object), and `refitScratchBufferSize` |
| Upstream | https://developer.apple.com/documentation/metal/mtlaccelerationstructurecommandencoder/refit(source:descriptor:destination:scratchbuffer:scratchbufferoffset:) and the options overload https://developer.apple.com/documentation/metal/mtlaccelerationstructurecommandencoder/refit(source:descriptor:destination:scratchbuffer:scratchbufferoffset:options:). SDK headers `MTLAccelerationStructureCommandEncoder.h`, `MTLAccelerationStructure.h`, and `MTLDevice.h` |
| Version | MacOSX27.2.sdk on the Apple M5 Max, consulted 2026-11-03. The same selectors are already wrapped by vendored metal-cpp (`thirdparty/metal-cpp/Metal/MTLAccelerationStructureCommandEncoder.hpp`, `MTLAccelerationStructure.hpp`, `MTLDevice.hpp`) |
| License | Apple Developer Documentation and SDK headers. Consulted, not copied. No sample code was copied. metal-cpp is already vendored under Apache-2.0 |
| Read on | 2026-11-03, for issue #45 |
| Consulted by | The #45 implementer |
| Used in | `drivers/metal/metal_acceleration_structure.cpp`, `drivers/metal/metal3_objects.cpp` |
| Copied files | None |

How it was used: a bottom-level UPDATE calls the options overload with `AccelerationStructureRefitOptionVertexData`. The SDK header states that without that option the vertex buffers on the geometry descriptor are ignored and previously encoded vertex data is copied. A top-level UPDATE calls the no-options overload so the instance descriptor is read. Source and destination are the same acceleration structure, which the header allows for an in-place refit. The scratch buffer for UPDATE is sized from `refitScratchBufferSize` when that query is non-zero, and otherwise from the build scratch size. `AccelerationStructureUsageRefit` was already set when `ALLOW_UPDATE` is set (#47). This issue does not change the front-face mapping.

### Apple Metal acceleration structure compaction

| Field | Value |
|---|---|
| What | `writeCompactedAccelerationStructureSize` and `copyAndCompactAccelerationStructure` on `MTLAccelerationStructureCommandEncoder`. The compacted size is written as `MTLDataTypeULong`. |
| Upstream | https://developer.apple.com/documentation/metal/mtlaccelerationstructurecommandencoder and the macOS SDK header `Metal.framework/Headers/MTLAccelerationStructureCommandEncoder.h` |
| Version | MacOSX27.2.sdk on the Apple M5 Max, consulted 2026-11-03 |
| License | Apple Developer Documentation and SDK headers. Consulted, not copied. No sample code was copied. metal-cpp already wraps the selectors (Apache-2.0, vendored). |
| Read on | 2026-11-03, for issue #38 |
| Consulted by | The #38 implementer |
| Used in | `drivers/metal/metal_acceleration_structure.cpp`, `drivers/metal/metal3_objects.cpp` |
| Copied files | None |

How it was used: the size query writes one uint64 per structure into a shared buffer, then a later command buffer copies into an acceleration structure allocated at that size. Compaction does not use a Metal usage bit. `ALLOW_COMPACTION` stays a RenderingDevice flag. `useResource` with write usage is used on the size buffer because `useHeaps` is read-only for buffer writes.

## Async compute queue (#174), Metal part 1

The second command queue reuses `newCommandQueue`. The cross-queue fork and join reuse the driver's existing semaphores, which are `MTL::Event` objects (`device->newEvent()`), with `encodeSignalEvent` and `encodeWait`. They are not `MTL::SharedEvent`: only the frame fence is a shared event, because the CPU waits on it. Issue step 5 asked for `MTL::SharedEvent`. A plain event is enough because both queues belong to the same device and the CPU never waits on these semaphores. Vulkan compute-only families, D3D12 compute queues, and queue-family release/acquire barriers are not in this change.

### Apple Metal event documentation

| Field | Value |
|---|---|
| What | `MTLEvent`: a nonshareable event that synchronizes work within a single Metal device; signal and wait commands may go to any command buffers of that device. `MTLSharedEvent` is the variant for CPUs, other devices and other processes. |
| Upstream | https://developer.apple.com/documentation/metal/mtlevent and https://developer.apple.com/documentation/metal/mtlsharedevent |
| Version | Apple Developer Documentation as published on 2026-11-09 |
| License | Apple Developer Documentation. Consulted, not copied. No sample code was copied. metal-cpp already wraps the selectors (Apache-2.0, vendored). |
| Read on | 2026-11-09, for issue #174 |
| Consulted by | The #174 fixer |
| Used in | `drivers/metal/rendering_device_driver_metal3.cpp` (unchanged semaphore type; confirms the choice) |
| Copied files | None |

How it was used: it confirms that the existing `MTL::Event` semaphores can order the main and the async compute queue, since both are on one device, so the semaphores stay `MTL::Event` instead of switching to `MTL::SharedEvent`.

## Level fences across command buffers (#186)

The driver orders the render graph's levels with two alternating `MTL::Fence` objects per command buffer, and its resources are untracked. A command buffer's first level waited on nothing, so the GPU could start the next frame's first commands (or the commands after the graph's mid-frame split for the swap chain pass) while the previous command buffer's last commands still ran. A command buffer that continues another one on the same queue now waits on that one's last level fence until a level of its own has updated a fence.

### Apple Metal fence documentation

| Field | Value |
|---|---|
| What | `MTLFence`: producing passes update a fence and consuming passes wait for it; when they are in different command buffers, commit the command buffers with the producing passes first. `updateFence:` captures the work of the encoder that updates the fence, and fences are evaluated at submission. |
| Upstream | https://developer.apple.com/documentation/metal/mtlfence and the macOS SDK header `Metal.framework/Headers/MTLComputeCommandEncoder.h` (`updateFence:`, `waitForFence:`) |
| Version | Apple Developer Documentation as published on 2026-11-11; MacOSX27.2.sdk |
| License | Apple Developer Documentation and SDK headers. Consulted, not copied. No sample code was copied. metal-cpp already wraps the selectors (Apache-2.0, vendored). |
| Read on | 2026-11-11, for issue #186 |
| Consulted by | The #186 fixer |
| Used in | `drivers/metal/metal_objects_shared.cpp` (`MDCommandBufferBase::continue_from()`), `drivers/metal/rendering_device_driver_metal.cpp` |
| Copied files | None |

How it was used: it confirms that a fence updated in one command buffer orders a later command buffer on the same queue when the updating one is committed first, which the frame's and the graph's command buffers are. Neither source states what a wait observes when several encoders update one fence; that question is not relied on here.

## 64-bit atomics (#175), Metal part

The Metal driver reports `SUPPORTS_BUFFER_INT64_ATOMICS` and `SUPPORTS_IMAGE_INT64_ATOMICS`, never `SUPPORTS_INT64_ATOMIC_ARITH`, and creates `R64_UINT` textures as `RG32Uint` with shader atomic usage. Its GPU condition reuses the driver's existing `supports_image_atomic_64_bit` (Apple9, or Apple8 in a Mac), which is not an outside source. SPIRV-Cross threw on every 64-bit atomic. The local patch `0003-msl-int64-atomic-min-max.patch` translates 64-bit `OpAtomicUMax` and `OpAtomicUMin` on device memory and on 2D, non-arrayed images with MSL 3.1, and keeps throwing for everything else. Like 0002, it is not taken from upstream SPIRV-Cross. The Vulkan and D3D12 parts are in `rhi-atomics64.md`.

### Apple Metal Shading Language specification, 64-bit atomics

| Field | Value |
|---|---|
| What | `atomic_ulong` (MSL 2.4 and later, section 2.6); the 64-bit atomic modify functions `atomic_max_explicit` and `atomic_min_explicit`, which take a `device` pointer to `atomic_ulong`, allow only `memory_order_relaxed` and return `void` (section 6.16.4.6); texture `atomic_max` and `atomic_min` for the `ulong` color type in MSL 3.1 and later, on an `MTLPixelFormatRG32Uint` texture, returning `void` and taking a `ulong4` operand (section 6.13); implicit scalar-to-vector conversion, which replicates the scalar (section 2.24) |
| Upstream | https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf |
| Version | Metal Shading Language Specification version 4.1, dated 2026-06-04 |
| License | Apple developer documentation, Copyright © 2026 Apple Inc., all rights reserved. Consulted, not copied. No sample code was copied |
| Read on | 2026-11-15, for issue #175 |
| Consulted by | The #175 implementer |
| Used in | `thirdparty/spirv-cross/spirv_msl.cpp` and `spirv_msl.hpp` (recorded as `thirdparty/spirv-cross/patches/0003-msl-int64-atomic-min-max.patch`), `drivers/metal/pixel_formats.cpp`, `drivers/metal/rendering_device_driver_metal.cpp`, `drivers/metal/rendering_shader_container_metal.cpp` |
| Copied files | None |

How it was used: buffer max and min become `atomic_max_explicit` or `atomic_min_explicit` on `(device atomic_ulong*)` with relaxed order, and image max and min become `.atomic_max` or `.atomic_min` on `texture2d<ulong, access::read_write>`, passing a scalar `ulong` that converts to `ulong4`. Because these functions return nothing, a shader that reads the result fails. Because the buffer functions take only `device` pointers, threadgroup memory has no 64-bit atomics, so RenderingDevice files 64-bit atomics on shared memory under the arithmetic flag, which Metal does not report. MSL 3.1 also has 64-bit texture atomics on 1D, arrayed and 3D textures, but the patch translates 2D, non-arrayed images only, so the other kinds also go under the arithmetic flag. Export baking defers shaders with `Int64Atomics` or `Int64ImageEXT` to runtime unless the target profile has Apple8 and MSL 3.1. No Metal compiler has seen this MSL yet; the M5 Max run checks it.

## Apple8 (M2) ray query and macOS 14 floor (#171)

Apple8 (M2) exposes Metal ray tracing (`supportsRaytracing()`) via software-traversed BVH without dedicated hardware ray-tracing units, whereas Apple9+ adds hardware ray-tracing units and indirect mesh draws. Indirect-instance acceleration-structure descriptors (`MTLIndirectInstanceAccelerationStructureDescriptor`) and MSL 3.1 `ulong` texture atomics require macOS 14, so the driver reports `RAY_QUERY_REASON_METAL_OS_TOO_OLD` (`METAL_OS_TOO_OLD`) on macOS below 14 instead of collapsing it into `NO_RAY_QUERY`.

### Apple Metal Feature Set Tables and macOS 14 SDK documentation

| Field | Value |
|---|---|
| What | Apple GPU family feature matrix: Apple8 vs Apple9 ray-tracing capabilities (`supportsRaytracing` vs hardware ray tracing), 64-bit integer texture/buffer atomic min/max support on Apple8 (Mac2) and Apple9+ in MSL 3.1 (macOS 14+), indirect mesh shader draws on Apple9+ only, and macOS 14 availability of `MTLIndirectInstanceAccelerationStructureDescriptor` |
| Upstream | https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf and https://developer.apple.com/documentation/metal/mtlindirectinstanceaccelerationstructuredescriptor |
| Version | Metal Feature Set Tables (Apple GPU families 7–10) and Metal documentation consulted 2026-11-24 |
| License | Apple Developer Documentation. Consulted, not copied. No sample code was copied. metal-cpp is vendored under Apache-2.0 |
| Read on | 2026-11-24, for issue #171 |
| Consulted by | The #171 implementer |
| Used in | `servers/rendering/rendering_device_commons.h`, `servers/rendering/rendering_device_driver.{h,cpp}`, `drivers/metal/metal_device_properties.{h,cpp}`, `drivers/metal/rendering_device_driver_metal.cpp`, `servers/rendering/renderer_rd/environment/next_gi.{h,cpp}` |
| Copied files | None |

How it was used: `RenderingDeviceDriver::metal_ray_query_caps()` reports `RAY_QUERY_REASON_METAL_OS_TOO_OLD` on macOS < 14, and on macOS >= 14 sets `ray_query = true, hardware = false` on Apple8 (reporting `NOT_HW_ACCELERATED` via NextGI) and `hardware = true` on Apple9+. `RenderingDeviceDriver::metal_int64_atomic_caps()` and `RenderingDeviceDriver::metal_mesh_shader_caps()` record Apple8's limits in `MetalDeviceProperties` (`supports_buffer_atomic_64_bit` and `supports_image_atomic_64_bit` max/min only with `supports_int64_atomic_arith = false`, and `supports_mesh_shader_indirect = false`).

## Experimental Metal ray shaders (#172)

Metal has no dedicated ray-tracing pipeline object like Vulkan's `VkPipeline` (ray tracing) or DXR's state object; instead, ray generation runs as a compute kernel that calls `metal::raytracing::intersector<triangle_data, instancing>` with an `MTL::IntersectionFunctionTable` (for triangle any-hit functions) and invokes `[[visible]]` closest-hit and miss functions through an `MTL::VisibleFunctionTable` linked into the compute pipeline via `MTL::LinkedFunctions`. Feature support (`SUPPORTS_RAYTRACING_PIPELINE`, `SUPPORTS_RAYTRACING_PIPELINE_LIBRARY`, `SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT`) is gated off by default behind `rendering/raytracing/metal_ray_shaders` and requires Apple9+ and macOS 26.4+. SPIRV-Cross had no MSL code generation for `RayGenerationKHR`, `ClosestHitKHR`, `MissKHR`, `AnyHitKHR`, or `OpTraceRayKHR`; the local patch `0004-msl-ray-tracing-pipelines.patch` emits MSL `<metal_raytracing>` kernels, `[[visible]]` functions, and `[[intersection(triangle, triangle_data, instancing)]]` functions.

### Apple Metal visible & intersection function tables and MSL ray-tracing specification

| Field | Value |
|---|---|
| What | Public Metal ray-tracing compute pipeline APIs (`MTLComputePipelineDescriptor::linkedFunctions`, `maxCallStackDepth`, `MTLVisibleFunctionTable`, `MTLIntersectionFunctionTable`, `MTLFunctionHandle`, `setVisibleFunctionTable:atBufferIndex:`, `setIntersectionFunctionTable:atBufferIndex:`) and MSL `<metal_raytracing>` (`intersector<triangle_data, instancing>`, `visible_function_table`, `intersection_function_table`, `[[visible]]`, `[[intersection(triangle, triangle_data, instancing)]]`) |
| Upstream | https://developer.apple.com/documentation/metal/mtlvisiblefunctiontable, https://developer.apple.com/documentation/metal/mtlintersectionfunctiontable, https://developer.apple.com/documentation/metal/mtllinkedfunctions, and https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf |
| Version | Apple Developer Documentation and Metal Shading Language Specification consulted 2026-11-24; vendored `thirdparty/metal-cpp/Metal/{MTLComputePipeline,MTLLinkedFunctions,MTLVisibleFunctionTable,MTLIntersectionFunctionTable,MTLComputeCommandEncoder}.hpp` (Apache-2.0) |
| License | Apple Developer Documentation. Consulted, not copied. No sample code was copied. metal-cpp is vendored under Apache-2.0 |
| Read on | 2026-11-24, for issue #172 |
| Consulted by | The #172 implementer |
| Used in | `servers/rendering/rendering_device_driver.{h,cpp}`, `drivers/metal/metal_device_profile.h`, `drivers/metal/metal_objects_shared.{h,cpp}`, `drivers/metal/metal3_objects.{h,cpp}`, `drivers/metal/rendering_shader_container_metal.{h,cpp}`, `drivers/metal/rendering_device_driver_metal.{h,cpp}`, `thirdparty/spirv-cross/{spirv_glsl.hpp,spirv_glsl.cpp,spirv_msl.hpp,spirv_msl.cpp}` (recorded as `thirdparty/spirv-cross/patches/0004-msl-ray-tracing-pipelines.patch`) |
| Copied files | None |

How it was used: `RayGenerationKHR` compiles to a compute `kernel` over the launch grid (`[[thread_position_in_grid]]` bounds-checked against `spvRTSBTParams`), `ClosestHitKHR` and `MissKHR` compile to `[[visible]]` functions taking a canonical signature with payload passed by `thread` pointer/reference, hit metadata, hit-record pointer, and `spvRTSBTParams`, and `AnyHitKHR` compiles to an `[[intersection(triangle, triangle_data, instancing)]]` function returning `bool` (`false` on `OpIgnoreIntersectionKHR`, `true` on `OpTerminateRayKHR` or normal return). `RenderingDeviceDriverMetal` builds one compute pipeline state per raygen entry with all miss/hit functions in `MTL::LinkedFunctions`, populates `MTL::VisibleFunctionTable` and `MTL::IntersectionFunctionTable`, and dispatches direct and indirect ray traces on a compute encoder. macOS compilation and Apple M5 Max GPU verification await the lead's Mac.
