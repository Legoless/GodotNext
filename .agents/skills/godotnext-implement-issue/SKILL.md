---
name: godotnext-implement-issue
description: Picks a ready GodotNext issue, implements it Metal-first, passes the PR basics, commits it to master with "Closes #N", and refreshes the build tracker. Use when starting, building or landing any task from the Legoless/GodotNext issue tracker (NextGI, NextGeo, RHI, upscaling or parity work).
---

# Implement a GodotNext issue

GodotNext is a Godot fork that brings full UE 5.8 rendering parity to v1. Every task is a GitHub issue on `Legoless/GodotNext`, written from the plan in `docs/godotnext/nextgi-nextgeo-plan.html`. This skill takes one issue from "ready" to "closed on master".

## 1. Pick a ready issue

An issue is **ready** when everything in its "Blocked by" list is closed.

- Open `docs/godotnext/tracker/index.html`. Its "Next up" list ranks ready tasks by how much work waits on them.
- Or check one issue from the command line:
  ```
  python3 .agents/skills/godotnext-issues/scripts/gh_issues.py blockers N
  ```
- Prefer work the M5 Max can do. Plan §24 puts Metal first:
  - P0 and P1 are built on Metal.
  - Vulkan/NVIDIA (P2), D3D12 (PD), DLSS (PU, PU+) and anything NVIDIA-only need the RTX 4080 SUPER machine (see `godotnext-fleet`).
- Gates (`type:gate`), spikes (`type:spike`) and every `phase:PQ` task except the PQ preparation tasks run only in the final test phase, after #225 closes. Don't start them during the build.

Mark the issue as yours so the tracker shows it in progress:
```
gh issue edit N -R Legoless/GodotNext --add-assignee @me
```

## 2. Read the issue and the plan

Every task body has the same sections:

| Section | Use |
|---|---|
| Summary | What to build |
| Plan reference | Read these plan sections before coding (`#steps` is §12, `#parity` §21, `#build-first` §22, `#ue-bar` §23, `#metal-first` §24, `#legal` §19) |
| Dependencies | Generated; don't edit by hand |
| Implementation steps | The intended approach, with file and line pointers into the fork |
| Acceptance criteria | "PR basics" (do now) and "Measured in PQ" (leave unticked; PQ runs them) |
| Validation | The commands to run |
| Parallelization | Lane, file conflicts and what to rebase after |
| Out of scope | What not to touch |

If the issue and the plan disagree, the plan wins. Note the disagreement in your commit message, and fix the issue body.

## 3. Build it

- **Code style:** follow Godot's. Use `.clang-format`, and run pre-commit (`pre-commit run --files <files>`) if it's installed.
- **Tests:** add or extend doctest tests under `tests/` for new code. Never skip a test, never add conditional skips, and never mark a failing test as expected.
- **API docs:** after changing a class's API, regenerate them with `--doctool` (see below).
- **Don't measure:** don't run or tune against PQ measurements (timings, quality corpora, soaks). Under build-first (§22), fixed millisecond values are report-only targets at most.

Build and test on the M5 Max (§13.1):

- **SCons needs Python 3.10 or newer.** The system `python3` on macOS is 3.9. Use a virtualenv:
  ```
  /opt/homebrew/bin/python3 -m venv ~/.cache/godotnext-venv
  ~/.cache/godotnext-venv/bin/pip install scons
  ```
- **No MoltenVK SDK means `use_volk=yes`.** `dev_mode=yes` doesn't add `.dev` to the binary name.

```
SCONS_CACHE=$HOME/.cache/godotnext-scons ~/.cache/godotnext-venv/bin/python -m SCons platform=macos target=editor dev_mode=yes arch=arm64 use_volk=yes -j8
./bin/godot.macos.editor.arm64 --headless --test --force-colors
./bin/godot.macos.editor.arm64 --headless --test --force-colors --test-case='*NextGI*'
./bin/godot.macos.editor.arm64 --doctool --headless
git diff --exit-code -- doc/classes
```

Run the repository's style checks on every file you changed. They are the same hooks CI runs (clang-format, ruff, mypy, codespell in US English, file-format, CODEOWNERS):
```
git ls-files -m -o --exclude-standard | xargs uvx prek run --files
```
Codespell rewrites files in place. Check its edits, because it can "correct" technical acronyms.

**Doctests must pass on every CI runner, including Linux and Windows without extra tools.** Never make a doctest depend on a locally installed tool such as `spirv-val`. Put that kind of check in a script under `misc/scripts/`. `godotnext_rq_toolchain_check.sh` is an example.

## 4. Pass the PR basics (§22.2)

These are the only tests during the build:

1. It builds on every target it touches. The M5 Max covers macOS; CI covers the rest.
2. CI, unit tests and doctests pass, with tests added for new code.
3. Zero validation errors on the phase smoke scene. On the M5 Max:
   ```
   MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ./bin/godot.macos.editor.dev.arm64 --path godotnext/testbed --rendering-driver metal <smoke scene>
   ```
   §22.3 lists each phase's smoke scene. Use the P0 testbed when the phase has none yet.
4. The smoke scene renders without crashing on the M5 Max.
5. A change to Vulkan or D3D12 code also needs a run on the RTX 4080 SUPER, once that machine is in use (§24.3).

## 5. Land it

The lead chose this on 2026-11-03: land work as a commit straight to master, with no branch or PR unless the lead asks for one.

```
git add <files>
git commit -m "Short imperative summary (#N)" -m "What changed and why, in two or three plain sentences." -m "Closes #N"
git push origin master
```
- No AI attribution or `Co-Authored-By` lines.
- "Closes #N" closes the issue when the push reaches master.
- Tick the PR-basics boxes you met in the issue body. Leave "Measured in PQ" boxes for PQ.
- Then refresh the tracker with `godotnext-update-tracker`, in the same push or the next one.

## Guardrails

- **The upstream remote:** always pass `-R Legoless/GodotNext` to `gh`. A bare `gh` in this checkout talks to `godotengine/godot`.
- **The clean room (§19):**
  - Never read Unreal Engine source or shaders, or any UE plugin source (including NVIDIA's UE DLSS plugin).
  - Never read NvRTX, or the DLSS sample or Donut source.
  - Never use decompiled binaries or model output that reproduces such code.
  - UE 5.8 runs only as a black-box binary for the comparison (§23.7), and UE projects stay outside this repository.
- **The NVIDIA SDK:** never commit NVIDIA SDK files. The DLSS SDK stays developer-supplied, outside the tree, and CI uses the mock backend.
- **Scope:** don't change scope silently. If the work shows the plan is wrong, change the plan with `godotnext-plan-revision` first.
- **Third-party code:** it needs its upstream URL, version, license and modifications recorded before it merges (§19, vendoring gate).
