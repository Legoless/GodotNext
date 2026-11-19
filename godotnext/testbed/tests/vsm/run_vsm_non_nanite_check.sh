#!/usr/bin/env bash
# Runs res://tests/vsm/non_nanite.tscn (#218): Virtual Shadow Map non-Nanite per-page GPU instance
# culling and indirect raster. Four fresh processes: VSM off (GODOT_DEV_VSM unset: nothing runs),
# VSM on with the controlled non-Nanite caster scene (GODOT_DEV_VSM=1: regular meshes, Skeleton3D,
# blend shapes, 3D/2D MultiMesh, GPUParticles3D, alpha-scissor/alpha-hash cutouts, VERTEX shader and
# sub-texel culling), and VSM on with ShadowGallery (GODOT_DEV_VSM=1 --gallery=true: cam_casters and
# cam_main, every shadowed light with maps has casters, and 0 missing_from_vsm against legacy render_shadows),
# and VSM on with the controlled scene and a 4,096-pair list (GODOT_DEV_VSM_PAIR_CAPACITY=4096 --overflow=true:
# the list overflows, the dropped pairs' pages are marked and drawn again, and a later frame drops nothing).
#
# Usage:
#   run_vsm_non_nanite_check.sh [--work=<dir>] -- <godot> <godot arguments without a scene>
#
# Prints one GODOTNEXT_VSM_NON_NANITE line per run with its status and its "ERROR:", "SCRIPT ERROR:",
# "Invalid Metal usage", "Error detected in", "Validation Error" and leak line counts. Exits non-zero
# when a run fails, logs any of those lines or does not print exactly one GODOTNEXT_RESULT line.

set -uo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

work=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--work=*) work="${1#--work=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_vsm_non_nanite_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vsm_non_nanite_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"

status=0
for run_kind in off on gallery overflow; do
	log="$work/vsm_non_nanite_$run_kind.log"
	case $run_kind in
	off) env -u GODOT_DEV_VSM "$@" res://tests/vsm/non_nanite.tscn -- --expect-active=false >"$log" 2>&1 ;;
	on) GODOT_DEV_VSM=1 "$@" res://tests/vsm/non_nanite.tscn -- --expect-active=true >"$log" 2>&1 ;;
	gallery) GODOT_DEV_VSM=1 "$@" res://tests/vsm/non_nanite.tscn -- --expect-active=true --gallery=true >"$log" 2>&1 ;;
	overflow) GODOT_DEV_VSM=1 GODOT_DEV_VSM_PAIR_CAPACITY=4096 "$@" res://tests/vsm/non_nanite.tscn -- --expect-active=true --overflow=true >"$log" 2>&1 ;;
	esac
	rc=$?
	errors=$(grep -c '^ERROR' "$log")
	script_errors=$(grep -c 'SCRIPT ERROR' "$log")
	# The script prints its result once and exits 1 when it fails; a run that never got there has none.
	results=$(grep -c '^GODOTNEXT_RESULT' "$log")
	metal=$(grep -c 'Invalid Metal usage' "$log")
	detected=$(grep -c 'Error detected in' "$log")
	validation=$(grep -ci 'Validation Error' "$log")
	leaked=$(grep -cE 'was leaked|were leaked|leaked at exit|Leaked instance' "$log")
	echo "GODOTNEXT_VSM_NON_NANITE run=$run_kind exit=$rc errors=$errors script_errors=$script_errors invalid_metal=$metal error_detected=$detected validation=$validation leaked=$leaked results=$results log=$log"
	grep -m1 '^GODOTNEXT_RESULT' "$log" | cut -c1-400
	if [[ $rc -ne 0 || $errors -ne 0 || $script_errors -ne 0 || $metal -ne 0 || $detected -ne 0 || $validation -ne 0 || $leaked -ne 0 || $results -ne 1 ]]; then
		status=1
	fi
done
exit $status
