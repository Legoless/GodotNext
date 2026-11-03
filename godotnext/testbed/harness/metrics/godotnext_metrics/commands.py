"""Load a harness capture directory and run one metric.

The harness files (``hdr_*.exr``, ``frame*.png``, ``timing.csv``, ``summary.json``,
``manifest.json``) and the scene patch JSON are read as those issues defined
them. Counters, velocity, masks, spp and the direct-only renders are sidecar
files. ``docs/godotnext/metrics.md`` lists each one.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from godotnext_metrics.counters import (
    q2_card_hit_coverage,
    q14_fallback_coverage,
    q15_geometry_memory,
    q16_clamped_energy,
    q18_rt_coverage,
)
from godotnext_metrics.image_metrics import q7_flip, q9_reflection_error, q10_ghost_trail
from godotnext_metrics.images import indexed_paths, read_exr, read_png
from godotnext_metrics.indirect import (
    q1_indirect_diffuse_error,
    q3_leak_ratio,
    q4a_step_convergence,
    q4b_continuous_convergence,
    q8_temporal_sigma,
)
from godotnext_metrics.jsonutil import as_dict, as_float, as_int, as_list
from godotnext_metrics.pixels import q5_crack_pixels, q6_hole_pixels
from godotnext_metrics.q19_upscaler_flip import q19_upscaler_flip, select_q19_frames
from godotnext_metrics.q20_final_indirect import q20_final_indirect
from godotnext_metrics.q21_invalid_mv import q21_invalid_motion_vectors
from godotnext_metrics.rays import load_analytic, q17_ray_query_parity, read_records
from godotnext_metrics.result import MetricInputError, MetricResult
from godotnext_metrics.timing_metrics import (
    TimingRun,
    budgets_for_host,
    q11_pass_time,
    q12_frame_time,
    q13_overhead,
    read_timing_csv,
    steady_state_total_p95,
)

SETTLING = 120
MEASURED = 64


def run_metric(metric: str, in_dir: Path, ref_dir: Path | None) -> MetricResult:
    key = metric.strip().lower()
    if key not in COMMANDS:
        known = ", ".join(sorted(COMMANDS))
        raise MetricInputError(metric, f"unknown metric {metric!r}; expected one of {known}")
    return COMMANDS[key](in_dir, ref_dir)


def run_q1(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q1", ref_dir)
    test_l = _average_window(_indirect_series(in_dir, "patches_q1.json", "Q1"), "Q1")
    ref_l = _average_window(_indirect_series(reference, "patches_q1.json", "Q1"), "Q1")
    meta = _read_json(reference / "reference.json", "Q1")
    return q1_indirect_diffuse_error(
        test_l,
        ref_l,
        spp=_optional_int(meta.get("spp")),
        per_patch_relative_se=_se_vector(meta, test_l.shape[0]),
    )


def run_q2(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    cameras = _counters(in_dir, "Q2")["q2"]
    if not isinstance(cameras, dict):
        raise MetricInputError("Q2", "counters.json q2 must be an object of CovCam rows")
    return q2_card_hit_coverage({str(name): dict(row) for name, row in cameras.items()})


def run_q3(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q3", ref_dir)
    dark, lit = _room_ids(in_dir)
    test = _mean_over_frames(_indirect_series(in_dir, "patches_q3.json", "Q3"))
    ref = _mean_over_frames(_indirect_series(reference, "patches_q3.json", "Q3"))
    return q3_leak_ratio(test[dark], test[lit], reference_dark=ref[dark], reference_lit=ref[lit])


def run_q4a(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    series = _indirect_series(in_dir, "patches_q1.json", "Q4a")
    for frame in (10, 32, 240):
        if frame not in series:
            raise MetricInputError("Q4a", f"missing indirect frame {frame}")
    return q4a_step_convergence(series[10], series[32], series[240])


def run_q4b(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q4b", ref_dir)
    config = _read_json(in_dir / "q4.json", "Q4b")
    spp = _optional_int(config.get("spp"))
    if spp is None and (reference / "reference.json").is_file():
        spp = _optional_int(_read_json(reference / "reference.json", "Q4b").get("spp"))
    return q4b_continuous_convergence(
        _indirect_series(in_dir, "patches_q1.json", "Q4b"),
        _indirect_series(reference, "patches_q1.json", "Q4b"),
        motion_start=as_int(config["motion_start"]),
        motion_end=as_int(config["motion_end"]),
        spp=spp,
    )


def run_q5(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    frames = _ldr_frames(in_dir, "Q5")
    coverage = read_png(in_dir / "coverage.png") if (in_dir / "coverage.png").is_file() else None
    clear = _clear_color(in_dir)
    # One image is the usual debug view. Several frames add their counts.
    total = 0
    magenta = 0
    passed = True
    for frame in frames:
        result = q5_crack_pixels(frame, coverage, clear_color=clear)
        total += as_int(result.value)
        magenta += as_int(result.values["magenta_pixels"])
        passed = passed and result.passed
    if len(frames) == 1:
        return q5_crack_pixels(frames[0], coverage, clear_color=clear)
    return MetricResult(
        metric="Q5",
        value=total,
        threshold=0,
        passed=passed,
        values={"crack_pixels": total, "magenta_pixels": magenta, "frames": len(frames)},
    )


def run_q6(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    if not (in_dir / "coverage.png").is_file():
        raise MetricInputError("Q6", "coverage.png is missing")
    return q6_hole_pixels(
        _ldr_frames(in_dir, "Q6"), read_png(in_dir / "coverage.png"), clear_color=_clear_color(in_dir)
    )


def run_q7(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q7", ref_dir)
    test_frames = _color_frames(in_dir, "Q7")
    ref_frames = _color_frames(reference, "Q7")
    pairs = _pair_frames(test_frames, ref_frames, "Q7")
    exposure = _exposure(in_dir)
    display = _uses_png(in_dir)
    if len(pairs) == 1:
        return q7_flip(pairs[0][1], pairs[0][0], exposure=exposure, already_display=display)
    means: list[float] = []
    p99s: list[float] = []
    for test, ref in pairs:
        result = q7_flip(ref, test, exposure=exposure, already_display=display)
        means.append(as_float(result.values["mean"]))
        p99s.append(as_float(result.values["p99"]))
    mean = float(np.mean(np.asarray(means)))
    return MetricResult(
        metric="Q7",
        value={"mean": mean, "p99": float(np.max(p99s))},
        threshold={"mean": 0.05},
        passed=bool(mean <= 0.05),
        values={"mean": mean, "p99": float(np.max(p99s)), "frames": len(pairs)},
    )


def run_q8(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    window = _window_matrix(_indirect_series(in_dir, "patches_q1.json", "Q8"), "Q8")
    return q8_temporal_sigma(window)


def run_q9(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q9", ref_dir)
    test = _single_color(in_dir, "Q9")
    ref = _single_color(reference, "Q9")
    roughness_path = in_dir / "roughness.exr"
    if not roughness_path.is_file():
        raise MetricInputError("Q9", "roughness.exr is missing")
    roughness = read_exr(roughness_path)[..., 0]
    return q9_reflection_error(ref, test, roughness, exposure=_exposure(in_dir))


def run_q10(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q10", ref_dir)
    frame = 4
    if (in_dir / "q10.json").is_file():
        frame = as_int(_read_json(in_dir / "q10.json", "Q10")["frame"])
    test_frames = _color_frames(in_dir, "Q10")
    ref_frames = _color_frames(reference, "Q10")
    if frame not in test_frames or frame not in ref_frames:
        raise MetricInputError("Q10", f"frame {frame} is missing from the test or the reference")
    mask_path = in_dir / "disocclusion.png"
    if not mask_path.is_file():
        raise MetricInputError("Q10", "disocclusion.png is missing")
    return q10_ghost_trail(ref_frames[frame], test_frames[frame], read_png(mask_path))


def run_q11(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    runs = _timing_runs(in_dir, "Q11")
    host = _manifest_host(in_dir)
    budgets = budgets_for_host(host)
    high_total: float | None = None
    line_budget: float | None = None
    if (in_dir / "q11.json").is_file():
        config = _read_json(in_dir / "q11.json", "Q11")
        extra = config.get("line_1b_columns", [])
        if not isinstance(extra, list):
            raise MetricInputError("Q11", "line_1b_columns must be a list")
        for run in runs:
            run.line_1b = frozenset(set(run.line_1b) | {str(name) for name in extra})
        if "high_total_ms" in config:
            high_total = as_float(config["high_total_ms"])
            line_budget = as_float(config["line_1b_budget_ms"])
    if high_total is None and budgets is not None:
        high_total, line_budget = budgets
    return q11_pass_time(runs, high_total_ms=high_total, line_1b_budget_ms=line_budget)


def run_q12(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    runs = _timing_runs(in_dir, "Q12")
    return q12_frame_time(runs[0])


def run_q13(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    baseline = _need_ref("Q13", ref_dir)
    test_runs = _timing_runs(in_dir, "Q13")
    base_runs = _timing_runs(baseline, "Q13")
    return q13_overhead(
        [steady_state_total_p95(run) for run in test_runs], [steady_state_total_p95(run) for run in base_runs]
    )


def run_q14(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    row = as_dict(_counters(in_dir, "Q14")["q14"])
    return q14_fallback_coverage(as_float(row["legacy_drawn_virtualized_pixels"]), as_float(row["opaque_pixels"]))


def run_q15(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    row = as_dict(_counters(in_dir, "Q15")["q15"])
    categories = {name: as_float(value) for name, value in as_dict(row["categories"]).items()}
    baseline = row.get("baseline_total")
    return q15_geometry_memory(categories, baseline_total=None if baseline is None else as_float(baseline))


def run_q16(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    row = as_dict(_counters(in_dir, "Q16")["q16"])
    return q16_clamped_energy(as_float(row["clamped_energy"]), as_float(row["total_indirect_energy"]))


def run_q17(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    rays = in_dir / "rays.bin"
    analytic_path = in_dir / "analytic.json"
    if not rays.is_file() or not analytic_path.is_file():
        raise MetricInputError("Q17", "rays.bin and analytic.json are required")
    got = read_records(rays)
    analytic = load_analytic(analytic_path)
    rel_t = 1e-5
    abs_bary = 1e-5
    if ref_dir is not None and (ref_dir / "rays.bin").is_file():
        # Across APIs the record under --ref is the other device's dump.
        other = read_records(ref_dir / "rays.bin")
        analytic = [_record_to_analytic(row) for row in other]
        rel_t = 1e-4
        abs_bary = 1e-4
        if len(got) != len(analytic):
            raise MetricInputError("Q17", "across-API record counts differ")
    return q17_ray_query_parity(got, analytic, rel_t=rel_t, abs_bary=abs_bary)


def run_q18(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    row = as_dict(_counters(in_dir, "Q18")["q18"])
    return q18_rt_coverage(as_float(row["tlas_instances"]), as_float(row["gi_opaque_instances_in_radius"]))


def run_q19(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q19", ref_dir)
    scenes = _q19_scene_dirs(in_dir, reference)
    if len(scenes) > 1:
        results = [_run_q19_scene(test, ref) for test, ref in scenes]
        accepted = [result for result in results if result.passed and result.values.get("mean") is not None]
        means = [as_float(result.values["mean"]) for result in accepted]
        cross = float(np.mean(np.asarray(means, dtype=np.float64))) if means else None
        return MetricResult(
            metric="Q19",
            value=cross,
            threshold={"reference_flip": 0.01},
            passed=bool(results) and all(result.passed for result in results),
            values={
                "cross_scene_mean": cross,
                "per_scene": {
                    str(result.values.get("scene", index)): result.values.get("mean")
                    for index, result in enumerate(results)
                },
            },
            details={"scenes": len(results), "reference_accepted": all(result.passed for result in results)},
        )
    return _run_q19_scene(in_dir, reference)


def run_q20(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    reference = _need_ref("Q20", ref_dir)
    test_l = _average_window(_indirect_series(in_dir, "patches_q1.json", "Q20"), "Q20")
    ref_l = _average_window(_indirect_series(reference, "patches_q1.json", "Q20"), "Q20")
    meta = _read_json(reference / "reference.json", "Q20")
    bins = _roughness_bins(in_dir, reference)
    return q20_final_indirect(
        test_l,
        ref_l,
        spp=_optional_int(meta.get("spp")),
        per_patch_relative_se=_se_vector(meta, test_l.shape[0]),
        roughness_bins=bins,
    )


def run_q21(in_dir: Path, ref_dir: Path | None) -> MetricResult:
    _ = ref_dir
    paths = indexed_paths(in_dir, "velocity_*.exr")
    if not paths:
        raise MetricInputError("Q21", "velocity_*.exr is missing")
    frames = {index: read_exr(path)[..., :2] for index, path in paths.items()}
    resets: list[int] = []
    if (in_dir / "q21.json").is_file():
        raw = _read_json(in_dir / "q21.json", "Q21").get("reset_frames", [])
        if not isinstance(raw, list):
            raise MetricInputError("Q21", "reset_frames must be a list")
        resets = [int(value) for value in raw]
    return q21_invalid_motion_vectors(frames, reset_frames=resets)


COMMANDS = {
    "q1": run_q1,
    "q2": run_q2,
    "q3": run_q3,
    "q4a": run_q4a,
    "q4b": run_q4b,
    "q5": run_q5,
    "q6": run_q6,
    "q7": run_q7,
    "q8": run_q8,
    "q9": run_q9,
    "q10": run_q10,
    "q11": run_q11,
    "q12": run_q12,
    "q13": run_q13,
    "q14": run_q14,
    "q15": run_q15,
    "q16": run_q16,
    "q17": run_q17,
    "q18": run_q18,
    "q19": run_q19,
    "q20": run_q20,
    "q21": run_q21,
}


def _run_q19_scene(in_dir: Path, reference: Path) -> MetricResult:
    config = _read_json(in_dir / "q19.json", "Q19") if (in_dir / "q19.json").is_file() else {"form": "static"}
    form = str(config.get("form", "static"))
    reset = _optional_int(config.get("reset_frame"))
    test_paths = _color_paths(in_dir)
    ref_paths = _color_paths(reference)
    msaa_paths = _color_paths(reference / "msaa4")
    indices = select_q19_frames(
        sorted(set(test_paths) & set(ref_paths) & set(msaa_paths)), form=form, reset_frame=reset
    )
    if form == "static" and len(indices) != 64:
        raise MetricInputError("Q19", f"static form needs 64 frames after settling, found {len(indices)}")
    if not indices:
        raise MetricInputError("Q19", "no frames left after the warm-up and reset exclusion")
    test_frames = [_load_color(test_paths[index]) for index in indices]
    ref_frames = [_load_color(ref_paths[index]) for index in indices]
    msaa_frames = [_load_color(msaa_paths[index]) for index in indices]
    masks = {path.stem.removeprefix("cmask_"): read_png(path) for path in sorted(in_dir.glob("cmask_*.png"))}
    manifest = _optional_json(reference / "manifest.json")
    resolution = None
    output_size = manifest.get("output_size") if manifest else None
    if manifest and isinstance(output_size, list) and len(output_size) == 2:
        resolution = (as_int(output_size[0]), as_int(output_size[1]))
        scale = manifest.get("scaling_3d_scale")
        mode = manifest.get("scaling_3d_mode")
        if scale is not None and as_float(scale) != 2.0:
            raise MetricInputError("Q19", f"reference scaling_3d_scale {scale} is not 2.0")
        if mode is not None and as_int(mode) != 0:
            raise MetricInputError("Q19", f"reference scaling_3d_mode {mode} is not bilinear (0)")
    result = q19_upscaler_flip(
        test_frames,
        ref_frames,
        msaa_frames,
        content_masks=masks or None,
        form=form,
        exposure=_exposure(in_dir),
        already_display=_uses_png(in_dir),
        scene=str(config.get("scene", in_dir.name)),
        output_resolution=resolution,
    )
    result.values["cross_scene_mean"] = result.values.get("mean")
    return result


def _q19_scene_dirs(in_dir: Path, reference: Path) -> list[tuple[Path, Path]]:
    children = [path for path in sorted(in_dir.iterdir()) if path.is_dir() and (path / "q19.json").is_file()]
    if not children or (in_dir / "q19.json").is_file():
        return [(in_dir, reference)]
    pairs: list[tuple[Path, Path]] = []
    for child in children:
        ref_child = reference / child.name
        if not ref_child.is_dir():
            raise MetricInputError("Q19", f"reference is missing scene {child.name}")
        pairs.append((child, ref_child))
    return pairs


def _indirect_series(directory: Path, patch_file: str, metric: str) -> dict[int, NDArray[np.float64]]:
    patch_ids = _patch_ids(directory, patch_file, metric)
    full = indexed_paths(directory, "hdr_*.exr")
    direct = indexed_paths(directory / "direct", "hdr_*.exr")
    if not full or not direct:
        raise MetricInputError(metric, "hdr_*.exr and direct/hdr_*.exr are required")
    masks = indexed_paths(directory, "mask_*.png")
    static = directory / "mask.png"
    if not masks and not static.is_file():
        raise MetricInputError(metric, "mask.png or mask_*.png is required")
    series: dict[int, NDArray[np.float64]] = {}
    for index, path in full.items():
        if index not in direct:
            continue
        indirect = read_exr(path)[..., :3].astype(np.float64) - read_exr(direct[index])[..., :3].astype(np.float64)
        mask_path = masks.get(index, static)
        if not mask_path.is_file():
            raise MetricInputError(metric, f"no patch mask for frame {index}")
        series[index] = _patch_means(indirect, _decode_ids(read_png(mask_path)), patch_ids, metric)
    if not series:
        raise MetricInputError(metric, "no frame has both a full and a direct-only EXR")
    return series


def _patch_ids(directory: Path, patch_file: str, metric: str) -> list[int]:
    path = directory / patch_file
    if not path.is_file():
        raise MetricInputError(metric, f"{patch_file} is missing")
    payload = _read_json(path, metric)
    return [as_int(as_dict(patch)["id"]) for patch in as_list(payload["patches"])]


def _room_ids(directory: Path) -> tuple[np.ndarray, np.ndarray]:
    payload = _read_json(directory / "patches_q3.json", "Q3")
    patches = [as_dict(patch) for patch in as_list(payload["patches"])]
    dark = [index for index, patch in enumerate(patches) if patch.get("room") == "dark"]
    lit = [index for index, patch in enumerate(patches) if patch.get("room") == "lit"]
    if len(dark) != 16 or len(lit) != 16:
        raise MetricInputError("Q3", f"expected 16 dark and 16 lit patches, found {len(dark)} and {len(lit)}")
    return np.asarray(dark, dtype=np.int64), np.asarray(lit, dtype=np.int64)


def _patch_means(
    indirect: NDArray[np.float64], ids: NDArray[np.int64], patch_ids: list[int], metric: str
) -> NDArray[np.float64]:
    from godotnext_metrics.color import luminance

    luma = luminance(indirect)
    means = np.empty(len(patch_ids), dtype=np.float64)
    for index, patch_id in enumerate(patch_ids):
        selected = luma[ids == patch_id]
        if selected.size == 0:
            raise MetricInputError(metric, f"patch {patch_id} has no mask pixels")
        means[index] = float(np.mean(selected))
    return means


def _decode_ids(mask: NDArray[np.uint8]) -> NDArray[np.int64]:
    if mask.ndim == 2:
        encoded = mask.astype(np.int64)
    else:
        encoded = mask[..., 0].astype(np.int64) + 256 * mask[..., 1].astype(np.int64)
    ids = encoded - 1
    ids[encoded == 0] = -1
    return ids


def _average_window(series: dict[int, NDArray[np.float64]], metric: str) -> NDArray[np.float64]:
    return np.mean(_window_matrix(series, metric), axis=0)


def _window_matrix(series: dict[int, NDArray[np.float64]], metric: str) -> NDArray[np.float64]:
    frames = [series[index] for index in range(SETTLING, SETTLING + MEASURED) if index in series]
    if len(frames) != MEASURED:
        raise MetricInputError(metric, f"need {MEASURED} frames starting at {SETTLING}, found {len(frames)}")
    return np.stack(frames, axis=0)


def _mean_over_frames(series: dict[int, NDArray[np.float64]]) -> NDArray[np.float64]:
    stacked = np.stack([series[index] for index in sorted(series)], axis=0)
    return np.mean(stacked, axis=0)


def _ldr_frames(directory: Path, metric: str) -> list[NDArray[np.uint8]]:
    paths = indexed_paths(directory, "frame*.png")
    if not paths:
        loose = sorted(directory.glob("frame*.png"))
        if not loose:
            raise MetricInputError(metric, "frame*.png is missing")
        return [read_png(path) for path in loose]
    return [read_png(paths[index]) for index in sorted(paths)]


def _color_frames(directory: Path, metric: str) -> dict[int, NDArray[np.generic]]:
    paths = _color_paths(directory)
    if not paths:
        raise MetricInputError(metric, "hdr_*.exr or frame*.png is missing")
    return {index: _load_color(path) for index, path in paths.items()}


def _color_paths(directory: Path) -> dict[int, Path]:
    exr = indexed_paths(directory, "hdr_*.exr")
    if exr:
        return exr
    return indexed_paths(directory, "frame*.png")


def _load_color(path: Path) -> NDArray[np.generic]:
    if path.suffix.lower() == ".exr":
        return read_exr(path)[..., :3]
    return read_png(path)


def _single_color(directory: Path, metric: str) -> NDArray[np.generic]:
    frames = _color_frames(directory, metric)
    return frames[sorted(frames)[0]]


def _pair_frames(
    test: dict[int, NDArray[np.generic]],
    reference: dict[int, NDArray[np.generic]],
    metric: str,
) -> list[tuple[NDArray[np.generic], NDArray[np.generic]]]:
    shared = sorted(set(test) & set(reference))
    if not shared:
        raise MetricInputError(metric, "test and reference have no frame index in common")
    return [(test[index], reference[index]) for index in shared]


def _uses_png(directory: Path) -> bool:
    return not any(directory.glob("hdr_*.exr")) and any(directory.glob("frame*.png"))


def _timing_runs(directory: Path, metric: str) -> list[TimingRun]:
    roots = _run_dirs(directory)
    runs: list[TimingRun] = []
    for root in roots:
        csv_path = root / "timing.csv"
        if not csv_path.is_file():
            raise MetricInputError(metric, f"{csv_path} is missing")
        summary = _optional_json(root / "summary.json")
        warmup = as_int(summary["warmup"]) if isinstance(summary, dict) and "warmup" in summary else None
        runs.append(TimingRun(columns=read_timing_csv(csv_path), warmup=warmup, summary=summary))
    if not runs:
        raise MetricInputError(metric, "timing.csv is missing")
    return runs


def _run_dirs(directory: Path) -> list[Path]:
    numbered = [path for path in sorted(directory.glob("run_*")) if path.is_dir()]
    if (directory / "timing.csv").is_file():
        return [directory, *numbered]
    return numbered


def _counters(directory: Path, metric: str) -> dict[str, object]:
    path = directory / "counters.json"
    if not path.is_file():
        raise MetricInputError(metric, "counters.json is missing")
    payload = _read_json(path, metric)
    return payload


def _se_vector(meta: dict[str, object], count: int) -> list[float] | None:
    raw = meta.get("per_patch_relative_se")
    if raw is None:
        return None
    if isinstance(raw, list):
        return [float(value) for value in raw]
    if isinstance(raw, dict):
        return [float(raw[str(index)]) if str(index) in raw else float(raw.get(index, 0.0)) for index in range(count)]
    raise MetricInputError("Q1", "per_patch_relative_se must be a list or an object")


def _roughness_bins(in_dir: Path, reference: Path) -> dict[str, tuple[NDArray[np.float64], NDArray[np.float64]]] | None:
    path = in_dir / "roughness.exr"
    if not path.is_file():
        return None
    from godotnext_metrics.color import luminance
    from godotnext_metrics.image_metrics import ROUGHNESS_BINS

    roughness = read_exr(path)[..., 0].astype(np.float64)
    test = _average_indirect_image(in_dir)
    ref = _average_indirect_image(reference)
    bins: dict[str, tuple[NDArray[np.float64], NDArray[np.float64]]] = {}
    for name, low, high, _gated in ROUGHNESS_BINS:
        if name.endswith("]"):
            selected = (roughness >= low) & (roughness <= high)
        else:
            selected = (roughness >= low) & (roughness < high)
        bins[name] = (luminance(test)[selected], luminance(ref)[selected])
    return bins


def _average_indirect_image(directory: Path) -> NDArray[np.float64]:
    full = indexed_paths(directory, "hdr_*.exr")
    direct = indexed_paths(directory / "direct", "hdr_*.exr")
    chosen = [index for index in range(SETTLING, SETTLING + MEASURED) if index in full and index in direct]
    if len(chosen) != MEASURED:
        raise MetricInputError("Q20", f"roughness bins need the {MEASURED}-frame window")
    acc = np.zeros_like(read_exr(full[chosen[0]])[..., :3], dtype=np.float64)
    for index in chosen:
        acc += read_exr(full[index])[..., :3].astype(np.float64) - read_exr(direct[index])[..., :3].astype(np.float64)
    return acc / float(len(chosen))


def _record_to_analytic(record: np.void) -> dict[str, object]:
    return {name: record[name].item() for name in record.dtype.names or ()}


def _clear_color(directory: Path) -> tuple[int, int, int]:
    payload = _optional_json(directory / "pixels.json")
    if not payload or "clear_color" not in payload:
        return (0, 0, 0)
    color = payload["clear_color"]
    if not isinstance(color, list) or len(color) != 3:
        raise MetricInputError("Q5", "clear_color must be three integers")
    return (int(color[0]), int(color[1]), int(color[2]))


def _exposure(directory: Path) -> float:
    payload = _optional_json(directory / "exposure.json")
    if not payload:
        return 1.0
    return as_float(payload["exposure"])


def _manifest_host(directory: Path) -> str | None:
    payload = _optional_json(directory / "manifest.json")
    if not payload:
        return None
    host = payload.get("host")
    return str(host) if isinstance(host, str) else None


def _need_ref(metric: str, ref_dir: Path | None) -> Path:
    if ref_dir is None:
        raise MetricInputError(metric, "--ref is required")
    return ref_dir


def _read_json(path: Path, metric: str) -> dict[str, object]:
    if not path.is_file():
        raise MetricInputError(metric, f"{path.name} is missing")
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise MetricInputError(metric, f"{path.name} must be an object")
    return payload


def _optional_json(path: Path) -> dict[str, object] | None:
    if not path.is_file():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must be an object")
    return payload


def _optional_int(value: object) -> int | None:
    if value is None:
        return None
    return as_int(value)
