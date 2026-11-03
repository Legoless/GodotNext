extends RefCounted
const Scaffold := preload("res://generators/scaffold.gd")
## Seeded NextGI Perf layout (#33). Instances are planned here and spawned
## at runtime. The shell scene does not store them.
##
## Drop rule from plan §2.1 / §13.3: an instance is outside the TLAS feed
## when it is beyond 30 m and its angular radius is under 0.5°. Angular
## radius is atan(bounding_radius / distance). The 0.4 m box uses the
## bounding-sphere radius 0.2 * sqrt(3).


const SEED := 1234
const DEFAULT_INSTANCES := 50000
const CELL := 0.5
const BOX := 0.4
const SPLINE_FRAMES := 600
const SEGMENT_FRAMES := 120
const MIN_DISTANCE_M := 30.0
const MAX_ANGLE_DEG := 0.5
const ROUGHNESS: Array[float] = [0.1, 0.25, 0.4, 0.55, 0.7, 0.85, 1.0]

const WAYPOINTS: Array[Vector3] = [
	Vector3(4.0, 3.2, 0.0),
	Vector3(18.0, 3.2, 0.0),
	Vector3(36.0, 3.4, -8.0),
	Vector3(55.0, 3.6, 10.0),
	Vector3(75.0, 3.4, -10.0),
	Vector3(95.0, 3.2, 0.0),
]


static func box_radius() -> float:
	return (BOX * 0.5) * sqrt(3.0)


static func shares(p_count: int) -> Dictionary:
	var count := maxi(p_count, 0)
	var multimesh := int(count * 3 / 20)
	var gridmap := int(count / 10)
	return {
		"mesh": count - multimesh - gridmap,
		"multimesh": multimesh,
		"gridmap": gridmap,
	}


static func is_dropped(p_distance: float, p_radius: float) -> bool:
	if p_distance <= MIN_DISTANCE_M:
		return false
	if p_radius <= 0.0:
		return true
	return atan(p_radius / p_distance) < deg_to_rad(MAX_ANGLE_DEG)


static func spline_position(p_frame: int) -> Vector3:
	var frame := clampi(p_frame, 0, SPLINE_FRAMES - 1)
	var segment := mini(int(frame / SEGMENT_FRAMES), WAYPOINTS.size() - 2)
	var local := float(frame - segment * SEGMENT_FRAMES) / float(SEGMENT_FRAMES)
	return WAYPOINTS[segment].lerp(WAYPOINTS[segment + 1], local)


static func spline_basis(p_frame: int) -> Basis:
	var origin := spline_position(p_frame)
	var ahead := spline_position(mini(p_frame + 1, SPLINE_FRAMES - 1))
	var forward := ahead - origin
	if forward.length_squared() < 1e-8:
		forward = WAYPOINTS[WAYPOINTS.size() - 1] - WAYPOINTS[WAYPOINTS.size() - 2]
	var up := Vector3.UP
	if absf(forward.normalized().dot(up)) > 0.99:
		up = Vector3.FORWARD
	return Basis.looking_at(forward, up)


static func plan(p_count: int, p_seed: int) -> Dictionary:
	var slots := _lattice()
	var count := mini(maxi(p_count, 0), slots.size())
	var rng := RandomNumberGenerator.new()
	rng.seed = p_seed
	for index in count:
		var swap := rng.randi_range(index, slots.size() - 1)
		var tmp: Vector3 = slots[index]
		slots[index] = slots[swap]
		slots[swap] = tmp
	var share := shares(count)
	var mesh_count := int(share["mesh"])
	var multi_count := int(share["multimesh"])
	var positions := PackedVector3Array()
	var roughness := PackedFloat32Array()
	var kind := PackedInt32Array()
	positions.resize(count)
	roughness.resize(count)
	kind.resize(count)
	for index in count:
		positions[index] = slots[index]
		roughness[index] = ROUGHNESS[index % ROUGHNESS.size()]
		if index < mesh_count:
			kind[index] = 0
		elif index < mesh_count + multi_count:
			kind[index] = 1
		else:
			kind[index] = 2
	return {
		"positions": positions,
		"roughness": roughness,
		"kind": kind,
		"mesh": mesh_count,
		"multimesh": multi_count,
		"gridmap": int(share["gridmap"]),
		"seed": p_seed,
		"count": count,
	}


static func positions_sha256(p_positions: PackedVector3Array) -> String:
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(p_positions.to_byte_array())
	return hashing.finish().hex_encode()


static func cell_for(p_position: Vector3) -> Vector3i:
	return Vector3i(
			int(round(p_position.x / CELL)),
			int(round(p_position.y / CELL)),
			int(round(p_position.z / CELL)))


## How many planned instances the drop rule removes at each spline frame.
static func spline_cull_counts(p_positions: PackedVector3Array, p_radius: float) -> PackedInt32Array:
	var limit := MIN_DISTANCE_M
	if p_radius > 0.0:
		limit = maxf(MIN_DISTANCE_M, p_radius / tan(deg_to_rad(MAX_ANGLE_DEG)))
	var buckets := _xz_buckets(p_positions, 8.0)
	var counts := PackedInt32Array()
	counts.resize(SPLINE_FRAMES)
	for frame in SPLINE_FRAMES:
		counts[frame] = _count_beyond(p_positions, buckets, spline_position(frame), limit, 8.0)
	return counts


static func spawn(p_parent: Node3D, p_plan: Dictionary) -> Dictionary:
	var positions: PackedVector3Array = p_plan["positions"]
	var roughness: PackedFloat32Array = p_plan["roughness"]
	var kind: PackedInt32Array = p_plan["kind"]
	var count := positions.size()
	var world_meshes: Array[BoxMesh] = []
	var grid_meshes: Array[BoxMesh] = []
	for index in ROUGHNESS.size():
		var material := Scaffold.make_constant_material(
				"PropR%d" % index, Color(0.62, 0.62, 0.62), ROUGHNESS[index])
		var world := BoxMesh.new()
		world.size = Vector3(BOX, BOX, BOX)
		world.material = material
		world_meshes.append(world)
		var grid_mesh := BoxMesh.new()
		var cell_size := BOX / CELL
		grid_mesh.size = Vector3(cell_size, cell_size, cell_size)
		grid_mesh.material = material
		grid_meshes.append(grid_mesh)

	var instances := Node3D.new()
	instances.name = "Instances"
	p_parent.add_child(instances)

	var meshes := Node3D.new()
	meshes.name = "Meshes"
	instances.add_child(meshes)

	var multi_root := Node3D.new()
	multi_root.name = "MultiMeshes"
	instances.add_child(multi_root)
	var multi_counts: Array[int] = []
	multi_counts.resize(ROUGHNESS.size())
	for index in count:
		if int(kind[index]) == 1:
			var bucket := index % ROUGHNESS.size()
			multi_counts[bucket] = int(multi_counts[bucket]) + 1
	var multi_nodes: Array[MultiMeshInstance3D] = []
	var multi_cursor: Array[int] = []
	for index in ROUGHNESS.size():
		var multi := MultiMesh.new()
		multi.transform_format = MultiMesh.TRANSFORM_3D
		multi.mesh = world_meshes[index]
		multi.instance_count = int(multi_counts[index])
		var node := MultiMeshInstance3D.new()
		node.name = "Roughness%d" % index
		node.multimesh = multi
		node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		node.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		multi_root.add_child(node)
		multi_nodes.append(node)
		multi_cursor.append(0)

	var library := MeshLibrary.new()
	for index in ROUGHNESS.size():
		library.create_item(index)
		library.set_item_mesh(index, grid_meshes[index])
		library.set_item_mesh_cast_shadow(index, RenderingServer.SHADOW_CASTING_SETTING_OFF)
		library.set_item_name(index, "Roughness%d" % index)
	var grid := GridMap.new()
	grid.name = "Grid"
	grid.mesh_library = library
	grid.cell_size = Vector3(CELL, CELL, CELL)
	grid.cell_center_x = false
	grid.cell_center_y = false
	grid.cell_center_z = false
	grid.collision_layer = 0
	grid.collision_mask = 0
	grid.bake_navigation = false
	instances.add_child(grid)

	var mesh_nodes := 0
	var multi_instances := 0
	for index in count:
		var position: Vector3 = positions[index]
		var roughness_index := index % ROUGHNESS.size()
		var which := int(kind[index])
		if which == 0:
			var mesh_node := MeshInstance3D.new()
			mesh_node.name = "Mesh%d" % index
			mesh_node.mesh = world_meshes[roughness_index]
			mesh_node.position = position
			mesh_node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			mesh_node.gi_mode = GeometryInstance3D.GI_MODE_STATIC
			meshes.add_child(mesh_node)
			mesh_nodes += 1
		elif which == 1:
			var cursor := int(multi_cursor[roughness_index])
			multi_nodes[roughness_index].multimesh.set_instance_transform(
					cursor, Transform3D(Basis.IDENTITY, position))
			multi_cursor[roughness_index] = cursor + 1
			multi_instances += 1
		else:
			grid.set_cell_item(cell_for(position), roughness_index)

	return {
		"mesh_nodes": mesh_nodes,
		"multimesh_instances": multi_instances,
		"grid_cells": grid.get_used_cells().size(),
		"roughness": roughness,
	}


static func contract_error() -> String:
	var share_50 := shares(50000)
	if int(share_50["mesh"]) != 37500 or int(share_50["multimesh"]) != 7500 or int(share_50["gridmap"]) != 5000:
		return "50000 instance shares are not 37500/7500/5000"
	if int(share_50["mesh"]) <= int(share_50["multimesh"]) + int(share_50["gridmap"]):
		return "mesh instances are not the majority at 50000"
	var share_100 := shares(100000)
	if int(share_100["mesh"]) != 75000 or int(share_100["multimesh"]) != 15000 or int(share_100["gridmap"]) != 10000:
		return "100000 instance shares are not 75000/15000/10000"
	var share_180 := shares(180)
	if int(share_180["mesh"]) != 135 or int(share_180["multimesh"]) != 27 or int(share_180["gridmap"]) != 18:
		return "180 instance shares are not 135/27/18"
	if not is_dropped(50.0, 0.2):
		return "a 0.2 m radius at 50 m should drop"
	if is_dropped(10.0, 0.2):
		return "a 0.2 m radius at 10 m should stay"
	if is_dropped(35.0, 10.0):
		return "a 10 m radius at 35 m should stay"
	var radius := box_radius()
	if is_dropped(35.0, radius):
		return "the 0.4 m box at 35 m should stay"
	if not is_dropped(50.0, radius):
		return "the 0.4 m box at 50 m should drop"
	if _lattice().size() != 110500:
		return "lattice does not have 110500 slots"
	if not is_equal_approx(spline_position(0).x, WAYPOINTS[0].x):
		return "spline frame 0 is not the first waypoint"
	if spline_position(599).distance_to(WAYPOINTS[5]) <= 0.01:
		return "spline frame 599 should still be short of the last waypoint"
	return ""


static func _lattice() -> PackedVector3Array:
	var points := PackedVector3Array()
	# Interior: x 1..22, z -6..6, five layers. 43 * 25 * 5 = 5375.
	for ix in range(2, 45):
		for iz in range(-12, 13):
			for iy in range(1, 6):
				points.append(Vector3(float(ix), float(iy), float(iz)) * CELL)
	# Courtyard: x 28..100, z -36..36, five layers. 145 * 145 * 5 = 105125.
	for ix in range(56, 201):
		for iz in range(-72, 73):
			for iy in range(1, 6):
				points.append(Vector3(float(ix), float(iy), float(iz)) * CELL)
	return points


static func _xz_buckets(p_positions: PackedVector3Array, p_cell: float) -> Dictionary:
	var buckets := {}
	for index in p_positions.size():
		var position := p_positions[index]
		var key := Vector2i(int(floor(position.x / p_cell)), int(floor(position.z / p_cell)))
		if not buckets.has(key):
			buckets[key] = []
		(buckets[key] as Array).append(index)
	return buckets


static func _count_beyond(
		p_positions: PackedVector3Array, p_buckets: Dictionary, p_camera: Vector3,
		p_limit: float, p_cell: float) -> int:
	var dropped := 0
	for key_value in p_buckets:
		var key: Vector2i = key_value
		var indices: Array = p_buckets[key]
		var min_x := float(key.x) * p_cell
		var min_z := float(key.y) * p_cell
		var dx := _axis_gap(p_camera.x, min_x, min_x + p_cell)
		var dz := _axis_gap(p_camera.z, min_z, min_z + p_cell)
		if sqrt(dx * dx + dz * dz) > p_limit:
			dropped += indices.size()
			continue
		var far_x := _axis_far(p_camera.x, min_x, min_x + p_cell)
		var far_z := _axis_far(p_camera.z, min_z, min_z + p_cell)
		# 4 m covers the camera-to-instance vertical gap in this scene.
		if sqrt(far_x * far_x + far_z * far_z + 16.0) <= p_limit:
			continue
		for index_value in indices:
			if p_camera.distance_to(p_positions[int(index_value)]) > p_limit:
				dropped += 1
	return dropped


static func _axis_gap(p_value: float, p_min: float, p_max: float) -> float:
	if p_value < p_min:
		return p_min - p_value
	if p_value > p_max:
		return p_value - p_max
	return 0.0


static func _axis_far(p_value: float, p_min: float, p_max: float) -> float:
	return maxf(absf(p_value - p_min), absf(p_value - p_max))
