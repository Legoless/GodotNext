extends CompositorEffect
## GPU readback for the material shading bins check (#158). After the opaque pass of a frame the check
## arms, copies every internal pixel's NextGeo instance ID, opaque colour, normal-roughness, velocity,
## separate specular, MegaLights albedo and material inputs and voxel GI instance pair into one buffer and
## reads it back. needs_motion_vectors and needs_separate_specular are set by the check for its phases, so
## Stage B runs with and without velocity and separate specular; read_megalights and read_voxel_gi say
## when Stage A writes those targets.

const PROBE_GLSL := """#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r32ui, set = 0, binding = 0) uniform readonly uimage2D inst_img;
layout(rgba16f, set = 0, binding = 1) uniform readonly image2D color_img;
layout(rgba8, set = 0, binding = 2) uniform readonly image2D nr_img;
layout(rg16f, set = 0, binding = 3) uniform readonly image2D vel_img;
layout(rgba16f, set = 0, binding = 4) uniform readonly image2D spec_img;
layout(rgba8, set = 0, binding = 6) uniform readonly image2D ml_albedo_img;
layout(rgba8, set = 0, binding = 7) uniform readonly image2D ml_material_img;
layout(rg8ui, set = 0, binding = 8) uniform readonly uimage2D voxel_gi_img;

layout(std430, set = 0, binding = 5) buffer restrict writeonly ProbeOut {
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
	uint base = (uint(p.y) * uint(params.size.x) + uint(p.x)) * 15u;
	vec4 color = imageLoad(color_img, p);
	vec4 nr = imageLoad(nr_img, p);
	vec2 velocity = all(lessThan(p, imageSize(vel_img))) ? imageLoad(vel_img, p).xy : vec2(0.0);
	vec4 specular = all(lessThan(p, imageSize(spec_img))) ? imageLoad(spec_img, p) : vec4(0.0);
	vec4 ml_albedo = all(lessThan(p, imageSize(ml_albedo_img))) ? imageLoad(ml_albedo_img, p) : vec4(0.0);
	vec4 ml_material = all(lessThan(p, imageSize(ml_material_img))) ? imageLoad(ml_material_img, p) : vec4(0.0);
	uvec2 voxel_gi = all(lessThan(p, imageSize(voxel_gi_img))) ? imageLoad(voxel_gi_img, p).xy : uvec2(0u);
	out_buf.data[base + 0u] = imageLoad(inst_img, p).r;
	out_buf.data[base + 1u] = floatBitsToUint(color.r);
	out_buf.data[base + 2u] = floatBitsToUint(color.g);
	out_buf.data[base + 3u] = floatBitsToUint(color.b);
	out_buf.data[base + 4u] = floatBitsToUint(color.a);
	out_buf.data[base + 5u] = packUnorm4x8(nr);
	out_buf.data[base + 6u] = floatBitsToUint(velocity.x);
	out_buf.data[base + 7u] = floatBitsToUint(velocity.y);
	out_buf.data[base + 8u] = floatBitsToUint(specular.r);
	out_buf.data[base + 9u] = floatBitsToUint(specular.g);
	out_buf.data[base + 10u] = floatBitsToUint(specular.b);
	out_buf.data[base + 11u] = floatBitsToUint(specular.a);
	out_buf.data[base + 12u] = packUnorm4x8(ml_albedo);
	out_buf.data[base + 13u] = packUnorm4x8(ml_material);
	out_buf.data[base + 14u] = (voxel_gi.x & 0xFFu) | ((voxel_gi.y & 0xFFu) << 8u);
}
"""

const WORDS_PER_PIXEL := 15
# The words of the MegaLights albedo and material inputs (RGBA8 each) and of the voxel GI pair (RG8UI).
const WORD_ML_ALBEDO := 12
const WORD_ML_MATERIAL := 13
const WORD_VOXEL_GI := 14

## Set by the check: whether to read the MegaLights inputs and the voxel GI target, which outlive the
## frames that write them.
var read_megalights := false
var read_voxel_gi := false

## Set by the check for one frame; the next render callback fills `capture` and clears it.
var capture_armed := false
## { "size": Vector2i, "words": PackedInt32Array, "velocity": bool, "specular": bool, "megalights": bool,
## "voxel_gi": bool } of the last armed frame.
var capture: Dictionary = {}

var _shader := RID()
var _pipeline := RID()
var _dummy_rg := RID()
var _dummy_rgba := RID()
var _dummy_rgba8 := RID()
var _dummy_rg8ui := RID()
var _out_buffer := RID()
var _out_bytes := 0
var _failed := false


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE
	needs_motion_vectors = false


func _dummy(rd: RenderingDevice, p_format: int) -> RID:
	var tf := RDTextureFormat.new()
	tf.format = p_format
	tf.width = 1
	tf.height = 1
	tf.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	return rd.texture_create(tf, RDTextureView.new())


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
		push_error("NextGeoShadingBinsProbe shader compile error: %s" % (spirv.compile_error_compute if spirv != null else "null"))
		_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "NextGeoShadingBinsProbe")
	if not _shader.is_valid():
		_failed = true
		return false
	_pipeline = rd.compute_pipeline_create(_shader)
	_dummy_rg = _dummy(rd, RenderingDevice.DATA_FORMAT_R16G16_SFLOAT)
	_dummy_rgba = _dummy(rd, RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT)
	_dummy_rgba8 = _dummy(rd, RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM)
	_dummy_rg8ui = _dummy(rd, RenderingDevice.DATA_FORMAT_R8G8_UINT)
	_failed = not _pipeline.is_valid() or not _dummy_rg.is_valid() or not _dummy_rgba.is_valid() or not _dummy_rgba8.is_valid() or not _dummy_rg8ui.is_valid()
	return not _failed


func _image(p_binding: int, p_texture: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u.binding = p_binding
	u.add_id(p_texture)
	return u


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	if not capture_armed:
		return
	capture_armed = false
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null or not buffers.has_texture(&"nextgeo", &"instance_id") or not buffers.has_texture(&"forward_clustered", &"normal_roughness"):
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
	# The specular target outlives the phases that write it: read it only while this effect asks for it.
	var specular := buffers.get_texture(&"forward_clustered", &"specular") if needs_separate_specular and buffers.has_texture(&"forward_clustered", &"specular") else RID()
	var megalights := read_megalights and buffers.has_texture(&"megalights", &"ml_albedo") and buffers.has_texture(&"megalights", &"ml_material")
	var ml_albedo := buffers.get_texture(&"megalights", &"ml_albedo") if megalights else _dummy_rgba8
	var ml_material := buffers.get_texture(&"megalights", &"ml_material") if megalights else _dummy_rgba8
	var voxel_gi := buffers.get_texture(&"forward_clustered", &"voxel_gi") if read_voxel_gi and buffers.has_texture(&"forward_clustered", &"voxel_gi") else RID()
	var uniforms: Array[RDUniform] = []
	uniforms.append(_image(0, buffers.get_texture(&"nextgeo", &"instance_id")))
	uniforms.append(_image(1, buffers.get_color_layer(0, false)))
	uniforms.append(_image(2, buffers.get_texture(&"forward_clustered", &"normal_roughness")))
	uniforms.append(_image(3, velocity if velocity.is_valid() else _dummy_rg))
	uniforms.append(_image(4, specular if specular.is_valid() else _dummy_rgba))
	var u_out := RDUniform.new()
	u_out.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u_out.binding = 5
	u_out.add_id(_out_buffer)
	uniforms.append(u_out)
	uniforms.append(_image(6, ml_albedo))
	uniforms.append(_image(7, ml_material))
	uniforms.append(_image(8, voxel_gi if voxel_gi.is_valid() else _dummy_rg8ui))
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
		"specular": specular.is_valid(),
		"megalights": megalights,
		"voxel_gi": voxel_gi.is_valid(),
	}


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	for rid in [_out_buffer, _pipeline, _dummy_rg, _dummy_rgba, _dummy_rgba8, _dummy_rg8ui, _shader]:
		if rid.is_valid():
			rd.free_rid(rid)
