#!/usr/bin/env bash
# Runs res://tests/tier/tier_check.tscn with NextGI enabled for the run (#15, gate G0-2).
#
# Usage:
#   run_tier_check.sh [--nextgi=on|off] [--hwrt=auto|off|force] -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with the nextgi settings, runs Godot and
# removes the file again. It refuses to overwrite an existing override.cfg. On top of the scene's
# own checks it counts the lines that start with "NextGI:" in the log: none with --nextgi=off, and
# with --nextgi=on exactly 1, the tier line. Plan §3.1 prints the tier and its reason once, and
# logs the auto quality choice with its reason, so the startup line carries all of it:
#   NextGI: tier=<TIER> reason=<REASON> quality=<QUALITY> (auto: <benchmark reason>)
# HWRT and SWRT alike. Other NextGI output is worded without the "NextGI:" prefix. A later tier
# change (runtime switch, AS_MEMORY, SWRT_REDUCED) prints a new line; the check scene makes none.
# Exits non-zero if Godot fails, the count is wrong or the line is not the tier line.
#
# Examples on the Apple M5 Max, which selects HWRT under Auto and the software tier under Off:
#   godotnext/testbed/tests/tier/run_tier_check.sh -- ./bin/godot.macos.editor.arm64 \
#     --path godotnext/testbed --rendering-driver metal res://tests/tier/tier_check.tscn \
#     -- --expect=HWRT
#   godotnext/testbed/tests/tier/run_tier_check.sh --hwrt=off -- ./bin/godot.macos.editor.arm64 \
#     --path godotnext/testbed --rendering-driver metal res://tests/tier/tier_check.tscn \
#     -- --expect=SWRT

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

nextgi=on
hwrt=auto
while [[ $# -gt 0 ]]; do
	case "$1" in
	--nextgi=on | --nextgi=off) nextgi="${1#--nextgi=}" ;;
	--hwrt=auto | --hwrt=off | --hwrt=force) hwrt="${1#--hwrt=}" ;;
	--)
		shift
		break
		;;
	*)
		echo "run_tier_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_tier_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_tier_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

case "$hwrt" in
auto) hwrt_value=0 ;;
off) hwrt_value=1 ;;
force) hwrt_value=2 ;;
esac
enabled_value=true
expected_lines=1
if [[ "$nextgi" == off ]]; then
	enabled_value=false
	expected_lines=0
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<EOF
[rendering]

global_illumination/nextgi/enabled=$enabled_value
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
EOF

# pipefail makes the pipeline's status Godot's when Godot fails.
set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

lines=$(grep -c '^NextGI:' "$log" || true)
tier_lines=$(grep -c '^NextGI: tier=' "$log" || true)
echo "GODOTNEXT_TIER_LINES {\"nextgi\":\"$nextgi\",\"hwrt\":\"$hwrt\",\"lines\":$lines,\"tier_lines\":$tier_lines,\"expected\":$expected_lines}"
if [[ "$lines" -ne "$expected_lines" ]]; then
	echo "run_tier_check.sh: expected $expected_lines NextGI: line(s), found $lines" >&2
	grep '^NextGI:' "$log" >&2 || true
	exit 1
fi
if [[ "$tier_lines" -ne "$expected_lines" ]]; then
	echo "run_tier_check.sh: the NextGI: line is not the tier line (NextGI: tier=...)" >&2
	grep '^NextGI:' "$log" >&2 || true
	exit 1
fi
exit "$status"
