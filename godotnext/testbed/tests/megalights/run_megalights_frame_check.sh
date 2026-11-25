#!/usr/bin/env bash
# Runs res://tests/megalights/megalights_frame_check.tscn (#143, contracts M1 & M6, GM-9, GM-10)
# with NextGI HWRT enabled so MegaLights frame integration, Forward+ color-pass composite,
# custom light() fallback identity, per-light MEGALIGHTS_DISALLOW / MEGALIGHTS_ALLOW /
# RAY_TRACED / VIRTUAL shadow methods, zero-alloc OFF restoration, and NextGI SRC <= 256,000
# ray cap interplay all execute end-to-end on the GPU.
#
# Usage:
#   run_megalights_frame_check.sh [--hwrt=auto|off|force] -- <godot> <godot arguments>

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
		echo "run_megalights_frame_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_megalights_frame_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_megalights_frame_check.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/hardware_ray_tracing.macos=$hwrt_value
EOF

"$@"
