#!/usr/bin/env bash
# Launch-to-launch HWRT safety net (#36, gate G1-6) on a ray-query device.
#
# Usage:
#   godotnext/testbed/tests/tier/run_hwrt_safety.sh <godot>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with NextGI on and a private
# user directory, then removes both the override and the latch file. Refuses to
# overwrite an existing override.cfg.
#
# Steps: Auto selects HWRT and logs the self-test; Off does not trace; a corrupted
# self-test falls back with SELFTEST_FAILED; an injected device loss writes the latch;
# the next launch falls back with HWRT_FAULTED; Force ignores the latch and keeps the
# file; a different driver key deletes it and selects HWRT again.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"
export MTL_DEBUG_LAYER=1
export MTL_SHADER_VALIDATION=1
export MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
unset GODOTNEXT_NEXTGI_SELFTEST_CORRUPT || true
unset GODOTNEXT_NEXTGI_SIMULATE_DEVICE_LOST || true

if [[ $# -ne 1 ]]; then
	echo "run_hwrt_safety.sh: pass the Godot binary" >&2
	exit 2
fi

godot=$1
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"
scene="res://tests/tier/hwrt_fault_check.tscn"
user_name="godotnext-issue36-hwrt-fault"
fault_file=""

if [[ -e "$override" ]]; then
	echo "run_hwrt_safety.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

py=/opt/homebrew/bin/python3
if [[ ! -x "$py" ]]; then
	py=python3
fi

cleanup() {
	rm -f "$override"
	if [[ -n "$fault_file" ]]; then
		rm -f "$fault_file"
	fi
	rm -f "$HOME/Library/Application Support/$user_name/nextgi_hwrt_fault.cfg"
}
trap cleanup EXIT

write_override() {
	local hwrt_value=$1
	cat >"$override" <<EOF
[application]

config/use_custom_user_dir=true
config/custom_user_dir_name="$user_name"

[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
EOF
}

metal_reports() {
	grep -E 'Invalid Metal usage|Error detected in|^ERROR:' "$1" || true
}

nextgi_lines() {
	grep -c '^NextGI:' "$1" || true
}

run_scene() {
	local log=$1
	shift
	set +e
	"$@" "$godot" --path "$testbed_dir" --rendering-driver metal "$scene" >"$log" 2>&1
	local status=$?
	set -e
	if [[ -z "$fault_file" ]]; then
		fault_file=$(grep '^GODOTNEXT_FAULT_DIR ' "$log" | head -n 1 | sed 's/^GODOTNEXT_FAULT_DIR //' || true)
	fi
	# A non-zero return is checked by the caller. `if` ignores set -e, so an
	# injected abort does not end the script.
	if [[ "$status" -ne 0 ]]; then
		return "$status"
	fi
}

require_clean_log() {
	local log=$1
	local reports
	reports=$(metal_reports "$log")
	if [[ -n "$reports" ]]; then
		echo "run_hwrt_safety.sh: Metal validation or ERROR lines:" >&2
		echo "$reports" >&2
		exit 1
	fi
}

result_field() {
	local log=$1
	local expr=$2
	"$py" - "$log" "$expr" <<'PY'
import json, sys
log_path, expr = sys.argv[1], sys.argv[2]
payload = None
for line in open(log_path, encoding="utf-8", errors="replace"):
    if line.startswith("GODOTNEXT_RESULT "):
        payload = json.loads(line[len("GODOTNEXT_RESULT "):])
if payload is None:
    sys.exit("missing GODOTNEXT_RESULT")
value = payload
for part in expr.split("."):
    value = value[part]
print(value)
PY
}

expect_reason() {
	local log=$1
	local reason=$2
	local got
	got=$(result_field "$log" "tier_info.reason")
	if [[ "$got" != "$reason" ]]; then
		echo "run_hwrt_safety.sh: reason is $got, expected $reason" >&2
		grep -E '^NextGI:|^ERROR:|self-test|Invalid Metal|Error detected in|GODOTNEXT_' "$log" >&2 || true
		exit 1
	fi
}

run_scene_ok() {
	local log=$1
	shift
	if ! run_scene "$log" "$@"; then
		echo "run_hwrt_safety.sh: Godot failed" >&2
		grep -E '^NextGI:|^ERROR:|self-test|Invalid Metal|Error detected in|GODOTNEXT_' "$log" >&2 || true
		exit 1
	fi
}

echo "run_hwrt_safety.sh: Auto HWRT"
write_override 0
log=$(mktemp)
run_scene_ok "$log"
require_clean_log "$log"
expect_reason "$log" "NONE"
if ! grep -q 'selftest=' "$log"; then
	echo "run_hwrt_safety.sh: Auto HWRT did not log the self-test time" >&2
	exit 1
fi
lines=$(nextgi_lines "$log")
if [[ "$lines" -ne 1 ]]; then
	echo "run_hwrt_safety.sh: expected 1 NextGI: line, found $lines" >&2
	exit 1
fi
if [[ "$(result_field "$log" "latch_exists")" != "False" ]]; then
	echo "run_hwrt_safety.sh: a clean HWRT launch wrote the latch file" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: hardware_ray_tracing Off"
write_override 1
log=$(mktemp)
run_scene_ok "$log"
require_clean_log "$log"
expect_reason "$log" "SETTING_OFF"
if grep -q 'selftest=' "$log"; then
	echo "run_hwrt_safety.sh: Off ran the self-test" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: injected self-test mismatch"
write_override 0
log=$(mktemp)
run_scene_ok "$log" env GODOTNEXT_NEXTGI_SELFTEST_CORRUPT=1
require_clean_log "$log"
expect_reason "$log" "SELFTEST_FAILED"
if ! grep -q 'selftest=' "$log"; then
	echo "run_hwrt_safety.sh: SELFTEST_FAILED did not log the self-test time" >&2
	exit 1
fi
if [[ "$(result_field "$log" "latch_exists")" != "False" ]]; then
	echo "run_hwrt_safety.sh: a self-test mismatch wrote the latch file" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: injected device loss"
write_override 0
log=$(mktemp)
if run_scene "$log" env GODOTNEXT_NEXTGI_SIMULATE_DEVICE_LOST=1; then
	echo "run_hwrt_safety.sh: injected device loss did not abort" >&2
	exit 1
fi
if ! grep -q 'NextGI simulated an HWRT device loss' "$log"; then
	echo "run_hwrt_safety.sh: crash log has no simulated device-loss message" >&2
	exit 1
fi
shader_reports=$(grep -E 'Invalid Metal usage|Error detected in' "$log" || true)
if [[ -n "$shader_reports" ]]; then
	echo "run_hwrt_safety.sh: device-loss run reported Metal shader validation errors" >&2
	echo "$shader_reports" >&2
	exit 1
fi
if [[ -z "$fault_file" || ! -f "$fault_file" ]]; then
	echo "run_hwrt_safety.sh: device loss did not write the latch file" >&2
	exit 1
fi
if ! grep -q '^driver_key=' "$fault_file"; then
	echo "run_hwrt_safety.sh: latch file has no driver_key" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: next launch is HWRT_FAULTED"
write_override 0
log=$(mktemp)
run_scene_ok "$log"
require_clean_log "$log"
expect_reason "$log" "HWRT_FAULTED"
if grep -q 'selftest=' "$log"; then
	echo "run_hwrt_safety.sh: a latched driver still ran the self-test" >&2
	exit 1
fi
if [[ "$(result_field "$log" "latch_exists")" != "True" ]]; then
	echo "run_hwrt_safety.sh: HWRT_FAULTED deleted the latch file" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: Force ignores the latch and keeps the file"
write_override 2
log=$(mktemp)
run_scene_ok "$log"
require_clean_log "$log"
expect_reason "$log" "NONE"
if ! grep -q 'selftest=' "$log"; then
	echo "run_hwrt_safety.sh: Force did not log the self-test time" >&2
	exit 1
fi
if [[ "$(result_field "$log" "latch_exists")" != "True" ]]; then
	echo "run_hwrt_safety.sh: Force removed the latch file" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: a different driver key clears the latch"
"$py" - "$fault_file" <<'PY'
import sys
path = sys.argv[1]
lines = open(path, encoding="utf-8").read().splitlines()
out = []
for line in lines:
    if line.startswith("driver_key="):
        out.append("driver_key=replaced-by-safety-check")
    else:
        out.append(line)
open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
PY
write_override 0
log=$(mktemp)
run_scene_ok "$log"
require_clean_log "$log"
expect_reason "$log" "NONE"
if ! grep -q 'selftest=' "$log"; then
	echo "run_hwrt_safety.sh: the cleared latch did not log the self-test time" >&2
	exit 1
fi
if [[ -f "$fault_file" ]]; then
	echo "run_hwrt_safety.sh: a different driver key left the latch file in place" >&2
	exit 1
fi
if [[ "$(result_field "$log" "latch_exists")" != "False" ]]; then
	echo "run_hwrt_safety.sh: latch_exists stayed true after the driver key changed" >&2
	exit 1
fi
rm -f "$log"

echo "run_hwrt_safety.sh: passed"
