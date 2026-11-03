"""Harness percentile contract and the metric-id rule."""

from __future__ import annotations

import re
from pathlib import Path

from godotnext_metrics.percentile import percentile
from godotnext_metrics.rays import RECORD_DTYPE
from godotnext_metrics.timing_metrics import TimingRun, q11_pass_time, q12_frame_time

_METRIC_ID = re.compile(r"\bM(?:1[0-7]|[1-9])\b")


def test_percentile_matches_the_harness_contract() -> None:
    samples = [float(value) for value in range(1, 101)]
    # manifest.gd / tests/test_args.gd: p95 of 1..100 is 95.05.
    assert percentile(samples, 95.0) == 95.05
    assert percentile(samples, 99.0) == 99.01
    assert percentile([], 95.0) == 0.0
    assert percentile([4.0], 95.0) == 4.0


def test_q11_and_q12_match_summary_json_within_a_hundredth_of_a_millisecond() -> None:
    samples: list[float | None] = [float(value) for value in range(1, 101)]
    summary = {
        "schema": 1,
        "frames": 100,
        "warmup": 120,
        "columns": {
            "wall_ms": {"p50": percentile(samples, 50.0), "p95": 95.05, "p99": 99.01, "n": 100},
            "gi_ms": {"p50": percentile(samples, 50.0), "p95": 95.05, "p99": 99.01, "n": 100},
        },
    }
    run = TimingRun(
        columns={"frame": list(range(100)), "wall_ms": samples, "gi_ms": samples}, warmup=120, summary=summary
    )
    q11 = q11_pass_time([run])
    q12 = q12_frame_time(run)
    passes = q11.values["passes"]
    assert isinstance(passes, dict)
    gi = passes["gi_ms"]
    assert isinstance(gi, dict)
    gi_p95 = gi["p95"]
    assert isinstance(gi_p95, float)
    assert abs(gi_p95 - 95.05) <= 0.01
    q12_p95 = q12.values["p95"]
    q12_p99 = q12.values["p99"]
    assert isinstance(q12_p95, float) and isinstance(q12_p99, float)
    assert abs(q12_p95 - 95.05) <= 0.01
    assert abs(q12_p99 - 99.01) <= 0.01
    assert q11.passed
    assert q12.passed
    assert q11.details["expected_measured_frames"] == 600
    assert q11.details["expected_warmup"] == 120


def test_ray_record_is_32_bytes() -> None:
    assert RECORD_DTYPE.itemsize == 32


def test_package_and_metrics_md_have_no_m1_through_m17_metric_ids() -> None:
    """Milestone numbers are not metric ids. The hardware name M5 Max is not one either."""
    package = Path(__file__).resolve().parents[1] / "godotnext_metrics"
    document = Path(__file__).resolve().parents[5] / "docs" / "godotnext" / "metrics.md"
    texts = [path.read_text(encoding="utf-8") for path in package.rglob("*.py")]
    texts.append(document.read_text(encoding="utf-8"))
    for text in texts:
        # The verbatim §13.4 text names the milestone range M0-M8 and the Apple M5 Max.
        scrubbed = text.replace("M5 Max", "").replace("M0\u2013M8", "")
        assert _METRIC_ID.search(scrubbed) is None
    from godotnext_metrics.commands import COMMANDS

    expected = {f"q{index}" for index in range(1, 22) if index != 4} | {"q4a", "q4b"}
    assert set(COMMANDS) == expected
