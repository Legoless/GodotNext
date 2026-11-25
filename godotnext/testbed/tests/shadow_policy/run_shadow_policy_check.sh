#!/usr/bin/env bash
# Runs a check scene of the RT visibility policy with NextGI on (#145):
#   res://tests/shadow_policy/shadow_policy_check.tscn  GM-4's caster-mask matrix (debug/shadow_ray_check);
#   res://tests/shadow_policy/shadow_alpha_check.tscn   alpha-tested and translucent casters through the SRC
#                                                       relight's shadow mask (debug/read_shadow_mask).
#
# Usage:
#   run_shadow_policy_check.sh -- <godot> <godot arguments>
#
# Writes godotnext/testbed/override.cfg (git ignores it) with NextGI on at High and the hardware tier on Auto,
# runs Godot and removes the file again. It refuses to overwrite an existing override.cfg. The scene turns its
# debug setting on and prints one "GODOTNEXT_RESULT" line. Exits non-zero when Godot fails, the result is a
# failure or the run logged any "ERROR:", "Validation Error", "Invalid Metal usage" or "Error detected in" line
# (the bar is 0).
# SHADOW_POLICY_HWRT_MODE=1 (Simplified Material) or 2 (Inline) also writes
# rendering/global_illumination/nextgi/hardware_ray_tracing_mode and its Windows and Linux entries (#286); add
# "-- --expect=simplified-material" or "-- --expect=inline" after shadow_alpha_check.tscn.
#
# GM-4's other lines are measured in PQ (plan §22) and are reported here, not checked: alpha foliage coverage
# IoU against the D16 reference, the translucent glass's opaque shadow, and the opaque-only shadow-ray cost.
#
# Example on the RTX 4080 SUPER machine:
#   godotnext/testbed/tests/shadow_policy/run_shadow_policy_check.sh -- godot --path godotnext/testbed \
#     --rendering-driver vulkan --gpu-validation res://tests/shadow_policy/shadow_policy_check.tscn

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed_dir=$(cd "$script_dir/../.." && pwd)
override="$testbed_dir/override.cfg"

if [[ "${1:-}" != "--" ]]; then
	echo "run_shadow_policy_check.sh: pass the Godot command after --" >&2
	exit 2
fi
shift
if [[ -e "$override" ]]; then
	echo "run_shadow_policy_check.sh: $override already exists; not overwriting it" >&2
	exit 2
fi

log=$(mktemp)
cleanup() {
	rm -f "$override" "$log"
}
trap cleanup EXIT

cat >"$override" <<CFG
[application]

config/use_custom_user_dir=true
config/custom_user_dir_name="godotnext-shadow-policy-check"

[rendering]

global_illumination/nextgi/enabled=true
global_illumination/nextgi/hardware_ray_tracing=0
global_illumination/nextgi/hardware_ray_tracing.macos=0
global_illumination/nextgi/quality=2
CFG
mode="${SHADOW_POLICY_HWRT_MODE:-}"
if [[ -n "$mode" ]]; then
	cat >>"$override" <<CFG
global_illumination/nextgi/hardware_ray_tracing_mode=$mode
global_illumination/nextgi/hardware_ray_tracing_mode.windows=$mode
global_illumination/nextgi/hardware_ray_tracing_mode.linuxbsd=$mode
global_illumination/nextgi/hardware_ray_tracing_mode.macos=$mode
CFG
fi

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

result=$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)
errors=$(grep -c 'ERROR:' "$log" || true)
validation=$(grep -ci 'Validation Error' "$log" || true)
metal_usage=$(grep -c 'Invalid Metal usage' "$log" || true)
metal_shader=$(grep -c 'Error detected in' "$log" || true)
echo "GODOTNEXT_SHADOW_POLICY_LOG {\"errors\":$errors,\"validation_errors\":$validation,\"invalid_metal_usage\":$metal_usage,\"shader_validation\":$metal_shader,\"status\":$status}"
echo "GM-4 alpha foliage coverage IoU >= 0.95: measured in PQ"
echo "GM-4 translucent glass shadows as opaque (LQ6): measured in PQ"
echo "GM opaque-only shadow-ray cost <= 1.05x: measured in PQ"
if [[ "$result" != *'"pass":true'* ]]; then
	echo "run_shadow_policy_check.sh: the check did not pass" >&2
	exit 1
fi
if [[ "$status" -ne 0 ]]; then
	exit "$status"
fi
if [[ "$errors" -ne 0 || "$validation" -ne 0 || "$metal_usage" -ne 0 || "$metal_shader" -ne 0 ]]; then
	echo "run_shadow_policy_check.sh: the run logged errors or validation lines; the bar is 0" >&2
	exit 1
fi
