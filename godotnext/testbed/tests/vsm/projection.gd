extends Node3D

## Virtual Shadow Map projection and SMRT soft shadows in the Forward+ light loop (#222; plan §21.3
## contract S6).
##
## A 1 m x 1 m slab 1 m above a white floor, lit from the side so that its shadow falls next to it, seen
## straight down by an orthographic camera (4 m across, 640 x 640 pixels, HDR, linear tone mapping, no
## ambient light). Each case turns one light on (a sun, a spot, an omni or an area light) with a size,
## renders the floor once with the slab casting no shadow (the reference: the light without the caster)
## and once with it casting, and divides the two: s = 1 lit, 0 shadowed. Each capture gets a new light
## node, so its virtual maps are new and drawn with the casters of that capture: VSM keeps a static
## caster's cached pages until #212's per-primitive invalidation, so a caster that starts casting under
## an existing light's maps would not reach them yet.
## - Hard cases (size 0): the pixels with s < 0.5 must match the slab's analytic shadow outside a band of
##   EDGE_BAND around its edges, for 99.5% of the floor the light reaches (in the atlas run the band is at
##   least half the width of the atlas's own filtered edge). The reference image is the negative control:
##   without the caster the shadow's region must be as bright as the floor around it.
## - Soft cases: the penumbra's width across the shadow's far edge (s from 0.05 to 0.95, averaged over
##   the rows of the shadow's middle) must grow with the light's size: sun 0.5 and 4 degrees, spot and
##   omni radius 0.02 and 0.08 m, and area light squares of 0.1 and 0.3 m. The sizes keep every ray of
##   the far edge within the 32-texel march cap, so the cap does not clamp the widths.
## With VSM on (--shadows=vsm) the debug info must show that the view sampled the lights' virtual maps and
## ran no shadow-atlas pass in the captured frame (projection.atlas_passes = 0, lights_sampled > 0). A new
## light still gets one atlas pass in its first frame, before it holds maps, and the atlas keeps that map,
## so two checks show that the light loop read the page pool and not that map: a hard shadow's edge is at
## most 0.05 m wide (the atlas's filtered edge is wider for the spot and omni lights), and no penumbra is
## wider than the light's geometric penumbra (the atlas's PCSS is far wider for the 4 degree sun). With
## --reference=<dir> (the --shadows=atlas run's --out folder) every hard case's shadow mask must match the
## atlas path's on the same scene for 99.5% of the floor outside a band of half the atlas edge's width
## (at least EDGE_BAND).
## With --prefiltered=true, Prefiltered Distant is on (the sun's shadow lies in the last two of its three
## clipmap levels): the hard sun case must still match its analytic shadow.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.
## User arguments: --shadows=atlas|vsm (default: vsm when GODOT_DEV_VSM is set), --out=<dir> (saves each
## case's shadow factor as <case>.exr), --reference=<dir>, --prefiltered=true|false.

const SceneArgs = preload("res://scenes/scene_args.gd")

const SIZE := 640
const EXTENT := 4.0
const SETTLE_FRAMES := 12
const SLAB_CENTER := Vector3(-1.0, 1.0, 0.0)
const SLAB_HALF := 0.5
const SLAB_THICKNESS := 0.01
# The slab's top, whose edges cast the shadow's outline.
const SLAB_TOP := SLAB_CENTER.y + SLAB_THICKNESS * 0.5
const EDGE_BAND := 0.03
const MASK_AGREEMENT := 0.995
# Point lights sit here; the slab's shadow on the floor is then about x in [-1, 1], |z| <= 1 (a point p
# of the slab's top projects to LIGHT_POSITION + k (p - LIGHT_POSITION), k = LIGHT_POSITION.y /
# (LIGHT_POSITION.y - SLAB_TOP)).
const LIGHT_POSITION := Vector3(-2.0, 2.0, 0.0)
const LIGHT_TARGET := Vector3(0.5, 0.0, 0.0)
const CASES: Array[Dictionary] = [
	{"name": "sun_hard", "light": "sun", "size": 0.0},
	{"name": "sun_0_5", "light": "sun", "size": 0.5},
	{"name": "sun_4", "light": "sun", "size": 4.0},
	{"name": "spot_hard", "light": "spot", "size": 0.0},
	{"name": "spot_0_02", "light": "spot", "size": 0.02},
	{"name": "spot_0_08", "light": "spot", "size": 0.08},
	{"name": "omni_hard", "light": "omni", "size": 0.0},
	{"name": "omni_0_02", "light": "omni", "size": 0.02},
	{"name": "omni_0_08", "light": "omni", "size": 0.08},
	{"name": "area_0_1", "light": "area", "size": 0.1},
	{"name": "area_0_3", "light": "area", "size": 0.3},
]

var _mode := "vsm"
var _out := ""
var _reference := ""
var _prefiltered := false
var _viewport: SubViewport
var _camera: Camera3D
var _slab: MeshInstance3D
var _light: Light3D
var _world_x := PackedFloat32Array()
var _world_z := PackedFloat32Array()
var _failures := PackedStringArray()
var _result := {}


func _ready() -> void:
	_mode = SceneArgs.value("--shadows", "vsm" if OS.get_environment("GODOT_DEV_VSM") != "" else "atlas")
	_out = SceneArgs.value("--out", "")
	_reference = SceneArgs.value("--reference", "")
	_prefiltered = SceneArgs.value("--prefiltered", "false") == "true"
	_result["shadows"] = _mode
	_result["prefiltered"] = _prefiltered
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	# The most rays and steps: the widths are measured on a smooth penumbra.
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_local", 32)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_rays_directional", 32)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/smrt_steps", 16)
	ProjectSettings.set_setting("rendering/lights_and_shadows/virtual_shadows/prefiltered_distant", _prefiltered)
	_build_scene()
	_run.call_deferred()


func _build_scene() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_energy = 0.0
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = environment
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
	_camera.far = 10.0
	_camera.transform = Transform3D(Basis.looking_at(Vector3.DOWN, Vector3.FORWARD), Vector3(0.0, 4.0, 0.0))
	_viewport.add_child(_camera)
	_camera.current = true

	var white := StandardMaterial3D.new()
	white.albedo_color = Color(1.0, 1.0, 1.0)
	white.roughness = 1.0
	white.metallic = 0.0
	white.metallic_specular = 0.0
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(12.0, 12.0)
	floor_mesh.mesh = plane
	floor_mesh.material_override = white
	add_child(floor_mesh)
	_slab = MeshInstance3D.new()
	var slab_mesh := BoxMesh.new()
	slab_mesh.size = Vector3(SLAB_HALF * 2.0, SLAB_THICKNESS, SLAB_HALF * 2.0)
	_slab.mesh = slab_mesh
	_slab.material_override = white
	_slab.position = SLAB_CENTER
	add_child(_slab)


# A new light of the case's kind and size (a new light instance: new virtual maps).
func _new_light(p_case: Dictionary) -> Light3D:
	var light: Light3D
	var size := float(p_case.size)
	match String(p_case.light):
		"sun":
			# The sun shines along (1, -1, 0): the slab's shadow is its footprint moved 1 m along +x.
			var sun := DirectionalLight3D.new()
			sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
			sun.directional_shadow_max_distance = 6.0
			sun.light_angular_distance = size
			light = sun
		"spot":
			var spot := SpotLight3D.new()
			spot.spot_angle = 60.0
			spot.spot_range = 10.0
			spot.light_size = size
			light = spot
		"omni":
			var omni := OmniLight3D.new()
			omni.omni_range = 10.0
			omni.light_size = size
			light = omni
		_:
			var area := AreaLight3D.new()
			area.area_range = 10.0
			area.area_size = Vector2(size, size)
			light = area
	light.shadow_enabled = true
	add_child(light)
	if p_case.light == "sun":
		light.transform = Transform3D(Basis.looking_at(Vector3(1.0, -1.0, 0.0).normalized(), Vector3.UP), Vector3(0.0, 5.0, 0.0))
	else:
		light.light_energy = 4.0
		light.look_at_from_position(LIGHT_POSITION, LIGHT_TARGET, Vector3.UP)
	return light


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


# The viewport's red channel (the floor and lights are white), linear.
func _capture() -> PackedFloat32Array:
	var image := _viewport.get_texture().get_image()
	image.convert(Image.FORMAT_RF)
	return image.get_data().to_float32_array()


func _run() -> void:
	await _frames(4)
	_map_pixels()
	var widths := {}
	var geometric := {}
	var cases := []
	for case: Dictionary in CASES:
		var record := await _run_case(case)
		cases.append(record)
		if record.has("width"):
			widths[case.name] = record.width
		if record.has("geometric_width"):
			geometric[case.name] = record.geometric_width
	_result["cases"] = cases
	_result["widths"] = widths
	if not _prefiltered:
		_check_widths(widths, geometric)
	_finish()


# The floor point each pixel shows (x, z on y = 0).
func _map_pixels() -> void:
	_world_x.resize(SIZE * SIZE)
	_world_z.resize(SIZE * SIZE)
	for y in SIZE:
		for x in SIZE:
			var point := _camera.project_position(Vector2(x + 0.5, y + 0.5), 4.0)
			_world_x[y * SIZE + x] = point.x
			_world_z[y * SIZE + x] = point.z


## Replaces the light with a new one for the case.
func _set_light(p_case: Dictionary) -> void:
	if _light != null:
		remove_child(_light)
		_light.queue_free()
	_light = _new_light(p_case)


func _run_case(p_case: Dictionary) -> Dictionary:
	var record := {"name": p_case.name}
	_slab.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_set_light(p_case)
	await _frames(SETTLE_FRAMES)
	var reference := _capture()
	_slab.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_set_light(p_case)
	await _frames(SETTLE_FRAMES)
	var shadowed := _capture()
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var projection: Dictionary = info.get("projection", {})
	record["projection"] = projection

	var max_reference := 0.0
	for value in reference:
		max_reference = maxf(max_reference, value)
	var factor := PackedFloat32Array()
	factor.resize(SIZE * SIZE)
	var reached := PackedByteArray()
	reached.resize(SIZE * SIZE)
	for i in SIZE * SIZE:
		var lit := reference[i] > 0.05 * max_reference
		reached[i] = 1 if lit else 0
		factor[i] = clampf(shadowed[i] / reference[i], 0.0, 1.0) if lit else 1.0
	record["max_reference"] = max_reference
	if max_reference <= 0.0:
		_failures.append("%s: the light does not reach the floor" % p_case.name)
		return record

	if _mode == "vsm":
		if not bool(info.get("active", false)):
			_failures.append("%s: VSM is not active" % p_case.name)
		if int(projection.get("lights_sampled", 0)) < 1:
			_failures.append("%s: the view sampled no light's virtual shadow maps" % p_case.name)
		if int(projection.get("atlas_passes", -1)) != 0:
			_failures.append("%s: %d shadow-atlas passes ran for a light the view samples through VSM" % [p_case.name, int(projection.get("atlas_passes", -1))])
		if int(projection.get("rays_local", 0)) != 32 or int(projection.get("rays_directional", 0)) != 32 or int(projection.get("steps", 0)) != 16:
			_failures.append("%s: the SMRT settings did not reach the light loop: %s" % [p_case.name, str(projection)])
		if bool(projection.get("prefiltered_distant", false)) != _prefiltered:
			_failures.append("%s: Prefiltered Distant is %s in the light loop" % [p_case.name, str(projection.get("prefiltered_distant", false))])
		if p_case.light == "sun" and int(projection.get("atlas_passes_skipped", 0)) < 1:
			_failures.append("%s: the sun's atlas pass was not skipped" % p_case.name)
	elif bool(info.get("active", false)):
		_failures.append("%s: VSM is active in the atlas run" % p_case.name)

	if _out != "":
		DirAccess.make_dir_recursive_absolute(_out)
		var image := Image.create_from_data(SIZE, SIZE, false, Image.FORMAT_RF, factor.to_byte_array())
		image.save_exr(_out.path_join("%s.exr" % p_case.name), true)

	var profile := _penumbra_profile(p_case, factor)
	record["width"] = profile.width
	record["inner_width"] = profile.inner_width
	record["outer_width"] = profile.outer_width
	record["edge_factor"] = profile.edge_factor
	if float(p_case.size) == 0.0:
		_check_mask(p_case, record, factor, reached, reference)
	else:
		_check_soft_center(p_case, record, factor)
		record["geometric_width"] = _geometric_width(p_case)
		if _mode == "vsm" and not _prefiltered and p_case.name in ["sun_4", "spot_0_08", "omni_0_08", "area_0_3"]:
			if float(profile.edge_factor) < 0.35 or float(profile.edge_factor) > 0.65:
				_failures.append("%s: penumbra is asymmetric at the geometric edge (edge_factor=%.3f, want 0.35..0.65)" % [p_case.name, float(profile.edge_factor)])
			if float(profile.inner_width) <= float(profile.width) * 0.25:
				_failures.append("%s: inner penumbra is missing or collapsed (inner=%.4f m of %.4f m)" % [p_case.name, float(profile.inner_width), float(profile.width)])
	return record


# Whether the floor point (x, z) is in the slab's shadow for the case's light, and its distance to
# the shadow's edge.
func _expected(p_case: Dictionary, p_x: float, p_z: float) -> Array:
	# The sun shines along (1, -1, 0): the top's outline moves SLAB_TOP along +x.
	var half := Vector2(SLAB_HALF, SLAB_HALF)
	var center := Vector2(SLAB_CENTER.x + SLAB_TOP, SLAB_CENTER.z)
	if p_case.light != "sun":
		var k := _point_scale()
		half = Vector2(SLAB_HALF, SLAB_HALF) * k
		center = Vector2(LIGHT_POSITION.x + k * (SLAB_CENTER.x - LIGHT_POSITION.x), LIGHT_POSITION.z + k * (SLAB_CENTER.z - LIGHT_POSITION.z))
	var dx := absf(p_x - center.x) - half.x
	var dz := absf(p_z - center.y) - half.y
	var inside := dx <= 0.0 and dz <= 0.0
	var edge := maxf(dx, dz) if inside else Vector2(maxf(dx, 0.0), maxf(dz, 0.0)).length()
	return [inside, absf(edge)]


func _point_scale() -> float:
	return LIGHT_POSITION.y / (LIGHT_POSITION.y - SLAB_TOP)


# Pixels that show the slab's top, not the floor.
func _on_slab(p_x: float, p_z: float) -> bool:
	return absf(p_x - SLAB_CENTER.x) <= SLAB_HALF + EDGE_BAND and absf(p_z - SLAB_CENTER.z) <= SLAB_HALF + EDGE_BAND


func _check_mask(p_case: Dictionary, r_record: Dictionary, p_factor: PackedFloat32Array, p_reached: PackedByteArray, p_reference: PackedFloat32Array) -> void:
	var counted := 0
	var agree := 0
	var shadow_pixels := 0
	var expected_shadow := 0
	var inside_reference := 0.0
	var inside_count := 0
	var ring_reference := 0.0
	var ring_count := 0
	var reference_factor := PackedFloat32Array()
	if _reference != "" and _mode == "vsm":
		var image := Image.load_from_file(_reference.path_join("%s.exr" % p_case.name))
		if image == null or image.get_width() != SIZE:
			_failures.append("%s: no atlas reference in %s" % [p_case.name, _reference])
		else:
			image.convert(Image.FORMAT_RF)
			reference_factor = image.get_data().to_float32_array()
	# The atlas filters its hard edges: where they are compared, the band is at least half the atlas
	# edge's width across the far edge (its 50% contour may lie that far from the analytic edge).
	var pixel := EXTENT / float(SIZE)
	var band := EDGE_BAND
	if _mode == "atlas":
		band = maxf(EDGE_BAND, float(r_record.get("width", 0.0)) * 0.5 + pixel)
	var atlas_band := EDGE_BAND
	if not reference_factor.is_empty():
		atlas_band = maxf(EDGE_BAND, _penumbra_width(p_case, reference_factor) * 0.5 + pixel)
		r_record["atlas_band"] = atlas_band
	r_record["band"] = band
	var atlas_counted := 0
	var atlas_agree := 0
	for i in SIZE * SIZE:
		var x := _world_x[i]
		var z := _world_z[i]
		if _on_slab(x, z):
			continue
		var expected: Array = _expected(p_case, x, z)
		# The negative control: without the caster the shadow's region is lit like the floor around it.
		if p_reached[i] == 1 and bool(expected[0]) and float(expected[1]) > 0.1:
			inside_reference += p_reference[i]
			inside_count += 1
		elif p_reached[i] == 1 and not bool(expected[0]) and float(expected[1]) > 0.1 and float(expected[1]) < 0.3:
			ring_reference += p_reference[i]
			ring_count += 1
		if p_reached[i] == 0:
			continue
		var distance := float(expected[1])
		var shadowed := p_factor[i] < 0.5
		if distance >= band:
			counted += 1
			agree += 1 if shadowed == bool(expected[0]) else 0
			shadow_pixels += 1 if shadowed else 0
			expected_shadow += 1 if bool(expected[0]) else 0
		if not reference_factor.is_empty() and distance >= atlas_band:
			atlas_counted += 1
			atlas_agree += 1 if shadowed == (reference_factor[i] < 0.5) else 0
	var agreement := float(agree) / maxf(float(counted), 1.0)
	r_record["mask_pixels"] = counted
	r_record["mask_agreement"] = agreement
	r_record["shadow_pixels"] = shadow_pixels
	r_record["expected_shadow_pixels"] = expected_shadow
	if counted < SIZE * SIZE / 4 or expected_shadow < 1000:
		_failures.append("%s: too few floor pixels to compare (%d, %d in the shadow)" % [p_case.name, counted, expected_shadow])
	if agreement < MASK_AGREEMENT:
		_failures.append("%s: the shadow matches the slab's for %.4f of the floor (need %.3f; %d shadowed, %d expected)" % [p_case.name, agreement, MASK_AGREEMENT, shadow_pixels, expected_shadow])
	var inside_mean := inside_reference / maxf(float(inside_count), 1.0)
	var ring_mean := ring_reference / maxf(float(ring_count), 1.0)
	r_record["reference_inside_over_ring"] = inside_mean / maxf(ring_mean, 1e-6)
	if inside_count == 0 or ring_count == 0 or inside_mean < 0.8 * ring_mean:
		_failures.append("%s: without the caster the shadow's region is not lit like the floor around it (%.4f vs %.4f)" % [p_case.name, inside_mean, ring_mean])
	if not reference_factor.is_empty():
		var atlas_agreement := float(atlas_agree) / maxf(float(atlas_counted), 1.0)
		r_record["atlas_agreement"] = atlas_agreement
		if atlas_agreement < MASK_AGREEMENT:
			_failures.append("%s: the VSM shadow matches the atlas path's for %.4f of the floor (need %.3f)" % [p_case.name, atlas_agreement, MASK_AGREEMENT])


# A light with a size: the shadow's middle is dark and the floor far from it lit.
func _check_soft_center(p_case: Dictionary, r_record: Dictionary, p_factor: PackedFloat32Array) -> void:
	var dark := 0.0
	var dark_count := 0
	var lit := 0.0
	var lit_count := 0
	for i in SIZE * SIZE:
		var x := _world_x[i]
		var z := _world_z[i]
		if _on_slab(x, z):
			continue
		var expected: Array = _expected(p_case, x, z)
		if bool(expected[0]) and float(expected[1]) > 0.3:
			dark += p_factor[i]
			dark_count += 1
		elif not bool(expected[0]) and float(expected[1]) > 0.6:
			lit += p_factor[i]
			lit_count += 1
	r_record["center_factor"] = dark / maxf(float(dark_count), 1.0)
	r_record["far_factor"] = lit / maxf(float(lit_count), 1.0)
	if dark_count == 0 or r_record.center_factor > 0.1:
		_failures.append("%s: the shadow's middle is not dark (%.3f)" % [p_case.name, r_record.center_factor])
	if lit_count == 0 or r_record.far_factor < 0.9:
		_failures.append("%s: the floor far from the shadow is not lit (%.3f)" % [p_case.name, r_record.far_factor])


# The penumbra across the shadow's far edge (+x), averaged over the rows of the shadow's middle: from
# the first column above 0.05 to the first above 0.95, in meters, plus its inner/outer split and value at
# the analytic geometric edge. width is -1 when it never gets there.
func _penumbra_profile(p_case: Dictionary, p_factor: PackedFloat32Array) -> Dictionary:
	var edge := SLAB_CENTER.x + SLAB_HALF + SLAB_TOP
	var rows := 0.3
	if p_case.light != "sun":
		edge = LIGHT_POSITION.x + _point_scale() * (SLAB_CENTER.x + SLAB_HALF - LIGHT_POSITION.x)
		rows = 0.5
	var sums := PackedFloat32Array()
	var counts := PackedInt32Array()
	sums.resize(SIZE)
	counts.resize(SIZE)
	for y in SIZE:
		var z := _world_z[y * SIZE]
		if absf(z) > rows:
			continue
		for x in SIZE:
			sums[x] += p_factor[y * SIZE + x]
			counts[x] += 1
	var low := -1.0
	var high := -1.0
	var edge_value := -1.0
	var best_edge_dist := INF
	for x in SIZE:
		var world_x := _world_x[x]
		if counts[x] == 0:
			continue
		var value := sums[x] / float(counts[x])
		var edge_dist := absf(world_x - edge)
		if edge_dist < best_edge_dist:
			best_edge_dist = edge_dist
			edge_value = value
		if world_x < edge - 0.4:
			continue
		if low < 0.0 and value > 0.05:
			low = world_x
		if high < 0.0 and value > 0.95:
			high = world_x
	if high < 0.0:
		return {"width": -1.0, "inner_width": -1.0, "outer_width": -1.0, "edge_factor": edge_value}
	var width := 0.0 if low < 0.0 else high - low
	var inner_width := 0.0 if low < 0.0 else maxf(edge - low, 0.0)
	var outer_width := maxf(high - edge, 0.0)
	return {"width": width, "inner_width": inner_width, "outer_width": outer_width, "edge_factor": edge_value}


func _penumbra_width(p_case: Dictionary, p_factor: PackedFloat32Array) -> float:
	return float(_penumbra_profile(p_case, p_factor).width)


# The full width (0 to 100%) of the light's geometric penumbra on the floor across the shadow's far
# edge, in the plane z = 0: SMRT's rays sample the light inside it, so a measured 5-95% width beyond it
# (plus two pixels) did not come from the virtual maps. The case's light must be the current one.
func _geometric_width(p_case: Dictionary) -> float:
	var size := float(p_case.size)
	if p_case.light == "sun":
		# Directions within half the angular diameter of 45 degrees from the vertical.
		var half_angle := deg_to_rad(size) * 0.5
		return SLAB_TOP * (tan(PI * 0.25 + half_angle) - tan(PI * 0.25 - half_angle))
	var edge := Vector3(SLAB_CENTER.x + SLAB_HALF, SLAB_TOP, 0.0)
	var offsets: Array[Vector3] = []
	if p_case.light == "area":
		var basis := _light.global_transform.basis
		for u: float in [-0.5, 0.5]:
			for v: float in [-0.5, 0.5]:
				offsets.append((basis.x * u + basis.y * v) * size)
	else:
		# The disk's points that land farthest apart on the floor: across the direction to the edge, in the
		# plane z = 0.
		var to_light := (LIGHT_POSITION - edge).normalized()
		var across := Vector3(-to_light.y, to_light.x, 0.0)
		offsets.append(across * size)
		offsets.append(-across * size)
	var low := INF
	var high := -INF
	for offset: Vector3 in offsets:
		var source := LIGHT_POSITION + offset
		var x := source.x + (edge.x - source.x) * source.y / (source.y - edge.y)
		low = minf(low, x)
		high = maxf(high, x)
	return high - low


func _check_widths(p_widths: Dictionary, p_geometric: Dictionary) -> void:
	if _mode != "vsm":
		return
	var pixel := EXTENT / float(SIZE)
	for name: String in p_widths:
		if float(p_widths[name]) < 0.0:
			_failures.append("%s: the penumbra never reaches full light" % name)
	for name: String in p_geometric:
		if float(p_widths.get(name, -1.0)) > float(p_geometric[name]) + 2.0 * pixel:
			_failures.append("%s: the penumbra (%.4f m) is wider than the light's geometric penumbra (%.4f m)" % [name, float(p_widths.get(name, -1.0)), float(p_geometric[name])])
	var pairs := [["sun_0_5", "sun_4"], ["spot_0_02", "spot_0_08"], ["omni_0_02", "omni_0_08"], ["area_0_1", "area_0_3"]]
	for pair: Array in pairs:
		var narrow := float(p_widths.get(pair[0], -1.0))
		var wide := float(p_widths.get(pair[1], -1.0))
		if not (wide > 1.5 * narrow and wide >= narrow + 3.0 * pixel):
			_failures.append("the penumbra does not widen from %s (%.4f m) to %s (%.4f m)" % [pair[0], narrow, pair[1], wide])
	for hard: String in ["sun_hard", "spot_hard", "omni_hard"]:
		if float(p_widths.get(hard, 1.0)) > 0.05:
			_failures.append("%s: a hard shadow's edge is %.4f m wide" % [hard, float(p_widths.get(hard, 1.0))])


func _finish() -> void:
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
