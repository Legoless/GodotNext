# GodotNext handoff

The current state of the build and what to do next. It is written for any agent: Claude, Gemini or another. Update this file whenever work stops and after each landing, in the same change set as the landing or the tracker refresh. Machine-specific details (exact local paths, VM names and zones, private library folders, saved lane results) are in the git-ignored companion `local/handoff-private.md`.

**Last updated:** 2026-11-20, evening. #298 (`96a9a204c5`) and #297 (plan v3.9.17, `430d5b13fe`) are closed; #150 (`7b7d2f5982`), #217 (`230a613b62`, VSM × Nanite — unblocking 12 M6/M7 tasks) and #305 (`95f6bc869b`, `MaterialData::texture_cache` refresh) landed as progress after Linux Vulkan VVL 1.4 and Windows Vulkan/D3D12/D3D12-GBV verification (waiting only on Metal/M5 Max); #301 is in its final Fix & Re-prove pass.

## How to continue

1. Read `AGENTS.md` (the rules) and the project skills in `.agents/skills/`. The skills are plain Markdown and work for any agent:
   - `godotnext-implement-issue` builds an issue and lands it;
   - `godotnext-fleet` covers the machines and how to use them;
   - `godotnext-update-tracker`, `godotnext-plan-revision` and `godotnext-issues` cover the tracker, the plan and the issues.
2. Read this file, then `local/handoff-private.md`. The private file has the two remaining cloud VMs (one Linux L4 and one Windows L4 with the private Mesa NIR library), the VM start command, and the pending on-VM agent setup plan.
3. `git fetch`; master should be at or after this commit. Refresh the tracker with `python3 docs/godotnext/tracker/export.py`.
4. Lead preference (2026-11-20): keep the orchestrating agent running locally on the Mac while builds, doctests and GPU validation runs execute on the cloud Linux L4 and Windows L4 VMs (or the RTX 4080 SUPER machine when reachable).
5. Stop cloud VMs as soon as they have no work left.

### The lane loop (what Claude's workflows did)

1. **Build.** Implement or finish the issue in its worktree. Builds and GPU runs go on the cloud VMs or the RTX 4080 SUPER machine, never the Mac. The helpers in `local/gce/` and `local/sal/` build a worktree's diff on a VM, run a scene job and fetch the logs; their header comments explain each one. Prove the issue's criteria: no warnings, full doctests, the issue's GPU checks with 0 validation errors on Vulkan and D3D12, and the phase smoke scene.
2. **Review.** An independent read of the diff against the issue, the plan and the clean room (§19), looking for concrete defects.
3. **Fix** the real findings and re-prove from a clean state.
4. **Landing check** on the cloud test machine (cloud Windows L4 VM with the private Mesa library) or the RTX 4080 SUPER machine. Apply the diff to current master, build, run the issue's landing runs, and name the machine in the issue comment (lead decision 2026-11-16).
5. **Land** on master. Use "Closes #N" only when every build criterion is met. Metal and macOS builds wait for the lead's Mac runs, so most issues land as progress. Then post the issue comment, refresh the tracker, update this file and delete the WIP branch.

The lead does not want questions. Take the recommended option, or follow UE 5.9, record the choice, and report it. Ask only about spending, accounts, the lead's own sign-offs, or publishing anything new.

## State at the handover

- Master: see `git log`. Plan v3.9.17.
- Landed on 2026-11-20:
  - **#298 closed (`96a9a204c5`)**: fixed upstream `EditorHelp` shutdown race (`EditorHelp::doc_cleaned`, thread handoff sync, guarded deferred callbacks, clearing `EditorFileSystem::singleton`), added 4 doctests and a 20-run cross-platform `test_headless_import.py` regression check; verified 20/20 imports + 0 warnings + full doctests + Vulkan VVL 1.4 / D3D12+GBV smoke on the cloud Linux L4 VM and cloud Windows L4 test machine (`25.3.1-gn4`).
  - **#297 closed (plan v3.9.17)**: counterbalanced 15×15 interleaved AB/BA runs on the cloud Windows L4 test machine (D3D12 + Vulkan, 1920×1080, `--gpu-validation`, NextGI off, VSM off) and a static audit across `b6596def89..953e76ffe0` proved legacy SDFGI's bimodal ±1 binary16 ULP variation is ordinary upstream GPU non-determinism (Fisher `p = 1.0000` D3D12, `p = 0.6999` Vulkan; bit-exact cross-tree EXRs in both clusters; zero engine code changes); recorded baselines and the interleaved AB/BA `max_ulp <= 1` rule in plan §22.2.
  - **#150 (progress, `7b7d2f5982`)**: added `displacement()` spatial shader function with `DISPLACEMENT` output and `texture()` → `textureGrad()` lowering, `Material.displacement_magnitude`/`displacement_center` (`RenderingServer.material_set_displacement()`), `BaseMaterial3D.heightmap_mode = PARALLAX | DISPLACEMENT`, `RASTER_FEATURE_DISPLACED` + cull bound inflation, `#163` dicing-compute compiler hook, `--doctool` class docs, doctests (Forward+, Forward Mobile, Compatibility, Vulkan/D3D12/Metal containers), and `nextgeo_displacement_check.tscn`. Independently reviewed (5 findings fixed) and verified on the cloud Linux L4 VM (`0` warnings, `2,188/2,188` doctests, Vulkan VVL 1.4) and cloud Windows L4 test machine (`25.3.1-gn4`, `0` warnings, `2,212/2,212` doctests, Vulkan + D3D12 + D3D12 GBV, `0/6,220,800` bytes diff at magnitude `0`). Waits only on macOS/Metal build and M5 Max GPU run.
  - **#217 (progress, `230a613b62`)**: added VSM × Nanite per-page multi-view cluster culling in `cluster_cull.glsl` (`1` directional + `1` local indirect dispatch chain), 1-level 8×8 per-page static Hi-Z, fixed-function HW (`vsm_raster_nextgeo.glsl`, `#177` indirect-count with `firstInstance = 0`) and SW (`vsm_raster_sw.glsl`, 32-bit `imageAtomicMax`) rasterizers, static/dynamic layer routing hook for `#212`, programmable raster-bin VSM Group 2 (`NEXTGEO_RASTER_DEPTH_ONLY`) integrated with `#150` (`RASTER_FEATURE_DISPLACED` + `displacement_bound` cull inflation), live NextGeo VSM projection sampling, `VSM_OVERDRAW` Nanite pairs, `[VSM][NextGeo]` doctests, and `tests/vsm/nanite.tscn`. Independently reviewed (5 findings fixed) and verified on `master` (over `#150`) on the cloud Linux L4 VM (`0` warnings, `2,198/2,198` doctests, Vulkan VVL 1.4) and cloud Windows L4 test machine (`25.3.1-gn4`, `0` warnings, `2,222/2,222` doctests, Vulkan + D3D12 + D3D12 GBV). Waits only on macOS/Metal build/run and PQ lines.
  - **#305 (progress, `95f6bc869b`)**: added `MaterialStorage::TextureCacheIndex` reverse index and `texture_rd_replaced()` so `texture_replace_compatible()` refreshes `MaterialData::texture_cache` in place (bumping `change_serial` for `#167` path tracer history), notified unconditionally with pre-free RD snapshots from `texture_replace()`, `texture_proxy_update()`, `texture_free()`, and `_update_render_target()` (including `0×0` shrink), hardened `MaterialData::get_parameter_uniforms()` across `#155`, `#158`, and `#217` VSM programmable bins, and hardened `NextGIHitMaterials` bindless slot acquisition/tracking. Independently reviewed (4 findings fixed) and verified on `master` (over `#217`) on the cloud Linux L4 VM (`0` warnings, `2,201/2,201` doctests, Vulkan VVL 1.4) and cloud Windows L4 test machine (`25.3.1-gn4`, `0` warnings, `2,225/2,225` doctests, Vulkan + D3D12 + D3D12 GBV). Waits only on macOS/Metal build/run.
- Landed on 2026-11-19, all as progress unless noted:
  - #193 TSR async compute, #218 VSM non-Nanite raster and #167 the path tracer (checked together on the cloud L4 and T4 test machines), with plan v3.9.15.
  - #299 closed (the D3D12 front-layer depth target).
  - #158 material shading bins, #295 AVBOIT, #145 RT visibility policy, #221 VSM product surface and #222 VSM projection and SMRT, with plan v3.9.16 (LQ6 decided: translucent casters shadow as opaque, as UE 5.9 does).
  - Stack 2 review and verification (`02a07e6b4d`): independent code review of #222, merged #221 and #145's #286/#167 port completed; 4 Major and 3 Minor defects fixed (centered depths in `vsm_prefiltered_distant()` + 10 km doctest, Frame-1 positional shadow-atlas slot release via `shadow_atlas_remove_light()`, `vsm_light_skips_atlas()` LRU touch + directional fallback guard, unified VSM projection readiness check, `vsm_sample_view` gating, #145 Simplified Material verdict latch, #145/#167 shared texture-table latch wakeup). Rebuilt with 0 warnings and full doctests (2,181 Linux / 2,205 Windows) and verified on the cloud Linux L4 VM (21/21 Vulkan VVL 1.4 checks) and the cloud Windows L4 test machine with private Mesa NIR 25.3.1-gn4 (64/64 full Stack 2 + 24/24 fix re-verification checks across Vulkan, D3D12 debug layer and D3D12 GPU-based validation, including #299's D3D12 front-layer AVBOIT rerun).
- Landed on 2026-11-18: the master repair (#130, #155 and #219 reopened for Metal), #122 and #293 closed (the private Mesa NIR library 25.3.1-gn4), #286 (Hardware Ray Tracing Mode), plans v3.9.13 and v3.9.14.
- New issues: #297 to #305 (#302 skinned alpha RT shadows, #303 NextGeo alpha RT shadows, #304 `discard` in #130 any-hit stages, #305 stale `MaterialData::texture_cache` in #155/#130).

## Open work from the closed lanes

All Linux Vulkan and Windows Vulkan/D3D12/D3D12-GBV verification and code-review items for Stack 2 (#145, #158, #221, #222, #295) are complete on master (`02a07e6b4d`). Each of those five issues now waits only on Metal / M5 Max builds and runs (plus PQ lines). No lane is running, and all cloud VMs are stopped.

## Ready to start (phase order)

- **In progress (Mac-hosted lane on cloud L4 VMs):** **#301** RD read-only depth attachments (GT-7, in Fix & Re-prove).
- **Unblocked on master by #150 and #217 (Linux/Windows code ready):** M6 VSM tasks (#212, #216) and M7 Nanite tasks (#149, #151, #153, #154, #160, #161, #163, #164, #165), plus #145 follow-ups (#302, #303, #304).

## Waiting on the lead

- **#98 DLSS license sign-off.** It holds the DLSS track, plus #236's UE-side Frame Generation, MFG, Reflex and Nsight.
- **Mac runs.** Every Metal build and run waits for the lead (~47 open issues already have code on master waiting mainly on Metal).
- **AMD test machine.** A cloud Radeon PRO V710 (#280).
- **RTX PRO 6000 gcloud quota:** the requests are pending, with 0 granted.

## Known issues and risks

- **PresentMon writes no rows on the RTX 4080 SUPER machine** (#202, #236; possibly no active monitor). It must be fixed before PQ.
- **#236's UE side is render-thread bound in editor game mode**, so it is not GPU-bound. PQ decides how to handle it.
- **The RTX 4080 SUPER machine is shared** with another project's UE automation. Never stop or touch anything that isn't GodotNext's own there.
- **The repository is public.** No local paths, hosts, IPs, VM names or account names go in tracked files, commit messages or issue comments.
- **Decisions made on 2026-11-16 under the lead's rule** are recorded in the plan's §18.1 and the issue comments. The lead can overrule any of them.
