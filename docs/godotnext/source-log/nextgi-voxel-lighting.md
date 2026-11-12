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

How it was used: each 11-bit channel is 6 mantissa bits plus a 5-bit exponent, and the 10-bit channel is 5 mantissa bits plus a 5-bit exponent. The exponent bias is 15. There is no sign bit and no NaN or infinity encoding. The pack clamps when the stored exponent is greater than 30, which is the largest finite value (the 11-bit channel's maximum is 65024). A stored exponent of 30 itself is 32768 and is encoded. Denormals (stored exponent at or below 0) are scaled by 2^(1−15−mantissa bits), rounded half up, and clamped to the maximum mantissa instead of being promoted. A rounding carry that pushes the stored exponent above 30 clamps the same way. The CPU pack and the GLSL pack in the include take the exponent and mantissa from the IEEE bits and follow those rules. The doctest checks the edge values on the CPU pack, which is a line-for-line twin of the GLSL pack; the GPU pack is exercised by `godotnext/testbed/tests/voxel_lighting/voxel_slot_check.gd`, which reads lookups of the shaded volume back.

### Lumen surface-cache lighting updates

| Field | Value |
|---|---|
| What | Surface-cache lighting is amortized over frames, and those updates are prioritized by feedback |
| Upstream | https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.8 and https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference?application_version=5.8 |
| Version | UE 5.8 documentation. The technical-details page was updated 2026-11-11. The console-variable page was updated 2025-03-24. Both were read on 2026-11-12 |
| License | Epic Games documentation. Consulted, not copied |
| Read on | 2026-11-12, for issue #188 |
| Consulted by | The #188 implementer |
| Used in | `servers/rendering/renderer_rd/environment/next_gi.cpp`, `servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.cpp` |
| Copied files | None |

How it was used: the technical-details page says Lumen calculates direct and indirect lighting for surface-cache positions and that these updates are amortized over multiple frames. It does not name a pass order. The software tier therefore shades voxel lighting in the same frame-block slot as the hardware tier, after that frame's surface-cache relight, and keeps the 1536-ray budget. The console-variable reference describes `r.LumenScene.Lighting.Feedback` (default 1) as whether surface-cache lighting updates are prioritized by feedback. The 5.8 English table did not render as text; the same row is on the 5.9 console-variable reference. A new or refilled volume's fill bricks are traced nearest the camera first, then outward, which is this project's priority for what the view sees. Dirty bricks stay ahead of fill bricks. No feedback buffer was added.

## Notes without an outside input

### Direction slots across thin walls

The direction-slot rule for a voxel center inside a solid is the project's own; the #188 thin-wall fixer consulted no outside source for it. Such a voxel keeps, on each axis, only the face its center is nearer to, and the slot the far face would fill stays empty, because lookups blend voxels from both sides of a wall.

The limit, with one slot per direction and no side test in the lookup, for a flat wall facing an axis: no voxel light crosses a wall at least two lighting voxels thick (0.4 m at level 0, doubling per level). For a wall T thick and a lighting voxel V, a surface that faces the wall from the other room, g from the wall's face, can still read the near room's wall face with a trilinear weight of at most max(0, 1 - (T/2 + g)/V). The wall's own faces read only their own room when the wall is at least one lighting voxel thick (0.2 m at level 0). Walls thinner than one lighting voxel can also pass light through voxels whose centers lie outside them. The doctest "keeps the far room out of voxels inside a thin wall" asserts this limit.
