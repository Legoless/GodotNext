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
# uses the ratio recorded on the Apple M5 Max (it never skips the software pass). Prints the
# counts of "ERROR:" lines and Metal validation reports of every pass. Exits non-zero when the
# software pass fails.

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
	echo "GODOTNEXT_LITE_LEAK_LOG {\"hwrt\":$hwrt,\"errors\":$(grep -c 'ERROR:' "$log" || true),\"invalid_metal_usage\":$(grep -c 'Invalid Metal usage' "$log" || true),\"shader_validation\":$(grep -c 'Error detected in' "$log" || true),\"status\":$pass_status}"
}

pass_args=(--expected-tier=HWRT)
run_pass 0 "$@" >&2
ratio=$(grep -m1 "^LITE_LEAK_REFERENCE " "$log" | sed -n 's/.*ratio=\([0-9.]*\).*/\1/p' || true)
if [[ "$pass_status" -ne 0 || -z "$ratio" ]]; then
	echo "run_lite_leak_check.sh: no hardware-tier reference on this machine; using the ratio recorded on the Apple M5 Max, $RECORDED_RATIO" >&2
	ratio=$RECORDED_RATIO
fi
pass_args=(--expected-tier=SWRT --hwrt-ratio="$ratio")
run_pass 1 "$@"
exit "$pass_status"
