extends Node

## GPU-count indirect draw fixture (#177 step 6; plan §21.1, GR-6).
##
## On a local RenderingDevice, draws --records indirect records through
## draw_list_draw_indirect_count() while a compute pass writes the count (--count) into the count
## buffer just before, so the count is only known on the GPU. Record r draws 1 + r % 4 instances of a
## small triangle. Each instance covers the center of one pixel of its own, picked from the record (a
## per-vertex attribute) and the instance (a per-instance attribute), so every instance that runs
## gives exactly one fragment. Three render targets take the fragments: one counts them with additive
## blending, one keeps the record, the per-instance attribute, gl_InstanceIndex and gl_DrawID, and one
## checks the draw parameters gl_VertexIndex, gl_BaseVertex and gl_BaseInstance against the record.
## Instances that come from no record (a record read with the wrong stride, or a poison word) go to one
## error pixel. The read-back is compared with a CPU reference: records at or past the count give 0
## fragments, the others exactly one per instance. The fragment shader writes no storage buffer, so
## the check works the same under Direct3D 12 GPU-based validation.
##
## Cases: indexed and non-indexed records with strides of 20, 32 and 64 bytes, each with firstInstance
## 0 and, where the device reports SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE, 17; and two more counts with
## 32-byte indexed records: 0 and more than --records. Record r draws vertices 3r to 3r + 2: non-indexed
## with firstVertex 3r, indexed with firstIndex r and vertexOffset 2r (index i is i). The bytes between
## records and before the first one are poison: read as a record, they draw 7 instances, which no
## reference expects.
##
## Where the device reports SUPPORTS_BINDLESS, one more case (key "table_s32_f0_c<count>", 32-byte indexed
## records) draws with a second raster shader that also reads a mark per record through a storage-buffer
## bindless table. A compute pass writes the marks just before the draw list, so the table has not read
## them since that write and the draw list's end must give them their barrier. The mark check is the
## fourth lane of the draw parameter target, 1 when the mark is the record's, so this case's read-back is
## the same as the other cases' with firstInstance 0 and that count.
##
## gl_DrawID is checked on Vulkan only: Metal has no draw index, and Direct3D 12 has none at any shader
## model (it would need root constants in every record). The fixture compiles its raster shader with
## RDShaderSPIRV.base_instance_native where the device reports SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE, so
## that gl_InstanceIndex includes firstInstance on Direct3D 12 (shader model 6.8), and gl_VertexIndex
## and gl_BaseVertex the first vertex or vertex offset. The draw parameters are checked only there.
##
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure. "digests" holds, per
## case, the SHA-256 of the read-back without the gl_DrawID lane, which runs on different drivers must
## match; every stride and both record kinds give the same digest for the same firstInstance and count.
##
## User arguments: --records=N --count=N, and --require=<checks> (a comma-separated list of
## first_instance, draw_parameters, draw_id and table) to fail the run when one of them is "unsupported".

const SceneArgs = preload("res://scenes/scene_args.gd")

const DEFAULT_RECORDS := 4096
const DEFAULT_COUNT := 3001
const MAX_INSTANCES := 4
# Records per row of the render targets; each record has MAX_INSTANCES pixels side by side.
const RECORDS_PER_ROW := 64
const WIDTH := RECORDS_PER_ROW * MAX_INSTANCES
const STRIDES: Array[int] = [20, 32, 64]
const FIRST_INSTANCES: Array[int] = [0, 17]
# The records start this far into their buffer, and the count sits at this offset in its buffer.
const RECORDS_OFFSET := 64
const COUNT_OFFSET := 12
# Words that are not records or the count. Read as a record, a poison word draws 7 instances.
const POISON := 7
const COUNT_POISON := 0x7fffffff
const HALF_ONE := 0x3c00
const REQUIRE_CHECKS: Array[String] = ["first_instance", "draw_parameters", "draw_id", "table"]
# The mark of record r in the table case: r * MARK_SCALE + 1. Contents the write has not reached fail the check.
const MARK_SCALE := 7

const VERTEX_SOURCE := """#version 460
#define USE_DRAW_ID %d
#define CHECK_DRAW_PARAMETERS %d
#define USE_TABLE %d
#if USE_TABLE
#extension GL_EXT_nonuniform_qualifier : require
#endif

layout(location = 0) in uint vertex_value;
layout(location = 1) in uint instance_value;

layout(location = 0) flat out uvec4 ids;
layout(location = 1) flat out uvec4 draw_parameters;

layout(push_constant, std430) uniform Params {
	uint records;
	uint first_instance;
	uint width;
	uint height;
	uint indexed;
	uint mark_slot;
	uint pad1;
	uint pad2;
}
params;

#if USE_TABLE
// The marks, in slot mark_slot of a storage-buffer bindless table.
layout(set = 0, binding = 0, std430) restrict readonly buffer Marks {
	uint data[];
}
marks[];
#endif

void main() {
	uint record = vertex_value / 3u;
	uint corner = vertex_value %% 3u;
	uint instance = instance_value - params.first_instance;
	// The pixel of this instance, or the error pixel (the first one of the last row).
	uvec2 pixel = uvec2(0u, params.height - 1u);
	if (record < params.records && instance_value >= params.first_instance && instance < %du) {
		pixel = uvec2((record %% %du) * %du + instance, record / %du);
	}
	// A triangle around the pixel's center that reaches no other pixel's center.
	vec2 offset = corner == 0u ? vec2(-0.25, -0.25) : (corner == 1u ? vec2(0.5, -0.25) : vec2(-0.25, 0.5));
	vec2 position = (vec2(pixel) + 0.5 + offset) / vec2(params.width, params.height) * 2.0 - 1.0;
	uint draw_id = 0xffffffffu;
#if USE_DRAW_ID
	draw_id = uint(gl_DrawID);
#endif
	ids = uvec4(record, instance_value, uint(gl_InstanceIndex), draw_id);
	// 1 in each lane when gl_VertexIndex is the vertex, and gl_BaseVertex (vertexOffset 2r indexed,
	// firstVertex 3r otherwise) and gl_BaseInstance are the record's. The same for the three vertices.
	draw_parameters = uvec4(1u);
#if CHECK_DRAW_PARAMETERS
	uint base_vertex = params.indexed != 0u ? 2u * record : 3u * record;
	draw_parameters.x = uint(gl_VertexIndex) - vertex_value + 1u;
	draw_parameters.y = uint(gl_BaseVertex) - base_vertex + 1u;
	draw_parameters.z = uint(gl_BaseInstance) - params.first_instance + 1u;
#endif
#if USE_TABLE
	// 1 when the table gives the record's mark.
	draw_parameters.w = marks[params.mark_slot].data[record] - (record * %du + 1u) + 1u;
#endif
	gl_Position = vec4(position, 0.0, 1.0);
}
"""

const FRAGMENT_SOURCE := """#version 450

layout(location = 0) flat in uvec4 ids;
layout(location = 1) flat in uvec4 draw_parameters;

layout(location = 0) out float frag_count;
layout(location = 1) out uvec4 frag_ids;
layout(location = 2) out uvec4 frag_draw_parameters;

void main() {
	frag_count = 1.0;
	frag_ids = ids + uvec4(1u);
	frag_draw_parameters = draw_parameters;
}
"""

const COUNT_SOURCE := """#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Count {
	uint data[];
}
count_buffer;

layout(push_constant, std430) uniform Params {
	uint count;
	uint poison;
	uint pad0;
	uint pad1;
}
params;

void main() {
	count_buffer.data[0] = params.poison;
	count_buffer.data[1] = params.poison;
	count_buffer.data[2] = params.poison;
	count_buffer.data[3] = params.count;
}
"""

const MARK_SOURCE := """#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Marks {
	uint data[];
}
marks;

layout(push_constant, std430) uniform Params {
	uint entries;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i < params.entries) {
		marks.data[i] = i * %du + 1u;
	}
}
"""

var _failures: PackedStringArray = PackedStringArray()
# The marks buffer's slot in the table case's bindless table.
var _mark_slot := 0


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var params := {
		"records": clampi(SceneArgs.int_value("--records", DEFAULT_RECORDS), 1, 1 << 14),
		"count": maxi(0, SceneArgs.int_value("--count", DEFAULT_COUNT)),
		"require": _required_checks(),
	}
	params["height"] = ceili(params["records"] / float(RECORDS_PER_ROW)) + 1
	var result := {
		"test": "indirect_count",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"params": params,
	}

	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result

	var features := {
		"draw_indirect_count": rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT),
		"draw_indirect_first_instance": rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_FIRST_INSTANCE),
		"bindless": rd.has_feature(RenderingDevice.SUPPORTS_BINDLESS),
	}
	var use_draw_id: bool = result["driver"] == "vulkan"
	result["adapter"] = rd.get_device_name()
	result["features"] = features
	var checks := {
		"first_instance": "pass" if features["draw_indirect_first_instance"] else "unsupported",
		"draw_parameters": "pass" if features["draw_indirect_first_instance"] else "unsupported",
		"draw_id": "pass" if use_draw_id else "unsupported",
		"table": "pass" if features["bindless"] else "unsupported",
	}
	var cases := {}
	var digests := {}
	if not features["draw_indirect_count"]:
		result["reason"] = "NO_DRAW_INDIRECT_COUNT"
		_check(false, "the device reports SUPPORTS_DRAW_INDIRECT_COUNT")
	else:
		var resources := _create_resources(rd, params, use_draw_id, features["draw_indirect_first_instance"], features["bindless"])
		if not resources.is_empty():
			var references := {}
			for case_info in _cases(params, features["draw_indirect_first_instance"], features["bindless"]):
				var kind := "table" if case_info["table"] else ("indexed" if case_info["indexed"] else "plain")
				var key := "%s_s%d_f%d_c%d" % [kind, case_info["stride"], case_info["first_instance"], case_info["count"]]
				var reference_key := "f%d_c%d" % [case_info["first_instance"], case_info["count"]]
				if not references.has(reference_key):
					references[reference_key] = _make_reference(params, case_info["first_instance"], case_info["count"], use_draw_id)
				var status := _run_case(rd, params, resources, case_info, references[reference_key], key, digests)
				cases[key] = status
				if status != "pass" and case_info["first_instance"] != 0:
					checks["first_instance"] = "fail"
				if status == "fail_draw_id":
					checks["draw_id"] = "fail"
				if status == "fail_draw_parameters":
					checks["draw_parameters"] = "fail"
				if case_info["table"] and status != "pass":
					checks["table"] = "fail"
			_check_digests(digests)
		_free_resources(rd, resources)
	rd.free()

	for required in params["require"]:
		if not REQUIRE_CHECKS.has(required):
			_check(false, "--require names an unknown check '%s' (known: %s)" % [required, ", ".join(REQUIRE_CHECKS)])
			continue
		_check(checks[required] != "unsupported", "the required check %s ran (--require=%s)" % [required, required])

	result["checks"] = checks
	result["cases"] = cases
	result["digests"] = digests
	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


# The --require list, without empty entries.
static func _required_checks() -> PackedStringArray:
	var required := PackedStringArray()
	for item in SceneArgs.value("--require", "").split(","):
		if not item.strip_edges().is_empty():
			required.append(item.strip_edges())
	return required


# Every stride with both record kinds and both first instances at --count, then the counts 0 and
# past --records with 32-byte indexed records, then the table case.
static func _cases(params: Dictionary, first_instance_supported: bool, bindless_supported: bool) -> Array[Dictionary]:
	var cases: Array[Dictionary] = []
	for stride in STRIDES:
		for indexed in [true, false]:
			for first_instance in FIRST_INSTANCES:
				if first_instance != 0 and not first_instance_supported:
					continue
				cases.append({ "indexed": indexed, "stride": stride, "first_instance": first_instance, "count": params["count"], "table": false })
	for count in [0, params["records"] + 904]:
		cases.append({ "indexed": true, "stride": 32, "first_instance": 0, "count": count, "table": false })
	if bindless_supported:
		cases.append({ "indexed": true, "stride": 32, "first_instance": 0, "count": params["count"], "table": true })
	return cases


# The number of instances record r draws.
static func _instances(record: int) -> int:
	return 1 + record % MAX_INSTANCES


# The pixel index of instance k of record r.
static func _pixel(record: int, instance: int) -> int:
	return (record / RECORDS_PER_ROW) * WIDTH + (record % RECORDS_PER_ROW) * MAX_INSTANCES + instance


# The vertex, instance and index data cover any record a poison word or a wrong stride could make, so
# a broken driver gives wrong results instead of reading past a buffer.
static func _data_entries(records: int) -> int:
	return records * 6 + 256


func _create_resources(rd: RenderingDevice, params: Dictionary, use_draw_id: bool, base_instance_native: bool, use_table: bool) -> Dictionary:
	var records: int = params["records"]
	var resources := {}

	resources["raster_shader"] = _create_raster_shader(rd, use_draw_id, base_instance_native, false)
	if use_table:
		resources["table_raster_shader"] = _create_raster_shader(rd, use_draw_id, base_instance_native, true)

	var count_source := RDShaderSource.new()
	count_source.source_compute = COUNT_SOURCE
	var count_spirv := rd.shader_compile_spirv_from_source(count_source)
	_check(count_spirv.compile_error_compute.is_empty(), "the count shader compiles: %s" % count_spirv.compile_error_compute)
	resources["count_shader"] = rd.shader_create_from_spirv(count_spirv, "indirect_count_count")
	if not _valid(resources, "raster_shader") or not _valid(resources, "count_shader"):
		_check(false, "the shaders are created")
		_free_resources(rd, resources)
		return {}
	resources["count_pipeline"] = rd.compute_pipeline_create(resources["count_shader"])
	if use_table:
		var mark_source := RDShaderSource.new()
		mark_source.source_compute = MARK_SOURCE % MARK_SCALE
		var mark_spirv := rd.shader_compile_spirv_from_source(mark_source)
		_check(mark_spirv.compile_error_compute.is_empty(), "the mark shader compiles: %s" % mark_spirv.compile_error_compute)
		resources["mark_shader"] = rd.shader_create_from_spirv(mark_spirv, "indirect_count_mark")
		if not _valid(resources, "table_raster_shader") or not _valid(resources, "mark_shader"):
			_check(false, "the table case's shaders are created")
			_free_resources(rd, resources)
			return {}
		resources["mark_pipeline"] = rd.compute_pipeline_create(resources["mark_shader"])

	# Vertex v holds v (record v / 3, corner v % 3); instance i holds i; index i is i.
	var entries := _data_entries(records)
	var values := PackedInt32Array()
	values.resize(entries)
	for i in entries:
		values[i] = i
	var value_bytes := values.to_byte_array()
	resources["vertices"] = rd.vertex_buffer_create(value_bytes.size(), value_bytes)
	resources["instances"] = rd.vertex_buffer_create(value_bytes.size(), value_bytes)
	resources["indices"] = rd.index_buffer_create(entries, RenderingDevice.INDEX_BUFFER_FORMAT_UINT32, value_bytes)
	var vertex_attribute := RDVertexAttribute.new()
	vertex_attribute.location = 0
	vertex_attribute.format = RenderingDevice.DATA_FORMAT_R32_UINT
	vertex_attribute.stride = 4
	vertex_attribute.frequency = RenderingDevice.VERTEX_FREQUENCY_VERTEX
	var instance_attribute := RDVertexAttribute.new()
	instance_attribute.location = 1
	instance_attribute.format = RenderingDevice.DATA_FORMAT_R32_UINT
	instance_attribute.stride = 4
	instance_attribute.frequency = RenderingDevice.VERTEX_FREQUENCY_INSTANCE
	var attributes: Array[RDVertexAttribute] = [vertex_attribute, instance_attribute]
	var vertex_format := rd.vertex_format_create(attributes)
	var vertex_buffers: Array[RID] = [resources["vertices"], resources["instances"]]
	resources["vertex_array"] = rd.vertex_array_create(entries, vertex_format, vertex_buffers)
	resources["index_array"] = rd.index_array_create(resources["indices"], 0, entries)

	resources["count"] = rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)
	var count_uniforms: Array[RDUniform] = [_buffer_uniform(0, resources["count"])]
	resources["count_uniforms"] = rd.uniform_set_create(count_uniforms, resources["count_shader"], 0)

	# The table case: a marks buffer the mark shader writes, in one slot of a storage-buffer table.
	if use_table:
		resources["marks"] = rd.storage_buffer_create(entries * 4)
		var mark_uniforms: Array[RDUniform] = [_buffer_uniform(0, resources["marks"])]
		resources["mark_uniforms"] = rd.uniform_set_create(mark_uniforms, resources["mark_shader"], 0)
		resources["table"] = rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 4)
		if _valid(resources, "table") and _valid(resources, "marks"):
			_mark_slot = rd.bindless_table_allocate(resources["table"], resources["marks"])
			_check(_mark_slot >= 0 and _mark_slot < 4, "the marks buffer gets a slot of the table (%d)" % _mark_slot)
			var table_uniform := RDUniform.new()
			table_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_BINDLESS_TABLE
			table_uniform.binding = 0
			table_uniform.add_id(resources["table"])
			resources["table_uniforms"] = rd.uniform_set_create([table_uniform], resources["table_raster_shader"], 0)

	# The fragment count per pixel (additive), and the values and draw parameter checks of its fragment.
	resources["counts"] = _target(rd, params, RenderingDevice.DATA_FORMAT_R16_SFLOAT)
	resources["ids"] = _target(rd, params, RenderingDevice.DATA_FORMAT_R32G32B32A32_UINT)
	resources["draw_parameters"] = _target(rd, params, RenderingDevice.DATA_FORMAT_R32G32B32A32_UINT)
	var framebuffer_textures: Array[RID] = [resources["counts"], resources["ids"], resources["draw_parameters"]]
	resources["framebuffer"] = rd.framebuffer_create(framebuffer_textures)
	if _valid(resources, "framebuffer"):
		var add := RDPipelineColorBlendStateAttachment.new()
		add.enable_blend = true
		add.src_color_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.dst_color_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.color_blend_op = RenderingDevice.BLEND_OP_ADD
		add.src_alpha_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.dst_alpha_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.alpha_blend_op = RenderingDevice.BLEND_OP_ADD
		var blend := RDPipelineColorBlendState.new()
		var blend_attachments: Array[RDPipelineColorBlendStateAttachment] = [add, RDPipelineColorBlendStateAttachment.new(), RDPipelineColorBlendStateAttachment.new()]
		blend.attachments = blend_attachments
		resources["raster_pipeline"] = rd.render_pipeline_create(resources["raster_shader"], rd.framebuffer_get_format(resources["framebuffer"]), vertex_format, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)
		if use_table:
			resources["table_raster_pipeline"] = rd.render_pipeline_create(resources["table_raster_shader"], rd.framebuffer_get_format(resources["framebuffer"]), vertex_format, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)

	# One record buffer per stride, record kind and first instance.
	for stride in STRIDES:
		for indexed in [true, false]:
			for first_instance in FIRST_INSTANCES:
				var bytes := _make_records(records, indexed, stride, first_instance)
				resources[_records_key(indexed, stride, first_instance)] = rd.storage_buffer_create(bytes.size(), bytes, RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)

	for key in resources:
		if not _valid(resources, key):
			_check(false, "%s is created" % key)
			_free_resources(rd, resources)
			return {}
	return resources


# The raster shader, with the table read (the table case) or without.
func _create_raster_shader(rd: RenderingDevice, use_draw_id: bool, base_instance_native: bool, use_table: bool) -> RID:
	var raster_source := RDShaderSource.new()
	raster_source.source_vertex = VERTEX_SOURCE % [1 if use_draw_id else 0, 1 if base_instance_native else 0, 1 if use_table else 0, MAX_INSTANCES, RECORDS_PER_ROW, MAX_INSTANCES, RECORDS_PER_ROW, MARK_SCALE]
	raster_source.source_fragment = FRAGMENT_SOURCE
	var raster_spirv := rd.shader_compile_spirv_from_source(raster_source)
	_check(raster_spirv.compile_error_vertex.is_empty() and raster_spirv.compile_error_fragment.is_empty(), "the raster shader compiles (table %s): %s %s" % [use_table, raster_spirv.compile_error_vertex, raster_spirv.compile_error_fragment])
	raster_spirv.base_instance_native = base_instance_native
	return rd.shader_create_from_spirv(raster_spirv, "indirect_count_table" if use_table else "indirect_count_raster")


func _target(rd: RenderingDevice, params: Dictionary, format_id: RenderingDevice.DataFormat) -> RID:
	var format := RDTextureFormat.new()
	format.format = format_id
	format.width = WIDTH
	format.height = params["height"]
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	return rd.texture_create(format, RDTextureView.new())


static func _records_key(indexed: bool, stride: int, first_instance: int) -> String:
	return "records_%s_%d_%d" % ["indexed" if indexed else "plain", stride, first_instance]


# RECORDS_OFFSET poison bytes, then the records, stride bytes apart with poison between them, then
# poison past the end. Indexed: index count, instance count, first index, vertex offset, first
# instance. Not indexed: vertex count, instance count, first vertex, first instance. Either way,
# record r draws vertices 3r to 3r + 2 (indexed: indices r to r + 2, which are r to r + 2, plus 2r).
static func _make_records(records: int, indexed: bool, stride: int, first_instance: int) -> PackedByteArray:
	var words := PackedInt32Array()
	words.resize((RECORDS_OFFSET + records * stride + 64) / 4)
	words.fill(POISON)
	for r in records:
		var base := (RECORDS_OFFSET + r * stride) / 4
		words[base] = 3
		words[base + 1] = _instances(r)
		if indexed:
			words[base + 2] = r
			words[base + 3] = r * 2
			words[base + 4] = first_instance
		else:
			words[base + 2] = r * 3
			words[base + 3] = first_instance
	return words.to_byte_array()


# The render targets as the GPU leaves them for one case: one fragment (a half-float 1), its values
# plus 1 and four 1s for its draw parameters and mark in each pixel of an instance that runs, 0 everywhere
# else, the error pixel included.
static func _make_reference(params: Dictionary, first_instance: int, count: int, use_draw_id: bool) -> Dictionary:
	var records: int = params["records"]
	var pixels: int = WIDTH * params["height"]
	var drawn := mini(count, records)
	var counts := PackedByteArray()
	counts.resize(pixels * 2)
	counts.fill(0)
	var ids := PackedInt32Array()
	ids.resize(pixels * 4)
	ids.fill(0)
	var draw_parameters := PackedInt32Array()
	draw_parameters.resize(pixels * 4)
	draw_parameters.fill(0)
	for r in drawn:
		for k in _instances(r):
			var pixel := _pixel(r, k)
			counts.encode_u16(pixel * 2, HALF_ONE)
			ids[pixel * 4] = r + 1
			ids[pixel * 4 + 1] = first_instance + k + 1
			ids[pixel * 4 + 2] = first_instance + k + 1
			ids[pixel * 4 + 3] = r + 1 if use_draw_id else 0
			for lane in 4:
				draw_parameters[pixel * 4 + lane] = 1
	return { "counts": counts, "ids": ids.to_byte_array(), "draw_parameters": draw_parameters.to_byte_array(), "drawn": drawn }


# Writes the count on the GPU and draws the records. Returns "pass", or why it failed.
func _run_case(rd: RenderingDevice, params: Dictionary, resources: Dictionary, case_info: Dictionary, reference: Dictionary, key: String, digests: Dictionary) -> String:
	var records: int = params["records"]
	var count_push := PackedInt32Array([case_info["count"], COUNT_POISON, 0, 0]).to_byte_array()
	var compute_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, resources["count_pipeline"])
	rd.compute_list_bind_uniform_set(compute_list, resources["count_uniforms"], 0)
	rd.compute_list_set_push_constant(compute_list, count_push, count_push.size())
	rd.compute_list_dispatch(compute_list, 1, 1, 1)
	rd.compute_list_end()
	if case_info["table"]:
		# The marks are written just before the draw list, so its table read needs their barrier.
		var entries := _data_entries(records)
		var mark_push := PackedInt32Array([entries, 0, 0, 0]).to_byte_array()
		compute_list = rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, resources["mark_pipeline"])
		rd.compute_list_bind_uniform_set(compute_list, resources["mark_uniforms"], 0)
		rd.compute_list_set_push_constant(compute_list, mark_push, mark_push.size())
		rd.compute_list_dispatch(compute_list, ceili(entries / 64.0), 1, 1)
		rd.compute_list_end()

	var raster_push := PackedInt32Array([records, case_info["first_instance"], WIDTH, params["height"], 1 if case_info["indexed"] else 0, _mark_slot, 0, 0]).to_byte_array()
	var draw_list := rd.draw_list_begin(resources["framebuffer"], RenderingDevice.DRAW_CLEAR_COLOR_ALL, PackedColorArray([Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0)]))
	rd.draw_list_bind_render_pipeline(draw_list, resources["table_raster_pipeline" if case_info["table"] else "raster_pipeline"])
	if case_info["table"]:
		rd.draw_list_bind_uniform_set(draw_list, resources["table_uniforms"], 0)
	rd.draw_list_bind_vertex_array(draw_list, resources["vertex_array"])
	if case_info["indexed"]:
		rd.draw_list_bind_index_array(draw_list, resources["index_array"])
	rd.draw_list_set_push_constant(draw_list, raster_push, raster_push.size())
	rd.draw_list_draw_indirect_count(draw_list, case_info["indexed"], resources[_records_key(case_info["indexed"], case_info["stride"], case_info["first_instance"])], RECORDS_OFFSET, resources["count"], COUNT_OFFSET, records, case_info["stride"])
	rd.draw_list_end()
	rd.submit()
	rd.sync()

	var counts := rd.texture_get_data(resources["counts"], 0)
	var ids := rd.texture_get_data(resources["ids"], 0)
	var draw_parameters := rd.texture_get_data(resources["draw_parameters"], 0)
	digests[key] = _digest(counts + _without_draw_id(ids) + draw_parameters)

	var error_pixel: int = WIDTH * (params["height"] - 1)
	if counts.decode_u16(error_pixel * 2) != 0:
		_check(false, "%s: %s fragments came from no record (a record read with the wrong stride, or a poison word)" % [key, counts.decode_half(error_pixel * 2)])
		return "fail"
	if counts != reference["counts"]:
		var wrong_records := {}
		var past_count := 0
		var first := -1
		for pixel in range(0, counts.size() / 2):
			if counts.decode_u16(pixel * 2) != reference["counts"].decode_u16(pixel * 2):
				var record: int = (pixel / WIDTH) * RECORDS_PER_ROW + (pixel % WIDTH) / MAX_INSTANCES
				if not wrong_records.has(record):
					wrong_records[record] = true
					if record >= reference["drawn"]:
						past_count += 1
				if first < 0:
					first = pixel
		_check(false, "%s: %d records drew the wrong fragments, %d of them at or past the count %d; first at pixel %d: %s fragments, expected %s" % [key, wrong_records.size(), past_count, case_info["count"], first, counts.decode_half(first * 2), reference["counts"].decode_half(first * 2)])
		return "fail"
	if ids != reference["ids"]:
		var lanes := PackedInt32Array([0, 0, 0, 0])
		var first := -1
		for i in range(0, ids.size(), 4):
			if ids.decode_u32(i) != reference["ids"].decode_u32(i):
				lanes[(i / 4) % 4] += 1
				if first < 0:
					first = i / 16
		_check(false, "%s: wrong values: %d records, %d instance attributes, %d gl_InstanceIndex, %d gl_DrawID; first at pixel %d: %s, expected %s" % [key, lanes[0], lanes[1], lanes[2], lanes[3], first, ids.slice(first * 16, first * 16 + 16).to_int32_array(), reference["ids"].slice(first * 16, first * 16 + 16).to_int32_array()])
		return "fail_draw_id" if lanes[0] == 0 and lanes[1] == 0 and lanes[2] == 0 else "fail"
	if draw_parameters != reference["draw_parameters"]:
		var lanes := PackedInt32Array([0, 0, 0, 0])
		var first := -1
		for i in range(0, draw_parameters.size(), 4):
			if draw_parameters.decode_u32(i) != reference["draw_parameters"].decode_u32(i):
				lanes[(i / 4) % 4] += 1
				if first < 0:
					first = i / 16
		# A lane holds the value minus the expected value, plus 1.
		_check(false, "%s: wrong draw parameters: %d gl_VertexIndex, %d gl_BaseVertex, %d gl_BaseInstance, %d table marks; first at pixel %d: %s, expected %s" % [key, lanes[0], lanes[1], lanes[2], lanes[3], first, draw_parameters.slice(first * 16, first * 16 + 16).to_int32_array(), reference["draw_parameters"].slice(first * 16, first * 16 + 16).to_int32_array()])
		return "fail_table" if lanes[0] == 0 and lanes[1] == 0 and lanes[2] == 0 else "fail_draw_parameters"
	return "pass"


# The values with the gl_DrawID lane cleared, so drivers without draw IDs can be compared.
static func _without_draw_id(ids: PackedByteArray) -> PackedByteArray:
	var words := ids.to_int32_array()
	for i in range(3, words.size(), 4):
		words[i] = 0
	return words.to_byte_array()


# Cases that differ only in stride and record kind must give the same read-back.
func _check_digests(digests: Dictionary) -> void:
	var by_result := {}
	for key: String in digests:
		var parts := key.split("_")
		var result_key := parts[2] + "_" + parts[3]
		if not by_result.has(result_key):
			by_result[result_key] = digests[key]
		elif by_result[result_key] != digests[key]:
			_check(false, "%s differs from the other cases with %s" % [key, result_key])


func _free_resources(rd: RenderingDevice, resources: Dictionary) -> void:
	# Dependents first: pipelines, uniform sets, arrays and the framebuffer before what they use.
	for key in ["raster_pipeline", "table_raster_pipeline", "count_pipeline", "mark_pipeline", "count_uniforms", "mark_uniforms", "table_uniforms", "table", "vertex_array", "index_array", "framebuffer"]:
		if _valid(resources, key):
			rd.free_rid(resources[key])
		resources.erase(key)
	for key in resources:
		if _valid(resources, key):
			rd.free_rid(resources[key])
	resources.clear()


static func _valid(resources: Dictionary, key: String) -> bool:
	return resources.has(key) and resources[key] is RID and (resources[key] as RID).is_valid()


func _buffer_uniform(binding: int, buffer: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


static func _digest(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
