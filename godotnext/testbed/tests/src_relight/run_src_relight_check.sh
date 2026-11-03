#!/usr/bin/env bash
# Runs res://tests/src_relight/src_relight_check.tscn with NextGI enabled (#31).
#
# Usage:
#   run_src_relight_check.sh [--probes] [--shadow-mask] [--hwrt=auto|off|force] -- <godot> <godot arguments>
#
# --probes turns on nextgi/debug/read_page_radiance. --shadow-mask turns on read_shadow_mask.
# Refuses to overwrite an existing override.cfg. Prints Metal validation counts and exits with Godot's status.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

probes=false
shadow_mask=false
hwrt=auto
while [[ $# -gt 0 ]]; do
	case "$1" in
	--probes) probes=true ;;
	--shadow-mask) shadow_mask=true ;;
	--hwrt=auto | --hwrt=off | --hwrt=force) hwrt="${1#--hwrt=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_src_relight_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_src_relight_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_src_relight_check.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/debug/read_page_radiance=$probes
global_illumination/nextgi/debug/read_shadow_mask=$shadow_mask
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_SRC_RELIGHT_LOG {\"probes\":$probes,\"shadow_mask\":$shadow_mask,\"hwrt\":\"$hwrt\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
exit "$status"
