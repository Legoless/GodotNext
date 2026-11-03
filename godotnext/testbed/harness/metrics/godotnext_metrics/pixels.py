"""Q5 crack pixels and Q6 hole pixels.

Magenta is the 8-bit color (255, 0, 255). A background pixel equals the clear
color, default (0, 0, 0). The reference coverage mask is eroded by 2 pixels
with a 3x3 square element applied twice (an 8-connected radius of 2).
"""

from __future__ import annotations

import numpy as np
from numpy.typing import ArrayLike, NDArray

from godotnext_metrics.result import MetricResult

Q5_THRESHOLD = 0
Q6_THRESHOLD = 0
_MAGENTA = np.array([255, 0, 255], dtype=np.uint8)


def q5_crack_pixels(
    image: ArrayLike,
    coverage_mask: ArrayLike | None = None,
    *,
    clear_color: tuple[int, int, int] = (0, 0, 0),
) -> MetricResult:
    rgb, mask = _prepare(image, coverage_mask)
    magenta = np.all(rgb == _MAGENTA, axis=-1)
    background = _background(rgb, clear_color)
    eroded = _erode(mask, 2) if mask is not None else np.zeros(rgb.shape[:2], dtype=bool)
    cracks = magenta | (background & eroded)
    count = int(np.count_nonzero(cracks))
    return MetricResult(
        metric="Q5",
        value=count,
        threshold=Q5_THRESHOLD,
        passed=count == Q5_THRESHOLD,
        values={"crack_pixels": count, "magenta_pixels": int(np.count_nonzero(magenta))},
    )


def q6_hole_pixels(
    frames: ArrayLike | list[ArrayLike],
    coverage_mask: ArrayLike,
    *,
    clear_color: tuple[int, int, int] = (0, 0, 0),
) -> MetricResult:
    sequence = frames if isinstance(frames, list) else [frames]
    _rgb0, mask = _prepare(sequence[0], coverage_mask)
    eroded = _erode(mask, 2)
    per_frame: list[int] = []
    for frame in sequence:
        rgb, _mask = _prepare(frame, None)
        holes = _background(rgb, clear_color) & eroded
        per_frame.append(int(np.count_nonzero(holes)))
    total = int(sum(per_frame))
    return MetricResult(
        metric="Q6",
        value=total,
        threshold=Q6_THRESHOLD,
        passed=total == Q6_THRESHOLD,
        values={"hole_pixels": total, "per_frame": per_frame},
    )


def _prepare(image: ArrayLike, coverage_mask: ArrayLike | None) -> tuple[NDArray[np.uint8], NDArray[np.bool_] | None]:
    array = np.asarray(image)
    if np.issubdtype(array.dtype, np.floating):
        scaled = np.clip(np.rint(array.astype(np.float64) * 255.0), 0, 255).astype(np.uint8)
    else:
        scaled = array.astype(np.uint8)
    if scaled.ndim == 2:
        scaled = np.repeat(scaled[..., None], 3, axis=-1)
    rgb = scaled[..., :3]
    mask: NDArray[np.bool_] | None = None
    if coverage_mask is not None:
        raw = np.asarray(coverage_mask)
        if np.issubdtype(raw.dtype, np.floating):
            mask = raw > 0.5
        else:
            mask = raw > 0
        mask = np.asarray(mask, dtype=bool)
        if mask.shape[:2] != rgb.shape[:2]:
            raise ValueError("coverage mask size does not match the image")
    return rgb, mask


def _background(rgb: NDArray[np.uint8], clear_color: tuple[int, int, int]) -> NDArray[np.bool_]:
    clear = np.array(clear_color, dtype=np.uint8)
    return np.all(rgb == clear, axis=-1)


def _erode(mask: NDArray[np.bool_], radius: int) -> NDArray[np.bool_]:
    current = np.asarray(mask, dtype=bool)
    height, width = current.shape[:2]
    for _step in range(radius):
        padded = np.pad(current, 1, constant_values=False)
        kept = np.ones((height, width), dtype=bool)
        for dy in range(3):
            for dx in range(3):
                kept &= padded[dy : dy + height, dx : dx + width]
        current = kept
    return current
