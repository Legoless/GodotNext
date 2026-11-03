"""Linear RGB helpers shared by the image metrics.

Luminance is Rec. 709 (0.2126, 0.7152, 0.0722) on scene-linear RGB. The linear
tonemapper is ``clip(rgb * exposure, 0, 1)`` with exposure 1 unless the capture
says otherwise. LDR FLIP then sees the sRGB encoding of that result.
"""

from __future__ import annotations

import numpy as np
from numpy.typing import NDArray

# Rec. 709 luminance on linear RGB.
_LUMA_R = 0.2126
_LUMA_G = 0.7152
_LUMA_B = 0.0722


def luminance(rgb: NDArray[np.floating]) -> NDArray[np.float64]:
    image = np.asarray(rgb, dtype=np.float64)
    if image.shape[-1] < 3:
        raise ValueError("luminance needs RGB")
    return image[..., 0] * _LUMA_R + image[..., 1] * _LUMA_G + image[..., 2] * _LUMA_B


def linear_tonemap(rgb: NDArray[np.floating], exposure: float = 1.0) -> NDArray[np.float64]:
    return np.clip(np.asarray(rgb, dtype=np.float64) * exposure, 0.0, 1.0)


def linear_to_srgb(linear: NDArray[np.floating]) -> NDArray[np.float32]:
    x = np.clip(np.asarray(linear, dtype=np.float64), 0.0, 1.0)
    encoded = np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)
    return np.clip(encoded, 0.0, 1.0).astype(np.float32)


def prepare_ldr(image: NDArray[np.generic], *, exposure: float, already_display: bool) -> NDArray[np.float32]:
    """Return an sRGB image in [0, 1] for LDR FLIP (``inputsRGB=True``)."""
    array = np.asarray(image)
    if already_display:
        if np.issubdtype(array.dtype, np.integer):
            encoded = array.astype(np.float32) / float(np.iinfo(array.dtype).max)
        else:
            encoded = np.clip(array.astype(np.float32), 0.0, 1.0)
        if encoded.ndim == 2:
            encoded = np.repeat(encoded[..., None], 3, axis=-1)
        return encoded[..., :3].astype(np.float32)
    linear = linear_tonemap(array.astype(np.float64), exposure)
    if linear.ndim == 2:
        linear = np.repeat(linear[..., None], 3, axis=-1)
    return linear_to_srgb(linear[..., :3])
