"""Q17 ray-query records.

The on-disk record is 32 bytes, little-endian, shared with issue #18::

    float t; uint custom_index; uint primitive; float u; float v;
    uint front_face; uint committed; uint candidate_count;

Analytic comparison uses relative t <= 1e-5 and absolute barycentrics <= 1e-5.
Across APIs both tolerances are 1e-4. An edge record fails unless
``candidate_count`` is 1 and the primitive is one of the adjacent triangles.
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
    edge = bool(analytic.get("edge", False))
    for name in ("custom_index", "front_face", "committed"):
        expected_id = as_int(analytic[name])
        if int(record[name]) != expected_id:
            reasons.append(f"{name} {int(record[name])} != {expected_id}")
    if edge:
        allowed = [as_int(value) for value in as_list(analytic["adjacent_primitives"])]
        if int(record["primitive"]) not in allowed:
            reasons.append(f"primitive {int(record['primitive'])} is not adjacent")
        if int(record["candidate_count"]) != 1:
            reasons.append(f"candidate_count {int(record['candidate_count'])} != 1")
    else:
        expected_primitive = as_int(analytic["primitive"])
        if int(record["primitive"]) != expected_primitive:
            reasons.append(f"primitive {int(record['primitive'])} != {expected_primitive}")
        if "candidate_count" in analytic and int(record["candidate_count"]) != as_int(analytic["candidate_count"]):
            reasons.append("candidate_count mismatch")
    expected_t = as_float(analytic["t"])
    rel = 0.0 if expected_t == 0.0 else abs(float(record["t"]) - expected_t) / abs(expected_t)
    if expected_t == 0.0:
        rel = abs(float(record["t"]))
    if rel > rel_t:
        reasons.append(f"relative t {rel} > {rel_t}")
    bary = max(abs(float(record["u"]) - as_float(analytic["u"])), abs(float(record["v"]) - as_float(analytic["v"])))
    if bary > abs_bary:
        reasons.append(f"barycentric error {bary} > {abs_bary}")
    return rel, bary, reasons
