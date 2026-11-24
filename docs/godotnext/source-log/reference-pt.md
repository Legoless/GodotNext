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

### Intel OpenImageDenoise C API documentation

| Field | Value |
|---|---|
| What | Public Intel OpenImageDenoise C API function and filter parameter names (`oidnNewDevice`, `oidnNewFilter("RT")`, `oidnNewBuffer`, `oidnSetFilterImage`, `hdr`/`srgb`/`cleanAux`) |
| Upstream | https://www.openimagedenoise.org/documentation.html |
| Version | OpenImageDenoise 2.x (with 1.x `oidnSetSharedFilterImage` fallback) |
| License | Apache-2.0 upstream library; no OIDN headers, source or binaries are committed to this repository (§19). |
| Read on | 2026-11-23, for issue #168 |
| Consulted by | The #168 implementer |
| Used in | `modules/oidn/oidn_denoiser_real.cpp` |
| Copied files | None |

How it was used: `modules/oidn` is disabled by default and compiles `oidn_denoiser_real.cpp` only when a developer enables `module_oidn_enabled=yes oidn_backend=oidn oidn_sdk_path=<external SDK>`. Official builds and CI use either the default build (OIDN disabled) or `oidn_backend=mock`. The built-in screen-space JNLM denoiser (`servers/rendering/renderer_rd/shaders/effects/path_tracer_denoise.glsl`) is adapted from Godot's existing `modules/lightmapper_rd/lm_compute.glsl` (`MODE_DENOISE`, MIT).

### Multiple Importance Sampling (Veach 1997), Concentric Disk Mapping (Shirley & Chiu 1997), Delta/Ratio Tracking (Woodcock et al. 1965; Novák et al. 2014), and Henyey-Greenstein Phase Function (1941)

| Field | Value |
|---|---|
| What | Standard textbook Monte Carlo rendering formulas: balance-heuristic MIS, concentric square-to-disk thin-lens mapping, octahedral solid-angle Jacobian (`4 * (|x|+|y|+|z|)^3`) for 2D marginal/conditional sky CDF importance sampling, Woodcock delta tracking and ratio tracking for heterogeneous participating media, and Henyey-Greenstein phase function sampling |
| Upstream | Standard public graphics literature (Veach PhD thesis 1997; Shirley & Chiu, Journal of Graphics Tools 1997; Novák, Selle & Jarosz, ACM TOG / SIGGRAPH Asia 2014; Pharr, Jakob & Humphreys, *Physically Based Rendering*) |
| Version | Published mathematical formulations; no source file was copied into the tree |
| License | Standard published algorithms. No third-party code copied. |
| Read on | 2026-11-24, for issue #169 |
| Consulted by | The #169 implementer |
| Used in | `servers/rendering/renderer_rd/effects/path_tracer.{h,cpp}`, `servers/rendering/renderer_rd/shaders/environment/path_tracer_inc.glsl`, `servers/rendering/renderer_rd/shaders/effects/path_tracer_sky_cdf.glsl`, `tests/servers/rendering/test_path_tracer.cpp` |
| Copied files | None |

How it was used: Light evaluation (directional angular cone, omni/spot sphere cap, projectors, AreaLight3D barn-door clipped quads + area textures, and #140 IES profiles), sky octmap UV-border lookups, FogVolume analytic SDFs, and `CameraAttributesPractical` DOF scaling match Godot's existing Forward+ shaders in this repository. Balance-heuristic MIS combines NEE light and 2D sky-CDF sampling with BSDF sampling. Unbiased Woodcock delta tracking and ratio tracking handle homogeneous volumetric fog and `FogVolume` media with Henyey-Greenstein phase sampling. Per-instance motion blur interpolates `RenderGeometryInstance` previous/current transforms across an 8-slice masked TLAS over the camera shutter interval.
