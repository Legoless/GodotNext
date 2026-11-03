---
name: godotnext-issues
description: Reads and changes the GodotNext work breakdown on GitHub (Legoless/GodotNext issues, labels, milestones, epics, sub-issues and "blocked by" links) following the project's conventions, with a helper script for links and generated Dependencies sections. Use when creating, retitling, moving, linking, closing or bulk-editing GodotNext issues, or when carrying a plan change into the issue tracker.
---

# Work with the GodotNext issues

The plan's work breakdown lives on GitHub as issues on `Legoless/GodotNext`. The roadmap tracker is #2, and its sub-issues are the phase epics.

## The trap: always name the repository

This checkout's upstream remote is `godotengine/godot`, so a bare `gh` command goes there. Always pass `-R Legoless/GodotNext`, or `repos/Legoless/GodotNext/...` for `gh api`.

## Conventions

| Thing | Rule |
|---|---|
| `type:` label | `task`, `epic`, `gate` or `spike` |
| `phase:` label | One per issue: `P0`, `P1a`, `P1b`, `P2`, `P3`, `PU`, `P4`, `P5`, `PR`, `PD`, `PS`, `PL`, `PV`, `PM`, `PP`, `PN`, `PT`, `PU+`, `PQ` (final test), `PX` (post-v1) |
| `lane:` label | One parallel workstream per issue (`rhi-metal`, `rhi-vulkan`, `nextgi-core`, `content`, `harness`, `lead`, …) |
| `size:` and `area:` labels | `size:XS`–`size:XL`; any number of `area:` labels |
| Milestones | M0–M8 are code-complete checkpoints, not releases. "MQ - Final test and v1 sign-off" holds PQ. Post-v1 holds PX |
| Epics | Each phase has an epic; its tasks are its sub-issues. Gates, spikes and PQ tasks are sub-issues of the PQ epic, #224 |
| Blocked-by links | Only from work to the work whose code it needs (task to task, or task to epic) |
| No links | Task → gate, task → spike or gate → gate |
| Gates and spikes | Wait on #225, "PQ entry", which waits on every build epic, the UE comparison set and the RTX 5090 machine |
| Superseded issues | Comment with the replacement, then close as "not planned" |

## Body format

Each task body has these sections, in this order:
1. `## Summary`
2. `## Plan reference`, linking plan sections with anchors
3. `## Dependencies`
4. `## Implementation steps`
5. `## Acceptance criteria`, split into "PR basics" and "Measured in PQ"
6. `## Validation`
7. `## Parallelization`
8. `## Out of scope`

Epics also have `## Children`. `## Dependencies` is generated from the links in this form:
```
**Blocked by:**
- #10 — <title>        (or "- nothing — can start now")

**Unblocks:**
- #19 — <title>        (or "- nothing directly")
```
Never edit it by hand. Change the links, then regenerate it with the script.

## Helper script

```
G=.agents/skills/godotnext-issues/scripts/gh_issues.py
python3 $G blockers 20                    # what #20 waits on and what waits on it
python3 $G block 28 --by 47               # #28 now waits on #47
python3 $G unblock 39 --by 9              # remove that link
python3 $G sub 1 47 48                    # move #47 and #48 under the P0 epic #1
python3 $G deps-section 28 47 --dry-run   # preview the regenerated Dependencies sections
python3 $G deps-section 28 47             # write them
python3 $G snapshot /tmp/gn-snap          # issues.json + links.json for every issue
```
Every write command takes `--dry-run`. Calls retry transient network errors, because the connection to api.github.com can be flaky.

The REST endpoints it wraps:
- `POST repos/Legoless/GodotNext/issues/N/dependencies/blocked_by` with `{"issue_id": <id>}`;
- `DELETE .../dependencies/blocked_by/<id>`;
- `GET .../dependencies/blocking`;
- `POST .../issues/P/sub_issues` with `{"sub_issue_id": <id>, "replace_parent": true}`.

These take the issue's database `id`, not its number.

## Bulk changes

For a plan revision or any change touching many issues:
1. Snapshot first.
2. Write the intended state.
3. Dry-run, then apply.
4. Snapshot again, and verify titles, bodies, milestones, labels, parents and links against the intended state.
5. Regenerate the Dependencies sections of every issue whose links or neighbours' titles changed.

Before re-running any step that posts comments, check the comments already there.

After issue changes, refresh the tracker snapshot (`godotnext-update-tracker`).
