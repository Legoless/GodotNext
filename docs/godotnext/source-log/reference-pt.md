# Reference path tracer source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used to write the P0 reference path tracer v0. The BRDF,
the non-physical light units and the omni falloff are ports of code already in this
repository, not outside sources.

## Approved inputs

### PCG hash, Jarzynski and Olano, JCGT 2020

| Field | Value |
|---|---|
| What | The PCG-RXS-M-XS integer hash used as the path tracer's RNG |
| Upstream | https://jcgt.org/published/0009/03/02/ |
| Version | Journal of Computer Graphics Techniques, vol. 9, no. 3, 2020 |
| License | Open-access paper. The listing is the authors' published hash; no source file was copied into the tree. |
| Read on | 2026-11-03, for issue #19 |
| Consulted by | The #19 implementer |
| Used in | `godotnext/testbed/tests/reference/ref_pt.glsl`, `godotnext/testbed/tests/reference/furnace_cpu.gd` |
| Copied files | None |

How it was used: each random number is the published hash of the pixel or patch index, the sample, the bounce, the dimension and the seed. The 24 high bits become a float in [0, 1). The same function is in the GPU kernel and the CPU furnace mirror.

### Sampling the GGX distribution of visible normals, Heitz, JCGT 2018

| Field | Value |
|---|---|
| What | Visible-normal sampling of the GGX distribution, then reflection into the outgoing direction |
| Upstream | https://jcgt.org/published/0007/04/01/ |
| Version | Journal of Computer Graphics Techniques, vol. 7, no. 4, 2018 |
| License | Open-access paper. No source file was copied into the tree. |
| Read on | 2026-11-03, for issue #19 |
| Consulted by | The #19 implementer |
| Used in | `godotnext/testbed/tests/reference/ref_pt.glsl` |
| Copied files | None |

How it was used: when a material has a non-zero specular weight, the kernel samples a visible GGX normal in the shading frame and reflects the view direction. The analytic fixtures use specular 0, so they take the cosine lobe only. The PDF is the visible-normal density converted by the reflection Jacobian.

### Path Tracer in Unreal Engine, Epic Games documentation

| Field | Value |
|---|---|
| What | The settings and behavior of UE's product path tracer: Samples Per Pixel, Max Bounces, Max Path Intensity, when samples are invalidated, emissive materials as lights and the Nanite fallback mesh |
| Upstream | https://dev.epicgames.com/documentation/en-us/unreal-engine/path-tracer-in-unreal-engine |
| Version | The page as published on 2026-11-18 |
| License | Public documentation. No source file was copied into the tree. |
| Read on | 2026-11-18, for issue #167 |
| Consulted by | The #167 implementer |
| Used in | `servers/rendering/renderer_rd/effects/path_tracer.{h,cpp}`, `servers/rendering/renderer_rd/shaders/environment/path_tracer_inc.glsl`, `scene/main/viewport.cpp` |
| Copied files | None |

How it was used: `Viewport.path_tracing_samples` and `path_tracing_max_bounces` follow the meaning of Samples Per Pixel and Max Bounces (the defaults, 2048 and 32, are the issue's). `path_tracing_firefly_clamp` follows Max Path Intensity: the largest exposed radiance one path sample adds; 0 turns it off here. The history restarts on the changes the page lists (camera moves, material changes, objects moved or added) and on light and setting changes. Emissive surfaces light the scene through hit emission. NextGeo meshes are traced through their RT fallback by default, as the page states for Nanite meshes, and through the native sets in the Experimental mode. The page does not cover backface culling; the cull rule follows plan v3.4.3 (one-sided materials are culled, two-sided ones are not).
