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
