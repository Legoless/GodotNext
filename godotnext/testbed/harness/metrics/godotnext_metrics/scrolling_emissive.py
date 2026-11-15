"""Upscaler Torture's scrolling emissive strip (#271): is it lit, does it scroll, does it hold still?

The strip (``ScrollingEmissive`` in ``generators/upscaler_torture.gd``) is unshaded and writes its
stripes to ALBEDO at x3. On the default timeline its texture scrolls 1/30 UV per frame, and the
stripe period is 1/8 UV, so the strip looks the same every 15 frames; ``--variant=static`` stops it.
The check reads two harness captures from the static camera, which frames 0-199 of the default
timeline and the whole static variant use:

- ``--default``: the default timeline, with ``--capture-frames`` on consecutive frames (0,1,2,3);
- ``--static``: the same frames with ``--variant=static``;

and the ``scrolling_emissive`` mask run of those frames, found as for Q8 and Q19: ``cmask_`` or
``mask_scrolling_emissive/`` inside a capture, or ``mask_scrolling_emissive/`` beside it.

Inside the mask, on the linear ``hdr_*.exr`` frames (a PNG frame clips the x3 stripes at 1.0):

- lit: every frame's mean luminance is at least ``LIT_FLOOR``. At x1 the strip's mean is about 0.3,
  at x3 about 0.9.
- scrolls: between each pair of adjacent captured frames of the default timeline, the mean absolute
  per-pixel luminance change is at least ``SCROLL_FLOOR``. The strip spans eight whole stripe
  periods, so its mean luminance is almost the same at any scroll offset; that change is reported
  as ``mean_change`` and is not the test.
- still: the static variant's frames are identical inside the mask, so their mean is constant.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.color import luminance
from godotnext_metrics.images import indexed_paths, read_exr
from godotnext_metrics.masks import load_mask_frames, union

MASK = "scrolling_emissive"
LIT_FLOOR = 0.6
SCROLL_FLOOR = 0.1


class EmissiveInputError(ValueError):
    """A capture or its mask run is missing frames."""


def _luminance_frames(capture: Path) -> dict[int, NDArray[np.float64]]:
    paths = indexed_paths(capture, "hdr_*.exr")
    if len(paths) < 2:
        raise EmissiveInputError(f"{capture} needs at least two hdr_*.exr frames (--capture-frames)")
    return {index: luminance(read_exr(path)[..., :3].astype(np.float64)) for index, path in sorted(paths.items())}


def _masks(capture: Path, indices: list[int], shape: tuple[int, int]) -> dict[int, NDArray[np.bool_]]:
    planes = load_mask_frames(capture, MASK, indices, shape)
    for index, plane in zip(indices, planes, strict=True):
        if not plane.any():
            raise EmissiveInputError(f"the {MASK} mask selects no pixel on frame {index} of {capture}")
    return dict(zip(indices, planes, strict=True))


def check(default_dir: Path, static_dir: Path) -> dict[str, object]:
    moving = _luminance_frames(default_dir)
    still = _luminance_frames(static_dir)
    first = next(iter(moving.values()))
    shape = (int(first.shape[0]), int(first.shape[1]))
    moving_masks = _masks(default_dir, list(moving), shape)
    still_masks = _masks(static_dir, list(still), shape)

    moving_means = {index: float(plane[moving_masks[index]].mean()) for index, plane in moving.items()}
    still_means = {index: float(plane[still_masks[index]].mean()) for index, plane in still.items()}
    lit = min(min(moving_means.values()), min(still_means.values())) >= LIT_FLOOR

    order = sorted(moving)
    changes: list[float] = []
    mean_changes: list[float] = []
    for before, after in zip(order, order[1:], strict=False):
        both = moving_masks[before] & moving_masks[after]
        changes.append(float(np.abs(moving[after][both] - moving[before][both]).mean()))
        mean_changes.append(abs(moving_means[after] - moving_means[before]))
    scrolls = min(changes) >= SCROLL_FLOOR

    held = union(list(still_masks.values()))
    reference = still[min(still)][held]
    holds_still = all(np.array_equal(plane[held], reference) for plane in still.values())

    return {
        "check": "Upscaler Torture scrolling emissive",
        "mask": MASK,
        "mask_pixels": int(np.count_nonzero(union(list(moving_masks.values())))),
        "mean_luminance": {
            "default": {str(index): value for index, value in moving_means.items()},
            "static": {str(index): value for index, value in still_means.items()},
        },
        "peak_luminance": max(float(plane[moving_masks[index]].max()) for index, plane in moving.items()),
        "lit_floor": LIT_FLOOR,
        "lit": bool(lit),
        "pixel_change": changes,
        "mean_change": max(mean_changes),
        "scroll_floor": SCROLL_FLOOR,
        "scrolls": bool(scrolls),
        "still": bool(holds_still),
        "pass": bool(lit and scrolls and holds_still),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.scrolling_emissive")
    parser.add_argument("--default", dest="default_dir", required=True, help="default-timeline capture")
    parser.add_argument("--static", dest="static_dir", required=True, help="--variant=static capture")
    args = parser.parse_args(argv)
    try:
        report = check(Path(args.default_dir), Path(args.static_dir))
    except (OSError, ValueError) as error:
        json.dump({"check": "Upscaler Torture scrolling emissive", "pass": False, "error": str(error)}, sys.stdout)
        sys.stdout.write("\n")
        return 2
    json.dump(report, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
