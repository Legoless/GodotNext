extends SceneTree
## 512-ray parity sign reference for the SDF import set (#181, GS-5).
##
## Run from the repository root:
## [code]godot --headless --path godotnext/testbed --script res://generators/sdf_sign_reference.gd[/code]
## One mesh while iterating: append [code]-- --only=sphere[/code]
## (writes /tmp output instead of the tracked file; see below).
##
## For every voxel center of a padded grid (longest axis
## [constant GRID_LONGEST_AXIS] voxels), 512 Fibonacci-sphere rays are
## cast through a BVH over the mesh triangles. A ray votes "inside"
## when the number of triangle crossings ahead of the center is odd
## (crossing parity). The voxel is inside when a majority of the 512
## rays vote inside; majority voting is what makes the reference
## robust where a single ray grazes an edge. The mesh SDF builder's
## own sign (plan L2: backface-hit fraction of 48 rays) is checked
## against this file by GS-5, so this tool must stay independent of
## that builder: it shares only the triangle data with it.
##
## Output: [code]res://assets/sdf_import_set/sign_reference.json[/code]
## (or [code]/tmp/godotnext_sign_reference_<name>.json[/code] with
## [code]--only[/code]). [code]inside[/code] is a string of "0"/"1",
## x-fastest, one character per voxel of [code]dims[/code].

const Scaffold := preload("res://generators/scaffold.gd")
const SdfMeshes := preload("res://generators/sdf_mesh_library.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const RAYS := 512
const GRID_LONGEST_AXIS := 16
const OUTPUT_PATH := "res://assets/sdf_import_set/sign_reference.json"

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var only := SceneArgs.value("--only", "")
	var entries: Array = []
	for mesh_value in SdfMeshes.all_meshes():
		var mesh_data: Dictionary = mesh_value
		if not only.is_empty() and String(mesh_data["name"]) != only:
			continue
		var started := Time.get_ticks_msec()
		entries.append(_reference_for(mesh_data))
		print("sdf_sign_reference: %s in %d ms" % [String(mesh_data["name"]), Time.get_ticks_msec() - started])
	if entries.is_empty():
		_fail("no mesh matched --only=%s" % only)
	if _failures.is_empty():
		var document := {
			"version": 1,
			"method": "512 Fibonacci rays per voxel center; a ray votes inside on odd triangle-crossing parity; inside on majority",
			"rays_per_voxel": RAYS,
			"grid_longest_axis": GRID_LONGEST_AXIS,
			"meshes": entries,
		}
		var path := OUTPUT_PATH
		if not only.is_empty():
			path = "/tmp/godotnext_sign_reference_%s.json" % only
		var err := Scaffold.write_text_chunked(path, Scaffold.to_json(document) + "\n")
		if err != OK:
			_fail("%s: write failed with error %d" % [path, err])
		else:
			print("sdf_sign_reference: wrote " + path)
	if _failures.is_empty():
		quit(0)
	else:
		for failure in _failures:
			push_error("sdf_sign_reference: " + failure)
		quit(1)


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("sdf_sign_reference FAIL: " + p_message)


func _reference_for(p_mesh: Dictionary) -> Dictionary:
	var vertices: PackedVector3Array = p_mesh["vertices"]
	var faces: PackedInt32Array = p_mesh["faces"]
	var stats := SdfMeshes.analyze(p_mesh)
	var min_v: Vector3 = stats["aabb_min"]
	var max_v: Vector3 = stats["aabb_max"]
	var size := max_v - min_v
	var longest := maxf(size.x, maxf(size.y, size.z))
	var voxel := longest / float(GRID_LONGEST_AXIS)
	var origin := min_v - Vector3.ONE * voxel
	var dims := Vector3i(
			int(ceil(size.x / voxel)) + 2,
			int(ceil(size.y / voxel)) + 2,
			int(ceil(size.z / voxel)) + 2)
	var centers := PackedVector3Array()
	centers.resize(dims.x * dims.y * dims.z)
	var cursor := 0
	for iz in dims.z:
		for iy in dims.y:
			for ix in dims.x:
				centers[cursor] = origin + (Vector3(ix, iy, iz) + Vector3.ONE * 0.5) * voxel
				cursor += 1
	var bvh := Bvh.new(vertices, faces)
	var votes := PackedInt32Array()
	votes.resize(centers.size())
	var directions := _fibonacci_directions(RAYS)
	for dir in directions:
		var inv := Vector3(
				1.0 / dir.x if absf(dir.x) > 1e-9 else 1e18,
				1.0 / dir.y if absf(dir.y) > 1e-9 else 1e18,
				1.0 / dir.z if absf(dir.z) > 1e-9 else 1e18)
		for index in centers.size():
			if bvh.crossing_parity(centers[index], dir, inv):
				votes[index] += 1
	var chars := PackedByteArray()
	chars.resize(centers.size())
	var inside_count := 0
	for index in centers.size():
		var inside := votes[index] * 2 > RAYS
		chars[index] = 49 if inside else 48
		if inside:
			inside_count += 1
	return {
		"name": String(p_mesh["name"]),
		"closed": bool(p_mesh["closed"]),
		"grid_origin": [origin.x, origin.y, origin.z],
		"voxel_size_m": voxel,
		"dims": [dims.x, dims.y, dims.z],
		"inside_count": inside_count,
		"inside": chars.get_string_from_ascii(),
	}


static func _fibonacci_directions(p_count: int) -> PackedVector3Array:
	var directions := PackedVector3Array()
	directions.resize(p_count)
	var golden := PI * (3.0 - sqrt(5.0))
	for i in p_count:
		var y := 1.0 - 2.0 * (float(i) + 0.5) / float(p_count)
		var radius := sqrt(maxf(0.0, 1.0 - y * y))
		var angle := golden * float(i)
		directions[i] = Vector3(cos(angle) * radius, y, sin(angle) * radius)
	return directions


## Median-split BVH over triangle indices, stored in parallel arrays.
## [method crossing_parity] counts every triangle crossing ahead of the
## ray origin; only the parity of the count is returned.
class Bvh:
	var _tri_a := PackedVector3Array()
	var _tri_e1 := PackedVector3Array()
	var _tri_e2 := PackedVector3Array()
	var _node_min := PackedVector3Array()
	var _node_max := PackedVector3Array()
	var _node_left := PackedInt32Array()
	var _node_right := PackedInt32Array()
	var _node_start := PackedInt32Array()
	var _node_count := PackedInt32Array()
	var _order: Array[int] = []

	func _init(p_vertices: PackedVector3Array, p_faces: PackedInt32Array) -> void:
		var triangles := p_faces.size() / 3
		_tri_a.resize(triangles)
		_tri_e1.resize(triangles)
		_tri_e2.resize(triangles)
		for t in triangles:
			var a := p_vertices[p_faces[t * 3]]
			_tri_a[t] = a
			_tri_e1[t] = p_vertices[p_faces[t * 3 + 1]] - a
			_tri_e2[t] = p_vertices[p_faces[t * 3 + 2]] - a
			_order.append(t)
		_build(0, triangles)

	func _build(p_start: int, p_count: int) -> int:
		var node := _node_min.size()
		_node_min.append(Vector3.ZERO)
		_node_max.append(Vector3.ZERO)
		_node_left.append(-1)
		_node_right.append(-1)
		_node_start.append(p_start)
		_node_count.append(p_count)
		var bounds := AABB(_tri_point(_order[p_start], 0), Vector3.ZERO)
		var centroid_bounds := AABB(_centroid(_order[p_start]), Vector3.ZERO)
		for k in range(p_start, p_start + p_count):
			var t := _order[k]
			for e in 3:
				bounds = bounds.expand(_tri_point(t, e))
			centroid_bounds = centroid_bounds.expand(_centroid(t))
		_node_min[node] = bounds.position
		_node_max[node] = bounds.position + bounds.size
		if p_count <= 4:
			return node
		var axis := 0
		var extent := centroid_bounds.size
		if extent.y > extent.x and extent.y >= extent.z:
			axis = 1
		elif extent.z > extent.x and extent.z > extent.y:
			axis = 2
		var slice: Array = _order.slice(p_start, p_start + p_count)
		slice.sort_custom(func(a: int, b: int) -> bool:
			return _centroid(a)[axis] < _centroid(b)[axis])
		for k in p_count:
			_order[p_start + k] = int(slice[k])
		var half := p_count / 2
		var left := _build(p_start, half)
		var right := _build(p_start + half, p_count - half)
		_node_left[node] = left
		_node_right[node] = right
		_node_count[node] = 0
		return node

	func _tri_point(p_tri: int, p_corner: int) -> Vector3:
		if p_corner == 0:
			return _tri_a[p_tri]
		if p_corner == 1:
			return _tri_a[p_tri] + _tri_e1[p_tri]
		return _tri_a[p_tri] + _tri_e2[p_tri]

	func _centroid(p_tri: int) -> Vector3:
		return _tri_a[p_tri] + (_tri_e1[p_tri] + _tri_e2[p_tri]) / 3.0

	func crossing_parity(p_origin: Vector3, p_dir: Vector3, p_inv: Vector3) -> bool:
		var odd := false
		var stack: Array[int] = [0]
		while not stack.is_empty():
			var node: int = stack[stack.size() - 1]
			stack.resize(stack.size() - 1)
			if not _ray_hits_node(p_origin, p_inv, node):
				continue
			var left := _node_left[node]
			if left >= 0:
				stack.append(left)
				stack.append(_node_right[node])
				continue
			var start := _node_start[node]
			var count := _node_count[node]
			for k in range(start, start + count):
				if _ray_hits_triangle(p_origin, p_dir, _order[k]):
					odd = not odd
		return odd

	func _ray_hits_node(p_origin: Vector3, p_inv: Vector3, p_node: int) -> bool:
		var min_v := _node_min[p_node]
		var max_v := _node_max[p_node]
		var t1 := (min_v.x - p_origin.x) * p_inv.x
		var t2 := (max_v.x - p_origin.x) * p_inv.x
		var tmin := minf(t1, t2)
		var tmax := maxf(t1, t2)
		t1 = (min_v.y - p_origin.y) * p_inv.y
		t2 = (max_v.y - p_origin.y) * p_inv.y
		tmin = maxf(tmin, minf(t1, t2))
		tmax = minf(tmax, maxf(t1, t2))
		t1 = (min_v.z - p_origin.z) * p_inv.z
		t2 = (max_v.z - p_origin.z) * p_inv.z
		tmin = maxf(tmin, minf(t1, t2))
		tmax = minf(tmax, maxf(t1, t2))
		return tmax >= maxf(tmin, 0.0)

	# Möller-Trumbore, both faces: parity counts a crossing either way.
	func _ray_hits_triangle(p_origin: Vector3, p_dir: Vector3, p_tri: int) -> bool:
		var e1 := _tri_e1[p_tri]
		var e2 := _tri_e2[p_tri]
		var pvec := p_dir.cross(e2)
		var det := e1.dot(pvec)
		if absf(det) < 1e-12:
			return false
		var inv_det := 1.0 / det
		var tvec := p_origin - _tri_a[p_tri]
		var u := tvec.dot(pvec) * inv_det
		if u < -1e-9 or u > 1.0 + 1e-9:
			return false
		var qvec := tvec.cross(e1)
		var v := p_dir.dot(qvec) * inv_det
		if v < -1e-9 or u + v > 1.0 + 1e-9:
			return false
		var t := e2.dot(qvec) * inv_det
		return t > 1e-7
