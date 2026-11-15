"""GU+ displayed-frame, latency, UI-capture, F1 and fallback tools (#202) on fixtures.

The fixtures in ``fixtures/fg`` are written from the tools' public documentation, except one CSV
recorded with PresentMon 2.6.0 (see the README there); expected values are worked out by hand from
their rows.
"""

from __future__ import annotations

import hashlib
import importlib.metadata
import json
import os
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
from godotnext_metrics import display_capture, fallback_log_check, nsys_dlssg, presentmon_metrics
from godotnext_metrics.images import write_png
from godotnext_metrics.presentmon_metrics import PresentMonInputError, read_frames, summarize, summary_report

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "fg"
PRESENTMON_CSV = FIXTURES / "presentmon_2_6_0_fg_off.csv"
V2_CSV = FIXTURES / "presentmon_v2_metrics.csv"
RECORDED_CSV = FIXTURES / "presentmon_2_6_0_recorded.csv"
HEADER = "Application,ProcessID,SwapChainAddress,MsGPUBusy,DisplayedTime,MsBetweenDisplayChange,MsPCLatency,MsBetweenAppStart"


def _launcher(folder: Path, name: str, script: Path) -> Path:
    """An executable file that runs ``script`` with this Python: a .cmd file on Windows, sh elsewhere."""
    if os.name == "nt":
        path = folder / f"{name}.cmd"
        path.write_text(f'@"{sys.executable}" "{script}" %*\r\n', encoding="utf-8")
    else:
        path = folder / name
        path.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="utf-8")
        path.chmod(0o755)
    return path


def _chain_rows(chain: str, rows: int, display_ms: float = 16.0) -> list[str]:
    shown = f"{display_ms:.4f}"
    return [f"godot.exe,7,{chain},15.5,{shown},{shown},30.0,16.0"] * rows


def _synthetic_csv(
    path: Path,
    rows: int,
    *,
    display_ms: float = 16.0,
    busy_ms: float = 15.5,
    interval_ms: float = 16.0,
    latency_ms: float | None = 30.0,
    drop_every: int = 0,
    swap_chains: int = 1,
) -> None:
    lines = [HEADER]
    for index in range(rows):
        dropped = drop_every > 0 and index % drop_every == drop_every - 1
        shown = "NA" if dropped else f"{display_ms:.4f}"
        change = "NA" if dropped else f"{display_ms:.4f}"
        latency = "NA" if latency_ms is None or dropped else f"{latency_ms:.4f}"
        chain = f"0x{(index % swap_chains) + 1:016X}"
        lines.append(f"godot.exe,7,{chain},{busy_ms:.4f},{shown},{change},{latency},{interval_ms:.4f}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# PresentMon -------------------------------------------------------------------------------------


def test_presentmon_fixture_window_math() -> None:
    report = summary_report(PRESENTMON_CSV, warmup=2, count=8)
    shown = report["displayed_frame_ms"]
    assert isinstance(shown, dict)
    # Window: rows 3, 5-11 (row 0 has no previous display change, row 4 was dropped).
    assert shown["p50"] == pytest.approx(16.1)
    assert shown["p99"] == pytest.approx(23.482)
    assert report["p99_over_median"] == pytest.approx(23.482 / 16.1)
    assert report["displayed_rate_hz"] == pytest.approx(8000.0 / 136.6)
    # Row 6 reports no PC latency: the p50 of the other seven window frames.
    assert report["pc_latency_ms"] == pytest.approx(30.0)
    latency = report["pc_latency"]
    assert isinstance(latency, dict)
    assert latency["frames"] == 7
    assert latency["mean"] == pytest.approx(222.0 / 7.0)
    # GPU busy over rows 3-11, the dropped row 4 included; the warm-up rows (busy 5 ms) are not.
    assert report["gpu_busy_fraction"] == pytest.approx(147.0 / 152.0)
    assert report["gpu_bound"] is True
    assert report["pass"] is True
    assert report["dropped_in_window"] == 1
    source = report["source"]
    assert isinstance(source, dict)
    assert source["columns"] == {
        "display": "MsBetweenDisplayChange",
        "gpu_busy": "MsGPUBusy",
        "frame_interval": "MsBetweenAppStart",
        "pc_latency": "MsPCLatency",
    }


def test_presentmon_recorded_csv() -> None:
    # PresentMon 2.6.0 on the static-UI scene (D3D12, cloud L4 VM, no Streamline). Other Godot
    # processes presented during the recording, so the CSV has three process / swap chain pairs.
    header = RECORDED_CSV.read_text(encoding="utf-8").splitlines()[0].split(",")
    # The real default CSV has no DisplayedTime column; the displayed-frame rule reads MsBetweenDisplayChange.
    assert "DisplayedTime" not in header and "MsBetweenDisplayChange" in header
    with pytest.raises(PresentMonInputError, match="3 process / swap chain pairs"):
        summary_report(RECORDED_CSV, warmup=20, count=150, require_latency=False)
    with pytest.raises(PresentMonInputError, match="MsPCLatency"):
        summary_report(RECORDED_CSV, warmup=20, count=150, swap_chain="auto")
    report = summary_report(RECORDED_CSV, warmup=20, count=150, require_latency=False, swap_chain="auto")
    source = report["source"]
    assert isinstance(source, dict)
    assert source["process"] == {
        "Application": "godot.windows.editor.x86_64.exe",
        "ProcessID": "3432",
        "SwapChainAddress": "0x1FF922B9B50",
    }
    assert source["rows"] == 193 and source["pairs_in_file"] == 3
    assert source["columns"] == {
        "display": "MsBetweenDisplayChange",
        "gpu_busy": "MsGPUBusy",
        "frame_interval": "MsBetweenAppStart",
        "pc_latency": "MsPCLatency",
    }
    # Expected values computed from the CSV rows with awk, independently of this module.
    shown = report["displayed_frame_ms"]
    assert isinstance(shown, dict)
    assert shown["p50"] == pytest.approx(16.6127)
    assert shown["p99"] == pytest.approx(17.637711)
    assert report["displayed_rate_hz"] == pytest.approx(60.0038450464)
    assert report["gpu_busy_fraction"] == pytest.approx(0.1575681585)
    assert report["gpu_busy_rows"] == 150 and report["dropped_in_window"] == 0
    # VSync-bound at 60 Hz with the GPU mostly idle: not a GPU-bound run.
    assert report["gpu_bound"] is False and report["pass"] is False
    explicit = summary_report(RECORDED_CSV, warmup=20, count=150, require_latency=False, swap_chain="0x1FF922B9B50")
    assert explicit["displayed_frame_ms"] == shown


def test_presentmon_v2_metrics_header_family() -> None:
    report = summary_report(V2_CSV, warmup=1, count=4, require_latency=False)
    shown = report["displayed_frame_ms"]
    assert isinstance(shown, dict)
    # DisplayedTime is the interval when MsBetweenDisplayChange is absent.
    assert shown["p50"] == pytest.approx(8.25)
    assert report["pc_latency_ms"] is None
    # 5 x 8.0 / 5 x 8.4 = 0.952: GPU-bound, just.
    assert report["gpu_busy_fraction"] == pytest.approx(40.0 / 42.0)
    assert report["gpu_bound"] is True


def test_presentmon_default_window_counts_displayed_frames_only(tmp_path: Path) -> None:
    csv_path = tmp_path / "run.csv"
    # Every 10th row is dropped: 800 rows hold 720 displayed frames exactly.
    _synthetic_csv(csv_path, 800, drop_every=10)
    frames, _source = read_frames(csv_path)
    assert sum(frame.displayed for frame in frames) == 720
    report = summarize(frames)
    assert report["frames"] == 600
    assert report["warmup"] == 120
    assert report["displayed_rate_hz"] == pytest.approx(62.5)
    # The window runs from row 133 (displayed frame 120) to row 798 and holds 66 dropped rows.
    assert report["dropped_in_window"] == 66
    with pytest.raises(PresentMonInputError, match="need 721 displayed frames"):
        summarize(frames, count=601)


def test_presentmon_short_run_is_an_input_error(tmp_path: Path) -> None:
    csv_path = tmp_path / "run.csv"
    _synthetic_csv(csv_path, 719)
    with pytest.raises(PresentMonInputError, match="need 720 displayed frames"):
        summary_report(csv_path)


@pytest.mark.parametrize(("busy_ms", "bound"), [(15.2, True), (15.19, False), (12.0, False)])
def test_gpu_bound_rejection(tmp_path: Path, busy_ms: float, bound: bool) -> None:
    csv_path = tmp_path / "run.csv"
    _synthetic_csv(csv_path, 720, busy_ms=busy_ms, interval_ms=16.0)
    report = summary_report(csv_path)
    assert report["gpu_busy_fraction"] == pytest.approx(busy_ms / 16.0)
    assert report["gpu_bound"] is bound
    assert report["pass"] is bound


def test_missing_pc_latency_is_an_error_unless_declared(tmp_path: Path) -> None:
    csv_path = tmp_path / "run.csv"
    _synthetic_csv(csv_path, 720, latency_ms=None)
    with pytest.raises(PresentMonInputError, match="MsPCLatency"):
        summary_report(csv_path)
    report = summary_report(csv_path, require_latency=False)
    assert report["pc_latency_ms"] is None
    assert report["pc_latency"] is None


def test_two_swap_chains_need_a_choice(tmp_path: Path) -> None:
    csv_path = tmp_path / "run.csv"
    _synthetic_csv(csv_path, 1440, swap_chains=2)
    with pytest.raises(PresentMonInputError, match="2 process / swap chain pairs"):
        summary_report(csv_path)
    report = summary_report(csv_path, swap_chain="0x0000000000000002")
    assert report["frames"] == 600
    source = report["source"]
    assert isinstance(source, dict)
    assert source["pairs_in_file"] == 2
    # The two swap chains have 720 displayed frames each: auto cannot choose.
    with pytest.raises(PresentMonInputError, match="720 displayed frames each"):
        summary_report(csv_path, swap_chain="auto")


def test_auto_reads_the_swap_chain_with_the_most_displayed_frames(tmp_path: Path) -> None:
    csv_path = tmp_path / "run.csv"
    # A short-lived first swap chain (recreated on a mode switch, say), then the run's own.
    rows = [HEADER, *_chain_rows("0xA", 30, display_ms=40.0), *_chain_rows("0xB", 720)]
    csv_path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    with pytest.raises(PresentMonInputError, match="choose one by its SwapChainAddress or auto"):
        summary_report(csv_path)
    report = summary_report(csv_path, swap_chain="auto")
    source = report["source"]
    assert isinstance(source, dict)
    assert source["process"] == {"Application": "godot.exe", "ProcessID": "7", "SwapChainAddress": "0xB"}
    assert source["rows"] == 720
    assert report["displayed_rate_hz"] == pytest.approx(62.5)


def test_compare_and_spread_choose_a_swap_chain_per_csv(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    # A SwapChainAddress belongs to one process, so every run's CSV has its own.
    paths = []
    for index, display_ms in enumerate((16.0, 10.0, 16.2)):
        path = tmp_path / f"run_{index}.csv"
        rows = [HEADER, *_chain_rows(f"0x{index}F", 5, display_ms=33.0), *_chain_rows(f"0x{index}A", 720, display_ms)]
        path.write_text("\n".join(rows) + "\n", encoding="utf-8")
        paths.append(str(path))
    base, test, third = paths
    compare_args = ["compare", "--kind", "fg", "--test", test, "--base", base]
    assert presentmon_metrics.main([*compare_args, "--test-swap-chain", "0x1A", "--base-swap-chain", "0x0A"]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["displayed_rate_ratio"] == pytest.approx(1.6)
    assert presentmon_metrics.main([*compare_args, "--test-swap-chain", "auto", "--base-swap-chain", "auto"]) == 0
    assert json.loads(capsys.readouterr().out)["displayed_rate_ratio"] == pytest.approx(1.6)
    assert presentmon_metrics.main([*compare_args, "--test-swap-chain", "0x1A"]) == 2
    assert "process / swap chain pairs" in json.loads(capsys.readouterr().out)["error"]
    assert presentmon_metrics.main(["spread", "--csv", base, third, "--swap-chain", "0x0A", "0x2A"]) == 0
    spread = json.loads(capsys.readouterr().out)
    assert spread["displayed_p99_ms"] == pytest.approx([16.0, 16.2])
    assert presentmon_metrics.main(["spread", "--csv", base, third, "--swap-chain", "auto"]) == 0
    assert json.loads(capsys.readouterr().out)["displayed_p99_ms"] == pytest.approx([16.0, 16.2])
    assert presentmon_metrics.main(["spread", "--csv", base, test, third, "--swap-chain", "0x0A", "0x1A"]) == 2
    assert "one --swap-chain per CSV (3)" in json.loads(capsys.readouterr().out)["error"]


def _summary_for(tmp_path: Path, name: str, **kwargs: float) -> dict[str, object]:
    path = tmp_path / f"{name}.csv"
    _synthetic_csv(path, 1500, **kwargs)  # type: ignore[arg-type]
    return summary_report(path)


def test_compare_fg_rate_ratio_and_pacing(tmp_path: Path) -> None:
    base = _summary_for(tmp_path, "off", display_ms=16.0)
    fast = _summary_for(tmp_path, "fg_fast", display_ms=10.0)
    slow = _summary_for(tmp_path, "fg_slow", display_ms=10.5)
    passed = presentmon_metrics.compare(fast, base, "fg")
    assert passed["displayed_rate_ratio"] == pytest.approx(1.6)
    assert passed["test_p99_over_median"] == pytest.approx(1.0)
    assert passed["pass"] is True
    assert presentmon_metrics.compare(slow, base, "fg")["pass"] is False
    cpu_bound_base = _summary_for(tmp_path, "off_cpu", display_ms=16.0, busy_ms=10.0)
    refused = presentmon_metrics.compare(fast, cpu_bound_base, "fg")
    assert refused["base_gpu_bound"] is False
    assert refused["pass"] is False


def test_compare_fg_fails_on_uneven_pacing(tmp_path: Path) -> None:
    base = _summary_for(tmp_path, "off", display_ms=16.0)
    rows = [HEADER]
    # FG 2x pacing that alternates 3 ms and 13 ms: twice the rate, but p99 / p50 = 13 / 8 > 1.5.
    for index in range(1500):
        shown = 3.0 if index % 2 else 13.0
        rows.append(f"godot.exe,7,0x1,7.6,{shown},{shown},20.0,8.0")
    path = tmp_path / "fg_uneven.csv"
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    uneven = summary_report(path)
    result = presentmon_metrics.compare(uneven, base, "fg")
    assert result["displayed_rate_ratio"] == pytest.approx(2.0)
    assert result["test_p99_over_median"] == pytest.approx(13.0 / 8.0)
    assert result["pass"] is False


@pytest.mark.parametrize(("latency_on", "passed"), [(27.0, True), (27.1, False)])
def test_compare_reflex_latency_reduction(tmp_path: Path, latency_on: float, passed: bool) -> None:
    base = _summary_for(tmp_path, "reflex_off", latency_ms=30.0)
    test = _summary_for(tmp_path, "reflex_on", latency_ms=latency_on, busy_ms=14.0)
    result = presentmon_metrics.compare(test, base, "reflex")
    assert result["latency_reduction"] == pytest.approx(1.0 - latency_on / 30.0)
    # Reflex On may leave the GPU a little idle; the GPU-bound rule is the baseline's.
    assert result["base_gpu_bound"] is True
    assert result["pass"] is passed


# 16.48 and 30.9 are a 3.0% spread, the bound itself; 16.5 is 3.125%.
@pytest.mark.parametrize(("third_p99", "third_latency", "passed"), [(16.48, 30.9, True), (16.5, 30.0, False)])
def test_spread_over_runs(tmp_path: Path, third_p99: float, third_latency: float, passed: bool) -> None:
    runs = [
        _summary_for(tmp_path, "run_1", display_ms=16.0, latency_ms=30.0),
        _summary_for(tmp_path, "run_2", display_ms=16.2, latency_ms=30.3),
        _summary_for(tmp_path, "run_3", display_ms=third_p99, latency_ms=third_latency),
    ]
    result = presentmon_metrics.spread(runs)
    assert result["displayed_p99_spread_percent"] == pytest.approx((third_p99 - 16.0) / 16.0 * 100.0)
    assert result["pass"] is passed


def test_presentmon_command_is_the_pinned_form(tmp_path: Path) -> None:
    command = presentmon_metrics.presentmon_command(
        "PresentMon-2.6.0-x64.exe", "godot.windows.editor.x86_64.exe", tmp_path / "pm.csv", delay_s=20
    )
    assert command[:5] == [
        "PresentMon-2.6.0-x64.exe",
        "--process_name",
        "godot.windows.editor.x86_64.exe",
        "--output_file",
        str(tmp_path / "pm.csv"),
    ]
    for flag in ("--track_pc_latency", "--terminate_on_proc_exit", "--stop_existing_session", "--no_console_stats"):
        assert flag in command
    assert command[-2:] == ["--delay", "20"]
    assert "--v2_metrics" not in command and "--v1_metrics" not in command


FAKE_PRESENTMON = """
import pathlib, shutil, sys, time
args = sys.argv[1:]
out = pathlib.Path(args[args.index("--output_file") + 1])
sentinel = out.parent / "launch_done"
deadline = time.time() + 30
while not sentinel.exists() and time.time() < deadline:
    time.sleep(0.05)
shutil.copy(sys.argv[0].replace("fake_presentmon.py", "source.csv"), out)
(out.parent / "presentmon_args.txt").write_text("\\n".join(args))
"""


def _record_args(wrapper: Path, out: Path) -> list[str]:
    launch = [sys.executable, "-c", f"import pathlib; pathlib.Path(r'{out / 'launch_done'}').write_text('1')"]
    options = ["--presentmon", str(wrapper), "--process-name", "godot.exe", "--out", str(out), "--start-wait", "0.2"]
    return ["record", *options, "--", *launch]


def _fake_presentmon(tmp_path: Path) -> Path:
    tools = tmp_path / "tools"
    tools.mkdir()
    (tools / "fake_presentmon.py").write_text(FAKE_PRESENTMON, encoding="utf-8")
    _synthetic_csv(tools / "source.csv", 720)
    return _launcher(tools, "presentmon", tools / "fake_presentmon.py")


def test_record_runs_presentmon_around_the_launch(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    wrapper = _fake_presentmon(tmp_path)
    digest = hashlib.sha256(wrapper.read_bytes()).hexdigest()
    # The fake stands in for the pinned executable.
    monkeypatch.setattr(presentmon_metrics, "PRESENTMON_SHA256", digest)
    out = tmp_path / "run"
    code = presentmon_metrics.main(_record_args(wrapper, out))
    report = json.loads(capsys.readouterr().out)
    assert code == 0, report
    assert report["record"]["launch_exit"] == 0
    assert report["record"]["presentmon_exit"] == 0
    assert report["record"]["presentmon_executable"] == {"file": wrapper.name, "sha256": digest, "version": "2.6.0"}
    assert report["frames"] == 600 and report["gpu_bound"] is True
    assert json.loads((out / "presentmon.json").read_text(encoding="utf-8")) == report
    assert "--track_pc_latency" in (out / "presentmon_args.txt").read_text(encoding="utf-8").split("\n")


def test_record_refuses_a_presentmon_other_than_the_pin(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    wrapper = _fake_presentmon(tmp_path)
    out = tmp_path / "run"
    assert presentmon_metrics.main(_record_args(wrapper, out)) == 2
    error = json.loads(capsys.readouterr().out)["error"]
    assert "is not PresentMon 2.6.0 (PresentMon-2.6.0-x64.exe" in error
    assert presentmon_metrics.PRESENTMON_SHA256 in error
    # Refused before anything ran.
    assert not out.exists()
    assert presentmon_metrics.main(_record_args(tmp_path / "no_such_presentmon.exe", out)) == 2
    assert "not found" in json.loads(capsys.readouterr().out)["error"]


def test_presentmon_cli_exit_codes(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert presentmon_metrics.main(["summary", "--csv", str(PRESENTMON_CSV), "--warmup", "2", "--frames", "8"]) == 0
    assert json.loads(capsys.readouterr().out)["gpu_bound"] is True
    assert presentmon_metrics.main(["summary", "--csv", str(tmp_path / "missing.csv")]) == 2
    assert json.loads(capsys.readouterr().out)["pass"] is False
    assert presentmon_metrics.main(["summary", "--csv", str(V2_CSV), "--warmup", "1", "--frames", "4"]) == 2
    assert "MsPCLatency" in json.loads(capsys.readouterr().out)["error"]


# Displayed-UI capture ---------------------------------------------------------------------------


def _mask(shape: tuple[int, int] = (24, 32)) -> np.ndarray:
    mask = np.zeros(shape, dtype=bool)
    mask[4:10, 3:20] = True  # a "label"
    mask[12, 6:26] = True  # a 1-px "crosshair" line
    mask[14:22, 22:30] = True  # a "minimap"
    return mask


def _ui_frame(rng: np.random.Generator, mask: np.ndarray, ui: np.ndarray) -> np.ndarray:
    """A BGRA desktop frame: a random 3D background with ``ui`` (RGB, per mask pixel) on top."""
    frame = rng.integers(0, 256, size=(*mask.shape, 4), dtype=np.uint8)
    frame[..., 3] = 255
    frame[mask, 0] = ui[:, 2]
    frame[mask, 1] = ui[:, 1]
    frame[mask, 2] = ui[:, 0]
    return frame


def test_capture_frames_skips_repeats_and_reads_bgra() -> None:
    mask = _mask()
    rng = np.random.default_rng(1)
    ui = rng.integers(0, 256, size=(int(mask.sum()), 3), dtype=np.uint8)
    frames = [None, _ui_frame(rng, mask, ui), None, None, _ui_frame(rng, mask, ui), _ui_frame(rng, mask, ui)]
    queue = list(frames)
    ticks = iter(range(100))
    pixels, stamps, grabs = display_capture.capture_frames(
        lambda: queue.pop(0), mask, frames=3, clock=lambda: float(next(ticks))
    )
    assert grabs == 6
    assert pixels.shape == (3, int(mask.sum()), 3)
    for index in range(3):
        np.testing.assert_array_equal(pixels[index], ui)
    assert np.all(np.diff(stamps) > 0.0)


def test_capture_frames_rejects_a_display_of_another_size() -> None:
    mask = _mask()
    with pytest.raises(display_capture.CaptureError, match="the UI mask 32x24"):
        display_capture.capture_frames(lambda: np.zeros((48, 64, 4), dtype=np.uint8), mask, frames=1)


def test_capture_frames_times_out_when_nothing_is_presented() -> None:
    ticks = iter(np.arange(0.0, 10.0, 0.5))
    with pytest.raises(display_capture.CaptureError, match="only 0 of 2"):
        display_capture.capture_frames(lambda: None, _mask(), frames=2, timeout_s=2.0, clock=lambda: float(next(ticks)))


def test_ui_flip_ignores_the_3d_image_behind_the_ui(tmp_path: Path) -> None:
    mask = _mask()
    rng = np.random.default_rng(7)
    ui = rng.integers(0, 256, size=(int(mask.sum()), 3), dtype=np.uint8)
    # Same UI pixels over two different backgrounds: exactly 0 on the UI pixels.
    test_frames = [_ui_frame(rng, mask, ui) for _ in range(4)]
    ref_frames = [_ui_frame(rng, mask, ui) for _ in range(4)]
    test_pixels, _stamps, _grabs = display_capture.capture_frames(iter(test_frames).__next__, mask, frames=4)
    ref_pixels, _stamps, _grabs = display_capture.capture_frames(iter(ref_frames).__next__, mask, frames=4)
    assert display_capture.ui_flip(mask, test_pixels, ref_pixels) == [0.0, 0.0, 0.0, 0.0]
    display_capture.save_capture(tmp_path / "fg_on", mask, test_pixels, np.arange(4.0))
    display_capture.save_capture(tmp_path / "fg_off", mask, ref_pixels, np.arange(4.0))
    report = display_capture.compare(tmp_path / "fg_on", tmp_path / "fg_off", frames=4)
    assert report["value"] == 0.0
    assert report["mask_pixels"] == int(mask.sum())
    assert report["output_resolution"] == [32, 24]
    assert report["pass"] is True


def test_ui_flip_sees_a_changed_ui_pixel(tmp_path: Path) -> None:
    mask = _mask()
    ui = np.full((int(mask.sum()), 3), 200, dtype=np.uint8)
    broken = ui.copy()
    broken[: broken.shape[0] // 2] = 20  # half the UI ghosted, as a generated frame might
    ref_pixels = np.stack([ui] * 3)
    test_pixels = np.stack([ui, broken, ui])
    values = display_capture.ui_flip(mask, test_pixels, ref_pixels)
    assert values[0] == 0.0 and values[2] == 0.0
    assert values[1] > 0.1
    display_capture.save_capture(tmp_path / "on", mask, test_pixels, np.arange(3.0))
    display_capture.save_capture(tmp_path / "off", mask, ref_pixels, np.arange(3.0))
    report = display_capture.compare(tmp_path / "on", tmp_path / "off", frames=3)
    assert report["max_frame"] == 1
    assert report["value"] == pytest.approx(values[1] / 3.0)
    assert report["pass"] is False


def test_compare_reads_harness_frame_folders(tmp_path: Path) -> None:
    mask = _mask()
    rng = np.random.default_rng(11)
    ui = rng.integers(0, 256, size=(int(mask.sum()), 3), dtype=np.uint8)
    for name in ("run_a", "run_b"):
        folder = tmp_path / name
        write_png(folder / "cmask_ui.png", (mask * 255).astype(np.uint8))
        for index in range(3):
            bgra = _ui_frame(rng, mask, ui)
            write_png(folder / f"frame{index:08d}.png", bgra[..., [2, 1, 0, 3]])
    report = display_capture.compare(tmp_path / "run_a", tmp_path / "run_b", frames=3)
    assert report["value"] == 0.0 and report["pass"] is True
    with pytest.raises(display_capture.CaptureError, match="need 4 frames"):
        display_capture.compare(tmp_path / "run_a", tmp_path / "run_b", frames=4)


def test_compare_rejects_mismatched_masks(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    mask = _mask()
    other = mask.copy()
    other[0, 0] = True
    pixels = np.zeros((1, int(mask.sum()), 3), dtype=np.uint8)
    display_capture.save_capture(tmp_path / "a", mask, pixels, np.zeros(1))
    display_capture.save_capture(tmp_path / "b", other, np.zeros((1, int(other.sum()), 3), dtype=np.uint8), np.zeros(1))
    code = display_capture.main([
        "compare",
        "--test",
        str(tmp_path / "a"),
        "--ref",
        str(tmp_path / "b"),
        "--frames",
        "1",
    ])
    assert code == 2
    assert "different UI masks" in json.loads(capsys.readouterr().out)["error"]


def test_dxcam_version_must_be_the_pin() -> None:
    assert display_capture.dxcam_version(lambda name: "0.3.0") == "0.3.0"
    with pytest.raises(display_capture.CaptureError, match="dxcam 0.2.9 is installed, the pin is 0.3.0"):
        display_capture.dxcam_version(lambda name: "0.2.9")

    def missing(name: str) -> str:
        raise importlib.metadata.PackageNotFoundError(name)

    with pytest.raises(display_capture.CaptureError, match="dxcam is not installed"):
        display_capture.dxcam_version(missing)


def test_capture_reports_a_display_dxcam_cannot_open(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    # Outside the interactive desktop dxcam raises a COMError, which is not an OSError or ValueError.
    class ComError(Exception):
        pass

    def create(**_kwargs: object) -> None:
        raise ComError("A resource is not available at the time of the call")

    write_png(tmp_path / "cmask_ui.png", (_mask() * 255).astype(np.uint8))
    monkeypatch.setitem(sys.modules, "dxcam", SimpleNamespace(create=create))
    monkeypatch.setattr(display_capture, "dxcam_version", lambda: "0.3.0")
    args = ["capture", "--mask", str(tmp_path / "cmask_ui.png"), "--out", str(tmp_path / "out"), "--frames", "1"]
    code = display_capture.main(args)
    error = json.loads(capsys.readouterr().out)["error"]
    assert code == 2
    assert "dxcam cannot open the display" in error and "interactive desktop session" in error
    assert not (tmp_path / "out").exists()


class _FakeGame:
    """Stands in for the launched game: ``poll`` gives ``exit_code`` (None while it runs)."""

    def __init__(self, exit_code: int | None) -> None:
        self.returncode = exit_code
        self.terminated = False

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True
        self.returncode = -15

    def wait(self) -> int | None:
        return self.returncode


@pytest.mark.parametrize("exit_code", [None, 0, 3])
def test_capture_fails_when_the_game_exits_before_the_last_grab(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch, exit_code: int | None
) -> None:
    mask = _mask()
    write_png(tmp_path / "cmask_ui.png", (mask * 255).astype(np.uint8))
    rng = np.random.default_rng(5)
    ui = rng.integers(0, 256, size=(int(mask.sum()), 3), dtype=np.uint8)
    frames = iter([_ui_frame(rng, mask, ui) for _ in range(3)])
    released: list[bool] = []
    game = _FakeGame(exit_code)
    launched: list[list[str]] = []

    def popen(command: list[str]) -> _FakeGame:
        launched.append(command)
        return game

    monkeypatch.setattr(display_capture, "dxcam_version", lambda: "0.3.0")
    monkeypatch.setattr(
        display_capture, "_dxcam_grab", lambda device, output: (frames.__next__, lambda: released.append(True))
    )
    monkeypatch.setattr(display_capture.subprocess, "Popen", popen)
    args = ["capture", "--mask", str(tmp_path / "cmask_ui.png"), "--out", str(tmp_path / "out"), "--frames", "3"]
    code = display_capture.main([*args, "--", "godot.exe", "res://scenes/static_ui/static_ui.tscn"])
    report = json.loads(capsys.readouterr().out)
    assert launched == [["godot.exe", "res://scenes/static_ui/static_ui.tscn"]]
    assert released == [True]
    if exit_code is None:
        assert code == 0, report
        assert report["launch"] == {"running_at_last_grab": True, "exit_after_stop": -15}
        assert report["dxcam"] == "0.3.0" and report["frames"] == 3
        assert game.terminated
        assert (tmp_path / "out" / "ui_pixels.npz").is_file()
    else:
        assert code == 2
        assert report["error"] == f"the launched game exited with {exit_code} before the last grab"
        assert not game.terminated
        assert not (tmp_path / "out").exists()


# F1 trace method --------------------------------------------------------------------------------


@pytest.fixture
def nsys_export(tmp_path: Path) -> Path:
    path = tmp_path / "report.sqlite"
    connection = sqlite3.connect(path)
    connection.executescript((FIXTURES / "nsys_dx12_workload.sql").read_text(encoding="utf-8"))
    connection.commit()
    connection.close()
    return path


def test_f1_p95_from_the_streamline_queue(nsys_export: Path) -> None:
    frames, info = nsys_dlssg.dlssg_frames(nsys_export)
    assert frames == pytest.approx({1: 1.2, 2: 1.2, 3: 1.5, 4: 1.0, 5: 1.3, 6: 1.1})
    assert info["matched_names"] == ["nv.sl.dlss_g.cmdQ.dlssg"]
    assert info["matched_without_frame"] == 1
    report = nsys_dlssg.f1_report(nsys_export, warmup=1, count=5)
    assert report["p50_ms"] == pytest.approx(1.2)
    assert report["p95_ms"] == pytest.approx(1.46)
    assert report["p99_ms"] == pytest.approx(1.492)
    assert report["pass"] is None
    # A wider prefix also takes the Reflex queue's work on frame 6.
    wider, _info = nsys_dlssg.dlssg_frames(nsys_export, prefix="nv.sl.")
    assert wider[6] == pytest.approx(1.3)


@pytest.mark.parametrize(("expect_ms", "passed"), [(1.40, True), (1.38, False)])
def test_f1_reproduces_a_reference_within_five_percent(nsys_export: Path, expect_ms: float, passed: bool) -> None:
    report = nsys_dlssg.f1_report(nsys_export, warmup=1, count=5, expect_ms=expect_ms)
    assert report["deviation"] == pytest.approx(abs(1.46 - expect_ms) / expect_ms)
    assert report["pass"] is passed


def test_f1_without_a_match_lists_the_names(nsys_export: Path, capsys: pytest.CaptureFixture[str]) -> None:
    code = nsys_dlssg.main(["--sqlite", str(nsys_export), "--name-prefix", "nv.sl.dlss_fg"])
    assert code == 2
    error = json.loads(capsys.readouterr().out)["error"]
    assert "GodotNext graphics queue" in error and "nv.sl.dlss_g.cmdQ.dlssg" in error
    with pytest.raises(nsys_dlssg.TraceInputError, match="need 720 frames"):
        nsys_dlssg.f1_report(nsys_export)


def test_f1_needs_the_dx12_tables(tmp_path: Path) -> None:
    path = tmp_path / "empty.sqlite"
    sqlite3.connect(path).close()
    with pytest.raises(nsys_dlssg.TraceInputError, match="DX12_WORKLOAD"):
        nsys_dlssg.dlssg_frames(path)


# GU+-7 fallback checker -------------------------------------------------------------------------


def _status_line(available: bool, **status: object) -> str:
    probe: dict[str, object] = {"available": available}
    if available:
        probe["status"] = status
        if "reason_name" in status:
            probe["reason_name"] = status.pop("reason_name")
    return fallback_log_check.STATUS_PREFIX + json.dumps(probe)


def test_fallback_passes_with_the_reason_and_one_log_line() -> None:
    config = fallback_log_check.configuration("vulkan_fg")
    log = "\n".join([
        "Godot Engine v4.8.beta",
        "Frame generation: DLSS-G needs D3D12 (API_UNSUPPORTED).",
        _status_line(True, reason=1, reason_name="FRAME_GENERATION_REASON_API_UNSUPPORTED"),
    ])
    result = fallback_log_check.analyze(config, log)
    assert result["failures"] == []
    assert result["pass"] is True


def test_fallback_fails_while_the_status_method_is_absent() -> None:
    config = fallback_log_check.configuration("vulkan_fg")
    result = fallback_log_check.analyze(config, _status_line(False))
    assert result["pass"] is False
    failures = result["failures"]
    assert isinstance(failures, list)
    assert any("absent" in failure for failure in failures)
    assert fallback_log_check.analyze(config, "no probe output")["pass"] is False


@pytest.mark.parametrize("lines", [0, 2])
def test_fallback_needs_exactly_one_log_line(lines: int) -> None:
    config = fallback_log_check.configuration("vulkan_fg")
    log = ["Frame generation fell back: API_UNSUPPORTED"] * lines
    log.append(_status_line(True, reason=1, reason_name="API_UNSUPPORTED"))
    result = fallback_log_check.analyze(config, "\n".join(log))
    assert result["reason_log_lines"] == lines
    assert result["pass"] is False


def test_fallback_mfg_on_ada_runs_fg_2x() -> None:
    config = fallback_log_check.configuration("mfg_on_ada")
    assert config.override_body() == (
        '[rendering]\nframe_generation/provider="DLSS-G"\nframe_generation/multiplier=4\n'
    )
    line = "DLSS-G: MFG_REQUIRES_RTX50, running FG 2x."
    good = _status_line(True, reason=3, reason_name="MFG_REQUIRES_RTX50", multiplier=2)
    assert fallback_log_check.analyze(config, line + "\n" + good)["pass"] is True
    wrong = _status_line(True, reason=3, reason_name="MFG_REQUIRES_RTX50", multiplier=4)
    assert fallback_log_check.analyze(config, line + "\n" + wrong)["pass"] is False
    other = _status_line(True, reason=0, reason_name="OK", multiplier=2)
    assert fallback_log_check.analyze(config, line + "\n" + other)["pass"] is False


def test_fallback_identical_frames(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    for name in ("off", "no_dlss"):
        folder = tmp_path / name
        folder.mkdir()
        for index in range(3):
            (folder / f"frame{index:08d}.png").write_bytes(bytes([index, 1, 2, 3]))
    assert fallback_log_check.identical(tmp_path / "off", tmp_path / "no_dlss")["pass"] is True
    (tmp_path / "no_dlss" / "frame00000001.png").write_bytes(bytes([1, 1, 2, 4]))
    result = fallback_log_check.identical(tmp_path / "off", tmp_path / "no_dlss")
    assert result["differing"] == ["frame00000001.png"]
    assert result["pass"] is False
    code = fallback_log_check.main(["identical", "--in", str(tmp_path / "off"), "--ref", str(tmp_path / "empty")])
    assert code == 2
    assert json.loads(capsys.readouterr().out)["pass"] is False


FAKE_GODOT = """
import json, pathlib, sys
args = sys.argv[1:]
testbed = pathlib.Path(args[args.index("--path") + 1])
override = (testbed / "override.cfg").read_text()
driver = args[args.index("--rendering-driver") + 1]
print("fake godot", driver, args[-1])
if "frame_generation/provider" in override and driver == "vulkan":
    print("Frame generation: API_UNSUPPORTED on Vulkan.")
print("GODOTNEXT_FG_STATUS " + json.dumps({"available": True, "status": {"reason": 1}, "reason_name": "API_UNSUPPORTED"}))
"""


def test_fallback_run_writes_and_removes_the_override(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    testbed = tmp_path / "testbed"
    testbed.mkdir()
    script = tmp_path / "fake_godot.py"
    script.write_text(FAKE_GODOT, encoding="utf-8")
    godot = _launcher(tmp_path, "godot", script)
    code = fallback_log_check.main(
        [
            "run",
            "--godot",
            str(godot),
            "--testbed",
            str(testbed),
            "--config",
            "vulkan_fg",
            "--out",
            str(tmp_path / "out"),
        ]
        + ["--", "--gpu-index", "0"]
    )
    report = json.loads(capsys.readouterr().out)
    assert code == 0 and report["pass"] is True, report
    assert not (testbed / "override.cfg").exists()
    log = (tmp_path / "out" / "vulkan_fg" / "run.log").read_text(encoding="utf-8")
    assert fallback_log_check.PROBE_SCENE in log
    (testbed / "override.cfg").write_text("[application]\n", encoding="utf-8")
    code = fallback_log_check.main([
        "run",
        "--godot",
        str(godot),
        "--testbed",
        str(testbed),
        "--config",
        "vulkan_fg",
        "--out",
        str(tmp_path / "out"),
    ])
    assert code == 2
    assert "never overwrites" in json.loads(capsys.readouterr().out)["error"]
    assert (testbed / "override.cfg").read_text(encoding="utf-8") == "[application]\n"
