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

### MegaLights visibility chain (#146, contract M4, GM-1, GM-8, GM-12, budget M-4), 2026-11-24

Written from first principles using plan §21.3 contract M4 (GM-1, GM-8, GM-12, budget M-4), issue #146,
and GodotNext's own subsystems: #142 MegaLights sampling core (`ml_generate_samples.glsl`, `ml_guiding.glsl`,
`MegaLights` / `MegaLightsViewData`), #145 `nextgi_shadow_ray_inc.glsl` (`trace_shadow()`, 6-bit TLAS caster
masks, bindless alpha cutout), #130 material retrace hit groups (`NextGIHitMaterials::dispatch_shadow_retrace`),
#126 far-field TLAS (`NextGIFarField`), #182 global SDF clipmaps (`nextgi_sdf_trace_inc.glsl`, `NextGISWRTTrace`),
#222 Virtual Shadow Maps (`vsm_smrt_inc.glsl`, `vsm_shadow_directional()`, `vsm_shadow_local()`), #171 Metal
inline ray query tier rules, and Godot's Hi-Z depth pyramid (`ss_effects->build_hiz`) and SSCS
`rendering/lights_and_shadows/contact_shadow/surface_thickness` setting. No Unreal Engine source or shaders,
UE plugins, NvRTX, or third-party visibility-chain implementations were consulted.

Contract decisions for M4:
- **Hi-Z screen trace (`ml_screen_trace.glsl`):** Marches a short view-space segment against the Hi-Z depth
  pyramid (`MAX` steps per quality preset: Low=8, Medium=12, High=16, Epic=24), blending from a short near
  trace length (`1.0 m`) to `rendering/megalights/distant_screen_traces_length` (default `4.0 m`) for distant
  pixels beyond `16 m` linear depth. Uses Hi-Z mip for empty-space skipping and always confirms hits against
  exact mip-0 depth using the project's `rendering/lights_and_shadows/contact_shadow/surface_thickness`
  thickness band (`max(0.02, scene_z * surface_thickness)`), with a grazing-angle guard (`dot(N, L) >= 0.12`)
  and `>= 1.5 px` start offset so screen-trace false occlusion stays well below 0.5%. Screen hits write
  `visibility = 0` and their world-space hit distance immediately and are excluded from ray tracing.
- **2×2 block ray deduplication and stream compaction (`ml_screen_trace.glsl`):** Within each 2×2 sample
  block, unresolved non-VSM samples are compacted into a dense `ray_list` (`MLVisRay`, 32 B) while merging
  samples that select the same light with origins within 1 texel of slope-aware depth tolerance, matching
  normals (`dot >= 0.90`), and aligned ray directions (`dot >= 0.995`), tracking the conservative minimum
  resume distance across merged followers. Samples belonging to `Light3D.MEGALIGHTS_SHADOW_METHOD_VIRTUAL`
  lights are compacted into a separate `vsm_list` (`MLVisVSMItem`, 8 B) and never enter `ray_list`.
- **Single-dispatch ray query + #130 retrace + optional far field (`ml_trace_rays.glsl`):** One indirect
  compute dispatch over `ray_list` calls #145 `trace_shadow()` for near-field HWRT + bindless alpha cutout
  and appends material-retrace requests to a shared retrace buffer resolved via #130
  `NextGIHitMaterials::dispatch_shadow_retrace()` + `MODE_RETRACE_RESOLVE`. When `rendering/megalights/far_field`
  is enabled (off by default) and the #126 far-field TLAS is ready, rays exiting the near-field radius continue
  against the far-field TLAS.
- **Optional non-RT SWRT continuation (`ml_trace_rays_swrt.glsl`):** Compiled as a separate non-ray-query
  shader (zero `GL_EXT_ray_query` requirement) so non-RT devices compile cleanly. When
  `rendering/megalights/swrt_continuation` is enabled (off by default; labeled as a GodotNext extension on
  the non-RT tier) and hardware ray queries are unavailable, unresolved rays trace the #182 global SDF clipmap
  starting from the screen-trace exit distance (`t_resume` pulled back by 8 cm).
- **VSM-method lights (`ml_trace_vsm.glsl`):** Compiled as a separate non-ray-query shader. On-demand VSM
  activation allocates virtual maps for `MEGALIGHTS_SHADOW_METHOD_VIRTUAL` lights when MegaLights is active
  (leaving zero allocations when MegaLights is off), and `ml_trace_vsm.glsl` evaluates `vsm_shadow_directional()` /
  `vsm_shadow_local()` with SMRT rays fixed to `2` plus `vsm_transmittance_*()` for hit distance.
- **Outputs & guiding feedback:** Writes packed RGB transmittance (`RGBA8`) into `visibility_buffer` and
  per-sample hit distance (`R16_SFLOAT` 2DArray, layer = sample index, miss = `65504.0`) into
  `hit_distance_texture` ready for #135 denoiser, and feeds `visibility_buffer` directly into #142's
  `ml_guiding.glsl`. Mac Apple M2+ inline-ray-query-only devices (#171) are reported cleanly as
  `HWRT_INLINE_ONLY` with `experimental = true` and `retrace_supported = false` via
  `RenderingServer.megalights_get_tier_info()`. Forward+ Set 1 bindings remain untouched (reserved for #143).

### MegaLights denoiser (#135, contract M5, GM-1, GM-7, budget M-5), 2026-11-24

- Standard published real-time denoising & PBR split-sum textbook algorithms implemented from first principles:
  - Spatio-temporal variance-guided filtering (SVGF — Schied et al. 2017: temporal luminance first/second moments
    feeding per-pixel variance into 2-iteration à-trous edge-stopping spatial filtering with variance propagation).
  - Multiscatter split-sum specular EnvBRDF demodulation matching Godot's own Forward+ `prefiltered_dfg()` /
    `get_energy_compensation()` (`scene_forward_clustered.glsl`) and #205 `rr_specular_albedo`, plus Karis 2014 /
    Lazarov 2013 analytical GGX split-sum polynomial for the CPU `env_brdf_approx()` helper.
- Everything else — `ml_shade_samples.glsl`, `ml_denoise_temporal.glsl`, `ml_denoise_spatial.glsl`,
  `NextGIDenoise::megalights_denoise()`, `MegaLights::shade_samples()` / `denoise_samples()`, debug views,
  and Forward+ prepass velocity wiring — was written from plan §21.3 contract M5, issue #135, and GodotNext's
  own #136 M1 direct-light inputs, #142 sampling core, #146 visibility chain (`visibility_buffer` +
  `hit_distance_texture`), #37 prepass velocity plan, #53/#54/#141 `NextGIDenoise` temporal/firefly/reset-region
  patterns, and `nextgi_view_inc.glsl`. No Unreal Engine source or shaders, UE plugins, NvRTX, or DLSS/Donut
  source were consulted.

Contract decisions for M5:
- **Demodulated shading & half-res fill (`ml_shade_samples.glsl`):** `MODE_SHADE` shades `(sample, visibility)`
  at sampling resolution and divides diffuse radiance by `max(diffuse_albedo, 0.01)` and specular radiance by
  `max(spec_env_brdf, 0.01)` (the exact multiscatter split-sum EnvBRDF term used by Forward+ and #205
  `rr_specular_albedo`), writing two `RGBA16F` targets (diffuse RGB + radiance-weighted occluded hit distance;
  specular RGB + valid flag). In `downsample = half` mode, `MODE_FILL_HALF` keeps each 2×2 block's sampled pixel
  untouched and fills the remaining 3 pixels via a 2×2 bracketing joint-bilateral stencil guided by plane-distance
  depth, view normal, roughness, and shading model. Structured with an explicit per-sample hook for #137's SSS
  back-face transmission.
- **Temporal accumulation, firefly clamp & moments (`ml_denoise_temporal.glsl`):** Reprojects at render
  resolution via #37 prepass velocity (on dynamic surfaces) or `prev_view_proj`, applies 2×2 bilinear history
  fetching with depth & world-normal disocclusion rejection, tracks diffuse and specular luminance moments
  (`m1, m2 -> variance`) and packed `R32_UINT` sample counts (`diff_len | (spec_len << 8)`), honors
  `denoise_reset_regions` (16×16 tiles via `NextGIDenoise::select_reset_probes`), resize, camera cuts, and first
  frame, and applies #54-style firefly clamping with a shared-memory workgroup reduction to a 16-byte GPU
  clamped-energy counter (`removed_fp`, `total_fp`, `clamped_pixels`, `valid_pixels` in physical direct-radiance
  units, read back asynchronously for the <= 2% PQ metric).
- **2-pass variance-guided à-trous spatial filter (`ml_denoise_spatial.glsl`):** Runs 2 iterations (step widths
  1 and 2) whose edge-stopping luminance tolerance and kernel width follow smoothed temporal variance, guided by
  plane-distance linear depth, view normal, roughness, shading model, and #146 hit distance (tightening near
  contact shadows while smoothing soft penumbrae). Spatial pass 2 writes final denoised render-resolution
  `RGBA16F` targets into the dead previous-history ping-pong slot while `history_*_cur` preserves clean
  pre-spatial temporal history for the next frame (avoiding 2 redundant full-res `RGBA16F` allocations).
- **RR Bypass & temporal upscaler handoff (`rendering/megalights/denoiser`):** Default `0` (`Builtin`) runs
  shade + temporal + 2-pass spatial denoise and outputs render-resolution `RGBA16F` demodulated diffuse and
  specular targets ready for #143's Forward+ composite before TAA, TSR, FSR2, MetalFX, and DLSS SR. Setting `1`
  (`RR Bypass`) skips temporal and spatial passes, allocates zero temporal/spatial history textures, and exposes
  the noisy demodulated diffuse and specular targets directly alongside surface guides for DLSS-RR (PU+).
- **Debug visualization & zero-cost off path:** Appends `VIEWPORT_DEBUG_DRAW_ML_NOISY` (70),
  `VIEWPORT_DEBUG_DRAW_ML_DENOISED` (71), and `VIEWPORT_DEBUG_DRAW_ML_VARIANCE` (72) after
  `VIEWPORT_DEBUG_DRAW_NEXTGI_WORLD_CACHE` (69); `MODE_DEBUG_VIS` renders on demand into a lazily allocated
  debug target only when one of the 3 modes is selected. When MegaLights is disabled, zero MegaLights shade,
  denoise, velocity, or debug buffers are allocated and legacy frames stay byte-identical. Forward+ color-pass
  Set 1 bindings (`69+`) and `ShaderSpecialization::packed_1` bit `16+` remain untouched (reserved for #143).

### MegaLights in the frame (#143, contracts M1 & M6, GM-9, GM-10), 2026-11-24

Written from first principles using plan §21.3 contracts M1 and M6 (GM-9, GM-10), issue #143, and GodotNext's
own subsystems: #136 M1 direct-light inputs, #139 packed froxel light grid (`LIGHT_GRID_ENTRY_FLAG_MEGALIGHTS`),
#142 sampling core, #146 visibility chain (`Light3D.megalights_shadow_method`), #135 denoiser (demodulated diffuse
and specular targets), #55 NextGI quality presets and RT-scene composition rules, #31 NextGI SRC relight
(`next_gi_relight.cpp`, `nextgi_src_relight_inc.glsl`), and `NextGIEditorPlugin`. No Unreal Engine source or
shaders, UE plugins, NvRTX, or third-party implementations were consulted.

Contract decisions for M1 & M6:
- **Project setting, Auto quality presets (#55) & shared RT-scene composition:** Registers
  `rendering/megalights/enabled = false` (opt-in). `rendering/megalights/samples_per_pixel = 0` (Auto) and
  `rendering/megalights/downsample = 2` (Auto) resolve from the active NextGI quality preset (Low: 2 spp / Half /
  8 screen-trace steps; Medium: 2 spp / Half / 12 steps; High: 4 spp / Half / 16 steps; Epic: 8 spp / Full /
  24 steps), while explicit user values override Auto. MegaLights shares `NextGIRTScene` directly so Medium
  exclusions automatically match NextGI.
- **Status labels & non-blocking editor HWRT offer:** `MegaLights::resolve_visibility_tier()` and
  `RenderingServer.megalights_get_tier_info()` report `status_label` (`"Production"` on Windows/Linux HWRT,
  `"Experimental"` on Apple M2+ inline ray query, `"Unavailable"` with an explicit `status_warning` on Mobile
  and Compatibility) plus live `memory_bytes`. `Light3D::get_configuration_warnings()` warns when MegaLights is
  enabled on an unsupported renderer, `EditorInspectorPluginMegaLights` displays the status label on `Light3D`,
  and `NextGIEditorPlugin` shows a one-time non-blocking `ConfirmationDialog` offering to enable HWRT when
  `rendering/megalights/enabled` flips from `false` to `true` while HWRT is disabled on a ray-query-capable device.
- **Per-light opt-in (`Light3D.megalights`) & `LIGHT_FLAG_MEGALIGHTS`:** Adds `Light3D.MegaLightsMode`
  (`MEGALIGHTS_PROJECT`, `MEGALIGHTS_ALLOW`, `MEGALIGHTS_DISALLOW`) alongside `#146`'s `megalights_shadow_method`
  and wires `RenderingServer.light_set_megalights()`. Replaces the first padding word in `LightData` (288 B) and
  `DirectionalLightData` (512 B) with `uint32_t light_flags` carrying `LIGHT_FLAG_MEGALIGHTS = (1u << 0u)` (also
  consumed by #137 volumetric fog) and `LIGHT_FLAG_MEGALIGHTS_DISALLOW = (1u << 1u)`, and propagates
  `is_ml_light` into `ClusterBuilderRD::add_packed_light()` (packed light-grid bit 18).
- **Frame order & Forward+ color-pass composite (`scene_forward_clustered.glsl`):** Executes in `_render_scene` /
  `_pre_opaque_render` strictly as: depth prepass (+ M1 inputs) → light buffers + packed light grid → VSM marking/
  raster/projection → `MegaLights::render()` [sample → visibility → guiding → shade + denoise] → Forward+ opaque
  color pass (regular Forward+ meshes and NextGeo Stage B tile quads & compute shading bins). Binds Set 1
  `69` (`megalights_diffuse`), `70` (`megalights_specular`), `71` (`megalights_material`, defaulting to
  `DEFAULT_RD_TEXTURE_BLACK` when off or during the depth prepass to avoid SyncVal hazards) and
  `ShaderSpecialization::packed_1` bit `16` (`use_megalights`, cleared during depth prepass and transparent pass).
  Non-fallback pixels re-modulate `diffuse_direct * max(albedo * (1 - metallic), 0.01) + specular_direct * max(EnvBRDF, 0.01)`
  scaled by the exact SSAO direct-occlusion ratio (since `ml_albedo.a` carries material/decal AO only) and skip
  `LIGHT_FLAG_MEGALIGHTS` lights in the directional, packed light-grid, and legacy clustered loops; fallback pixels
  (`MEGALIGHTS_FALLBACK` or M1 model 3, e.g. custom `light()`) skip the composite and run the full clustered loop
  identically to MegaLights off.
- **GM-9 off identity & zero allocations:** When `rendering/megalights/enabled = false`, `RenderSceneBuffersRD`
  and `MegaLightsViewData` allocate zero buffers (`memory_bytes == 0`), zero MegaLights passes run,
  `use_megalights = false`, `LIGHT_FLAG_MEGALIGHTS = 0`, and rendered frames are byte-identical to baseline.
- **NextGI SRC interplay (GM-10):** In `NextGIRelight::relight()`, MegaLights lights whose shadow method is not
  `VIRTUAL` skip shadow-atlas reuse (`NEXTGI_LIGHT_REUSE_NONE`) so `nextgi_src_relight_inc.glsl` traces ray-traced
  card shadows while rotating `<= 2` shadowed lights per texel per frame, and enforces the R2 cap
  (`NEXTGI_SRC_RELIGHT_MEGALIGHTS_HIGH_RAY_CAP = 256000`) at High quality (`src_relight_rays <= 256000`).

### MegaLights fog, translucency, front-layer pass & SSS transmission (#137, contract M7, GM-6, budget M-8), 2026-11-24

- Standard published volumetric lighting, spherical-harmonic irradiance, and subsurface Beer-Lambert textbook
  algorithms implemented from first principles:
  - Order-1 zonal-harmonic clamped-cosine projection (`L0 = 0.25/pi * E`, `L1 = 0.5/pi * E * L_ws`, matching
    exact hemisphere energy conservation and GodotNext's #61 D17 translucency volume layout `L0 + dot(N, L1)`).
  - Henyey-Greenstein phase function matching Godot's `volumetric_fog_process.glsl` sign convention.
  - Beer-Lambert exponential thickness attenuation (`exp(-thickness * extinction)`) with wrapped back-face
    transmission lobe (`clamp(boost - NdotL, 0, 1) / pi`) for SSS back-face transmission.
- Everything else — `ml_froxel_inc.glsl`, `ml_froxel.glsl`, `ml_froxel_compute.glsl`, `ml_front_layer.glsl`,
  `MegaLights::render_froxel()` / `render_front_layer()`, `NextGIWRC` conditional D17 direct SH-L1 channel,
  `Fog` / `volumetric_fog_process.glsl` MegaLights froxel integration, Forward+ transparent composite, and
  SSS per-sample transmission in `ml_shade_samples.glsl` / `ml_reference.glsl` — was written from plan §21.3
  contract M7 (GM-6, budget M-8), issue #137, and GodotNext's own subsystems (#61 D17 translucency volume and
  front-layer prepass, #138 Light Function Atlas, #139 packed froxel light grid, #140 IES profiles & area barn
  doors, #143 `LIGHT_FLAG_MEGALIGHTS`, #145 `trace_shadow()`, #146 `hit_distance_texture`, and #215 VSM coarse
  pages). No Unreal Engine source or shaders, UE plugins, NvRTX, or third-party implementations were consulted.

Contract decisions for M7:
- **Shared froxel lighting volume (`ml_froxel.glsl`, `ml_froxel_compute.glsl`, GM-6):** One stochastic MegaLights
  dispatch per frame (default `1` sample/froxel/frame on Low/Medium/High, `2` on Epic) evaluates all eligible
  `LIGHT_FLAG_MEGALIGHTS` lights with IES profiles (#140) and Light Function Atlas (#138), traces visibility ONCE
  via #145 `trace_shadow()` (or #215 `vsm_fog_shadow_coarse_*()` for `VIRTUAL` lights and non-RT fallback),
  temporally reprojects and accumulates volumetric in-scattering radiance into `ml_froxel_fog`, and projects the
  exact same sample set into SH-L1. When both volumetric fog and transparents are active (`shared_fog_d17`),
  `MODE_RESAMPLE_D17` projects the shared fog-grid SH-L1 into the D17 volume via a single trilinear lookup with
  zero additional light loops or rays; when volumetric fog is disabled (`d17_only`), Pass 1 dispatches directly
  on the D17 grid with zero fog froxel allocations.
- **Volumetric fog integration (`fog.{h,cpp}`, `volumetric_fog_process.glsl`):** Binds the accumulated MegaLights
  froxel 3D texture at Set 0 binding `30` (`MODE_DENSITY` only, defaulting to `DEFAULT_RD_TEXTURE_3D_BLACK` when
  inactive) and uses the first spare padding word of `VolumetricFogShader::ParamsUBO` for `ml_froxel_active`.
  In `volumetric_fog_process.glsl`, directional, omni, spot, and area light loops skip `LIGHT_FLAG_MEGALIGHTS`
  lights and add `ml_froxel_light_tex` once per froxel, preserving #215 VSM coarse-page bindings cleanly.
- **Conditional D17 direct SH-L1 channel (`next_gi_wrc.{h,cpp}`, `next_gi.{h,cpp}`):** Adds ping-pong `RGBA16F`
  direct-light SH-L1 volumes (`1/16 screen x 32` log slices, `near = 0.05`, `far = 64.0`) allocated strictly while
  MegaLights is enabled and freed immediately when MegaLights turns off (`d17_direct_memory_bytes == 0`). Works
  independently of whether NextGI GI itself is enabled (allocating only the direct channel, never the WRC).
- **High-quality front-layer translucency (`rendering/megalights/front_layer_translucency = false`, `ml_front_layer.glsl`):**
  Off by default (allocating zero front-layer MegaLights buffers). When enabled with transparent meshes present,
  reuses #61's front-layer depth and normal-roughness prepass (`NextGIFrontLayer`), synthesizes neutral demodulated
  M1 guides via `ml_front_layer.glsl`, and runs the full MegaLights sampling, visibility (with opaque Hi-Z screen
  traces disabled), guiding, shading, and spatio-temporal denoising pipeline on a separate per-view `MegaLightsViewData`
  history (`megalights_front_layer`). Forward+ Set 1 binds D17 direct SH-L1 at `72..74`, front-layer denoised
  diffuse/specular at `75..76`, and `ShaderSpecialization::packed_1` bits `17` (`use_megalights_translucent`) and
  `18` (`use_megalights_front_layer`): pixels matching front-layer depth use the high-quality per-pixel pass while
  deeper layers automatically fall back to the D17 direct SH-L1 volume.
- **SSS back-face transmission (`ml_light_eval_inc.glsl`, `ml_generate_samples.glsl`, `ml_trace_rays.glsl`,
  `ml_shade_samples.glsl`, `ml_reference.glsl`):** For SSS/transmission surfaces (`ML_MODEL_SSS` or `ML_FLAG_TRANSMISSION`),
  backlit lights receive non-zero WRS weight via the wrapped transmission lobe, back-facing shadow rays offset
  inward along `-world_normal` so rays measure local occluder thickness without front-face self-intersection, and
  `ml_shade_samples.glsl` / `ml_reference.glsl` accumulate `exp(-thickness * extinction)` transmitted radiance from
  `hit_distance_texture` directly into diffuse output, with zero bias on non-SSS pixels.
- **Zero-allocation OFF invariant:** When `rendering/megalights/enabled = false`, opaque, froxel, D17 direct-channel,
  and front-layer MegaLights allocations are all strictly `0` bytes (`memory_bytes == 0`) and rendered frames remain
  byte-identical to baseline.

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
