#!/usr/bin/env bash
# Runs the #295 AVBOIT draw-order fixture (tests/avboit/avboit_check.tscn) and checks its result,
# the engine's AVBOIT lines and the validation output.
# Usage: run_avboit.sh -- <godot> <godot arguments, the scene included>
set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_avboit.sh: pass the Godot command after --" >&2
	exit 2
fi
shift

log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_avboit.sh: the AVBOIT fixture did not pass" >&2
	exit 1
fi
for line in 'AVBOIT: on (EXPERIMENTAL)' 'AVBOIT: sorted (reason MSAA)' 'AVBOIT: sorted (reason SETTING)'; do
	if ! grep -qF "$line" "$log"; then
		echo "run_avboit.sh: the engine did not print \"$line\"" >&2
		exit 1
	fi
done
if grep -Eq 'ERROR:|Validation Error|Invalid Metal usage|Error detected in' "$log"; then
	echo "run_avboit.sh: engine or validation errors were reported" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
echo "AVBOIT draw-order fixture passed. FLIP and cost are measured in the final test phase."
