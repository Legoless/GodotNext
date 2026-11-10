#!/usr/bin/env bash
# Runs the #188 voxel lighting checks on the hardware ray tracing tier:
#   res://tests/voxel_lighting/voxel_slot_check.tscn (GPU texture space, direction slots, D6,
#   readback gating, refill after a tier switch) at quality High,
#   res://tests/voxel_lighting/voxel_thin_wall_check.tscn (no light through 0.2 m walls whose
#   inside voxel centers lie within the mesh SDF's eps of a face) at quality High, and
#   res://tests/voxel_lighting/voxel_lighting_check.tscn (kitbash stores hits and covers some)
#   at quality Auto.
# The slot and thin-wall checks pin High: at Medium (Lumen Lite) the SRC relight allocates
# radiance pages in a 4096 space while the Lite radiance atlas is 2048, so pages can sit outside
# the atlas and read black for every consumer. That is a Lite defect, not a voxel lighting one.
#
# Usage:
#   run_voxel_lighting_check.sh -- <godot> <godot arguments without a scene>
#
# Refuses to overwrite an existing override.cfg. Prints Metal validation counts per run and
# exits non-zero when a check fails or a run logs a validation error.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--)
		shift
		break
		;;
	*)
		echo "run_voxel_lighting_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_voxel_lighting_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_voxel_lighting_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

failed=0
for scene in res://tests/voxel_lighting/voxel_slot_check.tscn res://tests/voxel_lighting/voxel_thin_wall_check.tscn res://tests/voxel_lighting/voxel_lighting_check.tscn; do
	quality=0
	if [[ "$scene" == *voxel_slot_check* || "$scene" == *voxel_thin_wall_check* ]]; then
		quality=2
	fi
	cat >"$override" <<CFG
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=0
global_illumination/nextgi/quality=$quality
CFG
	set +e
	"$@" "$scene" 2>&1 | tee "$log"
	status=${PIPESTATUS[0]}
	set -e
	errors=$(grep -c 'ERROR:' "$log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
	metal_shader=$(grep -c 'Error detected in' "$log" || true)
	echo "GODOTNEXT_VOXEL_LIGHTING_LOG {\"scene\":\"$scene\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
	if [[ $status -ne 0 || $errors -ne 0 || $metal_usage -ne 0 || $metal_shader -ne 0 ]]; then
		failed=1
	fi
done
exit "$failed"
