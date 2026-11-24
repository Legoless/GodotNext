#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, budget M-3).
// Per-8x8-tile visible-light guiding aggregation pass.
// Runs after visibility (#146, or forced-visible=1 test mode): reads the sample buffer
// and packed RGBA8 visibility buffer, aggregates up to 32 distinct unoccluded light IDs
// with their visible sample counts per 8x8 full-resolution tile (deterministically sorted
// by highest count, tie-break by lowest packed light ID), and writes the tile table for
// next frame's ml_generate_samples.glsl.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define ML_SAMPLE_INVALID 0xFFFFFFFFu
#define ML_GUIDING_MAX_LIGHTS 32u
#define ML_GUIDING_HASH_SLOTS 64u
#define ML_GUIDING_TILE_WORDS 36u
#define ML_GUIDING_LIGHT_MASK 0x3FFFFu
#define ML_GUIDING_COUNT_SHIFT 18u
#define ML_GUIDING_COUNT_MAX 16383u

#define ML_GUIDING_FLAG_PHASE_MASK 3u
#define ML_GUIDING_FLAG_DOWNSAMPLE_HALF (1u << 2u)
#define ML_GUIDING_FLAG_FORCE_VISIBLE_ONE (1u << 3u)

layout(push_constant, std430) uniform GuidingPush {
	uvec2 full_size;
	uvec2 sample_size;
	uvec2 tile_dims;
	uint samples_per_pixel;
	uint flags;
}
params;

layout(set = 0, binding = 0, std430) restrict readonly buffer SampleBufferIn {
	uvec4 data[];
}
sample_buffer;

// Packed RGBA8 transmittance per sample from #146 visibility chain (1 uint per sample).
layout(set = 0, binding = 1, std430) restrict readonly buffer VisibilityBufferIn {
	uint data[];
}
visibility_buffer;

layout(set = 0, binding = 2, std430) restrict writeonly buffer GuidingTilesOut {
	uint data[];
}
guiding_tiles_out;

shared uint s_lights[ML_GUIDING_HASH_SLOTS];
shared uint s_counts[ML_GUIDING_HASH_SLOTS];
shared uint s_total_visible;
shared uint s_total_valid;

uvec2 ml_phase_offset(uint p_phase) {
	// Bayer 2x2 rotation matching MegaLights::downsample_offset() and ml_light_eval_inc.glsl.
	if (p_phase == 1u) {
		return uvec2(1u, 1u);
	}
	if (p_phase == 2u) {
		return uvec2(1u, 0u);
	}
	if (p_phase == 3u) {
		return uvec2(0u, 1u);
	}
	return uvec2(0u, 0u);
}

void ml_insert_visible_light(uint p_light_id) {
	uint h = ((p_light_id * 2654435761u) >> 26u) & (ML_GUIDING_HASH_SLOTS - 1u);
	for (uint probe = 0u; probe < ML_GUIDING_HASH_SLOTS; probe++) {
		uint slot = (h + probe) & (ML_GUIDING_HASH_SLOTS - 1u);
		uint prev = atomicCompSwap(s_lights[slot], ML_SAMPLE_INVALID, p_light_id);
		if (prev == ML_SAMPLE_INVALID || prev == p_light_id) {
			atomicAdd(s_counts[slot], 1u);
			return;
		}
	}
}

bool ml_entry_better(uint p_count_a, uint p_light_a, uint p_count_b, uint p_light_b) {
	// Valid entries (count > 0) before empty entries; higher count first; lower light ID breaks ties.
	if (p_count_a != p_count_b) {
		return p_count_a > p_count_b;
	}
	return p_light_a < p_light_b;
}

void main() {
	uvec2 tile = gl_WorkGroupID.xy;
	if (tile.x >= params.tile_dims.x || tile.y >= params.tile_dims.y) {
		return;
	}

	uint lid = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;
	s_lights[lid] = ML_SAMPLE_INVALID;
	s_counts[lid] = 0u;
	if (lid == 0u) {
		s_total_visible = 0u;
		s_total_valid = 0u;
	}
	barrier();

	uvec2 full_px = tile * 8u + gl_LocalInvocationID.xy;
	bool in_bounds = full_px.x < params.full_size.x && full_px.y < params.full_size.y;
	bool downsample_half = (params.flags & ML_GUIDING_FLAG_DOWNSAMPLE_HALF) != 0u;
	bool force_visible = (params.flags & ML_GUIDING_FLAG_FORCE_VISIBLE_ONE) != 0u;

	if (in_bounds) {
		bool is_sampled_pixel = true;
		uvec2 sample_px = full_px;
		if (downsample_half) {
			uvec2 off = ml_phase_offset(params.flags & ML_GUIDING_FLAG_PHASE_MASK);
			uvec2 block = full_px >> 1u;
			uvec2 expected_full = min(block * 2u + off, params.full_size - uvec2(1u));
			is_sampled_pixel = all(equal(full_px, expected_full));
			sample_px = block;
		}

		if (is_sampled_pixel && sample_px.x < params.sample_size.x && sample_px.y < params.sample_size.y) {
			uint base_idx = (sample_px.y * params.sample_size.x + sample_px.x) * params.samples_per_pixel;
			for (uint k = 0u; k < params.samples_per_pixel; k++) {
				uint s_idx = base_idx + k;
				uint light_packed = sample_buffer.data[s_idx].x;
				if (light_packed == ML_SAMPLE_INVALID) {
					continue;
				}
				atomicAdd(s_total_valid, 1u);

				bool visible = true;
				if (!force_visible) {
					vec4 vis = unpackUnorm4x8(visibility_buffer.data[s_idx]);
					visible = max(vis.r, max(vis.g, vis.b)) > 0.004;
				}
				if (visible) {
					atomicAdd(s_total_visible, 1u);
					ml_insert_visible_light(light_packed & ML_GUIDING_LIGHT_MASK);
				}
			}
		}
	}
	barrier();

	// Parallel bitonic sort over 64 slots so the top-32 most-seen visible lights land in slots 0..31.
	for (uint k_size = 2u; k_size <= ML_GUIDING_HASH_SLOTS; k_size <<= 1u) {
		for (uint j = k_size >> 1u; j > 0u; j >>= 1u) {
			uint partner = lid ^ j;
			if (partner > lid) {
				bool ascending = (lid & k_size) == 0u;
				uint ca = s_counts[lid];
				uint la = s_lights[lid];
				uint cb = s_counts[partner];
				uint lb = s_lights[partner];
				bool a_better = ml_entry_better(ca, la, cb, lb);
				bool should_swap = ascending ? !a_better : a_better;
				if (should_swap) {
					s_counts[lid] = cb;
					s_lights[lid] = lb;
					s_counts[partner] = ca;
					s_lights[partner] = la;
				}
			}
			barrier();
		}
	}

	uint tile_idx = tile.y * params.tile_dims.x + tile.x;
	uint out_base = tile_idx * ML_GUIDING_TILE_WORDS;

	if (lid < ML_GUIDING_MAX_LIGHTS) {
		uint cnt = min(s_counts[lid], ML_GUIDING_COUNT_MAX);
		uint packed_val = (cnt > 0u) ? ((s_lights[lid] & ML_GUIDING_LIGHT_MASK) | (cnt << ML_GUIDING_COUNT_SHIFT)) : ML_SAMPLE_INVALID;
		guiding_tiles_out.data[out_base + 4u + lid] = packed_val;
	}

	if (lid == 0u) {
		uint entry_count = 0u;
		for (uint i = 0u; i < ML_GUIDING_MAX_LIGHTS; i++) {
			if (s_counts[i] > 0u) {
				entry_count++;
			} else {
				break;
			}
		}
		uint max_count = entry_count > 0u ? min(s_counts[0], ML_GUIDING_COUNT_MAX) : 0u;
		guiding_tiles_out.data[out_base + 0u] = entry_count;
		guiding_tiles_out.data[out_base + 1u] = s_total_visible;
		guiding_tiles_out.data[out_base + 2u] = s_total_valid;
		guiding_tiles_out.data[out_base + 3u] = max_count;
	}
}
