extends RefCounted
## Triangle soups and material tables for the reference path tracer v0 (#19).
##
## The furnace is a closed 2 m box with inward faces. Sealed rooms are two
## 4 x 3 x 4 m interiors sharing a wall of the given thickness, built as closed
## boxes that overlap at the corners so the soup has no gap.

const FURNACE_EXTENT := 1.0
const FURNACE_ALBEDO := 0.8
const FURNACE_EMISSION := 1.0
const ROOM_SIZE := Vector3(4.0, 3.0, 4.0)
const ROOM_ALBEDO := 0.75
const LIGHT_ENERGY := 1.0
const LIGHT_RANGE := 6.0
const LIGHT_DECAY := 1.0
const LIGHT_SPECULAR := 0.5
const DIFFUSE_LAMBERT := 0.0
const ORIGIN_OFFSET := 1e-4


static func build_furnace() -> Dictionary:
	var tris: Array = []
	var s := FURNACE_EXTENT
	# +X face, inward normal -X. cross(+Z, +Y) = -X.
	_add_quad(tris, Vector3(s, -s, -s), Vector3(s, -s, s), Vector3(s, s, s), Vector3(s, s, -s), 0)
	# -X face, inward normal +X. cross(+Y, +Z) = +X.
	_add_quad(tris, Vector3(-s, -s, -s), Vector3(-s, s, -s), Vector3(-s, s, s), Vector3(-s, -s, s), 0)
	# +Y face, inward normal -Y. cross(+X, +Z) = -Y.
	_add_quad(tris, Vector3(-s, s, -s), Vector3(s, s, -s), Vector3(s, s, s), Vector3(-s, s, s), 0)
	# -Y face, inward normal +Y. cross(+Z, +X) = +Y.
	_add_quad(tris, Vector3(-s, -s, -s), Vector3(-s, -s, s), Vector3(s, -s, s), Vector3(s, -s, -s), 0)
	# +Z face, inward normal -Z. cross(+Y, +X) = -Z.
	_add_quad(tris, Vector3(-s, -s, s), Vector3(-s, s, s), Vector3(s, s, s), Vector3(s, -s, s), 0)
	# -Z face, inward normal +Z. cross(+X, +Y) = +Z.
	_add_quad(tris, Vector3(-s, -s, -s), Vector3(s, -s, -s), Vector3(s, s, -s), Vector3(-s, s, -s), 0)
	var materials := _pack_materials([{
		"albedo": Vector3(FURNACE_ALBEDO, FURNACE_ALBEDO, FURNACE_ALBEDO),
		"metallic": 0.0,
		"emission": Vector3(FURNACE_EMISSION, FURNACE_EMISSION, FURNACE_EMISSION),
		"roughness": 1.0,
		"specular": 0.0,
		"diffuse": DIFFUSE_LAMBERT,
	}])
	return _pack_soup(tris, materials, [], [], {
		"origin": Vector3.ZERO,
		"forward": Vector3(0.0, 0.0, -1.0),
		"up": Vector3(0.0, 1.0, 0.0),
		"right": Vector3(1.0, 0.0, 0.0),
		"fov_y": PI * 0.5,
		"width": 128,
		"height": 128,
	}, "furnace")


static func build_sealed(thickness: float) -> Dictionary:
	var tris: Array = []
	var y0 := 0.0
	var y1 := ROOM_SIZE.y
	var z0 := 0.0
	var z1 := ROOM_SIZE.z
	var lit_x0 := 0.0
	var lit_x1 := ROOM_SIZE.x
	# The shared wall is the lit room's east slab. The dark interior starts on its far side.
	_add_room_boxes(tris, lit_x0, lit_x1, y0, y1, z0, z1, thickness, true)
	var dark_x0 := lit_x1 + thickness
	var dark_x1 := dark_x0 + ROOM_SIZE.x
	_add_room_boxes(tris, dark_x0, dark_x1, y0, y1, z0, z1, thickness, false)
	var materials := _pack_materials([{
		"albedo": Vector3(ROOM_ALBEDO, ROOM_ALBEDO, ROOM_ALBEDO),
		"metallic": 0.0,
		"emission": Vector3.ZERO,
		"roughness": 1.0,
		"specular": 0.0,
		"diffuse": DIFFUSE_LAMBERT,
	}])
	var light_position := Vector3(lit_x0 + ROOM_SIZE.x * 0.5, ROOM_SIZE.y * 0.5, ROOM_SIZE.z * 0.5)
	var lights := _pack_lights([{
		"position": light_position,
		"range": LIGHT_RANGE,
		"color": Vector3.ONE,
		"energy": LIGHT_ENERGY,
		"decay": LIGHT_DECAY,
		"specular": LIGHT_SPECULAR,
	}])
	var patches: Array = []
	_add_room_patches(patches, "lit", lit_x0, lit_x1, y0, y1, z0, z1)
	_add_room_patches(patches, "dark", dark_x0, dark_x1, y0, y1, z0, z1)
	var soup := _pack_soup(tris, materials, lights, patches, {
		"origin": light_position,
		"forward": Vector3(0.0, 0.0, -1.0),
		"up": Vector3(0.0, 1.0, 0.0),
		"right": Vector3(1.0, 0.0, 0.0),
		"fov_y": PI * 0.5,
		"width": patches.size(),
		"height": 1,
	}, "sealed")
	soup["thickness"] = thickness
	soup["light_position"] = light_position
	soup["lit_max"] = Vector3(lit_x1, y1, z1)
	soup["dark_min"] = Vector3(dark_x0, y0, z0)
	soup["dark_max"] = Vector3(dark_x1, y1, z1)
	return soup


static func validate_furnace(soup: Dictionary) -> PackedStringArray:
	var failures := PackedStringArray()
	if int(soup["triangle_count"]) != 12:
		failures.append("furnace soup has %d triangles, expected 12" % int(soup["triangle_count"]))
	if int(soup["light_count"]) != 0:
		failures.append("furnace soup has a light")
	if int(soup["patch_count"]) != 0:
		failures.append("furnace soup has patches")
	var tris: Array = soup["triangles"]
	for i in tris.size():
		var tri: Array = tris[i]
		var normal := _normal(tri[0], tri[1], tri[2])
		var centroid: Vector3 = (tri[0] + tri[1] + tri[2]) / 3.0
		if normal.dot(centroid) >= -0.5:
			failures.append("furnace triangle %d does not face inward" % i)
			break
	return failures


static func validate_sealed(soup: Dictionary) -> PackedStringArray:
	var failures := PackedStringArray()
	# Lit room: 6 closed boxes. Dark room: 5, because the shared wall is the lit east slab.
	if int(soup["triangle_count"]) != 11 * 12:
		failures.append("sealed soup has %d triangles, expected 132" % int(soup["triangle_count"]))
	if int(soup["light_count"]) != 1:
		failures.append("sealed soup light count is %d" % int(soup["light_count"]))
	if int(soup["patch_count"]) != 32:
		failures.append("sealed soup has %d patches, expected 32" % int(soup["patch_count"]))
	var lit := 0
	var dark := 0
	var light_position: Vector3 = soup["light_position"]
	var records: Array = soup["patch_records"]
	for record in records:
		if String(record["room"]) == "lit":
			lit += 1
		elif String(record["room"]) == "dark":
			dark += 1
		var position: Vector3 = record["position"]
		var normal: Vector3 = record["normal"]
		if absf(normal.length() - 1.0) > 1e-5:
			failures.append("patch normal is not unit length")
			break
		var origin := position + normal * ORIGIN_OFFSET
		var to_light := light_position - origin
		var dist := to_light.length()
		var hit := _closest_hit(soup["triangles"], origin, to_light / dist, dist - ORIGIN_OFFSET)
		var occluded := hit >= 0.0
		if String(record["room"]) == "lit" and occluded:
			failures.append("lit patch %s sees the omni light as occluded" % String(record["wall"]))
			break
		if String(record["room"]) == "dark" and not occluded:
			failures.append("dark patch %s can see the omni light through the wall" % String(record["wall"]))
			break
	if lit != 16 or dark != 16:
		failures.append("expected 16 lit and 16 dark patches, got %d and %d" % [lit, dark])
	return failures


static func _add_room_boxes(tris: Array, x0: float, x1: float, y0: float, y1: float, z0: float, z1: float, thickness: float, include_shared_wall: bool) -> void:
	# Each slab extends through the corners it touches, so adjacent boxes overlap
	# instead of meeting on a crack.
	_add_solid_box(tris, Vector3(x0 - thickness, y0 - thickness, z0 - thickness), Vector3(x1 + thickness, y0, z1 + thickness))
	_add_solid_box(tris, Vector3(x0 - thickness, y1, z0 - thickness), Vector3(x1 + thickness, y1 + thickness, z1 + thickness))
	_add_solid_box(tris, Vector3(x0 - thickness, y0 - thickness, z0 - thickness), Vector3(x0, y1 + thickness, z1 + thickness))
	if include_shared_wall:
		_add_solid_box(tris, Vector3(x1, y0 - thickness, z0 - thickness), Vector3(x1 + thickness, y1 + thickness, z1 + thickness))
	_add_solid_box(tris, Vector3(x0 - thickness, y0 - thickness, z0 - thickness), Vector3(x1 + thickness, y1 + thickness, z0))
	_add_solid_box(tris, Vector3(x0 - thickness, y0 - thickness, z1), Vector3(x1 + thickness, y1 + thickness, z1 + thickness))


static func _add_room_patches(patches: Array, room: String, x0: float, x1: float, y0: float, y1: float, z0: float, z1: float) -> void:
	# Four patches on each vertical wall, at one third and two thirds, mirrored
	# between the two rooms by using the same fractions.
	var y_span := y1 - y0
	var z_span := z1 - z0
	var x_span := x1 - x0
	_add_wall_patches(patches, room, "-x", Vector3(x0, y0, z0), Vector3(0.0, y_span, 0.0), Vector3(0.0, 0.0, z_span), Vector3(1.0, 0.0, 0.0))
	_add_wall_patches(patches, room, "+x", Vector3(x1, y0, z0), Vector3(0.0, y_span, 0.0), Vector3(0.0, 0.0, z_span), Vector3(-1.0, 0.0, 0.0))
	_add_wall_patches(patches, room, "-z", Vector3(x0, y0, z0), Vector3(x_span, 0.0, 0.0), Vector3(0.0, y_span, 0.0), Vector3(0.0, 0.0, 1.0))
	_add_wall_patches(patches, room, "+z", Vector3(x0, y0, z1), Vector3(x_span, 0.0, 0.0), Vector3(0.0, y_span, 0.0), Vector3(0.0, 0.0, -1.0))


static func _add_wall_patches(patches: Array, room: String, wall: String, corner: Vector3, axis_u: Vector3, axis_v: Vector3, normal: Vector3) -> void:
	for u in [1.0 / 3.0, 2.0 / 3.0]:
		for v in [1.0 / 3.0, 2.0 / 3.0]:
			patches.append({
				"room": room,
				"wall": wall,
				"position": _snap(corner + axis_u * u + axis_v * v),
				"normal": normal,
				"material": 0,
			})


static func _add_solid_box(tris: Array, mn: Vector3, mx: Vector3) -> void:
	var dx := Vector3(mx.x - mn.x, 0.0, 0.0)
	var dy := Vector3(0.0, mx.y - mn.y, 0.0)
	var dz := Vector3(0.0, 0.0, mx.z - mn.z)
	# Outward normals. cross(tangent, bitangent) points out of the solid.
	_add_quad(tris, Vector3(mx.x, mn.y, mn.z), mn + Vector3(dx.x, 0.0, 0.0) + dy, Vector3(mx.x, mx.y, mx.z), Vector3(mx.x, mn.y, mx.z), 0)
	_add_quad(tris, mn, mn + dz, mn + dz + dy, mn + dy, 0)
	# +Y, outward +Y. cross(+Z, +X) = +Y.
	_add_quad(tris, Vector3(mn.x, mx.y, mn.z), Vector3(mn.x, mx.y, mx.z), Vector3(mx.x, mx.y, mx.z), Vector3(mx.x, mx.y, mn.z), 0)
	_add_quad(tris, mn, mn + dx, mn + dx + dz, mn + dz, 0)
	_add_quad(tris, Vector3(mn.x, mn.y, mx.z), Vector3(mx.x, mn.y, mx.z), Vector3(mx.x, mx.y, mx.z), Vector3(mn.x, mx.y, mx.z), 0)
	_add_quad(tris, mn, mn + dy, mn + dy + dx, mn + dx, 0)


static func _add_quad(tris: Array, v0: Vector3, v1: Vector3, v2: Vector3, v3: Vector3, material: int) -> void:
	_add_tri(tris, v0, v1, v2, material)
	_add_tri(tris, v0, v2, v3, material)


static func _add_tri(tris: Array, a: Vector3, b: Vector3, c: Vector3, material: int) -> void:
	tris.append([_snap(a), _snap(b), _snap(c), material])


static func _snap(v: Vector3) -> Vector3:
	var packed := PackedFloat32Array([v.x, v.y, v.z])
	return Vector3(packed[0], packed[1], packed[2])


static func _pack_soup(tris: Array, materials: PackedFloat32Array, lights: PackedFloat32Array, patches: Array, camera: Dictionary, name: String) -> Dictionary:
	var vertices := PackedFloat32Array()
	vertices.resize(tris.size() * 9)
	var packed_tris := PackedFloat32Array()
	packed_tris.resize(tris.size() * 12)
	for i in tris.size():
		var tri: Array = tris[i]
		for v in 3:
			var point: Vector3 = tri[v]
			vertices[i * 9 + v * 3] = point.x
			vertices[i * 9 + v * 3 + 1] = point.y
			vertices[i * 9 + v * 3 + 2] = point.z
			packed_tris[i * 12 + v * 4] = point.x
			packed_tris[i * 12 + v * 4 + 1] = point.y
			packed_tris[i * 12 + v * 4 + 2] = point.z
			packed_tris[i * 12 + v * 4 + 3] = 0.0
		packed_tris[i * 12 + 3] = float(tri[3])
	var packed_patches := PackedFloat32Array()
	packed_patches.resize(maxi(patches.size(), 1) * 8)
	for i in patches.size():
		var record: Dictionary = patches[i]
		var position: Vector3 = record["position"]
		var normal: Vector3 = record["normal"]
		packed_patches[i * 8] = position.x
		packed_patches[i * 8 + 1] = position.y
		packed_patches[i * 8 + 2] = position.z
		packed_patches[i * 8 + 3] = float(record["material"])
		packed_patches[i * 8 + 4] = normal.x
		packed_patches[i * 8 + 5] = normal.y
		packed_patches[i * 8 + 6] = normal.z
	var light_count := 0 if lights.is_empty() else int(lights.size() / 12)
	if lights.is_empty():
		lights = PackedFloat32Array()
		lights.resize(12)
	return {
		"name": name,
		"triangles": tris,
		"vertices": vertices,
		"triangle_floats": packed_tris,
		"materials": materials,
		"lights": lights,
		"patches": packed_patches,
		"patch_records": patches,
		"triangle_count": tris.size(),
		"light_count": light_count,
		"patch_count": patches.size(),
		"camera": camera,
	}


static func _pack_materials(rows: Array) -> PackedFloat32Array:
	var packed := PackedFloat32Array()
	packed.resize(rows.size() * 12)
	for i in rows.size():
		var row: Dictionary = rows[i]
		var albedo: Vector3 = row["albedo"]
		var emission: Vector3 = row["emission"]
		packed[i * 12] = albedo.x
		packed[i * 12 + 1] = albedo.y
		packed[i * 12 + 2] = albedo.z
		packed[i * 12 + 3] = float(row["metallic"])
		packed[i * 12 + 4] = emission.x
		packed[i * 12 + 5] = emission.y
		packed[i * 12 + 6] = emission.z
		packed[i * 12 + 7] = float(row["roughness"])
		packed[i * 12 + 8] = float(row["specular"])
		packed[i * 12 + 9] = float(row["diffuse"])
	return packed


static func _pack_lights(rows: Array) -> PackedFloat32Array:
	var packed := PackedFloat32Array()
	packed.resize(rows.size() * 12)
	for i in rows.size():
		var row: Dictionary = rows[i]
		var position: Vector3 = row["position"]
		var color: Vector3 = row["color"]
		packed[i * 12] = position.x
		packed[i * 12 + 1] = position.y
		packed[i * 12 + 2] = position.z
		packed[i * 12 + 3] = float(row["range"])
		packed[i * 12 + 4] = color.x
		packed[i * 12 + 5] = color.y
		packed[i * 12 + 6] = color.z
		packed[i * 12 + 7] = float(row["energy"])
		packed[i * 12 + 8] = float(row["decay"])
		packed[i * 12 + 9] = float(row["specular"])
	return packed


static func _normal(a: Vector3, b: Vector3, c: Vector3) -> Vector3:
	return (b - a).cross(c - a).normalized()


static func _closest_hit(tris: Array, origin: Vector3, direction: Vector3, tmax: float) -> float:
	var closest := -1.0
	for tri in tris:
		var t := _ray_triangle(origin, direction, tri[0], tri[1], tri[2])
		if t > 1e-5 and t < tmax and (closest < 0.0 or t < closest):
			closest = t
	return closest


static func _ray_triangle(origin: Vector3, direction: Vector3, v0: Vector3, v1: Vector3, v2: Vector3) -> float:
	var edge1 := v1 - v0
	var edge2 := v2 - v0
	var pvec := direction.cross(edge2)
	var det := edge1.dot(pvec)
	if absf(det) < 1e-12:
		return -1.0
	var inv_det := 1.0 / det
	var tvec := origin - v0
	var u := tvec.dot(pvec) * inv_det
	if u < 0.0 or u > 1.0:
		return -1.0
	var qvec := tvec.cross(edge1)
	var v := direction.dot(qvec) * inv_det
	if v < 0.0 or u + v > 1.0:
		return -1.0
	return edge2.dot(qvec) * inv_det
