#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1) uniform sampler2D dilated_velocity_tex;
layout(set = 0, binding = 2) uniform sampler2D prev_depth_tex;
layout(r8, set = 0, binding = 3) uniform restrict writeonly image2D disocclusion_out;

shared float s_lin_depth[TSR_GROUP_SIZE + 2][TSR_GROUP_SIZE + 2];
shared float s_raw_depth[TSR_GROUP_SIZE + 2][TSR_GROUP_SIZE + 2];

void main() {
	ivec2 group_base = ivec2(gl_WorkGroupID.xy) * TSR_GROUP_SIZE - ivec2(1);
	uint local_idx = gl_LocalInvocationIndex;

	for (uint i = local_idx; i < 100u; i += 64u) {
		int ty = int(i / 10u);
		int tx = int(i - uint(ty * 10));
		ivec2 c = clamp(group_base + ivec2(tx, ty), ivec2(0), params.internal_size - ivec2(1));
		float rd = texelFetch(depth_tex, c, 0).r;
		s_raw_depth[ty][tx] = rd;
		s_lin_depth[ty][tx] = linearize_depth(rd);
	}

	barrier();

	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, params.internal_size))) {
		return;
	}

	if ((params.flags & TSR_FLAG_RESET) != 0u) {
		imageStore(disocclusion_out, px, vec4(1.0, 0.0, 0.0, 0.0));
		return;
	}

	vec2 uv = (vec2(px) + 0.5) / vec2(params.internal_size);
	vec2 vel = sanitize_vec2(texelFetch(dilated_velocity_tex, px, 0).rg);
	vec2 uv_prev = uv + vel;

	if (any(lessThan(uv_prev, vec2(0.0))) || any(greaterThan(uv_prev, vec2(1.0)))) {
		imageStore(disocclusion_out, px, vec4(1.0, 0.0, 0.0, 0.0));
		return;
	}

	int lx = int(gl_LocalInvocationID.x) + 1;
	int ly = int(gl_LocalInvocationID.y) + 1;
	float raw_c = s_raw_depth[ly][lx];
	float z_c = s_lin_depth[ly][lx];

	// Sample the 2x2 footprint in previous-frame linear depth around uv_prev.
	ivec2 prev_size = textureSize(prev_depth_tex, 0);
	vec2 prev_pos = uv_prev * vec2(prev_size) - 0.5;
	ivec2 base_p = ivec2(floor(prev_pos));
	ivec2 max_c = prev_size - ivec2(1);

	float zp00 = texelFetch(prev_depth_tex, clamp(base_p + ivec2(0, 0), ivec2(0), max_c), 0).r;
	float zp10 = texelFetch(prev_depth_tex, clamp(base_p + ivec2(1, 0), ivec2(0), max_c), 0).r;
	float zp01 = texelFetch(prev_depth_tex, clamp(base_p + ivec2(0, 1), ivec2(0), max_c), 0).r;
	float zp11 = texelFetch(prev_depth_tex, clamp(base_p + ivec2(1, 1), ivec2(0), max_c), 0).r;

	// Freshly reallocated prev_depth after a dynamic-resolution step is cleared to 0.0
	// without resetting history; defer rejection to Pass 3 YCoCg variance clamping for this frame.
	if (zp00 <= 0.0 && zp10 <= 0.0 && zp01 <= 0.0 && zp11 <= 0.0) {
		imageStore(disocclusion_out, px, vec4(0.0, 0.0, 0.0, 0.0));
		return;
	}

	bool curr_sky = (raw_c <= 1e-7);
	if (curr_sky) {
		bool any_prev_sky = (zp00 >= 64500.0) || (zp10 >= 64500.0) || (zp01 >= 64500.0) || (zp11 >= 64500.0);
		imageStore(disocclusion_out, px, vec4(any_prev_sky ? 0.0 : 1.0, 0.0, 0.0, 0.0));
		return;
	}

	bool all_prev_sky = (zp00 >= 64500.0) && (zp10 >= 64500.0) && (zp01 >= 64500.0) && (zp11 >= 64500.0);
	if (all_prev_sky) {
		imageStore(disocclusion_out, px, vec4(1.0, 0.0, 0.0, 0.0));
		return;
	}

	// Expected previous depth from camera reprojection (handles camera dolly/rotation),
	// falling back to current depth for object Z translation.
	float z_expected = z_c;
	vec4 prev_clip = params.reprojection * vec4(uv * 2.0 - 1.0, raw_c * 2.0 - 1.0, 1.0);
	if (prev_clip.w > 1e-5) {
		float prev_raw = clamp(prev_clip.z / prev_clip.w * 0.5 + 0.5, 0.0, 1.0);
		if (prev_raw > 1e-7) {
			z_expected = linearize_depth(prev_raw);
		}
	}

	// One-sided depth slope prevents silhouette edges from inflating tolerance onto background pixels.
	float slope_x = min(abs(s_lin_depth[ly][lx + 1] - z_c), abs(z_c - s_lin_depth[ly][lx - 1]));
	float slope_y = min(abs(s_lin_depth[ly + 1][lx] - z_c), abs(z_c - s_lin_depth[ly - 1][lx]));
	float slope = min(slope_x + slope_y, 0.25 * z_c);

	float ref_z = min(z_c, z_expected);
	float tol = max(0.02 * ref_z, 2.0 * slope + 2e-3);

	float d00 = min(abs(z_expected - zp00), abs(z_c - zp00));
	float d10 = min(abs(z_expected - zp10), abs(z_c - zp10));
	float d01 = min(abs(z_expected - zp01), abs(z_c - zp01));
	float d11 = min(abs(z_expected - zp11), abs(z_c - zp11));
	float min_diff = min(min(d00, d10), min(d01, d11));

	float disocc = clamp((min_diff - tol) / max(tol * 2.0, 1e-3), 0.0, 1.0);
	imageStore(disocclusion_out, px, vec4(disocc, 0.0, 0.0, 0.0));
}
