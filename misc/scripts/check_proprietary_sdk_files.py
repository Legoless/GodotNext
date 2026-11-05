#!/usr/bin/env python3
"""Reject NVIDIA SDK files in the repository (plan §19, GU-6).

The official GodotNext builds stay MIT and DLSS-free; developers compile DLSS
in with their own NVIDIA SDK. This guard keeps every SDK header, library,
runtime DLL/SO, sample and `sl_nvperf` file out of the tree.

    python3 misc/scripts/check_proprietary_sdk_files.py
    python3 misc/scripts/check_proprietary_sdk_files.py --root /tmp/planted

Without `--root` it checks `git ls-files` (the checkout). Matching is on the
lower-cased basename only, so `DelegatingX509Certificate.java` stays clean.
Exit code 0 means clean, 1 means at least one match.
"""

from __future__ import annotations

import argparse
import fnmatch
import subprocess
import sys
from pathlib import Path

PATTERNS = (
    "nvsdk_ngx*.h",
    "nvsdk_ngx*.lib",
    "libnvsdk_ngx.a",
    "libsdk_nvngx.a",
    "nvngx_*.dll",
    "libnvidia-ngx*.so*",
    "sl_nvperf.*",
)


def tracked_files() -> list[str]:
    output = subprocess.run(
        ["git", "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    return [name for name in output.decode("utf-8", "replace").split("\0") if name]


def walk_files(root: Path) -> list[str]:
    return [str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()]


def match(path: str) -> str | None:
    name = Path(path).name.lower()
    for pattern in PATTERNS:
        if fnmatch.fnmatchcase(name, pattern):
            return pattern
    return None


def check(paths: list[str]) -> list[tuple[str, str]]:
    return [(path, pattern) for path in paths if (pattern := match(path)) is not None]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        help="walk this directory instead of the git checkout",
    )
    args = parser.parse_args(argv)

    paths = walk_files(args.root) if args.root is not None else tracked_files()
    hits = check(paths)
    for path, pattern in hits:
        print(f"proprietary SDK file: {path} (matches {pattern})", file=sys.stderr)
    if hits:
        print(
            f"check_proprietary_sdk_files: {len(hits)} hit(s); NVIDIA SDK files must stay outside the repository (plan §19).",
            file=sys.stderr,
        )
        return 1
    print(f"check_proprietary_sdk_files: {len(paths)} file(s) checked, no SDK files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
