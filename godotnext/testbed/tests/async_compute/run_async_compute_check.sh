#!/usr/bin/env bash
# Async compute fork/join check (#174) on Metal, under Metal API and shader validation.
#
# Usage (from any directory):
#   run_async_compute_check.sh [--scenes="<res path> ..."] [--strides="1 3"] [-- <godot binary>]
#
# The Godot binary defaults to bin/godot.macos.editor.arm64 in the repository. It must be a
# debug build (editor or template_debug): GODOT_RG_FORCE_ASYNC_COMPUTE only exists there.
#
# 1. async_compute_check.tscn on a local RenderingDevice with
#    rendering/rendering_device/async_compute off and on. Every readback must match the CPU
#    values. With the setting on, the device's "async:" frame counters must show the fork/join
#    and barrier cases submitted to the async queue and the overlapping chain fell back; off, they
#    must stay 0.
# 2. Harness captures (capture mode, HDR slots 5 and 11) of each scene with the setting off, then
#    on with GODOT_RG_FORCE_ASYNC_COMPUTE=<stride>, which moves every n-th compute list of a frame
#    to the async queue. Every capture must be byte-identical to the off run. The main device
#    must report forked frames ("submitted" above 0) in every forced run and none in the off run.
#
# Every run must log 0 "ERROR:", "Invalid Metal usage" and "Error detected in" lines. The script
# writes godotnext/testbed/override.cfg (git ignores it) and removes it again; it refuses to
# overwrite an existing one. Exits 0 when every check passes.

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
export MTL_DEBUG_LAYER=1
export MTL_SHADER_VALIDATION=1
export MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
unset GODOT_RG_FORCE_ASYNC_COMPUTE || true

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
repo_root=$(cd "$testbed_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

scenes="res://scenes/legacy/legacy_lightmap.tscn res://scenes/legacy/legacy_sdfgi.tscn res://scenes/legacy/legacy_voxelgi.tscn"
strides="1 3"
while [[ $# -gt 0 ]]; do
	case "$1" in
	--scenes=*) scenes="${1#--scenes=}" ;;
	--strides=*) strides="${1#--strides=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_async_compute_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
godot="${1:-$repo_root/bin/godot.macos.editor.arm64}"
if [[ ! -x "$godot" ]]; then
	echo "run_async_compute_check.sh: Godot binary is not executable: $godot" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_async_compute_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

work_root=$(mktemp -d "${TMPDIR:-/tmp}/godotnext-async-compute.XXXXXX")
override_written=0
cleanup() {
	if [[ "$override_written" == 1 ]]; then
		rm -f "$override"
	fi
}
trap cleanup EXIT

die() {
	echo "run_async_compute_check.sh: $1" >&2
	echo "run_async_compute_check.sh: logs in $work_root" >&2
	exit 1
}

write_override() { # write_override <true|false>
	cat >"$override" <<EOF
[autoload]

AsyncComputeReport="*res://tests/async_compute/async_compute_report.gd"

[rendering]

rendering_device/async_compute=$1
EOF
	override_written=1
}

remove_override() {
	rm -f "$override"
	override_written=0
}

# run_godot <log> <godot arguments...>: runs Godot and fails on its exit status or on any engine
# error or Metal validation report in the log.
run_godot() {
	local log=$1
	shift
	set +e
	"$godot" "$@" >"$log" 2>&1
	local status=$?
	set -e
	local errors usage shader
	errors=$(grep -c 'ERROR:' "$log" || true)
	usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	shader=$(grep -c 'Error detected in' "$log" || true)
	echo "  $(basename "$log" .log): exit=$status ERROR=$errors InvalidMetal=$usage ErrorDetected=$shader"
	if [[ $errors -ne 0 || $usage -ne 0 || $shader -ne 0 ]]; then
		grep -E 'ERROR:|Invalid Metal usage|Error detected in' "$log" | head -20 >&2 || true
		die "engine error or Metal validation report in $log"
	fi
	if [[ $status -ne 0 ]]; then
		tail -20 "$log" >&2 || true
		die "Godot exited $status ($log)"
	fi
}

# json_int <line> <key>
json_int() {
	printf '%s\n' "$1" | sed -n "s/.*\"$2\":\\([0-9]*\\).*/\\1/p"
}

echo "== Local-device fork/join fixture"
for setting in false true; do
	log="$work_root/fixture_async_$setting.log"
	write_override "$setting"
	run_godot "$log" --path "$testbed_dir" --rendering-driver metal --quit-after 600 res://tests/async_compute/async_compute_check.tscn
	remove_override
	result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
	echo "  $result"
	if [[ "$result" != *'"pass":true'* ]]; then
		die "the fixture failed with async_compute=$setting"
	fi
	if [[ "$setting" == true && "$result" != *'"active":true'* ]]; then
		die "async compute is not active with the setting on; this check needs a device with SUPPORTS_ASYNC_COMPUTE"
	fi
done

capture() { # capture <name> <scene>: the legacy identity capture, slots 5 and 11.
	local name=$1 scene=$2
	local out="$work_root/$name"
	mkdir -p "$out"
	run_godot "$work_root/$name.log" --path "$testbed_dir" --rendering-driver metal \
		--write-movie "$out/frame.png" --fixed-fps 60 --quit-after 1062 -- \
		--mode=capture --scene="$scene" --camera=CaptureCam --out="$out" \
		--seed=1 --frames=10 --warmup=2 --capture-frames=5,11
}

echo "== Scene captures, async compute off vs forced onto the async queue"
for scene in $scenes; do
	name=$(basename "$scene" .tscn)
	write_override false
	capture "${name}_off" "$scene"
	remove_override
	report=$(grep -m1 '^GODOTNEXT_ASYNC_COMPUTE_REPORT ' "$work_root/${name}_off.log" || true)
	echo "  $report"
	if [[ "$(json_int "$report" submitted)" != 0 || "$(json_int "$report" fell_back)" != 0 ]]; then
		die "$name with async compute off reported async frames: $report"
	fi
	for stride in $strides; do
		write_override true
		GODOT_RG_FORCE_ASYNC_COMPUTE=$stride capture "${name}_force$stride" "$scene"
		remove_override
		log="$work_root/${name}_force$stride.log"
		if ! grep -q "RG: Forcing every $stride compute list onto the async compute queue" "$log"; then
			die "$name stride $stride: the force switch did not engage (not a debug build, or no async queue)"
		fi
		report=$(grep -m1 '^GODOTNEXT_ASYNC_COMPUTE_REPORT ' "$log" || true)
		echo "  $report"
		submitted=$(json_int "$report" submitted)
		if [[ -z "$submitted" || "$submitted" -eq 0 ]]; then
			die "$name stride $stride: no frame forked to the async queue: $report"
		fi
		for slot in 5 11; do
			a="$work_root/${name}_off/hdr_$(printf '%05d' "$slot").exr"
			b="$work_root/${name}_force$stride/hdr_$(printf '%05d' "$slot").exr"
			if [[ ! -f "$a" || ! -f "$b" ]]; then
				die "missing capture $a or $b"
			fi
			if ! cmp -s "$a" "$b"; then
				die "$name stride $stride: HDR capture $slot differs from async compute off"
			fi
		done
		echo "  $name stride $stride: captures 5 and 11 match async compute off"
	done
done

echo "Async compute check passed (logs in $work_root)"
