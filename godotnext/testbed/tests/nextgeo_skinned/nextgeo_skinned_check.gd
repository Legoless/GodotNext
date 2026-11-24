extends Node3D
## NextGeo skinned runtime verification check (#160, plan §21.4 contract N8, GN-5, GN-12, N4).
##
## Verifies all four runtime guarantees of skinned Nanite:
##   (a) Posed/animated skinned virtual mesh matches legacy skinning across HW VS, SW compute,
##       and HW task/mesh raster paths (0 culled-but-visible clusters, cut_match, overflow == 0,
##       nextgeo_double_dispatch == 0, nextgeo_missing == 0). Also tests a masked skinned surface
##       through programmable raster bins.
##   (b) Motion velocity is near-zero on static frames and non-zero with accurate direction on
##       animated limbs.
##   (c) Dynamic VSM shadows follow the posed skeleton dynamically between Pose A and Pose B,
##       matching legacy shadow placement with overflow == 0 and sink == 0.
##   (d) An off-screen skinned virtual instance placed behind the camera inside NextGI RT range
##       receives mesh_instance_check_for_update and refits its #59 RT proxy BLAS
##       (rt_skinned_refit_triangles > 0) while the on-screen actor is hidden.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_skinned/nextgeo_skinned_probe.gd")

const PHASE_CAPTURE_FRAME := 20
const PHASE_STATS_FRAME := 24
const VEL_STATIC_CAPTURE := 16
const VEL_MOVING_CAPTURE := 24
const RT_REFIT_FRAMES := 24

# Color parity against legacy skinning: LOD / cluster quantization can cause subtle silhouette
# anti-aliasing differences at pixel edges, while interior shading and overall silhouette must match tightly.
const MAX_LEGACY_MAE := 0.015
const MAX_LEGACY_P99 := 0.08
const MAX_PATH_PARITY_MAE := 0.008
const MAX_PATH_PARITY_P99 := 0.04

const MASKED_SKINNED_SHADER := """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.82, 0.36, 0.24);
	ROUGHNESS = 0.65;
	ALPHA = step(0.35, fract(UV.y * 8.0));
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""

var _phases: Array[Dictionary] = [
	{ "name": "legacy_pose_a", "virtual": false, "pose": "a", "path": 1, "t": 0.0, "vel": false, "anim": false, "offscreen_rt": false },
	{ "name": "virtual_hw_vs", "virtual": true, "pose": "a", "path": 1, "t": 0.0, "vel": false, "anim": false, "offscreen_rt": false },
	{ "name": "virtual_sw", "virtual": true, "pose": "a", "path": 1, "t": 512.0, "vel": false, "anim": false, "offscreen_rt": false },
	{ "name": "virtual_hw_mesh", "virtual": true, "pose": "a", "path": 2, "t": 0.0, "vel": false, "anim": false, "offscreen_rt": false },
	{ "name": "velocity_static", "virtual": true, "pose": "a", "path": 1, "t": 0.0, "vel": true, "anim": false, "offscreen_rt": false },
	{ "name": "velocity_moving", "virtual": true, "pose": "anim", "path": 1, "t": 0.0, "vel": true, "anim": true, "offscreen_rt": false },
	{ "name": "legacy_pose_b", "virtual": false, "pose": "b", "path": 1, "t": 0.0, "vel": false, "anim": false, "offscreen_rt": false },
	{ "name": "virtual_pose_b", "virtual": true, "pose": "b", "path": 1, "t": 0.0, "vel": false, "anim": false, "offscreen_rt": false },
	{ "name": "offscreen_rt_refit", "virtual": false, "pose": "a", "path": 1, "t": 0.0, "vel": false, "anim": false, "offscreen_rt": true },
]

var _phase_idx := 0
var _phase_frame := 0
var _probe: CompositorEffect
var _env: Environment
var _camera: Camera3D
var _sun: DirectionalLight3D

var _skeleton: Skeleton3D
var _legacy_arm: MeshInstance3D
var _virtual_arm: MeshInstance3D
var _legacy_masked: MeshInstance3D
var _virtual_masked: MeshInstance3D

var _offscreen_skeleton: Skeleton3D
var _offscreen_virtual: MeshInstance3D

var _images: Dictionary = {}
var _vel_captures: Dictionary = {}
var _ng_stats: Dictionary = {}
var _vsm_stats: Dictionary = {}
var _vsm_peaks: Dictionary = {}
var _rt_refit_peak := 0
var _rt_refit_frames := 0
var _rt_build_peak := 0
var _rt_tier := ""
var _temp_files: Array[String] = []
var _failures: Array[String] = []
var _report: Dictionary = {}
var _expect_hybrid := false
var _expect_mesh := false


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/raster_mode", 0)
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/occlusion", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/shadow_method", 1)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 0)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", false)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", 0.0)

	var rd := RenderingServer.get_rendering_device()
	var image_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS)
	var buffer_int64 := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS)
	var indirect_count := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	var mesh_shader := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER)
	var mesh_indirect := rd != null and rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER_INDIRECT)
	_expect_hybrid = indirect_count and (image_int64 or buffer_int64)
	_expect_mesh = _expect_hybrid and mesh_shader and mesh_indirect

	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_hybrid": _expect_hybrid,
		"expect_mesh": _expect_mesh,
	}

	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.5

	_build_scene()
	_apply_phase()


func _make_skinned_arm_arrays(p_radial: int, p_rings: int, p_radius: float, p_height: float) -> Array:
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var tangs := PackedFloat32Array()
	var uvs := PackedVector2Array()
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	var indices := PackedInt32Array()

	var cols := p_radial + 1
	for r in range(p_rings + 1):
		var v := float(r) / float(p_rings)
		var y := v * p_height
		# Taper and ripple along height so NextGeo retains fine clusters (small max_edge for SW raster).
		var rad := p_radius * (1.0 - 0.35 * v) + 0.045 * sin(v * 18.0 * PI)
		var dr_dy := (-0.35 * p_radius + 0.045 * 18.0 * PI * cos(v * 18.0 * PI)) / p_height
		# Smooth 3-bone weights across joints at y = p_height/3 and y = 2*p_height/3.
		var w0 := clampf(1.0 - maxf(v - 0.18, 0.0) / 0.28, 0.0, 1.0)
		var w2 := clampf((v - 0.54) / 0.28, 0.0, 1.0)
		var w1 := maxf(0.0, 1.0 - w0 - w2)
		var wsum := w0 + w1 + w2
		if wsum > 0.0:
			w0 /= wsum
			w1 /= wsum
			w2 /= wsum
		for c in range(cols):
			var u := float(c) / float(p_radial)
			var ang := u * TAU
			var ca := cos(ang)
			var sa := sin(ang)
			verts.append(Vector3(rad * ca, y, rad * sa))
			norms.append(Vector3(ca, -dr_dy, sa).normalized())
			tangs.append(-sa)
			tangs.append(0.0)
			tangs.append(ca)
			tangs.append(1.0)
			uvs.append(Vector2(u, v))
			bones.append(0)
			bones.append(1)
			bones.append(2)
			bones.append(0)
			weights.append(w0)
			weights.append(w1)
			weights.append(w2)
			weights.append(0.0)

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
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights
	arrays[Mesh.ARRAY_INDEX] = indices
	return arrays


func _build_arm_meshes(p_arrays: Array, p_mat: Material, p_tag: String) -> Dictionary:
	var legacy := ArrayMesh.new()
	legacy.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, p_arrays)
	legacy.surface_set_material(0, p_mat)

	var path := "user://nextgeo_skinned_%s.ngm" % p_tag
	_temp_files.append(path)
	var importer := ImporterMesh.new()
	importer.add_surface(Mesh.PRIMITIVE_TRIANGLES, p_arrays, [], {}, p_mat)
	var err := importer.build_virtual_geometry(path, true)
	var virt := importer.get_mesh()
	if err != OK or virt == null or virt.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s, skinned=true) failed (err=%d)" % [p_tag, err])
	elif virt.get_virtual_geometry().get_skin_influences() != 4:
		_failures.append("%s: expected skin_influences=4, got %d" % [p_tag, virt.get_virtual_geometry().get_skin_influences()])
	return { "legacy": legacy, "virtual": virt }


func _make_3bone_skeleton(p_name: String, p_pos: Vector3) -> Dictionary:
	var skel := Skeleton3D.new()
	skel.name = p_name
	skel.position = p_pos
	skel.add_bone("Root")
	skel.set_bone_rest(0, Transform3D(Basis(), Vector3(0.0, 0.0, 0.0)))
	skel.add_bone("Mid")
	skel.set_bone_parent(1, 0)
	skel.set_bone_rest(1, Transform3D(Basis(), Vector3(0.0, 0.8, 0.0)))
	skel.add_bone("Tip")
	skel.set_bone_parent(2, 1)
	skel.set_bone_rest(2, Transform3D(Basis(), Vector3(0.0, 0.8, 0.0)))
	skel.reset_bone_poses()
	add_child(skel)

	var skin := Skin.new()
	skin.add_bind(0, Transform3D(Basis(), Vector3(0.0, 0.0, 0.0)))
	skin.add_bind(1, Transform3D(Basis(), Vector3(0.0, -0.8, 0.0)))
	skin.add_bind(2, Transform3D(Basis(), Vector3(0.0, -1.6, 0.0)))
	return { "skeleton": skel, "skin": skin }


func _set_skeleton_pose(p_skel: Skeleton3D, p_mid_z_deg: float, p_mid_y_deg: float, p_tip_z_deg: float, p_tip_scale: float) -> void:
	p_skel.set_bone_pose_position(0, Vector3.ZERO)
	p_skel.set_bone_pose_rotation(0, Quaternion.IDENTITY)
	p_skel.set_bone_pose_scale(0, Vector3.ONE)

	p_skel.set_bone_pose_position(1, Vector3(0.0, 0.8, 0.0))
	var q_mid := Quaternion(Vector3.BACK, deg_to_rad(p_mid_z_deg)) * Quaternion(Vector3.UP, deg_to_rad(p_mid_y_deg))
	p_skel.set_bone_pose_rotation(1, q_mid)
	p_skel.set_bone_pose_scale(1, Vector3.ONE)

	p_skel.set_bone_pose_position(2, Vector3(0.0, 0.8, 0.0))
	p_skel.set_bone_pose_rotation(2, Quaternion(Vector3.BACK, deg_to_rad(p_tip_z_deg)))
	p_skel.set_bone_pose_scale(2, Vector3(p_tip_scale, p_tip_scale, p_tip_scale))


func _build_scene() -> void:
	_camera = Camera3D.new()
	_camera.fov = 52.0
	_camera.near = 0.05
	_camera.far = 80.0
	_camera.position = Vector3(0.0, 1.8, 5.2)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3(0.0, 0.9, 0.0))

	# Floor catching dynamic VSM shadows from the posed skeleton.
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(14.0, 0.4, 14.0)
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.86, 0.86, 0.86)
	floor_mat.roughness = 0.92
	var floor_inst := MeshInstance3D.new()
	floor_inst.name = "Floor"
	floor_inst.mesh = floor_box
	floor_inst.material_override = floor_mat
	floor_inst.position = Vector3(0.0, -0.2, 0.0)
	add_child(floor_inst)

	# Build opaque skinned arm (high tessellation: 48x64 = 6144 tris -> ~48+ clusters) and masked skinned ribbon.
	var arm_arrays := _make_skinned_arm_arrays(48, 64, 0.36, 2.4)
	var opaque_mat := StandardMaterial3D.new()
	opaque_mat.albedo_color = Color(0.32, 0.62, 0.82)
	opaque_mat.roughness = 0.55
	var arm_meshes := _build_arm_meshes(arm_arrays, opaque_mat, "arm_opaque")

	var masked_arrays := _make_skinned_arm_arrays(32, 40, 0.24, 2.0)
	var masked_shader := Shader.new()
	masked_shader.code = MASKED_SKINNED_SHADER
	var masked_mat := ShaderMaterial.new()
	masked_mat.shader = masked_shader
	var masked_meshes := _build_arm_meshes(masked_arrays, masked_mat, "arm_masked")

	var rig := _make_3bone_skeleton("MainSkeleton", Vector3(-0.35, 0.0, 0.0))
	_skeleton = rig["skeleton"]
	var skin: Skin = rig["skin"]

	_legacy_arm = MeshInstance3D.new()
	_legacy_arm.name = "LegacyArm"
	_legacy_arm.mesh = arm_meshes["legacy"]
	_legacy_arm.skin = skin
	_legacy_arm.skeleton = NodePath("..")
	_legacy_arm.extra_cull_margin = 4.0
	_skeleton.add_child(_legacy_arm)

	_virtual_arm = MeshInstance3D.new()
	_virtual_arm.name = "VirtualArm"
	_virtual_arm.mesh = arm_meshes["virtual"]
	_virtual_arm.skin = skin
	_virtual_arm.skeleton = NodePath("..")
	_virtual_arm.extra_cull_margin = 4.0
	_skeleton.add_child(_virtual_arm)

	_legacy_masked = MeshInstance3D.new()
	_legacy_masked.name = "LegacyMaskedArm"
	_legacy_masked.mesh = masked_meshes["legacy"]
	_legacy_masked.skin = skin
	_legacy_masked.skeleton = NodePath("..")
	_legacy_masked.position = Vector3(1.35, 0.0, 0.0)
	_legacy_masked.extra_cull_margin = 4.0
	_skeleton.add_child(_legacy_masked)

	_virtual_masked = MeshInstance3D.new()
	_virtual_masked.name = "VirtualMaskedArm"
	_virtual_masked.mesh = masked_meshes["virtual"]
	_virtual_masked.skin = skin
	_virtual_masked.skeleton = NodePath("..")
	_virtual_masked.position = Vector3(1.35, 0.0, 0.0)
	_virtual_masked.extra_cull_margin = 4.0
	_skeleton.add_child(_virtual_masked)

	# Off-screen skinned virtual actor placed directly behind the camera (z = +9.5 vs cam z = +5.2 looking -Z),
	# strictly frustum-culled and ~4.3m away (well inside the 30m NextGI RT skinned refit range).
	var off_rig := _make_3bone_skeleton("OffscreenSkeleton", Vector3(0.0, 0.0, 9.5))
	_offscreen_skeleton = off_rig["skeleton"]
	_offscreen_virtual = MeshInstance3D.new()
	_offscreen_virtual.name = "OffscreenVirtualArm"
	_offscreen_virtual.mesh = arm_meshes["virtual"]
	_offscreen_virtual.skin = off_rig["skin"]
	_offscreen_virtual.skeleton = NodePath("..")
	_offscreen_virtual.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
	_offscreen_virtual.extra_cull_margin = 4.0
	_offscreen_virtual.visible = false
	_offscreen_skeleton.add_child(_offscreen_virtual)

	_sun = DirectionalLight3D.new()
	# Shine from behind-above the articulated arms toward the foreground floor so VSM shadows fall
	# directly in front of the camera and visibly shift between Pose A and Pose B.
	_sun.rotation_degrees = Vector3(-40.0, 165.0, 0.0)
	_sun.light_energy = 1.8
	_sun.shadow_enabled = true
	add_child(_sun)

	_env = Environment.new()
	_env.background_mode = Environment.BG_COLOR
	_env.background_color = Color(0.08, 0.1, 0.14)
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.14, 0.14, 0.16)
	_env.ambient_light_energy = 1.0
	_env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	_env.nextgi_enabled = false

	var compositor := Compositor.new()
	_probe = ProbeScript.new()
	_probe.needs_motion_vectors = false
	var effects: Array[CompositorEffect] = [_probe]
	compositor.compositor_effects = effects

	var world := WorldEnvironment.new()
	world.environment = _env
	world.compositor = compositor
	add_child(world)


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var pname: String = phase["name"]
	var use_virt: bool = phase["virtual"]
	var offscreen_rt: bool = phase["offscreen_rt"]

	ProjectSettings.set_setting("rendering/nextgeo/hw_raster_path", int(phase["path"]))
	ProjectSettings.set_setting("rendering/nextgeo/sw_raster_max_edge_px", float(phase["t"]))
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels", float(phase["t"]))

	_probe.needs_motion_vectors = bool(phase["vel"])
	_probe.capture = {}
	_probe.capture_armed = false

	_env.nextgi_enabled = offscreen_rt
	_legacy_arm.visible = (not use_virt) and (not offscreen_rt)
	_legacy_masked.visible = (not use_virt) and (not offscreen_rt)
	_virtual_arm.visible = use_virt and (not offscreen_rt)
	_virtual_masked.visible = use_virt and (not offscreen_rt)
	_offscreen_virtual.visible = offscreen_rt

	match String(phase["pose"]):
		"a":
			_set_skeleton_pose(_skeleton, 38.0, 22.0, -48.0, 1.15)
		"b":
			_set_skeleton_pose(_skeleton, -44.0, -18.0, 52.0, 1.15)
		"anim":
			_set_skeleton_pose(_skeleton, -25.0, 0.0, -20.0, 1.0)

	# Reset VSM pages cleanly at phase boundary for VSM parity phases.
	if not offscreen_rt and not bool(phase["vel"]):
		_sun.shadow_enabled = false
	else:
		_sun.shadow_enabled = true
	_vsm_peaks[pname] = { "hw": 0, "sw": 0, "prog_hw": 0, "prog_sw": 0, "overflow": 0, "sink": 0 }
	_phase_frame = 0


func _process(_delta: float) -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var pname: String = phase["name"]
	var offscreen_rt: bool = phase["offscreen_rt"]

	if not offscreen_rt and not bool(phase["vel"]) and _phase_frame == 3:
		_sun.shadow_enabled = true

	if bool(phase["anim"]):
		# Steady angular sweep so every limb pixel has clear, consistent screen-space motion.
		var angle := -25.0 + float(_phase_frame) * 5.0
		_set_skeleton_pose(_skeleton, angle, 0.0, angle * 0.6, 1.0)

	if offscreen_rt:
		# Animate ONLY the off-screen skinned virtual mesh behind the camera.
		var osc := sin(float(_phase_frame) * 0.45) * 45.0
		_set_skeleton_pose(_offscreen_skeleton, osc, osc * 0.5, -osc * 0.8, 1.0)
		if _phase_frame >= 2:
			var ti: Dictionary = RenderingServer.nextgi_get_tier_info()
			_rt_tier = String(ti.get("tier", ""))
			var refit := int(ti.get("rt_skinned_refit_triangles", 0))
			var build := int(ti.get("rt_skinned_build_triangles", 0))
			_rt_refit_peak = maxi(_rt_refit_peak, refit)
			_rt_build_peak = maxi(_rt_build_peak, build)
			if refit > 0:
				_rt_refit_frames += 1
		if _phase_frame >= RT_REFIT_FRAMES:
			_finish()
			return
		_phase_frame += 1
		return

	if _phase_frame >= 8:
		var vsm_now: Dictionary = RenderingServer.virtual_shadow_maps_get_debug_info()
		var vn: Dictionary = vsm_now.get("nanite", {})
		if bool(vn.get("active", false)) and bool(vn.get("gpu_counters_valid", false)):
			var p: Dictionary = _vsm_peaks[pname]
			p["hw"] = maxi(int(p["hw"]), int(vn.get("hw_clusters", 0)))
			p["sw"] = maxi(int(p["sw"]), int(vn.get("sw_clusters", 0)))
			p["prog_hw"] = maxi(int(p["prog_hw"]), int(vn.get("prog_hw_clusters", 0)))
			p["prog_sw"] = maxi(int(p["prog_sw"]), int(vn.get("prog_sw_clusters", 0)))
			p["overflow"] = maxi(int(p["overflow"]), int(vn.get("overflow", 0)))
			p["sink"] = maxi(int(p["sink"]), int(vn.get("sink", 0)))

	if bool(phase["vel"]):
		var cap_f := VEL_MOVING_CAPTURE if bool(phase["anim"]) else VEL_STATIC_CAPTURE
		if _phase_frame == cap_f:
			_probe.capture_armed = true
		elif _phase_frame == cap_f + 1:
			_vel_captures[pname] = _probe.capture
			_probe.capture = {}
		elif _phase_frame == cap_f + 3:
			_ng_stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
			_advance_phase()
			return
	else:
		if _phase_frame == PHASE_CAPTURE_FRAME:
			_images[pname] = get_viewport().get_texture().get_image()
		if _phase_frame == PHASE_STATS_FRAME:
			_ng_stats[pname] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
			_vsm_stats[pname] = RenderingServer.virtual_shadow_maps_get_debug_info()
			_advance_phase()
			return

	_phase_frame += 1


func _advance_phase() -> void:
	_phase_idx += 1
	if _phase_idx >= _phases.size():
		_finish()
		return
	_apply_phase()


func _compare_images(p_a_name: String, p_b_name: String, p_max_mae: float, p_max_p99: float) -> Dictionary:
	var out := {}
	var a: Image = _images.get(p_a_name)
	var b: Image = _images.get(p_b_name)
	if a == null or b == null or a.get_size() != b.get_size():
		_failures.append("%s vs %s: missing viewport image" % [p_b_name, p_a_name])
		return out
	var w := a.get_width()
	var h := a.get_height()
	var sum_err := 0.0
	var diffs: Array[float] = []
	diffs.resize(w * h)
	for y in h:
		for x in w:
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var d := (absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b)) / 3.0
			sum_err += d
			diffs[y * w + x] = d
	diffs.sort()
	var mae := sum_err / float(maxi(w * h, 1))
	var p99 := diffs[mini(int(float(diffs.size()) * 0.99), diffs.size() - 1)]
	out["mae"] = snappedf(mae, 1e-6)
	out["p99"] = snappedf(p99, 1e-6)
	if mae > p_max_mae or p99 > p_max_p99:
		_failures.append("%s vs %s: parity exceeded tolerance (mae=%.5f > %.5f or p99=%.5f > %.5f)" % [p_b_name, p_a_name, mae, p_max_mae, p99, p_max_p99])
	return out


func _analyze_velocity(p_name: String) -> Dictionary:
	var cap: Dictionary = _vel_captures.get(p_name, {})
	if cap.is_empty() or not bool(cap.get("velocity", false)):
		_failures.append("%s: missing velocity probe capture" % p_name)
		return {}
	var words: PackedInt32Array = cap["words"]
	var pixels := words.size() / ProbeScript.WORDS_PER_PIXEL
	var ng_pixels := 0
	var moving_pixels := 0
	var sum_mag := 0.0
	var max_mag := 0.0
	var sum_vx := 0.0
	var sum_vy := 0.0
	var buf := PackedByteArray()
	buf.resize(8)
	for i in pixels:
		var base := i * ProbeScript.WORDS_PER_PIXEL
		if words[base] == -1:
			continue
		ng_pixels += 1
		buf.encode_s32(0, words[base + 1])
		buf.encode_s32(4, words[base + 2])
		var vx := buf.decode_float(0)
		var vy := buf.decode_float(4)
		var mag := sqrt(vx * vx + vy * vy)
		sum_mag += mag
		sum_vx += vx
		sum_vy += vy
		if mag > max_mag:
			max_mag = mag
		if mag > 0.002:
			moving_pixels += 1
	var mean_mag := sum_mag / float(maxi(ng_pixels, 1))
	var mean_vx := sum_vx / float(maxi(ng_pixels, 1))
	var mean_vy := sum_vy / float(maxi(ng_pixels, 1))
	return {
		"ng_pixels": ng_pixels,
		"moving_pixels": moving_pixels,
		"mean_mag": snappedf(mean_mag, 1e-6),
		"max_mag": snappedf(max_mag, 1e-6),
		"mean_vx": snappedf(mean_vx, 1e-6),
		"mean_vy": snappedf(mean_vy, 1e-6),
	}


func _floor_shadow_shift(p_a_name: String, p_b_name: String) -> float:
	var a: Image = _images.get(p_a_name)
	var b: Image = _images.get(p_b_name)
	if a == null or b == null or a.get_size() != b.get_size():
		return 0.0
	# Sample lower third of the frame where the floor shadow falls.
	var w := a.get_width()
	var h := a.get_height()
	var y0 := int(float(h) * 0.65)
	var diff_sum := 0.0
	var count := 0
	for y in range(y0, h):
		for x in w:
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			diff_sum += (absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b)) / 3.0
			count += 1
	return diff_sum / float(maxi(count, 1))


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	_temp_files.clear()


func _finish() -> void:
	# (a) Visual & cut parity across HW VS, SW, and HW Mesh paths.
	var comparisons := {}
	comparisons["hw_vs_vs_legacy_a"] = _compare_images("legacy_pose_a", "virtual_hw_vs", MAX_LEGACY_MAE, MAX_LEGACY_P99)
	comparisons["sw_vs_legacy_a"] = _compare_images("legacy_pose_a", "virtual_sw", MAX_LEGACY_MAE, MAX_LEGACY_P99)
	comparisons["hw_mesh_vs_legacy_a"] = _compare_images("legacy_pose_a", "virtual_hw_mesh", MAX_LEGACY_MAE, MAX_LEGACY_P99)
	comparisons["sw_vs_hw_vs"] = _compare_images("virtual_hw_vs", "virtual_sw", MAX_PATH_PARITY_MAE, MAX_PATH_PARITY_P99)
	comparisons["hw_mesh_vs_hw_vs"] = _compare_images("virtual_hw_vs", "virtual_hw_mesh", MAX_PATH_PARITY_MAE, MAX_PATH_PARITY_P99)
	comparisons["pose_b_vs_legacy_b"] = _compare_images("legacy_pose_b", "virtual_pose_b", MAX_LEGACY_MAE, MAX_LEGACY_P99)

	for pname in ["virtual_hw_vs", "virtual_sw", "virtual_hw_mesh", "virtual_pose_b"]:
		var ng: Dictionary = _ng_stats.get(pname, {})
		if int(ng.get("double_dispatch", 0)) != 0:
			_failures.append("%s: nextgeo_double_dispatch=%d" % [pname, int(ng.get("double_dispatch", 0))])
		if int(ng.get("missing", 0)) != 0:
			_failures.append("%s: nextgeo_missing=%d" % [pname, int(ng.get("missing", 0))])
		if int(ng.get("overflow", 0)) != 0:
			_failures.append("%s: overflow=%d" % [pname, int(ng.get("overflow", 0))])
		if _expect_hybrid:
			if not bool(ng.get("cut_match", false)):
				_failures.append("%s: GPU/CPU cut_match is false" % pname)
			if pname == "virtual_sw" and int(ng.get("sw_clusters", 0)) <= 0:
				_failures.append("virtual_sw: expected sw_clusters > 0, got %d" % int(ng.get("sw_clusters", 0)))
			if pname == "virtual_hw_vs" and int(ng.get("hw_clusters", 0)) <= 0:
				_failures.append("virtual_hw_vs: expected hw_clusters > 0, got %d" % int(ng.get("hw_clusters", 0)))
			if pname == "virtual_hw_mesh" and _expect_mesh:
				if String(ng.get("hw_raster_path", "")) != "mesh":
					_failures.append("virtual_hw_mesh: expected hw_raster_path=mesh, got %s" % String(ng.get("hw_raster_path", "")))
				if int(ng.get("hw_clusters", 0)) <= 0:
					_failures.append("virtual_hw_mesh: expected hw_clusters > 0")
		var vp: Dictionary = _vsm_peaks.get(pname, {})
		if int(vp.get("overflow", 0)) != 0:
			_failures.append("%s: VSM overflow=%d" % [pname, int(vp.get("overflow", 0))])
		if int(vp.get("sink", 0)) != 0:
			_failures.append("%s: VSM sink=%d" % [pname, int(vp.get("sink", 0))])
		if OS.get_environment("GODOT_DEV_VSM") != "":
			var vsm_n: Dictionary = _vsm_stats.get(pname, {}).get("nanite", {})
			if not bool(vsm_n.get("active", false)):
				_failures.append("%s: GODOT_DEV_VSM is set but VSM nanite is inactive" % pname)
			var total_vsm := int(vp.get("hw", 0)) + int(vp.get("sw", 0)) + int(vp.get("prog_hw", 0)) + int(vp.get("prog_sw", 0))
			if total_vsm <= 0:
				_failures.append("%s: expected VSM skinned nanite clusters > 0" % pname)

	# (b) Motion velocity checks.
	var vel_static := _analyze_velocity("velocity_static")
	var vel_moving := _analyze_velocity("velocity_moving")
	if int(vel_static.get("ng_pixels", 0)) < 200:
		_failures.append("velocity_static: too few NextGeo skinned pixels (%d)" % int(vel_static.get("ng_pixels", 0)))
	if float(vel_static.get("mean_mag", 1.0)) > 0.0005:
		_failures.append("velocity_static: non-zero velocity on stationary skeleton (mean_mag=%.6f)" % float(vel_static.get("mean_mag", 1.0)))
	if int(vel_moving.get("moving_pixels", 0)) < 100 or float(vel_moving.get("max_mag", 0.0)) < 0.005:
		_failures.append("velocity_moving: expected non-zero velocity on animated limbs (moving_pixels=%d, max_mag=%.6f)" % [int(vel_moving.get("moving_pixels", 0)), float(vel_moving.get("max_mag", 0.0))])

	# (c) Dynamic VSM shadow movement between Pose A and Pose B.
	var shadow_shift := _floor_shadow_shift("virtual_hw_vs", "virtual_pose_b")
	if shadow_shift < 0.01:
		_failures.append("VSM shadow on floor did not shift between Pose A and Pose B (shift=%.5f)" % shadow_shift)

	# (d) Off-screen RT-range proxy refit verification.
	var rt_info := {
		"tier": _rt_tier,
		"skinned_refit_peak": _rt_refit_peak,
		"skinned_refit_frames": _rt_refit_frames,
		"skinned_build_peak": _rt_build_peak,
	}
	if _rt_tier == "HWRT" or _rt_tier == "SWRT":
		if _rt_refit_peak <= 0 or _rt_refit_frames <= 0:
			_failures.append("offscreen_rt_refit: off-screen skinned virtual mesh behind camera did not refit RT proxy BLAS (tier=%s, refit_peak=%d, build_peak=%d)" % [_rt_tier, _rt_refit_peak, _rt_build_peak])

	_cleanup_temp_files()
	_report["comparisons"] = comparisons
	_report["velocity"] = { "static": vel_static, "moving": vel_moving }
	_report["vsm_shadow_shift"] = snappedf(shadow_shift, 1e-6)
	_report["vsm_peaks"] = _vsm_peaks
	_report["offscreen_rt"] = rt_info
	_report["ng_stats"] = {
		"virtual_hw_vs": _ng_stats.get("virtual_hw_vs", {}),
		"virtual_sw": _ng_stats.get("virtual_sw", {}),
		"virtual_hw_mesh": _ng_stats.get("virtual_hw_mesh", {}),
	}
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
