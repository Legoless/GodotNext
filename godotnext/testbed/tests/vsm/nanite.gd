extends Node3D

## Virtual Shadow Map x Nanite per-page multi-view cluster cull & hybrid HW/SW raster (#217; plan §21.3
## contract S4).
##
## Tests every implementation step of #217 end-to-end on real virtualized NextGeo meshes:
## 1. Per-page multi-view cluster cull & hybrid raster into the VSM physical page pool with 1 indirect
##    dispatch chain for directional maps and 1 for local maps (<= 2 each), zero atlas passes, zero
##    overflows, zero #277 atomic sinks:
##    - Hard shadow accuracy (directional, spot, omni, area) in HW-only mode (sw_max = 0) and hybrid
##      SW+HW mode (sw_max = 24, verifying sw_clusters > 0, sw_writes > 0, and SW/HW shadow parity),
##      plus the provisional default threshold (sw_max = 8).
## 2. Shadow casting modes and culling flags on virtual casters:
##    - SHADOW_CASTING_SETTING_OFF casts no shadow.
##    - SHADOW_CASTING_SETTING_SHADOWS_ONLY is invisible in main view yet casts full analytic shadow.
##    - SHADOW_CASTING_SETTING_DOUBLE_SIDED and shadow_reverse_cull_face on a single-sided back-facing
##      virtual quad.
## 3. Step 3 (per-page 1-level 8x8 Hi-Z) & Step 6 (static/dynamic layer routing hook):
##    - With debug_nanite_dynamic_layer enabled, a static virtual wall builds per-page static Hi-Z
##      (hiz_pages_built > 0); a moving dynamic virtual caster behind the wall is Hi-Z culled
##      (hiz_culled_nodes + hiz_culled_clusters > 0); moving the dynamic caster into the open casts
##      a shadow from layer 1.
## 4. Step 7 (shared RASTER_BIN_TABLE / programmable raster bins):
##    - Masked alpha-scissor stripes and VERTEX-scaled WPO virtual meshes cast accurate masked/expanded
##      shadows (exercising programmable HW/SW VSM bins when hybrid programmable raster is active, and
##      verifying seamless non-Nanite VSM handoff otherwise).
## 5. VSM_OVERDRAW debug draw runs cleanly with NextGeo pairs (MODE_NANITE_PAIRS).
## 6. With --shadows=atlas (VSM off), verifies VSM stays completely inactive and NextGeo shadows still
##    render accurately through the legacy shadow atlas.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs = preload("res://scenes/scene_args.gd")

const SIZE := 512
const EXTENT := 4.0
const SETTLE_FRAMES := 10
const SLAB_CENTER := Vector3(-1.0, 1.0, 0.0)
const SLAB_HALF := 0.5
const SLAB_THICKNESS := 0.04
const SLAB_TOP := SLAB_CENTER.y + SLAB_THICKNESS * 0.5
const EDGE_BAND := 0.05
const MASK_AGREEMENT := 0.985
const LIGHT_POSITION := Vector3(-2.0, 2.0, 0.0)
const LIGHT_TARGET := Vector3(0.5, 0.0, 0.0)

const MASKED_STRIPES_SHADER := """
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(0.85, 0.85, 0.85);
	ALPHA = step(0.5, fract(UV.x * 4.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const VERTEX_OFFSET_SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform float expand_scale = 1.25;
void vertex() {
	VERTEX.xz *= expand_scale;
}
void fragment() {
	ALBEDO = vec3(0.85, 0.85, 0.85);
}
"""

# Regression shader for SHADOWS_ONLY programmable VSM casters whose bin is never visited by RENDER_LIST_OPAQUE:
# fails loudly (collapses geometry and discards) if MODEL_MATRIX translation or non-uniform MODEL_NORMAL_MATRIX
# is corrupted in nextgeo_instance_data, or if nextgeo_raster_bin_owners was not updated for VSM.
const SHADOWS_ONLY_PROG_SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform vec3 expected_origin = vec3(-1.0, 1.0, 0.0);
void vertex() {
	if (length(MODEL_MATRIX[3].xyz - expected_origin) > 0.05) {
		VERTEX *= 0.0;
	} else {
		VERTEX.xz *= 1.25;
	}
}
void fragment() {
	ALBEDO = vec3(0.85, 0.85, 0.85);
	if (length(MODEL_MATRIX[3].xyz - expected_origin) > 0.05 || abs(MODEL_NORMAL_MATRIX[1][1] - 0.5) > 0.05) {
		discard;
	}
	ALPHA = 1.0;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

# Regression shader for off-camera (frustum-culled from RENDER_LIST_OPAQUE) ALPHA_HASH programmable VSM casters.
const OFFCAMERA_ALPHA_HASH_SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform vec3 expected_origin = vec3(-2.6, 2.6, 0.0);
void fragment() {
	ALBEDO = vec3(0.85, 0.85, 0.85);
	if (length(MODEL_MATRIX[3].xyz - expected_origin) > 0.05 || abs(MODEL_NORMAL_MATRIX[1][1] - 0.5) > 0.05) {
		discard;
	}
	ALPHA = 1.0;
	ALPHA_HASH_SCALE = 1.0;
}
"""

var _mode := "vsm"
var _viewport: SubViewport
var _camera: Camera3D
var _floor: MeshInstance3D
var _caster: MeshInstance3D
var _light: Light3D
var _temp_files: Array[String] = []
var _ngm_counter := 0

var _vmesh_slab: ArrayMesh
var _vmesh_quad_single: ArrayMesh
var _vmesh_masked: ArrayMesh
var _vmesh_wpo: ArrayMesh

var _world_x := PackedFloat32Array()
var _world_z := PackedFloat32Array()
var _failures := PackedStringArray()
var _result := {}


func _ready() -> void:
	var vsm_setting := int(ProjectSettings.get_setting("rendering/lights_and_shadows/shadow_method", 0))
	var default_mode := "vsm" if (OS.get_environment("GODOT_DEV_VSM") != "" or vsm_setting == 1) else "atlas"
	_mode = SceneArgs.value("--shadows", default_mode)
	var hw_raster_arg := SceneArgs.value("--hw_raster_path", "").strip_edges().to_lower()
	if hw_raster_arg != "":
		var hw_raster_enum := 0
		if hw_raster_arg == "vertex" or hw_raster_arg == "vs":
			hw_raster_enum = 1
		elif hw_raster_arg == "mesh":
			hw_raster_enum = 2
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", hw_raster_enum)
	_result["shadows"] = _mode
	_result["hw_raster_path_arg"] = hw_raster_arg if hw_raster_arg != "" else "default"
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()

	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 1.0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/debug_nanite_dynamic_layer", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_hiz", true)

	_build_scene()
	_run.call_deferred()


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://vsm_nanite_%s_%d.ngm" % [p_tag, _ngm_counter]
	_temp_files.append(path)
	return path


func _make_virtual_mesh(p_prim: PrimitiveMesh, p_tag: String) -> ArrayMesh:
	var imp := ImporterMesh.from_mesh(p_prim)
	var err := imp.build_virtual_geometry(_alloc_temp_ngm(p_tag))
	if err != OK:
		_failures.append("%s: build_virtual_geometry failed (err=%d)" % [p_tag, err])
		return null
	var mesh := imp.get_mesh()
	if mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("%s: virtual geometry missing on built mesh" % p_tag)
		return null
	return mesh


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = env
	add_child(world)

	_viewport = SubViewport.new()
	_viewport.size = Vector2i(SIZE, SIZE)
	_viewport.use_hdr_2d = true
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.msaa_3d = Viewport.MSAA_DISABLED
	_viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	_viewport.use_taa = false
	add_child(_viewport)

	_camera = Camera3D.new()
	_camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	_camera.size = EXTENT
	_camera.near = 0.05
	_camera.far = 12.0
	_camera.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 4.0, 0.0))
	_viewport.add_child(_camera)
	_camera.current = true

	var white := StandardMaterial3D.new()
	white.albedo_color = Color(1.0, 1.0, 1.0)
	white.roughness = 1.0
	white.metallic = 0.0
	white.metallic_specular = 0.0

	_floor = MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(14.0, 14.0)
	_floor.mesh = plane
	_floor.material_override = white
	_floor.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_floor)

	# High-subdivision virtual slab so build_virtual_geometry emits many fine clusters & multiple pages.
	var slab_box := BoxMesh.new()
	slab_box.size = Vector3(SLAB_HALF * 2.0, SLAB_THICKNESS, SLAB_HALF * 2.0)
	slab_box.subdivide_width = 40
	slab_box.subdivide_depth = 40
	slab_box.subdivide_height = 2
	slab_box.material = white
	_vmesh_slab = _make_virtual_mesh(slab_box, "slab")

	var single_mat := StandardMaterial3D.new()
	single_mat.albedo_color = Color(1.0, 1.0, 1.0)
	single_mat.roughness = 1.0
	single_mat.metallic = 0.0
	single_mat.cull_mode = BaseMaterial3D.CULL_BACK
	var qm := QuadMesh.new()
	qm.size = Vector2(SLAB_HALF * 2.0, SLAB_HALF * 2.0)
	qm.subdivide_width = 48
	qm.subdivide_depth = 48
	qm.material = single_mat
	_vmesh_quad_single = _make_virtual_mesh(qm, "quad_single")

	var masked_shader := Shader.new()
	masked_shader.code = MASKED_STRIPES_SHADER
	var masked_mat := ShaderMaterial.new()
	masked_mat.shader = masked_shader
	var mp := PlaneMesh.new()
	mp.size = Vector2(SLAB_HALF * 2.0, SLAB_HALF * 2.0)
	mp.subdivide_width = 48
	mp.subdivide_depth = 48
	mp.material = masked_mat
	_vmesh_masked = _make_virtual_mesh(mp, "masked")

	var wpo_shader := Shader.new()
	wpo_shader.code = VERTEX_OFFSET_SHADER
	var wpo_mat := ShaderMaterial.new()
	wpo_mat.shader = wpo_shader
	var wp := PlaneMesh.new()
	wp.size = Vector2(0.8, 0.8)
	wp.subdivide_width = 48
	wp.subdivide_depth = 48
	wp.material = wpo_mat
	_vmesh_wpo = _make_virtual_mesh(wp, "wpo")

	_caster = MeshInstance3D.new()
	_caster.mesh = _vmesh_slab
	_caster.position = SLAB_CENTER
	add_child(_caster)

	_world_x.resize(SIZE)
	_world_z.resize(SIZE)
	for i in SIZE:
		var u := (float(i) + 0.5) / float(SIZE) - 0.5
		_world_x[i] = u * EXTENT
		_world_z[i] = u * EXTENT


func _new_light(p_kind: String, p_reverse_cull: bool = false, p_straight_down: bool = false) -> Light3D:
	var light: Light3D
	match p_kind:
		"sun":
			var sun := DirectionalLight3D.new()
			sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
			sun.directional_shadow_max_distance = 12.0
			light = sun
		"spot":
			var spot := SpotLight3D.new()
			spot.spot_angle = 60.0
			spot.spot_range = 10.0
			light = spot
		"omni":
			var omni := OmniLight3D.new()
			omni.omni_range = 10.0
			light = omni
		_:
			var area := AreaLight3D.new()
			area.area_range = 10.0
			area.area_size = Vector2(0.01, 0.01)
			light = area
	light.shadow_enabled = true
	light.shadow_reverse_cull_face = p_reverse_cull
	add_child(light)
	if p_kind == "sun":
		if p_straight_down:
			light.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 5.0, 0.0))
		else:
			light.transform = Transform3D(Basis.looking_at(Vector3(1.0, -1.0, 0.0).normalized(), Vector3.UP), Vector3(0.0, 5.0, 0.0))
	else:
		light.light_energy = 4.0
		light.look_at_from_position(LIGHT_POSITION, LIGHT_TARGET, Vector3.UP)
	return light


func _frames(p_count: int) -> void:
	for _i in p_count:
		await RenderingServer.frame_post_draw


func _capture() -> PackedFloat32Array:
	var img := _viewport.get_texture().get_image()
	img.convert(Image.FORMAT_RF)
	return img.get_data().to_float32_array()


# Signed distance to the analytic hard shadow rectangle on y = 0: negative inside, positive outside.
func _shadow_distance(p_kind: String, p_x: float, p_z: float, p_half: float = SLAB_HALF, p_center: Vector3 = SLAB_CENTER, p_top: float = SLAB_TOP) -> float:
	var x_min: float
	var x_max: float
	var z_min: float
	var z_max: float
	if p_kind == "sun":
		x_min = p_center.x - p_half + p_top
		x_max = p_center.x + p_half + p_top
		z_min = p_center.z - p_half
		z_max = p_center.z + p_half
	else:
		var k := LIGHT_POSITION.y / (LIGHT_POSITION.y - p_top)
		x_min = LIGHT_POSITION.x + k * (p_center.x - p_half - LIGHT_POSITION.x)
		x_max = LIGHT_POSITION.x + k * (p_center.x + p_half - LIGHT_POSITION.x)
		z_min = LIGHT_POSITION.z + k * (p_center.z - p_half - LIGHT_POSITION.z)
		z_max = LIGHT_POSITION.z + k * (p_center.z + p_half - LIGHT_POSITION.z)
	var dx := maxf(x_min - p_x, p_x - x_max)
	var dz := maxf(z_min - p_z, p_z - z_max)
	return maxf(dx, dz)


func _compare_analytic(p_kind: String, p_ref: PackedFloat32Array, p_lit: PackedFloat32Array, p_half: float = SLAB_HALF, p_center: Vector3 = SLAB_CENTER, p_top: float = SLAB_TOP) -> Dictionary:
	# Legacy AreaLight3D shadows use a filtered dual-paraboloid atlas map with a wider filter band;
	# VSM keeps the strict EDGE_BAND for all four light kinds.
	var edge_band := 0.14 if (_mode == "atlas" and p_kind == "area") else EDGE_BAND
	var checked := 0
	var matched := 0
	var shadowed_pixels := 0
	var lit_pixels := 0
	for py in SIZE:
		var wz := _world_z[py]
		var row := py * SIZE
		for px in SIZE:
			var wx := _world_x[px]
			# Skip the slab's own overhead footprint on the camera image.
			if absf(wx - p_center.x) <= p_half + edge_band and absf(wz - p_center.z) <= p_half + edge_band:
				continue
			var r := p_ref[row + px]
			if r < 0.02:
				continue
			var d := _shadow_distance(p_kind, wx, wz, p_half, p_center, p_top)
			if absf(d) < edge_band:
				continue
			var s := p_lit[row + px] / r
			var expect_shadow := d < 0.0
			checked += 1
			if expect_shadow:
				shadowed_pixels += 1
				if s < 0.5:
					matched += 1
			else:
				lit_pixels += 1
				if s >= 0.5:
					matched += 1
	return {
		"checked": checked,
		"matched": matched,
		"shadowed_pixels": shadowed_pixels,
		"lit_pixels": lit_pixels,
		"agreement": float(matched) / float(maxi(checked, 1)),
	}


func _check_vsm_nanite_invariants(p_label: String, p_info: Dictionary, p_expect_dir: bool, p_expect_local: bool, p_expect_sw: bool, p_expect_hw: bool) -> void:
	if _mode != "vsm":
		if bool(p_info.get("active", false)):
			_failures.append("%s: VSM unexpectedly active in atlas mode" % p_label)
		return
	if not bool(p_info.get("active", false)):
		_failures.append("%s: VSM inactive" % p_label)
		return
	var proj: Dictionary = p_info.get("projection", {})
	var nanite: Dictionary = p_info.get("nanite", {})
	if int(proj.get("atlas_passes", -1)) != 0:
		_failures.append("%s: expected 0 atlas_passes with VSM+NextGeo, got %s" % [p_label, str(proj.get("atlas_passes"))])
	if int(proj.get("lights_sampled", 0)) <= 0:
		_failures.append("%s: expected lights_sampled > 0, got %s" % [p_label, str(proj.get("lights_sampled"))])
	if not bool(nanite.get("active", false)):
		_failures.append("%s: nanite VSM path not active" % p_label)
		return
	if int(nanite.get("directional_chains", 99)) > 2 or int(nanite.get("local_chains", 99)) > 2:
		_failures.append("%s: dispatch chain count exceeded <=2 limit (dir=%s local=%s)" % [
			p_label, str(nanite.get("directional_chains")), str(nanite.get("local_chains"))])
	if bool(nanite.get("gpu_counters_valid", false)):
		if int(nanite.get("overflow", 0)) != 0:
			_failures.append("%s: nanite VSM overflow=%d" % [p_label, int(nanite.get("overflow", 0))])
		if int(nanite.get("sink", 0)) != 0:
			_failures.append("%s: #277 atomic sink tripped (sink=%d)" % [p_label, int(nanite.get("sink", 0))])
		if p_expect_dir and int(nanite.get("directional_clusters", 0)) <= 0:
			_failures.append("%s: expected directional_clusters > 0" % p_label)
		if p_expect_local and int(nanite.get("local_clusters", 0)) <= 0:
			_failures.append("%s: expected local_clusters > 0" % p_label)
		if p_expect_sw and int(nanite.get("sw_clusters", 0)) <= 0:
			_failures.append("%s: expected sw_clusters > 0" % p_label)
		if p_expect_hw and int(nanite.get("hw_clusters", 0)) <= 0:
			_failures.append("%s: expected hw_clusters > 0" % p_label)


func _settle_and_collect(p_count: int) -> Dictionary:
	var peak := {
		"active": false,
		"directional_chains": 0,
		"local_chains": 0,
		"gpu_counters_valid": false,
		"directional_clusters": 0,
		"local_clusters": 0,
		"sw_clusters": 0,
		"hw_clusters": 0,
		"prog_sw_clusters": 0,
		"prog_hw_clusters": 0,
		"sw_writes": 0,
		"hiz_pages_built": 0,
		"hiz_culled_nodes": 0,
		"hiz_culled_clusters": 0,
		"overflow": 0,
		"sink": 0,
		"programmable_bins": 0,
		"hw_raster_path": "",
		"hw_raster_reason": "",
	}
	var last_info := {}
	for _i in p_count:
		await RenderingServer.frame_post_draw
		last_info = RenderingServer.virtual_shadow_maps_get_debug_info()
		var n: Dictionary = last_info.get("nanite", {})
		if bool(n.get("active", false)):
			peak["active"] = true
			peak["directional_chains"] = maxi(int(peak["directional_chains"]), int(n.get("directional_chains", 0)))
			peak["local_chains"] = maxi(int(peak["local_chains"]), int(n.get("local_chains", 0)))
			peak["programmable_bins"] = maxi(int(peak["programmable_bins"]), int(n.get("programmable_bins", 0)))
			if n.has("hw_raster_path"):
				peak["hw_raster_path"] = String(n.get("hw_raster_path", ""))
			if n.has("hw_raster_reason"):
				peak["hw_raster_reason"] = String(n.get("hw_raster_reason", ""))
			if bool(n.get("gpu_counters_valid", false)):
				peak["gpu_counters_valid"] = true
				for k in ["directional_clusters", "local_clusters", "sw_clusters", "hw_clusters", "prog_sw_clusters", "prog_hw_clusters", "sw_writes", "hiz_pages_built", "hiz_culled_nodes", "hiz_culled_clusters", "overflow", "sink"]:
					peak[k] = maxi(int(peak[k]), int(n.get(k, 0)))
	return {
		"active": bool(last_info.get("active", false)),
		"projection": last_info.get("projection", {}),
		"nanite": peak,
	}


func _run_slab_case(p_name: String, p_kind: String, p_sw_max: float) -> Dictionary:
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", p_sw_max)
	_caster.mesh = _vmesh_slab
	_caster.position = SLAB_CENTER
	_caster.rotation = Vector3.ZERO
	_caster.visible = true
	_caster.extra_cull_margin = 0.0

	# Reference without shadow.
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_light = _new_light(p_kind)
	await _frames(SETTLE_FRAMES)
	var ref := _capture()
	_light.queue_free()
	await _frames(2)

	# Shadowed with a fresh light (new virtual maps).
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_light = _new_light(p_kind)
	var collected := await _settle_and_collect(SETTLE_FRAMES)
	var lit := _capture()
	_light.queue_free()
	await _frames(2)

	var cmp := _compare_analytic(p_kind, ref, lit)
	if cmp.shadowed_pixels < 100 or cmp.lit_pixels < 100:
		_failures.append("%s: degenerate floor coverage (shadowed=%d lit=%d)" % [p_name, cmp.shadowed_pixels, cmp.lit_pixels])
	if cmp.agreement < MASK_AGREEMENT:
		_failures.append("%s: shadow mask agreement %.4f < %.4f" % [p_name, cmp.agreement, MASK_AGREEMENT])
	cmp["active"] = collected.active
	cmp["nanite"] = collected.nanite
	cmp["projection"] = collected.projection
	return cmp


func _run() -> void:
	if not _failures.is_empty():
		_finish()
		return

	# Warm up streaming so virtual mesh pages are resident.
	_light = _new_light("sun")
	await _frames(15)
	_light.queue_free()
	await _frames(2)

	var cases_out := {}

	# 1. HW-only hard shadows across all 4 light kinds (directional, spot, omni, area).
	for kind in ["sun", "spot", "omni", "area"]:
		var cname := "hw_%s" % kind
		var cmp := await _run_slab_case(cname, kind, 0.0)
		var info := {"active": _mode == "vsm", "projection": cmp.projection, "nanite": cmp.nanite}
		if _mode == "atlas":
			info = RenderingServer.virtual_shadow_maps_get_debug_info()
		_check_vsm_nanite_invariants(cname, info, kind == "sun", kind != "sun", false, true)
		if _mode == "vsm" and bool(cmp.nanite.get("gpu_counters_valid", false)) and int(cmp.nanite.get("sw_clusters", 0)) != 0:
			_failures.append("%s: sw_max=0 expected sw_clusters==0, got %d" % [cname, int(cmp.nanite.get("sw_clusters", 0))])
		cases_out[cname] = {"agreement": cmp.agreement, "nanite": cmp.nanite}

	# 2. Hybrid SW+HW raster (sw_max = 48.0 routes fine slab clusters to SW; sw_max = 8.0 provisional default).
	for kind in ["sun", "spot"]:
		var cname := "sw_%s" % kind
		var cmp := await _run_slab_case(cname, kind, 48.0)
		var info := {"active": _mode == "vsm", "projection": cmp.projection, "nanite": cmp.nanite}
		if _mode == "atlas":
			info = RenderingServer.virtual_shadow_maps_get_debug_info()
		_check_vsm_nanite_invariants(cname, info, kind == "sun", kind != "sun", true, false)
		if _mode == "vsm" and bool(cmp.nanite.get("gpu_counters_valid", false)) and int(cmp.nanite.get("sw_writes", 0)) <= 0:
			_failures.append("%s: expected sw_writes > 0 in SW raster mode" % cname)
		cases_out[cname] = {"agreement": cmp.agreement, "nanite": cmp.nanite}

	var def_cmp := await _run_slab_case("default_8px_sun", "sun", 8.0)
	cases_out["default_8px_sun"] = {"agreement": def_cmp.agreement, "nanite": def_cmp.nanite}

	# 3. Casting modes (OFF, SHADOWS_ONLY, DOUBLE_SIDED) & reverse_cull_face.
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 8.0)
	await _test_casting_modes(cases_out)

	if _mode == "vsm":
		# 4. Step 3 (Per-page 1-level Hi-Z) & Step 6 (Dynamic layer routing hook).
		await _test_hiz_and_dynamic_layer(cases_out)

		# 5. Step 7 (Programmable raster bins: masked stripes & WPO expand).
		await _test_programmable_bins(cases_out)

		# 6. VSM_OVERDRAW debug draw with NextGeo pairs (MODE_NANITE_PAIRS).
		_light = _new_light("sun")
		_caster.mesh = _vmesh_slab
		_caster.position = SLAB_CENTER
		_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		_viewport.debug_draw = Viewport.DEBUG_DRAW_VSM_OVERDRAW
		await _frames(4)
		_viewport.debug_draw = Viewport.DEBUG_DRAW_DISABLED
		await _frames(2)
		_light.queue_free()
		await _frames(2)
		cases_out["vsm_overdraw_smoke"] = true

	_result["cases"] = cases_out
	_finish()


func _test_casting_modes(r_out: Dictionary) -> void:
	# Reference without shadow.
	_caster.mesh = _vmesh_slab
	_caster.position = SLAB_CENTER
	_caster.rotation = Vector3.ZERO
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_light = _new_light("sun")
	await _frames(SETTLE_FRAMES)
	var ref := _capture()
	_light.queue_free()
	await _frames(2)

	# SHADOW_CASTING_SETTING_SHADOWS_ONLY: casts shadow AND leaves caster's own footprint lit (invisible).
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	_light = _new_light("sun")
	await _frames(SETTLE_FRAMES)
	var so_img := _capture()
	_light.queue_free()
	await _frames(2)
	var so_cmp := _compare_analytic("sun", ref, so_img)
	if so_cmp.agreement < MASK_AGREEMENT:
		_failures.append("shadows_only: shadow agreement %.4f < %.4f" % [so_cmp.agreement, MASK_AGREEMENT])
	r_out["shadows_only"] = {"agreement": so_cmp.agreement}

	# Single-sided quad facing DOWN (back-facing to top-down sun):
	# QuadMesh faces +Z; rotating +90 deg around X turns +Z to -Y (downward, away from sun).
	_caster.mesh = _vmesh_quad_single
	_caster.position = SLAB_CENTER
	_caster.rotation_degrees = Vector3(90.0, 0.0, 0.0)

	# CAST_SHADOW_ON without reverse_cull -> back-facing -> casts NO shadow.
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_light = _new_light("sun", false)
	await _frames(SETTLE_FRAMES)
	var back_off_img := _capture()
	_light.queue_free()
	await _frames(2)
	var back_off_shadowed := _shadow_fraction("sun", ref, back_off_img)
	if back_off_shadowed > 0.05:
		_failures.append("backfacing_on: expected no shadow (<0.05), got %.4f" % back_off_shadowed)

	# CAST_SHADOW_DOUBLE_SIDED -> casts full shadow.
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_DOUBLE_SIDED
	_light = _new_light("sun", false)
	await _frames(SETTLE_FRAMES)
	var ds_img := _capture()
	_light.queue_free()
	await _frames(2)
	var ds_cmp := _compare_analytic("sun", ref, ds_img, SLAB_HALF, SLAB_CENTER, SLAB_CENTER.y)
	if ds_cmp.agreement < MASK_AGREEMENT:
		_failures.append("double_sided: agreement %.4f < %.4f" % [ds_cmp.agreement, MASK_AGREEMENT])

	# CAST_SHADOW_ON with shadow_reverse_cull_face = true -> back-facing quad now casts full shadow!
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_light = _new_light("sun", true)
	await _frames(SETTLE_FRAMES)
	var rev_img := _capture()
	_light.queue_free()
	await _frames(2)
	var rev_cmp := _compare_analytic("sun", ref, rev_img, SLAB_HALF, SLAB_CENTER, SLAB_CENTER.y)
	if rev_cmp.agreement < MASK_AGREEMENT:
		_failures.append("reverse_cull_face: agreement %.4f < %.4f" % [rev_cmp.agreement, MASK_AGREEMENT])

	r_out["culling"] = {
		"backfacing_shadow_fraction": back_off_shadowed,
		"double_sided_agreement": ds_cmp.agreement,
		"reverse_cull_agreement": rev_cmp.agreement,
	}
	_caster.rotation = Vector3.ZERO
	_caster.mesh = _vmesh_slab


func _shadow_fraction(p_kind: String, p_ref: PackedFloat32Array, p_lit: PackedFloat32Array, p_half: float = SLAB_HALF, p_center: Vector3 = SLAB_CENTER, p_top: float = SLAB_TOP) -> float:
	var inside := 0
	var dark := 0
	for py in SIZE:
		var wz := _world_z[py]
		var row := py * SIZE
		for px in SIZE:
			var wx := _world_x[px]
			if absf(wx - p_center.x) <= p_half + EDGE_BAND and absf(wz - p_center.z) <= p_half + EDGE_BAND:
				continue
			var r := p_ref[row + px]
			if r < 0.02:
				continue
			var d := _shadow_distance(p_kind, wx, wz, p_half, p_center, p_top)
			if d < -EDGE_BAND:
				inside += 1
				if p_lit[row + px] / r < 0.5:
					dark += 1
	return float(dark) / float(maxi(inside, 1))


func _test_hiz_and_dynamic_layer(r_out: Dictionary) -> void:
	# Enable Step 3 + Step 6 GPU verification hook.
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/debug_nanite_dynamic_layer", true)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_hiz", true)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 8.0)

	# Hide reused _caster and create a fresh stationary static occluder wall (never moved -> layer 0)
	# covering [-1.2, 1.2]^2 at y = 2.0 under straight-down sun.
	_caster.visible = false
	var static_wall := MeshInstance3D.new()
	static_wall.mesh = _vmesh_slab
	static_wall.position = Vector3(0.0, 2.0, 0.0)
	static_wall.scale = Vector3(2.4, 1.0, 2.4)
	static_wall.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	add_child(static_wall)

	var dyn := MeshInstance3D.new()
	dyn.mesh = _vmesh_slab
	dyn.scale = Vector3(0.5, 1.0, 0.5)
	dyn.position = Vector3(0.0, 0.5, 0.0) # Strictly behind static wall (2.0 m > 0.5 m).
	dyn.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	add_child(dyn)

	# Let static_wall's transform settle (prev_transform == transform -> static layer 0) before creating the light.
	await _frames(3)
	_light = _new_light("sun", false, true)
	var init_collected := await _settle_and_collect(6)
	var max_hiz_built := int(init_collected.nanite.get("hiz_pages_built", 0))
	var max_hiz_culled := int(init_collected.nanite.get("hiz_culled_nodes", 0)) + int(init_collected.nanite.get("hiz_culled_clusters", 0))

	# Move dynamic caster slightly every frame behind the static wall so it routes to layer 1 (RECORD_MOVED)
	# and hits cached static pages whose Hi-Z was built on the initial frames.
	for step in 12:
		dyn.position = Vector3(-0.2 + float(step % 4) * 0.1, 0.5, 0.0)
		await _frames(1)
		var n: Dictionary = RenderingServer.virtual_shadow_maps_get_debug_info().get("nanite", {})
		max_hiz_built = maxi(max_hiz_built, int(n.get("hiz_pages_built", 0)))
		max_hiz_culled = maxi(max_hiz_culled, int(n.get("hiz_culled_nodes", 0)) + int(n.get("hiz_culled_clusters", 0)))
		if int(n.get("sink", 0)) != 0:
			_failures.append("hiz_dynamic: #277 atomic sink tripped")

	if max_hiz_built <= 0:
		_failures.append("hiz_dynamic: expected hiz_pages_built > 0, got %d" % max_hiz_built)
	if max_hiz_culled <= 0:
		_failures.append("hiz_dynamic: expected GPU Hi-Z culling of occluded dynamic caster > 0, got %d" % max_hiz_culled)

	# Move dynamic caster out into the open at x = 1.55 (outside wall's [-1.2, 1.2]) and verify layer 1 casts shadow
	# while actively moving (transform != prev_transform on the captured frame)!
	dyn.position = Vector3(1.54, 0.5, 0.0)
	await _frames(1)
	dyn.position = Vector3(1.56, 0.5, 0.0)
	await _frames(1)
	var open_img := _capture()
	# Check shadow at (1.56, 0) (under dyn, outside wall) vs lit floor at (-1.6, 0).
	var dyn_px := int((1.56 / EXTENT + 0.5) * float(SIZE))
	var lit_px := int((-1.6 / EXTENT + 0.5) * float(SIZE))
	var mid_py := SIZE / 2
	var dyn_val := open_img[mid_py * SIZE + clampi(dyn_px, 0, SIZE - 1)]
	var lit_val := open_img[mid_py * SIZE + clampi(lit_px, 0, SIZE - 1)]
	if not (lit_val > 0.1 and dyn_val < lit_val * 0.25):
		_failures.append("hiz_dynamic: open dynamic caster failed to cast shadow from layer 1 (dyn=%.3f lit=%.3f)" % [dyn_val, lit_val])

	# Regression: a VSM-marking view where _vsm_raster_nanite() does not run (MSAA disables NextGeo)
	# must refresh/invalidate per-page Hi-Z so removing the static wall and scrolling/updating the map
	# never leaves stale static depths that falsely ghost-cull the dynamic caster on subsequent frames.
	static_wall.visible = false
	dyn.position = Vector3(0.0, 0.5, 0.0)
	_viewport.msaa_3d = Viewport.MSAA_4X
	_light.transform = Transform3D(Basis.looking_at(Vector3(0.01, -1.0, 0.0).normalized(), Vector3.FORWARD), Vector3(0.0, 5.0, 0.0))
	await _frames(4)
	_viewport.msaa_3d = Viewport.MSAA_DISABLED
	dyn.position = Vector3(0.02, 0.5, 0.0)
	await _frames(1)
	dyn.position = Vector3(0.0, 0.5, 0.0)
	await _frames(1)
	var after_msaa_img := _capture()
	var center_px := SIZE / 2
	var msaa_dyn_val := after_msaa_img[mid_py * SIZE + center_px]
	var msaa_lit_val := after_msaa_img[mid_py * SIZE + clampi(lit_px, 0, SIZE - 1)]
	if not (msaa_lit_val > 0.1 and msaa_dyn_val < msaa_lit_val * 0.25):
		_failures.append("hiz_dynamic: stale Hi-Z ghost-culled dynamic caster after MSAA view (dyn=%.3f lit=%.3f)" % [msaa_dyn_val, msaa_lit_val])

	dyn.queue_free()
	static_wall.queue_free()
	_light.queue_free()
	_caster.visible = true
	_caster.scale = Vector3.ONE
	_caster.position = SLAB_CENTER
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/debug_nanite_dynamic_layer", false)
	await _frames(3)

	r_out["hiz_dynamic"] = {
		"max_hiz_pages_built": max_hiz_built,
		"max_hiz_culled": max_hiz_culled,
		"open_dyn_val": dyn_val,
		"open_lit_val": lit_val,
		"after_msaa_dyn_val": msaa_dyn_val,
		"after_msaa_lit_val": msaa_lit_val,
	}


func _test_programmable_bins(r_out: Dictionary) -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	await _frames(2)

	# Reference without shadow.
	_caster.mesh = _vmesh_masked
	_caster.position = SLAB_CENTER
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_light = _new_light("sun")
	await _frames(SETTLE_FRAMES)
	var ref := _capture()
	_light.queue_free()
	await _frames(2)

	var prog_out := {}
	# Masked alpha-scissor stripes (both HW and SW).
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	for mode_pair in [["masked_hw", 0.0], ["masked_sw", 24.0]]:
		var label: String = mode_pair[0]
		var sw_max: float = mode_pair[1]
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", sw_max)
		_light = _new_light("sun")
		var collected := await _settle_and_collect(SETTLE_FRAMES)
		var img := _capture()
		var nanite: Dictionary = collected.nanite
		_light.queue_free()
		await _frames(2)
		var frac := _shadow_fraction("sun", ref, img, SLAB_HALF, SLAB_CENTER, SLAB_CENTER.y)
		if frac < 0.25 or frac > 0.75:
			_failures.append("%s: expected masked stripe shadow fraction in [0.25, 0.75], got %.4f" % [label, frac])
		if bool(nanite.get("gpu_counters_valid", false)) and int(nanite.get("sink", 0)) != 0:
			_failures.append("%s: #277 atomic sink tripped" % label)
		if int(nanite.get("programmable_bins", 0)) > 0 and bool(nanite.get("gpu_counters_valid", false)):
			var expected_cnt := int(nanite.get("prog_hw_clusters", 0)) if sw_max == 0.0 else int(nanite.get("prog_sw_clusters", 0))
			if expected_cnt <= 0:
				_failures.append("%s: expected programmable VSM bin clusters > 0, got %d" % [label, expected_cnt])
		prog_out[label] = {
			"shadow_fraction": frac,
			"programmable_bins": int(nanite.get("programmable_bins", 0)),
			"prog_hw_clusters": int(nanite.get("prog_hw_clusters", 0)),
			"prog_sw_clusters": int(nanite.get("prog_sw_clusters", 0)),
		}

	# WPO expanded slab (0.8 m mesh expanded 1.25x -> 1.0 m shadow).
	_caster.mesh = _vmesh_wpo
	_caster.position = SLAB_CENTER
	_caster.extra_cull_margin = 0.5
	_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 8.0)
	_light = _new_light("sun")
	var wpo_collected := await _settle_and_collect(SETTLE_FRAMES)
	var wpo_img := _capture()
	var wpo_nanite: Dictionary = wpo_collected.nanite
	_light.queue_free()
	await _frames(2)
	var wpo_cmp := _compare_analytic("sun", ref, wpo_img, SLAB_HALF, SLAB_CENTER, SLAB_CENTER.y)
	if wpo_cmp.agreement < 0.97:
		_failures.append("wpo_expand: expanded 1.0 m shadow agreement %.4f < 0.97" % wpo_cmp.agreement)
	prog_out["wpo_expand"] = {
		"agreement": wpo_cmp.agreement,
		"programmable_bins": int(wpo_nanite.get("programmable_bins", 0)),
		"prog_hw_clusters": int(wpo_nanite.get("prog_hw_clusters", 0)),
		"prog_sw_clusters": int(wpo_nanite.get("prog_sw_clusters", 0)),
	}

	# Regression: SHADOWS_ONLY programmable-bin caster whose material/bin is never visible in RENDER_LIST_OPAQUE,
	# verifying nextgeo_raster_bin_owners registration, transposed 3x4 MODEL_MATRIX translation, and non-uniform
	# scale MODEL_NORMAL_MATRIX in both HW and SW bins.
	_caster.visible = false
	_light = _new_light("sun")
	await _frames(SETTLE_FRAMES)
	var ref_empty := _capture()
	_light.queue_free()
	await _frames(2)

	var so_shader := Shader.new()
	so_shader.code = SHADOWS_ONLY_PROG_SHADER
	var so_mat := ShaderMaterial.new()
	so_mat.shader = so_shader
	var so_plane := PlaneMesh.new()
	so_plane.size = Vector2(0.8, 0.8)
	so_plane.subdivide_width = 48
	so_plane.subdivide_depth = 48
	so_plane.material = so_mat
	var so_vmesh := _make_virtual_mesh(so_plane, "shadows_only_prog")
	var so_caster := MeshInstance3D.new()
	so_caster.mesh = so_vmesh
	so_caster.position = SLAB_CENTER
	so_caster.scale = Vector3(1.0, 2.0, 1.0)
	so_caster.extra_cull_margin = 0.5
	so_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	add_child(so_caster)
	await _frames(4)
	for mode_pair in [["shadows_only_prog_hw", 0.0], ["shadows_only_prog_sw", 24.0]]:
		var label: String = mode_pair[0]
		var sw_max: float = mode_pair[1]
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", sw_max)
		_light = _new_light("sun")
		var so_collected := await _settle_and_collect(SETTLE_FRAMES)
		var so_img := _capture()
		var so_nanite: Dictionary = so_collected.nanite
		_light.queue_free()
		await _frames(2)
		var so_cmp := _compare_analytic("sun", ref_empty, so_img, SLAB_HALF, SLAB_CENTER, SLAB_CENTER.y)
		if so_cmp.agreement < 0.97:
			_failures.append("%s: SHADOWS_ONLY programmable caster agreement %.4f < 0.97" % [label, so_cmp.agreement])
		if bool(so_nanite.get("gpu_counters_valid", false)) and int(so_nanite.get("sink", 0)) != 0:
			_failures.append("%s: #277 atomic sink tripped" % label)
		if int(so_nanite.get("programmable_bins", 0)) > 0 and bool(so_nanite.get("gpu_counters_valid", false)):
			var expected_cnt := int(so_nanite.get("prog_hw_clusters", 0)) if sw_max == 0.0 else int(so_nanite.get("prog_sw_clusters", 0))
			if expected_cnt <= 0:
				_failures.append("%s: expected programmable VSM bin clusters > 0, got %d" % [label, expected_cnt])
		prog_out[label] = {
			"agreement": so_cmp.agreement,
			"programmable_bins": int(so_nanite.get("programmable_bins", 0)),
			"prog_hw_clusters": int(so_nanite.get("prog_hw_clusters", 0)),
			"prog_sw_clusters": int(so_nanite.get("prog_sw_clusters", 0)),
		}
	so_caster.queue_free()
	await _frames(2)

	# Regression: off-camera (frustum-culled from RENDER_LIST_OPAQUE) SHADOW_CASTING_SETTING_ON ALPHA_HASH caster
	# outside the camera's [-2, 2] orthogonal view casting its shadow onto x in [-0.5, +0.5] inside the view.
	var offcam_origin := Vector3(-2.6, 2.6, 0.0)
	var off_shader := Shader.new()
	off_shader.code = OFFCAMERA_ALPHA_HASH_SHADER
	var off_mat := ShaderMaterial.new()
	off_mat.shader = off_shader
	var off_plane := PlaneMesh.new()
	off_plane.size = Vector2(SLAB_HALF * 2.0, SLAB_HALF * 2.0)
	off_plane.subdivide_width = 48
	off_plane.subdivide_depth = 48
	off_plane.material = off_mat
	var off_vmesh := _make_virtual_mesh(off_plane, "offcam_alpha_hash")
	var off_caster := MeshInstance3D.new()
	off_caster.mesh = off_vmesh
	off_caster.position = offcam_origin
	off_caster.scale = Vector3(1.0, 2.0, 1.0)
	off_caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	add_child(off_caster)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 8.0)
	await _frames(4)
	_light = _new_light("sun")
	var off_collected := await _settle_and_collect(SETTLE_FRAMES)
	var off_img := _capture()
	var off_nanite: Dictionary = off_collected.nanite
	_light.queue_free()
	await _frames(2)
	var off_cmp := _compare_analytic("sun", ref_empty, off_img, SLAB_HALF, offcam_origin, offcam_origin.y)
	if off_cmp.agreement < 0.97:
		_failures.append("offcamera_alpha_hash: off-camera programmable caster agreement %.4f < 0.97" % off_cmp.agreement)
	if bool(off_nanite.get("gpu_counters_valid", false)) and int(off_nanite.get("sink", 0)) != 0:
		_failures.append("offcamera_alpha_hash: #277 atomic sink tripped")
	if int(off_nanite.get("programmable_bins", 0)) > 0 and bool(off_nanite.get("gpu_counters_valid", false)):
		if int(off_nanite.get("prog_hw_clusters", 0)) + int(off_nanite.get("prog_sw_clusters", 0)) <= 0:
			_failures.append("offcamera_alpha_hash: expected programmable VSM bin clusters > 0")
	prog_out["offcamera_alpha_hash"] = {
		"agreement": off_cmp.agreement,
		"programmable_bins": int(off_nanite.get("programmable_bins", 0)),
		"prog_hw_clusters": int(off_nanite.get("prog_hw_clusters", 0)),
		"prog_sw_clusters": int(off_nanite.get("prog_sw_clusters", 0)),
	}
	off_caster.queue_free()
	_caster.visible = true

	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)
	_caster.extra_cull_margin = 0.0
	_caster.mesh = _vmesh_slab
	await _frames(2)
	r_out["programmable"] = prog_out


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_temp_files.clear()


func _finish() -> void:
	_cleanup_temp_files()
	_result["ok"] = _failures.is_empty()
	_result["failures"] = Array(_failures)
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
