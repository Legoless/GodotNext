#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D history_color_curr_tex;
layout(set = 0, binding = 1) uniform sampler2D history_meta_curr_tex;
#ifdef MODE_HISTORY_RGBA16F
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D aa_color_out;
#else
layout(r11f_g11f_b10f, set = 0, binding = 2) uniform restrict writeonly image2D aa_color_out;
#endif

shared float s_luma[TSR_GROUP_SIZE + 2][TSR_GROUP_SIZE + 2];

void main() {
	ivec2 group_base = ivec2(gl_WorkGroupID.xy) * TSR_GROUP_SIZE - ivec2(1);
	uint local_idx = gl_LocalInvocationIndex;

	for (uint i = local_idx; i < 100u; i += 64u) {
		int ty = int(i / 10u);
		int tx = int(i - uint(ty * 10));
		ivec2 c = clamp(group_base + ivec2(tx, ty), ivec2(0), params.history_size - ivec2(1));
		vec3 rgb = sanitize_hdr(texelFetch(history_color_curr_tex, c, 0).rgb);
		float l = hdr_luma(rgb);
		s_luma[ty][tx] = l / (1.0 + l);
	}

	barrier();

	ivec2 hpx = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(hpx, params.history_size))) {
		return;
	}

	vec3 c0 = sanitize_hdr(texelFetch(history_color_curr_tex, hpx, 0).rgb);
	float conf = texelFetch(history_meta_curr_tex, hpx, 0).r;

	// Converged temporal pixels already have multi-frame anti-aliasing; preserve them untouched.
	if (conf >= 0.65) {
		imageStore(aa_color_out, hpx, vec4(c0, 1.0));
		return;
	}

	int lx = int(gl_LocalInvocationID.x) + 1;
	int ly = int(gl_LocalInvocationID.y) + 1;

	float l_tl = s_luma[ly - 1][lx - 1];
	float l_tc = s_luma[ly - 1][lx];
	float l_tr = s_luma[ly - 1][lx + 1];
	float l_ml = s_luma[ly][lx - 1];
	float l_mr = s_luma[ly][lx + 1];
	float l_bl = s_luma[ly + 1][lx - 1];
	float l_bc = s_luma[ly + 1][lx];
	float l_br = s_luma[ly + 1][lx + 1];

	float gx = (l_tr + 2.0 * l_mr + l_br) - (l_tl + 2.0 * l_ml + l_bl);
	float gy = (l_bl + 2.0 * l_bc + l_br) - (l_tl + 2.0 * l_tc + l_tr);
	float grad_len = length(vec2(gx, gy));

	if (grad_len < 1e-3) {
		imageStore(aa_color_out, hpx, vec4(c0, 1.0));
		return;
	}

	vec2 tangent = vec2(-gy, gx) / grad_len;
	vec2 inv_hist = 1.0 / vec2(params.history_size);
	vec2 uv0 = (vec2(hpx) + 0.5) * inv_hist;
	vec2 step_uv = tangent * inv_hist;

	vec3 s_n1 = sanitize_hdr(textureLod(history_color_curr_tex, clamp(uv0 - 0.75 * step_uv, vec2(0.0), vec2(1.0)), 0.0).rgb);
	vec3 s_p1 = sanitize_hdr(textureLod(history_color_curr_tex, clamp(uv0 + 0.75 * step_uv, vec2(0.0), vec2(1.0)), 0.0).rgb);
	vec3 s_n2 = sanitize_hdr(textureLod(history_color_curr_tex, clamp(uv0 - 1.50 * step_uv, vec2(0.0), vec2(1.0)), 0.0).rgb);
	vec3 s_p2 = sanitize_hdr(textureLod(history_color_curr_tex, clamp(uv0 + 1.50 * step_uv, vec2(0.0), vec2(1.0)), 0.0).rgb);

	float w0 = 0.40 / (1.0 + hdr_luma(c0));
	float wn1 = 0.20 / (1.0 + hdr_luma(s_n1));
	float wp1 = 0.20 / (1.0 + hdr_luma(s_p1));
	float wn2 = 0.10 / (1.0 + hdr_luma(s_n2));
	float wp2 = 0.10 / (1.0 + hdr_luma(s_p2));

	vec3 edge_filtered = (c0 * w0 + s_n1 * wn1 + s_p1 * wp1 + s_n2 * wn2 + s_p2 * wp2) / max(w0 + wn1 + wp1 + wn2 + wp2, 1e-5);
	float strength = clamp(1.0 - conf / 0.65, 0.0, 1.0) * clamp(grad_len * 4.0, 0.0, 1.0);
	vec3 out_rgb = sanitize_hdr(mix(c0, edge_filtered, strength));

	imageStore(aa_color_out, hpx, vec4(out_rgb, 1.0));
}
