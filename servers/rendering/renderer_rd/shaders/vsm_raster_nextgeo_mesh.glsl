#[task]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, step 6).
// Task stage for VSM x Nanite fixed-function HW rasterization: 1 32-thread workgroup per
// 32 compacted (cluster, dirty page) pairs, passing their indices to the mesh stage via a
// 128-byte task payload. Uses a dedicated readonly HW count buffer (binding 10) so
// vsm_counters (binding 9) stays writable-only in the fragment stage across Vulkan and D3D12.

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushParams {
	uint cluster_capacity;
	uint pad0;
	uint pad1;
	uint pad2;
}
push_params;

// Hardware pair indices grow down from cluster_capacity - 1 (shared convention with #162/#217).
layout(set = 0, binding = 5, std430) buffer restrict readonly VSMRasterLists {
	uint data[];
}
vsm_raster_lists;

layout(set = 0, binding = 10, std430) buffer restrict readonly VSMHWCount {
	uint hw_clusters;
}
vsm_hw_count;

struct TaskPayload {
	uint pairs[32];
};

taskPayloadSharedEXT TaskPayload payload;

void main() {
	uint base = gl_WorkGroupID.x * 32u;
	uint hw_count = min(vsm_hw_count.hw_clusters, push_params.cluster_capacity);
	uint batch_count = (base < hw_count) ? min(hw_count - base, 32u) : 0u;
	if (gl_LocalInvocationIndex < batch_count) {
		payload.pairs[gl_LocalInvocationIndex] = vsm_raster_lists.data[push_params.cluster_capacity - 1u - (base + gl_LocalInvocationIndex)];
	}
	barrier();
	EmitMeshTasksEXT(batch_count, 1u, 1u);
}

#[mesh]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, step 6).
// Mesh stage for VSM x Nanite fixed-function HW rasterization: one 128-thread workgroup per
// (cluster, dirty page) pair, projecting cluster vertices once into the target 64x64 VSM page
// and feeding the same 32-bit imageAtomicMax pixel stage as vsm_raster_nextgeo.glsl.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 128, max_primitives = 128) out;

// clang-format off
#include "spline_deform_inc.glsl"
#include "vsm_mark_inc.glsl"
#include "vsm_raster_inc.glsl"
#include "nextgeo/nextgeo_records_inc.glsl"
// clang-format on

struct TaskPayload {
	uint pairs[32];
};

taskPayloadSharedEXT TaskPayload payload;

layout(location = 0) perprimitiveEXT out uint vsm_page_info_out[];
layout(location = 1) out float vsm_depth_out[];

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

layout(set = 0, binding = 4, std430) buffer restrict readonly VSMClusters {
	uvec4 data[];
}
vsm_clusters;

layout(set = 0, binding = 6, std430) buffer restrict readonly VSMMaps {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 7, std430) buffer restrict readonly VSMDirty {
	uvec4 data[];
}
vsm_dirty;

// GodotNext: spline_meshes (#161).
layout(set = 0, binding = 11, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;

shared vec4 s_clip_pos[128];

void main() {
	uint pair_idx = payload.pairs[gl_WorkGroupID.x];
	if (pair_idx >= push_params.cluster_capacity) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}
	uvec4 pair = vsm_clusters.data[pair_idx];
	NextGeoInstanceRecord record = instances.data[pair.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || (record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}

	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || pair.y < mesh.cluster_offset) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}
	uint local_cluster = pair.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}

	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = desc.z & 0xFFu;
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	uint surface_id = desc.w & 0xFFFFu;
	if (stride < 3u || vert_count == 0u || vert_count > 128u || tri_count == 0u || tri_count > 128u || surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}

	uvec4 rec = vsm_dirty.data[pair.z];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	VSMMapData md = vsm_maps.data[map];

	SetMeshOutputsEXT(vert_count, tri_count);

	bool has_spline = (record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u;
	uint tid = gl_LocalInvocationIndex;
	if (tid < vert_count) {
		uint pos_word = desc.x + tid * stride;
		vec3 local_pos = vec3(
				uintBitsToFloat(pages.data[pos_word + 0u]),
				uintBitsToFloat(pages.data[pos_word + 1u]),
				uintBitsToFloat(pages.data[pos_word + 2u]));
		if (has_spline) {
			local_pos = spline_deform_position(spline_deforms.data[record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current, local_pos);
		}
		vec3 world_pos = vec3(
				dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
				dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
				dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
		float vsm_depth = 0.0;
		vec4 clip_pos = vsm_page_clip_position(md, mip, slot, world_pos, vsm_depth);
		gl_MeshVerticesEXT[tid].gl_Position = clip_pos;
		vsm_depth_out[tid] = vsm_depth;
		s_clip_pos[tid] = clip_pos;
	}

	barrier();

	bool cull_disabled = ((record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_DOUBLE_SIDED) || ((record.pad1[0] & (1u << surface_id)) != 0u);
	bool cull_front = (record.pad1[1] & (1u << surface_id)) != 0u;
	float map_cull_sign = ((pair.w & 2u) != 0u) ? 1.0 : -1.0;
	float effective_cull_sign = cull_front ? -map_cull_sign : map_cull_sign;
	mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
	bool flip_cluster = determinant(basis) * effective_cull_sign < 0.0;
	uint page_info = (rec.z & 0x00FFFFFFu) | ((pair.w & 1u) << 24u);

	if (tid < tri_count) {
		uint ib0 = desc.y + tid * 3u + 0u;
		uint ib1 = desc.y + tid * 3u + 1u;
		uint ib2 = desc.y + tid * 3u + 2u;
		uint i0 = (pages.data[ib0 >> 2u] >> ((ib0 & 3u) * 8u)) & 0xFFu;
		uint i1 = (pages.data[ib1 >> 2u] >> ((ib1 & 3u) * 8u)) & 0xFFu;
		uint i2 = (pages.data[ib2 >> 2u] >> ((ib2 & 3u) * 8u)) & 0xFFu;
		if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count) {
			gl_PrimitiveTriangleIndicesEXT[tid] = uvec3(0u, 0u, 0u);
			vsm_page_info_out[tid] = 0xFFFFFFFFu;
		} else {
			bool flip = cull_disabled ? (determinant(mat3(s_clip_pos[i0].xyw, s_clip_pos[i1].xyw, s_clip_pos[i2].xyw)) < 0.0) : flip_cluster;
			gl_PrimitiveTriangleIndicesEXT[tid] = flip ? uvec3(i0, i2, i1) : uvec3(i0, i1, i2);
			vsm_page_info_out[tid] = page_info;
		}
	}
}

#[fragment]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// clang-format off
#include "vsm_mark_inc.glsl"
#include "vsm_raster_inc.glsl"
// clang-format on

layout(location = 0) perprimitiveEXT flat in uint vsm_page_info_in;
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
