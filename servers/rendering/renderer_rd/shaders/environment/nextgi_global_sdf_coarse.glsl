#[compute]
#version 460
#VERSION_DEFINES
#define EXPLICIT_FP16

// GodotNext: ps_global_sdf (#182). The quarter-resolution coarse level of one clipmap.
// GodotNext: global_sdf_coarse_bound (#257). A conservative lower bound, which the software
// trace steps by (plan §21.2 L4; UE keeps a lower-resolution mip of each global distance field
// clipmap, r.AOGlobalDistanceField.MipFactor). NextGIGlobalSDF::build_coarse_bound() is the CPU
// twin; keep the two the same.
// - A cell of an allocated brick holds the minimum of the fine field over its 4^3 voxels and a
//   one-voxel border (every corner a trilinear sample in the cell can read), minus a fine
//   voxel's half-diagonal, so it never exceeds the trace's sample nor the distance itself.
//   Voxels the trace reads as absent (unallocated, or 60000 and more) do not count; a cell
//   with none left holds 0.
// - A cell of a missing brick (geometry without a pool slot) holds -FAR: never a step.
// - A cell of an unallocated brick holds the distance to the nearest cell that may hold a
//   surface (allocated or missing, or past the level's edge), less one cell: a Chebyshev flood
//   over the 26 neighbors, ITERATIONS cells deep and capped there.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
#include "../half_inc.glsl"

#define COARSE_SIZE 32
#define COARSE_VOXELS 32768u
#define COARSE_FAR 65504.0
#define COARSE_ABSENT 60000.0
#define PAGE_MISSING -2

#define MODE_SEED 0u
#define MODE_PROPAGATE_A_TO_B 1u
#define MODE_PROPAGATE_B_TO_A 2u
#define MODE_FINALIZE_A 3u
#define MODE_FINALIZE_B 4u

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
// Every level's bound, COARSE_VOXELS floats per level: the trace's binding 30.
layout(set = 0, binding = 3, std430) buffer CoarseBound {
	float data[];
}
coarse_bound;
// The seed of each cell: the allocated minimum, -FAR for missing, FAR for unallocated.
layout(set = 0, binding = 4, std430) buffer CoarseSeed {
	float data[];
}
coarse_seed;
// The flood's two buffers: the distance to the nearest surface cell, in cells times the cell size.
layout(set = 0, binding = 5, std430) buffer FloodA {
	float data[];
}
flood_a;
layout(set = 0, binding = 6, std430) buffer FloodB {
	float data[];
}
flood_b;

layout(push_constant, std430) uniform Params {
	uint level;
	uint mode;
	float voxel_size;
	uint iterations;
}
params;

uint cell_index(ivec3 cell) {
	return uint(cell.x + COARSE_SIZE * (cell.y + COARSE_SIZE * cell.z));
}

int brick_slot(ivec3 voxel) {
	ivec3 brick = voxel >> 3;
	return page_table.data[brick.x + 16 * (brick.y + 16 * brick.z)];
}

float flood_read(ivec3 cell, bool from_a) {
	uint index = cell_index(cell);
	return from_a ? flood_a.data[index] : flood_b.data[index];
}

void main() {
	ivec3 cell = ivec3(gl_GlobalInvocationID);
	uint index = cell_index(cell);
	float cell_size = 4.0 * params.voxel_size;
	if (params.mode == MODE_SEED) {
		int slot = brick_slot(cell * 4);
		float seed;
		float flood;
		if (slot == PAGE_MISSING) {
			seed = -COARSE_FAR;
			flood = 0.0;
		} else if (slot < 0) {
			seed = COARSE_FAR;
			flood = float(params.iterations + 1u) * cell_size;
		} else {
			float lowest = COARSE_FAR;
			for (int z = -1; z <= 4; z++) {
				for (int y = -1; y <= 4; y++) {
					for (int x = -1; x <= 4; x++) {
						ivec3 voxel = cell * 4 + ivec3(x, y, z);
						if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
							continue;
						}
						int voxel_slot = brick_slot(voxel);
						if (voxel_slot < 0) {
							continue; // The trace reads no value there.
						}
						ivec3 local = voxel & ivec3(7);
						uint voxel_index = uint(voxel_slot) * 512u + uint(local.x + 8 * (local.y + 8 * local.z));
						float value = min(float(static_pool.data[voxel_index]), float(dynamic_pool.data[voxel_index]));
						if (value < COARSE_ABSENT) { // swrt_fetch() reads a far voxel as absent.
							lowest = min(lowest, value);
						}
					}
				}
			}
			// A cell with no voxel the trace reads is a stop with no step of its own.
			seed = lowest < COARSE_ABSENT ? lowest - 0.8660254 * params.voxel_size : 0.0;
			flood = 0.0;
		}
		coarse_seed.data[index] = seed;
		flood_a.data[index] = flood;
		return;
	}
	if (params.mode == MODE_PROPAGATE_A_TO_B || params.mode == MODE_PROPAGATE_B_TO_A) {
		bool from_a = params.mode == MODE_PROPAGATE_A_TO_B;
		float value = flood_read(cell, from_a);
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					ivec3 neighbor = cell + ivec3(x, y, z);
					// Past the level's edge lies geometry this level does not hold.
					float other = (any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, ivec3(COARSE_SIZE)))) ? 0.0 : flood_read(neighbor, from_a);
					value = min(value, other + cell_size);
				}
			}
		}
		if (from_a) {
			flood_b.data[index] = value;
		} else {
			flood_a.data[index] = value;
		}
		return;
	}
	float seed = coarse_seed.data[index];
	float flood = flood_read(cell, params.mode == MODE_FINALIZE_A);
	coarse_bound.data[params.level * COARSE_VOXELS + index] = seed >= COARSE_FAR ? flood - cell_size : seed;
}
