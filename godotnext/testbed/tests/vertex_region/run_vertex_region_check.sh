#!/usr/bin/env bash
# Runs res://tests/vertex_region/vertex_region_check.tscn with NextGI enabled on one tier (#252).
#
# Usage:
#   run_vertex_region_check.sh [--hardware-ray-tracing=0|1] [--deny-ray-query] -- <godot> <godot arguments>
#
# 0 (Auto, the default) gives HWRT on a ray-query device; 1 (Off) gives the software tier.
# --deny-ray-query sets debug/deny_ray_query, which makes Auto fall back to the software tier.
# Writes godotnext/testbed/override.cfg (git ignores it) for the run and removes it again; it
# refuses to overwrite an existing one. Fails when Godot fails, the scene fails, or the log has
# an "ERROR:", "Invalid Metal usage" or "Error detected in" line.
#
# Example on the Apple M5 Max:
#   MTL_DEBUG_LAYER=1 godotnext/testbed/tests/vertex_region/run_vertex_region_check.sh --hardware-ray-tracing=1 -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal \
#     res://tests/vertex_region/vertex_region_check.tscn

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
hwrt=0
deny=false

while [[ $# -gt 0 ]]; do
	case "$1" in
	--hardware-ray-tracing=*)
		hwrt=${1#--hardware-ray-tracing=}
		;;
	--deny-ray-query)
		deny=true
		;;
	--)
		shift
		break
		;;
	*)
		echo "run_vertex_region_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vertex_region_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_vertex_region_check.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/debug/deny_ray_query=$deny
CFG

set +e
"$@" >"$log" 2>&1
status=$?
set -e
grep -E '^NextGI:|GODOTNEXT_RESULT' "$log" || true
reports=$(grep -cE '^ERROR:|Invalid Metal usage|Error detected in' "$log" || true)
echo "run_vertex_region_check.sh: exit $status, $reports validation or ERROR lines"
if [[ "$status" -ne 0 || "$reports" -ne 0 ]]; then
	grep -E '^ERROR:|Invalid Metal usage|Error detected in' "$log" | head -20 || true
	exit 1
fi
