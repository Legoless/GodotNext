# GodotNext handoff

The current state of the build and what to do next. It is written for any agent: Claude, Gemini or another. Update this file whenever work stops and after each landing, in the same change set as the landing or the tracker refresh. Machine-specific details (exact local paths, VM names and zones, private library folders, saved lane results) are in the git-ignored companion `local/handoff-private.md`.

**Last updated:** 2026-11-19, midday (Claude). Cloud VMs and lanes are running.

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

- Master: see `git log`. Plan v3.9.15.
- Landed on 2026-11-19, as progress (Metal criteria open): #193 TSR async compute (08ab628969), #218 VSM non-Nanite raster (49194956b6) and #167 the progressive path tracer (6a35db0f9d), with plan v3.9.15 (4f6f56cbeb). They were checked together, as one stacked tree, on the cloud L4 and T4 test machines. New follow-ups: #300 (XR async, post-v1) and #301 (RD read-only depth attachments, blocks GT-7).
- Landed on 2026-11-18: the master repair (1969d8d986; #130, #155 and #219 reopened for Metal), #122 and #293 (closed; the private Mesa NIR library 25.3.1-gn4), #286 as progress (Hardware Ray Tracing Mode), plans v3.9.13 and v3.9.14. Filed #297, #298 and #299.
- Earlier landings: see the tracker and `git log`.

## In progress (restart these first)

Each lane works in a local worktree under `.claude/worktrees/`, builds on the cloud VMs and keeps notes in its private lane folder (see `local/handoff-private.md`). Nothing is committed until it lands.

| Issue | Where it stopped | Next step | Machines |
|---|---|---|---|
| #158 Material shading bins | Built, reviewed, fixed (on bc37fd4781) | Port onto master with #145 and #295, landing check, land | Windows VM 1, Linux C |
| #145 RT visibility policy | Built, reviewed, fixed (on bc37fd4781); conflicts with #286 | Port (wire its retrace to #286's material-retrace consumer bit), landing check, land; plan text for LQ6 and GM-4 | Windows T4 VM, Linux E |
| #295 AVBOIT | Built, reviewed, fixed (on bc37fd4781) | Port, landing check, land | Windows VM 4, Linux E |
| #222 VSM projection and SMRT | Build step | Review, fix, landing check | Windows VM 1, Linux E |
| #221 VSM product surface | Build step | Review, fix, landing check | Windows VM 4, Linux E |
| #299 D3D12 front-layer depth target | Fix step | Landing check | Windows T4 VM, Windows VM 4, Linux E |

## Ready to start (phase order)

- **#217** VSM x Nanite (#218 has landed).
- **#150** Displacement material output, after #158 lands (same resolve code).
- **#298** headless import abort at shutdown, and **#297** D3D12 legacy SDFGI drift (harness).
- These open once their blocker lands: #294 (after #129), #296 (after #152 and #160), #141 (after #145, #139 and #221), #301 (after #193: ready now).

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
