#!/usr/bin/env python3
"""GitHub issue helpers for Legoless/GodotNext.

Every call passes the repository explicitly (bare `gh` in this checkout targets the upstream
godotengine/godot remote) and retries transient network errors.

  gh_issues.py snapshot DIR                  write DIR/issues.json and DIR/links.json for every issue
  gh_issues.py blockers N                    list what N is blocked by and what it blocks, with states
  gh_issues.py block N --by M [M ...]        add "blocked by" links: N waits on each M
  gh_issues.py unblock N --by M [M ...]      remove those links
  gh_issues.py sub PARENT CHILD [CHILD ...]  make each CHILD a sub-issue of PARENT (moves it from an old parent)
  gh_issues.py deps-section N [N ...]        rewrite the "## Dependencies" section of each body from the live links

Write commands accept --dry-run, which prints the change instead of making it.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

REPO = "Legoless/GodotNext"
TRANSIENT = ("timed out", "timeout", "connection reset", "EOF", "502", "503", "504", "secondary rate limit",
             "was submitted too quickly", "TLS handshake", "error connecting", "connection refused", "no such host")


def gh(args, input_text=None):
    r = None
    for attempt in range(8):
        r = subprocess.run(["gh"] + args, input=input_text, capture_output=True, text=True)
        if r.returncode == 0 or not any(s in r.stderr for s in TRANSIENT):
            return r
        time.sleep(3 * (attempt + 1))
    return r


def api(path, method="GET", payload=None, paginate=False):
    args = ["api", "-X", method, f"repos/{REPO}/{path}"]
    if paginate:
        args.insert(1, "--paginate")
    if payload is not None:
        args += ["--input", "-"]
    r = gh(args, json.dumps(payload) if payload is not None else None)
    if r.returncode != 0:
        raise RuntimeError(f"{method} {path}: {r.stderr.strip()[:300]}")
    out = r.stdout.strip()
    if not out:
        return None
    if paginate and out.startswith("[") and "][" in out:
        out = "[" + out.replace("][", ",")[1:-1] + "]"
    return json.loads(out)


def issue_id(n):
    return api(f"issues/{n}")["id"]


def cmd_snapshot(a):
    os.makedirs(a.dir, exist_ok=True)
    r = gh(["issue", "list", "-R", REPO, "--state", "all", "--limit", "1000", "--json",
            "number,title,state,stateReason,labels,milestone,assignees,body,url"])
    if r.returncode != 0:
        sys.exit(r.stderr)
    issues = json.loads(r.stdout)

    def links(n):
        return str(n), {"id": issue_id(n),
                        "blocked_by": [x["number"] for x in api(f"issues/{n}/dependencies/blocked_by", paginate=True) or []],
                        "sub_issues": [x["number"] for x in api(f"issues/{n}/sub_issues", paginate=True) or []]}
    with ThreadPoolExecutor(10) as pool:
        all_links = dict(pool.map(links, [i["number"] for i in issues]))
    json.dump(issues, open(os.path.join(a.dir, "issues.json"), "w"), indent=1)
    json.dump(all_links, open(os.path.join(a.dir, "links.json"), "w"), indent=1)
    print(f"{len(issues)} issues, {sum(len(v['blocked_by']) for v in all_links.values())} blocked-by links, "
          f"{sum(len(v['sub_issues']) for v in all_links.values())} sub-issue links -> {a.dir}")


def cmd_blockers(a):
    for title, path in (("blocked by", "blocked_by"), ("blocks", "blocking")):
        rows = api(f"issues/{a.n}/dependencies/{path}", paginate=True) or []
        print(f"#{a.n} {title}:")
        for x in sorted(rows, key=lambda x: x["number"]):
            print(f"  #{x['number']} [{x['state']}] {x['title']}")
        if not rows:
            print("  (none)")


def cmd_block(a, remove=False):
    for m in a.by:
        verb = "unblock" if remove else "block"
        if a.dry_run:
            print(f"[dry] {verb} #{a.n} by #{m}")
            continue
        mid = issue_id(m)
        try:
            if remove:
                api(f"issues/{a.n}/dependencies/blocked_by/{mid}", method="DELETE")
            else:
                api(f"issues/{a.n}/dependencies/blocked_by", method="POST", payload={"issue_id": mid})
            print(f"{verb}ed #{a.n} by #{m}")
        except RuntimeError as e:
            if "already" in str(e).lower() or (remove and "404" in str(e)):
                print(f"#{a.n} by #{m}: nothing to change")
            else:
                raise


def cmd_sub(a):
    for c in a.children:
        if a.dry_run:
            print(f"[dry] #{c} becomes a sub-issue of #{a.parent}")
            continue
        try:
            api(f"issues/{a.parent}/sub_issues", method="POST", payload={"sub_issue_id": issue_id(c), "replace_parent": True})
            print(f"#{c} is now a sub-issue of #{a.parent}")
        except RuntimeError as e:
            if "already" in str(e).lower() or "duplicate" in str(e).lower():
                print(f"#{c} is already under #{a.parent}")
            else:
                raise


def deps_text(n):
    def items(path):
        rows = api(f"issues/{n}/dependencies/{path}", paginate=True) or []
        return [x for x in sorted(rows, key=lambda x: x["number"]) if x.get("state_reason") != "not_planned"]
    before, after = items("blocked_by"), items("blocking")
    lines = ["**Blocked by:**"] + ([f"- #{x['number']} — {x['title']}" for x in before] or ["- nothing — can start now"])
    lines += ["", "**Unblocks:**"] + ([f"- #{x['number']} — {x['title']}" for x in after] or ["- nothing directly"])
    return "\n".join(lines)


def cmd_deps_section(a):
    for n in a.numbers:
        body = api(f"issues/{n}")["body"] or ""
        new = deps_text(n)
        pat = re.compile(r"(^## Dependencies[^\n]*\n)(.*?)(?=^## |\Z)", re.S | re.M)
        if pat.search(body):
            updated = pat.sub(lambda m: m.group(1) + new + "\n\n", body, count=1)
        else:
            updated = body.rstrip() + "\n\n## Dependencies\n" + new + "\n"
        if updated.strip() == body.strip():
            print(f"#{n}: Dependencies section already current")
            continue
        if a.dry_run:
            print(f"[dry] #{n} Dependencies section becomes:\n{new}\n")
            continue
        r = gh(["issue", "edit", str(n), "-R", REPO, "--body-file", "-"], updated)
        if r.returncode != 0:
            sys.exit(r.stderr)
        print(f"#{n}: Dependencies section rewritten")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("snapshot"); s.add_argument("dir")
    s = sp.add_parser("blockers"); s.add_argument("n", type=int)
    for name in ("block", "unblock"):
        s = sp.add_parser(name); s.add_argument("n", type=int); s.add_argument("--by", type=int, nargs="+", required=True)
        s.add_argument("--dry-run", action="store_true")
    s = sp.add_parser("sub"); s.add_argument("parent", type=int); s.add_argument("children", type=int, nargs="+")
    s.add_argument("--dry-run", action="store_true")
    s = sp.add_parser("deps-section"); s.add_argument("numbers", type=int, nargs="+"); s.add_argument("--dry-run", action="store_true")
    a = p.parse_args()
    {"snapshot": cmd_snapshot, "blockers": cmd_blockers, "block": cmd_block,
     "unblock": lambda x: cmd_block(x, remove=True), "sub": cmd_sub, "deps-section": cmd_deps_section}[a.cmd](a)


if __name__ == "__main__":
    main()
