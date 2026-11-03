#!/usr/bin/env bash
# PR check for the NextGI ray-query toolchain on the Apple M5 Max.
# A relative Godot binary path is resolved from the repository root.
# spirv-val and spirv-cross come from GODOTNEXT_SPIRV_VAL / GODOTNEXT_SPIRV_CROSS, or from PATH.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)" || exit 1
cd "$repo_root" || exit 1

if [[ $# -gt 1 ]]; then
	printf 'usage: %s [godot-binary]\n' "$0" >&2
	exit 1
fi

godot_bin="${1:-bin/godot.macos.editor.arm64}"

die() {
	printf '%s\n' "$1" >&2
	exit 1
}

resolve_tool() {
	local label="$1"
	local tool="$2"
	if [[ "$tool" == */* ]]; then
		if [[ ! -x "$tool" ]]; then
			die "$label is not executable: $tool"
		fi
		printf '%s\n' "$tool"
		return 0
	fi
	local found
	found="$(command -v "$tool" 2>/dev/null || true)"
	if [[ -z "$found" || ! -x "$found" ]]; then
		die "$label was not found on PATH: $tool"
	fi
	printf '%s\n' "$found"
}

if [[ ! -x "$godot_bin" ]]; then
	die "Godot binary is not executable: $godot_bin"
fi

spirv_val="$(resolve_tool "spirv-val" "${GODOTNEXT_SPIRV_VAL:-spirv-val}")" || exit 1
spirv_cross="$(resolve_tool "spirv-cross" "${GODOTNEXT_SPIRV_CROSS:-spirv-cross}")" || exit 1

spv_out="$(mktemp "${TMPDIR:-/tmp}/nextgi-rq-selftest.XXXXXX")" || exit 1
trap 'rm -f "$spv_out"' EXIT

GODOTNEXT_RQ_SPIRV_OUT="$spv_out" "$godot_bin" --headless --test --force-colors \
	--test-case='*compiles through glslang*'

if [[ ! -s "$spv_out" ]]; then
	die "The doctest wrote no SPIR-V module to GODOTNEXT_RQ_SPIRV_OUT."
fi

"$spirv_val" --target-env vulkan1.1 "$spv_out"

msl_out="$("$spirv_cross" "$spv_out" --msl --msl-version 30000 --stage comp)" || exit 1
msl_line=""
while IFS= read -r line; do
	case "$line" in
	*intersection_query*)
		printf '%s\n' "$line"
		msl_line="$line"
		;;
	esac
done <<< "$msl_out"

if [[ -z "$msl_line" ]]; then
	die "spirv-cross MSL does not contain intersection_query."
fi
