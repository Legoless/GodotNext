extends Node
## Native Nanite ray tracing GPU check (#156; plan §21.4 N13, GN-9, N7, MN4).
## rendering/nextgeo/ray_tracing_mode = Native is EXPERIMENTAL and off by default. NextGI runs on
## the hardware tier (run_nextgeo_rt_native_check.sh writes the override):
## 1. Fallback, the default: Concave Courtyard (courtyard_vg.tscn) traces its proxy BLASes, the
##    tier info says "fallback" and carries no rt_native_* key.
## 2-3. Native, the camera swept from far to near through the distance bands: the courtyard
##    enters the TLAS through native parts built from the resident cut, never coarser than the
##    proxy, within the N7 per-frame cap and the MN4 memory cap, and a near band traces a set finer
##    than the proxy. On a band change it keeps its previous native set until the new band's set is
##    built, or refused by MN4; when MN4 has room for the new set only without the previous one, the
##    previous one leaves first and the instance traces the proxy until the new set is built (a
##    reclaim, a frame or two). Its Stage-2 card coverage is reported next to the fallback's.
##    MN4 inside the AS memory line: with the courtyard on its band 1 set, debug/as_budget_bytes
##    drops to the proxies plus half the native bytes. The native cap follows at the next TLAS
##    phase, the set leaves (a cap release) and the courtyard traces its proxy without leaving the
##    hardware tier; with the budget back, the set is cut and built again.
## 4. Streaming: a wavy virtual sheet streams its pages in through a 2 MiB cache; the residency
##    changes rebuild its native set within the cap while the instance stays in the TLAS.
##    Exclusions, on the sheet (its card sets are small; each material change takes new SRC atlas
##    space until #41): a material override or surface material whose vertex() moves VERTEX keeps
##    the proxy; a fragment-only depth write, or an overlay that moves vertices, does not.
## 5. Back to fallback: the native sets and their RT-arena bytes are released.
## Every native frame: N7, MN4 (native build bytes within the cap), native BLAS bytes inside the AS
## line, and no traced set coarser than its mesh's proxy.

const MODE_SETTING := "rendering/nextgeo/ray_tracing_mode"
const NATIVE_BAND_BASE := 4.0 # NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE.
const NATIVE_RETIRE_FRAMES := 4 # NEXTGEO_RT_NATIVE_RETIRE_FRAMES.
const AS_BUDGET_SETTING := "rendering/global_illumination/nextgi/debug/as_budget_bytes"
const CACHE_MB := 2

var _failures := PackedStringArray()
var _temp_files: Array[String] = []


func _ready() -> void:
	var result := await run()
	_cleanup_temp_files()
	ProjectSettings.set_setting(MODE_SETTING, 0)
	ProjectSettings.set_setting(AS_BUDGET_SETTING, 0)
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if bool(result["pass"]) else 1)


func _check(p_condition: bool, p_what: String) -> void:
	if not p_condition:
		_failures.append(p_what)


func _cleanup_temp_files() -> void:
	for path in _temp_files:
		var abs_path := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs_path):
			DirAccess.remove_absolute(abs_path)
	_temp_files.clear()


func _set_native(p_native: bool) -> void:
	ProjectSettings.set_setting(MODE_SETTING, 1 if p_native else 0)


func _has_native_keys(p_info: Dictionary) -> bool:
	for key in p_info.keys():
		if String(key).begins_with("rt_native_"):
			return true
	return false


func run() -> Dictionary:
	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/coverage_counter", true)

	var enabled: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)
	_check(enabled, "nextgi/enabled must be true (run via run_nextgeo_rt_native_check.sh)")
	if not enabled:
		return {"pass": false, "failures": _failures}
	_check(int(ProjectSettings.get_setting(MODE_SETTING, -1)) == 0, "the default ray tracing mode is Fallback")

	var courtyard := await _check_courtyard()
	var streaming := await _check_streaming()

	return {
		"pass": _failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"courtyard_vg": courtyard,
		"streaming": streaming,
		"failures": _failures,
	}


func _prepare_scene(p_root: Node) -> void:
	var stack: Array[Node] = [p_root]
	while not stack.is_empty():
		var cur: Node = stack.pop_back()
		if cur is WorldEnvironment:
			var we := cur as WorldEnvironment
			if we.environment != null:
				var env: Environment = we.environment.duplicate()
				env.nextgi_enabled = true
				we.environment = env
		elif cur is DirectionalLight3D:
			(cur as DirectionalLight3D).shadow_enabled = true
		for child in cur.get_children():
			stack.append(child)


## Stage-2 card coverage of the traced gather from CovCam0 (as the #77 check measures it).
func _coverage(p_root: Node) -> Dictionary:
	var gather_setting := "rendering/global_illumination/nextgi/medium_gather"
	var previous_gather: Variant = ProjectSettings.get_setting(gather_setting, 0)
	ProjectSettings.set_setting(gather_setting, 1)
	var cov_cam := p_root.get_node_or_null("CovCam0") as Camera3D
	if cov_cam != null:
		cov_cam.current = true
	for _i in range(6):
		await RenderingServer.frame_post_draw
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	ProjectSettings.set_setting(gather_setting, previous_gather)
	var rays := int(info.get("src_coverage_rays", 0))
	var hits := int(info.get("src_coverage_hits", 0))
	return {"rays": rays, "hits": hits, "ratio": snappedf(float(hits) / float(maxi(rays, 1)), 0.0001)}


## Per-frame rules of the native mode: the N7 cap, the MN4 cap, native BLASes inside the AS line,
## no set coarser than its proxy, and the instance stays traced.
func _check_native_frame(p_label: String, p_info: Dictionary, p_require_instance: bool = true) -> void:
	var cap := int(p_info.get("rt_native_rebuild_cap", 0))
	var built := int(p_info.get("rt_native_build_triangles", -1))
	_check(cap > 0, "%s: the native rebuild cap is reported" % p_label)
	_check(built >= 0 and built <= cap, "%s: native BLAS builds %d within the per-frame cap %d" % [p_label, built, cap])
	# MN4 compares build sizes on both sides (compaction shrinks both later).
	var bytes := int(p_info.get("rt_native_build_bytes", -1))
	var memory_cap := int(p_info.get("rt_native_memory_cap_bytes", -1))
	_check(bytes >= 0 and bytes <= memory_cap, "%s: native BLAS build bytes %d within the MN4 cap %d" % [p_label, bytes, memory_cap])
	_check(int(p_info.get("rt_native_coarser_sets", -1)) == 0, "%s: no traced set has fewer triangles than its proxy (%d do)" % [p_label, int(p_info.get("rt_native_coarser_sets", -1))])
	if p_require_instance:
		_check(int(p_info.get("rt_instance_count", 0)) >= 1, "%s: the instance stays in the TLAS" % p_label)
		# The AS line holds the proxies and the native BLASes as they are now (line 1b of this frame).
		var as_bytes := int(p_info.get("rt_as_bytes", -1))
		var line := int(p_info.get("rt_native_as_proxy_bytes", -1)) + int(p_info.get("rt_native_blas_bytes", -1))
		_check(as_bytes == line, "%s: the AS line %d is the proxies plus the native BLASes (%d)" % [p_label, as_bytes, line])


func _check_courtyard() -> Dictionary:
	var packed := load("res://scenes/concave_courtyard/courtyard_vg.tscn") as PackedScene
	_check(packed != null, "courtyard_vg: scene loads")
	if packed == null:
		return {}
	var root := packed.instantiate() as Node3D
	add_child(root)
	_prepare_scene(root)
	var cov_cam := root.get_node_or_null("CovCam0") as Camera3D
	_check(cov_cam != null, "courtyard_vg: CovCam0 exists")
	if cov_cam == null:
		remove_child(root)
		root.free()
		return {}
	cov_cam.current = true

	# 1. Fallback.
	for _i in range(32):
		await RenderingServer.frame_post_draw
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if String(info.get("tier", "")) != "HWRT":
		_check(false, "courtyard_vg: expected tier == 'HWRT', got '%s'" % String(info.get("tier", "")))
		remove_child(root)
		root.free()
		return {"tier": info.get("tier", "")}
	_check(String(info.get("nextgeo_ray_tracing_mode", "")) == "fallback", "fallback: the tier info says fallback")
	_check(not _has_native_keys(info), "fallback: no native counters exist")
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	var proxy_triangles := int(counters.get("proxy_triangles", 0))
	var fallback_arena := int(counters.get("rt_arena_bytes", -1))
	_check(proxy_triangles > 0, "fallback: proxy_triangles > 0")
	_check(int(info.get("rt_instance_count", 0)) >= 1, "fallback: the courtyard is in the TLAS")
	var fallback_coverage := await _coverage(root)
	cov_cam.current = true

	# 2. Native, swept from far to near. The far bands' error targets reach the proxy's eps, so their
	# cut is about the proxy's size and fits MN4; nearer bands are finer. A band whose set MN4
	# refuses keeps the instance on its previous native set; when the new set fits MN4 only without
	# that previous set, the previous set leaves first and the instance traces the proxy until the
	# new set is built (plan §21.4 MN4: over the cap, instances keep the proxy).
	var sweep_cam := Camera3D.new()
	sweep_cam.name = "NativeSweepCam"
	sweep_cam.far = 400.0
	root.add_child(sweep_cam)
	# Distances to the courtyard's corner for bands 5 to 1 (base 4 m, doubling); band 0 is inside.
	var corner := Vector3(8.0, 2.0, 8.0)
	var steps: Array[Dictionary] = []
	var first_native_frame := -1
	var frame_index := 0
	var native_frames := 0
	var previous_frames := 0
	var proxy_frames_after_native := 0
	var max_build := 0
	var max_extract := 0
	var shared_seen := 0
	var finest_triangles := 0
	var coarsest_triangles := 0
	var max_native_blas_bytes := 0
	_set_native(true)
	for step in range(6):
		var band := 5 - step
		if band > 0:
			var distance := NATIVE_BAND_BASE * pow(2.0, float(band - 1)) * 1.4
			var offset := distance / sqrt(2.0)
			sweep_cam.look_at_from_position(corner + Vector3(offset, 0.0, offset), Vector3(0.0, 1.0, 0.0))
			sweep_cam.current = true
		else:
			cov_cam.current = true
		var step_native := 0
		var step_previous := 0
		var step_proxy := 0
		for _i in range(12):
			await RenderingServer.frame_post_draw
			info = RenderingServer.nextgi_get_tier_info()
			_check(String(info.get("nextgeo_ray_tracing_mode", "")) == "native", "native: the tier info says native (band %d)" % band)
			_check_native_frame("native band %d" % band, info)
			var on_band := int(info.get("rt_native_instances", 0)) >= 1
			var on_previous := int(info.get("rt_native_previous_instances", 0)) >= 1
			var on_proxy := int(info.get("rt_native_proxy_instances", 0)) >= 1
			step_native += 1 if on_band else 0
			step_previous += 1 if on_previous else 0
			step_proxy += 1 if on_proxy else 0
			if (on_band or on_previous) and first_native_frame < 0:
				first_native_frame = frame_index
			if first_native_frame >= 0 and on_proxy:
				proxy_frames_after_native += 1
			native_frames += 1 if (on_band or on_previous) else 0
			previous_frames += 1 if on_previous else 0
			max_build = maxi(max_build, int(info.get("rt_native_build_triangles", 0)))
			max_extract = maxi(max_extract, int(info.get("rt_native_extract_triangles", 0)))
			shared_seen = maxi(shared_seen, int(info.get("rt_native_shared_buckets", 0)))
			finest_triangles = maxi(finest_triangles, int(info.get("rt_native_traced_triangles_max", 0)))
			if on_band or on_previous:
				var traced_min := int(info.get("rt_native_traced_triangles_min", 0))
				coarsest_triangles = traced_min if coarsest_triangles == 0 else mini(coarsest_triangles, traced_min)
			max_native_blas_bytes = maxi(max_native_blas_bytes, int(info.get("rt_native_blas_bytes", 0)))
			frame_index += 1
		steps.append({
			"band": band,
			"native_frames": step_native,
			"previous_frames": step_previous,
			"proxy_frames": step_proxy,
			"band_mask": int(info.get("rt_native_band_mask", 0)),
			"triangles": int(info.get("rt_native_triangles", 0)),
			"build_bytes": int(info.get("rt_native_build_bytes", 0)),
			"memory_cap_bytes": int(info.get("rt_native_memory_cap_bytes", 0)),
			"refusals": int(info.get("rt_native_memory_refusals", 0)),
			"reclaims": int(info.get("rt_native_reclaims", 0)),
			"last_refusal": [int(info.get("rt_native_last_refusal_band", -1)), int(info.get("rt_native_last_refusal_triangles", 0)), int(info.get("rt_native_last_refusal_bytes", 0)), int(info.get("rt_native_last_refusal_headroom", 0))],
			"coarse_buckets": int(info.get("rt_native_coarse_buckets", 0)),
		})
	var far_step: Dictionary = steps[0]
	_check(int(far_step["native_frames"]) >= 8, "native: the far band's set is in the TLAS (%d of 12 frames)" % int(far_step["native_frames"]))
	_check(first_native_frame >= 0, "native: the courtyard traced a native set")
	var reclaims := int(info.get("rt_native_reclaims", 0))
	# Once native, the proxy is traced only while an MN4 reclaim's new set is built (one frame, two
	# when the sampled frame straddles it).
	_check(proxy_frames_after_native <= 2 * reclaims, "native: once native, the proxy was traced only in MN4 reclaims (%d frames, %d reclaims)" % [proxy_frames_after_native, reclaims])
	_check(previous_frames >= 1, "native: a band change kept the previous native set while the new band was pending")
	_check(max_build > 0, "native: BLAS builds were recorded")
	# The near bands' cuts are finer than the proxy, and Concave Courtyard's band 2 set fits MN4 on its own.
	_check(finest_triangles > proxy_triangles, "native: a near band traced a set finer than the proxy (%d > %d triangles)" % [finest_triangles, proxy_triangles])
	var refusals := int(info.get("rt_native_memory_refusals", 0))
	if refusals > 0:
		# MN4 refused a nearer band: the refusal is counted and the instance stayed native.
		_check(int(info.get("rt_native_last_refusal_band", -1)) >= 0, "native: the refused band is reported")
	_check(int(info.get("rt_native_excluded_vertex_offset", -1)) == 0 and int(info.get("rt_native_excluded_legacy_surfaces", -1)) == 0, "native: the courtyard is not excluded")
	# The AS line itself is checked every frame (_check_native_frame); here, that it held native bytes.
	_check(max_native_blas_bytes > 0, "native: native BLASes were in the AS line")
	var native_parts := int(info.get("rt_native_parts", 0))
	var native_triangles := int(info.get("rt_native_triangles", 0))
	_check(native_parts >= 1, "native: rt_native_parts >= 1 (got %d)" % native_parts)
	# Concave Courtyard is one mesh, so every traced set compares with its proxy's triangles.
	_check(coarsest_triangles >= proxy_triangles, "native: no traced set was coarser than the proxy (fewest %d, proxy %d triangles)" % [coarsest_triangles, proxy_triangles])
	# Stage-2 card coverage with the instance on a native set (band 0 here: its own or the previous).
	var native_coverage := await _coverage(root)
	info = RenderingServer.nextgi_get_tier_info()
	_check(int(info.get("rt_native_instances", 0)) + int(info.get("rt_native_previous_instances", 0)) >= 1, "native: the coverage frames traced a native set")
	_check(int(native_coverage["hits"]) > 0, "native: Stage-2 card hits > 0 through native BLASes (got %d)" % int(native_coverage["hits"]))
	var no_card_triangles := int(info.get("rt_native_no_card_triangles", -1))
	var chosen_card_triangles := int(info.get("rt_native_chosen_card_triangles", -1))
	var direct_cards := int(info.get("rt_native_direct_card_meshes", -1))
	var card_meshes := int(info.get("rt_native_card_meshes", -1))
	var proxy_card_meshes := int(info.get("rt_native_proxy_card_meshes", -1))
	var proxy_card_triangles := int(info.get("rt_native_proxy_card_triangles", -1))
	# Hit identity (#77): triangles the cut shares with the proxy keep the proxy's card groups; finer
	# ones take the card layer on their own axis that holds them (chosen per triangle on the GPU), so
	# Stage-2 lookups still work.
	_check(proxy_card_meshes >= 1, "native: the courtyard's native triangles take the proxy's card groups")
	_check(chosen_card_triangles > 0, "native: the finer set's triangles chose their card groups (got %d)" % chosen_card_triangles)
	_check(no_card_triangles == 0, "native: every native triangle can find a card group (got %d without)" % no_card_triangles)
	_check(float(native_coverage["ratio"]) >= 0.85, "native: Stage-2 card hit coverage >= 0.85 through native BLASes (got %.4f)" % float(native_coverage["ratio"]))
	var as_line := await _check_as_line(sweep_cam, corner)
	cov_cam.current = true

	# 5. Back to fallback: everything native is released at the next TLAS phase.
	_set_native(false)
	for _i in range(4):
		await RenderingServer.frame_post_draw
	info = RenderingServer.nextgi_get_tier_info()
	counters = RenderingServer.nextgi_get_counters()
	_check(String(info.get("nextgeo_ray_tracing_mode", "")) == "fallback", "back to fallback: the tier info says fallback")
	_check(not _has_native_keys(info), "back to fallback: no native counters exist")
	_check(int(counters.get("rt_arena_bytes", -2)) == fallback_arena, "back to fallback: RT-arena bytes return to %d (got %d)" % [fallback_arena, int(counters.get("rt_arena_bytes", -2))])
	_check(int(info.get("rt_instance_count", 0)) >= 1, "back to fallback: the courtyard traces its proxy")

	remove_child(root)
	root.free()
	for _i in range(3):
		await RenderingServer.frame_post_draw
	return {
		"proxy_triangles": proxy_triangles,
		"native_triangles": native_triangles,
		"native_parts": native_parts,
		"native_frames": native_frames,
		"previous_frames": previous_frames,
		"max_build_triangles": max_build,
		"max_extract_triangles": max_extract,
		"shared_buckets": shared_seen,
		"memory_refusals": refusals,
		"reclaims": reclaims,
		"finest_triangles": finest_triangles,
		"proxy_frames_after_native": proxy_frames_after_native,
		"coarsest_triangles": coarsest_triangles,
		"as_line": as_line,
		"max_native_blas_bytes": max_native_blas_bytes,
		"steps": steps,
		"no_card_triangles": no_card_triangles,
		"chosen_card_triangles": chosen_card_triangles,
		"card_meshes": card_meshes,
		"direct_card_meshes": direct_cards,
		"proxy_card_meshes": proxy_card_meshes,
		"proxy_card_triangles": proxy_card_triangles,
		"fallback_coverage": fallback_coverage,
		"native_coverage": native_coverage,
		"fallback_arena_bytes": fallback_arena,
	}


## MN4 inside the AS memory line (plan §21.4): over it, instances keep the proxy, and native sets
## never push the tier to AS_MEMORY (the proxies alone decide that).
func _check_as_line(p_cam: Camera3D, p_corner: Vector3) -> Dictionary:
	var offset := NATIVE_BAND_BASE * 1.4 / sqrt(2.0) # Band 1.
	p_cam.look_at_from_position(p_corner + Vector3(offset, 0.0, offset), Vector3(0.0, 1.0, 0.0))
	p_cam.current = true
	var info: Dictionary = {}
	for _i in range(12):
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
	var held := int(info.get("rt_native_build_bytes", 0))
	var proxies := int(info.get("rt_native_as_proxy_bytes", 0))
	var releases := int(info.get("rt_native_cap_releases", 0))
	_check(held > 0 and int(info.get("rt_native_instances", 0)) >= 1, "as line: the courtyard traces its band set before the budget falls (%d bytes held)" % held)
	var budget := proxies + (held >> 1)
	ProjectSettings.set_setting(AS_BUDGET_SETTING, budget)
	for _i in range(8):
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		_check_native_frame("as line low", info)
		_check(String(info.get("tier", "")) == "HWRT", "as line low: the tier stays HWRT (got '%s')" % String(info.get("tier", "")))
		_check(not bool(info.get("rt_as_memory_exceeded", true)), "as line low: native sets never exceed the AS line")
	var low := {
		"budget": budget,
		"held_before": held,
		"memory_cap_bytes": int(info.get("rt_native_memory_cap_bytes", -1)),
		"build_bytes": int(info.get("rt_native_build_bytes", -1)),
		"cap_releases": int(info.get("rt_native_cap_releases", 0)) - releases,
		"native_instances": int(info.get("rt_native_instances", 0)) + int(info.get("rt_native_previous_instances", 0)),
		"proxy_instances": int(info.get("rt_native_proxy_instances", 0)),
	}
	_check(int(low["memory_cap_bytes"]) < held, "as line low: the native cap follows the AS line (%d < %d)" % [int(low["memory_cap_bytes"]), held])
	_check(int(low["cap_releases"]) >= 1, "as line low: the native set left when the cap shrank (%s)" % str(low))
	_check(int(low["native_instances"]) == 0 and int(low["proxy_instances"]) >= 1, "as line low: the courtyard keeps its proxy (%s)" % str(low))
	ProjectSettings.set_setting(AS_BUDGET_SETTING, 0)
	for _i in range(12):
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		_check_native_frame("as line restored", info)
	var restored := int(info.get("rt_native_instances", 0)) + int(info.get("rt_native_previous_instances", 0))
	_check(restored >= 1, "as line restored: the band's set is cut and built again")
	low["restored_native_instances"] = restored
	return low


## Native-mode exclusions (#156 step 8): materials that move the surface the TLAS carries keep the
## proxy; others do not. Each case runs a few frames and reads the last frame's exclusion counter.
func _exclusion_frames(p_label: String) -> Dictionary:
	var info: Dictionary = {}
	for _i in range(6):
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		_check_native_frame("exclusions, %s" % p_label, info)
	return {
		"excluded_vertex_offset": int(info.get("rt_native_excluded_vertex_offset", -1)),
		"native_instances": int(info.get("rt_native_instances", 0)) + int(info.get("rt_native_previous_instances", 0)),
	}


func _check_exclusions(p_mesh_instance: MeshInstance3D) -> Dictionary:
	var mesh_instance := p_mesh_instance
	var moving := ShaderMaterial.new()
	moving.shader = Shader.new()
	moving.shader.code = "shader_type spatial;\nvoid vertex() {\n\tVERTEX += NORMAL * 0.002;\n}\nvoid fragment() {\n\tALBEDO = vec3(0.6);\n}\n"
	var depth_only := ShaderMaterial.new()
	depth_only.shader = Shader.new()
	depth_only.shader.code = "shader_type spatial;\nvoid fragment() {\n\tALBEDO = vec3(0.6);\n\tDEPTH = FRAGCOORD.z;\n}\n"
	var results := {}
	mesh_instance.material_override = moving
	results["override_moves"] = await _exclusion_frames("override moves vertices")
	mesh_instance.material_override = depth_only
	results["override_depth"] = await _exclusion_frames("override writes depth")
	mesh_instance.material_override = null
	mesh_instance.set_surface_override_material(0, moving)
	results["surface_moves"] = await _exclusion_frames("surface material moves vertices")
	mesh_instance.set_surface_override_material(0, null)
	mesh_instance.material_overlay = moving
	results["overlay_moves"] = await _exclusion_frames("overlay moves vertices")
	mesh_instance.material_overlay = null
	results["restored"] = await _exclusion_frames("restored")
	_check(int(results["override_moves"]["excluded_vertex_offset"]) >= 1 and int(results["override_moves"]["native_instances"]) == 0, "exclusions: a material override that moves vertices keeps the proxy (%s)" % str(results["override_moves"]))
	_check(int(results["surface_moves"]["excluded_vertex_offset"]) >= 1 and int(results["surface_moves"]["native_instances"]) == 0, "exclusions: a surface material that moves vertices keeps the proxy (%s)" % str(results["surface_moves"]))
	_check(int(results["override_depth"]["excluded_vertex_offset"]) == 0 and int(results["override_depth"]["native_instances"]) >= 1, "exclusions: a fragment depth write does not move the surface (%s)" % str(results["override_depth"]))
	_check(int(results["overlay_moves"]["excluded_vertex_offset"]) == 0 and int(results["overlay_moves"]["native_instances"]) >= 1, "exclusions: an overlay draws over the surface and does not move it (%s)" % str(results["overlay_moves"]))
	_check(int(results["restored"]["native_instances"]) >= 1, "exclusions: the courtyard traces native again once restored (%s)" % str(results["restored"]))
	return results


## A wavy sheet with real simplification error at every DAG level.
func _make_wavy_sheet(p_tag: String) -> ArrayMesh:
	var n := 96
	var size := 4.0
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var indices := PackedInt32Array()
	for z in range(n + 1):
		for x in range(n + 1):
			var fx := (float(x) / float(n) - 0.5) * size
			var fz := (float(z) / float(n) - 0.5) * size
			vertices.append(Vector3(fx, 0.25 * sin(fx * 3.1) * cos(fz * 2.7), fz))
			normals.append(Vector3.UP)
	for z in range(n):
		for x in range(n):
			var i0 := z * (n + 1) + x
			indices.append_array([i0, i0 + 1, i0 + n + 1, i0 + 1, i0 + n + 2, i0 + n + 1])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	var source := ArrayMesh.new()
	source.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.7, 0.7, 0.7)
	source.surface_set_material(0, material)
	var imp := ImporterMesh.from_mesh(source)
	var path := "user://ng_rt_native_%s.ngm" % p_tag
	_temp_files.append(path)
	var err := imp.build_virtual_geometry(path)
	if err != OK:
		_failures.append("streaming: build_virtual_geometry failed (err=%d)" % err)
		return null
	var mesh := imp.get_mesh()
	if mesh == null or mesh.get_virtual_geometry() == null:
		_failures.append("streaming: missing VirtualGeometryData")
		return null
	return mesh


func _check_streaming() -> Dictionary:
	var previous_cache: Variant = ProjectSettings.get_setting("rendering/limits/nextgeo/geometry_cache_mb", 512)
	var previous_upload: Variant = ProjectSettings.get_setting("rendering/limits/nextgeo/page_upload_kib_per_frame", 4096)
	var previous_delay: Variant = ProjectSettings.get_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2())
	ProjectSettings.set_setting("rendering/limits/nextgeo/geometry_cache_mb", CACHE_MB)
	ProjectSettings.set_setting("rendering/limits/nextgeo/page_upload_kib_per_frame", 64)
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(5.0, 10.0))
	_set_native(true)

	var root := Node3D.new()
	root.name = "NativeStreaming"
	add_child(root)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.3, 0.35, 0.4)
	env.nextgi_enabled = true
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	root.add_child(world_env)
	var sun := DirectionalLight3D.new()
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	root.add_child(sun)
	var cam := Camera3D.new()
	root.add_child(cam)
	cam.look_at_from_position(Vector3(0.0, 2.2, 2.2), Vector3.ZERO)
	cam.current = true

	var mesh := _make_wavy_sheet("sheet")
	if mesh == null:
		remove_child(root)
		root.free()
		return {}
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	root.add_child(instance)

	var swaps := 0
	var builds := 0
	var coarse_frames := 0
	var native_frames := 0
	var proxy_frames := 0
	var uploads_start := int(RenderingServer.virtual_geometry_get_routing_counters().get("page_uploads", 0))
	var info: Dictionary = {}
	var traced := false
	for frame in range(150):
		OS.delay_msec(4)
		await RenderingServer.frame_post_draw
		info = RenderingServer.nextgi_get_tier_info()
		if String(info.get("tier", "")) != "HWRT":
			continue
		# Once the sheet is in the TLAS (its proxy built), it never leaves it again.
		traced = traced or int(info.get("rt_instance_count", 0)) >= 1
		_check_native_frame("streaming frame %d" % frame, info, traced)
		swaps += int(info.get("rt_native_swaps", 0))
		builds += int(info.get("rt_native_build_count", 0))
		coarse_frames += 1 if int(info.get("rt_native_coarse_buckets", 0)) > 0 else 0
		native_frames += 1 if int(info.get("rt_native_instances", 0)) + int(info.get("rt_native_previous_instances", 0)) >= 1 else 0
		proxy_frames += 1 if int(info.get("rt_native_proxy_instances", 0)) >= 1 else 0
	var uploads := int(RenderingServer.virtual_geometry_get_routing_counters().get("page_uploads", 0)) - uploads_start
	_check(String(info.get("tier", "")) == "HWRT", "streaming: tier HWRT")
	_check(uploads > 0, "streaming: pages streamed in (%d uploads)" % uploads)
	_check(swaps >= 1, "streaming: residency changes rebuilt the sheet's native set (%d swaps)" % swaps)
	_check(native_frames >= 1, "streaming: the sheet traced its native set")
	var exclusions := await _check_exclusions(instance)

	# Switching modes with the sheet still traced: native adds its arena ranges and gives every one
	# of them back.
	_set_native(false)
	for _i in range(4):
		await RenderingServer.frame_post_draw
	var fallback_arena := int(RenderingServer.nextgi_get_counters().get("rt_arena_bytes", -1))
	_set_native(true)
	var native_arena := -1
	for _i in range(12):
		await RenderingServer.frame_post_draw
		native_arena = maxi(native_arena, int(RenderingServer.nextgi_get_counters().get("rt_arena_bytes", -1)))
	_set_native(false)
	for _i in range(4):
		await RenderingServer.frame_post_draw
	var back_arena := int(RenderingServer.nextgi_get_counters().get("rt_arena_bytes", -1))
	_check(native_arena > fallback_arena, "streaming: native sets add RT-arena bytes (%d > %d)" % [native_arena, fallback_arena])
	_check(back_arena == fallback_arena, "streaming: back in fallback the RT arena returns to %d bytes (got %d)" % [fallback_arena, back_arena])

	# The last instance leaves while native: its native sets and its proxy are released.
	_set_native(true)
	for _i in range(4):
		await RenderingServer.frame_post_draw
	remove_child(root)
	root.free()
	instance = null
	mesh = null
	for _i in range(NATIVE_RETIRE_FRAMES + 4):
		await RenderingServer.frame_post_draw
	var native_freed: Dictionary = RenderingServer.nextgi_get_counters()
	var native_freed_info: Dictionary = RenderingServer.nextgi_get_tier_info()
	_check(int(native_freed_info.get("rt_native_buckets", -1)) == 0, "streaming: no native bucket outlives the sheet (got %d)" % int(native_freed_info.get("rt_native_buckets", -1)))
	_check(int(native_freed.get("rt_arena_bytes", -1)) == 0, "streaming: 0 RT-arena bytes once the sheet is gone in native mode (got %d)" % int(native_freed.get("rt_arena_bytes", -1)))
	_set_native(false)
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", previous_delay)
	ProjectSettings.set_setting("rendering/limits/nextgeo/page_upload_kib_per_frame", previous_upload)
	ProjectSettings.set_setting("rendering/limits/nextgeo/geometry_cache_mb", previous_cache)
	for _i in range(4):
		await RenderingServer.frame_post_draw
	var freed: Dictionary = RenderingServer.nextgi_get_counters()
	_check(int(freed.get("proxy_triangles", -1)) == 0, "streaming: the sheet's proxy is released with its last instance")
	return {
		"fallback_arena_bytes": fallback_arena,
		"native_arena_bytes": native_arena,
		"native_freed_arena_bytes": int(native_freed.get("rt_arena_bytes", -1)),
		"back_arena_bytes": back_arena,
		"freed_proxy_triangles": int(freed.get("proxy_triangles", -1)),
		"freed_proxy_blas_parts": int(freed.get("proxy_blas_parts", -1)),
		"freed_rt_arena_bytes": int(freed.get("rt_arena_bytes", -1)),
		"page_uploads": uploads,
		"swaps": swaps,
		"build_count": builds,
		"coarse_frames": coarse_frames,
		"native_frames": native_frames,
		"proxy_frames": proxy_frames,
		"exclusions": exclusions,
	}
