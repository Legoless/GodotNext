# Testbed provenance

Budget: tracked size ≤ 50 MB. Current total: under 1 MB (about 0.4 MB at
creation; measure with `git ls-files godotnext/testbed | xargs du -ch`
once tracked).

Every asset in this project is an original generated in this repository.
License for every row: MIT (same as the repository). Host for every row:
the Apple M5 Max. Date for every row: 2026-11-03.

All `.tscn` and `.json` rows are outputs of
`godot --headless --path godotnext/testbed --script res://generators/build_all.gd`,
seed `0x6E657874`. Do not edit them by hand; edit the generator and rerun.

The Dense City Blocks rows (#63) are the exception in size only: the
generator is tracked, but its output (about 2 GB across all variants,
including a 775 MB `.obj`) is generated on demand into the gitignored
`generated/dense_city/` folder and never tracked. The tracked additions
for #63 are the generator, its validator, `variants.cfg`, its README and
`expected.json` (counts, AABBs and the reference SHA-256 of every
output), all under `generators/dense_city/` and well under 1 MB in
total, so the 50 MB testbed budget is unaffected by the scene scale.
Regenerate with `res://generators/dense_city/dense_city_generator.gd`,
seed `1` (one run per variant; see its README).

| Path | Generator / author | License | Date | Host |
|---|---|---|---|---|
| `project.godot` | hand-authored for #10 step 1 | MIT | 2026-11-03 | the Apple M5 Max |
| `.gitignore` | hand-authored for #10 step 1 | MIT | 2026-11-03 | the Apple M5 Max |
| `PROVENANCE.md` | hand-authored for #10 step 2 | MIT | 2026-11-03 | the Apple M5 Max |
| `README.md` | hand-authored for #10 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/scaffold.gd` | hand-authored for #10 step 3 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_color_box_room.gd` | hand-authored for #10 step 5 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_single_40m.gd` | hand-authored for #10 step 5 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_courtyard.gd` | hand-authored for #10 step 6 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_legacy.gd` | hand-authored for #10 step 8 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_all.gd` | hand-authored for #10 step 3 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/dense_city/dense_city_generator.gd` | hand-authored for #63 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/dense_city/validate_dense_city.gd` | hand-authored for #63 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/dense_city/variants.cfg` | hand-authored for #63 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/dense_city/README.md` | hand-authored for #63 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/dense_city/expected.json` | `dense_city_generator.gd`, seed `1`, all variants | MIT | 2026-11-03 | the Apple M5 Max |
| `generated/dense_city/**` (untracked) | `dense_city_generator.gd`, seed `1`; hashes in `expected.json` | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_a.tscn` | `build_color_box_room.gd`, seed `0x6E657874` stream 1 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_a_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 1 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_a_patches_q3.json` | `build_color_box_room.gd`, seed `0x6E657874` stream 1 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_b.tscn` | `build_color_box_room.gd`, seed `0x6E657874` stream 2 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_b_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 2 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_b_patches_q3.json` | `build_color_box_room.gd`, seed `0x6E657874` stream 2 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_offscreen.tscn` | `build_color_box_room.gd`, seed `0x6E657874` stream 3 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_offscreen_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 3 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_offscreen_patches_q3.json` | `build_color_box_room.gd`, seed `0x6E657874` stream 3 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_b_lightmap.tscn` | `build_color_box_room.gd`, seed `0x6E657874` stream 4; **unbaked**, see below | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_b_lightmap_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 4 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_b_lightmap_patches_q3.json` | `build_color_box_room.gd`, seed `0x6E657874` stream 4 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_single_40m.tscn` | `build_single_40m.gd`, seed `0x6E657874` stream 5 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_single_40m_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 5 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/color_box_room/cbr_single_40m_patches_q3.json` | `build_single_40m.gd`, seed `0x6E657874` stream 5 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/concave_courtyard/courtyard.tscn` | `build_courtyard.gd`, seed `0x6E657874` stream 6 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/concave_courtyard/courtyard_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 6 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/legacy/legacy_sdfgi.tscn` | `build_legacy.gd`, seed `0x6E657874` stream 7 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/legacy/legacy_voxelgi.tscn` | `build_legacy.gd`, seed `0x6E657874` stream 8; VoxelGI unbaked, see below | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/legacy/legacy_lightmap.tscn` | `build_legacy.gd`, seed `0x6E657874` stream 9; LightmapGI unbaked, see below | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/scene_args.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/dynamic_content_check.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/moving_light_door/motion.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/moving_light_door/moving_light_door.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_moving_light_door.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/moving_light_door/moving_light_door.tscn` | `build_moving_light_door.gd`, seed `0x6E657874` stream 33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/moving_light_door/moving_light_door_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/moving_light_door/moving_light_door_patches_q3.json` | `build_moving_light_door.gd`, seed `0x6E657874` stream 33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/moving_light_door/steps.json` | `build_moving_light_door.gd`, seed `0x6E657874` stream 33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/time_of_day/motion.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/time_of_day/time_of_day.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_time_of_day.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/time_of_day/time_of_day.tscn` | `build_time_of_day.gd`, seed `0x6E657874` stream 34 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/time_of_day/time_of_day_patches_q1.json` | `build_all.gd`, seed `0x6E657874` stream 34 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/time_of_day/steps.json` | `build_time_of_day.gd`, seed `0x6E657874` stream 34 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/nextgi_perf/layout.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/nextgi_perf/nextgi_perf.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `generators/build_nextgi_perf.gd` | hand-authored for #33 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/nextgi_perf/nextgi_perf.tscn` | `build_nextgi_perf.gd` shell only; instances are spawned at runtime from seed 1234 and are not stored | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/nextgi_perf/nextgi_perf_patches_q1.json` | `build_all.gd`, Q1 RNG seed 1234 | MIT | 2026-11-03 | the Apple M5 Max |
| `scenes/nextgi_perf/steps.json` | `build_nextgi_perf.gd`, seed 1234 | MIT | 2026-11-03 | the Apple M5 Max |

## Bake status

`cbr_b_lightmap.tscn` carries the recorded bake settings (`bounces=16`,
`quality=BAKE_QUALITY_ULTRA`, `use_denoiser=false`,
`use_texture_for_bounces=false`, light bake mode Dynamic, which is the
`Light3D` default and is therefore not serialized) but **no baked data**:
`LightmapGI::bake()` is C++-only and reachable only from the editor's
"Bake Lightmaps" button, so there is no reliable unattended bake. Bake it
in the editor before the report-only cross-check in #27, and record the
host here when done. The same applies to the `VoxelGI` in
`legacy_voxelgi.tscn` and the `LightmapGI` in `legacy_lightmap.tscn`.
