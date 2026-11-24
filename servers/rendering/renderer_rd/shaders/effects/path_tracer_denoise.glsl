#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: path_tracer_denoise (#168). Screen-space Joint Non-Local Means (JNLM) denoiser for
// RendererRD::PathTracer, adapted from LightmapperRD::_denoise (modules/lightmapper_rd/lm_compute.glsl).
//
// Based on YoctoImageDenoiser's JNLM implementation with corrections from "Nonlinearly Weighted
// First-order Regression for Denoising Monte Carlo Renderings":
// <https://github.com/ManuelPrandini/YoctoImageDenoiser/blob/06e19489dd64e47792acffde536393802ba48607/libs/yocto_extension/yocto_extension.cpp#L207>
// <https://benedikt-bitterli.me/nfor/nfor.pdf>
// MIT License (Copyright (c) 2020 ManuelPrandini).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, set = 0, binding = 0) restrict readonly uniform image2D src_accumulation;
layout(rgba32f, set = 0, binding = 1) restrict readonly uniform image2D src_guide_albedo;
layout(rgba32f, set = 0, binding = 2) restrict readonly uniform image2D src_guide_normal;
layout(rgba32f, set = 0, binding = 3) restrict writeonly uniform image2D dst_denoised;

layout(push_constant, std430) uniform DenoiseParams {
	uint width;
	uint height;
	float exposure;
	float spatial_bandwidth;
	float light_bandwidth;
	float albedo_bandwidth;
	float normal_bandwidth;
	float filter_strength;
	int half_search_window;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

const int HALF_PATCH_WINDOW = 2;
const int MAX_HALF_SEARCH = 5;
const int TILE_HALO = MAX_HALF_SEARCH + HALF_PATCH_WINDOW;
const int TILE_DIM = 8 + TILE_HALO * 2;
const int PATCH_WINDOW_DIM = HALF_PATCH_WINDOW * 2 + 1;
const int PATCH_WINDOW_DIM_SQ = PATCH_WINDOW_DIM * PATCH_WINDOW_DIM;
const float EPSILON = 1e-6;

// Tone-mapped exposed radiance cached in shared memory for the 5x5 patch comparisons.
shared vec3 tile_patch[TILE_DIM][TILE_DIM];

vec3 sanitize_radiance(vec3 p_v) {
	if (any(lessThan(p_v, vec3(-1e30))) || any(greaterThan(p_v, vec3(1e30)))) {
		return vec3(0.0);
	}
	return max(p_v, vec3(0.0));
}

vec3 resolve_albedo(vec4 p_raw) {
	if (p_raw.a <= EPSILON) {
		return vec3(0.0);
	}
	vec3 a = p_raw.rgb / p_raw.a;
	if (any(lessThan(a, vec3(-1e30))) || any(greaterThan(a, vec3(1e30)))) {
		return vec3(0.0);
	}
	return clamp(a, vec3(0.0), vec3(1.0));
}

vec3 resolve_normal(vec4 p_raw) {
	if (p_raw.a <= EPSILON) {
		return vec3(0.0);
	}
	if (any(lessThan(p_raw.rgb, vec3(-1e30))) || any(greaterThan(p_raw.rgb, vec3(1e30)))) {
		return vec3(0.0);
	}
	float len = length(p_raw.rgb);
	return len > EPSILON ? (p_raw.rgb / len) : vec3(0.0);
}

void main() {
	ivec2 max_coord = ivec2(max(int(params.width) - 1, 0), max(int(params.height) - 1, 0));
	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * 8 - ivec2(TILE_HALO);
	float safe_exposure = max(params.exposure, 1e-6);

	// All threads cooperatively fill the shared patch tile before the barrier.
	uint tid = gl_LocalInvocationIndex;
	for (uint idx = tid; idx < uint(TILE_DIM * TILE_DIM); idx += 64u) {
		int ly = int(idx / uint(TILE_DIM));
		int lx = int(idx % uint(TILE_DIM));
		ivec2 p = clamp(tile_origin + ivec2(lx, ly), ivec2(0), max_coord);
		vec4 acc = imageLoad(src_accumulation, p);
		vec3 rgb = acc.a > 0.0 ? sanitize_radiance(acc.rgb / acc.a) : vec3(0.0);
		vec3 exposed = rgb * safe_exposure;
		tile_patch[ly][lx] = exposed / (vec3(1.0) + exposed);
	}
	barrier();

	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= params.width || pixel.y >= params.height) {
		return;
	}

	ivec2 pos = ivec2(pixel);
	vec4 input_acc = imageLoad(src_accumulation, pos);
	vec3 input_rgb = input_acc.a > 0.0 ? sanitize_radiance(input_acc.rgb / input_acc.a) : vec3(0.0);
	vec3 input_albedo = resolve_albedo(imageLoad(src_guide_albedo, pos));
	vec3 input_normal = resolve_normal(imageLoad(src_guide_normal, pos));

	vec3 denoised_rgb = input_rgb;
	if (length(input_normal) > EPSILON) {
		int half_search = clamp(params.half_search_window, 1, MAX_HALF_SEARCH);
		float sigma_spatial = max(params.spatial_bandwidth, 1e-3);
		float sigma_light = max(params.light_bandwidth, 1e-4);
		float sigma_albedo = max(params.albedo_bandwidth, 1e-3);
		float sigma_normal = max(params.normal_bandwidth, 1e-3);
		float filter_value = max(params.filter_strength, 1e-3) * sigma_light;

		float two_sigma_spatial_sq = 2.0 * sigma_spatial * sigma_spatial;
		float two_sigma_light_sq = 2.0 * sigma_light * sigma_light;
		float two_sigma_albedo_sq = 2.0 * sigma_albedo * sigma_albedo;
		float two_sigma_normal_sq = 2.0 * sigma_normal * sigma_normal;
		float filter_sq_two_sigma_light_sq = max(filter_value * filter_value * two_sigma_light_sq, 1e-8);

		ivec2 center_local = ivec2(gl_LocalInvocationID.xy) + ivec2(TILE_HALO);
		vec3 accum_rgb = vec3(0.0);
		float sum_weights = 0.0;

		for (int sy = -half_search; sy <= half_search; sy++) {
			int ny = pos.y + sy;
			if (ny < 0 || ny >= int(params.height)) {
				continue;
			}
			for (int sx = -half_search; sx <= half_search; sx++) {
				int nx = pos.x + sx;
				if (nx < 0 || nx >= int(params.width)) {
					continue;
				}
				ivec2 search_pos = ivec2(nx, ny);
				vec3 search_normal = resolve_normal(imageLoad(src_guide_normal, search_pos));
				if (length(search_normal) <= EPSILON) {
					continue;
				}
				vec3 search_albedo = resolve_albedo(imageLoad(src_guide_albedo, search_pos));
				float pixel_sq_dist = float(sx * sx + sy * sy);
				vec3 albedo_delta = input_albedo - search_albedo;
				float albedo_sq_dist = dot(albedo_delta, albedo_delta);
				vec3 normal_delta = input_normal - search_normal;
				float normal_sq_dist = dot(normal_delta, normal_delta);

				float guide_weight = exp(
						-pixel_sq_dist / two_sigma_spatial_sq -
						albedo_sq_dist / two_sigma_albedo_sq -
						normal_sq_dist / two_sigma_normal_sq);
				if (guide_weight < 1e-5) {
					continue;
				}

				ivec2 search_local = center_local + ivec2(sx, sy);
				float patch_sq_dist = 0.0;
				for (int oy = -HALF_PATCH_WINDOW; oy <= HALF_PATCH_WINDOW; oy++) {
					for (int ox = -HALF_PATCH_WINDOW; ox <= HALF_PATCH_WINDOW; ox++) {
						vec3 delta = tile_patch[center_local.y + oy][center_local.x + ox] -
								tile_patch[search_local.y + oy][search_local.x + ox];
						patch_sq_dist += dot(delta, delta) - two_sigma_light_sq;
					}
				}
				patch_sq_dist = max(0.0, patch_sq_dist / (3.0 * float(PATCH_WINDOW_DIM_SQ)));

				float weight = guide_weight * exp(-patch_sq_dist / filter_sq_two_sigma_light_sq);
				vec4 search_acc = imageLoad(src_accumulation, search_pos);
				vec3 search_rgb = search_acc.a > 0.0 ? sanitize_radiance(search_acc.rgb / search_acc.a) : vec3(0.0);
				accum_rgb += weight * search_rgb;
				sum_weights += weight;
			}
		}

		denoised_rgb = sum_weights > EPSILON ? sanitize_radiance(accum_rgb / sum_weights) : input_rgb;
	}

	imageStore(dst_denoised, pos, vec4(denoised_rgb, 1.0));
}
