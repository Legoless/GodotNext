#!/usr/bin/env bash
# Runs res://tests/light_source_parity/light_source_parity_check.tscn with NextGI enabled (#140)
# so textured AreaLight3D SRC world-space relight is verified alongside Forward+, barn doors,
# GM-11 IES profiles, and per-light contact shadows.
#
# Usage:
#   run_light_source_parity_check.sh [--hwrt=auto|off|force] -- <godot> <godot arguments>

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
		echo "run_light_source_parity_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_light_source_parity_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_light_source_parity_check.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/debug/read_page_radiance=true
EOF

"$@" -- --require-nextgi=1
