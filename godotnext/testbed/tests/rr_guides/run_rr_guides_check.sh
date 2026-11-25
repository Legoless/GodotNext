#!/usr/bin/env bash
# #205. The DLSS Ray Reconstruction guide checks of res://tests/rr_guides/rr_guides_check.tscn:
#   off        GODOT_DEV_RR_GUIDES unset: no guide texture, and no RR shader variant is compiled,
#              on Color Box Room A and on its virtual-mesh twin.
#   factor     the specular albedo is the factor the indirect specular light is scaled by, the
#              diffuse albedo is the material's, transparent surfaces leave the guides alone, and
#              under a depth-prepass-alpha surface all three guides say "no surface".
#   cbr_a      Color Box Room A: albedo, normal and roughness guides against the scene's meshes
#              and materials; the specular trace skips every surface, so the hit distance is 65504.
#   cbr_a_vg   Color Box Room A built from virtual meshes: the same guide checks, on guides NextGeo
#              Stage B writes. Up to 5% of the samples may show another surface than the CPU ray
#              finds on the full meshes (the run reports how many).
#   corridor   Roughness Corridor from Cam0 and from CaptureCam: the same guide checks.
#   upscaled   Color Box Room A with FSR 2.2 and Roughness Corridor with TSR, both at scale 0.67:
#              the guides at the internal size. These scenes are static, so this does not check
#              the guides of moving surfaces, which the separate motion pass of upscaling writes.
#   attachments Roughness Corridor (NextGI off) with a velocity attachment, a separate specular
#              attachment and both in the opaque pass: each moves the guides' clear flags, and
#              the sky the camera sees must hold the clear values.
#   loads      The opaque pass clears the guides while it loads other attachments (#275):
#              Roughness Corridor with the background set to Keep (color loads, no clear color
#              before the guides) and with NextGI and motion vectors (the TAA velocity plan loads
#              velocity), and Color Box Room A's virtual-mesh twin with Keep, where Stage B
#              clears them.
#   hit_rule   The unit test of the hit distance rule (test_hit_distance_rule.gd), headless.
#   hit        Roughness Corridor from Cam0 and from CaptureCam: the hit distance against the
#              reference path tracer's reflections-only mode. The ratios guide / reference must
#              number at least 64, with the median within 1% of 1, p05 >= 0.85 and p95 <= 1.15
#              (hit_distance_rule.gd). These passes sample every odd pixel in x and y (--step=2)
#              rather than every fourth: only pixels whose reflections always hit within the
#              trace's range can be compared, and at --step=4 CaptureCam has fewer than 64.
#
# Usage:
#   run_rr_guides_check.sh [--work=<directory>] -- <godot> <godot arguments without a scene>
#
# Writes godotnext/testbed/override.cfg (git ignores it) per pass and removes it. Refuses to
# overwrite an existing one. The off and factor passes run with the shader cache off and
# GODOTNEXT_SPIRV_DUMP_DIR set, so the SPIR-V files show which shader variants were compiled.
# Exits non-zero when a pass fails or logs any "ERROR:", "Validation Error", "Invalid Metal
# usage" or "Error detected in" line.
#
# Example on a Linux machine with an NVIDIA card:
#   godotnext/testbed/tests/rr_guides/run_rr_guides_check.sh -- ./bin/godot.linuxbsd.editor.x86_64 \
#     --path godotnext/testbed --rendering-driver vulkan --gpu-validation --audio-driver Dummy

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
unset GODOT_DEV_RR_GUIDES || true

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
scene="res://tests/rr_guides/rr_guides_check.tscn"
work=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	--work=*)
		work="${1#--work=}"
		;;
	--)
		shift
		break
		;;
	*)
		echo "run_rr_guides_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_rr_guides_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_rr_guides_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"
godot_cmd=("$@")
failed=0

cleanup() {
	rm -f "$override"
}
trap cleanup EXIT

# The shader cache would hide which variants a run compiles.
no_cache=$'[rendering]\n\nshader_compiler/shader_cache/enabled=false\n'
# The RR mode runs NextGI HWRT at Epic. The reference path tracer's reflections-only mode gives
# the hit distance to compare with.
nextgi_epic=$'[rendering]\n\nglobal_illumination/nextgi/enabled=true\nglobal_illumination/nextgi/hardware_ray_tracing=0\nglobal_illumination/nextgi/hardware_ray_tracing.macos=0\nglobal_illumination/nextgi/quality=3\n'
upscaled=$'scaling_3d/scale=0.67\n'
nextgi_epic_reference="${nextgi_epic}global_illumination/nextgi/debug/reference_mode=3
global_illumination/nextgi/debug/reference_readback=true
"

# run_pass <label> <GODOT_DEV_RR_GUIDES value, or ""> <override.cfg body> <check arguments...>
run_pass() {
	local label=$1
	local guides=$2
	local body=$3
	shift 3
	local log="$work/$label.log"
	printf '%s' "$body" >"$override"
	set +e
	if [[ -n "$guides" ]]; then
		GODOT_DEV_RR_GUIDES="$guides" GODOTNEXT_SPIRV_DUMP_DIR="$work/spv_$label" "${godot_cmd[@]}" "$scene" -- "$@" >"$log" 2>&1
	else
		GODOTNEXT_SPIRV_DUMP_DIR="$work/spv_$label" "${godot_cmd[@]}" "$scene" -- "$@" >"$log" 2>&1
	fi
	local status=$?
	set -e
	rm -f "$override"
	local result
	result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
	local errors
	errors=$(grep -c 'ERROR:' "$log" || true)
	local validation
	validation=$(grep -c -i 'Validation Error' "$log" || true)
	local metal
	metal=$(grep -c -E 'Invalid Metal usage|Error detected in' "$log" || true)
	echo "RR_GUIDES $label status=$status errors=$errors validation_errors=$validation metal=$metal $result"
	if [[ "$status" -ne 0 || "$result" != *'"pass":true'* || "$errors" -ne 0 || "$validation" -ne 0 || "$metal" -ne 0 ]]; then
		echo "run_rr_guides_check.sh: the $label pass failed (log $log)" >&2
		failed=1
	fi
}

# The SPIR-V files of the variants that write the RR guides in a dump directory. The color pass
# variants are 30 to 93 (twice the 15 depth variants come first) and those with the RR guides
# flag are the upper half, 62 to 93; the four Stage B variants with the guides are 118 to 121
# (the doctest "[Upscalers][RRGuides] Color pass flag counts and shader version layout" pins both).
count_rr_variants() {
	find "$1" -name '*.spv' 2>/dev/null | grep -c -E '/SceneForwardClusteredShaderRD\.v(6[2-9]|[78][0-9]|9[0-3]|11[89]|12[01])\.|/RrGuidePackShaderRD\.' || true
}

count_color_variants() {
	find "$1" -name '*.spv' 2>/dev/null | grep -c -E '/SceneForwardClusteredShaderRD\.v' || true
}

# check_off_variants <label>: the pass compiled scene shader variants, none of them with the guides.
check_off_variants() {
	local color rr
	color=$(count_color_variants "$work/spv_$1")
	rr=$(count_rr_variants "$work/spv_$1")
	echo "RR_GUIDES $1 compiled scene shader files=$color with RR guides=$rr"
	if [[ "$color" -eq 0 || "$rr" -ne 0 ]]; then
		echo "run_rr_guides_check.sh: with the guides off, $rr RR shader files were compiled ($color scene shader files in all)" >&2
		failed=1
	fi
}

# The hit distance rule's unit test: no GPU, no window, no override.cfg.
rule_log="$work/hit_rule.log"
set +e
"${godot_cmd[@]}" --headless --script res://tests/rr_guides/test_hit_distance_rule.gd >"$rule_log" 2>&1
rule_status=$?
set -e
rule_result=$(grep -m1 '^test_hit_distance_rule: ' "$rule_log" || true)
rule_errors=$(grep -c -E 'ERROR:|SCRIPT ERROR' "$rule_log" || true)
echo "RR_GUIDES hit_rule status=$rule_status errors=$rule_errors $rule_result"
if [[ "$rule_status" -ne 0 || "$rule_result" != 'test_hit_distance_rule: ok' || "$rule_errors" -ne 0 ]]; then
	echo "run_rr_guides_check.sh: the hit_rule unit test failed (log $rule_log)" >&2
	failed=1
fi

run_pass off "" "$no_cache" --rr-check=off
check_off_variants off
run_pass off_vg "" "$no_cache" --rr-check=off --scene=res://scenes/color_box_room/cbr_a_vg.tscn
check_off_variants off_vg

run_pass factor 1 "$no_cache" --rr-check=factor
on_rr=$(count_rr_variants "$work/spv_factor")
echo "RR_GUIDES factor compiled shader files with RR guides=$on_rr"
if [[ "$on_rr" -eq 0 ]]; then
	echo "run_rr_guides_check.sh: with the guides on, no RR shader file was compiled" >&2
	failed=1
fi

run_pass cbr_a 1 "$nextgi_epic_reference" --rr-check=scene --scene=res://scenes/color_box_room/cbr_a.tscn --camera=CaptureCam
run_pass cbr_a_vg 1 "$nextgi_epic" --rr-check=scene --scene=res://scenes/color_box_room/cbr_a_vg.tscn --camera=CaptureCam --require-stage-b=true --max-surface-mismatch=0.05
run_pass corridor_cam0 1 "$nextgi_epic" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=Cam0
run_pass corridor_capturecam 1 "$nextgi_epic" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam
run_pass cbr_a_fsr2 1 "${nextgi_epic}scaling_3d/mode=2
scaling_3d/mode.macos=2
$upscaled" --rr-check=scene --scene=res://scenes/color_box_room/cbr_a.tscn --camera=CaptureCam
run_pass corridor_tsr 1 "${nextgi_epic}scaling_3d/mode=7
scaling_3d/mode.macos=7
$upscaled" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam
# Roughness Corridor's CaptureCam sees the sky, where the guides must hold their clear values.
run_pass corridor_velocity 1 "" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam --motion-vectors=true
run_pass corridor_specular 1 "" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam --separate-specular=true
run_pass corridor_specular_velocity 1 "" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam --separate-specular=true --motion-vectors=true
run_pass corridor_keep 1 "" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam --background=keep
run_pass corridor_taa_velocity 1 "$nextgi_epic" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam --motion-vectors=true
run_pass cbr_a_vg_keep 1 "$nextgi_epic" --rr-check=scene --scene=res://scenes/color_box_room/cbr_a_vg.tscn --camera=CaptureCam --require-stage-b=true --max-surface-mismatch=0.05 --background=keep
run_pass hit_cam0 1 "$nextgi_epic_reference" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=Cam0 --step=2 --min-hit-samples=64
run_pass hit_capturecam 1 "$nextgi_epic_reference" --rr-check=scene --scene=res://scenes/roughness_corridor/roughness_corridor.tscn --camera=CaptureCam --step=2 --min-hit-samples=64

if [[ "$failed" -ne 0 ]]; then
	echo "run_rr_guides_check.sh: failed (logs in $work)" >&2
	exit 1
fi
echo "RR_GUIDES all passes passed (logs in $work)"
