# GodotNext handoff

The current state of the build and what to do next. It is written for any agent: Claude, Gemini or another. Update this file whenever work stops and after each landing, in the same change set as the landing or the tracker refresh. Machine-specific details (exact local paths, VM names and zones, private library folders, saved lane results) are in the git-ignored companion `local/handoff-private.md`.

**Last updated:** 2026-11-17, night (Gemini active). Cloud VMs and active lanes are running under Gemini coordination.

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

- Master: see `git log`. Plan v3.9.12. The tracker showed 132 of 229 build tasks done; the 18 gates are measured in PQ by design.
- Landed on 2026-11-16 / 2026-11-17:
  - **Closed:** #272, #277, #278, #279, #287.
  - **Landed as progress:** #118, #121, #123, #156, #162, #176, #177, #178, #179, #195, #203, #205, #210, #219, #220, #229–#236, #275, #276, #283, #285.
  - **Plans:** v3.9.5 to v3.9.12.

## In progress (restart these first)

Each has a WIP branch, never to be merged as is, and a local worktree under `.claude/worktrees/`.

| Issue | WIP branch | Where it stopped | Next step | Machines |
|---|---|---|---|---|
| #130 Material hit groups | `feature/wip-130-material-hit-groups` | Rebased onto master (#121), review fixes applied, proven on Linux Vulkan and Windows D3D12 HWRT (`gn3`) | Finalize report and land | Linux D, Windows VM 2 |
| #219 follow-up: always-resident coarse VSM pages | `feature/wip-219-vsm-coarse-pages` | Partial edits, not built | Build, review and fix. Lead choice (follow UE): every shadowed light keeps coarse pages, handed out first, and the 25% pool run must overflow with zero sampled-but-unmapped pages. Also update the texts of #219 step 2, #216 and #215 | Windows VM 2, Linux E |
| #218 VSM non-Nanite path | no branch (no edits yet) | Not started | Build, review and fix | Windows T4 VM, Linux C |
| #155 Programmable raster bins | `feature/wip-155-raster-bins` | Partial edits, not built | Rebase, then build, review and fix | Windows VM 1, Linux C |
| #122 + #293 Private Mesa NIR 25.3.1-gn4 | `feature/wip-122-293-mesa-gn4` (engine side only) | Partial edits; possibly partial Mesa work on the Windows Mesa build VM | Build gn4 = gn3 + DXIL mesh/amplification shaders + `nonUniformIndex` for non-uniform bindless reads. **The D3D12 container format version for gn4 is 6** (4 is gn2/upstream, 5 is gn3). The Mesa fork, its patches and the packed library are private and never committed | Windows VM 3 (Mesa build), Windows T4 VM, Linux D |

## Ready to start (phase order)

- **#145** RT visibility policy shared by RT shadows, MegaLights and SRC (unblocked by #121).
- **#295** AVBOIT, Experimental and off by default (no blockers).
- **#221** VSM product surface and **#222** VSM projection and SMRT soft shadows. Start them after the #219 follow-up and #218.
- **#193** TSR async compute (code unblocked by #195 landing as `32531a8b35`).
- These open once their blocker lands: #158 (after #155), #286 and #167 (after #130), #294 (after #129, #130 and #286), #296 (after #152 and #160).

## Waiting on the lead

- **#98 DLSS license sign-off.** It holds the DLSS track, plus #236's UE-side Frame Generation, MFG, Reflex and Nsight.
- **Mac runs.** Every Metal build and run waits for the lead.
- **AMD test machine.** A cloud Radeon PRO V710 (#280).
- **RTX PRO 6000 gcloud quota:** the requests are pending, with 0 granted.

## Known issues and risks

- **PresentMon writes no rows on the RTX 4080 SUPER machine** (#202, #236; possibly no active monitor). It must be fixed before PQ.
- **#236's UE side is render-thread bound in editor game mode**, so it is not GPU-bound. PQ decides how to handle it.
- **The RTX 4080 SUPER machine is shared** with another project's UE automation. Never stop or touch anything that isn't GodotNext's own there.
- **The repository is public.** No local paths, hosts, IPs, VM names or account names go in tracked files, commit messages or issue comments.
- **Decisions made on 2026-11-16 under the lead's rule** are recorded in the plan's §18.1 and the issue comments. The lead can overrule any of them.
