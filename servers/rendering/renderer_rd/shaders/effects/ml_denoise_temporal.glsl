#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5, GM-1, GM-7, budget M-5).
// MegaLights temporal accumulation pass at render resolution:
//   - MODE_TEMPORAL: reprojects previous history with this frame's prepass velocity (#37) or
//     prev_view_proj (#53 helpers), performs 2x2 bilinear history sampling with depth and world-normal
//     disocclusion rejection, applies #54-style firefly clamping with a GPU clamped-energy counter
//     (in physical direct-radiance units for the <= 2% PQ metric), tracks per-pixel diffuse and
//     specular luminance moments (mean, second moment -> variance) and sample counts, and honors
//     dirty-region resets (denoise_reset_regions), resize, camera cuts, and first-frame reset.
//   - MODE_COPY_PREV_GUIDE: stores world-space normal + linear depth as next frame's disocclusion guide.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../environment/nextgi_view_inc.glsl"

#define ML_DENOISE_FLAG_HAS_HISTORY (1u << 0u)
#define ML_DENOISE_FLAG_VELOCITY_ENABLED (1u << 1u)
#define ML_DENOISE_FLAG_RESET_MASK_ENABLED (1u << 2u)
#define ML_DENOISE_FLAG_ORTHOGONAL (1u << 3u)
#define ML_DENOISE_FLAG_RR_BYPASS (1u << 4u)

#define ML_DEMOD_ALBEDO_FLOOR 0.01
#define ML_DEMOD_SPECULAR_FLOOR 0.01
#define ML_LUMA_R 0.30
#define ML_LUMA_G 0.59
#define ML_LUMA_B 0.11
#define ML_CB_SCALE 1.78
#define ML_CR_SCALE 1.40
#define ML_CHROMA_LEVELS 14.0
#define ML_CHROMA_BIAS 7.0

// std140, 336 bytes. Must match RendererRD::NextGIDenoise::MegaLightsDenoiseParamsGPU.
struct MLDenoiseParams {
	mat4 inv_projection;
	mat4 inv_view_proj;
	mat4 prev_view_proj;
	vec4 view_to_world[3];
	vec4 cur_depth_coeffs;
	vec4 prev_depth_coeffs;
	vec4 camera_pos;
	vec4 prev_camera_pos;
	uvec4 screen_reset_size; // xy = screen_size, zw = reset_tiles_xy
	uvec4 config; // x = frame_index, y = flags, z = max_history_diff, w = max_history_spec
};

layout(set = 0, binding = 0, std140) uniform MLDenoiseParamsUBO {
	MLDenoiseParams params;
};

void ml_denoise_unpack_nr(vec4 p_nr, out vec3 r_view_normal, out float r_roughness, out bool r_is_dynamic) {
	r_is_dynamic = p_nr.w > 0.5;
	float r = r_is_dynamic ? (1.0 - p_nr.w) : p_nr.w;
	r_roughness = clamp(r / (127.0 / 255.0), 0.0, 1.0);
	vec3 n = p_nr.xyz * 2.0 - vec3(1.0);
	float len2 = dot(n, n);
	r_view_normal = len2 > 1e-12 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

vec3 ml_denoise_view_to_world_normal(vec3 p_view_normal) {
	mat3 basis = mat3(params.view_to_world[0].xyz, params.view_to_world[1].xyz, params.view_to_world[2].xyz);
	vec3 wn = basis * p_view_normal;
	float len2 = dot(wn, wn);
	return len2 > 1e-12 ? wn * inversesqrt(len2) : vec3(0.0, 1.0, 0.0);
}

#ifdef MODE_TEMPORAL

layout(set = 0, binding = 1) uniform sampler2D noisy_diffuse_tex;
layout(set = 0, binding = 2) uniform sampler2D noisy_specular_tex;
layout(set = 0, binding = 3) uniform sampler2D history_diffuse_prev_tex;
layout(set = 0, binding = 4) uniform sampler2D history_specular_prev_tex;
layout(set = 0, binding = 5) uniform sampler2D moments_prev_tex;
layout(r32ui, set = 0, binding = 6) uniform restrict readonly uimage2D history_len_prev_img;
layout(set = 0, binding = 7) uniform sampler2D depth_tex;
layout(set = 0, binding = 8) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 9) uniform sampler2D velocity_tex;
layout(set = 0, binding = 10) uniform sampler2D prev_depth_normal_tex;
layout(set = 0, binding = 11) uniform sampler2D ml_albedo_tex;
layout(set = 0, binding = 12) uniform sampler2D ml_material_tex;
layout(set = 0, binding = 13) uniform sampler2D dfg_lut_tex;

layout(set = 0, binding = 14, std430) restrict readonly buffer MLDenoiseResetMask {
	uint data[];
}
reset_mask;

layout(rgba16f, set = 0, binding = 15) uniform restrict writeonly image2D history_diffuse_cur_img;
layout(rgba16f, set = 0, binding = 16) uniform restrict writeonly image2D history_specular_cur_img;
layout(rgba16f, set = 0, binding = 17) uniform restrict writeonly image2D moments_cur_img;
layout(r32ui, set = 0, binding = 18) uniform restrict writeonly uimage2D history_len_cur_img;

// Firefly clamped-energy counter (#54 / GM-1 <= 2% metric):
//   [0] = clamped_energy_removed (fixed-point * 16.0)
//   [1] = total_direct_energy    (fixed-point * 16.0)
//   [2] = clamped_pixel_count
//   [3] = valid_pixel_count
layout(set = 0, binding = 19, std430) restrict buffer MLDenoiseClampStats {
	uint removed_fp;
	uint total_fp;
	uint clamped_pixels;
	uint valid_pixels;
}
clamp_stats;

shared float s_removed[64];
shared float s_total[64];
shared uint s_clamped_px[64];
shared uint s_valid_px[64];

float ml_luma(vec3 p_c) {
	return dot(p_c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ml_unpack_f0(float p_luma_unorm, float p_tint_unorm) {
	uint tint_byte = uint(clamp(round(p_tint_unorm * 255.0), 0.0, 255.0));
	float cb = (float((tint_byte >> 4u) & 0xFu) - ML_CHROMA_BIAS) / ML_CHROMA_LEVELS;
	float cr = (float(tint_byte & 0xFu) - ML_CHROMA_BIAS) / ML_CHROMA_LEVELS;
	vec3 f0;
	f0.b = p_luma_unorm + cb * ML_CB_SCALE;
	f0.r = p_luma_unorm + cr * ML_CR_SCALE;
	f0.g = (p_luma_unorm - ML_LUMA_R * f0.r - ML_LUMA_B * f0.b) / ML_LUMA_G;
	return clamp(f0, vec3(0.0), vec3(1.0));
}

void main() {
	uint lid = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;
	s_removed[lid] = 0.0;
	s_total[lid] = 0.0;
	s_clamped_px[lid] = 0u;
	s_valid_px[lid] = 0u;

	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 screen_size = ivec2(params.screen_reset_size.xy);
	bool in_bounds = (px.x < screen_size.x && px.y < screen_size.y);

	if (in_bounds) {
		float depth = texelFetch(depth_tex, px, 0).r;
		vec4 cur_spec_in = texelFetch(noisy_specular_tex, px, 0);
		if (nextgi_view_is_background(depth) || cur_spec_in.a < 0.5) {
			imageStore(history_diffuse_cur_img, px, vec4(0.0));
			imageStore(history_specular_cur_img, px, vec4(0.0));
			imageStore(moments_cur_img, px, vec4(0.0));
			imageStore(history_len_cur_img, px, uvec4(0u));
		} else {
			vec4 cur_diff_in = texelFetch(noisy_diffuse_tex, px, 0);
			vec3 cur_diff = max(cur_diff_in.rgb, vec3(0.0));
			vec3 cur_spec = max(cur_spec_in.rgb, vec3(0.0));

			vec2 uv = (vec2(px) + vec2(0.5)) / vec2(screen_size);
			float cur_linear_z = max(nextgi_view_linear_depth(params.cur_depth_coeffs, depth), 1e-4);

			vec4 nr = texelFetch(normal_roughness_tex, px, 0);
			vec3 view_normal;
			float roughness;
			bool is_dynamic;
			ml_denoise_unpack_nr(nr, view_normal, roughness, is_dynamic);
			vec3 world_normal = ml_denoise_view_to_world_normal(view_normal);

			// Physical direct-energy factors for accurate clamped-energy accounting (#54 / GM-1 <= 2%).
			vec4 ml_alb = texelFetch(ml_albedo_tex, px, 0);
			vec4 ml_mat = texelFetch(ml_material_tex, px, 0);
			vec3 diff_remod = max(ml_alb.rgb, vec3(ML_DEMOD_ALBEDO_FLOOR));
			vec3 view_pos = nextgi_view_reconstruct(params.inv_projection, uv, depth);
			bool orthogonal = (params.config.y & ML_DENOISE_FLAG_ORTHOGONAL) != 0u;
			vec3 view_dir = orthogonal ? vec3(0.0, 0.0, 1.0) : normalize(-view_pos);
			float cNdotV = max(dot(view_normal, view_dir), 1e-4);
			vec3 f0 = ml_unpack_f0(ml_mat.r, ml_mat.g);
			vec2 env_brdf = textureLod(dfg_lut_tex, vec2(cNdotV, 1.0 - roughness), 0.0).rg;
			vec3 energy_comp = vec3(1.0) + f0 * (1.0 / max(env_brdf.y, 1e-4) - 1.0);
			float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), 0.0, 1.0);
			vec3 spec_remod = max(energy_comp * ((vec3(f90) - f0) * env_brdf.x + f0 * env_brdf.y), vec3(ML_DEMOD_SPECULAR_FLOOR));

			// 1. Reproject with this frame's prepass velocity (#37) or prev_view_proj (#53).
			bool has_history = (params.config.y & ML_DENOISE_FLAG_HAS_HISTORY) != 0u;
			bool velocity_enabled = (params.config.y & ML_DENOISE_FLAG_VELOCITY_ENABLED) != 0u;
			bool reset_mask_enabled = (params.config.y & ML_DENOISE_FLAG_RESET_MASK_ENABLED) != 0u;

			bool reset_pixel = !has_history;
			if (!reset_pixel && reset_mask_enabled) {
				uvec2 r_tile = uvec2(px) >> 4u;
				if (r_tile.x < params.screen_reset_size.z && r_tile.y < params.screen_reset_size.w) {
					uint r_idx = r_tile.y * params.screen_reset_size.z + r_tile.x;
					if (reset_mask.data[r_idx] != 0u) {
						reset_pixel = true;
					}
				}
			}

			vec3 hist_diff = vec3(0.0);
			vec3 hist_spec = vec3(0.0);
			vec4 hist_moments = vec4(0.0);
			float hist_len_diff_f = 0.0;
			float hist_len_spec_f = 0.0;
			bool history_accepted = false;

			if (!reset_pixel) {
				vec3 world_pos = nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
				vec3 prev_proj = nextgi_view_project(params.prev_view_proj, world_pos);
				vec2 prev_uv = prev_proj.xy;
				bool prev_valid = prev_proj.z >= 0.0;

				if (velocity_enabled && is_dynamic) {
					vec2 motion = texelFetch(velocity_tex, px, 0).xy;
					if (all(greaterThan(motion, vec2(-0.99))) && all(lessThan(motion, vec2(0.99)))) {
						prev_uv = uv + motion;
						prev_valid = true;
					}
				}
				prev_valid = prev_valid && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));

				if (prev_valid) {
					float expected_prev_z = prev_proj.z > 0.0
							? max(nextgi_view_linear_depth(params.prev_depth_coeffs, prev_proj.z), 1e-4)
							: cur_linear_z;
					float depth_tol = is_dynamic ? 0.15 : 0.10;
					float normal_tol = 0.85;

					// 2x2 bilinear history fetch with per-tap depth & world-normal disocclusion rejection.
					vec2 prev_pos = prev_uv * vec2(screen_size) - vec2(0.5);
					ivec2 p0 = ivec2(floor(prev_pos));
					vec2 frac = clamp(prev_pos - vec2(p0), vec2(0.0), vec2(1.0));
					float b_weights[4] = float[4](
							(1.0 - frac.x) * (1.0 - frac.y),
							frac.x * (1.0 - frac.y),
							(1.0 - frac.x) * frac.y,
							frac.x * frac.y);
					ivec2 b_offs[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));

					float w_sum = 0.0;
					for (int i = 0; i < 4; i++) {
						ivec2 q = p0 + b_offs[i];
						if (q.x < 0 || q.y < 0 || q.x >= screen_size.x || q.y >= screen_size.y) {
							continue;
						}
						vec4 pg = texelFetch(prev_depth_normal_tex, q, 0);
						float pz = pg.w;
						if (!(pz > 0.0)) {
							continue;
						}
						float rel_z = abs(pz - expected_prev_z) / max(expected_prev_z, 0.05);
						float ndot = dot(world_normal, pg.xyz);
						if (rel_z > depth_tol || ndot < normal_tol) {
							continue;
						}
						uint plen_packed = imageLoad(history_len_prev_img, q).r;
						uint plen_diff = plen_packed & 0xFFu;
						uint plen_spec = (plen_packed >> 8u) & 0xFFu;
						if (plen_diff == 0u) {
							continue;
						}
						float w = b_weights[i];
						hist_diff += texelFetch(history_diffuse_prev_tex, q, 0).rgb * w;
						hist_spec += texelFetch(history_specular_prev_tex, q, 0).rgb * w;
						hist_moments += texelFetch(moments_prev_tex, q, 0) * w;
						hist_len_diff_f += float(plen_diff) * w;
						hist_len_spec_f += float(plen_spec) * w;
						w_sum += w;
					}

					if (w_sum > 1e-4) {
						float inv_w = 1.0 / w_sum;
						hist_diff *= inv_w;
						hist_spec *= inv_w;
						hist_moments *= inv_w;
						hist_len_diff_f *= inv_w;
						hist_len_spec_f *= inv_w;
						history_accepted = true;
					}
				}
			}

			// 2. 3x3 same-surface neighborhood statistics (excluding center for Samuelson-free firefly detection,
			//    and including center for anti-ghosting & disocclusion spatial variance).
			vec3 diff_nbr_sum = vec3(0.0);
			vec3 diff_nbr_sq = vec3(0.0);
			float diff_l_nbr_sum = 0.0;
			float diff_l_nbr_sq = 0.0;
			vec3 spec_nbr_sum = vec3(0.0);
			vec3 spec_nbr_sq = vec3(0.0);
			float spec_l_nbr_sum = 0.0;
			float spec_l_nbr_sq = 0.0;
			uint valid_nbrs = 0u;

			for (int oy = -1; oy <= 1; oy++) {
				for (int ox = -1; ox <= 1; ox++) {
					if (ox == 0 && oy == 0) {
						continue;
					}
					ivec2 q = px + ivec2(ox, oy);
					if (q.x < 0 || q.y < 0 || q.x >= screen_size.x || q.y >= screen_size.y) {
						continue;
					}
					float qd = texelFetch(depth_tex, q, 0).r;
					if (nextgi_view_is_background(qd)) {
						continue;
					}
					vec4 q_sp = texelFetch(noisy_specular_tex, q, 0);
					if (q_sp.a < 0.5) {
						continue;
					}
					float qz = max(nextgi_view_linear_depth(params.cur_depth_coeffs, qd), 1e-4);
					if (abs(qz - cur_linear_z) > 0.1 * max(cur_linear_z, 0.05)) {
						continue;
					}
					vec3 qn;
					float qr;
					bool qdyn;
					ml_denoise_unpack_nr(texelFetch(normal_roughness_tex, q, 0), qn, qr, qdyn);
					if (dot(view_normal, qn) < 0.85) {
						continue;
					}
					vec3 q_df = max(texelFetch(noisy_diffuse_tex, q, 0).rgb, vec3(0.0));
					vec3 q_sv = max(q_sp.rgb, vec3(0.0));
					float dl = ml_luma(q_df);
					float sl = ml_luma(q_sv);
					diff_nbr_sum += q_df;
					diff_nbr_sq += q_df * q_df;
					diff_l_nbr_sum += dl;
					diff_l_nbr_sq += dl * dl;
					spec_nbr_sum += q_sv;
					spec_nbr_sq += q_sv * q_sv;
					spec_l_nbr_sum += sl;
					spec_l_nbr_sq += sl * sl;
					valid_nbrs++;
				}
			}

			// 3. Firefly clamp (#54 pattern) + GPU clamped-energy counter in physical direct-radiance units.
			float diff_luma_raw = ml_luma(cur_diff);
			float spec_luma_raw = ml_luma(cur_spec);
			float diff_phys_luma = ml_luma(cur_diff * diff_remod);
			float spec_phys_luma = ml_luma(cur_spec * spec_remod);
			float total_phys = diff_phys_luma + spec_phys_luma;
			float removed_phys = 0.0;
			bool was_clamped = false;

			if (valid_nbrs >= 3u) {
				float inv_n = 1.0 / float(valid_nbrs);
				float d_mean = diff_l_nbr_sum * inv_n;
				float d_sigma = sqrt(max(diff_l_nbr_sq * inv_n - d_mean * d_mean, 0.0));
				float diff_limit = max(d_mean + 3.0 * d_sigma, d_mean * 2.5 + 0.15);
				if (history_accepted && hist_len_diff_f >= 4.0) {
					float h_sig = sqrt(max(hist_moments.y - hist_moments.x * hist_moments.x, 0.0));
					diff_limit = max(diff_limit, hist_moments.x + 3.0 * h_sig);
				}
				if (diff_luma_raw > diff_limit && diff_luma_raw > 0.0) {
					float scale = diff_limit / diff_luma_raw;
					cur_diff *= scale;
					removed_phys += diff_phys_luma * (1.0 - scale);
					was_clamped = true;
				}

				float s_mean = spec_l_nbr_sum * inv_n;
				float s_sigma = sqrt(max(spec_l_nbr_sq * inv_n - s_mean * s_mean, 0.0));
				float spec_k = mix(5.0, 3.0, roughness);
				float spec_limit = max(s_mean + spec_k * s_sigma, s_mean * mix(4.0, 2.5, roughness) + 0.25);
				if (history_accepted && hist_len_spec_f >= 4.0) {
					float hs_sig = sqrt(max(hist_moments.w - hist_moments.z * hist_moments.z, 0.0));
					spec_limit = max(spec_limit, hist_moments.z + spec_k * hs_sig);
				}
				if (spec_luma_raw > spec_limit && spec_luma_raw > 0.0) {
					float scale = spec_limit / spec_luma_raw;
					cur_spec *= scale;
					removed_phys += spec_phys_luma * (1.0 - scale);
					was_clamped = true;
				}
			}

			s_removed[lid] = removed_phys;
			s_total[lid] = total_phys;
			s_clamped_px[lid] = was_clamped ? 1u : 0u;
			s_valid_px[lid] = 1u;

			// 4. Temporal accumulation + luminance moments (mean, second moment -> variance).
			float l_diff = ml_luma(cur_diff);
			float l_spec = ml_luma(cur_spec);

			float inv_all = 1.0 / float(valid_nbrs + 1u);
			vec3 diff_rgb_mean = (diff_nbr_sum + cur_diff) * inv_all;
			vec3 diff_rgb_sigma = sqrt(max((diff_nbr_sq + cur_diff * cur_diff) * inv_all - diff_rgb_mean * diff_rgb_mean, vec3(0.0)));
			float diff_l_mean = (diff_l_nbr_sum + l_diff) * inv_all;
			float spatial_var_diff = max((diff_l_nbr_sq + l_diff * l_diff) * inv_all - diff_l_mean * diff_l_mean, 0.0);

			vec3 spec_rgb_mean = (spec_nbr_sum + cur_spec) * inv_all;
			vec3 spec_rgb_sigma = sqrt(max((spec_nbr_sq + cur_spec * cur_spec) * inv_all - spec_rgb_mean * spec_rgb_mean, vec3(0.0)));
			float spec_l_mean = (spec_l_nbr_sum + l_spec) * inv_all;
			float spatial_var_spec = max((spec_l_nbr_sq + l_spec * l_spec) * inv_all - spec_l_mean * spec_l_mean, 0.0);

			uint max_diff_cap = max(params.config.z, 1u);
			uint max_spec_base = max(params.config.w, 1u);
			uint spec_cap = max(uint(round(mix(6.0, float(max_spec_base), clamp(roughness / 0.4, 0.0, 1.0)))), 1u);

			uint out_len_diff = 1u;
			uint out_len_spec = 1u;
			vec3 out_diff = cur_diff;
			vec3 out_spec = cur_spec;
			float m1_diff = l_diff;
			float m2_diff = l_diff * l_diff;
			float m1_spec = l_spec;
			float m2_spec = l_spec * l_spec;
			float var_diff = spatial_var_diff;
			float var_spec = spatial_var_spec;

			if (history_accepted) {
				out_len_diff = min(uint(round(hist_len_diff_f)) + 1u, max_diff_cap);
				out_len_spec = min(uint(round(hist_len_spec_f)) + 1u, spec_cap);

				float alpha_diff = max(1.0 / float(out_len_diff), 1.0 / float(max_diff_cap));
				float alpha_spec = max(1.0 / float(out_len_spec), 1.0 / float(spec_cap));
				float alpha_m_diff = max(alpha_diff, 0.2);
				float alpha_m_spec = max(alpha_spec, 0.2);

				// Generous anti-ghosting neighborhood box so static Monte Carlo converges cleanly
				// while moving shadows / light toggles respond promptly.
				vec3 clamped_hist_diff = clamp(hist_diff, max(diff_rgb_mean - 3.5 * diff_rgb_sigma - vec3(0.02), vec3(0.0)), diff_rgb_mean + 3.5 * diff_rgb_sigma + vec3(0.02));
				vec3 clamped_hist_spec = clamp(hist_spec, max(spec_rgb_mean - 3.5 * spec_rgb_sigma - vec3(0.02), vec3(0.0)), spec_rgb_mean + 3.5 * spec_rgb_sigma + vec3(0.02));

				out_diff = mix(clamped_hist_diff, cur_diff, alpha_diff);
				out_spec = mix(clamped_hist_spec, cur_spec, alpha_spec);

				m1_diff = mix(hist_moments.x, l_diff, alpha_m_diff);
				m2_diff = mix(hist_moments.y, l_diff * l_diff, alpha_m_diff);
				m1_spec = mix(hist_moments.z, l_spec, alpha_m_spec);
				m2_spec = mix(hist_moments.w, l_spec * l_spec, alpha_m_spec);

				var_diff = max(m2_diff - m1_diff * m1_diff, 0.0);
				var_spec = max(m2_spec - m1_spec * m1_spec, 0.0);

				if (out_len_diff < 4u) {
					var_diff = max(var_diff, spatial_var_diff * (1.0 - float(out_len_diff) * 0.25));
				}
				if (out_len_spec < 4u) {
					var_spec = max(var_spec, spatial_var_spec * (1.0 - float(out_len_spec) * 0.25));
				}
			}

			out_diff = min(max(out_diff, vec3(0.0)), vec3(65000.0));
			out_spec = min(max(out_spec, vec3(0.0)), vec3(65000.0));
			var_diff = clamp(var_diff, 0.0, 65000.0);
			var_spec = clamp(var_spec, 0.0, 65000.0);

			imageStore(history_diffuse_cur_img, px, vec4(out_diff, var_diff));
			imageStore(history_specular_cur_img, px, vec4(out_spec, var_spec));
			imageStore(moments_cur_img, px, vec4(clamp(m1_diff, 0.0, 65000.0), clamp(m2_diff, 0.0, 65000.0), clamp(m1_spec, 0.0, 65000.0), clamp(m2_spec, 0.0, 65000.0)));
			uint packed_len = min(out_len_diff, 255u) | (min(out_len_spec, 255u) << 8u);
			imageStore(history_len_cur_img, px, uvec4(packed_len, 0u, 0u, 0u));
		}
	}

	// Workgroup reduction for firefly clamped-energy counter (1 atomicAdd per 8x8 tile).
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (lid < stride) {
			s_removed[lid] += s_removed[lid + stride];
			s_total[lid] += s_total[lid + stride];
			s_clamped_px[lid] += s_clamped_px[lid + stride];
			s_valid_px[lid] += s_valid_px[lid + stride];
		}
		barrier();
	}
	if (lid == 0u && s_valid_px[0] > 0u) {
		uint rem_u = uint(clamp(round(s_removed[0] * 16.0), 0.0, 16777215.0));
		uint tot_u = uint(clamp(round(s_total[0] * 16.0), 0.0, 16777215.0));
		if (rem_u > 0u) {
			atomicAdd(clamp_stats.removed_fp, rem_u);
		}
		if (tot_u > 0u) {
			atomicAdd(clamp_stats.total_fp, tot_u);
		}
		if (s_clamped_px[0] > 0u) {
			atomicAdd(clamp_stats.clamped_pixels, s_clamped_px[0]);
		}
		atomicAdd(clamp_stats.valid_pixels, s_valid_px[0]);
	}
}

#elif defined(MODE_COPY_PREV_GUIDE)

layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D prev_depth_normal_img;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 screen_size = ivec2(params.screen_reset_size.xy);
	if (px.x >= screen_size.x || px.y >= screen_size.y) {
		return;
	}
	float depth = texelFetch(depth_tex, px, 0).r;
	if (nextgi_view_is_background(depth)) {
		imageStore(prev_depth_normal_img, px, vec4(0.0));
		return;
	}
	vec3 view_n;
	float roughness;
	bool is_dynamic;
	ml_denoise_unpack_nr(texelFetch(normal_roughness_tex, px, 0), view_n, roughness, is_dynamic);
	vec3 world_n = ml_denoise_view_to_world_normal(view_n);
	float linear_z = max(nextgi_view_linear_depth(params.cur_depth_coeffs, depth), 1e-4);
	imageStore(prev_depth_normal_img, px, vec4(world_n, linear_z));
}

#endif
