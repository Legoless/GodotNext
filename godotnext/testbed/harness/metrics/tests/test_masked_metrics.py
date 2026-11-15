"""Masked Q8 and Q19 (#190): the mask option leaves the frozen metrics unchanged."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from godotnext_metrics.cli import main as cli_main
from godotnext_metrics.color import luminance
from godotnext_metrics.commands import run_metric
from godotnext_metrics.flip_eval import ldr_flip
from godotnext_metrics.images import read_exr, read_png, write_exr, write_png
from godotnext_metrics.indirect import q8_masked_temporal_sigma, q8_temporal_sigma
from godotnext_metrics.masks import MaskError, coverage_report, load_mask_frames
from godotnext_metrics.q19_upscaler_flip import q19_upscaler_flip
from godotnext_metrics.result import MetricInputError, MetricResult

_METRICS_DIR = Path(__file__).resolve().parents[1]

# Unmasked outputs of the frozen metrics on the captures below, recorded with the metrics as #14
# and #95 left them (before the mask option), on x86-64 Linux. The keys and every non-float value
# must match exactly; floats within 1e-6, since FLIP's last digits can differ on another CPU. The
# byte-for-byte check compares each command's output with the frozen function run in-process.
_GOLDEN_Q8_CLI = (
    '{"details": {"frame_count_ok": true}, "metric": "Q8", "pass": false, "threshold": 0.015, '
    '"value": 0.016272419341223185, "values": {"frames": 64, "max_relative_sigma": 0.024068991468624592, '
    '"mean_relative_sigma": 0.016272419341223185}}'
)
_GOLDEN_Q19_CLI_STATIC = (
    '{"details": {"excluded_masks": [], "frames": 64, "reference_accepted": true}, "metric": "Q19", '
    '"pass": true, "threshold": {"reference_flip": 0.01}, "value": 0.1594076156616211, "values": '
    '{"convergence_flip": 0.002795738026179606, "cross_scene_mean": 0.1594076156616211, "form": "static", '
    '"mean": 0.1594076156616211, "output_resolution": [16, 16], "p99": 0.6449143761396409, "per_mask": {}, '
    '"scene": "taa"}}'
)
_GOLDEN_Q19_SCRIPT_MOVING = (
    '{"details": {"excluded_masks": [], "frames": 34, "reference_accepted": true}, "metric": "Q19", '
    '"pass": true, "threshold": {"reference_flip": 0.01}, "value": 0.1590388561872875, "values": '
    '{"convergence_flip": 0.0033048519176458392, "form": "moving", "mean": 0.1590388561872875, '
    '"output_resolution": [16, 16], "p99": 0.643632411956787, "per_mask": {}, "scene": "taa"}}'
)
# The masked values as #95 computed them, with one mask image or a full per-frame run.
_GOLDEN_Q19_CLI_CMASK_THIN = 0.37552861602231735
_GOLDEN_Q19_SCRIPT_MASK_THIN = 0.37649683401290457


def _build_q8_capture(root: Path) -> Path:
    capture = root / "q8"
    height, width = 6, 8
    ids = np.zeros((height, width), dtype=np.uint8)
    ids[0:3, 0:4] = 1
    ids[3:6, 4:8] = 2
    ids[5, 0] = 3
    write_png(capture / "mask.png", ids)
    patches = [{"id": index, "pos": [0.0, 0.0, 0.0], "normal": [0.0, 1.0, 0.0], "radius": 0.25} for index in range(3)]
    (capture / "patches_q1.json").write_text(json.dumps({"version": 1, "patches": patches}) + "\n", encoding="utf-8")
    yy, xx = np.mgrid[0:height, 0:width]
    for frame in range(120, 184):
        phase = (frame - 120) / 64.0
        base = 0.4 + 0.05 * xx + 0.03 * yy
        wobble = 0.02 * np.sin(2.0 * np.pi * phase * (1 + (xx + yy) % 3))
        full = np.stack([base + wobble, base * 0.9 + wobble, base * 0.8 - wobble], axis=-1).astype(np.float32)
        direct = (full * 0.25).astype(np.float32)
        write_exr(capture / f"hdr_{frame:05d}.exr", full)
        write_exr(capture / "direct" / f"hdr_{frame:05d}.exr", direct)
    return capture


def _build_q19_tree(root: Path) -> tuple[Path, Path, Path, Path]:
    test_dir = root / "tree" / "taa"
    ref_dir = root / "tree" / "ssaa_msaa8"
    msaa4_dir = ref_dir / "msaa4"
    mask_dir = root / "tree" / "mask_thin"
    yy, xx = np.mgrid[0:16, 0:16]
    for frame in range(118, 186):
        reference = np.full((16, 16, 3), 90, dtype=np.uint8)
        reference[:, 8:, :] = 170
        reference[(xx + yy + frame) % 7 == 0] = 230
        msaa4 = reference.copy()
        msaa4[2, 3, :] = 95
        test = reference.copy()
        test[4:12, 4:12, :] = np.uint8(110 + frame % 5)
        mask = np.zeros((16, 16), dtype=np.uint8)
        mask[5:9, 2:14] = 255
        write_png(ref_dir / f"frame{frame:08d}.png", reference)
        write_png(msaa4_dir / f"frame{frame:08d}.png", msaa4)
        write_png(test_dir / f"frame{frame:08d}.png", test)
        write_png(mask_dir / f"frame{frame:08d}.png", mask)
    (ref_dir / "manifest.json").write_text(
        json.dumps({"scaling_3d_mode": 0, "scaling_3d_scale": 2.0, "output_size": [16, 16]}) + "\n", encoding="utf-8"
    )
    return test_dir, ref_dir, msaa4_dir, mask_dir


def _run(*args: str) -> tuple[int, str]:
    env = dict(os.environ)
    env["PYTHONPATH"] = str(_METRICS_DIR) + os.pathsep + env.get("PYTHONPATH", "")
    completed = subprocess.run(
        [sys.executable, *args], check=False, capture_output=True, text=True, cwd=_METRICS_DIR, env=env
    )
    return completed.returncode, completed.stdout.strip()


def _close(actual: object, expected: object) -> None:
    if isinstance(expected, float):
        assert isinstance(actual, float) and actual == pytest.approx(expected, rel=1e-6)
    elif isinstance(expected, dict):
        assert isinstance(actual, dict) and set(actual) == set(expected)
        for key, value in expected.items():
            _close(actual[key], value)
    elif isinstance(expected, list):
        assert isinstance(actual, list) and len(actual) == len(expected)
        for item, value in zip(actual, expected, strict=True):
            _close(item, value)
    else:
        assert type(actual) is type(expected) and actual == expected


def _frozen_json(result: MetricResult) -> str:
    return json.dumps(result.to_json(), sort_keys=True, allow_nan=False)


def test_mask_option_leaves_unmasked_outputs_byte_identical(tmp_path: Path) -> None:
    q8 = _build_q8_capture(tmp_path)
    test_dir, ref_dir, msaa4_dir, _mask_dir = _build_q19_tree(tmp_path)

    # Q8: the frozen per-patch function on the patch means, run in-process.
    code, text = _run("-m", "godotnext_metrics", "q8", "--in", str(q8), "--json")
    ids = read_png(q8 / "mask.png").astype(np.int64) - 1
    window = []
    for frame in range(120, 184):
        full = read_exr(q8 / f"hdr_{frame:05d}.exr")[..., :3].astype(np.float64)
        direct = read_exr(q8 / "direct" / f"hdr_{frame:05d}.exr")[..., :3].astype(np.float64)
        luma = luminance(full - direct)
        window.append([float(np.mean(luma[ids == patch])) for patch in range(3)])
    assert code == 0 and text == _frozen_json(q8_temporal_sigma(np.asarray(window)))
    _close(json.loads(text), json.loads(_GOLDEN_Q8_CLI))

    # Q19 (#95 script, moving form) and the Q19 command (static form): the frozen function in-process.
    script = str(_METRICS_DIR / "q19_upscaler_flip.py")
    moving = ("--test", str(test_dir), "--reference", str(ref_dir), "--msaa4", str(msaa4_dir), "--form", "moving")
    code, text = _run(script, *moving, "--reset-frame", "150")
    kept = [*range(120, 150), *range(182, 186)]

    def frames(directory: Path, indices: list[int]) -> list[np.ndarray]:
        return [read_png(directory / f"frame{index:08d}.png") for index in indices]

    expected = q19_upscaler_flip(
        frames(test_dir, kept),
        frames(ref_dir, kept),
        frames(msaa4_dir, kept),
        form="moving",
        already_display=True,
        scene="taa",
        output_resolution=(16, 16),
    )
    assert code == 0 and text == _frozen_json(expected)
    _close(json.loads(text), json.loads(_GOLDEN_Q19_SCRIPT_MOVING))

    code, text = _run("-m", "godotnext_metrics", "q19", "--in", str(test_dir), "--ref", str(ref_dir), "--json")
    static = list(range(120, 184))
    expected = q19_upscaler_flip(
        frames(test_dir, static),
        frames(ref_dir, static),
        frames(msaa4_dir, static),
        form="static",
        already_display=True,
        scene="taa",
        output_resolution=(16, 16),
    )
    expected.values["cross_scene_mean"] = expected.values["mean"]
    assert code == 0 and text == _frozen_json(expected)
    _close(json.loads(text), json.loads(_GOLDEN_Q19_CLI_STATIC))


def test_mask_scores_match_the_frozen_per_mask_values(tmp_path: Path) -> None:
    """One mask image (CLI cmask) and a full per-frame run (script) give #95's masked means."""
    test_dir, ref_dir, msaa4_dir, mask_dir = _build_q19_tree(tmp_path)
    cmask = np.zeros((16, 16), dtype=np.uint8)
    cmask[5:9, 2:14] = 255
    write_png(test_dir / "cmask_thin.png", cmask)
    code, text = _run("-m", "godotnext_metrics", "q19", "--in", str(test_dir), "--ref", str(ref_dir), "--json")
    payload = json.loads(text)
    assert code == 0
    assert payload["values"]["per_mask"] == {"thin": pytest.approx(_GOLDEN_Q19_CLI_CMASK_THIN, rel=1e-6)}
    assert payload["details"]["mask_pixels"] == {"thin": {"frames": 64, "mean": 48.0, "min": 48}}
    script = str(_METRICS_DIR / "q19_upscaler_flip.py")
    code, text = _run(
        script,
        *("--test", str(test_dir), "--reference", str(ref_dir), "--msaa4", str(msaa4_dir)),
        *("--form", "moving", "--reset-frame", "150", "--mask", str(mask_dir)),
    )
    payload = json.loads(text)
    assert code == 0
    assert payload["values"]["per_mask"] == {"thin": pytest.approx(_GOLDEN_Q19_SCRIPT_MASK_THIN, rel=1e-6)}
    assert payload["values"]["mean"] == pytest.approx(json.loads(_GOLDEN_Q19_SCRIPT_MOVING)["values"]["mean"], rel=1e-6)


def _flip_frames(count: int) -> tuple[list[np.ndarray], list[np.ndarray], list[np.ndarray]]:
    rng = np.random.default_rng(190)
    references = [rng.uniform(0.05, 0.9, size=(16, 16, 3)).astype(np.float32) for _ in range(count)]
    tests = [np.clip(ref + rng.normal(0.0, 0.05, size=ref.shape), 0.0, 1.0).astype(np.float32) for ref in references]
    return tests, references, [ref.copy() for ref in references]


def test_mask_q19_one_pixel_mask_reproduces_that_pixel_of_the_unmasked_error_map() -> None:
    tests, references, msaa4 = _flip_frames(3)
    one = np.zeros((16, 16), dtype=np.uint8)
    one[7, 4] = 255
    result = q19_upscaler_flip(tests, references, msaa4, content_masks={"one": one}, form="moving")
    maps = [
        ldr_flip(ref, test, exposure=1.0, already_display=False)[2].reshape(16, 16)
        for test, ref in zip(tests, references, strict=True)
    ]
    expected = float(np.mean(np.asarray([error_map[7, 4] for error_map in maps], dtype=np.float64)))
    per_mask = result.values["per_mask"]
    assert isinstance(per_mask, dict)
    assert per_mask["one"] == expected
    # The unmasked mean is the mean of the per-frame map means, unchanged by the mask.
    unmasked = q19_upscaler_flip(tests, references, msaa4, form="moving")
    assert result.values["mean"] == unmasked.values["mean"]
    assert result.values["p99"] == unmasked.values["p99"]


def test_mask_q19_per_frame_masks_follow_each_frame() -> None:
    tests, references, msaa4 = _flip_frames(2)
    first = np.zeros((16, 16), dtype=bool)
    first[2, 3] = True
    second = np.zeros((16, 16), dtype=bool)
    second[13, 10] = True
    result = q19_upscaler_flip(tests, references, msaa4, content_masks={"moving": [first, second]}, form="moving")
    maps = [
        ldr_flip(ref, test, exposure=1.0, already_display=False)[2].reshape(16, 16)
        for test, ref in zip(tests, references, strict=True)
    ]
    expected = float(np.mean(np.asarray([maps[0][2, 3], maps[1][13, 10]], dtype=np.float64)))
    assert result.values["per_mask"] == {"moving": expected}
    with pytest.raises(ValueError, match="per-frame content mask"):
        q19_upscaler_flip(tests, references, msaa4, content_masks={"short": [first]}, form="moving")


def test_mask_q8_one_pixel_mask_reproduces_the_unmasked_one_pixel_patch(tmp_path: Path) -> None:
    capture = _build_q8_capture(tmp_path)
    # Patch 2 (gray value 3) is the single pixel (row 5, column 0). Make it the only patch.
    (capture / "patches_q1.json").write_text(
        json.dumps({"version": 1, "patches": [{"id": 2, "pos": [0.0, 0.0, 0.0], "normal": [0.0, 1.0, 0.0]}]}) + "\n",
        encoding="utf-8",
    )
    unmasked = run_metric("q8", capture, None)
    one = np.zeros((6, 8), dtype=np.uint8)
    one[5, 0] = 255
    write_png(capture / "cmask_one.png", one)
    masked = run_metric("q8", capture, None, "one")
    assert masked.value == unmasked.value
    assert masked.values["mask_as_patch"] == unmasked.value
    assert masked.values["masked_pixels"] == 1
    assert masked.details["luminance"] == "indirect"
    assert masked.details["patches"] == "pixels"
    # The same value straight from the frozen function on that pixel's indirect luminance.
    samples = []
    for frame in range(120, 184):
        full = read_exr(capture / f"hdr_{frame:05d}.exr")[..., :3].astype(np.float64)
        direct = read_exr(capture / "direct" / f"hdr_{frame:05d}.exr")[..., :3].astype(np.float64)
        samples.append(luminance(full - direct)[5, 0])
    assert q8_temporal_sigma(np.asarray(samples)).value == masked.value


def test_mask_q8_on_final_images_without_direct_renders(tmp_path: Path) -> None:
    exr_capture = tmp_path / "exr"
    png_capture = tmp_path / "png"
    rng = np.random.default_rng(8)
    static = rng.uniform(0.1, 0.8, size=(10, 14, 3))
    for frame in range(100, 200):
        flicker = static.copy()
        flicker[:, 7:, :] *= 1.0 + 0.04 * (1 if frame % 2 else -1)
        write_exr(exr_capture / f"hdr_{frame:05d}.exr", flicker.astype(np.float32))
        write_png(png_capture / f"frame{frame:08d}.png", (flicker * 255.0).round().astype(np.uint8))
    left = np.zeros((10, 14), dtype=np.uint8)
    left[:, :7] = 255
    right = 255 - left
    for capture in (exr_capture, png_capture):
        write_png(capture / "cmask_left.png", left)
        write_png(capture / "cmask_right.png", right)
        steady = run_metric("q8", capture, None, "left")
        flickering = run_metric("q8", capture, None, "right")
        everything = run_metric("q8", capture, None, "frame")
        assert steady.value == pytest.approx(0.0, abs=1e-12)
        assert isinstance(flickering.value, float) and flickering.value > 0.02
        assert everything.values["masked_pixels"] == 140
        assert everything.values["frames"] == 64
    assert run_metric("q8", exr_capture, None, "left").details["luminance"] == "hdr"
    assert run_metric("q8", png_capture, None, "left").details["luminance"] == "png"
    # Per pixel, the frozen function on linear luminance gives the EXR value.
    right_pixels = [
        luminance(read_exr(exr_capture / f"hdr_{frame:05d}.exr")[..., :3].astype(np.float64))[:, 7:].reshape(-1)
        for frame in range(120, 184)
    ]
    expected = q8_masked_temporal_sigma(np.stack(right_pixels), mask_name="right")
    assert run_metric("q8", exr_capture, None, "right").value == expected.value


def test_mask_q8_reads_a_mask_run_beside_the_capture(tmp_path: Path) -> None:
    capture = tmp_path / "upscaler_torture_static" / "taa"
    mask_run = tmp_path / "upscaler_torture_static" / "mask_moire"
    for frame in range(0, 200):
        image = np.full((6, 6, 3), 0.25 + 0.01 * (frame % 3), dtype=np.float32)
        image[:, :3, :] = 0.5
        write_exr(capture / f"hdr_{frame:05d}.exr", image)
        mask = np.zeros((6, 6), dtype=np.uint8)
        mask[:, :3] = 255
        write_png(mask_run / f"frame{frame:08d}.png", mask)
    result = run_metric("q8", capture, None, "moire")
    assert result.value == pytest.approx(0.0, abs=1e-12)
    assert result.values["masked_pixels"] == 18
    assert result.details["mask_frames_identical"] is True
    (mask_run / "frame00000150.png").unlink()
    with pytest.raises(MetricInputError, match="no frame 150"):
        run_metric("q8", capture, None, "moire")


def test_mask_cli_rules(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    capture = _build_q8_capture(tmp_path)
    assert cli_main(["q7", "--in", str(capture), "--ref", str(capture), "--mask", "moire"]) == 2
    assert "--mask applies to q19, q8" in json.loads(capsys.readouterr().out)["details"]["error"]
    assert cli_main(["q8", "--in", str(capture), "--mask", "absent"]) == 2
    assert "cannot find mask 'absent'" in json.loads(capsys.readouterr().out)["details"]["error"]
    empty = np.zeros((6, 8), dtype=np.uint8)
    write_png(capture / "cmask_empty.png", empty)
    assert cli_main(["q8", "--in", str(capture), "--mask", "empty"]) == 2
    assert "selects no pixel" in json.loads(capsys.readouterr().out)["details"]["error"]
    assert cli_main(["q8", "--in", str(capture), "--mask", "frame"]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["values"]["masked_pixels"] == 48


def test_mask_frames_check_size_and_union(tmp_path: Path) -> None:
    write_png(tmp_path / "cmask_small.png", np.full((3, 3), 255, dtype=np.uint8))
    with pytest.raises(MaskError, match="3x3"):
        load_mask_frames(tmp_path, "small", [120], (6, 8))
    frames = load_mask_frames(tmp_path, "frame", [120, 121], (6, 8))
    assert len(frames) == 2 and bool(np.all(frames[0]))


def test_mask_coverage_report(tmp_path: Path) -> None:
    run = tmp_path / "mask_thin_wire"
    for frame in range(4):
        mask = np.zeros((100, 100), dtype=np.uint8)
        mask[: frame + 1, :] = 255  # 1% to 4% of the frame
        write_png(run / f"frame{frame:08d}.png", mask)
    report = coverage_report(run)
    assert report["mask"] == "thin_wire"
    assert report["min"] == 0.01 and report["min_frame"] == 0
    assert report["max"] == 0.04
    assert report["pass"] is True
    assert coverage_report(run, minimum=0.02)["pass"] is False
    assert coverage_report(run, first=1, last=2)["frames"] == 2
    code, text = _run("-m", "godotnext_metrics.masks", "coverage", "--in", str(run))
    assert code == 0 and json.loads(text)["output_resolution"] == [100, 100]
    code, text = _run("-m", "godotnext_metrics.masks", "coverage", "--in", str(tmp_path / "absent"))
    assert code == 2
