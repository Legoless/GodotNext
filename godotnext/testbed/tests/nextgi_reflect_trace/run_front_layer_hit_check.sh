#!/usr/bin/env bash
# #276. The front layer's gather must leave the view's specular hit distances alone. Runs
# res://tests/nextgi_reflect_trace/reflect_trace_check.tscn through run_reflect_trace_check.sh three
# times on Glass & Water (every material at roughness 0.2, so the opaque room holds traced hits around
# the glass and the water), at one quality:
#   off         front-layer reflections off; dumps the view's hit distance texture;
#   off_repeat  off again; the texture must equal the first dump on every texel, which shows the
#               comparison is deterministic;
#   on          front-layer reflections on; the front layer must trace, its own hit distance target
#               must have the trace size, and the view's texture must equal the off dump bit for bit
#               outside the front layer (the alpha-blended meshes' screen bounds). Texels inside are
#               reported (compare_inside_differing).
# The comparisons need at least 1000 finite hit distances outside the front layer in the off dump (High
# has about 8000 at 640x360), so an overwrite cannot go unseen.
# Each pass also has the reflect trace check's own requirements and its 0-error, 0-validation bar.
#
# Usage:
#   run_front_layer_hit_check.sh [--quality=high|epic] [--work=<directory>] -- <godot> <godot arguments without a scene>
#
# High by default. The dumps and logs stay in --work (a new temporary directory by default).
# Exits non-zero when any pass fails.
#
# Example on a Linux machine with an NVIDIA card:
#   godotnext/testbed/tests/nextgi_reflect_trace/run_front_layer_hit_check.sh --quality=epic -- \
#     ./bin/godot.linuxbsd.editor.x86_64 --path godotnext/testbed --rendering-driver vulkan \
#     --gpu-validation --audio-driver Dummy --resolution 640x360

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runner="$script_dir/run_reflect_trace_check.sh"
scene="res://tests/nextgi_reflect_trace/reflect_trace_check.tscn"
quality=high
work=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	--quality=high | --quality=epic)
		quality="${1#--quality=}"
		;;
	--work=*)
		work="${1#--work=}"
		;;
	--)
		shift
		break
		;;
	*)
		echo "run_front_layer_hit_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_front_layer_hit_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"
# Godot changes its working directory to the project on --path, so the dump paths must be absolute.
work=$(cd "$work" && pwd)
expect=$(printf '%s' "$quality" | tr '[:lower:]' '[:upper:]')
args=(--scene=res://scenes/glass_water/glass_water.tscn --roughness=0.2 "--expect-quality=$expect")
compare=(--min-compared-hits=1000 "--compare=$work/off.bin")
failed=0

run_pass() { # run_pass <name> <check arguments...>
	local name=$1
	shift
	set +e
	bash "$runner" "--quality=$quality" -- "$@" > "$work/$name.log" 2>&1
	local status=$?
	set -e
	echo "GODOTNEXT_FRONT_LAYER_HIT $name exit=$status $(grep -m1 '^GODOTNEXT_REFLECT_TRACE_LOG ' "$work/$name.log" || true)"
	grep -m1 '^GODOTNEXT_REFLECT_TRACE GODOTNEXT_RESULT ' "$work/$name.log" || true
	if [[ "$status" -ne 0 ]]; then
		failed=1
	fi
}

godot_cmd=("$@")
run_pass off "${godot_cmd[@]}" "$scene" -- "${args[@]}" --front-layer=false "--dump=$work/off.bin"
run_pass off_repeat "${godot_cmd[@]}" "$scene" -- "${args[@]}" --front-layer=false "${compare[@]}"
run_pass on "${godot_cmd[@]}" "$scene" -- "${args[@]}" --front-layer=true "${compare[@]}" "--dump=$work/on.bin"

echo "run_front_layer_hit_check.sh: work directory $work"
if [[ "$failed" -ne 0 ]]; then
	echo "run_front_layer_hit_check.sh: a pass failed" >&2
	exit 1
fi
