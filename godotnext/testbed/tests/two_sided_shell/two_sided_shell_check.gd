extends Node3D

## #186 step 6: a single cull_disabled quad is a hit from either side on the
## software tier. The verification readback counts coverage hits on each side
## of the card. The side the ray meets along the card's basis axis is the back
## face (SWRT_HIT_BACKFACE), so the WRC probe relocation fires there. The other
## side is the front face. Master counts a hit from one side only.
##
## A cull_disabled box shell stands beside the card. It is not flat, so it has
## no card normal: its coverage hits keep the gradient normal and none is
## marked as a card or as back-facing.

const WAIT_FRAMES := 300
const MAX_FRAMES := 900
const MIN_HITS := 12
const HIT_SURFACE := 1
const HIT_SCREEN := 4
const HIT_COVERAGE := 64
const HIT_SKIPPED := 128
const HIT_CARD := 1024
const HIT_BACKFACE := 2048
const CARD_SIZE := Vector2(1.6, 1.6)
const SHELL_SIZE := Vector3(0.6, 0.6, 0.6)
const SHELL_POSITION := Vector3(-1.6, 0.3, 0.2)

var _card_axis := Vector3.UP
var _center := Vector3(0, 1.2, 0)
var _frames := 0
var _done := false
var _expected_tier := "SWRT"


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if str(arg).begins_with("--expected-tier="):
			_expected_tier = str(arg).trim_prefix("--expected-tier=")
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.25, 0.28, 0.32)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.15, 0.15, 0.15)
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)
	_add_receiver("Floor", Vector3(5.0, 0.2, 7.0), Vector3(0, -0.1, 1.0))
	_add_receiver("Ceiling", Vector3(5.0, 0.2, 7.0), Vector3(0, 2.4, 1.0))
	var basis := Basis(Vector3.RIGHT, -PI / 2.0)
	_card_axis = basis.z.normalized()
	var quad := _add_two_sided(_quad_mesh(CARD_SIZE), Vector3(CARD_SIZE.x, CARD_SIZE.y, 0.0), Transform3D(basis, _center))
	quad.name = "Card"
	var shell_box := BoxMesh.new()
	shell_box.size = SHELL_SIZE
	var shell := _add_two_sided(shell_box, SHELL_SIZE, Transform3D(Basis(), SHELL_POSITION))
	shell.name = "Shell"
	var camera := Camera3D.new()
	camera.position = Vector3(0, 1.2, 2.6)
	camera.fov = 60.0
	add_child(camera)
	camera.look_at(_center, Vector3.UP)
	camera.current = true


func _add_receiver(p_name: String, p_size: Vector3, p_position: Vector3) -> void:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.6, 0.6, 0.62)
	material.roughness = 0.8
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = mesh
	instance.position = p_position
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	add_child(instance)


func _quad_mesh(p_size: Vector2) -> QuadMesh:
	var quad := QuadMesh.new()
	quad.size = p_size
	return quad


## A cull_disabled instance of a primitive with the unsigned field of its box (a
## quad is a box with no depth).
func _add_two_sided(p_primitive: PrimitiveMesh, p_size: Vector3, p_transform: Transform3D) -> MeshInstance3D:
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, p_primitive.get_mesh_arrays())
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.15, 0.45, 0.2)
	material.roughness = 0.6
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.surface_set_material(0, material)
	mesh.distance_field = _unsigned_box_field(p_size)
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.transform = p_transform
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	add_child(instance)
	return instance


## The unsigned distance to the surface of a box of p_size (a quad when its depth
## is 0), one mip, in the MeshSDFBuilder brick layout.
func _unsigned_box_field(p_size: Vector3) -> MeshDistanceField:
	var voxel := 0.05
	var pad := 4
	var half := p_size * 0.5
	var dims := Vector3i(ceili(p_size.x / voxel) + pad * 2, ceili(p_size.y / voxel) + pad * 2, ceili(p_size.z / voxel) + pad * 2 + 1)
	var origin := -half - Vector3.ONE * voxel * float(pad)
	var band := voxel * float(pad)
	var grid := Vector3i((dims.x + 7) / 8, (dims.y + 7) / 8, (dims.z + 7) / 8)
	var pages := PackedInt32Array()
	pages.resize(grid.x * grid.y * grid.z)
	var bricks := PackedByteArray()
	for bz in grid.z:
		for by in grid.y:
			for bx in grid.x:
				var values := PackedFloat32Array()
				values.resize(512)
				var nearest := INF
				for lz in 8:
					for ly in 8:
						for lx in 8:
							var v := Vector3i(mini(bx * 8 + lx, dims.x - 1), mini(by * 8 + ly, dims.y - 1), mini(bz * 8 + lz, dims.z - 1))
							var p := origin + (Vector3(v) + Vector3.ONE * 0.5) * voxel
							var q := p.abs() - half
							var outside := Vector3(maxf(q.x, 0.0), maxf(q.y, 0.0), maxf(q.z, 0.0)).length()
							var distance := absf(outside + minf(maxf(q.x, maxf(q.y, q.z)), 0.0))
							values[lx + 8 * (ly + 8 * lz)] = distance
							nearest = minf(nearest, distance)
				var page := bx + grid.x * (by + grid.y * bz)
				if nearest > voxel:
					pages[page] = -1
					continue
				pages[page] = bricks.size() / 512
				for value in values:
					bricks.push_back(clampi(roundi((clampf(value / band, -1.0, 1.0) * 0.5 + 0.5) * 255.0), 0, 255))
	var field := MeshDistanceField.new()
	field.bounds = AABB(origin, Vector3(dims) * voxel)
	field.voxel_size = voxel
	field.flags = MeshDistanceField.FLAG_TWO_SIDED | MeshDistanceField.FLAG_UNSIGNED
	field.mip_dimensions = PackedInt32Array([dims.x, dims.y, dims.z])
	field.mip_brick_grid_dimensions = PackedInt32Array([grid.x, grid.y, grid.z])
	field.mip_bands = PackedFloat32Array([band])
	field.page_tables = [pages]
	field.brick_data = [bricks]
	return field


func _process(_delta: float) -> void:
	if _done:
		return
	_frames += 1
	if _frames < WAIT_FRAMES:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var readback: Dictionary = info.get("swrt_trace_readback", {})
	if not readback.has("gather"):
		if _frames >= MAX_FRAMES:
			_finish(["no SWRT readback (tier %s)" % str(info.get("tier", "?"))], {})
		return
	_finish(_checks(info, readback), _summary(info))


func _summary(p_info: Dictionary) -> Dictionary:
	return {"tier": p_info.get("tier", ""), "card_axis": _card_axis}


func _checks(p_info: Dictionary, p_readback: Dictionary) -> PackedStringArray:
	var reasons := PackedStringArray()
	var tier := str(p_info.get("tier", ""))
	if tier != _expected_tier:
		reasons.append("tier is %s, expected %s" % [tier, _expected_tier])
	if int(p_info.get("swrt_trace_two_sided", 0)) < 2:
		reasons.append("the quad and the shell did not both enter the SWRT scene as two-sided (%d)" % int(p_info.get("swrt_trace_two_sided", 0)))
	var bytes: PackedByteArray = p_readback["gather"]
	var front := 0
	var back := 0
	var front_marked_back := 0
	var back_marked_back := 0
	var shell_hits := 0
	var shell_marked := 0
	var shell_bounds := AABB(SHELL_POSITION - SHELL_SIZE * 0.5, SHELL_SIZE).grow(0.25)
	var count := bytes.size() / 80
	for index in count:
		var base := index * 80
		var flags := bytes.decode_u32(base + 76)
		if flags & HIT_SKIPPED or flags & HIT_SCREEN or (flags & HIT_SURFACE) == 0 or (flags & HIT_COVERAGE) == 0:
			continue
		var surface := Vector3(bytes.decode_float(base), bytes.decode_float(base + 4), bytes.decode_float(base + 8))
		var offset := bytes.decode_float(base + 12)
		var surface_normal := Vector3(bytes.decode_float(base + 16), bytes.decode_float(base + 20), bytes.decode_float(base + 24))
		var direction := Vector3(bytes.decode_float(base + 32), bytes.decode_float(base + 36), bytes.decode_float(base + 40))
		var t := bytes.decode_float(base + 60)
		if t < 0.0:
			continue
		var origin := surface + surface_normal * offset
		var hit := origin + direction * t
		if shell_bounds.has_point(hit):
			shell_hits += 1
			if flags & (HIT_CARD | HIT_BACKFACE):
				shell_marked += 1
			continue
		var from_center := hit - _center
		if absf(from_center.dot(_card_axis)) > 0.35 or absf(from_center.x) > CARD_SIZE.x * 0.5 + 0.25 or absf(from_center.z) > CARD_SIZE.y * 0.5 + 0.25:
			continue
		var facing := direction.dot(_card_axis)
		if facing > 0.45:
			back += 1
			if flags & HIT_BACKFACE:
				back_marked_back += 1
		elif facing < -0.45:
			front += 1
			if flags & HIT_BACKFACE:
				front_marked_back += 1
	print("TWO_SIDED front=%d back=%d front_marked_back=%d back_marked_back=%d shell=%d shell_marked=%d axis=%s" % [front, back, front_marked_back, back_marked_back, shell_hits, shell_marked, _card_axis])
	if front < MIN_HITS or back < MIN_HITS:
		reasons.append("coverage hits from the front %d and the back %d; both sides need at least %d" % [front, back, MIN_HITS])
	if back < MIN_HITS or back_marked_back < back:
		reasons.append("back-side coverage hits marked back-facing: %d of %d" % [back_marked_back, back])
	if front_marked_back != 0:
		reasons.append("%d front-side coverage hits were marked back-facing" % front_marked_back)
	if shell_hits < MIN_HITS:
		reasons.append("only %d coverage hits on the box shell (at least %d)" % [shell_hits, MIN_HITS])
	if shell_marked != 0:
		reasons.append("%d of %d coverage hits on the box shell were marked as a card or back-facing; it is not flat" % [shell_marked, shell_hits])
	return reasons


func _finish(p_reasons: PackedStringArray, p_summary: Dictionary) -> void:
	_done = true
	p_summary["pass"] = p_reasons.is_empty()
	p_summary["test"] = "two_sided_shell"
	p_summary["failures"] = p_reasons
	print("GODOTNEXT_RESULT " + JSON.stringify(p_summary))
	for reason in p_reasons:
		push_error("TWO_SIDED_FAIL %s" % reason)
	get_tree().quit(0 if p_reasons.is_empty() else 1)
