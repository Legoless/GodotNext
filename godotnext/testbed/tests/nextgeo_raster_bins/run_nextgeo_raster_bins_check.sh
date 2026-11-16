#!/usr/bin/env bash
# Runs res://tests/nextgeo_raster_bins/nextgeo_raster_bins_check.tscn, the programmable raster bins
# check (#155), and fails on a failed check, on any error or validation line, or when a rejected
# material shader did not log its UNSUPPORTED_RASTER_FEATURE line (the check has five).
#
# Usage:
#   run_nextgeo_raster_bins_check.sh -- <godot> <godot arguments>
# Pass --gpu-validation (Vulkan validation layers, D3D12 debug layer) or the Metal validation
# environment in the Godot command, as the PR basics ask (§22).

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_nextgeo_raster_bins_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift

log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
result=$(grep -c '^GODOTNEXT_RESULT .*"pass":true' "$log" || true)
rejected=$(grep -c 'stays on legacy raster (UNSUPPORTED_RASTER_FEATURE: ' "$log" || true)
echo "GODOTNEXT_NEXTGEO_RASTER_BINS_LOG {\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"rejected_logged\":$rejected,\"passed\":$result,\"status\":$status}"
if [[ "$status" -ne 0 || "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 || "$result" -ne 1 || "$rejected" -lt 5 ]]; then
	exit 1
fi
