#[vertex]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, step 4). Hardware rasterizer for fixed-function
// NextGeo clusters into VSM physical pages: vertex pulling from virtual mesh storage, projection into
// the target page's 64x64 rect (vsm_page_clip_position), no depth attachment, and 32-bit
// imageAtomicMax(vsm_pool, physical_texel, depth_uint). Drawn via draw_list_draw_indirect_count (#177)
// with firstInstance = 0 so every D3D12 shader model (including below SM 6.8) reads gl_InstanceIndex
// directly.

// clang-format off
#include "spline_deform_inc.glsl"
#include "vsm_mark_inc.glsl"
#include "vsm_raster_inc.glsl"
#include "nextgeo/nextgeo_records_inc.glsl"
// clang-format on

layout(location = 0) out flat uint vsm_page_info_out;
layout(location = 1) out float vsm_depth_out;

layout(push_constant, std430) uniform PushParams {
	uint cluster_capacity;
	uint pad0;
	uint pad1;
	uint pad2;
}
push_params;

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

// Per (cluster, dirty page) pair: x = record_idx, y = global_cluster, z = dirty_idx, w = flags
// (bit 0 = dynamic layer, bit 1 = reverse_cull).
layout(set = 0, binding = 4, std430) buffer restrict readonly VSMClusters {
	uvec4 data[];
}
vsm_clusters;

// Hardware pair indices grow down from cluster_capacity - 1 (shared convention with #162).
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

// GodotNext: nextgeo_foliage_wind (#152). Bindings 14..15 hold per-scenario wind params and per-mesh wind skeletons.
layout(set = 0, binding = 14, std430) buffer restrict readonly NextGeoWindParams {
	vec4 direction_strength;
	vec4 time_gust_flags;
	uvec4 meta;
}
nextgeo_wind_params;

layout(set = 0, binding = 15, std430) buffer restrict readonly NextGeoWindMeshes {
	NextGeoWindMesh data[];
}
nextgeo_wind_meshes;

#define NEXTGEO_HAS_WIND_BUFFERS
// clang-format off
#include "nextgeo/skinning_inc.glsl"
// clang-format on

void collapse_triangle() {
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
	vsm_page_info_out = 0xFFFFFFFFu;
	vsm_depth_out = 0.0;
}

void main() {
	uint inst = uint(gl_InstanceIndex);
	if (inst >= push_params.cluster_capacity) {
		collapse_triangle();
		return;
	}
	uint pair_idx = vsm_raster_lists.data[push_params.cluster_capacity - 1u - inst];
	if (pair_idx >= push_params.cluster_capacity) {
		collapse_triangle();
		return;
	}
	uvec4 pair = vsm_clusters.data[pair_idx];
	NextGeoInstanceRecord record = instances.data[pair.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || (record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF) {
		collapse_triangle();
		return;
	}

	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || pair.y < mesh.cluster_offset) {
		collapse_triangle();
		return;
	}
	uint local_cluster = pair.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		collapse_triangle();
		return;
	}

	uint t = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) - t * 3u;
	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = desc.z & 0xFFu;
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	uint surface_id = desc.w & 0xFFFFu;
	if (t >= tri_count || stride < 3u || vert_count == 0u || surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		collapse_triangle();
		return;
	}

	uvec4 rec = vsm_dirty.data[pair.z];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	VSMMapData md = vsm_maps.data[map];

	bool cull_disabled = ((record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_DOUBLE_SIDED) || ((record.pad1[0] & (1u << surface_id)) != 0u);
	bool cull_front = (record.pad1[1] & (1u << surface_id)) != 0u;
	// vsm_page_clip_position has no Y flip, matching _vsm_raster_non_nanite's flip_cull = !reverse_cull.
	float map_cull_sign = ((pair.w & 2u) != 0u) ? 1.0 : -1.0;

	bool has_spline = (record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u;
	uint spline_idx = record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT;
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	bool is_wind = !is_skinned && nextgeo_wind_mesh_active(record.virtual_mesh);
	uint cluster_table_idx = mesh.page_table_offset + local_cluster;
	if (cull_disabled) {
		vec4 tri_clip[3];
		for (uint c = 0u; c < 3u; c++) {
			uint ib = desc.y + t * 3u + c;
			uint vi = (pages.data[ib >> 2u] >> ((ib & 3u) * 8u)) & 0xFFu;
			if (vi >= vert_count) {
				collapse_triangle();
				return;
			}
			uint pw = desc.x + vi * stride;
			vec3 lp = vec3(
					uintBitsToFloat(pages.data[pw + 0u]),
					uintBitsToFloat(pages.data[pw + 1u]),
					uintBitsToFloat(pages.data[pw + 2u]));
			if (is_skinned) {
				lp = nextgeo_skin_position(lp, pair.x, cluster_table_idx, vi, vert_count, false);
			} else if (is_wind) {
				lp = nextgeo_wind_position(lp, record.virtual_mesh, record.transform, cluster_table_idx, vi, vert_count, false);
			}
			if (has_spline) {
				lp = spline_deform_position(spline_deforms.data[spline_idx].current, lp);
			}
			vec3 wp = vec3(
					dot(record.transform[0].xyz, lp) + record.transform[0].w,
					dot(record.transform[1].xyz, lp) + record.transform[1].w,
					dot(record.transform[2].xyz, lp) + record.transform[2].w);
			float dummy_depth = 0.0;
			tri_clip[c] = vsm_page_clip_position(md, mip, slot, wp, dummy_depth);
		}
		if (determinant(mat3(tri_clip[0].xyw, tri_clip[1].xyw, tri_clip[2].xyw)) < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	} else {
		float effective_cull_sign = cull_front ? -map_cull_sign : map_cull_sign;
		mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
		if (determinant(basis) * effective_cull_sign < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	}

	uint idx_byte = desc.y + t * 3u + corner;
	uint v_idx = (pages.data[idx_byte >> 2u] >> ((idx_byte & 3u) * 8u)) & 0xFFu;
	if (v_idx >= vert_count) {
		collapse_triangle();
		return;
	}
	uint pos_word = desc.x + v_idx * stride;
	vec3 local_pos = vec3(
			uintBitsToFloat(pages.data[pos_word + 0u]),
			uintBitsToFloat(pages.data[pos_word + 1u]),
			uintBitsToFloat(pages.data[pos_word + 2u]));
	if (is_skinned) {
		local_pos = nextgeo_skin_position(local_pos, pair.x, cluster_table_idx, v_idx, vert_count, false);
	} else if (is_wind) {
		local_pos = nextgeo_wind_position(local_pos, record.virtual_mesh, record.transform, cluster_table_idx, v_idx, vert_count, false);
	}
	if (has_spline) {
		local_pos = spline_deform_position(spline_deforms.data[spline_idx].current, local_pos);
	}
	vec3 world_pos = vec3(
			dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
			dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
			dot(record.transform[2].xyz, local_pos) + record.transform[2].w);

	float vsm_depth = 0.0;
	gl_Position = vsm_page_clip_position(md, mip, slot, world_pos, vsm_depth);
	vsm_page_info_out = (rec.z & 0x00FFFFFFu) | ((pair.w & 1u) << 24u);
	vsm_depth_out = vsm_depth;
}

#[fragment]

#version 450

#VERSION_DEFINES

// clang-format off
#include "vsm_mark_inc.glsl"
#include "vsm_raster_inc.glsl"
// clang-format on

layout(location = 0) in flat uint vsm_page_info_in;
layout(location = 1) in float vsm_depth_in;

layout(r32ui, set = 0, binding = 8) uniform restrict uimage2DArray vsm_pool;

// NextGeoVSMCounters as words (word 21 = #277 atomic sink).
layout(set = 0, binding = 9, std430) buffer restrict VSMCounters {
	uint data[];
}
vsm_counters;

const uint NEXTGEO_VSM_COUNTER_SINK = 21u;

void main() {
	if (gl_HelperInvocation || vsm_page_info_in == 0xFFFFFFFFu) {
		return;
	}
	ivec2 local_px = ivec2(gl_FragCoord.xy);
	if (any(lessThan(local_px, ivec2(0))) || any(greaterThanEqual(local_px, ivec2(int(VSM_PAGE_SIZE))))) {
		return;
	}
	uint packed_depth = vsm_depth_pack(vsm_depth_in);
	if (packed_depth != 0u) {
		uvec2 phys_page = vsm_raster_entry_page(vsm_page_info_in);
		int layer = int((vsm_page_info_in >> 24u) & 1u);
		ivec3 pool_coord = ivec3(ivec2(phys_page * VSM_PAGE_SIZE) + local_px, layer);
		// #277: every atomic's result is used so D3D12 GPU-based validation keeps the write.
		if (imageAtomicMax(vsm_pool, pool_coord, packed_depth) == 0xFFFFFFFFu) {
			vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
		}
	}
}
