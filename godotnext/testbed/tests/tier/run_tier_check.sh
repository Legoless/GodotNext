#!/usr/bin/env bash
# Runs res://tests/tier/tier_check.tscn with NextGI enabled for the run (#15, gate G0-2).
#
# Usage:
#   run_tier_check.sh [--nextgi=on|off] [--hwrt=auto|off|force] -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with the nextgi settings, runs Godot and
# removes the file again. It refuses to overwrite an existing override.cfg. On top of the scene's
# own checks it counts the "NextGI:" tier lines in the log: exactly 1 with --nextgi=on, none with
# --nextgi=off. Exits non-zero if Godot fails or the count is wrong.
#
# Example on the Apple M5 Max (Metal has no ray query until #48 and #47):
#   godotnext/testbed/tests/tier/run_tier_check.sh -- ./bin/godot.macos.editor.arm64 \
#     --path godotnext/testbed --rendering-driver metal res://tests/tier/tier_check.tscn \
#     -- --expect=NO_RAY_QUERY

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
echo "GODOTNEXT_TIER_LINES {\"nextgi\":\"$nextgi\",\"hwrt\":\"$hwrt\",\"lines\":$lines,\"expected\":$expected_lines}"
if [[ "$lines" -ne "$expected_lines" ]]; then
	echo "run_tier_check.sh: expected $expected_lines NextGI: line(s), found $lines" >&2
	exit 1
fi
exit "$status"
