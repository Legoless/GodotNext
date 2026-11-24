/**************************************************************************/
/*  mega_lights.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "mega_lights.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/effects/denoise.h"
#include "servers/rendering/renderer_rd/effects/denoiser_bypass.h"
#include "servers/rendering/renderer_rd/effects/megalights_stbn.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_front_layer.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_froxel.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_froxel_compute.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_generate_samples.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_guiding.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_reference.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_screen_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_shade_samples.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_trace_rays.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_trace_rays_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_trace_vsm.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_function_atlas.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_shadow_map_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#include <cmath>
#include <cstring>

using namespace RendererRD;

namespace {

struct MegaLightsCheckState {
	Mutex mutex;
	bool active = false;
	bool initialized = false;
	String mode;
	Dictionary options;
	Dictionary result;
	uint32_t completed_steps = 0;
	uint32_t total_steps = 0;
	uint32_t wait_frames = 0;
	bool checkpoint_recorded = false;
	double checkpoint_max_rse = 0.0;
	double checkpoint_p99_rse = 0.0;
	double checkpoint_mean_rse = 0.0;
	uint32_t checkpoint_penumbra_pixels = 0;
};

MegaLightsCheckState &ml_check_state() {
	static MegaLightsCheckState state;
	return state;
}

static uint64_t ml_hash_bytes(uint64_t p_h, const void *p_data, size_t p_len) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_len; i++) {
		p_h ^= uint64_t(bytes[i]);
		p_h *= 1099511628211ull;
	}
	return p_h;
}

static uint64_t ml_hash_camera_and_size(const Transform3D &p_cam, const Projection &p_proj, const Size2i &p_size) {
	uint64_t h = 14695981039346656037ull;
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			float v = float(p_cam.basis.rows[r][c]);
			h = ml_hash_bytes(h, &v, sizeof(v));
		}
		float o = float(p_cam.origin[r]);
		h = ml_hash_bytes(h, &o, sizeof(o));
	}
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			float m = float(p_proj.columns[c][r]);
			h = ml_hash_bytes(h, &m, sizeof(m));
		}
	}
	int32_t dims[2] = { p_size.x, p_size.y };
	h = ml_hash_bytes(h, dims, sizeof(dims));
	return h;
}

static float ml_cpu_luminance(float p_r, float p_g, float p_b) {
	return p_r * 0.2126f + p_g * 0.7152f + p_b * 0.0722f;
}

struct GuidingEntryCompare {
	bool operator()(const MegaLights::GuidingEntry &p_a, const MegaLights::GuidingEntry &p_b) const {
		if (p_a.count != p_b.count) {
			return p_a.count > p_b.count;
		}
		return p_a.light_packed < p_b.light_packed;
	}
};

static uint32_t ml_xorshift32_from_u64(uint64_t &r_state) {
	// SplitMix64 step -> high-quality uniform 32-bit value for CPU reference tests.
	r_state += 0x9E3779B97F4A7C15ull;
	uint64_t z = r_state;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z = z ^ (z >> 31);
	return uint32_t(z >> 32);
}

static SafeNumeric<uint64_t> ml_total_allocated_bytes;
static SafeNumeric<uint64_t> ml_froxel_allocated_bytes;
static SafeNumeric<uint64_t> ml_front_layer_allocated_bytes;
static SafeNumeric<uint32_t> ml_last_froxel_dispatch_mode; // 0=off, 1=shared_fog_d17, 2=fog_only, 3=d17_only
static SafeNumeric<uint32_t> ml_last_froxel_samples_per_cell;
static SafeNumeric<uint32_t> ml_last_froxel_dispatched_cells;

static RID ml_create_rgba16f_3d_texture(RD *p_rd, const Vector3i &p_size, const char *p_name) {
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = uint32_t(MAX(p_size.x, 1));
	tf.height = uint32_t(MAX(p_size.y, 1));
	tf.depth = uint32_t(MAX(p_size.z, 1));
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	RID tex = p_rd->texture_create(tf, RD::TextureView());
	if (tex.is_valid()) {
		if (p_name != nullptr) {
			p_rd->set_resource_name(tex, p_name);
		}
		p_rd->texture_clear(tex, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	return tex;
}

static void ml_free_rid(RD *p_rd, RID &r_rid) {
	if (p_rd != nullptr && r_rid.is_valid()) {
		p_rd->free_rid(r_rid);
	}
	r_rid = RID();
}

static void ml_release_denoise_readback(MegaLightsViewData::DenoiseReadbackState *p_state) {
	if (p_state != nullptr && p_state->refs.unref()) {
		memdelete(p_state);
	}
}

static void ml_denoise_stats_received(const PackedByteArray &p_data, uint64_t p_state_ptr) {
	MegaLightsViewData::DenoiseReadbackState *state = reinterpret_cast<MegaLightsViewData::DenoiseReadbackState *>(p_state_ptr);
	if (state == nullptr) {
		return;
	}
	if (state->alive.is_set() && p_data.size() >= int(4u * sizeof(uint32_t))) {
		uint32_t words[4] = {};
		memcpy(words, p_data.ptr(), sizeof(words));
		state->removed_fp = words[0];
		state->total_fp = words[1];
		state->clamped_pixels = words[2];
		state->valid_pixels = words[3];
		state->has_result = true;
	}
	state->pending.clear();
	ml_release_denoise_readback(state);
}

static RID ml_create_rgba16f_texture(RD *p_rd, const Size2i &p_size) {
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = uint32_t(MAX(p_size.x, 1));
	tf.height = uint32_t(MAX(p_size.y, 1));
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	return p_rd->texture_create(tf, RD::TextureView());
}

} // namespace

/* MegaLightsViewData */

MegaLightsViewData::MegaLightsViewData() {
	denoise_readback = memnew(DenoiseReadbackState);
	denoise_readback->refs.init(1);
	denoise_readback->alive.set();
	denoise_readback->pending.clear();
}

void MegaLightsViewData::_update_memory_accounting() {
	const uint64_t next_total = sampling_bytes + visibility_bytes + retrace_bytes + shade_bytes + denoise_bytes + denoise_debug_bytes + reference_bytes + verify_bytes + transient_bytes + froxel_bytes + front_layer_guide_bytes;
	if (next_total >= tracked_total_bytes) {
		ml_total_allocated_bytes.add(next_total - tracked_total_bytes);
	} else {
		ml_total_allocated_bytes.sub(tracked_total_bytes - next_total);
	}
	tracked_total_bytes = next_total;

	if (froxel_bytes >= tracked_froxel_bytes) {
		ml_froxel_allocated_bytes.add(froxel_bytes - tracked_froxel_bytes);
	} else {
		ml_froxel_allocated_bytes.sub(tracked_froxel_bytes - froxel_bytes);
	}
	tracked_froxel_bytes = froxel_bytes;

	const uint64_t next_fl = is_front_layer_view ? next_total : front_layer_guide_bytes;
	if (next_fl >= tracked_front_layer_bytes) {
		ml_front_layer_allocated_bytes.add(next_fl - tracked_front_layer_bytes);
	} else {
		ml_front_layer_allocated_bytes.sub(tracked_front_layer_bytes - next_fl);
	}
	tracked_front_layer_bytes = next_fl;
}

void MegaLightsViewData::_free_sampling_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, sample_buffer);
	ml_free_rid(rd, visibility_buffer);
	ml_free_rid(rd, hit_distance_texture);
	ml_free_rid(rd, guiding_buffers[0]);
	ml_free_rid(rd, guiding_buffers[1]);
	sample_grid_size = Size2i();
	guiding_tile_dims = Size2i();
	samples_per_pixel = 0;
	guiding_write_index = 0;
	guiding_has_history = false;
	sampling_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_visibility_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, vis_params_ubo);
	ml_free_rid(rd, vis_counters_buffer);
	ml_free_rid(rd, vis_indirect_buffer);
	ml_free_rid(rd, vis_vsm_params_buffer);
	ml_free_rid(rd, vis_ray_list_buffer);
	ml_free_rid(rd, vis_vsm_list_buffer);
	ml_free_rid(rd, vis_sample_ray_map_buffer);
	vis_list_capacity = 0;
	ml_free_rid(rd, vis_retrace_buffer);
	visibility_bytes = 0;
	retrace_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_shade_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, shade_sample_diffuse);
	ml_free_rid(rd, shade_sample_specular);
	ml_free_rid(rd, noisy_diffuse);
	ml_free_rid(rd, noisy_specular);
	ml_free_rid(rd, denoise_params_ubo);
	shade_sample_size = Size2i();
	shade_full_size = Size2i();
	denoise_output_diffuse = RID();
	denoise_output_specular = RID();
	shade_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_denoise_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, denoise_diffuse[0]);
	ml_free_rid(rd, denoise_diffuse[1]);
	ml_free_rid(rd, denoise_specular[0]);
	ml_free_rid(rd, denoise_specular[1]);
	ml_free_rid(rd, denoise_scratch_diffuse);
	ml_free_rid(rd, denoise_scratch_specular);
	ml_free_rid(rd, denoise_moments[0]);
	ml_free_rid(rd, denoise_moments[1]);
	ml_free_rid(rd, denoise_history_len[0]);
	ml_free_rid(rd, denoise_history_len[1]);
	ml_free_rid(rd, denoise_prev_depth_normal);
	ml_free_rid(rd, denoise_clamp_stats_buffer);
	ml_free_rid(rd, denoise_reset_mask_buffer);
	denoise_size = Size2i();
	denoise_reset_tile_dims = Size2i();
	denoise_history_index = 0;
	denoise_has_history = false;
	denoise_last_frame = 0;
	denoise_has_cam_transform = false;
	denoise_output_diffuse = RID();
	denoise_output_specular = RID();
	denoise_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_denoise_debug_texture() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, denoise_debug_texture);
	denoise_debug_size = Size2i();
	denoise_debug_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_reference_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, reference_accum_texture);
	reference_accum_size = Size2i();
	reference_frame_count = 0;
	reference_camera_hash = 0;
	reference_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_verify_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, verify_accum_texture);
	ml_free_rid(rd, verify_counts_buffer);
	verify_size = Size2i();
	verify_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_transient_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, params_ubo);
	ml_free_rid(rd, layer_group_buffer);
	layer_group_capacity = 0;
	ml_free_rid(rd, light_shadow_policy_buffer);
	light_shadow_policy_capacity = 0;
	transient_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_froxel_buffers() {
	RD *rd = RD::get_singleton();
	for (uint32_t i = 0; i < 2; i++) {
		ml_free_rid(rd, froxel_fog_tex[i]);
		ml_free_rid(rd, froxel_sh_r[i]);
		ml_free_rid(rd, froxel_sh_g[i]);
		ml_free_rid(rd, froxel_sh_b[i]);
	}
	ml_free_rid(rd, froxel_params_ubo);
	froxel_fog_size = Vector3i();
	froxel_slot = 0;
	froxel_history_valid = false;
	froxel_history_frames = 0;
	froxel_has_prev_cam = false;
	froxel_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::_free_front_layer_guide_textures() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, front_layer_albedo_tex);
	ml_free_rid(rd, front_layer_material_tex);
	front_layer_guide_size = Size2i();
	front_layer_guide_bytes = 0;
	_update_memory_accounting();
}

void MegaLightsViewData::set_is_front_layer_view(bool p_is_front_layer) {
	if (is_front_layer_view != p_is_front_layer) {
		is_front_layer_view = p_is_front_layer;
		_update_memory_accounting();
	}
}

void MegaLightsViewData::free_froxel_buffers() {
	_free_froxel_buffers();
}

void MegaLightsViewData::free_front_layer_guide_textures() {
	_free_front_layer_guide_textures();
}

bool MegaLightsViewData::ensure_froxel_buffers(bool p_fog_active, const Vector3i &p_fog_size, bool p_need_fog_sh) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (!froxel_params_ubo.is_valid()) {
		froxel_params_ubo = rd->uniform_buffer_create(sizeof(MegaLights::FroxelParamsUBO));
		ERR_FAIL_COND_V(!froxel_params_ubo.is_valid(), false);
	}

	uint64_t next_bytes = sizeof(MegaLights::FroxelParamsUBO);
	if (!p_fog_active || p_fog_size.x <= 0 || p_fog_size.y <= 0 || p_fog_size.z <= 0) {
		for (uint32_t i = 0; i < 2; i++) {
			ml_free_rid(rd, froxel_fog_tex[i]);
			ml_free_rid(rd, froxel_sh_r[i]);
			ml_free_rid(rd, froxel_sh_g[i]);
			ml_free_rid(rd, froxel_sh_b[i]);
		}
		froxel_fog_size = Vector3i();
		froxel_bytes = next_bytes;
		_update_memory_accounting();
		return true;
	}

	const bool size_changed = (froxel_fog_size != p_fog_size);
	if (size_changed || !froxel_fog_tex[0].is_valid()) {
		for (uint32_t i = 0; i < 2; i++) {
			ml_free_rid(rd, froxel_fog_tex[i]);
			ml_free_rid(rd, froxel_sh_r[i]);
			ml_free_rid(rd, froxel_sh_g[i]);
			ml_free_rid(rd, froxel_sh_b[i]);
		}
		froxel_fog_tex[0] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel Fog 0");
		froxel_fog_tex[1] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel Fog 1");
		if (!froxel_fog_tex[0].is_valid() || !froxel_fog_tex[1].is_valid()) {
			_free_froxel_buffers();
			return false;
		}
		froxel_fog_size = p_fog_size;
		froxel_slot = 0;
		froxel_history_valid = false;
		froxel_history_frames = 0;
	}
	const uint64_t tex_bytes = uint64_t(p_fog_size.x) * uint64_t(p_fog_size.y) * uint64_t(p_fog_size.z) * 8ULL;
	next_bytes += tex_bytes * 2ULL;

	if (p_need_fog_sh) {
		if (!froxel_sh_r[0].is_valid()) {
			froxel_sh_r[0] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel SH R0");
			froxel_sh_r[1] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel SH R1");
			froxel_sh_g[0] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel SH G0");
			froxel_sh_g[1] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel SH G1");
			froxel_sh_b[0] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel SH B0");
			froxel_sh_b[1] = ml_create_rgba16f_3d_texture(rd, p_fog_size, "MegaLights Froxel SH B1");
			if (!froxel_sh_r[0].is_valid() || !froxel_sh_r[1].is_valid() ||
					!froxel_sh_g[0].is_valid() || !froxel_sh_g[1].is_valid() ||
					!froxel_sh_b[0].is_valid() || !froxel_sh_b[1].is_valid()) {
				_free_froxel_buffers();
				return false;
			}
			froxel_history_valid = false;
			froxel_history_frames = 0;
		}
		next_bytes += tex_bytes * 6ULL;
	} else {
		for (uint32_t i = 0; i < 2; i++) {
			ml_free_rid(rd, froxel_sh_r[i]);
			ml_free_rid(rd, froxel_sh_g[i]);
			ml_free_rid(rd, froxel_sh_b[i]);
		}
	}

	froxel_bytes = next_bytes;
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_front_layer_guide_textures(const Size2i &p_size) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_size.x <= 0 || p_size.y <= 0) {
		return false;
	}
	if (front_layer_albedo_tex.is_valid() && front_layer_material_tex.is_valid() && front_layer_guide_size == p_size) {
		return true;
	}
	_free_front_layer_guide_textures();
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	tf.width = uint32_t(p_size.x);
	tf.height = uint32_t(p_size.y);
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	front_layer_albedo_tex = rd->texture_create(tf, RD::TextureView());
	front_layer_material_tex = rd->texture_create(tf, RD::TextureView());
	if (!front_layer_albedo_tex.is_valid() || !front_layer_material_tex.is_valid()) {
		_free_front_layer_guide_textures();
		return false;
	}
	rd->set_resource_name(front_layer_albedo_tex, "MegaLights Front-Layer Albedo");
	rd->set_resource_name(front_layer_material_tex, "MegaLights Front-Layer Material");
	front_layer_guide_size = p_size;
	front_layer_guide_bytes = uint64_t(p_size.x) * uint64_t(p_size.y) * 8ULL;
	_update_memory_accounting();
	return true;
}

void MegaLightsViewData::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (p_render_buffers == nullptr) {
		free_data();
		return;
	}
	const Size2i new_size = p_render_buffers->get_internal_size();
	if (new_size != internal_size) {
		_free_sampling_buffers();
		_free_visibility_buffers();
		_free_shade_buffers();
		_free_denoise_buffers();
		_free_denoise_debug_texture();
		_free_reference_buffers();
		_free_verify_buffers();
		_free_froxel_buffers();
		_free_front_layer_guide_textures();
		internal_size = new_size;
		frame_index = 0;
	}
}

void MegaLightsViewData::free_data() {
	_free_sampling_buffers();
	_free_visibility_buffers();
	_free_shade_buffers();
	_free_denoise_buffers();
	_free_denoise_debug_texture();
	_free_reference_buffers();
	_free_verify_buffers();
	_free_transient_buffers();
	_free_froxel_buffers();
	_free_front_layer_guide_textures();
	internal_size = Size2i();
	frame_index = 0;
	last_denoise_stats = DenoiseStats();
	denoise_last_rr_bypass = false;
}

MegaLightsViewData::~MegaLightsViewData() {
	free_data();
	if (denoise_readback != nullptr) {
		denoise_readback->alive.clear();
		ml_release_denoise_readback(denoise_readback);
		denoise_readback = nullptr;
	}
}

bool MegaLightsViewData::ensure_sampling_buffers(const Size2i &p_internal_size, uint32_t p_samples_per_pixel, uint32_t p_downsample_mode) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0) {
		return false;
	}
	const uint32_t spp = CLAMP(p_samples_per_pixel, MegaLights::MIN_SAMPLES_PER_PIXEL, MegaLights::MAX_SAMPLES_PER_PIXEL);
	const uint32_t ds = p_downsample_mode == MegaLights::DOWNSAMPLE_FULL ? uint32_t(MegaLights::DOWNSAMPLE_FULL) : uint32_t(MegaLights::DOWNSAMPLE_HALF);

	if (!params_ubo.is_valid()) {
		params_ubo = rd->uniform_buffer_create(sizeof(MegaLights::ParamsUBO));
		ERR_FAIL_COND_V(!params_ubo.is_valid(), false);
	}

	if (sample_buffer.is_valid() && internal_size == p_internal_size && samples_per_pixel == spp && downsample_mode == ds) {
		return true;
	}

	_free_sampling_buffers();
	internal_size = p_internal_size;
	samples_per_pixel = spp;
	downsample_mode = ds;
	sample_grid_size = MegaLights::compute_sample_grid_size(p_internal_size, MegaLights::DownsampleMode(ds));
	guiding_tile_dims = MegaLights::compute_guiding_tile_dims(p_internal_size);

	const uint64_t total_samples = MAX(uint64_t(sample_grid_size.x) * uint64_t(sample_grid_size.y) * uint64_t(spp), uint64_t(1));
	const uint32_t sample_bytes = uint32_t(total_samples * sizeof(MegaLights::SampleGPU));
	const uint32_t vis_bytes = uint32_t(total_samples * sizeof(uint32_t));
	const uint64_t total_tiles = MAX(uint64_t(guiding_tile_dims.x) * uint64_t(guiding_tile_dims.y), uint64_t(1));
	const uint32_t guiding_bytes = uint32_t(total_tiles * MegaLights::GUIDING_TILE_WORDS * sizeof(uint32_t));

	sample_buffer = rd->storage_buffer_create(sample_bytes);
	visibility_buffer = rd->storage_buffer_create(vis_bytes);
	guiding_buffers[0] = rd->storage_buffer_create(guiding_bytes);
	guiding_buffers[1] = rd->storage_buffer_create(guiding_bytes);

	// GodotNext: megalights_visibility (#146). Per-sample R16F hit distance (2DArray, layer = sample k, miss = 65504.0).
	RD::TextureFormat htf;
	htf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	htf.format = RD::DATA_FORMAT_R16_SFLOAT;
	htf.width = uint32_t(sample_grid_size.x);
	htf.height = uint32_t(sample_grid_size.y);
	htf.depth = 1;
	htf.array_layers = spp;
	htf.mipmaps = 1;
	htf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	hit_distance_texture = rd->texture_create(htf, RD::TextureView());

	ERR_FAIL_COND_V(!sample_buffer.is_valid() || !visibility_buffer.is_valid() || !hit_distance_texture.is_valid() || !guiding_buffers[0].is_valid() || !guiding_buffers[1].is_valid(), false);

	rd->texture_clear(hit_distance_texture, Color(MegaLights::HIT_DISTANCE_MISS, 0.0f, 0.0f, 0.0f), 0, 1, 0, spp);
	rd->buffer_clear(guiding_buffers[0], 0, guiding_bytes);
	rd->buffer_clear(guiding_buffers[1], 0, guiding_bytes);
	guiding_write_index = 0;
	guiding_has_history = false;
	sampling_bytes = uint64_t(sample_bytes) + uint64_t(vis_bytes) + 2ull * uint64_t(guiding_bytes) + total_samples * 2ull;
	transient_bytes = (params_ubo.is_valid() ? sizeof(MegaLights::ParamsUBO) : 0ull) + uint64_t(layer_group_capacity) * sizeof(uint32_t) + uint64_t(light_shadow_policy_capacity) * 4ull * sizeof(uint32_t);
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_visibility_buffers(uint32_t p_total_samples) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t required = MAX(p_total_samples, 64u);

	if (!vis_params_ubo.is_valid()) {
		vis_params_ubo = rd->uniform_buffer_create(sizeof(MegaLights::VisParamsUBO));
		ERR_FAIL_COND_V(!vis_params_ubo.is_valid(), false);
	}
	if (!vis_counters_buffer.is_valid()) {
		vis_counters_buffer = rd->storage_buffer_create(8u * sizeof(uint32_t));
		ERR_FAIL_COND_V(!vis_counters_buffer.is_valid(), false);
		rd->buffer_clear(vis_counters_buffer, 0, 8u * sizeof(uint32_t));
	}
	if (!vis_indirect_buffer.is_valid()) {
		vis_indirect_buffer = rd->storage_buffer_create(2u * 4u * sizeof(uint32_t), Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		ERR_FAIL_COND_V(!vis_indirect_buffer.is_valid(), false);
		rd->buffer_clear(vis_indirect_buffer, 0, 2u * 4u * sizeof(uint32_t));
	}
	if (!vis_vsm_params_buffer.is_valid()) {
		vis_vsm_params_buffer = rd->storage_buffer_create(sizeof(MegaLights::VisVSMProjectionParams));
		ERR_FAIL_COND_V(!vis_vsm_params_buffer.is_valid(), false);
		rd->buffer_clear(vis_vsm_params_buffer, 0, sizeof(MegaLights::VisVSMProjectionParams));
	}

	if (vis_ray_list_buffer.is_valid() && vis_vsm_list_buffer.is_valid() && vis_sample_ray_map_buffer.is_valid() && vis_list_capacity >= required) {
		return true;
	}

	ml_free_rid(rd, vis_ray_list_buffer);
	ml_free_rid(rd, vis_vsm_list_buffer);
	ml_free_rid(rd, vis_sample_ray_map_buffer);
	vis_list_capacity = required;

	vis_ray_list_buffer = rd->storage_buffer_create(vis_list_capacity * 2u * sizeof(uint32_t));
	vis_vsm_list_buffer = rd->storage_buffer_create(vis_list_capacity * sizeof(uint32_t));
	vis_sample_ray_map_buffer = rd->storage_buffer_create(vis_list_capacity * sizeof(uint32_t));
	ERR_FAIL_COND_V(!vis_ray_list_buffer.is_valid() || !vis_vsm_list_buffer.is_valid() || !vis_sample_ray_map_buffer.is_valid(), false);
	visibility_bytes = sizeof(MegaLights::VisParamsUBO) + 8ull * sizeof(uint32_t) + 8ull * sizeof(uint32_t) + sizeof(MegaLights::VisVSMProjectionParams) + uint64_t(vis_list_capacity) * 4ull * sizeof(uint32_t);
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_retrace_buffer() {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (vis_retrace_buffer.is_valid()) {
		return true;
	}
	const uint32_t bytes = nextgi_shadow_retrace_buffer_size(NEXTGI_SHADOW_RETRACE_MAX_RECORDS);
	vis_retrace_buffer = rd->storage_buffer_create(bytes);
	ERR_FAIL_COND_V(!vis_retrace_buffer.is_valid(), false);
	const uint32_t header[4] = { 0u, 0u, 0u, 0u };
	rd->buffer_update(vis_retrace_buffer, 0, sizeof(header), header);
	retrace_bytes = bytes;
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_shade_buffers(const Size2i &p_internal_size, const Size2i &p_sample_size, uint32_t p_downsample_mode) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0 || p_sample_size.x <= 0 || p_sample_size.y <= 0) {
		return false;
	}
	if (!denoise_params_ubo.is_valid()) {
		denoise_params_ubo = rd->uniform_buffer_create(sizeof(NextGIDenoise::MegaLightsDenoiseParamsGPU));
		ERR_FAIL_COND_V(!denoise_params_ubo.is_valid(), false);
	}

	const bool half_res = (p_downsample_mode != uint32_t(MegaLights::DOWNSAMPLE_FULL));
	if (!noisy_diffuse.is_valid() || !noisy_specular.is_valid() || shade_full_size != p_internal_size) {
		ml_free_rid(rd, noisy_diffuse);
		ml_free_rid(rd, noisy_specular);
		noisy_diffuse = ml_create_rgba16f_texture(rd, p_internal_size);
		noisy_specular = ml_create_rgba16f_texture(rd, p_internal_size);
		ERR_FAIL_COND_V(!noisy_diffuse.is_valid() || !noisy_specular.is_valid(), false);
		rd->texture_clear(noisy_diffuse, Color(0, 0, 0, MegaLights::HIT_DISTANCE_MISS), 0, 1, 0, 1);
		rd->texture_clear(noisy_specular, Color(0, 0, 0, 0), 0, 1, 0, 1);
		shade_full_size = p_internal_size;
	}

	if (half_res) {
		if (!shade_sample_diffuse.is_valid() || !shade_sample_specular.is_valid() || shade_sample_size != p_sample_size) {
			ml_free_rid(rd, shade_sample_diffuse);
			ml_free_rid(rd, shade_sample_specular);
			shade_sample_diffuse = ml_create_rgba16f_texture(rd, p_sample_size);
			shade_sample_specular = ml_create_rgba16f_texture(rd, p_sample_size);
			ERR_FAIL_COND_V(!shade_sample_diffuse.is_valid() || !shade_sample_specular.is_valid(), false);
			rd->texture_clear(shade_sample_diffuse, Color(0, 0, 0, MegaLights::HIT_DISTANCE_MISS), 0, 1, 0, 1);
			rd->texture_clear(shade_sample_specular, Color(0, 0, 0, 0), 0, 1, 0, 1);
			shade_sample_size = p_sample_size;
		}
	} else {
		ml_free_rid(rd, shade_sample_diffuse);
		ml_free_rid(rd, shade_sample_specular);
		shade_sample_size = Size2i();
	}
	const uint64_t full_px = uint64_t(MAX(shade_full_size.x, 0)) * uint64_t(MAX(shade_full_size.y, 0));
	const uint64_t sample_px = uint64_t(MAX(shade_sample_size.x, 0)) * uint64_t(MAX(shade_sample_size.y, 0));
	shade_bytes = sizeof(NextGIDenoise::MegaLightsDenoiseParamsGPU) + 2ull * full_px * 8ull + 2ull * sample_px * 8ull;
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_denoise_buffers(const Size2i &p_internal_size, bool p_rr_bypass) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0) {
		return false;
	}
	if (p_rr_bypass) {
		// RR Bypass allocates zero temporal/spatial history textures or scratch targets.
		if (denoise_diffuse[0].is_valid() || denoise_clamp_stats_buffer.is_valid()) {
			_free_denoise_buffers();
		}
		return true;
	}
	if (denoise_diffuse[0].is_valid() && denoise_size == p_internal_size) {
		return true;
	}

	_free_denoise_buffers();

	for (int i = 0; i < 2; i++) {
		denoise_diffuse[i] = ml_create_rgba16f_texture(rd, p_internal_size);
		denoise_specular[i] = ml_create_rgba16f_texture(rd, p_internal_size);
		denoise_moments[i] = ml_create_rgba16f_texture(rd, p_internal_size);

		RD::TextureFormat hlf;
		hlf.texture_type = RD::TEXTURE_TYPE_2D;
		hlf.format = RD::DATA_FORMAT_R32_UINT;
		hlf.width = uint32_t(p_internal_size.x);
		hlf.height = uint32_t(p_internal_size.y);
		hlf.depth = 1;
		hlf.array_layers = 1;
		hlf.mipmaps = 1;
		hlf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		denoise_history_len[i] = rd->texture_create(hlf, RD::TextureView());

		if (!denoise_diffuse[i].is_valid() || !denoise_specular[i].is_valid() || !denoise_moments[i].is_valid() || !denoise_history_len[i].is_valid()) {
			_free_denoise_buffers();
			ERR_FAIL_V_MSG(false, "MegaLights: failed to allocate temporal history textures.");
		}
		rd->texture_clear(denoise_diffuse[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(denoise_specular[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(denoise_moments[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(denoise_history_len[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	denoise_scratch_diffuse = ml_create_rgba16f_texture(rd, p_internal_size);
	denoise_scratch_specular = ml_create_rgba16f_texture(rd, p_internal_size);
	denoise_prev_depth_normal = ml_create_rgba16f_texture(rd, p_internal_size);
	denoise_clamp_stats_buffer = rd->storage_buffer_create(4u * sizeof(uint32_t));

	const int tiles_x = MAX((p_internal_size.x + int(MegaLights::DENOISE_RESET_TILE_SIZE) - 1) / int(MegaLights::DENOISE_RESET_TILE_SIZE), 1);
	const int tiles_y = MAX((p_internal_size.y + int(MegaLights::DENOISE_RESET_TILE_SIZE) - 1) / int(MegaLights::DENOISE_RESET_TILE_SIZE), 1);
	const uint32_t reset_bytes = uint32_t(tiles_x * tiles_y) * sizeof(uint32_t);
	denoise_reset_mask_buffer = rd->storage_buffer_create(reset_bytes);

	if (!denoise_scratch_diffuse.is_valid() || !denoise_scratch_specular.is_valid() || !denoise_prev_depth_normal.is_valid() || !denoise_clamp_stats_buffer.is_valid() || !denoise_reset_mask_buffer.is_valid()) {
		_free_denoise_buffers();
		ERR_FAIL_V_MSG(false, "MegaLights: failed to allocate spatial/guide denoise resources.");
	}

	rd->texture_clear(denoise_scratch_diffuse, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(denoise_scratch_specular, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(denoise_prev_depth_normal, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->buffer_clear(denoise_clamp_stats_buffer, 0, 4u * sizeof(uint32_t));
	rd->buffer_clear(denoise_reset_mask_buffer, 0, reset_bytes);

	denoise_size = p_internal_size;
	denoise_reset_tile_dims = Size2i(tiles_x, tiles_y);
	denoise_history_index = 0;
	denoise_has_history = false;
	denoise_has_cam_transform = false;
	const uint64_t px = uint64_t(p_internal_size.x) * uint64_t(p_internal_size.y);
	// 9 RGBA16F textures (2*diff, 2*spec, 2*moments, scratch diff/spec, prev_depth_normal) + 2 R32U history_len + stats + reset mask.
	denoise_bytes = 9ull * px * 8ull + 2ull * px * 4ull + 4ull * sizeof(uint32_t) + uint64_t(reset_bytes);
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_denoise_debug_texture(const Size2i &p_internal_size) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0) {
		return false;
	}
	if (denoise_debug_texture.is_valid() && denoise_debug_size == p_internal_size) {
		return true;
	}
	_free_denoise_debug_texture();
	denoise_debug_texture = ml_create_rgba16f_texture(rd, p_internal_size);
	ERR_FAIL_COND_V(!denoise_debug_texture.is_valid(), false);
	rd->texture_clear(denoise_debug_texture, Color(0, 0, 0, 1), 0, 1, 0, 1);
	denoise_debug_size = p_internal_size;
	denoise_debug_bytes = uint64_t(p_internal_size.x) * uint64_t(p_internal_size.y) * 8ull;
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_reference_buffers(const Size2i &p_internal_size) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0) {
		return false;
	}
	if (!params_ubo.is_valid()) {
		params_ubo = rd->uniform_buffer_create(sizeof(MegaLights::ParamsUBO));
		ERR_FAIL_COND_V(!params_ubo.is_valid(), false);
	}
	if (reference_accum_texture.is_valid() && reference_accum_size == p_internal_size) {
		return true;
	}
	_free_reference_buffers();
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	tf.width = uint32_t(p_internal_size.x);
	tf.height = uint32_t(p_internal_size.y);
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	reference_accum_texture = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(!reference_accum_texture.is_valid(), false);
	rd->texture_clear(reference_accum_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	reference_accum_size = p_internal_size;
	reference_frame_count = 0;
	reference_camera_hash = 0;
	reference_bytes = uint64_t(p_internal_size.x) * uint64_t(p_internal_size.y) * 16ull;
	transient_bytes = (params_ubo.is_valid() ? sizeof(MegaLights::ParamsUBO) : 0ull) + uint64_t(layer_group_capacity) * sizeof(uint32_t) + uint64_t(light_shadow_policy_capacity) * 4ull * sizeof(uint32_t);
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_verify_buffers(const Size2i &p_internal_size) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_internal_size.x <= 0 || p_internal_size.y <= 0) {
		return false;
	}
	if (verify_accum_texture.is_valid() && verify_counts_buffer.is_valid() && verify_size == p_internal_size) {
		return true;
	}
	_free_verify_buffers();
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	tf.width = uint32_t(p_internal_size.x);
	tf.height = uint32_t(p_internal_size.y);
	tf.depth = 1;
	tf.array_layers = 1;
	tf.mipmaps = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	verify_accum_texture = rd->texture_create(tf, RD::TextureView());
	const uint32_t count_bytes = uint32_t(MAX(uint64_t(p_internal_size.x) * uint64_t(p_internal_size.y), uint64_t(1)) * sizeof(uint32_t));
	verify_counts_buffer = rd->storage_buffer_create(count_bytes);
	ERR_FAIL_COND_V(!verify_accum_texture.is_valid() || !verify_counts_buffer.is_valid(), false);
	rd->texture_clear(verify_accum_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->buffer_clear(verify_counts_buffer, 0, count_bytes);
	verify_size = p_internal_size;
	verify_bytes = uint64_t(p_internal_size.x) * uint64_t(p_internal_size.y) * 16ull + uint64_t(count_bytes);
	_update_memory_accounting();
	return true;
}

bool MegaLightsViewData::ensure_layer_group_buffer(uint32_t p_count) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t required = MAX(p_count, 16u);
	if (layer_group_buffer.is_valid() && layer_group_capacity >= required) {
		return true;
	}
	ml_free_rid(rd, layer_group_buffer);
	layer_group_capacity = MAX(required, 256u);
	layer_group_buffer = rd->storage_buffer_create(layer_group_capacity * sizeof(uint32_t));
	transient_bytes = (params_ubo.is_valid() ? sizeof(MegaLights::ParamsUBO) : 0ull) + uint64_t(layer_group_capacity) * sizeof(uint32_t) + uint64_t(light_shadow_policy_capacity) * 4ull * sizeof(uint32_t);
	_update_memory_accounting();
	return layer_group_buffer.is_valid();
}

bool MegaLightsViewData::ensure_light_shadow_policy_buffer(uint32_t p_count) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t required = MAX(p_count, 16u);
	if (light_shadow_policy_buffer.is_valid() && light_shadow_policy_capacity >= required) {
		return true;
	}
	ml_free_rid(rd, light_shadow_policy_buffer);
	light_shadow_policy_capacity = MAX(required * 2u, 64u);
	light_shadow_policy_buffer = rd->storage_buffer_create(light_shadow_policy_capacity * 4u * sizeof(uint32_t));
	transient_bytes = (params_ubo.is_valid() ? sizeof(MegaLights::ParamsUBO) : 0ull) + uint64_t(layer_group_capacity) * sizeof(uint32_t) + uint64_t(light_shadow_policy_capacity) * 4ull * sizeof(uint32_t);
	_update_memory_accounting();
	return light_shadow_policy_buffer.is_valid();
}

void MegaLightsViewData::reset_guiding() {
	RD *rd = RD::get_singleton();
	if (rd != nullptr && guiding_tile_dims.x > 0 && guiding_tile_dims.y > 0) {
		const uint32_t bytes = uint32_t(uint64_t(guiding_tile_dims.x) * uint64_t(guiding_tile_dims.y) * MegaLights::GUIDING_TILE_WORDS * sizeof(uint32_t));
		if (guiding_buffers[0].is_valid()) {
			rd->buffer_clear(guiding_buffers[0], 0, bytes);
		}
		if (guiding_buffers[1].is_valid()) {
			rd->buffer_clear(guiding_buffers[1], 0, bytes);
		}
	}
	guiding_write_index = 0;
	guiding_has_history = false;
}

void MegaLightsViewData::reset_denoise_history() {
	RD *rd = RD::get_singleton();
	if (rd != nullptr) {
		for (int i = 0; i < 2; i++) {
			if (denoise_history_len[i].is_valid()) {
				rd->texture_clear(denoise_history_len[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
			}
		}
	}
	denoise_has_history = false;
	denoise_has_cam_transform = false;
}

void MegaLightsViewData::advance_denoise_history(uint64_t p_frame, const Transform3D &p_cam_transform, bool p_bypassed) {
	if (p_bypassed) {
		denoise_has_history = false;
	} else {
		denoise_history_index = 1u - denoise_history_index;
		denoise_has_history = true;
	}
	denoise_last_frame = p_frame;
	denoise_last_cam_transform = p_cam_transform;
	denoise_has_cam_transform = true;
}

void MegaLightsViewData::poll_denoise_stats() {
	if (denoise_readback != nullptr && !denoise_readback->pending.is_set() && denoise_readback->has_result) {
		record_denoise_stats(denoise_readback->removed_fp, denoise_readback->total_fp, denoise_readback->clamped_pixels, denoise_readback->valid_pixels, denoise_last_rr_bypass);
		denoise_readback->has_result = false;
	}
}

void MegaLightsViewData::submit_denoise_stats_readback() {
	RD *rd = RD::get_singleton();
	if (rd == nullptr || !denoise_clamp_stats_buffer.is_valid() || denoise_readback == nullptr) {
		return;
	}
	if (!denoise_readback->pending.set_if_clear()) {
		return;
	}
	const uint64_t state_ptr = reinterpret_cast<uint64_t>(denoise_readback);
	denoise_readback->refs.ref();
	if (rd->buffer_get_data_async(denoise_clamp_stats_buffer, callable_mp_static(&ml_denoise_stats_received).bind(state_ptr), 0, 4u * uint32_t(sizeof(uint32_t))) != OK) {
		denoise_readback->pending.clear();
		ml_release_denoise_readback(denoise_readback);
	}
}

void MegaLightsViewData::record_denoise_stats(uint32_t p_removed_fp, uint32_t p_total_fp, uint32_t p_clamped_pixels, uint32_t p_valid_pixels, bool p_rr_bypass) {
	last_denoise_stats.clamped_energy_removed_fp = p_removed_fp;
	last_denoise_stats.total_direct_energy_fp = p_total_fp;
	last_denoise_stats.clamped_energy_ratio = MegaLights::compute_clamped_energy_ratio(p_removed_fp, p_total_fp);
	last_denoise_stats.clamped_pixels = p_clamped_pixels;
	last_denoise_stats.valid_pixels = p_valid_pixels;
	last_denoise_stats.rr_bypass = p_rr_bypass;
}

void MegaLightsViewData::reset_reference() {
	RD *rd = RD::get_singleton();
	if (rd != nullptr && reference_accum_texture.is_valid()) {
		rd->texture_clear(reference_accum_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	reference_frame_count = 0;
	reference_camera_hash = 0;
}

void MegaLightsViewData::advance_guiding_history() {
	guiding_write_index = 1u - guiding_write_index;
	guiding_has_history = true;
}

/* MegaLights Pure Device-Free Helpers */

bool MegaLights::is_enabled() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	return ps != nullptr && ps->has_setting(SETTING_ENABLED) && bool(ps->get_setting(SETTING_ENABLED));
}

bool MegaLights::is_front_layer_enabled() {
	if (!is_enabled()) {
		return false;
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	return ps != nullptr && ps->has_setting(SETTING_FRONT_LAYER_TRANSLUCENCY) && bool(ps->get_setting(SETTING_FRONT_LAYER_TRANSLUCENCY));
}

bool MegaLights::effective_directional_lights() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const bool setting_on = ps != nullptr && ps->has_setting(SETTING_DIRECTIONAL_LIGHTS) && bool(ps->get_setting(SETTING_DIRECTIONAL_LIGHTS));
	return setting_on || check_wants_directional_lights();
}

int MegaLights::read_nextgi_quality_level() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	NextGI::Quality setting = NextGI::QUALITY_AUTO;
	if (ps != nullptr && ps->has_setting("rendering/global_illumination/nextgi/quality")) {
		setting = NextGI::Quality(CLAMP(int(ps->get_setting("rendering/global_illumination/nextgi/quality")), 0, NextGI::QUALITY_MAX - 1));
	}
	return int(NextGI::resolve_quality(setting));
}

MegaLights::DownsampleMode MegaLights::downsample_for_quality_preset(int p_quality_level) {
	// Low (0), Medium (1), High (2) use half-res 2x2 rotating downsample; Epic (3) uses full resolution.
	return p_quality_level >= 3 ? DOWNSAMPLE_FULL : DOWNSAMPLE_HALF;
}

MegaLights::QualityPreset MegaLights::quality_preset_for_level(int p_quality_level) {
	QualityPreset preset;
	preset.samples_per_pixel = samples_for_quality_preset(p_quality_level);
	preset.downsample = downsample_for_quality_preset(p_quality_level);
	preset.screen_trace_steps = screen_trace_steps_for_quality(p_quality_level);
	return preset;
}

bool MegaLights::light_uses_megalights(bool p_megalights_active, RSE::LightMegaLightsMode p_mode, RSE::LightType p_type, bool p_directional_lights_enabled) {
	if (!p_megalights_active) {
		return false;
	}
	if (p_mode == RSE::LIGHT_MEGALIGHTS_DISALLOW) {
		return false;
	}
	if (p_mode == RSE::LIGHT_MEGALIGHTS_ALLOW) {
		return true;
	}
	if (p_type == RSE::LIGHT_DIRECTIONAL) {
		return p_directional_lights_enabled;
	}
	return true;
}

uint64_t MegaLights::get_froxel_allocated_bytes() {
	return ml_froxel_allocated_bytes.get();
}

uint64_t MegaLights::get_front_layer_allocated_bytes() {
	return ml_front_layer_allocated_bytes.get();
}

uint64_t MegaLights::get_d17_direct_allocated_bytes() {
	return NextGIWRC::get_total_translucency_direct_bytes();
}

uint64_t MegaLights::get_total_allocated_bytes() {
	return ml_total_allocated_bytes.get() + NextGIWRC::get_total_translucency_direct_bytes();
}

MegaLights::DownsampleMode MegaLights::parse_downsample_variant(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		const String s = String(p_value).to_lower().strip_edges();
		if (s == "full" || s == "0") {
			return DOWNSAMPLE_FULL;
		}
		if (s == "auto" || s == "2") {
			return DOWNSAMPLE_AUTO;
		}
		return DOWNSAMPLE_HALF;
	}
	const int v = int(p_value);
	if (v == int(DOWNSAMPLE_FULL)) {
		return DOWNSAMPLE_FULL;
	}
	if (v == int(DOWNSAMPLE_AUTO)) {
		return DOWNSAMPLE_AUTO;
	}
	return DOWNSAMPLE_HALF;
}

MegaLights::DenoiserMode MegaLights::parse_denoiser_variant(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		const String s = String(p_value).to_lower().strip_edges();
		if (s == "rr_bypass" || s == "rr bypass" || s == "rr" || s == "bypass" || s == "1") {
			return DENOISER_RR_BYPASS;
		}
		return DENOISER_BUILTIN;
	}
	return int(p_value) == int(DENOISER_RR_BYPASS) ? DENOISER_RR_BYPASS : DENOISER_BUILTIN;
}

int MegaLights::debug_vis_mode_from_viewport(RSE::ViewportDebugDraw p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_DEBUG_DRAW_ML_NOISY:
			return 0;
		case RSE::VIEWPORT_DEBUG_DRAW_ML_DENOISED:
			return 1;
		case RSE::VIEWPORT_DEBUG_DRAW_ML_VARIANCE:
			return 2;
		default:
			return -1;
	}
}

MegaLights::Settings MegaLights::validate_settings(const Settings &p_settings) {
	Settings out = p_settings;
	out.samples_per_pixel = CLAMP(out.samples_per_pixel, MIN_SAMPLES_PER_PIXEL, MAX_SAMPLES_PER_PIXEL);
	out.downsample = out.downsample == DOWNSAMPLE_FULL ? DOWNSAMPLE_FULL : DOWNSAMPLE_HALF;
	out.screen_trace_steps = CLAMP(out.screen_trace_steps, 1u, 64u);
	out.denoiser = out.denoiser == DENOISER_RR_BYPASS ? DENOISER_RR_BYPASS : DENOISER_BUILTIN;
	if (!Math::is_finite(out.falloff_cull_threshold) || out.falloff_cull_threshold < 0.0f) {
		out.falloff_cull_threshold = DEFAULT_FALLOFF_CULL_THRESHOLD;
	}
	if (!Math::is_finite(out.distant_screen_traces_length) || out.distant_screen_traces_length < 0.0f) {
		out.distant_screen_traces_length = DEFAULT_DISTANT_SCREEN_TRACES_LENGTH;
	} else {
		out.distant_screen_traces_length = CLAMP(out.distant_screen_traces_length, 0.0f, 64.0f);
	}
	return out;
}

MegaLights::Settings MegaLights::read_settings() {
	Settings s;
	const QualityPreset preset = quality_preset_for_level(read_nextgi_quality_level());
	s.samples_per_pixel = preset.samples_per_pixel;
	s.downsample = preset.downsample;
	s.screen_trace_steps = preset.screen_trace_steps;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return validate_settings(s);
	}
	if (ps->has_setting(SETTING_ENABLED)) {
		s.enabled = bool(ps->get_setting(SETTING_ENABLED));
	}
	if (ps->has_setting(SETTING_SAMPLES_PER_PIXEL)) {
		const int raw_spp = int(ps->get_setting(SETTING_SAMPLES_PER_PIXEL));
		s.samples_per_pixel = raw_spp <= 0 ? preset.samples_per_pixel : uint32_t(raw_spp);
	}
	if (ps->has_setting(SETTING_DOWNSAMPLE)) {
		const DownsampleMode raw_ds = parse_downsample_variant(ps->get_setting(SETTING_DOWNSAMPLE));
		s.downsample = (raw_ds == DOWNSAMPLE_AUTO) ? preset.downsample : raw_ds;
	}
	if (ps->has_setting(SETTING_DIRECTIONAL_LIGHTS)) {
		s.directional_lights = bool(ps->get_setting(SETTING_DIRECTIONAL_LIGHTS));
	}
	if (ps->has_setting(SETTING_FALLOFF_CULL_THRESHOLD)) {
		s.falloff_cull_threshold = float(ps->get_setting(SETTING_FALLOFF_CULL_THRESHOLD));
	}
	if (ps->has_setting(SETTING_REFERENCE_MODE)) {
		s.reference_mode = bool(ps->get_setting(SETTING_REFERENCE_MODE));
	}
	if (ps->has_setting(SETTING_SCREEN_TRACES)) {
		s.screen_traces = bool(ps->get_setting(SETTING_SCREEN_TRACES));
	}
	if (ps->has_setting(SETTING_DISTANT_SCREEN_TRACES_LENGTH)) {
		s.distant_screen_traces_length = float(ps->get_setting(SETTING_DISTANT_SCREEN_TRACES_LENGTH));
	}
	if (ps->has_setting(SETTING_FAR_FIELD)) {
		s.far_field = bool(ps->get_setting(SETTING_FAR_FIELD));
	}
	if (ps->has_setting(SETTING_SWRT_CONTINUATION)) {
		s.swrt_continuation = bool(ps->get_setting(SETTING_SWRT_CONTINUATION));
	}
	if (ps->has_setting(SETTING_DENOISER)) {
		s.denoiser = parse_denoiser_variant(ps->get_setting(SETTING_DENOISER));
	}
	if (ps->has_setting(SETTING_FRONT_LAYER_TRANSLUCENCY)) {
		s.front_layer_translucency = bool(ps->get_setting(SETTING_FRONT_LAYER_TRANSLUCENCY));
	}
	return validate_settings(s);
}

uint32_t MegaLights::samples_for_quality_preset(int p_quality_level) {
	// Provisional quality mapping (Low/Medium=2, High=4, Epic=8); tuned in PQ.
	if (p_quality_level <= 1) {
		return 2u;
	}
	if (p_quality_level == 2) {
		return 4u;
	}
	return 8u;
}

uint32_t MegaLights::froxel_samples_for_quality_preset(int p_quality_level) {
	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7).
	// Default 1 sample per froxel per frame on Low/Medium/High, 2 on Epic.
	return p_quality_level >= 3 ? 2u : 1u;
}

void MegaLights::sh_l1_project_directional(const Vector3 &p_dir_ws, const Color &p_radiance, Vector4 &r_sh_r, Vector4 &r_sh_g, Vector4 &r_sh_b) {
	const Vector3 d = p_dir_ws.length_squared() > 1e-12f ? p_dir_ws.normalized() : Vector3(0.0f, 1.0f, 0.0f);
	constexpr float c0 = 0.25f / float(Math::PI);
	constexpr float c1 = 0.5f / float(Math::PI);
	r_sh_r += Vector4(p_radiance.r * c0, p_radiance.r * c1 * float(d.x), p_radiance.r * c1 * float(d.y), p_radiance.r * c1 * float(d.z));
	r_sh_g += Vector4(p_radiance.g * c0, p_radiance.g * c1 * float(d.x), p_radiance.g * c1 * float(d.y), p_radiance.g * c1 * float(d.z));
	r_sh_b += Vector4(p_radiance.b * c0, p_radiance.b * c1 * float(d.x), p_radiance.b * c1 * float(d.y), p_radiance.b * c1 * float(d.z));
}

Color MegaLights::sh_l1_evaluate(const Vector4 &p_sh_r, const Vector4 &p_sh_g, const Vector4 &p_sh_b, const Vector3 &p_normal_ws) {
	const Vector3 n = p_normal_ws.length_squared() > 1e-12f ? p_normal_ws.normalized() : Vector3(0.0f, 1.0f, 0.0f);
	const float r = MAX(float(p_sh_r.x) + float(n.x) * float(p_sh_r.y) + float(n.y) * float(p_sh_r.z) + float(n.z) * float(p_sh_r.w), 0.0f);
	const float g = MAX(float(p_sh_g.x) + float(n.x) * float(p_sh_g.y) + float(n.y) * float(p_sh_g.z) + float(n.z) * float(p_sh_g.w), 0.0f);
	const float b = MAX(float(p_sh_b.x) + float(n.x) * float(p_sh_b.y) + float(n.y) * float(p_sh_b.z) + float(n.z) * float(p_sh_b.w), 0.0f);
	return Color(r, g, b, 1.0f);
}

float MegaLights::sss_transmission_lobe(float p_ndotl, float p_boost) {
	return CLAMP(p_boost - p_ndotl, 0.0f, 1.0f) / float(Math::PI);
}

float MegaLights::sss_transmission_attenuation(float p_hit_dist, float p_visibility, float p_extinction) {
	if (p_visibility > 0.5f) {
		return 1.0f;
	}
	if (Math::is_finite(p_hit_dist) && p_hit_dist >= 0.0f && p_hit_dist < HIT_DISTANCE_MISS * 0.99f) {
		return std::exp(-p_hit_dist * MAX(p_extinction, 0.0f));
	}
	return 0.0f;
}

uint64_t MegaLights::estimate_d17_direct_bytes(bool p_megalights_enabled, const Size2i &p_screen_size) {
	if (!p_megalights_enabled || p_screen_size.x <= 0 || p_screen_size.y <= 0) {
		return 0ULL;
	}
	const uint64_t w = uint64_t(MAX((p_screen_size.x + 15) / 16, 1));
	const uint64_t h = uint64_t(MAX((p_screen_size.y + 15) / 16, 1));
	// 3 channels (R,G,B) x 2 ping-pong history slots x RGBA16F (8 bytes/texel) x 32 slices.
	return w * h * uint64_t(D17_VOLUME_SLICES) * 6ULL * 8ULL;
}

uint64_t MegaLights::estimate_front_layer_bytes(bool p_megalights_enabled, bool p_front_layer_enabled, const Size2i &p_screen_size, uint32_t p_spp, DownsampleMode p_downsample) {
	if (!p_megalights_enabled || !p_front_layer_enabled || p_screen_size.x <= 0 || p_screen_size.y <= 0) {
		return 0ULL;
	}
	const uint64_t full_px = uint64_t(p_screen_size.x) * uint64_t(p_screen_size.y);
	const Size2i sample_sz = compute_sample_grid_size(p_screen_size, p_downsample);
	const uint64_t sample_px = uint64_t(sample_sz.x) * uint64_t(sample_sz.y);
	const uint64_t spp = CLAMP(p_spp, MIN_SAMPLES_PER_PIXEL, MAX_SAMPLES_PER_PIXEL);
	// Front-layer M1 neutral guides (2x RGBA8) + sample/vis/hit + denoise history (diffuse/specular/moments/history_len/prev_dn).
	const uint64_t guide_bytes = full_px * 8ULL;
	const uint64_t sample_bytes = sample_px * spp * (16ULL + 4ULL + 2ULL);
	const uint64_t denoise_bytes = full_px * (6ULL * 8ULL + 2ULL * 4ULL + 2ULL * 2ULL + 8ULL);
	return guide_bytes + sample_bytes + denoise_bytes;
}

Vector2i MegaLights::downsample_offset(uint32_t p_frame_index) {
	switch (p_frame_index & 3u) {
		case 1u:
			return Vector2i(1, 1);
		case 2u:
			return Vector2i(1, 0);
		case 3u:
			return Vector2i(0, 1);
		default:
			return Vector2i(0, 0);
	}
}

Size2i MegaLights::compute_sample_grid_size(const Size2i &p_internal_size, DownsampleMode p_downsample) {
	const int w = MAX(p_internal_size.x, 1);
	const int h = MAX(p_internal_size.y, 1);
	if (p_downsample == DOWNSAMPLE_FULL) {
		return Size2i(w, h);
	}
	return Size2i((w + 1) / 2, (h + 1) / 2);
}

Vector2i MegaLights::sample_to_full_pixel(const Vector2i &p_sample_px, const Size2i &p_full_size, DownsampleMode p_downsample, uint32_t p_frame_index) {
	const int max_x = MAX(p_full_size.x - 1, 0);
	const int max_y = MAX(p_full_size.y - 1, 0);
	if (p_downsample == DOWNSAMPLE_FULL) {
		return Vector2i(CLAMP(p_sample_px.x, 0, max_x), CLAMP(p_sample_px.y, 0, max_y));
	}
	const Vector2i off = downsample_offset(p_frame_index);
	return Vector2i(CLAMP(p_sample_px.x * 2 + off.x, 0, max_x), CLAMP(p_sample_px.y * 2 + off.y, 0, max_y));
}

Size2i MegaLights::compute_guiding_tile_dims(const Size2i &p_full_size) {
	const int w = MAX(p_full_size.x, 1);
	const int h = MAX(p_full_size.y, 1);
	return Size2i((w + int(GUIDING_TILE_SIZE) - 1) / int(GUIDING_TILE_SIZE), (h + int(GUIDING_TILE_SIZE) - 1) / int(GUIDING_TILE_SIZE));
}

uint32_t MegaLights::pack_sample_light(uint32_t p_type, uint32_t p_index) {
	return (p_index & SAMPLE_INDEX_MASK) | ((p_type & SAMPLE_TYPE_MASK) << SAMPLE_TYPE_SHIFT);
}

uint32_t MegaLights::unpack_sample_light_type(uint32_t p_packed) {
	return (p_packed >> SAMPLE_TYPE_SHIFT) & SAMPLE_TYPE_MASK;
}

uint32_t MegaLights::unpack_sample_light_index(uint32_t p_packed) {
	return p_packed & SAMPLE_INDEX_MASK;
}

uint32_t MegaLights::pack_guiding_entry(uint32_t p_light_packed, uint32_t p_count) {
	const uint32_t clamped = MIN(p_count, GUIDING_COUNT_MAX);
	if (clamped == 0u) {
		return SAMPLE_INVALID;
	}
	return (p_light_packed & GUIDING_LIGHT_MASK) | (clamped << GUIDING_COUNT_SHIFT);
}

uint32_t MegaLights::unpack_guiding_light(uint32_t p_entry) {
	return p_entry & GUIDING_LIGHT_MASK;
}

uint32_t MegaLights::unpack_guiding_count(uint32_t p_entry) {
	if (p_entry == SAMPLE_INVALID) {
		return 0u;
	}
	return p_entry >> GUIDING_COUNT_SHIFT;
}

uint32_t MegaLights::pack_oct_snorm16(const Vector3 &p_dir) {
	const float l1 = Math::abs(p_dir.x) + Math::abs(p_dir.y) + Math::abs(p_dir.z);
	if (!(l1 > 1e-12f)) {
		return 0u;
	}
	float nx = p_dir.x / l1;
	float ny = p_dir.y / l1;
	const float nz = p_dir.z / l1;
	if (nz < 0.0f) {
		const float ox = (1.0f - Math::abs(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
		const float oy = (1.0f - Math::abs(nx)) * (ny >= 0.0f ? 1.0f : -1.0f);
		nx = ox;
		ny = oy;
	}
	const int16_t sx = int16_t(Math::round(CLAMP(nx, -1.0f, 1.0f) * 32767.0f));
	const int16_t sy = int16_t(Math::round(CLAMP(ny, -1.0f, 1.0f) * 32767.0f));
	return uint32_t(uint16_t(sx)) | (uint32_t(uint16_t(sy)) << 16u);
}

Vector3 MegaLights::unpack_oct_snorm16(uint32_t p_word) {
	const int16_t sx = int16_t(p_word & 0xFFFFu);
	const int16_t sy = int16_t((p_word >> 16u) & 0xFFFFu);
	float ex = CLAMP(float(sx) / 32767.0f, -1.0f, 1.0f);
	float ey = CLAMP(float(sy) / 32767.0f, -1.0f, 1.0f);
	float ez = 1.0f - Math::abs(ex) - Math::abs(ey);
	if (ez < 0.0f) {
		const float ox = (1.0f - Math::abs(ey)) * (ex >= 0.0f ? 1.0f : -1.0f);
		const float oy = (1.0f - Math::abs(ex)) * (ey >= 0.0f ? 1.0f : -1.0f);
		ex = ox;
		ey = oy;
	}
	return Vector3(ex, ey, ez).normalized();
}

float MegaLights::omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = MAX(1.0f - nd, 0.0f);
	nd *= nd;
	return nd * std::pow(MAX(p_distance, 0.0001f), -p_decay);
}

bool MegaLights::passes_falloff_cull(float p_attenuation, float p_threshold) {
	return p_attenuation > 0.0f && p_attenuation >= p_threshold;
}

float MegaLights::guiding_weight(uint32_t p_visible_count, uint32_t p_max_tile_count, bool p_has_tile_history, float p_floor) {
	if (!p_has_tile_history) {
		return 1.0f;
	}
	const float floor_clamped = CLAMP(p_floor, 0.01f, 1.0f);
	if (p_visible_count == 0u) {
		return floor_clamped;
	}
	const float denom = float(MAX(p_max_tile_count, 1u));
	const float confidence = CLAMP(float(p_visible_count) / denom, 0.25f, 1.0f);
	return Math::lerp(floor_clamped, 1.0f, confidence);
}

void MegaLights::aggregate_guiding_tile_cpu(const uint32_t *p_visible_lights, uint32_t p_count, LocalVector<GuidingEntry> &r_top32) {
	r_top32.clear();
	if (p_visible_lights == nullptr || p_count == 0u) {
		return;
	}
	HashMap<uint32_t, uint32_t> counts;
	for (uint32_t i = 0; i < p_count; i++) {
		const uint32_t lid = p_visible_lights[i] & GUIDING_LIGHT_MASK;
		if (p_visible_lights[i] == SAMPLE_INVALID) {
			continue;
		}
		if (uint32_t *existing = counts.getptr(lid)) {
			(*existing)++;
		} else {
			counts.insert(lid, 1u);
		}
	}
	LocalVector<GuidingEntry> all;
	all.reserve(counts.size());
	for (const KeyValue<uint32_t, uint32_t> &kv : counts) {
		GuidingEntry e;
		e.light_packed = kv.key;
		e.count = MIN(kv.value, GUIDING_COUNT_MAX);
		all.push_back(e);
	}
	if (all.size() > 1u) {
		SortArray<GuidingEntry, GuidingEntryCompare> sorter;
		sorter.sort(all.ptr(), all.size());
	}
	const uint32_t keep = MIN(uint32_t(all.size()), GUIDING_MAX_LIGHTS);
	r_top32.resize(keep);
	for (uint32_t i = 0; i < keep; i++) {
		r_top32[i] = all[i];
	}
}

void MegaLights::sample_reservoirs_cpu(const float *p_weights, uint32_t p_light_count, uint32_t p_k, uint64_t &r_rng_state, LocalVector<ReservoirSample> &r_out) {
	const uint32_t k_count = CLAMP(p_k, MIN_SAMPLES_PER_PIXEL, MAX_SAMPLES_PER_PIXEL);
	r_out.resize(k_count);
	LocalVector<float> chosen_w;
	chosen_w.resize(k_count);
	for (uint32_t k = 0; k < k_count; k++) {
		r_out[k].light_index = SAMPLE_INVALID;
		r_out[k].inv_pdf = 0.0f;
		chosen_w[k] = 0.0f;
	}
	if (p_weights == nullptr || p_light_count == 0u) {
		return;
	}
	double w_sum = 0.0;
	for (uint32_t i = 0; i < p_light_count; i++) {
		const float w = p_weights[i];
		if (!(w > 0.0f)) {
			continue;
		}
		w_sum += double(w);
		const double accept_p = double(w) / w_sum;
		for (uint32_t k = 0; k < k_count; k++) {
			const double u = double(ml_xorshift32_from_u64(r_rng_state)) * (1.0 / 4294967296.0);
			if (u < accept_p) {
				r_out[k].light_index = i;
				chosen_w[k] = w;
			}
		}
	}
	if (w_sum > 0.0) {
		for (uint32_t k = 0; k < k_count; k++) {
			if (r_out[k].light_index != SAMPLE_INVALID && chosen_w[k] > 0.0f) {
				r_out[k].inv_pdf = float(w_sum / double(chosen_w[k]));
			}
		}
	}
}

float MegaLights::welford_relative_std_error(float p_mean_luma, float p_m2, uint32_t p_sample_count) {
	if (p_sample_count < 2u || !(p_mean_luma > 1e-6f) || !(p_m2 > 0.0f)) {
		return 0.0f;
	}
	const double variance = double(p_m2) / double(p_sample_count - 1u);
	const double std_err = std::sqrt(variance / double(p_sample_count));
	return float(std_err / double(p_mean_luma));
}

// GodotNext: megalights_visibility (#146). Pure device-free visibility chain helpers.

uint32_t MegaLights::screen_trace_steps_for_quality(int p_quality_level) {
	if (p_quality_level <= 0) {
		return 8u; // Low
	}
	if (p_quality_level == 1) {
		return 12u; // Medium
	}
	if (p_quality_level == 2) {
		return 16u; // High
	}
	return 24u; // Epic
}

float MegaLights::screen_trace_max_distance(float p_linear_depth, float p_near_length, float p_distant_length, float p_distant_start_depth) {
	const float near_len = MAX(p_near_length, 0.0f);
	const float dist_len = MAX(p_distant_length, 0.0f);
	const float start_d = MAX(p_distant_start_depth, 0.1f);
	const float edge0 = start_d * 0.5f;
	const float edge1 = start_d * 2.0f;
	const float t = CLAMP((MAX(p_linear_depth, 0.0f) - edge0) / MAX(edge1 - edge0, 1e-4f), 0.0f, 1.0f);
	const float s = t * t * (3.0f - 2.0f * t);
	return Math::lerp(near_len, dist_len, s);
}

float MegaLights::screen_trace_surface_thickness(float p_linear_depth, float p_surface_thickness_setting) {
	return MAX(0.02f, MAX(p_linear_depth, 0.0f) * MAX(p_surface_thickness_setting, 0.001f));
}

float MegaLights::dedupe_depth_tolerance(float p_linear_depth, float p_normal_vs_z, float p_proj_y, uint32_t p_full_height, bool p_downsample_half, bool p_orthogonal) {
	const float denom = MAX(Math::abs(p_proj_y) * float(MAX(p_full_height, 1u)), 1e-4f);
	const float pixel_world = p_orthogonal ? (2.0f / denom) : (2.0f * MAX(p_linear_depth, 1e-4f) / denom);
	const float step_scale = p_downsample_half ? 2.0f : 1.0f;
	const float nz = CLAMP(p_normal_vs_z, -1.0f, 1.0f);
	const float slope_factor = CLAMP(std::sqrt(MAX(1.0f - nz * nz, 0.0f)) / MAX(Math::abs(nz), 0.25f), 0.5f, 2.5f);
	return MAX(pixel_world * step_scale * slope_factor, 0.01f);
}

bool MegaLights::can_dedupe_ray(uint32_t p_light_a, uint32_t p_light_b, float p_depth_a, float p_depth_b, float p_tol_a, float p_tol_b, const Vector3 &p_normal_a, const Vector3 &p_normal_b, const Vector3 &p_dir_a, const Vector3 &p_dir_b) {
	if (p_light_a == SAMPLE_INVALID || p_light_a != p_light_b) {
		return false;
	}
	if (!(p_depth_a > 0.0f) || !(p_depth_b > 0.0f)) {
		return false;
	}
	if (Math::abs(p_depth_a - p_depth_b) > MAX(p_tol_a, p_tol_b)) {
		return false;
	}
	if (p_normal_a.normalized().dot(p_normal_b.normalized()) < 0.90f) {
		return false;
	}
	if (p_dir_a.normalized().dot(p_dir_b.normalized()) < 0.995f) {
		return false;
	}
	return true;
}

MegaLights::DedupeBlockResult MegaLights::compact_and_dedupe_2x2_cpu(const DedupeBlockSample *p_samples, uint32_t p_spp, bool p_dedupe_enabled) {
	DedupeBlockResult res;
	const uint32_t spp = CLAMP(p_spp, MIN_SAMPLES_PER_PIXEL, MAX_SAMPLES_PER_PIXEL);
	const uint32_t total = 4u * spp;
	res.sample_ray_map.resize(total);
	for (uint32_t i = 0; i < total; i++) {
		res.sample_ray_map[i] = 0xFFFFFFFFu;
	}
	if (p_samples == nullptr) {
		return res;
	}

	uint32_t active_light[4][MAX_SAMPLES_PER_PIXEL];
	uint32_t leader_ray[4][MAX_SAMPLES_PER_PIXEL];
	for (uint32_t sub = 0; sub < 4u; sub++) {
		for (uint32_t k = 0; k < spp; k++) {
			active_light[sub][k] = SAMPLE_INVALID;
			leader_ray[sub][k] = 0xFFFFFFFFu;
			const uint32_t idx = sub * spp + k;
			const DedupeBlockSample &s = p_samples[idx];
			if (s.light_packed == SAMPLE_INVALID || !(s.linear_depth > 0.0f)) {
				continue;
			}
			if (!s.casts_shadow) {
				res.unshadowed_count++;
				continue;
			}
			if (s.use_vsm) {
				res.sample_ray_map[idx] = 0x80000000u | res.vsm_count;
				res.vsm_count++;
				continue;
			}
			if (s.screen_hit) {
				res.screen_hit_count++;
				continue;
			}
			active_light[sub][k] = s.light_packed;
		}
	}

	for (uint32_t sub = 0; sub < 4u; sub++) {
		for (uint32_t k = 0; k < spp; k++) {
			const uint32_t my_light = active_light[sub][k];
			if (my_light == SAMPLE_INVALID) {
				continue;
			}
			const uint32_t idx = sub * spp + k;
			const DedupeBlockSample &s = p_samples[idx];
			uint32_t assigned = 0xFFFFFFFFu;

			if (p_dedupe_enabled && sub > 0u) {
				for (uint32_t pass = 0u; pass < 2u && assigned == 0xFFFFFFFFu; pass++) {
					for (uint32_t prev = 0u; prev < sub && assigned == 0xFFFFFFFFu; prev++) {
						const uint32_t k_begin = (pass == 0u) ? k : 0u;
						const uint32_t k_end = (pass == 0u) ? (k + 1u) : spp;
						for (uint32_t ok = k_begin; ok < k_end; ok++) {
							if (leader_ray[prev][ok] == 0xFFFFFFFFu) {
								continue;
							}
							const DedupeBlockSample &o = p_samples[prev * spp + ok];
							if (can_dedupe_ray(my_light, active_light[prev][ok], s.linear_depth, o.linear_depth, s.depth_tol, o.depth_tol, s.world_normal, o.world_normal, s.ray_dir, o.ray_dir)) {
								assigned = leader_ray[prev][ok];
								break;
							}
						}
					}
				}
			}

			if (assigned != 0xFFFFFFFFu) {
				res.sample_ray_map[idx] = assigned;
				res.ray_min_t_resume[assigned] = MIN(res.ray_min_t_resume[assigned], MAX(s.t_resume, 0.0f));
				res.deduped_count++;
			} else {
				const uint32_t ray_idx = res.ray_count++;
				leader_ray[sub][k] = ray_idx;
				res.sample_ray_map[idx] = ray_idx;
				res.ray_min_t_resume.push_back(MAX(s.t_resume, 0.0f));
			}
		}
	}
	return res;
}

bool MegaLights::light_uses_vsm(RSE::LightMegaLightsShadowMethod p_method, bool p_vsm_available) {
	return p_method == RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_VIRTUAL && p_vsm_available;
}

MegaLights::TierStatus MegaLights::resolve_visibility_tier(const TierInputs &p_inputs) {
	TierStatus st;
	st.vsm_supported = p_inputs.vsm_available;
	if (!p_inputs.forward_plus) {
		st.tier = VISIBILITY_TIER_DISABLED;
		st.tier_name = "UNAVAILABLE";
		st.backend = "none";
		st.status_label = "Unavailable";
		st.status_warning = "MegaLights is unavailable in Mobile and Compatibility renderers; switch to the Forward+ renderer.";
		st.reason = "UNSUPPORTED_RENDERER";
		return st;
	}

	if (p_inputs.supports_ray_query && p_inputs.nextgi_hwrt_available) {
		if (p_inputs.is_metal) {
			if (p_inputs.is_apple_m2_or_newer) {
				st.tier = VISIBILITY_TIER_HWRT_INLINE_ONLY;
				st.experimental = true;
				st.inline_ray_query_only = true;
				st.retrace_supported = false;
				st.far_field_supported = p_inputs.far_field_requested;
				st.tier_name = "HWRT_INLINE_ONLY";
				st.backend = "metal_ray_query";
				st.status_label = "Experimental";
				st.status_warning = "Experimental on Apple M2+ (inline ray query only).";
				st.experimental_label = "Experimental (Apple M2+ inline ray query only)";
				st.reason = "METAL_INLINE_RAY_QUERY_EXPERIMENTAL";
				return st;
			}
		} else {
			st.tier = VISIBILITY_TIER_HWRT;
			st.experimental = false;
			st.inline_ray_query_only = false;
			st.retrace_supported = p_inputs.supports_ray_tracing_pipeline;
			st.far_field_supported = p_inputs.far_field_requested;
			st.tier_name = "HWRT";
			st.backend = "hwrt_ray_query";
			st.status_label = "Production";
			st.status_warning = "";
			st.reason = "HWRT_RAY_QUERY_AVAILABLE";
			return st;
		}
	}

	if (p_inputs.swrt_continuation_requested && p_inputs.df_sdf_available) {
		st.tier = VISIBILITY_TIER_SWRT_EXTENSION;
		st.swrt_extension = true;
		st.tier_name = "SWRT_EXTENSION";
		st.backend = "swrt_global_sdf";
		st.status_label = "Extension";
		st.status_warning = "Using non-RT SWRT global SDF continuation extension (hardware ray tracing recommended).";
		st.experimental_label = "Extension (SWRT global SDF continuation)";
		st.reason = "NON_RT_SWRT_CONTINUATION_EXTENSION";
		return st;
	}

	st.tier = VISIBILITY_TIER_SCREEN_ONLY;
	st.tier_name = "SCREEN_ONLY";
	st.backend = "screen_trace";
	st.status_label = "Screen-Trace Fallback";
	st.status_warning = "Hardware ray tracing is disabled or unavailable; MegaLights shadows use screen-space traces only. Enable NextGI hardware ray tracing for full MegaLights shadows.";
	st.reason = p_inputs.swrt_continuation_requested ? "SWRT_SDF_UNAVAILABLE" : "NO_HWRT_RAY_QUERY";
	return st;
}

Dictionary MegaLights::get_tier_info() {
	TierInputs in;
	RD *rd = RD::get_singleton();
	const Settings s = read_settings();
	const String method = OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_method() : String();
	in.forward_plus = (rd != nullptr) && (method.is_empty() || method == "forward_plus");
	in.screen_traces_enabled = s.screen_traces;
	in.far_field_requested = s.far_field;
	in.swrt_continuation_requested = s.swrt_continuation;
	in.vsm_available = RendererVirtualShadowMapStorage::is_vsm_available();
	in.df_sdf_available = RendererVirtualShadowMapStorage::is_df_available();

	if (rd != nullptr) {
		in.supports_ray_query = rd->has_feature(RD::SUPPORTS_RAY_QUERY);
		in.supports_ray_tracing_pipeline = rd->has_feature(RD::SUPPORTS_RAYTRACING_PIPELINE);
		const String api = rd->get_device_api_name().to_lower();
		const String dev_name = rd->get_device_name();
		in.is_metal = api.contains("metal") || (OS::get_singleton() != nullptr && OS::get_singleton()->get_name() == "macOS" && !api.contains("vulkan"));
		if (in.is_metal) {
			// Apple M2+ supports hardware inline ray queries via #171 (M1 does not expose SUPPORTS_RAY_QUERY).
			in.is_apple_m2_or_newer = in.supports_ray_query && !dev_name.contains("Apple M1");
		}
	}
	in.nextgi_hwrt_available = RendererVirtualShadowMapStorage::is_rt_available();

	const TierStatus st = resolve_visibility_tier(in);
	Dictionary info;
	info["enabled"] = s.enabled;
	info["tier"] = String(st.tier_name);
	info["backend"] = String(st.backend);
	info["status_label"] = String(st.status_label);
	info["status_warning"] = String(st.status_warning);
	info["experimental"] = st.experimental;
	info["experimental_label"] = String(st.experimental_label);
	info["inline_ray_query_only"] = st.inline_ray_query_only;
	info["retrace_supported"] = st.retrace_supported;
	info["swrt_extension"] = st.swrt_extension;
	info["far_field_enabled"] = st.far_field_supported;
	info["screen_traces_enabled"] = in.screen_traces_enabled;
	info["distant_screen_traces_length"] = double(s.distant_screen_traces_length);
	info["vsm_supported"] = st.vsm_supported;
	info["quality_level"] = read_nextgi_quality_level();
	info["samples_per_pixel"] = int(s.samples_per_pixel);
	info["downsample"] = (s.downsample == DOWNSAMPLE_FULL) ? String("full") : String("half");
	info["screen_trace_steps"] = int(s.screen_trace_steps);
	info["memory_bytes"] = int64_t(get_total_allocated_bytes());
	info["froxel_memory_bytes"] = int64_t(get_froxel_allocated_bytes());
	info["d17_direct_memory_bytes"] = int64_t(get_d17_direct_allocated_bytes());
	info["front_layer_memory_bytes"] = int64_t(get_front_layer_allocated_bytes());
	info["front_layer_translucency"] = s.front_layer_translucency;
	const uint32_t fmode = s.enabled ? ml_last_froxel_dispatch_mode.get() : 0u;
	info["froxel_dispatch_mode"] = fmode == 1u ? String("shared_fog_d17") : (fmode == 2u ? String("fog_only") : (fmode == 3u ? String("d17_only") : String("off")));
	info["froxel_samples_per_cell"] = s.enabled ? int(ml_last_froxel_samples_per_cell.get()) : 0;
	info["froxel_dispatched_cells"] = s.enabled ? int64_t(ml_last_froxel_dispatched_cells.get()) : int64_t(0);
	info["denoiser"] = (s.denoiser == DENOISER_RR_BYPASS) ? String("rr_bypass") : String("builtin");
	info["rr_bypass"] = (s.denoiser == DENOISER_RR_BYPASS);
	info["reason"] = String(st.reason);
	return info;
}

// GodotNext: megalights_denoiser (#135). Pure device-free demodulation, firefly clamp, and cut helpers.

Vector3 MegaLights::diffuse_demod_factor(const Vector3 &p_diffuse_albedo) {
	return Vector3(
			MAX(float(p_diffuse_albedo.x), DEMOD_ALBEDO_FLOOR),
			MAX(float(p_diffuse_albedo.y), DEMOD_ALBEDO_FLOOR),
			MAX(float(p_diffuse_albedo.z), DEMOD_ALBEDO_FLOOR));
}

Vector3 MegaLights::env_brdf_split_sum(const Vector3 &p_f0, float p_dfg_x, float p_dfg_y) {
	const Vector3 f0(
			CLAMP(float(p_f0.x), 0.0f, 1.0f),
			CLAMP(float(p_f0.y), 0.0f, 1.0f),
			CLAMP(float(p_f0.z), 0.0f, 1.0f));
	const float dfg_x = MAX(p_dfg_x, 0.0f);
	const float dfg_y = MAX(p_dfg_y, 1e-4f);
	const float inv_y = 1.0f / dfg_y - 1.0f;
	const float f90 = CLAMP((float(f0.x) + float(f0.y) + float(f0.z)) * (50.0f * 0.33f), 0.0f, 1.0f);
	const Vector3 energy_comp(
			1.0f + float(f0.x) * inv_y,
			1.0f + float(f0.y) * inv_y,
			1.0f + float(f0.z) * inv_y);
	const Vector3 term(
			(f90 - float(f0.x)) * dfg_x + float(f0.x) * dfg_y,
			(f90 - float(f0.y)) * dfg_x + float(f0.y) * dfg_y,
			(f90 - float(f0.z)) * dfg_x + float(f0.z) * dfg_y);
	return Vector3(
			MAX(float(energy_comp.x * term.x), 0.0f),
			MAX(float(energy_comp.y * term.y), 0.0f),
			MAX(float(energy_comp.z * term.z), 0.0f));
}

Vector3 MegaLights::env_brdf_approx(const Vector3 &p_f0, float p_roughness, float p_nov) {
	const float r = CLAMP(p_roughness, 0.0f, 1.0f);
	const float nov = CLAMP(p_nov, 0.0f, 1.0f);
	// Analytical GGX split-sum DFG approximation mapped to Godot's multiscatter (dfg.x, dfg.y) convention.
	const float rx = -1.0f * r + 1.0f;
	const float ry = -0.0275f * r + 0.0425f;
	const float rz = -0.572f * r + 1.04f;
	const float rw = 0.022f * r - 0.04f;
	const float a004 = MIN(rx * rx, std::exp2(-9.28f * nov)) * rx + ry;
	const float ab_x = -1.04f * a004 + rz;
	const float ab_y = 1.04f * a004 + rw;
	const float dfg_x = CLAMP(ab_y, 0.0f, 1.0f);
	const float dfg_y = CLAMP(ab_x + ab_y, 1e-4f, 1.0f);
	return env_brdf_split_sum(p_f0, dfg_x, dfg_y);
}

Vector3 MegaLights::specular_demod_factor(const Vector3 &p_spec_env_brdf) {
	return Vector3(
			MAX(float(p_spec_env_brdf.x), DEMOD_SPECULAR_FLOOR),
			MAX(float(p_spec_env_brdf.y), DEMOD_SPECULAR_FLOOR),
			MAX(float(p_spec_env_brdf.z), DEMOD_SPECULAR_FLOOR));
}

Vector3 MegaLights::demodulate_radiance(const Vector3 &p_radiance, const Vector3 &p_factor) {
	return Vector3(
			MAX(float(p_radiance.x), 0.0f) / MAX(float(p_factor.x), 1e-4f),
			MAX(float(p_radiance.y), 0.0f) / MAX(float(p_factor.y), 1e-4f),
			MAX(float(p_radiance.z), 0.0f) / MAX(float(p_factor.z), 1e-4f));
}

Vector3 MegaLights::remodulate_radiance(const Vector3 &p_demod, const Vector3 &p_factor) {
	return Vector3(
			MAX(float(p_demod.x), 0.0f) * MAX(float(p_factor.x), 0.0f),
			MAX(float(p_demod.y), 0.0f) * MAX(float(p_factor.y), 0.0f),
			MAX(float(p_demod.z), 0.0f) * MAX(float(p_factor.z), 0.0f));
}

double MegaLights::compute_clamped_energy_ratio(uint64_t p_removed_fp, uint64_t p_total_fp) {
	if (p_total_fp == 0u) {
		return 0.0;
	}
	return CLAMP(double(p_removed_fp) / double(p_total_fp), 0.0, 1.0);
}

float MegaLights::firefly_clamp_factor(float p_luma, float p_limit, float &r_removed) {
	if (!Math::is_finite(p_luma) || !Math::is_finite(p_limit) || !(p_luma > p_limit) || !(p_luma > 0.0f) || !(p_limit >= 0.0f)) {
		r_removed = 0.0f;
		return 1.0f;
	}
	const float scale = p_limit / p_luma;
	r_removed = MAX(p_luma - p_limit, 0.0f);
	return scale;
}

bool MegaLights::is_camera_cut(const Transform3D &p_prev, const Transform3D &p_cur) {
	if (p_prev.origin.distance_squared_to(p_cur.origin) > 100.0f) {
		return true;
	}
	const Vector3 prev_fwd = -p_prev.basis.get_column(2).normalized();
	const Vector3 cur_fwd = -p_cur.basis.get_column(2).normalized();
	if (prev_fwd.dot(cur_fwd) < 0.5f) {
		return true;
	}
	const Vector3 prev_up = p_prev.basis.get_column(1).normalized();
	const Vector3 cur_up = p_cur.basis.get_column(1).normalized();
	return prev_up.dot(cur_up) < 0.5f;
}

uint32_t MegaLights::pack_visibility_rgba8(float p_r, float p_g, float p_b, float p_a) {
	const uint32_t r = uint32_t(Math::round(CLAMP(p_r, 0.0f, 1.0f) * 255.0f));
	const uint32_t g = uint32_t(Math::round(CLAMP(p_g, 0.0f, 1.0f) * 255.0f));
	const uint32_t b = uint32_t(Math::round(CLAMP(p_b, 0.0f, 1.0f) * 255.0f));
	const uint32_t a = uint32_t(Math::round(CLAMP(p_a, 0.0f, 1.0f) * 255.0f));
	return r | (g << 8u) | (b << 16u) | (a << 24u);
}

Color MegaLights::unpack_visibility_rgba8(uint32_t p_packed) {
	return Color(
			float(p_packed & 0xFFu) / 255.0f,
			float((p_packed >> 8u) & 0xFFu) / 255.0f,
			float((p_packed >> 16u) & 0xFFu) / 255.0f,
			float((p_packed >> 24u) & 0xFFu) / 255.0f);
}

Vector<String> MegaLights::get_screen_trace_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_SCREEN_TRACE\n");
	modes.push_back("\n#define MODE_FINALIZE\n");
	modes.push_back("\n#define MODE_RETRACE_RESOLVE\n");
	modes.push_back("\n#define MODE_RESOLVE_VISIBILITY\n");
	return modes;
}

Vector<String> MegaLights::get_trace_rays_shader_variant_defines() {
	Vector<String> variants;
	variants.push_back("");
	variants.push_back("\n#define NEXTGI_SHADOW_ALPHA_TEXTURES\n");
	variants.push_back("\n#define ML_USE_FAR_FIELD\n");
	variants.push_back("\n#define NEXTGI_SHADOW_ALPHA_TEXTURES\n#define ML_USE_FAR_FIELD\n");
	return variants;
}

Vector<String> MegaLights::get_trace_rays_swrt_shader_variant_defines() {
	Vector<String> variants;
	variants.push_back("");
	return variants;
}

Vector<String> MegaLights::get_trace_vsm_shader_variant_defines() {
	Vector<String> variants;
	variants.push_back("");
	return variants;
}

Vector<String> MegaLights::get_shade_samples_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_SHADE\n");
	modes.push_back("\n#define MODE_FILL_HALF\n");
	return modes;
}

Vector<String> MegaLights::get_froxel_shader_variant_defines() {
	Vector<String> variants;
	variants.push_back("");
	variants.push_back("\n#define NEXTGI_SHADOW_ALPHA_TEXTURES\n");
	return variants;
}

Vector<String> MegaLights::get_froxel_compute_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_SAMPLE_NO_RT\n");
	modes.push_back("\n#define MODE_RESAMPLE_D17\n");
	return modes;
}

Vector<String> MegaLights::get_front_layer_shader_variant_defines() {
	Vector<String> variants;
	variants.push_back("");
	return variants;
}

const uint8_t *MegaLights::get_stbn_data() {
	return megalights_stbn_data;
}

uint32_t MegaLights::get_stbn_byte_size() {
	return uint32_t(sizeof(megalights_stbn_data));
}

Ref<MegaLightsViewData> MegaLights::get_view_data(RenderSceneBuffersRD *p_rb) {
	if (p_rb == nullptr || !p_rb->has_custom_data(RB_SCOPE_MEGALIGHTS)) {
		return Ref<MegaLightsViewData>();
	}
	return p_rb->get_custom_data(RB_SCOPE_MEGALIGHTS);
}

Ref<MegaLightsViewData> MegaLights::ensure_view_data(RenderSceneBuffersRD *p_rb) {
	ERR_FAIL_NULL_V(p_rb, Ref<MegaLightsViewData>());
	Ref<MegaLightsViewData> data = get_view_data(p_rb);
	if (data.is_null()) {
		data.instantiate();
		data->configure(p_rb);
		p_rb->set_custom_data(RB_SCOPE_MEGALIGHTS, data);
	}
	return data;
}

Ref<MegaLightsViewData> MegaLights::get_front_layer_view_data(RenderSceneBuffersRD *p_rb) {
	const StringName scope = SNAME("megalights_front_layer");
	if (p_rb == nullptr || !p_rb->has_custom_data(scope)) {
		return Ref<MegaLightsViewData>();
	}
	return p_rb->get_custom_data(scope);
}

Ref<MegaLightsViewData> MegaLights::ensure_front_layer_view_data(RenderSceneBuffersRD *p_rb) {
	ERR_FAIL_NULL_V(p_rb, Ref<MegaLightsViewData>());
	const StringName scope = SNAME("megalights_front_layer");
	Ref<MegaLightsViewData> data = get_front_layer_view_data(p_rb);
	if (data.is_null()) {
		data.instantiate();
		data->set_is_front_layer_view(true);
		data->configure(p_rb);
		p_rb->set_custom_data(scope, data);
	}
	return data;
}

void MegaLights::free_front_layer_view_data(RenderSceneBuffersRD *p_rb) {
	const StringName scope = SNAME("megalights_front_layer");
	if (p_rb == nullptr || !p_rb->has_custom_data(scope)) {
		return;
	}
	Ref<MegaLightsViewData> data = p_rb->get_custom_data(scope);
	if (data.is_valid()) {
		data->free_data();
	}
	p_rb->set_custom_data(scope, Ref<RenderBufferCustomDataRD>());
}

void MegaLights::request_check(const Dictionary &p_options) {
	MegaLightsCheckState &st = ml_check_state();
	MutexLock lock(st.mutex);
	st.active = true;
	st.initialized = false;
	st.options = p_options.duplicate(true);
	st.mode = String(p_options.get("mode", "unbiased"));
	st.completed_steps = 0;
	st.total_steps = 0;
	st.wait_frames = 0;
	st.checkpoint_recorded = false;
	st.checkpoint_max_rse = 0.0;
	st.checkpoint_p99_rse = 0.0;
	st.checkpoint_mean_rse = 0.0;
	st.checkpoint_penumbra_pixels = 0;
	st.result = Dictionary();
	st.result["done"] = false;
	st.result["mode"] = st.mode;
}

bool MegaLights::has_pending_check() {
	MegaLightsCheckState &st = ml_check_state();
	MutexLock lock(st.mutex);
	return st.active;
}

bool MegaLights::check_wants_far_field() {
	MegaLightsCheckState &st = ml_check_state();
	MutexLock lock(st.mutex);
	return st.active && bool(st.options.get("far_field", false));
}

bool MegaLights::check_wants_swrt() {
	MegaLightsCheckState &st = ml_check_state();
	MutexLock lock(st.mutex);
	return st.active && bool(st.options.get("swrt", false));
}

bool MegaLights::check_wants_directional_lights() {
	MegaLightsCheckState &st = ml_check_state();
	MutexLock lock(st.mutex);
	return st.active && bool(st.options.get("directional_lights", false));
}

Dictionary MegaLights::get_check_result() {
	MegaLightsCheckState &st = ml_check_state();
	MutexLock lock(st.mutex);
	return st.result.duplicate(true);
}

/* MegaLights GPU State & Execution */

struct MegaLights::GPUState {
	MlGenerateSamplesShaderRD generate_shader;
	RID generate_version;
	RID generate_pipeline;

	MlGuidingShaderRD guiding_shader;
	RID guiding_version;
	RID guiding_pipeline;

	MlReferenceShaderRD ref_compute_shader;
	RID ref_compute_version;
	RID ref_unshadowed_pipeline;
	RID ref_verify_pipeline;

	MlReferenceShaderRD ref_rt_shader;
	RID ref_rt_version;
	RID ref_rt_pipeline;
	bool ref_rt_initialized = false;
	bool ref_rt_failed = false;

	MlReferenceShaderRD ref_rt_alpha_shader;
	RID ref_rt_alpha_version;
	RID ref_rt_alpha_pipeline;
	bool ref_rt_alpha_initialized = false;
	bool ref_rt_alpha_failed = false;
	RID shadow_texture_set;
	RID shadow_texture_set_table;

	// GodotNext: megalights_visibility (#146, plan §21.3 contract M4).
	enum ScreenTraceMode : uint32_t {
		SCREEN_TRACE_MODE_TRACE = 0,
		SCREEN_TRACE_MODE_FINALIZE = 1,
		SCREEN_TRACE_MODE_RETRACE_RESOLVE = 2,
		SCREEN_TRACE_MODE_RESOLVE = 3,
		SCREEN_TRACE_MODE_MAX = 4,
	};
	MlScreenTraceShaderRD vis_screen_shader;
	RID vis_screen_version;
	RID vis_screen_pipelines[SCREEN_TRACE_MODE_MAX];
	bool vis_screen_initialized = false;
	bool vis_screen_failed = false;

	MlTraceRaysShaderRD vis_trace_rays_shader;
	RID vis_trace_rays_version;
	RID vis_trace_rays_pipelines[4];
	bool vis_trace_rays_initialized = false;
	bool vis_trace_rays_failed = false;
	RID vis_shadow_texture_set;
	RID vis_shadow_texture_set_table;

	MlTraceRaysSwrtShaderRD vis_trace_swrt_shader;
	RID vis_trace_swrt_version;
	RID vis_trace_swrt_pipeline;
	bool vis_trace_swrt_initialized = false;
	bool vis_trace_swrt_failed = false;

	MlTraceVsmShaderRD vis_trace_vsm_shader;
	RID vis_trace_vsm_version;
	RID vis_trace_vsm_pipeline;
	bool vis_trace_vsm_initialized = false;
	bool vis_trace_vsm_failed = false;

	// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5).
	enum ShadeMode : uint32_t {
		SHADE_MODE_SHADE = 0,
		SHADE_MODE_FILL_HALF = 1,
		SHADE_MODE_MAX = 2,
	};
	MlShadeSamplesShaderRD shade_shader;
	RID shade_version;
	RID shade_pipelines[SHADE_MODE_MAX];
	bool shade_initialized = false;
	bool shade_failed = false;
	NextGIDenoise *denoiser = nullptr;

	RID stbn_texture;
	RID dummy_storage_buffer;
	RID dummy_3d_storage_tex;

	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7).
	MlFroxelShaderRD froxel_rt_shader;
	RID froxel_rt_version;
	RID froxel_rt_pipelines[2];
	RID froxel_shadow_texture_set;
	RID froxel_shadow_texture_set_table;
	bool froxel_rt_initialized = false;
	bool froxel_rt_failed = false;

	enum FroxelComputeMode : uint32_t {
		FROXEL_COMPUTE_SAMPLE_NO_RT = 0,
		FROXEL_COMPUTE_RESAMPLE_D17 = 1,
		FROXEL_COMPUTE_MODE_MAX = 2,
	};
	MlFroxelComputeShaderRD froxel_compute_shader;
	RID froxel_compute_version;
	RID froxel_compute_pipelines[FROXEL_COMPUTE_MODE_MAX];
	bool froxel_compute_initialized = false;
	bool froxel_compute_failed = false;

	MlFrontLayerShaderRD front_layer_shader;
	RID front_layer_version;
	RID front_layer_pipeline;
	bool front_layer_initialized = false;
	bool front_layer_failed = false;

	bool common_initialized = false;
	bool common_failed = false;
};

MegaLights::MegaLights() {
	gpu = memnew(GPUState);
}

MegaLights::~MegaLights() {
	free_resources();
	if (gpu != nullptr) {
		memdelete(gpu);
		gpu = nullptr;
	}
}

void MegaLights::free_resources() {
	if (gpu == nullptr) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (gpu->shadow_texture_set.is_valid() && rd->uniform_set_is_valid(gpu->shadow_texture_set)) {
			rd->free_rid(gpu->shadow_texture_set);
		}
		gpu->shadow_texture_set = RID();
		gpu->shadow_texture_set_table = RID();

		ml_free_rid(rd, gpu->generate_pipeline);
		if (gpu->generate_version.is_valid()) {
			gpu->generate_shader.version_free(gpu->generate_version);
			gpu->generate_version = RID();
		}

		ml_free_rid(rd, gpu->guiding_pipeline);
		if (gpu->guiding_version.is_valid()) {
			gpu->guiding_shader.version_free(gpu->guiding_version);
			gpu->guiding_version = RID();
		}

		ml_free_rid(rd, gpu->ref_unshadowed_pipeline);
		ml_free_rid(rd, gpu->ref_verify_pipeline);
		if (gpu->ref_compute_version.is_valid()) {
			gpu->ref_compute_shader.version_free(gpu->ref_compute_version);
			gpu->ref_compute_version = RID();
		}

		ml_free_rid(rd, gpu->ref_rt_pipeline);
		if (gpu->ref_rt_version.is_valid()) {
			gpu->ref_rt_shader.version_free(gpu->ref_rt_version);
			gpu->ref_rt_version = RID();
		}

		ml_free_rid(rd, gpu->ref_rt_alpha_pipeline);
		if (gpu->ref_rt_alpha_version.is_valid()) {
			gpu->ref_rt_alpha_shader.version_free(gpu->ref_rt_alpha_version);
			gpu->ref_rt_alpha_version = RID();
		}

		// GodotNext: megalights_visibility (#146).
		if (gpu->vis_shadow_texture_set.is_valid() && rd->uniform_set_is_valid(gpu->vis_shadow_texture_set)) {
			rd->free_rid(gpu->vis_shadow_texture_set);
		}
		gpu->vis_shadow_texture_set = RID();
		gpu->vis_shadow_texture_set_table = RID();

		for (uint32_t i = 0; i < GPUState::SCREEN_TRACE_MODE_MAX; i++) {
			ml_free_rid(rd, gpu->vis_screen_pipelines[i]);
		}
		if (gpu->vis_screen_version.is_valid()) {
			gpu->vis_screen_shader.version_free(gpu->vis_screen_version);
			gpu->vis_screen_version = RID();
		}

		for (uint32_t i = 0; i < 4u; i++) {
			ml_free_rid(rd, gpu->vis_trace_rays_pipelines[i]);
		}
		if (gpu->vis_trace_rays_version.is_valid()) {
			gpu->vis_trace_rays_shader.version_free(gpu->vis_trace_rays_version);
			gpu->vis_trace_rays_version = RID();
		}

		ml_free_rid(rd, gpu->vis_trace_swrt_pipeline);
		if (gpu->vis_trace_swrt_version.is_valid()) {
			gpu->vis_trace_swrt_shader.version_free(gpu->vis_trace_swrt_version);
			gpu->vis_trace_swrt_version = RID();
		}

		ml_free_rid(rd, gpu->vis_trace_vsm_pipeline);
		if (gpu->vis_trace_vsm_version.is_valid()) {
			gpu->vis_trace_vsm_shader.version_free(gpu->vis_trace_vsm_version);
			gpu->vis_trace_vsm_version = RID();
		}

		// GodotNext: megalights_denoiser (#135).
		for (uint32_t i = 0; i < GPUState::SHADE_MODE_MAX; i++) {
			ml_free_rid(rd, gpu->shade_pipelines[i]);
		}
		if (gpu->shade_version.is_valid()) {
			gpu->shade_shader.version_free(gpu->shade_version);
			gpu->shade_version = RID();
		}

		// GodotNext: megalights_fog_translucency (#137).
		if (gpu->froxel_shadow_texture_set.is_valid() && rd->uniform_set_is_valid(gpu->froxel_shadow_texture_set)) {
			rd->free_rid(gpu->froxel_shadow_texture_set);
		}
		gpu->froxel_shadow_texture_set = RID();
		gpu->froxel_shadow_texture_set_table = RID();

		for (uint32_t i = 0; i < 2u; i++) {
			ml_free_rid(rd, gpu->froxel_rt_pipelines[i]);
		}
		if (gpu->froxel_rt_version.is_valid()) {
			gpu->froxel_rt_shader.version_free(gpu->froxel_rt_version);
			gpu->froxel_rt_version = RID();
		}

		for (uint32_t i = 0; i < GPUState::FROXEL_COMPUTE_MODE_MAX; i++) {
			ml_free_rid(rd, gpu->froxel_compute_pipelines[i]);
		}
		if (gpu->froxel_compute_version.is_valid()) {
			gpu->froxel_compute_shader.version_free(gpu->froxel_compute_version);
			gpu->froxel_compute_version = RID();
		}

		ml_free_rid(rd, gpu->front_layer_pipeline);
		if (gpu->front_layer_version.is_valid()) {
			gpu->front_layer_shader.version_free(gpu->front_layer_version);
			gpu->front_layer_version = RID();
		}

		ml_free_rid(rd, gpu->stbn_texture);
		ml_free_rid(rd, gpu->dummy_storage_buffer);
		ml_free_rid(rd, gpu->dummy_3d_storage_tex);
	}
	if (gpu->denoiser != nullptr) {
		memdelete(gpu->denoiser);
		gpu->denoiser = nullptr;
	}
	gpu->common_initialized = false;
	gpu->common_failed = false;
	gpu->ref_rt_initialized = false;
	gpu->ref_rt_failed = false;
	gpu->ref_rt_alpha_initialized = false;
	gpu->ref_rt_alpha_failed = false;
	gpu->vis_screen_initialized = false;
	gpu->vis_screen_failed = false;
	gpu->vis_trace_rays_initialized = false;
	gpu->vis_trace_rays_failed = false;
	gpu->vis_trace_swrt_initialized = false;
	gpu->vis_trace_swrt_failed = false;
	gpu->vis_trace_vsm_initialized = false;
	gpu->vis_trace_vsm_failed = false;
	gpu->shade_initialized = false;
	gpu->shade_failed = false;
	gpu->froxel_rt_initialized = false;
	gpu->froxel_rt_failed = false;
	gpu->froxel_compute_initialized = false;
	gpu->froxel_compute_failed = false;
	gpu->front_layer_initialized = false;
	gpu->front_layer_failed = false;
}

void MegaLights::_ensure_stbn_texture() {
	if (gpu->stbn_texture.is_valid()) {
		return;
	}
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.format = RD::DATA_FORMAT_R8G8_UNORM;
	tf.width = STBN_WIDTH;
	tf.height = STBN_HEIGHT;
	tf.depth = 1;
	tf.array_layers = STBN_DEPTH;
	tf.mipmaps = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;

	const uint32_t layer_bytes = STBN_WIDTH * STBN_HEIGHT * STBN_CHANNELS;
	Vector<Vector<uint8_t>> layers;
	layers.resize(STBN_DEPTH);
	for (uint32_t t = 0; t < STBN_DEPTH; t++) {
		Vector<uint8_t> slice;
		slice.resize(layer_bytes);
		memcpy(slice.ptrw(), megalights_stbn_data + t * layer_bytes, layer_bytes);
		layers.write[t] = slice;
	}
	gpu->stbn_texture = rd->texture_create(tf, RD::TextureView(), layers);
}

bool MegaLights::_ensure_common_shaders() {
	if (gpu->common_initialized) {
		return true;
	}
	if (gpu->common_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(light_storage, false);

	const String max_dir_def = "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(light_storage->get_max_directional_lights()) + "\n";

	gpu->generate_shader.initialize(Vector<String>({ "" }), max_dir_def);
	gpu->generate_version = gpu->generate_shader.version_create(false);
	if (!gpu->generate_version.is_valid()) {
		gpu->common_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_generate_samples.glsl failed to compile.");
	}
	gpu->generate_pipeline = rd->compute_pipeline_create(gpu->generate_shader.version_get_shader(gpu->generate_version, 0));

	gpu->guiding_shader.initialize(Vector<String>({ "" }));
	gpu->guiding_version = gpu->guiding_shader.version_create(false);
	if (!gpu->guiding_version.is_valid()) {
		gpu->common_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_guiding.glsl failed to compile.");
	}
	gpu->guiding_pipeline = rd->compute_pipeline_create(gpu->guiding_shader.version_get_shader(gpu->guiding_version, 0));

	Vector<String> ref_modes;
	ref_modes.push_back("\n#define MODE_UNSHADOWED\n");
	ref_modes.push_back("\n#define MODE_VERIFY_SAMPLES\n");
	gpu->ref_compute_shader.initialize(ref_modes, max_dir_def);
	gpu->ref_compute_version = gpu->ref_compute_shader.version_create(false);
	if (!gpu->ref_compute_version.is_valid()) {
		gpu->common_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_reference.glsl compute variants failed to compile.");
	}
	gpu->ref_unshadowed_pipeline = rd->compute_pipeline_create(gpu->ref_compute_shader.version_get_shader(gpu->ref_compute_version, 0));
	gpu->ref_verify_pipeline = rd->compute_pipeline_create(gpu->ref_compute_shader.version_get_shader(gpu->ref_compute_version, 1));

	Vector<uint8_t> zero16;
	zero16.resize(16);
	memset(zero16.ptrw(), 0, 16);
	gpu->dummy_storage_buffer = rd->storage_buffer_create(uint32_t(zero16.size()), zero16);
	_ensure_stbn_texture();

	if (!gpu->generate_pipeline.is_valid() || !gpu->guiding_pipeline.is_valid() || !gpu->ref_unshadowed_pipeline.is_valid() || !gpu->ref_verify_pipeline.is_valid() || !gpu->stbn_texture.is_valid() || !gpu->dummy_storage_buffer.is_valid()) {
		gpu->common_failed = true;
		return false;
	}
	gpu->common_initialized = true;
	return true;
}

bool MegaLights::_ensure_rt_reference_shader(NextGI *p_next_gi, bool p_use_alpha_textures) {
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	if (rd == nullptr || light_storage == nullptr || !rd->has_feature(RD::SUPPORTS_RAY_QUERY)) {
		return false;
	}
	if (p_next_gi != nullptr && !p_next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	const String max_dir_def = "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(light_storage->get_max_directional_lights()) + "\n";

	if (p_use_alpha_textures) {
		if (gpu->ref_rt_alpha_initialized) {
			return true;
		}
		if (gpu->ref_rt_alpha_failed) {
			return false;
		}
		gpu->ref_rt_alpha_shader.initialize(Vector<String>({ "\n#define MODE_RAY_QUERY\n#define NEXTGI_SHADOW_ALPHA_TEXTURES\n" }), max_dir_def);
		gpu->ref_rt_alpha_version = gpu->ref_rt_alpha_shader.version_create(false);
		if (!gpu->ref_rt_alpha_version.is_valid()) {
			gpu->ref_rt_alpha_failed = true;
			ERR_FAIL_V_MSG(false, "MegaLights: ml_reference.glsl ray-query alpha variant failed to compile.");
		}
		if (p_next_gi != nullptr) {
			p_next_gi->notify_ray_query_shader_initialized();
		}
		gpu->ref_rt_alpha_pipeline = rd->compute_pipeline_create(gpu->ref_rt_alpha_shader.version_get_shader(gpu->ref_rt_alpha_version, 0));
		if (!gpu->ref_rt_alpha_pipeline.is_valid()) {
			gpu->ref_rt_alpha_failed = true;
			return false;
		}
		gpu->ref_rt_alpha_initialized = true;
		return true;
	}

	if (gpu->ref_rt_initialized) {
		return true;
	}
	if (gpu->ref_rt_failed) {
		return false;
	}
	gpu->ref_rt_shader.initialize(Vector<String>({ "\n#define MODE_RAY_QUERY\n" }), max_dir_def);
	gpu->ref_rt_version = gpu->ref_rt_shader.version_create(false);
	if (!gpu->ref_rt_version.is_valid()) {
		gpu->ref_rt_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_reference.glsl ray-query variant failed to compile.");
	}
	if (p_next_gi != nullptr) {
		p_next_gi->notify_ray_query_shader_initialized();
	}
	gpu->ref_rt_pipeline = rd->compute_pipeline_create(gpu->ref_rt_shader.version_get_shader(gpu->ref_rt_version, 0));
	if (!gpu->ref_rt_pipeline.is_valid()) {
		gpu->ref_rt_failed = true;
		return false;
	}
	gpu->ref_rt_initialized = true;
	return true;
}

bool MegaLights::_ensure_screen_trace_shaders() {
	if (gpu->vis_screen_initialized) {
		return true;
	}
	if (gpu->vis_screen_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	gpu->vis_screen_shader.initialize(get_screen_trace_shader_variant_defines());
	gpu->vis_screen_version = gpu->vis_screen_shader.version_create(false);
	if (!gpu->vis_screen_version.is_valid()) {
		gpu->vis_screen_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_screen_trace.glsl failed to compile.");
	}
	for (uint32_t i = 0; i < GPUState::SCREEN_TRACE_MODE_MAX; i++) {
		gpu->vis_screen_pipelines[i] = rd->compute_pipeline_create(gpu->vis_screen_shader.version_get_shader(gpu->vis_screen_version, int(i)));
		if (!gpu->vis_screen_pipelines[i].is_valid()) {
			gpu->vis_screen_failed = true;
			return false;
		}
	}
	gpu->vis_screen_initialized = true;
	return true;
}

bool MegaLights::_ensure_trace_rays_shaders(NextGI *p_next_gi) {
	if (gpu->vis_trace_rays_initialized) {
		return true;
	}
	if (gpu->vis_trace_rays_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr || !rd->has_feature(RD::SUPPORTS_RAY_QUERY)) {
		return false;
	}
	if (p_next_gi != nullptr && !p_next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	gpu->vis_trace_rays_shader.initialize(get_trace_rays_shader_variant_defines());
	gpu->vis_trace_rays_version = gpu->vis_trace_rays_shader.version_create(false);
	if (!gpu->vis_trace_rays_version.is_valid()) {
		gpu->vis_trace_rays_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_trace_rays.glsl failed to compile.");
	}
	if (p_next_gi != nullptr) {
		p_next_gi->notify_ray_query_shader_initialized();
	}
	for (uint32_t i = 0; i < 4u; i++) {
		gpu->vis_trace_rays_pipelines[i] = rd->compute_pipeline_create(gpu->vis_trace_rays_shader.version_get_shader(gpu->vis_trace_rays_version, int(i)));
		if (!gpu->vis_trace_rays_pipelines[i].is_valid()) {
			gpu->vis_trace_rays_failed = true;
			return false;
		}
	}
	gpu->vis_trace_rays_initialized = true;
	return true;
}

bool MegaLights::_ensure_trace_rays_swrt_shader() {
	if (gpu->vis_trace_swrt_initialized) {
		return true;
	}
	if (gpu->vis_trace_swrt_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	gpu->vis_trace_swrt_shader.initialize(get_trace_rays_swrt_shader_variant_defines());
	gpu->vis_trace_swrt_version = gpu->vis_trace_swrt_shader.version_create(false);
	if (!gpu->vis_trace_swrt_version.is_valid()) {
		gpu->vis_trace_swrt_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_trace_rays_swrt.glsl failed to compile.");
	}
	gpu->vis_trace_swrt_pipeline = rd->compute_pipeline_create(gpu->vis_trace_swrt_shader.version_get_shader(gpu->vis_trace_swrt_version, 0));
	if (!gpu->vis_trace_swrt_pipeline.is_valid()) {
		gpu->vis_trace_swrt_failed = true;
		return false;
	}
	gpu->vis_trace_swrt_initialized = true;
	return true;
}

bool MegaLights::_ensure_trace_vsm_shader() {
	if (gpu->vis_trace_vsm_initialized) {
		return true;
	}
	if (gpu->vis_trace_vsm_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(light_storage, false);
	const String defines = RendererVirtualShadowMapStorage::get_glsl_defines() +
			"\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(light_storage->get_max_directional_lights()) + "\n";
	gpu->vis_trace_vsm_shader.initialize(get_trace_vsm_shader_variant_defines(), defines);
	gpu->vis_trace_vsm_version = gpu->vis_trace_vsm_shader.version_create(false);
	if (!gpu->vis_trace_vsm_version.is_valid()) {
		gpu->vis_trace_vsm_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_trace_vsm.glsl failed to compile.");
	}
	gpu->vis_trace_vsm_pipeline = rd->compute_pipeline_create(gpu->vis_trace_vsm_shader.version_get_shader(gpu->vis_trace_vsm_version, 0));
	if (!gpu->vis_trace_vsm_pipeline.is_valid()) {
		gpu->vis_trace_vsm_failed = true;
		return false;
	}
	gpu->vis_trace_vsm_initialized = true;
	return true;
}

bool MegaLights::_ensure_shade_shaders() {
	if (gpu->shade_initialized) {
		return true;
	}
	if (gpu->shade_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(light_storage, false);
	const String max_dir_def = "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(light_storage->get_max_directional_lights()) + "\n";
	gpu->shade_shader.initialize(get_shade_samples_shader_variant_defines(), max_dir_def);
	gpu->shade_version = gpu->shade_shader.version_create(false);
	if (!gpu->shade_version.is_valid()) {
		gpu->shade_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_shade_samples.glsl failed to compile.");
	}
	for (uint32_t i = 0; i < GPUState::SHADE_MODE_MAX; i++) {
		gpu->shade_pipelines[i] = rd->compute_pipeline_create(gpu->shade_shader.version_get_shader(gpu->shade_version, int(i)));
		if (!gpu->shade_pipelines[i].is_valid()) {
			gpu->shade_failed = true;
			return false;
		}
	}
	gpu->shade_initialized = true;
	return true;
}

NextGIDenoise *MegaLights::_ensure_denoiser() {
	if (gpu->denoiser == nullptr) {
		gpu->denoiser = memnew(NextGIDenoise(false));
	}
	return gpu->denoiser;
}

bool MegaLights::_ensure_froxel_rt_shader(NextGI *p_next_gi) {
	if (gpu->froxel_rt_initialized) {
		return true;
	}
	if (gpu->froxel_rt_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	if (rd == nullptr || light_storage == nullptr || !rd->has_feature(RD::SUPPORTS_RAY_QUERY)) {
		return false;
	}
	if (p_next_gi != nullptr && !p_next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	const String defines = RendererVirtualShadowMapStorage::get_glsl_defines() +
			"\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(light_storage->get_max_directional_lights()) + "\n";
	gpu->froxel_rt_shader.initialize(get_froxel_shader_variant_defines(), defines);
	gpu->froxel_rt_version = gpu->froxel_rt_shader.version_create(false);
	if (!gpu->froxel_rt_version.is_valid()) {
		gpu->froxel_rt_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_froxel.glsl failed to compile.");
	}
	if (p_next_gi != nullptr) {
		p_next_gi->notify_ray_query_shader_initialized();
	}
	for (uint32_t i = 0; i < 2u; i++) {
		gpu->froxel_rt_pipelines[i] = rd->compute_pipeline_create(gpu->froxel_rt_shader.version_get_shader(gpu->froxel_rt_version, int(i)));
		if (!gpu->froxel_rt_pipelines[i].is_valid()) {
			gpu->froxel_rt_failed = true;
			return false;
		}
	}
	if (!gpu->dummy_3d_storage_tex.is_valid()) {
		gpu->dummy_3d_storage_tex = ml_create_rgba16f_3d_texture(rd, Vector3i(1, 1, 1), "MegaLights Dummy 3D Storage");
	}
	gpu->froxel_rt_initialized = true;
	return true;
}

bool MegaLights::_ensure_froxel_compute_shaders() {
	if (gpu->froxel_compute_initialized) {
		return true;
	}
	if (gpu->froxel_compute_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(light_storage, false);
	const String defines = RendererVirtualShadowMapStorage::get_glsl_defines() +
			"\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(light_storage->get_max_directional_lights()) + "\n";
	gpu->froxel_compute_shader.initialize(get_froxel_compute_shader_variant_defines(), defines);
	gpu->froxel_compute_version = gpu->froxel_compute_shader.version_create(false);
	if (!gpu->froxel_compute_version.is_valid()) {
		gpu->froxel_compute_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_froxel_compute.glsl failed to compile.");
	}
	for (uint32_t i = 0; i < GPUState::FROXEL_COMPUTE_MODE_MAX; i++) {
		gpu->froxel_compute_pipelines[i] = rd->compute_pipeline_create(gpu->froxel_compute_shader.version_get_shader(gpu->froxel_compute_version, int(i)));
		if (!gpu->froxel_compute_pipelines[i].is_valid()) {
			gpu->froxel_compute_failed = true;
			return false;
		}
	}
	if (!gpu->dummy_3d_storage_tex.is_valid()) {
		gpu->dummy_3d_storage_tex = ml_create_rgba16f_3d_texture(rd, Vector3i(1, 1, 1), "MegaLights Dummy 3D Storage");
	}
	gpu->froxel_compute_initialized = true;
	return true;
}

bool MegaLights::_ensure_front_layer_shader() {
	if (gpu->front_layer_initialized) {
		return true;
	}
	if (gpu->front_layer_failed) {
		return false;
	}
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	gpu->front_layer_shader.initialize(get_front_layer_shader_variant_defines());
	gpu->front_layer_version = gpu->front_layer_shader.version_create(false);
	if (!gpu->front_layer_version.is_valid()) {
		gpu->front_layer_failed = true;
		ERR_FAIL_V_MSG(false, "MegaLights: ml_front_layer.glsl failed to compile.");
	}
	gpu->front_layer_pipeline = rd->compute_pipeline_create(gpu->front_layer_shader.version_get_shader(gpu->front_layer_version, 0));
	if (!gpu->front_layer_pipeline.is_valid()) {
		gpu->front_layer_failed = true;
		return false;
	}
	gpu->front_layer_initialized = true;
	return true;
}

void MegaLights::_fill_params_ubo(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_frame_index, bool p_has_guiding, bool p_force_visible_one, float p_falloff_override, ParamsUBO &r_ubo) const {
	const RenderDataRD *rd_data = p_inputs.render_data;
	const RenderSceneDataRD *scene_data = rd_data->scene_data;
	LightStorage *light_storage = LightStorage::get_singleton();

	const Projection depth_proj = NextGIViewMath::depth_projection(scene_data->cam_projection, p_inputs.flip_y, scene_data->taa_jitter);
	const Projection inv_proj = depth_proj.inverse();
	MaterialStorage::store_camera(inv_proj, r_ubo.inv_projection);
	MaterialStorage::store_transform_transposed_3x4(scene_data->cam_transform, r_ubo.inv_view_rows);

	const Size2i full_size = p_view->get_internal_size();
	const Size2i sample_size = p_view->get_sample_grid_size();
	const Size2i tile_dims = p_view->get_guiding_tile_dims();

	r_ubo.screen_sample_size[0] = uint32_t(full_size.x);
	r_ubo.screen_sample_size[1] = uint32_t(full_size.y);
	r_ubo.screen_sample_size[2] = uint32_t(sample_size.x);
	r_ubo.screen_sample_size[3] = uint32_t(sample_size.y);

	r_ubo.grid_dims_flags[0] = uint32_t(MAX(rd_data->packed_light_grid_dims.x, 1));
	r_ubo.grid_dims_flags[1] = uint32_t(MAX(rd_data->packed_light_grid_dims.y, 1));
	r_ubo.grid_dims_flags[2] = uint32_t(MAX(rd_data->packed_light_grid_dims.z, 1));
	r_ubo.grid_dims_flags[3] = rd_data->packed_light_grid_flags;

	r_ubo.grid_z_params[0] = rd_data->packed_light_grid_z_near;
	r_ubo.grid_z_params[1] = rd_data->packed_light_grid_z_far;
	r_ubo.grid_z_params[2] = rd_data->packed_light_grid_z_scale;
	r_ubo.grid_z_params[3] = rd_data->packed_light_grid_z_bias;

	r_ubo.light_counts[0] = light_storage->get_omni_light_count();
	r_ubo.light_counts[1] = light_storage->get_spot_light_count();
	r_ubo.light_counts[2] = light_storage->get_area_light_count();
	r_ubo.light_counts[3] = rd_data->directional_light_count;

	uint32_t flags = 0u;
	if (p_settings.directional_lights) {
		flags |= PARAM_FLAG_DIRECTIONAL_LIGHTS;
	}
	if (scene_data->cam_orthogonal) {
		flags |= PARAM_FLAG_ORTHOGONAL;
	}
	if (p_has_guiding) {
		flags |= PARAM_FLAG_HAS_GUIDING_HISTORY;
	}
	if (p_inputs.light_function_atlas != nullptr && p_inputs.light_function_atlas->has_baked_atlas() && p_inputs.light_function_atlas->get_view_entry_count() > 0u) {
		flags |= PARAM_FLAG_USE_LIGHT_FUNCTION;
	}
	if (p_force_visible_one) {
		flags |= PARAM_FLAG_FORCE_VISIBLE_ONE;
	}
	if (p_inputs.use_soft_shadows) {
		flags |= PARAM_FLAG_USE_SOFT_SHADOWS;
	}

	r_ubo.sample_config[0] = p_settings.samples_per_pixel;
	r_ubo.sample_config[1] = uint32_t(p_settings.downsample);
	r_ubo.sample_config[2] = p_frame_index;
	r_ubo.sample_config[3] = flags;

	r_ubo.guiding_tile_info[0] = uint32_t(tile_dims.x);
	r_ubo.guiding_tile_info[1] = uint32_t(tile_dims.y);
	r_ubo.guiding_tile_info[2] = MegaLightsInputs::get_layer_group_count();
	r_ubo.guiding_tile_info[3] = p_ref_frame_index;

	const float falloff = p_falloff_override >= 0.0f ? p_falloff_override : p_settings.falloff_cull_threshold;
	r_ubo.thresholds[0] = falloff;
	r_ubo.thresholds[1] = DEFAULT_GUIDING_FLOOR;
	r_ubo.thresholds[2] = float(scene_data->z_far);
	r_ubo.thresholds[3] = 0.0f;
}

void MegaLights::_upload_layer_group_masks(const Ref<MegaLightsViewData> &p_view) const {
	RD *rd = RD::get_singleton();
	const LocalVector<uint32_t> &masks = MegaLightsInputs::get_layer_group_masks();
	const uint32_t count = masks.size();
	if (!p_view->ensure_layer_group_buffer(count)) {
		return;
	}
	if (count > 0u) {
		rd->buffer_update(p_view->get_layer_group_buffer(), 0, count * sizeof(uint32_t), masks.ptr());
	} else {
		const uint32_t all_layers = 0xFFFFFFFFu;
		rd->buffer_update(p_view->get_layer_group_buffer(), 0, sizeof(uint32_t), &all_layers);
	}
}

void MegaLights::_upload_light_shadow_policies(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, bool p_include_directional, bool p_allow_vsm) const {
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;

	LocalVector<LightStorage::MegaLightsLightPolicy> policies;
	const uint32_t dir_count = p_include_directional ? p_inputs.render_data->directional_light_count : 0u;
	light_storage->get_view_light_megalights_policies(dir_count, policies);

	const uint32_t count = MAX(uint32_t(policies.size()), 1u);
	if (!p_view->ensure_light_shadow_policy_buffer(count)) {
		return;
	}

	LocalVector<uint32_t> policy_words;
	policy_words.resize(count * 4u);
	for (uint32_t i = 0; i < policies.size(); i++) {
		const LightStorage::MegaLightsLightPolicy &p = policies[i];
		const uint32_t cm = p.caster_mask & NEXTGI_SHADOW_LAYER_MASK;
		uint32_t flags = 0u;
		if (p.caster_mask != 0u) {
			flags |= POLICY_FLAG_CASTS_SHADOW;
		}
		if (p_allow_vsm && light_uses_vsm(p.shadow_method, p.vsm_sampleable)) {
			flags |= POLICY_FLAG_USE_VSM;
		}
		if (cm == NEXTGI_SHADOW_LAYER_MASK) {
			flags |= POLICY_FLAG_FULL_CASTER_MASK;
		}
		policy_words[i * 4u + 0u] = (p.caster_mask != 0u && rt_scene != nullptr) ? rt_scene->get_shadow_light_mode(p.caster_mask) : 0u;
		policy_words[i * 4u + 1u] = cm;
		policy_words[i * 4u + 2u] = flags;
		policy_words[i * 4u + 3u] = 0u;
	}
	if (policies.is_empty()) {
		policy_words[0] = 0u;
		policy_words[1] = 0u;
		policy_words[2] = 0u;
		policy_words[3] = 0u;
	}
	rd->buffer_update(p_view->get_light_shadow_policy_buffer(), 0, policy_words.size() * sizeof(uint32_t), policy_words.ptr());
}

void MegaLights::_build_common_uniforms(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, Vector<RD::Uniform> &r_uniforms) const {
	LightStorage *light_storage = LightStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	const RID default_white = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	const RID default_black = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	const RID default_array_white = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);

	RID area_atlas = texture_storage->area_light_atlas_get_texture();
	if (!area_atlas.is_valid()) {
		area_atlas = default_white;
	}
	RID ies_atlas = light_storage->get_ies_atlas_texture();
	if (!ies_atlas.is_valid()) {
		ies_atlas = default_array_white;
	}
	RID lf_atlas = p_inputs.light_function_atlas != nullptr ? p_inputs.light_function_atlas->get_atlas_texture_or_default() : default_white;
	RID lf_entries = p_inputs.light_function_atlas != nullptr ? p_inputs.light_function_atlas->get_view_entry_buffer_or_default() : gpu->dummy_storage_buffer;

	const RID sampler_nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID sampler_linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID sampler_linear_mips = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	r_uniforms.clear();
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_params_ubo()));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.depth_texture.is_valid() ? p_inputs.depth_texture : default_black));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.normal_roughness_texture.is_valid() ? p_inputs.normal_roughness_texture : default_black));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_inputs.ml_albedo_texture.is_valid() ? p_inputs.ml_albedo_texture : default_white));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, p_inputs.ml_material_texture.is_valid() ? p_inputs.ml_material_texture : default_black));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, p_inputs.dfg_lut_texture.is_valid() ? p_inputs.dfg_lut_texture : default_white));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_inputs.ltc_lut1_texture.is_valid() ? p_inputs.ltc_lut1_texture : default_white));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, p_inputs.ltc_lut2_texture.is_valid() ? p_inputs.ltc_lut2_texture : default_white));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 8, area_atlas));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 9, ies_atlas));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 10, lf_atlas));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, gpu->stbn_texture));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 12, sampler_nearest));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 13, sampler_linear));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 14, sampler_linear_mips));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, light_storage->get_omni_light_buffer()));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, light_storage->get_spot_light_buffer()));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, light_storage->get_area_light_buffer()));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 18, light_storage->get_directional_light_buffer()));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, lf_entries));
}

void MegaLights::_dispatch_generate(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, bool p_has_guiding, float p_falloff_override) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(usc);

	ParamsUBO ubo;
	_fill_params_ubo(p_inputs, p_view, p_settings, p_frame_index, 0u, p_has_guiding, false, p_falloff_override, ubo);
	rd->buffer_update(p_view->get_params_ubo(), 0, sizeof(ubo), &ubo);

	Vector<RD::Uniform> uniforms;
	_build_common_uniforms(p_inputs, p_view, uniforms);
	const RID headers_buf = p_inputs.render_data->packed_light_grid_headers_buffer.is_valid() ? p_inputs.render_data->packed_light_grid_headers_buffer : gpu->dummy_storage_buffer;
	const RID indices_buf = p_inputs.render_data->packed_light_grid_index_buffer.is_valid() ? p_inputs.render_data->packed_light_grid_index_buffer : gpu->dummy_storage_buffer;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, headers_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, indices_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, p_view->get_layer_group_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, p_view->get_previous_guiding_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, p_view->get_sample_buffer()));

	LocalVector<RD::Uniform> uniform_lv;
	uniform_lv.resize(uniforms.size());
	for (int i = 0; i < uniforms.size(); i++) {
		uniform_lv[i] = uniforms[i];
	}

	const RID shader_rid = gpu->generate_shader.version_get_shader(gpu->generate_version, 0);
	const RID uniform_set = usc->get_cache_vec(shader_rid, 0, uniform_lv);
	ERR_FAIL_COND(!uniform_set.is_valid());

	const Size2i sample_size = p_view->get_sample_grid_size();
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, gpu->generate_pipeline);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	rd->compute_list_dispatch_threads(cl, uint32_t(sample_size.x), uint32_t(sample_size.y), 1u);
	rd->compute_list_end();
}

void MegaLights::_dispatch_visibility_chain(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, const VisibilityOptions &p_opts, bool *r_retrace_ran) {
	if (r_retrace_ran != nullptr) {
		*r_retrace_ran = false;
	}
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_COND(rd == nullptr || usc == nullptr || light_storage == nullptr || texture_storage == nullptr || material_storage == nullptr);

	if (!_ensure_screen_trace_shaders()) {
		return;
	}

	const Size2i full_size = p_view->get_internal_size();
	const Size2i sample_size = p_view->get_sample_grid_size();
	const uint32_t spp = p_view->get_samples_per_pixel();
	const uint32_t total_samples = MAX(uint32_t(sample_size.x) * uint32_t(sample_size.y) * spp, 1u);
	if (!p_view->ensure_visibility_buffers(total_samples)) {
		return;
	}

	const RenderDataRD *rd_data = p_inputs.render_data;
	const RenderSceneDataRD *scene_data = rd_data->scene_data;
	const uint32_t frame_index = p_opts.use_explicit_frame_index ? p_opts.frame_index : p_view->get_frame_index();

	const bool use_screen_traces = p_opts.override_screen_traces ? p_opts.screen_traces : p_settings.screen_traces;
	const bool want_far_field = p_opts.override_far_field ? p_opts.far_field : p_settings.far_field;
	const bool want_swrt = p_opts.override_swrt ? p_opts.swrt : p_settings.swrt_continuation;

	NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;
	const bool hwrt_tlas_ready = rt_scene != nullptr && rt_scene->is_tlas_traceable() && rt_scene->get_tlas().is_valid() &&
			rt_scene->get_shadow_instance_buffer().is_valid() && rt_scene->get_shadow_alpha_buffer().is_valid();
	const bool swrt_ready = want_swrt && p_inputs.swrt_trace != nullptr && p_inputs.swrt_trace->is_ready() && _ensure_trace_rays_swrt_shader();
	const bool hwrt_ready = !swrt_ready && hwrt_tlas_ready && _ensure_trace_rays_shaders(p_inputs.next_gi);

	VirtualShadowMapStorage *vsm = VirtualShadowMapStorage::get_singleton();
	const bool vsm_buffers_ready = !p_opts.force_no_vsm && p_inputs.vsm_sample_view && vsm != nullptr && vsm->is_active() &&
			vsm->get_table_capacity() > 0u && vsm->get_map_buffers_capacity() == vsm->get_table_capacity() &&
			vsm->get_pool_texture().is_valid() && vsm->get_page_table_buffer().is_valid() &&
			vsm->get_map_data_buffer().is_valid() && vsm->get_map_state_buffer().is_valid() &&
			vsm->get_coarse_miss_buffer().is_valid() && _ensure_trace_vsm_shader();

	NextGIFarField *far_field = (hwrt_ready && want_far_field && rt_scene != nullptr) ? rt_scene->get_far_field() : nullptr;
	const bool use_far_field = far_field != nullptr && far_field->is_trace_ready() && far_field->get_tlas().is_valid();

	// Upload per-light shadow policies for all local and directional lights (so per-light
	// Light3D.megalights == ALLOW directional lights get policies even when directional_lights == false).
	_upload_light_shadow_policies(p_inputs, p_view, true, vsm_buffers_ready);

	// Fill MLVisParams UBO.
	const Projection depth_proj = NextGIViewMath::depth_projection(scene_data->cam_projection, p_inputs.flip_y, scene_data->taa_jitter);
	const Projection inv_proj = depth_proj.inverse();
	VisParamsUBO ubo;
	MaterialStorage::store_camera(inv_proj, ubo.inv_projection);
	MaterialStorage::store_camera(depth_proj, ubo.depth_projection);
	MaterialStorage::store_transform_transposed_3x4(scene_data->cam_transform, ubo.inv_view_rows);

	ubo.screen_sample_size[0] = uint32_t(full_size.x);
	ubo.screen_sample_size[1] = uint32_t(full_size.y);
	ubo.screen_sample_size[2] = uint32_t(sample_size.x);
	ubo.screen_sample_size[3] = uint32_t(sample_size.y);

	uint32_t vis_flags = 0u;
	if (use_screen_traces) {
		vis_flags |= VIS_FLAG_SCREEN_TRACES;
	}
	if (p_opts.dedupe_enabled) {
		vis_flags |= VIS_FLAG_DEDUPE;
	}
	if (scene_data->cam_orthogonal) {
		vis_flags |= VIS_FLAG_ORTHOGONAL;
	}
	if (use_far_field) {
		vis_flags |= VIS_FLAG_FAR_FIELD;
	}
	if (swrt_ready) {
		vis_flags |= VIS_FLAG_SWRT;
	}
	if (vsm_buffers_ready) {
		vis_flags |= VIS_FLAG_VSM_ENABLED;
	}
	if (!hwrt_ready && !swrt_ready) {
		vis_flags |= VIS_FLAG_NO_RT_FALLBACK;
	}
	if (p_inputs.hiz_texture.is_valid() && p_inputs.hiz_mip_count > 1u) {
		vis_flags |= VIS_FLAG_USE_HIZ;
	}

	ubo.sample_config[0] = spp;
	ubo.sample_config[1] = p_view->get_downsample_mode();
	ubo.sample_config[2] = frame_index;
	ubo.sample_config[3] = vis_flags;

	ubo.light_counts[0] = light_storage->get_omni_light_count();
	ubo.light_counts[1] = light_storage->get_spot_light_count();
	ubo.light_counts[2] = light_storage->get_area_light_count();
	ubo.light_counts[3] = rd_data->directional_light_count;

	const float dist_len = p_settings.distant_screen_traces_length;
	const float near_len = MIN(DEFAULT_NEAR_SCREEN_TRACE_LENGTH, dist_len);
	ubo.screen_trace_params[0] = MAX(p_inputs.surface_thickness, 0.001f);
	ubo.screen_trace_params[1] = near_len;
	ubo.screen_trace_params[2] = dist_len;
	ubo.screen_trace_params[3] = DEFAULT_DISTANT_SCREEN_TRACE_START_DEPTH;

	ubo.trace_config[0] = screen_trace_steps_for_quality(p_inputs.quality_level);
	ubo.trace_config[1] = p_inputs.hiz_mip_count > 0u ? (p_inputs.hiz_mip_count - 1u) : 0u;
	ubo.trace_config[2] = p_view->get_vis_list_capacity();
	ubo.trace_config[3] = p_view->get_vis_list_capacity();

	const float near_radius = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));
	const float far_distance = float(GLOBAL_GET("rendering/global_illumination/nextgi/far_field_distance"));
	ubo.rt_params[0] = MAX(near_radius, 1.0f);
	ubo.rt_params[1] = MAX(far_distance, near_radius + 1.0f);
	ubo.rt_params[2] = float(scene_data->z_far);
	ubo.rt_params[3] = float(NextGISWRTTrace::MAX_STEPS);

	rd->buffer_update(p_view->get_vis_params_ubo(), 0, sizeof(ubo), &ubo);
	rd->buffer_clear(p_view->get_vis_counters_buffer(), 0, 8u * sizeof(uint32_t));

	const RID default_black = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	const RID depth_tex = p_inputs.depth_texture.is_valid() ? p_inputs.depth_texture : default_black;
	const RID nr_tex = p_inputs.normal_roughness_texture.is_valid() ? p_inputs.normal_roughness_texture : default_black;
	const RID hiz_tex = p_inputs.hiz_texture.is_valid() ? p_inputs.hiz_texture : depth_tex;
	const RID sampler_nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	rd->draw_command_begin_label("MegaLights Visibility Chain");

	// Pass 1: Hi-Z screen trace, 2x2 block ray dedupe, and stream compaction into ray_list & vsm_list.
	{
		const RID shader_rid = gpu->vis_screen_shader.version_get_shader(gpu->vis_screen_version, GPUState::SCREEN_TRACE_MODE_TRACE);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_vis_params_ubo()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, depth_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, nr_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, hiz_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 4, sampler_nearest));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_view->get_sample_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_view->get_light_shadow_policy_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_view->get_vis_counters_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_view->get_vis_ray_list_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_view->get_vis_vsm_list_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_view->get_vis_sample_ray_map_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_view->get_visibility_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, p_view->get_hit_distance_texture()));
		const RID set = usc->get_cache_vec(shader_rid, 0, u);
		if (set.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->vis_screen_pipelines[GPUState::SCREEN_TRACE_MODE_TRACE]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch_threads(cl, uint32_t(sample_size.x), uint32_t(sample_size.y), 1u);
			rd->compute_list_end();
		}
	}

	// Pass 2: Build indirect dispatch arguments for ray list and VSM list.
	{
		const RID shader_rid = gpu->vis_screen_shader.version_get_shader(gpu->vis_screen_version, GPUState::SCREEN_TRACE_MODE_FINALIZE);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_vis_params_ubo()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_view->get_vis_counters_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_view->get_vis_indirect_buffer()));
		const RID set = usc->get_cache_vec(shader_rid, 0, u);
		if (set.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->vis_screen_pipelines[GPUState::SCREEN_TRACE_MODE_FINALIZE]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, 1u, 1u, 1u);
			rd->compute_list_end();
		}
	}

	// Pass 3a: Trace compacted ray list via SWRT global SDF (non-RT extension) OR HWRT ray query (+ optional far-field & #130 retrace).
	if (swrt_ready) {
		const RID shader_rid = gpu->vis_trace_swrt_shader.version_get_shader(gpu->vis_trace_swrt_version, 0);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_vis_params_ubo()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, depth_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, nr_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3, sampler_nearest));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_view->get_sample_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_view->get_light_shadow_policy_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_view->get_vis_counters_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_view->get_vis_ray_list_buffer()));
		const RID set0 = usc->get_cache_vec(shader_rid, 0, u);
		const RID set1 = p_inputs.swrt_trace->create_trace_uniform_set(shader_rid, 1, true, p_inputs.voxel_lighting);
		if (set0.is_valid() && set1.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->vis_trace_swrt_pipeline);
			rd->compute_list_bind_uniform_set(cl, set0, 0);
			rd->compute_list_bind_uniform_set(cl, set1, 1);
			rd->compute_list_dispatch_indirect(cl, p_view->get_vis_indirect_buffer(), 0);
			rd->compute_list_end();
		}
		if (set1.is_valid()) {
			rd->free_rid(set1);
		}
	} else if (hwrt_ready && p_view->ensure_retrace_buffer()) {
		const bool use_alpha = rt_scene->wants_shadow_alpha_variant() && rd->has_feature(RD::SUPPORTS_BINDLESS) && rt_scene->ensure_shadow_texture_table().is_valid();
		NextGIHitMaterials *hit_materials = (p_inputs.next_gi != nullptr && p_inputs.next_gi->get_hit_availability_for(NEXTGI_HIT_CONSUMER_MATERIAL_RETRACE) == NEXTGI_HIT_AVAILABLE) ? p_inputs.next_gi->get_hit_materials() : nullptr;
		const uint32_t retrace_capacity = (hit_materials != nullptr && rt_scene->wants_shadow_retrace() && hit_materials->has_current_records()) ? uint32_t(NEXTGI_SHADOW_RETRACE_MAX_RECORDS) : 0u;
		const uint32_t retrace_header[4] = { 0u, retrace_capacity, 0u, 0u };
		rd->buffer_update(p_view->get_vis_retrace_buffer(), 0, sizeof(retrace_header), retrace_header);

		const int variant = (use_alpha ? 1 : 0) | (use_far_field ? 2 : 0);
		const RID shader_rid = gpu->vis_trace_rays_shader.version_get_shader(gpu->vis_trace_rays_version, variant);

		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_vis_params_ubo()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, depth_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, nr_tex));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3, sampler_nearest));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_view->get_sample_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_view->get_light_shadow_policy_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_view->get_vis_counters_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_view->get_vis_ray_list_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 8, rt_scene->get_tlas()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, rt_scene->get_shadow_instance_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, rt_scene->get_shadow_alpha_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_view->get_vis_retrace_buffer()));

		if (use_alpha) {
			LocalVector<RD::Uniform> alpha_uniforms;
			rt_scene->append_shadow_alpha_uniforms(12, 16, alpha_uniforms);
			for (uint32_t i = 0; i < alpha_uniforms.size(); i++) {
				u.push_back(alpha_uniforms[i]);
			}
			const RID table = rt_scene->ensure_shadow_texture_table();
			if (gpu->vis_shadow_texture_set.is_null() || gpu->vis_shadow_texture_set_table != table || !rd->uniform_set_is_valid(gpu->vis_shadow_texture_set)) {
				if (gpu->vis_shadow_texture_set.is_valid() && rd->uniform_set_is_valid(gpu->vis_shadow_texture_set)) {
					rd->free_rid(gpu->vis_shadow_texture_set);
				}
				RD::Uniform table_uniform(RD::UNIFORM_TYPE_BINDLESS_TABLE, 0, table);
				gpu->vis_shadow_texture_set = rd->uniform_set_create(table_uniform, shader_rid, 1);
				gpu->vis_shadow_texture_set_table = table;
			}
		}
		if (use_far_field) {
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 17, far_field->get_tlas()));
		}

		const RID set0 = usc->get_cache_vec(shader_rid, 0, u);
		if (set0.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->vis_trace_rays_pipelines[variant]);
			rd->compute_list_bind_uniform_set(cl, set0, 0);
			if (use_alpha && gpu->vis_shadow_texture_set.is_valid()) {
				rd->compute_list_bind_uniform_set(cl, gpu->vis_shadow_texture_set, 1);
			}
			rd->compute_list_dispatch_indirect(cl, p_view->get_vis_indirect_buffer(), 0);
			rd->compute_list_end();
		}

		// Optional Pass 3b: #145/#130 material retrace for custom shader alpha/discard casters.
		if (retrace_capacity > 0u && hit_materials != nullptr &&
				hit_materials->trace_shadow_retrace(rt_scene, p_view->get_vis_retrace_buffer(), retrace_capacity, float(scene_data->time))) {
			const RID resolve_shader = gpu->vis_screen_shader.version_get_shader(gpu->vis_screen_version, GPUState::SCREEN_TRACE_MODE_RETRACE_RESOLVE);
			LocalVector<RD::Uniform> ru;
			ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_view->get_vis_retrace_buffer()));
			ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_view->get_vis_ray_list_buffer()));
			const RID rset = usc->get_cache_vec(resolve_shader, 0, ru);
			if (rset.is_valid()) {
				RD::ComputeListID cl = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(cl, gpu->vis_screen_pipelines[GPUState::SCREEN_TRACE_MODE_RETRACE_RESOLVE]);
				rd->compute_list_bind_uniform_set(cl, rset, 0);
				rd->compute_list_dispatch(cl, (retrace_capacity + 63u) / 64u, 1u, 1u);
				rd->compute_list_end();
				if (r_retrace_ran != nullptr) {
					*r_retrace_ran = true;
				}
			}
		}
	}

	// Pass 4: VSM-method lights (megalights_shadow_method == VIRTUAL), SMRT rays = 2, zero ray queries.
	if (vsm_buffers_ready) {
		VisVSMProjectionParams vp;
		const Projection &cam_proj = scene_data->cam_projection;
		const float size_x = 2.0f / (Math::abs(float(cam_proj.columns[0][0])) * float(MAX(full_size.x, 1)));
		const float size_y = 2.0f / (Math::abs(float(cam_proj.columns[1][1])) * float(MAX(full_size.y, 1)));
		if (cam_proj.is_orthogonal()) {
			vp.footprint[0] = 0.0f;
			vp.footprint[1] = MIN(size_x, size_y);
		} else {
			vp.footprint[0] = MIN(size_x, size_y);
			vp.footprint[1] = 0.0f;
		}
		vp.bias[0] = vsm->get_lod_bias_directional();
		vp.bias[1] = vsm->get_lod_bias_local();
		vp.counts[0] = vsm->get_table_capacity();
		vp.counts[1] = VSM_SMRT_RAYS; // Local SMRT rays = 2 (plan §21.3 M4).
		vp.counts[2] = VSM_SMRT_RAYS; // Directional SMRT rays = 2 (plan §21.3 M4).
		vp.counts[3] = MAX(RendererVirtualShadowMapStorage::smrt_preset_steps(vsm->get_quality()), 1u);
		vp.misc[0] = 0u;
		rd->buffer_update(p_view->get_vis_vsm_params_buffer(), 0, sizeof(vp), &vp);

		const RID shader_rid = gpu->vis_trace_vsm_shader.version_get_shader(gpu->vis_trace_vsm_version, 0);
		const RID vsm_bn = p_inputs.vsm_blue_noise_texture.is_valid() ? p_inputs.vsm_blue_noise_texture : default_black;
		LocalVector<RD::Uniform> vu;
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_vis_params_ubo()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, depth_tex));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, nr_tex));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 3, sampler_nearest));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_view->get_sample_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_view->get_vis_counters_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_view->get_vis_vsm_list_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_view->get_visibility_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_view->get_hit_distance_texture()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, light_storage->get_omni_light_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, light_storage->get_spot_light_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, light_storage->get_area_light_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 12, light_storage->get_directional_light_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 13, vsm->get_pool_texture()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, vsm->get_page_table_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, vsm->get_map_data_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, vsm->get_map_state_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, p_view->get_vis_vsm_params_buffer()));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 18, vsm_bn));
		vu.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, vsm->get_coarse_miss_buffer()));
		const RID vset = usc->get_cache_vec(shader_rid, 0, vu);
		if (vset.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->vis_trace_vsm_pipeline);
			rd->compute_list_bind_uniform_set(cl, vset, 0);
			rd->compute_list_dispatch_indirect(cl, p_view->get_vis_indirect_buffer(), 16);
			rd->compute_list_end();
		}
	}

	// Pass 5: Scatter compacted ray_list outputs to all leader and 2x2 deduplicated follower samples.
	if (hwrt_ready || swrt_ready) {
		const RID shader_rid = gpu->vis_screen_shader.version_get_shader(gpu->vis_screen_version, GPUState::SCREEN_TRACE_MODE_RESOLVE);
		LocalVector<RD::Uniform> su;
		su.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_vis_params_ubo()));
		su.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_view->get_vis_sample_ray_map_buffer()));
		su.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_view->get_vis_ray_list_buffer()));
		su.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_view->get_visibility_buffer()));
		su.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_view->get_hit_distance_texture()));
		const RID sset = usc->get_cache_vec(shader_rid, 0, su);
		if (sset.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->vis_screen_pipelines[GPUState::SCREEN_TRACE_MODE_RESOLVE]);
			rd->compute_list_bind_uniform_set(cl, sset, 0);
			rd->compute_list_dispatch(cl, (total_samples + 63u) / 64u, 1u, 1u);
			rd->compute_list_end();
		}
	}

	rd->draw_command_end_label();
}

void MegaLights::trace_visibility(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null()) {
		return;
	}
	Ref<MegaLightsViewData> view = get_view_data(p_rb.ptr());
	if (view.is_null() || !view->get_sample_buffer().is_valid()) {
		return;
	}
	const Settings settings = read_settings();
	VisibilityOptions opts;
	_dispatch_visibility_chain(p_inputs, view, settings, opts);
}

void MegaLights::_dispatch_guiding(const Ref<MegaLightsViewData> &p_view, RID p_visibility_buffer, uint32_t p_frame_index, bool p_force_visible_one) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(usc);

	const Size2i full_size = p_view->get_internal_size();
	const Size2i sample_size = p_view->get_sample_grid_size();
	const Size2i tile_dims = p_view->get_guiding_tile_dims();

	GuidingPushConstant pc;
	pc.full_size[0] = uint32_t(full_size.x);
	pc.full_size[1] = uint32_t(full_size.y);
	pc.sample_size[0] = uint32_t(sample_size.x);
	pc.sample_size[1] = uint32_t(sample_size.y);
	pc.tile_dims[0] = uint32_t(tile_dims.x);
	pc.tile_dims[1] = uint32_t(tile_dims.y);
	pc.samples_per_pixel = p_view->get_samples_per_pixel();
	pc.flags = (p_frame_index & GUIDING_FLAG_PHASE_MASK);
	if (p_view->get_downsample_mode() == DOWNSAMPLE_HALF) {
		pc.flags |= GUIDING_FLAG_DOWNSAMPLE_HALF;
	}
	if (p_force_visible_one || !p_visibility_buffer.is_valid()) {
		pc.flags |= GUIDING_FLAG_FORCE_VISIBLE_ONE;
	}

	const RID vis_buf = p_visibility_buffer.is_valid() ? p_visibility_buffer : p_view->get_visibility_buffer();
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_view->get_sample_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, vis_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_view->get_current_guiding_buffer()));

	const RID shader_rid = gpu->guiding_shader.version_get_shader(gpu->guiding_version, 0);
	const RID uniform_set = usc->get_cache_vec(shader_rid, 0, uniforms);
	ERR_FAIL_COND(!uniform_set.is_valid());

	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, gpu->guiding_pipeline);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	rd->compute_list_set_push_constant(cl, &pc, sizeof(pc));
	rd->compute_list_dispatch(cl, uint32_t(tile_dims.x), uint32_t(tile_dims.y), 1u);
	rd->compute_list_end();
}

void MegaLights::_dispatch_reference_unshadowed(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_n, float p_falloff_override) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(usc);

	ParamsUBO ubo;
	_fill_params_ubo(p_inputs, p_view, p_settings, p_frame_index, p_ref_n, false, true, p_falloff_override, ubo);
	rd->buffer_update(p_view->get_params_ubo(), 0, sizeof(ubo), &ubo);

	Vector<RD::Uniform> uniforms;
	_build_common_uniforms(p_inputs, p_view, uniforms);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, p_view->get_layer_group_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 21, p_view->get_reference_accum_texture()));

	LocalVector<RD::Uniform> uniform_lv;
	uniform_lv.resize(uniforms.size());
	for (int i = 0; i < uniforms.size(); i++) {
		uniform_lv[i] = uniforms[i];
	}

	const RID shader_rid = gpu->ref_compute_shader.version_get_shader(gpu->ref_compute_version, 0);
	const RID uniform_set = usc->get_cache_vec(shader_rid, 0, uniform_lv);
	ERR_FAIL_COND(!uniform_set.is_valid());

	const Size2i full_size = p_view->get_internal_size();
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, gpu->ref_unshadowed_pipeline);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	rd->compute_list_dispatch_threads(cl, uint32_t(full_size.x), uint32_t(full_size.y), 1u);
	rd->compute_list_end();
}

void MegaLights::_dispatch_reference_rt(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_n, bool p_force_visible_one, float p_falloff_override) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(usc);

	NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;
	const bool tlas_ready = rt_scene != nullptr && rt_scene->is_tlas_traceable() && rt_scene->get_tlas().is_valid() && rt_scene->get_shadow_instance_buffer().is_valid() && rt_scene->get_shadow_alpha_buffer().is_valid();
	if (p_force_visible_one || !tlas_ready) {
		_dispatch_reference_unshadowed(p_inputs, p_view, p_settings, p_frame_index, p_ref_n, p_falloff_override);
		return;
	}

	const bool want_alpha = rt_scene->wants_shadow_alpha_variant() && rd->has_feature(RD::SUPPORTS_BINDLESS) && rt_scene->ensure_shadow_texture_table().is_valid();
	const bool use_alpha = want_alpha && _ensure_rt_reference_shader(p_inputs.next_gi, true);
	if (!use_alpha && !_ensure_rt_reference_shader(p_inputs.next_gi, false)) {
		_dispatch_reference_unshadowed(p_inputs, p_view, p_settings, p_frame_index, p_ref_n, p_falloff_override);
		return;
	}

	ParamsUBO ubo;
	_fill_params_ubo(p_inputs, p_view, p_settings, p_frame_index, p_ref_n, false, false, p_falloff_override, ubo);
	rd->buffer_update(p_view->get_params_ubo(), 0, sizeof(ubo), &ubo);

	Vector<RD::Uniform> uniforms;
	_build_common_uniforms(p_inputs, p_view, uniforms);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, p_view->get_layer_group_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 21, p_view->get_reference_accum_texture()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 22, rt_scene->get_tlas()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, rt_scene->get_shadow_instance_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, rt_scene->get_shadow_alpha_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, p_view->get_light_shadow_policy_buffer()));

	LocalVector<RD::Uniform> uniform_lv;
	uniform_lv.resize(uniforms.size());
	for (int i = 0; i < uniforms.size(); i++) {
		uniform_lv[i] = uniforms[i];
	}

	RID shader_rid;
	RID pipeline_rid;
	if (use_alpha) {
		LocalVector<RD::Uniform> alpha_uniforms;
		rt_scene->append_shadow_alpha_uniforms(26, 30, alpha_uniforms);
		for (uint32_t i = 0; i < alpha_uniforms.size(); i++) {
			uniform_lv.push_back(alpha_uniforms[i]);
		}
		shader_rid = gpu->ref_rt_alpha_shader.version_get_shader(gpu->ref_rt_alpha_version, 0);
		pipeline_rid = gpu->ref_rt_alpha_pipeline;
		const RID table = rt_scene->ensure_shadow_texture_table();
		if (gpu->shadow_texture_set.is_null() || gpu->shadow_texture_set_table != table || !rd->uniform_set_is_valid(gpu->shadow_texture_set)) {
			if (gpu->shadow_texture_set.is_valid() && rd->uniform_set_is_valid(gpu->shadow_texture_set)) {
				rd->free_rid(gpu->shadow_texture_set);
			}
			RD::Uniform table_uniform(RD::UNIFORM_TYPE_BINDLESS_TABLE, 0, table);
			gpu->shadow_texture_set = rd->uniform_set_create(table_uniform, shader_rid, 1);
			gpu->shadow_texture_set_table = table;
		}
	} else {
		shader_rid = gpu->ref_rt_shader.version_get_shader(gpu->ref_rt_version, 0);
		pipeline_rid = gpu->ref_rt_pipeline;
	}

	const RID uniform_set = usc->get_cache_vec(shader_rid, 0, uniform_lv);
	ERR_FAIL_COND(!uniform_set.is_valid());

	const Size2i full_size = p_view->get_internal_size();
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipeline_rid);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	if (use_alpha) {
		rd->compute_list_bind_uniform_set(cl, gpu->shadow_texture_set, 1);
	}
	rd->compute_list_dispatch_threads(cl, uint32_t(full_size.x), uint32_t(full_size.y), 1u);
	rd->compute_list_end();
}

void MegaLights::_dispatch_verify_samples(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_step_n, float p_falloff_override) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(usc);

	ParamsUBO ubo;
	_fill_params_ubo(p_inputs, p_view, p_settings, p_frame_index, p_step_n, false, true, p_falloff_override, ubo);
	rd->buffer_update(p_view->get_params_ubo(), 0, sizeof(ubo), &ubo);

	Vector<RD::Uniform> uniforms;
	_build_common_uniforms(p_inputs, p_view, uniforms);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, p_view->get_layer_group_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 21, p_view->get_verify_accum_texture()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, p_view->get_sample_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, p_view->get_verify_counts_buffer()));

	LocalVector<RD::Uniform> uniform_lv;
	uniform_lv.resize(uniforms.size());
	for (int i = 0; i < uniforms.size(); i++) {
		uniform_lv[i] = uniforms[i];
	}

	const RID shader_rid = gpu->ref_compute_shader.version_get_shader(gpu->ref_compute_version, 1);
	const RID uniform_set = usc->get_cache_vec(shader_rid, 0, uniform_lv);
	ERR_FAIL_COND(!uniform_set.is_valid());

	const Size2i sample_size = p_view->get_sample_grid_size();
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, gpu->ref_verify_pipeline);
	rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
	rd->compute_list_dispatch_threads(cl, uint32_t(sample_size.x), uint32_t(sample_size.y), 1u);
	rd->compute_list_end();
}

void MegaLights::generate_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null()) {
		return;
	}
	if (!_ensure_common_shaders()) {
		return;
	}
	const Settings settings = read_settings();
	Ref<MegaLightsViewData> view = ensure_view_data(p_rb.ptr());
	if (view.is_null() || !view->ensure_sampling_buffers(p_rb->get_internal_size(), settings.samples_per_pixel, uint32_t(settings.downsample))) {
		return;
	}
	_upload_layer_group_masks(view);
	_dispatch_generate(p_inputs, view, settings, view->get_frame_index(), view->has_guiding_history(), -1.0f);
}

void MegaLights::update_guiding(Ref<RenderSceneBuffersRD> p_rb, RID p_visibility_buffer, bool p_force_visible_one) {
	if (p_rb.is_null() || !_ensure_common_shaders()) {
		return;
	}
	Ref<MegaLightsViewData> view = get_view_data(p_rb.ptr());
	if (view.is_null() || !view->get_sample_buffer().is_valid()) {
		return;
	}
	_dispatch_guiding(view, p_visibility_buffer, view->get_frame_index(), p_force_visible_one);
	view->advance_guiding_history();
}

void MegaLights::_dispatch_shade(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, bool p_force_visible_one, float p_falloff_override) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_COND(rd == nullptr || usc == nullptr || texture_storage == nullptr || material_storage == nullptr);

	if (!_ensure_shade_shaders()) {
		return;
	}

	const Size2i full_size = p_view->get_internal_size();
	const Size2i sample_size = p_view->get_sample_grid_size();
	const uint32_t ds_mode = uint32_t(p_settings.downsample);
	if (!p_view->ensure_shade_buffers(full_size, sample_size, ds_mode)) {
		return;
	}

	ParamsUBO ubo;
	_fill_params_ubo(p_inputs, p_view, p_settings, p_frame_index, 0u, p_view->has_guiding_history(), p_force_visible_one, p_falloff_override, ubo);
	rd->buffer_update(p_view->get_params_ubo(), 0, sizeof(ubo), &ubo);

	const bool half_res = (p_settings.downsample == DOWNSAMPLE_HALF);
	const RID out_diff = half_res ? p_view->get_shade_sample_diffuse() : p_view->get_noisy_diffuse_texture();
	const RID out_spec = half_res ? p_view->get_shade_sample_specular() : p_view->get_noisy_specular_texture();

	rd->draw_command_begin_label("MegaLights Shade & Demodulate");

	// Pass 1: Shade samples at sampling resolution and demodulate diffuse (albedo) and specular (EnvBRDF).
	{
		Vector<RD::Uniform> uniforms;
		_build_common_uniforms(p_inputs, p_view, uniforms);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, p_view->get_layer_group_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, p_view->get_sample_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, p_view->get_visibility_buffer()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 23, p_view->get_hit_distance_texture()));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 24, out_diff));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 25, out_spec));

		LocalVector<RD::Uniform> uniform_lv;
		uniform_lv.resize(uniforms.size());
		for (int i = 0; i < uniforms.size(); i++) {
			uniform_lv[i] = uniforms[i];
		}

		const RID shader_rid = gpu->shade_shader.version_get_shader(gpu->shade_version, GPUState::SHADE_MODE_SHADE);
		const RID set = usc->get_cache_vec(shader_rid, 0, uniform_lv);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->shade_pipelines[GPUState::SHADE_MODE_SHADE]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch_threads(cl, uint32_t(sample_size.x), uint32_t(sample_size.y), 1u);
			rd->compute_list_end();
		}
	}

	// Pass 2 (half-res only): 2x2 joint-bilateral fill to render resolution, keeping sampled pixels exact.
	if (half_res) {
		const RID default_black = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		const RID default_white = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
		const RID sampler_nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

		LocalVector<RD::Uniform> fu;
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_view->get_params_ubo()));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.depth_texture.is_valid() ? p_inputs.depth_texture : default_black));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.normal_roughness_texture.is_valid() ? p_inputs.normal_roughness_texture : default_black));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_inputs.ml_albedo_texture.is_valid() ? p_inputs.ml_albedo_texture : default_white));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, p_inputs.ml_material_texture.is_valid() ? p_inputs.ml_material_texture : default_black));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, p_view->get_shade_sample_diffuse()));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_view->get_shade_sample_specular()));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 7, sampler_nearest));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_view->get_noisy_diffuse_texture()));
		fu.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_view->get_noisy_specular_texture()));

		const RID fill_shader = gpu->shade_shader.version_get_shader(gpu->shade_version, GPUState::SHADE_MODE_FILL_HALF);
		const RID fset = usc->get_cache_vec(fill_shader, 0, fu);
		if (fset.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->shade_pipelines[GPUState::SHADE_MODE_FILL_HALF]);
			rd->compute_list_bind_uniform_set(cl, fset, 0);
			rd->compute_list_dispatch_threads(cl, uint32_t(full_size.x), uint32_t(full_size.y), 1u);
			rd->compute_list_end();
		}
	}

	rd->draw_command_end_label();
}

void MegaLights::_dispatch_denoise(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, uint32_t p_frame_index, bool p_rr_bypass, int p_debug_vis_mode, bool p_force_reset, const Vector<Rect2i> &p_dirty_regions, bool p_submit_async_readback, bool p_in_check_loop) {
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr) {
		return;
	}
	const Size2i full_size = p_view->get_internal_size();
	if (!p_view->get_noisy_diffuse_texture().is_valid() || !p_view->get_noisy_specular_texture().is_valid() || !p_view->get_denoise_params_ubo().is_valid()) {
		return;
	}
	if (!p_view->ensure_denoise_buffers(full_size, p_rr_bypass)) {
		return;
	}

	RID debug_out;
	if (p_debug_vis_mode >= 0 && p_view->ensure_denoise_debug_texture(full_size)) {
		debug_out = p_view->get_denoise_debug_texture();
	}

	NextGIDenoise *denoiser = _ensure_denoiser();
	ERR_FAIL_NULL(denoiser);

	const RenderSceneDataRD &sd = *p_inputs.render_data->scene_data;
	const uint64_t cur_frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0u;
	const bool frame_gap = !p_in_check_loop && p_view->has_denoise_history() && cur_frame > 0u && cur_frame != p_view->get_denoise_last_frame() + 1u;
	const bool cam_cut = !p_in_check_loop && p_view->has_denoise_cam_transform() && is_camera_cut(p_view->get_denoise_last_cam_transform(), sd.cam_transform);
	const bool has_history = !p_rr_bypass && p_view->has_denoise_history() && !p_force_reset && !frame_gap && !cam_cut;

	const Projection depth_proj = NextGIView::depth_projection(sd);
	const Projection inv_proj = NextGIView::inv_projection(sd);
	const Projection inv_view_proj = NextGIView::inv_view_projection(sd);
	const Projection cur_view_proj = depth_proj * Projection(sd.cam_transform.affine_inverse());
	const Projection prev_view_proj = (p_in_check_loop && p_frame_index > 0u) ? cur_view_proj : NextGIView::prev_view_projection(sd);
	const Projection prev_depth_proj = (p_in_check_loop && p_frame_index > 0u) ? depth_proj : NextGIViewMath::depth_projection(sd.prev_cam_projection, sd.flip_y, sd.prev_taa_jitter);
	const Vector4 cur_depth_coeffs = NextGIViewMath::depth_coefficients(depth_proj);
	const Vector4 prev_depth_coeffs = NextGIViewMath::depth_coefficients(prev_depth_proj);

	const uint32_t cur_slot = p_view->get_denoise_history_index();
	const uint32_t prev_slot = 1u - cur_slot;

	bool reset_mask_enabled = false;
	if (!p_rr_bypass && !p_dirty_regions.is_empty() && p_view->get_denoise_reset_mask_buffer().is_valid()) {
		const Size2i tile_dims = p_view->get_denoise_reset_tile_dims();
		const PackedByteArray byte_mask = NextGIDenoise::select_reset_probes(p_dirty_regions, tile_dims);
		LocalVector<uint32_t> mask_words;
		mask_words.resize(uint32_t(byte_mask.size()));
		for (int i = 0; i < byte_mask.size(); i++) {
			mask_words[uint32_t(i)] = uint32_t(byte_mask[i]);
		}
		if (!mask_words.is_empty()) {
			rd->buffer_update(p_view->get_denoise_reset_mask_buffer(), 0, mask_words.size() * sizeof(uint32_t), mask_words.ptr());
			reset_mask_enabled = true;
		}
	}

	NextGIDenoise::MegaLightsDenoiseParams dp;
	dp.rr_bypass = p_rr_bypass;
	// GodotNext: pu_dlss_rr (#206). Forward the view's RenderSceneBuffersRD so megalights_denoise()
	// honours the vendor-neutral DenoiserBypass contract.
	dp.render_buffers = p_inputs.render_data != nullptr ? p_inputs.render_data->render_buffers.ptr() : nullptr;
	dp.has_history = has_history;
	dp.velocity_enabled = !p_in_check_loop && p_inputs.velocity_enabled && p_inputs.velocity_texture.is_valid();
	dp.reset_mask_enabled = reset_mask_enabled;
	dp.orthogonal = sd.cam_orthogonal;
	dp.screen_size = full_size;
	dp.reset_tile_dims = p_view->get_denoise_reset_tile_dims();
	dp.inv_projection = inv_proj;
	dp.inv_view_proj = inv_view_proj;
	dp.prev_view_proj = prev_view_proj;
	dp.view_to_world = sd.cam_transform.basis;
	dp.cur_depth_coeffs = cur_depth_coeffs;
	dp.prev_depth_coeffs = prev_depth_coeffs;
	dp.camera_pos = sd.cam_transform.origin;
	dp.prev_camera_pos = (p_in_check_loop && p_frame_index > 0u) ? sd.cam_transform.origin : sd.prev_cam_transform.origin;
	dp.frame_index = p_frame_index;
	dp.max_history_diffuse = 16u;
	dp.max_history_specular = 16u;
	dp.noisy_diffuse = p_view->get_noisy_diffuse_texture();
	dp.noisy_specular = p_view->get_noisy_specular_texture();
	dp.depth = p_inputs.depth_texture;
	dp.normal_roughness = p_inputs.normal_roughness_texture;
	dp.ml_albedo = p_inputs.ml_albedo_texture;
	dp.ml_material = p_inputs.ml_material_texture;
	dp.dfg_lut = p_inputs.dfg_lut_texture;
	dp.velocity = p_inputs.velocity_texture;
	dp.reset_mask = p_view->get_denoise_reset_mask_buffer();
	dp.prev_depth_normal = p_view->get_denoise_prev_depth_normal();
	dp.history_diffuse_prev = p_view->get_denoise_diffuse_slot(prev_slot);
	dp.history_diffuse_cur = p_view->get_denoise_diffuse_slot(cur_slot);
	dp.history_specular_prev = p_view->get_denoise_specular_slot(prev_slot);
	dp.history_specular_cur = p_view->get_denoise_specular_slot(cur_slot);
	dp.spatial_scratch_diffuse = p_view->get_denoise_scratch_diffuse();
	dp.spatial_scratch_specular = p_view->get_denoise_scratch_specular();
	dp.moments_prev = p_view->get_denoise_moments_slot(prev_slot);
	dp.moments_cur = p_view->get_denoise_moments_slot(cur_slot);
	dp.history_len_prev = p_view->get_denoise_history_len_slot(prev_slot);
	dp.history_len_cur = p_view->get_denoise_history_len_slot(cur_slot);
	dp.clamp_stats_buffer = p_view->get_denoise_clamp_stats_buffer();
	dp.params_ubo = p_view->get_denoise_params_ubo();
	dp.debug_vis_mode = p_debug_vis_mode;
	dp.debug_vis_output = debug_out;

	if (p_submit_async_readback && !p_rr_bypass) {
		p_view->poll_denoise_stats();
	}

	const NextGIDenoise::MegaLightsDenoiseOutput out = denoiser->megalights_denoise(dp);
	p_view->set_denoise_output(out.diffuse, out.specular, out.bypassed);
	p_view->advance_denoise_history(cur_frame, sd.cam_transform, out.bypassed);

	if (p_submit_async_readback) {
		if (out.bypassed) {
			p_view->record_denoise_stats(0u, 0u, 0u, 0u, true);
		} else {
			p_view->submit_denoise_stats_readback();
		}
	}
}

void MegaLights::shade_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null()) {
		return;
	}
	Ref<MegaLightsViewData> view = get_view_data(p_rb.ptr());
	if (view.is_null() || !view->get_sample_buffer().is_valid()) {
		return;
	}
	const Settings settings = read_settings();
	_dispatch_shade(p_inputs, view, settings, view->get_frame_index(), false, -1.0f);
}

void MegaLights::denoise_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null()) {
		return;
	}
	Ref<MegaLightsViewData> view = get_view_data(p_rb.ptr());
	if (view.is_null() || !view->get_noisy_diffuse_texture().is_valid()) {
		return;
	}
	const Settings settings = read_settings();
	// GodotNext: pu_dlss_rr (#206). Honour vendor-neutral DenoiserBypass contract for MegaLights.
	const bool rr_bypass = (settings.denoiser == DENOISER_RR_BYPASS) || DenoiserBypass::is_bypassed(p_rb.ptr());
	const int debug_vis_mode = debug_vis_mode_from_viewport(p_inputs.debug_draw);
	_dispatch_denoise(p_inputs, view, view->get_frame_index(), rr_bypass, debug_vis_mode, p_inputs.reset_temporal_history, p_inputs.dirty_reset_regions, true, false);
}

void MegaLights::render_reference(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb, bool p_force_visible_one) {
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null()) {
		return;
	}
	if (!_ensure_common_shaders()) {
		return;
	}
	const Settings settings = read_settings();
	Ref<MegaLightsViewData> view = ensure_view_data(p_rb.ptr());
	const Size2i full_size = p_rb->get_internal_size();
	if (view.is_null() || !view->ensure_reference_buffers(full_size)) {
		return;
	}
	const uint64_t cam_hash = ml_hash_camera_and_size(p_inputs.render_data->scene_data->cam_transform, p_inputs.render_data->scene_data->cam_projection, full_size);
	uint32_t next_n = view->get_reference_frame_count() + 1u;
	if (view->get_reference_frame_count() == 0u || view->get_reference_camera_hash() != cam_hash) {
		view->reset_reference();
		next_n = 1u;
	}
	_upload_layer_group_masks(view);
	_upload_light_shadow_policies(p_inputs, view, true);
	// Reference mode uses falloff threshold 0.0 for unbiased exhaustive shading.
	_dispatch_reference_rt(p_inputs, view, settings, next_n - 1u, next_n, p_force_visible_one, 0.0f);
	view->set_reference_state(next_n, cam_hash);
}

Dictionary MegaLights::_run_check(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Dictionary &p_options) {
	RD *rd = RD::get_singleton();
	MegaLightsCheckState &st = ml_check_state();
	const Size2i full_size = p_inputs.render_data->render_buffers->get_internal_size();
	const uint64_t pixel_count = uint64_t(full_size.x) * uint64_t(full_size.y);

	Settings cfg = read_settings();
	if (p_options.has("samples_per_pixel")) {
		cfg.samples_per_pixel = uint32_t(MAX(int(p_options["samples_per_pixel"]), 1));
	}
	if (p_options.has("downsample")) {
		cfg.downsample = parse_downsample_variant(p_options["downsample"]);
	}
	if (p_options.has("directional_lights")) {
		cfg.directional_lights = bool(p_options["directional_lights"]);
	}
	if (p_options.has("falloff_cull_threshold")) {
		cfg.falloff_cull_threshold = float(p_options["falloff_cull_threshold"]);
	}
	cfg = validate_settings(cfg);

	const String mode = String(p_options.get("mode", "unbiased"));
	const uint32_t steps_per_frame = uint32_t(CLAMP(int(p_options.get("steps_per_frame", 64)), 1, 256));

	if (!p_view->ensure_sampling_buffers(full_size, cfg.samples_per_pixel, uint32_t(cfg.downsample)) ||
			!p_view->ensure_reference_buffers(full_size) ||
			!p_view->ensure_verify_buffers(full_size)) {
		Dictionary err;
		err["done"] = true;
		err["error"] = "Failed to allocate MegaLights check buffers.";
		return err;
	}
	_upload_layer_group_masks(p_view);

	if (mode == "unbiased") {
		const uint32_t total_frames = uint32_t(CLAMP(int(p_options.get("total_frames", 4096)), 1, 16384));
		const bool use_guiding = bool(p_options.get("use_guiding", true));
		const float ref_falloff = float(p_options.get("ref_falloff_threshold", 0.0));

		if (!st.initialized) {
			st.initialized = true;
			st.completed_steps = 0;
			st.total_steps = total_frames;
			p_view->reset_guiding();
			rd->texture_clear(p_view->get_verify_accum_texture(), Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->buffer_clear(p_view->get_verify_counts_buffer(), 0, uint32_t(pixel_count * sizeof(uint32_t)));
		}

		const uint32_t batch = MIN(steps_per_frame, st.total_steps - st.completed_steps);
		for (uint32_t i = 0; i < batch; i++) {
			const uint32_t step = st.completed_steps + i;
			const bool has_guiding = use_guiding && p_view->has_guiding_history();
			_dispatch_generate(p_inputs, p_view, cfg, step, has_guiding, cfg.falloff_cull_threshold);
			_dispatch_verify_samples(p_inputs, p_view, cfg, step, step + 1u, cfg.falloff_cull_threshold);
			if (use_guiding) {
				_dispatch_guiding(p_view, RID(), step, true);
				p_view->advance_guiding_history();
			}
		}
		st.completed_steps += batch;

		if (st.completed_steps < st.total_steps) {
			Dictionary progress;
			progress["done"] = false;
			progress["mode"] = mode;
			progress["progress"] = int(st.completed_steps);
			progress["total"] = int(st.total_steps);
			return progress;
		}

		// Exhaustive unshadowed ground truth on the exact same prepass & lights.
		_dispatch_reference_unshadowed(p_inputs, p_view, cfg, 0u, 1u, ref_falloff);

		const Vector<uint8_t> sample_bytes = rd->texture_get_data(p_view->get_verify_accum_texture(), 0);
		const Vector<uint8_t> ref_bytes = rd->texture_get_data(p_view->get_reference_accum_texture(), 0);
		const Vector<uint8_t> count_bytes = rd->buffer_get_data(p_view->get_verify_counts_buffer());

		const float *sample_px = reinterpret_cast<const float *>(sample_bytes.ptr());
		const float *ref_px = reinterpret_cast<const float *>(ref_bytes.ptr());
		const uint32_t *counts_px = reinterpret_cast<const uint32_t *>(count_bytes.ptr());

		LocalVector<float> luma_errs;
		LocalVector<float> rgb_errs;
		double sum_luma_err = 0.0;
		double sum_rgb_err = 0.0;
		double sum_ref_luma = 0.0;
		double sum_sample_luma = 0.0;
		float max_luma_err = 0.0f;
		float max_rgb_err = 0.0f;
		uint32_t lit_pixels = 0u;
		uint32_t min_visits = UINT32_MAX;
		uint32_t max_visits = 0u;

		for (uint64_t i = 0; i < pixel_count; i++) {
			const float rr = ref_px[i * 4u + 0u];
			const float rg = ref_px[i * 4u + 1u];
			const float rb = ref_px[i * 4u + 2u];
			const float ref_luma = ml_cpu_luminance(rr, rg, rb);
			if (!(ref_luma > 1e-3f)) {
				continue;
			}
			const float sr = sample_px[i * 4u + 0u];
			const float sg = sample_px[i * 4u + 1u];
			const float sb = sample_px[i * 4u + 2u];
			const float sample_luma = ml_cpu_luminance(sr, sg, sb);
			const uint32_t visits = counts_px[i];

			const float luma_rel = Math::abs(sample_luma - ref_luma) / ref_luma;
			const float dr = sr - rr;
			const float dg = sg - rg;
			const float db = sb - rb;
			const float ref_norm = std::sqrt(rr * rr + rg * rg + rb * rb);
			const float rgb_rel = std::sqrt(dr * dr + dg * dg + db * db) / MAX(ref_norm, 1e-6f);

			luma_errs.push_back(luma_rel);
			rgb_errs.push_back(rgb_rel);
			sum_luma_err += double(luma_rel);
			sum_rgb_err += double(rgb_rel);
			sum_ref_luma += double(ref_luma);
			sum_sample_luma += double(sample_luma);
			max_luma_err = MAX(max_luma_err, luma_rel);
			max_rgb_err = MAX(max_rgb_err, rgb_rel);
			min_visits = MIN(min_visits, visits);
			max_visits = MAX(max_visits, visits);
			lit_pixels++;
		}

		float p99_luma_err = 0.0f;
		float p99_rgb_err = 0.0f;
		if (!luma_errs.is_empty()) {
			SortArray<float> sorter;
			sorter.sort(luma_errs.ptr(), luma_errs.size());
			sorter.sort(rgb_errs.ptr(), rgb_errs.size());
			const uint32_t idx99 = MIN(uint32_t(double(luma_errs.size()) * 0.99), uint32_t(luma_errs.size() - 1u));
			p99_luma_err = luma_errs[idx99];
			p99_rgb_err = rgb_errs[idx99];
		}

		Dictionary res;
		res["done"] = true;
		res["mode"] = mode;
		res["frames"] = int(st.total_steps);
		res["samples_per_pixel"] = int(cfg.samples_per_pixel);
		res["downsample"] = cfg.downsample == DOWNSAMPLE_FULL ? "full" : "half";
		res["use_guiding"] = use_guiding;
		res["lit_pixels"] = int(lit_pixels);
		res["min_visits_per_pixel"] = lit_pixels > 0u ? int(min_visits) : 0;
		res["max_visits_per_pixel"] = int(max_visits);
		res["max_luma_rel_err"] = double(max_luma_err);
		res["p99_luma_rel_err"] = double(p99_luma_err);
		res["mean_luma_rel_err"] = lit_pixels > 0u ? (sum_luma_err / double(lit_pixels)) : 0.0;
		res["max_rgb_rel_err"] = double(max_rgb_err);
		res["p99_rgb_rel_err"] = double(p99_rgb_err);
		res["mean_rgb_rel_err"] = lit_pixels > 0u ? (sum_rgb_err / double(lit_pixels)) : 0.0;
		res["mean_ref_luma"] = lit_pixels > 0u ? (sum_ref_luma / double(lit_pixels)) : 0.0;
		res["mean_sample_luma"] = lit_pixels > 0u ? (sum_sample_luma / double(lit_pixels)) : 0.0;
		res["global_rel_err"] = sum_ref_luma > 1e-6 ? (Math::abs(sum_sample_luma - sum_ref_luma) / sum_ref_luma) : 0.0;
		return res;
	}

	if (mode == "reference_convergence") {
		const uint32_t total_frames = uint32_t(CLAMP(int(p_options.get("total_frames", 1024)), 2, 4096));
		const uint32_t checkpoint = uint32_t(CLAMP(int(p_options.get("rse_checkpoint", 16)), 2, int(total_frames)));
		const bool require_rt = bool(p_options.get("require_rt", true));

		NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;
		const bool tlas_ready = rt_scene != nullptr && rt_scene->is_tlas_traceable() && rt_scene->get_tlas().is_valid() && rt_scene->get_shadow_instance_buffer().is_valid() && rt_scene->get_shadow_alpha_buffer().is_valid();
		if (require_rt && !tlas_ready) {
			st.wait_frames++;
			if (st.wait_frames < 90u) {
				Dictionary waiting;
				waiting["done"] = false;
				waiting["mode"] = mode;
				waiting["waiting_for_tlas"] = true;
				waiting["wait_frames"] = int(st.wait_frames);
				return waiting;
			}
			Dictionary err;
			err["done"] = true;
			err["mode"] = mode;
			err["tlas_ready"] = false;
			err["error"] = "NextGI TLAS did not become traceable for reference_convergence.";
			return err;
		}

		if (!st.initialized) {
			st.initialized = true;
			st.completed_steps = 0;
			st.total_steps = total_frames;
			st.checkpoint_recorded = false;
			p_view->reset_reference();
		}

		_upload_light_shadow_policies(p_inputs, p_view, true);

		uint32_t target_after_batch = MIN(st.completed_steps + steps_per_frame, st.total_steps);
		if (!st.checkpoint_recorded && st.completed_steps < checkpoint && target_after_batch > checkpoint) {
			target_after_batch = checkpoint;
		}
		const uint32_t batch = target_after_batch - st.completed_steps;
		for (uint32_t i = 0; i < batch; i++) {
			const uint32_t n = st.completed_steps + i + 1u;
			_dispatch_reference_rt(p_inputs, p_view, cfg, n - 1u, n, !require_rt, 0.0f);
		}
		st.completed_steps += batch;

		auto compute_rse = [&](uint32_t p_n, double &r_max_rse, double &r_p99_rse, double &r_mean_rse, uint32_t &r_lit, uint32_t &r_penumbra, double &r_mean_luma) {
			const Vector<uint8_t> bytes = rd->texture_get_data(p_view->get_reference_accum_texture(), 0);
			const float *px = reinterpret_cast<const float *>(bytes.ptr());
			LocalVector<float> rses;
			double sum_rse = 0.0;
			double sum_luma = 0.0;
			float max_rse = 0.0f;
			uint32_t lit = 0u;
			uint32_t penumbra = 0u;
			for (uint64_t i = 0; i < pixel_count; i++) {
				const float mean_luma = ml_cpu_luminance(px[i * 4u + 0u], px[i * 4u + 1u], px[i * 4u + 2u]);
				const float m2 = px[i * 4u + 3u];
				if (!(mean_luma > 1e-3f)) {
					continue;
				}
				const float rse = welford_relative_std_error(mean_luma, m2, p_n);
				rses.push_back(rse);
				sum_rse += double(rse);
				sum_luma += double(mean_luma);
				max_rse = MAX(max_rse, rse);
				if (m2 > 1e-6f) {
					penumbra++;
				}
				lit++;
			}
			float p99 = 0.0f;
			if (!rses.is_empty()) {
				SortArray<float> sorter;
				sorter.sort(rses.ptr(), rses.size());
				const uint32_t idx99 = MIN(uint32_t(double(rses.size()) * 0.99), uint32_t(rses.size() - 1u));
				p99 = rses[idx99];
			}
			r_max_rse = double(max_rse);
			r_p99_rse = double(p99);
			r_mean_rse = lit > 0u ? (sum_rse / double(lit)) : 0.0;
			r_lit = lit;
			r_penumbra = penumbra;
			r_mean_luma = lit > 0u ? (sum_luma / double(lit)) : 0.0;
		};

		if (!st.checkpoint_recorded && st.completed_steps >= checkpoint) {
			uint32_t cp_lit = 0u;
			double cp_mean_luma = 0.0;
			compute_rse(st.completed_steps, st.checkpoint_max_rse, st.checkpoint_p99_rse, st.checkpoint_mean_rse, cp_lit, st.checkpoint_penumbra_pixels, cp_mean_luma);
			st.checkpoint_recorded = true;
		}

		if (st.completed_steps < st.total_steps) {
			Dictionary progress;
			progress["done"] = false;
			progress["mode"] = mode;
			progress["progress"] = int(st.completed_steps);
			progress["total"] = int(st.total_steps);
			return progress;
		}

		double max_rse = 0.0;
		double p99_rse = 0.0;
		double mean_rse = 0.0;
		double mean_luma = 0.0;
		uint32_t lit_pixels = 0u;
		uint32_t penumbra_pixels = 0u;
		compute_rse(st.total_steps, max_rse, p99_rse, mean_rse, lit_pixels, penumbra_pixels, mean_luma);

		Dictionary res;
		res["done"] = true;
		res["mode"] = mode;
		res["tlas_ready"] = tlas_ready;
		res["frames"] = int(st.total_steps);
		res["checkpoint_frames"] = int(checkpoint);
		res["checkpoint_max_rse"] = st.checkpoint_max_rse;
		res["checkpoint_p99_rse"] = st.checkpoint_p99_rse;
		res["checkpoint_mean_rse"] = st.checkpoint_mean_rse;
		res["checkpoint_penumbra_pixels"] = int(st.checkpoint_penumbra_pixels);
		res["lit_pixels"] = int(lit_pixels);
		res["penumbra_pixels"] = int(penumbra_pixels);
		res["max_rse"] = max_rse;
		res["p99_rse"] = p99_rse;
		res["mean_rse"] = mean_rse;
		res["mean_luma"] = mean_luma;
		return res;
	}

	if (mode == "guiding") {
		const uint32_t visible_limit = uint32_t(MAX(int(p_options.get("visible_limit", 4)), 1));
		const uint32_t after_frames = uint32_t(CLAMP(int(p_options.get("after_frames", 64)), 1, 256));

		p_view->reset_guiding();
		// Frame 0: uniform sampling (no guiding history).
		_dispatch_generate(p_inputs, p_view, cfg, 0u, false, cfg.falloff_cull_threshold);

		const Vector<uint8_t> s0_bytes = rd->buffer_get_data(p_view->get_sample_buffer());
		const SampleGPU *s0 = reinterpret_cast<const SampleGPU *>(s0_bytes.ptr());
		const uint32_t total_samples = uint32_t(s0_bytes.size() / sizeof(SampleGPU));

		LocalVector<uint32_t> vis_words;
		vis_words.resize(total_samples);
		uint64_t before_valid = 0;
		uint64_t before_vis = 0;
		for (uint32_t i = 0; i < total_samples; i++) {
			if (s0[i].light_packed == SAMPLE_INVALID) {
				vis_words[i] = 0u;
				continue;
			}
			before_valid++;
			const uint32_t idx = unpack_sample_light_index(s0[i].light_packed);
			if (idx < visible_limit) {
				vis_words[i] = 0xFFFFFFFFu;
				before_vis++;
			} else {
				vis_words[i] = 0u;
			}
		}
		rd->buffer_update(p_view->get_visibility_buffer(), 0, vis_words.size() * sizeof(uint32_t), vis_words.ptr());

		// Aggregate visible lights into 8x8 tiles via ml_guiding.glsl.
		_dispatch_guiding(p_view, p_view->get_visibility_buffer(), 0u, false);

		const Vector<uint8_t> g_bytes = rd->buffer_get_data(p_view->get_current_guiding_buffer());
		const uint32_t *g_words = reinterpret_cast<const uint32_t *>(g_bytes.ptr());
		const Size2i tile_dims = p_view->get_guiding_tile_dims();
		const uint32_t tile_count = uint32_t(tile_dims.x * tile_dims.y);

		uint32_t non_empty_tiles = 0u;
		uint32_t max_entries = 0u;
		uint32_t occluded_leaks = 0u;
		uint64_t tile_vis_sum = 0;
		for (uint32_t t = 0; t < tile_count; t++) {
			const uint32_t base = t * GUIDING_TILE_WORDS;
			const uint32_t entry_count = g_words[base + 0u];
			const uint32_t tile_vis = g_words[base + 1u];
			tile_vis_sum += tile_vis;
			if (entry_count > 0u) {
				non_empty_tiles++;
			}
			max_entries = MAX(max_entries, entry_count);
			for (uint32_t e = 0; e < MIN(entry_count, GUIDING_MAX_LIGHTS); e++) {
				const uint32_t entry = g_words[base + GUIDING_HEADER_WORDS + e];
				const uint32_t lid = unpack_guiding_light(entry);
				if (unpack_sample_light_index(lid) >= visible_limit) {
					occluded_leaks++;
				}
			}
		}

		// Flip ping-pong so next frames' ml_generate_samples reads the guided table.
		p_view->advance_guiding_history();

		uint64_t after_valid = 0;
		uint64_t after_vis = 0;
		uint64_t after_occ = 0;
		for (uint32_t f = 1u; f <= after_frames; f++) {
			_dispatch_generate(p_inputs, p_view, cfg, f, true, cfg.falloff_cull_threshold);
			const Vector<uint8_t> sf_bytes = rd->buffer_get_data(p_view->get_sample_buffer());
			const SampleGPU *sf = reinterpret_cast<const SampleGPU *>(sf_bytes.ptr());
			for (uint32_t i = 0; i < total_samples; i++) {
				if (sf[i].light_packed == SAMPLE_INVALID) {
					continue;
				}
				after_valid++;
				if (unpack_sample_light_index(sf[i].light_packed) < visible_limit) {
					after_vis++;
				} else {
					after_occ++;
				}
			}
		}

		Dictionary res;
		res["done"] = true;
		res["mode"] = mode;
		res["tile_count"] = int(tile_count);
		res["non_empty_tiles"] = int(non_empty_tiles);
		res["max_entries_per_tile"] = int(max_entries);
		res["occluded_leaks_in_tiles"] = int(occluded_leaks);
		res["tile_visible_sum"] = int64_t(tile_vis_sum);
		res["before_valid_samples"] = int64_t(before_valid);
		res["before_visible_samples"] = int64_t(before_vis);
		res["before_visible_fraction"] = before_valid > 0 ? (double(before_vis) / double(before_valid)) : 0.0;
		res["after_valid_samples"] = int64_t(after_valid);
		res["after_visible_samples"] = int64_t(after_vis);
		res["after_occluded_samples"] = int64_t(after_occ);
		res["after_visible_fraction"] = after_valid > 0 ? (double(after_vis) / double(after_valid)) : 0.0;
		return res;
	}

	if (mode == "visibility_chain") {
		const bool use_screen_traces = bool(p_options.get("screen_traces", cfg.screen_traces));
		const bool use_dedupe = bool(p_options.get("dedupe", true));
		const bool use_far_field = bool(p_options.get("far_field", false));
		const bool use_swrt = bool(p_options.get("swrt", false));
		const bool compare_bruteforce = bool(p_options.get("compare_bruteforce", true));
		const bool wait_for_tlas = bool(p_options.get("wait_for_tlas", !use_swrt));
		const bool wait_for_far_field = bool(p_options.get("wait_for_far_field", use_far_field));
		const bool wait_for_swrt = bool(p_options.get("wait_for_swrt", use_swrt));
		const bool wait_for_vsm = bool(p_options.get("wait_for_vsm", false));
		const bool wait_for_retrace = bool(p_options.get("wait_for_retrace", false));

		NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;
		const bool tlas_ready = rt_scene != nullptr && rt_scene->is_tlas_traceable() && rt_scene->get_tlas().is_valid() &&
				rt_scene->get_shadow_instance_buffer().is_valid() && rt_scene->get_shadow_alpha_buffer().is_valid();
		NextGIFarField *far_field = rt_scene != nullptr ? rt_scene->get_far_field() : nullptr;
		const bool far_field_ready = far_field != nullptr && far_field->is_trace_ready() && far_field->get_tlas().is_valid();
		const bool swrt_ready = p_inputs.swrt_trace != nullptr && p_inputs.swrt_trace->is_ready() && p_inputs.swrt_trace->get_instance_count() > 0u;
		VirtualShadowMapStorage *vsm = VirtualShadowMapStorage::get_singleton();
		const bool vsm_ready = p_inputs.vsm_sample_view && vsm != nullptr && vsm->is_active() && vsm->get_marked_view_count() > 0u;
		NextGIHitMaterials *hit_materials = (p_inputs.next_gi != nullptr && p_inputs.next_gi->get_hit_availability_for(NEXTGI_HIT_CONSUMER_MATERIAL_RETRACE) == NEXTGI_HIT_AVAILABLE) ? p_inputs.next_gi->get_hit_materials() : nullptr;
		const bool retrace_ready = hit_materials != nullptr && rt_scene != nullptr && rt_scene->wants_shadow_retrace() && hit_materials->has_current_records();

		const bool still_waiting = (wait_for_tlas && !tlas_ready) ||
				(wait_for_far_field && !far_field_ready) ||
				(wait_for_swrt && !swrt_ready) ||
				(wait_for_vsm && !vsm_ready) ||
				(wait_for_retrace && !retrace_ready);
		if (still_waiting) {
			st.wait_frames++;
			if (st.wait_frames < 120u) {
				Dictionary waiting;
				waiting["done"] = false;
				waiting["mode"] = mode;
				waiting["wait_frames"] = int(st.wait_frames);
				waiting["tlas_ready"] = tlas_ready;
				waiting["far_field_ready"] = far_field_ready;
				waiting["swrt_ready"] = swrt_ready;
				waiting["vsm_ready"] = vsm_ready;
				waiting["retrace_ready"] = retrace_ready;
				return waiting;
			}
			Dictionary err;
			err["done"] = true;
			err["mode"] = mode;
			err["tlas_ready"] = tlas_ready;
			err["far_field_ready"] = far_field_ready;
			err["swrt_ready"] = swrt_ready;
			err["vsm_ready"] = vsm_ready;
			err["retrace_ready"] = retrace_ready;
			err["error"] = "Timed out waiting for visibility chain prerequisites.";
			return err;
		}

		// Step 1: Generate samples once on the current view so both visibility chain & brute-force share exact samples.
		p_view->reset_guiding();
		_dispatch_generate(p_inputs, p_view, cfg, 0u, false, cfg.falloff_cull_threshold);

		const Vector<uint8_t> sample_bytes = rd->buffer_get_data(p_view->get_sample_buffer());
		const SampleGPU *samples = reinterpret_cast<const SampleGPU *>(sample_bytes.ptr());
		const uint32_t total_samples = uint32_t(sample_bytes.size() / sizeof(SampleGPU));

		// Step 2: Run the MegaLights visibility chain with requested options.
		VisibilityOptions chain_opts;
		chain_opts.override_screen_traces = true;
		chain_opts.screen_traces = use_screen_traces;
		chain_opts.dedupe_enabled = use_dedupe;
		chain_opts.override_far_field = true;
		chain_opts.far_field = use_far_field;
		chain_opts.override_swrt = true;
		chain_opts.swrt = use_swrt;
		chain_opts.force_no_vsm = false;
		chain_opts.use_explicit_frame_index = true;
		chain_opts.frame_index = 0u;
		bool retrace_ran = false;
		_dispatch_visibility_chain(p_inputs, p_view, cfg, chain_opts, &retrace_ran);

		const Vector<uint8_t> vis_bytes = rd->buffer_get_data(p_view->get_visibility_buffer());
		const uint32_t *vis_words = reinterpret_cast<const uint32_t *>(vis_bytes.ptr());
		const Vector<uint8_t> ctr_bytes = rd->buffer_get_data(p_view->get_vis_counters_buffer());
		const uint32_t *ctrs = reinterpret_cast<const uint32_t *>(ctr_bytes.ptr());

		// Also verify hit_distance_texture readback (R16F 2DArray, layer = k) and guiding feed!
		uint64_t finite_hit_dists = 0;
		double hit_dist_sum = 0.0;
		const Size2i sgrid = p_view->get_sample_grid_size();
		for (uint32_t k = 0; k < cfg.samples_per_pixel; k++) {
			const Vector<uint8_t> hd_bytes = rd->texture_get_data(p_view->get_hit_distance_texture(), k);
			const uint16_t *hd_half = reinterpret_cast<const uint16_t *>(hd_bytes.ptr());
			const uint32_t texels = uint32_t(hd_bytes.size() / sizeof(uint16_t));
			for (uint32_t i = 0; i < texels; i++) {
				const float d = Math::half_to_float(hd_half[i]);
				if (Math::is_finite(d) && d < HIT_DISTANCE_MISS * 0.99f && d >= 0.0f) {
					finite_hit_dists++;
					hit_dist_sum += double(d);
				}
			}
		}

		// Verify visibility_buffer feeds cleanly into ml_guiding.glsl.
		_dispatch_guiding(p_view, p_view->get_visibility_buffer(), 0u, false);
		const Vector<uint8_t> g_bytes = rd->buffer_get_data(p_view->get_current_guiding_buffer());
		const uint32_t *g_words = reinterpret_cast<const uint32_t *>(g_bytes.ptr());
		const Size2i tdims = p_view->get_guiding_tile_dims();
		const uint32_t tcount = uint32_t(tdims.x * tdims.y);
		uint32_t guiding_non_empty = 0u;
		uint64_t guiding_vis_sum = 0;
		for (uint32_t t = 0; t < tcount; t++) {
			const uint32_t base = t * GUIDING_TILE_WORDS;
			if (g_words[base + 0u] > 0u) {
				guiding_non_empty++;
			}
			guiding_vis_sum += g_words[base + 1u];
		}

		uint64_t valid_count = 0;
		uint64_t visible_count = 0;
		uint64_t occluded_count = 0;
		uint64_t partial_count = 0;
		for (uint32_t i = 0; i < total_samples; i++) {
			if (samples[i].light_packed == SAMPLE_INVALID) {
				continue;
			}
			valid_count++;
			const float v = unpack_visibility_rgba8(vis_words[i]).r;
			if (v > 0.9f) {
				visible_count++;
			} else if (v < 0.1f) {
				occluded_count++;
			} else {
				partial_count++;
			}
		}

		Dictionary res;
		res["done"] = true;
		res["mode"] = mode;
		res["tlas_ready"] = tlas_ready;
		res["far_field_ready"] = far_field_ready;
		res["swrt_ready"] = swrt_ready;
		res["vsm_ready"] = vsm_ready;
		res["retrace_ran"] = retrace_ran;
		res["sample_grid_width"] = sgrid.x;
		res["sample_grid_height"] = sgrid.y;
		res["samples_per_pixel"] = int(cfg.samples_per_pixel);
		res["valid_samples"] = int64_t(valid_count);
		res["visible_samples"] = int64_t(visible_count);
		res["occluded_samples"] = int64_t(occluded_count);
		res["partial_samples"] = int64_t(partial_count);
		res["ray_count"] = int64_t(ctrs[0]);
		res["vsm_count"] = int64_t(ctrs[1]);
		res["screen_hit_count"] = int64_t(ctrs[2]);
		res["deduped_count"] = int64_t(ctrs[3]);
		res["unshadowed_count"] = int64_t(ctrs[4]);
		res["far_field_count"] = int64_t(ctrs[5]);
		res["retrace_appended_count"] = int64_t(ctrs[6]);
		res["counter_valid_samples"] = int64_t(ctrs[7]);
		res["finite_hit_dist_count"] = int64_t(finite_hit_dists);
		res["mean_hit_dist"] = finite_hit_dists > 0 ? (hit_dist_sum / double(finite_hit_dists)) : 0.0;
		res["guiding_non_empty_tiles"] = int(guiding_non_empty);
		res["guiding_visible_sum"] = int64_t(guiding_vis_sum);

		// Step 3: Optional brute-force trace_shadow() comparison on the EXACT same sample_buffer!
		if (compare_bruteforce && tlas_ready) {
			VisibilityOptions brute_opts;
			brute_opts.override_screen_traces = true;
			brute_opts.screen_traces = false;
			brute_opts.dedupe_enabled = false;
			brute_opts.override_far_field = true;
			brute_opts.far_field = use_far_field;
			brute_opts.override_swrt = true;
			brute_opts.swrt = false;
			brute_opts.force_no_vsm = true;
			brute_opts.use_explicit_frame_index = true;
			brute_opts.frame_index = 0u;
			_dispatch_visibility_chain(p_inputs, p_view, cfg, brute_opts);

			const Vector<uint8_t> brute_vis_bytes = rd->buffer_get_data(p_view->get_visibility_buffer());
			const uint32_t *brute_vis = reinterpret_cast<const uint32_t *>(brute_vis_bytes.ptr());
			const Vector<uint8_t> brute_ctr_bytes = rd->buffer_get_data(p_view->get_vis_counters_buffer());
			const uint32_t *brute_ctrs = reinterpret_cast<const uint32_t *>(brute_ctr_bytes.ptr());

			uint64_t matched = 0;
			uint64_t false_occ = 0;
			uint64_t false_lit = 0;
			uint64_t brute_vis_cnt = 0;
			uint64_t brute_occ_cnt = 0;
			for (uint32_t i = 0; i < total_samples; i++) {
				if (samples[i].light_packed == SAMPLE_INVALID) {
					continue;
				}
				const bool c_vis = unpack_visibility_rgba8(vis_words[i]).r > 0.5f;
				const bool b_vis = unpack_visibility_rgba8(brute_vis[i]).r > 0.5f;
				if (b_vis) {
					brute_vis_cnt++;
				} else {
					brute_occ_cnt++;
				}
				if (c_vis == b_vis) {
					matched++;
				} else if (!c_vis && b_vis) {
					false_occ++;
				} else {
					false_lit++;
				}
			}

			res["brute_ray_count"] = int64_t(brute_ctrs[0]);
			res["brute_visible_samples"] = int64_t(brute_vis_cnt);
			res["brute_occluded_samples"] = int64_t(brute_occ_cnt);
			res["matched_samples"] = int64_t(matched);
			res["false_occlusion_samples"] = int64_t(false_occ);
			res["false_lit_samples"] = int64_t(false_lit);
			res["match_rate"] = valid_count > 0 ? (double(matched) / double(valid_count)) : 1.0;
			res["false_occlusion_rate"] = valid_count > 0 ? (double(false_occ) / double(valid_count)) : 0.0;
			res["false_lit_rate"] = valid_count > 0 ? (double(false_lit) / double(valid_count)) : 0.0;
			res["ray_reduction_fraction"] = brute_ctrs[0] > 0u ? (1.0 - double(ctrs[0]) / double(brute_ctrs[0])) : 0.0;
		}
		return res;
	}

	if (mode == "denoise") {
		const uint32_t total_frames = uint32_t(CLAMP(int(p_options.get("frames", 16)), 1, 64));
		const bool rr_bypass = bool(p_options.get("rr_bypass", cfg.denoiser == DENOISER_RR_BYPASS));
		const bool use_screen_traces = bool(p_options.get("screen_traces", cfg.screen_traces));
		const bool use_dedupe = bool(p_options.get("dedupe", true));
		const bool wait_for_tlas = bool(p_options.get("wait_for_tlas", true));
		const bool compare_reference = bool(p_options.get("compare_reference", true));
		const uint32_t ref_frames = uint32_t(CLAMP(int(p_options.get("ref_frames", 16)), 1, 128));
		const bool test_reset_region = bool(p_options.get("test_reset_region", false));
		const int debug_vis_mode = CLAMP(int(p_options.get("debug_vis_mode", -1)), -1, 2);

		NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;
		const bool tlas_ready = rt_scene != nullptr && rt_scene->is_tlas_traceable() && rt_scene->get_tlas().is_valid() &&
				rt_scene->get_shadow_instance_buffer().is_valid() && rt_scene->get_shadow_alpha_buffer().is_valid();
		if (wait_for_tlas && !tlas_ready) {
			st.wait_frames++;
			if (st.wait_frames < 120u) {
				Dictionary waiting;
				waiting["done"] = false;
				waiting["mode"] = mode;
				waiting["wait_frames"] = int(st.wait_frames);
				waiting["tlas_ready"] = tlas_ready;
				return waiting;
			}
			Dictionary err;
			err["done"] = true;
			err["mode"] = mode;
			err["tlas_ready"] = tlas_ready;
			err["error"] = "Timed out waiting for NextGI TLAS in MegaLights denoise check.";
			return err;
		}

		p_view->reset_guiding();
		p_view->reset_denoise_history();

		Rect2i reset_tile_rect;
		for (uint32_t f = 0; f < total_frames; f++) {
			_dispatch_generate(p_inputs, p_view, cfg, f, f > 0u, cfg.falloff_cull_threshold);

			VisibilityOptions vis_opts;
			vis_opts.override_screen_traces = true;
			vis_opts.screen_traces = use_screen_traces;
			vis_opts.dedupe_enabled = use_dedupe;
			vis_opts.use_explicit_frame_index = true;
			vis_opts.frame_index = f;
			_dispatch_visibility_chain(p_inputs, p_view, cfg, vis_opts);

			_dispatch_guiding(p_view, p_view->get_visibility_buffer(), f, false);
			p_view->advance_guiding_history();

			_dispatch_shade(p_inputs, p_view, cfg, f, false, cfg.falloff_cull_threshold);

			Vector<Rect2i> dirty_regions;
			const bool is_last = (f + 1u == total_frames);
			if (is_last && test_reset_region && !rr_bypass) {
				const int tiles_x = MAX((full_size.x + int(DENOISE_RESET_TILE_SIZE) - 1) / int(DENOISE_RESET_TILE_SIZE), 1);
				const int tiles_y = MAX((full_size.y + int(DENOISE_RESET_TILE_SIZE) - 1) / int(DENOISE_RESET_TILE_SIZE), 1);
				reset_tile_rect = Rect2i(tiles_x / 4, tiles_y / 4, MAX(tiles_x / 2, 1), MAX(tiles_y / 2, 1));
				dirty_regions.push_back(reset_tile_rect);
			}
			_dispatch_denoise(p_inputs, p_view, f, rr_bypass, is_last ? debug_vis_mode : -1, f == 0u, dirty_regions, false, true);
		}

		const RID noisy_diff_rid = p_view->get_noisy_diffuse_texture();
		const RID noisy_spec_rid = p_view->get_noisy_specular_texture();
		const RID den_diff_rid = p_view->get_denoised_diffuse_texture();
		const RID den_spec_rid = p_view->get_denoised_specular_texture();
		if (!noisy_diff_rid.is_valid() || !noisy_spec_rid.is_valid() || !den_diff_rid.is_valid() || !den_spec_rid.is_valid()) {
			Dictionary err;
			err["done"] = true;
			err["mode"] = mode;
			err["error"] = "MegaLights shade/denoise output textures were not initialized.";
			return err;
		}

		const Vector<uint8_t> nd_bytes = rd->texture_get_data(noisy_diff_rid, 0);
		const Vector<uint8_t> ns_bytes = rd->texture_get_data(noisy_spec_rid, 0);
		const Vector<uint8_t> dd_bytes = rd->texture_get_data(den_diff_rid, 0);
		const Vector<uint8_t> ds_bytes = rd->texture_get_data(den_spec_rid, 0);

		const uint16_t *nd_h = reinterpret_cast<const uint16_t *>(nd_bytes.ptr());
		const uint16_t *ns_h = reinterpret_cast<const uint16_t *>(ns_bytes.ptr());
		const uint16_t *dd_h = reinterpret_cast<const uint16_t *>(dd_bytes.ptr());
		const uint16_t *ds_h = reinterpret_cast<const uint16_t *>(ds_bytes.ptr());

		uint64_t valid_px = 0;
		uint64_t nf_nd = 0;
		uint64_t nf_ns = 0;
		uint64_t nf_dd = 0;
		uint64_t nf_ds = 0;
		double sum_nd = 0.0;
		double sum_ns = 0.0;
		double sum_dd = 0.0;
		double sum_ds = 0.0;

		LocalVector<float> noisy_luma;
		LocalVector<float> den_luma;
		LocalVector<uint8_t> valid_mask;
		noisy_luma.resize(uint32_t(pixel_count));
		den_luma.resize(uint32_t(pixel_count));
		valid_mask.resize(uint32_t(pixel_count));

		for (uint64_t i = 0; i < pixel_count; i++) {
			const float nd_r = Math::half_to_float(nd_h[i * 4u + 0u]);
			const float nd_g = Math::half_to_float(nd_h[i * 4u + 1u]);
			const float nd_b = Math::half_to_float(nd_h[i * 4u + 2u]);
			const float ns_r = Math::half_to_float(ns_h[i * 4u + 0u]);
			const float ns_g = Math::half_to_float(ns_h[i * 4u + 1u]);
			const float ns_b = Math::half_to_float(ns_h[i * 4u + 2u]);
			const float ns_a = Math::half_to_float(ns_h[i * 4u + 3u]);

			const float dd_r = Math::half_to_float(dd_h[i * 4u + 0u]);
			const float dd_g = Math::half_to_float(dd_h[i * 4u + 1u]);
			const float dd_b = Math::half_to_float(dd_h[i * 4u + 2u]);
			const float ds_r = Math::half_to_float(ds_h[i * 4u + 0u]);
			const float ds_g = Math::half_to_float(ds_h[i * 4u + 1u]);
			const float ds_b = Math::half_to_float(ds_h[i * 4u + 2u]);

			if (!Math::is_finite(nd_r) || !Math::is_finite(nd_g) || !Math::is_finite(nd_b)) {
				nf_nd++;
			}
			if (!Math::is_finite(ns_r) || !Math::is_finite(ns_g) || !Math::is_finite(ns_b)) {
				nf_ns++;
			}
			if (!Math::is_finite(dd_r) || !Math::is_finite(dd_g) || !Math::is_finite(dd_b)) {
				nf_dd++;
			}
			if (!Math::is_finite(ds_r) || !Math::is_finite(ds_g) || !Math::is_finite(ds_b)) {
				nf_ds++;
			}

			const bool is_valid = Math::is_finite(ns_a) && ns_a > 0.5f;
			valid_mask[uint32_t(i)] = is_valid ? 1u : 0u;
			const float nl_d = ml_cpu_luminance(nd_r, nd_g, nd_b);
			const float nl_s = ml_cpu_luminance(ns_r, ns_g, ns_b);
			const float dl_d = ml_cpu_luminance(dd_r, dd_g, dd_b);
			const float dl_s = ml_cpu_luminance(ds_r, ds_g, ds_b);
			noisy_luma[uint32_t(i)] = nl_d + nl_s;
			den_luma[uint32_t(i)] = dl_d + dl_s;
			if (is_valid) {
				valid_px++;
				sum_nd += double(nl_d);
				sum_ns += double(nl_s);
				sum_dd += double(dl_d);
				sum_ds += double(dl_s);
			}
		}

		double noisy_spatial_std = 0.0;
		double denoised_spatial_std = 0.0;
		uint64_t smooth_interior = 0;

		uint32_t removed_fp = 0u;
		uint32_t total_fp = 0u;
		uint32_t clamped_px = 0u;
		uint32_t clamp_valid_px = 0u;
		double temporal_var_diff_mean = 0.0;
		double temporal_var_spec_mean = 0.0;
		uint32_t max_len_diff = 0u;
		uint32_t max_len_spec = 0u;
		double mean_len_diff = 0.0;
		uint32_t reset_inside_max_len = 0u;
		uint32_t reset_outside_max_len = 0u;
		uint64_t reset_inside_px = 0u;
		uint64_t reset_outside_px = 0u;

		if (!rr_bypass) {
			const Vector<uint8_t> stats_bytes = rd->buffer_get_data(p_view->get_denoise_clamp_stats_buffer());
			if (stats_bytes.size() >= int(4u * sizeof(uint32_t))) {
				const uint32_t *w = reinterpret_cast<const uint32_t *>(stats_bytes.ptr());
				removed_fp = w[0];
				total_fp = w[1];
				clamped_px = w[2];
				clamp_valid_px = w[3];
				p_view->record_denoise_stats(removed_fp, total_fp, clamped_px, clamp_valid_px, false);
			}

			// The temporal pass wrote into cur_slot, and advance_denoise_history() flipped denoise_history_index.
			const uint32_t written_slot = 1u - p_view->get_denoise_history_index();
			const Vector<uint8_t> hl_bytes = rd->texture_get_data(p_view->get_denoise_history_len_slot(written_slot), 0);
			const uint32_t *hl = reinterpret_cast<const uint32_t *>(hl_bytes.ptr());
			const Vector<uint8_t> td_bytes = rd->texture_get_data(p_view->get_denoise_diffuse_slot(written_slot), 0);
			const Vector<uint8_t> ts_bytes = rd->texture_get_data(p_view->get_denoise_specular_slot(written_slot), 0);
			const uint16_t *td_h = reinterpret_cast<const uint16_t *>(td_bytes.ptr());
			const uint16_t *ts_h = reinterpret_cast<const uint16_t *>(ts_bytes.ptr());
			const Vector<uint8_t> gn_bytes = rd->texture_get_data(p_view->get_denoise_prev_depth_normal(), 0);
			const uint16_t *gn_h = reinterpret_cast<const uint16_t *>(gn_bytes.ptr());

			double sum_vd = 0.0;
			double sum_vs = 0.0;
			uint64_t sum_len_d = 0;
			for (int y = 0; y < full_size.y; y++) {
				for (int x = 0; x < full_size.x; x++) {
					const uint64_t i = uint64_t(y) * uint64_t(full_size.x) + uint64_t(x);
					if (!valid_mask[uint32_t(i)]) {
						continue;
					}
					const uint32_t packed = hl[i];
					const uint32_t ld = packed & 0xFFu;
					const uint32_t ls = (packed >> 8u) & 0xFFu;
					max_len_diff = MAX(max_len_diff, ld);
					max_len_spec = MAX(max_len_spec, ls);
					sum_len_d += ld;
					sum_vd += double(MAX(Math::half_to_float(td_h[i * 4u + 3u]), 0.0f));
					sum_vs += double(MAX(Math::half_to_float(ts_h[i * 4u + 3u]), 0.0f));

					if (test_reset_region) {
						const Point2i tile(x / int(DENOISE_RESET_TILE_SIZE), y / int(DENOISE_RESET_TILE_SIZE));
						if (reset_tile_rect.has_point(tile)) {
							reset_inside_max_len = MAX(reset_inside_max_len, ld);
							reset_inside_px++;
						} else {
							reset_outside_max_len = MAX(reset_outside_max_len, ld);
							reset_outside_px++;
						}
					}
				}
			}
			if (valid_px > 0) {
				temporal_var_diff_mean = sum_vd / double(valid_px);
				temporal_var_spec_mean = sum_vs / double(valid_px);
				mean_len_diff = double(sum_len_d) / double(valid_px);
			}

			// Spatial noise residual over smooth interior pixels (4-neighbors on same surface plane/normal).
			double sum_noisy_r2 = 0.0;
			double sum_den_r2 = 0.0;
			auto read_guide = [&](uint64_t p_idx, Vector3 &r_n, float &r_z) {
				r_n = Vector3(Math::half_to_float(gn_h[p_idx * 4u + 0u]), Math::half_to_float(gn_h[p_idx * 4u + 1u]), Math::half_to_float(gn_h[p_idx * 4u + 2u]));
				r_z = Math::half_to_float(gn_h[p_idx * 4u + 3u]);
			};
			for (int y = 1; y < full_size.y - 1; y++) {
				for (int x = 1; x < full_size.x - 1; x++) {
					const uint64_t c = uint64_t(y) * uint64_t(full_size.x) + uint64_t(x);
					const uint64_t nb[4] = { c - 1u, c + 1u, c - uint64_t(full_size.x), c + uint64_t(full_size.x) };
					if (!valid_mask[uint32_t(c)] || !valid_mask[uint32_t(nb[0])] || !valid_mask[uint32_t(nb[1])] || !valid_mask[uint32_t(nb[2])] || !valid_mask[uint32_t(nb[3])]) {
						continue;
					}
					Vector3 nc;
					float zc = 0.0f;
					read_guide(c, nc, zc);
					if (!(zc > 0.0f)) {
						continue;
					}
					bool same_surface = true;
					for (int k = 0; k < 4; k++) {
						Vector3 nq;
						float zq = 0.0f;
						read_guide(nb[k], nq, zq);
						if (!(zq > 0.0f) || Math::abs(zq - zc) > 0.05f * MAX(zc, 0.1f) || nc.dot(nq) < 0.95f) {
							same_surface = false;
							break;
						}
					}
					if (!same_surface) {
						continue;
					}
					const double nr = double(noisy_luma[uint32_t(c)]) - 0.25 * (double(noisy_luma[uint32_t(nb[0])]) + double(noisy_luma[uint32_t(nb[1])]) + double(noisy_luma[uint32_t(nb[2])]) + double(noisy_luma[uint32_t(nb[3])]));
					const double dr = double(den_luma[uint32_t(c)]) - 0.25 * (double(den_luma[uint32_t(nb[0])]) + double(den_luma[uint32_t(nb[1])]) + double(den_luma[uint32_t(nb[2])]) + double(den_luma[uint32_t(nb[3])]));
					sum_noisy_r2 += nr * nr;
					sum_den_r2 += dr * dr;
					smooth_interior++;
				}
			}
			if (smooth_interior > 0) {
				noisy_spatial_std = std::sqrt(sum_noisy_r2 / double(smooth_interior));
				denoised_spatial_std = std::sqrt(sum_den_r2 / double(smooth_interior));
			}
		}

		uint64_t debug_non_black = 0;
		uint64_t debug_non_finite = 0;
		double debug_mean_luma = 0.0;
		if (debug_vis_mode >= 0 && p_view->get_denoise_debug_texture().is_valid()) {
			const Vector<uint8_t> dbg_bytes = rd->texture_get_data(p_view->get_denoise_debug_texture(), 0);
			const uint16_t *dbg_h = reinterpret_cast<const uint16_t *>(dbg_bytes.ptr());
			double dbg_sum = 0.0;
			for (uint64_t i = 0; i < pixel_count; i++) {
				const float r = Math::half_to_float(dbg_h[i * 4u + 0u]);
				const float g = Math::half_to_float(dbg_h[i * 4u + 1u]);
				const float b = Math::half_to_float(dbg_h[i * 4u + 2u]);
				if (!Math::is_finite(r) || !Math::is_finite(g) || !Math::is_finite(b)) {
					debug_non_finite++;
					continue;
				}
				const float l = ml_cpu_luminance(r, g, b);
				if (l > 1e-4f) {
					debug_non_black++;
				}
				dbg_sum += double(l);
			}
			debug_mean_luma = pixel_count > 0 ? (dbg_sum / double(pixel_count)) : 0.0;
		}

		double ref_mean_luma = 0.0;
		double physical_shaded_mean_luma = (clamp_valid_px > 0u) ? (double(total_fp) / (16.0 * double(clamp_valid_px))) : 0.0;
		double ref_rel_err = 0.0;
		if (compare_reference) {
			p_view->reset_reference();
			_upload_light_shadow_policies(p_inputs, p_view, true);
			for (uint32_t rf = 0; rf < ref_frames; rf++) {
				_dispatch_reference_rt(p_inputs, p_view, cfg, rf, rf + 1u, !tlas_ready, cfg.falloff_cull_threshold);
			}
			const Vector<uint8_t> ref_bytes = rd->texture_get_data(p_view->get_reference_accum_texture(), 0);
			const float *ref_px = reinterpret_cast<const float *>(ref_bytes.ptr());
			double ref_sum = 0.0;
			uint64_t ref_valid = 0;
			for (uint64_t i = 0; i < pixel_count; i++) {
				if (!valid_mask[uint32_t(i)]) {
					continue;
				}
				ref_sum += double(ml_cpu_luminance(ref_px[i * 4u + 0u], ref_px[i * 4u + 1u], ref_px[i * 4u + 2u]));
				ref_valid++;
			}
			ref_mean_luma = ref_valid > 0 ? (ref_sum / double(ref_valid)) : 0.0;
			if (ref_mean_luma > 1e-4 && physical_shaded_mean_luma > 0.0) {
				ref_rel_err = Math::abs(physical_shaded_mean_luma - ref_mean_luma) / ref_mean_luma;
			}
		}

		const bool rr_bypass_identical = rr_bypass &&
				den_diff_rid == noisy_diff_rid &&
				den_spec_rid == noisy_spec_rid &&
				!p_view->get_denoise_diffuse_slot(0).is_valid() &&
				!p_view->get_denoise_specular_slot(0).is_valid();

		Dictionary res;
		res["done"] = true;
		res["mode"] = mode;
		res["tlas_ready"] = tlas_ready;
		res["frames"] = int(total_frames);
		res["samples_per_pixel"] = int(cfg.samples_per_pixel);
		res["downsample"] = cfg.downsample == DOWNSAMPLE_FULL ? "full" : "half";
		res["rr_bypass"] = rr_bypass;
		res["rr_bypass_identical"] = rr_bypass_identical;
		res["valid_pixels"] = int64_t(valid_px);
		res["non_finite_noisy_diff"] = int64_t(nf_nd);
		res["non_finite_noisy_spec"] = int64_t(nf_ns);
		res["non_finite_denoised_diff"] = int64_t(nf_dd);
		res["non_finite_denoised_spec"] = int64_t(nf_ds);
		res["noisy_diff_mean"] = valid_px > 0 ? (sum_nd / double(valid_px)) : 0.0;
		res["noisy_spec_mean"] = valid_px > 0 ? (sum_ns / double(valid_px)) : 0.0;
		res["denoised_diff_mean"] = valid_px > 0 ? (sum_dd / double(valid_px)) : 0.0;
		res["denoised_spec_mean"] = valid_px > 0 ? (sum_ds / double(valid_px)) : 0.0;
		res["smooth_interior_pixels"] = int64_t(smooth_interior);
		res["noisy_spatial_std"] = noisy_spatial_std;
		res["denoised_spatial_std"] = denoised_spatial_std;
		res["temporal_var_diff_mean"] = temporal_var_diff_mean;
		res["temporal_var_spec_mean"] = temporal_var_spec_mean;
		res["max_history_len_diff"] = int(max_len_diff);
		res["max_history_len_spec"] = int(max_len_spec);
		res["mean_history_len_diff"] = mean_len_diff;
		res["clamped_energy_removed_fp"] = int64_t(removed_fp);
		res["total_direct_energy_fp"] = int64_t(total_fp);
		res["clamped_energy_ratio"] = compute_clamped_energy_ratio(removed_fp, total_fp);
		res["clamped_pixels"] = int64_t(clamped_px);
		res["clamp_valid_pixels"] = int64_t(clamp_valid_px);
		res["reset_region_tested"] = test_reset_region;
		res["reset_inside_max_len"] = int(reset_inside_max_len);
		res["reset_outside_max_len"] = int(reset_outside_max_len);
		res["reset_inside_pixels"] = int64_t(reset_inside_px);
		res["reset_outside_pixels"] = int64_t(reset_outside_px);
		res["debug_vis_mode"] = debug_vis_mode;
		res["debug_non_black_pixels"] = int64_t(debug_non_black);
		res["debug_non_finite_count"] = int64_t(debug_non_finite);
		res["debug_mean_luma"] = debug_mean_luma;
		res["ref_mean_luma"] = ref_mean_luma;
		res["physical_shaded_mean_luma"] = physical_shaded_mean_luma;
		res["ref_rel_err"] = ref_rel_err;
		return res;
	}

	if (mode == "falloff_cull") {
		const float low_thresh = float(p_options.get("low_threshold", 0.0));
		const float high_thresh = float(p_options.get("high_threshold", 0.2));

		auto count_valid_samples = [&](float p_thresh) -> int64_t {
			_dispatch_generate(p_inputs, p_view, cfg, 0u, false, p_thresh);
			const Vector<uint8_t> bytes = rd->buffer_get_data(p_view->get_sample_buffer());
			const SampleGPU *samples = reinterpret_cast<const SampleGPU *>(bytes.ptr());
			const uint32_t count = uint32_t(bytes.size() / sizeof(SampleGPU));
			int64_t valid = 0;
			for (uint32_t i = 0; i < count; i++) {
				if (samples[i].light_packed != SAMPLE_INVALID) {
					valid++;
				}
			}
			return valid;
		};

		const int64_t valid_low = count_valid_samples(low_thresh);
		const int64_t valid_high = count_valid_samples(high_thresh);

		Dictionary res;
		res["done"] = true;
		res["mode"] = mode;
		res["low_threshold"] = double(low_thresh);
		res["high_threshold"] = double(high_thresh);
		res["valid_samples_low"] = valid_low;
		res["valid_samples_high"] = valid_high;
		return res;
	}

	// "light_eval": exhaustive unshadowed evaluation summary across all light types.
	_dispatch_reference_unshadowed(p_inputs, p_view, cfg, 0u, 1u, cfg.falloff_cull_threshold);
	const Vector<uint8_t> bytes = rd->texture_get_data(p_view->get_reference_accum_texture(), 0);
	const float *px = reinterpret_cast<const float *>(bytes.ptr());
	double sum_r = 0.0;
	double sum_g = 0.0;
	double sum_b = 0.0;
	double max_luma = 0.0;
	uint32_t lit = 0u;
	for (uint64_t i = 0; i < pixel_count; i++) {
		const float r = px[i * 4u + 0u];
		const float g = px[i * 4u + 1u];
		const float b = px[i * 4u + 2u];
		const float l = ml_cpu_luminance(r, g, b);
		if (l > 1e-5f) {
			lit++;
		}
		sum_r += double(r);
		sum_g += double(g);
		sum_b += double(b);
		max_luma = MAX(max_luma, double(l));
	}
	const double inv_px = pixel_count > 0 ? (1.0 / double(pixel_count)) : 0.0;
	LightStorage *light_storage = LightStorage::get_singleton();
	Dictionary res;
	res["done"] = true;
	res["mode"] = mode;
	res["lit_pixels"] = int(lit);
	res["mean_r"] = sum_r * inv_px;
	res["mean_g"] = sum_g * inv_px;
	res["mean_b"] = sum_b * inv_px;
	res["mean_luma"] = ml_cpu_luminance(float(sum_r * inv_px), float(sum_g * inv_px), float(sum_b * inv_px));
	res["max_luma"] = max_luma;
	res["omni_count"] = int(light_storage->get_omni_light_count());
	res["spot_count"] = int(light_storage->get_spot_light_count());
	res["area_count"] = int(light_storage->get_area_light_count());
	res["directional_count"] = int(p_inputs.render_data->directional_light_count);
	return res;
}

void MegaLights::render(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	render_sampling(p_inputs, p_rb);
}

void MegaLights::render_sampling(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null()) {
		return;
	}
	if (!_ensure_common_shaders()) {
		return;
	}

	bool run_check = false;
	Dictionary check_opts;
	{
		MegaLightsCheckState &st = ml_check_state();
		MutexLock lock(st.mutex);
		if (st.active) {
			run_check = true;
			check_opts = st.options;
		}
	}

	Ref<MegaLightsViewData> view = ensure_view_data(p_rb.ptr());
	if (view.is_null()) {
		return;
	}

	if (run_check) {
		Dictionary out = _run_check(p_inputs, view, check_opts);
		MegaLightsCheckState &st = ml_check_state();
		MutexLock lock(st.mutex);
		st.result = out;
		if (bool(out.get("done", false))) {
			st.active = false;
			st.initialized = false;
		}
		return;
	}

	const Settings settings = read_settings();
	if (settings.reference_mode) {
		render_reference(p_inputs, p_rb, false);
		return;
	}

	generate_samples(p_inputs, p_rb);
	trace_visibility(p_inputs, p_rb);
	update_guiding(p_rb, view->get_visibility_buffer(), false);
	shade_samples(p_inputs, p_rb);
	denoise_samples(p_inputs, p_rb);
	view->advance_frame_index();
}

MegaLights::FroxelResult MegaLights::render_froxel(const FroxelInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb) {
	FroxelResult result;
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null() || (!p_inputs.fog_active && !p_inputs.translucency_active)) {
		if (p_rb.is_valid()) {
			Ref<MegaLightsViewData> existing = get_view_data(p_rb.ptr());
			if (existing.is_valid()) {
				existing->free_froxel_buffers();
			}
			if (p_inputs.next_gi != nullptr) {
				p_inputs.next_gi->free_megalights_translucency_direct(p_rb.ptr());
			}
		}
		ml_last_froxel_dispatch_mode.set(0u);
		ml_last_froxel_samples_per_cell.set(0u);
		ml_last_froxel_dispatched_cells.set(0u);
		return result;
	}

	if (!_ensure_common_shaders() || !_ensure_froxel_compute_shaders()) {
		return result;
	}
	_ensure_stbn_texture();

	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, result);
	ERR_FAIL_NULL_V(usc, result);
	ERR_FAIL_NULL_V(light_storage, result);
	ERR_FAIL_NULL_V(texture_storage, result);
	ERR_FAIL_NULL_V(material_storage, result);

	Ref<MegaLightsViewData> view = ensure_view_data(p_rb.ptr());
	if (view.is_null()) {
		return result;
	}

	const Size2i full_size = p_rb->get_internal_size();
	NextGIWRC *wrc = nullptr;
	bool d17_active = false;
	if (p_inputs.translucency_active && p_inputs.next_gi != nullptr) {
		wrc = p_inputs.next_gi->ensure_megalights_translucency_direct(p_rb.ptr(), full_size);
		d17_active = (wrc != nullptr && wrc->has_translucency_direct_volume());
	} else if (p_inputs.next_gi != nullptr) {
		p_inputs.next_gi->free_megalights_translucency_direct(p_rb.ptr());
	}

	const bool fog_active = p_inputs.fog_active && p_inputs.fog_size.x > 0 && p_inputs.fog_size.y > 0 && p_inputs.fog_size.z > 0;
	if (!fog_active && !d17_active) {
		view->free_froxel_buffers();
		ml_last_froxel_dispatch_mode.set(0u);
		ml_last_froxel_samples_per_cell.set(0u);
		ml_last_froxel_dispatched_cells.set(0u);
		return result;
	}

	const bool shared_fog_d17 = fog_active && d17_active;
	if (!view->ensure_froxel_buffers(fog_active, p_inputs.fog_size, shared_fog_d17)) {
		return result;
	}

	// Ensure per-light shadow policy buffer includes all local + directional lights for froxel visibility.
	FrameInputs policy_inputs;
	policy_inputs.render_data = p_inputs.render_data;
	policy_inputs.next_gi = p_inputs.next_gi;
	_upload_light_shadow_policies(policy_inputs, view, true, p_inputs.vsm_sample_view);

	const RenderDataRD *rd_data = p_inputs.render_data;
	const RenderSceneDataRD &sd = *rd_data->scene_data;
	const Settings settings = read_settings();
	const NextGIWRC::ViewParams wrc_view = NextGIWRC::view_params(sd);

	const bool cam_cut = p_inputs.reset_temporal_history || !view->has_froxel_prev_cam() || is_camera_cut(view->get_froxel_prev_cam(), sd.cam_transform);
	const bool history_valid = !cam_cut && (fog_active ? view->is_froxel_history_valid() : wrc->is_translucency_direct_history_valid());
	const uint32_t hist_frames = history_valid ? view->get_froxel_history_frames() : 0u;
	const float history_blend = history_valid ? MIN(float(hist_frames) / float(hist_frames + 1u), 0.88f) : 0.0f;

	const Vector3i d17_size = d17_active ? wrc->get_translucency_direct_size() : Vector3i(1, 1, D17_VOLUME_SLICES);
	const Vector3i dispatch_size = fog_active ? p_inputs.fog_size : d17_size;
	const uint32_t spp = froxel_samples_for_quality_preset(p_inputs.quality_level);

	// Compute fog frustum extents matching Fog::volumetric_fog_update() exactly.
	const Vector2 frustum_near_size = sd.cam_projection.get_viewport_half_extents();
	const Vector2 frustum_far_size = sd.cam_projection.get_far_plane_half_extents();
	const float z_near = sd.cam_projection.get_z_near();
	const float z_far = sd.cam_projection.get_z_far();
	const float fog_end = MAX(p_inputs.fog_end, 0.1f);
	const Vector2 fog_far_size = frustum_near_size.lerp(frustum_far_size, (fog_end - z_near) / MAX(z_far - z_near, 1e-4f));
	const Vector2 fog_near_size = sd.cam_projection.is_orthogonal() ? fog_far_size : frustum_near_size.maxf(0.001f);

	const Transform3D to_prev_view = view->has_froxel_prev_cam() ? (view->get_froxel_prev_cam().affine_inverse() * sd.cam_transform) : Transform3D();
	const Projection prev_vp = view->has_froxel_prev_cam() ? view->get_froxel_prev_view_proj() : wrc_view.view_proj;

	// VSM coarse-page bindings (defaults when VSM is off).
	VirtualShadowMapStorage *vsm = VirtualShadowMapStorage::get_singleton();
	const bool vsm_ready = p_inputs.vsm_sample_view && vsm != nullptr && vsm->is_active() &&
			vsm->get_table_capacity() > 0u && vsm->get_map_buffers_capacity() == vsm->get_table_capacity() &&
			vsm->get_pool_texture().is_valid() && vsm->get_page_table_buffer().is_valid() &&
			vsm->get_map_data_buffer().is_valid() && vsm->get_coarse_miss_buffer().is_valid();

	const bool use_lf = p_inputs.light_function_atlas != nullptr && p_inputs.light_function_atlas->has_baked_atlas() && p_inputs.light_function_atlas->get_view_entry_count() > 0u;
	const bool use_packed_grid = (rd_data->packed_light_grid_flags & 1u) != 0u && rd_data->packed_light_grid_headers_buffer.is_valid() && rd_data->packed_light_grid_index_buffer.is_valid();

	uint32_t flags = 0u;
	if (fog_active) {
		flags |= FROXEL_FLAG_FOG_ACTIVE;
	} else {
		flags |= FROXEL_FLAG_D17_DIRECT_WRITE;
	}
	if (d17_active) {
		flags |= FROXEL_FLAG_WRITE_SH;
	}
	if (history_valid) {
		flags |= FROXEL_FLAG_HISTORY_VALID;
	}
	if (use_lf) {
		flags |= FROXEL_FLAG_USE_LIGHT_FUNCTION;
	}
	if (vsm_ready) {
		flags |= FROXEL_FLAG_USE_VSM;
	}
	if (settings.directional_lights) {
		flags |= FROXEL_FLAG_DIRECTIONAL_LIGHTS;
	}
	if (sd.cam_orthogonal) {
		flags |= FROXEL_FLAG_ORTHOGONAL;
	}

	FroxelParamsUBO ubo;
	MaterialStorage::store_transform_transposed_3x4(sd.cam_transform, ubo.inv_view_rows);
	MaterialStorage::store_transform_transposed_3x4(to_prev_view, ubo.to_prev_view_rows);
	MaterialStorage::store_camera(wrc_view.inv_view_proj, ubo.inv_view_proj);
	MaterialStorage::store_camera(prev_vp, ubo.prev_view_proj);
	ubo.depth_coeffs[0] = float(wrc_view.depth_coefficients.x);
	ubo.depth_coeffs[1] = float(wrc_view.depth_coefficients.y);
	ubo.depth_coeffs[2] = float(wrc_view.depth_coefficients.z);
	ubo.depth_coeffs[3] = float(wrc_view.depth_coefficients.w);
	ubo.fog_frustum_size[0] = fog_near_size.x;
	ubo.fog_frustum_size[1] = fog_near_size.y;
	ubo.fog_frustum_size[2] = fog_far_size.x;
	ubo.fog_frustum_size[3] = fog_far_size.y;
	ubo.fog_z_params[0] = fog_end;
	ubo.fog_z_params[1] = MAX(p_inputs.fog_detail_spread, 0.1f);
	ubo.fog_z_params[2] = CLAMP(p_inputs.fog_phase_g, -0.95f, 0.95f);
	ubo.fog_z_params[3] = 10.0f; // INV_FOG_FADE matching volumetric_fog_process.glsl
	ubo.d17_params[0] = D17_VOLUME_NEAR;
	ubo.d17_params[1] = D17_VOLUME_FAR;
	ubo.d17_params[2] = history_blend;
	ubo.d17_params[3] = settings.falloff_cull_threshold;
	ubo.grid_dims[0] = uint32_t(dispatch_size.x);
	ubo.grid_dims[1] = uint32_t(dispatch_size.y);
	ubo.grid_dims[2] = uint32_t(dispatch_size.z);
	ubo.grid_dims[3] = flags;
	ubo.d17_dims[0] = uint32_t(d17_size.x);
	ubo.d17_dims[1] = uint32_t(d17_size.y);
	ubo.d17_dims[2] = uint32_t(d17_size.z);
	ubo.d17_dims[3] = spp;
	ubo.fog_dims_pad[0] = uint32_t(MAX(p_inputs.fog_size.x, 1));
	ubo.fog_dims_pad[1] = uint32_t(MAX(p_inputs.fog_size.y, 1));
	ubo.fog_dims_pad[2] = uint32_t(MAX(p_inputs.fog_size.z, 1));
	ubo.fog_dims_pad[3] = use_packed_grid ? 1u : 0u;
	ubo.light_counts[0] = light_storage->get_omni_light_count();
	ubo.light_counts[1] = light_storage->get_spot_light_count();
	ubo.light_counts[2] = light_storage->get_area_light_count();
	ubo.light_counts[3] = rd_data->directional_light_count;
	ubo.light_grid_dims[0] = uint32_t(MAX(rd_data->packed_light_grid_dims.x, 1));
	ubo.light_grid_dims[1] = uint32_t(MAX(rd_data->packed_light_grid_dims.y, 1));
	ubo.light_grid_dims[2] = uint32_t(MAX(rd_data->packed_light_grid_dims.z, 1));
	ubo.light_grid_dims[3] = rd_data->packed_light_grid_flags;
	ubo.light_grid_z_params[0] = rd_data->packed_light_grid_z_near;
	ubo.light_grid_z_params[1] = rd_data->packed_light_grid_z_far;
	ubo.light_grid_z_params[2] = rd_data->packed_light_grid_z_scale;
	ubo.light_grid_z_params[3] = rd_data->packed_light_grid_z_bias;
	ubo.screen_frame_vsm[0] = uint32_t(MAX(full_size.x, 1));
	ubo.screen_frame_vsm[1] = uint32_t(MAX(full_size.y, 1));
	ubo.screen_frame_vsm[2] = view->get_frame_index();
	ubo.screen_frame_vsm[3] = vsm_ready ? vsm->get_table_capacity() : 0u;

	rd->buffer_update(view->get_froxel_params_ubo(), 0, sizeof(ubo), &ubo);

	const RID default_white = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	const RID default_black_3d = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK);
	const RID default_array_white = texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
	const RID sampler_nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID sampler_linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID sampler_linear_mips = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RID ies_atlas = light_storage->get_ies_atlas_texture();
	if (!ies_atlas.is_valid()) {
		ies_atlas = default_array_white;
	}
	RID area_atlas = texture_storage->area_light_atlas_get_texture();
	if (!area_atlas.is_valid()) {
		area_atlas = default_white;
	}
	RID lf_atlas = p_inputs.light_function_atlas != nullptr ? p_inputs.light_function_atlas->get_atlas_texture_or_default() : default_white;
	RID lf_entries = p_inputs.light_function_atlas != nullptr ? p_inputs.light_function_atlas->get_view_entry_buffer_or_default() : gpu->dummy_storage_buffer;

	const RID lg_headers = use_packed_grid ? rd_data->packed_light_grid_headers_buffer : gpu->dummy_storage_buffer;
	const RID lg_indices = use_packed_grid ? rd_data->packed_light_grid_index_buffer : gpu->dummy_storage_buffer;
	const RID vsm_pool = vsm_ready ? vsm->get_pool_texture() : p_inputs.vsm_default_pool;
	const RID vsm_pt = vsm_ready ? vsm->get_page_table_buffer() : (p_inputs.vsm_default_buffer.is_valid() ? p_inputs.vsm_default_buffer : gpu->dummy_storage_buffer);
	const RID vsm_maps_buf = vsm_ready ? vsm->get_map_data_buffer() : (p_inputs.vsm_default_buffer.is_valid() ? p_inputs.vsm_default_buffer : gpu->dummy_storage_buffer);
	const RID vsm_miss = vsm_ready ? vsm->get_coarse_miss_buffer() : (p_inputs.vsm_default_miss_buffer.is_valid() ? p_inputs.vsm_default_miss_buffer : gpu->dummy_storage_buffer);

	// Select ping-pong input/output 3D targets (zero write aliasing via dummy_3d_storage_tex when unused).
	const RID fog_prev = fog_active ? view->get_froxel_fog_texture() : default_black_3d;
	const RID fog_out = fog_active ? view->get_froxel_fog_write_texture() : gpu->dummy_3d_storage_tex;
	RID sh_r_prev = default_black_3d;
	RID sh_g_prev = default_black_3d;
	RID sh_b_prev = default_black_3d;
	RID sh_r_out = gpu->dummy_3d_storage_tex;
	RID sh_g_out = gpu->dummy_3d_storage_tex;
	RID sh_b_out = gpu->dummy_3d_storage_tex;
	if (shared_fog_d17) {
		sh_r_prev = view->get_froxel_sh_r();
		sh_g_prev = view->get_froxel_sh_g();
		sh_b_prev = view->get_froxel_sh_b();
		sh_r_out = view->get_froxel_sh_write_r();
		sh_g_out = view->get_froxel_sh_write_g();
		sh_b_out = view->get_froxel_sh_write_b();
	} else if (d17_active) {
		sh_r_prev = wrc->get_translucency_direct_r();
		sh_g_prev = wrc->get_translucency_direct_g();
		sh_b_prev = wrc->get_translucency_direct_b();
		sh_r_out = wrc->get_translucency_direct_write_r();
		sh_g_out = wrc->get_translucency_direct_write_g();
		sh_b_out = wrc->get_translucency_direct_write_b();
	}

	LocalVector<RD::Uniform> u;
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, view->get_froxel_params_ubo()));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, light_storage->get_omni_light_buffer()));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, light_storage->get_spot_light_buffer()));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, light_storage->get_area_light_buffer()));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, light_storage->get_directional_light_buffer()));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, lg_headers));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, lg_indices));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, gpu->stbn_texture));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 8, ies_atlas));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 9, lf_atlas));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, lf_entries));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, area_atlas));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 12, sampler_nearest));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 13, sampler_linear));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 14, sampler_linear_mips));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, view->get_light_shadow_policy_buffer()));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 16, vsm_pool));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, vsm_pt));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, vsm_maps_buf));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, vsm_miss));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 20, fog_prev));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 21, sh_r_prev));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 22, sh_g_prev));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 23, sh_b_prev));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 24, fog_out));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 25, sh_r_out));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 26, sh_g_out));
	u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 27, sh_b_out));

	NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;
	const bool hwrt_ready = rt_scene != nullptr && rt_scene->is_tlas_traceable() && rt_scene->get_tlas().is_valid() &&
			rt_scene->get_shadow_instance_buffer().is_valid() && rt_scene->get_shadow_alpha_buffer().is_valid() &&
			_ensure_froxel_rt_shader(p_inputs.next_gi);

	rd->draw_command_begin_label("MegaLights Shared Froxel Volume");

	// Pass 1: Single shared stochastic sample & visibility pass over the active froxel grid.
	if (hwrt_ready) {
		const bool use_alpha = rt_scene->wants_shadow_alpha_variant() && rd->has_feature(RD::SUPPORTS_BINDLESS) && rt_scene->ensure_shadow_texture_table().is_valid();
		const int variant = use_alpha ? 1 : 0;
		const RID shader_rid = gpu->froxel_rt_shader.version_get_shader(gpu->froxel_rt_version, variant);
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 28, rt_scene->get_tlas()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, rt_scene->get_shadow_instance_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 30, rt_scene->get_shadow_alpha_buffer()));
		if (use_alpha) {
			LocalVector<RD::Uniform> alpha_uniforms;
			rt_scene->append_shadow_alpha_uniforms(31, 35, alpha_uniforms);
			for (uint32_t i = 0; i < alpha_uniforms.size(); i++) {
				u.push_back(alpha_uniforms[i]);
			}
			const RID table = rt_scene->ensure_shadow_texture_table();
			if (gpu->froxel_shadow_texture_set.is_null() || gpu->froxel_shadow_texture_set_table != table || !rd->uniform_set_is_valid(gpu->froxel_shadow_texture_set)) {
				if (gpu->froxel_shadow_texture_set.is_valid() && rd->uniform_set_is_valid(gpu->froxel_shadow_texture_set)) {
					rd->free_rid(gpu->froxel_shadow_texture_set);
				}
				RD::Uniform table_uniform(RD::UNIFORM_TYPE_BINDLESS_TABLE, 0, table);
				gpu->froxel_shadow_texture_set = rd->uniform_set_create(table_uniform, shader_rid, 1);
				gpu->froxel_shadow_texture_set_table = table;
			}
		}
		const RID set0 = usc->get_cache_vec(shader_rid, 0, u);
		if (set0.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->froxel_rt_pipelines[variant]);
			rd->compute_list_bind_uniform_set(cl, set0, 0);
			if (use_alpha && gpu->froxel_shadow_texture_set.is_valid()) {
				rd->compute_list_bind_uniform_set(cl, gpu->froxel_shadow_texture_set, 1);
			}
			rd->compute_list_dispatch(cl, (uint32_t(dispatch_size.x) + 3u) / 4u, (uint32_t(dispatch_size.y) + 3u) / 4u, (uint32_t(dispatch_size.z) + 3u) / 4u);
			rd->compute_list_end();
		}
	} else {
		const RID shader_rid = gpu->froxel_compute_shader.version_get_shader(gpu->froxel_compute_version, GPUState::FROXEL_COMPUTE_SAMPLE_NO_RT);
		const RID set0 = usc->get_cache_vec(shader_rid, 0, u);
		if (set0.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->froxel_compute_pipelines[GPUState::FROXEL_COMPUTE_SAMPLE_NO_RT]);
			rd->compute_list_bind_uniform_set(cl, set0, 0);
			rd->compute_list_dispatch(cl, (uint32_t(dispatch_size.x) + 3u) / 4u, (uint32_t(dispatch_size.y) + 3u) / 4u, (uint32_t(dispatch_size.z) + 3u) / 4u);
			rd->compute_list_end();
		}
	}

	// Pass 2 (only when BOTH fog and D17 are active): zero-ray trilinear resample from the shared
	// fog-grid SH-L1 volume into the D17 translucency direct-light SH-L1 channel.
	if (shared_fog_d17) {
		const RID resample_shader = gpu->froxel_compute_shader.version_get_shader(gpu->froxel_compute_version, GPUState::FROXEL_COMPUTE_RESAMPLE_D17);
		LocalVector<RD::Uniform> ru;
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, view->get_froxel_params_ubo()));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, view->get_froxel_sh_write_r()));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, view->get_froxel_sh_write_g()));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, view->get_froxel_sh_write_b()));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 4, sampler_linear));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, wrc->get_translucency_direct_write_r()));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, wrc->get_translucency_direct_write_g()));
		ru.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, wrc->get_translucency_direct_write_b()));
		const RID rset = usc->get_cache_vec(resample_shader, 0, ru);
		if (rset.is_valid()) {
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->froxel_compute_pipelines[GPUState::FROXEL_COMPUTE_RESAMPLE_D17]);
			rd->compute_list_bind_uniform_set(cl, rset, 0);
			rd->compute_list_dispatch(cl, (uint32_t(d17_size.x) + 3u) / 4u, (uint32_t(d17_size.y) + 3u) / 4u, (uint32_t(d17_size.z) + 3u) / 4u);
			rd->compute_list_end();
		}
	}

	rd->draw_command_end_label();

	view->advance_froxel_history(sd.cam_transform, wrc_view.view_proj, cam_cut);
	if (d17_active) {
		wrc->advance_translucency_direct_slot(true);
		result.d17_direct_r = wrc->get_translucency_direct_r();
		result.d17_direct_g = wrc->get_translucency_direct_g();
		result.d17_direct_b = wrc->get_translucency_direct_b();
		result.d17_active = true;
	}
	if (fog_active) {
		result.fog_light_texture = view->get_froxel_fog_texture();
		result.fog_active = result.fog_light_texture.is_valid();
	}

	ml_last_froxel_dispatch_mode.set(shared_fog_d17 ? 1u : (fog_active ? 2u : 3u));
	ml_last_froxel_samples_per_cell.set(spp);
	ml_last_froxel_dispatched_cells.set(uint32_t(dispatch_size.x) * uint32_t(dispatch_size.y) * uint32_t(dispatch_size.z));
	return result;
}

MegaLights::FrontLayerResult MegaLights::render_front_layer(const FrameInputs &p_inputs, RID p_front_layer_depth, RID p_front_layer_normal_roughness, Ref<RenderSceneBuffersRD> p_rb) {
	FrontLayerResult result;
	if (p_inputs.render_data == nullptr || p_inputs.render_data->scene_data == nullptr || p_rb.is_null() ||
			!p_front_layer_depth.is_valid() || !p_front_layer_normal_roughness.is_valid()) {
		return result;
	}
	if (!_ensure_common_shaders() || !_ensure_front_layer_shader()) {
		return result;
	}

	RD *rd = RD::get_singleton();
	UniformSetCacheRD *usc = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, result);
	ERR_FAIL_NULL_V(usc, result);
	ERR_FAIL_NULL_V(material_storage, result);

	const Size2i full_size = p_rb->get_internal_size();
	Ref<MegaLightsViewData> fl_view = ensure_front_layer_view_data(p_rb.ptr());
	if (fl_view.is_null() || !fl_view->ensure_front_layer_guide_textures(full_size)) {
		return result;
	}

	rd->draw_command_begin_label("MegaLights Front-Layer Translucency");

	// Step 1: Synthesize neutral demodulated M1 albedo + material guides from front-layer depth/normal.
	{
		const RID sampler_nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		const RID shader_rid = gpu->front_layer_shader.version_get_shader(gpu->front_layer_version, 0);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, p_front_layer_depth));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_front_layer_normal_roughness));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 2, sampler_nearest));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, fl_view->get_front_layer_albedo_texture()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, fl_view->get_front_layer_material_texture()));
		const RID uset = usc->get_cache_vec(shader_rid, 0, u);
		if (uset.is_valid()) {
			FrontLayerPushConstant pc;
			pc.size[0] = uint32_t(full_size.x);
			pc.size[1] = uint32_t(full_size.y);
			RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, gpu->front_layer_pipeline);
			rd->compute_list_bind_uniform_set(cl, uset, 0);
			rd->compute_list_set_push_constant(cl, &pc, sizeof(pc));
			rd->compute_list_dispatch(cl, (uint32_t(full_size.x) + 7u) / 8u, (uint32_t(full_size.y) + 7u) / 8u, 1u);
			rd->compute_list_end();
		}
	}

	// Step 2: Reuse the opaque sampling, visibility, guiding, shading, and denoise kernels with
	// a completely separate front-layer view history (and screen_traces disabled since Hi-Z is opaque).
	FrameInputs fl_inputs = p_inputs;
	fl_inputs.depth_texture = p_front_layer_depth;
	fl_inputs.normal_roughness_texture = p_front_layer_normal_roughness;
	fl_inputs.ml_albedo_texture = fl_view->get_front_layer_albedo_texture();
	fl_inputs.ml_material_texture = fl_view->get_front_layer_material_texture();
	fl_inputs.hiz_texture = RID();
	fl_inputs.hiz_mip_count = 1u;
	fl_inputs.velocity_enabled = false;
	fl_inputs.velocity_texture = RID();
	fl_inputs.debug_draw = RSE::VIEWPORT_DEBUG_DRAW_DISABLED;

	const Settings settings = read_settings();
	if (!fl_view->ensure_sampling_buffers(full_size, settings.samples_per_pixel, uint32_t(settings.downsample))) {
		rd->draw_command_end_label();
		return result;
	}
	_upload_layer_group_masks(fl_view);
	_dispatch_generate(fl_inputs, fl_view, settings, fl_view->get_frame_index(), fl_view->has_guiding_history(), -1.0f);

	VisibilityOptions vis_opts;
	vis_opts.override_screen_traces = true;
	vis_opts.screen_traces = false;
	_dispatch_visibility_chain(fl_inputs, fl_view, settings, vis_opts);

	_dispatch_guiding(fl_view, fl_view->get_visibility_buffer(), fl_view->get_frame_index(), false);
	fl_view->advance_guiding_history();

	_dispatch_shade(fl_inputs, fl_view, settings, fl_view->get_frame_index(), false, -1.0f);

	// GodotNext: pu_dlss_rr (#206). Honour vendor-neutral DenoiserBypass contract for MegaLights.
	const bool rr_bypass = (settings.denoiser == DENOISER_RR_BYPASS) || DenoiserBypass::is_bypassed(p_rb.ptr());
	_dispatch_denoise(fl_inputs, fl_view, fl_view->get_frame_index(), rr_bypass, -1, fl_inputs.reset_temporal_history, fl_inputs.dirty_reset_regions, false, false);
	fl_view->advance_frame_index();

	rd->draw_command_end_label();

	result.diffuse_texture = fl_view->get_denoised_diffuse_texture();
	result.specular_texture = fl_view->get_denoised_specular_texture();
	result.active = result.diffuse_texture.is_valid() && result.specular_texture.is_valid();
	return result;
}
