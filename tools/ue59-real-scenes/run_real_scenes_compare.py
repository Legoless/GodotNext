#!/usr/bin/env python3
"""Interleaved locked-clock UE 5.9 Vulkan SM6 (Nanite + Lumen HWRT High + VSM) vs.
GodotNext Vulkan (NextGeo + NextGI HWRT High + VSM) comparison on real-world scenes."""
import argparse
import csv
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = Path(os.environ.get("GODOTNEXT_ROOT") or SCRIPT_DIR.parents[1])
TOOLS_DIR = Path(os.environ.get("GODOTNEXT_TOOLS_DIR") or REPO_ROOT / "local/godotnext-tools")
GODOT_BIN = REPO_ROOT / "bin/godot.linuxbsd.editor.x86_64"
REAL_ROOT = TOOLS_DIR / "ue59-real-scenes"
GODOT_PROJ = REAL_ROOT / "godot_project"

sys.path.insert(0, str(REPO_ROOT / "godotnext/testbed/harness/metrics"))
from godotnext_metrics.ue_bar import run_statistic
from godotnext_metrics.ue_csv import read_csv_profile, group_ms

UE_EXEC_CMDS = [
    "r.ScreenPercentage 100",
    "sg.ResolutionQuality 100",
    "sg.ViewDistanceQuality 2",
    "sg.AntiAliasingQuality 2",
    "sg.ShadowQuality 2",
    "sg.GlobalIlluminationQuality 2",
    "sg.ReflectionQuality 2",
    "sg.PostProcessQuality 2",
    "sg.TextureQuality 2",
    "sg.EffectsQuality 2",
    "sg.FoliageQuality 2",
    "sg.ShadingQuality 2",
    "sg.LandscapeQuality 2",
    "r.Lumen.HardwareRayTracing 1",
    "r.Shadow.Virtual.Enable 1",
    "r.Nanite 1",
    "r.RDG.AsyncCompute 0",
    "r.Lumen.AsyncCompute 0",
    "r.TSR.AsyncCompute 0",
]

SCENES = {
    "bistro": {
        "title": "Amazon Lumberyard Bistro (2.8M tris, exterior+interior Parisian street, glTF 2.0)",
        "ue_project": REAL_ROOT / "ue_project/RealScenesUE59.uproject",
        "ue_map": "/Game/RealScenes/BistroV2/BistroNaniteLumen",
        "gn_args": [
            "--scene=res://assets/bistro/bistro.gltf",
            "--use-scene-cam=1",
            "--disable-scene-points=1",
            "--sun-rot=-50,-30,0",
            "--sun-energy=1.2",
            "--exposure=0.55",
        ],
    },
    "intel_sponza": {
        "title": "Intel Sponza 2022 + Curtains + Geometric Ivy (10.81M tris, 4K PBR photogrammetry, glTF 2.0)",
        "ue_project": REAL_ROOT / "ue_project/RealScenesUE59.uproject",
        "ue_map": "/Game/RealScenes/IntelSponzaV2/IntelSponzaNaniteLumen",
        "gn_args": [
            "--scene=res://assets/intel_sponza/main/NewSponza_Main_glTF_003.gltf",
            "--extra=res://assets/intel_sponza/curtains/NewSponza_Curtains_glTF.gltf,res://assets/intel_sponza/ivy/NewSponza_IvyGrowth_glTF.gltf",
            "--cam-pos=-8.5,2.2,0.0",
            "--cam-target=6.0,3.0,0.0",
            "--cam-fov=45.0",
            "--sun-rot=-65,-25,0",
            "--sun-energy=3.5",
        ],
    },
    "classic_sponza": {
        "title": "ClassicSponzaUnreal (native UE5 .uproject converted to Nanite+Lumen, exported GLB -> NextGeo+NextGI)",
        "ue_project": REAL_ROOT / "ClassicSponzaUE59/ClassicSponzaUnreal.uproject",
        "ue_map": "/Game/ClassicSponza/Maps/SponzaPersistent",
        "gn_args": [
            "--scene=res://assets/classic_sponza/ClassicSponza.glb",
            "--cam-pos=0.0,-4.4,-10.5",
            "--cam-target=0.0,-3.6,10.5",
            "--cam-fov=41.5",
            "--sun-rot=-55,-35,0",
            "--sun-energy=3.0",
        ],
    },
}

UE_GROUPS = {
    "whole_frame_gpu": ["GPUTime"],
    "lumen_total": [
        "GPU/RayTracingScene",
        "GPU/LumenSceneUpdate",
        "GPU/UpdateLumenSceneBuffers",
        "GPU/LumenSceneLighting",
        "GPU/LumenScreenProbeGather",
        "GPU/LumenReflections",
    ],
    "nanite_visbuffer": ["GPU/NaniteVisBuffer"],
    "nanite_basepass": ["GPU/NaniteBasePass"],
    "vsm_total": ["GPU/ShadowDepths", "GPU/ShadowProjection"],
}


def read_gn_csv(path: Path) -> dict[str, list[float | None]]:
    cols: dict[str, list[float | None]] = {}
    with open(path, "r", newline="") as fh:
        r = csv.DictReader(fh)
        for name in r.fieldnames or []:
            cols[name] = []
        for row in r:
            for name in r.fieldnames or []:
                v = row.get(name, "")
                if name == "frame":
                    cols[name].append(float(v) if v else None)
                else:
                    cols[name].append(float(v) if v not in ("", None) else None)
    return cols


def gn_group_p95(cols: dict[str, list[float | None]], select_fn, nframes: int) -> float | None:
    pass_cols = [c for c in cols.keys() if c not in ("frame", "wall_ms") and select_fn(c)]
    if not pass_cols:
        return None
    n = len(cols.get("frame", []))
    totals = []
    for i in range(n):
        vals = [cols[c][i] for c in pass_cols]
        totals.append(None if all(v is None for v in vals) else sum(v or 0.0 for v in vals))
    val, _ = run_statistic(totals, "p95", nframes)
    return val


def ue_root() -> Path:
    """The Unreal Engine 5.9 install (the folder that holds Engine/), from UE_ROOT."""
    root = os.environ.get("UE_ROOT")
    if not root:
        raise SystemExit("UE_ROOT is not set: point it at the Unreal Engine 5.9 install (the folder that holds Engine/).")
    return Path(root)


def ue_editor() -> Path:
    return Path(os.environ.get("UE_EDITOR") or ue_root() / "Engine/Binaries/Linux/UnrealEditor")


def newest_ue_csv(ue_project: Path, since_ts: float) -> Path | None:
    dirs = [
        ue_project.parent / "Saved/Profiling/CSV",
        ue_root() / "Engine/Saved/Profiling/CSV",
    ]
    best = None
    best_m = since_ts
    for d in dirs:
        if not d.is_dir():
            continue
        for p in d.glob("*.csv"):
            m = p.stat().st_mtime
            if m >= best_m:
                best_m = m
                best = p
    return best


def run_gn(cfg, out_dir: Path, warmup: int, frames: int, capture_png: bool) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "xvfb-run", "-a", "-s", "-screen 0 1920x1080x24",
        str(GODOT_BIN),
        "--rendering-driver", "vulkan",
        "--disable-vsync",
        "--gpu-profile",
        "--path", str(GODOT_PROJ),
        "--",
        *cfg["gn_args"],
        f"--warmup={warmup}",
        f"--frames={frames}",
        f"--out={out_dir}",
        f"--capture-png={'1' if capture_png else '0'}",
    ]
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    with open(out_dir / "godot.log", "w") as lf:
        res = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=900)
    return res.returncode


def run_ue(cfg, out_dir: Path, warmup: int, frames: int, capture_png: bool, token: str) -> tuple[int, Path | None]:
    out_dir.mkdir(parents=True, exist_ok=True)
    exec_cmds = list(UE_EXEC_CMDS)
    if capture_png:
        exec_cmds.append("HighResShot 1920x1080")
    cmd = [
        str(ue_editor()),
        str(cfg["ue_project"]),
        cfg["ue_map"],
        "-game",
        "-vulkan",
        "-sm6",
        "-RenderOffScreen",
        "-ResX=1920",
        "-ResY=1080",
        "-novsync",
        "-unattended",
        "-nosplash",
        "-NoSound",
        "-NoScreenMessages",
        "-benchmark",
        "-fps=60",
        "-deterministic",
        "-csvGpuStats",
        f"-csvCaptureFrames={warmup + frames}",
        "-ExitAfterCsvProfiling",
        f"-csvMetadata=realscene={token}",
        f"-abslog={out_dir / 'ue.log'}",
        "-ExecCmds=" + ",".join(exec_cmds),
    ]
    env = os.environ.copy()
    env.setdefault("DISPLAY", ":0")
    t0 = time.time() - 1.0
    res = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    csv_src = newest_ue_csv(cfg["ue_project"], t0)
    csv_dst = None
    if csv_src is not None:
        csv_dst = out_dir / "ue_profile.csv"
        shutil.copy2(csv_src, csv_dst)
    if capture_png:
        shot_dir = cfg["ue_project"].parent / "Saved/Screenshots/LinuxEditor"
        if shot_dir.is_dir():
            shots = sorted([p for p in shot_dir.glob("*.png") if p.stat().st_mtime >= t0], key=lambda p: p.stat().st_mtime)
            if shots:
                shutil.copy2(shots[-1], out_dir / "ue59_frame.png")
    return res.returncode, csv_dst


def bench_scene(key: str, out_root: Path, runs: int, frames: int):
    cfg = SCENES[key]
    sdir = out_root / key
    sdir.mkdir(parents=True, exist_ok=True)
    print(f"\n=== REAL SCENE: {key} ({cfg['title']}) ===", flush=True)
    print("  [warmup] godotnext...", flush=True)
    print("    rc=", run_gn(cfg, sdir / "warmup_gn", 60, 60, True), flush=True)
    print("  [warmup] ue59...", flush=True)
    urc, _ = run_ue(cfg, sdir / "warmup_ue", 60, 60, True, f"{key}_wu")
    print("    rc=", urc, flush=True)

    gn_runs, ue_runs = [], []
    for i in range(1, runs + 1):
        gdir = sdir / f"gn_{i}"
        print(f"  [run {i}/{runs}] godotnext...", flush=True)
        grc = run_gn(cfg, gdir, 120, frames, i == 1)
        print(f"    gn rc={grc}", flush=True)
        if grc == 0 and (gdir / "timing.csv").is_file():
            gn_runs.append(gdir / "timing.csv")

        udir = sdir / f"ue_{i}"
        print(f"  [run {i}/{runs}] ue59...", flush=True)
        urc, ucsv = run_ue(cfg, udir, 300, frames, i == 1, f"{key}_r{i}")
        print(f"    ue rc={urc} csv={ucsv is not None}", flush=True)
        # Accept CSV even if UE crashes on shutdown (-11) AFTER flushing CSV!
        if ucsv is not None and ucsv.is_file() and ucsv.stat().st_size > 1000:
            ue_runs.append(ucsv)

    gn_metrics = {
        "whole_frame_gpu": lambda c: True,
        "nextgi_total": lambda c: (c.startswith("nextgi_") and c not in ("nextgi_2_ms", "nextgi_as_builds_ms")) or c == "skinned_refit_ms",
        "nextgeo_cull_raster": lambda c: c in (
            "nextgeo_instance_cull_ms", "nextgeo_bvh_ms", "nextgeo_cluster_cull_ms",
            "nextgeo_raster_p1_ms", "nextgeo_hiz_ms", "nextgeo_phase2_cull_ms", "nextgeo_raster_p2_ms",
        ),
        "nextgeo_resolve": lambda c: c in ("nextgeo_stage_a_ms", "nextgeo_stage_b_ms"),
        "vsm_total": lambda c: c.startswith("vsm_") or "virtual_shadow" in c or c in ("nextgeo_shadow_cull_ms", "nextgeo_shadow_raster_ms", "nextgeo_shadow_cull_2_ms", "nextgeo_shadow_raster_2_ms"),
    }
    pairs = [
        ("whole_frame_gpu", "whole_frame_gpu"),
        ("nextgi_total", "lumen_total"),
        ("nextgeo_cull_raster", "nanite_visbuffer"),
        ("nextgeo_resolve", "nanite_basepass"),
        ("vsm_total", "vsm_total"),
    ]
    summary = {"scene": key, "title": cfg["title"], "gn_runs": len(gn_runs), "ue_runs": len(ue_runs), "metrics": {}}
    gn_parsed = [read_gn_csv(p) for p in gn_runs]
    ue_parsed = [read_csv_profile(p) for p in ue_runs]
    if gn_parsed:
        summary["gn_columns"] = [c for c in gn_parsed[0].keys() if c not in ("frame", "wall_ms")]
    for gname, uname in pairs:
        gvals = [v for cols in gn_parsed if (v := gn_group_p95(cols, gn_metrics[gname], frames)) is not None]
        uvals = []
        for prof in ue_parsed:
            avail = [c for c in UE_GROUPS[uname] if prof.has_column(c)]
            if avail:
                try:
                    series = group_ms(prof, avail, 300, frames)
                    val, _ = run_statistic(series, "p95", frames)
                    uvals.append(val)
                except Exception:
                    pass
        gmed = statistics.median(gvals) if gvals else None
        umed = statistics.median(uvals) if uvals else None
        ratio = (gmed / umed) if (gmed is not None and umed is not None and umed > 0) else None
        summary["metrics"][gname] = {
            "ue_group": uname,
            "gn_p95_ms": gmed,
            "ue_p95_ms": umed,
            "ratio": ratio,
            "gn_runs": gvals,
            "ue_runs": uvals,
        }
        gs = f"{gmed:.3f} ms" if gmed is not None else "-"
        us = f"{umed:.3f} ms" if umed is not None else "-"
        rs = f"{ratio:.3f}x" if ratio is not None else "-"
        print(f"  METRIC {gname:22s} vs {uname:18s} | GN={gs:10s} | UE={us:10s} | Ratio={rs}", flush=True)
    (sdir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenes", nargs="*", default=["bistro", "intel_sponza", "classic_sponza"])
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--out", default=str(REAL_ROOT / "results"))
    args = ap.parse_args()
    ue_root()  # fail before the first run when UE_ROOT is unset
    out_root = Path(args.out)
    out_root.mkdir(parents=True, exist_ok=True)
    all_res = []
    for s in args.scenes:
        all_res.append(bench_scene(s, out_root, args.runs, args.frames))
        (out_root / "all_real_summary.json").write_text(json.dumps(all_res, indent=2) + "\n")


if __name__ == "__main__":
    main()
