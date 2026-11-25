#!/usr/bin/env bash
# Runs res://tests/vsm/caching.tscn (#212): Virtual Shadow Map caching, one fresh process per case.
# VSM off (GODOT_DEV_VSM unset: nothing is active), then with GODOT_DEV_VSM=1:
# - static: a static scene and camera re-render at most 1% of the allocated pages in any frame (600 frames);
# - actor: a 2 m RIGID actor at 5 m/s: each checked frame's one invalidation box equals its swept bounds, it
#   re-renders at most 1.5x the static pages those bounds overlap, and the GPU's invalidation pass equals the
#   CPU models in every checked frame;
# - static_mode: a moving STATIC instance invalidates 0 static pages; hiding, showing, shadow on/off, layer
#   changes, leaving and re-entering the tree and freeing still do; a moved STATIC slab's hide clears its old
#   and new bounds after a material change, and switching a moved STATIC slab to RIGID re-adds it;
# - always: ALWAYS casters (a mesh and a NextGeo mesh) never draw into layer 0;
# - materials: VERTEX-animated foliage, DEPTH and VERTEX materials are dynamic under AUTO;
# - move_toggle: moving or toggling a static caster's shadow updates it (#222), read from the image;
# - budget: deferred_invalidation_budget 512 while the sun turns (sun LOD bias -1): pages are deferred, the
#   GPU counters equal the CPU model, and the cache converges once the sun stops;
# - fog_atlas: under VSM + volumetric fog (#215) a VSM spot light skips the atlas and updates VSM pages;
#   an explicit SHADOW_METHOD_ATLAS spot light still runs its atlas pass and redraws when a caster moves;
# - multimesh: MultiMesh slabs with a custom AABB (AUTO by instance transform, AUTO by buffer, RIGID) move their
#   shadows when moved, after settling and when moved back, read from the image;
# - range_swap: two slabs swapped by their visibility ranges swap their shadows, read from the image.
#
# Usage:
#   run_vsm_caching_check.sh [--work=<dir>] [--cases=<case,case,...>] [--frames=<n>] [--budget=<n>] \
#       -- <godot> <godot arguments without a scene>
#
# --frames sets the static case's frame count (default 600), --budget the budget case's pages (default 512)
# and --lod-bias the sun's resolution LOD bias in the budget case (default -1, finer than the project's, so
# that a 640x360 or 1920x1080 view invalidates more requested pages per frame than the budget; "project"
# keeps the project's, with which a 1920x1080 view invalidates fewer than 512).
# Prints one GODOTNEXT_VSM_CACHING line per run with its status and its "ERROR:", "SCRIPT ERROR:", "Invalid
# Metal usage", "Error detected in", "Validation Error" and leak line counts. Exits non-zero when a run
# fails, logs any of those lines or does not print exactly one GODOTNEXT_RESULT line.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/vsm/run_vsm_caching_check.sh -- \
#     ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal

set -uo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

work=""
cases="off,static,actor,static_mode,always,materials,move_toggle,budget,fog_atlas,multimesh,range_swap"
frames=600
budget=512
lod_bias=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--work=*) work="${1#--work=}" ;;
	--cases=*) cases="${1#--cases=}" ;;
	--frames=*) frames="${1#--frames=}" ;;
	--budget=*) budget="${1#--budget=}" ;;
	--lod-bias=*) lod_bias="${1#--lod-bias=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_vsm_caching_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vsm_caching_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"

status=0
IFS=',' read -r -a case_list <<<"$cases"
for case_name in "${case_list[@]}"; do
	log="$work/vsm_caching_$case_name.log"
	case $case_name in
	off) env -u GODOT_DEV_VSM "$@" res://tests/vsm/caching.tscn -- --expect-active=false >"$log" 2>&1 ;;
	*) GODOT_DEV_VSM=1 "$@" res://tests/vsm/caching.tscn -- --case="$case_name" --frames="$frames" --budget="$budget" ${lod_bias:+--lod-bias="$lod_bias"} >"$log" 2>&1 ;;
	esac
	rc=$?
	errors=$(grep -c '^ERROR' "$log")
	script_errors=$(grep -c 'SCRIPT ERROR' "$log")
	results=$(grep -c '^GODOTNEXT_RESULT' "$log")
	metal=$(grep -c 'Invalid Metal usage' "$log")
	detected=$(grep -c 'Error detected in' "$log")
	validation=$(grep -ci 'Validation Error' "$log")
	leaked=$(grep -cE 'was leaked|were leaked|leaked at exit|Leaked instance' "$log")
	echo "GODOTNEXT_VSM_CACHING case=$case_name exit=$rc errors=$errors script_errors=$script_errors invalid_metal=$metal error_detected=$detected validation=$validation leaked=$leaked results=$results log=$log"
	grep -m1 '^GODOTNEXT_RESULT' "$log" | cut -c1-400
	if [[ $rc -ne 0 || $errors -ne 0 || $script_errors -ne 0 || $metal -ne 0 || $detected -ne 0 || $validation -ne 0 || $leaked -ne 0 || $results -ne 1 ]]; then
		status=1
	fi
done
exit $status
