#!/usr/bin/env bash
# Checks the UE comparison exporter (#235, #233, #232, #230, #234) on the overlap-heavy kitbash, RT
# Shadow Yard, MegaLights Hall, Upscaler Torture, Glass & Water, Character Walk, NextGI Perf and the PN
# scenes Foliage Field, Displacement Cliff and Character Crowd.
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
#    refuse (exit 1) and name alpha_antialiasing, diffuse_mode, particles and shader_material (its
#    skinned character exports as a skeletal mesh by default); a cut outside the path must exit 2;
#    then two exports with those approximations and rigid_skin (the character as rigid parts, as
#    the #232 port imports it) and check_ue_export.gd on the pair (moving fan and occluders, the
#    character's rigid parts, the cut, UV transforms, texture filters and the shader). Each export
#    log must hold exactly one UPSCALER_TORTURE_RESET line, at the --camera-cuts frame: the cut UE
#    plays is the frame the scene resets the temporal history at.
# 7. Glass & Water (CaptureCam, 60 frames): without --approximate it must refuse and name
#    translucent_shadow; then two exports with it and check_ue_export.gd on the pair. Character
#    Walk (CaptureCam, the walk's 240 frames), which needs no approximation: two exports and
#    check_ue_export.gd on the pair. NextGI Perf (CaptureCam, --instances=180 --variant=p3, 120
#    frames): without --approximate it must refuse and name camera_far (CaptureCam's 80 m far plane
#    clips the courtyard's far end); then two exports with it and check_ue_export.gd on the pair
#    (the skinned characters as skeletal meshes, the GridMap cells, the far-plane row count).
# 8. The PN scenes (#234): generators/build_pn.gd writes them into the gitignored
#    res://generated/pn_content/ first (a failed generation fails the step; nothing is skipped).
#    Foliage Field (cam_main, 60 frames) without --approximate must refuse and name diffuse_mode;
#    then two exports each of Foliage Field (with it), Displacement Cliff and Character Crowd
#    (cam_main, 60 frames) and check_ue_export.gd on each pair (the 200,000 seeded MultiMesh
#    placements and masked materials, the heightfield and its tiled noise texture, the 100 walkers
#    as skeletal instances with their walk-cycle poses).
# 9. The per-row refusals, on tests/ue_export/motion_fixture.tscn (20 frames, rigid_skin): the
#    still fixture must export; a light that moves and returns, a mesh hidden for one frame, a
#    MultiMesh instance and a GridMap cell that move and return must each refuse (exit 1) and name
#    the node with row 8, the row it changed at (a first-and-last comparison would miss them); a
#    Skeleton3D beside its skinned mesh, a camera cull mask that hides a placed layer and a bone
#    skin scale must refuse and say so. Without rigid_skin the still fixture must export one
#    skeletal instance, and must refuse with each of: the offset skeleton, a skinned mesh without a
#    Skin, a bone skin scale, show_rest_only, a disabled bone, a SkeletonModifier3D, an alpha-blended
#    GridMap item that casts shadows (translucent_shadow), two GridMap items that share a mesh with
#    different shadow settings, and a 5 m far plane (camera_far), which exports with
#    --approximate=camera_far. No refused run may write scene.json. Two exports of the still
#    fixture without rigid_skin go through check_ue_export.gd (its two GridMaps' cells against
#    transforms written out from their settings), and two of mover_far (only the box, a mover,
#    reaches past a 20 m far plane, on row 8) with camera_far.
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

# check <name> [scene]: check_ue_export.gd <scene, default name> on $work/<name>_a and _b.
check() {
	local name=$1 scene=${2:-$1}
	status=0
	"$godot" --headless --audio-driver Dummy --path "$testbed_dir" --script res://tests/ue_export/check_ue_export.gd -- \
		"$scene" "$work/${name}_a" "$work/${name}_b" >"$work/check_$name.log" 2>&1 || status=$?
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
# Without --approximate the character exports as a skeletal mesh, so the refusal names the others.
torture_refused_ids=(alpha_antialiasing diffuse_mode particles shader_material)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${torture_args[@]}" --out="$work/torture_refused" >"$work/torture_refused.log" 2>&1 || status=$?
report torture_refused "$status" 1 "$work/torture_refused.log"
for id in "${torture_refused_ids[@]}"; do
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

glass_args=(--scene=res://scenes/glass_water/glass_water.tscn --camera=CaptureCam --warmup=0 --frames=60)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${glass_args[@]}" --out="$work/glass_refused" >"$work/glass_refused.log" 2>&1 || status=$?
report glass_refused "$status" 1 "$work/glass_refused.log"
for id in translucent_shadow; do
	if ! grep -q -- "--approximate=$id" "$work/glass_refused.log" || [[ -e "$work/glass_refused/scene.json" ]]; then
		echo "run_ue_export_check.sh: the refused Glass & Water export does not name --approximate=$id, or wrote scene.json" >&2
		failures=$((failures + 1))
	fi
done
export_pair glass "${glass_args[@]}" --approximate=translucent_shadow
check glass

export_pair walk --scene=res://scenes/character_walk/character_walk.tscn --camera=CaptureCam --warmup=0 --frames=240
check walk

perf_args=(--scene=res://scenes/nextgi_perf/nextgi_perf.tscn --camera=CaptureCam --warmup=0 --frames=120 --instances=180 --variant=p3)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${perf_args[@]}" --out="$work/perf_refused" >"$work/perf_refused.log" 2>&1 || status=$?
report perf_refused "$status" 1 "$work/perf_refused.log"
if ! grep -q -- "--approximate=camera_far" "$work/perf_refused.log" || [[ -e "$work/perf_refused/scene.json" ]]; then
	echo "run_ue_export_check.sh: the refused NextGI Perf export does not name --approximate=camera_far, or wrote scene.json" >&2
	failures=$((failures + 1))
fi
export_pair perf "${perf_args[@]}" --approximate=camera_far
check perf

# The PN scenes are generated, not committed: build them (byte-identical on every run) first.
status=0
"$godot" --headless --audio-driver Dummy --path "$testbed_dir" --script res://generators/build_pn.gd >"$work/pn_build.log" 2>&1 || status=$?
report pn_generate "$status" 0 "$work/pn_build.log"
foliage_args=(--scene=res://generated/pn_content/foliage_field.tscn --camera=cam_main --warmup=0 --frames=60)
status=0
"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
	"${foliage_args[@]}" --out="$work/foliage_refused" >"$work/foliage_refused.log" 2>&1 || status=$?
report foliage_refused "$status" 1 "$work/foliage_refused.log"
if ! grep -q -- "--approximate=diffuse_mode" "$work/foliage_refused.log" || [[ -e "$work/foliage_refused/scene.json" ]]; then
	echo "run_ue_export_check.sh: the refused Foliage Field export does not name --approximate=diffuse_mode, or wrote scene.json" >&2
	failures=$((failures + 1))
fi
export_pair foliage "${foliage_args[@]}" --approximate=diffuse_mode
check foliage
export_pair cliff --scene=res://generated/pn_content/displacement_cliff.tscn --camera=cam_main --warmup=0 --frames=60
check cliff
export_pair crowd --scene=res://generated/pn_content/character_crowd.tscn --camera=cam_main --warmup=0 --frames=60
check crowd

fixture_args=(--scene=res://tests/ue_export/motion_fixture.tscn --camera=Camera --warmup=0 --frames=20)
# One fixture case: its expected exit code, for a refusal the text the log must hold, and the
# export's --approximate (rigid_skin: the skinned triangle as a rigid part; none: a skeletal mesh).
fixture_case() {
	local name=$1 expected=$2 pattern=$3 approximate=${4:-rigid_skin}
	local label="${name}_$approximate"
	local extra=()
	if [[ "$approximate" != none ]]; then
		extra=(--approximate="$approximate")
	fi
	status=0
	"$godot" --audio-driver Dummy --path "$testbed_dir" ${godot_args[@]+"${godot_args[@]}"} --script res://tools/ue_export/ue_export.gd -- \
		"${fixture_args[@]}" ${extra[@]+"${extra[@]}"} --case="$name" --out="$work/fixture_$label" >"$work/fixture_$label.log" 2>&1 || status=$?
	report "fixture_$label" "$status" "$expected" "$work/fixture_$label.log"
	if [[ "$expected" -eq 0 ]]; then
		if [[ ! -e "$work/fixture_$label/scene.json" ]]; then
			echo "run_ue_export_check.sh: the still fixture ($approximate) wrote no scene.json" >&2
			failures=$((failures + 1))
		fi
	elif ! grep -q -F -- "$pattern" "$work/fixture_$label.log" || [[ -e "$work/fixture_$label/scene.json" ]]; then
		echo "run_ue_export_check.sh: the $name fixture ($approximate) did not refuse with \"$pattern\", or wrote scene.json" >&2
		failures=$((failures + 1))
	fi
}
fixture_case none 0 ""
# Each motion case changes on frame 8 and is back on frame 9.
fixture_case light_returns 1 "Light moved or changed at row 8;"
fixture_case mesh_blinks 1 "Box moved or changed at row 8;"
fixture_case multimesh_returns 1 "Rocks moved or changed at row 8;"
fixture_case gridmap_returns 1 "Grid moved or changed at row 8;"
fixture_case skeleton_offset 1 "skinned mesh Skinned whose Skeleton3D is not at the mesh instance's transform"
fixture_case camera_cull 1 "a camera cull mask that hides some placed layers"
fixture_case skin_scale 1 "skinned mesh Skinned: bone root has a skin scale"
# The skeletal path (the default since #230): the still fixture exports its skinned triangle as a
# skeletal mesh, and the offset skeleton is refused there too.
fixture_case none 0 "" none
fixture_case skeleton_offset 1 "skinned mesh Skinned whose Skeleton3D is not at the mesh instance's transform" none
if ! grep -q '"skeletal":1' "$work/fixture_none_none.log"; then
	echo "run_ue_export_check.sh: the still fixture without rigid_skin did not export one skeletal instance" >&2
	failures=$((failures + 1))
fi
# What the renderer skins besides the bones' poses (a Skin made from the rests, a skin scale, the
# rest pose only, a disabled bone, a modifier) is refused, not exported as the bones' poses.
fixture_case auto_skin 1 "skinned mesh Skinned has no Skin" none
fixture_case skin_scale 1 "skinned mesh Skinned: bone root has a skin scale" none
fixture_case rest_only 1 "skinned mesh Skinned: its Skeleton3D shows the rest pose only" none
fixture_case bone_disabled 1 "skinned mesh Skinned: bone root is disabled" none
fixture_case modifier 1 "skinned mesh Skinned: its Skeleton3D has a SkeletonModifier3D (Turn)" none
fixture_case gridmap_glass 1 "translucent_shadow on Grid (pass --approximate=translucent_shadow" none
fixture_case gridmap_shadows 1 "GridMap Grid: items [0, 1] share a mesh with different shadow settings" none
fixture_case camera_far 1 "camera_far on Camera (pass --approximate=camera_far" none
fixture_case camera_far 0 "" camera_far
if ! grep -q '"approximations":\["camera_far"\]' "$work/fixture_camera_far_camera_far.log"; then
	echo "run_ue_export_check.sh: the fixture with a 5 m far plane did not export camera_far" >&2
	failures=$((failures + 1))
fi
# The still fixture's two GridMaps (one with a rotated node, non-cubic cells centered on two axes,
# a cell scale, an item mesh transform and rotated cells) against transforms written out by hand.
export_pair fixture "${fixture_args[@]}"
check fixture
# A mover alone reaches past the far plane, on one row: the count follows movers row by row.
export_pair fixture_far "${fixture_args[@]}" --case=mover_far --approximate=camera_far
check fixture_far fixture

if [[ "$failures" -ne 0 ]]; then
	echo "run_ue_export_check.sh: $failures step(s) failed" >&2
	exit 1
fi
echo "run_ue_export_check.sh: all steps passed"
