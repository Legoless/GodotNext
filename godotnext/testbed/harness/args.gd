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
## --lights (#233): MegaLights Hall's light count (64, 1024 or 4096), read by the scene.
const FLAG_LIGHTS := "--lights"
const FLAG_VELOCITY_DUMP := "--velocity-dump"
const FLAG_REFERENCE := "--reference"
const FLAG_MSAA := "--msaa"
const FLAG_MASK := "--mask"
const FLAG_UPSCALER := "--upscaler"
const FLAG_SCALE := "--scale"
const FLAG_SPP := "--spp"
const FLAG_TSR_QUALITY := "--tsr-quality"
## --nextgi=on (#229): NextGI forced on in the scene's Environment for this run, for the UE
## comparison of a scene whose committed Environment leaves it off.
const FLAG_NEXTGI := "--nextgi"
## Dense City flight/GV-3 (#231): scene-local deterministic runtime switches.
const FLAG_FLIGHT := "--flight"
const FLAG_STORM := "--storm"

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
	FLAG_LIGHTS: true,
	FLAG_VELOCITY_DUMP: true,
	FLAG_REFERENCE: true,
	FLAG_MSAA: true,
	FLAG_MASK: true,
	FLAG_UPSCALER: true,
	FLAG_SCALE: true,
	FLAG_SPP: true,
	FLAG_TSR_QUALITY: true,
	FLAG_NEXTGI: true,
	FLAG_FLIGHT: true,
	FLAG_STORM: true,
}

const VALID_MASKS := {
	"alpha_particles": true,
	"scrolling_emissive": true,
	"moire": true,
	"thin_wire": true,
}

## --tsr-quality (#190): the Viewport.tsr_quality preset for --upscaler=tsr runs.
const VALID_TSR_QUALITIES := {
	"low": Viewport.TSR_QUALITY_LOW,
	"medium": Viewport.TSR_QUALITY_MEDIUM,
	"high": Viewport.TSR_QUALITY_HIGH,
	"epic": Viewport.TSR_QUALITY_EPIC,
	"cinematic": Viewport.TSR_QUALITY_CINEMATIC,
}

const VALID_REFERENCES := {
	"ssaa": true,
	"pt": true,
}

## --reference=pt (#263): the path-traced reference accumulates in 16 equal sample batches
## of whole 32-sample frames, so --spp is a multiple of 16 * 32.
const PT_BATCHES := 16
const PT_SPP_STEP := 512
const PT_DEFAULT_SPP := 4096

const VALID_UPSCALERS := {
	"taa": true,
	"fsr2": true,
	"copy": true,
	"dlss": true,
	"metalfx": true,
	"tsr": true,
	"custom": true,
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
	var reference := String(values.get(FLAG_REFERENCE, ""))
	if values.has(FLAG_REFERENCE) and not VALID_REFERENCES.has(reference):
		result.ok = false
		result.error = "--reference must be ssaa or pt"
		return result
	var frames := -1
	if values.has(FLAG_FRAMES):
		frames = _parse_non_negative_int(String(values[FLAG_FRAMES]))
	elif reference == "pt":
		# The path-traced reference runs until it reaches --spp; --frames does not apply.
		frames = 1
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
	var lights := -1
	if values.has(FLAG_LIGHTS):
		lights = _parse_non_negative_int(String(values[FLAG_LIGHTS]))
		if lights < 1:
			result.ok = false
			result.error = "--lights must be a positive integer"
			return result
	if values.has(FLAG_VELOCITY_DUMP):
		var vd := String(values[FLAG_VELOCITY_DUMP])
		if vd != "0" and vd != "1":
			result.ok = false
			result.error = "--velocity-dump must be 0 or 1"
			return result
	var spp := -1
	if reference == "pt":
		if mode != "capture":
			result.ok = false
			result.error = "--reference=pt requires --mode=capture"
			return result
		for key in [FLAG_UPSCALER, FLAG_MSAA, FLAG_SCALE, FLAG_MASK, FLAG_CAPTURE_FRAMES, FLAG_VELOCITY_DUMP, FLAG_TSR_QUALITY]:
			if values.has(key):
				result.ok = false
				result.error = "%s does not apply to --reference=pt" % key
				return result
		spp = PT_DEFAULT_SPP
		if values.has(FLAG_SPP):
			spp = _parse_non_negative_int(String(values[FLAG_SPP]))
			if spp < PT_SPP_STEP or spp % PT_SPP_STEP != 0:
				result.ok = false
				result.error = "--spp must be a positive multiple of %d" % PT_SPP_STEP
				return result
	elif values.has(FLAG_SPP):
		result.ok = false
		result.error = "--spp requires --reference=pt"
		return result
	if values.has(FLAG_MSAA):
		var msaa := String(values[FLAG_MSAA])
		if msaa != "4" and msaa != "8":
			result.ok = false
			result.error = "--msaa must be 4 or 8"
			return result
	if values.has(FLAG_MASK) and not VALID_MASKS.has(String(values[FLAG_MASK])):
		result.ok = false
		result.error = "--mask must be alpha_particles, scrolling_emissive, moire, or thin_wire"
		return result
	if values.has(FLAG_UPSCALER) and not VALID_UPSCALERS.has(String(values[FLAG_UPSCALER])):
		result.ok = false
		result.error = "--upscaler must be taa, fsr2, copy, dlss, metalfx, tsr, or custom"
		return result
	if values.has(FLAG_TSR_QUALITY):
		if not VALID_TSR_QUALITIES.has(String(values[FLAG_TSR_QUALITY])):
			result.ok = false
			result.error = "--tsr-quality must be low, medium, high, epic, or cinematic"
			return result
		if String(values.get(FLAG_UPSCALER, "")) != "tsr":
			result.ok = false
			result.error = "--tsr-quality requires --upscaler=tsr"
			return result
	for key in [FLAG_FLIGHT, FLAG_STORM]:
		if values.has(key) and String(values[key]) not in ["0", "1"]:
			result.ok = false
			result.error = "%s must be 0 or 1" % key
			return result
	if values.has(FLAG_NEXTGI):
		if String(values[FLAG_NEXTGI]) != "on":
			result.ok = false
			result.error = "--nextgi must be on"
			return result
		if reference == "pt":
			result.ok = false
			result.error = "--nextgi does not apply to --reference=pt"
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
		"lights": lights,
		"output_resolution": _infer_engine_resolution(engine_args),
	}
	if spp > 0:
		result["args"]["spp"] = spp
	for key in [FLAG_VELOCITY_DUMP, FLAG_REFERENCE, FLAG_MSAA, FLAG_MASK, FLAG_UPSCALER, FLAG_SCALE, FLAG_TSR_QUALITY, FLAG_NEXTGI, FLAG_FLIGHT, FLAG_STORM]:
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
