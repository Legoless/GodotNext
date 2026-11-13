"""Q17 over RD smoke-test dump folders (#49): analytic mode and across APIs (G2-1)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, cast

import numpy as np
import pytest
from godotnext_metrics.commands import run_q17
from godotnext_metrics.rays import (
    API_TOLERANCE,
    DumpInputError,
    q17_dump,
    records_from_dicts,
    write_records,
)
from godotnext_metrics.result import MetricInputError, MetricResult


def _values(result: MetricResult) -> dict[str, Any]:
    return cast(dict[str, Any], result.values)


def _failures(result: MetricResult) -> list[str]:
    return cast(list[str], result.details["failures"])


def _hit(**overrides: object) -> dict[str, object]:
    row: dict[str, object] = {
        "t": 2.0,
        "custom_index": 1000,
        "primitive": 1,
        "u": 0.25,
        "v": 0.5,
        "front_face": 1,
        "committed": 1,
        "candidate_count": 1,
    }
    row.update(overrides)
    return row


def _edge_analytic(adjacent: list[int], custom_index: int = 1001) -> dict[str, object]:
    return {
        "edge": True,
        "custom_index": custom_index,
        "adjacent_primitives": adjacent,
        "committed": 1,
        "candidate_count": 1,
    }


def _edge_record(primitive: int, **overrides: object) -> dict[str, object]:
    row: dict[str, object] = {
        "t": 1.5,
        "custom_index": 1001,
        "primitive": primitive,
        "u": 0.0,
        "v": 0.4,
        "front_face": 0,
        "committed": 1,
        "candidate_count": 1,
    }
    row.update(overrides)
    return row


def _fixture_rows() -> dict[str, tuple[list[dict[str, object]], list[dict[str, object]]]]:
    """Records and analytic rows per fixture; the records match the analytic values."""
    tri = [_hit(t=5.0, custom_index=7, primitive=0), _hit(t=5.0, custom_index=7, primitive=0, front_face=0)]
    rays = [_hit(), _hit(t=3.5, custom_index=1002, primitive=4, u=0.1, v=0.2)]
    winding = [_hit(custom_index=2000, primitive=0), _hit(custom_index=2001, primitive=0, front_face=0)]
    edge_records = [_edge_record(2), _edge_record(5)]
    edge_analytic = [_edge_analytic([2, 3]), _edge_analytic([4, 5])]
    return {
        "tri": (tri, tri),
        "rays": (rays, rays),
        "edges": (edge_records, edge_analytic),
        "winding": (winding, winding),
    }


def _write_dump(
    folder: Path,
    *,
    overrides: dict[str, dict[int, dict[str, object]]] | None = None,
    analytic: bool = True,
    skip: tuple[str, ...] = (),
    driver: str = "metal",
) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    for fixture, (records, expected) in _fixture_rows().items():
        if fixture in skip:
            continue
        rows = [dict(row) for row in records]
        for index, change in (overrides or {}).get(fixture, {}).items():
            rows[index].update(change)
        write_records(folder / f"{fixture}.bin", records_from_dicts(rows))
        if analytic:
            (folder / f"{fixture}_analytic.json").write_text(json.dumps({"records": expected}), encoding="utf-8")
        counts[fixture] = len(rows)
    meta = {"schema": 1, "test": "rd_smoke", "driver": driver, "adapter": f"{driver} device", "fixtures": counts}
    (folder / "meta.json").write_text(json.dumps(meta), encoding="utf-8")
    return folder


def test_analytic_mode_passes_on_all_four_fixtures(tmp_path: Path) -> None:
    result = q17_dump(_write_dump(tmp_path / "metal"))
    assert result.passed
    assert _values(result)["mode"] == "analytic"
    assert set(_values(result)["fixtures"]) == {"tri", "rays", "edges", "winding"}
    assert _values(result)["in_device"] == {"driver": "metal", "adapter": "metal device"}


def test_edge_rows_need_no_t_barycentrics_or_front_face(tmp_path: Path) -> None:
    # The edge analytic rows carry none of t, u, v or front_face; any value passes.
    folder = _write_dump(tmp_path / "metal", overrides={"edges": {0: {"t": 9.0, "u": 0.9, "front_face": 1}}})
    assert q17_dump(folder).passed


@pytest.mark.parametrize(
    ("change", "reason"),
    [
        ({"candidate_count": 2}, "candidate_count 2 != 1"),
        ({"committed": 0}, "miss: committed 0"),
        ({"primitive": 6}, "primitive 6 is not adjacent"),
        ({"custom_index": 1000}, "custom_index 1000 != 1001"),
    ],
)
def test_edge_rule_failures(tmp_path: Path, change: dict[str, object], reason: str) -> None:
    result = q17_dump(_write_dump(tmp_path / "metal", overrides={"edges": {1: change}}))
    assert not result.passed
    assert any(reason in failure for failure in _failures(result))


def test_tolerance_boundary_between_modes(tmp_path: Path) -> None:
    # t 2.0 * (1 + 5e-5) is inside the across-API tolerance and outside the analytic one.
    shifted = float(np.float32(2.0 * (1.0 + 5e-5)))
    assert 1e-5 < abs(shifted - 2.0) / 2.0 <= API_TOLERANCE
    metal = _write_dump(tmp_path / "metal")
    vulkan = _write_dump(tmp_path / "vulkan", overrides={"rays": {0: {"t": shifted}}}, driver="vulkan")
    assert q17_dump(vulkan, metal).passed
    assert not q17_dump(vulkan).passed


def test_across_api_needs_no_analytic_files_except_edges(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal", analytic=False)
    (metal / "edges_analytic.json").write_text(json.dumps({"records": _fixture_rows()["edges"][1]}), encoding="utf-8")
    vulkan = _write_dump(tmp_path / "vulkan", analytic=False, driver="vulkan")
    result = q17_dump(vulkan, metal)
    assert result.passed
    assert _values(result)["mode"] == "across_api"
    assert _values(result)["ref_device"]["driver"] == "metal"


def test_across_api_edges_may_pick_different_adjacent_triangles(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal")
    vulkan = _write_dump(tmp_path / "vulkan", overrides={"edges": {0: {"primitive": 3}, 1: {"primitive": 4}}})
    assert q17_dump(vulkan, metal).passed


def test_across_api_edges_judge_each_side(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal", overrides={"edges": {0: {"candidate_count": 2}}})
    vulkan = _write_dump(tmp_path / "vulkan")
    result = q17_dump(vulkan, metal)
    assert not result.passed
    assert any(failure.startswith("edges ref record 0") for failure in _failures(result))


def test_across_api_id_mismatch_fails(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal")
    vulkan = _write_dump(tmp_path / "vulkan", overrides={"rays": {1: {"primitive": 5}}})
    result = q17_dump(vulkan, metal)
    assert not result.passed
    assert _values(result)["fixtures"]["rays"]["failures"] == 1


def test_across_api_winding_front_face_mismatch_fails(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal")
    vulkan = _write_dump(tmp_path / "vulkan", overrides={"winding": {0: {"front_face": 0}}})
    result = q17_dump(vulkan, metal)
    assert not result.passed
    assert any("winding record 0: front_face 0 != 1" in failure for failure in _failures(result))


def test_nan_t_fails(tmp_path: Path) -> None:
    folder = _write_dump(tmp_path / "metal", overrides={"rays": {0: {"t": float("nan")}}})
    assert not q17_dump(folder).passed


def test_record_count_mismatch_is_an_input_error(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal")
    vulkan = _write_dump(tmp_path / "vulkan")
    write_records(vulkan / "rays.bin", records_from_dicts([_hit()]))
    with pytest.raises(DumpInputError, match="record counts differ"):
        q17_dump(vulkan, metal)


def test_fixture_set_mismatch_is_an_input_error(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal")
    vulkan = _write_dump(tmp_path / "vulkan", skip=("winding",))
    with pytest.raises(DumpInputError, match="fixture sets differ"):
        q17_dump(vulkan, metal)


def test_missing_edge_analytic_is_an_input_error(tmp_path: Path) -> None:
    metal = _write_dump(tmp_path / "metal", analytic=False)
    vulkan = _write_dump(tmp_path / "vulkan", analytic=False)
    with pytest.raises(DumpInputError, match="edges_analytic.json is missing"):
        q17_dump(vulkan, metal)


def test_analytic_mode_needs_each_fixtures_analytic(tmp_path: Path) -> None:
    folder = _write_dump(tmp_path / "metal")
    (folder / "winding_analytic.json").unlink()
    with pytest.raises(DumpInputError, match="winding_analytic.json is missing"):
        q17_dump(folder)


def test_rays_fixture_is_required(tmp_path: Path) -> None:
    folder = _write_dump(tmp_path / "metal", skip=("rays",))
    with pytest.raises(DumpInputError, match="has no rays.bin"):
        q17_dump(folder)


def test_legacy_folder_still_reads(tmp_path: Path) -> None:
    folder = tmp_path / "legacy"
    folder.mkdir()
    rows = _fixture_rows()["rays"][0]
    write_records(folder / "rays.bin", records_from_dicts(rows))
    (folder / "analytic.json").write_text(json.dumps({"records": rows}), encoding="utf-8")
    result = q17_dump(folder)
    assert result.passed
    assert list(_values(result)["fixtures"]) == ["rays"]


def test_command_turns_dump_errors_into_metric_input_errors(tmp_path: Path) -> None:
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(MetricInputError):
        run_q17(empty, None)
