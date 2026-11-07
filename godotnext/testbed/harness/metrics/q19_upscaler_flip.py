#!/usr/bin/env python3
"""Read Q19 captures and report the frozen metric without changing the captures."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from godotnext_metrics.images import indexed_paths, read_exr, read_png
from godotnext_metrics.q19_upscaler_flip import (
    BILINEAR_SCALING_MODE,
    REFERENCE_SCALE,
    q19_upscaler_flip,
    select_q19_frames,
)
from numpy.typing import NDArray


def _to_float(value: object) -> float:
    if isinstance(value, (int, float, str)):
        return float(value)
    raise ValueError(f"expected numeric value, got {value!r}")


def _to_int(value: object) -> int:
    if isinstance(value, (int, str)):
        return int(value)
    if isinstance(value, float) and value.is_integer():
        return int(value)
    raise ValueError(f"expected integer value, got {value!r}")


def _load_directory_paths(directory: Path) -> tuple[dict[int, Path], dict[int, Path]]:
    exr = indexed_paths(directory, "hdr_*.exr") or indexed_paths(directory, "*.exr")
    png = indexed_paths(directory, "frame*.png") or indexed_paths(directory, "*.png")
    return exr, png


def _select_modality(
    test_dir: Path,
    ref_dir: Path,
    msaa4_dir: Path,
    *,
    form: str,
    reset_frame: int | None,
) -> tuple[list[int], dict[int, Path], dict[int, Path], dict[int, Path], bool]:
    test_exr, test_png = _load_directory_paths(test_dir)
    ref_exr, ref_png = _load_directory_paths(ref_dir)
    msaa4_exr, msaa4_png = _load_directory_paths(msaa4_dir)

    exr_shared = sorted(set(test_exr) & set(ref_exr) & set(msaa4_exr))
    exr_indices = select_q19_frames(exr_shared, form=form, reset_frame=reset_frame) if exr_shared else []
    png_shared = sorted(set(test_png) & set(ref_png) & set(msaa4_png))
    png_indices = select_q19_frames(png_shared, form=form, reset_frame=reset_frame) if png_shared else []

    # Prefer full EXR sequences when all selected frames exist as EXR; otherwise
    # use the full MovieWriter PNG sequence (which harness captures always emit).
    if exr_indices and len(exr_indices) >= len(png_indices):
        return exr_indices, test_exr, ref_exr, msaa4_exr, False
    if png_indices:
        return png_indices, test_png, ref_png, msaa4_png, True
    raise ValueError("no matching Q19 frames across test, MSAA 8x, and MSAA 4x captures")


def _load_frame(path: Path) -> NDArray[np.generic]:
    if path.suffix.lower() == ".exr":
        return read_exr(path)[..., :3]
    return read_png(path)


def _read_optional_json(path: Path) -> dict[str, object] | None:
    if not path.is_file():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must be a JSON object")
    return payload


def _infer_mask_name(mask_dir: Path) -> str:
    manifest = _read_optional_json(mask_dir / "manifest.json")
    if manifest:
        cmd = manifest.get("command_line")
        if isinstance(cmd, dict) and isinstance(cmd.get("user"), list):
            for idx, token in enumerate(cmd["user"]):
                text = str(token)
                if text.startswith("--mask="):
                    return text.removeprefix("--mask=")
                if text == "--mask" and idx + 1 < len(cmd["user"]):
                    return str(cmd["user"][idx + 1])
    return mask_dir.name.removeprefix("mask_")


def _load_mask_entry(spec: str, indices: list[int], test_dir: Path, ref_dir: Path) -> tuple[str, NDArray[np.uint8]]:
    candidate = Path(spec)
    if candidate.is_file():
        name = candidate.stem.removeprefix("cmask_")
        return name, read_png(candidate)
    if candidate.is_dir():
        mask_paths = indexed_paths(candidate, "frame*.png") or indexed_paths(candidate, "*.png")
        if not mask_paths:
            raise ValueError(f"mask directory {candidate} has no indexed PNGs")
        chosen = [mask_paths[idx] for idx in indices if idx in mask_paths]
        if not chosen:
            chosen = [mask_paths[sorted(mask_paths)[0]]]
        acc: NDArray[np.bool_] | None = None
        for path in chosen:
            img = read_png(path)
            plane = img[..., 0] > 0 if img.ndim == 3 else img > 0
            acc = plane if acc is None else (acc | plane)
        assert acc is not None
        return _infer_mask_name(candidate), (acc.astype(np.uint8) * 255)
    for base in (test_dir, ref_dir):
        cmask = base / f"cmask_{spec}.png"
        if cmask.is_file():
            return spec, read_png(cmask)
    sibling = ref_dir.parent / f"mask_{spec}"
    if sibling.is_dir():
        return _load_mask_entry(str(sibling), indices, test_dir, ref_dir)
    raise ValueError(f"cannot locate mask '{spec}'")


def _check_reference_manifest(ref_dir: Path) -> tuple[int, int] | None:
    manifest = _read_optional_json(ref_dir / "manifest.json")
    if not manifest:
        return None
    scale = manifest.get("scaling_3d_scale")
    mode = manifest.get("scaling_3d_mode")
    if scale is not None and _to_float(scale) != REFERENCE_SCALE:
        raise ValueError(f"reference scaling_3d_scale {scale} is not {REFERENCE_SCALE}")
    if mode is not None and _to_int(mode) != BILINEAR_SCALING_MODE:
        raise ValueError(f"reference scaling_3d_mode {mode} is not bilinear ({BILINEAR_SCALING_MODE})")
    output_size = manifest.get("output_size")
    if isinstance(output_size, list) and len(output_size) == 2:
        return (_to_int(output_size[0]), _to_int(output_size[1]))
    return None


def _exposure(directory: Path) -> float:
    payload = _read_optional_json(directory / "exposure.json")
    if not payload or "exposure" not in payload:
        return 1.0
    return _to_float(payload["exposure"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--msaa4", type=Path, required=True)
    parser.add_argument("--mask", action="append", default=[])
    parser.add_argument("--form", choices=("static", "moving"), default="moving")
    parser.add_argument("--reset-frame", type=int, default=400)
    args = parser.parse_args()
    try:
        reset_frame = args.reset_frame if args.form == "moving" else None
        resolution = _check_reference_manifest(args.reference)
        indices, test_paths, ref_paths, msaa4_paths, uses_png = _select_modality(
            args.test,
            args.reference,
            args.msaa4,
            form=args.form,
            reset_frame=reset_frame,
        )
        if args.form == "static" and len(indices) != 64:
            raise ValueError(f"static form needs 64 frames after settling, found {len(indices)}")
        masks: dict[str, NDArray[np.generic]] = {}
        for mask_spec in args.mask:
            name, mask_img = _load_mask_entry(str(mask_spec), indices, args.test, args.reference)
            masks[name] = mask_img
        result = q19_upscaler_flip(
            [_load_frame(test_paths[index]) for index in indices],
            [_load_frame(ref_paths[index]) for index in indices],
            [_load_frame(msaa4_paths[index]) for index in indices],
            content_masks=masks or None,
            form=args.form,
            exposure=_exposure(args.test),
            already_display=uses_png,
            scene=args.test.name,
            output_resolution=resolution,
        )
        print(json.dumps(result.to_json(), sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({"metric": "Q19", "pass": False, "error": str(error)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
