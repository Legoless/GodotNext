/**************************************************************************/
/*  nextgeo_resolve.cpp                                                   */
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

#include "nextgeo_resolve.h"

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <atomic>

namespace RendererSceneRenderImplementation {

namespace {

struct ClassifyPush {
	uint32_t viewport_width = 0;
	uint32_t viewport_height = 0;
	uint32_t bucket_count = 0;
	uint32_t max_tiles = 0;
};

static_assert(sizeof(ClassifyPush) == 16);
static_assert(sizeof(NextGeoResolve::ResolvePush) == 32);

// GodotNext: nextgeo_shading_bins (#158). shading_binning.glsl's PushParams.
struct BinningPush {
	uint32_t viewport_width = 0;
	uint32_t viewport_height = 0;
	uint32_t bin_count = 0;
	uint32_t pixel_capacity = 0;
	uint32_t region_min[2] = { 0, 0 };
	uint32_t region_max[2] = { 0, 0 };
};

static_assert(sizeof(BinningPush) == 32);

enum BinningPass {
	BINNING_CLEAR,
	BINNING_COUNT,
	BINNING_PREFIX,
	BINNING_SCATTER,
	BINNING_PASS_MAX,
};

std::atomic<uint32_t> s_last_published_bucket_count{ 0 };
std::atomic<uint32_t> s_last_published_stage_a_draws{ 0 };
std::atomic<uint32_t> s_last_published_stage_b_draws{ 0 };
// GodotNext: nextgeo_shading_bins (#158). Buckets each stage shaded through compute bins.
std::atomic<uint32_t> s_last_published_stage_a_compute{ 0 };
std::atomic<uint32_t> s_last_published_stage_b_compute{ 0 };

uint32_t round_up_pow2(uint32_t p_value, uint32_t p_min) {
	uint32_t v = MAX(p_value, p_min);
	v--;
	v |= v >> 1u;
	v |= v >> 2u;
	v |= v >> 4u;
	v |= v >> 8u;
	v |= v >> 16u;
	v++;
	return v;
}

} // namespace

uint32_t NextGeoResolve::get_last_published_bucket_count() {
	return s_last_published_bucket_count.load(std::memory_order_relaxed);
}

uint32_t NextGeoResolve::get_last_published_stage_a_draws() {
	return s_last_published_stage_a_draws.load(std::memory_order_relaxed);
}

uint32_t NextGeoResolve::get_last_published_stage_b_draws() {
	return s_last_published_stage_b_draws.load(std::memory_order_relaxed);
}

// GodotNext: nextgeo_shading_bins (#158)
uint32_t NextGeoResolve::get_last_published_stage_a_compute() {
	return s_last_published_stage_a_compute.load(std::memory_order_relaxed);
}

uint32_t NextGeoResolve::get_last_published_stage_b_compute() {
	return s_last_published_stage_b_compute.load(std::memory_order_relaxed);
}

uint32_t NextGeoResolve::get_compute_bucket_count() const {
	uint32_t count = 0;
	for (uint32_t i = 0; i < buckets.size(); i++) {
		if (is_bucket_compute(i)) {
			count++;
		}
	}
	return count;
}

// GodotNext: nextgeo_vertex_offset (#164).
bool NextGeoResolve::has_vertex_offset_buckets() const {
	for (uint32_t i = 0; i < buckets.size(); i++) {
		if (buckets[i].shader != nullptr && buckets[i].shader->uses_vertex) {
			return true;
		}
	}
	return false;
}

bool NextGeoResolve::compute_target_storable(RID p_texture, RD::DataFormat p_format) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_texture.is_null() || !rd->texture_is_valid(p_texture)) {
		return false;
	}
	const RD::TextureFormat tf = rd->texture_get_format(p_texture);
	return tf.format == p_format && tf.samples == RD::TEXTURE_SAMPLES_1 && tf.texture_type == RD::TEXTURE_TYPE_2D && (tf.usage_bits & RD::TEXTURE_USAGE_STORAGE_BIT) != 0 &&
			rd->texture_is_format_supported_for_usage(p_format, RD::TEXTURE_USAGE_STORAGE_BIT);
}

void NextGeoResolve::begin_frame() {
	buckets.clear();
	bucket_map.clear();
	current_max_tiles = 0;
	binned = false; // GodotNext: nextgeo_shading_bins (#158)
}

uint32_t NextGeoResolve::register_bucket(SceneShaderForwardClustered::MaterialData *p_material, SceneShaderForwardClustered::ShaderData *p_shader, RID p_material_uniform_set, uint32_t p_material_feedback_index, SceneShaderForwardClustered::ShaderSpecialization p_specialization) {
	if (p_material == nullptr || p_shader == nullptr) {
		return BUCKET_INVALID;
	}
	MaterialBucketKey key;
	key.material = p_material;
	key.shader = p_shader;
	key.material_uniform_set = p_material_uniform_set;
	key.spec_packed_0 = p_specialization.packed_0;
	key.spec_packed_1 = p_specialization.packed_1;

	const uint32_t *existing = bucket_map.getptr(key);
	if (existing != nullptr) {
		if (buckets[*existing].material_feedback_index == UINT32_MAX && p_material_feedback_index != UINT32_MAX) {
			buckets[*existing].material_feedback_index = p_material_feedback_index;
		}
		return *existing;
	}

	if (buckets.size() >= BUCKET_INVALID) {
		return BUCKET_INVALID;
	}

	const uint32_t slot = buckets.size();
	MaterialBucket bucket;
	bucket.material = p_material;
	bucket.shader = p_shader;
	bucket.material_uniform_set = p_material_uniform_set;
	bucket.material_feedback_index = p_material_feedback_index;
	bucket.specialization = p_specialization;
	buckets.push_back(bucket);
	bucket_map[key] = slot;
	return slot;
}

bool NextGeoResolve::_ensure() {
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

#ifdef REAL_T_IS_DOUBLE
	const String base_defines = "\n#define REAL_T_IS_DOUBLE\n";
#else
	const String base_defines = "";
#endif
	classify_shader.initialize(Vector<String>({
			base_defines + "\n#define INIT_INDIRECT\n",
			base_defines,
	}));
	classify_version = classify_shader.version_create();
	if (!classify_version.is_valid()) {
		failed = true;
		return false;
	}
	init_indirect_pipeline = rd->compute_pipeline_create(classify_shader.version_get_shader(classify_version, 0));
	classify_pipeline = rd->compute_pipeline_create(classify_shader.version_get_shader(classify_version, 1));
	if (!init_indirect_pipeline.is_valid() || !classify_pipeline.is_valid()) {
		failed = true;
		return false;
	}

	Vector<uint8_t> zero_buf;
	zero_buf.resize(256);
	zero_buf.fill(0);
	dummy_buffer = rd->storage_buffer_create(256, zero_buf);

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_UINT;
	tf.width = 1;
	tf.height = 1;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> vis_fill;
	vis_fill.resize(4);
	const uint32_t invalid_vis = 0xFFFFFFFFu;
	memcpy(vis_fill.ptrw(), &invalid_vis, sizeof(uint32_t));
	Vector<Vector<uint8_t>> tex_data;
	tex_data.push_back(vis_fill);
	dummy_vis_tex = rd->texture_create(tf, RD::TextureView(), tex_data);

	initialized = dummy_buffer.is_valid() && dummy_vis_tex.is_valid();
	if (!initialized) {
		failed = true;
	}
	return initialized;
}

void NextGeoResolve::_ensure_classify_buffers(uint32_t p_bucket_count, uint32_t p_max_tiles) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	const uint32_t wanted_buckets = round_up_pow2(p_bucket_count, 16u);
	if (!indirect_args_buffer.is_valid() || wanted_buckets > indirect_bucket_capacity) {
		if (indirect_args_buffer.is_valid()) {
			rd->free_rid(indirect_args_buffer);
		}
		indirect_bucket_capacity = wanted_buckets;
		indirect_args_buffer = rd->storage_buffer_create(indirect_bucket_capacity * 16u, Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}

	const uint64_t wanted_words_64 = uint64_t(wanted_buckets) * uint64_t(MAX(p_max_tiles, 1u));
	const uint32_t wanted_words = round_up_pow2(uint32_t(MIN<uint64_t>(wanted_words_64, 0x3FFFFFFFull)), 256u);
	if (!tile_list_buffer.is_valid() || wanted_words > tile_list_word_capacity) {
		if (tile_list_buffer.is_valid()) {
			rd->free_rid(tile_list_buffer);
		}
		tile_list_word_capacity = wanted_words;
		tile_list_buffer = rd->storage_buffer_create(tile_list_word_capacity * sizeof(uint32_t));
	}
}

RID NextGeoResolve::get_instance_data_buffer() {
	if (!_ensure()) {
		return RID();
	}
	return instance_data_buffer.is_valid() ? instance_data_buffer : dummy_buffer;
}

RID NextGeoResolve::get_bucket_table_buffer() {
	if (!_ensure()) {
		return RID();
	}
	return bucket_table_buffer.is_valid() ? bucket_table_buffer : dummy_buffer;
}

bool NextGeoResolve::upload_tables(const uint32_t *p_bucket_table, uint32_t p_bucket_table_words, const void *p_instance_data, uint32_t p_instance_data_bytes) {
	if (!_ensure()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	const uint32_t bucket_bytes = p_bucket_table_words * sizeof(uint32_t);
	if (bucket_bytes > 0 && p_bucket_table != nullptr) {
		const uint32_t wanted = round_up_pow2(bucket_bytes, 256u);
		if (!bucket_table_buffer.is_valid() || wanted > bucket_table_byte_capacity) {
			if (bucket_table_buffer.is_valid()) {
				rd->free_rid(bucket_table_buffer);
			}
			bucket_table_byte_capacity = wanted;
			bucket_table_buffer = rd->storage_buffer_create(bucket_table_byte_capacity);
		}
		if (bucket_table_buffer.is_valid()) {
			rd->buffer_update(bucket_table_buffer, 0, bucket_bytes, p_bucket_table);
		}
	}

	if (p_instance_data_bytes > 0 && p_instance_data != nullptr) {
		const uint32_t wanted = round_up_pow2(p_instance_data_bytes, 256u);
		if (!instance_data_buffer.is_valid() || wanted > instance_data_byte_capacity) {
			if (instance_data_buffer.is_valid()) {
				rd->free_rid(instance_data_buffer);
			}
			instance_data_byte_capacity = wanted;
			instance_data_buffer = rd->storage_buffer_create(instance_data_byte_capacity);
		}
		if (instance_data_buffer.is_valid()) {
			rd->buffer_update(instance_data_buffer, 0, p_instance_data_bytes, p_instance_data);
		}
	}

	return true;
}

bool NextGeoResolve::classify(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, const Size2i &p_viewport_size) {
	s_last_published_bucket_count.store(buckets.size(), std::memory_order_relaxed);
	s_last_published_stage_a_draws.store(0, std::memory_order_relaxed);
	s_last_published_stage_b_draws.store(0, std::memory_order_relaxed);
	// GodotNext: nextgeo_shading_bins (#158)
	s_last_published_stage_a_compute.store(0, std::memory_order_relaxed);
	s_last_published_stage_b_compute.store(0, std::memory_order_relaxed);
	binned = false;

	if (buckets.is_empty() || p_storage == nullptr || !p_visibility_tex.is_valid() || !p_visible_clusters_buf.is_valid()) {
		return false;
	}
	if (!_ensure()) {
		return false;
	}

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return false;
	}

	const uint32_t width = uint32_t(MAX(p_viewport_size.x, 1));
	const uint32_t height = uint32_t(MAX(p_viewport_size.y, 1));
	const uint32_t tiles_x = (width + 7u) / 8u;
	const uint32_t tiles_y = (height + 7u) / 8u;
	current_max_tiles = MAX(tiles_x * tiles_y, 1u);
	_ensure_classify_buffers(buckets.size(), current_max_tiles);
	if (!indirect_args_buffer.is_valid() || !tile_list_buffer.is_valid()) {
		return false;
	}

	const RID record_buf = p_storage->get_record_buffer().is_valid() ? p_storage->get_record_buffer() : dummy_buffer;
	const RID mesh_buf = p_storage->get_mesh_record_buffer().is_valid() ? p_storage->get_mesh_record_buffer() : dummy_buffer;
	const RID cluster_buf = p_storage->get_cluster_buffer().is_valid() ? p_storage->get_cluster_buffer() : dummy_buffer;
	const RID bucket_buf = bucket_table_buffer.is_valid() ? bucket_table_buffer : dummy_buffer;

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, p_visibility_tex));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_visible_clusters_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, record_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, mesh_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, cluster_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, bucket_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, indirect_args_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, tile_list_buffer));

	ClassifyPush push;
	push.viewport_width = width;
	push.viewport_height = height;
	push.bucket_count = buckets.size();
	push.max_tiles = current_max_tiles;

	RID init_set = uniform_set_cache->get_cache_vec(classify_shader.version_get_shader(classify_version, 0), 0, uniforms);
	if (!init_set.is_valid()) {
		return false;
	}
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, init_indirect_pipeline);
	rd->compute_list_bind_uniform_set(cl, init_set, 0);
	rd->compute_list_set_push_constant(cl, &push, sizeof(push));
	rd->compute_list_dispatch(cl, (buckets.size() + 63u) / 64u, 1u, 1u);
	rd->compute_list_end();

	RID classify_set = uniform_set_cache->get_cache_vec(classify_shader.version_get_shader(classify_version, 1), 0, uniforms);
	if (!classify_set.is_valid()) {
		return false;
	}
	cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, classify_pipeline);
	rd->compute_list_bind_uniform_set(cl, classify_set, 0);
	rd->compute_list_set_push_constant(cl, &push, sizeof(push));
	rd->compute_list_dispatch(cl, tiles_x, tiles_y, 1u);
	rd->compute_list_end();

	return true;
}

RID NextGeoResolve::_build_resolve_uniform_set(RID p_shader_rid, RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (uniform_set_cache == nullptr || !p_shader_rid.is_valid() || p_storage == nullptr) {
		return RID();
	}

	const RID vis_tex = p_visibility_tex.is_valid() ? p_visibility_tex : dummy_vis_tex;
	const RID clusters_buf = p_visible_clusters_buf.is_valid() ? p_visible_clusters_buf : dummy_buffer;
	const RID record_buf = p_storage->get_record_buffer().is_valid() ? p_storage->get_record_buffer() : dummy_buffer;
	const RID mesh_buf = p_storage->get_mesh_record_buffer().is_valid() ? p_storage->get_mesh_record_buffer() : dummy_buffer;
	const RID cluster_buf = p_storage->get_cluster_buffer().is_valid() ? p_storage->get_cluster_buffer() : dummy_buffer;
	const RID page_buf = p_storage->get_page_buffer().is_valid() ? p_storage->get_page_buffer() : dummy_buffer;
	const RID bucket_buf = bucket_table_buffer.is_valid() ? bucket_table_buffer : dummy_buffer;
	const RID tiles_buf = tile_list_buffer.is_valid() ? tile_list_buffer : dummy_buffer;
	const RID inst_buf = instance_data_buffer.is_valid() ? instance_data_buffer : dummy_buffer;
	const RID spline_buf = p_storage->get_spline_buffer().is_valid() ? p_storage->get_spline_buffer() : dummy_buffer;

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, vis_tex));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, clusters_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, record_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, mesh_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, cluster_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, page_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bucket_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, tiles_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, inst_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, spline_buf));

	return uniform_set_cache->get_cache_vec(p_shader_rid, RenderForwardClustered::TRANSFORMS_UNIFORM_SET, uniforms);
}

// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7).
bool NextGeoResolve::_ensure_binning() {
	if (binning_initialized) {
		return true;
	}
	if (binning_failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	binning_shader.initialize(Vector<String>({
			"\n#define MODE_CLEAR\n",
			"\n#define MODE_COUNT\n",
			"\n#define MODE_PREFIX\n",
			"\n#define MODE_SCATTER\n",
	}));
	binning_version = binning_shader.version_create();
	if (!binning_version.is_valid()) {
		binning_failed = true;
		return false;
	}
	for (uint32_t i = 0; i < BINNING_PASS_MAX; i++) {
		binning_pipelines[i] = rd->compute_pipeline_create(binning_shader.version_get_shader(binning_version, i));
		if (!binning_pipelines[i].is_valid()) {
			binning_failed = true;
			return false;
		}
	}
	binning_initialized = true;
	return true;
}

void NextGeoResolve::_ensure_bin_buffers(uint32_t p_bin_count, uint32_t p_pixel_capacity) {
	RenderingDevice *rd = RD::get_singleton();
	const uint32_t wanted_bins = round_up_pow2(p_bin_count, 16u);
	if (!shading_bin_state_buffer.is_valid() || wanted_bins > shading_bin_capacity) {
		for (RID *buffer : { &shading_bucket_bins_buffer, &shading_bin_state_buffer, &shading_args_buffer }) {
			if (buffer->is_valid()) {
				rd->free_rid(*buffer);
				*buffer = RID();
			}
		}
		shading_bin_capacity = wanted_bins;
		shading_bucket_bins_buffer = rd->storage_buffer_create(shading_bin_capacity * sizeof(uint32_t));
		shading_bin_state_buffer = rd->storage_buffer_create(shading_bin_capacity * RendererRD::NEXTGEO_SHADING_BIN_STATE_WORDS * sizeof(uint32_t));
		shading_args_buffer = rd->storage_buffer_create(shading_bin_capacity * RendererRD::NEXTGEO_SHADING_ARGS_WORDS * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	const uint32_t wanted_pixels = round_up_pow2(MAX(p_pixel_capacity, 64u), 256u);
	if (!shading_pixel_buffer.is_valid() || wanted_pixels > shading_pixel_capacity) {
		if (shading_pixel_buffer.is_valid()) {
			rd->free_rid(shading_pixel_buffer);
		}
		shading_pixel_capacity = wanted_pixels;
		shading_pixel_buffer = rd->storage_buffer_create(shading_pixel_capacity * sizeof(uint32_t));
	}
}

bool NextGeoResolve::bin(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, const Size2i &p_viewport_size, const Rect2i &p_region) {
	binned = false;
	shading_bucket_bins.clear();
	if (buckets.is_empty() || p_storage == nullptr || !p_visibility_tex.is_valid() || !p_visible_clusters_buf.is_valid() || !bucket_table_buffer.is_valid()) {
		return false;
	}
	if (!_ensure() || !_ensure_binning()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (rd == nullptr || uniform_set_cache == nullptr) {
		return false;
	}

	const uint32_t width = uint32_t(CLAMP(p_viewport_size.x, 1, int(RendererRD::NEXTGEO_SHADING_MAX_EXTENT)));
	const uint32_t height = uint32_t(CLAMP(p_viewport_size.y, 1, int(RendererRD::NEXTGEO_SHADING_MAX_EXTENT)));
	_ensure_bin_buffers(buckets.size(), width * height);
	if (!shading_bucket_bins_buffer.is_valid() || !shading_bin_state_buffer.is_valid() || !shading_args_buffer.is_valid() || !shading_pixel_buffer.is_valid()) {
		return false;
	}
	// Bin i is bucket i, for the buckets whose material shades in compute; the rest keep the tile quads.
	shading_bucket_bins.resize(buckets.size());
	for (uint32_t i = 0; i < buckets.size(); i++) {
		shading_bucket_bins[i] = is_bucket_compute(i) ? i : RendererRD::NEXTGEO_SHADING_BIN_NONE;
	}
	rd->buffer_update(shading_bucket_bins_buffer, 0, shading_bucket_bins.size() * sizeof(uint32_t), shading_bucket_bins.ptr());

	const RID record_buf = p_storage->get_record_buffer().is_valid() ? p_storage->get_record_buffer() : dummy_buffer;
	const RID mesh_buf = p_storage->get_mesh_record_buffer().is_valid() ? p_storage->get_mesh_record_buffer() : dummy_buffer;
	const RID cluster_buf = p_storage->get_cluster_buffer().is_valid() ? p_storage->get_cluster_buffer() : dummy_buffer;

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, p_visibility_tex));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_visible_clusters_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, record_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, mesh_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, cluster_buf));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, shading_bucket_bins_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, shading_bin_state_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, shading_args_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, shading_pixel_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, bucket_table_buffer));

	BinningPush push;
	push.viewport_width = width;
	push.viewport_height = height;
	push.bin_count = buckets.size();
	push.pixel_capacity = shading_pixel_capacity;
	if (p_region.size.x > 0 && p_region.size.y > 0) {
		push.region_min[0] = uint32_t(CLAMP(p_region.position.x, 0, int(width)));
		push.region_min[1] = uint32_t(CLAMP(p_region.position.y, 0, int(height)));
		push.region_max[0] = uint32_t(CLAMP(p_region.position.x + p_region.size.x, 0, int(width)));
		push.region_max[1] = uint32_t(CLAMP(p_region.position.y + p_region.size.y, 0, int(height)));
	} else {
		push.region_max[0] = width;
		push.region_max[1] = height;
	}

	const uint32_t tiles_x = (width + RendererRD::NEXTGEO_SHADING_TILE_SIZE - 1u) / RendererRD::NEXTGEO_SHADING_TILE_SIZE;
	const uint32_t tiles_y = (height + RendererRD::NEXTGEO_SHADING_TILE_SIZE - 1u) / RendererRD::NEXTGEO_SHADING_TILE_SIZE;
	const uint32_t groups[BINNING_PASS_MAX][2] = {
		{ (push.bin_count + 63u) / 64u, 1u },
		{ tiles_x, tiles_y },
		{ 1u, 1u },
		{ tiles_x, tiles_y },
	};
	// One compute list per pass, so each pass sees the previous pass's writes.
	for (uint32_t pass = 0; pass < BINNING_PASS_MAX; pass++) {
		const RID set = uniform_set_cache->get_cache_vec(binning_shader.version_get_shader(binning_version, pass), 0, uniforms);
		if (!set.is_valid()) {
			shading_bucket_bins.clear();
			return false;
		}
		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, binning_pipelines[pass]);
		rd->compute_list_bind_uniform_set(cl, set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch(cl, groups[pass][0], groups[pass][1], 1u);
		rd->compute_list_end();
	}

	binned = true;
	return true;
}

void NextGeoResolve::_prepare_compute_buckets(const ComputeStage &p_compute, RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, SceneShaderForwardClustered::ShaderSpecialization p_stage_a_spec, const SceneShaderForwardClustered::ShaderSpecialization *p_stage_b_spec, bool p_use_directional_soft_shadow, LocalVector<ComputeBucket> &r_compute) {
	r_compute.clear();
	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (!binned || rd == nullptr || uniform_set_cache == nullptr || p_storage == nullptr) {
		return;
	}

	const RID vis_tex = p_visibility_tex.is_valid() ? p_visibility_tex : dummy_vis_tex;
	const RID clusters_buf = p_visible_clusters_buf.is_valid() ? p_visible_clusters_buf : dummy_buffer;
	const RID record_buf = p_storage->get_record_buffer().is_valid() ? p_storage->get_record_buffer() : dummy_buffer;
	const RID mesh_buf = p_storage->get_mesh_record_buffer().is_valid() ? p_storage->get_mesh_record_buffer() : dummy_buffer;
	const RID cluster_buf = p_storage->get_cluster_buffer().is_valid() ? p_storage->get_cluster_buffer() : dummy_buffer;
	const RID page_buf = p_storage->get_page_buffer().is_valid() ? p_storage->get_page_buffer() : dummy_buffer;
	const RID bucket_buf = bucket_table_buffer.is_valid() ? bucket_table_buffer : dummy_buffer;
	const RID inst_buf = instance_data_buffer.is_valid() ? instance_data_buffer : dummy_buffer;
	const RID spline_buf = p_storage->get_spline_buffer().is_valid() ? p_storage->get_spline_buffer() : dummy_buffer;

	// The resolve set of the tile quads, with the pixel list at binding 7, the bin state and the images.
	LocalVector<RD::Uniform> resolve_uniforms;
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, vis_tex));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, clusters_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, record_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, mesh_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, cluster_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, page_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bucket_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, shading_pixel_buffer));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, inst_buf));
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, COMPUTE_SHADING_BIN_STATE_BINDING, shading_bin_state_buffer));
	for (uint32_t i = 0; i < COMPUTE_IMAGE_COUNT; i++) {
		if (p_compute.images[i].is_valid()) {
			resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, COMPUTE_IMAGE_BINDING_BASE + i, p_compute.images[i]));
		}
	}
	resolve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, spline_buf));

	for (uint32_t i = 0; i < buckets.size(); i++) {
		const MaterialBucket &bucket = buckets[i];
		// Only a bucket this frame's bin() binned has a pixel list; a compute form that failed since (in
		// Stage A) keeps the tile quads.
		if (bucket.shader == nullptr || bucket.material == nullptr || !is_bucket_binned(i) || !bucket.shader->has_resolve_compute()) {
			continue;
		}
		// The specialization of the bucket's tile quads.
		SceneShaderForwardClustered::ShaderSpecialization spec = p_stage_a_spec;
		if (p_stage_b_spec != nullptr) {
			spec = *p_stage_b_spec;
			spec.multimesh = 0;
			spec.multimesh_format_2d = 0;
			spec.multimesh_has_color = 0;
			spec.multimesh_has_custom_data = 0;
			spec.use_forward_gi = bucket.specialization.use_forward_gi;
			spec.use_light_soft_shadows = bucket.specialization.use_light_soft_shadows;
			spec.use_light_projector = bucket.specialization.use_light_projector;
			spec.use_directional_soft_shadows = p_use_directional_soft_shadow;
			spec.use_lightmap_specular = bucket.specialization.use_lightmap_specular;
			spec.material_feedback = bucket.specialization.material_feedback;
		}
		ComputeBucket entry;
		entry.bucket = i;
		entry.pipeline = bucket.shader->get_resolve_compute_pipeline(p_compute.variant, spec);
		const RID shader_rid = bucket.shader->get_resolve_compute_shader(p_compute.variant);
		if (!entry.pipeline.is_valid() || !shader_rid.is_valid()) {
			continue;
		}
		entry.sets[RenderForwardClustered::SCENE_UNIFORM_SET] = uniform_set_cache->get_cache_vec(shader_rid, RenderForwardClustered::SCENE_UNIFORM_SET, p_compute.scene_uniforms);
		entry.sets[RenderForwardClustered::RENDER_PASS_UNIFORM_SET] = uniform_set_cache->get_cache_vec(shader_rid, RenderForwardClustered::RENDER_PASS_UNIFORM_SET, p_compute.pass_uniforms);
		entry.sets[RenderForwardClustered::TRANSFORMS_UNIFORM_SET] = uniform_set_cache->get_cache_vec(shader_rid, RenderForwardClustered::TRANSFORMS_UNIFORM_SET, resolve_uniforms);
		if (bucket.material_uniform_set.is_valid()) {
			Vector<RD::Uniform> material_uniforms;
			// GodotNext: nextgeo_texture_cache_refresh (#305). Streaming texture swaps refresh texture_cache in
			// place; this guards against a texture freed mid-frame before update_dirty_resources().
			bool textures_valid = bucket.material->get_parameter_uniforms(material_uniforms, bucket.shader->texture_uniforms, bucket.shader->ubo_size, true);
			for (const RD::Uniform &uniform : material_uniforms) {
				if (uniform.uniform_type == RD::UNIFORM_TYPE_TEXTURE) {
					textures_valid = textures_valid && uniform.get_id_count() > 0;
					for (uint32_t j = 0; j < uniform.get_id_count(); j++) {
						textures_valid = textures_valid && rd->texture_is_valid(uniform.get_id(j));
					}
				}
			}
			if (!textures_valid) {
				RendererRD::MaterialStorage::get_singleton()->material_queue_texture_update(bucket.material->get_self());
				continue;
			}
			if (!material_uniforms.is_empty()) {
				LocalVector<RD::Uniform> material_list;
				for (const RD::Uniform &uniform : material_uniforms) {
					material_list.push_back(uniform);
				}
				entry.sets[RenderForwardClustered::MATERIAL_UNIFORM_SET] = uniform_set_cache->get_cache_vec(shader_rid, RenderForwardClustered::MATERIAL_UNIFORM_SET, material_list);
			}
			// The tile quads bind a material set here; a dispatch without one would leave the bucket unshaded.
			if (!entry.sets[RenderForwardClustered::MATERIAL_UNIFORM_SET].is_valid()) {
				continue;
			}
		}
		if (!entry.sets[RenderForwardClustered::SCENE_UNIFORM_SET].is_valid() || !entry.sets[RenderForwardClustered::RENDER_PASS_UNIFORM_SET].is_valid() || !entry.sets[RenderForwardClustered::TRANSFORMS_UNIFORM_SET].is_valid()) {
			continue;
		}
		r_compute.push_back(entry);
	}
}

uint32_t NextGeoResolve::_dispatch_compute_buckets(const LocalVector<ComputeBucket> &p_compute, bool p_stage_b, uint32_t p_flags, bool p_reverse_cull, bool p_stage_a_motion, bool p_reset_velocity, float p_vertex_offset_default) {
	RenderingDevice *rd = RD::get_singleton();
	if (p_compute.is_empty() || rd == nullptr || !shading_args_buffer.is_valid()) {
		return 0;
	}
	uint32_t dispatches = 0;
	RD::ComputeListID cl = rd->compute_list_begin();
	for (const ComputeBucket &entry : p_compute) {
		const MaterialBucket &bucket = buckets[entry.bucket];
		rd->compute_list_bind_compute_pipeline(cl, entry.pipeline);
		for (uint32_t set = 0; set < 4; set++) {
			if (entry.sets[set].is_valid()) {
				rd->compute_list_bind_uniform_set(cl, entry.sets[set], set);
			}
		}
		bucket.material->set_as_used();

		const bool writes_vertex = bucket.shader != nullptr && bucket.shader->uses_vertex;
		uint32_t compute_flags = p_flags;
		if (!p_stage_b && p_stage_a_motion && !writes_vertex) {
			compute_flags |= COMPUTE_FLAG_SKIP_VELOCITY;
		}
		const bool write_velocity = (!p_stage_b && p_stage_a_motion && writes_vertex) || (p_stage_b && (compute_flags & COMPUTE_FLAG_SKIP_VELOCITY) == 0u);

		ResolvePush push;
		push.bucket_id = entry.bucket;
		push.material_feedback_index = p_stage_b ? bucket.material_feedback_index : UINT32_MAX;
		push.tile_list_base = compute_flags;
		push.cull_sign = p_reverse_cull ? -1.0f : 1.0f;
		push.vertex_offset_max = writes_vertex && bucket.material != nullptr ? bucket.material->get_effective_vertex_offset_max(p_vertex_offset_default) : 0.0f;
		push.flags = (writes_vertex ? RESOLVE_FLAG_VERTEX_OFFSET : 0u) | (write_velocity ? RESOLVE_FLAG_WRITE_VELOCITY : 0u) | (p_reset_velocity ? RESOLVE_FLAG_RESET_VELOCITY : 0u);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_indirect(cl, shading_args_buffer, entry.bucket * RendererRD::NEXTGEO_SHADING_ARGS_WORDS * sizeof(uint32_t));
		dispatches++;
	}
	rd->compute_list_end();
	return dispatches;
}

uint32_t NextGeoResolve::draw_stage_a(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, RID p_render_base_uniform_set, RID p_rp_uniform_set, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_using_voxelgi, bool p_reverse_cull, const Rect2 &p_region, bool p_megalights_inputs, bool &r_request_redraw, const ComputeStage *p_compute, bool p_with_velocity, bool p_reset_velocity, float p_vertex_offset_default) {
	if (buckets.is_empty() || current_max_tiles == 0 || !p_framebuffer.is_valid() || !indirect_args_buffer.is_valid()) {
		return 0;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return 0;
	}

	const RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(p_framebuffer);
	// GodotNext: megalights_inputs (#136), nextgeo_vertex_offset (#164).
	SceneShaderForwardClustered::PipelineVersion version = SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES;
	if (p_with_velocity) {
		if (p_megalights_inputs) {
			version = p_using_voxelgi ? SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION : SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION;
		} else {
			version = p_using_voxelgi ? SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION : SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION;
		}
	} else if (p_megalights_inputs) {
		version = p_using_voxelgi ? SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML : SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML;
	} else if (p_using_voxelgi) {
		version = SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI;
	}

	SceneShaderForwardClustered::ShaderSpecialization stage_a_spec = p_base_specialization;
	stage_a_spec.multimesh = 0;
	stage_a_spec.multimesh_format_2d = 0;
	stage_a_spec.multimesh_has_color = 0;
	stage_a_spec.multimesh_has_custom_data = 0;
	stage_a_spec.material_feedback = 0;

	// GodotNext: nextgeo_shading_bins (#158). The buckets that shade through their bins; the tile quads
	// shade the rest, and Stage A loads its targets, so the draw list opens only when a bucket needs it.
	LocalVector<ComputeBucket> compute;
	LocalVector<bool> compute_bucket;
	if (p_compute != nullptr) {
		_prepare_compute_buckets(*p_compute, p_storage, p_visibility_tex, p_visible_clusters_buf, stage_a_spec, nullptr, false, compute);
		compute_bucket.resize_initialized(buckets.size());
		for (const ComputeBucket &entry : compute) {
			compute_bucket[entry.bucket] = true;
		}
	}
	bool tile_quads = p_compute == nullptr;
	for (uint32_t i = 0; i < buckets.size() && !tile_quads; i++) {
		tile_quads = buckets[i].shader != nullptr && buckets[i].material != nullptr && !compute_bucket[i];
	}
	uint32_t draws = 0;
	if (tile_quads) {
		RD::DrawListID dl = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_region);
		rd->draw_list_bind_uniform_set(dl, p_render_base_uniform_set, RenderForwardClustered::SCENE_UNIFORM_SET);
		rd->draw_list_bind_uniform_set(dl, p_rp_uniform_set, RenderForwardClustered::RENDER_PASS_UNIFORM_SET);

		RID prev_pipeline;
		RID prev_resolve_set;
		RID prev_material_set;

		for (uint32_t i = 0; i < buckets.size(); i++) {
			const MaterialBucket &bucket = buckets[i];
			if (bucket.shader == nullptr || bucket.material == nullptr) {
				continue;
			}
			if (bucket.shader->uses_time) {
				r_request_redraw = true;
			}
			if (!compute_bucket.is_empty() && compute_bucket[i]) {
				continue;
			}

			const bool writes_vertex = bucket.shader->uses_vertex;

			SceneShaderForwardClustered::ShaderData::PipelineKey key;
			key.vertex_format_id = RD::INVALID_ID;
			key.framebuffer_format_id = fb_format;
			key.cull_mode = RD::POLYGON_CULL_DISABLED;
			key.primitive_type = RSE::PRIMITIVE_TRIANGLES;
			key.version = version;
			key.color_pass_flags = 0;
			key.shader_specialization = stage_a_spec;
			key.wireframe = false;
			key.ubershader = 0;
			// GodotNext: nextgeo_vertex_offset (#164). In Stage A motion mode, non-WPO buckets keep
			// visibility_export's rigid motion vectors untouched; only WPO buckets write velocity.
			key.zero_velocity_write = (p_with_velocity && !writes_vertex) ? 1 : 0;

			const uint32_t key_hash = key.hash();
			const RID pipeline = bucket.shader->pipeline_hash_map.get_pipeline(key, key_hash, true, RSE::PIPELINE_SOURCE_SPECIALIZATION);
			if (!pipeline.is_valid()) {
				continue;
			}

			const RID shader_rid = bucket.shader->get_shader_variant(version, 0, false);
			const RID resolve_set = _build_resolve_uniform_set(shader_rid, p_storage, p_visibility_tex, p_visible_clusters_buf);
			if (!resolve_set.is_valid()) {
				continue;
			}

			if (pipeline != prev_pipeline) {
				rd->draw_list_bind_render_pipeline(dl, pipeline);
				prev_pipeline = pipeline;
			}
			if (resolve_set != prev_resolve_set) {
				rd->draw_list_bind_uniform_set(dl, resolve_set, RenderForwardClustered::TRANSFORMS_UNIFORM_SET);
				prev_resolve_set = resolve_set;
			}
			if (bucket.material_uniform_set.is_valid() && bucket.material_uniform_set != prev_material_set) {
				rd->draw_list_bind_uniform_set(dl, bucket.material_uniform_set, RenderForwardClustered::MATERIAL_UNIFORM_SET);
				prev_material_set = bucket.material_uniform_set;
			}
			bucket.material->set_as_used();

			ResolvePush push;
			push.bucket_id = i;
			push.material_feedback_index = UINT32_MAX;
			push.tile_list_base = i * current_max_tiles;
			push.cull_sign = p_reverse_cull ? -1.0f : 1.0f;
			push.vertex_offset_max = writes_vertex ? bucket.material->get_effective_vertex_offset_max(p_vertex_offset_default) : 0.0f;
			push.flags = (writes_vertex ? RESOLVE_FLAG_VERTEX_OFFSET : 0u) | ((p_with_velocity && writes_vertex) ? RESOLVE_FLAG_WRITE_VELOCITY : 0u) | (p_reset_velocity ? RESOLVE_FLAG_RESET_VELOCITY : 0u);
			rd->draw_list_set_push_constant(dl, &push, sizeof(push));
			rd->draw_list_draw_indirect(dl, false, indirect_args_buffer, i * 16u, 1, 0);
			draws++;
		}

		rd->draw_list_end();
	}
	// GodotNext: nextgeo_shading_bins (#158). Then each compute bucket's bin, in one compute list.
	if (!compute.is_empty()) {
		for (const ComputeBucket &entry : compute) {
			if (buckets[entry.bucket].shader->uses_time) {
				r_request_redraw = true;
			}
		}
		const uint32_t dispatches = _dispatch_compute_buckets(compute, false, p_compute->flags, p_reverse_cull, p_with_velocity, p_reset_velocity, p_vertex_offset_default);
		draws += dispatches;
		s_last_published_stage_a_compute.store(dispatches, std::memory_order_relaxed);
	}
	s_last_published_stage_a_draws.store(draws, std::memory_order_relaxed);
	return draws;
}

bool NextGeoResolve::draw_stage_b(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_colors, RID p_render_base_uniform_set, RID p_rp_uniform_set, uint32_t p_pipeline_color_pass_flags, bool p_zero_velocity_write, bool p_use_directional_soft_shadow, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_reverse_cull, const Rect2 &p_region, bool &r_request_redraw, uint32_t &r_draws, const ComputeStage *p_compute, bool p_reset_velocity, float p_vertex_offset_default) {
	r_draws = 0;
	if (buckets.is_empty() || current_max_tiles == 0 || !p_framebuffer.is_valid() || !indirect_args_buffer.is_valid()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}

	const RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(p_framebuffer);
	const SceneShaderForwardClustered::PipelineVersion version = SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_COLOR;

	// GodotNext: nextgeo_shading_bins (#158). The buckets that shade through their bins. The draw list
	// still opens first: it applies the colour pass's clears, then the tile quads shade the other buckets.
	LocalVector<ComputeBucket> compute;
	LocalVector<bool> compute_bucket;
	if (p_compute != nullptr) {
		_prepare_compute_buckets(*p_compute, p_storage, p_visibility_tex, p_visible_clusters_buf, SceneShaderForwardClustered::ShaderSpecialization(), &p_base_specialization, p_use_directional_soft_shadow, compute);
		compute_bucket.resize_initialized(buckets.size());
		for (const ComputeBucket &entry : compute) {
			compute_bucket[entry.bucket] = true;
		}
	}

	RD::DrawListID dl = rd->draw_list_begin(p_framebuffer, p_draw_flags, p_clear_colors, 0.0f, 0u, p_region);
	rd->draw_list_bind_uniform_set(dl, p_render_base_uniform_set, RenderForwardClustered::SCENE_UNIFORM_SET);
	rd->draw_list_bind_uniform_set(dl, p_rp_uniform_set, RenderForwardClustered::RENDER_PASS_UNIFORM_SET);

	RID prev_pipeline;
	RID prev_resolve_set;
	RID prev_material_set;

	for (uint32_t i = 0; i < buckets.size(); i++) {
		const MaterialBucket &bucket = buckets[i];
		if (bucket.shader == nullptr || bucket.material == nullptr) {
			continue;
		}
		if (bucket.shader->uses_time) {
			r_request_redraw = true;
		}
		if (!compute_bucket.is_empty() && compute_bucket[i]) {
			continue;
		}

		SceneShaderForwardClustered::ShaderSpecialization spec = p_base_specialization;
		spec.multimesh = 0;
		spec.multimesh_format_2d = 0;
		spec.multimesh_has_color = 0;
		spec.multimesh_has_custom_data = 0;
		spec.use_forward_gi = bucket.specialization.use_forward_gi;
		spec.use_light_soft_shadows = bucket.specialization.use_light_soft_shadows;
		spec.use_light_projector = bucket.specialization.use_light_projector;
		spec.use_directional_soft_shadows = p_use_directional_soft_shadow;
		spec.use_lightmap_specular = bucket.specialization.use_lightmap_specular;
		spec.material_feedback = bucket.specialization.material_feedback;

		SceneShaderForwardClustered::ShaderData::PipelineKey key;
		key.vertex_format_id = RD::INVALID_ID;
		key.framebuffer_format_id = fb_format;
		key.cull_mode = RD::POLYGON_CULL_DISABLED;
		key.primitive_type = RSE::PRIMITIVE_TRIANGLES;
		key.version = version;
		key.color_pass_flags = p_pipeline_color_pass_flags;
		key.shader_specialization = spec;
		key.wireframe = false;
		key.ubershader = 0;
		key.zero_velocity_write = p_zero_velocity_write ? 1 : 0;

		const uint32_t key_hash = key.hash();
		const RID pipeline = bucket.shader->pipeline_hash_map.get_pipeline(key, key_hash, true, RSE::PIPELINE_SOURCE_SPECIALIZATION);
		if (!pipeline.is_valid()) {
			continue;
		}

		const RID shader_rid = bucket.shader->get_shader_variant(version, p_pipeline_color_pass_flags, false);
		const RID resolve_set = _build_resolve_uniform_set(shader_rid, p_storage, p_visibility_tex, p_visible_clusters_buf);
		if (!resolve_set.is_valid()) {
			continue;
		}

		if (pipeline != prev_pipeline) {
			rd->draw_list_bind_render_pipeline(dl, pipeline);
			prev_pipeline = pipeline;
		}
		if (resolve_set != prev_resolve_set) {
			rd->draw_list_bind_uniform_set(dl, resolve_set, RenderForwardClustered::TRANSFORMS_UNIFORM_SET);
			prev_resolve_set = resolve_set;
		}
		if (bucket.material_uniform_set.is_valid() && bucket.material_uniform_set != prev_material_set) {
			rd->draw_list_bind_uniform_set(dl, bucket.material_uniform_set, RenderForwardClustered::MATERIAL_UNIFORM_SET);
			prev_material_set = bucket.material_uniform_set;
		}
		bucket.material->set_as_used();

		const bool writes_vertex = bucket.shader->uses_vertex;
		const bool write_velocity = !p_zero_velocity_write && (p_pipeline_color_pass_flags & SceneShaderForwardClustered::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS) != 0;

		ResolvePush push;
		push.bucket_id = i;
		push.material_feedback_index = bucket.material_feedback_index;
		push.tile_list_base = i * current_max_tiles;
		push.cull_sign = p_reverse_cull ? -1.0f : 1.0f;
		push.vertex_offset_max = writes_vertex ? bucket.material->get_effective_vertex_offset_max(p_vertex_offset_default) : 0.0f;
		push.flags = (writes_vertex ? RESOLVE_FLAG_VERTEX_OFFSET : 0u) | (write_velocity ? RESOLVE_FLAG_WRITE_VELOCITY : 0u) | (p_reset_velocity ? RESOLVE_FLAG_RESET_VELOCITY : 0u);
		rd->draw_list_set_push_constant(dl, &push, sizeof(push));
		rd->draw_list_draw_indirect(dl, false, indirect_args_buffer, i * 16u, 1, 0);
		r_draws++;
	}

	rd->draw_list_end();
	// GodotNext: nextgeo_shading_bins (#158)
	if (!compute.is_empty()) {
		const uint32_t dispatches = _dispatch_compute_buckets(compute, true, p_compute->flags, p_reverse_cull, false, p_reset_velocity, p_vertex_offset_default);
		r_draws += dispatches;
		s_last_published_stage_b_compute.store(dispatches, std::memory_order_relaxed);
	}
	s_last_published_stage_b_draws.store(r_draws, std::memory_order_relaxed);
	return true;
}

void NextGeoResolve::free_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (indirect_args_buffer.is_valid()) {
			rd->free_rid(indirect_args_buffer);
		}
		if (tile_list_buffer.is_valid()) {
			rd->free_rid(tile_list_buffer);
		}
		if (bucket_table_buffer.is_valid()) {
			rd->free_rid(bucket_table_buffer);
		}
		if (instance_data_buffer.is_valid()) {
			rd->free_rid(instance_data_buffer);
		}
		if (dummy_buffer.is_valid()) {
			rd->free_rid(dummy_buffer);
		}
		if (dummy_vis_tex.is_valid()) {
			rd->free_rid(dummy_vis_tex);
		}
		if (classify_version.is_valid()) {
			classify_shader.version_free(classify_version);
		}
		// GodotNext: nextgeo_shading_bins (#158)
		for (RID buffer : { shading_bucket_bins_buffer, shading_bin_state_buffer, shading_args_buffer, shading_pixel_buffer }) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
		if (binning_version.is_valid()) {
			binning_shader.version_free(binning_version);
		}
	}
	indirect_args_buffer = RID();
	indirect_bucket_capacity = 0;
	tile_list_buffer = RID();
	tile_list_word_capacity = 0;
	bucket_table_buffer = RID();
	bucket_table_byte_capacity = 0;
	instance_data_buffer = RID();
	instance_data_byte_capacity = 0;
	dummy_buffer = RID();
	dummy_vis_tex = RID();
	classify_version = RID();
	init_indirect_pipeline = RID();
	classify_pipeline = RID();
	buckets.clear();
	bucket_map.clear();
	initialized = false;
	failed = false;
	// GodotNext: nextgeo_shading_bins (#158)
	shading_bucket_bins.clear();
	shading_bucket_bins_buffer = RID();
	shading_bin_state_buffer = RID();
	shading_args_buffer = RID();
	shading_bin_capacity = 0;
	shading_pixel_buffer = RID();
	shading_pixel_capacity = 0;
	binning_version = RID();
	for (RID &pipeline : binning_pipelines) {
		pipeline = RID();
	}
	binning_initialized = false;
	binning_failed = false;
	binned = false;
}

} // namespace RendererSceneRenderImplementation
