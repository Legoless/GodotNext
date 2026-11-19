# GodotNext handoff

The current state of the build and what to do next. It is written for any agent: Claude, Gemini or another. Update this file whenever work stops and after each landing, in the same change set as the landing or the tracker refresh. Machine-specific details (exact local paths, VM names and zones, private library folders, saved lane results) are in the git-ignored companion `local/handoff-private.md`.

**Last updated:** 2026-11-19, afternoon (Claude). The lead closed all lanes: no lane is running, and the cloud VMs are stopped.

## How to continue

1. Read `AGENTS.md` (the rules) and the project skills in `.agents/skills/`. The skills are plain Markdown and work for any agent:
   - `godotnext-implement-issue` builds an issue and lands it;
   - `godotnext-fleet` covers the machines and how to use them;
   - `godotnext-update-tracker`, `godotnext-plan-revision` and `godotnext-issues` cover the tracker, the plan and the issues.
2. Read this file, then `local/handoff-private.md`. The private file has the VM start command, which also writes each VM's new IP into `local/fleet.env`, plus where every lane's saved results and notes are.
3. `git fetch`; master should be at or after this commit. Refresh the tracker with `python3 docs/godotnext/tracker/export.py`.
4. Start only the cloud VMs the lane you pick up needs (machine column below). Stop VMs as soon as they have no work left.
5. Work one lane at a time, or a few in parallel, through the loop below. Land each one when it passes.

### The lane loop (what Claude's workflows did)

1. **Build.** Implement or finish the issue in its worktree. Builds and GPU runs go on the cloud VMs or the RTX 4080 SUPER machine, never the Mac. The helpers in `local/gce/` and `local/sal/` build a worktree's diff on a VM, run a scene job and fetch the logs; their header comments explain each one. Prove the issue's criteria: no warnings, full doctests, the issue's GPU checks with 0 validation errors on Vulkan and D3D12, and the phase smoke scene.
2. **Review.** An independent read of the diff against the issue, the plan and the clean room (§19), looking for concrete defects.
3. **Fix** the real findings and re-prove from a clean state.
4. **Landing check** on the cloud test machine (Windows VM 4 with the private Mesa library) or the RTX 4080 SUPER machine. Apply the diff to current master, build, run the issue's landing runs, and name the machine in the issue comment (lead decision 2026-11-16).
5. **Land** on master. Use "Closes #N" only when every build criterion is met. Metal and macOS builds wait for the lead's Mac runs, so most issues land as progress. Then post the issue comment, refresh the tracker, update this file and delete the WIP branch.

The lead does not want questions. Take the recommended option, or follow UE 5.9, record the choice, and report it. Ask only about spending, accounts, the lead's own sign-offs, or publishing anything new.

## State at the handover

- Master: see `git log`. Plan v3.9.16.
- Landed on 2026-11-19, all as progress unless noted:
  - #193 TSR async compute, #218 VSM non-Nanite raster and #167 the path tracer (checked together on the cloud L4 and T4 test machines), with plan v3.9.15.
  - #299 closed (the D3D12 front-layer depth target).
  - When the lead closed the lanes, #158 material shading bins, #295 AVBOIT, #145 RT visibility policy, #221 VSM product surface and #222 VSM projection and SMRT, with plan v3.9.16 (LQ6 decided: translucent casters shadow as opaque, as UE 5.9 does). This stack had one combined build and full-doctest pass on Linux (GCC) and Windows (MSVC, gn4), and no landing check.
- Landed on 2026-11-18: the master repair (#130, #155 and #219 reopened for Metal), #122 and #293 closed (the private Mesa NIR library 25.3.1-gn4), #286 (Hardware Ray Tracing Mode), plans v3.9.13 and v3.9.14.
- New issues: #297 to #301.

## Open work from the closed lanes (restart these first)

No lane is running. Each item below is on master; what is open is in the issue's landing comment.

| Issue | What is open |
|---|---|
| #222 VSM projection and SMRT | Its build criteria (0 validation errors on the smoke scene and the Flythrough with VSM on, the pool-sampling fixture, the penumbra check), a review and a landing check. It was merged before its build step finished. |
| #221 VSM product surface | Its review fixes passed builds and doctests, but their GPU runs never finished. Also a landing check. |
| #145 RT visibility policy | Its port onto #286 and #167 has had only builds and doctests: rerun the GM-4 matrix, the alpha fixture and the hit-group mode rows on Vulkan and D3D12. Three follow-up issues to file (alpha-tested skinned and NextGeo alpha RT shadows, `discard` in #130's any-hit stage). |
| #158, #295 | The four-machine landing check of the merged stack did not run. #295 also has the front-layer rerun on D3D12 with AVBOIT on (from #299). |
| Merged stack | No Windows GPU run of the whole stack. A first job: rerun each lane's main fixture once on Vulkan and D3D12. |

## Ready to start (phase order)

- **#217** VSM x Nanite, and **#150** displacement material output.
- **#301** RD read-only depth attachments (GT-7 waits on it).
- **#298** headless import abort at shutdown, and **#297** D3D12 legacy SDFGI drift.
- Blocked until their blocker lands: #294, #296 and #141.

## Waiting on the lead

- **#98 DLSS license sign-off.** It holds the DLSS track, plus #236's UE-side Frame Generation, MFG, Reflex and Nsight.
- **Mac runs.** Every Metal build and run waits for the lead.
- **AMD test machine.** A cloud Radeon PRO V710 (#280).
- **RTX PRO 6000 gcloud quota:** the requests are pending, with 0 granted.

## Known issues and risks

- **PresentMon writes no rows on the RTX 4080 SUPER machine** (#202, #236; possibly no active monitor). It must be fixed before PQ.
- **#236's UE side is render-thread bound in editor game mode**, so it is not GPU-bound. PQ decides how to handle it.
- **The RTX 4080 SUPER machine is shared** with another project's UE automation. Never stop or touch anything that isn't GodotNext's own there.
- **A headless `--import` sometimes aborts at shutdown** ("Parameter \"singleton\" is null" in `is_cmdline_mode`) after the reimports finish, also on older trees. It needs its own issue and a backtrace from a debug build.
- **The repository is public.** No local paths, hosts, IPs, VM names or account names go in tracked files, commit messages or issue comments.
- **Decisions made on 2026-11-16 under the lead's rule** are recorded in the plan's §18.1 and the issue comments. The lead can overrule any of them.
