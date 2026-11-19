# MegaLights source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used to write MegaLights code, and the contract decisions
that the plan and the issues leave open.

## Approved inputs

The direct-light inputs (#136) were written from the plan (§21.3, contract
M1), the issue and Godot's own Forward+ shaders. No paper, article or outside code was
consulted. The same holds for the 2026-11-09 follow-up below (decals, alpha-pass fallback),
which reuses Godot's own decal loop.

### RT visibility policy (#145, contract M4), 2026-11-18

- Epic Games, *Hardware Ray Tracing* (Unreal Engine 5.9 documentation, public page, read on 2026-11-18),
  Supported Ray Tracing Features table. Used for one behavior, not for code: the Light Transmission row is
  marked not supported with the note "The translucent shadow is treated as opaque, meaning no colored shadows or
  light transmission through the material"; the Translucent Shadows row is supported and the Colored Shadows row
  is not. GodotNext's ray-traced shadow rays therefore treat translucent (alpha-blended) casters as opaque (lead
  question LQ6, decided by following UE 5.9). Nothing else from the page was used.

- Khronos, `GL_EXT_ray_tracing` and `GL_EXT_ray_query` GLSL extensions and the Vulkan specification's ray
  traversal chapter (public). Used only for the documented meaning of the ray flags and instance flags the
  policy relies on (opaque and non-opaque instances, `TerminateOnFirstHit`, `SkipClosestHitShader`, cull masks).

The mask allocation, the instance and light words, the candidate loop and GM-4's check were written from the
plan (§21.3 M4, GM-4), the issue and Godot's own shadow caster rule (`renderer_scene_cull.cpp`: a caster
shadows a light when its layer mask meets the light's caster mask and its cast shadow setting is not off). The
bindless-texture alpha path reads BaseMaterial3D's own generated alpha (`scene/resources/material.cpp`), and the
material retrace reuses #130's hit groups and their any-hit stages.

Where the RT caster rule differs from Godot's raster shadow pass, on purpose (decided 2026-11-18, recorded for
review with GM-4):

- The camera's cull mask is not applied. The raster pass draws a caster only when the camera's cull mask shows it;
  a ray-traced shadow is view-independent (the surface cache relights for every view), so a caster on a layer the
  camera hides still casts.
- The raster pass drops alpha-blended materials, including alpha scissor with alpha antialiasing
  (`casts_shadows()` is false). The rays treat alpha scissor (with or without antialiasing) and alpha hash as
  alpha-tested at their threshold, and alpha blending as opaque (LQ6, following UE 5.9 above).
- The bindless alpha path samples the albedo texture's top mip with its declared filter and repeat (emulated with
  one linear repeating sampler); the raster pass also uses the mips.
- Shaders that cut with `discard`, and any other shader the material hit groups refuse (#130,
  `nextgi_hit_classify()`), cannot be retraced: their casters shadow as opaque.
- The far field (merged per-cell proxies) traces every proxy as an opaque caster of every light: caster masks
  and cast shadow off apply to near-field instances only.

## Contract decisions

### M1 direct-light input targets (#136), 2026-11-08 — for later review

**Status: approved by the lead on 2026-11-08, on the reviewer's proposal. Flagged for
later review**, when #143 (MegaLights in the frame) consumes the targets and when the
PQ spike #144 compares the input paths.

The plan's M1 lists only "`ml_albedo` and `ml_material` (F0, shading model, layer-group
index)". Issue #136 contradicted itself: its summary called `ml_albedo` RGB the
*diffuse* albedo, while its acceptance line compared it with the color pass's `ALBEDO`,
the base color. The prepass also stored nothing a consumer could recover metallic
from. These decisions settle the contract so that #143 can reproduce the clustered
loop exactly:

| Field | Decision | Why |
|---|---|---|
| `ml_albedo.rgb` | Diffuse albedo, `ALBEDO * (1 - METALLIC)`. The PR check compares it with `ALBEDO * (1 - METALLIC)` within 1/255, on materials with metallic 0, 0.5 and 1. | This is the diffuse reflectance the clustered loop applies (`diffuse_light *= albedo * (1 - metallic)`). F0 already carries the metal tint, and metallic cannot be recovered from the packed F0. |
| `ml_albedo.a` | Direct-light occlusion, `mix(1.0, ao, ao_light_affect)` (the bent-normal variant where the color pass uses one for direct light). The field is no longer "specular occlusion". | This is the factor Forward+ applies to direct diffuse and specular light. The material's raw AO, or Godot's ambient-based specular occlusion, would make MegaLights frames darker or lighter than MegaLights-off frames. |
| Fallback (shading model 3, keeps the clustered loop) | Custom `light()`, `unshaded`, `diffuse_toon`, `diffuse_lambert_wrap`, `specular_toon` and `specular_disabled` (the issue's list), plus rim, backlight, anisotropy and `diffuse_lambert`. Transmission (model 1) requires SSS together with transmittance, as the color pass does. | `ml_material` cannot express rim, backlight, anisotropy or Lambert diffuse (Godot's default is Burley), so those materials stay on the clustered loop. Their pixels stay byte-identical to MegaLights-off frames. |

Issue #136's acceptance line is reworded to compare against `ALBEDO * (1 - METALLIC)`
when the change lands. The decision does not change the plan's M1 text; if #143 or the
spike #144 needs a different packing, revise it with the lead and record the change here.

### M1 follow-up (#136), 2026-11-09

These changes apply the approved contract above; they do not change the packing.

- **Decals.** The contract defines `ml_albedo` and the F0 in `ml_material` as what the
  clustered loop applies. Forward+ decals change the albedo, AO and metallic the clustered
  loop uses, so the MegaLights input prepass now runs the same decal loop as the color
  pass. The light cluster is baked after the depth prepass, so the frame bakes a
  decal-only cluster before a MegaLights input prepass; the full cluster is rebuilt later
  in the frame as before. A material without `fragment()` takes the same path, so decals
  reach it as well.
- **Normal-roughness stays as upstream.** The issue reuses the existing normal-roughness
  target, and SSR, SSAO and NextGI read it, so the decal normal and the decal roughness
  are not written to it. #143 has to account for that on decal pixels.
- **Screen-space AO is not in `ml_albedo.a`.** The color pass also folds SSAO (with its
  light-affect amount) into the direct-light occlusion. SSAO runs after the prepass, so
  `ml_albedo.a` holds the material and decal occlusion only; #143's composite applies SSAO.
- **Alpha-pass surfaces with a depth prepass are fallback.** Materials with
  `depth_prepass_alpha` or alpha antialiasing are drawn by the depth prepass but shaded by
  the alpha pass, which keeps the clustered loop; the opaque color pass never shades
  them. They now write shading model 3, as the fallback rule says.

**Open for the lead.** By the fallback rule above, a material `ml_material` cannot express
stays on the clustered loop. Clearcoat (model 2) needs the clearcoat amount and roughness,
and SSS transmission (model 1) needs the transmittance color, depth and boost; the targets
store only flag bits for them. Marking them fallback would contradict the issue's models
1 and 2 and plan M7 ("SSS pixels evaluate transmission per sample"); carrying their
parameters changes the approved packing. Until the lead decides, models 1 and 2 are
written as before, and #143 cannot reproduce those two lobes from the targets alone.
