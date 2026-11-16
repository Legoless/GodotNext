extends Node

## RT pipeline fixture (#179 step 6; plan §21.1 C5, GR-5).
##
## Run it on a GPU, with a window:
##   godot --path godotnext/testbed --rendering-driver vulkan res://tests/rd_parity/rt_pipeline.tscn
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.
##
## 16 materials, one hit group each. A hit group is a closest-hit and an alpha-tested any-hit shader;
## its 8-byte shader record holds the material and the slot of the material's alpha texture in a
## bindless texture table, and its any-hit shader gets the material's alpha cutoff as a specialization
## constant. 48 non-opaque panels of 32 triangles each (some mirrored, half of the triangles wound the
## other way) sit in a TLAS at the hit shader binding table range of their material. 256 x 256 = 65,536
## rays, from a buffer both sides read, go through them.
##
## The reference is an inline ray query in a compute shader that alpha-tests in its candidate loop with
## the same texture texels and cutoffs, from a material table. Every RT pipeline result must equal it
## bit for bit: the instance, the primitive, t, the front face, the material and slot from the record
## and the alpha at the accepted hit. The variants:
## - "direct": one pipeline with all 16 hit groups, traced with raytracing_list_trace_rays();
## - "indirect": the same pipeline, traced with raytracing_list_trace_rays_indirect() from a launch size a
##   compute shader wrote in the same frame;
## - "restrided": a hit shader binding table made for the same hit groups without records, given the
##   pipeline with records by hit_sbt_set_pipeline(), so its entries grow and it is rewritten (on Direct3D 12
##   they keep their size: the hit shaders read a 16-byte record, which every entry holds there, #121);
## - "mid_list_1" and "mid_list_2": two traces of one table in one raytracing list, with
##   hit_sbt_range_alloc() growing the table between them, so the second trace reallocates the table
##   that the first one already put in the list;
## - "linked": a base library (ray generation and miss) and 16 one-hit-group libraries, linked;
## - "added": the base and the first 15 libraries linked, then that pipeline linked with the 16th
##   library (the new material) and swapped into the hit shader binding table with
##   hit_sbt_set_pipeline();
## - "reorder": the direct pipeline with a ray generation shader that reorders its hits (shader execution
##   reordering, with whichever GLSL extension LIMIT_RAYTRACING_REORDER_EXTENSIONS names), created with
##   RAYTRACING_PIPELINE_FLAG_REORDER.
## A device without SUPPORTS_RAYTRACING_REORDER, SUPPORTS_RAYTRACING_PIPELINE_LIBRARY or
## SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT runs the other variants and fails with reason NO_REORDER,
## NO_LIBRARIES or NO_INDIRECT. The one exception: Direct3D 12 has no shader execution reordering in v1
## (plan §21.1: DXIL's HitObject, shader model 6.9, is not in the Mesa NIR library), so on the d3d12 driver
## the result says "reorder": "not on D3D12" and the run does not fail for it (#121).
##
## Off the main thread: the first 15 libraries are created on WorkerThreadPool tasks and the base is linked
## with them on another, while the main thread traces "direct" again and again on the same device and
## compares each result. The 16th library and the link that adds it each run alone on a task while the
## main thread keeps calling compute_pipeline_is_valid(), which takes the device's lock: the task counts
## how many of those calls finished while its own call ran, and the check fails below
## MIN_OFF_THREAD_PROBES, which is what a call that held the lock while the driver compiled would give.
##
## The scene is only meaningful when the alpha test decides hits: the reference must differ from an
## opaque trace on some rays, every material must be hit, and both faces must be hit.
##
## User arguments: --out=<dir> to write the compiled ray generation SPIR-V there.

const SceneArgs = preload("res://scenes/scene_args.gd")

# The drivers that have no shader execution reordering by the plan (§21.1), and so do not fail without it.
const DRIVERS_WITHOUT_REORDER := ["d3d12"]

const WIDTH := 256
const HEIGHT := 256
const MATERIALS := 16
const INSTANCES := 48
const GRID := 4 # Quads per panel side.
const TEXTURE_SIZE := 16
const GROUP_SIZE := 8
const RECORD_SIZE := 8
const MISS := 0xffffffff
const MASK_32 := 0xffffffff
# The scene must make the alpha test matter: at least this share of rays changes against an opaque trace.
const MIN_ALPHA_DECIDED_SHARE := 0.05
# Lock-taking calls the main thread must finish while one worker call creates a library or links. A call
# that held the device's lock while the driver worked lets through only the calls that slip in before it
# takes the lock or after it lets go: 10 and 20 on an NVIDIA L4 when create and link were made to hold it,
# against 1,502 to 28,541 without it.
const MIN_OFF_THREAD_PROBES := 256

const COMMON := """#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
%s
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict readonly buffer Rays {
	vec4 data[];
}
rays;
layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	uvec4 data[];
}
results;
layout(set = 1, binding = 0) uniform texture2D alpha_textures[];

layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint pad0;
	uint pad1;
}
params;

// The alpha byte of the texel that the barycentrics land on. bary * 16 is exact, so both sides pick the
// same texel from the same barycentrics.
uint alpha_at(uint slot, vec2 bary) {
	uvec2 texel = min(uvec2(bary * 16.0), uvec2(15u));
	return uint(texelFetch(alpha_textures[nonuniformEXT(slot)], ivec2(texel), 0).a * 255.0 + 0.5);
}
"""

const RAYGEN := """
layout(location = 0) rayPayloadEXT uvec4 payload;

void main() {
	uint index = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
	vec4 origin = rays.data[index * 2u];
	vec4 direction = rays.data[index * 2u + 1u];
#if defined(REORDER_EXT)
	hitObjectEXT hit;
	hitObjectTraceRayEXT(hit, tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, origin.xyz, origin.w, direction.xyz, direction.w, 0);
	reorderThreadEXT(hit);
	hitObjectExecuteShaderEXT(hit, 0);
#elif defined(REORDER_NV)
	hitObjectNV hit;
	hitObjectTraceRayNV(hit, tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, origin.xyz, origin.w, direction.xyz, direction.w, 0);
	reorderThreadNV(hit);
	hitObjectExecuteShaderNV(hit, 0);
#else
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, origin.xyz, origin.w, direction.xyz, direction.w, 0);
#endif
	results.data[index] = payload;
}
"""

const MISS_SHADER := """
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u);
}
"""

# Not readonly: glslang then decorates the record NonWritable, which spirv-val rejects for the shader record
# storage class.
const RECORD := """
layout(shaderRecordEXT, std430) buffer HitRecord {
	uint material;
	uint slot;
}
record;
hitAttributeEXT vec2 bary;
"""

const ANY_HIT := """
layout(constant_id = 0) const uint ALPHA_CUTOFF = 128u;

void main() {
	if (alpha_at(record.slot, bary) < ALPHA_CUTOFF) {
		ignoreIntersectionEXT;
	}
}
"""

const CLOSEST_HIT := """
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	uint front = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? 1u : 0u;
	payload = uvec4(uint(gl_InstanceID), uint(gl_PrimitiveID), floatBitsToUint(gl_HitTEXT), front | (record.material << 1u) | (record.slot << 8u) | (alpha_at(record.slot, bary) << 24u));
}
"""

const REFERENCE := """#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict readonly buffer Rays {
	vec4 data[];
}
rays;
layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	uvec4 data[];
}
results;
// Per material: the alpha texture's slot and the alpha cutoff.
layout(set = 0, binding = 3, std430) restrict readonly buffer Materials {
	uvec2 data[];
}
materials;
layout(set = 1, binding = 0) uniform texture2D alpha_textures[];

layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint opaque;
	uint pad;
}
params;

uint alpha_at(uint slot, vec2 bary) {
	uvec2 texel = min(uvec2(bary * 16.0), uvec2(15u));
	return uint(texelFetch(alpha_textures[nonuniformEXT(slot)], ivec2(texel), 0).a * 255.0 + 0.5);
}

void main() {
	uvec2 id = gl_GlobalInvocationID.xy;
	if (id.x >= params.width || id.y >= params.height) {
		return;
	}
	uint index = id.y * params.width + id.x;
	vec4 origin = rays.data[index * 2u];
	vec4 direction = rays.data[index * 2u + 1u];

	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, params.opaque != 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT, 0xFF, origin.xyz, origin.w, direction.xyz, direction.w);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			uvec2 material = materials.data[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)];
			if (alpha_at(material.x, rayQueryGetIntersectionBarycentricsEXT(rq, false)) >= material.y) {
				rayQueryConfirmIntersectionEXT(rq);
			}
		}
	}

	uvec4 hit = uvec4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u);
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		uint material = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
		uint slot = materials.data[material].x;
		uint front = rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 1u : 0u;
		hit = uvec4(uint(rayQueryGetIntersectionInstanceIdEXT(rq, true)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), floatBitsToUint(rayQueryGetIntersectionTEXT(rq, true)), front | (material << 1u) | (slot << 8u) | (alpha_at(slot, rayQueryGetIntersectionBarycentricsEXT(rq, true)) << 24u));
	}
	results.data[index] = hit;
}
"""

# Writes the indirect launch size, so the indirect trace reads a size the GPU wrote in the same frame.
const LAUNCH_SIZE := """#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Launch {
	uint data[];
}
launch;

layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint depth;
	uint pad;
}
params;

void main() {
	launch.data[0] = params.width;
	launch.data[1] = params.height;
	launch.data[2] = params.depth;
}
"""

var _failures := PackedStringArray()
var _rd: RenderingDevice
var _freed: Array[RID] = []
# Filled by the WorkerThreadPool tasks, under _mutex.
var _mutex := Mutex.new()
var _libraries: Array[RID] = []
var _task_result := RID()
# The lock-taking calls the main thread has finished in the current probed task, and their count when the
# task's own call started and when it returned (-1 before).
var _probes := 0
var _probes_at_start := -1
var _probes_at_end := -1
# The hit shader binding table ranges of each table, by material.
var _ranges_of := {}


func _ready() -> void:
	var result: Dictionary = _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var result := {
		"test": "rt_pipeline",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"params": { "width": WIDTH, "height": HEIGHT, "materials": MATERIALS, "instances": INSTANCES },
	}
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result
	_rd = rd
	result["adapter"] = rd.get_device_name()
	result["supported"] = {
		"raytracing_pipeline": rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE),
		"ray_query": rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY),
		"bindless": rd.has_feature(RenderingDevice.SUPPORTS_BINDLESS),
		"reorder": rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_REORDER),
		"reorder_extensions": rd.limit_get(RenderingDevice.LIMIT_RAYTRACING_REORDER_EXTENSIONS),
		"libraries": rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE_LIBRARY),
		"indirect": rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT),
	}
	if not result["supported"]["raytracing_pipeline"]:
		result["reason"] = "NO_RAYTRACING_PIPELINE"
		_check(false, "the device reports SUPPORTS_RAYTRACING_PIPELINE")
	elif not result["supported"]["ray_query"]:
		result["reason"] = "NO_RAY_QUERY"
		_check(false, "the device reports SUPPORTS_RAY_QUERY, which the reference needs")
	elif not result["supported"]["bindless"]:
		result["reason"] = "NO_BINDLESS"
		_check(false, "the device reports SUPPORTS_BINDLESS")
	else:
		_run_pipelines(rd, result)
		# Each missing feature fails the run with its reason; the variants that need it did not run.
		for feature in [["reorder", "NO_REORDER", "SUPPORTS_RAYTRACING_REORDER"], ["indirect", "NO_INDIRECT", "SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT"], ["libraries", "NO_LIBRARIES", "SUPPORTS_RAYTRACING_PIPELINE_LIBRARY"]]:
			if feature[0] == "reorder" and not result["supported"]["reorder"] and DRIVERS_WITHOUT_REORDER.has(result["driver"]):
				result["reorder"] = "not on D3D12"
				continue
			if not result["supported"][feature[0]]:
				result["reason"] = feature[1]
				_check(false, "the device reports " + feature[2])

	for i in range(_freed.size() - 1, -1, -1):
		if _freed[i].is_valid():
			rd.free_rid(_freed[i])
	rd.free()
	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


func _run_pipelines(rd: RenderingDevice, result: Dictionary) -> void:
	# The scene.
	var slots := _material_slots()
	var cutoffs := _material_cutoffs()
	var table := rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_TEXTURE, 64)
	_check(table.is_valid(), "the texture table is created")
	if not table.is_valid():
		return
	_freed.append(table)
	# Slot k holds the texture of the material whose slot is k.
	var material_of_slot := PackedInt32Array()
	material_of_slot.resize(MATERIALS)
	for m in MATERIALS:
		material_of_slot[slots[m]] = m
	for k in MATERIALS:
		var texture := _create_alpha_texture(rd, material_of_slot[k])
		_freed.append(texture)
		_check(rd.bindless_table_allocate(table, texture) == k, "the texture of material %d gets slot %d" % [material_of_slot[k], k])

	var tlas := _build_scene(rd, result)
	if not tlas.is_valid():
		return

	var rays := rd.storage_buffer_create(WIDTH * HEIGHT * 32, _rays().to_byte_array())
	_freed.append(rays)

	# Shaders.
	var raygen := _create_shader(rd, { "raygen": COMMON % "" + RAYGEN, "miss": COMMON % "" + MISS_SHADER }, "rt raygen")
	var hit := _create_shader(rd, { "any_hit": COMMON % "" + RECORD + ANY_HIT, "closest_hit": COMMON % "" + RECORD + CLOSEST_HIT }, "rt hit")
	var reference := _create_shader(rd, { "compute": REFERENCE }, "rt reference")
	var launch_size := _create_shader(rd, { "compute": LAUNCH_SIZE }, "rt launch size")
	if not (raygen.is_valid() and hit.is_valid() and reference.is_valid() and launch_size.is_valid()):
		return

	# The inline reference, alpha-tested and opaque.
	var material_table := PackedInt32Array()
	for m in MATERIALS:
		material_table.append(slots[m])
		material_table.append(cutoffs[m])
	var materials := rd.storage_buffer_create(material_table.size() * 4, material_table.to_byte_array())
	_freed.append(materials)
	var reference_pipeline := rd.compute_pipeline_create(reference)
	_freed.append(reference_pipeline)
	var alpha_tested := _trace_reference(rd, reference, reference_pipeline, tlas, rays, materials, table, false)
	var opaque := _trace_reference(rd, reference, reference_pipeline, tlas, rays, materials, table, true)
	result["scene"] = _check_scene(alpha_tested, opaque)

	var hit_groups: Array[RDHitGroup] = []
	for m in MATERIALS:
		hit_groups.append(_hit_group(hit, m, slots[m], cutoffs[m]))
	var raygen_shader := _pipeline_shader(raygen)
	var miss_shader := _pipeline_shader(raygen)

	var variants := {}
	# Direct and indirect, one pipeline with every hit group.
	var direct_pipeline := rd.raytracing_pipeline_create([raygen_shader], [miss_shader], hit_groups, 1)
	_check(direct_pipeline.is_valid(), "the pipeline with 16 hit groups is created")
	if direct_pipeline.is_valid():
		_freed.append(direct_pipeline)
		var sbt := _hit_sbt(rd, direct_pipeline, MATERIALS)
		variants["direct"] = _compare(alpha_tested, _trace(rd, raygen, direct_pipeline, sbt, tlas, rays, table, RID()))
		if result["supported"]["indirect"]:
			var launch := rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT, RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT)
			_freed.append(launch)
			variants["indirect"] = _compare(alpha_tested, _trace(rd, raygen, direct_pipeline, sbt, tlas, rays, table, launch, launch_size))
		# Two traces of one table in one list; the table grows between them, so the second trace replaces
		# the buffer (and its tracker) that the first one put in the list.
		var grown := _hit_sbt(rd, direct_pipeline, MATERIALS)
		var traces := _trace_growing(rd, raygen, direct_pipeline, grown, tlas, rays, table)
		variants["mid_list_1"] = _compare(alpha_tested, traces[0])
		variants["mid_list_2"] = _compare(alpha_tested, traces[1])
		# A table made for the same hit groups without records takes the pipeline with them: its entries
		# grow by the record, so the table is reallocated and rewritten (on Direct3D 12 only rewritten).
		var plain_groups: Array[RDHitGroup] = []
		for m in MATERIALS:
			var plain := _hit_group(hit, m, slots[m], cutoffs[m])
			plain.record_data = PackedByteArray()
			plain_groups.append(plain)
		var plain_pipeline := rd.raytracing_pipeline_create([raygen_shader], [miss_shader], plain_groups, 1)
		_check(plain_pipeline.is_valid(), "the pipeline without records is created")
		if plain_pipeline.is_valid():
			_freed.append(plain_pipeline)
			var restrided := _hit_sbt(rd, plain_pipeline, MATERIALS)
			_check(rd.hit_sbt_set_pipeline(restrided, direct_pipeline) == OK, "the table without records takes the pipeline with them")
			variants["restrided"] = _compare(alpha_tested, _trace(rd, raygen, direct_pipeline, restrided, tlas, rays, table, RID()))

	# Libraries, created and linked off the main thread.
	if result["supported"]["libraries"]:
		var base_library := rd.raytracing_pipeline_create([raygen_shader], [miss_shader], [], 1, RenderingDevice.RAYTRACING_PIPELINE_FLAG_LIBRARY)
		_check(base_library.is_valid(), "the base library (ray generation and miss) is created")
		if base_library.is_valid() and direct_pipeline.is_valid():
			_freed.append(base_library)
			_run_libraries(rd, base_library, raygen, direct_pipeline, hit_groups, tlas, rays, table, alpha_tested, variants, result)

	# Shader execution reordering.
	if result["supported"]["reorder"]:
		var extensions := rd.limit_get(RenderingDevice.LIMIT_RAYTRACING_REORDER_EXTENSIONS)
		var define := "#extension GL_EXT_shader_invocation_reorder : require\n#define REORDER_EXT\n" if extensions & 1 else "#extension GL_NV_shader_invocation_reorder : require\n#define REORDER_NV\n"
		result["reorder_extension"] = "EXT" if extensions & 1 else "NV"
		var reorder_raygen := _create_shader(rd, { "raygen": COMMON % define + RAYGEN, "miss": COMMON % "" + MISS_SHADER }, "rt raygen reorder", SceneArgs.value("--out", ""))
		if reorder_raygen.is_valid():
			var reorder_pipeline := rd.raytracing_pipeline_create([_pipeline_shader(reorder_raygen)], [_pipeline_shader(reorder_raygen)], hit_groups, 1, RenderingDevice.RAYTRACING_PIPELINE_FLAG_REORDER)
			_check(reorder_pipeline.is_valid(), "the reordering pipeline is created")
			if reorder_pipeline.is_valid():
				_freed.append(reorder_pipeline)
				var sbt := _hit_sbt(rd, reorder_pipeline, MATERIALS)
				variants["reorder"] = _compare(alpha_tested, _trace(rd, reorder_raygen, reorder_pipeline, sbt, tlas, rays, table, RID()))

	var expected := ["direct", "restrided", "mid_list_1", "mid_list_2"]
	if result["supported"]["indirect"]:
		expected.append("indirect")
	if result["supported"]["libraries"]:
		expected.append_array(["linked", "added"])
	if result["supported"]["reorder"]:
		expected.append("reorder")
	for name in expected:
		_check(variants.has(name), "the %s variant ran" % name)
		if variants.has(name):
			var compared: Dictionary = variants[name]
			_check(compared["mismatches"] == 0, "%s: %d of %d rays differ from the inline reference (instance %d, primitive %d, t %d, front face %d, material %d, slot %d, alpha %d), first at %d" % [name, compared["mismatches"], WIDTH * HEIGHT, compared["instance"], compared["primitive"], compared["t"], compared["front_face"], compared["material"], compared["slot"], compared["alpha"], compared["first"]])
	result["variants"] = variants


func _run_libraries(rd: RenderingDevice, base_library: RID, raygen: RID, direct_pipeline: RID, hit_groups: Array[RDHitGroup], tlas: RID, rays: RID, table: RID, alpha_tested: PackedByteArray, variants: Dictionary, result: Dictionary) -> void:
	var off_thread := {}
	result["off_thread"] = off_thread
	var direct_sbt := _hit_sbt(rd, direct_pipeline, MATERIALS)

	# The first 15 libraries, one per material, each on a WorkerThreadPool task, while the main thread
	# traces on the same device and checks each trace.
	_libraries.resize(MATERIALS)
	var group := WorkerThreadPool.add_group_task(_create_library.bind(hit_groups), MATERIALS - 1, -1, true, "rt_pipeline libraries")
	off_thread["library_traces"] = _trace_while(rd, func() -> bool: return WorkerThreadPool.is_group_task_completed(group), raygen, direct_pipeline, direct_sbt, tlas, rays, table, alpha_tested)
	WorkerThreadPool.wait_for_group_task_completion(group)

	# The 16th library alone on a task, while the main thread keeps taking the device's lock.
	var last := MATERIALS - 1
	_libraries[last] = _probed(_create_library_probed.bind(last, hit_groups), "library", off_thread)
	var created := 0
	for library in _libraries:
		if library.is_valid():
			created += 1
			_freed.append(library)
	_check(created == MATERIALS, "every material library is created (%d of %d)" % [created, MATERIALS])
	if created != MATERIALS:
		return

	# The base and the first 15 libraries, linked on a task while the main thread traces.
	_task_result = RID()
	var task := WorkerThreadPool.add_task(_link.bind(base_library, _libraries.slice(0, last)), true, "rt_pipeline link")
	off_thread["link_traces"] = _trace_while(rd, func() -> bool: return WorkerThreadPool.is_task_completed(task), raygen, direct_pipeline, direct_sbt, tlas, rays, table, alpha_tested)
	WorkerThreadPool.wait_for_task_completion(task)
	_mutex.lock()
	var linked_15 := _task_result
	_mutex.unlock()
	_check(linked_15.is_valid(), "the base and 15 libraries are linked")
	if not linked_15.is_valid():
		return
	_freed.append(linked_15)

	# All 16 at once.
	var linked := rd.raytracing_pipeline_link(base_library, _libraries)
	_check(linked.is_valid(), "the base and 16 libraries are linked")
	if linked.is_valid():
		_freed.append(linked)
		var sbt := _hit_sbt(rd, linked, MATERIALS)
		variants["linked"] = _compare(alpha_tested, _trace(rd, raygen, linked, sbt, tlas, rays, table, RID()))

	# The new material: link the 15-library pipeline with the 16th library on a task, probed, then swap it in.
	var sbt_15 := _hit_sbt(rd, linked_15, MATERIALS - 1)
	var added: Array[RID] = [_libraries[last]]
	var linked_16 := _probed(_link_probed.bind(linked_15, added), "add", off_thread)
	_check(linked_16.is_valid(), "the 15-library pipeline is linked with the 16th library")
	if not linked_16.is_valid():
		return
	_freed.append(linked_16)
	_check(rd.hit_sbt_set_pipeline(sbt_15, linked_16) == OK, "the hit shader binding table takes the pipeline with the new material")
	var last_range: int = _ranges_of[sbt_15][last]
	_check(rd.hit_sbt_range_update(sbt_15, last_range, 0, PackedInt32Array([last])) == OK, "the new material's range gets its hit group")
	variants["added"] = _compare(alpha_tested, _trace(rd, raygen, linked_16, sbt_15, tlas, rays, table, RID()))
	# The table now depends on linked_16, which frees it; free it first, so the cleanup frees each RID once.
	rd.free_rid(sbt_15)
	_freed.erase(sbt_15)


# Traces with the direct pipeline until p_done returns true (at least once), checking every result against
# the reference. Returns the number of traces.
func _trace_while(rd: RenderingDevice, p_done: Callable, raygen: RID, pipeline: RID, sbt: RID, tlas: RID, rays: RID, table: RID, alpha_tested: PackedByteArray) -> int:
	var traces := 0
	var mismatches := 0
	while true:
		var compared := _compare(alpha_tested, _trace(rd, raygen, pipeline, sbt, tlas, rays, table, RID()))
		traces += 1
		mismatches += compared["mismatches"]
		if p_done.call():
			break
	_check(mismatches == 0, "the traces made while a worker built libraries match the reference (%d rays differ over %d traces)" % [mismatches, traces])
	return traces


# Runs p_task (_create_library_probed or _link_probed) on a WorkerThreadPool task while the main thread
# keeps calling compute_pipeline_is_valid(), which takes the device's lock, and returns the task's RID.
func _probed(p_task: Callable, p_name: String, off_thread: Dictionary) -> RID:
	_mutex.lock()
	_probes = 0
	_probes_at_start = -1
	_probes_at_end = -1
	_task_result = RID()
	_mutex.unlock()
	var task := WorkerThreadPool.add_task(p_task, true, "rt_pipeline " + p_name)
	var probe := RID()
	while not WorkerThreadPool.is_task_completed(task):
		_rd.compute_pipeline_is_valid(probe)
		_mutex.lock()
		_probes += 1
		_mutex.unlock()
	WorkerThreadPool.wait_for_task_completion(task)
	_mutex.lock()
	var probes := _probes_at_end - _probes_at_start
	var rid := _task_result
	_mutex.unlock()
	off_thread[p_name + "_probes"] = probes
	_check(probes >= MIN_OFF_THREAD_PROBES, "the main thread takes the device's lock while a worker's %s call runs (%d calls finished during it, at least %d expected)" % [p_name, probes, MIN_OFF_THREAD_PROBES])
	return rid


func _create_library(p_index: int, p_hit_groups: Array[RDHitGroup]) -> void:
	var library := _rd.raytracing_pipeline_create([], [], [p_hit_groups[p_index]], 1, RenderingDevice.RAYTRACING_PIPELINE_FLAG_LIBRARY)
	_mutex.lock()
	_libraries[p_index] = library
	_mutex.unlock()


func _create_library_probed(p_index: int, p_hit_groups: Array[RDHitGroup]) -> void:
	var hit_group := [p_hit_groups[p_index]]
	_mutex.lock()
	_probes_at_start = _probes
	_mutex.unlock()
	var library := _rd.raytracing_pipeline_create([], [], hit_group, 1, RenderingDevice.RAYTRACING_PIPELINE_FLAG_LIBRARY)
	_mutex.lock()
	_probes_at_end = _probes
	_task_result = library
	_mutex.unlock()


func _link(p_base: RID, p_libraries: Array[RID]) -> void:
	var linked := _rd.raytracing_pipeline_link(p_base, p_libraries)
	_mutex.lock()
	_task_result = linked
	_mutex.unlock()


func _link_probed(p_base: RID, p_libraries: Array[RID]) -> void:
	_mutex.lock()
	_probes_at_start = _probes
	_mutex.unlock()
	var linked := _rd.raytracing_pipeline_link(p_base, p_libraries)
	_mutex.lock()
	_probes_at_end = _probes
	_task_result = linked
	_mutex.unlock()


# A hit shader binding table with one range of one hit group per material. Ranges are handed out in
# order, so range m starts at entry m in every table, which is the offset the TLAS instances hold.
# Materials from p_bound on get hit group 0 until the pipeline has theirs.
func _hit_sbt(rd: RenderingDevice, p_pipeline: RID, p_bound: int) -> RID:
	var sbt := rd.hit_sbt_create(p_pipeline, MATERIALS)
	_check(sbt.is_valid(), "the hit shader binding table is created")
	_freed.append(sbt)
	var ranges := []
	for m in MATERIALS:
		var range := rd.hit_sbt_range_alloc(sbt, 1)
		_check(range == _range(m), "material %d's range starts at entry %d" % [m, m])
		_check(rd.hit_sbt_range_update(sbt, range, 0, PackedInt32Array([m if m < p_bound else 0])) == OK, "material %d's range is set" % m)
		ranges.append(range)
	_ranges_of[sbt] = ranges
	return sbt


# The value of a range of one hit group at entry p_entry.
static func _range(p_entry: int) -> int:
	return (1 << 32) | p_entry


func _trace(rd: RenderingDevice, p_shader: RID, p_pipeline: RID, p_sbt: RID, p_tlas: RID, p_rays: RID, p_table: RID, p_launch: RID, p_launch_shader: RID = RID()) -> PackedByteArray:
	var results := rd.storage_buffer_create(WIDTH * HEIGHT * 16)
	var io_set := rd.uniform_set_create([_uniform(RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_tlas), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 1, p_rays), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 2, results)], p_shader, 0)
	var table_set := _table_set(rd, p_shader, p_table)
	_check(io_set.is_valid() and table_set.is_valid(), "the RT uniform sets are created")
	var push := PackedInt32Array([WIDTH, HEIGHT, 0, 0]).to_byte_array()
	var launch_set := RID()
	var launch_pipeline := RID()
	if p_launch.is_valid():
		launch_pipeline = rd.compute_pipeline_create(p_launch_shader)
		launch_set = rd.uniform_set_create([_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 0, p_launch)], p_launch_shader, 0)
		var size := PackedInt32Array([WIDTH, HEIGHT, 1, 0]).to_byte_array()
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, launch_pipeline)
		rd.compute_list_bind_uniform_set(compute_list, launch_set, 0)
		rd.compute_list_set_push_constant(compute_list, size, size.size())
		rd.compute_list_dispatch(compute_list, 1, 1, 1)
		rd.compute_list_end()

	var list := rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(list, p_pipeline)
	rd.raytracing_list_bind_uniform_set(list, io_set, 0)
	rd.raytracing_list_bind_uniform_set(list, table_set, 1)
	rd.raytracing_list_set_push_constant(list, push, push.size())
	if p_launch.is_valid():
		rd.raytracing_list_trace_rays_indirect(list, 0, p_sbt, p_launch, 0)
	else:
		rd.raytracing_list_trace_rays(list, 0, p_sbt, WIDTH, HEIGHT, 1)
	rd.raytracing_list_end()
	rd.submit()
	rd.sync()
	var data := rd.buffer_get_data(results)
	for rid in [launch_set, launch_pipeline, table_set, io_set, results]:
		if rid.is_valid():
			rd.free_rid(rid)
	return data


# Two traces of p_sbt in one raytracing list, into two result buffers. Between them a new range grows the
# table past its capacity, so the second trace reallocates the table the first one already used.
func _trace_growing(rd: RenderingDevice, p_shader: RID, p_pipeline: RID, p_sbt: RID, p_tlas: RID, p_rays: RID, p_table: RID) -> Array[PackedByteArray]:
	var results: Array[RID] = [rd.storage_buffer_create(WIDTH * HEIGHT * 16), rd.storage_buffer_create(WIDTH * HEIGHT * 16)]
	var io_sets: Array[RID] = []
	for buffer in results:
		io_sets.append(rd.uniform_set_create([_uniform(RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_tlas), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 1, p_rays), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 2, buffer)], p_shader, 0))
	var table_set := _table_set(rd, p_shader, p_table)
	_check(io_sets[0].is_valid() and io_sets[1].is_valid() and table_set.is_valid(), "the RT uniform sets of the growing table are created")
	var push := PackedInt32Array([WIDTH, HEIGHT, 0, 0]).to_byte_array()

	var list := rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(list, p_pipeline)
	rd.raytracing_list_bind_uniform_set(list, io_sets[0], 0)
	rd.raytracing_list_bind_uniform_set(list, table_set, 1)
	rd.raytracing_list_set_push_constant(list, push, push.size())
	rd.raytracing_list_trace_rays(list, 0, p_sbt, WIDTH, HEIGHT, 1)
	_check(rd.hit_sbt_range_alloc(p_sbt, 1) == _range(MATERIALS), "the growing table's new range comes after the materials' ranges")
	rd.raytracing_list_bind_uniform_set(list, io_sets[1], 0)
	rd.raytracing_list_trace_rays(list, 0, p_sbt, WIDTH, HEIGHT, 1)
	rd.raytracing_list_end()
	rd.submit()
	rd.sync()

	var data: Array[PackedByteArray] = [rd.buffer_get_data(results[0]), rd.buffer_get_data(results[1])]
	for rid in [table_set, io_sets[0], io_sets[1], results[0], results[1]]:
		rd.free_rid(rid)
	return data


func _trace_reference(rd: RenderingDevice, p_shader: RID, p_pipeline: RID, p_tlas: RID, p_rays: RID, p_materials: RID, p_table: RID, p_opaque: bool) -> PackedByteArray:
	var results := rd.storage_buffer_create(WIDTH * HEIGHT * 16)
	var io_set := rd.uniform_set_create([_uniform(RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_tlas), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 1, p_rays), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 2, results), _uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 3, p_materials)], p_shader, 0)
	var table_set := _table_set(rd, p_shader, p_table)
	_check(io_set.is_valid() and table_set.is_valid(), "the reference uniform sets are created")
	var push := PackedInt32Array([WIDTH, HEIGHT, 1 if p_opaque else 0, 0]).to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, p_pipeline)
	rd.compute_list_bind_uniform_set(list, io_set, 0)
	rd.compute_list_bind_uniform_set(list, table_set, 1)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, WIDTH / GROUP_SIZE, HEIGHT / GROUP_SIZE, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()
	var data := rd.buffer_get_data(results)
	for rid in [table_set, io_set, results]:
		rd.free_rid(rid)
	return data


# The scene must exercise what the fixture claims: rays that hit and miss, both faces, every material,
# and alpha decisions that change the result.
func _check_scene(p_alpha_tested: PackedByteArray, p_opaque: PackedByteArray) -> Dictionary:
	var rays := WIDTH * HEIGHT
	var hits := 0
	var front := 0
	var back := 0
	var materials_hit := {}
	var alpha_decided := 0
	for i in rays:
		var flags := p_alpha_tested.decode_u32(i * 16 + 12)
		var instance := p_alpha_tested.decode_u32(i * 16)
		if instance != MISS:
			hits += 1
			if flags & 1:
				front += 1
			else:
				back += 1
			materials_hit[(flags >> 1) & 0x7f] = true
		for w in 4:
			if p_alpha_tested.decode_u32(i * 16 + w * 4) != p_opaque.decode_u32(i * 16 + w * 4):
				alpha_decided += 1
				break
	_check(hits > rays / 4 and hits < rays, "rays both hit and miss (%d of %d hit)" % [hits, rays])
	_check(front > 0 and back > 0, "both faces are hit (%d front, %d back)" % [front, back])
	_check(materials_hit.size() == MATERIALS, "every material is hit (%d of %d)" % [materials_hit.size(), MATERIALS])
	_check(alpha_decided >= int(rays * MIN_ALPHA_DECIDED_SHARE), "the alpha test changes the result of at least %d%% of rays (%d)" % [int(MIN_ALPHA_DECIDED_SHARE * 100), alpha_decided])
	return { "hits": hits, "front": front, "back": back, "materials_hit": materials_hit.size(), "alpha_decided": alpha_decided, "sha256": _digest(p_alpha_tested) }


func _compare(p_expected: PackedByteArray, p_data: PackedByteArray) -> Dictionary:
	var compared := { "mismatches": 0, "instance": 0, "primitive": 0, "t": 0, "front_face": 0, "material": 0, "slot": 0, "alpha": 0, "first": -1, "sha256": _digest(p_data) }
	if p_data.size() != p_expected.size():
		compared["mismatches"] = WIDTH * HEIGHT
		return compared
	if p_data == p_expected:
		return compared
	for i in WIDTH * HEIGHT:
		var differs := false
		for w in 3:
			if p_data.decode_u32(i * 16 + w * 4) != p_expected.decode_u32(i * 16 + w * 4):
				compared[["instance", "primitive", "t"][w]] += 1
				differs = true
		var a := p_data.decode_u32(i * 16 + 12)
		var b := p_expected.decode_u32(i * 16 + 12)
		if (a & 1) != (b & 1):
			compared["front_face"] += 1
			differs = true
		if ((a >> 1) & 0x7f) != ((b >> 1) & 0x7f):
			compared["material"] += 1
			differs = true
		if ((a >> 8) & 0xffff) != ((b >> 8) & 0xffff):
			compared["slot"] += 1
			differs = true
		if (a >> 24) != (b >> 24):
			compared["alpha"] += 1
			differs = true
		if differs:
			compared["mismatches"] += 1
			if compared["first"] < 0:
				compared["first"] = i
	return compared


# 48 panels in front of a camera at the origin looking down -Z, each with its material's range.
func _build_scene(rd: RenderingDevice, result: Dictionary) -> RID:
	var vertices := PackedFloat32Array()
	var indices := PackedInt32Array()
	for y in GRID + 1:
		for x in GRID + 1:
			vertices.append_array([float(x) / GRID * 2.0 - 1.0, float(y) / GRID * 2.0 - 1.0, 0.0])
	for y in GRID:
		for x in GRID:
			var v00 := y * (GRID + 1) + x
			var v10 := v00 + 1
			var v01 := v00 + GRID + 1
			var v11 := v01 + 1
			indices.append_array([v00, v10, v11])
			# Every other quad winds its second triangle the other way, so a panel shows both faces.
			if (x + y) % 2 == 0:
				indices.append_array([v00, v11, v01])
			else:
				indices.append_array([v00, v01, v11])
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_bytes := vertices.to_byte_array()
	var index_bytes := indices.to_byte_array()
	var vertex_buffer := rd.storage_buffer_create(vertex_bytes.size(), vertex_bytes, 0, input_bits)
	var index_buffer := rd.storage_buffer_create(index_bytes.size(), index_bytes, 0, input_bits)
	_freed.append(vertex_buffer)
	_freed.append(index_buffer)
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = vertices.size() / 3
	geometry.index_buffer = index_buffer
	geometry.index_count = indices.size()
	geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32
	# Non-opaque: the any-hit shaders and the reference's candidate loop decide.
	geometry.flags = 0
	var blas := rd.blas_create([geometry], 0)
	_check(blas.is_valid(), "the panel BLAS is created")
	if not blas.is_valid():
		return RID()
	_freed.append(blas)
	_check(rd.blas_build(blas) == OK, "the panel BLAS is built")

	var instances: Array[RDAccelerationStructureInstance] = []
	var mirrored := 0
	for i in INSTANCES:
		var material := i % MATERIALS
		var instance := RDAccelerationStructureInstance.new()
		instance.blas = blas
		instance.id = material
		instance.hit_sbt_range = _range(material)
		var angle_x := (_unit(i, 1) - 0.5) * 1.4
		var angle_y := (_unit(i, 2) - 0.5) * 1.4
		var scale := 0.8 + _unit(i, 3) * 1.4
		var basis := Basis.from_euler(Vector3(angle_x, angle_y, _unit(i, 4) * TAU)).scaled(Vector3(scale, scale, scale))
		if i % 5 == 0:
			basis = basis.scaled(Vector3(-1, 1, 1))
			mirrored += 1
		var origin := Vector3((_unit(i, 5) - 0.5) * 7.0, (_unit(i, 6) - 0.5) * 7.0, -4.0 - _unit(i, 7) * 8.0)
		instance.transform = Transform3D(basis, origin)
		instances.append(instance)
	var tlas := rd.tlas_create(INSTANCES, 0)
	_check(tlas.is_valid(), "the TLAS is created")
	if not tlas.is_valid():
		return RID()
	_freed.append(tlas)
	_check(rd.tlas_build(tlas, instances) == OK, "the TLAS of %d instances is built" % INSTANCES)
	result["mirrored_instances"] = mirrored
	return tlas


# One ray per pixel from the origin, through a 90-degree view down -Z: origin and t_min, then direction
# and t_max.
func _rays() -> PackedFloat32Array:
	var data := PackedFloat32Array()
	data.resize(WIDTH * HEIGHT * 8)
	for y in HEIGHT:
		for x in WIDTH:
			var i := (y * WIDTH + x) * 8
			var direction := Vector3((x + 0.5) / WIDTH * 2.0 - 1.0, 1.0 - (y + 0.5) / HEIGHT * 2.0, -1.0).normalized()
			data[i + 3] = 0.001
			data[i + 4] = direction.x
			data[i + 5] = direction.y
			data[i + 6] = direction.z
			data[i + 7] = 1000.0
	return data


# Material m's alpha texture sits at slot slots[m], a permutation, so a record that names the wrong slot
# shows.
static func _material_slots() -> PackedInt32Array:
	var slots := PackedInt32Array()
	for m in MATERIALS:
		slots.append((m * 7 + 3) % MATERIALS)
	return slots


# Cutoffs from 32 to 212: some materials are mostly opaque, some mostly cut out.
static func _material_cutoffs() -> PackedInt32Array:
	var cutoffs := PackedInt32Array()
	for m in MATERIALS:
		cutoffs.append(32 + 12 * m)
	return cutoffs


func _create_alpha_texture(rd: RenderingDevice, p_material: int) -> RID:
	var texels := PackedByteArray()
	texels.resize(TEXTURE_SIZE * TEXTURE_SIZE * 4)
	for y in TEXTURE_SIZE:
		for x in TEXTURE_SIZE:
			var h := _hash3(p_material, x, y)
			var i := (y * TEXTURE_SIZE + x) * 4
			texels[i] = h & 0xff
			texels[i + 1] = (h >> 8) & 0xff
			texels[i + 2] = (h >> 16) & 0xff
			texels[i + 3] = (h >> 24) & 0xff
	var format := RDTextureFormat.new()
	format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.width = TEXTURE_SIZE
	format.height = TEXTURE_SIZE
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
	var texture := rd.texture_create(format, RDTextureView.new(), [texels])
	_check(texture.is_valid(), "the alpha texture of material %d is created" % p_material)
	return texture


func _hit_group(p_hit_shader: RID, p_material: int, p_slot: int, p_cutoff: int) -> RDHitGroup:
	var group := RDHitGroup.new()
	group.closest_hit_shader = _pipeline_shader(p_hit_shader)
	var any_hit := _pipeline_shader(p_hit_shader)
	var cutoff := RDPipelineSpecializationConstant.new()
	cutoff.constant_id = 0
	cutoff.value = p_cutoff
	any_hit.specialization_constants = [cutoff]
	group.any_hit_shader = any_hit
	group.record_data = PackedInt32Array([p_material, p_slot]).to_byte_array()
	_check(group.record_data.size() == RECORD_SIZE, "the record of material %d holds %d bytes" % [p_material, RECORD_SIZE])
	return group


func _pipeline_shader(p_shader: RID) -> RDPipelineShader:
	var pipeline_shader := RDPipelineShader.new()
	pipeline_shader.shader = p_shader
	return pipeline_shader


func _create_shader(rd: RenderingDevice, p_stages: Dictionary, p_name: String, p_out_dir: String = "") -> RID:
	var source := RDShaderSource.new()
	for stage in p_stages:
		source.set("source_" + stage, p_stages[stage])
	var spirv := rd.shader_compile_spirv_from_source(source)
	for stage in p_stages:
		var error: String = spirv.get("compile_error_" + stage)
		_check(error.is_empty(), "%s: the %s stage compiles: %s" % [p_name, stage, error])
		if not error.is_empty():
			return RID()
	if not p_out_dir.is_empty() and p_stages.has("raygen"):
		DirAccess.make_dir_recursive_absolute(p_out_dir)
		var file := FileAccess.open(p_out_dir.path_join(p_name.replace(" ", "_") + ".spv"), FileAccess.WRITE)
		if file != null:
			file.store_buffer(spirv.bytecode_raygen)
	var shader := rd.shader_create_from_spirv(spirv, p_name)
	_check(shader.is_valid(), "%s: the shader is created" % p_name)
	if shader.is_valid():
		_freed.append(shader)
	return shader


func _table_set(rd: RenderingDevice, p_shader: RID, p_table: RID) -> RID:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_BINDLESS_TABLE
	uniform.binding = 0
	uniform.add_id(p_table)
	return rd.uniform_set_create([uniform], p_shader, 1)


static func _uniform(p_type: RenderingDevice.UniformType, p_binding: int, p_rid: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = p_type
	uniform.binding = p_binding
	uniform.add_id(p_rid)
	return uniform


# A number in [0, 1) from two integers, the same on every platform.
static func _unit(p_a: int, p_b: int) -> float:
	return float(_hash3(p_a, p_b, 0x5254) & 0xffffff) / float(0x1000000)


# A 32-bit hash of three numbers (a multiply-xorshift mix), the same on every platform.
static func _hash3(p_a: int, p_b: int, p_c: int) -> int:
	var h := (p_a * 0x9e3779b1 + p_b * 0x85ebca77 + p_c * 0xc2b2ae3d) & MASK_32
	h = ((h ^ (h >> 15)) * 0x2c1b3c6d) & MASK_32
	h = ((h ^ (h >> 12)) * 0x297a2d39) & MASK_32
	return (h ^ (h >> 15)) & MASK_32


static func _digest(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
