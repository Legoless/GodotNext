#!/usr/bin/env bash
# Check of GODOTNEXT_BACKGROUND=1 on the Apple M5 Max: a rendered (non-headless) run must draw
# frames on Metal without ever becoming the frontmost app. Don't switch apps while it runs.
# A relative Godot binary path is resolved from the repository root.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)" || exit 1
cd "$repo_root" || exit 1

if [[ $# -gt 1 ]]; then
	printf 'usage: %s [godot-binary]\n' "$0" >&2
	exit 2
fi
godot="${1:-bin/godot.macos.editor.arm64}"
[[ -x "$godot" ]] || {
	printf 'Godot binary not found: %s\n' "$godot" >&2
	exit 2
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat >"$work/project.godot" <<'EOF'
config_version=5

[application]
config/name="godotnext_background_check"
EOF

cat >"$work/check.gd" <<'EOF'
extends SceneTree

var frames := 0


func _initialize() -> void:
	var box := MeshInstance3D.new()
	box.mesh = BoxMesh.new()
	get_root().add_child(box)
	var light := OmniLight3D.new()
	light.position = Vector3(0.0, 1.0, 3.0)
	get_root().add_child(light)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 3.0)
	get_root().add_child(camera)
	camera.make_current()


func _process(_delta: float) -> bool:
	frames += 1
	if frames == 60:
		var image := get_root().get_texture().get_image()
		var center := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		print("GODOTNEXT_BACKGROUND_CHECK {\"rendered\":%s}" % str(center.r > 0.05).to_lower())
		quit()
	return false
EOF

front_before="$(lsappinfo front)"
GODOTNEXT_BACKGROUND=1 "$godot" --path "$work" --rendering-driver metal --resolution 320x180 --script res://check.gd >"$work/godot.log" 2>&1 &
godot_pid=$!

focus_lost=0
while kill -0 "$godot_pid" 2>/dev/null; do
	if [[ "$(lsappinfo front)" != "$front_before" ]]; then
		focus_lost=1
	fi
	perl -e 'select(undef, undef, undef, 0.2)'
done
wait "$godot_pid" || {
	cat "$work/godot.log" >&2
	printf 'FAIL: Godot exited with an error.\n' >&2
	exit 1
}

if ! grep -q 'GODOTNEXT_BACKGROUND_CHECK {"rendered":true}' "$work/godot.log"; then
	cat "$work/godot.log" >&2
	printf 'FAIL: no frame was rendered.\n' >&2
	exit 1
fi
if [[ "$focus_lost" -ne 0 ]]; then
	printf 'FAIL: the frontmost app changed during the run.\n' >&2
	exit 1
fi
printf 'PASS: frames rendered on Metal, and the frontmost app never changed.\n'
