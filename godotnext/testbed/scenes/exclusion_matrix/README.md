# Exclusion Matrix (#74)

Twenty-two labeled cells. Each case sits beside a non-virtualized twin.
`exclusion_rules.gd` is the route and reason contract. #79 fills
`RenderingServer.virtual_geometry_get_debug_stats`. Until that method
exists, `check_exclusion.gd` reports the authored scene and the report
tool records `counters_source: absent`.

```
godot --headless --path godotnext/testbed --script res://generators/build_exclusion_matrix.gd
godot --headless --path godotnext/testbed --script res://scenes/exclusion_matrix/check_exclusion.gd
```

The generator is seed stream 74 (`0x6E657874`). Do not edit
`exclusion_matrix.tscn`, `cells.json` or `moving_track.json` by hand.

## Routes and reasons

| Cell | Route | Reason |
|---|---|---|
| Opaque surface of the mixed mesh | virtual | `NONE` |
| Glass surface of the mixed mesh | legacy | `alpha` |
| `next_pass`, `material_overlay`, stencil reader | legacy, whole chain | `NEXT_PASS_OR_OVERLAY` |
| Vertex lighting | legacy | `VERTEX_LIGHTING` |
| Position-altering `vertex()` | legacy | `UNSUPPORTED_CUSTOM_SHADER` |
| Screen texture, alpha scissor, alpha hash | legacy | `alpha` |
| Non-varying `dFdx` | legacy | `UNSUPPORTED_DERIVATIVE` |
| Skinning | legacy | `SKINNING` |
| Instance uniforms | legacy | `INSTANCE_UNIFORMS` |
| MultiMesh and particles | legacy | `MULTIMESH_OR_PARTICLES` (counted warning) |
| Octant GridMap | legacy | `VGD_IGNORED` (counted warning; not a Fallback enum value) |
| Baked GridMap (`make_baked_meshes`) | legacy | `BAKED_GRIDMAP` |
| Heightfield capture | `auxiliary_proxy` | `COARSE_OR_RT_PROXY` (policy name, not a Fallback enum value) |
| Animated `TIME` | virtual | `NONE`, excluded from static SRC caching |
| Mutated mesh | virtual `NONE` before frame 61; legacy `MUTATED_MESH` from frame 61 | `surface_update_vertex_region` at frame 60 |
| MSAA viewport | legacy | `MSAA` |
| Multiview viewport (`use_xr`) | legacy | `MULTIVIEW` |
| Moving virtual instance | virtual | `NONE`, plus the G5-7 track |

`VGD_IGNORED` and `COARSE_OR_RT_PROXY` are the strings #79 should report.
They are not members of `NextGeoFallback`.

The screen-texture candidate and its twin both sample the screen. The
animated-`TIME` candidate and its twin both run the `TIME` shader.
Particles emit with `use_fixed_seed`, seed 1, `fixed_fps` 60,
`interpolate` off, `fract_delta` off, spread 0 and randomness 0, so
`--fixed-fps 60` repeats the same simulation. The
MSAA viewport (`MSAA_2X`, `own_world_3d`) updates every frame. The
multiview viewport sets `use_xr` and `UPDATE_ALWAYS`, and its camera is
an `XRCamera3D`. It does not rasterize on this machine: the renderer
skips a `use_xr` viewport until `XRServer` has a primary interface.
That limit is `Rules.MULTIVIEW_DRAW_LIMIT`. The stencil reader disables
depth draw so the forward renderer accepts the stencil read; the reason
stays `NEXT_PASS_OR_OVERLAY`. The mutated mesh edits itself only while
a timeline is live (no harness, or the harness is driving the capture
window), so settle frames do not bake the edit into the capture.
`CopyShaderRD` from the screen-texture copy is a known #239 pipeline.

## G5-7 track

`moving_track.json` stores frames 0..120. Frame 0's previous UV equals
its current UV. Later frames use the previous frame's analytic UV. The
projection is cell-local at 50° vertical FOV and a 16:9 aspect, from
`(0, 0.8, 2.2)` looking at `(0, 0.4, 0)`. The box center rests at
`y = 0.25` and moves on `sin(t * TAU / 4) * 0.35` meters, `t = frame / 60`.

DLSS is not a `Viewport.Scaling3DMode` in this build. The track reports
status `N/A`, fallback `FSR2`, scale `0.667`, and an empty DLL hash.
`G57Cam` is not the current camera. TAA, FSR2 and MetalFX are named in
the track; measured reprojection waits for a velocity sidecar and #95.
A build whose scaling enum contains `DLSS` reports status `ready`.

## Runtime sidecar

With an absolute `--out`, the scene writes `exclusion_observed.json` on
exit: schema 1, the frame, double-drawn and missing counts, the mutated
expectation, the DLSS record and `debug_stats` when the method exists.
