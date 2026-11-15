extends CompositorEffect
## GT-6 load step burner (#190): one compute dispatch per frame over the internal render size,
## each invocation running [member iterations] rounds of an integer hash. It reads and writes no
## frame data, so the image is unchanged. Its cost scales with the internal pixel count, so dynamic
## resolution can win it back by lowering the scale. [code]tests/gpu_load_step.gd[/code]
## calibrates [member iterations] and switches [member CompositorEffect.enabled].

const SHADER := """#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) restrict writeonly buffer Sink {
	uint values[];
} sink;
layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint iterations;
	uint salt;
} params;
void main() {
	uvec2 p = gl_GlobalInvocationID.xy;
	if (p.x >= params.width || p.y >= params.height) {
		return;
	}
	uint h = p.x * 1973u + p.y * 9277u + params.salt;
	for (uint i = 0u; i < params.iterations; i++) {
		h ^= h >> 16;
		h *= 0x7feb352du;
		h ^= h >> 15;
		h *= 0x846ca68bu;
		h ^= h >> 16;
	}
	// Practically never true, but the compiler cannot know it, so the loop stays.
	if (h == 0x9e3779b9u) {
		sink.values[gl_LocalInvocationIndex] = h;
	}
}
"""
const SINK_WORDS := 64
const MAX_ITERATIONS := 1 << 16

var iterations := 0
## The internal size of the last dispatch, for the run's CSV.
var last_internal_size := Vector2i.ZERO
var dispatches := 0
var _shader := RID()
var _pipeline := RID()
var _sink := RID()
var _failed := false


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	enabled = false


func _ensure_pipeline(rd: RenderingDevice) -> bool:
	if _pipeline.is_valid():
		return true
	if _failed or rd == null:
		return false
	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = SHADER
	var spirv := rd.shader_compile_spirv_from_source(source)
	if spirv == null or not spirv.compile_error_compute.is_empty():
		push_error("gpu_load_step: burner shader compile failed: %s" % (spirv.compile_error_compute if spirv != null else "no SPIR-V"))
		_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "GpuLoadStepBurner")
	if _shader.is_valid():
		_pipeline = rd.compute_pipeline_create(_shader)
	if _pipeline.is_valid():
		var zeros := PackedByteArray()
		zeros.resize(SINK_WORDS * 4)
		_sink = rd.storage_buffer_create(zeros.size(), zeros)
	_failed = not _sink.is_valid()
	return not _failed


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	if iterations <= 0:
		return
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null:
		return
	var rd := RenderingServer.get_rendering_device()
	if not _ensure_pipeline(rd):
		return
	var size := buffers.get_internal_size()
	var sink_uniform := RDUniform.new()
	sink_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	sink_uniform.binding = 0
	sink_uniform.add_id(_sink)
	var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, [sink_uniform])
	if not uniform_set.is_valid():
		return
	var push := PackedInt32Array([size.x, size.y, mini(iterations, MAX_ITERATIONS), dispatches & 0xFFFF]).to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, _pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, int((size.x + 7) / 8), int((size.y + 7) / 8), 1)
	rd.compute_list_end()
	last_internal_size = size
	dispatches += 1


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	for rid in [_sink, _pipeline, _shader]:
		if rid.is_valid():
			rd.free_rid(rid)
