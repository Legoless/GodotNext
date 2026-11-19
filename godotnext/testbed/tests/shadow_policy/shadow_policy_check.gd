extends Node3D

## RT visibility policy check (#145, plan §21.3 contract M4, GM-4's caster-mask matrix).
##
## Loads the RT Shadow Yard (--scene=, by default its own scene): 20 occluders, occluder i on render layer i, 20
## matrix spot lights, light i casting layer i only, 44 omni lights and the sun casting every layer, alpha-tested
## foliage and translucent glass. With rendering/global_illumination/nextgi/debug/shadow_ray_check on, NextGI traces
## 65,536 rays aimed at the opaque casters through the shared shadow-ray include, for eight caster masks (all
## layers, layers 1, 2, 5, 8 and 20, and two multi-layer masks), and counts how many agree with the raster shadow
## pass's caster rule computed on the CPU from the same triangles (next_gi_shadow_policy.h). Every run must agree on
## 100% of the 65,536 rays.
##
## Three phases, so both the opaque fast path, the candidate loop and the fallback run, and the 60-frame
## hysteresis is crossed:
##   1. The yard's lights: all layers and layers 1 to 5 hold the six TLAS mask bits (the six most frequent caster
##      masks, ties by the lower mask). Layer 5's mask (16) is allocated, 0x0F0F0 falls back.
##   2. Three matrix lights switch to 0x0F0F0: it becomes the second most frequent mask, but the allocation holds
##      for 60 frames; runs in that window must still agree (0x0F0F0 still falls back).
##   3. After the flip, 0x0F0F0 holds a bit and layer 5's mask falls back; runs must agree again.
##
## Run it with NextGI on (High), on the hardware tier, through run_shadow_policy_check.sh:
##   godot --path godotnext/testbed --rendering-driver vulkan res://tests/shadow_policy/shadow_policy_check.tscn
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const SETTING := "rendering/global_illumination/nextgi/debug/shadow_ray_check"
const YARD := "res://scenes/rt_shadow_yard/rt_shadow_yard.tscn"
const RAYS := 65536
const MULTI_MASK := 0x0F0F0
const LAYER_5_MASK := 16
const SWITCHED_LIGHTS := ["MatrixLight05", "MatrixLight06", "MatrixLight07"]
const FALLBACK := 1 << 9
const LOOP := 1 << 8
const RUNS_PER_PHASE := 3
const MAX_WAIT_FRAMES := 1200
const HYSTERESIS_FRAMES := 60

var _failures := PackedStringArray()
var _result := {}
var _scene_path := YARD


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--scene="):
			_scene_path = arg.trim_prefix("--scene=")
	ProjectSettings.set_setting(SETTING, true)
	var packed: PackedScene = load(_scene_path)
	_check(packed != null, "the scene loads: " + _scene_path)
	if packed == null:
		_finish()
		return
	var yard := packed.instantiate()
	add_child(yard)
	var world_environment := yard.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world_environment != null and world_environment.environment != null:
		world_environment.environment = world_environment.environment.duplicate()
		world_environment.environment.nextgi_enabled = true
	var camera := yard.get_node_or_null("cam_main") as Camera3D
	if camera != null:
		camera.current = true
	_run(yard)


func _run(p_yard: Node) -> void:
	await _frames(20)
	var tier: Dictionary = RenderingServer.nextgi_get_tier_info()
	_result["tier"] = String(tier.get("tier", ""))
	_check(_result["tier"] == "HWRT", "the hardware tier is active (%s)" % _result["tier"])
	if _result["tier"] != "HWRT":
		_finish()
		return

	# 1. The yard's own lights.
	var first := await _runs("yard", -1)
	var generation := int(first.get("generation", -1))
	_check(_allocated(MULTI_MASK) == false, "0x0F0F0 holds no bit at first (%s)" % str(first.get("allocation", [])))
	_check(_allocated(LAYER_5_MASK), "layer 5's mask holds a bit at first (%s)" % str(first.get("allocation", [])))

	# 2. Three matrix lights switch to the multi-layer mask: the allocation holds for 60 frames.
	for light_name in SWITCHED_LIGHTS:
		var light := p_yard.get_node_or_null(light_name) as Light3D
		_check(light != null, "the yard has " + light_name)
		if light != null:
			light.shadow_caster_mask = MULTI_MASK
	var switched_frame := Engine.get_frames_drawn()
	var pending := await _runs("pending", generation, switched_frame + HYSTERESIS_FRAMES - 10)
	_check(int(pending.get("generation", -1)) == generation, "the allocation holds while the new choice is younger than 60 frames")

	# 3. After the flip.
	var waited := 0
	while int(RenderingServer.nextgi_get_counters().get("shadow_allocation_generation", -1)) == generation and waited < MAX_WAIT_FRAMES:
		await _frames(1)
		waited += 1
	var flip_frames := Engine.get_frames_drawn() - switched_frame
	_result["flip_frames"] = flip_frames
	_check(flip_frames >= HYSTERESIS_FRAMES, "the allocation changed no earlier than 60 frames after the lights did (%d)" % flip_frames)
	_check(_allocated(MULTI_MASK), "0x0F0F0 holds a bit after the flip")
	_check(not _allocated(LAYER_5_MASK), "layer 5's mask lost its bit after the flip")
	var flipped := await _runs("flipped", generation + 1)

	# The lights' words follow the allocation: 0x0F0F0 fell back, then took a bit; layer 5's mask the other way.
	var multi_index := _mask_index(first, MULTI_MASK)
	var layer_index := _mask_index(first, LAYER_5_MASK)
	if multi_index >= 0 and layer_index >= 0:
		_check((int(first["modes"][multi_index]) & FALLBACK) != 0 and (int(flipped["modes"][multi_index]) & FALLBACK) == 0, "0x0F0F0 went from the fallback to an allocated bit")
		_check((int(first["modes"][layer_index]) & FALLBACK) == 0 and (int(flipped["modes"][layer_index]) & FALLBACK) != 0, "layer 5's mask went from an allocated bit to the fallback")
	var paths := {"fast": 0, "loop": 0, "fallback": 0}
	for phase in [first, pending, flipped]:
		for path in paths:
			paths[path] += int(phase.get(path, 0))
	_result["paths"] = paths
	for path in paths:
		_check(int(paths[path]) > 0, "rays took the %s path" % path)
	_finish()


## Waits for RUNS_PER_PHASE check runs against the allocation p_generation (any with -1), or with p_until_frame
## for every run until that frame, and checks each: 65,536 rays, all agreeing with the raster caster rule.
func _runs(p_phase: String, p_generation: int, p_until_frame := -1) -> Dictionary:
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var last_run := int(counters.get("shadow_check_runs", 0))
	var phase := {"runs": 0, "fast": 0, "loop": 0, "fallback": 0}
	var waited := 0
	while waited < MAX_WAIT_FRAMES:
		if p_until_frame >= 0 and Engine.get_frames_drawn() >= p_until_frame and int(phase["runs"]) > 0:
			break
		if p_until_frame < 0 and int(phase["runs"]) >= RUNS_PER_PHASE:
			break
		await _frames(1)
		waited += 1
		counters = RenderingServer.nextgi_get_counters()
		var run := int(counters.get("shadow_check_runs", 0))
		if run == last_run:
			continue
		last_run = run
		var generation := int(counters.get("shadow_check_generation", -1))
		if p_generation >= 0 and generation != p_generation:
			continue
		phase["runs"] = int(phase["runs"]) + 1
		phase["generation"] = generation
		phase["allocation"] = counters.get("shadow_allocation", [])
		phase["masks"] = counters.get("shadow_check_masks", [])
		phase["modes"] = counters.get("shadow_check_modes", [])
		phase["fast"] = int(phase["fast"]) + int(counters.get("shadow_check_fast_rays", 0))
		phase["loop"] = int(phase["loop"]) + int(counters.get("shadow_check_loop_rays", 0))
		phase["fallback"] = int(phase["fallback"]) + int(counters.get("shadow_check_fallback_rays", 0))
		phase["expected_occluded"] = int(counters.get("shadow_check_expected_occluded", 0))
		phase["traced_occluded"] = int(counters.get("shadow_check_traced_occluded", 0))
		phase["alpha_instances"] = int(counters.get("shadow_alpha_instances", 0))
		phase["shadow_only_instances"] = int(counters.get("shadow_only_instances", 0))
		phase["word_misaligned"] = int(counters.get("shadow_word_misaligned", -1))
		var rays := int(counters.get("shadow_check_rays", 0))
		var agree := int(counters.get("shadow_check_agree", 0))
		_check(rays == RAYS, "%s run %d traced %d rays, not %d" % [p_phase, phase["runs"], rays, RAYS])
		# Both answers must be in the matrix, or agreement would prove nothing about the occluded (or clear) rays.
		var expected_occluded := int(phase["expected_occluded"])
		_check(expected_occluded > 0 and expected_occluded < rays, "%s run %d: %d of %d rays are expected occluded; the matrix needs both answers" % [p_phase, phase["runs"], expected_occluded, rays])
		_check(int(phase["word_misaligned"]) == 0, "%s run %d: %d shadow instance words out of step with the TLAS custom indices" % [p_phase, phase["runs"], int(phase["word_misaligned"])])
		_check(agree == rays, "%s run %d: %d of %d rays agree with the raster caster rule (first disagreement at ray %d; fast %d, loop %d, fallback %d disagree)" % [p_phase, phase["runs"], agree, rays, int(counters.get("shadow_check_first_disagreement", -1)), int(counters.get("shadow_check_fast_disagree", 0)), int(counters.get("shadow_check_loop_disagree", 0)), int(counters.get("shadow_check_fallback_disagree", 0))])
		phase["agree"] = agree
		phase["rays"] = rays
	_check(int(phase["runs"]) > 0, "the %s phase got a check run" % p_phase)
	_result[p_phase] = phase
	return phase


func _allocated(p_mask: int) -> bool:
	var allocation = RenderingServer.nextgi_get_counters().get("shadow_allocation", PackedInt64Array())
	return allocation.has(p_mask)


func _mask_index(p_phase: Dictionary, p_mask: int) -> int:
	var masks = p_phase.get("masks", PackedInt64Array())
	return masks.find(p_mask)


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _check(p_condition: bool, p_message: String) -> void:
	if not p_condition:
		_failures.append(p_message)
		printerr("shadow_policy_check: " + p_message)


func _finish() -> void:
	ProjectSettings.set_setting(SETTING, false)
	_result["pass"] = _failures.is_empty()
	_result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
