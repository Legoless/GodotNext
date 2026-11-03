"""Q2, Q14, Q15, Q16 and Q18.

These read a counter JSON object. The renderer that fills it is later work.
Q15 requires the ``MEMORY_ACCELERATION_STRUCTURES`` category.
"""

from __future__ import annotations

import math

from godotnext_metrics.result import MetricResult

Q2_THRESHOLD = 0.85
Q14_THRESHOLD = 0.20
Q15_BASELINE_RATIO = 0.5
Q15_AS_FRACTION = 0.25
Q16_THRESHOLD = 0.02
Q18_THRESHOLD = 0.95
Q2_CAMERAS = ("CovCam0", "CovCam1", "CovCam2", "CovCam3")
AS_CATEGORY = "MEMORY_ACCELERATION_STRUCTURES"


def q2_card_hit_coverage(cameras: dict[str, dict[str, float]]) -> MetricResult:
    missing = [name for name in Q2_CAMERAS if name not in cameras]
    numerator = 0.0
    denominator = 0.0
    for name in Q2_CAMERAS:
        if name not in cameras:
            continue
        row = cameras[name]
        numerator += float(row["valid_card_hits"])
        denominator += float(row["hits_within_10m"])
    ratio = _ratio(numerator, denominator)
    passed = bool(not missing and ratio >= Q2_THRESHOLD)
    return MetricResult(
        metric="Q2",
        value=ratio,
        threshold=Q2_THRESHOLD,
        passed=passed,
        values={"valid_card_hits": numerator, "hits_within_10m": denominator},
        details={"missing_cameras": missing},
    )


def q14_fallback_coverage(legacy_drawn_virtualized_pixels: float, opaque_pixels: float) -> MetricResult:
    ratio = _ratio(legacy_drawn_virtualized_pixels, opaque_pixels)
    # At or above 20% fails.
    passed = bool(math.isfinite(ratio) and ratio < Q14_THRESHOLD)
    return MetricResult(
        metric="Q14",
        value=ratio,
        threshold=Q14_THRESHOLD,
        passed=passed,
        values={"legacy_drawn_virtualized_pixels": legacy_drawn_virtualized_pixels, "opaque_pixels": opaque_pixels},
    )


def q15_geometry_memory(categories: dict[str, float], *, baseline_total: float | None) -> MetricResult:
    if AS_CATEGORY not in categories:
        return MetricResult(
            metric="Q15",
            value=None,
            threshold={"baseline_ratio": Q15_BASELINE_RATIO, "as_fraction": Q15_AS_FRACTION},
            passed=False,
            details={"error": f"missing {AS_CATEGORY}"},
        )
    total = float(sum(categories.values()))
    as_bytes = float(categories[AS_CATEGORY])
    as_fraction = _ratio(as_bytes, total)
    baseline_ok = True
    baseline_ratio: float | None = None
    if baseline_total is not None:
        baseline_ratio = _ratio(total, baseline_total)
        baseline_ok = bool(math.isfinite(baseline_ratio) and baseline_ratio <= Q15_BASELINE_RATIO)
    passed = bool(baseline_ok and math.isfinite(as_fraction) and as_fraction <= Q15_AS_FRACTION)
    return MetricResult(
        metric="Q15",
        value=total,
        threshold={"baseline_ratio": Q15_BASELINE_RATIO, "as_fraction": Q15_AS_FRACTION},
        passed=passed,
        values={"total": total, "as_bytes": as_bytes, "as_fraction": as_fraction, "baseline_ratio": baseline_ratio},
    )


def q16_clamped_energy(clamped_energy: float, total_indirect_energy: float) -> MetricResult:
    fraction = _ratio(clamped_energy, total_indirect_energy)
    passed = bool(math.isfinite(fraction) and fraction <= Q16_THRESHOLD)
    return MetricResult(
        metric="Q16",
        value=fraction,
        threshold=Q16_THRESHOLD,
        passed=passed,
        values={"clamped_energy": clamped_energy, "total_indirect_energy": total_indirect_energy},
    )


def q18_rt_coverage(tlas_instances: float, gi_opaque_instances_in_radius: float) -> MetricResult:
    ratio = _ratio(tlas_instances, gi_opaque_instances_in_radius)
    passed = bool(math.isfinite(ratio) and ratio >= Q18_THRESHOLD)
    return MetricResult(
        metric="Q18",
        value=ratio,
        threshold=Q18_THRESHOLD,
        passed=passed,
        values={"tlas_instances": tlas_instances, "gi_opaque_instances_in_radius": gi_opaque_instances_in_radius},
    )


def _ratio(numerator: float, denominator: float) -> float:
    if denominator == 0.0:
        return 0.0 if numerator == 0.0 else math.inf
    return float(numerator) / float(denominator)
