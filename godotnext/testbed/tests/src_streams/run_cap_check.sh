#!/usr/bin/env bash
# SRC relight stays inside the High texel and shadow-ray caps (#42).
# Usage: run_cap_check.sh -- <godot> <godot arguments>

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--)
		shift
		break
		;;
	*)
		echo "run_cap_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_cap_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_cap_check.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/hardware_ray_tracing=0
global_illumination/nextgi/quality=2
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_SRC_LOG {\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
exit "$status"
