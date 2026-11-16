#!/usr/bin/env bash
# Runs the #203 frame-generation scene and checks its log: a passing result and no engine errors.
# When the run walked through the fallbacks (--fg-fallbacks), each fallback reason must appear in
# exactly one log line, NOT_MAIN_WINDOW in none (a game's main window never reaches it), and the one
# allowed error is the PROVIDER_ALREADY_ACTIVE line. Request the sample provider with an override.cfg:
#   [rendering]
#   frame_generation/provider="SamplePassthrough"
# Usage: run_frame_generation.sh -- <godot> --path godotnext/testbed res://tests/frame_generation/frame_generation.tscn [-- --fg-fallbacks]
set -euo pipefail

if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
	echo "run_frame_generation.sh: pass the Godot command after --" >&2
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
	echo "run_frame_generation.sh: the frame-generation check did not pass" >&2
	fail=1
fi
# Engine lines only: the scene's own lines (FG_*, GODOTNEXT_RESULT) never name a reason.
engine_lines() { grep -v -e '^FG_' -e '^GODOTNEXT_RESULT' "$log" || true; }
allowed_errors=0
if grep -q '^FG_FALLBACK ' "$log"; then
	for token in API_UNSUPPORTED FG_GPU_UNSUPPORTED NOT_NVIDIA_RTX MFG_REQUIRES_RTX50 PROVIDER_ALREADY_ACTIVE PROVIDER_UNAVAILABLE; do
		count=$(engine_lines | grep -c -- "$token" || true)
		if [[ "$count" -ne 1 ]]; then
			echo "run_frame_generation.sh: expected 1 log line naming $token, saw $count" >&2
			fail=1
		fi
	done
	allowed_errors=1
fi
not_main=$(engine_lines | grep -c 'NOT_MAIN_WINDOW' || true)
if [[ "$not_main" -ne 0 ]]; then
	echo "run_frame_generation.sh: expected no NOT_MAIN_WINDOW line in a game, saw $not_main" >&2
	fail=1
fi
errors=$(engine_lines | grep -c '^ERROR:' || true)
already=$(engine_lines | grep '^ERROR:' | grep -c 'PROVIDER_ALREADY_ACTIVE' || true)
if [[ "$errors" -ne "$already" || "$already" -gt "$allowed_errors" ]]; then
	echo "run_frame_generation.sh: unexpected engine errors ($errors, of which $already PROVIDER_ALREADY_ACTIVE)" >&2
	fail=1
fi
if grep -Eq 'Invalid Metal usage|Validation Error|Error detected in|leaked' "$log"; then
	echo "run_frame_generation.sh: validation reports or leaks were found" >&2
	fail=1
fi
if [[ "$fail" -ne 0 ]]; then
	exit 1
fi
exit "$status"
