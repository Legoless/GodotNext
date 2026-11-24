#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, GM-1, GM-2, budget M-3).
// Per-sampling-pixel weighted reservoir light sampling over the #139 packed froxel light grid.
// Iterates the packed cell list once (plus optional directional lights), applies falloff early cull
// and last-frame 8x8 tile visible-light guiding (with a strictly positive weight floor for occluded
// lights so the Monte Carlo estimator stays unbiased), maintains K reservoirs, and writes 16-byte
// (light_packed, 1/pdf, oct(ray_dir_ws), max_distance) samples for #146 visibility & #135 denoise.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define ML_DECLARE_COMMON_BINDINGS
#include "ml_light_eval_inc.glsl"

layout(set = 0, binding = 20, std430) restrict readonly buffer MLPackedGridHeaders {
	uvec2 data[];
}
packed_grid_headers;

layout(set = 0, binding = 21, std430) restrict readonly buffer MLPackedGridIndices {
	uint data[];
}
packed_grid_indices;

layout(set = 0, binding = 22, std430) restrict readonly buffer MLLayerGroupMasks {
	uint data[];
}
layer_group_masks;

layout(set = 0, binding = 23, std430) restrict readonly buffer MLGuidingTilesIn {
	uint data[];
}
guiding_tiles_in;

layout(set = 0, binding = 24, std430) restrict writeonly buffer MLSampleBufferOut {
	uvec4 data[];
}
sample_buffer_out;

// Cooperative shared cache of the guiding tiles overlapped by this 8x8 workgroup:
// 1 tile in full-res (8x8 px = 1 tile), up to 2x2 = 4 tiles in half-res (16x16 px = 2x2 tiles).
shared uint s_guiding[4][ML_GUIDING_TILE_WORDS];

float ml_tile_guiding_weight(uint p_slot, uint p_light_id, float p_floor) {
	uint total_valid = s_guiding[p_slot][2];
	if (total_valid == 0u) {
		return 1.0; // No history for this tile yet: uniform weights.
	}
	uint entry_count = min(s_guiding[p_slot][0], ML_GUIDING_MAX_LIGHTS);
	uint max_count = max(s_guiding[p_slot][3], 1u);
	for (uint i = 0u; i < entry_count; i++) {
		uint packed_val = s_guiding[p_slot][ML_GUIDING_HEADER_WORDS + i];
		if ((packed_val & ML_GUIDING_LIGHT_MASK) == p_light_id) {
			uint vis_count = packed_val >> ML_GUIDING_COUNT_SHIFT;
			float confidence = clamp(float(vis_count) / float(max_count), 0.25, 1.0);
			return mix(p_floor, 1.0, confidence);
		}
	}
	// Occluded / unseen light retains strictly positive minimum weight floor so estimator stays unbiased.
	return p_floor;
}

void main() {
	bool downsample_half = params.sample_config.y != 0u;
	bool has_guiding = (params.sample_config.w & ML_FLAG_HAS_GUIDING_HISTORY) != 0u;
	uint lid = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;

	uvec2 wg_full_origin = downsample_half ? (gl_WorkGroupID.xy * 16u) : (gl_WorkGroupID.xy * 8u);
	uvec2 base_tile = wg_full_origin >> ML_GUIDING_TILE_SHIFT;
	uint tile_slots = downsample_half ? 4u : 1u;

	if (has_guiding) {
		uint total_words = tile_slots * ML_GUIDING_TILE_WORDS;
		for (uint w = lid; w < total_words; w += 64u) {
			uint slot = w / ML_GUIDING_TILE_WORDS;
			uint word_idx = w - slot * ML_GUIDING_TILE_WORDS;
			uvec2 t_coord = base_tile + uvec2(slot & 1u, slot >> 1u);
			if (t_coord.x < params.guiding_tile_info.x && t_coord.y < params.guiding_tile_info.y) {
				uint g_idx = (t_coord.y * params.guiding_tile_info.x + t_coord.x) * ML_GUIDING_TILE_WORDS + word_idx;
				s_guiding[slot][word_idx] = guiding_tiles_in.data[g_idx];
			} else {
				s_guiding[slot][word_idx] = 0u;
			}
		}
	}
	barrier();

	uvec2 spx = gl_GlobalInvocationID.xy;
	if (spx.x >= params.screen_sample_size.z || spx.y >= params.screen_sample_size.w) {
		return;
	}

	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint out_base = (spx.y * params.screen_sample_size.z + spx.x) * spp;
	uint frame = params.sample_config.z;

	uvec2 fpx = ml_sample_to_full_pixel(spx, params.screen_sample_size.xy, downsample_half, frame);
	uint layer_group = ml_read_layer_group_index(fpx);
	uint layer_mask = (layer_group < params.guiding_tile_info.z) ? layer_group_masks.data[layer_group] : 0xFFFFFFFFu;

	MLSurface surf;
	if (!ml_unpack_surface(fpx, layer_mask, surf)) {
		for (uint k = 0u; k < spp; k++) {
			sample_buffer_out.data[out_base + k] = uvec4(ML_SAMPLE_INVALID, 0u, 0u, 0u);
		}
		return;
	}

	uvec2 px_tile = fpx >> ML_GUIDING_TILE_SHIFT;
	uint local_tile_slot = 0u;
	if (downsample_half) {
		uvec2 rel = min(px_tile - base_tile, uvec2(1u));
		local_tile_slot = rel.y * 2u + rel.x;
	}

	float falloff_threshold = params.thresholds.x;
	float guiding_floor = clamp(params.thresholds.y, 0.01, 1.0);
	bool use_lf = (params.sample_config.w & ML_FLAG_USE_LIGHT_FUNCTION) != 0u;
	bool use_soft_shadows = (params.sample_config.w & ML_FLAG_USE_SOFT_SHADOWS) != 0u;
	bool include_directional = (params.sample_config.w & ML_FLAG_DIRECTIONAL_LIGHTS) != 0u;

	uint res_light[ML_MAX_SAMPLES_PER_PIXEL];
	float res_weight[ML_MAX_SAMPLES_PER_PIXEL];
	for (uint k = 0u; k < spp; k++) {
		res_light[k] = ML_SAMPLE_INVALID;
		res_weight[k] = 0.0;
	}
	float w_sum = 0.0;

	// Single pass over this pixel's packed froxel cell list.
	if ((params.grid_dims_flags.w & ML_LIGHT_GRID_FLAG_ENABLED) != 0u) {
		uint cell_idx = ml_light_grid_cell_index(fpx, -surf.view_pos.z);
		uvec2 cell_header = packed_grid_headers.data[cell_idx];
		uint list_offset = cell_header.x;
		uint list_count = cell_header.y;

		for (uint i = 0u; i < list_count; i++) {
			uint entry = packed_grid_indices.data[list_offset + i];
			// GodotNext: megalights_frame (#143). Skip lights excluded via Light3D.megalights = DISALLOW.
			if ((entry & (1u << 18u)) == 0u) {
				continue;
			}
			uint ltype = (entry >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
			uint lidx = entry & ML_SAMPLE_INDEX_MASK;
			uint light_id = ml_pack_sample_light(ltype, lidx);

			MLLightEval ev = ml_eval_packed_light(light_id, surf, falloff_threshold, use_soft_shadows, use_lf, vec2(0.5), false);
			if (!ev.valid) {
				continue;
			}

			float w_base = ml_luminance(abs(ev.diffuse + ev.specular));
			if (!(w_base > 0.0)) {
				continue;
			}

			float g_mult = has_guiding ? ml_tile_guiding_weight(local_tile_slot, light_id, guiding_floor) : 1.0;
			float w = w_base * g_mult;
			w_sum += w;
			float accept_prob = w / w_sum;

			for (uint k = 0u; k < spp; k++) {
				float u = ml_stbn_select_u(spx, frame, light_id * 17u + k * 977u + 1u);
				if (u < accept_prob) {
					res_light[k] = light_id;
					res_weight[k] = w;
				}
			}
		}
	}

	// Directional lights: included when rendering/megalights/directional_lights is enabled (unless DISALLOW)
	// or per-light Light3D.megalights == ALLOW (carried via LIGHT_FLAG_MEGALIGHTS).
	{
		uint dir_count = min(params.light_counts.w, uint(MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS));
		for (uint d = 0u; d < dir_count; d++) {
			uint dflags = ml_directional_lights.data[d].light_flags;
			bool d_ml = ((dflags & LIGHT_FLAG_MEGALIGHTS) != 0u) || (include_directional && (dflags & LIGHT_FLAG_MEGALIGHTS_DISALLOW) == 0u);
			if (!d_ml) {
				continue;
			}
			uint light_id = ml_pack_sample_light(ML_LIGHT_TYPE_DIRECTIONAL, d);
			MLLightEval ev = ml_eval_directional(ml_directional_lights.data[d], surf, use_soft_shadows, use_lf, vec2(0.5), false);
			if (!ev.valid) {
				continue;
			}
			float w_base = ml_luminance(abs(ev.diffuse + ev.specular));
			if (!(w_base > 0.0)) {
				continue;
			}

			float g_mult = has_guiding ? ml_tile_guiding_weight(local_tile_slot, light_id, guiding_floor) : 1.0;
			float w = w_base * g_mult;
			w_sum += w;
			float accept_prob = w / w_sum;

			for (uint k = 0u; k < spp; k++) {
				float u = ml_stbn_select_u(spx, frame, light_id * 17u + k * 977u + 1u);
				if (u < accept_prob) {
					res_light[k] = light_id;
					res_weight[k] = w;
				}
			}
		}
	}

	// Emit K packed 16-byte samples (light_packed, 1/pdf, oct(ray_dir_ws), max_distance).
	for (uint k = 0u; k < spp; k++) {
		uint chosen = res_light[k];
		float chosen_w = res_weight[k];
		if (chosen != ML_SAMPLE_INVALID && chosen_w > 0.0 && w_sum > 0.0) {
			vec2 u_emit = ml_stbn_sample_uv(spx, frame, chosen * 31u + k * 1301u + 53u);
			MLLightEval ray_ev = ml_eval_packed_light(chosen, surf, 0.0, use_soft_shadows, false, u_emit, true);
			float inv_pdf = w_sum / chosen_w;
			sample_buffer_out.data[out_base + k] = uvec4(
					chosen,
					floatBitsToUint(inv_pdf),
					ml_pack_oct(ray_ev.ray_dir_ws),
					floatBitsToUint(ray_ev.ray_dist));
		} else {
			sample_buffer_out.data[out_base + k] = uvec4(ML_SAMPLE_INVALID, 0u, 0u, 0u);
		}
	}
}
