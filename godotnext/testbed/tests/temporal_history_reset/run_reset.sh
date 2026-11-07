#!/usr/bin/env bash
# Runs the #104 one-shot reset check on Metal with API and shader validation.
set -euo pipefail

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_reset.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
log_dir=$(mktemp -d)
trap 'rm -rf "$log_dir"' EXIT

for mode in fsr2 metalfx copy copy_second_viewport; do
	case "$mode" in
		fsr2) expected=1 ;;
		metalfx) expected=2 ;;
		*) expected=3 ;;
	esac
	expected_resets=2
	if [[ "$mode" == "copy_second_viewport" ]]; then
		expected_resets=4
	fi
	log="$log_dir/$mode.log"
	set +e
	GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 GODOT_DEV_COPY_UPSCALER=1 \
		"$@" --verbose --rendering-driver metal --path godotnext/testbed \
		res://tests/temporal_history_reset/temporal_history_reset.tscn -- --reset-backend="$mode" 2>&1 | tee "$log"
	status=${PIPESTATUS[0]}
	set -e
	count=$(grep -c "GODOTNEXT_TEMPORAL_RESET mode=$expected" "$log" || true)
	if [[ "$count" -ne "$expected_resets" ]]; then
		echo "run_reset.sh: $mode expected $expected_resets one-shot resets, found $count" >&2
		exit 1
	fi
	if ! grep -q '^GODOTNEXT_RESULT .*"pass":true' "$log"; then
		echo "run_reset.sh: $mode did not pass its rendered check" >&2
		exit 1
	fi
	if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in' "$log"; then
		echo "run_reset.sh: $mode reported engine or Metal validation errors" >&2
		exit 1
	fi
	if [[ "$status" -ne 0 ]]; then
		exit "$status"
	fi
done
