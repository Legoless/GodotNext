#!/usr/bin/env python3
"""Interleaved Linux Vulkan RTX 5090 GodotNext vs. UE 5.9 Vulkan SM6 comparison runner."""
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path("/home/legoless/Projects/legoless/GodotNext")
TESTBED_DIR = REPO_ROOT / "godotnext/testbed"
GODOT_BIN = REPO_ROOT / "bin/godot.linuxbsd.editor.x86_64"
UE_EDITOR = Path("/home/legoless/Projects/vendor/UnrealEngine/Engine/Binaries/Linux/UnrealEditor")
UE_PROJECT = Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-compare/GodotNextCompare.uproject")
UE_CSV_DIRS = [
    Path("/home/legoless/Projects/vendor/UnrealEngine/Engine/Saved/Profiling/CSV"),
    Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-compare/Saved/Profiling/CSV"),
]

sys.path.insert(0, str(TESTBED_DIR / "harness/metrics"))
from godotnext_metrics.timing_metrics import read_timing_csv
from godotnext_metrics.ue_bar import run_statistic
from godotnext_metrics.ue_compare import load_config, godotnext_args
from godotnext_metrics.ue_csv import read_csv_profile, group_ms


def newest_ue_csv(since_ts: float) -> Path | None:
    best = None
    best_m = since_ts
    for d in UE_CSV_DIRS:
        if not d.is_dir():
            continue
        for p in d.glob("*.csv"):
            m = p.stat().st_mtime
            if m >= best_m:
                best_m = m
                best = p
    return best


def run_godotnext(cfg, run_dir: Path, is_warmup: bool) -> int:
    run_dir.mkdir(parents=True, exist_ok=True)
    override_dst = TESTBED_DIR / "override.cfg"
    override_src = None
    if cfg.godotnext.override:
        rel = cfg.godotnext.override.removeprefix("res://")
        override_src = TESTBED_DIR / rel
    w, h = cfg.resolution
    args = [
        "xvfb-run", "-a", "-s", f"-screen 0 {w}x{h}x24",
        str(GODOT_BIN), "--rendering-driver", "vulkan",
    ]
    for a in godotnext_args(cfg):
        args.append(a.replace("{out}", str(run_dir)))
    if is_warmup:
        # Short warmup launch just to populate shader/PSO caches
        args = [("--frames=60" if a.startswith("--frames=") else a) for a in args]
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    try:
        if override_src and override_src.is_file():
            shutil.copy2(override_src, override_dst)
        elif override_dst.exists():
            override_dst.unlink()
        log_path = run_dir / "godot.log"
        with open(log_path, "w") as lf:
            res = subprocess.run(
                args,
                cwd=str(REPO_ROOT),
                env=env,
                stdout=lf,
                stderr=subprocess.STDOUT,
                timeout=int(cfg.godotnext.timeout_minutes * 60),
            )
        return res.returncode
    finally:
        if override_dst.exists():
            override_dst.unlink()


def run_ue(cfg, run_dir: Path, is_warmup: bool, token: str) -> tuple[int, Path | None]:
    run_dir.mkdir(parents=True, exist_ok=True)
    w, h = cfg.resolution
    capture_frames = 120 if is_warmup else (cfg.ue.warmup_frames + cfg.frames)
    ue_extra = ["-vulkan" if a.lower() == "-dx12" else a for a in cfg.ue.args]
    if "-vulkan" not in [a.lower() for a in ue_extra]:
        ue_extra.append("-vulkan")
    exec_cmds = list(cfg.ue.exec_cmds)
    # Remove Windows-only ProfileGPU if any; keep scalability & Lumen/async settings
    cmd = [
        str(UE_EDITOR),
        str(UE_PROJECT),
        cfg.ue.map,
        "-game",
        *ue_extra,
        "-RenderOffScreen",
        f"-ResX={w}",
        f"-ResY={h}",
        "-novsync",
        "-unattended",
        "-nosplash",
        "-NoSound",
        "-NoScreenMessages",
        "-benchmark",
        f"-fps={cfg.ue.fps}",
        "-deterministic",
        "-csvGpuStats",
        f"-csvCaptureFrames={capture_frames}",
        "-ExitAfterCsvProfiling",
        f"-csvMetadata=godotnextrun={token}",
        f"-abslog={run_dir / 'ue.log'}",
    ]
    if exec_cmds:
        cmd.append("-ExecCmds=" + ",".join(exec_cmds))
    env = os.environ.copy()
    env.setdefault("DISPLAY", ":0")
    t0 = time.time() - 1.0
    res = subprocess.run(
        cmd,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=int(cfg.ue.timeout_minutes * 60),
    )
    csv_src = newest_ue_csv(t0)
    csv_dst = None
    if csv_src is not None:
        csv_dst = run_dir / "ue_profile.csv"
        shutil.copy2(csv_src, csv_dst)
    return res.returncode, csv_dst


def eval_set(cfg_path: Path, out_root: Path, runs: int, skip_warmup: bool):
    cfg = load_config(cfg_path)
    out_dir = out_root / cfg.name
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n=== COMPARE {cfg.name} ({cfg.resolution[0]}x{cfg.resolution[1]}, {runs} interleaved runs) ===", flush=True)

    if not skip_warmup:
        print(f"  [warmup] godotnext...", flush=True)
        rc = run_godotnext(cfg, out_dir / "runs/warmup_gn", True)
        print(f"    rc={rc}", flush=True)
        print(f"  [warmup] ue59...", flush=True)
        rc, _ = run_ue(cfg, out_dir / "runs/warmup_ue", True, f"{cfg.name}_wu")
        print(f"    rc={rc}", flush=True)

    gn_dirs = []
    ue_csvs = []
    for i in range(1, runs + 1):
        gdir = out_dir / f"runs/gn_{i}"
        print(f"  [run {i}/{runs}] godotnext...", flush=True)
        grc = run_godotnext(cfg, gdir, False)
        print(f"    gn rc={grc}", flush=True)
        if grc == 0 and (gdir / "timing.csv").is_file():
            gn_dirs.append(gdir)

        udir = out_dir / f"runs/ue_{i}"
        print(f"  [run {i}/{runs}] ue59...", flush=True)
        urc, ucsv = run_ue(cfg, udir, False, f"{cfg.name}_r{i}")
        print(f"    ue rc={urc} csv={ucsv is not None}", flush=True)
        if urc == 0 and ucsv and ucsv.is_file():
            ue_csvs.append(ucsv)

    # Evaluate lines
    results = {"name": cfg.name, "title": cfg.title, "resolution": list(cfg.resolution), "runs": runs, "lines": []}
    for spec in cfg.lines:
        gn_vals = []
        if spec.godotnext is not None:
            for gdir in gn_dirs:
                cols = read_timing_csv(gdir / "timing.csv")
                names = spec.godotnext.select(list(cols))
                if not names:
                    continue
                n = len(cols.get("frame", []))
                totals = []
                for row in range(n):
                    cells = [cols[nm][row] for nm in names]
                    totals.append(None if all(c is None for c in cells) else sum(c or 0.0 for c in cells))
                try:
                    val, _ = run_statistic(totals, spec.statistic, cfg.frames)
                    gn_vals.append(val)
                except Exception as e:
                    print(f"    gn stat err {spec.id}: {e}")
        ue_vals = []
        if spec.ue is not None and spec.ue.columns:
            for ucsv in ue_csvs:
                prof = read_csv_profile(ucsv)
                avail = [c for c in spec.ue.columns if prof.has_column(c)]
                if not avail:
                    continue
                try:
                    series = group_ms(prof, avail, cfg.ue.warmup_frames, cfg.frames)
                    val, _ = run_statistic(series, spec.statistic, cfg.frames)
                    ue_vals.append(val)
                except Exception as e:
                    print(f"    ue stat err {spec.id}: {e}")

        gn_med = statistics.median(gn_vals) if gn_vals else None
        ue_med = statistics.median(ue_vals) if ue_vals else None
        gn_spread = ((max(gn_vals) - min(gn_vals)) / gn_med * 100.0) if (len(gn_vals) >= 2 and gn_med and gn_med > 0) else None
        ue_spread = ((max(ue_vals) - min(ue_vals)) / ue_med * 100.0) if (len(ue_vals) >= 2 and ue_med and ue_med > 0) else None
        ratio = (gn_med / ue_med) if (gn_med is not None and ue_med is not None and ue_med > 0) else None
        entry = {
            "id": spec.id,
            "name": spec.name,
            "gates": list(spec.gates),
            "status": spec.status,
            "gn_median_ms": gn_med,
            "gn_spread_pct": gn_spread,
            "gn_runs": gn_vals,
            "ue_median_ms": ue_med,
            "ue_spread_pct": ue_spread,
            "ue_runs": ue_vals,
            "ratio": ratio,
        }
        results["lines"].append(entry)
        gn_s = f"{gn_med:.3f} ms" if gn_med is not None else "-"
        ue_s = f"{ue_med:.3f} ms" if ue_med is not None else "-"
        rat_s = f"{ratio:.3f}x" if ratio is not None else "-"
        sp_s = f"gn±{gn_spread:.1f}%/ue±{ue_spread:.1f}%" if (gn_spread is not None and ue_spread is not None) else ""
        print(f"  LINE {spec.id:30s} | GN={gn_s:10s} | UE={ue_s:10s} | Ratio={rat_s:8s} {sp_s}", flush=True)

    (out_dir / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("configs", nargs="+")
    ap.add_argument("--out", default="/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-compare/results")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--skip-warmup", action="store_true")
    args = ap.parse_args()
    out_root = Path(args.out)
    all_res = []
    for c in args.configs:
        p = Path(c)
        if not p.is_absolute():
            p = REPO_ROOT / "godotnext/testbed/ue_compare" / (c if c.endswith(".json") else c + ".json")
        try:
            all_res.append(eval_set(p, out_root, args.runs, args.skip_warmup))
        except Exception as e:
            print(f"ERROR in {c}: {e}", flush=True)
            all_res.append({"name": p.stem, "error": str(e)})
        (out_root / "all_summary.json").write_text(json.dumps(all_res, indent=2) + "\n")


if __name__ == "__main__":
    main()
