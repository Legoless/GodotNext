"""Side CLI for the #74 streaming reports.

``python3 -m godotnext_metrics.streaming_reports`` does not add a Q metric.
The frozen ``COMMANDS`` set stays q1–q21. Exit 0 when a report object was
produced, including an absent source or a failed gate. Exit 2 when ``--in``
is missing or a sidecar is not valid JSON.

#78 fills page-upload milliseconds and the memory-report byte counts.
#79 fills fallback debug stats. Until those methods exist the fields are null.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import cast

from godotnext_metrics.flight_reports import (
    exclusion_report,
    q14_flight_report,
    render_markdown,
    run_envelope,
    slow_io_report,
)
from godotnext_metrics.geometry_memory import memory_report
from godotnext_metrics.result import json_ready
from godotnext_metrics.timing_metrics import read_timing_csv

REPORTS = {
    "q15": "geometry_memory",
    "q14": "fallback_coverage",
    "slow-io": "slow_io_flight",
    "exclusion": "exclusion_matrix",
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.streaming_reports")
    parser.add_argument("report", choices=sorted(REPORTS))
    parser.add_argument("--in", dest="in_dir", required=True, help="run directory")
    parser.add_argument("--out", dest="out_dir", help="directory for the JSON and Markdown files")
    parser.add_argument("--markdown", action="store_true", help="also write the Markdown summary when --out is set")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        code = exc.code
        return int(code) if isinstance(code, int) else 2
    in_dir = Path(args.in_dir)
    if not in_dir.is_dir():
        _emit({"metric": args.report, "pass": None, "error": f"missing input directory {in_dir}"})
        return 2
    try:
        report = build_report(args.report, in_dir)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        _emit({"metric": args.report, "pass": None, "error": str(exc)})
        return 2
    _emit(report)
    if args.out_dir:
        out_dir = Path(args.out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        name = REPORTS[args.report]
        text = json.dumps(json_ready(report), indent=2, sort_keys=True, allow_nan=False) + "\n"
        (out_dir / f"{name}.json").write_text(text, encoding="utf-8")
        (out_dir / f"{name}.md").write_text(render_markdown(report), encoding="utf-8")
    elif args.markdown:
        sys.stderr.write(render_markdown(report))
    return 0


def build_report(kind: str, in_dir: Path) -> dict[str, object]:
    manifest = _optional_json(in_dir / "manifest.json")
    if kind == "q15":
        report = memory_report(_optional_json(in_dir / "memory_report.json"))
    elif kind == "q14":
        report = q14_flight_report(_optional_json(in_dir / "routing_frames.json"))
    elif kind == "slow-io":
        report = _slow_io(in_dir)
    elif kind == "exclusion":
        report = exclusion_report(
            _optional_json(in_dir / "cells.json"),
            _optional_json(in_dir / "exclusion_observed.json"),
            _optional_json(in_dir / "moving_track.json"),
        )
    else:
        raise ValueError(f"unknown report {kind}")
    report["envelope"] = run_envelope(manifest)
    return report


def _slow_io(in_dir: Path) -> dict[str, object]:
    params = _optional_json(in_dir / "slow_io_params.json")
    wall: list[float] | None = None
    timing = in_dir / "timing.csv"
    if timing.is_file():
        columns = read_timing_csv(timing)
        wall = [value for value in columns.get("wall_ms", []) if isinstance(value, float)]
    return slow_io_report(params, wall, None)


def _optional_json(path: Path) -> object | None:
    if not path.is_file():
        return None
    loaded = cast(object, json.loads(path.read_text(encoding="utf-8")))
    if not isinstance(loaded, (dict, list)):
        raise ValueError(f"{path.name} must be a JSON object or list")
    return loaded


def _emit(report: dict[str, object]) -> None:
    json.dump(json_ready(report), sys.stdout, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    sys.exit(main())
