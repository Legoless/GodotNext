#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_gpu_cull (#76). Level-synchronous BVH8 traversal and
// cluster compaction. Variants:
//   (none)       BVH_LEVEL: descend one level of the tree.
//   CLUSTER_CULL: test leaf groups and emit (record, cluster).
//   CUT_HASH:    XOR the order-independent hash of every compacted cluster.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
#include "nextgeo_cull_inc.glsl"
// clang-format on

layout(set = 0, binding = 8, std430) buffer restrict readonly LevelInput {
	uvec2 data[];
}
level_input;
layout(set = 0, binding = 9, std430) buffer restrict LevelOutput {
	uvec2 data[];
}
level_output;
layout(set = 0, binding = 10, std430) buffer restrict GroupsOut {
	uvec2 data[];
}
groups_out;
layout(set = 0, binding = 11, std430) buffer restrict ClustersOut {
	uvec2 data[];
}
clusters_out;

#ifdef CUT_HASH
// Matches RendererRD::nextgeo_cut_pair_hash, split into two 32-bit words so
// the Metal backend never sees a 64-bit integer type.
const uint PHASE2_CLUSTER_BASE = 1u << 20u;

uint mix32(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

void hash_pair(uvec2 pair) {
	uint lo = mix32(pair.x ^ mix32(pair.y ^ 0x9e3779b9u));
	uint hi = mix32(pair.x ^ 0x85ebca6bu) ^ mix32(pair.y ^ 0xc2b2ae35u);
	atomicXor(counters.cut_hash[0], lo);
	atomicXor(counters.cut_hash[1], hi);
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index < counters.phase1_clusters) {
		hash_pair(clusters_out.data[index]);
	}
	if (index < counters.phase2_clusters) {
		hash_pair(clusters_out.data[PHASE2_CLUSTER_BASE + index]);
	}
}
#else
layout(push_constant, std430) uniform Params {
	uint level;
	uint output_capacity;
	uint list_base;
	uint phase;
}
params;
layout(set = 0, binding = 12, std430) buffer restrict BVHArgs {
	uvec4 data[];
}
bvh_args;
layout(set = 0, binding = 13, std430) buffer restrict DrawArgs {
	uvec4 data[];
}
draw_args;

vec3 instance_point(NextGeoInstanceRecord p_record, vec3 p_local) {
	return vec3(
			dot(p_record.transform[0].xyz, p_local) + p_record.transform[0].w,
			dot(p_record.transform[1].xyz, p_local) + p_record.transform[1].w,
			dot(p_record.transform[2].xyz, p_local) + p_record.transform[2].w);
}

// Plan §7.3 step 3: the projected error of a mesh-space sphere with the
// node's maximum parent error as the geometric error term.
float projected_error(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error) {
	float scale = p_record.world_aabb_min.w;
	vec3 center = instance_point(p_record, p_lod_sphere.xyz);
	float radius = p_lod_sphere.w * scale;
	float distance = max(length(center - cull.camera.xyz) - radius, cull.view.z);
	return p_max_parent_error * scale * 0.5 * cull.view.y * abs(cull.view_proj[1][1]) / distance * cull.view.w;
}

uint dag_uint(uint p_byte_offset) {
	return dag.data[p_byte_offset >> 2u];
}

float dag_float(uint p_byte_offset) {
	return uintBitsToFloat(dag.data[p_byte_offset >> 2u]);
}

#ifdef CLUSTER_CULL
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint input_count = params.phase == 0u ? counters.group_count : counters.phase2_group_count;
	if (index >= input_count) {
		return;
	}
	uvec2 entry = level_input.data[index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.bvh_node_offset <= mesh.group_offset) {
		return;
	}
	uint group_count = (mesh.bvh_node_offset - mesh.group_offset) / 60u;
	if (entry.y >= group_count) {
		return;
	}
	const uint base = mesh.group_offset + entry.y * 60u;
	uint cluster_offset = dag_uint(base);
	uint cluster_count = dag_uint(base + 4u);
	vec4 parent_lod = vec4(dag_float(base + 16u), dag_float(base + 20u), dag_float(base + 24u), dag_float(base + 28u));
	float parent_error = dag_float(base + 32u);
	if (projected_error(record, parent_lod, parent_error) <= cull.camera.w) {
		return;
	}
	for (uint cluster = 0u; cluster < cluster_count; cluster++) {
		uint slot = params.phase == 0u ? atomicAdd(counters.phase1_clusters, 1u) : atomicAdd(counters.phase2_clusters, 1u);
		if (slot >= params.output_capacity) {
			atomicAdd(counters.overflow, 1u);
			return;
		}
		atomicAdd(counters.cluster_count, 1u);
		atomicAdd(draw_args.data[params.phase].y, 1u);
		clusters_out.data[params.list_base + slot] = uvec2(entry.x, cluster_offset + cluster);
	}
}
#else
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint input_count = 0u;
	if (params.phase == 0u) {
		input_count = params.level == 0u ? counters.phase1_count : counters.level_counts[params.level - 1u];
	} else {
		input_count = params.level == 0u ? counters.phase2_visible_count : counters.phase2_level_counts[params.level - 1u];
	}
	if (index >= input_count) {
		return;
	}
	uvec2 entry = level_input.data[index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	const uint base = mesh.bvh_node_offset + entry.y * 80u;
	vec4 lod_sphere = vec4(dag_float(base), dag_float(base + 4u), dag_float(base + 8u), dag_float(base + 12u));
	float max_parent_error = dag_float(base + 16u);
	uint child_count = min(dag_uint(base + 44u), 8u);
	if (projected_error(record, lod_sphere, max_parent_error) <= cull.camera.w) {
		return;
	}
	for (uint child = 0u; child < child_count; child++) {
		uint value = dag_uint(base + 48u + child * 4u);
		if ((value & NEXTGEO_BVH_LEAF_BIT) != 0u) {
			uint slot = 0u;
			if (params.phase == 0u) {
				slot = atomicAdd(counters.group_count, 1u);
			} else {
				atomicAdd(counters.group_count, 1u);
				slot = atomicAdd(counters.phase2_group_count, 1u);
			}
			if (slot >= params.output_capacity) {
				atomicAdd(counters.overflow, 1u);
				continue;
			}
			groups_out.data[slot] = uvec2(entry.x, value & ~NEXTGEO_BVH_LEAF_BIT);
		} else {
			uint slot = params.phase == 0u ? atomicAdd(counters.level_counts[params.level], 1u) : atomicAdd(counters.phase2_level_counts[params.level], 1u);
			if (slot >= params.output_capacity) {
				atomicAdd(counters.overflow, 1u);
				continue;
			}
			uint groups = (slot + 64u) / 64u;
			uint args_base = params.phase == 0u ? 1u : 10u;
			atomicMax(bvh_args.data[args_base + min(params.level, 7u)].x, groups);
			level_output.data[slot] = uvec2(entry.x, value);
		}
	}
}
#endif
#endif
