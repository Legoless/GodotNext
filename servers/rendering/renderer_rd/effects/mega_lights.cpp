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
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_rd/effects/megalights_stbn.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_generate_samples.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_guiding.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_reference.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_function_atlas.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"

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

static void ml_free_rid(RD *p_rd, RID &r_rid) {
	if (p_rd != nullptr && r_rid.is_valid()) {
		p_rd->free_rid(r_rid);
	}
	r_rid = RID();
}

} // namespace

/* MegaLightsViewData */

void MegaLightsViewData::_free_sampling_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, sample_buffer);
	ml_free_rid(rd, visibility_buffer);
	ml_free_rid(rd, guiding_buffers[0]);
	ml_free_rid(rd, guiding_buffers[1]);
	sample_grid_size = Size2i();
	guiding_tile_dims = Size2i();
	samples_per_pixel = 0;
	guiding_write_index = 0;
	guiding_has_history = false;
}

void MegaLightsViewData::_free_reference_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, reference_accum_texture);
	reference_accum_size = Size2i();
	reference_frame_count = 0;
	reference_camera_hash = 0;
}

void MegaLightsViewData::_free_verify_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, verify_accum_texture);
	ml_free_rid(rd, verify_counts_buffer);
	verify_size = Size2i();
}

void MegaLightsViewData::_free_transient_buffers() {
	RD *rd = RD::get_singleton();
	ml_free_rid(rd, params_ubo);
	ml_free_rid(rd, layer_group_buffer);
	layer_group_capacity = 0;
	ml_free_rid(rd, light_shadow_policy_buffer);
	light_shadow_policy_capacity = 0;
}

void MegaLightsViewData::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (p_render_buffers == nullptr) {
		free_data();
		return;
	}
	const Size2i new_size = p_render_buffers->get_internal_size();
	if (new_size != internal_size) {
		_free_sampling_buffers();
		_free_reference_buffers();
		_free_verify_buffers();
		internal_size = new_size;
		frame_index = 0;
	}
}

void MegaLightsViewData::free_data() {
	_free_sampling_buffers();
	_free_reference_buffers();
	_free_verify_buffers();
	_free_transient_buffers();
	internal_size = Size2i();
	frame_index = 0;
}

MegaLightsViewData::~MegaLightsViewData() {
	free_data();
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

	ERR_FAIL_COND_V(!sample_buffer.is_valid() || !visibility_buffer.is_valid() || !guiding_buffers[0].is_valid() || !guiding_buffers[1].is_valid(), false);

	rd->buffer_clear(guiding_buffers[0], 0, guiding_bytes);
	rd->buffer_clear(guiding_buffers[1], 0, guiding_bytes);
	guiding_write_index = 0;
	guiding_has_history = false;
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
	light_shadow_policy_buffer = rd->storage_buffer_create(light_shadow_policy_capacity * 2u * sizeof(uint32_t));
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

MegaLights::DownsampleMode MegaLights::parse_downsample_variant(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		const String s = String(p_value).to_lower().strip_edges();
		if (s == "full" || s == "0") {
			return DOWNSAMPLE_FULL;
		}
		return DOWNSAMPLE_HALF;
	}
	return int(p_value) == int(DOWNSAMPLE_FULL) ? DOWNSAMPLE_FULL : DOWNSAMPLE_HALF;
}

MegaLights::Settings MegaLights::validate_settings(const Settings &p_settings) {
	Settings out = p_settings;
	out.samples_per_pixel = CLAMP(out.samples_per_pixel, MIN_SAMPLES_PER_PIXEL, MAX_SAMPLES_PER_PIXEL);
	out.downsample = out.downsample == DOWNSAMPLE_FULL ? DOWNSAMPLE_FULL : DOWNSAMPLE_HALF;
	if (!Math::is_finite(out.falloff_cull_threshold) || out.falloff_cull_threshold < 0.0f) {
		out.falloff_cull_threshold = DEFAULT_FALLOFF_CULL_THRESHOLD;
	}
	return out;
}

MegaLights::Settings MegaLights::read_settings() {
	Settings s;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return s;
	}
	if (ps->has_setting(SETTING_SAMPLES_PER_PIXEL)) {
		s.samples_per_pixel = uint32_t(MAX(int(ps->get_setting(SETTING_SAMPLES_PER_PIXEL)), 1));
	}
	if (ps->has_setting(SETTING_DOWNSAMPLE)) {
		s.downsample = parse_downsample_variant(ps->get_setting(SETTING_DOWNSAMPLE));
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

	RID stbn_texture;
	RID dummy_storage_buffer;

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

		ml_free_rid(rd, gpu->stbn_texture);
		ml_free_rid(rd, gpu->dummy_storage_buffer);
	}
	gpu->common_initialized = false;
	gpu->common_failed = false;
	gpu->ref_rt_initialized = false;
	gpu->ref_rt_failed = false;
	gpu->ref_rt_alpha_initialized = false;
	gpu->ref_rt_alpha_failed = false;
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

void MegaLights::_upload_light_shadow_policies(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, bool p_include_directional) const {
	RD *rd = RD::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	NextGIRTScene *rt_scene = p_inputs.next_gi != nullptr ? p_inputs.next_gi->get_rt_scene() : nullptr;

	LocalVector<uint32_t> caster_masks;
	const uint32_t dir_count = p_include_directional ? p_inputs.render_data->directional_light_count : 0u;
	light_storage->get_view_light_shadow_caster_masks(dir_count, caster_masks);

	const uint32_t count = MAX(uint32_t(caster_masks.size()), 1u);
	if (!p_view->ensure_light_shadow_policy_buffer(count)) {
		return;
	}

	LocalVector<uint32_t> policy_words;
	policy_words.resize(count * 2u);
	for (uint32_t i = 0; i < caster_masks.size(); i++) {
		const uint32_t cm = caster_masks[i];
		if (cm != 0u && rt_scene != nullptr) {
			policy_words[i * 2u] = rt_scene->get_shadow_light_mode(cm);
			policy_words[i * 2u + 1u] = cm & NEXTGI_SHADOW_LAYER_MASK;
		} else {
			policy_words[i * 2u] = 0u;
			policy_words[i * 2u + 1u] = 0u;
		}
	}
	if (caster_masks.is_empty()) {
		policy_words[0] = 0u;
		policy_words[1] = 0u;
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
	_upload_light_shadow_policies(p_inputs, view, settings.directional_lights);
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

		_upload_light_shadow_policies(p_inputs, p_view, cfg.directional_lights);

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
	// Until #146 visibility chain populates visibility_buffer, callers invoke update_guiding()
	// after visibility; advance frame index for next frame's 2x2 downsample rotation & STBN slice.
	view->advance_frame_index();
}
