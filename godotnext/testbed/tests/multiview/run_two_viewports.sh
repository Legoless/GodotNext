#!/usr/bin/env bash
# Runs res://tests/multiview/run_two_viewports.tscn with NextGI enabled (#53).
#
# Usage:
#   run_two_viewports.sh -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with the NextGI settings, runs Godot and
# removes the file again. It refuses to overwrite an existing override.cfg. The scene prints one
# "GODOTNEXT_RESULT" line; this script counts the denoise views that rendered and fails unless the
# scene passed. Exits non-zero when Godot fails or the result is a failure.
#
# Example on the Apple M5 Max:
#   godotnext/testbed/tests/multiview/run_two_viewports.sh -- ./bin/godot.macos.editor.arm64 \
#     --path godotnext/testbed --rendering-driver metal \
#     res://tests/multiview/run_two_viewports.tscn

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--)
		shift
		break
		;;
	*)
		echo "run_two_viewports.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
done
if [[ $# -eq 0 ]]; then
	echo "run_two_viewports.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_two_viewports.sh: $override already exists; not overwriting it" >&2
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
EOF

# pipefail makes the pipeline's status Godot's when Godot fails.
set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
echo "GODOTNEXT_TWO_VIEWPORTS $result"
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_two_viewports.sh: the two-viewport check did not pass" >&2
	exit 1
fi
exit "$status"
