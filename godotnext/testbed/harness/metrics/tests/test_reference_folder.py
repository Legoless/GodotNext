"""Q1 reads a path-traced reference folder as `--reference=pt` writes it (#263)."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from godotnext_metrics.images import write_exr, write_png

# Two Q1 patches (ids 7 and 9) on a 4x3 view. The harness writes the mask as RGB8 with
# R + 256 * G = patch id + 1, the half-float full and direct-only means at the held frame,
# the camera's patch list and reference.json.
_IDS = [7, 9]
_MASK = np.array([[8, 8, 0, 10], [8, 8, 0, 10], [0, 0, 0, 10]], dtype=np.int64)


def _run(*args: str) -> tuple[int, dict[str, object]]:
    command = [sys.executable, "-m", "godotnext_metrics", *args, "--json"]
    env = dict(os.environ)
    root = str(Path(__file__).resolve().parents[1])
    env["PYTHONPATH"] = root + os.pathsep + env.get("PYTHONPATH", "")
    completed = subprocess.run(command, check=False, capture_output=True, text=True, env=env)
    payload = json.loads(completed.stdout)
    assert isinstance(payload, dict), completed.stdout
    return completed.returncode, payload


def _patch_file(directory: Path, ids: list[int]) -> None:
    rows = [
        {
            "id": patch_id,
            "pos": [0.0, 0.0, 0.0],
            "normal": [0.0, 1.0, 0.0],
            "radius": 0.25,
            "node": "Floor",
            "camera": "CovCam0",
        }
        for patch_id in ids
    ]
    payload = {"version": 1, "scene": "cbr_a.tscn", "seed": 1, "camera": "CovCam0", "count": len(rows), "patches": rows}
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "patches_q1.json").write_text(json.dumps(payload) + "\n", encoding="utf-8")


def _mask_png(directory: Path) -> None:
    rgb = np.zeros((*_MASK.shape, 3), dtype=np.uint8)
    rgb[..., 0] = _MASK & 0xFF
    rgb[..., 1] = _MASK >> 8
    write_png(directory / "mask.png", rgb)


def _images(directory: Path, indirect: list[float], frame: int, direct_value: float = 0.25) -> None:
    full = np.full((*_MASK.shape, 3), direct_value, dtype=np.float16)
    for patch_id, value in zip(_IDS, indirect, strict=True):
        full[_MASK == patch_id + 1] = direct_value + value
    direct = np.full((*_MASK.shape, 3), direct_value, dtype=np.float16)
    write_exr(directory / f"hdr_{frame:05d}.exr", full)
    write_exr(directory / "direct" / f"hdr_{frame:05d}.exr", direct)


def _pt_reference(directory: Path, *, spp: int = 4096, se: list[float | None] | None = None, frame: int = 0) -> Path:
    _patch_file(directory, _IDS)
    _mask_png(directory)
    _images(directory, [1.0, 0.5], frame)
    meta = {
        "schema": 1,
        "kind": "pt",
        "scene": "res://scenes/color_box_room/cbr_a.tscn",
        "camera": "CovCam0",
        "frame": frame,
        "seed": 1,
        "spp": spp,
        "direct_spp": spp,
        "batches": 16,
        "batch_spp": spp // 16,
        "patch_ids": _IDS,
        "patch_indirect_luminance": [1.0, 0.5],
        "per_patch_relative_se": [0.004, 0.006] if se is None else se,
    }
    (directory / "reference.json").write_text(json.dumps(meta) + "\n", encoding="utf-8")
    return directory


def _test_capture(directory: Path, indirect: list[float], ids: list[int] | None = None) -> Path:
    _patch_file(directory, _IDS if ids is None else ids)
    _mask_png(directory)
    for frame in range(120, 184):
        _images(directory, indirect, frame)
    return directory


def _details(payload: dict[str, object]) -> dict[str, object]:
    details = payload["details"]
    assert isinstance(details, dict)
    return details


def test_q1_scores_a_capture_against_a_path_traced_reference(tmp_path: Path) -> None:
    reference = _pt_reference(tmp_path / "ref")
    capture = _test_capture(tmp_path / "in", [1.1, 0.45])
    code, payload = _run("q1", "--in", str(capture), "--ref", str(reference))
    assert code == 0, payload
    value = payload["value"]
    assert isinstance(value, float)
    # Mean of |1.1 - 1| / 1 and |0.45 - 0.5| / 0.5, within half-float rounding.
    assert abs(value - 0.1) < 2e-3
    assert payload["pass"] is True
    assert _details(payload)["reference_rejected"] is False


def test_q1_reads_the_held_frame_index(tmp_path: Path) -> None:
    reference = _pt_reference(tmp_path / "ref", frame=32)
    code, payload = _run("q1", "--ref", str(reference))
    assert code == 0, payload
    assert payload["pass"] is True


def test_q1_alone_accepts_a_valid_reference_folder(tmp_path: Path) -> None:
    code, payload = _run("q1", "--ref", str(_pt_reference(tmp_path / "ref")))
    assert code == 0, payload
    assert payload["pass"] is True
    assert payload["value"] is None
    details = _details(payload)
    assert details["reference_only"] is True
    assert details["reference_rejected"] is False
    assert details["reference_reason"] == ""
    values = payload["values"]
    assert isinstance(values, dict)
    assert values["patches"] == 2
    assert values["spp"] == 4096
    assert values["max_relative_se"] == 0.006


def test_q1_alone_rejects_a_short_reference(tmp_path: Path) -> None:
    code, payload = _run("q1", "--ref", str(_pt_reference(tmp_path / "ref", spp=512)))
    assert code == 0, payload
    assert payload["pass"] is False
    details = _details(payload)
    assert details["reference_rejected"] is True
    assert details["reference_reason"] == "reference spp 512 is below 4096"


def test_q1_alone_rejects_a_noisy_reference(tmp_path: Path) -> None:
    code, payload = _run("q1", "--ref", str(_pt_reference(tmp_path / "ref", se=[0.004, 0.012])))
    assert code == 0, payload
    assert payload["pass"] is False
    assert _details(payload)["reference_reason"] == "per-patch relative standard error 0.012 is above 0.01"


def test_q1_rejects_a_missing_or_non_finite_patch_error(tmp_path: Path) -> None:
    code, payload = _run("q1", "--ref", str(_pt_reference(tmp_path / "short", se=[0.004])))
    assert code == 0, payload
    assert payload["pass"] is False
    assert _details(payload)["reference_reason"] == "per-patch relative standard error has 1 values for 2 patches"
    code, payload = _run("q1", "--ref", str(_pt_reference(tmp_path / "null", se=[0.004, None])))
    assert code == 0, payload
    assert payload["pass"] is False
    assert _details(payload)["reference_reason"] == "per-patch relative standard error of patch 1 is not finite"


def test_a_rejected_reference_fails_the_gate_but_reports_the_error(tmp_path: Path) -> None:
    reference = _pt_reference(tmp_path / "ref", spp=512)
    capture = _test_capture(tmp_path / "in", [1.0, 0.5])
    code, payload = _run("q1", "--in", str(capture), "--ref", str(reference))
    assert code == 0, payload
    assert payload["pass"] is False
    assert isinstance(payload["value"], float)
    assert _details(payload)["reference_reason"] == "reference spp 512 is below 4096"


def test_q1_refuses_different_patch_lists(tmp_path: Path) -> None:
    reference = _pt_reference(tmp_path / "ref")
    capture = _test_capture(tmp_path / "in", [1.0, 0.5], ids=[9, 7])
    code, payload = _run("q1", "--in", str(capture), "--ref", str(reference))
    assert code == 2
    assert "patch lists differ" in str(_details(payload)["error"])


def test_only_q1_runs_without_a_capture(tmp_path: Path) -> None:
    code, payload = _run("q1")
    assert code == 2
    assert "--ref is required" in str(_details(payload)["error"])
    code, payload = _run("q2", "--ref", str(tmp_path))
    assert code == 2
    assert "--in is required" in str(_details(payload)["error"])
