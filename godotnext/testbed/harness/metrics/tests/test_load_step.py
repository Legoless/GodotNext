"""GT-6 load step check (#190) on synthetic GPU frame times."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from godotnext_metrics.load_step import LoadStepInputError, gpu_frame_times, load_step_ratio, main


def _write_load_step_csv(path: Path, before_ms: float, after_ms: float, *, frames: int = 720, lag: int = 3) -> None:
    lines = ["frame,burner,gpu_ms,iterations,internal_width,internal_height"]
    for frame in range(frames):
        burner = 1 if frame >= 600 else 0
        # The measured GPU time trails the frame that switched the burner by a few frames.
        loaded = frame >= 600 + lag
        jitter = 0.01 * ((frame * 7) % 5 - 2)
        lines.append(f"{frame},{burner},{(after_ms if loaded else before_ms) + jitter:.6f},4096,2560,1440")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_load_step_doubling_passes(tmp_path: Path) -> None:
    _write_load_step_csv(tmp_path / "gpu_load_step.csv", 6.0, 12.1)
    report = load_step_ratio(gpu_frame_times(tmp_path / "gpu_load_step.csv"))
    assert report["before_ms"] == pytest.approx(6.0, abs=0.02)
    assert report["after_ms"] == pytest.approx(12.1, abs=0.02)
    assert report["ratio"] == pytest.approx(12.1 / 6.0, rel=0.01)
    assert report["pass"] is True


@pytest.mark.parametrize("after_ms", [11.3, 12.7, 6.0])
def test_load_step_outside_five_percent_fails(tmp_path: Path, after_ms: float) -> None:
    _write_load_step_csv(tmp_path / "gpu_load_step.csv", 6.0, after_ms)
    assert load_step_ratio(gpu_frame_times(tmp_path / "gpu_load_step.csv"))["pass"] is False


def test_load_step_settle_skips_the_lagging_frames(tmp_path: Path) -> None:
    # With no settle, the lagging frames would pull the median of the after window down.
    _write_load_step_csv(tmp_path / "gpu_load_step.csv", 5.0, 10.0, lag=24)
    times = gpu_frame_times(tmp_path / "gpu_load_step.csv")
    assert load_step_ratio(times, settle=0)["pass"] is False
    assert load_step_ratio(times, settle=24)["pass"] is True


def test_load_step_reads_harness_timing_csv(tmp_path: Path) -> None:
    rows = ["frame,wall_ms,scene_ms,post_ms"]
    for frame in range(700):
        scale = 2.0 if frame >= 604 else 1.0
        rows.append(f"{frame},16.67,{3.0 * scale:.3f},{1.0 * scale:.3f}")
    rows.append("700,16.67,,")
    (tmp_path / "timing.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    times = gpu_frame_times(tmp_path / "timing.csv")
    assert times[0] == pytest.approx(4.0)
    assert 700 not in times
    assert load_step_ratio(times)["ratio"] == pytest.approx(2.0)


def test_load_step_cli(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    _write_load_step_csv(tmp_path / "gpu_load_step.csv", 6.0, 12.0)
    (tmp_path / "gpu_load_step.json").write_text(
        json.dumps({"schema": 1, "calibration": {"baseline_ms": 6.0, "ratio": 2.01, "pass": True}}) + "\n",
        encoding="utf-8",
    )
    assert main(["--in", str(tmp_path)]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["source"] == "gpu_load_step.csv"
    assert payload["pass"] is True
    assert payload["calibration"]["ratio"] == 2.01
    # A harness timing run decides (GT-6 measures with #12 timing mode); the scene's own figure stays beside it.
    rows = ["frame,wall_ms,scene_ms"] + [f"{frame},16.67,{3.0 if frame < 604 else 5.4:.3f}" for frame in range(700)]
    (tmp_path / "timing.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    assert main(["--in", str(tmp_path)]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["source"] == "timing.csv"
    assert payload["ratio"] == pytest.approx(1.8)
    assert payload["pass"] is False
    assert payload["sources"]["gpu_load_step.csv"]["pass"] is True
    assert payload["sources"]["timing.csv"]["pass"] is False
    assert main(["--in", str(tmp_path / "absent")]) == 2
    assert "no gpu_load_step.csv or timing.csv" in json.loads(capsys.readouterr().out)["error"]


def test_load_step_needs_both_windows(tmp_path: Path) -> None:
    _write_load_step_csv(tmp_path / "gpu_load_step.csv", 6.0, 12.0, frames=620)
    with pytest.raises(LoadStepInputError, match="need 30 frames"):
        load_step_ratio(gpu_frame_times(tmp_path / "gpu_load_step.csv"))
