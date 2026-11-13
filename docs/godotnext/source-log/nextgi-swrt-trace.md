# NextGI software ray trace source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for the software trace of gather, reflection and
SDF shadow rays (#186).

## No outside input

The march, the screen-to-SDF order, the 1.5-voxel self offset, the 2 m detail
range, the froxel grid and the WRC handoff were written from the plan (§21.2 L5)
and the issue. No Unreal Engine source, shader, plugin, NvRTX sample or
decompile was opened.

## Public documentation (2026-11-12, #186 steps 4 and 6)

- Epic, Unreal Engine 5.9 console variable reference
  (https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference,
  read 2026-11-12): `r.Lumen.HardwareRayTracing.PullbackBias` default 8.0,
  "the pull-back bias when resuming screen-traced rays". The plan's §21.2.6 row
  cites the 5.8 console-variable page for the same variable. Unreal's default
  unit is a centimeter, so the start value of the bias is 0.08 m. The Lumen
  technical-details page
  (https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.8)
  says screen traces hand over "if no hit is found, or the ray passes behind a
  surface"; the march here stops at the first sample that is off screen or
  behind the depth buffer (beyond the hit thickness). No shader or source was
  opened. The rest is this project's own: the bias is taken back from the last
  sample before the stop, which the march saw in front of the depth buffer;
  each sample is the point of the ray that projects to the sampled pixel
  (perspective division, textbook); on the software tier the bias grows to one
  voxel of the SDF level at that sample, a resume point within that voxel of
  the origin starts at the origin, a resumed ray has no start-of-ray escape
  rule, and Detail mode's 2 m range still ends 2 m from the surface the ray
  left.
- Epic, Mesh Distance Fields in Unreal Engine 5.8
  (https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine?application_version=5.8,
  read 2026-11-12): two-sided distance fields for foliage, a translucent shell
  rather than a solid. Used only to confirm the plan's §21.2.6 row that a
  two-sided shell is traced from either side. The card's back face, which
  relocates a world-radiance probe, follows the hardware tier's back-face hit
  on `cull_disabled` triangles. The card rule is this project's: a two-sided
  mesh whose own bounds are thinner than two of its finest SDF voxels along one
  local axis is a flat card with that axis as its normal, resolved from the
  instance that was hit (the detail march's instance, else the two-sided
  instance whose mesh SDF is nearest the hit); any other two-sided mesh keeps
  the SDF gradient normal and has no back face. No shader or source was opened.

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
