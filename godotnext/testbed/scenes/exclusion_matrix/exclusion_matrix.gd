extends Node3D
## Exclusion Matrix runtime (#74, #79).
##
## Virtualizes candidate meshes when running on a real rendering device, moves
## the G5-7 instance on the seeded path, and calls `surface_update_vertex_region`
## on the mutated-mesh cell at frame 60. When `--out` is set, `_exit_tree`
## writes `exclusion_observed.json` for the report tool.

const Rules = preload("res://scenes/exclusion_matrix/exclusion_rules.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

var _hold := -1
var _local_frame := 0
var _saw := false
var _mutated := false
var _frame := 0
var _ngm_counter := 0
var _temp_ngm_files: Array[String] = []
var _out_dir := ""
var _last_cells: Array = []


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	_out_dir = SceneArgs.value("--out", "")
	if RenderingServer.get_current_rendering_driver_name().to_lower() != "dummy":
		ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
		ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
		ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
		_virtualize_candidates()
	process_priority = 1
	_hold = SceneArgs.int_value("--hold-frame", -1)
	var status: Dictionary = Rules.dlss_status()
	print("exclusion_matrix: cells=%d dlss=%s fallback=%s scale=%.3f" % [
		Rules.cells().size(),
		String(status["status"]),
		String(status["fallback"]),
		float(status["scale"]),
	])
	_apply(0 if _hold < 0 else _hold)


func _virtualize_candidates() -> void:
	for cell_value in Rules.cells():
		var cell: Dictionary = cell_value
		var cid := String(cell["id"])
		var kind := String(cell["kind"])
		var cand := _find_named_first(get_node_or_null(cid), "Candidate")
		match kind:
			"mesh", "moving":
				var mi := cand as MeshInstance3D
				if mi != null and mi.mesh != null and mi.mesh.get_virtual_geometry() == null:
					var vmesh := _build_virtual_mesh(mi.mesh, cid, mi.skin != null, true)
					if vmesh != null:
						mi.mesh = vmesh
			"multimesh":
				var mmi := cand as MultiMeshInstance3D
				if mmi != null and mmi.multimesh != null and mmi.multimesh.mesh != null and mmi.multimesh.mesh.get_virtual_geometry() == null:
					var mm_vmesh := _build_virtual_mesh(mmi.multimesh.mesh, cid, false, false)
					if mm_vmesh != null:
						mmi.multimesh.mesh = mm_vmesh
			"particles":
				var gpu := cand as GPUParticles3D
				if gpu != null and gpu.draw_pass_1 != null and gpu.draw_pass_1.get_virtual_geometry() == null:
					var p_vmesh := _build_virtual_mesh(gpu.draw_pass_1, cid, false, false)
					if p_vmesh != null:
						gpu.draw_pass_1 = p_vmesh
			"gridmap":
				var gm := cand as GridMap
				if gm != null and gm.mesh_library != null:
					var rebuilt := false
					for item_id in gm.mesh_library.get_item_list():
						var im := gm.mesh_library.get_item_mesh(item_id)
						if im != null and im.get_virtual_geometry() == null:
							var ivm := _build_virtual_mesh(im, "%s_%d" % [cid, item_id], false, false)
							if ivm != null:
								gm.mesh_library.set_item_mesh(item_id, ivm)
								rebuilt = true
					if rebuilt:
						gm.mesh_library = gm.mesh_library
			"heightfield":
				var proxy := get_node_or_null(cid + "/CoarseProxy") as MeshInstance3D
				if proxy != null and proxy.mesh != null and proxy.mesh.get_virtual_geometry() == null:
					var h_vmesh := _build_virtual_mesh(proxy.mesh, cid + "_proxy", false, false)
					if h_vmesh != null:
						proxy.mesh = h_vmesh
			"msaa", "multiview":
				var vbox := get_node_or_null(cid + "/Candidate/ViewBox") as MeshInstance3D
				if vbox != null and vbox.mesh != null and vbox.mesh.get_virtual_geometry() == null:
					var v_vmesh := _build_virtual_mesh(vbox.mesh, cid + "_vbox", false, false)
					if v_vmesh != null:
						vbox.mesh = v_vmesh


func _build_virtual_mesh(p_mesh: Mesh, p_tag: String, p_skinned: bool, p_force_vgd_on_base_fallbacks: bool) -> ArrayMesh:
	var surf_count := p_mesh.get_surface_count()
	var orig_mats: Array[Material] = []
	for s in surf_count:
		orig_mats.append(p_mesh.surface_get_material(s))
	var imp := ImporterMesh.from_mesh(p_mesh)
	var stripped_single := false
	if p_force_vgd_on_base_fallbacks and surf_count == 1 and orig_mats[0] is BaseMaterial3D:
		imp.set_surface_material(0, null)
		stripped_single = true
	_ngm_counter += 1
	var ngm_path := "user://excl_matrix_%d_%s_%d.ngm" % [OS.get_process_id(), p_tag.validate_filename(), _ngm_counter]
	_temp_ngm_files.append(ngm_path)
	if imp.build_virtual_geometry(ngm_path, p_skinned) != OK:
		return null
	var vmesh := imp.get_mesh()
	if vmesh != null and stripped_single and orig_mats[0] != null:
		vmesh.surface_set_material(0, orig_mats[0])
	return vmesh


func _find_named_first(p_node: Node, p_name: String) -> Node:
	if p_node == null:
		return null
	if String(p_node.name) == p_name:
		return p_node
	for child in p_node.get_children():
		var found := _find_named_first(child, p_name)
		if found != null:
			return found
	return null


func _baked_count(p_gridmap: GridMap) -> int:
	if p_gridmap == null:
		return 0
	var val: Variant = p_gridmap.get("baked_meshes")
	if val is Array:
		return (val as Array).size()
	return 0


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	var stepped: Dictionary = SceneArgs.step_frame(self, _local_frame, _saw, _hold)
	_local_frame = int(stepped["local_frame"])
	_saw = bool(stepped["saw"])
	_apply(int(stepped["frame"]))


func _apply(p_frame: int) -> void:
	_frame = p_frame
	var moving := get_node_or_null("moving_instance/Candidate") as Node3D
	if moving != null:
		moving.position = Rules.moving_offset(p_frame)
	# Settle frames are not the capture timeline. Mutating then would bake
	# the edit into every later frame, including captures that never reach 60.
	if p_frame >= Rules.MUTATE_FRAME and not _mutated and _timeline_is_live():
		_mutate()
	if not _out_dir.is_empty() and _out_dir.is_absolute_path() and RenderingServer.has_method(Rules.DEBUG_STATS_METHOD):
		var vp_stats: Dictionary = RenderingServer.call(Rules.DEBUG_STATS_METHOD, get_viewport().get_viewport_rid())
		_last_cells = _observed_cells(vp_stats)


func _timeline_is_live() -> bool:
	var harness := get_node_or_null("/root/Harness")
	if harness == null or not bool(harness.get("_active")):
		return true
	return harness.has_method("is_driving_animation") and bool(harness.call("is_driving_animation"))


func _mutate() -> void:
	var candidate := get_node_or_null("mutated_mesh/Candidate") as MeshInstance3D
	if candidate == null or not (candidate.mesh is ArrayMesh):
		return
	var mesh := candidate.mesh as ArrayMesh
	var payload := PackedFloat32Array([0.01, 0.02, 0.03]).to_byte_array()
	mesh.surface_update_vertex_region(0, 0, payload)
	_mutated = true
	candidate.set_meta("mutated_frame", Rules.MUTATE_FRAME)
	print("exclusion_matrix: surface_update_vertex_region at frame %d; MUTATED_MESH by frame %d." % [
		Rules.MUTATE_FRAME, Rules.MUTATED_BY_FRAME])


func _exit_tree() -> void:
	if bool(get_meta("skip_runtime", false)):
		_cleanup_temp_ngm()
		return
	var out := SceneArgs.value("--out", "")
	if not out.is_empty() and out.is_absolute_path():
		var report := _observed_report()
		var path := out.path_join("exclusion_observed.json")
		DirAccess.make_dir_recursive_absolute(out)
		var file := FileAccess.open(path, FileAccess.WRITE)
		if file == null:
			push_error("exclusion_matrix: cannot write %s." % path)
		else:
			file.store_string(JSON.stringify(report, "\t"))
			file.close()
	_cleanup_temp_ngm()


func _cleanup_temp_ngm() -> void:
	for path in _temp_ngm_files:
		var abs_path := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs_path):
			DirAccess.remove_absolute(abs_path)
	_temp_ngm_files.clear()


func _observed_report() -> Dictionary:
	var debug_stats: Variant = null
	var source := "absent"
	var double_drawn := 0
	var missing := 0
	if RenderingServer.has_method(Rules.DEBUG_STATS_METHOD):
		var vp_rid := get_viewport().get_viewport_rid()
		var stats: Dictionary = RenderingServer.call(Rules.DEBUG_STATS_METHOD, vp_rid)
		source = Rules.DEBUG_STATS_METHOD
		double_drawn = int(stats.get("double_dispatch", 0))
		missing = int(stats.get("missing", 0))
		stats["cells"] = _last_cells if not _last_cells.is_empty() else _observed_cells(stats)
		debug_stats = stats
	var expectation := Rules.mutated_expectation(_frame)
	return {
		"schema": 1,
		"counters_source": source,
		"frame": _frame,
		"double_drawn": double_drawn,
		"missing": missing,
		"mutated": expectation,
		"dlss": Rules.dlss_status(),
		"debug_stats": debug_stats,
		"cells": Rules.cells(),
	}


func _observed_cells(p_vp_stats: Dictionary) -> Array:
	var rows: Array = []
	for cell_value in Rules.cells():
		var cell: Dictionary = cell_value
		var cid := String(cell["id"])
		var kind := String(cell["kind"])
		var cand := _find_named_first(get_node_or_null(cid), "Candidate")
		var surfaces: Array = []
		match kind:
			"mesh", "moving", "multimesh", "particles":
				var vi := cand as VisualInstance3D
				if vi != null:
					var st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(vi.get_instance())
					surfaces = st.get("surfaces", [])
					if surfaces.is_empty():
						surfaces = [{"index": 0, "route": String(st.get("route", "")), "reason": String(st.get("reason", ""))}]
			"baked_gridmap":
				var bgm := cand as GridMap
				if bgm != null and _baked_count(bgm) > 0:
					var bst: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(bgm.get_bake_mesh_instance(0))
					surfaces = bst.get("surfaces", [])
					if surfaces.is_empty():
						surfaces = [{"index": 0, "route": String(bst.get("route", "")), "reason": String(bst.get("reason", ""))}]
			"gridmap":
				var gm := cand as GridMap
				if gm != null and _baked_count(gm) == 0 and int(p_vp_stats.get("vgd_ignored_warnings", 0)) > 0:
					surfaces = [{"index": 0, "route": Rules.ROUTE_LEGACY, "reason": Rules.REASON_VGD}]
				else:
					surfaces = [{"index": 0, "route": Rules.ROUTE_LEGACY, "reason": "NONE"}]
			"heightfield":
				var proxy := get_node_or_null(cid + "/CoarseProxy") as MeshInstance3D
				var pst: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(proxy.get_instance()) if proxy != null else {}
				if String(pst.get("route", "")) == Rules.ROUTE_VIRTUAL and int(p_vp_stats.get("auxiliary_proxy_draws", 0)) > 0 and int(p_vp_stats.get("auxiliary_authored_virtual_draws", -1)) == 0:
					surfaces = [{"index": 0, "route": Rules.ROUTE_AUXILIARY, "reason": Rules.REASON_PROXY}]
				else:
					surfaces = [{"index": 0, "route": String(pst.get("route", "legacy")), "reason": String(pst.get("reason", "NONE"))}]
			"msaa", "multiview":
				var svp := cand as SubViewport
				if svp != null:
					var vst: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(svp.get_viewport_rid())
					surfaces = [{"index": 0, "route": String(vst.get("view_route", "")), "reason": String(vst.get("view_reason", ""))}]
		rows.append({"id": cid, "surfaces": surfaces})
	return rows
