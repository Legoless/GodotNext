#[compute]
#version 460
#VERSION_DEFINES
#define EXPLICIT_FP16

// GodotNext: ps_global_sdf (#182). Min-composite sparse global-SDF bricks
// from source mesh-SDF pages, split into static and dynamic slots.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
#include "../half_inc.glsl"

struct MeshInfo {
	vec3 bounds_position;
	float voxel_size;
	ivec3 dimensions;
	uint flags;
	ivec3 brick_grid;
	float band;
	uint page_offset;
	uint brick_offset;
	uint pad[2];
};

struct Instance {
	vec4 inverse[3];
	vec4 world_min;
	vec4 world_max;
	uint flags;
	uint pad[3];
};

struct BrickWork {
	ivec4 brick;
	uint slot;
	uint instance_offset;
	uint instance_count;
	uint pad;
};

layout(set = 0, binding = 0, std430) readonly buffer Instances {
	Instance data[];
}
instances;
layout(set = 0, binding = 1, std430) readonly buffer Meshes {
	MeshInfo data[];
}
meshes;
layout(set = 0, binding = 2, std430) readonly buffer PageTables {
	int data[];
}
page_tables;
layout(set = 0, binding = 3, std430) readonly buffer BrickData {
	uint data[];
}
brick_data;
layout(set = 0, binding = 4, std430) writeonly buffer StaticPool {
	float16_t data[];
}
static_pool;
layout(set = 0, binding = 5, std430) writeonly buffer DynamicPool {
	float16_t data[];
}
dynamic_pool;
layout(set = 0, binding = 6, std430) buffer Coverage {
	uint data[];
}
coverage;
layout(set = 0, binding = 7, std430) readonly buffer Work {
	BrickWork data[];
}
work;
layout(set = 0, binding = 8, std430) readonly buffer WorkIndices {
	uint data[];
}
work_indices;

layout(push_constant, std430) uniform Params {
	uint level;
	ivec4 origin;
	ivec4 brick;
	uint slot;
	uint instance_count;
	uint pad[2];
}
params;

float voxel_value(MeshInfo mesh, ivec3 voxel) {
	ivec3 clamped = clamp(voxel, ivec3(0), mesh.dimensions - ivec3(1));
	ivec3 brick = clamped / 8;
	ivec3 local = clamped % 8;
	int entry = page_tables.data[mesh.page_offset + uint(brick.x + mesh.brick_grid.x * (brick.y + mesh.brick_grid.y * brick.z))];
	if (entry == -1) {
		return mesh.band;
	}
	if (entry == -2) {
		return -mesh.band;
	}
	uint voxel_index = uint(local.x + 8 * (local.y + 8 * local.z));
	uint packed = brick_data.data[mesh.brick_offset + uint(entry) * 128u + voxel_index / 4u];
	uint encoded = (packed >> ((voxel_index % 4u) * 8u)) & 255u;
	return (float(encoded) / 255.0 * 2.0 - 1.0) * mesh.band;
}

float sample_mesh(MeshInfo mesh, vec3 position) {
	vec3 f = (position - mesh.bounds_position) / mesh.voxel_size - vec3(0.5);
	f = clamp(f, vec3(0.0), vec3(mesh.dimensions - ivec3(1)));
	ivec3 base = ivec3(floor(f));
	vec3 frac = f - vec3(base);
	float c000 = voxel_value(mesh, base);
	float c100 = voxel_value(mesh, base + ivec3(1, 0, 0));
	float c010 = voxel_value(mesh, base + ivec3(0, 1, 0));
	float c110 = voxel_value(mesh, base + ivec3(1, 1, 0));
	float c001 = voxel_value(mesh, base + ivec3(0, 0, 1));
	float c101 = voxel_value(mesh, base + ivec3(1, 0, 1));
	float c011 = voxel_value(mesh, base + ivec3(0, 1, 1));
	float c111 = voxel_value(mesh, base + ivec3(1, 1, 1));
	float x00 = mix(c000, c100, frac.x);
	float x10 = mix(c010, c110, frac.x);
	float x01 = mix(c001, c101, frac.x);
	float x11 = mix(c011, c111, frac.x);
	return mix(mix(x00, x10, frac.y), mix(x01, x11, frac.y), frac.z);
}

vec3 instance_point(Instance instance, vec3 world) {
	return vec3(
			dot(instance.inverse[0].xyz, world) + instance.inverse[0].w,
			dot(instance.inverse[1].xyz, world) + instance.inverse[1].w,
			dot(instance.inverse[2].xyz, world) + instance.inverse[2].w);
}

float sample_instance(Instance instance, MeshInfo mesh, vec3 world) {
	vec3 local = instance_point(instance, world);
	return sample_mesh(mesh, local) * instance.world_min.w;
}

void main() {
	BrickWork brick_work = work.data[gl_WorkGroupID.x];
	ivec3 voxel = ivec3(gl_LocalInvocationID);
	uint voxel_index = voxel.x + voxel.y * 8u + voxel.z * 64u;
	float voxel_size = 0.1 * float(1u << params.level);
	vec3 brick_min = vec3(params.origin.xyz + brick_work.brick.xyz * 8) * voxel_size;
	vec3 world = brick_min + (vec3(voxel) + 0.5) * voxel_size;
	float static_distance = 65504.0;
	float dynamic_distance = 65504.0;

	for (uint index = 0; index < brick_work.instance_count; index++) {
		uint source_index = work_indices.data[brick_work.instance_offset + index];
		Instance instance = instances.data[source_index];
		vec3 lo = brick_min - vec3(voxel_size);
		vec3 hi = brick_min + vec3(voxel_size * 9.0);
		if (any(greaterThan(instance.world_min.xyz, hi)) || any(lessThan(instance.world_max.xyz, lo))) {
			continue;
		}
		MeshInfo mesh = meshes.data[source_index];
		float distance = sample_instance(instance, mesh, world);
		if (distance >= 65504.0) {
			continue;
		}
		if ((instance.flags & 1u) != 0u) {
			dynamic_distance = min(dynamic_distance, distance);
		} else {
			static_distance = min(static_distance, distance);
		}
	}

	uint pool_index = brick_work.slot * 512u + voxel_index;
	static_pool.data[pool_index] = float16_t(static_distance);
	dynamic_pool.data[pool_index] = float16_t(dynamic_distance);

	for (uint index = 0; index < brick_work.instance_count; index++) {
		uint source_index = work_indices.data[brick_work.instance_offset + index];
		Instance instance = instances.data[source_index];
		if ((instance.flags & 2u) == 0u) {
			continue;
		}
		MeshInfo mesh = meshes.data[source_index];
		float distance = sample_instance(instance, mesh, world);
		float target = (instance.flags & 1u) != 0u ? dynamic_distance : static_distance;
		if (distance == target && distance < 65504.0) {
			atomicOr(coverage.data[brick_work.slot * 16u + voxel_index / 8u], 1u << (voxel_index & 7u));
		}
	}
}
