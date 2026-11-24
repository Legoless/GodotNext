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

// Shared includes before #ifdef VSM_VIEW so glsl_builders.py's include-once expansion makes them
// visible to both VSM_VIEW and main/shadow variants.
// clang-format off
#include "../spline_deform_inc.glsl"
#include "nextgeo_records_inc.glsl"
#include "raster_features_inc.glsl"
#include "voxel_inc.glsl"
// clang-format on

#if !defined(CUT_HASH) && !defined(VSM_SETUP)
// GodotNext: nextgeo_foliage_wind (#152). Bindings 27..28 hold wind params and per-mesh wind skeletons (29..30 reserved).
layout(set = 0, binding = 27, std430) buffer restrict readonly NextGeoWindParams {
	vec4 direction_strength;
	vec4 time_gust_flags;
	uvec4 meta;
}
nextgeo_wind_params;

layout(set = 0, binding = 28, std430) buffer restrict readonly NextGeoWindMeshes {
	NextGeoWindMesh data[];
}
nextgeo_wind_meshes;

#define NEXTGEO_HAS_WIND_BUFFERS
#define NEXTGEO_WIND_HELPERS_DEFINED
bool nextgeo_wind_enabled() {
	return (floatBitsToUint(nextgeo_wind_params.time_gust_flags.w) & NEXTGEO_WIND_PARAM_FLAG_ENABLED) != 0u;
}

float nextgeo_wind_mesh_max_sway(uint p_mesh_idx) {
	if (!nextgeo_wind_enabled() || p_mesh_idx >= nextgeo_wind_params.meta.x) {
		return 0.0;
	}
	uvec4 hdr = nextgeo_wind_meshes.data[p_mesh_idx].header;
	return hdr.x > 0u ? uintBitsToFloat(hdr.y) : 0.0;
}

bool nextgeo_wind_mesh_active(uint p_mesh_idx) {
	if (!nextgeo_wind_enabled() || !(nextgeo_wind_params.direction_strength.w > 0.0) || p_mesh_idx >= nextgeo_wind_params.meta.x) {
		return false;
	}
	return nextgeo_wind_meshes.data[p_mesh_idx].header.x > 0u;
}
#elif defined(VSM_SETUP)
float nextgeo_wind_mesh_max_sway(uint p_mesh_idx) {
	return 0.0;
}
#endif

#ifdef CLUSTER_CULL
// GodotNext: nextgeo_skinned_runtime (#160). Shared across VSM_VIEW and main/shadow CLUSTER_CULL
// before #ifdef VSM_VIEW so glsl_builders.py's include-once expansion exposes skinning_inc.glsl
// to both branches.
layout(set = 0, binding = 14, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;

layout(set = 0, binding = 23, std430) buffer restrict readonly NextGeoSkinPalette {
	vec4 data[];
}
nextgeo_skin_palette;

layout(set = 0, binding = 24, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

layout(set = 0, binding = 25, std430) buffer restrict readonly NextGeoClusterSkin {
	uint data[];
}
nextgeo_cluster_skin;

// clang-format off
#include "skinning_inc.glsl"
// clang-format on
#endif

#ifdef VSM_VIEW
// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, steps 1-3, 5-7). Per-page multi-view cluster
// cull for Virtual Shadow Maps. One indirect dispatch chain handles all directional dirty pages, a
// second handles all local dirty pages.
// Variants (with VSM_VIEW defined):
//   VSM_SETUP:    1 thread. Builds VSM_SEED indirect dispatch args from vsm_alloc_state dirty counts.
//   VSM_SEED:     Tests (instance, dirty_page) pairs against map cull mask, layer dirty bit, page
//                 rect and previous-frame static Hi-Z (cached pages only); seeds BVH root nodes.
//   (none)        VSM_BVH: level-synchronous BVH8 traversal with page-mip error (<= 1 shadow texel),
//                 per-node AABB vs page rect and cached-page Hi-Z test.
//   CLUSTER_CULL: Tests leaf groups and clusters against page-mip error, page rect and cached-page
//                 Hi-Z, routes each surviving (cluster, page) pair to HW or SW (and programmable
//                 bins shared with #155) with indirect dispatch/draw arguments.

const uint NEXTGEO_BVH_LEAF_BIT = 0x80000000u;
const uint NEXTGEO_VSM_HIZ_BLOCK_SHIFT = 3u;
const uint NEXTGEO_VSM_HIZ_PAGE_SIZE = 8u;
const uint NEXTGEO_VSM_CULL_FLAG_HIZ = 1u << 0u;
const uint NEXTGEO_VSM_CULL_FLAG_PROGRAMMABLE_BINS = 1u << 1u;
const uint NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK = 1u << 2u;
const uint NEXTGEO_VSM_CULL_FLAG_TESSELLATION = 1u << 4u; // #163: dice displaced clusters on this chain
const uint NEXTGEO_VSM_CULL_FLAG_VOXELS = 1u << 5u; // #165: swap coarse voxelized groups for sparse 4^3 bricks
const uint NEXTGEO_VSM_MAP_META_REVERSE_CULL = 1u << 0u;
const uint NEXTGEO_VSM_MAP_META_ACTIVE = 1u << 1u;

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

layout(set = 0, binding = 3, std430) buffer restrict readonly VSMParams {
	uint table_maps;
	uint pool_pages;
	uint grid_width;
	uint record_count;
	uint dirty_begin;
	uint dirty_end;
	uint node_capacity;
	uint group_capacity;
	uint cluster_capacity;
	uint cull_flags;
	uint chain_index;
	uint bucket_table_words;
	float max_texel_error;
	float lod_bias;
	float sw_max_edge_texels;
	float dicing_rate; // #163: shadow texels per diced edge
	vec4 camera_position;
	uint bin_features[68];
}
vsm_params;

layout(set = 0, binding = 4, std430) buffer restrict readonly Residency {
	uint data[];
}
residency;

layout(set = 0, binding = 5, std430) buffer restrict VSMCounters {
	uint dirty_pages;
	uint seeded_pairs;
	uint bvh_level_counts[8];
	uint group_count;
	uint cluster_count;
	uint sw_clusters;
	uint hw_clusters;
	uint prog_sw_clusters;
	uint prog_hw_clusters;
	uint hiz_culled_nodes;
	uint hiz_culled_clusters;
	uint overflow;
	uint redraw_marks;
	uint sw_writes;
	uint sink;
	uint hiz_pages_built;
	// GodotNext: vsm_caching (#212).
	uint static_clusters;
	uint dynamic_clusters;
	uint always_layer0_clusters;
	uint distance_fixed_clusters; // #153: pixel-programmable clusters demoted to FIXED past programmable_raster_distance
	uint vertex_offset_distance_fixed; // #164: WPO clusters demoted to FIXED past vertex_offset_disable_distance
	uint vertex_offset_clamped; // #164: vertices clamped to Material.vertex_offset_max
	uint voxel_bricks; // #165: visible VSM (voxel brick, page) pairs
	uint voxel_writes; // #165: VSM voxel SW texel writes
	uint pad1;
}
vsm_counters;

layout(set = 0, binding = 10, std430) buffer restrict VSMPageTable {
	uint entries[];
}
vsm_page_table;

// clang-format off
#include "../vsm_mark_inc.glsl"
#include "../vsm_page_table_inc.glsl"
#include "../vsm_raster_inc.glsl"
// clang-format on

layout(set = 0, binding = 6, std430) buffer restrict readonly VSMMaps {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 7, std430) buffer restrict readonly VSMMapMeta {
	uvec4 data[]; // x = cull_mask, y = flags (bit 0 reverse_cull, bit 1 active).
}
vsm_map_meta;

layout(set = 0, binding = 8, std430) buffer restrict readonly VSMDirty {
	uvec4 data[];
}
vsm_dirty;

layout(set = 0, binding = 9, std430) buffer restrict readonly VSMAllocState {
	uint data[];
}
vsm_alloc_state;

layout(r32ui, set = 0, binding = 11) uniform restrict readonly uimage2D vsm_hiz;

layout(set = 0, binding = 12, std430) buffer restrict readonly VSMHizOwner {
	uint data[];
}
vsm_hiz_owner;

layout(set = 0, binding = 13, std430) buffer restrict readonly VSMNodesIn {
	uvec4 data[];
}
vsm_nodes_in;

#ifdef CLUSTER_CULL
layout(set = 0, binding = 15, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;

layout(set = 0, binding = 16, std430) buffer restrict writeonly VSMClustersOut {
	uvec4 data[];
}
vsm_clusters_out;

// [0..3]: fixed-function SW dispatch (x, y, 1, 0); [4..7]: fixed-function HW draw (384, count, 0, 0).
layout(set = 0, binding = 17, std430) buffer restrict VSMRasterArgs {
	uint data[];
}
vsm_raster_args;

layout(set = 0, binding = 18, std430) buffer restrict VSMRasterCounts {
	uint data[];
}
vsm_raster_counts;

layout(set = 0, binding = 19, std430) buffer restrict writeonly VSMRasterLists {
	uint data[];
}
vsm_raster_lists;

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

// GodotNext: nextgeo_voxels (#165). Bindings 29..30 avoid 27..28 reserved for #152.
layout(set = 0, binding = 29, std430) buffer restrict writeonly VSMVoxelBricksOut {
	uvec4 data[];
}
vsm_voxel_bricks_out;

// [0..3]: VSM voxel compute indirect dispatch args (x, y, 1, 0).
layout(set = 0, binding = 30, std430) buffer restrict VSMVoxelArgs {
	uint data[];
}
vsm_voxel_args;
#else
layout(set = 0, binding = 14, std430) buffer restrict writeonly VSMNodesOut {
	uvec4 data[];
}
vsm_nodes_out;

layout(set = 0, binding = 15, std430) buffer restrict writeonly VSMGroupsOut {
	uvec4 data[];
}
vsm_groups_out;

layout(set = 0, binding = 16, std430) buffer restrict VSMArgsOut {
	uvec4 data[];
}
vsm_args_out;

layout(set = 0, binding = 17, std430) buffer restrict VSMGroupArgsOut {
	uvec4 data[];
}
vsm_group_args_out;

#if !defined(VSM_SETUP) && !defined(VSM_SEED)
layout(set = 0, binding = 24, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

float vsm_skin_instance_max_scale(uint p_record_idx) {
	uvec4 sinfo = nextgeo_skin_instances.data[p_record_idx];
	if (sinfo.z == 0u) {
		return 1.0;
	}
	float s = uintBitsToFloat(sinfo.w);
	return s > 0.0 ? s : 1.0;
}
#endif
#endif

// GodotNext: spline_meshes (#161). Bound in VSM_BVH and VSM CLUSTER_CULL (binding 26, after 23..25 skinning).
#if !defined(VSM_SETUP) && !defined(VSM_SEED)
layout(set = 0, binding = 26, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;
#endif

layout(push_constant, std430) uniform VSMCullPush {
	uint level;
	uint pad0;
	uint pad1;
	uint pad2;
}
vsm_push;

// #277: D3D12 GPU-based validation requires every atomic's return value to be used.
void vsm_ng_sink(uint value) {
	if (value == 0xFFFFFFFFu) {
		vsm_counters.sink = 1u;
	}
}

void vsm_ng_mark_overflow(uvec4 rec) {
	vsm_ng_sink(atomicAdd(vsm_counters.overflow, 1u));
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	if (map >= vsm_params.table_maps || mip >= VSM_MIP_COUNT) {
		return;
	}
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	vsm_ng_sink(atomicOr(vsm_page_table.entries[vsm_entry_index(map, mip, slot)], VSM_ENTRY_SCROLLED));
	vsm_ng_sink(atomicAdd(vsm_counters.redraw_marks, 1u));
}

vec3 vsm_instance_point(NextGeoInstanceRecord p_record, vec3 p_local) {
	return vec3(
			dot(p_record.transform[0].xyz, p_local) + p_record.transform[0].w,
			dot(p_record.transform[1].xyz, p_local) + p_record.transform[1].w,
			dot(p_record.transform[2].xyz, p_local) + p_record.transform[2].w);
}

float vsm_instance_scale(NextGeoInstanceRecord p_record) {
	vec3 c0 = vec3(p_record.transform[0].x, p_record.transform[1].x, p_record.transform[2].x);
	vec3 c1 = vec3(p_record.transform[0].y, p_record.transform[1].y, p_record.transform[2].y);
	vec3 c2 = vec3(p_record.transform[0].z, p_record.transform[1].z, p_record.transform[2].z);
	float d01 = abs(dot(c0, c1));
	float d02 = abs(dot(c0, c2));
	float d12 = abs(dot(c1, c2));
	return sqrt(max(dot(c0, c0) + d01 + d02, max(dot(c1, c1) + d01 + d12, dot(c2, c2) + d02 + d12)));
}

bool vsm_record_moved(NextGeoInstanceRecord p_record) {
	return any(notEqual(p_record.transform[0], p_record.prev_transform[0])) ||
			any(notEqual(p_record.transform[1], p_record.prev_transform[1])) ||
			any(notEqual(p_record.transform[2], p_record.prev_transform[2]));
}

uint vsm_instance_layer(NextGeoInstanceRecord p_record, uint p_cull_flags, bool p_dynamic_material) {
	if ((p_cull_flags & NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK) == 0u) {
		return 0u;
	}
	// GodotNext: vsm_caching (#212, plan §21.3 contract S5).
	uint mode = p_record.flags & NEXTGEO_RECORD_VSM_CACHE_MODE_MASK;
	if (mode == NEXTGEO_RECORD_VSM_CACHE_ALWAYS) {
		return 1u;
	}
	if (mode == NEXTGEO_RECORD_VSM_CACHE_STATIC || mode == NEXTGEO_RECORD_VSM_CACHE_RIGID) {
		return 0u;
	}
	bool is_dynamic = ((p_record.flags & (NEXTGEO_RECORD_VSM_DYNAMIC | NEXTGEO_RECORD_RT_SKINNED | NEXTGEO_RECORD_HAS_SKELETON | NEXTGEO_RECORD_SKINNED_PALETTE)) != 0u) || (nextgeo_wind_mesh_max_sway(p_record.virtual_mesh) > 0.0) || vsm_record_moved(p_record) || p_dynamic_material;
	return is_dynamic ? 1u : 0u;
}

vec3 vsm_light_origin(VSMMapData md) {
	float len0 = length(md.row0.xyz);
	float inv_cot = len0 > 1.0e-20 ? (1.0 / len0) : 0.0;
	vec3 x_axis = md.row0.xyz * inv_cot;
	vec3 y_axis = md.row1.xyz * inv_cot;
	vec3 z_axis = -md.row3.xyz;
	vec3 t = vec3(md.row0.w * inv_cot, md.row1.w * inv_cot, -md.row3.w);
	return -(x_axis * t.x + y_axis * t.y + z_axis * t.z);
}

// Step 2: Projected error in shadow texels of the page's mip (matches P5's texel rule at mip 0,
// scaled by 2^mip for coarser pages). Skinned instances scale error by max bone scale (#160).
float vsm_projected_error_skinned(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_error, float p_bone_scale, bool p_use_world_aabb_dist, VSMMapData md, uint mip) {
	if (p_error >= 3.4e38) {
		return vsm_params.max_texel_error + 1.0;
	}
#if !defined(VSM_SETUP) && !defined(VSM_SEED)
	// GodotNext: spline_meshes (#161). Deform LOD sphere, scale error by max stretch, and add monotonic curve sag.
	if ((p_record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
		SplineDeformPose sp = spline_deforms.data[p_record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current;
		float raw_radius = p_lod_sphere.w;
		spline_deform_bounds_sphere(sp, p_lod_sphere.xyz, p_lod_sphere.w);
		p_error = p_error * sp.up_max_stretch.w + spline_deform_curvature_error(sp, raw_radius);
	}
#endif
	float scale = vsm_instance_scale(p_record);
	float eff_scale = scale * p_bone_scale;
	float page_texel = md.texel_scale * float(1u << mip);
	if (!(page_texel > 0.0)) {
		return vsm_params.max_texel_error + 1.0;
	}
	if (md.kind == VSM_MAP_KIND_DIRECTIONAL) {
		return (p_error * eff_scale / page_texel) * vsm_params.lod_bias;
	}
	vec3 light_origin = vsm_light_origin(md);
	float distance;
	if (p_use_world_aabb_dist) {
		vec3 closest = clamp(light_origin, p_record.world_aabb_min.xyz, p_record.world_aabb_max.xyz);
		distance = max(length(closest - light_origin), max(md.near_w, 1.0e-5));
	} else {
		vec3 center = vsm_instance_point(p_record, p_lod_sphere.xyz);
		// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152) x vsm_nanite (#217).
		// Inflate sphere radius by displacement, vertex-offset, and foliage wind sway bounds.
		float expand = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_record_vertex_offset_bound(p_record) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
		float radius = (p_lod_sphere.w + expand) * scale;
		distance = max(length(center - light_origin) - radius, max(md.near_w, 1.0e-5));
	}
	return (p_error * eff_scale / (page_texel * distance)) * vsm_params.lod_bias;
}

float vsm_projected_error(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_error, VSMMapData md, uint mip) {
	return vsm_projected_error_skinned(p_record, p_lod_sphere, p_error, 1.0, false, md, mip);
}

bool vsm_corners_page_bounds(vec3 corners[8], VSMMapData md, uint mip, uvec2 slot, out vec2 r_px_min, out vec2 r_px_max, out float r_min_depth, out bool r_straddles_near) {
	r_straddles_near = false;
	if (md.kind == VSM_MAP_KIND_NONE || mip >= VSM_MIP_COUNT) {
		r_px_min = vec2(0.0);
		r_px_max = vec2(-1.0);
		r_min_depth = 0.0;
		return false;
	}
	if (md.kind == VSM_MAP_KIND_DIRECTIONAL) {
		float min_tx = 0.0;
		float max_tx = 0.0;
		float min_ty = 0.0;
		float max_ty = 0.0;
		float min_d = 0.0;
		for (int c = 0; c < 8; c++) {
			float tx = vsm_row(md.row0, corners[c]);
			float ty = vsm_row(md.row1, corners[c]);
			float tz = vsm_row(md.row2, corners[c]);
			if (c == 0) {
				min_tx = tx;
				max_tx = tx;
				min_ty = ty;
				max_ty = ty;
				min_d = tz;
			} else {
				min_tx = min(min_tx, tx);
				max_tx = max(max_tx, tx);
				min_ty = min(min_ty, ty);
				max_ty = max(max_ty, ty);
				min_d = min(min_d, tz);
			}
		}
		uint mask = (VSM_PAGES_PER_SIDE >> mip) - 1u;
		ivec2 mip_origin = md.origin >> int(mip);
		ivec2 abs_p = mip_origin + ivec2((slot - uvec2(mip_origin)) & uvec2(mask));
		float page_texels = float(VSM_PAGE_SIZE << mip);
		float inv_mip_scale = 1.0 / float(1u << mip);
		vec2 pmin = vec2(abs_p) * page_texels;
		r_px_min = (vec2(min_tx, min_ty) - pmin) * inv_mip_scale;
		r_px_max = (vec2(max_tx, max_ty) - pmin) * inv_mip_scale;
		r_min_depth = min_d;
		return r_px_max.x >= 0.0 && r_px_min.x <= float(VSM_PAGE_SIZE) && r_px_max.y >= 0.0 && r_px_min.y <= float(VSM_PAGE_SIZE);
	}

	float n = float(VSM_PAGES_PER_SIDE >> mip);
	float ox = n - 2.0 * float(slot.x) - 1.0;
	float oy = n - 2.0 * float(slot.y) - 1.0;
	uint out_near = 0u;
	uint out_far = 0u;
	uint out_left = 0u;
	uint out_right = 0u;
	uint out_bottom = 0u;
	uint out_top = 0u;
	float cx[8];
	float cy[8];
	float cw[8];
	float min_w = 0.0;
	for (int c = 0; c < 8; c++) {
		float x = vsm_row(md.row0, corners[c]);
		float y = vsm_row(md.row1, corners[c]);
		float w = vsm_row(md.row3, corners[c]);
		float xc = x * n + w * ox;
		float yc = y * n + w * oy;
		cx[c] = xc;
		cy[c] = yc;
		cw[c] = w;
		min_w = c == 0 ? w : min(min_w, w);
		out_near += w < md.near_w ? 1u : 0u;
		out_far += w > md.max_distance ? 1u : 0u;
		out_left += xc < -w ? 1u : 0u;
		out_right += xc > w ? 1u : 0u;
		out_bottom += yc < -w ? 1u : 0u;
		out_top += yc > w ? 1u : 0u;
	}
	if (out_near == 8u || out_far == 8u || out_left == 8u || out_right == 8u || out_bottom == 8u || out_top == 8u) {
		r_px_min = vec2(0.0);
		r_px_max = vec2(-1.0);
		r_min_depth = 0.0;
		return false;
	}
	if (min_w <= md.near_w) {
		r_straddles_near = true;
		r_px_min = vec2(0.0);
		r_px_max = vec2(float(VSM_PAGE_SIZE));
		r_min_depth = md.near_w;
		return true;
	}
	float min_px = 0.0;
	float max_px = 0.0;
	float min_py = 0.0;
	float max_py = 0.0;
	for (int c = 0; c < 8; c++) {
		float px = ((cx[c] / cw[c]) * 0.5 + 0.5) * float(VSM_PAGE_SIZE);
		float py = ((cy[c] / cw[c]) * 0.5 + 0.5) * float(VSM_PAGE_SIZE);
		if (c == 0) {
			min_px = px;
			max_px = px;
			min_py = py;
			max_py = py;
		} else {
			min_px = min(min_px, px);
			max_px = max(max_px, px);
			min_py = min(min_py, py);
			max_py = max(max_py, py);
		}
	}
	r_px_min = vec2(min_px, min_py);
	r_px_max = vec2(max_px, max_py);
	r_min_depth = min_w;
	return max_px >= 0.0 && min_px <= float(VSM_PAGE_SIZE) && max_py >= 0.0 && min_py <= float(VSM_PAGE_SIZE);
}

bool vsm_world_aabb_page_bounds(vec3 bmin, vec3 bmax, VSMMapData md, uint mip, uvec2 slot, out vec2 r_px_min, out vec2 r_px_max, out float r_min_depth, out bool r_straddles_near) {
	if (!(bmax.x >= bmin.x && bmax.y >= bmin.y && bmax.z >= bmin.z)) {
		r_px_min = vec2(0.0);
		r_px_max = vec2(-1.0);
		r_min_depth = 0.0;
		r_straddles_near = false;
		return false;
	}
	vec3 corners[8];
	for (int c = 0; c < 8; c++) {
		corners[c] = vec3((c & 1) != 0 ? bmax.x : bmin.x, (c & 2) != 0 ? bmax.y : bmin.y, (c & 4) != 0 ? bmax.z : bmin.z);
	}
	return vsm_corners_page_bounds(corners, md, mip, slot, r_px_min, r_px_max, r_min_depth, r_straddles_near);
}

bool vsm_local_aabb_page_bounds(NextGeoInstanceRecord p_record, vec3 lmin, vec3 lmax, VSMMapData md, uint mip, uvec2 slot, out vec2 r_px_min, out vec2 r_px_max, out float r_min_depth, out bool r_straddles_near) {
	if (!(lmax.x >= lmin.x && lmax.y >= lmin.y && lmax.z >= lmin.z)) {
		r_px_min = vec2(0.0);
		r_px_max = vec2(-1.0);
		r_min_depth = 0.0;
		r_straddles_near = false;
		return false;
	}
#if !defined(VSM_SETUP) && !defined(VSM_SEED)
	// GodotNext: spline_meshes (#161). Deform local BVH/group AABB along the Hermite segment.
	if ((p_record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
		SplineDeformPose sp = spline_deforms.data[p_record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current;
		spline_deform_local_aabb(sp, lmin, lmax);
	}
#endif
	// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152) x vsm_nanite (#217).
	// Inflate local bounds by displacement, vertex-offset, and foliage wind sway bounds.
	float expand = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_record_vertex_offset_bound(p_record) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
	lmin -= vec3(expand);
	lmax += vec3(expand);
	vec3 corners[8];
	for (int c = 0; c < 8; c++) {
		vec3 lp = vec3((c & 1) != 0 ? lmax.x : lmin.x, (c & 2) != 0 ? lmax.y : lmin.y, (c & 4) != 0 ? lmax.z : lmin.z);
		corners[c] = vsm_instance_point(p_record, lp);
	}
	return vsm_corners_page_bounds(corners, md, mip, slot, r_px_min, r_px_max, r_min_depth, r_straddles_near);
}

// Step 3: Per-page 1-level 8x8 Hi-Z occlusion test. Uncached, scrolled, static-dirty, unowned or
// near-straddling bounds skip Hi-Z and return false (visible).
bool vsm_hiz_occluded(uvec4 rec, uint map, uint mip, uvec2 slot, vec2 px_min, vec2 px_max, float min_depth, bool straddles_near) {
	if ((vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_HIZ) == 0u || straddles_near) {
		return false;
	}
	if ((rec.w & (VSM_ENTRY_UNCACHED | VSM_ENTRY_SCROLLED | VSM_ENTRY_DIRTY_STATIC)) != 0u) {
		return false;
	}
	uvec2 phys_page = vsm_raster_entry_page(rec.z);
	uint page_idx = phys_page.y * vsm_params.grid_width + phys_page.x;
	if (page_idx >= vsm_params.pool_pages) {
		return false;
	}
	uint expected_owner = vsm_entry_index(map, mip, slot) + 1u;
	if (vsm_hiz_owner.data[page_idx] != expected_owner) {
		return false;
	}
	uint nearest_packed = vsm_depth_pack(min_depth);
	if (nearest_packed == 0u) {
		return false;
	}
	ivec2 c0 = clamp(ivec2(floor(px_min)) >> int(NEXTGEO_VSM_HIZ_BLOCK_SHIFT), ivec2(0), ivec2(int(NEXTGEO_VSM_HIZ_PAGE_SIZE) - 1));
	ivec2 c1 = clamp(ivec2(floor(px_max)) >> int(NEXTGEO_VSM_HIZ_BLOCK_SHIFT), ivec2(0), ivec2(int(NEXTGEO_VSM_HIZ_PAGE_SIZE) - 1));
	ivec2 hiz_base = ivec2(phys_page * NEXTGEO_VSM_HIZ_PAGE_SIZE);
	uint min_hiz = 0xFFFFFFFFu;
	for (int cy = c0.y; cy <= c1.y; cy++) {
		for (int cx = c0.x; cx <= c1.x; cx++) {
			uint h = imageLoad(vsm_hiz, hiz_base + ivec2(cx, cy)).r;
			min_hiz = min(min_hiz, h);
		}
	}
	return nearest_packed < min_hiz;
}

uint vsm_dag_uint(uint p_byte_offset) {
	return dag.data[p_byte_offset >> 2u];
}

float vsm_dag_float(uint p_byte_offset) {
	return uintBitsToFloat(dag.data[p_byte_offset >> 2u]);
}

bool vsm_mesh_group_refine_ok(uint p_mesh_idx, uint p_group_idx) {
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

#ifdef VSM_SETUP
void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	uint total_dirty = min(vsm_alloc_state.data[VSM_STATE_DIRTY], vsm_params.pool_pages);
	uint dir_dirty = min(vsm_alloc_state.data[VSM_STATE_DIRTY_DIRECTIONAL], total_dirty);
	uint d_begin = (vsm_params.chain_index == 0u) ? 0u : dir_dirty;
	uint d_end = (vsm_params.chain_index == 0u) ? dir_dirty : total_dirty;
	uint d_count = d_end > d_begin ? (d_end - d_begin) : 0u;
	vsm_counters.dirty_pages = d_count;
	uint inst_groups = (vsm_params.record_count + 63u) / 64u;
	vsm_args_out.data[0] = uvec4((d_count > 0u && vsm_params.record_count > 0u) ? inst_groups : 0u, d_count, 1u, 0u);
}
#elif defined(VSM_SEED)
void main() {
	uint total_dirty = min(vsm_alloc_state.data[VSM_STATE_DIRTY], vsm_params.pool_pages);
	uint dir_dirty = min(vsm_alloc_state.data[VSM_STATE_DIRTY_DIRECTIONAL], total_dirty);
	uint d_begin = (vsm_params.chain_index == 0u) ? 0u : dir_dirty;
	uint d_end = (vsm_params.chain_index == 0u) ? dir_dirty : total_dirty;

	uint record_idx = gl_GlobalInvocationID.x;
	uint dirty_idx = d_begin + gl_WorkGroupID.y;
	if (record_idx >= vsm_params.record_count || dirty_idx >= d_end) {
		return;
	}

	uvec4 rec = vsm_dirty.data[dirty_idx];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	if (map >= vsm_params.table_maps || mip >= VSM_MIP_COUNT) {
		return;
	}
	uvec4 meta = vsm_map_meta.data[map];
	if ((meta.y & NEXTGEO_VSM_MAP_META_ACTIVE) == 0u) {
		return;
	}

	NextGeoInstanceRecord record = instances.data[record_idx];
	uint req_flags = NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE;
	if ((record.flags & req_flags) != req_flags ||
			(record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF ||
			(record.flags & (NEXTGEO_RECORD_HAS_INSTANCE_UNIFORMS | NEXTGEO_RECORD_MULTIMESH_PARENT | NEXTGEO_RECORD_HAS_ASSEMBLY)) != 0u ||
			(record.flags & (NEXTGEO_RECORD_HAS_SKELETON | NEXTGEO_RECORD_SKINNED_PALETTE)) == NEXTGEO_RECORD_HAS_SKELETON ||
			(record.gi_flags & NEXTGEO_GI_LIGHTMAPPED) != 0u ||
			(record.layer_mask & meta.x) == 0u ||
			record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	bool prog_enabled = (vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_PROGRAMMABLE_BINS) != 0u;
	uint avail_mask = prog_enabled ? record.surface_mask : record.pad0;
	if (avail_mask == 0u) {
		return;
	}

	bool has_prog_surface = prog_enabled && ((record.surface_mask & ~record.pad0) != 0u);
	// GodotNext: vsm_caching (#212). Only AUTO instances with programmable surfaces can mix static
	// and dynamic surfaces; ALWAYS/RIGID/STATIC route every surface to one deterministic layer.
	uint cache_mode = record.flags & NEXTGEO_RECORD_VSM_CACHE_MODE_MASK;
	if (has_prog_surface && cache_mode == NEXTGEO_RECORD_VSM_CACHE_AUTO && (vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK) != 0u) {
		if ((rec.w & (VSM_ENTRY_DIRTY_STATIC | VSM_ENTRY_DIRTY_DYNAMIC)) == 0u) {
			return;
		}
	} else {
		uint layer = vsm_instance_layer(record, vsm_params.cull_flags, false);
		uint req_dirty = layer != 0u ? VSM_ENTRY_DIRTY_DYNAMIC : VSM_ENTRY_DIRTY_STATIC;
		if ((rec.w & req_dirty) == 0u) {
			return;
		}
	}

	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.bvh_level_count == 0u || mesh.bvh_node_offset == 0u) {
		return;
	}

	VSMMapData md = vsm_maps.data[map];
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	vec2 px_min;
	vec2 px_max;
	float min_depth;
	bool straddles_near;
	// GodotNext: nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152). Inflate instance AABB by (vertex_offset_max + wind_max_sway) * max_scale.
	float wpo_margin = (nextgeo_record_vertex_offset_bound(record) + nextgeo_wind_mesh_max_sway(record.virtual_mesh)) * abs(record.world_aabb_min.w);
	if (!vsm_world_aabb_page_bounds(record.world_aabb_min.xyz - vec3(wpo_margin), record.world_aabb_max.xyz + vec3(wpo_margin), md, mip, slot, px_min, px_max, min_depth, straddles_near)) {
		return;
	}
	if (!has_prog_surface && vsm_hiz_occluded(rec, map, mip, slot, px_min, px_max, min_depth, straddles_near)) {
		vsm_ng_sink(atomicAdd(vsm_counters.hiz_culled_nodes, 1u));
		return;
	}

	uint node_slot = atomicAdd(vsm_counters.bvh_level_counts[0], 1u);
	if (node_slot >= vsm_params.node_capacity) {
		vsm_ng_mark_overflow(rec);
		return;
	}
	vsm_ng_sink(atomicAdd(vsm_counters.seeded_pairs, 1u));
	vsm_nodes_out.data[node_slot] = uvec4(record_idx, 0u, dirty_idx, 0u);
	uint groups = (node_slot + 64u) / 64u;
	vsm_ng_sink(atomicMax(vsm_args_out.data[0].x, groups));
}
#elif defined(CLUSTER_CULL)
bool vsm_sw_raster_route(NextGeoInstanceRecord p_record, float p_scale, vec4 p_bounds, float p_max_edge, VSMMapData md, uint mip) {
	float threshold = vsm_params.sw_max_edge_texels;
	if (!(threshold > 0.0) || !(p_scale > 0.0) || !(p_max_edge >= 0.0) || isinf(p_max_edge)) {
		return false;
	}
	float page_texel = md.texel_scale * float(1u << mip);
	if (!(page_texel > 0.0)) {
		return false;
	}
	if (md.kind == VSM_MAP_KIND_DIRECTIONAL) {
		return (p_max_edge * p_scale / page_texel) < threshold;
	}
	vec3 center = vsm_instance_point(p_record, p_bounds.xyz);
	float expand = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_record_vertex_offset_bound(p_record) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
	float w_min = vsm_row(md.row3, center) - (p_bounds.w + expand) * p_scale;
	if (!(w_min > md.near_w)) {
		return false;
	}
	return (p_max_edge * p_scale / max(page_texel * w_min, 1.0e-20)) < threshold;
}

uint vsm_raster_bin_of(NextGeoInstanceRecord p_record, uint p_surface_id) {
	if (p_surface_id >= 32u) {
		return NEXTGEO_RASTER_BIN_FIXED;
	}
	uint word_idx = nextgeo_raster_bin_table_word(p_record.bucket_base, p_surface_id);
	if (word_idx >= vsm_params.bucket_table_words) {
		return NEXTGEO_RASTER_BIN_FIXED;
	}
	uint word = raster_bin_table.data[word_idx];
	uint bin = (word >> nextgeo_raster_bin_table_shift(p_surface_id)) & 0xFFu;
	return bin < NEXTGEO_RASTER_BIN_LIMIT ? bin : NEXTGEO_RASTER_BIN_FIXED;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint input_count = min(vsm_counters.group_count, vsm_params.group_capacity);
	if (index >= input_count) {
		return;
	}
	uvec4 entry = vsm_nodes_in.data[index];
	uint record_idx = entry.x;
	uint group_idx = entry.y;
	uint dirty_idx = entry.z;

	NextGeoInstanceRecord record = instances.data[record_idx];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	uint mesh_idx = record.virtual_mesh;
	NextGeoMeshRecord mesh = meshes.data[mesh_idx];
	if (mesh.bvh_node_offset <= mesh.group_offset) {
		return;
	}
	uint group_count = (mesh.bvh_node_offset - mesh.group_offset) / 60u;
	if (group_idx >= group_count || !vsm_mesh_group_refine_ok(mesh_idx, group_idx)) {
		return;
	}

	uvec4 rec = vsm_dirty.data[dirty_idx];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	if (map >= vsm_params.table_maps || mip >= VSM_MIP_COUNT) {
		return;
	}
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	VSMMapData md = vsm_maps.data[map];
	uvec4 meta = vsm_map_meta.data[map];
	bool map_reverse_cull = (meta.y & NEXTGEO_VSM_MAP_META_REVERSE_CULL) != 0u;

	uint base = mesh.group_offset + group_idx * 60u;
	uint cluster_offset = vsm_dag_uint(base);
	uint cluster_count = vsm_dag_uint(base + 4u);
	uint group_page_id = vsm_dag_uint(base + 8u);
	vec4 parent_lod = vec4(vsm_dag_float(base + 16u), vsm_dag_float(base + 20u), vsm_dag_float(base + 24u), vsm_dag_float(base + 28u));
	float parent_error = vsm_dag_float(base + 32u);
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	float inst_bone_scale = is_skinned ? nextgeo_skin_instance_max_scale(record_idx) : 1.0;
	if (vsm_projected_error_skinned(record, parent_lod, parent_error, inst_bone_scale, is_skinned, md, mip) <= vsm_params.max_texel_error) {
		return;
	}

	bool prog_enabled = (vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_PROGRAMMABLE_BINS) != 0u;
	bool pure_fixed = !prog_enabled || (record.surface_mask == record.pad0);
	if (pure_fixed && !is_skinned) {
		float disp_bound = max(record.prev_world_aabb_max.w, 0.0);
		vec3 g_min = vec3(vsm_dag_float(base + 36u), vsm_dag_float(base + 40u), vsm_dag_float(base + 44u)) - vec3(disp_bound);
		vec3 g_max = vec3(vsm_dag_float(base + 48u), vsm_dag_float(base + 52u), vsm_dag_float(base + 56u)) + vec3(disp_bound);
		vec2 px_min;
		vec2 px_max;
		float min_depth;
		bool straddles_near;
		if (!vsm_local_aabb_page_bounds(record, g_min, g_max, md, mip, slot, px_min, px_max, min_depth, straddles_near)) {
			return;
		}
		if (vsm_hiz_occluded(rec, map, mip, slot, px_min, px_max, min_depth, straddles_near)) {
			vsm_ng_sink(atomicAdd(vsm_counters.hiz_culled_nodes, 1u));
			return;
		}
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
	float route_scale = vsm_instance_scale(record);
	uint avail_mask = prog_enabled ? record.surface_mask : record.pad0;

	// GodotNext: nextgeo_voxels (#165) x vsm_nanite (#217). Swap coarse voxelized groups for
	// sparse 4^3 bricks when a brick projects <= V shadow texels and the group's error test passes.
	bool vsm_voxels_on = (vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_VOXELS) != 0u &&
			!is_skinned && (record.flags & NEXTGEO_RECORD_HAS_SPLINE) == 0u &&
			page_slot != 0xFFFFFFFFu && slot_page_size >= 96u && avail_mask != 0u;
	if (vsm_voxels_on) {
		uint page_word_base = (page_slot * slot_page_size) >> 2u;
		uint page_words = slot_page_size >> 2u;
		uint voxel_rel_word = pages.data[page_word_base + 3u];
		if (voxel_rel_word >= 4u && voxel_rel_word + 4u <= page_words) {
			uint voxel_base = page_word_base + voxel_rel_word;
			if (pages.data[voxel_base + 0u] == NEXTGEO_VOXEL_SECTION_MAGIC) {
				uint first_g = pages.data[voxel_base + 1u];
				uint page_g_count = pages.data[voxel_base + 2u];
				if (group_idx >= first_g && (group_idx - first_g) < page_g_count &&
						voxel_rel_word + 4u + (group_idx - first_g + 1u) * 4u <= page_words) {
					uint dir_word = voxel_base + 4u + (group_idx - first_g) * 4u;
					uint brick_rel_word = pages.data[dir_word + 0u];
					uint brick_count = pages.data[dir_word + 1u];
					float voxel_size = uintBitsToFloat(pages.data[dir_word + 2u]);
					float vsm_voxel_thresh = uintBitsToFloat(vsm_params.bin_features[65]);
					uint vsm_voxel_cap = vsm_params.bin_features[66];
					if (brick_count > 0u && voxel_size > 0.0 && vsm_voxel_thresh > 0.0 && vsm_voxel_cap > 0u) {
						float texel_size = md.texel_scale * float(1u << mip);
						float brick_world = float(NEXTGEO_VOXEL_BRICK_DIM) * voxel_size * route_scale;
						float brick_tx = 1e30;
						if (texel_size > 0.0) {
							if (md.kind == VSM_MAP_KIND_DIRECTIONAL) {
								brick_tx = brick_world / texel_size;
							} else {
								vec3 c_w = vsm_instance_point(record, parent_lod.xyz);
								float z_min = max(length(c_w - vsm_light_origin(md)) - parent_lod.w * route_scale, max(md.near_w, 1e-4));
								brick_tx = brick_world / (texel_size * z_min);
							}
						}
						if (brick_tx <= vsm_voxel_thresh) {
							bool group_error_passes = true;
							for (uint c = 0u; c < cluster_count; c++) {
								uint gc = cluster_offset + c;
								if (gc < mesh.cluster_offset) {
									continue;
								}
								uint lc = gc - mesh.cluster_offset;
								if (lc >= mesh.cluster_count) {
									continue;
								}
								if (page_cluster_hdr > 0u && lc >= page_first_cluster && (lc - page_first_cluster) < page_cluster_hdr) {
									uint rec_word = page_word_base + 4u + (lc - page_first_cluster) * 20u;
									int refined_group = int(pages.data[rec_word + 15u]);
									if (refined_group >= 0 && uint(refined_group) < group_count) {
										vec4 own_lod = vec4(
												uintBitsToFloat(pages.data[rec_word + 5u]),
												uintBitsToFloat(pages.data[rec_word + 6u]),
												uintBitsToFloat(pages.data[rec_word + 7u]),
												uintBitsToFloat(pages.data[rec_word + 8u]));
										float own_error = uintBitsToFloat(pages.data[rec_word + 9u]);
										float child_err = vsm_projected_error_skinned(record, own_lod, own_error, 1.0, false, md, mip);
										if (child_err > vsm_params.max_texel_error && vsm_mesh_group_refine_ok(mesh_idx, uint(refined_group))) {
											group_error_passes = false;
											break;
										}
									}
								}
							}
							if (group_error_passes) {
								uint layer = vsm_instance_layer(record, vsm_params.cull_flags, false);
								uint req_dirty = layer != 0u ? VSM_ENTRY_DIRTY_DYNAMIC : VSM_ENTRY_DIRTY_STATIC;
								if ((rec.w & req_dirty) != 0u) {
									uint cursor = voxel_base + brick_rel_word;
									for (uint b = 0u; b < brick_count; b++) {
										uint meta = pages.data[cursor + 6u];
										uint occ_count = meta & 0xFFu;
										uint vslot = atomicAdd(vsm_counters.voxel_bricks, 1u);
										if (vslot < vsm_voxel_cap) {
											vsm_voxel_bricks_out.data[vslot] = uvec4(record_idx, cursor, dirty_idx, layer & 1u);
											vsm_ng_sink(atomicMax(vsm_voxel_args.data[0], min(vslot + 1u, NEXTGEO_SW_DISPATCH_WIDTH)));
											vsm_ng_sink(atomicMax(vsm_voxel_args.data[1], vslot / NEXTGEO_SW_DISPATCH_WIDTH + 1u));
										} else {
											vsm_ng_mark_overflow(rec);
											break;
										}
										cursor += NEXTGEO_VOXEL_BRICK_HEADER_WORDS + occ_count * NEXTGEO_VOXEL_DATA_WORDS;
									}
								}
								return;
							}
						}
					}
				}
			}
		}
	}

	for (uint cluster = 0u; cluster < cluster_count; cluster++) {
		uint global_cluster = cluster_offset + cluster;
		if (global_cluster < mesh.cluster_offset) {
			continue;
		}
		uint local_cluster = global_cluster - mesh.cluster_offset;
		if (local_cluster >= mesh.cluster_count) {
			continue;
		}

		uint cluster_table_idx = mesh.page_table_offset + local_cluster;
		vec3 skin_lmin = vec3(0.0);
		vec3 skin_lmax = vec3(0.0);
		float cluster_bone_scale = inst_bone_scale;
		bool has_skin_bounds = is_skinned && nextgeo_skin_cluster_bounds(record_idx, cluster_table_idx, skin_lmin, skin_lmax, cluster_bone_scale);
		vec4 skin_sphere = has_skin_bounds ? vec4(0.5 * (skin_lmin + skin_lmax), 0.5 * length(skin_lmax - skin_lmin)) : vec4(0.0);

		uint route_rec_word = 0xFFFFFFFFu;
		if (page_cluster_hdr > 0u && local_cluster >= page_first_cluster && (local_cluster - page_first_cluster) < page_cluster_hdr) {
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
				float child_err = has_skin_bounds
						? vsm_projected_error_skinned(record, skin_sphere, own_error, cluster_bone_scale, false, md, mip)
						: vsm_projected_error_skinned(record, own_lod, own_error, inst_bone_scale, is_skinned, md, mip);
				if (child_err > vsm_params.max_texel_error && vsm_mesh_group_refine_ok(mesh_idx, uint(refined_group))) {
					continue;
				}
			}
		}

		uint surface_id = cluster_table.data[cluster_table_idx].w & 0xFFFFu;
		if (surface_id >= 32u || (avail_mask & (1u << surface_id)) == 0u) {
			continue;
		}
		uint bin = NEXTGEO_RASTER_BIN_FIXED;
		if (prog_enabled) {
			bin = vsm_raster_bin_of(record, surface_id);
		}
		uint bin_features = bin < 68u ? vsm_params.bin_features[bin] : 0u;
		bool dist_demoted = false;
		bool masked_demoted = false;
		bool wpo_demoted = false;
		bool wpo_disabled = false;
		// GodotNext: nextgeo_masked_two_sided (#153), nextgeo_vertex_offset (#164). Past
		// programmable_raster_distance / vertex_offset_disable_distance, demote non-displaced
		// programmable bins to the FIXED shadow rasterizer once all active programmable features disable.
		if (bin != NEXTGEO_RASTER_BIN_FIXED && (bin_features & NEXTGEO_RASTER_FEATURE_DISPLACED) == 0u) {
			bool has_pix_feat = (bin_features & NEXTGEO_RASTER_PIXEL_PROGRAMMABLE_FEATURES) != 0u;
			bool has_wpo_feat = (bin_features & NEXTGEO_RASTER_FEATURE_VERTEX_OFFSET) != 0u;
			float prog_dist = record.prev_world_aabb_min.w;
			float wpo_dist = nextgeo_record_vertex_offset_disable_distance(record);
			if ((has_pix_feat && prog_dist > 0.0) || (has_wpo_feat && wpo_dist > 0.0)) {
				vec3 closest = clamp(vsm_params.camera_position.xyz, record.world_aabb_min.xyz, record.world_aabb_max.xyz);
				float cam_dist = length(vsm_params.camera_position.xyz - closest);
				bool pix_disabled = has_pix_feat && prog_dist > 0.0 && cam_dist > prog_dist;
				wpo_disabled = has_wpo_feat && wpo_dist > 0.0 && cam_dist > wpo_dist;
				if ((has_pix_feat || has_wpo_feat) && (!has_pix_feat || pix_disabled) && (!has_wpo_feat || wpo_disabled)) {
					bin = NEXTGEO_RASTER_BIN_FIXED;
					bin_features = 0u;
					dist_demoted = true;
					masked_demoted = pix_disabled;
					wpo_demoted = wpo_disabled;
				}
			}
		}
		if (bin == NEXTGEO_RASTER_BIN_FIXED && !dist_demoted && (record.pad0 & (1u << surface_id)) == 0u) {
			continue;
		}
		bool has_wpo = ((bin_features & NEXTGEO_RASTER_FEATURE_VERTEX_OFFSET) != 0u) && !wpo_disabled;
		bool has_dynamic_bin = has_wpo || ((bin_features & NEXTGEO_RASTER_FEATURE_DEPTH_OFFSET) != 0u);
		uint layer = vsm_instance_layer(record, vsm_params.cull_flags, has_dynamic_bin);
		uint req_dirty = layer != 0u ? VSM_ENTRY_DIRTY_DYNAMIC : VSM_ENTRY_DIRTY_STATIC;
		if ((rec.w & req_dirty) == 0u) {
			continue;
		}

		bool software = false;
		if (route_rec_word != 0xFFFFFFFFu) {
			vec4 bounds = vec4(
					uintBitsToFloat(pages.data[route_rec_word + 0u]),
					uintBitsToFloat(pages.data[route_rec_word + 1u]),
					uintBitsToFloat(pages.data[route_rec_word + 2u]),
					uintBitsToFloat(pages.data[route_rec_word + 3u]));
			float max_edge = unpackHalf2x16(pages.data[route_rec_word + 16u]).x;
			// GodotNext: spline_meshes (#161). Deform cluster sphere and scale edge length by max spline stretch.
			if ((record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
				SplineDeformPose sp = spline_deforms.data[record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current;
				spline_deform_bounds_sphere(sp, bounds.xyz, bounds.w);
				max_edge *= sp.up_max_stretch.w;
			}
			float expand = max(record.prev_world_aabb_max.w, 0.0) + (has_wpo ? nextgeo_record_vertex_offset_bound(record) : 0.0) + nextgeo_wind_mesh_max_sway(record.virtual_mesh);
			vec2 px_min;
			vec2 px_max;
			float min_depth;
			bool straddles_near;
			if (has_skin_bounds) {
				if (!vsm_local_aabb_page_bounds(record, skin_lmin - vec3(expand), skin_lmax + vec3(expand), md, mip, slot, px_min, px_max, min_depth, straddles_near)) {
					continue;
				}
				if (!has_wpo && vsm_hiz_occluded(rec, map, mip, slot, px_min, px_max, min_depth, straddles_near)) {
					vsm_ng_sink(atomicAdd(vsm_counters.hiz_culled_clusters, 1u));
					continue;
				}
			} else if (!is_skinned) {
				vec3 c_w = vsm_instance_point(record, bounds.xyz);
				float r_w = (bounds.w + expand) * route_scale;
				if (!vsm_world_aabb_page_bounds(c_w - vec3(r_w), c_w + vec3(r_w), md, mip, slot, px_min, px_max, min_depth, straddles_near)) {
					continue;
				}
				if (!has_wpo && vsm_hiz_occluded(rec, map, mip, slot, px_min, px_max, min_depth, straddles_near)) {
					vsm_ng_sink(atomicAdd(vsm_counters.hiz_culled_clusters, 1u));
					continue;
				}
			}
			if (has_skin_bounds) {
				software = vsm_sw_raster_route(record, route_scale, skin_sphere, max_edge * cluster_bone_scale, md, mip);
			} else if (!is_skinned) {
				software = vsm_sw_raster_route(record, route_scale, bounds, max_edge, md, mip);
			}
		}
		// GodotNext: nextgeo_tessellation (#163). When VSM tessellation is active on this chain,
		// route displaced clusters into the bin's SW worklist for tessellation_dice.glsl.
		if (bin != NEXTGEO_RASTER_BIN_FIXED &&
				(vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_TESSELLATION) != 0u &&
				(bin_features & NEXTGEO_RASTER_FEATURE_DISPLACED) != 0u) {
			software = true;
		}

		uint cslot = atomicAdd(vsm_counters.cluster_count, 1u);
		if (cslot >= vsm_params.cluster_capacity) {
			vsm_ng_mark_overflow(rec);
			break;
		}
		vsm_clusters_out.data[cslot] = uvec4(record_idx, global_cluster, dirty_idx, (layer & 1u) | (map_reverse_cull ? 2u : 0u));
		// GodotNext: vsm_caching (#212). Per-layer cluster telemetry + ALWAYS layer-0 invariant counter.
		if (layer != 0u) {
			vsm_ng_sink(atomicAdd(vsm_counters.dynamic_clusters, 1u));
		} else {
			vsm_ng_sink(atomicAdd(vsm_counters.static_clusters, 1u));
			if ((record.flags & NEXTGEO_RECORD_VSM_CACHE_MODE_MASK) == NEXTGEO_RECORD_VSM_CACHE_ALWAYS) {
				vsm_ng_sink(atomicAdd(vsm_counters.always_layer0_clusters, 1u));
			}
		}

		if (bin == NEXTGEO_RASTER_BIN_FIXED) {
			if (masked_demoted) {
				vsm_ng_sink(atomicAdd(vsm_counters.distance_fixed_clusters, 1u));
			}
			if (wpo_demoted) {
				vsm_ng_sink(atomicAdd(vsm_counters.vertex_offset_distance_fixed, 1u));
			}
			if (software) {
				uint s = atomicAdd(vsm_counters.sw_clusters, 1u);
				if (s < vsm_params.cluster_capacity) {
					vsm_raster_lists.data[s] = cslot;
					vsm_ng_sink(atomicMax(vsm_raster_args.data[0], min(s + 1u, NEXTGEO_SW_DISPATCH_WIDTH)));
					vsm_ng_sink(atomicMax(vsm_raster_args.data[1], s / NEXTGEO_SW_DISPATCH_WIDTH + 1u));
				}
			} else {
				uint h = atomicAdd(vsm_counters.hw_clusters, 1u);
				if (h < vsm_params.cluster_capacity) {
					vsm_raster_lists.data[vsm_params.cluster_capacity - 1u - h] = cslot;
					vsm_ng_sink(atomicAdd(vsm_raster_args.data[4u + 1u], 1u));
					// GodotNext: nextgeo_mesh_raster (#154). Indirect mesh-task X group count (1 per 32 clusters).
					vsm_ng_sink(atomicMax(vsm_raster_args.data[8u + 0u], (h >> 5u) + 1u));
					vsm_ng_sink(atomicMax(vsm_raster_counts.data[0], 1u));
				}
			}
		} else {
			uint kind = software ? NEXTGEO_RASTER_BIN_KIND_SW : NEXTGEO_RASTER_BIN_KIND_HW;
			uint aslot = atomicAdd(raster_bin_state.data[NEXTGEO_RASTER_BIN_STATE_APPENDED], 1u);
			if (aslot < vsm_params.cluster_capacity) {
				raster_bin_append.data[aslot] = nextgeo_raster_bin_pack_entry(cslot, bin, kind);
				vsm_ng_sink(atomicAdd(raster_bin_state.data[nextgeo_raster_bin_count_word(0u, bin, kind)], 1u));
				if (software) {
					vsm_ng_sink(atomicAdd(vsm_counters.prog_sw_clusters, 1u));
				} else {
					vsm_ng_sink(atomicAdd(vsm_counters.prog_hw_clusters, 1u));
				}
			} else {
				vsm_ng_mark_overflow(rec);
			}
		}
	}
}
#else
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint level = min(vsm_push.level, 7u);
	uint input_count = min(vsm_counters.bvh_level_counts[level], vsm_params.node_capacity);
	if (index >= input_count) {
		return;
	}
	uvec4 entry = vsm_nodes_in.data[index];
	uint record_idx = entry.x;
	uint node_idx = entry.y;
	uint dirty_idx = entry.z;

	NextGeoInstanceRecord record = instances.data[record_idx];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.bvh_level_count == 0u || mesh.bvh_node_offset == 0u) {
		return;
	}

	uvec4 rec = vsm_dirty.data[dirty_idx];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	if (map >= vsm_params.table_maps || mip >= VSM_MIP_COUNT) {
		return;
	}
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	VSMMapData md = vsm_maps.data[map];

	uint base = mesh.bvh_node_offset + node_idx * 80u;
	vec4 lod_sphere = vec4(vsm_dag_float(base), vsm_dag_float(base + 4u), vsm_dag_float(base + 8u), vsm_dag_float(base + 12u));
	float max_parent_error = vsm_dag_float(base + 16u);
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	float inst_bone_scale = is_skinned ? vsm_skin_instance_max_scale(record_idx) : 1.0;
	if (vsm_projected_error_skinned(record, lod_sphere, max_parent_error, inst_bone_scale, is_skinned, md, mip) <= vsm_params.max_texel_error) {
		return;
	}

	bool prog_enabled = (vsm_params.cull_flags & NEXTGEO_VSM_CULL_FLAG_PROGRAMMABLE_BINS) != 0u;
	bool pure_fixed = !prog_enabled || (record.surface_mask == record.pad0);
	if (pure_fixed && !is_skinned) {
		float disp_bound = max(record.prev_world_aabb_max.w, 0.0);
		vec3 n_min = vec3(vsm_dag_float(base + 20u), vsm_dag_float(base + 24u), vsm_dag_float(base + 28u)) - vec3(disp_bound);
		vec3 n_max = vec3(vsm_dag_float(base + 32u), vsm_dag_float(base + 36u), vsm_dag_float(base + 40u)) + vec3(disp_bound);
		vec2 px_min;
		vec2 px_max;
		float min_depth;
		bool straddles_near;
		if (!vsm_local_aabb_page_bounds(record, n_min, n_max, md, mip, slot, px_min, px_max, min_depth, straddles_near)) {
			return;
		}
		if (vsm_hiz_occluded(rec, map, mip, slot, px_min, px_max, min_depth, straddles_near)) {
			vsm_ng_sink(atomicAdd(vsm_counters.hiz_culled_nodes, 1u));
			return;
		}
	}

	uint child_count = min(vsm_dag_uint(base + 44u), 8u);
	uint next_level = min(level + 1u, 7u);
	for (uint child = 0u; child < child_count; child++) {
		uint value = vsm_dag_uint(base + 48u + child * 4u);
		if ((value & NEXTGEO_BVH_LEAF_BIT) != 0u) {
			uint gslot = atomicAdd(vsm_counters.group_count, 1u);
			if (gslot >= vsm_params.group_capacity) {
				vsm_ng_mark_overflow(rec);
				continue;
			}
			vsm_groups_out.data[gslot] = uvec4(record_idx, value & ~NEXTGEO_BVH_LEAF_BIT, dirty_idx, 0u);
			uint groups = (gslot + 64u) / 64u;
			vsm_ng_sink(atomicMax(vsm_group_args_out.data[0].x, groups));
		} else if (level + 1u < 8u) {
			uint nslot = atomicAdd(vsm_counters.bvh_level_counts[next_level], 1u);
			if (nslot >= vsm_params.node_capacity) {
				vsm_ng_mark_overflow(rec);
				continue;
			}
			vsm_nodes_out.data[nslot] = uvec4(record_idx, value, dirty_idx, 0u);
			uint groups = (nslot + 64u) / 64u;
			vsm_ng_sink(atomicMax(vsm_args_out.data[next_level].x, groups));
		}
	}
}
#endif

#else

// clang-format off
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
#ifndef CLUSTER_CULL
layout(set = 0, binding = 14, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;
#endif
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
const uint NEXTGEO_CULL_FLAG_TESSELLATION = 1u << 7u;
const uint NEXTGEO_CULL_FLAG_VOXELS = 1u << 8u;

// GodotNext: nextgeo_voxels (#165). Bindings 29..30 avoid 27..28 reserved for #152.
layout(set = 0, binding = 29, std430) buffer restrict writeonly VisibleVoxelBricks {
	uvec4 data[];
}
visible_voxel_bricks;

// [0..3]: phase 0 indirect dispatch (x, y, 1, 0); [4..7]: phase 1 indirect dispatch (x, y, 1, 0).
layout(set = 0, binding = 30, std430) buffer restrict VoxelRasterArgs {
	uint data[];
}
voxel_raster_args;
#endif

// GodotNext: spline_meshes (#161). Bound in BVH_LEVEL and CLUSTER_CULL (main and shadow) at binding 26.
layout(set = 0, binding = 26, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;

#ifndef CLUSTER_CULL
layout(set = 0, binding = 24, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

float skin_instance_max_scale(uint p_record_idx) {
	uvec4 sinfo = nextgeo_skin_instances.data[p_record_idx];
	if (sinfo.z == 0u) {
		return 1.0;
	}
	float s = uintBitsToFloat(sinfo.w);
	return s > 0.0 ? s : 1.0;
}
#endif

vec3 instance_point(NextGeoInstanceRecord p_record, vec3 p_local) {
	return vec3(
			dot(p_record.transform[0].xyz, p_local) + p_record.transform[0].w,
			dot(p_record.transform[1].xyz, p_local) + p_record.transform[1].w,
			dot(p_record.transform[2].xyz, p_local) + p_record.transform[2].w);
}

// Plan §7.3 step 3 & §7.5: projected error in pixels / shadow texels for
// perspective, orthographic directional splits (bit 2), and dual-paraboloid
// hemispheres (bit 3). Skinned instances scale error by max bone scale (#160).
float projected_error_params_skinned(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error, float p_bone_scale, bool p_use_world_aabb_dist, vec4 p_camera, vec4 p_view, mat4 p_view_proj, uint p_flags) {
	if (p_max_parent_error >= 3.4e38) {
		return p_camera.w + 1.0;
	}
	// GodotNext: spline_meshes (#161). Deform cluster/group/BVH LOD sphere, scale error, and add monotonic curve sag.
	if ((p_record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
		SplineDeformPose sp = spline_deforms.data[p_record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current;
		float raw_radius = p_lod_sphere.w;
		spline_deform_bounds_sphere(sp, p_lod_sphere.xyz, p_lod_sphere.w);
		p_max_parent_error = p_max_parent_error * sp.up_max_stretch.w + spline_deform_curvature_error(sp, raw_radius);
	}
	float scale = p_record.world_aabb_min.w;
	float eff_scale = scale * p_bone_scale;
	if ((p_flags & 4u) != 0u) {
		return p_max_parent_error * eff_scale * p_view.x * p_view.w;
	}
	float distance;
	if (p_use_world_aabb_dist) {
		vec3 closest = clamp(p_camera.xyz, p_record.world_aabb_min.xyz, p_record.world_aabb_max.xyz);
		distance = max(length(closest - p_camera.xyz), p_view.z);
	} else {
		vec3 center = instance_point(p_record, p_lod_sphere.xyz);
		// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152). Inflate sphere radius by displacement, WPO, and wind sway bounds.
		float expand = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_record_vertex_offset_bound(p_record) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
		float radius = (p_lod_sphere.w + expand) * scale;
		distance = max(length(center - p_camera.xyz) - radius, p_view.z);
	}
	if ((p_flags & 8u) != 0u) {
		return p_max_parent_error * eff_scale * 0.5 * p_view.y / distance * p_view.w;
	}
	return p_max_parent_error * eff_scale * 0.5 * p_view.y * abs(p_view_proj[1][1]) / distance * p_view.w;
}

float projected_error_params(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error, vec4 p_camera, vec4 p_view, mat4 p_view_proj, uint p_flags) {
	return projected_error_params_skinned(p_record, p_lod_sphere, p_max_parent_error, 1.0, false, p_camera, p_view, p_view_proj, p_flags);
}

#ifndef SHADOW_VIEW
float projected_error_skinned(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error, float p_bone_scale, bool p_use_world_aabb_dist) {
	return projected_error_params_skinned(p_record, p_lod_sphere, p_max_parent_error, p_bone_scale, p_use_world_aabb_dist, cull.camera, cull.view, cull.view_proj, cull.cull.w);
}

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
// GodotNext: nextgeo_skinned_runtime (#160). Frustum and Phase-2 Hi-Z culling of skinned
// cluster bone-bound union AABBs transformed to world space.
bool skin_aabb_visible_planes(NextGeoInstanceRecord p_record, vec3 p_lmin, vec3 p_lmax, vec4 p_frustum[6]) {
	float disp_bound = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
	vec3 lmin = p_lmin - vec3(disp_bound);
	vec3 lmax = p_lmax + vec3(disp_bound);
	vec3 corners[8];
	for (int c = 0; c < 8; c++) {
		vec3 lp = vec3((c & 1) != 0 ? lmax.x : lmin.x, (c & 2) != 0 ? lmax.y : lmin.y, (c & 4) != 0 ? lmax.z : lmin.z);
		corners[c] = instance_point(p_record, lp);
	}
	for (uint plane = 0u; plane < 6u; plane++) {
		bool any_in = false;
		for (int c = 0; c < 8; c++) {
			if (dot(p_frustum[plane].xyz, corners[c]) + p_frustum[plane].w >= 0.0) {
				any_in = true;
				break;
			}
		}
		if (!any_in) {
			return false;
		}
	}
	return true;
}

#ifndef SHADOW_VIEW
bool skin_aabb_hiz_occluded(NextGeoInstanceRecord p_record, vec3 p_lmin, vec3 p_lmax) {
	if (params.phase == 0u || (cull.cull.w & 1u) == 0u) {
		return false;
	}
	float disp_bound = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
	vec3 lmin = p_lmin - vec3(disp_bound);
	vec3 lmax = p_lmax + vec3(disp_bound);
	vec2 uv_min = vec2(2.0);
	vec2 uv_max = vec2(-1.0);
	float nearest_depth = 0.0;
	for (int c = 0; c < 8; c++) {
		vec3 lp = vec3((c & 1) != 0 ? lmax.x : lmin.x, (c & 2) != 0 ? lmax.y : lmin.y, (c & 4) != 0 ? lmax.z : lmin.z);
		vec3 wp = instance_point(p_record, lp);
		vec4 clip = cull.view_proj * vec4(wp, 1.0);
		if (clip.w <= 0.0) {
			return false;
		}
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		uv_min = min(uv_min, uv);
		uv_max = max(uv_max, uv);
		nearest_depth = max(nearest_depth, clip.z / clip.w);
	}
	if (uv_min.x < 0.0 || uv_min.y < 0.0 || uv_max.x > 1.0 || uv_max.y > 1.0) {
		return false;
	}
	vec2 sz = (uv_max - uv_min) * cull.view.xy * 0.5;
	float mip = max(floor(log2(max(max(sz.x, sz.y), 1.0))), 0.0);
	float occ = 1.0;
	for (int tap = 0; tap < 4; tap++) {
		vec2 uv = vec2((tap & 1) != 0 ? uv_max.x : uv_min.x, (tap & 2) != 0 ? uv_max.y : uv_min.y);
		occ = min(occ, textureLod(hiz_prev, uv, mip).r);
	}
	return nearest_depth < occ;
}
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
	// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152). Inflate cluster culling sphere by displacement, WPO, and wind sway bounds.
	float expand = max(p_record.prev_world_aabb_max.w, 0.0) + nextgeo_record_vertex_offset_bound(p_record) + nextgeo_wind_mesh_max_sway(p_record.virtual_mesh);
	float z_min = dot(center - cull.camera.xyz, cull.raster_forward.xyz) - (p_bounds.w + expand) * p_scale;
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
		// GodotNext: nextgeo_mesh_raster (#154). Indirect mesh-task X group count (1 per 32 clusters).
		uint mesh_word = nextgeo_raster_args_word(params.phase, NEXTGEO_RASTER_BIN_FIXED, NEXTGEO_RASTER_ARGS_HW_MESH);
		atomicMax(raster_args.data[mesh_word + 0u], (slot >> 5u) + 1u);
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
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	float inst_bone_scale = is_skinned ? nextgeo_skin_instance_max_scale(entry.x) : 1.0;
	if (projected_error_params_skinned(record, parent_lod, parent_error, inst_bone_scale, is_skinned, sc.camera, sc.view, sc.view_proj, sc.cull.w) <= sc.camera.w) {
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
		vec3 skin_lmin = vec3(0.0);
		vec3 skin_lmax = vec3(0.0);
		float cluster_bone_scale = inst_bone_scale;
		bool has_skin_bounds = false;
		vec4 skin_sphere = vec4(0.0);
		if (is_skinned && global_cluster >= mesh.cluster_offset) {
			uint lc = global_cluster - mesh.cluster_offset;
			if (lc < mesh.cluster_count) {
				has_skin_bounds = nextgeo_skin_cluster_bounds(entry.x, mesh.page_table_offset + lc, skin_lmin, skin_lmax, cluster_bone_scale);
				if (has_skin_bounds) {
					if (!skin_aabb_visible_planes(record, skin_lmin, skin_lmax, sc.frustum)) {
						continue;
					}
					skin_sphere = vec4(0.5 * (skin_lmin + skin_lmax), 0.5 * length(skin_lmax - skin_lmin));
				}
			}
		}
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
					float child_px_err = has_skin_bounds
							? projected_error_params_skinned(record, skin_sphere, own_error, cluster_bone_scale, false, sc.camera, sc.view, sc.view_proj, sc.cull.w)
							: projected_error_params_skinned(record, own_lod, own_error, inst_bone_scale, is_skinned, sc.camera, sc.view, sc.view_proj, sc.cull.w);
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
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	float inst_bone_scale = is_skinned ? nextgeo_skin_instance_max_scale(entry.x) : 1.0;
	if (projected_error_skinned(record, parent_lod, parent_error, inst_bone_scale, is_skinned) <= cull.camera.w) {
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

	// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10). Swap a voxelized group for its
	// sparse 4^3 bricks when a brick projects <= V px and the group's error test passes.
	bool voxels_swapped = false;
	bool voxels_on = hybrid_raster && (cull.cull.w & NEXTGEO_CULL_FLAG_VOXELS) != 0u &&
			!is_skinned && (record.flags & NEXTGEO_RECORD_HAS_SPLINE) == 0u &&
			page_slot != 0xFFFFFFFFu && slot_page_size >= 96u && record.surface_mask != 0u;
	if (voxels_on) {
		uint page_word_base = (page_slot * slot_page_size) >> 2u;
		uint page_words = slot_page_size >> 2u;
		uint voxel_rel_word = pages.data[page_word_base + 3u];
		if (voxel_rel_word >= 4u && voxel_rel_word + 4u <= page_words) {
			uint voxel_base = page_word_base + voxel_rel_word;
			if (pages.data[voxel_base + 0u] == NEXTGEO_VOXEL_SECTION_MAGIC) {
				uint first_g = pages.data[voxel_base + 1u];
				uint page_g_count = pages.data[voxel_base + 2u];
				if (entry.y >= first_g && (entry.y - first_g) < page_g_count &&
						voxel_rel_word + 4u + (entry.y - first_g + 1u) * 4u <= page_words) {
					uint dir_word = voxel_base + 4u + (entry.y - first_g) * 4u;
					uint brick_rel_word = pages.data[dir_word + 0u];
					uint brick_count = pages.data[dir_word + 1u];
					float voxel_size = uintBitsToFloat(pages.data[dir_word + 2u]);
					float voxel_thresh_px = uintBitsToFloat(cull.raster_bin_tess_mask.z);
					uint phase_cap = cull.raster_bin_tess_mask.w >> 1u;
					if (brick_count > 0u && voxel_size > 0.0 && voxel_thresh_px > 0.0 && phase_cap > 0u) {
						vec3 center_w = instance_point(record, parent_lod.xyz);
						float brick_world = float(NEXTGEO_VOXEL_BRICK_DIM) * voxel_size * route_scale;
						float brick_px = 1e30;
						if (cull.raster_params.z != 0.0) {
							brick_px = brick_world * cull.raster_params.y;
						} else {
							float z_min = dot(center_w - cull.camera.xyz, cull.raster_forward.xyz) - parent_lod.w * route_scale;
							if (z_min > max(cull.raster_forward.w, 1e-4)) {
								brick_px = brick_world * cull.raster_params.y / z_min;
							}
						}
						if (brick_px <= voxel_thresh_px) {
							bool group_error_passes = true;
							for (uint c = 0u; c < cluster_count; c++) {
								uint gc = cluster_offset + c;
								if (page_cluster_hdr > 0u && gc >= mesh.cluster_offset) {
									uint lc = gc - mesh.cluster_offset;
									if (lc >= page_first_cluster && (lc - page_first_cluster) < page_cluster_hdr) {
										uint rec_word = page_word_base + 4u + (lc - page_first_cluster) * 20u;
										int refined_group = int(pages.data[rec_word + 15u]);
										if (refined_group >= 0 && uint(refined_group) < group_count) {
											vec4 own_lod = vec4(
													uintBitsToFloat(pages.data[rec_word + 5u]),
													uintBitsToFloat(pages.data[rec_word + 6u]),
													uintBitsToFloat(pages.data[rec_word + 7u]),
													uintBitsToFloat(pages.data[rec_word + 8u]));
											float own_error = uintBitsToFloat(pages.data[rec_word + 9u]);
											float child_px_err = projected_error_skinned(record, own_lod, own_error, 1.0, false);
											if (child_px_err > cull.camera.w && mesh_group_refine_ok(mesh_idx, uint(refined_group))) {
												group_error_passes = false;
												break;
											}
										}
									}
								}
							}
							if (group_error_passes) {
								uint phase_base = params.phase == 0u ? 0u : phase_cap;
								uint arg_base = params.phase * 4u;
								uint cursor = voxel_base + brick_rel_word;
								for (uint b = 0u; b < brick_count; b++) {
									uint meta = pages.data[cursor + 6u];
									uint occ_count = meta & 0xFFu;
									uint vslot = atomicAdd(counters.voxel_bricks[params.phase], 1u);
									if (vslot < phase_cap) {
										visible_voxel_bricks.data[phase_base + vslot] = uvec4(entry.x, cursor, entry.y, meta);
										atomicMax(voxel_raster_args.data[arg_base + 0u], min(vslot + 1u, NEXTGEO_SW_DISPATCH_WIDTH));
										atomicMax(voxel_raster_args.data[arg_base + 1u], vslot / NEXTGEO_SW_DISPATCH_WIDTH + 1u);
									} else {
										atomicAdd(counters.overflow, 1u);
										break;
									}
									cursor += NEXTGEO_VOXEL_BRICK_HEADER_WORDS + occ_count * NEXTGEO_VOXEL_DATA_WORDS;
								}
								emitted_any = true;
								voxels_swapped = true;
							}
						}
					}
				}
			}
		}
	}

	for (uint cluster = 0u; !voxels_swapped && cluster < cluster_count; cluster++) {
		uint global_cluster = cluster_offset + cluster;
		vec3 skin_lmin = vec3(0.0);
		vec3 skin_lmax = vec3(0.0);
		float cluster_bone_scale = inst_bone_scale;
		bool has_skin_bounds = false;
		vec4 skin_sphere = vec4(0.0);
		if (is_skinned && global_cluster >= mesh.cluster_offset) {
			uint lc = global_cluster - mesh.cluster_offset;
			if (lc < mesh.cluster_count) {
				has_skin_bounds = nextgeo_skin_cluster_bounds(entry.x, mesh.page_table_offset + lc, skin_lmin, skin_lmax, cluster_bone_scale);
				if (has_skin_bounds) {
					if (!skin_aabb_visible_planes(record, skin_lmin, skin_lmax, cull.frustum)) {
						continue;
					}
					if (skin_aabb_hiz_occluded(record, skin_lmin, skin_lmax)) {
						continue;
					}
					skin_sphere = vec4(0.5 * (skin_lmin + skin_lmax), 0.5 * length(skin_lmax - skin_lmin));
				}
			}
		}
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
					float child_px_err = has_skin_bounds
							? projected_error_skinned(record, skin_sphere, own_error, cluster_bone_scale, false)
							: projected_error_skinned(record, own_lod, own_error, inst_bone_scale, is_skinned);
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
								vec4 req_lod = has_skin_bounds ? skin_sphere : own_lod;
								if ((record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
									spline_deform_bounds_sphere(spline_deforms.data[record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current, req_lod.xyz, req_lod.w);
								}
								vec3 center = instance_point(record, req_lod.xyz);
								// GodotNext: nextgeo_displacement (#150), nextgeo_vertex_offset (#164), nextgeo_foliage_wind (#152). Inflate sphere radius by displacement, WPO, and wind sway bounds.
								float expand = max(record.prev_world_aabb_max.w, 0.0) + nextgeo_record_vertex_offset_bound(record) + nextgeo_wind_mesh_max_sway(record.virtual_mesh);
								float dist = max(length(center - cull.camera.xyz) - (req_lod.w + expand) * scale, 0.0);
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
				// GodotNext: spline_meshes (#161). Deform cluster sphere and scale edge length by max spline stretch.
				if ((record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
					SplineDeformPose sp = spline_deforms.data[record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current;
					spline_deform_bounds_sphere(sp, bounds.xyz, bounds.w);
					max_edge *= sp.up_max_stretch.w;
				}
				if (has_skin_bounds) {
					software = sw_raster_route(record, route_scale, skin_sphere, max_edge * cluster_bone_scale);
				} else if (!is_skinned) {
					software = sw_raster_route(record, route_scale, bounds, max_edge);
				}
			}
			uint bin = NEXTGEO_RASTER_BIN_FIXED;
			if ((cull.cull.w & NEXTGEO_CULL_FLAG_PROGRAMMABLE_BINS) != 0u) {
				bin = raster_bin_of(record, mesh, global_cluster);
				// GodotNext: nextgeo_masked_two_sided (#153), nextgeo_vertex_offset (#164). Past
				// programmable_raster_distance / vertex_offset_disable_distance, demote non-displaced
				// programmable bins to FIXED once all active programmable features are disabled.
				if (bin > NEXTGEO_RASTER_BIN_FIXED && bin < NEXTGEO_RASTER_BIN_LIMIT) {
					uint demote_bit = bin - 1u;
					uint bit_mask = 1u << (demote_bit & 31u);
					uint pix_word = demote_bit < 32u ? cull.raster_bin_demote_mask.x : cull.raster_bin_demote_mask.y;
					uint wpo_word = demote_bit < 32u ? cull.raster_bin_demote_mask.z : cull.raster_bin_demote_mask.w;
					bool has_pix = (pix_word & bit_mask) != 0u;
					bool has_wpo = (wpo_word & bit_mask) != 0u;
					if (has_pix || has_wpo) {
						float prog_dist = record.prev_world_aabb_min.w;
						float wpo_dist = nextgeo_record_vertex_offset_disable_distance(record);
						if ((has_pix && prog_dist > 0.0) || (has_wpo && wpo_dist > 0.0)) {
							vec3 closest = clamp(cull.camera.xyz, record.world_aabb_min.xyz, record.world_aabb_max.xyz);
							float cam_dist = length(cull.camera.xyz - closest);
							bool pix_demoted = has_pix && prog_dist > 0.0 && cam_dist > prog_dist;
							bool wpo_demoted = has_wpo && wpo_dist > 0.0 && cam_dist > wpo_dist;
							if ((!has_pix || pix_demoted) && (!has_wpo || wpo_demoted)) {
								bin = NEXTGEO_RASTER_BIN_FIXED;
								if (pix_demoted) {
									atomicAdd(counters.raster_distance_fixed[params.phase], 1u);
								}
								if (wpo_demoted) {
									atomicAdd(counters.vertex_offset_distance_fixed[params.phase], 1u);
								}
							}
						}
					}
				}
			}
			if (bin == NEXTGEO_RASTER_BIN_FIXED) {
				raster_route(params.list_base + slot, software);
			} else {
				// GodotNext: nextgeo_tessellation (#163). When tessellation is active, route displaced
				// bins into their SW dicing worklist so tessellation_dice.glsl dices and splits them.
				if ((cull.cull.w & NEXTGEO_CULL_FLAG_TESSELLATION) != 0u && bin < NEXTGEO_RASTER_BIN_LIMIT) {
					uint tess_bit = bin - 1u;
					uint tess_word = tess_bit < 32u ? cull.raster_bin_tess_mask.x : cull.raster_bin_tess_mask.y;
					if ((tess_word & (1u << (tess_bit & 31u))) != 0u) {
						software = true;
					}
				}
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
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	float inst_bone_scale = is_skinned ? skin_instance_max_scale(entry.x) : 1.0;
	if (projected_error_params_skinned(record, lod_sphere, max_parent_error, inst_bone_scale, is_skinned, sc.camera, sc.view, sc.view_proj, sc.cull.w) <= sc.camera.w) {
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
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	float inst_bone_scale = is_skinned ? skin_instance_max_scale(entry.x) : 1.0;
	if (projected_error_skinned(record, lod_sphere, max_parent_error, inst_bone_scale, is_skinned) <= cull.camera.w) {
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
#endif // VSM_VIEW
