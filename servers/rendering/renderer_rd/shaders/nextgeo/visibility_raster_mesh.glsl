#[task]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10).
// Task stage for the fixed-function HW visibility bin: 1 32-thread workgroup per 32 compacted
// HW clusters, packing their visible-cluster indices into a 128-byte task payload and emitting
// one 128-thread mesh workgroup per cluster.

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushParams {
	uint list_base;
	uint region_size;
	uint target_width;
	uint phase;
}
push_params;

// Hardware entries grow down from the top of the phase's region (raster_features_inc.glsl).
layout(set = 0, binding = 7, std430) buffer restrict readonly RasterLists {
	uint data[];
}
raster_lists;

// CutCounters as uint words: raster_hw[0] sits at word 33, raster_hw[1] at word 34.
layout(set = 0, binding = 9, std430) buffer restrict readonly Counters {
	uint data[];
}
counters;

const uint NEXTGEO_COUNTER_RASTER_HW_BASE = 33u;

struct TaskPayload {
	uint clusters[32];
};

taskPayloadSharedEXT TaskPayload payload;

void main() {
	uint base = gl_WorkGroupID.x * 32u;
	uint hw_count = min(counters.data[NEXTGEO_COUNTER_RASTER_HW_BASE + (push_params.phase & 1u)], push_params.region_size);
	uint batch_count = (base < hw_count) ? min(hw_count - base, 32u) : 0u;
	if (gl_LocalInvocationIndex < batch_count) {
		payload.clusters[gl_LocalInvocationIndex] = raster_lists.data[push_params.list_base + push_params.region_size - 1u - (base + gl_LocalInvocationIndex)];
	}
	barrier();
	EmitMeshTasksEXT(batch_count, 1u, 1u);
}

#[mesh]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10).
// Mesh stage for the fixed-function HW visibility bin: one 128-thread workgroup per cluster
// (<= 128 vertices, <= 128 triangles, §7.1 cap), transforming each cluster vertex once,
// matching visibility_raster.glsl's winding/culling, and outputting per-primitive vis_id_out
// to the same VIS64 image/buffer pixel shader.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 128, max_primitives = 128) out;

// clang-format off
#include "nextgeo_records_inc.glsl"
// clang-format on

struct TaskPayload {
	uint clusters[32];
};

taskPayloadSharedEXT TaskPayload payload;

layout(location = 0) perprimitiveEXT out uint vis_id_out[];

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

layout(set = 0, binding = 4, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

layout(set = 0, binding = 5, std430) buffer restrict readonly RasterParams {
	mat4 proj;
	mat4 view;
	mat4 prev_proj;
	mat4 prev_view;
	vec2 taa_jitter;
	vec2 prev_taa_jitter;
	uint visible_capacity;
	uint reset_velocity;
	float cull_sign;
	uint bucket_table_words;
}
raster;

layout(set = 0, binding = 6, std430) buffer restrict readonly BucketTable {
	uint data[];
}
bucket_table;

shared vec4 s_clip_pos[128];

void main() {
	uint vis_cluster = payload.clusters[gl_WorkGroupID.x];
	if (vis_cluster >= raster.visible_capacity || vis_cluster >= 0x1FFFFFFu) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}

	uvec2 entry = visible_clusters.data[vis_cluster];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}

	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || entry.y < mesh.cluster_offset) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}
	uint local_cluster = entry.y - mesh.cluster_offset;
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

	uint bucket_flags = 0u;
	if (raster.bucket_table_words > 0u) {
		uint bucket_slot = record.bucket_base + surface_id;
		if (bucket_slot >= raster.bucket_table_words) {
			SetMeshOutputsEXT(0u, 0u);
			return;
		}
		uint bucket_word = bucket_table.data[bucket_slot];
		if ((bucket_word & NEXTGEO_BUCKET_ID_MASK) == NEXTGEO_BUCKET_INVALID) {
			SetMeshOutputsEXT(0u, 0u);
			return;
		}
		bucket_flags = bucket_word;
	}

	SetMeshOutputsEXT(vert_count, tri_count);

	uint tid = gl_LocalInvocationIndex;
	if (tid < vert_count) {
		uint pos_word = desc.x + tid * stride;
		vec3 local_pos = vec3(
				uintBitsToFloat(pages.data[pos_word + 0u]),
				uintBitsToFloat(pages.data[pos_word + 1u]),
				uintBitsToFloat(pages.data[pos_word + 2u]));
		vec3 world_pos = vec3(
				dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
				dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
				dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
		vec3 view_pos = (raster.view * vec4(world_pos, 1.0)).xyz;
		vec4 cur_clip = raster.proj * vec4(view_pos, 1.0);
		gl_MeshVerticesEXT[tid].gl_Position = cur_clip;
		s_clip_pos[tid] = cur_clip;
	}

	barrier();

	bool cull_disabled = (bucket_flags & NEXTGEO_BUCKET_CULL_DISABLED) != 0u;
	float effective_cull_sign = ((bucket_flags & NEXTGEO_BUCKET_CULL_FRONT) != 0u) ? -raster.cull_sign : raster.cull_sign;
	mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
	bool flip_cluster = determinant(basis) * effective_cull_sign < 0.0;

	if (tid < tri_count) {
		uint ib0 = desc.y + tid * 3u + 0u;
		uint ib1 = desc.y + tid * 3u + 1u;
		uint ib2 = desc.y + tid * 3u + 2u;
		uint i0 = (pages.data[ib0 >> 2u] >> ((ib0 & 3u) * 8u)) & 0xFFu;
		uint i1 = (pages.data[ib1 >> 2u] >> ((ib1 & 3u) * 8u)) & 0xFFu;
		uint i2 = (pages.data[ib2 >> 2u] >> ((ib2 & 3u) * 8u)) & 0xFFu;
		if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count) {
			gl_PrimitiveTriangleIndicesEXT[tid] = uvec3(0u, 0u, 0u);
			vis_id_out[tid] = 0xFFFFFFFFu;
		} else {
			bool flip = false;
#ifndef DEBUG_BACKFACE
			if (cull_disabled) {
				flip = determinant(mat3(s_clip_pos[i0].xyw, s_clip_pos[i1].xyw, s_clip_pos[i2].xyw)) < 0.0;
			} else {
				flip = flip_cluster;
			}
#endif
			gl_PrimitiveTriangleIndicesEXT[tid] = flip ? uvec3(i0, i2, i1) : uvec3(i0, i1, i2);
			vis_id_out[tid] = (vis_cluster << 7u) | (tid & 0x7Fu);
		}
	}
}

#[fragment]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif

layout(location = 0) perprimitiveEXT flat in uint vis_id_in;

layout(push_constant, std430) uniform PushParams {
	uint list_base;
	uint region_size;
	uint target_width;
	uint phase;
}
push_params;

#ifdef VIS64_BUFFER
layout(set = 0, binding = 8, std430) buffer restrict Vis64 {
	uint64_t data[];
}
vis64;
#else
layout(set = 0, binding = 8, r64ui) uniform restrict u64image2D vis64;
#endif

void main() {
	// Depth 0 is the far plane and the empty texel.
	if (!(gl_FragCoord.z > 0.0) || vis_id_in == 0xFFFFFFFFu) {
		return;
	}
	uint64_t value = (uint64_t(floatBitsToUint(min(gl_FragCoord.z, 1.0))) << 32u) | uint64_t(vis_id_in);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
#ifdef VIS64_BUFFER
	atomicMax(vis64.data[uint(pixel.y) * push_params.target_width + uint(pixel.x)], value);
#else
	imageAtomicMax(vis64, pixel, value);
#endif
}
