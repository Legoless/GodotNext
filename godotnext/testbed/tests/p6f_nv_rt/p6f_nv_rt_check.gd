extends Node3D

# GodotNext: p6f_nv_rt (#90). Live GPU validation check for NVIDIA CLAS + PTLAS and 2-state OMM.
# Verifies:
# 1. Default-off state leaves vendor tiers inactive with zero CLAS/OMM allocations.
# 2. Toggling nv_cluster_rt builds real CLAS + Cluster BLAS + PTLAS partitions with 0 validation errors.
# 3. Toggling nv_opacity_micromap admits an alpha-tested mesh with a real 2-state OMM attached to its BLAS.
# 4. Toggling both off cleans up cleanly.

var _frame: int = 0
var _phase: int = 0
var _box_mi: MeshInstance3D = null
var _alpha_mesh_instance: MeshInstance3D = null
var _failed: bool = false
var _dump_off_path: String = ""

func _fail(msg: String) -> void:
	printerr("P6F_NV_RT FAIL: ", msg)
	_failed = true
	get_tree().quit(1)

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for i in range(args.size()):
		if args[i] == "--dump-off" and i + 1 < args.size():
			_dump_off_path = args[i + 1]

	ProjectSettings.set_setting("rendering/global_illumination/nextgi/enabled", true)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/hardware_ray_tracing", 0)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/vendor/nv_cluster_rt", false)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/vendor/nv_opacity_micromap", false)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.15, 0.2, 0.3)
	env.nextgi_enabled = true
	env.sdfgi_enabled = false
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45.0, 35.0, 0.0)
	sun.light_energy = 2.0
	add_child(sun)

	var floor_mi := MeshInstance3D.new()
	var floor_mesh := BoxMesh.new()
	floor_mesh.size = Vector3(12.0, 0.5, 12.0)
	floor_mi.mesh = floor_mesh
	floor_mi.position = Vector3(0.0, -0.25, 0.0)
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.6, 0.6, 0.6)
	floor_mi.material_override = floor_mat
	add_child(floor_mi)

	_box_mi = MeshInstance3D.new()
	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3(2.0, 2.0, 2.0)
	_box_mi.mesh = box_mesh
	_box_mi.position = Vector3(-1.5, 1.0, 0.0)
	var box_mat := StandardMaterial3D.new()
	box_mat.albedo_color = Color(0.85, 0.25, 0.2)
	_box_mi.material_override = box_mat
	add_child(_box_mi)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.5, 6.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, 0.0), Vector3.UP)
	add_child(cam)
	cam.current = true

func _process(_delta: float) -> void:
	if _failed:
		return
	_frame += 1

	var rd := RenderingServer.get_rendering_device()
	var has_clas := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_CLUSTER_ACCELERATION_STRUCTURE)
	var has_ptlas := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_PARTITIONED_ACCELERATION_STRUCTURE)
	var has_omm := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_OPACITY_MICROMAP)

	if _phase == 0 and _frame == 12:
		var info := RenderingServer.nextgi_get_tier_info()
		print("P6F_NV_RT phase0(off): tier=", info.get("tier"), " clas_cap=", has_clas, " ptlas_cap=", has_ptlas, " omm_cap=", has_omm,
			" nv_clas=", info.get("nv_cluster_rt_active"), " nv_omm=", info.get("nv_opacity_micromap_active"),
			" clas_bytes=", info.get("clas_memory_bytes"), " omm_count=", info.get("omm_count"))
		if bool(info.get("nv_cluster_rt_active", true)):
			_fail("nv_cluster_rt_active must be false when flag is off")
			return
		if bool(info.get("nv_opacity_micromap_active", true)):
			_fail("nv_opacity_micromap_active must be false when flag is off")
			return
		if int(info.get("clas_memory_bytes", -1)) != 0:
			_fail("clas_memory_bytes must be 0 when flag is off")
			return
		if int(info.get("omm_count", -1)) != 0:
			_fail("omm_count must be 0 when flag is off")
			return
		if not _dump_off_path.is_empty():
			var img := get_viewport().get_texture().get_image()
			if img == null:
				_fail("Failed to capture viewport image in off phase")
				return
			var err := img.save_png(_dump_off_path)
			if err != OK:
				_fail("Failed to save --dump-off PNG: " + str(err))
				return
			print("P6F_NV_RT dumped off PNG: ", _dump_off_path)
		# Phase 1: enable nv_cluster_rt (CLAS + PTLAS).
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/vendor/nv_cluster_rt", true)
		_phase = 1
		return

	if _phase == 1 and _frame == 23 and _box_mi != null:
		_box_mi.position.x = -1.25
		return

	if _phase == 1 and _frame == 24:
		var info := RenderingServer.nextgi_get_tier_info()
		print("P6F_NV_RT phase1(clas+ptlas): active=", info.get("nv_cluster_rt_active"),
			" reason=", info.get("nv_cluster_rt_reason"),
			" clusters=", info.get("clas_cluster_count"),
			" cblas=", info.get("clas_blas_count"),
			" clas_bytes=", info.get("clas_memory_bytes"),
			" ptlas_parts=", info.get("ptlas_partition_count"),
			" ptlas_updated=", info.get("ptlas_updated_partitions"))
		if has_clas and has_ptlas and String(info.get("tier", "")) == "HWRT":
			if not bool(info.get("nv_cluster_rt_active", false)):
				_fail("Expected nv_cluster_rt_active=true on supporting HWRT GPU")
				return
			if int(info.get("clas_cluster_count", 0)) <= 0:
				_fail("Expected clas_cluster_count > 0")
				return
			if int(info.get("clas_blas_count", 0)) <= 0:
				_fail("Expected clas_blas_count > 0")
				return
			if int(info.get("clas_memory_bytes", 0)) <= 0:
				_fail("Expected clas_memory_bytes > 0")
				return
			if int(info.get("ptlas_partition_count", 0)) <= 0:
				_fail("Expected ptlas_partition_count > 0")
				return
		else:
			if bool(info.get("nv_cluster_rt_active", false)):
				_fail("nv_cluster_rt_active must be false without hardware extensions")
				return
		# Phase 2: turn off CLAS+PTLAS, turn on 2-state OMM, and add an alpha-tested surface.
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/vendor/nv_cluster_rt", false)
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/vendor/nv_opacity_micromap", true)
		var alpha_mesh := ArrayMesh.new()
		var st := SurfaceTool.new()
		st.begin(Mesh.PRIMITIVE_TRIANGLES)
		st.set_uv(Vector2(0.0, 0.0)); st.add_vertex(Vector3(-1.0, 0.0, 0.0))
		st.set_uv(Vector2(1.0, 0.0)); st.add_vertex(Vector3(1.0, 0.0, 0.0))
		st.set_uv(Vector2(1.0, 1.0)); st.add_vertex(Vector3(1.0, 2.0, 0.0))
		st.set_uv(Vector2(0.0, 0.0)); st.add_vertex(Vector3(-1.0, 0.0, 0.0))
		st.set_uv(Vector2(1.0, 1.0)); st.add_vertex(Vector3(1.0, 2.0, 0.0))
		st.set_uv(Vector2(0.0, 1.0)); st.add_vertex(Vector3(-1.0, 2.0, 0.0))
		st.generate_normals()
		var alpha_mat := StandardMaterial3D.new()
		alpha_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		alpha_mat.alpha_scissor_threshold = 0.5
		alpha_mat.albedo_color = Color(0.2, 0.8, 0.25, 1.0)
		alpha_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
		st.set_material(alpha_mat)
		st.commit(alpha_mesh)
		_alpha_mesh_instance = MeshInstance3D.new()
		_alpha_mesh_instance.mesh = alpha_mesh
		_alpha_mesh_instance.position = Vector3(1.5, 0.1, 0.0)
		add_child(_alpha_mesh_instance)
		_phase = 2
		return

	if _phase == 2 and _frame == 38:
		var info := RenderingServer.nextgi_get_tier_info()
		print("P6F_NV_RT phase2(omm): active=", info.get("nv_opacity_micromap_active"),
			" reason=", info.get("nv_opacity_micromap_reason"),
			" omm_count=", info.get("omm_count"),
			" omm_admitted=", info.get("omm_admitted_surfaces"),
			" omm_bytes=", info.get("omm_memory_bytes"),
			" clas_bytes_after_off=", info.get("clas_memory_bytes"))
		if int(info.get("clas_memory_bytes", -1)) != 0:
			_fail("clas_memory_bytes must drop back to 0 after turning nv_cluster_rt off")
			return
		if has_omm and String(info.get("tier", "")) == "HWRT":
			if not bool(info.get("nv_opacity_micromap_active", false)):
				_fail("Expected nv_opacity_micromap_active=true on supporting HWRT GPU")
				return
			if int(info.get("omm_count", 0)) <= 0:
				_fail("Expected omm_count > 0 for alpha-tested mesh")
				return
			if int(info.get("omm_admitted_surfaces", 0)) <= 0:
				_fail("Expected omm_admitted_surfaces > 0")
				return
			if int(info.get("omm_memory_bytes", 0)) <= 0:
				_fail("Expected omm_memory_bytes > 0")
				return
		else:
			if bool(info.get("nv_opacity_micromap_active", false)):
				_fail("nv_opacity_micromap_active must be false without hardware extension")
				return
		# Phase 3: turn both off cleanly.
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/vendor/nv_opacity_micromap", false)
		_phase = 3
		return

	if _phase == 3 and _frame == 44:
		var info := RenderingServer.nextgi_get_tier_info()
		if bool(info.get("nv_cluster_rt_active", true)) or bool(info.get("nv_opacity_micromap_active", true)):
			_fail("Vendor tiers did not turn off cleanly")
			return
		print("P6F_NV_RT PASS")
		get_tree().quit(0)
