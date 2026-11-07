extends Node3D

const SceneArgs := preload("res://scenes/scene_args.gd")

var _hold := -1
var _local_frame := 0
var _saw := false
var _reset_fired := false
var _locked_time_offset := false
var _logged_occluder_start := false
var _logged_occluder_end := false

var _camera: Camera3D
var _path: PathFollow3D
var _occluders: Node3D
var _fan: Node3D
var _player: AnimationPlayer
var _emissive: MeshInstance3D


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	process_priority = 1
	_hold = SceneArgs.int_value("--hold-frame", -1)
	_camera = get_node("CameraPath/CameraFollow/Camera3D") as Camera3D
	_path = get_node("CameraPath/CameraFollow") as PathFollow3D
	_occluders = get_node("Occluders") as Node3D
	_fan = get_node("Occluders/Fan") as Node3D
	_player = get_node("Occluders/SkinnedCharacter/AnimationPlayer") as AnimationPlayer
	_emissive = get_node("ScrollingEmissive") as MeshInstance3D
	if _player != null:
		_player.play("swing")
		_player.seek(0.0, true)
	_apply_frame(0, true)


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var harness := get_node_or_null("/root/Harness")
	var settling := harness != null and harness.has_method("is_settling") and bool(harness.call("is_settling"))
	var driving := harness != null and harness.has_method("is_driving_animation") and bool(harness.call("is_driving_animation"))
	if driving and not _locked_time_offset:
		_locked_time_offset = true
		if _emissive != null and _emissive.material_override is ShaderMaterial:
			var shader_time := float(Engine.get_frames_drawn() + 1) / 60.0
			(_emissive.material_override as ShaderMaterial).set_shader_parameter("time_offset", -shader_time * 2.0)
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply_frame(int(stepped["frame"]), settling)


func _apply_frame(frame: int, settling: bool) -> void:
	if frame < 200:
		_path.progress_ratio = 0.0
	elif frame < 400:
		_path.progress_ratio = float(frame - 200) / 199.0 * 0.45
	else:
		var post_cut := clampf(float(frame - 400) / 199.0, 0.0, 1.0)
		_path.progress_ratio = 0.78 + post_cut * 0.22
	_camera.look_at(Vector3(0.0, 1.1, -0.4), Vector3.UP)

	# Fan spins in screen plane (around Z) while character limbs swing deterministically.
	_fan.rotation.z = float(frame) * 0.24
	if _player != null:
		var anim_time := fposmod(float(frame) / 60.0 * 3.0, 0.5)
		_player.seek(anim_time, true)

	if frame < 300:
		_occluders.position.x = -1.35
	elif frame <= 340:
		_occluders.position.x = lerpf(-1.35, 1.35, float(frame - 300) / 40.0)
		if not settling and frame == 300 and not _logged_occluder_start:
			_logged_occluder_start = true
			print("UPSCALER_TORTURE_OCCLUDER frame=300")
		elif not settling and frame == 340 and not _logged_occluder_end:
			_logged_occluder_end = true
			print("UPSCALER_TORTURE_OCCLUDER frame=340")
	else:
		_occluders.position.x = 1.35

	if not settling and frame == 400 and not _reset_fired:
		_reset_fired = true
		print("UPSCALER_TORTURE_RESET frame=400")
		var viewport := get_viewport()
		if viewport != null:
			viewport.reset_temporal_history()
