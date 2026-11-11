#!/usr/bin/env bash
# Two NextGI views of different sizes in one frame (#186), under Metal API and shader validation.
#
# Usage (from any directory):
#   run_two_sizes_check.sh [--passes="swrt:2 swrt:3 hwrt:3"] [-- <godot binary>]
#
# Each pass is <tier>:<quality>: tier swrt sets hardware_ray_tracing to Off, hwrt to Auto (the
# pass then fails unless the device has the hardware tier); quality 2 is High, 3 is Epic. A pass
# writes godotnext/testbed/override.cfg (git ignores it) with NextGI enabled and the pass's
# settings, runs res://tests/multiview/two_sizes_check.tscn and removes the file again; it refuses
# to overwrite an existing one. The Godot binary defaults to bin/godot.macos.editor.arm64 in the
# repository.
#
# A pass fails unless the scene passes (the gather runs for both views every frame, see
# two_sizes_check.gd), the gather ran on the pass's tier and the run logs 0 "ERROR:", "Invalid Metal
# usage" and "Error detected in" lines. GPU work of one frame overlapping the next frame's makes the
# small view read past the end of its own buffers, which only shader validation reports. Exits 0
# when every pass passes.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/multiview/run_two_sizes_check.sh -- ./bin/godot.macos.editor.arm64

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
export MTL_DEBUG_LAYER=1
export MTL_SHADER_VALIDATION=1
export MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
repo_root=$(cd "$testbed_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

passes="swrt:2 swrt:3 hwrt:3"
while [[ $# -gt 0 ]]; do
	case "$1" in
	--passes=*) passes="${1#--passes=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_two_sizes_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
godot="${1:-$repo_root/bin/godot.macos.editor.arm64}"
if [[ ! -x "$godot" ]]; then
	echo "run_two_sizes_check.sh: Godot binary is not executable: $godot" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_two_sizes_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
override_written=0
cleanup() {
	rm -f "$log"
	if [[ "$override_written" == 1 ]]; then
		rm -f "$override"
	fi
}
trap cleanup EXIT

failed=0
for pass in $passes; do
	tier="${pass%%:*}"
	quality="${pass##*:}"
	case "$tier" in
	swrt)
		hwrt=1
		expected_tier=SWRT
		;;
	hwrt)
		hwrt=0
		expected_tier=HWRT
		;;
	*)
		echo "run_two_sizes_check.sh: unknown tier in pass '$pass'" >&2
		exit 2
		;;
	esac
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/quality=$quality
CFG
	override_written=1
	set +e
	"$godot" --path "$testbed_dir" --rendering-driver metal res://tests/multiview/two_sizes_check.tscn >"$log" 2>&1
	status=$?
	set -e
	rm -f "$override"
	override_written=0

	result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
	errors=$(grep -c 'ERROR:' "$log" || true)
	invalid=$(grep -c 'Invalid Metal usage' "$log" || true)
	detected=$(grep -c 'Error detected in' "$log" || true)
	echo "GODOTNEXT_TWO_SIZES pass=$pass exit=$status errors=$errors invalid_metal_usage=$invalid error_detected=$detected $result"
	if [[ "$invalid" != 0 ]]; then
		grep -A3 'Invalid Metal usage' "$log" | grep 'pipeline:' | sed 's/pipeline UID.*//' | sort | uniq -c || true
	fi
	if [[ "$errors" != 0 ]]; then
		grep 'ERROR:' "$log" | sort | uniq -c | head -n 10 || true
	fi
	if [[ "$result" != *"\"gather_tier\":\"$expected_tier\""* ]]; then
		echo "run_two_sizes_check.sh: pass $pass did not run the gather on the $expected_tier tier" >&2
		status=1
	fi
	if [[ "$status" != 0 || "$errors" != 0 || "$invalid" != 0 || "$detected" != 0 || "$result" != *'"pass":true'* ]]; then
		echo "run_two_sizes_check.sh: pass $pass failed" >&2
		failed=1
	fi
done
exit "$failed"
