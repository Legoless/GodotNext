"""Q21 invalid motion vectors.

A texel counts when its stored R and G are bit-equal to -1.0 in that channel's
dtype, or when either component is non-finite. The value -1.0001 is not the
sentinel in float32. The gate is 0 on every frame except a scripted reset frame,
which is reported and does not fail the gate.
"""

from __future__ import annotations

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.result import MetricResult


def q21_invalid_count(velocity_rg: NDArray[np.generic]) -> int:
    array = np.asarray(velocity_rg)
    if array.ndim != 3 or array.shape[-1] < 2:
        raise ValueError("velocity must have shape HxWx2 or more")
    red = array[..., 0]
    green = array[..., 1]
    sentinel = _bit_equal(red, -1.0) & _bit_equal(green, -1.0)
    nonfinite = ~np.isfinite(red) | ~np.isfinite(green)
    return int(np.count_nonzero(sentinel | nonfinite))


def q21_invalid_motion_vectors(
    frames: dict[int, NDArray[np.generic]],
    *,
    reset_frames: list[int] | None = None,
) -> MetricResult:
    resets = set(reset_frames or [])
    per_frame = {int(index): q21_invalid_count(image) for index, image in sorted(frames.items())}
    gated = [count for index, count in per_frame.items() if index not in resets]
    reported = {index: per_frame[index] for index in sorted(resets) if index in per_frame}
    total = int(sum(gated))
    return MetricResult(
        metric="Q21",
        value=total,
        threshold=0,
        passed=total == 0,
        values={"invalid_texels": total, "per_frame": per_frame, "reset_frames": reported},
    )


def _bit_equal(component: NDArray[np.generic], number: float) -> NDArray[np.bool_]:
    # A channel slice from an EXR stack is not contiguous. The integer view needs a copy.
    values = np.ascontiguousarray(component)
    target = np.array(number, dtype=values.dtype)
    if values.dtype == np.float16:
        same = values.view(np.uint16) == target.view(np.uint16)
    elif values.dtype == np.float32:
        same = values.view(np.uint32) == target.view(np.uint32)
    elif values.dtype == np.float64:
        same = values.view(np.uint64) == np.array(number, dtype=np.float64).view(np.uint64)
    else:
        same = values == target
    return np.asarray(same, dtype=bool)
