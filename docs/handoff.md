# GodotNext handoff

The current state of the build and what to do next. Update this file whenever work stops and after each landing, in the same change set as the landing or the tracker refresh. Machine-specific details (exact local paths, workflow run IDs, VM names and zones, private library folders) are in the git-ignored companion `local/handoff-private.md`.

**Last updated:** 2026-11-16, about 17:00 CET. Every lane was stopped and every cloud VM was shut down for an offline period.

## Resume in five steps

1. Read this file, then `local/handoff-private.md`.
2. `git fetch` and confirm master is at or after `c88a8a6dbb`. Refresh the tracker (`python3 docs/godotnext/tracker/export.py`).
3. Start only the cloud VMs the next lanes need (table below). Stopped VMs get new external IPs, so write them into `local/fleet.env` after starting.
4. Restart the in-progress lanes in the order of the "In progress" table. Each one has uncommitted work in a local worktree, plus saved build and review results.
5. Start ready issues in phase order on the machines that are free (the "Ready" table).

## State at the stop

- Master: `c88a8a6dbb`. Plan: v3.9.12 (`4a44d8b50c`).
- Tracker: 132 of 229 build tasks done, 0 of 18 gates (gates are measured in PQ by design).
- Every lane is stopped and nothing of GodotNext's is running on the RTX 4080 SUPER machine. All cloud VMs are stopped.

### Landed on 2026-11-16

Closed: #272, #277, #278, #279, #287.

Landed as progress (open for Metal/macOS, PQ boxes or later steps): #118, #121, #123, #156, #162, #176 (and its all-vendor follow-up), #177, #178, #179, #203, #205, #210, #219, #220, #229–#236 (the UE 5.9 comparison ports and settings rules), #275, #276, #283, #285.

Plan revisions v3.9.5 to v3.9.12. They cover the lead decisions of 2026-11-16 (§18.1) and the new §26 (hardware support follows UE 5.9). They added issues #280–#287 and #292–#296.

## In progress (restart these first)

Each worktree is under `.claude/worktrees/` in the local checkout. The unfinished work is also pushed as WIP branches, so it survives without the local checkout. They are unreviewed and partly unbuilt: never merge them as they are. Restart a lane from its branch, or from the worktree if it is still there, then land through the normal checks.

| Issue | WIP branch |
|---|---|
| #195 | `feature/wip-195-tsr-d3d12-metal` |
| #130 | `feature/wip-130-material-hit-groups` |
| #219 follow-up | `feature/wip-219-vsm-coarse-pages` |
| #155 | `feature/wip-155-raster-bins` |
| #122 + #293 | `feature/wip-122-293-mesa-gn4` (engine side only; the private Mesa library is not in the repository) |

#218 had no edits yet, so it has no branch.

| Issue | Worktree | Where it stopped | Next step | Machines |
|---|---|---|---|---|
| #195 TSR on D3D12 and Metal | `p195` (base `1e574116f6`) | Build done (all 5 presets clean under D3D12 GPU-based validation on an L4). Review: 0 blockers, 5 minors. Fix step was running | Fix step: the 5 minors. Lead choice: a device missing a TSR storage format falls back to the plain upscaler with a logged reason, not black frames; the Metal driver's format query stays as is. Then rebase and a landing check on the cloud test machine | Windows VM 4, Linux C |
| #130 Material hit groups | `p130` (base `beab45e691`) | Build done (Vulkan clean on two L4 VMs). Review: 4 majors and 12 minors, none affecting default rendering. Fix step was running | Fix step, then rebase onto master (#121 landed and changed the D3D12 RT side) and the landing check | Linux D, Windows VM 2 |
| #219 follow-up: always-resident coarse VSM pages | `p219c` (base `531306c6b9`) | Build stopped by a usage limit; partial edits in the worktree | Build, review and fix. Lead choice (follow UE): every shadowed light keeps coarse pages, handed out first, and the 25% pool run must overflow with zero sampled-but-unmapped pages. Also update the texts of #219 step 2, #216 and #215 | Windows VM 2, Linux E |
| #218 VSM non-Nanite path | `p218` (base `531306c6b9`) | Build stopped by a usage limit; no edits yet | Build, review and fix | Windows T4 VM, Linux C |
| #155 Programmable raster bins | `p155` (base `928dad38b6`) | Build stopped twice by usage limits; partial edits | Build, review and fix; rebase first (master moved a lot) | Windows VM 1, Linux C |
| #122 + #293 Private Mesa NIR 25.3.1-gn4 | `pgn4` (base `531306c6b9`) | Build stopped by a usage limit; partial edits, and possibly partial Mesa work on the Windows Mesa build VM | gn4 = gn3 + DXIL mesh/amplification shaders (#122) + `nonUniformIndex` for non-uniform bindless reads (#293). **The D3D12 container format version for gn4 is 6** (#121 renumbered: 4 for gn2/upstream, 5 for gn3) | Windows VM 3 (Mesa build), Windows T4 VM, Linux D |

## Ready to start (phase order)

- **#145** RT visibility policy shared by RT shadows, MegaLights and SRC (PM; unblocked by #121).
- **#295** AVBOIT, Experimental and off by default (PL; no blockers).
- **#221** VSM product surface and **#222** VSM projection and SMRT soft shadows (PV). Start them after the #219 follow-up and #218 settle, so three lanes don't collide in the same VSM files.
- These open once their blocker lands: #158 (after #155), #286 and #167 (after #130), #193 (after #195), #294 (after #129, #130 and #286), #296 (after #152 and #160).

## Waiting on the lead

- **#98 DLSS license sign-off.** It holds the DLSS track (#99 and the rest), plus #236's UE-side Frame Generation, MFG, Reflex and Nsight.
- **Mac runs.** Nothing has been built or run on Metal yet. Every landed issue's Metal half and the macOS builds wait for this.
- **AMD test machine.** A cloud Radeon PRO V710 (#280), being set up by the lead.
- **RTX PRO 6000 quota** on gcloud: the requests for europe-west4 and us-central1 are still at 0 granted. The VM script is ready once quota exists.
- **Orchestrator VM** (optional): an always-on VM running the agent loop, so lanes continue while the lead's Mac is offline. It was proposed on 2026-11-16 and not yet approved.

## Known issues and risks

- **Usage limits.** The account hit its spend limit four times on 2026-11-16, at about 10:00, 14:41, 14:58 and 15:45. Each time it lifted within minutes, but it stopped running agents. With more than ten lanes at once, expect this again.
- **PresentMon writes no rows on the RTX 4080 SUPER machine** (noted on #202 and #236; possibly no active monitor). It must be fixed before PQ.
- **#236's UE side is render-thread bound in editor game mode**, so it is not GPU-bound. PQ decides how to handle it.
- **Resuming workflows:** editing one item's prompt in a multi-item workflow re-runs every later agent call. To restart one unfinished item, run it as a new single-item workflow.

## Decisions made under the lead's "recommended option or follow UE" rule

The lead asked on 2026-11-16 for no more questions: take the recommended option, or follow UE, and report the choice. The plan's §18.1 and the issue comments record each one, for example:
- #219's mip footprint and overflow;
- #121's record padding;
- #283's AMD APU detection;
- the UE column-fairness choices;
- LQ14's platform targets as post-v1.

The lead can overrule any of them.
