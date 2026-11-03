"""Q20 final-image indirect error.

This calls Q1's patch function on the final pre-tonemap indirect luminance.
On Roughness Corridor the same function is applied per Q9 roughness bin.
"""

from __future__ import annotations

import numpy as np
from numpy.typing import ArrayLike

from godotnext_metrics.image_metrics import ROUGHNESS_BINS
from godotnext_metrics.indirect import Q1_MAX_RELATIVE_SE, Q1_MIN_SPP, Q1_THRESHOLD, q1_patch_error
from godotnext_metrics.result import MetricResult


def q20_final_indirect(
    l_test: ArrayLike,
    l_ref: ArrayLike,
    *,
    scene_mean_indirect: float | None = None,
    spp: int | None = None,
    per_patch_relative_se: ArrayLike | None = None,
    roughness_bins: dict[str, tuple[ArrayLike, ArrayLike]] | None = None,
    threshold: float = Q1_THRESHOLD,
) -> MetricResult:
    ref = np.asarray(l_ref, dtype=np.float64).reshape(-1)
    scene_mean = float(np.mean(ref)) if scene_mean_indirect is None else float(scene_mean_indirect)
    error = q1_patch_error(l_test, ref, scene_mean)
    rejected = spp is None or spp < Q1_MIN_SPP
    se_reason = ""
    if per_patch_relative_se is None:
        rejected = True
        se_reason = "per-patch relative standard error is missing"
    else:
        worst = float(np.max(np.asarray(per_patch_relative_se, dtype=np.float64)))
        if worst > Q1_MAX_RELATIVE_SE:
            rejected = True
            se_reason = f"per-patch relative standard error {worst} is above {Q1_MAX_RELATIVE_SE}"
    bin_errors: dict[str, float] = {}
    if roughness_bins:
        for name, low, high, _gated in ROUGHNESS_BINS:
            if name not in roughness_bins:
                continue
            test_bin, ref_bin = roughness_bins[name]
            ref_values = np.asarray(ref_bin, dtype=np.float64).reshape(-1)
            if ref_values.size == 0:
                continue
            bin_errors[name] = q1_patch_error(test_bin, ref_values, float(np.mean(ref_values)))
            _ = (low, high)
    passed = bool(error <= threshold and not rejected)
    return MetricResult(
        metric="Q20",
        value=error,
        threshold=threshold,
        passed=passed,
        values={"error": error, "bins": bin_errors, "scene_mean_indirect": scene_mean},
        details={
            "reference_rejected": rejected,
            "reference_reason": se_reason,
            "spp": spp,
            "reuses_q1_patch_error": True,
        },
    )
