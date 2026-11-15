#!/usr/bin/env bash
# Runs res://tests/tsr_debug_views/tsr_debug_views.tscn, the TSR pass visualizer check (#198).
#
# Usage:
#   run_tsr_debug_views.sh -- <godot> <godot arguments, the scene included> [-- --tsr-preset=<preset> --tsr-thin=<on|off>]
#
# The scene sets the TSR preset, thin-geometry detection and the debug view itself, so the script
# writes no override.cfg; it refuses to run while one exists, because that could change the
# project settings under test. The scene prints one "GODOTNEXT_RESULT" line; this script repeats it
# and exits non-zero when Godot fails or the check did not pass.
#
# Example on the Linux L4 machine (Vulkan, validation on):
#   godotnext/testbed/tests/tsr_debug_views/run_tsr_debug_views.sh -- ./bin/godot.linuxbsd.editor.x86_64 \
#     --path godotnext/testbed --rendering-driver vulkan --gpu-validation --gpu-abort --resolution 1920x1080 \
#     res://tests/tsr_debug_views/tsr_debug_views.tscn -- --tsr-preset=high --tsr-thin=on

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
		echo "run_tsr_debug_views.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
done
if [[ $# -eq 0 ]]; then
	echo "run_tsr_debug_views.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_tsr_debug_views.sh: $override exists; remove it first" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$log"
}
trap cleanup EXIT

# pipefail makes the pipeline's status Godot's when Godot fails.
set +e
"$@" 2>&1 | tee "$log"
status=$?
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
echo "GODOTNEXT_TSR_DEBUG_VIEWS $result"
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_tsr_debug_views.sh: the TSR debug view check did not pass" >&2
	exit 1
fi
exit "$status"
