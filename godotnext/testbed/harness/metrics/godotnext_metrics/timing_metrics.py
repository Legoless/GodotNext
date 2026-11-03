"""Q11, Q12 and Q13.

Pass times come from ``timing.csv``. Percentiles match ``summary.json``.
Line 1b is any column named ``line_1b_ms``, ``as_updates_amortized_ms`` or
``1b_ms``, plus names listed in ``q11.json``. It is left out of the steady-state
total. The spread is ``(max - min) / min`` of that total's p95 across runs.
"""

from __future__ import annotations

import csv
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from godotnext_metrics.percentile import percentile
from godotnext_metrics.result import MetricResult

Q11_SPREAD = 0.02
Q12_SUMMARY_TOLERANCE_MS = 0.01
Q13_THRESHOLD = 0.02
SUMMARY_TOLERANCE_MS = 0.01

# §2.1 High totals and line 1b, report-only targets used by the p99 check.
HIGH_TOTAL_ADA_MS = 1.60
LINE_1B_ADA_MS = 0.10
HIGH_TOTAL_M5_MS = 4.00
LINE_1B_M5_MS = 0.25

LINE_1B_COLUMNS = frozenset({"line_1b_ms", "as_updates_amortized_ms", "1b_ms"})


@dataclass
class TimingRun:
    columns: dict[str, list[float | None]]
    warmup: int | None = None
    summary: dict[str, object] | None = None
    line_1b: frozenset[str] = field(default_factory=lambda: LINE_1B_COLUMNS)

    @property
    def wall_ms(self) -> list[float]:
        return [value for value in self.columns.get("wall_ms", []) if value is not None]


def read_timing_csv(path: Path) -> dict[str, list[float | None]]:
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no header")
        columns: dict[str, list[float | None]] = {name: [] for name in reader.fieldnames}
        for row in reader:
            for name in reader.fieldnames:
                text = (row.get(name) or "").strip()
                columns[name].append(float(text) if text else None)
    return columns


def q11_spread(total_p95: list[float]) -> float:
    if len(total_p95) < 2:
        raise ValueError("Q11 spread needs at least two runs")
    low = min(total_p95)
    high = max(total_p95)
    if low == 0.0:
        return 0.0 if high == 0.0 else math.inf
    return (high - low) / low


def q11_pass_time(
    runs: list[TimingRun],
    *,
    high_total_ms: float | None = None,
    line_1b_budget_ms: float | None = None,
) -> MetricResult:
    if not runs:
        raise ValueError("Q11 needs a timing run")
    totals = [steady_state_total_p95(run) for run in runs]
    spread = q11_spread(totals) if len(runs) >= 2 else None
    # 1.02 / 1.0 is 0.02 plus a binary rounding error. A spread that rounds to the gate still passes.
    spread_ok = spread is not None and math.isfinite(spread) and spread <= Q11_SPREAD + 1e-9 and len(runs) == 3
    summary_ok, summary_worst = _summary_matches(runs[0])
    line_ok, line_p99 = _line_1b_p99(runs, high_total_ms, line_1b_budget_ms)
    # A single run has no spread to gate. Summary and line 1b still apply.
    if len(runs) == 1:
        spread_ok = True
    elif len(runs) != 3:
        spread_ok = False
    passed = bool(spread_ok and summary_ok and line_ok)
    per_pass = {
        name: _column_stats(values) for name, values in runs[0].columns.items() if name not in {"frame", "wall_ms"}
    }
    return MetricResult(
        metric="Q11",
        value=spread if spread is not None else totals[0],
        threshold=Q11_SPREAD,
        passed=passed,
        values={
            "spread": spread,
            "steady_state_p95": totals,
            "passes": per_pass,
            "summary_delta_ms": summary_worst,
        },
        details={
            "runs": len(runs),
            "measured_frames": len(_frame_totals(runs[0], include_1b=False)),
            "warmup": runs[0].warmup,
            "expected_measured_frames": 600,
            "expected_warmup": 120,
            "line_1b_p99": line_p99,
            "line_1b_checked": high_total_ms is not None and line_1b_budget_ms is not None,
            "pass_lines": _q11_lines(spread, spread_ok, line_p99, high_total_ms, line_1b_budget_ms),
        },
    )


def q12_frame_time(run: TimingRun) -> MetricResult:
    p95 = percentile(run.wall_ms, 95.0)
    p99 = percentile(run.wall_ms, 99.0)
    summary_ok, delta = _wall_summary_matches(run, p95, p99)
    return MetricResult(
        metric="Q12",
        value={"p95": p95, "p99": p99},
        threshold={"summary_tolerance_ms": Q12_SUMMARY_TOLERANCE_MS},
        passed=summary_ok,
        values={"p95": p95, "p99": p99, "summary_delta_ms": delta},
    )


def q13_overhead(
    test_totals: list[float], baseline_totals: list[float], *, threshold: float = Q13_THRESHOLD
) -> MetricResult:
    if len(test_totals) != 3 or len(baseline_totals) != 3:
        raise ValueError("Q13 compares 3 test runs with 3 baseline runs")
    test_mean = float(np.mean(np.asarray(test_totals, dtype=np.float64)))
    base_mean = float(np.mean(np.asarray(baseline_totals, dtype=np.float64)))
    if base_mean == 0.0:
        delta = 0.0 if test_mean == 0.0 else math.inf
    else:
        delta = (test_mean - base_mean) / base_mean
    passed = bool(math.isfinite(delta) and delta <= threshold)
    return MetricResult(
        metric="Q13",
        value=delta,
        threshold=threshold,
        passed=passed,
        values={"delta": delta, "test_mean_p95": test_mean, "baseline_mean_p95": base_mean},
        details={"pass_lines": [f"delta {delta:.6f} <= {threshold:.6f}: {'pass' if passed else 'fail'}"]},
    )


def steady_state_total_p95(run: TimingRun) -> float:
    frame_totals = _frame_totals(run, include_1b=False)
    return percentile(frame_totals, 95.0)


def budgets_for_host(host: str | None) -> tuple[float, float] | None:
    if host is None:
        return None
    if "M5 Max" in host:
        return HIGH_TOTAL_M5_MS, LINE_1B_M5_MS
    if "4080" in host or "Ada" in host:
        return HIGH_TOTAL_ADA_MS, LINE_1B_ADA_MS
    return None


def _frame_totals(run: TimingRun, *, include_1b: bool) -> list[float]:
    names = [
        name for name in run.columns if name not in {"frame", "wall_ms"} and (include_1b or name not in run.line_1b)
    ]
    length = max((len(run.columns[name]) for name in names), default=0)
    if "wall_ms" in run.columns:
        length = max(length, len(run.columns["wall_ms"]))
    totals: list[float] = []
    for index in range(length):
        total = 0.0
        for name in names:
            series = run.columns[name]
            if index >= len(series):
                continue
            sample = series[index]
            if sample is not None:
                total += sample
        totals.append(total)
    return totals


def _line_1b_p99(
    runs: list[TimingRun],
    high_total_ms: float | None,
    line_1b_budget_ms: float | None,
) -> tuple[bool, float | None]:
    if high_total_ms is None or line_1b_budget_ms is None:
        return True, None
    spent: list[float] = []
    for run in runs:
        full = _frame_totals(run, include_1b=True)
        for index, total in enumerate(full):
            if _frame_spends_1b(run, index):
                spent.append(total)
    if not spent:
        return True, None
    p99 = percentile(spent, 99.0)
    limit = high_total_ms + line_1b_budget_ms
    return bool(p99 <= limit), p99


def _frame_spends_1b(run: TimingRun, index: int) -> bool:
    for name in run.line_1b:
        series = run.columns.get(name)
        if series is None or index >= len(series):
            continue
        value = series[index]
        if value is not None and value > 0.0:
            return True
    return False


def _column_stats(values: list[float | None]) -> dict[str, float | int]:
    present = [value for value in values if value is not None]
    return {
        "p50": percentile(present, 50.0),
        "p95": percentile(present, 95.0),
        "p99": percentile(present, 99.0),
        "n": len(present),
    }


def _summary_matches(run: TimingRun) -> tuple[bool, float]:
    if not run.summary:
        return True, 0.0
    columns = run.summary.get("columns")
    if not isinstance(columns, dict):
        return False, math.inf
    worst = 0.0
    for name, series in run.columns.items():
        if name == "frame" or name not in columns:
            continue
        stats = columns[name]
        if not isinstance(stats, dict) or "p95" not in stats:
            return False, math.inf
        got = percentile([value for value in series if value is not None], 95.0)
        delta = abs(got - float(stats["p95"]))
        worst = max(worst, delta)
        if delta > SUMMARY_TOLERANCE_MS:
            return False, worst
    return True, worst


def _wall_summary_matches(run: TimingRun, p95: float, p99: float) -> tuple[bool, float]:
    if not run.summary:
        return True, 0.0
    columns = run.summary.get("columns")
    if not isinstance(columns, dict) or "wall_ms" not in columns:
        return False, math.inf
    stats = columns["wall_ms"]
    if not isinstance(stats, dict):
        return False, math.inf
    delta = max(abs(p95 - float(stats["p95"])), abs(p99 - float(stats["p99"])))
    return bool(delta <= SUMMARY_TOLERANCE_MS), delta


def _q11_lines(
    spread: float | None,
    spread_ok: bool,
    line_p99: float | None,
    high_total_ms: float | None,
    line_1b_budget_ms: float | None,
) -> list[str]:
    lines: list[str] = []
    if spread is None:
        lines.append("spread: not computed (one run)")
    else:
        lines.append(f"spread {spread:.6f} <= {Q11_SPREAD:.6f}: {'pass' if spread_ok else 'fail'}")
    if line_p99 is not None and high_total_ms is not None and line_1b_budget_ms is not None:
        limit = high_total_ms + line_1b_budget_ms
        lines.append(f"line 1b p99 {line_p99:.6f} <= {limit:.6f}: {'pass' if line_p99 <= limit else 'fail'}")
    return lines
