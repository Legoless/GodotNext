#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1) uniform sampler2D velocity_tex;
layout(rg16f, set = 0, binding = 2) uniform restrict writeonly image2D dilated_velocity_out;

shared float s_depth[TSR_GROUP_SIZE + 2][TSR_GROUP_SIZE + 2];

void main() {
	ivec2 group_base = ivec2(gl_WorkGroupID.xy) * TSR_GROUP_SIZE - ivec2(1);
	uint local_idx = gl_LocalInvocationIndex;

	for (uint i = local_idx; i < 100u; i += 64u) {
		int ty = int(i / 10u);
		int tx = int(i - uint(ty * 10));
		ivec2 c = clamp(group_base + ivec2(tx, ty), ivec2(0), params.internal_size - ivec2(1));
		s_depth[ty][tx] = texelFetch(depth_tex, c, 0).r;
	}

	barrier();

	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, params.internal_size))) {
		return;
	}

	if ((params.flags & TSR_FLAG_RESET) != 0u) {
		imageStore(dilated_velocity_out, px, vec4(0.0));
		return;
	}

	int lx = int(gl_LocalInvocationID.x) + 1;
	int ly = int(gl_LocalInvocationID.y) + 1;

	// Reverse-Z: larger raw depth is closer to the camera. Initialize with center
	// pixel so equal-depth regions preserve exact center velocity.
	float best_depth = s_depth[ly][lx];
	ivec2 best_offset = ivec2(0);

	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			float d = s_depth[ly + dy][lx + dx];
			if (d > best_depth) {
				best_depth = d;
				best_offset = ivec2(dx, dy);
			}
		}
	}

	ivec2 vel_coord = clamp(px + best_offset, ivec2(0), params.internal_size - ivec2(1));
	vec2 vel = sanitize_vec2(texelFetch(velocity_tex, vel_coord, 0).rg);
	imageStore(dilated_velocity_out, px, vec4(vel, 0.0, 0.0));
}
