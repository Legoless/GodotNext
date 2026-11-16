#!/usr/bin/env bash
# Checks the UE comparison exporter (#235, #233, #232) on the overlap-heavy kitbash, RT Shadow Yard,
# MegaLights Hall and Upscaler Torture.
#
# Usage:
#   run_ue_export_check.sh <godot> [godot arguments, for example --rendering-driver vulkan]
#
# 1. A --headless export must refuse to run (exit 2): the dummy renderer keeps no MultiMesh
#    instance transforms, so a headless export would lose half the rocks.
# 2. Two rendered exports of res://scenes/swrt_kitbash/swrt_kitbash.tscn (CaptureCam, warmup 120,
#    600 frames) into a temporary folder. Each must exit 0 and log no "ERROR:" or "SCRIPT ERROR"
#    line (a script error inside the exporter would otherwise drop content and still exit 0).
# 3. check_ue_export.gd, headless, on the two exports (see that script for what it checks).
# 4. RT Shadow Yard without --approximate must refuse (exit 1) and name the diffuse_mode,
#    shadow_caster_mask and translucent_shadow approximations it needs; an unknown --approximate id
#    must exit 2; a run the engine ends after 3 frames must exit 1 and write nothing.
# 5. Two rendered exports each of RT Shadow Yard (cam_main, --approximate=diffuse_mode,
#    shadow_caster_mask,translucent_shadow) and MegaLights Hall at --lights=1024 (cam_main,
#    --approximate=diffuse_mode,light_cull_mask,light_projector,translucent_shadow), 60 frames, then
#    check_ue_export.gd on each pair. The hall without light_cull_mask must refuse (exit 1) and name
#    it.
# 6. Upscaler Torture (its camera path, 600 frames, cut at 400): without --approximate it must
#    refuse (exit 1) and name alpha_antialiasing, diffuse_mode, particles, rigid_skin and
#    shader_material; a cut outside the path must exit 2; then two exports with those
#    approximations and check_ue_export.gd on the pair (moving fan and occluders, the character's
#    rigid parts, the cut, UV transforms, texture filters and the shader). Each export log must hold
#    exactly one UPSCALER_TORTURE_RESET line, at the --camera-cuts frame: the cut UE plays is the
#    frame the scene resets the temporal history at.
# 7. The per-row refusals, on tests/ue_export/motion_fixture.tscn (20 frames, rigid_skin): the
#    still fixture must export; a light that moves and returns, a mesh hidden for one frame and a
#    MultiMesh instance that moves and returns must each refuse (exit 1) and name the node with the
#    row it changed at (a first-and-last comparison would miss all three); a Skeleton3D beside its
#    skinned mesh and a camera cull mask that hides a placed layer must refuse and say so. No
#    refused run may write scene.json.
#
# The exports need a window: on a headless Linux machine set DISPLAY to an X server, as on the
# cloud L4 VMs (DISPLAY=:0). Exits non-zero when any step fails.
#
# Example on a Linux L4 VM:
#   DISPLAY=:0 godotnext/testbed/tests/ue_export/run_ue_export_check.sh \
#     ./bin/godot.linuxbsd.editor.x86_64 --rendering-driver vulkan

set -euo pipefail

if [[ $# -lt 1 ]]; then
	echo "run_ue_export_check.sh: pass the Godot binary" >&2
	exit 2
fi
godot=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
godot_args=("$@")

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)

work=$(mktemp -d)
cleanup() {
	rm -rf "$work"
}
trap cleanup EXIT

failures=0
export_args=(--scene=res://scenes/swrt_kitbash/swrt_kitbash.tscn --camera=CaptureCam --warmup=120 --frames=600)

# Prints the step result; counts a failure when the exit code differs from the expected one or
# the log has an error line.
report() {
	local name=$1 status=$2 expected=$3 log=$4
	local errors
	errors=$(grep -c -E 'ERROR:|SCRIPT ERROR' "$log" || true)
	echo "GODOTNEXT_UE_EXPORT_STEP {\"step\":\"$name\",\"exit\":$status,\"expected\":$expected,\"errors\":$errors}"
	if [[ "$status" -ne "$expected" || ("$expected" -eq 0 && "$errors" -ne 0) ]]; then
		grep -A3 -E 'ERROR:|SCRIPT ERROR|ue_export:' "$log" >&2 || true
		tail -n 20 "$log" >&2
		failures=$((failures + 1))
	fi
}

status=0
"$godot" --headless --audio-driver Dummy --path "$testbed_dir" --script res://tools/ue_export/ue_export.gd -- \
	"${export_args[@]}" --out="$work/headless" >"$work/headless.log" 2>&1 || status=$?
report headless_refused "$status" 2 "$work/headless.log"
if ! grep -q 'not --headless' "$work/headless.log"; then
	echo "run_ue_export_check.sh: the headless export did not say why it refused" >&2
	failures=$((failures + 1))
fi

for run in a b; do
	status=0
	"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
		"${export_args[@]}" --out="$work/kitbash_$run" >"$work/kitbash_$run.log" 2>&1 || status=$?
	report "export_kitbash_$run" "$status" 0 "$work/kitbash_$run.log"
	grep '^GODOTNEXT_UE_EXPORT ' "$work/kitbash_$run.log" || true
done

check() {
	local name=$1
	status=0
	"$godot" --headless --audio-driver Dummy --path "$testbed_dir" --script res://tests/ue_export/check_ue_export.gd -- \
		"$name" "$work/${name}_a" "$work/${name}_b" >"$work/check_$name.log" 2>&1 || status=$?
	report "check_$name" "$status" 0 "$work/check_$name.log"
	grep '^GODOTNEXT_RESULT' "$work/check_$name.log" || true
}

# Two rendered exports of one scene into $work/<name>_a and _b.
export_pair() {
	local name=$1
	shift
	for run in a b; do
		status=0
		"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
			"$@" --out="$work/${name}_$run" >"$work/${name}_$run.log" 2>&1 || status=$?
		report "export_${name}_$run" "$status" 0 "$work/${name}_$run.log"
		grep '^GODOTNEXT_UE_EXPORT ' "$work/${name}_$run.log" || true
	done
}

check kitbash

yard_args=(--scene=res://scenes/rt_shadow_yard/rt_shadow_yard.tscn --camera=cam_main --warmup=0 --frames=60)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${yard_args[@]}" --out="$work/yard_refused" >"$work/yard_refused.log" 2>&1 || status=$?
report yard_refused "$status" 1 "$work/yard_refused.log"
for id in diffuse_mode shadow_caster_mask translucent_shadow; do
	if ! grep -q -- "--approximate=$id" "$work/yard_refused.log"; then
		echo "run_ue_export_check.sh: the refused yard export does not name --approximate=$id" >&2
		failures=$((failures + 1))
	fi
done
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${yard_args[@]}" --approximate=no_such_id --out="$work/yard_bad" >"$work/yard_bad.log" 2>&1 || status=$?
report yard_unknown_approximation "$status" 2 "$work/yard_bad.log"

# A run that ends before the export is written (here the engine's --quit-after, as a scene's own
# --frames limit would) must exit 1, say so and write nothing.
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --quit-after 3 --script res://tools/ue_export/ue_export.gd -- \
	"${yard_args[@]}" --approximate=diffuse_mode,shadow_caster_mask,translucent_shadow --out="$work/yard_quit" >"$work/yard_quit.log" 2>&1 || status=$?
report yard_ended_early "$status" 1 "$work/yard_quit.log"
if ! grep -q 'tree quit at camera row' "$work/yard_quit.log" || [[ -e "$work/yard_quit/scene.json" ]]; then
	echo "run_ue_export_check.sh: the run that ended early did not say so, or wrote scene.json" >&2
	failures=$((failures + 1))
fi

export_pair yard "${yard_args[@]}" --approximate=diffuse_mode,shadow_caster_mask,translucent_shadow
check yard

# The hall's two lighting-channel lights light no placed layer: without light_cull_mask the export
# must refuse and name it.
hall_args=(--scene=res://scenes/mega_lights_hall/mega_lights_hall.tscn --camera=cam_main --lights=1024 --warmup=0 --frames=60)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${hall_args[@]}" --approximate=diffuse_mode,light_projector,translucent_shadow --out="$work/hall_refused" >"$work/hall_refused.log" 2>&1 || status=$?
report hall_refused "$status" 1 "$work/hall_refused.log"
if ! grep -q -- "--approximate=light_cull_mask" "$work/hall_refused.log" || [[ -e "$work/hall_refused/scene.json" ]]; then
	echo "run_ue_export_check.sh: the refused hall export does not name --approximate=light_cull_mask, or wrote scene.json" >&2
	failures=$((failures + 1))
fi
export_pair hall "${hall_args[@]}" --approximate=diffuse_mode,light_cull_mask,light_projector,translucent_shadow
check hall

torture_base=(--scene=res://scenes/upscaler_torture/upscaler_torture.tscn --camera=CameraPath/CameraFollow/Camera3D --warmup=0 --frames=600)
torture_cut=400
torture_args=("${torture_base[@]}" --camera-cuts="$torture_cut")
torture_ids=(alpha_antialiasing diffuse_mode particles rigid_skin shader_material)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${torture_args[@]}" --out="$work/torture_refused" >"$work/torture_refused.log" 2>&1 || status=$?
report torture_refused "$status" 1 "$work/torture_refused.log"
for id in "${torture_ids[@]}"; do
	if ! grep -q -- "--approximate=$id" "$work/torture_refused.log"; then
		echo "run_ue_export_check.sh: the refused torture export does not name --approximate=$id" >&2
		failures=$((failures + 1))
	fi
done
if [[ -e "$work/torture_refused/scene.json" ]]; then
	echo "run_ue_export_check.sh: the refused torture export wrote scene.json" >&2
	failures=$((failures + 1))
fi
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${torture_base[@]}" --camera-cuts=600 --out="$work/torture_bad_cut" >"$work/torture_bad_cut.log" 2>&1 || status=$?
report torture_cut_outside_path "$status" 2 "$work/torture_bad_cut.log"
torture_approximations=$(IFS=,; echo "${torture_ids[*]}")
export_pair torture "${torture_args[@]}" --approximate="$torture_approximations"
# The scene prints UPSCALER_TORTURE_RESET when it resets the temporal history (the export drives
# it as a timing run does): the cut the export names must be that frame.
for run in a b; do
	resets=$(grep -o 'UPSCALER_TORTURE_RESET frame=[0-9]*' "$work/torture_$run.log" || true)
	echo "GODOTNEXT_UE_EXPORT_STEP {\"step\":\"torture_reset_$run\",\"resets\":\"${resets//$'\n'/;}\",\"cut\":$torture_cut}"
	if [[ "$resets" != "UPSCALER_TORTURE_RESET frame=$torture_cut" ]]; then
		echo "run_ue_export_check.sh: torture export $run reset the temporal history at '${resets//$'\n'/, }', not only at the --camera-cuts frame $torture_cut" >&2
		failures=$((failures + 1))
	fi
done
check torture

fixture_args=(--scene=res://tests/ue_export/motion_fixture.tscn --camera=Camera --warmup=0 --frames=20 --approximate=rigid_skin)
# One fixture case: its expected exit code and, for a refusal, the text the log must hold.
fixture_case() {
	local name=$1 expected=$2 pattern=$3
	status=0
	"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
		"${fixture_args[@]}" --case="$name" --out="$work/fixture_$name" >"$work/fixture_$name.log" 2>&1 || status=$?
	report "fixture_$name" "$status" "$expected" "$work/fixture_$name.log"
	if [[ "$expected" -eq 0 ]]; then
		if [[ ! -e "$work/fixture_$name/scene.json" ]]; then
			echo "run_ue_export_check.sh: the still fixture wrote no scene.json" >&2
			failures=$((failures + 1))
		fi
	elif ! grep -q -F -- "$pattern" "$work/fixture_$name.log" || [[ -e "$work/fixture_$name/scene.json" ]]; then
		echo "run_ue_export_check.sh: the $name fixture did not refuse with \"$pattern\", or wrote scene.json" >&2
		failures=$((failures + 1))
	fi
}
fixture_case none 0 ""
fixture_case light_returns 1 "Light moved or changed at row"
fixture_case mesh_blinks 1 "Box moved or changed at row"
fixture_case multimesh_returns 1 "Rocks moved or changed at row"
fixture_case skeleton_offset 1 "skinned mesh Skinned whose Skeleton3D is not at the mesh instance's transform"
fixture_case camera_cull 1 "a camera cull mask that hides some placed layers"

if [[ "$failures" -ne 0 ]]; then
	echo "run_ue_export_check.sh: $failures step(s) failed" >&2
	exit 1
fi
echo "run_ue_export_check.sh: all steps passed"
