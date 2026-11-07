// GodotNext: nextgeo_gpu_cull (#76, plan §7.3)
// Shared cull layouts. Offsets match RendererRD::VirtualMeshStorage
// (records) and nextgeo_fill_vgd() (DAG groups and BVH8 nodes).

struct NextGeoGroup {
	uint cluster_offset; // 0
	uint cluster_count; // 4
	uint page_id; // 8
	uint depth; // 12
	vec4 parent_lod; // 16 (center xyz, radius)
	float parent_error; // 32
	vec3 aabb_min; // 36
	vec3 aabb_max; // 48
}; // 60 B

struct NextGeoBVHNode {
	vec4 lod_sphere; // 0
	float max_parent_error; // 16
	vec3 aabb_min; // 20
	vec3 aabb_max; // 32
	uint child_count; // 44
	uint children[8]; // 48
}; // 80 B

const uint NEXTGEO_BVH_LEAF_BIT = 0x80000000u;

layout(set = 0, binding = 0, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
instances;
layout(set = 0, binding = 1, std430) buffer restrict readonly Meshes {
	NextGeoMeshRecord data[];
}
meshes;
layout(set = 0, binding = 2, std430) buffer restrict readonly Dag {
	uint data[];
}
dag;

layout(set = 0, binding = 3, std430) buffer restrict readonly CullParams {
	mat4 view_proj;
	mat4 prev_view_proj;
	vec4 frustum[6];
	vec4 camera; // xyz, w = max pixel error
	vec4 view; // x = internal width, y = internal height, z = z_near, w = lod bias
	uvec4 cull; // x = layer mask, y = instance count, z = list capacity, w = phase2 offset
}
cull;

layout(set = 0, binding = 4, std430) buffer restrict Lists {
	uvec2 data[];
}
lists;

layout(set = 0, binding = 5, std430) buffer restrict Counters {
	uint phase1_count;
	uint phase2_count;
	uint overflow;
	uint cluster_count;
	uint cut_lo;
	uint cut_hi;
	uint group_count;
	uint level_counts[8];
	uint cut_hash[2];
	uint pad[2];
	uint phase1_clusters;
	uint phase2_clusters;
	uint phase2_visible_count;
	uint phase2_group_count;
	uint phase2_level_counts[8];
}
counters;

layout(set = 0, binding = 6, std430) buffer restrict Nodes {
	uvec2 data[];
}
nodes;

layout(set = 0, binding = 7) uniform sampler2D hiz_prev;
