"""F1 trace method (#202, plan §21.5.4): the DLSS-G pass's GPU time from an Nsight Systems trace.

DLSS-G runs inside the Streamline Present, on a D3D12 queue Streamline creates, so no RenderingDevice
timestamp can bracket it. F1 reads it from a trace instead: 600 frames at FG 2x and 2160p, recorded
with D3D12 API and per-workload GPU trace, exported to SQLite:

    nsys profile --trace=dx12 --dx12-gpu-workload=individual --output=<report> <launch command>
    nsys export --type=sqlite --output=<report>.sqlite <report>.nsys-rep

Each ``DX12_WORKLOAD`` row is one GPU workload with ``start`` and ``end`` in ns and the graphics
frame index ``frameId``; its ``objectNameId`` (the D3D12 object name) and ``textId`` (PIX marker
text) point into ``StringIds``. Streamline names the D3D12 objects it creates ``nv.sl.*``; the
Nsight Systems User Guide's example is the queue ``nv.sl.dlss_g.cmdQ.dlssg``. A workload is DLSS-G
when either name starts with ``--name-prefix`` (default ``nv.sl.dlss_g``). Which of the two names
carries the queue name in a real trace is checked on the first PQ trace: the report lists the
names it matched, and with no match it lists the names it found and exits 2.

A frame's DLSS-G time is the union of its matched workloads' intervals, in ms. After ``--warmup``
frames, ``--frames`` (600) frames give p50, p95 (F1's statistic) and p99, with the harness
percentile. ``--expect-ms`` checks the #202 PQ line: the method reproduces a reference trace's
value within 5%. Times and the UE bar are PQ work (§22).
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

from godotnext_metrics.percentile import percentile

NAME_PREFIX = "nv.sl.dlss_g"
WARMUP = 120
FRAMES = 600
REPRODUCE_TOLERANCE = 0.05
F1_TARGET_MS = 3.0


class TraceInputError(ValueError):
    """The export has no usable DLSS-G workloads."""


def _union_ms(intervals: list[tuple[int, int]]) -> float:
    """Total ns covered by the intervals, overlaps counted once, in ms."""
    total = 0
    reach: int | None = None
    for start, end in sorted(intervals):
        if reach is None or start >= reach:
            total += end - start
            reach = end
        elif end > reach:
            total += end - reach
            reach = end
    return total / 1.0e6


def dlssg_frames(database: Path, *, prefix: str = NAME_PREFIX) -> tuple[dict[int, float], dict[str, object]]:
    """DLSS-G GPU ms per frame index, and what was matched."""
    if not database.is_file():
        raise TraceInputError(f"{database} does not exist")
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type = 'table'")}
        missing = {"DX12_WORKLOAD", "StringIds"} - tables
        if missing:
            raise TraceInputError(
                f"{database.name} has no {', '.join(sorted(missing))} table (trace with --trace=dx12)"
            )
        rows = connection.execute(
            """
            SELECT w.start, w.end, w.frameId, object_name.value, marker.value
            FROM DX12_WORKLOAD AS w
            LEFT JOIN StringIds AS object_name ON object_name.id = w.objectNameId
            LEFT JOIN StringIds AS marker ON marker.id = w.textId
            """
        ).fetchall()
    finally:
        connection.close()
    per_frame: dict[int, list[tuple[int, int]]] = defaultdict(list)
    matched: set[str] = set()
    seen: set[str] = set()
    unframed = 0
    for start, end, frame_id, object_name, marker in rows:
        names = [name for name in (object_name, marker) if isinstance(name, str)]
        seen.update(names)
        hits = [name for name in names if name.startswith(prefix)]
        if not hits:
            continue
        matched.update(hits)
        if frame_id is None:
            unframed += 1
            continue
        per_frame[int(frame_id)].append((int(start), int(end)))
    if not matched:
        listed = ", ".join(sorted(seen)[:20]) or "none"
        raise TraceInputError(f"no DX12 workload is named {prefix}*; names found: {listed}")
    frames = {frame: _union_ms(intervals) for frame, intervals in per_frame.items()}
    info: dict[str, object] = {
        "workloads": len(rows),
        "matched_names": sorted(matched),
        "matched_frames": len(frames),
        "matched_without_frame": unframed,
    }
    return frames, info


def f1_report(
    database: Path,
    *,
    prefix: str = NAME_PREFIX,
    warmup: int = WARMUP,
    count: int = FRAMES,
    expect_ms: float | None = None,
) -> dict[str, object]:
    frames, info = dlssg_frames(database, prefix=prefix)
    ordered = [frames[index] for index in sorted(frames)]
    if len(ordered) < warmup + count:
        raise TraceInputError(
            f"need {warmup + count} frames with DLSS-G work ({warmup} warm-up + {count}), found {len(ordered)}"
        )
    window = ordered[warmup : warmup + count]
    p95 = percentile(window, 95.0)
    report: dict[str, object] = {
        "metric": "F1 DLSS-G pass (trace method)",
        "name_prefix": prefix,
        "warmup": warmup,
        "frames": count,
        "p50_ms": percentile(window, 50.0),
        "p95_ms": p95,
        "p99_ms": percentile(window, 99.0),
        "report_only_target_ms": F1_TARGET_MS,
    }
    report.update(info)
    if expect_ms is not None:
        deviation = abs(p95 - expect_ms) / expect_ms if expect_ms > 0.0 else None
        report["expect_ms"] = expect_ms
        report["deviation"] = deviation
        report["tolerance"] = REPRODUCE_TOLERANCE
        report["pass"] = bool(deviation is not None and deviation <= REPRODUCE_TOLERANCE + 1e-9)
    else:
        # F1 itself passes or fails against UE 5.9 in PQ (§23); the tool only reports.
        report["pass"] = None
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.nsys_dlssg")
    parser.add_argument("--sqlite", type=Path, required=True, help="nsys export --type=sqlite output")
    parser.add_argument("--name-prefix", default=NAME_PREFIX)
    parser.add_argument("--warmup", type=int, default=WARMUP)
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--expect-ms", type=float, default=None, help="reference p95: reproduce within 5%%")
    args = parser.parse_args(argv)
    try:
        report = f1_report(
            args.sqlite, prefix=args.name_prefix, warmup=args.warmup, count=args.frames, expect_ms=args.expect_ms
        )
    except (OSError, ValueError, sqlite3.Error) as error:
        json.dump({"metric": "F1 DLSS-G pass (trace method)", "pass": False, "error": str(error)}, sys.stdout)
        sys.stdout.write("\n")
        return 2
    json.dump(report, sys.stdout, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
