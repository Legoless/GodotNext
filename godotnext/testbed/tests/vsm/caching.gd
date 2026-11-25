extends Node3D

## Virtual Shadow Map caching (#212; plan §21.3 contract S5): static and dynamic layers, per-page
## invalidation from swept bounds, the per-instance cache modes and the deferred invalidation budget.
##
## A static set (a floor, boxes, a sun with 7 clipmap levels and a spot light) seen by a fixed camera in
## the main window. One case per process (--case=<name>):
## - static: after a warm-up, at most 1% of the view's allocated pages (kept + new) are re-rendered in any
##   of --frames=N frames (default 600); a page check (GPU against the CPU models) passes.
## - actor: a 2 m RIGID actor moves at 5 m/s. Every CHECK_EVERY frames a page check compares the GPU's
##   invalidation pass with the CPU models (invalidate_map_model(), invalidate_box_model(): entries, page
##   flags and counters must be equal) and the allocation with its model. Each checked frame has exactly one
##   invalidation box, and it equals the actor's swept bounds (its previous and current box, from the positions
##   this script moved it to), so the CPU model's count on it (box_static_pages_model) is the analytic swept-bounds
##   overlap. The static pages re-rendered in the checked frames (dirty_static - new) must be at most 1.5 times
##   that overlap, which must be above 0; the invalidated pages no view re-rendered (evicted) are reported.
## - static_mode: a STATIC box moves: 0 static pages are invalidated and every move is counted as an
##   invalidation STATIC suppressed; hiding it, showing it, turning its shadow off and on, changing its layers,
##   removing it from the tree, adding it back and freeing it still invalidate (UE: "Add/Remove still triggers
##   invalidations"). A STATIC slab that moved and then had its material changed is hidden: the hide's box
##   holds its old and new bounds, and its stale shadow at the old place is gone (image). Another moved STATIC
##   slab switched to RIGID gets its shadow back where it is (UE: changing the behavior re-adds the primitive).
## - always: an ALWAYS box and an ALWAYS NextGeo mesh: in a raster check with every page new, the box has
##   pairs only in layer 1 (always_layer0_pairs = 0), a static box only in layer 0, and the NextGeo stats
##   count dynamic clusters and 0 always_layer0_clusters.
## - materials: under AUTO a VERTEX-animated foliage material (VERTEX and TIME in vertex()), a material that
##   writes DEPTH and one that writes VERTEX without TIME classify as dynamic (reasons VERTEX, DEPTH) and
##   draw only into layer 1; a plain box stays static.
## - move_toggle (#222's request): moving a static (AUTO) caster moves its shadow, both while it is dynamic
##   and after it settles back into the static layer; turning its shadow off and on removes and restores
##   it; the same for a RIGID caster's move and a STATIC caster's hide and show (read from the image).
## - budget: deferred_invalidation_budget = --budget=N (default 512) while the sun turns 0.5 degrees per
##   frame: static pages are deferred (deferred_static > 0), every checked frame's GPU counters equal the
##   CPU model (deferred_static included), and once the sun stops the cache converges (no dirty or deferred
##   page in the last CONVERGED_FRAMES frames). The sun's resolution LOD bias, static and moving, is -1
##   (--lod-bias=N; --lod-bias=project keeps the project's), so that even a small view invalidates more
##   requested pages per frame than the budget.
## - fog_atlas: a spot light that keeps its shadow-atlas pass while VSM is active (explicit
##   SHADOW_METHOD_ATLAS after #215 moved VSM volumetric fog onto coarse pages); when a caster
##   moves, its atlas shadow is redrawn (the light's shadow version advances), as without VSM.
## - multimesh: MultiMesh slabs with a custom AABB (which suppresses the AABB notification) are moved, one with
##   set_instance_transform(), one by replacing its buffer (both AUTO), and a RIGID one: their shadows move
##   while they are dynamic, after they settle, and when they move back (image).
## - range_swap: a slab hidden by its visibility range and a slab shown by it swap (the camera does not move):
##   the shadows swap too, and swap back (image).
##
## Run with GODOT_DEV_VSM=1 (with --expect-active=false and GODOT_DEV_VSM unset it only checks that nothing
## is active). Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs = preload("res://scenes/scene_args.gd")

const WARMUP_FRAMES := 60
const RESULT_WAIT_FRAMES := 8
const MAX_STATIC_RATIO := 0.01
const ACTOR_SPEED := 5.0 # m/s, one step per frame at 60 frames per second.
const ACTOR_FRAMES := 90
const CHECK_EVERY := 6
const MAX_ACTOR_RATIO := 1.5
const ACTOR_SIZE := Vector3(0.6, 2.0, 0.6)
const BOX_EPSILON := 0.001 # Meters: an invalidation box against the bounds this script expects.
const SLAB_SIZE := Vector3(1.6, 0.1, 1.6)
const STATIC_MOVES := 10
const SETTLE_FRAMES := 4
const COOLDOWN_FRAMES := 20 # Past the 8-frame AUTO cooldown.
const TURN_FRAMES := 60
const TURN_DEGREES := 0.5
const CONVERGE_FRAMES := 90
const CONVERGED_FRAMES := 10
const SHADOWED := 0.5 # Of the open floor's brightness.
const LIT := 0.8
const DYNAMIC_VERTEX := 1 << 5
const DYNAMIC_VERTEX_TIME := 1 << 8
const DYNAMIC_DEPTH := 1 << 9
const DYNAMIC_MODE_ALWAYS := 1 << 10
const BUDGET_SETTING := "rendering/lights_and_shadows/virtual_shadows/deferred_invalidation_budget"

const FOLIAGE_SHADER := """
shader_type spatial;
void vertex() {
	VERTEX.x += sin(TIME * 3.0 + VERTEX.y) * 0.05 * VERTEX.y;
}
"""
const DEPTH_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.8);
	DEPTH = FRAGCOORD.z;
}
"""
const VERTEX_SHADER := """
shader_type spatial;
void vertex() {
	VERTEX *= 1.05;
}
"""

var _case := "static"
var _camera: Camera3D
var _environment: Environment
var _sun: DirectionalLight3D
var _spot: SpotLight3D
var _shadowed_lights: Array[Light3D] = []
var _failures := PackedStringArray()
var _result := {}
var _temp_files: Array[String] = []


func _ready() -> void:
	_case = SceneArgs.value("--case", "static")
	_result["case"] = _case
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	if _case == "budget":
		ProjectSettings.set_setting(BUDGET_SETTING, SceneArgs.int_value("--budget", 512))
		# --lod-bias=N (default -1): the sun's resolution LOD bias, static and moving, in every preset. The finer
		# bias requests more pages per view: with the project's (UE's coarser bias while the sun turns) a
		# 1920x1080 view invalidates fewer requested pages per frame than a 512-page budget defers.
		# --lod-bias=project keeps the project's.
		var lod_bias := SceneArgs.value("--lod-bias", "-1")
		if lod_bias != "project":
			_result["lod_bias"] = int(lod_bias)
			for quality in ["low", "medium", "high", "epic"]:
				for setting in ["resolution_lod_bias_directional", "resolution_lod_bias_directional_moving"]:
					ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/%s.%s" % [setting, quality], int(lod_bias))
	if _case == "always":
		ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
		ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	_build_scene()
	_run.call_deferred()


func _build_scene() -> void:
	_environment = Environment.new()
	_environment.background_mode = Environment.BG_COLOR
	_environment.background_color = Color(0.1, 0.12, 0.15)
	_environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_environment.ambient_light_color = Color(1.0, 1.0, 1.0)
	_environment.ambient_light_energy = 0.03
	_environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	_environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = _environment
	add_child(world)

	_camera = Camera3D.new()
	add_child(_camera)
	_camera.position = Vector3(0.0, 16.0, 12.0)
	_camera.look_at(Vector3(0.0, 0.0, -1.0))
	_camera.current = true

	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(60.0, 60.0)
	floor_mesh.mesh = plane
	floor_mesh.material_override = _material(Color(1.0, 1.0, 1.0))
	add_child(floor_mesh)
	var rng := RandomNumberGenerator.new()
	rng.seed = 212
	for i in 10:
		var box := MeshInstance3D.new()
		var mesh := BoxMesh.new()
		mesh.size = Vector3(rng.randf_range(0.5, 1.5), rng.randf_range(0.5, 2.5), rng.randf_range(0.5, 1.5))
		box.mesh = mesh
		box.material_override = _material(Color(0.7, 0.7, 0.7))
		var angle := TAU * float(i) / 10.0
		box.position = Vector3(cos(angle) * 9.0, mesh.size.y * 0.5, sin(angle) * 7.0 - 1.0)
		add_child(box)

	_sun = DirectionalLight3D.new()
	_sun.shadow_enabled = true
	_sun.directional_shadow_max_distance = 100.0
	# The sun shines toward the camera's side (+z) so that a caster's shadow falls where the camera sees the
	# floor, not behind the caster.
	_sun.rotation_degrees = Vector3(-60.0, 210.0, 0.0)
	add_child(_sun)
	_spot = SpotLight3D.new()
	_spot.shadow_enabled = true
	_spot.spot_range = 14.0
	_spot.spot_angle = 40.0
	_spot.light_energy = 2.0
	add_child(_spot)
	_spot.look_at_from_position(Vector3(7.0, 7.0, 4.0), Vector3(7.0, 0.0, 3.0), Vector3.FORWARD)
	_shadowed_lights = [_sun, _spot]


func _material(p_color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = p_color
	material.roughness = 1.0
	material.metallic_specular = 0.0
	return material


func _shader_material(p_code: String) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = p_code
	var material := ShaderMaterial.new()
	material.shader = shader
	return material


func _box(p_name: String, p_size: Vector3, p_position: Vector3, p_mode: int) -> MeshInstance3D:
	var node := MeshInstance3D.new()
	node.name = p_name
	var mesh := BoxMesh.new()
	mesh.size = p_size
	node.mesh = mesh
	node.material_override = _material(Color(0.8, 0.8, 0.8))
	node.position = p_position
	node.shadow_cache_invalidation = p_mode
	add_child(node)
	return node


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _info() -> Dictionary:
	return RenderingServer.virtual_shadow_maps_get_debug_info()


func _pages() -> Dictionary:
	return _info().get("pages", {})


func _caching() -> Dictionary:
	return _info().get("caching", {})


# A page check of the next marked view (GPU against the CPU models). Its failures are this run's.
func _page_check(p_label: String) -> Dictionary:
	RenderingServer.virtual_shadow_maps_request_check()
	for i in RESULT_WAIT_FRAMES:
		await RenderingServer.frame_post_draw
		var check := RenderingServer.virtual_shadow_maps_get_check_result()
		if bool(check.get("captured", false)):
			_record_check(p_label, check)
			return check
	_failures.append("%s: no view captured the page check in %d frames" % [p_label, RESULT_WAIT_FRAMES])
	return {}


func _record_check(p_label: String, p_check: Dictionary) -> void:
	for failure in p_check.get("failures", PackedStringArray()):
		_failures.append("%s: %s" % [p_label, failure])


# A raster check with every page new: the lights' shadows are off for a frame, so their maps are new.
func _raster_check(p_label: String) -> Dictionary:
	for light in _shadowed_lights:
		light.shadow_enabled = false
	await _frames(1)
	for light in _shadowed_lights:
		light.shadow_enabled = true
	RenderingServer.virtual_shadow_maps_request_raster_check()
	for i in RESULT_WAIT_FRAMES + 2:
		await RenderingServer.frame_post_draw
		var check := RenderingServer.virtual_shadow_maps_get_raster_check_result()
		if bool(check.get("captured", false)):
			for failure in check.get("failures", PackedStringArray()):
				_failures.append("%s: %s" % [p_label, failure])
			return check
	_failures.append("%s: no view captured the raster check" % p_label)
	return {}


func _caster(p_check: Dictionary, p_node: Node) -> Dictionary:
	for caster: Dictionary in p_check.get("casters", []):
		if int(caster.get("object_id", 0)) == p_node.get_instance_id():
			return caster
	return {}


func _run() -> void:
	await _frames(2)
	var info := _info()
	if SceneArgs.value("--expect-active", "true") != "true":
		# VSM off (GODOT_DEV_VSM unset): nothing is active and no view marks pages.
		await _frames(8)
		info = _info()
		_result["expect_active"] = false
		if bool(info.get("active", true)) or int(info.get("marked_views", -1)) != 0:
			_failures.append("virtual shadow maps are active (%s) or marked %d views with GODOT_DEV_VSM unset" % [str(info.get("active", true)), int(info.get("marked_views", -1))])
		_finish()
		return
	if not bool(info.get("active", false)):
		_failures.append("virtual shadow maps are not active (run with GODOT_DEV_VSM=1)")
		_finish()
		return
	match _case:
		"static":
			await _case_static()
		"actor":
			await _case_actor()
		"static_mode":
			await _case_static_mode()
		"always":
			await _case_always()
		"materials":
			await _case_materials()
		"move_toggle":
			await _case_move_toggle()
		"budget":
			await _case_budget()
		"fog_atlas":
			await _case_fog_atlas()
		"multimesh":
			await _case_multimesh()
		"range_swap":
			await _case_range_swap()
		_:
			_failures.append("unknown --case=%s" % _case)
	_finish()


func _case_static() -> void:
	var frames := SceneArgs.int_value("--frames", 600)
	await _frames(WARMUP_FRAMES)
	var start := _pages()
	var max_ratio := 0.0
	var worst_frame := -1
	var dirty_sum := 0
	var allocated_sum := 0
	var counted := 0
	for i in frames:
		await _frames(1)
		var pages := _pages()
		var allocated := int(pages.get("kept", 0)) + int(pages.get("new", 0))
		var dirty := int(pages.get("dirty", 0))
		if allocated <= 0:
			continue
		counted += 1
		dirty_sum += dirty
		allocated_sum += allocated
		var ratio := float(dirty) / float(allocated)
		if ratio > max_ratio:
			max_ratio = ratio
			worst_frame = i
	var end := _pages()
	await _page_check("static")
	_result["frames"] = frames
	_result["frames_counted"] = counted
	_result["max_rerendered_ratio"] = max_ratio
	_result["worst_frame"] = worst_frame
	_result["mean_allocated"] = float(allocated_sum) / float(maxi(counted, 1))
	_result["mean_rerendered"] = float(dirty_sum) / float(maxi(counted, 1))
	_result["invalidated_static"] = int(end.get("total_invalidated_static", 0)) - int(start.get("total_invalidated_static", 0))
	if counted < frames:
		_failures.append("%d of %d frames had no allocated pages" % [frames - counted, frames])
	if max_ratio > MAX_STATIC_RATIO:
		_failures.append("frame %d re-rendered %.2f%% of its allocated pages (at most %.0f%%)" % [worst_frame, max_ratio * 100.0, MAX_STATIC_RATIO * 100.0])


func _case_actor() -> void:
	var actor := _box("Actor", ACTOR_SIZE, Vector3(-4.0, 1.0, 0.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_RIGID)
	await _frames(WARMUP_FRAMES)
	var start := _pages()
	var rerendered := 0
	var analytic := 0
	var invalidated := 0
	var checks := 0
	var armed := false
	var samples := []
	var previous_x := actor.position.x
	var current_x := actor.position.x
	for f in ACTOR_FRAMES + RESULT_WAIT_FRAMES:
		previous_x = current_x
		if f < ACTOR_FRAMES:
			actor.position.x += ACTOR_SPEED / 60.0
		current_x = actor.position.x
		if not armed and f < ACTOR_FRAMES and f % CHECK_EVERY == 0:
			RenderingServer.virtual_shadow_maps_request_check()
			armed = true
		await RenderingServer.frame_post_draw
		if armed:
			var check := RenderingServer.virtual_shadow_maps_get_check_result()
			if bool(check.get("captured", false)):
				armed = false
				checks += 1
				_record_check("actor frame %d" % f, check)
				var invalidation: Dictionary = check.get("invalidation", {})
				var allocation: Dictionary = check.get("allocation", {})
				# The frame's one box must be the actor's swept bounds: its box at the position before this frame's
				# step and at the one after it. The CPU model's count on that box is then the analytic overlap.
				var expected := _actor_aabb(previous_x).merge(_actor_aabb(current_x))
				var box_list: Array = invalidation.get("box_list", [])
				var box_text := "none"
				if box_list.size() == 1:
					var box: Dictionary = box_list[0]
					box_text = "%s..%s" % [str(box.get("min")), str(box.get("max"))]
				if box_list.size() != 1 or not _box_equals(box_list[0], expected):
					_failures.append("actor frame %d: %d invalidation boxes (%s), expected one equal to the swept bounds %s..%s" % [f, box_list.size(), box_text, str(expected.position), str(expected.end)])
				var page_rerendered := int(allocation.get("dirty_static", 0)) - int(allocation.get("new", 0))
				var overlap := int(invalidation.get("box_static_pages_model", 0))
				var page_invalidated := int(invalidation.get("static_pages", 0))
				rerendered += page_rerendered
				analytic += overlap
				invalidated += page_invalidated
				samples.append({"frame": f, "rerendered": page_rerendered, "overlap": overlap, "boxes": box_list.size(), "box": box_text, "static_pages": page_invalidated, "dynamic_pages": invalidation.get("dynamic_pages", 0)})
				if int(invalidation.get("dynamic_pages", 0)) != 0:
					_failures.append("actor frame %d: a RIGID actor invalidated %d dynamic pages" % [f, int(invalidation.get("dynamic_pages", 0))])
	var end := _pages()
	_result["checks"] = checks
	_result["samples"] = samples
	_result["rerendered_static"] = rerendered
	_result["analytic_overlap"] = analytic
	_result["ratio"] = float(rerendered) / float(maxi(analytic, 1))
	# Invalidated pages that were not re-rendered: no view requested them, so they were evicted.
	_result["invalidated_not_rerendered"] = invalidated - rerendered
	_result["invalidated_static"] = int(end.get("total_invalidated_static", 0)) - int(start.get("total_invalidated_static", 0))
	if checks < ACTOR_FRAMES / CHECK_EVERY / 2:
		_failures.append("only %d page checks were captured" % checks)
	if analytic <= 0:
		_failures.append("the actor's swept bounds overlapped no allocated page in the checked frames")
	elif float(rerendered) > MAX_ACTOR_RATIO * float(analytic):
		_failures.append("%d static pages re-rendered, more than %.1f x the %d the actor's swept bounds overlap" % [rerendered, MAX_ACTOR_RATIO, analytic])


func _actor_aabb(p_x: float) -> AABB:
	return AABB(Vector3(p_x, 1.0, 0.0) - ACTOR_SIZE * 0.5, ACTOR_SIZE)


func _slab_aabb(p_center: Vector3) -> AABB:
	return AABB(p_center - SLAB_SIZE * 0.5, SLAB_SIZE)


func _box_equals(p_box: Dictionary, p_expected: AABB) -> bool:
	var lo: Vector3 = p_box.get("min", Vector3.INF)
	var hi: Vector3 = p_box.get("max", Vector3.INF)
	return lo.distance_to(p_expected.position) <= BOX_EPSILON and hi.distance_to(p_expected.end) <= BOX_EPSILON


# Whether one of the check's invalidation boxes holds p_aabb.
func _boxes_hold(p_check: Dictionary, p_aabb: AABB) -> bool:
	var invalidation: Dictionary = p_check.get("invalidation", {})
	for box: Dictionary in invalidation.get("box_list", []):
		var lo: Vector3 = box.get("min", Vector3.INF)
		var hi: Vector3 = box.get("max", -Vector3.INF)
		var want_lo := p_aabb.position + Vector3.ONE * BOX_EPSILON
		var want_hi := p_aabb.end - Vector3.ONE * BOX_EPSILON
		if lo.x <= want_lo.x and lo.y <= want_lo.y and lo.z <= want_lo.z and hi.x >= want_hi.x and hi.y >= want_hi.y and hi.z >= want_hi.z:
			return true
	return false


func _case_static_mode() -> void:
	var box := _box("StaticBox", Vector3(1.0, 1.0, 1.0), Vector3(3.0, 0.5, 2.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_STATIC)
	# Two STATIC slabs for the footprint steps at the end; their shadows are cached during the warm-up.
	var footprint_from := Vector3(1.0, 1.5, -3.0)
	var footprint_to := Vector3(4.0, 1.5, -3.0)
	var footprint := _box("FootprintSlab", SLAB_SIZE, footprint_from, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_STATIC)
	var mode_from := Vector3(-5.0, 1.5, -1.0)
	var mode_to := Vector3(-2.0, 1.5, -1.0)
	var mode_slab := _box("ModeSlab", SLAB_SIZE, mode_from, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_STATIC)
	await _frames(WARMUP_FRAMES)
	var start := _pages()
	var suppressed_start := int(_caching().get("static_mode_invalidations", 0))
	for i in STATIC_MOVES:
		box.position.x += 0.5
		if i == STATIC_MOVES / 2:
			await _page_check("static_mode move")
		else:
			await _frames(1)
	await _frames(SETTLE_FRAMES)
	var moved := _pages()
	var suppressed := int(_caching().get("static_mode_invalidations", 0)) - suppressed_start
	var moved_invalidated := int(moved.get("total_invalidated_static", 0)) - int(start.get("total_invalidated_static", 0))
	_result["moves"] = STATIC_MOVES
	_result["suppressed"] = suppressed
	_result["invalidated_while_moving"] = moved_invalidated
	if moved_invalidated != 0:
		_failures.append("a moving STATIC instance invalidated %d static pages" % moved_invalidated)
	if suppressed != STATIC_MOVES:
		_failures.append("%d moves of the STATIC instance were counted as suppressed invalidations, expected %d" % [suppressed, STATIC_MOVES])
	# Adding and removing still invalidate.
	var steps := {}
	for step in ["hide", "show", "shadow_off", "shadow_on", "layers", "layers_back", "exit", "enter", "free"]:
		var before := int(_pages().get("total_invalidated_static", 0))
		match step:
			"hide":
				box.visible = false
			"show":
				box.visible = true
			"shadow_off":
				box.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			"shadow_on":
				box.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
			"layers":
				box.layers = 2
			"layers_back":
				box.layers = 1
			"exit":
				remove_child(box)
			"enter":
				add_child(box)
			"free":
				box.queue_free()
		await _page_check("static_mode " + step)
		await _frames(SETTLE_FRAMES)
		var invalidated := int(_pages().get("total_invalidated_static", 0)) - before
		steps[step] = invalidated
		if invalidated <= 0:
			_failures.append("%s on a STATIC instance invalidated no static page" % step)
	_result["add_remove"] = steps
	await _static_footprint(footprint, footprint_from, footprint_to, mode_slab, mode_from, mode_to)


# A moved STATIC caster keeps its whole footprint: a later sync that does not invalidate (a material change) keeps
# it, so hiding the caster clears its shadow where it was drawn (UE leaves a moved STATIC caster's shadow
# undefined, so the moved state is only recorded). Switching a moved STATIC caster to RIGID re-adds it.
func _static_footprint(p_slab: MeshInstance3D, p_from: Vector3, p_to: Vector3, p_mode_slab: MeshInstance3D, p_mode_from: Vector3, p_mode_to: Vector3) -> void:
	var images := {}
	var old_point := _shadow_point(p_from)
	var new_point := _shadow_point(p_to)
	_expect_shadows("footprint start", {"old": [old_point, true], "new": [new_point, false]}, images)
	for i in 3:
		p_slab.position = p_from.lerp(p_to, float(i + 1) / 3.0)
		await _frames(1)
	await _frames(SETTLE_FRAMES)
	p_slab.material_override = _material(Color(0.6, 0.6, 0.6))
	await _frames(SETTLE_FRAMES)
	_record_shadows("footprint moved", {"old": old_point, "new": new_point}, images)
	p_slab.visible = false
	var hide_check := await _page_check("static_mode footprint hide")
	var footprint := _slab_aabb(p_from).merge(_slab_aabb(p_to))
	if not _boxes_hold(hide_check, footprint):
		_failures.append("hiding the moved STATIC slab invalidated no box holding its old and new bounds %s..%s: %s" % [str(footprint.position), str(footprint.end), str(hide_check.get("invalidation", {}).get("box_list", []))])
	await _frames(SETTLE_FRAMES)
	_expect_shadows("footprint hidden", {"old": [old_point, false], "new": [new_point, false]}, images)

	var mode_old := _shadow_point(p_mode_from)
	var mode_new := _shadow_point(p_mode_to)
	_expect_shadows("mode start", {"old": [mode_old, true], "new": [mode_new, false]}, images)
	for i in 3:
		p_mode_slab.position = p_mode_from.lerp(p_mode_to, float(i + 1) / 3.0)
		await _frames(1)
	await _frames(SETTLE_FRAMES)
	_record_shadows("mode moved", {"old": mode_old, "new": mode_new}, images)
	p_mode_slab.shadow_cache_invalidation = GeometryInstance3D.SHADOW_CACHE_INVALIDATION_RIGID
	var mode_check := await _page_check("static_mode to rigid")
	var mode_footprint := _slab_aabb(p_mode_from).merge(_slab_aabb(p_mode_to))
	if not _boxes_hold(mode_check, mode_footprint):
		_failures.append("switching the moved STATIC slab to RIGID invalidated no box holding its old and new bounds %s..%s: %s" % [str(mode_footprint.position), str(mode_footprint.end), str(mode_check.get("invalidation", {}).get("box_list", []))])
	await _frames(SETTLE_FRAMES)
	_expect_shadows("mode rigid", {"old": [mode_old, false], "new": [mode_new, true]}, images)
	_result["footprint"] = images


func _case_always() -> void:
	var always := _box("AlwaysBox", Vector3(1.0, 1.5, 1.0), Vector3(2.0, 0.75, -3.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_ALWAYS)
	var still := _box("StaticBox", Vector3(1.0, 1.5, 1.0), Vector3(-2.0, 0.75, -3.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	var virtual_mesh := _make_virtual_mesh()
	var nextgeo: MeshInstance3D = null
	if virtual_mesh != null:
		nextgeo = MeshInstance3D.new()
		nextgeo.name = "AlwaysNextGeo"
		nextgeo.mesh = virtual_mesh
		nextgeo.position = Vector3(0.0, 1.0, 2.0)
		nextgeo.shadow_cache_invalidation = GeometryInstance3D.SHADOW_CACHE_INVALIDATION_ALWAYS
		add_child(nextgeo)
	await _frames(WARMUP_FRAMES)
	var check := await _raster_check("always")
	var always_info := _caster(check, always)
	var still_info := _caster(check, still)
	_result["always_layer0_pairs"] = check.get("always_layer0_pairs", -1)
	_result["always_box"] = {"layer0": always_info.get("layer0_pairs", -1), "layer1": always_info.get("layer1_pairs", -1), "reasons": always_info.get("dynamic_reasons", -1)}
	_result["static_box"] = {"layer0": still_info.get("layer0_pairs", -1), "layer1": still_info.get("layer1_pairs", -1), "reasons": still_info.get("dynamic_reasons", -1)}
	if int(check.get("always_layer0_pairs", -1)) != 0:
		_failures.append("always_layer0_pairs = %s" % str(check.get("always_layer0_pairs", -1)))
	if always_info.is_empty() or int(always_info.get("layer0_pairs", -1)) != 0 or int(always_info.get("layer1_pairs", 0)) <= 0:
		_failures.append("the ALWAYS box did not draw only into layer 1: %s" % str(_result["always_box"]))
	if (int(always_info.get("dynamic_reasons", 0)) & DYNAMIC_MODE_ALWAYS) == 0:
		_failures.append("the ALWAYS box's dynamic reasons lack MODE_ALWAYS: %d" % int(always_info.get("dynamic_reasons", 0)))
	if still_info.is_empty() or int(still_info.get("layer1_pairs", -1)) != 0 or int(still_info.get("layer0_pairs", 0)) <= 0:
		_failures.append("the static box did not draw only into layer 0: %s" % str(_result["static_box"]))
	# NextGeo: the cluster cull's layer counters over a few frames.
	var dynamic_clusters := 0
	var always_layer0 := 0
	var stats_seen := false
	for i in 10:
		await _frames(1)
		var nanite: Dictionary = _info().get("nanite", {})
		if nanite.has("dynamic_clusters"):
			stats_seen = true
			dynamic_clusters = maxi(dynamic_clusters, int(nanite.get("dynamic_clusters", 0)))
			always_layer0 += int(nanite.get("always_layer0_clusters", 0))
	_result["nextgeo"] = {"mesh": virtual_mesh != null, "stats_seen": stats_seen, "max_dynamic_clusters": dynamic_clusters, "always_layer0_clusters": always_layer0}
	if virtual_mesh == null:
		_failures.append("the NextGeo mesh could not be built")
	elif not stats_seen:
		_failures.append("no NextGeo VSM stats were published")
	else:
		if dynamic_clusters <= 0:
			_failures.append("the ALWAYS NextGeo mesh rasterized no dynamic clusters")
		if always_layer0 != 0:
			_failures.append("%d ALWAYS NextGeo clusters went to layer 0" % always_layer0)


func _make_virtual_mesh() -> ArrayMesh:
	var box := BoxMesh.new()
	box.size = Vector3(1.5, 1.5, 1.5)
	box.subdivide_width = 24
	box.subdivide_height = 24
	box.subdivide_depth = 24
	var importer := ImporterMesh.from_mesh(box)
	var path := "user://vsm_caching_always.ngm"
	_temp_files.append(path)
	if importer.build_virtual_geometry(path) != OK:
		return null
	var mesh := importer.get_mesh()
	if mesh == null or mesh.get_virtual_geometry() == null:
		return null
	return mesh


func _case_materials() -> void:
	# The displacing materials get a cull margin that holds their moved vertices (the raster check verifies it).
	var foliage := _box("Foliage", Vector3(0.6, 2.0, 0.6), Vector3(-3.0, 1.0, -2.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	foliage.material_override = _shader_material(FOLIAGE_SHADER)
	foliage.extra_cull_margin = 0.5
	var depth := _box("DepthWriter", Vector3(1.0, 1.0, 1.0), Vector3(0.0, 0.5, -2.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	depth.material_override = _shader_material(DEPTH_SHADER)
	var vertex := _box("VertexWriter", Vector3(1.0, 1.0, 1.0), Vector3(3.0, 0.5, -2.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	vertex.material_override = _shader_material(VERTEX_SHADER)
	vertex.extra_cull_margin = 0.5
	var plain := _box("Plain", Vector3(1.0, 1.0, 1.0), Vector3(0.0, 0.5, 3.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	await _frames(WARMUP_FRAMES)
	var check := await _raster_check("materials")
	var expect := [[foliage, DYNAMIC_VERTEX | DYNAMIC_VERTEX_TIME, true], [depth, DYNAMIC_DEPTH, true], [vertex, DYNAMIC_VERTEX, true], [plain, 0, false]]
	var casters := {}
	for e: Array in expect:
		var node: Node = e[0]
		var info := _caster(check, node)
		var reasons := int(info.get("dynamic_reasons", -1))
		casters[node.name] = {"reasons": reasons, "layer0": info.get("layer0_pairs", -1), "layer1": info.get("layer1_pairs", -1)}
		if info.is_empty():
			_failures.append("%s is not among the raster check's casters" % node.name)
			continue
		var bits: int = e[1]
		if bool(e[2]):
			if (reasons & bits) != bits:
				_failures.append("%s: dynamic reasons %d lack %d under AUTO" % [node.name, reasons, bits])
			if int(info.get("layer0_pairs", -1)) != 0 or int(info.get("layer1_pairs", 0)) <= 0:
				_failures.append("%s: drew into layer 0 (%d pairs) or not into layer 1 (%d pairs)" % [node.name, int(info.get("layer0_pairs", -1)), int(info.get("layer1_pairs", 0))])
		elif reasons != 0 or int(info.get("layer1_pairs", -1)) != 0:
			_failures.append("%s: a plain static box has dynamic reasons %d and %d layer-1 pairs" % [node.name, reasons, int(info.get("layer1_pairs", -1))])
	_result["casters"] = casters


# The point of the floor where the shadow of p_point falls (the sun shines along its -Z).
func _shadow_point(p_point: Vector3) -> Vector3:
	var direction := -_sun.global_transform.basis.z.normalized()
	return p_point + direction * (-p_point.y / direction.y)


func _brightness(p_image: Image, p_world: Vector3) -> float:
	var pixel := _camera.unproject_position(p_world)
	var total := 0.0
	var count := 0
	for dy in range(-1, 2):
		for dx in range(-1, 2):
			var x := clampi(int(pixel.x) + dx, 0, p_image.get_width() - 1)
			var y := clampi(int(pixel.y) + dy, 0, p_image.get_height() - 1)
			var color := p_image.get_pixel(x, y)
			total += (color.r + color.g + color.b) / 3.0
			count += 1
	return total / float(count)


# Checks which of the points are in shadow: p_expect maps a label to [world point, shadowed].
func _expect_shadows(p_step: String, p_expect: Dictionary, r_steps: Dictionary) -> void:
	var image := get_viewport().get_texture().get_image()
	var open := _brightness(image, Vector3(0.0, 0.0, 6.0))
	var record := {"open": open}
	for label: String in p_expect:
		var entry: Array = p_expect[label]
		var value := _brightness(image, entry[0])
		record[label] = value
		var shadowed := value < SHADOWED * open
		var lit := value > LIT * open
		if bool(entry[1]) and not shadowed:
			_failures.append("%s: %s should be in shadow (%.3f of open %.3f)" % [p_step, label, value, open])
		elif not bool(entry[1]) and not lit:
			_failures.append("%s: %s should be lit (%.3f of open %.3f)" % [p_step, label, value, open])
	r_steps[p_step] = record


# Records the brightness at the points (a label to a world point) without expecting anything.
func _record_shadows(p_step: String, p_points: Dictionary, r_steps: Dictionary) -> void:
	var image := get_viewport().get_texture().get_image()
	var record := {"open": _brightness(image, Vector3(0.0, 0.0, 6.0))}
	for label: String in p_points:
		record[label] = _brightness(image, p_points[label])
	r_steps[p_step] = record


func _case_move_toggle() -> void:
	var p1 := Vector3(1.0, 1.5, -3.0)
	var p2 := Vector3(4.0, 1.5, -3.0)
	var slab := _box("AutoSlab", Vector3(1.6, 0.1, 1.6), p1, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	var p3 := Vector3(-5.0, 1.5, -1.0)
	var p4 := Vector3(-2.0, 1.5, -1.0)
	var rigid := _box("RigidSlab", Vector3(1.6, 0.1, 1.6), p3, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_RIGID)
	var p5 := Vector3(-1.0, 1.5, 3.0)
	var fixed := _box("StaticSlab", Vector3(1.6, 0.1, 1.6), p5, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_STATIC)
	var a := _shadow_point(p1)
	var b := _shadow_point(p2)
	var c := _shadow_point(p3)
	var d := _shadow_point(p4)
	var e := _shadow_point(p5)
	var steps := {}
	await _frames(WARMUP_FRAMES)
	_expect_shadows("start", {"auto_old": [a, true], "auto_new": [b, false], "rigid_old": [c, true], "rigid_new": [d, false], "static": [e, true]}, steps)
	slab.position = p2
	rigid.position = p4
	await _frames(SETTLE_FRAMES)
	_expect_shadows("moved", {"auto_old": [a, false], "auto_new": [b, true], "rigid_old": [c, false], "rigid_new": [d, true]}, steps)
	await _frames(COOLDOWN_FRAMES)
	_expect_shadows("settled", {"auto_old": [a, false], "auto_new": [b, true], "rigid_old": [c, false], "rigid_new": [d, true]}, steps)
	slab.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	fixed.visible = false
	await _frames(SETTLE_FRAMES)
	_expect_shadows("shadow_off_and_hidden", {"auto_new": [b, false], "static": [e, false]}, steps)
	slab.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	fixed.visible = true
	await _frames(SETTLE_FRAMES)
	_expect_shadows("shadow_on_and_shown", {"auto_new": [b, true], "static": [e, true]}, steps)
	await _frames(COOLDOWN_FRAMES)
	_expect_shadows("final", {"auto_old": [a, false], "auto_new": [b, true], "rigid_new": [d, true], "static": [e, true]}, steps)
	_result["steps"] = steps


func _case_budget() -> void:
	var budget := SceneArgs.int_value("--budget", 512)
	await _frames(WARMUP_FRAMES)
	var caching := _caching()
	_result["budget"] = budget
	if int(caching.get("deferred_invalidation_budget", -1)) != budget:
		_failures.append("the deferred invalidation budget is %s, expected %d" % [str(caching.get("deferred_invalidation_budget", -1)), budget])
	var start := _pages()
	var start_basis := _sun.transform.basis
	var checks := 0
	var check_deferred := 0
	var max_deferred := 0
	var armed := false
	for f in TURN_FRAMES:
		_sun.transform.basis = Basis(Vector3.UP, deg_to_rad(TURN_DEGREES) * float(f + 1)) * start_basis
		if not armed and f % 10 == 5:
			RenderingServer.virtual_shadow_maps_request_check()
			armed = true
		await RenderingServer.frame_post_draw
		max_deferred = maxi(max_deferred, int(_pages().get("deferred_static", 0)))
		if armed:
			var check := RenderingServer.virtual_shadow_maps_get_check_result()
			if bool(check.get("captured", false)):
				armed = false
				checks += 1
				_record_check("budget turn frame %d" % f, check)
				var allocation: Dictionary = check.get("allocation", {})
				check_deferred += int(allocation.get("deferred_static", 0))
	var turned := _pages()
	var deferred := int(turned.get("total_deferred_static", 0)) - int(start.get("total_deferred_static", 0))
	# The sun stops: the deferred pages are re-rendered, at most the budget per frame.
	var converged_at := -1
	var clean_run := 0
	for f in CONVERGE_FRAMES:
		await _frames(1)
		var pages := _pages()
		if int(pages.get("dirty", 0)) == 0 and int(pages.get("deferred_static", 0)) == 0:
			clean_run += 1
			if clean_run == CONVERGED_FRAMES:
				converged_at = f - CONVERGED_FRAMES + 1
				break
		else:
			clean_run = 0
	await _page_check("budget converged")
	_result["checks"] = checks
	_result["deferred_static"] = deferred
	_result["max_deferred_per_frame"] = max_deferred
	_result["deferred_in_checks"] = check_deferred
	_result["converged_after_frames"] = converged_at
	if checks < 3:
		_failures.append("only %d page checks were captured while the sun turned" % checks)
	if deferred <= 0:
		_failures.append("no static page was deferred with a budget of %d pages" % budget)
	if converged_at < 0:
		_failures.append("the cache did not converge within %d frames after the sun stopped" % CONVERGE_FRAMES)


func _case_fog_atlas() -> void:
	_environment.volumetric_fog_enabled = true
	_environment.volumetric_fog_density = 0.02
	_spot.light_volumetric_fog_energy = 1.0
	# After #215 moved VSM volumetric fog to coarse pages, request an explicit atlas pass on the
	# fog-lit spot light so R1's shadow-atlas dirty marking is still exercised while VSM is active.
	_spot.shadow_method = Light3D.SHADOW_METHOD_ATLAS
	var caster := _box("FogCaster", Vector3(1.0, 1.0, 1.0), Vector3(6.0, 2.0, 3.0), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	await _frames(WARMUP_FRAMES)
	var before: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_spot.get_instance())
	var projection: Dictionary = _info().get("projection", {})
	var max_atlas_passes := int(projection.get("atlas_passes", 0))
	var versions := [int(before.get("shadow_version", -1))]
	for i in 3:
		caster.position.x += 0.5
		await _frames(1)
		var move_projection: Dictionary = _info().get("projection", {})
		if int(move_projection.get("atlas_passes", 0)) > max_atlas_passes:
			max_atlas_passes = int(move_projection.get("atlas_passes", 0))
			projection = move_projection
		await _frames(SETTLE_FRAMES - 1)
		var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_spot.get_instance())
		versions.append(int(stats.get("shadow_version", -1)))
	_result["shadow_versions"] = versions
	_result["projection"] = projection
	_result["max_atlas_passes"] = max_atlas_passes
	if max_atlas_passes <= 0:
		_failures.append("the fog-lit spot light ran no shadow-atlas pass: %s" % str(projection))
	for i in range(1, versions.size()):
		if int(versions[i]) <= int(versions[i - 1]):
			_failures.append("move %d: the fog-lit spot light's atlas shadow was not redrawn (shadow versions %s)" % [i, str(versions)])
			break


# A MultiMeshInstance3D of slabs (TRANSFORM_3D) with a custom AABB, which suppresses the AABB notification a buffer
# change would otherwise send.
func _slab_multimesh(p_name: String, p_positions: Array, p_custom_aabb: AABB, p_mode: int) -> MultiMeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = SLAB_SIZE
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = mesh
	multimesh.instance_count = p_positions.size()
	for i in p_positions.size():
		var position: Vector3 = p_positions[i]
		multimesh.set_instance_transform(i, Transform3D(Basis(), position))
	multimesh.custom_aabb = p_custom_aabb
	var node := MultiMeshInstance3D.new()
	node.name = p_name
	node.multimesh = multimesh
	node.material_override = _material(Color(0.8, 0.8, 0.8))
	node.shadow_cache_invalidation = p_mode
	add_child(node)
	return node


# The buffer of a TRANSFORM_3D MultiMesh with unrotated instances at p_positions.
func _slab_buffer(p_positions: Array) -> PackedFloat32Array:
	var buffer := PackedFloat32Array()
	for p: Vector3 in p_positions:
		buffer.append_array(PackedFloat32Array([1.0, 0.0, 0.0, p.x, 0.0, 1.0, 0.0, p.y, 0.0, 0.0, 1.0, p.z]))
	return buffer


func _case_multimesh() -> void:
	var a0 := Vector3(1.0, 1.5, -3.0)
	var a1 := Vector3(4.0, 1.5, -3.0)
	var b0 := Vector3(-5.0, 1.5, -1.0)
	var b1 := Vector3(-2.0, 1.5, -1.0)
	var c0 := Vector3(-1.0, 1.5, 3.0)
	var c1 := Vector3(2.0, 1.5, 3.0)
	# Each custom AABB holds both of its slab's places.
	var by_instance := _slab_multimesh("MultiMeshTransform", [a0], _slab_aabb(a0).merge(_slab_aabb(a1)), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	var by_buffer := _slab_multimesh("MultiMeshBuffer", [b0], _slab_aabb(b0).merge(_slab_aabb(b1)), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	var rigid := _slab_multimesh("MultiMeshRigid", [c0], _slab_aabb(c0).merge(_slab_aabb(c1)), GeometryInstance3D.SHADOW_CACHE_INVALIDATION_RIGID)
	var points := {"a0": _shadow_point(a0), "a1": _shadow_point(a1), "b0": _shadow_point(b0), "b1": _shadow_point(b1), "c0": _shadow_point(c0), "c1": _shadow_point(c1)}
	var steps := {}
	await _frames(WARMUP_FRAMES)
	_expect_shadows("start", _multimesh_expect(points, false), steps)
	by_instance.multimesh.set_instance_transform(0, Transform3D(Basis(), a1))
	by_buffer.multimesh.buffer = _slab_buffer([b1])
	rigid.multimesh.set_instance_transform(0, Transform3D(Basis(), c1))
	await _frames(SETTLE_FRAMES)
	_expect_shadows("moved", _multimesh_expect(points, true), steps)
	await _frames(COOLDOWN_FRAMES)
	_expect_shadows("settled", _multimesh_expect(points, true), steps)
	by_instance.multimesh.set_instance_transform(0, Transform3D(Basis(), a0))
	by_buffer.multimesh.buffer = _slab_buffer([b0])
	rigid.multimesh.set_instance_transform(0, Transform3D(Basis(), c0))
	await _frames(SETTLE_FRAMES + COOLDOWN_FRAMES)
	_expect_shadows("back", _multimesh_expect(points, false), steps)
	_result["steps"] = steps


func _multimesh_expect(p_points: Dictionary, p_moved: bool) -> Dictionary:
	var expect := {}
	for slab in ["a", "b", "c"]:
		expect[slab + "0"] = [p_points[slab + "0"], not p_moved]
		expect[slab + "1"] = [p_points[slab + "1"], p_moved]
	return expect


func _case_range_swap() -> void:
	var near_place := Vector3(1.0, 1.5, -3.0)
	var far_place := Vector3(4.0, 1.5, -3.0)
	# The camera is about 20 m away: a range ending at 1 m hides a slab, one beginning at 1000 m too.
	var near := _box("RangeNear", SLAB_SIZE, near_place, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	near.visibility_range_end = 1000.0
	var far := _box("RangeFar", SLAB_SIZE, far_place, GeometryInstance3D.SHADOW_CACHE_INVALIDATION_AUTO)
	far.visibility_range_begin = 1000.0
	var near_point := _shadow_point(near_place)
	var far_point := _shadow_point(far_place)
	var steps := {}
	await _frames(WARMUP_FRAMES)
	_expect_shadows("start", {"near": [near_point, true], "far": [far_point, false]}, steps)
	near.visibility_range_end = 1.0
	far.visibility_range_begin = 0.0
	await _frames(SETTLE_FRAMES)
	_expect_shadows("swapped", {"near": [near_point, false], "far": [far_point, true]}, steps)
	near.visibility_range_end = 1000.0
	far.visibility_range_begin = 1000.0
	await _frames(SETTLE_FRAMES)
	_expect_shadows("back", {"near": [near_point, true], "far": [far_point, false]}, steps)
	_result["steps"] = steps


func _finish() -> void:
	for path in _temp_files:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
