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
