extends Node3D
## NextGeo two-phase occlusion check (#76). A persistent VGD building sits
## behind a plain (non-VGD) box that is toggled in front of it every few
## frames. Phase 1 tests the previous frame's Hi-Z, phase 2 re-tests the
## occluded instance against the Hi-Z built from the current depth, so on the
## frames the occluder moves away the building is disoccluded in the same
## frame and the cut matches the CPU reference.
##
## Requires the generated Dense City kit and
## rendering/nextgeo/debug_stats=true, rendering/nextgeo/occlusion=true.

const KIT := "res://generated/dense_city/kit_micro_detail/building_a.glb"
const COVERED := Vector3(0.0, 20.0, 0.0)
const AWAY := Vector3(0.0, 20.0, 300.0)
const TOGGLE_FRAME := 10
const TOGGLE_PERIOD := 8
const LAST_FRAME := 72

var _occluder: MeshInstance3D
var _frames := 0
var _ticks: Dictionary = {}


func _ready() -> void:
	if not ResourceLoader.exists(KIT):
		print("NEXTGEO_OCCLUDER skip: %s is missing." % KIT)
		get_tree().quit(0)
		return
	var scene: PackedScene = load(KIT)
	var building: Node3D = scene.instantiate()
	building.position = Vector3(0.0, 0.0, -200.0)
	add_child(building)

	var box := BoxMesh.new()
	box.size = Vector3(120.0, 120.0, 4.0)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.2, 0.2)
	box.material = material
	_occluder = MeshInstance3D.new()
	_occluder.mesh = box
	_occluder.position = COVERED
	add_child(_occluder)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 8.0, 55.0)
	camera.rotation_degrees = Vector3(-5.0, 0.0, 0.0)
	camera.current = true
	add_child(camera)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	add_child(sun)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.5, 0.6, 0.7)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.5, 0.5, 0.5)
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames >= TOGGLE_FRAME and (_frames - TOGGLE_FRAME) % TOGGLE_PERIOD == 0:
		_occluder.position = AWAY if _occluder.position == COVERED else COVERED
	var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var tick := int(stats.get("tick", -1))
	if tick >= 0 and not _ticks.has(tick):
		_ticks[tick] = stats
		if tick == 1:
			print("NEXTGEO_OCCLUDER_DUMP %s" % JSON.stringify(stats))
		print("NEXTGEO_OCCLUDER tick=%d phase1=%s phase2=%s hit=%s tested=%s clusters=%s cpu_clusters=%s match=%s" % [
			tick, stats.get("phase1_instances", -1), stats.get("phase2_instances", -1),
			stats.get("hiz_hit", -1), stats.get("hiz_tested", -1), stats.get("clusters", -1),
			stats.get("cpu_clusters", -1), stats.get("cut_match", false)])
	if _frames >= LAST_FRAME:
		_finish()


func _finish() -> void:
	var occluded_ticks := 0
	var disoccluded_ticks := 0
	var disoccluded_phase2_ticks := 0
	var mismatched_ticks := 0
	for tick in _ticks:
		var stats: Dictionary = _ticks[tick]
		var phase2 := int(stats.get("phase2_instances", 0))
		var clusters := int(stats.get("clusters", 0))
		var matched := bool(stats.get("cut_match", false))
		if phase2 > 0 and clusters == 0:
			occluded_ticks += 1
		if clusters > 0:
			if matched:
				disoccluded_ticks += 1
			if phase2 > 0:
				disoccluded_phase2_ticks += 1
			if not matched:
				mismatched_ticks += 1
	var ok := true
	if occluded_ticks <= 0:
		print("NEXTGEO_OCCLUDER FAIL: no frame kept the building in phase 2 with the occluder in front.")
		ok = false
	if disoccluded_ticks <= 0:
		print("NEXTGEO_OCCLUDER FAIL: the building was not disoccluded or did not match the CPU reference.")
		ok = false
	if disoccluded_phase2_ticks <= 0:
		print("NEXTGEO_OCCLUDER FAIL: no frame disoccluded the building in the phase-2 pass.")
		ok = false
	if mismatched_ticks > 0:
		print("NEXTGEO_OCCLUDER FAIL: %d disoccluded frames disagree with the CPU reference." % mismatched_ticks)
		ok = false
	print("NEXTGEO_OCCLUDER_SUMMARY ticks=%d occluded=%d disoccluded=%d disoccluded_phase2=%d mismatched=%d" % [
		_ticks.size(), occluded_ticks, disoccluded_ticks, disoccluded_phase2_ticks, mismatched_ticks])
	get_tree().quit(0 if ok else 1)
