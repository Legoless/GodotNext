#!/usr/bin/env python3
"""Snapshot Legoless/GodotNext issues for the local build tracker (index.html).

Writes tracker-data.js (issues, dependency links, issue states, daily history) next to this file,
and keeps history.json as the running day-by-day record.

    python3 docs/godotnext/tracker/export.py

Needs the GitHub CLI (`gh`) signed in. The page itself refreshes issue states live from GitHub's
public API each time it opens; run this script when dependency links change or to record a day.
"""
import json, os, subprocess, time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = "Legoless/GodotNext"
PHASES = ["P0", "P1a", "P1b", "P2", "P3", "PU", "P4", "P5", "PR", "PD", "PS", "PL", "PV", "PM", "PP", "PN", "PT", "PU+", "PQ", "PX"]
PQ_ENTRY = 225
TRACKER = 2


def gh(args):
    r = None
    for attempt in range(8):
        r = subprocess.run(["gh"] + args, capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout
        time.sleep(1.5 * (attempt + 1))
    raise SystemExit(f"gh {' '.join(args[:3])} failed: {r.stderr[:300]}")


def blocked_by(n):
    out = gh(["api", "--paginate", f"repos/{REPO}/issues/{n}/dependencies/blocked_by"]).strip()
    if not out:
        return n, []
    rows = json.loads("[" + out.replace("][", ",")[1:-1] + "]") if out.startswith("[") else json.loads(out)
    return n, sorted(x["number"] for x in rows)


def sub_issues(n):
    out = gh(["api", "--paginate", f"repos/{REPO}/issues/{n}/sub_issues"]).strip()
    if not out:
        return n, []
    rows = json.loads("[" + out.replace("][", ",")[1:-1] + "]") if out.startswith("[") else json.loads(out)
    return n, sorted(x["number"] for x in rows)


def labels(issue, prefix):
    return [l["name"][len(prefix):] for l in issue["labels"] if l["name"].startswith(prefix)]


def main():
    issues = json.loads(gh(["issue", "list", "-R", REPO, "--state", "all", "--limit", "1000",
                            "--json", "number,title,state,stateReason,labels,milestone,assignees,url"]))
    states = {i["number"]: i["state"] for i in issues}
    live = [i for i in issues if not (i["state"] == "CLOSED" and i.get("stateReason") == "NOT_PLANNED")]
    with ThreadPoolExecutor(12) as pool:
        deps = dict(pool.map(blocked_by, [i["number"] for i in live]))
    epic_issues = [i for i in live if "epic" in labels(i, "type:")]
    with ThreadPoolExecutor(8) as pool:
        kids = dict(pool.map(sub_issues, [e["number"] for e in epic_issues]))
    epics = [{"number": e["number"], "title": e["title"], "phase": (labels(e, "phase:") or [""])[0],
              "state": e["state"], "children": kids.get(e["number"], [])} for e in epic_issues]
    rows = []
    for i in sorted(live, key=lambda x: x["number"]):
        n = i["number"]
        kind = (labels(i, "type:") or ["task"])[0]
        if kind == "epic" or n == TRACKER:
            continue
        phase = (labels(i, "phase:") or [""])[0]
        milestone = (i.get("milestone") or {}).get("title") or ""
        if phase == "PX":
            stage = "post-v1"
        elif kind in ("gate", "spike") or n == PQ_ENTRY or PQ_ENTRY in deps.get(n, []):
            stage = "test"
        else:
            stage = "build"
        rows.append({
            "number": n, "title": i["title"], "type": kind, "phase": phase,
            "phase_order": PHASES.index(phase) if phase in PHASES else 99,
            "lane": (labels(i, "lane:") or [""])[0], "size": (labels(i, "size:") or [""])[0],
            "milestone": milestone.split(" - ")[0] if " - " in milestone else milestone,
            "stage": stage, "state": i["state"],
            "assigned": bool(i["assignees"]),
            "in_progress_label": any(l["name"] == "status:in-progress" for l in i["labels"]),
            "blocked_by": deps.get(n, []), "url": i["url"],
        })
    now = datetime.now(timezone.utc).replace(microsecond=0)
    stamp = now.isoformat().replace("+00:00", "Z")

    # The day's history row uses the same status rules as the page.
    def status(r):
        if r["state"] == "CLOSED":
            return "done"
        if r["assigned"] or r["in_progress_label"]:
            return "in progress"
        return "blocked" if any(states.get(b) == "OPEN" for b in r["blocked_by"]) else "ready"

    build = [r for r in rows if r["stage"] == "build"]
    gates = [r for r in rows if r["type"] == "gate"]
    snap = {"day": stamp[:10], "build_total": len(build),
            "build_done": sum(status(r) == "done" for r in build),
            "in_progress": sum(status(r) == "in progress" for r in build),
            "ready": sum(status(r) == "ready" for r in build),
            "blocked": sum(status(r) == "blocked" for r in build),
            "gates_passed": sum(status(r) == "done" for r in gates), "gates_total": len(gates)}
    hist_path = os.path.join(HERE, "history.json")
    history = json.load(open(hist_path)) if os.path.exists(hist_path) else []
    history = [h for h in history if h["day"] != snap["day"]] + [snap]
    json.dump(history, open(hist_path, "w"), indent=1)

    data = {"generated": stamp, "repo": REPO, "rows": rows, "epics": epics,
            "states": {str(k): v for k, v in states.items()}, "history": history}
    with open(os.path.join(HERE, "tracker-data.js"), "w", encoding="utf-8") as f:
        f.write("// Generated by export.py; do not edit by hand.\n")
        f.write("window.GODOTNEXT_TRACKER = " + json.dumps(data, ensure_ascii=False, separators=(",", ":")) + ";\n")
    print(f"{stamp}: {len(rows)} issues, {sum(len(r['blocked_by']) for r in rows)} links; today {snap}")


if __name__ == "__main__":
    main()
