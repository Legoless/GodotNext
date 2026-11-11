extends Node3D
## #183: Lumen Lite shading on the hardware tier (the M5 Max auto default).
## GodotNext: ps_swrt_trace (#186). tests/lumen_lite/lumen_lite_shading_check.tscn runs the same
## checks on the software tier (expected_tier SWRT; the folder's override.cfg turns hardware ray
## tracing off). SSR, SSAO, SSIL and contact shadows stay off, so only the prepass that Lite
## views force writes the normals and roughness the field and the rough specular read.
##
## A procedural scene the checks can reason about: spheres float under a panorama sky whose
## upper half is bright with vertical stripes and whose lower half is dim. There is no light,
## so the surface cache is black and the field and the rough specular carry only the sky that
## the WRC probes see.
## - Directional probes (L0 + L1): on a diffuse sphere, the field's irradiance on the side that
##   faces the sky is well above the side that faces the ground. One mean value per probe
##   makes the two sides nearly equal.
## - Rough specular (roughness above 0.25) reads the WRC mips of the surrounding probes at the
##   reflection vector. On a rough metal sphere the reflection buffer is bright where it
##   reflects the sky and dim where it reflects the ground. A fixed octahedral texel makes the
##   two sides equal.
## - Nothing outside WRC level 1 (15 m around the camera): a rough sphere 26 m away leaves the
##   reflection buffer empty instead of reading a clamped edge probe.
## - Smooth specular (roughness 0.25 or below): Lite writes no reflection there, and the forward
##   pass keeps its own (sky) specular, so a mirror sphere renders as it does with NextGI off.
##   The ambient fallback would paint the mirror with the smooth irradiance instead of the
##   striped sky.
## The camera is rolled a quarter turn, so its view space differs from world space. The prepass
## normals are in view space; Lite must rotate them to world space before it evaluates the L1
## field or the reflection vector, or the sky and ground sides come out level (no rotation) or
## swapped (the inverse rotation).
##
## Prints LITE_SHADING lines and LITE_SHADING_PASS, or LITE_SHADING_FAIL with every failed
## assertion, and exits 0 or 1.

const WARMUP_FRAMES := 240
const VIEW_FRAMES := 12
const OFF_FRAMES := 30
## Panorama radiance: the stripes above the horizon and the ground below it.
const SKY_BRIGHT := 1.6
const SKY_DIM := 0.6
const SKY_GROUND := 0.08
## The side facing the sky must be at least this many times the side facing the ground.
const MIN_DIRECTIONAL_RATIO := 2.0
## A reflection-buffer pixel at or below this luminance is empty.
const EMPTY_LUMA := 0.01
## Mirror sphere: mean absolute channel difference allowed between Lite and NextGI off, and the
## luminance spread its samples need for the comparison to see the ambient fallback.
const MIRROR_TOLERANCE := 0.02
const MIRROR_MIN_SPREAD := 0.15
## A quarter-turn roll: view-space up is world-space left.
const CAMERA_ROLL := PI * 0.5
const CONTACT_SHADOWS := "rendering/lights_and_shadows/contact_shadow/enabled"

@export var expected_tier := "HWRT"

var _camera: Camera3D
var _environment: Environment
var _diffuse_center := Vector3(-1.4, 0.0, -3.2)
var _rough_center := Vector3(0.0, 0.0, -3.2)
var _mirror_center := Vector3(1.4, 0.0, -3.2)
var _far_center := Vector3(0.0, 8.0, -26.0)
var _far_radius := 2.5
var _radius := 0.5
var _phase := 0
var _phase_frame := 0
var _failures := PackedStringArray()
var _summary := {}
var _mirror_lite := PackedColorArray()
var _done := false


func _ready() -> void:
	_environment = Environment.new()
	_environment.background_mode = Environment.BG_SKY
	var sky_material := PanoramaSkyMaterial.new()
	sky_material.panorama = ImageTexture.create_from_image(_panorama())
	var sky := Sky.new()
	sky.sky_material = sky_material
	_environment.sky = sky
	_environment.nextgi_enabled = true
	# None of the screen-space effects that would also request the normal-roughness prepass.
	_environment.ssr_enabled = false
	_environment.ssao_enabled = false
	_environment.ssil_enabled = false
	var world := WorldEnvironment.new()
	world.environment = _environment
	add_child(world)

	_add_sphere(_diffuse_center, _radius, 0.0, 1.0)
	_add_sphere(_rough_center, _radius, 1.0, 0.6)
	_add_sphere(_mirror_center, _radius, 1.0, 0.05)
	_add_sphere(_far_center, _far_radius, 1.0, 0.6)

	_camera = Camera3D.new()
	_camera.rotation = Vector3(0.0, 0.0, CAMERA_ROLL)
	add_child(_camera)
	_camera.current = true


# Equirectangular: the top half is the sky (16 stripes of longitude), the bottom half the ground.
func _panorama() -> Image:
	var image := Image.create_empty(512, 256, false, Image.FORMAT_RGBH)
	image.fill(Color(SKY_GROUND, SKY_GROUND, SKY_GROUND))
	for stripe in 16:
		var value := SKY_BRIGHT if stripe % 2 == 0 else SKY_DIM
		image.fill_rect(Rect2i(stripe * 32, 0, 32, 128), Color(value, value, value))
	return image


func _add_sphere(center: Vector3, radius: float, metallic: float, roughness: float) -> void:
	var mesh := SphereMesh.new()
	mesh.radius = radius
	mesh.height = radius * 2.0
	mesh.radial_segments = 64
	mesh.rings = 32
	var material := StandardMaterial3D.new()
	material.albedo_color = Color.WHITE
	material.metallic = metallic
	material.roughness = roughness
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = center
	add_child(instance)


func _write_manifest() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir := ""
	for index in args.size():
		var arg := str(args[index])
		if arg.begins_with("--out="):
			out_dir = arg.substr("--out=".length())
		elif arg == "--out" and index + 1 < args.size():
			out_dir = str(args[index + 1])
	if out_dir.is_empty():
		return
	DirAccess.make_dir_recursive_absolute(out_dir)
	var file := FileAccess.open(out_dir.path_join("manifest.json"), FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify({"source": "lumen_lite_shading_check", "summary": _summary, "failures": _failures}, "\t") + "\n")


func _finish() -> void:
	_done = true
	_write_manifest()
	if _failures.is_empty():
		print("LITE_SHADING_PASS")
		get_tree().quit(0)
		return
	for failure in _failures:
		push_error(failure)
	print("LITE_SHADING_FAIL %s" % "; ".join(_failures))
	get_tree().quit(1)


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


# The point of a sphere seen at 45 degrees above (+1) or below (-1) the direction to the camera.
func _side_point(center: Vector3, radius: float, side: float) -> Vector3:
	var to_camera := (_camera.global_position - center).normalized()
	return center + (to_camera + Vector3.UP * side).normalized() * radius


func _pixel(image: Image, world: Vector3) -> Vector2i:
	var screen := _camera.unproject_position(world)
	var size := get_viewport().get_visible_rect().size
	return Vector2i(int(screen.x / size.x * image.get_width()), int(screen.y / size.y * image.get_height()))


func _window_luma(image: Image, center: Vector2i, half: int = 2) -> float:
	var total := 0.0
	var count := 0
	for y in range(center.y - half, center.y + half + 1):
		for x in range(center.x - half, center.x + half + 1):
			if x >= 0 and y >= 0 and x < image.get_width() and y < image.get_height():
				total += image.get_pixel(x, y).get_luminance()
				count += 1
	return total / float(maxi(count, 1))


# Colors on a grid over the inner 70% of a sphere's screen disc.
func _disc_samples(image: Image, center: Vector3, radius: float) -> PackedColorArray:
	var samples := PackedColorArray()
	var middle := _pixel(image, center)
	var edge := _pixel(image, center + _camera.global_transform.basis.x * radius)
	var pixel_radius := float(edge.x - middle.x) * 0.7
	for row in range(-4, 5):
		for column in range(-4, 5):
			var offset := Vector2(column, row) / 4.0
			if offset.length() > 1.0:
				continue
			var pixel := middle + Vector2i(offset * pixel_radius)
			if pixel.x >= 0 and pixel.y >= 0 and pixel.x < image.get_width() and pixel.y < image.get_height():
				samples.append(image.get_pixel(pixel.x, pixel.y))
	return samples


func _max_luma(samples: PackedColorArray) -> float:
	var result := 0.0
	for sample in samples:
		result = maxf(result, sample.get_luminance())
	return result


func _image() -> Image:
	return get_viewport().get_texture().get_image()


func _process(_delta: float) -> void:
	if _done:
		return
	_phase_frame += 1
	match _phase:
		0:
			if _phase_frame < WARMUP_FRAMES:
				return
			var info: Dictionary = RenderingServer.nextgi_get_tier_info()
			var counters: Dictionary = RenderingServer.nextgi_get_counters()
			_check(str(info.get("tier", "")) == expected_tier, "tier is %s, expected %s" % [str(info.get("tier", "")), expected_tier])
			_check(not bool(ProjectSettings.get_setting(CONTACT_SHADOWS, false)), "%s is on: the check needs every screen-space effect off" % CONTACT_SHADOWS)
			_check(bool(info.get("lite_beta", false)), "Lite is not active (quality %s)" % str(info.get("quality", "")))
			_check(int(counters.get("gather_dispatches", -1)) == 0, "Lite recorded gather dispatches")
			_check(int(counters.get("reflection_dispatches", -1)) == 0, "Lite recorded reflection dispatches")
			_check(not _camera.is_position_behind(_far_center), "the far sphere is behind the camera")
			if not _failures.is_empty():
				_finish()
				return
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_LITE_PROBES
			_next_phase(1)
		1:
			if _phase_frame < VIEW_FRAMES:
				return
			var image := _image()
			var top := _window_luma(image, _pixel(image, _side_point(_diffuse_center, _radius, 1.0)))
			var bottom := _window_luma(image, _pixel(image, _side_point(_diffuse_center, _radius, -1.0)))
			_summary["field_diffuse"] = {"top": top, "bottom": bottom}
			print("LITE_SHADING field diffuse_sphere top=%.4f bottom=%.4f" % [top, bottom])
			_check(top > 0.05, "the Lite field is empty on the diffuse sphere (top %.4f)" % top)
			_check(top >= bottom * MIN_DIRECTIONAL_RATIO, "the Lite field is not directional: the sky side (%.4f) is not %.1fx the ground side (%.4f)" % [top, MIN_DIRECTIONAL_RATIO, bottom])
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_REFLECTION
			_next_phase(2)
		2:
			if _phase_frame < VIEW_FRAMES:
				return
			var image := _image()
			var top := _window_luma(image, _pixel(image, _side_point(_rough_center, _radius, 1.0)))
			var bottom := _window_luma(image, _pixel(image, _side_point(_rough_center, _radius, -1.0)))
			var mirror := _max_luma(_disc_samples(image, _mirror_center, _radius))
			var far := _max_luma(_disc_samples(image, _far_center, _far_radius))
			_summary["reflection"] = {"rough_top": top, "rough_bottom": bottom, "mirror_max": mirror, "far_max": far}
			print("LITE_SHADING reflection rough_top=%.4f rough_bottom=%.4f mirror_max=%.4f far_max=%.4f" % [top, bottom, mirror, far])
			_check(top > 0.05, "Lite wrote no rough specular on the rough sphere (top %.4f)" % top)
			_check(top >= bottom * MIN_DIRECTIONAL_RATIO, "the rough specular does not follow the reflection vector: the sky side (%.4f) is not %.1fx the ground side (%.4f)" % [top, MIN_DIRECTIONAL_RATIO, bottom])
			_check(mirror <= EMPTY_LUMA, "Lite wrote a reflection on the mirror sphere at roughness 0.05 (max %.4f)" % mirror)
			_check(far <= EMPTY_LUMA, "Lite wrote a reflection beyond WRC level 1 (far sphere max %.4f)" % far)
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
			_next_phase(3)
		3:
			if _phase_frame < VIEW_FRAMES:
				return
			_mirror_lite = _disc_samples(_image(), _mirror_center, _radius)
			_environment.nextgi_enabled = false
			_next_phase(4)
		4:
			if _phase_frame < OFF_FRAMES:
				return
			var mirror_off := _disc_samples(_image(), _mirror_center, _radius)
			var difference := 0.0
			var low := INF
			var high := 0.0
			for index in mini(mirror_off.size(), _mirror_lite.size()):
				var lite: Color = _mirror_lite[index]
				var off: Color = mirror_off[index]
				difference += (absf(lite.r - off.r) + absf(lite.g - off.g) + absf(lite.b - off.b)) / 3.0
				low = minf(low, off.get_luminance())
				high = maxf(high, off.get_luminance())
			difference /= float(maxi(mirror_off.size(), 1))
			var spread := high - low
			_summary["mirror"] = {"samples": mirror_off.size(), "mean_difference": difference, "spread_off": spread}
			print("LITE_SHADING mirror samples=%d mean_difference=%.4f spread_off=%.4f" % [mirror_off.size(), difference, spread])
			_check(mirror_off.size() >= 40 and mirror_off.size() == _mirror_lite.size(), "the mirror sphere is not on screen (%d samples)" % mirror_off.size())
			_check(spread >= MIRROR_MIN_SPREAD, "the mirror does not show the striped sky with NextGI off (spread %.4f)" % spread)
			_check(difference <= MIRROR_TOLERANCE, "the mirror's specular changes under Lite (mean difference %.4f): smooth surfaces must keep the forward specular" % difference)
			_finish()


func _next_phase(phase: int) -> void:
	_phase = phase
	_phase_frame = 0
