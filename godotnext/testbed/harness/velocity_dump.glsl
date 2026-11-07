#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rg16f, set = 0, binding = 0) uniform readonly image2D velocity_buffer;

layout(std430, set = 0, binding = 1) buffer Counts {
	uint sentinel;
	uint nonfinite;
}
counts;

layout(push_constant, std430) uniform Params {
	uvec2 resolution;
	uint count_sentinel;
	uint pad;
}
params;

void main() {
	if (any(greaterThanEqual(gl_GlobalInvocationID.xy, params.resolution))) {
		return;
	}
	vec2 velocity = imageLoad(velocity_buffer, ivec2(gl_GlobalInvocationID.xy)).xy;
	uvec2 bits = floatBitsToUint(velocity);
	bool is_sentinel = (bits.x == 0xBF800000u) && (bits.y == 0xBF800000u);
	bool is_nonfinite = ((bits.x & 0x7F800000u) == 0x7F800000u) || ((bits.y & 0x7F800000u) == 0x7F800000u);
	if (params.count_sentinel != 0u && is_sentinel) {
		atomicAdd(counts.sentinel, 1u);
	}
	if (is_nonfinite) {
		atomicAdd(counts.nonfinite, 1u);
	}
}
