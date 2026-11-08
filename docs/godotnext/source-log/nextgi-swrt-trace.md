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
