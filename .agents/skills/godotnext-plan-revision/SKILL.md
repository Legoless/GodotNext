---
name: godotnext-plan-revision
description: Revises the GodotNext implementation plan (docs/godotnext/nextgi-nextgeo-plan.html) for a lead decision or scope change, records it as a new plan version, and carries the change to the GitHub issues and the build tracker. Use when the lead changes scope, order, hardware, test rules or any other plan decision, or when an implementation finding shows the plan is wrong.
---

# Revise the plan

The plan is one HTML file, `docs/godotnext/nextgi-nextgeo-plan.html`, and it's the source of truth. The GitHub issues and the build tracker follow it. Each lead decision becomes a new plan version: v3.1 build-first, v3.2 test fleet and UE bar, v3.3 Metal first.

## Section map

| id | Section |
|---|---|
| `goal`, `success`, `verdict` | §1–§3: goal, success criteria and budgets (§2.1), tier model and v1 scope |
| `current`, `decisions`, `architecture` | §4–§6: snapshot audit, decisions D1–D22, target architecture |
| `nextgeo`, `nextgi`, `shared`, `platforms`, `files` | §7–§11: designs, shared foundations, platforms, file map |
| `steps`, `validation`, `estimates` | §12–§14: phases and gates, validation plan and scenes, estimates and the milestone table |
| `risks`, `alternatives`, `backlog`, `open` | §15–§18: risks, rejected alternatives, first backlog, open questions and §18.1 lead decisions |
| `legal`, `sources` | §19–§20: clean room and licences, sources |
| `parity` … `parity-upscaling` | §21: the UE 5.8 parity program (PR, PD, PS, PL, PV, PM, PP, PN, PT, PU+) |
| `build-first` | §22: build-first execution, PR basics, smoke scenes, PQ, and the integrator defaults (§22.10) |
| `ue-bar` | §23: the two-card test fleet and the ≤1.20× UE 5.8 bar |
| `metal-first` | §24: the Metal-first build order |
| `revisions`, `evidence` | Appendix A revision history, Appendix B evidence index |

The head (before `goal`) holds the version chip, the one-page summary, the milestone table and the table of contents.

## Steps

1. **Capture the decision.**
   - Quote the lead's words verbatim, and record any follow-up answers.
   - Write a short spec for yourself: what changes, what explicitly doesn't, and the exact issue changes.
   - Ask the lead before deciding anything that is theirs to decide.
2. **Write the governing section.** A decision that changes how other sections read gets its own `<section id="...">` titled "N. Title (vX.Y)". Open it with the quote and a callout saying what it governs and what stays unchanged. Smaller decisions can edit their section in place.
3. **Edit every place the decision touches:**
   - the version chip in the head ("X.Y — what changed; earlier versions stand");
   - the table of contents, and the "Details:" link line, for a new section;
   - a §18.1 row: "Decided YYYY-MM-DD" with the quote, and mark rows it replaces as superseded;
   - a §15 risk row if the decision brings a new risk;
   - the Appendix A revision row "vX.Y (YYYY-MM-DD)", appended after the newest row (the table runs oldest first);
   - every statement elsewhere that now contradicts the decision. Search the plain text for the old terms, and fix each hit outside history rows.
4. **Keep the style.**
   - Plain, short, concrete sentences; no marketing.
   - Sections are cited as §N, and issue keys and numbers are written plainly.
   - Never invent figures, settings or UE console variables. Write "to confirm in <issue>" instead.
   - Don't rewrite history tables or older revision rows.
5. **Validate.**
   ```
   python3 .agents/skills/godotnext-plan-revision/scripts/check_html.py docs/godotnext/nextgi-nextgeo-plan.html
   ```
   It must print `no tag errors` and `unclosed: []`. Then search once more for the old terms.
6. **Carry it through.**
   - Issues: titles, bodies, phases, milestones, epics and links, with `godotnext-issues`.
   - Tracker: the page's `AREAS`, `MILESTONES` and `GATE_ORDER` constants and the snapshot, with `godotnext-update-tracker`.
7. **Commit to master and push.** Use the subject "Update NextGI and NextGeo implementation plan to vX.Y", with a short body listing what changed and what didn't.

## Editing many sections at once

For large revisions, or several agents working in parallel, split the plan into one fragment per top-level section. Each agent edits only its own fragments.

```
S=.agents/skills/godotnext-plan-revision/scripts
python3 $S/plan_split.py docs/godotnext/nextgi-nextgeo-plan.html /tmp/plan-frag
# edit /tmp/plan-frag/NN-<id>.html; a new section is a new file named in order.json
python3 $S/plan_join.py /tmp/plan-frag docs/godotnext/nextgi-nextgeo-plan.html
python3 $S/check_html.py docs/godotnext/nextgi-nextgeo-plan.html
```
Splitting and joining are lossless: join(split(plan)) is byte-identical.

## Standing rules a revision must not break without a lead decision

- **Build first (§22).** Only PR basics run during the build. Every gate and spike runs together in PQ, and v1 signs off at MQ. M0–M8 are code-complete checkpoints, not releases.
- **The fleet and the bar (§23).** The RTX 4080 SUPER and the RTX 5090 (second machine) run every NVIDIA line, and the M5 Max runs Metal. Timing lines pass at ≤1.20× UE 5.8 on the same machine; fixed ms values are report-only.
- **Metal first (§24).** P0 and P1 are built on the M5 Max; P2 is Vulkan/NVIDIA.
- **The clean room and licences (§19).**
- **Defaults and decisions.** The ten §22.10 integrator defaults and decisions D1–D22.
