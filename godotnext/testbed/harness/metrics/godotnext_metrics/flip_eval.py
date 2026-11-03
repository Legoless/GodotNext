"""LDR FLIP through flip-evaluator, with the magma map left off.

Q7, Q9 and Q19 all use this. The error map is the scalar FLIP value per pixel.
Mean and p99 use that map. p99 uses the harness percentile.
"""

from __future__ import annotations

from typing import cast

import flip_evaluator
import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.color import prepare_ldr
from godotnext_metrics.percentile import percentile


def ldr_flip(
    reference: NDArray[np.generic],
    test: NDArray[np.generic],
    *,
    exposure: float = 1.0,
    already_display: bool = False,
) -> tuple[float, float, NDArray[np.float64]]:
    ref_ldr = prepare_ldr(reference, exposure=exposure, already_display=already_display)
    test_ldr = prepare_ldr(test, exposure=exposure, already_display=already_display)
    if ref_ldr.shape != test_ldr.shape:
        raise ValueError(f"FLIP images differ in shape: {ref_ldr.shape} vs {test_ldr.shape}")
    error_map, mean_error, _parameters = flip_evaluator.evaluate(
        ref_ldr,
        test_ldr,
        "LDR",
        inputsRGB=True,
        applyMagma=False,
        computeMeanError=True,
    )
    scalar = np.asarray(error_map, dtype=np.float64).reshape(-1)
    mean = float(cast(float, mean_error))
    return mean, percentile(scalar, 99.0), scalar
