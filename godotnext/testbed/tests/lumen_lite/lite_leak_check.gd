extends Node

## #183, #188: Lumen Lite does not carry a lit room's light through a wall into the dark room
## beside it. Two closed rooms, 6 x 3 x 6 m each, share one 0.4 m wall; an omni light (shadows on)
## lights only the west room, so the east room's floor sees nothing but what leaks. The Lite field
## (DEBUG_DRAW_NEXTGI_LITE_PROBES) is measured on each room's floor FLOOR_OFFSET from the wall,
## from a camera looking down in that room: the dark room first, from the start of the run, then
## the lit room. The field keeps its history for hundreds of frames after a camera cut, so the
## dark room is measured before the camera has ever seen the lit room, and the lit room after
## LIT_FRAMES more frames, still converging: its value is a lower bound, which makes the ratio
## an upper bound. A view per room at once did not separate them while the second view read the
## probes on a grid snapped to its own camera instead of the WRC's (the dark view read 0.96 of the
## lit one); tests/lite_two_views now checks two views at once, and one view keeps this check's
## numbers comparable. Both tiers first wait until the voxel-lighting volume has finished filling
## (voxel_lighting_retrace 0 for SETTLE_FRAMES frames, within MAX_FRAMES): the software tier's WRC
## probes shade their SDF hits from it. Each room is sampled MEASURE_SAMPLES times and averaged.
## With --expected-tier=HWRT it prints the reference ("LITE_LEAK_REFERENCE ratio=R"). With
## --expected-tier=SWRT and --hwrt-ratio=R it asserts the software tier's dark/lit ratio is at
## most LEAK_FACTOR x R, and that the lit floor is lit (MIN_LIT_LUMA). On the
## M5 Max both tiers measure about 0.0002, so the bound is about 0.002. Light that crosses the
## wall fails it: with the software relight's SDF shadow rays never occluded the ratio was 0.053.
## The software Lite field without the WRC moments' visibility and the push-out (camera.w = 0)
## measured 0.008, above that bound. run_lite_leak_check.sh runs the hardware pass
## first and passes its ratio; on a device without hardware ray query it passes the ratio recorded
## on the M5 Max instead. Prints one "GODOTNEXT_RESULT <json>" line.

const VIEW := Vector2i(960, 540)
const WARMUP_FRAMES := 240
const SETTLE_FRAMES := 30
const MAX_FRAMES := 5000
const MEASURE_FRAMES := 120
const LIT_FRAMES := 600
const MEASURE_SAMPLES := 8
const LEAK_FACTOR := 10.0
const MIN_LIT_LUMA := 0.002
const WALL_THICKNESS := 0.4
## The measured floor patch's distance from the shared wall's face.
const FLOOR_OFFSET := 0.5

var expected_tier := "SWRT"
var hwrt_ratio := -1.0
var viewport: SubViewport
var camera: Camera3D
var frames := 0
var phase := 0
var phase_frame := 0
var settle_count := 0
var lit_samples := PackedFloat32Array()
var dark_samples := PackedFloat32Array()
var done := false


func _ready() -> void:
	# run_lite_leak_check.sh takes the recorded ratio only when the device has no hardware ray
	# query; on any other device a reference pass that ends without a result is a failure.
	var device := RenderingServer.get_rendering_device()
	var hardware := device != null and device.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
	print("LITE_LEAK_DEVICE ray_query_hardware=%s" % ("true" if hardware else "false"))
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expected-tier="):
			expected_tier = arg.trim_prefix("--expected-tier=")
		elif arg.begins_with("--hwrt-ratio="):
			hwrt_ratio = float(arg.trim_prefix("--hwrt-ratio="))
	if expected_tier != "SWRT" and expected_tier != "HWRT":
		_finish(["--expected-tier must be SWRT or HWRT"])
		return
	if expected_tier == "SWRT" and hwrt_ratio <= 0.0:
		_finish(["--hwrt-ratio=R (the hardware tier's dark/lit ratio, above 0) is required with --expected-tier=SWRT"])
		return
	viewport = SubViewport.new()
	viewport.size = VIEW
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.use_hdr_2d = true
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.use_taa = false
	viewport.debug_draw = Viewport.DEBUG_DRAW_NEXTGI_LITE_PROBES
	add_child(viewport)
	var root := Node3D.new()
	viewport.add_child(root)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.BLACK
	environment.ambient_light_energy = 0.0
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	root.add_child(world)
	# x from -6.2 to 6.2, z from -3.2 to 3.2, y from -0.2 to 3.2: floor, ceiling, outer walls and
	# the shared wall at x = 0.
	_box(root, Vector3(0, -0.1, 0), Vector3(12.4, 0.2, 6.4))
	_box(root, Vector3(0, 3.1, 0), Vector3(12.4, 0.2, 6.4))
	_box(root, Vector3(-6.1, 1.5, 0), Vector3(0.2, 3.0, 6.4))
	_box(root, Vector3(6.1, 1.5, 0), Vector3(0.2, 3.0, 6.4))
	_box(root, Vector3(0, 1.5, -3.1), Vector3(12.4, 3.0, 0.2))
	_box(root, Vector3(0, 1.5, 3.1), Vector3(12.4, 3.0, 0.2))
	_box(root, Vector3(0, 1.5, 0), Vector3(WALL_THICKNESS, 3.0, 6.0))
	var light := OmniLight3D.new()
	light.position = Vector3(-2.0, 2.0, 0.0)
	light.omni_range = 8.0
	light.light_energy = 2.0
	light.shadow_enabled = true
	root.add_child(light)
	camera = Camera3D.new()
	camera.fov = 60.0
	root.add_child(camera)
	_aim(1.0)


func _box(p_root: Node3D, p_center: Vector3, p_size: Vector3) -> void:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = p_center
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	p_root.add_child(instance)


# Looks straight down at the floor FLOOR_OFFSET from the shared wall, from 1.2 m above it, on the
# side p_side (-1 the lit room, 1 the dark room).
func _aim(p_side: float) -> void:
	var target := Vector3(p_side * (WALL_THICKNESS * 0.5 + FLOOR_OFFSET), 0.0, 0.0)
	camera.look_at_from_position(target + Vector3(0, 1.2, 0), target, Vector3(0, 0, -1))
	camera.current = true


# The mean luminance of the field over the middle quarter of the view (the floor patch).
func _patch_luma() -> float:
	var image := viewport.get_texture().get_image()
	var sum := 0.0
	var count := 0
	for y in range(image.get_height() * 3 / 8, image.get_height() * 5 / 8, 4):
		for x in range(image.get_width() * 3 / 8, image.get_width() * 5 / 8, 4):
			sum += image.get_pixel(x, y).get_luminance()
			count += 1
	return sum / float(maxi(count, 1))


func _mean(p_values: PackedFloat32Array) -> float:
	var sum := 0.0
	for value in p_values:
		sum += value
	return sum / float(maxi(p_values.size(), 1))


func _process(_delta: float) -> void:
	if done:
		return
	frames += 1
	phase_frame += 1
	if phase == 0:
		if frames < WARMUP_FRAMES:
			return
		var retrace := int(RenderingServer.nextgi_get_counters().get("voxel_lighting_retrace", -1))
		settle_count = settle_count + 1 if retrace == 0 else 0
		if settle_count >= SETTLE_FRAMES:
			phase = 1
			phase_frame = 0
		elif frames >= MAX_FRAMES:
			_finish(["voxel lighting was still filling after %d frames" % frames])
		return
	if phase == 1:
		# The dark room, which the camera has watched since the start.
		if phase_frame <= MEASURE_FRAMES:
			return
		dark_samples.append(_patch_luma())
		if dark_samples.size() < MEASURE_SAMPLES:
			return
		_aim(-1.0)
		phase = 2
		phase_frame = 0
		return
	if phase_frame <= LIT_FRAMES:
		return
	lit_samples.append(_patch_luma())
	if lit_samples.size() < MEASURE_SAMPLES:
		return
	_finish(_checks())


func _checks() -> PackedStringArray:
	var result := PackedStringArray()
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := str(info.get("tier", ""))
	if tier != expected_tier:
		result.append("tier is %s, expected %s" % [tier, expected_tier])
		return result
	if not bool(info.get("lite_beta", false)):
		result.append("Lumen Lite is not active (quality %s)" % str(info.get("quality", "")))
		return result
	var lit := _mean(lit_samples)
	var dark := _mean(dark_samples)
	var ratio := dark / maxf(lit, 1e-6)
	if expected_tier == "HWRT":
		print("LITE_LEAK_REFERENCE ratio=%.6f lit=%.6f dark=%.6f frames=%d" % [ratio, lit, dark, frames])
		if lit < MIN_LIT_LUMA:
			result.append("the lit floor's field is dark (%.6f)" % lit)
		return result
	var bound := LEAK_FACTOR * hwrt_ratio
	print("LITE_LEAK tier=%s ratio=%.6f lit=%.6f dark=%.6f hwrt_ratio=%.6f bound=%.6f frames=%d" % [tier, ratio, lit, dark, hwrt_ratio, bound, frames])
	if lit < MIN_LIT_LUMA:
		result.append("the lit floor's field is dark (%.6f)" % lit)
	if ratio > bound:
		result.append("the dark room's field is %.4f of the lit room's, above %.4f (%.1f x the hardware tier's %.4f)" % [ratio, bound, LEAK_FACTOR, hwrt_ratio])
	return result


func _finish(p_failures: PackedStringArray) -> void:
	done = true
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "lite_leak",
		"pass": p_failures.is_empty(),
		"expected_tier": expected_tier,
		"tier": info.get("tier", ""),
		"quality": info.get("quality", ""),
		"failures": p_failures,
	}))
	get_tree().quit(0 if p_failures.is_empty() else 1)
