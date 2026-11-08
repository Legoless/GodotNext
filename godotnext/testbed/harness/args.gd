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
const FLAG_VELOCITY_DUMP := "--velocity-dump"
const FLAG_REFERENCE := "--reference"
const FLAG_MSAA := "--msaa"
const FLAG_MASK := "--mask"
const FLAG_UPSCALER := "--upscaler"
const FLAG_SCALE := "--scale"

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
	FLAG_VELOCITY_DUMP: true,
	FLAG_REFERENCE: true,
	FLAG_MSAA: true,
	FLAG_MASK: true,
	FLAG_UPSCALER: true,
	FLAG_SCALE: true,
}

const VALID_MASKS := {
	"alpha_particles": true,
	"scrolling_emissive": true,
}

const VALID_UPSCALERS := {
	"taa": true,
	"fsr2": true,
	"copy": true,
	"dlss": true,
	"metalfx": true,
	"tsr": true,
}


static func parse(user_args: PackedStringArray, engine_args: PackedStringArray = PackedStringArray()) -> Dictionary:
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
		if not has_value and key == FLAG_VELOCITY_DUMP:
			value = "1"
			has_value = true
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
	var scene := ""
	if values.has(FLAG_SCENE):
		scene = String(values[FLAG_SCENE])
	else:
		scene = _infer_engine_scene(engine_args)
	if scene.is_empty():
		result.ok = false
		result.error = "missing --scene"
		return result
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
	var frames := -1
	if values.has(FLAG_FRAMES):
		frames = _parse_non_negative_int(String(values[FLAG_FRAMES]))
	elif mode == "capture" and _has_engine_quit_after(engine_args):
		frames = 600
	else:
		result.ok = false
		result.error = "missing --frames"
		return result
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
	if values.has(FLAG_VELOCITY_DUMP):
		var vd := String(values[FLAG_VELOCITY_DUMP])
		if vd != "0" and vd != "1":
			result.ok = false
			result.error = "--velocity-dump must be 0 or 1"
			return result
	if values.has(FLAG_REFERENCE) and String(values[FLAG_REFERENCE]) != "ssaa":
		result.ok = false
		result.error = "--reference must be ssaa"
		return result
	if values.has(FLAG_MSAA):
		var msaa := String(values[FLAG_MSAA])
		if msaa != "4" and msaa != "8":
			result.ok = false
			result.error = "--msaa must be 4 or 8"
			return result
	if values.has(FLAG_MASK) and not VALID_MASKS.has(String(values[FLAG_MASK])):
		result.ok = false
		result.error = "--mask must be alpha_particles or scrolling_emissive"
		return result
	if values.has(FLAG_UPSCALER) and not VALID_UPSCALERS.has(String(values[FLAG_UPSCALER])):
		result.ok = false
		result.error = "--upscaler must be taa, fsr2, copy, dlss, metalfx, or tsr"
		return result
	if values.has(FLAG_SCALE):
		var scale_text := String(values[FLAG_SCALE])
		if not scale_text.is_valid_float():
			result.ok = false
			result.error = "--scale must be a number in [0.25, 2.0]"
			return result
		var scale_val := float(scale_text)
		if scale_val < 0.25 or scale_val > 2.0:
			result.ok = false
			result.error = "--scale must be a number in [0.25, 2.0]"
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
		"output_resolution": _infer_engine_resolution(engine_args),
	}
	for key in [FLAG_VELOCITY_DUMP, FLAG_REFERENCE, FLAG_MSAA, FLAG_MASK, FLAG_UPSCALER, FLAG_SCALE]:
		if values.has(key):
			var normalized := String(key).trim_prefix("--").replace("-", "_")
			result["args"][normalized] = float(values[key]) if key == FLAG_SCALE else values[key]
	return result


static func _infer_engine_scene(engine_args: PackedStringArray) -> String:
	for token_variant in engine_args:
		var token := String(token_variant)
		if token == "--":
			break
		if token.begins_with("res://") and (token.ends_with(".tscn") or token.ends_with(".scn")):
			return token
	return ""


static func _has_engine_quit_after(engine_args: PackedStringArray) -> bool:
	var index := 0
	while index < engine_args.size():
		var token := String(engine_args[index])
		if token == "--":
			break
		if token == "--quit-after" and index + 1 < engine_args.size():
			return _parse_non_negative_int(String(engine_args[index + 1])) > 0
		if token.begins_with("--quit-after="):
			return _parse_non_negative_int(token.trim_prefix("--quit-after=")) > 0
		index += 1
	return false


static func _infer_engine_resolution(engine_args: PackedStringArray) -> Array:
	var index := 0
	while index < engine_args.size():
		var token := String(engine_args[index])
		if token == "--":
			break
		var spec := ""
		if token == "--resolution" and index + 1 < engine_args.size():
			spec = String(engine_args[index + 1])
		elif token.begins_with("--resolution="):
			spec = token.trim_prefix("--resolution=")
		if not spec.is_empty():
			var parts := spec.split("x")
			if parts.size() == 2 and parts[0].is_valid_int() and parts[1].is_valid_int():
				var width := int(parts[0])
				var height := int(parts[1])
				if width > 0 and height > 0:
					return [width, height]
		index += 1
	return []


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
