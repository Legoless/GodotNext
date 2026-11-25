#!/usr/bin/env bash
# Runs res://tests/megalights/megalights_denoise_check.tscn (#135, contract M5) in two phases:
#   1. Core phase (default env, MegaLights off by default): verifies zero allocation & byte-identical
#      legacy frame, then exercises full-res & half-res shade + temporal + 2-pass spatial denoise,
#      firefly clamped-energy ratio (<= 2%), rr_bypass, and denoise_reset_regions via check hooks.
#   2. Viewport views phase (GODOTNEXT_MEGALIGHTS_INPUTS=1): verifies live Viewport debug draws
#      (ML_NOISY, ML_DENOISED, ML_VARIANCE) and TSR/FSR2 + prepass velocity handoff.
#
# Usage:
#   run_megalights_denoise_check.sh [--hwrt=auto|off|force] -- <godot> <godot arguments>

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
		echo "run_megalights_denoise_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_megalights_denoise_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_megalights_denoise_check.sh: $override already exists; not overwriting it" >&2
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

"$@" -- --ml-denoise-check=core
GODOTNEXT_MEGALIGHTS_INPUTS=1 "$@" -- --ml-denoise-check=viewport_views
