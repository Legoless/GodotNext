"""Tests for standalone #95 Q19, Q21, and runner job scripts."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from godotnext_metrics.images import write_exr, write_png

_METRICS_DIR = Path(__file__).resolve().parents[1]
_HARNESS_DIR = _METRICS_DIR.parent


def _run_script(script: Path, *args: str) -> tuple[int, dict[str, object]]:
    completed = subprocess.run(
        [sys.executable, str(script), *args],
        check=False,
        capture_output=True,
        text=True,
    )
    payload = json.loads(completed.stdout)
    assert isinstance(payload, dict), completed.stdout
    return completed.returncode, payload


def test_q21_csv_excludes_reset_frame_and_flags_other_frames(tmp_path: Path) -> None:
    csv_path = tmp_path / "q21.csv"
    csv_path.write_text(
        "frame,view,sentinel,nonfinite\n0,0,0,0\n300,0,0,0\n340,0,0,0\n400,0,128,0\n401,0,0,0\n",
        encoding="utf-8",
    )
    code, payload = _run_script(_METRICS_DIR / "q21_invalid_mv.py", str(csv_path))
    assert code == 0
    assert payload["metric"] == "Q21"
    assert payload["pass"] is True
    assert payload["value"] == 0
    values = payload["values"]
    assert isinstance(values, dict)
    assert values["reset_frames"] == {"400": 128}

    bad_csv = tmp_path / "q21_bad.csv"
    bad_csv.write_text(
        "frame,view,sentinel,nonfinite\n320,0,3,1\n400,0,64,0\n",
        encoding="utf-8",
    )
    code_bad, payload_bad = _run_script(_METRICS_DIR / "q21_invalid_mv.py", str(bad_csv))
    assert code_bad == 0
    assert payload_bad["pass"] is False
    assert payload_bad["value"] == 4


def test_q19_cli_handles_display_pngs_and_content_masks(tmp_path: Path) -> None:
    # Build synthetic 16x16 display-referred PNG captures over frames 119..125
    # with distinct non-white mid-gray patterns so uint8 clamping would fail if
    # already_display were mishandled.
    test_dir = tmp_path / "taa"
    ref_dir = tmp_path / "ssaa_msaa8"
    msaa4_dir = tmp_path / "ssaa_msaa4"
    mask_dir = tmp_path / "mask_alpha_particles"
    for directory in (test_dir, ref_dir, msaa4_dir, mask_dir):
        directory.mkdir(parents=True)

    (ref_dir / "manifest.json").write_text(
        json.dumps({
            "scaling_3d_mode": 0,
            "scaling_3d_scale": 2.0,
            "output_size": [16, 16],
        })
        + "\n",
        encoding="utf-8",
    )

    base = np.full((16, 16, 3), 96, dtype=np.uint8)
    base[:, 8:, :] = 164
    test_img = base.copy()
    test_img[4:12, 4:12, :] = 120
    mask_img = np.zeros((16, 16, 3), dtype=np.uint8)
    mask_img[4:12, 4:12, :] = 255

    for idx in range(119, 125):
        write_png(ref_dir / f"frame{idx:08d}.png", base)
        write_png(msaa4_dir / f"frame{idx:08d}.png", base)
        write_png(test_dir / f"frame{idx:08d}.png", test_img)
        write_png(mask_dir / f"frame{idx:08d}.png", mask_img)
    # Sparse EXR (only frame 4) must not shadow the 120..124 PNG sequence.
    write_exr(test_dir / "hdr_00004.exr", np.full((16, 16, 3), 0.5, dtype=np.float32))

    code, payload = _run_script(
        _METRICS_DIR / "q19_upscaler_flip.py",
        "--test",
        str(test_dir),
        "--reference",
        str(ref_dir),
        "--msaa4",
        str(msaa4_dir),
        "--mask",
        str(mask_dir),
        "--form",
        "moving",
    )
    assert code == 0
    assert payload["metric"] == "Q19"
    assert payload["pass"] is True
    val = payload["value"]
    assert isinstance(val, (int, float)) and float(val) > 0.0
    values = payload["values"]
    assert isinstance(values, dict)
    assert values["convergence_flip"] == 0.0
    per_mask = values["per_mask"]
    assert isinstance(per_mask, dict)
    assert "alpha_particles" in per_mask
    mask_score = per_mask["alpha_particles"]
    assert isinstance(mask_score, (int, float)) and float(mask_score) > 0.0


def test_upscaler_jobs_list_and_dry_run(tmp_path: Path) -> None:
    runner = _HARNESS_DIR / "runners" / "upscaler_jobs.sh"
    listed = subprocess.run(
        ["bash", str(runner), "--list"],
        check=True,
        capture_output=True,
        text=True,
    )
    rows = [line.split("\t") for line in listed.stdout.strip().splitlines()]
    # upscaler_torture has 7 jobs x 2 resolutions = 14; 3 legacy scenes have 5 jobs x 2 = 30 -> 44 total.
    assert len(rows) == 44
    dry = subprocess.run(
        [
            "bash",
            str(runner),
            "--dry-run",
            "--scene=upscaler_torture",
            "--job=copy",
            "--resolution=3840x2160",
            f"--out-root={tmp_path}",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert "GODOT_DEV_COPY_UPSCALER=1" in dry.stdout
    assert "3840x2160" in dry.stdout
    assert "--upscaler=copy" in dry.stdout
    assert "--velocity-dump" in dry.stdout
