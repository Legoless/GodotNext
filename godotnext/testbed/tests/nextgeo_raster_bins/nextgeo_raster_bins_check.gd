extends Node3D
## NextGeo programmable raster bins check (#155, plan §21.4 N3, N4).
##
## Renders virtualized meshes with a fixed-function background box behind four programmable
## materials: a plane masked by alpha scissor stripes, a sphere masked by a discard checker, a sphere
## whose vertex() scales it by VERTEX_SCALE (vertex offset), and a plane with a StandardMaterial3D
## whose albedo texture's alpha is cut by its alpha scissor (textures, samplers and textureGrad in the
## raster target). Five small boxes carry masked materials the raster target rejects (FRAGCOORD, the
## screen, depth and normal-roughness textures, and the absolute DEPTH). Every internal pixel's
## NextGeo instance and visibility IDs are read back (the #162 probe) in these phases:
## - `legacy`: rendering/nextgeo/debug_programmable_raster off, the default. The programmable materials
##   stay on legacy raster with their #80 reasons; the background's NextGeo pixels show through the
##   masked holes and around the scaled sphere. This is the reference.
## - `bins_hw`: the setting on, sw_raster_max_edge_px = 0: every programmable cluster takes its bin's
##   hardware form. The programmable surfaces route virtual, and the background's pixels must match the
##   reference (same holes, same silhouettes) within edge tolerances.
## - `bins_sw`: T = 32: the plane's and spheres' small clusters take the bins' software form. The same
##   comparison, and the programmable instances' coverage must match `bins_hw`.
## - `bins_sw_buffer`: rendering/nextgeo/debug_vis64_buffer. Must equal `bins_sw` exactly.
## - `legacy_again`: the setting off again; must equal `legacy` exactly.
## The rejected materials stay legacy with UNSUPPORTED_RASTER_FEATURE in every phase. Below the Nanite
## tier (N1, for example D3D12 without 64-bit atomics) the setting admits nothing: every phase equals
## the reference and keeps the default reasons.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")
const SENTINEL := -1 # 0xFFFFFFFF read as int32.
const CAPTURE_FRAMES: Array[int] = [40, 41]
const STATS_FRAME := 46
const VERTEX_SCALE := 1.25
# Functional bounds, per capture: stripe and checker edges and silhouettes may flip a pixel where the
# forward pass's hardware interpolation and the raster target's analytic barycentrics round apart.
const MAX_BACKGROUND_ONE_SIDED_FRACTION := 0.01 # Of the reference's background pixels.
const MAX_BACKGROUND_ONE_SIDED_PIXELS := 24
const MAX_SW_HW_ONE_SIDED_FRACTION := 0.01 # Of the programmable instances' pixels.
const MAX_SW_HW_ONE_SIDED_PIXELS := 24
# The masks are real: the stripes and the checker cover about half of what the plane and the sphere
# would cover (the plane fills its screen rectangle, a sphere about 0.785 of its square).
const MAX_STRIPES_FILL := 0.75
const MAX_CHECKER_FILL := 0.6
const MIN_MASK_FILL := 0.2

const MASKED_STRIPES_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.8, 0.3, 0.2);
	ALPHA = step(0.5, fract(UV.x * 6.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const MASKED_CHECKER_SHADER := """
shader_type spatial;
void fragment() {
	bool u = fract(UV.x * 12.0) < 0.5;
	bool v = fract(UV.y * 6.0) < 0.5;
	if (u != v) {
		discard;
	}
	ALBEDO = vec3(0.2, 0.4, 0.8);
}
"""

const VERTEX_OFFSET_SHADER := """
shader_type spatial;
uniform float vertex_scale = 1.0;
void vertex() {
	VERTEX *= vertex_scale;
}
void fragment() {
	ALBEDO = vec3(0.3, 0.8, 0.3);
}
"""

const TWO_SIDED_FRAG_TO_LIGHT_SHADER := """
shader_type spatial;
render_mode cull_disabled;
varying float v_f2l;
float stripe_alpha(vec2 uv) {
	return step(0.5, fract(uv.x * 6.0));
}
float light_boost(float x) {
	return 0.5 + 0.5 * x;
}
void fragment() {
	v_f2l = UV.y;
	ALBEDO = vec3(0.7, 0.3, 0.8);
	ALPHA = stripe_alpha(UV);
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
void light() {
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION * light_boost(v_f2l);
}
"""

const NEAR_PLANE_MASKED_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(0.2, 0.8, 0.8);
	ALPHA = step(0.5, fract(UV.x * 6.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

# The raster target rejects these only in materials with a raster program, so each one is masked.
const REJECTED_SHADERS := {
	"fragcoord": """
shader_type spatial;
void fragment() {
	ALPHA = step(0.5, fract(FRAGCOORD.x * 0.125));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
""",
	"screen_texture": """
shader_type spatial;
uniform sampler2D screen_tex : hint_screen_texture;
void fragment() {
	ALBEDO = texture(screen_tex, SCREEN_UV).rgb;
	ALPHA = step(0.5, fract(UV.x * 4.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
""",
	"depth_texture": """
shader_type spatial;
uniform sampler2D depth_tex : hint_depth_texture;
void fragment() {
	ALBEDO = vec3(texture(depth_tex, SCREEN_UV).r);
	ALPHA = step(0.5, fract(UV.x * 4.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
""",
	"normal_roughness_texture": """
shader_type spatial;
uniform sampler2D nr_tex : hint_normal_roughness_texture;
void fragment() {
	ALBEDO = texture(nr_tex, SCREEN_UV).rgb;
	ALPHA = step(0.5, fract(UV.x * 4.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
""",
	"depth": """
shader_type spatial;
void fragment() {
	if (fract(UV.y * 4.0) < 0.5) {
		discard;
	}
	ALBEDO = vec3(1.0);
	DEPTH = FRAGCOORD.z;
}
""",
	"function_discard": """
shader_type spatial;
void clip_half(float x) {
	if (fract(x * 4.0) < 0.5) {
		discard;
	}
}
void fragment() {
	clip_half(UV.x);
	ALBEDO = vec3(1.0, 0.5, 0.2);
}
""",
	"light_discard": """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.5, 1.0, 0.5);
}
void light() {
	if (fract(UV.x * 4.0) < 0.5) {
		discard;
	}
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION;
}
""",
}

var _phases: Array[Dictionary] = [
	{ "name": "legacy", "bins": false, "raster_mode": 1, "t": 8.0, "buffer": false, "vis_only": false, "backface": false, "fill_64": false },
	{ "name": "bins_hw", "bins": true, "raster_mode": 0, "t": 0.0, "buffer": false, "vis_only": false, "backface": false, "fill_64": false },
	{ "name": "bins_sw", "bins": true, "raster_mode": 0, "t": 32.0, "buffer": false, "vis_only": false, "backface": false, "fill_64": false },
	{ "name": "bins_sw_buffer", "bins": true, "raster_mode": 0, "t": 32.0, "buffer": true, "vis_only": false, "backface": false, "fill_64": false },
	{ "name": "bins_vis_only", "bins": true, "raster_mode": 0, "t": 32.0, "buffer": false, "vis_only": true, "backface": false, "fill_64": false },
	{ "name": "bins_backface_hw", "bins": true, "raster_mode": 0, "t": 0.0, "buffer": false, "vis_only": false, "backface": true, "fill_64": false },
	{ "name": "bins_backface_sw", "bins": true, "raster_mode": 0, "t": 32.0, "buffer": false, "vis_only": false, "backface": true, "fill_64": false },
	{ "name": "bins_64", "bins": true, "raster_mode": 0, "t": 32.0, "buffer": false, "vis_only": false, "backface": false, "fill_64": true },
	{ "name": "legacy_again", "bins": false, "raster_mode": 1, "t": 8.0, "buffer": false, "vis_only": false, "backface": false, "fill_64": false },
]

var _probe: CompositorEffect
var _background: MeshInstance3D
var _box_mesh: Mesh
var _shared_masked_shader: Shader
var _programmable: Array[MeshInstance3D] = []
var _extra_bins: Array[MeshInstance3D] = []
var _rejected: Dictionary = {} # name -> MeshInstance3D
var _materials: Dictionary = {} # MeshInstance3D -> Material
var _phase_idx := 0
var _phase_frame := 0
var _captures: Dictionary = {}
var _color_images: Dictionary = {} # phase name -> Image
var _stats: Dictionary = {}
var _routes: Dictionary = {} # phase name -> { instance name -> [route, reason] }
var _extra_routes: Array = [] # [route, reason] for each extra bin instance in bins_64
var _records: Dictionary = {} # instance name -> NextGeo record index
var _report: Dictionary = {}
var _failures: Array[String] = []
var _expect_bins := false


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_bins = indirect_count and (image_int64 or buffer_int64)
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"image_int64_atomics": image_int64,
		"buffer_int64_atomics": buffer_int64,
		"draw_indirect_count": indirect_count,
		"expect_programmable_bins": _expect_bins,
	}
	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5
	_build_scene()
	_apply_phase()


func _shader_material(p_code: String) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = p_code
	var material := ShaderMaterial.new()
	material.shader = shader
	return material


func _add(p_name: String, p_mesh: Mesh, p_transform: Transform3D, p_material: Material) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = p_mesh
	instance.transform = p_transform
	add_child(instance)
	if p_material != null:
		instance.material_override = p_material
		_materials[instance] = p_material
	return instance


func _build_scene() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(1.0, 1.0, 1.0)
	var box_material := StandardMaterial3D.new()
	box_material.albedo_color = Color(0.7, 0.6, 0.5)
	box.material = box_material
	var box_mesh := _virtualize(box, "user://nextgeo_raster_bins_box.ngm")
	_box_mesh = box_mesh
	_shared_masked_shader = Shader.new()
	_shared_masked_shader.code = MASKED_STRIPES_SHADER

	var plane := PlaneMesh.new()
	plane.size = Vector2(3.0, 2.0)
	plane.subdivide_width = 48
	plane.subdivide_depth = 32
	var plane_mesh := _virtualize(plane, "user://nextgeo_raster_bins_plane.ngm")

	var near_plane := PlaneMesh.new()
	near_plane.size = Vector2(0.025, 0.30)
	near_plane.subdivide_width = 2
	near_plane.subdivide_depth = 0
	var near_plane_mesh := _virtualize(near_plane, "user://nextgeo_raster_bins_near.ngm")

	var sphere := SphereMesh.new()
	sphere.radius = 0.6
	sphere.height = 1.2
	sphere.radial_segments = 96
	sphere.rings = 48
	var sphere_mesh := _virtualize(sphere, "user://nextgeo_raster_bins_sphere.ngm")

	var camera := Camera3D.new()
	camera.fov = 60.0
	camera.near = 0.05
	camera.far = 100.0
	camera.position = Vector3(0.0, 0.2, 4.5)
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 0.0, 0.0))

	# The fixed-function background, wide and behind everything, catching masked and WPO shadows.
	_background = _add("Background", box_mesh, Transform3D(Basis.from_scale(Vector3(14.0, 9.0, 0.5)), Vector3(0.0, 0.0, -3.0)), null)
	# The plane faces the camera (PlaneMesh faces +Y).
	_programmable.append(_add("MaskedStripes", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(90.0)), Vector3(0.0, 0.0, 0.0)), _shader_material(MASKED_STRIPES_SHADER)))
	_programmable.append(_add("MaskedChecker", sphere_mesh, Transform3D(Basis(), Vector3(-2.3, -0.4, 0.3)), _shader_material(MASKED_CHECKER_SHADER)))
	var vertex_offset := _shader_material(VERTEX_OFFSET_SHADER)
	vertex_offset.set_shader_parameter("vertex_scale", VERTEX_SCALE)
	_programmable.append(_add("VertexOffset", sphere_mesh, Transform3D(Basis(), Vector3(2.3, -0.4, 0.3)), vertex_offset))
	_programmable.append(_add("TexturedScissor", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(90.0)).scaled(Vector3.ONE * 0.5), Vector3(3.4, 1.4, 0.2)), _textured_scissor_material()))
	# Back-facing plane with cull_disabled + frag_to_light varying (MASKED | TWO_SIDED + BUCKET_CULL_DISABLED).
	_programmable.append(_add("TwoSidedFragToLight", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(-90.0)).scaled(Vector3.ONE * 0.5), Vector3(-3.4, 1.4, 0.2)), _shader_material(TWO_SIDED_FRAG_TO_LIGHT_SHADER)))
	# Near-plane-straddling plane: front edge w = +0.10 > near (0.05), back edge w = -0.20 < 0 (behind camera eye).
	_programmable.append(_add("NearPlaneMasked", near_plane_mesh, camera.transform * Transform3D(Basis(), Vector3(0.0, -0.024, 0.05)), _shader_material(NEAR_PLANE_MASKED_SHADER)))
	# Single-sided back-facing plane: culled normally, visible only under debug_backface.
	_programmable.append(_add("BackfaceProbe", plane_mesh, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(-90.0)).scaled(Vector3.ONE * 0.45), Vector3(3.5, -1.8, 0.2)), _shader_material(MASKED_STRIPES_SHADER)))
	var i := 0
	for rejected_name: String in REJECTED_SHADERS:
		_rejected[rejected_name] = _add("Rejected_" + rejected_name, box_mesh, Transform3D(Basis.from_scale(Vector3.ONE * 0.35), Vector3(-2.1 + float(i) * 0.7, 1.6, 0.5)), _shader_material(REJECTED_SHADERS[rejected_name]))
		i += 1

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-25.0, 20.0, 0.0)
	sun.shadow_enabled = true
	add_child(sun)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.1, 0.12, 0.16)
	var compositor := Compositor.new()
	_probe = ProbeScript.new()
	var effects: Array[CompositorEffect] = [_probe]
	compositor.compositor_effects = effects
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	world_environment.compositor = compositor
	add_child(world_environment)


# A StandardMaterial3D with alpha scissor, whose albedo texture holds vertical stripes of alpha 0 and 1.
func _textured_scissor_material() -> StandardMaterial3D:
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			image.set_pixel(x, y, Color(0.9, 0.8, 0.2, 1.0 if ((x >> 3) & 1) == 0 else 0.0))
	image.generate_mipmaps()
	var material := StandardMaterial3D.new()
	material.albedo_texture = ImageTexture.create_from_image(image)
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	material.alpha_scissor_threshold = 0.5
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
	return material


func _virtualize(p_mesh: PrimitiveMesh, p_ngm_path: String) -> Mesh:
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


# The setting applies when a surface's material is next assigned: assign every material again.
func _reassign_materials() -> void:
	for instance: MeshInstance3D in _materials:
		instance.material_override = null
		instance.material_override = _materials[instance]


func _clear_extra_bins() -> void:
	for inst: MeshInstance3D in _extra_bins:
		_materials.erase(inst)
		remove_child(inst)
		inst.free()
	_extra_bins.clear()


func _spawn_extra_bins() -> void:
	_clear_extra_bins()
	# Fill all 64 programmable bins plus 1 overflow material (total 65 programmable materials).
	var needed := 65 - _programmable.size()
	for idx in needed:
		var mat := ShaderMaterial.new()
		mat.shader = _shared_masked_shader
		var col := idx % 30
		var row := idx / 30
		var x := -2.9 + float(col) * 0.2
		var y := -2.0 - float(row) * 0.2
		var inst := _add("ExtraBin_%d" % idx, _box_mesh, Transform3D(Basis.from_scale(Vector3.ONE * 0.06), Vector3(x, y, 0.4)), mat)
		_extra_bins.append(inst)


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", int(phase["raster_mode"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", bool(phase["bins"]))
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", bool(phase["buffer"]))
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", bool(phase["vis_only"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_backface", bool(phase["backface"]))
	if bool(phase["fill_64"]):
		_spawn_extra_bins()
	else:
		_clear_extra_bins()
	_reassign_materials()
	_phase_frame = 0
	_captures[phase["name"]] = []


func _route(p_instance: MeshInstance3D) -> Array:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_instance.get_instance())
	var route := String(st.get("route", ""))
	var reason := String(st.get("reason", ""))
	var surfaces: Array = st.get("surfaces", [])
	if not surfaces.is_empty():
		var s0: Dictionary = surfaces[0]
		route = String(s0.get("route", ""))
		reason = String(s0.get("reason", ""))
	_records[p_instance.name] = int(st.get("record_index", -1))
	return [route, reason]


func _process(_delta: float) -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var phase_name: String = phase["name"]
	if CAPTURE_FRAMES.has(_phase_frame - 1):
		var capture: Dictionary = _probe.get("capture")
		if capture.is_empty():
			_failures.append("%s: no capture at frame %d" % [phase_name, _phase_frame - 1])
		_captures[phase_name].append(capture)
		_probe.set("capture", {})
	if CAPTURE_FRAMES.has(_phase_frame):
		_probe.set("capture_armed", true)
	if _phase_frame == STATS_FRAME:
		_stats[phase_name] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		if not bool(phase["vis_only"]) and not bool(phase["backface"]) and not bool(phase["fill_64"]):
			_color_images[phase_name] = get_viewport().get_texture().get_image()
		var routes := {}
		for instance: MeshInstance3D in _programmable:
			routes[instance.name] = _route(instance)
		for rejected_name: String in _rejected:
			routes["Rejected_" + rejected_name] = _route(_rejected[rejected_name])
		routes["Background"] = _route(_background)
		_routes[phase_name] = routes
		if bool(phase["fill_64"]):
			_extra_routes.clear()
			for inst: MeshInstance3D in _extra_bins:
				_extra_routes.append(_route(inst))
		_phase_idx += 1
		if _phase_idx >= _phases.size():
			_finish()
			return
		_apply_phase()
		return
	_phase_frame += 1


# The pixels of one capture whose NextGeo instance is p_record.
func _pixels_of(p_capture: Dictionary, p_record: int) -> PackedByteArray:
	var size: Vector2i = p_capture["size"]
	var words: PackedInt32Array = p_capture["words"]
	var mask := PackedByteArray()
	mask.resize(size.x * size.y)
	for i in size.x * size.y:
		mask[i] = 1 if words[i * ProbeScript.WORDS_PER_PIXEL] == p_record else 0
	return mask


# The pixels of one capture whose NextGeo instance is any of p_records.
func _pixels_of_any(p_capture: Dictionary, p_records: Array[int]) -> PackedByteArray:
	var size: Vector2i = p_capture["size"]
	var words: PackedInt32Array = p_capture["words"]
	var mask := PackedByteArray()
	mask.resize(size.x * size.y)
	for i in size.x * size.y:
		mask[i] = 1 if p_records.has(words[i * ProbeScript.WORDS_PER_PIXEL]) else 0
	return mask


# [pixels, bounding-box pixels, fill] of instance p_record in one capture.
func _bbox_fill(p_capture: Dictionary, p_record: int) -> Array:
	var size: Vector2i = p_capture["size"]
	var mask := _pixels_of(p_capture, p_record)
	var pixels := 0
	var lo := Vector2i(size.x, size.y)
	var hi := Vector2i(-1, -1)
	for y in size.y:
		for x in size.x:
			if mask[y * size.x + x] == 1:
				pixels += 1
				lo = Vector2i(mini(lo.x, x), mini(lo.y, y))
				hi = Vector2i(maxi(hi.x, x), maxi(hi.y, y))
	if pixels == 0:
		return [0, 0, 0.0]
	var area := (hi.x - lo.x + 1) * (hi.y - lo.y + 1)
	return [pixels, area, snappedf(float(pixels) / float(area), 1e-4)]


func _compare_masks(p_reference: PackedByteArray, p_test: PackedByteArray) -> Dictionary:
	var reference := 0
	var test := 0
	var both := 0
	for i in p_reference.size():
		reference += p_reference[i]
		test += p_test[i]
		both += 1 if p_reference[i] == 1 and p_test[i] == 1 else 0
	var union := reference + test - both
	return {
		"reference": reference,
		"test": test,
		"both": both,
		"one_sided": reference + test - 2 * both,
		"iou": snappedf(float(both) / float(maxi(union, 1)), 1e-6),
	}


func _captures_ok(p_phase: String) -> bool:
	var captures: Array = _captures.get(p_phase, [])
	if captures.size() != CAPTURE_FRAMES.size():
		_failures.append("%s: expected %d captures, got %d" % [p_phase, CAPTURE_FRAMES.size(), captures.size()])
		return false
	for capture: Dictionary in captures:
		if capture.is_empty():
			return false
	return true


# Every pixel of every capture identical: instance, triangle and depth. The visible-cluster part of the
# visibility ID follows cull's atomic order, which varies run to run, so only its triangle bits are
# compared, as the #162 check does.
func _check_identical(p_reference: String, p_test: String) -> void:
	if not _captures_ok(p_reference) or not _captures_ok(p_test):
		return
	for k in CAPTURE_FRAMES.size():
		var a: Dictionary = _captures[p_reference][k]
		var b: Dictionary = _captures[p_test][k]
		var wa: PackedInt32Array = a["words"]
		var wb: PackedInt32Array = b["words"]
		var differ := 0
		for i in wa.size() / ProbeScript.WORDS_PER_PIXEL:
			var base := i * ProbeScript.WORDS_PER_PIXEL
			if wa[base] != wb[base] or (wa[base + 1] & 0x7F) != (wb[base + 1] & 0x7F) or wa[base + 2] != wb[base + 2]:
				differ += 1
		if differ != 0:
			_failures.append("%s vs %s: %d pixels differ (capture %d)" % [p_test, p_reference, differ, k])


func _record(p_name: String) -> int:
	return int(_records.get(p_name, -1))


func _programmable_records() -> Array[int]:
	var out: Array[int] = []
	for instance: MeshInstance3D in _programmable:
		out.append(_record(instance.name))
	return out


func _check_routes() -> void:
	for phase in _phases:
		var phase_name: String = phase["name"]
		var routes: Dictionary = _routes.get(phase_name, {})
		for instance: MeshInstance3D in _programmable:
			var got: Array = routes.get(instance.name, ["", ""])
			# #153 admits masked surfaces and #164 admits VertexOffset by default on the Nanite tier.
			var admitted := _expect_bins
			var expected_route := "virtual" if admitted else "legacy"
			var expected_reason := "NONE"
			if not admitted:
				expected_reason = "UNSUPPORTED_CUSTOM_SHADER" if instance.name == "VertexOffset" else "alpha"
			if got[0] != expected_route or got[1] != expected_reason:
				_failures.append("%s/%s: route %s/%s, expected %s/%s" % [phase_name, instance.name, got[0], got[1], expected_route, expected_reason])
		for rejected_name: String in _rejected:
			var got: Array = routes.get("Rejected_" + rejected_name, ["", ""])
			if got[0] != "legacy" or got[1] != "UNSUPPORTED_RASTER_FEATURE":
				_failures.append("%s/Rejected_%s: route %s/%s, expected legacy/UNSUPPORTED_RASTER_FEATURE" % [phase_name, rejected_name, got[0], got[1]])
		var background: Array = routes.get("Background", ["", ""])
		if background[0] != "virtual":
			_failures.append("%s/Background: route %s/%s, expected virtual" % [phase_name, background[0], background[1]])


func _check_stats() -> void:
	for phase in _phases:
		var phase_name: String = phase["name"]
		var stats: Dictionary = _stats.get(phase_name, {})
		if stats.is_empty() or not stats.has("hw_clusters"):
			_failures.append("%s: no NextGeo cut stats" % phase_name)
			continue
		var dd := int(stats.get("double_dispatched_surfaces", 0))
		var ms := int(stats.get("missing_surfaces", 0))
		if dd != 0 or ms != 0:
			_failures.append("%s: double_dispatched=%d missing=%d" % [phase_name, dd, ms])
		var admitted := bool(phase["bins"]) and _expect_bins
		var bins := int(stats.get("programmable_bins", -1))
		var sw := int(stats.get("programmable_sw_clusters", -1))
		var hw := int(stats.get("programmable_hw_clusters", -1))
		if not admitted:
			if bins != 0 or sw != 0 or hw != 0:
				_failures.append("%s: programmable bins in use (bins %d, sw %d, hw %d)" % [phase_name, bins, sw, hw])
			continue
		var expected_bins := 64 if bool(phase["fill_64"]) else _programmable.size()
		if bins != expected_bins:
			_failures.append("%s: %d programmable bins, expected %d" % [phase_name, bins, expected_bins])
		if float(phase["t"]) == 0.0:
			if sw != 0 or hw <= 0:
				_failures.append("%s: T = 0 must route every programmable cluster to hardware (sw %d, hw %d)" % [phase_name, sw, hw])
		elif sw <= 0:
			_failures.append("%s: expected software clusters in the programmable bins (sw %d, hw %d)" % [phase_name, sw, hw])
		var visible := int(stats.get("phase1_clusters", 0)) + int(stats.get("phase2_clusters", 0))
		var routed := int(stats.get("sw_clusters", 0)) + int(stats.get("hw_clusters", 0)) + sw + hw
		if routed != visible:
			_failures.append("%s: %d routed clusters, %d visible" % [phase_name, routed, visible])


func _check_shadows(p_test: String) -> Dictionary:
	var out := {}
	if not _captures_ok("legacy") or not _captures_ok(p_test):
		return out
	var ref_img: Image = _color_images.get("legacy")
	var tst_img: Image = _color_images.get(p_test)
	if ref_img == null or tst_img == null:
		_failures.append("%s: missing viewport color image for shadow check" % p_test)
		return out
	var cap_ref: Dictionary = _captures["legacy"][0]
	var cap_tst: Dictionary = _captures[p_test][0]
	var size: Vector2i = cap_ref["size"]
	var img_size := ref_img.get_size()
	var background := _record("Background")
	var bg_ref := _pixels_of(cap_ref, background)
	var bg_tst := _pixels_of(cap_tst, background)
	var shared := 0
	var ref_lit := 0
	var ref_shadowed := 0
	var mismatch := 0
	for y in size.y:
		var sy := mini(int((float(y) + 0.5) * float(img_size.y) / float(size.y)), img_size.y - 1)
		for x in size.x:
			var i := y * size.x + x
			if bg_ref[i] != 1 or bg_tst[i] != 1:
				continue
			shared += 1
			var sx := mini(int((float(x) + 0.5) * float(img_size.x) / float(size.x)), img_size.x - 1)
			var l_ref := ref_img.get_pixel(sx, sy).get_luminance() > 0.16
			var l_tst := tst_img.get_pixel(sx, sy).get_luminance() > 0.16
			if l_ref:
				ref_lit += 1
			else:
				ref_shadowed += 1
			if l_ref != l_tst:
				mismatch += 1
	out = {
		"shared_background_pixels": shared,
		"reference_lit": ref_lit,
		"reference_shadowed": ref_shadowed,
		"mismatch": mismatch,
	}
	if ref_lit < 100 or ref_shadowed < 50:
		_failures.append("%s: shadow check lacks lit (%d) or shadowed (%d) background pixels" % [p_test, ref_lit, ref_shadowed])
	var limit := maxi(32, int(0.02 * float(shared)))
	if mismatch > limit:
		_failures.append("%s: background shadow lighting differs on %d pixels (limit %d of %d)" % [p_test, mismatch, limit, shared])
	return out


# The background's pixels in p_test against the reference: the programmable materials must leave the
# same holes and silhouettes.
func _check_background(p_test: String) -> Array:
	var out := []
	if not _captures_ok("legacy") or not _captures_ok(p_test):
		return out
	var background := _record("Background")
	for k in CAPTURE_FRAMES.size():
		var c := _compare_masks(_pixels_of(_captures["legacy"][k], background), _pixels_of(_captures[p_test][k], background))
		out.append(c)
		var limit := maxi(MAX_BACKGROUND_ONE_SIDED_PIXELS, int(MAX_BACKGROUND_ONE_SIDED_FRACTION * float(c["reference"])))
		if int(c["reference"]) <= 0:
			_failures.append("%s: no background pixels in the reference" % p_test)
		if int(c["one_sided"]) > limit:
			_failures.append("%s: background differs from the reference on %d pixels (limit %d, reference %d, test %d)" % [p_test, c["one_sided"], limit, c["reference"], c["test"]])
	return out


func _finish() -> void:
	_check_routes()
	_check_stats()
	_check_identical("legacy", "legacy_again")
	var hw := []
	var sw := []
	var sw_vs_hw := []
	if _expect_bins:
		hw = _check_background("bins_hw")
		sw = _check_background("bins_sw")
		_report["bins_hw_shadows"] = _check_shadows("bins_hw")
		_report["bins_sw_shadows"] = _check_shadows("bins_sw")
		_check_identical("bins_sw", "bins_sw_buffer")
		_check_identical("bins_sw", "bins_vis_only")
		if _captures_ok("bins_hw") and _captures_ok("bins_sw"):
			var records := _programmable_records()
			for k in CAPTURE_FRAMES.size():
				var c := _compare_masks(_pixels_of_any(_captures["bins_hw"][k], records), _pixels_of_any(_captures["bins_sw"][k], records))
				sw_vs_hw.append(c)
				var limit := maxi(MAX_SW_HW_ONE_SIDED_PIXELS, int(MAX_SW_HW_ONE_SIDED_FRACTION * float(c["reference"])))
				if int(c["reference"]) <= 0:
					_failures.append("bins_hw: no programmable pixels")
				if int(c["one_sided"]) > limit:
					_failures.append("bins_sw vs bins_hw: programmable coverage differs on %d pixels (limit %d)" % [c["one_sided"], limit])
			# The masks are real: each masked instance fills well under its screen bounding box.
			for phase_name in ["bins_hw", "bins_sw"]:
				for entry in [["MaskedStripes", MAX_STRIPES_FILL], ["MaskedChecker", MAX_CHECKER_FILL], ["TexturedScissor", MAX_STRIPES_FILL], ["TwoSidedFragToLight", MAX_STRIPES_FILL], ["NearPlaneMasked", MAX_STRIPES_FILL]]:
					var fill := _bbox_fill(_captures[phase_name][0], _record(entry[0]))
					_report["%s_%s_fill" % [phase_name, entry[0]]] = fill
					if int(fill[0]) < 20 or float(fill[2]) < MIN_MASK_FILL or float(fill[2]) > float(entry[1]):
						_failures.append("%s/%s: covers %.3f of its bounding box (%d of %d pixels), expected %.2f to %.2f" % [phase_name, entry[0], fill[2], fill[0], fill[1], MIN_MASK_FILL, entry[1]])
				var bf_off := _bbox_fill(_captures[phase_name][0], _record("BackfaceProbe"))
				if int(bf_off[0]) != 0:
					_failures.append("%s/BackfaceProbe: expected 0 pixels with debug_backface off, got %d" % [phase_name, bf_off[0]])
		for bf_phase in ["bins_backface_hw", "bins_backface_sw"]:
			if _captures_ok(bf_phase):
				var bf_on := _bbox_fill(_captures[bf_phase][0], _record("BackfaceProbe"))
				_report["%s_BackfaceProbe_fill" % bf_phase] = bf_on
				if int(bf_on[0]) < 20 or float(bf_on[2]) < MIN_MASK_FILL or float(bf_on[2]) > MAX_STRIPES_FILL:
					_failures.append("%s/BackfaceProbe: expected visible striped backface, got %s" % [bf_phase, str(bf_on)])
		# 64-bin fill: 64 - _programmable.size() extra materials route virtual/NONE and exactly one, the 65th
		# programmable material to reach a bin, routes legacy/alpha. Bins go to surfaces in the order the renderer
		# builds their caches, which is the dirty-instance list's (most recently queued first), not the spawn
		# order, so the check counts routes instead of naming the overflowing instance.
		var fit_count := 64 - _programmable.size()
		if _extra_routes.size() != fit_count + 1:
			_failures.append("bins_64: expected %d extra routes, got %d" % [fit_count + 1, _extra_routes.size()])
		else:
			var admitted := 0
			var overflowed := 0
			for idx in _extra_routes.size():
				var r: Array = _extra_routes[idx]
				if r[0] == "virtual" and r[1] == "NONE":
					admitted += 1
				elif r[0] == "legacy" and r[1] == "alpha":
					overflowed += 1
				else:
					_failures.append("bins_64/ExtraBin_%d: route %s/%s, expected virtual/NONE or legacy/alpha" % [idx, r[0], r[1]])
			if admitted != fit_count or overflowed != 1:
				_failures.append("bins_64: %d extra materials got a bin and %d stayed legacy/alpha, expected %d and 1" % [admitted, overflowed, fit_count])
	else:
		# Below the Nanite tier the setting admits nothing: every phase without vis_only/backface/extra is the reference.
		for test in ["bins_hw", "bins_sw", "bins_sw_buffer"]:
			_check_identical("legacy", test)
	var stats_report := {}
	for phase_name in _stats:
		var s: Dictionary = _stats[phase_name]
		stats_report[phase_name] = {
			"raster_mode": s.get("raster_mode"),
			"raster_reason": s.get("raster_reason"),
			"vis64_storage": s.get("vis64_storage"),
			"programmable_bins": s.get("programmable_bins"),
			"sw_clusters": s.get("sw_clusters"),
			"hw_clusters": s.get("hw_clusters"),
			"programmable_sw_clusters": s.get("programmable_sw_clusters"),
			"programmable_hw_clusters": s.get("programmable_hw_clusters"),
		}
	_report["stats"] = stats_report
	_report["routes"] = _routes
	_report["records"] = _records
	_report["bins_hw_background_vs_legacy"] = hw
	_report["bins_sw_background_vs_legacy"] = sw
	_report["bins_sw_vs_bins_hw"] = sw_vs_hw
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
