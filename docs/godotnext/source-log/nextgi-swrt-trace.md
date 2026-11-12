# NextGI software ray trace source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for the software trace of gather, reflection and
SDF shadow rays (#186).

## No outside input

The march, the screen-to-SDF order, the 1.5-voxel self offset, the two-sided
back-face rule, the 2 m detail range, the froxel grid and the WRC handoff were
written from the plan (§21.2 L5) and the issue. No Unreal Engine source, shader,
plugin, NvRTX sample or decompile was opened.

Global hits shade through `nextgi_voxel_lighting_inc.glsl`. That include's
R11G11B10 layout is already logged in
`docs/godotnext/source-log/nextgi-voxel-lighting.md` (Khronos EXT_packed_float,
consulted, not copied). This change does not re-copy that layout.

## Copied files

None.

## Mesh-SDF shadow rays of the SRC relight (#186 step 5)

Epic's public console-variable reference (Unreal Engine 5.8 and 5.9 documentation):

- https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference?application_version=5.8

Consulted 2026-11-12 (the plan's §21.2.6 row cites it).
`r.LumenScene.DirectLighting.OffscreenShadowing.TraceMeshSDFs` defaults to 1: Lumen's
surface-cache direct lighting traces off-screen shadows against mesh signed distance
fields instead of the lower-resolution global SDF. How UE selects the meshes it traces is
not described there. The per-light candidate lists (the instances whose mesh-SDF bounds
overlap the light's range sphere), the per-ray cap of 8, the 192-step march, the
inside-the-solid occlusion rule and the global-SDF fallback are this change's own design
and start values. No Unreal Engine source, shader, plugin, NvRTX sample or decompile was
opened.
