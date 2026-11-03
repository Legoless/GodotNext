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
