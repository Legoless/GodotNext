extends Node3D
## NextGeo Dynamic Wind for Nanite Foliage GPU verification check (#296, plan §25 / §21.4).
##
## Verifies all runtime guarantees of experimental Dynamic Wind:
##   1. Project settings `rendering/nextgeo/experimental/dynamic_wind` (false) and
##      `rendering/nextgeo/experimental/dynamic_wind_mode` (0 = Force Based) default cleanly;
##      when off, frames are byte-identical (0 diff bytes) regardless of World3D wind strength,
##      with 0 dynamic wind GPU buffers allocated and 0 simulation dispatches.
##   2. Force-Based mode (`dynamic_wind = true`, `dynamic_wind_mode = 0`):
##      - Runs GPU per-bone spring + flutter compute simulation writing into #160's skin palette
##        arena with 0 validation errors, 0 SyncVal hazards, 0 overflow, and `cut_match = true`.
##      - Multiple instances of the same wind mesh share `<= 4` global simulation slots.
##      - Visibly displaces foliage across frames and produces non-zero motion velocity via
##        #160's previous-frame palette slot.
##      - Drives VSM dynamic shadows (`vsm.nanite.dynamic_clusters > 0`, `overflow = 0`, `sink = 0`)
##        with visible shadow shift when flipping wind direction.
##   3. Local Wind Force Promotion:
##      - Adding a local wind force overlapping ONLY the left tree promotes ONLY that instance
##        (`dynamic_wind_promoted_instances == 1`), visibly deflecting the left tree while leaving
##        the untouched right sibling calm.
##      - Removing the local force lets the promoted instance settle and demote back to the shared
##        simulation pool (`dynamic_wind_promoted_instances == 0`).
##   4. Parametric mode (`dynamic_wind = true`, `dynamic_wind_mode = 1`):
##      - Releases dynamic wind simulation buffers/slots (`dynamic_wind_buffers_allocated == false`,
##        `dynamic_wind_dispatches == 0`, `skin_palette_used_bones == 0`, `skin_palette_bytes == 0`)
##        while visibly swaying foliage via #152's procedural shader bones.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_skinned/nextgeo_skinned_probe.gd")

var _phase := ""
var _phase_frame := 0
var _phase_time := 0.0
var _force_a_captured := false
var _param_a_captured := false
var _phase_root: Node3D
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

var _tree_mesh: ArrayMesh

var _cam: Camera3D
var _sun: DirectionalLight3D
var _we: WorldEnvironment
var _probe: CompositorEffect
var _ground_mi: MeshInstance3D
var _shared_trees: Array[MeshInstance3D] = []
var _left_tree: MeshInstance3D
var _right_tree: MeshInstance3D
var _vsm_tree: MeshInstance3D

var _img_off_wind: Image
var _img_off_calm: Image
var _img_force_a: Image
var _img_force_b: Image
var _img_promo_base: Image
var _img_promo_active: Image
var _img_vsm_pos_x: Image
var _img_vsm_neg_x: Image
var _img_param_a: Image
var _img_param_b: Image
var _local_force_id := 0
var _demote_frames := 0


func _ready() -> void:
	# 1. Verify experimental Dynamic Wind defaults before touching ProjectSettings.
	var default_dw := bool(ProjectSettings.get_setting("rendering/nextgeo/experimental/dynamic_wind", true))
	var default_dw_mode := int(ProjectSettings.get_setting("rendering/nextgeo/experimental/dynamic_wind_mode", -1))
	var default_fw := bool(ProjectSettings.get_setting("rendering/nextgeo/experimental/foliage_wind", true))
	if default_dw:
		_failures.append("init: rendering/nextgeo/experimental/dynamic_wind defaulted to true, expected false")
	if default_dw_mode != 0:
		_failures.append("init: rendering/nextgeo/experimental/dynamic_wind_mode defaulted to %d, expected 0" % default_dw_mode)
	if default_fw:
		_failures.append("init: rendering/nextgeo/experimental/foliage_wind defaulted to true, expected false")
	_summary["defaults"] = {
		"dynamic_wind": default_dw,
		"dynamic_wind_mode": default_dw_mode,
		"foliage_wind": default_fw,
	}

	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 0.0)

	_summary["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_wind_mesh()
	_build_scene()
	_enter_phase("off_wind")


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_dynwind_%d_%s_%d.ngm" % [OS.get_process_id(), p_tag.validate_filename(), _ngm_counter]
	_temp_files.append(path)
	return path


func _make_tree_arrays(p_radial: int, p_rings: int, p_height: float) -> Array:
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var tangs := PackedFloat32Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()

	var cols := p_radial + 1
	for r in range(p_rings + 1):
		var v := float(r) / float(p_rings)
		var y := v * p_height
		var rad := 0.20
		if v > 0.28:
			var t := (v - 0.28) / 0.72
			rad = 0.20 + 1.45 * sin(t * PI) + 0.08 * sin(t * 14.0 * PI)
		for c in range(cols):
			var u := float(c) / float(p_radial)
			var ang := u * TAU
			var ca := cos(ang)
			var sa := sin(ang)
			var lobe := 1.0 + 0.20 * sin(ang * 4.0) * (1.0 if v > 0.28 else 0.0)
			var rr := rad * lobe
			verts.append(Vector3(rr * ca, y, rr * sa))
			norms.append(Vector3(ca, 0.25, sa).normalized())
			tangs.append(-sa)
			tangs.append(0.0)
			tangs.append(ca)
			tangs.append(1.0)
			uvs.append(Vector2(u, v))

	for r in range(p_rings):
		for c in range(p_radial):
			var i0 := r * cols + c
			var i1 := i0 + 1
			var i2 := (r + 1) * cols + c
			var i3 := i2 + 1
			indices.append(i0)
			indices.append(i2)
			indices.append(i1)
			indices.append(i1)
			indices.append(i2)
			indices.append(i3)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_TANGENT] = tangs
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	return arrays


func _build_wind_mesh() -> void:
	var arrays := _make_tree_arrays(28, 36, 4.5)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.24, 0.64, 0.27)
	mat.roughness = 0.72

	var imp := ImporterMesh.new()
	imp.add_surface(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, mat)
	var err := imp.build_virtual_geometry(_alloc_temp_ngm("tree"), false, false, true)
	_tree_mesh = imp.get_mesh()
	if err != OK or _tree_mesh == null or _tree_mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(tree, wind=true) failed: err=%d" % err)
		return
	var wind := _tree_mesh.get_virtual_geometry().get_wind()
	if wind == null or wind.get_bone_count() < 2:
		_failures.append("imported VirtualGeometryWind invalid")
		return
	var masses := wind.get_bone_masses()
	var catches := wind.get_bone_wind_catches()
	if masses.size() != wind.get_bone_count() or catches.size() != wind.get_bone_count():
		_failures.append("imported bone mass/wind_catch arrays mismatch bone count (%d/%d vs %d)" % [masses.size(), catches.size(), wind.get_bone_count()])
	_summary["imported_wind"] = {
		"bone_count": wind.get_bone_count(),
		"max_sway": wind.get_max_sway(),
		"trunk_mass": masses[0] if masses.size() > 0 else 0.0,
		"branch0_wind_catch": catches[1] if catches.size() > 1 else 0.0,
	}


func _build_scene() -> void:
	_phase_root = Node3D.new()
	_phase_root.name = "DynamicWindTestRoot"
	add_child(_phase_root)

	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-40.0, 15.0, 0.0)
	_sun.light_energy = 2.2
	_sun.shadow_enabled = true
	_phase_root.add_child(_sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.06, 0.08, 0.11)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.28, 0.30, 0.32)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR

	_probe = ProbeScript.new()
	var comp := Compositor.new()
	var effects: Array[CompositorEffect] = [_probe]
	comp.compositor_effects = effects

	_we = WorldEnvironment.new()
	_we.environment = env
	_we.compositor = comp
	_phase_root.add_child(_we)

	_cam = Camera3D.new()
	_cam.fov = 52.0
	_cam.near = 0.2
	_cam.far = 200.0
	_cam.current = true
	_phase_root.add_child(_cam)

	var ground_mesh := PlaneMesh.new()
	ground_mesh.size = Vector2(44.0, 44.0)
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.82, 0.82, 0.82)
	ground_mat.roughness = 0.9
	_ground_mi = MeshInstance3D.new()
	_ground_mi.name = "Ground"
	_ground_mi.mesh = ground_mesh
	_ground_mi.material_override = ground_mat
	_phase_root.add_child(_ground_mi)

	# 8 trees for shared-slot pooling + global Force-Based wind verification.
	for i in range(8):
		var mi := MeshInstance3D.new()
		mi.name = "SharedTree_%d" % i
		mi.mesh = _tree_mesh
		var row := i / 4
		var col := i % 4
		mi.position = Vector3((float(col) - 1.5) * 3.2, 0.0, -float(row) * 3.8)
		mi.scale = Vector3(0.75, 0.75, 0.75)
		_phase_root.add_child(mi)
		_shared_trees.append(mi)

	# 2 widely separated side-by-side trees for local wind force promotion & parametric mode check.
	_left_tree = MeshInstance3D.new()
	_left_tree.name = "LeftPromoTree"
	_left_tree.mesh = _tree_mesh
	_left_tree.position = Vector3(-3.2, 0.0, 0.0)
	_left_tree.scale = Vector3(0.70, 0.70, 0.70)
	_left_tree.visible = false
	_phase_root.add_child(_left_tree)

	_right_tree = MeshInstance3D.new()
	_right_tree.name = "RightPromoTree"
	_right_tree.mesh = _tree_mesh
	_right_tree.position = Vector3(3.2, 0.0, 0.0)
	_right_tree.scale = Vector3(0.70, 0.70, 0.70)
	_right_tree.visible = false
	_phase_root.add_child(_right_tree)

	# Dedicated single tree for VSM dynamic shadow verification.
	_vsm_tree = MeshInstance3D.new()
	_vsm_tree.name = "VsmTree"
	_vsm_tree.mesh = _tree_mesh
	_vsm_tree.position = Vector3(0.0, 0.0, 0.0)
	_vsm_tree.visible = false
	_phase_root.add_child(_vsm_tree)


func _set_visibility(p_shared: bool, p_pair: bool, p_vsm: bool) -> void:
	for mi in _shared_trees:
		mi.visible = p_shared
	_left_tree.visible = p_pair
	_right_tree.visible = p_pair
	_vsm_tree.visible = p_vsm


func _set_world_wind(p_dir: Vector3, p_strength: float, p_gust_freq: float) -> void:
	var w := get_world_3d()
	if w != null:
		w.wind_direction = p_dir
		w.wind_strength = p_strength
		w.wind_gust_frequency = p_gust_freq


func _enter_phase(p_name: String) -> void:
	_phase = p_name
	_phase_frame = 0
	_phase_time = 0.0
	_force_a_captured = false
	_param_a_captured = false
	var w := get_world_3d()
	match p_name:
		"off_wind":
			_set_visibility(true, false, false)
			_sun.rotation_degrees = Vector3(-40.0, 15.0, 0.0)
			_cam.position = Vector3(0.0, 3.6, 12.0)
			_cam.look_at(Vector3(0.0, 1.8, -1.5), Vector3.UP)
			if w != null:
				w.clear_wind_forces()
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", false)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind", false)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 2.0)
		"off_calm":
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 0.0, 2.0)
		"force_shared":
			_set_visibility(true, false, false)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/foliage_wind", false)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 0)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.5, 2.2)
		"force_velocity":
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 5.0, 2.8)
		"promo_settle":
			_set_visibility(false, true, false)
			_cam.position = Vector3(0.0, 2.5, 11.2)
			_cam.look_at(Vector3(0.0, 1.8, 0.0), Vector3.UP)
			if w != null:
				w.clear_wind_forces()
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 0.0, 1.0)
		"promo_active":
			if w != null:
				# Radius 2.0 centered at left tree (-3.2, 2.0, 0.0): reaches X in [-5.2, -1.2],
				# overlapping ONLY _left_tree and staying > 2.4m clear of _right_tree (+3.2).
				_local_force_id = w.add_wind_force(
					Vector3(-3.2, 2.0, 0.0),
					2.0,
					Vector3(1.0, 0.0, 0.25).normalized(),
					14.0,
					1.0
				)
		"promo_demote":
			if w != null and _local_force_id > 0:
				w.remove_wind_force(_local_force_id)
				_local_force_id = 0
			_demote_frames = 0
		"vsm_pos_x":
			_set_visibility(false, false, true)
			_sun.rotation_degrees = Vector3(-35.0, 0.0, 0.0)
			_cam.position = Vector3(0.0, 9.5, 6.5)
			_cam.look_at(Vector3(0.0, 0.0, -3.5), Vector3.UP)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 6.0, 0.25)
		"vsm_neg_x":
			_set_world_wind(Vector3(-1.0, 0.0, 0.0), 6.0, 0.25)
		"parametric_mode":
			_set_visibility(false, true, false)
			_sun.rotation_degrees = Vector3(-40.0, 15.0, 0.0)
			_cam.position = Vector3(0.0, 2.5, 11.2)
			_cam.look_at(Vector3(0.0, 1.8, 0.0), Vector3.UP)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind", true)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 1)
			_set_world_wind(Vector3(1.0, 0.0, 0.0), 4.8, 2.4)
		"shutdown_off":
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind", false)
			ProjectSettings.set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 0)


func _capture_rgb8() -> Image:
	var img := get_viewport().get_texture().get_image()
	if img != null and img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _diff_byte_count(p_a: Image, p_b: Image) -> int:
	if p_a == null or p_b == null or p_a.get_size() != p_b.get_size():
		return -1
	var da := p_a.get_data()
	var db := p_b.get_data()
	if da.size() != db.size():
		return -1
	var diffs := 0
	for i in range(da.size()):
		if da[i] != db[i]:
			diffs += 1
	return diffs


func _mean_abs_diff_rect(p_a: Image, p_b: Image, p_x0: float = 0.0, p_x1: float = 1.0, p_y0: float = 0.0, p_y1: float = 1.0) -> float:
	if p_a == null or p_b == null or p_a.get_size() != p_b.get_size():
		return 1.0
	var w := p_a.get_width()
	var h := p_a.get_height()
	var x0 := clampi(int(round(float(w) * p_x0)), 0, w)
	var x1 := clampi(int(round(float(w) * p_x1)), x0 + 1, w)
	var y0 := clampi(int(round(float(h) * p_y0)), 0, h)
	var y1 := clampi(int(round(float(h) * p_y1)), y0 + 1, h)
	var da := p_a.get_data()
	var db := p_b.get_data()
	var total := 0.0
	var pixels := 0
	for y in range(y0, y1):
		var row_base := y * w * 4
		for x in range(x0, x1):
			var idx := row_base + x * 4
			total += absf(float(da[idx]) - float(db[idx]))
			total += absf(float(da[idx + 1]) - float(db[idx + 1]))
			total += absf(float(da[idx + 2]) - float(db[idx + 2]))
			pixels += 1
	return total / (float(maxi(pixels, 1)) * 3.0 * 255.0)


func _analyze_velocity(p_cap: Dictionary) -> Dictionary:
	var words: PackedInt32Array = p_cap.get("words", PackedInt32Array())
	var n := words.size() / 3
	var covered := 0
	var moving := 0
	var sum_mag := 0.0
	var max_mag := 0.0
	var bytes := PackedByteArray()
	bytes.resize(8)
	for i in range(n):
		var inst_id := words[i * 3 + 0]
		if inst_id == -1 or inst_id == 0xFFFFFFFF:
			continue
		covered += 1
		bytes.encode_s32(0, words[i * 3 + 1])
		bytes.encode_s32(4, words[i * 3 + 2])
		var vx := bytes.decode_float(0)
		var vy := bytes.decode_float(4)
		var mag := sqrt(vx * vx + vy * vy)
		sum_mag += mag
		if mag > max_mag:
			max_mag = mag
		if mag > 1.0e-4:
			moving += 1
	return {
		"covered": covered,
		"moving": moving,
		"mean_mag": sum_mag / float(maxi(covered, 1)),
		"max_mag": max_mag,
		"velocity_valid": bool(p_cap.get("velocity", false)),
	}


func _check_common_ng_stats(p_tag: String) -> Dictionary:
	var stats := RenderingServer.virtual_geometry_get_debug_stats()
	if int(stats.get("overflow", 0)) != 0:
		_failures.append("%s: NextGeo cut overflow=%d" % [p_tag, int(stats.get("overflow", 0))])
	if int(stats.get("double_dispatch", 0)) != 0:
		_failures.append("%s: double_dispatch=%d" % [p_tag, int(stats.get("double_dispatch", 0))])
	if int(stats.get("missing", 0)) != 0:
		_failures.append("%s: missing=%d" % [p_tag, int(stats.get("missing", 0))])
	if not bool(stats.get("cut_match", false)):
		_failures.append("%s: GPU/CPU cut_match=false" % p_tag)
	return stats


func _process(delta: float) -> void:
	_phase_frame += 1
	_phase_time += delta
	match _phase:
		"off_wind":
			if _phase_frame == 12:
				_img_off_wind = _capture_rgb8()
				var st := _check_common_ng_stats("off_wind")
				if bool(st.get("dynamic_wind_buffers_allocated", true)):
					_failures.append("off_wind: dynamic_wind_buffers_allocated=true while dynamic_wind=false")
				if int(st.get("dynamic_wind_dispatches", -1)) != 0 or int(st.get("dynamic_wind_total_dispatches", -1)) != 0:
					_failures.append("off_wind: expected 0 dynamic wind dispatches while off")
				if int(st.get("skin_palette_bytes", -1)) != 0 or int(st.get("skin_palette_used_bones", -1)) != 0:
					_failures.append("off_wind: expected 0 skin palette memory while off")
				_summary["off_wind"] = {
					"buffers_allocated": bool(st.get("dynamic_wind_buffers_allocated", false)),
					"dispatches": int(st.get("dynamic_wind_dispatches", 0)),
					"total_dispatches": int(st.get("dynamic_wind_total_dispatches", 0)),
					"skin_palette_bytes": int(st.get("skin_palette_bytes", 0)),
					"floor_reason": str(st.get("dynamic_wind_floor_reason", "")),
				}
				_enter_phase("off_calm")
		"off_calm":
			if _phase_frame == 12:
				_img_off_calm = _capture_rgb8()
				var diff_bytes := _diff_byte_count(_img_off_wind, _img_off_calm)
				_summary["off_byte_identical_diff_bytes"] = diff_bytes
				if diff_bytes != 0:
					_failures.append("off_byte_identical: expected 0 diff bytes when dynamic_wind=false, got %d" % diff_bytes)
				_enter_phase("force_shared")
		"force_shared":
			if not _force_a_captured and _phase_frame >= 16 and _phase_time >= 0.45:
				_force_a_captured = true
				_img_force_a = _capture_rgb8()
				var st := _check_common_ng_stats("force_shared")
				var shared_slots := int(st.get("dynamic_wind_shared_slots", 0))
				var promoted := int(st.get("dynamic_wind_promoted_instances", -1))
				var dispatches := int(st.get("dynamic_wind_dispatches", 0))
				var bufs := bool(st.get("dynamic_wind_buffers_allocated", false))
				var supported := bool(st.get("dynamic_wind_supported", false))
				var reason := str(st.get("dynamic_wind_floor_reason", ""))
				if not supported or reason != "NONE":
					_failures.append("force_shared: expected dynamic_wind supported (NONE), got supported=%s reason=%s" % [str(supported), reason])
				if not bufs or dispatches < 1:
					_failures.append("force_shared: expected GPU buffers allocated & dispatch >= 1, got bufs=%s dispatches=%d" % [str(bufs), dispatches])
				if shared_slots <= 0 or shared_slots > 4:
					_failures.append("force_shared: expected 8 instances to share 1..4 slots, got %d" % shared_slots)
				if promoted != 0:
					_failures.append("force_shared: expected 0 promoted instances without local forces, got %d" % promoted)
				_summary["force_shared"] = {
					"supported": supported,
					"floor_reason": reason,
					"mode": str(st.get("dynamic_wind_mode", "")),
					"buffers_allocated": bufs,
					"dispatches": dispatches,
					"shared_slots": shared_slots,
					"promoted_instances": promoted,
					"palette_bones": int(st.get("dynamic_wind_palette_bones", 0)),
					"cut_match": bool(st.get("cut_match", false)),
				}
			elif _force_a_captured and _phase_frame >= 32 and _phase_time >= 0.90:
				_img_force_b = _capture_rgb8()
				var diff_a := _mean_abs_diff_rect(_img_force_a, _img_off_calm)
				var diff_b := _mean_abs_diff_rect(_img_force_b, _img_off_calm)
				var diff_ab := _mean_abs_diff_rect(_img_force_a, _img_force_b)
				_summary["force_shared_displacement_mae"] = maxf(diff_a, diff_b)
				_summary["force_shared_motion_mae"] = diff_ab
				if maxf(diff_a, diff_b) < 0.0025:
					_failures.append("force_shared: insufficient displacement vs rest (mae=%.6f)" % maxf(diff_a, diff_b))
				if diff_ab < 0.0004:
					_failures.append("force_shared: insufficient frame-to-frame motion (mae=%.6f)" % diff_ab)
				_enter_phase("force_velocity")
		"force_velocity":
			if _phase_frame == 8:
				_probe.capture = {}
				_probe.capture_armed = true
			elif _phase_frame == 12:
				var vel := _analyze_velocity(_probe.capture)
				_summary["force_velocity"] = vel
				if vel["covered"] <= 0 or vel["moving"] < 20 or vel["max_mag"] < 1.0e-4:
					_failures.append("force_velocity: expected non-zero velocity on GPU-simulated foliage, got %s" % str(vel))
				_enter_phase("promo_settle")
		"promo_settle":
			if _phase_frame >= 30 and _phase_time >= 0.70:
				_img_promo_base = _capture_rgb8()
				var st := _check_common_ng_stats("promo_settle")
				if int(st.get("dynamic_wind_promoted_instances", -1)) != 0:
					_failures.append("promo_settle: expected 0 promoted instances at calm baseline, got %d" % int(st.get("dynamic_wind_promoted_instances", -1)))
				_enter_phase("promo_active")
		"promo_active":
			if _phase_frame >= 20 and _phase_time >= 0.55:
				_img_promo_active = _capture_rgb8()
				var st := _check_common_ng_stats("promo_active")
				var promoted := int(st.get("dynamic_wind_promoted_instances", 0))
				var forces := int(st.get("dynamic_wind_local_forces", 0))
				var mae_left := _mean_abs_diff_rect(_img_promo_active, _img_promo_base, 0.0, 0.48, 0.0, 1.0)
				var mae_right := _mean_abs_diff_rect(_img_promo_active, _img_promo_base, 0.52, 1.0, 0.0, 1.0)
				_summary["local_promotion"] = {
					"promoted_instances": promoted,
					"local_forces": forces,
					"mae_left": mae_left,
					"mae_right": mae_right,
				}
				if promoted != 1:
					_failures.append("promo_active: expected exactly 1 promoted instance, got %d" % promoted)
				if forces != 1:
					_failures.append("promo_active: expected 1 local wind force, got %d" % forces)
				if mae_left < 0.002:
					_failures.append("promo_active: promoted left tree showed insufficient localized deflection (mae_left=%.6f)" % mae_left)
				if mae_left <= mae_right * 3.0:
					_failures.append("promo_active: expected left deflection >> untouched right sibling (left=%.6f right=%.6f)" % [mae_left, mae_right])
				_enter_phase("promo_demote")
		"promo_demote":
			_demote_frames += 1
			var st := RenderingServer.virtual_geometry_get_debug_stats()
			var promoted := int(st.get("dynamic_wind_promoted_instances", -1))
			if promoted == 0:
				_summary["promo_demote_frames"] = _demote_frames
				_enter_phase("vsm_pos_x")
			elif _demote_frames >= 120:
				_summary["promo_demote_frames"] = _demote_frames
				_failures.append("promo_demote: promoted instance failed to demote within 120 frames (still %d)" % promoted)
				_enter_phase("vsm_pos_x")
		"vsm_pos_x":
			if _phase_frame >= 20 and _phase_time >= 0.55:
				_img_vsm_pos_x = _capture_rgb8()
				var _st := _check_common_ng_stats("vsm_pos_x")
				var vsm_info := RenderingServer.virtual_shadow_maps_get_debug_info()
				var vn: Dictionary = vsm_info.get("nanite", {})
				_summary["vsm_pos_x"] = {
					"active": bool(vn.get("active", false)),
					"dynamic_clusters": int(vn.get("dynamic_clusters", 0)),
					"static_clusters": int(vn.get("static_clusters", 0)),
					"overflow": int(vn.get("overflow", 0)),
					"sink": int(vn.get("sink", 0)),
				}
				if int(vn.get("overflow", 0)) != 0:
					_failures.append("vsm_pos_x: VSM nanite overflow=%d" % int(vn.get("overflow", 0)))
				if int(vn.get("sink", 0)) != 0:
					_failures.append("vsm_pos_x: VSM nanite sink=%d" % int(vn.get("sink", 0)))
				if int(vn.get("dynamic_clusters", 0)) <= 0:
					_failures.append("vsm_pos_x: expected VSM nanite dynamic_clusters > 0, got %d" % int(vn.get("dynamic_clusters", 0)))
				_enter_phase("vsm_neg_x")
		"vsm_neg_x":
			if _phase_frame >= 30 and _phase_time >= 0.85:
				_img_vsm_neg_x = _capture_rgb8()
				var shadow_diff := _mean_abs_diff_rect(_img_vsm_pos_x, _img_vsm_neg_x, 0.0, 1.0, 0.05, 0.55)
				_summary["vsm_shadow_shift_mae"] = shadow_diff
				if shadow_diff < 0.0015:
					_failures.append("vsm_dynamic_shadow: expected visible shadow shift between +X and -X wind (mae=%.6f)" % shadow_diff)
				_enter_phase("parametric_mode")
		"parametric_mode":
			if not _param_a_captured and _phase_frame >= 12 and _phase_time >= 0.30:
				_param_a_captured = true
				_img_param_a = _capture_rgb8()
				var st := _check_common_ng_stats("parametric_mode")
				var bufs := bool(st.get("dynamic_wind_buffers_allocated", true))
				var dispatches := int(st.get("dynamic_wind_dispatches", -1))
				var active_slots := int(st.get("dynamic_wind_active_slots", -1))
				var pal_bones := int(st.get("skin_palette_used_bones", -1))
				var pal_bytes := int(st.get("skin_palette_bytes", -1))
				if str(st.get("dynamic_wind_mode", "")) != "parametric":
					_failures.append("parametric_mode: expected mode='parametric', got '%s'" % str(st.get("dynamic_wind_mode", "")))
				if bufs or dispatches != 0 or active_slots != 0:
					_failures.append("parametric_mode: expected 0 dynamic wind buffers/dispatches/slots, got bufs=%s disp=%d slots=%d" % [str(bufs), dispatches, active_slots])
				if pal_bones != 0 or pal_bytes != 0:
					_failures.append("parametric_mode: expected 0 skin palette memory, got bones=%d bytes=%d" % [pal_bones, pal_bytes])
				_summary["parametric_mode"] = {
					"mode": str(st.get("dynamic_wind_mode", "")),
					"buffers_allocated": bufs,
					"dispatches": dispatches,
					"active_slots": active_slots,
					"skin_palette_used_bones": pal_bones,
					"skin_palette_bytes": pal_bytes,
					"cut_match": bool(st.get("cut_match", false)),
				}
			elif _param_a_captured and _phase_frame >= 24 and _phase_time >= 0.65:
				_img_param_b = _capture_rgb8()
				var diff_rest := maxf(_mean_abs_diff_rect(_img_param_a, _img_promo_base), _mean_abs_diff_rect(_img_param_b, _img_promo_base))
				var diff_ab := _mean_abs_diff_rect(_img_param_a, _img_param_b)
				_summary["parametric_displacement_mae"] = diff_rest
				_summary["parametric_motion_mae"] = diff_ab
				if diff_rest < 0.0025:
					_failures.append("parametric_mode: insufficient foliage sway vs rest (mae=%.6f)" % diff_rest)
				if diff_ab < 0.0004:
					_failures.append("parametric_mode: insufficient frame-to-frame sway motion (mae=%.6f)" % diff_ab)
				_enter_phase("shutdown_off")
		"shutdown_off":
			if _phase_frame == 6:
				var st := _check_common_ng_stats("shutdown_off")
				if bool(st.get("dynamic_wind_buffers_allocated", true)) or int(st.get("dynamic_wind_dispatches", -1)) != 0:
					_failures.append("shutdown_off: expected dynamic wind buffers freed and 0 dispatches after turning off")
				_finish()


func _finish() -> void:
	for p in _temp_files:
		var abs_p := ProjectSettings.globalize_path(p)
		if FileAccess.file_exists(abs_p):
			DirAccess.remove_absolute(abs_p)
	_summary["failures"] = _failures
	_summary["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT %s" % JSON.stringify(_summary))
	get_tree().quit(0 if _failures.is_empty() else 1)
