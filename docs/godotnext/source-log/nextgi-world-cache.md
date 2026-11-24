# NextGI World Cache source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the outside
inputs used for the NextGI World Cache (#294, plan §21.2.1 Lumen World Cache row, §21.0.3 world-space
light grid, §26.1, §26.2): the startup-read `rendering/global_illumination/nextgi/world_cache` project
setting (off by default), availability gating for Vulkan and D3D12 HWRT in `Full` or `Simplified
Material` mode with one-time-per-session fallback logging for SWRT, Inline HWRT, and Metal/macOS,
Surface Cache (SRC) bypass and non-allocation when active, compact linear surface-colour packing for
the Simplified Material hit shader (#286 follow-up), the lazily allocated sparse 8-way set-associative
world-space radiance hash grid (5 camera-distance cascades), the runtime path-trace update raygen stage
and temporal resolve/eviction compute pass, cache lookup variants in diffuse gather, reflections, WRC,
translucency GI, and hit lighting, and the `VIEWPORT_DEBUG_DRAW_NEXTGI_WORLD_CACHE` debug view.

## No Unreal Engine input

All C++ host code, GLSL compute/ray-tracing shaders, policy gates, simplified material colour packing,
sparse hash-table design, doctests, and Vulkan testbed checks were written from scratch from the plan
(§21.2.1, §21.0.3, §26.1, §26.2) and issue #294. No Unreal Engine source, shader, plugin, NvRTX sample,
or decompile was opened.

## Spatial hash-grid world-space radiance caching (published graphics literature)

| Field | Value |
|---|---|
| What | General spatial-hashing world-space radiance caching concept (discretizing hit positions and dominant normal faces into camera-distance LOD cells indexed in an open-addressed / set-associative GPU hash table with temporal EMA accumulation and frame-age eviction), as widely described in real-time rendering literature (e.g. Binder et al., "Massively Parallel Path Space Filtering", HPG 2019; Gautron, "Real-Time Ray-Traced Ambient Occlusion of Complex Scenes using Spatial Hashing", SIGGRAPH 2020; Boisse, "World-Space Spatiotemporal Reservoir Reuse for Ray-Traced Global Illumination", SIGGRAPH Asia 2021). |
| Upstream | Public SIGGRAPH / HPG papers and standard real-time graphics literature |
| Version | Published algorithms (2019–2021) |
| License | High-level published algorithm concepts only. Consulted from domain knowledge, not copied |
| Read on | 2026-11-24, for issue #294 |
| Consulted by | The #294 implementer |
| Used in | `servers/rendering/renderer_rd/environment/next_gi_world_cache.{h,cpp}`, `servers/rendering/renderer_rd/shaders/environment/nextgi_world_cache_common_inc.glsl`, `nextgi_world_cache_trace.glsl`, `nextgi_world_cache_resolve.glsl` |
| Copied files | None |

## Existing repository components reused

- Material hit groups, pipeline libraries, bindless tables, and `NEXTGI_HIT_SIMPLIFIED_CONSUMERS` (#130, #286, `next_gi_hit_materials.{h,cpp}`, `nextgi_hit_layout_inc.glsl`, `nextgi_hit_base.glsl`).
- 64^3 world-space light grid builder, direct light collector, and hit-lighting indirect fallback (#129, `next_gi_hit_lighting.{h,cpp}`, `nextgi_light_grid.glsl`, `nextgi_hit_lighting.glsl`).
- Shared RT shadow visibility policy and inline `trace_shadow()` (#145, `next_gi_shadow_policy.{h,cpp}`, `nextgi_shadow_ray_inc.glsl`).
- Diffuse screen-probe gather, specular reflections, front-layer reflections, WRC, and translucency GI probe traces (`next_gi_gather.{h,cpp}`, `next_gi_wrc.{h,cpp}`, `nextgi_gather.glsl`, `nextgi_gather_body_inc.glsl`, `nextgi_wrc_update.glsl`, `nextgi_wrc_update_inc.glsl`).
- Viewport debug visualization pipeline (`rendering_server_enums.h`, `viewport.{h,cpp}`, `render_forward_clustered.cpp`, `node_3d_editor_viewport.{h,cpp}`).
