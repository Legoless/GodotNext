#!/usr/bin/env bash
# Runs res://tests/src_relight/src_relight_check.tscn with NextGI enabled (#31).
#
# Usage:
#   run_src_relight_check.sh [--probes] [--shadow-mask] [--hwrt=auto|off|force] [--quality=0|1|2|3]
#     [--deny-ray-query] -- <godot> <godot arguments>
#
# --probes turns on nextgi/debug/read_page_radiance. --shadow-mask turns on read_shadow_mask.
# --quality pins nextgi/quality (the default keeps the project's Auto). GodotNext: ps_swrt_trace
# (#186, #188): the scene is told the tier to expect (--expected-tier, appended to its arguments):
# SWRT for --hwrt=off or --deny-ray-query (debug/deny_ray_query: a device without ray query), else
# HWRT. Refuses to overwrite an existing override.cfg. Prints Metal validation counts and exits with
# Godot's status, or 1 when the run logged any "ERROR:", "Invalid Metal usage" or "Error detected
# in" line (the bar is 0).

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

probes=false
shadow_mask=false
hwrt=auto
quality_line=""
deny=false
while [[ $# -gt 0 ]]; do
	case "$1" in
	--probes) probes=true ;;
	--shadow-mask) shadow_mask=true ;;
	--hwrt=auto | --hwrt=off | --hwrt=force) hwrt="${1#--hwrt=}" ;;
	--quality=0 | --quality=1 | --quality=2 | --quality=3) quality_line="global_illumination/nextgi/quality=${1#--quality=}" ;;
	--deny-ray-query) deny=true ;;
	--)
		shift
		break
		;;
	*)
		echo "run_src_relight_check.sh: unknown argument '$1'" >&2
		exit 2
		;;
	esac
	shift
done
if [[ $# -eq 0 ]]; then
	echo "run_src_relight_check.sh: pass the Godot command after --" >&2
	exit 2
fi
if [[ -e "$override" ]]; then
	echo "run_src_relight_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

case "$hwrt" in
auto) hwrt_value=0 ;;
off) hwrt_value=1 ;;
force) hwrt_value=2 ;;
esac

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<EOF
[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=$hwrt_value
global_illumination/nextgi/debug/read_page_radiance=$probes
global_illumination/nextgi/debug/read_shadow_mask=$shadow_mask
global_illumination/nextgi/debug/deny_ray_query=$deny
$quality_line
EOF

expected_tier=HWRT
if [[ "$hwrt" == off || "$deny" == true ]]; then
	expected_tier=SWRT
fi
command=("$@")
separator=false
for argument in "${command[@]}"; do
	[[ "$argument" == "--" ]] && separator=true
done
if [[ "$separator" == true ]]; then
	command+=("--expected-tier=$expected_tier")
else
	command+=("--" "--expected-tier=$expected_tier")
fi

set +e
"${command[@]}" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

errors=$(grep -c 'ERROR:' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_SRC_RELIGHT_LOG {\"probes\":$probes,\"shadow_mask\":$shadow_mask,\"hwrt\":\"$hwrt\",\"deny_ray_query\":$deny,\"expected_tier\":\"$expected_tier\",\"errors\":$errors,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
if [[ "$errors" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	echo "run_src_relight_check.sh: the run logged validation lines; the bar is 0" >&2
	exit 1
fi
