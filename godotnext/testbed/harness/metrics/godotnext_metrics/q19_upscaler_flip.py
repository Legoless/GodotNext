"""Q19 upscaler image quality.

LDR FLIP follows Q7. A scene reference is accepted only when the mean FLIP
between its MSAA 4x and MSAA 8x references is <= 0.01. A content mask whose
own 4x/8x FLIP exceeds 0.01 is excluded and reported. The moving form drops
the 32 frames starting at the reset cut, after a 120-frame warm-up. The static
form uses 64 frames after 120 settling frames. ``pass`` is reference acceptance.
The quality ceilings are comparisons with FSR2 or TAA and are not a fixed number.
"""

from __future__ import annotations

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.flip_eval import ldr_flip
from godotnext_metrics.percentile import percentile
from godotnext_metrics.result import MetricResult

Q19_REFERENCE_FLIP = 0.01
Q19_SETTLING = 120
Q19_STATIC_FRAMES = 64
Q19_RESET_EXCLUDE = 32
BILINEAR_SCALING_MODE = 0
REFERENCE_SCALE = 2.0


def q19_upscaler_flip(
    test_frames: list[NDArray[np.generic]],
    reference_frames: list[NDArray[np.generic]],
    msaa4_frames: list[NDArray[np.generic]] | None,
    *,
    content_masks: dict[str, NDArray[np.generic]] | None = None,
    form: str = "static",
    exposure: float = 1.0,
    already_display: bool = False,
    scene: str = "",
    output_resolution: tuple[int, int] | None = None,
) -> MetricResult:
    if not test_frames or len(test_frames) != len(reference_frames):
        raise ValueError("Q19 test and reference frame counts differ")
    if msaa4_frames is None or len(msaa4_frames) != len(reference_frames):
        return _rejected("MSAA 4x reference is missing or its frame count differs", scene, output_resolution)
    convergence_values: list[float] = []
    pooled: list[np.ndarray] = []
    per_frame: list[float] = []
    for test, reference, msaa4 in zip(test_frames, reference_frames, msaa4_frames, strict=True):
        conv_mean, _conv_p99, conv_map = ldr_flip(reference, msaa4, exposure=exposure, already_display=already_display)
        convergence_values.append(conv_mean)
        mean, _p99, error_map = ldr_flip(reference, test, exposure=exposure, already_display=already_display)
        per_frame.append(mean)
        pooled.append(error_map)
        _ = conv_map
    convergence = float(np.mean(np.asarray(convergence_values, dtype=np.float64)))
    accepted = bool(convergence <= Q19_REFERENCE_FLIP)
    flat = np.concatenate(pooled) if pooled else np.zeros(1, dtype=np.float64)
    mean_flip = float(np.mean(np.asarray(per_frame, dtype=np.float64)))
    p99 = percentile(flat, 99.0)
    masks = content_masks or {}
    per_mask, excluded = _mask_scores(
        reference_frames,
        msaa4_frames,
        test_frames,
        masks,
        exposure=exposure,
        already_display=already_display,
    )
    resolution = output_resolution
    if resolution is None:
        shape = np.asarray(test_frames[0]).shape
        resolution = (int(shape[1]), int(shape[0]))
    return MetricResult(
        metric="Q19",
        value=mean_flip if accepted else None,
        threshold={"reference_flip": Q19_REFERENCE_FLIP},
        passed=accepted,
        values={
            "mean": mean_flip,
            "p99": p99,
            "convergence_flip": convergence,
            "per_mask": per_mask,
            "form": form,
            "scene": scene,
            "output_resolution": list(resolution),
        },
        details={
            "reference_accepted": accepted,
            "excluded_masks": excluded,
            "frames": len(test_frames),
        },
    )


def select_q19_frames(indices: list[int], *, form: str, reset_frame: int | None) -> list[int]:
    """Frame indices the moving or static form keeps."""
    ordered = sorted(indices)
    if form == "static":
        return [index for index in ordered if Q19_SETTLING <= index < Q19_SETTLING + Q19_STATIC_FRAMES]
    if form != "moving":
        raise ValueError("Q19 form must be static or moving")
    kept: list[int] = []
    for index in ordered:
        if index < Q19_SETTLING:
            continue
        if reset_frame is not None and reset_frame <= index < reset_frame + Q19_RESET_EXCLUDE:
            continue
        kept.append(index)
    return kept


def _mask_scores(
    reference_frames: list[NDArray[np.generic]],
    msaa4_frames: list[NDArray[np.generic]],
    test_frames: list[NDArray[np.generic]],
    masks: dict[str, NDArray[np.generic]],
    *,
    exposure: float,
    already_display: bool,
) -> tuple[dict[str, float], list[str]]:
    per_mask: dict[str, float] = {}
    excluded: list[str] = []
    if not masks:
        return per_mask, excluded
    # Convergence and score use the first paired frame's error maps averaged over the mask.
    # Full sequences average the per-frame masked means.
    for name, mask in masks.items():
        selected = _mask_bool(mask)
        conv_means: list[float] = []
        score_means: list[float] = []
        for reference, msaa4, test in zip(reference_frames, msaa4_frames, test_frames, strict=True):
            conv_mean, _p99, conv_map = ldr_flip(reference, msaa4, exposure=exposure, already_display=already_display)
            _ = conv_mean
            score_mean, _score_p99, score_map = ldr_flip(
                reference, test, exposure=exposure, already_display=already_display
            )
            _ = score_mean
            conv_image = conv_map.reshape(selected.shape)
            score_image = score_map.reshape(selected.shape)
            if np.any(selected):
                conv_means.append(float(np.mean(conv_image[selected])))
                score_means.append(float(np.mean(score_image[selected])))
        conv = float(np.mean(np.asarray(conv_means, dtype=np.float64))) if conv_means else 0.0
        if conv > Q19_REFERENCE_FLIP:
            excluded.append(name)
            continue
        per_mask[name] = float(np.mean(np.asarray(score_means, dtype=np.float64))) if score_means else 0.0
    return per_mask, excluded


def _mask_bool(mask: NDArray[np.generic]) -> NDArray[np.bool_]:
    array = np.asarray(mask)
    if array.ndim == 3:
        array = array[..., 0]
    if np.issubdtype(array.dtype, np.floating):
        return np.asarray(array > 0.5, dtype=bool)
    return np.asarray(array > 0, dtype=bool)


def _rejected(reason: str, scene: str, output_resolution: tuple[int, int] | None) -> MetricResult:
    return MetricResult(
        metric="Q19",
        value=None,
        threshold={"reference_flip": Q19_REFERENCE_FLIP},
        passed=False,
        details={"reference_accepted": False, "error": reason, "scene": scene, "output_resolution": output_resolution},
    )
