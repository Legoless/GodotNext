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
static_assert(sizeof(NextGeoResolve::ResolvePush) == 16);

std::atomic<uint32_t> s_last_published_bucket_count{ 0 };
std::atomic<uint32_t> s_last_published_stage_a_draws{ 0 };
std::atomic<uint32_t> s_last_published_stage_b_draws{ 0 };

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

void NextGeoResolve::begin_frame() {
	buckets.clear();
	bucket_map.clear();
	current_max_tiles = 0;
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

	return uniform_set_cache->get_cache_vec(p_shader_rid, RenderForwardClustered::TRANSFORMS_UNIFORM_SET, uniforms);
}

uint32_t NextGeoResolve::draw_stage_a(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, RID p_render_base_uniform_set, RID p_rp_uniform_set, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_using_voxelgi, bool p_reverse_cull, const Rect2 &p_region, bool &r_request_redraw) {
	if (buckets.is_empty() || current_max_tiles == 0 || !p_framebuffer.is_valid() || !indirect_args_buffer.is_valid()) {
		return 0;
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return 0;
	}

	const RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(p_framebuffer);
	const SceneShaderForwardClustered::PipelineVersion version = p_using_voxelgi ? SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI : SceneShaderForwardClustered::PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES;

	SceneShaderForwardClustered::ShaderSpecialization stage_a_spec = p_base_specialization;
	stage_a_spec.multimesh = 0;
	stage_a_spec.multimesh_format_2d = 0;
	stage_a_spec.multimesh_has_color = 0;
	stage_a_spec.multimesh_has_custom_data = 0;
	stage_a_spec.material_feedback = 0;

	RD::DrawListID dl = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_region);
	rd->draw_list_bind_uniform_set(dl, p_render_base_uniform_set, RenderForwardClustered::SCENE_UNIFORM_SET);
	rd->draw_list_bind_uniform_set(dl, p_rp_uniform_set, RenderForwardClustered::RENDER_PASS_UNIFORM_SET);

	RID prev_pipeline;
	RID prev_resolve_set;
	RID prev_material_set;
	uint32_t draws = 0;

	for (uint32_t i = 0; i < buckets.size(); i++) {
		const MaterialBucket &bucket = buckets[i];
		if (bucket.shader == nullptr || bucket.material == nullptr) {
			continue;
		}
		if (bucket.shader->uses_time) {
			r_request_redraw = true;
		}

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
		key.zero_velocity_write = 0;

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
		rd->draw_list_set_push_constant(dl, &push, sizeof(push));
		rd->draw_list_draw_indirect(dl, false, indirect_args_buffer, i * 16u, 1, 0);
		draws++;
	}

	rd->draw_list_end();
	s_last_published_stage_a_draws.store(draws, std::memory_order_relaxed);
	return draws;
}

bool NextGeoResolve::draw_stage_b(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_colors, RID p_render_base_uniform_set, RID p_rp_uniform_set, uint32_t p_pipeline_color_pass_flags, bool p_zero_velocity_write, bool p_use_directional_soft_shadow, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_reverse_cull, const Rect2 &p_region, bool &r_request_redraw, uint32_t &r_draws) {
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

		ResolvePush push;
		push.bucket_id = i;
		push.material_feedback_index = bucket.material_feedback_index;
		push.tile_list_base = i * current_max_tiles;
		push.cull_sign = p_reverse_cull ? -1.0f : 1.0f;
		rd->draw_list_set_push_constant(dl, &push, sizeof(push));
		rd->draw_list_draw_indirect(dl, false, indirect_args_buffer, i * 16u, 1, 0);
		r_draws++;
	}

	rd->draw_list_end();
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
}

} // namespace RendererSceneRenderImplementation
