extends Node3D

## Virtual Shadow Map core on the GPU (#220; plan §21.3 contract S2, budget V6).
##
## 1. A small lit set: a floor, a camera, and one shadow-casting light of each kind (directional,
##    spot, omni, area). With GODOT_DEV_VSM in the environment (--expect-active=true) each light
##    gets its virtual maps: the directional light one per clipmap level for its 100 m max distance
##    (7), the spot 1, the omni 6 and the area 5. Without it (--expect-active=false) nothing is
##    allocated and no GPU resource exists before the self-check. A second view, a SubViewport
##    whose camera follows the main one with a 20 m far plane, draws the same lights every frame:
##    the sun's levels follow its max distance, not a view's far plane, so no map is reallocated.
## 2. Active only: the cameras move one level-0 page along the sun's X axis, so the clipmap scroll
##    pass runs. Then 64 more shadow-casting omni lights join: the page table of 65 omni lights,
##    a spot, an area and the directional light must stay within V6's 16 MiB. Then those 64 are
##    hidden: they keep their maps while the table has room, and when 64 new omni lights join, the
##    hidden ones (stale: in no view for two frames) make room. The table must not grow.
## 3. RenderingServer.virtual_shadow_maps_run_self_check(): the pool at each quality (V6 sizes),
##    the GPU scroll pass against its CPU model, --queries GPU lookups against the CPU model, and
##    --cycles map allocate and free cycles with the pool recreated in between, followed by a
##    read-back of the whole page table.
## 4. RenderingDevice's buffer and texture counters, read before the check and SETTLE_FRAMES
##    frames after it (once RenderingDevice has released what the check freed), must differ by
##    exactly the check's expected deltas: no GPU memory leaks. The driver's total (MEMORY_TOTAL) is
##    reported beside them.
## 5. Active only: the lights are freed; their maps must be gone on the next frame.
##
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.
## User arguments: --expect-active=true|false (default: GODOT_DEV_VSM is set), --queries=N
## (default 65536), --cycles=N (default 10000), --seed=N.

const SceneArgs = preload("res://scenes/scene_args.gd")

const MIB := 1024 * 1024
const V6_POOL_MIB: Array[int] = [64, 128, 256, 512]
const V6_TABLE_MIB := 16
const SUN_LEVELS := 7 # clipmap levels for the default 100 m directional max distance
const FIRST_MAPS := SUN_LEVELS + 1 + 6 + 5
const EXTRA_OMNI := 64
const SHORT_FAR := 20.0
const VIEW_FRAMES := 6
const WARMUP_FRAMES := 20
const STABLE_FRAMES := 10
const SETTLE_FRAMES := 12
const MAX_FREE_FRAMES := 1

var _expect_active := false
var _queries := 65536
var _cycles := 10000
var _seed := 220

var _frame := 0
var _step := "warmup"
var _step_frame := 0
var _camera: Camera3D
var _short_camera: Camera3D
var _sun: DirectionalLight3D
var _lights: Array[Light3D] = []
var _extra: Array[Light3D] = []
var _failures := PackedStringArray()
var _result := {}
var _baseline := {}
var _check := {}
var _scroll_before := 0
var _allocations_before := 0
var _info_before := {}
var _freed_frame := -1


func _ready() -> void:
	_expect_active = SceneArgs.value("--expect-active", "true" if OS.get_environment("GODOT_DEV_VSM") != "" else "false") == "true"
	_queries = SceneArgs.int_value("--queries", _queries)
	_cycles = SceneArgs.int_value("--cycles", _cycles)
	_seed = SceneArgs.int_value("--seed", _seed)
	_result["expect_active"] = _expect_active
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()

	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(60.0, 60.0)
	floor_mesh.mesh = plane
	add_child(floor_mesh)
	var box := MeshInstance3D.new()
	box.mesh = BoxMesh.new()
	box.position = Vector3(0.0, 0.5, -4.0)
	add_child(box)

	_camera = Camera3D.new()
	add_child(_camera)
	_camera.position = Vector3(0.0, 3.0, 6.0)
	_camera.look_at(Vector3(0.0, 0.0, -4.0))
	_camera.current = true

	# The second view: same world, same camera transform, a 20 m far plane.
	var view := SubViewport.new()
	view.size = Vector2i(128, 72)
	view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(view)
	_short_camera = Camera3D.new()
	_short_camera.far = SHORT_FAR
	view.add_child(_short_camera)
	_short_camera.global_transform = _camera.global_transform
	_short_camera.current = true

	_sun = DirectionalLight3D.new()
	_sun.shadow_enabled = true
	_sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	add_child(_sun)
	_lights.append(_sun)
	var spot := SpotLight3D.new()
	spot.shadow_enabled = true
	spot.position = Vector3(-2.0, 3.0, -2.0)
	spot.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	add_child(spot)
	_lights.append(spot)
	var omni := OmniLight3D.new()
	omni.shadow_enabled = true
	omni.position = Vector3(2.0, 2.0, -3.0)
	add_child(omni)
	_lights.append(omni)
	var area := AreaLight3D.new()
	area.shadow_enabled = true
	area.position = Vector3(0.0, 3.0, -5.0)
	area.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	add_child(area)
	_lights.append(area)


func _process(_delta: float) -> void:
	_frame += 1
	_step_frame += 1
	match _step:
		"warmup":
			if _step_frame >= WARMUP_FRAMES:
				_check_lights()
				_allocations_before = int(RenderingServer.virtual_shadow_maps_get_debug_info().get("map_allocations", -1))
				_next("views")
		"views":
			if _step_frame >= VIEW_FRAMES:
				_check_views()
				_next("move")
		"move":
			if _step_frame == 1:
				var info := RenderingServer.virtual_shadow_maps_get_debug_info()
				_scroll_before = int(info.get("scroll_passes", 0))
				# One level-0 page (0.0625 m) and a bit along the sun's X axis.
				_camera.global_position += _sun.global_transform.basis.x.normalized() * 0.1
				_short_camera.global_transform = _camera.global_transform
			elif _step_frame >= 3:
				_check_scroll()
				_extra = _add_omni_lights()
				_next("many_lights")
		"many_lights":
			if _step_frame >= 3:
				_check_many_lights()
				for light in _extra:
					light.visible = false
				_next("hidden")
		"hidden":
			if _step_frame >= 4:
				_info_before = RenderingServer.virtual_shadow_maps_get_debug_info()
				_check_hidden()
				_add_omni_lights()
				_next("evict")
		"evict":
			if _step_frame >= 3:
				_check_evict()
				_next("baseline")
		"baseline":
			if _step_frame == 1:
				_baseline = _memory()
			elif _step_frame >= STABLE_FRAMES:
				var again := _memory()
				# MEMORY_TOTAL on Metal queries MTLDevice.currentAllocatedSize(), which includes
				# driver-internal and GPU-validation scratch heaps; match _check_memory() and check
				# RenderingDevice's tracked buffer and texture allocations.
				var stable := int(again["buffers"]) == int(_baseline["buffers"]) and int(again["textures"]) == int(_baseline["textures"])
				_result["baseline_stable"] = stable
				if not stable:
					_failures.append("RenderingDevice memory changed with nothing happening: %s then %s" % [_baseline, again])
				_baseline = again
				_run_self_check()
				_next("settle")
		"settle":
			if _step_frame >= SETTLE_FRAMES:
				_check_memory()
				if _expect_active:
					for light in _lights:
						light.free()
					_lights.clear()
					_freed_frame = _frame
					_next("freed")
				else:
					_finish()
		"freed":
			var info := RenderingServer.virtual_shadow_maps_get_debug_info()
			if int(info.get("maps_allocated", -1)) == 0 and int(info.get("lights", -1)) == 0:
				_result["frames_to_free"] = _frame - _freed_frame
				_finish()
			elif _frame - _freed_frame > MAX_FREE_FRAMES:
				_failures.append("the freed lights still hold %d maps %d frames later" % [int(info.get("maps_allocated", -1)), _frame - _freed_frame])
				_result["frames_to_free"] = _frame - _freed_frame
				_finish()


func _next(p_step: String) -> void:
	_step = p_step
	_step_frame = 0


func _check_lights() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	if info.is_empty():
		_failures.append("virtual_shadow_maps_get_debug_info() is empty: no virtual shadow map storage")
		return
	_result["info"] = info
	if bool(info.get("active", false)) != _expect_active:
		_failures.append("active is %s, expected %s" % [info.get("active"), _expect_active])
	if _expect_active:
		if int(info["lights"]) != 4 or int(info["maps_allocated"]) != FIRST_MAPS:
			_failures.append("4 lights should hold %d maps, got %d lights and %d maps" % [FIRST_MAPS, int(info["lights"]), int(info["maps_allocated"])])
		var pages := int(info["pool_pages"])
		if pages <= 0 or int(info["pool_bytes"]) != pages * 64 * 64 * 4 * 2:
			_failures.append("pool of %d pages has %d bytes" % [pages, int(info["pool_bytes"])])
	else:
		for key in ["lights", "maps_allocated", "pool_pages", "page_table_maps", "page_table_bytes", "pool_driver_bytes"]:
			if int(info[key]) != 0:
				_failures.append("VSM is off but %s is %d" % [key, int(info[key])])


func _check_scroll() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var scrolls := int(info.get("scroll_passes", 0)) - _scroll_before
	_result["scroll_passes_after_move"] = scrolls
	if _expect_active and scrolls < 1:
		_failures.append("moving the camera one level-0 page ran no scroll pass")
	if not _expect_active and scrolls != 0:
		_failures.append("VSM is off but %d scroll passes ran" % scrolls)


func _check_views() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var allocations := int(info.get("map_allocations", -1)) - _allocations_before
	_result["views"] = { "frames": VIEW_FRAMES, "short_far": SHORT_FAR, "map_allocations": allocations, "maps": int(info.get("maps_allocated", -1)), "lights": int(info.get("lights", -1)) }
	if allocations != 0:
		_failures.append("with a second view of %.0f m far plane, %d map ranges were allocated again in %d frames" % [SHORT_FAR, allocations, VIEW_FRAMES])
	if _expect_active and int(info.get("maps_allocated", -1)) != FIRST_MAPS:
		_failures.append("with a second view, 4 lights hold %d maps, expected %d" % [int(info.get("maps_allocated", -1)), FIRST_MAPS])


func _add_omni_lights() -> Array[Light3D]:
	var added: Array[Light3D] = []
	for i in EXTRA_OMNI:
		var light := OmniLight3D.new()
		light.shadow_enabled = true
		light.omni_range = 1.5
		# Eight columns, four rows and two layers in front of the camera, so that none is culled.
		light.position = Vector3(-3.5 + float(i & 7), 0.6 + float(i >> 5), -1.0 - float((i >> 3) & 3))
		add_child(light)
		_lights.append(light)
		added.append(light)
	return added


func _check_many_lights() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var table_bytes := int(info.get("page_table_bytes", -1))
	_result["many_lights"] = { "lights": int(info.get("lights", -1)), "maps": int(info.get("maps_allocated", -1)), "page_table_maps": int(info.get("page_table_maps", -1)), "page_table_bytes": table_bytes }
	if not _expect_active:
		if int(info.get("maps_allocated", -1)) != 0:
			_failures.append("VSM is off but %d maps are allocated" % int(info.get("maps_allocated", -1)))
		return
	var maps := FIRST_MAPS + EXTRA_OMNI * 6
	if int(info.get("lights", -1)) != 4 + EXTRA_OMNI or int(info.get("maps_allocated", -1)) != maps:
		_failures.append("%d lights should hold %d maps, got %d lights and %d maps" % [4 + EXTRA_OMNI, maps, int(info.get("lights", -1)), int(info.get("maps_allocated", -1))])
	if table_bytes != int(info.get("page_table_maps", 0)) * int(info.get("map_table_bytes", 0)) or table_bytes > V6_TABLE_MIB * MIB or table_bytes <= 0:
		_failures.append("the page table of %d maps is %d bytes (V6: at most %d MiB)" % [int(info.get("page_table_maps", 0)), table_bytes, V6_TABLE_MIB])


func _check_hidden() -> void:
	# Hidden lights are in no view's cull result, but nothing needs their maps yet: they keep them.
	var info := _info_before
	_result["hidden"] = { "lights": int(info.get("lights", -1)), "maps": int(info.get("maps_allocated", -1)), "evicted_lights": int(info.get("evicted_lights", -1)), "page_table_maps": int(info.get("page_table_maps", -1)) }
	if not _expect_active:
		return
	if int(info.get("lights", -1)) != 4 + EXTRA_OMNI or int(info.get("evicted_lights", -1)) != 0:
		_failures.append("hidden lights should keep their maps while the table has room: %d lights, %d evicted" % [int(info.get("lights", -1)), int(info.get("evicted_lights", -1))])


func _check_evict() -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var evicted := int(info.get("evicted_lights", -1)) - int(_info_before.get("evicted_lights", 0))
	var lights := int(info.get("lights", -1))
	var maps := int(info.get("maps_allocated", -1))
	_result["evict"] = { "lights": lights, "maps": maps, "evicted_lights": evicted, "page_table_maps": int(info.get("page_table_maps", -1)), "page_table_bytes": int(info.get("page_table_bytes", -1)), "failed_allocations": int(info.get("failed_allocations", -1)) }
	if not _expect_active:
		if maps != 0 or lights != 0:
			_failures.append("VSM is off but %d lights hold %d maps" % [lights, maps])
		return
	# The 64 new lights got their maps from hidden ones, without growing the table or failing.
	if int(info.get("page_table_maps", -1)) != int(_info_before.get("page_table_maps", -2)):
		_failures.append("the page table grew from %d to %d maps although hidden lights could make room" % [int(_info_before.get("page_table_maps", -2)), int(info.get("page_table_maps", -1))])
	if int(info.get("failed_allocations", -1)) != int(_info_before.get("failed_allocations", -2)):
		_failures.append("%d map allocations failed" % (int(info.get("failed_allocations", -1)) - int(_info_before.get("failed_allocations", 0))))
	if evicted <= 0:
		_failures.append("no hidden light was evicted for the new ones")
	var live := 4 + 2 * EXTRA_OMNI - evicted
	if lights != live or maps != FIRST_MAPS + 6 * (2 * EXTRA_OMNI - evicted):
		_failures.append("after %d evictions, expected %d lights and %d maps, got %d and %d" % [evicted, live, FIRST_MAPS + 6 * (2 * EXTRA_OMNI - evicted), lights, maps])


func _memory() -> Dictionary:
	var device := RenderingServer.get_rendering_device()
	var memory := {
		"buffers": device.get_memory_usage(RenderingDevice.MEMORY_BUFFERS),
		"textures": device.get_memory_usage(RenderingDevice.MEMORY_TEXTURES),
		"total": device.get_memory_usage(RenderingDevice.MEMORY_TOTAL),
	}
	return memory


func _run_self_check() -> void:
	var started := Time.get_ticks_msec()
	_check = RenderingServer.virtual_shadow_maps_run_self_check(_seed, _queries, _cycles)
	_result["self_check_ms"] = Time.get_ticks_msec() - started
	_result["check"] = _check
	if _check.is_empty() or _check.has("error"):
		_failures.append("the self-check did not run: %s" % _check.get("error", "empty result"))
		return
	var pools: Array = _check["pools"]
	for i in pools.size():
		var pool: Dictionary = pools[i]
		var v6 := V6_POOL_MIB[i] * MIB
		if not bool(pool["created"]) or int(pool["bytes"]) != v6 or int(pool["texture_bytes"]) != v6:
			_failures.append("%s pool: %d bytes, texture %d bytes, V6 %d" % [pool["quality"], int(pool["bytes"]), int(pool["texture_bytes"]), v6])
		var driver := int(pool["driver_bytes"])
		if driver < v6 or driver > v6 + MIB:
			_failures.append("%s pool: the driver allocated %d bytes for %d" % [pool["quality"], driver, v6])
	if int(_check["scroll_mismatched_entries"]) != 0 or int(_check["scroll_marked_entries"]) <= 0:
		_failures.append("scroll pass: %d of %d entries differ from the CPU model (%d marked)" % [int(_check["scroll_mismatched_entries"]), int(_check["scroll_checked_entries"]), int(_check["scroll_marked_entries"])])
	if int(_check["lookup_matches"]) != _queries or int(_check["lookup_mismatches"]) != 0:
		_failures.append("lookup: %d of %d queries match the CPU model; first mismatch %s" % [int(_check["lookup_matches"]), _queries, _check.get("lookup_first_mismatch", {})])
	for key in ["lookup_at_requested_mip", "lookup_fallbacks", "lookup_misses"]:
		if int(_check[key]) <= 0:
			_failures.append("lookup: no query of the %s kind" % key)
	if int(_check["lookup_mutant_mismatches"]) != int(_check["lookup_fallbacks"]):
		_failures.append("lookup: the CPU model without fallback differs on %d queries, expected the %d fallbacks" % [int(_check["lookup_mutant_mismatches"]), int(_check["lookup_fallbacks"])])
	if int(_check["clipmap_matches"]) != int(_check["clipmap_queries"]) or int(_check["clipmap_queries"]) <= 0:
		_failures.append("clipmap lookup: %d of %d queries match the CPU model; first mismatch %s" % [int(_check["clipmap_matches"]), int(_check["clipmap_queries"]), _check.get("clipmap_first_mismatch", {})])
	for key in ["clipmap_valid", "clipmap_fallbacks", "clipmap_outside", "clipmap_window_stops"]:
		if int(_check[key]) <= 0:
			_failures.append("clipmap lookup: no query of the %s kind" % key)
	if int(_check["clipmap_window_mutant_mismatches"]) != int(_check["clipmap_window_stops"]):
		_failures.append("clipmap lookup: the lookup without the window check differs on %d queries, expected the %d window stops" % [int(_check["clipmap_window_mutant_mismatches"]), int(_check["clipmap_window_stops"])])
	if int(_check["map_guard_matches"]) != int(_check["map_guard_queries"]) or int(_check["map_guard_queries"]) <= 0:
		_failures.append("map guard: %d of %d queries match the CPU model; first mismatch %s" % [int(_check["map_guard_matches"]), int(_check["map_guard_queries"]), _check.get("map_guard_first_mismatch", {})])
	for key in ["map_guard_rejected", "map_guard_valid", "map_guard_mutant_mismatches"]:
		if int(_check[key]) <= 0:
			_failures.append("map guard: no query of the %s kind" % key)
	if int(_check["cycles"]) != _cycles or int(_check["cycle_failures"]) != 0 or int(_check["cycle_allocations"]) <= 0:
		_failures.append("cycles: %d run, %d failed allocations" % [int(_check["cycles"]), int(_check["cycle_failures"])])
	for key in ["leaked_maps", "leaked_lights", "table_nonzero_entries"]:
		if int(_check[key]) != 0:
			_failures.append("after the cycles %s is %d" % [key, int(_check[key])])
	if int(_check["free_maps"]) != int(_check["free_maps_expected"]):
		_failures.append("after the cycles %d maps are free, expected %d" % [int(_check["free_maps"]), int(_check["free_maps_expected"])])
	if int(_check["pool_pages_after"]) != int(_check["pool_pages_before"]):
		_failures.append("the pool was not restored: %d pages, was %d" % [int(_check["pool_pages_after"]), int(_check["pool_pages_before"])])


func _check_memory() -> void:
	if _check.is_empty():
		return
	var after := _memory()
	var delta := {}
	for key in after:
		delta[key] = int(after[key]) - int(_baseline.get(key, 0))
	_result["memory_before"] = _baseline
	_result["memory_after"] = after
	_result["memory_delta"] = delta
	if int(delta["buffers"]) != int(_check["expected_buffer_delta"]):
		_failures.append("buffer memory changed by %d bytes, expected %d" % [int(delta["buffers"]), int(_check["expected_buffer_delta"])])
	if int(delta["textures"]) != int(_check["expected_texture_delta"]):
		_failures.append("texture memory changed by %d bytes, expected %d" % [int(delta["textures"]), int(_check["expected_texture_delta"])])


func _finish() -> void:
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
