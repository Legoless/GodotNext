"""§2.1 lines added to ``summary.json`` by issue #33.

Keep this in sync with ``timing_fields`` in ``godotnext/testbed/harness/manifest.gd``.
Schema stays 1. These fields are extra keys. Q11 still reads ``columns`` and
still drops line 1b by column name. This mapper instead tags whole frames.

A frame is a line-1b frame when its counters report a BLAS build or a
compaction copy, or when the summed line-1b timestamps are above zero.
Steady-state percentiles use the other frames. Line 1b itself is summarized
on the tagged frames. The p99 of those frames is the full GPU pass sum
(not ``wall_ms``), compared with High total + line 1b when the device is
known. The RTX 5090 machine has no millisecond budget here.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from godotnext_metrics.percentile import percentile

LineSpec = tuple[str, str, tuple[str, ...]]

# Timestamp columns after ``sanitize_pass_name`` (lowercase, non-alnum to "_").
LINES: tuple[LineSpec, ...] = (
    ("1a", "AS updates", ("nextgi_as_updates_ms", "as_updates_ms", "tlas_rebuild_ms", "skinned_refit_ms")),
    (
        "1b",
        "AS builds",
        (
            "nextgi_as_builds_ms",
            "as_builds_ms",
            "blas_build_ms",
            "compaction_copies_ms",
            "compaction_copy_ms",
            "line_1b_ms",
            "as_updates_amortized_ms",
            "1b_ms",
        ),
    ),
    ("2a", "SRC capture", ("nextgi_src_capture_ms", "src_capture_ms")),
    ("2b", "SRC relight", ("nextgi_src_relight_ms", "src_relight_ms")),
    # nextgi_wrc_update_ms and nextgi_integration_ms are the NextGI frame-block slot names (#26).
    ("3", "WRC update", ("nextgi_wrc_ms", "nextgi_wrc_update_ms", "wrc_update_ms", "translucency_volume_ms")),
    ("4", "Screen traces", ("nextgi_screen_traces_ms", "screen_traces_ms")),
    ("5", "HWRT diffuse gather", ("nextgi_diffuse_gather_ms", "hwrt_diffuse_gather_ms")),
    ("7", "Gather integration", ("nextgi_gather_integration_ms", "nextgi_integration_ms", "gather_integration_ms")),
)

DIFFUSE_KEYS: tuple[str, ...] = ("1a", "2a", "2b", "3", "4", "5", "7")

COUNTER_FIELDS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("relight_texels", ("relight_texels", "src_relight_texels")),
    ("shadow_rays", ("shadow_rays", "shadow_rays_traced")),
    ("q18", ("q18", "rt_coverage")),
    ("tlas_fill_ms", ("tlas_fill_ms",)),
    ("capture_cpu_ms", ("capture_cpu_ms",)),
)

_BLAS_KEYS = ("blas_builds", "blas_build_count")
_COPY_KEYS = ("compaction_copies", "compaction_copy_count")

_HIGH_TOTAL_M5_MS = 4.0
_LINE_1B_M5_MS = 0.25
_HIGH_TOTAL_ADA_MS = 1.6
_LINE_1B_ADA_MS = 0.1


def _column_values(row: Mapping[str, Any]) -> dict[str, float]:
    """Last sample wins per column, matching ``render_csv``."""
    found: dict[str, float] = {}
    passes = row.get("passes", [])
    if not isinstance(passes, list):
        return found
    for sample in passes:
        if not isinstance(sample, Mapping):
            continue
        column = sample.get("column")
        if not isinstance(column, str):
            continue
        ms = sample.get("ms", 0.0)
        if isinstance(ms, bool) or not isinstance(ms, (int, float)):
            continue
        found[column] = float(ms)
    return found


def _line_sums(columns: Mapping[str, float]) -> dict[str, float]:
    sums: dict[str, float] = {}
    for key, _name, names in LINES:
        total = 0.0
        for name in names:
            if name in columns:
                total += float(columns[name])
        sums[key] = total
    return sums


def _as_float(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


def _counter_value(counters: Mapping[str, Any], keys: tuple[str, ...]) -> float:
    for key in keys:
        if key not in counters:
            continue
        parsed = _as_float(counters[key])
        if parsed is not None:
            return parsed
    return 0.0


def _row_counters(row: Mapping[str, Any]) -> dict[str, Any]:
    counters = row.get("counters", {})
    if isinstance(counters, Mapping):
        return dict(counters)
    return {}


def _is_line_1b(columns: Mapping[str, float], counters: Mapping[str, Any]) -> bool:
    if _counter_value(counters, _BLAS_KEYS) > 0.0:
        return True
    if _counter_value(counters, _COPY_KEYS) > 0.0:
        return True
    return _line_sums(columns)["1b"] > 0.0


def _stats(values: Sequence[float]) -> dict[str, float | int]:
    return {
        "p50": float(percentile(list(values), 50.0)),
        "p95": float(percentile(list(values), 95.0)),
        "p99": float(percentile(list(values), 99.0)),
        "n": len(values),
    }


def _device_budget(host: str, adapter: str) -> dict[str, Any]:
    text = f"{host} {adapter}"
    # 5090 is checked first so a label that also mentions 4080 stays unbudgeted.
    if "5090" in text:
        device = "RTX 5090"
        high: float | None = None
        budget: float | None = None
        limit: float | None = None
    elif "4080" in text:
        device = "RTX 4080 SUPER"
        high = _HIGH_TOTAL_ADA_MS
        budget = _LINE_1B_ADA_MS
        limit = 1.7
    elif "M5 Max" in text:
        device = "Apple M5 Max"
        high = _HIGH_TOTAL_M5_MS
        budget = _LINE_1B_M5_MS
        limit = 4.25
    else:
        device = ""
        high = None
        budget = None
        limit = None
    return {
        "device": device,
        "high_total_ms": high,
        "line_1b_budget_ms": budget,
        "limit_ms": limit,
    }


def _counters_block(
    rows: Sequence[Mapping[str, Any]],
    counters_present: bool,
) -> dict[str, Any]:
    present = counters_present
    if not present:
        for row in rows:
            counters = row.get("counters")
            if isinstance(counters, Mapping) and len(counters) > 0:
                present = True
                break
    block: dict[str, Any] = {"source": "nextgi_get_counters" if present else "absent"}
    for field, aliases in COUNTER_FIELDS:
        if not present:
            block[field] = None
            continue
        samples: list[float] = []
        for row in rows:
            samples.append(_counter_value(_row_counters(row), aliases))
        block[field] = _stats(samples)
    return block


def timing_fields(
    rows: Sequence[Mapping[str, Any]],
    host: str = "",
    adapter: str = "",
    counters_present: bool = False,
) -> dict[str, Any]:
    """Extra ``summary.json`` object. Existing keys are left to the caller."""
    parsed: list[tuple[dict[str, float], dict[str, float], bool]] = []
    steady_index: list[int] = []
    line_1b_index: list[int] = []
    for index, row in enumerate(rows):
        columns = _column_values(row)
        sums = _line_sums(columns)
        tagged = _is_line_1b(columns, _row_counters(row))
        parsed.append((columns, sums, tagged))
        if tagged:
            line_1b_index.append(index)
        else:
            steady_index.append(index)

    lines: dict[str, Any] = {}
    for key, name, _names in LINES:
        if key == "1b":
            samples = [parsed[index][1][key] for index in line_1b_index]
        else:
            samples = [parsed[index][1][key] for index in steady_index]
        stats = _stats(samples)
        lines[key] = {"name": name, **stats}

    diffuse = [sum(parsed[index][1][key] for key in DIFFUSE_KEYS) for index in steady_index]
    diffuse_stats = _stats(diffuse)
    gpu_1b = [sum(parsed[index][0].values()) for index in line_1b_index]
    gpu_stats = _stats(gpu_1b)
    budget = _device_budget(host, adapter)
    p99 = float(gpu_stats["p99"])
    limit = budget["limit_ms"]
    within: bool | None
    if limit is None:
        within = None
    else:
        within = p99 <= float(limit) + 1e-9
    return {
        "lines": lines,
        "diffuse_path": {
            "name": "diffuse path",
            "columns": list(DIFFUSE_KEYS),
            "p50": diffuse_stats["p50"],
            "p95": diffuse_stats["p95"],
            "p99": diffuse_stats["p99"],
            "n": diffuse_stats["n"],
        },
        "line_1b_frames": {
            "count": len(line_1b_index),
            "frames": line_1b_index,
            "p99": p99,
            "high_total_ms": budget["high_total_ms"],
            "line_1b_budget_ms": budget["line_1b_budget_ms"],
            "limit_ms": limit,
            "device": budget["device"],
            "within_limit": within,
        },
        "counters": _counters_block(rows, counters_present),
    }
