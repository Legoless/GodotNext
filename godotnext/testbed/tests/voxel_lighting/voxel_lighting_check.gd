extends Node3D
## #188: the kitbash must store voxel-lighting hits. A missing counter fails the run.
## Both tiers also require coverage and resampled radiance and then print
## VOXEL_LIGHTING_ASSERTED: the software tier captures and relights the SRC too (its relight's
## shadow rays march the global SDF), so its voxels shade from live pages. Under Lumen Lite
## (lite_beta: Medium with the irradiance field) both
## are informational: the run prints them on a VOXEL_LIGHTING_LITE line and does not fail on them.
## Lite's half-size surface cache keeps radiance pages for few of the kitbash's 2,400 rocks, and
## voxel lighting resamples only paged instances (lead decision 2026-11-10, plan §21.2.5).
## run_voxel_lighting_check.sh requests quality High for the asserting run, not the device's
## auto pick. Coverage and resampled radiance are GPU counters, so the check turns on
## debug/voxel_lighting_readback.
## Set rendering/global_illumination/nextgi/enabled before launch. The tier stays
## disabled while that project setting is off.
## GodotNext: ps_swrt_trace (#186, #188). With debug/deny_ray_query (a device without ray query)
## the run must also be on the software tier for NO_RAY_QUERY, with no RT arena, no ray-query
## ShaderRD created in the process and SWRT instances (runtime mesh SDFs from the CPU copies).
## With the user argument --expected-tier=HWRT or --expected-tier=SWRT the run fails on any other
## tier; run_voxel_lighting_check.sh passes SWRT for its hardware_ray_tracing Off and deny rows.
## --expected-tier=device expects the device's Auto pick: HWRT when the rendering device reports
## SUPPORTS_RAY_QUERY_HARDWARE (and debug/deny_ray_query is off), else SWRT. The runner passes it
## for its Auto rows.

const READBACK := "rendering/global_illumination/nextgi/debug/voxel_lighting_readback"

var _frames := 0
var _expected_tier := ""


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expected-tier="):
			_expected_tier = arg.trim_prefix("--expected-tier=")
	if _expected_tier == "device":
		var device := RenderingServer.get_rendering_device()
		var hardware := device != null and device.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
		var denied := bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false))
		_expected_tier = "HWRT" if hardware and not denied else "SWRT"
	if not _expected_tier.is_empty() and _expected_tier != "HWRT" and _expected_tier != "SWRT":
		_fail("--expected-tier must be HWRT, SWRT or device, got %s" % _expected_tier)
		return
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
	if not _expected_tier.is_empty() and tier != _expected_tier:
		_fail("ran on the %s tier, expected %s" % [tier, _expected_tier])
		return
	if stored <= 0:
		_fail("stored hits are 0")
		return
	if bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false)):
		var deny_failure := _deny_failure(info, counters)
		if not deny_failure.is_empty():
			_fail(deny_failure)
			return
	if bool(info.get("lite_beta", false)):
		var pages := int(info.get("src_page_count", -1))
		print("VOXEL_LIGHTING_LITE tier=%s coverage_ppm=%d resampled_nonzero=%d unpaged_hits=%d src_page_count=%d (informational; coverage is asserted at High)" % [tier, coverage_ppm, resampled, unpaged, pages])
	else:
		if coverage_ppm <= 0:
			_fail("coverage is 0 on the %s tier" % tier)
			return
		if resampled <= 0:
			_fail("resampled radiance is 0 on the %s tier" % tier)
			return
		print("VOXEL_LIGHTING_ASSERTED tier=%s quality=%s coverage_ppm=%d resampled_nonzero=%d" % [tier, quality, coverage_ppm, resampled])
	ProjectSettings.set_setting(READBACK, false)
	get_tree().quit(0)


# GodotNext: ps_swrt_trace (#186, #188). A device without ray query (debug/deny_ray_query): the
# software tier for NO_RAY_QUERY, no RT arena, no ray-query ShaderRD in the process, and SWRT
# instances, whose runtime mesh SDFs come from the CPU copies kept without ray query.
func _deny_failure(p_info: Dictionary, p_counters: Dictionary) -> String:
	if str(p_info.get("tier", "")) != "SWRT" or str(p_info.get("reason", "")) != "NO_RAY_QUERY":
		return "deny_ray_query: tier %s/%s, expected SWRT/NO_RAY_QUERY" % [str(p_info.get("tier", "")), str(p_info.get("reason", ""))]
	if int(p_counters.get("rt_arena_bytes", -1)) != 0:
		return "deny_ray_query: %d RT arena bytes" % int(p_counters.get("rt_arena_bytes", -1))
	if int(p_info.get("ray_query_shader_versions", -1)) != 0:
		return "deny_ray_query: %d ray-query ShaderRD versions" % int(p_info.get("ray_query_shader_versions", -1))
	if int(p_info.get("swrt_trace_instances", 0)) <= 0:
		return "deny_ray_query: no SWRT instances (no runtime mesh SDFs)"
	return ""
