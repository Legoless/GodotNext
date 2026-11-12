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

### Lumen technical details, software ray tracing wall thickness

| Field | Value |
|---|---|
| What | The software-tracing guidance that walls of 10 cm or more should not leak |
| Upstream | https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine |
| Version | Unreal Engine 5.9 documentation page, fetched 2026-11-12. The same sentence is the one plan §21.2.6 cites from the 5.8 application_version URL of this page |
| License | Epic Games documentation. Consulted, not copied |
| Read on | 2026-11-12, for issue #188 |
| Consulted by | The #188 side-test implementer |
| Used in | `servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h`, `servers/rendering/renderer_rd/shaders/environment/nextgi_voxel_lighting_inc.glsl` |
| Copied files | None |

How it was used: the page's software ray tracing limitations say "Walls should be no thinner than 10 centimeters (cm) to avoid light leaking." That sentence is the target for the voxel-lookup side test. The stored offset, the quantization and the accept rule are the project's own. No Unreal source, shader or plugin was opened.

## Notes without an outside input

### Direction slots across thin walls

The direction-slot rule for a voxel center inside a solid is the project's own; the #188 thin-wall fixer consulted no outside source for it. Such a voxel keeps, on each axis, only the face its center is nearer to, and the slot the far face would fill stays empty, because lookups blend voxels from both sides of a wall.

The lookup then applies a side test. Each slot stores a signed offset of its hit along the slot axis, in units of that level's voxel, clamped to [-1, 1] and rounded toward -1. The offset occupies the top 4 bits of the radiance word. The color uses the low 28 bits (red and green with 5 mantissa bits, blue with 3), so the volume stays 24 MiB of radiance, 6 MiB of weights and 2 MiB of staging, 32 MiB in all. A sample is accepted when it lies on the front of that axis plane or on the plane. A sample behind the plane contributes nothing, and the radiance weight is the blend of the accepted slots. A flat wall facing an axis then keeps its lit face out of the room on the far side down to a 10 cm wall at level 0. The doctest "side test keeps 10 cm and 20 cm walls from leaking" asserts that, including the share the same geometry still has when the test is not applied.
