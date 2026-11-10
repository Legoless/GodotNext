#!/usr/bin/env bash
# PR check for MegaLights direct-light inputs (#136) on the Apple M5 Max.
# Usage: godotnext_megalights_inputs_check.sh [godot-binary] <base-commit-binary>
# The base binary is required. The script compares MegaLights-off captures of
# the three legacy scenes and courtyard_vg against that binary.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

godot_bin="${1:-bin/godot.macos.editor.arm64}"
base_bin="${2:-}"
runner="$repo_root/godotnext/testbed/harness/runners/macos_local.sh"
testbed="$repo_root/godotnext/testbed"
scene="res://tests/megalights_inputs/megalights_inputs_check.tscn"
work_root="${TMPDIR:-/tmp}/godotnext-136-megalights-inputs"
mkdir -p "$work_root"

export MTL_DEBUG_LAYER=1
export MTL_SHADER_VALIDATION=1
export MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
unset GODOTNEXT_MEGALIGHTS_INPUTS || true

die() {
	printf '%s\n' "$1" >&2
	exit 1
}

if [[ ! -x "$godot_bin" ]]; then
	die "Godot binary is not executable: $godot_bin"
fi
if [[ -z "$base_bin" ]]; then
	die "base binary is required; MegaLights-off identity was not run"
fi
if [[ ! -x "$base_bin" ]]; then
	die "Base binary is not executable: $base_bin"
fi

override_cfg="$testbed/override.cfg"
override_written=0
cleanup_override() {
	if [[ "$override_written" == 1 ]]; then
		rm -f "$override_cfg"
		override_written=0
	fi
}
trap cleanup_override EXIT

run_godot() {
	local log="$1"
	shift
	set +e
	"$runner" -- "$@" >"$log" 2>&1
	local status=$?
	set -e
	if grep -Eq 'ERROR:|Invalid Metal usage|Error detected in' "$log"; then
		grep -E 'ERROR:|Invalid Metal usage|Error detected in' "$log" >&2 || true
		die "Metal validation or engine error in $log"
	fi
	if [[ $status -ne 0 ]]; then
		tail -40 "$log" >&2 || true
		die "Godot exited $status (log $log)"
	fi
}

result_field() {
	local log="$1"
	local key="$2"
	local line
	line="$(grep -m1 '^GODOTNEXT_RESULT ' "$log" || true)"
	if [[ "$line" != *'"pass":true'* ]]; then
		printf '%s\n' "$line" >&2
		die "check did not pass ($log)"
	fi
	printf '%s\n' "$line" | sed -n "s/.*\"$key\":\"\\([^\"]*\\)\".*/\\1/p"
}

check_albedo() {
	local name="$1"
	local albedo="$2"
	local metallic="${3:-0}"
	# AO texture byte; -1 leaves AO off, so ml_albedo.a must be 1.
	local ao="${4:--1}"
	local out="$work_root/albedo_$name"
	local log="$work_root/albedo_$name.log"
	rm -rf "$out"
	mkdir -p "$out"
	GODOTNEXT_MEGALIGHTS_INPUTS=1 run_godot "$log" \
		"$godot_bin" --path "$testbed" --rendering-driver metal \
		--resolution 128x128 "$scene" -- \
		--scene="$scene" --out="$out" --ml-check=albedo --albedo="$albedo" --metallic="$metallic" --ao="$ao"
	result_field "$log" "mode" >/dev/null
	printf 'albedo %s metallic %s ao %s passed\n' "$albedo" "$metallic" "$ao"
}

check_albedo "red" "1,0,0" "0"
check_albedo "green" "0,1,0" "0"
check_albedo "blue" "0.2,0.4,0.8" "0"
check_albedo "blue_metal_half" "0.2,0.4,0.8" "0.5"
check_albedo "blue_metal" "0.2,0.4,0.8" "1"
# ml_albedo.a = mix(1, ao, ao_light_affect) with ao 64/255 and ao_light_affect 0.6.
check_albedo "blue_ao" "0.2,0.4,0.8" "0.5" "64"

if [[ -e "$override_cfg" ]]; then
	die "override.cfg already exists: $override_cfg"
fi
printf '[rendering]\n\ndriver/depth_prepass/enable=false\n' >"$override_cfg"
override_written=1
check_albedo "blue_no_prepass" "0.2,0.4,0.8" "0"
rm -f "$override_cfg"
override_written=0
printf 'albedo passed with depth prepass disabled\n'

off_out="$work_root/off_debug"
off_log="$work_root/off_debug.log"
rm -rf "$off_out"
mkdir -p "$off_out"
run_godot "$off_log" \
	"$godot_bin" --path "$testbed" --rendering-driver metal \
	--resolution 128x128 "$scene" -- \
	--scene="$scene" --out="$off_out" --ml-check=off --albedo="1,0,0"
result_field "$off_log" "mode" >/dev/null
printf 'off debug view did not show the albedo target\n'

run_fallback() {
	local label="$1"
	local out="$work_root/fallback_$label"
	local log="$work_root/fallback_$label.log"
	rm -rf "$out"
	mkdir -p "$out"
	if [[ "$label" == "on" ]]; then
		GODOTNEXT_MEGALIGHTS_INPUTS=1 run_godot "$log" \
			"$godot_bin" --path "$testbed" --rendering-driver metal \
			--resolution 128x128 "$scene" -- \
			--scene="$scene" --out="$out" --ml-check=fallback
	else
		run_godot "$log" \
			"$godot_bin" --path "$testbed" --rendering-driver metal \
			--resolution 128x128 "$scene" -- \
			--scene="$scene" --out="$out" --ml-check=fallback
	fi
	result_field "$log" "left_hex"
	printf '%s\n' "$(grep -m1 '^GODOTNEXT_RESULT ' "$log")"
}

left_off="$(run_fallback off | tail -1)"
left_on="$(run_fallback on | tail -1)"
off_left="$(printf '%s\n' "$left_off" | sed -n 's/.*"left_hex":"\([^"]*\)".*/\1/p')"
on_left="$(printf '%s\n' "$left_on" | sed -n 's/.*"left_hex":"\([^"]*\)".*/\1/p')"
off_full="$(printf '%s\n' "$left_off" | sed -n 's/.*"full_hex":"\([^"]*\)".*/\1/p')"
on_full="$(printf '%s\n' "$left_on" | sed -n 's/.*"full_hex":"\([^"]*\)".*/\1/p')"
if [[ -z "$off_left" || "$off_left" != "$on_left" ]]; then
	die "fallback region is not byte-identical"
fi
printf 'fallback region matches\n'
# Nothing in #136 reads the input targets, so turning them on must not change any pixel.
# #143 adds MegaLights shading; from then on only the fallback region must match, and
# #143 and GM-9 carry the fallback-identity check.
if [[ -z "$off_full" || "$off_full" != "$on_full" ]]; then
	die "full fallback frame differs with MegaLights inputs on"
fi
printf 'full fallback frame matches\n'

smoke_scene() {
	local label="$1"
	local scene_path="$2"
	local out="$work_root/smoke_$label"
	local log="$work_root/smoke_$label.log"
	rm -rf "$out"
	mkdir -p "$out"
	if [[ "$label" == *on ]]; then
		GODOTNEXT_MEGALIGHTS_INPUTS=1 run_godot "$log" \
			"$godot_bin" --path "$testbed" --rendering-driver metal \
			--write-movie "$out/frame.png" --fixed-fps 60 --quit-after 400 -- \
			--mode=capture --scene="$scene_path" --camera=CaptureCam --out="$out" \
			--seed=1 --frames=3 --warmup=1 --capture-frames=1
	else
		run_godot "$log" \
			"$godot_bin" --path "$testbed" --rendering-driver metal \
			--write-movie "$out/frame.png" --fixed-fps 60 --quit-after 400 -- \
			--mode=capture --scene="$scene_path" --camera=CaptureCam --out="$out" \
			--seed=1 --frames=3 --warmup=1 --capture-frames=1
	fi
	printf 'smoke %s passed\n' "$label"
}

check_material() {
	local out="$work_root/material"
	local log="$work_root/material.log"
	rm -rf "$out"
	mkdir -p "$out"
	GODOTNEXT_MEGALIGHTS_INPUTS=1 run_godot "$log" \
		"$godot_bin" --path "$testbed" --rendering-driver metal \
		--resolution 128x128 "$scene" -- \
		--scene="$scene" --out="$out" --ml-check=material
	result_field "$log" "mode" >/dev/null
	printf '%s\n' "$(grep -m1 '^GODOTNEXT_RESULT ' "$log")"
	printf 'material bytes passed\n'
}

check_decal() {
	local out="$work_root/decal"
	local log="$work_root/decal.log"
	rm -rf "$out"
	mkdir -p "$out"
	GODOTNEXT_MEGALIGHTS_INPUTS=1 run_godot "$log" \
		"$godot_bin" --path "$testbed" --rendering-driver metal \
		--resolution 128x128 "$scene" -- \
		--scene="$scene" --out="$out" --ml-check=decal
	result_field "$log" "mode" >/dev/null
	printf '%s\n' "$(grep -m1 '^GODOTNEXT_RESULT ' "$log")"
	printf 'decal albedo, AO and metallic reach the inputs\n'
}

check_msaa() {
	local out="$work_root/msaa"
	local log="$work_root/msaa.log"
	rm -rf "$out"
	mkdir -p "$out"
	GODOTNEXT_MEGALIGHTS_INPUTS=1 run_godot "$log" \
		"$godot_bin" --path "$testbed" --rendering-driver metal \
		--resolution 128x128 "$scene" -- \
		--scene="$scene" --out="$out" --ml-check=msaa
	result_field "$log" "mode" >/dev/null
	printf 'msaa allocated no megalights textures\n'
}

check_material
check_decal
check_msaa

smoke_scene cbr_off "res://scenes/color_box_room/cbr_a.tscn"
smoke_scene cbr_on "res://scenes/color_box_room/cbr_a.tscn"
smoke_scene courtyard_off "res://scenes/concave_courtyard/courtyard.tscn"
smoke_scene courtyard_on "res://scenes/concave_courtyard/courtyard.tscn"
smoke_scene courtyard_vg_off "res://scenes/concave_courtyard/courtyard_vg.tscn"
smoke_scene courtyard_vg_on "res://scenes/concave_courtyard/courtyard_vg.tscn"

identity_scenes=(
	"res://scenes/legacy/legacy_lightmap.tscn"
	"res://scenes/legacy/legacy_sdfgi.tscn"
	"res://scenes/legacy/legacy_voxelgi.tscn"
	"res://scenes/concave_courtyard/courtyard_vg.tscn"
)
for scene_path in "${identity_scenes[@]}"; do
		name="$(basename "$scene_path" .tscn)"
		for side in base head; do
			bin="$godot_bin"
			if [[ "$side" == "base" ]]; then
				bin="$base_bin"
			fi
			out="$work_root/identity_${name}_${side}"
			log="$work_root/identity_${name}_${side}.log"
			rm -rf "$out"
			mkdir -p "$out"
			run_godot "$log" \
				"$bin" --path "$testbed" --rendering-driver metal \
				--write-movie "$out/frame.png" --fixed-fps 60 --quit-after 1062 -- \
				--mode=capture --scene="$scene_path" --camera=CaptureCam --out="$out" \
				--seed=1 --frames=10 --warmup=2 --capture-frames=5,11
		done
		for slot in 5 11; do
			a="$work_root/identity_${name}_base/hdr_$(printf '%05d' "$slot").exr"
			b="$work_root/identity_${name}_head/hdr_$(printf '%05d' "$slot").exr"
			if [[ ! -f "$a" || ! -f "$b" ]]; then
				die "missing capture $a or $b"
			fi
			if ! cmp -s "$a" "$b"; then
				die "MegaLights-off capture differs: $name frame $slot"
			fi
		done
		printf 'identity %s matches the base binary\n' "$name"
done

printf 'MegaLights input checks passed\n'
