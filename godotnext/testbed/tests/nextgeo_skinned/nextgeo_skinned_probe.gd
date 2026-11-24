extends CompositorEffect
## GPU readback probe for NextGeo skinned runtime velocity check (#160).
## Captures NextGeo instance_id (r32ui) and motion velocity (rg16f) after the opaque pass.

const PROBE_GLSL := """#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r32ui, set = 0, binding = 0) uniform readonly uimage2D inst_img;
layout(rg16f, set = 0, binding = 1) uniform readonly image2D vel_img;

layout(std430, set = 0, binding = 2) buffer restrict writeonly ProbeOut {
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
	uint base = (uint(p.y) * uint(params.size.x) + uint(p.x)) * 3u;
	vec2 velocity = all(lessThan(p, imageSize(vel_img))) ? imageLoad(vel_img, p).xy : vec2(0.0);
	out_buf.data[base + 0u] = imageLoad(inst_img, p).r;
	out_buf.data[base + 1u] = floatBitsToUint(velocity.x);
	out_buf.data[base + 2u] = floatBitsToUint(velocity.y);
}
"""

const WORDS_PER_PIXEL := 3

var capture_armed := false
var capture: Dictionary = {}

var _shader := RID()
var _pipeline := RID()
var _dummy_rg := RID()
var _out_buffer := RID()
var _out_bytes := 0
var _failed := false


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE
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
		push_error("NextGeoSkinnedProbe shader compile error: %s" % (spirv.compile_error_compute if spirv != null else "null"))
		_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "NextGeoSkinnedProbe")
	if not _shader.is_valid():
		_failed = true
		return false
	_pipeline = rd.compute_pipeline_create(_shader)
	var tf := RDTextureFormat.new()
	tf.format = RenderingDevice.DATA_FORMAT_R16G16_SFLOAT
	tf.width = 1
	tf.height = 1
	tf.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	_dummy_rg = rd.texture_create(tf, RDTextureView.new())
	_failed = not _pipeline.is_valid() or not _dummy_rg.is_valid()
	return not _failed


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	if not capture_armed:
		return
	capture_armed = false
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null or not buffers.has_texture(&"nextgeo", &"instance_id"):
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

	var velocity := buffers.get_velocity_layer(0, false) if needs_motion_vectors else RID()
	var uniforms: Array[RDUniform] = []
	var u0 := RDUniform.new()
	u0.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u0.binding = 0
	u0.add_id(buffers.get_texture(&"nextgeo", &"instance_id"))
	uniforms.append(u0)
	var u1 := RDUniform.new()
	u1.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u1.binding = 1
	u1.add_id(velocity if velocity.is_valid() else _dummy_rg)
	uniforms.append(u1)
	var u2 := RDUniform.new()
	u2.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u2.binding = 2
	u2.add_id(_out_buffer)
	uniforms.append(u2)
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
	capture = {
		"size": size,
		"words": rd.buffer_get_data(_out_buffer).to_int32_array(),
		"velocity": velocity.is_valid(),
	}


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	for rid in [_out_buffer, _pipeline, _dummy_rg, _shader]:
		if rid.is_valid():
			rd.free_rid(rid)
