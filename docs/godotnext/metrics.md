# §13.4 metric definitions

Frozen at G0: commit <sha>, date <YYYY-MM-DD>

The lead fills in the commit and the date when G0 stamps this freeze inside PQ. This file does not stamp it. Any change to a Q-metric definition after `p0-metrics-freeze` merges needs an Appendix A entry (§22.10).

  <p>Q1–Q21 are frozen at G0; inside PQ that freeze runs before the rows that use it (§22). Once <code>p0-metrics-freeze</code> merges, any change to a Q-metric definition gets an Appendix A entry (§22.10). §2 and every gate cite these Q IDs; milestones keep M0–M8 and MQ.</p>
  <table>
    <thead><tr><th>ID</th><th>Metric</th><th>Definition</th></tr></thead>
    <tbody>
      <tr><td>Q1</td><td>Indirect diffuse patch error</td><td>64–256 seeded world-space patches (0.25 m discs on surfaces with roughness ≥0.8). Indirect = full render minus a direct-only render at the same exposure. Error = mean |L<sub>test</sub> − L<sub>ref</sub>| / max(L<sub>ref</sub>, 0.01 × scene mean indirect). Test value = average of 64 frames after 120 settling frames. Reference: ≥4096 spp, with per-patch relative standard error ≤1%.</td></tr>
      <tr><td>Q2</td><td>Card-hit coverage</td><td>Ray set: 1 seeded ray per half-resolution pixel from the gather origins of 4 fixed cameras. Denominator: rays whose HWRT hit is within 10 m. Numerator: those landing on a valid card texel (resident page, card chosen per D6, coverage mask 1, depth error ≤2 card texels). Counted by a GPU counter.</td></tr>
      <tr><td>Q3</td><td>Leak ratio</td><td>Mean indirect luminance of 16 dark-room wall patches divided by that of 16 lit-room wall patches. Sealed rooms share a 10 cm (variant A) or 20 cm (variant B) wall; the reference must show &lt;0.1%.</td></tr>
      <tr><td>Q4a</td><td>Convergence, step scenes</td><td>Light toggled, sun jumped 15° and then held, or door teleported from closed to open (1 m), all at frame 0. L<sub>end</sub> = post-step static endpoint at frame 240; e(t) = mean |L<sub>t</sub> − L<sub>end</sub>| / L<sub>end</sub>. Gate: e(10) ≤25% and e(32) ≤10%.</td></tr>
      <tr><td>Q4b</td><td>Convergence, continuous scenes</td><td>Door opens 0 → 1 m over 2 s; Time of Day sweeps the sun 90° over 10 s. e<sub>lag</sub>(t) = mean |L<sub>t</sub> − L<sub>ref</sub>(t)| / L<sub>ref</sub>(t), against a path-traced reference rendered at the same animation state (every 4th frame, ≥1024 spp). Gate: mean e<sub>lag</sub> over the motion ≤25%, and e(32) ≤10% measured 32 frames after the motion ends.</td></tr>
      <tr><td>Q5</td><td>Crack pixels</td><td>A debug view draws back faces of closed meshes magenta. Crack = a magenta pixel, or a background pixel inside the reference coverage mask eroded by 2 px.</td></tr>
      <tr><td>Q6</td><td>Hole pixels</td><td>Background pixels inside the eroded reference mask during flight.</td></tr>
      <tr><td>Q7</td><td>FLIP</td><td>LDR FLIP on images tonemapped with the linear tonemapper at fixed exposure; report mean and p99. Fixed seeds in all stochastic passes.</td></tr>
      <tr><td>Q8</td><td>Temporal sigma</td><td>Per-patch relative luminance σ over 64 static frames.</td></tr>
      <tr><td>Q9</td><td>Reflection error</td><td>Reflection-only buffer, per roughness bin; mean radiance error and FLIP.</td></tr>
      <tr><td>Q10</td><td>Ghost trail</td><td>Mean error in disoccluded pixels at frame 4 after a cut or occluder move.</td></tr>
      <tr><td>Q11</td><td>Pass time</td><td>RD timestamps per named pass in timing mode: 600 frames after a 120-frame warmup, p95, 3 runs with ≤2% spread. §2.1 line 1b (amortized BLAS builds + compaction copies) is excluded from the steady-state p95 total; frames that spend it must hold p99 ≤ High total + 1b. Clocks locked with <code>nvidia-smi</code> on the RTX 4080 SUPER machine. M5 Max plugged in, High Power mode, thermal state recorded.</td></tr>
      <tr><td>Q12</td><td>Frame time p95/p99</td><td>Wall clock in timing mode (no movie writer, <code>--disable-vsync</code>).</td></tr>
      <tr><td>Q13</td><td>Overhead</td><td>Q11 total delta vs baseline over 3 runs, at the stated threshold. Where the baseline is plain SDFGI (G0-2, G1-4 and any other row that compares against it), it is the upstream Godot build at the fork’s import commit, with the same scene and settings (§22.10).</td></tr>
      <tr><td>Q14</td><td>Fallback coverage</td><td>Opaque pixels drawn by the legacy path for surfaces of virtualized meshes.</td></tr>
      <tr><td>Q15</td><td>Geometry memory</td><td>RD memory report categories, including the new AS accounting.</td></tr>
      <tr><td>Q16</td><td>Clamped energy fraction</td><td>Energy removed by firefly clamps / total indirect energy; must be ≤2%.</td></tr>
      <tr><td>Q17</td><td>Ray-query parity</td><td>Fixture rays start ≥1e-3 in barycentric distance from any edge, with no ties in t. Instance and primitive IDs exact. Barycentrics within 1e-5 absolute of analytic values (G0) and 1e-4 across APIs; t within 1e-5 relative of analytic values and 1e-4 across APIs. A separate edge-straddling set asserts that exactly one of the adjacent triangles is reported, with no misses and no double hits.</td></tr>
      <tr><td>Q18</td><td>RT coverage</td><td>Fraction of GI-contributing opaque instances within <code>rt_scene_radius</code> that are present in the TLAS, read from the RT-coverage counter. Gate: ≥95% on NextGI Perf (G1, G3).</td></tr>
      <tr><td>Q19</td><td>Upscaler image quality</td><td>LDR FLIP (Q7 rules) of the final output against a reference of the same frames at the same output resolution: <code>scaling_3d_mode</code> bilinear, <code>scaling_3d_scale</code> 2.0, 3D MSAA 8× with alpha-to-coverage on alpha-scissor materials; TAA, FSR2 and DLSS off; same seeds and spline. A reference is accepted only if FLIP(MSAA 4× ref, MSAA 8× ref) ≤0.01 per scene; failing content masks are excluded and reported. Moving form: mean over the spline frames after a 120-frame warm-up, excluding the 32 frames after a reset cut; p99 reported. Static form: 64 frames of the static segment after 120 settling frames. Reported per scene, per content mask and per output resolution. Gates use the mean across the gate scenes per output resolution.</td></tr>
      <tr><td>Q20</td><td>Final-image indirect error</td><td>Q1’s patch method (and Q9’s roughness bins on Roughness Corridor) applied to the final pre-tonemap linear HDR output at output resolution. I<sub>test</sub> = full − direct-only, both through the same upscaler/denoiser path, where direct-only zeroes NextGI’s ambient and reflection outputs. I<sub>ref</sub> = D16 full − direct at output resolution (≥4096 spp, per-patch relative standard error ≤1%). Error = mean |I<sub>test</sub> − I<sub>ref</sub>| / max(I<sub>ref</sub>, 0.01 × scene mean indirect), over 64 frames after 120 settling frames. Defined now; first used by GU+-3 (DLSS Ray Reconstruction, PU+, §21.5).</td></tr>
      <tr><td>Q21</td><td>Invalid motion vectors</td><td>Per-frame count of velocity texels that are bit-equal to the pre-clear sentinel <code>(-1.0, -1.0)</code> or have any non-finite component. Read from the velocity texture bound to a dense-velocity temporal upscaler (DLSS; the dev DENSE copy upscaler; MetalFX reported) by a debug pass immediately before dispatch, in capture mode only. Gate: 0 on every frame except a scripted reset frame, which is reported.</td></tr>
    </tbody>
  </table>

## Operational choices

These are the choices the tools implement. They do not change a §2 threshold. Where a gate publishes more than one ceiling, the tool's default is the primary one named here, and the others stay in the definition for the gate that cites them.

1. **Q1.** Indirect = full render minus direct-only. Error = mean over patches of |L_test − L_ref| / max(L_ref, 0.01 × scene mean indirect). The scene mean is the mean of the reference patch luminances. L_test is the mean of 64 frames after 120 settling frames (indices 120 through 183). A reference below 4096 spp, or with any per-patch relative standard error above 1%, is rejected and the gate fails. The error is still reported.
2. **Q2.** `valid_card_hits / hits_within_10m`, summed over CovCam0, CovCam1, CovCam2 and CovCam3. A missing camera fails the gate. Default gate: ≥ 0.85.
3. **Q3.** Mean indirect of the 16 dark-room patches divided by the mean of the 16 lit-room patches. The patch list is `patches_q3.json`; `room` is `dark` or `lit`. Default gate: ≤ 0.01. A reference whose own ratio is ≥ 0.1% (0.001) is flagged and the gate fails.
4. **Q4a.** L_end is frame 240. e(t) = mean |L_t − L_end| / L_end. Gate: e(10) ≤ 25% and e(32) ≤ 10%.
5. **Q4b.** `motion_end` is the first frame index after the motion stops. e_lag samples every 4th frame in `[motion_start, motion_end)` and divides by L_ref(t) with no 0.01 floor. The reference is at least 1024 spp. e(32) compares the test frame 32 after `motion_end` with the test frame 240 after `motion_end`. Gate: mean e_lag ≤ 25% and e(32) ≤ 10%.
6. **Q5.** Magenta is the 8-bit color (255, 0, 255). A background pixel equals the clear color, default (0, 0, 0). The reference coverage mask is eroded by 2 pixels with a 3×3 square element applied twice. Gate: count = 0.
7. **Q6.** Background pixels inside that eroded mask, summed over the flight frames. Gate: count = 0.
8. **Q7.** LDR FLIP (`flip-evaluator`, dynamic range `LDR`) on images tonemapped with clip(rgb × exposure, 0, 1), exposure 1 unless the capture says otherwise, then the IEC 61966-2-1 sRGB encoding. PNG frames are already display-referred. Report mean and p99. p99 uses the harness percentile. Default mean gate: ≤ 0.05. Other published ceilings (0.02, 0.005, 0.10) are not the default.
9. **Q8.** Per-patch population standard deviation (ddof 0) divided by the absolute mean, then the mean of those ratios, over 64 static frames. A zero mean with zero deviation is 0. A zero mean with a non-zero deviation is non-finite and fails. The frame count must be 64. Default gate: ≤ 0.015. Glass at ≤ 0.03 is a separate published ceiling, not the default.
10. **Q9.** Roughness bins are [0, 0.1), [0.1, 0.25) and [0.25, 0.4]. The last bin is closed at 0.4 because the gate is roughness ≤ 0.4. Radiance error uses Q1's patch function on the bin's luminance. FLIP is the mean of the Q7 error map inside the bin. The mirror bin is reported and does not fail the gate. Default gates: radiance error ≤ 0.30 on the two gated bins, FLIP ≤ 0.15 on [0.1, 0.25) and FLIP ≤ 0.10 on [0.25, 0.4].
11. **Q10.** Q1's patch function on the luminance of pixels marked by the disocclusion mask, at the frame named in `q10.json` (default 4). Default gate: ≤ 0.10.
12. **Q11.** Percentiles use every row of `timing.csv`, which is what `summary.json` was computed from. The 600-frame window after a 120-frame warmup is reported (`expected_measured_frames`, `expected_warmup`) and a shorter file does not fail by itself. Per frame, the steady-state total sums pass columns and leaves out line 1b (`line_1b_ms`, `as_updates_amortized_ms`, `1b_ms`, plus names in `q11.json`). Spread = (max − min) / min of that total's p95 across exactly 3 runs. Default gate: ≤ 0.02. One run has no spread to gate. Each pass p95 must match `summary.json` within 0.01 ms when a summary is present. Frames that spend line 1b must hold p99 of the full GPU time (including line 1b) ≤ High total + 1b when the device is known: Apple M5 Max 4.00 + 0.25 ms, RTX 4080 SUPER / Ada 1.60 + 0.10 ms, or the pair in `q11.json`. Those millisecond totals are report-only targets. `pass_lines` carries the spread line and the line 1b line. Issue #33 adds optional summary fields (`lines`, `diffuse_path`, `line_1b_frames`, `counters`). Q11 does not read them. It still excludes line 1b by column name on every frame. The new `line_1b_frames` tag is a separate, whole-frame split for the harness summary.
13. **Q12.** p95 and p99 of `wall_ms`. Pass when both match `summary.json` within 0.01 ms. With no summary, the gate passes and the percentiles are still reported. Millisecond targets are report-only.
14. **Q13.** (mean of 3 test steady-state p95 totals − mean of 3 baseline totals) / mean baseline. Default gate: ≤ 0.02. The command prints a pass line either way and exits 0 when the numbers were computed.
15. **Q14.** legacy-drawn virtualized pixels / opaque pixels. The gate fails at or above 0.20.
16. **Q15.** Sum of the RD memory categories. `MEMORY_ACCELERATION_STRUCTURES` is required; a missing category fails with a null value. Default gates: total ≤ 0.5× the baseline when a baseline is present, and the acceleration-structure category ≤ 25% of the total.
17. **Q16.** clamped energy / total indirect energy. Default gate: ≤ 0.02.
18. **Q17.** 32-byte little-endian records `{float t; uint custom_index; uint primitive; float u; float v; uint front_face; uint committed; uint candidate_count;}`, the layout agreed with issue #18. `--in` is one device's RD smoke-test dump folder (#49): `meta.json` plus, per fixture (`tri`, `rays`, `edges`, `winding`), `<name>.bin` and `<name>_analytic.json`. `rays` is required. A legacy folder with only `rays.bin` and `analytic.json` is still read. For `tri`, `rays` and `winding`, IDs, primitive, front face, committed and candidate count are exact. Analytic mode compares with relative t ≤ 1e-5 and absolute barycentrics ≤ 1e-5. Across APIs (`--ref` is the other device's folder, G2-1), the other device's records are the expected values, both tolerances are 1e-4, and no analytic file is needed for those fixtures. The two folders must hold the same fixtures with the same record counts. An `edges` record passes only when it committed one candidate (`committed` 1, `candidate_count` 1) on the right instance and the primitive is one of the two adjacent triangles. Across APIs, each device's edges are judged on their own against `edges_analytic.json`, because the two APIs may legally pick different adjacent triangles.
19. **Q19.** Q7-rules FLIP of the final output against the same frames at the same output size. The reference manifest, when present, must say `scaling_3d_scale` 2.0 and `scaling_3d_mode` 0 (bilinear). A scene reference is accepted only when mean FLIP(MSAA 4×, MSAA 8×) ≤ 0.01. `pass` is that acceptance. A content mask whose own 4×/8× FLIP exceeds 0.01 is excluded and reported, and it is not given a score. Moving form: frames at index ≥ 120, except the half-open range `[reset_frame, reset_frame + 32)`. Static form: 64 frames on `[120, 184)`. The result carries the scene, the per-mask scores, the output resolution and p99. When `--in` has child directories that each contain `q19.json`, the value is the mean of the accepted scene means.
20. **Q20.** Calls Q1's `q1_patch_error` on the final pre-tonemap indirect luminance (full minus direct-only). The spp and standard-error rules are Q1's. On a capture that also has `roughness.exr`, the same function is applied per Q9 bin. Default gate: ≤ 0.20.
21. **Q21.** A texel counts when its stored R and G are bit-equal to -1.0 in that channel's dtype (half or float), or when either component is non-finite. -1.0001 in float32 is not the sentinel. The value is the count on non-reset frames. Reset frames listed in `q21.json` are reported and do not fail the gate. Gate: 0.

A ratio whose denominator and numerator are both 0 is 0. A positive numerator with a 0 denominator is non-finite and fails. JSON encodes non-finite numbers as null.

## Inputs

The harness files and the patch JSON are read as issues #12 and #10 defined them. This package does not change those formats.

| File | Role |
|---|---|
| `hdr_*.exr`, `direct/hdr_*.exr` | Full and direct-only linear HDR. Indirect is their difference. |
| `frame*.png` | Display-referred 8-bit frames. Used when no `hdr_*.exr` is present. |
| `timing.csv`, `summary.json` | Harness timing. Columns `frame,wall_ms,<pass>_ms`. Blank cells are skipped. `summary.json` may also carry `lines`, `diffuse_path`, `line_1b_frames`, and `counters` (issue #33). Extra keys do not change Q11. |
| `manifest.json` | Optional `host`, and for Q19 `output_size`, `scaling_3d_scale`, `scaling_3d_mode`. |
| `patches_q1.json`, `patches_q3.json` | Seeded patches. Q3 rows add `room` (`lit` or `dark`); dark rows may add `mirror_of`. |
| `mask.png` or `mask_*.png` | Patch ids. Encoded id = R + 256·G, gray masks use the gray value, 0 means none, patch id = encoded − 1. |
| `coverage.png` | Q5 and Q6 reference coverage. |
| `cmask_<name>.png` | Q19 content mask. |
| `roughness.exr` | Q9, and Q20's per-bin path. |
| `disocclusion.png`, `q10.json` | Q10. |
| `counters.json` | Q2, Q14, Q15, Q16, Q18. The renderer that fills it is later work. |
| `reference.json` | `spp` and `per_patch_relative_se` for Q1 and Q20. |
| `q4.json` | `motion_start`, `motion_end`, optional `spp`. |
| `q11.json` | Optional extra line-1b column names and device budgets. |
| `q19.json` | `form` (`static` or `moving`), optional `reset_frame` and `scene`. |
| `q21.json` | `reset_frames`. |
| `velocity_*.exr` | Q21, RG, half or float. |
| `meta.json`, `<fixture>.bin`, `<fixture>_analytic.json` | Q17, with `tri`, `rays`, `edges` and `winding` (legacy: `rays.bin`, `analytic.json`). Across APIs, `--ref` is the other device's dump folder. |
| `pixels.json`, `exposure.json` | Optional clear color and exposure. |
| `msaa4/` | Q19 MSAA 4× reference, next to the MSAA 8× frames in `--ref`. |

The harness capture does not yet contain direct-only renders, patch masks, content masks, coverage, velocity, counters, spp, roughness, disocclusion, the MSAA 4× stack or ray records. Those sidecars are named here so later issues can write them without changing the metric.

Several runs of one timing metric live as `timing.csv` in the directory plus `run_2/`, `run_3/`, or as only `run_*` children. Q13 reads the baseline from `--ref` the same way.

## Command

From `godotnext/testbed/harness/metrics`:

```
python3 -m godotnext_metrics <metric> --in <dir> [--ref <dir>] --json
```

The process prints one JSON object with `metric`, `value`, `threshold` and `pass`, plus `values` when there is a breakdown. Exit 0 when that object is a computed result, including a failed gate, so a q11 or q13 pass line does not fail the PR. Exit 2, still with JSON, when the metric name is unknown or an input is missing.

## Python

The tools need Python >= 3.11. `numpy` 2.3 and `OpenEXR` 3.5 both require it and both publish wheels for 3.11 through 3.14. `flip-evaluator` 1.7 publishes wheels through 3.13 and builds from its sdist on 3.14. The issue text says Python >= 3.10; 3.10 cannot install these pins. Doctests in the engine do not import this package.

## Streaming reports (#74)

These are not new Q commands. `python3 -m godotnext_metrics q15` still sums `counters.json` and still requires `MEMORY_ACCELERATION_STRUCTURES`. The streaming CLI is:

```
python3 -m godotnext_metrics.streaming_reports {q15,q14,slow-io,exclusion} --in <dir> [--out <dir>] [--markdown]
```

Schema 1. Exit 0 when the JSON object was produced, including a null source or a failed gate. Exit 2 when `--in` is missing or a sidecar is malformed. `--out` writes the JSON file and a Markdown summary.

| Report | File | Sidecar | Absent until |
|---|---|---|---|
| Q15 geometry memory | `geometry_memory.json` | `memory_report.json` | #78 / #79 `virtual_geometry_get_memory_report` |
| Q14 flight | `fallback_coverage.json` | `routing_frames.json` | routing counters (`legacy_drawn_virtualized_pixels`, `opaque_pixels`, or a `fallback_coverage` number) |
| Slow IO Flight | `slow_io_flight.json` | `slow_io_params.json`, optional `timing.csv` | #78 page-upload samples. Frame-time percentiles are filled from `wall_ms` when `timing.csv` is present. `pass` stays null (PQ). |
| Exclusion Matrix | `exclusion_matrix.json` | `cells.json`, optional `exclusion_observed.json` and `moving_track.json` | #79 `virtual_geometry_get_debug_stats` |

Q15 lists `authored_remainder`, `lods`, `shadow_mesh`, `coarse_fallback`, `rt_arena`, `blas`, `tlas`, `as_scratch`, `dag_headers` and `page_cache` for `strip_authored_to_coarse`, `discrete_lod` and `retain_authored`. The ratio is stripped/baseline. It passes at or under 0.5 when both totals exist. `rt_share` is `(rt_arena + blas + tlas + as_scratch) / total` of the stripped export. `retain_authored` is reported and is not gated. A missing category on a present source makes the total null and fails. 0/0 is 0. A positive total over 0 is non-finite, becomes JSON null, and fails.

Q14 over a flight reports the mean and the maximum. A frame with both pixel counters uses the frozen Q14 ratio. A frame with only `fallback_coverage` uses that number. Mean or maximum at or above 0.20 fails. An empty or absent source leaves both null and `pass` null.

Each report copies `envelope` from `manifest.json` when that file is present: `host`, adapter name, `api_version` as the driver, `commit` as `build_hash`, and `seed`. `gpu_index` is parsed from `command_line.engine` and stays null when that string is missing. A DLSS run can add `dll_sha256` and `preset` from `manifest.json`'s `dlss` object. Without that object the envelope says `N/A`. The Exclusion Matrix track ships with an empty DLL hash and names FSR2 as the fallback until a build exposes a DLSS scaling mode.
