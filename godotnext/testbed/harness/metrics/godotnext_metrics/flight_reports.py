"""Q14 flight, Slow IO Flight and Exclusion Matrix reports (#74).

Schema 1. Missing runtime counters stay null with ``source: absent``, the
same shape issue #33 used. #78 fills page-upload samples. #79 fills fallback
debug stats. The Slow IO timing gate is PQ and is not evaluated here.
"""

from __future__ import annotations

import math

from godotnext_metrics.counters import Q14_THRESHOLD, q14_fallback_coverage
from godotnext_metrics.jsonutil import as_dict, as_float, as_list
from godotnext_metrics.percentile import percentile

PAGE_UPLOAD_SOURCE = "virtual_geometry_get_routing_counters"
DEBUG_STATS_SOURCE = "virtual_geometry_get_debug_stats"


def q14_flight_report(document: object | None) -> dict[str, object]:
    """Mean and maximum fallback coverage over a flight."""
    if not isinstance(document, dict):
        return _q14_shell("absent", 0, None, None, None)
    body = as_dict(document)
    source = body.get("source", "absent")
    if not isinstance(source, str):
        source = "absent"
    frames_raw = body.get("frames", [])
    frames = as_list(frames_raw) if isinstance(frames_raw, list) else []
    if source == "absent" or not frames:
        return _q14_shell(source if source == "absent" or not frames else source, len(frames), None, None, None)
    ratios: list[float] = []
    non_finite = False
    for frame in frames:
        value = _frame_ratio(frame)
        if value is None:
            continue
        if not math.isfinite(value):
            non_finite = True
            continue
        ratios.append(value)
    if non_finite or not ratios:
        return _q14_shell(source, len(frames), None, None, False)
    mean = float(sum(ratios) / float(len(ratios)))
    maximum = float(max(ratios))
    passed = bool(mean < Q14_THRESHOLD and maximum < Q14_THRESHOLD)
    return _q14_shell(source, len(frames), mean, maximum, passed)


def slow_io_report(
    document: object | None,
    wall_ms: list[float] | None = None,
    page_upload_ms: list[float] | None = None,
) -> dict[str, object]:
    """Delay record plus frame-time and page-upload percentiles.

    ``pass`` stays null: the p99 gate is a PQ measurement. ``record_complete``
    is the structural check this issue can make on the current engine.
    """
    body = as_dict(document) if isinstance(document, dict) else {}
    delay_raw = body.get("debug_io_delay_ms", {})
    delay = as_dict(delay_raw) if isinstance(delay_raw, dict) else {}
    samples = _finite(wall_ms if wall_ms is not None else _optional_samples(body.get("wall_ms")))
    uploads = page_upload_ms
    if uploads is None:
        uploads = _optional_samples(body.get("page_upload_ms"))
    upload_samples = _finite(uploads)
    frame_time: dict[str, object] = {
        "p50": percentile(samples, 50.0) if samples else None,
        "p95": percentile(samples, 95.0) if samples else None,
        "p99": percentile(samples, 99.0) if samples else None,
        "n": len(samples),
    }
    page_p95 = percentile(upload_samples, 95.0) if upload_samples else None
    min_ms = _optional_number(delay.get("min_ms"))
    max_ms = _optional_number(delay.get("max_ms"))
    applied = delay.get("applied_in")
    applied_text = applied if isinstance(applied, str) else None
    record = {
        "distribution": delay.get("distribution") if isinstance(delay.get("distribution"), str) else None,
        "min_ms": min_ms,
        "max_ms": max_ms,
        "applied_in": applied_text,
        "per_request": delay.get("per_request") if isinstance(delay.get("per_request"), bool) else None,
        "gate": False,
        "setting": delay.get("setting") if isinstance(delay.get("setting"), str) else None,
    }
    complete = applied_text == "wall_time" and min_ms is not None and max_ms is not None
    variant = body.get("variant")
    upload_source = body.get("page_upload_source", "absent")
    if not isinstance(upload_source, str):
        upload_source = "absent"
    return {
        "schema": 1,
        "metric": "slow_io_flight",
        "variant": variant if isinstance(variant, str) else None,
        "source": upload_source,
        "debug_io_delay_ms": record,
        "frame_time_ms": frame_time,
        "page_upload_ms_p95": page_p95,
        "record_complete": complete,
        "pass": None,
        "timing_gate": "PQ",
        "filled_by": PAGE_UPLOAD_SOURCE,
    }


def exclusion_report(
    cells_document: object | None,
    observed: object | None = None,
    track: object | None = None,
) -> dict[str, object]:
    """Routes and reasons, plus runtime mismatches once debug stats exist."""
    if cells_document is None and observed is None:
        return {
            "schema": 1,
            "metric": "exclusion_matrix",
            "counters_source": "absent",
            "cell_count": 0,
            "double_drawn": None,
            "missing": None,
            "mismatches": [],
            "dlss": _dlss_na(),
            "pass": None,
            "filled_by": DEBUG_STATS_SOURCE,
        }
    cells = _cells(cells_document, observed)
    observed_body = as_dict(observed) if isinstance(observed, dict) else {}
    source = observed_body.get("counters_source", "absent")
    if not isinstance(source, str):
        source = "absent"
    double_drawn = _count(observed_body.get("double_drawn"), 0)
    missing = _count(observed_body.get("missing"), 0)
    frame = _count(observed_body.get("frame"), 0)
    mismatches: list[str] = []
    if source != "absent":
        stats = observed_body.get("debug_stats")
        mismatches = _mismatches(cells, stats, frame)
    if double_drawn != 0 or missing != 0 or mismatches:
        passed: bool | None = False
    elif not cells:
        passed = False
    else:
        passed = True
    return {
        "schema": 1,
        "metric": "exclusion_matrix",
        "counters_source": source,
        "cell_count": len(cells),
        "double_drawn": double_drawn,
        "missing": missing,
        "mismatches": mismatches,
        "dlss": _dlss(observed_body.get("dlss"), track),
        "pass": passed,
        "filled_by": DEBUG_STATS_SOURCE,
    }


def _gpu_index(engine: str | None) -> int | None:
    if engine is None:
        return None
    parts = engine.split()
    for index, part in enumerate(parts):
        token = part
        if token == "--gpu-index" and index + 1 < len(parts):
            token = parts[index + 1]
        elif token.startswith("--gpu-index="):
            token = token.split("=", 1)[1]
        else:
            continue
        try:
            return int(token)
        except ValueError:
            return None
    return None


def run_envelope(manifest: object | None) -> dict[str, object]:
    """Host, GPU, driver, build hash and seed. Missing command line leaves gpu_index null."""
    if not isinstance(manifest, dict):
        return {
            "host": None,
            "gpu": None,
            "gpu_index": None,
            "driver": None,
            "build_hash": None,
            "seed": None,
            "dlss": "N/A",
        }
    body = as_dict(manifest)
    adapter = as_dict(body["adapter"]) if isinstance(body.get("adapter"), dict) else {}
    command = as_dict(body["command_line"]) if isinstance(body.get("command_line"), dict) else {}
    engine = command.get("engine")
    engine_text = engine if isinstance(engine, str) else None
    dlss: object = "N/A"
    if isinstance(body.get("dlss"), dict):
        record = as_dict(body["dlss"])
        digest = record.get("dll_sha256")
        preset = record.get("preset")
        if isinstance(digest, str) and digest:
            dlss = {"dll_sha256": digest, "preset": preset if isinstance(preset, str) else ""}
    seed = body.get("seed")
    return {
        "host": body.get("host") if isinstance(body.get("host"), str) else None,
        "gpu": adapter.get("name") if isinstance(adapter.get("name"), str) else None,
        "gpu_index": _gpu_index(engine_text),
        "driver": adapter.get("api_version") if isinstance(adapter.get("api_version"), str) else None,
        "build_hash": body.get("commit") if isinstance(body.get("commit"), str) else None,
        "seed": seed if isinstance(seed, (int, str)) and not isinstance(seed, bool) else None,
        "dlss": dlss,
    }


def render_markdown(report: dict[str, object]) -> str:
    """Short Markdown summary. Q15 always names the ratio fields."""
    metric = report.get("metric", "report")
    lines = [f"# {metric}", "", f"schema: {report.get('schema', 1)}", f"pass: {report.get('pass')}", ""]
    if metric == "Q15":
        lines.append("Categories include authored_remainder, lods, shadow_mesh, coarse_fallback,")
        lines.append("rt_arena, blas, tlas, as_scratch, dag_headers and page_cache.")
        lines.append(f"ratio (stripped/baseline): {report.get('ratio')}")
        lines.append(f"rt_share: {report.get('rt_share')}")
        lines.append(f"retain_authored: {report.get('retain_authored')} (reported, not gated)")
    elif metric == "slow_io_flight":
        frame = report.get("frame_time_ms")
        lines.append(f"frame_time_ms: {frame}")
        lines.append(f"page_upload_ms_p95: {report.get('page_upload_ms_p95')}")
        lines.append("Delays are applied in wall time. The p99 gate is PQ.")
        lines.append(f"record_complete: {report.get('record_complete')}")
    elif metric == "exclusion_matrix":
        lines.append(f"double_drawn: {report.get('double_drawn')}")
        lines.append(f"missing: {report.get('missing')}")
        lines.append(f"counters_source: {report.get('counters_source')}")
        lines.append(f"dlss: {report.get('dlss')}")
    else:
        lines.append(f"mean: {report.get('mean')}")
        lines.append(f"maximum: {report.get('maximum')}")
        lines.append(f"source: {report.get('source')}")
    lines.append("")
    return "\n".join(lines)


def _q14_shell(
    source: str,
    count: int,
    mean: float | None,
    maximum: float | None,
    passed: bool | None,
) -> dict[str, object]:
    return {
        "schema": 1,
        "metric": "Q14",
        "report": "fallback_coverage",
        "source": source,
        "frames": count,
        "mean": mean,
        "maximum": maximum,
        "threshold": Q14_THRESHOLD,
        "pass": passed,
        "filled_by": "fallback_coverage",
    }


def _frame_ratio(frame: object) -> float | None:
    if not isinstance(frame, dict):
        return None
    body = as_dict(frame)
    if "legacy_drawn_virtualized_pixels" in body and "opaque_pixels" in body:
        try:
            legacy = as_float(body["legacy_drawn_virtualized_pixels"])
            opaque = as_float(body["opaque_pixels"])
        except ValueError:
            return None
        return _q14_value(legacy, opaque)
    if "fallback_coverage" not in body or body["fallback_coverage"] is None:
        return None
    try:
        return as_float(body["fallback_coverage"])
    except ValueError:
        return None


def _q14_value(legacy: float, opaque: float) -> float:
    result = q14_fallback_coverage(legacy, opaque)
    if isinstance(result.value, float):
        return result.value
    if isinstance(result.value, int) and not isinstance(result.value, bool):
        return float(result.value)
    return math.nan


def _optional_samples(value: object) -> list[float] | None:
    if value is None or not isinstance(value, list):
        return None
    return _finite(as_list(value))


def _finite(values: list[object] | list[float] | None) -> list[float]:
    if not values:
        return []
    samples: list[float] = []
    for value in values:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            continue
        number = float(value)
        if math.isfinite(number):
            samples.append(number)
    return samples


def _optional_number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


def _cells(cells_document: object | None, observed: object | None) -> list[dict[str, object]]:
    for document in (cells_document, observed):
        if not isinstance(document, dict):
            continue
        raw = document.get("cells")
        if isinstance(raw, list):
            return [as_dict(item) for item in raw if isinstance(item, dict)]
    return []


def _count(value: object, default: int) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return int(value)


def _mismatches(cells: list[dict[str, object]], stats: object, frame: int) -> list[str]:
    if not isinstance(stats, dict):
        return []
    raw = stats.get("cells")
    if not isinstance(raw, list):
        return []
    found: dict[str, dict[str, object]] = {}
    for item in raw:
        if isinstance(item, dict) and isinstance(item.get("id"), str):
            found[str(item["id"])] = as_dict(item)
    mismatches: list[str] = []
    for cell in cells:
        cell_id = cell.get("id")
        if not isinstance(cell_id, str):
            continue
        got = found.get(cell_id)
        if got is None:
            mismatches.append(f"{cell_id}: missing from debug stats")
            continue
        expected = _surfaces(cell.get("surfaces"))
        actual = _surfaces(got.get("surfaces"))
        for index, surface in enumerate(expected):
            route, reason = _expected_after(surface, frame)
            if index >= len(actual):
                mismatches.append(f"{cell_id}: surface {index} missing")
                continue
            got_route = actual[index].get("route")
            got_reason = actual[index].get("reason")
            if got_route != route or got_reason != reason:
                mismatches.append(f"{cell_id}: surface {index} expected {route}/{reason}, got {got_route}/{got_reason}")
    return mismatches


def _surfaces(value: object) -> list[dict[str, object]]:
    if not isinstance(value, list):
        return []
    return [as_dict(item) for item in value if isinstance(item, dict)]


def _expected_after(surface: dict[str, object], frame: int) -> tuple[object, object]:
    boundary = surface.get("reason_by_frame")
    if isinstance(boundary, int) and not isinstance(boundary, bool) and frame >= boundary:
        return surface.get("route_after"), surface.get("reason_after")
    return surface.get("route"), surface.get("reason")


def _dlss(observed: object, track: object | None) -> dict[str, object]:
    for candidate in (observed, _track_dlss(track)):
        if isinstance(candidate, dict):
            body = as_dict(candidate)
            if "status" in body or "dll_sha256" in body:
                return _dlss_from(body)
    return _dlss_na()


def _track_dlss(track: object | None) -> object:
    if not isinstance(track, dict):
        return None
    return track.get("dlss")


def _dlss_from(body: dict[str, object]) -> dict[str, object]:
    status = body.get("status")
    fallback = body.get("fallback")
    digest = body.get("dll_sha256")
    preset = body.get("preset")
    scale = body.get("scale")
    return {
        "status": status if isinstance(status, str) else "N/A",
        "fallback": fallback if isinstance(fallback, str) else "FSR2",
        "scale": float(scale) if isinstance(scale, (int, float)) and not isinstance(scale, bool) else 0.667,
        "dll_sha256": digest if isinstance(digest, str) else "",
        "preset": preset if isinstance(preset, str) else "",
    }


def _dlss_na() -> dict[str, object]:
    return {
        "status": "N/A",
        "fallback": "FSR2",
        "scale": 0.667,
        "dll_sha256": "",
        "preset": "",
    }
