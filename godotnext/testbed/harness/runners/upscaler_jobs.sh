#!/usr/bin/env bash
# Runner job definitions and launcher for #95 (GU-4 / GU-5 upscaler & Q19/Q21 captures).
# Covers TAA, FSR2, the DEV copy upscaler, SSAA 2x MSAA 8x/4x references, and
# content masks at 2560x1440 and 3840x2160 across Upscaler Torture and the
# three legacy baseline scenes.

set -euo pipefail

export GODOTNEXT_BACKGROUND="${GODOTNEXT_BACKGROUND:-1}"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../../../.." && pwd)
testbed="$repo_root/godotnext/testbed"

GODOT="${GODOT:-$repo_root/bin/godot.macos.editor.arm64}"
GODOT_DEV="${GODOT_DEV:-$repo_root/bin/godot.macos.editor.dev.arm64}"

SCENES=(upscaler_torture legacy_sdfgi legacy_voxelgi legacy_lightmap)
RESOLUTIONS=(2560x1440 3840x2160)
JOBS=(taa fsr2 copy ssaa_msaa8 ssaa_msaa4 mask_alpha_particles mask_scrolling_emissive)

list_only=0
dry_run=0
filter_scene=""
filter_job=""
filter_res=""
override_frames=0
seed_val=1
out_root="$testbed/captures/upscaler_jobs"

usage() {
	cat <<'EOF'
Usage: upscaler_jobs.sh [--list] [--dry-run] [--scene <id>] [--job <id>] [--resolution <WxH>] [--frames <N>] [--seed <N>] [--out-root <abs_dir>]

Scenes:      upscaler_torture | legacy_sdfgi | legacy_voxelgi | legacy_lightmap
Jobs:        taa | fsr2 | copy | ssaa_msaa8 | ssaa_msaa4 | mask_alpha_particles | mask_scrolling_emissive
Resolutions: 2560x1440 | 3840x2160
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--list)
		list_only=1
		shift
		;;
	--dry-run)
		dry_run=1
		shift
		;;
	--scene)
		filter_scene=$2
		shift 2
		;;
	--scene=*)
		filter_scene=${1#--scene=}
		shift
		;;
	--job)
		filter_job=$2
		shift 2
		;;
	--job=*)
		filter_job=${1#--job=}
		shift
		;;
	--resolution)
		filter_res=$2
		shift 2
		;;
	--resolution=*)
		filter_res=${1#--resolution=}
		shift
		;;
	--frames)
		override_frames=$2
		shift 2
		;;
	--frames=*)
		override_frames=${1#--frames=}
		shift
		;;
	--seed)
		seed_val=$2
		shift 2
		;;
	--seed=*)
		seed_val=${1#--seed=}
		shift
		;;
	--out-root)
		out_root=$2
		shift 2
		;;
	--out-root=*)
		out_root=${1#--out-root=}
		shift
		;;
	--help|-h)
		usage
		exit 0
		;;
	*)
		echo "upscaler_jobs.sh: unknown argument $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

case "$out_root" in
/*) ;;
*)
	echo "upscaler_jobs.sh: --out-root must be an absolute path" >&2
	exit 2
	;;
esac

scene_path_for() {
	case "$1" in
	upscaler_torture) printf '%s\n' "res://scenes/upscaler_torture/upscaler_torture.tscn" ;;
	legacy_sdfgi) printf '%s\n' "res://scenes/legacy/legacy_sdfgi.tscn" ;;
	legacy_voxelgi) printf '%s\n' "res://scenes/legacy/legacy_voxelgi.tscn" ;;
	legacy_lightmap) printf '%s\n' "res://scenes/legacy/legacy_lightmap.tscn" ;;
	*) return 1 ;;
	esac
}

camera_for() {
	case "$1" in
	upscaler_torture) printf '%s\n' "Camera3D" ;;
	*) printf '%s\n' "CaptureCam" ;;
	esac
}

default_frames_for() {
	case "$1" in
	upscaler_torture) printf '%s\n' "600" ;;
	*) printf '%s\n' "184" ;;
	esac
}

job_applies_to_scene() {
	local scene_id=$1
	local job_id=$2
	case "$job_id" in
	mask_alpha_particles|mask_scrolling_emissive)
		[[ "$scene_id" == "upscaler_torture" ]]
		;;
	*)
		return 0
		;;
	esac
}

matched=0
for scene_id in "${SCENES[@]}"; do
	if [[ -n "$filter_scene" && "$scene_id" != "$filter_scene" ]]; then
		continue
	fi
	scene_res=$(scene_path_for "$scene_id")
	cam=$(camera_for "$scene_id")
	frames=$(default_frames_for "$scene_id")
	if [[ $override_frames -gt 0 ]]; then
		frames=$override_frames
	fi
	quit_after=$((900 + 30 + 120 + frames))

	for res in "${RESOLUTIONS[@]}"; do
		if [[ -n "$filter_res" && "$res" != "$filter_res" ]]; then
			continue
		fi
		for job_id in "${JOBS[@]}"; do
			if [[ -n "$filter_job" && "$job_id" != "$filter_job" ]]; then
				continue
			fi
			if ! job_applies_to_scene "$scene_id" "$job_id"; then
				continue
			fi
			matched=$((matched + 1))
			out_dir="$out_root/$res/$scene_id/$job_id"
			bin_path="$GODOT"
			use_copy_env=0
			extra_user_args=()
			case "$job_id" in
			taa)
				extra_user_args+=(--upscaler=taa)
				;;
			fsr2)
				extra_user_args+=(--upscaler=fsr2)
				;;
			copy)
				bin_path="$GODOT_DEV"
				use_copy_env=1
				extra_user_args+=(--upscaler=copy)
				;;
			ssaa_msaa8)
				extra_user_args+=(--reference=ssaa --msaa=8)
				;;
			ssaa_msaa4)
				extra_user_args+=(--reference=ssaa --msaa=4)
				;;
			mask_alpha_particles)
				extra_user_args+=(--mask=alpha_particles)
				;;
			mask_scrolling_emissive)
				extra_user_args+=(--mask=scrolling_emissive)
				;;
			esac
			if [[ "$scene_id" == "upscaler_torture" && "$job_id" != mask_* && "$job_id" != ssaa_* ]]; then
				extra_user_args+=(--velocity-dump)
			fi

			if [[ $list_only -eq 1 ]]; then
				printf '%s\t%s\t%s\t%s\t%d\n' "$scene_id" "$res" "$job_id" "$scene_res" "$frames"
				continue
			fi

			cmd=(
				"$script_dir/macos_local.sh" --
				"$bin_path"
				--path "$testbed"
				--rendering-driver metal
				--resolution "$res"
				--write-movie "$out_dir/frame.png"
				--fixed-fps 60
				--quit-after "$quit_after"
				"$scene_res"
				--
				--mode=capture
				--scene="$scene_res"
				--camera="$cam"
				--out="$out_dir"
				--seed="$seed_val"
				--frames="$frames"
				"${extra_user_args[@]}"
			)

			if [[ $dry_run -eq 1 ]]; then
				if [[ $use_copy_env -eq 1 ]]; then
					printf 'GODOT_DEV_COPY_UPSCALER=1 '
				fi
				printf '%q ' "${cmd[@]}"
				printf '\n'
				continue
			fi

			rm -rf "$out_dir"
			mkdir -p "$out_dir"
			if [[ $use_copy_env -eq 1 ]]; then
				GODOT_DEV_COPY_UPSCALER=1 "${cmd[@]}"
			else
				"${cmd[@]}"
			fi
			rm -f "$out_dir"/*.wav
		done
	done
done

if [[ $matched -eq 0 ]]; then
	echo "upscaler_jobs.sh: no jobs matched filter" >&2
	exit 2
fi
