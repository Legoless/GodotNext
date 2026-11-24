/**************************************************************************/
/*  scene_shader_forward_clustered.h                                      */
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

#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/renderer_rd/pipeline_hash_map_rd.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_hw.glsl.gen.h"
// GodotNext: nextgeo_mesh_raster (#154)
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_hw_mesh.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_sw.glsl.gen.h"
// GodotNext: nextgeo_shading_bins (#158)
#include "servers/rendering/renderer_rd/shaders/nextgeo/resolve_compute.glsl.gen.h"
// GodotNext: nextgeo_vertex_offset (#164)
#include "servers/rendering/renderer_rd/shaders/nextgeo/rt_vertex_offset.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/rendering_server_types.h"

// GodotNext: nextgi_hit_groups (#130)
namespace RendererRD {
struct NextGIHitShaderSource;
} // namespace RendererRD

namespace RendererSceneRenderImplementation {

class SceneShaderForwardClustered {
private:
	static SceneShaderForwardClustered *singleton;
	static Mutex singleton_mutex;

public:
	// GodotNext: nextgi_hit_groups (#130). The material the rasterizer draws a surface without one with.
	static RID get_default_material_for_nextgi() { return singleton != nullptr ? singleton->default_material : RID(); }

	enum ShaderGroup {
		SHADER_GROUP_BASE, // Always compiled at the beginning.
		SHADER_GROUP_ADVANCED,
		SHADER_GROUP_MULTIVIEW,
		SHADER_GROUP_ADVANCED_MULTIVIEW,
		SHADER_GROUP_NEXTGEO,
		// GodotNext: megalights_inputs (#136). Off unless this view writes inputs.
		SHADER_GROUP_MEGALIGHTS,
		SHADER_GROUP_MEGALIGHTS_MULTIVIEW,
		SHADER_GROUP_NEXTGEO_MEGALIGHTS,
		// GodotNext: rr_guides (#205). Off unless this view writes the DLSS-RR guides.
		// RR views are single-view (NextGI is), so nothing enables the multiview group;
		// it only keeps the color pass flags a plain bit field.
		SHADER_GROUP_RR_GUIDES,
		SHADER_GROUP_RR_GUIDES_MULTIVIEW,
		SHADER_GROUP_NEXTGEO_RR_GUIDES,
		// GodotNext: vsm_non_nanite (#218). Off unless VSM raster runs.
		SHADER_GROUP_VSM,
		// GodotNext: avboit (#295). The AVBOIT pre-pass variants, off until a view draws with AVBOIT.
		SHADER_GROUP_AVBOIT,
		// GodotNext: nextgeo_vertex_offset (#164). Stage A motion-vector variants, off until a view has WPO buckets.
		SHADER_GROUP_NEXTGEO_MOTION,
		SHADER_GROUP_NEXTGEO_MEGALIGHTS_MOTION,
	};

	// Not an enum because these values are constants that are processed as numbers
	// to arrive at a unique version for a particular shader.
	struct ShaderVersion {
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS = 0;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_DP = 1;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS = 2;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI = 3;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_MULTIVIEW = 4;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW = 5;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW = 6;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MATERIAL = 7;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_SDF = 8;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION = 9;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION = 10;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION_MULTIVIEW = 11;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW = 12;
		constexpr static uint16_t SHADER_VERSION_NEXTGI_TRANSLUCENCY_MARK = 13;
		constexpr static uint16_t SHADER_VERSION_NEXTGEO_FALLBACK = 14;
		constexpr static uint16_t SHADER_VERSION_COLOR_PASS = 15;
		// GodotNext: rr_guides (#205). The color pass block is SHADER_COLOR_PASS_FLAG_COUNT wide.
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_BASE = SHADER_VERSION_COLOR_PASS * 2 + (1 << 6);
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + 0;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + 1;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_COLOR = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + 2;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_SEPARATE_SPECULAR = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + 3;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_MOTION_VECTORS = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + 4;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_SEPARATE_SPECULAR_MOTION_VECTORS = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + 5;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_COUNT = 6;
		// GodotNext: megalights_inputs (#136). Appended after the visibility-resolve
		// block so SHADER_VERSION_COLOR_PASS and its flag indices stay put.
		constexpr static uint16_t SHADER_VERSION_MEGALIGHTS_INPUTS_BASE = SHADER_VERSION_VISIBILITY_RESOLVE_BASE + SHADER_VERSION_VISIBILITY_RESOLVE_COUNT;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 0;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 1;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 2;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 3;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_MULTIVIEW = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 4;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_MULTIVIEW = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 5;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION_MULTIVIEW = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 6;
		constexpr static uint16_t SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 7;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_ML = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 8;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 9;
		constexpr static uint16_t SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT = 10;
		// Ubershader twins of the eight depth variants. The render list falls back to these
		// while the specialized pipeline compiles, and that path supplies the 32-byte push constant.
		constexpr static uint16_t SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT = 8;
		// GodotNext: rr_guides (#205). The Stage B color variants with the RR guide outputs,
		// appended last so no earlier index moves. Same order as the four above.
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_BASE = SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT + SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_COUNT = 4;
		// GodotNext: vsm_non_nanite (#218). Specialized and ubershader VSM depth raster variants.
		constexpr static uint16_t SHADER_VERSION_VSM_BASE = SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_BASE + SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_COUNT;
		constexpr static uint16_t SHADER_VERSION_VSM_DEPTH = SHADER_VERSION_VSM_BASE + 0;
		constexpr static uint16_t SHADER_VERSION_VSM_DEPTH_UBERSHADER = SHADER_VERSION_VSM_BASE + 1;
		constexpr static uint16_t SHADER_VERSION_VSM_COUNT = 2;
		// GodotNext: avboit (#295). The bounds and occlusion pre-pass variants, then their ubershader
		// twins, appended last (after the VSM variants) so no earlier index moves.
		constexpr static uint16_t SHADER_VERSION_AVBOIT_BASE = SHADER_VERSION_VSM_BASE + SHADER_VERSION_VSM_COUNT;
		constexpr static uint16_t SHADER_VERSION_AVBOIT_BOUNDS = SHADER_VERSION_AVBOIT_BASE + 0;
		constexpr static uint16_t SHADER_VERSION_AVBOIT_OCCLUSION = SHADER_VERSION_AVBOIT_BASE + 1;
		constexpr static uint16_t SHADER_VERSION_AVBOIT_PASS_COUNT = 2;
		constexpr static uint16_t SHADER_VERSION_AVBOIT_COUNT = SHADER_VERSION_AVBOIT_PASS_COUNT * 2;
		// GodotNext: nextgeo_vertex_offset (#164). Stage A guide variants with motion-vector output so
		// VERTEX_OFFSET buckets write accurate velocity before NextGI and motion-pass upscalers.
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_BASE = SHADER_VERSION_AVBOIT_BASE + SHADER_VERSION_AVBOIT_COUNT;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION = SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_BASE + 0;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION = SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_BASE + 1;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION = SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_BASE + 2;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION = SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_BASE + 3;
		constexpr static uint16_t SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_COUNT = 4;
		// GodotNext: megalights_inputs (#136). init() pushes these groups, default off.
		static constexpr bool MEGALIGHTS_VARIANTS_DEFAULT_ENABLED = false;
		static constexpr ShaderGroup megalights_variant_group(uint16_t p_relative) {
			uint16_t index = p_relative;
			if (index >= SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT) {
				index = uint16_t(index - SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT);
			}
			if (index >= 8) {
				return SHADER_GROUP_NEXTGEO_MEGALIGHTS;
			}
			if (index >= 4) {
				return SHADER_GROUP_MEGALIGHTS_MULTIVIEW;
			}
			return SHADER_GROUP_MEGALIGHTS;
		}
		static const char *megalights_variant_defines(uint16_t p_relative);
	};

	enum ShaderColorPassFlags {
		SHADER_COLOR_PASS_FLAG_UBERSHADER = 1 << 0,
		SHADER_COLOR_PASS_FLAG_SEPARATE_SPECULAR = 1 << 1,
		SHADER_COLOR_PASS_FLAG_LIGHTMAP = 1 << 2,
		SHADER_COLOR_PASS_FLAG_MULTIVIEW = 1 << 3,
		SHADER_COLOR_PASS_FLAG_MOTION_VECTORS = 1 << 4,
		// GodotNext: rr_guides (#205). Adds the diffuse and specular albedo outputs.
		SHADER_COLOR_PASS_FLAG_RR_GUIDES = 1 << 5,
		SHADER_COLOR_PASS_FLAG_COUNT = 1 << 6
	};

	// GodotNext: rr_guides (#205). The group of a color pass variant that has the RR guide outputs.
	static constexpr ShaderGroup color_pass_variant_rr_guides_group(uint32_t p_shader_flags) {
		return (p_shader_flags & SHADER_COLOR_PASS_FLAG_MULTIVIEW) ? SHADER_GROUP_RR_GUIDES_MULTIVIEW : SHADER_GROUP_RR_GUIDES;
	}

	// Color attachments of the color pass: color, separate specular, velocity, then the
	// two RR guides (diffuse albedo, specular albedo) while a view writes them.
	enum ColorPassAttachment {
		COLOR_PASS_ATTACHMENT_VELOCITY = 2,
		COLOR_PASS_ATTACHMENTS = 3,
		COLOR_PASS_ATTACHMENTS_RR_GUIDES = 5,
	};

	enum PipelineVersion {
		PIPELINE_VERSION_DEPTH_PASS,
		PIPELINE_VERSION_DEPTH_PASS_DP,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL,
		PIPELINE_VERSION_DEPTH_PASS_WITH_SDF,
		PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW,
		PIPELINE_VERSION_NEXTGI_TRANSLUCENCY_MARK,
		PIPELINE_VERSION_NEXTGEO_FALLBACK,
		PIPELINE_VERSION_COLOR_PASS,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_COLOR,
		// GodotNext: megalights_inputs (#136)
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION_MULTIVIEW,
		PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML,
		// GodotNext: vsm_non_nanite (#218).
		PIPELINE_VERSION_VSM_DEPTH,
		// GodotNext: avboit (#295)
		PIPELINE_VERSION_AVBOIT_BOUNDS,
		PIPELINE_VERSION_AVBOIT_OCCLUSION,
		// GodotNext: nextgeo_vertex_offset (#164)
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION,
		PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION,
		PIPELINE_VERSION_MAX
	};

	// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The variants of the Stage A/B resolve's compute
	// form (resolve_compute_shader), one group each: Stage A's guides (+ voxel GI, + MegaLights inputs,
	// + motion vectors for #164 WPO) and Stage B's colour (+ separate specular, + motion vectors). The RR
	// guide targets have no storage usage, so Stage B with the guides keeps the tile quads and has no
	// compute variant.
	enum ResolveComputeVariant {
		RESOLVE_COMPUTE_GUIDES,
		RESOLVE_COMPUTE_GUIDES_VOXEL_GI,
		RESOLVE_COMPUTE_GUIDES_ML,
		RESOLVE_COMPUTE_GUIDES_VOXEL_GI_ML,
		RESOLVE_COMPUTE_COLOR,
		RESOLVE_COMPUTE_COLOR_SEPARATE_SPECULAR,
		RESOLVE_COMPUTE_COLOR_MOTION_VECTORS,
		RESOLVE_COMPUTE_COLOR_SEPARATE_SPECULAR_MOTION_VECTORS,
		// GodotNext: nextgeo_vertex_offset (#164). Stage A guides with velocity output for WPO.
		RESOLVE_COMPUTE_GUIDES_MOTION,
		RESOLVE_COMPUTE_GUIDES_VOXEL_GI_MOTION,
		RESOLVE_COMPUTE_GUIDES_ML_MOTION,
		RESOLVE_COMPUTE_GUIDES_VOXEL_GI_ML_MOTION,
		RESOLVE_COMPUTE_VARIANT_MAX
	};
	static constexpr ResolveComputeVariant resolve_compute_stage_a_variant(bool p_voxel_gi, bool p_megalights_inputs, bool p_motion_vectors = false) {
		const int base = p_motion_vectors ? int(RESOLVE_COMPUTE_GUIDES_MOTION) : int(RESOLVE_COMPUTE_GUIDES);
		return ResolveComputeVariant(base + (p_voxel_gi ? 1 : 0) + (p_megalights_inputs ? 2 : 0));
	}
	static constexpr ResolveComputeVariant resolve_compute_stage_b_variant(bool p_separate_specular, bool p_motion_vectors) {
		return ResolveComputeVariant(RESOLVE_COMPUTE_COLOR + (p_separate_specular ? 1 : 0) + (p_motion_vectors ? 2 : 0));
	}

	enum PipelineColorPassFlags {
		PIPELINE_COLOR_PASS_FLAG_TRANSPARENT = 1 << 0, // Can't combine with SEPARATE_SPECULAR.
		PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR = 1 << 1, // Can't combine with TRANSPARENT.
		PIPELINE_COLOR_PASS_FLAG_LIGHTMAP = 1 << 2,
		PIPELINE_COLOR_PASS_FLAG_MULTIVIEW = 1 << 3,
		PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS = 1 << 4,
		// GodotNext: rr_guides (#205). Opaque passes only; can't combine with TRANSPARENT or MULTIVIEW.
		PIPELINE_COLOR_PASS_FLAG_RR_GUIDES = 1 << 5,
		PIPELINE_COLOR_PASS_FLAG_OPTIONS = 6,
		PIPELINE_COLOR_PASS_FLAG_COMBINATIONS = 1 << PIPELINE_COLOR_PASS_FLAG_OPTIONS,
	};

	struct ShaderSpecialization {
		union {
			uint32_t packed_0;

			struct {
				uint32_t use_forward_gi : 1;
				uint32_t use_light_projector : 1;
				uint32_t use_light_soft_shadows : 1;
				uint32_t use_directional_soft_shadows : 1;
				uint32_t decal_use_mipmaps : 1;
				uint32_t projector_use_mipmaps : 1;
				uint32_t use_depth_fog : 1;
				uint32_t use_lightmap_bicubic_filter : 1;
				uint32_t soft_shadow_samples : 6;
				uint32_t penumbra_shadow_samples : 6;
				uint32_t directional_soft_shadow_samples : 6;
				uint32_t directional_penumbra_shadow_samples : 6;
			};
		};

		union {
			uint32_t packed_1;

			struct {
				uint32_t multimesh : 1;
				uint32_t multimesh_format_2d : 1;
				uint32_t multimesh_has_color : 1;
				uint32_t multimesh_has_custom_data : 1;
				uint32_t fog_use_legacy_blending : 1;
				uint32_t cluster_has_area_light : 1;
				uint32_t use_lightmap_specular : 1;
				uint32_t material_feedback : 1;
				// GodotNext: avboit (#295). RendererRD::AVBOIT::CompositeMode, 0 outside the AVBOIT composite.
				uint32_t avboit_composite : 3;
				uint32_t use_vsm : 1; // GodotNext: vsm_projection (#222). The view samples virtual shadow maps.
				uint32_t use_vsm_coarse : 1; // GodotNext: vsm_fog_translucency (#215). Transparent coarse-page VSM lookup.
				uint32_t use_df_shadows : 1; // GodotNext: df_shadows (#214). The view samples half-res DF shadow mask.
				uint32_t use_light_function : 1; // GodotNext: light_function_atlas (#138). Sample Light Function Atlas.
			};
		};

		uint32_t packed_2;
	};

	struct UbershaderConstants {
		union {
			uint32_t packed_0;

			struct {
				uint32_t cull_mode : 2;
			};
		};
	};

	struct ShaderData : public RendererRD::MaterialStorage::ShaderData {
		enum DepthDraw {
			DEPTH_DRAW_DISABLED,
			DEPTH_DRAW_OPAQUE,
			DEPTH_DRAW_ALWAYS
		};

		enum DepthTest {
			DEPTH_TEST_DISABLED,
			DEPTH_TEST_ENABLED,
			DEPTH_TEST_ENABLED_INVERTED,
		};

		enum CullVariant {
			CULL_VARIANT_NORMAL,
			CULL_VARIANT_REVERSED,
			CULL_VARIANT_DOUBLE_SIDED,
			CULL_VARIANT_MAX

		};

		enum AlphaAntiAliasing {
			ALPHA_ANTIALIASING_OFF,
			ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE,
			ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE
		};

		enum StencilFlags {
			STENCIL_FLAG_READ = 1,
			STENCIL_FLAG_WRITE = 2,
			STENCIL_FLAG_WRITE_DEPTH_FAIL = 4,
		};

		enum StencilCompare {
			STENCIL_COMPARE_LESS,
			STENCIL_COMPARE_EQUAL,
			STENCIL_COMPARE_LESS_OR_EQUAL,
			STENCIL_COMPARE_GREATER,
			STENCIL_COMPARE_NOT_EQUAL,
			STENCIL_COMPARE_GREATER_OR_EQUAL,
			STENCIL_COMPARE_ALWAYS,
			STENCIL_COMPARE_MAX // Not an actual operator, just the amount of operators.
		};

		struct PipelineKey {
			RD::VertexFormatID vertex_format_id;
			RD::FramebufferFormatID framebuffer_format_id;
			RD::PolygonCullMode cull_mode = RD::POLYGON_CULL_MAX;
			RSE::PrimitiveType primitive_type = RSE::PRIMITIVE_MAX;
			PipelineVersion version = PipelineVersion::PIPELINE_VERSION_MAX;
			uint32_t color_pass_flags = 0;
			ShaderSpecialization shader_specialization = {};
			uint32_t wireframe = false;
			uint32_t ubershader = false;
			uint32_t zero_velocity_write = false;
			// GodotNext: avboit (#295). The AVBOIT composite's blend and depth state. A key field and not
			// only a specialization bit, because the ubershader key clears the specialization.
			uint32_t avboit = false;

			uint32_t hash() const {
				uint32_t h = hash_murmur3_one_64(vertex_format_id);
				h = hash_murmur3_one_32(framebuffer_format_id, h);
				h = hash_murmur3_one_32(cull_mode, h);
				h = hash_murmur3_one_32(primitive_type, h);
				h = hash_murmur3_one_32(version, h);
				h = hash_murmur3_one_32(color_pass_flags, h);
				h = hash_murmur3_one_32(shader_specialization.packed_0, h);
				h = hash_murmur3_one_32(shader_specialization.packed_1, h);
				h = hash_murmur3_one_32(shader_specialization.packed_2, h);
				h = hash_murmur3_one_32(wireframe, h);
				h = hash_murmur3_one_32(ubershader, h);
				h = hash_murmur3_one_32(zero_velocity_write, h);
				// GodotNext: avboit (#295). Mixed in only when set, so every other key hashes as before.
				if (avboit) {
					h = hash_murmur3_one_32(avboit, h);
				}
				return hash_fmix32(h);
			}
		};

		void _create_pipeline(PipelineKey p_pipeline_key);
		PipelineHashMapRD<PipelineKey, ShaderData, void (ShaderData::*)(PipelineKey)> pipeline_hash_map;

		RID version;

		// GodotNext: megalights_inputs (#136)
		// GodotNext: rr_guides (#205). One entry per shader variant, the Stage B RR variants included.
		// GodotNext: vsm_non_nanite (#218). Plus the VSM depth variants.
		// GodotNext: avboit (#295). And the AVBOIT pre-pass variants.
		// GodotNext: nextgeo_vertex_offset (#164). And the Stage A motion variants for WPO.
		static const uint32_t VERTEX_INPUT_MASKS_SIZE = ShaderVersion::SHADER_VERSION_COLOR_PASS * 2 + SHADER_COLOR_PASS_FLAG_COUNT + ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_COUNT + ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT + ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT + ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_COUNT + ShaderVersion::SHADER_VERSION_VSM_COUNT + ShaderVersion::SHADER_VERSION_AVBOIT_COUNT + ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION_COUNT;
		std::atomic<uint64_t> vertex_input_masks[VERTEX_INPUT_MASKS_SIZE] = {};

		Vector<ShaderCompiler::GeneratedCode::Texture> texture_uniforms;

		Vector<uint32_t> ubo_offsets;
		uint32_t ubo_size = 0;

		String code;

		DepthDraw depth_draw = DEPTH_DRAW_OPAQUE;
		DepthTest depth_test = DEPTH_TEST_ENABLED;

		int blend_mode = BLEND_MODE_MIX;
		int depth_test_disabledi = 0;
		int depth_test_invertedi = 0;
		int alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;

		bool uses_point_size = false;
		bool uses_alpha = false;
		bool uses_blend_alpha = false;
		bool nextgi_rt_opaque = false;
		bool uses_alpha_clip = false;
		// GodotNext: nextgeo_masked_two_sided (#153). Separate scissor and hash usage flags.
		bool uses_alpha_scissor = false;
		bool uses_alpha_hash = false;
		bool uses_alpha_antialiasing = false;
		bool uses_depth_prepass_alpha = false;
		// GodotNext: avboit (#295). The shadow_to_opacity render mode: its alpha comes from lighting.
		bool uses_shadow_to_opacity = false;
		bool uses_discard = false;
		bool uses_roughness = false;
		bool uses_normal = false;
		bool uses_tangent = false;
		bool writes_tangent = false;
		bool uses_particle_trails = false;
		bool uses_normal_map = false;
		bool uses_bent_normal_map = false;
		bool wireframe = false;

		bool unshaded = false;
		// GodotNext: megalights_inputs (#136). One editor warning per shader.
		bool megalights_fallback_warned = false;
		bool uses_vertex = false;
		bool uses_position = false;
		bool uses_sss = false;
		bool uses_transmittance = false;
		bool uses_screen_texture = false;
		bool uses_depth_texture = false;
		bool uses_normal_texture = false;
		bool uses_time = false;
		bool uses_vertex_time = false;
		bool uses_fragment_time = false;
		bool writes_modelview_or_projection = false;
		bool uses_main_camera = false; // GodotNext: vsm_non_nanite (#218). MAIN_CAM_INV_VIEW_MATRIX.
		bool uses_world_coordinates = false;
		bool uses_screen_texture_mipmaps = false;
		bool uses_z_clip_scale = false;
		bool uses_vertex_lighting = false;
		bool uses_skip_vertex_transform = false;
		bool writes_depth = false;
		// GodotNext: depth_offset (#149, plan §21.4 contract N6). Relative view-space depth push (>= 0.0).
		bool uses_depth_offset = false;
		bool uses_unsupported_derivative = false;
		RendererRD::NextGeoFallback nextgeo_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
		// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). The material's raster features, what its
		// raster target rejects, its fallback reason when rendering/nextgeo/debug_programmable_raster admits
		// programmable raster, and its raster target (the code, kept only for a material a programmable bin
		// can take, and the software and hardware forms' versions, made the first time a bin takes it).
		uint32_t raster_features = 0;
		// GodotNext: nextgeo_displacement (#150). Raw ShaderCompiler::RasterTargetUse bits + displacement metadata/hook for #163.
		uint32_t raster_uses = 0;
		uint32_t raster_rejects = 0;
		bool has_displacement = false;
		bool uses_displacement_time = false;
		String displacement_code;
		// GodotNext: nextgeo_masked_two_sided (#153). Fallback when masked programmable bins are admitted
		// by default on the Nanite device tier (vertex offset still requires debug_programmable_raster).
		RendererRD::NextGeoFallback nextgeo_masked_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
		RendererRD::NextGeoFallback nextgeo_programmable_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
		bool raster_target_valid = false;
		HashMap<String, String> raster_code;
		String raster_globals;
		String raster_uniforms;
		Vector<String> raster_defines;
		RID raster_sw_version;
		RID raster_hw_version;
		// GodotNext: nextgeo_mesh_raster (#154). Mesh-shader form of the HW material raster target.
		RID raster_hw_mesh_version;
		// GodotNext: nextgeo_vertex_offset (#164). Lazily compiled RT proxy vertex-offset compute shader.
		RID rt_vertex_offset_version;
		bool nextgeo_raster_reject_logged = false;
		// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The Stage A/B resolve's compute form: whether
		// the material has one (a material NextGeo resolves, and whose code cannot discard or write DEPTH),
		// whether it failed to compile or make a pipeline (it then keeps the tile quads until its code
		// changes), its version, made from the scene shader version's code when a shading bin first takes the
		// material, and its pipelines.
		bool resolve_compute_valid = false;
		bool resolve_compute_failed = false;
		RID resolve_compute_version;
		struct ResolveComputePipelineKey {
			uint32_t variant = 0;
			uint32_t spec_packed_0 = 0;
			uint32_t spec_packed_1 = 0;
			bool operator==(const ResolveComputePipelineKey &p_other) const {
				return variant == p_other.variant && spec_packed_0 == p_other.spec_packed_0 && spec_packed_1 == p_other.spec_packed_1;
			}
			uint32_t hash() const {
				uint32_t h = hash_murmur3_one_32(variant);
				h = hash_murmur3_one_32(spec_packed_0, h);
				h = hash_murmur3_one_32(spec_packed_1, h);
				return hash_fmix32(h);
			}
		};
		HashMap<ResolveComputePipelineKey, RID> resolve_compute_pipelines;
		RSE::CullMode cull_mode = RSE::CULL_MODE_DISABLED;

		bool stencil_enabled = false;
		uint32_t stencil_flags = 0;
		StencilCompare stencil_compare = STENCIL_COMPARE_LESS;
		uint32_t stencil_reference = 0;

		uint64_t last_pass = 0;
		uint32_t index = 0;

		_FORCE_INLINE_ bool uses_alpha_pass() const {
			bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
			bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
			bool has_blend_alpha = uses_blend_alpha;
			bool has_alpha = has_base_alpha || has_blend_alpha;
			bool no_depth_draw = depth_draw == DEPTH_DRAW_DISABLED;
			bool no_depth_test = depth_test != DEPTH_TEST_ENABLED;
			return has_alpha || has_read_screen_alpha || no_depth_draw || no_depth_test;
		}

		_FORCE_INLINE_ bool uses_depth_in_alpha_pass() const {
			bool no_depth_draw = depth_draw == DEPTH_DRAW_DISABLED;
			bool no_depth_test = depth_test != DEPTH_TEST_ENABLED;
			return (uses_depth_prepass_alpha || uses_alpha_antialiasing) && !(no_depth_draw || no_depth_test);
		}

		_FORCE_INLINE_ bool uses_shared_shadow_material() const {
			bool backface_culling = cull_mode == RSE::CULL_MODE_BACK;
			return !uses_particle_trails && !writes_modelview_or_projection && !uses_vertex && !uses_position && !uses_discard && !uses_depth_prepass_alpha && !uses_alpha_clip && !uses_alpha_antialiasing && backface_culling && !uses_point_size && !uses_world_coordinates && !wireframe && !uses_z_clip_scale && !stencil_enabled && !uses_depth_offset;
		}

		virtual void set_code(const String &p_Code) override;

		virtual bool is_animated() const override;
		virtual bool casts_shadows() const override;
		// GodotNext: nextgi_alpha_policy (#61).
		virtual RendererRD::MaterialStorage::ShaderData::NextGIRTClass get_nextgi_rt_class() const override;
		virtual uint32_t get_nextgeo_fallback() const override { return uint32_t(nextgeo_fallback); }
		virtual uint32_t get_raster_features() const override { return raster_features; }
		// GodotNext: nextgeo_rt_native (#156)
		virtual bool nextgeo_moves_vertices() const override { return uses_vertex || uses_position || writes_modelview_or_projection || uses_skip_vertex_transform; }
		// GodotNext: nextgeo_vertex_offset (#164)
		virtual bool nextgeo_writes_vertex() const override { return uses_vertex; }
		virtual RID get_rt_vertex_offset_shader() override;
		// GodotNext: path_tracer (#167).
		virtual RSE::CullMode get_cull_mode() const override { return cull_mode; }
		// GodotNext: vsm_caching (#212, plan §21.3 contract S5).
		virtual uint32_t get_vsm_dynamic_flags() const override;
		// GodotNext: nextgeo_raster_bins (#155). True when a programmable raster bin can take the material on
		// this device.
		bool has_raster_target() const { return raster_target_valid && SceneShaderForwardClustered::singleton != nullptr && SceneShaderForwardClustered::singleton->raster_target_ready; }
		// Makes the raster target's versions (groups stay disabled until first runtime use or shader baking).
		bool ensure_raster_target();
		// The raster target's shader for one form (software or hardware) and visibility-buffer storage
		// (image or buffer); invalid when this device does not build that storage.
		RID get_raster_target_shader(bool p_hardware, bool p_buffer) const;
		// GodotNext: vsm_nanite (#217). Depth-only VSM form (group 2): 32-bit imageAtomicMax, no 64-bit atomics.
		RID get_raster_target_vsm_shader(bool p_hardware) const;
		// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9). Mesh-shader forms of the HW
		// material raster target (groups 0/1 = VIS64 image/buffer, group 2 = VSM depth-only).
		RID get_raster_target_mesh_shader(bool p_buffer) const;
		RID get_raster_target_vsm_mesh_shader() const;
		void _free_raster_target();
		// GodotNext: nextgeo_shading_bins (#158). The compute form's shader and pipeline for one variant and
		// specialization; invalid when the material has no compute form or it did not compile, which also
		// marks the compute form failed.
		bool has_resolve_compute() const { return resolve_compute_valid && !resolve_compute_failed; }
		RID get_resolve_compute_shader(ResolveComputeVariant p_variant);
		RID get_resolve_compute_pipeline(ResolveComputeVariant p_variant, ShaderSpecialization p_specialization);
		void _free_resolve_compute();
		virtual RenderingServerTypes::ShaderNativeSourceCode get_native_source_code() const override;
		virtual Pair<ShaderRD *, RID> get_native_shader_and_version() const override;
		virtual void get_extra_native_shader_and_versions(LocalVector<Pair<ShaderRD *, RID>> &r_versions) override;
		uint16_t _get_shader_version(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const;
		RID _get_shader_variant(uint16_t p_shader_version) const;
		void _clear_vertex_input_mask_cache();
		RID get_shader_variant(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const;
		uint64_t get_vertex_input_mask(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader);
		RD::PolygonCullMode get_cull_mode_from_cull_variant(CullVariant p_cull_variant);
		bool is_valid() const;

		// GodotNext: nextgi_hit_groups (#130). Unique across every shader and every set_code(), so NextGI's hit
		// groups know when a shader's code changed. 0 until the shader has code.
		uint64_t nextgi_hit_serial = 0;
		// The NextGI hit variant: the ShaderCompiler output of p_code in nextgi_hit mode. Takes the compiler's
		// mutex, as set_code() does, so any thread may call it (NextGI calls it once per serial on a worker).
		static bool build_nextgi_hit_source(const String &p_code, const String &p_path, RendererRD::NextGIHitShaderSource &r_source);

		SelfList<ShaderData> shader_list_element;
		ShaderData();
		virtual ~ShaderData();
	};

	SelfList<ShaderData>::List shader_list;

	RendererRD::MaterialStorage::ShaderData *_create_shader_func();
	static RendererRD::MaterialStorage::ShaderData *_create_shader_funcs() {
		return static_cast<SceneShaderForwardClustered *>(singleton)->_create_shader_func();
	}

	struct MaterialData : public RendererRD::MaterialStorage::MaterialData {
		ShaderData *shader_data = nullptr;
		RID uniform_set;
		uint64_t last_pass = 0;
		uint32_t index = 0;
		RID next_pass;
		uint8_t priority;
		// GodotNext: nextgeo_displacement (#150, plan §21.4 N11).
		float displacement_magnitude = 0.0f;
		float displacement_center = 0.5f;
		// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5). -1.0 inherits project default.
		float vertex_offset_max = -1.0f;
		virtual void set_render_priority(int p_priority);
		virtual void set_next_pass(RID p_pass);
		virtual void set_displacement(float p_magnitude, float p_center) override;
		virtual void set_vertex_offset_max(float p_vertex_offset_max) override;
		float get_effective_vertex_offset_max(float p_project_default) const;
		virtual bool get_rt_vertex_offset_uniforms(Vector<RD::Uniform> &r_uniforms) const override;
		uint32_t get_raster_features() const;
		virtual bool update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty);
		virtual ~MaterialData();
	};

	RendererRD::MaterialStorage::MaterialData *_create_material_func(ShaderData *p_shader);
	static RendererRD::MaterialStorage::MaterialData *_create_material_funcs(RendererRD::MaterialStorage::ShaderData *p_shader) {
		return static_cast<SceneShaderForwardClustered *>(singleton)->_create_material_func(static_cast<ShaderData *>(p_shader));
	}

	SceneForwardClusteredShaderRD shader;
	ShaderCompiler compiler;
	// GodotNext: nextgeo_raster_bins (#155). The material raster target's two forms (group 0: image,
	// group 1: buffer visibility-buffer storage). Initialized unconditionally for the shader baker; groups
	// enable on demand on devices with matching 64-bit atomic support.
	RasterMaterialSwShaderRD raster_sw_shader;
	RasterMaterialHwShaderRD raster_hw_shader;
	// GodotNext: nextgeo_mesh_raster (#154). Mesh-shader form of the HW material raster target.
	RasterMaterialHwMeshShaderRD raster_hw_mesh_shader;
	// GodotNext: nextgeo_shading_bins (#158). The Stage A/B resolve's compute form, one group per
	// ResolveComputeVariant; groups enable when a shading bin first uses them.
	ResolveComputeShaderRD resolve_compute_shader;
	// GodotNext: nextgeo_vertex_offset (#164). Opt-in RT proxy vertex-offset compute shader.
	RtVertexOffsetShaderRD rt_vertex_offset_shader;
	bool raster_target_ready = false;
	bool raster_image_int64_supported = false;
	bool raster_buffer_int64_supported = false;
	// GodotNext: vsm_nanite (#217). Group 2 needs only 32-bit image atomics.
	bool raster_vsm_supported = false;
	// GodotNext: nextgeo_mesh_raster (#154). Device supports indirect mesh-shader HW raster.
	bool raster_mesh_supported = false;
	bool emulate_point_size = false;
	bool depth_prepass_enabled = false;

	RID default_shader;
	RID default_material;
	RID overdraw_material_shader;
	RID overdraw_material;
	RID debug_shadow_splits_material_shader;
	RID debug_shadow_splits_material;
	RID default_shader_rd;
	RID default_multiview_shader_rd; // This is lazily initialized, use "get_default_shader_rd" instead.
	RID default_shader_sdfgi_rd;
	RID default_shader_vsm_rd; // GodotNext: vsm_non_nanite (#218)

	RID default_vec4_xform_buffer;
	RID default_vec4_xform_uniform_set;
	RID default_material_feedback_buffer;

	RID shadow_sampler;

	RID default_material_uniform_set;
	ShaderData *default_material_shader_ptr = nullptr;

	RID overdraw_material_uniform_set;
	ShaderData *overdraw_material_shader_ptr = nullptr;

	RID debug_shadow_splits_material_uniform_set;
	ShaderData *debug_shadow_splits_material_shader_ptr = nullptr;

	ShaderSpecialization default_specialization = {};

	uint32_t pipeline_compilations[RSE::PIPELINE_SOURCE_MAX] = {};

	SceneShaderForwardClustered();
	~SceneShaderForwardClustered();

	void init(const String p_defines);
	void set_default_specialization(const ShaderSpecialization &p_specialization);
	void enable_multiview_shader_group();
	void enable_advanced_shader_group(bool p_needs_multiview = false);
	void enable_nextgeo_shader_group();
	// GodotNext: megalights_inputs (#136)
	void enable_megalights_shader_group(bool p_needs_multiview = false);
	void enable_nextgeo_megalights_shader_group();
	// GodotNext: rr_guides (#205)
	void enable_rr_guides_shader_group();
	void enable_nextgeo_rr_guides_shader_group();
	// GodotNext: vsm_non_nanite (#218)
	void enable_vsm_shader_group();
	bool is_vsm_shader_group_enabled() const;
	// GodotNext: avboit (#295)
	void enable_avboit_shader_group();
	// GodotNext: nextgeo_vertex_offset (#164)
	void enable_nextgeo_motion_shader_group(bool p_megalights_inputs = false);
	bool is_multiview_shader_group_enabled() const;
	bool is_advanced_shader_group_enabled(bool p_multiview) const;
	bool is_nextgeo_shader_group_enabled() const;
	uint32_t get_pipeline_compilations(RSE::PipelineSource p_source);
	RID get_default_shader_rd(bool p_is_multiview = false);
};

} // namespace RendererSceneRenderImplementation
