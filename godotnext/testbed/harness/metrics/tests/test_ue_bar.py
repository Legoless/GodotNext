"""The UE bar verdict logic (#229, plan §23.3 and §23.4) on synthetic numbers."""

from __future__ import annotations

from dataclasses import replace

import pytest
from godotnext_metrics.percentile import percentile
from godotnext_metrics.ue_bar import (
    DRY_RUN_NOTE,
    LINUX_REASON,
    MAX_MISSING_SHARE,
    UNNAMED_UE_BAR_NOTE,
    ComparisonError,
    GodotNextSelection,
    RunRecord,
    bar_passes,
    bar_ratio,
    check_set,
    evaluate_line,
    interleave_order,
    median_of_runs,
    parse_line,
    run_statistic,
)


def _line(**overrides: object) -> dict[str, object]:
    line: dict[str, object] = {
        "id": "total",
        "name": "Feature total",
        "gates": ["GX-1"],
        "status": "ue_bar",
        "statistic": "p95",
        "direction": "lower",
        "godotnext": {"columns": ["feature_ms"]},
        "ue": {"group": "Feature", "columns": ["Synthetic/GPU/Feature"]},
    }
    line.update(overrides)
    return line


def _records(runs: int = 5, warmup: bool = True, **ue: object) -> list[RunRecord]:
    records = []
    for role, engine, index in interleave_order(runs, warmup):
        record = RunRecord(
            engine=engine,
            role=role,
            index=index,
            exit_code=0,
            resolution=(1920, 1080) if role == "timing" else None,
            clocks_locked=True,
            godotnext_commit="abc123" if engine == "godotnext" else "",
            godotnext_editor_binary=False if engine == "godotnext" else None,
            ue_build_version="++Synthetic+Release-0.0-CL-1" if engine == "ue" else "",
            ue_run_mode="editor_game" if engine == "ue" else "",
            ue_vsync="0" if engine == "ue" else "",
        )
        if engine == "ue" and role == "timing" and ue:
            record = replace(record, **ue)  # type: ignore[arg-type]
        records.append(record)
    return records


def test_run_statistic_is_the_harness_percentile_over_the_window() -> None:
    numbers = [float(value) for value in range(1, 101)]
    values: list[float | None] = list(numbers)
    assert run_statistic(values, "p95", 100) == (percentile(numbers, 95.0), 0)
    assert run_statistic(values, "p95", 100)[0] == 95.05
    assert run_statistic(values, "p99", 100) == (99.01, 0)
    assert run_statistic(values, "p50", 100) == (50.5, 0)
    with pytest.raises(ComparisonError, match="has 100 frames, not 120"):
        run_statistic(values, "p95", 120)
    with pytest.raises(ComparisonError, match="unknown statistic"):
        run_statistic(values, "mean", 100)


def test_frames_without_a_value_are_dropped_and_counted_not_zero() -> None:
    values: list[float | None] = [1.0] * 98 + [None, None]
    # Two empty frames as 0 ms would pull the p50 down; dropped, the statistic is over 98 frames.
    assert run_statistic(values, "p50", 100) == (1.0, 2)
    allowed = int(MAX_MISSING_SHARE * 100)
    too_many: list[float | None] = [1.0] * (99 - allowed) + [None] * (allowed + 1)
    with pytest.raises(ComparisonError, match=f"{allowed + 1} of 100 frames"):
        run_statistic(too_many, "p95", 100)
    with pytest.raises(ComparisonError, match="have no value"):
        run_statistic([None, None], "p95", 2)


def test_median_of_five_runs() -> None:
    assert median_of_runs([3.0, 1.0, 5.0, 2.0, 4.0]) == 3.0
    # Ties.
    assert median_of_runs([2.0, 2.0, 2.0, 1.0, 9.0]) == 2.0
    with pytest.raises(ComparisonError):
        median_of_runs([])


def test_the_bar_is_inclusive_at_exactly_one_point_two() -> None:
    assert bar_passes(1.2, 1.0, "lower")
    assert not bar_passes(1.2001, 1.0, "lower")
    # 1.368 / 1.14 is 1.2000000000000002 in binary; it rounds to the bar and passes.
    assert 1.368 / 1.14 > 1.2
    assert bar_passes(1.368, 1.14, "lower")
    assert not bar_passes(1.369, 1.14, "lower")
    assert bar_passes(0.5, 3.0, "lower")


def test_higher_is_better_passes_at_ue_divided_by_one_point_two() -> None:
    assert bar_passes(2.5, 3.0, "higher")
    assert not bar_passes(2.4999, 3.0, "higher")
    assert bar_passes(10.0, 3.0, "higher")
    assert bar_ratio(2.5, 3.0) == pytest.approx(2.5 / 3.0)


def test_a_zero_ue_value_cannot_make_a_ratio() -> None:
    with pytest.raises(ComparisonError, match="positive UE value"):
        bar_ratio(1.0, 0.0)


def test_judged_ue_bar_line_compares_the_medians() -> None:
    spec = parse_line(_line())
    godotnext = [1.10, 1.30, 1.20, 1.25, 1.15]
    ue = [1.00, 0.95, 1.05, 1.02, 0.98]
    row = evaluate_line(spec, godotnext, ue, judge=True)
    assert row["godotnext"] == {
        "selection": {"columns": ["feature_ms"]},
        "runs": godotnext,
        "missing_frames": [],
        "median": 1.20,
    }
    assert row["ue"]["median"] == 1.00  # type: ignore[index]
    assert row["ratio"] == pytest.approx(1.20)
    assert row["verdict"] == "pass"
    assert row["bar"] == 1.2
    failing = evaluate_line(spec, [1.30, 1.40, 1.35, 1.25, 1.45], ue, judge=True)
    assert failing["verdict"] == "fail"


def test_judged_higher_is_better_line() -> None:
    spec = parse_line(_line(direction="higher", statistic="p50"))
    row = evaluate_line(spec, [80.0] * 5, [100.0] * 5, judge=True)
    assert row["verdict"] == "fail"
    row = evaluate_line(spec, [84.0] * 5, [100.0] * 5, judge=True)
    assert row["verdict"] == "pass"


def test_report_only_line_has_both_numbers_and_no_verdict() -> None:
    spec = parse_line(_line(status="report_only", reason="Per-pass line; its parent total carries the bar."))
    row = evaluate_line(spec, [2.0] * 5, [1.0] * 5, judge=True)
    assert row["status"] == "report_only"
    assert row["ratio"] == 2.0
    assert row["verdict"] is None
    assert row["bar"] is None
    assert row["reason"] == "Per-pass line; its parent total carries the bar."


def test_no_equivalent_line_has_only_the_godotnext_number() -> None:
    spec = parse_line(_line(status="no_equivalent", reason="GodotNext-only mode.", ue=None))
    row = evaluate_line(spec, [2.0] * 5, None, judge=True)
    assert row["ue"] == {"selection": None, "runs": [], "missing_frames": [], "median": None}
    assert row["godotnext"]["median"] == 2.0  # type: ignore[index]
    assert row["ratio"] is None
    assert row["verdict"] is None


def test_linux_lines_are_report_only() -> None:
    spec = parse_line(_line())
    row = evaluate_line(spec, [1.0] * 5, [1.0] * 5, judge=True, platform="linux")
    assert row["status"] == "report_only"
    assert row["reason"] == LINUX_REASON
    assert row["verdict"] is None
    assert row["ratio"] == 1.0


def test_dry_run_computes_the_ratio_but_writes_no_verdict() -> None:
    spec = parse_line(_line())
    row = evaluate_line(spec, [1.0], [2.0], judge=False)
    assert row["ratio"] == 0.5
    assert row["verdict"] is None
    assert DRY_RUN_NOTE in row["notes"]  # type: ignore[operator]


def test_an_unnamed_ue_column_keeps_the_bar_but_cannot_be_judged() -> None:
    spec = parse_line(_line(ue={"group": "Feature", "columns": None}))
    row = evaluate_line(spec, [1.0], None, judge=False)
    assert row["status"] == "ue_bar"
    assert UNNAMED_UE_BAR_NOTE in row["notes"]  # type: ignore[operator]
    with pytest.raises(ComparisonError, match="UE column is not set"):
        evaluate_line(spec, [1.0] * 5, None, judge=True)
    undefined = parse_line(_line(godotnext=None))
    with pytest.raises(ComparisonError, match="GodotNext column group is not set"):
        evaluate_line(undefined, None, [1.0] * 5, judge=True)


def test_line_specs_are_validated() -> None:
    with pytest.raises(ComparisonError, match="status"):
        parse_line(_line(status="maybe"))
    with pytest.raises(ComparisonError, match="needs a reason"):
        parse_line(_line(status="report_only"))
    with pytest.raises(ComparisonError, match="needs its UE stat group"):
        parse_line(_line(ue=None))
    with pytest.raises(ComparisonError, match="no UE side"):
        parse_line(_line(status="no_equivalent", reason="x"))
    with pytest.raises(ComparisonError, match="p50, p95 or p99"):
        parse_line(_line(statistic="p90"))
    with pytest.raises(ComparisonError, match="lower or higher"):
        parse_line(_line(direction="up"))
    with pytest.raises(ComparisonError, match="not both"):
        parse_line(_line(godotnext={"all_passes": True, "columns": ["a_ms"]}))
    with pytest.raises(ComparisonError, match="not both"):
        parse_line(_line(godotnext={"prefix": "a_", "columns": ["a_ms"]}))
    with pytest.raises(ComparisonError, match="ending in _"):
        parse_line(_line(godotnext={"prefix": "a"}))
    with pytest.raises(ComparisonError, match="exclude goes with prefix"):
        parse_line(_line(godotnext={"columns": ["a_ms"], "exclude": ["b_ms"]}))
    with pytest.raises(ComparisonError, match="include goes with prefix"):
        parse_line(_line(godotnext={"columns": ["a_ms"], "include": ["b_ms"]}))
    for column in ("a_b_ms", "b_ms", ""):
        with pytest.raises(ComparisonError, match="include names a column outside the prefix"):
            parse_line(_line(godotnext={"prefix": "a_", "exclude": ["b_ms"], "include": [column]}))
    with pytest.raises(ComparisonError, match="non-empty list"):
        parse_line(_line(ue={"group": "Feature", "columns": []}))


def test_interleave_order_is_g_u_g_u_after_one_warm_up_each() -> None:
    assert interleave_order(2, True) == [
        ("warmup", "godotnext", 0),
        ("warmup", "ue", 0),
        ("timing", "godotnext", 1),
        ("timing", "ue", 1),
        ("timing", "godotnext", 2),
        ("timing", "ue", 2),
    ]
    assert len(interleave_order(5, False)) == 10
    with pytest.raises(ComparisonError):
        interleave_order(0, True)


def test_a_valid_judged_set_reports_its_shared_facts() -> None:
    pinned = "++Synthetic+Release-0.0-CL-1"
    facts = check_set(_records(), judge=True, ue_run_mode="editor_game", ue_build_version=pinned)
    assert facts["runs_per_engine"] == 5
    assert facts["resolution"] == [1920, 1080]
    assert facts["clocks_locked"] is True
    assert facts["ue_build_version"] == pinned
    assert facts["ue_run_mode"] == "editor_game"
    assert facts["godotnext_release_export"] is True
    assert facts["notes"] == []


def test_a_judged_set_needs_timing_jobs_warm_up_launches_a_pinned_build_and_a_release_export() -> None:
    pinned = "++Synthetic+Release-0.0-CL-1"
    with pytest.raises(ComparisonError, match="timing jobs"):
        check_set(_records(), judge=True, ue_run_mode="editor_game", ue_build_version=pinned, job_kind="run")
    with pytest.raises(ComparisonError, match="warm-up launches"):
        check_set(_records(warmup=False), judge=True, ue_run_mode="editor_game", ue_build_version=pinned)
    with pytest.raises(ComparisonError, match="pins the UE build"):
        check_set(_records(), judge=True, ue_run_mode="editor_game", ue_build_version=None)
    editor = [
        replace(record, godotnext_editor_binary=True) if record.engine == "godotnext" else record
        for record in _records()
    ]
    with pytest.raises(ComparisonError, match="editor binary"):
        check_set(editor, judge=True, ue_run_mode="editor_game", ue_build_version=pinned)
    # A dry run notes what a judged run would refuse.
    facts = check_set(editor, judge=False, ue_run_mode="editor_game", ue_build_version=None, job_kind="run")
    notes = facts["notes"]
    assert isinstance(notes, list)
    assert any("editor binary" in note for note in notes)
    assert any("not pinned" in note for note in notes)
    blank = [replace(record, godotnext_commit="") if record.engine == "godotnext" else record for record in _records()]
    with pytest.raises(ComparisonError, match="one recorded commit"):
        check_set(blank, judge=False, ue_run_mode="editor_game", ue_build_version=None)


def test_a_judged_set_needs_five_runs_per_engine_but_a_dry_run_does_not() -> None:
    with pytest.raises(ComparisonError, match="5 runs per engine, not 1"):
        check_set(_records(runs=1), judge=True, ue_run_mode="editor_game", ue_build_version="x")
    facts = check_set(_records(runs=1, warmup=False), judge=False, ue_run_mode="editor_game", ue_build_version=None)
    assert facts["runs_per_engine"] == 1


def test_a_ue_run_in_another_mode_rejects_the_set() -> None:
    records = _records(runs=1, ue_run_mode="unknown (SyntheticGame.exe)")
    with pytest.raises(ComparisonError, match="run mode"):
        check_set(records, judge=False, ue_run_mode="editor_game", ue_build_version=None)


def test_one_failed_run_rejects_the_set() -> None:
    records = _records()
    records[4] = replace(records[4], exit_code=3)
    with pytest.raises(ComparisonError, match="runs failed: godotnext timing 2"):
        check_set(records, judge=True, ue_run_mode="editor_game", ue_build_version=None)


def test_runs_out_of_order_reject_the_set() -> None:
    records = _records()
    records[2], records[3] = records[3], records[2]
    with pytest.raises(ComparisonError, match="not interleaved"):
        check_set(records, judge=True, ue_run_mode="editor_game", ue_build_version=None)


def test_mismatched_settings_reject_the_set() -> None:
    def refused(match: str, records: list[RunRecord], **kwargs: object) -> None:
        options: dict[str, object] = {"ue_run_mode": "editor_game", "ue_build_version": "++Synthetic+Release-0.0-CL-1"}
        options.update(kwargs)
        with pytest.raises(ComparisonError, match=match):
            check_set(records, judge=True, **options)  # type: ignore[arg-type]

    refused("differ in resolution", _records(resolution=(2560, 1440)))
    refused("clocks are locked for some runs", _records(clocks_locked=False))
    refused("do not share one recorded build version", _records(ue_build_version=""))
    refused(r"pins \+\+Other", _records(), ue_build_version="++Other")
    refused("VSync is off", _records(ue_vsync="1"))
    records = _records()
    records[2] = replace(records[2], godotnext_commit="def456")
    refused("one recorded commit", records)
    records = _records()
    records[3] = replace(records[3], resolution=None)
    refused("does not record its resolution", records)


def test_a_prefix_selection_takes_the_block_columns_without_the_excluded_ones() -> None:
    selection = GodotNextSelection(prefix="nextgi_", exclude=("nextgi_2_ms", "nextgi_as_builds_ms"))
    header = ["frame", "wall_ms", "opaque_ms", "nextgi_ms", "nextgi_as_builds_ms", "nextgi_sdf_trace_ms", "nextgi_2_ms"]
    assert selection.select(header) == ["nextgi_ms", "nextgi_sdf_trace_ms"]
    assert GodotNextSelection(all_passes=True).select(header) == header[2:]
    assert GodotNextSelection(columns=("opaque_ms",)).select(header) == ["opaque_ms"]
    spec = parse_line(_line(godotnext={"prefix": "nextgi_", "exclude": ["nextgi_2_ms"]}))
    assert spec.godotnext is not None
    assert spec.godotnext.to_json() == {"prefix": "nextgi_", "exclude": ["nextgi_2_ms"]}


def test_a_prefix_selection_adds_the_included_columns_a_run_has() -> None:
    # Line 1a's "Skinned refit" timestamp sits inside the NextGI block without the prefix: its column
    # joins the block on the runs (and frames) that emit it, and a run without it sums the block alone.
    data = {"prefix": "nextgi_", "exclude": ["nextgi_2_ms"], "include": ["skinned_refit_ms"]}
    spec = parse_line(_line(godotnext=data))
    assert spec.godotnext is not None and spec.godotnext.to_json() == data
    with_refit = ["frame", "wall_ms", "nextgi_as_updates_ms", "skinned_refit_ms", "nextgi_as_builds_ms", "nextgi_2_ms"]
    assert spec.godotnext.select(with_refit) == ["nextgi_as_updates_ms", "skinned_refit_ms", "nextgi_as_builds_ms"]
    without = [name for name in with_refit if name != "skinned_refit_ms"]
    assert spec.godotnext.select(without) == ["nextgi_as_updates_ms", "nextgi_as_builds_ms"]
    assert "skinned_refit_ms" not in GodotNextSelection(prefix="nextgi_").select(with_refit)
