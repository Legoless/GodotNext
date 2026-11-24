# NextGI hit lighting source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the outside
inputs used for hit lighting for reflections (#129, plan §21.2 contract L7, GL-1 to GL-3) and hit lighting
for GI (#128, plan §21.2 contract L7, GL-4): the Ray Lighting Mode project setting and per-`Environment`
override, the advanced Epic-only `hit_lighting_gi` toggle, reflection and diffuse screen-probe hit marking,
16-bit material-key counting sort with disjoint reflection (`[0x0000..0x7FFF]`) and diffuse GI
(`[0x8000..0xFFFF]`) key ranges, the 64^3 world-space light grid, sorted-deferred re-trace into #130
material hit groups with optional Shader Execution Reordering (SER), inline ray-query area shadows via
#145 `trace_shadow()` (4 shadowed lights for reflections, 1 shadowed light for diffuse GI), multi-bounce
mirror reflections, and translucent TLAS admission with tinted skipping.

## No Unreal Engine input

The mark shader, screen-probe GI hit-marking variant, 16-bit counting sort, 64^3 world light grid builder,
raygen shader, BRDF shading, translucent TLAS mask bit reservation, and verification tests were written
from the plan (§21.2 L7), issues #129 and #128, and public specifications/talks listed below. No Unreal
Engine source, shader, plugin, NvRTX sample, or decompile was opened.

## Sorted-deferred hit lighting architecture (public talk)

| Field | Value |
|---|---|
| What | Daniel Wright, Krzysztof Narkowicz, Patrick Kelly, "Lumen: Real-time Global Illumination in Unreal Engine 5", SIGGRAPH 2022 Advances in Real-Time Rendering course (public presentation slides): high-level two-stage ray lighting concept — trace primary rays to record hits and material IDs, compact and sort active hits by material, and evaluate full material shading and direct lights with shadows on the sorted hits, with an optional fallback to the Surface Radiance Cache. |
| Upstream | https://advances.realtimerendering.com/s2022/ |
| Version | SIGGRAPH 2022 published course notes |
| License | Public SIGGRAPH course slides; high-level algorithm concept only. Consulted, not copied |
| Read on | 2026-11-24, for issues #129 and #128 (known published algorithm) |
| Consulted by | The #129 and #128 implementers |
| Used in | `servers/rendering/renderer_rd/environment/next_gi_hit_lighting.{h,cpp}`, `next_gi_gather.{h,cpp}`, `servers/rendering/renderer_rd/shaders/environment/nextgi_reflect.glsl`, `nextgi_gather.glsl`, `nextgi_gather_body_inc.glsl`, `nextgi_hit_sort.glsl`, `nextgi_light_grid.glsl`, `nextgi_hit_lighting.glsl` |
| Copied files | None |

## GLSL Shader Execution Reordering extensions

| Field | Value |
|---|---|
| What | `GL_EXT_shader_invocation_reorder` (`hitObjectEXT`, `hitObjectTraceRayEXT`, `reorderThreadEXT`, `hitObjectExecuteShaderEXT`) and `GL_NV_shader_invocation_reorder` (`hitObjectNV`, `hitObjectTraceRayNV`, `reorderThreadNV`, `hitObjectExecuteShaderNV`) for reordering raygen shader invocations by hit object and a 16-bit coherence hint (material key). |
| Upstream | https://github.com/KhronosGroup/GLSL/tree/main/extensions/ext and https://github.com/KhronosGroup/GLSL/tree/main/extensions/nv |
| Version | Khronos GLSL extension specifications supported by the vendored glslang and #179 RT pipelines |
| License | Khronos specifications. Consulted, not copied |
| Read on | 2026-11-24, for issue #129 |
| Consulted by | The #129 implementer |
| Used in | `servers/rendering/renderer_rd/shaders/environment/nextgi_hit_lighting.glsl`, `servers/rendering/renderer_rd/environment/next_gi_hit_materials.cpp` |
| Copied files | None |

## Existing repository components reused

- Material hit groups, bindless tables, and pipeline library linking (#130, `next_gi_hit_materials.{h,cpp}`, `nextgi_hit_layout_inc.glsl`).
- Shared RT shadow visibility policy and inline `trace_shadow()` (#145, `next_gi_shadow_policy.{h,cpp}`, `nextgi_shadow_ray_inc.glsl`).
- D14 scenario light set filtering, physical light energy conversion, and omni/spot attenuation (`next_gi_relight.cpp`, `next_gi_src_relight.h`).
- SRC indirect texture and WRC probe sampling (`next_gi_src.h`, `next_gi_relight.h`, `next_gi_wrc.h`, `nextgi_wrc_sample_inc.glsl`).
- Forward+ GGX PBR BRDF formulation (Trowbridge-Reitz NDF, height-correlated Smith-GGX visibility, Schlick Fresnel, Lambertian diffuse) and reflection fog (`nextgi_gather_body_inc.glsl`).
