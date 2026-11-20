#!/usr/bin/env bash
# Runs res://tests/nextgeo_texture_swap/nextgeo_texture_swap_check.tscn (#305), exercising
# RenderingServer.texture_replace_compatible() and texture_replace() with #155 programmable raster
# bins, #158 compute shading bins, #130 NextGI material hit groups and #145 shadow alpha tables active.
#
# Usage:
#   run_nextgeo_texture_swap_check.sh -- <godot> <godot arguments>

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_nextgeo_texture_swap_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
if [[ -e "$override" ]]; then
	echo "run_nextgeo_texture_swap_check.sh: $override already exists; not overwriting it" >&2
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
config/custom_user_dir_name="godotnext-texture-swap-check"

[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=0
global_illumination/nextgi/quality=2
CFG

set +e
"$@" res://tests/nextgeo_texture_swap/nextgeo_texture_swap_check.tscn 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -ci 'validation error\|VUID-' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
result=$(grep -c '^GODOTNEXT_RESULT .*"pass":true' "$log" || true)
echo "GODOTNEXT_TEXTURE_SWAP_LOG {\"errors\":$errors,\"validation\":$validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"passed\":$result,\"status\":$status}"
if [[ "$status" -ne 0 || "$errors" -ne 0 || "$validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 || "$result" -ne 1 ]]; then
	exit 1
fi
