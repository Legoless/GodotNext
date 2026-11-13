"""AS calibration derivation (#9): field check, lines 1a and 1b, spread and the tool cross-check."""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

import pytest
from godotnext_metrics.as_calibration import (
    derive_card,
    line_1a_ms,
    main,
    markdown_table,
    missing_fields,
    triangle_cap,
)


def _run(tlas_p95: float = 0.05, blas_tpm: float = 2_500_000.0, refit_tpm: float = 4_000_000.0) -> dict[str, Any]:
    def tlas_point(p95: float) -> dict[str, Any]:
        return {"p50_ms": p95 * 0.9, "p95_ms": p95, "cpu_p50_ms": 1.0, "cpu_p95_ms": 1.2, "memory_bytes": 1000}

    def blas_entry(tpm: float) -> dict[str, Any]:
        return {
            "triangles_per_ms": tpm,
            "p95_ms_total": 2.0,
            "buckets": {"1k-8k": {"triangles_per_ms": tpm * 0.5}, "32k-64k": {"triangles_per_ms": tpm * 1.2}},
            "memory_bytes": 5000,
        }

    return {
        "blas": {"fast_trace": blas_entry(blas_tpm), "fast_build": blas_entry(blas_tpm * 2.0)},
        "tlas": {
            "10000": {"fast_trace": tlas_point(tlas_p95 / 4), "fast_build": tlas_point(tlas_p95 / 5)},
            "50000": {"fast_trace": tlas_point(tlas_p95), "fast_build": tlas_point(tlas_p95 * 0.8)},
            "100000": {"fast_trace": tlas_point(tlas_p95 * 2), "fast_build": tlas_point(tlas_p95 * 1.6)},
        },
        "refit_rd": {
            "10000": {"triangles_per_ms": refit_tpm * 0.5, "p95_ms": 0.01},
            "200000": {"triangles_per_ms": refit_tpm, "p95_ms": 0.05},
        },
    }


def _result(runs: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {
        "test": "as_calibration",
        "adapter": "NVIDIA GeForce RTX 4080 SUPER",
        "driver": "vulkan",
        "tlas_sizes": [10000, 50000, 100000],
        "refit_sizes": [10000, 200000],
        "runs": runs if runs is not None else [_run()],
    }


def test_complete_result_has_no_missing_fields() -> None:
    assert missing_fields(_result()) == []


def test_missing_fields_are_named() -> None:
    result = _result()
    run = result["runs"][0]
    del run["tlas"]["50000"]["fast_build"]["p95_ms"]
    del run["blas"]["fast_trace"]["buckets"]
    del run["refit_rd"]["200000"]
    gaps = missing_fields(result)
    assert "runs[0].tlas.50000.fast_build.p95_ms" in gaps
    assert "runs[0].blas.fast_trace.buckets" in gaps
    assert "runs[0].refit_rd.200000" in gaps


def test_empty_runs_are_missing() -> None:
    assert "runs[0]" in missing_fields(_result(runs=[]))


def test_triangle_cap_rounds_down_to_10k() -> None:
    # 2.5M triangles/ms x 0.10 ms = 250k; 2.549M gives 254.9k, rounded down to 250k.
    assert triangle_cap(2_500_000.0) == 250_000
    assert triangle_cap(2_549_000.0) == 250_000
    assert triangle_cap(99_999.0) == 0


def test_line_1a_adds_the_skinned_refit_budget() -> None:
    # 0.05 ms TLAS + 200k triangles / 4M triangles per ms = 0.05 + 0.05.
    assert line_1a_ms(0.05, 4_000_000.0) == pytest.approx(0.10)
    assert line_1a_ms(0.05, 0.0) is None


def test_derive_card_lines_and_buckets() -> None:
    card = derive_card(_result())
    assert card["line_1b_triangle_cap"] == 250_000
    assert card["line_1a"]["50000"]["ms"] == pytest.approx(0.10)
    assert card["line_1a"]["50000"]["within_target"] is True
    assert card["line_1a"]["100000"]["ms"] == pytest.approx(0.15)
    assert card["line_1a"]["100000"]["within_target"] is False
    assert card["refit_source"] == "rd"
    assert card["blas"]["fast_build"]["buckets"]["32k-64k"] == pytest.approx(6_000_000.0)


def test_spread_uses_the_median_of_runs() -> None:
    fast = _run(tlas_p95=0.050)
    slow = _run(tlas_p95=0.051)
    middle = _run(tlas_p95=0.0505)
    card = derive_card(_result(runs=[fast, slow, middle]))
    assert card["tlas"]["50000"]["fast_trace"]["p95_ms"] == pytest.approx(0.0505)
    assert card["spread_percent"]["tlas.50000.fast_trace.p95"] == pytest.approx(0.001 / 0.0505 * 100.0)
    assert card["runs"] == 3


def test_tool_refit_replaces_rd_and_is_cross_checked() -> None:
    tool = {
        "refit_triangles_per_ms": 2_000_000.0,
        "tlas_100k_p95_ms": 0.11,
        "blas_fast_trace_triangles_per_ms": 2_000_000.0,
    }
    card = derive_card(_result(), tool)
    assert card["refit_source"] == "tool"
    assert card["line_1a"]["50000"]["ms"] == pytest.approx(0.05 + 0.1)
    check = card["tool_cross_check"]
    assert check["tlas_100k_p95_ms"]["within_15_percent"] is True
    assert check["blas_fast_trace_triangles_per_ms"]["within_15_percent"] is False


def test_markdown_table_has_one_column_per_card() -> None:
    other = copy.deepcopy(_result())
    other["adapter"] = "NVIDIA GeForce RTX 5090"
    table = markdown_table([derive_card(_result()), derive_card(other)])
    assert "| Measure | NVIDIA GeForce RTX 4080 SUPER | NVIDIA GeForce RTX 5090 |" in table
    assert "Line 1b triangle cap" in table


def test_cli_writes_outputs_and_fails_on_missing_fields(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    good = tmp_path / "good.json"
    good.write_text(json.dumps(_result()), encoding="utf-8")
    out = tmp_path / "out"
    assert main([str(good), "--out", str(out)]) == 0
    assert (out / "as_calibration.json").is_file()
    assert (out / "as_calibration.md").is_file()
    capsys.readouterr()
    broken = _result()
    del broken["runs"][0]["blas"]
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(broken), encoding="utf-8")
    assert main([str(bad)]) == 1
    summary = json.loads(capsys.readouterr().out)
    assert summary["pass"] is False
    assert "runs[0].blas.fast_trace" in summary["missing_fields"][str(bad)]
