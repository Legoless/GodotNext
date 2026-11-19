extends Node3D
## #295: the AVBOIT draw-order fixture. Overlapping translucent quads (unshaded) and an opaque bar
## between the middle and the far quad, drawn in different orders (forced with render_priority),
## with sorted translucency and with AVBOIT. A PR-basics check, not a PQ quality or timing
## measurement (FLIP and cost run in PQ with GL, #134).
## Two sets of blend modes:
## - the mix set: three mix quads, near, middle and far;
## - the blend set: the near quad adds, the middle one is premultiplied alpha, the far one mixes, and
##   a fourth quad in front of them all multiplies (mul is drawn after the AVBOIT resolve, in the
##   sorted order, so it sits in front where the sorted pass would also draw it last).
## It passes when:
## - the sorted pass changes with the draw order, so the fixture does reorder the quads;
## - AVBOIT gives the same image in every order: at most ORDER_MAX_LEVELS apart per channel;
## - AVBOIT is not the sorted pass's wrong-order image, so the AVBOIT passes ran;
## - AVBOIT stays close to the correctly sorted image: on average within SORTED_MEAN_LEVELS (the
##   occlusion grid is 4 pixels square, so the quads' edges differ over a few pixels);
## - one quad alone looks as in the sorted pass (it does not dim itself): within SINGLE_MAX_LEVELS;
## - with MSAA the view falls back to sorted translucency: the same image as the sorted pass;
## - a compositor effect at EFFECT_CALLBACK_TYPE_POST_TRANSPARENT reads the same viewport size from
##   the scene data uniform buffer with AVBOIT as with the sorted pass (the AVBOIT pre-passes set up
##   grid-sized scene data of their own, which must not be what the frame is left with);
## - a shadow_to_opacity catcher, whose alpha in the color pass is 0 here (no light, no ambient
##   light), does not hide the far quad behind it: within SINGLE_MAX_LEVELS of the sorted pass;
## - two close parallel quads seen at a steep angle (the depth changes by more than the gap between
##   them across one cell) give the same AVBOIT image in both draw orders, stay within
##   SORTED_MEAN_LEVELS of the sorted pass on average, and differ from it by more than 8 levels on at
##   most TILTED_OVER_8_MAX of the pixels (their edges; classifying each fragment by its depth at the
##   cell's center keeps the inside of the overlap right).
## Printed but not gated, as a known limit of the build (servers/rendering/renderer_rd/effects/avboit.h):
## - mul behind mix: a mul quad behind the middle quad. The remainder pass draws it over the nearer
##   AVBOIT surface, where the sorted pass draws the middle quad over it.
## Levels are 8-bit levels of the viewport's image. The runner also checks the engine's
## "AVBOIT: on" and "AVBOIT: sorted (reason MSAA)" lines.

const METHOD := "rendering/transparency/method"
const SORTED := 0
const AVBOIT := 1
const ORDER_MAX_LEVELS := 2
const SORTED_MEAN_LEVELS := 1.0
const WRONG_ORDER_MIN_MEAN_LEVELS := 3.0
const SINGLE_MAX_LEVELS := 2
const MAX_SETTLE_TRIES := 40

# render_priority of the near, middle, far and front quad. Equal priorities leave the depth sort,
# which is back to front; a higher priority is drawn later.
const ORDER_SORTED := [0, 0, 0, 0]
const ORDER_NEAR_FIRST := [0, 1, 2, 0]
const ORDER_MIDDLE_FIRST := [2, 0, 1, 0]
# The blend set front to back: the front quad first, the far quad last.
const ORDER_FRONT_FIRST := [1, 2, 3, 0]

const MIX_SET := [
	BaseMaterial3D.BLEND_MODE_MIX,
	BaseMaterial3D.BLEND_MODE_MIX,
	BaseMaterial3D.BLEND_MODE_MIX,
	BaseMaterial3D.BLEND_MODE_MUL,
]
const BLEND_SET := [
	BaseMaterial3D.BLEND_MODE_ADD,
	BaseMaterial3D.BLEND_MODE_PREMULT_ALPHA,
	BaseMaterial3D.BLEND_MODE_MIX,
	BaseMaterial3D.BLEND_MODE_MUL,
]

# The tilted pair: two parallel quads TILTED_GAP apart, turned TILTED_ANGLE_DEGREES away from facing
# the camera, so that across one 4-pixel cell the depth changes by more than the gap between them.
const TILTED_ANGLE_DEGREES := 75.0
const TILTED_GAP := 0.01
# The share of pixels that may differ from the sorted pass by more than 8 levels: the quads' edges.
const TILTED_OVER_8_MAX := 0.01


## Copies the start of the scene data uniform buffer after the transparent pass.
class SceneDataProbe extends CompositorEffect:
	const VEC4_COUNT := 64
	const COPY_SHADER := "#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SceneDataStart {
	vec4 data[64];
}
scene_data_start;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Copy {
	vec4 data[64];
}
copy;

void main() {
	copy.data[gl_LocalInvocationID.x] = scene_data_start.data[gl_LocalInvocationID.x];
}
"

	var _mutex := Mutex.new()
	var _floats := PackedFloat32Array()
	var _internal_size := Vector2i()
	var _shader := RID()
	var _pipeline := RID()
	var _buffer := RID()
	var _failed := false

	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
		needs_motion_vectors = false

	## The last frame's copy and its internal size: {"floats", "size"}, or {} before the first.
	func snapshot() -> Dictionary:
		_mutex.lock()
		var result := {}
		if not _floats.is_empty():
			result = {"floats": _floats.duplicate(), "size": _internal_size}
		_mutex.unlock()
		return result

	func _ensure(p_rd: RenderingDevice) -> bool:
		if _pipeline.is_valid():
			return true
		if _failed or p_rd == null:
			return false
		_failed = true
		var source := RDShaderSource.new()
		source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
		source.source_compute = COPY_SHADER
		var spirv := p_rd.shader_compile_spirv_from_source(source)
		if spirv == null or not spirv.compile_error_compute.is_empty():
			push_error("AVBOIT scene data probe: the copy shader did not compile")
			return false
		_shader = p_rd.shader_create_from_spirv(spirv, "AVBOITSceneDataProbe")
		if not _shader.is_valid():
			return false
		_pipeline = p_rd.compute_pipeline_create(_shader)
		var zeros := PackedByteArray()
		zeros.resize(VEC4_COUNT * 16)
		_buffer = p_rd.storage_buffer_create(zeros.size(), zeros)
		_failed = not (_pipeline.is_valid() and _buffer.is_valid())
		return not _failed

	func _render_callback(_callback_type: int, p_render_data: RenderData) -> void:
		var scene_data := p_render_data.get_render_scene_data()
		var buffers := p_render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if scene_data == null or buffers == null:
			return
		var uniform_buffer := scene_data.get_uniform_buffer()
		var rd := RenderingServer.get_rendering_device()
		if not uniform_buffer.is_valid() or not _ensure(rd):
			return
		var scene_uniform := RDUniform.new()
		scene_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
		scene_uniform.binding = 0
		scene_uniform.add_id(uniform_buffer)
		var copy_uniform := RDUniform.new()
		copy_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		copy_uniform.binding = 1
		copy_uniform.add_id(_buffer)
		var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, [scene_uniform, copy_uniform])
		if not uniform_set.is_valid():
			return
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list, _pipeline)
		rd.compute_list_bind_uniform_set(list, uniform_set, 0)
		rd.compute_list_dispatch(list, 1, 1, 1)
		rd.compute_list_end()
		var floats := rd.buffer_get_data(_buffer).to_float32_array()
		_mutex.lock()
		_floats = floats
		_internal_size = buffers.get_internal_size()
		_mutex.unlock()

	func _notification(p_what: int) -> void:
		if p_what != NOTIFICATION_PREDELETE:
			return
		var rd := RenderingServer.get_rendering_device()
		if rd == null:
			return
		for rid: RID in [_buffer, _pipeline, _shader]:
			if rid.is_valid():
				rd.free_rid(rid)


var _quads: Array[MeshInstance3D] = []
var _materials: Array[StandardMaterial3D] = []
# The extra placements, hidden unless a capture names them: name -> MeshInstance3D.
var _extras := {}
var _unsettled := PackedStringArray()
var _probe: SceneDataProbe


func _ready() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.25, 0.25, 0.3)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var camera := Camera3D.new()
	camera.fov = 50.0
	add_child(camera)
	camera.make_current()
	_probe = SceneDataProbe.new()
	var compositor := Compositor.new()
	compositor.compositor_effects = [_probe]
	camera.compositor = compositor

	var bar := MeshInstance3D.new()
	var bar_mesh := BoxMesh.new()
	bar_mesh.size = Vector3(0.5, 6.0, 0.1)
	bar.mesh = bar_mesh
	var bar_material := StandardMaterial3D.new()
	bar_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	bar_material.albedo_color = Color(0.9, 0.8, 0.2)
	bar.material_override = bar_material
	bar.position = Vector3(1.4, 0.0, -3.8)
	add_child(bar)

	# Near, middle, far: each overlaps the others, and all three overlap in the middle of the screen.
	# The front quad, in front of them all, covers part of that overlap.
	var specs := [
		[Vector3(-0.25, 0.15, -2.0), 1.1, Color(1.0, 0.1, 0.1, 0.5)],
		[Vector3(0.15, -0.2, -3.0), 1.6, Color(0.1, 1.0, 0.1, 0.5)],
		[Vector3(0.5, 0.3, -4.5), 2.4, Color(0.1, 0.2, 1.0, 0.6)],
		[Vector3(0.05, 0.0, -1.5), 0.6, Color(0.6, 0.9, 1.0, 1.0)],
	]
	for spec: Array in specs:
		var quad := _add_quad(Vector2(spec[1], spec[1]), spec[0], _unshaded(spec[2], BaseMaterial3D.BLEND_MODE_MIX))
		_quads.append(quad)
		_materials.append(quad.material_override as StandardMaterial3D)

	# The tilted pair, near and far.
	var tilt := Basis(Vector3.UP, deg_to_rad(TILTED_ANGLE_DEGREES))
	var tilted_normal := tilt * Vector3.BACK
	var tilted_center := Vector3(0.0, 0.0, -3.0)
	_add_extra("tilted_near", Vector2(3.0, 1.2), tilted_center, _unshaded(Color(1.0, 0.2, 0.1, 0.5), BaseMaterial3D.BLEND_MODE_MIX), tilt)
	_add_extra("tilted_far", Vector2(3.0, 1.2), tilted_center - tilted_normal * TILTED_GAP, _unshaded(Color(0.1, 0.3, 1.0, 0.5), BaseMaterial3D.BLEND_MODE_MIX), tilt)
	# A mul quad behind the middle quad.
	_add_extra("mul_behind", Vector2(1.0, 1.0), Vector3(-0.2, 0.1, -3.4), _unshaded(Color(0.6, 0.9, 1.0, 1.0), BaseMaterial3D.BLEND_MODE_MUL))
	# A shadow_to_opacity catcher in front of the far quad: lit (the default shading), so its alpha in
	# the color pass is the light it catches, here none. Its material ALPHA is 1.
	var catcher_material := StandardMaterial3D.new()
	catcher_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	catcher_material.shadow_to_opacity = true
	catcher_material.albedo_color = Color(0.9, 0.9, 0.9, 1.0)
	_add_extra("catcher", Vector2(1.2, 1.2), Vector3(0.5, 0.3, -4.0), catcher_material)
	_run()


func _unshaded(p_color: Color, p_blend_mode: BaseMaterial3D.BlendMode) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.blend_mode = p_blend_mode
	material.albedo_color = p_color
	return material


func _add_quad(p_size: Vector2, p_position: Vector3, p_material: StandardMaterial3D, p_basis := Basis()) -> MeshInstance3D:
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = p_size
	quad.mesh = mesh
	quad.material_override = p_material
	quad.transform = Transform3D(p_basis, p_position)
	add_child(quad)
	return quad


func _add_extra(p_name: String, p_size: Vector2, p_position: Vector3, p_material: StandardMaterial3D, p_basis := Basis()) -> void:
	var quad := _add_quad(p_size, p_position, p_material, p_basis)
	quad.visible = false
	_extras[p_name] = quad


# p_extras: the extra placements to show, name -> render_priority.
func _configure(p_method: int, p_order: Array, p_visible: Array, p_msaa: Viewport.MSAA, p_blend_modes: Array = MIX_SET, p_extras: Dictionary = {}) -> void:
	ProjectSettings.set_setting(METHOD, p_method)
	get_viewport().msaa_3d = p_msaa
	for i in _quads.size():
		_materials[i].blend_mode = p_blend_modes[i]
		_materials[i].render_priority = p_order[i]
		_quads[i].visible = p_visible[i]
	for extra_name: String in _extras:
		var quad: MeshInstance3D = _extras[extra_name]
		quad.visible = p_extras.has(extra_name)
		(quad.material_override as StandardMaterial3D).render_priority = int(p_extras.get(extra_name, 0))


# Renders until two captures four frames apart are the same, so pipelines that were still compiling
# (the ubershader draws meanwhile) do not count.
func _capture(p_name: String, p_method: int, p_order: Array, p_visible: Array, p_msaa: Viewport.MSAA, p_blend_modes: Array = MIX_SET, p_extras: Dictionary = {}) -> Image:
	_configure(p_method, p_order, p_visible, p_msaa, p_blend_modes, p_extras)
	var previous: Image = null
	for i in MAX_SETTLE_TRIES:
		for frame in 4:
			await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		image.convert(Image.FORMAT_RGBA8)
		if previous != null and image.get_data() == previous.get_data():
			image.save_png("user://avboit_%s.png" % p_name)
			return image
		previous = image
	_unsettled.append(p_name)
	return previous


func _diff(p_a: Image, p_b: Image) -> Dictionary:
	var a := p_a.get_data()
	var b := p_b.get_data()
	if a.size() != b.size() or a.size() == 0:
		return {"max": 255, "mean": 255.0, "over_2": 1.0, "over_8": 1.0}
	var max_levels := 0
	var total := 0
	var over := 0
	var over_8 := 0
	var pixels := a.size() / 4
	for i in range(0, a.size(), 4):
		var d := maxi(maxi(absi(a[i] - b[i]), absi(a[i + 1] - b[i + 1])), absi(a[i + 2] - b[i + 2]))
		max_levels = maxi(max_levels, d)
		total += d
		if d > 2:
			over += 1
		if d > 8:
			over_8 += 1
	return {"max": max_levels, "mean": float(total) / float(pixels), "over_2": float(over) / float(pixels), "over_8": float(over_8) / float(pixels)}


# Where the sorted pass's scene data holds the view's viewport size and screen pixel size, and what
# AVBOIT's holds there.
func _scene_data_check(p_sorted: Dictionary, p_avboit: Dictionary) -> Dictionary:
	var result := {"index": -1, "sorted": [], "avboit": []}
	if p_sorted.is_empty() or p_avboit.is_empty():
		return result
	var floats: PackedFloat32Array = p_sorted.floats
	var size: Vector2i = p_sorted.size
	for i in floats.size() - 3:
		if floats[i] == float(size.x) and floats[i + 1] == float(size.y) and is_equal_approx(floats[i + 2], 1.0 / float(size.x)) and is_equal_approx(floats[i + 3], 1.0 / float(size.y)):
			result.index = i
			break
	if result.index < 0:
		return result
	var avboit_floats: PackedFloat32Array = p_avboit.floats
	for i in 4:
		result.sorted.append(floats[result.index + i])
		result.avboit.append(avboit_floats[result.index + i])
	return result


func _run() -> void:
	# Let the scene and its pipelines come up first.
	for i in 30:
		await RenderingServer.frame_post_draw
	var all := [true, true, true, false]
	var middle_only := [false, true, false, false]
	var far_only := [false, false, true, false]
	var none := [false, false, false, false]
	var all_blend := [true, true, true, true]
	var off := Viewport.MSAA_DISABLED
	var sorted_ref := await _capture("sorted", SORTED, ORDER_SORTED, all, off)
	var scene_data_sorted := _probe.snapshot()
	var sorted_near_first := await _capture("sorted_near_first", SORTED, ORDER_NEAR_FIRST, all, off)
	var avboit_sorted := await _capture("avboit", AVBOIT, ORDER_SORTED, all, off)
	var scene_data_avboit := _probe.snapshot()
	var avboit_near_first := await _capture("avboit_near_first", AVBOIT, ORDER_NEAR_FIRST, all, off)
	var avboit_middle_first := await _capture("avboit_middle_first", AVBOIT, ORDER_MIDDLE_FIRST, all, off)
	var sorted_single := await _capture("sorted_single", SORTED, ORDER_SORTED, middle_only, off)
	var avboit_single := await _capture("avboit_single", AVBOIT, ORDER_SORTED, middle_only, off)
	var sorted_msaa := await _capture("sorted_msaa", SORTED, ORDER_NEAR_FIRST, all, Viewport.MSAA_2X)
	var avboit_msaa := await _capture("avboit_msaa", AVBOIT, ORDER_NEAR_FIRST, all, Viewport.MSAA_2X)
	var sorted_blend := await _capture("sorted_blend", SORTED, ORDER_SORTED, all_blend, off, BLEND_SET)
	var sorted_blend_front_first := await _capture("sorted_blend_front_first", SORTED, ORDER_FRONT_FIRST, all_blend, off, BLEND_SET)
	var avboit_blend := await _capture("avboit_blend", AVBOIT, ORDER_SORTED, all_blend, off, BLEND_SET)
	var avboit_blend_front_first := await _capture("avboit_blend_front_first", AVBOIT, ORDER_FRONT_FIRST, all_blend, off, BLEND_SET)
	# The tilted pair: the near quad drawn last is the right order, the far one drawn last the wrong one.
	var tilted_right := {"tilted_near": 1, "tilted_far": 0}
	var tilted_wrong := {"tilted_near": 0, "tilted_far": 1}
	var sorted_tilted := await _capture("sorted_tilted", SORTED, ORDER_SORTED, none, off, MIX_SET, tilted_right)
	var sorted_tilted_wrong := await _capture("sorted_tilted_wrong", SORTED, ORDER_SORTED, none, off, MIX_SET, tilted_wrong)
	var avboit_tilted := await _capture("avboit_tilted", AVBOIT, ORDER_SORTED, none, off, MIX_SET, tilted_right)
	var avboit_tilted_wrong := await _capture("avboit_tilted_wrong", AVBOIT, ORDER_SORTED, none, off, MIX_SET, tilted_wrong)
	var sorted_mul_behind := await _capture("sorted_mul_behind", SORTED, ORDER_SORTED, middle_only, off, MIX_SET, {"mul_behind": 0})
	var avboit_mul_behind := await _capture("avboit_mul_behind", AVBOIT, ORDER_SORTED, middle_only, off, MIX_SET, {"mul_behind": 0})
	var sorted_catcher := await _capture("sorted_catcher", SORTED, ORDER_SORTED, far_only, off, MIX_SET, {"catcher": 0})
	var avboit_catcher := await _capture("avboit_catcher", AVBOIT, ORDER_SORTED, far_only, off, MIX_SET, {"catcher": 0})
	_configure(SORTED, ORDER_SORTED, all, off)

	var metrics := {
		"sorted_order": _diff(sorted_ref, sorted_near_first),
		"avboit_order_near_first": _diff(avboit_sorted, avboit_near_first),
		"avboit_order_middle_first": _diff(avboit_sorted, avboit_middle_first),
		"avboit_vs_wrong_order": _diff(avboit_near_first, sorted_near_first),
		"avboit_vs_sorted": _diff(avboit_sorted, sorted_ref),
		"single": _diff(avboit_single, sorted_single),
		"msaa": _diff(avboit_msaa, sorted_msaa),
		"blend_sorted_order": _diff(sorted_blend, sorted_blend_front_first),
		"blend_avboit_order": _diff(avboit_blend, avboit_blend_front_first),
		"blend_avboit_vs_wrong_order": _diff(avboit_blend_front_first, sorted_blend_front_first),
		"blend_avboit_vs_sorted": _diff(avboit_blend, sorted_blend),
		"scene_data_post_transparent": _scene_data_check(scene_data_sorted, scene_data_avboit),
		"catcher_avboit_vs_sorted": _diff(avboit_catcher, sorted_catcher),
		"tilted_sorted_order": _diff(sorted_tilted, sorted_tilted_wrong),
		"tilted_avboit_order": _diff(avboit_tilted, avboit_tilted_wrong),
		"tilted_avboit_vs_sorted": _diff(avboit_tilted, sorted_tilted),
		"report_mul_behind_avboit_vs_sorted": _diff(avboit_mul_behind, sorted_mul_behind),
	}
	var failures := PackedStringArray()
	if not _unsettled.is_empty():
		failures.append("captures did not settle: %s" % ", ".join(_unsettled))
	for key in ["sorted_order", "blend_sorted_order", "tilted_sorted_order"]:
		if metrics[key].mean < WRONG_ORDER_MIN_MEAN_LEVELS:
			failures.append("%s: the sorted pass does not change with the draw order; the fixture does not reorder" % key)
	for key in ["avboit_order_near_first", "avboit_order_middle_first", "blend_avboit_order", "tilted_avboit_order"]:
		if metrics[key].max > ORDER_MAX_LEVELS:
			failures.append("%s: AVBOIT differs by %d levels between draw orders" % [key, metrics[key].max])
	for key in ["avboit_vs_wrong_order", "blend_avboit_vs_wrong_order"]:
		if metrics[key].mean < WRONG_ORDER_MIN_MEAN_LEVELS:
			failures.append("%s: AVBOIT gives the sorted pass's wrong-order image; the AVBOIT passes did not run" % key)
	for key in ["avboit_vs_sorted", "blend_avboit_vs_sorted", "tilted_avboit_vs_sorted"]:
		if metrics[key].mean > SORTED_MEAN_LEVELS:
			failures.append("%s: AVBOIT is %.3f levels from the sorted image on average" % [key, metrics[key].mean])
	if metrics.tilted_avboit_vs_sorted.over_8 > TILTED_OVER_8_MAX:
		failures.append("tilted: AVBOIT differs from the sorted image by more than 8 levels on %.2f%% of the pixels" % (100.0 * metrics.tilted_avboit_vs_sorted.over_8))
	if metrics.single.max > SINGLE_MAX_LEVELS:
		failures.append("one quad alone differs from the sorted pass by %d levels" % metrics.single.max)
	if metrics.msaa.max != 0:
		failures.append("with MSAA, AVBOIT did not fall back to the sorted image")
	var scene_data: Dictionary = metrics.scene_data_post_transparent
	if scene_data.index < 0:
		failures.append("the POST_TRANSPARENT probe found no viewport size in the sorted pass's scene data")
	elif scene_data.avboit != scene_data.sorted:
		failures.append("with AVBOIT, POST_TRANSPARENT effects read another viewport size from the scene data: %s, not %s" % [str(scene_data.avboit), str(scene_data.sorted)])
	if metrics.catcher_avboit_vs_sorted.max > SINGLE_MAX_LEVELS:
		failures.append("a shadow_to_opacity catcher with alpha 0 changes the AVBOIT image by %d levels" % metrics.catcher_avboit_vs_sorted.max)
	var size := get_viewport().get_visible_rect().size
	var result := {
		"test": "avboit",
		"pass": failures.is_empty(),
		"size": [int(size.x), int(size.y)],
		"tolerances": {"order_max": ORDER_MAX_LEVELS, "sorted_mean": SORTED_MEAN_LEVELS, "wrong_order_min_mean": WRONG_ORDER_MIN_MEAN_LEVELS, "single_max": SINGLE_MAX_LEVELS, "tilted_over_8_max": TILTED_OVER_8_MAX},
		"metrics": metrics,
		"failures": failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if failures.is_empty() else 1)
