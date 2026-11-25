#!/usr/bin/env bash
# Runs res://tests/nextgi_reflect_trace/reflect_trace_check.tscn with NextGI enabled (#274).
#
# Usage:
#   run_reflect_trace_check.sh [--quality=medium|high|epic] [--hwrt=auto|off|force] -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with the NextGI settings (the quality,
# Epic by default; Medium with the traced screen-probe gather, since the default Lumen Lite Medium
# traces no reflections; the hardware_ray_tracing mode, auto by default), runs Godot and removes
# the file again. It refuses to overwrite an existing override.cfg. The scene prints one
# "GODOTNEXT_RESULT" line. Exits non-zero when Godot fails, the result is a failure or the run
# logged any "ERROR:", Vulkan "Validation Error", "Invalid Metal usage" or "Error detected in"
# line (the bar is 0).
#
# The issue's checks, each with --resolution 640x360 after the Godot binary:
#   Epic, Color Box Room A, every surface traceable (each quarter must hold hits):
#     ... -- res://tests/nextgi_reflect_trace/reflect_trace_check.tscn -- \
#       --scene=res://scenes/color_box_room/cbr_a.tscn --roughness=0.2 --expect-quality=EPIC --require-quarters=true
#   Epic, Roughness Corridor, camera tilted onto the strips (each quarter must hold hits):
#     ... -- --expect-quality=EPIC --pitch=35 --require-quarters=true
#   Epic or High, Roughness Corridor, CaptureCam (the strips reflect the sky):
#     ... -- --expect-quality=HIGH --require-sky-misses=true
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/nextgi_reflect_trace/run_reflect_trace_check.sh -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal \
#     --resolution 640x360 res://tests/nextgi_reflect_trace/reflect_trace_check.tscn -- \
#     --expect-quality=EPIC --pitch=35 --require-quarters=true

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

quality_value=3
medium_gather=0
hwrt_value=0
while [[ $# -gt 0 ]]; do
	case "$1" in
	--quality=medium)
		quality_value=1
		medium_gather=1
		shift
		;;
	--quality=high)
		quality_value=2
		shift
		;;
	--quality=epic)
		quality_value=3
		shift
		;;
	--hwrt=auto)
		hwrt_value=0
		shift
		;;
	--hwrt=off)
		hwrt_value=1
		shift
		;;
	--hwrt=force)
		hwrt_value=2
		shift
		;;
	--)
		shift
		break
		;;
	*)
		echo "run_reflect_trace_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
done
if [[ $# -eq 0 ]]; then
	echo "run_reflect_trace_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_reflect_trace_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<EOF
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
global_illumination/nextgi/hardware_ray_tracing.macos=$hwrt_value
global_illumination/nextgi/quality=$quality_value
global_illumination/nextgi/medium_gather=$medium_gather
EOF

# pipefail makes the pipeline's status Godot's when Godot fails.
set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
errors=$(grep -c 'ERROR:' "$log" || true)
vulkan_validation=$(grep -c -i 'Validation Error' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_REFLECT_TRACE $result"
echo "GODOTNEXT_REFLECT_TRACE_LOG {\"errors\":$errors,\"vulkan_validation\":$vulkan_validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_reflect_trace_check.sh: the reflection trace check did not pass" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
if [[ "$errors" -ne 0 || "$vulkan_validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	echo "run_reflect_trace_check.sh: the run logged errors or validation lines; the bar is 0" >&2
	exit 1
fi
