#[compute]

#version 450

// Async compute check (#174): dst[i] = a chain of integer hashes of src[i] ^ salt. The CPU
// recomputes every value, so a list that ran before its input was ready, or a readback that ran
// before the list finished, shows up as a wrong value.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Source {
	uint data[];
}
source_buffer;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Destination {
	uint data[];
}
destination_buffer;

layout(push_constant, std430) uniform Params {
	uint count;
	uint rounds;
	uint salt;
	uint pad;
}
params;

uint hash(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	x ^= x >> 16;
	return x;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	uint value = source_buffer.data[i] ^ params.salt;
	for (uint round = 0u; round < params.rounds; round++) {
		value = hash(value + round);
	}
	destination_buffer.data[i] = value;
}
