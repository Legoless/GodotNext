#!/usr/bin/env bash
# Runs the #192 custom upscaler scene (GT-1 subset) and checks its log:
# a passing result, exactly one unavailable and one scale-clamp warning, and
# no engine errors, leaks or Metal validation reports.
set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_custom_upscaler.sh: pass the Godot command after --" >&2
	exit 2
fi
shift

log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e
fail=0
if ! grep -q '^GODOTNEXT_RESULT .*"pass":true' "$log"; then
	echo "run_custom_upscaler.sh: the custom upscaler check did not pass" >&2
	fail=1
fi
unavailable=$(grep -c 'Custom 3D temporal upscaler is unavailable' "$log" || true)
if [[ "$unavailable" -ne 1 ]]; then
	echo "run_custom_upscaler.sh: expected 1 unavailable warning, saw $unavailable" >&2
	fail=1
fi
clamped=$(grep -c "Custom temporal upscaler 'SampleCopy' scale must be between" "$log" || true)
if [[ "$clamped" -ne 1 ]]; then
	echo "run_custom_upscaler.sh: expected 1 scale clamp warning, saw $clamped" >&2
	fail=1
fi
if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in|leaked' "$log"; then
	echo "run_custom_upscaler.sh: engine errors, leaks or Metal validation reports were found" >&2
	fail=1
fi
if [[ "$fail" -ne 0 ]]; then
	exit 1
fi
exit "$status"
