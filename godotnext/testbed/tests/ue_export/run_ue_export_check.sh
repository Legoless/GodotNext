#!/usr/bin/env bash
# Checks the UE comparison exporter (#235) on the overlap-heavy kitbash.
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
	"$godot" --audio-driver Dummy --path "$testbed_dir" "$@" --script res://tools/ue_export/ue_export.gd -- \
		"${export_args[@]}" --out="$work/$run" >"$work/$run.log" 2>&1 || status=$?
	report "export_$run" "$status" 0 "$work/$run.log"
	grep '^GODOTNEXT_UE_EXPORT ' "$work/$run.log" || true
done

status=0
"$godot" --headless --audio-driver Dummy --path "$testbed_dir" --script res://tests/ue_export/check_ue_export.gd -- \
	"$work/a" "$work/b" >"$work/check.log" 2>&1 || status=$?
report check "$status" 0 "$work/check.log"
grep '^GODOTNEXT_RESULT' "$work/check.log" || true

if [[ "$failures" -ne 0 ]]; then
	echo "run_ue_export_check.sh: $failures step(s) failed" >&2
	exit 1
fi
echo "run_ue_export_check.sh: all steps passed"
