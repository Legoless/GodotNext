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
- Shaders the material hit groups refuse (#130, `nextgi_hit_classify()`) cannot be retraced: their casters
  shadow as opaque. Since #304 a `discard` in `fragment()` is retraced (the any-hit stage ignores the
  intersection there); a `discard` in a helper function is still refused, as #155's raster target refuses it.
- The far field (merged per-cell proxies) traces every proxy as an opaque caster of every light: caster masks
  and cast shadow off apply to near-field instances only.

### Skinned alpha-tested RT shadows (#302), 2026-11-23

Written from the plan (§21.3 M4, D17), issue #302 and GodotNext's own #59 posed skinned slots, #130 attribute
streams / material hit groups and #145 RT shadow visibility policy. No outside paper, article or code was
consulted. Posed skinned surfaces reuse their mesh surface's static #130 attribute stream for UVs/attributes,
register their posed slot indices in the shared shadow alpha table, mark alpha-tested skinned TLAS entries
`FORCE_NO_OPAQUE` (deferred as shadow-only without the GI bit when the mesh material excludes them per D17),
and bind #130 hit groups on alpha-tested skinned slots so Full HWRT material retrace evaluates shader-code alpha.

### Light functions and the Light Function Atlas (#138, contract M8), 2026-11-24

Written from the plan (§21.3 contract M8, GM-11, M-9), issue #138, and Godot's own shader compiler,
`MaterialStorage`, volumetric fog shader compiler template (`FogShaderData`), Forward+ lighting shaders,
volumetric fog density pass, and NextGI SRC relight pass. No outside paper, article or code was consulted.

Contract decisions for M8:
- **Shader language:** `shader_type light_function;` with main function `void light_function()`, read-only
  built-ins `vec2 LIGHT_UV` (normalized `[0, 1]` tile UV), `float TIME`, and constants `PI`/`TAU`/`E`, and
  read-write output `vec3 LIGHT_MASK` (initialized to `vec3(1.0)`). In R8 atlas mode (default), the compute
  bake writes Rec.709 luminance `dot(clamp(LIGHT_MASK, 0.0, 1.0), vec3(0.2126, 0.7152, 0.0722))` to `r8`
  (falling back to `rgba8` if the device lacks `R8_UNORM` storage-image support); in RGBA8 mode, it writes
  `vec4(clamp(LIGHT_MASK, 0.0, 1.0), 1.0)`.
- **Atlas layout & dirty tracking:** Shelf-packed square tiles starting at 256 px and adaptively halving
  down to 16 px on capacity overflow so 1,024+ lights fit cleanly in the default 2048×2048 atlas. Static
  functions (no `TIME` usage) bake once on creation and re-bake only when `material_get_change_serial()`
  advances or the atlas resizes/repacks; animated functions (`TIME`) bake at most once per rendered frame.
  The atlas texture is lazily allocated on first active light function so scenes without light functions
  perform zero bakes and render byte-identically.
- **Projection & consumers:** Per-light `LightFunctionEntry` (80 bytes, std430) stores the half-texel-inset
  tile UV rect, world-to-light 3×4 transform, `light_function_scale` parameters, and mode/format flags:
  directional and area lights use wrapped planar projection, omni lights use octahedral projection of the
  scaled local direction, and spot lights use perspective frustum projection (returning `vec3(0.0)` behind
  the light). Shared helper `light_function_inc.glsl` is consumed by Forward+ (Set 1 bindings 62/63; opaque
  always, forward transparents gated on `rendering/lights_and_shadows/light_function_atlas/translucent`),
  Volumetric Fog (`MODE_DENSITY`, Set 0 bindings 27/28), and NextGI SRC relight (Set 0 bindings 29/30),
  and is ready for #141 RT shadows and #142 MegaLights.
### Alpha-tested NextGeo RT shadows (#303), 2026-11-24

Written from the plan (§21.3 M4, D17), issue #303 and GodotNext's own #68/#77 VGD RT proxies, #153 masked
NextGeo eligibility, #156 native NextGeo RT, #130 attribute streams / material hit groups and #145 RT shadow
visibility policy. No outside paper, article or code was consulted. VGD RT proxies carry parallel float32 UV0s
(`VirtualGeometryData::rt_proxy_uvs`, preserving UV seams via vertex-key welding), emit shadow-only proxy parts
for alpha-excluded surfaces (D17), pack lazy #130 attribute streams on demand for alpha-tested NextGeo proxy parts
only (leaving opaque NextGeo RT proxies unchanged), and extract #130 attribute streams in `rt_cut_extract.glsl`
for #156 native NextGeo BLASes when an instance carries an alpha-tested effective material. Alpha-tested NextGeo
TLAS instances register in the #145 alpha table, set `FORCE_NO_OPAQUE`, and bind #130 hit groups for Full HWRT
material retrace.

### Dual-route RT shadows for BaseMaterial3D-pattern shaders with discard (#307), 2026-11-24

Written from the plan (§21.3 M4), issue #307 and GodotNext's own #145 RT visibility policy, #286 Hardware Ray
Tracing Mode and #304 any-hit discard support. No outside paper, article or code was consulted. Spatial shaders
matching BaseMaterial3D's alpha-scissor pattern that also contain a `discard` statement (`ShaderData::uses_discard`)
now keep their bindless texture-alpha entry AND also set `NEXTGI_SHADOW_ALPHA_RETRACE` when retrace is available
and the shader's retrace verdict is not `OPAQUE` (`nextgi_shadow_alpha_route()` / `nextgi_shadow_alpha_route_for_mode()`).
`nextgi_shadow_ray_inc.glsl` therefore retraces at Full mode (so the any-hit stage evaluates both the alpha test
and `fragment()`'s `discard`) and falls back to the bindless texture-alpha cut — instead of shadowing as opaque —
in Simplified Material, Inline, on GPU-written instances, on fallback lights, or when the hit groups refuse the
shader (such as a `discard` in a helper function per #304).
### Light-source parity (#140, contract M8, GM-11), 2026-11-24

- ANSI/IESNA LM-63-1995 and LM-63-2002 (*IESNA Standard File Format for Electronic Transfer of Photometric
  Data*, public lighting-industry standard). Used only for the documented ASCII file format grammar, header
  keywords, `TILT=NONE` / `TILT=INCLUDE` handling, photometric type C/B/A coordinate conventions, horizontal
  symmetry rules (0°, 90°, 180°, 90°–270°, 360°), and candela multiplier fields. No manufacturer `.ies`
  files or third-party parser implementations were read or included; all test profiles are synthetically
  generated.

- Barn-door rectangle clipping (`barn_door_clip_rect`, `barn_door_clip_texture_rect`), lazy 256-layer R16F
  IES atlas allocation (`IESAtlasAllocator`), textured rect-light evaluation in NextGI SRC relight (reusing
  Godot's `area_lights_inc.glsl` LTC helpers and `area_light_atlas`), and per-light contact shadow controls
  (reusing Godot's `screen_space_contact_shadows.glsl` wavefront ray march and #139's `barn_door_angle` /
  `barn_door_length` culling) were written from first principles, plan §21.3 contract M8, issue #140, and
  Godot's own Forward+ and NextGI shaders. No Unreal Engine source, shaders, or third-party code were consulted.

### Standalone ray-traced shadows (#141, contract M9), 2026-11-24

Written from the plan (§21.3 contract M9, GM-3, GM-4, budget M-7, §8.2 ray origin offset, LQ6 translucent-as-opaque
shadow policy), issue #141, and GodotNext's own subsystems: #145 (`trace_shadow()` visibility policy, 20-layer caster
masks, bindless alpha cutout and #130 material retrace), #37 (prepass velocity plan), #54 (64x64 RG8 blue-noise tile),
#221 (`Light3D.ShadowMethod` / tier reporting), and Forward+ light shading. No outside source code was consulted.

Contract decisions for M9:

- **Tile-sparse R8 shadow-mask atlas (`16x16` pixel tiles):** Each `(light, visible 16x16 screen tile)` allocates one
  `16x16` tile in an `R8_UNORM` atlas (translucent casters shadow as opaque per LQ6, so a single scalar visibility
  channel suffices). Quality presets budget `16,384` (Low, 4 MiB), `32,768` (Medium, 8 MiB), `65,536` (High, 16 MiB,
  matching budget M-7), and `131,072` (Epic, 32 MiB) tiles per view, with no fixed light-count cap.
- **Counted overflow fallback:** Lights that overflow the per-view tile budget increment GPU overflow counters, report
  back asynchronously, and fall back cleanly to VSM or shadow atlas (`light_uses_vsm_fallback`) on subsequent frames
  until the tile budget has headroom again.
- **Auxiliary hit-distance atlas (`R16_SFLOAT`):** Records per-pixel blocker distance from `trace_shadow()` for both
  penumbra-guided spatial denoising (`hit_distance / light_size`) and subsurface transmittance
  (`LIGHT_PARAM_TRANSMITTANCE_BIAS`).
- **Per-light spatio-temporal denoising (`NextGIDenoise::shadow_denoise()`):** Temporal accumulation uses #37 prepass
  velocity with a twin history atlas, invalidating tile history whenever a tile's stable light ID or screen-tile owner
  changes (or on depth/normal disocclusion), followed by 2 à-trous spatial passes (step sizes 1 and 2) guided by depth,
  normal, and penumbra radius.
- **Zero-cost setting-off path:** When `rendering/lights_and_shadows/ray_traced_shadows` is `false` and no visible
  light uses `SHADOW_METHOD_RAY_TRACED`, `NextGIRTShadows` is never instantiated, zero atlas VRAM is allocated, zero
  RT shadow passes run, and specialization bit 15 (`use_rt_shadows`) remains `false`.

### MegaLights sampling core (#142, contract M3, GM-1, GM-2, budget M-3), 2026-11-24

- Standard published Monte Carlo & computer-graphics textbook algorithms implemented from first principles:
  - Weighted Reservoir Sampling (Chao / Efraimidis-Spirakis / streaming `w_i / w_sum` single-pass WRS) for
    unbiased K-reservoir per-pixel light selection over the #139 packed froxel cell list.
  - Per-slice stratified 3D periodic void-and-cluster (Ulichney 1993 void-and-cluster generalized to a
    3D periodic `(64, 64, 32)` Gaussian kernel with spatial `sigma_xy = 1.9`, temporal `sigma_t = 1.2`)
    written from scratch in `godotnext/tools/blue_noise/generate.py` to generate the committed 64×64×32
    RG8 spatio-temporal blue-noise table (`servers/rendering/renderer_rd/effects/megalights_stbn.h`).
  - Welford online mean and M2 variance recurrence for reference-mode convergence measurement.
  - Octahedral unit-vector encoding packed into `snorm16x2` (Cigolle et al. 2014 *Survey of Efficient
    Representations for Independent Unit Vectors*).
- Everything else — `ml_light_eval_inc.glsl`, `ml_generate_samples.glsl`, `ml_guiding.glsl`,
  `ml_reference.glsl`, `MegaLights` / `MegaLightsViewData`, and the Forward+ pre-opaque wiring — was
  written from plan §21.3 contract M3, issue #142, and GodotNext's own #136 direct-light inputs, #138
  Light Function Atlas, #139 packed froxel light grid, #140 IES/barn-door/textured rect light helpers,
  #145 `nextgi_shadow_ray_inc.glsl` (`trace_shadow()`), and Godot's Forward+ PBR / LTC area-light code.
  No Unreal Engine source, shaders, NVIDIA/NvRTX samples, or third-party STBN tables were consulted.

Contract decisions for M3:
- **Sample buffer packing (16 bytes/sample, budget M-3 & 64 MiB VRAM cap):** Each sample is a packed
  `uvec4` (`light_packed`, `floatBitsToUint(inv_pdf)`, `packSnorm2x16(oct(ray_dir_ws))`,
  `floatBitsToUint(max_distance)`), storing 4 spp at 1080p half-res in ~31.6 MiB (full-res 4 spp
  in 126.6 MiB, well under the 64 MiB default-half-res MegaLights budget) while keeping #146 visibility
  and #135 denoiser inputs compact and aligned.
- **Continuous sub-quantized STBN:** The committed 64×64×32 RG8 table provides exact uniform 256-bin
  blue-noise strata per slice; `ml_light_eval_inc.glsl` dithers within each 1/256 bin using a 24-bit
  PCG sub-offset so the estimator stays strictly unbiased even for very dim candidate lights
  (`w_i / w_sum < 1/256`). Soft-shadow emitter sampling uses the full 64×64×32 spatio-temporal cycle
  (with epoch shifts every 32 frames); adaptive WRS light selection (`ml_stbn_select_u`) re-hashes the
  shared toroidal shift and slice each frame to preserve spatial blue noise within a frame while
  remaining conditionally independent of last frame's tile guiding table.
- **Unbiased visible-light guiding floor:** `ml_guiding.glsl` aggregates up to 32 visible light IDs
  and hit counts per 8×8 screen tile into a 36-uint (144-byte) ping-pong record. `ml_generate_samples.glsl`
  applies `mix(0.25, 1.0, count / max_count)` to lights present in the previous frame's tile and keeps
  a strictly positive weight floor `0.25` for unlisted/occluded lights so every light retains non-zero
  selection probability and the weighted estimator remains mathematically unbiased.
- **Provisional defaults (tuned in PQ):** `samples_per_pixel = 4` (quality presets Low/Medium=2,
  High=4, Epic=8), `downsample = 1` (`Half`, 2×2 Bayer cycle `(0,0),(1,1),(1,0),(0,1)`),
  `directional_lights = false`, `falloff_cull_threshold = 0.001`, `reference_mode = false`.

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
