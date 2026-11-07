#!/usr/bin/env bash
# Runs #132 reflection behavior checks with the selected NextGI mode.
# Usage: run_reflection_behaviors.sh [--rough|--reflections-only|--max-roughness] -- <godot> <godot arguments>
set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
mode=0
reflections=true
if [[ "${1:-}" == "--rough" ]]; then
	reflections=false
	shift
elif [[ "${1:-}" == "--max-roughness" ]]; then
	shift
elif [[ "${1:-}" == "--reflections-only" ]]; then
	mode=1
	shift
fi
if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_reflection_behaviors.sh: pass a mode and the Godot command after --" >&2
	exit 2
fi
shift
if [[ -e "$override" ]]; then
	echo "run_reflection_behaviors.sh: $override already exists; not overwriting it" >&2
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
global_illumination/nextgi/mode=$mode
global_illumination/nextgi/reflections=$reflections
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e
result=$(grep -m1 '^NEXTGI_MODE_RESULT pass=' "$log" || true)
if [[ "$result" != *"pass=true"* ]]; then
	echo "run_reflection_behaviors.sh: the reflection behavior check did not pass" >&2
	exit 1
fi
if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in' "$log"; then
	echo "run_reflection_behaviors.sh: engine or Metal validation errors were reported" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
echo "Reflection quality and timing measurements are deferred to the final test phase."
