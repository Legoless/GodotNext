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
