"""The GT-2 comparison report (#190) on a tiny upscaler_jobs.sh tree."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from godotnext_metrics.images import write_png

_METRICS_DIR = Path(__file__).resolve().parents[1]


def _frames(directory: Path, offset: int, frames: range, *, flicker: bool = False) -> None:
    yy, xx = np.mgrid[0:16, 0:16]
    for frame in frames:
        image = np.full((16, 16, 3), 80, dtype=np.uint8)
        image[(xx + yy) % 5 == 0] = 200
        image[2:6, 2:14, :] = np.uint8(120 + offset + (frame % 2 if flicker else 0) * 6)
        write_png(directory / f"frame{frame:08d}.png", image)


def _mask_run(directory: Path, frames: range, name: str) -> None:
    for frame in frames:
        mask = np.zeros((16, 16), dtype=np.uint8)
        mask[2:6, 2:14] = 255
        write_png(directory / f"frame{frame:08d}.png", mask)
    manifest = {"command_line": {"user": [f"--mask={name}"]}}
    (directory / "manifest.json").write_text(json.dumps(manifest) + "\n", encoding="utf-8")


def _scene(root: Path, scene: str, jobs: dict[str, int], masks: list[str]) -> Path:
    scene_dir = root / "16x16" / scene
    window = range(120, 184)
    _frames(scene_dir / "ssaa_msaa8", 0, window)
    _frames(scene_dir / "ssaa_msaa4", 0, window)
    (scene_dir / "ssaa_msaa8" / "manifest.json").write_text(
        json.dumps({"scaling_3d_mode": 0, "scaling_3d_scale": 2.0, "output_size": [16, 16]}) + "\n", encoding="utf-8"
    )
    for job, offset in jobs.items():
        _frames(scene_dir / job, offset, window, flicker=job == "fsr2")
    for name in masks:
        _mask_run(scene_dir / f"mask_{name}", window, name)
    return scene_dir


def test_upscaler_compare_report_masks_and_report_only_dlss(tmp_path: Path) -> None:
    root = tmp_path / "jobs"
    static_dir = _scene(
        root, "upscaler_torture_static", {"taa": 0, "fsr2": 10, "dlss_quality": 4, "dlss_performance": 6}, ["moire"]
    )
    (static_dir / "dlss_performance" / "manifest.json").write_text(
        json.dumps({"dlss": {"preset": "K", "dll_sha256": ""}}) + "\n", encoding="utf-8"
    )
    _scene(root, "upscaler_torture", {"taa": 3}, ["thin_wire"])
    out = tmp_path / "report"
    env = dict(os.environ)
    env["PYTHONPATH"] = str(_METRICS_DIR) + os.pathsep + env.get("PYTHONPATH", "")
    completed = subprocess.run(
        [sys.executable, str(_METRICS_DIR / "upscaler_compare.py"), "--in", str(root), "--out", str(out)],
        check=False,
        capture_output=True,
        text=True,
        cwd=_METRICS_DIR,
        env=env,
    )
    assert completed.returncode == 0, completed.stderr
    report = json.loads((out / "upscaler_compare.json").read_text(encoding="utf-8"))
    assert report == json.loads(completed.stdout)
    assert [config["key"] for config in report["configs"]][:3] == ["taa_native", "fsr2_quality", "fsr2_performance"]
    assert {config["key"] for config in report["configs"] if config["report_only"]} == {
        "dlss_dlaa",
        "dlss_quality",
        "dlss_performance",
    }
    scenes = report["resolutions"]["16x16"]["scenes"]
    static = scenes["upscaler_torture_static"]
    assert static["taa_native"]["q19"] == 0.0
    assert static["taa_native"]["q8"] == pytest.approx(0.0, abs=1e-12)
    assert static["taa_native"]["q8_moire"] == pytest.approx(0.0, abs=1e-12)
    assert static["fsr2_performance"]["q19"] > 0.0
    assert static["fsr2_performance"]["q8_moire"] > static["fsr2_performance"]["q8"] > 0.0
    # The moiré region flickers as a whole, so it shows as one patch too; the static frame does not.
    assert static["fsr2_performance"]["q8_moire_as_patch"] > 0.0
    assert static["taa_native"]["q8_as_patch"] == pytest.approx(0.0, abs=1e-12)
    assert static["fsr2_quality"]["q19"] is None
    assert static["fsr2_quality"]["missing"] == ["no fsr2_quality capture"]
    # A DLSS folder without the DLSS record stays empty; one with it is reported.
    assert static["dlss_quality"]["q19"] is None
    assert "no DLSS-enabled capture" in static["dlss_quality"]["missing"][0]
    assert static["dlss_performance"]["q19"] is not None
    torture = scenes["upscaler_torture"]["taa_native"]
    assert torture["q19"] > 0.0
    assert torture["q19_thin_wire"] > torture["q19"]
    assert torture["q8"] is None
    assert "Q10: no disocclusion.png" in torture["missing"]
    cross = report["resolutions"]["16x16"]["cross_scene"]
    assert cross["taa_native"]["scenes"] == 2
    markdown = (out / "upscaler_compare.md").read_text(encoding="utf-8")
    assert "| upscaler_torture_static | TSR | High | 0.667 |" in markdown
    missing = subprocess.run(
        [sys.executable, str(_METRICS_DIR / "upscaler_compare.py"), "--in", str(tmp_path / "absent")],
        check=False,
        capture_output=True,
        text=True,
        cwd=_METRICS_DIR,
        env=env,
    )
    assert missing.returncode == 2
