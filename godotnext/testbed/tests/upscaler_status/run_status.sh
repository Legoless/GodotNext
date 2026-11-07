#!/usr/bin/env bash
# Runs the #103 status scene with a DLSS request and no registered module.
set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_status.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ -e "$override" ]]; then
	echo "run_status.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT
cat >"$override" <<'EOF'
[rendering]

scaling_3d/mode=6
EOF

set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e
result=$(grep -m1 '^GODOTNEXT_RESULT .*"pass":true' "$log" || true)
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_status.sh: the upscaler status check did not pass" >&2
	exit 1
fi
if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in' "$log"; then
	echo "run_status.sh: engine or Metal validation errors were reported" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
