/**************************************************************************/
/*  nextgeo_resolve.h                                                     */
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

#pragma once

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/resolve_classify.glsl.gen.h"

class RendererVirtualMeshStorage;

namespace RendererSceneRenderImplementation {

// GodotNext: nextgeo_resolve (#80, plan §5 D7, §6.1, §7.4).
// 8x8 tile classifier and per-MaterialBucket indirect Stage A / Stage B resolve
// dispatcher over RB_TEX_NEXTGEO_VISIBILITY.
class NextGeoResolve {
public:
	static constexpr uint32_t BUCKET_ID_MASK = 0x0FFFFFFFu;
	static constexpr uint32_t BUCKET_INVALID = 0x0FFFFFFFu;
	static constexpr uint32_t BUCKET_CULL_FRONT = 1u << 28u;
	static constexpr uint32_t BUCKET_CULL_DISABLED = 1u << 29u;
	static constexpr uint32_t SURFACES_PER_RECORD = 32u;

	struct ResolvePush {
		uint32_t bucket_id = 0;
		uint32_t material_feedback_index = UINT32_MAX;
		uint32_t tile_list_base = 0;
		float cull_sign = 1.0f;
	};

	struct MaterialBucketKey {
		const SceneShaderForwardClustered::MaterialData *material = nullptr;
		const SceneShaderForwardClustered::ShaderData *shader = nullptr;
		RID material_uniform_set;
		uint32_t spec_packed_0 = 0;
		uint32_t spec_packed_1 = 0;

		bool operator==(const MaterialBucketKey &p_other) const {
			return material == p_other.material &&
					shader == p_other.shader &&
					material_uniform_set == p_other.material_uniform_set &&
					spec_packed_0 == p_other.spec_packed_0 &&
					spec_packed_1 == p_other.spec_packed_1;
		}
	};

	struct MaterialBucketKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const MaterialBucketKey &p_key) {
			uint32_t h = hash_murmur3_one_64(reinterpret_cast<uint64_t>(p_key.material));
			h = hash_murmur3_one_64(reinterpret_cast<uint64_t>(p_key.shader), h);
			h = hash_murmur3_one_64(p_key.material_uniform_set.get_id(), h);
			h = hash_murmur3_one_32(p_key.spec_packed_0, h);
			h = hash_murmur3_one_32(p_key.spec_packed_1, h);
			return hash_fmix32(h);
		}
	};

	struct MaterialBucket {
		SceneShaderForwardClustered::MaterialData *material = nullptr;
		SceneShaderForwardClustered::ShaderData *shader = nullptr;
		RID material_uniform_set;
		uint32_t material_feedback_index = UINT32_MAX;
		SceneShaderForwardClustered::ShaderSpecialization specialization = {};
	};

	void begin_frame();
	uint32_t register_bucket(SceneShaderForwardClustered::MaterialData *p_material, SceneShaderForwardClustered::ShaderData *p_shader, RID p_material_uniform_set, uint32_t p_material_feedback_index, SceneShaderForwardClustered::ShaderSpecialization p_specialization);
	uint32_t get_bucket_count() const { return buckets.size(); }
	static uint32_t get_last_published_bucket_count();
	static uint32_t get_last_published_stage_a_draws();
	static uint32_t get_last_published_stage_b_draws();

	bool upload_tables(const uint32_t *p_bucket_table, uint32_t p_bucket_table_words, const void *p_instance_data, uint32_t p_instance_data_bytes);
	RID get_bucket_table_buffer();

	bool classify(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, const Size2i &p_viewport_size);

	uint32_t draw_stage_a(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, RID p_render_base_uniform_set, RID p_rp_uniform_set, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_using_voxelgi, bool p_reverse_cull, const Rect2 &p_region, bool p_megalights_inputs, bool &r_request_redraw);

	bool draw_stage_b(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_colors, RID p_render_base_uniform_set, RID p_rp_uniform_set, uint32_t p_pipeline_color_pass_flags, bool p_zero_velocity_write, bool p_use_directional_soft_shadow, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_reverse_cull, const Rect2 &p_region, bool &r_request_redraw, uint32_t &r_draws);

	void free_resources();

private:
	bool _ensure();
	void _ensure_classify_buffers(uint32_t p_bucket_count, uint32_t p_max_tiles);
	RID _build_resolve_uniform_set(RID p_shader_rid, RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf);

	ResolveClassifyShaderRD classify_shader;
	RID classify_version;
	RID init_indirect_pipeline;
	RID classify_pipeline;

	RID indirect_args_buffer;
	uint32_t indirect_bucket_capacity = 0;

	RID tile_list_buffer;
	uint32_t tile_list_word_capacity = 0;

	RID bucket_table_buffer;
	uint32_t bucket_table_byte_capacity = 0;

	RID instance_data_buffer;
	uint32_t instance_data_byte_capacity = 0;

	RID dummy_buffer;
	RID dummy_vis_tex;

	uint32_t current_max_tiles = 0;
	LocalVector<MaterialBucket> buckets;
	HashMap<MaterialBucketKey, uint32_t, MaterialBucketKeyHasher> bucket_map;

	bool initialized = false;
	bool failed = false;
};

} // namespace RendererSceneRenderImplementation
