"""Upscaler Torture's scrolling emissive check (#271) on synthetic captures."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from godotnext_metrics.images import write_exr, write_png
from godotnext_metrics.scrolling_emissive import LIT_FLOOR, check, main

_SIZE = (12, 32)
_STRIP_ROWS = slice(3, 9)
_FRAMES = (0, 1, 2, 3)


def _frame(offset: int, brightness: float) -> np.ndarray:
    # A gray background and, inside the strip, stripes 8 px wide every 32 px: bright, dark, bright,
    # dark, like the generator's stripe texture, shifted by ``offset`` pixels.
    image = np.full((*_SIZE, 3), 0.2, dtype=np.float32)
    columns = (np.arange(_SIZE[1]) + offset) % 32 // 8
    stripes = np.where(columns % 2 == 0, 0.6 * brightness, 0.002)
    image[_STRIP_ROWS, :, :] = stripes[None, :, None]
    return image


def _capture(directory: Path, offsets: list[int], brightness: float = 3.0) -> Path:
    for index, offset in zip(_FRAMES, offsets, strict=True):
        write_exr(directory / f"hdr_{index:05d}.exr", _frame(offset, brightness))
    return directory


def _mask_run(root: Path) -> None:
    mask = np.zeros(_SIZE, dtype=np.uint8)
    mask[_STRIP_ROWS, :] = 255
    for index in _FRAMES:
        write_png(root / "mask_scrolling_emissive" / f"frame{index:08d}.png", mask)


def _captures(tmp_path: Path, moving: list[int], frozen: list[int], brightness: float = 3.0) -> tuple[Path, Path]:
    _mask_run(tmp_path)
    return (
        _capture(tmp_path / "default", moving, brightness),
        _capture(tmp_path / "static", frozen, brightness),
    )


def test_lit_scrolling_strip_passes(tmp_path: Path) -> None:
    default, static = _captures(tmp_path, [0, 9, 18, 27], [0, 0, 0, 0])
    report = check(default, static)
    assert report["lit"] is True and report["scrolls"] is True and report["still"] is True
    assert report["pass"] is True
    assert report["mask_pixels"] == 6 * 32
    peak = report["peak_luminance"]
    assert isinstance(peak, float) and peak > 1.0
    assert main(["--default", str(default), "--static", str(static)]) == 0


def test_black_strip_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    # The bug: an unshaded material ignores EMISSION, so the strip rendered black.
    default, static = _captures(tmp_path, [0, 9, 18, 27], [0, 0, 0, 0], brightness=0.0)
    assert main(["--default", str(default), "--static", str(static)]) == 1
    report = json.loads(capsys.readouterr().out)
    assert report["lit"] is False and report["scrolls"] is False and report["pass"] is False


def test_strip_at_one_times_brightness_is_not_lit(tmp_path: Path) -> None:
    default, static = _captures(tmp_path, [0, 9, 18, 27], [0, 0, 0, 0], brightness=1.0)
    report = check(default, static)
    means = report["mean_luminance"]
    assert isinstance(means, dict)
    assert all(value < LIT_FLOOR for value in means["default"].values())
    assert report["lit"] is False and report["pass"] is False


def test_whole_periods_keep_the_mean_but_not_the_pixels(tmp_path: Path) -> None:
    # The strip holds whole stripe periods, so the mean stays put while every pixel changes.
    default, static = _captures(tmp_path, [0, 8, 16, 24], [0, 0, 0, 0])
    report = check(default, static)
    assert report["mean_change"] == pytest.approx(0.0, abs=1e-6)
    changes = report["pixel_change"]
    assert isinstance(changes, list) and min(changes) > 0.5
    assert report["scrolls"] is True


def test_frozen_default_timeline_does_not_scroll(tmp_path: Path) -> None:
    default, static = _captures(tmp_path, [0, 0, 32, 32], [0, 0, 0, 0])
    report = check(default, static)
    assert report["scrolls"] is False and report["pass"] is False


def test_moving_static_variant_fails(tmp_path: Path) -> None:
    default, static = _captures(tmp_path, [0, 9, 18, 27], [0, 0, 1, 0])
    report = check(default, static)
    assert report["still"] is False and report["pass"] is False


def test_missing_inputs_exit_2(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    default = _capture(tmp_path / "default", [0, 9, 18, 27])
    static = _capture(tmp_path / "static", [0, 0, 0, 0])
    assert main(["--default", str(default), "--static", str(static)]) == 2
    assert "cannot find mask 'scrolling_emissive'" in json.loads(capsys.readouterr().out)["error"]
    _mask_run(tmp_path)
    for path in static.glob("hdr_*.exr"):
        path.unlink()
    assert main(["--default", str(default), "--static", str(static)]) == 2
    assert "at least two hdr_*.exr frames" in json.loads(capsys.readouterr().out)["error"]
