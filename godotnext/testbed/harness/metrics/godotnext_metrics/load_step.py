"""GT-6 load step check (#190): does the scripted GPU load step double the GPU frame time?

``tests/gpu_load_step.tscn`` flies the Dense City spline and switches a calibrated compute burner on
at flight frame 600. It writes ``gpu_load_step.csv`` (``frame``, ``burner``, ``gpu_ms``, ...) and
``gpu_load_step.json`` (its startup calibration). A harness timing run (#12) writes ``timing.csv``,
where a row's GPU frame time is the sum of its pass columns. GT-6 measures with #12 timing mode, so
``timing.csv`` decides when it is present; the scene's own viewport GPU time is reported beside it.

The ratio is the median GPU frame time over ``window`` frames that start ``settle`` frames after
the switch, divided by the median over the ``window`` frames before it. ``settle`` skips the frames
whose GPU timestamps still belong to frames before the switch. The step passes when the ratio is
within 5% of 2. This is a PQ measurement (GT-6, §22); at PR time the tool only has to run.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

SWITCH_FRAME = 600
WINDOW = 30
SETTLE = 8
TARGET_RATIO = 2.0
TOLERANCE = 0.05


class LoadStepInputError(ValueError):
    """The run folder has no usable GPU frame times."""


def gpu_frame_times(path: Path) -> dict[int, float]:
    """GPU frame time by frame index from ``gpu_load_step.csv`` or a harness ``timing.csv``."""
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        fields = reader.fieldnames or []
        if "frame" not in fields:
            raise LoadStepInputError(f"{path.name} has no frame column")
        if "gpu_ms" in fields:
            pass_columns = ["gpu_ms"]
        else:
            pass_columns = [name for name in fields if name.endswith("_ms") and name != "wall_ms"]
        if not pass_columns:
            raise LoadStepInputError(f"{path.name} has no gpu_ms or pass columns")
        times: dict[int, float] = {}
        for row in reader:
            cells = [row.get(name, "") for name in pass_columns]
            values = [float(cell) for cell in cells if cell not in ("", None)]
            if values:
                times[int(row["frame"])] = float(sum(values))
    return times


def load_step_ratio(
    times: dict[int, float],
    *,
    switch_frame: int = SWITCH_FRAME,
    window: int = WINDOW,
    settle: int = SETTLE,
    target: float = TARGET_RATIO,
    tolerance: float = TOLERANCE,
) -> dict[str, object]:
    before = [times[index] for index in range(switch_frame - window, switch_frame) if index in times]
    start = switch_frame + settle
    after = [times[index] for index in range(start, start + window) if index in times]
    if len(before) < window or len(after) < window:
        raise LoadStepInputError(
            f"need {window} frames before frame {switch_frame} and from frame {start}, found {len(before)} and {len(after)}"
        )
    before_ms = float(np.median(np.asarray(before, dtype=np.float64)))
    after_ms = float(np.median(np.asarray(after, dtype=np.float64)))
    ratio = after_ms / before_ms if before_ms > 0.0 else math.inf
    finite = math.isfinite(ratio)
    return {
        "metric": "GT-6 load step",
        "switch_frame": switch_frame,
        "window": window,
        "settle": settle,
        "before_ms": before_ms,
        "after_ms": after_ms,
        "ratio": ratio if finite else None,
        "target": target,
        "tolerance": tolerance,
        # The 1e-9 keeps the band's edges inside it despite rounding.
        "pass": bool(finite and abs(ratio / target - 1.0) <= tolerance + 1e-9),
    }


def run(in_dir: Path, *, switch_frame: int, window: int, settle: int) -> dict[str, object]:
    reports: dict[str, dict[str, object]] = {}
    for name in ("timing.csv", "gpu_load_step.csv"):
        path = in_dir / name
        if path.is_file():
            reports[name] = load_step_ratio(
                gpu_frame_times(path), switch_frame=switch_frame, window=window, settle=settle
            )
    if not reports:
        raise LoadStepInputError(f"{in_dir} has no gpu_load_step.csv or timing.csv")
    source = next(iter(reports))
    report = dict(reports[source])
    report["source"] = source
    report["sources"] = {
        name: {key: item[key] for key in ("before_ms", "after_ms", "ratio", "pass")} for name, item in reports.items()
    }
    calibration = in_dir / "gpu_load_step.json"
    if calibration.is_file():
        payload = json.loads(calibration.read_text(encoding="utf-8"))
        if isinstance(payload, dict):
            report["calibration"] = payload.get("calibration")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.load_step")
    parser.add_argument("--in", dest="in_dir", required=True, help="the run folder")
    parser.add_argument("--switch-frame", type=int, default=SWITCH_FRAME)
    parser.add_argument("--window", type=int, default=WINDOW)
    parser.add_argument("--settle", type=int, default=SETTLE)
    args = parser.parse_args(argv)
    try:
        report = run(Path(args.in_dir), switch_frame=args.switch_frame, window=args.window, settle=args.settle)
    except (OSError, ValueError) as error:
        json.dump({"metric": "GT-6 load step", "pass": False, "error": str(error)}, sys.stdout, sort_keys=True)
        sys.stdout.write("\n")
        return 2
    json.dump(report, sys.stdout, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
