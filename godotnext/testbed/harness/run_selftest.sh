#!/usr/bin/env bash
# Self-test driver for the harness.
# Default: timing three times (warmup 120, frames 600) and capture twice.
# Capture hashes must match. Timing spread is printed and does not fail the run.

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../../.." && pwd)
testbed="$repo_root/godotnext/testbed"
scene="res://harness/selftest/selftest.tscn"
camera="CaptureCam"

Mode="all"
Runs=0
Frames=0
Warmup=-1
Seed=1
Scene=""
Camera=""
Label=""

usage() {
	cat <<'EOF'
Usage: run_selftest.sh [-Mode capture|timing|all] [-Runs N] [-Frames N] [-Warmup N] [-Seed N] [-Scene res://...] [-Camera name] [-Label name]

Capture defaults are warmup 4, frames 8, and EXR slots at the warmup index and
three frames later. Timing defaults are warmup 120 and frames 600.
-Frames and -Warmup override those defaults for the selected mode.
-Scene and -Camera default to the self-test scene and its CaptureCam.
-Label names the captures/<label> output folder; it defaults to selftest,
or to the scene file name when -Scene is given without one.
On macOS the launch goes through runners/macos_local.sh.
On Linux, timing is rejected and capture goes through runners/linux_vm.sh.
The runner applies a per-scene override.cfg next to -Scene for that launch
only (#240) and records it in the manifest.
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	-Mode)
		Mode=$2
		shift 2
		;;
	-Runs)
		Runs=$2
		shift 2
		;;
	-Frames)
		Frames=$2
		shift 2
		;;
	-Warmup)
		Warmup=$2
		shift 2
		;;
	-Seed)
		Seed=$2
		shift 2
		;;
	-Scene)
		Scene=$2
		shift 2
		;;
	-Camera)
		Camera=$2
		shift 2
		;;
	-Label)
		Label=$2
		shift 2
		;;
	--help|-h)
		usage
		exit 0
		;;
	*)
		echo "run_selftest.sh: unknown argument $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if [[ -n "$Scene" ]]; then
	scene=$Scene
fi
if [[ -n "$Camera" ]]; then
	camera=$Camera
fi
if [[ -z "$Label" ]]; then
	if [[ -n "$Scene" ]]; then
		Label=$(basename "$scene" .tscn)
	else
		Label=selftest
	fi
fi
case "$Label" in
*/*|..|"")
	echo "run_selftest.sh: -Label must be one folder name" >&2
	exit 2
	;;
esac
captures_root="$testbed/captures/$Label"

case "$Mode" in
capture|timing|all) ;;
*)
	echo "run_selftest.sh: -Mode must be capture, timing, or all" >&2
	exit 2
	;;
esac

case "$(uname -s)" in
Darwin) host_kind=macos ;;
Linux) host_kind=linux ;;
*)
	echo "run_selftest.sh: this host is not the Apple M5 Max runner or the Linux VM. The Windows runner is issue 237." >&2
	exit 2
	;;
esac

if [[ "$host_kind" == "linux" && "$Mode" != "capture" ]]; then
	echo "run_selftest.sh: the Linux VM runner never runs timing mode" >&2
	exit 1
fi

godot_default="$repo_root/bin/godot.macos.editor.arm64"
if [[ "$host_kind" == "linux" ]]; then
	godot_default="$repo_root/bin/godot.linuxbsd.editor.x86_64"
fi
GODOT="${GODOT:-$godot_default}"

capture_warmup=4
capture_frame_count=8
timing_warmup=120
timing_frame_count=600
if [[ $Warmup -ge 0 ]]; then
	capture_warmup=$Warmup
	timing_warmup=$Warmup
fi
if [[ $Frames -gt 0 ]]; then
	capture_frame_count=$Frames
	timing_frame_count=$Frames
fi

capture_runs=2
timing_runs=3
if [[ $Runs -gt 0 ]]; then
	capture_runs=$Runs
	timing_runs=$Runs
fi

quit_after_for() {
	warmup_value=$1
	frame_value=$2
	# The movie writer quit-after counts settle draws as well as the captured
	# window. 900 is the specialization budget, 30 is the flat streak, and 120
	# is margin. The harness quits itself when the window finishes.
	echo $((900 + 30 + 120 + warmup_value + frame_value))
}

capture_list_for() {
	warmup_value=$1
	frame_value=$2
	window=$((warmup_value + frame_value))
	slot_a=$warmup_value
	slot_b=$((warmup_value + 3))
	if [[ $slot_a -ge $window ]]; then
		slot_a=$((window - 1))
	fi
	if [[ $slot_b -ge $window ]]; then
		slot_b=$((window - 1))
	fi
	if [[ $slot_a -eq $slot_b ]]; then
		printf '%s\n' "$slot_a"
	else
		printf '%s,%s\n' "$slot_a" "$slot_b"
	fi
}

launch_godot() {
	if [[ "$host_kind" == "macos" ]]; then
		"$script_dir/runners/macos_local.sh" -- "$GODOT" "$@"
	else
		# linux_vm.sh functional supplies the binary and --headless itself.
		"$script_dir/runners/linux_vm.sh" functional -- "$@"
	fi
}

run_capture() {
	index=$1
	out="$captures_root/capture_${index}"
	rm -rf "$out"
	mkdir -p "$out"
	list=$(capture_list_for "$capture_warmup" "$capture_frame_count")
	quit_after=$(quit_after_for "$capture_warmup" "$capture_frame_count")
	log="$out/run.log"
	set +e
	if [[ "$host_kind" == "macos" ]]; then
		launch_godot \
			--path "$testbed" --rendering-driver metal \
			--write-movie "$out/frame.png" --fixed-fps 60 --quit-after "$quit_after" -- \
			--mode=capture --scene="$scene" --camera="$camera" --out="$out" \
			--seed="$Seed" --frames="$capture_frame_count" --warmup="$capture_warmup" \
			--capture-frames="$list" >"$log" 2>&1
	else
		launch_godot \
			--path "$testbed" \
			--write-movie "$out/frame.png" --fixed-fps 60 --quit-after "$quit_after" -- \
			--mode=capture --scene="$scene" --camera="$camera" --out="$out" \
			--seed="$Seed" --frames="$capture_frame_count" --warmup="$capture_warmup" \
			--capture-frames="$list" >"$log" 2>&1
	fi
	status=$?
	set -e
	if [[ -e "$testbed/override.cfg" ]]; then
		echo "run_selftest.sh: the runner left a root override.cfg behind" >&2
		exit 1
	fi
	# write_end() runs after the harness trims the movie, so remove any wav
	# the writer still has open at process exit.
	rm -f "$out"/*.wav
	if [[ $status -ne 0 ]]; then
		echo "run_selftest.sh: capture run $index exited $status" >&2
		exit "$status"
	fi
	if ! grep -q "harness: capture complete" "$log"; then
		echo "run_selftest.sh: capture run $index did not finish the captured window" >&2
		exit 1
	fi
	KEEP=$out EXPECTED_PNG=$((capture_warmup + capture_frame_count)) python3 - <<'PY'
import os
import sys

root = os.environ["KEEP"]
expected = int(os.environ["EXPECTED_PNG"])
pngs = [name for name in os.listdir(root) if name.endswith(".png")]
exrs = [name for name in os.listdir(root) if name.endswith(".exr")]
wavs = [name for name in os.listdir(root) if name.endswith(".wav")]
if len(pngs) != expected:
    sys.stderr.write("capture kept %d png files, expected %d\n" % (len(pngs), expected))
    sys.exit(1)
if not exrs:
    sys.stderr.write("capture wrote no exr files\n")
    sys.exit(1)
if wavs:
    sys.stderr.write("capture left a wav file behind\n")
    sys.exit(1)
print("capture files: png=%d exr=%d" % (len(pngs), len(exrs)))
PY
}

compare_captures() {
	if [[ "$capture_runs" -lt 2 ]]; then
		echo "run_selftest.sh: one capture run, hash comparison skipped"
		return
	fi
	CAPTURE_ROOT="$captures_root" CAPTURE_RUNS="$capture_runs" python3 - <<'PY'
import hashlib
import os
import sys

root = os.environ["CAPTURE_ROOT"]
runs = int(os.environ["CAPTURE_RUNS"])

def collect(directory):
    rows = []
    pngs = 0
    exrs = 0
    for name in sorted(os.listdir(directory)):
        if not (name.endswith(".png") or name.endswith(".exr")):
            continue
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            continue
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        rows.append((name, digest))
        if name.endswith(".png"):
            pngs += 1
        else:
            exrs += 1
    return rows, pngs, exrs

base_dir = os.path.join(root, "capture_1")
base, pngs, exrs = collect(base_dir)
if pngs == 0 or exrs == 0:
    sys.stderr.write("capture_1 has no png/exr pair to compare\n")
    sys.exit(1)
for index in range(2, runs + 1):
    other, other_pngs, other_exrs = collect(os.path.join(root, "capture_%d" % index))
    if other != base:
        base_map = dict(base)
        other_map = dict(other)
        names = sorted(set(base_map) | set(other_map))
        for name in names:
            if base_map.get(name) != other_map.get(name):
                sys.stderr.write("hash mismatch: %s\n" % name)
        sys.exit(1)
    if other_pngs != pngs or other_exrs != exrs:
        sys.stderr.write("file count mismatch on capture_%d\n" % index)
        sys.exit(1)
print("capture sha256: png=%d exr=%d runs=%d all matched" % (pngs, exrs, runs))
PY
}

check_manifest() {
	out=$1
	TESTBED_DIR="$testbed" SCENE_RES="$scene" python3 - "$out/manifest.json" <<'PY'
import hashlib
import json
import os
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)

scene = os.environ["SCENE_RES"]
folder = scene[len("res://"):].rsplit("/", 1)[0] if "/" in scene[len("res://"):] else ""
override_path = os.path.join(os.environ["TESTBED_DIR"], folder, "override.cfg") if folder else os.path.join(os.environ["TESTBED_DIR"], "override.cfg")
record = data.get("override")
if not isinstance(record, dict):
    sys.stderr.write("manifest missing the override record\n")
    sys.exit(1)
if os.path.isfile(override_path):
    expected_source = "res://" + (folder + "/" if folder else "") + "override.cfg"
    if record.get("applied") is not True or record.get("source") != expected_source:
        sys.stderr.write("manifest override record does not name %s\n" % expected_source)
        sys.exit(1)
    digest = hashlib.sha256(open(override_path, "rb").read()).hexdigest()
    if record.get("sha256") != digest:
        sys.stderr.write("manifest override sha256 does not match the scene file\n")
        sys.exit(1)
elif record.get("applied") is not False or record.get("source") or record.get("sha256"):
    sys.stderr.write("manifest override record should be unused for this scene\n")
    sys.exit(1)
missing = [key for key in ("scaling_3d_mode", "scaling_3d_scale", "output_size") if key not in data]
if missing:
    sys.stderr.write("manifest missing %s\n" % ", ".join(missing))
    sys.exit(1)
size = data["output_size"]
if not isinstance(size, list) or len(size) != 2:
    sys.stderr.write("manifest output_size is not [width, height]\n")
    sys.exit(1)
if int(size[0]) <= 0 or int(size[1]) <= 0:
    sys.stderr.write("manifest output_size is empty\n")
    sys.exit(1)
power = data["runner"]["power"]
for key in ("on_ac", "powermode", "powermode_name", "thermal", "before", "after"):
    if key not in power:
        sys.stderr.write("manifest runner.power missing %s\n" % key)
        sys.exit(1)
if "dlss" in data:
    sys.stderr.write("self-test manifest unexpectedly included dlss\n")
    sys.exit(1)
print("manifest fields: scaling_3d_mode scaling_3d_scale output_size runner.power")
PY
}

run_timing() {
	index=$1
	out="$captures_root/timing_${index}"
	rm -rf "$out"
	mkdir -p "$out"
	quit_after=$(quit_after_for "$timing_warmup" "$timing_frame_count")
	log="$out/run.log"
	set +e
	if [[ "$host_kind" == "macos" ]]; then
		launch_godot \
			--path "$testbed" --rendering-driver metal --disable-vsync --gpu-profile \
			--quit-after "$quit_after" -- \
			--mode=timing --scene="$scene" --camera="$camera" --out="$out" \
			--seed="$Seed" --frames="$timing_frame_count" --warmup="$timing_warmup" >"$log" 2>&1
	else
		launch_godot \
			--path "$testbed" --disable-vsync --gpu-profile \
			--quit-after "$quit_after" -- \
			--mode=timing --scene="$scene" --camera="$camera" --out="$out" \
			--seed="$Seed" --frames="$timing_frame_count" --warmup="$timing_warmup" >"$log" 2>&1
	fi
	status=$?
	set -e
	if [[ -e "$testbed/override.cfg" ]]; then
		echo "run_selftest.sh: the runner left a root override.cfg behind" >&2
		exit 1
	fi
	if [[ $status -ne 0 ]]; then
		echo "run_selftest.sh: timing run $index exited $status" >&2
		exit "$status"
	fi
	if ! grep -q "harness: timing complete" "$log"; then
		echo "run_selftest.sh: timing run $index did not finish" >&2
		exit 1
	fi
	python3 - "$out/summary.json" "$out/timing.csv" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    summary = json.load(handle)
with open(sys.argv[2], encoding="utf-8") as handle:
    header = handle.readline().rstrip("\n").split(",")
    rows = [line for line in handle if line.strip()]
if summary.get("schema") != 1:
    sys.stderr.write("summary.json schema is not 1\n")
    sys.exit(1)
if header[:2] != ["frame", "wall_ms"]:
    sys.stderr.write("timing.csv header is not frame,wall_ms,...\n")
    sys.exit(1)
if len(rows) != int(summary.get("frames", -1)):
    sys.stderr.write("timing.csv row count does not match summary.json\n")
    sys.exit(1)
columns = summary.get("columns", {})
for name in header[1:]:
    column = columns.get(name)
    if not isinstance(column, dict):
        sys.stderr.write("summary.json missing column %s\n" % name)
        sys.exit(1)
    for key in ("p50", "p95", "p99", "n"):
        if key not in column:
            sys.stderr.write("summary.json column %s missing %s\n" % (name, key))
            sys.exit(1)
wall_n = int(columns["wall_ms"]["n"])
if wall_n != len(rows):
    sys.stderr.write("wall_ms n does not match the csv row count\n")
    sys.exit(1)
print("timing files well formed: rows=%d columns=%s" % (len(rows), ",".join(header)))
PY
}

report_spread() {
	if [[ "$timing_runs" -lt 2 ]]; then
		echo "timing: one run, spread is not computed here (the spread gate is PQ)"
		return
	fi
	TIMING_ROOT="$captures_root" TIMING_RUNS="$timing_runs" python3 - <<'PY'
import json
import os

root = os.environ["TIMING_ROOT"]
runs = int(os.environ["TIMING_RUNS"])
values = []
for index in range(1, runs + 1):
    path = os.path.join(root, "timing_%d" % index, "summary.json")
    with open(path, encoding="utf-8") as handle:
        summary = json.load(handle)
    values.append(float(summary["columns"]["wall_ms"]["p95"]))
low = min(values)
high = max(values)
if low == 0:
    spread = 0.0
else:
    spread = (high - low) / low * 100.0
print("timing spread line (not a failure, PQ gates it): wall_ms p95 spread %.4f%% across %d runs" % (spread, runs))
PY
}

if [[ "$Mode" == "capture" || "$Mode" == "all" ]]; then
	run_index=1
	while [[ $run_index -le $capture_runs ]]; do
		run_capture "$run_index"
		check_manifest "$captures_root/capture_${run_index}"
		run_index=$((run_index + 1))
	done
	compare_captures
fi

if [[ "$Mode" == "timing" || "$Mode" == "all" ]]; then
	run_index=1
	while [[ $run_index -le $timing_runs ]]; do
		run_timing "$run_index"
		check_manifest "$captures_root/timing_${run_index}"
		run_index=$((run_index + 1))
	done
	report_spread
fi
