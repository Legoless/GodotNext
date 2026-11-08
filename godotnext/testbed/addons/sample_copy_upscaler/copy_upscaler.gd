extends TemporalUpscalerExtension
## Test-only sample custom upscaler (#192, plan §21.5 U2, GT-1). Testbed
## only: it never ships in the engine, and it is not a vendor upscaler.
##
## A bilinear compute copy from the internal color texture to the upscaled
## output, recorded through RenderingServer.get_rendering_device() on the
## render thread (the CompositorEffect contract). The counters let the
## custom_upscaler test scene check dispatch, resets and RID lifetime.

const UPSCALER_NAME := "SampleCopy"

const SHADER_SOURCE := """
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D destination_color;

layout(push_constant, std430) uniform Params {
	uvec2 output_size;
	uvec2 pad;
}
params;

void main() {
	if (any(greaterThanEqual(gl_GlobalInvocationID.xy, params.output_size))) {
		return;
	}
	vec2 uv = (vec2(gl_GlobalInvocationID.xy) + 0.5) / vec2(params.output_size);
	imageStore(destination_color, ivec2(gl_GlobalInvocationID.xy), textureLod(source_color, uv, 0.0));
}
"""

## Live shader RIDs across every SampleCopy instance; 0 once all are freed.
static var live_shaders := 0

var available := true
var scale_range := Vector2(0.25, 2.0)
var mip_bias_offset := -0.5
## -1 keeps the engine's default jitter phase count.
var jitter_phases := -1

var process_count := 0
var reset_count := 0
## Values from the last _process call; the inputs object itself is not kept.
var last := {}

var _shader := RID()
var _pipeline := RID()
var _sampler := RID()
var _init_failed := false


func _get_name() -> String:
	return UPSCALER_NAME


func _is_available() -> bool:
	return available


func _get_scale_range() -> Vector2:
	return scale_range


func _get_mip_bias_offset() -> float:
	return mip_bias_offset


func _get_jitter_phase_count(_target_size: Vector2i, _render_size: Vector2i) -> int:
	return jitter_phases


func _reset_history() -> void:
	reset_count += 1


func _process(inputs: TemporalUpscalerInputs) -> void:
	var rd := RenderingServer.get_rendering_device()
	if not _ensure_pipeline(rd):
		return
	if not inputs.color.is_valid() or not inputs.output.is_valid():
		push_error("SampleCopy: missing color or output texture")
		return
	var output_size := inputs.output_size
	var internal_size := inputs.internal_size
	var source := RDUniform.new()
	source.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	source.binding = 0
	source.add_id(_sampler)
	source.add_id(inputs.color)
	var destination := RDUniform.new()
	destination.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	destination.binding = 1
	destination.add_id(inputs.output)
	var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, [source, destination])
	if not uniform_set.is_valid():
		push_error("SampleCopy: uniform set creation failed")
		return
	var push := PackedByteArray()
	push.resize(16)
	push.encode_u32(0, output_size.x)
	push.encode_u32(4, output_size.y)
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, uniform_set, 0)
	rd.compute_list_set_push_constant(compute, push, push.size())
	rd.compute_list_dispatch(compute, int((output_size.x + 7) / 8), int((output_size.y + 7) / 8), 1)
	rd.compute_list_end()
	process_count += 1
	last = {
		"output_size": output_size,
		"internal_size": internal_size,
		"max_internal_size": inputs.max_internal_size,
		"jitter": inputs.jitter,
		"view_index": inputs.view_index,
		"has_depth_and_velocity": inputs.depth.is_valid() and inputs.velocity.is_valid(),
		"has_render_buffers": inputs.render_scene_buffers is RenderSceneBuffersRD,
	}


func _ensure_pipeline(rd: RenderingDevice) -> bool:
	if _pipeline.is_valid():
		return true
	if _init_failed or rd == null:
		return false
	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = SHADER_SOURCE
	var spirv := rd.shader_compile_spirv_from_source(src)
	if spirv == null or not spirv.compile_error_compute.is_empty():
		push_error("SampleCopy: shader compile failed: %s" % (spirv.compile_error_compute if spirv != null else "null SPIR-V"))
		_init_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "SampleCopyUpscaler")
	if not _shader.is_valid():
		_init_failed = true
		return false
	live_shaders += 1
	_pipeline = rd.compute_pipeline_create(_shader)
	var sampler_state := RDSamplerState.new()
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_sampler = rd.sampler_create(sampler_state)
	if not _pipeline.is_valid() or not _sampler.is_valid():
		_init_failed = true
		return false
	return true


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	if _sampler.is_valid():
		rd.free_rid(_sampler)
	# Freeing the shader also frees its pipeline.
	if _shader.is_valid():
		rd.free_rid(_shader)
		live_shaders -= 1
	_sampler = RID()
	_pipeline = RID()
	_shader = RID()
