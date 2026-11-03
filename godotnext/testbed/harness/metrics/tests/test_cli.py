"""Every metric command prints JSON. A failed gate still exits 0."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from godotnext_metrics.images import write_exr, write_png
from godotnext_metrics.percentile import percentile
from godotnext_metrics.rays import records_from_dicts, write_records

_METRICS = [
    "q1",
    "q2",
    "q3",
    "q4a",
    "q4b",
    "q5",
    "q6",
    "q7",
    "q8",
    "q9",
    "q10",
    "q11",
    "q12",
    "q13",
    "q14",
    "q15",
    "q16",
    "q17",
    "q18",
    "q19",
    "q20",
    "q21",
]


def _invoke(metric: str, in_dir: Path, ref: Path | None) -> tuple[int, dict[str, object]]:
    command = [sys.executable, "-m", "godotnext_metrics", metric, "--in", str(in_dir), "--json"]
    if ref is not None:
        command.extend(["--ref", str(ref)])
    env = dict(os.environ)
    root = str(Path(__file__).resolve().parents[1])
    env["PYTHONPATH"] = root + os.pathsep + env.get("PYTHONPATH", "")
    completed = subprocess.run(command, check=False, capture_output=True, text=True, env=env)
    payload = json.loads(completed.stdout)
    if not isinstance(payload, dict):
        raise AssertionError(completed.stdout)
    return completed.returncode, {str(key): value for key, value in payload.items()}


def _write_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload) + "\n", encoding="utf-8")


def _patches(count: int, rooms: list[str] | None = None) -> dict[str, object]:
    rows: list[dict[str, object]] = []
    for index in range(count):
        row: dict[str, object] = {
            "id": index,
            "pos": [0.0, 0.0, 0.0],
            "normal": [0.0, 1.0, 0.0],
            "radius": 0.25,
            "node": "Fixture",
            "camera": "Camera",
        }
        if rooms is not None:
            row["room"] = rooms[index]
            if rooms[index] == "dark":
                row["mirror_of"] = index
        rows.append(row)
    return {"version": 1, "scene": "fixture", "seed": 1, "count": count, "patches": rows}


def _write_indirect(
    directory: Path, frames: dict[int, list[float]], patch_file: str, rooms: list[str] | None = None
) -> None:
    count = len(next(iter(frames.values())))
    _write_json(directory / patch_file, _patches(count, rooms))
    write_png(directory / "mask.png", np.arange(1, count + 1, dtype=np.uint8).reshape(1, count))
    direct = np.zeros((1, count, 3), dtype=np.float32)
    for index, values in frames.items():
        color = np.repeat(np.asarray(values, dtype=np.float32).reshape(1, count, 1), 3, axis=2)
        write_exr(directory / f"hdr_{index:05d}.exr", color)
        write_exr(directory / "direct" / f"hdr_{index:05d}.exr", direct)


def _window(value: float) -> dict[int, list[float]]:
    return {index: [value] for index in range(120, 184)}


def _write_timing(directory: Path, samples: list[float], *, summary_p95: float | None) -> None:
    lines = ["frame,wall_ms,gi_ms"]
    for index, sample in enumerate(samples):
        lines.append(f"{index},{sample:.6f},{sample:.6f}")
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "timing.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if summary_p95 is None:
        return
    stats = {
        "p50": percentile(samples, 50.0),
        "p95": summary_p95,
        "p99": percentile(samples, 99.0),
        "n": len(samples),
    }
    _write_json(
        directory / "summary.json",
        {"schema": 1, "frames": len(samples), "warmup": 120, "columns": {"wall_ms": stats, "gi_ms": stats}},
    )


def _png(color: tuple[int, int, int], size: int = 4) -> np.ndarray:
    image = np.zeros((size, size, 3), dtype=np.uint8)
    image[..., 0] = color[0]
    image[..., 1] = color[1]
    image[..., 2] = color[2]
    return image


def _build(metric: str, root: Path) -> tuple[Path, Path | None]:
    capture = root / "in"
    reference = root / "ref"
    if metric in {"q1", "q8", "q20"}:
        _write_indirect(capture, _window(1.0), "patches_q1.json")
        if metric == "q8":
            return capture, None
        _write_indirect(reference, _window(1.0), "patches_q1.json")
        _write_json(reference / "reference.json", {"spp": 4096, "per_patch_relative_se": [0.0]})
        return capture, reference
    if metric == "q2":
        rows = {f"CovCam{index}": {"valid_card_hits": 90, "hits_within_10m": 100} for index in range(4)}
        _write_json(capture / "counters.json", {"q2": rows})
        return capture, None
    if metric == "q3":
        rooms = ["dark"] * 16 + ["lit"] * 16
        values = [0.0001] * 16 + [1.0] * 16
        _write_indirect(capture, {0: values}, "patches_q3.json", rooms)
        _write_indirect(reference, {0: values}, "patches_q3.json", rooms)
        return capture, reference
    if metric == "q4a":
        _write_indirect(capture, {10: [1.2], 32: [1.05], 240: [1.0]}, "patches_q1.json")
        return capture, None
    if metric == "q4b":
        _write_indirect(capture, {0: [1.0], 36: [1.0], 244: [1.0]}, "patches_q1.json")
        _write_indirect(reference, {0: [1.0]}, "patches_q1.json")
        _write_json(capture / "q4.json", {"motion_start": 0, "motion_end": 4, "spp": 1024})
        return capture, reference
    if metric == "q5":
        write_png(capture / "frame00000000.png", _png((128, 128, 128)))
        return capture, None
    if metric == "q6":
        write_png(capture / "frame00000000.png", _png((255, 255, 255)))
        write_png(capture / "coverage.png", np.ones((4, 4), dtype=np.uint8) * 255)
        return capture, None
    if metric == "q7":
        image = _png((40, 80, 120))
        write_png(capture / "frame00000000.png", image)
        write_png(reference / "frame00000000.png", image)
        return capture, reference
    if metric == "q9":
        image = np.full((4, 4, 3), 0.2, dtype=np.float32)
        write_exr(capture / "hdr_00000.exr", image)
        write_exr(reference / "hdr_00000.exr", image)
        write_exr(capture / "roughness.exr", np.full((4, 4), 0.2, dtype=np.float32))
        return capture, reference
    if metric == "q10":
        image = np.full((4, 4, 3), 0.5, dtype=np.float32)
        write_exr(capture / "hdr_00004.exr", image)
        write_exr(reference / "hdr_00004.exr", image)
        mask = np.zeros((4, 4), dtype=np.uint8)
        mask[1, 1] = 255
        write_png(capture / "disocclusion.png", mask)
        _write_json(capture / "q10.json", {"frame": 4})
        return capture, reference
    if metric in {"q11", "q12"}:
        samples = [float(value) for value in range(1, 101)]
        _write_timing(capture, samples, summary_p95=percentile(samples, 95.0))
        return capture, None
    if metric == "q13":
        for name in (
            capture,
            capture / "run_2",
            capture / "run_3",
            reference,
            reference / "run_2",
            reference / "run_3",
        ):
            _write_timing(name, [1.0, 1.0, 1.0], summary_p95=None)
        return capture, reference
    if metric == "q14":
        _write_json(capture / "counters.json", {"q14": {"legacy_drawn_virtualized_pixels": 10, "opaque_pixels": 100}})
        return capture, None
    if metric == "q15":
        _write_json(
            capture / "counters.json",
            {"q15": {"categories": {"MEMORY_ACCELERATION_STRUCTURES": 10, "other": 90}, "baseline_total": None}},
        )
        return capture, None
    if metric == "q16":
        _write_json(capture / "counters.json", {"q16": {"clamped_energy": 1, "total_indirect_energy": 100}})
        return capture, None
    if metric == "q17":
        row: dict[str, object] = {
            "t": 1.0,
            "custom_index": 3,
            "primitive": 7,
            "u": 0.2,
            "v": 0.3,
            "front_face": 1,
            "committed": 1,
            "candidate_count": 1,
        }
        write_records(capture / "rays.bin", records_from_dicts([row]))
        _write_json(capture / "analytic.json", {"records": [row]})
        return capture, None
    if metric == "q18":
        _write_json(capture / "counters.json", {"q18": {"tlas_instances": 95, "gi_opaque_instances_in_radius": 100}})
        return capture, None
    if metric == "q19":
        image = _png((20, 40, 60))
        _write_json(capture / "q19.json", {"form": "moving", "scene": "fixture"})
        write_png(capture / "frame00000120.png", image)
        write_png(reference / "frame00000120.png", image)
        write_png(reference / "msaa4" / "frame00000120.png", image)
        return capture, reference
    if metric == "q21":
        velocity = np.zeros((1, 3, 2), dtype=np.float32)
        velocity[0, 0] = (-1.0, -1.0)
        velocity[0, 1] = (np.nan, 0.0)
        velocity[0, 2] = (-1.0001, -1.2)
        write_exr(capture / "velocity_00000.exr", velocity)
        return capture, None
    raise AssertionError(metric)


@pytest.mark.parametrize("metric", _METRICS)
def test_every_cli_emits_json(metric: str, tmp_path: Path) -> None:
    capture, reference = _build(metric, tmp_path)
    code, payload = _invoke(metric, capture, reference)
    assert code == 0, payload
    for key in ("metric", "pass", "threshold", "value"):
        assert key in payload
    assert isinstance(payload["pass"], bool)
    assert "values" in payload


def test_failed_gate_still_exits_zero(tmp_path: Path) -> None:
    capture = tmp_path / "in"
    magenta = np.zeros((3, 3, 3), dtype=np.uint8)
    magenta[..., 0] = 255
    magenta[..., 2] = 255
    write_png(capture / "frame00000000.png", magenta)
    code, payload = _invoke("q5", capture, None)
    assert code == 0
    assert payload["value"] == 9
    assert payload["pass"] is False


def test_q11_and_q12_cli_match_summary(tmp_path: Path) -> None:
    samples = [float(value) for value in range(1, 101)]
    capture = tmp_path / "in"
    _write_timing(capture, samples, summary_p95=95.05)
    q11_code, q11 = _invoke("q11", capture, None)
    q12_code, q12 = _invoke("q12", capture, None)
    assert q11_code == 0 and q12_code == 0
    q11_value = q11["value"]
    assert isinstance(q11_value, (int, float))
    assert abs(float(q11_value) - 95.05) <= 0.01
    value = q12["value"]
    assert isinstance(value, dict)
    assert abs(float(value["p95"]) - 95.05) <= 0.01
    assert abs(float(value["p99"]) - 99.01) <= 0.01


def test_q21_cli_reports_two(tmp_path: Path) -> None:
    code, payload = _invoke("q21", _build("q21", tmp_path)[0], None)
    assert code == 0
    assert payload["value"] == 2
    assert payload["pass"] is False


def test_unknown_metric_and_missing_input_are_json_errors(tmp_path: Path) -> None:
    unknown_code, unknown = _invoke("q99", tmp_path, None)
    assert unknown_code != 0
    assert unknown["pass"] is False
    details = unknown["details"]
    assert isinstance(details, dict)
    assert "unknown metric" in str(details["error"])
    missing_code, missing = _invoke("q2", tmp_path, None)
    assert missing_code != 0
    assert missing["pass"] is False
    missing_details = missing["details"]
    assert isinstance(missing_details, dict)
    assert "counters.json" in str(missing_details["error"])
