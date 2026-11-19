#!/usr/bin/env bash
# Runs res://tests/nextgeo_shading_bins/nextgeo_shading_bins_check.tscn, the material shading bins check
# (#158), and fails on a failed check or on any error or validation line.
#
# Usage:
#   run_nextgeo_shading_bins_check.sh -- <godot> <godot arguments>
# Pass --gpu-validation (Vulkan validation layers, D3D12 debug layer) or the Metal validation
# environment in the Godot command, as the PR basics ask (§22).

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_nextgeo_shading_bins_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift

log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
"$@" res://tests/nextgeo_shading_bins/nextgeo_shading_bins_check.tscn 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -ci 'validation error\|VUID-' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
result=$(grep -c '^GODOTNEXT_RESULT .*"pass":true' "$log" || true)
echo "GODOTNEXT_NEXTGEO_SHADING_BINS_LOG {\"errors\":$errors,\"validation\":$validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"passed\":$result,\"status\":$status}"
if [[ "$status" -ne 0 || "$errors" -ne 0 || "$validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 || "$result" -ne 1 ]]; then
	exit 1
fi
