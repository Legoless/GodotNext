#!/usr/bin/env bash
# Path-traced scene reference capture (#263).
# Writes the testbed's override.cfg with NextGI on (hardware ray tracing Auto), runs one
# harness capture with --reference=pt, counts validation lines and removes override.cfg.
# Usage: reference_pt.sh [--fail-on-validation] -- <godot> <godot arguments> -- <harness arguments>
#
# References are not tracked (plan §13.3, v3.8.1): write each to
# captures/references/<driver>/<scene>/<camera>/ in the testbed (git ignores captures/), under the
# same names on every backend. Example, Color Box Room A seen from CovCam0 on Metal:
#   reference_pt.sh -- ./bin/godot.macos.editor.arm64 --path godotnext/testbed \
#     --rendering-driver metal -- --mode=capture --scene=res://scenes/color_box_room/cbr_a.tscn \
#     --camera=CovCam0 --out=godotnext/testbed/captures/references/metal/cbr_a/CovCam0 \
#     --seed=1 --reference=pt --spp=4096
# The Q1 tool checks a folder alone with:
#   PYTHONPATH=godotnext/testbed/harness/metrics python3 -m godotnext_metrics q1 --ref <folder>

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
fail_on_validation=0

while [[ $# -gt 0 ]]; do
	case "$1" in
	--fail-on-validation)
		fail_on_validation=1
		;;
	--)
		shift
		break
		;;
	*)
		echo "reference_pt.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "reference_pt.sh: pass the Godot command after --" >&2
	exit 2
fi
has_reference=0
for argument in "$@"; do
	if [[ "$argument" == "--reference=pt" ]]; then
		has_reference=1
	fi
done
if [[ $has_reference -eq 0 ]]; then
	echo "reference_pt.sh: the harness arguments need --reference=pt" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "reference_pt.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<EOF
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=0
global_illumination/nextgi/hardware_ray_tracing.macos=0
EOF

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
vulkan=$(grep -c 'Validation Error' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_REFERENCE_LOG {\"errors\":$errors,\"validation_error\":$vulkan,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ $status -eq 0 && $fail_on_validation -eq 1 && $((errors + vulkan + metal_usage + metal_shader)) -gt 0 ]]; then
	exit 1
fi
exit "$status"
