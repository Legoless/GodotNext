extends Node

## RD ray-query smoke test (#18; plan §12 P0, §13.2 and §13.4 Q17).
##
## On a local RenderingDevice it builds BLASes and a ray-query-only TLAS
## (hit_sbt_range 0, no RT pipeline), dispatches the rq_kernel / rq_edges
## compute kernels and compares t, instance custom index, primitive ID,
## barycentrics and front face against double-precision analytic values:
##
## - Fixture A: one triangle, one forward and one reverse ray.
## - Fixture B: 4096 seeded rays against 64 instances of a quad and an
##   8-triangle bipyramid. IDs exact; t within 1e-5 relative; barycentrics
##   within 1e-5 absolute.
## - Fixture C: 1024 rays aimed at shared edges. Each must report exactly
##   one candidate, one of the two adjacent triangles.
##
## Every fixture runs twice, once with BLAS inputs in vertex/index buffers
## and once in storage buffers; the two result buffers must be bit-identical.
## A device without ray query asserts the fallback instead of skipping.
##
## Run (see README.md for all hosts):
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/rd_smoke/rd_smoke.tscn
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.
## Writes user://rd_smoke_kernel.spv, user://rd_smoke_edges.spv,
## user://rd_smoke_rays.bin (B records), user://rd_smoke_edges.bin (C records)
## and user://rd_smoke_analytic.json (B analytic records for the Q17 tool).

const SEED := 0x4E455854
const RAYS_B := 4096
const RAYS_C := 1024
const INSTANCES := 64
const INSTANCE_ID_BASE := 1000
const GRID_SPACING := 10.0
const REL_T_TOLERANCE := 1e-5
const ABS_BARY_TOLERANCE := 1e-5
const RECORD_SIZE := 32

# The expected front face follows the API default shared by Vulkan, Metal and DXR: a positive
# Möller–Trumbore determinant (det > 0 below). FLIP_FACING is the only way to invert it.
const TRI_VERTS: Array[float] = [0, 0, 0, 1, 0, 0, 0, 1, 0]
const QUAD_VERTS: Array[float] = [-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0]
const QUAD_IDX: Array[int] = [0, 1, 2, 0, 2, 3]
# Bipyramid: apexes 0 (top) and 1 (bottom), equator 2..5 in the XZ plane.
const BIPY_VERTS: Array[float] = [0, 0.5, 0, 0, -0.5, 0, 0.5, 0, 0, 0, 0, 0.5, -0.5, 0, 0, 0, 0, -0.5]
const BIPY_IDX: Array[int] = [0, 3, 2, 0, 4, 3, 0, 5, 4, 0, 2, 5, 1, 2, 3, 1, 3, 4, 1, 4, 5, 1, 5, 2]

var _failures := PackedStringArray()
var _expected_errors := PackedStringArray()
var _max_rel_t := 0.0
var _max_abs_bary := 0.0


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "test": "rd_smoke", "pass": false, "reason": "NO_RENDERING_DEVICE" }

	if not rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY):
		var fallback := _run_fallback(rd)
		rd.free()
		return fallback

	var kernel := _load_kernel(rd, "res://tests/rd_smoke/rq_kernel.glsl", "user://rd_smoke_kernel.spv")
	var edges := _load_kernel(rd, "res://tests/rd_smoke/rq_edges.glsl", "user://rd_smoke_edges.spv")
	var stats := { "id_exact": 0, "front_face_exact": 0, "edge_ok": 0 }
	if kernel.is_valid():
		_fixture_a(rd, kernel)
		_fixture_b(rd, kernel, stats)
	if edges.is_valid():
		_fixture_c(rd, edges, stats)
	_free_all(rd, [kernel, edges])
	rd.free()

	return {
		"test": "rd_smoke",
		"rays_b": RAYS_B,
		"edges": RAYS_C,
		"id_exact": stats["id_exact"],
		"front_face_exact": stats["front_face_exact"],
		"max_rel_t": _max_rel_t,
		"max_abs_bary": _max_abs_bary,
		"edge_ok": stats["edge_ok"],
		"pass": _failures.is_empty(),
		"failures": _failures,
		"expected_errors": _expected_errors,
	}


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func _run_fallback(rd: RenderingDevice) -> Dictionary:
	# RenderingDevice does not bind get_ray_query_unsupported_reason() yet;
	# assert it when present (enum: 0 NO_RAY_QUERY, 1 D3D12_NO_DXR).
	var reason_ok := true
	if rd.has_method("get_ray_query_unsupported_reason"):
		var reason: int = rd.call("get_ray_query_unsupported_reason")
		reason_ok = reason == 0 or reason == 1
	_expected_errors.append("blas_create: neither raytracing pipeline nor ray query support")
	var blas := rd.blas_create([], 0)
	var pass_fallback := reason_ok and not blas.is_valid()
	return {
		"test": "rd_smoke",
		"fallback": true,
		"pass": pass_fallback,
		"expected_errors": _expected_errors,
	}


func _load_kernel(rd: RenderingDevice, path: String, spv_path: String) -> RID:
	var file := load(path) as RDShaderFile
	_check(file != null, "shader file loads: " + path)
	if file == null:
		return RID()
	var spirv := file.get_spirv()
	_check(spirv != null, "shader file has SPIR-V: " + path)
	if spirv == null:
		return RID()
	_check(spirv.compile_error_compute.is_empty(), "kernel compiles (%s): %s" % [path, spirv.compile_error_compute])
	var bytecode := spirv.get_stage_bytecode(RenderingDevice.SHADER_STAGE_COMPUTE)
	_check(not bytecode.is_empty(), "kernel has compute bytecode: " + path)
	var out := FileAccess.open(spv_path, FileAccess.WRITE)
	_check(out != null, "SPIR-V module saved: " + spv_path)
	if out != null:
		out.store_buffer(bytecode)
		out.close()
	var shader := rd.shader_create_from_spirv(spirv, path.get_file().get_basename())
	_check(shader.is_valid(), "shader created: " + path)
	return shader


# Builds one BLAS from the given geometry. When storage is true the vertex
# and index data live in storage buffers, otherwise in vertex/index buffers.
# Returns [blas, vertex_buffer, index_buffer] (index RID invalid when unused).
func _build_blas(rd: RenderingDevice, verts: Array[float], indices: Array[int], storage: bool, no_duplicate: bool) -> Array[RID]:
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_bytes := PackedFloat32Array(verts).to_byte_array()
	var index_bytes := PackedInt32Array(indices).to_byte_array()
	var vertex_buffer: RID
	var index_buffer := RID()
	if storage:
		vertex_buffer = rd.storage_buffer_create(vertex_bytes.size(), vertex_bytes, 0, input_bits)
		if not indices.is_empty():
			index_buffer = rd.storage_buffer_create(index_bytes.size(), index_bytes, 0, input_bits)
	else:
		vertex_buffer = rd.vertex_buffer_create(vertex_bytes.size(), vertex_bytes, input_bits)
		if not indices.is_empty():
			index_buffer = rd.index_buffer_create(indices.size(), RenderingDevice.INDEX_BUFFER_FORMAT_UINT32, index_bytes, false, input_bits)

	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	if no_duplicate:
		geometry.flags |= RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = verts.size() / 3
	if not indices.is_empty():
		geometry.index_buffer = index_buffer
		geometry.index_count = indices.size()
		geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32

	var blas := rd.blas_create([geometry], 0)
	_check(blas.is_valid() and rd.blas_build(blas) == OK, "BLAS builds (%d triangles, storage=%s)" % [_triangle_count(verts, indices), storage])
	return [blas, vertex_buffer, index_buffer]


func _triangle_count(verts: Array[float], indices: Array[int]) -> int:
	return indices.size() / 3 if not indices.is_empty() else verts.size() / 9


func _instance(blas: RID, id: int, transform: Transform3D) -> RDAccelerationStructureInstance:
	var instance := RDAccelerationStructureInstance.new()
	instance.blas = blas
	instance.id = id
	instance.mask = 0xFF
	instance.hit_sbt_range = 0
	instance.transform = transform
	return instance


func _build_tlas(rd: RenderingDevice, instances: Array[RDAccelerationStructureInstance]) -> RID:
	var tlas := rd.tlas_create(instances.size(), 0)
	_check(tlas.is_valid() and rd.tlas_build(tlas, instances) == OK, "TLAS builds with %d instances at hit_sbt_range 0" % instances.size())
	return tlas


# Dispatches count rays (32 bytes each: origin/tmin, dir/tmax) and returns
# the raw Q17 result records.
func _trace(rd: RenderingDevice, shader: RID, tlas: RID, rays: PackedByteArray, count: int) -> PackedByteArray:
	var pipeline := rd.compute_pipeline_create(shader)
	var ray_buffer := rd.storage_buffer_create(rays.size(), rays)
	var result_buffer := rd.storage_buffer_create(count * RECORD_SIZE)
	var uniforms: Array[RDUniform] = []
	var bindings: Array[RID] = [tlas, ray_buffer, result_buffer]
	var types: Array[int] = [RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER]
	for i in 3:
		var uniform := RDUniform.new()
		uniform.uniform_type = types[i] as RenderingDevice.UniformType
		uniform.binding = i
		uniform.add_id(bindings[i])
		uniforms.append(uniform)
	var uniform_set := rd.uniform_set_create(uniforms, shader, 0)
	_check(pipeline.is_valid() and uniform_set.is_valid(), "trace pipeline and uniform set are created")
	var data := PackedByteArray()
	if pipeline.is_valid() and uniform_set.is_valid():
		var push := PackedInt32Array([count, 0, 0, 0]).to_byte_array()
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
		rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		rd.compute_list_set_push_constant(compute_list, push, push.size())
		rd.compute_list_dispatch(compute_list, ceili(count / 64.0), 1, 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
		data = rd.buffer_get_data(result_buffer)
	_free_all(rd, [uniform_set, result_buffer, ray_buffer, pipeline])
	return data


func _record(data: PackedByteArray, i: int) -> Dictionary:
	var o := i * RECORD_SIZE
	return {
		"t": data.decode_float(o),
		"custom_index": data.decode_u32(o + 4),
		"primitive": data.decode_u32(o + 8),
		"u": data.decode_float(o + 12),
		"v": data.decode_float(o + 16),
		"front_face": data.decode_u32(o + 20),
		"committed": data.decode_u32(o + 24),
		"candidate_count": data.decode_u32(o + 28),
	}


# Scores one record against its analytic hit, counting exact IDs
# (custom index, primitive, committed), exact front face and misses of the
# Q17 t / barycentric tolerances, and updating the running maxima.
func _score(got: Dictionary, expected: Dictionary, counts: Dictionary) -> void:
	if got["custom_index"] == expected["custom_index"] and got["primitive"] == expected["primitive"] and got["committed"] == expected["committed"]:
		counts["id"] += 1
	if got["front_face"] == expected["front_face"]:
		counts["front_face"] += 1
	var rel_t: float = absf(got["t"] - expected["t"]) / absf(expected["t"])
	var bary: float = maxf(absf(got["u"] - expected["u"]), absf(got["v"] - expected["v"]))
	_max_rel_t = maxf(_max_rel_t, rel_t)
	_max_abs_bary = maxf(_max_abs_bary, bary)
	if rel_t > REL_T_TOLERANCE:
		counts["t_over"] += 1
	if bary > ABS_BARY_TOLERANCE:
		counts["bary_over"] += 1


func _counts() -> Dictionary:
	return { "id": 0, "front_face": 0, "t_over": 0, "bary_over": 0 }


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)


# --- Double-precision CPU side -------------------------------------------------
# GDScript floats are doubles, but Vector3 and Basis are single precision, so
# the analytic side works on plain [x, y, z] arrays. Its inputs are the exact
# float32 values the GPU sees: the float32 vertices, the float32 instance
# transforms and the float32-quantized rays.

func _v_sub(a: Array, b: Array) -> Array:
	return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


func _v_dot(a: Array, b: Array) -> float:
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


func _v_cross(a: Array, b: Array) -> Array:
	return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


func _v_norm(a: Array) -> Array:
	var length := sqrt(_v_dot(a, a))
	return [a[0] / length, a[1] / length, a[2] / length]


func _v_at(flat: Array, vertex: int) -> Array:
	return [flat[vertex * 3], flat[vertex * 3 + 1], flat[vertex * 3 + 2]]


# One instance of the fixture B/C scene: kind 0 is the quad, kind 1 the
# bipyramid. 64 instances on a centered 10 m grid with seeded rotations.
func _instance_specs(rng: RandomNumberGenerator) -> Array:
	var specs: Array = []
	for i in INSTANCES:
		var euler := Vector3(rng.randf() * TAU, rng.randf() * TAU, rng.randf() * TAU)
		var origin := Vector3((i % 4 - 1.5) * GRID_SPACING, (i / 4 % 4 - 1.5) * GRID_SPACING, (i / 16 - 1.5) * GRID_SPACING)
		specs.append({
			"kind": i % 2,
			"id": INSTANCE_ID_BASE + i,
			"transform": Transform3D(Basis.from_euler(euler), origin),
		})
	return specs


func _geometry_of(kind: int) -> Array:
	return [QUAD_VERTS, QUAD_IDX] if kind == 0 else [BIPY_VERTS, BIPY_IDX]


func _world_vertices(spec: Dictionary) -> Array:
	var geometry := _geometry_of(spec["kind"])
	var verts: Array = geometry[0]
	var basis: Basis = spec["transform"].basis
	var origin: Vector3 = spec["transform"].origin
	var out: Array = []
	out.resize(verts.size())
	for v in verts.size() / 3:
		var x: float = verts[v * 3]
		var y: float = verts[v * 3 + 1]
		var z: float = verts[v * 3 + 2]
		out[v * 3] = origin.x + basis.x.x * x + basis.y.x * y + basis.z.x * z
		out[v * 3 + 1] = origin.y + basis.x.y * x + basis.y.y * y + basis.z.y * z
		out[v * 3 + 2] = origin.z + basis.x.z * x + basis.y.z * y + basis.z.z * z
	return out


# Flat world triangles for the CPU intersector: 9 doubles per triangle.
func _world_triangles(specs: Array) -> Dictionary:
	var flat := PackedFloat64Array()
	var ids := PackedInt32Array()
	var prims := PackedInt32Array()
	for spec in specs:
		var geometry := _geometry_of(spec["kind"])
		var indices: Array = geometry[1]
		var world := _world_vertices(spec)
		for prim in indices.size() / 3:
			for corner in 3:
				var vertex: int = indices[prim * 3 + corner]
				flat.append_array([world[vertex * 3], world[vertex * 3 + 1], world[vertex * 3 + 2]])
			ids.append(spec["id"])
			prims.append(prim)
	return { "flat": flat, "ids": ids, "prims": prims }


# Two-sided Möller–Trumbore against every world triangle. Returns the closest
# hit and the second-closest t, so callers can reject ties.
func _intersect(tris: Dictionary, origin: Array, dir: Array, tmin: float, tmax: float) -> Dictionary:
	var flat: PackedFloat64Array = tris["flat"]
	var best := { "hit": false, "t": tmax, "second_t": tmax, "u": 0.0, "v": 0.0, "custom_index": 0, "primitive": 0, "front_face": 0 }
	for tri in flat.size() / 9:
		var o := tri * 9
		var e1 := [flat[o + 3] - flat[o], flat[o + 4] - flat[o + 1], flat[o + 5] - flat[o + 2]]
		var e2 := [flat[o + 6] - flat[o], flat[o + 7] - flat[o + 1], flat[o + 8] - flat[o + 2]]
		var p := _v_cross(dir, e2)
		var det := _v_dot(e1, p)
		if absf(det) < 1e-12:
			continue
		var inv := 1.0 / det
		var s := [origin[0] - flat[o], origin[1] - flat[o + 1], origin[2] - flat[o + 2]]
		var u := _v_dot(s, p) * inv
		if u < 0.0 or u > 1.0:
			continue
		var q := _v_cross(s, e1)
		var v := _v_dot(dir, q) * inv
		if v < 0.0 or u + v > 1.0:
			continue
		var t := _v_dot(e2, q) * inv
		if t < tmin or t > tmax:
			continue
		if t < best["t"]:
			best["second_t"] = best["t"]
			best["hit"] = true
			best["t"] = t
			best["u"] = u
			best["v"] = v
			best["custom_index"] = tris["ids"][tri]
			best["primitive"] = tris["prims"][tri]
			best["front_face"] = 1 if det > 0.0 else 0
		elif t < best["second_t"]:
			best["second_t"] = t
	return best


# A unit direction inside the cone of half-angle max_deg around axis.
func _dir_in_cone(axis: Array, max_deg: float, rng: RandomNumberGenerator) -> Array:
	var helper: Array = [1, 0, 0] if absf(axis[0]) < 0.9 else [0, 1, 0]
	var t1 := _v_norm(_v_cross(axis, helper))
	var t2 := _v_cross(axis, t1)
	var cos_theta := lerpf(cos(deg_to_rad(max_deg)), 1.0, rng.randf())
	var sin_theta := sqrt(maxf(0.0, 1.0 - cos_theta * cos_theta))
	var phi := rng.randf() * TAU
	var a := sin_theta * cos(phi)
	var b := sin_theta * sin(phi)
	return _v_norm([
		axis[0] * cos_theta + t1[0] * a + t2[0] * b,
		axis[1] * cos_theta + t1[1] * a + t2[1] * b,
		axis[2] * cos_theta + t1[2] * a + t2[2] * b,
	])


# Appends one float32-quantized ray and returns [origin, dir] as the exact
# float32 values the GPU reads back.
func _append_ray(rays: PackedByteArray, origin: Array, tmin: float, dir: Array, tmax: float) -> Array:
	var o := rays.size()
	rays.resize(o + 32)
	var values: Array[float] = [origin[0], origin[1], origin[2], tmin, dir[0], dir[1], dir[2], tmax]
	for i in 8:
		rays.encode_float(o + i * 4, values[i])
	var q_origin: Array = [rays.decode_float(o), rays.decode_float(o + 4), rays.decode_float(o + 8)]
	var q_dir: Array = [rays.decode_float(o + 16), rays.decode_float(o + 20), rays.decode_float(o + 24)]
	return [q_origin, q_dir]


# Builds the 64-instance TLAS for fixtures B and C. Returns all RIDs to free,
# TLAS first.
func _build_scene(rd: RenderingDevice, specs: Array, storage: bool, no_duplicate: bool) -> Array:
	var rids: Array = []
	var blases: Array[RID] = []
	for kind in 2:
		var geometry := _geometry_of(kind)
		var built := _build_blas(rd, geometry[0], geometry[1], storage, no_duplicate)
		blases.append(built[0])
		rids.append_array(built)
	var instances: Array[RDAccelerationStructureInstance] = []
	for spec in specs:
		instances.append(_instance(blases[spec["kind"]], spec["id"], spec["transform"]))
	var tlas := _build_tlas(rd, instances)
	rids.push_front(tlas)
	return rids


func _fixture_a(rd: RenderingDevice, shader: RID) -> void:
	var rays := PackedByteArray()
	_append_ray(rays, [0.25, 0.25, 0], 0.001, [0, 0, -1], 100)
	_append_ray(rays, [0.25, 0.25, -10], 0.001, [0, 0, 1], 100)
	var expected: Array[Dictionary] = [
		{ "t": 5.0, "custom_index": 7, "primitive": 0, "u": 0.25, "v": 0.25, "front_face": 1, "committed": 1 },
		{ "t": 5.0, "custom_index": 7, "primitive": 0, "u": 0.25, "v": 0.25, "front_face": 0, "committed": 1 },
	]
	var variants: Array[PackedByteArray] = []
	for storage in [false, true]:
		var built := _build_blas(rd, TRI_VERTS, [], storage, false)
		var transform := Transform3D(Basis(), Vector3(0, 0, -5))
		var tlas := _build_tlas(rd, [_instance(built[0], 7, transform)])
		var data := _trace(rd, shader, tlas, rays, 2)
		_check(data.size() == 2 * RECORD_SIZE, "fixture A returns 2 records (storage=%s)" % storage)
		if data.size() == 2 * RECORD_SIZE:
			var counts := _counts()
			for i in 2:
				_score(_record(data, i), expected[i], counts)
			_check(counts["id"] == 2 and counts["t_over"] == 0 and counts["bary_over"] == 0, "fixture A matches the analytic t, IDs and barycentrics (storage=%s)" % storage)
			_check(counts["front_face"] == 2, "fixture A front face is counter-clockwise (storage=%s)" % storage)
			_check(_record(data, 0)["front_face"] != _record(data, 1)["front_face"], "fixture A forward and reverse rays disagree on front face")
		variants.append(data)
		_free_all(rd, [tlas] + Array(built))
	if variants.size() == 2:
		_check(variants[0] == variants[1], "fixture A vertex-buffer and storage-buffer results are bit-identical")


# Generates the 4096 fixture B rays and their analytic hits. A candidate is
# kept only when the CPU intersector confirms the intended triangle is the
# closest hit, at least 1e-3 m ahead of the second hit and at least 1e-3 in
# barycentric distance from every edge.
func _generate_b(specs: Array, tris: Dictionary, rng: RandomNumberGenerator) -> Dictionary:
	var rays := PackedByteArray()
	var expected: Array = []
	var worlds: Array = []
	for spec in specs:
		worlds.append(_world_vertices(spec))
	var attempts := 0
	while expected.size() < RAYS_B and attempts < RAYS_B * 50:
		attempts += 1
		var si := rng.randi_range(0, INSTANCES - 1)
		var spec: Dictionary = specs[si]
		var geometry := _geometry_of(spec["kind"])
		var indices: Array = geometry[1]
		var prim := rng.randi_range(0, indices.size() / 3 - 1)
		var world: Array = worlds[si]
		var v0 := _v_at(world, indices[prim * 3])
		var v1 := _v_at(world, indices[prim * 3 + 1])
		var v2 := _v_at(world, indices[prim * 3 + 2])
		var e1 := _v_sub(v1, v0)
		var e2 := _v_sub(v2, v0)
		var u := 0.05 + rng.randf() * 0.8
		var v := 0.05 + rng.randf() * (0.85 - u)
		var target := [v0[0] + e1[0] * u + e2[0] * v, v0[1] + e1[1] * u + e2[1] * v, v0[2] + e1[2] * u + e2[2] * v]
		var normal := _v_norm(_v_cross(e1, e2))
		var from_back: bool = spec["kind"] == 0 and rng.randf() < 0.5
		var axis := normal if from_back else [-normal[0], -normal[1], -normal[2]]
		var dir := _dir_in_cone(axis, 60.0, rng)
		var d := lerpf(0.5, 2.0, rng.randf())
		var origin := [target[0] - dir[0] * d, target[1] - dir[1] * d, target[2] - dir[2] * d]
		var candidate := PackedByteArray()
		var quantized := _append_ray(candidate, origin, 0.001, dir, 100)
		var hit := _intersect(tris, quantized[0], quantized[1], 0.001, 100)
		if not hit["hit"] or hit["custom_index"] != spec["id"] or hit["primitive"] != prim:
			continue
		if hit["second_t"] - hit["t"] <= 1e-3:
			continue
		if hit["u"] < 1e-3 or hit["v"] < 1e-3 or 1.0 - hit["u"] - hit["v"] < 1e-3:
			continue
		rays.append_array(candidate)
		expected.append({
			"t": hit["t"], "custom_index": hit["custom_index"], "primitive": hit["primitive"],
			"u": hit["u"], "v": hit["v"], "front_face": hit["front_face"], "committed": 1, "candidate_count": 1,
		})
	_check(expected.size() == RAYS_B, "fixture B generated %d rays" % RAYS_B)
	return { "rays": rays, "expected": expected }


func _fixture_b(rd: RenderingDevice, shader: RID, stats: Dictionary) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var specs := _instance_specs(rng)
	var generated := _generate_b(specs, _world_triangles(specs), rng)
	var rays: PackedByteArray = generated["rays"]
	var expected: Array = generated["expected"]
	if expected.size() != RAYS_B:
		return
	var variants: Array[PackedByteArray] = []
	for storage in [false, true]:
		var rids := _build_scene(rd, specs, storage, false)
		var data := _trace(rd, shader, rids[0], rays, RAYS_B)
		_check(data.size() == RAYS_B * RECORD_SIZE, "fixture B returns %d records (storage=%s)" % [RAYS_B, storage])
		if data.size() == RAYS_B * RECORD_SIZE:
			var counts := _counts()
			for i in RAYS_B:
				_score(_record(data, i), expected[i], counts)
			_check(counts["id"] == RAYS_B, "fixture B IDs exact %d/%d (storage=%s)" % [counts["id"], RAYS_B, storage])
			_check(counts["front_face"] == RAYS_B, "fixture B front face exact %d/%d (storage=%s)" % [counts["front_face"], RAYS_B, storage])
			_check(counts["t_over"] == 0 and counts["bary_over"] == 0, "fixture B within Q17 tolerances (t over: %d, barycentrics over: %d, storage=%s)" % [counts["t_over"], counts["bary_over"], storage])
			if not storage:
				stats["id_exact"] = counts["id"]
				stats["front_face_exact"] = counts["front_face"]
				_write_bytes("user://rd_smoke_rays.bin", data)
				_write_text("user://rd_smoke_analytic.json", JSON.stringify({ "records": expected }))
		variants.append(data)
		_free_all(rd, rids)
	if variants.size() == 2:
		_check(variants[0] == variants[1], "fixture B vertex-buffer and storage-buffer results are bit-identical")


# Edges shared by exactly two triangles of one geometry, as
# {a, b, adjacent: [prim, prim]} in local vertex indices.
func _shared_edges(indices: Array) -> Array:
	var by_key := {}
	for prim in indices.size() / 3:
		for corner in 3:
			var a: int = indices[prim * 3 + corner]
			var b: int = indices[prim * 3 + (corner + 1) % 3]
			var key := "%d_%d" % [mini(a, b), maxi(a, b)]
			if not by_key.has(key):
				by_key[key] = { "a": a, "b": b, "adjacent": [] }
			by_key[key]["adjacent"].append(prim)
	var edges: Array = []
	for key in by_key:
		if by_key[key]["adjacent"].size() == 2:
			edges.append(by_key[key])
	return edges


func _generate_c(specs: Array, tris: Dictionary, rng: RandomNumberGenerator) -> Dictionary:
	var rays := PackedByteArray()
	var expected: Array = []
	var worlds: Array = []
	var edges_by_kind: Array = [_shared_edges(QUAD_IDX), _shared_edges(BIPY_IDX)]
	for spec in specs:
		worlds.append(_world_vertices(spec))
	var attempts := 0
	while expected.size() < RAYS_C and attempts < RAYS_C * 50:
		attempts += 1
		var si := rng.randi_range(0, INSTANCES - 1)
		var spec: Dictionary = specs[si]
		var geometry := _geometry_of(spec["kind"])
		var indices: Array = geometry[1]
		var edges: Array = edges_by_kind[spec["kind"]]
		var edge: Dictionary = edges[rng.randi_range(0, edges.size() - 1)]
		var world: Array = worlds[si]
		var a := _v_at(world, edge["a"])
		var b := _v_at(world, edge["b"])
		var s := lerpf(0.1, 0.9, rng.randf())
		var target := [lerpf(a[0], b[0], s), lerpf(a[1], b[1], s), lerpf(a[2], b[2], s)]
		var mean := [0.0, 0.0, 0.0]
		for prim in edge["adjacent"]:
			var v0 := _v_at(world, indices[prim * 3])
			var n := _v_norm(_v_cross(_v_sub(_v_at(world, indices[prim * 3 + 1]), v0), _v_sub(_v_at(world, indices[prim * 3 + 2]), v0)))
			for c in 3:
				mean[c] += n[c]
		mean = _v_norm(mean)
		var dir := _dir_in_cone([-mean[0], -mean[1], -mean[2]], 45.0, rng)
		var d := lerpf(0.5, 2.0, rng.randf())
		var origin := [target[0] - dir[0] * d, target[1] - dir[1] * d, target[2] - dir[2] * d]
		var candidate := PackedByteArray()
		var quantized := _append_ray(candidate, origin, 0.001, dir, 100)
		var hit := _intersect(tris, quantized[0], quantized[1], 0.001, 100)
		if not hit["hit"] or hit["custom_index"] != spec["id"] or not edge["adjacent"].has(hit["primitive"]):
			continue
		rays.append_array(candidate)
		expected.append({ "custom_index": spec["id"], "adjacent": edge["adjacent"] })
	_check(expected.size() == RAYS_C, "fixture C generated %d edge rays" % RAYS_C)
	return { "rays": rays, "expected": expected }


func _fixture_c(rd: RenderingDevice, shader: RID, stats: Dictionary) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var specs := _instance_specs(rng)
	var generated := _generate_c(specs, _world_triangles(specs), rng)
	var rays: PackedByteArray = generated["rays"]
	var expected: Array = generated["expected"]
	if expected.size() != RAYS_C:
		return
	var variants: Array[PackedByteArray] = []
	for storage in [false, true]:
		var rids := _build_scene(rd, specs, storage, true)
		var data := _trace(rd, shader, rids[0], rays, RAYS_C)
		_check(data.size() == RAYS_C * RECORD_SIZE, "fixture C returns %d records (storage=%s)" % [RAYS_C, storage])
		if data.size() == RAYS_C * RECORD_SIZE:
			var ok := 0
			for i in RAYS_C:
				var record := _record(data, i)
				if record["candidate_count"] == 1 and record["committed"] == 1 and record["custom_index"] == expected[i]["custom_index"] and expected[i]["adjacent"].has(record["primitive"]):
					ok += 1
			_check(ok == RAYS_C, "fixture C exactly one adjacent triangle %d/%d (storage=%s)" % [ok, RAYS_C, storage])
			if not storage:
				stats["edge_ok"] = ok
				_write_bytes("user://rd_smoke_edges.bin", data)
		variants.append(data)
		_free_all(rd, rids)
	if variants.size() == 2:
		_check(variants[0] == variants[1], "fixture C vertex-buffer and storage-buffer results are bit-identical")


func _write_bytes(path: String, data: PackedByteArray) -> void:
	var out := FileAccess.open(path, FileAccess.WRITE)
	_check(out != null, "records written: " + path)
	if out != null:
		out.store_buffer(data)
		out.close()


func _write_text(path: String, text: String) -> void:
	var out := FileAccess.open(path, FileAccess.WRITE)
	_check(out != null, "text written: " + path)
	if out != null:
		out.store_string(text)
		out.close()
