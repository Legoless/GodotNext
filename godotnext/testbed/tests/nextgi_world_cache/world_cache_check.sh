#!/usr/bin/env bash
# Runs res://tests/nextgi_world_cache/world_cache_check.tscn (#294).
#
# Usage:
#   world_cache_check.sh -- <godot> <godot arguments>
#
# Environment variables:
#   WORLD_CACHE_ENABLED=0|1     rendering/global_illumination/nextgi/world_cache (default 0)
#   WORLD_CACHE_HWRT=0|1|2      rendering/global_illumination/nextgi/hardware_ray_tracing (default 0 Auto)
#   WORLD_CACHE_HWRT_MODE=0|1|2 rendering/global_illumination/nextgi/hardware_ray_tracing_mode (default 0 Full)
#
# Writes a temporary godotnext/testbed/override.cfg (git-ignored), runs Godot, checks that
# GODOTNEXT_RESULT reports "pass":true with 0 ERROR: and 0 Validation Error lines, and removes
# override.cfg on exit.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

if [[ "${1:-}" != "--" ]]; then
	echo "world_cache_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ -e "$override" ]]; then
	echo "world_cache_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

wc_enabled="${WORLD_CACHE_ENABLED:-0}"
wc_bool="false"
if [[ "$wc_enabled" == "1" || "$wc_enabled" == "true" ]]; then
	wc_bool="true"
fi
hwrt="${WORLD_CACHE_HWRT:-0}"
hwrt_mode="${WORLD_CACHE_HWRT_MODE:-0}"

cat >"$override" <<CFG
[application]

config/use_custom_user_dir=true
config/custom_user_dir_name="godotnext-world-cache-check"

[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/hardware_ray_tracing.macos=$hwrt
global_illumination/nextgi/quality=2
global_illumination/nextgi/world_cache=$wc_bool
global_illumination/nextgi/hardware_ray_tracing_mode=$hwrt_mode
global_illumination/nextgi/hardware_ray_tracing_mode.windows=$hwrt_mode
global_illumination/nextgi/hardware_ray_tracing_mode.linuxbsd=$hwrt_mode
global_illumination/nextgi/hardware_ray_tracing_mode.macos=$hwrt_mode
CFG

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -ci 'Validation Error' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_WORLD_CACHE_LOG {\"errors\":$errors,\"validation_errors\":$validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ "$result" != *'"pass":true'* ]]; then
	echo "world_cache_check.sh: the World Cache check did not pass" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
if [[ "$errors" -ne 0 || "$validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	echo "world_cache_check.sh: the run logged errors or validation lines; the bar is 0" >&2
	exit 1
fi
