#!/usr/bin/env bash
# Runs the #188 voxel lighting checks:
#   res://tests/voxel_lighting/voxel_slot_check.tscn (GPU texture space, direction slots, D6,
#   readback gating, refill after a tier switch) at quality High on the hardware tier,
#   res://tests/voxel_lighting/voxel_thin_wall_check.tscn (no light through 0.2 m walls, from
#   either room, whose inside voxel centers lie within the mesh SDF's eps of a face) at quality
#   High on the hardware tier, and
#   res://tests/voxel_lighting/voxel_lighting_check.tscn (kitbash stores hits and covers some)
#   at quality High, where it must print VOXEL_LIGHTING_ASSERTED, and again at quality Medium
#   (Lumen Lite), where it must print its coverage on a VOXEL_LIGHTING_LITE line for information.
#   res://tests/voxel_lighting/voxel_relight_frame_check.tscn (software tier at quality High: a
#   light switched on this frame shades the wall's voxels this frame, from the SRC relight slot)
#   must print VOXEL_RELIGHT_FRAME_OK.
#   GodotNext: ps_swrt_trace (#186, #188). The kitbash runs on both tiers: hardware_ray_tracing
#   Auto (0, the hardware tier here), Off (1, the software tier, whose SRC is captured and relit
#   too) and, with --deny-ray-query, Auto under debug/deny_ray_query (the software tier as on a
#   device without ray query; the check then also asserts NO_RAY_QUERY, no RT arena, no ray-query
#   ShaderRD and SWRT instances). The Off and deny rows pass --expected-tier=SWRT, so they fail
#   when the kitbash runs on another tier; the Auto rows pass --expected-tier=device, so on a
#   device that reports SUPPORTS_RAY_QUERY_HARDWARE they fail unless the kitbash runs on HWRT.
# Voxel lighting asserts at High, never at the device's auto pick (lead decision 2026-11-10,
# plan §21.2.5): Lite's half-size surface cache keeps radiance pages for few of the kitbash's
# 2,400 rocks, and voxel lighting resamples only paged instances. The Lite run still fails on a
# missing counter, zero stored hits or a validation error.
#
# Usage:
#   run_voxel_lighting_check.sh [--deny-ray-query] -- <godot> <godot arguments without a scene>
#
# Refuses to overwrite an existing override.cfg. Prints Metal validation counts per run and
# exits non-zero when a check fails or a run logs a validation error.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

deny=false
while [[ $# -gt 0 ]]; do
	case "$1" in
	--deny-ray-query) deny=true ;;
	--)
		shift
		break
		;;
	*)
		echo "run_voxel_lighting_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_voxel_lighting_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_voxel_lighting_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

# Scene, quality (1 Medium, 2 High), hardware_ray_tracing (0 Auto, 1 Off), the line the run
# must print ("-" for none) and the tier voxel_lighting_check.tscn must run on ("-" for any,
# "device" for the device's Auto pick).
if [[ "$deny" == true ]]; then
	runs=(
		"res://tests/voxel_lighting/voxel_lighting_check.tscn 2 0 VOXEL_LIGHTING_ASSERTED SWRT"
		"res://tests/voxel_lighting/voxel_lighting_check.tscn 1 0 VOXEL_LIGHTING_LITE SWRT"
		"res://tests/voxel_lighting/voxel_relight_frame_check.tscn 2 0 VOXEL_RELIGHT_FRAME_OK SWRT"
	)
else
	runs=(
		"res://tests/voxel_lighting/voxel_slot_check.tscn 2 0 - -"
		"res://tests/voxel_lighting/voxel_thin_wall_check.tscn 2 0 - -"
		"res://tests/voxel_lighting/voxel_lighting_check.tscn 2 0 VOXEL_LIGHTING_ASSERTED device"
		"res://tests/voxel_lighting/voxel_lighting_check.tscn 1 0 VOXEL_LIGHTING_LITE device"
		"res://tests/voxel_lighting/voxel_lighting_check.tscn 2 1 VOXEL_LIGHTING_ASSERTED SWRT"
		"res://tests/voxel_lighting/voxel_lighting_check.tscn 1 1 VOXEL_LIGHTING_LITE SWRT"
		"res://tests/voxel_lighting/voxel_relight_frame_check.tscn 2 1 VOXEL_RELIGHT_FRAME_OK SWRT"
	)
fi

failed=0
for run in "${runs[@]}"; do
	read -r scene quality hwrt expected tier <<<"$run"
	scene_args=()
	if [[ "$tier" != "-" ]]; then
		scene_args=(-- "--expected-tier=$tier")
	fi
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt
global_illumination/nextgi/quality=$quality
global_illumination/nextgi/debug/deny_ray_query=$deny
CFG
	set +e
	"$@" "$scene" ${scene_args[@]+"${scene_args[@]}"} 2>&1 | tee "$log"
	status=${PIPESTATUS[0]}
	set -e
	errors=$(grep -c 'ERROR:' "$log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	metal_shader=$(grep -c 'Error detected in' "$log" || true)
	expected_found=true
	if [[ "$expected" != "-" ]] && ! grep -q "^$expected " "$log"; then
		expected_found=false
	fi
	echo "GODOTNEXT_VOXEL_LIGHTING_LOG {\"scene\":\"$scene\",\"quality\":$quality,\"hwrt\":$hwrt,\"deny_ray_query\":$deny,\"expected_tier\":\"$tier\",\"expected_line\":\"$expected\",\"expected_found\":$expected_found,\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
	if [[ $status -ne 0 || $errors -ne 0 || $metal_usage -ne 0 || $metal_shader -ne 0 || $expected_found != true ]]; then
		failed=1
	fi
done
exit "$failed"
