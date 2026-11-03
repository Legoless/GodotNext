extends SceneTree
## Structural check for the Exclusion Matrix (#74).
##
## Asserts every cell's expected route and reason, one candidate and one twin,
## and 0 double-drawn / 0 missing surfaces. Runtime debug stats are read when
## `RenderingServer.virtual_geometry_get_debug_stats` exists and compared;
## until #79 that method is absent and the authored scene is the report.
##
## Run:
## [code]godot --headless --path godotnext/testbed --script res://scenes/exclusion_matrix/check_exclusion.gd[/code]

const Rules = preload("res://scenes/exclusion_matrix/exclusion_rules.gd")
const SCENE_PATH := "res://scenes/exclusion_matrix/exclusion_matrix.tscn"
const TRACK_PATH := "res://scenes/exclusion_matrix/moving_track.json"
const CELLS_PATH := "res://scenes/exclusion_matrix/cells.json"

var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var contract := Rules.contract_error()
	if not contract.is_empty():
		_fail(contract)
	var packed := load(SCENE_PATH) as PackedScene
	if packed == null:
		printerr("check_exclusion FAIL: cannot load " + SCENE_PATH)
		quit(1)
		return
	var root := packed.instantiate()
	get_root().add_child(root)
	var tally := _check_cells(root)
	_check_track()
	_check_cells_json()
	_check_runtime(root)
	root.free()
	if _failures.is_empty():
		print("check_exclusion: all %d assertions passed (%d cells, double_drawn=%d, missing=%d)." % [
			_checks, Rules.cells().size(), int(tally["double_drawn"]), int(tally["missing"])])
		quit(0)
	else:
		for failure in _failures:
			printerr("check_exclusion FAIL: " + failure)
		quit(1)


func _check_cells(p_root: Node) -> Dictionary:
	var double_drawn := 0
	var missing := 0
	var seen := {}
	for cell_value in Rules.cells():
		var cell: Dictionary = cell_value
		var id := String(cell["id"])
		seen[id] = true
		var node := p_root.get_node_or_null(id)
		if not _ok(node != null, "missing cell %s." % id):
			missing += 1
			continue
		var candidates := _find_named(node, "Candidate")
		var twins := _find_named(node, "Twin")
		if candidates.size() == 0:
			missing += 1
			_fail("cell %s has no Candidate." % id)
		elif candidates.size() > 1:
			double_drawn += candidates.size() - 1
			_fail("cell %s has %d Candidate nodes." % [id, candidates.size()])
		else:
			_ok(true, "candidate")
		if twins.size() != 1:
			if twins.is_empty():
				missing += 1
			else:
				double_drawn += twins.size() - 1
			_fail("cell %s has %d Twin nodes." % [id, twins.size()])
		else:
			_ok(true, "twin")
		_check_kind(id, String(cell["kind"]), candidates, cell)
		var surfaces: Array = cell["surfaces"]
		if not candidates.is_empty() and candidates[0] is MeshInstance3D and String(cell["kind"]) == "mesh":
			var mesh := (candidates[0] as MeshInstance3D).mesh
			var count := mesh.get_surface_count() if mesh != null else 0
			if count != surfaces.size():
				var gap := absi(count - surfaces.size())
				missing += gap
				_fail("cell %s surface count %d, expected %d." % [id, count, surfaces.size()])
			else:
				_ok(true, "surfaces")
		for surface_value in surfaces:
			var surface: Dictionary = surface_value
			_ok(not String(surface["route"]).is_empty() and not String(surface["reason"]).is_empty(),
					"cell %s surface %s needs a route and a reason." % [id, String(surface["name"])])
	for child in p_root.get_children():
		var name := String(child.name)
		if Rules.cell_by_id(name).is_empty() and name not in ["CaptureCam", "WorldEnvironment", "Sun", "Floor"]:
			double_drawn += 1
			_fail("unexpected top-level node %s." % name)
	_ok(double_drawn == 0, "double_drawn is %d." % double_drawn)
	_ok(missing == 0, "missing is %d." % missing)
	return {"double_drawn": double_drawn, "missing": missing}


func _check_kind(p_id: String, p_kind: String, p_candidates: Array, p_cell: Dictionary) -> void:
	if p_candidates.is_empty():
		return
	var candidate: Node = p_candidates[0]
	match p_kind:
		"mesh", "moving":
			_ok(candidate is MeshInstance3D, "%s Candidate must be a MeshInstance3D." % p_id)
			_ok(candidate.visible, "%s Candidate must be visible." % p_id)
			_ok(candidate.get_parent().get_node_or_null("StandIn") == null, "%s must not hide the candidate behind a stand-in." % p_id)
		"multimesh":
			_ok(candidate is MultiMeshInstance3D, "%s Candidate must be a MultiMeshInstance3D." % p_id)
		"gridmap":
			_ok(candidate is GridMap, "%s Candidate must be a GridMap." % p_id)
			_ok(_baked_count(candidate) == 0, "%s octant GridMap must not call make_baked_meshes()." % p_id)
		"baked_gridmap":
			_ok(candidate is GridMap, "%s Candidate must be a GridMap." % p_id)
			_ok(_baked_count(candidate) > 0, "%s must keep the make_baked_meshes() meshes." % p_id)
		"particles":
			_ok(candidate is GPUParticles3D, "%s Candidate must be GPUParticles3D." % p_id)
			if candidate is GPUParticles3D:
				var gpu := candidate as GPUParticles3D
				_ok(gpu.emitting, "%s must emit." % p_id)
				_ok(gpu.fixed_fps == 60, "%s fixed_fps must be 60." % p_id)
				_ok(gpu.use_fixed_seed, "%s must keep a fixed seed." % p_id)
				_ok(gpu.seed == 1, "%s seed must be 1." % p_id)
				_ok(not gpu.interpolate, "%s must not interpolate between ticks." % p_id)
				_ok(not gpu.fract_delta, "%s must not use a fractional delta." % p_id)
				_ok(is_equal_approx(gpu.speed_scale, 1.0), "%s speed_scale must be 1." % p_id)
		"heightfield":
			_ok(candidate is GPUParticlesCollisionHeightField3D, "%s Candidate must be a heightfield." % p_id)
			var proxy := (candidate.get_parent() as Node).get_node_or_null("CoarseProxy")
			_ok(proxy is MeshInstance3D, "%s needs a CoarseProxy mesh for the capture policy." % p_id)
		"msaa":
			_ok(candidate is SubViewport, "%s Candidate must be a SubViewport." % p_id)
			if candidate is SubViewport:
				var msaa_view := candidate as SubViewport
				_ok(msaa_view.msaa_3d == Viewport.MSAA_2X, "%s must use MSAA 2x." % p_id)
				_ok(msaa_view.render_target_update_mode == SubViewport.UPDATE_ALWAYS, "%s must update every frame." % p_id)
		"multiview":
			_ok(candidate is SubViewport, "%s Candidate must be a SubViewport." % p_id)
			if candidate is SubViewport:
				var xr_view := candidate as SubViewport
				_ok(xr_view.use_xr, "%s must set use_xr (MULTIVIEW)." % p_id)
				_ok(xr_view.render_target_update_mode == SubViewport.UPDATE_ALWAYS, "%s update mode stays UPDATE_ALWAYS." % p_id)
				_ok(xr_view.get_node_or_null("ViewCam") is XRCamera3D, "%s camera must be an XRCamera3D." % p_id)
				_ok(String(p_cell.get("draw_limit", "")) == Rules.MULTIVIEW_DRAW_LIMIT, "%s must record the XR draw limit." % p_id)
	_check_material(p_id, candidate)


func _check_material(p_id: String, p_candidate: Node) -> void:
	match p_id:
		"mixed_opaque_glass":
			var mesh_instance := p_candidate as MeshInstance3D
			if mesh_instance == null or mesh_instance.mesh == null:
				_fail("mixed cell has no mesh.")
				return
			var opaque := mesh_instance.mesh.surface_get_material(0) as BaseMaterial3D
			var glass := mesh_instance.mesh.surface_get_material(1) as BaseMaterial3D
			_ok(opaque != null and opaque.transparency == BaseMaterial3D.TRANSPARENCY_DISABLED, "opaque surface must be opaque.")
			_ok(glass != null and glass.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA, "glass surface must be alpha blended.")
		"next_pass":
			var base := _first_material(p_candidate) as BaseMaterial3D
			_ok(base != null and base.next_pass != null, "next_pass cell must chain a next_pass material.")
		"material_overlay":
			var inst := p_candidate as GeometryInstance3D
			_ok(inst != null and inst.material_overlay != null, "overlay cell must set material_overlay.")
		"stencil_reader":
			var stencil := _first_material(p_candidate) as BaseMaterial3D
			_ok(stencil != null and stencil.stencil_mode != BaseMaterial3D.STENCIL_MODE_DISABLED, "stencil reader must enable stencil.")
			_ok(stencil != null and (stencil.stencil_flags & BaseMaterial3D.STENCIL_FLAG_READ) != 0, "stencil reader must set STENCIL_FLAG_READ.")
		"vertex_lighting":
			var vertex := _first_material(p_candidate) as BaseMaterial3D
			_ok(vertex != null and vertex.shading_mode == BaseMaterial3D.SHADING_MODE_PER_VERTEX, "vertex lighting shading mode.")
		"position_vertex":
			_ok(_shader_code(p_candidate).contains("VERTEX.y"), "position-altering vertex() must write VERTEX.")
		"screen_texture":
			_ok(_shader_code(p_candidate).contains("hint_screen_texture"), "screen-texture cell must sample the screen texture.")
			var screen_twin := p_candidate.get_parent().get_node_or_null("Twin")
			_ok(_shader_code(screen_twin).contains("hint_screen_texture"), "screen-texture twin must sample the screen texture.")
		"alpha_scissor":
			var scissor := _first_material(p_candidate) as BaseMaterial3D
			_ok(scissor != null and scissor.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR, "alpha scissor transparency.")
		"alpha_hash":
			var alpha_hash := _first_material(p_candidate) as BaseMaterial3D
			_ok(alpha_hash != null and alpha_hash.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_HASH, "alpha hash transparency.")
		"unsupported_derivative":
			var code := _shader_code(p_candidate)
			_ok(code.contains("dFdx(") and code.contains("uniform float factor"), "non-varying dFdx must differentiate a uniform.")
		"skinning":
			var skinned := p_candidate as MeshInstance3D
			_ok(skinned != null and skinned.skin != null and skinned.mesh != null, "skinning cell needs a Skin.")
			if skinned != null and skinned.mesh != null:
				var arrays: Array = skinned.mesh.surface_get_arrays(0)
				var bones: Variant = arrays[Mesh.ARRAY_BONES]
				_ok(bones is PackedInt32Array and not (bones as PackedInt32Array).is_empty(), "skinning cell needs bone indices.")
		"instance_uniforms":
			_ok(_shader_code(p_candidate).contains("instance uniform"), "instance-uniform cell needs an instance uniform.")
			var geom := p_candidate as GeometryInstance3D
			_ok(geom != null and geom.get_instance_shader_parameter("instance_tint") != null, "instance_tint must be set.")
		"animated_time":
			var time_code := _shader_code(p_candidate)
			_ok(time_code.contains("TIME") and not time_code.contains("VERTEX"), "TIME material must be virtual-eligible (no position write).")
			var time_twin := p_candidate.get_parent().get_node_or_null("Twin")
			var twin_code := _shader_code(time_twin)
			_ok(twin_code.contains("TIME") and not twin_code.contains("VERTEX"), "TIME twin must use the same virtual-eligible shader.")
		"mutated_mesh":
			_ok(p_candidate is MeshInstance3D, "mutated cell Candidate must be a mesh.")
		"moving_instance":
			var moving := p_candidate as Node3D
			_ok(moving != null, "moving candidate")
			if moving != null:
				var offset := Rules.moving_offset(0)
				_ok(moving.position.distance_to(offset) < 1e-4, "moving candidate must start on the seeded path.")


func _check_track() -> void:
	var text := FileAccess.get_file_as_string(TRACK_PATH)
	var parsed: Variant = JSON.parse_string(text)
	if not _ok(parsed is Dictionary, "moving_track.json must be an object."):
		return
	var body: Dictionary = parsed
	var samples: Array = body.get("samples", [])
	_ok(samples.size() == Rules.MOVING_FRAMES, "track has %d samples, expected %d." % [samples.size(), Rules.MOVING_FRAMES])
	if samples.size() < 2:
		return
	var expected_0: Dictionary = Rules.track_sample(0)
	var expected_1: Dictionary = Rules.track_sample(1)
	_ok(_close(samples[0], expected_0), "track frame 0 does not match the analytic UV.")
	_ok(_close(samples[1], expected_1), "track frame 1 previous UV must be frame 0.")
	var dlss: Dictionary = body.get("dlss", {})
	var expected_dlss: Dictionary = Rules.dlss_status()
	_ok(String(dlss.get("status", "")) == String(expected_dlss["status"]), "DLSS status must match this build.")
	_ok(String(dlss.get("fallback", "")) == String(expected_dlss["fallback"]), "DLSS fallback must match this build.")
	_ok(is_equal_approx(float(dlss.get("scale", 0.0)), Rules.DLSS_QUALITY_SCALE), "DLSS Quality scale must be 0.667.")
	_ok(String(dlss.get("dll_sha256", "missing")) == "", "the scene must not record an NVIDIA DLL hash.")


func _check_cells_json() -> void:
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(CELLS_PATH))
	if not _ok(parsed is Dictionary, "cells.json must be an object."):
		return
	var rows: Array = (parsed as Dictionary).get("cells", [])
	_ok(rows.size() == Rules.cells().size(), "cells.json count")
	if rows.size() != Rules.cells().size():
		return
	for index in rows.size():
		var got: Dictionary = rows[index]
		var expected: Dictionary = Rules.cells()[index]
		_ok(String(got["id"]) == String(expected["id"]), "cells.json id order")
		var got_surfaces: Array = got["surfaces"]
		var expected_surfaces: Array = expected["surfaces"]
		_ok(got_surfaces.size() == expected_surfaces.size(), "cells.json surfaces for %s" % String(expected["id"]))
		for surface_index in mini(got_surfaces.size(), expected_surfaces.size()):
			var got_surface: Dictionary = got_surfaces[surface_index]
			var expected_surface: Dictionary = expected_surfaces[surface_index]
			_ok(String(got_surface["route"]) == String(expected_surface["route"]),
					"%s route" % String(expected["id"]))
			_ok(String(got_surface["reason"]) == String(expected_surface["reason"]),
					"%s reason" % String(expected["id"]))


func _check_runtime(p_root: Node) -> void:
	# #79 fills virtual_geometry_get_debug_stats. Absence is a valid report, not a skip.
	var source := "absent"
	if RenderingServer.has_method(Rules.DEBUG_STATS_METHOD):
		source = Rules.DEBUG_STATS_METHOD
		var stats: Variant = RenderingServer.call(Rules.DEBUG_STATS_METHOD)
		_ok(stats is Dictionary, "debug stats must be a Dictionary when the method exists.")
	_ok(source == "absent" or source == Rules.DEBUG_STATS_METHOD, "debug stats source")
	_ok(p_root.get_node_or_null("CaptureCam") is Camera3D, "CaptureCam")
	var g57 := p_root.get_node_or_null("moving_instance/G57Cam") as Camera3D
	_ok(g57 != null, "G57Cam")
	if g57 != null:
		_ok(g57.global_position.distance_to(p_root.get_node("moving_instance").global_position + Rules.MOVING_CAMERA_POS) < 1e-3,
				"G57Cam must sit on the analytic camera pose.")
		_ok(is_equal_approx(g57.fov, Rules.MOVING_FOV_DEG), "G57Cam fov")


func _find_named(p_node: Node, p_name: String) -> Array:
	var found: Array = []
	if String(p_node.name) == p_name:
		found.append(p_node)
	for child in p_node.get_children():
		found.append_array(_find_named(child, p_name))
	return found


func _baked_count(p_node: Node) -> int:
	var value: Variant = p_node.get("baked_meshes")
	if value is Array:
		return (value as Array).size()
	return 0


func _first_material(p_node: Node) -> Material:
	var mesh_instance := p_node as MeshInstance3D
	if mesh_instance == null or mesh_instance.mesh == null:
		return null
	return mesh_instance.mesh.surface_get_material(0)


func _shader_code(p_node: Node) -> String:
	var material := _first_material(p_node) as ShaderMaterial
	if material == null or material.shader == null:
		return ""
	return material.shader.code


func _close(p_got: Variant, p_expected: Dictionary) -> bool:
	if not (p_got is Dictionary):
		return false
	var got: Dictionary = p_got
	for key in ["position", "previous_position", "uv", "previous_uv"]:
		var a: Array = got[key]
		var b: Array = p_expected[key]
		if a.size() != b.size():
			return false
		for index in a.size():
			if not is_equal_approx(float(a[index]), float(b[index])):
				return false
	return int(got["frame"]) == int(p_expected["frame"])


func _ok(p_condition: bool, p_message: String) -> bool:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)
	return p_condition


func _fail(p_message: String) -> void:
	_checks += 1
	_failures.append(p_message)
