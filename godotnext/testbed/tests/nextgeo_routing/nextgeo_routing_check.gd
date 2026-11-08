extends Node3D
## NextGeo exclusive dispatch, auxiliary proxy & debug-view GPU check (#79):
## 1. Chain rules (valid vs empty/invalid ShaderMaterial next_pass / overlay),
##    octant GridMap VGD_IGNORED vs MultiMeshInstance3D MULTIMESH_OR_PARTICLES,
##    baked GridMap BAKED_GRIDMAP, auxiliary proxy routing (ReflectionProbe +
##    GPUParticlesCollisionHeightField3D -> proxy, 0 authored virtual draws,
##    0 non-main-view page requests), and UV2 bake on authored surfaces.
## 2. All 7 Viewport.DEBUG_DRAW_NEXTGEO_* modes (including MSAA SubViewport),
##    view_route == "virtual", 0 double-drawn / 0 missing, and 16-bin
##    triangle_size_histogram readback.
## 3. NEXTGEO_CRACKS exact (255, 0, 255) magenta check: 0 pixels on watertight
##    closed virtual + legacy cubes; > 0 pixels on both virtual and legacy
##    closed cubes when a front face is removed to expose interior backfaces.
## 4. NEXTGEO_FALLBACK Q14 async readback: fallback_coverage == 0.0 when VGD
##    surfaces route virtual alongside non-VGD legacy geometry, and
##    0.0 < fallback_coverage < 1.0 when a VGD surface falls back to legacy.

var _phase := ""
var _phase_frame := 0
var _phase_root: Node3D
var _temp_files: Array[String] = []
var _temp_rids: Array[RID] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

# Phase 1 nodes
var _p1_valid_empty_np: MeshInstance3D
var _p1_blocked_np: MeshInstance3D
var _p1_valid_empty_ov: MeshInstance3D
var _p1_blocked_ov: MeshInstance3D
var _p1_stencil: MeshInstance3D
var _p1_mmi: MultiMeshInstance3D
var _p1_internal_mm_inst: RID
var _p1_baked_gm: GridMap
var _p1_uv2_mesh: ArrayMesh
var _p1_uv2_mat: StandardMaterial3D
var _p1_aux_base := 0
var _p1_vgd_warn_base := 0
var _p1_non_main_base := 0

# Phase 2 state
var _p2_modes: Array[Dictionary] = []
var _p2_idx := 0
var _p2_msaa_vp: SubViewport

# Phase 3 state
var _p3_step := 0

# Phase 4 state
var _p4_step := 0
var _p4_fallback_inst: MeshInstance3D


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0
	_start_phase_1()


func _alloc_temp_path(p_prefix: String, p_ext: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_routing_%d_%s_%d.%s" % [OS.get_process_id(), p_prefix.validate_filename(), _ngm_counter, p_ext]
	_temp_files.append(path)
	return path


func _clear_phase_root() -> void:
	for rid in _temp_rids:
		if rid.is_valid():
			RenderingServer.free_rid(rid)
	_temp_rids.clear()
	if _phase_root != null:
		remove_child(_phase_root)
		_phase_root.free()
		_phase_root = null
	_phase_frame = 0


func _make_closed_box_arrays(p_half: float, p_omit_front_face: bool, p_with_uv2: bool) -> Array:
	# Six outward-facing faces (+Z front, -Z back, +X, -X, +Y, -Y).
	# Godot's clockwise front-face convention in right-handed space:
	# looking at a face from outside, (v0, v1, v2) winds clockwise -> cross(v1-v0, v2-v0) points inward?
	# Wait: let's match BoxMesh's exact winding by taking BoxMesh arrays and optionally dropping face 0!
	var bm := BoxMesh.new()
	bm.size = Vector3(p_half * 2.0, p_half * 2.0, p_half * 2.0)
	var src := bm.surface_get_arrays(0)
	var verts: PackedVector3Array = src[Mesh.ARRAY_VERTEX]
	var norms: PackedVector3Array = src[Mesh.ARRAY_NORMAL]
	var tangs: PackedFloat32Array = src[Mesh.ARRAY_TANGENT]
	var uvs: PackedVector2Array = src[Mesh.ARRAY_TEX_UV]
	var idx: PackedInt32Array = src[Mesh.ARRAY_INDEX]

	if p_omit_front_face:
		var kept := PackedInt32Array()
		var tri := 0
		while tri + 2 < idx.size():
			var i0 := idx[tri]
			var i1 := idx[tri + 1]
			var i2 := idx[tri + 2]
			var avg_n := (norms[i0] + norms[i1] + norms[i2]) / 3.0
			# Drop the +Z face facing the camera at +Z so interior backfaces of -Z/+-X/+-Y are visible!
			if avg_n.z <= 0.5:
				kept.push_back(i0)
				kept.push_back(i1)
				kept.push_back(i2)
			tri += 3
		idx = kept

	var out := []
	out.resize(Mesh.ARRAY_MAX)
	out[Mesh.ARRAY_VERTEX] = verts
	out[Mesh.ARRAY_NORMAL] = norms
	out[Mesh.ARRAY_TANGENT] = tangs
	out[Mesh.ARRAY_TEX_UV] = uvs
	if p_with_uv2:
		var uv2 := PackedVector2Array()
		uv2.resize(verts.size())
		for i in verts.size():
			uv2[i] = uvs[i]
		out[Mesh.ARRAY_TEX_UV2] = uv2
	out[Mesh.ARRAY_INDEX] = idx
	return out


func _build_vmesh(p_arrays: Array, p_mat: Material, p_tag: String) -> ArrayMesh:
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, p_arrays)
	var imp := ImporterMesh.from_mesh(am)
	var err := imp.build_virtual_geometry(_alloc_temp_path(p_tag, "ngm"), false)
	if err != OK:
		_failures.append("build_virtual_geometry(%s) failed: %d" % [p_tag, err])
		return null
	var vmesh := imp.get_mesh()
	if vmesh != null and p_mat != null:
		vmesh.surface_set_material(0, p_mat)
	return vmesh


func _setup_env_and_light(p_root: Node3D, p_bg: Color = Color(0.05, 0.06, 0.08)) -> void:
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-38.0, 26.0, 0.0)
	p_root.add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = p_bg
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.4, 0.4, 0.45)
	var we := WorldEnvironment.new()
	we.environment = env
	p_root.add_child(we)


func _process(_delta: float) -> void:
	_phase_frame += 1
	match _phase:
		"p1":
			_tick_p1()
		"p2":
			_tick_p2()
		"p3":
			_tick_p3()
		"p4":
			_tick_p4()


# ---------------------------------------------------------------------------
# Phase 1: Chain rules, GridMap VGD_IGNORED, Auxiliary Proxy & UV2 Bake
# ---------------------------------------------------------------------------

func _start_phase_1() -> void:
	_clear_phase_root()
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED

	var base_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	_p1_aux_base = int(base_stats.get("auxiliary_proxy_draws", 0))
	_p1_vgd_warn_base = int(base_stats.get("vgd_ignored_warnings", 0))
	_p1_non_main_base = int(base_stats.get("non_main_view_requests", 0))

	_phase_root = Node3D.new()
	_phase_root.name = "Phase1RoutingAndProxy"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 6.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, 0.0), Vector3.UP)
	cam.current = true
	_phase_root.add_child(cam)

	var box_arrays := _make_closed_box_arrays(0.4, false, true)

	# 1. Empty ShaderMaterial next_pass -> still virtual/NONE.
	var mat_empty_np := StandardMaterial3D.new()
	mat_empty_np.albedo_color = Color(0.3, 0.8, 0.4)
	mat_empty_np.next_pass = ShaderMaterial.new() # null shader!
	_p1_valid_empty_np = MeshInstance3D.new()
	_p1_valid_empty_np.mesh = _build_vmesh(box_arrays, mat_empty_np, "empty_np")
	_p1_valid_empty_np.position = Vector3(-2.4, 0.0, 0.0)
	_phase_root.add_child(_p1_valid_empty_np)

	# 2. Valid StandardMaterial3D next_pass -> legacy/NEXT_PASS_OR_OVERLAY.
	var mat_valid_np := StandardMaterial3D.new()
	mat_valid_np.albedo_color = Color(0.8, 0.4, 0.3)
	var shell_np := StandardMaterial3D.new()
	shell_np.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	shell_np.albedo_color = Color(1.0, 1.0, 0.4, 0.4)
	mat_valid_np.next_pass = shell_np
	_p1_blocked_np = MeshInstance3D.new()
	_p1_blocked_np.mesh = _build_vmesh(box_arrays, mat_valid_np, "blocked_np")
	_p1_blocked_np.position = Vector3(-1.2, 0.0, 0.0)
	_phase_root.add_child(_p1_blocked_np)

	# 3. Empty ShaderMaterial material_overlay -> still virtual/NONE.
	var mat_plain := StandardMaterial3D.new()
	mat_plain.albedo_color = Color(0.3, 0.6, 0.9)
	_p1_valid_empty_ov = MeshInstance3D.new()
	_p1_valid_empty_ov.mesh = _build_vmesh(box_arrays, mat_plain, "empty_ov")
	_p1_valid_empty_ov.material_overlay = ShaderMaterial.new() # null shader!
	_p1_valid_empty_ov.position = Vector3(0.0, 0.0, 0.0)
	_phase_root.add_child(_p1_valid_empty_ov)

	# 4. Valid StandardMaterial3D material_overlay -> legacy/NEXT_PASS_OR_OVERLAY.
	_p1_blocked_ov = MeshInstance3D.new()
	_p1_blocked_ov.mesh = _build_vmesh(box_arrays, mat_plain, "blocked_ov")
	_p1_blocked_ov.material_overlay = shell_np
	_p1_blocked_ov.position = Vector3(1.2, 0.0, 0.0)
	_phase_root.add_child(_p1_blocked_ov)

	# 5. Stencil enabled -> legacy/NEXT_PASS_OR_OVERLAY.
	var mat_stencil := StandardMaterial3D.new()
	mat_stencil.stencil_mode = BaseMaterial3D.STENCIL_MODE_CUSTOM
	mat_stencil.stencil_flags = BaseMaterial3D.STENCIL_FLAG_READ
	mat_stencil.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	_p1_stencil = MeshInstance3D.new()
	_p1_stencil.mesh = _build_vmesh(box_arrays, mat_stencil, "stencil")
	_p1_stencil.position = Vector3(2.4, 0.0, 0.0)
	_phase_root.add_child(_p1_stencil)

	# 6. MultiMeshInstance3D (MULTIMESH_OR_PARTICLES) vs internal MultiMesh (VGD_IGNORED) + octant & baked GridMap.
	var shared_vmesh := _build_vmesh(box_arrays, mat_plain, "mm_vmesh")
	_p1_uv2_mesh = shared_vmesh
	_p1_uv2_mat = mat_plain

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.instance_count = 1
	mm.mesh = shared_vmesh
	mm.set_instance_transform(0, Transform3D(Basis(), Vector3(-2.0, -1.3, 0.0)))
	_p1_mmi = MultiMeshInstance3D.new()
	_p1_mmi.multimesh = mm
	_phase_root.add_child(_p1_mmi)

	var raw_mm := RenderingServer.multimesh_create()
	_temp_rids.append(raw_mm)
	RenderingServer.multimesh_allocate_data(raw_mm, 1, RenderingServer.MULTIMESH_TRANSFORM_3D)
	RenderingServer.multimesh_set_mesh(raw_mm, shared_vmesh.get_rid())
	RenderingServer.multimesh_instance_set_transform(raw_mm, 0, Transform3D(Basis(), Vector3(-0.8, -1.3, 0.0)))
	_p1_internal_mm_inst = RenderingServer.instance_create2(raw_mm, get_world_3d().scenario)
	_temp_rids.append(_p1_internal_mm_inst)

	var mlib := MeshLibrary.new()
	mlib.create_item(0)
	mlib.set_item_mesh(0, shared_vmesh)
	var oct_gm := GridMap.new()
	oct_gm.mesh_library = mlib
	oct_gm.cell_size = Vector3(1.0, 1.0, 1.0)
	oct_gm.set_cell_item(Vector3i(0, 0, 0), 0)
	oct_gm.position = Vector3(0.4, -1.3, 0.0)
	_phase_root.add_child(oct_gm)

	_p1_baked_gm = GridMap.new()
	_p1_baked_gm.mesh_library = mlib
	_p1_baked_gm.cell_size = Vector3(1.0, 1.0, 1.0)
	_p1_baked_gm.set_cell_item(Vector3i(0, 0, 0), 0)
	_p1_baked_gm.position = Vector3(1.8, -1.3, 0.0)
	_phase_root.add_child(_p1_baked_gm)
	_p1_baked_gm.make_baked_meshes()

	# 7. Auxiliary proxy consumers: ReflectionProbe + GPUParticlesCollisionHeightField3D.
	var rprobe := ReflectionProbe.new()
	rprobe.size = Vector3(8.0, 6.0, 8.0)
	rprobe.update_mode = ReflectionProbe.UPDATE_ALWAYS
	_phase_root.add_child(rprobe)

	var hf := GPUParticlesCollisionHeightField3D.new()
	hf.size = Vector3(8.0, 6.0, 8.0)
	hf.update_mode = GPUParticlesCollisionHeightField3D.UPDATE_MODE_ALWAYS
	_phase_root.add_child(hf)

	_phase = "p1"


func _assert_surface(p_rid: RID, p_exp_route: String, p_exp_reason: String, p_ctx: String) -> void:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_rid)
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


func _tick_p1() -> void:
	if _phase_frame < 6:
		return
	_assert_surface(_p1_valid_empty_np.get_instance(), "virtual", "NONE", "p1/empty_shader_next_pass")
	_assert_surface(_p1_blocked_np.get_instance(), "legacy", "NEXT_PASS_OR_OVERLAY", "p1/valid_next_pass")
	_assert_surface(_p1_valid_empty_ov.get_instance(), "virtual", "NONE", "p1/empty_shader_overlay")
	_assert_surface(_p1_blocked_ov.get_instance(), "legacy", "NEXT_PASS_OR_OVERLAY", "p1/valid_overlay")
	_assert_surface(_p1_stencil.get_instance(), "legacy", "NEXT_PASS_OR_OVERLAY", "p1/stencil")
	_assert_surface(_p1_mmi.get_instance(), "legacy", "MULTIMESH_OR_PARTICLES", "p1/multimesh_instance")
	_assert_surface(_p1_internal_mm_inst, "legacy", "VGD_IGNORED", "p1/internal_multimesh_vgd_ignored")

	if _p1_baked_gm.get_bake_meshes().size() != 2:
		_failures.append("p1/baked_gridmap: expected 1 baked mesh pair")
	else:
		_assert_surface(_p1_baked_gm.get_bake_mesh_instance(0), "legacy", "BAKED_GRIDMAP", "p1/baked_gridmap")

	# Exercise bake_render_uv2 -> must use authored surfaces (uv2_authored_draws > 0, uv2_proxy_draws == 0).
	var pre_uv2_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var uv2_base := int(pre_uv2_stats.get("uv2_authored_draws", 0))
	var mat_overrides: Array[RID] = [_p1_uv2_mat.get_rid()]
	var baked_imgs := RenderingServer.bake_render_uv2(_p1_uv2_mesh.get_rid(), mat_overrides, Vector2i(64, 64))
	if baked_imgs.is_empty():
		_failures.append("p1/bake_render_uv2: returned empty image array")

	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var aux_delta := int(vp_stats.get("auxiliary_proxy_draws", 0)) - _p1_aux_base
	var aux_auth := int(vp_stats.get("auxiliary_authored_virtual_draws", -1))
	var vgd_warn_delta := int(vp_stats.get("vgd_ignored_warnings", 0)) - _p1_vgd_warn_base
	var non_main_delta := int(vp_stats.get("non_main_view_requests", 0)) - _p1_non_main_base
	var uv2_delta := int(vp_stats.get("uv2_authored_draws", 0)) - uv2_base
	var uv2_proxy := int(vp_stats.get("uv2_proxy_draws", -1))

	if aux_delta <= 0:
		_failures.append("p1: expected auxiliary_proxy_draws > 0, delta=%d" % aux_delta)
	if aux_auth != 0:
		_failures.append("p1: expected auxiliary_authored_virtual_draws == 0, got %d" % aux_auth)
	if vgd_warn_delta <= 0:
		_failures.append("p1: expected vgd_ignored_warnings > 0, delta=%d" % vgd_warn_delta)
	if non_main_delta != 0:
		_failures.append("p1: expected 0 non_main_view_requests from auxiliary views, delta=%d" % non_main_delta)
	if uv2_delta <= 0:
		_failures.append("p1: expected uv2_authored_draws > 0 after bake_render_uv2, delta=%d" % uv2_delta)
	if uv2_proxy != 0:
		_failures.append("p1: expected uv2_proxy_draws == 0, got %d" % uv2_proxy)
	if int(vp_stats.get("double_dispatch", -1)) != 0 or int(vp_stats.get("missing", -1)) != 0:
		_failures.append("p1: double_dispatch=%s missing=%s" % [str(vp_stats.get("double_dispatch")), str(vp_stats.get("missing"))])

	_summary["p1"] = {
		"auxiliary_proxy_draws": aux_delta,
		"vgd_ignored_warnings": vgd_warn_delta,
		"uv2_authored_draws": uv2_delta,
	}
	_start_phase_2()


# ---------------------------------------------------------------------------
# Phase 2: All 7 NextGeo debug-draw modes + MSAA debug-draw + histogram
# ---------------------------------------------------------------------------

func _start_phase_2() -> void:
	_clear_phase_root()
	_phase_root = Node3D.new()
	_phase_root.name = "Phase2DebugViews"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.8, 3.2)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, 0.0), Vector3.UP)
	cam.current = true
	_phase_root.add_child(cam)

	var box_arrays := _make_closed_box_arrays(0.55, false, false)
	var mat_v := StandardMaterial3D.new()
	mat_v.albedo_color = Color(0.4, 0.75, 0.5)
	var v_inst := MeshInstance3D.new()
	v_inst.mesh = _build_vmesh(box_arrays, mat_v, "p2_virt")
	v_inst.position = Vector3(-0.75, 0.0, 0.0)
	_phase_root.add_child(v_inst)

	var mat_fb := StandardMaterial3D.new()
	mat_fb.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
	mat_fb.albedo_color = Color(0.85, 0.5, 0.35)
	var fb_inst := MeshInstance3D.new()
	fb_inst.mesh = _build_vmesh(box_arrays, mat_fb, "p2_fb")
	fb_inst.position = Vector3(0.75, 0.0, 0.0)
	_phase_root.add_child(fb_inst)

	# MSAA 2x SubViewport rendering NEXTGEO_RT_PROXY / NEXTGEO_FALLBACK to exercise 1x depth paths.
	_p2_msaa_vp = SubViewport.new()
	_p2_msaa_vp.own_world_3d = true
	_p2_msaa_vp.size = Vector2i(128, 128)
	_p2_msaa_vp.msaa_3d = Viewport.MSAA_2X
	_p2_msaa_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_p2_msaa_vp.debug_draw = Viewport.DEBUG_DRAW_NEXTGEO_RT_PROXY
	_phase_root.add_child(_p2_msaa_vp)
	var sub_cam := Camera3D.new()
	sub_cam.position = Vector3(0.0, 0.4, 2.2)
	sub_cam.look_at_from_position(sub_cam.position, Vector3.ZERO, Vector3.UP)
	sub_cam.current = true
	_p2_msaa_vp.add_child(sub_cam)
	var sub_box := MeshInstance3D.new()
	sub_box.mesh = _build_vmesh(box_arrays, mat_v, "p2_msaa_box")
	_p2_msaa_vp.add_child(sub_box)

	_p2_modes = [
		{"name": "CRACKS", "mode": Viewport.DEBUG_DRAW_NEXTGEO_CRACKS},
		{"name": "RT_PROXY", "mode": Viewport.DEBUG_DRAW_NEXTGEO_RT_PROXY},
		{"name": "CLUSTER_ERROR", "mode": Viewport.DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR},
		{"name": "RESIDENCY", "mode": Viewport.DEBUG_DRAW_NEXTGEO_RESIDENCY},
		{"name": "VISIBILITY_ID", "mode": Viewport.DEBUG_DRAW_NEXTGEO_VISIBILITY_ID},
		{"name": "FALLBACK", "mode": Viewport.DEBUG_DRAW_NEXTGEO_FALLBACK},
		{"name": "TRIANGLE_SIZE", "mode": Viewport.DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE},
	]
	_p2_idx = 0
	get_viewport().debug_draw = int(_p2_modes[0]["mode"])
	_phase = "p2"


func _tick_p2() -> void:
	if _phase_frame == 3 and _p2_msaa_vp != null:
		_p2_msaa_vp.debug_draw = Viewport.DEBUG_DRAW_NEXTGEO_FALLBACK
	if _phase_frame < 6:
		return

	var entry := _p2_modes[_p2_idx]
	var mname := String(entry["name"])
	var vp := get_viewport()
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(vp.get_viewport_rid())
	if String(st.get("view_route", "")) != "virtual" or String(st.get("view_reason", "")) != "NONE":
		_failures.append("p2/%s: expected view_route=virtual/NONE, got %s/%s" % [
			mname, String(st.get("view_route", "")), String(st.get("view_reason", ""))])
	if int(st.get("double_dispatch", -1)) != 0 or int(st.get("missing", -1)) != 0:
		_failures.append("p2/%s: double_dispatch=%s missing=%s" % [
			mname, str(st.get("double_dispatch")), str(st.get("missing"))])

	var img := vp.get_texture().get_image()
	if img == null or img.is_empty():
		_failures.append("p2/%s: empty viewport image" % mname)
	else:
		var non_black := 0
		for sy in 12:
			for sx in 12:
				var px := img.get_pixel(
					int((float(sx) + 0.5) * float(img.get_width()) / 12.0),
					int((float(sy) + 0.5) * float(img.get_height()) / 12.0)
				)
				if not is_finite(px.r) or not is_finite(px.g) or not is_finite(px.b):
					_failures.append("p2/%s: non-finite pixel at (%d,%d)" % [mname, sx, sy])
				if px.r + px.g + px.b > 0.01:
					non_black += 1
		if non_black == 0:
			_failures.append("p2/%s: debug view is completely black" % mname)

	if mname == "TRIANGLE_SIZE":
		var hist: Array = st.get("triangle_size_histogram", [])
		var hist_sum := 0
		for v in hist:
			hist_sum += int(v)
		if hist.size() != 16 or hist_sum <= 0:
			_failures.append("p2/TRIANGLE_SIZE: expected 16-bin positive histogram, got size=%d sum=%d (%s)" % [
				hist.size(), hist_sum, str(hist)])
		_summary["p2_triangle_hist_sum"] = hist_sum

	_p2_idx += 1
	if _p2_idx >= _p2_modes.size():
		_p2_msaa_vp = null
		_start_phase_3()
		return
	vp.debug_draw = int(_p2_modes[_p2_idx]["mode"])
	_phase_frame = 0


# ---------------------------------------------------------------------------
# Phase 3: NEXTGEO_CRACKS exact (255, 0, 255) magenta check
# ---------------------------------------------------------------------------

func _start_phase_3() -> void:
	_p3_step = 0
	_build_p3_scene(false)
	_phase = "p3"


func _build_p3_scene(p_cracked: bool) -> void:
	_clear_phase_root()
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGEO_CRACKS

	_phase_root = Node3D.new()
	_phase_root.name = "Phase3Cracks"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root, Color(0.0, 0.0, 0.0))

	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 3.2
	cam.near = 0.2
	cam.far = 20.0
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.current = true
	_phase_root.add_child(cam)

	var mat := StandardMaterial3D.new()
	mat.cull_mode = BaseMaterial3D.CULL_BACK
	mat.albedo_color = Color(0.6, 0.6, 0.6)

	var arrays := _make_closed_box_arrays(0.55, p_cracked, false)

	# Left half (x < 0): virtual closed mesh.
	var virt := MeshInstance3D.new()
	virt.mesh = _build_vmesh(arrays, mat, "p3_cracked" if p_cracked else "p3_closed")
	virt.position = Vector3(-0.75, 0.0, 0.0)
	virt.rotation_degrees = Vector3(14.0, -18.0, 0.0)
	_phase_root.add_child(virt)

	# Right half (x > 0): legacy closed mesh (no VGD).
	var leg_mesh := ArrayMesh.new()
	leg_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	leg_mesh.surface_set_material(0, mat)
	var leg := MeshInstance3D.new()
	leg.mesh = leg_mesh
	leg.position = Vector3(0.75, 0.0, 0.0)
	leg.rotation_degrees = Vector3(14.0, -18.0, 0.0)
	_phase_root.add_child(leg)


func _count_magenta_halves(p_img: Image) -> Vector2i:
	var w := p_img.get_width()
	var h := p_img.get_height()
	var mid_x := w / 2
	var left_mag := 0
	var right_mag := 0
	for y in range(0, h, 2):
		for x in range(0, w, 2):
			var r8 := p_img.get_pixel(x, y).r8
			var g8 := p_img.get_pixel(x, y).g8
			var b8 := p_img.get_pixel(x, y).b8
			if r8 == 255 and g8 == 0 and b8 == 255:
				if x < mid_x:
					left_mag += 1
				else:
					right_mag += 1
	return Vector2i(left_mag, right_mag)


func _tick_p3() -> void:
	if _phase_frame < 5:
		return
	var img := get_viewport().get_texture().get_image()
	if img == null or img.is_empty():
		_failures.append("p3/step%d: empty image" % _p3_step)
	else:
		var halves := _count_magenta_halves(img)
		if _p3_step == 0:
			if halves.x != 0 or halves.y != 0:
				_failures.append("p3/closed: expected 0 magenta pixels on watertight closed meshes, got left=%d right=%d" % [halves.x, halves.y])
			_summary["p3_closed_magenta"] = [halves.x, halves.y]
		else:
			if halves.x <= 0 or halves.y <= 0:
				_failures.append("p3/cracked: expected > 0 magenta pixels on BOTH virtual (left=%d) and legacy (right=%d) cracked closed cubes" % [halves.x, halves.y])
			_summary["p3_cracked_magenta"] = [halves.x, halves.y]

	if _p3_step == 0:
		_p3_step = 1
		_build_p3_scene(true)
	else:
		_start_phase_4()


# ---------------------------------------------------------------------------
# Phase 4: NEXTGEO_FALLBACK Q14 fallback_coverage readback
# ---------------------------------------------------------------------------

func _start_phase_4() -> void:
	_clear_phase_root()
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGEO_FALLBACK

	_phase_root = Node3D.new()
	_phase_root.name = "Phase4Q14"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root)

	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 3.4
	cam.near = 0.2
	cam.far = 20.0
	cam.position = Vector3(0.0, 0.0, 4.0)
	cam.current = true
	_phase_root.add_child(cam)

	var arrays := _make_closed_box_arrays(0.55, false, false)
	var mat_v := StandardMaterial3D.new()
	mat_v.albedo_color = Color(0.3, 0.8, 0.5)

	# Virtual VGD mesh on the left.
	var virt := MeshInstance3D.new()
	virt.mesh = _build_vmesh(arrays, mat_v, "p4_virt")
	virt.position = Vector3(-0.75, 0.0, 0.0)
	_phase_root.add_child(virt)

	# Non-VGD legacy backdrop at z = -1.5 (must count toward opaque_pixels, NOT legacy_drawn_virtualized_pixels!).
	var bg_quad := QuadMesh.new()
	bg_quad.size = Vector2(2.8, 2.2)
	bg_quad.material = mat_v
	var leg_bg := MeshInstance3D.new()
	leg_bg.mesh = bg_quad
	leg_bg.position = Vector3(0.0, 0.0, -1.5)
	_phase_root.add_child(leg_bg)

	# Fallback VGD mesh on the right, initially hidden in step 0!
	var mat_fb := StandardMaterial3D.new()
	mat_fb.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
	mat_fb.albedo_color = Color(0.9, 0.4, 0.3)
	_p4_fallback_inst = MeshInstance3D.new()
	_p4_fallback_inst.mesh = _build_vmesh(arrays, mat_fb, "p4_fb")
	_p4_fallback_inst.position = Vector3(0.75, 0.0, 0.0)
	_p4_fallback_inst.visible = false
	_phase_root.add_child(_p4_fallback_inst)

	_p4_step = 0
	_phase = "p4"


func _tick_p4() -> void:
	# Wait 7 frames so 3-slot async readback reflects the steady-state scene.
	if _phase_frame < 7:
		return

	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var q14 := float(st.get("fallback_coverage", -1.0))
	var leg_px := int(st.get("legacy_drawn_virtualized_pixels", -1))
	var opq_px := int(st.get("opaque_pixels", 0))

	if _p4_step == 0:
		if opq_px <= 0 or leg_px != 0 or not is_equal_approx(q14, 0.0):
			_failures.append("p4/zero_fallback: expected opaque>0, legacy_vgd=0, q14=0.0; got opaque=%d legacy_vgd=%d q14=%f" % [
				opq_px, leg_px, q14])
		_summary["p4_zero_q14"] = {"opaque": opq_px, "legacy_vgd": leg_px, "q14": q14}
		_p4_fallback_inst.visible = true
		_p4_step = 1
		_phase_frame = 0
	else:
		if opq_px <= 0 or leg_px <= 0 or q14 <= 0.0 or q14 >= 1.0:
			_failures.append("p4/partial_fallback: expected 0 < q14 < 1 with legacy_vgd > 0; got opaque=%d legacy_vgd=%d q14=%f" % [
				opq_px, leg_px, q14])
		_summary["p4_partial_q14"] = {"opaque": opq_px, "legacy_vgd": leg_px, "q14": q14}
		_finish()


func _finish() -> void:
	set_process(false)
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	_p4_fallback_inst = null
	_clear_phase_root()
	for p in _temp_files:
		var abs_p := ProjectSettings.globalize_path(p)
		if FileAccess.file_exists(abs_p):
			DirAccess.remove_absolute(abs_p)
	_temp_files.clear()

	if _failures.is_empty():
		print("NEXTGEO_ROUTING PASS: " + JSON.stringify(_summary))
		get_tree().quit(0)
	else:
		for f in _failures:
			printerr("NEXTGEO_ROUTING FAIL: " + f)
		get_tree().quit(1)
