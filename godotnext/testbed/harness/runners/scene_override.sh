#!/usr/bin/env bash
# Per-scene override.cfg handling for the harness runners (#240).
# Sourced, not executed. A scene folder may ship an override.cfg with
# restart-only, project-wide settings (the Feature Parity Room does).
# The runner copies it to the testbed root for that launch only and
# removes it afterwards. It never overwrites an existing root file.

SCENE_OVERRIDE_APPLIED=0
SCENE_OVERRIDE_SOURCE=""
SCENE_OVERRIDE_SHA256=""
SCENE_OVERRIDE_ROOT=""

# Prints the value of --<name> (either --<name>=value or --<name> value)
# from the remaining arguments, or nothing when absent.
scene_override_find_arg() {
	local name=$1
	shift
	local arg
	while [[ $# -gt 0 ]]; do
		arg=$1
		case "$arg" in
		--"$name"=*)
			printf '%s\n' "${arg#--"$name"=}"
			return 0
			;;
		--"$name")
			if [[ $# -ge 2 ]]; then
				printf '%s\n' "$2"
			fi
			return 0
			;;
		esac
		shift
	done
	return 0
}

scene_override_sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{ print $1 }'
	elif command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{ print $1 }'
	else
		python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$1"
	fi
}

# scene_override_apply <testbed_dir> <scene res:// path>
# Sets SCENE_OVERRIDE_* and copies the file when the scene folder has one.
scene_override_apply() {
	local testbed_dir=$1
	local scene=$2
	SCENE_OVERRIDE_APPLIED=0
	SCENE_OVERRIDE_SOURCE=""
	SCENE_OVERRIDE_SHA256=""
	SCENE_OVERRIDE_ROOT="$testbed_dir/override.cfg"
	if [[ -z "$scene" ]]; then
		return 0
	fi
	case "$scene" in
	res://*) ;;
	*)
		echo "scene_override: scene is not a res:// path: $scene" >&2
		return 2
		;;
	esac
	local relative=${scene#res://}
	case "/$relative/" in
	*/../*)
		echo "scene_override: scene path escapes the testbed: $scene" >&2
		return 2
		;;
	esac
	local folder
	folder=$(dirname "$relative")
	local source_path="$testbed_dir/$folder/override.cfg"
	if [[ "$folder" == "." ]]; then
		source_path="$testbed_dir/override.cfg"
	fi
	if [[ ! -f "$source_path" ]]; then
		return 0
	fi
	if [[ "$source_path" == "$SCENE_OVERRIDE_ROOT" ]]; then
		# The scene sits at the testbed root, so its override.cfg already
		# is the root file. Nothing to copy, and nothing to remove.
		SCENE_OVERRIDE_SOURCE="res://override.cfg"
		SCENE_OVERRIDE_SHA256=$(scene_override_sha256 "$source_path")
		return 0
	fi
	if [[ -e "$SCENE_OVERRIDE_ROOT" ]]; then
		echo "scene_override: $SCENE_OVERRIDE_ROOT already exists; not overwriting it" >&2
		return 2
	fi
	cp "$source_path" "$SCENE_OVERRIDE_ROOT"
	SCENE_OVERRIDE_APPLIED=1
	SCENE_OVERRIDE_SOURCE="res://$folder/override.cfg"
	SCENE_OVERRIDE_SHA256=$(scene_override_sha256 "$source_path")
	echo "scene_override: applied $SCENE_OVERRIDE_SOURCE for this run"
	return 0
}

scene_override_remove() {
	if [[ "$SCENE_OVERRIDE_APPLIED" == "1" && -n "$SCENE_OVERRIDE_ROOT" ]]; then
		rm -f "$SCENE_OVERRIDE_ROOT"
	fi
	SCENE_OVERRIDE_APPLIED=0
}
