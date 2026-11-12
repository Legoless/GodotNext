#!/usr/bin/env bash
# Runs the software-tier GI checks (#186): each mode first runs a hardware-tier reference pass on
# the same view, then the software-tier pass against it.
#
# Usage:
#   run_swrt_gi_check.sh [--mode=smoke|reflect|toggle|budget|bleed|detail] [--deny-ray-query] --
#     <godot> <godot arguments without a scene>
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
# --mode=bleed (#188): swrt_gi_check.tscn --check=bleed on Color Box Room A seen from above, at High:
#   the floor's red and green bleed from the off-screen walls, against the hardware tier's. The
#   software tier records the frame the bounce settles. On this room that is about 780 either way;
#   frame 1456 is when the trace queue goes empty.
# --mode=detail (#186 step 3): swrt_gi_check.tscn --check=detail, the bleed view on the software
#   tier at High with software_ray_tracing_mode Detail (1): detail hits resolve through D6. The
#   hardware tier has no detail mode, so there is no reference pass.
# --deny-ray-query (smoke, bleed and detail): the software passes run with hardware_ray_tracing
#   Auto and debug/deny_ray_query, the software tier as on a device without ray query
#   (NO_RAY_QUERY); the check then also asserts no RT arena and SWRT instances. The reference
#   passes run without it.
#
# Each pass writes godotnext/testbed/override.cfg (git ignores it) with nextgi/enabled=true, the
# pass's hardware_ray_tracing (0 Auto for the reference, 1 Off for the software tier) and the
# quality, runs Godot, and removes the file again; it refuses to overwrite an existing one. The
# reference pass must report the HWRT tier. On a machine without it the script says so and uses
# the reference recorded on the Apple M5 Max (it never skips the software pass). The SWRT/HWRT
# ratios are printed report-only (§22). Prints the counts of "ERROR:" lines and Metal validation
# reports of every pass. Exits non-zero when a software-tier pass (or the toggle or budget pass)
# fails or logs any "ERROR:", "Invalid Metal usage" or "Error detected in" line. A hardware
# reference pass only prints its counts: on a machine without the HWRT tier it is expected to fail.
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
# (rays - misses - screen hits) at High and Epic, the mean over the sample window's distinct
# snapshots of the asynchronous gather counters (#186); and the Roughness Corridor's reflection
# hits at High, measured with b0098c6b. Single frames vary strongly with the ray rotation (1570
# to 2548 at High); the window mean moves between 1718 and 1782 when the window starts 1-3 frames
# later.
RECORDED_WORLD_HITS_HIGH=1770
RECORDED_WORLD_HITS_EPIC=7462
RECORDED_REFLECTION_HITS_HIGH=30409
# The hardware tier's floor bleed on the bleed view at High once voxel lighting has filled,
# measured by this check with b0098c6b.
RECORDED_RED_BLEED_HIGH=0.007472
RECORDED_GREEN_BLEED_HIGH=0.005414

mode=smoke
deny=false
while [[ $# -gt 0 ]]; do
	case "$1" in
	--mode=smoke | --mode=reflect | --mode=toggle | --mode=budget | --mode=bleed | --mode=detail)
		mode="${1#--mode=}"
		shift
		;;
	--deny-ray-query)
		deny=true
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

# run_pass <hardware_ray_tracing> <quality> <scene> <scene arguments...>. pass_deny=true adds
# debug/deny_ray_query and pass_detail=true software_ray_tracing_mode Detail.
pass_deny=false
pass_detail=false
pass_validation=0
run_pass() {
	local hwrt=$1 quality=$2 scene=$3
	shift 3
	if [[ -e "$override" ]]; then
		echo "run_swrt_gi_check.sh: $override already exists; not overwriting it" >&2
		exit 2
	fi
	local swrt_mode=0
	[[ "$pass_detail" == true ]] && swrt_mode=1
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/quality=$quality
global_illumination/nextgi/debug/deny_ray_query=$pass_deny
global_illumination/nextgi/software_ray_tracing_mode=$swrt_mode
CFG
	set +e
	"${godot[@]}" "$scene" -- "$@" >"$log" 2>&1
	pass_status=$?
	set -e
	rm -f "$override"
	cat "$log"
	local errors metal_usage metal_shader
	errors=$(grep -c 'ERROR:' "$log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	metal_shader=$(grep -c 'Error detected in' "$log" || true)
	pass_validation=0
	if [[ "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
		pass_validation=1
	fi
	echo "GODOTNEXT_SWRT_GI_LOG {\"hwrt\":$hwrt,\"quality\":$quality,\"deny_ray_query\":$pass_deny,\"software_ray_tracing_mode\":$swrt_mode,\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$pass_status}"
}

# pass_failed: the last pass failed its check or logged a validation line (the bar is 0).
pass_failed() {
	[[ "$pass_status" -ne 0 || "$pass_validation" -ne 0 ]]
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

# software_pass <quality> <scene> <scene arguments...>: hardware_ray_tracing Off, or Auto under
# debug/deny_ray_query with --deny-ray-query.
software_pass() {
	local hwrt=1
	if [[ "$deny" == true ]]; then
		hwrt=0
		pass_deny=true
	fi
	run_pass "$hwrt" "$@"
	pass_deny=false
}

# bleed_reference <name> <recorded>: the hardware tier's bleed value from the reference pass's log.
bleed_reference() {
	local key=$1 recorded=$2 value
	value=$(grep -m1 "^SWRT_GI_REFERENCE " "$log" | sed -n "s/.*$key=\([0-9.]*\).*/\1/p" || true)
	if [[ "$pass_status" -ne 0 || -z "$value" ]] || ! awk -v v="$value" 'BEGIN { exit !(v > 0) }'; then
		echo "run_swrt_gi_check.sh: no hardware-tier $key on this machine; using the value recorded on the Apple M5 Max, $key=$recorded" >&2
		value=$recorded
	fi
	echo "$value"
}

godot=("$@")
failed=0
if [[ "$deny" == true && "$mode" != "smoke" && "$mode" != "bleed" && "$mode" != "detail" ]]; then
	echo "run_swrt_gi_check.sh: --deny-ray-query runs with --mode=smoke, bleed or detail" >&2
	exit 2
fi
if [[ "$mode" == "smoke" ]]; then
	for quality in 2 3; do
		recorded=$RECORDED_WORLD_HITS_HIGH
		[[ "$quality" == 3 ]] && recorded=$RECORDED_WORLD_HITS_EPIC
		hits=$(reference world_hits "$recorded" "$quality" res://tests/swrt_gi/swrt_gi_check.tscn --check=smoke --expected-tier=HWRT)
		software_pass "$quality" res://tests/swrt_gi/swrt_gi_check.tscn --check=smoke --expected-tier=SWRT --hwrt-world-hits="$hits"
		pass_failed && failed=1
	done
elif [[ "$mode" == "bleed" ]]; then
	run_pass 0 2 res://tests/swrt_gi/swrt_gi_check.tscn --check=bleed --expected-tier=HWRT >&2
	red=$(bleed_reference red_bleed "$RECORDED_RED_BLEED_HIGH")
	green=$(bleed_reference green_bleed "$RECORDED_GREEN_BLEED_HIGH")
	software_pass 2 res://tests/swrt_gi/swrt_gi_check.tscn --check=bleed --expected-tier=SWRT --hwrt-red-bleed="$red" --hwrt-green-bleed="$green"
	pass_failed && failed=1
elif [[ "$mode" == "detail" ]]; then
	pass_detail=true
	software_pass 2 res://tests/swrt_gi/swrt_gi_check.tscn --check=detail --expected-tier=SWRT
	pass_detail=false
	pass_failed && failed=1
elif [[ "$mode" == "toggle" || "$mode" == "budget" ]]; then
	run_pass 0 2 res://tests/swrt_gi/swrt_gi_check.tscn --check="$mode"
	pass_failed && failed=1
else
	hits=$(reference reflection_hits "$RECORDED_REFLECTION_HITS_HIGH" 2 res://tests/nextgi_reflect/run_reflect.tscn --expected-tier=HWRT)
	run_pass 1 2 res://tests/nextgi_reflect/run_reflect.tscn --expected-tier=SWRT --hwrt-reflection-hits="$hits"
	pass_failed && failed=1
fi
exit "$failed"
