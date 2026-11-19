#!/usr/bin/env bash
# Runs res://tests/vsm/projection.tscn (#222): Virtual Shadow Map projection and SMRT soft shadows in
# the Forward+ light loop. Three fresh processes: the shadow atlas (GODOT_DEV_VSM unset, --shadows=atlas:
# the scene's shadows through the atlas path, saved as the reference), VSM on (GODOT_DEV_VSM=1,
# --shadows=vsm: no atlas pass for the lights the view samples, every hard shadow where the slab's is and
# where the atlas path's is, penumbras that widen with the light's size), and VSM on with Prefiltered
# Distant (--prefiltered=true: the sun's hard shadow still where the slab's is).
#
# Usage:
#   run_vsm_projection_check.sh [--work=<dir>] -- <godot> <godot arguments without a scene>
#
# Prints one GODOTNEXT_VSM_PROJECTION line per run with its status and its "ERROR:", "SCRIPT ERROR:",
# "Invalid Metal usage", "Error detected in", "Validation Error" and leak line counts. Exits non-zero
# when a run fails, logs any of those lines or does not print exactly one GODOTNEXT_RESULT line.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/vsm/run_vsm_projection_check.sh -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal

set -uo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
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
		echo "run_vsm_projection_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vsm_projection_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"

status=0
for run_kind in atlas vsm prefiltered; do
	log="$work/vsm_projection_$run_kind.log"
	case $run_kind in
	atlas) env -u GODOT_DEV_VSM "$@" res://tests/vsm/projection.tscn -- --shadows=atlas --out="$work/atlas" >"$log" 2>&1 ;;
	vsm) GODOT_DEV_VSM=1 "$@" res://tests/vsm/projection.tscn -- --shadows=vsm --out="$work/vsm" --reference="$work/atlas" >"$log" 2>&1 ;;
	prefiltered) GODOT_DEV_VSM=1 "$@" res://tests/vsm/projection.tscn -- --shadows=vsm --prefiltered=true --out="$work/prefiltered" >"$log" 2>&1 ;;
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
	echo "GODOTNEXT_VSM_PROJECTION run=$run_kind exit=$rc errors=$errors script_errors=$script_errors invalid_metal=$metal error_detected=$detected validation=$validation leaked=$leaked results=$results log=$log"
	grep -m1 '^GODOTNEXT_RESULT' "$log" | cut -c1-600
	if [[ $rc -ne 0 || $errors -ne 0 || $script_errors -ne 0 || $metal -ne 0 || $detected -ne 0 || $validation -ne 0 || $leaked -ne 0 || $results -ne 1 ]]; then
		status=1
	fi
done
exit $status
