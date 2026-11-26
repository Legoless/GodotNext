extends SceneTree

# GodotNext: #71 multi-material kit root-cut & DAG depth spike measurement.
# Evaluates:
#   1. The Dense City kit asset (--variant=kit, res://generated/dense_city/kit/kit_facade.glb)
#   2. Seeded 8-material kit (~500k triangles) from dense_city_generator.gd
#   3. Seeded 16-material kit (~500k triangles) from dense_city_generator.gd
# Prints one JSON line per asset and exits non-zero if any asset misses:
#   root_cut_triangles <= max(ceil(0.02 * source_triangles), 4096)
#   dag_depth <= floor(log2(source_triangles / 128.0)) + 3

func _init() -> void:
	_run.call_deferred()

func _find_mesh(p_node: Node) -> Mesh:
	if p_node is MeshInstance3D:
		var mi := p_node as MeshInstance3D
		if mi.mesh != null:
			return mi.mesh
	for child in p_node.get_children():
		var m := _find_mesh(child)
		if m != null:
			return m
	return null

func _load_glb_mesh(p_abs_path: String) -> Mesh:
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err := doc.append_from_file(p_abs_path, state)
	if err != OK:
		printerr("multimaterial_stats: failed to load GLB %s (err=%d)" % [p_abs_path, err])
		return null
	var root := doc.generate_scene(state)
	if root == null:
		return null
	var mesh := _find_mesh(root)
	root.free()
	return mesh

func _run_generator_kit(p_materials: int, p_seed: int, p_out_res: String) -> String:
	var args := PackedStringArray([
		"--headless",
		"--path", ProjectSettings.globalize_path("res://"),
		"--script", "res://generators/dense_city/dense_city_generator.gd",
		"--",
		"--seed=%d" % p_seed,
		"--variant=kit",
		"--materials=%d" % p_materials,
		"--out=%s" % p_out_res,
	])
	var output := []
	var code := OS.execute(OS.get_executable_path(), args, output, true)
	if code != 0:
		printerr("multimaterial_stats: generator failed (materials=%d, seed=%d, code=%d): %s" % [p_materials, p_seed, code, "\n".join(output)])
		return ""
	var out_abs := ProjectSettings.globalize_path(p_out_res)
	for candidate in [
		out_abs.path_join("kit/kit_facade.glb"),
		out_abs.path_join("kit_facade.glb"),
		out_abs.path_join("kit_m%d/kit_facade.glb" % p_materials),
	]:
		if FileAccess.file_exists(candidate):
			return candidate
	# Search one level deep in out_abs.
	var dir := DirAccess.open(out_abs)
	if dir != null:
		dir.list_dir_begin()
		var entry := dir.get_next()
		while entry != "":
			if not entry.begins_with("."):
				var p := out_abs.path_join(entry)
				if entry.ends_with(".glb"):
					dir.list_dir_end()
					return p
				if dir.current_is_dir():
					var sub := DirAccess.open(p)
					if sub != null:
						sub.list_dir_begin()
						var se := sub.get_next()
						while se != "":
							if se.ends_with(".glb"):
								sub.list_dir_end()
								dir.list_dir_end()
								return p.path_join(se)
							se = sub.get_next()
						sub.list_dir_end()
			entry = dir.get_next()
		dir.list_dir_end()
	return ""

func _measure_asset(p_name: String, p_materials: int, p_mesh: Mesh) -> Dictionary:
	if p_mesh == null:
		return {"asset": p_name, "materials": p_materials, "error": "null mesh", "pass": false}
	var stats: Dictionary = NextGeoDebug.build_stats(p_mesh)
	if stats.is_empty():
		return {"asset": p_name, "materials": p_materials, "error": "build_stats failed", "pass": false}
	var src := int(stats["source_triangles"])
	var root_cut := int(stats["root_cut_triangles"])
	var depth := int(stats["dag_depth"])
	var root_target := maxi(int(ceil(0.02 * float(src))), 4096)
	var depth_limit := int(floor(log(maxf(float(src) / 128.0, 1.0)) / log(2.0))) + 3
	var passed := (root_cut <= root_target) and (depth <= depth_limit)
	return {
		"asset": p_name,
		"materials": p_materials,
		"surface_count": int(stats["surface_count"]),
		"source_triangles": src,
		"root_cut_triangles": root_cut,
		"root_cut_target": root_target,
		"dag_depth": depth,
		"dag_depth_limit": depth_limit,
		"cluster_count": int(stats["cluster_count"]),
		"group_count": int(stats["group_count"]),
		"terminal_groups": int(stats["terminal_groups"]),
		"locked_vertex_fraction": float(stats["locked_vertex_fraction"]),
		"build_ms": float(stats["build_ms"]),
		"pass": passed,
	}

func _cleanup_dir(p_abs: String) -> void:
	var dir := DirAccess.open(p_abs)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		if not entry.begins_with("."):
			var full := p_abs.path_join(entry)
			if dir.current_is_dir():
				_cleanup_dir(full)
			else:
				DirAccess.remove_absolute(full)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(p_abs)

func _run() -> void:
	var any_failed := false
	var results: Array[Dictionary] = []
	var tmp_root := "user://nextgeo_mm71_tmp"
	var tmp_abs := ProjectSettings.globalize_path(tmp_root)
	_cleanup_dir(tmp_abs)
	DirAccess.make_dir_recursive_absolute(tmp_abs)

	# 1. Existing Dense City kit asset (plain GLB -> ArrayMesh).
	var default_glb := ProjectSettings.globalize_path("res://generated/dense_city/kit/kit_facade.glb")
	if not FileAccess.file_exists(default_glb):
		default_glb = _run_generator_kit(8, 1, tmp_root + "/kit_default")
	var m_default := _load_glb_mesh(default_glb) if not default_glb.is_empty() else null
	results.append(_measure_asset("dense_city_kit", m_default.get_surface_count() if m_default != null else 8, m_default))

	# 2. Seeded 8-material kit (~500k triangles, seed=2).
	var glb8 := _run_generator_kit(8, 2, tmp_root + "/kit_m8")
	var m8 := _load_glb_mesh(glb8) if not glb8.is_empty() else null
	results.append(_measure_asset("seeded_kit_8mat", 8, m8))

	# 3. Seeded 16-material kit (~500k triangles, seed=3).
	var glb16 := _run_generator_kit(16, 3, tmp_root + "/kit_m16")
	var m16 := _load_glb_mesh(glb16) if not glb16.is_empty() else null
	results.append(_measure_asset("seeded_kit_16mat", 16, m16))

	_cleanup_dir(tmp_abs)

	for row in results:
		print(JSON.stringify(row))
		if not bool(row.get("pass", false)):
			any_failed = true

	quit(1 if any_failed else 0)
