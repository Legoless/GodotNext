"""Read and write the harness image files.

EXR frames keep the channel dtype OpenEXR returns, so a half-float velocity
sentinel is compared in half precision. PNG frames are 8-bit.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import OpenEXR
from numpy.typing import NDArray

from godotnext_metrics.pngutil import decode_png, encode_png

_RGB = ("R", "G", "B", "A")


def write_exr(path: Path, image: NDArray[np.floating]) -> None:
    array = np.asarray(image)
    if array.ndim == 2:
        planes = {"R": np.ascontiguousarray(array), "G": np.zeros_like(array), "B": np.zeros_like(array)}
    else:
        planes = {name: np.ascontiguousarray(array[..., index]) for index, name in enumerate(_RGB[: array.shape[-1]])}
    header = {"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage}
    path.parent.mkdir(parents=True, exist_ok=True)
    with OpenEXR.File(header, planes) as outfile:
        outfile.write(str(path))


def read_exr(path: Path) -> NDArray[np.floating]:
    with OpenEXR.File(str(path), separate_channels=True) as infile:
        channels = infile.channels()
        names = [name for name in _RGB if name in channels]
        if not names:
            raise ValueError(f"{path} has no R, G, B or A channel")
        planes = [np.array(channels[name].pixels, copy=True) for name in names]
    stacked = np.stack(planes, axis=-1)
    return stacked


def write_png(path: Path, image: NDArray[np.uint8]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encode_png(np.asarray(image, dtype=np.uint8)))


def read_png(path: Path) -> NDArray[np.uint8]:
    return decode_png(path.read_bytes())


def frame_index(path: Path) -> int | None:
    """Index encoded in ``hdr_00000.exr``, ``frame00000000.png``, ``mask_00000.png`` or ``velocity_00000.exr``."""
    stem = path.stem
    for prefix in ("hdr_", "frame", "mask_", "velocity_", "cmask_"):
        if stem.startswith(prefix) and stem[len(prefix) :].isdigit():
            return int(stem[len(prefix) :])
    if stem.isdigit():
        return int(stem)
    return None


def indexed_paths(directory: Path, pattern: str) -> dict[int, Path]:
    found: dict[int, Path] = {}
    if not directory.is_dir():
        return found
    for path in sorted(directory.glob(pattern)):
        index = frame_index(path)
        if index is not None:
            found[index] = path
    return found
