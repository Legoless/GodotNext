#[compute]
#version 460
#VERSION_DEFINES
#define EXPLICIT_FP16

// GodotNext: ps_global_sdf (#182). Resample one clipmap at quarter
// resolution, then propagate its six-neighbour distance four times.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
#include "../half_inc.glsl"

layout(set = 0, binding = 0, std430) readonly buffer StaticPool {
	float16_t data[];
}
static_pool;
layout(set = 0, binding = 1, std430) readonly buffer DynamicPool {
	float16_t data[];
}
dynamic_pool;
layout(set = 0, binding = 2, std430) readonly buffer PageTable {
	int data[];
}
page_table;
layout(set = 0, binding = 3, std430) writeonly buffer CoarseOutput {
	float data[];
}
coarse_output;
layout(set = 0, binding = 4, std430) readonly buffer CoarseInput {
	float data[];
}
coarse_input;

layout(push_constant, std430) uniform Params {
	ivec4 origin;
	uint level;
	uint propagate;
	uint pad[2];
}
params;

float sample_fine(vec3 world) {
	float voxel_size = 0.1 * float(1u << params.level);
	ivec3 voxel = ivec3(floor(world / voxel_size)) - params.origin.xyz;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return 65504.0;
	}
	ivec3 brick = voxel / 8;
	ivec3 local = voxel % 8;
	int slot = page_table.data[brick.x + 16 * (brick.y + 16 * brick.z)];
	if (slot < 0) {
		return 65504.0;
	}
	uint index = uint(slot) * 512u + uint(local.x + 8 * (local.y + 8 * local.z));
	return min(float(static_pool.data[index]), float(dynamic_pool.data[index]));
}

void main() {
	ivec3 voxel = ivec3(gl_GlobalInvocationID);
	uint index = voxel.x + 32 * (voxel.y + 32 * voxel.z);
	float voxel_size = 0.1 * float(1u << params.level);
	float step = 4.0 * voxel_size;
	if (params.propagate == 0u) {
		vec3 world = vec3(params.origin.xyz) * voxel_size + (vec3(voxel) + 0.5) * step;
		coarse_output.data[index] = sample_fine(world);
		return;
	}
	float value = coarse_input.data[index];
	for (uint axis = 0u; axis < 3u; axis++) {
		for (int direction = -1; direction <= 1; direction += 2) {
			ivec3 offset = ivec3(0);
			offset[axis] = direction;
			ivec3 neighbor = voxel + offset;
			if (any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, ivec3(32)))) {
				continue;
			}
			uint neighbour_index = neighbor.x + 32 * (neighbor.y + 32 * neighbor.z);
			value = min(value, coarse_input.data[neighbour_index] + step);
		}
	}
	coarse_output.data[index] = value;
}
