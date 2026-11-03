#[compute]
#version 450

// Displaces tightly packed float3 vertices on Y (#45). std430 vec3 is 16-byte
// aligned, so the buffer is a float array and Y is at index i * 3 + 1.
// The displacement is at most the push-constant amplitude (10% of the grid extent).

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) restrict buffer Vertices {
	float data[];
};

layout(push_constant, std430) uniform Params {
	uint vertex_count;
	uint seed;
	float amplitude;
	float pad;
}
params;

uint hash_u32(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.vertex_count) {
		return;
	}
	uint h = hash_u32(i ^ params.seed);
	float y = (float(h & 0x00ffffffu) / float(0x00ffffffu)) * 2.0 - 1.0;
	data[i * 3u + 1u] += y * params.amplitude;
}
