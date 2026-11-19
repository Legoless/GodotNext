#!/usr/bin/env bash
# Runs res://tests/path_tracer/path_tracer_check.tscn with NextGI on (#167).
#
# Usage:
#   run_path_tracer_check.sh -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with NextGI on and the hardware tier on Auto,
# runs Godot and removes the file again. It refuses to overwrite an existing override.cfg. The scene
# path traces Color Box Room A and prints one "GODOTNEXT_RESULT" line. Exits non-zero when Godot
# fails, the result is a failure or the run logged any "ERROR:", "Validation Error", "Invalid Metal
# usage" or "Error detected in" line (the bar is 0).
#
# Example on the RTX 4080 SUPER machine:
#   godotnext/testbed/tests/path_tracer/run_path_tracer_check.sh -- godot --path godotnext/testbed \
#     --rendering-driver vulkan --gpu-validation res://tests/path_tracer/path_tracer_check.tscn
# Where the hit groups do not run (no RT pipelines), add "-- --expect-backend=inline" after the scene.
# PATH_TRACER_HWRT_MODE=1 (Simplified Material) or 2 (Inline) also writes
# rendering/global_illumination/nextgi/hardware_ray_tracing_mode and its Windows and Linux entries (#286); the
# path tracer then traces with its INLINE backend, so add "-- --expect-backend=inline" after the scene.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

if [[ "${1:-}" != "--" ]]; then
	echo "run_path_tracer_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ -e "$override" ]]; then
	echo "run_path_tracer_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<CFG
[application]

config/use_custom_user_dir=true
config/custom_user_dir_name="godotnext-path-tracer-check"

[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=0
CFG
mode="${PATH_TRACER_HWRT_MODE:-}"
if [[ -n "$mode" ]]; then
	cat >>"$override" <<CFG
global_illumination/nextgi/hardware_ray_tracing_mode=$mode
global_illumination/nextgi/hardware_ray_tracing_mode.windows=$mode
global_illumination/nextgi/hardware_ray_tracing_mode.linuxbsd=$mode
CFG
fi

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -ci 'Validation Error' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_PATH_TRACER_LOG {\"errors\":$errors,\"validation_errors\":$validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_path_tracer_check.sh: the path tracer check did not pass" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
if [[ "$errors" -ne 0 || "$validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	echo "run_path_tracer_check.sh: the run logged errors or validation lines; the bar is 0" >&2
	exit 1
fi
