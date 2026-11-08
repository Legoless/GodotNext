/**************************************************************************/
/*  nextgeo_debug.cpp                                                     */
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

#include "nextgeo_debug.h"

#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

namespace RendererRD {

namespace {

struct DebugParamsGPU {
	float proj[16];
	float view[16];
	float view_proj[16];
	float camera[4];
	float view_info[4];
};
static_assert(sizeof(DebugParamsGPU) == 224, "DebugParamsGPU must stay 224 bytes.");

struct DebugPushGPU {
	float internal_size[2];
	float output_size[2];
	uint32_t mode;
	uint32_t visible_capacity;
	uint32_t bucket_table_words;
	float cull_sign;
	uint32_t record_count;
	uint32_t mesh_count;
	uint32_t using_nextgeo;
	uint32_t is_orthogonal;
};
static_assert(sizeof(DebugPushGPU) == 48, "DebugPushGPU must stay 48 bytes.");

struct Q14ReducePushGPU {
	uint32_t width;
	uint32_t height;
	uint32_t using_nextgeo;
	uint32_t pad0;
};
static_assert(sizeof(Q14ReducePushGPU) == 16, "Q14ReducePushGPU must stay 16 bytes.");

Mutex s_registry_mutex;
HashMap<uint64_t, NextGeoDebug *> s_registry;
uint64_t s_next_token = 1;

Mutex s_debug_stats_mutex;
double s_fallback_coverage = 0.0;
int64_t s_legacy_drawn_virtualized_pixels = 0;
int64_t s_non_virtual_legacy_opaque_pixels = 0;
int64_t s_virtual_opaque_pixels = 0;
int64_t s_opaque_pixels = 0;
bool s_has_histogram = false;
Array s_triangle_size_histogram;

} // namespace

bool NextGeoDebug::_ensure() {
	if (initialized) {
		return true;
	}
	if (failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}

	debug_shader.initialize(Vector<String>({ "" }));
	debug_version = debug_shader.version_create();
	if (!debug_version.is_valid()) {
		failed = true;
		return false;
	}

	RD::PipelineRasterizationState raster_state;
	RD::PipelineMultisampleState multisample_state;
	RD::PipelineDepthStencilState depth_state;
	RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled(1);
	debug_pipeline.setup(debug_shader.version_get_shader(debug_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, multisample_state, depth_state, blend_state);

	q14_shader.initialize(Vector<String>({ "" }));
	q14_version = q14_shader.version_create();
	if (!q14_version.is_valid()) {
		failed = true;
		return false;
	}
	q14_pipeline = rd->compute_pipeline_create(q14_shader.version_get_shader(q14_version, 0));
	if (!q14_pipeline.is_valid()) {
		failed = true;
		return false;
	}

	Vector<uint8_t> zero_params;
	zero_params.resize(sizeof(DebugParamsGPU));
	zero_params.fill(0);
	debug_params_buffer = rd->storage_buffer_create(sizeof(DebugParamsGPU), zero_params);

	Vector<uint8_t> zero_dummy;
	zero_dummy.resize(64);
	zero_dummy.fill(0);
	dummy_buffer = rd->storage_buffer_create(64, zero_dummy);

	Vector<uint8_t> zero_q14;
	zero_q14.resize(16);
	zero_q14.fill(0);
	q14_scratch = rd->storage_buffer_create(16, zero_q14);

	Vector<uint8_t> zero_hist;
	zero_hist.resize(64);
	zero_hist.fill(0);
	histogram_scratch = rd->storage_buffer_create(64, zero_hist);

	for (int i = 0; i < 3; i++) {
		q14_ring[i] = rd->storage_buffer_create(16, zero_q14);
		q14_ring_pending[i] = false;
		histogram_ring[i] = rd->storage_buffer_create(64, zero_hist);
		histogram_ring_pending[i] = false;
	}
	q14_ring_cursor = 0;
	histogram_ring_cursor = 0;

	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_UINT;
		tf.width = 1;
		tf.height = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> vis_bytes;
		vis_bytes.resize(4);
		const uint32_t empty_vis = 0xFFFFFFFFu;
		memcpy(vis_bytes.ptrw(), &empty_vis, 4);
		Vector<Vector<uint8_t>> vis_data;
		vis_data.push_back(vis_bytes);
		dummy_vis_tex = rd->texture_create(tf, RD::TextureView(), vis_data);
	}

	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8_UINT;
		tf.width = 1;
		tf.height = 1;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<uint8_t> fb_bytes;
		fb_bytes.resize(1);
		fb_bytes.write[0] = 0u;
		Vector<Vector<uint8_t>> fb_data;
		fb_data.push_back(fb_bytes);
		dummy_fallback_tex = rd->texture_create(tf, RD::TextureView(), fb_data);
	}

	RD::SamplerState sampler_state;
	sampler_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	nearest_sampler = rd->sampler_create(sampler_state);

	initialized = true;
	return true;
}

void NextGeoDebug::_release_fallback_texture() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && fallback_class_tex.is_valid()) {
		rd->free_rid(fallback_class_tex);
	}
	fallback_class_tex = RID();
	fallback_size = Size2i();
}

void NextGeoDebug::_release_rt_proxy_depth() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && rt_proxy_depth_tex.is_valid()) {
		rd->free_rid(rt_proxy_depth_tex);
	}
	rt_proxy_depth_tex = RID();
	rt_proxy_size = Size2i();
}

RID NextGeoDebug::prepare_fallback_fb(const Size2i &p_internal_size, RID p_depth_tex) {
	if (!_ensure() || !p_depth_tex.is_valid()) {
		return RID();
	}
	RenderingDevice *rd = RD::get_singleton();
	FramebufferCacheRD *fb_cache = FramebufferCacheRD::get_singleton();
	if (rd == nullptr || fb_cache == nullptr) {
		return RID();
	}
	const Size2i sz(MAX(p_internal_size.width, 1), MAX(p_internal_size.height, 1));
	if (!fallback_class_tex.is_valid() || fallback_size != sz) {
		_release_fallback_texture();
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8_UINT;
		tf.width = uint32_t(sz.width);
		tf.height = uint32_t(sz.height);
		tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		fallback_class_tex = rd->texture_create(tf, RD::TextureView());
		if (!fallback_class_tex.is_valid()) {
			return RID();
		}
		fallback_size = sz;
	}
	return fb_cache->get_cache(fallback_class_tex, p_depth_tex);
}

void NextGeoDebug::clear_fallback_texture() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && fallback_class_tex.is_valid()) {
		rd->texture_clear(fallback_class_tex, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
}

RID NextGeoDebug::prepare_rt_proxy_depth_fb(const Size2i &p_internal_size) {
	if (!_ensure()) {
		return RID();
	}
	RenderingDevice *rd = RD::get_singleton();
	FramebufferCacheRD *fb_cache = FramebufferCacheRD::get_singleton();
	if (rd == nullptr || fb_cache == nullptr) {
		return RID();
	}
	const Size2i sz(MAX(p_internal_size.width, 1), MAX(p_internal_size.height, 1));
	if (!rt_proxy_depth_tex.is_valid() || rt_proxy_size != sz) {
		_release_rt_proxy_depth();
		RD::TextureFormat tf;
		tf.format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D32_SFLOAT, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT) ? RD::DATA_FORMAT_D32_SFLOAT : RD::DATA_FORMAT_D24_UNORM_S8_UINT;
		tf.width = uint32_t(sz.width);
		tf.height = uint32_t(sz.height);
		tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		rt_proxy_depth_tex = rd->texture_create(tf, RD::TextureView());
		if (!rt_proxy_depth_tex.is_valid()) {
			return RID();
		}
		rt_proxy_size = sz;
	}
	return fb_cache->get_cache(rt_proxy_depth_tex);
}

void NextGeoDebug::record_scene_params(const NextGeoDebugSceneParams &p_params) {
	if (!_ensure()) {
		return;
	}
	last_scene_params = p_params;
	has_scene_params = true;

	DebugParamsGPU dp = {};
	MaterialStorage::store_camera(p_params.proj, dp.proj);
	MaterialStorage::store_transform(p_params.view, dp.view);
	MaterialStorage::store_camera(p_params.view_proj, dp.view_proj);
	dp.camera[0] = float(p_params.cam_pos.x);
	dp.camera[1] = float(p_params.cam_pos.y);
	dp.camera[2] = float(p_params.cam_pos.z);
	dp.camera[3] = p_params.max_pixel_error;
	dp.view_info[0] = float(MAX(p_params.internal_size.width, 1));
	dp.view_info[1] = float(MAX(p_params.internal_size.height, 1));
	dp.view_info[2] = float(MAX(p_params.proj.get_z_near(), 1e-4));
	dp.view_info[3] = p_params.lod_bias;
	RD::get_singleton()->buffer_update(debug_params_buffer, 0, sizeof(DebugParamsGPU), &dp);
}

void NextGeoDebug::dispatch_q14_reduce(RID p_visibility_tex) {
	if (!_ensure() || !fallback_class_tex.is_valid()) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return;
	}

	uint32_t slot = q14_ring_cursor % 3u;
	q14_ring_cursor++;
	bool do_readback = false;
	RID counts_buf = q14_scratch;
	if (!q14_ring_pending[slot] && q14_ring[slot].is_valid()) {
		counts_buf = q14_ring[slot];
		do_readback = true;
	}

	const uint32_t zero_counts[4] = { 0u, 0u, 0u, 0u };
	rd->buffer_update(counts_buf, 0, sizeof(zero_counts), zero_counts);

	const RID vis_tex = (last_scene_params.using_nextgeo && p_visibility_tex.is_valid()) ? p_visibility_tex : dummy_vis_tex;
	RID shader = q14_shader.version_get_shader(q14_version, 0);
	RD::Uniform u0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, fallback_class_tex }));
	RD::Uniform u1(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, vis_tex }));
	RD::Uniform u2(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, counts_buf);
	RID set = uniform_set_cache->get_cache(shader, 0, u0, u1, u2);
	if (!set.is_valid()) {
		return;
	}

	const uint32_t width = uint32_t(MAX(fallback_size.width, 1));
	const uint32_t height = uint32_t(MAX(fallback_size.height, 1));
	Q14ReducePushGPU push = {};
	push.width = width;
	push.height = height;
	push.using_nextgeo = (last_scene_params.using_nextgeo && p_visibility_tex.is_valid()) ? 1u : 0u;
	push.pad0 = 0u;

	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, q14_pipeline);
	rd->compute_list_bind_uniform_set(cl, set, 0);
	rd->compute_list_set_push_constant(cl, &push, sizeof(push));
	rd->compute_list_dispatch(cl, (width + 7u) / 8u, (height + 7u) / 8u, 1u);
	rd->compute_list_end();

	if (do_readback) {
		uint64_t token = 0;
		{
			MutexLock lock(s_registry_mutex);
			token = s_next_token++;
			s_registry.insert(token, this);
		}
		pending_tokens.push_back(token);
		q14_ring_pending[slot] = true;
		if (rd->buffer_get_data_async(counts_buf, callable_mp_static(&NextGeoDebug::_readback_q14_static).bind(token, slot), 0, 4 * sizeof(uint32_t)) != OK) {
			q14_ring_pending[slot] = false;
			pending_tokens.erase(token);
			MutexLock lock(s_registry_mutex);
			s_registry.erase(token);
		}
	}
}

bool NextGeoDebug::draw_debug_view(RSE::ViewportDebugDraw p_mode, RID p_visibility_tex, RID p_depth_tex, RID p_dest_fb, const Size2i &p_internal_size, const Size2i &p_output_size) {
	if (!_ensure() || !p_depth_tex.is_valid() || !p_dest_fb.is_valid()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return false;
	}

	uint32_t shader_mode = 0u;
	switch (p_mode) {
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS:
			shader_mode = 0u;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR:
			shader_mode = 1u;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RESIDENCY:
			shader_mode = 2u;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VISIBILITY_ID:
			shader_mode = 3u;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK:
			shader_mode = 4u;
			break;
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE:
			shader_mode = 5u;
			break;
		default:
			return false;
	}

	uint32_t hist_slot = 0u;
	bool do_hist_readback = false;
	RID active_hist_buf = histogram_scratch;
	if (shader_mode == 5u) {
		hist_slot = histogram_ring_cursor % 3u;
		histogram_ring_cursor++;
		if (!histogram_ring_pending[hist_slot] && histogram_ring[hist_slot].is_valid()) {
			active_hist_buf = histogram_ring[hist_slot];
			do_hist_readback = true;
		}
		const uint32_t zero_bins[16] = {};
		rd->buffer_update(active_hist_buf, 0, sizeof(zero_bins), zero_bins);
	}

	RendererVirtualMeshStorage *vms = RSG::virtual_mesh_storage;
	const bool has_vms = vms != nullptr;
	const bool using_nextgeo = has_scene_params && last_scene_params.using_nextgeo && p_visibility_tex.is_valid() && last_scene_params.visible_clusters_buf.is_valid() && has_vms;

	const RID vis_tex = using_nextgeo ? p_visibility_tex : dummy_vis_tex;
	const RID fb_tex = fallback_class_tex.is_valid() ? fallback_class_tex : dummy_fallback_tex;
	const RID rec_buf = (using_nextgeo && vms->get_record_buffer().is_valid()) ? vms->get_record_buffer() : dummy_buffer;
	const RID mesh_rec_buf = (using_nextgeo && vms->get_mesh_record_buffer().is_valid()) ? vms->get_mesh_record_buffer() : dummy_buffer;
	const RID page_buf = (using_nextgeo && vms->get_page_buffer().is_valid()) ? vms->get_page_buffer() : dummy_buffer;
	const RID cluster_buf = (using_nextgeo && vms->get_cluster_buffer().is_valid()) ? vms->get_cluster_buffer() : dummy_buffer;
	const RID vis_clusters_buf = (using_nextgeo && last_scene_params.visible_clusters_buf.is_valid()) ? last_scene_params.visible_clusters_buf : dummy_buffer;
	const RID bucket_buf = (using_nextgeo && last_scene_params.bucket_table_buf.is_valid()) ? last_scene_params.bucket_table_buf : dummy_buffer;
	const RID dag_buf = (using_nextgeo && vms->get_dag_buffer().is_valid()) ? vms->get_dag_buffer() : dummy_buffer;
	const RID residency_buf = (using_nextgeo && vms->get_residency_buffer().is_valid()) ? vms->get_residency_buffer() : dummy_buffer;

	LocalVector<RD::Uniform> uniforms;
	uniforms.reserve(13);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, vis_tex })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_depth_tex })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, fb_tex })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, rec_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, mesh_rec_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, page_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, cluster_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, vis_clusters_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, bucket_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, dag_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, residency_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, debug_params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, active_hist_buf));

	RID shader = debug_shader.version_get_shader(debug_version, 0);
	RID set = uniform_set_cache->get_cache_vec(shader, 0, uniforms);
	if (!set.is_valid()) {
		return false;
	}

	DebugPushGPU push = {};
	push.internal_size[0] = float(MAX(p_internal_size.width, 1));
	push.internal_size[1] = float(MAX(p_internal_size.height, 1));
	push.output_size[0] = float(MAX(p_output_size.width, 1));
	push.output_size[1] = float(MAX(p_output_size.height, 1));
	push.mode = shader_mode;
	push.visible_capacity = using_nextgeo ? last_scene_params.visible_capacity : 0u;
	push.bucket_table_words = (using_nextgeo && last_scene_params.bucket_table_buf.is_valid()) ? last_scene_params.bucket_table_words : 0u;
	push.cull_sign = has_scene_params ? last_scene_params.cull_sign : 1.0f;
	push.record_count = using_nextgeo ? vms->get_record_count() : 0u;
	push.mesh_count = using_nextgeo ? vms->get_mesh_record_count() : 0u;
	push.using_nextgeo = using_nextgeo ? 1u : 0u;
	push.is_orthogonal = (has_scene_params && last_scene_params.proj.is_orthogonal()) ? 1u : 0u;

	RD::DrawListID dl = rd->draw_list_begin(p_dest_fb);
	RID pipeline = debug_pipeline.get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_dest_fb), false, rd->draw_list_get_current_pass());
	rd->draw_list_bind_render_pipeline(dl, pipeline);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_set_push_constant(dl, &push, sizeof(push));
	rd->draw_list_draw(dl, false, 1u, 3u);
	rd->draw_list_end();

	if (do_hist_readback) {
		uint64_t token = 0;
		{
			MutexLock lock(s_registry_mutex);
			token = s_next_token++;
			s_registry.insert(token, this);
		}
		pending_tokens.push_back(token);
		histogram_ring_pending[hist_slot] = true;
		if (rd->buffer_get_data_async(active_hist_buf, callable_mp_static(&NextGeoDebug::_readback_histogram_static).bind(token, hist_slot), 0, 16 * sizeof(uint32_t)) != OK) {
			histogram_ring_pending[hist_slot] = false;
			pending_tokens.erase(token);
			MutexLock lock(s_registry_mutex);
			s_registry.erase(token);
		}
	}

	return true;
}

void NextGeoDebug::_readback_q14_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoDebug *dbg = nullptr;
	{
		MutexLock lock(s_registry_mutex);
		NextGeoDebug **found = s_registry.getptr(p_token);
		if (found != nullptr) {
			dbg = *found;
			s_registry.erase(p_token);
		}
	}
	if (dbg != nullptr) {
		dbg->pending_tokens.erase(p_token);
		dbg->_publish_q14(p_data, p_ring_slot);
	}
}

void NextGeoDebug::_publish_q14(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot >= 3) {
		return;
	}
	q14_ring_pending[p_ring_slot] = false;
	if (p_data.size() < 12) {
		return;
	}
	const uint32_t *counts = reinterpret_cast<const uint32_t *>(p_data.ptr());
	const uint64_t n1 = uint64_t(counts[0]);
	const uint64_t n2 = uint64_t(counts[1]);
	const uint64_t n3 = uint64_t(counts[2]);
	const uint64_t opaque_total = n1 + n2 + n3;
	const double coverage = opaque_total > 0 ? double(n1) / double(opaque_total) : 0.0;

	MutexLock lock(s_debug_stats_mutex);
	s_fallback_coverage = coverage;
	s_legacy_drawn_virtualized_pixels = int64_t(n1);
	s_non_virtual_legacy_opaque_pixels = int64_t(n2);
	s_virtual_opaque_pixels = int64_t(n3);
	s_opaque_pixels = int64_t(opaque_total);
}

void NextGeoDebug::_readback_histogram_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot) {
	NextGeoDebug *dbg = nullptr;
	{
		MutexLock lock(s_registry_mutex);
		NextGeoDebug **found = s_registry.getptr(p_token);
		if (found != nullptr) {
			dbg = *found;
			s_registry.erase(p_token);
		}
	}
	if (dbg != nullptr) {
		dbg->pending_tokens.erase(p_token);
		dbg->_publish_histogram(p_data, p_ring_slot);
	}
}

void NextGeoDebug::_publish_histogram(const PackedByteArray &p_data, uint32_t p_ring_slot) {
	if (p_ring_slot >= 3) {
		return;
	}
	histogram_ring_pending[p_ring_slot] = false;
	if (p_data.size() < 64) {
		return;
	}
	const uint32_t *bins = reinterpret_cast<const uint32_t *>(p_data.ptr());
	Array hist;
	hist.resize(16);
	for (int i = 0; i < 16; i++) {
		hist[i] = int64_t(bins[i]);
	}

	MutexLock lock(s_debug_stats_mutex);
	s_has_histogram = true;
	s_triangle_size_histogram = hist;
}

void NextGeoDebug::fill_debug_stats(Dictionary &r_stats) {
	MutexLock lock(s_debug_stats_mutex);
	r_stats["fallback_coverage"] = s_fallback_coverage;
	r_stats["legacy_drawn_virtualized_pixels"] = s_legacy_drawn_virtualized_pixels;
	r_stats["non_virtual_legacy_opaque_pixels"] = s_non_virtual_legacy_opaque_pixels;
	r_stats["virtual_opaque_pixels"] = s_virtual_opaque_pixels;
	r_stats["opaque_pixels"] = s_opaque_pixels;
	if (s_has_histogram) {
		r_stats["triangle_size_histogram"] = s_triangle_size_histogram.duplicate();
	} else {
		Array zero_hist;
		zero_hist.resize(16);
		for (int i = 0; i < 16; i++) {
			zero_hist[i] = int64_t(0);
		}
		r_stats["triangle_size_histogram"] = zero_hist;
	}
}

void NextGeoDebug::reset_debug_stats() {
	MutexLock lock(s_debug_stats_mutex);
	s_fallback_coverage = 0.0;
	s_legacy_drawn_virtualized_pixels = 0;
	s_non_virtual_legacy_opaque_pixels = 0;
	s_virtual_opaque_pixels = 0;
	s_opaque_pixels = 0;
	s_has_histogram = false;
	s_triangle_size_histogram.clear();
}

void NextGeoDebug::free_resources() {
	{
		MutexLock lock(s_registry_mutex);
		for (const uint64_t &token : pending_tokens) {
			s_registry.erase(token);
		}
	}
	pending_tokens.clear();
	for (int i = 0; i < 3; i++) {
		q14_ring_pending[i] = false;
		histogram_ring_pending[i] = false;
	}
	has_scene_params = false;

	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}

	_release_fallback_texture();
	_release_rt_proxy_depth();
	debug_pipeline.clear();

	if (debug_version.is_valid()) {
		debug_shader.version_free(debug_version);
		debug_version = RID();
	}
	if (q14_version.is_valid()) {
		q14_shader.version_free(q14_version);
		q14_version = RID();
	}
	q14_pipeline = RID();

	for (RID *rid : { &debug_params_buffer, &dummy_buffer, &dummy_vis_tex, &dummy_fallback_tex, &nearest_sampler, &q14_scratch, &q14_ring[0], &q14_ring[1], &q14_ring[2], &histogram_scratch, &histogram_ring[0], &histogram_ring[1], &histogram_ring[2] }) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}

	reset_debug_stats();
	initialized = false;
}

} // namespace RendererRD
