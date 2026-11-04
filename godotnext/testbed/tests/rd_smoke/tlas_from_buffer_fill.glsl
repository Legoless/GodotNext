#version 460

// Fixture kernel for tlas_from_buffer.gd (#57). Writes 1024 instance records in the driver's own
// layout so the GPU-built TLAS can be compared against the CPU tlas_build path.
// Compile with LAYOUT_72 for Metal, without it for Vulkan's 64-byte record.

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Instances {
	uint data[];
}
instances;

layout(push_constant, std430) uniform Params {
	uint count;
	uint grid;
	uvec2 ref0;
	uvec2 ref1;
	uvec2 ref2;
	uvec2 ref3;
}
params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.count) {
		return;
	}
	uint gx = index % params.grid;
	uint gy = index / params.grid;
	float x = (float(gx) - float(params.grid) * 0.5 + 0.5) * 10.0;
	float y = (float(gy) - float(params.grid) * 0.5 + 0.5) * 10.0;
	uint which = (gx + gy) % 4u;
	uint id = 1000u + index;

	uvec2 reference = params.ref0;
	if (which == 1u) {
		reference = params.ref1;
	} else if (which == 2u) {
		reference = params.ref2;
	} else if (which == 3u) {
		reference = params.ref3;
	}

#ifdef LAYOUT_72
	uint base = index * 18u;
	// Identity basis, translation (x, y, 0): four PackedFloat3 columns.
	instances.data[base + 0u] = floatBitsToUint(1.0);
	instances.data[base + 1u] = floatBitsToUint(0.0);
	instances.data[base + 2u] = floatBitsToUint(0.0);
	instances.data[base + 3u] = floatBitsToUint(0.0);
	instances.data[base + 4u] = floatBitsToUint(1.0);
	instances.data[base + 5u] = floatBitsToUint(0.0);
	instances.data[base + 6u] = floatBitsToUint(0.0);
	instances.data[base + 7u] = floatBitsToUint(0.0);
	instances.data[base + 8u] = floatBitsToUint(1.0);
	instances.data[base + 9u] = floatBitsToUint(x);
	instances.data[base + 10u] = floatBitsToUint(y);
	instances.data[base + 11u] = floatBitsToUint(0.0);
	instances.data[base + 12u] = 0u; // options
	instances.data[base + 13u] = 0xFFu; // mask
	instances.data[base + 14u] = 0u; // intersection function table offset
	instances.data[base + 15u] = id;
	instances.data[base + 16u] = reference.x;
	instances.data[base + 17u] = reference.y;
#else
	uint base = index * 16u;
	// Row-major 3x4, translation (x, y, 0).
	instances.data[base + 0u] = floatBitsToUint(1.0);
	instances.data[base + 1u] = floatBitsToUint(0.0);
	instances.data[base + 2u] = floatBitsToUint(0.0);
	instances.data[base + 3u] = floatBitsToUint(x);
	instances.data[base + 4u] = floatBitsToUint(0.0);
	instances.data[base + 5u] = floatBitsToUint(1.0);
	instances.data[base + 6u] = floatBitsToUint(0.0);
	instances.data[base + 7u] = floatBitsToUint(y);
	instances.data[base + 8u] = floatBitsToUint(0.0);
	instances.data[base + 9u] = floatBitsToUint(0.0);
	instances.data[base + 10u] = floatBitsToUint(1.0);
	instances.data[base + 11u] = floatBitsToUint(0.0);
	instances.data[base + 12u] = (id & 0x00FFFFFFu) | (0xFFu << 24u);
	instances.data[base + 13u] = 0u; // SBT offset 0, flags 0
	instances.data[base + 14u] = reference.x;
	instances.data[base + 15u] = reference.y;
#endif
}
