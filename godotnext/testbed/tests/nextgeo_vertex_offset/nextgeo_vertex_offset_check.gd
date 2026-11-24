extends Node3D
## NextGeo vertex offset (UE WPO) end-to-end GPU check (#164, plan §21.4 contract N5, GN-3, GN-4, GN-12).
##
## Runs with `rendering/nextgeo/debug_programmable_raster = false` (default) to verify every #164
## deliverable on real virtualized NextGeo meshes:
## 1. Default routing & rendering parity vs Forward+ across HW VS, SW, VIS64 buffer, and HW Mesh:
##    VERTEX writes route virtual (VERTEX_OFFSET bin) with default inheritance from
##    rendering/nextgeo/vertex_offset_max_default (0.5 m); POSITION, MODELVIEW_MATRIX, and
##    PROJECTION_MATRIX writes remain legacy (unsupported_custom_shader).
## 2. Offset clamping & clamped vertex debug counter: offsets exceeding Material.vertex_offset_max
##    are clamped in HW VS, SW, and HW Mesh, increment vertex_offset_clamped, and clamp the silhouette;
##    raising vertex_offset_max above the offset drops vertex_offset_clamped to 0 and matches Forward+.
## 3. GeometryInstance3D.vertex_offset_disable_distance: past distance, 100% of WPO clusters demote
##    to FIXED bin 0, visibility resolve skips vertex() offset, and velocity drops to zero.
## 4. Stage A motion vectors: animated WPO on a stationary mesh writes non-zero accurate velocity
##    matching Forward+ legacy velocity (in both Tile Quads and Compute Bins) while keeping stationary
##    FIXED background velocity at zero.
## 5. VSM dynamic shadow tracking (GODOT_DEV_VSM=1): active WPO routes to VSM VERTEX_OFFSET bin
##    (HW VS, SW, and HW Mesh), marks instances dynamic so cached VSM pages invalidate as WPO shifts
##    without moving the node or light, and respects vertex_offset_disable_distance in VSM views.
## 6. Opt-in GeometryInstance3D.rt_evaluate_vertex_offset (when NextGI HWRT is enabled): compute pass
##    evaluates vertex() on the RT proxy and UPDATE-refits its BLAS so HWRT reflection rays hit the
##    displaced silhouette matching rasterization, and respects vertex_offset_disable_distance.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_sw_raster/nextgeo_sw_raster_probe.gd")

const SETTLE_FRAMES := 14
const VSM_SETTLE_FRAMES := 10
const RT_SETTLE_FRAMES := 16
const VSM_SIZE := 256
const VSM_EXTENT := 4.0

const WPO_WAVE_SHADER := """
shader_type spatial;
void vertex() {
	VERTEX.x += sin(VERTEX.y * 4.0) * 0.25;
	VERTEX.y += 0.25;
}
void fragment() {
	ALBEDO = vec3(0.20, 0.78, 0.35);
	ROUGHNESS = 0.85;
}
"""

const WPO_MASKED_SHADER := """
shader_type spatial;
void vertex() {
	VERTEX.x += 0.25;
}
void fragment() {
	ALBEDO = vec3(0.85, 0.55, 0.18);
	ALPHA = step(0.5, fract(UV.x * 6.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

const LEGACY_POSITION_SHADER := """
shader_type spatial;
void vertex() {
	POSITION = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
}
void fragment() {
	ALBEDO = vec3(0.75, 0.25, 0.25);
}
"""

const LEGACY_MODELVIEW_SHADER := """
shader_type spatial;
void vertex() {
	MODELVIEW_MATRIX = VIEW_MATRIX * MODEL_MATRIX;
}
void fragment() {
	ALBEDO = vec3(0.75, 0.25, 0.65);
}
"""

const LEGACY_PROJECTION_SHADER := """
shader_type spatial;
void vertex() {
	PROJECTION_MATRIX = PROJECTION_MATRIX;
}
void fragment() {
	ALBEDO = vec3(0.35, 0.35, 0.85);
}
"""

const WPO_CLAMP_SHADER := """
shader_type spatial;
uniform float offset_x = 1.2;
void vertex() {
	VERTEX.x += offset_x;
}
void fragment() {
	ALBEDO = vec3(0.25, 0.65, 0.90);
	ROUGHNESS = 0.85;
}
"""

const WPO_ANIM_ORBIT_SHADER := """
shader_type spatial;
uniform float base_x = 0.0;
uniform float amp = 0.35;
uniform float speed = 3.0;
void vertex() {
	float a = TIME * speed;
	VERTEX.x += base_x + cos(a) * amp;
	VERTEX.y += sin(a) * amp;
}
void fragment() {
	ALBEDO = vec3(0.90, 0.35, 0.25);
	ROUGHNESS = 0.85;
}
"""

const VSM_WPO_SHADER := """
shader_type spatial;
uniform float shift_x = 0.0;
void vertex() {
	VERTEX.x += shift_x + 0.0005 * sin(TIME);
}
void fragment() {
	ALBEDO = vec3(0.9, 0.9, 0.9);
}
"""

const RT_WPO_SHADER := """
shader_type spatial;
uniform float shift_x = 0.90;
void vertex() {
	VERTEX.x += shift_x;
}
void fragment() {
	ALBEDO = vec3(0.95, 0.25, 0.15);
	EMISSION = vec3(0.5, 0.1, 0.05);
	ROUGHNESS = 0.8;
}
"""

var _probe: CompositorEffect
var _world_env: WorldEnvironment
var _env: Environment
var _camera: Camera3D
var _sun: DirectionalLight3D
var _box_mesh: Mesh
var _plane_mesh: Mesh
var _sphere_mesh: Mesh
var _instances: Dictionary = {}
var _records: Dictionary = {}
var _temp_files: Array[String] = []
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_bins := false
var _expect_mesh := false
var _require_rt := false


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg == "--require-rt":
			_require_rt = true

	var default_wpo_max := float(ProjectSettings.get_setting("rendering/nextgeo/vertex_offset_max_default", -1.0))
	if absf(default_wpo_max - 0.5) > 0.001:
		_failures.append("project default rendering/nextgeo/vertex_offset_max_default=%.3f expected 0.5" % default_wpo_max)

	# Explicitly verify #164 works with debug_programmable_raster OFF.
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 2)
	ProjectSettings.set_setting("rendering/nextgeo/vertex_offset_max_default", 0.5)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_hiz", true)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	var mesh_shader := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER)
	var mesh_indirect := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER_INDIRECT)
	_expect_bins = indirect_count and (image_int64 or buffer_int64)
	_expect_mesh = _expect_bins and mesh_shader and mesh_indirect
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_programmable_bins": _expect_bins,
		"expect_mesh_hw": _expect_mesh,
		"nextgi_enabled": bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)),
		"vertex_offset_max_default": default_wpo_max,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_run.call_deferred()


func _virtualize(p_prim: PrimitiveMesh, p_tag: String) -> Mesh:
	var path := "user://nextgeo_vertex_offset_%s.ngm" % p_tag
	_temp_files.append(path)
	var imp := ImporterMesh.from_mesh(p_prim)
	var err := imp.build_virtual_geometry(path)
	var mesh := imp.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_tag, err])
	return mesh


func _shader_mat(p_code: String, p_wpo_max: float = -1.0) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = p_code
	var mat := ShaderMaterial.new()
	mat.shader = sh
	mat.vertex_offset_max = p_wpo_max
	return mat


func _add_inst(p_name: String, p_mesh: Mesh, p_xform: Transform3D, p_mat: Material) -> MeshInstance3D:
	var inst := MeshInstance3D.new()
	inst.name = p_name
	inst.mesh = p_mesh
	inst.transform = p_xform
	if p_mat != null:
		inst.material_override = p_mat
	add_child(inst)
	_instances[p_name] = inst
	return inst


func _build_scene() -> void:
	var box_prim := BoxMesh.new()
	box_prim.size = Vector3(1.0, 1.0, 1.0)
	var bg_mat := StandardMaterial3D.new()
	bg_mat.albedo_color = Color(0.22, 0.24, 0.28)
	bg_mat.roughness = 1.0
	box_prim.material = bg_mat
	_box_mesh = _virtualize(box_prim, "box")

	var plane_prim := PlaneMesh.new()
	plane_prim.size = Vector2(1.2, 1.2)
	plane_prim.subdivide_width = 40
	plane_prim.subdivide_depth = 40
	_plane_mesh = _virtualize(plane_prim, "plane")

	var sphere_prim := SphereMesh.new()
	sphere_prim.radius = 0.55
	sphere_prim.height = 1.1
	sphere_prim.radial_segments = 96
	sphere_prim.rings = 48
	_sphere_mesh = _virtualize(sphere_prim, "sphere")

	_camera = Camera3D.new()
	_camera.fov = 55.0
	_camera.near = 0.05
	_camera.far = 100.0
	_camera.position = Vector3(0.0, 0.0, 6.2)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3.ZERO)

	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-18.0, 14.0, 0.0)
	_sun.light_energy = 1.2
	_sun.shadow_enabled = false
	add_child(_sun)

	_env = Environment.new()
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.04, 0.05, 0.07)
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.10, 0.10, 0.10)
	_env.ambient_light_energy = 1.0
	_env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var compositor := Compositor.new()
	_probe = ProbeScript.new()
	var effects: Array[CompositorEffect] = [_probe]
	compositor.compositor_effects = effects
	_world_env = WorldEnvironment.new()
	_world_env.environment = _env
	_world_env.compositor = compositor
	add_child(_world_env)

	var face_cam := Basis(Vector3.RIGHT, deg_to_rad(90.0))

	# Fixed-function opaque background behind all test tiles.
	_add_inst("Background", _box_mesh, Transform3D(Basis.from_scale(Vector3(16.0, 10.0, 0.4)), Vector3(0.0, 0.0, -2.5)), null)

	# Admitted WPO meshes (default inheritance -1.0 -> 0.5 m and explicit 0.5 m).
	_add_inst("WpoWave", _sphere_mesh, Transform3D(Basis(), Vector3(-2.4, 0.8, 0.0)), _shader_mat(WPO_WAVE_SHADER, -1.0))
	_add_inst("WpoMasked", _plane_mesh, Transform3D(face_cam, Vector3(0.0, 0.8, 0.0)), _shader_mat(WPO_MASKED_SHADER, 0.5))

	# Legacy fallbacks: POSITION, MODELVIEW_MATRIX, PROJECTION_MATRIX writes must stay legacy.
	_add_inst("LegacyPosition", _sphere_mesh, Transform3D(Basis(), Vector3(-2.4, -1.1, 0.0)), _shader_mat(LEGACY_POSITION_SHADER, 0.5))
	_add_inst("LegacyModelView", _sphere_mesh, Transform3D(Basis(), Vector3(0.0, -1.1, 0.0)), _shader_mat(LEGACY_MODELVIEW_SHADER, 0.5))
	_add_inst("LegacyProjection", _sphere_mesh, Transform3D(Basis(), Vector3(2.4, -1.1, 0.0)), _shader_mat(LEGACY_PROJECTION_SHADER, 0.5))


func _frames(p_n: int) -> void:
	for _i in p_n:
		await RenderingServer.frame_post_draw


func _capture_probe() -> Dictionary:
	_probe.set("capture", {})
	_probe.set("capture_armed", true)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var cap: Dictionary = _probe.get("capture")
	_probe.set("capture", {})
	return cap


func _route_of(p_inst: MeshInstance3D) -> Array:
	var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(p_inst.get_instance())
	var route := String(st.get("route", ""))
	var reason := String(st.get("reason", ""))
	var surfaces: Array = st.get("surfaces", [])
	if not surfaces.is_empty():
		var s0: Dictionary = surfaces[0]
		route = String(s0.get("route", ""))
		reason = String(s0.get("reason", ""))
	_records[p_inst.name] = int(st.get("record_index", -1))
	return [route, reason]


func _pixels_of(p_cap: Dictionary, p_record: int) -> PackedByteArray:
	var size: Vector2i = p_cap["size"]
	var words: PackedInt32Array = p_cap["words"]
	var mask := PackedByteArray()
	mask.resize(size.x * size.y)
	for i in size.x * size.y:
		mask[i] = 1 if words[i * ProbeScript.WORDS_PER_PIXEL] == p_record else 0
	return mask


func _compare_masks(p_ref: PackedByteArray, p_tst: PackedByteArray) -> Dictionary:
	var ref_cnt := 0
	var tst_cnt := 0
	var both := 0
	for i in p_ref.size():
		ref_cnt += p_ref[i]
		tst_cnt += p_tst[i]
		both += 1 if (p_ref[i] == 1 and p_tst[i] == 1) else 0
	return {
		"ref": ref_cnt,
		"tst": tst_cnt,
		"both": both,
		"one_sided": ref_cnt + tst_cnt - 2 * both,
	}


func _record_centroid_and_count(p_cap: Dictionary, p_record: int) -> Dictionary:
	var size: Vector2i = p_cap["size"]
	var words: PackedInt32Array = p_cap["words"]
	var sx := 0.0
	var sy := 0.0
	var count := 0
	for y in size.y:
		for x in size.x:
			if words[(y * size.x + x) * ProbeScript.WORDS_PER_PIXEL] == p_record:
				sx += (float(x) + 0.5) / float(size.x)
				sy += (float(y) + 0.5) / float(size.y)
				count += 1
	if count == 0:
		return {"count": 0, "u": -1.0, "v": -1.0}
	return {"count": count, "u": sx / float(count), "v": sy / float(count)}


func _image_non_bg_centroid(p_img: Image, p_bg: Color, p_thresh: float = 0.06) -> Dictionary:
	var w := p_img.get_width()
	var h := p_img.get_height()
	var sx := 0.0
	var sy := 0.0
	var count := 0
	for y in h:
		for x in w:
			var c := p_img.get_pixel(x, y)
			var d := absf(c.r - p_bg.r) + absf(c.g - p_bg.g) + absf(c.b - p_bg.b)
			if d > p_thresh:
				sx += (float(x) + 0.5) / float(w)
				sy += (float(y) + 0.5) / float(h)
				count += 1
	if count == 0:
		return {"count": 0, "u": -1.0, "v": -1.0}
	return {"count": count, "u": sx / float(count), "v": sy / float(count)}


func _bits_to_float(p_bits: int) -> float:
	var ba := PackedByteArray()
	ba.resize(4)
	ba.encode_s32(0, p_bits)
	return ba.decode_float(0)


func _record_velocity_stats(p_cap: Dictionary, p_record: int) -> Dictionary:
	var size: Vector2i = p_cap["size"]
	var words: PackedInt32Array = p_cap["words"]
	var count := 0
	var sum_mag := 0.0
	var sum_vx := 0.0
	var sum_vy := 0.0
	var sentinel_count := 0
	var non_finite := 0
	for i in size.x * size.y:
		var base := i * ProbeScript.WORDS_PER_PIXEL
		if words[base] != p_record:
			continue
		var vx := _bits_to_float(words[base + 3])
		var vy := _bits_to_float(words[base + 4])
		if is_nan(vx) or is_inf(vx) or is_nan(vy) or is_inf(vy):
			non_finite += 1
			continue
		if absf(vx + 1.0) < 0.002 and absf(vy + 1.0) < 0.002:
			sentinel_count += 1
			continue
		count += 1
		sum_vx += vx
		sum_vy += vy
		sum_mag += sqrt(vx * vx + vy * vy)
	if count == 0:
		return {"count": 0, "mean_mag": 0.0, "mean_vx": 0.0, "mean_vy": 0.0, "sentinels": sentinel_count, "non_finite": non_finite}
	return {
		"count": count,
		"mean_mag": sum_mag / float(count),
		"mean_vx": sum_vx / float(count),
		"mean_vy": sum_vy / float(count),
		"sentinels": sentinel_count,
		"non_finite": non_finite,
	}


func _foreground_velocity_stats_from_image(p_cap: Dictionary, p_img: Image, p_bg: Color) -> Dictionary:
	# For Forward+ legacy reference (where NextGeo instance_id is UINT32_MAX), identify foreground pixels via image color.
	var size: Vector2i = p_cap["size"]
	var words: PackedInt32Array = p_cap["words"]
	var img_size := p_img.get_size()
	var count := 0
	var sum_mag := 0.0
	var sum_vx := 0.0
	var sum_vy := 0.0
	var sentinel_count := 0
	for y in size.y:
		var sy := mini(int((float(y) + 0.5) * float(img_size.y) / float(size.y)), img_size.y - 1)
		for x in size.x:
			var sx := mini(int((float(x) + 0.5) * float(img_size.x) / float(size.x)), img_size.x - 1)
			var c := p_img.get_pixel(sx, sy)
			var d := absf(c.r - p_bg.r) + absf(c.g - p_bg.g) + absf(c.b - p_bg.b)
			if d < 0.12:
				continue
			var base := (y * size.x + x) * ProbeScript.WORDS_PER_PIXEL
			var vx := _bits_to_float(words[base + 3])
			var vy := _bits_to_float(words[base + 4])
			if is_nan(vx) or is_inf(vx) or is_nan(vy) or is_inf(vy):
				continue
			if absf(vx + 1.0) < 0.002 and absf(vy + 1.0) < 0.002:
				sentinel_count += 1
				continue
			count += 1
			sum_vx += vx
			sum_vy += vy
			sum_mag += sqrt(vx * vx + vy * vy)
	if count == 0:
		return {"count": 0, "mean_mag": 0.0, "mean_vx": 0.0, "mean_vy": 0.0, "sentinels": sentinel_count}
	return {
		"count": count,
		"mean_mag": sum_mag / float(count),
		"mean_vx": sum_vx / float(count),
		"mean_vy": sum_vy / float(count),
		"sentinels": sentinel_count,
	}


func _run() -> void:
	if not _failures.is_empty():
		_finish()
		return

	await _test_routing_and_parity()
	if _expect_bins:
		await _test_clamping()
		await _test_disable_distance()
		await _test_velocity()
		await _test_vsm()
		await _test_rt_proxy()

	_finish()


func _test_routing_and_parity() -> void:
	# 1. Forward+ legacy reference (raster_mode = 1 draws programmable bins on legacy Forward+).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 1)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 8.0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	await _frames(SETTLE_FRAMES)
	var cap_legacy := await _capture_probe()

	# 2. HW VS path (debug_programmable_raster = false, raster_mode = 0, hw_raster_path = 1, sw = 0).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)
	await _frames(SETTLE_FRAMES)

	var routes := {}
	for inst_name: String in _instances:
		routes[inst_name] = _route_of(_instances[inst_name])
	_report["routes"] = routes

	for n in ["Background"]:
		var r: Array = routes[n]
		if r[0] != "virtual" or r[1] != "NONE":
			_failures.append("route/%s: got %s/%s expected virtual/NONE" % [n, r[0], r[1]])
	for n in ["WpoWave", "WpoMasked"]:
		var r: Array = routes[n]
		var exp_route := "virtual" if _expect_bins else "legacy"
		var exp_reason := "NONE" if _expect_bins else "UNSUPPORTED_CUSTOM_SHADER"
		if r[0] != exp_route or r[1] != exp_reason:
			_failures.append("route/%s: got %s/%s expected %s/%s" % [n, r[0], r[1], exp_route, exp_reason])
	for n in ["LegacyPosition", "LegacyModelView", "LegacyProjection"]:
		var r: Array = routes[n]
		if r[0] != "legacy" or r[1] != "UNSUPPORTED_CUSTOM_SHADER":
			_failures.append("route/%s: got %s/%s expected legacy/UNSUPPORTED_CUSTOM_SHADER" % [n, r[0], r[1]])

	var cap_hw := await _capture_probe()
	var stats_hw: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())

	# 3. SW path (T = 32) and VIS64 buffer parity.
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 32.0)
	await _frames(SETTLE_FRAMES)
	var cap_sw := await _capture_probe()
	var stats_sw: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())

	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", true)
	await _frames(SETTLE_FRAMES)
	var cap_buf := await _capture_probe()
	ProjectSettings.set_setting("rendering/nextgeo/debug_vis64_buffer", false)

	# 4. HW Mesh path (hw_raster_path = 2, T = 0) if supported.
	var cap_mesh := {}
	var stats_mesh := {}
	if _expect_mesh:
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 2)
		ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 0.0)
		await _frames(SETTLE_FRAMES)
		cap_mesh = await _capture_probe()
		stats_mesh = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)

	_report["parity_stats"] = {
		"hw": {
			"prog_hw": int(stats_hw.get("programmable_hw_clusters", 0)),
			"prog_sw": int(stats_hw.get("programmable_sw_clusters", 0)),
			"clamped": int(stats_hw.get("vertex_offset_clamped", 0)),
			"cut_match": bool(stats_hw.get("cut_match", false)),
		},
		"sw": {
			"prog_hw": int(stats_sw.get("programmable_hw_clusters", 0)),
			"prog_sw": int(stats_sw.get("programmable_sw_clusters", 0)),
			"clamped": int(stats_sw.get("vertex_offset_clamped", 0)),
			"cut_match": bool(stats_sw.get("cut_match", false)),
		},
		"mesh": {
			"path": String(stats_mesh.get("hw_raster_path", "")),
			"prog_hw": int(stats_mesh.get("programmable_hw_clusters", 0)),
			"clamped": int(stats_mesh.get("vertex_offset_clamped", 0)),
			"cut_match": bool(stats_mesh.get("cut_match", false)),
		},
	}

	if not _expect_bins:
		return

	if int(stats_hw.get("programmable_hw_clusters", 0)) <= 0 or int(stats_hw.get("programmable_sw_clusters", -1)) != 0:
		_failures.append("parity_hw: expected prog_hw > 0 and prog_sw == 0, got %s" % str(_report["parity_stats"]["hw"]))
	if int(stats_sw.get("programmable_sw_clusters", 0)) <= 0:
		_failures.append("parity_sw: expected prog_sw > 0, got %s" % str(_report["parity_stats"]["sw"]))
	if int(stats_hw.get("vertex_offset_clamped", -1)) != 0 or int(stats_sw.get("vertex_offset_clamped", -1)) != 0:
		_failures.append("parity: expected 0 clamped vertices within 0.5 m default bound, got hw=%s sw=%s" % [str(stats_hw.get("vertex_offset_clamped")), str(stats_sw.get("vertex_offset_clamped"))])
	if not bool(stats_hw.get("cut_match", false)) or not bool(stats_sw.get("cut_match", false)):
		_failures.append("parity: CPU_cut_hash mismatch with WPO bounds inflation (hw=%s sw=%s)" % [str(stats_hw.get("cut_match")), str(stats_sw.get("cut_match"))])

	var bg_rec := int(_records["Background"])
	var bg_ref := _pixels_of(cap_legacy, bg_rec)
	var parity_checks := [["hw", cap_hw], ["sw", cap_sw]]
	if _expect_mesh:
		if String(stats_mesh.get("hw_raster_path", "")) != "mesh" or int(stats_mesh.get("programmable_hw_clusters", 0)) <= 0:
			_failures.append("parity_mesh: expected hw_raster_path=='mesh' and prog_hw > 0, got %s" % str(_report["parity_stats"]["mesh"]))
		if int(stats_mesh.get("vertex_offset_clamped", -1)) != 0:
			_failures.append("parity_mesh: expected 0 clamped vertices, got %s" % str(stats_mesh.get("vertex_offset_clamped")))
		parity_checks.append(["mesh", cap_mesh])

	for pair in parity_checks:
		var label: String = pair[0]
		var cap: Dictionary = pair[1]
		var cmp := _compare_masks(bg_ref, _pixels_of(cap, bg_rec))
		_report["bg_vs_legacy_" + label] = cmp
		var limit := maxi(64, int(0.025 * float(cmp["ref"])))
		if int(cmp["one_sided"]) > limit:
			_failures.append("bg_vs_legacy_%s: %d pixels differ (limit %d of %d)" % [label, cmp["one_sided"], limit, cmp["ref"]])
		var wave_c := _record_centroid_and_count(cap, int(_records["WpoWave"]))
		var masked_c := _record_centroid_and_count(cap, int(_records["WpoMasked"]))
		if int(wave_c["count"]) < 100 or int(masked_c["count"]) < 60:
			_failures.append("parity_%s: WPO instances missing pixels (wave=%d masked=%d)" % [label, wave_c["count"], masked_c["count"]])

	# SW image vs VIS64 buffer must be bit-identical.
	var w_sw: PackedInt32Array = cap_sw["words"]
	var w_buf: PackedInt32Array = cap_buf["words"]
	var buf_diff := 0
	for i in w_sw.size() / ProbeScript.WORDS_PER_PIXEL:
		var b := i * ProbeScript.WORDS_PER_PIXEL
		if w_sw[b] != w_buf[b] or (w_sw[b + 1] & 0x7F) != (w_buf[b + 1] & 0x7F) or w_sw[b + 2] != w_buf[b + 2]:
			buf_diff += 1
	if buf_diff != 0:
		_failures.append("vis64_buffer: %d pixels differ from image mode" % buf_diff)


func _set_all_base_visible(p_vis: bool) -> void:
	for inst_name: String in _instances:
		(_instances[inst_name] as MeshInstance3D).visible = p_vis


func _test_clamping() -> void:
	_set_all_base_visible(false)

	var mat := _shader_mat(WPO_CLAMP_SHADER, 0.25)
	mat.set_shader_parameter("offset_x", 1.2)
	var inst := MeshInstance3D.new()
	inst.name = "ClampSphere"
	inst.mesh = _sphere_mesh
	inst.material_override = mat
	inst.position = Vector3(0.0, 0.0, 0.0)
	add_child(inst)

	# Forward+ legacy unclamped reference (+1.2 m in X).
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 1)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	await _frames(SETTLE_FRAMES)
	var img_legacy := get_viewport().get_texture().get_image()
	var c_legacy := _image_non_bg_centroid(img_legacy, _env.background_color)

	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	var rec_info: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(inst.get_instance())
	var rec := int(rec_info.get("record_index", -1))

	var modes := [
		{"name": "hw_vs", "path": 1, "sw": 0.0},
		{"name": "sw", "path": 1, "sw": 32.0},
	]
	if _expect_mesh:
		modes.append({"name": "hw_mesh", "path": 2, "sw": 0.0})

	var clamp_rep := {"legacy_unclamped_u": snappedf(float(c_legacy["u"]), 1e-4)}
	for m in modes:
		var mname: String = m["name"]
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(m["path"]))
		ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(m["sw"]))

		# Clamped at 0.25 m (< 1.2 m shader offset).
		mat.vertex_offset_max = 0.25
		await _frames(SETTLE_FRAMES)
		var cap_clamped := await _capture_probe()
		var st_clamped: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		var c_clamped := _record_centroid_and_count(cap_clamped, rec)
		var clamped_cnt := int(st_clamped.get("vertex_offset_clamped", 0))

		if clamped_cnt <= 0:
			_failures.append("clamp_%s: expected vertex_offset_clamped > 0 at max=0.25, got %d" % [mname, clamped_cnt])
		if int(c_clamped["count"]) < 100:
			_failures.append("clamp_%s: too few pixels (%d)" % [mname, c_clamped["count"]])
		elif float(c_clamped["u"]) <= 0.51 or float(c_legacy["u"]) - float(c_clamped["u"]) < 0.05:
			_failures.append("clamp_%s: clamped centroid u=%.4f not strictly between 0.51 and legacy %.4f" % [mname, float(c_clamped["u"]), float(c_legacy["u"])])

		# Unclamped at 1.5 m (> 1.2 m shader offset).
		mat.vertex_offset_max = 1.5
		await _frames(SETTLE_FRAMES)
		var cap_free := await _capture_probe()
		var st_free: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		var c_free := _record_centroid_and_count(cap_free, rec)
		var free_cnt := int(st_free.get("vertex_offset_clamped", -1))

		if free_cnt != 0:
			_failures.append("clamp_%s: expected vertex_offset_clamped == 0 at max=1.5, got %d" % [mname, free_cnt])
		if absf(float(c_free["u"]) - float(c_legacy["u"])) > 0.02:
			_failures.append("clamp_%s: unclamped centroid u=%.4f differs from legacy %.4f" % [mname, float(c_free["u"]), float(c_legacy["u"])])

		clamp_rep[mname] = {
			"clamped_vertices": clamped_cnt,
			"clamped_u": snappedf(float(c_clamped["u"]), 1e-4),
			"unclamped_vertices": free_cnt,
			"unclamped_u": snappedf(float(c_free["u"]), 1e-4),
		}

	_report["clamping"] = clamp_rep
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	inst.queue_free()
	await _frames(2)


func _test_disable_distance() -> void:
	_set_all_base_visible(false)

	var mat := _shader_mat(WPO_ANIM_ORBIT_SHADER, 1.2)
	mat.set_shader_parameter("base_x", 0.75)
	mat.set_shader_parameter("amp", 0.20)
	mat.set_shader_parameter("speed", 3.0)
	var inst := MeshInstance3D.new()
	inst.name = "DisableDistSphere"
	inst.mesh = _sphere_mesh
	inst.material_override = mat
	inst.position = Vector3(0.0, 0.0, 0.0)
	add_child(inst)

	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 24.0)
	await _frames(4)
	var rec := int(RenderingServer.virtual_geometry_get_debug_stats(inst.get_instance()).get("record_index", -1))

	# Camera at z=6.2, sphere radius 0.55 -> camera-to-AABB distance ~5.65 m.
	# 1. Disable distance = 12.0 (> 5.65 m): stays programmable, displaced right, non-zero velocity.
	inst.vertex_offset_disable_distance = 12.0
	await _frames(SETTLE_FRAMES)
	var st_near: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var cap_near := await _capture_probe()
	var c_near := _record_centroid_and_count(cap_near, rec)
	var v_near := _record_velocity_stats(cap_near, rec)

	if int(st_near.get("programmable_clusters", 0)) <= 0:
		_failures.append("disable_dist_near: expected programmable_clusters > 0, got %s" % str(st_near.get("programmable_clusters")))
	if int(st_near.get("vertex_offset_distance_fixed_clusters", -1)) != 0:
		_failures.append("disable_dist_near: expected vertex_offset_distance_fixed_clusters == 0, got %s" % str(st_near.get("vertex_offset_distance_fixed_clusters")))
	if float(c_near["u"]) < 0.54:
		_failures.append("disable_dist_near: expected displaced centroid u >= 0.54, got %.4f" % float(c_near["u"]))

	# 2. Disable distance = 2.0 (< 5.65 m): 100% FIXED, snaps back to center, velocity == 0.
	inst.vertex_offset_disable_distance = 2.0
	await _frames(SETTLE_FRAMES)
	var st_far: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var cap_far := await _capture_probe()
	var c_far := _record_centroid_and_count(cap_far, rec)
	var v_far := _record_velocity_stats(cap_far, rec)

	var far_prog := int(st_far.get("programmable_clusters", -1))
	var far_prog_sw := int(st_far.get("programmable_sw_clusters", -1))
	var far_prog_hw := int(st_far.get("programmable_hw_clusters", -1))
	var far_fixed := int(st_far.get("fixed_clusters", 0))
	var far_demoted := int(st_far.get("vertex_offset_distance_fixed_clusters", 0))

	if far_prog != 0 or far_prog_sw != 0 or far_prog_hw != 0:
		_failures.append("disable_dist_far: expected 100%% FIXED (0 programmable clusters), got prog=%d sw=%d hw=%d" % [far_prog, far_prog_sw, far_prog_hw])
	if far_fixed <= 0 or far_demoted != far_fixed:
		_failures.append("disable_dist_far: expected vertex_offset_distance_fixed_clusters == fixed_clusters > 0, got demoted=%d fixed=%d" % [far_demoted, far_fixed])
	if absf(float(c_far["u"]) - 0.50) > 0.02:
		_failures.append("disable_dist_far: expected undisplaced center u ~ 0.50, got %.4f" % float(c_far["u"]))
	if float(v_far["mean_mag"]) > 0.0005 or int(v_far["sentinels"]) != 0:
		_failures.append("disable_dist_far: expected zero velocity when demoted, got %s" % str(v_far))

	_report["disable_distance"] = {
		"near": {
			"prog": int(st_near.get("programmable_clusters", 0)),
			"demoted": int(st_near.get("vertex_offset_distance_fixed_clusters", 0)),
			"u": snappedf(float(c_near["u"]), 1e-4),
			"vel_mag": snappedf(float(v_near["mean_mag"]), 1e-5),
		},
		"far": {
			"prog": far_prog,
			"fixed": far_fixed,
			"demoted": far_demoted,
			"u": snappedf(float(c_far["u"]), 1e-4),
			"vel_mag": snappedf(float(v_far["mean_mag"]), 1e-5),
		},
	}

	inst.queue_free()
	await _frames(2)


func _measure_orbit_velocity_rate(p_rec: int, p_legacy: bool) -> Dictionary:
	# Wait one frame, timestamp, wait next frame while capturing probe, compute speed / dt_sec.
	await RenderingServer.frame_post_draw
	var t0 := Time.get_ticks_usec()
	_probe.set("capture", {})
	_probe.set("capture_armed", true)
	await RenderingServer.frame_post_draw
	var t1 := Time.get_ticks_usec()
	await RenderingServer.frame_post_draw
	var cap: Dictionary = _probe.get("capture")
	_probe.set("capture", {})
	var dt := maxf(float(t1 - t0) * 1e-6, 0.001)
	var vstats: Dictionary
	if p_legacy:
		var img := get_viewport().get_texture().get_image()
		vstats = _foreground_velocity_stats_from_image(cap, img, _env.background_color)
	else:
		vstats = _record_velocity_stats(cap, p_rec)
	var bg_v := _record_velocity_stats(cap, int(_records["Background"]))
	vstats["dt"] = dt
	vstats["rate"] = float(vstats["mean_mag"]) / dt
	vstats["bg_mag"] = float(bg_v["mean_mag"])
	vstats["bg_count"] = int(bg_v["count"])
	return vstats


func _test_velocity() -> void:
	_set_all_base_visible(false)
	(_instances["Background"] as MeshInstance3D).visible = true

	var mat := _shader_mat(WPO_ANIM_ORBIT_SHADER, 0.6)
	mat.set_shader_parameter("base_x", 0.0)
	mat.set_shader_parameter("amp", 0.35)
	mat.set_shader_parameter("speed", 3.0)
	var inst := MeshInstance3D.new()
	inst.name = "VelSphere"
	inst.mesh = _sphere_mesh
	inst.material_override = mat
	inst.position = Vector3(0.0, 0.0, 0.0)
	add_child(inst)

	# 1. Forward+ legacy reference (hide background briefly so foreground color isolate is clean).
	(_instances["Background"] as MeshInstance3D).visible = false
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 1)
	await _frames(SETTLE_FRAMES)
	var v_legacy := await _measure_orbit_velocity_rate(-1, true)
	(_instances["Background"] as MeshInstance3D).visible = true

	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", 8.0)
	await _frames(4)
	var rec := int(RenderingServer.virtual_geometry_get_debug_stats(inst.get_instance()).get("record_index", -1))

	var vel_rep := {
		"legacy": {
			"count": int(v_legacy["count"]),
			"mean_mag": snappedf(float(v_legacy["mean_mag"]), 1e-5),
			"rate": snappedf(float(v_legacy["rate"]), 1e-4),
		},
	}

	if int(v_legacy["count"]) < 100 or float(v_legacy["mean_mag"]) < 0.0002:
		_failures.append("velocity_legacy: invalid legacy reference %s" % str(v_legacy))

	# 2. Test both Tile Quads (shading_path = 2) and Compute Bins (shading_path = 1).
	for sp in [["tile_quads", 2], ["compute_bins", 1]]:
		var label: String = sp[0]
		ProjectSettings.set_setting("rendering/nextgeo/shading_path", int(sp[1]))
		await _frames(SETTLE_FRAMES)
		var v_ng := await _measure_orbit_velocity_rate(rec, false)
		vel_rep[label] = {
			"count": int(v_ng["count"]),
			"mean_mag": snappedf(float(v_ng["mean_mag"]), 1e-5),
			"rate": snappedf(float(v_ng["rate"]), 1e-4),
			"sentinels": int(v_ng["sentinels"]),
			"non_finite": int(v_ng["non_finite"]),
			"bg_mag": snappedf(float(v_ng["bg_mag"]), 1e-5),
		}
		if int(v_ng["count"]) < 100:
			_failures.append("velocity_%s: too few sphere pixels (%d)" % [label, v_ng["count"]])
		if int(v_ng["sentinels"]) != 0 or int(v_ng["non_finite"]) != 0:
			_failures.append("velocity_%s: invalid velocity pixels (sentinels=%d non_finite=%d)" % [label, v_ng["sentinels"], v_ng["non_finite"]])
		if float(v_ng["mean_mag"]) < 0.0003:
			_failures.append("velocity_%s: expected non-zero WPO velocity, got %.6f" % [label, float(v_ng["mean_mag"])])
		elif float(v_legacy["rate"]) > 0.001:
			var ratio := float(v_ng["rate"]) / float(v_legacy["rate"])
			if ratio < 0.45 or ratio > 2.2:
				_failures.append("velocity_%s: velocity rate %.4f deviates from legacy %.4f (ratio=%.2f)" % [label, float(v_ng["rate"]), float(v_legacy["rate"]), ratio])
		if float(v_ng["bg_mag"]) > 0.0005:
			_failures.append("velocity_%s: stationary background velocity corrupted (%.6f)" % [label, float(v_ng["bg_mag"])])

	ProjectSettings.set_setting("rendering/nextgeo/shading_path", 2)
	_report["velocity"] = vel_rep
	inst.queue_free()
	await _frames(2)


func _test_vsm() -> void:
	_set_all_base_visible(false)
	_sun.visible = false
	_camera.current = false

	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1) # Virtual Shadow Maps
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", false)

	var sv := SubViewport.new()
	sv.size = Vector2i(VSM_SIZE, VSM_SIZE)
	sv.use_hdr_2d = true
	sv.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sv.msaa_3d = Viewport.MSAA_DISABLED
	sv.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sv.use_taa = false
	add_child(sv)

	var vcam := Camera3D.new()
	vcam.projection = Camera3D.PROJECTION_ORTHOGONAL
	vcam.size = VSM_EXTENT
	vcam.near = 0.05
	vcam.far = 20.0
	vcam.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 5.0, 0.0))
	sv.add_child(vcam)
	vcam.current = true

	var white := StandardMaterial3D.new()
	white.albedo_color = Color(1.0, 1.0, 1.0)
	white.roughness = 1.0
	white.metallic = 0.0
	white.metallic_specular = 0.0

	var floor_inst := MeshInstance3D.new()
	var fp := PlaneMesh.new()
	fp.size = Vector2(12.0, 12.0)
	floor_inst.mesh = fp
	floor_inst.material_override = white
	floor_inst.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(floor_inst)

	var caster_plane := PlaneMesh.new()
	caster_plane.size = Vector2(0.8, 0.8)
	caster_plane.subdivide_width = 40
	caster_plane.subdivide_depth = 40
	var caster_vmesh := _virtualize(caster_plane, "vsm_wpo_caster")

	var wpo_mat := _shader_mat(VSM_WPO_SHADER, 1.2)
	var caster := MeshInstance3D.new()
	caster.mesh = caster_vmesh
	caster.material_override = wpo_mat
	caster.position = Vector3(-1.0, 1.0, 0.0)
	add_child(caster)

	# Reference unshadowed floor.
	var sun_ref := _make_vsm_sun()
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	await _frames(VSM_SETTLE_FRAMES)
	var ref_lum := _capture_subviewport_lum(sv)
	sun_ref.queue_free()
	await _frames(2)

	var vsm_modes := [
		{"name": "vsm_wpo_hw_vs", "path": 1, "sw_max": 0.0, "expect": "hw"},
		{"name": "vsm_wpo_sw", "path": 1, "sw_max": 24.0, "expect": "sw"},
	]
	if _expect_mesh:
		vsm_modes.append({"name": "vsm_wpo_hw_mesh", "path": 2, "sw_max": 0.0, "expect": "hw"})

	var vsm_out := {}
	caster.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON

	for m in vsm_modes:
		var mname: String = m["name"]
		ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(m["path"]))
		ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(m["sw_max"]))

		# Keep the SAME DirectionalLight3D alive across shifts to verify #212 dynamic VSM cache invalidation!
		var sun := _make_vsm_sun()

		# Phase A: shift_x = 0.0 -> shadow centered near x = 0.0.
		caster.vertex_offset_disable_distance = 0.0
		wpo_mat.set_shader_parameter("shift_x", 0.0)
		var peak_a := await _settle_vsm_peak(VSM_SETTLE_FRAMES)
		var lum_a := _capture_subviewport_lum(sv)
		var sh_a := _vsm_shadow_centroid_x(ref_lum, lum_a)

		# Phase B: shift_x = +0.85 (node transform unchanged!) -> cached VSM pages must dynamically invalidate and move!
		wpo_mat.set_shader_parameter("shift_x", 0.85)
		var peak_b := await _settle_vsm_peak(VSM_SETTLE_FRAMES)
		var lum_b := _capture_subviewport_lum(sv)
		var sh_b := _vsm_shadow_centroid_x(ref_lum, lum_b)

		# Phase C: vertex_offset_disable_distance = 2.0 (< 4.1 m camera-to-caster) -> demotes to FIXED, shadow snaps back!
		caster.vertex_offset_disable_distance = 2.0
		var peak_c := await _settle_vsm_peak(VSM_SETTLE_FRAMES)
		var lum_c := _capture_subviewport_lum(sv)
		var sh_c := _vsm_shadow_centroid_x(ref_lum, lum_c)

		sun.queue_free()
		await _frames(2)

		var expect_kind: String = m["expect"]
		for pair in [["a", peak_a], ["b", peak_b]]:
			var pl: String = pair[0]
			var pk: Dictionary = pair[1]
			if not bool(pk["vsm_active"]) or not bool(pk["nanite_active"]) or not bool(pk["gpu_counters_valid"]):
				_failures.append("%s_%s: VSM inactive or counters invalid (%s)" % [mname, pl, str(pk)])
			if int(pk["overflow"]) != 0 or int(pk["sink"]) != 0:
				_failures.append("%s_%s: overflow=%d sink=%d" % [mname, pl, int(pk["overflow"]), int(pk["sink"])])
			if int(pk["dynamic_clusters"]) <= 0:
				_failures.append("%s_%s: expected dynamic_clusters > 0 for active WPO, got %d" % [mname, pl, int(pk["dynamic_clusters"])])
			if expect_kind == "hw" and int(pk["prog_hw_clusters"]) <= 0:
				_failures.append("%s_%s: expected prog_hw_clusters > 0, got %s" % [mname, pl, str(pk)])
			if expect_kind == "sw" and int(pk["prog_sw_clusters"]) <= 0:
				_failures.append("%s_%s: expected prog_sw_clusters > 0, got %s" % [mname, pl, str(pk)])

		if int(sh_a["dark"]) < 200 or int(sh_b["dark"]) < 200 or int(sh_c["dark"]) < 200:
			_failures.append("%s: insufficient shadow pixels (a=%d b=%d c=%d)" % [mname, sh_a["dark"], sh_b["dark"], sh_c["dark"]])
		if float(sh_b["cx"]) - float(sh_a["cx"]) < 0.60:
			_failures.append("%s: VSM shadow failed to track WPO shift (cx_a=%.3f -> cx_b=%.3f)" % [mname, float(sh_a["cx"]), float(sh_b["cx"])])
		if absf(float(sh_c["cx"]) - float(sh_a["cx"])) > 0.15:
			_failures.append("%s: VSM disable_distance did not revert shadow (cx_a=%.3f cx_c=%.3f)" % [mname, float(sh_a["cx"]), float(sh_c["cx"])])
		if int(peak_c["prog_hw_clusters"]) != 0 or int(peak_c["prog_sw_clusters"]) != 0 or int(peak_c["vertex_offset_distance_fixed_clusters"]) <= 0:
			_failures.append("%s_c: expected VSM WPO demotion (prog=0, wpo_fixed>0), got %s" % [mname, str(peak_c)])

		vsm_out[mname] = {
			"cx_a": snappedf(float(sh_a["cx"]), 1e-3),
			"cx_b": snappedf(float(sh_b["cx"]), 1e-3),
			"cx_c": snappedf(float(sh_c["cx"]), 1e-3),
			"dyn_b": int(peak_b["dynamic_clusters"]),
			"prog_hw_b": int(peak_b["prog_hw_clusters"]),
			"prog_sw_b": int(peak_b["prog_sw_clusters"]),
			"wpo_demoted_c": int(peak_c["vertex_offset_distance_fixed_clusters"]),
		}

	_report["vsm"] = vsm_out
	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", 1)
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 0)
	caster.queue_free()
	floor_inst.queue_free()
	sv.queue_free()
	_camera.current = true
	_sun.visible = true
	await _frames(2)


func _make_vsm_sun() -> DirectionalLight3D:
	var sun := DirectionalLight3D.new()
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	sun.directional_shadow_max_distance = 12.0
	sun.shadow_enabled = true
	sun.shadow_method = Light3D.SHADOW_METHOD_VIRTUAL
	sun.transform = Transform3D(Basis.looking_at(Vector3(1.0, -1.0, 0.0).normalized(), Vector3.UP), Vector3(0.0, 5.0, 0.0))
	add_child(sun)
	return sun


func _settle_vsm_peak(p_frames: int) -> Dictionary:
	var peak := {
		"vsm_active": false,
		"nanite_active": false,
		"gpu_counters_valid": false,
		"sw_clusters": 0,
		"hw_clusters": 0,
		"prog_sw_clusters": 0,
		"prog_hw_clusters": 0,
		"dynamic_clusters": 0,
		"vertex_offset_distance_fixed_clusters": 0,
		"overflow": 0,
		"sink": 0,
	}
	for i in p_frames:
		await RenderingServer.frame_post_draw
		# Sample the second half of settle frames so stale pre-change frames don't pollute peak.
		if i < p_frames / 2:
			continue
		var info := RenderingServer.virtual_shadow_maps_get_debug_info()
		if bool(info.get("active", false)):
			peak["vsm_active"] = true
		var n: Dictionary = info.get("nanite", {})
		if bool(n.get("active", false)):
			peak["nanite_active"] = true
		if bool(n.get("gpu_counters_valid", false)):
			peak["gpu_counters_valid"] = true
			for k in ["sw_clusters", "hw_clusters", "prog_sw_clusters", "prog_hw_clusters", "dynamic_clusters", "vertex_offset_distance_fixed_clusters", "overflow", "sink"]:
				peak[k] = maxi(int(peak[k]), int(n.get(k, 0)))
	return peak


func _capture_subviewport_lum(p_sv: SubViewport) -> PackedFloat32Array:
	var img := p_sv.get_texture().get_image()
	img.convert(Image.FORMAT_RF)
	return img.get_data().to_float32_array()


func _vsm_shadow_centroid_x(p_ref: PackedFloat32Array, p_lit: PackedFloat32Array) -> Dictionary:
	var dark := 0
	var sum_x := 0.0
	for py in VSM_SIZE:
		var row := py * VSM_SIZE
		for px in VSM_SIZE:
			var r := p_ref[row + px]
			if r < 0.02:
				continue
			if p_lit[row + px] / r < 0.5:
				var wx := ((float(px) + 0.5) / float(VSM_SIZE) - 0.5) * VSM_EXTENT
				sum_x += wx
				dark += 1
	return {"dark": dark, "cx": sum_x / float(maxi(dark, 1))}


func _specular_hit_centroid(p_img: Image) -> Dictionary:
	# DEBUG_DRAW_NEXTGI_SPECULAR_HIT outputs 1.0 (white) on miss and log2(1 + dist)/8 (~0.34) on hit.
	var w := p_img.get_width()
	var h := p_img.get_height()
	var count := 0
	var sx := 0.0
	var sy := 0.0
	var min_r := 10.0
	var max_r := -1.0
	for y in h:
		for x in w:
			var c := p_img.get_pixel(x, y)
			min_r = minf(min_r, c.r)
			max_r = maxf(max_r, c.r)
			if c.r < 0.85:
				sx += (float(x) + 0.5) / float(w)
				sy += (float(y) + 0.5) / float(h)
				count += 1
	var mid := p_img.get_pixel(w / 2, h / 2)
	if count == 0:
		return {"count": 0, "u": -1.0, "v": -1.0, "min_r": snappedf(min_r, 1e-3), "max_r": snappedf(max_r, 1e-3), "mid": [snappedf(mid.r, 1e-3), snappedf(mid.g, 1e-3), snappedf(mid.b, 1e-3)]}
	return {"count": count, "u": sx / float(count), "v": sy / float(count), "min_r": snappedf(min_r, 1e-3), "max_r": snappedf(max_r, 1e-3)}


func _test_rt_proxy() -> void:
	var nextgi_project_enabled := bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false))
	if not nextgi_project_enabled:
		_report["rt_proxy"] = {"nextgi_enabled": false}
		if _require_rt:
			_failures.append("rt_proxy: --require-rt set but rendering/global_illumination/nextgi/enabled is false")
		return

	_set_all_base_visible(false)
	_env.nextgi_enabled = true

	var mirror_mat := StandardMaterial3D.new()
	mirror_mat.albedo_color = Color(1.0, 1.0, 1.0)
	mirror_mat.metallic = 1.0
	mirror_mat.roughness = 0.0
	var mirror_mesh := PlaneMesh.new()
	mirror_mesh.size = Vector2(12.0, 12.0)
	var mirror := MeshInstance3D.new()
	mirror.name = "RtMirror"
	mirror.mesh = mirror_mesh
	mirror.material_override = mirror_mat
	mirror.transform = Transform3D(Basis(Vector3.RIGHT, deg_to_rad(90.0)), Vector3(0.0, 0.0, -0.45))
	add_child(mirror)

	var rt_mat := _shader_mat(RT_WPO_SHADER, 0.8)
	rt_mat.set_shader_parameter("shift_x", 0.70)
	var wpo_inst := MeshInstance3D.new()
	wpo_inst.name = "RtWpoSphere"
	wpo_inst.mesh = _sphere_mesh
	wpo_inst.material_override = rt_mat
	wpo_inst.transform = Transform3D(Basis.from_scale(Vector3.ONE * 0.6), Vector3(0.0, 0.0, 0.50))
	wpo_inst.rt_evaluate_vertex_offset = false
	add_child(wpo_inst)

	# 1. Ground-truth displaced raster silhouette seen from the virtual mirrored camera at (0, 0, -0.95)
	# looking toward +Z at (0, 0, +0.50). Flipping U (1.0 - u) matches the reflection camera at (0, 0, +0.05)
	# reflected across the mirror plane at z = -0.45 (sphere is ~0.95 m from mirror, well inside the 1.5 m WRC handoff).
	mirror.visible = false
	_camera.fov = 50.0
	_camera.position = Vector3(0.0, 0.0, -0.95)
	_camera.look_at(Vector3(0.0, 0.0, 0.50))
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	await _frames(SETTLE_FRAMES)
	var wpo_rec := int(RenderingServer.virtual_geometry_get_debug_stats(wpo_inst.get_instance()).get("record_index", -1))
	var cap_raster := await _capture_probe()
	var c_raster := _record_centroid_and_count(cap_raster, wpo_rec)
	var expected_mirror_u := 1.0 - float(c_raster["u"])

	# 2. Switch to mirror view (WPO sphere is behind camera at z in [+0.17, +0.83] -> 100% off-screen -> pure HWRT TLAS hit!).
	mirror.visible = true
	_camera.position = Vector3(0.0, 0.0, 0.05)
	_camera.look_at(Vector3(0.0, 0.0, -0.45))
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_HIT

	# 2a. rt_evaluate_vertex_offset = false -> static undisplaced proxy centered near u = 0.50.
	wpo_inst.rt_evaluate_vertex_offset = false
	wpo_inst.vertex_offset_disable_distance = 0.0
	await _frames(RT_SETTLE_FRAMES)
	var info_off: Dictionary = RenderingServer.nextgi_get_tier_info()
	var hit_off := _specular_hit_centroid(get_viewport().get_texture().get_image())

	var tier := String(info_off.get("tier", ""))
	if tier != "HWRT":
		_failures.append("rt_proxy: expected NextGI tier HWRT, got '%s'" % tier)
	else:
		if int(info_off.get("rt_vertex_offset_instances", -1)) != 0:
			_failures.append("rt_proxy_off: expected rt_vertex_offset_instances == 0, got %s" % str(info_off.get("rt_vertex_offset_instances")))
		if int(hit_off["count"]) < 50 or absf(float(hit_off["u"]) - 0.50) > 0.08:
			_failures.append("rt_proxy_off: expected undisplaced hit near u=0.50, got %s" % str(hit_off))

		# 2b. rt_evaluate_vertex_offset = true -> compute evaluates vertex() + refits BLAS -> hit shifts to match raster!
		wpo_inst.rt_evaluate_vertex_offset = true
		await _frames(RT_SETTLE_FRAMES)
		var info_on: Dictionary = RenderingServer.nextgi_get_tier_info()
		var hit_on := _specular_hit_centroid(get_viewport().get_texture().get_image())

		if int(info_on.get("rt_vertex_offset_instances", 0)) < 1:
			_failures.append("rt_proxy_on: expected rt_vertex_offset_instances >= 1, got %s" % str(info_on.get("rt_vertex_offset_instances")))
		if int(info_on.get("rt_vertex_offset_refit_triangles", 0)) <= 0:
			_failures.append("rt_proxy_on: expected rt_vertex_offset_refit_triangles > 0 after settle, got %s" % str(info_on.get("rt_vertex_offset_refit_triangles")))
		if int(hit_on["count"]) < 50:
			_failures.append("rt_proxy_on: expected RT hits on displaced proxy, got %d" % hit_on["count"])
		else:
			if float(hit_on["u"]) - float(hit_off["u"]) < 0.06:
				_failures.append("rt_proxy_on: RT hit did not shift with WPO (off=%.4f on=%.4f)" % [float(hit_off["u"]), float(hit_on["u"])])
			if absf(float(hit_on["u"]) - expected_mirror_u) > 0.06:
				_failures.append("rt_proxy_on: RT hit u=%.4f does not match displaced raster u=%.4f" % [float(hit_on["u"]), expected_mirror_u])

		# 2c. vertex_offset_disable_distance = 0.05 (< ~0.12 m camera-to-AABB) disables RT WPO evaluation too!
		wpo_inst.vertex_offset_disable_distance = 0.05
		await _frames(RT_SETTLE_FRAMES)
		var info_dis: Dictionary = RenderingServer.nextgi_get_tier_info()
		var hit_dis := _specular_hit_centroid(get_viewport().get_texture().get_image())
		if int(info_dis.get("rt_vertex_offset_instances", -1)) != 0:
			_failures.append("rt_proxy_disabled: expected rt_vertex_offset_instances == 0 past disable distance, got %s" % str(info_dis.get("rt_vertex_offset_instances")))
		if int(hit_dis["count"]) < 50 or absf(float(hit_dis["u"]) - float(hit_off["u"])) > 0.06:
			_failures.append("rt_proxy_disabled: expected RT hit to revert to undisplaced u=%.4f, got %s" % [float(hit_off["u"]), str(hit_dis)])

		_report["rt_proxy"] = {
			"tier": tier,
			"expected_mirror_u": snappedf(expected_mirror_u, 1e-4),
			"off": {
				"u": snappedf(float(hit_off["u"]), 1e-4),
				"count": int(hit_off["count"]),
				"min_r": hit_off.get("min_r", -1.0),
				"max_r": hit_off.get("max_r", -1.0),
				"mid": hit_off.get("mid", []),
				"instances": int(info_off.get("rt_vertex_offset_instances", 0)),
				"tlas_instances": int(info_off.get("rt_instance_count", 0)),
				"refl_rays": int(info_off.get("reflection_rays", 0)),
				"refl_hits": int(info_off.get("reflection_hits", 0)),
				"refl_misses": int(info_off.get("reflection_misses", 0)),
			},
			"on": {
				"u": snappedf(float(hit_on["u"]), 1e-4),
				"count": int(hit_on["count"]),
				"min_r": hit_on.get("min_r", -1.0),
				"instances": int(info_on.get("rt_vertex_offset_instances", 0)),
				"refit_tris": int(info_on.get("rt_vertex_offset_refit_triangles", 0)),
				"refl_hits": int(info_on.get("reflection_hits", 0)),
			},
			"disabled": {"u": snappedf(float(hit_dis["u"]), 1e-4), "count": int(hit_dis["count"]), "instances": int(info_dis.get("rt_vertex_offset_instances", 0))},
		}

	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	_env.nextgi_enabled = false
	mirror.queue_free()
	wpo_inst.queue_free()
	await _frames(2)


func _cleanup() -> void:
	for p in _temp_files:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(p))
	_temp_files.clear()


func _finish() -> void:
	_cleanup()
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
