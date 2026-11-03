"""Synthetic tests for the #74 streaming reports."""

from __future__ import annotations

import json
import math
import os
import subprocess
import sys
from pathlib import Path

from godotnext_metrics.commands import COMMANDS
from godotnext_metrics.flight_reports import (
    exclusion_report,
    q14_flight_report,
    render_markdown,
    run_envelope,
    slow_io_report,
)
from godotnext_metrics.geometry_memory import (
    EXPORT_BASELINE,
    EXPORT_RETAIN,
    EXPORT_STRIP,
    Q15_CATEGORIES,
    memory_report,
)
from godotnext_metrics.percentile import percentile
from godotnext_metrics.result import json_ready
from godotnext_metrics.streaming_reports import main

_METRICS_ROOT = Path(__file__).resolve().parents[1]


def _categories(
    total_in_first: float,
    rt: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0),
) -> dict[str, float]:
    values = {name: 0.0 for name in Q15_CATEGORIES}
    values["authored_remainder"] = total_in_first
    values["rt_arena"] = rt[0]
    values["blas"] = rt[1]
    values["tlas"] = rt[2]
    values["as_scratch"] = rt[3]
    return values


def _memory(
    strip: dict[str, float] | None,
    baseline: dict[str, float] | None,
    retain: dict[str, float] | None,
) -> dict[str, object]:
    def row(source: str, categories: dict[str, float] | None) -> dict[str, object]:
        if categories is None:
            return {"source": "absent", "categories": {name: None for name in Q15_CATEGORIES}}
        return {"source": source, "categories": categories}

    present = strip is not None or baseline is not None or retain is not None
    return {
        "schema": 1,
        "warmup_frames": 120,
        "source": "virtual_geometry_get_memory_report" if present else "absent",
        "exports": {
            EXPORT_STRIP: row("virtual_geometry_get_memory_report" if strip is not None else "absent", strip),
            EXPORT_BASELINE: row("virtual_geometry_get_memory_report" if baseline is not None else "absent", baseline),
            EXPORT_RETAIN: row("virtual_geometry_get_memory_report" if retain is not None else "absent", retain),
        },
    }


def test_q15_category_order() -> None:
    assert Q15_CATEGORIES == (
        "authored_remainder",
        "lods",
        "shadow_mesh",
        "coarse_fallback",
        "rt_arena",
        "blas",
        "tlas",
        "as_scratch",
        "dag_headers",
        "page_cache",
    )
    assert len(Q15_CATEGORIES) == 10


def test_q15_absent_lists_ten_nulls() -> None:
    report = memory_report(None)
    assert report["source"] == "absent"
    assert report["ratio"] is None
    assert report["rt_share"] is None
    assert report["retain_authored"] is None
    assert report["pass"] is None
    assert report["retain_authored_gated"] is False
    exports = report["exports"]
    assert isinstance(exports, dict)
    for name in (EXPORT_STRIP, EXPORT_BASELINE, EXPORT_RETAIN):
        row = exports[name]
        assert isinstance(row, dict)
        categories = row["categories"]
        assert isinstance(categories, dict)
        assert list(categories) == list(Q15_CATEGORIES)
        assert all(value is None for value in categories.values())


def test_q15_ratio_pass_and_fail() -> None:
    passing = memory_report(_memory(_categories(100.0), _categories(400.0), _categories(9999.0)))
    assert passing["ratio"] == 0.25
    assert passing["pass"] is True
    assert passing["retain_authored"] == 9999.0
    failing = memory_report(_memory(_categories(300.0), _categories(400.0), None))
    assert failing["ratio"] == 0.75
    assert failing["pass"] is False


def test_q15_rt_share() -> None:
    strip = _categories(100.0, (10.0, 20.0, 30.0, 40.0))
    report = memory_report(_memory(strip, _categories(400.0), None))
    assert report["rt_share"] == 0.5
    assert report["ratio"] == 200.0 / 400.0


def test_q15_missing_category_fails() -> None:
    categories = _categories(100.0)
    del categories["page_cache"]
    report = memory_report(_memory(categories, _categories(400.0), None))
    assert report["pass"] is False
    assert report["ratio"] is None
    exports = report["exports"]
    assert isinstance(exports, dict)
    strip = exports[EXPORT_STRIP]
    assert isinstance(strip, dict)
    assert strip["total"] is None


def test_q15_zero_and_divide_by_zero() -> None:
    zeros = memory_report(_memory(_categories(0.0), _categories(0.0), None))
    assert zeros["ratio"] == 0.0
    assert zeros["pass"] is True
    broken = memory_report(_memory(_categories(10.0), _categories(0.0), None))
    assert broken["pass"] is False
    broken_ratio = broken["ratio"]
    assert isinstance(broken_ratio, float)
    assert not math.isfinite(broken_ratio)
    encoded = json_ready(broken)
    assert isinstance(encoded, dict)
    assert encoded["ratio"] is None


def test_q14_mean_and_maximum() -> None:
    passing = q14_flight_report({
        "source": "routing",
        "frames": [{"fallback_coverage": 0.1}, {"fallback_coverage": 0.19}],
    })
    assert passing["pass"] is True
    assert passing["mean"] == (0.1 + 0.19) / 2.0
    assert passing["maximum"] == 0.19
    failing = q14_flight_report({
        "source": "routing",
        "frames": [{"fallback_coverage": 0.1}, {"fallback_coverage": 0.20}],
    })
    assert failing["pass"] is False
    assert failing["maximum"] == 0.20


def test_q14_absent_and_pixel_ratios() -> None:
    empty = q14_flight_report({"source": "absent", "frames": []})
    assert empty["source"] == "absent"
    assert empty["mean"] is None
    assert empty["maximum"] is None
    assert empty["pass"] is None
    zero = q14_flight_report({
        "source": "routing",
        "frames": [{"legacy_drawn_virtualized_pixels": 0, "opaque_pixels": 0}],
    })
    assert zero["mean"] == 0.0
    assert zero["pass"] is True
    infinite = q14_flight_report({
        "source": "routing",
        "frames": [{"legacy_drawn_virtualized_pixels": 5, "opaque_pixels": 0}],
    })
    assert infinite["pass"] is False
    assert infinite["mean"] is None


def test_slow_io_percentiles_and_delay_record() -> None:
    samples = [float(value) for value in range(1, 101)]
    uploads = [1.0, 2.0, 3.0, 4.0, 10.0]
    report = slow_io_report(
        {
            "variant": "delayed",
            "page_upload_source": "absent",
            "debug_io_delay_ms": {
                "distribution": "uniform",
                "min_ms": 50,
                "max_ms": 250,
                "applied_in": "wall_time",
                "per_request": True,
                "gate": False,
            },
            "page_upload_ms": uploads,
        },
        samples,
        None,
    )
    frame = report["frame_time_ms"]
    assert isinstance(frame, dict)
    assert frame["p50"] == percentile(samples, 50.0)
    assert frame["p95"] == percentile(samples, 95.0)
    assert frame["p99"] == percentile(samples, 99.0)
    assert report["page_upload_ms_p95"] == percentile(uploads, 95.0)
    assert report["record_complete"] is True
    assert report["pass"] is None
    assert report["timing_gate"] == "PQ"
    rejected = slow_io_report({"debug_io_delay_ms": {"min_ms": 50, "max_ms": 250, "applied_in": "frames"}}, None, None)
    assert rejected["record_complete"] is False
    nodelay = slow_io_report(
        {"variant": "nodelay", "debug_io_delay_ms": {"min_ms": 0, "max_ms": 0, "applied_in": "wall_time"}},
        None,
        None,
    )
    delay = nodelay["debug_io_delay_ms"]
    assert isinstance(delay, dict)
    assert delay["min_ms"] == 0
    assert delay["max_ms"] == 0
    assert "p50" in frame
    nodelay_frame = nodelay["frame_time_ms"]
    assert isinstance(nodelay_frame, dict)
    assert nodelay_frame["p50"] is None
    assert nodelay_frame["p95"] is None
    assert nodelay_frame["p99"] is None
    assert nodelay["page_upload_ms_p95"] is None
    assert nodelay["record_complete"] is True


def test_exclusion_counts_and_runtime_mismatch() -> None:
    cells = {"schema": 1, "cells": [{"id": "opaque", "surfaces": [{"route": "virtual", "reason": "NONE"}]}]}
    clean = exclusion_report(
        cells,
        {"counters_source": "absent", "double_drawn": 0, "missing": 0, "cells": cells["cells"]},
    )
    assert clean["pass"] is True
    assert clean["double_drawn"] == 0
    assert clean["missing"] == 0
    doubled = exclusion_report(cells, {"counters_source": "absent", "double_drawn": 1, "missing": 0})
    assert doubled["pass"] is False
    wrong = {
        "counters_source": "virtual_geometry_get_debug_stats",
        "double_drawn": 0,
        "missing": 0,
        "debug_stats": {"cells": [{"id": "opaque", "surfaces": [{"route": "legacy", "reason": "ALPHA"}]}]},
    }
    mismatched = exclusion_report(cells, wrong)
    assert mismatched["pass"] is False
    assert mismatched["mismatches"]
    ignored = exclusion_report(cells, {**wrong, "counters_source": "absent"})
    assert ignored["pass"] is True
    assert ignored["mismatches"] == []
    dlss = exclusion_report(None, None)
    record = dlss["dlss"]
    assert isinstance(record, dict)
    assert record["status"] == "N/A"
    assert record["dll_sha256"] == ""
    assert record["fallback"] == "FSR2"
    assert dlss["pass"] is None


def test_markdown_names_the_q15_fields() -> None:
    report = memory_report(_memory(_categories(100.0), _categories(400.0), _categories(50.0)))
    text = render_markdown(report)
    assert "authored_remainder" in text
    assert "retain_authored" in text
    assert "rt_share" in text


def test_envelope_leaves_gpu_index_null_without_a_command_line() -> None:
    envelope = run_envelope({"host": "the Apple M5 Max", "adapter": {"name": "Apple GPU", "api_version": "Metal"}})
    assert envelope["host"] == "the Apple M5 Max"
    assert envelope["gpu"] == "Apple GPU"
    assert envelope["driver"] == "Metal"
    assert envelope["gpu_index"] is None
    indexed = run_envelope({"command_line": {"engine": "godot --gpu-index 1"}, "commit": "abc", "seed": 1})
    assert indexed["gpu_index"] == 1
    assert indexed["build_hash"] == "abc"


def test_commands_stay_on_the_frozen_set() -> None:
    expected = {f"q{index}" for index in range(1, 22) if index != 4} | {"q4a", "q4b"}
    assert set(COMMANDS) == expected


def test_cli_exit_codes(tmp_path: Path) -> None:
    env = os.environ.copy()
    env["PYTHONPATH"] = str(_METRICS_ROOT)
    missing = subprocess.run(
        [sys.executable, "-m", "godotnext_metrics.streaming_reports", "q15", "--in", str(tmp_path / "missing")],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert missing.returncode == 2
    run = tmp_path / "run"
    run.mkdir()
    (run / "memory_report.json").write_text("{", encoding="utf-8")
    malformed = subprocess.run(
        [sys.executable, "-m", "godotnext_metrics.streaming_reports", "q15", "--in", str(run)],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert malformed.returncode == 2
    (run / "memory_report.json").write_text(json.dumps(_memory(None, None, None)), encoding="utf-8")
    out = tmp_path / "out"
    produced = subprocess.run(
        [
            sys.executable,
            "-m",
            "godotnext_metrics.streaming_reports",
            "q15",
            "--in",
            str(run),
            "--out",
            str(out),
            "--markdown",
        ],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert produced.returncode == 0
    payload = json.loads(produced.stdout)
    assert payload["pass"] is None
    assert (out / "geometry_memory.json").is_file()
    summary = (out / "geometry_memory.md").read_text(encoding="utf-8")
    assert "authored_remainder" in summary
    assert main(["q14", "--in", str(run)]) == 0
