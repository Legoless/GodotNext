extends Node

## Metal acceleration-structure checks (#47): instance limit, 1000 traced instances,
## a BLAS-less miss, allocation accounting, and a 10k create/build/free churn.
##
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/metal_as.tscn

const INSTANCE_COUNT := 1000
const CHURN_COUNT := 10000
const CHURN_BATCH := 250
const LINEAR_BATCH := 32

const KERNEL := """#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Hits {
	uint data[];
}
hits;

layout(push_constant, std430) uniform Params {
	uint count;
	float empty_x;
	uint pad0;
	uint pad1;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i > params.count) {
		return;
	}
	float x = i < params.count ? float(i) * 2.0 + 0.25 : params.empty_x;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, vec3(x, 0.25, 1.0), 0.0, vec3(0.0, 0.0, -1.0), 10.0);
	rayQueryProceedEXT(rq);
	uint id = 0u;
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		id = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
	}
	hits.data[i] = id;
}
"""

var _failures := PackedStringArray()
var _expected_errors := PackedStringArray()


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "pass": false, "reason": "NO_RENDERING_DEVICE" }

	var ray_query := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY)
	var ray_query_hw := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
	_check(ray_query, "SUPPORTS_RAY_QUERY")
	_check(ray_query_hw, "SUPPORTS_RAY_QUERY_HARDWARE")
	if ray_query:
		_check_limit(rd)
		_check_accounting(rd)
		_check_instances(rd)
		_check_churn(rd)
	rd.free()
	return {
		"pass": _failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"ray_query": ray_query,
		"ray_query_hardware": ray_query_hw,
		"failures": _failures,
		"expected_errors": _expected_errors,
	}


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func _settle(rd: RenderingDevice) -> void:
	rd.submit()
	rd.sync()


func _triangle_buffer(rd: RenderingDevice) -> RID:
	var vertices := PackedFloat32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]).to_byte_array()
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	return rd.vertex_buffer_create(vertices.size(), vertices, input_bits)


func _geometry(vertex_buffer: RID) -> RDAccelerationStructureGeometry:
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = 3
	return geometry


func _check_limit(rd: RenderingDevice) -> void:
	var limit := rd.limit_get(RenderingDevice.LIMIT_MAX_ACCELERATION_STRUCTURE_INSTANCES)
	_check(limit >= 100000, "instance limit %d is below 100000" % limit)
	var tlas := rd.tlas_create(100000, 0)
	_check(tlas.is_valid(), "tlas_create(100000) failed")
	if tlas.is_valid():
		rd.free_rid(tlas)
	_expected_errors.append("Acceleration structure instance count exceeds the device limit.")
	_expected_errors.append("Failed to create TLAS.")
	var too_many := rd.tlas_create(limit + 1, 0)
	_check(not too_many.is_valid(), "tlas_create above the instance limit returned a RID")
	_settle(rd)


func _check_accounting(rd: RenderingDevice) -> void:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry(vertex_buffer)
	_settle(rd)
	var baseline := rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS)
	var first: Array[RID] = []
	for _i in LINEAR_BATCH:
		first.append(rd.blas_create([geometry], 0))
	var half := rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS) - baseline
	var second: Array[RID] = []
	for _i in LINEAR_BATCH:
		second.append(rd.blas_create([geometry], 0))
	var full := rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS) - baseline
	_check(half > 0, "creating a BLAS did not increase buffer memory")
	if half > 0:
		var drift := absi(full - half * 2)
		_check(drift * 100 <= half, "AS bytes of two equal batches differ by %d (half %d)" % [drift, half])
	for blas in first:
		rd.free_rid(blas)
	for blas in second:
		rd.free_rid(blas)
	_settle(rd)
	var after := rd.get_memory_usage(RenderingDevice.MEMORY_BUFFERS)
	_check(after == baseline, "BLAS memory %d did not return to baseline %d" % [after, baseline])
	rd.free_rid(vertex_buffer)
	_settle(rd)


func _check_instances(rd: RenderingDevice) -> void:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry(vertex_buffer)
	var blases: Array[RID] = []
	var built := true
	for _i in INSTANCE_COUNT:
		var blas := rd.blas_create([geometry], 0)
		if not blas.is_valid() or rd.blas_build(blas) != OK:
			built = false
			break
		blases.append(blas)
	_check(built and blases.size() == INSTANCE_COUNT, "failed to build %d BLASes" % INSTANCE_COUNT)
	if not built:
		_free_all(rd, blases)
		rd.free_rid(vertex_buffer)
		_settle(rd)
		return

	var tlas := rd.tlas_create(INSTANCE_COUNT + 1, 0)
	var instances: Array[RDAccelerationStructureInstance] = []
	for i in INSTANCE_COUNT:
		var instance := RDAccelerationStructureInstance.new()
		instance.blas = blases[i]
		instance.id = i + 1
		instance.transform = Transform3D(Basis(), Vector3(i * 2.0, 0, 0))
		instances.append(instance)
	var empty := RDAccelerationStructureInstance.new()
	empty.id = 999
	empty.mask = 0xFF
	empty.transform = Transform3D(Basis(), Vector3(-4, 0, 0))
	instances.append(empty)
	_check(tlas.is_valid() and rd.tlas_build(tlas, instances) == OK, "1000-instance TLAS build failed")

	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = KERNEL
	var spirv := rd.shader_compile_spirv_from_source(source)
	_check(spirv.compile_error_compute.is_empty(), "instance kernel failed to compile: " + spirv.compile_error_compute)
	var shader := rd.shader_create_from_spirv(spirv, "metal_as")
	var pipeline := rd.compute_pipeline_create(shader) if shader.is_valid() else RID()
	var ray_count := INSTANCE_COUNT + 1
	var hits := rd.storage_buffer_create(ray_count * 4)
	var tlas_uniform := RDUniform.new()
	tlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE
	tlas_uniform.binding = 0
	tlas_uniform.add_id(tlas)
	var hits_uniform := RDUniform.new()
	hits_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	hits_uniform.binding = 1
	hits_uniform.add_id(hits)
	var uniform_set := rd.uniform_set_create([tlas_uniform, hits_uniform], shader, 0) if shader.is_valid() else RID()
	_check(pipeline.is_valid() and uniform_set.is_valid(), "instance trace pipeline failed")
	if pipeline.is_valid() and uniform_set.is_valid():
		var push := PackedByteArray()
		push.resize(16)
		push.encode_u32(0, INSTANCE_COUNT)
		push.encode_float(4, -3.75)
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
		rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		rd.compute_list_set_push_constant(compute_list, push, push.size())
		rd.compute_list_dispatch(compute_list, ceili(ray_count / 64.0), 1, 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
		var data := rd.buffer_get_data(hits)
		var missed := 0
		var wrong := 0
		for i in INSTANCE_COUNT:
			if data.decode_u32(i * 4) != i + 1:
				if data.decode_u32(i * 4) == 0:
					missed += 1
				else:
					wrong += 1
		var empty_hit := data.decode_u32(INSTANCE_COUNT * 4)
		_check(missed == 0 and wrong == 0, "1000-instance trace missed %d and mismatched %d" % [missed, wrong])
		_check(empty_hit == 0, "BLAS-less instance was hit (id %d)" % empty_hit)
	_free_all(rd, [uniform_set, hits, pipeline, shader, tlas])
	_free_all(rd, blases)
	rd.free_rid(vertex_buffer)
	_settle(rd)


func _check_churn(rd: RenderingDevice) -> void:
	var vertex_buffer := _triangle_buffer(rd)
	var geometry := _geometry(vertex_buffer)
	_settle(rd)
	# Acceleration-structure objects only (#22): build scratch lives in per-frame rings that persist.
	var baseline := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
	var built := 0
	while built < CHURN_COUNT:
		var batch: Array[RID] = []
		var count := mini(CHURN_BATCH, CHURN_COUNT - built)
		var ok := true
		for _i in count:
			var blas := rd.blas_create([geometry], 0)
			if not blas.is_valid() or rd.blas_build(blas) != OK:
				ok = false
				if blas.is_valid():
					batch.append(blas)
				break
			batch.append(blas)
		for blas in batch:
			rd.free_rid(blas)
		_settle(rd)
		built += count
		if not ok:
			_check(false, "10k BLAS churn failed at %d" % built)
			break
	var after := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
	_check(after == baseline, "10k BLAS churn left acceleration-structure memory at %d (baseline %d)" % [after, baseline])
	rd.free_rid(vertex_buffer)
	_settle(rd)


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)
