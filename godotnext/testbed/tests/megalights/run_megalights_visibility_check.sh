#!/usr/bin/env bash
# Runs res://tests/megalights/megalights_visibility_check.tscn with NextGI HWRT enabled (#146)
# so MegaLights visibility chain (Hi-Z screen trace, 2x2 block dedupe & stream compaction,
# near-field ray query via trace_shadow(), alpha cutout + #130 material retrace, optional
# #126 far-field TLAS continuation, VSM-method lights with SMRT 2 rays, and optional #182
# SWRT global-SDF continuation) all execute end-to-end on the GPU.
#
# Usage:
#   run_megalights_visibility_check.sh [--hwrt=auto|off|force] -- <godot> <godot arguments>

set -euo pipefail

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
		echo "run_megalights_visibility_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_megalights_visibility_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_megalights_visibility_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

case "$hwrt" in
auto) hwrt_value=0 ;;
off) hwrt_value=1 ;;
force) hwrt_value=2 ;;
esac

cleanup() {
	rm -f "$override"
}
trap cleanup EXIT

cat >"$override" <<EOF
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
EOF

"$@"
