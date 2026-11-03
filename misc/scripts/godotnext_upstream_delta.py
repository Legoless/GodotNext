#!/usr/bin/env python3
"""Report how much of the fork's delta touches upstream Godot files.

The merge base of HEAD and the base ref (default ``upstream/master``) is
found with ``git merge-base``. ``git diff --numstat`` between the merge
base and HEAD is then filtered to paths that already exist at the merge
base (``git cat-file -e <merge-base>:<path>``), so new GodotNext files do
not count as upstream delta. The report lists the added plus deleted line
totals, the file count, the top 20 files by changed lines, and every
``GodotNext:`` hook marker in the changed upstream files, with file and
line. See ``docs/godotnext/fork-maintenance.md`` for the named-hook rule
this tracks.

Exit codes: 0 on success, 2 when there is no merge base (for example in a
clone without the replace-ref graft), 1 on any other git failure.
"""

import argparse
import json
import re
import subprocess
import sys
from typing import TypedDict

TOP_FILE_COUNT = 20
MARKER_PATTERN = "GodotNext:"
GREP_BATCH_SIZE = 256

NUMSTAT_HEADER = re.compile(r"^(?P<added>-|\d+)\t(?P<deleted>-|\d+)\t(?P<path>.*)$")
GREP_LINE = re.compile(r"^(?P<path>.*):(?P<line>\d+):(?P<text>.*)$")


class FileDelta(TypedDict):
    path: str
    added: int
    deleted: int
    changed: int
    binary: bool


class Marker(TypedDict):
    path: str
    line: int
    text: str


class Report(TypedDict):
    base: str
    head: str
    merge_base: str
    upstream_files_changed: int
    added_lines: int
    deleted_lines: int
    changed_lines: int
    top_files: list[FileDelta]
    markers: list[Marker]


class GitError(Exception):
    """A git command failed."""


class NoMergeBaseError(GitError):
    """HEAD and the base ref have no merge base."""


def run_git(args: list[str], *, ok_returncodes: tuple[int, ...] = (0,)) -> str:
    result = subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode not in ok_returncodes:
        detail = result.stderr.strip() or result.stdout.strip()
        suffix = f": {detail}" if detail else ""
        raise GitError(f"git {' '.join(args)} failed (exit {result.returncode}){suffix}")
    return result.stdout


def find_merge_base(head: str, base: str) -> str:
    try:
        merge_base = run_git(["merge-base", head, base]).strip()
    except GitError as error:
        raise NoMergeBaseError(str(error)) from error
    if not merge_base:
        raise NoMergeBaseError(f"git merge-base {head} {base} printed nothing")
    return merge_base


def parse_numstat_entry(header: re.Match[str], path: str, base_path: str) -> tuple[FileDelta, str]:
    added_text = header.group("added")
    deleted_text = header.group("deleted")
    binary = added_text == "-" or deleted_text == "-"
    added = 0 if binary else int(added_text)
    deleted = 0 if binary else int(deleted_text)
    delta: FileDelta = {
        "path": path,
        "added": added,
        "deleted": deleted,
        "changed": added + deleted,
        "binary": binary,
    }
    return delta, base_path


def diff_numstat(merge_base: str, head: str) -> list[tuple[FileDelta, str]]:
    """Return (delta, path at merge base) pairs for ``merge_base..head``.

    With ``-z``, rename entries have an empty path in the numstat header
    and carry the old and new paths as the next two NUL-separated fields.
    The old path is the one that exists at the merge base.
    """
    output = run_git(["diff", "--numstat", "-z", f"{merge_base}..{head}"])
    tokens = output.split("\0")
    entries: list[tuple[FileDelta, str]] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        index += 1
        if not token:
            continue
        header = NUMSTAT_HEADER.match(token)
        if header is None:
            raise GitError(f"unexpected git diff --numstat output: {token!r}")
        path = header.group("path")
        if path:
            entries.append(parse_numstat_entry(header, path, path))
        else:
            if index + 1 >= len(tokens):
                raise GitError("truncated rename entry in git diff --numstat output")
            old_path, new_path = tokens[index], tokens[index + 1]
            index += 2
            entries.append(parse_numstat_entry(header, new_path, old_path))
    return entries


def exists_at(commit: str, path: str) -> bool:
    result = subprocess.run(
        ["git", "cat-file", "-e", f"{commit}:{path}"],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    return result.returncode == 0


def find_markers(head: str, paths: list[str]) -> list[Marker]:
    markers: list[Marker] = []
    prefix = f"{head}:"
    for start in range(0, len(paths), GREP_BATCH_SIZE):
        batch = paths[start : start + GREP_BATCH_SIZE]
        output = run_git(
            ["grep", "-n", "-I", "-e", MARKER_PATTERN, head, "--", *batch],
            ok_returncodes=(0, 1),  # 1 means "no matches".
        )
        for line in output.splitlines():
            if line.startswith(prefix):
                line = line[len(prefix) :]
            match = GREP_LINE.match(line)
            if match is None:
                raise GitError(f"unexpected git grep output: {line!r}")
            markers.append({
                "path": match.group("path"),
                "line": int(match.group("line")),
                "text": match.group("text").strip(),
            })
    markers.sort(key=lambda marker: (marker["path"], marker["line"]))
    return markers


def build_report(head: str, base: str) -> Report:
    merge_base = find_merge_base(head, base)
    upstream_files = [delta for delta, base_path in diff_numstat(merge_base, head) if exists_at(merge_base, base_path)]
    upstream_files.sort(key=lambda delta: (-delta["changed"], delta["path"]))
    added_lines = sum(delta["added"] for delta in upstream_files)
    deleted_lines = sum(delta["deleted"] for delta in upstream_files)
    markers = find_markers(head, [delta["path"] for delta in upstream_files])
    return {
        "base": base,
        "head": head,
        "merge_base": merge_base,
        "upstream_files_changed": len(upstream_files),
        "added_lines": added_lines,
        "deleted_lines": deleted_lines,
        "changed_lines": added_lines + deleted_lines,
        "top_files": upstream_files[:TOP_FILE_COUNT],
        "markers": markers,
    }


def markdown_escape(text: str) -> str:
    return text.replace("|", "\\|").replace("`", "'")


def render_markdown(report: Report) -> str:
    lines = [
        "# Upstream delta",
        "",
        f"- Base: `{report['base']}`",
        f"- Head: `{report['head']}`",
        f"- Merge base: `{report['merge_base']}`",
        f"- Upstream files changed: {report['upstream_files_changed']}",
        f"- Added lines: {report['added_lines']}",
        f"- Deleted lines: {report['deleted_lines']}",
        f"- Changed lines (added + deleted): {report['changed_lines']}",
        "",
        f"## Top {TOP_FILE_COUNT} files",
        "",
    ]
    if report["top_files"]:
        lines += [
            "| File | Added | Deleted | Changed |",
            "| --- | ---: | ---: | ---: |",
        ]
        for delta in report["top_files"]:
            path = markdown_escape(delta["path"])
            if delta["binary"]:
                path += " (binary)"
            lines.append(f"| {path} | {delta['added']} | {delta['deleted']} | {delta['changed']} |")
    else:
        lines.append("_No upstream files changed._")
    lines += [
        "",
        f"## `{MARKER_PATTERN}` markers",
        "",
    ]
    if report["markers"]:
        lines += [
            "| File | Line | Marker |",
            "| --- | ---: | --- |",
        ]
        for marker in report["markers"]:
            path = markdown_escape(marker["path"])
            text = markdown_escape(marker["text"])
            lines.append(f"| {path} | {marker['line']} | `{text}` |")
    else:
        lines.append("_No markers in the changed upstream files._")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Report added/deleted lines in files that already exist at the merge base with the base ref.",
    )
    parser.add_argument(
        "--base",
        default="upstream/master",
        help="Base ref to find the merge base against (default: upstream/master).",
    )
    parser.add_argument(
        "--head",
        default="HEAD",
        help="Head ref to diff (default: HEAD).",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print JSON instead of Markdown.",
    )
    args = parser.parse_args(argv)

    try:
        report = build_report(args.head, args.base)
    except NoMergeBaseError as error:
        print(f"error: no merge base between {args.head} and {args.base}: {error}", file=sys.stderr)
        print(
            "hint: clones that merge upstream need the replace-ref graft; see docs/godotnext/fork-maintenance.md",
            file=sys.stderr,
        )
        return 2
    except GitError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(render_markdown(report), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
