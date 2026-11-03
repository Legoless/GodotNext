extends SceneTree
## Writes the SDF import-set OBJ assets and their manifest (#181).
##
## Run from the repository root:
## [code]godot --headless --path godotnext/testbed --script res://generators/build_sdf_assets.gd[/code]
##
## Deterministic: the meshes come from sdf_mesh_library.gd and the OBJ
## writer uses fixed float formatting, so two runs are byte-identical.
## The manifest records the SHA-256 of each OBJ, its triangle and edge
## census and its signed volume, which is what GS-5's import check
## reads back. Import itself is the editor's OBJ importer; run
## [code]--headless --import[/code] once after regenerating so the
## [code].obj.import[/code] files next to the assets stay in step.

const Scaffold := preload("res://generators/scaffold.gd")
const SdfMeshes := preload("res://generators/sdf_mesh_library.gd")

const ASSET_DIR := "res://assets/sdf_import_set"

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var contract := String(SdfMeshes.contract_error())
	if not contract.is_empty():
		_fail(contract)
	var entries: Array = []
	if _failures.is_empty():
		for mesh_value in SdfMeshes.all_meshes():
			var mesh_data: Dictionary = mesh_value
			var stats := SdfMeshes.analyze(mesh_data)
			var obj := SdfMeshes.to_obj(mesh_data)
			var file := String(mesh_data["name"]) + ".obj"
			var err := Scaffold.write_text_chunked(ASSET_DIR.path_join(file), obj)
			if err != OK:
				_fail("%s: write failed with error %d" % [file, err])
				continue
			print("build_sdf_assets: wrote " + ASSET_DIR.path_join(file))
			var min_v: Vector3 = stats["aabb_min"]
			var max_v: Vector3 = stats["aabb_max"]
			entries.append({
				"name": String(mesh_data["name"]),
				"file": file,
				"closed": bool(mesh_data["closed"]),
				"vertices": int(stats["vertices"]),
				"triangles": int(stats["triangles"]),
				"boundary_edges": int(stats["boundary_edges"]),
				"nonmanifold_edges": int(stats["nonmanifold_edges"]),
				"signed_volume": float(stats["signed_volume"]),
				"aabb_min": [min_v.x, min_v.y, min_v.z],
				"aabb_max": [max_v.x, max_v.y, max_v.z],
				"obj_sha256": obj.sha256_text(),
			})
	if _failures.is_empty():
		var document := {
			"version": 1,
			"seed": Scaffold.stream_seed(44),
			"format": "obj, counter-clockwise faces seen from outside",
			"closed_count": SdfMeshes.CLOSED_NAMES.size(),
			"open_count": SdfMeshes.OPEN_NAMES.size(),
			"meshes": entries,
		}
		var manifest_path := ASSET_DIR.path_join("manifest.json")
		var err := Scaffold.write_text_chunked(manifest_path, Scaffold.to_json(document) + "\n")
		if err != OK:
			_fail("manifest.json: write failed with error %d" % err)
		else:
			print("build_sdf_assets: wrote " + manifest_path)
	if _failures.is_empty():
		print("build_sdf_assets: %d meshes written." % entries.size())
		quit(0)
	else:
		for failure in _failures:
			push_error("build_sdf_assets: " + failure)
		quit(1)


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("build_sdf_assets FAIL: " + p_message)
