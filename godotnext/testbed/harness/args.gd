extends RefCounted

## Parses the harness arguments that follow `--` on the Godot command line.

const FLAG_MODE := "--mode"
const FLAG_SCENE := "--scene"
const FLAG_CAMERA := "--camera"
const FLAG_OUT := "--out"
const FLAG_SEED := "--seed"
const FLAG_FRAMES := "--frames"
const FLAG_WARMUP := "--warmup"
const FLAG_CAPTURE_FRAMES := "--capture-frames"
const FLAG_VARIANT := "--variant"
const FLAG_HOLD_FRAME := "--hold-frame"
const FLAG_INSTANCES := "--instances"

const KNOWN_FLAGS := {
	FLAG_MODE: true,
	FLAG_SCENE: true,
	FLAG_CAMERA: true,
	FLAG_OUT: true,
	FLAG_SEED: true,
	FLAG_FRAMES: true,
	FLAG_WARMUP: true,
	FLAG_CAPTURE_FRAMES: true,
	FLAG_VARIANT: true,
	FLAG_HOLD_FRAME: true,
	FLAG_INSTANCES: true,
}


static func parse(user_args: PackedStringArray) -> Dictionary:
	var result := {
		"ok": true,
		"error": "",
		"present": false,
		"args": {},
	}
	var values := {}
	var unknown: PackedStringArray = []
	var index := 0
	while index < user_args.size():
		var token := String(user_args[index])
		if not token.begins_with("--"):
			result.ok = false
			result.error = "unexpected argument '%s'" % token
			return result
		var key := token
		var has_value := false
		var value := ""
		var equals_at := token.find("=")
		if equals_at != -1:
			key = token.substr(0, equals_at)
			value = token.substr(equals_at + 1)
			has_value = true
		if not KNOWN_FLAGS.has(key):
			unknown.append(key)
			if not has_value and index + 1 < user_args.size() and not String(user_args[index + 1]).begins_with("--"):
				index += 1
			index += 1
			continue
		if not has_value:
			if index + 1 >= user_args.size() or String(user_args[index + 1]).begins_with("--"):
				result.ok = false
				result.error = "missing value for %s" % key
				return result
			value = String(user_args[index + 1])
			index += 1
		if values.has(key):
			result.ok = false
			result.error = "duplicate argument %s" % key
			return result
		values[key] = value
		index += 1

	if not values.has(FLAG_MODE):
		# Other testbed scenes may pass their own user arguments. Stay idle
		# unless this invocation is actually a harness run.
		result.present = false
		result.ok = true
		return result
	result.present = true
	if unknown.size() > 0:
		result.ok = false
		result.error = "unknown argument '%s'" % unknown[0]
		return result

	var mode := String(values[FLAG_MODE])
	if mode != "capture" and mode != "timing":
		result.ok = false
		result.error = "--mode must be capture or timing"
		return result
	if not values.has(FLAG_SCENE):
		result.ok = false
		result.error = "missing --scene"
		return result
	var scene := String(values[FLAG_SCENE])
	if not scene.begins_with("res://"):
		result.ok = false
		result.error = "--scene must be a res:// path"
		return result
	if not values.has(FLAG_OUT):
		result.ok = false
		result.error = "missing --out"
		return result
	var out_dir := String(values[FLAG_OUT])
	if not _is_filesystem_absolute(out_dir):
		result.ok = false
		result.error = "--out must be an absolute filesystem directory"
		return result
	if not values.has(FLAG_FRAMES):
		result.ok = false
		result.error = "missing --frames"
		return result
	var frames := _parse_non_negative_int(String(values[FLAG_FRAMES]))
	if frames < 1:
		result.ok = false
		result.error = "--frames must be a positive integer"
		return result
	var warmup := 0
	if values.has(FLAG_WARMUP):
		warmup = _parse_non_negative_int(String(values[FLAG_WARMUP]))
		if warmup < 0:
			result.ok = false
			result.error = "--warmup must be a non-negative integer"
			return result
	var seed_value := 1
	if values.has(FLAG_SEED):
		if not String(values[FLAG_SEED]).is_valid_int():
			result.ok = false
			result.error = "--seed must be an integer"
			return result
		seed_value = int(values[FLAG_SEED])
	var capture_frames: Array = []
	if values.has(FLAG_CAPTURE_FRAMES):
		var parsed_frames := _parse_frame_list(String(values[FLAG_CAPTURE_FRAMES]))
		if not bool(parsed_frames["ok"]):
			result.ok = false
			result.error = String(parsed_frames["error"])
			return result
		capture_frames = parsed_frames["frames"]
	var camera := ""
	if values.has(FLAG_CAMERA):
		camera = String(values[FLAG_CAMERA])
	var variant := ""
	if values.has(FLAG_VARIANT):
		variant = String(values[FLAG_VARIANT])
	var hold_frame := -1
	if values.has(FLAG_HOLD_FRAME):
		hold_frame = _parse_non_negative_int(String(values[FLAG_HOLD_FRAME]))
		if hold_frame < 0:
			result.ok = false
			result.error = "--hold-frame must be a non-negative integer"
			return result
	var instances := -1
	if values.has(FLAG_INSTANCES):
		instances = _parse_non_negative_int(String(values[FLAG_INSTANCES]))
		if instances < 1:
			result.ok = false
			result.error = "--instances must be a positive integer"
			return result

	result.args = {
		"mode": mode,
		"scene": scene,
		"camera": camera,
		"out": out_dir,
		"seed": seed_value,
		"frames": frames,
		"warmup": warmup,
		"capture_frames": capture_frames,
		"variant": variant,
		"hold_frame": hold_frame,
		"instances": instances,
	}
	return result


static func _is_filesystem_absolute(path: String) -> bool:
	if path.begins_with("res://") or path.begins_with("user://"):
		return false
	return path.is_absolute_path()


static func _parse_non_negative_int(text: String) -> int:
	if not text.is_valid_int():
		return -1
	var value := int(text)
	if value < 0:
		return -1
	return value


static func _parse_frame_list(text: String) -> Dictionary:
	var frames: Array = []
	if text.strip_edges().is_empty():
		return {"ok": true, "error": "", "frames": frames}
	for part in text.split(","):
		var token := String(part).strip_edges()
		if token.is_empty() or not token.is_valid_int():
			return {"ok": false, "error": "--capture-frames must be a comma-separated list of integers", "frames": []}
		var value := int(token)
		if value < 0:
			return {"ok": false, "error": "--capture-frames values must be non-negative", "frames": []}
		frames.append(value)
	return {"ok": true, "error": "", "frames": frames}
