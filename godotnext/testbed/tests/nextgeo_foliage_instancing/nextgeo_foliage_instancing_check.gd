extends Node3D
## NextGeo foliage instancing, MultiMesh GPU records, Nanite Assemblies, and VSM GPU check (#151):
## 1. Per-instance GPU culling vs CPU reference across multi-frame camera sweep (0 false negatives,
##    exact GPU vs CPU cut hash match across frustum, distance, dirty transform/visible-count updates,
##    plus 2-phase Hi-Z occlusion check).
## 2. Non-virtualized MultiMesh renders byte-identical to baseline (with active NextGeo MultiMeshes).
## 3. INSTANCE_CUSTOM & COLOR reach both programmable raster bins (WPO + masked) AND visibility
##    resolve (both Tile Quads and Compute Bins) accurately.
## 4. Nanite Assemblies (experimental, default false; enabled = true): 2-level assembly visually
##    matches flattened MeshInstance3Ds and casts VSM shadows; MultiMesh of 2-level assembly renders.

const TAG := "GODOTNEXT_NEXTGEO_FOLIAGE_INSTANCING"

var _phase := ""
var _phase_frame := 0
var _phase_root: Node3D
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

# Phase 1 state
var _p1_cam: Camera3D
var _p1_mmi: MultiMeshInstance3D
var _p1_mm: MultiMesh
var _p1_occluder: MeshInstance3D
var _p1_step := 0
var _p1_poses: Array[Dictionary] = []

# Phase 2 state
var _p2_step := 0
var _p2_baseline_img: Image
var _p2_plain_mmi: MultiMeshInstance3D
var _p2_offscreen_vmm: MultiMeshInstance3D

# Phase 3 state
var _p3_step := 0
var _p3_cam: Camera3D
var _p3_tile_opaque_img: Image
var _p3_compute_opaque_img: Image
var _p3_tile_prog_img: Image
var _p3_compute_prog_img: Image

# Phase 4 state
var _p4_step := 0
var _p4_cam: Camera3D
var _p4_flat_img: Image
var _p4_asm_img: Image
var _p4_mmi_asm_img: Image
var _p4_trunk_vmesh: ArrayMesh
var _p4_leaf_vmesh: ArrayMesh
var _p4_asm_vmesh: ArrayMesh


func _ready() -> void:
	# Verify experimental assemblies defaults to false before touching any setting.
	var default_asm := bool(ProjectSettings.get_setting("rendering/nextgeo/experimental/assemblies", true))
	if default_asm:
		_failures.append("init: rendering/nextgeo/experimental/assemblies defaulted to true, expected false")
	_summary["default_assemblies_setting"] = default_asm

	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 0)
	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0
	_start_phase_1()


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_foliage_%d_%s_%d.ngm" % [OS.get_process_id(), p_tag.validate_filename(), _ngm_counter]
	_temp_files.append(path)
	return path


func _clear_phase_root() -> void:
	if _phase_root != null:
		remove_child(_phase_root)
		_phase_root.free()
		_phase_root = null
	_phase_frame = 0


func _build_vmesh(p_prim: PrimitiveMesh, p_mat: Material, p_tag: String, p_preserve_area: bool = false) -> ArrayMesh:
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, p_prim.get_mesh_arrays())
	var imp := ImporterMesh.from_mesh(am)
	var err := imp.build_virtual_geometry(_alloc_temp_ngm(p_tag), false, p_preserve_area)
	if err != OK:
		_failures.append("build_virtual_geometry(%s) failed: %d" % [p_tag, err])
		return null
	var vmesh := imp.get_mesh()
	if vmesh != null and p_mat != null:
		vmesh.surface_set_material(0, p_mat)
	return vmesh


func _setup_env_and_light(p_root: Node3D, p_shadows: bool = false, p_bg: Color = Color(0.04, 0.05, 0.07)) -> DirectionalLight3D:
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-48.0, 28.0, 0.0)
	sun.light_energy = 2.0
	sun.shadow_enabled = p_shadows
	p_root.add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = p_bg
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.25, 0.25, 0.28)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var we := WorldEnvironment.new()
	we.environment = env
	p_root.add_child(we)
	return sun


func _capture_rgb8() -> Image:
	var img := get_viewport().get_texture().get_image()
	if img != null and img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _sample_patch_avg(p_img: Image, p_uv: Vector2, p_radius: int = 3) -> Color:
	var w := p_img.get_width()
	var h := p_img.get_height()
	var cx := clampi(int(round(p_uv.x * float(w - 1))), p_radius, w - 1 - p_radius)
	var cy := clampi(int(round(p_uv.y * float(h - 1))), p_radius, h - 1 - p_radius)
	var accum := Color(0.0, 0.0, 0.0, 0.0)
	var n := 0
	for dy in range(-p_radius, p_radius + 1):
		for dx in range(-p_radius, p_radius + 1):
			accum += p_img.get_pixel(cx + dx, cy + dy)
			n += 1
	return accum / float(max(n, 1))


func _project_uv(p_cam: Camera3D, p_world: Vector3) -> Vector2:
	var vp_size := Vector2(get_viewport().size)
	var screen := p_cam.unproject_position(p_world)
	return Vector2(screen.x / max(vp_size.x, 1.0), screen.y / max(vp_size.y, 1.0))


func _mean_abs_diff(p_a: Image, p_b: Image) -> float:
	if p_a == null or p_b == null or p_a.get_size() != p_b.get_size():
		return 1.0
	var da := p_a.get_data()
	var db := p_b.get_data()
	var count := mini(da.size(), db.size())
	if count == 0:
		return 1.0
	var total := 0.0
	for i in range(0, count, 4):
		total += absf(float(da[i]) - float(db[i]))
		total += absf(float(da[i + 1]) - float(db[i + 1]))
		total += absf(float(da[i + 2]) - float(db[i + 2]))
	return total / (float(count) * 0.75 * 255.0)


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
# Phase 1: Per-instance GPU culling vs CPU reference across camera sweep
# ---------------------------------------------------------------------------

func _start_phase_1() -> void:
	_clear_phase_root()
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/nextgeo/experimental/assemblies", false)

	_phase_root = Node3D.new()
	_phase_root.name = "Phase1PerInstanceCull"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root, false)

	_p1_cam = Camera3D.new()
	_p1_cam.fov = 60.0
	_p1_cam.near = 0.2
	_p1_cam.far = 200.0
	_p1_cam.current = true
	_phase_root.add_child(_p1_cam)

	var box := BoxMesh.new()
	box.size = Vector3(0.7, 0.7, 0.7)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.35, 0.75, 0.35)
	var vmesh := _build_vmesh(box, mat, "p1_foliage_cube", true)

	_p1_mm = MultiMesh.new()
	_p1_mm.transform_format = MultiMesh.TRANSFORM_3D
	_p1_mm.use_colors = true
	_p1_mm.use_custom_data = true
	_p1_mm.instance_count = 64
	_p1_mm.mesh = vmesh
	for z in range(8):
		for x in range(8):
			var idx := z * 8 + x
			var pos := Vector3((float(x) - 3.5) * 2.5, 0.0, (float(z) - 3.5) * 2.5)
			_p1_mm.set_instance_transform(idx, Transform3D(Basis(), pos))
			_p1_mm.set_instance_color(idx, Color(float(x) / 7.0, float(z) / 7.0, 0.4, 1.0))
			_p1_mm.set_instance_custom_data(idx, Color(float(idx) / 63.0, 0.5, 0.25, 1.0))

	_p1_mmi = MultiMeshInstance3D.new()
	_p1_mmi.multimesh = _p1_mm
	_p1_mmi.visibility_range_end = 18.0
	_phase_root.add_child(_p1_mmi)

	# Large occluder wall (hidden until the Hi-Z step).
	var wall_mesh := BoxMesh.new()
	wall_mesh.size = Vector3(24.0, 12.0, 0.4)
	var wall_mat := StandardMaterial3D.new()
	wall_mat.albedo_color = Color(0.2, 0.2, 0.25)
	_p1_occluder = MeshInstance3D.new()
	_p1_occluder.mesh = wall_mesh
	_p1_occluder.material_override = wall_mat
	_p1_occluder.position = Vector3(0.0, 0.0, 10.5)
	_p1_occluder.visible = false
	_phase_root.add_child(_p1_occluder)

	_p1_poses = [
		{ "name": "wide_all", "pos": Vector3(0.0, 6.0, 14.0), "target": Vector3(0.0, 0.0, 0.0), "occlusion": false, "mutate": "" },
		{ "name": "frustum_right", "pos": Vector3(11.0, 2.5, 10.0), "target": Vector3(14.0, 0.0, -4.0), "occlusion": false, "mutate": "" },
		{ "name": "distance_close", "pos": Vector3(0.0, 2.0, 15.0), "target": Vector3(0.0, 0.0, -10.0), "occlusion": false, "mutate": "" },
		{ "name": "dirty_transform_and_visible", "pos": Vector3(0.0, 6.0, 14.0), "target": Vector3(0.0, 0.0, 0.0), "occlusion": false, "mutate": "shrink_and_move" },
		{ "name": "look_away_parent_cull", "pos": Vector3(0.0, 6.0, 16.0), "target": Vector3(0.0, 6.0, 30.0), "occlusion": false, "mutate": "" },
		{ "name": "hiz_occluded", "pos": Vector3(0.0, 0.5, 13.5), "target": Vector3(0.0, 0.0, 0.0), "occlusion": true, "mutate": "restore_visible" },
	]
	_p1_step = 0
	_apply_p1_pose(0)
	_phase = "p1"


func _apply_p1_pose(p_idx: int) -> void:
	var pose: Dictionary = _p1_poses[p_idx]
	var mut := String(pose["mutate"])
	if mut == "shrink_and_move":
		_p1_mm.visible_instance_count = 32
		_p1_mm.set_instance_transform(0, Transform3D(Basis(), Vector3(0.0, 1.5, 6.0)))
	elif mut == "restore_visible":
		_p1_mm.visible_instance_count = 64
	var occ := bool(pose["occlusion"])
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", occ)
	_p1_occluder.visible = occ
	_p1_cam.look_at_from_position(Vector3(pose["pos"]), Vector3(pose["target"]), Vector3.UP)
	_phase_frame = 0


func _tick_p1() -> void:
	if _phase_frame < 6:
		return
	var pose: Dictionary = _p1_poses[_p1_step]
	var pname := String(pose["name"])
	var occ := bool(pose["occlusion"])

	var inst_st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_p1_mmi.get_instance())
	if int(inst_st.get("record_index", -1)) < 0 or String(inst_st.get("route", "")) != "virtual":
		_failures.append("p1/%s: expected virtual MultiMesh record, got %s" % [pname, JSON.stringify(inst_st)])

	var vp_st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var double_disp := int(vp_st.get("double_dispatch", -1))
	var missing := int(vp_st.get("missing", -1))
	if double_disp != 0 or missing != 0:
		_failures.append("p1/%s: double_dispatch=%d missing=%d" % [pname, double_disp, missing])

	var gpu_hash := str(vp_st.get("cut_hash", ""))
	var cpu_hash := str(vp_st.get("cpu_cut_hash", ""))
	var hash_match := gpu_hash != "" and gpu_hash == cpu_hash
	var cpu_inst := int(vp_st.get("cpu_instances", -1))
	var gpu_clusters := int(vp_st.get("clusters", -1))
	var cpu_clusters := int(vp_st.get("cpu_clusters", -1))

	if not occ:
		if not hash_match or gpu_hash != cpu_hash or gpu_clusters != cpu_clusters:
			_failures.append("p1/%s: GPU/CPU cut mismatch (gpu_hash=%s cpu_hash=%s gpu_clusters=%d cpu_clusters=%d cpu_inst=%d)" % [
				pname, gpu_hash, cpu_hash, gpu_clusters, cpu_clusters, cpu_inst])
		if pname == "wide_all" and cpu_inst <= 0:
			_failures.append("p1/wide_all: expected > 0 visible MultiMesh instances, got %d" % cpu_inst)
		if pname == "frustum_right" and (cpu_inst <= 0 or cpu_inst >= 64):
			_failures.append("p1/frustum_right: expected partial per-instance frustum cull (0 < n < 64), got %d" % cpu_inst)
		if pname == "distance_close" and (cpu_inst <= 0 or cpu_inst >= 64):
			_failures.append("p1/distance_close: expected partial per-instance distance cull (0 < n < 64), got %d" % cpu_inst)
		if pname == "dirty_transform_and_visible" and (cpu_inst <= 0 or cpu_inst > 32):
			_failures.append("p1/dirty_transform_and_visible: expected 0 < n <= 32 after visible_instance_count=32, got %d" % cpu_inst)
		if pname == "look_away_parent_cull" and (cpu_inst != 0 or gpu_clusters != 0):
			_failures.append("p1/look_away_parent_cull: expected 0 instances/clusters, got cpu_inst=%d gpu_clusters=%d" % [cpu_inst, gpu_clusters])

	_summary["p1_" + pname] = {
		"cpu_instances": cpu_inst,
		"gpu_clusters": gpu_clusters,
		"cpu_clusters": cpu_clusters,
		"hash_match": hash_match,
	}

	_p1_step += 1
	if _p1_step < _p1_poses.size():
		_apply_p1_pose(_p1_step)
	else:
		ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
		_start_phase_2()


# ---------------------------------------------------------------------------
# Phase 2: Non-virtualized MultiMesh byte-identical to baseline
# ---------------------------------------------------------------------------

func _start_phase_2() -> void:
	_clear_phase_root()
	_phase_root = Node3D.new()
	_phase_root.name = "Phase2NonVirtualizedIdentity"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root, false)

	var cam := Camera3D.new()
	cam.look_at_from_position(Vector3(0.0, 2.0, 7.0), Vector3(0.0, 0.0, 0.0), Vector3.UP)
	cam.current = true
	_phase_root.add_child(cam)

	var plain_box := BoxMesh.new()
	plain_box.size = Vector3(0.9, 0.9, 0.9)
	var plain_mat := StandardMaterial3D.new()
	plain_mat.vertex_color_use_as_albedo = true
	plain_box.material = plain_mat

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.use_custom_data = true
	mm.instance_count = 4
	mm.mesh = plain_box
	var cols := [Color(0.9, 0.2, 0.2), Color(0.2, 0.9, 0.2), Color(0.2, 0.3, 0.95), Color(0.95, 0.85, 0.2)]
	for i in range(4):
		mm.set_instance_transform(i, Transform3D(Basis(), Vector3((float(i) - 1.5) * 1.5, 0.0, 0.0)))
		mm.set_instance_color(i, cols[i])
		mm.set_instance_custom_data(i, Color(float(i) * 0.25, 0.5, 0.75, 1.0))

	_p2_plain_mmi = MultiMeshInstance3D.new()
	_p2_plain_mmi.multimesh = mm
	_phase_root.add_child(_p2_plain_mmi)

	_p2_step = 0
	_phase = "p2"


func _tick_p2() -> void:
	if _phase_frame < 5:
		return
	if _p2_step == 0:
		var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_p2_plain_mmi.get_instance())
		if int(st.get("record_index", -1)) != -1 or String(st.get("route", "")) != "legacy":
			_failures.append("p2: non-virtualized MultiMesh must have record_index=-1 & legacy route, got %s" % JSON.stringify(st))
		_p2_baseline_img = _capture_rgb8()

		# Add an active virtualized MultiMesh behind the camera so NextGeo MultiMesh GPU expansion
		# runs every frame alongside the non-virtualized MultiMesh.
		var vbox := BoxMesh.new()
		vbox.size = Vector3(0.5, 0.5, 0.5)
		var vmesh := _build_vmesh(vbox, StandardMaterial3D.new(), "p2_offscreen_vmm")
		var vmm := MultiMesh.new()
		vmm.transform_format = MultiMesh.TRANSFORM_3D
		vmm.instance_count = 8
		vmm.mesh = vmesh
		for i in range(8):
			vmm.set_instance_transform(i, Transform3D(Basis(), Vector3(float(i), 0.0, 40.0)))
		_p2_offscreen_vmm = MultiMeshInstance3D.new()
		_p2_offscreen_vmm.multimesh = vmm
		_phase_root.add_child(_p2_offscreen_vmm)

		_p2_step = 1
		_phase_frame = 0
		return

	var after_img := _capture_rgb8()
	var byte_equal := _p2_baseline_img != null and after_img != null and _p2_baseline_img.get_data() == after_img.get_data()
	var diff := _mean_abs_diff(_p2_baseline_img, after_img)
	if not byte_equal:
		_failures.append("p2: non-virtualized MultiMesh image changed when NextGeo MultiMesh was active (mean_diff=%.6f)" % diff)
	_summary["p2_non_virtualized_byte_identical"] = byte_equal
	_start_phase_3()


# ---------------------------------------------------------------------------
# Phase 3: INSTANCE_CUSTOM & COLOR across programmable raster bins & resolve
#          (both Tile Quads and Compute Bins)
# ---------------------------------------------------------------------------

func _make_custom_color_shader(p_programmable: bool) -> ShaderMaterial:
	var sh := Shader.new()
	if p_programmable:
		sh.code = """
shader_type spatial;
render_mode unshaded, cull_disabled;
varying vec4 v_custom;
varying vec4 v_color;
void vertex() {
	VERTEX.y += INSTANCE_CUSTOM.x;
	v_custom = INSTANCE_CUSTOM;
	v_color = COLOR;
}
void fragment() {
	// Masked programmable bin: keep pixels where UV.x <= INSTANCE_CUSTOM.y (always > 0.8 for visible quads).
	if (UV.x > v_custom.y) {
		discard;
	}
	ALBEDO = mix(v_color.rgb, vec3(v_custom.z, v_custom.w, 0.0), 0.25);
}
"""
	else:
		sh.code = """
shader_type spatial;
render_mode unshaded;
varying vec4 v_custom;
varying vec4 v_color;
void vertex() {
	v_custom = INSTANCE_CUSTOM;
	v_color = COLOR;
}
void fragment() {
	ALBEDO = vec3(v_color.r, v_color.g, v_custom.z);
}
"""
	var mat := ShaderMaterial.new()
	mat.shader = sh
	if p_programmable:
		mat.vertex_offset_max = 1.5
	return mat


func _setup_p3_scene(p_programmable: bool, p_shading_path: int) -> void:
	_clear_phase_root()
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", p_shading_path)

	_phase_root = Node3D.new()
	_phase_root.name = "Phase3CustomAndColor"
	add_child(_phase_root)
	_setup_env_and_light(_phase_root, false, Color(0.0, 0.0, 0.0))

	_p3_cam = Camera3D.new()
	_p3_cam.fov = 50.0
	_p3_cam.look_at_from_position(Vector3(0.0, 0.5, 9.0), Vector3(0.0, 0.5, 0.0), Vector3.UP)
	_p3_cam.current = true
	_phase_root.add_child(_p3_cam)

	var box := BoxMesh.new()
	box.size = Vector3(1.2, 1.2, 1.2)
	var mat := _make_custom_color_shader(p_programmable)
	var vmesh := _build_vmesh(box, mat, "p3_box_%s_%d" % ["prog" if p_programmable else "op", p_shading_path])

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.use_custom_data = true
	mm.instance_count = 4
	mm.mesh = vmesh

	# 4 instances with distinct COLOR and INSTANCE_CUSTOM:
	# - instance 0: red, y_offset=0.0, alpha_keep=1.0, z=0.1, w=0.0
	# - instance 1: green, y_offset=0.9, alpha_keep=1.0, z=0.9, w=0.0
	# - instance 2: blue-tint via custom.z=0.9, y_offset=0.0, alpha_keep=1.0, w=0.8
	# - instance 3: alpha-discarded in programmable mode (alpha_keep=-0.1 -> all pixels discarded!)
	var xs := [-2.7, -0.9, 0.9, 2.7]
	var colors := [
		Color(1.0, 0.0, 0.0, 1.0),
		Color(0.0, 1.0, 0.0, 1.0),
		Color(0.0, 0.0, 1.0, 1.0),
		Color(1.0, 1.0, 0.0, 1.0),
	]
	var customs := [
		Color(0.0, 1.1, 0.0, 0.0),
		Color(0.9, 1.1, 0.0, 0.0),
		Color(0.0, 1.1, 1.0, 0.0),
		Color(0.0, -0.1, 1.0, 1.0),
	]
	for i in range(4):
		mm.set_instance_transform(i, Transform3D(Basis(), Vector3(xs[i], 0.0, 0.0)))
		mm.set_instance_color(i, colors[i])
		mm.set_instance_custom_data(i, customs[i])

	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	_phase_root.add_child(mmi)


func _start_phase_3() -> void:
	_p3_step = 0
	_setup_p3_scene(false, 1) # Opaque resolve, Tile Quads
	_phase = "p3"


func _verify_p3_opaque(p_img: Image, p_label: String) -> void:
	# In opaque shader: ALBEDO = vec3(COLOR.r, COLOR.g, INSTANCE_CUSTOM.z)
	# inst 0: (1, 0, 0), inst 1: (0, 1, 0), inst 2: (0, 0, 1), inst 3: (1, 1, 1)
	var xs := [-2.7, -0.9, 0.9, 2.7]
	var expected := [
		Color(1.0, 0.0, 0.0),
		Color(0.0, 1.0, 0.0),
		Color(0.0, 0.0, 1.0),
		Color(1.0, 1.0, 1.0),
	]
	for i in range(4):
		var uv := _project_uv(_p3_cam, Vector3(xs[i], 0.0, 0.6))
		var got := _sample_patch_avg(p_img, uv, 3)
		var exp: Color = expected[i]
		var err := maxf(absf(got.r - exp.r), maxf(absf(got.g - exp.g), absf(got.b - exp.b)))
		if err > 0.12:
			_failures.append("p3/%s inst %d: expected %s got %s (err=%.3f)" % [p_label, i, str(exp), str(got), err])


func _verify_p3_programmable(p_img: Image, p_label: String) -> void:
	var vp_st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	if int(vp_st.get("programmable_bins", 0)) <= 0:
		_failures.append("p3/%s: expected programmable_bins > 0, got %s" % [p_label, JSON.stringify(vp_st)])

	# inst 0 at y=0.0 -> red (0.75, 0, 0)
	var c0 := _sample_patch_avg(p_img, _project_uv(_p3_cam, Vector3(-2.7, 0.0, 0.6)), 3)
	if c0.r < 0.55 or c0.g > 0.15 or c0.b > 0.15:
		_failures.append("p3/%s inst 0 (COLOR red): got %s" % [p_label, str(c0)])

	# inst 1 offset by INSTANCE_CUSTOM.x = +0.9 -> green at y=0.9 (and empty/dark below at y=-0.45)
	var c1_hi := _sample_patch_avg(p_img, _project_uv(_p3_cam, Vector3(-0.9, 0.9, 0.6)), 3)
	var c1_lo := _sample_patch_avg(p_img, _project_uv(_p3_cam, Vector3(-0.9, -0.45, 0.6)), 2)
	if c1_hi.g < 0.55 or c1_hi.r > 0.15:
		_failures.append("p3/%s inst 1 (WPO +0.9 via INSTANCE_CUSTOM.x + COLOR green): got hi=%s" % [p_label, str(c1_hi)])
	if c1_lo.g > 0.15:
		_failures.append("p3/%s inst 1 (expected empty below WPO shift): got lo=%s" % [p_label, str(c1_lo)])

	# inst 2 at y=0.0 -> mix((0,0,1), (1,0,0), 0.25) = (0.25, 0, 0.75)
	var c2 := _sample_patch_avg(p_img, _project_uv(_p3_cam, Vector3(0.9, 0.0, 0.6)), 3)
	if c2.b < 0.55 or c2.r < 0.12 or c2.g > 0.15:
		_failures.append("p3/%s inst 2 (COLOR blue + INSTANCE_CUSTOM.z): got %s" % [p_label, str(c2)])

	# inst 3 discarded via INSTANCE_CUSTOM.y = -0.1 -> black background (0, 0, 0)
	var c3 := _sample_patch_avg(p_img, _project_uv(_p3_cam, Vector3(2.7, 0.0, 0.6)), 3)
	if c3.r > 0.08 or c3.g > 0.08 or c3.b > 0.08:
		_failures.append("p3/%s inst 3 (expected alpha-discarded via INSTANCE_CUSTOM.y): got %s" % [p_label, str(c3)])


func _tick_p3() -> void:
	if _phase_frame < 6:
		return
	match _p3_step:
		0:
			_p3_tile_opaque_img = _capture_rgb8()
			_verify_p3_opaque(_p3_tile_opaque_img, "opaque_tile_quads")
			_p3_step = 1
			_setup_p3_scene(false, 2) # Opaque resolve, Compute Bins
		1:
			_p3_compute_opaque_img = _capture_rgb8()
			_verify_p3_opaque(_p3_compute_opaque_img, "opaque_compute_bins")
			var diff_op := _mean_abs_diff(_p3_tile_opaque_img, _p3_compute_opaque_img)
			if diff_op > 0.005:
				_failures.append("p3: opaque Tile Quads vs Compute Bins diff=%.5f" % diff_op)
			_summary["p3_opaque_tile_vs_compute_diff"] = diff_op
			_p3_step = 2
			_setup_p3_scene(true, 1) # Programmable bin, Tile Quads
		2:
			_p3_tile_prog_img = _capture_rgb8()
			_verify_p3_programmable(_p3_tile_prog_img, "prog_tile_quads")
			_p3_step = 3
			_setup_p3_scene(true, 2) # Programmable bin, Compute Bins
		3:
			_p3_compute_prog_img = _capture_rgb8()
			_verify_p3_programmable(_p3_compute_prog_img, "prog_compute_bins")
			var diff_prog := _mean_abs_diff(_p3_tile_prog_img, _p3_compute_prog_img)
			if diff_prog > 0.005:
				_failures.append("p3: programmable Tile Quads vs Compute Bins diff=%.5f" % diff_prog)
			_summary["p3_prog_tile_vs_compute_diff"] = diff_prog
			ProjectSettings.set_setting("rendering/nextgeo/shading_path", 0)
			_start_phase_4()


# ---------------------------------------------------------------------------
# Phase 4: Nanite Assemblies (2-level hierarchy) visual parity with flattened
#          instances, VSM shadows, and MultiMesh-of-Assembly
# ---------------------------------------------------------------------------

func _build_p4_assets() -> void:
	var trunk_box := BoxMesh.new()
	trunk_box.size = Vector3(0.5, 2.0, 0.5)
	var trunk_mat := StandardMaterial3D.new()
	trunk_mat.albedo_color = Color(0.55, 0.32, 0.16)
	_p4_trunk_vmesh = _build_vmesh(trunk_box, trunk_mat, "p4_trunk")

	var leaf_box := BoxMesh.new()
	leaf_box.size = Vector3(0.85, 0.65, 0.85)
	var leaf_mat := StandardMaterial3D.new()
	leaf_mat.albedo_color = Color(0.22, 0.72, 0.28)
	_p4_leaf_vmesh = _build_vmesh(leaf_box, leaf_mat, "p4_leaf", true)

	# 2-level VirtualGeometryAssembly:
	# Root: 1 direct trunk at (0, 1.0, 0) + 2 child branch assemblies at (0, 2.1, 0) and (0, 2.9, 0)
	# Each branch: 2 leaf parts at (-0.75, 0, 0) and (+0.75, 0, 0). Total = 5 leaf parts.
	var branch := VirtualGeometryAssembly.new()
	branch.add_part(_p4_leaf_vmesh, Transform3D(Basis(), Vector3(-0.75, 0.0, 0.0)))
	branch.add_part(_p4_leaf_vmesh, Transform3D(Basis(), Vector3(0.75, 0.0, 0.0)))

	var root_asm := VirtualGeometryAssembly.new()
	root_asm.add_part(_p4_trunk_vmesh, Transform3D(Basis(), Vector3(0.0, 1.0, 0.0)))
	var err0 := root_asm.add_child_assembly(branch, Transform3D(Basis(), Vector3(0.0, 2.1, 0.0)))
	var err1 := root_asm.add_child_assembly(branch, Transform3D(Basis(), Vector3(0.0, 2.9, 0.0)))
	if err0 != OK or err1 != OK or root_asm.get_total_leaf_part_count() != 5 or root_asm.get_depth() != 2:
		_failures.append("p4: failed to construct 2-level VirtualGeometryAssembly")

	# Container ArrayMesh holding VirtualGeometryData with assembly payload.
	_p4_asm_vmesh = _build_vmesh(trunk_box, trunk_mat, "p4_asm_container")
	var vgd := _p4_asm_vmesh.get_virtual_geometry()
	vgd.assembly = root_asm
	_p4_asm_vmesh.set_virtual_geometry(vgd)


func _setup_p4_scene(p_mode: String) -> void:
	_clear_phase_root()
	_phase_root = Node3D.new()
	_phase_root.name = "Phase4Assemblies_" + p_mode
	add_child(_phase_root)
	_setup_env_and_light(_phase_root, true, Color(0.08, 0.1, 0.14))

	_p4_cam = Camera3D.new()
	_p4_cam.fov = 50.0
	_p4_cam.look_at_from_position(Vector3(0.0, 4.2, 8.5), Vector3(0.0, 1.2, 0.0), Vector3.UP)
	_p4_cam.current = true
	_phase_root.add_child(_p4_cam)

	# Ground receiver plane for VSM shadow verification.
	var plane := PlaneMesh.new()
	plane.size = Vector2(20.0, 20.0)
	var gmat := StandardMaterial3D.new()
	gmat.albedo_color = Color(0.75, 0.75, 0.75)
	var ground := MeshInstance3D.new()
	ground.mesh = plane
	ground.material_override = gmat
	_phase_root.add_child(ground)

	if p_mode == "flattened":
		ProjectSettings.set_setting("rendering/nextgeo/experimental/assemblies", false)
		var placements := [
			{ "mesh": _p4_trunk_vmesh, "pos": Vector3(0.0, 1.0, 0.0) },
			{ "mesh": _p4_leaf_vmesh, "pos": Vector3(-0.75, 2.1, 0.0) },
			{ "mesh": _p4_leaf_vmesh, "pos": Vector3(0.75, 2.1, 0.0) },
			{ "mesh": _p4_leaf_vmesh, "pos": Vector3(-0.75, 2.9, 0.0) },
			{ "mesh": _p4_leaf_vmesh, "pos": Vector3(0.75, 2.9, 0.0) },
		]
		for p in placements:
			var mi := MeshInstance3D.new()
			mi.mesh = p["mesh"]
			mi.position = Vector3(p["pos"])
			_phase_root.add_child(mi)
	elif p_mode == "assembly":
		ProjectSettings.set_setting("rendering/nextgeo/experimental/assemblies", true)
		var mi := MeshInstance3D.new()
		mi.mesh = _p4_asm_vmesh
		_phase_root.add_child(mi)
	elif p_mode == "multimesh_assembly":
		ProjectSettings.set_setting("rendering/nextgeo/experimental/assemblies", true)
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.instance_count = 2
		mm.mesh = _p4_asm_vmesh
		mm.set_instance_transform(0, Transform3D(Basis(), Vector3(-2.0, 0.0, 0.0)))
		mm.set_instance_transform(1, Transform3D(Basis(), Vector3(2.0, 0.0, 0.0)))
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = mm
		_phase_root.add_child(mmi)


func _start_phase_4() -> void:
	_build_p4_assets()
	_p4_step = 0
	_setup_p4_scene("flattened")
	_phase = "p4"


func _tick_p4() -> void:
	if _phase_frame < 8:
		return
	match _p4_step:
		0:
			_p4_flat_img = _capture_rgb8()
			_p4_step = 1
			_setup_p4_scene("assembly")
		1:
			_p4_asm_img = _capture_rgb8()
			var diff := _mean_abs_diff(_p4_flat_img, _p4_asm_img)
			if diff > 0.015:
				_failures.append("p4/assembly_vs_flattened: visual mismatch mean_diff=%.5f (expected <= 0.015)" % diff)

			# Verify both canopy leaves (green) and ground VSM shadow (darker than lit ground) appear.
			var leaf_px := _sample_patch_avg(_p4_asm_img, _project_uv(_p4_cam, Vector3(-0.75, 2.1, 0.4)), 3)
			var lit_ground := _sample_patch_avg(_p4_asm_img, _project_uv(_p4_cam, Vector3(4.5, 0.0, 2.0)), 4)
			var shadow_min_luma := 1.0
			for sz in [-2.4, -1.8, -1.2, -0.6]:
				for sx in [-2.2, -1.5, -0.8, -0.2, 0.5]:
					var s := _sample_patch_avg(_p4_asm_img, _project_uv(_p4_cam, Vector3(sx, 0.0, sz)), 2)
					shadow_min_luma = minf(shadow_min_luma, s.get_luminance())
			var lit_luma := lit_ground.get_luminance()
			if leaf_px.g <= leaf_px.r * 1.3:
				_failures.append("p4/assembly: expected green leaf part at (-0.75, 2.1, 0.4), got %s" % str(leaf_px))
			if shadow_min_luma >= lit_luma * 0.75:
				_failures.append("p4/assembly: expected VSM cast shadow on ground (min_luma=%.3f vs lit=%.3f)" % [shadow_min_luma, lit_luma])

			_summary["p4_assembly_vs_flattened_diff"] = diff
			_summary["p4_shadow_min_luma"] = shadow_min_luma
			_summary["p4_lit_ground_luma"] = lit_luma

			_p4_step = 2
			_setup_p4_scene("multimesh_assembly")
		2:
			_p4_mmi_asm_img = _capture_rgb8()
			var left_leaf := _sample_patch_avg(_p4_mmi_asm_img, _project_uv(_p4_cam, Vector3(-2.75, 2.1, 0.4)), 3)
			var right_leaf := _sample_patch_avg(_p4_mmi_asm_img, _project_uv(_p4_cam, Vector3(2.75, 2.1, 0.4)), 3)
			if left_leaf.g <= left_leaf.r * 1.25 or right_leaf.g <= right_leaf.r * 1.25:
				_failures.append("p4/multimesh_assembly: expected green leaves on both MultiMesh assembly instances, got left=%s right=%s" % [str(left_leaf), str(right_leaf)])
			_summary["p4_multimesh_assembly_ok"] = (left_leaf.g > left_leaf.r * 1.25 and right_leaf.g > right_leaf.r * 1.25)

			ProjectSettings.set_setting("rendering/nextgeo/experimental/assemblies", false)
			_finish()


func _finish() -> void:
	_clear_phase_root()
	for path in _temp_files:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	var out := {
		"pass": _failures.is_empty(),
		"failures": _failures,
		"summary": _summary,
	}
	print("%s %s" % [TAG, JSON.stringify(out)])
	get_tree().quit(0 if _failures.is_empty() else 1)
