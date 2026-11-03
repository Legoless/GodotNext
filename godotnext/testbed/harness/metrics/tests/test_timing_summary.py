"""Pytest cases for the §2.1 summary fields. Keep the fixture in sync with test_args.gd."""

from __future__ import annotations

import json
from typing import Any

from godotnext_metrics.percentile import percentile
from godotnext_metrics.timing_summary import timing_fields


def _row(passes: list[tuple[str, float]], counters: dict[str, Any] | None = None) -> dict[str, Any]:
    row: dict[str, Any] = {
        "wall_ms": 1.0,
        "passes": [{"column": column, "ms": ms} for column, ms in passes],
    }
    if counters is not None:
        row["counters"] = counters
    return row


def _fixture() -> list[dict[str, Any]]:
    return [
        _row([("as_updates_ms", 1.0), ("src_capture_ms", 0.4)]),
        _row(
            [("as_updates_ms", 3.0), ("src_capture_ms", 0.4), ("blas_build_ms", 5.0)],
            {"blas_builds": 2},
        ),
        _row([("as_updates_ms", 2.0), ("src_capture_ms", 0.8)]),
        _row([("as_updates_ms", 4.0), ("src_capture_ms", 0.8), ("compaction_copy_ms", 1.0)]),
    ]


def test_fixture_lines_match_the_percentile_helper() -> None:
    fields = timing_fields(_fixture(), host="Apple M5 Max", adapter="Apple M5 Max")
    lines = fields["lines"]
    assert lines["1a"]["n"] == 2
    assert lines["1a"]["p95"] == percentile([1.0, 2.0], 95.0)
    assert lines["1a"]["p50"] == percentile([1.0, 2.0], 50.0)
    assert lines["1a"]["p99"] == percentile([1.0, 2.0], 99.0)
    assert lines["2a"]["p95"] == percentile([0.4, 0.8], 95.0)
    assert lines["1b"]["n"] == 2
    assert lines["1b"]["p95"] == percentile([5.0, 1.0], 95.0)
    assert fields["diffuse_path"]["p95"] == percentile([1.4, 2.8], 95.0)
    assert fields["diffuse_path"]["n"] == 2
    tagged = fields["line_1b_frames"]
    assert tagged["frames"] == [1, 3]
    assert tagged["count"] == 2
    assert tagged["p99"] == percentile([8.4, 5.8], 99.0)
    assert tagged["device"] == "Apple M5 Max"
    assert tagged["high_total_ms"] == 4.0
    assert tagged["line_1b_budget_ms"] == 0.25
    assert tagged["limit_ms"] == 4.25
    assert tagged["within_limit"] is False


def test_counter_only_tag_and_alias_columns() -> None:
    rows = [
        _row([("as_updates_ms", 1.0)], {"blas_build_count": 1}),
        _row([("tlas_rebuild_ms", 1.0), ("skinned_refit_ms", 0.5)]),
        _row([("as_updates_2_ms", 9.0), ("as_updates_ms", 1.0), ("as_updates_ms", 4.0)]),
    ]
    fields = timing_fields(rows)
    assert fields["line_1b_frames"]["frames"] == [0]
    # Frame 0 is tagged, so steady 1a is the alias sum and the last-wins column.
    assert fields["lines"]["1a"]["n"] == 2
    assert fields["lines"]["1a"]["p50"] == percentile([1.5, 4.0], 50.0)
    assert fields["lines"]["1b"]["n"] == 1
    assert fields["lines"]["1b"]["p95"] == 0.0


def test_counter_aggregation_uses_aliases_and_stays_null_when_absent() -> None:
    rows = [
        _row(
            [],
            {
                "src_relight_texels": 10,
                "shadow_rays_traced": 1,
                "rt_coverage": 0.5,
                "tlas_fill_ms": 0.2,
                "capture_cpu_ms": 0.1,
            },
        ),
        _row(
            [],
            {
                "relight_texels": 30,
                "shadow_rays": 3,
                "q18": 0.5,
                "tlas_fill_ms": 0.4,
                "capture_cpu_ms": 0.3,
            },
        ),
    ]
    present = timing_fields(rows)
    counters = present["counters"]
    assert counters["source"] == "nextgi_get_counters"
    assert counters["relight_texels"]["p95"] == percentile([10.0, 30.0], 95.0)
    assert counters["shadow_rays"]["n"] == 2
    assert counters["q18"]["p50"] == percentile([0.5, 0.5], 50.0)
    assert counters["tlas_fill_ms"]["p95"] == percentile([0.2, 0.4], 95.0)
    assert counters["capture_cpu_ms"]["p99"] == percentile([0.1, 0.3], 99.0)

    absent = timing_fields([_row([])], counters_present=False)
    assert absent["counters"]["source"] == "absent"
    for key in ("relight_texels", "shadow_rays", "q18", "tlas_fill_ms", "capture_cpu_ms"):
        assert absent["counters"][key] is None

    forced = timing_fields([_row([])], counters_present=True)
    assert forced["counters"]["source"] == "nextgi_get_counters"
    assert forced["counters"]["relight_texels"]["n"] == 1
    assert forced["counters"]["relight_texels"]["p95"] == 0.0


def test_device_budgets_and_no_mid_range_keys() -> None:
    rows = _fixture()
    ada = timing_fields(rows, host="", adapter="RTX 4080 SUPER")
    assert ada["line_1b_frames"]["device"] == "RTX 4080 SUPER"
    assert ada["line_1b_frames"]["high_total_ms"] == 1.6
    assert ada["line_1b_frames"]["line_1b_budget_ms"] == 0.1
    assert ada["line_1b_frames"]["limit_ms"] == 1.7

    blackwell = timing_fields(rows, host="RTX 5090", adapter="RTX 4080 SUPER")
    assert blackwell["line_1b_frames"]["device"] == "RTX 5090"
    assert blackwell["line_1b_frames"]["limit_ms"] is None
    assert blackwell["line_1b_frames"]["high_total_ms"] is None
    assert blackwell["line_1b_frames"]["within_limit"] is None

    unknown = timing_fields(rows, host="RTX 3060", adapter="")
    assert unknown["line_1b_frames"]["device"] == ""
    assert unknown["line_1b_frames"]["limit_ms"] is None
    assert unknown["line_1b_frames"]["within_limit"] is None
    blob = json.dumps(unknown)
    assert "3060" not in blob
    assert "mid_range" not in blob
    assert "mid-range" not in blob

    apple = timing_fields(rows, host="lab", adapter="Apple M5 Max")
    assert apple["line_1b_frames"]["device"] == "Apple M5 Max"
    assert apple["line_1b_frames"]["limit_ms"] == 4.25


def test_summary_keys_exist() -> None:
    fields = timing_fields(_fixture())
    assert list(fields["lines"]) == ["1a", "1b", "2a", "2b", "3", "4", "5", "7"]
    for key in ("1a", "1b", "2a", "2b", "3", "4", "5", "7"):
        assert set(fields["lines"][key]) == {"name", "p50", "p95", "p99", "n"}
    assert set(fields["counters"]) == {
        "source",
        "relight_texels",
        "shadow_rays",
        "q18",
        "tlas_fill_ms",
        "capture_cpu_ms",
    }
    assert fields["diffuse_path"]["columns"] == ["1a", "2a", "2b", "3", "4", "5", "7"]
