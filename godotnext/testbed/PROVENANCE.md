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
