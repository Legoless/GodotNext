# Async compute source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used for the Vulkan and D3D12 async compute queue (#174):
what each is, where it came from, its license, and how it was used. The Metal part is
in `rhi-metal.md`.

## Async compute queue (#174), Vulkan and D3D12

The Vulkan async queue is the queue of a compute-only family (no graphics bit). Once
RenderingDevice has created it (`command_queue_set_async_compute`), the driver creates buffers and images with `VK_SHARING_MODE_CONCURRENT` across
every family that has a queue (main, transfer, async compute), instead of emitting queue
family release/acquire barrier pairs (deviation from issue step 2). The render graph
cannot emit pairs for resources it does not track (immutable textures and buffers bound
through uniform sets), nor for a resource the main and the async queue both read inside
the fork-join window.

The D3D12 async queue is a `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue, used only with
enhanced barriers. On both APIs, the render graph runs each barrier whole on one queue:
texture barriers from or to a render-target, depth-stencil, resolve or shading-rate layout
run on the main queue at the end of the fork command buffer, and the rest run on the async
queue with graphics stages and accesses removed. The fork semaphore orders the main-queue
work before both.

### Vulkan specification, sharing modes

| Field | Value |
|---|---|
| What | `VkSharingMode`: `VK_SHARING_MODE_CONCURRENT` allows access to a resource from multiple queue families, which must be the families listed at creation (`pQueueFamilyIndices`). An `VK_SHARING_MODE_EXCLUSIVE` resource that another family takes without an ownership transfer has undefined contents unless `maintenance9` is enabled. Concurrent access may be slower. |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VkSharingMode.html |
| Version | Vulkan reference pages as published on 2026-11-14 |
| License | Khronos specification (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-14, for issue #174 |
| Consulted by | The #174 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`command_queue_set_async_compute`, `_update_resource_sharing_queue_families`, `buffer_create`, `texture_create`) |
| Copied files | None |

How it was used: it confirms that concurrent sharing gives both queue families defined contents without ownership transfers, and that the transfer family must be in the list because the transfer workers write the same resources.

### Direct3D 12 Enhanced Barriers specification

| Field | Value |
|---|---|
| What | Queue-type compatibility of barrier layouts, sync and access bits. A compute command list may use the `COMMON`, `GENERIC_READ`, `UNORDERED_ACCESS`, `SHADER_RESOURCE`, `COPY_SOURCE` and `COPY_DEST` layouts (and their compute-queue variants), and cannot transition into or out of `RENDER_TARGET` or the depth-stencil layouts. `DRAW`, `INDEX_INPUT`, `VERTEX_SHADING`, `PIXEL_SHADING`, `DEPTH_STENCIL`, `RENDER_TARGET` and `RESOLVE` sync, and vertex, index, render-target, depth-stencil, shading-rate and resolve access, are not allowed on compute queues. |
| Upstream | https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html |
| Version | DirectX-Specs as published on 2026-11-14 |
| License | Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied. |
| Read on | 2026-11-14, for issue #174 |
| Consulted by | The #174 implementer |
| Used in | `servers/rendering/rendering_device_graph_async.cpp` (`async_queue_supports_texture_layout`, `async_queue_stages`, `async_queue_access`), `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`SUPPORTS_ASYNC_COMPUTE`) |
| Copied files | None |

How it was used: it sets which texture barriers of async lists move to the main queue before the fork and which stages and accesses the async queue's barriers keep.

## Cross-queue validation control (#193)

### Khronos validation layer, synchronization validation settings

| Field | Value |
|---|---|
| What | The `VK_LAYER_KHRONOS_validation` settings for synchronization validation as the layer's manifest (`VkLayer_khronos_validation.json`) describes them: `validate_sync` (default off) turns synchronization validation on, and `syncval_full_validation` (default on; `syncval_submit_time_validation` is its deprecated name) validates all accesses and reports errors during queue submission, where hazards between command buffers and between queues show. In this layer version a hazard message reads `<command>(): <KIND> hazard detected`, without the `SYNC-HAZARD-` message ID text of older layers. |
| Upstream | The layer manifest shipped with the LunarG Vulkan SDK 1.4.363.1 (Khronos Vulkan-ValidationLayers) |
| Version | Layer 1.4.363.1, compared with the Ubuntu 24.04 system layer 1.3.275 |
| License | Apache-2.0 (Vulkan-ValidationLayers). The manifest's setting descriptions and the layer's messages were read; no layer source was read or copied. |
| Read on | 2026-11-18, for issue #193 |
| Consulted by | The #193 implementer |
| Used in | `godotnext/testbed/tests/async_compute/cross_queue_hazard_probe.gd` (what the probe expects, and how a run counts hazards) |
| Copied files | None |

How it was used: it names the settings a 0-hazard async compute run needs, and the message text to count. That the probe's aliased write and clear give submit-time `WRITE_RACING_WRITE` errors naming two queues, and that layer 1.3.275 also reports `RACING` hazards for the render graph's fork and join where layer 1.4.363.1 reports none, are GodotNext's own observations from runs.
