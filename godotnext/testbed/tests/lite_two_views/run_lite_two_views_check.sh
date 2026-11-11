#!/usr/bin/env bash
# Runs res://tests/lite_two_views/lite_two_views_check.tscn (#183, #186): Lumen Lite (Medium, the
# irradiance-field gather) with two views rendering at once, on one tier.
#
# Usage:
#   run_lite_two_views_check.sh [--hwrt=auto|off] [--deny-ray-query] [--case=kitbash|rooms|all] \
#     -- <godot> <godot arguments without a scene>
#
# --hwrt=off (the default) runs the software tier, --hwrt=auto the device's pick: the hardware tier
# on a device that reports SUPPORTS_RAY_QUERY_HARDWARE, which the scene asserts. --deny-ray-query
# sets debug/deny_ray_query with Auto (a device without ray query: the software tier, NO_RAY_QUERY).
# The kitbash case is one run. The rooms case is four fresh runs: the lit room's view alone, the
# dark room's view alone, then both views with the lit one first and with the dark one first,
# which compare each view with its solo value. Solo values come from separate processes, because
# a view that stops rendering moves the WRC's grid and the WRC keeps the old cells' history.
#
# Each run writes godotnext/testbed/override.cfg (git ignores it) with nextgi/enabled=true,
# quality 1, medium_gather 0, the hardware_ray_tracing mode and debug/deny_ray_query; it refuses
# to overwrite an existing one and removes it afterwards. Prints one GODOTNEXT_LITE_TWO_VIEWS_LOG
# line per run with its "ERROR:", "Invalid Metal usage" and "Error detected in" counts. Exits
# non-zero when any run fails or logs any of those lines (the bar is 0).
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/lite_two_views/run_lite_two_views_check.sh --hwrt=off -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
scene=res://tests/lite_two_views/lite_two_views_check.tscn

hwrt=off
deny=false
case_name=all
while [[ $# -gt 0 ]]; do
	case "$1" in
	--hwrt=auto | --hwrt=off) hwrt="${1#--hwrt=}" ;;
	--deny-ray-query) deny=true ;;
	--case=kitbash | --case=rooms | --case=all) case_name="${1#--case=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_lite_two_views_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_lite_two_views_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ "$deny" == true && "$hwrt" != auto ]]; then
	echo "run_lite_two_views_check.sh: --deny-ray-query needs --hwrt=auto" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_lite_two_views_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

hwrt_value=1
expected=SWRT
if [[ "$hwrt" == auto ]]; then
	hwrt_value=0
	if [[ "$deny" == false ]]; then
		expected=device
	fi
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

failed=0
# run_scene <label> <scene arguments...>: one fresh process; sets run_status.
run_scene() {
	local label=$1
	shift
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
global_illumination/nextgi/quality=1
global_illumination/nextgi/medium_gather=0
global_illumination/nextgi/debug/deny_ray_query=$deny
CFG
	set +e
	"${godot[@]}" "$scene" -- --expected-tier="$expected" "$@" >"$log" 2>&1
	run_status=$?
	set -e
	rm -f "$override"
	cat "$log"
	local errors metal_usage metal_shader
	errors=$(grep -c 'ERROR:' "$log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	metal_shader=$(grep -c 'Error detected in' "$log" || true)
	echo "GODOTNEXT_LITE_TWO_VIEWS_LOG {\"run\":\"$label\",\"hwrt\":\"$hwrt\",\"deny_ray_query\":$deny,\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$run_status}"
	if [[ "$run_status" -ne 0 || "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
		failed=1
	fi
}

godot=("$@")

if [[ "$case_name" == kitbash || "$case_name" == all ]]; then
	run_scene kitbash --case=kitbash
fi

if [[ "$case_name" == rooms || "$case_name" == all ]]; then
	run_scene rooms_lit --case=rooms --views=lit
	lit_solo=$(sed -n 's/^LITE_TWO_VIEWS_SOLO view=lit luma=\([0-9.]*\).*/\1/p' "$log" | head -n 1)
	run_scene rooms_dark --case=rooms --views=dark
	dark_solo=$(sed -n 's/^LITE_TWO_VIEWS_SOLO view=dark luma=\([0-9.]*\).*/\1/p' "$log" | head -n 1)
	if [[ -z "$lit_solo" || -z "$dark_solo" ]]; then
		echo "run_lite_two_views_check.sh: a solo run printed no value; the pairs cannot run" >&2
		exit 1
	fi
	run_scene rooms_lit_first --case=rooms --views=lit,dark --lit-solo="$lit_solo" --dark-solo="$dark_solo"
	run_scene rooms_dark_first --case=rooms --views=dark,lit --lit-solo="$lit_solo" --dark-solo="$dark_solo"
fi

exit "$failed"
