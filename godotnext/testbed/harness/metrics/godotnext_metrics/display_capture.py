"""Displayed-UI capture and the GU+-6 UI comparison (#202).

Frame generation composites the UI into generated frames after the renderer, so only the displayed
image shows whether the UI survived it. ``capture`` records 240 displayed frames of the static-UI
scene (``scenes/static_ui``) running borderless fullscreen, through Desktop Duplication with the
``dxcam`` pin in ``requirements-fg.txt`` (Windows only; it must run in the interactive desktop
session, not an SSH login). Each new frame is cropped to the UI mask the scene dumps
(``cmask_ui.png``: the pixels where the HUD's alpha is at least 0.99) and kept as those pixels
only, in ``ui_pixels.npz``. An installed ``dxcam`` other than the pin is refused. When ``capture``
starts the game itself, the game has to be running after the last grab, or the frames could show
whatever replaced it on the desktop: a game that exited first fails the capture with its exit code.

``compare`` is GU+-6's first line: the UI pixels of an FG-on capture against an FG-off capture,
frame i against frame i. Each frame pair is rebuilt on the mask's bounding box with everything
outside the mask zero in both images, so the moving 3D image behind thin text cannot reach a UI
pixel through FLIP's spatial filters. LDR FLIP (Q7's call, ``flip_eval.py``) is averaged over the
mask pixels, then over the frames; the result passes at ≤ 0.005. A folder of ``frame*.png`` (a
harness capture) is read the same way, so two FG-off harness captures compare with this tool too.
"""

from __future__ import annotations

import argparse
import importlib.metadata
import json
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.flip_eval import ldr_flip
from godotnext_metrics.images import indexed_paths, read_png
from godotnext_metrics.percentile import percentile
from godotnext_metrics.q19_upscaler_flip import mask_bool

FRAMES = 240
FLIP_MAX = 0.005
CAPTURE_FILE = "ui_pixels.npz"
MASK_FILE = "cmask_ui.png"
DXCAM_VERSION = "0.3.0"

Grab = Callable[[], "NDArray[np.uint8] | None"]


class CaptureError(ValueError):
    """The capture or its comparison cannot be made."""


def load_mask(path: Path) -> NDArray[np.bool_]:
    mask = mask_bool(read_png(path))
    if not mask.any():
        raise CaptureError(f"{path.name} selects no pixel")
    return mask


def capture_frames(
    grab: Grab,
    mask: NDArray[np.bool_],
    *,
    frames: int = FRAMES,
    timeout_s: float = 120.0,
    clock: Callable[[], float] = time.perf_counter,
    channel_order: tuple[int, int, int] = (2, 1, 0),
) -> tuple[NDArray[np.uint8], NDArray[np.float64], int]:
    """The UI pixels of ``frames`` new frames from ``grab``, with their times and the number of grabs.

    ``grab`` returns an H×W×C uint8 frame, or None when nothing new was presented since the last
    call (dxcam's ``grab()``). ``channel_order`` picks R, G and B from it: BGRA by default.
    """
    flat_index = np.flatnonzero(mask)
    pixels = np.empty((frames, flat_index.size, 3), dtype=np.uint8)
    stamps = np.empty(frames, dtype=np.float64)
    order = list(channel_order)
    deadline = clock() + timeout_s
    got = 0
    grabs = 0
    while got < frames:
        if clock() > deadline:
            raise CaptureError(f"only {got} of {frames} new frames within {timeout_s:g} s")
        frame = grab()
        grabs += 1
        if frame is None:
            continue
        if frame.shape[:2] != mask.shape:
            raise CaptureError(
                f"the display is {frame.shape[1]}x{frame.shape[0]}, the UI mask {mask.shape[1]}x{mask.shape[0]}"
            )
        pixels[got] = frame.reshape(-1, frame.shape[2])[flat_index][:, order]
        stamps[got] = clock()
        got += 1
    return pixels, stamps, grabs


def save_capture(
    out_dir: Path, mask: NDArray[np.bool_], pixels: NDArray[np.uint8], stamps: NDArray[np.float64]
) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / CAPTURE_FILE
    np.savez_compressed(path, mask=mask, pixels=pixels, timestamps=stamps)
    return path


def load_capture(path: Path, mask_path: Path | None = None) -> tuple[NDArray[np.bool_], NDArray[np.uint8]]:
    """The mask and the per-frame UI pixels of ``ui_pixels.npz``, or of a folder of ``frame*.png``."""
    archive = path / CAPTURE_FILE if path.is_dir() else path
    if archive.is_file() and archive.suffix == ".npz":
        with np.load(archive) as data:
            mask = np.asarray(data["mask"], dtype=bool)
            pixels = np.asarray(data["pixels"], dtype=np.uint8)
        if mask_path is not None and not np.array_equal(load_mask(mask_path), mask):
            raise CaptureError(f"{archive.name} was captured with a different UI mask")
        return mask, pixels
    frames = indexed_paths(path, "frame*.png")
    if not frames:
        raise CaptureError(f"{path} has no {CAPTURE_FILE} and no frame*.png")
    mask_file = mask_path if mask_path is not None else path / MASK_FILE
    if not mask_file.is_file():
        raise CaptureError(f"{path} needs a UI mask: {MASK_FILE} or --mask")
    mask = load_mask(mask_file)
    flat_index = np.flatnonzero(mask)
    rows = []
    for index in sorted(frames):
        image = read_png(frames[index])
        if image.shape[:2] != mask.shape:
            raise CaptureError(f"{frames[index].name} is {image.shape[1]}x{image.shape[0]}, the mask is not")
        channels = image.reshape(-1, image.shape[2] if image.ndim == 3 else 1)
        rgb = channels[:, :3] if channels.shape[1] >= 3 else np.repeat(channels, 3, axis=1)
        rows.append(rgb[flat_index])
    return mask, np.stack(rows).astype(np.uint8)


def _bounding_box(mask: NDArray[np.bool_]) -> tuple[slice, slice]:
    ys, xs = np.nonzero(mask)
    return slice(int(ys.min()), int(ys.max()) + 1), slice(int(xs.min()), int(xs.max()) + 1)


def ui_flip(mask: NDArray[np.bool_], test: NDArray[np.uint8], reference: NDArray[np.uint8]) -> list[float]:
    """Mean LDR FLIP over the UI pixels of each frame pair, on the mask's box, zero outside the mask."""
    rows, cols = _bounding_box(mask)
    inner = mask[rows, cols]
    values: list[float] = []
    for test_pixels, ref_pixels in zip(test, reference, strict=True):
        test_image = np.zeros((*inner.shape, 3), dtype=np.uint8)
        ref_image = np.zeros((*inner.shape, 3), dtype=np.uint8)
        test_image[inner] = test_pixels
        ref_image[inner] = ref_pixels
        _mean, _p99, error = ldr_flip(ref_image, test_image, already_display=True)
        values.append(float(error.reshape(inner.shape)[inner].mean()))
    return values


def compare(
    test_path: Path, ref_path: Path, *, mask_path: Path | None = None, frames: int = FRAMES, threshold: float = FLIP_MAX
) -> dict[str, object]:
    test_mask, test = load_capture(test_path, mask_path)
    ref_mask, reference = load_capture(ref_path, mask_path)
    if not np.array_equal(test_mask, ref_mask):
        raise CaptureError("the two captures use different UI masks")
    if test.shape[0] < frames or reference.shape[0] < frames:
        raise CaptureError(f"need {frames} frames in each capture, found {test.shape[0]} and {reference.shape[0]}")
    values = ui_flip(test_mask, test[:frames], reference[:frames])
    mean = float(np.mean(values))
    return {
        "metric": "GU+-6 UI FLIP (provisional)",
        "frames": frames,
        "mask_pixels": int(np.count_nonzero(test_mask)),
        "output_resolution": [int(test_mask.shape[1]), int(test_mask.shape[0])],
        "value": mean,
        "p99": percentile(values, 99.0),
        "max": float(np.max(values)),
        "max_frame": int(np.argmax(values)),
        "threshold": threshold,
        "pass": bool(mean <= threshold),
    }


def dxcam_version(version_of: Callable[[str], str] = importlib.metadata.version) -> str:
    """The installed dxcam version; anything but the pin is refused."""
    try:
        version = version_of("dxcam")
    except importlib.metadata.PackageNotFoundError as error:
        raise CaptureError(
            f"dxcam is not installed; pip install -r requirements-fg.txt (dxcam=={DXCAM_VERSION})"
        ) from error
    if version != DXCAM_VERSION:
        raise CaptureError(f"dxcam {version} is installed, the pin is {DXCAM_VERSION} (requirements-fg.txt)")
    return version


def _dxcam_grab(device_idx: int, output_idx: int) -> tuple[Grab, Callable[[], None]]:
    try:
        import dxcam  # type: ignore[import-not-found]  # Windows only, from requirements-fg.txt.

        camera: Any = dxcam.create(device_idx=device_idx, output_idx=output_idx, output_color="BGRA")
    except Exception as error:
        # dxcam enumerates the outputs on import; outside the interactive desktop (an SSH login) that
        # raises a COMError, which is neither an OSError nor a ValueError.
        raise CaptureError(
            f"dxcam cannot open the display ({error}); run the capture in the interactive desktop session"
        ) from error
    if camera is None:
        raise CaptureError(f"dxcam has no output {output_idx} on device {device_idx}")

    def grab() -> NDArray[np.uint8] | None:
        frame = camera.grab()
        return None if frame is None else np.asarray(frame, dtype=np.uint8)

    return grab, camera.release


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.display_capture")
    sub = parser.add_subparsers(dest="command", required=True)
    grab = sub.add_parser("capture", help="240 displayed frames, cropped to the UI mask (Windows, dxcam)")
    grab.add_argument("--mask", type=Path, required=True, help="the run's cmask_ui.png")
    grab.add_argument("--out", type=Path, required=True)
    grab.add_argument("--frames", type=int, default=FRAMES)
    grab.add_argument("--device-idx", type=int, default=0)
    grab.add_argument("--output-idx", type=int, default=0)
    grab.add_argument("--timeout", type=float, default=120.0)
    grab.add_argument("--delay", type=float, default=0.0, help="seconds between the launch and the first grab")
    grab.add_argument("launch", nargs=argparse.REMAINDER, help="optional: -- <launch command> to start first")
    check = sub.add_parser("compare", help="GU+-6: UI pixels, FG on against FG off")
    check.add_argument("--test", type=Path, required=True)
    check.add_argument("--ref", type=Path, required=True)
    check.add_argument("--mask", type=Path, default=None, help="for frame*.png folders without cmask_ui.png")
    check.add_argument("--frames", type=int, default=FRAMES)
    args = parser.parse_args(argv)
    try:
        if args.command == "compare":
            report = compare(args.test, args.ref, mask_path=args.mask, frames=args.frames)
        else:
            mask = load_mask(args.mask)
            version = dxcam_version()
            launch = args.launch[1:] if args.launch[:1] == ["--"] else args.launch
            process = subprocess.Popen(launch) if launch else None
            try:
                time.sleep(args.delay)
                grabber, release = _dxcam_grab(args.device_idx, args.output_idx)
                try:
                    pixels, stamps, grabs = capture_frames(grabber, mask, frames=args.frames, timeout_s=args.timeout)
                finally:
                    release()
                if process is not None and (exit_code := process.poll()) is not None:
                    raise CaptureError(f"the launched game exited with {exit_code} before the last grab")
            finally:
                if process is not None and process.poll() is None:
                    process.terminate()
                    process.wait()
            path = save_capture(args.out, mask, pixels, stamps)
            intervals = np.diff(stamps) * 1000.0
            report = {
                "metric": "displayed-UI capture",
                "dxcam": version,
                "file": path.name,
                "frames": int(pixels.shape[0]),
                "grabs": grabs,
                "mask_pixels": int(pixels.shape[1]),
                "grab_interval_ms_p50": percentile(intervals, 50.0),
                # Launched here: still running at the last grab, then stopped. None: started elsewhere.
                "launch": None
                if process is None
                else {"running_at_last_grab": True, "exit_after_stop": process.returncode},
                "pass": True,
            }
    except (OSError, ValueError, ImportError) as error:
        json.dump({"metric": "GU+-6 UI FLIP (provisional)", "pass": False, "error": str(error)}, sys.stdout)
        sys.stdout.write("\n")
        return 2
    json.dump(report, sys.stdout, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
