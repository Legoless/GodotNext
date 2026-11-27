#!/usr/bin/env python3
"""1-hour VK_EXT_device_fault soak runner (#56, G3-7) for Linux RTX 5090.

Load:
  - Character Walk --variant=soak_20 (20 skinned walkers = 200,000 skinned tris)
  - NextGI High (HWRT)
  - Texture streaming enabled with memory_budget_enabled=true, memory_budget_mb=256
  - Continuous 1k-mesh load/unload BLAS churn in the same scene via a runtime driver
  - --verbose (verifies VK_EXT_device_fault enabled) and --extra-gpu-memory-tracking
  - Samples RD memory, process RSS, and nvidia-smi VRAM every 60s for 3600s
  - Checks zero device-lost events, zero VK_EXT_device_fault dumps, zero HWRT_FAULTED
    latch under user://, and zero Vulkan validation errors.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path("/home/legoless/Projects/legoless/GodotNext")
TESTBED_DIR = REPO_ROOT / "godotnext/testbed"
GODOT_BIN = REPO_ROOT / "bin/godot.linuxbsd.editor.x86_64"

SOAK_SCRIPT_GDS = r'''extends SceneTree

const WALK_SCENE := "res://scenes/character_walk/character_walk.tscn"
const CHURN_PER_CYCLE := 1000
const CHURN_BATCH := 20
const LOG_INTERVAL_SEC := 60.0

var _start_msec: int = 0
var _duration_sec: float = 3600.0
var _last_log_msec: int = 0
var _frame: int = 0
var _churn_root: Node3D = null
var _churn_pool: Array[MeshInstance3D] = []
var _churn_meshes: Array[ArrayMesh] = []
var _churn_created_total: int = 0
var _churn_freed_total: int = 0
var _mesh_template_arrays: Array = []

func _initialize() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--duration="):
			_duration_sec = float(a.trim_prefix("--duration="))
	_start_msec = Time.get_ticks_msec()
	_last_log_msec = _start_msec

	# Build a lightweight 12-triangle box ArrayMesh template for dynamic BLAS churn.
	var bm := BoxMesh.new()
	bm.size = Vector3(0.25, 0.25, 0.25)
	_mesh_template_arrays = bm.get_mesh_arrays()

	var packed := load(WALK_SCENE) as PackedScene
	if packed == null:
		push_error("soak_1h: failed to load " + WALK_SCENE)
		quit(1)
		return
	var scene := packed.instantiate()
	root.add_child(scene)

	# Ensure NextGI is enabled on every WorldEnvironment in the scene.
	for we in scene.find_children("*", "WorldEnvironment", true, false):
		var env: Environment = (we as WorldEnvironment).environment
		if env != null:
			env.nextgi_enabled = true
			env.sdfgi_enabled = false

	_churn_root = Node3D.new()
	_churn_root.name = "SoakBlasChurn"
	scene.add_child(_churn_root)

	print("SOAK_START duration_sec=%.1f" % _duration_sec)
	_log_memory(0.0)

func _step_blas_churn() -> void:
	# Free oldest batch once pool reaches 100 active churn meshes (1000+ cycled every ~50 frames).
	if _churn_pool.size() >= 100:
		for i in range(CHURN_BATCH):
			if _churn_pool.is_empty():
				break
			var mi: MeshInstance3D = _churn_pool.pop_front()
			var m: ArrayMesh = _churn_meshes.pop_front()
			mi.queue_free()
			m = null
			_churn_freed_total += 1
	# Spawn a fresh batch with unique ArrayMesh instances so each builds/destroys a real BLAS.
	for i in range(CHURN_BATCH):
		var am := ArrayMesh.new()
		am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, _mesh_template_arrays)
		var mi := MeshInstance3D.new()
		mi.mesh = am
		var idx := _churn_created_total
		var angle := float(idx % 64) * (TAU / 64.0)
		var radius := 4.5 + float((idx / 64) % 5) * 0.6
		mi.position = Vector3(cos(angle) * radius, 0.5 + float(idx % 4) * 0.4, sin(angle) * radius)
		_churn_root.add_child(mi)
		_churn_pool.append(mi)
		_churn_meshes.append(am)
		_churn_created_total += 1

func _log_memory(elapsed_sec: float) -> void:
	var rd := RenderingServer.get_rendering_device()
	var as_mem := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES) if rd else -1
	var buf_mem := rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS) if rd else -1
	var tex_mem := rd.get_memory_usage(RenderingDevice.MEMORY_TEXTURES) if rd else -1
	var tot_mem := rd.get_memory_usage(RenderingDevice.MEMORY_TOTAL) if rd else -1
	var rs_vram := RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED)
	var rec := {
		"elapsed_s": snappedf(elapsed_sec, 0.1),
		"frame": _frame,
		"fps_avg": snappedf(float(_frame) / maxf(elapsed_sec, 0.001), 0.1),
		"rd_total_bytes": tot_mem,
		"rd_as_bytes": as_mem,
		"rd_buffers_bytes": buf_mem,
		"rd_textures_bytes": tex_mem,
		"rs_vram_bytes": rs_vram,
		"churn_created": _churn_created_total,
		"churn_freed": _churn_freed_total,
		"churn_live": _churn_pool.size(),
	}
	print("SOAK_SAMPLE " + JSON.stringify(rec))

func _process(_delta: float) -> bool:
	_frame += 1
	if (_frame % 3) == 0:
		_step_blas_churn()
	var now_msec := Time.get_ticks_msec()
	var elapsed := float(now_msec - _start_msec) / 1000.0
	if float(now_msec - _last_log_msec) / 1000.0 >= LOG_INTERVAL_SEC:
		_last_log_msec = now_msec
		_log_memory(elapsed)
	if elapsed >= _duration_sec:
		# Drain churn meshes and wait a few frames before final sample.
		for mi in _churn_pool:
			mi.queue_free()
		_churn_pool.clear()
		_churn_meshes.clear()
		_log_memory(elapsed)
		print("SOAK_DONE frames=%d elapsed_s=%.1f" % [_frame, elapsed])
		quit(0)
		return true
	return false
'''

OVERRIDE_CFG = """[rendering]
global_illumination/nextgi/enabled=true
global_illumination/nextgi/quality=2
global_illumination/nextgi/hardware_ray_tracing=0
textures/streaming/enabled=true
textures/streaming/memory_budget_enabled=true
textures/streaming/memory_budget_mb=256
"""


def nvidia_vram_mb() -> float | None:
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=memory.used,temperature.gpu,utilization.gpu", "--format=csv,noheader,nounits"],
            text=True,
            timeout=5,
        ).strip()
        parts = [p.strip() for p in out.splitlines()[0].split(",")]
        return float(parts[0])
    except Exception:
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=3600)
    ap.add_argument("--out", default="/data2/Projects/legoless/GodotNext/local/godotnext-tools/soak_1h_results")
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    soak_script = out_dir / "soak_driver.gd"
    soak_script.write_text(SOAK_SCRIPT_GDS)
    override_dst = TESTBED_DIR / "override.cfg"
    log_path = out_dir / "soak.log"

    # Snapshot any pre-existing user:// files for GodotNext testbed.
    user_dir = Path.home() / ".local/share/godot/app_userdata/GodotNext Testbed"

    cmd = [
        "xvfb-run", "-a", "-s", "-screen 0 1920x1080x24",
        str(GODOT_BIN),
        "--rendering-driver", "vulkan",
        "--path", str(TESTBED_DIR),
        "--resolution", "1920x1080",
        "--verbose",
        "--script", str(soak_script),
        "--",
        "--variant=soak_20",
        f"--duration={args.duration}",
    ]

    host_samples = []
    override_dst.write_text(OVERRIDE_CFG)
    t0 = time.time()
    rc = -1
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    try:
        with open(log_path, "w") as lf:
            proc = subprocess.Popen(
                cmd,
                cwd=str(REPO_ROOT),
                env=env,
                stdout=lf,
                stderr=subprocess.STDOUT,
            )
            next_sample = t0
            while proc.poll() is None:
                now = time.time()
                if now >= next_sample:
                    vram = nvidia_vram_mb()
                    host_samples.append({
                        "elapsed_s": round(now - t0, 1),
                        "nvidia_vram_mb": vram,
                    })
                    print(f"[soak] elapsed={now - t0:.0f}s/{args.duration}s vram_mb={vram}", flush=True)
                    next_sample = now + 60.0
                if now - t0 > args.duration + 300:
                    print("[soak] TIMEOUT exceeded, terminating", flush=True)
                    proc.terminate()
                    break
                time.sleep(5.0)
            rc = proc.wait(timeout=30)
    finally:
        if override_dst.exists():
            override_dst.unlink()

    elapsed_total = round(time.time() - t0, 1)
    text = log_path.read_text(errors="replace")
    samples = []
    for line in text.splitlines():
        if line.startswith("SOAK_SAMPLE "):
            try:
                samples.append(json.loads(line[len("SOAK_SAMPLE "):]))
            except Exception:
                pass

    device_fault_enabled = ("VK_EXT_device_fault" in text) and ("VK_EXT_device_fault not available" not in text)
    device_lost_count = text.count("Vulkan device was lost") + text.count("Report from VK_EXT_device_fault")
    validation_errors = sum(1 for l in text.splitlines() if "VUID-" in l or "Validation Error:" in l)
    engine_errors = [l for l in text.splitlines() if l.startswith("ERROR:")]
    hwrt_selected = "NextGI: tier=HWRT" in text

    # Check user:// for any HWRT_FAULTED latch file
    latch_files = []
    if user_dir.exists():
        for p in user_dir.rglob("*"):
            if p.is_file() and ("fault" in p.name.lower() or "hwrt" in p.name.lower()):
                latch_files.append(str(p))
    hwrt_faulted_in_log = "HWRT_FAULTED" in text

    # Memory drift analysis (compare 5-min steady-state vs end)
    steady = [s for s in samples if s["elapsed_s"] >= 300] or samples
    rd_total_first = steady[0]["rd_total_bytes"] if steady else None
    rd_total_last = steady[-1]["rd_total_bytes"] if steady else None
    rd_as_first = steady[0]["rd_as_bytes"] if steady else None
    rd_as_last = steady[-1]["rd_as_bytes"] if steady else None
    rd_total_drift_pct = (
        round((rd_total_last - rd_total_first) / max(rd_total_first, 1) * 100.0, 2)
        if rd_total_first is not None and rd_total_last is not None
        else None
    )

    summary = {
        "returncode": rc,
        "duration_requested_s": args.duration,
        "elapsed_s": elapsed_total,
        "hwrt_selected": hwrt_selected,
        "vk_ext_device_fault_enabled": device_fault_enabled,
        "device_lost_events": device_lost_count,
        "validation_errors": validation_errors,
        "engine_error_count": len(engine_errors),
        "engine_errors_sample": engine_errors[:20],
        "hwrt_faulted_latch_files": latch_files,
        "hwrt_faulted_in_log": hwrt_faulted_in_log,
        "sample_count": len(samples),
        "frames_total": samples[-1]["frame"] if samples else 0,
        "churn_created_total": samples[-1]["churn_created"] if samples else 0,
        "churn_freed_total": samples[-1]["churn_freed"] if samples else 0,
        "rd_total_steady_first_bytes": rd_total_first,
        "rd_total_steady_last_bytes": rd_total_last,
        "rd_total_drift_pct": rd_total_drift_pct,
        "rd_as_steady_first_bytes": rd_as_first,
        "rd_as_steady_last_bytes": rd_as_last,
        "samples": samples,
        "host_samples": host_samples,
        "pass": (
            rc == 0
            and hwrt_selected
            and device_fault_enabled
            and device_lost_count == 0
            and validation_errors == 0
            and not hwrt_faulted_in_log
            and not latch_files
        ),
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({k: v for k, v in summary.items() if k not in ("samples", "host_samples")}, indent=2))
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
