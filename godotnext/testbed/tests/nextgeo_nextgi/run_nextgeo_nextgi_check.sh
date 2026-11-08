#!/usr/bin/env bash
# Runs res://tests/nextgeo_nextgi/nextgeo_nextgi_check.tscn with NextGI enabled (#77).
#
# Usage:
#   run_nextgeo_nextgi_check.sh [--hwrt=auto|off|force] -- <godot> <godot arguments>

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

hwrt=auto
while [[ $# -gt 0 ]]; do
	case "$1" in
	--hwrt=auto | --hwrt=off | --hwrt=force) hwrt="${1#--hwrt=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_nextgeo_nextgi_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_nextgeo_nextgi_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_nextgeo_nextgi_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

case "$hwrt" in
auto) hwrt_value=0 ;;
off) hwrt_value=1 ;;
force) hwrt_value=2 ;;
esac

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<EOF
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_NEXTGEO_NEXTGI_LOG {\"hwrt\":\"$hwrt\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ "$status" -ne 0 || "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	exit 1
fi
