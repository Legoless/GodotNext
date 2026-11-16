extends Node

## RT pipeline records, push constants and indirect traces without a bindless table (#121; plan §21.1 GD-4).
##
## Run it on a GPU, with a window:
##   godot --path godotnext/testbed --rendering-driver d3d12 res://tests/rd_parity/rt_pipeline_records.tscn
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.
##
## Unlike rt_pipeline.tscn it needs no SUPPORTS_BINDLESS, so it also runs where the Direct3D 12 driver uses legacy
## resource barriers (GODOT_D3D12_LEGACY_BARRIERS=1), which have no bindless tables. Four quads, each an instance with a
## hit group of its own and half of its triangles wound the other way, sit in front of 64 x 64 rays. Every result must
## equal an inline ray-query reference bit for bit: the primitive, t, the value the closest-hit shader read and the hit
## kind. The variants:
## - "record_8": hit groups with 8-byte records (on Direct3D 12, a local root signature of 16 bytes of root constants);
## - "record_32": 32-byte records whose value sits in the last 8 bytes (on Direct3D 12, 32 bytes of root constants);
## - "push_constant": the closest-hit shader adds a push constant to its record's value. The ray generation shader,
##   whose layout the pipeline takes, does not read the push constants;
## - "indirect": INDIRECT_TRACES indirect traces of the record_8 pipeline in one raytracing list, each into a results
##   buffer of its own, in each of INDIRECT_LISTS submits: more than the 32 indirect traces one Direct3D 12 argument
##   buffer holds, so a list needs two, and the next recording reuses them;
## - "no_record" (Direct3D 12 only): hit groups without records, whose closest-hit shader reads one. Direct3D 12 gives
##   every hit group room for the record its shaders read, filled with zeros, so the value is 0. Vulkan leaves such a
##   read undefined.
## A device without SUPPORTS_RAYTRACING_PIPELINE, SUPPORTS_RAY_QUERY (the reference) or
## SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT fails with reason NO_RAYTRACING_PIPELINE, NO_RAY_QUERY or NO_INDIRECT.

const RTPipeline = preload("res://tests/rd_parity/rt_pipeline.gd")

const SIZE := 64
const GROUP_SIZE := 8
const INSTANCES := 4
const MISS := 0xffffffff
# The record value of instance i is RECORD_BASE + RECORD_STEP * i.
const RECORD_BASE := 0x1000
const RECORD_STEP := 0x111
# What the push_constant variant's closest-hit shader adds.
const PUSH_ADD := 0x50000
# Fills the first 24 bytes of a 32-byte record, which the shader does not read.
const RECORD_FILL := 0x5eed0000
const INDIRECT_TRACES := 40
const INDIRECT_LISTS := 2

const COMMON := """#version 460
#extension GL_EXT_ray_tracing : require
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Results {
	uvec4 data[];
}
results;
layout(push_constant, std430) uniform Params {
	uint add;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;
"""

const RAYGEN := """
layout(location = 0) rayPayloadEXT uvec4 payload;

void main() {
	uint index = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
	vec2 uv = (vec2(gl_LaunchIDEXT.xy) + 0.5) / vec2(gl_LaunchSizeEXT.xy) * 2.0 - 1.0;
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 1, 0, vec3(0.0), 0.001, normalize(vec3(uv * 1.5, -1.0)), 100.0, 0);
	results.data[index] = payload;
}
"""

const MISS_SHADER := """
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(0xFFFFFFFFu);
}
"""

# Not readonly: glslang then decorates the record NonWritable, which spirv-val rejects for the shader record
# storage class.
const RECORD_8 := """
layout(shaderRecordEXT, std430) buffer HitRecord {
	uint value;
	uint pad;
}
record;
"""

const RECORD_32 := """
layout(shaderRecordEXT, std430) buffer HitRecord {
	uint fill[6];
	uint value;
	uint pad;
}
record;
"""

const CLOSEST_HIT := """
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(uint(gl_PrimitiveID), floatBitsToUint(gl_HitTEXT), record.value, gl_HitKindEXT);
}
"""

const CLOSEST_HIT_PUSH_CONSTANT := """
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(uint(gl_PrimitiveID), floatBitsToUint(gl_HitTEXT), record.value + params.add, gl_HitKindEXT);
}
"""

const REFERENCE := """#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Results {
	uvec4 data[];
}
results;
layout(push_constant, std430) uniform Params {
	uint add;
	uint records;
	uint pad0;
	uint pad1;
}
params;

void main() {
	uvec2 id = gl_GlobalInvocationID.xy;
	uint index = id.y * %du + id.x;
	vec2 uv = (vec2(id) + 0.5) / vec2(%d.0) * 2.0 - 1.0;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, vec3(0.0), 0.001, normalize(vec3(uv * 1.5, -1.0)), 100.0);
	while (rayQueryProceedEXT(rq)) {
	}
	uvec4 hit = uvec4(0xFFFFFFFFu);
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		uint instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
		uint value = params.records != 0u ? %du + %du * instance + params.add : 0u;
		hit = uvec4(uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), floatBitsToUint(rayQueryGetIntersectionTEXT(rq, true)), value, rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 0xFEu : 0xFFu);
	}
	results.data[index] = hit;
}
"""

# rt_pipeline.gd's helpers, on an instance that is never added to the tree.
var _base: RTPipeline = RTPipeline.new()
# The hit shader binding table of each pipeline _pipeline() made.
var _sbt_of := {}


func _ready() -> void:
	var result: Dictionary = _run()
	_base.free()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var result := {
		"test": "rt_pipeline_records",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"params": { "size": SIZE, "instances": INSTANCES, "indirect_traces": INDIRECT_TRACES, "indirect_lists": INDIRECT_LISTS },
		"legacy_barriers_env": OS.get_environment("GODOT_D3D12_LEGACY_BARRIERS"),
	}
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result
	_base._rd = rd
	result["adapter"] = rd.get_device_name()
	result["supported"] = {
		"raytracing_pipeline": rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE),
		"ray_query": rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY),
		"indirect": rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT),
		"bindless": rd.has_feature(RenderingDevice.SUPPORTS_BINDLESS),
	}
	var needs := [["raytracing_pipeline", "NO_RAYTRACING_PIPELINE"], ["ray_query", "NO_RAY_QUERY"], ["indirect", "NO_INDIRECT"]]
	for need in needs:
		if not result["supported"][need[0]] and not result.has("reason"):
			result["reason"] = need[1]
			_base._check(false, "the device reports " + need[0])
	if not result.has("reason"):
		_run_variants(rd, result)

	for i in range(_base._freed.size() - 1, -1, -1):
		if _base._freed[i].is_valid():
			rd.free_rid(_base._freed[i])
	rd.free()
	result["pass"] = _base._failures.is_empty()
	result["failures"] = _base._failures
	return result


func _run_variants(rd: RenderingDevice, result: Dictionary) -> void:
	var tlas := _build_scene(rd)
	if not tlas.is_valid():
		return
	var raygen := _base._create_shader(rd, { "raygen": COMMON + RAYGEN, "miss": COMMON + MISS_SHADER }, "rt records raygen")
	var hit_8 := _base._create_shader(rd, { "closest_hit": COMMON + RECORD_8 + CLOSEST_HIT }, "rt records hit 8")
	var hit_32 := _base._create_shader(rd, { "closest_hit": COMMON + RECORD_32 + CLOSEST_HIT }, "rt records hit 32")
	var hit_push := _base._create_shader(rd, { "closest_hit": COMMON + RECORD_8 + CLOSEST_HIT_PUSH_CONSTANT }, "rt records hit push constant")
	var reference := _base._create_shader(rd, { "compute": REFERENCE % [SIZE, SIZE, RECORD_BASE, RECORD_STEP] }, "rt records reference")
	var launch_size := _base._create_shader(rd, { "compute": RTPipeline.LAUNCH_SIZE }, "rt records launch size")
	if not (raygen.is_valid() and hit_8.is_valid() and hit_32.is_valid() and hit_push.is_valid() and reference.is_valid() and launch_size.is_valid()):
		return

	var reference_pipeline := rd.compute_pipeline_create(reference)
	_base._freed.append(reference_pipeline)
	var expected := _trace_reference(rd, reference, reference_pipeline, tlas, 0, true)
	var expected_push := _trace_reference(rd, reference, reference_pipeline, tlas, PUSH_ADD, true)
	var expected_zero := _trace_reference(rd, reference, reference_pipeline, tlas, 0, false)
	result["scene"] = _check_scene(expected)

	var variants := {}
	var raygen_shader: RDPipelineShader = _base._pipeline_shader(raygen)
	var pipeline_8 := _pipeline(rd, raygen_shader, hit_8, 8, "record_8")
	if pipeline_8.is_valid():
		variants["record_8"] = _compare(expected, _traces(rd, raygen, pipeline_8, tlas, launch_size, 1, false, 0)[0])
		# The indirect traces, in INDIRECT_LISTS submits of one list each.
		var indirect := { "mismatches": 0, "traces": 0, "first": -1 }
		for _list_index in INDIRECT_LISTS:
			for data in _traces(rd, raygen, pipeline_8, tlas, launch_size, INDIRECT_TRACES, true, 0):
				var trace_compared := _compare(expected, data)
				indirect["traces"] += 1
				indirect["mismatches"] += trace_compared["mismatches"]
				if indirect["first"] < 0 and trace_compared["first"] >= 0:
					indirect["first"] = trace_compared["first"]
		variants["indirect"] = indirect
	var pipeline_32 := _pipeline(rd, raygen_shader, hit_32, 32, "record_32")
	if pipeline_32.is_valid():
		variants["record_32"] = _compare(expected, _traces(rd, raygen, pipeline_32, tlas, launch_size, 1, false, 0)[0])
	var pipeline_push := _pipeline(rd, raygen_shader, hit_push, 8, "push_constant")
	if pipeline_push.is_valid():
		variants["push_constant"] = _compare(expected_push, _traces(rd, raygen, pipeline_push, tlas, launch_size, 1, false, PUSH_ADD)[0])
	var drivers_with_zero_records := ["d3d12"]
	if drivers_with_zero_records.has(result["driver"]):
		var pipeline_none := _pipeline(rd, raygen_shader, hit_8, 0, "no_record")
		if pipeline_none.is_valid():
			variants["no_record"] = _compare(expected_zero, _traces(rd, raygen, pipeline_none, tlas, launch_size, 1, false, 0)[0])

	var names := ["record_8", "record_32", "push_constant", "indirect"]
	if drivers_with_zero_records.has(result["driver"]):
		names.append("no_record")
	for name in names:
		_base._check(variants.has(name), "the %s variant ran" % name)
		if variants.has(name):
			var compared: Dictionary = variants[name]
			_base._check(compared["mismatches"] == 0, "%s: %d rays differ from the inline reference, first at %d" % [name, compared["mismatches"], compared["first"]])
	_base._check(variants.has("indirect") and variants["indirect"]["traces"] == INDIRECT_TRACES * INDIRECT_LISTS, "every indirect trace ran")
	result["variants"] = variants


# A pipeline of the ray generation and miss shader and one closest-hit group per instance, whose records hold
# p_record_size bytes (0 for none), with a hit shader binding table that gives instance i hit group i.
func _pipeline(rd: RenderingDevice, p_raygen: RDPipelineShader, p_hit: RID, p_record_size: int, p_name: String) -> RID:
	var groups: Array[RDHitGroup] = []
	for i in INSTANCES:
		var group := RDHitGroup.new()
		group.closest_hit_shader = _base._pipeline_shader(p_hit)
		var value := RECORD_BASE + RECORD_STEP * i
		if p_record_size == 8:
			group.record_data = PackedInt32Array([value, 0]).to_byte_array()
		elif p_record_size == 32:
			group.record_data = PackedInt32Array([RECORD_FILL + 1, RECORD_FILL + 2, RECORD_FILL + 3, RECORD_FILL + 4, RECORD_FILL + 5, RECORD_FILL + 6, value, 0]).to_byte_array()
		_base._check(group.record_data.size() == p_record_size, "%s: the record of instance %d holds %d bytes" % [p_name, i, p_record_size])
		groups.append(group)
	var pipeline := rd.raytracing_pipeline_create([p_raygen], [p_raygen], groups, 1)
	_base._check(pipeline.is_valid(), "%s: the pipeline is created" % p_name)
	if not pipeline.is_valid():
		return RID()
	_base._freed.append(pipeline)
	var sbt := rd.hit_sbt_create(pipeline, INSTANCES)
	_base._check(sbt.is_valid(), "%s: the hit shader binding table is created" % p_name)
	if not sbt.is_valid():
		return RID()
	_base._freed.append(sbt)
	for i in INSTANCES:
		var range := rd.hit_sbt_range_alloc(sbt, 1)
		_base._check(range == RTPipeline._range(i) and rd.hit_sbt_range_update(sbt, range, 0, PackedInt32Array([i])) == OK, "%s: instance %d's range holds hit group %d" % [p_name, i, i])
	_sbt_of[pipeline] = sbt
	return pipeline


# p_count traces of p_pipeline in one raytracing list, each into a results buffer of its own: direct, or indirect
# with a launch size a compute shader wrote in the same frame. Returns the results.
func _traces(rd: RenderingDevice, p_raygen: RID, p_pipeline: RID, p_tlas: RID, p_launch_size: RID, p_count: int, p_indirect: bool, p_add: int) -> Array[PackedByteArray]:
	var buffers: Array[RID] = []
	var sets: Array[RID] = []
	for i in p_count:
		var buffer := rd.storage_buffer_create(SIZE * SIZE * 16)
		buffers.append(buffer)
		sets.append(rd.uniform_set_create([RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_tlas), RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 1, buffer)], p_raygen, 0))
	var temporary: Array[RID] = []
	var launch := RID()
	if p_indirect:
		launch = rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT, RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT)
		var launch_pipeline := rd.compute_pipeline_create(p_launch_size)
		var launch_set := rd.uniform_set_create([RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 0, launch)], p_launch_size, 0)
		temporary.append_array([launch_set, launch_pipeline, launch])
		var size := PackedInt32Array([SIZE, SIZE, 1, 0]).to_byte_array()
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, launch_pipeline)
		rd.compute_list_bind_uniform_set(compute_list, launch_set, 0)
		rd.compute_list_set_push_constant(compute_list, size, size.size())
		rd.compute_list_dispatch(compute_list, 1, 1, 1)
		rd.compute_list_end()

	var push := PackedInt32Array([p_add, 0, 0, 0]).to_byte_array()
	var sbt: RID = _sbt_of[p_pipeline]
	var list := rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(list, p_pipeline)
	rd.raytracing_list_set_push_constant(list, push, push.size())
	for i in p_count:
		rd.raytracing_list_bind_uniform_set(list, sets[i], 0)
		if p_indirect:
			rd.raytracing_list_trace_rays_indirect(list, 0, sbt, launch, 0)
		else:
			rd.raytracing_list_trace_rays(list, 0, sbt, SIZE, SIZE, 1)
	rd.raytracing_list_end()
	rd.submit()
	rd.sync()

	var data: Array[PackedByteArray] = []
	for buffer in buffers:
		data.append(rd.buffer_get_data(buffer))
	for rid in sets + buffers + temporary:
		rd.free_rid(rid)
	return data


func _trace_reference(rd: RenderingDevice, p_shader: RID, p_pipeline: RID, p_tlas: RID, p_add: int, p_records: bool) -> PackedByteArray:
	var results := rd.storage_buffer_create(SIZE * SIZE * 16)
	var io_set := rd.uniform_set_create([RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_tlas), RTPipeline._uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 1, results)], p_shader, 0)
	_base._check(io_set.is_valid(), "the reference uniform set is created")
	var push := PackedInt32Array([p_add, 1 if p_records else 0, 0, 0]).to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, p_pipeline)
	rd.compute_list_bind_uniform_set(list, io_set, 0)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, SIZE / GROUP_SIZE, SIZE / GROUP_SIZE, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()
	var data := rd.buffer_get_data(results)
	rd.free_rid(io_set)
	rd.free_rid(results)
	return data


# The scene must exercise what the fixture claims: rays that hit and miss, every instance and both faces.
func _check_scene(p_expected: PackedByteArray) -> Dictionary:
	var rays := SIZE * SIZE
	var hits := 0
	var front := 0
	var back := 0
	var values := {}
	for i in rays:
		if p_expected.decode_u32(i * 16) == MISS:
			continue
		hits += 1
		values[p_expected.decode_u32(i * 16 + 8)] = true
		if p_expected.decode_u32(i * 16 + 12) == 0xfe:
			front += 1
		else:
			back += 1
	_base._check(hits > rays / 8 and hits < rays, "rays both hit and miss (%d of %d hit)" % [hits, rays])
	_base._check(values.size() == INSTANCES, "every instance is hit (%d of %d)" % [values.size(), INSTANCES])
	_base._check(front > 0 and back > 0, "both faces are hit (%d front, %d back)" % [front, back])
	return { "hits": hits, "front": front, "back": back, "instances_hit": values.size(), "sha256": RTPipeline._digest(p_expected) }


static func _compare(p_expected: PackedByteArray, p_data: PackedByteArray) -> Dictionary:
	var compared := { "mismatches": 0, "first": -1, "sha256": RTPipeline._digest(p_data) }
	if p_data.size() != p_expected.size():
		compared["mismatches"] = SIZE * SIZE
		return compared
	if p_data == p_expected:
		return compared
	for i in SIZE * SIZE:
		for w in 4:
			if p_data.decode_u32(i * 16 + w * 4) != p_expected.decode_u32(i * 16 + w * 4):
				compared["mismatches"] += 1
				if compared["first"] < 0:
					compared["first"] = i
				break
	return compared


# Four quads of two triangles (the second wound the other way) in front of a camera at the origin looking down -Z,
# instance i at hit shader binding table range i with custom index i.
func _build_scene(rd: RenderingDevice) -> RID:
	var vertices := PackedFloat32Array([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0]).to_byte_array()
	var indices := PackedInt32Array([0, 1, 2, 0, 3, 2]).to_byte_array()
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_buffer := rd.storage_buffer_create(vertices.size(), vertices, 0, input_bits)
	var index_buffer := rd.storage_buffer_create(indices.size(), indices, 0, input_bits)
	_base._freed.append(vertex_buffer)
	_base._freed.append(index_buffer)
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = 4
	geometry.index_buffer = index_buffer
	geometry.index_count = 6
	geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32
	var blas := rd.blas_create([geometry], 0)
	_base._check(blas.is_valid(), "the quad BLAS is created")
	if not blas.is_valid():
		return RID()
	_base._freed.append(blas)
	_base._check(rd.blas_build(blas) == OK, "the quad BLAS is built")

	var instances: Array[RDAccelerationStructureInstance] = []
	for i in INSTANCES:
		var instance := RDAccelerationStructureInstance.new()
		instance.blas = blas
		instance.id = i
		instance.hit_sbt_range = RTPipeline._range(i)
		var origin := Vector3(-1.6 if i % 2 == 0 else 1.6, -1.6 if i < 2 else 1.6, -3.0 - 0.5 * i)
		instance.transform = Transform3D(Basis().scaled(Vector3(1.5, 1.5, 1.5)), origin)
		instances.append(instance)
	var tlas := rd.tlas_create(INSTANCES, 0)
	_base._check(tlas.is_valid(), "the TLAS is created")
	if not tlas.is_valid():
		return RID()
	_base._freed.append(tlas)
	_base._check(rd.tlas_build(tlas, instances) == OK, "the TLAS of %d instances is built" % INSTANCES)
	return tlas
