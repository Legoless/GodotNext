extends Node3D
## NextGeo material-bucket resolve end-to-end GPU check (#80):
## 1. Stage A normal-roughness (<= 1 LSB vs Forward+) + instance-ID parity on
##    static and moving instances at the finest cut.
## 2. Feature Parity Room with visibility_only = false across cameras and cuts:
##    Stage A/B active, per-surface fallback reasons, 0 double-drawn / 0 missing.
## 3. Exclusion Matrix all 22 cells + frame-60 mesh mutation fallback.
## 4. Virtual-only StreamedTexture2D (.stex) mip convergence matching Forward+
##    within <= 120 frames via per-bucket material_feedback_index.

const ProbeScript = preload("res://tests/nextgeo_resolve/nextgeo_resolve_probe.gd")
const ExclusionRules = preload("res://scenes/exclusion_matrix/exclusion_rules.gd")

const SENTINEL_U32 := 0xFFFFFFFF
const REEXEC_ARG := "--nextgeo-resolve-reexec"
const OVERRIDE_SRC := "res://tests/nextgeo_resolve/override.cfg"
const OVERRIDE_DST := "res://override.cfg"

var _phase := ""
var _phase_frame := 0
var _phase_root: Node3D
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

# Phase 1 state
var _p1_cam: Camera3D
var _p1_virt: MeshInstance3D
var _p1_leg: MeshInstance3D
var _p1_probe: CompositorEffect

# Phase 2 state
var _p2_room: Node3D
var _p2_steps: Array[Dictionary] = []
var _p2_step_idx := 0

# Phase 3 state
var _p3_matrix: Node3D
var _p3_pre_checked := false

# Phase 4 state
var _p4_cam: Camera3D
var _p4_virt: MeshInstance3D
var _p4_fwd: MeshInstance3D
var _p4_stex_virt: StreamedTexture2D
var _p4_stex_fwd: StreamedTexture2D
var _p4_init_virt_w := 0
var _p4_init_fwd_w := 0


func _ready() -> void:
	if not bool(ProjectSettings.get_setting("rendering/textures/streaming/enabled", false)):
		_reexec_with_streaming_override()
		return

	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)

	_start_phase_1()


func _reexec_with_streaming_override() -> void:
	if REEXEC_ARG in OS.get_cmdline_user_args():
		printerr("NEXTGEO_RESOLVE FAIL: rendering/textures/streaming/enabled remained false after override re-exec.")
		get_tree().quit(1)
		return

	var dst_abs := ProjectSettings.globalize_path(OVERRIDE_DST)
	if FileAccess.file_exists(dst_abs):
		printerr("NEXTGEO_RESOLVE FAIL: %s already exists without streaming enabled." % dst_abs)
		get_tree().quit(1)
		return

	var cfg_bytes := FileAccess.get_file_as_bytes(OVERRIDE_SRC)
	if cfg_bytes.is_empty():
		printerr("NEXTGEO_RESOLVE FAIL: missing %s." % OVERRIDE_SRC)
		get_tree().quit(1)
		return

	var out_file := FileAccess.open(dst_abs, FileAccess.WRITE)
	if out_file == null:
		printerr("NEXTGEO_RESOLVE FAIL: cannot write temporary %s." % dst_abs)
		get_tree().quit(1)
		return
	out_file.store_buffer(cfg_bytes)
	out_file.close()

	var driver := RenderingServer.get_current_rendering_driver_name().to_lower()
	var args := PackedStringArray([
		"--path", ProjectSettings.globalize_path("res://"),
		"--rendering-driver", driver,
		"res://tests/nextgeo_resolve/nextgeo_resolve_check.tscn",
		"--",
		REEXEC_ARG,
	])
	var output := []
	var code := OS.execute(OS.get_executable_path(), args, output, true)
	DirAccess.remove_absolute(dst_abs)
	for chunk in output:
		print(String(chunk).strip_edges(false, true))
	set_process(false)
	get_tree().quit(0 if code == 0 else 1)


func _alloc_temp_path(p_prefix: String, p_ext: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_resolve_%s_%d.%s" % [p_prefix, _ngm_counter, p_ext]
	_temp_files.append(path)
	return path


func _clear_phase_root() -> void:
	if _phase_root != null:
		remove_child(_phase_root)
		_phase_root.free()
		_phase_root = null
	_phase_frame = 0


func _process(_delta: float) -> void:
	_phase_frame += 1
	match _phase:
		"p1_static":
			_tick_p1_static()
		"p1_moving":
			_tick_p1_moving()
		"p2_fpr":
			_tick_p2_fpr()
		"p3_exclusion":
			_tick_p3_exclusion()
		"p4_streaming":
			_tick_p4_streaming()


# ---------------------------------------------------------------------------
# Phase 1: Stage A normal-roughness (<= 1 LSB) + instance_id (static & moving)
# ---------------------------------------------------------------------------

func _make_test_normal_texture() -> ImageTexture:
	var img := Image.create_empty(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var u := (float(x) + 0.5) / 64.0 - 0.5
			var v := (float(y) + 0.5) / 64.0 - 0.5
			var n := Vector3(0.22 * cos(u * PI), -0.18 * cos(v * PI), 1.0).normalized()
			img.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0))
	img.generate_mipmaps(true)
	return ImageTexture.create_from_image(img)


func _start_phase_1() -> void:
	_clear_phase_root()
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.0)
	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0

	_phase_root = Node3D.new()
	_phase_root.name = "Phase1Guides"
	add_child(_phase_root)

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.35, 0.68, 0.45)
	mat.roughness = 0.42
	mat.metallic = 0.15
	mat.normal_enabled = true
	mat.normal_scale = 0.65
	mat.normal_texture = _make_test_normal_texture()

	var qmesh := QuadMesh.new()
	qmesh.size = Vector2(1.5, 1.5)
	qmesh.subdivide_width = 8
	qmesh.subdivide_depth = 8
	qmesh.material = mat

	var imp := ImporterMesh.from_mesh(qmesh)
	var err := imp.build_virtual_geometry(_alloc_temp_path("p1_quad", "ngm"))
	var vmesh := imp.get_mesh()
	if err != OK or vmesh == null or vmesh.get_virtual_geometry() == null:
		_failures.append("p1: build_virtual_geometry failed (err=%d)" % err)

	_p1_virt = MeshInstance3D.new()
	_p1_virt.name = "VirtualQuad"
	_p1_virt.mesh = vmesh
	_p1_virt.position = Vector3(-1.05, 0.0, 0.0)
	_p1_virt.rotation_degrees = Vector3(18.0, -22.0, 10.0)
	_phase_root.add_child(_p1_virt)

	_p1_leg = MeshInstance3D.new()
	_p1_leg.name = "LegacyQuad"
	_p1_leg.mesh = qmesh
	_p1_leg.position = Vector3(1.05, 0.0, 0.0)
	_p1_leg.rotation_degrees = Vector3(18.0, -22.0, 10.0)
	_phase_root.add_child(_p1_leg)

	_p1_cam = Camera3D.new()
	_p1_cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	_p1_cam.size = 4.2
	_p1_cam.near = 0.5
	_p1_cam.far = 20.0
	_p1_cam.position = Vector3(0.0, 0.0, 5.0)
	_p1_cam.current = true
	_phase_root.add_child(_p1_cam)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40.0, 28.0, 0.0)
	_phase_root.add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.08, 0.10, 0.12)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.35, 0.40)

	_p1_probe = ProbeScript.new()
	var comp := Compositor.new()
	var effects: Array[CompositorEffect] = [_p1_probe]
	comp.compositor_effects = effects

	var we := WorldEnvironment.new()
	we.environment = env
	we.compositor = comp
	_phase_root.add_child(we)

	_update_p1_probe_uvs()
	_phase = "p1_static"


func _update_p1_probe_uvs() -> void:
	var vp_size := get_viewport().get_visible_rect().size
	if vp_size.x > 1.0 and vp_size.y > 1.0:
		_p1_probe.set("virt_uv", _p1_cam.unproject_position(_p1_virt.global_position) / vp_size)
		_p1_probe.set("leg_uv", _p1_cam.unproject_position(_p1_leg.global_position) / vp_size)


func _tick_p1_static() -> void:
	_update_p1_probe_uvs()
	if _phase_frame < 6:
		return
	_verify_p1_sample("static", false)
	_phase = "p1_moving"
	_phase_frame = 0


func _tick_p1_moving() -> void:
	var dx := float(_phase_frame) * 0.045
	_p1_virt.position = Vector3(-1.15 + dx, 0.02 * float(_phase_frame), 0.0)
	_p1_leg.position = Vector3(0.95 + dx, 0.02 * float(_phase_frame), 0.0)
	_update_p1_probe_uvs()
	if _phase_frame < 5:
		return
	_verify_p1_sample("moving", true)
	_start_phase_2()


func _verify_p1_sample(p_label: String, p_expect_dynamic: bool) -> void:
	var sample: Dictionary = _p1_probe.get("last_sample")
	if sample.is_empty():
		_failures.append("p1_%s: missing GPU probe sample" % p_label)
		return

	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var inst_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_p1_virt.get_instance())
	var rec_idx := int(inst_stats.get("record_index", -1))

	if int(vp_stats.get("resolve_stage_a_draws", 0)) < 1:
		_failures.append("p1_%s: expected resolve_stage_a_draws >= 1, got %s" % [p_label, str(vp_stats.get("resolve_stage_a_draws"))])
	if int(vp_stats.get("resolve_stage_b_draws", 0)) < 1 or not bool(vp_stats.get("resolve_stage_b_wrote", false)):
		_failures.append("p1_%s: Stage B did not draw (stats=%s)" % [p_label, JSON.stringify(vp_stats)])

	var virt_nr: Array = sample["virt_nr"]
	var leg_nr: Array = sample["leg_nr"]
	var max_diff := 0
	for c in 4:
		max_diff = maxi(max_diff, absi(int(virt_nr[c]) - int(leg_nr[c])))
	if max_diff > 1:
		_failures.append("p1_%s: normal_roughness differed by %d LSB (> 1): virt=%s leg=%s" % [
			p_label, max_diff, str(virt_nr), str(leg_nr)])

	if p_expect_dynamic:
		if int(virt_nr[3]) < 128 or int(leg_nr[3]) < 128:
			_failures.append("p1_%s: expected dynamic high-bit roughness (>= 128), got virt=%d leg=%d" % [
				p_label, int(virt_nr[3]), int(leg_nr[3])])
	else:
		if int(virt_nr[3]) >= 128 or int(leg_nr[3]) >= 128:
			_failures.append("p1_%s: expected static roughness (< 128), got virt=%d leg=%d" % [
				p_label, int(virt_nr[3]), int(leg_nr[3])])

	var virt_inst := int(sample["virt_inst"])
	var leg_inst := int(sample["leg_inst"])
	var bg_inst := int(sample["bg_inst"])
	var virt_vis := int(sample["virt_vis"])
	if virt_vis == SENTINEL_U32:
		_failures.append("p1_%s: virtual quad visibility stayed 0xFFFFFFFF" % p_label)
	if rec_idx < 0 or virt_inst != rec_idx:
		_failures.append("p1_%s: virt_inst=%d expected record_index=%d" % [p_label, virt_inst, rec_idx])
	if leg_inst != SENTINEL_U32 or bg_inst != SENTINEL_U32:
		_failures.append("p1_%s: expected leg/bg instance_id == 0xFFFFFFFF, got (%d, %d)" % [p_label, leg_inst, bg_inst])

	_summary["p1_" + p_label] = {
		"virt_nr": virt_nr,
		"leg_nr": leg_nr,
		"max_lsb_diff": max_diff,
		"virt_inst": virt_inst,
	}


# ---------------------------------------------------------------------------
# Phase 2: Feature Parity Room (default & finest cuts, Stage A/B + fallbacks)
# ---------------------------------------------------------------------------

func _start_phase_2() -> void:
	_p1_probe = null
	_clear_phase_root()
	_phase_root = Node3D.new()
	_phase_root.name = "Phase2FeatureParityRoom"
	add_child(_phase_root)

	var packed := load("res://scenes/feature_parity_room/feature_parity_room.tscn") as PackedScene
	if packed == null:
		_failures.append("p2: cannot load feature_parity_room.tscn")
		_start_phase_3()
		return

	_p2_room = packed.instantiate() as Node3D
	_virtualize_subtree_meshes(_p2_room, false)
	_phase_root.add_child(_p2_room)

	_p2_steps = [
		{"cam": "cam_0", "err": 1.0, "lightmapped": true},
		{"cam": "cam_0", "err": 1.0, "lightmapped": false},
		{"cam": "cam_1", "err": 1.0, "lightmapped": false},
		{"cam": "cam_2", "err": 1.0, "lightmapped": false},
		{"cam": "cam_0", "err": 0.0, "lightmapped": false},
	]
	_p2_step_idx = 0
	_apply_p2_step(0)
	_phase = "p2_fpr"


func _virtualize_subtree_meshes(p_node: Node, p_force_vgd_on_base_fallbacks: bool) -> void:
	if p_node is MeshInstance3D:
		var mi := p_node as MeshInstance3D
		if mi.mesh != null and mi.mesh.get_surface_count() > 0:
			var skinned := mi.skin != null
			var vmesh := _build_virtual_mesh(mi.mesh, String(mi.name), skinned, p_force_vgd_on_base_fallbacks)
			if vmesh != null:
				mi.mesh = vmesh
	for child in p_node.get_children():
		_virtualize_subtree_meshes(child, p_force_vgd_on_base_fallbacks)


func _build_virtual_mesh(p_mesh: Mesh, p_tag: String, p_skinned: bool, p_force_vgd_on_base_fallbacks: bool) -> ArrayMesh:
	var surf_count := p_mesh.get_surface_count()
	var orig_mats: Array[Material] = []
	for s in surf_count:
		orig_mats.append(p_mesh.surface_get_material(s))

	var imp := ImporterMesh.from_mesh(p_mesh)
	var stripped_single := false
	if p_force_vgd_on_base_fallbacks and surf_count == 1 and orig_mats[0] is BaseMaterial3D:
		imp.set_surface_material(0, null)
		stripped_single = true

	var clean_tag := p_tag.validate_filename()
	var err := imp.build_virtual_geometry(_alloc_temp_path("m_" + clean_tag, "ngm"), p_skinned)
	if err != OK:
		_failures.append("build_virtual_geometry failed for %s (err=%d)" % [p_tag, err])
		return null
	var vmesh := imp.get_mesh()
	if vmesh != null and stripped_single and orig_mats[0] != null:
		vmesh.surface_set_material(0, orig_mats[0])
	return vmesh


func _apply_p2_step(p_idx: int) -> void:
	_phase_frame = 0
	var step := _p2_steps[p_idx]
	if not bool(step["lightmapped"]):
		var lm := _p2_room.get_node_or_null("LightmapGI") as LightmapGI
		if lm != null and lm.light_data != null:
			lm.light_data = null
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", float(step["err"]))
	for cname in ["cam_0", "cam_1", "cam_2"]:
		var c := _p2_room.get_node_or_null(cname) as Camera3D
		if c != null:
			c.current = (cname == String(step["cam"]))


func _tick_p2_fpr() -> void:
	if _phase_frame < 4:
		return
	var step := _p2_steps[_p2_step_idx]
	var lm_tag := "lm" if bool(step["lightmapped"]) else "dyn"
	var tag := "%s_%s_err%.1f" % [lm_tag, String(step["cam"]), float(step["err"])]
	_verify_p2_step(tag, bool(step["lightmapped"]))
	_p2_step_idx += 1
	if _p2_step_idx >= _p2_steps.size():
		_start_phase_3()
		return
	_apply_p2_step(_p2_step_idx)


func _verify_p2_step(p_tag: String, p_lightmapped: bool) -> void:
	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	if int(vp_stats.get("double_dispatch", -1)) != 0:
		_failures.append("p2_%s: double_dispatch=%s" % [p_tag, str(vp_stats.get("double_dispatch"))])
	if int(vp_stats.get("missing", -1)) != 0:
		_failures.append("p2_%s: missing=%s" % [p_tag, str(vp_stats.get("missing"))])
	var min_buckets := 1 if p_lightmapped else 7
	if int(vp_stats.get("resolve_active_buckets", 0)) < min_buckets:
		_failures.append("p2_%s: resolve_active_buckets=%s (< %d)" % [p_tag, str(vp_stats.get("resolve_active_buckets")), min_buckets])
	if int(vp_stats.get("resolve_stage_a_draws", 0)) < min_buckets:
		_failures.append("p2_%s: resolve_stage_a_draws=%s (< %d)" % [p_tag, str(vp_stats.get("resolve_stage_a_draws")), min_buckets])
	if int(vp_stats.get("resolve_stage_b_draws", 0)) < min_buckets or not bool(vp_stats.get("resolve_stage_b_wrote", false)):
		_failures.append("p2_%s: Stage B did not draw (%s)" % [p_tag, JSON.stringify(vp_stats)])

	_assert_instance_surface(_p2_room.get_node_or_null("SHReceiver") as VisualInstance3D, 0, "virtual", "NONE", "p2_%s/SHReceiver" % p_tag)

	var static_features := [
		"Floor", "MetalPanel", "TriplanarBlock", "AnisotropyBlock",
		"ClearcoatBlock", "RimBlock", "ShaderBlock", "Layer2Block",
	]
	for node_name in static_features:
		var exp_r := "legacy" if p_lightmapped else "virtual"
		var exp_rsn := "LIGHTMAPPED_INSTANCE" if p_lightmapped else "NONE"
		_assert_instance_surface(_p2_room.get_node_or_null(node_name) as VisualInstance3D, 0, exp_r, exp_rsn, "p2_%s/%s" % [p_tag, node_name])

	if p_lightmapped:
		_assert_instance_surface(_p2_room.get_node_or_null("fallback_per_vertex") as VisualInstance3D, 0, "legacy", "LIGHTMAPPED_INSTANCE", "p2_%s/fallback_per_vertex" % p_tag)
		_assert_instance_surface(_p2_room.get_node_or_null("fallback_next_pass") as VisualInstance3D, 0, "legacy", "LIGHTMAPPED_INSTANCE", "p2_%s/fallback_next_pass" % p_tag)
		_assert_instance_surface(_p2_room.get_node_or_null("fallback_overlay") as VisualInstance3D, 0, "legacy", "LIGHTMAPPED_INSTANCE", "p2_%s/fallback_overlay" % p_tag)
	else:
		_assert_instance_surface(_p2_room.get_node_or_null("fallback_per_vertex") as VisualInstance3D, 0, "legacy", "VERTEX_LIGHTING", "p2_%s/fallback_per_vertex" % p_tag)
		_assert_instance_surface(_p2_room.get_node_or_null("fallback_next_pass") as VisualInstance3D, 0, "legacy", "NEXT_PASS_OR_OVERLAY", "p2_%s/fallback_next_pass" % p_tag)
		_assert_instance_surface(_p2_room.get_node_or_null("fallback_overlay") as VisualInstance3D, 0, "legacy", "NEXT_PASS_OR_OVERLAY", "p2_%s/fallback_overlay" % p_tag)

	var img := get_viewport().get_texture().get_image()
	if img == null or img.is_empty():
		_failures.append("p2_%s: empty viewport image" % p_tag)
	else:
		var non_black := 0
		for sy in 8:
			for sx in 8:
				var px := img.get_pixel(
					int((float(sx) + 0.5) * float(img.get_width()) / 8.0),
					int((float(sy) + 0.5) * float(img.get_height()) / 8.0)
				)
				if not is_finite(px.r) or not is_finite(px.g) or not is_finite(px.b):
					_failures.append("p2_%s: non-finite pixel at (%d,%d)" % [p_tag, sx, sy])
				if px.r + px.g + px.b > 0.02:
					non_black += 1
		if non_black == 0:
			_failures.append("p2_%s: viewport image is completely black" % p_tag)

	_summary["p2_" + p_tag] = {
		"buckets": int(vp_stats.get("resolve_active_buckets", 0)),
		"stage_a_draws": int(vp_stats.get("resolve_stage_a_draws", 0)),
		"stage_b_draws": int(vp_stats.get("resolve_stage_b_draws", 0)),
	}


func _assert_instance_surface(p_inst: VisualInstance3D, p_surface: int, p_exp_route: String, p_exp_reason: String, p_ctx: String) -> void:
	if p_inst == null:
		_failures.append("%s: missing VisualInstance3D" % p_ctx)
		return
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_inst.get_instance())
	var surfaces: Array = st.get("surfaces", [])
	if p_surface >= surfaces.size():
		_failures.append("%s: surface %d missing in %s" % [p_ctx, p_surface, JSON.stringify(st)])
		return
	var sd: Dictionary = surfaces[p_surface]
	if String(sd.get("route", "")) != p_exp_route or String(sd.get("reason", "")) != p_exp_reason:
		_failures.append("%s: surface %d got (%s, %s), expected (%s, %s)" % [
			p_ctx, p_surface, String(sd.get("route", "")), String(sd.get("reason", "")), p_exp_route, p_exp_reason])


# ---------------------------------------------------------------------------
# Phase 3: Exclusion Matrix (all 22 cells + frame-60 mesh mutation)
# ---------------------------------------------------------------------------

func _find_named_first(p_node: Node, p_name: String) -> Node:
	if p_node == null:
		return null
	if String(p_node.name) == p_name:
		return p_node
	for child in p_node.get_children():
		var found := _find_named_first(child, p_name)
		if found != null:
			return found
	return null


func _baked_mesh_count(p_gridmap: GridMap) -> int:
	if p_gridmap == null:
		return 0
	var val: Variant = p_gridmap.get("baked_meshes")
	if val is Array:
		return (val as Array).size()
	return 0


func _start_phase_3() -> void:
	_p2_room = null
	_clear_phase_root()
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)

	_phase_root = Node3D.new()
	_phase_root.name = "Phase3ExclusionMatrix"
	add_child(_phase_root)

	var packed := load("res://scenes/exclusion_matrix/exclusion_matrix.tscn") as PackedScene
	if packed == null:
		_failures.append("p3: cannot load exclusion_matrix.tscn")
		_start_phase_4()
		return

	_p3_matrix = packed.instantiate() as Node3D
	for cell_val in ExclusionRules.cells():
		var cell: Dictionary = cell_val
		var cid := String(cell["id"])
		var kind := String(cell["kind"])
		var cand := _find_named_first(_p3_matrix.get_node_or_null(cid), "Candidate")
		if cand is MeshInstance3D and kind in ["mesh", "moving"]:
			var mi := cand as MeshInstance3D
			if mi.mesh != null:
				var vmesh := _build_virtual_mesh(mi.mesh, cid, mi.skin != null, true)
				if vmesh != null:
					mi.mesh = vmesh
		elif cand is MultiMeshInstance3D:
			var mmi := cand as MultiMeshInstance3D
			if mmi.multimesh != null and mmi.multimesh.mesh != null:
				var mm_vmesh := _build_virtual_mesh(mmi.multimesh.mesh, cid, false, false)
				if mm_vmesh != null:
					mmi.multimesh.mesh = mm_vmesh
		elif cand is GridMap and kind == "gridmap":
			var gm := cand as GridMap
			if gm.mesh_library != null:
				for item_id in gm.mesh_library.get_item_list():
					var im := gm.mesh_library.get_item_mesh(item_id)
					if im != null:
						var ivm := _build_virtual_mesh(im, "%s_%d" % [cid, item_id], false, false)
						if ivm != null:
							gm.mesh_library.set_item_mesh(item_id, ivm)

	_phase_root.add_child(_p3_matrix)
	var cap_cam := _p3_matrix.get_node_or_null("CaptureCam") as Camera3D
	if cap_cam != null:
		cap_cam.current = true
	_p3_pre_checked = false
	_phase = "p3_exclusion"


func _tick_p3_exclusion() -> void:
	if not _p3_pre_checked and _phase_frame == 6:
		_verify_p3_matrix(false)
		_p3_pre_checked = true
		return
	# When Testbed's /root/Harness autoload is idle (no --mode), exclusion_matrix.gd's
	# _timeline_is_live() stays false; invoke its _mutate() explicitly at MUTATE_FRAME.
	if _phase_frame == ExclusionRules.MUTATE_FRAME and _p3_matrix != null and not bool(_p3_matrix.get("_mutated")):
		_p3_matrix.call("_mutate")
		return
	if _phase_frame >= ExclusionRules.MUTATE_FRAME + 4:
		_verify_p3_matrix(true)
		_start_phase_4()


func _verify_p3_matrix(p_post_mutate: bool) -> void:
	var label := "post60" if p_post_mutate else "pre60"
	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	if int(vp_stats.get("double_dispatch", -1)) != 0:
		_failures.append("p3_%s: double_dispatch=%s" % [label, str(vp_stats.get("double_dispatch"))])
	if int(vp_stats.get("missing", -1)) != 0:
		_failures.append("p3_%s: missing=%s" % [label, str(vp_stats.get("missing"))])

	var verified_cells := 0
	for cell_val in ExclusionRules.cells():
		var cell: Dictionary = cell_val
		var cid := String(cell["id"])
		var kind := String(cell["kind"])
		var surfaces: Array = cell["surfaces"]
		var cand := _find_named_first(_p3_matrix.get_node_or_null(cid), "Candidate")
		if cand == null:
			_failures.append("p3_%s/%s: missing Candidate" % [label, cid])
			continue

		match kind:
			"mesh", "moving":
				for s_idx in surfaces.size():
					var s_def: Dictionary = surfaces[s_idx]
					var exp_route := String(s_def["route"])
					var exp_reason := String(s_def["reason"])
					if cid == "mutated_mesh" and p_post_mutate:
						exp_route = ExclusionRules.ROUTE_LEGACY
						exp_reason = ExclusionRules.REASON_MUTATED
					_assert_instance_surface(cand as VisualInstance3D, s_idx, exp_route, exp_reason, "p3_%s/%s" % [label, cid])
			"multimesh", "particles":
				_assert_instance_surface(cand as VisualInstance3D, 0, "legacy", "MULTIMESH_OR_PARTICLES", "p3_%s/%s" % [label, cid])
			"baked_gridmap":
				var bgm := cand as GridMap
				if bgm == null or _baked_mesh_count(bgm) == 0:
					_failures.append("p3_%s/%s: missing baked meshes" % [label, cid])
				else:
					var bake_rid := bgm.get_bake_mesh_instance(0)
					var bst: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(bake_rid)
					if String(bst.get("route", "")) != "legacy" or String(bst.get("reason", "")) != "BAKED_GRIDMAP":
						_failures.append("p3_%s/%s: baked_gridmap got %s" % [label, cid, JSON.stringify(bst)])
			"gridmap":
				var gm := cand as GridMap
				if gm == null or _baked_mesh_count(gm) != 0:
					_failures.append("p3_%s/%s: octant GridMap unexpected state" % [label, cid])
			"heightfield":
				var proxy := _p3_matrix.get_node_or_null(cid + "/CoarseProxy") as MeshInstance3D
				if proxy == null:
					_failures.append("p3_%s/%s: missing CoarseProxy" % [label, cid])
			"msaa", "multiview":
				var svp := cand as SubViewport
				if svp == null:
					_failures.append("p3_%s/%s: Candidate is not SubViewport" % [label, cid])
				else:
					var vst: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(svp.get_viewport_rid())
					var exp_vreason := "MSAA" if kind == "msaa" else "MULTIVIEW"
					if String(vst.get("view_route", "")) != "legacy" or String(vst.get("view_reason", "")) != exp_vreason:
						_failures.append("p3_%s/%s: viewport stats got %s, expected legacy/%s" % [
							label, cid, JSON.stringify(vst), exp_vreason])
		verified_cells += 1

	_summary["p3_" + label] = {
		"cells": verified_cells,
		"double_dispatch": int(vp_stats.get("double_dispatch", -1)),
		"missing": int(vp_stats.get("missing", -1)),
	}


# ---------------------------------------------------------------------------
# Phase 4: StreamedTexture2D (.stex) mip convergence on virtual-only material
# ---------------------------------------------------------------------------

func _write_test_stex(p_path: String) -> bool:
	var img := Image.create_empty(512, 512, true, Image.FORMAT_RGBA8)
	var num_mipmaps := img.get_mipmap_count()
	var data := img.get_data()
	for mip in range(num_mipmaps + 1):
		var ofs := img.get_mipmap_offset(mip)
		var end_ofs := img.get_mipmap_offset(mip + 1) if mip < num_mipmaps else data.size()
		# Mips 0..1 (512, 256) = bright green; coarse mips >= 2 (<= 128, including initial max_lod=3 @ 64) = bright red.
		var r_byte := 0 if mip <= 1 else 255
		var g_byte := 255 if mip <= 1 else 0
		var i := ofs
		while i + 3 < end_ofs:
			data[i + 0] = r_byte
			data[i + 1] = g_byte
			data[i + 2] = 0
			data[i + 3] = 255
			i += 4

	var f := FileAccess.open(p_path, FileAccess.WRITE)
	if f == null:
		return false
	f.store_8(ord("G"))
	f.store_8(ord("S"))
	f.store_8(ord("T"))
	f.store_8(ord("F"))
	f.store_32(1) # FORMAT_VERSION
	f.store_32(512)
	f.store_32(512)
	f.store_32(data.size())
	f.store_32(int(Image.FORMAT_RGBA8))
	f.store_32(num_mipmaps)
	for mip in range(num_mipmaps + 1):
		var ofs := img.get_mipmap_offset(mip)
		var end_ofs := img.get_mipmap_offset(mip + 1) if mip < num_mipmaps else data.size()
		f.store_32(ofs)
		f.store_32(end_ofs - ofs)
	f.store_32(0) # flags
	f.store_32(0) # streaming_min
	f.store_32(0) # streaming_max
	f.store_32(0) # reserved
	f.store_buffer(data)
	f.close()
	return true


func _start_phase_4() -> void:
	_p3_matrix = null
	_clear_phase_root()
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.0)

	_phase_root = Node3D.new()
	_phase_root.name = "Phase4Streaming"
	add_child(_phase_root)

	var virt_stex_path := _alloc_temp_path("stream_virt", "stex")
	var fwd_stex_path := _alloc_temp_path("stream_fwd", "stex")
	if not _write_test_stex(virt_stex_path) or not _write_test_stex(fwd_stex_path):
		_failures.append("p4: failed to write test .stex files")
		_finish()
		return

	_p4_stex_virt = StreamedTexture2D.new()
	var err_v := _p4_stex_virt.load(virt_stex_path)
	_p4_stex_fwd = StreamedTexture2D.new()
	var err_f := _p4_stex_fwd.load(fwd_stex_path)
	if err_v != OK or err_f != OK:
		_failures.append("p4: StreamedTexture2D.load failed (virt=%d fwd=%d)" % [err_v, err_f])
		_finish()
		return

	var init_v_img := RenderingServer.texture_2d_get(_p4_stex_virt.get_rid())
	var init_f_img := RenderingServer.texture_2d_get(_p4_stex_fwd.get_rid())
	_p4_init_virt_w = init_v_img.get_width() if init_v_img != null else 0
	_p4_init_fwd_w = init_f_img.get_width() if init_f_img != null else 0
	if _p4_init_virt_w >= 512 or _p4_init_fwd_w >= 512:
		_failures.append("p4: expected coarse initial streamed mip (< 512), got virt=%d fwd=%d" % [
			_p4_init_virt_w, _p4_init_fwd_w])

	var vmat := StandardMaterial3D.new()
	vmat.albedo_texture = _p4_stex_virt
	vmat.roughness = 1.0

	var fmat := StandardMaterial3D.new()
	fmat.albedo_texture = _p4_stex_fwd
	fmat.roughness = 1.0

	var vquad := QuadMesh.new()
	vquad.size = Vector2(1.6, 1.6)
	vquad.subdivide_width = 8
	vquad.subdivide_depth = 8
	vquad.material = vmat

	var imp := ImporterMesh.from_mesh(vquad)
	var b_err := imp.build_virtual_geometry(_alloc_temp_path("p4_vquad", "ngm"))
	var vmesh := imp.get_mesh()
	if b_err != OK or vmesh == null or vmesh.get_virtual_geometry() == null:
		_failures.append("p4: build_virtual_geometry failed (err=%d)" % b_err)

	_p4_virt = MeshInstance3D.new()
	_p4_virt.mesh = vmesh
	_p4_virt.position = Vector3(-0.9, 0.0, 0.0)
	_phase_root.add_child(_p4_virt)

	var fquad := QuadMesh.new()
	fquad.size = Vector2(1.6, 1.6)
	fquad.material = fmat

	_p4_fwd = MeshInstance3D.new()
	_p4_fwd.mesh = fquad
	_p4_fwd.position = Vector3(0.9, 0.0, 0.0)
	_phase_root.add_child(_p4_fwd)

	_p4_cam = Camera3D.new()
	_p4_cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	_p4_cam.size = 3.6
	_p4_cam.position = Vector3(0.0, 0.0, 4.0)
	_p4_cam.current = true
	_phase_root.add_child(_p4_cam)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.05, 0.05)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1.0, 1.0, 1.0)
	env.ambient_light_energy = 1.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var we := WorldEnvironment.new()
	we.environment = env
	_phase_root.add_child(we)

	_phase = "p4_streaming"


func _tick_p4_streaming() -> void:
	# Pace frames (~12 ms) so TextureStreaming's 30 ms feedback cadence fires
	# reliably across <= 120 frames even on uncapped fast GPUs.
	OS.delay_msec(12)

	var v_img := RenderingServer.texture_2d_get(_p4_stex_virt.get_rid())
	var f_img := RenderingServer.texture_2d_get(_p4_stex_fwd.get_rid())
	var vw := v_img.get_width() if v_img != null else 0
	var fw := f_img.get_width() if f_img != null else 0

	var vp_img := get_viewport().get_texture().get_image()
	var virt_green := false
	var fwd_green := false
	var virt_col := Color.BLACK
	var fwd_col := Color.BLACK
	if vp_img != null and not vp_img.is_empty():
		var vp_size := get_viewport().get_visible_rect().size
		var v_uv := _p4_cam.unproject_position(_p4_virt.global_position) / vp_size
		var f_uv := _p4_cam.unproject_position(_p4_fwd.global_position) / vp_size
		virt_col = vp_img.get_pixel(
			clampi(int(v_uv.x * float(vp_img.get_width())), 0, vp_img.get_width() - 1),
			clampi(int(v_uv.y * float(vp_img.get_height())), 0, vp_img.get_height() - 1)
		)
		fwd_col = vp_img.get_pixel(
			clampi(int(f_uv.x * float(vp_img.get_width())), 0, vp_img.get_width() - 1),
			clampi(int(f_uv.y * float(vp_img.get_height())), 0, vp_img.get_height() - 1)
		)
		virt_green = virt_col.g > 0.25 and virt_col.g > virt_col.r * 2.0
		fwd_green = fwd_col.g > 0.25 and fwd_col.g > fwd_col.r * 2.0

	if vw == 512 and fw == 512 and virt_green and fwd_green and _phase_frame >= 4:
		_summary["p4_streaming"] = {
			"frames": _phase_frame,
			"init_virt_w": _p4_init_virt_w,
			"init_fwd_w": _p4_init_fwd_w,
			"final_virt_w": vw,
			"final_fwd_w": fw,
		}
		_finish()
		return

	if _phase_frame >= 120:
		_failures.append("p4_streaming: failed to converge within 120 frames (init=%d/%d final=%d/%d virt_col=%s fwd_col=%s)" % [
			_p4_init_virt_w, _p4_init_fwd_w, vw, fw, str(virt_col), str(fwd_col)])
		_summary["p4_streaming"] = {
			"frames": _phase_frame,
			"init_virt_w": _p4_init_virt_w,
			"init_fwd_w": _p4_init_fwd_w,
			"final_virt_w": vw,
			"final_fwd_w": fw,
		}
		_finish()


func _finish() -> void:
	set_process(false)
	_p4_stex_virt = null
	_p4_stex_fwd = null
	_clear_phase_root()
	for path in _temp_files:
		var abs_p := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs_p):
			DirAccess.remove_absolute(abs_p)
	_temp_files.clear()

	var ok := _failures.is_empty()
	print("NEXTGEO_RESOLVE_SUMMARY pass=%s summary=%s failures=%s" % [
		str(ok),
		JSON.stringify(_summary),
		JSON.stringify(_failures),
	])
	get_tree().quit(0 if ok else 1)
