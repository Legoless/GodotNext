#!/usr/bin/env bash
# Runs res://tests/frame_block/frame_block_check.tscn with NextGI enabled for the run (#26).
#
# Usage:
#   run_frame_block_check.sh [--half-resolution] [--hwrt=auto|off|force] [--quality=0|1|2|3] [--disabled] \
#     [--deny-ray-query] -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with nextgi/enabled=true (false with
# --disabled), the hardware_ray_tracing mode, nextgi/quality (0 Auto by default, 1 Medium,
# 2 High, 3 Epic) and, with --half-resolution, gi/use_half_resolution=true. --deny-ray-query sets
# debug/deny_ray_query (a device without ray query: the software tier, NO_RAY_QUERY, with Auto).
# Runs Godot, removes
# the file again and refuses to overwrite an existing override.cfg. Prints the counts of "ERROR:"
# lines and Metal validation reports in the log. Exits with Godot's status.
#
# Example on the Apple M5 Max once it has the HWRT tier (#47):
#   godotnext/testbed/tests/frame_block/run_frame_block_check.sh -- ./bin/godot.macos.editor.arm64 \
#     --path godotnext/testbed --rendering-driver metal --gpu-profile \
#     res://tests/frame_block/frame_block_check.tscn -- --expect=active --frames=600

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

half=false
hwrt=auto
quality=0
deny=false
enabled=true
while [[ $# -gt 0 ]]; do
	case "$1" in
	--half-resolution) half=true ;;
	--hwrt=auto | --hwrt=off | --hwrt=force) hwrt="${1#--hwrt=}" ;;
	--quality=0 | --quality=1 | --quality=2 | --quality=3) quality="${1#--quality=}" ;;
	--disabled) enabled=false ;;
	--deny-ray-query) deny=true ;;
	--)
		shift
		break
		;;
	*)
		echo "run_frame_block_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_frame_block_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_frame_block_check.sh: $override already exists; not overwriting it" >&2
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

global_illumination/nextgi/enabled=$enabled
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
global_illumination/nextgi/quality=$quality
global_illumination/gi/use_half_resolution=$half
global_illumination/nextgi/debug/deny_ray_query=$deny
EOF

# pipefail makes the pipeline's status Godot's when Godot fails.
set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_FRAME_BLOCK_LOG {\"half_resolution\":$half,\"hwrt\":\"$hwrt\",\"deny_ray_query\":$deny,\"quality\":$quality,\"enabled\":$enabled,\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
exit "$status"
