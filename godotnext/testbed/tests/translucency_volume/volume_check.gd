extends Node3D
## #133: Metal smoke checks for the traced translucency volume.
## This is a build validation, not a PQ quality or timing measurement.

const FRAMES := 60


func _ready() -> void:
	var sealed := "--sealed" in OS.get_cmdline_user_args()
	var fog := "--fog" in OS.get_cmdline_user_args()
	var path := "res://scenes/fog_emissive_street/fog_emissive_street.tscn" if fog else ("res://scenes/color_box_room/cbr_a.tscn" if sealed else "res://scenes/glass_water/glass_water.tscn")
	var scene := (load(path) as PackedScene).instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		environment.nextgi_read_sky_light = not sealed
		world.environment = environment
	if sealed:
		var surface := MeshInstance3D.new()
		var plane := PlaneMesh.new()
		plane.size = Vector2(1.5, 1.5)
		surface.mesh = plane
		surface.position = Vector3(-2.0, 1.2, 0.0)
		var material := StandardMaterial3D.new()
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		material.albedo_color = Color(0.3, 0.6, 0.8, 0.35)
		material.roughness = 0.15
		material.cull_mode = BaseMaterial3D.CULL_DISABLED
		surface.material_override = material
		surface.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		scene.add_child(surface)
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	_run()


func _run() -> void:
	for i in FRAMES:
		await RenderingServer.frame_post_draw
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var medium := "--medium" in OS.get_cmdline_user_args()
	var sealed := "--sealed" in OS.get_cmdline_user_args()
	var fog := "--fog" in OS.get_cmdline_user_args()
	var marked := int(info.get("translucency_marked_probes", -1))
	var rays := int(info.get("translucency_probe_rays", -1))
	var hits := int(info.get("translucency_probe_hits", -1))
	var failures := PackedStringArray()
	if String(info.get("tier", "")) != "HWRT":
		failures.append("tier is not HWRT")
	if bool(info.get("translucency_lite", false)) != medium:
		failures.append("Lite mode does not match quality")
	if medium:
		if marked != 0 or rays != 0:
			failures.append("Medium dispatched translucency probes")
	elif not fog:
		if marked <= 0 or marked > 4096:
			failures.append("marked probe count is outside the 1..4096 range")
		if rays < marked * 64:
			failures.append("not every marked probe received 64 rays")
		if hits <= 0:
			failures.append("translucency probe trace found no geometry")
	if sealed and int(info.get("translucency_sky_energy", -1)) != 0:
		failures.append("sky lighting is enabled despite the Environment's sky-occlusion rule")
	if fog and int(info.get("wrc_fog_samples", 0)) <= 0:
		failures.append("volumetric fog did not read the shared volume")
	if not bool(info.get("translucency_history_valid", false)):
		failures.append("temporal history is not valid")
	if int(info.get("translucency_volume_bytes", 0)) != 80 * 45 * 32 * 6 * 8:
		failures.append("current and history volume allocation is incorrect")
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "translucency_volume",
		"pass": failures.is_empty(),
		"medium": medium,
		"sealed": sealed,
		"marked": marked,
		"rays": rays,
		"hits": hits,
		"history": info.get("translucency_history_valid", false),
		"failures": failures,
	}))
	print("GL7_PQ_DEFERRED sh_l1_error=not_run sealed_sky_ratio=not_run timing=not_run")
	get_tree().quit(0 if failures.is_empty() else 1)
