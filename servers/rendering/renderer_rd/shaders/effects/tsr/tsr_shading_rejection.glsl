#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D scene_color_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D dilated_velocity_tex;
layout(set = 0, binding = 3) uniform sampler2D history_color_prev_tex;
layout(set = 0, binding = 4) uniform sampler2D reactive_tex;
layout(set = 0, binding = 5) uniform sampler2D disocclusion_tex;
layout(r8, set = 0, binding = 6) uniform restrict writeonly image2D rejection_out;
layout(r16f, set = 0, binding = 7) uniform restrict writeonly image2D prev_depth_out;

shared vec3 s_ycocg[TSR_GROUP_SIZE + 2][TSR_GROUP_SIZE + 2];

void main() {
	ivec2 group_base = ivec2(gl_WorkGroupID.xy) * TSR_GROUP_SIZE - ivec2(1);
	uint local_idx = gl_LocalInvocationIndex;

	for (uint i = local_idx; i < 100u; i += 64u) {
		int ty = int(i / 10u);
		int tx = int(i - uint(ty * 10));
		ivec2 c = clamp(group_base + ivec2(tx, ty), ivec2(0), params.internal_size - ivec2(1));
		vec3 rgb = sanitize_hdr(texelFetch(scene_color_tex, c, 0).rgb);
		s_ycocg[ty][tx] = rgb_to_perceptual_ycocg(rgb);
	}

	barrier();

	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, params.internal_size))) {
		return;
	}

	float raw_d = texelFetch(depth_tex, px, 0).r;
	imageStore(prev_depth_out, px, vec4(linearize_depth(raw_d), 0.0, 0.0, 0.0));

	if ((params.flags & TSR_FLAG_RESET) != 0u) {
		imageStore(rejection_out, px, vec4(1.0, 0.0, 0.0, 0.0));
		return;
	}

	vec2 uv = (vec2(px) + 0.5) / vec2(params.internal_size);
	vec2 vel = sanitize_vec2(texelFetch(dilated_velocity_tex, px, 0).rg);
	vec2 uv_prev = uv + vel;

	float disocc = texelFetch(disocclusion_tex, px, 0).r;
	if (any(lessThan(uv_prev, vec2(0.0))) || any(greaterThan(uv_prev, vec2(1.0)))) {
		disocc = 1.0;
	}

	int lx = int(gl_LocalInvocationID.x) + 1;
	int ly = int(gl_LocalInvocationID.y) + 1;

	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	vec3 bmin = vec3(1e9);
	vec3 bmax = vec3(-1e9);

	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			vec3 s = s_ycocg[ly + dy][lx + dx];
			m1 += s;
			m2 += s * s;
			bmin = min(bmin, s);
			bmax = max(bmax, s);
		}
	}

	vec3 mu = m1 * (1.0 / 9.0);
	vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(1e-6)));
	vec3 box_lo = min(bmin, mu - 1.25 * sigma);
	vec3 box_hi = max(bmax, mu + 1.25 * sigma);

	vec3 hist_rgb = sanitize_hdr(textureLod(history_color_prev_tex, clamp(uv_prev, vec2(0.0), vec2(1.0)), 0.0).rgb);
	vec3 hist_ycocg = rgb_to_perceptual_ycocg(hist_rgb);

	vec3 excess = max(max(box_lo - hist_ycocg, hist_ycocg - box_hi), vec3(0.0));
	vec3 scale = max(sigma * 2.25, vec3(0.02, 0.015, 0.015));
	vec3 norm_excess = excess / scale;
	float shade_rej = clamp(max(norm_excess.x, 0.85 * length(norm_excess.yz)), 0.0, 1.0);

	// Reactive mask from RB_TEX_COLOR.a (0 on opaque/sky, >0 on translucent surfaces).
	float reactive = clamp(texelFetch(reactive_tex, px, 0).r, 0.0, 1.0);

	float rejection = clamp(max(disocc, max(reactive, shade_rej)), 0.0, 1.0);
	imageStore(rejection_out, px, vec4(rejection, 0.0, 0.0, 0.0));
}
