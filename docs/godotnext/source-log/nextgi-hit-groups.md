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
