#!/usr/bin/env bash
# Runs res://tests/lumen_lite/lite_leak_check.tscn (#183, #188): Lumen Lite (Medium, the
# irradiance-field gather) on the hardware tier first, for the reference dark/lit ratio, then on
# the software tier against it.
#
# Usage:
#   run_lite_leak_check.sh -- <godot> <godot arguments without a scene>
#
# Each pass writes godotnext/testbed/override.cfg (git ignores it) with nextgi/enabled=true,
# quality 1 and medium_gather 0 (Lumen Lite), and hardware_ray_tracing 0 (Auto, the reference) or
# 1 (Off, the software tier); it refuses to overwrite an existing one and removes it afterwards.
# The reference pass must report the HWRT tier; on a machine without it the script says so and
# uses the ratio recorded on the Apple M5 Max (it never skips the software pass). A reference pass
# that ran on the HWRT tier and still failed, or logged a validation line, fails the script after
# the software pass. Prints the counts of "ERROR:" lines and Metal validation reports of every
# pass. Exits non-zero when the software pass fails or logs any "ERROR:", "Invalid Metal usage"
# or "Error detected in" line (the bar is 0), or when the reference pass failed on the HWRT tier.

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

# The hardware tier's dark/lit ratio on this scene, measured by this check with b0098c6b.
RECORDED_RATIO=0.000178

while [[ $# -gt 0 ]]; do
	case "$1" in
	--)
		shift
		break
		;;
	*)
		echo "run_lite_leak_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
done
if [[ $# -eq 0 ]]; then
	echo "run_lite_leak_check.sh: pass the Godot command after --" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

# run_pass <hardware_ray_tracing> <scene arguments...>
run_pass() {
	local hwrt=$1
	shift
	if [[ -e "$override" ]]; then
		echo "run_lite_leak_check.sh: $override already exists; not overwriting it" >&2
		exit 2
	fi
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/quality=1
global_illumination/nextgi/medium_gather=0
CFG
	set +e
	"$@" res://tests/lumen_lite/lite_leak_check.tscn -- "${pass_args[@]}" >"$log" 2>&1
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
	echo "GODOTNEXT_LITE_LEAK_LOG {\"hwrt\":$hwrt,\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$pass_status}"
}

pass_args=(--expected-tier=HWRT)
run_pass 0 "$@" >&2
ratio=$(grep -m1 "^LITE_LEAK_REFERENCE " "$log" | sed -n 's/.*ratio=\([0-9.]*\).*/\1/p' || true)
reference_tier=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" | sed -n 's/.*"tier":"\([A-Z_]*\)".*/\1/p' || true)
reference_failed=0
if [[ "$reference_tier" == HWRT && ("$pass_status" -ne 0 || -z "$ratio" || "$pass_validation" -ne 0) ]]; then
	echo "run_lite_leak_check.sh: the reference pass ran on the HWRT tier and failed or logged validation lines (status $pass_status); the software pass uses the ratio recorded on the Apple M5 Max, $RECORDED_RATIO" >&2
	reference_failed=1
	ratio=$RECORDED_RATIO
elif [[ "$pass_status" -ne 0 || -z "$ratio" ]]; then
	echo "run_lite_leak_check.sh: no hardware-tier reference on this machine (the reference pass ran on '${reference_tier:-no tier}'); using the ratio recorded on the Apple M5 Max, $RECORDED_RATIO" >&2
	ratio=$RECORDED_RATIO
fi
pass_args=(--expected-tier=SWRT --hwrt-ratio="$ratio")
run_pass 1 "$@"
if [[ "$pass_status" -ne 0 ]]; then
	exit "$pass_status"
fi
if [[ "$pass_validation" -ne 0 ]]; then
	echo "run_lite_leak_check.sh: the software pass logged validation lines; the bar is 0" >&2
	exit 1
fi
if [[ "$reference_failed" -ne 0 ]]; then
	echo "run_lite_leak_check.sh: the hardware-tier reference pass failed" >&2
	exit 1
fi
