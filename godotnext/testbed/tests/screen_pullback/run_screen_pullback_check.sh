#!/usr/bin/env bash
# #186 step 4. A thin wall that a screen-space reflection ray passes behind must
# occlude the red emissive beyond the screen exit, on the hardware tier, the
# software tier, and under debug/deny_ray_query.
#
# Usage:
#   run_screen_pullback_check.sh -- <godot> <godot arguments without a scene>
#
# Writes godotnext/testbed/override.cfg (git ignores it) per pass and removes it.
# Refuses to overwrite an existing one. Exits non-zero when a pass fails or logs
# any "ERROR:", "Invalid Metal usage" or "Error detected in" line.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
export MTL_DEBUG_LAYER="${MTL_DEBUG_LAYER:-1}"
# Metal shader validation is opt-in: export MTL_SHADER_VALIDATION=1 to run with it.
if [[ -n "${MTL_SHADER_VALIDATION:-}" ]]; then
	export MTL_SHADER_VALIDATION_REPORT_TO_STDERR="${MTL_SHADER_VALIDATION_REPORT_TO_STDERR:-1}"
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--)
		shift
		break
		;;
	*)
		echo "run_screen_pullback_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
done
if [[ $# -eq 0 ]]; then
	echo "run_screen_pullback_check.sh: pass the Godot command after --" >&2
	exit 2
fi

log=$(mktemp)
godot_cmd=("$@")
# The override is removed only while this run owns it: one that was there before stays.
own_override=false
cleanup() {
	if [[ "$own_override" == true ]]; then
		rm -f "$override"
	fi
	rm -f "$log"
}
trap cleanup EXIT

# run_pass <label> <hardware_ray_tracing> <deny_ray_query> <expected tier> <readback>
run_pass() {
	local label=$1
	local hwrt=$2
	local deny=$3
	local expected=$4
	local readback=$5
	if [[ -e "$override" ]]; then
		echo "run_screen_pullback_check.sh: $override already exists; not overwriting it" >&2
		exit 2
	fi
	own_override=true
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/quality=2
global_illumination/nextgi/debug/deny_ray_query=$deny
global_illumination/nextgi/debug/swrt_trace_readback=$readback
CFG
	set +e
	"${godot_cmd[@]}" res://tests/screen_pullback/screen_pullback_check.tscn -- --expected-tier="$expected" >"$log" 2>&1
	local status=$?
	set -e
	rm -f "$override"
	own_override=false
	cat "$log"
	local errors metal_usage metal_shader
	errors=$(grep -c 'ERROR:' "$log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	metal_shader=$(grep -c 'Error detected in' "$log" || true)
	echo "GODOTNEXT_SCREEN_PULLBACK_LOG {\"pass\":\"$label\",\"hwrt\":$hwrt,\"deny_ray_query\":$deny,\"expected_tier\":\"$expected\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
	if [[ "$status" -ne 0 ]]; then
		exit "$status"
	fi
	if [[ "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
		echo "run_screen_pullback_check.sh: $label logged validation lines; the bar is 0" >&2
		exit 1
	fi
}

run_pass hwrt 0 false HWRT false
run_pass swrt 1 false SWRT true
run_pass deny 0 true SWRT true
