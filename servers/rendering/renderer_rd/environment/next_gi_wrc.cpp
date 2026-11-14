/**************************************************************************/
/*  next_gi_wrc.cpp                                                       */
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

#include "next_gi_wrc.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_device.h"

#include <cstddef>

using namespace RendererRD;

// GodotNext: ps_swrt_trace (#186). The six update pipelines, shared by the hardware update and
// its software sibling.
struct NextGIWRCPipelines {
	RID version;
	RID trace;
	RID project;
	RID translucency;
	RID translucency_mark;
	RID translucency_trace;
	RID translucency_project;
};

struct NextGIWRC::ShaderState : NextGIWRCPipelines {
	NextgiWrcUpdateShaderRD shader;
};

// GodotNext: ps_swrt_trace (#186). No ray query: the software tier's WRC producer (L9b).
struct NextGIWRC::SWRTShaderState : NextGIWRCPipelines {
	NextgiWrcUpdateSwrtShaderRD shader;
};

static Vector<String> _nextgi_wrc_variant_defines() {
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("\n#define MODE_SH\n");
	defines.push_back("\n#define MODE_TRANSLUCENCY_VOLUME\n");
	defines.push_back("\n#define MODE_TRANSLUCENCY_MARK\n");
	defines.push_back("\n#define MODE_TRANSLUCENCY_TRACE\n");
	defines.push_back("\n#define MODE_TRANSLUCENCY_PROJECT\n");
	return defines;
}

static bool _nextgi_wrc_create_pipelines(ShaderRD &p_shader, NextGIWRCPipelines &r_pipelines) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RID *pipelines[6] = { &r_pipelines.trace, &r_pipelines.project, &r_pipelines.translucency, &r_pipelines.translucency_mark, &r_pipelines.translucency_trace, &r_pipelines.translucency_project };
	bool valid = true;
	for (int i = 0; i < 6; i++) {
		*pipelines[i] = rd->compute_pipeline_create(p_shader.version_get_shader(r_pipelines.version, i));
		valid = valid && pipelines[i]->is_valid();
	}
	return valid;
}

struct NextGIWRCParamsGPU {
	float camera[4] = {};
	float sky[4] = {};
	float origin_spacing[4] = {};
	uint32_t probe_base = 0;
	uint32_t probe_count = 0;
	uint32_t frame = 0;
	uint32_t record_count = 0;
	uint32_t page_rect_count = 0;
	// GodotNext: nextgi_wrc_shift (#254). Bit L: level L's probes take their trace without history.
	uint32_t fresh_levels = 0;
	uint32_t pad[2] = {};
	float inv_view_proj[16] = {};
	float volume[4] = {}; // x width, y height, z near, w far.
	// GodotNext: nextgi_view_math. nextgi_wrc_update.glsl declares this vec4; before it was
	// C++-only padding and the shader read previous_view_proj and history 16 bytes off. The
	// forward shader's NextGIWRCForward copy declares it too.
	float depth_coefficients[4] = {};
	float previous_view_proj[16] = {};
	float history[4] = {}; // blend, Lite, valid, sky lighting enabled.
};
static_assert(sizeof(NextGIWRCParamsGPU) == 256, "WRC params must match nextgi_wrc_update.glsl.");
// GodotNext: nextgi_view_math. The std430 offsets nextgi_wrc_update.glsl reads them at.
static_assert(offsetof(NextGIWRCParamsGPU, volume) == 144, "WRC params: volume must sit at std430 offset 144.");
static_assert(offsetof(NextGIWRCParamsGPU, depth_coefficients) == 160, "WRC params: depth_coefficients must sit at std430 offset 160.");
static_assert(offsetof(NextGIWRCParamsGPU, previous_view_proj) == 176, "WRC params: previous_view_proj must sit at std430 offset 176.");
static_assert(offsetof(NextGIWRCParamsGPU, history) == 240, "WRC params: history must sit at std430 offset 240.");

struct NextGIWRCCountersGPU {
	uint32_t rays = 0;
	uint32_t hits = 0;
	uint32_t sky_misses = 0;
	uint32_t energy_bits = 0;
	uint32_t fog_samples = 0;
	uint32_t fog_ambient_bits = 0;
	uint32_t pad_a = 0;
	uint32_t pad_b = 0;
	uint32_t translucent_probes = 0;
	uint32_t translucent_rays = 0;
	uint32_t translucent_hits = 0;
	uint32_t translucent_sky_misses = 0;
	uint32_t translucent_sky_energy = 0;
	// GodotNext: ps_swrt_trace (#186). Written only by the software sibling (NEXTGI_TRACE_SDF):
	// probe-ray SDF hits, and the step-limit and near-origin ones among them.
	uint32_t sdf_hits = 0;
	uint32_t sdf_exhausted = 0;
	uint32_t sdf_near = 0;
};
static_assert(sizeof(NextGIWRCCountersGPU) == 64, "WRC counters must match NextGIWRCCounters.");

static Mutex nextgi_wrc_counter_mutex;
static HashMap<uint64_t, NextGIWRC *> nextgi_wrc_counter_registry;
static uint64_t nextgi_wrc_counter_next_token = 1;

NextGIWRC *NextGIWRC::singleton = nullptr;
NextGIWRC::ShaderState *NextGIWRC::shared_shader = nullptr;
NextGIWRC::SWRTShaderState *NextGIWRC::shared_swrt_shader = nullptr;

// GodotNext: nextgi_wrc_per_view (#253). Every live WRC (render thread), the shared update
// shaders, and the WRC whose counters the tier info reports.
static LocalVector<NextGIWRC *> nextgi_wrc_live;
static NextGIWRC *nextgi_wrc_last_updated = nullptr;

namespace RendererRD {

uint32_t nextgi_wrc_level_for_distance(float p_distance) {
	const float distance = MAX(p_distance, 0.0f);
	if (distance <= 8.0f) {
		return 0;
	}
	if (distance <= 16.0f) {
		return 1;
	}
	if (distance <= 32.0f) {
		return 2;
	}
	return 3;
}

// GodotNext: nextgi_wrc_shift (#254)
static_assert(NEXTGI_WRC_FRESH_FRAMES == NEXTGI_WRC_PROBE_COUNT / NEXTGI_WRC_PROBES_PER_FRAME, "A fresh window is one full update cycle.");
static_assert(NEXTGI_WRC_LEVELS == 4, "The fresh bookkeeping covers four levels.");

uint32_t nextgi_wrc_shifted_levels(const Vector3 &p_previous_origin, const Vector3 &p_origin, bool p_had_previous) {
	if (!p_had_previous) {
		return 0u;
	}
	uint32_t shifted = 0u;
	for (uint32_t level = 0; level < NEXTGI_WRC_LEVELS; level++) {
		const float spacing = float(1u << level);
		if (!nextgi_wrc_snap_origin(p_previous_origin, spacing).is_equal_approx(nextgi_wrc_snap_origin(p_origin, spacing))) {
			shifted |= 1u << level;
		}
	}
	return shifted;
}

// GodotNext: nextgi_wrc_per_view (#253). A probe's first trace takes no history (its offset texel
// is still clear), so one full pass leaves every probe complete.
uint32_t nextgi_wrc_probes_to_trace(bool p_had_previous) {
	return p_had_previous ? uint32_t(NEXTGI_WRC_PROBES_PER_FRAME) : uint32_t(NEXTGI_WRC_PROBE_COUNT);
}

void nextgi_wrc_mark_fresh(uint32_t p_shifted_levels, uint32_t r_fresh_frames[4]) {
	for (uint32_t level = 0; level < NEXTGI_WRC_LEVELS; level++) {
		if ((p_shifted_levels & (1u << level)) != 0u) {
			r_fresh_frames[level] = NEXTGI_WRC_FRESH_FRAMES;
		}
	}
}

uint32_t nextgi_wrc_fresh_mask(const uint32_t p_fresh_frames[4]) {
	uint32_t mask = 0u;
	for (uint32_t level = 0; level < NEXTGI_WRC_LEVELS; level++) {
		if (p_fresh_frames[level] > 0u) {
			mask |= 1u << level;
		}
	}
	return mask;
}

void nextgi_wrc_age_fresh(uint32_t r_fresh_frames[4]) {
	for (uint32_t level = 0; level < NEXTGI_WRC_LEVELS; level++) {
		if (r_fresh_frames[level] > 0u) {
			r_fresh_frames[level]--;
		}
	}
}

Vector3 nextgi_wrc_snap_origin(const Vector3 &p_camera, float p_spacing) {
	const float spacing = MAX(p_spacing, 0.001f);
	return Vector3(
			Math::floor(p_camera.x / spacing) * spacing,
			Math::floor(p_camera.y / spacing) * spacing,
			Math::floor(p_camera.z / spacing) * spacing);
}

Vector3 nextgi_wrc_relocate(const Vector3 &p_probe, const Vector3 &p_mean_escape, float p_backface_fraction, float p_spacing) {
	if (p_backface_fraction <= 0.25f) {
		return p_probe;
	}
	const float length = p_mean_escape.length();
	if (length <= 0.0001f) {
		return p_probe;
	}
	const float travel = MIN(length, 0.4f * p_spacing);
	return p_probe + p_mean_escape / length * travel;
}

int32_t nextgi_translucency_probe_index(const Vector3 &p_world, const Vector3 &p_camera, const Vector3 &p_origin) {
	const float distance = p_world.distance_to(p_camera);
	if (distance > 16.0f) {
		return -1;
	}
	const uint32_t level = distance <= 8.0f ? 0 : 1;
	const float spacing = nextgi_wrc_spacing(level);
	const Vector3 origin = nextgi_wrc_snap_origin(p_origin, spacing);
	const Vector3 rel = (p_world - origin) / spacing + Vector3(7.5, 7.5, 7.5);
	const Vector3i cell(
			CLAMP(int(Math::round(rel.x)), 0, 15),
			CLAMP(int(Math::round(rel.y)), 0, 15),
			CLAMP(int(Math::round(rel.z)), 0, 15));
	return level * 4096 + cell.x + cell.y * 16 + cell.z * 256;
}

uint32_t nextgi_translucency_volume_level(float p_distance, bool p_lite) {
	return MIN(nextgi_wrc_level_for_distance(p_distance) + uint32_t(p_lite), uint32_t(NEXTGI_WRC_LEVELS - 1));
}

float nextgi_translucency_history_weight(bool p_valid, const Vector2 &p_previous_uv) {
	return p_valid && p_previous_uv.x >= 0.0f && p_previous_uv.y >= 0.0f && p_previous_uv.x <= 1.0f && p_previous_uv.y <= 1.0f ? 0.9f : 0.0f;
}

} // namespace RendererRD

NextGIWRC::NextGIWRC() {
	if (singleton == nullptr) {
		singleton = this;
	}
	nextgi_wrc_live.push_back(this);
}

NextGIWRC::~NextGIWRC() {
	{
		MutexLock lock(nextgi_wrc_counter_mutex);
		if (counter_token != 0) {
			nextgi_wrc_counter_registry.erase(counter_token);
			counter_token = 0;
		}
		counters_pending = false;
	}
	if (singleton == this) {
		singleton = nullptr;
	}
	nextgi_wrc_live.erase(this);
	if (nextgi_wrc_last_updated == this) {
		nextgi_wrc_last_updated = nullptr;
	}
}

void NextGIWRC::_free_uniform_set() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		if (uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
			rd->free_rid(uniform_set);
		}
		if (volume_uniform_set.is_valid() && rd->uniform_set_is_valid(volume_uniform_set)) {
			rd->free_rid(volume_uniform_set);
		}
		if (translucency_uniform_set.is_valid() && rd->uniform_set_is_valid(translucency_uniform_set)) {
			rd->free_rid(translucency_uniform_set);
		}
		if (translucency_mark_uniform_set.is_valid() && rd->uniform_set_is_valid(translucency_mark_uniform_set)) {
			rd->free_rid(translucency_mark_uniform_set);
		}
	}
	// GodotNext: ps_swrt_trace (#186)
	if (rd != nullptr && swrt_trace_set.is_valid() && rd->uniform_set_is_valid(swrt_trace_set)) {
		rd->free_rid(swrt_trace_set);
	}
	swrt_trace_set = RID();
	uniform_set = RID();
	volume_uniform_set = RID();
	translucency_uniform_set = RID();
	translucency_mark_uniform_set = RID();
}

void NextGIWRC::free_resources() {
	_free_uniform_set();
	// GodotNext: nextgi_wrc_per_view (#253). The update shaders are shared: free_shared() frees them.
	swrt_shader = nullptr;
	{
		MutexLock lock(nextgi_wrc_counter_mutex);
		if (counter_token != 0) {
			nextgi_wrc_counter_registry.erase(counter_token);
			counter_token = 0;
		}
		counters_pending = false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	shader = nullptr;
	if (rd == nullptr) {
		return;
	}
	if (readback_buffer.is_valid()) {
		rd->free_rid(readback_buffer);
		readback_buffer = RID();
	}
	RID resources[31] = { radiance, mip4, mip2, visibility, sh_r, sh_g, sh_b, offsets, translucency_r, translucency_g, translucency_b, translucency_history_r, translucency_history_g, translucency_history_b, translucency_radiance, translucency_mip4, translucency_mip2, translucency_visibility, translucency_sh_r, translucency_sh_g, translucency_sh_b, translucency_offsets, translucency_mark_framebuffer, translucency_mark_texture, translucency_marks, translucency_indices, params_buffer, counter_buffer, sampler, dummy_tex, dummy_buffer };
	for (RID resource : resources) {
		if (resource.is_valid()) {
			rd->free_rid(resource);
		}
	}
	radiance = RID();
	mip4 = RID();
	mip2 = RID();
	visibility = RID();
	sh_r = RID();
	sh_g = RID();
	sh_b = RID();
	offsets = RID();
	translucency_r = RID();
	translucency_g = RID();
	translucency_b = RID();
	translucency_history_r = RID();
	translucency_history_g = RID();
	translucency_history_b = RID();
	translucency_radiance = RID();
	translucency_mip4 = RID();
	translucency_mip2 = RID();
	translucency_visibility = RID();
	translucency_sh_r = RID();
	translucency_sh_g = RID();
	translucency_sh_b = RID();
	translucency_offsets = RID();
	translucency_mark_texture = RID();
	translucency_mark_framebuffer = RID();
	translucency_marks = RID();
	translucency_indices = RID();
	translucency_mark_size = Size2i();
	translucency_size = Vector3i();
	params_buffer = RID();
	counter_buffer = RID();
	sampler = RID();
	dummy_tex = RID();
	dummy_buffer = RID();
	volume_history_valid = false;
	has_previous_camera = false;
	// GodotNext: nextgi_wrc_per_view (#253). The textures come back cleared: the next update is a first one.
	has_previous_origin = false;
	cursor = 0;
	previous_sky_read = false;
	grid_camera_valid = false;
	translucency_marked = 0;
	translucency_rays = 0;
	translucency_hits = 0;
}

RID NextGIWRC::get_translucency_mark_target(const Size2i &p_size) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, RID());
	if (translucency_mark_size == p_size && translucency_mark_framebuffer.is_valid()) {
		return translucency_mark_framebuffer;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, RID());
	if (translucency_mark_framebuffer.is_valid()) {
		rd->free_rid(translucency_mark_framebuffer);
		translucency_mark_framebuffer = RID();
	}
	if (translucency_mark_texture.is_valid()) {
		rd->free_rid(translucency_mark_texture);
	}
	RD::TextureFormat format;
	format.width = p_size.x;
	format.height = p_size.y;
	format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	translucency_mark_texture = rd->texture_create(format, RD::TextureView());
	ERR_FAIL_COND_V(translucency_mark_texture.is_null(), RID());
	rd->set_resource_name(translucency_mark_texture, "NextGI Translucency Probe Marks");
	translucency_mark_framebuffer = rd->framebuffer_create(Vector<RID>({ translucency_mark_texture }));
	rd->set_resource_name(translucency_mark_framebuffer, "NextGI Translucency Mark Framebuffer");
	translucency_mark_size = p_size;
	return translucency_mark_framebuffer;
}

bool NextGIWRC::uses_translucency_probes() const {
	NextGI *next_gi = NextGI::get_singleton();
	return next_gi != nullptr && next_gi->get_resolved_quality() != NextGI::QUALITY_MEDIUM;
}

bool NextGIWRC::_ensure_textures() {
	if (radiance.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	BitField<RD::TextureUsageBits> usage = RD::TEXTURE_USAGE_STORAGE_BIT;
	usage.set_flag(RD::TEXTURE_USAGE_SAMPLING_BIT);
	usage.set_flag(RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	usage.set_flag(RD::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	packed_radiance = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, usage);

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = usage;
	tf.width = 1280;
	tf.height = 1280;
	tf.format = packed_radiance ? RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32 : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	radiance = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(radiance.is_null(), false);
	rd->set_resource_name(radiance, "NextGI WRC Radiance");
	rd->texture_clear(radiance, Color(0, 0, 0, 1), 0, 1, 0, 1);

	tf.width = 1024;
	tf.height = 1024;
	tf.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	visibility = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(visibility.is_null(), false);
	rd->texture_clear(visibility, Color(0, 0, 0, 0), 0, 1, 0, 1);

	tf.width = 768;
	tf.height = 768;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	mip4 = rd->texture_create(tf, RD::TextureView());
	tf.width = 512;
	tf.height = 512;
	mip2 = rd->texture_create(tf, RD::TextureView());

	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.width = 16;
	tf.height = 16;
	tf.depth = 64;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	sh_r = rd->texture_create(tf, RD::TextureView());
	sh_g = rd->texture_create(tf, RD::TextureView());
	sh_b = rd->texture_create(tf, RD::TextureView());
	offsets = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(sh_r.is_null() || sh_g.is_null() || sh_b.is_null() || offsets.is_null(), false);
	// 3D textures have one layer; depth is the clipmap stack, not array layers.
	rd->texture_clear(sh_r, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(sh_g, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(sh_b, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(offsets, Color(0, 0, 0, 0), 0, 1, 0, 1);

	RD::SamplerState state;
	state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler = rd->sampler_create(state);
	params_buffer = rd->storage_buffer_create(sizeof(NextGIWRCParamsGPU));
	counter_buffer = rd->storage_buffer_create(sizeof(NextGIWRCCountersGPU));
	RD::TextureFormat dummy_format;
	dummy_format.width = 1;
	dummy_format.height = 1;
	dummy_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	dummy_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	dummy_tex = rd->texture_create(dummy_format, RD::TextureView());
	dummy_buffer = rd->storage_buffer_create(16);
	return radiance.is_valid() && counter_buffer.is_valid() && dummy_tex.is_valid();
}

bool NextGIWRC::_ensure_translucency_cache() {
	if (translucency_radiance.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	tf.width = 640;
	tf.height = 640;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	translucency_radiance = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(translucency_radiance.is_null(), false);
	rd->set_resource_name(translucency_radiance, "NextGI Translucency Radiance");
	tf.width = 512;
	tf.height = 512;
	tf.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	translucency_visibility = rd->texture_create(tf, RD::TextureView());
	tf.width = 384;
	tf.height = 384;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	translucency_mip4 = rd->texture_create(tf, RD::TextureView());
	tf.width = 256;
	tf.height = 256;
	translucency_mip2 = rd->texture_create(tf, RD::TextureView());
	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.width = 16;
	tf.height = 16;
	tf.depth = 32;
	translucency_sh_r = rd->texture_create(tf, RD::TextureView());
	translucency_sh_g = rd->texture_create(tf, RD::TextureView());
	translucency_sh_b = rd->texture_create(tf, RD::TextureView());
	translucency_offsets = rd->texture_create(tf, RD::TextureView());
	RID textures[] = { translucency_radiance, translucency_visibility, translucency_mip4, translucency_mip2, translucency_sh_r, translucency_sh_g, translucency_sh_b, translucency_offsets };
	for (const RID &texture : textures) {
		ERR_FAIL_COND_V(texture.is_null(), false);
		rd->texture_clear(texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	translucency_marks = rd->storage_buffer_create(256 * sizeof(uint32_t));
	translucency_indices = rd->storage_buffer_create((NEXTGI_TRANSLUCENCY_PROBE_CAP + 1) * sizeof(uint32_t));
	return translucency_marks.is_valid() && translucency_indices.is_valid();
}

// GodotNext: nextgi_view_math
NextGIWRC::ViewParams NextGIWRC::view_params(const RenderSceneDataRD &p_scene_data) {
	ViewParams view;
	view.view_proj = NextGIView::view_projection(p_scene_data);
	view.inv_view_proj = view.view_proj.inverse();
	view.depth_coefficients = NextGIViewMath::depth_coefficients(NextGIView::depth_projection(p_scene_data));
	return view;
}

Vector<String> NextGIWRC::get_shader_variant_defines() {
	return _nextgi_wrc_variant_defines();
}

String NextGIWRC::build_update_shader_source(int p_variant) {
	NextgiWrcUpdateShaderRD update_shader;
	Vector<String> defines;
	defines.push_back("");
	update_shader.initialize(defines);
	const RID version = update_shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = update_shader.version_build_variant_stage_sources(version, p_variant);
	update_shader.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

// GodotNext: ps_swrt_trace (#186)
String NextGIWRC::build_update_swrt_shader_source(int p_variant) {
	NextgiWrcUpdateSwrtShaderRD update_shader;
	update_shader.initialize(_nextgi_wrc_variant_defines());
	const RID version = update_shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = update_shader.version_build_variant_stage_sources(version, p_variant);
	update_shader.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

bool NextGIWRC::_ensure_shader() {
	if (shader != nullptr && shader->version.is_valid()) {
		return true;
	}
	// GodotNext: nextgi_wrc_per_view (#253). One shared shader for every view's WRC.
	if (shared_shader != nullptr && shared_shader->version.is_valid()) {
		shader = shared_shader;
		return true;
	}
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_COND_V(!next_gi->can_initialize_ray_query_shaders(), false);
	shader = memnew(ShaderState);
	shader->shader.initialize(_nextgi_wrc_variant_defines());
	// GodotNext: shader_baker_ray_query
	// Embedded so the shader baker bakes or skips the ray-query version per target (#40).
	shader->version = shader->shader.version_create(true);
	if (!shader->version.is_valid()) {
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI WRC shader failed to compile.");
	}
	if (!_nextgi_wrc_create_pipelines(shader->shader, *shader)) {
		shader->shader.version_free(shader->version);
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI WRC pipeline failed to compile.");
	}
	next_gi->notify_ray_query_shader_initialized();
	shared_shader = shader;
	return true;
}

// GodotNext: ps_swrt_trace (#186)
bool NextGIWRC::_ensure_swrt_shader() {
	if (swrt_shader != nullptr && swrt_shader->version.is_valid()) {
		return true;
	}
	// GodotNext: nextgi_wrc_per_view (#253). One shared software sibling for every view's WRC.
	if (shared_swrt_shader != nullptr && shared_swrt_shader->version.is_valid()) {
		swrt_shader = shared_swrt_shader;
		return true;
	}
	swrt_shader = memnew(SWRTShaderState);
	swrt_shader->shader.initialize(_nextgi_wrc_variant_defines());
	swrt_shader->version = swrt_shader->shader.version_create(false);
	if (!swrt_shader->version.is_valid()) {
		memdelete(swrt_shader);
		swrt_shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI software WRC shader failed to compile.");
	}
	if (!_nextgi_wrc_create_pipelines(swrt_shader->shader, *swrt_shader)) {
		swrt_shader->shader.version_free(swrt_shader->version);
		memdelete(swrt_shader);
		swrt_shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI software WRC pipeline failed to compile.");
	}
	shared_swrt_shader = swrt_shader;
	return true;
}

// GodotNext: nextgi_async_counters (#186). The counters reach fill_info() one or more frames late.
void NextGIWRC::_request_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !counter_buffer.is_valid()) {
		return;
	}
	uint64_t token = 0;
	{
		MutexLock lock(nextgi_wrc_counter_mutex);
		if (counters_pending) {
			return;
		}
		counters_pending = true;
		token = nextgi_wrc_counter_next_token++;
		counter_token = token;
		nextgi_wrc_counter_registry.insert(token, this);
	}
	if (!readback_buffer.is_valid()) {
		readback_buffer = rd->storage_buffer_create(sizeof(NextGIWRCCountersGPU));
	}
	Error err = readback_buffer.is_valid() ? OK : FAILED;
	if (err == OK) {
		// A copy, so the zeroing below cannot race the read.
		rd->buffer_copy(counter_buffer, readback_buffer, 0, 0, sizeof(NextGIWRCCountersGPU));
		err = rd->buffer_get_data_async(readback_buffer, callable_mp_static(&NextGIWRC::_counters_received).bind(token), 0, sizeof(NextGIWRCCountersGPU));
	}
	if (err != OK) {
		MutexLock lock(nextgi_wrc_counter_mutex);
		nextgi_wrc_counter_registry.erase(token);
		counter_token = 0;
		counters_pending = false;
	}
}

void NextGIWRC::_counters_received(const Vector<uint8_t> &p_data, uint64_t p_token) {
	MutexLock lock(nextgi_wrc_counter_mutex);
	NextGIWRC **owner = nextgi_wrc_counter_registry.getptr(p_token);
	if (owner == nullptr) {
		return;
	}
	NextGIWRC *self = *owner;
	nextgi_wrc_counter_registry.erase(p_token);
	self->counter_token = 0;
	self->counters_pending = false;
	if (p_data.size() < int(sizeof(NextGIWRCCountersGPU))) {
		return;
	}
	NextGIWRCCountersGPU got;
	memcpy(&got, p_data.ptr(), sizeof(got));
	// One frame's totals: the update's probe and translucency rays, and the fog that read the
	// cache after the slot.
	self->last_rays = got.rays;
	self->last_hits = got.hits;
	self->last_sky = got.sky_misses;
	self->last_energy_bits = got.energy_bits;
	self->fog_samples = got.fog_samples;
	self->fog_ambient_bits = got.fog_ambient_bits;
	self->translucency_marked = MIN(got.translucent_probes, uint32_t(NEXTGI_TRANSLUCENCY_PROBE_CAP));
	self->translucency_rays = got.translucent_rays;
	self->translucency_hits = got.translucent_hits;
	self->translucency_sky_energy = float(got.translucent_sky_energy) / 64.0f;
	self->last_sdf_hits = got.sdf_hits;
	self->last_sdf_exhausted = got.sdf_exhausted;
	self->last_sdf_near = got.sdf_near;
}

void NextGIWRC::update(const RenderDataRD *p_render_data) {
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi == nullptr) {
		return;
	}
	// GodotNext: ps_swrt_trace (#186, plan §21.2 L9b). The software tier updates the cache with
	// the SDF trace through the sibling ShaderRD; no ray-query shader is created there.
	const bool swrt = next_gi->get_tier_result().tier == NextGI::TIER_SOFTWARE;
	NextGISWRTTrace *swrt_tracer = swrt ? next_gi->get_swrt_trace() : nullptr;
	if (swrt) {
		if (swrt_tracer == nullptr || !swrt_tracer->is_ready()) {
			return;
		}
	} else if (!next_gi->can_initialize_ray_query_shaders() || next_gi->get_rt_scene() == nullptr || !next_gi->get_rt_scene()->is_tlas_traceable()) {
		return;
	}
	if (!_ensure_textures() || !_ensure_translucency_cache() || !(swrt ? _ensure_swrt_shader() : _ensure_shader())) {
		return;
	}
	nextgi_wrc_last_updated = this; // GodotNext: nextgi_wrc_per_view (#253). The tier info reports it.
	ShaderRD &active_shader = swrt ? static_cast<ShaderRD &>(swrt_shader->shader) : static_cast<ShaderRD &>(shader->shader);
	const NextGIWRCPipelines &active = swrt ? static_cast<const NextGIWRCPipelines &>(*swrt_shader) : static_cast<const NextGIWRCPipelines &>(*shader);
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const Vector3 camera = p_render_data->scene_data->cam_transform.origin;
	// GodotNext: nextgi_translucency_volume (#61). 1/16-screen x 32 slice irradiance volume.
	Size2i screen = p_render_data->render_buffers.is_valid() ? p_render_data->render_buffers->get_internal_size() : Size2i(1, 1);
	const Vector3i volume_size(MAX((screen.width + 15) / 16, 1), MAX((screen.height + 15) / 16, 1), 32);
	if (volume_size != translucency_size) {
		RID *textures[6] = { &translucency_r, &translucency_g, &translucency_b, &translucency_history_r, &translucency_history_g, &translucency_history_b };
		for (RID *texture : textures) {
			if (texture->is_valid()) {
				rd->free_rid(*texture);
			}
			RD::TextureFormat volume_format;
			volume_format.texture_type = RD::TEXTURE_TYPE_3D;
			volume_format.width = volume_size.x;
			volume_format.height = volume_size.y;
			volume_format.depth = volume_size.z;
			volume_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			volume_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			*texture = rd->texture_create(volume_format, RD::TextureView());
			rd->texture_clear(*texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
		translucency_size = volume_size;
		volume_history_valid = false;
	}
	const Vector3 origin = nextgi_wrc_snap_origin(camera, 1.0f);
	NextGIWRCParamsGPU params;
	params.camera[0] = camera.x;
	params.camera[1] = camera.y;
	params.camera[2] = camera.z;
	params.camera[3] = 1.0f;
	params.origin_spacing[0] = origin.x;
	params.origin_spacing[1] = origin.y;
	params.origin_spacing[2] = origin.z;
	params.origin_spacing[3] = 1.0f;
	// GodotNext: nextgi_wrc_per_view (#253). A first update traces every probe.
	const uint32_t trace_probes = nextgi_wrc_probes_to_trace(has_previous_origin);
	params.probe_base = cursor;
	params.probe_count = trace_probes;
	// GodotNext: nextgi_wrc_shift (#254)
	nextgi_wrc_mark_fresh(nextgi_wrc_shifted_levels(previous_origin, origin, has_previous_origin), fresh_frames);
	params.fresh_levels = nextgi_wrc_fresh_mask(fresh_frames);
	previous_origin = origin;
	has_previous_origin = true;
	params.frame = frame;
	params.record_count = swrt ? 0u : next_gi->get_rt_scene()->get_stats().instance_count;
	NextGIRelight *relight = NextGIRelight::get_singleton();
	params.page_rect_count = relight != nullptr ? relight->get_page_rect_count() : 0;
	// GodotNext: nextgi_view_math. The froxels and the history use the depth texture's clip space.
	const ViewParams view = view_params(*p_render_data->scene_data);
	const Projection &view_proj = view.view_proj;
	for (int i = 0; i < 16; i++) {
		params.inv_view_proj[i] = view.inv_view_proj.columns[i / 4][i % 4];
	}
	for (int i = 0; i < 4; i++) {
		params.depth_coefficients[i] = view.depth_coefficients[i];
	}
	params.volume[0] = float(volume_size.x);
	params.volume[1] = float(volume_size.y);
	params.volume[2] = 0.05f;
	params.volume[3] = 64.0f;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	// GodotNext: ps_lumen_lite (#183). A miss reads the sky radiance, as the gather's misses do;
	// the background color is only the fallback without a sky. Lite gets all of its GI from here.
	RID sky_radiance;
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const Color bg = scene->environment_get_bg_color(p_render_data->environment);
		const float energy = scene->environment_get_bg_energy_multiplier(p_render_data->environment);
		params.sky[0] = bg.r * energy;
		params.sky[1] = bg.g * energy;
		params.sky[2] = bg.b * energy;
		const RID sky = scene->environment_get_sky(p_render_data->environment);
		if (sky.is_valid() && scene->get_sky() != nullptr) {
			sky_radiance = scene->get_sky()->sky_get_radiance_texture_2d(sky);
		}
	}
	const bool sky_read = scene != nullptr && p_render_data->environment.is_valid() && scene->environment_get_nextgi_read_sky_light(p_render_data->environment);
	// Environment.nextgi_read_sky_light off keeps the sky out of the cache: misses read the
	// background color, as they did before the sky term.
	params.sky[3] = sky_radiance.is_valid() && sky_read ? 1.0f : 0.0f;
	const Vector3 forward = -p_render_data->scene_data->cam_transform.basis.get_column(2).normalized();
	if (!has_previous_camera || camera.distance_to(previous_camera_position) > 16.0f || forward.dot(previous_camera_forward) < 0.5f || previous_sky_read != sky_read) {
		volume_history_valid = false;
	}
	params.history[0] = 0.9f;
	params.history[1] = uses_translucency_probes() ? 0.0f : 1.0f;
	params.history[2] = volume_history_valid ? 1.0f : 0.0f;
	params.history[3] = sky_read ? 1.0f : 0.0f;
	const Projection history_projection = has_previous_camera ? previous_view_projection : view_proj;
	for (int i = 0; i < 16; i++) {
		params.previous_view_proj[i] = history_projection.columns[i / 4][i % 4];
	}
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);
	// GodotNext: ps_lumen_lite (#183). The grid the probes are read on from here on.
	grid_camera = camera;
	grid_camera_valid = true;
	// GodotNext: nextgi_async_counters (#186). The previous frame's totals, read before the zeroing.
	_request_counters();
	NextGIWRCCountersGPU zero;
	rd->buffer_update(counter_buffer, 0, sizeof(zero), &zero);
	uint32_t mark_zero[256] = {};
	rd->buffer_update(translucency_marks, 0, sizeof(mark_zero), mark_zero);
	uint32_t index_zero = 0;
	rd->buffer_update(translucency_indices, 0, sizeof(index_zero), &index_zero);

	NextGISRC *src = NextGISRC::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RID chunks[4] = { dummy_buffer, dummy_buffer, dummy_buffer, dummy_buffer };
	if (mesh_storage != nullptr) {
		for (uint32_t i = 0; i < MIN(mesh_storage->mesh_nextgi_rt_get_chunk_count(), 4u); i++) {
			const RID chunk = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(i);
			if (chunk.is_valid()) {
				chunks[i] = chunk;
			}
		}
	}
	RID records = swrt ? RID() : next_gi->get_rt_scene()->get_instance_record_buffer();
	RID surfaces = swrt ? RID() : next_gi->get_rt_scene()->get_surface_record_buffer();
	RID cards = src != nullptr ? src->get_card_buffer() : RID();
	RID card_sets = src != nullptr ? src->get_card_set_buffer() : RID();
	RID page_rects = relight != nullptr ? relight->get_page_rect_buffer() : RID();
	RID src_radiance = relight != nullptr ? relight->get_radiance_texture() : RID();
	RID material_depth = src != nullptr ? src->get_atlas_texture(NextGISRC::ATLAS_DEPTH) : RID();
	auto valid_buffer = [&](RID p_buffer) {
		return p_buffer.is_valid() ? p_buffer : dummy_buffer;
	};
	auto valid_tex = [&](RID p_texture) {
		return p_texture.is_valid() ? p_texture : dummy_tex;
	};
	_free_uniform_set();
	RD::Uniform uniforms[36] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, swrt ? RID() : next_gi->get_rt_scene()->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, radiance),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, visibility),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, sh_r),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, sh_g),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, sh_b),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, offsets),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, mip4),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, mip2),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, valid_buffer(records)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, valid_buffer(surfaces)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, valid_buffer(cards)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, valid_buffer(card_sets)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, valid_buffer(page_rects)),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 18, Vector<RID>({ sampler, valid_tex(src_radiance) })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ sampler, valid_tex(material_depth) })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, counter_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 22, valid_tex(translucency_mark_texture)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, translucency_marks),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, translucency_indices),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 25, translucency_r),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 26, translucency_g),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 27, translucency_b),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 28, translucency_sh_r),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 29, translucency_sh_g),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 30, translucency_sh_b),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 31, translucency_history_r),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 32, translucency_history_g),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 33, translucency_history_b),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 34, sampler),
		// GodotNext: ps_lumen_lite (#183). Every variant declares the sky.
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 35, Vector<RID>({ sampler, valid_tex(sky_radiance) })),
	};
	Vector<RD::Uniform> trace_uniforms;
	for (int i = 0; i < 22; i++) {
		trace_uniforms.push_back(uniforms[i]);
	}
	trace_uniforms.push_back(uniforms[35]);
	uniform_set = rd->uniform_set_create(trace_uniforms, active_shader.version_get_shader(active.version, 0), 0);
	volume_uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 36), active_shader.version_get_shader(active.version, 2), 0);
	translucency_mark_uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 36), active_shader.version_get_shader(active.version, 3), 0);
	rd->set_resource_name(translucency_mark_uniform_set, "NextGI Translucency Mark Uniforms");
	uniforms[1] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, translucency_radiance);
	uniforms[2] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, translucency_visibility);
	uniforms[6] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, translucency_offsets);
	uniforms[7] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, translucency_mip4);
	uniforms[8] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, translucency_mip2);
	translucency_uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 36), active_shader.version_get_shader(active.version, 4), 0);
	// GodotNext: ps_swrt_trace (#186). Set 1 is the SDF trace with the frame's scene params
	// (NextGISWRTTrace::prepare_scene_params() in the AS-updates slot); every variant declares it.
	if (swrt) {
		swrt_trace_set = swrt_tracer->create_trace_uniform_set(active_shader.version_get_shader(active.version, 0), 1, false, next_gi->get_voxel_lighting());
		if (!swrt_trace_set.is_valid()) {
			_free_uniform_set();
			return;
		}
	}
	auto bind_trace_set = [&](RD::ComputeListID p_list) {
		if (swrt) {
			rd->compute_list_bind_uniform_set(p_list, swrt_trace_set, 1);
		}
	};
	if (uniform_set.is_valid()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, active.trace);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		bind_trace_set(list);
		rd->compute_list_dispatch(list, trace_probes, 1, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, active.project);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		bind_trace_set(list);
		rd->compute_list_dispatch(list, trace_probes, 1, 1);
		rd->compute_list_end();
	}
	if (uses_translucency_probes() && translucency_mark_texture.is_valid() && translucency_mark_uniform_set.is_valid() && translucency_uniform_set.is_valid()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, active.translucency_mark);
		rd->compute_list_bind_uniform_set(list, translucency_mark_uniform_set, 0);
		bind_trace_set(list);
		rd->compute_list_dispatch(list, (translucency_size.x + 7) / 8, (translucency_size.y + 7) / 8, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, active.translucency_trace);
		rd->compute_list_bind_uniform_set(list, translucency_uniform_set, 0);
		bind_trace_set(list);
		rd->compute_list_dispatch(list, 8192, 1, 1);
		rd->compute_list_end();
		// GodotNext: nextgi_async_counters (#186). The project pass runs the probe cap instead of
		// reading the marked count back: each group past min(count, cap) returns at once
		// (nextgi_wrc_project), so the output is the same without a synchronous readback.
		const RD::ComputeListID project_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(project_list, active.translucency_project);
		rd->compute_list_bind_uniform_set(project_list, translucency_uniform_set, 0);
		bind_trace_set(project_list);
		rd->compute_list_dispatch(project_list, NEXTGI_TRANSLUCENCY_PROBE_CAP, 1, 1);
		rd->compute_list_end();
	}
	// One shared volume feeds forward translucency and volumetric fog.
	if (volume_uniform_set.is_valid() && translucency_r.is_valid()) {
		const RD::ComputeListID volume_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(volume_list, active.translucency);
		rd->compute_list_bind_uniform_set(volume_list, volume_uniform_set, 0);
		bind_trace_set(volume_list);
		rd->compute_list_dispatch(volume_list, (translucency_size.x + 7) / 8, (translucency_size.y + 7) / 8, 32);
		rd->compute_list_end();
		rd->texture_copy(translucency_r, translucency_history_r, Vector3(), Vector3(), Vector3(translucency_size), 0, 0, 0, 0);
		rd->texture_copy(translucency_g, translucency_history_g, Vector3(), Vector3(), Vector3(translucency_size), 0, 0, 0, 0);
		rd->texture_copy(translucency_b, translucency_history_b, Vector3(), Vector3(), Vector3(translucency_size), 0, 0, 0, 0);
		volume_history_valid = true;
	}
	previous_view_projection = view_proj;
	previous_camera_position = camera;
	previous_camera_forward = forward;
	previous_sky_read = sky_read;
	has_previous_camera = true;
	cursor = (cursor + trace_probes) % NEXTGI_WRC_PROBE_COUNT;
	// GodotNext: nextgi_wrc_shift (#254)
	nextgi_wrc_age_fresh(fresh_frames);
	frame++;
}

// GodotNext: auto_quality_wrc (#259)
bool NextGIWRC::benchmark_swrt_update(const NextGISWRTTrace &p_tracer, const Vector3 &p_camera, uint64_t &r_usec, String &r_failure) {
	r_usec = 0;
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		r_failure = "no rendering device";
		return false;
	}
	NextGIWRC wrc;
	if (!wrc._ensure_textures() || !wrc._ensure_swrt_shader()) {
		r_failure = "the WRC update's software sibling is unavailable";
		return false;
	}
	const Vector3 origin = nextgi_wrc_snap_origin(p_camera, 1.0f);
	NextGIWRCParamsGPU params;
	params.camera[0] = p_camera.x;
	params.camera[1] = p_camera.y;
	params.camera[2] = p_camera.z;
	params.camera[3] = 1.0f;
	params.sky[0] = 0.5f;
	params.sky[1] = 0.6f;
	params.sky[2] = 0.8f;
	params.origin_spacing[0] = origin.x;
	params.origin_spacing[1] = origin.y;
	params.origin_spacing[2] = origin.z;
	params.origin_spacing[3] = 1.0f;
	// Four frames of the update in one dispatch (2048 probes: the earlier kernel's 131072 rays), so
	// the trace stands clear of the round trip's noise on a fast GPU; the result is per frame.
	constexpr uint32_t frames = 4;
	params.probe_base = 0;
	params.probe_count = NEXTGI_WRC_PROBES_PER_FRAME * frames;
	params.history[0] = 0.9f;
	params.history[1] = 1.0f; // Lite: the probe trace without translucency probes.
	rd->buffer_update(wrc.params_buffer, 0, sizeof(params), &params);
	const RID shader = wrc.swrt_shader->shader.version_get_shader(wrc.swrt_shader->version, 0);
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, RID()));
	const RID images[8] = { wrc.radiance, wrc.visibility, wrc.sh_r, wrc.sh_g, wrc.sh_b, wrc.offsets, wrc.mip4, wrc.mip2 };
	for (int i = 0; i < 8; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, i + 1, images[i]));
	}
	for (int binding = 9; binding <= 17; binding++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding, wrc.dummy_buffer));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 18, Vector<RID>({ wrc.sampler, wrc.dummy_tex })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ wrc.sampler, wrc.dummy_tex })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, wrc.params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, wrc.counter_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 35, Vector<RID>({ wrc.sampler, wrc.dummy_tex })));
	const RID set0 = rd->uniform_set_create(uniforms, shader, 0);
	const RID set1 = p_tracer.create_trace_uniform_set(shader, 1, false, nullptr);
	if (!set0.is_valid() || !set1.is_valid()) {
		if (set0.is_valid()) {
			rd->free_rid(set0);
		}
		if (set1.is_valid()) {
			rd->free_rid(set1);
		}
		r_failure = "the benchmark uniform sets failed";
		return false;
	}
	auto dispatch = [&](uint32_t p_probes) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, wrc.swrt_shader->trace);
		rd->compute_list_bind_uniform_set(list, set0, 0);
		rd->compute_list_bind_uniform_set(list, set1, 1);
		rd->compute_list_dispatch(list, p_probes, 1, 1);
		rd->compute_list_end();
	};
	// The warm-up pays for pipeline creation. Each timing is a submit, the dispatch and a
	// blocking readback; the same round trip with one probe is subtracted, so the result is the
	// trace. The fastest of three runs keeps a GPU that is still raising its clock from deciding.
	dispatch(1);
	rd->buffer_get_data(wrc.counter_buffer, 0, 16);
	Vector<uint8_t> result;
	auto fastest_of_three = [&](uint32_t p_probes) {
		uint64_t fastest = UINT64_MAX;
		for (int run = 0; run < 3; run++) {
			const uint64_t started = OS::get_singleton()->get_ticks_usec();
			dispatch(p_probes);
			result = rd->buffer_get_data(wrc.counter_buffer, 0, 16);
			fastest = MIN(fastest, OS::get_singleton()->get_ticks_usec() - started);
		}
		return fastest;
	};
	const uint64_t round_trip = fastest_of_three(1);
	const uint64_t traced = fastest_of_three(NEXTGI_WRC_PROBES_PER_FRAME * frames);
	r_usec = MAX(traced > round_trip ? (traced - round_trip) / frames : uint64_t(0), uint64_t(1));
	rd->free_rid(set0);
	rd->free_rid(set1);
	if (result.size() != 16) {
		r_failure = "the benchmark readback failed";
		return false;
	}
	// The counters add up over every dispatch: a timing only counts for probe rays that ran and
	// hit the fixture's room.
	const uint32_t *counters = reinterpret_cast<const uint32_t *>(result.ptr());
	if (counters[0] == 0 || counters[1] == 0) {
		r_failure = vformat("the benchmark traced %d probe rays with %d hits", counters[0], counters[1]);
		return false;
	}
	return true;
}

// GodotNext: nextgi_wrc_per_view (#253)
NextGIWRC *NextGIWRC::get_view_wrc(RenderSceneBuffersRD *p_render_buffers) {
	if (p_render_buffers == nullptr || !p_render_buffers->has_custom_data(SNAME("nextgi_wrc"))) {
		return nullptr;
	}
	Ref<NextGIWRCView> view = p_render_buffers->get_custom_data(SNAME("nextgi_wrc"));
	return view.is_valid() ? view->wrc : nullptr;
}

NextGIWRC *NextGIWRC::ensure_view_wrc(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL_V(p_render_buffers, nullptr);
	Ref<NextGIWRCView> view;
	if (p_render_buffers->has_custom_data(SNAME("nextgi_wrc"))) {
		view = p_render_buffers->get_custom_data(SNAME("nextgi_wrc"));
	}
	if (view.is_null()) {
		view.instantiate();
		p_render_buffers->set_custom_data(SNAME("nextgi_wrc"), view);
	}
	if (view->wrc == nullptr) {
		view->wrc = memnew(NextGIWRC);
	}
	return view->wrc;
}

void NextGIWRC::free_all_resources() {
	for (NextGIWRC *wrc : nextgi_wrc_live) {
		wrc->free_resources();
	}
	nextgi_wrc_last_updated = nullptr;
	free_shared();
}

void NextGIWRC::free_shared() {
	for (NextGIWRC *wrc : nextgi_wrc_live) {
		wrc->shader = nullptr;
		wrc->swrt_shader = nullptr;
		wrc->_free_uniform_set();
	}
	if (shared_shader != nullptr) {
		if (shared_shader->version.is_valid()) {
			shared_shader->shader.version_free(shared_shader->version);
		}
		memdelete(shared_shader);
		shared_shader = nullptr;
	}
	if (shared_swrt_shader != nullptr) {
		if (shared_swrt_shader->version.is_valid()) {
			shared_swrt_shader->shader.version_free(shared_swrt_shader->version);
		}
		memdelete(shared_swrt_shader);
		shared_swrt_shader = nullptr;
	}
}

uint32_t NextGIWRC::get_live_count() {
	return nextgi_wrc_live.size();
}

void NextGIWRC::fill_views_info(Dictionary &r_info) {
	uint32_t views = 0;
	uint64_t bytes = 0;
	for (const NextGIWRC *wrc : nextgi_wrc_live) {
		if (!wrc->is_ready()) {
			continue;
		}
		views++;
		bytes += nextgi_wrc_bytes() + uint64_t(wrc->translucency_size.x) * uint64_t(wrc->translucency_size.y) * 32 * 6 * 8;
	}
	r_info["wrc_views"] = int64_t(views);
	r_info["wrc_views_bytes"] = int64_t(bytes);
	if (nextgi_wrc_last_updated != nullptr) {
		nextgi_wrc_last_updated->fill_info(r_info);
	}
}

void NextGIWRCView::free_data() {
	// The cache is world-space and does not depend on the view's size, MSAA or scaling, so a
	// reconfigure keeps it; with a full first trace, dropping it would re-trace every probe on
	// each frame of a window resize. It goes with the render buffers.
}

NextGIWRCView::~NextGIWRCView() {
	if (wrc != nullptr) {
		wrc->free_resources();
		memdelete(wrc);
		wrc = nullptr;
	}
}

void NextGIWRC::fill_info(Dictionary &r_info) const {
	// GodotNext: nextgi_async_counters (#186). The readback callback writes the counters.
	MutexLock lock(nextgi_wrc_counter_mutex);
	r_info["wrc_bytes"] = int64_t(nextgi_wrc_bytes());
	r_info["wrc_probes"] = int64_t(NEXTGI_WRC_PROBE_COUNT);
	r_info["wrc_rays"] = int64_t(last_rays);
	r_info["wrc_hits"] = int64_t(last_hits);
	r_info["wrc_sky_misses"] = int64_t(last_sky);
	r_info["wrc_energy_bits"] = int64_t(last_energy_bits);
	r_info["wrc_fog_samples"] = int64_t(fog_samples);
	r_info["wrc_fog_ambient_bits"] = int64_t(fog_ambient_bits);
	r_info["translucency_volume_bytes"] = int64_t(translucency_size.x) * int64_t(translucency_size.y) * 32 * 6 * 8;
	r_info["translucency_marked_probes"] = int64_t(translucency_marked);
	r_info["translucency_probe_rays"] = int64_t(translucency_rays);
	r_info["translucency_probe_hits"] = int64_t(translucency_hits);
	r_info["translucency_history_valid"] = volume_history_valid;
	r_info["translucency_lite"] = !uses_translucency_probes();
	r_info["translucency_sky_energy"] = translucency_sky_energy;
	// GodotNext: ps_swrt_trace (#186). The software sibling's probe-ray SDF counters.
	r_info["wrc_sdf_hits"] = int64_t(last_sdf_hits);
	r_info["wrc_sdf_exhausted"] = int64_t(last_sdf_exhausted);
	r_info["wrc_sdf_near"] = int64_t(last_sdf_near);
}
