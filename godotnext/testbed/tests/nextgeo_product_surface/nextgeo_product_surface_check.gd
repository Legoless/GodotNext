extends Node3D
## NextGeo Nanite parity product surface GPU check (#157, plan §21.4, §21.4.1, N1, N3, GN-14).
##
## Verifies on GPU (Vulkan + VVL SyncVal):
## 1. All 8 new Viewport.DEBUG_DRAW_NEXTGEO_* visualization modes render distinct,
##    finite, non-empty pixels while keeping view_route == "virtual", 0 double_dispatch,
##    and 0 missing:
##    - DEBUG_DRAW_NEXTGEO_RASTER_BINS (fixed gray vs distinct programmable bin hues)
##    - DEBUG_DRAW_NEXTGEO_HW_SW (SW green vs HW red/orange across VS and Mesh paths)
##    - DEBUG_DRAW_NEXTGEO_SHADING_BINS (Compute Bins vs Tile Quads hatching)
##    - DEBUG_DRAW_NEXTGEO_PROGRAMMABLE (fixed gray vs masked/WPO/PDO/displaced colors)
##    - DEBUG_DRAW_NEXTGEO_VERTEX_OFFSET (active WPO orange vs distance-disabled blue vs static gray)
##    - DEBUG_DRAW_NEXTGEO_SKINNED (skeletal green + procedural wind cyan vs rigid gray)
##    - DEBUG_DRAW_NEXTGEO_TESSELLATION (diced-patch heatmap vs non-tessellated gray)
##    - DEBUG_DRAW_NEXTGEO_VOXELS (distant voxel bricks emerald vs triangle clusters gray)
## 2. All required per-frame NextGeo stats keys populate cleanly:
##    sw_clusters, hw_clusters, sw_clusters_phase1/2, hw_clusters_phase1/2,
##    programmable_bins, shading_bins, shading_compute_buckets, vertex_offset_clamped,
##    dicing_overflow, native_rt_rebuilds, voxel_bricks, raster_mode, raster_reason,
##    ray_tracing_mode.
## 3. All 4 new §6.3 PN fallback reasons trigger and report cleanly:
##    - UNSUPPORTED_RASTER_FEATURE (discard + FRAGCOORD)
##    - BLEND_SHAPES (offline ImporterMesh eligibility + runtime attached VGD)
##    - NO_INT64_ATOMICS (view raster_mode=hardware, raster_reason=NO_INT64_ATOMICS)
##    - SW_RASTER_UNAVAILABLE (displaced and voxelized instances under no int64 atomics)
##    Plus DEBUG_DRAW_NEXTGEO_FALLBACK renders all fallback reasons cleanly.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 12

var _saved_settings: Dictionary = {}
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _report: Dictionary = {}

var _camera: Camera3D
var _inst_fixed: MeshInstance3D
var _inst_masked: MeshInstance3D
var _inst_wpo_active: MeshInstance3D
var _inst_wpo_disabled: MeshInstance3D
var _inst_pdo: MeshInstance3D
var _inst_skinned: MeshInstance3D
var _inst_wind: MeshInstance3D
var _inst_displaced: MeshInstance3D
var _inst_voxels: MeshInstance3D
var _inst_unsupported_raster: MeshInstance3D
var _inst_blend_shapes: MeshInstance3D

var _phases: Array[Dictionary] = []
var _phase_idx := 0
var _phase_frame := 0
var _phase_images: Dictionary = {}
var _phase_stats: Dictionary = {}
var _expect_hybrid := false


func _ready() -> void:
	for key in [
		"rendering/nextgeo/visibility_only",
		"rendering/nextgeo/debug_stats",
		"rendering/nextgeo/debug_stats_interval",
		"rendering/nextgeo/raster_mode",
		"rendering/nextgeo/hw_raster_path",
		"rendering/nextgeo/shading_path",
		"rendering/nextgeo/max_pixel_error",
		"rendering/nextgeo/occlusion",
		"rendering/nextgeo/debug_no_int64_atomics",
		"rendering/nextgeo/experimental/tessellation",
		"rendering/nextgeo/experimental/foliage_wind",
		"rendering/nextgeo/experimental/voxels",
	]:
		_saved_settings[key] = ProjectSettings.get_setting(key)

	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 0)
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_no_int64_atomics", false)
	ProjectSettings.set_setting("rendering/nextgeo/experimental/tessellation", true)
	ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", true)
	ProjectSettings.set_setting("rendering/nextgeo/experimental/voxels", true)

	var rd := RenderingServer.get_rendering_device()
	var img_i64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buf_i64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var ind_cnt := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_hybrid = ind_cnt and (img_i64 or buf_i64)

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0

	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_hybrid": _expect_hybrid,
	}

	_build_scene()
	_phases = [
		{"name": "baseline_lit", "mode": Viewport.DEBUG_DRAW_DISABLED, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "raster_bins", "mode": Viewport.DEBUG_DRAW_NEXTGEO_RASTER_BINS, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "hw_sw_vs", "mode": Viewport.DEBUG_DRAW_NEXTGEO_HW_SW, "shading": 1, "hw_path": 1, "no_i64": false, "far_voxels": false},
		{"name": "hw_sw_mesh", "mode": Viewport.DEBUG_DRAW_NEXTGEO_HW_SW, "shading": 1, "hw_path": 2, "no_i64": false, "far_voxels": false},
		{"name": "shading_bins_compute", "mode": Viewport.DEBUG_DRAW_NEXTGEO_SHADING_BINS, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "shading_bins_tiles", "mode": Viewport.DEBUG_DRAW_NEXTGEO_SHADING_BINS, "shading": 2, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "programmable", "mode": Viewport.DEBUG_DRAW_NEXTGEO_PROGRAMMABLE, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "vertex_offset", "mode": Viewport.DEBUG_DRAW_NEXTGEO_VERTEX_OFFSET, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "skinned", "mode": Viewport.DEBUG_DRAW_NEXTGEO_SKINNED, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "tessellation", "mode": Viewport.DEBUG_DRAW_NEXTGEO_TESSELLATION, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "voxels", "mode": Viewport.DEBUG_DRAW_NEXTGEO_VOXELS, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": true},
		{"name": "fallback_hybrid", "mode": Viewport.DEBUG_DRAW_NEXTGEO_FALLBACK, "shading": 1, "hw_path": 0, "no_i64": false, "far_voxels": false},
		{"name": "fallback_no_int64", "mode": Viewport.DEBUG_DRAW_NEXTGEO_FALLBACK, "shading": 1, "hw_path": 0, "no_i64": true, "far_voxels": false},
	]
	_phase_idx = 0
	_apply_phase()


func _alloc_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := OS.get_user_data_dir().path_join("ng_prod_%d_%s_%d.ngm" % [OS.get_process_id(), p_tag, _ngm_counter])
	_temp_files.append(path)
	return path


func _make_micro_grid_mesh(p_size: float, p_subdiv: int, p_mat: Material) -> ArrayMesh:
	# High-frequency micro-detail surface whose triangles project < 8 px so hybrid cluster cull
	# routes its clusters to the SW compute rasterizer at default max_pixel_error = 1.0.
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var uvs := PackedVector2Array()
	var idx := PackedInt32Array()
	var stride := p_subdiv + 1
	for iy in stride:
		var v := float(iy) / float(p_subdiv)
		for ix in stride:
			var u := float(ix) / float(p_subdiv)
			var x := (u - 0.5) * p_size
			var y := (v - 0.5) * p_size
			var z := 0.008 * sin(u * PI * 24.0) * cos(v * PI * 24.0)
			verts.push_back(Vector3(x, y, z))
			norms.push_back(Vector3(0.0, 0.0, 1.0))
			uvs.push_back(Vector2(u, v))
	for iy in p_subdiv:
		for ix in p_subdiv:
			var v00 := iy * stride + ix
			var v10 := v00 + 1
			var v01 := v00 + stride
			var v11 := v01 + 1
			idx.append_array([v00, v10, v11, v00, v11, v01])
	var arr := []
	arr.resize(Mesh.ARRAY_MAX)
	arr[Mesh.ARRAY_VERTEX] = verts
	arr[Mesh.ARRAY_NORMAL] = norms
	arr[Mesh.ARRAY_TEX_UV] = uvs
	arr[Mesh.ARRAY_INDEX] = idx
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	am.surface_set_material(0, p_mat)
	return am


func _make_sphere_mesh(p_radius: float, p_rings: int, p_segs: int, p_mat: Material) -> ArrayMesh:
	var sm := SphereMesh.new()
	sm.radius = p_radius
	sm.height = p_radius * 2.0
	sm.rings = p_rings
	sm.radial_segments = p_segs
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, sm.get_mesh_arrays())
	if p_mat != null:
		am.surface_set_material(0, p_mat)
	return am


func _make_skinned_sphere_mesh(p_radius: float, p_mat: Material) -> ArrayMesh:
	var base := _make_sphere_mesh(p_radius, 18, 24, p_mat)
	var arr := base.surface_get_arrays(0)
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	bones.resize(verts.size() * 4)
	weights.resize(verts.size() * 4)
	for i in verts.size():
		bones[i * 4] = 0
		weights[i * 4] = 1.0
	arr[Mesh.ARRAY_BONES] = bones
	arr[Mesh.ARRAY_WEIGHTS] = weights
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	am.surface_set_material(0, p_mat)
	return am


func _make_foliage_mesh(p_mat: Material) -> ArrayMesh:
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var uvs := PackedVector2Array()
	var idx := PackedInt32Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 1572026
	for i in 320:
		var theta := rng.randf() * TAU
		var phi := acos(rng.randf_range(-0.75, 0.95))
		var rad := rng.randf_range(0.55, 1.45)
		var center := Vector3(rad * sin(phi) * cos(theta), 1.8 + rad * cos(phi) * 0.85, rad * sin(phi) * sin(theta))
		var nrm := (center - Vector3(0.0, 1.7, 0.0)).normalized()
		var up_ref := Vector3.UP if absf(nrm.y) < 0.9 else Vector3.RIGHT
		var t_axis := up_ref.cross(nrm).normalized()
		var b_axis := nrm.cross(t_axis).normalized()
		var half := rng.randf_range(0.14, 0.22)
		var base_v := verts.size()
		for iy in 3:
			for ix in 3:
				var u := float(ix) * 0.5
				var v := float(iy) * 0.5
				verts.push_back(center + t_axis * ((u - 0.5) * 2.0 * half) + b_axis * ((v - 0.5) * 2.0 * half))
				norms.push_back(nrm)
				uvs.push_back(Vector2(u, v))
		for iy in 2:
			for ix in 2:
				var v00 := base_v + iy * 3 + ix
				var v10 := v00 + 1
				var v01 := v00 + 3
				var v11 := v01 + 1
				idx.append_array([v00, v10, v11, v00, v11, v01])
	var arr := []
	arr.resize(Mesh.ARRAY_MAX)
	arr[Mesh.ARRAY_VERTEX] = verts
	arr[Mesh.ARRAY_NORMAL] = norms
	arr[Mesh.ARRAY_TEX_UV] = uvs
	arr[Mesh.ARRAY_INDEX] = idx
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	am.surface_set_material(0, p_mat)
	return am


func _virtualize(p_mesh: ArrayMesh, p_tag: String, p_skinned: bool = false, p_wind: bool = false, p_voxels: bool = false) -> ArrayMesh:
	var orig_mat := p_mesh.surface_get_material(0)
	var imp := ImporterMesh.from_mesh(p_mesh)
	if orig_mat is BaseMaterial3D:
		imp.set_surface_material(0, null)
	var err := imp.build_virtual_geometry(_alloc_ngm(p_tag), p_skinned, false, p_wind, p_voxels)
	if err != OK:
		_failures.append("build_virtual_geometry(%s) failed: %d" % [p_tag, err])
		return p_mesh
	var vmesh := imp.get_mesh()
	if vmesh != null and orig_mat != null:
		vmesh.surface_set_material(0, orig_mat)
	return vmesh


func _shader_mat(p_code: String) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = p_code
	var sm := ShaderMaterial.new()
	sm.shader = sh
	return sm


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.06, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.36, 0.40)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40.0, 28.0, 0.0)
	sun.light_energy = 1.6
	add_child(sun)

	var world := get_world_3d()
	if world != null:
		world.wind_direction = Vector3(1.0, 0.0, 0.0)
		world.wind_strength = 0.6
		world.wind_gust_frequency = 1.2

	# 1. Fixed-function rigid micro-detail virtual mesh (small triangles -> SW compute raster).
	var mat_fixed := StandardMaterial3D.new()
	mat_fixed.albedo_color = Color(0.60, 0.62, 0.66)
	_inst_fixed = MeshInstance3D.new()
	_inst_fixed.name = "FixedRigid"
	_inst_fixed.mesh = _virtualize(_make_micro_grid_mesh(1.3, 64, mat_fixed), "fixed")
	_inst_fixed.position = Vector3(-3.2, 0.9, 0.0)
	add_child(_inst_fixed)

	# 2. Masked (alpha scissor) virtual mesh.
	var mat_masked := _shader_mat(
		"shader_type spatial;\n" +
		"void fragment() {\n" +
		"\tif (sin(UV.x * 24.0) * sin(UV.y * 24.0) < -0.55) { discard; }\n" +
		"\tALBEDO = vec3(0.25, 0.78, 0.42);\n" +
		"}\n"
	)
	_inst_masked = MeshInstance3D.new()
	_inst_masked.name = "Masked"
	_inst_masked.mesh = _virtualize(_make_sphere_mesh(0.68, 20, 28, mat_masked), "masked")
	_inst_masked.position = Vector3(-1.6, 0.9, 0.0)
	add_child(_inst_masked)

	# 3. Active WPO mesh with small vertex_offset_max so vertex_offset_clamped > 0 is exercised!
	var mat_wpo := _shader_mat(
		"shader_type spatial;\n" +
		"void vertex() {\n" +
		"\tVERTEX.y += 0.06;\n" +
		"}\n" +
		"void fragment() {\n" +
		"\tALBEDO = vec3(0.90, 0.55, 0.18);\n" +
		"}\n"
	)
	mat_wpo.vertex_offset_max = 0.02
	_inst_wpo_active = MeshInstance3D.new()
	_inst_wpo_active.name = "WpoActive"
	_inst_wpo_active.mesh = _virtualize(_make_sphere_mesh(0.68, 20, 28, mat_wpo), "wpo_active")
	_inst_wpo_active.position = Vector3(0.0, 0.9, 0.0)
	add_child(_inst_wpo_active)

	# 4. Distance-disabled WPO mesh (vertex_offset_disable_distance = 0.1m < camera distance).
	var mat_wpo_dis := mat_wpo.duplicate() as ShaderMaterial
	_inst_wpo_disabled = MeshInstance3D.new()
	_inst_wpo_disabled.name = "WpoDisabled"
	_inst_wpo_disabled.mesh = _virtualize(_make_sphere_mesh(0.68, 20, 28, mat_wpo_dis), "wpo_disabled")
	_inst_wpo_disabled.vertex_offset_disable_distance = 0.1
	_inst_wpo_disabled.position = Vector3(1.6, 0.9, 0.0)
	add_child(_inst_wpo_disabled)

	# 5. Pixel Depth Offset (DEPTH_OFFSET) virtual mesh.
	var mat_pdo := _shader_mat(
		"shader_type spatial;\n" +
		"void fragment() {\n" +
		"\tDEPTH_OFFSET = 0.02 * (0.5 + 0.5 * sin(UV.x * 16.0));\n" +
		"\tALBEDO = vec3(0.75, 0.35, 0.85);\n" +
		"}\n"
	)
	_inst_pdo = MeshInstance3D.new()
	_inst_pdo.name = "PixelDepthOffset"
	_inst_pdo.mesh = _virtualize(_make_sphere_mesh(0.68, 20, 28, mat_pdo), "pdo")
	_inst_pdo.position = Vector3(3.2, 0.9, 0.0)
	add_child(_inst_pdo)

	# 6. Skeletally skinned Nanite mesh.
	var skel := Skeleton3D.new()
	skel.name = "SkelRig"
	skel.add_bone("root")
	skel.set_bone_rest(0, Transform3D.IDENTITY)
	add_child(skel)
	var mat_skin := StandardMaterial3D.new()
	mat_skin.albedo_color = Color(0.28, 0.72, 0.52)
	_inst_skinned = MeshInstance3D.new()
	_inst_skinned.name = "SkinnedNanite"
	_inst_skinned.mesh = _virtualize(_make_skinned_sphere_mesh(0.68, mat_skin), "skinned", true, false, false)
	var skin := Skin.new()
	skin.add_named_bind("root", Transform3D.IDENTITY)
	_inst_skinned.skin = skin
	_inst_skinned.skeleton = NodePath("..")
	_inst_skinned.position = Vector3(-3.2, -0.9, 0.0)
	skel.add_child(_inst_skinned)

	# 7. Procedural foliage-wind Nanite mesh.
	var mat_wind := StandardMaterial3D.new()
	mat_wind.albedo_color = Color(0.22, 0.68, 0.38)
	_inst_wind = MeshInstance3D.new()
	_inst_wind.name = "FoliageWindNanite"
	_inst_wind.mesh = _virtualize(_make_sphere_mesh(0.68, 20, 28, mat_wind), "wind", false, true, false)
	_inst_wind.position = Vector3(-1.6, -0.9, 0.0)
	add_child(_inst_wind)

	# 8. Displaced / tessellated Nanite mesh.
	var mat_disp := _shader_mat(
		"shader_type spatial;\n" +
		"void displacement() {\n" +
		"\tDISPLACEMENT = 0.5 + 0.4 * sin(UV.x * 12.0) * cos(UV.y * 12.0);\n" +
		"}\n" +
		"void fragment() {\n" +
		"\tALBEDO = vec3(0.82, 0.58, 0.25);\n" +
		"}\n"
	)
	mat_disp.displacement_magnitude = 0.14
	_inst_displaced = MeshInstance3D.new()
	_inst_displaced.name = "DisplacedNanite"
	_inst_displaced.mesh = _virtualize(_make_sphere_mesh(0.70, 14, 20, mat_disp), "displaced")
	_inst_displaced.position = Vector3(0.0, -0.9, 0.0)
	add_child(_inst_displaced)

	# 9. Fallback: UNSUPPORTED_RASTER_FEATURE (discard + FRAGCOORD).
	var mat_unsup := _shader_mat(
		"shader_type spatial;\n" +
		"void fragment() {\n" +
		"\tif (mod(FRAGCOORD.x, 4.0) < 0.25) { discard; }\n" +
		"\tALBEDO = vec3(0.85, 0.22, 0.38);\n" +
		"}\n"
	)
	_inst_unsupported_raster = MeshInstance3D.new()
	_inst_unsupported_raster.name = "FallbackUnsupportedRaster"
	_inst_unsupported_raster.mesh = _virtualize(_make_sphere_mesh(0.65, 16, 24, mat_unsup), "unsup_raster")
	_inst_unsupported_raster.position = Vector3(1.6, -0.9, 0.0)
	add_child(_inst_unsupported_raster)

	# 10. Fallback: BLEND_SHAPES (verify both offline ImporterMesh eligibility and runtime route).
	var base_bs := _make_sphere_mesh(0.65, 16, 24, mat_fixed)
	var bs_src := base_bs.surface_get_arrays(0)
	var bs_verts: PackedVector3Array = bs_src[Mesh.ARRAY_VERTEX]
	var morphed_verts := PackedVector3Array()
	morphed_verts.resize(bs_verts.size())
	for i in bs_verts.size():
		morphed_verts[i] = bs_verts[i] * 1.08
	var bs_delta := []
	bs_delta.resize(Mesh.ARRAY_MAX)
	bs_delta[Mesh.ARRAY_VERTEX] = morphed_verts
	bs_delta[Mesh.ARRAY_NORMAL] = bs_src[Mesh.ARRAY_NORMAL]
	bs_delta[Mesh.ARRAY_TANGENT] = bs_src[Mesh.ARRAY_TANGENT]
	var bs_mesh := ArrayMesh.new()
	bs_mesh.add_blend_shape("inflate")
	bs_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, bs_src, [bs_delta])
	bs_mesh.surface_set_material(0, mat_fixed)
	var bs_imp := ImporterMesh.from_mesh(bs_mesh)
	var bs_build_err := bs_imp.build_virtual_geometry(_alloc_ngm("blend_shapes_rejected"), true)
	var bs_built_mesh := bs_imp.get_mesh()
	var bs_rejected_offline := (bs_build_err == OK) and (bs_built_mesh == null or bs_built_mesh.get_virtual_geometry() == null)
	_report["blend_shapes_rejected_offline"] = bs_rejected_offline
	if not bs_rejected_offline:
		_failures.append("Expected offline ImporterMesh.build_virtual_geometry() to reject blend-shape mesh cleanly")
	# Attach VGD from unmorphed copy so runtime routing also emits legacy / BLEND_SHAPES.
	var unmorphed_vmesh := _virtualize(base_bs, "blend_shapes_vgd")
	bs_mesh.set_virtual_geometry(unmorphed_vmesh.get_virtual_geometry())
	_inst_blend_shapes = MeshInstance3D.new()
	_inst_blend_shapes.name = "FallbackBlendShapes"
	_inst_blend_shapes.mesh = bs_mesh
	_inst_blend_shapes.position = Vector3(3.2, -0.9, 0.0)
	add_child(_inst_blend_shapes)

	# 11. Distant voxelized foliage mesh (placed far on -Z for the VOXELS phase and SW_RASTER_UNAVAILABLE check).
	var mat_vox := StandardMaterial3D.new()
	mat_vox.albedo_color = Color(0.20, 0.65, 0.25)
	_inst_voxels = MeshInstance3D.new()
	_inst_voxels.name = "VoxelsFoliage"
	_inst_voxels.mesh = _virtualize(_make_foliage_mesh(mat_vox), "voxels", false, false, true)
	_inst_voxels.position = Vector3(0.0, -1.6, -110.0)
	add_child(_inst_voxels)

	_camera = Camera3D.new()
	_camera.fov = 50.0
	_camera.near = 0.2
	_camera.far = 300.0
	add_child(_camera)
	_camera.make_current()


func _apply_phase() -> void:
	var p := _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", int(p["shading"]))
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(p["hw_path"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_no_int64_atomics", bool(p["no_i64"]))
	get_viewport().debug_draw = int(p["mode"])

	if bool(p["far_voxels"]):
		ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
		# Frame the distant voxelized canopy (~110m away) alongside the close grid in the foreground.
		_camera.look_at_from_position(Vector3(0.0, 2.2, 6.2), Vector3(0.0, 0.0, -110.0), Vector3.UP)
	else:
		ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.25)
		_camera.look_at_from_position(Vector3(0.0, 0.1, 6.8), Vector3(0.0, 0.0, 0.0), Vector3.UP)
	_phase_frame = 0


func _process(_delta: float) -> void:
	_phase_frame += 1
	var wait_frames := 24 if _phase_idx == 0 else SETTLE_FRAMES
	if _phase_frame < wait_frames:
		return
	var p := _phases[_phase_idx]
	var pname := String(p["name"])
	var vp := get_viewport()
	var img := vp.get_texture().get_image()
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(vp.get_viewport_rid()).duplicate(true)
	_phase_images[pname] = img
	_phase_stats[pname] = st

	_check_phase(p, img, st)

	_phase_idx += 1
	if _phase_idx < _phases.size():
		_apply_phase()
	else:
		_finish()


func _analyze_image(p_img: Image) -> Dictionary:
	if p_img == null or p_img.is_empty():
		return {"non_finite": 1, "non_bg": 0, "colored": 0, "green": 0, "red_orange": 0, "orange": 0, "blue": 0, "cyan": 0, "emerald": 0, "warm": 0}
	var w := p_img.get_width()
	var h := p_img.get_height()
	var non_finite := 0
	var non_bg := 0
	var colored := 0
	var green := 0
	var red_orange := 0
	var orange := 0
	var blue := 0
	var cyan := 0
	var emerald := 0
	var warm := 0
	for y in range(0, h, 2):
		for x in range(0, w, 2):
			var c := p_img.get_pixel(x, y)
			if not is_finite(c.r) or not is_finite(c.g) or not is_finite(c.b):
				non_finite += 1
				continue
			if c.r + c.g + c.b > 0.12:
				non_bg += 1
			var sat := maxf(c.r, maxf(c.g, c.b)) - minf(c.r, minf(c.g, c.b))
			if sat > 0.18:
				colored += 1
			if c.g > 0.55 and c.g > c.r + 0.25 and c.g > c.b + 0.15:
				green += 1
			if c.r > 0.65 and c.r > c.b + 0.30:
				red_orange += 1
			if c.r > 0.70 and c.g > 0.28 and c.g < 0.75 and c.b < 0.30:
				orange += 1
			if c.b > 0.65 and c.b > c.r + 0.25:
				blue += 1
			if c.g > 0.60 and c.b > 0.65 and c.r < 0.40:
				cyan += 1
			if c.g > 0.65 and c.b > 0.35 and c.r < 0.45:
				emerald += 1
			if (c.r > 0.55 or c.g > 0.55) and sat > 0.25:
				warm += 1
	return {
		"non_finite": non_finite,
		"non_bg": non_bg,
		"colored": colored,
		"green": green,
		"red_orange": red_orange,
		"orange": orange,
		"blue": blue,
		"cyan": cyan,
		"emerald": emerald,
		"warm": warm,
	}


func _images_differ(p_a: Image, p_b: Image) -> int:
	if p_a == null or p_b == null:
		return 0
	var w := mini(p_a.get_width(), p_b.get_width())
	var h := mini(p_a.get_height(), p_b.get_height())
	var diff_px := 0
	for y in range(0, h, 4):
		for x in range(0, w, 4):
			var ca := p_a.get_pixel(x, y)
			var cb := p_b.get_pixel(x, y)
			if maxf(absf(ca.r - cb.r), maxf(absf(ca.g - cb.g), absf(ca.b - cb.b))) > 0.05:
				diff_px += 1
	return diff_px


func _assert_instance_route(p_inst: VisualInstance3D, p_exp_route: String, p_exp_reason: String, p_ctx: String) -> void:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_inst.get_instance())
	var surfaces: Array = st.get("surfaces", [])
	var got_route := String(st.get("route", ""))
	var got_reason := String(st.get("reason", ""))
	if not surfaces.is_empty():
		var s0: Dictionary = surfaces[0]
		got_route = String(s0.get("route", ""))
		got_reason = String(s0.get("reason", ""))
	if got_route != p_exp_route or got_reason != p_exp_reason:
		_failures.append("%s: expected %s/%s, got %s/%s (%s)" % [
			p_ctx, p_exp_route, p_exp_reason, got_route, got_reason, JSON.stringify(st)])


func _check_phase(p_phase: Dictionary, p_img: Image, p_stats: Dictionary) -> void:
	var pname := String(p_phase["name"])
	var px := _analyze_image(p_img)

	if int(px["non_finite"]) > 0:
		_failures.append("%s: found %d non-finite pixels" % [pname, int(px["non_finite"])])
	if int(px["non_bg"]) < 200:
		_failures.append("%s: rendered too few non-background pixels (%d)" % [pname, int(px["non_bg"])])

	if String(p_stats.get("view_route", "")) != "virtual" or String(p_stats.get("view_reason", "")) != "NONE":
		_failures.append("%s: expected view_route=virtual/NONE, got %s/%s" % [
			pname, String(p_stats.get("view_route", "")), String(p_stats.get("view_reason", ""))])
	if int(p_stats.get("double_dispatch", -1)) != 0 or int(p_stats.get("missing", -1)) != 0:
		_failures.append("%s: double_dispatch=%s missing=%s" % [
			pname, str(p_stats.get("double_dispatch")), str(p_stats.get("missing"))])

	# Verify required per-frame stat keys exist cleanly in every phase.
	for req_key in [
		"sw_clusters", "hw_clusters", "sw_clusters_phase1", "sw_clusters_phase2",
		"hw_clusters_phase1", "hw_clusters_phase2", "programmable_bins", "shading_bins",
		"shading_compute_buckets", "vertex_offset_clamped", "dicing_overflow",
		"native_rt_rebuilds", "voxel_bricks", "raster_mode", "raster_reason", "ray_tracing_mode",
	]:
		if not p_stats.has(req_key):
			_failures.append("%s: missing required debug stat key '%s'" % [pname, req_key])

	var summary := {
		"non_bg": px["non_bg"],
		"colored": px["colored"],
		"raster_mode": p_stats.get("raster_mode", ""),
		"raster_reason": p_stats.get("raster_reason", ""),
		"sw_clusters": int(p_stats.get("sw_clusters", 0)),
		"hw_clusters": int(p_stats.get("hw_clusters", 0)),
		"programmable_bins": int(p_stats.get("programmable_bins", 0)),
		"shading_bins": int(p_stats.get("shading_bins", 0)),
		"vertex_offset_clamped": int(p_stats.get("vertex_offset_clamped", 0)),
		"dicing_overflow": int(p_stats.get("dicing_overflow", 0)),
		"native_rt_rebuilds": int(p_stats.get("native_rt_rebuilds", 0)),
		"voxel_bricks": int(p_stats.get("voxel_bricks", 0)),
	}

	if _expect_hybrid and not bool(p_phase["no_i64"]):
		match pname:
			"baseline_lit":
				if int(p_stats.get("programmable_bins", 0)) < 3:
					_failures.append("baseline_lit: expected >= 3 programmable_bins, got %d" % int(p_stats.get("programmable_bins", 0)))
				if int(p_stats.get("shading_bins", 0)) < 1:
					_failures.append("baseline_lit: expected shading_bins >= 1, got %d" % int(p_stats.get("shading_bins", 0)))
				if int(p_stats.get("vertex_offset_clamped", 0)) <= 0:
					_failures.append("baseline_lit: expected vertex_offset_clamped > 0, got %d" % int(p_stats.get("vertex_offset_clamped", 0)))
				if int(p_stats.get("sw_clusters", 0)) <= 0 or int(p_stats.get("hw_clusters", 0)) <= 0:
					_failures.append("baseline_lit: expected both sw_clusters (%d) > 0 and hw_clusters (%d) > 0" % [int(p_stats.get("sw_clusters", 0)), int(p_stats.get("hw_clusters", 0))])
				_assert_instance_route(_inst_unsupported_raster, "legacy", "UNSUPPORTED_RASTER_FEATURE", "baseline/unsupported_raster")
				_assert_instance_route(_inst_blend_shapes, "legacy", "BLEND_SHAPES", "baseline/blend_shapes")
				_assert_instance_route(_inst_displaced, "virtual", "NONE", "baseline/displaced_virtual")
			"raster_bins":
				if int(px["colored"]) < 100:
					_failures.append("raster_bins: expected distinct colored programmable bin pixels, got %d" % int(px["colored"]))
			"hw_sw_vs", "hw_sw_mesh":
				if int(px["green"]) < 30 or int(px["red_orange"]) < 30:
					_failures.append("%s: expected BOTH SW green (%d) >= 30 and HW red/orange (%d) >= 30 pixels" % [pname, int(px["green"]), int(px["red_orange"])])
			"shading_bins_compute", "shading_bins_tiles":
				if int(px["colored"]) < 100:
					_failures.append("%s: expected colored shading-bin pixels, got %d" % [pname, int(px["colored"])])
			"programmable":
				if int(px["colored"]) < 100:
					_failures.append("programmable: expected highlighted programmable pixels, got %d" % int(px["colored"]))
			"vertex_offset":
				if int(px["orange"]) < 30 or int(px["blue"]) < 30:
					_failures.append("vertex_offset: expected both active WPO (orange=%d) and distance-disabled WPO (blue=%d) pixels" % [int(px["orange"]), int(px["blue"])])
			"skinned":
				if int(px["green"]) < 30 or int(px["cyan"]) < 30:
					_failures.append("skinned: expected both skeletal (green=%d) and wind-skinned (cyan=%d) pixels" % [int(px["green"]), int(px["cyan"])])
			"tessellation":
				if int(px["warm"]) < 30:
					_failures.append("tessellation: expected highlighted diced-patch pixels, got warm=%d" % int(px["warm"]))
			"voxels":
				if int(p_stats.get("voxel_bricks", 0)) <= 0:
					_failures.append("voxels: expected voxel_bricks > 0 in distant view, got %d" % int(p_stats.get("voxel_bricks", 0)))
				if int(px["emerald"]) < 5:
					_failures.append("voxels: expected highlighted emerald voxel pixels, got %d" % int(px["emerald"]))
			"fallback_hybrid":
				if int(px["colored"]) < 100:
					_failures.append("fallback_hybrid: expected colored virtual/fallback pixels, got %d" % int(px["colored"]))

	if pname == "fallback_no_int64":
		if String(p_stats.get("raster_mode", "")) != "hardware" or String(p_stats.get("raster_reason", "")) != "NO_INT64_ATOMICS":
			_failures.append("fallback_no_int64: expected raster_mode=hardware, raster_reason=NO_INT64_ATOMICS; got %s / %s" % [
				String(p_stats.get("raster_mode", "")), String(p_stats.get("raster_reason", ""))])
		_assert_instance_route(_inst_displaced, "legacy", "SW_RASTER_UNAVAILABLE", "no_int64/displaced")
		_assert_instance_route(_inst_voxels, "legacy", "SW_RASTER_UNAVAILABLE", "no_int64/voxels")
		_assert_instance_route(_inst_unsupported_raster, "legacy", "UNSUPPORTED_RASTER_FEATURE", "no_int64/unsupported_raster")
		_assert_instance_route(_inst_blend_shapes, "legacy", "BLEND_SHAPES", "no_int64/blend_shapes")

	if not _report.has("phases"):
		_report["phases"] = {}
	(_report["phases"] as Dictionary)[pname] = summary


func _finish() -> void:
	set_process(false)
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED

	# Verify visualization modes produce distinct outputs from each other and from baseline.
	if _expect_hybrid:
		var pairs := [
			["baseline_lit", "raster_bins"],
			["raster_bins", "hw_sw_vs"],
			["hw_sw_vs", "shading_bins_compute"],
			["shading_bins_compute", "shading_bins_tiles"],
			["programmable", "vertex_offset"],
			["vertex_offset", "skinned"],
			["skinned", "tessellation"],
			["fallback_hybrid", "fallback_no_int64"],
		]
		var pair_diffs := {}
		for pair in pairs:
			var a := String(pair[0])
			var b := String(pair[1])
			var d := _images_differ(_phase_images.get(a), _phase_images.get(b))
			pair_diffs["%s_vs_%s" % [a, b]] = d
			if d < 20:
				_failures.append("Visualization modes %s and %s did not produce distinct outputs (diff_samples=%d)" % [a, b, d])
		_report["pair_diffs"] = pair_diffs

	for key in _saved_settings:
		ProjectSettings.set_setting(key, _saved_settings[key])
	for p in _temp_files:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(p)
	_temp_files.clear()

	_report["passed"] = _failures.is_empty()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
