extends RefCounted

## Path-traced scene reference capture, `--reference=pt` (#263).
##
## The harness settles and reloads the scene as in capture mode, then hands the held
## frame to this script. Two passes run with the same seed: Full, then Direct Only.
## Each pass bumps `rendering/global_illumination/nextgi/debug/reference_epoch`, so the
## scene path (#27) starts from zero samples whatever the reload left in its
## accumulation, and the pass reads the float32 accumulation back at 16 equal sample
## checkpoints. A restart the harness did not ask for means the scene changed, and both
## passes start again.
##
## The folder holds `hdr_<frame>.exr`, `direct/hdr_<frame>.exr`, `mask.png`,
## `patches_q1.json`, `reference.json` and the harness manifest. The per-patch relative
## standard error is the batch-means standard error of the patch's indirect luminance
## (Rec. 709 luminance of full minus direct) over the 16 batches, divided by that
## luminance. Batch b of both passes covers the same sample range, so their noise is
## paired.

const ArgsScript = preload("res://harness/args.gd")

const BATCHES := ArgsScript.PT_BATCHES
const MAX_RESTARTS := 16
const STALL_FRAMES := 600
const MODE_FULL := 1
const MODE_DIRECT := 2
const SETTING_MODE := "rendering/global_illumination/nextgi/debug/reference_mode"
const SETTING_SEED := "rendering/global_illumination/nextgi/debug/reference_seed"
const SETTING_EPOCH := "rendering/global_illumination/nextgi/debug/reference_epoch"
const SETTING_READBACK := "rendering/global_illumination/nextgi/debug/reference_readback"
## Rec. 709, as the Q1 tool's luminance.
const LUMA_R := 0.2126
const LUMA_G := 0.7152
const LUMA_B := 0.0722
## A mask pixel is occluded when a collider is hit this far in front of the disc.
const OCCLUSION_TOLERANCE := 0.02
const SE_METHOD := "batch means over 16 equal sample ranges of the patch's Rec. 709 indirect luminance (full minus direct, same seed and range)"

var spp := 0

## The harness autoload (harness.gd), called dynamically.
var _harness
var _out := ""
var _scene := ""
var _seed := 1
var _frame := 0
var _batch_spp := 0
var _pass := 0
var _awaiting_reset := false
var _resets_before := 0
var _resets_expected := 0
var _checkpoint := 1
var _last_samples := 0
var _stall := 0
var _restarts := 0
var _sums: Array = []
var _batch_means: Array = [[], []]
var _width := 0
var _height := 0
var _exposure := 1.0
var _camera_name := ""
var _patch_path := ""
var _patch_document: Dictionary = {}
var _patches: Array = []
var _patch_pixels: Array = []
var _mask_ids := PackedInt32Array()
var _mask_ready := false
var _done := false


func _init(p_harness: Node, p_out: String, p_scene: String, p_seed: int, p_spp: int, p_frame: int) -> void:
	_harness = p_harness
	_out = p_out
	_scene = p_scene
	_seed = p_seed
	spp = p_spp
	_frame = p_frame
	_batch_spp = spp / BATCHES
	# The accumulation stays off while the harness settles; begin() turns it on.
	ProjectSettings.set_setting(SETTING_MODE, 0)
	ProjectSettings.set_setting(SETTING_READBACK, false)


func begin() -> void:
	_pass = 0
	_batch_means = [[], []]
	_start_pass()


func poll() -> void:
	if _done:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := String(info.get("tier", ""))
	if tier != "HWRT":
		_fail("--reference=pt needs the hardware ray tracing tier (tier=%s reason=%s)" % [tier, String(info.get("reason", ""))])
		return
	if bool(info.get("reference_invalid", false)):
		_fail("the reference path tracer marked this scene invalid (a textured or non-StandardMaterial3D material)")
		return
	var resets := int(info.get("reference_resets", 0))
	var samples := int(info.get("reference_samples", 0))
	if _awaiting_reset:
		if resets == _resets_before:
			_tick_stall(info, samples)
			return
		_awaiting_reset = false
		_resets_expected = resets
	elif resets != _resets_expected:
		_restart("the accumulation restarted at %d samples without a harness request" % _last_samples)
		return
	if samples == _last_samples:
		_tick_stall(info, samples)
		return
	_stall = 0
	_last_samples = samples
	var target := _checkpoint * _batch_spp
	if samples < target:
		return
	if samples > target:
		_restart("the accumulation passed checkpoint %d (%d samples, expected %d)" % [_checkpoint, samples, target])
		return
	var data := _read_accumulation(info, samples)
	if data.is_empty():
		return
	if not _mask_ready and not _build_mask():
		return
	if _sums.is_empty():
		var zeros := PackedFloat64Array()
		zeros.resize(_patches.size())
		_sums.append(zeros)
	_sums.append(patch_luminance_sums(data, _patch_pixels))
	print("harness: reference %s %d/%d spp (batch %d/%d)" % [_pass_name(), samples, spp, _checkpoint, BATCHES])
	_checkpoint += 1
	if _checkpoint > BATCHES:
		_finish_pass(data)


func _pass_name() -> String:
	return "full" if _pass == 0 else "direct"


func _start_pass() -> void:
	ProjectSettings.set_setting(SETTING_MODE, MODE_FULL if _pass == 0 else MODE_DIRECT)
	ProjectSettings.set_setting(SETTING_SEED, _seed)
	ProjectSettings.set_setting(SETTING_READBACK, true)
	ProjectSettings.set_setting(SETTING_EPOCH, int(ProjectSettings.get_setting(SETTING_EPOCH, 0)) + 1)
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	_resets_before = int(info.get("reference_resets", 0))
	_awaiting_reset = true
	_checkpoint = 1
	_last_samples = 0
	_stall = 0
	_sums = []
	print("harness: reference %s pass: %d spp in %d batches, seed %d, frame %d" % [_pass_name(), spp, BATCHES, _seed, _frame])


func _restart(reason: String) -> void:
	_restarts += 1
	if _restarts > MAX_RESTARTS:
		_fail("the reference accumulation restarted %d times; last: %s" % [_restarts, reason])
		return
	print("harness: reference restart %d: %s" % [_restarts, reason])
	_pass = 0
	_batch_means = [[], []]
	_start_pass()


func _tick_stall(info: Dictionary, samples: int) -> void:
	_stall += 1
	if _stall >= STALL_FRAMES:
		_fail("the reference accumulation stayed at %d samples for %d frames (tier=%s reason=%s)" % [samples, STALL_FRAMES, String(info.get("tier", "")), String(info.get("reason", ""))])


func _fail(message: String) -> void:
	_done = true
	ProjectSettings.set_setting(SETTING_MODE, 0)
	ProjectSettings.set_setting(SETTING_READBACK, false)
	_harness.fail_reference(message)


func _read_accumulation(info: Dictionary, samples: int) -> PackedFloat32Array:
	_width = int(info.get("reference_width", 0))
	_height = int(info.get("reference_height", 0))
	_exposure = float(info.get("reference_exposure", 1.0))
	var bytes: PackedByteArray = info.get("reference_accumulation", PackedByteArray())
	if _width <= 0 or _height <= 0 or bytes.size() != _width * _height * 16:
		_restart("the accumulation readback is missing or is not %dx%d" % [_width, _height])
		return PackedFloat32Array()
	if _mask_ready and _mask_ids.size() != _width * _height:
		_fail("the viewport size changed during the reference capture")
		return PackedFloat32Array()
	var data := bytes.to_float32_array()
	if int(data[3]) != samples:
		_restart("the readback holds %d samples, expected %d" % [int(data[3]), samples])
		return PackedFloat32Array()
	return data


func _finish_pass(data: PackedFloat32Array) -> void:
	_batch_means[_pass] = batch_patch_means(_sums, _patch_pixels, _batch_spp, _exposure)
	var path := _image_path(_pass)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var image := mean_image(data, _width, _height, _exposure)
	var err := image.save_exr(path, false, false, -1.0)
	if err != OK:
		_fail("failed to save %s (%s)" % [path, error_string(err)])
		return
	if _pass == 0:
		_pass = 1
		_start_pass()
		return
	_done = true
	ProjectSettings.set_setting(SETTING_MODE, 0)
	ProjectSettings.set_setting(SETTING_READBACK, false)
	if not _write_folder():
		return
	_harness.finish_reference()


func _image_path(p_pass: int) -> String:
	var file := "hdr_%05d.exr" % _frame
	return _out.path_join(file) if p_pass == 0 else _out.path_join("direct").path_join(file)


func _build_mask() -> bool:
	var camera: Camera3D = _harness.get_reference_camera()
	if camera == null:
		_fail("--reference=pt has no active Camera3D")
		return false
	_camera_name = String(camera.name)
	_patch_path = q1_patch_path(_scene)
	if not FileAccess.file_exists(_patch_path):
		_fail("the Q1 patch file %s is missing" % _patch_path)
		return false
	var document = JSON.parse_string(FileAccess.get_file_as_string(_patch_path))
	if typeof(document) != TYPE_DICTIONARY or typeof(document.get("patches")) != TYPE_ARRAY:
		_fail("%s has no patches array" % _patch_path)
		return false
	_patch_document = document
	_mask_ids = PackedInt32Array()
	_mask_ids.resize(_width * _height)
	_mask_ids.fill(-1)
	var view_size := camera.get_viewport().get_visible_rect().size
	var space := camera.get_world_3d().direct_space_state
	for patch in document["patches"]:
		if String(patch.get("camera", "")) != _camera_name:
			continue
		var pixels := _disc_pixels(camera, space, patch, view_size)
		if pixels.is_empty():
			continue
		var patch_id := int(patch["id"])
		for index in pixels:
			_mask_ids[index] = patch_id
		_patches.append(patch)
		_patch_pixels.append(pixels)
	if _patches.is_empty():
		_fail("no Q1 patch of camera %s in %s is visible" % [_camera_name, _patch_path])
		return false
	_mask_ready = true
	var total := 0
	for pixels in _patch_pixels:
		total += (pixels as PackedInt32Array).size()
	print("harness: reference mask %d patches of %s, %d pixels" % [_patches.size(), _camera_name, total])
	return true


## The pixels of one Q1 disc: the camera ray through the pixel center meets the disc
## within its radius, and no collider lies in front of it.
func _disc_pixels(camera: Camera3D, space: PhysicsDirectSpaceState3D, patch: Dictionary, view_size: Vector2) -> PackedInt32Array:
	var pixels := PackedInt32Array()
	var center := _vector(patch["pos"])
	var normal := _vector(patch["normal"]).normalized()
	var radius := float(patch["radius"])
	var helper := Vector3.UP if absf(normal.y) < 0.9 else Vector3.RIGHT
	var u := normal.cross(helper).normalized()
	var v := normal.cross(u)
	var to_pixel := Vector2(float(_width) / view_size.x, float(_height) / view_size.y)
	var lo := Vector2(INF, INF)
	var hi := Vector2(-INF, -INF)
	for k in 32:
		var angle := TAU * float(k) / 32.0
		var point := center + (u * cos(angle) + v * sin(angle)) * radius
		if camera.is_position_behind(point):
			continue
		var screen := camera.unproject_position(point) * to_pixel
		lo = lo.min(screen)
		hi = hi.max(screen)
	if lo.x > hi.x:
		return pixels
	var x0 := clampi(int(floor(lo.x)) - 1, 0, _width - 1)
	var x1 := clampi(int(ceil(hi.x)) + 1, 0, _width - 1)
	var y0 := clampi(int(floor(lo.y)) - 1, 0, _height - 1)
	var y1 := clampi(int(ceil(hi.y)) + 1, 0, _height - 1)
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			var index := y * _width + x
			if _mask_ids[index] != -1:
				continue
			var screen := Vector2((float(x) + 0.5) / to_pixel.x, (float(y) + 0.5) / to_pixel.y)
			var origin := camera.project_ray_origin(screen)
			var direction := camera.project_ray_normal(screen)
			var t := disc_hit_distance(origin, direction, center, normal, radius)
			if t < 0.0:
				continue
			if space != null and t > OCCLUSION_TOLERANCE:
				var query := PhysicsRayQueryParameters3D.create(origin, origin + direction * (t - OCCLUSION_TOLERANCE))
				if not space.intersect_ray(query).is_empty():
					continue
			pixels.append(index)
	return pixels


func _write_folder() -> bool:
	var stats := batch_statistics(_batch_means[0], _batch_means[1])
	var mask := Image.create_from_data(_width, _height, false, Image.FORMAT_RGB8, encode_mask(_mask_ids))
	var err := mask.save_png(_out.path_join("mask.png"))
	if err != OK:
		_fail("failed to save mask.png (%s)" % error_string(err))
		return false
	var patch_ids: Array = []
	for patch in _patches:
		patch_ids.append(int(patch["id"]))
	var patch_file := {
		"version": _patch_document.get("version", 1),
		"scene": _patch_document.get("scene", ""),
		"seed": _patch_document.get("seed", 0),
		"camera": _camera_name,
		"source": _patch_path,
		"count": _patches.size(),
		"patches": _patches,
	}
	if not _write_json(_out.path_join("patches_q1.json"), patch_file):
		return false
	var relative: Array = stats["relative_se"]
	var worst := 0.0
	var finite := true
	for value in relative:
		if value == null:
			finite = false
		else:
			worst = maxf(worst, float(value))
	var version := Engine.get_version_info()
	var reference := {
		"schema": 1,
		"kind": "pt",
		"scene": _scene,
		"camera": _camera_name,
		"frame": _frame,
		"seed": _seed,
		"spp": spp,
		"direct_spp": spp,
		"batches": BATCHES,
		"batch_spp": _batch_spp,
		"resolution": [_width, _height],
		"exposure": _exposure,
		"restarts": _restarts,
		"device": {
			"host": String(_harness.get_host_label()),
			"adapter": RenderingServer.get_video_adapter_name(),
			"api_version": RenderingServer.get_video_adapter_api_version(),
			"driver": RenderingServer.get_current_rendering_driver_name(),
		},
		"commit": String(version.get("hash", "")),
		"date": Time.get_datetime_string_from_system(true),
		"files": {
			"full": _image_path(0).trim_prefix(_out).trim_prefix("/"),
			"direct": _image_path(1).trim_prefix(_out).trim_prefix("/"),
			"mask": "mask.png",
			"patches": "patches_q1.json",
		},
		"patch_ids": patch_ids,
		"patch_indirect_luminance": Array(stats["luminance"]),
		"per_patch_relative_se": relative,
		"max_relative_se": worst if finite else null,
		"se_method": SE_METHOD,
	}
	if not _write_json(_out.path_join("reference.json"), reference):
		return false
	print("harness: reference complete spp=%d patches=%d max_relative_se=%s" % [spp, _patches.size(), str(worst) if finite else "non-finite"])
	return true


func _write_json(path: String, payload: Dictionary) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("cannot write %s" % path)
		return false
	file.store_string(JSON.stringify(payload, "\t", true, true))
	file.store_string("\n")
	file.close()
	return true


static func _vector(value: Variant) -> Vector3:
	var values: Array = value
	return Vector3(float(values[0]), float(values[1]), float(values[2]))


## `res://scenes/x/cbr_a.tscn` keeps its Q1 patches in `res://scenes/x/cbr_a_patches_q1.json`.
static func q1_patch_path(scene: String) -> String:
	return scene.get_base_dir().path_join(scene.get_file().get_basename() + "_patches_q1.json")


## Distance along the ray to the disc, or -1 when the ray misses it or sees its back.
static func disc_hit_distance(origin: Vector3, direction: Vector3, center: Vector3, normal: Vector3, radius: float) -> float:
	var facing := direction.dot(normal)
	if facing >= -1e-6:
		return -1.0
	var t := (center - origin).dot(normal) / facing
	if t <= 0.0:
		return -1.0
	if (origin + direction * t).distance_to(center) > radius:
		return -1.0
	return t


## Sum over each patch's pixels of the Rec. 709 luminance of the radiance sums.
static func patch_luminance_sums(data: PackedFloat32Array, patch_pixels: Array) -> PackedFloat64Array:
	var sums := PackedFloat64Array()
	sums.resize(patch_pixels.size())
	for p in patch_pixels.size():
		var total := 0.0
		for index in patch_pixels[p]:
			var base: int = index * 4
			total += LUMA_R * data[base] + LUMA_G * data[base + 1] + LUMA_B * data[base + 2]
		sums[p] = total
	return sums


## Per-batch patch mean luminance from the checkpoint sums (index 0 is zero samples).
static func batch_patch_means(sums: Array, patch_pixels: Array, batch_spp: int, exposure: float) -> Array:
	var means: Array = []
	for b in range(1, sums.size()):
		var current: PackedFloat64Array = sums[b]
		var previous: PackedFloat64Array = sums[b - 1]
		var batch := PackedFloat64Array()
		batch.resize(current.size())
		for p in current.size():
			var pixel_count := (patch_pixels[p] as PackedInt32Array).size()
			batch[p] = (current[p] - previous[p]) / (float(batch_spp) * float(pixel_count)) * exposure
		means.append(batch)
	return means


## Indirect = full - direct per batch. Returns the mean indirect luminance per patch and
## its relative standard error (sample std over batches / sqrt(batches) / |mean|; null
## when the mean is 0 and the batches differ).
static func batch_statistics(full: Array, direct: Array) -> Dictionary:
	var batches := full.size()
	var patches := (full[0] as PackedFloat64Array).size() if batches > 0 else 0
	var luminance := PackedFloat64Array()
	luminance.resize(patches)
	var relative: Array = []
	for p in patches:
		var values := PackedFloat64Array()
		values.resize(batches)
		var total := 0.0
		for b in batches:
			values[b] = (full[b] as PackedFloat64Array)[p] - (direct[b] as PackedFloat64Array)[p]
			total += values[b]
		var mean := total / float(batches)
		var squares := 0.0
		for b in batches:
			squares += (values[b] - mean) * (values[b] - mean)
		var standard_error := sqrt(squares / float(batches - 1) / float(batches)) if batches > 1 else INF
		luminance[p] = mean
		if absf(mean) > 0.0:
			relative.append(standard_error / absf(mean))
		elif standard_error == 0.0:
			relative.append(0.0)
		else:
			relative.append(null)
	return {"luminance": luminance, "relative_se": relative}


## RGB float image of sum / count times the exposure, saved as half floats.
static func mean_image(data: PackedFloat32Array, width: int, height: int, exposure: float) -> Image:
	var rgb := PackedFloat32Array()
	rgb.resize(width * height * 3)
	for i in width * height:
		var weight := data[i * 4 + 3]
		var scale := exposure / weight if weight > 0.0 else 0.0
		rgb[i * 3] = data[i * 4] * scale
		rgb[i * 3 + 1] = data[i * 4 + 1] * scale
		rgb[i * 3 + 2] = data[i * 4 + 2] * scale
	var image := Image.create_from_data(width, height, false, Image.FORMAT_RGBF, rgb.to_byte_array())
	image.convert(Image.FORMAT_RGBH)
	return image


## The Q1 tool's mask encoding: R + 256 * G = patch id + 1, 0 = no patch.
static func encode_mask(ids: PackedInt32Array) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(ids.size() * 3)
	for i in ids.size():
		var encoded := ids[i] + 1
		bytes[i * 3] = encoded & 0xFF
		bytes[i * 3 + 1] = (encoded >> 8) & 0xFF
		bytes[i * 3 + 2] = 0
	return bytes
