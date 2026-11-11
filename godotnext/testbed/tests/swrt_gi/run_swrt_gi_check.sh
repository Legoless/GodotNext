#!/usr/bin/env bash
# Runs the software-tier GI checks (#186): each mode first runs a hardware-tier reference pass on
# the same view, then the software-tier pass against it.
#
# Usage:
#   run_swrt_gi_check.sh [--mode=smoke|reflect|toggle|budget] -- <godot> <godot arguments without a scene>
#
# --mode=smoke (default): res://tests/swrt_gi/swrt_gi_check.tscn on the kitbash at High and at
#   Epic (quality 2 and 3).
# --mode=reflect: res://tests/nextgi_reflect/run_reflect.tscn on the Roughness Corridor at High.
# --mode=toggle: swrt_gi_check.tscn --check=toggle at High, started with hardware_ray_tracing Auto:
#   HWRT, then SWRT, then HWRT again through the runtime switch. It needs a device with the HWRT
#   tier; elsewhere it fails.
# --mode=budget: swrt_gi_check.tscn --check=budget at High, started with hardware_ray_tracing Auto:
#   HWRT until debug/as_budget_bytes drops to 1 byte at runtime, then SWRT (AS_MEMORY) from the next
#   frame on. It needs a device with the HWRT tier; elsewhere it fails.
#
# Each pass writes godotnext/testbed/override.cfg (git ignores it) with nextgi/enabled=true, the
# pass's hardware_ray_tracing (0 Auto for the reference, 1 Off for the software tier) and the
# quality, runs Godot, and removes the file again; it refuses to overwrite an existing one. The
# reference pass must report the HWRT tier. On a machine without it the script says so and uses
# the reference recorded on the Apple M5 Max (it never skips the software pass). The SWRT/HWRT
# ratios are printed report-only (§22). Prints the counts of "ERROR:" lines and Metal validation
# reports of every pass. Exits non-zero when a software-tier pass fails.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/swrt_gi/run_swrt_gi_check.sh --mode=smoke -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

# The hardware tier's references recorded on the Apple M5 Max: the kitbash smoke view's world hits
# (rays - misses - screen hits) of the median snapshot at High and Epic, measured by this check
# with the asynchronous gather counters (#186), which publish every other frame; and the Roughness
# Corridor's reflection hits at High, measured with b0098c6b. World hits vary with the frame's ray
# rotation: b0098c6b's median frame gives 2548 and 10431.
RECORDED_WORLD_HITS_HIGH=1570
RECORDED_WORLD_HITS_EPIC=6725
RECORDED_REFLECTION_HITS_HIGH=30409

mode=smoke
while [[ $# -gt 0 ]]; do
	case "$1" in
	--mode=smoke | --mode=reflect | --mode=toggle | --mode=budget)
		mode="${1#--mode=}"
		shift
		;;
	--)
		shift
		break
		;;
	*)
		echo "run_swrt_gi_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
done
if [[ $# -eq 0 ]]; then
	echo "run_swrt_gi_check.sh: pass the Godot command after --" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

# run_pass <hardware_ray_tracing> <quality> <scene> <scene arguments...>
run_pass() {
	local hwrt=$1 quality=$2 scene=$3
	shift 3
	if [[ -e "$override" ]]; then
		echo "run_swrt_gi_check.sh: $override already exists; not overwriting it" >&2
		exit 2
	fi
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/quality=$quality
CFG
	set +e
	"${godot[@]}" "$scene" -- "$@" >"$log" 2>&1
	pass_status=$?
	set -e
	rm -f "$override"
	cat "$log"
	echo "GODOTNEXT_SWRT_GI_LOG {\"hwrt\":$hwrt,\"quality\":$quality,\"errors\":$(grep -c 'ERROR:' "$log" || true),\"invalid_metal_usage\":$(grep -c 'Invalid Metal usage' "$log" || true),\"shader_validation\":$(grep -c 'Error detected in' "$log" || true),\"status\":$pass_status}"
}

# reference <key> <recorded> <scene> <scene arguments...>: prints the reference value.
reference() {
	local key=$1 recorded=$2 quality=$3 scene=$4
	shift 4
	run_pass 0 "$quality" "$scene" "$@" >&2
	local value
	value=$(grep -m1 "^SWRT_GI_REFERENCE " "$log" | sed -n "s/.*$key=\([0-9]*\).*/\1/p" || true)
	if [[ "$pass_status" -ne 0 || -z "$value" || "$value" -le 0 ]]; then
		echo "run_swrt_gi_check.sh: no hardware-tier reference on this machine (the HWRT pass did not report one); using the value recorded on the Apple M5 Max, $key=$recorded" >&2
		value=$recorded
	fi
	echo "$value"
}

godot=("$@")
failed=0
if [[ "$mode" == "smoke" ]]; then
	for quality in 2 3; do
		recorded=$RECORDED_WORLD_HITS_HIGH
		[[ "$quality" == 3 ]] && recorded=$RECORDED_WORLD_HITS_EPIC
		hits=$(reference world_hits "$recorded" "$quality" res://tests/swrt_gi/swrt_gi_check.tscn --check=smoke --expected-tier=HWRT)
		run_pass 1 "$quality" res://tests/swrt_gi/swrt_gi_check.tscn --check=smoke --expected-tier=SWRT --hwrt-world-hits="$hits"
		[[ "$pass_status" -ne 0 ]] && failed=1
	done
elif [[ "$mode" == "toggle" || "$mode" == "budget" ]]; then
	run_pass 0 2 res://tests/swrt_gi/swrt_gi_check.tscn --check="$mode"
	[[ "$pass_status" -ne 0 ]] && failed=1
else
	hits=$(reference reflection_hits "$RECORDED_REFLECTION_HITS_HIGH" 2 res://tests/nextgi_reflect/run_reflect.tscn --expected-tier=HWRT)
	run_pass 1 2 res://tests/nextgi_reflect/run_reflect.tscn --expected-tier=SWRT --hwrt-reflection-hits="$hits"
	[[ "$pass_status" -ne 0 ]] && failed=1
fi
exit "$failed"
