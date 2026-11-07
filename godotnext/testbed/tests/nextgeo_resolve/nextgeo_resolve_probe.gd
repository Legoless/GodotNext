extends CompositorEffect
## GPU readback probe for NextGeo Stage A normal-roughness and instance-ID guides (#80).

const PROBE_GLSL := """#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D nr_tex;
layout(r32ui, set = 0, binding = 1) uniform readonly uimage2D inst_img;
layout(r32ui, set = 0, binding = 2) uniform readonly uimage2D vis_img;

layout(std430, set = 0, binding = 3) buffer ProbeOut {
	uvec4 virt_nr;
	uvec4 leg_nr;
	uvec4 bg_nr;
	uint virt_inst;
	uint leg_inst;
	uint bg_inst;
	uint virt_vis;
	uint leg_vis;
	uint bg_vis;
	uint pad0;
	uint pad1;
} out_buf;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 virt_px;
	ivec2 leg_px;
	ivec2 bg_px;
} params;

uvec4 load_nr_u8(ivec2 p) {
	ivec2 c = clamp(p, ivec2(0), params.size - ivec2(1));
	vec4 v = clamp(texelFetch(nr_tex, c, 0), vec4(0.0), vec4(1.0));
	return uvec4(round(v * 255.0));
}

void main() {
	ivec2 vp = clamp(params.virt_px, ivec2(0), params.size - ivec2(1));
	ivec2 lp = clamp(params.leg_px, ivec2(0), params.size - ivec2(1));
	ivec2 bp = clamp(params.bg_px, ivec2(0), params.size - ivec2(1));

	out_buf.virt_nr = load_nr_u8(vp);
	out_buf.leg_nr = load_nr_u8(lp);
	out_buf.bg_nr = load_nr_u8(bp);

	out_buf.virt_inst = imageLoad(inst_img, vp).r;
	out_buf.leg_inst = imageLoad(inst_img, lp).r;
	out_buf.bg_inst = imageLoad(inst_img, bp).r;

	out_buf.virt_vis = imageLoad(vis_img, vp).r;
	out_buf.leg_vis = imageLoad(vis_img, lp).r;
	out_buf.bg_vis = imageLoad(vis_img, bp).r;
}
"""

const OUT_BYTES := 80

var last_sample: Dictionary = {}
var sample_count := 0
var virt_uv := Vector2(0.30, 0.50)
var leg_uv := Vector2(0.70, 0.50)

var _shader := RID()
var _pipeline := RID()
var _sampler := RID()
var _out_buffer := RID()
var _failed := false


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	needs_motion_vectors = false


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
		push_error("NextGeoResolveProbe shader compile error: %s" % (spirv.compile_error_compute if spirv != null else "null"))
		_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "NextGeoResolveProbe")
	if not _shader.is_valid():
		_failed = true
		return false
	_pipeline = rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
		_failed = true
		return false
	var ss := RDSamplerState.new()
	ss.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	ss.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	ss.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	_sampler = rd.sampler_create(ss)
	if not _sampler.is_valid():
		_failed = true
		return false
	var zeros := PackedByteArray()
	zeros.resize(OUT_BYTES)
	_out_buffer = rd.storage_buffer_create(OUT_BYTES, zeros)
	return _out_buffer.is_valid()


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null:
		return
	if not buffers.has_texture(&"forward_clustered", &"normal_roughness"):
		return
	if not buffers.has_texture(&"nextgeo", &"visibility") or not buffers.has_texture(&"nextgeo", &"instance_id"):
		return
	var rd := RenderingServer.get_rendering_device()
	if not _ensure(rd):
		return

	var nr_tex := buffers.get_texture(&"forward_clustered", &"normal_roughness")
	var inst_tex := buffers.get_texture(&"nextgeo", &"instance_id")
	var vis_tex := buffers.get_texture(&"nextgeo", &"visibility")
	var size := buffers.get_internal_size()
	if size.x < 16 or size.y < 16:
		return

	rd.buffer_clear(_out_buffer, 0, OUT_BYTES)

	var u0 := RDUniform.new()
	u0.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	u0.binding = 0
	u0.add_id(_sampler)
	u0.add_id(nr_tex)

	var u1 := RDUniform.new()
	u1.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u1.binding = 1
	u1.add_id(inst_tex)

	var u2 := RDUniform.new()
	u2.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u2.binding = 2
	u2.add_id(vis_tex)

	var u3 := RDUniform.new()
	u3.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u3.binding = 3
	u3.add_id(_out_buffer)

	var uset := UniformSetCacheRD.get_cache(_shader, 0, [u0, u1, u2, u3])
	if not uset.is_valid():
		return

	var virt_px := Vector2i(
		clampi(int(roundf(virt_uv.x * float(size.x))), 0, size.x - 1),
		clampi(int(roundf(virt_uv.y * float(size.y))), 0, size.y - 1)
	)
	var leg_px := Vector2i(
		clampi(int(roundf(leg_uv.x * float(size.x))), 0, size.x - 1),
		clampi(int(roundf(leg_uv.y * float(size.y))), 0, size.y - 1)
	)
	var bg_px := Vector2i(4, 4)
	var push := PackedInt32Array([
		size.x, size.y,
		virt_px.x, virt_px.y,
		leg_px.x, leg_px.y,
		bg_px.x, bg_px.y,
	]).to_byte_array()

	var cl := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(cl, _pipeline)
	rd.compute_list_bind_uniform_set(cl, uset, 0)
	rd.compute_list_set_push_constant(cl, push, push.size())
	rd.compute_list_dispatch(cl, 1, 1, 1)
	rd.compute_list_end()

	var raw := rd.buffer_get_data(_out_buffer)
	if raw.size() < OUT_BYTES:
		return
	sample_count += 1
	last_sample = {
		"sample": sample_count,
		"size": size,
		"virt_nr": [raw.decode_u32(0), raw.decode_u32(4), raw.decode_u32(8), raw.decode_u32(12)],
		"leg_nr": [raw.decode_u32(16), raw.decode_u32(20), raw.decode_u32(24), raw.decode_u32(28)],
		"bg_nr": [raw.decode_u32(32), raw.decode_u32(36), raw.decode_u32(40), raw.decode_u32(44)],
		"virt_inst": raw.decode_u32(48),
		"leg_inst": raw.decode_u32(52),
		"bg_inst": raw.decode_u32(56),
		"virt_vis": raw.decode_u32(60),
		"leg_vis": raw.decode_u32(64),
		"bg_vis": raw.decode_u32(68),
	}


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	if _out_buffer.is_valid():
		rd.free_rid(_out_buffer)
		_out_buffer = RID()
	if _sampler.is_valid():
		rd.free_rid(_sampler)
		_sampler = RID()
	if _pipeline.is_valid():
		rd.free_rid(_pipeline)
		_pipeline = RID()
	if _shader.is_valid():
		rd.free_rid(_shader)
		_shader = RID()
