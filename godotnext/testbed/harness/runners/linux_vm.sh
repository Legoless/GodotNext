#!/usr/bin/env bash
# Linux VM runner. Doctests and lavapipe functional runs only. Never timing.
# The caller points Vulkan at lavapipe (for example with VK_DRIVER_FILES).
# This script does not ship or require that ICD.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scene_override.sh
source "$script_dir/scene_override.sh"
repo_root=$(cd "$script_dir/../../../.." && pwd)
GODOT="${GODOT:-$repo_root/bin/godot.linuxbsd.editor.x86_64}"

usage() {
	cat <<'EOF'
Usage: linux_vm.sh doctest
       linux_vm.sh functional -- <godot arguments>

doctest runs `godot --headless --test --force-colors`.
functional launches Godot headless and rejects --mode=timing.
Pass an absolute --out when the arguments are a harness run.
EOF
}

reject_timing() {
	index=0
	while [[ $index -lt ${#func_args[@]} ]]; do
		arg=${func_args[$index]}
		if [[ "$arg" == "--mode=timing" ]]; then
			echo "linux_vm.sh: timing mode is not allowed on the Linux VM" >&2
			exit 1
		fi
		if [[ "$arg" == "--mode" ]]; then
			next=$((index + 1))
			if [[ $next -lt ${#func_args[@]} && "${func_args[$next]}" == "timing" ]]; then
				echo "linux_vm.sh: timing mode is not allowed on the Linux VM" >&2
				exit 1
			fi
		fi
		index=$((index + 1))
	done
}

find_out_dir() {
	out_dir=""
	index=0
	while [[ $index -lt ${#func_args[@]} ]]; do
		arg=${func_args[$index]}
		case "$arg" in
		--out=*)
			out_dir=${arg#--out=}
			;;
		--out)
			if [[ $((index + 1)) -lt ${#func_args[@]} ]]; then
				out_dir=${func_args[$((index + 1))]}
			fi
			;;
		esac
		index=$((index + 1))
	done
}

write_job() {
	JOB_PATH="$out_dir/runner_job.json"
	export GODOTNEXT_HARNESS_JOB="$JOB_PATH"
	JOB_PATH="$JOB_PATH" OVERRIDE_SOURCE="$SCENE_OVERRIDE_SOURCE" \
		OVERRIDE_SHA256="$SCENE_OVERRIDE_SHA256" python3 - <<'PY'
import json
import os

job = {
    "override": {
        "applied": bool(os.environ["OVERRIDE_SOURCE"]),
        "source": os.environ["OVERRIDE_SOURCE"],
        "sha256": os.environ["OVERRIDE_SHA256"],
    },
    "host": "Linux VM",
    "clock": {
        "locked": False,
        "detail": "The Linux VM runner does not lock clocks and does not run timing mode.",
    },
    "power": {
        "on_ac": False,
        "powermode": -1,
        "powermode_name": "unknown",
        "thermal": "unknown",
        "before": {},
    },
}
with open(os.environ["JOB_PATH"], "w", encoding="utf-8") as handle:
    json.dump(job, handle, indent="\t")
    handle.write("\n")
PY
}

if [[ $# -lt 1 ]]; then
	usage >&2
	exit 2
fi

command_name=$1
shift

case "$command_name" in
doctest)
	if [[ $# -ne 0 ]]; then
		echo "linux_vm.sh: doctest takes no arguments" >&2
		exit 2
	fi
	exec "$GODOT" --headless --test --force-colors
	;;
functional)
	func_args=()
	if [[ $# -gt 0 && "$1" == "--" ]]; then
		shift
	fi
	if [[ $# -gt 0 ]]; then
		func_args=("$@")
	fi
	if [[ ${#func_args[@]} -eq 0 ]]; then
		usage >&2
		exit 2
	fi
	reject_timing
	find_out_dir
	if [[ -n "$out_dir" ]]; then
		case "$out_dir" in
		/*) ;;
		*)
			echo "linux_vm.sh: --out must be an absolute directory" >&2
			exit 2
			;;
		esac
		mkdir -p "$out_dir"
	fi
	# A per-scene override.cfg next to --scene applies to this launch only (#240).
	scene_arg=$(scene_override_find_arg scene "${func_args[@]}")
	path_arg=$(scene_override_find_arg path "${func_args[@]}")
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
	if [[ -n "$out_dir" ]]; then
		write_job
	fi
	set +e
	"$GODOT" --headless "${func_args[@]}"
	status=$?
	set -e
	scene_override_remove
	trap - EXIT
	exit "$status"
	;;
--help|-h|help)
	usage
	exit 0
	;;
*)
	usage >&2
	exit 2
	;;
esac
