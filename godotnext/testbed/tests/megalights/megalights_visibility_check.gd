extends Node3D
## #146: MegaLights visibility chain GPU verification (plan §21.3 contract M4, GM-1, GM-8, GM-12, budget M-4).
## Verifies on GPU with Vulkan validation (VVL + SyncVal) enabled:
##  1. tier_and_legacy_off: megalights_get_tier_info() reports HWRT tier cleanly; when MegaLights is off,
##     consecutive frames are byte-identical and allocate 0 MegaLights bytes.
##  2. screen_trace_and_dedupe: Hi-Z screen trace + 2x2 block ray deduplication + stream-compacted near-field
##     trace_shadow() matches brute-force trace_shadow() for >= 99.5% of samples (screen-trace false occlusion
##     <= 0.5%), writes R16F hit distances, feeds visibility_buffer into ml_guiding.glsl, and measurably reduces
##     traced rays in 2x2 blocks.
##  3. alpha_and_retrace: alpha-scissor textured caster (#145 bindless alpha) and custom spatial shader discard
##     caster (#130 material retrace) produce matching shadows (>= 99.5% match vs brute-force) with retrace active.
##  4. vsm_method_light: DirectionalLight3D with megalights_shadow_method = VIRTUAL skips ray queries (ray_count == 0)
##     and evaluates shadows via vsm_shadow() (SMRT 2 rays) with real shadow contrast and hit distances.
##  5. swrt_continuation: optional non-RT SWRT global-SDF continuation (#182, ml_trace_rays_swrt.glsl) traces
##     real occlusion and hit distances without HWRT ray queries.
##  6. far_field_continuation: optional #126 far-field TLAS continuation traces static casters outside
##     rt_scene_radius when enabled and skips them when disabled (default off).

const VIEW_SIZE := Vector2i(128, 128)


func _ready() -> void:
	get_tree().root.disable_3d = true
	_run()


func _arg(key: String, fallback: String) -> String:
	var prefix := "--" + key + "="
	for a in OS.get_cmdline_user_args():
		if a.begins_with(prefix):
			return a.substr(prefix.length())
	return fallback


func _check(failures: PackedStringArray, cond: bool, msg: String) -> void:
	if not cond:
		failures.append(msg)


func _run() -> void:
	var mode := _arg("ml-vis-check", "all")
	var failures := PackedStringArray()
	var report := {
		"test": "megalights_visibility_chain",
		"mode": mode,
		"pass": false,
	}

	if mode == "all" or mode == "tier_and_legacy_off":
		report["tier_and_legacy_off"] = await _check_tier_and_legacy_off(failures)
	if mode == "all" or mode == "screen_trace_and_dedupe":
		report["screen_trace_and_dedupe"] = await _check_screen_trace_and_dedupe(failures)
	if mode == "all" or mode == "alpha_and_retrace":
		report["alpha_and_retrace"] = await _check_alpha_and_retrace(failures)
	if mode == "all" or mode == "vsm_method_light":
		report["vsm_method_light"] = await _check_vsm_method_light(failures)
	if mode == "all" or mode == "swrt_continuation":
		report["swrt_continuation"] = await _check_swrt_continuation(failures)
	if mode == "all" or mode == "far_field_continuation":
		report["far_field_continuation"] = await _check_far_field_continuation(failures)

	report["failures"] = failures
	report["pass"] = failures.is_empty()
	print("MEGALIGHTS_VISIBILITY_RESULT=" + JSON.stringify(report))
	get_tree().quit(0 if failures.is_empty() else 1)


func _make_viewport(size: Vector2i, enable_nextgi: bool) -> SubViewport:
	var sub := SubViewport.new()
	sub.own_world_3d = true
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	sub.msaa_3d = Viewport.MSAA_DISABLED
	sub.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	sub.positional_shadow_atlas_size = 0
	sub.size = size
	add_child(sub)

	var world := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.nextgi_enabled = enable_nextgi
	world.environment = env
	sub.add_child(world)
	return sub


func _add_box(parent: Node, pos: Vector3, size: Vector3, albedo: Color, roughness: float = 0.7) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	var mat := StandardMaterial3D.new()
	mat.albedo_color = albedo
	mat.roughness = roughness
	mat.metallic = 0.0
	box.material = mat
	mi.mesh = box
	mi.position = pos
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	parent.add_child(mi)
	return mi


func _run_gpu_check(options: Dictionary, max_wait_frames: int = 180) -> Dictionary:
	for _w in range(3):
		await RenderingServer.frame_post_draw
	RenderingServer.megalights_request_check(options)
	var res := {}
	for _i in range(max_wait_frames):
		await RenderingServer.frame_post_draw
		res = RenderingServer.megalights_get_check_result()
		if bool(res.get("done", false)):
			return res
	res["error"] = "Timed out waiting for MegaLights visibility GPU check."
	return res


func _check_tier_and_legacy_off(failures: PackedStringArray) -> Dictionary:
	var tier_info := RenderingServer.megalights_get_tier_info()
	var is_metal := RenderingServer.get_current_rendering_driver_name() == "metal"
	var expected_tier := "HWRT_INLINE_ONLY" if is_metal else "HWRT"
	_check(failures, str(tier_info.get("tier", "")) == expected_tier,
			"tier_info: expected %s tier, got %s" % [expected_tier, str(tier_info.get("tier", ""))])
	_check(failures, bool(tier_info.get("experimental", not is_metal)) == is_metal,
			"tier_info: unexpected experimental flag %s for driver %s" % [str(tier_info.get("experimental", false)), RenderingServer.get_current_rendering_driver_name()])
	if not is_metal:
		_check(failures, bool(tier_info.get("retrace_supported", false)),
				"tier_info: expected retrace_supported=true on Vulkan/D3D12 HWRT")
	else:
		_check(failures, not bool(tier_info.get("retrace_supported", true)),
				"tier_info: expected retrace_supported=false on Metal inline ray query tier")
	_check(failures, bool(tier_info.get("vsm_supported", false)),
			"tier_info: expected vsm_supported=true on Forward+")
	_check(failures, bool(tier_info.get("screen_traces_enabled", false)),
			"tier_info: expected screen_traces_enabled default true")

	var sub := _make_viewport(VIEW_SIZE, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 6.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.5, 0.0), Vector3.UP)
	cam.current = true
	var probe := MlAllocProbe.new()
	var comp := Compositor.new()
	comp.compositor_effects = [probe]
	cam.compositor = comp
	sub.add_child(cam)

	_add_box(sub, Vector3(0.0, 1.5, -2.0), Vector3(10.0, 8.0, 0.5), Color(0.75, 0.75, 0.75))
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 2.0, 1.0)
	omni.omni_range = 12.0
	omni.light_energy = 2.0
	omni.shadow_enabled = false
	sub.add_child(omni)

	for _i in range(4):
		await RenderingServer.frame_post_draw
	var img1 := sub.get_texture().get_image()
	await RenderingServer.frame_post_draw
	var img2 := sub.get_texture().get_image()

	var bytes_equal := img1 != null and img2 != null and img1.get_data() == img2.get_data()
	_check(failures, bytes_equal, "legacy_off: consecutive frames were not byte-identical")
	_check(failures, probe.calls > 0, "legacy_off: compositor probe did not execute")
	_check(failures, probe.memory_bytes == 0 and not probe.has_albedo and not probe.has_material,
			"legacy_off: MegaLights buffers allocated while inactive (bytes=%d)" % probe.memory_bytes)

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"tier_info": tier_info,
		"bytes_equal": bytes_equal,
		"probe_calls": probe.calls,
		"memory_bytes": probe.memory_bytes,
	}


func _check_screen_trace_and_dedupe(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, true)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Large flat receiver wall + floor (high 2x2 coherence) + near contact occluders + mid-room pillar.
	_add_box(sub, Vector3(0.0, 2.0, -2.5), Vector3(14.0, 8.0, 0.5), Color(0.8, 0.8, 0.8))
	_add_box(sub, Vector3(0.0, -0.25, 0.0), Vector3(14.0, 0.5, 10.0), Color(0.75, 0.75, 0.75))
	# Contact-shadow block sitting directly against the wall and floor (hit by short Hi-Z screen traces).
	_add_box(sub, Vector3(-1.4, 0.6, -1.95), Vector3(0.7, 1.2, 0.6), Color(0.5, 0.5, 0.55))
	_add_box(sub, Vector3(1.5, 0.5, -1.9), Vector3(0.6, 1.0, 0.6), Color(0.55, 0.5, 0.5))
	# Mid-room pillar casting long shadows (resolved by near-field HWRT ray queries).
	_add_box(sub, Vector3(0.1, 1.2, -0.3), Vector3(0.7, 2.4, 0.5), Color(0.6, 0.6, 0.6))

	# 4 Omni + 2 Spot + 2 Area lights (all casting shadows; coherent 2x2 light picks for dedupe).
	for i in range(4):
		var omni := OmniLight3D.new()
		omni.position = Vector3(-2.4 + float(i) * 1.6, 2.6 + float(i % 2) * 0.6, 2.0)
		omni.omni_range = 18.0
		omni.light_energy = 2.4
		omni.shadow_enabled = true
		sub.add_child(omni)

	for i in range(2):
		var spot := SpotLight3D.new()
		spot.position = Vector3(-2.0 + float(i) * 4.0, 3.2, 2.4)
		spot.look_at_from_position(spot.position, Vector3(0.0, 0.8, -2.2), Vector3.UP)
		spot.spot_range = 18.0
		spot.spot_angle = 60.0
		spot.light_energy = 3.0
		spot.shadow_enabled = true
		sub.add_child(spot)

	for i in range(2):
		var area := AreaLight3D.new()
		area.position = Vector3(-1.2 + float(i) * 2.4, 3.0, 1.8)
		area.look_at_from_position(area.position, Vector3(0.0, 0.8, -2.2), Vector3.UP)
		area.area_size = Vector2(0.4, 0.4)
		area.area_range = 18.0
		area.light_energy = 2.5
		area.shadow_enabled = true
		sub.add_child(area)

	# 1. Screen trace ON, dedupe OFF vs brute-force trace_shadow() -> isolates screen-trace accuracy!
	var res_st := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 4,
		"falloff_cull_threshold": 0.0,
		"screen_traces": true,
		"dedupe": false,
		"compare_bruteforce": true,
		"wait_for_tlas": true,
	})
	_check(failures, bool(res_st.get("done", false)) and not res_st.has("error"),
			"screen_trace_only: check failed (%s)" % str(res_st.get("error", "")))
	_check(failures, bool(res_st.get("tlas_ready", false)), "screen_trace_only: TLAS not ready")
	_check(failures, int(res_st.get("valid_samples", 0)) > 10000,
			"screen_trace_only: expected > 10000 valid samples, got %d" % int(res_st.get("valid_samples", 0)))
	_check(failures, int(res_st.get("screen_hit_count", 0)) > 0,
			"screen_trace_only: expected screen_hit_count > 0")
	_check(failures, int(res_st.get("visible_samples", 0)) > 0 and int(res_st.get("occluded_samples", 0)) > 0,
			"screen_trace_only: expected both visible and occluded samples")
	_check(failures, int(res_st.get("finite_hit_dist_count", 0)) > 0,
			"screen_trace_only: expected finite R16F hit distances > 0")
	_check(failures, int(res_st.get("guiding_non_empty_tiles", 0)) > 0 and int(res_st.get("guiding_visible_sum", 0)) > 0,
			"screen_trace_only: visibility_buffer did not populate ml_guiding tiles")
	_check(failures, float(res_st.get("match_rate", 0.0)) >= 0.995,
			"screen_trace_only: match_rate %.6f < 0.995" % float(res_st.get("match_rate", 0.0)))
	_check(failures, float(res_st.get("false_occlusion_rate", 1.0)) <= 0.005,
			"screen_trace_only: false_occlusion_rate %.6f > 0.005" % float(res_st.get("false_occlusion_rate", 1.0)))

	# 2. Screen trace OFF, dedupe ON vs brute-force -> isolates 2x2 block ray deduplication!
	var res_dedupe := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 4,
		"falloff_cull_threshold": 0.0,
		"screen_traces": false,
		"dedupe": true,
		"compare_bruteforce": true,
		"wait_for_tlas": true,
	})
	_check(failures, bool(res_dedupe.get("done", false)) and not res_dedupe.has("error"),
			"dedupe_only: check failed (%s)" % str(res_dedupe.get("error", "")))
	_check(failures, int(res_dedupe.get("deduped_count", 0)) > 0,
			"dedupe_only: expected deduped_count > 0 in 2x2 blocks")
	_check(failures, int(res_dedupe.get("ray_count", 0)) < int(res_dedupe.get("brute_ray_count", 0)),
			"dedupe_only: expected ray_count (%d) < brute_ray_count (%d)" % [int(res_dedupe.get("ray_count", 0)), int(res_dedupe.get("brute_ray_count", 0))])
	_check(failures, float(res_dedupe.get("ray_reduction_fraction", 0.0)) > 0.05,
			"dedupe_only: expected ray_reduction_fraction > 0.05, got %.4f" % float(res_dedupe.get("ray_reduction_fraction", 0.0)))
	_check(failures, float(res_dedupe.get("match_rate", 0.0)) >= 0.995,
			"dedupe_only: match_rate %.6f < 0.995" % float(res_dedupe.get("match_rate", 0.0)))

	# 3. Full combined chain: Screen trace ON + 2x2 dedupe ON vs brute-force.
	var res_full := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 4,
		"falloff_cull_threshold": 0.0,
		"screen_traces": true,
		"dedupe": true,
		"compare_bruteforce": true,
		"wait_for_tlas": true,
	})
	_check(failures, bool(res_full.get("done", false)) and not res_full.has("error"),
			"full_chain: check failed (%s)" % str(res_full.get("error", "")))
	_check(failures, int(res_full.get("screen_hit_count", 0)) > 0 and int(res_full.get("deduped_count", 0)) > 0,
			"full_chain: expected both screen hits and 2x2 deduped rays")
	_check(failures, float(res_full.get("match_rate", 0.0)) >= 0.995,
			"full_chain: match_rate %.6f < 0.995" % float(res_full.get("match_rate", 0.0)))
	_check(failures, float(res_full.get("false_occlusion_rate", 1.0)) <= 0.005,
			"full_chain: false_occlusion_rate %.6f > 0.005" % float(res_full.get("false_occlusion_rate", 1.0)))

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"screen_trace_only": res_st,
		"dedupe_only": res_dedupe,
		"full_chain": res_full,
	}


func _check_alpha_and_retrace(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, true)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 1.5, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.2, -3.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Large bright receiver wall behind the two cutout casters.
	_add_box(sub, Vector3(0.0, 1.5, -3.25), Vector3(14.0, 9.0, 0.5), Color(0.85, 0.85, 0.85))

	# 1. Alpha-scissor textured caster (#145 bindless alpha path): opaque outer ring, transparent center hole.
	var alpha_img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var hole := (x >= 16 and x < 48 and y >= 16 and y < 48)
			alpha_img.set_pixel(x, y, Color(0.6, 0.6, 0.6, 0.0 if hole else 1.0))
	var alpha_tex := ImageTexture.create_from_image(alpha_img)
	var alpha_mat := StandardMaterial3D.new()
	alpha_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	alpha_mat.alpha_scissor_threshold = 0.5
	alpha_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	alpha_mat.albedo_texture = alpha_tex
	alpha_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST

	var alpha_mi := MeshInstance3D.new()
	var alpha_quad := QuadMesh.new()
	alpha_quad.size = Vector2(2.2, 2.2)
	alpha_mi.mesh = alpha_quad
	alpha_mi.material_override = alpha_mat
	alpha_mi.position = Vector3(-1.35, 1.5, -0.8)
	alpha_mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	sub.add_child(alpha_mi)

	# 2. Custom spatial shader discard caster (#130 material retrace path): opaque ring, discard center hole.
	var retrace_shader := Shader.new()
	retrace_shader.code = """shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(0.55, 0.55, 0.65);
	ROUGHNESS = 0.8;
	if (UV.x > 0.25 && UV.x < 0.75 && UV.y > 0.25 && UV.y < 0.75) {
		discard;
	}
}
"""
	var retrace_mat := ShaderMaterial.new()
	retrace_mat.shader = retrace_shader
	var retrace_mi := MeshInstance3D.new()
	var retrace_quad := QuadMesh.new()
	retrace_quad.size = Vector2(2.2, 2.2)
	retrace_mi.mesh = retrace_quad
	retrace_mi.material_override = retrace_mat
	retrace_mi.position = Vector3(1.35, 1.5, -0.8)
	retrace_mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	sub.add_child(retrace_mi)

	# Point light in front of both casters shining toward the wall.
	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 1.5, 2.2)
	omni.omni_range = 16.0
	omni.light_energy = 4.0
	omni.shadow_enabled = true
	sub.add_child(omni)

	var expect_retrace := bool(RenderingServer.megalights_get_tier_info().get("retrace_supported", false))

	# Run with screen_traces=false first to guarantee rays hit both alpha & retrace casters cleanly.
	var res_rt := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"falloff_cull_threshold": 0.0,
		"screen_traces": false,
		"dedupe": false,
		"compare_bruteforce": true,
		"wait_for_tlas": true,
		"wait_for_retrace": expect_retrace,
	}, 240)
	_check(failures, bool(res_rt.get("done", false)) and not res_rt.has("error"),
			"alpha_retrace_rt: check failed (%s)" % str(res_rt.get("error", "")))
	if expect_retrace:
		_check(failures, bool(res_rt.get("retrace_ran", false)),
				"alpha_retrace_rt: expected #130 material retrace to execute")
		_check(failures, int(res_rt.get("retrace_appended_count", 0)) > 0,
				"alpha_retrace_rt: expected retrace_appended_count > 0")
	else:
		_check(failures, not bool(res_rt.get("retrace_ran", true)),
				"alpha_retrace_rt: unexpected retrace on inline-only tier")
	_check(failures, int(res_rt.get("visible_samples", 0)) > 0 and int(res_rt.get("occluded_samples", 0)) > 0,
			"alpha_retrace_rt: expected both lit holes and occluded rings")
	_check(failures, float(res_rt.get("match_rate", 0.0)) >= 0.995,
			"alpha_retrace_rt: match_rate %.6f < 0.995" % float(res_rt.get("match_rate", 0.0)))

	# Also verify full chain (screen_traces=true + dedupe=true) on alpha/retrace scene.
	var res_full := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"falloff_cull_threshold": 0.0,
		"screen_traces": true,
		"dedupe": true,
		"compare_bruteforce": true,
		"wait_for_tlas": true,
		"wait_for_retrace": expect_retrace,
	}, 240)
	_check(failures, bool(res_full.get("done", false)) and not res_full.has("error"),
			"alpha_retrace_full: check failed (%s)" % str(res_full.get("error", "")))
	_check(failures, float(res_full.get("match_rate", 0.0)) >= 0.995,
			"alpha_retrace_full: match_rate %.6f < 0.995" % float(res_full.get("match_rate", 0.0)))

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"rt_only": res_rt,
		"full_chain": res_full,
	}


func _check_vsm_method_light(failures: PackedStringArray) -> Dictionary:
	var sub := _make_viewport(VIEW_SIZE, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 6.0, 7.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, 0.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Matte receiver floor + opaque blocker casting clear VSM shadow across center of floor.
	_add_box(sub, Vector3(0.0, -0.25, 0.0), Vector3(16.0, 0.5, 16.0), Color(0.85, 0.85, 0.85))
	_add_box(sub, Vector3(-1.5, 1.5, 0.0), Vector3(1.8, 2.2, 2.6), Color(0.5, 0.5, 0.55))

	var sun := DirectionalLight3D.new()
	sun.light_energy = 2.0
	sun.light_color = Color.WHITE
	sun.shadow_enabled = true
	sun.light_angular_distance = 0.5
	sun.megalights_shadow_method = Light3D.MEGALIGHTS_SHADOW_METHOD_VIRTUAL
	sub.add_child(sun)
	sun.look_at_from_position(Vector3(-4.0, 5.0, 0.0), Vector3(0.0, 0.0, 0.0), Vector3.UP)

	# Screen traces OFF -> 100% of directional samples route to ml_trace_vsm.glsl (SMRT 2 rays), 0 ray queries!
	var res_vsm := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"directional_lights": true,
		"falloff_cull_threshold": 0.0,
		"screen_traces": false,
		"dedupe": false,
		"compare_bruteforce": false,
		"wait_for_tlas": false,
		"wait_for_vsm": true,
	}, 240)
	_check(failures, bool(res_vsm.get("done", false)) and not res_vsm.has("error"),
			"vsm_method_light: check failed (%s)" % str(res_vsm.get("error", "")))
	_check(failures, bool(res_vsm.get("vsm_ready", false)),
			"vsm_method_light: VSM did not activate on demand for VIRTUAL light")
	_check(failures, int(res_vsm.get("vsm_count", 0)) > 0,
			"vsm_method_light: expected vsm_count > 0")
	_check(failures, int(res_vsm.get("ray_count", -1)) == 0,
			"vsm_method_light: expected 0 ray-query rays for VIRTUAL light, got %d" % int(res_vsm.get("ray_count", -1)))
	_check(failures, int(res_vsm.get("visible_samples", 0)) > 0 and int(res_vsm.get("occluded_samples", 0)) > 0,
			"vsm_method_light: expected real VSM shadow contrast (vis=%d occ=%d)" % [int(res_vsm.get("visible_samples", 0)), int(res_vsm.get("occluded_samples", 0))])
	_check(failures, int(res_vsm.get("finite_hit_dist_count", 0)) > 0,
			"vsm_method_light: expected finite hit distances from VSM transmittance")

	# Also verify with screen_traces=true: still 0 ray queries!
	var res_vsm_st := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"directional_lights": true,
		"falloff_cull_threshold": 0.0,
		"screen_traces": true,
		"dedupe": true,
		"compare_bruteforce": false,
		"wait_for_tlas": false,
		"wait_for_vsm": true,
	}, 240)
	_check(failures, bool(res_vsm_st.get("done", false)) and not res_vsm_st.has("error"),
			"vsm_method_light_st: check failed (%s)" % str(res_vsm_st.get("error", "")))
	_check(failures, int(res_vsm_st.get("ray_count", -1)) == 0,
			"vsm_method_light_st: expected 0 ray-query rays with screen_traces=true, got %d" % int(res_vsm_st.get("ray_count", -1)))
	_check(failures, int(res_vsm_st.get("vsm_count", 0)) > 0,
			"vsm_method_light_st: expected vsm_count > 0 with screen_traces=true")

	sub.queue_free()
	await RenderingServer.frame_post_draw
	return {
		"vsm_only": res_vsm,
		"vsm_with_screen_traces": res_vsm_st,
	}


func _check_swrt_continuation(failures: PackedStringArray) -> Dictionary:
	var prev_gen := bool(ProjectSettings.get_setting("rendering/global_illumination/mesh_sdf/generate", false))
	var prev_keep := bool(ProjectSettings.get_setting("rendering/global_illumination/mesh_sdf/keep_when_hwrt", false))
	ProjectSettings.set_setting("rendering/global_illumination/mesh_sdf/generate", true)
	ProjectSettings.set_setting("rendering/global_illumination/mesh_sdf/keep_when_hwrt", true)

	var sub := _make_viewport(VIEW_SIZE, false)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 5.5)
	cam.look_at_from_position(cam.position, Vector3(0.0, 1.2, -2.5), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Receiver wall + thick static occluder box whose mesh SDF is generated on upload.
	_add_box(sub, Vector3(0.0, 1.5, -2.8), Vector3(14.0, 8.0, 0.8), Color(0.8, 0.8, 0.8))
	_add_box(sub, Vector3(0.0, 1.4, -0.4), Vector3(1.6, 2.2, 0.8), Color(0.5, 0.55, 0.6))

	var omni := OmniLight3D.new()
	omni.position = Vector3(0.0, 2.0, 2.4)
	omni.omni_range = 18.0
	omni.light_energy = 3.0
	omni.shadow_enabled = true
	sub.add_child(omni)

	# Trace via ml_trace_rays_swrt.glsl (global SDF clipmap, no ray queries).
	var res_swrt := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"falloff_cull_threshold": 0.0,
		"screen_traces": false,
		"dedupe": false,
		"swrt": true,
		"compare_bruteforce": false,
		"wait_for_tlas": false,
		"wait_for_swrt": true,
	}, 240)
	_check(failures, bool(res_swrt.get("done", false)) and not res_swrt.has("error"),
			"swrt_continuation: check failed (%s)" % str(res_swrt.get("error", "")))
	_check(failures, bool(res_swrt.get("swrt_ready", false)),
			"swrt_continuation: global SDF SWRT did not become ready")
	_check(failures, int(res_swrt.get("ray_count", 0)) > 0,
			"swrt_continuation: expected ray_count > 0")
	_check(failures, int(res_swrt.get("visible_samples", 0)) > 0 and int(res_swrt.get("occluded_samples", 0)) > 0,
			"swrt_continuation: expected real global-SDF shadow contrast (vis=%d occ=%d)" % [int(res_swrt.get("visible_samples", 0)), int(res_swrt.get("occluded_samples", 0))])
	_check(failures, int(res_swrt.get("finite_hit_dist_count", 0)) > 0,
			"swrt_continuation: expected finite hit distances from global SDF")

	# Also run with screen_traces=true + dedupe=true + swrt=true (hybrid screen + SWRT continuation).
	var res_swrt_st := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"falloff_cull_threshold": 0.0,
		"screen_traces": true,
		"dedupe": true,
		"swrt": true,
		"compare_bruteforce": false,
		"wait_for_tlas": false,
		"wait_for_swrt": true,
	}, 240)
	_check(failures, bool(res_swrt_st.get("done", false)) and not res_swrt_st.has("error"),
			"swrt_continuation_st: check failed (%s)" % str(res_swrt_st.get("error", "")))
	_check(failures, int(res_swrt_st.get("visible_samples", 0)) > 0 and int(res_swrt_st.get("occluded_samples", 0)) > 0,
			"swrt_continuation_st: expected shadow contrast with screen_traces+SWRT")

	sub.queue_free()
	await RenderingServer.frame_post_draw
	ProjectSettings.set_setting("rendering/global_illumination/mesh_sdf/generate", prev_gen)
	ProjectSettings.set_setting("rendering/global_illumination/mesh_sdf/keep_when_hwrt", prev_keep)
	return {
		"swrt_only": res_swrt,
		"swrt_with_screen_traces": res_swrt_st,
	}


func _check_far_field_continuation(failures: PackedStringArray) -> Dictionary:
	var prev_near := float(ProjectSettings.get_setting("rendering/global_illumination/nextgi/rt_scene_radius", 100.0))
	var prev_far_dist := float(ProjectSettings.get_setting("rendering/global_illumination/nextgi/far_field_distance", 1000.0))
	var prev_ml_ff := bool(ProjectSettings.get_setting("rendering/megalights/far_field", false))

	# Small near-field radius (12 m) so a static mountain/wall at z=-26 m sits cleanly in the far-field shell.
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/rt_scene_radius", 12.0)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/far_field_distance", 200.0)
	ProjectSettings.set_setting("rendering/megalights/far_field", true)

	var sub := _make_viewport(VIEW_SIZE, true)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 3.5, 5.0)
	cam.look_at_from_position(cam.position, Vector3(0.0, 0.0, -2.0), Vector3.UP)
	cam.current = true
	sub.add_child(cam)

	# Wide matte receiver floor near camera (inside near-field radius).
	_add_box(sub, Vector3(0.0, -0.25, -2.0), Vector3(18.0, 0.5, 10.0), Color(0.85, 0.85, 0.85))

	# Tall static far-field occluder wall strictly beyond rt_scene_radius (12 m), centered in front of the sun.
	var far_wall := _add_box(sub, Vector3(0.0, 12.0, -26.0), Vector3(8.0, 26.0, 2.0), Color(0.5, 0.5, 0.5))
	if far_wall.has_method("set_gi_far_field"):
		far_wall.gi_far_field = GeometryInstance3D.FAR_FIELD_ALWAYS

	# Sun angled from -Z downward toward +Z onto the floor so rays toward the sun pass through z=-26 m.
	var sun := DirectionalLight3D.new()
	sun.light_energy = 2.5
	sun.light_color = Color.WHITE
	sun.shadow_enabled = true
	sub.add_child(sun)
	sun.look_at_from_position(Vector3(0.0, 24.0, -26.0), Vector3(0.0, 0.0, -2.0), Vector3.UP)

	var res_on := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"directional_lights": true,
		"falloff_cull_threshold": 0.0,
		"screen_traces": false,
		"dedupe": false,
		"far_field": true,
		"compare_bruteforce": false,
		"wait_for_tlas": true,
		"wait_for_far_field": true,
	}, 240)
	_check(failures, bool(res_on.get("done", false)) and not res_on.has("error"),
			"far_field_on: check failed (%s)" % str(res_on.get("error", "")))
	_check(failures, bool(res_on.get("far_field_ready", false)),
			"far_field_on: far-field TLAS did not become ready")
	_check(failures, int(res_on.get("far_field_count", 0)) > 0,
			"far_field_on: expected far_field_count > 0")
	_check(failures, int(res_on.get("occluded_samples", 0)) > 0 and int(res_on.get("visible_samples", 0)) > 0,
			"far_field_on: expected far-field wall to cast shadow on center of floor (vis=%d occ=%d)" % [int(res_on.get("visible_samples", 0)), int(res_on.get("occluded_samples", 0))])

	# Now disable far-field continuation on the exact same scene: far wall must NOT occlude!
	var res_off := await _run_gpu_check({
		"mode": "visibility_chain",
		"downsample": "full",
		"samples_per_pixel": 2,
		"directional_lights": true,
		"falloff_cull_threshold": 0.0,
		"screen_traces": false,
		"dedupe": false,
		"far_field": false,
		"compare_bruteforce": false,
		"wait_for_tlas": true,
		"wait_for_far_field": false,
	}, 240)
	_check(failures, bool(res_off.get("done", false)) and not res_off.has("error"),
			"far_field_off: check failed (%s)" % str(res_off.get("error", "")))
	_check(failures, int(res_off.get("far_field_count", -1)) == 0,
			"far_field_off: expected far_field_count == 0 when disabled, got %d" % int(res_off.get("far_field_count", -1)))
	_check(failures, int(res_off.get("occluded_samples", 0)) < int(res_on.get("occluded_samples", 0)),
			"far_field_off: expected fewer occluded samples when far_field=false (off=%d on=%d)" % [int(res_off.get("occluded_samples", 0)), int(res_on.get("occluded_samples", 0))])

	sub.queue_free()
	await RenderingServer.frame_post_draw
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/rt_scene_radius", prev_near)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/far_field_distance", prev_far_dist)
	ProjectSettings.set_setting("rendering/megalights/far_field", prev_ml_ff)
	return {
		"far_field_on": res_on,
		"far_field_off": res_off,
	}


class MlAllocProbe extends CompositorEffect:
	var calls := 0
	var has_albedo := false
	var has_material := false
	var memory_bytes := 0

	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE

	func _render_callback(_callback_type: int, render_data: RenderData) -> void:
		var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
		if buffers == null:
			return
		calls += 1
		has_albedo = buffers.has_texture(&"megalights", &"ml_albedo")
		has_material = buffers.has_texture(&"megalights", &"ml_material")
		memory_bytes = int(buffers.get_context_memory_bytes(&"megalights"))
