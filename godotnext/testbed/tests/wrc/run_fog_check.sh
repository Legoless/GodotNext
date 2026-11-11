#!/usr/bin/env bash
# Volumetric fog reads the world radiance cache on Color Box Room A (#44).
# Usage: run_fog_check.sh [--hwrt=auto|off|force] [--quality=0|1|2|3] -- <godot> <godot arguments>
# GodotNext: ps_swrt_trace (#186). --hwrt=off runs the software tier, whose WRC producer traces
# the SDF (plan §21.2 L9b); pass the scene --expected-tier=SWRT then. The default keeps the
# project's quality (Auto) and hardware_ray_tracing Auto. Exits with Godot's status, or 1 when the
# run logged any "ERROR:", "Invalid Metal usage" or "Error detected in" line (the bar is 0).

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

hwrt_value=0
quality_line=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--hwrt=auto) hwrt_value=0 ;;
	--hwrt=off) hwrt_value=1 ;;
	--hwrt=force) hwrt_value=2 ;;
	--quality=0 | --quality=1 | --quality=2 | --quality=3) quality_line="global_illumination/nextgi/quality=${1#--quality=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_fog_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_fog_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_fog_check.sh: $override already exists; not overwriting it" >&2
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
$quality_line
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_WRC_LOG {\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
if [[ "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	echo "run_fog_check.sh: the run logged validation lines; the bar is 0" >&2
	exit 1
fi
