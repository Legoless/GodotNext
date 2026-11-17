#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_gpu_cull (#76). Level-synchronous BVH8 traversal and
// cluster compaction. Variants:
//   (none)       BVH_LEVEL: descend one level of the tree.
//   CLUSTER_CULL: test leaf groups and emit (record, cluster). With the hybrid
//                raster flag (#162), it also routes each emitted main-view
//                cluster to the software or hardware list of its raster bin.
//                With the programmable-bins flag (#155), a cluster whose
//                surface has a programmable raster bin is appended, with its
//                bin, for raster_bin_compact.glsl instead.
//   CUT_HASH:    XOR the order-independent hash of every compacted cluster.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
#include "nextgeo_cull_inc.glsl"
#include "raster_features_inc.glsl"
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

#if defined(CLUSTER_CULL) && !defined(SHADOW_VIEW)
// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N2, N3). Per phase and raster bin: the software
// dispatch and hardware draw arguments, the hardware draw counts, and the lists of visible-cluster
// indices (layouts in raster_features_inc.glsl).
layout(set = 0, binding = 16, std430) buffer restrict RasterArgs {
	uint data[];
}
raster_args;
layout(set = 0, binding = 17, std430) buffer restrict RasterCounts {
	uint data[];
}
raster_counts;
layout(set = 0, binding = 18, std430) buffer restrict writeonly RasterLists {
	uint data[];
}
raster_lists;

const uint NEXTGEO_CULL_FLAG_HYBRID_RASTER = 1u << 5u;

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3). Read only under the programmable-bins flag: the
// cluster table (for the cluster's surface), the per-(instance, surface) raster-bin table, and the
// append list and state of the programmable bins (layouts in raster_features_inc.glsl).
layout(set = 0, binding = 19, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;
layout(set = 0, binding = 20, std430) buffer restrict readonly RasterBinTable {
	uint data[];
}
raster_bin_table;
layout(set = 0, binding = 21, std430) buffer restrict writeonly RasterBinAppend {
	uint data[];
}
raster_bin_append;
layout(set = 0, binding = 22, std430) buffer restrict RasterBinState {
	uint data[];
}
raster_bin_state;

const uint NEXTGEO_CULL_FLAG_PROGRAMMABLE_BINS = 1u << 6u;
#endif

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
#ifndef SHADOW_VIEW
// The most the instance's basis stretches a length, RendererRD::nextgeo_sw_raster_instance_scale(): a
// Gershgorin bound on the Gram matrix of its columns, exact for a rotation times a scale, mirrored or
// not. world_aabb_min.w cannot serve: it is negative for a mirrored instance.
float sw_raster_instance_scale(NextGeoInstanceRecord p_record) {
	vec3 c0 = vec3(p_record.transform[0].x, p_record.transform[1].x, p_record.transform[2].x);
	vec3 c1 = vec3(p_record.transform[0].y, p_record.transform[1].y, p_record.transform[2].y);
	vec3 c2 = vec3(p_record.transform[0].z, p_record.transform[1].z, p_record.transform[2].z);
	float d01 = abs(dot(c0, c1));
	float d02 = abs(dot(c0, c2));
	float d12 = abs(dot(c1, c2));
	return sqrt(max(dot(c0, c0) + d01 + d02, max(dot(c1, c1) + d01 + d12, dot(c2, c2) + d02 + d12)));
}

// The triangle-size switch (N2), RendererRD::nextgeo_sw_raster_route(): software when the cluster's
// culling sphere lies in front of the near plane and its longest edge projects below T px.
// p_scale: sw_raster_instance_scale(p_record).
bool sw_raster_route(NextGeoInstanceRecord p_record, float p_scale, vec4 p_bounds, float p_max_edge) {
	float max_edge_px = cull.raster_params.x;
	if (!(max_edge_px > 0.0) || !(p_scale > 0.0) || !(p_max_edge >= 0.0) || isinf(p_max_edge)) {
		return false;
	}
	vec3 center = instance_point(p_record, p_bounds.xyz);
	float z_min = dot(center - cull.camera.xyz, cull.raster_forward.xyz) - p_bounds.w * p_scale;
	if (!(z_min > cull.raster_forward.w)) {
		return false;
	}
	float edge_px = p_max_edge * p_scale * cull.raster_params.y;
	if (cull.raster_params.z == 0.0) {
		edge_px /= max(z_min, 1e-20);
	}
	return edge_px < max_edge_px;
}

// GodotNext: nextgeo_raster_bins (#155). The raster bin of a cluster's surface: 0 (fixed-function) unless
// the surface has a programmable bin.
uint raster_bin_of(NextGeoInstanceRecord p_record, NextGeoMeshRecord p_mesh, uint p_global_cluster) {
	if (p_global_cluster < p_mesh.cluster_offset) {
		return NEXTGEO_RASTER_BIN_FIXED;
	}
	uint local_cluster = p_global_cluster - p_mesh.cluster_offset;
	if (local_cluster >= p_mesh.cluster_count) {
		return NEXTGEO_RASTER_BIN_FIXED;
	}
	uint surface_id = cluster_table.data[p_mesh.page_table_offset + local_cluster].w & 0xFFFFu;
	if (surface_id >= 32u) {
		return NEXTGEO_RASTER_BIN_FIXED;
	}
	uint word = raster_bin_table.data[nextgeo_raster_bin_table_word(p_record.bucket_base, surface_id)];
	uint bin = (word >> nextgeo_raster_bin_table_shift(surface_id)) & 0xFFu;
	return bin < NEXTGEO_RASTER_BIN_LIMIT ? bin : NEXTGEO_RASTER_BIN_FIXED;
}

// Appends visible cluster p_vis_index, with its programmable bin and path, for the per-bin compaction.
void raster_bin_route(uint p_vis_index, uint p_bin, bool p_software) {
	uint kind = p_software ? NEXTGEO_RASTER_BIN_KIND_SW : NEXTGEO_RASTER_BIN_KIND_HW;
	uint slot = atomicAdd(raster_bin_state.data[NEXTGEO_RASTER_BIN_STATE_APPENDED + params.phase], 1u);
	raster_bin_append.data[params.list_base + slot] = nextgeo_raster_bin_pack_entry(p_vis_index, p_bin, kind);
	atomicAdd(raster_bin_state.data[nextgeo_raster_bin_count_word(params.phase, p_bin, kind)], 1u);
	if (p_software) {
		atomicAdd(counters.raster_bin_sw[params.phase], 1u);
	} else {
		atomicAdd(counters.raster_bin_hw[params.phase], 1u);
	}
}

// Appends visible cluster p_vis_index to the software or hardware list of the fixed-function bin.
void raster_route(uint p_vis_index, bool p_software) {
	uint region_base = params.list_base;
	if (p_software) {
		uint slot = atomicAdd(counters.raster_sw[params.phase], 1u);
		raster_lists.data[region_base + slot] = p_vis_index;
		uint word = nextgeo_raster_args_word(params.phase, NEXTGEO_RASTER_BIN_FIXED, NEXTGEO_RASTER_ARGS_SW);
		atomicMax(raster_args.data[word + 0u], min(slot + 1u, NEXTGEO_SW_DISPATCH_WIDTH));
		atomicMax(raster_args.data[word + 1u], slot / NEXTGEO_SW_DISPATCH_WIDTH + 1u);
	} else {
		uint slot = atomicAdd(counters.raster_hw[params.phase], 1u);
		raster_lists.data[region_base + params.output_capacity - 1u - slot] = p_vis_index;
		uint word = nextgeo_raster_args_word(params.phase, NEXTGEO_RASTER_BIN_FIXED, NEXTGEO_RASTER_ARGS_HW);
		atomicAdd(raster_args.data[word + 1u], 1u);
		atomicMax(raster_counts.data[params.phase], NEXTGEO_RASTER_BIN_FIXED + 1u);
	}
}
#endif

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
	bool hybrid_raster = (cull.cull.w & NEXTGEO_CULL_FLAG_HYBRID_RASTER) != 0u;
	float route_scale = hybrid_raster ? sw_raster_instance_scale(record) : 0.0;

	for (uint cluster = 0u; cluster < cluster_count; cluster++) {
		uint global_cluster = cluster_offset + cluster;
		uint route_rec_word = 0xFFFFFFFFu;
		if (page_cluster_hdr > 0u && global_cluster >= mesh.cluster_offset) {
			uint local_cluster = global_cluster - mesh.cluster_offset;
			if (local_cluster >= page_first_cluster && (local_cluster - page_first_cluster) < page_cluster_hdr) {
				uint in_page = local_cluster - page_first_cluster;
				uint rec_word = ((page_slot * slot_page_size) >> 2u) + 4u + in_page * 20u;
				route_rec_word = rec_word;
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
		if (hybrid_raster) {
			bool software = false;
			if (route_rec_word != 0xFFFFFFFFu) {
				// The cluster record: culling sphere at words 0-3, max_edge (f16, .ngm v2) at word 16.
				vec4 bounds = vec4(
						uintBitsToFloat(pages.data[route_rec_word + 0u]),
						uintBitsToFloat(pages.data[route_rec_word + 1u]),
						uintBitsToFloat(pages.data[route_rec_word + 2u]),
						uintBitsToFloat(pages.data[route_rec_word + 3u]));
				float max_edge = unpackHalf2x16(pages.data[route_rec_word + 16u]).x;
				software = sw_raster_route(record, route_scale, bounds, max_edge);
			}
			uint bin = NEXTGEO_RASTER_BIN_FIXED;
			if ((cull.cull.w & NEXTGEO_CULL_FLAG_PROGRAMMABLE_BINS) != 0u) {
				bin = raster_bin_of(record, mesh, global_cluster);
			}
			if (bin == NEXTGEO_RASTER_BIN_FIXED) {
				raster_route(params.list_base + slot, software);
			} else {
				raster_bin_route(params.list_base + slot, bin, software);
			}
		}
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
