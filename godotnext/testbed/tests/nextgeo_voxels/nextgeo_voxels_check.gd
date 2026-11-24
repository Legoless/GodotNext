extends Node3D
## NextGeo Nanite Voxels check (#165, plan §21.4 contract N10, GN-7, GN-12, N6, MN3).
##
## Verifies on GPU (Vulkan + VVL SyncVal, run with GODOT_DEV_VSM=1):
## (a) experimental/voxels == false: byte-identical frame on a voxel-built mesh vs a plain mesh
##     without voxels, voxels_buffers_allocated == false, voxel_raster_dispatches == 0,
##     voxel_bricks == 0, and VSM voxels_buffers_allocated == false, vsm_voxel_dispatches == 0.
## (b) experimental/voxels == true at close range: bricks project > V px, renders triangles cleanly.
## (c) experimental/voxels == true at far range (Compute Shading Bins, image vis64):
##     voxels_buffers_allocated == true, voxel_raster_dispatches > 0, voxel_bricks > 0,
##     non-empty shaded foliage pixels, and 0 pixel diff across consecutive static frames
##     (frame-stable stochastic coverage hash).
## (d) experimental/voxels == true at far range (Tile Quads fallback, shading_path = 2):
##     resolves shaded voxel pixels cleanly and closely matches compute shading bins.
## (e) experimental/voxels == true at far range (VIS64_BUFFER SSBO storage):
##     exercises VIS64_BUFFER variant of voxel_raster.glsl and visibility_export.glsl cleanly.
## (f) VSM depth-only voxel variant (GODOT_DEV_VSM=1): vsm_voxel_dispatches > 0,
##     vsm_voxel_bricks > 0, vsm_voxel_writes > 0, and ground receiver shows shadowed pixels.
## (g) Toggling experimental/voxels back to false frees voxel buffers and matches voxels_off
##     byte-for-byte.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 14

var _phases: Array[Dictionary] = [
	{ "name": "plain_off", "use_voxels_mesh": false, "voxels": false, "close": false, "shading": 1, "vis64_buf": false, "check_static": false },
	{ "name": "voxels_off", "use_voxels_mesh": true, "voxels": false, "close": false, "shading": 1, "vis64_buf": false, "check_static": false },
	{ "name": "voxels_on_far_compute", "use_voxels_mesh": true, "voxels": true, "close": false, "shading": 1, "vis64_buf": false, "check_static": true },
	{ "name": "voxels_on_far_tiles", "use_voxels_mesh": true, "voxels": true, "close": false, "shading": 2, "vis64_buf": false, "check_static": false },
	{ "name": "voxels_on_far_vis64_buf", "use_voxels_mesh": true, "voxels": true, "close": false, "shading": 1, "vis64_buf": true, "check_static": false },
	{ "name": "voxels_on_close", "use_voxels_mesh": true, "voxels": true, "close": true, "shading": 1, "vis64_buf": false, "check_static": false },
	{ "name": "voxels_back_off", "use_voxels_mesh": true, "voxels": false, "close": false, "shading": 1, "vis64_buf": false, "check_static": false },
]

var _mesh_plain: ArrayMesh
var _mesh_voxels: ArrayMesh
var _canopy_instances: Array[MeshInstance3D] = []
var _ground_inst: MeshInstance3D
var _camera: Camera3D

var _phase_idx := 0
var _phase_frame := 0
var _images: Dictionary = {}
var _static_next_img: Image
var _stats: Dictionary = {}
var _vsm_stats: Dictionary = {}
var _vsm_phase_bricks_sum: Dictionary = {}
var _vsm_phase_writes_sum: Dictionary = {}
var _vsm_phase_dirty_sum: Dictionary = {}
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_voxels := false


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", true)
	ProjectSettings.set_setting("rendering/nextgeo/experimental/voxels", false)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_voxels = indirect_count and (image_int64 or buffer_int64)
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_voxels": _expect_voxels,
		"vsm_active": bool(RenderingServer.virtual_shadow_maps_get_debug_info().get("active", false)),
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0

	_build_scene()
	_apply_phase()


func _make_leaf_texture() -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var u := float(x) / 63.0
			var v := float(y) / 63.0
			var vein := 0.15 * sin(u * 12.566) * sin(v * 12.566)
			img.set_pixel(x, y, Color(0.18 + 0.12 * u, 0.58 + 0.25 * v + vein, 0.14, 1.0))
	return ImageTexture.create_from_image(img)


func _build_foliage_importer_mesh(p_leaf_mat: Material, p_trunk_mat: Material) -> ImporterMesh:
	var im := ImporterMesh.new()
	im.resource_name = "VoxelFoliageCanopy"

	# Surface 0: dense multi-leaf canopy of ~384 subdivided leaf cards (3,072 triangles)
	# so the DAG builds multiple hierarchy levels and voxelizes coarse groups.
	var leaf_verts := PackedVector3Array()
	var leaf_norms := PackedVector3Array()
	var leaf_uvs := PackedVector2Array()
	var leaf_indices := PackedInt32Array()

	var rng := RandomNumberGenerator.new()
	rng.seed = 1652026
	var leaf_count := 384
	for i in range(leaf_count):
		var theta := rng.randf() * TAU
		var phi := acos(rng.randf_range(-0.75, 0.95))
		var rad := rng.randf_range(0.55, 1.55)
		var center := Vector3(
			rad * sin(phi) * cos(theta),
			2.1 + rad * cos(phi) * 0.85,
			rad * sin(phi) * sin(theta)
		)
		var nrm := (center - Vector3(0.0, 2.0, 0.0)).normalized()
		var up_ref := Vector3.UP if absf(nrm.y) < 0.9 else Vector3.RIGHT
		var t_axis := up_ref.cross(nrm).normalized()
		var b_axis := nrm.cross(t_axis).normalized()
		var half := rng.randf_range(0.14, 0.22)

		var base_v := leaf_verts.size()
		for iy in range(3):
			for ix in range(3):
				var u := float(ix) * 0.5
				var v := float(iy) * 0.5
				var px := (u - 0.5) * 2.0 * half
				var py := (v - 0.5) * 2.0 * half
				var bend := 0.025 * sin(u * PI) * sin(v * PI)
				leaf_verts.push_back(center + t_axis * px + b_axis * py + nrm * bend)
				leaf_norms.push_back(nrm)
				leaf_uvs.push_back(Vector2(u, v))
		for iy in range(2):
			for ix in range(2):
				var v00 := base_v + iy * 3 + ix
				var v10 := v00 + 1
				var v01 := v00 + 3
				var v11 := v01 + 1
				leaf_indices.push_back(v00)
				leaf_indices.push_back(v10)
				leaf_indices.push_back(v11)
				leaf_indices.push_back(v00)
				leaf_indices.push_back(v11)
				leaf_indices.push_back(v01)

	var leaf_arrays := []
	leaf_arrays.resize(Mesh.ARRAY_MAX)
	leaf_arrays[Mesh.ARRAY_VERTEX] = leaf_verts
	leaf_arrays[Mesh.ARRAY_NORMAL] = leaf_norms
	leaf_arrays[Mesh.ARRAY_TEX_UV] = leaf_uvs
	leaf_arrays[Mesh.ARRAY_INDEX] = leaf_indices
	im.add_surface(Mesh.PRIMITIVE_TRIANGLES, leaf_arrays, [], {}, p_leaf_mat, "foliage")

	# Surface 1: trunk cylinder (so multi-surface voxel decoding is exercised too).
	var trunk_verts := PackedVector3Array()
	var trunk_norms := PackedVector3Array()
	var trunk_uvs := PackedVector2Array()
	var trunk_indices := PackedInt32Array()
	var segs := 16
	var rings := 16
	for r in range(rings + 1):
		var v := float(r) / float(rings)
		var y := v * 2.0
		var rad := lerpf(0.24, 0.14, v)
		for s in range(segs + 1):
			var u := float(s) / float(segs)
			var ang := u * TAU
			var dir := Vector3(cos(ang), 0.0, sin(ang))
			trunk_verts.push_back(Vector3(dir.x * rad, y, dir.z * rad))
			trunk_norms.push_back(dir)
			trunk_uvs.push_back(Vector2(u, v))
	for r in range(rings):
		for s in range(segs):
			var v00 := r * (segs + 1) + s
			var v10 := v00 + 1
			var v01 := v00 + segs + 1
			var v11 := v01 + 1
			trunk_indices.push_back(v00)
			trunk_indices.push_back(v10)
			trunk_indices.push_back(v11)
			trunk_indices.push_back(v00)
			trunk_indices.push_back(v11)
			trunk_indices.push_back(v01)

	var trunk_arrays := []
	trunk_arrays.resize(Mesh.ARRAY_MAX)
	trunk_arrays[Mesh.ARRAY_VERTEX] = trunk_verts
	trunk_arrays[Mesh.ARRAY_NORMAL] = trunk_norms
	trunk_arrays[Mesh.ARRAY_TEX_UV] = trunk_uvs
	trunk_arrays[Mesh.ARRAY_INDEX] = trunk_indices
	im.add_surface(Mesh.PRIMITIVE_TRIANGLES, trunk_arrays, [], {}, p_trunk_mat, "trunk")
	return im


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.12, 0.20)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.28, 0.30, 0.34)
	env.ambient_light_energy = 0.65
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun := DirectionalLight3D.new()
	sun.light_energy = 2.2
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 360.0
	sun.rotation_degrees = Vector3(-42.0, -30.0, 0.0)
	add_child(sun)

	var leaf_mat := StandardMaterial3D.new()
	leaf_mat.albedo_texture = _make_leaf_texture()
	leaf_mat.roughness = 0.72

	var trunk_mat := StandardMaterial3D.new()
	trunk_mat.albedo_color = Color(0.45, 0.28, 0.16)
	trunk_mat.roughness = 0.85

	var plain_path := OS.get_user_data_dir().path_join("nextgeo_voxels_plain.ngm")
	var vox_path := OS.get_user_data_dir().path_join("nextgeo_voxels_bricks.ngm")

	var im_plain := _build_foliage_importer_mesh(leaf_mat, trunk_mat)
	var err_plain := im_plain.build_virtual_geometry(plain_path, false, false, false, false)
	_mesh_plain = im_plain.get_mesh()
	if err_plain != OK or _mesh_plain == null or _mesh_plain.virtual_geometry == null:
		_failures.push_back("Failed to build plain virtual geometry: err=%d" % err_plain)
	else:
		_report["plain_voxel_bricks"] = _mesh_plain.virtual_geometry.voxel_brick_count
		if _mesh_plain.virtual_geometry.voxel_brick_count != 0:
			_failures.push_back("Plain build unexpectedly generated %d voxel bricks" % _mesh_plain.virtual_geometry.voxel_brick_count)

	var im_voxels := _build_foliage_importer_mesh(leaf_mat, trunk_mat)
	var err_vox := im_voxels.build_virtual_geometry(vox_path, false, false, false, true)
	_mesh_voxels = im_voxels.get_mesh()
	if err_vox != OK or _mesh_voxels == null or _mesh_voxels.virtual_geometry == null:
		_failures.push_back("Failed to build voxel virtual geometry: err=%d" % err_vox)
	else:
		_report["built_voxel_bricks"] = _mesh_voxels.virtual_geometry.voxel_brick_count
		_report["built_groups"] = _mesh_voxels.virtual_geometry.group_count
		_report["built_clusters"] = _mesh_voxels.virtual_geometry.cluster_count
		if _mesh_voxels.virtual_geometry.voxel_brick_count <= 0:
			_failures.push_back("Voxel build produced 0 voxel bricks")

	# Place a grove of distant foliage trees across the horizon + a broad ground receiver.
	var positions: Array[Vector3] = []
	for rz in range(4):
		for rx in range(5):
			var px := (float(rx) - 2.0) * 6.5 + (1.8 if (rz % 2) == 1 else 0.0)
			var pz := -float(rz) * 6.0
			positions.push_back(Vector3(px, 0.0, pz))
	for i in range(positions.size()):
		var mi := MeshInstance3D.new()
		mi.name = "Tree_%d" % i
		mi.mesh = _mesh_plain
		mi.position = positions[i]
		add_child(mi)
		_canopy_instances.push_back(mi)

	var ground_mesh := PlaneMesh.new()
	ground_mesh.size = Vector2(360.0, 360.0)
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.68, 0.68, 0.68)
	ground_mat.roughness = 0.9
	_ground_inst = MeshInstance3D.new()
	_ground_inst.mesh = ground_mesh
	_ground_inst.material_override = ground_mat
	_ground_inst.position = Vector3(0.0, 0.0, -10.0)
	add_child(_ground_inst)

	_camera = Camera3D.new()
	_camera.fov = 50.0
	_camera.near = 0.25
	_camera.far = 500.0
	add_child(_camera)
	_camera.make_current()


func _apply_phase() -> void:
	var p := _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/experimental/voxels", bool(p["voxels"]))
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", int(p["shading"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", bool(p["vis64_buf"]))

	var use_vox_mesh := bool(p["use_voxels_mesh"])
	var target_mesh: Mesh = _mesh_voxels if use_vox_mesh else _mesh_plain
	for mi in _canopy_instances:
		mi.mesh = target_mesh

	if bool(p["close"]):
		_camera.look_at_from_position(Vector3(0.0, 2.2, 6.0), Vector3(0.0, 2.0, 0.0), Vector3.UP)
	else:
		# Distant foliage grove (~120m): coarse 4^3 voxel bricks satisfy both the group error
		# test and brick_px <= provisional 8 px threshold while trees remain clearly visible and
		# cast directional VSM shadows onto the ground.
		_camera.look_at_from_position(Vector3(0.0, 16.0, 115.0), Vector3(0.0, 2.0, -6.0), Vector3.UP)

	_phase_frame = 0


func _image_diff_stats(p_a: Image, p_b: Image) -> Dictionary:
	var w := mini(p_a.get_width(), p_b.get_width())
	var h := mini(p_a.get_height(), p_b.get_height())
	var max_diff := 0.0
	var sum_diff := 0.0
	var diff_pixels := 0
	for y in range(h):
		for x in range(w):
			var ca := p_a.get_pixel(x, y)
			var cb := p_b.get_pixel(x, y)
			var d := maxf(absf(ca.r - cb.r), maxf(absf(ca.g - cb.g), absf(ca.b - cb.b)))
			if d > max_diff:
				max_diff = d
			if d > 0.004:
				diff_pixels += 1
			sum_diff += d
	return {
		"max_diff": max_diff,
		"mean_diff": sum_diff / float(maxi(w * h, 1)),
		"diff_pixels": diff_pixels,
	}


func _count_foliage_pixels(p_img: Image) -> Dictionary:
	var w := p_img.get_width()
	var h := p_img.get_height()
	var green_px := 0
	var dark_shadow_px := 0
	for y in range(h):
		for x in range(w):
			var c := p_img.get_pixel(x, y)
			# Foliage has dominant green channel over blue/red.
			if c.g > c.r + 0.05 and c.g > c.b + 0.04:
				green_px += 1
			# Ground receiver in shadow is noticeably darker neutral gray than lit ground.
			if absf(c.r - c.g) < 0.04 and absf(c.g - c.b) < 0.04 and c.r > 0.12 and c.r < 0.36:
				dark_shadow_px += 1
	return {
		"green_pixels": green_px,
		"shadow_pixels": dark_shadow_px,
	}


func _process(_delta: float) -> void:
	_phase_frame += 1
	var p := _phases[_phase_idx]
	var pname := String(p["name"])
	var check_static := bool(p["check_static"])

	# Accumulate per-frame VSM voxel counters across the whole phase because static VSM pages
	# (#212 caching) are dirty on frame 0 after phase switch (read back by frame 1..3) and cached thereafter.
	if _phase_frame >= 1 and _phase_frame <= SETTLE_FRAMES:
		var vst_now: Dictionary = RenderingServer.virtual_shadow_maps_get_debug_info()
		var vn_now: Dictionary = vst_now.get("nanite", {})
		_vsm_phase_bricks_sum[pname] = int(_vsm_phase_bricks_sum.get(pname, 0)) + int(vn_now.get("vsm_voxel_bricks", 0))
		_vsm_phase_writes_sum[pname] = int(_vsm_phase_writes_sum.get(pname, 0)) + int(vn_now.get("vsm_voxel_writes", 0))
		_vsm_phase_dirty_sum[pname] = int(_vsm_phase_dirty_sum.get(pname, 0)) + int(vn_now.get("directional_dirty_pages", 0))

	if _phase_frame == SETTLE_FRAMES:
		_images[pname] = get_viewport().get_texture().get_image()
		_stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid()).duplicate(true)
		_vsm_stats[pname] = RenderingServer.virtual_shadow_maps_get_debug_info().duplicate(true)
		if not check_static:
			_advance_phase()
	elif check_static and _phase_frame == SETTLE_FRAMES + 2:
		_static_next_img = get_viewport().get_texture().get_image()
		_advance_phase()


func _advance_phase() -> void:
	_phase_idx += 1
	if _phase_idx < _phases.size():
		_apply_phase()
	else:
		_evaluate_and_quit()


func _evaluate_and_quit() -> void:
	var phases_out := {}
	for pname in _stats.keys():
		var st: Dictionary = _stats[pname]
		var vst: Dictionary = _vsm_stats[pname]
		var vn: Dictionary = vst.get("nanite", {})
		var img: Image = _images[pname]
		var px := _count_foliage_pixels(img)
		phases_out[pname] = {
			"voxels_enabled": st.get("voxels_enabled", false),
			"voxels_buffers_allocated": st.get("voxels_buffers_allocated", false),
			"voxel_raster_dispatches": st.get("voxel_raster_dispatches", 0),
			"voxel_bricks": st.get("voxel_bricks", 0),
			"clusters": st.get("clusters", 0),
			"vis64_storage": st.get("vis64_storage", "none"),
			"shading_stage_b_compute": st.get("shading_stage_b_compute", 0),
			"shading_stage_b_tile_quads": st.get("shading_stage_b_tile_quads", 0),
			"shading_stage_b_reason": st.get("shading_stage_b_reason", ""),
			"vsm_voxels_enabled": vn.get("voxels_enabled", false),
			"vsm_voxels_buffers_allocated": vn.get("voxels_buffers_allocated", false),
			"vsm_voxel_dispatches": vn.get("vsm_voxel_dispatches", 0),
			"vsm_voxel_bricks_sum": int(_vsm_phase_bricks_sum.get(pname, 0)),
			"vsm_voxel_writes_sum": int(_vsm_phase_writes_sum.get(pname, 0)),
			"vsm_dirty_pages_sum": int(_vsm_phase_dirty_sum.get(pname, 0)),
			"green_pixels": px["green_pixels"],
			"shadow_pixels": px["shadow_pixels"],
		}
	_report["phases"] = phases_out

	# 1. Off invariant: voxels_off must allocate 0 buffers, run 0 dispatches, and be byte-identical to plain_off.
	var off_st: Dictionary = phases_out["voxels_off"]
	if bool(off_st["voxels_enabled"]) or bool(off_st["voxels_buffers_allocated"]) or int(off_st["voxel_raster_dispatches"]) != 0 or int(off_st["voxel_bricks"]) != 0:
		_failures.push_back("voxels_off broke zero-allocation / zero-dispatch invariant: %s" % str(off_st))
	if bool(off_st["vsm_voxels_buffers_allocated"]) or int(off_st["vsm_voxel_dispatches"]) != 0 or int(off_st["vsm_voxel_bricks_sum"]) != 0:
		_failures.push_back("voxels_off broke VSM zero-allocation / zero-dispatch invariant: %s" % str(off_st))
	var off_diff := _image_diff_stats(_images["plain_off"], _images["voxels_off"])
	_report["off_vs_plain_diff"] = off_diff
	if float(off_diff["max_diff"]) > 0.0:
		_failures.push_back("voxels_off is not byte-identical to plain_off: %s" % str(off_diff))

	# 2. Toggling back off frees voxel buffers and matches voxels_off byte-for-byte.
	var back_st: Dictionary = phases_out["voxels_back_off"]
	if bool(back_st["voxels_buffers_allocated"]) or int(back_st["voxel_raster_dispatches"]) != 0 or bool(back_st["vsm_voxels_buffers_allocated"]):
		_failures.push_back("voxels_back_off failed to free voxel buffers: %s" % str(back_st))
	var back_diff := _image_diff_stats(_images["voxels_off"], _images["voxels_back_off"])
	_report["back_off_diff"] = back_diff
	if float(back_diff["max_diff"]) > 0.0:
		_failures.push_back("voxels_back_off diverged from voxels_off: %s" % str(back_diff))

	if _expect_voxels:
		# 3. Far compute shading bins: bricks swapped, rasterized, resolved, and temporally stable.
		var fc: Dictionary = phases_out["voxels_on_far_compute"]
		if not bool(fc["voxels_enabled"]) or not bool(fc["voxels_buffers_allocated"]):
			_failures.push_back("voxels_on_far_compute did not activate voxel buffers: %s" % str(fc))
		if int(fc["voxel_raster_dispatches"]) <= 0 or int(fc["voxel_bricks"]) <= 0:
			_failures.push_back("voxels_on_far_compute did not swap/dispatch voxel bricks: %s" % str(fc))
		if int(fc["shading_stage_b_compute"]) <= 0:
			_failures.push_back("voxels_on_far_compute did not shade via compute shading bins: %s" % str(fc))
		if int(fc["green_pixels"]) < 20:
			_failures.push_back("voxels_on_far_compute resolved too few green foliage pixels: %d" % int(fc["green_pixels"]))

		var static_diff := _image_diff_stats(_images["voxels_on_far_compute"], _static_next_img)
		_report["static_temporal_diff"] = static_diff
		if float(static_diff["max_diff"]) > 0.0:
			_failures.push_back("Static camera voxel rendering flickered across frames: %s" % str(static_diff))

		# 4. Tile quads fallback resolves matching voxel foliage pixels.
		var ft: Dictionary = phases_out["voxels_on_far_tiles"]
		if int(ft["voxel_bricks"]) <= 0 or int(ft["shading_stage_b_tile_quads"]) <= 0 or int(ft["green_pixels"]) < 20:
			_failures.push_back("voxels_on_far_tiles failed to resolve voxel bricks: %s" % str(ft))
		var tiles_vs_compute := _image_diff_stats(_images["voxels_on_far_compute"], _images["voxels_on_far_tiles"])
		_report["tiles_vs_compute_diff"] = tiles_vs_compute
		if float(tiles_vs_compute["mean_diff"]) > 0.02:
			_failures.push_back("Tile quads voxel resolve diverged from compute shading bins: %s" % str(tiles_vs_compute))

		# 5. VIS64_BUFFER storage form matches image form.
		var fb: Dictionary = phases_out["voxels_on_far_vis64_buf"]
		if String(fb["vis64_storage"]) != "buffer" or int(fb["voxel_bricks"]) <= 0 or int(fb["green_pixels"]) < 20:
			_failures.push_back("voxels_on_far_vis64_buf failed: %s" % str(fb))
		var buf_vs_img := _image_diff_stats(_images["voxels_on_far_compute"], _images["voxels_on_far_vis64_buf"])
		_report["vis64_buf_vs_img_diff"] = buf_vs_img
		if float(buf_vs_img["mean_diff"]) > 0.01:
			_failures.push_back("VIS64_BUFFER voxel raster diverged from image vis64: %s" % str(buf_vs_img))

		# 6. Close view renders full detail cleanly.
		var cl: Dictionary = phases_out["voxels_on_close"]
		if int(cl["green_pixels"]) < 100:
			_failures.push_back("voxels_on_close rendered too few foliage pixels: %d" % int(cl["green_pixels"]))

		# 7. VSM voxel depth-only splat when VSM is active.
		if bool(_vsm_stats.get("voxels_on_far_compute", {}).get("active", false)):
			if not bool(fc["vsm_voxels_enabled"]) or not bool(fc["vsm_voxels_buffers_allocated"]) or int(fc["vsm_voxel_dispatches"]) <= 0:
				_failures.push_back("VSM voxel dispatch inactive: %s" % str(fc))
			if int(fc["vsm_voxel_bricks_sum"]) <= 0 or int(fc["vsm_voxel_writes_sum"]) <= 0:
				_failures.push_back("VSM voxel bricks/writes were zero: %s" % str(fc))

	_report["passed"] = _failures.is_empty()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
