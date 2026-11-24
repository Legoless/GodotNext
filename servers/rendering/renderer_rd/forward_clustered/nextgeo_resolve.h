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
// GodotNext: nextgeo_shading_bins (#158)
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_shading_bins.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/shading_binning.glsl.gen.h"

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

	// GodotNext: nextgeo_vertex_offset (#164). Flags carried in ResolvePush::flags.
	static constexpr uint32_t RESOLVE_FLAG_VERTEX_OFFSET = 1u << 0u;
	static constexpr uint32_t RESOLVE_FLAG_WRITE_VELOCITY = 1u << 1u;
	static constexpr uint32_t RESOLVE_FLAG_RESET_VELOCITY = 1u << 2u;

	struct ResolvePush {
		uint32_t bucket_id = 0;
		uint32_t material_feedback_index = UINT32_MAX;
		uint32_t tile_list_base = 0;
		float cull_sign = 1.0f;
		// GodotNext: nextgeo_vertex_offset (#164). Per-bucket WPO clamp bound and mode flags.
		float vertex_offset_max = 0.0f;
		uint32_t flags = 0;
		uint32_t pad0 = 0;
		uint32_t pad1 = 0;
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

	// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The compute form's storage images: set 2 bindings
	// 10 to 15 of resolve_compute_inc.glsl, in its order for each stage (binding 15: Stage A velocity for #164).
	static constexpr uint32_t COMPUTE_IMAGE_BINDING_BASE = 10u;
	static constexpr uint32_t COMPUTE_IMAGE_COUNT = 6u;
	// The formats resolve_compute_inc.glsl declares for those images, per stage (DATA_FORMAT_MAX: no image at
	// that binding). A target of another format keeps the tile quads (NO_STORAGE).
	static constexpr RD::DataFormat COMPUTE_STAGE_A_FORMATS[COMPUTE_IMAGE_COUNT] = { RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::DATA_FORMAT_R8G8_UINT, RD::DATA_FORMAT_R32_UINT, RD::DATA_FORMAT_R16G16_SFLOAT };
	static constexpr RD::DataFormat COMPUTE_STAGE_B_FORMATS[COMPUTE_IMAGE_COUNT] = { RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::DATA_FORMAT_R16G16_SFLOAT, RD::DATA_FORMAT_MAX, RD::DATA_FORMAT_MAX, RD::DATA_FORMAT_MAX };
	static constexpr uint32_t COMPUTE_SHADING_BIN_STATE_BINDING = 9u;
	// ResolvePush.tile_list_base of the compute form (resolve_compute_inc.glsl).
	static constexpr uint32_t COMPUTE_FLAG_SKIP_VELOCITY = 1u << 0u;

	// What one stage's compute bins bind besides what its tile quads bind: the compute form's variant, the
	// tile quads' set 0 and set 1 uniforms (a set is made per shader, so the compute form gets its own),
	// and the stage's output targets as storage images.
	struct ComputeStage {
		SceneShaderForwardClustered::ResolveComputeVariant variant = SceneShaderForwardClustered::RESOLVE_COMPUTE_GUIDES;
		LocalVector<RD::Uniform> scene_uniforms;
		LocalVector<RD::Uniform> pass_uniforms;
		RID images[COMPUTE_IMAGE_COUNT];
		uint32_t flags = 0;
	};

	// Whether p_texture can be bound as a compute-form storage image of p_format: that format, single
	// sampled, created with storage usage, and storable on this device.
	static bool compute_target_storable(RID p_texture, RD::DataFormat p_format);

	void begin_frame();
	uint32_t register_bucket(SceneShaderForwardClustered::MaterialData *p_material, SceneShaderForwardClustered::ShaderData *p_shader, RID p_material_uniform_set, uint32_t p_material_feedback_index, SceneShaderForwardClustered::ShaderSpecialization p_specialization);
	uint32_t get_bucket_count() const { return buckets.size(); }
	// GodotNext: nextgeo_vertex_offset (#164). True when at least one registered bucket writes VERTEX.
	bool has_vertex_offset_buckets() const;
	// GodotNext: nextgeo_shading_bins (#158). Buckets whose material has a compute form (one that has not
	// failed to compile).
	uint32_t get_compute_bucket_count() const;
	bool is_bucket_compute(uint32_t p_bucket) const { return p_bucket < buckets.size() && buckets[p_bucket].shader != nullptr && buckets[p_bucket].shader->has_resolve_compute(); }
	static uint32_t get_last_published_bucket_count();
	static uint32_t get_last_published_stage_a_draws();
	static uint32_t get_last_published_stage_b_draws();

	bool upload_tables(const uint32_t *p_bucket_table, uint32_t p_bucket_table_words, const void *p_instance_data, uint32_t p_instance_data_bytes);
	RID get_bucket_table_buffer();
	// GodotNext: nextgeo_raster_bins (#155). The per-record InstanceData the resolve binds; a programmable
	// raster bin's material code reads it too.
	RID get_instance_data_buffer();
	// GodotNext: nextgeo_tessellation (#163). Diced patch records buffer (invalid when tessellation is off).
	void set_diced_patches_buffer(RID p_buffer) { diced_patches_buffer = p_buffer; }
	// GodotNext: nextgeo_voxels (#165). Visible sparse 4^3 voxel bricks buffer (invalid when voxels are off).
	void set_voxel_bricks_buffer(RID p_buffer) { voxel_bricks_buffer = p_buffer; }

	bool classify(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, const Size2i &p_viewport_size);

	// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). After classify(), bins every pixel whose bucket
	// has a compute form (is_bucket_compute()): CLEAR, COUNT, PREFIX and SCATTER of shading_binning.glsl,
	// over the bucket table upload_tables() took and this frame's per-bucket bin words. Bin i is bucket i.
	// p_region is the render region (empty: the viewport).
	bool bin(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, const Size2i &p_viewport_size, const Rect2i &p_region);
	bool is_binned() const { return binned; }
	// Whether the last bin() binned bucket p_bucket; only those shade in compute this frame.
	bool is_bucket_binned(uint32_t p_bucket) const { return binned && p_bucket < shading_bucket_bins.size() && shading_bucket_bins[p_bucket] == p_bucket; }
	// The bin state (per bin uvec4: count, offset, cursor, unused) and the indirect arguments of the last
	// bin(), for #157's stats and visualization; invalid before the first bin().
	RID get_shading_bin_state_buffer() const { return shading_bin_state_buffer; }
	RID get_shading_bucket_bins_buffer() const { return binned ? shading_bucket_bins_buffer : RID(); }
	uint32_t get_shading_bucket_bins_count() const { return binned ? shading_bucket_bins.size() : 0u; }
	static uint32_t get_last_published_stage_a_compute();
	static uint32_t get_last_published_stage_b_compute();

	// GodotNext: nextgeo_shading_bins (#158). With p_compute (after bin()), every bucket whose material has a
	// compute form shades through its bin's indirect dispatch, and the rest draw their tile quads in the
	// same frame; the returned and r_draws counts include both.
	uint32_t draw_stage_a(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, RID p_render_base_uniform_set, RID p_rp_uniform_set, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_using_voxelgi, bool p_reverse_cull, const Rect2 &p_region, bool p_megalights_inputs, bool &r_request_redraw, const ComputeStage *p_compute = nullptr, bool p_with_velocity = false, bool p_reset_velocity = false, float p_vertex_offset_default = 0.5f);

	bool draw_stage_b(RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, RID p_framebuffer, BitField<RD::DrawFlags> p_draw_flags, const Vector<Color> &p_clear_colors, RID p_render_base_uniform_set, RID p_rp_uniform_set, uint32_t p_pipeline_color_pass_flags, bool p_zero_velocity_write, bool p_use_directional_soft_shadow, SceneShaderForwardClustered::ShaderSpecialization p_base_specialization, bool p_reverse_cull, const Rect2 &p_region, bool &r_request_redraw, uint32_t &r_draws, const ComputeStage *p_compute = nullptr, bool p_reset_velocity = false, float p_vertex_offset_default = 0.5f);

	void free_resources();

private:
	bool _ensure();
	void _ensure_classify_buffers(uint32_t p_bucket_count, uint32_t p_max_tiles);
	RID _build_resolve_uniform_set(RID p_shader_rid, RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf);
	// GodotNext: nextgeo_shading_bins (#158). The buckets of a stage that shade in compute (pipeline and
	// sets ready), and their dispatches.
	struct ComputeBucket {
		uint32_t bucket = 0;
		RID pipeline;
		RID sets[4];
	};
	bool _ensure_binning();
	void _ensure_bin_buffers(uint32_t p_bin_count, uint32_t p_pixel_capacity);
	void _prepare_compute_buckets(const ComputeStage &p_compute, RendererVirtualMeshStorage *p_storage, RID p_visibility_tex, RID p_visible_clusters_buf, SceneShaderForwardClustered::ShaderSpecialization p_stage_a_spec, const SceneShaderForwardClustered::ShaderSpecialization *p_stage_b_spec, bool p_use_directional_soft_shadow, LocalVector<ComputeBucket> &r_compute);
	uint32_t _dispatch_compute_buckets(const LocalVector<ComputeBucket> &p_compute, bool p_stage_b, uint32_t p_flags, bool p_reverse_cull, bool p_stage_a_motion, bool p_reset_velocity, float p_vertex_offset_default);

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
	// GodotNext: nextgeo_tessellation (#163). Non-owning handle to NextGeoCut's diced patch buffer for this frame.
	RID diced_patches_buffer;
	// GodotNext: nextgeo_voxels (#165). Non-owning handle to NextGeoCut's visible voxel bricks buffer for this frame.
	RID voxel_bricks_buffer;

	// GodotNext: nextgeo_shading_bins (#158). Made on the first bin(), so tile quads allocate nothing new.
	ShadingBinningShaderRD binning_shader;
	RID binning_version;
	RID binning_pipelines[4];
	bool binning_initialized = false;
	bool binning_failed = false;
	// The last bin()'s per-bucket bin words (bin i is bucket i, NEXTGEO_SHADING_BIN_NONE: tile quads), and
	// their buffer (shading_bin_capacity words).
	LocalVector<uint32_t> shading_bucket_bins;
	RID shading_bucket_bins_buffer;
	RID shading_bin_state_buffer;
	RID shading_args_buffer;
	uint32_t shading_bin_capacity = 0;
	RID shading_pixel_buffer;
	uint32_t shading_pixel_capacity = 0;
	bool binned = false;

	uint32_t current_max_tiles = 0;
	LocalVector<MaterialBucket> buckets;
	HashMap<MaterialBucketKey, uint32_t, MaterialBucketKeyHasher> bucket_map;

	bool initialized = false;
	bool failed = false;
};

} // namespace RendererSceneRenderImplementation
