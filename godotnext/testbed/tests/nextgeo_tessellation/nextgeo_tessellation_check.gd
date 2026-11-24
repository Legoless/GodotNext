extends Node3D
## NextGeo runtime displacement tessellation check (#163, plan §21.4 contract N11, GN-6, GN-12, N5).
##
## Verifies on GPU (Vulkan + VVL SyncVal, run with GODOT_DEV_VSM=1):
## (a) experimental/tessellation == false: byte-identical frame vs undisplaced materials,
##     tessellation_buffers_allocated == false, tessellation_dicing_dispatches == 0, dicing_overflow == 0.
## (b) experimental/tessellation == true on coarse displaced planes/cliffs using BOTH StandardMaterial3D
##     (HEIGHTMAP_MODE_DISPLACEMENT) AND custom displacement() ShaderMaterial:
##     - Visibly displaces geometry matching a high-res CPU-displaced reference mesh.
##     - 0 crack pixels across interior shared edges and locked DAG cluster borders (both
##       DEBUG_DRAW_NEXTGEO_CRACKS exact (255, 0, 255) check on closed mesh AND zero background bleed
##       through the interior of a multi-cluster displaced cliff).
##     - Both SW dicing raster and bounded HW patch stream paths exercise cleanly.
## (c) VSM shadow variant: with directional VSM active (GODOT_DEV_VSM=1), shadows follow the displaced
##     silhouette when tessellation_directional == true and revert to base cluster silhouette when false.
## (d) Tight patch cap (debug_max_patches = 8): increments dicing_overflow cleanly with 0 VVL errors.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const SETTLE_FRAMES := 16
const DISP_MAGNITUDE := 0.85
const DISP_CENTER := 0.0

const CUSTOM_DISP_SHADER := """
shader_type spatial;
uniform sampler2D height_tex;
void displacement() {
	DISPLACEMENT = texture(height_tex, UV).r;
}
void fragment() {
	ALBEDO = vec3(0.85, 0.45, 0.2);
	ROUGHNESS = 0.65;
}
"""

const PLAIN_SHADER := """
shader_type spatial;
uniform sampler2D height_tex;
void fragment() {
	ALBEDO = vec3(0.85, 0.45, 0.2);
	ROUGHNESS = 0.65;
}
"""

var _phases: Array[Dictionary] = [
	{ "name": "plain_off", "use_disp": false, "use_cpu_ref": false, "tess": false, "vsm_dir": true, "cap": 0, "sw_t": 8.0, "debug": "none" },
	{ "name": "disp_off", "use_disp": true, "use_cpu_ref": false, "tess": false, "vsm_dir": true, "cap": 0, "sw_t": 8.0, "debug": "none" },
	{ "name": "cpu_ref", "use_disp": false, "use_cpu_ref": true, "tess": false, "vsm_dir": true, "cap": 0, "sw_t": 8.0, "debug": "none" },
	{ "name": "tess_on_vsm_on", "use_disp": true, "use_cpu_ref": false, "tess": true, "vsm_dir": true, "cap": 0, "sw_t": 8.0, "debug": "none" },
	{ "name": "tess_on_vsm_off", "use_disp": true, "use_cpu_ref": false, "tess": true, "vsm_dir": false, "cap": 0, "sw_t": 8.0, "debug": "none" },
	{ "name": "tess_on_hw", "use_disp": true, "use_cpu_ref": false, "tess": true, "vsm_dir": true, "cap": 0, "sw_t": 0.0, "debug": "none" },
	{ "name": "tess_cracks", "use_disp": true, "use_cpu_ref": false, "tess": true, "vsm_dir": true, "cap": 0, "sw_t": 8.0, "debug": "cracks" },
	{ "name": "tess_cliff_coverage", "use_disp": true, "use_cpu_ref": false, "tess": true, "vsm_dir": true, "cap": 0, "sw_t": 8.0, "debug": "fallback" },
	{ "name": "tess_overflow", "use_disp": true, "use_cpu_ref": false, "tess": true, "vsm_dir": true, "cap": 8, "sw_t": 8.0, "debug": "none" },
]

var _height_img: Image
var _height_tex: ImageTexture
var _sphere_height_tex: ImageTexture
var _plain_shader_mat: ShaderMaterial
var _disp_shader_mat: ShaderMaterial
var _plain_std_mat: StandardMaterial3D
var _disp_std_mat: StandardMaterial3D
var _disp_sphere_mat: StandardMaterial3D
var _receiver_mat: StandardMaterial3D

var _mesh_coarse_plane: Mesh
var _mesh_cpu_ref_plane: Mesh
var _mesh_sphere: Mesh
var _mesh_receiver: Mesh

var _inst_std_cliff: MeshInstance3D
var _inst_shader_cliff: MeshInstance3D
var _inst_sphere: MeshInstance3D
var _inst_receiver: MeshInstance3D
var _camera: Camera3D

var _phase_idx := 0
var _phase_frame := 0
var _images: Dictionary = {}
var _stats: Dictionary = {}
var _vsm_stats: Dictionary = {}
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_tess := false


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/tessellation/dicing_rate", 2.0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_local", false)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_expect_tess = indirect_count and (image_int64 or buffer_int64)
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_tessellation": _expect_tess,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_apply_phase()


func _sample_height_analytic(p_u: float, p_v: float) -> float:
	# Smooth deterministic ridge dome centered at UV (0.5, 0.5) so CPU ref and texture match tightly.
	# Fade ripple smoothly to 0 at v=0 and v=1 so polar-UV SphereMesh poles stay single-valued (no UV-seam tear).
	var dx := (p_u - 0.5) * 2.0
	var dy := (p_v - 0.5) * 2.0
	var r2 := dx * dx + dy * dy
	var dome := clampf(1.0 - r2 * 1.15, 0.0, 1.0)
	var pole_fade := sin(clampf(p_v, 0.0, 1.0) * PI)
	var ripple := 0.12 * pole_fade * (0.5 + 0.5 * sin(p_u * TAU * 2.0) * cos(p_v * TAU * 2.0))
	return clampf(dome * 0.88 + ripple, 0.0, 1.0)


func _make_height_texture() -> ImageTexture:
	_height_img = Image.create(128, 128, false, Image.FORMAT_RGBA8)
	for y in 128:
		for x in 128:
			var u := (float(x) + 0.5) / 128.0
			var v := (float(y) + 0.5) / 128.0
			var h := _sample_height_analytic(u, v)
			_height_img.set_pixel(x, y, Color(h, h, h, 1.0))
	_height_img.generate_mipmaps()
	return ImageTexture.create_from_image(_height_img)


func _make_sphere_height_texture() -> ImageTexture:
	# Strictly front-hemisphere dome + ripple centered at UV (0.5, 0.5), identically 0.0 on the
	# silhouette, poles, and back hemisphere so DEBUG_DRAW_NEXTGEO_CRACKS tests purely for mesh cracks
	# across interior shared edges and locked DAG cluster borders without back-hemisphere protrusion.
	var img := Image.create(128, 128, false, Image.FORMAT_RGBA8)
	for y in 128:
		for x in 128:
			var u := (float(x) + 0.5) / 128.0
			var v := (float(y) + 0.5) / 128.0
			var dx := (u - 0.5) * 2.0
			var dy := (v - 0.5) * 2.0
			var r2 := dx * dx + dy * dy
			var env_w := clampf(1.0 - r2 * 2.4, 0.0, 1.0)
			var smooth_w := env_w * env_w * (3.0 - 2.0 * env_w)
			var detail := 0.72 + 0.28 * sin(u * TAU * 4.0) * cos(v * TAU * 4.0)
			var h := clampf(smooth_w * detail, 0.0, 1.0)
			img.set_pixel(x, y, Color(h, h, h, 1.0))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _base_grid_point_and_normal(p_u: float, p_v: float, p_w: float, p_h: float) -> Array:
	# Gentle smooth relief so Nanite's DAG preserves multiple clusters and locked DAG group borders.
	var x: float = (p_u - 0.5) * p_w
	var z: float = (p_v - 0.5) * p_h
	var amp: float = 0.08
	var kx: float = TAU * 1.5
	var kz: float = TAU * 1.5
	var y: float = amp * sin(p_u * kx) * cos(p_v * kz)
	var dydx: float = (amp * kx / p_w) * cos(p_u * kx) * cos(p_v * kz)
	var dydz: float = (-amp * kz / p_h) * sin(p_u * kx) * sin(p_v * kz)
	var nrm := Vector3(-dydx, 1.0, -dydz).normalized()
	return [Vector3(x, y, z), nrm]


func _build_grid_mesh(p_subdiv: int, p_displace_cpu: bool, p_ngm_path: String) -> Mesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var w := 2.4
	var h := 2.4
	for iy in p_subdiv:
		for ix in p_subdiv:
			var u0 := float(ix) / float(p_subdiv)
			var u1 := float(ix + 1) / float(p_subdiv)
			var v0 := float(iy) / float(p_subdiv)
			var v1 := float(iy + 1) / float(p_subdiv)
			var uvs: Array[Vector2] = [Vector2(u0, v0), Vector2(u1, v0), Vector2(u1, v1), Vector2(u0, v1)]
			var pts: Array[Vector3] = []
			var nrms: Array[Vector3] = []
			for uv in uvs:
				var pn := _base_grid_point_and_normal(uv.x, uv.y, w, h)
				var pt: Vector3 = pn[0]
				var nrm: Vector3 = pn[1]
				if p_displace_cpu:
					var hv: float = _sample_height_analytic(uv.x, uv.y)
					pt += nrm * ((hv - DISP_CENTER) * DISP_MAGNITUDE)
				pts.append(pt)
				nrms.append(nrm)
			var tri_idx: Array[int] = [0, 1, 2, 0, 2, 3]
			for idx in tri_idx:
				st.set_normal(nrms[idx])
				st.set_uv(uvs[idx])
				st.add_vertex(pts[idx])
	var arr_mesh := st.commit()
	var importer := ImporterMesh.from_mesh(arr_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


func _virtualize(p_mesh: PrimitiveMesh, p_ngm_path: String) -> Mesh:
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


func _build_scene() -> void:
	_height_tex = _make_height_texture()
	_sphere_height_tex = _make_sphere_height_texture()

	# Coarse grid with enough triangles to span multiple NextGeo clusters (> 128 tris -> 24x24 = 1152 tris)
	# while keeping edges long enough to trigger high runtime dicing factors across interior & cluster borders.
	_mesh_coarse_plane = _build_grid_mesh(24, false, "user://nextgeo_tess_coarse.ngm")
	_mesh_cpu_ref_plane = _build_grid_mesh(96, true, "user://nextgeo_tess_cpu_ref.ngm")

	var sphere := SphereMesh.new()
	sphere.radius = 0.72
	sphere.height = 1.44
	sphere.radial_segments = 48
	sphere.rings = 24
	_mesh_sphere = _virtualize(sphere, "user://nextgeo_tess_sphere.ngm")

	var receiver_plane := PlaneMesh.new()
	receiver_plane.size = Vector2(14.0, 10.0)
	receiver_plane.subdivide_width = 4
	receiver_plane.subdivide_depth = 4
	_mesh_receiver = receiver_plane

	var sh_plain := Shader.new()
	sh_plain.code = PLAIN_SHADER
	_plain_shader_mat = ShaderMaterial.new()
	_plain_shader_mat.shader = sh_plain
	_plain_shader_mat.set_shader_parameter("height_tex", _height_tex)

	var sh_disp := Shader.new()
	sh_disp.code = CUSTOM_DISP_SHADER
	_disp_shader_mat = ShaderMaterial.new()
	_disp_shader_mat.shader = sh_disp
	_disp_shader_mat.set_shader_parameter("height_tex", _height_tex)
	_disp_shader_mat.displacement_magnitude = DISP_MAGNITUDE
	_disp_shader_mat.displacement_center = DISP_CENTER

	_plain_std_mat = StandardMaterial3D.new()
	_plain_std_mat.albedo_color = Color(0.28, 0.72, 0.42)
	_plain_std_mat.roughness = 0.65

	_disp_std_mat = StandardMaterial3D.new()
	_disp_std_mat.albedo_color = Color(0.28, 0.72, 0.42)
	_disp_std_mat.roughness = 0.65
	_disp_std_mat.heightmap_enabled = true
	_disp_std_mat.heightmap_mode = BaseMaterial3D.HEIGHTMAP_MODE_DISPLACEMENT
	_disp_std_mat.heightmap_texture = _height_tex
	_disp_std_mat.displacement_magnitude = DISP_MAGNITUDE
	_disp_std_mat.displacement_center = DISP_CENTER

	_disp_sphere_mat = StandardMaterial3D.new()
	_disp_sphere_mat.albedo_color = Color(0.28, 0.72, 0.42)
	_disp_sphere_mat.roughness = 0.65
	_disp_sphere_mat.heightmap_enabled = true
	_disp_sphere_mat.heightmap_mode = BaseMaterial3D.HEIGHTMAP_MODE_DISPLACEMENT
	_disp_sphere_mat.heightmap_texture = _sphere_height_tex
	_disp_sphere_mat.displacement_magnitude = 0.22
	_disp_sphere_mat.displacement_center = 0.0

	_receiver_mat = StandardMaterial3D.new()
	_receiver_mat.albedo_color = Color(0.76, 0.82, 0.96)
	_receiver_mat.roughness = 0.9

	# Side-angle camera looking slightly downward across the displaced cliffs & shadow receiver behind them.
	_camera = Camera3D.new()
	_camera.fov = 50.0
	_camera.near = 0.05
	_camera.far = 100.0
	_camera.position = Vector3(0.0, 2.2, 5.2)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 0.35, -0.4))

	# Left cliff: StandardMaterial3D HEIGHTMAP_MODE_DISPLACEMENT.
	_inst_std_cliff = MeshInstance3D.new()
	_inst_std_cliff.name = "StdCliff"
	_inst_std_cliff.mesh = _mesh_coarse_plane
	_inst_std_cliff.position = Vector3(-1.45, 0.0, 0.2)
	add_child(_inst_std_cliff)

	# Right cliff: Custom displacement() ShaderMaterial.
	_inst_shader_cliff = MeshInstance3D.new()
	_inst_shader_cliff.name = "ShaderCliff"
	_inst_shader_cliff.mesh = _mesh_coarse_plane
	_inst_shader_cliff.position = Vector3(1.45, 0.0, 0.2)
	add_child(_inst_shader_cliff)

	# Closed watertight displaced sphere in front-center for DEBUG_DRAW_NEXTGEO_CRACKS verification.
	# Rotated 180 deg around Y so the dome peak at UV (0.5, 0.5) faces the camera rather than protruding out the back hemisphere.
	_inst_sphere = MeshInstance3D.new()
	_inst_sphere.name = "DispSphere"
	_inst_sphere.mesh = _mesh_sphere
	_inst_sphere.position = Vector3(0.0, 0.55, 1.8)
	_inst_sphere.rotation_degrees = Vector3(0.0, 180.0, 0.0)
	add_child(_inst_sphere)

	# Shadow receiver plane tilted slightly behind the cliffs to catch the directional VSM silhouette shadow.
	_inst_receiver = MeshInstance3D.new()
	_inst_receiver.name = "ShadowReceiver"
	_inst_receiver.mesh = _mesh_receiver
	_inst_receiver.material_override = _receiver_mat
	_inst_receiver.position = Vector3(0.0, 0.0, -2.5)
	_inst_receiver.rotation_degrees = Vector3(72.0, 0.0, 0.0)
	add_child(_inst_receiver)

	# Low-angle sun shining from front-top toward the receiver wall behind the cliffs so height displacement
	# casts a prominent taller silhouette shadow onto the receiver wall.
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 2.5
	sun.rotation_degrees = Vector3(-22.0, 0.0, 0.0)
	sun.shadow_enabled = true
	add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.12, 0.85)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.15, 0.15, 0.18)
	env.ambient_light_energy = 0.25
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/experimental/tessellation", bool(phase["tess"]))
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_directional", bool(phase["vsm_dir"]))
	ProjectSettings.set_setting("rendering/nextgeo/tessellation/debug_max_patches", int(phase["cap"]))
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["sw_t"]))

	var use_disp := bool(phase["use_disp"])
	var use_cpu_ref := bool(phase["use_cpu_ref"])
	var dbg: String = phase["debug"]
	var cracks_mode := dbg == "cracks"
	var fallback_mode := dbg == "fallback"

	if cracks_mode:
		get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGEO_CRACKS
	elif fallback_mode:
		get_viewport().debug_draw = Viewport.DEBUG_DRAW_NEXTGEO_FALLBACK
	else:
		get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED

	_inst_std_cliff.mesh = _mesh_cpu_ref_plane if use_cpu_ref else _mesh_coarse_plane
	_inst_shader_cliff.mesh = _mesh_cpu_ref_plane if use_cpu_ref else _mesh_coarse_plane
	_inst_std_cliff.material_override = _disp_std_mat if use_disp else _plain_std_mat
	_inst_shader_cliff.material_override = _disp_shader_mat if use_disp else _plain_shader_mat
	_inst_sphere.material_override = _disp_sphere_mat if use_disp else _plain_std_mat
	# Hide the sphere in non-crack phases so cliffs + shadow silhouette are completely unobstructed.
	# In cracks_mode (DEBUG_DRAW_NEXTGEO_CRACKS), show ONLY the closed watertight displaced sphere.
	# In fallback_mode (DEBUG_DRAW_NEXTGEO_FALLBACK), show ONLY the multi-cluster displaced cliffs
	# against exact empty background to verify 0 interior cracks across shared edges and DAG borders.
	_inst_sphere.visible = cracks_mode
	_inst_std_cliff.visible = not cracks_mode
	_inst_shader_cliff.visible = not cracks_mode
	_inst_receiver.visible = not cracks_mode and not fallback_mode

	_phase_frame = 0


func _process(_delta: float) -> void:
	if _phase_frame < SETTLE_FRAMES:
		_phase_frame += 1
		return

	var phase: Dictionary = _phases[_phase_idx]
	var pname: String = phase["name"]
	_images[pname] = get_viewport().get_texture().get_image()
	_stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	_vsm_stats[pname] = RenderingServer.virtual_shadow_maps_get_debug_info()

	_phase_idx += 1
	if _phase_idx >= _phases.size():
		_finish()
		return
	_apply_phase()


func _compare_images(p_a: Image, p_b: Image) -> Dictionary:
	var ba: PackedByteArray = p_a.get_data()
	var bb: PackedByteArray = p_b.get_data()
	var diff_bytes := 0
	var max_delta := 0
	var n := mini(ba.size(), bb.size())
	for i in n:
		var d := absi(int(ba[i]) - int(bb[i]))
		if d != 0:
			diff_bytes += 1
			if d > max_delta:
				max_delta = d
	return {
		"same_size": ba.size() == bb.size() and p_a.get_size() == p_b.get_size(),
		"total_bytes": ba.size(),
		"diff_bytes": diff_bytes,
		"max_delta": max_delta,
	}


func _count_diff_pixels_in_rect(p_a: Image, p_b: Image, p_x0: int, p_y0: int, p_x1: int, p_y1: int, p_thresh: int) -> int:
	var w := mini(p_a.get_width(), p_b.get_width())
	var h := mini(p_a.get_height(), p_b.get_height())
	var x0 := clampi(p_x0, 0, w)
	var x1 := clampi(p_x1, 0, w)
	var y0 := clampi(p_y0, 0, h)
	var y1 := clampi(p_y1, 0, h)
	var count := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var ca := p_a.get_pixel(x, y)
			var cb := p_b.get_pixel(x, y)
			var dr := absi(ca.r8 - cb.r8)
			var dg := absi(ca.g8 - cb.g8)
			var db := absi(ca.b8 - cb.b8)
			if dr > p_thresh or dg > p_thresh or db > p_thresh:
				count += 1
	return count


func _count_silhouette_overlap(p_tess: Image, p_ref: Image, p_flat: Image, p_x0: int, p_y0: int, p_x1: int, p_y1: int) -> Dictionary:
	# Compare where geometry silhouette differs from the flat undisplaced render.
	var w := mini(p_tess.get_width(), p_ref.get_width())
	var h := mini(p_tess.get_height(), p_ref.get_height())
	var x0 := clampi(p_x0, 0, w)
	var x1 := clampi(p_x1, 0, w)
	var y0 := clampi(p_y0, 0, h)
	var y1 := clampi(p_y1, 0, h)
	var tess_moved := 0
	var ref_moved := 0
	var both_moved := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var ct := p_tess.get_pixel(x, y)
			var cr := p_ref.get_pixel(x, y)
			var cf := p_flat.get_pixel(x, y)
			var t_chg := absi(ct.r8 - cf.r8) > 12 or absi(ct.g8 - cf.g8) > 12 or absi(ct.b8 - cf.b8) > 12
			var r_chg := absi(cr.r8 - cf.r8) > 12 or absi(cr.g8 - cf.g8) > 12 or absi(cr.b8 - cf.b8) > 12
			if t_chg:
				tess_moved += 1
			if r_chg:
				ref_moved += 1
			if t_chg and r_chg:
				both_moved += 1
	var iou := float(both_moved) / maxf(float(tess_moved + ref_moved - both_moved), 1.0)
	return { "tess_moved": tess_moved, "ref_moved": ref_moved, "both_moved": both_moved, "iou": iou }


func _count_magenta_cracks(p_img: Image) -> int:
	var count := 0
	var w := p_img.get_width()
	var h := p_img.get_height()
	for y in h:
		for x in w:
			var c := p_img.get_pixel(x, y)
			if c.r8 == 255 and c.g8 == 0 and c.b8 == 255:
				count += 1
	return count


func _is_virtual_fallback_green(p_c: Color) -> bool:
	# DEBUG_DRAW_NEXTGEO_FALLBACK paints virtual surface pixels emerald/teal (g8 > 140) and empty
	# background pixels near-black (r8 < 16, g8 < 16, b8 < 16).
	return p_c.g8 > 140 and p_c.r8 < 80


func _count_cliff_interior_cracks(p_img: Image) -> Dictionary:
	# In DEBUG_DRAW_NEXTGEO_FALLBACK with only the multi-cluster displaced cliffs visible, any
	# non-virtual (empty background) pixel surrounded on all 4 cardinal sides (at +-6 px) by virtual
	# cliff pixels is an interior crack across shared edges or locked DAG cluster borders.
	var w := p_img.get_width()
	var h := p_img.get_height()
	var interior_cliff_px := 0
	var interior_hole_px := 0
	var pad := 6
	for y in range(pad, h - pad, 2):
		for x in range(pad, w - pad, 2):
			var c := p_img.get_pixel(x, y)
			if _is_virtual_fallback_green(c):
				interior_cliff_px += 1
				continue
			if _is_virtual_fallback_green(p_img.get_pixel(x - pad, y)) and _is_virtual_fallback_green(p_img.get_pixel(x + pad, y)) and _is_virtual_fallback_green(p_img.get_pixel(x, y - pad)) and _is_virtual_fallback_green(p_img.get_pixel(x, y + pad)):
				interior_hole_px += 1
	return { "interior_cliff_samples": interior_cliff_px, "interior_hole_pixels": interior_hole_px }


func _finish() -> void:
	# Restore settings cleanly.
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	ProjectSettings.set_setting("rendering/nextgeo/experimental/tessellation", false)
	ProjectSettings.set_setting("rendering/nextgeo/tessellation/debug_max_patches", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_directional", true)

	# (a) Feature-off invariant: byte-identical frames, 0 dicing buffers, 0 dicing dispatches.
	var off_cmp := _compare_images(_images["plain_off"], _images["disp_off"])
	_report["feature_off_cmp"] = off_cmp
	if not bool(off_cmp["same_size"]) or int(off_cmp["diff_bytes"]) != 0:
		_failures.append("experimental/tessellation=false not byte-identical: diff_bytes=%d max_delta=%d" % [int(off_cmp["diff_bytes"]), int(off_cmp["max_delta"])])

	var st_off: Dictionary = _stats.get("disp_off", {})
	_report["stats_off"] = {
		"tessellation_enabled": bool(st_off.get("tessellation_enabled", true)),
		"tessellation_buffers_allocated": bool(st_off.get("tessellation_buffers_allocated", true)),
		"tessellation_dicing_dispatches": int(st_off.get("tessellation_dicing_dispatches", -1)),
		"dicing_overflow": int(st_off.get("dicing_overflow", -1)),
	}
	if bool(st_off.get("tessellation_enabled", true)):
		_failures.append("disp_off: expected tessellation_enabled == false")
	if bool(st_off.get("tessellation_buffers_allocated", true)):
		_failures.append("disp_off: expected tessellation_buffers_allocated == false (0 buffers when off)")
	if int(st_off.get("tessellation_dicing_dispatches", -1)) != 0:
		_failures.append("disp_off: expected tessellation_dicing_dispatches == 0, got %d" % int(st_off.get("tessellation_dicing_dispatches", -1)))

	var vsm_off_nanite: Dictionary = _vsm_stats.get("disp_off", {}).get("nanite", {})
	if bool(vsm_off_nanite.get("tessellation_buffers_allocated", false)):
		_failures.append("disp_off: expected VSM tessellation_buffers_allocated == false")
	if int(vsm_off_nanite.get("tessellation_dicing_dispatches", 0)) != 0:
		_failures.append("disp_off: expected VSM tessellation_dicing_dispatches == 0")

	if _expect_tess:
		var img_flat: Image = _images["disp_off"]
		var img_ref: Image = _images["cpu_ref"]
		var img_tess: Image = _images["tess_on_vsm_on"]
		var img_tess_vsm_off: Image = _images["tess_on_vsm_off"]
		var img_tess_hw: Image = _images["tess_on_hw"]
		var img_cracks: Image = _images["tess_cracks"]
		var img_cliff_cov: Image = _images["tess_cliff_coverage"]
		var w := img_tess.get_width()
		var h := img_tess.get_height()

		# (b) Both StandardMaterial3D (left half) and custom displacement() shader (right half) visibly displace
		# and match the high-res CPU-displaced reference mesh closely.
		var left_match := _count_silhouette_overlap(img_tess, img_ref, img_flat, 0, int(h * 0.35), int(w * 0.5), h)
		var right_match := _count_silhouette_overlap(img_tess, img_ref, img_flat, int(w * 0.5), int(h * 0.35), w, h)
		_report["displacement_match"] = { "std_left": left_match, "shader_right": right_match }
		if int(left_match["tess_moved"]) < 50 or float(left_match["iou"]) < 0.45:
			_failures.append("StandardMaterial3D displacement mismatch vs CPU ref: %s" % JSON.stringify(left_match))
		if int(right_match["tess_moved"]) < 50 or float(right_match["iou"]) < 0.45:
			_failures.append("Custom displacement() shader mismatch vs CPU ref: %s" % JSON.stringify(right_match))

		# Check HW stream phase produces valid displaced render closely matching SW+HW hybrid phase.
		var hw_vs_flat := _count_diff_pixels_in_rect(img_tess_hw, img_flat, 0, 0, w, h, 12)
		var st_on: Dictionary = _stats.get("tess_on_vsm_on", {})
		var st_hw: Dictionary = _stats.get("tess_on_hw", {})
		_report["stats_on"] = {
			"tessellation_enabled": bool(st_on.get("tessellation_enabled", false)),
			"tessellation_buffers_allocated": bool(st_on.get("tessellation_buffers_allocated", false)),
			"tessellation_dicing_dispatches": int(st_on.get("tessellation_dicing_dispatches", 0)),
			"tessellation_patches": int(st_on.get("tessellation_patches", 0)),
			"tessellation_diced_clusters": int(st_on.get("tessellation_diced_clusters", 0)),
			"tessellation_sw_micro_tris": int(st_on.get("tessellation_sw_micro_tris", 0)),
			"tessellation_hw_patches": int(st_on.get("tessellation_hw_patches", 0)),
			"dicing_overflow": int(st_on.get("dicing_overflow", -1)),
		}
		_report["stats_hw"] = {
			"tessellation_hw_patches": int(st_hw.get("tessellation_hw_patches", 0)),
			"tessellation_patches": int(st_hw.get("tessellation_patches", 0)),
			"hw_vs_flat_diff_pixels": hw_vs_flat,
		}
		if not bool(st_on.get("tessellation_enabled", false)) or not bool(st_on.get("tessellation_buffers_allocated", false)):
			_failures.append("tess_on_vsm_on: expected tessellation_enabled and tessellation_buffers_allocated == true")
		if int(st_on.get("tessellation_dicing_dispatches", 0)) <= 0 or int(st_on.get("tessellation_patches", 0)) <= 0:
			_failures.append("tess_on_vsm_on: expected dicing dispatches > 0 and patches > 0")
		if int(st_on.get("tessellation_diced_clusters", 0)) < 4:
			_failures.append("tess_on_vsm_on: expected multi-cluster dicing (>= 4 clusters for cluster-border coverage), got %d" % int(st_on.get("tessellation_diced_clusters", 0)))
		if int(st_on.get("dicing_overflow", -1)) != 0:
			_failures.append("tess_on_vsm_on: unexpected dicing_overflow=%d" % int(st_on.get("dicing_overflow", -1)))
		if int(st_hw.get("tessellation_hw_patches", 0)) <= 0 or hw_vs_flat < 100:
			_failures.append("tess_on_hw: expected bounded HW stream patches > 0 and displaced pixels > 100 (got patches=%d diff=%d)" % [int(st_hw.get("tessellation_hw_patches", 0)), hw_vs_flat])

		# Crack checks:
		# 1. 0 magenta (255, 0, 255) crack pixels in DEBUG_DRAW_NEXTGEO_CRACKS on closed displaced multi-cluster sphere.
		# 2. 0 interior pinhole/crack pixels inside both multi-cluster displaced cliffs (spanning 18 clusters & DAG borders).
		var crack_px := _count_magenta_cracks(img_cracks)
		var cliff_cracks := _count_cliff_interior_cracks(img_cliff_cov)
		_report["crack_pixels"] = crack_px
		_report["cliff_interior_cracks"] = cliff_cracks
		if crack_px != 0:
			_failures.append("tess_cracks: detected %d magenta crack pixels (expected 0)" % crack_px)
		if int(cliff_cracks["interior_cliff_samples"]) < 1000 or int(cliff_cracks["interior_hole_pixels"]) != 0:
			_failures.append("tess_on_vsm_on: cliff interior crack check failed: %s" % JSON.stringify(cliff_cracks))

		# (c) VSM shadow variant: directional shadow silhouette changes between tessellation_directional=true vs false.
		var vsm_on_info: Dictionary = _vsm_stats.get("tess_on_vsm_on", {})
		var vsm_off_info: Dictionary = _vsm_stats.get("tess_on_vsm_off", {})
		var vsm_active := bool(vsm_on_info.get("active", false))
		var shadow_diff_px := _count_diff_pixels_in_rect(img_tess, img_tess_vsm_off, 0, 0, w, int(h * 0.65), 10)
		_report["vsm_shadow_check"] = {
			"vsm_active": vsm_active,
			"shadow_diff_pixels": shadow_diff_px,
			"vsm_nanite_on": vsm_on_info.get("nanite", {}),
			"vsm_nanite_off": vsm_off_info.get("nanite", {}),
		}
		if vsm_active:
			var nanite_on: Dictionary = vsm_on_info.get("nanite", {})
			var nanite_off: Dictionary = vsm_off_info.get("nanite", {})
			if int(nanite_on.get("tessellation_dicing_dispatches", 0)) <= 0:
				_failures.append("tess_on_vsm_on: expected VSM tessellation_dicing_dispatches > 0")
			if int(nanite_off.get("tessellation_dicing_dispatches", 0)) != 0:
				_failures.append("tess_on_vsm_off: expected VSM tessellation_dicing_dispatches == 0 when tessellation_directional=false")
			if shadow_diff_px < 20:
				_failures.append("VSM shadows did not reflect displaced silhouette between tessellation_directional=true vs false (diff_px=%d)" % shadow_diff_px)

		# (d) Tight patch cap overflow check.
		var st_ov: Dictionary = _stats.get("tess_overflow", {})
		_report["stats_overflow"] = {
			"dicing_overflow": int(st_ov.get("dicing_overflow", 0)),
			"tessellation_patches": int(st_ov.get("tessellation_patches", 0)),
			"tessellation_coarsened_tris": int(st_ov.get("tessellation_coarsened_tris", 0)),
		}
		if int(st_ov.get("dicing_overflow", 0)) <= 0:
			_failures.append("tess_overflow: expected dicing_overflow > 0 under debug_max_patches=8")
		if int(st_ov.get("tessellation_patches", 999999)) > 8:
			_failures.append("tess_overflow: expected clamped tessellation_patches <= 8, got %d" % int(st_ov.get("tessellation_patches", 999999)))

	_report["pass"] = _failures.is_empty()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
