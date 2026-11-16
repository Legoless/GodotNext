#!/usr/bin/env bash
# Runs res://tests/vsm/page_table.tscn (#220): the Virtual Shadow Map core on the GPU. Two fresh
# processes: VSM off (GODOT_DEV_VSM unset: nothing is allocated until the self-check), then VSM on
# (GODOT_DEV_VSM=1: every shadow-casting light of the scene gets its maps). Each run compares 65,536
# GPU page-table lookups and the clipmap scroll pass with the CPU model, checks the pool at each
# quality against budget V6, and runs 10,000 map allocate and free cycles with RenderingDevice's
# memory counters checked for leaks.
#
# Usage:
#   run_vsm_page_table_check.sh [--work=<dir>] -- <godot> <godot arguments without a scene>
#
# Prints one GODOTNEXT_VSM_PAGE_TABLE line per run with its status and its "ERROR:", "Invalid Metal
# usage", "Error detected in", "Validation Error" and leak line counts (RenderingDevice's leaked
# RIDs and ObjectDB's leaked instances at exit). Exits non-zero when a run fails or logs any of
# those lines.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/vsm/run_vsm_page_table_check.sh -- \
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
		echo "run_vsm_page_table_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vsm_page_table_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"

status=0
for mode in off on; do
	log="$work/vsm_page_table_$mode.log"
	if [[ $mode == on ]]; then
		GODOT_DEV_VSM=1 "$@" res://tests/vsm/page_table.tscn -- --expect-active=true >"$log" 2>&1
	else
		env -u GODOT_DEV_VSM "$@" res://tests/vsm/page_table.tscn -- --expect-active=false >"$log" 2>&1
	fi
	rc=$?
	errors=$(grep -c '^ERROR' "$log")
	metal=$(grep -c 'Invalid Metal usage' "$log")
	detected=$(grep -c 'Error detected in' "$log")
	validation=$(grep -ci 'Validation Error' "$log")
	leaked=$(grep -cE 'was leaked|were leaked|leaked at exit|Leaked instance' "$log")
	echo "GODOTNEXT_VSM_PAGE_TABLE mode=$mode exit=$rc errors=$errors invalid_metal=$metal error_detected=$detected validation=$validation leaked=$leaked log=$log"
	grep -m1 '^GODOTNEXT_RESULT' "$log" | cut -c1-400
	if [[ $rc -ne 0 || $errors -ne 0 || $metal -ne 0 || $detected -ne 0 || $validation -ne 0 || $leaked -ne 0 ]]; then
		status=1
	fi
done
exit $status
