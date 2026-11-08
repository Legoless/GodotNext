extends SceneTree
## Virtualized G5-6 scene and mesh generator (#77 step 8).
##
## Authors the subdivided glTF 2.0 binary mesh assets (`.glb` + `.import` sidecars
## with `importer="ArrayMesh"` and `meshes/generate_virtual_geometry=true`) and the
## virtualized copies of the five G5-6 scenes beside the P0 originals:
##   - `res://scenes/color_box_room/cbr_a_vg.tscn` (+ `_patches_q1.json`, `_patches_q3.json`)
##   - `res://scenes/color_box_room/cbr_b_vg.tscn` (+ `_patches_q1.json`, `_patches_q3.json`)
##   - `res://scenes/color_box_room/cbr_offscreen_vg.tscn` (+ `_patches_q1.json`, `_patches_q3.json`)
##   - `res://scenes/color_box_room/cbr_single_40m_vg.tscn` (+ `_patches_q1.json`, `_patches_q3.json`)
##   - `res://scenes/concave_courtyard/courtyard_vg.tscn` (+ `_patches_q1.json`)
##
## Two-step bootstrap from the repository root when `.glb` assets change:
##   1. `godot --headless --path godotnext/testbed --script res://generators/build_g5_6_vg.gd -- --phase=assets`
##   2. `godot --headless --path godotnext/testbed --import`
##   3. `godot --headless --path godotnext/testbed --script res://generators/build_g5_6_vg.gd -- --phase=scenes`
## Without `--phase=...`, runs `--phase=scenes` (assuming assets are already imported).

const Scaffold := preload("res://generators/scaffold.gd")
const ColorBoxRoom := preload("res://generators/build_color_box_room.gd")
const Single40m := preload("res://generators/build_single_40m.gd")
const Courtyard := preload("res://generators/build_courtyard.gd")

const Q1_PATCH_COUNT := 128
const VG_BOX_DIR := "res://scenes/color_box_room/vg_meshes"
const SINGLE_40M_GLB := "res://scenes/color_box_room/cbr_single_40m_vg.glb"
const COURTYARD_GLB := "res://scenes/concave_courtyard/courtyard_vg.glb"

const BOX_SPECS := {
	"floor_a": Vector3(8.50, 0.20, 4.40),
	"floor_b": Vector3(8.60, 0.20, 4.40),
	"end_wall": Vector3(0.20, 3.40, 4.40),
	"shared_wall_a": Vector3(0.10, 3.40, 4.40),
	"shared_wall_b": Vector3(0.20, 3.40, 4.40),
	"room_side_wall": Vector3(4.00, 3.40, 0.20),
	"shared_side_wall_a": Vector3(0.10, 3.40, 0.20),
	"shared_side_wall_b": Vector3(0.20, 3.40, 0.20),
	"block_large": Vector3(1.20, 1.20, 1.20),
	"block_small": Vector3(0.60, 0.60, 0.60),
}

var _failures: Array[String] = []


class GltfSubdivBuilder:
	var primitives: Array[Dictionary] = []
	var grid_step := 0.5
	var min_face_segs := 1

	func _init(p_grid_step: float = 0.5, p_min_face_segs: int = 1) -> void:
		grid_step = maxf(p_grid_step, 0.05)
		min_face_segs = maxi(p_min_face_segs, 1)

	func _current_primitive() -> Dictionary:
		if primitives.is_empty():
			primitives.append({
				"positions": PackedVector3Array(),
				"normals": PackedVector3Array(),
				"uvs": PackedVector2Array(),
				"indices": PackedInt32Array(),
			})
		return primitives[primitives.size() - 1]

	func begin_primitive() -> void:
		if primitives.is_empty():
			_current_primitive()
			return
		var last: Dictionary = primitives[primitives.size() - 1]
		var last_idx: PackedInt32Array = last["indices"]
		if not last_idx.is_empty():
			primitives.append({
				"positions": PackedVector3Array(),
				"normals": PackedVector3Array(),
				"uvs": PackedVector2Array(),
				"indices": PackedInt32Array(),
			})

	func get_primitive_count() -> int:
		return primitives.size()

	func get_primitive_triangle_count(p_prim: int) -> int:
		var idx: PackedInt32Array = primitives[p_prim]["indices"]
		return idx.size() / 3

	func _segs(p_length: float) -> int:
		if p_length <= 0.35:
			return min_face_segs
		return maxi(int(ceil(p_length / grid_step)), maxi(min_face_segs, 2))

	## Emits a subdivided planar quad on [param p_center] +/- [param p_u] +/- [param p_v],
	## where `p_u.cross(p_v)` points along `+p_normal`.
	## Triangles use glTF 2.0 CCW front-face winding (`(v1 - v0).cross(v2 - v0)` along `+p_normal`),
	## which Godot's glTF importer flips to Godot's CW front-face convention on import.
	func add_quad(p_center: Vector3, p_u: Vector3, p_v: Vector3, p_normal: Vector3) -> void:
		var prim := _current_primitive()
		var positions: PackedVector3Array = prim["positions"]
		var normals: PackedVector3Array = prim["normals"]
		var uvs: PackedVector2Array = prim["uvs"]
		var indices: PackedInt32Array = prim["indices"]
		var nu := _segs(p_u.length() * 2.0)
		var nv := _segs(p_v.length() * 2.0)
		var base_vertex := positions.size()
		var n := p_normal.normalized()
		for iv in range(nv + 1):
			var tv := -1.0 + 2.0 * float(iv) / float(nv)
			for iu in range(nu + 1):
				var tu := -1.0 + 2.0 * float(iu) / float(nu)
				var vtx := p_center + p_u * tu + p_v * tv
				positions.append(vtx)
				normals.append(n)
				uvs.append(Vector2(vtx.x + vtx.z, vtx.y + vtx.z))
		var row_stride := nu + 1
		for iv in range(nv):
			for iu in range(nu):
				var a := base_vertex + iv * row_stride + iu
				var b := a + 1
				var c := a + row_stride + 1
				var d := a + row_stride
				# CCW in glTF: (b - a) = +du, (c - a) = +du + dv -> cross = +du x dv = +normal.
				indices.append(a)
				indices.append(b)
				indices.append(c)
				indices.append(a)
				indices.append(c)
				indices.append(d)
		prim["positions"] = positions
		prim["normals"] = normals
		prim["uvs"] = uvs
		prim["indices"] = indices

	func add_box(p_center: Vector3, p_size: Vector3) -> void:
		var hx := p_size.x * 0.5
		var hy := p_size.y * 0.5
		var hz := p_size.z * 0.5
		add_quad(p_center + Vector3(hx, 0.0, 0.0), Vector3(0.0, 0.0, -hz), Vector3(0.0, hy, 0.0), Vector3.RIGHT)
		add_quad(p_center + Vector3(-hx, 0.0, 0.0), Vector3(0.0, 0.0, hz), Vector3(0.0, hy, 0.0), Vector3.LEFT)
		add_quad(p_center + Vector3(0.0, hy, 0.0), Vector3(hx, 0.0, 0.0), Vector3(0.0, 0.0, -hz), Vector3.UP)
		add_quad(p_center + Vector3(0.0, -hy, 0.0), Vector3(hx, 0.0, 0.0), Vector3(0.0, 0.0, hz), Vector3.DOWN)
		add_quad(p_center + Vector3(0.0, 0.0, hz), Vector3(hx, 0.0, 0.0), Vector3(0.0, hy, 0.0), Vector3(0.0, 0.0, 1.0))
		add_quad(p_center + Vector3(0.0, 0.0, -hz), Vector3(-hx, 0.0, 0.0), Vector3(0.0, hy, 0.0), Vector3(0.0, 0.0, -1.0))

	func add_primitive_box(p_center: Vector3, p_size: Vector3) -> void:
		begin_primitive()
		add_box(p_center, p_size)

	func to_glb_bytes(p_mesh_name: String) -> PackedByteArray:
		var bin_spb := StreamPeerBuffer.new()
		bin_spb.big_endian = false
		var gltf_primitives: Array = []
		var accessors: Array = []
		var buffer_views: Array = []

		for p_idx in range(primitives.size()):
			var prim: Dictionary = primitives[p_idx]
			var positions: PackedVector3Array = prim["positions"]
			var normals: PackedVector3Array = prim["normals"]
			var uvs: PackedVector2Array = prim["uvs"]
			var indices: PackedInt32Array = prim["indices"]
			var v_count := positions.size()
			var i_count := indices.size()
			var min_pos := positions[0] if v_count > 0 else Vector3.ZERO
			var max_pos := min_pos
			var pos_offset := bin_spb.get_position()
			for i in range(v_count):
				var p := positions[i]
				min_pos = min_pos.min(p)
				max_pos = max_pos.max(p)
				bin_spb.put_float(p.x)
				bin_spb.put_float(p.y)
				bin_spb.put_float(p.z)
			var nrm_offset := bin_spb.get_position()
			for i in range(v_count):
				var n := normals[i]
				bin_spb.put_float(n.x)
				bin_spb.put_float(n.y)
				bin_spb.put_float(n.z)
			var uv_offset := bin_spb.get_position()
			for i in range(v_count):
				var uv := uvs[i]
				bin_spb.put_float(uv.x)
				bin_spb.put_float(uv.y)
			var idx_offset := bin_spb.get_position()
			for i in range(i_count):
				bin_spb.put_u32(indices[i])

			var bv_base := buffer_views.size()
			buffer_views.append({"buffer": 0, "byteLength": v_count * 12, "byteOffset": pos_offset})
			buffer_views.append({"buffer": 0, "byteLength": v_count * 12, "byteOffset": nrm_offset})
			buffer_views.append({"buffer": 0, "byteLength": v_count * 8, "byteOffset": uv_offset})
			buffer_views.append({"buffer": 0, "byteLength": i_count * 4, "byteOffset": idx_offset})

			var acc_base := accessors.size()
			accessors.append({
				"bufferView": bv_base + 0,
				"componentType": 5126,
				"count": v_count,
				"max": [max_pos.x, max_pos.y, max_pos.z],
				"min": [min_pos.x, min_pos.y, min_pos.z],
				"type": "VEC3",
			})
			accessors.append({
				"bufferView": bv_base + 1,
				"componentType": 5126,
				"count": v_count,
				"type": "VEC3",
			})
			accessors.append({
				"bufferView": bv_base + 2,
				"componentType": 5126,
				"count": v_count,
				"type": "VEC2",
			})
			accessors.append({
				"bufferView": bv_base + 3,
				"componentType": 5125,
				"count": i_count,
				"type": "SCALAR",
			})

			gltf_primitives.append({
				"attributes": {
					"NORMAL": acc_base + 1,
					"POSITION": acc_base + 0,
					"TEXCOORD_0": acc_base + 2,
				},
				"indices": acc_base + 3,
				"material": 0,
			})

		var total_bytes := bin_spb.get_position()
		while (bin_spb.get_position() % 4) != 0:
			bin_spb.put_u8(0)
		var bin_bytes := bin_spb.data_array
		var doc := {
			"asset": {
				"generator": "GodotNext build_g5_6_vg (#77)",
				"version": "2.0",
			},
			"scene": 0,
			"scenes": [{"name": p_mesh_name, "nodes": [0]}],
			"nodes": [{"mesh": 0, "name": p_mesh_name}],
			"materials": [
				{
					"doubleSided": false,
					"name": "Grey075",
					"pbrMetallicRoughness": {
						"baseColorFactor": [0.75, 0.75, 0.75, 1.0],
						"metallicFactor": 0.0,
						"roughnessFactor": 1.0,
					},
				},
			],
			"meshes": [
				{
					"name": p_mesh_name,
					"primitives": gltf_primitives,
				},
			],
			"accessors": accessors,
			"bufferViews": buffer_views,
			"buffers": [
				{"byteLength": total_bytes},
			],
		}
		var json_bytes := Scaffold.to_json(doc).to_utf8_buffer()
		while (json_bytes.size() % 4) != 0:
			json_bytes.append(0x20)
		var glb := StreamPeerBuffer.new()
		glb.big_endian = false
		var total_glb_len := 12 + 8 + json_bytes.size() + 8 + bin_bytes.size()
		glb.put_u32(0x46546C67) # "glTF"
		glb.put_u32(2)
		glb.put_u32(total_glb_len)
		glb.put_u32(json_bytes.size())
		glb.put_u32(0x4E4F534A) # "JSON"
		glb.put_data(json_bytes)
		glb.put_u32(bin_bytes.size())
		glb.put_u32(0x004E4942) # "BIN\0"
		glb.put_data(bin_bytes)
		return glb.data_array


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var phase := "scenes"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--phase="):
			phase = arg.trim_prefix("--phase=")
	if phase == "assets":
		_write_all_assets()
	elif phase == "scenes":
		await _build_all_scenes()
	else:
		_fail("unknown --phase='%s' (expected 'assets' or 'scenes')." % phase)
	if _failures.is_empty():
		print("build_g5_6_vg: phase '%s' finished cleanly." % phase)
		quit(0)
	else:
		for failure in _failures:
			push_error("build_g5_6_vg: " + failure)
		quit(1)


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("build_g5_6_vg FAIL: " + p_message)


func _write_bytes_if_changed(p_path: String, p_bytes: PackedByteArray) -> bool:
	if FileAccess.file_exists(p_path) and FileAccess.get_file_as_bytes(p_path) == p_bytes:
		return true
	var dir_path := ProjectSettings.globalize_path(p_path.get_base_dir())
	var dir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if dir_err != OK and dir_err != ERR_ALREADY_EXISTS:
		_fail("%s: mkdir failed with error %d." % [p_path, dir_err])
		return false
	var file := FileAccess.open(p_path, FileAccess.WRITE)
	if file == null:
		_fail("%s: open failed with error %d." % [p_path, FileAccess.get_open_error()])
		return false
	file.store_buffer(p_bytes)
	file.close()
	print("build_g5_6_vg: wrote " + p_path)
	return true


func _ensure_import_sidecar(p_glb_path: String) -> bool:
	var import_path := p_glb_path + ".import"
	if FileAccess.file_exists(import_path):
		var existing := FileAccess.get_file_as_string(import_path)
		if existing.contains("importer=\"ArrayMesh\"") and existing.contains("meshes/generate_virtual_geometry=true"):
			return true
	var seed_import := """[remap]

importer="ArrayMesh"
importer_version=1
type="ArrayMesh"

[params]

meshes/generate_virtual_geometry=true
meshes/light_baking=0
"""
	var err := Scaffold.write_text_chunked(import_path, seed_import)
	if err != OK:
		_fail("%s: write failed with error %d." % [import_path, err])
		return false
	print("build_g5_6_vg: wrote " + import_path)
	return true


func _write_glb_asset(p_path: String, p_name: String, p_builder: GltfSubdivBuilder) -> void:
	var prim_count := p_builder.get_primitive_count()
	if prim_count < 1 or prim_count > 32:
		_fail("%s: expected 1..32 primitives for VGD eligible_surface_mask, got %d." % [p_path, prim_count])
		return
	for p_idx in range(prim_count):
		var tri_count := p_builder.get_primitive_triangle_count(p_idx)
		if tri_count < 160:
			_fail("%s: primitive %d expected >= 160 triangles for VGD eligibility, got %d." % [p_path, p_idx, tri_count])
			return
	if _write_bytes_if_changed(p_path, p_builder.to_glb_bytes(p_name)):
		_ensure_import_sidecar(p_path)


func _write_all_assets() -> void:
	var names: Array = BOX_SPECS.keys()
	names.sort()
	for name_val in names:
		var box_name := String(name_val)
		var size: Vector3 = BOX_SPECS[box_name]
		var max_side := maxf(size.x, maxf(size.y, size.z))
		var step := 0.15 if max_side <= 1.5 else 0.25
		var min_segs := 4 if max_side <= 1.5 else 2
		var builder := GltfSubdivBuilder.new(step, min_segs)
		builder.add_box(Vector3.ZERO, size)
		_write_glb_asset("%s/%s.glb" % [VG_BOX_DIR, box_name], box_name, builder)

	# 42 m single-mesh hall with six 0.10 m interior walls (#77 step 8):
	# 12 primitives on one ArrayMesh so each wall/floor/ceiling slab forms its own
	# unoccluded Stage-2 cluster group(s).
	var hall_builder := GltfSubdivBuilder.new(0.5, 2)
	_append_single_40m_primitives(hall_builder)
	_write_glb_asset(SINGLE_40M_GLB, "ColorBoxRoomSingle40mVg", hall_builder)

	# 16 x 16 m concave courtyard with colonnade and 16 columns (#77 step 8):
	# 25 primitives on one ArrayMesh so outer walls, inner walls, roofs, and corner
	# column pairs form spatially disjoint Stage-2 cluster groups without self-occlusion.
	var cy_builder := GltfSubdivBuilder.new(0.5, 2)
	_append_courtyard_primitives(cy_builder)
	_write_glb_asset(COURTYARD_GLB, "ConcaveCourtyardVg", cy_builder)


func _append_single_40m_primitives(p_builder: GltfSubdivBuilder) -> void:
	var half_depth := Single40m.INTERIOR.z * 0.5
	var height := Single40m.INTERIOR.y
	var total_x := Single40m.INTERIOR.x + 2.0 * Single40m.OUTER_WALL
	var total_z := Single40m.INTERIOR.z + 2.0 * Single40m.OUTER_WALL
	var span_y := height + 2.0 * Single40m.OUTER_WALL
	var center_y := height * 0.5
	p_builder.add_primitive_box(Vector3(0.0, -Single40m.OUTER_WALL * 0.5, 0.0), Vector3(total_x, Single40m.OUTER_WALL, total_z))
	p_builder.add_primitive_box(Vector3(0.0, height + Single40m.OUTER_WALL * 0.5, 0.0), Vector3(total_x, Single40m.OUTER_WALL, total_z))
	p_builder.add_primitive_box(Vector3(-Single40m.INTERIOR.x * 0.5 - Single40m.OUTER_WALL * 0.5, center_y, 0.0), Vector3(Single40m.OUTER_WALL, span_y, total_z))
	p_builder.add_primitive_box(Vector3(Single40m.INTERIOR.x * 0.5 + Single40m.OUTER_WALL * 0.5, center_y, 0.0), Vector3(Single40m.OUTER_WALL, span_y, total_z))
	p_builder.add_primitive_box(Vector3(0.0, center_y, -half_depth - Single40m.OUTER_WALL * 0.5), Vector3(total_x, span_y, Single40m.OUTER_WALL))
	p_builder.add_primitive_box(Vector3(0.0, center_y, half_depth + Single40m.OUTER_WALL * 0.5), Vector3(total_x, span_y, Single40m.OUTER_WALL))
	for wall_x in Single40m.WALL_X:
		p_builder.add_primitive_box(Vector3(wall_x, center_y, 0.0), Vector3(Single40m.INTERIOR_WALL, span_y, total_z))


func _append_courtyard_primitives(p_builder: GltfSubdivBuilder) -> void:
	var w := Courtyard.WALL
	var wh := Courtyard.WALL_HEIGHT
	p_builder.add_primitive_box(Vector3(0.0, -0.15, 0.0), Vector3(16.0, w, 16.0))
	p_builder.add_primitive_box(Vector3(0.0, 3.0, -7.85), Vector3(16.0, wh, w))
	p_builder.add_primitive_box(Vector3(0.0, 3.0, 7.85), Vector3(16.0, wh, w))
	p_builder.add_primitive_box(Vector3(-7.85, 3.0, 0.0), Vector3(w, wh, 15.4))
	p_builder.add_primitive_box(Vector3(7.85, 3.0, 0.0), Vector3(w, wh, 15.4))
	p_builder.add_primitive_box(Vector3(0.0, 3.0, -4.15), Vector3(8.6, wh, w))
	p_builder.add_primitive_box(Vector3(0.0, 3.0, 4.15), Vector3(8.6, wh, w))
	p_builder.add_primitive_box(Vector3(-4.15, 3.0, 0.0), Vector3(w, wh, 8.0))
	p_builder.add_primitive_box(Vector3(4.15, 3.0, 0.0), Vector3(w, wh, 8.0))
	p_builder.add_primitive_box(Vector3(0.0, 6.15, -6.0), Vector3(16.0, w, 4.0))
	p_builder.add_primitive_box(Vector3(0.0, 6.15, 6.0), Vector3(16.0, w, 4.0))
	p_builder.add_primitive_box(Vector3(-6.0, 6.15, 0.0), Vector3(4.0, w, 8.0))
	p_builder.add_primitive_box(Vector3(6.0, 6.15, 0.0), Vector3(4.0, w, 8.0))
	p_builder.add_primitive_box(Vector3(0.0, 3.15, -3.0), Vector3(8.0, w, 2.0))
	p_builder.add_primitive_box(Vector3(0.0, 3.15, 3.0), Vector3(8.0, w, 2.0))
	p_builder.add_primitive_box(Vector3(-3.0, 3.15, 0.0), Vector3(2.0, w, 8.0))
	p_builder.add_primitive_box(Vector3(3.0, 3.15, 0.0), Vector3(2.0, w, 8.0))
	# Pair each (sx * d, 1.5, sz * 2.15) column with (sx * 2.15, 1.5, sz * d) so no two
	# columns in the same primitive overlap along X or Z.
	for sx in [-1.0, 1.0]:
		for sz in [-1.0, 1.0]:
			for dist in [1.0, 3.0]:
				p_builder.begin_primitive()
				p_builder.add_box(Vector3(sx * dist, 1.5, sz * 2.15), Vector3(w, 3.0, w))
				p_builder.add_box(Vector3(sx * 2.15, 1.5, sz * dist), Vector3(w, 3.0, w))


func _append_single_40m_boxes(p_builder) -> void:
	var half_depth := Single40m.INTERIOR.z * 0.5
	var height := Single40m.INTERIOR.y
	var total_x := Single40m.INTERIOR.x + 2.0 * Single40m.OUTER_WALL
	var total_z := Single40m.INTERIOR.z + 2.0 * Single40m.OUTER_WALL
	var span_y := height + 2.0 * Single40m.OUTER_WALL
	var center_y := height * 0.5
	p_builder.add_box(Vector3(0.0, -Single40m.OUTER_WALL * 0.5, 0.0), Vector3(total_x, Single40m.OUTER_WALL, total_z))
	p_builder.add_box(Vector3(0.0, height + Single40m.OUTER_WALL * 0.5, 0.0), Vector3(total_x, Single40m.OUTER_WALL, total_z))
	p_builder.add_box(Vector3(-Single40m.INTERIOR.x * 0.5 - Single40m.OUTER_WALL * 0.5, center_y, 0.0), Vector3(Single40m.OUTER_WALL, span_y, total_z))
	p_builder.add_box(Vector3(Single40m.INTERIOR.x * 0.5 + Single40m.OUTER_WALL * 0.5, center_y, 0.0), Vector3(Single40m.OUTER_WALL, span_y, total_z))
	p_builder.add_box(Vector3(0.0, center_y, -half_depth - Single40m.OUTER_WALL * 0.5), Vector3(total_x, span_y, Single40m.OUTER_WALL))
	p_builder.add_box(Vector3(0.0, center_y, half_depth + Single40m.OUTER_WALL * 0.5), Vector3(total_x, span_y, Single40m.OUTER_WALL))
	for wall_x in Single40m.WALL_X:
		p_builder.add_box(Vector3(wall_x, center_y, 0.0), Vector3(Single40m.INTERIOR_WALL, span_y, total_z))


func _append_courtyard_boxes(p_builder) -> void:
	var w := Courtyard.WALL
	var wh := Courtyard.WALL_HEIGHT
	p_builder.add_box(Vector3(0.0, -0.15, 0.0), Vector3(16.0, w, 16.0))
	p_builder.add_box(Vector3(0.0, 3.0, -7.85), Vector3(16.0, wh, w))
	p_builder.add_box(Vector3(0.0, 3.0, 7.85), Vector3(16.0, wh, w))
	p_builder.add_box(Vector3(-7.85, 3.0, 0.0), Vector3(w, wh, 15.4))
	p_builder.add_box(Vector3(7.85, 3.0, 0.0), Vector3(w, wh, 15.4))
	p_builder.add_box(Vector3(0.0, 3.0, -4.15), Vector3(8.6, wh, w))
	p_builder.add_box(Vector3(0.0, 3.0, 4.15), Vector3(8.6, wh, w))
	p_builder.add_box(Vector3(-4.15, 3.0, 0.0), Vector3(w, wh, 8.0))
	p_builder.add_box(Vector3(4.15, 3.0, 0.0), Vector3(w, wh, 8.0))
	p_builder.add_box(Vector3(0.0, 6.15, -6.0), Vector3(16.0, w, 4.0))
	p_builder.add_box(Vector3(0.0, 6.15, 6.0), Vector3(16.0, w, 4.0))
	p_builder.add_box(Vector3(-6.0, 6.15, 0.0), Vector3(4.0, w, 8.0))
	p_builder.add_box(Vector3(6.0, 6.15, 0.0), Vector3(4.0, w, 8.0))
	p_builder.add_box(Vector3(0.0, 3.15, -3.0), Vector3(8.0, w, 2.0))
	p_builder.add_box(Vector3(0.0, 3.15, 3.0), Vector3(8.0, w, 2.0))
	p_builder.add_box(Vector3(-3.0, 3.15, 0.0), Vector3(2.0, w, 8.0))
	p_builder.add_box(Vector3(3.0, 3.15, 0.0), Vector3(2.0, w, 8.0))
	for offset in [-3.0, -1.0, 1.0, 3.0]:
		p_builder.add_box(Vector3(offset, 1.5, -2.15), Vector3(w, 3.0, w))
		p_builder.add_box(Vector3(offset, 1.5, 2.15), Vector3(w, 3.0, w))
		p_builder.add_box(Vector3(-2.15, 1.5, offset), Vector3(w, 3.0, w))
		p_builder.add_box(Vector3(2.15, 1.5, offset), Vector3(w, 3.0, w))


func _load_vg_mesh(p_path: String) -> ArrayMesh:
	var mesh := load(p_path) as ArrayMesh
	if mesh == null:
		_fail("%s: failed to load imported ArrayMesh (run --phase=assets then --import first)." % p_path)
		return null
	var vg := mesh.get_virtual_geometry()
	if vg == null:
		_fail("%s: imported ArrayMesh has no VirtualGeometryData." % p_path)
		return null
	if vg.format_version != 2:
		_fail("%s: expected VirtualGeometryData format_version == 2, got %d." % [p_path, vg.format_version])
		return null
	if vg.card_descriptors.is_empty() or (vg.card_descriptors.size() % 32) != 0:
		_fail("%s: expected non-empty 32-byte card_descriptors, got %d bytes." % [p_path, vg.card_descriptors.size()])
		return null
	if vg.rt_proxy_triangle_groups.is_empty():
		_fail("%s: expected non-empty rt_proxy_triangle_groups." % p_path)
		return null
	var surf_count := mesh.get_surface_count()
	var elig := vg.surface_eligibility
	if surf_count < 1 or surf_count > 32 or elig.size() != surf_count:
		_fail("%s: expected 1..32 surfaces matching surface_eligibility size (%d vs %d)." % [p_path, surf_count, elig.size()])
		return null
	for s in range(surf_count):
		if elig[s] != 1:
			_fail("%s: surface %d was not virtualized (eligibility=%d)." % [p_path, s, elig[s]])
			return null
	return mesh


func _add_vg_box_instance(
		p_root: Node,
		p_name: String,
		p_mesh: ArrayMesh,
		p_center: Vector3,
		p_size: Vector3,
		p_material: Material) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = p_mesh
	instance.position = p_center
	if p_material != null:
		instance.set_surface_override_material(0, p_material)
	Scaffold.add_owned(p_root, instance, p_root)
	var body := StaticBody3D.new()
	body.name = "Collider"
	Scaffold.add_owned(instance, body, p_root)
	var shape_node := CollisionShape3D.new()
	shape_node.name = "Shape"
	var shape := BoxShape3D.new()
	shape.size = p_size
	shape_node.shape = shape
	Scaffold.add_owned(body, shape_node, p_root)
	return instance


func _add_coarse_trimesh_collider(p_root: Node, p_instance: MeshInstance3D, p_coarse_mesh: ArrayMesh) -> void:
	var body := StaticBody3D.new()
	body.name = "Collider"
	Scaffold.add_owned(p_instance, body, p_root)
	var shape_node := CollisionShape3D.new()
	shape_node.name = "Shape"
	shape_node.shape = p_coarse_mesh.create_trimesh_shape()
	Scaffold.add_owned(body, shape_node, p_root)


func _build_cbr_vg(p_variant: String) -> Dictionary:
	var is_a := p_variant == "a"
	var shared := 0.10 if is_a else 0.20
	var half_shared := shared * 0.5
	var lit_min_x := -half_shared - ColorBoxRoom.ROOM_SIZE.x
	var lit_max_x := -half_shared
	var dark_min_x := half_shared
	var dark_max_x := half_shared + ColorBoxRoom.ROOM_SIZE.x
	var lit_center_x := (lit_min_x + lit_max_x) * 0.5
	var dark_center_x := (dark_min_x + dark_max_x) * 0.5
	var height := ColorBoxRoom.ROOM_SIZE.y
	var half_depth := ColorBoxRoom.ROOM_SIZE.z * 0.5
	var full_min_x := lit_min_x - ColorBoxRoom.OUTER_WALL
	var full_max_x := dark_max_x + ColorBoxRoom.OUTER_WALL
	var full_size_x := full_max_x - full_min_x
	var full_center_x := (full_min_x + full_max_x) * 0.5
	var wall_span_y := height + 2.0 * ColorBoxRoom.OUTER_WALL
	var wall_center_y := height * 0.5
	var full_size_z := ColorBoxRoom.ROOM_SIZE.z + 2.0 * ColorBoxRoom.OUTER_WALL

	var floor_mesh := _load_vg_mesh("%s/%s.glb" % [VG_BOX_DIR, "floor_a" if is_a else "floor_b"])
	var end_wall_mesh := _load_vg_mesh("%s/end_wall.glb" % VG_BOX_DIR)
	var shared_wall_mesh := _load_vg_mesh("%s/%s.glb" % [VG_BOX_DIR, "shared_wall_a" if is_a else "shared_wall_b"])
	var side_wall_mesh := _load_vg_mesh("%s/room_side_wall.glb" % VG_BOX_DIR)
	var shared_side_mesh := _load_vg_mesh("%s/%s.glb" % [VG_BOX_DIR, "shared_side_wall_a" if is_a else "shared_side_wall_b"])
	var block_large_mesh := _load_vg_mesh("%s/block_large.glb" % VG_BOX_DIR)
	var block_small_mesh := _load_vg_mesh("%s/block_small.glb" % VG_BOX_DIR)

	var root := Node3D.new()
	root.name = "ColorBoxRoom" + p_variant.capitalize() + "Vg"
	root.set_meta("shared_wall_thickness_m", shared)
	root.set_meta("room_interior_m", ColorBoxRoom.ROOM_SIZE)
	root.set_meta("virtual_geometry", true)

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))
	var red := Scaffold.make_constant_material("RedWall", Color(0.75, 0.05, 0.05))
	var green := Scaffold.make_constant_material("GreenWall", Color(0.05, 0.75, 0.05))

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	_add_vg_box_instance(root, "Floor", floor_mesh,
			Vector3(full_center_x, -ColorBoxRoom.OUTER_WALL * 0.5, 0.0),
			Vector3(full_size_x, ColorBoxRoom.OUTER_WALL, full_size_z), gray)
	_add_vg_box_instance(root, "Ceiling", floor_mesh,
			Vector3(full_center_x, height + ColorBoxRoom.OUTER_WALL * 0.5, 0.0),
			Vector3(full_size_x, ColorBoxRoom.OUTER_WALL, full_size_z), gray)
	_add_vg_box_instance(root, "LitEndWall", end_wall_mesh,
			Vector3(lit_min_x - ColorBoxRoom.OUTER_WALL * 0.5, wall_center_y, 0.0),
			Vector3(ColorBoxRoom.OUTER_WALL, wall_span_y, full_size_z), gray)
	_add_vg_box_instance(root, "DarkEndWall", end_wall_mesh,
			Vector3(dark_max_x + ColorBoxRoom.OUTER_WALL * 0.5, wall_center_y, 0.0),
			Vector3(ColorBoxRoom.OUTER_WALL, wall_span_y, full_size_z), gray)
	_add_vg_box_instance(root, "SharedWall", shared_wall_mesh,
			Vector3(0.0, wall_center_y, 0.0),
			Vector3(shared, wall_span_y, full_size_z), gray)

	var north_z := -half_depth - ColorBoxRoom.OUTER_WALL * 0.5
	var south_z := half_depth + ColorBoxRoom.OUTER_WALL * 0.5
	_add_vg_box_instance(root, "LitNorthWall", side_wall_mesh,
			Vector3(lit_center_x, wall_center_y, north_z),
			Vector3(ColorBoxRoom.ROOM_SIZE.x, wall_span_y, ColorBoxRoom.OUTER_WALL), red)
	_add_vg_box_instance(root, "SharedNorthWall", shared_side_mesh,
			Vector3(0.0, wall_center_y, north_z),
			Vector3(shared, wall_span_y, ColorBoxRoom.OUTER_WALL), gray)
	_add_vg_box_instance(root, "DarkNorthWall", side_wall_mesh,
			Vector3(dark_center_x, wall_center_y, north_z),
			Vector3(ColorBoxRoom.ROOM_SIZE.x, wall_span_y, ColorBoxRoom.OUTER_WALL), gray)
	_add_vg_box_instance(root, "LitSouthWall", side_wall_mesh,
			Vector3(lit_center_x, wall_center_y, south_z),
			Vector3(ColorBoxRoom.ROOM_SIZE.x, wall_span_y, ColorBoxRoom.OUTER_WALL), green)
	_add_vg_box_instance(root, "SharedSouthWall", shared_side_mesh,
			Vector3(0.0, wall_center_y, south_z),
			Vector3(shared, wall_span_y, ColorBoxRoom.OUTER_WALL), gray)
	_add_vg_box_instance(root, "DarkSouthWall", side_wall_mesh,
			Vector3(dark_center_x, wall_center_y, south_z),
			Vector3(ColorBoxRoom.ROOM_SIZE.x, wall_span_y, ColorBoxRoom.OUTER_WALL), gray)

	_add_vg_box_instance(root, "BlockLarge", block_large_mesh,
			Vector3(lit_center_x - 0.5, 0.6, -0.5), Vector3(1.2, 1.2, 1.2), gray)
	_add_vg_box_instance(root, "BlockSmall", block_small_mesh,
			Vector3(lit_center_x + 0.7, 0.3, 0.6), Vector3(0.6, 0.6, 0.6), gray)

	var capture_pos := Vector3(lit_min_x + 0.8, 2.0, 0.0)
	if p_variant == "offscreen":
		var spot := SpotLight3D.new()
		spot.name = "OffscreenSpot"
		var spot_pos := capture_pos + Vector3(-0.45, 0.3, 0.0)
		var target := Vector3(lit_max_x, 1.4, 0.0)
		spot.transform = Transform3D(
				Basis.looking_at((target - spot_pos).normalized(), Vector3.UP), spot_pos)
		spot.light_energy = 1.0
		spot.spot_range = 6.0
		spot.spot_angle = 35.0
		spot.shadow_enabled = true
		Scaffold.add_owned(root, spot, root)
	else:
		var omni := OmniLight3D.new()
		omni.name = "RoomLight"
		omni.position = Vector3(lit_center_x, 2.6, 0.0)
		omni.light_energy = 1.0
		omni.omni_range = 6.0
		omni.shadow_enabled = true
		Scaffold.add_owned(root, omni, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(lit_min_x + 0.5, 2.5, -1.5), Vector3(lit_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(lit_min_x + 0.5, 2.5, 1.5), Vector3(lit_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(dark_min_x + 0.5, 2.5, -1.5), Vector3(dark_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(dark_min_x + 0.5, 2.5, 1.5), Vector3(dark_center_x, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			capture_pos, Vector3(lit_max_x, 1.2, 0.0), 15.0, 70.0, true)
	Scaffold.make_camera(root, root, "LeakCamLit",
			Vector3(lit_center_x, 1.5, 0.0), Vector3(lit_max_x, 1.5, 0.0), 4.0)
	Scaffold.make_camera(root, root, "LeakCamDark",
			Vector3(dark_center_x, 1.5, 0.0), Vector3(dark_min_x, 1.5, 0.0), 3.0)

	return {
		"root": root,
		"cameras": ColorBoxRoom.CAMERAS,
		"q1_surfaces": ColorBoxRoom._q1_surfaces(lit_min_x, lit_max_x, dark_min_x, dark_max_x, height, half_depth),
		"q3_patches": ColorBoxRoom._q3_patches(lit_min_x, lit_center_x, half_shared, half_depth),
		"shared_wall_thickness_m": shared,
		"offscreen_light_node": "OffscreenSpot" if p_variant == "offscreen" else "",
	}


func _build_single_40m_vg() -> Dictionary:
	var hall_mesh := _load_vg_mesh(SINGLE_40M_GLB)
	var root := Node3D.new()
	root.name = "ColorBoxRoomSingle40mVg"
	root.set_meta("interior_wall_thickness_m", Single40m.INTERIOR_WALL)
	root.set_meta("hall_interior_m", Single40m.INTERIOR)
	root.set_meta("virtual_geometry", true)

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	var instance := MeshInstance3D.new()
	instance.name = "HallMesh"
	instance.mesh = hall_mesh
	if hall_mesh != null:
		for s in range(hall_mesh.get_surface_count()):
			instance.set_surface_override_material(s, gray)
	instance.set_meta("interior_wall_thickness_m", Single40m.INTERIOR_WALL)
	Scaffold.add_owned(root, instance, root)

	var coarse_builder = Scaffold.ChunkedSurfaceBuilder.new(null)
	_append_single_40m_boxes(coarse_builder)
	_add_coarse_trimesh_collider(root, instance, coarse_builder.finish())

	for room in Single40m.LIT_ROOMS:
		var bounds := Single40m.room_bounds(room)
		var omni := OmniLight3D.new()
		omni.name = "RoomLight%d" % room
		omni.position = Vector3((bounds.x + bounds.y) * 0.5, 2.6, 0.0)
		omni.light_energy = 1.0
		omni.omni_range = 6.0
		omni.shadow_enabled = true
		Scaffold.add_owned(root, omni, root)

	var center_bounds := Single40m.room_bounds(3)
	var dark_bounds := Single40m.room_bounds(4)
	for room in Single40m.Q1_ROOMS:
		var bounds := Single40m.room_bounds(room)
		var camera_name: String = Single40m.Q1_CAMERAS[room]
		Scaffold.make_camera(root, root, camera_name,
				Vector3(bounds.x + 0.5, 2.5, -1.5),
				Vector3((bounds.x + bounds.y) * 0.5, 1.0, 0.0), 15.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(center_bounds.x + 0.8, 2.0, 0.0), Vector3(center_bounds.y, 1.2, 0.0), 15.0, 70.0, true)
	Scaffold.make_camera(root, root, "LeakCamLit",
			Vector3((center_bounds.x + center_bounds.y) * 0.5, 1.5, 0.0), Vector3(center_bounds.y, 1.5, 0.0), 5.0)
	Scaffold.make_camera(root, root, "LeakCamDark",
			Vector3((dark_bounds.x + dark_bounds.y) * 0.5, 1.5, 0.0), Vector3(dark_bounds.x, 1.5, 0.0), 5.0)

	var height := Single40m.INTERIOR.y
	var half_depth := Single40m.INTERIOR.z * 0.5
	return {
		"root": root,
		"cameras": Single40m.CAMERAS,
		"q1_surfaces": Single40m._q1_surfaces(height, half_depth),
		"q3_patches": Single40m._q3_patches(half_depth),
		"interior_wall_thickness_m": Single40m.INTERIOR_WALL,
		"mesh_node": "HallMesh",
	}


func _build_courtyard_vg() -> Dictionary:
	var cy_mesh := _load_vg_mesh(COURTYARD_GLB)
	var root := Node3D.new()
	root.name = "ConcaveCourtyardVg"
	root.set_meta("wall_thickness_m", Courtyard.WALL)
	root.set_meta("virtual_geometry", true)

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))

	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	var instance := MeshInstance3D.new()
	instance.name = "CourtyardMesh"
	instance.mesh = cy_mesh
	if cy_mesh != null:
		for s in range(cy_mesh.get_surface_count()):
			instance.set_surface_override_material(s, gray)
	Scaffold.add_owned(root, instance, root)

	var coarse_builder = Scaffold.ChunkedSurfaceBuilder.new(null)
	_append_courtyard_boxes(coarse_builder)
	_add_coarse_trimesh_collider(root, instance, coarse_builder.finish())

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	var elevation := deg_to_rad(35.0)
	var azimuth := deg_to_rad(45.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3.ZERO)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	Scaffold.make_camera(root, root, "CovCam0",
			Vector3(-3.5, 2.5, -3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam1",
			Vector3(3.5, 2.5, -3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam2",
			Vector3(3.5, 2.5, 3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CovCam3",
			Vector3(-3.5, 2.5, 3.5), Vector3(0.0, 1.0, 0.0), 60.0)
	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(-10.0, 10.0, -10.0), Vector3(0.0, 0.0, 0.0), 100.0, 60.0, true)

	return {
		"root": root,
		"cameras": Courtyard.CAMERAS,
		"q1_surfaces": Courtyard._q1_surfaces(),
		"mesh_node": "CourtyardMesh",
	}


func _build_all_scenes() -> void:
	var color_box_dir := "res://scenes/color_box_room"
	var jobs: Array[Dictionary] = [
		{"built": _build_cbr_vg("a"), "scene": color_box_dir + "/cbr_a_vg.tscn", "stream": 1},
		{"built": _build_cbr_vg("b"), "scene": color_box_dir + "/cbr_b_vg.tscn", "stream": 2},
		{"built": _build_cbr_vg("offscreen"), "scene": color_box_dir + "/cbr_offscreen_vg.tscn", "stream": 3},
		{"built": _build_single_40m_vg(), "scene": color_box_dir + "/cbr_single_40m_vg.tscn", "stream": 5},
		{"built": _build_courtyard_vg(), "scene": "res://scenes/concave_courtyard/courtyard_vg.tscn", "stream": 6},
	]
	if not _failures.is_empty():
		for job in jobs:
			var r: Node = job["built"]["root"]
			r.free()
		return
	for job in jobs:
		await _process_job(job)


func _process_job(p_job: Dictionary) -> void:
	var built: Dictionary = p_job["built"]
	var root: Node3D = built["root"]
	var scene_path: String = p_job["scene"]
	var base := scene_path.get_basename()
	var cameras: Array = built.get("cameras", [])

	get_root().add_child(root)
	await physics_frame
	await physics_frame

	var ok := true
	if built.has("q1_surfaces"):
		var rng := Scaffold.make_rng(int(p_job["stream"]))
		var surplus := Scaffold.generate_q1_patches(built["q1_surfaces"], Q1_PATCH_COUNT * 2, rng)
		var patches := _filter_line_of_sight(root, surplus, cameras)
		if patches.size() < Q1_PATCH_COUNT:
			_fail("%s: %d line-of-sight-verified Q1 patches, expected %d." % [scene_path, patches.size(), Q1_PATCH_COUNT])
			ok = false
		else:
			patches.resize(Q1_PATCH_COUNT)
			_sort_and_renumber(patches)
			if not _write_json(base + "_patches_q1.json", _patch_document(scene_path, patches)):
				ok = false
	if ok and built.has("q3_patches"):
		var q3: Array = built["q3_patches"]
		var lit_count := 0
		var dark_count := 0
		for patch_value in q3:
			var patch: Dictionary = patch_value
			if patch["room"] == "lit":
				lit_count += 1
			elif patch["room"] == "dark":
				dark_count += 1
		if lit_count != 16 or dark_count != 16:
			_fail("%s: Q3 patches lit=%d dark=%d, expected 16+16." % [scene_path, lit_count, dark_count])
			ok = false
		elif not _write_json(base + "_patches_q3.json", _patch_document(scene_path, q3)):
			ok = false
	if ok and String(built.get("offscreen_light_node", "")) != "":
		if not _verify_offscreen(root, String(built["offscreen_light_node"]), cameras):
			ok = false
	if ok and built.has("interior_wall_thickness_m"):
		if not _verify_single_mesh(root, String(built["mesh_node"]), float(built["interior_wall_thickness_m"])):
			ok = false

	get_root().remove_child(root)
	for camera_name in cameras:
		var camera := root.get_node(String(camera_name)) as Camera3D
		if camera != null:
			camera.current = camera.name == &"CaptureCam"
	if ok:
		var err := Scaffold.save_scene(root, scene_path)
		if err != OK:
			_fail("%s: save failed with error %d." % [scene_path, err])
		else:
			print("build_g5_6_vg: wrote " + scene_path)
	root.free()


func _patch_document(p_scene_path: String, p_patches: Array) -> Dictionary:
	return {
		"version": 1,
		"scene": p_scene_path.get_file(),
		"seed": Scaffold.DEFAULT_SEED,
		"count": p_patches.size(),
		"patches": p_patches,
	}


func _write_json(p_path: String, p_document: Dictionary) -> bool:
	var err := Scaffold.write_text_chunked(p_path, Scaffold.to_json(p_document) + "\n")
	if err != OK:
		_fail("%s: write failed with error %d." % [p_path, err])
		return false
	print("build_g5_6_vg: wrote " + p_path)
	return true


func _sort_and_renumber(p_patches: Array) -> void:
	p_patches.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if a["node"] != b["node"]:
			return String(a["node"]) < String(b["node"])
		var pa: Array = a["pos"]
		var pb: Array = b["pos"]
		for axis in range(3):
			if not is_equal_approx(float(pa[axis]), float(pb[axis])):
				return float(pa[axis]) < float(pb[axis])
		return false)
	for i in range(p_patches.size()):
		var patch: Dictionary = p_patches[i]
		patch["id"] = i


func _filter_line_of_sight(p_root: Node3D, p_patches: Array, p_cameras: Array) -> Array:
	var space := p_root.get_world_3d().direct_space_state
	var camera_nodes: Array[Camera3D] = []
	for camera_name in p_cameras:
		var camera := p_root.get_node(String(camera_name)) as Camera3D
		if camera != null:
			camera_nodes.append(camera)
	var visible: Array = []
	for patch_value in p_patches:
		var patch: Dictionary = patch_value
		var pos_array: Array = patch["pos"]
		var normal_array: Array = patch["normal"]
		var pos := Vector3(float(pos_array[0]), float(pos_array[1]), float(pos_array[2]))
		var normal := Vector3(float(normal_array[0]), float(normal_array[1]), float(normal_array[2]))
		var ordered: Array[Camera3D] = []
		for camera in camera_nodes:
			if camera.name == String(patch["camera"]):
				ordered.push_front(camera)
			else:
				ordered.append(camera)
		var visible_from := ""
		for camera in ordered:
			var from := camera.global_position
			var to := pos + normal * 0.02
			var query := PhysicsRayQueryParameters3D.create(from, to)
			query.collide_with_areas = false
			query.collide_with_bodies = true
			var hit := space.intersect_ray(query)
			if hit.is_empty() or Vector3(hit["position"]).distance_to(pos) < 0.06:
				visible_from = camera.name
				break
		if not visible_from.is_empty():
			patch["camera"] = visible_from
			visible.append(patch)
	return visible


func _verify_offscreen(p_root: Node3D, p_light_node: String, p_cameras: Array) -> bool:
	var light := p_root.get_node(p_light_node) as Node3D
	if light == null:
		_fail("off-screen light node %s not found." % p_light_node)
		return false
	for camera_name in p_cameras:
		var camera := p_root.get_node(String(camera_name)) as Camera3D
		if camera == null:
			continue
		if Scaffold.is_in_frustum(camera, light.global_position):
			_fail("off-screen light is inside the frustum of %s." % camera.name)
			return false
		if camera.is_inside_tree() and camera.is_position_in_frustum(light.global_position):
			_fail("off-screen light is inside the engine frustum of %s." % camera.name)
			return false
	print("build_g5_6_vg: off-screen light verified outside every camera frustum.")
	return true


func _verify_single_mesh(p_root: Node3D, p_mesh_node: String, p_wall_thickness: float) -> bool:
	var instance := p_root.get_node(p_mesh_node) as MeshInstance3D
	if instance == null or instance.mesh == null:
		_fail("single-mesh node %s not found." % p_mesh_node)
		return false
	var aabb := instance.mesh.get_aabb()
	var longest := maxf(aabb.size.x, maxf(aabb.size.y, aabb.size.z))
	if longest < 40.0:
		_fail("single mesh longest AABB side is %0.3f m, expected >= 40 m." % longest)
		return false
	if not is_equal_approx(p_wall_thickness, 0.10):
		_fail("single mesh interior wall thickness is %0.3f m, expected 0.10 m." % p_wall_thickness)
		return false
	if not is_equal_approx(float(instance.get_meta("interior_wall_thickness_m", -1.0)), 0.10):
		_fail("single mesh is missing its interior_wall_thickness_m metadata.")
		return false
	print("build_g5_6_vg: single mesh verified (longest side %0.2f m, walls 0.10 m)." % longest)
	return true
