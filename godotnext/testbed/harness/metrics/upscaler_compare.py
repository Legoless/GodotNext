#!/usr/bin/env python3
"""GT-2 upscaler comparison report (#190): TAA, TSR, FSR2 and DLSS Q19, Q8 and Q10 per scene and scale.

``--in`` is a ``harness/runners/upscaler_jobs.sh`` output root, ``<root>/<WxH>/<scene>/<job>/``.
Each scene folder holds the upscaler captures, the ``ssaa_msaa8`` and ``ssaa_msaa4`` references
and the ``mask_<name>`` runs. Per output resolution, scene and configuration the report gives:

- Q19, the frozen FLIP mean against the reference: moving form (reset cut at frame 400) on
  ``upscaler_torture``, static form on ``upscaler_torture_static`` and the legacy scenes;
- thin-wire-mask Q19 on ``upscaler_torture``, frame by frame from ``mask_thin_wire``;
- static Q8 on ``upscaler_torture_static`` and the legacy scenes over the whole frame
  (``--mask frame``), and moiré-mask Q8 on ``upscaler_torture_static``, each with every pixel a
  patch and with the mask as one patch (``*_as_patch``);
- Q10, when the capture has the ``disocclusion.png`` (and optional ``q10.json``) that Q10 reads.

DLSS rows are report-only. A DLSS cell is filled only from a capture whose ``manifest.json`` has
the ``dlss`` object a DLSS-enabled run records, so a build without the DLSS SDK leaves them empty.
Nothing here is a pass or a fail: the GT-2 comparisons run in PQ with GT (#199). Exit 0 when the
report was written (cells may be empty), 2 when ``--in`` is missing.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from godotnext_metrics.commands import run_metric
from godotnext_metrics.q19_upscaler_flip import q19_upscaler_flip
from godotnext_metrics.result import MetricInputError, MetricResult, json_ready
from q19_upscaler_flip import (
    _check_reference_manifest,
    _exposure,
    _load_frame,
    _load_mask_entry,
    _select_modality,
)

SCHEMA = 1
REFERENCE_JOB = "ssaa_msaa8"
CONVERGENCE_JOB = "ssaa_msaa4"
RESET_FRAME = 400
THIN_WIRE_MASK = "thin_wire"
MOIRE_MASK = "moire"


@dataclass(frozen=True)
class Config:
    key: str
    upscaler: str
    preset: str
    scale: float
    job: str
    report_only: bool = False


CONFIGS: tuple[Config, ...] = (
    Config("taa_native", "TAA", "native", 1.0, "taa"),
    Config("fsr2_quality", "FSR2", "Quality", 0.667, "fsr2_quality"),
    Config("fsr2_performance", "FSR2", "Performance", 0.5, "fsr2"),
    Config("tsr_epic_native", "TSR", "Epic", 1.0, "tsr_epic_native"),
    Config("tsr_high_quality", "TSR", "High", 0.667, "tsr_high_quality"),
    Config("tsr_high_performance", "TSR", "High", 0.5, "tsr_high_performance"),
    Config("dlss_dlaa", "DLSS", "DLAA", 1.0, "dlss_dlaa", True),
    Config("dlss_quality", "DLSS", "Quality", 0.667, "dlss_quality", True),
    Config("dlss_performance", "DLSS", "Performance", 0.5, "dlss_performance", True),
)


@dataclass(frozen=True)
class Scene:
    name: str
    form: str
    thin_wire: bool = False
    q8: bool = False
    moire: bool = False


SCENES: tuple[Scene, ...] = (
    Scene("upscaler_torture", "moving", thin_wire=True),
    Scene("upscaler_torture_static", "static", q8=True, moire=True),
    Scene("legacy_sdfgi", "static", q8=True),
    Scene("legacy_voxelgi", "static", q8=True),
    Scene("legacy_lightmap", "static", q8=True),
)


def q19_cell(test_dir: Path, scene_dir: Path, scene: Scene) -> tuple[MetricResult, float | None]:
    """Q19 against the scene's references, plus the thin-wire-mask mean when the scene has one."""
    ref_dir = scene_dir / REFERENCE_JOB
    msaa4_dir = scene_dir / CONVERGENCE_JOB
    reset = RESET_FRAME if scene.form == "moving" else None
    resolution = _check_reference_manifest(ref_dir)
    indices, test_paths, ref_paths, msaa4_paths, uses_png = _select_modality(
        test_dir, ref_dir, msaa4_dir, form=scene.form, reset_frame=reset
    )
    if scene.form == "static" and len(indices) != 64:
        raise ValueError(f"static form needs 64 frames after settling, found {len(indices)}")
    masks = {}
    if scene.thin_wire and (scene_dir / f"mask_{THIN_WIRE_MASK}").is_dir():
        name, frames = _load_mask_entry(THIN_WIRE_MASK, indices, test_dir, ref_dir)
        masks[name] = frames
    result = q19_upscaler_flip(
        [_load_frame(test_paths[index]) for index in indices],
        [_load_frame(ref_paths[index]) for index in indices],
        [_load_frame(msaa4_paths[index]) for index in indices],
        content_masks=masks or None,
        form=scene.form,
        exposure=_exposure(test_dir),
        already_display=uses_png,
        scene=scene.name,
        output_resolution=resolution,
    )
    per_mask = result.values.get("per_mask")
    thin = per_mask.get(THIN_WIRE_MASK) if isinstance(per_mask, dict) else None
    return result, (float(thin) if thin is not None else None)


def _has_dlss_record(capture: Path) -> bool:
    manifest = capture / "manifest.json"
    if not manifest.is_file():
        return False
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    return isinstance(payload, dict) and isinstance(payload.get("dlss"), dict)


def build_cell(scene_dir: Path, scene: Scene, config: Config) -> dict[str, object]:
    capture = scene_dir / config.job
    cell: dict[str, object] = {
        "q19": None,
        "q19_p99": None,
        "q19_reference_accepted": None,
        "q19_thin_wire": None,
        "q8": None,
        "q8_as_patch": None,
        "q8_moire": None,
        "q8_moire_as_patch": None,
        "q10": None,
        "missing": [],
    }
    missing: list[str] = []
    if not capture.is_dir():
        cell["missing"] = [f"no {config.job} capture"]
        return cell
    if config.report_only and not _has_dlss_record(capture):
        cell["missing"] = ["no DLSS-enabled capture (manifest has no dlss object)"]
        return cell
    try:
        result, thin = q19_cell(capture, scene_dir, scene)
        cell["q19"] = result.values.get("mean")
        cell["q19_p99"] = result.values.get("p99")
        cell["q19_reference_accepted"] = result.passed
        cell["q19_thin_wire"] = thin
        if scene.thin_wire and thin is None:
            excluded = result.details.get("excluded_masks")
            if isinstance(excluded, list) and THIN_WIRE_MASK in excluded:
                missing.append("thin_wire mask excluded by its MSAA 4x/8x convergence check")
            else:
                missing.append("no mask_thin_wire run")
    except (OSError, ValueError) as error:
        missing.append(f"Q19: {error}")
    if scene.q8:
        cell["q8"], cell["q8_as_patch"] = _q8(capture, "frame", missing)
        if scene.moire:
            cell["q8_moire"], cell["q8_moire_as_patch"] = _q8(capture, MOIRE_MASK, missing)
    if (capture / "disocclusion.png").is_file():
        try:
            q10 = run_metric("q10", capture, scene_dir / REFERENCE_JOB)
            cell["q10"] = q10.value
        except (MetricInputError, OSError, ValueError, KeyError) as error:
            missing.append(f"Q10: {error}")
    else:
        missing.append("Q10: no disocclusion.png")
    cell["missing"] = missing
    return cell


def _q8(capture: Path, mask: str, missing: list[str]) -> tuple[float | None, float | None]:
    """Masked Q8 with every pixel a patch, and with the mask as one patch."""
    try:
        result = run_metric("q8", capture, None, mask)
    except (MetricInputError, OSError, ValueError, KeyError) as error:
        missing.append(f"Q8 ({mask}): {error}")
        return None, None
    whole = result.values.get("mask_as_patch")
    return (
        float(result.value) if isinstance(result.value, (int, float)) else None,
        float(whole) if isinstance(whole, (int, float)) else None,
    )


def build_report(root: Path) -> dict[str, object]:
    resolutions = sorted(
        path.name
        for path in root.iterdir()
        if path.is_dir() and "x" in path.name and path.name.replace("x", "").isdigit()
    )
    tables: dict[str, object] = {}
    for resolution in resolutions:
        scenes: dict[str, object] = {}
        for scene in SCENES:
            scene_dir = root / resolution / scene.name
            if not scene_dir.is_dir():
                continue
            scenes[scene.name] = {config.key: build_cell(scene_dir, scene, config) for config in CONFIGS}
        cross: dict[str, object] = {}
        for config in CONFIGS:
            values = [
                float(row[config.key]["q19"])  # type: ignore[index]
                for row in scenes.values()
                if isinstance(row, dict) and row[config.key]["q19"] is not None
            ]
            cross[config.key] = {
                "q19_mean_across_scenes": float(np.mean(np.asarray(values))) if values else None,
                "scenes": len(values),
            }
        tables[resolution] = {"scenes": scenes, "cross_scene": cross}
    return {
        "schema": SCHEMA,
        "report": "GT-2 upscaler comparison",
        "configs": [
            {
                "key": config.key,
                "upscaler": config.upscaler,
                "preset": config.preset,
                "scale": config.scale,
                "job": config.job,
                "report_only": config.report_only,
            }
            for config in CONFIGS
        ],
        "resolutions": tables,
        "gates": "report only; the GT-2 comparisons run in PQ with GT (#199)",
    }


def _fmt(value: object) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.4f}"
    return str(value)


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# GT-2 upscaler comparison",
        "",
        "Report only. DLSS rows are report-only and stay empty without a DLSS-enabled capture.",
        "",
    ]
    configs = {str(row["key"]): row for row in report["configs"]}
    resolutions = report["resolutions"]
    assert isinstance(resolutions, dict)
    for resolution, table in resolutions.items():
        lines.append(f"## {resolution}")
        lines.append("")
        lines.append(
            "| Scene | Upscaler | Preset | Scale | Q19 | Q19 thin wire | Q8 per pixel | Q8 as one patch "
            "| Q8 moiré per pixel | Q8 moiré as one patch | Q10 |"
        )
        lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
        for scene_name, row in table["scenes"].items():
            for key, cell in row.items():
                config = configs[key]
                values = [
                    cell[name]
                    for name in ("q19", "q19_thin_wire", "q8", "q8_as_patch", "q8_moire", "q8_moire_as_patch", "q10")
                ]
                lines.append(
                    f"| {scene_name} | {config['upscaler']} | {config['preset']} | {config['scale']} | "
                    + " | ".join(_fmt(value) for value in values)
                    + " |"
                )
        lines.append("")
        lines.append("| Upscaler | Preset | Scale | Mean Q19 across scenes | Scenes |")
        lines.append("|---|---|---|---|---|")
        for key, cross in table["cross_scene"].items():
            config = configs[key]
            lines.append(
                f"| {config['upscaler']} | {config['preset']} | {config['scale']} | {_fmt(cross['q19_mean_across_scenes'])} | {cross['scenes']} |"
            )
        lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="upscaler_compare.py")
    parser.add_argument("--in", dest="in_dir", type=Path, required=True, help="upscaler_jobs.sh output root")
    parser.add_argument("--out", type=Path, help="write upscaler_compare.json and upscaler_compare.md here")
    args = parser.parse_args(argv)
    if not args.in_dir.is_dir():
        json.dump({"report": "GT-2 upscaler comparison", "error": f"{args.in_dir} is not a folder"}, sys.stdout)
        sys.stdout.write("\n")
        return 2
    # Non-finite values (a non-finite Q8) become null.
    report = json_ready(build_report(args.in_dir))
    assert isinstance(report, dict)
    text = json.dumps(report, sort_keys=True, indent=2, allow_nan=False)
    if args.out is not None:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "upscaler_compare.json").write_text(text + "\n", encoding="utf-8")
        (args.out / "upscaler_compare.md").write_text(markdown(report) + "\n", encoding="utf-8")
    sys.stdout.write(text + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
