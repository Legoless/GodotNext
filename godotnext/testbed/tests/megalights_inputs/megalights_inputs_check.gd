extends Node3D
## #136: MegaLights albedo, material pack, decals, allocation, and the off switch.
## This is a build check. It does not measure prepass cost.

# Byte values, so the textures hold them exactly and the expected values carry no rounding.
const AO_LIGHT_AFFECT := 0.6
const DECAL_ALBEDO_SRGB := Color8(0, 255, 0)
const DECAL_ORM := Color8(64, 255, 128)

var _probe: MlProbe


func _ready() -> void:
	_run()


func _run() -> void:
	var mode := _arg("ml-check", "albedo")
	var out := _arg("out", "")
	if out != "":
		DirAccess.make_dir_recursive_absolute(out)
		var manifest := FileAccess.open(out.path_join("manifest.json"), FileAccess.WRITE)
		if manifest != null:
			manifest.store_string("{}\n")
			manifest.close()

	var sub := SubViewport.new()
	sub.name = "View"
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_2X if mode == "msaa" else Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = 0
	# HDR keeps the nearest UNORM8 blit as linear floats, so round(channel * 255) is the packed byte.
	# An LDR sRGB target would decode those bytes.
	sub.use_hdr_2d = true
	# An opaque HDR viewport is RGBH and swizzles alpha to 1, so the packed layer index reads back as 255.
	sub.transparent_bg = mode == "material" or mode == "albedo" or mode == "decal"
	sub.size = Vector2i(64, 32 if mode == "fallback" or mode == "material" else 64)
	sub.debug_draw = _debug_draw(mode)
	add_child(sub)

	var world := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0, 0, 0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0, 0, 0)
	environment.ambient_light_energy = 0.0
	world.environment = environment
	sub.add_child(world)

	var light := DirectionalLight3D.new()
	light.light_color = Color(0, 0, 1)
	light.light_energy = 2.0
	light.shadow_enabled = false
	light.rotation_degrees = Vector3(-45, 30, 0)
	sub.add_child(light)

	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 2.0
	camera.position = Vector3(0, 0, 2)
	camera.current = true
	_probe = MlProbe.new()
	var compositor := Compositor.new()
	compositor.compositor_effects = [_probe]
	camera.compositor = compositor
	sub.add_child(camera)

	var metallic := float(_arg("metallic", "0"))
	if mode == "fallback" or mode == "material":
		# A is the layer-group index. Groups are keyed by MeshInstance3D.layers.
		_add_quad(sub, Vector3(-0.5, 0, 0), Vector2(1, 2), Color(0.25, 0.5, 0.75), true, 0.0, 1)
		if mode == "material":
			# Metallic red. 77 and 0x5E are pack_f0 for F0 (1, 0, 0), locked by the doctest.
			_add_quad(sub, Vector3(0.5, 0, 0), Vector2(1, 2), Color(1, 0, 0), false, 1.0, 2)
			# A lit alpha-pass surface that the depth prepass also draws. The opaque color
			# pass never shades it, so it is a fallback (model 3). Alpha 1 keeps it above the
			# opaque prepass threshold. Same layers as the left quad, so the same group.
			var alpha_material := _add_quad(sub, Vector3(-1.5, 0, 0), Vector2(1, 2), Color(1, 1, 1, 1), false, 0.0, 1)
			alpha_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS
		else:
			_add_quad(sub, Vector3(0.5, 0, 0), Vector2(1, 2), Color(1, 0.2, 0.1), false, 0.0, 2)
	elif mode == "decal":
		# Red floor, AO texture white, so only the decal's ORM can lower ml_albedo.a.
		var floor_material := _add_quad(sub, Vector3(0, 0, 0), Vector2(2, 2), Color(1, 0, 0), false, 0.0, 1)
		floor_material.ao_enabled = true
		floor_material.ao_texture = _solid_texture(Color8(255, 255, 255))
		floor_material.ao_light_affect = AO_LIGHT_AFFECT
		# Decal3D projects along its local -Y. Rotated so that axis is world -Z, toward the floor.
		# The floor sits on the box center, so the upper and lower fades are 1. The box is
		# larger than the view, so every pixel samples the middle of the atlas rects.
		var decal := Decal.new()
		decal.size = Vector3(4, 2, 4)
		decal.rotation_degrees = Vector3(90, 0, 0)
		decal.texture_albedo = _solid_texture(DECAL_ALBEDO_SRGB)
		decal.texture_orm = _solid_texture(DECAL_ORM)
		sub.add_child(decal)
	elif mode != "msaa":
		# albedo_color is sRGB. The color pass and the input target store the linear value.
		var material := _add_quad(sub, Vector3(0, 0, 0), Vector2(2, 2), _albedo_srgb(), false, metallic, 1)
		var ao_byte := _ao_byte()
		if ao_byte >= 0:
			material.ao_enabled = true
			material.ao_texture = _solid_texture(Color8(ao_byte, ao_byte, ao_byte))
			material.ao_light_affect = AO_LIGHT_AFFECT

	for _i in 12:
		await RenderingServer.frame_post_draw

	var image := sub.get_texture().get_image()
	var failures := PackedStringArray()
	var result := {"test": "megalights_inputs", "mode": mode, "pass": false}
	_check_allocation(mode, result, failures)
	if image == null:
		failures.append("viewport image is missing")
	elif mode == "fallback":
		result["left_hex"] = _region_hex(image, 0, image.get_width() / 2)
		result["full_hex"] = _region_hex(image, 0, image.get_width())
		result["width"] = image.get_width()
		result["height"] = image.get_height()
	elif mode == "material":
		_check_material(image, result, failures)
	elif mode == "off":
		var albedo := _expected_albedo(0.0)
		var worst := _worst_delta(image, albedo)
		result["worst"] = worst
		if worst <= 1.0 / 255.0:
			failures.append("off debug view matched albedo, so the input target was shown")
	elif mode == "albedo":
		var albedo := _expected_albedo(metallic)
		var worst := _worst_delta(image, albedo)
		result["worst"] = worst
		result["metallic"] = metallic
		result["ao_byte"] = _ao_byte()
		result["samples"] = image.get_width() * image.get_height()
		var center := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		result["center"] = "%s,%s,%s,%s" % [center.r, center.g, center.b, center.a]
		if worst > 1.0 / 255.0:
			failures.append("albedo delta %s exceeds 1/255" % worst)
	elif mode == "decal":
		# The decal replaces albedo, AO and metallic (albedo_mix 1, full coverage), as the
		# clustered loop applies them: RGB is decal albedo * (1 - decal metallic), A is
		# mix(1, decal AO, ao_light_affect).
		var decal_albedo := DECAL_ALBEDO_SRGB.srgb_to_linear() * (1.0 - DECAL_ORM.b)
		var expected := Color(decal_albedo.r, decal_albedo.g, decal_albedo.b, lerpf(1.0, DECAL_ORM.r, AO_LIGHT_AFFECT))
		var worst := _worst_delta(image, expected)
		result["worst"] = worst
		var center := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
		result["center_bytes"] = "%s,%s,%s,%s" % [_byte(center.r), _byte(center.g), _byte(center.b), _byte(center.a)]
		result["expected_decal_bytes"] = "%s,%s,%s,%s" % [_byte(expected.r), _byte(expected.g), _byte(expected.b), _byte(expected.a)]
		if worst > 1.0 / 255.0:
			failures.append("decal albedo delta %s exceeds 1/255" % worst)
	elif mode != "msaa":
		failures.append("unknown ml-check mode %s" % mode)

	result["pass"] = failures.is_empty()
	result["failures"] = failures
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if failures.is_empty() else 1)


func _debug_draw(mode: String) -> Viewport.DebugDraw:
	if mode == "fallback" or mode == "msaa":
		return Viewport.DEBUG_DRAW_DISABLED
	if mode == "material":
		return Viewport.DEBUG_DRAW_ML_MATERIAL
	return Viewport.DEBUG_DRAW_ML_ALBEDO


func _inputs_requested() -> bool:
	return OS.get_environment("GODOTNEXT_MEGALIGHTS_INPUTS") == "1"


func _check_allocation(mode: String, result: Dictionary, failures: PackedStringArray) -> void:
	var expect := false
	if mode == "msaa":
		expect = false
		if not _inputs_requested():
			failures.append("msaa check requires GODOTNEXT_MEGALIGHTS_INPUTS=1")
	elif mode == "off":
		expect = false
	elif mode == "albedo" or mode == "material" or mode == "decal":
		expect = true
		if not _inputs_requested():
			failures.append("%s check requires GODOTNEXT_MEGALIGHTS_INPUTS=1" % mode)
	else:
		expect = _inputs_requested()
	result["alloc_calls"] = _probe.calls
	result["has_albedo"] = _probe.has_albedo
	result["has_material"] = _probe.has_material
	result["memory_bytes"] = _probe.memory_bytes
	result["internal_size"] = "%s,%s" % [_probe.internal_size.x, _probe.internal_size.y]
	result["view_count"] = _probe.view_count
	if _probe.calls == 0:
		failures.append("compositor did not read render buffers")
		return
	if expect:
		if not _probe.has_albedo or not _probe.has_material:
			failures.append("megalights textures missing while inputs are active")
		var expected_bytes := 2 * _probe.internal_size.x * _probe.internal_size.y * 4 * _probe.view_count
		result["expected_bytes"] = expected_bytes
		if _probe.internal_size.x <= 0 or _probe.internal_size.y <= 0 or _probe.view_count <= 0:
			failures.append("megalights size is empty")
		elif _probe.memory_bytes != expected_bytes:
			failures.append("megalights bytes %s != %s" % [_probe.memory_bytes, expected_bytes])
	else:
		if _probe.has_albedo or _probe.has_material or _probe.memory_bytes != 0:
			failures.append("megalights textures allocated while inputs are off")


func _check_material(image: Image, result: Dictionary, failures: PackedStringArray) -> void:
	# 64x32, orthogonal size 2, aspect 2: world x -2..2. Quads sit at x=-1.5, -0.5 and 0.5, size 1.
	# Pixels 4/8/12, 20/24/28 and 36/40/44 are inside those quads, clear of the edges.
	var alpha := _sample_bytes(image, [4, 8, 12], [8, 16, 24])
	var left := _sample_bytes(image, [20, 24, 28], [8, 16, 24])
	var right := _sample_bytes(image, [36, 40, 44], [8, 16, 24])
	result["alpha_bytes"] = _bytes_text(alpha)
	result["left_bytes"] = _bytes_text(left)
	result["right_bytes"] = _bytes_text(right)
	if alpha.is_empty() or left.is_empty() or right.is_empty():
		failures.append("material samples are missing")
		return
	for quad in [["alpha-pass", alpha], ["fallback", left], ["metallic", right]]:
		for sample in quad[1]:
			if sample != quad[1][0]:
				failures.append("%s quad bytes are not stable" % quad[0])
				break
	# R is F0 luma, G is Cb (high nibble) and Cr (low nibble), B is the shading model plus
	# flags, A is the layer group. The bytes are MegaLightsInputs::pack_f0 values the doctest
	# locks, so a GLSL pack that drifts from the C++ rule fails here.
	# Gray dielectric F0 0.04: luma 10, both chroma nibbles at the bias 7, so 0x77.
	_expect_byte(failures, "fallback F0 luma", left[0][0], 10)
	_expect_byte(failures, "fallback F0 tint", left[0][1], 0x77)
	_expect_byte(failures, "fallback shading model", left[0][2], 3)
	# F0 (1, 0, 0): luma 77, Cb nibble 5, Cr nibble 14, so 0x5E. Swapped nibbles read 0xE5.
	_expect_byte(failures, "metallic F0 luma", right[0][0], 77)
	_expect_byte(failures, "metallic F0 tint", right[0][1], 0x5E)
	_expect_byte(failures, "metallic shading model", right[0][2], 0)
	_expect_byte(failures, "alpha-pass F0 luma", alpha[0][0], 10)
	_expect_byte(failures, "alpha-pass F0 tint", alpha[0][1], 0x77)
	_expect_byte(failures, "alpha-pass shading model", alpha[0][2], 3)
	var left_a := int(left[0][3])
	var right_a := int(right[0][3])
	if [left_a, right_a] != [0, 1] and [left_a, right_a] != [1, 0]:
		failures.append("layer groups %s and %s, expected 0 and 1" % [left_a, right_a])
	_expect_byte(failures, "alpha-pass layer group", alpha[0][3], left_a)


func _expect_byte(failures: PackedStringArray, label: String, actual: int, expected: int) -> void:
	if actual != expected:
		failures.append("%s byte %s, expected %s" % [label, actual, expected])


func _sample_bytes(image: Image, xs: Array, ys: Array) -> Array:
	var samples: Array = []
	for y in ys:
		for x in xs:
			var pixel := image.get_pixel(x, y)
			samples.append(PackedInt32Array([_byte(pixel.r), _byte(pixel.g), _byte(pixel.b), _byte(pixel.a)]))
	return samples


func _byte(channel: float) -> int:
	return clampi(int(round(channel * 255.0)), 0, 255)


func _bytes_text(samples: Array) -> String:
	if samples.is_empty():
		return ""
	var pixel: PackedInt32Array = samples[0]
	return "%s,%s,%s,%s" % [pixel[0], pixel[1], pixel[2], pixel[3]]


func _add_quad(sub: SubViewport, position: Vector3, size: Vector2, albedo: Color, unshaded: bool, metallic: float, layers: int) -> StandardMaterial3D:
	var mesh_instance := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = size
	mesh_instance.mesh = mesh
	mesh_instance.position = position
	mesh_instance.layers = layers
	var material := StandardMaterial3D.new()
	material.albedo_color = albedo
	material.roughness = 1.0
	material.metallic = metallic
	if unshaded:
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mesh_instance.material_override = material
	sub.add_child(mesh_instance)
	return material


func _solid_texture(color: Color) -> ImageTexture:
	var image := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)


func _expected_albedo(metallic: float) -> Color:
	var linear := _albedo_srgb().srgb_to_linear()
	var diffuse := linear * (1.0 - metallic)
	# A is the direct-light occlusion, mix(1, ao, ao_light_affect). Without --ao, ao is 1.
	var occlusion := 1.0
	var ao_byte := _ao_byte()
	if ao_byte >= 0:
		occlusion = lerpf(1.0, float(ao_byte) / 255.0, AO_LIGHT_AFFECT)
	return Color(diffuse.r, diffuse.g, diffuse.b, occlusion)


# --ao is an AO texture byte. -1 leaves AO off.
func _ao_byte() -> int:
	return int(_arg("ao", "-1"))


func _worst_delta(image: Image, albedo: Color) -> float:
	var worst := 0.0
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			worst = maxf(worst, absf(pixel.r - albedo.r))
			worst = maxf(worst, absf(pixel.g - albedo.g))
			worst = maxf(worst, absf(pixel.b - albedo.b))
			worst = maxf(worst, absf(pixel.a - albedo.a))
	return worst


func _region_hex(image: Image, x0: int, x1: int) -> String:
	var bytes := PackedByteArray()
	for y in image.get_height():
		for x in range(x0, x1):
			var pixel := image.get_pixel(x, y)
			bytes.append(_byte(pixel.r))
			bytes.append(_byte(pixel.g))
			bytes.append(_byte(pixel.b))
	return bytes.hex_encode()


func _albedo_srgb() -> Color:
	var parts := _arg("albedo", "1,0,0").split(",")
	return Color(float(parts[0]), float(parts[1]), float(parts[2]))


func _arg(name: String, fallback: String) -> String:
	var prefix := "--%s=" % name
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return arg.substr(prefix.length())
	return fallback


class MlProbe extends CompositorEffect:
	var calls := 0
	var has_albedo := false
	var has_material := false
	var memory_bytes := 0
	var internal_size := Vector2i.ZERO
	var view_count := 0

	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE

	func _render_callback(_callback_type: int, render_data: RenderData) -> void:
		var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if buffers == null:
			return
		calls += 1
		has_albedo = buffers.has_texture(&"megalights", &"ml_albedo")
		has_material = buffers.has_texture(&"megalights", &"ml_material")
		memory_bytes = int(buffers.get_context_memory_bytes(&"megalights"))
		internal_size = buffers.get_internal_size()
		view_count = buffers.get_view_count()
