"""Known answers and a pass/fail pair for every Q metric."""

from __future__ import annotations

import math

import numpy as np
import pytest
from godotnext_metrics.counters import (
    q2_card_hit_coverage,
    q14_fallback_coverage,
    q15_geometry_memory,
    q16_clamped_energy,
    q18_rt_coverage,
)
from godotnext_metrics.image_metrics import q7_flip, q9_reflection_error, q10_ghost_trail
from godotnext_metrics.indirect import (
    q1_indirect_diffuse_error,
    q1_patch_error,
    q3_leak_ratio,
    q4a_step_convergence,
    q4b_continuous_convergence,
    q8_temporal_sigma,
)
from godotnext_metrics.pixels import q5_crack_pixels, q6_hole_pixels
from godotnext_metrics.q19_upscaler_flip import q19_upscaler_flip, select_q19_frames
from godotnext_metrics.q20_final_indirect import q20_final_indirect
from godotnext_metrics.q21_invalid_mv import q21_invalid_count, q21_invalid_motion_vectors
from godotnext_metrics.rays import q17_ray_query_parity, records_from_dicts
from godotnext_metrics.result import json_ready
from godotnext_metrics.timing_metrics import TimingRun, q11_pass_time, q11_spread, q12_frame_time, q13_overhead


def _number(value: object) -> float:
    assert isinstance(value, (int, float))
    return float(value)


def _cameras(ratio: float) -> dict[str, dict[str, float]]:
    return {f"CovCam{index}": {"valid_card_hits": ratio, "hits_within_10m": 1.0} for index in range(4)}


def _ray(**overrides: object) -> dict[str, object]:
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
    row.update(overrides)
    return row


def _split_image() -> np.ndarray:
    image = np.zeros((16, 16, 3), dtype=np.float32)
    image[:, :8, :] = 1.0
    return image


def test_q1_constant_images_are_zero_and_ten_percent_is_point_one() -> None:
    exact = q1_indirect_diffuse_error([1.0, 1.0], [1.0, 1.0], spp=4096, per_patch_relative_se=[0.0, 0.0])
    assert exact.value == 0.0
    assert exact.passed
    scaled = q1_patch_error([1.1], [1.0], 1.0)
    assert scaled == pytest.approx(0.10)
    wrapped = q1_indirect_diffuse_error([1.1], [1.0], spp=4096, per_patch_relative_se=[0.0])
    assert wrapped.value == pytest.approx(0.10)
    assert wrapped.passed


def test_q1_fails_on_a_large_error_and_on_a_short_reference() -> None:
    large = q1_indirect_diffuse_error([2.0], [1.0], spp=4096, per_patch_relative_se=[0.0])
    assert large.value == pytest.approx(1.0)
    assert not large.passed
    short = q1_indirect_diffuse_error([1.0], [1.0], spp=100, per_patch_relative_se=[0.0])
    assert short.value == 0.0
    assert not short.passed


def test_q2_pass_and_fail() -> None:
    assert q2_card_hit_coverage(_cameras(0.85)).passed
    low = q2_card_hit_coverage(_cameras(0.5))
    assert low.value == pytest.approx(0.5)
    assert not low.passed
    missing = dict(_cameras(1.0))
    del missing["CovCam3"]
    assert not q2_card_hit_coverage(missing).passed


def test_q3_pass_fail_and_reference_flag() -> None:
    passed = q3_leak_ratio([0.001] * 16, [1.0] * 16, reference_dark=[0.0] * 16, reference_lit=[1.0] * 16)
    assert passed.value == pytest.approx(0.001)
    assert passed.passed
    leaked = q3_leak_ratio([0.02] * 16, [1.0] * 16)
    assert leaked.value == pytest.approx(0.02)
    assert not leaked.passed
    flagged = q3_leak_ratio([0.0] * 16, [1.0] * 16, reference_dark=[0.001] * 16, reference_lit=[1.0] * 16)
    assert flagged.details["reference_flagged"]
    assert not flagged.passed


def test_q4a_pass_and_fail() -> None:
    passed = q4a_step_convergence([1.2], [1.05], [1.0])
    assert passed.values["e10"] == pytest.approx(0.20)
    assert passed.values["e32"] == pytest.approx(0.05)
    assert passed.passed
    failed = q4a_step_convergence([2.0], [1.0], [1.0])
    assert failed.values["e10"] == pytest.approx(1.0)
    assert not failed.passed


def test_q4b_pass_and_fail() -> None:
    frames = {0: [1.0], 36: [1.0], 244: [1.0]}
    passed = q4b_continuous_convergence(frames, {0: [1.0]}, motion_start=0, motion_end=4, spp=1024)
    assert passed.values["mean_e_lag"] == pytest.approx(0.0)
    assert passed.passed
    lagging = {0: [2.0], 36: [1.0], 244: [1.0]}
    failed = q4b_continuous_convergence(lagging, {0: [1.0]}, motion_start=0, motion_end=4, spp=1024)
    assert failed.values["mean_e_lag"] == pytest.approx(1.0)
    assert not failed.passed


def test_q5_magenta_block_is_nine_and_a_clean_image_passes() -> None:
    magenta = np.zeros((3, 3, 3), dtype=np.uint8)
    magenta[..., 0] = 255
    magenta[..., 2] = 255
    cracks = q5_crack_pixels(magenta)
    assert cracks.value == 9
    assert not cracks.passed
    clean = np.full((3, 3, 3), 128, dtype=np.uint8)
    assert q5_crack_pixels(clean).passed


def test_q6_pass_and_fail() -> None:
    mask = np.ones((5, 5), dtype=np.uint8)
    white = np.full((5, 5, 3), 255, dtype=np.uint8)
    assert q6_hole_pixels(white, mask).value == 0
    black = np.zeros((5, 5, 3), dtype=np.uint8)
    holes = q6_hole_pixels(black, mask)
    assert holes.value == 1
    assert not holes.passed


def test_q7_pass_and_fail() -> None:
    image = np.full((8, 8, 3), 0.4, dtype=np.float32)
    same = q7_flip(image, image, already_display=True)
    assert same.values["mean"] == 0.0
    assert same.passed
    white = np.ones((8, 8, 3), dtype=np.float32)
    black = np.zeros((8, 8, 3), dtype=np.float32)
    different = q7_flip(white, black, already_display=True)
    assert _number(different.values["mean"]) > 0.05
    assert not different.passed


def test_q8_pass_and_fail() -> None:
    steady = np.ones((64, 2), dtype=np.float64)
    assert q8_temporal_sigma(steady).value == 0.0
    assert q8_temporal_sigma(steady).passed
    noisy = np.resize(np.array([1.0, 1.1], dtype=np.float64), (64, 1))
    failed = q8_temporal_sigma(noisy)
    assert _number(failed.value) > 0.015
    assert not failed.passed


def test_q9_pass_fail_and_mirror_bin_is_report_only() -> None:
    image = np.full((4, 4, 3), 0.2, dtype=np.float32)
    rough = np.full((4, 4), 0.2, dtype=np.float32)
    assert q9_reflection_error(image, image, rough).passed
    bright = np.ones((4, 4, 3), dtype=np.float32)
    failed = q9_reflection_error(image, bright, rough)
    assert not failed.passed
    mirror = np.full((4, 4), 0.05, dtype=np.float32)
    reported = q9_reflection_error(image, bright, mirror)
    assert reported.passed
    assert reported.details["mirror_bin_reported_only"]


def test_q10_pass_and_fail() -> None:
    image = np.full((4, 4, 3), 0.5, dtype=np.float32)
    mask = np.zeros((4, 4), dtype=np.uint8)
    mask[0, 0] = 255
    assert q10_ghost_trail(image, image, mask).value == 0.0
    other = np.ones((4, 4, 3), dtype=np.float32)
    failed = q10_ghost_trail(image, other, mask)
    assert _number(failed.value) > 0.10
    assert not failed.passed


def test_q11_two_percent_spread_and_a_wider_spread_fails() -> None:
    assert q11_spread([1.0, 1.0, 1.02]) == pytest.approx(0.02)
    tight = q11_pass_time([TimingRun(columns={"gi_ms": [value]}) for value in (1.0, 1.0, 1.02)])
    assert tight.value == pytest.approx(0.02)
    assert tight.passed
    wide = q11_pass_time([TimingRun(columns={"gi_ms": [value]}) for value in (1.0, 1.0, 1.05)])
    assert not wide.passed


def test_q11_excludes_line_1b_from_the_steady_total() -> None:
    run = TimingRun(columns={"gi_ms": [1.0, 1.0], "line_1b_ms": [50.0, 0.0]})
    result = q11_pass_time([run])
    assert result.values["steady_state_p95"] == [1.0]
    assert result.passed


def test_q12_pass_and_fail() -> None:
    samples: list[float | None] = [float(value) for value in range(1, 101)]
    matched = q12_frame_time(
        TimingRun(
            columns={"wall_ms": samples},
            summary={"columns": {"wall_ms": {"p95": 95.05, "p99": 99.01}}},
        )
    )
    assert matched.values["p95"] == pytest.approx(95.05)
    assert matched.values["p99"] == pytest.approx(99.01)
    assert matched.passed
    tampered = q12_frame_time(
        TimingRun(
            columns={"wall_ms": samples},
            summary={"columns": {"wall_ms": {"p95": 0.0, "p99": 0.0}}},
        )
    )
    assert not tampered.passed


def test_q13_pass_and_fail() -> None:
    assert q13_overhead([1.0, 1.0, 1.0], [1.0, 1.0, 1.0]).value == 0.0
    assert q13_overhead([1.0, 1.0, 1.0], [1.0, 1.0, 1.0]).passed
    overhead = q13_overhead([1.1, 1.1, 1.1], [1.0, 1.0, 1.0])
    assert overhead.value == pytest.approx(0.1)
    assert not overhead.passed


def test_q14_pass_and_fail() -> None:
    assert q14_fallback_coverage(10, 100).passed
    at_ceiling = q14_fallback_coverage(20, 100)
    assert at_ceiling.value == pytest.approx(0.20)
    assert not at_ceiling.passed


def test_q15_pass_fail_and_missing_category() -> None:
    categories = {"MEMORY_ACCELERATION_STRUCTURES": 10.0, "other": 90.0}
    assert q15_geometry_memory(categories, baseline_total=None).passed
    over_baseline = q15_geometry_memory(categories, baseline_total=100.0)
    assert not over_baseline.passed
    missing = q15_geometry_memory({"other": 1.0}, baseline_total=None)
    assert missing.value is None
    assert not missing.passed


def test_q16_pass_and_fail() -> None:
    assert q16_clamped_energy(1.0, 100.0).passed
    assert not q16_clamped_energy(5.0, 100.0).passed


def test_q17_tolerance_and_double_hit() -> None:
    expected = [_ray()]
    got_t = float(np.float32(1.0 + 1e-5))
    relative = abs(got_t - 1.0)
    assert 1e-6 < relative <= 1e-4
    shifted = records_from_dicts([_ray(t=got_t)])
    assert q17_ray_query_parity(shifted, expected, rel_t=1e-4, abs_bary=1e-4).passed
    assert not q17_ray_query_parity(shifted, expected, rel_t=1e-6, abs_bary=1e-4).passed
    exact = records_from_dicts([_ray()])
    assert q17_ray_query_parity(exact, expected).passed
    edge = [_ray(edge=True, adjacent_primitives=[7, 8], candidate_count=2)]
    double_hit = records_from_dicts([_ray(candidate_count=2)])
    assert not q17_ray_query_parity(double_hit, edge).passed


def test_q18_pass_and_fail() -> None:
    assert q18_rt_coverage(95, 100).passed
    assert not q18_rt_coverage(94, 100).passed


def test_q19_identical_shift_and_reference_rejection() -> None:
    image = _split_image()
    identical = q19_upscaler_flip([image], [image], [image], already_display=True)
    assert identical.value == 0.0
    assert identical.passed
    shifted = q19_upscaler_flip([np.roll(image, 1, axis=1)], [image], [image], already_display=True)
    assert _number(shifted.value) > 0.0
    assert shifted.passed
    white = np.ones((8, 8, 3), dtype=np.float32)
    black = np.zeros((8, 8, 3), dtype=np.float32)
    rejected = q19_upscaler_flip(
        [white], [white], [black], already_display=True, content_masks={"all": np.ones((8, 8), dtype=np.uint8)}
    )
    assert rejected.value is None
    assert not rejected.passed
    assert _number(rejected.values["convergence_flip"]) > 0.01
    excluded = rejected.details["excluded_masks"]
    assert isinstance(excluded, list)
    assert "all" in excluded


def test_q19_static_window_and_reset_exclusion() -> None:
    static = select_q19_frames(list(range(200)), form="static", reset_frame=None)
    assert static == list(range(120, 184))
    moving = select_q19_frames(list(range(300)), form="moving", reset_frame=200)
    assert 200 not in moving
    assert 231 not in moving
    assert 232 in moving
    assert 119 not in moving


def test_q20_reuses_q1_and_reports_ten_percent() -> None:
    error = q1_patch_error([1.1], [1.0], 1.0)
    result = q20_final_indirect([1.1], [1.0], spp=4096, per_patch_relative_se=[0.0])
    assert result.details["reuses_q1_patch_error"] is True
    assert result.value == pytest.approx(error)
    assert result.value == pytest.approx(0.10, abs=0.001)
    assert result.passed
    low_spp = q20_final_indirect([1.0], [1.0], spp=10, per_patch_relative_se=[0.0])
    assert not low_spp.passed
    large = q20_final_indirect([2.0], [1.0], spp=4096, per_patch_relative_se=[0.0])
    assert not large.passed


def test_q21_synthetic_dump_reports_two() -> None:
    velocity = np.zeros((1, 3, 2), dtype=np.float32)
    velocity[0, 0] = (-1.0, -1.0)
    velocity[0, 1] = (np.nan, 0.0)
    velocity[0, 2] = (-1.0001, -1.2)
    assert q21_invalid_count(velocity) == 2
    packed = np.zeros((1, 3, 4), dtype=np.float32)
    packed[0, :, :2] = velocity[0]
    assert q21_invalid_count(packed[..., :2]) == 2
    half = np.zeros((1, 1, 2), dtype=np.float16)
    half[0, 0] = (-1.0, -1.0)
    assert q21_invalid_count(half) == 1
    good = np.zeros((1, 1, 2), dtype=np.float32)
    reported = q21_invalid_motion_vectors({0: velocity, 1: good}, reset_frames=[0])
    assert reported.value == 0
    assert reported.passed
    resets = reported.values["reset_frames"]
    assert isinstance(resets, dict)
    assert resets[0] == 2
    failed = q21_invalid_motion_vectors({1: velocity})
    assert failed.value == 2
    assert not failed.passed


def test_non_finite_json_is_null() -> None:
    assert json_ready(math.inf) is None
    assert json_ready(math.nan) is None
