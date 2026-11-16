extends CompositorEffect
## GPU readback for the hybrid HW/SW raster check (#162). On a frame the check arms, copies every
## internal pixel's NextGeo instance ID, visibility ID, depth bits and velocity into one buffer and
## reads it back. Sets needs_motion_vectors so the depth pre-pass and NextGeo write velocity without
## TAA, which would add a per-frame jitter.

const PROBE_GLSL := """#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r32ui, set = 0, binding = 0) uniform readonly uimage2D vis_img;
layout(r32ui, set = 0, binding = 1) uniform readonly uimage2D inst_img;
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(rg16f, set = 0, binding = 3) uniform readonly image2D vel_img;

layout(std430, set = 0, binding = 4) buffer restrict writeonly ProbeOut {
	uint data[];
} out_buf;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 pad;
} params;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= params.size.x || p.y >= params.size.y) {
		return;
	}
	uint base = (uint(p.y) * uint(params.size.x) + uint(p.x)) * 5u;
	vec2 velocity = imageLoad(vel_img, p).xy;
	out_buf.data[base + 0u] = imageLoad(inst_img, p).r;
	out_buf.data[base + 1u] = imageLoad(vis_img, p).r;
	out_buf.data[base + 2u] = floatBitsToUint(texelFetch(depth_tex, p, 0).r);
	out_buf.data[base + 3u] = floatBitsToUint(velocity.x);
	out_buf.data[base + 4u] = floatBitsToUint(velocity.y);
}
"""

const WORDS_PER_PIXEL := 5

## Set by the check for one frame; the next render callback fills `capture` and clears it.
var capture_armed := false
## { "size": Vector2i, "words": PackedInt32Array } of the last armed frame.
var capture: Dictionary = {}

var _shader := RID()
var _pipeline := RID()
var _sampler := RID()
var _out_buffer := RID()
var _out_bytes := 0
var _failed := false


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	needs_motion_vectors = true


func _ensure(rd: RenderingDevice) -> bool:
	if _pipeline.is_valid():
		return true
	if _failed or rd == null:
		return false
	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = PROBE_GLSL
	var spirv := rd.shader_compile_spirv_from_source(src)
	if spirv == null or not spirv.compile_error_compute.is_empty():
		push_error("NextGeoSwRasterProbe shader compile error: %s" % (spirv.compile_error_compute if spirv != null else "null"))
		_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "NextGeoSwRasterProbe")
	if not _shader.is_valid():
		_failed = true
		return false
	_pipeline = rd.compute_pipeline_create(_shader)
	var sampler_state := RDSamplerState.new()
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	_sampler = rd.sampler_create(sampler_state)
	_failed = not _pipeline.is_valid() or not _sampler.is_valid()
	return not _failed


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	if not capture_armed:
		return
	capture_armed = false
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null or not buffers.has_texture(&"nextgeo", &"visibility") or not buffers.has_texture(&"nextgeo", &"instance_id"):
		return
	var velocity := buffers.get_velocity_layer(0, false)
	if not velocity.is_valid():
		return
	var rd := RenderingServer.get_rendering_device()
	if not _ensure(rd):
		return
	var size := buffers.get_internal_size()
	var bytes := size.x * size.y * WORDS_PER_PIXEL * 4
	if bytes != _out_bytes:
		if _out_buffer.is_valid():
			rd.free_rid(_out_buffer)
		_out_buffer = rd.storage_buffer_create(bytes)
		_out_bytes = bytes

	var uniforms: Array[RDUniform] = []
	var textures := [buffers.get_texture(&"nextgeo", &"visibility"), buffers.get_texture(&"nextgeo", &"instance_id")]
	for i in 2:
		var u := RDUniform.new()
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		u.binding = i
		u.add_id(textures[i])
		uniforms.append(u)
	var u_depth := RDUniform.new()
	u_depth.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	u_depth.binding = 2
	u_depth.add_id(_sampler)
	u_depth.add_id(buffers.get_depth_layer(0, false))
	uniforms.append(u_depth)
	var u_velocity := RDUniform.new()
	u_velocity.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u_velocity.binding = 3
	u_velocity.add_id(velocity)
	uniforms.append(u_velocity)
	var u_out := RDUniform.new()
	u_out.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u_out.binding = 4
	u_out.add_id(_out_buffer)
	uniforms.append(u_out)
	var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, uniforms)
	if not uniform_set.is_valid():
		return

	var push := PackedInt32Array([size.x, size.y, 0, 0]).to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, _pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, int((size.x + 7) / 8.0), int((size.y + 7) / 8.0), 1)
	rd.compute_list_end()
	capture = { "size": size, "words": rd.buffer_get_data(_out_buffer).to_int32_array() }


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	for rid in [_out_buffer, _pipeline, _sampler, _shader]:
		if rid.is_valid():
			rd.free_rid(rid)
