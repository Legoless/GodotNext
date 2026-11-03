"""Q7, Q9 and Q10."""

from __future__ import annotations

import numpy as np
from numpy.typing import ArrayLike, NDArray

from godotnext_metrics.color import luminance
from godotnext_metrics.flip_eval import ldr_flip
from godotnext_metrics.indirect import q1_patch_error
from godotnext_metrics.result import MetricResult

Q7_MEAN_THRESHOLD = 0.05
Q9_ERROR_THRESHOLD = 0.30
Q9_FLIP_LOW = 0.15  # roughness bin [0.1, 0.25)
Q9_FLIP_HIGH = 0.10  # roughness bin [0.25, 0.4]
Q10_THRESHOLD = 0.10

# [low, high) roughness. The mirror bin is reported and does not fail the gate.
ROUGHNESS_BINS: tuple[tuple[str, float, float, bool], ...] = (
    ("[0, 0.1)", 0.0, 0.1, False),
    ("[0.1, 0.25)", 0.1, 0.25, True),
    ("[0.25, 0.4]", 0.25, 0.4, True),
)


def q7_flip(
    reference: NDArray[np.generic],
    test: NDArray[np.generic],
    *,
    exposure: float = 1.0,
    already_display: bool = False,
    threshold: float = Q7_MEAN_THRESHOLD,
) -> MetricResult:
    mean, p99, _scalar = ldr_flip(reference, test, exposure=exposure, already_display=already_display)
    return MetricResult(
        metric="Q7",
        value={"mean": mean, "p99": p99},
        threshold={"mean": threshold},
        passed=bool(mean <= threshold),
        values={"mean": mean, "p99": p99},
    )


def q9_reflection_error(
    reference: NDArray[np.generic],
    test: NDArray[np.generic],
    roughness: ArrayLike,
    *,
    exposure: float = 1.0,
) -> MetricResult:
    rough = np.asarray(roughness, dtype=np.float64)
    if rough.shape[:2] != np.asarray(reference).shape[:2]:
        raise ValueError("roughness map size does not match the reflection image")
    ref_rgb = np.asarray(reference)
    test_rgb = np.asarray(test)
    ref_luma = luminance(ref_rgb.astype(np.float64))
    test_luma = luminance(test_rgb.astype(np.float64))
    _mean, _p99, error_map = ldr_flip(reference, test, exposure=exposure, already_display=False)
    error_image = error_map.reshape(ref_luma.shape)
    bins: dict[str, dict[str, float | bool | None]] = {}
    passed = True
    for name, low, high, gated in ROUGHNESS_BINS:
        # The top gate bin includes roughness 0.4. The open interval in the plan is [0.25, 0.4].
        if name == "[0.25, 0.4]":
            selected = (rough >= low) & (rough <= high)
        else:
            selected = (rough >= low) & (rough < high)
        if not np.any(selected):
            bins[name] = {"error": None, "flip_mean": None, "gated": gated, "pixels": 0}
            continue
        ref_values = ref_luma[selected]
        test_values = test_luma[selected]
        scene_mean = float(np.mean(ref_values))
        error = q1_patch_error(test_values, ref_values, scene_mean)
        flip_mean = float(np.mean(error_image[selected]))
        flip_limit = _flip_limit(name)
        bin_pass = True
        if gated:
            bin_pass = error <= Q9_ERROR_THRESHOLD and (flip_limit is None or flip_mean <= flip_limit)
        bins[name] = {
            "error": error,
            "flip_mean": flip_mean,
            "flip_threshold": flip_limit,
            "gated": gated,
            "pixels": int(np.count_nonzero(selected)),
            "pass": bin_pass,
        }
        passed = passed and bin_pass
    return MetricResult(
        metric="Q9",
        value={name: bins[name]["error"] for name, *_rest in ROUGHNESS_BINS},
        threshold={"radiance_error": Q9_ERROR_THRESHOLD, "[0.1, 0.25)": Q9_FLIP_LOW, "[0.25, 0.4]": Q9_FLIP_HIGH},
        passed=passed,
        values={"bins": bins},
        details={"mirror_bin_reported_only": True},
    )


def q10_ghost_trail(
    reference: NDArray[np.generic],
    test: NDArray[np.generic],
    disocclusion: ArrayLike,
    *,
    threshold: float = Q10_THRESHOLD,
) -> MetricResult:
    mask = np.asarray(disocclusion)
    selected = mask > (0.5 if np.issubdtype(mask.dtype, np.floating) else 0)
    if not np.any(selected):
        raise ValueError("Q10 disocclusion mask is empty")
    ref_luma = luminance(np.asarray(reference, dtype=np.float64))[selected]
    test_luma = luminance(np.asarray(test, dtype=np.float64))[selected]
    error = q1_patch_error(test_luma, ref_luma, float(np.mean(ref_luma)))
    return MetricResult(
        metric="Q10",
        value=error,
        threshold=threshold,
        passed=bool(error <= threshold),
        values={"error": error, "pixels": int(np.count_nonzero(selected))},
        details={"frame_after_event": 4},
    )


def _flip_limit(name: str) -> float | None:
    if name == "[0.1, 0.25)":
        return Q9_FLIP_LOW
    if name == "[0.25, 0.4]":
        return Q9_FLIP_HIGH
    return None
