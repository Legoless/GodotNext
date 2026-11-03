#!/usr/bin/env bash
# Runs res://tests/src_cards/src_cards_check.tscn with NextGI enabled for the run (#30).
#
# Usage:
#   run_src_cards_check.sh [--no-dump] [--hwrt=auto|off|force] -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with nextgi/enabled=true, the
# hardware_ray_tracing mode and, unless --no-dump, nextgi/debug/dump_material_atlas=true (the
# atlas EXRs land in the project's user:// directory and the log gets per-card coverage). Runs
# Godot, removes the file again and refuses to overwrite an existing override.cfg. Prints the
# counts of "ERROR:" lines and Metal validation reports in the log. Exits with Godot's status.
#
# Example on the Apple M5 Max (HWRT tier since #47), with Metal API and shader validation:
#   MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
#   godotnext/testbed/tests/src_cards/run_src_cards_check.sh -- ./bin/godot.macos.editor.arm64 \
#     --path godotnext/testbed --rendering-driver metal res://tests/src_cards/src_cards_check.tscn \
#     -- --scene=res://scenes/color_box_room/cbr_a.tscn --min-coverage=0.95

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

dump=true
hwrt=auto
while [[ $# -gt 0 ]]; do
	case "$1" in
	--no-dump) dump=false ;;
	--hwrt=auto | --hwrt=off | --hwrt=force) hwrt="${1#--hwrt=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_src_cards_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_src_cards_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_src_cards_check.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/debug/dump_material_atlas=$dump
EOF

# pipefail makes the pipeline's status Godot's when Godot fails.
set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_SRC_CARDS_LOG {\"dump\":$dump,\"hwrt\":\"$hwrt\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
exit "$status"
