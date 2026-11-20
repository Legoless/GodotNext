#!/usr/bin/env python3
"""Headless testbed --import shutdown regression check (#298).

Runs consecutive `--headless --path <testbed> --import` invocations inside an
isolated editor cache directory, alternating cold (missing doc cache), stale
(version_hash mismatch simulating an editor binary switch), and warm doc cache
states. Fails if any run exits non-zero or logs an ERROR: line.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

STALE_SCRIPT = """extends SceneTree

func _init() -> void:
\tvar args := OS.get_cmdline_user_args()
\tif args.size() < 2:
\t\tpush_error("test_headless_import: expected src and dst cache paths")
\t\tquit(2)
\t\treturn
\tvar res := ResourceLoader.load(args[0])
\tif res == null:
\t\tpush_error("test_headless_import: failed to load doc cache: " + args[0])
\t\tquit(3)
\t\treturn
\tres.set_meta("version_hash", "godotnext_stale_doc_cache_298")
\tvar err := ResourceSaver.save(res, args[1], ResourceSaver.FLAG_COMPRESS)
\tif err != OK:
\t\tpush_error("test_headless_import: failed to save stale doc cache: " + str(err))
\t\tquit(4)
\t\treturn
\tquit(0)
"""


def default_godot_path(repo_root: Path) -> Path:
    system = platform.system()
    if system == "Windows":
        return repo_root / "bin" / "godot.windows.editor.x86_64.console.exe"
    if system == "Darwin":
        return repo_root / "bin" / "godot.macos.editor.arm64"
    return repo_root / "bin" / "godot.linuxbsd.editor.x86_64"


def build_isolated_env(work_dir: Path) -> tuple[dict[str, str], Path]:
    env = os.environ.copy()
    system = platform.system()
    if system == "Windows":
        cache_root = work_dir / "localappdata"
        cache_root.mkdir(parents=True, exist_ok=True)
        env["LOCALAPPDATA"] = str(cache_root)
        return env, cache_root
    if system == "Darwin":
        home_root = work_dir / "home"
        (home_root / "Library" / "Caches").mkdir(parents=True, exist_ok=True)
        env["HOME"] = str(home_root)
        return env, home_root / "Library" / "Caches"
    cache_root = work_dir / "cache"
    config_root = work_dir / "config"
    data_root = work_dir / "data"
    cache_root.mkdir(parents=True, exist_ok=True)
    config_root.mkdir(parents=True, exist_ok=True)
    data_root.mkdir(parents=True, exist_ok=True)
    env["XDG_CACHE_HOME"] = str(cache_root)
    env["XDG_CONFIG_HOME"] = str(config_root)
    env["XDG_DATA_HOME"] = str(data_root)
    return env, cache_root


def find_doc_cache(cache_root: Path) -> Path | None:
    matches = sorted(cache_root.rglob("editor_doc_cache-*.res"))
    return matches[0] if matches else None


def make_stale_cache(
    godot: Path,
    testbed: Path,
    src_cache: Path,
    dst_cache: Path,
    work_dir: Path,
    env: dict[str, str],
    timeout: int,
) -> None:
    script_path = work_dir / "make_stale_doc_cache.gd"
    script_path.write_text(STALE_SCRIPT, encoding="utf-8", newline="\n")
    cmd = [
        str(godot),
        "--headless",
        "--path",
        str(testbed),
        "--script",
        str(script_path),
        "--",
        str(src_cache),
        str(dst_cache),
    ]
    proc = subprocess.run(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
    )
    if proc.returncode != 0 or not dst_cache.is_file():
        sys.stderr.write(f"test_headless_import: failed to synthesize stale doc cache (exit={proc.returncode})\n")
        sys.stderr.write(proc.stdout[-2000:] + "\n")
        sys.exit(2)


def run_import(
    godot: Path,
    testbed: Path,
    env: dict[str, str],
    timeout: int,
) -> subprocess.CompletedProcess[str]:
    cmd = [str(godot), "--headless", "--path", str(testbed), "--import"]
    return subprocess.run(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
    )


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    testbed_default = script_dir.parent.parent
    repo_root = testbed_default.parent.parent

    parser = argparse.ArgumentParser(
        description="Run consecutive headless --import passes with stale/warm/cold editor doc caches."
    )
    parser.add_argument(
        "--godot",
        default=os.environ.get("GODOT", str(default_godot_path(repo_root))),
        help="Path to the Godot editor binary.",
    )
    parser.add_argument(
        "--testbed",
        default=str(testbed_default),
        help="Path to the testbed project directory.",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=20,
        help="Number of consecutive headless --import runs (default: 20).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="Per-run timeout in seconds (default: 300).",
    )
    args = parser.parse_args()

    if args.runs < 1:
        sys.stderr.write("test_headless_import: --runs must be >= 1\n")
        return 2

    godot = Path(args.godot).resolve()
    testbed = Path(args.testbed).resolve()
    if not godot.is_file():
        sys.stderr.write(f"test_headless_import: godot binary not found: {godot}\n")
        return 2
    if not (testbed / "project.godot").is_file():
        sys.stderr.write(f"test_headless_import: project.godot not found in {testbed}\n")
        return 2

    counts = {"cold": 0, "stale": 0, "warm": 0}
    mid_cold = (args.runs // 2) + 1 if args.runs >= 4 else -1

    with tempfile.TemporaryDirectory(prefix="godotnext_import_test_") as tmp:
        work_dir = Path(tmp)
        env, cache_root = build_isolated_env(work_dir)
        cache_file: Path | None = None
        stale_cache = work_dir / "stale_editor_doc_cache.res"

        for run_idx in range(1, args.runs + 1):
            if run_idx == 1 or run_idx == mid_cold:
                mode = "cold"
                if cache_file is not None and cache_file.exists():
                    cache_file.unlink()
            elif run_idx % 2 == 0:
                mode = "stale"
                assert cache_file is not None
                shutil.copy2(stale_cache, cache_file)
            else:
                mode = "warm"

            counts[mode] += 1
            proc = run_import(godot, testbed, env, args.timeout)
            error_lines = [line for line in proc.stdout.splitlines() if "ERROR:" in line]
            if proc.returncode != 0 or error_lines:
                sys.stderr.write(
                    f"test_headless_import: FAIL run {run_idx}/{args.runs} ({mode}) "
                    f"exit={proc.returncode} errors={len(error_lines)}\n"
                )
                for line in error_lines[:10]:
                    sys.stderr.write(f"  {line}\n")
                tail = proc.stdout.splitlines()[-25:]
                if tail:
                    sys.stderr.write("--- tail ---\n" + "\n".join(tail) + "\n")
                return 1

            if cache_file is None:
                cache_file = find_doc_cache(cache_root)
                if cache_file is None:
                    sys.stderr.write(
                        f"test_headless_import: editor_doc_cache-*.res was not created under {cache_root}\n"
                    )
                    return 1
                if args.runs >= 2:
                    make_stale_cache(godot, testbed, cache_file, stale_cache, work_dir, env, args.timeout)
            elif not cache_file.is_file() or cache_file.stat().st_size == 0:
                sys.stderr.write(f"test_headless_import: doc cache missing or empty after run {run_idx}/{args.runs}\n")
                return 1

    print(
        f"test_headless_import: ok (runs={args.runs} "
        f"cold={counts['cold']} stale={counts['stale']} warm={counts['warm']} errors=0)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
