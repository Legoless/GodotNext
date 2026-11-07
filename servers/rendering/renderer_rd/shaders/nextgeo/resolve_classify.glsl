#[compute]

#version 450
#extension GL_EXT_samplerless_texture_functions : enable

#VERSION_DEFINES

// GodotNext: nextgeo_resolve (#80, plan §6.1, §7.4).
// 8x8 tile classifier over RB_TEX_NEXTGEO_VISIBILITY: builds per-MaterialBucket
// indirect draw arguments ({6, tile_count, 0, 0}) and compacted tile lists.

// clang-format off
#include "nextgeo_records_inc.glsl"
// clang-format on

#ifdef INIT_INDIRECT
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

layout(push_constant, std430) uniform PushParams {
	uvec2 viewport_size;
	uint bucket_count;
	uint max_tiles;
}
push_params;

layout(set = 0, binding = 0) uniform utexture2D visibility_tex;

layout(set = 0, binding = 1, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

layout(set = 0, binding = 2, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
records;

layout(set = 0, binding = 3, std430) buffer restrict readonly Meshes {
	NextGeoMeshRecord data[];
}
meshes;

layout(set = 0, binding = 4, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;

layout(set = 0, binding = 5, std430) buffer restrict readonly BucketTable {
	uint data[];
}
bucket_table;

layout(set = 0, binding = 6, std430) buffer restrict IndirectArgs {
	uvec4 data[];
}
indirect_args;

layout(set = 0, binding = 7, std430) buffer restrict writeonly TileList {
	uint data[];
}
tile_list;

#ifdef INIT_INDIRECT
void main() {
	uint slot = gl_GlobalInvocationID.x;
	if (slot >= push_params.bucket_count) {
		return;
	}
	// D3D12-safe zero firstInstance (plan §7.4 D2); tile_list_base is passed via push constant.
	indirect_args.data[slot] = uvec4(6u, 0u, 0u, 0u);
}
#else
shared uint tile_buckets[64];

void main() {
	uint tid = gl_LocalInvocationIndex;
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	uint my_bucket = 0xFFFFFFFFu;

	if (px.x < int(push_params.viewport_size.x) && px.y < int(push_params.viewport_size.y)) {
		uint vis_id = texelFetch(visibility_tex, px, 0).r;
		if (vis_id != 0xFFFFFFFFu) {
			uint vis_cluster = vis_id >> 7u;
			uvec2 entry = visible_clusters.data[vis_cluster];
			NextGeoInstanceRecord record = records.data[entry.x];
			if ((record.flags & NEXTGEO_RECORD_VALID) != 0u) {
				NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
				if (mesh.page_count > 0u && entry.y >= mesh.cluster_offset) {
					uint local_cluster = entry.y - mesh.cluster_offset;
					if (local_cluster < mesh.cluster_count) {
						uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
						uint surface_id = desc.w & 0xFFFFu;
						if (surface_id < 32u && (record.surface_mask & (1u << surface_id)) != 0u) {
							uint candidate = bucket_table.data[record.bucket_base + surface_id] & NEXTGEO_BUCKET_ID_MASK;
							if (candidate < push_params.bucket_count) {
								my_bucket = candidate;
							}
						}
					}
				}
			}
		}
	}

	tile_buckets[tid] = my_bucket;
	barrier();

	if (my_bucket != 0xFFFFFFFFu) {
		bool first_occurrence = true;
		for (uint prev = 0u; prev < tid; prev++) {
			if (tile_buckets[prev] == my_bucket) {
				first_occurrence = false;
				break;
			}
		}
		if (first_occurrence) {
			uint slot = atomicAdd(indirect_args.data[my_bucket].y, 1u);
			if (slot < push_params.max_tiles) {
				tile_list.data[my_bucket * push_params.max_tiles + slot] = (gl_WorkGroupID.x & 0xFFFFu) | (gl_WorkGroupID.y << 16u);
			}
		}
	}
}
#endif
