extends SceneTree

## Acceleration-structure BUILD/UPDATE (#45).
##
## On a local RenderingDevice:
## - An ALLOW_UPDATE BLAS over a 64x64 quad grid (8192 triangles) is displaced
##   by a seeded compute pass (at most 10% of the extent). UPDATE is compared
##   with a fresh BUILD of the displaced vertices on 4096 rays. IDs match.
##   t is within 1e-5 relative and barycentrics within 1e-5 absolute, both
##   between the two GPU results and against the analytic centroid hit.
## - A TLAS UPDATE that moves instances is compared with a fresh BUILD the
##   same way, on 4096 rays.
## - Each invalid UPDATE returns ERR_INVALID_PARAMETER, leaves buffer memory
##   unchanged, and does not change hits that were already built.
## - Buffer memory stays constant across 1000 alternating BUILD/UPDATE frames.
##
## A device without ray query checks the fallback and exits 0.
##
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal \
##     --script res://tests/rd_smoke/as_build_modes.gd

const SEED := 0x4E455854
const GRID := 64
const STRIDE := 65
const RAYS := 4096
const REL_T_TOLERANCE := 1e-5
const ABS_BARY_TOLERANCE := 1e-5
const RECORD_SIZE := 32
const RAY_SIZE := 32
const AMPLITUDE := 0.1
const SCRATCH_FRAMES := 1000
const MIN_BARY := 1e-3

var _failures := PackedStringArray()
var _expected_errors := PackedStringArray()


func _initialize() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	quit(0 if result["pass"] else 1)


func _process(_delta: float) -> bool:
	return true


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)
		print("FAIL: " + what)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "test": "as_build_modes", "pass": false, "reason": "NO_RENDERING_DEVICE" }
	if not rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY):
		var fallback := _run_fallback(rd)
		rd.free()
		return fallback

	var kernel := _load_shader(rd, "res://tests/rd_smoke/rq_kernel.glsl")
	var displace := _load_shader(rd, "res://tests/rd_smoke/as_build_modes_displace.glsl")
	var stats := {
		"blas_rays": 0,
		"tlas_rays": 0,
		"max_rel_t": 0.0,
		"max_abs_bary": 0.0,
		"invalid_ok": false,
		"scratch_before": 0,
		"scratch_after": 0,
	}
	if kernel.is_valid() and displace.is_valid():
		_check_invalid(rd, kernel)
		stats["invalid_ok"] = _failures.is_empty()
		_check_blas_refit(rd, kernel, displace, stats)
		_check_tlas_refit(rd, kernel, stats)
		_check_scratch(rd, stats)
	_free_all(rd, [kernel, displace])
	rd.free()
	stats["test"] = "as_build_modes"
	stats["pass"] = _failures.is_empty()
	stats["failures"] = _failures
	stats["expected_errors"] = _expected_errors
	return stats


func _run_fallback(rd: RenderingDevice) -> Dictionary:
	_expected_errors.append("blas_create: neither raytracing pipeline nor ray query support")
	var blas := rd.blas_create([], 0)
	return {
		"test": "as_build_modes",
		"fallback": true,
		"pass": not blas.is_valid(),
		"expected_errors": _expected_errors,
	}


func _load_shader(rd: RenderingDevice, path: String) -> RID:
	var file := load(path) as RDShaderFile
	_check(file != null, "shader file loads: " + path)
	if file == null:
		return RID()
	var spirv := file.get_spirv()
	_check(spirv != null and spirv.compile_error_compute.is_empty(), "shader compiles (%s): %s" % [path, spirv.compile_error_compute if spirv != null else "missing"])
	if spirv == null or not spirv.compile_error_compute.is_empty():
		return RID()
	var shader := rd.shader_create_from_spirv(spirv, path.get_file().get_basename())
	_check(shader.is_valid(), "shader created: " + path)
	return shader


func _input_bits() -> int:
	return RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT


func _triangle_buffers(rd: RenderingDevice) -> Array[RID]:
	var verts := PackedFloat32Array([0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0])
	var indices := PackedInt32Array([0, 1, 2])
	var vertex_bytes := verts.to_byte_array()
	var index_bytes := indices.to_byte_array()
	var vertex_buffer := rd.storage_buffer_create(vertex_bytes.size(), vertex_bytes, 0, _input_bits())
	var index_buffer := rd.storage_buffer_create(index_bytes.size(), index_bytes, 0, _input_bits())
	return [vertex_buffer, index_buffer]


func _make_blas(rd: RenderingDevice, vertex_buffer: RID, index_buffer: RID, vertex_count: int, index_count: int, flags: int) -> RID:
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = vertex_count
	geometry.index_buffer = index_buffer
	geometry.index_count = index_count
	geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32
	var blas := rd.blas_create([geometry], flags)
	_check(blas.is_valid(), "BLAS creates")
	return blas


func _instance(blas: RID, id: int, origin: Vector3) -> RDAccelerationStructureInstance:
	var instance := RDAccelerationStructureInstance.new()
	instance.blas = blas
	instance.id = id
	instance.mask = 0xFF
	instance.hit_sbt_range = 0
	instance.transform = Transform3D(Basis.IDENTITY, origin)
	return instance


func _sync_build(rd: RenderingDevice, blas: RID, mode: int) -> int:
	var err := rd.blas_build(blas, mode)
	if err == OK:
		rd.submit()
		rd.sync()
	return err


func _sync_tlas(rd: RenderingDevice, tlas: RID, instances: Array[RDAccelerationStructureInstance], mode: int) -> int:
	var err := rd.tlas_build(tlas, instances, mode)
	if err == OK:
		rd.submit()
		rd.sync()
	return err


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
	else:
		_check(false, "trace pipeline and uniform set are created")
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
	}


func _pack_ray(data: PackedByteArray, i: int, ox: float, oy: float, oz: float) -> void:
	var o := i * RAY_SIZE
	data.encode_float(o, ox)
	data.encode_float(o + 4, oy)
	data.encode_float(o + 8, oz)
	data.encode_float(o + 12, 0.0)
	data.encode_float(o + 16, 0.0)
	data.encode_float(o + 20, -1.0)
	data.encode_float(o + 24, 0.0)
	data.encode_float(o + 28, 4.0)


func _expect_error(message: String) -> void:
	_expected_errors.append(message)


func _memory(rd: RenderingDevice) -> int:
	return int(rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS))


func _check_invalid(rd: RenderingDevice, kernel: RID) -> void:
	var buffers := _triangle_buffers(rd)
	var vertex_buffer: RID = buffers[0]
	var index_buffer: RID = buffers[1]
	var mode_update := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE
	var mode_build := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_BUILD

	var unbuilt := _make_blas(rd, vertex_buffer, index_buffer, 3, 3, 0)
	var mem_unbuilt := _memory(rd)
	_expect_error("Acceleration structure UPDATE requires ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT.")
	_check(rd.blas_build(unbuilt, mode_update) == ERR_INVALID_PARAMETER, "UPDATE without ALLOW_UPDATE and before BUILD returns ERR_INVALID_PARAMETER")
	_check(_memory(rd) == mem_unbuilt, "UPDATE without ALLOW_UPDATE allocates no scratch")

	var never := _make_blas(rd, vertex_buffer, index_buffer, 3, 3, RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)
	var mem_never := _memory(rd)
	_expect_error("Acceleration structure UPDATE requires a previous BUILD. The structure has not been built, or it was invalidated.")
	_check(rd.blas_build(never, mode_update) == ERR_INVALID_PARAMETER, "UPDATE before any BUILD returns ERR_INVALID_PARAMETER")
	_expect_error("Acceleration structure UPDATE requires a previous BUILD. The structure has not been built, or it was invalidated.")
	_check(rd.blas_build(never, mode_update) == ERR_INVALID_PARAMETER, "a rejected UPDATE leaves the BLAS unbuilt")
	_check(_memory(rd) == mem_never, "UPDATE before BUILD allocates no scratch")
	_check(_sync_build(rd, never, mode_build) == OK, "BUILD after a rejected UPDATE succeeds")

	var fixed := _make_blas(rd, vertex_buffer, index_buffer, 3, 3, 0)
	_check(_sync_build(rd, fixed, mode_build) == OK, "BLAS without ALLOW_UPDATE builds")
	var tlas_fixed := rd.tlas_create(1, 0)
	var fixed_instances: Array[RDAccelerationStructureInstance] = [_instance(fixed, 7, Vector3.ZERO)]
	_check(_sync_tlas(rd, tlas_fixed, fixed_instances, mode_build) == OK, "TLAS over a fixed BLAS builds")
	var fixed_rays := PackedByteArray()
	fixed_rays.resize(RAY_SIZE)
	_pack_ray(fixed_rays, 0, 1.0 / 3.0, 2.0, 2.0 / 3.0)
	var before_hits := _trace(rd, kernel, tlas_fixed, fixed_rays, 1)
	var mem_fixed := _memory(rd)
	_expect_error("Acceleration structure UPDATE requires ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT.")
	_check(rd.blas_build(fixed, mode_update) == ERR_INVALID_PARAMETER, "UPDATE without ALLOW_UPDATE returns ERR_INVALID_PARAMETER")
	_check(_memory(rd) == mem_fixed, "rejected UPDATE without ALLOW_UPDATE allocates no scratch")
	var after_hits := _trace(rd, kernel, tlas_fixed, fixed_rays, 1)
	_check(before_hits == after_hits and before_hits.size() == RECORD_SIZE, "rejected UPDATE leaves the built BLAS hits unchanged")
	if before_hits.size() == RECORD_SIZE:
		var hit := _record(before_hits, 0)
		_check(hit["committed"] == 1 and hit["custom_index"] == 7 and hit["primitive"] == 0 and hit["front_face"] == 1, "fixed triangle reports the analytic hit")

	var tlas_early := rd.tlas_create(1, RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)
	var mem_early := _memory(rd)
	_expect_error("Acceleration structure UPDATE requires a previous BUILD. The structure has not been built, or it was invalidated.")
	_check(rd.tlas_build(tlas_early, fixed_instances, mode_update) == ERR_INVALID_PARAMETER, "TLAS UPDATE before any BUILD returns ERR_INVALID_PARAMETER")
	_check(_memory(rd) == mem_early, "TLAS UPDATE before BUILD allocates no scratch or instance buffer")

	var moved := _make_blas(rd, vertex_buffer, index_buffer, 3, 3, RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)
	_check(_sync_build(rd, moved, mode_build) == OK, "instance-count BLAS builds")
	var count_tlas := rd.tlas_create(2, RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)
	var two: Array[RDAccelerationStructureInstance] = [
		_instance(moved, 10, Vector3(0.0, 0.0, 0.0)),
		_instance(moved, 11, Vector3(3.0, 0.0, 0.0)),
	]
	_check(_sync_tlas(rd, count_tlas, two, mode_build) == OK, "TLAS builds two instances")
	var two_rays := PackedByteArray()
	two_rays.resize(RAY_SIZE * 2)
	_pack_ray(two_rays, 0, 1.0 / 3.0, 2.0, 2.0 / 3.0)
	_pack_ray(two_rays, 1, 3.0 + 1.0 / 3.0, 2.0, 2.0 / 3.0)
	var two_before := _trace(rd, kernel, count_tlas, two_rays, 2)
	var mem_count := _memory(rd)
	var one: Array[RDAccelerationStructureInstance] = [_instance(moved, 10, Vector3(0.0, 0.0, 0.0))]
	_expect_error("Top-level acceleration structure UPDATE cannot change the instance count. Rebuild it instead.")
	_check(rd.tlas_build(count_tlas, one, mode_update) == ERR_INVALID_PARAMETER, "TLAS UPDATE that changes the instance count returns ERR_INVALID_PARAMETER")
	_check(_memory(rd) == mem_count, "rejected TLAS instance-count UPDATE allocates nothing")
	var two_after := _trace(rd, kernel, count_tlas, two_rays, 2)
	_check(two_before == two_after and two_before.size() == RECORD_SIZE * 2, "rejected instance-count UPDATE keeps both previous hits")

	_free_all(rd, [count_tlas, moved, tlas_early, tlas_fixed, fixed, never, unbuilt, index_buffer, vertex_buffer])


func _grid_vertices() -> PackedFloat32Array:
	var verts := PackedFloat32Array()
	verts.resize(STRIDE * STRIDE * 3)
	var n := 0
	for iz in STRIDE:
		for ix in STRIDE:
			verts[n] = float(ix) / float(GRID)
			verts[n + 1] = 0.0
			verts[n + 2] = float(iz) / float(GRID)
			n += 3
	return verts


func _grid_indices() -> PackedInt32Array:
	# (v00, v01, v11) and (v00, v11, v10). A ray along (0, -1, 0) has a positive
	# Möller–Trumbore determinant, which is Godot's front face. FLIP_FACING is not set.
	var indices := PackedInt32Array()
	indices.resize(GRID * GRID * 6)
	var n := 0
	for iz in GRID:
		for ix in GRID:
			var v00 := iz * STRIDE + ix
			var v10 := v00 + 1
			var v01 := v00 + STRIDE
			var v11 := v01 + 1
			indices[n] = v00
			indices[n + 1] = v01
			indices[n + 2] = v11
			indices[n + 3] = v00
			indices[n + 4] = v11
			indices[n + 5] = v10
			n += 6
	return indices


func _displace(rd: RenderingDevice, shader: RID, vertex_buffer: RID, vertex_count: int) -> void:
	var pipeline := rd.compute_pipeline_create(shader)
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = 0
	uniform.add_id(vertex_buffer)
	var uniform_set := rd.uniform_set_create([uniform], shader, 0)
	_check(pipeline.is_valid() and uniform_set.is_valid(), "displacement pipeline is created")
	if pipeline.is_valid() and uniform_set.is_valid():
		var push := PackedByteArray()
		push.resize(16)
		push.encode_u32(0, vertex_count)
		push.encode_u32(4, SEED)
		push.encode_float(8, AMPLITUDE)
		push.encode_float(12, 0.0)
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
		rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		rd.compute_list_set_push_constant(compute_list, push, push.size())
		rd.compute_list_dispatch(compute_list, ceili(vertex_count / 64.0), 1, 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
	_free_all(rd, [uniform_set, pipeline])


func _grid_rays(verts: PackedFloat32Array) -> Dictionary:
	var rays := PackedByteArray()
	rays.resize(RAYS * RAY_SIZE)
	var analytic: Array[Dictionary] = []
	var n := 0
	for iz in GRID:
		for ix in GRID:
			var v00 := (iz * STRIDE + ix) * 3
			var v01 := ((iz + 1) * STRIDE + ix) * 3
			var v11 := ((iz + 1) * STRIDE + ix + 1) * 3
			var x := (verts[v00] + verts[v01] + verts[v11]) / 3.0
			var y := (verts[v00 + 1] + verts[v01 + 1] + verts[v11 + 1]) / 3.0
			var z := (verts[v00 + 2] + verts[v01 + 2] + verts[v11 + 2]) / 3.0
			_pack_ray(rays, n, x, 2.0, z)
			analytic.append({
				"t": 2.0 - y,
				"custom_index": 42,
				"primitive": (iz * GRID + ix) * 2,
				"u": 1.0 / 3.0,
				"v": 1.0 / 3.0,
				"front_face": 1,
				"committed": 1,
			})
			n += 1
	return { "rays": rays, "analytic": analytic }


func _hash_u32(x: int) -> int:
	var n := x & 0x7fffffff
	n = (n * 1103515245 + 12345) & 0x7fffffff
	return n


func _bary_pair(index: int) -> Vector2:
	for n in 64:
		var h1 := _hash_u32(index * 2 + n)
		var h2 := _hash_u32(index * 2 + n + 7919)
		var a := float(h1 & 0xffffff) / float(0xffffff)
		var b := float(h2 & 0xffffff) / float(0xffffff)
		if a + b > 1.0:
			a = 1.0 - a
			b = 1.0 - b
		var c := 1.0 - a - b
		if minf(a, minf(b, c)) >= MIN_BARY:
			return Vector2(a, b)
	return Vector2(1.0 / 3.0, 1.0 / 3.0)


func _compare(update_data: PackedByteArray, build_data: PackedByteArray, analytic: Array[Dictionary], label: String, stats: Dictionary) -> void:
	_check(update_data.size() == analytic.size() * RECORD_SIZE and build_data.size() == update_data.size(), label + " returns one record per ray")
	if update_data.size() != analytic.size() * RECORD_SIZE or build_data.size() != update_data.size():
		return
	var id_mismatch := 0
	var pair_mismatch := 0
	var t_over := 0
	var bary_over := 0
	var front_mismatch := 0
	var uncommitted := 0
	var first := ""
	for i in analytic.size():
		var got := _record(update_data, i)
		var rebuilt := _record(build_data, i)
		var expected: Dictionary = analytic[i]
		if got["committed"] != 1 or rebuilt["committed"] != 1:
			uncommitted += 1
		if got["custom_index"] != expected["custom_index"] or got["primitive"] != expected["primitive"] or rebuilt["custom_index"] != expected["custom_index"] or rebuilt["primitive"] != expected["primitive"] or got["custom_index"] != rebuilt["custom_index"] or got["primitive"] != rebuilt["primitive"]:
			id_mismatch += 1
		if got["front_face"] != expected["front_face"] or rebuilt["front_face"] != expected["front_face"] or got["front_face"] != rebuilt["front_face"] or got["committed"] != rebuilt["committed"]:
			front_mismatch += 1
			pair_mismatch += 1
		var rel_pair := absf(got["t"] - rebuilt["t"]) / maxf(absf(rebuilt["t"]), 1e-12)
		var rel_got := absf(got["t"] - float(expected["t"])) / maxf(absf(float(expected["t"])), 1e-12)
		var rel_rebuilt := absf(rebuilt["t"] - float(expected["t"])) / maxf(absf(float(expected["t"])), 1e-12)
		var bary_pair := maxf(absf(got["u"] - rebuilt["u"]), absf(got["v"] - rebuilt["v"]))
		var bary_got := maxf(absf(got["u"] - float(expected["u"])), absf(got["v"] - float(expected["v"])))
		var bary_rebuilt := maxf(absf(rebuilt["u"] - float(expected["u"])), absf(rebuilt["v"] - float(expected["v"])))
		var rel_t := maxf(rel_pair, maxf(rel_got, rel_rebuilt))
		var bary := maxf(bary_pair, maxf(bary_got, bary_rebuilt))
		stats["max_rel_t"] = maxf(float(stats["max_rel_t"]), rel_t)
		stats["max_abs_bary"] = maxf(float(stats["max_abs_bary"]), bary)
		if rel_t > REL_T_TOLERANCE:
			t_over += 1
		if bary > ABS_BARY_TOLERANCE:
			bary_over += 1
		if first.is_empty() and (id_mismatch > 0 or front_mismatch > 0 or uncommitted > 0 or t_over > 0 or bary_over > 0):
			first = "ray %d update=%s rebuild=%s expected t=%s u=%s v=%s prim=%s" % [i, got, rebuilt, expected["t"], expected["u"], expected["v"], expected["primitive"]]
	_check(id_mismatch == 0 and front_mismatch == 0 and uncommitted == 0 and t_over == 0 and bary_over == 0, "%s IDs exact, t within 1e-5 relative, barycentrics within 1e-5 absolute (id=%d front=%d miss=%d t=%d bary=%d) %s" % [label, id_mismatch, front_mismatch, uncommitted, t_over, bary_over, first])


func _check_blas_refit(rd: RenderingDevice, kernel: RID, displace: RID, stats: Dictionary) -> void:
	var verts := _grid_vertices()
	var indices := _grid_indices()
	var vertex_bytes := verts.to_byte_array()
	var index_bytes := indices.to_byte_array()
	var vertex_buffer := rd.storage_buffer_create(vertex_bytes.size(), vertex_bytes, 0, _input_bits())
	var index_buffer := rd.storage_buffer_create(index_bytes.size(), index_bytes, 0, _input_bits())
	var flags := RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT
	var mode_build := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_BUILD
	var mode_update := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE
	var updated := _make_blas(rd, vertex_buffer, index_buffer, STRIDE * STRIDE, indices.size(), flags)
	_check(_sync_build(rd, updated, mode_build) == OK, "grid BLAS builds")
	_displace(rd, displace, vertex_buffer, STRIDE * STRIDE)
	var readback := rd.buffer_get_data(vertex_buffer).to_float32_array()
	_check(readback.size() == STRIDE * STRIDE * 3, "displaced vertices read back")
	var max_abs_y := 0.0
	for i in STRIDE * STRIDE:
		max_abs_y = maxf(max_abs_y, absf(readback[i * 3 + 1]))
	_check(max_abs_y <= AMPLITUDE + 1e-6 and max_abs_y > 0.0, "displacement stays within 10 percent of the extent (max abs y=%s)" % max_abs_y)
	stats["max_abs_y"] = max_abs_y
	_check(_sync_build(rd, updated, mode_update) == OK, "grid BLAS refits")
	var rebuilt := _make_blas(rd, vertex_buffer, index_buffer, STRIDE * STRIDE, indices.size(), flags)
	_check(_sync_build(rd, rebuilt, mode_build) == OK, "fresh grid BLAS builds from the displaced vertices")
	var prepared := _grid_rays(readback)
	var rays: PackedByteArray = prepared["rays"]
	var analytic: Array[Dictionary] = prepared["analytic"]
	var tlas_update := rd.tlas_create(1, 0)
	var tlas_build := rd.tlas_create(1, 0)
	var instances_update: Array[RDAccelerationStructureInstance] = [_instance(updated, 42, Vector3.ZERO)]
	var instances_build: Array[RDAccelerationStructureInstance] = [_instance(rebuilt, 42, Vector3.ZERO)]
	_check(_sync_tlas(rd, tlas_update, instances_update, mode_build) == OK, "TLAS over the refit BLAS builds")
	_check(_sync_tlas(rd, tlas_build, instances_build, mode_build) == OK, "TLAS over the rebuilt BLAS builds")
	var update_hits := _trace(rd, kernel, tlas_update, rays, RAYS)
	var build_hits := _trace(rd, kernel, tlas_build, rays, RAYS)
	stats["blas_rays"] = RAYS
	_compare(update_hits, build_hits, analytic, "BLAS refit vs rebuild", stats)
	_free_all(rd, [tlas_update, tlas_build, updated, rebuilt, index_buffer, vertex_buffer])


func _check_tlas_refit(rd: RenderingDevice, kernel: RID, stats: Dictionary) -> void:
	var buffers := _triangle_buffers(rd)
	var vertex_buffer: RID = buffers[0]
	var index_buffer: RID = buffers[1]
	var blas := _make_blas(rd, vertex_buffer, index_buffer, 3, 3, 0)
	var mode_build := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_BUILD
	var mode_update := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE
	_check(_sync_build(rd, blas, mode_build) == OK, "TLAS-test BLAS builds")
	var flags := RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT
	var updated := rd.tlas_create(4, flags)
	var initial: Array[RDAccelerationStructureInstance] = []
	for i in 4:
		initial.append(_instance(blas, 10 + i, Vector3(float(i) * 3.0, 0.0, 0.0)))
	_check(_sync_tlas(rd, updated, initial, mode_build) == OK, "TLAS builds before the instance move")
	var moved: Array[RDAccelerationStructureInstance] = []
	for i in 4:
		moved.append(_instance(blas, 10 + i, Vector3(float(i) * 3.0, 0.05, 0.2)))
	_check(_sync_tlas(rd, updated, moved, mode_update) == OK, "TLAS refits moved instances")
	var rebuilt := rd.tlas_create(4, flags)
	_check(_sync_tlas(rd, rebuilt, moved, mode_build) == OK, "fresh TLAS builds the moved instances")

	var rays := PackedByteArray()
	rays.resize(RAYS * RAY_SIZE)
	var analytic: Array[Dictionary] = []
	for i in RAYS:
		var instance_index := i / 1024
		var bary := _bary_pair(i)
		var a := bary.x
		var b := bary.y
		var origin := Vector3(float(instance_index) * 3.0 + b, 0.05, 0.2 + a + b)
		_pack_ray(rays, i, origin.x, 2.0, origin.z)
		analytic.append({
			"t": 2.0 - 0.05,
			"custom_index": 10 + instance_index,
			"primitive": 0,
			"u": a,
			"v": b,
			"front_face": 1,
			"committed": 1,
		})
	var update_hits := _trace(rd, kernel, updated, rays, RAYS)
	var build_hits := _trace(rd, kernel, rebuilt, rays, RAYS)
	stats["tlas_rays"] = RAYS
	_compare(update_hits, build_hits, analytic, "TLAS refit vs rebuild", stats)
	_free_all(rd, [updated, rebuilt, blas, index_buffer, vertex_buffer])


func _check_scratch(rd: RenderingDevice, stats: Dictionary) -> void:
	var verts := _grid_vertices()
	var indices := _grid_indices()
	var vertex_buffer := rd.storage_buffer_create(verts.to_byte_array().size(), verts.to_byte_array(), 0, _input_bits())
	var index_buffer := rd.storage_buffer_create(indices.to_byte_array().size(), indices.to_byte_array(), 0, _input_bits())
	var blas := _make_blas(rd, vertex_buffer, index_buffer, STRIDE * STRIDE, indices.size(), RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)
	var mode_build := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_BUILD
	var mode_update := RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE
	_check(_sync_build(rd, blas, mode_build) == OK, "scratch BLAS builds")
	var before := _memory(rd)
	var samples := PackedInt64Array()
	samples.append(before)
	var build_error := false
	for i in SCRATCH_FRAMES:
		var mode := mode_update if i % 2 == 0 else mode_build
		if _sync_build(rd, blas, mode) != OK:
			build_error = true
			_check(false, "scratch frame %d build failed" % i)
			break
		if i == 0 or i == 1 or i == 2 or i == 9 or i == 99 or i == SCRATCH_FRAMES - 1:
			samples.append(_memory(rd))
			if i == 0 or i == 249 or i == 499 or i == 749:
				print("as_build_modes: scratch frame %d memory %d" % [i + 1, _memory(rd)])
	var after := _memory(rd)
	stats["scratch_before"] = before
	stats["scratch_after"] = after
	stats["scratch_samples"] = samples
	_check(not build_error and before == after, "scratch buffer bytes stay constant over %d alternating frames (%d -> %d)" % [SCRATCH_FRAMES, before, after])
	_free_all(rd, [blas, index_buffer, vertex_buffer])


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)
