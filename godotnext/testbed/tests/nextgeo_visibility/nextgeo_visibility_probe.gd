extends CompositorEffect
## GPU readback probe for NextGeo visibility, instance-ID, velocity and Q21 (#83).
## Sets `needs_motion_vectors = false` so only the active upscaler / TAA drives
## the §6.1 velocity plan.

const PROBE_GLSL := """#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r32ui, set = 0, binding = 0) uniform readonly uimage2D vis_img;
layout(r32ui, set = 0, binding = 1) uniform readonly uimage2D inst_img;
layout(rg16f, set = 0, binding = 2) uniform readonly image2D vel_img;

layout(std430, set = 0, binding = 3) buffer ProbeOut {
	uint sentinel_count;
	uint nonfinite_count;
	uint virtual_pixels;
	uint bg_vis;
	uint bg_inst;
	uint virt_vis;
	uint virt_inst;
	uint leg_vis;
	uint leg_inst;
	uint pad0;
	vec2 bg_vel;
	vec2 virt_vel;
	vec2 leg_vel;
} out_buf;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	ivec2 virt_px;
	ivec2 leg_px;
	ivec2 bg_px;
} params;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= params.size.x || p.y >= params.size.y) {
		return;
	}
	uint vis = imageLoad(vis_img, p).r;
	uint inst = imageLoad(inst_img, p).r;
	vec2 vel = imageLoad(vel_img, p).xy;
	uvec2 bits = floatBitsToUint(vel);

	if (vis != 0xFFFFFFFFu) {
		atomicAdd(out_buf.virtual_pixels, 1u);
	}
	bool non_finite = ((bits.x & 0x7F800000u) == 0x7F800000u) || ((bits.y & 0x7F800000u) == 0x7F800000u);
	if (non_finite) {
		atomicAdd(out_buf.nonfinite_count, 1u);
	} else if (bits.x == 0xBF800000u && bits.y == 0xBF800000u) {
		atomicAdd(out_buf.sentinel_count, 1u);
	}

	if (p == params.bg_px) {
		out_buf.bg_vis = vis;
		out_buf.bg_inst = inst;
		out_buf.bg_vel = vel;
	}
	if (p == params.virt_px) {
		out_buf.virt_vis = vis;
		out_buf.virt_inst = inst;
		out_buf.virt_vel = vel;
	}
	if (p == params.leg_px) {
		out_buf.leg_vis = vis;
		out_buf.leg_inst = inst;
		out_buf.leg_vel = vel;
	}
}
"""

const OUT_BYTES := 64

var last_sample: Dictionary = {}
var sample_count := 0
var virt_uv := Vector2(0.35, 0.50)
var leg_uv := Vector2(0.65, 0.50)

var _shader := RID()
var _pipeline := RID()
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
		push_error("NextGeoVisibilityProbe shader compile error: %s" % (spirv.compile_error_compute if spirv != null else "null"))
		_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "NextGeoVisibilityProbe")
	if not _shader.is_valid():
		_failed = true
		return false
	_pipeline = rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
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
	if not buffers.has_texture(&"nextgeo", &"visibility") or not buffers.has_texture(&"nextgeo", &"instance_id"):
		return
	var velocity := buffers.get_velocity_layer(0, false)
	if not velocity.is_valid():
		return
	var rd := RenderingServer.get_rendering_device()
	if not _ensure(rd):
		return

	var vis_tex := buffers.get_texture(&"nextgeo", &"visibility")
	var inst_tex := buffers.get_texture(&"nextgeo", &"instance_id")
	var size := buffers.get_internal_size()
	if size.x < 16 or size.y < 16:
		return

	rd.buffer_clear(_out_buffer, 0, OUT_BYTES)

	var u0 := RDUniform.new()
	u0.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u0.binding = 0
	u0.add_id(vis_tex)

	var u1 := RDUniform.new()
	u1.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u1.binding = 1
	u1.add_id(inst_tex)

	var u2 := RDUniform.new()
	u2.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u2.binding = 2
	u2.add_id(velocity)

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
	rd.compute_list_dispatch(cl, int((size.x + 7) / 8), int((size.y + 7) / 8), 1)
	rd.compute_list_end()

	var raw := rd.buffer_get_data(_out_buffer)
	if raw.size() < OUT_BYTES:
		return
	sample_count += 1
	last_sample = {
		"sample": sample_count,
		"size": size,
		"mode": buffers.get_scaling_3d_mode(),
		"use_taa": buffers.get_use_taa(),
		"sentinel_count": raw.decode_u32(0),
		"nonfinite_count": raw.decode_u32(4),
		"virtual_pixels": raw.decode_u32(8),
		"bg_vis": raw.decode_u32(12),
		"bg_inst": raw.decode_u32(16),
		"virt_vis": raw.decode_u32(20),
		"virt_inst": raw.decode_u32(24),
		"leg_vis": raw.decode_u32(28),
		"leg_inst": raw.decode_u32(32),
		"bg_vel": Vector2(raw.decode_float(40), raw.decode_float(44)),
		"virt_vel": Vector2(raw.decode_float(48), raw.decode_float(52)),
		"leg_vel": Vector2(raw.decode_float(56), raw.decode_float(60)),
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
	if _pipeline.is_valid():
		rd.free_rid(_pipeline)
		_pipeline = RID()
	if _shader.is_valid():
		rd.free_rid(_shader)
		_shader = RID()
