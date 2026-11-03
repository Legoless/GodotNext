extends RefCounted
## Deterministic triangle meshes for the SDF import set (#181, GS-5).
##
## Every mesh is plain data: [code]{"name", "closed", "vertices"
## (PackedVector3Array), "faces" (PackedInt32Array triples)}[/code].
## Faces are counter-clockwise seen from outside (the OBJ convention), so
## the geometric normal [code](b - a).cross(c - a)[/code] points outward.
## Closed meshes are flipped as a whole until their signed volume is
## positive, which pins the same convention regardless of how a single
## parametric grid was walked. [method build_array_mesh] reverses each
## triangle again, because Godot's front faces are clockwise.
##
## The asset tool writes these meshes as OBJ files, the sign reference
## tool evaluates signs on exactly these triangles, and the display scene
## builds ArrayMeshes from them, so all three describe the same geometry.

const CLOSED_NAMES: Array[String] = [
	"box", "sphere", "cylinder", "cone", "torus", "capsule",
	"tetrahedron", "octahedron", "icosahedron", "hexagonal_prism",
	"l_prism", "wedge",
]
const OPEN_NAMES: Array[String] = [
	"plane_sheet", "open_box", "open_tube", "hemisphere_bowl",
]


static func all_meshes() -> Array:
	var meshes: Array = []
	meshes.append(_box(Vector3(1.6, 1.2, 1.4), "box", true))
	meshes.append(_icosphere(0.8, 2, "sphere"))
	meshes.append(_cylinder(0.7, 1.6, 24, true, "cylinder"))
	meshes.append(_cone(0.75, 1.6, 24))
	meshes.append(_torus(0.65, 0.3, 32, 16))
	meshes.append(_capsule(0.45, 0.9, 20, 6))
	meshes.append(_tetrahedron())
	meshes.append(_octahedron())
	meshes.append(_icosahedron_mesh())
	meshes.append(_extrude(_regular_polygon(6, 0.75), 1.4, "hexagonal_prism"))
	meshes.append(_extrude(_l_polygon(), 1.3, "l_prism"))
	meshes.append(_extrude([Vector2(-0.8, -0.6), Vector2(0.8, -0.6), Vector2(-0.8, 0.6)], 1.5, "wedge"))
	meshes.append(_plane_sheet())
	meshes.append(_box(Vector3(1.5, 1.1, 1.3), "open_box", false))
	meshes.append(_cylinder(0.7, 1.6, 24, false, "open_tube"))
	meshes.append(_hemisphere_bowl(0.85, 24, 10))
	return meshes


static func mesh_named(p_name: String) -> Dictionary:
	for mesh_value in all_meshes():
		var mesh: Dictionary = mesh_value
		if String(mesh["name"]) == p_name:
			return mesh
	return {}


## Triangle/edge census. A closed, consistently oriented manifold has no
## boundary edges, no non-manifold edges and a positive signed volume.
static func analyze(p_mesh: Dictionary) -> Dictionary:
	var vertices: PackedVector3Array = p_mesh["vertices"]
	var faces: PackedInt32Array = p_mesh["faces"]
	var edge_counts := {}
	var aabb := AABB(vertices[0], Vector3.ZERO)
	for vertex in vertices:
		aabb = aabb.expand(vertex)
	var volume := 0.0
	for t in range(0, faces.size(), 3):
		var a := vertices[faces[t]]
		var b := vertices[faces[t + 1]]
		var c := vertices[faces[t + 2]]
		volume += a.dot(b.cross(c)) / 6.0
		for e in 3:
			var u := faces[t + e]
			var v := faces[t + (e + 1) % 3]
			var key := Vector2i(mini(u, v), maxi(u, v))
			edge_counts[key] = int(edge_counts.get(key, 0)) + 1
	var boundary := 0
	var nonmanifold := 0
	for key in edge_counts:
		var count := int(edge_counts[key])
		if count == 1:
			boundary += 1
		elif count != 2:
			nonmanifold += 1
	return {
		"vertices": vertices.size(),
		"triangles": faces.size() / 3,
		"edges": edge_counts.size(),
		"boundary_edges": boundary,
		"nonmanifold_edges": nonmanifold,
		"signed_volume": volume,
		"aabb_min": aabb.position,
		"aabb_max": aabb.position + aabb.size,
	}


static func contract_error() -> String:
	var meshes := all_meshes()
	if meshes.size() != CLOSED_NAMES.size() + OPEN_NAMES.size():
		return "sdf mesh set has %d meshes, expected %d" % [meshes.size(), CLOSED_NAMES.size() + OPEN_NAMES.size()]
	var seen := {}
	for mesh_value in meshes:
		var mesh: Dictionary = mesh_value
		var name := String(mesh["name"])
		if seen.has(name):
			return "duplicate sdf mesh " + name
		seen[name] = true
		var stats := analyze(mesh)
		if int(stats["nonmanifold_edges"]) != 0:
			return "%s has %d non-manifold edges" % [name, int(stats["nonmanifold_edges"])]
		if bool(mesh["closed"]):
			if not CLOSED_NAMES.has(name):
				return name + " is closed but not in the closed list"
			if int(stats["boundary_edges"]) != 0:
				return "%s is listed closed but has %d boundary edges" % [name, int(stats["boundary_edges"])]
			if float(stats["signed_volume"]) <= 0.0:
				return name + " has a non-positive signed volume"
		else:
			if not OPEN_NAMES.has(name):
				return name + " is open but not in the open list"
			if int(stats["boundary_edges"]) == 0:
				return name + " is listed open but has no boundary edges"
	for name in CLOSED_NAMES + OPEN_NAMES:
		if not seen.has(name):
			return "sdf mesh set is missing " + name
	return ""


## ArrayMesh with smooth (area-weighted) vertex normals. Triangles are
## reversed to Godot's clockwise front-face winding.
static func build_array_mesh(p_mesh: Dictionary, p_material: Material) -> ArrayMesh:
	var vertices: PackedVector3Array = p_mesh["vertices"]
	var faces: PackedInt32Array = p_mesh["faces"]
	var normals := PackedVector3Array()
	normals.resize(vertices.size())
	for t in range(0, faces.size(), 3):
		var a := vertices[faces[t]]
		var b := vertices[faces[t + 1]]
		var c := vertices[faces[t + 2]]
		var face_normal := (b - a).cross(c - a)
		for e in 3:
			var index := faces[t + e]
			normals[index] = normals[index] + face_normal
	for i in normals.size():
		var n := normals[i]
		normals[i] = n.normalized() if n.length_squared() > 1e-12 else Vector3.UP
	var indices := PackedInt32Array()
	indices.resize(faces.size())
	for t in range(0, faces.size(), 3):
		indices[t] = faces[t]
		indices[t + 1] = faces[t + 2]
		indices[t + 2] = faces[t + 1]
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	if p_material != null:
		mesh.surface_set_material(0, p_material)
	return mesh


## OBJ text: fixed 6-decimal floats and 1-based face indices, so two runs
## of the asset tool are byte-identical.
static func to_obj(p_mesh: Dictionary) -> String:
	var vertices: PackedVector3Array = p_mesh["vertices"]
	var faces: PackedInt32Array = p_mesh["faces"]
	var lines: Array[String] = []
	lines.append("# GodotNext SDF import set (#181). Generated; do not edit by hand.")
	lines.append("# Regenerate with res://generators/build_sdf_assets.gd.")
	lines.append("o " + String(p_mesh["name"]))
	for vertex in vertices:
		lines.append("v %0.6f %0.6f %0.6f" % [vertex.x, vertex.y, vertex.z])
	for t in range(0, faces.size(), 3):
		lines.append("f %d %d %d" % [faces[t] + 1, faces[t + 1] + 1, faces[t + 2] + 1])
	return "\n".join(lines) + "\n"


static func _finish(p_name: String, p_closed: bool, p_vertices: PackedVector3Array, p_faces: PackedInt32Array) -> Dictionary:
	if p_closed:
		var volume := 0.0
		for t in range(0, p_faces.size(), 3):
			var a := p_vertices[p_faces[t]]
			var b := p_vertices[p_faces[t + 1]]
			var c := p_vertices[p_faces[t + 2]]
			volume += a.dot(b.cross(c))
		if volume < 0.0:
			for t in range(0, p_faces.size(), 3):
				var tmp := p_faces[t + 1]
				p_faces[t + 1] = p_faces[t + 2]
				p_faces[t + 2] = tmp
	return {"name": p_name, "closed": p_closed, "vertices": p_vertices, "faces": p_faces}


static func _quad(p_faces: PackedInt32Array, p_a: int, p_b: int, p_c: int, p_d: int) -> void:
	# Vertices counter-clockwise seen from outside: (a,b,c) and (a,c,d).
	p_faces.append_array([p_a, p_b, p_c, p_a, p_c, p_d])


static func _box(p_size: Vector3, p_name: String, p_top: bool) -> Dictionary:
	var hx := p_size.x * 0.5
	var hy := p_size.y * 0.5
	var hz := p_size.z * 0.5
	var vertices := PackedVector3Array([
		Vector3(-hx, -hy, -hz), Vector3(hx, -hy, -hz), Vector3(hx, -hy, hz), Vector3(-hx, -hy, hz),
		Vector3(-hx, hy, -hz), Vector3(hx, hy, -hz), Vector3(hx, hy, hz), Vector3(-hx, hy, hz),
	])
	var faces := PackedInt32Array()
	_quad(faces, 0, 3, 2, 1) # Bottom, normal -Y.
	_quad(faces, 1, 2, 6, 5) # +X.
	_quad(faces, 3, 0, 4, 7) # -X.
	_quad(faces, 2, 3, 7, 6) # +Z.
	_quad(faces, 0, 1, 5, 4) # -Z.
	if p_top:
		_quad(faces, 4, 5, 6, 7) # Top, normal +Y.
	return _finish(p_name, p_top, vertices, faces)


static func _icosahedron_data(p_radius: float) -> Dictionary:
	var t := (1.0 + sqrt(5.0)) * 0.5
	var raw := [
		Vector3(-1, t, 0), Vector3(1, t, 0), Vector3(-1, -t, 0), Vector3(1, -t, 0),
		Vector3(0, -1, t), Vector3(0, 1, t), Vector3(0, -1, -t), Vector3(0, 1, -t),
		Vector3(t, 0, -1), Vector3(t, 0, 1), Vector3(-t, 0, -1), Vector3(-t, 0, 1),
	]
	var vertices := PackedVector3Array()
	for value in raw:
		vertices.append((value as Vector3).normalized() * p_radius)
	var faces := PackedInt32Array([
		0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11,
		1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
		3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9,
		4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1,
	])
	return {"vertices": vertices, "faces": faces}


static func _icosahedron_mesh() -> Dictionary:
	var data := _icosahedron_data(0.9)
	return _finish("icosahedron", true, data["vertices"], data["faces"])


static func _icosphere(p_radius: float, p_subdivisions: int, p_name: String) -> Dictionary:
	var data := _icosahedron_data(p_radius)
	var vertices: PackedVector3Array = data["vertices"]
	var faces: PackedInt32Array = data["faces"]
	for step in p_subdivisions:
		var midpoints := {}
		var subdivided := PackedInt32Array()
		for t in range(0, faces.size(), 3):
			var a := faces[t]
			var b := faces[t + 1]
			var c := faces[t + 2]
			var ab := _midpoint(vertices, midpoints, a, b, p_radius)
			var bc := _midpoint(vertices, midpoints, b, c, p_radius)
			var ca := _midpoint(vertices, midpoints, c, a, p_radius)
			subdivided.append_array([a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca])
		faces = subdivided
	return _finish(p_name, true, vertices, faces)


static func _midpoint(p_vertices: PackedVector3Array, p_cache: Dictionary, p_a: int, p_b: int, p_radius: float) -> int:
	var key := Vector2i(mini(p_a, p_b), maxi(p_a, p_b))
	if p_cache.has(key):
		return int(p_cache[key])
	var mid := ((p_vertices[p_a] + p_vertices[p_b]) * 0.5).normalized() * p_radius
	p_vertices.append(mid)
	var index := p_vertices.size() - 1
	p_cache[key] = index
	return index


static func _ring_point(p_radius: float, p_y: float, p_index: int, p_count: int) -> Vector3:
	var angle := TAU * float(p_index) / float(p_count)
	return Vector3(cos(angle) * p_radius, p_y, sin(angle) * p_radius)


static func _cylinder(p_radius: float, p_height: float, p_radial: int, p_caps: bool, p_name: String) -> Dictionary:
	var half := p_height * 0.5
	var vertices := PackedVector3Array()
	for i in p_radial:
		vertices.append(_ring_point(p_radius, -half, i, p_radial))
	for i in p_radial:
		vertices.append(_ring_point(p_radius, half, i, p_radial))
	var faces := PackedInt32Array()
	for i in p_radial:
		var j := (i + 1) % p_radial
		_quad(faces, i, j, p_radial + j, p_radial + i)
	if p_caps:
		var bottom := vertices.size()
		vertices.append(Vector3(0.0, -half, 0.0))
		var top := vertices.size()
		vertices.append(Vector3(0.0, half, 0.0))
		for i in p_radial:
			var j := (i + 1) % p_radial
			faces.append_array([bottom, j, i])
			faces.append_array([top, p_radial + i, p_radial + j])
	return _finish(p_name, p_caps, vertices, faces)


static func _cone(p_radius: float, p_height: float, p_radial: int) -> Dictionary:
	var half := p_height * 0.5
	var vertices := PackedVector3Array()
	for i in p_radial:
		vertices.append(_ring_point(p_radius, -half, i, p_radial))
	var apex := vertices.size()
	vertices.append(Vector3(0.0, half, 0.0))
	var base := vertices.size()
	vertices.append(Vector3(0.0, -half, 0.0))
	var faces := PackedInt32Array()
	for i in p_radial:
		var j := (i + 1) % p_radial
		faces.append_array([i, j, apex])
		faces.append_array([base, j, i])
	return _finish("cone", true, vertices, faces)


static func _torus(p_major: float, p_minor: float, p_tubular: int, p_radial: int) -> Dictionary:
	var vertices := PackedVector3Array()
	for i in p_tubular:
		var u := TAU * float(i) / float(p_tubular)
		for j in p_radial:
			var v := TAU * float(j) / float(p_radial)
			var ring := p_major + p_minor * cos(v)
			vertices.append(Vector3(ring * cos(u), p_minor * sin(v), ring * sin(u)))
	var faces := PackedInt32Array()
	for i in p_tubular:
		var ni := (i + 1) % p_tubular
		for j in p_radial:
			var nj := (j + 1) % p_radial
			_quad(faces,
					i * p_radial + j, ni * p_radial + j,
					ni * p_radial + nj, i * p_radial + nj)
	return _finish("torus", true, vertices, faces)


## Lathe profile: hemisphere, straight run, hemisphere.
static func _capsule(p_radius: float, p_straight: float, p_radial: int, p_arc_steps: int) -> Dictionary:
	var profile: Array[Vector2] = []
	for k in range(1, p_arc_steps + 1):
		var angle := -PI * 0.5 + (PI * 0.5) * float(k) / float(p_arc_steps)
		profile.append(Vector2(p_radius * cos(angle), -p_straight * 0.5 + p_radius * sin(angle)))
	for k in range(1, p_arc_steps):
		var angle := (PI * 0.5) * float(k) / float(p_arc_steps)
		profile.append(Vector2(p_radius * cos(angle), p_straight * 0.5 + p_radius * sin(angle)))
	var vertices := PackedVector3Array()
	var bottom_pole := 0
	vertices.append(Vector3(0.0, -p_straight * 0.5 - p_radius, 0.0))
	var ring_start: Array[int] = []
	for point in profile:
		ring_start.append(vertices.size())
		for i in p_radial:
			vertices.append(_ring_point(point.x, point.y, i, p_radial))
	var top_pole := vertices.size()
	vertices.append(Vector3(0.0, p_straight * 0.5 + p_radius, 0.0))
	var faces := PackedInt32Array()
	for i in p_radial:
		var j := (i + 1) % p_radial
		faces.append_array([bottom_pole, ring_start[0] + j, ring_start[0] + i])
	for r in range(profile.size() - 1):
		for i in p_radial:
			var j := (i + 1) % p_radial
			_quad(faces, ring_start[r] + i, ring_start[r] + j, ring_start[r + 1] + j, ring_start[r + 1] + i)
	var last := ring_start[ring_start.size() - 1]
	for i in p_radial:
		var j := (i + 1) % p_radial
		faces.append_array([top_pole, last + i, last + j])
	return _finish("capsule", true, vertices, faces)


static func _tetrahedron() -> Dictionary:
	var vertices := PackedVector3Array([
		Vector3(1, 1, 1), Vector3(1, -1, -1), Vector3(-1, 1, -1), Vector3(-1, -1, 1),
	])
	for i in vertices.size():
		vertices[i] = vertices[i].normalized() * 0.95
	var faces := PackedInt32Array([0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2])
	return _finish("tetrahedron", true, vertices, faces)


static func _octahedron() -> Dictionary:
	var vertices := PackedVector3Array([
		Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0),
		Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1),
	])
	for i in vertices.size():
		vertices[i] = vertices[i] * 0.9
	var faces := PackedInt32Array([
		2, 4, 0, 2, 0, 5, 2, 5, 1, 2, 1, 4,
		3, 0, 4, 3, 5, 0, 3, 1, 5, 3, 4, 1,
	])
	return _finish("octahedron", true, vertices, faces)


static func _regular_polygon(p_sides: int, p_radius: float) -> Array:
	var polygon: Array = []
	for i in p_sides:
		var angle := TAU * float(i) / float(p_sides)
		polygon.append(Vector2(cos(angle) * p_radius, sin(angle) * p_radius))
	return polygon


static func _l_polygon() -> Array:
	# Starts at the reflex corner so the cap fan from vertex 0 stays inside.
	return [
		Vector2(0.0, 0.0), Vector2(0.8, 0.0), Vector2(0.8, -0.8),
		Vector2(-0.8, -0.8), Vector2(-0.8, 0.8), Vector2(0.0, 0.8),
	]


## Extrudes a counter-clockwise XY polygon along Z, centered on the origin.
## Caps are fans from vertex 0; the polygons used here are convex or fan
## from their reflex corner, so every fan triangle lies inside the cap.
static func _extrude(p_polygon: Array, p_depth: float, p_name: String) -> Dictionary:
	var half := p_depth * 0.5
	var count := p_polygon.size()
	var vertices := PackedVector3Array()
	for value in p_polygon:
		var point: Vector2 = value
		vertices.append(Vector3(point.x, point.y, -half))
	for value in p_polygon:
		var point: Vector2 = value
		vertices.append(Vector3(point.x, point.y, half))
	var faces := PackedInt32Array()
	for i in range(1, count - 1):
		faces.append_array([0, i + 1, i]) # Back cap (-Z).
		faces.append_array([count, count + i, count + i + 1]) # Front cap (+Z).
	for i in count:
		var j := (i + 1) % count
		_quad(faces, i, j, count + j, count + i)
	return _finish(p_name, true, vertices, faces)


static func _plane_sheet() -> Dictionary:
	var cells := 8
	var extent := 0.8
	var vertices := PackedVector3Array()
	for iz in range(cells + 1):
		for ix in range(cells + 1):
			vertices.append(Vector3(
					-extent + 2.0 * extent * float(ix) / float(cells), 0.0,
					-extent + 2.0 * extent * float(iz) / float(cells)))
	var faces := PackedInt32Array()
	var row := cells + 1
	for iz in cells:
		for ix in cells:
			var a := iz * row + ix
			# Counter-clockwise from +Y: the sheet normal is up.
			_quad(faces, a, a + row, a + row + 1, a + 1)
	return _finish("plane_sheet", false, vertices, faces)


static func _hemisphere_bowl(p_radius: float, p_radial: int, p_rings: int) -> Dictionary:
	var vertices := PackedVector3Array()
	var pole := 0
	vertices.append(Vector3(0.0, -p_radius, 0.0))
	var ring_start: Array[int] = []
	for r in range(1, p_rings + 1):
		var angle := -PI * 0.5 + (PI * 0.5) * float(r) / float(p_rings)
		ring_start.append(vertices.size())
		for i in p_radial:
			vertices.append(_ring_point(p_radius * cos(angle), p_radius * sin(angle), i, p_radial))
	var faces := PackedInt32Array()
	for i in p_radial:
		var j := (i + 1) % p_radial
		faces.append_array([pole, ring_start[0] + j, ring_start[0] + i])
	for r in range(p_rings - 1):
		for i in p_radial:
			var j := (i + 1) % p_radial
			_quad(faces, ring_start[r] + i, ring_start[r] + j, ring_start[r + 1] + j, ring_start[r + 1] + i)
	return _finish("hemisphere_bowl", false, vertices, faces)
