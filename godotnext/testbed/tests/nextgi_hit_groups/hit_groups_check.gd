extends Node3D

## Material hit groups check (#130, plan §21.2 L7).
##
## 256 materials, each on its own panel facing the camera: StandardMaterial3D feature sets (color,
## albedo texture at four UV scales, normal map, roughness and metallic textures, vertex color,
## alpha scissor and alpha hash traced as opaque, emission) and seven ShaderMaterial shaders (vertex() UV
## transform with a custom varying and a helper both stages use, texture() and textureProj() with a bias
## and a custom light() whose helper reads a derivative, vertex color with UV2, world vertex coordinates,
## a texture() read in vertex(), a cull_disabled shader whose vertex normals point away from the winding's
## front face, and a cull_disabled shader with ordinary normals). Several materials share each shader.
## Half the panels use an uncompressed mesh, half the same mesh with compressed attributes. Five more
## panels carry materials the hit groups refuse (dFdx, FRONT_FACING, an instance uniform, a cube texture,
## a screen texture). A few more panels have three surfaces with a different shader each and normals
## that bend across the panel (the geometry index selects the group), and one panel has no material
## (the default material's group shades it).
##
## In front of the panels float two MultiMeshes whose elements carry a color or custom data the hit
## groups do not carry (counted, fallback group) and one animated MultiMesh whose elements the GPU writes
## into the TLAS (fallback group, no record).
##
## With rendering/global_illumination/nextgi/debug/hit_material_check on, NextGI compiles one hit group
## per material shader, links them off the render thread, and traces every covered texel of each
## panel's captured surface-cache card back into the TLAS through them (nextgi_hit_base.glsl). The
## payload's albedo, normal, roughness, metallic and opacity must match the raster capture of the same
## texel within 1/255 on 99.9% of the samples, the group's shader record must name the material's
## shader, and a second trace without forced opacity must hit exactly where the capture kept the texel
## (the alpha-tested groups' any-hit stage). A third trace leaves each texel the other way and reaches the
## MultiMeshes: the GPU-written instances must land on the fallback group.
##
## The check runs through five phases: the setting off (no attribute stream: the RT arena keeps its
## bytes), on (the check passes), a shared shader's code changed again and again (dead groups are dropped
## from the counters and relinked away, and the check passes again), off again with NextGI first turned
## off in the environment, so no NextGI frame block runs (the stream and the hit groups are still freed:
## the arena is back to its bytes), and on again (the check passes again).
##
## Run it with NextGI on (High), for example through hit_groups_check.sh:
##   godot --path godotnext/testbed --rendering-driver vulkan res://tests/nextgi_hit_groups/hit_groups_check.tscn
## On a driver without RT pipelines (Metal until #172) pass -- --expect=no-pipeline: the hit groups
## report NO_RAYTRACING_PIPELINE, no attribute stream is allocated and nothing crashes. On the software
## tier (a D3D12 build without DXR, for example) pass -- --expect=not-hwrt: there is no RT arena, so no
## stream, and the hit groups report NOT_HWRT.
##
## With rendering/global_illumination/nextgi/hardware_ray_tracing_mode (#286) set to Inline, pass
## -- --expect=inline: no hit pipeline is made, no attribute stream is allocated, the arena keeps its bytes,
## and the hit groups and every feature that needs them report HWRT_MODE_INLINE. Set to Simplified Material,
## pass -- --expect=simplified-material: the check runs on the base library alone, every hit comes from the
## fallback group (the simplified hit shader), no material shader compiles a hit group, no stream is
## allocated, and the other features report HWRT_MODE_SIMPLIFIED_MATERIAL. The default, Full, is the check
## above, and the tier info must report FULL.
##
## GodotNext: nextgi_hit_discard (#304). Four more panels carry shaders that discard stripes in fragment(), two of
## them with an alpha scissor across the stripes as well. The raster capture drops the discarded texels, so the check
## also traces the texels the capture did not cover: a forced-opaque ray that hits a discard panel there must come back
## reported discarded (the closest-hit stage's discard branch, shaded as a fallback hit), the same ray without forced
## opacity must miss (the any-hit stage ignores the intersection), and almost no covered texel may report a discard.
## -- --materials=N makes a smaller run (N below 256: a multiple of 20, at least 120): the same kinds, fewer of each.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const SETTING := "rendering/global_illumination/nextgi/debug/hit_material_check"
const MATERIALS := 256
# 13 StandardMaterial3D feature sets, then 7 ShaderMaterial shaders.
const STANDARD_KINDS := 13
const KINDS := 20
const COLUMNS := 16
const PANEL_SIZE := Vector2(1.0, 0.5)
const PANEL_CELLS := Vector2i(4, 2)
const SPACING := Vector2(1.25, 0.75)
const TEXTURE_SIZE := 64
const MIN_MATCH := 0.999
const MAX_WAIT_FRAMES := 2400
const SETTLE_FRAMES := 40
# Three-surface panels, and the code changes of one shared shader (past the relink threshold of 16).
const MULTI_SURFACE_PANELS := 6
# GodotNext: nextgi_hit_discard (#304). The discard panels, after the refused ones.
const DISCARD_PANELS := 4
const SHADER_EDITS := 20
const EDIT_WAIT_FRAMES := 600
# The MultiMeshes in front of the panels.
const ELEMENT_GRID := Vector2i(4, 4)
const ELEMENT_Z := 1.5

var _failures := PackedStringArray()
var _result := {}
var _expect_pipeline := true
var _expect_hwrt := true
# GodotNext: nextgi_hwrt_mode (#286). The Hardware Ray Tracing Mode the run expects (tier info's name).
var _expect_mode := "FULL"
# Diagnostics: --kinds=0,4,12 builds every material from these kinds only, --uv-scale=1 fixes the UV scale.
var _kinds := PackedInt32Array()
var _uv_scale := 0.0
# GodotNext: nextgi_hit_discard (#304). --materials=N builds N materials instead of 256 (N of 256 or more keeps 256),
# rounded down to a multiple of KINDS and at least six of each kind (the multi-surface panels use materials of the
# fourth round, and the five refused panels must stay under a twentieth of the samples), for a smaller run.
var _material_count := MATERIALS
var _textures := {}
var _shaders := {}
var _environment: Environment
var _animated: MultiMesh
var _element_count := 0


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg == "--expect=no-pipeline":
			_expect_pipeline = false
		elif arg == "--expect=not-hwrt":
			_expect_pipeline = false
			_expect_hwrt = false
		elif arg == "--expect=inline":
			_expect_pipeline = false
			_expect_mode = "INLINE"
		elif arg == "--expect=simplified-material":
			_expect_mode = "SIMPLIFIED_MATERIAL"
		elif arg.begins_with("--kinds="):
			for kind in arg.trim_prefix("--kinds=").split(","):
				_kinds.append(int(kind))
		elif arg.begins_with("--uv-scale="):
			_uv_scale = float(arg.trim_prefix("--uv-scale="))
		elif arg.begins_with("--materials="):
			var wanted := int(arg.trim_prefix("--materials="))
			_material_count = MATERIALS if wanted >= MATERIALS else maxi(wanted / KINDS, 6) * KINDS
	_result["materials"] = _material_count
	ProjectSettings.set_setting(SETTING, false)
	_build_scene()
	_run()


func _run() -> void:
	# Off: no consumer, so no attribute stream and no hit groups.
	await _frames(SETTLE_FRAMES)
	var off: Dictionary = RenderingServer.nextgi_get_counters()
	var tier: Dictionary = RenderingServer.nextgi_get_tier_info()
	_result["tier"] = String(tier.get("tier", ""))
	_check(_result["tier"] == ("HWRT" if _expect_hwrt else "SWRT"), "the %s tier is active (%s)" % ["hardware" if _expect_hwrt else "software", _result["tier"]])
	var baseline_arena := int(off.get("rt_arena_bytes", 0))
	_result["arena_bytes_off"] = baseline_arena
	_check((baseline_arena > 0) == _expect_hwrt, "the RT arena holds the panels on the hardware tier only (%d bytes)" % baseline_arena)
	_check(not off.has("rt_attribute_bytes"), "no attribute stream is reported with the setting off")
	_check(String(tier.get("hit_groups", "")) == "NO_CONSUMER", "the hit groups report NO_CONSUMER with the setting off (%s)" % tier.get("hit_groups", ""))
	_result["hardware_ray_tracing_mode"] = String(tier.get("hardware_ray_tracing_mode", ""))
	_check(_result["hardware_ray_tracing_mode"] == _expect_mode, "the Hardware Ray Tracing Mode is %s (%s)" % [_expect_mode, _result["hardware_ray_tracing_mode"]])

	# On.
	ProjectSettings.set_setting(SETTING, true)
	_result["first"] = await _measure(baseline_arena)

	# One shared shader's code changes again and again: each code is a new hit group, the old ones die.
	if _expect_pipeline and _expect_mode == "FULL":
		_result["edits"] = await _edit_shader(baseline_arena)

	# Off again, with NextGI off in the environment first, so no frame block runs: everything is still freed.
	_environment.nextgi_enabled = false
	await _frames(4)
	ProjectSettings.set_setting(SETTING, false)
	await _frames(SETTLE_FRAMES)
	var off_again: Dictionary = RenderingServer.nextgi_get_counters()
	_result["arena_bytes_off_again"] = int(off_again.get("rt_arena_bytes", 0))
	_check(int(off_again.get("rt_arena_bytes", 0)) == baseline_arena, "the arena is back to its bytes once the setting is off (%d, %d)" % [off_again.get("rt_arena_bytes", 0), baseline_arena])
	_check(not off_again.has("rt_attribute_bytes"), "no attribute stream is reported once the setting is off again")
	_check(String(RenderingServer.nextgi_get_tier_info().get("hit_groups", "")) == "NO_CONSUMER", "the hit groups are gone once the setting is off again")

	# On again: the groups compile and link from scratch, and the check passes again.
	_environment.nextgi_enabled = true
	ProjectSettings.set_setting(SETTING, true)
	_result["second"] = await _measure(baseline_arena)
	ProjectSettings.set_setting(SETTING, false)

	_result["pass"] = _failures.is_empty()
	_result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)


func _measure(p_baseline_arena: int, p_runs_before := 0) -> Dictionary:
	var out := {}
	var counters := {}
	var waited := 0
	var runs_before := p_runs_before
	while waited < MAX_WAIT_FRAMES:
		await RenderingServer.frame_post_draw
		waited += 1
		counters = RenderingServer.nextgi_get_counters()
		if not _expect_pipeline:
			if waited >= SETTLE_FRAMES:
				break
			continue
		if int(counters.get("hit_check_runs", 0)) > runs_before:
			break
	out["frames"] = waited
	var tier: Dictionary = RenderingServer.nextgi_get_tier_info()
	out["hit_groups"] = String(tier.get("hit_groups", ""))
	# The TLAS's instances (CPU-fed and GPU-written) and those left out, for the report.
	for key in ["rt_fed_instance_count", "rt_instance_count"]:
		out[key] = int(tier.get(key, 0))
	for key in ["rt_dynamic_excluded", "rt_coverage_numerator", "rt_coverage_denominator", "rt_blas_pending", "rt_not_in_scene"]:
		out[key] = int(counters.get(key, 0))
	var attribute_bytes := int(counters.get("rt_attribute_bytes", 0))
	out["rt_attribute_bytes"] = attribute_bytes
	out["rt_attribute_surfaces"] = int(counters.get("rt_attribute_surfaces", 0))
	out["rt_arena_bytes"] = int(counters.get("rt_arena_bytes", 0))
	var by_consumer: Dictionary = tier.get("hit_groups_by_consumer", {})
	out["hit_groups_by_consumer"] = by_consumer
	if _expect_mode == "INLINE":
		# GodotNext: nextgi_hwrt_mode (#286). No hit pipeline: the mode is the reason, for every feature.
		_check(out["hit_groups"] == "HWRT_MODE_INLINE", "the hit groups report HWRT_MODE_INLINE (%s)" % out["hit_groups"])
		for feature in ["hit_lighting", "translucency", "far_field", "material_retrace", "check", "path_tracer"]:
			_check(String(by_consumer.get(feature, "")) == "HWRT_MODE_INLINE", "%s reports HWRT_MODE_INLINE (%s)" % [feature, by_consumer.get(feature, "")])
		_check(attribute_bytes == 0 and int(counters.get("rt_attribute_surfaces", 0)) == 0, "no attribute stream is allocated in Inline (%d bytes)" % attribute_bytes)
		_check(int(counters.get("rt_arena_bytes", 0)) == p_baseline_arena, "the arena keeps its bytes in Inline (%d, %d)" % [counters.get("rt_arena_bytes", 0), p_baseline_arena])
		_check(not counters.has("hit_check_runs") and not counters.has("hit_shaders"), "no hit pipeline exists in Inline")
		return out
	if not _expect_hwrt:
		# The software tier has no RT arena, so a consumer gets no stream and no hit group.
		_check(out["hit_groups"] == "NOT_HWRT", "the hit groups report NOT_HWRT (%s)" % out["hit_groups"])
		_check(attribute_bytes == 0 and int(counters.get("rt_attribute_surfaces", 0)) == 0, "no attribute stream exists without the RT arena (%d bytes)" % attribute_bytes)
		_check(not counters.has("hit_check_runs"), "no hit group runs on the software tier")
		return out
	if not _expect_pipeline:
		# The stream exists only for hit groups that run: none on a driver without RT pipelines.
		_check(out["hit_groups"] == "NO_RAYTRACING_PIPELINE", "the hit groups report NO_RAYTRACING_PIPELINE (%s)" % out["hit_groups"])
		_check(attribute_bytes == 0 and int(counters.get("rt_attribute_surfaces", 0)) == 0, "no attribute stream is allocated for hit groups that cannot run (%d bytes)" % attribute_bytes)
		_check(int(counters.get("rt_arena_bytes", 0)) == p_baseline_arena, "the arena keeps its bytes (%d, %d)" % [counters.get("rt_arena_bytes", 0), p_baseline_arena])
		_check(not counters.has("hit_check_runs"), "no hit group runs without RT pipelines")
		return out
	if _expect_mode == "SIMPLIFIED_MATERIAL":
		return _measure_simplified(out, counters, by_consumer, p_baseline_arena, runs_before)
	_check(int(counters.get("hit_material_groups", 0)) == 1, "the material hit groups are on at Full")
	# GodotNext: nextgi_hwrt_mode (#286). The check asks, so its reason is the one the frame block found.
	_check(String(by_consumer.get("check", "")) == out["hit_groups"], "the check reports the hit groups' reason (%s, %s)" % [by_consumer.get("check", ""), out["hit_groups"]])
	# Both panel meshes (uncompressed and compressed) get a stream of 32 header bytes and 20 bytes per vertex,
	# in the arena.
	var vertices := (PANEL_CELLS.x + 1) * (PANEL_CELLS.y + 1)
	_check(int(counters.get("rt_attribute_surfaces", 0)) >= 2, "both panel meshes have an attribute stream (%d surfaces)" % counters.get("rt_attribute_surfaces", 0))
	_check(attribute_bytes >= 2 * (32 + vertices * 20), "the attribute stream holds the panels' vertices (%d bytes)" % attribute_bytes)
	_check(int(counters.get("rt_arena_bytes", 0)) == p_baseline_arena + attribute_bytes, "the stream's bytes are arena bytes (%d = %d + %d)" % [counters.get("rt_arena_bytes", 0), p_baseline_arena, attribute_bytes])
	_check(out["hit_groups"] == "AVAILABLE", "the hit groups are available (%s)" % out["hit_groups"])
	_check(int(counters.get("hit_check_runs", 0)) > runs_before, "the check ran within %d frames" % MAX_WAIT_FRAMES)

	for key in counters.keys():
		if String(key).begins_with("hit_"):
			out[key] = counters[key]
	var samples := int(counters.get("hit_check_samples", 0))
	var hits := int(counters.get("hit_check_hits", 0))
	var fallback := int(counters.get("hit_check_fallback", 0))
	var shaded := hits - fallback
	_check(samples >= _material_count * 1000, "the check traced the panels' texels (%d samples)" % samples)
	_check(hits >= samples * MIN_MATCH, "the rays hit (%d of %d)" % [hits, samples])
	# The refused materials' panels are shaded by the fallback group; nothing else is.
	_check(fallback > 0, "the refused materials' hits use the fallback group (%d)" % fallback)
	_check(fallback < samples / 20, "only the refused materials use the fallback group (%d of %d)" % [fallback, samples])
	for name in ["albedo", "normal", "roughness", "metallic", "opacity", "record", "all"]:
		var matched := int(counters.get("hit_check_" + name, 0))
		var share := float(matched) / float(max(shaded, 1))
		out["share_" + name] = share
		_check(share >= MIN_MATCH, "%s matches the raster capture on %.5f of the shaded samples (%d of %d)" % [name, share, matched, shaded])
	var any_samples := int(counters.get("hit_check_any_hit_samples", 0))
	var any_agree := int(counters.get("hit_check_any_hit_agree", 0))
	out["share_any_hit"] = float(any_agree) / float(max(any_samples, 1))
	_check(any_samples == samples, "every sample is traced again without forced opacity")
	_check(any_agree >= any_samples * MIN_MATCH, "the any-hit decisions match the captured alpha (%d of %d)" % [any_agree, any_samples])
	# GodotNext: nextgi_hit_discard (#304). The discard panels' stripes: the capture dropped them, the closest-hit stage
	# reports them discarded and the any-hit stage ignores them.
	var discard_cut := int(counters.get("hit_check_discard_cut", 0))
	var discard_ignored := int(counters.get("hit_check_discard_ignored", 0))
	var discard_covered := int(counters.get("hit_check_discard_covered", 0))
	out["discard_cut"] = discard_cut
	out["discard_ignored"] = discard_ignored
	out["discard_covered"] = discard_covered
	_check(discard_cut >= DISCARD_PANELS * 100, "the closest-hit stage reports the texels fragment() discards (%d)" % discard_cut)
	_check(discard_ignored >= discard_cut * MIN_MATCH, "the any-hit stage ignores the texels fragment() discards (%d of %d)" % [discard_ignored, discard_cut])
	_check(discard_covered <= samples * (1.0 - MIN_MATCH), "almost no texel the capture kept reports a discard (%d of %d)" % [discard_covered, samples])
	_check(int(counters.get("hit_check_alpha_tested", 0)) > 0, "samples land on alpha-tested groups")
	_check(int(counters.get("hit_check_alpha_cut", 0)) > 0, "the scissor cut some of those texels, so the any-hit had to ignore them")
	# Every material, and the default material of the panel without one, has its own group.
	_check(int(counters.get("hit_check_materials", 0)) >= _material_count + 1, "every material and the default material were sampled (%d)" % counters.get("hit_check_materials", 0))
	_check(int(counters.get("hit_groups_linked", 0)) >= _shaders.size(), "a group per eligible shader is linked (%d, %d shaders)" % [counters.get("hit_groups_linked", 0), _shaders.size()])
	_check(int(counters.get("hit_groups_pending", 0)) == 0, "no group is pending")
	_check(int(counters.get("hit_ineligible", 0)) >= 5, "the refused shaders are counted (%d)" % counters.get("hit_ineligible", 0))
	_check(int(counters.get("hit_ineligible_screen_read", 0)) >= 1, "the screen-texture shader is refused as SCREEN_READ")
	_check(int(counters.get("hit_ineligible_derivative", 0)) >= 1, "the dFdx shader is refused as DERIVATIVE")
	_check(int(counters.get("hit_ineligible_builtin", 0)) >= 1, "the FRONT_FACING shader is refused as BUILTIN")
	_check(int(counters.get("hit_ineligible_instance_uniform", 0)) >= 1, "the instance-uniform shader is refused as INSTANCE_UNIFORM")
	_check(int(counters.get("hit_ineligible_texture_type", 0)) >= 1, "the cube-texture shader is refused as TEXTURE_TYPE")
	_check(int(counters.get("hit_light_skipped", 0)) >= 1, "the custom light() shader is counted as skipped at hits")
	_check(int(counters.get("hit_materials", 0)) >= _material_count, "every material has a hit record")
	# The MultiMesh elements whose color or custom data the material reads use the fallback group, counted.
	var element_geometries := int(counters.get("hit_element_data_geometries", 0))
	out["element_data_geometries"] = element_geometries
	_check(element_geometries > 0 and element_geometries <= _element_count, "the MultiMesh elements with color or custom data are counted (%d of %d)" % [element_geometries, _element_count])
	# Nothing else uses the fallback group: not the panel without a material (the default material's group).
	_check(int(counters.get("hit_fallback_geometries", 0)) == 5 + element_geometries, "only the refused panels and those elements use the fallback group (%d, expected %d)" % [counters.get("hit_fallback_geometries", 0), 5 + element_geometries])
	# The third trace reaches the MultiMeshes: the GPU-written instances have no record and land on the
	# fallback group.
	var probe_gpu := int(counters.get("hit_check_probe_gpu_instances", 0))
	out["probe_hits"] = int(counters.get("hit_check_probe_hits", 0))
	out["probe_gpu_instances"] = probe_gpu
	_check(probe_gpu > 0, "the probe rays hit GPU-written TLAS instances (%d)" % probe_gpu)
	_check(int(counters.get("hit_check_probe_gpu_fallback", 0)) == probe_gpu, "every GPU-written instance hit uses the fallback group (%d of %d)" % [counters.get("hit_check_probe_gpu_fallback", 0), probe_gpu])
	# Report only (GL-3, measured in PQ): the off-thread group add and link times.
	print("NEXTGI_HIT_TIMING group_add_ms_max=%.3f link_ms_max=%.3f links=%d" % [counters.get("hit_group_add_ms_max", 0.0), counters.get("hit_link_ms_max", 0.0), counters.get("hit_links", 0)])
	return out


# GodotNext: nextgi_hwrt_mode (#286). Simplified Material: the base library alone. The check traces through
# the fallback group, the simplified hit shader; no material compiles a hit group and no stream exists.
func _measure_simplified(p_out: Dictionary, p_counters: Dictionary, p_by_consumer: Dictionary, p_baseline_arena: int, p_runs_before: int) -> Dictionary:
	for key in p_counters.keys():
		if String(key).begins_with("hit_"):
			p_out[key] = p_counters[key]
	_check(p_out["hit_groups"] == "AVAILABLE", "the simplified hit pipeline runs for the check (%s)" % p_out["hit_groups"])
	_check(String(p_by_consumer.get("check", "")) == "AVAILABLE", "the check reports AVAILABLE (%s)" % p_by_consumer.get("check", ""))
	for feature in ["hit_lighting", "translucency", "far_field", "material_retrace", "path_tracer"]:
		_check(String(p_by_consumer.get(feature, "")) == "HWRT_MODE_SIMPLIFIED_MATERIAL", "%s reports HWRT_MODE_SIMPLIFIED_MATERIAL (%s)" % [feature, p_by_consumer.get(feature, "")])
	_check(int(p_counters.get("hit_check_runs", 0)) > p_runs_before, "the check ran within %d frames" % MAX_WAIT_FRAMES)
	_check(int(p_counters.get("hit_material_groups", 1)) == 0, "the material hit groups are off")
	_check(int(p_counters.get("rt_attribute_bytes", 0)) == 0 and int(p_counters.get("rt_attribute_surfaces", 0)) == 0, "no attribute stream is allocated (%d bytes)" % p_counters.get("rt_attribute_bytes", 0))
	_check(int(p_counters.get("rt_arena_bytes", 0)) == p_baseline_arena, "the arena keeps its bytes (%d, %d)" % [p_counters.get("rt_arena_bytes", 0), p_baseline_arena])
	_check(int(p_counters.get("hit_shaders", -1)) == 0, "no material shader compiled a hit group (%d)" % p_counters.get("hit_shaders", -1))
	_check(int(p_counters.get("hit_groups_linked", -1)) == 0 and int(p_counters.get("hit_groups_pending", -1)) == 0, "no material group is linked or pending")
	_check(int(p_counters.get("hit_materials", -1)) == 0, "no material was looked up (%d)" % p_counters.get("hit_materials", -1))
	var geometries := int(p_counters.get("hit_geometries", 0))
	_check(geometries > 0 and int(p_counters.get("hit_fallback_geometries", 0)) == geometries, "every geometry uses the simplified hit shader (%d of %d)" % [p_counters.get("hit_fallback_geometries", 0), geometries])
	var samples := int(p_counters.get("hit_check_samples", 0))
	var hits := int(p_counters.get("hit_check_hits", 0))
	_check(samples >= _material_count * 1000, "the check traced the panels' texels (%d samples)" % samples)
	_check(hits >= samples * MIN_MATCH, "the rays hit (%d of %d)" % [hits, samples])
	_check(int(p_counters.get("hit_check_fallback", 0)) == hits, "every hit comes from the simplified hit shader (%d of %d)" % [p_counters.get("hit_check_fallback", 0), hits])
	var probe_gpu := int(p_counters.get("hit_check_probe_gpu_instances", 0))
	p_out["probe_gpu_instances"] = probe_gpu
	_check(probe_gpu > 0, "the probe rays hit GPU-written TLAS instances (%d)" % probe_gpu)
	_check(int(p_counters.get("hit_check_probe_gpu_fallback", 0)) == probe_gpu, "GPU-written instance hits use the fallback group too (%d of %d)" % [p_counters.get("hit_check_probe_gpu_fallback", 0), probe_gpu])
	return p_out


# Changes one shared shader's code SHADER_EDITS times, each time waiting until its new group is linked, so
# every old code leaves a dead linked group behind. The counters must count only the live shaders, the dead
# groups must be relinked away (hit_relinks), and the check must pass again on the new table.
func _edit_shader(p_baseline_arena: int) -> Dictionary:
	var before: Dictionary = RenderingServer.nextgi_get_counters()
	var live := int(before.get("hit_shaders", 0))
	var linked_before := int(before.get("hit_groups_linked", 0))
	var relinks_before := int(before.get("hit_relinks", 0))
	var shader: Shader = _shaders["shader_2"]
	var edits_linked := 0
	for edit in SHADER_EDITS:
		shader.code = _shader_code(2) + "\n// Edit %d.\n" % edit
		var waited := 0
		var counters := {}
		# The new code's group compiles and links within a few frames once the materials ask for it.
		await _frames(4)
		while waited < EDIT_WAIT_FRAMES:
			await RenderingServer.frame_post_draw
			waited += 1
			counters = RenderingServer.nextgi_get_counters()
			if int(counters.get("hit_groups_pending", 1)) == 0:
				break
		if int(counters.get("hit_groups_pending", 1)) == 0:
			edits_linked += 1
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var waited := 0
	while waited < EDIT_WAIT_FRAMES and (int(counters.get("hit_relinks", 0)) == relinks_before or int(counters.get("hit_groups_pending", 1)) != 0):
		await RenderingServer.frame_post_draw
		waited += 1
		counters = RenderingServer.nextgi_get_counters()
	var out := await _measure(p_baseline_arena, int(counters.get("hit_check_runs", 0)))
	counters = RenderingServer.nextgi_get_counters()
	out["edits_linked"] = edits_linked
	out["shaders_before"] = live
	out["shaders_after"] = int(counters.get("hit_shaders", 0))
	out["relinks"] = int(counters.get("hit_relinks", 0)) - relinks_before
	_check(edits_linked == SHADER_EDITS, "every new code of the edited shader linked (%d of %d)" % [edits_linked, SHADER_EDITS])
	_check(out["shaders_after"] == live, "the dead codes are not counted: %d shaders before the edits, %d after" % [live, out["shaders_after"]])
	_check(int(counters.get("hit_groups_linked", 0)) == linked_before, "only the live shaders' groups are counted as linked (%d before the edits, %d after)" % [linked_before, counters.get("hit_groups_linked", 0)])
	_check(out["relinks"] >= 1, "the dead groups were relinked away (%d relinks)" % out["relinks"])
	return out


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _check(p_condition: bool, p_message: String) -> void:
	if not p_condition:
		_failures.append(p_message)
		printerr("hit_groups_check: " + p_message)


func _build_scene() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.05, 0.06)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1)
	env.ambient_light_energy = 0.3
	env.nextgi_enabled = true
	_environment = env
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50, -30, 0)
	add_child(light)

	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 14)
	camera.current = true
	add_child(camera)

	var plain := _panel_mesh(0)
	var compressed := _panel_mesh(Mesh.ARRAY_FLAG_COMPRESS_ATTRIBUTES)
	var materials := _materials()
	var panels := materials.size() + MULTI_SURFACE_PANELS + 1
	var rows := int(ceil(float(panels) / float(COLUMNS)))
	for i in panels:
		var panel := MeshInstance3D.new()
		var column := i % COLUMNS
		var row := i / COLUMNS
		panel.position = Vector3((column - (COLUMNS - 1) * 0.5) * SPACING.x, ((rows - 1) * 0.5 - row) * SPACING.y, 0.0)
		if i < materials.size():
			panel.mesh = plain if i % 2 == 0 else compressed
			panel.material_override = materials[i]
		elif i < materials.size() + MULTI_SURFACE_PANELS:
			# Three surfaces, three shaders: a StandardMaterial3D with normal and ORM textures, the vertex() UV
			# transform shader and the vertex color shader, as surface override materials.
			var k := i - materials.size()
			panel.mesh = _multi_surface_mesh(k % 2 == 1)
			panel.set_surface_override_material(0, materials[6 + KINDS * (k % 4)])
			panel.set_surface_override_material(1, materials[STANDARD_KINDS + 0 + KINDS * (k % 4)])
			panel.set_surface_override_material(2, materials[STANDARD_KINDS + 2 + KINDS * (k % 4)])
		else:
			# No material at all: the default material.
			panel.mesh = plain
		add_child(panel)
		if i < materials.size() and materials[i] is ShaderMaterial and materials[i].has_meta("instance_tint"):
			panel.set_instance_shader_parameter("instance_tint", materials[i].get_meta("instance_tint"))
	_build_multimeshes()


func _process(_delta: float) -> void:
	# Changed every frame, so NextGI feeds this MultiMesh's elements as GPU-written TLAS instances.
	if _animated != null:
		_animated.set_instance_transform(0, _animated.get_instance_transform(0))


# Quads between the camera and the panels, which the check's probe rays reach: one MultiMesh with element
# colors under a material that reads COLOR, one with custom data under a shader that reads INSTANCE_CUSTOM
# (both CPU-fed elements), and one animated MultiMesh (GPU-written elements).
func _build_multimeshes() -> void:
	var quad := QuadMesh.new()
	quad.size = Vector2(0.6, 0.6)
	var colored := StandardMaterial3D.new()
	colored.vertex_color_use_as_albedo = true
	var custom := ShaderMaterial.new()
	custom.shader = Shader.new()
	custom.shader.code = "shader_type spatial;\nvarying float v_custom;\nvoid vertex() {\n\tv_custom = INSTANCE_CUSTOM.x;\n}\nvoid fragment() {\n\tALBEDO = vec3(0.4, 0.5, 0.6);\n\tROUGHNESS = v_custom;\n}\n"
	var plain := StandardMaterial3D.new()
	plain.albedo_color = Color(0.7, 0.7, 0.7)
	var specs := [[true, false, colored, Vector2(-4.5, 2.0)], [false, true, custom, Vector2(4.5, 2.0)], [false, false, plain, Vector2(0.0, -2.0)]]
	for spec in specs:
		var multimesh := MultiMesh.new()
		multimesh.transform_format = MultiMesh.TRANSFORM_3D
		multimesh.use_colors = spec[0]
		multimesh.use_custom_data = spec[1]
		multimesh.mesh = quad
		multimesh.instance_count = ELEMENT_GRID.x * ELEMENT_GRID.y
		# The GPU writes the animated MultiMesh's element transforms into the TLAS as they are, without the
		# node's transform, so that node stays at the origin and its elements carry the offset.
		var animated: bool = not spec[0] and not spec[1]
		var offset := Vector3(spec[3].x, spec[3].y, ELEMENT_Z)
		for e in multimesh.instance_count:
			var x := float(e % ELEMENT_GRID.x) - (ELEMENT_GRID.x - 1) * 0.5
			var y := float(e / ELEMENT_GRID.x) - (ELEMENT_GRID.y - 1) * 0.5
			multimesh.set_instance_transform(e, Transform3D(Basis(), Vector3(x * 0.9, y * 0.9, 0.0) + (offset if animated else Vector3())))
			if spec[0]:
				multimesh.set_instance_color(e, Color.from_hsv(float(e) / multimesh.instance_count, 0.7, 0.9))
			if spec[1]:
				multimesh.set_instance_custom_data(e, Color(float(e) / multimesh.instance_count, 0.0, 0.0, 1.0))
		var node := MultiMeshInstance3D.new()
		node.multimesh = multimesh
		node.material_override = spec[2]
		node.position = Vector3() if animated else offset
		add_child(node)
		if spec[0] or spec[1]:
			_element_count += multimesh.instance_count
		else:
			_animated = multimesh


# A panel of three surfaces (thirds along X) whose normals bend across the panel, so the hit's interpolated
# normal differs from the face normal. Compressed attributes on request.
func _multi_surface_mesh(p_compressed: bool) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	for part in 3:
		var positions := PackedVector3Array()
		var normals := PackedVector3Array()
		var tangents := PackedFloat32Array()
		var uvs := PackedVector2Array()
		var uv2s := PackedVector2Array()
		var colors := PackedColorArray()
		var indices := PackedInt32Array()
		for y in PANEL_CELLS.y + 1:
			for x in 3:
				var fx := (float(part) + float(x) * 0.5) / 3.0
				var fy := float(y) / PANEL_CELLS.y
				positions.append(Vector3((fx - 0.5) * PANEL_SIZE.x, (fy - 0.5) * PANEL_SIZE.y, 0.0))
				var normal := Vector3((fx - 0.5) * 1.2, (fy - 0.5) * 0.6, 1.0).normalized()
				normals.append(normal)
				var tangent := Vector3.UP.cross(normal).normalized()
				tangents.append_array(PackedFloat32Array([tangent.x, tangent.y, tangent.z, 1.0]))
				uvs.append(Vector2(fx * PANEL_SIZE.x, (1.0 - fy) * PANEL_SIZE.y))
				uv2s.append(Vector2(fx * PANEL_SIZE.x * 0.5 + 0.25, (1.0 - fy) * PANEL_SIZE.y * 0.5 + 0.25))
				colors.append(Color(0.2 + 0.6 * fx, 0.3 + 0.5 * fy, 0.8 - 0.5 * fx * fy, 1.0))
		for y in PANEL_CELLS.y:
			for x in 2:
				var i := y * 3 + x
				var j := i + 3
				indices.append_array(PackedInt32Array([i, j, i + 1, i + 1, j, j + 1]))
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = positions
		arrays[Mesh.ARRAY_NORMAL] = normals
		arrays[Mesh.ARRAY_TANGENT] = tangents
		arrays[Mesh.ARRAY_TEX_UV] = uvs
		arrays[Mesh.ARRAY_TEX_UV2] = uv2s
		arrays[Mesh.ARRAY_COLOR] = colors
		arrays[Mesh.ARRAY_INDEX] = indices
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, Mesh.ARRAY_FLAG_COMPRESS_ATTRIBUTES if p_compressed else 0)
	return mesh


# A grid panel in the XY plane facing +Z: normals, tangents, two UV sets with the same texel density on
# both axes (so the raster's derivatives and the ray cone pick the same level), and smooth vertex colors.
func _panel_mesh(p_flags: int) -> ArrayMesh:
	var positions := PackedVector3Array()
	var normals := PackedVector3Array()
	var tangents := PackedFloat32Array()
	var uvs := PackedVector2Array()
	var uv2s := PackedVector2Array()
	var colors := PackedColorArray()
	var indices := PackedInt32Array()
	for y in PANEL_CELLS.y + 1:
		for x in PANEL_CELLS.x + 1:
			var fx := float(x) / PANEL_CELLS.x
			var fy := float(y) / PANEL_CELLS.y
			positions.append(Vector3((fx - 0.5) * PANEL_SIZE.x, (fy - 0.5) * PANEL_SIZE.y, 0.0))
			normals.append(Vector3(0, 0, 1))
			tangents.append_array(PackedFloat32Array([1.0, 0.0, 0.0, 1.0]))
			uvs.append(Vector2(fx * PANEL_SIZE.x, (1.0 - fy) * PANEL_SIZE.y))
			uv2s.append(Vector2(fx * PANEL_SIZE.x * 0.5 + 0.25, (1.0 - fy) * PANEL_SIZE.y * 0.5 + 0.25))
			colors.append(Color(0.2 + 0.6 * fx, 0.3 + 0.5 * fy, 0.8 - 0.5 * fx * fy, 1.0))
	for y in PANEL_CELLS.y:
		for x in PANEL_CELLS.x:
			var i := y * (PANEL_CELLS.x + 1) + x
			var j := i + PANEL_CELLS.x + 1
			# Godot's front faces wind clockwise as seen from +Z.
			indices.append_array(PackedInt32Array([i, j, i + 1, i + 1, j, j + 1]))
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = positions
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TANGENT] = tangents
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_TEX_UV2] = uv2s
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, p_flags)
	return mesh


# Smooth patterns (but the mask's alpha): neighboring texels differ little, so a UV that lands a little
# off samples the same values the raster's UVs do.
func _texture(p_kind: String, p_seed: int, p_mipmaps: bool) -> Texture2D:
	var key := "%s_%d_%s" % [p_kind, p_seed, p_mipmaps]
	if _textures.has(key):
		return _textures[key]
	var image := Image.create_empty(TEXTURE_SIZE, TEXTURE_SIZE, false, Image.FORMAT_RGBA8)
	var phase := float(p_seed) * 0.37
	for y in TEXTURE_SIZE:
		for x in TEXTURE_SIZE:
			var u := TAU * float(x) / TEXTURE_SIZE
			var v := TAU * float(y) / TEXTURE_SIZE
			var c := Color()
			match p_kind:
				"albedo":
					c = Color(0.5 + 0.4 * sin(u + phase), 0.5 + 0.4 * cos(v * 2.0 + phase), 0.5 + 0.4 * sin(u + v + phase), 1.0)
				"normal":
					c = Color(0.5 + 0.25 * sin(u * 2.0 + phase), 0.5 + 0.25 * cos(v + phase), 1.0, 1.0)
				"gray":
					var g := 0.5 + 0.45 * sin(u + v * 0.5 + phase)
					c = Color(g, g, g, 1.0)
				"cutout":
					var a := 0.5 + 0.5 * sin(u + phase) * cos(v)
					c = Color(0.3 + 0.4 * cos(v + phase), 0.6, 0.4 + 0.3 * sin(u), a)
				"mask":
					# Alpha 0 or 1 only: the hash keeps or cuts a texel whatever its threshold.
					var keep := sin(u * 2.0 + phase) * cos(v * 2.0) > -0.2
					c = Color(0.3 + 0.4 * cos(v + phase), 0.5 + 0.3 * sin(u), 0.6, 1.0 if keep else 0.0)
			image.set_pixel(x, y, c)
	if p_mipmaps:
		image.generate_mipmaps()
	var texture := ImageTexture.create_from_image(image)
	_textures[key] = texture
	return texture


func _materials() -> Array[Material]:
	var list: Array[Material] = []
	var uv_scales := [1.0, 2.0, 4.0, 8.0]
	for i in _material_count:
		var kind := i % KINDS if _kinds.is_empty() else _kinds[i % _kinds.size()]
		var m: Material
		if kind < STANDARD_KINDS:
			m = _standard_material(kind, i, _uv_scale if _uv_scale > 0.0 else uv_scales[(i / KINDS) % 4])
		else:
			m = _shader_material(kind - STANDARD_KINDS, i)
		list.append(m)
	# The materials the hit groups refuse.
	for code in _refused_shaders():
		var refused := ShaderMaterial.new()
		refused.shader = Shader.new()
		refused.shader.code = code
		if code.contains("instance uniform"):
			refused.set_meta("instance_tint", Color(0.5, 0.2, 0.9))
		list.append(refused)
	# GodotNext: nextgi_hit_discard (#304). The discard panels: each discard shader on DISCARD_PANELS / 2 materials.
	var discard_codes := _discard_shaders()
	for i in DISCARD_PANELS:
		var key := "discard_%d" % (i % discard_codes.size())
		if not _shaders.has(key):
			var shader := Shader.new()
			shader.code = discard_codes[i % discard_codes.size()]
			_shaders[key] = shader
		var m := ShaderMaterial.new()
		m.shader = _shaders[key]
		m.set_shader_parameter("tint", Color.from_hsv(0.15 + 0.2 * i, 0.6, 0.9))
		list.append(m)
	return list


func _standard_material(p_kind: int, p_index: int, p_uv_scale: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	var hue := fmod(float(p_index) * 0.137, 1.0)
	m.albedo_color = Color.from_hsv(hue, 0.6, 0.9)
	m.roughness = 0.1 + 0.8 * fmod(float(p_index) * 0.31, 1.0)
	m.metallic = fmod(float(p_index) * 0.53, 1.0)
	m.uv1_scale = Vector3(p_uv_scale, p_uv_scale, 1.0)
	var mipmaps := p_index % 3 != 0
	match p_kind:
		0, 1:
			pass # Color only (two feature sets: with and without specular tweaks).
		2, 3:
			m.albedo_texture = _texture("albedo", p_index % 7, mipmaps)
		4, 5:
			m.albedo_texture = _texture("albedo", p_index % 5, mipmaps)
			m.normal_enabled = true
			m.normal_texture = _texture("normal", p_index % 4, mipmaps)
			m.normal_scale = 0.5 + float(p_index % 3) * 0.5
		6, 7:
			m.albedo_texture = _texture("albedo", p_index % 6, mipmaps)
			m.normal_enabled = true
			m.normal_texture = _texture("normal", p_index % 3, mipmaps)
			m.roughness_texture = _texture("gray", p_index % 4, mipmaps)
			m.metallic_texture = _texture("gray", (p_index + 2) % 4, mipmaps)
		8:
			m.vertex_color_use_as_albedo = true
		9:
			m.albedo_texture = _texture("cutout", p_index % 4, mipmaps)
			m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
			m.alpha_scissor_threshold = 0.5
			m.nextgi_rt_treat_as_opaque = true
		10:
			m.emission_enabled = true
			m.emission = Color.from_hsv(hue, 0.8, 1.0)
			m.emission_energy_multiplier = 2.0
		11:
			m.albedo_texture = _texture("albedo", p_index % 3, mipmaps)
			m.vertex_color_use_as_albedo = true
		12:
			m.albedo_texture = _texture("mask", p_index % 4, false)
			m.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
			m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_HASH
			m.nextgi_rt_treat_as_opaque = true
			# Nearest texels: at UV scale 1 the card texels sample a quarter texel in from the texel edges (at 4 and
			# 8 they would sit on the edges, where either neighbor is right).
			m.uv1_scale = Vector3(1.0, 1.0, 1.0)
	if p_kind == 1:
		m.metallic_specular = 0.3
	return m


func _shader_material(p_kind: int, p_index: int) -> ShaderMaterial:
	var key := "shader_%d" % p_kind
	if not _shaders.has(key):
		var shader := Shader.new()
		shader.code = _shader_code(p_kind)
		_shaders[key] = shader
	var m := ShaderMaterial.new()
	m.shader = _shaders[key]
	var hue := fmod(float(p_index) * 0.219, 1.0)
	m.set_shader_parameter("tint", Color.from_hsv(hue, 0.5, 0.95))
	m.set_shader_parameter("rough", 0.15 + 0.7 * fmod(float(p_index) * 0.41, 1.0))
	m.set_shader_parameter("metal", fmod(float(p_index) * 0.29, 1.0))
	m.set_shader_parameter("tex", _texture("albedo", p_index % 5, true))
	m.set_shader_parameter("uv_scale", 1.0 + float(p_index % 3))
	return m


func _shader_code(p_kind: int) -> String:
	var common := """
uniform vec4 tint : source_color = vec4(1.0);
uniform float rough = 0.5;
uniform float metal = 0.0;
uniform float uv_scale = 1.0;
uniform sampler2D tex : source_color, filter_linear_mipmap, repeat_enable;
"""
	match p_kind:
		0:
			return "shader_type spatial;\n" + common + """
varying vec2 v_detail;

float shaped(float x) {
	return x * 0.5 + 0.25;
}

void vertex() {
	UV = UV * uv_scale;
	v_detail = vec2(shaped(UV.x), shaped(UV.y));
}

void fragment() {
	ALBEDO = texture(tex, UV).rgb * tint.rgb * shaped(1.0) + vec3(v_detail * 0.25, 0.0);
	ROUGHNESS = rough;
	METALLIC = metal;
}
"""
		1:
			# A texture() bias, textureProj() with a bias, and a custom light() with a helper only light() uses,
			# which reads a derivative (hits never run light(), so neither refuses the shader).
			return "shader_type spatial;\n" + common + """
float light_only_falloff(float x) {
	return x + dFdx(x) * 0.0;
}

void fragment() {
	vec3 plain = texture(tex, UV * uv_scale, 0.0).rgb;
	vec3 projected = textureProj(tex, vec3(UV * uv_scale * 2.0, 2.0), 0.0).rgb;
	ALBEDO = (plain + projected) * 0.5 * tint.rgb;
	ROUGHNESS = rough;
	METALLIC = metal;
}

void light() {
	DIFFUSE_LIGHT += ATTENUATION * LIGHT_COLOR * light_only_falloff(max(dot(NORMAL, LIGHT), 0.0)) * ALBEDO;
}
"""
		2:
			return "shader_type spatial;\n" + common + """
void fragment() {
	ALBEDO = COLOR.rgb * tint.rgb * texture(tex, UV2).rgb;
	ROUGHNESS = rough * COLOR.g;
	METALLIC = metal;
}
"""
		4:
			# texture() in vertex(): the rasterizer samples level 0 there.
			return "shader_type spatial;\n" + common + """
void vertex() {
	COLOR = texture(tex, UV * 0.5 + vec2(0.25));
}

void fragment() {
	ALBEDO = COLOR.rgb * tint.rgb;
	ROUGHNESS = rough;
	METALLIC = metal;
}
"""
		5:
			# Double-sided, with vertex normals that point away from the winding's front face: the side check
			# follows the winding (gl_FrontFacing), not the normal.
			return "shader_type spatial;\nrender_mode cull_disabled;\n" + common + """
void vertex() {
	NORMAL = normalize(vec3(0.0, 0.6, -0.8));
}

void fragment() {
	ALBEDO = tint.rgb;
	ROUGHNESS = rough;
	METALLIC = metal;
}
"""
		6:
			# Double-sided with ordinary normals (foliage cards).
			return "shader_type spatial;\nrender_mode cull_disabled;\n" + common + """
void fragment() {
	ALBEDO = texture(tex, UV).rgb * tint.rgb;
	ROUGHNESS = rough;
	METALLIC = metal;
}
"""
		_:
			return "shader_type spatial;\nrender_mode world_vertex_coords;\n" + common + """
varying float v_height;

void vertex() {
	v_height = clamp(VERTEX.y * 0.05 + 0.5, 0.0, 1.0);
}

void fragment() {
	ALBEDO = tint.rgb * v_height;
	ROUGHNESS = rough;
	METALLIC = metal;
}
"""


# GodotNext: nextgi_hit_discard (#304). Stripes a quarter of the panel's UV width apart are discarded before the
# albedo is written (the hit must not shade them with what fragment() never wrote); the second shader also cuts bands
# across them with an alpha scissor, so its any-hit stage ignores the intersection at two places. Both are traced as
# opaque (nextgi_rt_opaque), as the alpha-tested StandardMaterial3D kinds are: the groups still get their any-hit stage,
# but no shadow caster asks for the material retrace, which would make the hit groups with the check's setting off.
func _discard_shaders() -> PackedStringArray:
	return PackedStringArray([
		"shader_type spatial;\nrender_mode nextgi_rt_opaque;\nuniform vec4 tint : source_color = vec4(1.0);\nvoid fragment() {\n\tif (fract(UV.x * 4.0) < 0.5) {\n\t\tdiscard;\n\t}\n\tALBEDO = tint.rgb;\n\tROUGHNESS = 0.6;\n}\n",
		"shader_type spatial;\nrender_mode nextgi_rt_opaque;\nuniform vec4 tint : source_color = vec4(1.0);\nvoid fragment() {\n\tALBEDO = tint.rgb;\n\tROUGHNESS = 0.4;\n\tALPHA = fract(UV.y * 8.0) < 0.5 ? 0.0 : 1.0;\n\tALPHA_SCISSOR_THRESHOLD = 0.5;\n\tif (fract(UV.x * 4.0) < 0.5) {\n\t\tdiscard;\n\t}\n}\n",
	])


func _refused_shaders() -> PackedStringArray:
	return PackedStringArray([
		"shader_type spatial;\nvoid fragment() {\n\tALBEDO = vec3(abs(dFdx(UV.x)) * 100.0);\n}\n",
		"shader_type spatial;\nvoid fragment() {\n\tALBEDO = FRONT_FACING ? vec3(1.0, 0.2, 0.2) : vec3(0.2);\n}\n",
		"shader_type spatial;\ninstance uniform vec4 instance_tint : source_color = vec4(1.0);\nvoid fragment() {\n\tALBEDO = instance_tint.rgb;\n}\n",
		"shader_type spatial;\nuniform samplerCube sky_tex;\nvoid fragment() {\n\tALBEDO = texture(sky_tex, NORMAL).rgb + vec3(0.2);\n}\n",
		"shader_type spatial;\nuniform sampler2D screen_tex : hint_screen_texture;\nvoid fragment() {\n\tALBEDO = texture(screen_tex, SCREEN_UV).rgb;\n}\n",
	])
