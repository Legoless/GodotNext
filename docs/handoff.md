# GodotNext handoff

The current state of the build and what to do next. It is written for any agent: Claude, Gemini or another. Update this file whenever work stops and after each landing, in the same change set as the landing or the tracker refresh. Machine-specific details (exact local paths, VM names and zones, private library folders, saved lane results) are in the git-ignored companion `local/handoff-private.md`.

**Last updated:** 2026-11-21. Cleanup and readiness pass: no lane is running, master is the only branch locally and on origin, no worktrees or stashes, and nothing GodotNext runs on the RTX 4080 SUPER machine. A fresh check after the cloud sign-in confirmed that no GodotNext cloud VMs, disks, snapshots, images or reserved addresses exist. Ten issues with landed progress that had never been assigned (#105, #123, #198, #203, #263, #265, #274, #275, #283, #285) are now assigned, so the tracker counts them in progress, not ready. "Ready to start" below was rebuilt from a read-only review of 41 candidate issues; the detailed lane queue and a cloud rebuild runbook are in the private companion. Earlier on 2026-11-21 the tracker page moved to plan v3.9.17. On 2026-11-20 night, #212 (`a5b4d5d660`, VSM caching and per-page invalidation) landed as unverified progress: repository hooks pass, but its engine builds, new tests, counter checks and GPU validation remain open. The earlier Mac-hosted batch is complete across 6 issues: #298 (`96a9a204c5`) and #297 (plan v3.9.17, `430d5b13fe`) closed; #150 (`7b7d2f5982`), #217 (`230a613b62`, VSM × Nanite — unblocking 12 M6/M7 tasks), #305 (`95f6bc869b`, `MaterialData::texture_cache` refresh), and #301 (`1c738bdcf7`, RD read-only depth attachments for TSR level 2 overlap / GT-7) landed as progress after Linux Vulkan VVL 1.4 and Windows Vulkan/D3D12/D3D12-GBV verification (waiting only on Metal/M5 Max). No lane is running, all worktrees are removed, and all GodotNext cloud VMs, boot disks and template snapshots are deleted.

**#212 build fix landed on 2026-11-21 (`7739b79509`).** Master now builds with `dev_mode` on the GPU-less Linux VM and the RTX 4080 SUPER machine. Linux ran the full doctests (2,209 cases) and a clean `--doctool` round trip; Windows ran the targeted VSM tests (63 cases). The fix declares `InstanceGeometryData` before its VSM use, updates the NextGeo counter-layout assertion to 112 bytes and aligns the cache-invalidation API documentation. This is build-fix progress only: #212's behavioral, counter, GPU and Metal criteria remain open.

## How to continue

1. Read `AGENTS.md` (the rules) and the project skills in `.agents/skills/`. The skills are plain Markdown and work for any agent:
   - `godotnext-implement-issue` builds an issue and lands it;
   - `godotnext-fleet` covers the machines and how to use them;
   - `godotnext-update-tracker`, `godotnext-plan-revision` and `godotnext-issues` cover the tracker, the plan and the issues.
2. Read this file, then `local/handoff-private.md`. It points to the private lane queue, the cloud rebuild runbook and the archived history. No GodotNext cloud VMs, disks or template snapshots exist.
3. `git fetch`; master should be at or after this commit. Refresh the tracker with `python3 docs/godotnext/tracker/export.py`.
4. Lead preference (2026-11-20): keep the orchestrating agent running locally on the Mac while builds, doctests and GPU validation runs execute on cloud Linux L4 and Windows L4 VMs (or the RTX 4080 SUPER machine when reachable). Recreating cloud VMs is spending: get the lead's go-ahead first, then follow the private rebuild runbook.
5. Stop cloud VMs as soon as they have no work left; delete them when the build is paused.

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
  - **#301 (progress, `1c738bdcf7`)**: added `RenderingDevice::DRAW_READ_ONLY_DEPTH`, `ATTACHMENT_STORE_OP_NONE`, `API_TRAIT_SUPPORTS_ATTACHMENT_STORE_OP_NONE`, `RDG::ATTACHMENT_OPERATION_READ_ONLY`, `SharedReadLayoutTransition` (with slice inheritance, bindless dirty-depth `SHADER_READ` merging, cross-frame visibility barriers, and 128-bit `FramebufferStorageKey`), spec-verified Vulkan (`VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL` + `VK_KHR/EXT_load_store_op_none`), D3D12 (`D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE` + read-only DSVs), and Metal (`MTL::StoreActionDontCare` without store promotion) support, Forward+ read-only depth for sky, specular merge, AVBOIT composite/remainder, and transparent passes (when no `RENDER_LIST_ALPHA` surface writes depth or stencil), updated `RenderingDevice.xml`/`ProjectSettings.xml`/`rhi-async-compute.md`, and added render-graph/bindless/Forward+ doctests. Independently reviewed (6 findings fixed) and verified on `master` on the cloud Linux L4 VM (`0` warnings, `2,209/2,209` doctests, `--doctool` `0` diff, Vulkan VVL 1.4.363.1 syncval with live cross-queue positive control, `0` hazards, `30/30` level-2 forks) and cloud Windows L4 test machine (`25.3.1-gn4`, `0` warnings, `2,233/2,233` doctests, Vulkan VVL 1.4.363.1 syncval + D3D12 debug layer + D3D12 GBV + byte-identity across smoke & legacy scenes). Waits only on macOS/Metal build/run and PQ gate GT-7.
  - **#212 (progress, `a5b4d5d660` + build fix `7739b79509`)**: added VSM static/dynamic instance routing, swept-bounds and light-map invalidation, the deferred static invalidation budget, the per-instance cache API and documentation, NextGeo dynamic-hook routing and cache telemetry. The follow-up fixes the Linux and Windows compile failure, the NextGeo counter-layout test and the cache-invalidation API docs. Both builds pass; Linux full doctests and doctool pass, Windows targeted VSM tests pass, and repository hooks pass. Behavioral tests, counter checks, Vulkan/D3D12/Metal validation and smoke renders remain open; every acceptance box remains open.
- Landed on 2026-11-19, all as progress unless noted:
  - #193 TSR async compute, #218 VSM non-Nanite raster and #167 the path tracer (checked together on the cloud L4 and T4 test machines), with plan v3.9.15.
  - #299 closed (the D3D12 front-layer depth target).
  - #158 material shading bins, #295 AVBOIT, #145 RT visibility policy, #221 VSM product surface and #222 VSM projection and SMRT, with plan v3.9.16 (LQ6 decided: translucent casters shadow as opaque, as UE 5.9 does).
  - Stack 2 review and verification (`02a07e6b4d`): independent code review of #222, merged #221 and #145's #286/#167 port completed; 4 Major and 3 Minor defects fixed (centered depths in `vsm_prefiltered_distant()` + 10 km doctest, Frame-1 positional shadow-atlas slot release via `shadow_atlas_remove_light()`, `vsm_light_skips_atlas()` LRU touch + directional fallback guard, unified VSM projection readiness check, `vsm_sample_view` gating, #145 Simplified Material verdict latch, #145/#167 shared texture-table latch wakeup). Rebuilt with 0 warnings and full doctests (2,181 Linux / 2,205 Windows) and verified on the cloud Linux L4 VM (21/21 Vulkan VVL 1.4 checks) and the cloud Windows L4 test machine with private Mesa NIR 25.3.1-gn4 (64/64 full Stack 2 + 24/24 fix re-verification checks across Vulkan, D3D12 debug layer and D3D12 GPU-based validation, including #299's D3D12 front-layer AVBOIT rerun).
- Landed on 2026-11-18: the master repair (#130, #155 and #219 reopened for Metal), #122 and #293 closed (the private Mesa NIR library 25.3.1-gn4), #286 (Hardware Ray Tracing Mode), plans v3.9.13 and v3.9.14.
- New issues: #297 to #305 (#302 skinned alpha RT shadows, #303 NextGeo alpha RT shadows, #304 `discard` in #130 any-hit stages, #305 stale `MaterialData::texture_cache` in #155/#130).

## Open work from the closed lanes

All Linux Vulkan (VVL 1.4 / 1.4.363.1 syncval) and Windows Vulkan/D3D12/D3D12-GBV verification and code-review items for Stack 2 (`#145`, `#158`, `#221`, `#222`, `#295`) and today's earlier 2026-11-20 batch (`#150`, `#217`, `#305`, `#301`, plus closed `#298` and `#297`) are complete on `master` (`1c738bdcf7`). The open issues from both batches wait only on Metal / M5 Max builds and runs (plus PQ lines). #212's code builds on `master` after `7739b79509`; its next lane must add the required behavioral tests and prove the counter and GPU acceptance criteria before it can close. The #229 harness lane is in progress on the existing machines. No cloud VM, boot disk or template snapshot has been created; all GodotNext cloud resources remain deleted.

## Ready to start (phase order)

Every issue below has a macOS/Metal criterion, so it lands as progress until the lead's Mac runs. "Code-unblocked" means its blockers are still open, but their code is on master, proven on Linux and Windows and waiting mainly on Metal. On the tracker, "ready" means every blocker is closed.

**Master builds again (#212)**

- The #212 build fix is on master (`7739b79509`): Linux `dev_mode`, full doctests and doctool pass; Windows `dev_mode` and targeted VSM tests pass. Other engine lanes can start from this fix and must not patch #212's code independently.
- **#212 proof:** fix the risks found by reading the code (the atlas-skip rule must match #222's `vsm_light_skips_atlas()`; the STATIC counter is never incremented; hiding or freeing a STATIC instance must behave as in UE), add the `[VSM]` doctests and a VSM caching scene with counter checks, run Windows Vulkan, D3D12 and GBV on the RTX 4080 SUPER machine (gn4 is installed) and Linux Vulkan VVL on a cloud Linux L4 VM, then re-run the #217 to #222 checks and the legacy byte-identity check with VSM off.
- Current master has Linux and Windows build coverage. Mac close-outs still need the lead's macOS/Metal run; the Windows Vulkan, D3D12 and GBV legs can run on the RTX 4080 SUPER machine. Any landing that needs Linux Vulkan VVL waits for the lead's go-ahead to recreate a cloud Linux VM.

**Start now (no engine build needed)**

- **#229** UE comparison harness: Python with pytest, configs and UE-side observations on the existing comparison binary. It opens the test-prep lane below.

**Start once the #212 build fix lands**

- **RT shadow follow-ups** (code-unblocked by #145, and by #130 for #304): **#302** skinned alpha RT shadows, then **#303** NextGeo alpha RT shadows, in one lane; **#304** `discard` → `ignoreIntersection` in #130's any-hit stages as its own lane. #302 and #304 touch no #212 files. #303 may edit `mesh_storage.cpp` and `virtual_mesh_storage.*`, which #212 changed, so it rebases over any #212 fix and agrees its UV path with #153. All three add cases to the `shadow_policy` fixture, so land them one after another.
- **#139** light grid: code-unblocked (its only open blocker, #221, is on master) and independent of #212; check its Forward+ and light-storage files against the #212 lane first. #141 follows it.
- **#129** hit lighting on Vulkan and D3D12: #121 and #130 are on master, while its Metal part waits on #172, which has no code yet. #294 follows it, after a second reader re-checks the archived UE World Cache page (#286's note).
- **M7 Nanite** (code-unblocked by #150, #155, #162, #178 and #217). They share files with each other and with #212, so run at most two lanes, land them stacked and rebase over any later #212 fix.
  - **#153** masked/two-sided/custom-UV materials + VSM first, since it unlocks the most; **#161** spline meshes beside it, the node and legacy half first.
  - Then **#154** mesh-shader HW raster (D3D12 needs gn4) and **#149** pixel depth offset (steps 1 to 3; its VSM step waits until #212 is proven).
  - Then **#163** tessellation (Experimental, off). Agree its reserved visible-cluster ID range with #165 first.

**Side jobs (fit them into machine gaps)**

- **#283** (no code): run `tier_check` and the smoke scenes on Vulkan and D3D12 on the RTX 4080 SUPER machine, since its criteria name that card.
- **#123:** a GCC MinGW build fix (locals named `near`/`far` in NextGI files collide with MinGW macros) or a record that MinGW D3D12 is out of scope.
- **#198:** two plan records. D4: keep the landed design and update the issue's step 3. LQ11: follow UE 5.9 (thin geometry on automatically with Nanite foliage) and record a #151 follow-up.

**After #212 is proven**

- **#216** VSM local lights: it changes the same files as #212, so it follows in the same lane.
- **#164** WPO + VSM: #212 is a formal blocker, and it lands after #153.
- Behind them, and not code-unblocked yet: **#160** (also needs #154), **#151** (needs #153 and #164), **#165** (needs #151), **#296** (needs #152 and #160).
- #149's VSM step, and the GodotNext half of #231's GV-3.

**Waits on the Mac (close-outs: their code is on master)**

- Don't start these as new work.
  - Build-only closes: #285, #283 (after its RTX 4080 SUPER run) and #274.
  - Runs: #174 async compute; #275 `attachment_clears` (its Metal hunks have never been compiled); #263 the reference capture and the Q1 tool; #265 legacy SDFGI identity (not on unproven #212 code).
  - TSR and frame generation: #198, #203 and #105, together with #195, #193, #301 and #210.
  - **#123** (`673499de5b`): proven on the RTX 4080 SUPER machine and a cloud Windows L4 (boxes 1, 2 and 5 met). It waits on the macOS/Metal check and on the MinGW fix or decision. The code of its direct build dependents (#118, #122, #175, #177, #195) is on master; #125 is a PQ gate and #273 waits on the lead, so its large "unlocks" count overstates the startable work.
- About 47 open issues in all have code on master waiting mainly on Metal.

**Test prep (MQ)**

- One UE-comparison lane, one task at a time, on the RTX 4080 SUPER machine (UE side) and the GPU-less Linux VM (metrics tests). These issues share the comparison configs, harness runners, exporter, `settings-map.md` and plan §23.5.
- Order: **#229** harness, **#235** exporter and port fixes, **#230** re-export and calibrate, **#232**'s TSR part, **#231** without GV-3, the UE-side items of **#233** and **#234**, then **#236**'s PresentMon diagnosis. Fold in **#228**: check whether the UE 5.9 final notes are out, and carry plan v3.9.3 (editor game mode) into `settings-map.md`.
- #233's GodotNext side waits on MegaLights (#142, #143, #146, #135) and on #141; #234's waits on #160, #163 and #151. #199 is a PQ gate behind #225.

## Waiting on the lead

- **Cloud machines.** All GodotNext cloud VMs, disks and template snapshots were deleted on 2026-11-20. Recreating the cloud Linux L4 VM (the Linux Vulkan VVL landing leg), the cloud Windows L4 test machine and the cloud T4 is spending, so it needs the lead's go-ahead and a fresh cloud sign-in.
- **#98 DLSS license sign-off.** It holds the DLSS track (#99, #100, #106, #168's OIDN option, #200, #208), #232's GU-5 rows, #236's UE-side Frame Generation, MFG, Reflex and Nsight, and #229's DLSS plugin read. Its CI box needs CI re-enabled or waived.
- **Mac runs.** Every Metal build and run waits for the lead (~47 open issues already have code on master waiting mainly on Metal).
- **Hardware.** #227 RTX 5090 (the RTX PRO 6000 stand-in quota requests are pending, with 0 granted) and #280 AMD Radeon PRO V710.
- **Sign-offs.** Each UE port's settings record and screenshots (#230 to #236).

## Known issues and risks

- **PresentMon writes no rows on the RTX 4080 SUPER machine** (#202, #236; possibly no active monitor). It must be fixed before PQ.
- **#236's UE side is render-thread bound in editor game mode**, so it is not GPU-bound. PQ decides how to handle it.
- **The RTX 4080 SUPER machine is shared** with another project's UE automation. Never stop or touch anything that isn't GodotNext's own there.
- **The repository is public.** No local paths, hosts, IPs, VM names or account names go in tracked files, commit messages or issue comments.
- **Decisions made on 2026-11-16 under the lead's rule** are recorded in the plan's §18.1 and the issue comments. The lead can overrule any of them.
