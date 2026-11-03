"""Q15 geometry-memory report for the three Dense City exports (#74).

The frozen ``q15`` command still reads ``MEMORY_ACCELERATION_STRUCTURES``.
This report is the ten-category breakdown from plan §13.4. Byte counts stay
null while ``RenderingServer.virtual_geometry_get_memory_report`` is absent
(#78 / #79). ``retain_authored`` is listed and is not part of the ratio.
"""

from __future__ import annotations

import math
from typing import cast

from godotnext_metrics.counters import Q15_BASELINE_RATIO
from godotnext_metrics.jsonutil import as_dict

Q15_CATEGORIES: tuple[str, ...] = (
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
RT_CATEGORIES: tuple[str, ...] = ("rt_arena", "blas", "tlas", "as_scratch")
EXPORT_STRIP = "strip_authored_to_coarse"
EXPORT_BASELINE = "discrete_lod"
EXPORT_RETAIN = "retain_authored"
EXPORTS: tuple[str, ...] = (EXPORT_STRIP, EXPORT_BASELINE, EXPORT_RETAIN)


def ratio(numerator: float, denominator: float) -> float:
    """0/0 is 0. A positive numerator over 0 is non-finite."""
    if denominator == 0.0:
        return 0.0 if numerator == 0.0 else math.inf
    return float(numerator) / float(denominator)


def memory_report(document: object | None) -> dict[str, object]:
    """Build the Q15 report. A missing document is ``source: absent``."""
    body = as_dict(document) if isinstance(document, dict) else {}
    source = body.get("source", "absent")
    if not isinstance(source, str):
        source = "absent"
    warmup = body.get("warmup_frames", 120)
    exports_in = body.get("exports", {})
    if not isinstance(exports_in, dict):
        exports_in = {}
    exports: dict[str, object] = {}
    for name in EXPORTS:
        exports[name] = _export_view(exports_in.get(name))
    strip = cast(dict[str, object], exports[EXPORT_STRIP])
    baseline = cast(dict[str, object], exports[EXPORT_BASELINE])
    retain = cast(dict[str, object], exports[EXPORT_RETAIN])
    compared, passed = _pair(strip, baseline)
    return {
        "schema": 1,
        "metric": "Q15",
        "report": "geometry_memory",
        "source": source,
        "warmup_frames": warmup if isinstance(warmup, int) and not isinstance(warmup, bool) else None,
        "categories": list(Q15_CATEGORIES),
        "exports": exports,
        "ratio": compared,
        "rt_share": strip["rt_share"],
        "retain_authored": retain["total"],
        "retain_authored_gated": False,
        "pass": passed,
        "filled_by": "virtual_geometry_get_memory_report",
    }


def _export_view(row: object) -> dict[str, object]:
    body = as_dict(row) if isinstance(row, dict) else {}
    source = body.get("source", "absent")
    if not isinstance(source, str) or source == "":
        source = "absent"
    raw = body.get("categories", {})
    if not isinstance(raw, dict):
        raw = {}
    categories: dict[str, float | None] = {}
    complete = source != "absent"
    for name in Q15_CATEGORIES:
        if name not in raw or raw[name] is None:
            categories[name] = None
            complete = False
            continue
        number = _number(raw[name])
        categories[name] = number
        if number is None:
            complete = False
    total: float | None = None
    share: float | None = None
    if complete:
        values = [categories[name] for name in Q15_CATEGORIES]
        total = float(sum(value or 0.0 for value in values))
        rt_values = [categories[name] for name in RT_CATEGORIES]
        share = ratio(float(sum(value or 0.0 for value in rt_values)), total)
    return {
        "source": source,
        "categories": categories,
        "total": total,
        "rt_share": share,
        "complete": complete,
    }


def _pair(strip: dict[str, object], baseline: dict[str, object]) -> tuple[float | None, bool | None]:
    strip_complete = bool(strip["complete"])
    baseline_complete = bool(baseline["complete"])
    strip_present = strip["source"] != "absent"
    baseline_present = baseline["source"] != "absent"
    if strip_present and not strip_complete:
        return None, False
    if baseline_present and not baseline_complete:
        return None, False
    if not strip_complete or not baseline_complete:
        return None, None
    strip_total = cast(float, strip["total"])
    baseline_total = cast(float, baseline["total"])
    compared = ratio(strip_total, baseline_total)
    passed = bool(math.isfinite(compared) and compared <= Q15_BASELINE_RATIO)
    return compared, passed


def _number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)
