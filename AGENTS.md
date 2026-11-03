# GodotNext agent guide

GodotNext is a fork of Godot Engine (MIT) that brings full Unreal Engine 5.8 rendering parity to v1. That means NextGI (Lumen-class global illumination), NextGeo (Nanite-class geometry), virtual shadow maps, MegaLights, a path tracer, TSR and full DLSS.

## Where things are

- **The plan:** `docs/godotnext/nextgi-nextgeo-plan.html`. It's the source of truth.
  - §12 and §21: phases and gates.
  - §22: build-first execution and the final test phase (PQ).
  - §23: the test fleet and the UE 5.8 bar.
  - §24: the Metal-first build order.
  - §19: the clean room.
- **The work breakdown:** issues on `Legoless/GodotNext`. The roadmap tracker is #2, the phase epics are its sub-issues, and the final test phase is #224.
- **The build tracker:** `docs/godotnext/tracker/index.html`, the project-manager view of progress.

## Rules

- **Name the repository.** Pass `-R Legoless/GodotNext` to every `gh` command. This checkout's upstream remote is `godotengine/godot`.
- **Build first, test at the end (§22).** During the build only the PR basics run: build, CI and tests, zero validation errors and a smoke render on the phase smoke scene. Every gate, spike, timing, quality and soak run happens in PQ. Leave "Measured in PQ" boxes unticked.
- **Metal first (§24).** P0 and P1 are built and smoke-tested on the M5 Max. Vulkan/NVIDIA (P2), D3D12 (PD) and DLSS need the RTX 4080 SUPER machine.
- **The clean room (§19):**
  - Never read Unreal Engine source or shaders, UE plugin source (including NVIDIA's UE DLSS plugin), NvRTX, or the DLSS sample and Donut source.
  - Never commit NVIDIA SDK files.
  - UE 5.8 runs only as a black-box binary for the performance comparison, and UE projects stay outside this repository.
- **Tests.** Add tests for new code, and never skip, disable or conditionally skip a test.
- **Landing work.** The lead decided on 2026-11-03: commit finished issue work straight to master with "Closes #N" and push. No branches or PRs unless the lead asks. No AI attribution or `Co-Authored-By` lines.
- **One source of truth.** A plan change is carried to the issues and the tracker in the same change set. After work lands, refresh the tracker snapshot.
- **The tracker** shows no timing estimates.

## Skills

Project skills live in `.agents/skills/`. Claude Code reads the same folder through `.claude/skills`.

| Skill | Use it to |
|---|---|
| [godotnext-implement-issue](.agents/skills/godotnext-implement-issue/SKILL.md) | Pick a ready issue, build it Metal-first, pass the PR basics and land it on master |
| [godotnext-update-tracker](.agents/skills/godotnext-update-tracker/SKILL.md) | Refresh the tracker data after progress, or change the tracker page |
| [godotnext-plan-revision](.agents/skills/godotnext-plan-revision/SKILL.md) | Turn a lead decision into a new plan version and carry it to the issues and the tracker |
| [godotnext-issues](.agents/skills/godotnext-issues/SKILL.md) | Create, link, move and bulk-edit issues with the project's conventions and helper script |
| [godotnext-fleet](.agents/skills/godotnext-fleet/SKILL.md) | Use the M5 Max, the RTX 4080 SUPER machine, the RTX 5090 machine and the Linux VM, and check HTML pages in headless Chrome |

For Godot's own engine conventions (code style, tests, docs), follow `CONTRIBUTING.md` and the existing code.
