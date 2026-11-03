---
name: godotnext-update-tracker
description: Refreshes and edits the GodotNext v1 build tracker page (docs/godotnext/tracker), the project-manager view of how much of v1 is built. Use after issues close or progress lands, or when changing the tracker's layout, labels or wording.
---

# Update the build tracker

The tracker is a standalone page in the repository, `docs/godotnext/tracker/index.html`. Project managers open it in a browser to see:
- how much of v1 is built;
- what each phase delivers;
- the road through milestones M0–M8 to the final test and v1;
- what's next, and the final-test status.

| File | Role |
|---|---|
| `index.html` | The page: HTML, CSS and JavaScript, no build step |
| `tracker-data.js` | Snapshot of every issue, its "blocked by" links, epic children and the day-by-day history |
| `history.json` | The running day-by-day record (one row per UTC day) |
| `export.py` | Rebuilds the snapshot from GitHub with `gh` |

Opening the page also reads the current issue states from GitHub's public API (about three requests, no sign-in). The committed snapshot still supplies the dependency links and the history.

## Refresh the data

The lead's choice: the tracker is updated by hand on master once progress lands.

```
python3 docs/godotnext/tracker/export.py
git add docs/godotnext/tracker/tracker-data.js docs/godotnext/tracker/history.json
git commit -m "Update build tracker data"
git push origin master
```
Do this when issues close, as part of landing the work (see `godotnext-implement-issue`), not on a timer. Running it twice on the same day replaces that day's history row.

## How statuses are worked out

These rules live in `export.py` and the page script; keep them the same in both:

- **done:** closed as completed. Issues closed as not planned are left out.
- **in progress:** open and assigned, or labelled `status:in-progress`.
- **ready:** open, with every "blocked by" issue closed.
- **blocked:** open, waiting on at least one open issue.
- **stage:**
  - `post-v1` is phase PX.
  - `test` covers gates, spikes, #225 and anything that waits on #225.
  - `build` is everything else.
- **"Unlocks":** how many open issues wait on an issue, directly or further down the chain. An epic counts as all of its children.

## Editing the page

The lead set these rules:
- **No timing estimates.** No dates, month windows, durations or pace forecasts.
- **Minimal text.** Plain language for project managers, with one short line per section at most.
- **The look:** an engineering-sheet style (drafting paper in light mode, a blueprint in dark mode), with these status colours:
  - green = done;
  - amber = in progress;
  - blue = ready;
  - hatched = blocked.
- **No header refresh panel.** The page refreshes silently on open.

The plain-language constants in the script must follow the plan. Update them whenever the plan changes phases, milestones or gates:
- `AREAS`: what each phase delivers, grouped by area;
- `MILESTONES`: M0–M8 in plain words;
- `GATE_ORDER`: the gates in plan order.

Check every edit before committing:
```
python3 - <<'EOF'
import re; s = open('docs/godotnext/tracker/index.html', encoding='utf-8').read()
open('/tmp/tracker-check.js', 'w').write(re.findall(r'<script>(.*?)</script>', s, re.S)[0])
EOF
node --check /tmp/tracker-check.js
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
  --window-size=1240,2300 --virtual-time-budget=9000 --screenshot=/tmp/tracker.png \
  "file://$PWD/docs/godotnext/tracker/index.html"
```
Look at the screenshot once, fix what it shows, then commit to master and push.
