extends Node3D

## Specular trace size and miss distance check for #274.
##
## Loads a scene (--scene, the Roughness Corridor by default) with NextGI on and views it through
## --camera (CaptureCam). After --warmup frames a CompositorEffect copies RB_TEX_SPECULAR_HIT, the
## depth and the prepass normal-roughness to the CPU. Against the active preset's reflection divisor
## (Epic 1, High 2, Medium 4) the check requires:
## - the hit distance texture has the trace size, floor(view / divisor) per axis, also as reported
##   in reflection_trace_width/height;
## - reflection_rays, the traced counter, equals the trace pixels whose sampled view pixel has depth
##   (the kernel samples view pixel (t + 0.5) / trace * view): at Epic on a closed room that is
##   every view pixel;
## - every texel whose sampled pixel is sky or rougher than the trace limit holds the miss value;
## - texels holding a distance a miss stops at are no more than a rounding share (a hit within half
##   an R16F step of it), not the bulk of the misses: a ray that ends there without a hit writes
##   65504 (before #274 it wrote that distance). The stops are the WRC handoff (1.5 m x 1, 2, 4 or 8)
##   and rt_scene_radius on the hardware tier, rt_scene_radius on the software tier, which traces
##   without the handoff.
## --require-quarters: each quarter of the texture holds finite distances, so every quarter was
## traced. --require-sky-misses: some traced texels hold 65504 (reflections that leave the scene).
## --roughness=R sets every BaseMaterial3D to roughness R (Color Box Room A is rough everywhere,
## so it would trace nothing). --pitch=D tilts the camera down by D degrees. --fog=true turns on
## exponential depth fog, which NextGI also applies at reflection hits. For an A/B between binaries,
## --dump=PATH writes the hit texture as two int32 (width, height) and width x height float32, and
## --capture=PATH saves the frame shown right after the copy as a PNG.
##
## Front layer (#276). --front-layer=true turns on the Environment's front-layer translucency
## reflections (false turns them off). With them on, the front layer must have traced (dispatches
## and rays in the tier info) and its own hit distance target must have the trace size: its gather
## writes there, not into the view's texture. --compare=PATH compares the view's hit texture with a
## --dump from another run (same scene, quality and frame count, so the same frame) texel by texel,
## bit for bit, outside the front layer: outside the screen bounds of every visible mesh with an
## alpha-blended material, grown by 2 pixels (a trace texel is outside when every view pixel it may
## sample is). Every outside texel must match, and the dump must hold at least
## --min-compared-hits (1) finite distances outside, or the comparison would not see an overwrite.
## Texels inside are compared and reported, not required to match.
##
## Run through run_reflect_trace_check.sh, which sets the NextGI quality. Prints one
## "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs = preload("res://scenes/scene_args.gd")

const MISS_DISTANCE := 65504.0
const HANDOFF_DISTANCES := [1.5, 3.0, 6.0, 12.0]
const DIVISORS := {"EPIC": 1, "HIGH": 2, "MEDIUM": 4}
const MAX_TRACE_ROUGHNESS := {"EPIC": 0.4, "HIGH": 0.4, "MEDIUM": 0.25}

var _failures := PackedStringArray()
var _result := {"test": "nextgi_reflect_trace"}
var _probe: TraceProbe
var _environment: Environment
var _scene: Node
var _camera: Camera3D


func _ready() -> void:
	var scene_path := SceneArgs.value("--scene", "res://scenes/roughness_corridor/roughness_corridor.tscn")
	_result["scene"] = scene_path
	var packed := load(scene_path) as PackedScene
	if packed == null:
		_finish_with("cannot load %s" % scene_path)
		return
	var scene := packed.instantiate()
	scene.set_meta("skip_runtime", true)
	add_child(scene)
	_scene = scene
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world == null or world.environment == null:
		_finish_with("the scene has no WorldEnvironment")
		return
	_environment = world.environment.duplicate()
	_environment.nextgi_enabled = true
	_environment.sdfgi_enabled = false
	_environment.ssr_enabled = false
	if SceneArgs.value("--fog", "false") == "true":
		_environment.fog_enabled = true
		_environment.fog_mode = Environment.FOG_MODE_EXPONENTIAL
		_environment.fog_density = 0.05
		_environment.nextgi_fog_on_reflection_hits = true
		_result["fog"] = true
	var front_layer := SceneArgs.value("--front-layer", "")
	if not front_layer.is_empty():
		_environment.nextgi_translucency_front_layer_reflections = front_layer == "true"
		_result["front_layer"] = front_layer == "true"
	world.environment = _environment
	var roughness := float(SceneArgs.value("--roughness", "-1"))
	if roughness >= 0.0:
		_result["roughness_override"] = roughness
		_override_roughness(scene, roughness)
	var camera_name := SceneArgs.value("--camera", "CaptureCam")
	var camera := scene.get_node_or_null(camera_name) as Camera3D
	if camera == null:
		_finish_with("the scene has no camera %s" % camera_name)
		return
	_result["camera"] = camera_name
	var pitch := float(SceneArgs.value("--pitch", "0"))
	if pitch != 0.0:
		_result["pitch"] = pitch
		camera.rotate_object_local(Vector3.RIGHT, -deg_to_rad(pitch))
	camera.current = true
	_camera = camera
	_probe = TraceProbe.new()
	var compositor := Compositor.new()
	var effects := compositor.compositor_effects
	effects.append(_probe)
	compositor.compositor_effects = effects
	camera.compositor = compositor
	_run()


func _run() -> void:
	for i in maxi(SceneArgs.int_value("--warmup", 90), 1):
		await RenderingServer.frame_post_draw
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var quality := String(info.get("quality", ""))
	_result["tier"] = String(info.get("tier", ""))
	_result["quality"] = quality
	var expected_tier := SceneArgs.value("--expected-tier", "HWRT")
	if _result["tier"] != expected_tier:
		_failures.append("tier is %s, expected %s" % [_result["tier"], expected_tier])
	var expected_quality := SceneArgs.value("--expect-quality", "")
	if not expected_quality.is_empty() and quality != expected_quality:
		_failures.append("quality is %s, expected %s" % [quality, expected_quality])
	if not DIVISORS.has(quality):
		_finish_with("no reflection divisor for quality %s" % quality)
		return
	var divisor: int = DIVISORS[quality]
	var max_roughness := minf(MAX_TRACE_ROUGHNESS[quality], _environment.nextgi_max_roughness_to_trace)
	_result["divisor"] = divisor

	_probe.request(SceneArgs.value("--dump", ""))
	var taken := {}
	for i in 8:
		await RenderingServer.frame_post_draw
		taken = _probe.take()
		if not taken.is_empty():
			break
	if not _probe.error.is_empty():
		_failures.append(_probe.error)
	var capture_path := SceneArgs.value("--capture", "")
	if not capture_path.is_empty():
		var image := get_viewport().get_texture().get_image()
		if image == null or image.save_png(capture_path) != OK:
			_failures.append("cannot save the frame to %s" % capture_path)
		else:
			_result["capture"] = capture_path
	for texture_name in ["hit", "depth", "normal_roughness"]:
		if not taken.has(texture_name):
			_finish_with("the %s texture could not be read" % texture_name)
			return
	var view: Vector2i = taken["view_size"]
	var hit_size: Vector2i = taken["hit_size"]
	var expected_trace := Vector2i(maxi(view.x / divisor, 1), maxi(view.y / divisor, 1))
	_result["view_size"] = [view.x, view.y]
	_result["hit_texture_size"] = [hit_size.x, hit_size.y]
	_result["expected_trace_size"] = [expected_trace.x, expected_trace.y]
	if hit_size != expected_trace:
		_failures.append("the hit distance texture is %dx%d, expected the trace size %dx%d (view %dx%d / %d)" % [hit_size.x, hit_size.y, expected_trace.x, expected_trace.y, view.x, view.y, divisor])
	if taken["depth_size"] != view or taken["normal_roughness_size"] != view:
		_failures.append("the depth or normal-roughness texture is not the view size")
	if _probe.error.is_empty() and taken["dump"] != "":
		_result["dump"] = taken["dump"]

	var hit: PackedFloat32Array = taken["hit"]
	var depth: PackedFloat32Array = taken["depth"]
	var normal_roughness: PackedFloat32Array = taken["normal_roughness"]
	# The kernel's float math puts a sample that lands on a pixel boundary ((t + 0.5) / trace * view
	# is an integer when divisor > 1) on either neighbor, so such a texel counts as certain only
	# when both candidate pixels agree.
	var columns := _sample_candidates(hit_size.x, view.x)
	var rows := _sample_candidates(hit_size.y, view.y)
	var stop_distances := PackedFloat32Array() if _result["tier"] == "SWRT" else PackedFloat32Array(HANDOFF_DISTANCES)
	stop_distances.append(float(ProjectSettings.get_setting("rendering/global_illumination/nextgi/rt_scene_radius", 200.0)))
	var traced := 0
	var ambiguous := 0
	var ambiguous_with_depth := 0
	var smooth := 0
	var smooth_misses := 0
	var finite := 0
	var finite_max := 0.0
	var untraced_not_miss := 0
	var handoff_valued := 0
	var quarters := [0, 0, 0, 0]
	var smooth_quarters := [0, 0, 0, 0]
	for y in hit_size.y:
		for x in hit_size.x:
			var distance := hit[(y * hit_size.x + x) * 4]
			var quarter := (2 if y >= hit_size.y / 2 else 0) + (1 if x >= hit_size.x / 2 else 0)
			if distance < MISS_DISTANCE:
				finite += 1
				quarters[quarter] += 1
				finite_max = maxf(finite_max, distance)
				if stop_distances.has(distance):
					handoff_valued += 1
			# Class 0: sky, 1: a surface too rough to trace, 2: a traced surface.
			var classes := 0
			for full_y: int in rows[y]:
				for full_x: int in columns[x]:
					var full_index := (full_y * view.x + full_x) * 4
					var sample_class := 0
					if depth[full_index] > 0.0:
						var alpha := normal_roughness[full_index + 3]
						if alpha > 0.5:
							alpha = 1.0 - alpha
						sample_class = 2 if alpha / (127.0 / 255.0) <= max_roughness else 1
					classes |= 1 << sample_class
			if classes != 1 and classes != 2 and classes != 4:
				ambiguous += 1
				if classes & 6:
					ambiguous_with_depth += 1
				continue
			if classes != 1:
				traced += 1
			if classes == 4:
				smooth += 1
				smooth_quarters[quarter] += 1
				if distance >= MISS_DISTANCE:
					smooth_misses += 1
			elif distance < MISS_DISTANCE:
				untraced_not_miss += 1

	# The counters arrive asynchronously, one or more frames late; the scene is static.
	var rays := -1
	for i in 6:
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		rays = int(info.get("reflection_rays", -1))
	var reported := Vector2i(int(info.get("reflection_trace_width", -1)), int(info.get("reflection_trace_height", -1)))
	_result["reflection_trace_size"] = [reported.x, reported.y]
	_result["reflection_rays"] = rays
	_result["expected_reflection_rays"] = [traced, traced + ambiguous_with_depth]
	_result["boundary_texels"] = ambiguous
	_result["view_pixels"] = view.x * view.y
	_result["reflection_hits"] = int(info.get("reflection_hits", -1))
	_result["reflection_misses"] = int(info.get("reflection_misses", -1))
	_result["smooth_texels"] = smooth
	_result["smooth_texels_per_quarter"] = smooth_quarters
	_result["smooth_texels_holding_65504"] = smooth_misses
	_result["finite_texels"] = finite
	_result["finite_texels_per_quarter"] = quarters
	_result["finite_max"] = finite_max
	_result["texels_at_a_handoff_distance"] = handoff_valued
	_result["sky_or_rough_texels_not_65504"] = untraced_not_miss
	if reported != expected_trace:
		_failures.append("reflection_trace_width/height report %dx%d, expected %dx%d" % [reported.x, reported.y, expected_trace.x, expected_trace.y])
	if rays < traced or rays > traced + ambiguous_with_depth:
		_failures.append("reflection_rays is %d, expected %d to %d (trace pixels whose view sample has depth)" % [rays, traced, traced + ambiguous_with_depth])
	if untraced_not_miss != 0:
		_failures.append("%d texels on sky or on surfaces rougher than %.2f hold a distance instead of 65504" % [untraced_not_miss, max_roughness])
	if handoff_valued > maxi(2, finite / 500):
		_failures.append("%d of %d hit distances are a miss's stop distance (%s); a ray that ends there without a hit must write 65504" % [handoff_valued, finite, str(stop_distances)])
	if smooth > 0 and finite == 0:
		_failures.append("%d texels were ray traced and none holds a hit distance" % smooth)
	if SceneArgs.value("--require-quarters", "false") == "true":
		for quarter in 4:
			if int(quarters[quarter]) == 0:
				_failures.append("quarter %d of the hit distance texture holds no traced hit (top left, top right, bottom left, bottom right)" % quarter)
	if SceneArgs.value("--require-sky-misses", "false") == "true" and smooth_misses == 0:
		_failures.append("no traced texel holds 65504, though the scene's reflections reach the sky")
	if _result.get("front_layer", false):
		_check_front_layer(info, expected_trace)
	var compare_path := SceneArgs.value("--compare", "")
	if not compare_path.is_empty():
		_compare(compare_path, hit, hit_size, view, columns, rows)
	_finish()


## The front layer traced, and its own hit distance target has the trace size (#276).
func _check_front_layer(p_info: Dictionary, p_expected_trace: Vector2i) -> void:
	var dispatches := int(p_info.get("front_layer_dispatch_count", -1))
	var traced := int(p_info.get("front_layer_traced", -1))
	var trace := Vector2i(int(p_info.get("front_layer_trace_width", -1)), int(p_info.get("front_layer_trace_height", -1)))
	var target := Vector2i(int(p_info.get("front_layer_hit_distance_width", -1)), int(p_info.get("front_layer_hit_distance_height", -1)))
	_result["front_layer_dispatch_count"] = dispatches
	_result["front_layer_traced"] = traced
	_result["front_layer_hits"] = int(p_info.get("front_layer_hits", -1))
	_result["front_layer_trace_size"] = [trace.x, trace.y]
	_result["front_layer_hit_distance_size"] = [target.x, target.y]
	if dispatches <= 0 or traced <= 0:
		_failures.append("the front layer did not trace (dispatches %d, rays %d)" % [dispatches, traced])
	if trace != p_expected_trace:
		_failures.append("the front layer traces %dx%d, expected the view's trace size %dx%d" % [trace.x, trace.y, p_expected_trace.x, p_expected_trace.y])
	if target != trace:
		_failures.append("the front layer's hit distance target is %dx%d, expected its trace size %dx%d" % [target.x, target.y, trace.x, trace.y])


## Compares the view's hit distances with a --dump of another run, outside the front layer (#276).
func _compare(p_path: String, p_hit: PackedFloat32Array, p_hit_size: Vector2i, p_view: Vector2i, p_columns: Array, p_rows: Array) -> void:
	_result["compare"] = p_path
	var file := FileAccess.open(p_path, FileAccess.READ)
	if file == null:
		_failures.append("cannot read the comparison dump %s" % p_path)
		return
	var width := int(file.get_32())
	var height := int(file.get_32())
	var size := Vector2i(width, height)
	var reference := file.get_buffer(width * height * 4)
	file.close()
	if size != p_hit_size or reference.size() != size.x * size.y * 4:
		_failures.append("the comparison dump is %dx%d, the hit distance texture %dx%d" % [size.x, size.y, p_hit_size.x, p_hit_size.y])
		return
	var red := PackedFloat32Array()
	red.resize(size.x * size.y)
	for i in red.size():
		red[i] = p_hit[i * 4]
	var current := red.to_byte_array()
	var rects := []
	_front_layer_rects(_scene, p_view, rects)
	var outside_mask := PackedByteArray()
	outside_mask.resize(size.x * size.y)
	for y in size.y:
		for x in size.x:
			var outside := 1
			for full_y: int in p_rows[y]:
				for full_x: int in p_columns[x]:
					var center := Vector2(full_x + 0.5, full_y + 0.5)
					for rect: Rect2 in rects:
						if rect.has_point(center):
							outside = 0
			outside_mask[y * size.x + x] = outside
	var outside_texels := 0
	var outside_hits := 0
	var outside_differing := 0
	var inside_texels := 0
	var inside_differing := 0
	var examples := []
	for i in size.x * size.y:
		var same := current.decode_u32(i * 4) == reference.decode_u32(i * 4)
		if outside_mask[i] == 1:
			outside_texels += 1
			if reference.decode_float(i * 4) < MISS_DISTANCE:
				outside_hits += 1
			if not same:
				outside_differing += 1
				if examples.size() < 4:
					examples.append([i % size.x, i / size.x, reference.decode_float(i * 4), current.decode_float(i * 4)])
		else:
			inside_texels += 1
			if not same:
				inside_differing += 1
	var rect_list := []
	for rect: Rect2 in rects:
		rect_list.append([rect.position.x, rect.position.y, rect.size.x, rect.size.y])
	_result["compare_front_layer_rects"] = rect_list
	_result["compare_outside_texels"] = outside_texels
	_result["compare_outside_hits"] = outside_hits
	_result["compare_outside_differing"] = outside_differing
	_result["compare_outside_examples"] = examples
	_result["compare_inside_texels"] = inside_texels
	_result["compare_inside_differing"] = inside_differing
	if outside_differing != 0:
		_failures.append("%d of %d hit distances outside the front layer differ from %s (x, y, dump, now: %s)" % [outside_differing, outside_texels, p_path, str(examples)])
	var min_hits := SceneArgs.int_value("--min-compared-hits", 1)
	if outside_hits < min_hits:
		_failures.append("the dump holds %d finite hit distances outside the front layer, fewer than %d" % [outside_hits, min_hits])


## Screen rectangles, in view pixels, that hold every surface the front layer can draw: the projected
## bounds of every visible mesh with an alpha-blended material (or a material whose blending is not
## known here), grown by 2 pixels. A mesh reaching behind the camera covers the whole view.
func _front_layer_rects(p_node: Node, p_view: Vector2i, r_rects: Array) -> void:
	var mesh_instance := p_node as MeshInstance3D
	if mesh_instance != null and mesh_instance.mesh != null and mesh_instance.is_visible_in_tree() and _maybe_alpha_blended(mesh_instance):
		var viewport_size := _camera.get_viewport().get_visible_rect().size
		var scale := Vector2(p_view) / viewport_size
		var aabb := mesh_instance.get_aabb()
		var bounds := Rect2()
		var behind := false
		for corner in 8:
			var world := mesh_instance.global_transform * aabb.get_endpoint(corner)
			if _camera.is_position_behind(world):
				behind = true
				break
			var screen := _camera.unproject_position(world) * scale
			bounds = Rect2(screen, Vector2()) if corner == 0 else bounds.expand(screen)
		r_rects.append(Rect2(Vector2(), Vector2(p_view)) if behind else bounds.grow(2.0))
	for child in p_node.get_children():
		_front_layer_rects(child, p_view, r_rects)


func _maybe_alpha_blended(p_mesh_instance: MeshInstance3D) -> bool:
	if p_mesh_instance.transparency > 0.0:
		return true
	for surface in p_mesh_instance.mesh.get_surface_count():
		var material := p_mesh_instance.get_active_material(surface)
		var base := material as BaseMaterial3D
		if material != null and base == null:
			return true
		if base != null and (base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA or base.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS):
			return true
	return false


## Per trace pixel along one axis, the view pixels the kernel may sample: one, or the two
## neighbors of a boundary.
func _sample_candidates(p_trace: int, p_view: int) -> Array:
	var candidates := []
	for t in p_trace:
		var position := (float(t) + 0.5) / float(p_trace) * float(p_view)
		var low := clampi(int(floor(position - 0.001)), 0, p_view - 1)
		var high := clampi(int(floor(position + 0.001)), 0, p_view - 1)
		candidates.append([low] if low == high else [low, high])
	return candidates


func _override_roughness(p_node: Node, p_roughness: float) -> void:
	var mesh_instance := p_node as MeshInstance3D
	if mesh_instance != null and mesh_instance.mesh != null:
		for surface in mesh_instance.mesh.get_surface_count():
			var material := mesh_instance.get_active_material(surface) as BaseMaterial3D
			if material != null:
				var smooth := material.duplicate() as BaseMaterial3D
				smooth.roughness = p_roughness
				mesh_instance.set_surface_override_material(surface, smooth)
	for child in p_node.get_children():
		_override_roughness(child, p_roughness)


func _finish_with(p_reason: String) -> void:
	_failures.append(p_reason)
	_finish()


func _finish() -> void:
	var ok := _failures.is_empty()
	_result["pass"] = ok
	_result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if ok else 1)


## Copies the hit distance, depth and prepass normal-roughness of the view it is attached to.
class TraceProbe extends CompositorEffect:
	## Copies a texture to a buffer of one vec4 per texel.
	const COPY_SHADER := "#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Texels {
	vec4 data[];
}
texels;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	texels.data[pos.y * params.size.x + pos.x] = texelFetch(source, pos, 0);
}
"

	var error := ""

	var _mutex := Mutex.new()
	var _wanted := false
	var _dump_path := ""
	var _taken := {}
	var _shader := RID()
	var _pipeline := RID()
	var _sampler := RID()

	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT

	## Copies the textures on the next frame; a non-empty path also writes the hit distances there.
	func request(p_dump_path: String) -> void:
		_mutex.lock()
		_wanted = true
		_dump_path = p_dump_path
		_taken = {}
		_mutex.unlock()

	func take() -> Dictionary:
		_mutex.lock()
		var taken := _taken
		_taken = {}
		_mutex.unlock()
		return taken

	func _render_callback(_callback_type: int, p_render_data: RenderData) -> void:
		var buffers := p_render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if buffers == null:
			return
		_mutex.lock()
		var wanted := _wanted
		var dump_path := _dump_path
		_wanted = false
		_mutex.unlock()
		if not wanted:
			return
		var rd := RenderingServer.get_rendering_device()
		if not _ensure_pipeline(rd):
			return
		var taken := {"view_size": buffers.get_internal_size(), "dump": ""}
		var sources := {
			"hit": [&"rbgi", &"specular_hit"],
			"normal_roughness": [&"forward_clustered", &"normal_roughness"],
		}
		for texture_name in sources:
			var source: Array = sources[texture_name]
			if not buffers.has_texture(source[0], source[1]):
				continue
			var format := buffers.get_texture_format(source[0], source[1])
			var size := Vector2i(format.width, format.height)
			var data := _copy(rd, buffers.get_texture_slice(source[0], source[1], 0, 0, 1, 1), size)
			if data.size() == size.x * size.y * 4:
				taken[texture_name] = data
				taken[texture_name + "_size"] = size
		var depth_size: Vector2i = taken["view_size"]
		var depth_data := _copy(rd, buffers.get_depth_layer(0), depth_size)
		if depth_data.size() == depth_size.x * depth_size.y * 4:
			taken["depth"] = depth_data
			taken["depth_size"] = depth_size
		if not dump_path.is_empty() and taken.has("hit"):
			var file := FileAccess.open(dump_path, FileAccess.WRITE)
			if file == null:
				error = "cannot write %s" % dump_path
			else:
				var hit_size: Vector2i = taken["hit_size"]
				var hit: PackedFloat32Array = taken["hit"]
				file.store_32(hit_size.x)
				file.store_32(hit_size.y)
				var red := PackedFloat32Array()
				red.resize(hit_size.x * hit_size.y)
				for i in red.size():
					red[i] = hit[i * 4]
				file.store_buffer(red.to_byte_array())
				file.close()
				taken["dump"] = dump_path
		_mutex.lock()
		_taken = taken
		_mutex.unlock()

	func _ensure_pipeline(p_rd: RenderingDevice) -> bool:
		if _pipeline.is_valid():
			return true
		if not error.is_empty() or p_rd == null:
			return false
		var source := RDShaderSource.new()
		source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
		source.source_compute = COPY_SHADER
		var spirv := p_rd.shader_compile_spirv_from_source(source)
		if spirv == null or not spirv.compile_error_compute.is_empty():
			error = "the copy shader did not compile: %s" % (spirv.compile_error_compute if spirv != null else "no SPIR-V")
			return false
		_shader = p_rd.shader_create_from_spirv(spirv, "ReflectTraceCheckCopy")
		if _shader.is_valid():
			_pipeline = p_rd.compute_pipeline_create(_shader)
		_sampler = p_rd.sampler_create(RDSamplerState.new())
		if not _pipeline.is_valid() or not _sampler.is_valid():
			error = "the copy pipeline could not be created"
			return false
		return true

	func _copy(p_rd: RenderingDevice, p_texture: RID, p_size: Vector2i) -> PackedFloat32Array:
		if not p_texture.is_valid() or p_size.x <= 0 or p_size.y <= 0:
			return PackedFloat32Array()
		var buffer := p_rd.storage_buffer_create(p_size.x * p_size.y * 16)
		var texture_uniform := RDUniform.new()
		texture_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		texture_uniform.binding = 0
		texture_uniform.add_id(_sampler)
		texture_uniform.add_id(p_texture)
		var buffer_uniform := RDUniform.new()
		buffer_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		buffer_uniform.binding = 1
		buffer_uniform.add_id(buffer)
		var uniform_set := p_rd.uniform_set_create([texture_uniform, buffer_uniform], _shader, 0)
		var data := PackedFloat32Array()
		if uniform_set.is_valid():
			var push := PackedInt32Array([p_size.x, p_size.y, 0, 0]).to_byte_array()
			var compute := p_rd.compute_list_begin()
			p_rd.compute_list_bind_compute_pipeline(compute, _pipeline)
			p_rd.compute_list_bind_uniform_set(compute, uniform_set, 0)
			p_rd.compute_list_set_push_constant(compute, push, push.size())
			p_rd.compute_list_dispatch(compute, (p_size.x + 7) / 8, (p_size.y + 7) / 8, 1)
			p_rd.compute_list_end()
			data = p_rd.buffer_get_data(buffer).to_float32_array()
			p_rd.free_rid(uniform_set)
		p_rd.free_rid(buffer)
		return data

	func _notification(p_what: int) -> void:
		if p_what != NOTIFICATION_PREDELETE:
			return
		var rd := RenderingServer.get_rendering_device()
		if rd == null:
			return
		for rid in [_pipeline, _shader, _sampler]:
			if rid.is_valid():
				rd.free_rid(rid)
