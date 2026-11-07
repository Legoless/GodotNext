#!/usr/bin/env python3
"""Read frame/view/sentinel/nonfinite rows produced by velocity_dump.gd."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from godotnext_metrics.result import MetricResult


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("--reset-frame", type=int, default=400)
    args = parser.parse_args()
    counts: dict[int, int] = {}
    try:
        with args.csv.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != ["frame", "view", "sentinel", "nonfinite"]:
                raise ValueError("q21.csv must have frame,view,sentinel,nonfinite columns")
            for row in reader:
                frame = int(row["frame"])
                sentinel = int(row["sentinel"])
                nonfinite = int(row["nonfinite"])
                if sentinel < 0 or nonfinite < 0:
                    raise ValueError("q21.csv counts must be non-negative")
                counts[frame] = counts.get(frame, 0) + sentinel + nonfinite
        if not counts:
            raise ValueError("q21.csv has no samples")
        ordered = {frame: counts[frame] for frame in sorted(counts)}
        reported = {args.reset_frame: ordered[args.reset_frame]} if args.reset_frame in ordered else {}
        invalid = sum(count for frame, count in ordered.items() if frame != args.reset_frame)
        result = MetricResult(
            metric="Q21",
            value=invalid,
            threshold=0,
            passed=invalid == 0,
            values={
                "invalid_texels": invalid,
                "per_frame": ordered,
                "reset_frames": reported,
            },
        )
        print(json.dumps(result.to_json(), sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({"metric": "Q21", "pass": False, "error": str(error)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
