"""Q17 ray-query records.

The on-disk record is 32 bytes, little-endian, shared with issue #18::

    float t; uint custom_index; uint primitive; float u; float v;
    uint front_face; uint committed; uint candidate_count;

Analytic comparison uses relative t <= 1e-5 and absolute barycentrics <= 1e-5.
Across APIs both tolerances are 1e-4. An edge record fails unless
``candidate_count`` is 1 and the primitive is one of the adjacent triangles.

The RD smoke test (#18, #49) writes one dump folder per device: ``meta.json``
and, for each fixture in ``FIXTURES``, ``<name>.bin`` with the records and
``<name>_analytic.json`` with the analytic expectations. Older capture folders
hold only ``rays.bin`` and ``analytic.json``. ``q17_dump`` compares one folder
against its analytic files, or against another device's folder (G2-1):

- ``tri``, ``rays``, ``winding``: IDs, primitive, committed, front face and
  candidate count exact; t and barycentrics within the mode's tolerance.
- ``edges``: each side is judged on its own by the edge rule. The two APIs may
  legally report different adjacent triangles, so they are never compared.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.jsonutil import as_float, as_int, as_list
from godotnext_metrics.result import MetricResult

RECORD_DTYPE = np.dtype([
    ("t", "<f4"),
    ("custom_index", "<u4"),
    ("primitive", "<u4"),
    ("u", "<f4"),
    ("v", "<f4"),
    ("front_face", "<u4"),
    ("committed", "<u4"),
    ("candidate_count", "<u4"),
])
assert RECORD_DTYPE.itemsize == 32

ANALYTIC_REL_T = 1e-5
ANALYTIC_ABS_BARY = 1e-5
API_TOLERANCE = 1e-4
FIXTURES = ("tri", "rays", "edges", "winding")
EDGE_FIXTURE = "edges"
EXACT_FIELDS = ("custom_index", "primitive", "committed", "front_face", "candidate_count")


def read_records(path: Path) -> NDArray[np.void]:
    data = np.fromfile(path, dtype=RECORD_DTYPE)
    if data.size == 0 and path.stat().st_size != 0:
        raise ValueError(f"{path} is not a multiple of 32 bytes")
    return data


def write_records(path: Path, records: NDArray[np.void]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    np.asarray(records, dtype=RECORD_DTYPE).tofile(path)


def records_from_dicts(rows: list[dict[str, object]]) -> NDArray[np.void]:
    data = np.zeros(len(rows), dtype=RECORD_DTYPE)
    for index, row in enumerate(rows):
        for name in RECORD_DTYPE.names or ():
            if name in row:
                data[index][name] = row[name]
    return data


def q17_ray_query_parity(
    got: NDArray[np.void],
    expected: list[dict[str, object]],
    *,
    rel_t: float = ANALYTIC_REL_T,
    abs_bary: float = ANALYTIC_ABS_BARY,
) -> MetricResult:
    if len(got) != len(expected):
        raise ValueError(f"Q17 record count {len(got)} != expected {len(expected)}")
    failures: list[str] = []
    max_rel_t = 0.0
    max_abs_bary = 0.0
    for index, (record, analytic) in enumerate(zip(got, expected, strict=True)):
        rel, bary, reasons = _compare_one(record, analytic, rel_t=rel_t, abs_bary=abs_bary)
        max_rel_t = max(max_rel_t, rel)
        max_abs_bary = max(max_abs_bary, bary)
        failures.extend(f"record {index}: {reason}" for reason in reasons)
    passed = not failures
    return MetricResult(
        metric="Q17",
        value={"max_rel_t": max_rel_t, "max_abs_bary": max_abs_bary, "failures": len(failures)},
        threshold={"rel_t": rel_t, "abs_bary": abs_bary},
        passed=passed,
        values={"max_rel_t": max_rel_t, "max_abs_bary": max_abs_bary, "records": len(got)},
        details={"failures": failures[:8]},
    )


def load_analytic(path: Path) -> list[dict[str, object]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    rows = payload["records"] if isinstance(payload, dict) else payload
    if not isinstance(rows, list):
        raise ValueError("analytic Q17 file must be a list or an object with records")
    return [dict(row) for row in rows]


def _compare_one(
    record: np.void,
    analytic: dict[str, object],
    *,
    rel_t: float,
    abs_bary: float,
) -> tuple[float, float, list[str]]:
    reasons: list[str] = []
    if bool(analytic.get("edge", False)):
        # The edge rule only: which adjacent triangle wins is not specified, so
        # t, barycentrics and front face are not compared.
        return 0.0, 0.0, _edge_reasons(record, analytic)
    for name in ("custom_index", "front_face", "committed"):
        expected_id = as_int(analytic[name])
        if int(record[name]) != expected_id:
            reasons.append(f"{name} {int(record[name])} != {expected_id}")
    expected_primitive = as_int(analytic["primitive"])
    if int(record["primitive"]) != expected_primitive:
        reasons.append(f"primitive {int(record['primitive'])} != {expected_primitive}")
    if "candidate_count" in analytic and int(record["candidate_count"]) != as_int(analytic["candidate_count"]):
        reasons.append("candidate_count mismatch")
    rel = _relative_t(float(record["t"]), as_float(analytic["t"]))
    if rel > rel_t:
        reasons.append(f"relative t {rel} > {rel_t}")
    bary = max(abs(float(record["u"]) - as_float(analytic["u"])), abs(float(record["v"]) - as_float(analytic["v"])))
    if not np.isfinite(bary) or bary > abs_bary:
        reasons.append(f"barycentric error {bary} > {abs_bary}")
    return rel, bary, reasons


def _edge_reasons(record: np.void, analytic: dict[str, object]) -> list[str]:
    reasons: list[str] = []
    if int(record["committed"]) != 1:
        reasons.append("miss: committed 0")
    if int(record["candidate_count"]) != 1:
        reasons.append(f"candidate_count {int(record['candidate_count'])} != 1")
    expected_id = as_int(analytic["custom_index"])
    if int(record["custom_index"]) != expected_id:
        reasons.append(f"custom_index {int(record['custom_index'])} != {expected_id}")
    allowed = [as_int(value) for value in as_list(analytic["adjacent_primitives"])]
    if int(record["primitive"]) not in allowed:
        reasons.append(f"primitive {int(record['primitive'])} is not adjacent")
    return reasons


def _relative_t(got: float, expected: float) -> float:
    if not (np.isfinite(got) and np.isfinite(expected)):
        return float("inf")
    return abs(got) if expected == 0.0 else abs(got - expected) / abs(expected)


def _compare_across(record: np.void, other: np.void, *, tolerance: float) -> tuple[float, float, list[str]]:
    reasons = [
        f"{name} {int(record[name])} != {int(other[name])}"
        for name in EXACT_FIELDS
        if int(record[name]) != int(other[name])
    ]
    rel = _relative_t(float(record["t"]), float(other["t"]))
    if rel > tolerance:
        reasons.append(f"relative t {rel} > {tolerance}")
    bary = max(abs(float(record["u"]) - float(other["u"])), abs(float(record["v"]) - float(other["v"])))
    if not np.isfinite(bary) or bary > tolerance:
        reasons.append(f"barycentric error {bary} > {tolerance}")
    return rel, bary, reasons


class DumpInputError(ValueError):
    """A dump folder is missing a file or does not match the other folder."""


def dump_fixtures(folder: Path) -> list[str]:
    """Fixtures present in a dump folder, in ``FIXTURES`` order."""
    return [name for name in FIXTURES if (folder / f"{name}.bin").is_file()]


def _analytic_path(folder: Path, fixture: str) -> Path | None:
    path = folder / f"{fixture}_analytic.json"
    if path.is_file():
        return path
    legacy = folder / "analytic.json"
    if fixture == "rays" and legacy.is_file():
        return legacy
    return None


def _read_meta(folder: Path) -> dict[str, object]:
    path = folder / "meta.json"
    if not path.is_file():
        return {}
    payload = json.loads(path.read_text(encoding="utf-8"))
    return dict(payload) if isinstance(payload, dict) else {}


def q17_dump(in_dir: Path, ref_dir: Path | None = None) -> MetricResult:
    """Q17 over a dump folder: analytic mode, or across APIs when ``ref_dir`` is given."""
    across = ref_dir is not None
    fixtures = dump_fixtures(in_dir)
    if "rays" not in fixtures:
        raise DumpInputError(f"{in_dir} has no rays.bin")
    if ref_dir is not None:
        other_fixtures = dump_fixtures(ref_dir)
        if fixtures != other_fixtures:
            raise DumpInputError(f"fixture sets differ: {fixtures} vs {other_fixtures}")
    tolerance_t = API_TOLERANCE if across else ANALYTIC_REL_T
    tolerance_bary = API_TOLERANCE if across else ANALYTIC_ABS_BARY
    failures: list[str] = []
    per_fixture: dict[str, object] = {}
    max_rel_t = 0.0
    max_abs_bary = 0.0
    for fixture in fixtures:
        got = read_records(in_dir / f"{fixture}.bin")
        fixture_failures: list[str] = []
        fixture_rel = 0.0
        fixture_bary = 0.0
        if fixture == EDGE_FIXTURE:
            sides = [("in", in_dir, got)]
            if ref_dir is not None:
                sides.append(("ref", ref_dir, read_records(ref_dir / f"{fixture}.bin")))
            for side, folder, records in sides:
                path = _analytic_path(folder, fixture)
                if path is None and ref_dir is not None:
                    path = _analytic_path(ref_dir if folder == in_dir else in_dir, fixture)
                if path is None:
                    raise DumpInputError(f"{fixture}_analytic.json is missing")
                analytic = load_analytic(path)
                if len(records) != len(analytic):
                    raise DumpInputError(f"{fixture} ({side}) has {len(records)} records, analytic has {len(analytic)}")
                for index, (record, row) in enumerate(zip(records, analytic, strict=True)):
                    fixture_failures.extend(
                        f"{fixture} {side} record {index}: {reason}" for reason in _edge_reasons(record, row)
                    )
        elif ref_dir is not None:
            other = read_records(ref_dir / f"{fixture}.bin")
            if len(got) != len(other):
                raise DumpInputError(f"{fixture} record counts differ: {len(got)} vs {len(other)}")
            for index, (record, row) in enumerate(zip(got, other, strict=True)):
                rel, bary, reasons = _compare_across(record, row, tolerance=API_TOLERANCE)
                fixture_rel = max(fixture_rel, rel)
                fixture_bary = max(fixture_bary, bary)
                fixture_failures.extend(f"{fixture} record {index}: {reason}" for reason in reasons)
        else:
            path = _analytic_path(in_dir, fixture)
            if path is None:
                raise DumpInputError(f"{fixture}_analytic.json is missing")
            analytic = load_analytic(path)
            if len(got) != len(analytic):
                raise DumpInputError(f"{fixture} has {len(got)} records, analytic has {len(analytic)}")
            for index, (record, row) in enumerate(zip(got, analytic, strict=True)):
                rel, bary, reasons = _compare_one(record, row, rel_t=tolerance_t, abs_bary=tolerance_bary)
                fixture_rel = max(fixture_rel, rel)
                fixture_bary = max(fixture_bary, bary)
                fixture_failures.extend(f"{fixture} record {index}: {reason}" for reason in reasons)
        max_rel_t = max(max_rel_t, fixture_rel)
        max_abs_bary = max(max_abs_bary, fixture_bary)
        per_fixture[fixture] = {
            "records": len(got),
            "failures": len(fixture_failures),
            "max_rel_t": fixture_rel,
            "max_abs_bary": fixture_bary,
        }
        failures.extend(fixture_failures)
    values: dict[str, object] = {
        "mode": "across_api" if across else "analytic",
        "fixtures": per_fixture,
        "max_rel_t": max_rel_t,
        "max_abs_bary": max_abs_bary,
    }
    meta = _read_meta(in_dir)
    if meta:
        values["in_device"] = {"driver": meta.get("driver"), "adapter": meta.get("adapter")}
    if ref_dir is not None:
        ref_meta = _read_meta(ref_dir)
        if ref_meta:
            values["ref_device"] = {"driver": ref_meta.get("driver"), "adapter": ref_meta.get("adapter")}
    return MetricResult(
        metric="Q17",
        value={"failures": len(failures), "max_rel_t": max_rel_t, "max_abs_bary": max_abs_bary},
        threshold={"rel_t": tolerance_t, "abs_bary": tolerance_bary},
        passed=not failures,
        values=values,
        details={"failures": failures[:8]},
    )
