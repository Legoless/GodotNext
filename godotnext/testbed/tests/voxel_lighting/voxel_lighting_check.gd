extends Node3D
## #188: the kitbash must store voxel-lighting hits. A missing counter fails the run.
## Hardware ray tracing also requires coverage and resampled radiance and then prints
## VOXEL_LIGHTING_ASSERTED. Under Lumen Lite (lite_beta: Medium with the irradiance field) both
## are informational: the run prints them on a VOXEL_LIGHTING_LITE line and does not fail on them.
## Lite's half-size surface cache keeps radiance pages for few of the kitbash's 2,400 rocks, and
## voxel lighting resamples only paged instances (lead decision 2026-11-10, plan §21.2.5).
## run_voxel_lighting_check.sh requests quality High for the asserting run, not the device's
## auto pick. Coverage and resampled radiance are GPU counters, so the check turns on
## debug/voxel_lighting_readback.
## Set rendering/global_illumination/nextgi/enabled before launch. The tier stays
## disabled while that project setting is off.
## hardware_ray_tracing Off (1) is the software tier and only requires stored hits.

const READBACK := "rendering/global_illumination/nextgi/debug/voxel_lighting_readback"

var _frames := 0


func _ready() -> void:
	ProjectSettings.set_setting(READBACK, true)
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
	ProjectSettings.set_setting(READBACK, false)
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
	var quality := str(info.get("quality", "NONE"))
	var unpaged := int(info.get("voxel_lighting_unpaged_hits", -1))
	print("VOXEL_LIGHTING retrace=%d coverage_ppm=%d volume_bytes=%d stored_hits=%d unpaged_hits=%d resampled_nonzero=%d tier=%s quality=%s" % [retrace, coverage_ppm, volume_bytes, stored, unpaged, resampled, tier, quality])
	if stored <= 0:
		_fail("stored hits are 0")
		return
	var ray_tracing := int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing"))
	if ray_tracing != 1:
		if bool(info.get("lite_beta", false)):
			var pages := int(info.get("src_page_count", -1))
			print("VOXEL_LIGHTING_LITE coverage_ppm=%d resampled_nonzero=%d unpaged_hits=%d src_page_count=%d (informational; coverage is asserted at High)" % [coverage_ppm, resampled, unpaged, pages])
		else:
			if coverage_ppm <= 0:
				_fail("coverage is 0")
				return
			if resampled <= 0:
				_fail("resampled radiance is 0")
				return
			print("VOXEL_LIGHTING_ASSERTED quality=%s coverage_ppm=%d resampled_nonzero=%d" % [quality, coverage_ppm, resampled])
	ProjectSettings.set_setting(READBACK, false)
	get_tree().quit(0)
