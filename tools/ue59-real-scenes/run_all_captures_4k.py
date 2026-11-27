#!/usr/bin/env python3
"""4K Best-Quality comparison capture runner:
1) Configures matched 4K UE 5.9 camera maps (8K HDRI SkyLight, sun, warm punctual lights, subtle fog & bloom).
2) Captures native 3840x2160 UE 5.9 (Cinematic sg.*=4 + Nanite + Lumen HWRT Hit Lighting + VSM + TSR) PNGs + CSV GPU profiles.
3) Captures native 3840x2160 GodotNext (Epic NextGI HWRT Hit Lighting + Hit Lighting GI + NextGeo + VSM + TSR Cinematic) PNGs + GPU pass CSVs.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path("/home/legoless/Projects/legoless/GodotNext")
GODOT_BIN = REPO_ROOT / "bin/godot.linuxbsd.editor.x86_64"
UE_EDITOR = Path("/home/legoless/Projects/vendor/UnrealEngine/Engine/Binaries/Linux/UnrealEditor")
UE_EDITOR_CMD = Path("/home/legoless/Projects/vendor/UnrealEngine/Engine/Binaries/Linux/UnrealEditor-Cmd")
UE_SHOT_DIRS = [
    Path("/data2/Projects/vendor/UnrealEngine/Engine/Saved/Screenshots/LinuxEditor"),
    Path("/data2/Projects/vendor/UnrealEngine/Engine/Saved/Screenshots/Linux"),
]
REAL_ROOT = Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes")
GODOT_PROJ = REAL_ROOT / "godot_project"
COMP_ROOT = REAL_ROOT / "comparison_4k"

UE_EXEC_CMDS = [
    "r.ScreenPercentage 100",
    "sg.ResolutionQuality 100",
    "sg.ViewDistanceQuality 4",
    "sg.AntiAliasingQuality 4",
    "sg.ShadowQuality 4",
    "sg.GlobalIlluminationQuality 4",
    "sg.ReflectionQuality 4",
    "sg.PostProcessQuality 4",
    "sg.TextureQuality 4",
    "sg.EffectsQuality 4",
    "sg.FoliageQuality 4",
    "sg.ShadingQuality 4",
    "sg.LandscapeQuality 4",
    "r.AntiAliasingMethod 4",
    "r.TSR.History.ScreenPercentage 100",
    "r.Streaming.PoolSize 6000",
    "r.Streaming.FullyLoadUsedTextures 1",
    "r.MaxAnisotropy 16",
    "r.Lumen.HardwareRayTracing 1",
    "r.Lumen.HardwareRayTracing.LightingMode 2",
    "r.Shadow.Virtual.Enable 1",
    "r.Nanite 1",
    "r.RDG.AsyncCompute 0",
    "r.Lumen.AsyncCompute 0",
    "r.TSR.AsyncCompute 0",
]


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
    cmd = [
        str(UE_EDITOR_CMD),
        sc["ue_project"],
        "-nullrhi", "-unattended", "-nosplash", "-nop4", "-NoSound",
        f"-ExecutePythonScript={REAL_ROOT / 'setup_ue_comparison_4k.py'}",
    ]
    log_p = COMP_ROOT / sid / "ue_setup_cams.log"
    print(f"[{sid}] Setting up {len(sc['cameras'])} UE 5.9 4K camera maps...", flush=True)
    with open(log_p, "w") as lf:
        res = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=360)
    print(f"[{sid}] UE camera map setup rc={res.returncode}", flush=True)


def capture_ue_scene(sc: dict, display: str, only_cams: set[str] | None = None, warmup_delay: int = 140, total_frames: int = 185):
    sid = sc["id"]
    ue_proj = Path(sc["ue_project"])
    out_dir = COMP_ROOT / sid
    out_dir.mkdir(parents=True, exist_ok=True)
    shot_dirs = UE_SHOT_DIRS + [
        ue_proj.parent / "Saved/Screenshots/LinuxEditor",
        ue_proj.parent / "Saved/Screenshots/Linux",
    ]
    csv_dirs = [
        ue_proj.parent / "Saved/Profiling/CSV",
        Path("/data2/Projects/vendor/UnrealEngine/Engine/Saved/Profiling/CSV"),
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
            "HighResShot 1",
        ]
        log_p = out_dir / f"{cid}_ue.log"
        cmd = [
            str(UE_EDITOR),
            str(ue_proj),
            cam_map,
            "-game", "-vulkan", "-sm6", "-RenderOffScreen", "-NoTextureStreaming",
            "-ResX=3840", "-ResY=2160",
            "-novsync", "-unattended", "-nosplash", "-NoSound", "-NoScreenMessages",
            "-benchmark", "-fps=60", "-deterministic",
            "-csvGpuStats", f"-csvCaptureFrames={total_frames}", "-ExitAfterCsvProfiling",
            f"-csvMetadata=compare4k={sid}_{cid}",
            f"-abslog={log_p}",
            "-ExecCmds=" + ",".join(exec_cmds),
        ]
        dst_png = out_dir / f"{cid}_ue59.png"
        dst_csv = out_dir / f"{cid}_ue_profile.csv"
        for attempt in range(1, 5):
            t0 = time.time() - 1.0
            print(f"  [UE 5.9 4K] {sid}/{cid} ({cam_map}, attempt {attempt})...", flush=True)
            res = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=480)
            shot = newest_file(shot_dirs, "*.png", t0)
            csv_p = newest_file(csv_dirs, "*.csv", t0)
            if res.returncode == 0 and shot is not None and shot.stat().st_size > 10000:
                shutil.copy2(shot, dst_png)
                if csv_p is not None:
                    shutil.copy2(csv_p, dst_csv)
                print(f"    -> rc=0 png=True ({dst_png.stat().st_size:,} B) csv={dst_csv.exists()}", flush=True)
                break
            print(f"    -> transient rc={res.returncode} shot={shot is not None}, retrying...", flush=True)
            time.sleep(1.5)


def capture_godot_scene(sc: dict, display: str, only_cams: set[str] | None = None, warmup: int = 180, frames: int = 45):
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
        "sun_color": sc.get("sun_color", [1.0, 0.94, 0.85]),
        "sun_energy": sc.get("gn_sun_energy", 5.0),
        "sky_energy": sc.get("gn_sky_energy", 1.8),
        "sky_hdri": sc.get("gn_sky_hdri", "res://assets/sky/kloppenheim_06_puresky_8k.hdr"),
        "sky_rotation_deg": sc.get("gn_sky_rotation_deg", sc.get("sky_rotation_deg", 0.0)),
        "exposure": sc.get("gn_exposure", 1.1),
        "light_color_is_linear": sc.get("gn_light_color_is_linear", False),
        "tonemap_mode": sc.get("gn_tonemap_mode", "agx"),
        "glow_intensity": sc.get("gn_glow_intensity", 0.14),
        "glow_hdr_threshold": sc.get("gn_glow_hdr_threshold", 2.0),
        "fog_density": sc.get("fog_density", 0.0),
        "fog_color": sc.get("fog_color", [0.78, 0.83, 0.90]),
        "fog_light_energy": sc.get("gn_fog_light_energy", 0.75),
        "fog_aerial_perspective": sc.get("gn_fog_aerial_perspective", 0.45),
        "fog_sky_affect": sc.get("gn_fog_sky_affect", 0.15),
        "point_lights": sc.get("point_lights", []),
        "warmup": warmup,
        "frames": frames,
        "cameras": cams,
    }
    cfg_path = out_dir / "godotnext_config.json"
    cfg_path.write_text(json.dumps(cfg, indent=2))

    override_p = GODOT_PROJ / "override.cfg"
    override_p.write_text(
        "[rendering]\n"
        "limits/nextgeo/geometry_cache_mb=2048\n"
        "limits/nextgeo/page_upload_kib_per_frame=65536\n"
        "nextgeo/max_pixel_error=1.0\n"
        "global_illumination/nextgi/enabled=true\n"
        "global_illumination/nextgi/hardware_ray_tracing=2\n"
        "global_illumination/nextgi/quality=3\n"
        "global_illumination/nextgi/ray_lighting_mode=1\n"
        "global_illumination/nextgi/hit_lighting_gi=true\n"
        "global_illumination/nextgi/max_reflection_bounces=2\n"
        "lights_and_shadows/shadow_method=1\n"
        "lights_and_shadows/virtual_shadows/quality=3\n"
        "anti_aliasing/tsr/quality=4\n"
        "anti_aliasing/tsr/thin_geometry_detection=true\n"
        "anti_aliasing/quality/use_debanding=true\n"
        "textures/default_filters/anisotropic_filtering_level=4\n"
    )
    env = os.environ.copy()
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("XAUTHORITY", None)
    env["DISPLAY"] = display
    env["GODOTNEXT_BACKGROUND"] = "1"

    log_p = out_dir / "godotnext.log"
    cmd = [
        str(GODOT_BIN),
        "--rendering-driver", "vulkan",
        "--resolution", "3840x2160",
        "--disable-vsync",
        "--gpu-profile",
        "--path", str(GODOT_PROJ),
        "res://capture_multi_cam_4k.tscn",
        "--",
        f"--config={cfg_path}",
    ]
    try:
        for attempt in range(1, 7):
            print(f"  [GodotNext 4K] {sid} ({len(cams)} cams, attempt {attempt})...", flush=True)
            with open(log_p, "w") as lf:
                res = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=720)
            txt = log_p.read_text(errors="replace")
            if "VkResult error -3" in txt:
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
    ap.add_argument("--display", default=":262")
    ap.add_argument("--gn-warmup", type=int, default=180)
    ap.add_argument("--gn-frames", type=int, default=45)
    args = ap.parse_args()

    specs = json.loads((REAL_ROOT / "scene_specs_4k.json").read_text())
    wanted_scenes = None if args.scenes == "all" else set(args.scenes.split(","))
    wanted_cams = None if args.cams == "all" else set(args.cams.split(","))
    selected = [s for s in specs["scenes"] if wanted_scenes is None or s["id"] in wanted_scenes]

    if not args.skip_ue_setup and not args.skip_ue:
        for sc in selected:
            setup_ue_maps_for_scene(sc)

    if not args.skip_ue or not args.skip_gn:
        subprocess.run(["nvidia-smi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        fds = []
        for dev in ("/dev/nvidiactl", "/dev/nvidia0", "/dev/nvidia-modeset"):
            try:
                fds.append(os.open(dev, os.O_RDONLY))
            except OSError:
                pass
        xvfb = subprocess.Popen(
            ["Xvfb", args.display, "-screen", "0", "3840x2160x24", "-nolisten", "tcp", "-noreset", "+extension", "GLX"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(1.5)
            if not args.skip_ue:
                for sc in selected:
                    capture_ue_scene(sc, args.display, only_cams=wanted_cams)
            if not args.skip_gn:
                for sc in selected:
                    capture_godot_scene(sc, args.display, only_cams=wanted_cams, warmup=args.gn_warmup, frames=args.gn_frames)
        finally:
            xvfb.terminate()
            try:
                xvfb.wait(timeout=5)
            except Exception:
                xvfb.kill()
            for fd in fds:
                try:
                    os.close(fd)
                except OSError:
                    pass


if __name__ == "__main__":
    main()
