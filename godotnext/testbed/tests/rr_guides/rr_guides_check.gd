extends Node

## #205: the DLSS Ray Reconstruction guides.
##
## The opaque color passes write the diffuse albedo and the specular albedo
## (RGBA8), a pack pass writes the world-space normal with the roughness
## (RGBA16F), and the NextGI specular trace writes the hit distance (R16F, 65504
## on a miss). The environment variable GODOT_DEV_RR_GUIDES=1 turns the guides on
## without DLSS. A CompositorEffect copies the guide textures to the CPU.
##
## --rr-check=scene (the default). Loads --scene (Color Box Room A by default)
## and looks through --camera (CaptureCam). Every --step pixels a ray is cast
## from the camera against the scene's meshes on the CPU, which gives the
## surface, its material and its normal. At those pixels:
## - the diffuse albedo is the material's albedo * (1 - metallic), to the RGBA8
##   byte, and 0.5 with alpha 0 where there is no surface;
## - the specular albedo is 0 where the material has no specular reflectance
##   (metallic 0 and specular 0) and where there is no surface;
## - the packed normal is the prepass normal of the same pixel turned to world
##   space, within --normal-tolerance (1e-3), and its distance from the mesh
##   normal is reported and kept under --geometry-normal-tolerance: the prepass
##   stores the normal in an RGBA8 texel, so the mesh normal is not within 1e-3;
## - the roughness is the material's, within the prepass's 7 bits.
## A sample where the renderer drew no opaque surface (the pack writes 0 where
## the diffuse albedo guide's alpha is 0) or another one (its normal is more than
## 0.1 from the mesh's) is a surface mismatch: the CPU reference does not apply
## there, but the guides must hold the clear values where nothing was drawn and
## the prepass normal where something was. Their share of the surface samples must stay within
## --max-surface-mismatch (0).
## With the reference path tracer in its reflections-only mode
## (debug/reference_mode=3, debug/reference_readback=true) the hit distance is
## compared with the path tracer's mean hit distance on pixels whose material
## roughness is at most --hit-max-roughness (0.4, every traced pixel) and where
## every path-traced sample and every guide frame hit: a mean over hits alone is
## biased where some rays miss. The guide is one ray per pixel and frame, so
## --hit-frames frames (64) are averaged. Per pixel, that mean must be within
## --hit-tolerance (1%) plus three of its standard errors of the reference; per
## material roughness, the summed means must be within the tolerance alone.
## --min-hit-samples is the least number of such pixels the scene must have.
## Pixels the trace skips (prepass roughness above 0.4, no surface) must hold
## 65504. At High the trace runs at half resolution, and trace pixel t samples
## view pixel 2t + 1, so the samples sit on odd pixels.
## --require-stage-b=true fails the run unless NextGeo Stage B drew virtual
## meshes, so their guides come from the Stage B variants.
## --separate-specular=true and --motion-vectors=true ask for a separate specular
## and a velocity attachment in the opaque pass (through the probe's
## CompositorEffect flags). They move the guides' clear flags
## (RRGuides::load_guides()), and the run fails if the view lacks them.
##
## --rr-check=factor. A scene built here: quads lit only by a white sky, so the
## frame color of a quad without diffuse light is the factor the forward shader
## scales the indirect specular light by. The specular albedo guide must be that
## color, the diffuse albedo guide the material's, and an unshaded quad has no
## specular albedo. Where no quad is, the guides hold their clear values. Then a
## transparent quad is added in front of the first one: it must change the frame
## and leave both guides exactly as they were. Last, a depth-prepass-alpha quad is
## added in front of the fourth one: the depth prepass draws it, the opaque passes
## do not, so all three guides must say "no surface" there (the albedo clear
## values and a zero normal), while the prepass texel is the quad's.
##
## --rr-check=off. GODOT_DEV_RR_GUIDES is not set: the view must hold no guide
## texture.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs = preload("res://scenes/scene_args.gd")

const MISS_DISTANCE := 65504.0
## A guide normal this far from the mesh normal (largest axis) is another surface.
const SURFACE_MISMATCH_NORMAL := 0.1
const SETTLE_FRAMES := 24
## The roughness above which the specular trace does not run (plan §8.5).
const MAX_TRACE_ROUGHNESS := 0.4
const REFERENCE_MODE_SETTING := "rendering/global_illumination/nextgi/debug/reference_mode"
const REFERENCE_READBACK_SETTING := "rendering/global_illumination/nextgi/debug/reference_readback"
const REFERENCE_REFLECTIONS_ONLY := 3
const REFERENCE_STALL_FRAMES := 600

var _failures := PackedStringArray()
var _result := {"test": "rr_guides"}
var _probe: GuideProbe


func _ready() -> void:
	_run()


func _run() -> void:
	var mode := SceneArgs.value("--rr-check", "scene")
	_result["mode"] = mode
	_result["requested"] = _guides_requested()
	if mode == "scene":
		await _check_scene()
	elif mode == "factor":
		await _check_factor()
	elif mode == "off":
		await _check_off()
	else:
		_failures.append("unknown --rr-check mode %s" % mode)
	_result["pass"] = _failures.is_empty()
	_result["failures"] = _failures
	var out := SceneArgs.value("--out", "")
	if out != "":
		DirAccess.make_dir_recursive_absolute(out)
		var file := FileAccess.open(out.path_join("rr_guides.json"), FileAccess.WRITE)
		if file != null:
			file.store_string(JSON.stringify(_result, "\t") + "\n")
			file.close()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)


func _guides_requested() -> bool:
	return OS.get_environment("GODOT_DEV_RR_GUIDES") == "1"


func _float_arg(p_flag: String, p_default: float) -> float:
	var text := SceneArgs.value(p_flag, "")
	return float(text) if text.is_valid_float() else p_default


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


## Asks the probe for the named textures and waits for the frame that copies them.
func _dump(p_names: PackedStringArray) -> Dictionary:
	_probe.request(p_names)
	for i in 8:
		await RenderingServer.frame_post_draw
		var taken := _probe.take()
		if not taken.is_empty():
			return taken
	return {}


func _attach_probe(p_camera: Camera3D) -> void:
	_probe = GuideProbe.new()
	_probe.needs_separate_specular = SceneArgs.value("--separate-specular", "") == "true"
	_probe.needs_motion_vectors = SceneArgs.value("--motion-vectors", "") == "true"
	var compositor := p_camera.compositor
	if compositor == null:
		compositor = Compositor.new()
		p_camera.compositor = compositor
	var effects := compositor.compositor_effects
	effects.append(_probe)
	compositor.compositor_effects = effects


func _record_allocation() -> void:
	_result["probe_calls"] = _probe.calls
	_result["guide_textures"] = _probe.guide_textures
	_result["guide_bytes"] = _probe.guide_bytes
	_result["internal_size"] = "%dx%d" % [_probe.internal_size.x, _probe.internal_size.y]
	_result["separate_specular_texture"] = _probe.has_specular
	_result["velocity_texture"] = _probe.has_velocity
	if _probe.calls == 0:
		_failures.append("the compositor effect did not see the render buffers")
	if _probe.needs_separate_specular and not _probe.has_specular:
		_failures.append("--separate-specular=true, but the view has no separate specular texture")
	if _probe.needs_motion_vectors and not _probe.has_velocity:
		_failures.append("--motion-vectors=true, but the view has no velocity texture")
	if not _probe.error.is_empty():
		_failures.append(_probe.error)


func _load_scene() -> Camera3D:
	var path := SceneArgs.value("--scene", "res://scenes/color_box_room/cbr_a.tscn")
	_result["scene"] = path
	var size_text := SceneArgs.value("--size", "640x360").split("x")
	if size_text.size() == 2:
		get_window().size = Vector2i(int(size_text[0]), int(size_text[1]))
	var packed := load(path) as PackedScene
	if packed == null:
		_failures.append("cannot load %s" % path)
		return null
	var scene := packed.instantiate()
	add_child(scene)
	var worlds := scene.find_children("*", "WorldEnvironment", true, false)
	var world := worlds[0] as WorldEnvironment if not worlds.is_empty() else null
	if world != null and world.environment != null and bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)):
		var environment: Environment = world.environment.duplicate()
		environment.nextgi_enabled = true
		environment.sdfgi_enabled = false
		environment.ssr_enabled = false
		world.environment = environment
	var camera_name := SceneArgs.value("--camera", "CaptureCam")
	_result["camera"] = camera_name
	var camera := scene.find_child(camera_name, true, false) as Camera3D
	if camera == null:
		_failures.append("%s has no Camera3D named %s" % [path, camera_name])
		return null
	camera.current = true
	return camera


func _check_off() -> void:
	if _guides_requested():
		_failures.append("--rr-check=off needs GODOT_DEV_RR_GUIDES unset")
	var camera := _load_scene()
	if camera == null:
		return
	_attach_probe(camera)
	await _frames(SETTLE_FRAMES)
	_record_allocation()
	if _probe.guide_textures != 0 or _probe.guide_bytes != 0:
		_failures.append("%d guide textures (%d bytes) exist with the guides off" % [_probe.guide_textures, _probe.guide_bytes])


func _check_scene() -> void:
	if not _guides_requested():
		_failures.append("--rr-check=scene needs GODOT_DEV_RR_GUIDES=1")
	var camera := _load_scene()
	if camera == null:
		return
	_attach_probe(camera)
	await _frames(SETTLE_FRAMES)
	_record_allocation()
	var size := _probe.internal_size
	if _probe.guide_textures != 3:
		_failures.append("%d of the 3 guide textures exist" % _probe.guide_textures)
		return
	# Two RGBA8 guides and one RGBA16F guide at the internal size.
	var expected_bytes := size.x * size.y * (4 + 4 + 8)
	_result["expected_guide_bytes"] = expected_bytes
	if _probe.guide_bytes != expected_bytes:
		_failures.append("the guides hold %d bytes, expected %d" % [_probe.guide_bytes, expected_bytes])

	# Virtual meshes write their guides in NextGeo Stage B (--require-stage-b).
	var nextgeo: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	_result["stage_b_draws"] = int(nextgeo.get("resolve_stage_b_draws", 0))
	_result["stage_b_wrote"] = bool(nextgeo.get("resolve_stage_b_wrote", false))
	if SceneArgs.value("--require-stage-b", "") == "true" and not (_result["stage_b_wrote"] and _result["stage_b_draws"] > 0):
		_failures.append("NextGeo Stage B did not draw (%d draws), so its guide variants were not exercised" % _result["stage_b_draws"])

	var names := PackedStringArray(["diffuse_albedo", "specular_albedo", "normal_roughness", "prepass_normal_roughness"])
	var guides := await _dump(names)
	for texture_name in names:
		if not guides.has(texture_name):
			_failures.append("the %s texture could not be read" % texture_name)
	if not _failures.is_empty():
		return

	var surfaces := SceneSurfaces.new(self)
	var step := maxi(SceneArgs.int_value("--step", 4), 1)
	var normal_tolerance := _float_arg("--normal-tolerance", 1e-3)
	var geometry_tolerance := _float_arg("--geometry-normal-tolerance", 1.0 / 127.0)
	var view_to_world := camera.get_camera_transform().basis
	# Guide texels are internal-size pixels; CPU rays take viewport coordinates.
	var to_view := get_viewport().get_visible_rect().size / Vector2(size)
	_result["view_scale"] = [to_view.x, to_view.y]
	var samples: Array = []
	var counts := {"surface": 0, "sky": 0, "edge": 0, "unsupported": 0, "surface_mismatch": 0}
	var worst := {"diffuse_albedo": 0.0, "specular_albedo": 0.0, "normal_pack": 0.0, "normal_geometry": 0.0, "roughness": 0.0}
	var specular_zero_checked := 0
	var specular_range := Vector2(INF, -INF)
	var examples := {}
	var limit := {"diffuse_albedo": 0, "specular_albedo": 0, "normal_pack": 0, "normal_geometry": 0, "roughness": 0, "sky": 0, "surface_mismatch_inconsistent": 0}
	# Odd pixels: a half-resolution specular trace samples pixel 2t + 1 (_check_hit_distance()).
	for y in range(step / 2 + 1, size.y, step):
		for x in range(step / 2 + 1, size.x, step):
			var hit := surfaces.cast_pixel(camera, x, y, to_view)
			if hit.get("edge", false):
				counts["edge"] += 1
				continue
			var index := (y * size.x + x) * 4
			var diffuse := _texel(guides["diffuse_albedo"], index)
			var specular := _texel(guides["specular_albedo"], index)
			var packed := _texel(guides["normal_roughness"], index)
			if hit.is_empty():
				counts["sky"] += 1
				# 0.5 is between two RGBA8 codes.
				var sky_ok := absf(diffuse.r - 0.5) <= 0.6 / 255.0 and absf(diffuse.g - 0.5) <= 0.6 / 255.0 and absf(diffuse.b - 0.5) <= 0.6 / 255.0 and diffuse.a == 0.0
				sky_ok = sky_ok and specular == Color(0, 0, 0, 0) and packed == Color(0, 0, 0, 0)
				if not sky_ok:
					limit["sky"] += 1
					_example(examples, "sky", x, y, [diffuse, specular, packed])
				samples.append({"index": index, "x": x, "y": y, "sky": true})
				continue
			var material := hit["material"] as BaseMaterial3D
			if not SceneSurfaces.is_plain_opaque(material):
				counts["unsupported"] += 1
				continue
			# The renderer drew no opaque surface here (the pack writes 0 where the diffuse albedo
			# guide's alpha is 0) or another one than the mesh the CPU ray found: the CPU
			# reference does not apply.
			var guide_normal := Vector3(packed.r, packed.g, packed.b)
			if guide_normal == Vector3() or (not material.normal_enabled and _max_axis(guide_normal - hit["normal"]) > SURFACE_MISMATCH_NORMAL):
				counts["surface_mismatch"] += 1
				_example(examples, "surface_mismatch", x, y, [diffuse, specular, packed])
				# The guides must still agree with what was drawn: the clear values where nothing
				# was, and the prepass normal where something was.
				var consistent := true
				if guide_normal == Vector3():
					consistent = absf(diffuse.r - 0.5) <= 0.6 / 255.0 and diffuse.r == diffuse.g and diffuse.r == diffuse.b and diffuse.a == 0.0 and specular == Color(0, 0, 0, 0)
				else:
					var drawn := _texel(guides["prepass_normal_roughness"], index)
					consistent = _max_axis(guide_normal - (view_to_world * (Vector3(drawn.r, drawn.g, drawn.b) * 2.0 - Vector3.ONE)).normalized()) <= normal_tolerance and diffuse.a == 1.0 and specular.a == 1.0
				if not consistent:
					limit["surface_mismatch_inconsistent"] += 1
					_example(examples, "surface_mismatch_inconsistent", x, y, [diffuse, specular, packed])
				continue
			counts["surface"] += 1
			var albedo := material.albedo_color.srgb_to_linear() * (1.0 - material.metallic)
			var diffuse_error := maxf(absf(diffuse.r - albedo.r), maxf(absf(diffuse.g - albedo.g), absf(diffuse.b - albedo.b)))
			worst["diffuse_albedo"] = maxf(worst["diffuse_albedo"], diffuse_error)
			# The byte nearest to the value, and alpha 1 for a surface.
			if diffuse_error > 0.5 / 255.0 + 1e-5 or diffuse.a != 1.0:
				limit["diffuse_albedo"] += 1
				_example(examples, "diffuse_albedo", x, y, [diffuse, specular, packed])
			specular_range = Vector2(minf(specular_range.x, minf(specular.r, minf(specular.g, specular.b))), maxf(specular_range.y, maxf(specular.r, maxf(specular.g, specular.b))))
			if material.metallic == 0.0 and material.metallic_specular == 0.0:
				specular_zero_checked += 1
				worst["specular_albedo"] = maxf(worst["specular_albedo"], maxf(specular.r, maxf(specular.g, specular.b)))
				if specular != Color(0, 0, 0, 1):
					limit["specular_albedo"] += 1
					_example(examples, "specular_albedo", x, y, [diffuse, specular, packed])
			elif specular.a != 1.0:
				limit["specular_albedo"] += 1
				_example(examples, "specular_albedo", x, y, [diffuse, specular, packed])

			var prepass := _texel(guides["prepass_normal_roughness"], index)
			var prepass_normal := (view_to_world * (Vector3(prepass.r, prepass.g, prepass.b) * 2.0 - Vector3.ONE)).normalized()
			var pack_error := _max_axis(guide_normal - prepass_normal)
			worst["normal_pack"] = maxf(worst["normal_pack"], pack_error)
			if pack_error > normal_tolerance:
				limit["normal_pack"] += 1
				_example(examples, "normal_pack", x, y, [diffuse, specular, packed])
			if not material.normal_enabled:
				var geometry_error := _max_axis(guide_normal - hit["normal"])
				worst["normal_geometry"] = maxf(worst["normal_geometry"], geometry_error)
				if geometry_error > geometry_tolerance:
					limit["normal_geometry"] += 1
					_example(examples, "normal_geometry", x, y, [diffuse, specular, packed])
			var roughness_error := absf(packed.a - material.roughness)
			worst["roughness"] = maxf(worst["roughness"], roughness_error)
			# The prepass keeps 7 bits of roughness; the guide is an RGBA16F texel.
			if roughness_error > 0.5 / 127.0 + 1e-3:
				limit["roughness"] += 1
				_example(examples, "roughness", x, y, [diffuse, specular, packed])
			# NextGI decides what to trace from the prepass roughness, which the guide holds.
			samples.append({"index": index, "x": x, "y": y, "sky": false, "roughness": packed.a})

	_result["samples"] = counts
	_result["worst"] = worst
	_result["over_limit"] = limit
	_result["over_limit_examples"] = examples
	_result["specular_albedo_zero_samples"] = specular_zero_checked
	_result["specular_albedo_range"] = [specular_range.x, specular_range.y] if counts["surface"] > 0 else []
	_result["normal_within_1e-3_of_mesh"] = worst["normal_geometry"] <= 1e-3
	if counts["surface"] < 64:
		_failures.append("only %d surface samples" % counts["surface"])
	var max_mismatch := _float_arg("--max-surface-mismatch", 0.0)
	var mismatch_share := float(counts["surface_mismatch"]) / float(maxi(counts["surface"] + counts["surface_mismatch"], 1))
	_result["surface_mismatch_share"] = mismatch_share
	if mismatch_share > max_mismatch:
		_failures.append("at %d samples (%.4f of the surfaces) the renderer drew no surface or another one than the CPU ray found; --max-surface-mismatch is %s" % [counts["surface_mismatch"], mismatch_share, str(max_mismatch)])
	for key in limit:
		if limit[key] > 0:
			_failures.append("%d samples fail the %s check (worst %s)" % [limit[key], key, str(worst.get(key, ""))])

	await _check_hit_distance(size, samples)


func _check_hit_distance(p_size: Vector2i, p_samples: Array) -> void:
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var hit := {"tier": String(info.get("tier", "")), "quality": String(info.get("quality", "")), "compared": false}
	for key in ["reflection_rays", "reflection_hits", "reflection_misses", "screen_trace_hits"]:
		hit[key] = int(info.get(key, -1))
	_result["hit_distance"] = hit
	var min_samples := SceneArgs.int_value("--min-hit-samples", 0)
	var reference_mode := int(ProjectSettings.get_setting(REFERENCE_MODE_SETTING, 0))
	if reference_mode != REFERENCE_REFLECTIONS_ONLY or not bool(ProjectSettings.get_setting(REFERENCE_READBACK_SETTING, false)):
		hit["skipped"] = "the reference path tracer is not in its reflections-only mode with readback"
		if min_samples > 0:
			_failures.append("--min-hit-samples=%d: %s" % [min_samples, hit["skipped"]])
		return
	if hit["tier"] != "HWRT":
		_failures.append("the hit distance check needs the hardware ray tracing tier (tier=%s)" % hit["tier"])
		return

	# The trace runs at the view size divided by the quality's reflection divisor (Epic 1, High 2)
	# and writes trace pixel t, which samples view pixel floor((t + 0.5) * divisor): 2t + 1 at
	# High, so the odd sample pixels map onto trace pixels exactly.
	var divisor := {"EPIC": 1, "HIGH": 2}.get(hit["quality"], 0) as int
	hit["trace_divisor"] = divisor
	if divisor == 0:
		_failures.append("the hit distance check needs NextGI quality Epic or High (quality=%s)" % hit["quality"])
		return
	var hit_index := PackedInt32Array()

	# The guide is one ray per pixel and frame, so average it over frames.
	var frames := maxi(SceneArgs.int_value("--hit-frames", 64), 1)
	var sums := PackedFloat64Array()
	sums.resize(p_samples.size())
	var squares := PackedFloat64Array()
	squares.resize(p_samples.size())
	var misses := PackedInt32Array()
	misses.resize(p_samples.size())
	var finite_max := 0.0
	for frame in frames:
		var taken := await _dump(PackedStringArray(["hit_distance"]))
		if not taken.has("hit_distance"):
			_failures.append("the specular hit distance texture could not be read")
			return
		var texture_size: Vector2i = _probe.dump_sizes["hit_distance"]
		if texture_size.x * divisor < p_size.x or texture_size.y * divisor < p_size.y:
			_failures.append("the specular hit distance texture is %s, too small for a %s view traced at 1/%d" % [str(texture_size), str(p_size), divisor])
			return
		var distances: PackedFloat32Array = taken["hit_distance"]
		if frame == 0:
			hit["texture_size"] = [texture_size.x, texture_size.y]
			for sample in p_samples:
				hit_index.append(((int(sample["y"]) - divisor / 2) / divisor * texture_size.x + (int(sample["x"]) - divisor / 2) / divisor) * 4)
			# Texels holding a distance, per quarter of the view (top left, top right, bottom left,
			# bottom right): a full-resolution trace fills all four.
			var quarters := [0, 0, 0, 0]
			for y in texture_size.y:
				for x in texture_size.x:
					if distances[(y * texture_size.x + x) * 4] < MISS_DISTANCE:
						quarters[(2 if y >= texture_size.y / 2 else 0) + (1 if x >= texture_size.x / 2 else 0)] += 1
			hit["first_frame_distances_per_quarter"] = quarters
		for i in p_samples.size():
			var distance := distances[hit_index[i]]
			if distance >= MISS_DISTANCE:
				misses[i] += 1
			else:
				sums[i] += distance
				squares[i] += distance * distance
				finite_max = maxf(finite_max, distance)

	var spp := maxi(SceneArgs.int_value("--reference-spp", 256), 1)
	var stall := 0
	var last := -1
	while true:
		info = RenderingServer.nextgi_get_tier_info()
		var have := int(info.get("reference_samples", 0))
		if bool(info.get("reference_invalid", false)):
			_failures.append("the reference path tracer cannot render this scene")
			return
		if have >= spp:
			break
		stall = stall + 1 if have == last else 0
		last = have
		if stall > REFERENCE_STALL_FRAMES:
			_failures.append("the reference path tracer stopped at %d of %d samples" % [have, spp])
			return
		await RenderingServer.frame_post_draw
	var bytes: PackedByteArray = info.get("reference_reflection_guides", PackedByteArray())
	if int(info.get("reference_width", 0)) != p_size.x or int(info.get("reference_height", 0)) != p_size.y or bytes.size() != p_size.x * p_size.y * 16:
		_failures.append("the reference reflection guides are missing or are not %s" % str(p_size))
		return
	# Per pixel: hit distance sum, hits, samples.
	var reference := bytes.to_float32_array()
	hit["reference_samples"] = int(info.get("reference_samples", 0))
	hit["frames"] = frames

	var max_roughness := _float_arg("--hit-max-roughness", MAX_TRACE_ROUGHNESS)
	var tolerance := _float_arg("--hit-tolerance", 0.01)
	var errors := PackedFloat64Array()
	# Per pixel the guide mean has a standard error; the test allows three of them on top of
	# the tolerance. Per roughness, the summed guide means must match the summed reference
	# means within the tolerance alone.
	var over := 0
	var worst_excess := 0.0
	var groups := {}
	# Guide mean over the frames that hit / reference mean, on every traced pixel the reference
	# always hit (a report, not a test).
	var ratios := PackedFloat64Array()
	var untraced := 0
	var untraced_wrong := 0
	var traced := 0
	var mixed := 0
	var reference_miss := 0
	var reference_miss_guide_hit := 0
	var above_max_roughness := 0
	var reference_partial := 0
	var guide_misses := 0
	var traced_all_miss := 0
	var traced_some_miss := 0
	for i in p_samples.size():
		var sample: Dictionary = p_samples[i]
		var index := int(sample["index"])
		if bool(sample["sky"]) or float(sample["roughness"]) > MAX_TRACE_ROUGHNESS:
			untraced += 1
			if misses[i] != frames:
				untraced_wrong += 1
			continue
		traced += 1
		if misses[i] == frames:
			traced_all_miss += 1
		elif misses[i] > 0:
			traced_some_miss += 1
		var reference_hits := reference[index + 1]
		var reference_count := reference[index + 2]
		if reference_count <= 0.0:
			continue
		if reference_hits <= 0.0:
			reference_miss += 1
			if misses[i] == 0:
				reference_miss_guide_hit += 1
			continue
		var reference_distance := maxf(reference[index] / reference_hits, 1e-6)
		if reference_hits >= reference_count and misses[i] < frames:
			ratios.append(sums[i] / float(frames - misses[i]) / reference_distance)
		# Compare only where both always hit: a mean over hits alone is biased where some rays miss.
		if float(sample["roughness"]) > max_roughness:
			above_max_roughness += 1
		elif reference_hits < reference_count:
			reference_partial += 1
		elif misses[i] != 0:
			guide_misses += 1
		if float(sample["roughness"]) > max_roughness or reference_hits < reference_count or misses[i] != 0:
			mixed += 1
			continue
		var mean := sums[i] / float(frames)
		var standard_error := sqrt(maxf(squares[i] / float(frames) - mean * mean, 0.0) / float(frames))
		var error := absf(mean - reference_distance) / reference_distance
		var allowed := tolerance + 3.0 * standard_error / reference_distance
		errors.append(error)
		if error > allowed:
			over += 1
			worst_excess = maxf(worst_excess, error - allowed)
		var key := "%.3f" % float(sample["roughness"])
		var group: Array = groups.get(key, [0, 0.0, 0.0])
		groups[key] = [int(group[0]) + 1, float(group[1]) + mean, float(group[2]) + reference_distance]
	errors.sort()
	ratios.sort()
	hit["ratio_samples"] = ratios.size()
	hit["traced_guide_65504_every_frame"] = traced_all_miss
	hit["traced_guide_65504_some_frames"] = traced_some_miss
	hit["guide_largest_distance"] = finite_max
	if not ratios.is_empty():
		hit["ratio_p05_p50_p95"] = [ratios[int(ratios.size() * 0.05)], ratios[ratios.size() / 2], ratios[mini(int(ratios.size() * 0.95), ratios.size() - 1)]]
	hit["compared"] = not errors.is_empty()
	hit["compared_samples"] = errors.size()
	hit["traced_samples"] = traced
	hit["not_compared_samples"] = mixed
	hit["not_compared_rougher"] = above_max_roughness
	hit["not_compared_reference_some_misses"] = reference_partial
	hit["not_compared_guide_some_misses"] = guide_misses
	hit["untraced_samples"] = untraced
	hit["untraced_not_65504"] = untraced_wrong
	hit["reference_miss_samples"] = reference_miss
	hit["reference_miss_guide_hit"] = reference_miss_guide_hit
	hit["max_roughness"] = max_roughness
	hit["tolerance"] = tolerance
	if untraced_wrong > 0:
		_failures.append("%d untraced samples do not hold 65504 in every frame" % untraced_wrong)
	if errors.size() < min_samples:
		_failures.append("%d samples to compare the hit distance on, expected at least %d (roughness at most %s, every reference sample a hit)" % [errors.size(), min_samples, str(max_roughness)])
	if errors.is_empty():
		return
	hit["median_error"] = errors[errors.size() / 2]
	hit["p95_error"] = errors[mini(int(errors.size() * 0.95), errors.size() - 1)]
	hit["max_error"] = errors[errors.size() - 1]
	hit["over_tolerance"] = over
	hit["worst_excess"] = worst_excess
	if over > 0:
		_failures.append("the hit distance of %d of %d samples is more than %s plus three standard errors from the reference path tracer (median error %.4f, worst excess %.4f)" % [over, errors.size(), str(tolerance), hit["median_error"], worst_excess])
	var by_roughness := {}
	for key in groups:
		var group: Array = groups[key]
		var bias := float(group[1]) / float(group[2]) - 1.0
		by_roughness[key] = {"samples": group[0], "bias": bias}
		if absf(bias) > tolerance:
			_failures.append("at roughness %s the mean hit distance of %d samples is %.4f from the reference path tracer's" % [key, group[0], bias])
	hit["by_roughness"] = by_roughness


## Quads in a row, seen by an orthogonal camera in a view of FACTOR_VIEW pixels.
## Each is [name, x, albedo, metallic, specular, roughness, unshaded].
const FACTOR_VIEW := Vector2i(320, 64)
## The depth-prepass-alpha quad's roughness, which tells its prepass texel from the fourth quad's.
const PREPASS_QUAD_ROUGHNESS := 0.1
const FACTOR_QUADS := [
	["metal", -4.0, Color(1.0, 0.5, 0.25), 1.0, 0.5, 0.3, false],
	["rough_metal", -2.0, Color(1.0, 1.0, 1.0), 1.0, 0.5, 0.8, false],
	["dielectric", 0.0, Color(0.0, 0.0, 0.0), 0.0, 0.5, 0.2, false],
	["strong_dielectric", 2.0, Color(0.0, 0.0, 0.0), 0.0, 1.0, 0.6, false],
	["unshaded", 4.0, Color(0.2, 0.6, 0.9), 0.0, 0.5, 0.5, true],
]


func _check_factor() -> void:
	if not _guides_requested():
		_failures.append("--rr-check=factor needs GODOT_DEV_RR_GUIDES=1")
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	# HDR keeps the frame linear, so it can be compared with the guide.
	sub.use_hdr_2d = true
	sub.size = FACTOR_VIEW
	add_child(sub)

	# A white sky is the only light: ambient 1 and reflections 1 at every roughness.
	var sky_shader := Shader.new()
	sky_shader.code = "shader_type sky;\nvoid sky() {\n\tCOLOR = vec3(1.0);\n}\n"
	var sky_material := ShaderMaterial.new()
	sky_material.shader = sky_shader
	var sky := Sky.new()
	sky.sky_material = sky_material
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.environment = environment
	sub.add_child(world)

	# 12 m by 2.4 m: 5 quads of 2 m, with a strip of sky above and below them.
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 2.4
	camera.position = Vector3(0, 0, 2)
	camera.current = true
	sub.add_child(camera)
	_attach_probe(camera)

	for quad in FACTOR_QUADS:
		var material := _add_quad(sub, Vector3(quad[1], 0, 0), Vector2(2, 2), quad[2])
		material.metallic = quad[3]
		material.metallic_specular = quad[4]
		material.roughness = quad[5]
		if quad[6]:
			material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	# The sky's radiance map takes a few frames to filter.
	await _frames(SETTLE_FRAMES * 2)
	_record_allocation()
	if _probe.guide_textures != 3:
		_failures.append("%d of the 3 guide textures exist" % _probe.guide_textures)
		return
	var guides := await _dump(PackedStringArray(["diffuse_albedo", "specular_albedo"]))
	if not guides.has("diffuse_albedo") or not guides.has("specular_albedo"):
		_failures.append("the albedo guides could not be read")
		return
	var image := sub.get_texture().get_image()
	if image == null or image.get_size() != FACTOR_VIEW:
		_failures.append("the frame could not be read")
		return

	var quads := {}
	var pixels_per_meter := float(FACTOR_VIEW.x) / (camera.size * float(FACTOR_VIEW.x) / float(FACTOR_VIEW.y))
	var center_y := FACTOR_VIEW.y / 2
	for quad in FACTOR_QUADS:
		# Three pixels inside the right half of the quad, clear of its edges.
		var quad_result := {}
		var expected_diffuse: Color = (quad[2] as Color).srgb_to_linear() * (1.0 - float(quad[3]))
		var worst_diffuse := 0.0
		var worst_specular := 0.0
		var specular_value := Color()
		for offset in [0.25, 0.5, 0.75]:
			var x := int(float(FACTOR_VIEW.x) / 2.0 + (float(quad[1]) + offset) * pixels_per_meter)
			var index := (center_y * FACTOR_VIEW.x + x) * 4
			var diffuse := _texel(guides["diffuse_albedo"], index)
			var specular := _texel(guides["specular_albedo"], index)
			var frame := image.get_pixel(x, center_y)
			specular_value = specular
			worst_diffuse = maxf(worst_diffuse, maxf(absf(diffuse.r - expected_diffuse.r), maxf(absf(diffuse.g - expected_diffuse.g), absf(diffuse.b - expected_diffuse.b))))
			if diffuse.a != 1.0 or specular.a != 1.0:
				_failures.append("%s: a guide's alpha is not 1 on a surface" % quad[0])
			if quad[6]:
				worst_specular = maxf(worst_specular, maxf(specular.r, maxf(specular.g, specular.b)))
			else:
				worst_specular = maxf(worst_specular, maxf(absf(specular.r - frame.r), maxf(absf(specular.g - frame.g), absf(specular.b - frame.b))))
		quad_result["diffuse_error"] = worst_diffuse
		quad_result["specular_error"] = worst_specular
		quad_result["specular_albedo"] = [specular_value.r, specular_value.g, specular_value.b]
		quads[quad[0]] = quad_result
		if worst_diffuse > 0.5 / 255.0 + 1e-5:
			_failures.append("%s: the diffuse albedo is %.5f from the material's" % [quad[0], worst_diffuse])
		if quad[6]:
			if worst_specular != 0.0:
				_failures.append("%s: an unshaded surface has a specular albedo of %.5f" % [quad[0], worst_specular])
		elif worst_specular > 1.5 / 255.0:
			_failures.append("%s: the specular albedo is %.5f from the factor the frame shows" % [quad[0], worst_specular])
		elif maxf(specular_value.r, maxf(specular_value.g, specular_value.b)) <= 0.0:
			_failures.append("%s: the specular albedo is 0" % quad[0])
	_result["quads"] = quads

	# Above and below the quads there is only sky: the four rows at each edge hold the clear values.
	var sky_texels := 0
	var sky_wrong := 0
	var sky_example := []
	for y in [0, 1, 2, 3, FACTOR_VIEW.y - 4, FACTOR_VIEW.y - 3, FACTOR_VIEW.y - 2, FACTOR_VIEW.y - 1]:
		for x in FACTOR_VIEW.x:
			var index: int = (y * FACTOR_VIEW.x + x) * 4
			var sky_diffuse := _texel(guides["diffuse_albedo"], index)
			var sky_specular := _texel(guides["specular_albedo"], index)
			sky_texels += 1
			# 0.5 is between two RGBA8 codes.
			var diffuse_ok := absf(sky_diffuse.r - 0.5) <= 0.6 / 255.0 and sky_diffuse.r == sky_diffuse.g and sky_diffuse.r == sky_diffuse.b and sky_diffuse.a == 0.0
			if not diffuse_ok or sky_specular != Color(0, 0, 0, 0):
				sky_wrong += 1
				if sky_example.is_empty():
					sky_example = [x, y, str(sky_diffuse), str(sky_specular)]
	_result["sky_texels"] = sky_texels
	_result["sky_texels_wrong"] = sky_wrong
	if sky_wrong > 0:
		_failures.append("%d of %d sky texels do not hold diffuse 0.5 and specular 0 with alpha 0 (first at %s)" % [sky_wrong, sky_texels, str(sky_example)])

	# A transparent quad in front of the left half of the first quad: the frame changes there,
	# the guides nowhere.
	var glass := _add_quad(sub, Vector3(-4.5, 0, 0.5), Vector2(1, 2), Color(1, 0, 0, 0.5))
	glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	await _frames(8)
	var with_glass := await _dump(PackedStringArray(["diffuse_albedo", "specular_albedo"]))
	var glass_image := sub.get_texture().get_image()
	if not with_glass.has("diffuse_albedo") or not with_glass.has("specular_albedo") or glass_image == null:
		_failures.append("the guides or the frame could not be read with the transparent quad")
		return
	var glass_x := int(float(FACTOR_VIEW.x) / 2.0 + (-4.5) * pixels_per_meter)
	_result["transparent_changes_frame"] = glass_image.get_pixel(glass_x, center_y) != image.get_pixel(glass_x, center_y)
	if not _result["transparent_changes_frame"]:
		_failures.append("the transparent quad did not change the frame")
	for texture_name in ["diffuse_albedo", "specular_albedo"]:
		var before: PackedFloat32Array = guides[texture_name]
		var after: PackedFloat32Array = with_glass[texture_name]
		var changed := 0
		for i in range(0, before.size(), 4):
			if before[i] != after[i] or before[i + 1] != after[i + 1] or before[i + 2] != after[i + 2] or before[i + 3] != after[i + 3]:
				changed += 1
		_result["transparent_changed_%s_texels" % texture_name] = changed
		if changed > 0:
			_failures.append("the transparent quad changed %d texels of the %s guide" % [changed, texture_name])

	# A depth-prepass-alpha quad in front of the right half of the fourth quad. The depth prepass
	# draws it (its depth and normal-roughness), the opaque passes do not (it is drawn with the
	# transparent surfaces), so the surface behind fails the depth test there. All three guides
	# must say "no surface", and the prepass texel must be the quad's, so the case is exercised.
	var prepass_quad := _add_quad(sub, Vector3(2.5, 0, 0.5), Vector2(1, 2), Color(0.2, 0.8, 0.2, 1.0))
	prepass_quad.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS
	prepass_quad.roughness = PREPASS_QUAD_ROUGHNESS
	await _frames(8)
	var prepass_names := PackedStringArray(["diffuse_albedo", "specular_albedo", "normal_roughness", "prepass_normal_roughness"])
	var with_prepass := await _dump(prepass_names)
	for texture_name in prepass_names:
		if not with_prepass.has(texture_name):
			_failures.append("the %s texture could not be read with the depth-prepass-alpha quad" % texture_name)
			return
	var prepass_drawn := 0
	var prepass_wrong := 0
	var prepass_examples := []
	for offset in [0.25, 0.5, 0.75]:
		var x := int(float(FACTOR_VIEW.x) / 2.0 + (2.0 + offset) * pixels_per_meter)
		var index := (center_y * FACTOR_VIEW.x + x) * 4
		var diffuse := _texel(with_prepass["diffuse_albedo"], index)
		var specular := _texel(with_prepass["specular_albedo"], index)
		var packed := _texel(with_prepass["normal_roughness"], index)
		var prepass := _texel(with_prepass["prepass_normal_roughness"], index)
		# The quad's normal faces the camera, and the prepass keeps its roughness in 7 bits,
		# mirrored above 0.5 for a dynamic instance.
		var stored := prepass.a if prepass.a <= 0.5 else 1.0 - prepass.a
		if prepass.b > 0.75 and absf(stored * 255.0 / 127.0 - PREPASS_QUAD_ROUGHNESS) <= 0.5 / 127.0 + 1e-3:
			prepass_drawn += 1
		# 0.5 is between two RGBA8 codes.
		var no_surface := absf(diffuse.r - 0.5) <= 0.6 / 255.0 and diffuse.r == diffuse.g and diffuse.r == diffuse.b and diffuse.a == 0.0
		no_surface = no_surface and specular == Color(0, 0, 0, 0) and packed == Color(0, 0, 0, 0)
		if not no_surface:
			prepass_wrong += 1
			prepass_examples.append([x, center_y, str(diffuse), str(specular), str(packed)])
	_result["depth_prepass_alpha"] = {"samples": 3, "prepass_drew_quad": prepass_drawn, "guides_not_no_surface": prepass_wrong, "examples": prepass_examples}
	if prepass_drawn != 3:
		_failures.append("the depth prepass drew the depth-prepass-alpha quad at only %d of 3 samples, so the case was not exercised" % prepass_drawn)
	if prepass_wrong > 0:
		_failures.append("under the depth-prepass-alpha quad %d of 3 samples have a guide that does not say no surface (%s)" % [prepass_wrong, str(prepass_examples)])


func _add_quad(p_parent: Node, p_position: Vector3, p_size: Vector2, p_albedo: Color) -> StandardMaterial3D:
	var mesh := QuadMesh.new()
	mesh.size = p_size
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = p_position
	var material := StandardMaterial3D.new()
	material.albedo_color = p_albedo
	instance.material_override = material
	p_parent.add_child(instance)
	return material


## Keeps the first four failing samples of a check: x, y and the guide texels.
static func _example(r_examples: Dictionary, p_key: String, p_x: int, p_y: int, p_texels: Array) -> void:
	var list: Array = r_examples.get(p_key, [])
	if list.size() < 4:
		list.append([p_x, p_y] + p_texels.map(func(p_texel: Color) -> String: return str(p_texel)))
		r_examples[p_key] = list


static func _texel(p_data: PackedFloat32Array, p_index: int) -> Color:
	return Color(p_data[p_index], p_data[p_index + 1], p_data[p_index + 2], p_data[p_index + 3])


static func _max_axis(p_vector: Vector3) -> float:
	return maxf(absf(p_vector.x), maxf(absf(p_vector.y), absf(p_vector.z)))


## The scene's meshes for camera rays on the CPU.
class SceneSurfaces:
	var _instances: Array = []

	func _init(p_root: Node) -> void:
		for node in p_root.find_children("*", "MeshInstance3D", true, false):
			var instance := node as MeshInstance3D
			if instance.mesh == null or not instance.is_visible_in_tree():
				continue
			var triangles := instance.mesh.generate_triangle_mesh()
			if triangles == null:
				continue
			var entry := {
				"triangles": triangles,
				"to_world": instance.global_transform,
				"to_local": instance.global_transform.affine_inverse(),
				"normal_to_world": instance.global_transform.basis.inverse().transposed(),
				"surfaces": [],
			}
			var first_face := 0
			for surface in instance.mesh.get_surface_count():
				var arrays := instance.mesh.surface_get_arrays(surface)
				var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
				var indices := PackedInt32Array()
				if arrays[Mesh.ARRAY_INDEX] != null:
					indices = arrays[Mesh.ARRAY_INDEX]
				var faces := (indices.size() if not indices.is_empty() else vertices.size()) / 3
				var normals := PackedVector3Array()
				if arrays[Mesh.ARRAY_NORMAL] != null:
					normals = arrays[Mesh.ARRAY_NORMAL]
				entry["surfaces"].append({
					"first_face": first_face,
					"faces": faces,
					"vertices": vertices,
					"normals": normals,
					"indices": indices,
					"material": instance.get_active_material(surface),
				})
				first_face += faces
			_instances.append(entry)

	## The surface the pixel center sees, as {material, normal, position, id}, {} for none, or
	## {edge = true} when the pixel's corners see another surface or another normal.
	func cast_pixel(p_camera: Camera3D, p_x: int, p_y: int, p_to_view: Vector2) -> Dictionary:
		var center := _cast(p_camera, Vector2(p_x + 0.5, p_y + 0.5) * p_to_view)
		for corner in [Vector2(0, 0), Vector2(1, 0), Vector2(0, 1), Vector2(1, 1)]:
			var other := _cast(p_camera, (Vector2(p_x, p_y) + corner) * p_to_view)
			if other.is_empty() != center.is_empty():
				return {"edge": true}
			if not center.is_empty() and (other["id"] != center["id"] or other["normal"].dot(center["normal"]) < 0.9999):
				return {"edge": true}
		return center

	func _cast(p_camera: Camera3D, p_point: Vector2) -> Dictionary:
		var origin := p_camera.project_ray_origin(p_point)
		var direction := p_camera.project_ray_normal(p_point)
		var best := {}
		var best_distance := INF
		for instance_index in _instances.size():
			var entry: Dictionary = _instances[instance_index]
			var to_local: Transform3D = entry["to_local"]
			var hit: Dictionary = entry["triangles"].intersect_ray(to_local * origin, (to_local.basis * direction).normalized())
			if hit.is_empty():
				continue
			var position: Vector3 = entry["to_world"] * hit["position"]
			var distance := (position - origin).dot(direction)
			if distance >= best_distance:
				continue
			var face := int(hit["face_index"])
			for surface_index in entry["surfaces"].size():
				var surface: Dictionary = entry["surfaces"][surface_index]
				if face < surface["first_face"] or face >= surface["first_face"] + surface["faces"]:
					continue
				best_distance = distance
				best = {
					"id": instance_index * 1024 + surface_index,
					"material": surface["material"],
					"position": position,
					"normal": (entry["normal_to_world"] * _shading_normal(surface, face - int(surface["first_face"]), hit)).normalized(),
				}
				break
		return best

	## The vertex normals at the hit, or the face normal when the surface has none.
	func _shading_normal(p_surface: Dictionary, p_face: int, p_hit: Dictionary) -> Vector3:
		var normals: PackedVector3Array = p_surface["normals"]
		var face_normal: Vector3 = p_hit["normal"]
		if normals.is_empty():
			return face_normal
		var vertices: PackedVector3Array = p_surface["vertices"]
		var indices: PackedInt32Array = p_surface["indices"]
		var corner := PackedInt32Array([p_face * 3, p_face * 3 + 1, p_face * 3 + 2])
		if not indices.is_empty():
			corner = PackedInt32Array([indices[corner[0]], indices[corner[1]], indices[corner[2]]])
		var weights := Geometry3D.get_triangle_barycentric_coords(p_hit["position"], vertices[corner[0]], vertices[corner[1]], vertices[corner[2]])
		var normal := normals[corner[0]] * weights.x + normals[corner[1]] * weights.y + normals[corner[2]] * weights.z
		return normal.normalized() if normal.length_squared() > 1e-12 else face_normal

	## An opaque, lit StandardMaterial3D or ORMMaterial3D whose albedo is one color.
	static func is_plain_opaque(p_material: BaseMaterial3D) -> bool:
		if p_material == null:
			return false
		if p_material.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED or p_material.shading_mode == BaseMaterial3D.SHADING_MODE_UNSHADED:
			return false
		if p_material.albedo_texture != null or p_material.vertex_color_use_as_albedo or p_material.detail_enabled:
			return false
		return p_material.get_texture(BaseMaterial3D.TEXTURE_METALLIC) == null and p_material.get_texture(BaseMaterial3D.TEXTURE_ROUGHNESS) == null


## Reads the guide textures of the view it is attached to.
class GuideProbe extends CompositorEffect:
	const GUIDE_CONTEXT := &"rr_guides"
	const GUIDE_TEXTURES := [&"diffuse_albedo", &"specular_albedo", &"normal_roughness"]
	## Name, render buffers context, texture.
	const TEXTURES := {
		"diffuse_albedo": [&"rr_guides", &"diffuse_albedo"],
		"specular_albedo": [&"rr_guides", &"specular_albedo"],
		"normal_roughness": [&"rr_guides", &"normal_roughness"],
		"hit_distance": [&"rbgi", &"specular_hit"],
		"prepass_normal_roughness": [&"forward_clustered", &"normal_roughness"],
	}
	## Copies a texture to a buffer of one vec4 per texel.
	const COPY_SHADER := "#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Texels {
	vec4 data[];
}
texels;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}
	texels.data[pos.y * params.size.x + pos.x] = texelFetch(source, pos, 0);
}
"

	var calls := 0
	var guide_textures := 0
	var guide_bytes := 0
	var internal_size := Vector2i.ZERO
	var has_specular := false
	var has_velocity := false
	var error := ""
	var dump_sizes := {}

	var _mutex := Mutex.new()
	var _wanted := PackedStringArray()
	var _taken := {}
	var _shader := RID()
	var _pipeline := RID()
	var _sampler := RID()
	var _failed := false

	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT

	func request(p_names: PackedStringArray) -> void:
		_mutex.lock()
		_wanted = p_names
		_taken = {}
		_mutex.unlock()

	## The textures of the last request once a frame has copied them, or {}.
	func take() -> Dictionary:
		_mutex.lock()
		var taken := _taken
		_taken = {}
		_mutex.unlock()
		return taken

	func _render_callback(_callback_type: int, p_render_data: RenderData) -> void:
		var buffers := p_render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if buffers == null:
			return
		_mutex.lock()
		calls += 1
		guide_textures = 0
		for texture in GUIDE_TEXTURES:
			if buffers.has_texture(GUIDE_CONTEXT, texture):
				guide_textures += 1
		guide_bytes = int(buffers.get_context_memory_bytes(GUIDE_CONTEXT))
		internal_size = buffers.get_internal_size()
		has_specular = buffers.has_texture(&"forward_clustered", &"specular")
		has_velocity = buffers.has_texture(&"render_buffers", &"velocity")
		var wanted := _wanted
		_wanted = PackedStringArray()
		_mutex.unlock()
		if wanted.is_empty():
			return
		var rd := RenderingServer.get_rendering_device()
		if not _ensure_pipeline(rd):
			return
		var taken := {}
		var sizes := {}
		for texture_name in wanted:
			var source: Array = TEXTURES[texture_name]
			if not buffers.has_texture(source[0], source[1]):
				continue
			var format := buffers.get_texture_format(source[0], source[1])
			var size := Vector2i(format.width, format.height)
			var data := _copy(rd, buffers.get_texture_slice(source[0], source[1], 0, 0, 1, 1), size)
			if data.size() == size.x * size.y * 4:
				taken[texture_name] = data
				sizes[texture_name] = size
		_mutex.lock()
		_taken = taken
		dump_sizes = sizes
		_mutex.unlock()

	func _ensure_pipeline(p_rd: RenderingDevice) -> bool:
		if _pipeline.is_valid():
			return true
		if _failed or p_rd == null:
			return false
		var source := RDShaderSource.new()
		source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
		source.source_compute = COPY_SHADER
		var spirv := p_rd.shader_compile_spirv_from_source(source)
		if spirv == null or not spirv.compile_error_compute.is_empty():
			error = "the copy shader did not compile: %s" % (spirv.compile_error_compute if spirv != null else "no SPIR-V")
			_failed = true
			return false
		_shader = p_rd.shader_create_from_spirv(spirv, "RRGuidesCheckCopy")
		if _shader.is_valid():
			_pipeline = p_rd.compute_pipeline_create(_shader)
		_sampler = p_rd.sampler_create(RDSamplerState.new())
		if not _pipeline.is_valid() or not _sampler.is_valid():
			error = "the copy pipeline could not be created"
			_failed = true
			return false
		return true

	func _copy(p_rd: RenderingDevice, p_texture: RID, p_size: Vector2i) -> PackedFloat32Array:
		if not p_texture.is_valid() or p_size.x <= 0 or p_size.y <= 0:
			return PackedFloat32Array()
		var buffer := p_rd.storage_buffer_create(p_size.x * p_size.y * 16)
		var texture_uniform := RDUniform.new()
		texture_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		texture_uniform.binding = 0
		texture_uniform.add_id(_sampler)
		texture_uniform.add_id(p_texture)
		var buffer_uniform := RDUniform.new()
		buffer_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		buffer_uniform.binding = 1
		buffer_uniform.add_id(buffer)
		var uniform_set := p_rd.uniform_set_create([texture_uniform, buffer_uniform], _shader, 0)
		var data := PackedFloat32Array()
		if uniform_set.is_valid():
			var push := PackedInt32Array([p_size.x, p_size.y, 0, 0]).to_byte_array()
			var compute := p_rd.compute_list_begin()
			p_rd.compute_list_bind_compute_pipeline(compute, _pipeline)
			p_rd.compute_list_bind_uniform_set(compute, uniform_set, 0)
			p_rd.compute_list_set_push_constant(compute, push, push.size())
			p_rd.compute_list_dispatch(compute, (p_size.x + 7) / 8, (p_size.y + 7) / 8, 1)
			p_rd.compute_list_end()
			data = p_rd.buffer_get_data(buffer).to_float32_array()
			p_rd.free_rid(uniform_set)
		p_rd.free_rid(buffer)
		return data

	func _notification(p_what: int) -> void:
		if p_what != NOTIFICATION_PREDELETE:
			return
		var rd := RenderingServer.get_rendering_device()
		if rd == null:
			return
		for rid in [_pipeline, _shader, _sampler]:
			if rid.is_valid():
				rd.free_rid(rid)
