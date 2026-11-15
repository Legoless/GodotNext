"""Q19 upscaler image quality.

LDR FLIP follows Q7. A scene reference is accepted only when the mean FLIP
between its MSAA 4x and MSAA 8x references is <= 0.01. A content mask whose
own 4x/8x FLIP exceeds 0.01 is excluded and reported. The moving form drops
the 32 frames starting at the reset cut, after a 120-frame warm-up. The static
form uses 64 frames after 120 settling frames. ``pass`` is reference acceptance.
The quality ceilings are comparisons with FSR2 or TAA and are not a fixed number.

A content mask is one image for every frame, or one image per frame (#190). Its score is the
mean over frames of the frozen FLIP error map averaged inside that frame's mask.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence

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
    content_masks: Mapping[str, NDArray[np.generic] | Sequence[NDArray[np.generic]]] | None = None,
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
    masks = {name: _mask_frames(mask, len(test_frames)) for name, mask in (content_masks or {}).items()}
    masked = {name: _MaskedMeans() for name in masks}
    convergence_values: list[float] = []
    pooled: list[np.ndarray] = []
    per_frame: list[float] = []
    for index, (test, reference, msaa4) in enumerate(zip(test_frames, reference_frames, msaa4_frames, strict=True)):
        conv_mean, _conv_p99, conv_map = ldr_flip(reference, msaa4, exposure=exposure, already_display=already_display)
        convergence_values.append(conv_mean)
        mean, _p99, error_map = ldr_flip(reference, test, exposure=exposure, already_display=already_display)
        per_frame.append(mean)
        pooled.append(error_map)
        for name, frames in masks.items():
            masked[name].add(frames[index], conv_map, error_map)
    convergence = float(np.mean(np.asarray(convergence_values, dtype=np.float64)))
    accepted = bool(convergence <= Q19_REFERENCE_FLIP)
    flat = np.concatenate(pooled) if pooled else np.zeros(1, dtype=np.float64)
    mean_flip = float(np.mean(np.asarray(per_frame, dtype=np.float64)))
    p99 = percentile(flat, 99.0)
    per_mask, excluded = _mask_scores(masked)
    resolution = output_resolution
    if resolution is None:
        shape = np.asarray(test_frames[0]).shape
        resolution = (int(shape[1]), int(shape[0]))
    details: dict[str, object] = {
        "reference_accepted": accepted,
        "excluded_masks": excluded,
        "frames": len(test_frames),
    }
    if masks:
        details["mask_pixels"] = {name: means.pixels() for name, means in masked.items()}
        details["mask_convergence_flip"] = {name: means.convergence() for name, means in masked.items()}
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
        details=details,
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


class _MaskedMeans:
    """Per-frame means of the convergence and score error maps inside one content mask."""

    def __init__(self) -> None:
        self.conv: list[float] = []
        self.score: list[float] = []
        self.counts: list[int] = []

    def add(self, selected: NDArray[np.bool_], conv_map: np.ndarray, score_map: np.ndarray) -> None:
        # Frames where the mask is empty do not count.
        if not np.any(selected):
            return
        self.conv.append(float(np.mean(conv_map.reshape(selected.shape)[selected])))
        self.score.append(float(np.mean(score_map.reshape(selected.shape)[selected])))
        self.counts.append(int(np.count_nonzero(selected)))

    def pixels(self) -> dict[str, object]:
        """How many frames the mask covered, and its mean and smallest pixel count on them."""
        if not self.counts:
            return {"frames": 0, "mean": 0.0, "min": 0}
        return {"frames": len(self.counts), "mean": float(np.mean(self.counts)), "min": min(self.counts)}

    def convergence(self) -> float:
        return float(np.mean(np.asarray(self.conv, dtype=np.float64))) if self.conv else 0.0

    def mean_score(self) -> float:
        return float(np.mean(np.asarray(self.score, dtype=np.float64))) if self.score else 0.0


def _mask_scores(masked: dict[str, _MaskedMeans]) -> tuple[dict[str, float], list[str]]:
    """A mask whose own MSAA 4x/8x FLIP exceeds 0.01 is excluded and reported, with no score."""
    per_mask: dict[str, float] = {}
    excluded: list[str] = []
    for name, means in masked.items():
        if means.convergence() > Q19_REFERENCE_FLIP:
            excluded.append(name)
            continue
        per_mask[name] = means.mean_score()
    return per_mask, excluded


def _mask_frames(
    mask: NDArray[np.generic] | Sequence[NDArray[np.generic]], frame_count: int
) -> list[NDArray[np.bool_]]:
    """One boolean mask per frame: a single image repeats, a sequence must match the frame count."""
    if isinstance(mask, np.ndarray):
        plane = mask_bool(mask)
        return [plane for _ in range(frame_count)]
    frames = [mask_bool(frame) for frame in mask]
    if len(frames) != frame_count:
        raise ValueError(f"a per-frame content mask has {len(frames)} frames for {frame_count} captured frames")
    return frames


def mask_bool(mask: NDArray[np.generic]) -> NDArray[np.bool_]:
    """A content mask's pixels: a non-zero first channel, or above 0.5 for a float image."""
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
