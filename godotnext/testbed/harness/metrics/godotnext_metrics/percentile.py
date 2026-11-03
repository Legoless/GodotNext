"""Percentiles that match the harness writer in ``manifest.gd``.

Linear interpolation. Rank is ``(percent / 100) * (n - 1)``. An empty sample is
0. A single sample is that sample. This is the function ``summary.json`` uses,
so Q11 and Q12 can be checked against it within 0.01 ms.
"""

from __future__ import annotations

import math

import numpy as np
from numpy.typing import ArrayLike, NDArray


def percentile(values: ArrayLike, percent: float) -> float:
    data = np.asarray(values, dtype=np.float64).reshape(-1)
    count = int(data.size)
    if count <= 0:
        return 0.0
    if count == 1:
        return float(data[0])
    ordered: NDArray[np.float64] = np.sort(data)
    rank = (percent / 100.0) * float(count - 1)
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    low = max(0, low)
    high = min(count - 1, high)
    if low == high:
        return float(ordered[low])
    fraction = rank - float(low)
    return float(ordered[low]) * (1.0 - fraction) + float(ordered[high]) * fraction
