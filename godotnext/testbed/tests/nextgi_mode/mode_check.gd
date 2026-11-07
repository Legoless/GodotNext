extends Node3D
## NextGI mode check (#132). Loads Color Box Room A with NextGI on and verifies
## that rendering/global_illumination/nextgi/mode selects reflections-only: the
## status reports the mode and the diffuse gather dispatches drop to zero while
## reflections keep rendering.

const CBR := "res://scenes/color_box_room/cbr_a.tscn"
const WAIT_FRAMES := 240

var _requested := 0
var _lightmapgi: LightmapGI
var _voxelgi: VoxelGI


func _ready() -> void:
	_requested = 2 if "--rough" in OS.get_cmdline_user_args() else int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/mode", 0))
	var path := CBR
	if "--g3" in OS.get_cmdline_user_args():
		path = "res://scenes/roughness_corridor/roughness_corridor.tscn"
	elif "--lightmap" in OS.get_cmdline_user_args():
		path = "res://scenes/color_box_room/cbr_b_lightmap.tscn"
	elif "--voxel" in OS.get_cmdline_user_args():
		path = "res://scenes/legacy/legacy_voxelgi.tscn"
	var packed: PackedScene = load(path)
	var scene: Node3D = packed.instantiate()
	add_child(scene)
	_lightmapgi = scene.get_node_or_null("LightmapGI") as LightmapGI
	_voxelgi = scene.get_node_or_null("VoxelGI") as VoxelGI
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		# Keep the default roughness comparison on the unmodified G3 scene.
		environment.fog_enabled = not "--max-roughness" in OS.get_cmdline_user_args()
		environment.fog_light_color = Color(0.6, 0.65, 0.7)
		environment.fog_density = 0.02
		environment.fog_height = 2.0
		environment.fog_height_density = 0.02
		if "--max-roughness" in OS.get_cmdline_user_args():
			environment.nextgi_max_roughness_to_trace = 0.4
			environment.nextgi_fog_on_reflection_hits = false
		world.environment = environment
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	_run()


func _run() -> void:
	for i in WAIT_FRAMES:
		await RenderingServer.frame_post_draw
	var status: Dictionary = RenderingServer.nextgi_get_status()
	var counters: Dictionary = RenderingServer.nextgi_get_tier_info()
	print("NEXTGI_MODE_COUNTERS " + JSON.stringify(counters))
	var mode := str(status.get("mode", ""))
	var diffuse := int(counters.get("gather_diffuse_dispatches", -1))
	var reflections := bool(status.get("reflections", false))
	var reflection_rays := int(counters.get("reflection_rays", -1))
	print("NEXTGI_MODE requested=%d mode=%s diffuse=%d reflections=%s reflection_rays=%d" % [_requested, mode, diffuse, reflections, reflection_rays])
	var expected := "reflections_only" if _requested == 1 else "gi_and_reflections"
	var ok := mode == expected
	if _requested == 1 and diffuse != 0:
		ok = false
	if _requested == 0 and diffuse <= 0:
		ok = false
	if reflections != bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/reflections", true)):
		ok = false
	if reflections and reflection_rays <= 0:
		ok = false
	if not reflections and reflection_rays != 0:
		ok = false
	if _requested == 2:
		if diffuse <= 0 or reflections or reflection_rays != 0:
			ok = false
		if int(counters.get("rough_reflection_mip_count", 1)) <= 1:
			ok = false
	if "--lightmap" in OS.get_cmdline_user_args():
		var texture_count := 0
		if _lightmapgi != null and _lightmapgi.light_data != null:
			texture_count = _lightmapgi.light_data.get_lightmap_textures().size()
		print("NEXTGI_LIGHTMAP textures=%d" % texture_count)
		if not reflections or _requested != 1 or texture_count == 0:
			ok = false
	if "--voxel" in OS.get_cmdline_user_args():
		var has_bake := _voxelgi != null and _voxelgi.data != null
		print("NEXTGI_VOXELGI baked=%s" % str(has_bake))
		if not reflections or _requested != 1 or not has_bake:
			ok = false
	if "--capture" in OS.get_cmdline_user_args():
		var error := get_viewport().get_texture().get_image().save_png("user://mode_check.png")
		if error != OK:
			ok = false
		else:
			print("NEXTGI_MODE_CAPTURE path=%s" % OS.get_user_data_dir().path_join("mode_check.png"))
	print("NEXTGI_MODE_RESULT pass=%s" % str(ok))
	get_tree().quit(0 if ok else 1)
