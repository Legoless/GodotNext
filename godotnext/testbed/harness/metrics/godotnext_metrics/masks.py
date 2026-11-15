"""Content masks for Q8 and Q19 (#190).

A mask is a white-on-black image: a pixel is in the mask when its first channel is non-zero (or
above 0.5 for a float image). The harness writes a mask run (``--mask=<name>``) as single-channel
8-bit PNG frames, ``frame00000000.png`` upward, in its own folder. The runner puts that folder
beside the captures it belongs to as ``mask_<name>/``. A single image named ``cmask_<name>.png``
inside a capture folder is the same mask for every frame. The name ``frame`` selects every pixel.

The masked forms do not change a metric definition. Q19 averages the frozen FLIP error map over the
mask, frame by frame. Q8 treats each masked pixel as a patch.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.images import indexed_paths, read_png
from godotnext_metrics.q19_upscaler_flip import mask_bool

FRAME_MASK = "frame"
MIN_COVERAGE = 0.005


class MaskError(ValueError):
    """A named mask is missing or does not fit the frames."""


def mask_directory_candidates(capture: Path, name: str) -> list[Path]:
    """Where a per-frame mask run for ``capture`` can live: inside it, or beside it."""
    return [capture / f"mask_{name}", capture.parent / f"mask_{name}"]


def load_mask_frames(capture: Path, name: str, indices: list[int], shape: tuple[int, int]) -> list[NDArray[np.bool_]]:
    """One boolean mask per frame index, from ``cmask_<name>.png``, a ``mask_<name>/`` run, or ``frame``."""
    if not indices:
        raise MaskError(f"mask {name!r} needs at least one frame")
    if name == FRAME_MASK:
        everything = np.ones(shape, dtype=bool)
        return [everything for _ in indices]
    single = capture / f"cmask_{name}.png"
    if single.is_file():
        plane = _checked(mask_bool(read_png(single)), shape, single)
        return [plane for _ in indices]
    for directory in mask_directory_candidates(capture, name):
        paths = indexed_paths(directory, "frame*.png")
        if not paths:
            continue
        missing = [index for index in indices if index not in paths]
        if missing:
            raise MaskError(f"mask run {directory.name} has no frame {missing[0]}")
        return [_checked(mask_bool(read_png(paths[index])), shape, paths[index]) for index in indices]
    raise MaskError(f"cannot find mask {name!r} (cmask_{name}.png or mask_{name}/) for {capture}")


def union(frames: list[NDArray[np.bool_]]) -> NDArray[np.bool_]:
    combined = np.zeros_like(frames[0], dtype=bool)
    for plane in frames:
        combined |= plane
    return combined


def coverage(mask: NDArray[np.generic]) -> float:
    plane = mask_bool(mask)
    return float(np.count_nonzero(plane)) / float(plane.size)


def coverage_report(
    directory: Path, *, minimum: float = MIN_COVERAGE, first: int = 0, last: int | None = None
) -> dict[str, object]:
    """Per-frame coverage of a mask run folder over ``[first, last]``, against ``minimum``."""
    paths = indexed_paths(directory, "frame*.png")
    chosen = [index for index in sorted(paths) if index >= first and (last is None or index <= last)]
    if not chosen:
        raise MaskError(f"{directory} has no mask frames in the requested range")
    values = [coverage(read_png(paths[index])) for index in chosen]
    shape = read_png(paths[chosen[0]]).shape
    worst = int(np.argmin(np.asarray(values)))
    return {
        "mask": directory.name.removeprefix("mask_"),
        "frames": len(chosen),
        "first": chosen[0],
        "last": chosen[-1],
        "output_resolution": [int(shape[1]), int(shape[0])],
        "min": values[worst],
        "min_frame": chosen[worst],
        "mean": float(np.mean(np.asarray(values))),
        "max": float(np.max(np.asarray(values))),
        "minimum": minimum,
        "pass": bool(values[worst] >= minimum),
    }


def _checked(plane: NDArray[np.bool_], shape: tuple[int, int], path: Path) -> NDArray[np.bool_]:
    if plane.shape != shape:
        raise MaskError(f"{path.name} is {plane.shape[1]}x{plane.shape[0]}, the frames are {shape[1]}x{shape[0]}")
    return plane


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.masks")
    parser.add_argument("command", choices=("coverage",))
    parser.add_argument("--in", dest="in_dir", required=True, help="a mask_<name>/ run folder")
    parser.add_argument("--first", type=int, default=0)
    parser.add_argument("--last", type=int, default=None)
    parser.add_argument("--minimum", type=float, default=MIN_COVERAGE)
    args = parser.parse_args(argv)
    try:
        report = coverage_report(Path(args.in_dir), minimum=args.minimum, first=args.first, last=args.last)
    except (OSError, ValueError) as error:
        json.dump({"error": str(error), "pass": False}, sys.stdout, sort_keys=True)
        sys.stdout.write("\n")
        return 2
    json.dump(report, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
