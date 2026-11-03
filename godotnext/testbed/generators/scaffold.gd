extends RefCounted
## GodotNext testbed generator scaffolding (#10).
##
## Shared, deterministic helpers for every seeded scene generator in this
## folder, including the later 10M+ triangle generators:
## seed plumbing ([method make_rng]), a chunked [SurfaceTool] writer
## ([ChunkedSurfaceBuilder]) that flushes into [ArrayMesh] surfaces so
## memory stays bounded, a chunked text writer ([method write_text_chunked]),
## a deterministic JSON writer ([method to_json]) and a byte-stable scene
## saver ([method save_scene]).

const DEFAULT_SEED := 0x6E657874
const Q1_PATCH_RADIUS := 0.25
const Q1_PATCH_MIN_DISTANCE := 0.5
const Q1_PATCH_EDGE_MARGIN := 0.3


## Seed for stream [param p_stream], derived from [constant DEFAULT_SEED].
## [method make_rng] uses this value, and scene sidecars record it.
static func stream_seed(p_stream: int) -> int:
	return DEFAULT_SEED + p_stream * 1013904223


## Returns a [RandomNumberGenerator] for stream [param p_stream], derived
## deterministically from [constant DEFAULT_SEED]. Every generator takes a
## distinct stream number so streams stay independent of build order.
static func make_rng(p_stream: int) -> RandomNumberGenerator:
	var rng := RandomNumberGenerator.new()
	rng.seed = stream_seed(p_stream)
	return rng


## Constant-parameter [StandardMaterial3D] as required for gate scenes:
## no textures, Lambert diffuse. Colors are given in linear space and are
## stored sRGB-encoded, so the rendered linear value matches the input.
static func make_constant_material(
		p_name: String,
		p_albedo_linear: Color,
		p_roughness: float = 1.0,
		p_metallic: float = 0.0,
		p_metallic_specular: float = 0.0,
		p_emission_linear: Color = Color(0.0, 0.0, 0.0, 1.0),
		p_emission_energy_multiplier: float = 1.0) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.resource_name = p_name
	material.albedo_color = p_albedo_linear.linear_to_srgb()
	material.roughness = p_roughness
	material.metallic = p_metallic
	material.metallic_specular = p_metallic_specular
	material.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
	if p_emission_linear.get_luminance() > 0.0:
		material.emission_enabled = true
		material.emission = p_emission_linear.linear_to_srgb()
		material.emission_energy_multiplier = p_emission_energy_multiplier
	return material


static func add_owned(p_parent: Node, p_child: Node, p_root: Node) -> Node:
	p_parent.add_child(p_child)
	p_child.owner = p_root
	return p_child


## A [MeshInstance3D] box (centered at [param p_center]) with an optional
## [StaticBody3D] box collider as a child named "Collider".
static func add_box_instance(
		p_root: Node,
		p_parent: Node,
		p_name: String,
		p_center: Vector3,
		p_size: Vector3,
		p_material: Material,
		p_collider: bool = true) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = mesh
	instance.position = p_center
	if p_material != null:
		instance.set_surface_override_material(0, p_material)
	add_owned(p_parent, instance, p_root)
	if p_collider:
		var body := StaticBody3D.new()
		body.name = "Collider"
		add_owned(instance, body, p_root)
		var shape_node := CollisionShape3D.new()
		shape_node.name = "Shape"
		var shape := BoxShape3D.new()
		shape.size = p_size
		shape_node.shape = shape
		add_owned(body, shape_node, p_root)
	return instance


## A trimesh [StaticBody3D] collider for a single-mesh [MeshInstance3D].
static func add_trimesh_collider(p_root: Node, p_instance: MeshInstance3D) -> void:
	var body := StaticBody3D.new()
	body.name = "Collider"
	add_owned(p_instance, body, p_root)
	var shape_node := CollisionShape3D.new()
	shape_node.name = "Shape"
	shape_node.shape = p_instance.mesh.create_trimesh_shape()
	add_owned(body, shape_node, p_root)


static func make_camera(
		p_root: Node,
		p_parent: Node,
		p_name: String,
		p_position: Vector3,
		p_target: Vector3,
		p_far: float = 50.0,
		p_fov: float = 70.0,
		p_current: bool = false) -> Camera3D:
	var camera := Camera3D.new()
	camera.name = p_name
	var direction := (p_target - p_position).normalized()
	camera.transform = Transform3D(Basis.looking_at(direction, Vector3.UP), p_position)
	camera.fov = p_fov
	camera.near = 0.05
	camera.far = p_far
	camera.current = p_current
	add_owned(p_parent, camera, p_root)
	return camera


## Manual perspective-frustum test (16:9), independent of viewport state, so
## generator assertions give the same answer on every host.
static func is_in_frustum(p_camera: Camera3D, p_world_position: Vector3) -> bool:
	var local := p_camera.global_transform.affine_inverse() * p_world_position
	if local.z > -p_camera.near or local.z < -p_camera.far:
		return false
	var half_height := -local.z * tan(deg_to_rad(p_camera.fov) * 0.5)
	var half_width := half_height * 16.0 / 9.0
	return absf(local.y) <= half_height and absf(local.x) <= half_width


## Chunked [SurfaceTool] writer: flushes into a new [ArrayMesh] surface every
## [member ChunkedSurfaceBuilder.chunk_triangle_limit] triangles, so very
## large generated meshes never hold one giant surface in memory.
class ChunkedSurfaceBuilder:
	var mesh := ArrayMesh.new()
	var chunk_triangle_limit := 8192
	var _material: Material
	var _tool: SurfaceTool
	var _triangles_in_chunk := 0

	func _init(p_material: Material, p_chunk_triangle_limit: int = 8192) -> void:
		_material = p_material
		chunk_triangle_limit = maxi(p_chunk_triangle_limit, 1)
		_begin()

	func _begin() -> void:
		_tool = SurfaceTool.new()
		_tool.begin(Mesh.PRIMITIVE_TRIANGLES)
		if _material != null:
			_tool.set_material(_material)

	func flush() -> void:
		if _triangles_in_chunk == 0:
			return
		_tool.commit(mesh)
		_triangles_in_chunk = 0
		_begin()

	func finish() -> ArrayMesh:
		flush()
		return mesh

	func add_triangle(p_a: Vector3, p_b: Vector3, p_c: Vector3, p_normal: Vector3) -> void:
		if _triangles_in_chunk >= chunk_triangle_limit:
			flush()
		for vertex in [p_a, p_b, p_c]:
			_tool.set_normal(p_normal)
			_tool.set_uv(Vector2(vertex.x + vertex.z, vertex.y + vertex.z))
			_tool.add_vertex(vertex)
		_triangles_in_chunk += 1

	## Quad on the plane through [param p_center] spanned by the half-edge
	## vectors [param p_u] and [param p_v]; [code]p_u.cross(p_v)[/code] must
	## point along [param p_normal]. The triangles are emitted clockwise as
	## seen from the [param p_normal] side, which is Godot's front-face winding.
	func add_quad(p_center: Vector3, p_u: Vector3, p_v: Vector3, p_normal: Vector3) -> void:
		var a := p_center - p_u - p_v
		var b := p_center + p_u - p_v
		var c := p_center + p_u + p_v
		var d := p_center - p_u + p_v
		add_triangle(a, c, b, p_normal)
		add_triangle(a, d, c, p_normal)

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


## One planar rectangular surface usable for Q1 patch placement.
## [param p_u] and [param p_v] are the full edge vectors from [param p_origin].
static func make_patch_surface(
		p_node: String, p_origin: Vector3, p_u: Vector3, p_v: Vector3, p_normal: Vector3, p_camera: String) -> Dictionary:
	return {
		"node": p_node,
		"origin": p_origin,
		"u": p_u,
		"v": p_v,
		"normal": p_normal.normalized(),
		"camera": p_camera,
	}


## Greedy jittered-grid Q1 patch placement: up to [param p_count] patches,
## each at least [constant Q1_PATCH_MIN_DISTANCE] m from the others and
## [constant Q1_PATCH_EDGE_MARGIN] m from every surface edge. Patches come
## back in deterministic acceptance order; callers may filter and renumber.
static func generate_q1_patches(p_surfaces: Array, p_count: int, p_rng: RandomNumberGenerator) -> Array:
	var candidates: Array = []
	for surface_value in p_surfaces:
		var surface: Dictionary = surface_value
		var origin: Vector3 = surface["origin"]
		var u: Vector3 = surface["u"]
		var v: Vector3 = surface["v"]
		var u_length := u.length()
		var v_length := v.length()
		var u_dir := u.normalized()
		var v_dir := v.normalized()
		var u_steps := int(floor((u_length - 2.0 * Q1_PATCH_EDGE_MARGIN) / Q1_PATCH_MIN_DISTANCE)) + 1
		var v_steps := int(floor((v_length - 2.0 * Q1_PATCH_EDGE_MARGIN) / Q1_PATCH_MIN_DISTANCE)) + 1
		for iu in range(maxi(u_steps, 0)):
			for iv in range(maxi(v_steps, 0)):
				var du := Q1_PATCH_EDGE_MARGIN + float(iu) * Q1_PATCH_MIN_DISTANCE + p_rng.randf_range(-0.08, 0.08)
				var dv := Q1_PATCH_EDGE_MARGIN + float(iv) * Q1_PATCH_MIN_DISTANCE + p_rng.randf_range(-0.08, 0.08)
				du = clampf(du, Q1_PATCH_EDGE_MARGIN, u_length - Q1_PATCH_EDGE_MARGIN)
				dv = clampf(dv, Q1_PATCH_EDGE_MARGIN, v_length - Q1_PATCH_EDGE_MARGIN)
				candidates.append({
					"pos": origin + u_dir * du + v_dir * dv,
					"normal": surface["normal"],
					"node": surface["node"],
					"camera": surface["camera"],
				})
	# Deterministic Fisher-Yates shuffle.
	for i in range(candidates.size() - 1, 0, -1):
		var j := p_rng.randi_range(0, i)
		var tmp = candidates[i]
		candidates[i] = candidates[j]
		candidates[j] = tmp
	var accepted: Array = []
	var min_distance_sq := Q1_PATCH_MIN_DISTANCE * Q1_PATCH_MIN_DISTANCE
	for candidate_value in candidates:
		if accepted.size() >= p_count:
			break
		var candidate: Dictionary = candidate_value
		var pos: Vector3 = candidate["pos"]
		var far_enough := true
		for other_value in accepted:
			var other: Dictionary = other_value
			if pos.distance_squared_to(other["pos"]) < min_distance_sq:
				far_enough = false
				break
		if far_enough:
			accepted.append(candidate)
	# Acceptance order is kept: callers line-of-sight-filter the surplus and
	# sort/renumber the survivors, which keeps the spread across surfaces.
	var patches: Array = []
	for i in range(accepted.size()):
		var candidate: Dictionary = accepted[i]
		var pos: Vector3 = candidate["pos"]
		var normal: Vector3 = candidate["normal"]
		patches.append({
			"id": i,
			"pos": [pos.x, pos.y, pos.z],
			"normal": [normal.x, normal.y, normal.z],
			"radius": Q1_PATCH_RADIUS,
			"node": candidate["node"],
			"camera": candidate["camera"],
		})
	return patches


## Deterministic JSON: sorted object keys, fixed 6-decimal floats.
static func to_json(p_value: Variant, p_indent: int = 0) -> String:
	var pad := "\t".repeat(p_indent)
	var pad_inner := "\t".repeat(p_indent + 1)
	match typeof(p_value):
		TYPE_DICTIONARY:
			var dict: Dictionary = p_value
			if dict.is_empty():
				return "{}"
			var keys: Array = dict.keys()
			keys.sort()
			var parts: Array[String] = []
			for key in keys:
				parts.append(pad_inner + JSON.stringify(String(key)) + ": " + to_json(dict[key], p_indent + 1))
			return "{\n" + ",\n".join(parts) + "\n" + pad + "}"
		TYPE_ARRAY, TYPE_PACKED_STRING_ARRAY:
			var array: Array = p_value
			if array.is_empty():
				return "[]"
			var scalar := true
			for item in array:
				if typeof(item) == TYPE_DICTIONARY or typeof(item) == TYPE_ARRAY:
					scalar = false
					break
			var rendered: Array[String] = []
			for item in array:
				rendered.append(to_json(item, p_indent + 1))
			if scalar:
				return "[" + ", ".join(rendered) + "]"
			var parts: Array[String] = []
			for item_text in rendered:
				parts.append(pad_inner + item_text)
			return "[\n" + ",\n".join(parts) + "\n" + pad + "]"
		TYPE_FLOAT:
			return "%0.6f" % float(p_value)
		TYPE_INT:
			return str(int(p_value))
		TYPE_BOOL:
			return "true" if bool(p_value) else "false"
		TYPE_STRING, TYPE_STRING_NAME:
			return JSON.stringify(String(p_value))
		TYPE_NIL:
			return "null"
	return JSON.stringify(str(p_value))


## Writes [param p_text] to [param p_path] in fixed-size byte chunks, so the
## same code path serves multi-megabyte generated files.
static func write_text_chunked(p_path: String, p_text: String, p_chunk_bytes: int = 65536) -> Error:
	var dir_path := ProjectSettings.globalize_path(p_path.get_base_dir())
	var dir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if dir_err != OK and dir_err != ERR_ALREADY_EXISTS:
		return dir_err
	var file := FileAccess.open(p_path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	var bytes := p_text.to_utf8_buffer()
	var offset := 0
	while offset < bytes.size():
		var end := mini(offset + p_chunk_bytes, bytes.size())
		file.store_buffer(bytes.slice(offset, end))
		offset = end
	file.close()
	return OK


## Packs and saves [param p_root] as a text scene, then strips the volatile
## resource UIDs and node unique IDs that Godot regenerates per run, so two
## generator runs produce byte-identical [code].tscn[/code] files. Scenes
## load by path, which does not need the stripped IDs.
static func save_scene(p_root: Node, p_path: String) -> Error:
	var dir_path := ProjectSettings.globalize_path(p_path.get_base_dir())
	var dir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if dir_err != OK and dir_err != ERR_ALREADY_EXISTS:
		return dir_err
	var packed := PackedScene.new()
	var pack_err := packed.pack(p_root)
	if pack_err != OK:
		return pack_err
	var save_err := ResourceSaver.save(packed, p_path)
	if save_err != OK:
		return save_err
	var text := FileAccess.get_file_as_string(p_path)
	if text.is_empty():
		return ERR_FILE_CORRUPT
	var uid_pattern := RegEx.new()
	uid_pattern.compile("\\s+uid=\"[^\"]*\"")
	text = uid_pattern.sub(text, "", true)
	var unique_pattern := RegEx.new()
	unique_pattern.compile("\\s+unique_id=-?\\d+")
	text = unique_pattern.sub(text, "", true)
	return write_text_chunked(p_path, text)


## User flag for the deliberate regenerate-then-re-bake cycle. Without it,
## the generators keep a scene file that already carries bake data (#238):
## they still build and verify the scene in memory, but do not overwrite
## the file, so a routine determinism run cannot drop a bake silently.
static func replace_baked_requested() -> bool:
	return OS.get_cmdline_user_args().has("--replace-baked")


## True when the text scene at [param p_path] already carries bake data: a
## [LightmapGI] [code]light_data[/code] assignment or a [VoxelGIData]
## resource (sub-resource or external). Reads the scene as text on
## purpose: loading it would need the editor import of the lightmap
## textures, which a headless generator run cannot assume.
static func scene_file_has_bake_data(p_path: String) -> bool:
	if not FileAccess.file_exists(p_path):
		return false
	var text := FileAccess.get_file_as_string(p_path)
	return text.contains("\nlight_data = ") or text.contains("type=\"VoxelGIData\"")
