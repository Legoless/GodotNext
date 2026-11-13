extends Node

## BLAS compaction (#38): hit equivalence, RD accounting, and a 10k create/compact/free churn.
##
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/blas_compact.tscn
##
## Q17: instance and primitive ids exact, t within 1e-5 relative, barycentrics within
## 1e-5 absolute, front face exact. bit_identical records a raw compare of t, u and v.
## Drivers without compaction assert ERR_UNAVAILABLE and exit 0. No ray query does the same.

const TRIANGLE_COUNT := 10000
const RAY_COUNT := 4096
const ACCOUNT_COUNT := 4
const CHURN_COUNT := 10000
const CHURN_BATCH := 250
const CUSTOM_INDEX := 42
const T_REL := 1e-5
const BARY_ABS := 1e-5
const ANALYTIC_T := 1.0
const ANALYTIC_U := 0.375
const ANALYTIC_V := 0.25

const ERROR_NO_RT := "The current rendering device has neither raytracing pipeline nor ray query support."
const ERROR_UNSUPPORTED := "BLAS compaction is not supported by this rendering driver."
const ERROR_NO_FLAG := "BLAS was not created with ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT."
const ERROR_UNBUILT := "BLAS either has not been built yet, or has been invalidated by an operation and needs to be rebuilt."
const ERROR_PENDING := "BLAS compaction is already in progress."
const ERROR_COMPACTED := "BLAS is already compacted."
const ERROR_NOT_BLAS := "Compaction requires a bottom-level acceleration structure."
const ERROR_REBUILD := "A compacted BLAS cannot be rebuilt."

const KERNEL := """#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

struct Result {
	float t;
	uint custom_index;
	uint primitive;
	float u;
	float v;
	uint front_face;
	uint committed;
	uint candidate_count;
};

layout(set = 0, binding = 1, std430) restrict writeonly buffer Results {
	Result data[];
}
results;

layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	float x = float(i) * 2.0 + 0.5;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, vec3(x, 0.25, 1.0), 0.0, vec3(0.0, 0.0, -1.0), 10.0);
	rayQueryProceedEXT(rq);
	Result result;
	result.t = -1.0;
	result.custom_index = 0u;
	result.primitive = 0u;
	result.u = 0.0;
	result.v = 0.0;
	result.front_face = 0u;
	result.committed = 0u;
	result.candidate_count = 0u;
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		result.t = rayQueryGetIntersectionTEXT(rq, true);
		result.custom_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		result.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		result.u = bary.x;
		result.v = bary.y;
		result.front_face = rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 1u : 0u;
		result.committed = 1u;
		result.candidate_count = 1u;
	}
	results.data[i] = result;
}
"""

var _failures := PackedStringArray()
var _expected_errors := PackedStringArray()
var _ray_query := false
var _bit_identical := false
var _sizes := {}
var _accounting := {}
var _churn_ms := -1


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		_failures.append("NO_RENDERING_DEVICE")
		return _result("no_device")

	_ray_query = rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY)
	if not _ray_query:
		_expected_errors.append(ERROR_NO_RT)
		var err := rd.blas_compact(RID())
		_check(err == ERR_UNAVAILABLE, "no ray query: blas_compact returned %d" % err)
		rd.free()
		return _result("no_ray_query")

	var mode := _probe(rd)
	if mode != "compacted":
		rd.free()
		return _result(mode)

	_check_accounting(rd)
	_check_equivalence(rd)
	_check_negatives(rd)
	_check_churn(rd)
	rd.free()
	return _result("compacted")


func _result(mode: String) -> Dictionary:
	return {
		"pass": _failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"ray_query": _ray_query,
		"mode": mode,
		"failures": _failures,
		"expected_errors": _expected_errors,
		"bit_identical": _bit_identical,
		"sizes": _sizes,
		"accounting": _accounting,
		"churn_ms": _churn_ms,
	}


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func _settle(rd: RenderingDevice) -> void:
	rd.submit()
	rd.sync()


func _input_bits() -> int:
	return RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT


func _triangle_buffer(rd: RenderingDevice) -> RID:
	var vertices := PackedFloat32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]).to_byte_array()
	return rd.vertex_buffer_create(vertices.size(), vertices, _input_bits())


func _geometry_from(vertex_buffer: RID, vertex_count: int) -> RDAccelerationStructureGeometry:
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = vertex_count
	return geometry


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)


func _memory(rd: RenderingDevice) -> int:
	return rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS)


## The acceleration-structure objects alone (#22). Build scratch lives in per-frame rings that
## persist and only grow, so buffer memory keeps them after the BLAS are freed.
func _as_memory(rd: RenderingDevice) -> int:
	return rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)


func _near_rel(got: float, expected: float) -> bool:
	return absf(got - expected) <= T_REL * maxf(absf(expected), 1e-20)


func _near_abs(got: float, expected: float) -> bool:
	return absf(got - expected) <= BARY_ABS


func _probe(rd: RenderingDevice) -> String:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry_from(vertex_buffer, 3)
	var blas := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)
	_check(blas.is_valid() and rd.blas_build(blas) == OK, "probe BLAS build failed")
	if not blas.is_valid():
		rd.free_rid(vertex_buffer)
		_settle(rd)
		return "probe_failed"
	var original := rd.acceleration_structure_get_size_bytes(blas)
	var err := rd.blas_compact(blas)
	if err == ERR_UNAVAILABLE:
		_expected_errors.append(ERROR_UNSUPPORTED)
		_free_all(rd, [blas, vertex_buffer])
		_settle(rd)
		return "unavailable"
	_check(err == OK, "probe blas_compact returned %d" % err)
	_check(not rd.blas_is_compacted(blas), "probe reported compacted before sync")
	if err == OK:
		_settle(rd)
		var compacted := rd.acceleration_structure_get_size_bytes(blas)
		_check(rd.blas_is_compacted(blas), "probe was not compacted after sync")
		_check(compacted > 0 and compacted <= original, "probe compacted size %d exceeds original %d" % [compacted, original])
		_sizes = { "probe_original": original, "probe_compacted": compacted }
	_free_all(rd, [blas, vertex_buffer])
	_settle(rd)
	# A size miss is recorded, but the compact itself succeeded. Keep going so
	# equivalence and the churn still run.
	if err != OK:
		return "probe_failed"
	return "compacted"


func _check_accounting(rd: RenderingDevice) -> void:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry_from(vertex_buffer, 3)
	_settle(rd)
	var baseline := _memory(rd)
	var blases: Array[RID] = []
	var originals: Array[int] = []
	var sum_original := 0
	for _i in ACCOUNT_COUNT:
		var blas := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)
		blases.append(blas)
		var size := rd.acceleration_structure_get_size_bytes(blas) if blas.is_valid() else 0
		originals.append(size)
		sum_original += size
	var after_create := _memory(rd) - baseline
	_check(after_create == sum_original, "create delta %d != sum of sizes %d" % [after_create, sum_original])
	var built := true
	for blas in blases:
		if not blas.is_valid() or rd.blas_build(blas) != OK:
			built = false
	_check(built, "accounting BLAS build failed")
	_settle(rd)
	var scratch_total := _memory(rd) - baseline - sum_original
	_check(scratch_total >= 0, "scratch total is negative (%d)" % scratch_total)
	var compact_ok := true
	for blas in blases:
		if rd.blas_compact(blas) != OK:
			compact_ok = false
	_check(compact_ok, "accounting blas_compact failed")
	_settle(rd)
	var sum_compacted := 0
	for i in blases.size():
		var compacted: bool = rd.blas_is_compacted(blases[i])
		var size := rd.acceleration_structure_get_size_bytes(blases[i])
		_check(compacted, "accounting BLAS %d was not compacted" % i)
		_check(size > 0 and size <= originals[i], "accounting BLAS %d size %d exceeds %d" % [i, size, originals[i]])
		sum_compacted += size
	_settle(rd)
	var after_compact := _memory(rd) - baseline
	_check(after_compact == sum_compacted + scratch_total, "after compact, memory delta %d != compacted %d + scratch %d" % [after_compact, sum_compacted, scratch_total])
	_free_all(rd, blases)
	_settle(rd)
	var after_free := _memory(rd)
	_check(after_free == baseline, "accounting memory %d did not return to baseline %d" % [after_free, baseline])
	rd.free_rid(vertex_buffer)
	_settle(rd)
	_accounting = {
		"baseline": baseline,
		"after_create": after_create,
		"sum_original": sum_original,
		"scratch_total": scratch_total,
		"sum_compacted": sum_compacted,
		"after_compact": after_compact,
		"after_free": after_free,
	}


func _check_equivalence(rd: RenderingDevice) -> void:
	var vertices := PackedFloat32Array()
	vertices.resize(TRIANGLE_COUNT * 9)
	for i in TRIANGLE_COUNT:
		var base := float(i) * 2.0
		var o := i * 9
		vertices[o] = base
		vertices[o + 1] = 0.0
		vertices[o + 2] = 0.0
		vertices[o + 3] = base + 1.0
		vertices[o + 4] = 0.0
		vertices[o + 5] = 0.0
		vertices[o + 6] = base + 0.5
		vertices[o + 7] = 1.0
		vertices[o + 8] = 0.0
	var vertex_bytes := vertices.to_byte_array()
	var vertex_buffer := rd.vertex_buffer_create(vertex_bytes.size(), vertex_bytes, _input_bits())
	var geometry := _geometry_from(vertex_buffer, TRIANGLE_COUNT * 3)
	var blas := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)
	var built := blas.is_valid() and rd.blas_build(blas) == OK
	var tlas := rd.tlas_create(1, 0)
	var instance := RDAccelerationStructureInstance.new()
	instance.blas = blas
	instance.id = CUSTOM_INDEX
	instance.mask = 0xFF
	instance.hit_sbt_range = 0
	instance.transform = Transform3D(Basis(), Vector3(0, 0, 0))
	built = built and tlas.is_valid() and rd.tlas_build(tlas, [instance]) == OK
	_check(built, "equivalence BLAS or TLAS build failed")

	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = KERNEL
	var spirv := rd.shader_compile_spirv_from_source(source)
	_check(spirv.compile_error_compute.is_empty(), "equivalence kernel failed to compile: " + spirv.compile_error_compute)
	var shader := rd.shader_create_from_spirv(spirv, "blas_compact") if spirv.compile_error_compute.is_empty() else RID()
	var pipeline := rd.compute_pipeline_create(shader) if shader.is_valid() else RID()
	var hits := rd.storage_buffer_create(RAY_COUNT * 32)
	var tlas_uniform := RDUniform.new()
	tlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE
	tlas_uniform.binding = 0
	tlas_uniform.add_id(tlas)
	var hits_uniform := RDUniform.new()
	hits_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	hits_uniform.binding = 1
	hits_uniform.add_id(hits)
	var uniform_set := rd.uniform_set_create([tlas_uniform, hits_uniform], shader, 0) if shader.is_valid() else RID()
	_check(pipeline.is_valid() and uniform_set.is_valid() and hits.is_valid(), "equivalence trace pipeline failed")

	var before := PackedByteArray()
	var after := PackedByteArray()
	if built and pipeline.is_valid() and uniform_set.is_valid():
		_dispatch(rd, pipeline, uniform_set)
		_settle(rd)
		before = rd.buffer_get_data(hits)
		var original := rd.acceleration_structure_get_size_bytes(blas)
		var compact_err := rd.blas_compact(blas)
		_check(compact_err == OK, "equivalence blas_compact returned %d" % compact_err)
		_check(not rd.blas_is_compacted(blas), "equivalence reported compacted before sync")
		if compact_err == OK:
			_settle(rd)
			var compacted_size := rd.acceleration_structure_get_size_bytes(blas)
			_check(rd.blas_is_compacted(blas), "equivalence was not compacted after sync")
			_check(compacted_size > 0 and compacted_size <= original, "equivalence compacted size %d exceeds original %d" % [compacted_size, original])
			_sizes["original"] = original
			_sizes["compacted"] = compacted_size
			# The copy was recorded by the sync above. Rebuild the TLAS and trace in this
			# same frame so the graph orders copy, TLAS build, then the trace.
			var rebuilt := rd.tlas_build(tlas, [instance]) == OK
			_check(rebuilt, "equivalence TLAS rebuild after compaction failed")
			if rebuilt:
				_dispatch(rd, pipeline, uniform_set)
			_settle(rd)
			after = rd.buffer_get_data(hits)
		_compare_traces(before, after)

	_free_all(rd, [uniform_set, hits, pipeline, shader, tlas, blas, vertex_buffer])
	_settle(rd)


func _dispatch(rd: RenderingDevice, pipeline: RID, uniform_set: RID) -> void:
	var push := PackedByteArray()
	push.resize(16)
	push.encode_u32(0, RAY_COUNT)
	var compute_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	rd.compute_list_set_push_constant(compute_list, push, push.size())
	rd.compute_list_dispatch(compute_list, ceili(float(RAY_COUNT) / 64.0), 1, 1)
	rd.compute_list_end()


func _compare_traces(before: PackedByteArray, after: PackedByteArray) -> void:
	var record_bytes := RAY_COUNT * 32
	_check(before.size() >= record_bytes and after.size() >= record_bytes, "trace readback is short (%d, %d)" % [before.size(), after.size()])
	if before.size() < record_bytes or after.size() < record_bytes:
		return
	var id_miss := 0
	var front_miss := 0
	var t_over := 0
	var bary_over := 0
	var bits_same := true
	for i in RAY_COUNT:
		var o := i * 32
		if before.decode_u32(o) != after.decode_u32(o) or before.decode_u32(o + 12) != after.decode_u32(o + 12) or before.decode_u32(o + 16) != after.decode_u32(o + 16):
			bits_same = false
		var custom_index := after.decode_u32(o + 4)
		var primitive := after.decode_u32(o + 8)
		var front := after.decode_u32(o + 20)
		var committed := after.decode_u32(o + 24)
		if custom_index != CUSTOM_INDEX or primitive != i or committed != 1 or before.decode_u32(o + 4) != CUSTOM_INDEX or before.decode_u32(o + 8) != i:
			id_miss += 1
		if front != 1 or before.decode_u32(o + 20) != 1:
			front_miss += 1
		var t := after.decode_float(o)
		var u := after.decode_float(o + 12)
		var v := after.decode_float(o + 16)
		var t0 := before.decode_float(o)
		var u0 := before.decode_float(o + 12)
		var v0 := before.decode_float(o + 16)
		if not _near_rel(t, ANALYTIC_T) or not _near_rel(t, t0) or not _near_rel(t0, ANALYTIC_T):
			t_over += 1
		if not _near_abs(u, ANALYTIC_U) or not _near_abs(v, ANALYTIC_V) or not _near_abs(u, u0) or not _near_abs(v, v0):
			bary_over += 1
	_bit_identical = bits_same
	_check(id_miss == 0, "ids mismatched on %d/%d rays" % [id_miss, RAY_COUNT])
	_check(front_miss == 0, "front face mismatched on %d/%d rays" % [front_miss, RAY_COUNT])
	_check(t_over == 0, "t outside 1e-5 relative on %d/%d rays" % [t_over, RAY_COUNT])
	_check(bary_over == 0, "barycentrics outside 1e-5 absolute on %d/%d rays" % [bary_over, RAY_COUNT])


func _expect(rd: RenderingDevice, blas: RID, message: String) -> void:
	_expected_errors.append(message)
	var err := rd.blas_compact(blas)
	_check(err == ERR_INVALID_PARAMETER, "%s returned %d" % [message, err])


func _check_negatives(rd: RenderingDevice) -> void:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry_from(vertex_buffer, 3)

	var no_flag := rd.blas_create([geometry], 0)
	_expect(rd, no_flag, ERROR_NO_FLAG)
	_check(no_flag.is_valid() and rd.blas_build(no_flag) == OK, "BLAS without ALLOW_COMPACTION could not be built after a rejected compact")

	var unbuilt := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)
	_expect(rd, unbuilt, ERROR_UNBUILT)
	_check(unbuilt.is_valid() and rd.blas_build(unbuilt) == OK, "unbuilt BLAS could not be built after a rejected compact")

	var tlas := rd.tlas_create(1, 0)
	_expect(rd, tlas, ERROR_NOT_BLAS)

	var pending := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)
	var pending_built := pending.is_valid() and rd.blas_build(pending) == OK
	_check(pending_built, "pending-compaction BLAS build failed")
	if pending_built:
		_check(rd.blas_compact(pending) == OK, "first compact of a built BLAS failed")
		_expect(rd, pending, ERROR_PENDING)
		_settle(rd)
		_check(rd.blas_is_compacted(pending), "BLAS was not compacted after the pending query")
		_expect(rd, pending, ERROR_COMPACTED)
		_expected_errors.append(ERROR_REBUILD)
		var rebuild := rd.blas_build(pending)
		_check(rebuild == ERR_INVALID_PARAMETER, "blas_build on a compacted BLAS returned %d" % rebuild)

	_free_all(rd, [no_flag, unbuilt, tlas, pending, vertex_buffer])
	_settle(rd)


func _check_churn(rd: RenderingDevice) -> void:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry_from(vertex_buffer, 3)
	_settle(rd)
	var baseline := _as_memory(rd)
	var scratch_after_first := -1
	var started := Time.get_ticks_msec()
	var built := 0
	var not_compacted := 0
	var oversized := 0
	var failed := false
	while built < CHURN_COUNT and not failed:
		var count := mini(CHURN_BATCH, CHURN_COUNT - built)
		var batch: Array[RID] = []
		var originals: Array[int] = []
		for _i in count:
			var blas := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT)
			if not blas.is_valid() or rd.blas_build(blas) != OK or rd.blas_compact(blas) != OK:
				failed = true
				if blas.is_valid():
					batch.append(blas)
				break
			originals.append(rd.acceleration_structure_get_size_bytes(blas))
			batch.append(blas)
		if not failed:
			_settle(rd)
			for i in batch.size():
				if not rd.blas_is_compacted(batch[i]):
					not_compacted += 1
					failed = true
				var compacted := rd.acceleration_structure_get_size_bytes(batch[i])
				if compacted <= 0 or compacted > originals[i]:
					oversized += 1
					failed = true
		_free_all(rd, batch)
		_settle(rd)
		var now := _as_memory(rd)
		if now != baseline:
			failed = true
			_check(false, "churn acceleration-structure memory %d != baseline %d at %d" % [now, baseline, built + batch.size()])
		# The scratch rings may grow during the first batch and must not grow after it.
		var scratch := _memory(rd) - now
		if scratch_after_first < 0:
			scratch_after_first = scratch
		elif scratch != scratch_after_first:
			failed = true
			_check(false, "churn scratch %d != %d after the first batch, at %d" % [scratch, scratch_after_first, built + batch.size()])
		built += batch.size()
		if built % 2500 == 0 or built >= CHURN_COUNT or failed:
			print("blas_compact churn %d/%d" % [built, CHURN_COUNT])
	_churn_ms = Time.get_ticks_msec() - started
	_check(not failed, "10k BLAS churn failed at %d" % built)
	_check(built == CHURN_COUNT, "10k BLAS churn built %d" % built)
	_check(not_compacted == 0, "churn left %d BLASes uncompacted" % not_compacted)
	_check(oversized == 0, "churn compacted size exceeded the original on %d BLASes" % oversized)
	var after := _as_memory(rd)
	_check(after == baseline, "10k BLAS churn left acceleration-structure memory at %d (baseline %d)" % [after, baseline])
	rd.free_rid(vertex_buffer)
	_settle(rd)
