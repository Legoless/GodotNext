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

#ifdef SHADOW_VIEW
layout(push_constant, std430) uniform HashParams {
	uint cluster_capacity;
	uint view_count;
	uint pad0;
	uint pad1;
}
hash_params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint view_idx = gl_GlobalInvocationID.y;
	if (view_idx >= hash_params.view_count) {
		return;
	}
	uint count = min(shadow_counters.data[view_idx].phase1_clusters, hash_params.cluster_capacity);
	if (index < count) {
		uvec2 pair = clusters_out.data[view_idx * hash_params.cluster_capacity + index];
		uint lo = mix32(pair.x ^ mix32(pair.y ^ 0x9e3779b9u));
		uint hi = mix32(pair.x ^ 0x85ebca6bu) ^ mix32(pair.y ^ 0xc2b2ae35u);
		atomicXor(shadow_counters.data[view_idx].cut_hash[0], lo);
		atomicXor(shadow_counters.data[view_idx].cut_hash[1], hi);
	}
}
#else
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
#endif
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
layout(set = 0, binding = 14, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;
// Word layout:
//   [0] = request_count, [1] = request_capacity, [2] = overflow, [3] = used_slot_words
//   [4 .. 4 + used_slot_words) = active resident slot bitset
//   [4 + used_slot_words + req * 4 + 0..3] = ((visible<<31)|mesh_idx, page_id, dist_bits, err_bits)
layout(set = 0, binding = 15, std430) buffer restrict PageRequests {
	uint data[];
}
page_requests;

vec3 instance_point(NextGeoInstanceRecord p_record, vec3 p_local) {
	return vec3(
			dot(p_record.transform[0].xyz, p_local) + p_record.transform[0].w,
			dot(p_record.transform[1].xyz, p_local) + p_record.transform[1].w,
			dot(p_record.transform[2].xyz, p_local) + p_record.transform[2].w);
}

// Plan §7.3 step 3 & §7.5: projected error in pixels / shadow texels for
// perspective, orthographic directional splits (bit 2), and dual-paraboloid
// hemispheres (bit 3).
float projected_error_params(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error, vec4 p_camera, vec4 p_view, mat4 p_view_proj, uint p_flags) {
	if (p_max_parent_error >= 3.4e38) {
		return p_camera.w + 1.0;
	}
	float scale = p_record.world_aabb_min.w;
	if ((p_flags & 4u) != 0u) {
		return p_max_parent_error * scale * p_view.x * p_view.w;
	}
	vec3 center = instance_point(p_record, p_lod_sphere.xyz);
	float radius = p_lod_sphere.w * scale;
	float distance = max(length(center - p_camera.xyz) - radius, p_view.z);
	if ((p_flags & 8u) != 0u) {
		return p_max_parent_error * scale * 0.5 * p_view.y / distance * p_view.w;
	}
	return p_max_parent_error * scale * 0.5 * p_view.y * abs(p_view_proj[1][1]) / distance * p_view.w;
}

#ifndef SHADOW_VIEW
float projected_error(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error) {
	return projected_error_params(p_record, p_lod_sphere, p_max_parent_error, cull.camera, cull.view, cull.view_proj, cull.cull.w);
}
#endif

uint dag_uint(uint p_byte_offset) {
	return dag.data[p_byte_offset >> 2u];
}

float dag_float(uint p_byte_offset) {
	return uintBitsToFloat(dag.data[p_byte_offset >> 2u]);
}

bool mesh_group_refine_ok(uint p_mesh_idx, uint p_group_idx) {
	if (p_mesh_idx >= residency.data[1]) {
		return true;
	}
	uint hdr = 2u + p_mesh_idx * 4u;
	uint res_groups = residency.data[hdr + 2u];
	if (p_group_idx >= res_groups) {
		return false;
	}
	uint refine_off = residency.data[hdr + 0u];
	uint word = residency.data[refine_off + (p_group_idx >> 5u)];
	return (word & (1u << (p_group_idx & 31u))) != 0u;
}

#ifdef CLUSTER_CULL
#ifdef SHADOW_VIEW
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint view_idx = gl_GlobalInvocationID.y;
	if (view_idx >= params.phase) {
		return;
	}
	uint group_cap = params.list_base;
	uint cluster_cap = params.output_capacity;
	uint input_count = min(shadow_counters.data[view_idx].group_count, group_cap);
	if (index >= input_count) {
		return;
	}
	NextGeoCullParamsData sc = shadow_culls.data[view_idx];
	uvec2 entry = level_input.data[view_idx * group_cap + index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	uint mesh_idx = record.virtual_mesh;
	NextGeoMeshRecord mesh = meshes.data[mesh_idx];
	if (mesh.bvh_node_offset <= mesh.group_offset) {
		return;
	}
	uint group_count = (mesh.bvh_node_offset - mesh.group_offset) / 60u;
	if (entry.y >= group_count || !mesh_group_refine_ok(mesh_idx, entry.y)) {
		return;
	}
	const uint base = mesh.group_offset + entry.y * 60u;
	uint cluster_offset = dag_uint(base);
	uint cluster_count = dag_uint(base + 4u);
	uint group_page_id = dag_uint(base + 8u);
	vec4 parent_lod = vec4(dag_float(base + 16u), dag_float(base + 20u), dag_float(base + 24u), dag_float(base + 28u));
	float parent_error = dag_float(base + 32u);
	if (projected_error_params(record, parent_lod, parent_error, sc.camera, sc.view, sc.view_proj, sc.cull.w) <= sc.camera.w) {
		return;
	}

	uint page_slot = 0xFFFFFFFFu;
	uint page_first_cluster = 0u;
	uint slot_page_size = residency.data[0];
	if (mesh_idx < residency.data[1]) {
		uint hdr = 2u + mesh_idx * 4u;
		uint page_table_off = residency.data[hdr + 1u];
		uint res_pages = residency.data[hdr + 3u];
		if (group_page_id < res_pages) {
			page_slot = residency.data[page_table_off + group_page_id * 2u + 0u];
			page_first_cluster = residency.data[page_table_off + group_page_id * 2u + 1u];
		}
	}
	uint page_cluster_hdr = (page_slot != 0xFFFFFFFFu && slot_page_size >= 96u) ? pages.data[(page_slot * slot_page_size) >> 2u] : 0u;

	for (uint cluster = 0u; cluster < cluster_count; cluster++) {
		uint global_cluster = cluster_offset + cluster;
		if (page_cluster_hdr > 0u && global_cluster >= mesh.cluster_offset) {
			uint local_cluster = global_cluster - mesh.cluster_offset;
			if (local_cluster >= page_first_cluster && (local_cluster - page_first_cluster) < page_cluster_hdr) {
				uint in_page = local_cluster - page_first_cluster;
				uint rec_word = ((page_slot * slot_page_size) >> 2u) + 4u + in_page * 20u;
				int refined_group = int(pages.data[rec_word + 15u]);
				if (refined_group >= 0 && uint(refined_group) < group_count) {
					vec4 own_lod = vec4(
							uintBitsToFloat(pages.data[rec_word + 5u]),
							uintBitsToFloat(pages.data[rec_word + 6u]),
							uintBitsToFloat(pages.data[rec_word + 7u]),
							uintBitsToFloat(pages.data[rec_word + 8u]));
					float own_error = uintBitsToFloat(pages.data[rec_word + 9u]);
					float child_px_err = projected_error_params(record, own_lod, own_error, sc.camera, sc.view, sc.view_proj, sc.cull.w);
					if (child_px_err > sc.camera.w) {
						uint child_g = uint(refined_group);
						if (mesh_group_refine_ok(mesh_idx, child_g)) {
							continue;
						}
						// Shadow views never request pages (plan §7.5): fall back to resident cut.
					}
				}
			}
		}

		uint slot = atomicAdd(shadow_counters.data[view_idx].phase1_clusters, 1u);
		if (slot >= cluster_cap) {
			atomicAdd(shadow_counters.data[view_idx].overflow, 1u);
			break;
		}
		atomicAdd(shadow_counters.data[view_idx].cluster_count, 1u);
		atomicAdd(draw_args.data[view_idx].y, 1u);
		clusters_out.data[view_idx * cluster_cap + slot] = uvec2(entry.x, global_cluster);
	}
}
#else
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint input_count = params.phase == 0u ? counters.group_count : counters.phase2_group_count;
	if (index >= input_count) {
		return;
	}
	uvec2 entry = level_input.data[index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	uint mesh_idx = record.virtual_mesh;
	NextGeoMeshRecord mesh = meshes.data[mesh_idx];
	if (mesh.bvh_node_offset <= mesh.group_offset) {
		return;
	}
	uint group_count = (mesh.bvh_node_offset - mesh.group_offset) / 60u;
	if (entry.y >= group_count || !mesh_group_refine_ok(mesh_idx, entry.y)) {
		return;
	}
	const uint base = mesh.group_offset + entry.y * 60u;
	uint cluster_offset = dag_uint(base);
	uint cluster_count = dag_uint(base + 4u);
	uint group_page_id = dag_uint(base + 8u);
	vec4 parent_lod = vec4(dag_float(base + 16u), dag_float(base + 20u), dag_float(base + 24u), dag_float(base + 28u));
	float parent_error = dag_float(base + 32u);
	if (projected_error(record, parent_lod, parent_error) <= cull.camera.w) {
		return;
	}

	uint page_slot = 0xFFFFFFFFu;
	uint page_first_cluster = 0u;
	uint slot_page_size = residency.data[0];
	if (mesh_idx < residency.data[1]) {
		uint hdr = 2u + mesh_idx * 4u;
		uint page_table_off = residency.data[hdr + 1u];
		uint res_pages = residency.data[hdr + 3u];
		if (group_page_id < res_pages) {
			page_slot = residency.data[page_table_off + group_page_id * 2u + 0u];
			page_first_cluster = residency.data[page_table_off + group_page_id * 2u + 1u];
		}
	}
	uint page_cluster_hdr = (page_slot != 0xFFFFFFFFu && slot_page_size >= 96u) ? pages.data[(page_slot * slot_page_size) >> 2u] : 0u;
	uint last_requested_page = 0xFFFFFFFFu;
	bool emitted_any = false;

	for (uint cluster = 0u; cluster < cluster_count; cluster++) {
		uint global_cluster = cluster_offset + cluster;
		if (page_cluster_hdr > 0u && global_cluster >= mesh.cluster_offset) {
			uint local_cluster = global_cluster - mesh.cluster_offset;
			if (local_cluster >= page_first_cluster && (local_cluster - page_first_cluster) < page_cluster_hdr) {
				uint in_page = local_cluster - page_first_cluster;
				uint rec_word = ((page_slot * slot_page_size) >> 2u) + 4u + in_page * 20u;
				int refined_group = int(pages.data[rec_word + 15u]);
				if (refined_group >= 0 && uint(refined_group) < group_count) {
					vec4 own_lod = vec4(
							uintBitsToFloat(pages.data[rec_word + 5u]),
							uintBitsToFloat(pages.data[rec_word + 6u]),
							uintBitsToFloat(pages.data[rec_word + 7u]),
							uintBitsToFloat(pages.data[rec_word + 8u]));
					float own_error = uintBitsToFloat(pages.data[rec_word + 9u]);
					float child_px_err = projected_error(record, own_lod, own_error);
					if (child_px_err > cull.camera.w) {
						uint child_g = uint(refined_group);
						if (mesh_group_refine_ok(mesh_idx, child_g)) {
							continue;
						}
						if ((cull.cull.w & 2u) != 0u) {
							uint missing_page = dag_uint(mesh.group_offset + child_g * 60u + 8u);
							if (missing_page != last_requested_page) {
								last_requested_page = missing_page;
								float scale = record.world_aabb_min.w;
								vec3 center = instance_point(record, own_lod.xyz);
								float dist = max(length(center - cull.camera.xyz) - own_lod.w * scale, 0.0);
								uint req_slot = atomicAdd(page_requests.data[0], 1u);
								if (req_slot < page_requests.data[1]) {
									uint req_base = 4u + page_requests.data[3] + req_slot * 4u;
									page_requests.data[req_base + 0u] = (1u << 31u) | mesh_idx;
									page_requests.data[req_base + 1u] = missing_page;
									page_requests.data[req_base + 2u] = floatBitsToUint(dist);
									page_requests.data[req_base + 3u] = floatBitsToUint(child_px_err);
								} else {
									atomicAdd(page_requests.data[2], 1u);
								}
							}
						}
					}
				}
			}
		}

		uint slot = params.phase == 0u ? atomicAdd(counters.phase1_clusters, 1u) : atomicAdd(counters.phase2_clusters, 1u);
		if (slot >= params.output_capacity) {
			atomicAdd(counters.overflow, 1u);
			break;
		}
		emitted_any = true;
		atomicAdd(counters.cluster_count, 1u);
		atomicAdd(draw_args.data[params.phase].y, 1u);
		clusters_out.data[params.list_base + slot] = uvec2(entry.x, global_cluster);
	}

	if (emitted_any && (cull.cull.w & 2u) != 0u && page_slot != 0xFFFFFFFFu) {
		uint word_idx = page_slot >> 5u;
		if (word_idx < page_requests.data[3]) {
			atomicOr(page_requests.data[4u + word_idx], 1u << (page_slot & 31u));
		}
	}
}
#endif
#else
#ifdef SHADOW_VIEW
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint view_idx = params.phase;
	uint node_cap = params.output_capacity;
	uint group_cap = params.list_base;
	uint input_count = params.level == 0u ? shadow_counters.data[view_idx].phase1_count : shadow_counters.data[view_idx].level_counts[params.level - 1u];
	input_count = min(input_count, node_cap);
	if (index >= input_count) {
		return;
	}
	NextGeoCullParamsData sc = shadow_culls.data[view_idx];
	uvec2 entry = level_input.data[view_idx * node_cap + index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.bvh_level_count == 0u || mesh.bvh_node_offset == 0u) {
		return;
	}
	const uint base = mesh.bvh_node_offset + entry.y * 80u;
	vec4 lod_sphere = vec4(dag_float(base), dag_float(base + 4u), dag_float(base + 8u), dag_float(base + 12u));
	float max_parent_error = dag_float(base + 16u);
	uint child_count = min(dag_uint(base + 44u), 8u);
	if (projected_error_params(record, lod_sphere, max_parent_error, sc.camera, sc.view, sc.view_proj, sc.cull.w) <= sc.camera.w) {
		return;
	}
	for (uint child = 0u; child < child_count; child++) {
		uint value = dag_uint(base + 48u + child * 4u);
		if ((value & NEXTGEO_BVH_LEAF_BIT) != 0u) {
			uint slot = atomicAdd(shadow_counters.data[view_idx].group_count, 1u);
			if (slot >= group_cap) {
				atomicAdd(shadow_counters.data[view_idx].overflow, 1u);
				continue;
			}
			groups_out.data[view_idx * group_cap + slot] = uvec2(entry.x, value & ~NEXTGEO_BVH_LEAF_BIT);
		} else {
			uint slot = atomicAdd(shadow_counters.data[view_idx].level_counts[params.level], 1u);
			if (slot >= node_cap) {
				atomicAdd(shadow_counters.data[view_idx].overflow, 1u);
				continue;
			}
			uint groups = (slot + 64u) / 64u;
			atomicMax(bvh_args.data[view_idx * 9u + 1u + min(params.level, 7u)].x, groups);
			level_output.data[view_idx * node_cap + slot] = uvec2(entry.x, value);
		}
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
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.bvh_level_count == 0u || mesh.bvh_node_offset == 0u) {
		return;
	}
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
#endif
