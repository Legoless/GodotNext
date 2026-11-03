extends Node

## RenderingDevice ray-query-only API fixture (#17; plan D5 and §9 RT API items 1, 4 and 5).
##
## Run it on a GPU, with a window:
##   godot --path godotnext/testbed --rendering-driver <driver> res://tests/rd_api/rd_api.tscn
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.
##
## - A device without ray query (D3D12 today) asserts the fallback errors: the acceleration
##   structure and RT pipeline entry points return invalid RIDs. Metal with acceleration
##   structures takes the ray-query branch.
## - A device with ray query builds two BLASes of the same quad, one from storage buffers and one
##   from vertex and index buffers, puts them in a 2-instance TLAS at hit_sbt_range 0 and traces
##   that TLAS from a ray-query compute kernel. Both instances must return the same hits, and those
##   hits must match the analytic ones.
## - A device without RT pipelines asserts that raytracing_pipeline_create, hit_sbt_create and
##   raytracing_list_begin fail cleanly.
##
## Some steps print engine errors on purpose; "expected_errors" in the result lists them.

const GRID := 8 # Rays per axis per instance.
const INSTANCE_IDS: Array[int] = [10, 20]
const INSTANCE_OFFSET_X: Array[float] = [-2.0, 2.0]
const T_TOLERANCE := 1e-4
const BARYCENTRIC_TOLERANCE := 1e-4

# D5 kernel rules: 8x8 workgroup, no shared memory, opaque traversal with one rayQueryProceedEXT,
# no position fetch and no SBT-offset getter.
const KERNEL := """#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Hits {
	vec4 data[];
}
hits;

layout(push_constant, std430) uniform Params {
	uint grid;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uvec2 id = gl_GlobalInvocationID.xy;
	if (id.x >= params.grid * 2u || id.y >= params.grid) {
		return;
	}
	uint local_x = id.x % params.grid;
	float offset_x = id.x < params.grid ? -2.0 : 2.0;
	vec2 local_pos = vec2((float(local_x) + 0.25) / float(params.grid), (float(id.y) + 0.75) / float(params.grid)) * 2.0 - 1.0;
	vec3 origin = vec3(local_pos.x + offset_x, local_pos.y, 1.0);

	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.0, vec3(0.0, 0.0, -1.0), 10.0);
	rayQueryProceedEXT(rq);

	vec4 a = vec4(-1.0, -1.0, -1.0, 0.0);
	vec4 b = vec4(0.0);
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		a = vec4(rayQueryGetIntersectionTEXT(rq, true), float(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)), float(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), 1.0);
		b = vec4(rayQueryGetIntersectionBarycentricsEXT(rq, true), rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 1.0 : 0.0, 0.0);
	}
	uint ray = id.y * params.grid * 2u + id.x;
	hits.data[ray * 2u] = a;
	hits.data[ray * 2u + 1u] = b;
}
"""

var _checks := 0
var _failures := PackedStringArray()
var _expected_errors := PackedStringArray()


func _ready() -> void:
	var result := run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func run() -> Dictionary:
	_check_bindings()

	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "pass": false, "reason": "NO_RENDERING_DEVICE" }

	var ray_query := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY)
	var raytracing_pipeline := rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE)
	if ray_query:
		_run_ray_query(rd)
	else:
		_run_without_ray_query(rd, raytracing_pipeline)
	if not raytracing_pipeline:
		_run_without_raytracing_pipeline(rd, ray_query)
	rd.free()

	return {
		"pass": _failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"ray_query": ray_query,
		"raytracing_pipeline": raytracing_pipeline,
		"checks": _checks,
		"failures": _failures,
		"expected_errors": _expected_errors,
	}


func _check(condition: bool, what: String) -> void:
	_checks += 1
	if not condition:
		_failures.append(what)


func _check_bindings() -> void:
	var geometry := RDAccelerationStructureGeometry.new()
	_check(geometry.index_format == RenderingDevice.INDEX_BUFFER_FORMAT_UINT32, "RDAccelerationStructureGeometry.index_format defaults to INDEX_BUFFER_FORMAT_UINT32")
	geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT16
	_check(geometry.index_format == RenderingDevice.INDEX_BUFFER_FORMAT_UINT16, "RDAccelerationStructureGeometry.index_format can be set")
	_check(RDAccelerationStructureInstance.new().hit_sbt_range == 0, "RDAccelerationStructureInstance.hit_sbt_range defaults to 0")


func _quad_vertices() -> PackedByteArray:
	return PackedFloat32Array([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0]).to_byte_array()


func _quad_indices() -> PackedByteArray:
	return PackedInt32Array([0, 1, 2, 0, 2, 3]).to_byte_array()


func _geometry(vertex_buffer: RID, index_buffer: RID) -> RDAccelerationStructureGeometry:
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = 4
	geometry.index_buffer = index_buffer
	geometry.index_count = 6
	geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32
	return geometry


func _run_without_ray_query(rd: RenderingDevice, raytracing_pipeline: bool) -> void:
	_check(not raytracing_pipeline, "a device without ray query also lacks RT pipelines (no such device is known)")
	var vertices := _quad_vertices()
	var indices := _quad_indices()
	var vertex_buffer := rd.storage_buffer_create(vertices.size(), vertices)
	var index_buffer := rd.storage_buffer_create(indices.size(), indices)
	_check(vertex_buffer.is_valid() and index_buffer.is_valid(), "plain storage buffers are created")

	_expected_errors.append("blas_create: neither raytracing pipeline nor ray query support")
	var blas := rd.blas_create([_geometry(vertex_buffer, index_buffer)], 0)
	_check(not blas.is_valid(), "blas_create returns an invalid RID without ray tracing")

	_expected_errors.append("tlas_create: neither raytracing pipeline nor ray query support")
	var tlas := rd.tlas_create(2, 0)
	_check(not tlas.is_valid(), "tlas_create returns an invalid RID without ray tracing")

	rd.free_rid(index_buffer)
	rd.free_rid(vertex_buffer)


func _run_without_raytracing_pipeline(rd: RenderingDevice, ray_query: bool) -> void:
	var reason := "no raytracing pipeline support" if ray_query else "neither raytracing pipeline nor ray query support"

	_expected_errors.append("raytracing_pipeline_create: " + reason)
	var pipeline := rd.raytracing_pipeline_create([], [], [], 1)
	_check(not pipeline.is_valid(), "raytracing_pipeline_create returns an invalid RID without SUPPORTS_RAYTRACING_PIPELINE")

	_expected_errors.append("hit_sbt_create: " + reason)
	var hit_sbt := rd.hit_sbt_create(RID(), 1)
	_check(not hit_sbt.is_valid(), "hit_sbt_create returns an invalid RID without SUPPORTS_RAYTRACING_PIPELINE")

	_expected_errors.append("raytracing_list_begin: no raytracing pipeline support")
	var raytracing_list := rd.raytracing_list_begin()
	_check(raytracing_list == RenderingDevice.INVALID_ID, "raytracing_list_begin returns INVALID_ID without SUPPORTS_RAYTRACING_PIPELINE")


func _run_ray_query(rd: RenderingDevice) -> void:
	var vertices := _quad_vertices()
	var indices := _quad_indices()
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var freed: Array[RID] = []

	# The same quad from storage buffers and from vertex and index buffers.
	var storage_vertices := rd.storage_buffer_create(vertices.size(), vertices, 0, input_bits)
	var storage_indices := rd.storage_buffer_create(indices.size(), indices, 0, input_bits)
	var vertex_buffer := rd.vertex_buffer_create(vertices.size(), vertices, input_bits)
	var index_buffer := rd.index_buffer_create(6, RenderingDevice.INDEX_BUFFER_FORMAT_UINT32, indices, false, input_bits)
	_check(storage_vertices.is_valid() and storage_indices.is_valid() and vertex_buffer.is_valid() and index_buffer.is_valid(), "build input buffers are created")

	var blas_storage := rd.blas_create([_geometry(storage_vertices, storage_indices)], 0)
	_check(blas_storage.is_valid(), "blas_create accepts storage-buffer vertices and indices")
	var blas_vertex := rd.blas_create([_geometry(vertex_buffer, index_buffer)], 0)
	_check(blas_vertex.is_valid(), "blas_create accepts vertex and index buffers")

	# Storage buffers without the build input bits are rejected without a crash.
	var plain_storage := rd.storage_buffer_create(vertices.size(), vertices)
	var address_only := rd.storage_buffer_create(vertices.size(), vertices, 0, RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT)
	_expected_errors.append("blas_create: vertex buffer of geometry 0 lacks the build input creation bits (plain storage buffer)")
	_check(not rd.blas_create([_geometry(plain_storage, storage_indices)], 0).is_valid(), "blas_create rejects a storage buffer without the build input bits")
	_expected_errors.append("blas_create: index buffer of geometry 0 lacks the build input creation bits (device address only)")
	_check(not rd.blas_create([_geometry(storage_vertices, address_only)], 0).is_valid(), "blas_create rejects a storage buffer with only BUFFER_CREATION_DEVICE_ADDRESS_BIT")

	_check(rd.blas_build(blas_storage) == OK, "blas_build of the storage-buffer BLAS")
	_check(rd.blas_build(blas_vertex) == OK, "blas_build of the vertex-buffer BLAS")

	# A 2-instance TLAS at hit_sbt_range 0: ray-query only.
	var tlas := rd.tlas_create(2, 0)
	var instances: Array[RDAccelerationStructureInstance] = []
	var blases: Array[RID] = [blas_storage, blas_vertex]
	for i in 2:
		var instance := RDAccelerationStructureInstance.new()
		instance.blas = blases[i]
		instance.id = INSTANCE_IDS[i]
		instance.transform = Transform3D(Basis(), Vector3(INSTANCE_OFFSET_X[i], 0, 0))
		instance.hit_sbt_range = 0
		instances.append(instance)
	_check(rd.tlas_build(tlas, instances) == OK, "tlas_build accepts 2 instances at hit_sbt_range 0")

	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = KERNEL
	var spirv := rd.shader_compile_spirv_from_source(source)
	_check(spirv.compile_error_compute.is_empty(), "the ray-query kernel compiles: " + spirv.compile_error_compute)
	var shader := rd.shader_create_from_spirv(spirv)
	_check(shader.is_valid(), "the ray-query shader is created")
	if not shader.is_valid():
		freed.append_array([tlas, blas_vertex, blas_storage, address_only, plain_storage, index_buffer, vertex_buffer, storage_indices, storage_vertices])
		_free_all(rd, freed)
		return
	var pipeline := rd.compute_pipeline_create(shader)

	var ray_count := 2 * GRID * GRID
	var hits := rd.storage_buffer_create(ray_count * 32)
	var uniform_set := rd.uniform_set_create(_uniforms(tlas, hits), shader, 0)
	_check(uniform_set.is_valid(), "a uniform set holds the zero-SBT TLAS")

	# An unbuilt TLAS: binding it to a compute list prints the invalidated error in debug builds.
	var unbuilt_tlas := rd.tlas_create(1, 0)
	var unbuilt_set := rd.uniform_set_create(_uniforms(unbuilt_tlas, hits), shader, 0)

	var push_constant := PackedInt32Array([GRID, 0, 0, 0]).to_byte_array()
	var compute_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	rd.compute_list_set_push_constant(compute_list, push_constant, push_constant.size())
	rd.compute_list_dispatch(compute_list, ceili(2 * GRID / 8.0), ceili(GRID / 8.0), 1)
	rd.compute_list_end()

	_expected_errors.append("compute_list_bind_uniform_set: acceleration structure has not been built yet (debug builds)")
	compute_list = rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, unbuilt_set, 0)
	rd.compute_list_end()

	rd.submit()
	rd.sync()

	_check_hits(rd.buffer_get_data(hits).to_float32_array())

	freed.append_array([unbuilt_set, unbuilt_tlas, uniform_set, hits, pipeline, shader, tlas, blas_vertex, blas_storage, address_only, plain_storage, index_buffer, vertex_buffer, storage_indices, storage_vertices])
	_free_all(rd, freed)


func _uniforms(tlas: RID, hits: RID) -> Array[RDUniform]:
	var tlas_uniform := RDUniform.new()
	tlas_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE
	tlas_uniform.binding = 0
	tlas_uniform.add_id(tlas)
	var hits_uniform := RDUniform.new()
	hits_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	hits_uniform.binding = 1
	hits_uniform.add_id(hits)
	return [tlas_uniform, hits_uniform]


func _free_all(rd: RenderingDevice, rids: Array[RID]) -> void:
	for rid in rids:
		if rid.is_valid():
			rd.free_rid(rid)


# The analytic hit for a ray down -Z at local (x, y) of the quad: triangle 0 (vertices 0, 1, 2)
# covers y <= x and triangle 1 (vertices 0, 2, 3) covers y >= x. The grid never lands on the diagonal.
func _analytic_hit(x: float, y: float) -> Vector3:
	if y < x:
		return Vector3(0, (x - y) * 0.5, (y + 1.0) * 0.5)
	return Vector3(1, (x + 1.0) * 0.5, (y - x) * 0.5)


func _check_hits(data: PackedFloat32Array) -> void:
	var ray_count := 2 * GRID * GRID
	_check(data.size() == ray_count * 8, "the hit buffer holds 8 floats per ray")
	if data.size() != ray_count * 8:
		return
	var missed := 0
	var wrong_id := 0
	var wrong_analytic := 0
	var different := 0
	for y in GRID:
		for x in GRID:
			var local := Vector2((x + 0.25) / GRID, (y + 0.75) / GRID) * 2.0 - Vector2.ONE
			var expected := _analytic_hit(local.x, local.y)
			var records: Array[PackedFloat32Array] = []
			for i in 2:
				var ray := y * GRID * 2 + i * GRID + x
				var record := data.slice(ray * 8, ray * 8 + 8)
				records.append(record)
				if record[3] != 1.0:
					missed += 1
					continue
				if int(record[1]) != INSTANCE_IDS[i]:
					wrong_id += 1
				if absf(record[0] - 1.0) > T_TOLERANCE or int(record[2]) != int(expected.x) or absf(record[4] - expected.y) > BARYCENTRIC_TOLERANCE or absf(record[5] - expected.z) > BARYCENTRIC_TOLERANCE:
					wrong_analytic += 1
			# BLAS hit identity: the storage-buffer BLAS and the vertex-buffer BLAS return the same hit.
			var a := records[0]
			var b := records[1]
			if a[3] != b[3] or absf(a[0] - b[0]) > T_TOLERANCE or a[2] != b[2] or absf(a[4] - b[4]) > BARYCENTRIC_TOLERANCE or absf(a[5] - b[5]) > BARYCENTRIC_TOLERANCE or a[6] != b[6]:
				different += 1
	_check(missed == 0, "every ray hits its instance (%d missed)" % missed)
	_check(wrong_id == 0, "every hit reports its instance custom index (%d wrong)" % wrong_id)
	_check(wrong_analytic == 0, "every hit matches the analytic t, primitive and barycentrics (%d wrong)" % wrong_analytic)
	_check(different == 0, "the storage-buffer and vertex-buffer BLASes return identical hits (%d differ)" % different)
