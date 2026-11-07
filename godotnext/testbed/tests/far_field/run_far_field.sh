#!/usr/bin/env bash
# Runs the #126 valley with HWRT enabled before renderer initialization.
# Usage: run_far_field.sh -- <godot> <godot arguments> [-- --off|--occlusion]
set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_far_field.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ -e "$override" ]]; then
	echo "run_far_field.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/quality=2
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

result=$(grep -E '^FAR_FIELD_(SUMMARY|OFF) pass=' "$log" || true)
if [[ "$result" != *"pass=true"* ]]; then
	echo "run_far_field.sh: the far-field scene did not pass" >&2
	exit 1
fi
if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in' "$log"; then
	echo "run_far_field.sh: engine or Metal validation errors were reported" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
echo "Far-field quality and timing measurements are deferred to the final test phase."
