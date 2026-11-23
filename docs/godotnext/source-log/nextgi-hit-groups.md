# NextGI material hit groups source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the outside
inputs used for the material hit groups (#130, plan §21.2 L7): Godot spatial materials compiled to
closest-hit and any-hit stages, linked through #179's pipeline libraries, with material textures in #176's
bindless tables and vertex attributes from an RT-arena attribute stream.

## No Unreal Engine input

The hit variant, the attribute stream, the eligibility rules, the hit shader binding table ranges and the
check were written from the plan (§21.2 L7, §7.4) and the issue. No Unreal Engine source, shader, plugin,
NvRTX sample or decompile was opened.

## Vulkan specification, ray traversal: opacity

| Field | Value |
|---|---|
| What | Ray Traversal chapter, opacity of a candidate: the geometry's `VK_GEOMETRY_OPAQUE_BIT_KHR`, then the instance flags `VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR` and `..._FORCE_NO_OPAQUE_BIT_KHR`, then the ray flags `OpaqueKHR` and `NoOpaqueKHR`, which take precedence ("overrides all opaque geometries to behave as if they are non-opaque"); a non-opaque candidate of a pipeline trace invokes the any-hit shader, and `OpIgnoreIntersectionKHR` drops it. |
| Upstream | https://docs.vulkan.org/spec/latest/chapters/raytraversal.html |
| Version | Vulkan specification as published on 2026-11-16 |
| License | Generated from the specification sources (CC-BY-4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-16, for issue #130 |
| Consulted by | The #130 implementer |
| Used in | `servers/rendering/renderer_rd/shaders/environment/nextgi_hit_base.glsl` (the check's second trace) |
| Copied files | None |

How it was used: NextGI's TLAS forces every instance opaque and keeps alpha-tested surfaces out unless their
material traces them as opaque (#61); that policy is #145's. The check traces each sample a second time with
`gl_RayFlagsNoOpaqueEXT`, which overrides the instance flag, so the any-hit stage of an alpha-tested
material's group runs on the same TLAS and its decisions can be compared with the raster capture's alpha.

## Ray cones for texture level of detail

| Field | Value |
|---|---|
| What | T. Akenine-Möller, C. Crassin, J. Boksansky, L. Belcour, A. Panteleev, O. Wright, "Improved Shader and Texture Level of Detail Using Ray Cones", Journal of Computer Graphics Techniques 10(1), 2021: the LOD of a texture at a hit is the triangle's texel-to-world density term, 0.5 log2 of (texture-space area / world-space area), plus log2 of the cone's width at the hit over the absolute cosine between the ray and the surface. |
| Upstream | https://jcgt.org/published/0010/01/01/ |
| Version | The published paper (2021) |
| License | JCGT publishes papers for reading; no code from it is used. Consulted, not copied |
| Read on | 2026-11-16, for issue #130 (the formula was known from the paper; the page was not needed again) |
| Consulted by | The #130 implementer |
| Used in | `servers/rendering/renderer_rd/shaders/environment/nextgi_hit_material_inc.glsl` (`hit_lod_base`, `nextgi_hit_texture_lod()`) |
| Copied files | None |

How it was used: the issue asks for `texture()` to become `textureLod` at a ray-cone LOD. The texel-to-world
term comes from the triangle's corners after the material's `vertex()` ran on them, so a `vertex()` that
scales UVs changes the level as it does on the rasterizer; the texture's size is added per call. The caller
passes the cone's width at the origin and its spread in the payload. A UV transform written in `fragment()`
is not seen by the cone, as the paper's method does not see it either; the check measures that case
separately (the material shader that scales UVs in `fragment()`).

## GLSL ray tracing and descriptor indexing extensions

| Field | Value |
|---|---|
| What | `GL_EXT_ray_tracing` (shader stages, `shaderRecordEXT`, `hitAttributeEXT`, `rayPayloadEXT` / `rayPayloadInEXT`, `ignoreIntersectionEXT`, `gl_InstanceCustomIndexEXT`, `gl_GeometryIndexEXT`, `gl_HitKindEXT`), `GL_EXT_nonuniform_qualifier` (`nonuniformEXT` indexing of run-time descriptor arrays) and `GL_EXT_samplerless_texture_functions`. |
| Upstream | https://github.com/KhronosGroup/GLSL/tree/main/extensions/ext |
| Version | The extension texts in the Khronos GLSL repository's main branch, as used by the vendored glslang |
| License | Khronos specifications. Consulted, not copied |
| Read on | 2026-11-16, for issue #130 |
| Consulted by | The #130 implementer |
| Used in | `servers/rendering/renderer_rd/shaders/environment/nextgi_hit_layout_inc.glsl`, `nextgi_hit_material_inc.glsl`, `nextgi_hit_base.glsl` |
| Copied files | None |

How it was used, and two behaviors of the vendored glslang that the shaders rely on, both observed on the
NVIDIA L4 (Vulkan) while bringing the stages up:
- An unsized array of descriptors that no code indexes with a run-time index is sized to one element, so a
  stage that never reads a bindless table would declare a plain texture and its pipeline layout would differ
  from the other libraries'. Every stage therefore holds a never-called function that indexes each table with
  `nonuniformEXT`.
- A combined image-sampler constructor (`sampler2D(texture, sampler)`) may appear only at its point of use,
  not as a function argument, so the level-of-detail function takes the texture and the sampler stays in the
  `textureLod` call.

## Godot's compressed vertex format (this repository)

The attribute stream stores UV and UV2 as two unorm16 each over the surface's UV range, kept in a 32-byte
header, as Godot's compressed vertex format stores UVs over `uv_scale` (`RenderingServer::_surface_set_data()`).
The issue's text asked for half floats; on the check's panels a half float's 11 bits moved tiled UVs (scaled
by up to 8 in `vertex()`) by up to an eighth of a texel, and 2% of the albedo samples missed the 1/255 bound.

## Hardware Ray Tracing Mode (#286): Epic's public documentation

The per-platform setting `rendering/global_illumination/nextgi/hardware_ray_tracing_mode` (plan §21.1.1) follows
Unreal Engine 5.9's Hardware Ray Tracing Mode as Epic's public pages describe it. Only those pages were read; no
Unreal Engine source, shader, plugin, NvRTX sample or decompile was opened. Each page was fetched with curl and its
document text taken from the page's embedded document data.

| Field | Value |
|---|---|
| What | Epic, Ray Tracing Performance Guide, section "Ray Tracing Mode": "On platforms that support both inline ray tracing and ray tracing shaders, you can use the per-platform option for fine-grained control over how ray tracing is enabled. You can find this in the Project Settings under Platforms > [Platform] > Hardware Ray Tracing Mode." Modes: "Disabled: All ray tracing on this platform is disabled."; "Inline: Only inline ray tracing passes are enabled, such as Lumen Hardware Ray Tracing mode with Surface Cache and Barycentrics / Traversal debug view. In this mode, ray tracing material shaders and select global shaders are not compiled. This saves memory and CPU cost by not having to prepare binding for ray tracing shaders."; "Simplified Material: Inline ray tracing passes, plus simplified material hit shaders. Only some features, such as parts of Lumen, use these simplified materials. Other features fall back to inline ray tracing."; "Full: Every feature of ray tracing is supported on this platform." No default, no named platform and no maturity label. |
| Upstream | https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine?application_version=5.9 |
| Version | Revision 5005060, last revised 2026-11-11, tagged 5.8 and 5.9 (the same text as on 2026-11-16) |
| License | Epic's public documentation. Consulted, not copied |
| Read on | 2026-11-18, for issue #286 |
| Consulted by | The #286 implementer |
| Used in | `servers/rendering/renderer_rd/environment/next_gi_hit_materials.h` (`NextGIHitMode`), the plan's §21.1.1 row |
| Copied files | None |

| Field | Value |
|---|---|
| What | The same guide's UE 5.8 text, as the Internet Archive kept it: the setting was called "Ray Tracing Mode" (Platforms > [Platform] > Ray Tracing Mode), with Disabled, Inline and Full only. So Simplified Material and the name Hardware Ray Tracing Mode are new in the 5.9 text. |
| Upstream | https://web.archive.org/web/20260824124602/https://dev.epicgames.com/documentation/unreal-engine/ray-tracing-performance-guide-in-unreal-engine |
| Version | The page as last revised 2026-06-17, archived 2026-08-24 |
| License | Epic's public documentation. Consulted, not copied |
| Read on | 2026-11-18, for issue #286 |
| Consulted by | The #286 implementer |
| Used in | The plan's §21.1.1 row (the "UE 5.9 Preview" label) |
| Copied files | None |

| Field | Value |
|---|---|
| What | Epic, Lumen World Cache, section "Simplified Material Ray Tracing Mode": "You can find Hardware Ray Tracing Mode in Project Settings > Platforms > Windows, in the Renderer section. Linux has the same setting."; Inline "disables ray tracing passes that use material shaders, such as Lumen hit lighting"; Simplified Material: "Inline ray tracing passes, plus a simplified material hit shader. Only some features, such as the Lumen World Cache, can use materials. Other features fall back to inline ray tracing." and "It compiles only a simplified material hit shader, so it costs less in shaders and memory than Full, but it can still give the World Cache the surface color it needs at each hit. Lumen hit lighting isn't available in this mode"; Full: "Full support for ray tracing features, including ray generation, hit and miss shaders, and inline ray queries. This is the default." Requirements: "The platform's Hardware Ray Tracing Mode is Full or Simplified Material." |
| Upstream | https://dev.epicgames.com/documentation/unreal-engine/lumen-world-cache-in-unreal-engine (withdrawn: on 2026-11-18 every URL form returned Epic's not-found page), read from the Internet Archive's copy https://web.archive.org/web/20261116120711/https://dev.epicgames.com/documentation/unreal-engine/lumen-world-cache-in-unreal-engine |
| Version | Revision 5004928, last revised 2026-11-10, tagged 5.9; archived 2026-11-16. Its text is the text the #287 readers read on 2026-11-16 |
| License | Epic's public documentation. Consulted, not copied |
| Read on | 2026-11-18, for issue #286 |
| Consulted by | The #286 implementer |
| Used in | Full as the default, the Windows and Linux entries, and which consumers the Simplified Material mode runs (`NEXTGI_HIT_SIMPLIFIED_CONSUMERS`): hit lighting needs Full; the World Cache (#294) may run on the simplified hit shader |
| Copied files | None |

The 5.9 Preview release notes (https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes,
"Last updated 11/17/26", read 2026-11-18) no longer carry the Lumen World Cache paragraph that the 2026-11-10 revision
had; nothing else changed.

How it was used, and what is this project's own: the three modes are UE's, and Full, the default, keeps the material
hit groups exactly as #130 built them. Inline makes no hit pipeline at all, which is what UE's "ray tracing material
shaders and select global shaders are not compiled" means for GodotNext: the material hit groups and the base
library (ray generation, miss and fallback group) are its only ray tracing pipeline shaders. For Simplified
Material, UE's "simplified material hit shader" is #130's fallback group, the one hit shader that shades every
geometry without the material's own code (hit distance, geometric normal and side; no material is looked up, so it
returns no material id), so the pipeline is the base library alone, with no attribute stream. Which features may
run on it follows the archived World Cache page until the plan's §25.3 re-check: hit lighting does not, the World
Cache does (#294 adds its consumer and what the simplified shader returns for it). UE's simplified hit shader "can
still give the World Cache the surface color it needs at each hit"; GodotNext's does not yet, so the surface
colour, and any per-material code or stream it needs, comes with #294. The debug check also runs on it, which
proves on a device that no material hit group exists. Translucency, far field, material retrace (#145, and #146
through it) and the path tracer (#167, which then traces with its INLINE backend and the D16 table materials) keep
their inline path, as UE's "Other features fall back to inline ray tracing"; each has its consumer bit, so tier
info reports the reason for it. UE's Disabled is
`rendering/global_illumination/nextgi/hardware_ray_tracing = Off`. The Windows and Linux entries use Godot's
feature-tag overrides, as `rendering/rendering_device/driver.windows` does.

## discard in the any-hit stage (#304), 2026-11-22

No new outside input. The `GL_EXT_ray_tracing` entry above covers `ignoreIntersectionEXT`: it is allowed only in
the any-hit stage, ends that invocation and drops the candidate, as `discard` ends a fragment invocation and drops
the fragment. Mesa NIR turns it into DXIL's `IgnoreHit()` on Direct3D 12, as it already did for the alpha scissor and
hash test. No Unreal Engine source, shader or plugin was opened.

What is this project's own: ShaderCompiler's hit mode writes a `discard` in `fragment()` itself as the template's
`NEXTGI_HIT_DISCARD`, which marks the hit and returns from `fragment()`; the any-hit stage then ignores the
intersection. A forced-opaque trace (GI, the path tracer) runs no any-hit stage and so reaches the closest-hit stage at
a discarded texel. There `fragment()` has written only what came before the discard, so the closest-hit stage reports
the hit with opacity 0 and the fallback flag plus `NEXTGI_HIT_PAYLOAD_DISCARDED`: the caller shades it as a
fallback-group hit (the path tracer keeps its D16 table entry), as it did before #304 when the shader was refused,
instead of with the template's default material. A `discard` in a helper function cannot leave `fragment()` that
way, so it stays `discard` and `nextgi_hit_classify()` refuses the shader (DISCARD), as #155's raster target refuses
it. A group that has the any-hit stage only for `discard` cuts nothing by alpha: alpha without a scissor or hash test
is blending, which ray-traced shadows treat as opaque (LQ6). A shader that discards in `fragment()` has an any-hit
stage, so #145's retrace verdict for it is RETRACE at Full; Simplified Material and Inline compile no material group,
so its casters stay opaque there. The hit check also traces the texels its raster capture did not cover, where a
discard panel's `fragment()` discarded: the closest-hit stage must report them discarded and the any-hit stage must
ignore them.
