"""Q1, Q3, Q4a, Q4b and Q8.

Q1's patch error is the function Q20 calls. The denominator floor is
``max(L_ref, 0.01 * scene mean indirect)``, with the scene mean taken over the
reference patches.
"""

from __future__ import annotations

import math

import numpy as np
from numpy.typing import ArrayLike, NDArray

from godotnext_metrics.result import MetricResult

Q1_THRESHOLD = 0.20
Q3_THRESHOLD = 0.01
Q3_REFERENCE_FLAG = 0.001
Q4_E10 = 0.25
Q4_E32 = 0.10
Q4B_MEAN = 0.25
Q8_THRESHOLD = 0.015
Q1_MIN_SPP = 4096
Q4B_MIN_SPP = 1024
Q1_MAX_RELATIVE_SE = 0.01


def q1_patch_error(l_test: ArrayLike, l_ref: ArrayLike, scene_mean_indirect: float) -> float:
    """Mean over patches of ``|L_test - L_ref| / max(L_ref, 0.01 * scene mean)``."""
    test = np.asarray(l_test, dtype=np.float64).reshape(-1)
    ref = np.asarray(l_ref, dtype=np.float64).reshape(-1)
    if test.shape != ref.shape:
        raise ValueError("Q1 test and reference patch vectors differ in length")
    if test.size == 0:
        raise ValueError("Q1 needs at least one patch")
    floor = 0.01 * float(scene_mean_indirect)
    denominator = np.maximum(ref, floor)
    numerator = np.abs(test - ref)
    ratio = _safe_ratio(numerator, denominator)
    return float(np.mean(ratio))


def q1_indirect_diffuse_error(
    l_test: ArrayLike,
    l_ref: ArrayLike,
    *,
    scene_mean_indirect: float | None = None,
    spp: int | None = None,
    per_patch_relative_se: ArrayLike | None = None,
    threshold: float = Q1_THRESHOLD,
) -> MetricResult:
    ref = np.asarray(l_ref, dtype=np.float64).reshape(-1)
    scene_mean = float(np.mean(ref)) if scene_mean_indirect is None else float(scene_mean_indirect)
    error = q1_patch_error(l_test, ref, scene_mean)
    rejected, reason = _reference_rejection(spp, per_patch_relative_se, Q1_MIN_SPP, Q1_MAX_RELATIVE_SE)
    return MetricResult(
        metric="Q1",
        value=error,
        threshold=threshold,
        passed=bool(error <= threshold and not rejected),
        values={"error": error, "scene_mean_indirect": scene_mean, "patches": int(ref.size)},
        details={"reference_rejected": rejected, "reference_reason": reason, "spp": spp},
    )


def q3_leak_ratio(
    dark_indirect: ArrayLike,
    lit_indirect: ArrayLike,
    *,
    reference_dark: ArrayLike | None = None,
    reference_lit: ArrayLike | None = None,
    threshold: float = Q3_THRESHOLD,
) -> MetricResult:
    ratio = _room_ratio(dark_indirect, lit_indirect)
    flagged = False
    reference_ratio: float | None = None
    if reference_dark is not None and reference_lit is not None:
        reference_ratio = _room_ratio(reference_dark, reference_lit)
        flagged = bool(reference_ratio >= Q3_REFERENCE_FLAG)
    passed = bool(ratio <= threshold and not flagged)
    return MetricResult(
        metric="Q3",
        value=ratio,
        threshold=threshold,
        passed=passed,
        values={"leak_ratio": ratio, "reference_leak_ratio": reference_ratio},
        details={
            "reference_flagged": flagged,
            "reference_flag_at": Q3_REFERENCE_FLAG,
            "dark_patches": 16,
            "lit_patches": 16,
        },
    )


def q4a_step_convergence(
    luminance_at_10: ArrayLike,
    luminance_at_32: ArrayLike,
    luminance_end: ArrayLike,
) -> MetricResult:
    e10 = _endpoint_error(luminance_at_10, luminance_end)
    e32 = _endpoint_error(luminance_at_32, luminance_end)
    passed = bool(e10 <= Q4_E10 and e32 <= Q4_E32)
    return MetricResult(
        metric="Q4a",
        value={"e10": e10, "e32": e32},
        threshold={"e10": Q4_E10, "e32": Q4_E32},
        passed=passed,
        values={"e10": e10, "e32": e32},
    )


def q4b_continuous_convergence(
    test_by_frame: dict[int, ArrayLike],
    reference_by_frame: dict[int, ArrayLike],
    *,
    motion_start: int,
    motion_end: int,
    spp: int | None,
) -> MetricResult:
    """``motion_end`` is the first frame index after the motion has stopped."""
    samples = list(range(motion_start, motion_end, 4))
    if not samples:
        raise ValueError("Q4b motion range has no every-4th-frame sample")
    lag_values: list[float] = []
    for frame in samples:
        if frame not in test_by_frame or frame not in reference_by_frame:
            raise ValueError(f"Q4b is missing test or reference luminance at frame {frame}")
        reference = np.asarray(reference_by_frame[frame], dtype=np.float64).reshape(-1)
        test = np.asarray(test_by_frame[frame], dtype=np.float64).reshape(-1)
        lag_values.append(float(np.mean(_safe_ratio(np.abs(test - reference), reference))))
    mean_lag = float(np.mean(np.asarray(lag_values, dtype=np.float64)))
    after = motion_end + 32
    end = motion_end + 240
    if after not in test_by_frame or end not in test_by_frame:
        raise ValueError("Q4b needs the test frame 32 after the motion and the frame 240 after it")
    e32 = _endpoint_error(test_by_frame[after], test_by_frame[end])
    spp_ok = spp is not None and spp >= Q4B_MIN_SPP
    passed = bool(mean_lag <= Q4B_MEAN and e32 <= Q4_E32 and spp_ok)
    return MetricResult(
        metric="Q4b",
        value={"mean_e_lag": mean_lag, "e32": e32},
        threshold={"mean_e_lag": Q4B_MEAN, "e32": Q4_E32, "min_spp": Q4B_MIN_SPP},
        passed=passed,
        values={"mean_e_lag": mean_lag, "e32": e32, "samples": len(samples)},
        details={"spp": spp, "spp_accepted": spp_ok, "motion_start": motion_start, "motion_end": motion_end},
    )


def q8_temporal_sigma(samples: ArrayLike, *, threshold: float = Q8_THRESHOLD) -> MetricResult:
    """``samples`` is frames by patches. Relative σ is the population std divided by the mean."""
    data = np.asarray(samples, dtype=np.float64)
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    if data.shape[0] < 2:
        raise ValueError("Q8 needs at least two frames")
    mean = np.mean(data, axis=0)
    deviation = np.std(data, axis=0)
    relative = np.zeros_like(mean)
    nonzero = mean != 0.0
    relative[nonzero] = deviation[nonzero] / np.abs(mean[nonzero])
    relative[~nonzero] = np.where(deviation[~nonzero] == 0.0, 0.0, math.inf)
    value = float(np.mean(relative))
    return MetricResult(
        metric="Q8",
        value=value,
        threshold=threshold,
        passed=bool(value <= threshold and data.shape[0] == 64),
        values={
            "mean_relative_sigma": value,
            "max_relative_sigma": float(np.max(relative)),
            "frames": int(data.shape[0]),
        },
        details={"frame_count_ok": data.shape[0] == 64},
    )


def _endpoint_error(luminance_t: ArrayLike, luminance_end: ArrayLike) -> float:
    current = np.asarray(luminance_t, dtype=np.float64).reshape(-1)
    end = np.asarray(luminance_end, dtype=np.float64).reshape(-1)
    return float(np.mean(_safe_ratio(np.abs(current - end), end)))


def _room_ratio(dark: ArrayLike, lit: ArrayLike) -> float:
    dark_mean = float(np.mean(np.asarray(dark, dtype=np.float64)))
    lit_mean = float(np.mean(np.asarray(lit, dtype=np.float64)))
    if lit_mean == 0.0:
        return 0.0 if dark_mean == 0.0 else math.inf
    return dark_mean / lit_mean


def _safe_ratio(numerator: NDArray[np.float64], denominator: NDArray[np.float64]) -> NDArray[np.float64]:
    ratio = np.zeros(numerator.shape, dtype=np.float64)
    ok = denominator > 0.0
    ratio[ok] = numerator[ok] / denominator[ok]
    ratio[~ok] = np.where(numerator[~ok] == 0.0, 0.0, math.inf)
    return ratio


def _reference_rejection(
    spp: int | None,
    per_patch_relative_se: ArrayLike | None,
    min_spp: int,
    max_se: float,
) -> tuple[bool, str]:
    if spp is None:
        return True, f"reference spp is missing (need >= {min_spp})"
    if spp < min_spp:
        return True, f"reference spp {spp} is below {min_spp}"
    if per_patch_relative_se is None:
        return True, "per-patch relative standard error is missing"
    errors = np.asarray(per_patch_relative_se, dtype=np.float64).reshape(-1)
    if errors.size == 0:
        return True, "per-patch relative standard error is empty"
    worst = float(np.max(errors))
    if worst > max_se:
        return True, f"per-patch relative standard error {worst} is above {max_se}"
    return False, ""
