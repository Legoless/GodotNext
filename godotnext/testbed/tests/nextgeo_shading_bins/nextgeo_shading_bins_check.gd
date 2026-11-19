extends Node3D
## NextGeo material shading bins check (#158, plan §21.4 N7).
##
## Renders one static scene of virtualized meshes (a sphere and a box sharing tiles, the box textured and
## normal-mapped, a second sphere with another material, and, where the hybrid raster runs, a masked
## plane admitted through rendering/nextgeo/debug_programmable_raster), lit by a shadowed directional
## light and a shadowed omni light, and reads back every internal pixel's instance ID, opaque colour,
## normal-roughness, velocity, separate specular, MegaLights inputs and voxel GI pair after the opaque pass,
## in phases:
## - `tile_quads`, `tile_quads_mv`, `tile_quads_spec`, `tile_quads_spec_mv`, `tile_quads_vgi`:
##   rendering/nextgeo/shading_path = Tile Quads, with plain colour, with motion vectors, with separate
##   specular, with both, and with a VoxelGI in view, so Stage A writes the voxel GI target (the
##   references).
## - `compute_bins`, `compute_bins_mv`, `compute_bins_spec`, `compute_bins_spec_mv`, `compute_bins_vgi`:
##   Compute Bins in the same five forms.
##   Every material with a compute form shades through its bin's dispatch (Stage A and Stage B), and the
##   masked material, which the compute form rejects, through the tile quads in the same frame. Instance
##   IDs and the voxel GI pair must equal the reference exactly; normal-roughness, the MegaLights inputs,
##   colour, velocity and specular within the bounds below, and every pixel without a NextGeo surface (the
##   stages' clears) exactly. The depth prepass clears Stage A's targets every frame, so a target the
##   compute form left unwritten would show its clear value.
## - `no_storage`: Compute Bins with rendering/nextgeo/debug_shading_bins_no_storage, the path of a device
##   whose target formats lack storage support: both stages keep the tile quads (NO_STORAGE) and the
##   capture equals `tile_quads` exactly.
## - `tile_quads_again`: the reference once more, which must equal the first.
## With GODOTNEXT_MEGALIGHTS_INPUTS=1 in the environment, Stage A also writes the MegaLights inputs, so the
## compute form's MegaLights variants run too, and their albedo and material inputs are compared.
##
## Prints `GODOTNEXT_RESULT {...}` and exits 0 on pass, 1 on failure.

const ProbeScript = preload("res://tests/nextgeo_shading_bins/nextgeo_shading_bins_probe.gd")
const SENTINEL := -1 # 0xFFFFFFFF read as int32.
const CAPTURE_FRAME := 24
const STATS_FRAME := 28
# Bounds against the tile quads, from the exact or near-exact match measured on the L4 with headroom:
# the compute form runs the same code, but its compiler may contract or reorder float math. Every
# covered pixel's colour and specular must be within COLOR_TOLERANCE (the largest error measured on the
# L4, Vulkan and D3D12, was 0.12 of it), so a pixel a compute bin leaves unshaded fails.
const MAX_NORMAL_ROUGHNESS_LSB := 1
const MAX_NORMAL_ROUGHNESS_OVER_0_FRACTION := 0.01
const MAX_MEGALIGHTS_LSB := 1
const MAX_MEGALIGHTS_OVER_0_FRACTION := 0.01
const COLOR_TOLERANCE := 2e-3 # Relative, per channel, with an absolute floor of the same value.
const VELOCITY_TOLERANCE := 1e-5

var _phases: Array[Dictionary] = [
	{ "name": "tile_quads", "path": 2, "mv": false, "spec": false, "vgi": false, "no_storage": false },
	{ "name": "compute_bins", "path": 1, "mv": false, "spec": false, "vgi": false, "no_storage": false },
	{ "name": "tile_quads_mv", "path": 2, "mv": true, "spec": false, "vgi": false, "no_storage": false },
	{ "name": "compute_bins_mv", "path": 1, "mv": true, "spec": false, "vgi": false, "no_storage": false },
	{ "name": "tile_quads_spec", "path": 2, "mv": false, "spec": true, "vgi": false, "no_storage": false },
	{ "name": "compute_bins_spec", "path": 1, "mv": false, "spec": true, "vgi": false, "no_storage": false },
	{ "name": "tile_quads_spec_mv", "path": 2, "mv": true, "spec": true, "vgi": false, "no_storage": false },
	{ "name": "compute_bins_spec_mv", "path": 1, "mv": true, "spec": true, "vgi": false, "no_storage": false },
	{ "name": "tile_quads_vgi", "path": 2, "mv": false, "spec": false, "vgi": true, "no_storage": false },
	{ "name": "compute_bins_vgi", "path": 1, "mv": false, "spec": false, "vgi": true, "no_storage": false },
	{ "name": "no_storage", "path": 1, "mv": false, "spec": false, "vgi": false, "no_storage": true },
	{ "name": "tile_quads_again", "path": 2, "mv": false, "spec": false, "vgi": false, "no_storage": false },
]

var _probe: CompositorEffect
var _voxel_gi: VoxelGI
var _phase_idx := 0
var _phase_frame := 0
var _captures: Dictionary = {} # phase name -> capture
var _stats: Dictionary = {} # phase name -> debug stats
var _report: Dictionary = {}
var _failures: Array[String] = []
var _expect_masked := false
var _megalights_inputs := OS.get_environment("GODOTNEXT_MEGALIGHTS_INPUTS") == "1"


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	# Masked materials route through the programmable raster bins only on the hybrid raster's tier.
	ProjectSettings.set_setting("rendering/nextgeo/debug_programmable_raster", true)
	var rd := RenderingServer.get_rendering_device()
	var int64 := rd != null and (rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS) or rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS))
	_expect_masked = int64 and rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT)
	_report["device"] = {
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"adapter": RenderingServer.get_video_adapter_name(),
		"expect_masked_virtual": _expect_masked,
		"megalights_inputs": _megalights_inputs,
	}
	var vp := get_viewport()
	vp.use_taa = false
	vp.msaa_3d = Viewport.MSAA_DISABLED
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 0.25
	_build_scene()
	_apply_phase()


func _virtualize(p_mesh: PrimitiveMesh, p_ngm_path: String) -> Mesh:
	var importer := ImporterMesh.from_mesh(p_mesh)
	var err := importer.build_virtual_geometry(p_ngm_path)
	var mesh := importer.get_mesh()
	if err != OK or mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("build_virtual_geometry(%s) failed (err=%d)" % [p_ngm_path, err])
	return mesh


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = Sky.new()
	env.sky.sky_material = ProceduralSkyMaterial.new()
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world := WorldEnvironment.new()
	world.environment = env
	add_child(world)

	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.6, 4.2)
	cam.fov = 60.0
	add_child(cam)
	cam.look_at(Vector3(0.0, 0.0, 0.0))
	cam.make_current()

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	sun.shadow_enabled = true
	add_child(sun)
	var omni := OmniLight3D.new()
	omni.position = Vector3(-1.2, 1.4, 1.6)
	omni.omni_range = 6.0
	omni.light_color = Color(1.0, 0.75, 0.5)
	omni.light_energy = 2.0
	omni.shadow_enabled = true
	add_child(omni)

	var sphere := SphereMesh.new()
	sphere.radial_segments = 96
	sphere.rings = 48
	var sphere_material := StandardMaterial3D.new()
	sphere_material.albedo_color = Color(0.3, 0.7, 0.4)
	sphere_material.roughness = 0.35
	sphere.material = sphere_material
	var box := BoxMesh.new()
	box.size = Vector3(1.4, 1.4, 1.4)
	box.subdivide_width = 8
	box.subdivide_height = 8
	box.subdivide_depth = 8
	# Textured and normal-mapped, so the material set's textures and the analytic textureGrad are compared too.
	var box_material := StandardMaterial3D.new()
	box_material.albedo_color = Color(0.75, 0.45, 0.3)
	box_material.albedo_texture = _checker_texture()
	box_material.normal_enabled = true
	box_material.normal_texture = _bump_normal_texture()
	box_material.uv1_scale = Vector3(3.0, 3.0, 3.0)
	box_material.metallic = 0.6
	box_material.roughness = 0.5
	box.material = box_material
	var sphere_b := SphereMesh.new()
	sphere_b.radial_segments = 64
	sphere_b.rings = 32
	var sphere_b_material := StandardMaterial3D.new()
	sphere_b_material.albedo_color = Color(0.35, 0.4, 0.8)
	sphere_b_material.rim_enabled = true
	sphere_b.material = sphere_b_material

	_add_instance("Sphere", _virtualize(sphere, "user://nextgeo_shading_bins_sphere.ngm"), Transform3D(Basis(), Vector3(-0.55, 0.0, 0.0)))
	_add_instance("Box", _virtualize(box, "user://nextgeo_shading_bins_box.ngm"), Transform3D(Basis(Vector3.UP, deg_to_rad(30.0)), Vector3(0.6, -0.1, -0.6)))
	_add_instance("SphereB", _virtualize(sphere_b, "user://nextgeo_shading_bins_sphere_b.ngm"), Transform3D(Basis.from_scale(Vector3.ONE * 0.6), Vector3(0.2, 0.9, 0.6)))

	# Masked: the compute form rejects it, so it keeps the tile quads in the same frame. Its material is
	# assigned to the instance, after the mesh is virtualized.
	var plane := PlaneMesh.new()
	plane.size = Vector2(4.0, 4.0)
	plane.subdivide_width = 48
	plane.subdivide_depth = 48
	var plane_material := StandardMaterial3D.new()
	plane_material.albedo_color = Color(0.8, 0.8, 0.75)
	plane_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	plane_material.alpha_scissor_threshold = 0.5
	var floor_instance := _add_instance("MaskedFloor", _virtualize(plane, "user://nextgeo_shading_bins_plane.ngm"), Transform3D(Basis(), Vector3(0.0, -1.0, 0.0)))
	floor_instance.material_override = plane_material

	# Shown in the `vgi` phases only: a VoxelGI in view (baked here, as one with no data is not drawn) turns
	# on Stage A's voxel GI target.
	_voxel_gi = VoxelGI.new()
	_voxel_gi.size = Vector3(8.0, 8.0, 8.0)
	_voxel_gi.subdiv = VoxelGI.SUBDIV_64
	add_child(_voxel_gi)
	_voxel_gi.bake(self)
	if _voxel_gi.data == null:
		_failures.append("VoxelGI bake made no data")
	_voxel_gi.visible = false

	_probe = ProbeScript.new()
	var compositor := Compositor.new()
	compositor.compositor_effects = [_probe]
	cam.compositor = compositor


# A 256x256 checker with mipmaps, made on the CPU so it is ready before the first frame.
func _checker_texture() -> ImageTexture:
	var image := Image.create(256, 256, false, Image.FORMAT_RGBA8)
	for y in 256:
		for x in 256:
			var odd := ((x >> 5) + (y >> 5)) % 2 == 1
			var v := 0.35 + 0.65 * float((x * 7 + y * 13) % 64) / 63.0
			image.set_pixel(x, y, Color(v, v * 0.8, v * 0.6) if odd else Color(0.2, 0.25, 0.3))
	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)


# A tangent-space normal map of round bumps, with mipmaps.
func _bump_normal_texture() -> ImageTexture:
	var image := Image.create(128, 128, false, Image.FORMAT_RGBA8)
	for y in 128:
		for x in 128:
			var d := Vector2(float(x % 32) - 15.5, float(y % 32) - 15.5) / 16.0
			var n := Vector3(d.x * 0.7, d.y * 0.7, 1.0).normalized() if d.length() < 1.0 else Vector3(0.0, 0.0, 1.0)
			image.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))
	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)


func _add_instance(p_name: String, p_mesh: Mesh, p_transform: Transform3D) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.name = p_name
	mi.mesh = p_mesh
	mi.transform = p_transform
	add_child(mi)
	return mi


func _apply_phase() -> void:
	var phase: Dictionary = _phases[_phase_idx]
	ProjectSettings.set_setting("rendering/nextgeo/shading_path", int(phase["path"]))
	ProjectSettings.set_setting("rendering/nextgeo/debug_shading_bins_no_storage", bool(phase["no_storage"]))
	_probe.set("needs_motion_vectors", bool(phase["mv"]))
	_probe.set("needs_separate_specular", bool(phase["spec"]))
	_probe.set("read_megalights", _megalights_inputs)
	_probe.set("read_voxel_gi", bool(phase["vgi"]))
	_voxel_gi.visible = bool(phase["vgi"])
	_phase_frame = 0


func _process(_delta: float) -> void:
	var phase: Dictionary = _phases[_phase_idx]
	var phase_name: String = phase["name"]
	# The capture armed on the previous frame was taken while rendering that frame.
	if _phase_frame == CAPTURE_FRAME + 1:
		var capture: Dictionary = _probe.get("capture")
		if capture.is_empty():
			_failures.append("%s: no capture at frame %d" % [phase_name, CAPTURE_FRAME])
		_captures[phase_name] = capture
		_probe.set("capture", {})
	if _phase_frame == CAPTURE_FRAME:
		_probe.set("capture_armed", true)
	if _phase_frame == STATS_FRAME:
		_stats[phase_name] = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		_phase_idx += 1
		if _phase_idx >= _phases.size():
			_finish()
			return
		_apply_phase()
		return
	_phase_frame += 1


func _half_ulps(p_a: float, p_b: float) -> float:
	return absf(p_a - p_b) / maxf(COLOR_TOLERANCE, maxf(absf(p_a), absf(p_b)) * COLOR_TOLERANCE)


# Per-pixel comparison of a test capture against its reference.
func _compare(p_reference_name: String, p_test_name: String) -> Dictionary:
	var result := { "reference": p_reference_name, "test": p_test_name }
	var reference: Dictionary = _captures.get(p_reference_name, {})
	var test: Dictionary = _captures.get(p_test_name, {})
	if reference.is_empty() or test.is_empty() or reference["size"] != test["size"]:
		result["error"] = "missing or mismatched captures"
		return result
	var a: PackedInt32Array = reference["words"]
	var b: PackedInt32Array = test["words"]
	var af := a.to_byte_array().to_float32_array()
	var bf := b.to_byte_array().to_float32_array()
	var size: Vector2i = reference["size"]
	var pixels := size.x * size.y
	if a.size() != pixels * ProbeScript.WORDS_PER_PIXEL or b.size() != a.size():
		result["error"] = "capture sizes do not match the target"
		return result
	# The reference's values where no NextGeo surface is (the depth prepass's clears of Stage A's targets).
	var clear_ml_albedo := 0
	var clear_voxel_gi := 0
	for i in pixels:
		if a[i * ProbeScript.WORDS_PER_PIXEL] == SENTINEL:
			clear_ml_albedo = a[i * ProbeScript.WORDS_PER_PIXEL + ProbeScript.WORD_ML_ALBEDO]
			clear_voxel_gi = a[i * ProbeScript.WORDS_PER_PIXEL + ProbeScript.WORD_VOXEL_GI]
			break
	var covered := 0
	var uncovered_differing := 0
	var ml_max := 0
	var ml_over_0 := 0
	var ml_written := 0
	var voxel_gi_mismatch := 0
	var voxel_gi_written := 0
	var id_mismatch := 0
	var identical := 0
	var nr_max := 0
	var nr_over_0 := 0
	var color_max := 0.0
	var color_over := 0
	var velocity_max := 0.0
	var specular_max := 0.0
	var specular_over := 0
	for i in pixels:
		var base := i * ProbeScript.WORDS_PER_PIXEL
		if a[base] != b[base]:
			id_mismatch += 1
			continue
		if a[base] == SENTINEL:
			# No NextGeo surface: the stages' clears and the legacy passes, the same on either path.
			for w in range(1, ProbeScript.WORDS_PER_PIXEL):
				if a[base + w] != b[base + w]:
					uncovered_differing += 1
					break
			continue
		covered += 1
		ml_written += 1 if a[base + ProbeScript.WORD_ML_ALBEDO] != clear_ml_albedo else 0
		voxel_gi_written += 1 if a[base + ProbeScript.WORD_VOXEL_GI] != clear_voxel_gi else 0
		var same := true
		for w in range(1, ProbeScript.WORDS_PER_PIXEL):
			if a[base + w] != b[base + w]:
				same = false
				break
		if same:
			identical += 1
			continue
		var nr_lsb := 0
		for c in 4:
			nr_lsb = maxi(nr_lsb, absi(((a[base + 5] >> (8 * c)) & 0xFF) - ((b[base + 5] >> (8 * c)) & 0xFF)))
		nr_max = maxi(nr_max, nr_lsb)
		nr_over_0 += 1 if nr_lsb > 0 else 0
		var ml_lsb := 0
		for w: int in [ProbeScript.WORD_ML_ALBEDO, ProbeScript.WORD_ML_MATERIAL]:
			for c in 4:
				ml_lsb = maxi(ml_lsb, absi(((a[base + w] >> (8 * c)) & 0xFF) - ((b[base + w] >> (8 * c)) & 0xFF)))
		ml_max = maxi(ml_max, ml_lsb)
		ml_over_0 += 1 if ml_lsb > 0 else 0
		voxel_gi_mismatch += 1 if a[base + ProbeScript.WORD_VOXEL_GI] != b[base + ProbeScript.WORD_VOXEL_GI] else 0
		var color_error := 0.0
		for c in 4:
			color_error = maxf(color_error, _half_ulps(af[base + 1 + c], bf[base + 1 + c]))
		color_max = maxf(color_max, color_error)
		color_over += 1 if color_error > 1.0 else 0
		velocity_max = maxf(velocity_max, maxf(absf(af[base + 6] - bf[base + 6]), absf(af[base + 7] - bf[base + 7])))
		var specular_error := 0.0
		for c in 4:
			specular_error = maxf(specular_error, _half_ulps(af[base + 8 + c], bf[base + 8 + c]))
		specular_max = maxf(specular_max, specular_error)
		specular_over += 1 if specular_error > 1.0 else 0
	result["pixels"] = pixels
	result["covered"] = covered
	result["uncovered_differing"] = uncovered_differing
	result["id_mismatch"] = id_mismatch
	result["identical"] = identical
	result["normal_roughness_max_lsb"] = nr_max
	result["normal_roughness_differing"] = nr_over_0
	result["color_max_error"] = snappedf(color_max, 1e-4) # In units of COLOR_TOLERANCE.
	result["color_over_tolerance"] = color_over
	result["velocity_max_error"] = velocity_max
	result["specular_max_error"] = snappedf(specular_max, 1e-4)
	result["specular_over_tolerance"] = specular_over
	result["megalights_max_lsb"] = ml_max
	result["megalights_differing"] = ml_over_0
	result["megalights_written"] = ml_written
	result["voxel_gi_mismatch"] = voxel_gi_mismatch
	result["voxel_gi_written"] = voxel_gi_written
	result["velocity_captured"] = bool(reference["velocity"]) and bool(test["velocity"])
	result["specular_captured"] = bool(reference["specular"]) and bool(test["specular"])
	result["megalights_captured"] = bool(reference["megalights"]) and bool(test["megalights"])
	result["voxel_gi_captured"] = bool(reference["voxel_gi"]) and bool(test["voxel_gi"])
	return result


func _check_identical(p_result: Dictionary) -> void:
	var label := "%s vs %s" % [p_result["test"], p_result["reference"]]
	if p_result.has("error"):
		_failures.append("%s: %s" % [label, p_result["error"]])
		return
	if int(p_result["id_mismatch"]) != 0 or int(p_result["identical"]) != int(p_result["covered"]) or int(p_result["uncovered_differing"]) != 0:
		_failures.append("%s: expected identical captures (id mismatch %d, %d of %d covered pixels identical, %d uncovered pixels differ)" % [label, p_result["id_mismatch"], p_result["identical"], p_result["covered"], p_result["uncovered_differing"]])


func _check_close(p_result: Dictionary, p_voxel_gi: bool) -> void:
	var label := "%s vs %s" % [p_result["test"], p_result["reference"]]
	if p_result.has("error"):
		_failures.append("%s: %s" % [label, p_result["error"]])
		return
	var covered := maxi(int(p_result["covered"]), 1)
	if int(p_result["covered"]) < 1000:
		_failures.append("%s: only %d NextGeo pixels covered" % [label, p_result["covered"]])
	if int(p_result["id_mismatch"]) != 0:
		_failures.append("%s: %d instance-ID mismatches" % [label, p_result["id_mismatch"]])
	if int(p_result["uncovered_differing"]) != 0:
		_failures.append("%s: %d pixels without a NextGeo surface differ" % [label, p_result["uncovered_differing"]])
	if int(p_result["normal_roughness_max_lsb"]) > MAX_NORMAL_ROUGHNESS_LSB or float(p_result["normal_roughness_differing"]) / covered > MAX_NORMAL_ROUGHNESS_OVER_0_FRACTION:
		_failures.append("%s: normal-roughness off by up to %d LSB on %d pixels" % [label, p_result["normal_roughness_max_lsb"], p_result["normal_roughness_differing"]])
	if int(p_result["color_over_tolerance"]) != 0:
		_failures.append("%s: %d pixels' colour off by more than %s" % [label, p_result["color_over_tolerance"], str(COLOR_TOLERANCE)])
	if int(p_result["specular_over_tolerance"]) != 0:
		_failures.append("%s: %d pixels' specular off by more than %s" % [label, p_result["specular_over_tolerance"], str(COLOR_TOLERANCE)])
	if _megalights_inputs:
		# Stage A's MegaLights albedo and material inputs, written by the compute form's variants 2 and 3.
		if not bool(p_result["megalights_captured"]):
			_failures.append("%s: MegaLights inputs not captured" % label)
		elif int(p_result["megalights_written"]) * 2 <= covered:
			_failures.append("%s: the reference wrote the MegaLights albedo on only %d of %d covered pixels" % [label, p_result["megalights_written"], covered])
		if int(p_result["megalights_max_lsb"]) > MAX_MEGALIGHTS_LSB or float(p_result["megalights_differing"]) / covered > MAX_MEGALIGHTS_OVER_0_FRACTION:
			_failures.append("%s: MegaLights inputs off by up to %d LSB on %d pixels" % [label, p_result["megalights_max_lsb"], p_result["megalights_differing"]])
	if p_voxel_gi:
		if not bool(p_result["voxel_gi_captured"]):
			_failures.append("%s: voxel GI target not captured" % label)
		elif int(p_result["voxel_gi_written"]) * 2 <= covered:
			_failures.append("%s: the reference wrote the voxel GI pair on only %d of %d covered pixels" % [label, p_result["voxel_gi_written"], covered])
		if int(p_result["voxel_gi_mismatch"]) != 0:
			_failures.append("%s: %d pixels' voxel GI pair differs" % [label, p_result["voxel_gi_mismatch"]])
	if float(p_result["velocity_max_error"]) > VELOCITY_TOLERANCE:
		_failures.append("%s: velocity off by %s" % [label, str(p_result["velocity_max_error"])])


func _stat(p_phase: String, p_key: String) -> Variant:
	var s: Dictionary = _stats.get(p_phase, {})
	return s.get(p_key)


func _check_stats() -> void:
	for phase in _phases:
		var phase_name: String = phase["name"]
		var compute := int(phase["path"]) == 1
		var buckets := int(_stat(phase_name, "resolve_active_buckets"))
		var rejected := int(_stat(phase_name, "shading_rejected_buckets"))
		var stage_a := int(_stat(phase_name, "shading_stage_a_compute"))
		var stage_b := int(_stat(phase_name, "shading_stage_b_compute"))
		var quads_a := int(_stat(phase_name, "shading_stage_a_tile_quads"))
		var quads_b := int(_stat(phase_name, "shading_stage_b_tile_quads"))
		var reason_a := str(_stat(phase_name, "shading_stage_a_reason"))
		var reason_b := str(_stat(phase_name, "shading_stage_b_reason"))
		var variant_a := int(_stat(phase_name, "shading_stage_a_variant"))
		var variant_b := int(_stat(phase_name, "shading_stage_b_variant"))
		# Three materials with a compute form, plus the masked floor where it routes virtual.
		var expected_buckets := 4 if _expect_masked else 3
		if buckets != expected_buckets:
			_failures.append("%s: %d buckets, expected %d" % [phase_name, buckets, expected_buckets])
		if rejected != (1 if _expect_masked else 0):
			_failures.append("%s: %d buckets without a compute form, expected %d" % [phase_name, rejected, 1 if _expect_masked else 0])
		if not compute:
			if stage_a != 0 or stage_b != 0 or quads_a != buckets or quads_b != buckets or reason_a != "SETTING" or reason_b != "SETTING":
				_failures.append("%s: tile quads expected (compute %d/%d, quads %d/%d, reasons %s/%s)" % [phase_name, stage_a, stage_b, quads_a, quads_b, reason_a, reason_b])
		elif bool(phase["no_storage"]):
			if stage_a != 0 or stage_b != 0 or quads_a != buckets or quads_b != buckets or reason_a != "NO_STORAGE" or reason_b != "NO_STORAGE":
				_failures.append("%s: storage denied, tile quads expected (compute %d/%d, quads %d/%d, reasons %s/%s)" % [phase_name, stage_a, stage_b, quads_a, quads_b, reason_a, reason_b])
		else:
			# Every material with a compute form in its bin, the rest in tile quads, in the same frame.
			if not bool(_stat(phase_name, "shading_binned")) or stage_a != buckets - rejected or stage_b != buckets - rejected or quads_a != rejected or quads_b != rejected or reason_a != "NONE" or reason_b != "NONE":
				_failures.append("%s: compute bins expected (binned %s, compute %d/%d, quads %d/%d, reasons %s/%s)" % [phase_name, str(_stat(phase_name, "shading_binned")), stage_a, stage_b, quads_a, quads_b, reason_a, reason_b])
			# The compute form's variants (SceneShaderForwardClustered::ResolveComputeVariant): Stage A's guides
			# (+1 voxel GI, +2 MegaLights inputs), Stage B's colour at 4 (+1 separate specular, +2 motion vectors).
			var expected_b := 4 + (1 if bool(phase["spec"]) else 0) + (2 if bool(phase["mv"]) else 0)
			var expected_a := (1 if bool(phase["vgi"]) else 0) + (2 if _megalights_inputs else 0)
			if variant_a != expected_a or variant_b != expected_b:
				_failures.append("%s: compute variants %d/%d, expected %d/%d" % [phase_name, variant_a, variant_b, expected_a, expected_b])
		if (not compute or bool(phase["no_storage"])) and (variant_a != -1 or variant_b != -1):
			_failures.append("%s: tile quads expected, compute variants %d/%d" % [phase_name, variant_a, variant_b])


func _finish() -> void:
	_check_stats()
	var comparisons := []
	for pair in [["tile_quads", "compute_bins"], ["tile_quads_mv", "compute_bins_mv"], ["tile_quads_spec", "compute_bins_spec"], ["tile_quads_spec_mv", "compute_bins_spec_mv"], ["tile_quads_vgi", "compute_bins_vgi"]]:
		var c := _compare(pair[0], pair[1])
		_check_close(c, pair[1] == "compute_bins_vgi")
		comparisons.append(c)
	var no_storage := _compare("tile_quads", "no_storage")
	_check_identical(no_storage)
	var again := _compare("tile_quads", "tile_quads_again")
	_check_identical(again)
	if comparisons.size() == 5:
		if not bool(comparisons[1].get("velocity_captured", false)):
			_failures.append("compute_bins_mv: no velocity captured")
		if not bool(comparisons[2].get("specular_captured", false)):
			_failures.append("compute_bins_spec: no separate specular captured")
		if not bool(comparisons[3].get("velocity_captured", false)) or not bool(comparisons[3].get("specular_captured", false)):
			_failures.append("compute_bins_spec_mv: velocity and separate specular not both captured")
	var stats_report := {}
	for phase_name in _stats:
		var s: Dictionary = _stats[phase_name]
		var entry := {}
		for key in ["raster_mode", "resolve_active_buckets", "resolve_stage_a_draws", "resolve_stage_b_draws", "shading_path", "shading_binned", "shading_rejected_buckets", "shading_stage_a_compute", "shading_stage_b_compute", "shading_stage_a_tile_quads", "shading_stage_b_tile_quads", "shading_stage_a_reason", "shading_stage_b_reason", "shading_stage_a_variant", "shading_stage_b_variant"]:
			entry[key] = s.get(key)
		stats_report[phase_name] = entry
	_report["stats"] = stats_report
	_report["compute_vs_tile_quads"] = comparisons
	_report["no_storage_vs_tile_quads"] = no_storage
	_report["tile_quads_again"] = again
	_report["failures"] = _failures
	_report["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_report))
	get_tree().quit(0 if _failures.is_empty() else 1)
