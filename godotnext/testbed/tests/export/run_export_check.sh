#!/usr/bin/env bash
# Exports the whole testbed and fails on any ERROR: line (#242, #244).
#
# Usage:
#   run_export_check.sh <godot> [preset ...]
#
# Imports godotnext/testbed, then for each preset (default: "NextGeo Linux" and "NextGeo Android")
# clears the export cache, runs --export-pack and counts the ERROR: lines in the log. The cache is
# cleared so every scene is converted again; a cached copy would hide conversion errors. Each pack
# is then checked with check_voxelgi_pack.gd, which loads the legacy VoxelGI scene from the pack and
# compares its GI data with the project's, and with check_lightmap_pack.gd, which does the same for
# the probe data of the LightmapGIData embedded in embedded_lightmap.tscn. Exits non-zero if any
# step fails or prints ERROR:.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/export/run_export_check.sh ./bin/godot.macos.editor.arm64

set -euo pipefail

if [[ $# -lt 1 ]]; then
	echo "run_export_check.sh: pass the Godot binary" >&2
	exit 2
fi
godot=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
presets=("$@")
if [[ ${#presets[@]} -eq 0 ]]; then
	presets=("NextGeo Linux" "NextGeo Android")
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)

work=$(mktemp -d)
cleanup() {
	rm -rf "$work"
}
trap cleanup EXIT

failures=0

# Runs Godot headless in the testbed, logs to $work/$1.log and checks the exit code and ERROR: lines.
run_step() {
	local name=$1
	shift
	local log="$work/$name.log"
	local status=0
	"$godot" --headless --path "$testbed_dir" "$@" >"$log" 2>&1 || status=$?
	local errors
	errors=$(grep -c 'ERROR:' "$log" || true)
	echo "GODOTNEXT_EXPORT_STEP {\"step\":\"$name\",\"exit\":$status,\"errors\":$errors}"
	if [[ "$status" -ne 0 || "$errors" -ne 0 ]]; then
		grep -A2 'ERROR:' "$log" >&2 || true
		tail -n 20 "$log" >&2
		failures=$((failures + 1))
	fi
}

run_step import --import

index=0
for preset in "${presets[@]}"; do
	index=$((index + 1))
	pack="$work/export_$index.pck"
	rm -rf "$testbed_dir/.godot/exported"
	run_step "export_$index" --export-pack "$preset" "$pack"
	if [[ -f "$pack" ]]; then
		run_step "voxelgi_$index" --script res://tests/export/check_voxelgi_pack.gd -- "$pack"
		grep '^GODOTNEXT_RESULT' "$work/voxelgi_$index.log" || true
		run_step "lightmap_$index" --script res://tests/export/check_lightmap_pack.gd -- "$pack"
		grep '^GODOTNEXT_RESULT' "$work/lightmap_$index.log" || true
	else
		echo "run_export_check.sh: \"$preset\" wrote no pack" >&2
		failures=$((failures + 1))
	fi
done

if [[ "$failures" -ne 0 ]]; then
	echo "run_export_check.sh: $failures step(s) failed" >&2
	exit 1
fi
echo "run_export_check.sh: ${#presets[@]} preset(s) exported with 0 ERROR: lines"
