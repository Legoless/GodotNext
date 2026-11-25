#!/usr/bin/env bash
# Runs res://tests/nextgeo_rt_native/nextgeo_rt_native_check.tscn with NextGI on (#156).
# Native Nanite ray tracing is EXPERIMENTAL; the check switches rendering/nextgeo/ray_tracing_mode
# itself and starts in the default Fallback mode.
#
# Usage:
#   run_nextgeo_rt_native_check.sh -- <godot> <godot arguments>
# Pass the testbed path and, for a validation run, --gpu-validation, for example:
#   run_nextgeo_rt_native_check.sh -- ./bin/godot.linuxbsd.editor.x86_64 --path godotnext/testbed \
#     --rendering-driver vulkan --gpu-validation res://tests/nextgeo_rt_native/nextgeo_rt_native_check.tscn

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

if [[ "${1:-}" != "--" ]]; then
	echo "run_nextgeo_rt_native_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ $# -eq 0 ]]; then
	echo "run_nextgeo_rt_native_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_nextgeo_rt_native_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=0
global_illumination/nextgi/hardware_ray_tracing.macos=0
CFG

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -c -i 'Validation Error\|SYNC-HAZARD' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
result=$(grep -c '"pass":true' "$log" || true)
echo "GODOTNEXT_NEXTGEO_RT_NATIVE_LOG {\"errors\":$errors,\"validation\":$validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status,\"pass\":$result}"
if [[ "$status" -ne 0 || "$errors" -ne 0 || "$validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 || "$result" -ne 1 ]]; then
	exit 1
fi
