# AVBOIT source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records the outside
inputs used for AVBOIT, Adaptive Voxel-Based Order Independent Transparency, on Forward+ (#295, plan
§21.2.1 and §26.1): a transparency method, off by default and EXPERIMENTAL, that blends the alpha list
the same way whatever order it is drawn in.

## No Unreal Engine input

The setting, the fallback rules, the bounds and occlusion pre-passes, the adaptive slices, the
composite's weighting, the resolve and the checks were designed for GodotNext from Epic's public
description of what AVBOIT does, quoted in the issue and the plan, and from Godot's own transparent pass.
No Unreal Engine source, shader, plugin, NvRTX sample or decompile was opened.

## Epic: the AVBOIT page

| Field | Value |
|---|---|
| What | Unreal Engine 5.9 documentation, "Adaptive Voxel-Based Order Independent Transparency in Unreal Engine": what AVBOIT does (a camera-aligned grid of cells, translucency drawn once at low resolution to record how much light it blocks per cell, depth slices placed where translucency is, each pixel weighted by the light that gets through everything in front of it at its depth, a final pass that combines the weighted surfaces over the scene), its requirements (an SM6 desktop platform; not on mobile or SM5; off when the view uses MSAA; no raster ordered views), its Experimental banner, its console variables and its listed limitations. |
| Upstream | https://dev.epicgames.com/documentation/en-us/unreal-engine/adaptive-voxelbased-order-independent-transparency-in-unreal-engine?application_version=5.9 |
| Version | Revision 5004929 (2026-11-09) as two readers recorded it on 2026-11-16 (plan §26.1 [AV]); re-read on 2026-11-18, when the page showed no revision |
| License | Epic's public documentation, read only. Consulted, not copied |
| Read on | 2026-11-16 (two readers, for #287) and 2026-11-18 (the #295 implementer) |
| Consulted by | The #287 readers and the #295 implementer |
| Used in | `servers/rendering/avboit_policy.h` (the floor and the fallback reasons), `servers/rendering/renderer_rd/effects/avboit.h` (the passes) |
| Copied files | None |

How it was used: the issue's summary of the page, which both readers confirmed, set what to build: a
low-resolution pre-pass into a camera-aligned grid with adaptive depth slices, a full-resolution
composite weighted by the transmittance in front of each pixel, the MSAA cut-off and sorted translucency
on mobile. The way GodotNext does each step is its own: the bounds pass that keeps each cell's nearest
and farthest translucent depth with min and max blending, sixteen slices spaced logarithmically between
them in four RGBA16F layers filled by additive blending, a composite that classifies each fragment by its
surface's depth at the cell's center (its window depth extrapolated there along its screen derivatives)
and takes the surface's own optical depth off its own slice only, so a sloped surface does not hide
itself, an accumulation target whose alpha holds the coverage at full resolution, and a compute resolve.
The center-depth classification came from the 2026-11-18 code review of the first build (close layers
seen at an angle were weighed by their depth at the pixel), not from an outside input. The 2026-11-18
re-read came after the first build and changed no code. It also showed points the 2026-11-16 readers
had not recorded, read by one reader only, so they are not asserted in the plan: colored transmittance
(`r.AVBOIT.RGB`, default on), a cell size setting (`r.AVBOIT.PixelSize`), a maximum grid distance
(`r.AVBOIT.MaxDistance`, 200 m), an early out behind fully opaque translucency (`r.AVBOIT.EarlyOut`),
AVBOIT turning off in view modes that replace materials with debug shading, and the limitations that
refraction and distortion are not handled, surfaces in the same depth slice blend as a weighted average,
and colors step at the size of a grid cell where layers overlap. GodotNext's build shares the last three limitations in its own form; its own
limitations are listed in `servers/rendering/renderer_rd/effects/avboit.h`.

## Epic: the UE 5.9 Preview release notes

| Field | Value |
|---|---|
| What | The Rendering and Lighting entry "Adaptive Voxel-Based Order Independent Transparency": "AVBOIT blends overlapping translucent surfaces, such as glass, smoke and particles, correctly no matter what order they're drawn in." |
| Upstream | https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes?application_version=5.9 |
| Version | Revision 5005205 (2026-11-10) as recorded on 2026-11-16 (plan §26.1 [N59]); re-read on 2026-11-18 |
| License | Epic's public documentation, read only. Consulted, not copied |
| Read on | 2026-11-16 and 2026-11-18 |
| Consulted by | The #287 readers and the #295 implementer |
| Used in | The fixture's goal: overlapping translucent surfaces give the same image in any draw order |
| Copied files | None |

## General graphics knowledge

Optical depth as -ln(1 - alpha), transmittance as exp(-optical depth), premultiplied "over"
compositing and additive accumulation are textbook graphics and were not looked up for this task.
