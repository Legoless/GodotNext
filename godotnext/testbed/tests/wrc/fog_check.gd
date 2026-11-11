extends Node

## Volumetric fog samples the world radiance cache in Color Box Room A (#44).
## GodotNext: ps_swrt_trace (#186). --expected-tier=HWRT (default) or SWRT: the tier the run must
## report. The software tier has its own WRC producer, and fog reads it there too (L9b). On the
## software tier no ray-query ShaderRD may be created since the tier was selected, nor in the
## whole process when hardware_ray_tracing was Off at launch (Off skips the startup self-test).


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)
	var room: Node3D = load("res://scenes/color_box_room/cbr_a.tscn").instantiate()
	viewport.add_child(room)
	var world_environment := room.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world_environment != null and world_environment.environment != null:
		var environment := world_environment.environment.duplicate()
		environment.nextgi_enabled = true
		environment.sdfgi_enabled = false
		environment.volumetric_fog_enabled = true
		environment.volumetric_fog_density = 0.05
		environment.volumetric_fog_gi_inject = 1.0
		environment.background_mode = Environment.BG_COLOR
		environment.background_color = Color(0.65, 0.75, 0.9)
		environment.background_energy_multiplier = 1.0
		world_environment.environment = environment
	var camera := room.get_node("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	var bytes := 0
	var rays := 0
	var fog_samples := 0
	var fog_bits := 0
	var energy := 0
	var wrc_hits := 0
	var uncovered := 0
	for _frame in 24:
		await get_tree().process_frame
		var info := RenderingServer.nextgi_get_tier_info()
		bytes = maxi(bytes, int(info.get("wrc_bytes", 0)))
		rays = maxi(rays, int(info.get("wrc_rays", 0)))
		fog_samples = maxi(fog_samples, int(info.get("wrc_fog_samples", 0)))
		fog_bits = maxi(fog_bits, int(info.get("wrc_fog_ambient_bits", 0)))
		energy = maxi(energy, int(info.get("wrc_energy_bits", 0)))
		wrc_hits = maxi(wrc_hits, int(info.get("wrc_gather_samples", 0)))
		uncovered = maxi(uncovered, int(info.get("src_gather_uncovered", 0)))
	var failures := PackedStringArray()
	var expected_tier := "HWRT"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expected-tier="):
			expected_tier = arg.trim_prefix("--expected-tier=")
	var final_info := RenderingServer.nextgi_get_tier_info()
	var tier := String(final_info.get("tier", ""))
	if tier != expected_tier:
		failures.append("tier %s, expected %s" % [tier, expected_tier])
	if expected_tier == "SWRT":
		if int(final_info.get("ray_query_shader_versions_since_tier", -1)) != 0:
			failures.append("%d ray-query ShaderRD versions created on the SWRT tier" % int(final_info.get("ray_query_shader_versions_since_tier", -1)))
		if int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing", 0)) == 1 and int(final_info.get("ray_query_shader_versions", -1)) != 0:
			failures.append("%d ray-query ShaderRD versions created in a process started with hardware_ray_tracing Off" % int(final_info.get("ray_query_shader_versions", -1)))
	if bytes != 14680064:
		failures.append("bytes %d" % bytes)
	if rays < 1:
		failures.append("rays %d" % rays)
	if fog_samples < 1:
		failures.append("fog samples %d" % fog_samples)
	if fog_bits < 1:
		failures.append("fog ambient %d energy %d" % [fog_bits, energy])
	return {
		"pass": failures.is_empty(),
		"failures": " | ".join(failures),
		"tier": tier,
		"bytes": bytes,
		"rays": rays,
		"fog_samples": fog_samples,
		"fog_ambient_bits": fog_bits,
		"energy_bits": energy,
		"wrc_gather_samples": wrc_hits,
		"uncovered": uncovered,
	}
