extends Node3D
## #126: a seeded valley with far landmarks and a reflective lake.
## This is a build/Metal smoke test, not a PQ quality or timing measurement.
## Run through tests/far_field/run_far_field.sh to enable NextGI before renderer initialization.
## Append -- --off or -- --occlusion to exercise the disabled or sky-visibility path.

var _frames := 0
var _occlusion := false


func _ready() -> void:
	_occlusion = "--occlusion" in OS.get_cmdline_user_args()
	var off := "--off" in OS.get_cmdline_user_args()
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/far_field", not off)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/far_field_distance", 1000.0)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/far_field_occlusion_only", _occlusion)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.08, 0.11, 0.17)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.25, 0.32, 0.45)
	environment.ambient_light_energy = 0.6
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	if _occlusion:
		environment.fog_enabled = true
		environment.fog_light_color = Color(0.8, 0.1, 0.2)
		environment.nextgi_fog_on_reflection_hits = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-48.0, -28.0, 0.0)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	add_child(sun)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 2.0, 0.0)
	camera.fov = 65.0
	camera.far = 2000.0
	camera.current = true
	add_child(camera)
	_make_mesh(PlaneMesh.new(), Transform3D(Basis(), Vector3(0.0, 0.0, -500.0)), Color(0.35, 0.45, 0.65), 0.2, 1.0)
	for index in range(3):
		var distance: float = [300.0, 600.0, 950.0][index]
		var mesh := BoxMesh.new()
		mesh.size = Vector3(12.0, 28.0, 12.0)
		var transform := Transform3D(Basis(), Vector3((index - 1) * 9.0, 14.0, -distance))
		var color: Color = [Color(0.85, 0.3, 0.18), Color(0.25, 0.8, 0.35), Color(0.25, 0.4, 0.95)][index]
		_make_mesh(mesh, transform, color, 0.3, 0.0)


func _make_mesh(p_mesh: Mesh, p_transform: Transform3D, p_color: Color, p_roughness: float, p_metallic: float) -> void:
	if p_mesh is PlaneMesh:
		p_mesh.size = Vector2(80.0, 1100.0)
	var material := StandardMaterial3D.new()
	material.albedo_color = p_color
	material.roughness = p_roughness
	material.metallic = p_metallic
	material.metallic_specular = 0.8
	p_mesh.surface_set_material(0, material)
	var instance := MeshInstance3D.new()
	instance.mesh = p_mesh
	instance.transform = p_transform
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	if instance.has_method("set_gi_far_field"):
		instance.gi_far_field = GeometryInstance3D.FAR_FIELD_ALWAYS
	add_child(instance)


func _finish(p_exit_code: int) -> void:
	set_process(false)
	if "--capture" in OS.get_cmdline_user_args():
		await RenderingServer.frame_post_draw
		var mode := "off" if "--off" in OS.get_cmdline_user_args() else ("occlusion" if _occlusion else "on")
		if "--capture-current" in OS.get_cmdline_user_args():
			mode += "_current"
		var image := get_viewport().get_texture().get_image()
		var path := "user://far_field_%s.png" % mode
		var error := image.save_png(path)
		print("FAR_FIELD_CAPTURE path=%s error=%d" % [OS.get_user_data_dir().path_join("far_field_%s.png" % mode), error])
		if error != OK:
			p_exit_code = 1
	get_tree().quit(p_exit_code)


func _process(_delta: float) -> void:
	_frames += 1
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if "--off" in OS.get_cmdline_user_args():
		if _frames >= 60:
			var disabled := not info.has("far_field_trace_ready") or not bool(info["far_field_trace_ready"])
			print("FAR_FIELD_OFF pass=%s" % disabled)
			_finish(0 if disabled else 1)
		return
	var cells := int(info.get("far_field_cell_count", 0))
	var pending := int(info.get("far_field_pending_count", 0))
	var ready := bool(info.get("far_field_trace_ready", false))
	var captured := int(info.get("src_captured_card_count", 0))
	var card_counts: PackedInt32Array = info.get("src_card_set_card_counts", PackedInt32Array())
	if _frames % 60 == 0:
		print("FAR_FIELD_WAIT frame=%d tier=%s cells=%d pending=%d bytes=%d ready=%s" % [_frames, str(info.get("tier", "")), cells, pending, int(info.get("far_field_proxy_bytes", 0)), ready])
	if cells >= 3 and ready and captured >= cells * 6:
		var valid := not bool(info.get("far_field_budget_exceeded", true)) and int(info.get("far_field_near_cap", 0)) == 100000
		valid = valid and card_counts.size() == cells and captured == cells * 6
		for count in card_counts:
			valid = valid and count == 6
		if _occlusion:
			valid = valid and bool(info.get("far_field_occlusion_only", false))
		print("FAR_FIELD_SUMMARY pass=%s cells=%d pending=%d bytes=%d near_cap=%d tlas_builds=%d" % [valid, cells, pending, int(info.get("far_field_proxy_bytes", 0)), int(info.get("far_field_near_cap", 0)), int(info.get("far_field_tlas_build_count", 0))])
		_finish(0 if valid else 1)
	elif _frames > 600:
		print("FAR_FIELD_FAIL cells=%d pending=%d ready=%s budget=%s" % [cells, pending, ready, info.get("far_field_budget_exceeded", true)])
		_finish(1)
