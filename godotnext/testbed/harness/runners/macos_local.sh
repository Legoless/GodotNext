#!/usr/bin/env bash
# Local runner for the Apple M5 Max.
# Reads power state and refuses to launch unless the machine is on AC power
# in High Power mode (pmset powermode 2). Never changes power settings.

set -euo pipefail

# Rendered runs on the M5 Max must not take the desktop's focus (docs/godotnext/fleet.md).
export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scene_override.sh
source "$script_dir/scene_override.sh"
repo_root=$(cd "$script_dir/../../../.." && pwd)
GODOT="${GODOT:-$repo_root/bin/godot.macos.editor.arm64}"
HOST_NAME="Apple M5 Max"

usage() {
	cat <<'EOF'
Usage: macos_local.sh --check
       macos_local.sh [--fail-on-validation] -- <godot arguments>

--check prints AC power, powermode, and thermal state, then exits.
--fail-on-validation exits 1 when Godot's output has any "ERROR:",
"Invalid Metal usage" or "Error detected in" line, even when Godot exits 0.
The godot arguments must include an absolute --out directory.
This script only reads `pmset -g`, `pmset -g batt`, and `pmset -g therm`.
EOF
}

filter_dump() {
	awk 'index($0, "/") { next } /prevented by/ { next } { print }'
}

thermal_summary() {
	awk 'BEGIN { out = "" } /^Note: / {
		line = substr($0, 7)
		if (out == "") {
			out = line
		} else {
			out = out "; " line
		}
	} END { print out }'
}

powermode_name() {
	case "$1" in
	0) printf '%s\n' "Automatic" ;;
	1) printf '%s\n' "Low Power" ;;
	2) printf '%s\n' "High Power" ;;
	*) printf '%s\n' "unknown" ;;
	esac
}

read_power() {
	pmset_raw=$(pmset -g | filter_dump)
	batt_raw=$(pmset -g batt | filter_dump)
	therm_raw=$(pmset -g therm | filter_dump)
	powermode=$(printf '%s\n' "$pmset_raw" | awk '$1 == "powermode" { print $2; exit }')
	thermal=$(printf '%s\n' "$therm_raw" | thermal_summary)
	if [[ -z "$thermal" ]]; then
		thermal="unparsed"
	fi
	on_ac=false
	if printf '%s\n' "$batt_raw" | grep -q "Now drawing from 'AC Power'"; then
		on_ac=true
	fi
	if [[ -z "$powermode" ]]; then
		mode_label="unknown"
	else
		mode_label=$(powermode_name "$powermode")
	fi
}

print_state() {
	printf 'on_ac=%s\n' "$on_ac"
	printf 'powermode=%s\n' "${powermode:-missing}"
	printf 'powermode_name=%s\n' "$mode_label"
	printf 'thermal=%s\n' "$thermal"
}

write_job() {
	JOB_PATH="$out_dir/runner_job.json"
	export GODOTNEXT_HARNESS_JOB="$JOB_PATH"
	PMSET_RAW="$pmset_raw" BATT_RAW="$batt_raw" THERM_RAW="$therm_raw" \
		POWERMODE="$powermode" MODE_LABEL="$mode_label" THERMAL="$thermal" ON_AC="$on_ac" \
		JOB_PATH="$JOB_PATH" HOST_NAME="$HOST_NAME" \
		OVERRIDE_SOURCE="$SCENE_OVERRIDE_SOURCE" OVERRIDE_SHA256="$SCENE_OVERRIDE_SHA256" \
		python3 - <<'PY'
import json
import os

on_ac = os.environ["ON_AC"] == "true"
powermode = os.environ.get("POWERMODE", "")
job = {
    "override": {
        "applied": bool(os.environ["OVERRIDE_SOURCE"]),
        "source": os.environ["OVERRIDE_SOURCE"],
        "sha256": os.environ["OVERRIDE_SHA256"],
    },
    "host": os.environ["HOST_NAME"],
    "clock": {
        "locked": False,
        "detail": "Clock locks belong to issue 237 and are not taken by this runner.",
    },
    "power": {
        "on_ac": on_ac,
        "powermode": int(powermode) if powermode else -1,
        "powermode_name": os.environ["MODE_LABEL"],
        "thermal": os.environ["THERMAL"],
        "before": {
            "pmset": os.environ["PMSET_RAW"],
            "batt": os.environ["BATT_RAW"],
            "therm": os.environ["THERM_RAW"],
        },
    },
}
with open(os.environ["JOB_PATH"], "w", encoding="utf-8") as handle:
    json.dump(job, handle, indent="\t")
    handle.write("\n")
PY
}

patch_manifest() {
	manifest="$out_dir/manifest.json"
	after_path="$out_dir/runner_power_after.json"
	PMSET_RAW="$pmset_raw" BATT_RAW="$batt_raw" THERM_RAW="$therm_raw" AFTER_PATH="$after_path" \
		python3 - <<'PY' || return 1
import json
import os

after = {
    "pmset": os.environ["PMSET_RAW"],
    "batt": os.environ["BATT_RAW"],
    "therm": os.environ["THERM_RAW"],
}
with open(os.environ["AFTER_PATH"], "w", encoding="utf-8") as handle:
    json.dump(after, handle)
    handle.write("\n")
PY
	if [[ ! -f "$manifest" ]]; then
		echo "macos_local.sh: manifest.json was not written" >&2
		return 1
	fi
	MANIFEST_PATH="$manifest" AFTER_PATH="$after_path" python3 - <<'PY' || return 1
import json
import os

manifest_path = os.environ["MANIFEST_PATH"]
with open(os.environ["AFTER_PATH"], encoding="utf-8") as handle:
    after = json.load(handle)
with open(manifest_path, encoding="utf-8") as handle:
    data = json.load(handle)
runner = data.setdefault("runner", {})
power = runner.setdefault("power", {})
power["after"] = after
with open(manifest_path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent="\t")
    handle.write("\n")
PY
}

find_out_dir() {
	out_dir=""
	index=0
	while [[ $index -lt ${#godot_args[@]} ]]; do
		arg=${godot_args[$index]}
		case "$arg" in
		--out=*)
			out_dir=${arg#--out=}
			;;
		--out)
			if [[ $((index + 1)) -lt ${#godot_args[@]} ]]; then
				out_dir=${godot_args[$((index + 1))]}
			fi
			;;
		esac
		index=$((index + 1))
	done
}

check_only=0
fail_on_validation=0
godot_args=()
while [[ $# -gt 0 ]]; do
	case "$1" in
	--check)
		check_only=1
		shift
		;;
	--fail-on-validation)
		fail_on_validation=1
		shift
		;;
	--help|-h)
		usage
		exit 0
		;;
	--)
		shift
		if [[ $# -gt 0 ]]; then
			godot_args=("$@")
		fi
		break
		;;
	*)
		godot_args+=("$1")
		shift
		;;
	esac
done

read_power
print_state

if [[ -z "${powermode:-}" || "$powermode" != "2" || "$on_ac" != "true" ]]; then
	echo "macos_local.sh: refusing to launch. Need AC power and High Power mode (powermode 2)." >&2
	exit 1
fi

if [[ $check_only -eq 1 ]]; then
	exit 0
fi

if [[ ${#godot_args[@]} -eq 0 ]]; then
	usage >&2
	exit 2
fi

find_out_dir
if [[ -z "$out_dir" ]]; then
	echo "macos_local.sh: godot arguments must include an absolute --out directory" >&2
	exit 2
fi
case "$out_dir" in
/*) ;;
*)
	echo "macos_local.sh: --out must be an absolute directory" >&2
	exit 2
	;;
esac

mkdir -p "$out_dir"

# A per-scene override.cfg next to --scene applies to this launch only (#240).
scene_arg=$(scene_override_find_arg scene "${godot_args[@]}")
path_arg=$(scene_override_find_arg path "${godot_args[@]}")
testbed_dir="$repo_root/godotnext/testbed"
if [[ -n "$path_arg" ]]; then
	case "$path_arg" in
	/*) testbed_dir=$path_arg ;;
	*) testbed_dir="$PWD/$path_arg" ;;
	esac
fi
if ! scene_override_apply "$testbed_dir" "$scene_arg"; then
	exit 2
fi
trap scene_override_remove EXIT

write_job

set +e
if [[ $fail_on_validation -eq 1 ]]; then
	# Opt-in, because captures of the legacy scenes share this runner and some of them log a
	# known intermittent validation report.
	validation_log=$(mktemp)
	"${godot_args[@]}" 2>&1 | tee "$validation_log"
	status=${PIPESTATUS[0]}
else
	"${godot_args[@]}"
	status=$?
fi
set -e
if [[ $fail_on_validation -eq 1 ]]; then
	errors=$(grep -c 'ERROR:' "$validation_log" || true)
	metal_usage=$(grep -c 'Invalid Metal usage' "$validation_log" || true)
	metal_shader=$(grep -c 'Error detected in' "$validation_log" || true)
	rm -f "$validation_log"
	printf 'GODOTNEXT_RUNNER_VALIDATION {"errors":%d,"invalid_metal_usage":%d,"shader_validation":%d,"status":%d}\n' "$errors" "$metal_usage" "$metal_shader" "$status"
	if [[ $status -eq 0 && ($errors -ne 0 || $metal_usage -ne 0 || $metal_shader -ne 0) ]]; then
		echo "macos_local.sh: the run logged validation lines; the bar is 0" >&2
		status=1
	fi
fi

read_power
print_state
if ! patch_manifest; then
	if [[ $status -ne 0 ]]; then
		exit "$status"
	fi
	exit 1
fi
exit "$status"
