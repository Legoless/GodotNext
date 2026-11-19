#!/usr/bin/env bash
# Runs res://tests/vsm/product_surface.tscn (#221): the Virtual Shadow Map product surface on ShadowGallery,
# plus the PV smoke scene. One fresh process per run, each under `timeout` (when the system has one):
# - off: no setting and GODOT_DEV_VSM unset: the tier is available but nothing is active, the stats stay 0,
#   no fallback line is printed and each VSM debug draw leaves the image unchanged.
# - setting: rendering/lights_and_shadows/shadow_method = Virtual: the stats and monitors count pages, each
#   of the six VSM debug draws passes its color checks, VSM_OVERDRAW shows no draws on a view without
#   casters, a mixed light set (one light ATLAS, one VIRTUAL; the atlas light then PROJECT, RAY_TRACED,
#   DISTANCE_FIELD and RAY_TRACED again, each resolving to Virtual) passes the non-Nanite raster check, and
#   every quality preset's pool equals budget V6 within 1 MiB, with the stats counting and a raster check
#   passing on it.
# - dev: GODOT_DEV_VSM=1 with no setting: the dev switch still stands for the Virtual setting.
# - mobile: the setting at Virtual with --rendering-method mobile: VSM is inactive, the tier reason is
#   UNSUPPORTED_RENDERER and the log holds exactly one fallback line. (The configuration warning that names
#   the reason on the shadowed lights is not callable from scripts; a [VSM][SceneTree] doctest checks it.)
# - flythrough: product_surface.tscn with the setting at Virtual on the PV smoke scene (VSM Flythrough).
# - smoke: the PV smoke scene itself (300 frames) with the setting at Virtual.
# - pairmax: the setting run's debug draws with GODOT_DEV_VSM_PAIR_CAPACITY=4194304, the largest non-Nanite
#   pair list: VSM_OVERDRAW's pair pass stays within the 65,535-group dispatch limit (D3D12), so no error.
# - headless: the setting at Virtual with --headless: the dummy renderer prints no fallback line and no
#   shadow-method warning.
#
# Usage:
#   run_vsm_product_surface_check.sh [--work=<dir>] [--out=<dir>] [--user-dir=<name>] [--timeout=<minutes>]
#       [--runs=<run,run,...>] -- <godot> <godot arguments without a scene>
#
# Every run writes a temporary override.cfg in the testbed root with the private user dir (default
# gn_vsm_product_surface) and the run's settings; it refuses to run when an override.cfg exists and removes
# its own afterwards. --out saves each debug draw as a PNG under <dir>/<run>/. Prints one
# GODOTNEXT_VSM_PRODUCT_SURFACE line per run with its status and its "ERROR:", "SCRIPT ERROR:",
# "Invalid Metal usage", "Error detected in", "Validation Error", leak and fallback line counts. Exits
# non-zero when a run fails, times out, logs any of those lines, prints a fallback line it should not (or
# not exactly one on mobile), (except smoke) does not print exactly one GODOTNEXT_RESULT line, or (pairmax)
# does not print the line that the pair capacity is set.

set -uo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

work=""
out=""
user_dir="gn_vsm_product_surface"
minutes=10
runs="off,setting,dev,mobile,flythrough,smoke,pairmax,headless"
while [[ $# -gt 0 ]]; do
	case "$1" in
	--work=*) work="${1#--work=}" ;;
	--out=*) out="${1#--out=}" ;;
	--user-dir=*) user_dir="${1#--user-dir=}" ;;
	--timeout=*) minutes="${1#--timeout=}" ;;
	--runs=*) runs="${1#--runs=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_vsm_product_surface_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_vsm_product_surface_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -z "$work" ]]; then
	work=$(mktemp -d)
fi
mkdir -p "$work"
testbed=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
override="$testbed/override.cfg"
limit=()
if command -v timeout >/dev/null 2>&1; then
	limit=(timeout -k 30 "${minutes}m")
fi

# write_override <shadow method or empty>
write_override() {
	if [[ -e "$override" ]]; then
		echo "run_vsm_product_surface_check.sh: $override exists; not overwriting it" >&2
		return 1
	fi
	printf '[application]\n\nconfig/use_custom_user_dir=true\nconfig/custom_user_dir_name="%s"\n' "$user_dir" >"$override"
	if [[ -n "$1" ]]; then
		printf '\n[rendering]\n\nlights_and_shadows/shadow_method=%s\n' "$1" >>"$override"
	fi
}

status=0
IFS=',' read -r -a run_list <<<"$runs"
for run_kind in "${run_list[@]}"; do
	log="$work/vsm_product_surface_$run_kind.log"
	out_args=()
	if [[ -n "$out" ]]; then
		out_args=("--out=$out/$run_kind")
	fi
	case $run_kind in
	off | dev) method="" ;;
	setting | mobile | flythrough | smoke | pairmax | headless) method=1 ;;
	*)
		echo "run_vsm_product_surface_check.sh: unknown run '$run_kind'" >&2
		status=1
		continue
		;;
	esac
	if ! write_override "$method"; then
		status=1
		continue
	fi
	case $run_kind in
	off) env -u GODOT_DEV_VSM ${limit[@]+"${limit[@]}"} "$@" res://tests/vsm/product_surface.tscn -- --expect-active=false --expect-reason=NONE ${out_args[@]+"${out_args[@]}"} >"$log" 2>&1 ;;
	setting) env -u GODOT_DEV_VSM ${limit[@]+"${limit[@]}"} "$@" res://tests/vsm/product_surface.tscn -- --expect-active=true --expect-reason=NONE --mixed=true --presets=true ${out_args[@]+"${out_args[@]}"} >"$log" 2>&1 ;;
	dev) env GODOT_DEV_VSM=1 ${limit[@]+"${limit[@]}"} "$@" res://tests/vsm/product_surface.tscn -- --expect-active=true --expect-reason=NONE ${out_args[@]+"${out_args[@]}"} >"$log" 2>&1 ;;
	mobile) env -u GODOT_DEV_VSM ${limit[@]+"${limit[@]}"} "$@" --rendering-method mobile res://tests/vsm/product_surface.tscn -- --expect-active=false --expect-reason=UNSUPPORTED_RENDERER >"$log" 2>&1 ;;
	flythrough) env -u GODOT_DEV_VSM ${limit[@]+"${limit[@]}"} "$@" res://tests/vsm/product_surface.tscn -- --expect-active=true --expect-reason=NONE --scene=res://scenes/vsm_flythrough/vsm_flythrough.tscn ${out_args[@]+"${out_args[@]}"} >"$log" 2>&1 ;;
	smoke) env -u GODOT_DEV_VSM ${limit[@]+"${limit[@]}"} "$@" --fixed-fps 60 --quit-after 320 res://scenes/vsm_flythrough/vsm_flythrough.tscn -- --frames=300 >"$log" 2>&1 ;;
	pairmax) env -u GODOT_DEV_VSM GODOT_DEV_VSM_PAIR_CAPACITY=4194304 ${limit[@]+"${limit[@]}"} "$@" res://tests/vsm/product_surface.tscn -- --expect-active=true --expect-reason=NONE ${out_args[@]+"${out_args[@]}"} >"$log" 2>&1 ;;
	headless) env -u GODOT_DEV_VSM ${limit[@]+"${limit[@]}"} "$@" --headless res://tests/vsm/product_surface.tscn -- --expect-headless=true --warmup=10 >"$log" 2>&1 ;;
	esac
	rc=$?
	rm -f "$override"
	errors=$(grep -c '^ERROR' "$log")
	script_errors=$(grep -c 'SCRIPT ERROR' "$log")
	results=$(grep -c '^GODOTNEXT_RESULT' "$log")
	metal=$(grep -c 'Invalid Metal usage' "$log")
	detected=$(grep -c 'Error detected in' "$log")
	validation=$(grep -ci 'Validation Error' "$log")
	leaked=$(grep -cE 'was leaked|were leaked|leaked at exit|Leaked instance' "$log")
	fallback=$(grep -c 'VSM: virtual shadow maps are not available' "$log")
	pair_line=$(grep -c 'GODOT_DEV_VSM_PAIR_CAPACITY is set: the non-Nanite pair list starts at 4194304 pairs' "$log")
	echo "GODOTNEXT_VSM_PRODUCT_SURFACE run=$run_kind exit=$rc errors=$errors script_errors=$script_errors invalid_metal=$metal error_detected=$detected validation=$validation leaked=$leaked fallback_lines=$fallback pair_capacity_line=$pair_line results=$results log=$log"
	grep -m1 '^GODOTNEXT_RESULT' "$log" | cut -c1-600
	expected_results=1
	expected_fallback=0
	if [[ $run_kind == smoke ]]; then
		expected_results=0
	elif [[ $run_kind == mobile ]]; then
		expected_fallback=1
	fi
	if [[ $rc -ne 0 || $errors -ne 0 || $script_errors -ne 0 || $metal -ne 0 || $detected -ne 0 || $validation -ne 0 || $leaked -ne 0 || $results -ne $expected_results || $fallback -ne $expected_fallback ]]; then
		status=1
	fi
	if [[ $run_kind == pairmax && $pair_line -ne 1 ]]; then
		status=1
	fi
done
exit $status
