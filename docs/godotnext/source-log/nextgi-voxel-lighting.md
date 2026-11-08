# NextGI voxel lighting source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for voxel lighting of global-SDF hits.

## Approved inputs

### Khronos EXT_packed_float, R11F_G11F_B10F layout

| Field | Value |
|---|---|
| What | Bit layout of the unsigned 11/11/10 packed float used for one direction's radiance |
| Upstream | https://registry.khronos.org/OpenGL/extensions/EXT/EXT_packed_float.txt |
| Version | The registry text consulted on 2026-11-08. The channel layout is the unsigned 11-bit and 10-bit floats in that extension |
| License | Copyright Khronos Group. Consulted, not copied |
| Read on | 2026-11-08, for issue #188 |
| Consulted by | The #188 implementer |
| Used in | `servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.cpp`, `servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h`, `servers/rendering/renderer_rd/shaders/environment/nextgi_voxel_lighting_inc.glsl` |
| Copied files | None |

How it was used: each 11-bit channel is 6 mantissa bits plus a 5-bit exponent, and the 10-bit channel is 5 mantissa bits plus a 5-bit exponent. The exponent bias is 15. There is no sign bit and no NaN or infinity encoding. The pack clamps when the stored exponent is greater than 30, which is the largest finite value (the 11-bit channel's maximum is 65024). A stored exponent of 30 itself is 32768 and is encoded. Denormals (stored exponent at or below 0) are scaled by 2^(1−15−mantissa bits), rounded half up, and clamped to the maximum mantissa instead of being promoted. A rounding carry that pushes the stored exponent above 30 clamps the same way. The CPU pack and the GLSL pack in the include take the exponent and mantissa from the IEEE bits and follow those rules. The doctest calls the production pack and checks the same edge values.
