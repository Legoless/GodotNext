#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_shadow_policy (#145). The resolve pass of the SRC relight's material retrace: after the
// relight appended the rays that met only casters whose alpha comes from shader code (nextgi_shadow_ray_inc.glsl)
// and the RT-pipeline retrace traced them through the material any-hit stages (nextgi_hit_base.glsl), each record's
// answer sets or clears its light's bit in the shadow mask. The relight appends at most one record per mask texel
// per frame, so no two invocations write one texel.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(r8ui, set = 0, binding = 0) uniform uimage2D mask_image;

// next_gi_shadow_policy.h, NextGIShadowRetraceRecord. info = (mask texel x | y << 16, light word, light slot,
// result: 0 pending, 1 occluded, 2 clear).
layout(set = 0, binding = 1, std430) restrict readonly buffer NextGIShadowRetrace {
	uvec4 header;
	uvec4 data[];
}
records;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= min(records.header.x, records.header.y)) {
		return;
	}
	uvec4 info = records.data[index * 3u];
	if (info.w == 0u || info.z >= 8u) {
		return; // Not traced: the bit keeps its last answer.
	}
	ivec2 coord = ivec2(info.x & 0xFFFFu, info.x >> 16u);
	uint bit = 1u << info.z;
	uint mask = imageLoad(mask_image, coord).r;
	mask = info.w == 2u ? (mask | bit) : (mask & ~bit);
	imageStore(mask_image, coord, uvec4(mask, 0u, 0u, 0u));
}
