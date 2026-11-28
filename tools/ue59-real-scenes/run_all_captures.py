#!/usr/bin/env python3
"""Master comparison capture runner:
1) Configures matched UE 5.9 camera maps (exact sun direction vector, sky, manual exposure, camera look-at + HFOV).
2) Captures 1920x1080 converged UE 5.9 (Nanite + Lumen HWRT High + VSM) PNGs + CSV GPU profiles.
3) Captures 1920x1080 converged GodotNext (NextGeo + NextGI HWRT High + VSM) PNGs + GPU pass CSVs
   using a single persistent Xvfb display with warm retry on transient NVIDIA vkCreateDevice -3.
"""
import argparse
import json
import os
import shutil
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
COMP_ROOT = REAL_ROOT / "comparison_v3"

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


def ue_root() -> Path:
    """The Unreal Engine 5.9 install (the folder that holds Engine/), from UE_ROOT."""
    root = os.environ.get("UE_ROOT")
    if not root:
        raise SystemExit("UE_ROOT is not set: point it at the Unreal Engine 5.9 install (the folder that holds Engine/).")
    return Path(root)


def ue_editor() -> Path:
    return Path(os.environ.get("UE_EDITOR") or ue_root() / "Engine/Binaries/Linux/UnrealEditor")


def ue_editor_cmd() -> Path:
    return Path(os.environ.get("UE_EDITOR_CMD") or ue_root() / "Engine/Binaries/Linux/UnrealEditor-Cmd")


def ue_shot_dirs() -> list[Path]:
    saved = ue_root() / "Engine/Saved/Screenshots"
    return [saved / "LinuxEditor", saved / "Linux"]


def load_specs(name: str) -> dict:
    """Read a scene spec file kept next to this script. Its paths are relative to TOOLS_DIR; absolute ones are used as they are."""
    specs = json.loads((SCRIPT_DIR / name).read_text())
    for sc in specs["scenes"]:
        for key in ("ue_project", "ue_sky_hdri"):
            if sc.get(key):
                sc[key] = str(TOOLS_DIR / sc[key])
    return specs


def newest_file(dirs: list[Path], pattern: str, since_ts: float) -> Path | None:
    best = None
    best_m = since_ts
    for d in dirs:
        if not d.is_dir():
            continue
        for p in d.glob(pattern):
            m = p.stat().st_mtime
            if m >= best_m:
                best_m = m
                best = p
    return best


def setup_ue_maps_for_scene(sc: dict):
    sid = sc["id"]
    spec_tmp = COMP_ROOT / sid / "_ue_scene_spec.json"
    spec_tmp.parent.mkdir(parents=True, exist_ok=True)
    spec_tmp.write_text(json.dumps(sc, indent=2))
    env = os.environ.copy()
    env["UE_ACTION"] = "setup_cam_maps"
    env["SCENE_SPEC_JSON"] = str(spec_tmp)
    env["GODOTNEXT_TOOLS_DIR"] = str(TOOLS_DIR)
    cmd = [
        str(ue_editor_cmd()),
        sc["ue_project"],
        "-nullrhi", "-unattended", "-nosplash", "-nop4", "-NoSound",
        f"-ExecutePythonScript={SCRIPT_DIR / 'setup_ue_comparison.py'}",
    ]
    log_p = COMP_ROOT / sid / "ue_setup_cams.log"
    print(f"[{sid}] Setting up {len(sc['cameras'])} UE 5.9 camera maps...", flush=True)
    with open(log_p, "w") as lf:
        res = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=300)
    print(f"[{sid}] UE camera map setup rc={res.returncode}", flush=True)


def capture_ue_scene(sc: dict, display: str, only_cams: set[str] | None = None, warmup_delay: int = 135, total_frames: int = 185):
    sid = sc["id"]
    ue_proj = Path(sc["ue_project"])
    out_dir = COMP_ROOT / sid
    out_dir.mkdir(parents=True, exist_ok=True)
    shot_dirs = ue_shot_dirs() + [
        ue_proj.parent / "Saved/Screenshots/LinuxEditor",
        ue_proj.parent / "Saved/Screenshots/Linux",
    ]
    csv_dirs = [
        ue_proj.parent / "Saved/Profiling/CSV",
        ue_root() / "Engine/Saved/Profiling/CSV",
    ]
    env = os.environ.copy()
    env.pop("WAYLAND_DISPLAY", None)
    env["DISPLAY"] = display
    for cam in sc["cameras"]:
        cid = cam["id"]
        if only_cams and cid not in only_cams:
            continue
        cam_map = f"{sc['ue_cam_map_prefix']}_{cid}"
        exec_cmds = list(UE_EXEC_CMDS) + [
            f"r.HighResScreenshotDelay {warmup_delay}",
            "HighResShot 1920x1080",
        ]
        log_p = out_dir / f"{cid}_ue.log"
        cmd = [
            str(ue_editor()),
            str(ue_proj),
            cam_map,
            "-game", "-vulkan", "-sm6", "-RenderOffScreen",
            "-ResX=1920", "-ResY=1080",
            "-novsync", "-unattended", "-nosplash", "-NoSound", "-NoScreenMessages",
            "-benchmark", "-fps=60", "-deterministic",
            "-csvGpuStats", f"-csvCaptureFrames={total_frames}", "-ExitAfterCsvProfiling",
            f"-csvMetadata=compare={sid}_{cid}",
            f"-abslog={log_p}",
            "-ExecCmds=" + ",".join(exec_cmds),
        ]
        dst_png = out_dir / f"{cid}_ue59.png"
        dst_csv = out_dir / f"{cid}_ue_profile.csv"
        for attempt in range(1, 5):
            t0 = time.time() - 1.0
            print(f"  [UE 5.9] {sid}/{cid} ({cam_map}, attempt {attempt})...", flush=True)
            res = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=420)
            shot = newest_file(shot_dirs, "*.png", t0)
            csv_p = newest_file(csv_dirs, "*.csv", t0)
            if res.returncode == 0 and shot is not None and shot.stat().st_size > 10000:
                shutil.copy2(shot, dst_png)
                if csv_p is not None:
                    shutil.copy2(csv_p, dst_csv)
                print(f"    -> rc=0 png=True ({dst_png.stat().st_size} B) csv={dst_csv.exists()}", flush=True)
                break
            print(f"    -> transient rc={res.returncode} shot={shot is not None}, retrying...", flush=True)
            time.sleep(1.5)


def capture_godot_scene(sc: dict, display: str, only_cams: set[str] | None = None, warmup: int = 160, frames: int = 45):
    sid = sc["id"]
    out_dir = COMP_ROOT / sid
    out_dir.mkdir(parents=True, exist_ok=True)
    cams = [c for c in sc["cameras"] if not only_cams or c["id"] in only_cams]
    if not cams:
        return

    cfg = {
        "scene_id": sid,
        "scenes": sc["gn_scenes"],
        "out_dir": str(out_dir),
        "sun_dir": sc["sun_dir"],
        "sun_color": sc.get("sun_color", [1.0, 0.96, 0.90]),
        "sun_energy": sc.get("gn_sun_energy", 4.0),
        "sky_energy": sc.get("gn_sky_energy", 1.5),
        "exposure": sc.get("gn_exposure", 1.0),
        "warmup": warmup,
        "frames": frames,
        "cameras": cams,
    }
    cfg_path = out_dir / "godotnext_config.json"
    cfg_path.write_text(json.dumps(cfg, indent=2))

    # Write clean temporary override.cfg so large 10M-tri scenes stream full LOD within warmup
    override_p = GODOT_PROJ / "override.cfg"
    override_p.write_text(
        "[rendering]\n"
        "limits/nextgeo/geometry_cache_mb=1024\n"
        "limits/nextgeo/page_upload_kib_per_frame=65536\n"
        "global_illumination/nextgi/enabled=true\n"
        "global_illumination/nextgi/hardware_ray_tracing=0\n"
        "global_illumination/nextgi/quality=2\n"
        "lights_and_shadows/shadow_method=1\n"
    )
    env = os.environ.copy()
    env.pop("WAYLAND_DISPLAY", None)
    env["DISPLAY"] = display

    log_p = out_dir / "godotnext.log"
    cmd = [
        str(GODOT_BIN),
        "--rendering-driver", "vulkan",
        "--disable-vsync",
        "--gpu-profile",
        "--path", str(GODOT_PROJ),
        "res://capture_multi_cam.tscn",
        "--",
        f"--config={cfg_path}",
    ]
    try:
        for attempt in range(1, 7):
            print(f"  [GodotNext] {sid} ({len(cams)} cams, attempt {attempt})...", flush=True)
            with open(log_p, "w") as lf:
                res = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=600)
            txt = log_p.read_text(errors="replace")
            if "Couldn't create Vulkan device (VkResult error -3)" in txt:
                print(f"    -> transient VkResult -3 on attempt {attempt}, retrying...", flush=True)
                time.sleep(1.0)
                continue
            ok_pngs = [c["id"] for c in cams if (out_dir / f"{c['id']}_godotnext.png").is_file()]
            print(f"    -> rc={res.returncode} captured={ok_pngs}", flush=True)
            break
    finally:
        if override_p.exists():
            override_p.unlink()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenes", default="all", help="Comma-separated scene IDs or 'all'")
    ap.add_argument("--cams", default="all", help="Comma-separated camera IDs or 'all'")
    ap.add_argument("--skip-ue-setup", action="store_true")
    ap.add_argument("--skip-ue", action="store_true")
    ap.add_argument("--skip-gn", action="store_true")
    ap.add_argument("--display", default=":260")
    ap.add_argument("--gn-warmup", type=int, default=160)
    ap.add_argument("--gn-frames", type=int, default=45)
    args = ap.parse_args()

    specs = load_specs("scene_specs.json")
    wanted_scenes = None if args.scenes == "all" else set(args.scenes.split(","))
    wanted_cams = None if args.cams == "all" else set(args.cams.split(","))
    selected = [s for s in specs["scenes"] if wanted_scenes is None or s["id"] in wanted_scenes]

    # Step 1: UE camera map setup (pure CPU -nullrhi)
    if not args.skip_ue:
        ue_root()  # fail before Xvfb starts when UE_ROOT is unset
    if not args.skip_ue_setup and not args.skip_ue:
        for sc in selected:
            setup_ue_maps_for_scene(sc)

    # Step 2 & 3: Shared persistent Xvfb for UE 5.9 Vulkan and GodotNext Vulkan
    if not args.skip_ue or not args.skip_gn:
        xvfb = subprocess.Popen(
            ["Xvfb", args.display, "-screen", "0", "1920x1080x24", "-nolisten", "tcp", "-noreset", "+extension", "GLX"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(1.5)
            if not args.skip_ue:
                for sc in selected:
                    capture_ue_scene(sc, args.display, only_cams=wanted_cams)
            if not args.skip_gn:
                # Warm wake NVIDIA Vulkan ICD on this Xvfb
                w_env = os.environ.copy()
                w_env.pop("WAYLAND_DISPLAY", None)
                w_env["DISPLAY"] = args.display
                subprocess.run(
                    [str(GODOT_BIN), "--rendering-driver", "vulkan", "--quit", "--path", str(GODOT_PROJ)],
                    env=w_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30,
                )
                time.sleep(0.5)
                for sc in selected:
                    capture_godot_scene(sc, args.display, only_cams=wanted_cams, warmup=args.gn_warmup, frames=args.gn_frames)
        finally:
            xvfb.terminate()
            try:
                xvfb.wait(timeout=5)
            except Exception:
                xvfb.kill()


if __name__ == "__main__":
    main()
