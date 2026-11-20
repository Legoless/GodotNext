# TSR source log

The plan's clean-room protocol (§19) and issue #194 (step 0) require a source log
recording every external document and existing in-tree reference consulted while
designing and implementing GodotNext's Temporal Super Resolution (`RendererRD::TSR`).

## Approved inputs

### Epic Games public Unreal Engine documentation (high-level feature descriptions only)

| Field | Value |
|---|---|
| Upstream 1 | https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine |
| Upstream 2 | https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-thin-geometry-detection-in-unreal-engine |
| Upstream 3 | https://dev.epicgames.com/documentation/en-us/unreal-engine/anti-aliasing-and-upscaling-in-unreal-engine |
| Upstream 4 | https://dev.epicgames.com/documentation/en-us/unreal-engine/dynamic-resolution-in-unreal-engine |
| Type / License | Publicly accessible online user documentation (Epic Games). Read for user-facing behavior, scalability preset feature splits, and public console-variable descriptions only; zero code or constants copied. |
| Read on | 2026-11-06 (#197) and 2026-11-08 (#194) |
| Used in | `servers/rendering/renderer_rd/effects/tsr_presets.h`, `servers/rendering/renderer_rd/effects/tsr.h`, `servers/rendering/renderer_rd/effects/tsr.cpp`, `servers/rendering/renderer_rd/shaders/effects/tsr/*.glsl` |

What was used from the public documentation:

- User-facing scalability preset split across `Low`, `Medium`, `High`, `Epic`, and
  `Cinematic` (`100%` output-resolution history on `Low`/`Medium`/`High`, `200%`
  history on `Epic`/`Cinematic`, spatial anti-aliasing of rejected pixels off on `Low`
  and on from `Medium` upward, and the stability features deferred to #198).
- High-level pipeline concepts described in prose: closest-depth velocity dilation,
  parallax disocclusion via previous-frame depth reprojection, luma/chroma neighborhood
  shading rejection augmented by a translucency reactive mask, subpixel jitter-aware
  history accumulation at `100%` or `200%` of output resolution, spatial edge filtering
  on rejected pixels, and final resolve to output resolution.
- Input resolution scaling range `[0.25, 2.0]` (upscaling through 2x supersampling) and
  temporal jitter phase scaling `round(8 * (output / internal)^2)` with a minimum of `8`
  phases.

### Epic Games public documentation for the stability set (#198)

| Field | Value |
|---|---|
| Upstream | Upstream 1 above, sections "History Resurrection", "Reprojection Field", "TSR Flickering Temporal Analysis" (with "Effects of Frame Rate") and "Visualize TSR" |
| Upstream 5 | https://dev.epicgames.com/documentation/unreal-engine/thin-geometry-detection-with-temporal-super-resolution ("Thin Geometry Detection with Temporal Super Resolution"). On 2026-11-15 the Upstream 2 URL above returned the documentation landing page; this is the page it pointed to. |
| Type / License | As above: feature-level descriptions only; zero code or constants copied. |
| Read on | 2026-11-15 (#198 design) |
| Used in | `servers/rendering/renderer_rd/effects/tsr_stability.h`, `servers/rendering/renderer_rd/effects/tsr.h`, `servers/rendering/renderer_rd/effects/tsr.cpp`, `servers/rendering/renderer_rd/shaders/effects/tsr/tsr_stability_inc.glsl`, `servers/rendering/renderer_rd/shaders/effects/tsr/tsr_persistent_store.glsl`, `servers/rendering/renderer_rd/shaders/effects/tsr/tsr_history_update.glsl`, `servers/rendering/renderer_rd/shaders/effects/tsr/tsr_stability_analysis.glsl`, `servers/rendering/renderer_rd/shaders/effects/tsr/tsr_shading_rejection.glsl`, `servers/rendering/renderer_rd/shaders/effects/tsr/tsr_debug_view.glsl` |

What was used (history resurrection):

- Older persistent frames are kept in a texture array, like the previous-frame history.
- A persistent frame is used in place of the previous frame where it matches the current
  frame better.
- Frames are recorded at an interval, and the oldest one is replaced.

What was not used: every default count, interval and threshold the page lists for its
console variables. GodotNext's values are its own and provisional until the PQ sweep (§22):

- The persistent-frame count (2) and size (1/3 of the output per axis at 100% history,
  1/2 at 200% history) come from the T6 budget. Two is the smallest count that always
  holds one frame at least one capture interval old.
- The capture interval (35 frames) is above two history convergence times (2 x 16 frames)
  and shares no factor with the jitter phase counts at the T-line scales (8, 18 and 32).
- The match test reuses GodotNext's own shading-rejection box measure
  (`tsr_shading_rejection.glsl`).
- Translucency keeps its rejection: no resurrection where the reactive mask is above 0.25,
  and the rejection never drops below the reactive mask, as for flicker and thin geometry.

The derived count of 2 happens to equal the default count the page lists. The derivation
does not depend on it; the interval, the frame size and every other value differ.

What was used (reprojection field):

- Each rendered pixel carries a 2x2 Jacobian, so the displayed pixels inside one rendered
  pixel are reprojected more precisely and geometry edges stay sharp in motion.
- It costs extra dependent fetches at the start of the history update.

What was used (flicker analysis):

- Only the luminance is watched.
- Where the luminance oscillates faster than a set period, shading rejection is relaxed so
  the history can hold.
- The period threshold follows the frame rate.
- The analysis is off per pixel for moving objects and large parallax.
- Detection takes a few frames.

What was not used: every default period, frame-rate limit, weight and threshold the pages
list for their console variables. GodotNext's own choices, provisional until PQ (§22):

- No field texture: the Jacobian is derived in the history update from the dilated velocity
  that the core passes already store, because a stored field does not fit T6. The gradient
  is a one-sided difference (the smaller of the two), so a silhouette does not enter it,
  clamped to 0.5 internal pixel of velocity per internal pixel. Where the Jacobian minifies
  the previous history, the fetch blends toward a 4-tap box over the pixel's footprint.
- No luma ring: one byte per texel counts time in ticks of 1/120 s since the luma last
  reversed direction against the reprojected history, so the state is the same size at any
  frame rate. A reversal within 1/30 s (an oscillation of 15 Hz or more, the fastest a
  30 Hz stream can show) counts as flicker, so the same frequencies count at 30, 60 and
  120 Hz. The clock runs on the main loop's wall-clock frame step, not the one scaled by
  `Engine.time_scale`, so it keeps counting while game time is slowed or frozen. Scoring
  (+3 per qualifying reversal, -2 per slow one, 15 levels), release (60 levels per second
  after the window), the motion limit (15 internal pixels per second), the noise floor
  (1/64 of perceptual luma), the most relaxation (75% of the rejection) and the widest clip
  box (3 standard deviations) are GodotNext's own.
- Stated deviations: the analysis reads the TSR input colour and is switched off where the
  reactive (translucency) mask is set, instead of measuring opaque lighting before
  translucency in a pass of its own; there is no per-material animation flag (out of scope
  in #198).

What was used (thin geometry, Upstream 5):

- Thin-geometry detection estimates the sub-pixel coverage of thin features and relaxes the
  history rejection there, so thin lines stay in the history; it is off by default.
- The coverage is averaged over several frames.
- Depth edges and luma lines are detected separately.
- The relaxation is cut where translucency dominates.

What was not used: the rest of the page's description of its internal passes (its
shading-model classes, its diffusion and its line persistence), and every default value,
weight and threshold it lists for its console variables. GodotNext's own choices,
provisional until PQ (§22):

- One byte of coverage per texel at half the internal resolution, read through a bilinear
  tap, because it marks regions that hold thin features rather than single pixels.
- A pixel holds a thin feature when, along the horizontal, the vertical or a diagonal, both
  opposite neighbors are farther than it by more than 5% of its view depth (a depth ridge
  one pixel wide), or both differ from it in perceptual luma, with the same sign, by more
  than 0.08 (a luma line).
- The coverage moves toward each frame's detection by 1/8 along the reprojection, because 8
  is the smallest jitter phase count, so the average spans one jitter cycle; it restarts off
  the screen.
- The relax weight rises from a coverage of 1/32 to 1/8 (one detection in four jitter cycles
  to one per cycle) and removes at most 75% of the shading and the parallax-disocclusion
  rejection, as for flicker; the history clip box widens with it. Disocclusion relaxes too,
  because a wire hit for the first time is nearer than all four previous-depth taps.
- Translucency: no coverage where the reactive mask is above 0.25, and the reactive term of
  the rejection is never relaxed.
- The setting (`rendering/anti_aliasing/tsr/thin_geometry_detection`) works at every preset.
  Turning it on automatically with virtual-geometry foliage (lead question LQ11) is not built.

What was used (visualizer, "Visualize TSR"):

- The user-facing colour legend: green helps image quality, red or yellow costs it, and
  magenta means the feature is off or disabled for that pixel.

The six views (history validity, rejection, disocclusion, resurrection hits, flicker period,
thin coverage) are the ones issue #198 lists, each drawn from GodotNext's own textures over
the scene's luma at 25%. The resurrection-hits view recomputes the history update's own test
(`tsr_resurrection_match` in `tsr_stability_inc.glsl`).

### Shader and format portability across Vulkan, D3D12 and Metal (#195)

No Epic document was read for this work: UE's TSR page says only that TSR runs on every
desktop RHI. The inputs are API specifications and the open-source shader toolchains the
repository already builds with.

| Input | Upstream | Version / Read on | License | What was used |
|---|---|---|---|---|
| Khronos *GLSL.std.450* extended instruction set | https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html | Version 1.00, Revision 17; read 2026-11-16 | Khronos specification, public | `Round`: "The fraction 0.5 rounds in a direction chosen by the implementation". `RoundEven`: 0.5 rounds toward the nearest even whole number. |
| SPIRV-Cross (in tree, `thirdparty/spirv-cross/spirv_msl.cpp`, `spirv_glsl.cpp`) | The in-tree copy | Read 2026-11-16 | Apache-2.0 | The MSL backend writes `RoundEven` as `rint`; `Round` falls through to the GLSL backend's `round`. |
| Mesa SPIR-V to NIR (`src/compiler/spirv/vtn_glsl450.c`, the source of the private Mesa NIR library #123 builds for D3D12) | The #123 Mesa checkout | Mesa 25.3.1, the base of the #123 library 25.3.1-gn3; read 2026-11-16 | MIT | `Round` and `RoundEven` both become `fround_even`, which DXIL emits as `round_ne`. |
| Godot's RenderingDevice drivers (in tree, `drivers/d3d12`, `drivers/metal`, `drivers/vulkan`) | The in-tree code | Read 2026-11-16 | MIT | How each driver answers `texture_is_format_supported_for_usage` with the storage bit: D3D12 from `D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW`; Metal never clears the storage bit (its format table gives `RG11B10Float` every capability on macOS, and no write capability on iOS GPUs below the Apple3 family). |

Note on Metal Shading Language math semantics: Apple's *Metal Shading Language Specification*
was not consulted for this work. SPIRV-Cross's MSL backend emits `rint` for `RoundEven` and
`round` for `Round`, and this change assumes MSL's `round` and `rint` follow C99 (`round`
rounds halfway cases away from zero; `rint` rounds to nearest even).

What was used:

- The history update's sample position (`tsr_history_update.glsl`, mirrored in
  `tsr_debug_view.glsl`) rounds with `roundEven` instead of `round`. The x jitter is a base-2
  Halton value, so with a power-of-two ratio between the history and internal sizes (scale
  0.5 at 100% history, 1.0 at 200% history) the position can land on an exact `.5` tie.
  GLSL leaves `round`'s tie-breaking direction to the implementation, whereas `roundEven`
  emits SPIR-V `RoundEven`, DXIL `round_ne` and MSL `rint`, so exact `.5` ties round to even
  across all three backends (the floating-point evaluation of `x_in` itself is not guaranteed
  to be bit-identical across drivers or fast-math modes).
- TSR asks the device about every format of `TSR_FORMAT_TABLE` and
  `TSR_STABILITY_STORE_TABLE` with the usage it creates them with (sampling, storage and
  copy-to). R11G11B10 history colour falls back to RGBA16F, as #194 already did for that
  format alone; the extra 4 B per texel count against T6. On a device missing any other
  `RB_SCOPE_TSR` format, `RenderForwardClustered` leaves TSR off (`SCALE_NONE`, plain 3D
  scaling) and logs one warning when TSR is requested.

GodotNext's own: the selection order, the "first missing format" report, the
`GODOTNEXT_TSR_DENY_STORAGE_FORMATS` test switch and the doctest's mock capability table.
TSR uses no subgroup operations, and its largest thread-group array is 10 x 10 `vec3`
(1,600 B with Metal's 16-byte `float3`), far below Metal's 32 KB.

### Epic Games public documentation for TSR async compute (#193)

| Field | Value |
|---|---|
| Upstream | Upstream 1 above (https://dev.epicgames.com/documentation/unreal-engine/temporal-super-resolution-in-unreal-engine), its `r.TSR.AsyncCompute` console variable entry and its notes on async compute and the critical path |
| Type / License | As above: feature-level descriptions only; zero code or constants copied. |
| Read on | 2026-11-18 (#193) |
| Used in | `servers/rendering/renderer_rd/effects/tsr_async.h`, `servers/rendering/renderer_rd/effects/tsr.{h,cpp}`, `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`, `doc/classes/ProjectSettings.xml` |

What was used:

- The four levels and the default: 0 off; 1 runs on async compute only the passes that are
  completely independent from any intermediary resource of the frame; 2 (the default) also
  runs the passes that depend only on the depth and velocity buffers, which can overlap other
  work such as translucency and depth of field, while the passes on the critical path stay on
  the graphics queue; 3 runs every pass on async compute.
- The idea that the parallax work depends only on depth and velocity, which are ready early in
  the frame, so it can run asynchronously; and the split of TSR's cost into total GPU time and
  critical-path time, which the `"TSR critical path"` timestamp mirrors (beside `"TSR depth and
  velocity"` in a split frame; `"TSR"` always covers all of TSR).

What was not used: the page's own pass names (its level-1 examples) and its cost figures. Which
GodotNext pass belongs to which level follows from what each pass reads (`tsr_async.h`):
velocity dilation and parallax disocclusion read only the depth, the velocity and TSR's own
textures; every other pass reads the scene color or the history built from it; no pass reads
only earlier frames, so level 1 moves nothing. The record point after the motion-vector pass,
the conditions that keep level 2 on the main queue (MSAA, transparent depth writes, compositor
effects, VoxelGI and SDFGI debug draws), and level 3 recording at the upscaler's place are
GodotNext's own, from the render graph's one fork-join window per frame (#174,
`source-log/rhi-async-compute.md`), as are the rules that only the first TSR viewport of a frame
uses the async queue and that multiview (XR) viewports do not, and the timestamp names. With read-only depth attachments (#301), the sky pass, read-only depth post-opaque work (SSS, specular merge, depth copy), and the transparent pass (when no `depth_draw_always` or stencil-writing surface draws) share the depth buffer's read-only layout with the async depth and velocity passes, so the fork-join window spans them and closes at TSR's shading rejection pass.

### Standard academic / public graphics literature (general mathematical techniques)

| Reference | Date / Venue | License / Status | What was used |
|---|---|---|---|
| Marco Salvi, *An Excursion in Temporal Supersampling*, GDC 2016 (`https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf`, already cited in `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`) | 2016 | Public conference presentation | First- and second-moment (`mean` / `stddev`) neighborhood variance clipping in YCoCg space. |
| Brian Karis, *High Quality Temporal Supersampling*, SIGGRAPH 2014 Advances in Real-Time Rendering | 2014 | Public conference course notes | Perceptual HDR luminance weighting `1 / (1 + luma)` and inverse weighting `1 / (1 - luma)` during neighborhood statistics, history accumulation, and 2x2 resolve downsampling; 3x3 closest-depth velocity dilation; 5-tap Catmull-Rom bicubic history sampling. |

### Existing Godot Engine / GodotNext codebase (in-tree MIT code)

| Path | License | What was used |
|---|---|---|
| `servers/rendering/renderer_rd/effects/taa.{h,cpp}`, `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl` | MIT | `RenderSceneBuffersRD` slice access, reverse-Z depth convention, UV-space velocity convention (`uv_prev = uv_curr + velocity`), and compute dispatch patterns. |
| `servers/rendering/renderer_rd/effects/fsr2.{h,cpp}` | MIT | `RenderForwardClustered` pre-tonemap temporal upscaler slot, `rb->get_internal_texture_reactive(v)` alpha-swizzled translucency mask binding, and camera `reprojection` matrix construction. |
| `servers/rendering/renderer_rd/effects/temporal_upscaler.h` | MIT | `TemporalHistoryReset` one-shot semantics, `nextgi_temporal_jitter_phase_count`, and `nextgi_temporal_mip_bias`. |
| `servers/rendering/renderer_rd/effects/motion_vectors_store.{h,cpp}`, `servers/rendering/renderer_rd/forward_clustered/nextgi_velocity.h` | MIT | `NEXTGI_VELOCITY_DENSE` pre-clear `(-1, -1)` and `MotionVectorsStore` dense camera/sky velocity population. |

## GodotNext clean-room design notes

All shaders (`tsr_inc.glsl`, `tsr_velocity_dilate.glsl`, `tsr_disocclusion.glsl`,
`tsr_shading_rejection.glsl`, `tsr_history_update.glsl`, `tsr_rejection_aa.glsl`,
`tsr_resolve.glsl`) and C++ host code (`tsr.h`, `tsr.cpp`) were authored from scratch
for GodotNext's `RenderingDevice` compute pipeline:

- History color ping-pong textures (`history_color_0`, `history_color_1`) use
  `DATA_FORMAT_B10G11R11_UFLOAT_PACK32` (4 B/px, with `RGBA16F` fallback when storage
  usage on `R11G11B10F` is unsupported by the device) paired with `R8_UNORM` metadata
  (`history_meta_0`, `history_meta_1`, 1 B/px), plus internal-resolution `dilated_velocity`
  (`RG16F`, 4 B/px), `prev_depth` (`R16F`, 2 B/px), `disocclusion` (`R8_UNORM`, 1 B/px),
  and `rejection` (`R8_UNORM`, 1 B/px) in `RB_SCOPE_TSR` — keeping history-rate storage
  at 10 B/history pixel and total TSR memory within T6 (`<= 14 B` per history pixel).
- When `rejection_aa` is enabled (`Medium` and above), Pass 5 reads the newly updated
  `history_color[write_idx]` and writes spatially edge-filtered output for low-confidence
  rejected pixels into the retired `history_color[read_idx]` buffer for Pass 6 (`Resolve`),
  avoiding both an extra history-sized scratch allocation and recursive blur in the
  persistent temporal history.

## Forbidden inputs

No Unreal Engine source code, `.usf`/`.ush` shaders, shader constants, Unreal Engine
plugin source (including NVIDIA's UE DLSS plugin), NvRTX, DLSS SDK sample or Donut
source, proprietary SDK headers, or third-party reverse-engineering write-ups reproducing
Unreal Engine's TSR source or constants were read, consulted, or used.
