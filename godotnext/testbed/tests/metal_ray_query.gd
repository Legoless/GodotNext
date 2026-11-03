extends Node

## Builds a one-triangle TLAS and traces it with a ray-query compute kernel (#48, #47).
##
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/metal_ray_query.tscn

const KERNEL := """#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Hits {
	float data[];
}
hits;

void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, vec3(0.25, 0.25, 1.0), 0.0, vec3(0.0, 0.0, -1.0), 10.0);
	rayQueryProceedEXT(rq);
	vec4 hit = vec4(0.0);
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		hit = vec4(rayQueryGetIntersectionTEXT(rq, true), float(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)), 1.0, 0.0);
	}
	hits.data[0] = hit.x;
	hits.data[1] = hit.y;
	hits.data[2] = hit.z;
	hits.data[3] = hit.w;
}
"""


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "pass": false, "reason": "NO_RENDERING_DEVICE" }

	var failures := PackedStringArray()
	var ray_query := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY)
	var ray_query_hw := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY_HARDWARE)
	if not ray_query:
		failures.append("SUPPORTS_RAY_QUERY is false")
	if not ray_query_hw:
		failures.append("SUPPORTS_RAY_QUERY_HARDWARE is false")
	if rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE):
		failures.append("SUPPORTS_RAYTRACING_PIPELINE is true")

	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = KERNEL
	var spirv := rd.shader_compile_spirv_from_source(source)
	if not spirv.compile_error_compute.is_empty():
		failures.append("compile: " + spirv.compile_error_compute)
	var shader := rd.shader_create_from_spirv(spirv, "metal_ray_query")
	if not shader.is_valid():
		failures.append("shader_create_from_spirv failed")
	elif ray_query:
		var pipeline := rd.compute_pipeline_create(shader)
		if not pipeline.is_valid():
			failures.append("compute_pipeline_create failed")
		else:
			_trace(rd, shader, pipeline, failures)
			rd.free_rid(pipeline)
		rd.free_rid(shader)
	elif shader.is_valid():
		rd.free_rid(shader)
	rd.free()

	return {
		"pass": failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"ray_query": ray_query,
		"ray_query_hardware": ray_query_hw,
		"failures": failures,
	}


func _trace(rd: RenderingDevice, shader: RID, pipeline: RID, failures: PackedStringArray) -> void:
	var vertices := PackedFloat32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]).to_byte_array()
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_buffer := rd.vertex_buffer_create(vertices.size(), vertices, input_bits)
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = 3
	var blas := rd.blas_create([geometry], 0)
	if not blas.is_valid() or rd.blas_build(blas) != OK:
		failures.append("BLAS build failed")
		_free(rd, [blas, vertex_buffer])
		return

	var tlas := rd.tlas_create(1, 0)
	var instance := RDAccelerationStructureInstance.new()
	instance.blas = blas
	instance.id = 7
	if not tlas.is_valid() or rd.tlas_build(tlas, [instance]) != OK:
		failures.append("TLAS build failed")
		_free(rd, [tlas, blas, vertex_buffer])
		return

	var hits := rd.storage_buffer_create(16)
	var tlas_uniform := RDUniform.new()
	tlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE
	tlas_uniform.binding = 0
	tlas_uniform.add_id(tlas)
	var hits_uniform := RDUniform.new()
	hits_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	hits_uniform.binding = 1
	hits_uniform.add_id(hits)
	var uniform_set := rd.uniform_set_create([tlas_uniform, hits_uniform], shader, 0)
	if not uniform_set.is_valid():
		failures.append("uniform set failed")
		_free(rd, [hits, tlas, blas, vertex_buffer])
		return

	var compute_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	rd.compute_list_dispatch(compute_list, 1, 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()

	var data := rd.buffer_get_data(hits).to_float32_array()
	if data.size() < 4 or data[2] != 1.0 or absf(data[0] - 1.0) > 1e-4 or int(data[1]) != 7:
		failures.append("ray missed the triangle or reported the wrong hit: %s" % data)
	_free(rd, [uniform_set, hits, tlas, blas, vertex_buffer])


func _free(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)
