#!/usr/bin/env bash
# Runs #133 with High probes, Medium WRC-only, sealed-room or fog checks.
# Usage: run_volume.sh [--medium|--sealed|--fog] -- <godot> <godot arguments>
set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
quality=2
mode_arg=""
if [[ "${1:-}" == "--medium" ]]; then
	quality=1
	mode_arg="$1"
	shift
elif [[ "${1:-}" == "--sealed" || "${1:-}" == "--fog" ]]; then
	mode_arg="$1"
	shift
fi
if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_volume.sh: pass a mode and the Godot command after --" >&2
	exit 2
fi
shift
if [[ -n "$mode_arg" ]]; then
	set -- "$@" -- "$mode_arg"
fi
if [[ -e "$override" ]]; then
	echo "run_volume.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/quality=$quality
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e
result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_volume.sh: the translucency volume check did not pass" >&2
	exit 1
fi
if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in' "$log"; then
	echo "run_volume.sh: engine or Metal validation errors were reported" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
echo "Translucency quality and timing measurements are deferred to the final test phase."
