#!/usr/bin/env bash
# Runs res://tests/vsm/fog_translucency.tscn (#215): VSM coarse-page lookups in volumetric fog and
# forward translucency, plus Fog Hall and VSM Flythrough checks.
# Three fresh processes: legacy shadow atlas (GODOT_DEV_VSM unset), VSM on (GODOT_DEV_VSM=1), and
# VSM with 25% pool overflow (GODOT_DEV_VSM=25%, verifying coarse pages stay resident with 0 misses).
#
# Usage:
#   run_vsm_fog_translucency_check.sh [--work=<dir>] -- <godot> <godot arguments without a scene>

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
		echo "run_vsm_fog_translucency_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vsm_fog_translucency_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"

status=0
for run_kind in atlas vsm overflow25; do
	log="$work/vsm_fog_translucency_$run_kind.log"
	case $run_kind in
	atlas) env -u GODOT_DEV_VSM "$@" res://tests/vsm/fog_translucency.tscn -- --shadows=atlas >"$log" 2>&1 ;;
	vsm) GODOT_DEV_VSM=1 "$@" res://tests/vsm/fog_translucency.tscn -- --shadows=vsm >"$log" 2>&1 ;;
	overflow25) GODOT_DEV_VSM=25% "$@" res://tests/vsm/fog_translucency.tscn -- --shadows=vsm >"$log" 2>&1 ;;
	esac
	rc=$?
	errors=$(grep -c '^ERROR' "$log")
	script_errors=$(grep -c 'SCRIPT ERROR' "$log")
	results=$(grep -c '^GODOTNEXT_RESULT' "$log")
	metal=$(grep -c 'Invalid Metal usage' "$log")
	detected=$(grep -c 'Error detected in' "$log")
	validation=$(grep -ci 'Validation Error' "$log")
	leaked=$(grep -cE 'was leaked|were leaked|leaked at exit|Leaked instance' "$log")
	echo "GODOTNEXT_VSM_FOG_TRANSLUCENCY run=$run_kind exit=$rc errors=$errors script_errors=$script_errors invalid_metal=$metal error_detected=$detected validation=$validation leaked=$leaked results=$results log=$log"
	grep -m1 '^GODOTNEXT_RESULT' "$log" | cut -c1-600
	if [[ $rc -ne 0 || $errors -ne 0 || $script_errors -ne 0 || $metal -ne 0 || $detected -ne 0 || $validation -ne 0 || $leaked -ne 0 || $results -ne 1 ]]; then
		status=1
	fi
done
exit $status
