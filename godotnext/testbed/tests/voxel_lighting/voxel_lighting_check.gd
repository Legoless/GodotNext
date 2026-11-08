extends Node3D
## #188: the kitbash must store voxel-lighting hits. A missing counter fails the run.
## Hardware ray tracing also requires coverage and resampled radiance.
## Set rendering/global_illumination/nextgi/enabled before launch. The tier stays
## disabled while that project setting is off.
## hardware_ray_tracing Off (1) is the software tier and only requires stored hits.

var _frames := 0


func _ready() -> void:
	var kitbash: PackedScene = load("res://scenes/swrt_kitbash/swrt_kitbash.tscn")
	var root := kitbash.instantiate()
	var world := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		world.environment.nextgi_enabled = true
	add_child(root)
	var camera := Camera3D.new()
	add_child(camera)
	camera.position = Vector3(0.0, 2.0, 8.0)
	camera.look_at(Vector3(0.0, 1.0, 0.0))
	camera.current = true


func _fail(reason: String) -> void:
	push_error(reason)
	print("VOXEL_LIGHTING_FAIL %s" % reason)
	get_tree().quit(1)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < 240:
		return
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var required: PackedStringArray = PackedStringArray([
		"voxel_lighting_retrace",
		"voxel_lighting_coverage_ppm",
		"voxel_lighting_volume_bytes",
		"voxel_lighting_stored_hits",
		"voxel_lighting_resampled_nonzero",
	])
	for key in required:
		if not counters.has(key):
			_fail("missing counter %s" % key)
			return
	if not info.has("tier"):
		_fail("missing tier")
		return
	var retrace := int(counters["voxel_lighting_retrace"])
	var coverage_ppm := int(counters["voxel_lighting_coverage_ppm"])
	var volume_bytes := int(counters["voxel_lighting_volume_bytes"])
	var stored := int(counters["voxel_lighting_stored_hits"])
	var resampled := int(counters["voxel_lighting_resampled_nonzero"])
	var tier := str(info["tier"])
	print("VOXEL_LIGHTING retrace=%d coverage_ppm=%d volume_bytes=%d stored_hits=%d resampled_nonzero=%d tier=%s" % [retrace, coverage_ppm, volume_bytes, stored, resampled, tier])
	if stored <= 0:
		_fail("stored hits are 0")
		return
	var ray_tracing := int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing"))
	if ray_tracing != 1:
		if coverage_ppm <= 0:
			_fail("coverage is 0")
			return
		if resampled <= 0:
			_fail("resampled radiance is 0")
			return
	get_tree().quit(0)
