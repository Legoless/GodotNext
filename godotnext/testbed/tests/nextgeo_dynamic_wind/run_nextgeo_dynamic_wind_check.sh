#!/usr/bin/env bash
# Runs res://tests/nextgeo_dynamic_wind/nextgeo_dynamic_wind_check.tscn with VSM enabled (#296).
#
# Usage:
#   run_nextgeo_dynamic_wind_check.sh -- <godot> <godot arguments>

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
export GODOT_DEV_VSM="${GODOT_DEV_VSM:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

if [[ "${1:-}" != "--" ]]; then
	echo "run_nextgeo_dynamic_wind_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ $# -eq 0 ]]; then
	echo "run_nextgeo_dynamic_wind_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_nextgeo_dynamic_wind_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<CFG
[rendering]

lights_and_shadows/shadow_method=1
CFG

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -c -i 'Validation Error\|SYNC-HAZARD' "$log" || true)
result=$(grep -c '"pass":true' "$log" || true)
echo "GODOTNEXT_NEXTGEO_DYNAMIC_WIND_LOG {\"errors\":$errors,\"validation\":$validation,\"status\":$status,\"pass\":$result}"
if [[ "$status" -ne 0 || "$errors" -ne 0 || "$validation" -ne 0 || "$result" -ne 1 ]]; then
	exit 1
fi
