#!/usr/bin/env bash
# Runs res://tests/src_pages/src_pages_check.tscn (#247): SRC page levels and solid-angle priority
# under a moving camera, on the kitbash at Lumen Lite, on one tier. Two fresh processes: the camera
# still at the flight's end (--flight=still), then the flight (--flight=move), which compares its
# ambient after the flight with the still run's.
#
# Usage:
#   run_src_pages_check.sh [--hwrt=auto|off] -- <godot> <godot arguments without a scene>
#
# --hwrt=off (the default) runs the software tier, --hwrt=auto the device's pick (the hardware tier
# on a device that reports SUPPORTS_RAY_QUERY_HARDWARE, which the scene asserts). Before #266 the
# software tier failed the comparison: the still run's WRC kept the sky radiance of its first
# trace, which ran while the mesh SDFs still streamed in, and read twice as bright as the flight
# and the hardware tier. Each run writes godotnext/testbed/override.cfg (git ignores it) with
# nextgi/enabled=true, quality 1 and medium_gather 0; it refuses to overwrite an existing one and
# removes it afterwards. Prints one GODOTNEXT_SRC_PAGES_LOG line per run with its "ERROR:",
# "Invalid Metal usage" and "Error detected in" counts. Exits non-zero when a run fails or logs
# any of those lines.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/src_pages/run_src_pages_check.sh --hwrt=auto -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
scene=res://tests/src_pages/src_pages_check.tscn

hwrt=off
while [[ $# -gt 0 ]]; do
	case "$1" in
	--hwrt=auto | --hwrt=off) hwrt="${1#--hwrt=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_src_pages_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_src_pages_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_src_pages_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi
hwrt_value=1
expected=SWRT
if [[ "$hwrt" == auto ]]; then
	hwrt_value=0
	expected=device
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

failed=0
# run_scene <label> <scene arguments...>: one fresh process.
run_scene() {
	local label=$1
	shift
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
global_illumination/nextgi/hardware_ray_tracing.macos=$hwrt_value
global_illumination/nextgi/quality=1
global_illumination/nextgi/medium_gather=0
CFG
	set +e
	"$@" "$scene" -- --expected-tier="$expected" "${scene_args[@]}" >"$log" 2>&1
	local status=$?
	set -e
	rm -f "$override"
	cat "$log"
	local errors metal_usage metal_shader
	errors=$(grep -c 'ERROR:' "$log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	metal_shader=$(grep -c 'Error detected in' "$log" || true)
	echo "GODOTNEXT_SRC_PAGES_LOG {\"run\":\"$label\",\"hwrt\":\"$hwrt\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
	if [[ "$status" -ne 0 || "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
		failed=1
	fi
}

scene_args=(--flight=still)
run_scene still "$@"
still_luma=$(sed -n 's/^SRC_PAGES_AMBIENT flight=still lit=[0-9.]* luma=\([0-9.]*\).*/\1/p' "$log" | head -n 1)
if [[ -z "$still_luma" ]]; then
	echo "run_src_pages_check.sh: the still run printed no ambient; the flight cannot be compared" >&2
	exit 1
fi
scene_args=(--flight=move --still-luma="$still_luma")
run_scene move "$@"

exit "$failed"
