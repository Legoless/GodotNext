#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, steps 3 & 5).
//   (none)          Software rasterizer for small fixed-function NextGeo clusters into VSM physical
//                   pages: one 64-thread workgroup per (cluster, dirty page) pair, shared edge rules
//                   (sw_raster_setup_inc.glsl) over the 64x64 page rect, and 32-bit imageAtomicMax.
//                   Directional pages interpolate linear depth directly; perspective local faces
//                   interpolate 1/w (affine in screen space) and recover linear depth w = 1 / (1/w).
//   MODE_HIZ_BUILD  Builds the 1-level 8x8 max-depth Hi-Z mip per cached dirty page from static
//                   layer 0 after raster (dispatched indirectly from VSM_INDIRECT_ALL). Uncached
//                   pages clear their Hi-Z cells and owner entry to 0.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "vsm_mark_inc.glsl"
#include "vsm_raster_inc.glsl"
// clang-format on

#ifdef MODE_HIZ_BUILD

layout(set = 0, binding = 0, std430) buffer restrict readonly VSMAllocState {
	uint data[];
}
vsm_alloc_state;

layout(set = 0, binding = 1, std430) buffer restrict readonly VSMDirty {
	uvec4 data[];
}
vsm_dirty;

layout(r32ui, set = 0, binding = 2) uniform restrict readonly uimage2DArray vsm_pool;
layout(r32ui, set = 0, binding = 3) uniform restrict writeonly uimage2D vsm_hiz;

layout(set = 0, binding = 4, std430) buffer restrict VSMHizOwner {
	uint data[];
}
vsm_hiz_owner;

layout(set = 0, binding = 5, std430) buffer restrict VSMCounters {
	uint data[];
}
vsm_counters;

layout(push_constant, std430) uniform PushParams {
	uint pool_pages;
	uint grid_width;
	uint table_maps;
	uint pad0;
}
push_params;

const uint NEXTGEO_VSM_HIZ_BLOCK_SIZE = 8u;
const uint NEXTGEO_VSM_COUNTER_SINK = 21u;
const uint NEXTGEO_VSM_COUNTER_HIZ_PAGES_BUILT = 22u;

shared uint s_keep_hiz;

uint vsm_hiz_mip_entry_offset(uint mip) {
	return ((1u << (2u * VSM_MIP_COUNT)) - (1u << (2u * (VSM_MIP_COUNT - mip)))) / 3u;
}

uint vsm_hiz_entry_index(uint map, uint mip, uvec2 page) {
	return map * VSM_ENTRIES_PER_MAP + vsm_hiz_mip_entry_offset(mip) + page.y * (VSM_PAGES_PER_SIDE >> mip) + page.x;
}

void main() {
	uint d = gl_WorkGroupID.x;
	uint dirty_count = min(vsm_alloc_state.data[VSM_STATE_DIRTY], push_params.pool_pages);
	if (d >= dirty_count) {
		return;
	}
	uvec4 rec = vsm_dirty.data[d];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	uvec2 phys_page = vsm_raster_entry_page(rec.z);
	uint page_idx = phys_page.y * push_params.grid_width + phys_page.x;
	if (page_idx >= push_params.pool_pages || map >= push_params.table_maps || mip >= VSM_MIP_COUNT) {
		return;
	}

	uint cell_x = gl_LocalInvocationIndex & 7u;
	uint cell_y = gl_LocalInvocationIndex >> 3u;
	ivec2 hiz_coord = ivec2(phys_page * 8u) + ivec2(int(cell_x), int(cell_y));

	if ((rec.w & VSM_ENTRY_UNCACHED) != 0u) {
		imageStore(vsm_hiz, hiz_coord, uvec4(0u));
		if (gl_LocalInvocationIndex == 0u) {
			vsm_hiz_owner.data[page_idx] = 0u;
		}
		return;
	}

	uint owner = vsm_hiz_entry_index(map, mip, slot) + 1u;
	// Evaluate once on thread 0 before the barrier so warps never race against thread 0's write below.
	if (gl_LocalInvocationIndex == 0u) {
		s_keep_hiz = ((rec.w & VSM_ENTRY_DIRTY_STATIC) == 0u && vsm_hiz_owner.data[page_idx] == owner) ? 1u : 0u;
	}
	barrier();
	// Static layer did not change and this page already holds its Hi-Z: keep it without re-reading.
	if (s_keep_hiz != 0u) {
		return;
	}

	ivec2 block_base = ivec2(phys_page * VSM_PAGE_SIZE) + ivec2(int(cell_x * NEXTGEO_VSM_HIZ_BLOCK_SIZE), int(cell_y * NEXTGEO_VSM_HIZ_BLOCK_SIZE));
	uint min_packed = 0xFFFFFFFFu;
	for (int dy = 0; dy < 8; dy++) {
		for (int dx = 0; dx < 8; dx++) {
			uint texel = imageLoad(vsm_pool, ivec3(block_base + ivec2(dx, dy), 0)).r;
			min_packed = min(min_packed, texel);
		}
	}
	if (min_packed == 0xFFFFFFFFu) {
		min_packed = 0u;
	}
	imageStore(vsm_hiz, hiz_coord, uvec4(min_packed, 0u, 0u, 0u));
	if (gl_LocalInvocationIndex == 0u) {
		vsm_hiz_owner.data[page_idx] = owner;
		// #277: every atomic's result is used.
		if (atomicAdd(vsm_counters.data[NEXTGEO_VSM_COUNTER_HIZ_PAGES_BUILT], 1u) == 0xFFFFFFFFu) {
			vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
		}
	}
}

#elif defined(MODE_MARK_DYNAMIC_TEST)

// GodotNext: vsm_nanite (#217). Debug/test hook (off by default until #212 lands): marks
// VSM_ENTRY_DIRTY_DYNAMIC on cached physical pages with valid static Hi-Z so dynamic-layer routing
// and next-frame Hi-Z culling can be verified end-to-end on the GPU.
layout(set = 0, binding = 0, std430) buffer restrict VSMPageTable {
	uint entries[];
}
vsm_page_table;

layout(set = 0, binding = 1, std430) buffer restrict readonly VSMHizOwner {
	uint data[];
}
vsm_hiz_owner;

layout(set = 0, binding = 2, std430) buffer restrict VSMCounters {
	uint data[];
}
vsm_counters;

layout(push_constant, std430) uniform PushParams {
	uint pool_pages;
	uint table_maps;
	uint pad0;
	uint pad1;
}
push_params;

const uint NEXTGEO_VSM_COUNTER_SINK = 21u;

void main() {
	uint p = gl_GlobalInvocationID.x;
	if (p >= push_params.pool_pages) {
		return;
	}
	uint owner = vsm_hiz_owner.data[p];
	if (owner == 0u) {
		return;
	}
	uint entry_idx = owner - 1u;
	if (entry_idx >= push_params.table_maps * VSM_ENTRIES_PER_MAP) {
		return;
	}
	uint entry = vsm_page_table.entries[entry_idx];
	if ((entry & (VSM_ENTRY_ALLOCATED | VSM_ENTRY_UNCACHED)) == VSM_ENTRY_ALLOCATED) {
		// #277: every atomic's result is used.
		if (atomicOr(vsm_page_table.entries[entry_idx], VSM_ENTRY_DIRTY_DYNAMIC) == 0xFFFFFFFFu) {
			vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
		}
	}
}

#else

// clang-format off
#include "spline_deform_inc.glsl"
#include "nextgeo/nextgeo_records_inc.glsl"
#include "nextgeo/raster_features_inc.glsl"
#include "nextgeo/sw_raster_setup_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 1, std430) buffer restrict readonly Meshes {
	NextGeoMeshRecord data[];
}
meshes;

layout(set = 0, binding = 2, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;

layout(set = 0, binding = 3, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;

layout(set = 0, binding = 4, std430) buffer restrict readonly VSMClusters {
	uvec4 data[];
}
vsm_clusters;

layout(set = 0, binding = 5, std430) buffer restrict readonly VSMRasterLists {
	uint data[];
}
vsm_raster_lists;

layout(set = 0, binding = 6, std430) buffer restrict readonly VSMMaps {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 7, std430) buffer restrict readonly VSMDirty {
	uvec4 data[];
}
vsm_dirty;

layout(r32ui, set = 0, binding = 8) uniform restrict uimage2DArray vsm_pool;

layout(set = 0, binding = 9, std430) buffer restrict VSMCounters {
	uint data[];
}
vsm_counters;

// GodotNext: spline_meshes (#161).
layout(set = 0, binding = 10, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;

// GodotNext: nextgeo_skinned_runtime (#160). Skeleton palette arena, per-record skin table, cluster skin offsets.
layout(set = 0, binding = 11, std430) buffer restrict readonly NextGeoSkinPalette {
	vec4 data[];
}
nextgeo_skin_palette;

layout(set = 0, binding = 12, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

layout(set = 0, binding = 13, std430) buffer restrict readonly NextGeoClusterSkin {
	uint data[];
}
nextgeo_cluster_skin;

// clang-format off
#include "nextgeo/skinning_inc.glsl"
// clang-format on

layout(push_constant, std430) uniform PushParams {
	uint cluster_capacity;
	uint flags; // bit 0 = count SW writes.
	uint pad0;
	uint pad1;
}
push_params;

const uint NEXTGEO_VSM_COUNTER_SW_CLUSTERS = 12u;
const uint NEXTGEO_VSM_COUNTER_SW_WRITES = 20u;
const uint NEXTGEO_VSM_COUNTER_SINK = 21u;

// Projected vertices: xy = page pixel [0, VSM_PAGE_SIZE], z = linear depth (directional) or 1/w
// (perspective local face), w = 1 when valid.
shared vec4 s_vertices[256];
shared uint s_writes;

void main() {
	uint local_idx = gl_WorkGroupID.y * NEXTGEO_SW_DISPATCH_WIDTH + gl_WorkGroupID.x;
	// Workgroup-uniform early exits.
	if (local_idx >= vsm_counters.data[NEXTGEO_VSM_COUNTER_SW_CLUSTERS] || local_idx >= push_params.cluster_capacity) {
		return;
	}
	uint pair_idx = vsm_raster_lists.data[local_idx];
	if (pair_idx >= push_params.cluster_capacity) {
		return;
	}

	uvec4 pair = vsm_clusters.data[pair_idx];
	NextGeoInstanceRecord record = instances.data[pair.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || (record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF) {
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || pair.y < mesh.cluster_offset) {
		return;
	}
	uint local_cluster = pair.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		return;
	}

	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = desc.z & 0xFFu;
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	uint surface_id = desc.w & 0xFFFFu;
	if (stride < 3u || vert_count == 0u || surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		return;
	}

	uvec4 rec = vsm_dirty.data[pair.z];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	uvec2 phys_page = vsm_raster_entry_page(rec.z);
	ivec2 phys_origin = ivec2(phys_page * VSM_PAGE_SIZE);
	int layer = int(pair.w & 1u);
	VSMMapData md = vsm_maps.data[map];
	bool is_directional = md.kind == VSM_MAP_KIND_DIRECTIONAL;

	bool cull_disabled = ((record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_DOUBLE_SIDED) || ((record.pad1[0] & (1u << surface_id)) != 0u);
	int facing = NEXTGEO_SW_KEEP_BOTH;
	if (!cull_disabled) {
		bool cull_front = (record.pad1[1] & (1u << surface_id)) != 0u;
		float map_cull_sign = ((pair.w & 2u) != 0u) ? 1.0 : -1.0;
		float effective_cull_sign = cull_front ? -map_cull_sign : map_cull_sign;
		mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
		facing = (determinant(basis) * effective_cull_sign < 0.0) ? NEXTGEO_SW_KEEP_NEGATIVE : NEXTGEO_SW_KEEP_POSITIVE;
	}

	if (gl_LocalInvocationIndex == 0u) {
		s_writes = 0u;
	}
	bool has_spline = (record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u;
	SplineDeformPose spline_pose;
	if (has_spline) {
		spline_pose = spline_deforms.data[record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current;
	}
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	uint cluster_table_idx = mesh.page_table_offset + local_cluster;
	for (uint v = gl_LocalInvocationIndex; v < vert_count; v += 64u) {
		uint pos_word = desc.x + v * stride;
		vec3 local_pos = vec3(
				uintBitsToFloat(pages.data[pos_word + 0u]),
				uintBitsToFloat(pages.data[pos_word + 1u]),
				uintBitsToFloat(pages.data[pos_word + 2u]));
		if (is_skinned) {
			local_pos = nextgeo_skin_position(local_pos, pair.x, cluster_table_idx, v, vert_count, false);
		}
		if (has_spline) {
			local_pos = spline_deform_position(spline_pose, local_pos);
		}
		vec3 world_pos = vec3(
				dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
				dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
				dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
		float depth = 0.0;
		vec4 clip = vsm_page_clip_position(md, mip, slot, world_pos, depth);
		if (is_directional) {
			vec2 px = (clip.xy * 0.5 + 0.5) * float(VSM_PAGE_SIZE);
			s_vertices[v] = vec4(px, depth, 1.0);
		} else if (clip.w > 0.0) {
			float inv_w = 1.0 / clip.w;
			vec2 px = (clip.xy * inv_w * 0.5 + 0.5) * float(VSM_PAGE_SIZE);
			s_vertices[v] = vec4(px, inv_w, 1.0);
		} else {
			s_vertices[v] = vec4(0.0);
		}
	}
	barrier();

	uint writes = 0u;
	ivec2 page_size = ivec2(int(VSM_PAGE_SIZE));
	for (uint t = gl_LocalInvocationIndex; t < tri_count; t += 64u) {
		uint ib = desc.y + t * 3u;
		uint i0 = (pages.data[ib >> 2u] >> ((ib & 3u) * 8u)) & 0xFFu;
		uint i1 = (pages.data[(ib + 1u) >> 2u] >> (((ib + 1u) & 3u) * 8u)) & 0xFFu;
		uint i2 = (pages.data[(ib + 2u) >> 2u] >> (((ib + 2u) & 3u) * 8u)) & 0xFFu;
		if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count) {
			continue;
		}
		vec4 a = s_vertices[i0];
		vec4 b = s_vertices[i1];
		vec4 c = s_vertices[i2];
		if (a.w == 0.0 || b.w == 0.0 || c.w == 0.0) {
			continue;
		}
		NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(a.xy, b.xy, c.xy, vec3(a.z, b.z, c.z), page_size, facing);
		if (!tri.valid) {
			continue;
		}
		for (int y = tri.rect_min.y; y <= tri.rect_max.y; y++) {
			for (int x = tri.rect_min.x; x <= tri.rect_max.x; x++) {
				float z_interp = 0.0;
				if (!nextgeo_sw_sample(tri, ivec2(x, y), z_interp)) {
					continue;
				}
				float depth = z_interp;
				if (!is_directional) {
					if (!(z_interp > 0.0)) {
						continue;
					}
					depth = 1.0 / z_interp;
					if (!(depth >= md.near_w && depth <= md.max_distance)) {
						continue;
					}
				}
				uint packed_depth = vsm_depth_pack(depth);
				if (packed_depth != 0u) {
					// #277: every atomic's result is used.
					if (imageAtomicMax(vsm_pool, ivec3(phys_origin + ivec2(x, y), layer), packed_depth) == 0xFFFFFFFFu) {
						vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
					}
					writes++;
				}
			}
		}
	}

	if ((push_params.flags & 1u) != 0u) {
		if (writes > 0u) {
			if (atomicAdd(s_writes, writes) == 0xFFFFFFFFu) {
				vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
			}
		}
		barrier();
		if (gl_LocalInvocationIndex == 0u && s_writes > 0u) {
			if (atomicAdd(vsm_counters.data[NEXTGEO_VSM_COUNTER_SW_WRITES], s_writes) == 0xFFFFFFFFu) {
				vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
			}
		}
	}
}

#endif
