extends RefCounted
## Seeded overlap-heavy kitbash layout (#181, gate GS-9): 2,400 rock
## instances in piles within 30 m of the origin, the overlap case where
## the plan expects SWRT to beat HWRT. Instances are planned here and
## spawned at runtime; the packed scene is the shell only, like
## NextGI Perf. The first half spawn as individual [MeshInstance3D]
## nodes, the second half as [MultiMesh] instances, so both instance
## feeds carry the overlap load.

const SdfMeshes := preload("res://generators/sdf_mesh_library.gd")
const Scaffold := preload("res://generators/scaffold.gd")

const SEED := 18142
const DEFAULT_INSTANCES := 2400
const CLUSTER_COUNT := 28
const FIELD_RADIUS_M := 27.5
const ROCK_VARIANTS := 4
# Overlap census lower bound for the default plan, counted by
# [method overlap_pairs]. The seeded plan measures well above this.
const MIN_OVERLAP_PAIRS := 1500


static func plan(p_count: int, p_seed: int) -> Dictionary:
	var count := maxi(p_count, 0)
	var rng := RandomNumberGenerator.new()
	rng.seed = p_seed
	var centers: Array[Vector3] = []
	for cluster in CLUSTER_COUNT:
		var angle := rng.randf() * TAU
		var radius := sqrt(rng.randf()) * 23.0
		centers.append(Vector3(cos(angle) * radius, 0.0, sin(angle) * radius))
	var positions := PackedVector3Array()
	var rotations := PackedVector3Array()
	var scales := PackedFloat32Array()
	var variants := PackedInt32Array()
	positions.resize(count)
	rotations.resize(count)
	scales.resize(count)
	variants.resize(count)
	for index in count:
		var center := centers[index % CLUSTER_COUNT]
		var offset := Vector3(
				(rng.randf() + rng.randf() + rng.randf() - 1.5) * 2.4,
				0.0,
				(rng.randf() + rng.randf() + rng.randf() - 1.5) * 2.4)
		var pos := center + offset
		var flat := Vector2(pos.x, pos.z)
		if flat.length() > FIELD_RADIUS_M:
			flat = flat.normalized() * FIELD_RADIUS_M
			pos.x = flat.x
			pos.z = flat.y
		var scale := rng.randf_range(0.55, 1.35)
		var pile := maxf(0.0, 2.4 - offset.length() * 0.55)
		pos.y = scale * 0.45 + rng.randf() * pile
		positions[index] = pos
		rotations[index] = Vector3(rng.randf() * TAU, rng.randf() * TAU, rng.randf() * TAU)
		scales[index] = scale
		variants[index] = index % ROCK_VARIANTS
	return {
		"positions": positions,
		"rotations": rotations,
		"scales": scales,
		"variants": variants,
		"count": count,
		"seed": p_seed,
	}


static func positions_sha256(p_positions: PackedVector3Array) -> String:
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(p_positions.to_byte_array())
	return hashing.finish().hex_encode()


## Pairs whose bounding spheres overlap by at least 15 percent of their
## summed radii. Counted through a uniform grid so a 2,400-instance
## plan stays cheap in the headless check.
static func overlap_pairs(p_plan: Dictionary) -> int:
	var positions: PackedVector3Array = p_plan["positions"]
	var scales: PackedFloat32Array = p_plan["scales"]
	var cell := 3.0
	var buckets := {}
	for index in positions.size():
		var key := _cell_key(positions[index], cell)
		if not buckets.has(key):
			buckets[key] = []
		(buckets[key] as Array).append(index)
	var pairs := 0
	for index in positions.size():
		var pos := positions[index]
		var base := _cell_key(pos, cell)
		for dx in range(-1, 2):
			for dy in range(-1, 2):
				for dz in range(-1, 2):
					var key := base + Vector3i(dx, dy, dz)
					if not buckets.has(key):
						continue
					for other_value in buckets[key]:
						var other := int(other_value)
						if other <= index:
							continue
						var limit := (float(scales[index]) + float(scales[other])) * 0.85
						if pos.distance_squared_to(positions[other]) < limit * limit:
							pairs += 1
	return pairs


static func _cell_key(p_position: Vector3, p_cell: float) -> Vector3i:
	return Vector3i(
			int(floor(p_position.x / p_cell)),
			int(floor(p_position.y / p_cell)),
			int(floor(p_position.z / p_cell)))


static func max_field_radius_m(p_plan: Dictionary) -> float:
	var positions: PackedVector3Array = p_plan["positions"]
	var scales: PackedFloat32Array = p_plan["scales"]
	var furthest := 0.0
	for index in positions.size():
		var pos := positions[index]
		var extent := Vector2(pos.x, pos.z).length() + float(scales[index]) * 1.3
		furthest = maxf(furthest, extent)
	return furthest


## Displaced-icosphere rock. The displacement is a smooth function of
## direction with a per-variant phase, so the mesh stays a closed,
## non-self-intersecting shell at these amplitudes.
static func rock_mesh(p_variant: int, p_material: Material) -> ArrayMesh:
	var base := SdfMeshes.mesh_named("sphere")
	var vertices: PackedVector3Array = (base["vertices"] as PackedVector3Array).duplicate()
	var phase := float(p_variant) * 1.7
	for i in vertices.size():
		var dir := vertices[i].normalized()
		var factor := 1.0 \
				+ 0.16 * sin(3.0 * dir.x + phase) * cos(2.0 * dir.y - phase) \
				+ 0.12 * sin(5.0 * dir.z + 2.0 * phase) * cos(3.0 * dir.x + dir.y)
		vertices[i] = vertices[i] * factor
	var displaced := {
		"name": "rock%d" % p_variant, "closed": true,
		"vertices": vertices, "faces": base["faces"],
	}
	return SdfMeshes.build_array_mesh(displaced, p_material)


static func spawn(p_parent: Node3D, p_plan: Dictionary) -> Dictionary:
	var positions: PackedVector3Array = p_plan["positions"]
	var rotations: PackedVector3Array = p_plan["rotations"]
	var scales: PackedFloat32Array = p_plan["scales"]
	var variants: PackedInt32Array = p_plan["variants"]
	var count := positions.size()
	var split := count / 2

	var material := Scaffold.make_constant_material("Rock", Color(0.42, 0.4, 0.38))
	var rocks: Array[ArrayMesh] = []
	for variant in ROCK_VARIANTS:
		rocks.append(rock_mesh(variant, material))

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
	multi_counts.resize(ROCK_VARIANTS)
	for index in range(split, count):
		multi_counts[int(variants[index])] += 1
	var multi_nodes: Array[MultiMeshInstance3D] = []
	var multi_cursors: Array[int] = []
	for variant in ROCK_VARIANTS:
		var multi := MultiMesh.new()
		multi.transform_format = MultiMesh.TRANSFORM_3D
		multi.mesh = rocks[variant]
		multi.instance_count = multi_counts[variant]
		var node := MultiMeshInstance3D.new()
		node.name = "RockVariant%d" % variant
		node.multimesh = multi
		node.gi_mode = GeometryInstance3D.GI_MODE_STATIC
		multi_root.add_child(node)
		multi_nodes.append(node)
		multi_cursors.append(0)

	var mesh_nodes := 0
	var multi_instances := 0
	for index in count:
		var variant := int(variants[index])
		var euler := rotations[index]
		var basis := Basis.from_euler(euler) * Basis.from_scale(Vector3.ONE * float(scales[index]))
		var transform := Transform3D(basis, positions[index])
		if index < split:
			var node := MeshInstance3D.new()
			node.name = "Rock%d" % index
			node.mesh = rocks[variant]
			node.transform = transform
			node.gi_mode = GeometryInstance3D.GI_MODE_STATIC
			meshes.add_child(node)
			mesh_nodes += 1
		else:
			var cursor := multi_cursors[variant]
			multi_nodes[variant].multimesh.set_instance_transform(cursor, transform)
			multi_cursors[variant] = cursor + 1
			multi_instances += 1
	return {"mesh_nodes": mesh_nodes, "multimesh_instances": multi_instances, "total": count}


static func contract_error() -> String:
	var first := plan(DEFAULT_INSTANCES, SEED)
	if int(first["count"]) < 2000:
		return "kitbash default has %d instances, expected at least 2000" % int(first["count"])
	var second := plan(DEFAULT_INSTANCES, SEED)
	var positions: PackedVector3Array = first["positions"]
	var again: PackedVector3Array = second["positions"]
	if positions_sha256(positions) != positions_sha256(again):
		return "kitbash plan is not deterministic"
	var furthest := max_field_radius_m(first)
	if furthest > 30.0:
		return "kitbash extent reaches %0.2f m, expected within 30 m" % furthest
	var pairs := overlap_pairs(first)
	if pairs < MIN_OVERLAP_PAIRS:
		return "kitbash has %d overlapping pairs, expected at least %d" % [pairs, MIN_OVERLAP_PAIRS]
	return ""
