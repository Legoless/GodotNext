#[task]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10).
// Task stage for a programmable HW raster bin (both main VIS64 and VSM depth-only):
// 1 32-thread workgroup per 32 compacted HW-bin clusters, passing their visible-cluster
// indices through a 128-byte task payload to the mesh stage.

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "raster_features_inc.glsl"
// clang-format on

layout(set = 2, binding = 0, std430) buffer restrict readonly RasterBinLists {
	uint data[];
}
raster_bin_lists;

layout(set = 2, binding = 1, std430) buffer restrict readonly RasterBinState {
	uint data[];
}
raster_bin_state;

layout(push_constant, std430) uniform RasterBinPush {
	uint list_base;
	uint phase;
	uint bin;
	uint flags;
	ivec2 size;
	uvec2 pad;
}
bin_push;

struct TaskPayload {
	uint clusters[32];
};

taskPayloadSharedEXT TaskPayload payload;

void main() {
	uint start = raster_bin_state.data[nextgeo_raster_bin_cursor_word(bin_push.phase, bin_push.bin, NEXTGEO_RASTER_BIN_KIND_HW)];
	uint count = raster_bin_state.data[nextgeo_raster_bin_count_word(bin_push.phase, bin_push.bin, NEXTGEO_RASTER_BIN_KIND_HW)];
	uint base = gl_WorkGroupID.x * 32u;
	uint batch_count = (base < count) ? min(count - base, 32u) : 0u;
	if (gl_LocalInvocationIndex < batch_count) {
		payload.clusters[gl_LocalInvocationIndex] = raster_bin_lists.data[bin_push.list_base + start + base + gl_LocalInvocationIndex];
	}
	barrier();
	EmitMeshTasksEXT(batch_count, 1u, 1u);
}

#[mesh]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10).
// Mesh stage for a programmable HW raster bin: one 128-thread workgroup per cluster
// (<= 128 vertices, <= 128 triangles), running vertex() once per cluster vertex and
// emitting per-primitive visibility IDs to the shared coverage pixel stage.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 128, max_primitives = 128) out;

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_HW_VERTEX
#define NEXTGEO_RASTER_HW_MESH

// clang-format off
#include "raster_material_inc.glsl"
// clang-format on

struct TaskPayload {
	uint clusters[32];
};

taskPayloadSharedEXT TaskPayload payload;

layout(location = 0) perprimitiveEXT out uint vis_id_out[];

const uint RASTER_BIN_FLAG_NO_CULL = 1u << 0u; // The back-face debug view rasterizes both faces.

shared vec4 s_clip_pos[128];

void main() {
	uint vis_cluster = payload.clusters[gl_WorkGroupID.x];
	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(vis_cluster, entry, desc, cull_flags)) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	if (vert_count == 0u || vert_count > 128u || tri_count == 0u || tri_count > 128u) {
		SetMeshOutputsEXT(0u, 0u);
		return;
	}

	SetMeshOutputsEXT(vert_count, tri_count);

	uint tid = gl_LocalInvocationIndex;
	if (tid < vert_count) {
		vec4 clip_pos = vec4(0.0, 0.0, -2.0, 1.0);
		_ng_raster_eval_vertex(entry, desc, tid, clip_pos);
		gl_MeshVerticesEXT[tid].gl_Position = clip_pos;
		s_clip_pos[tid] = clip_pos;
	}

	barrier();

	bool no_cull = (bin_push.flags & RASTER_BIN_FLAG_NO_CULL) != 0u || (cull_flags & NEXTGEO_BUCKET_CULL_DISABLED) != 0u;
	NextGeoInstanceRecord record = nextgeo_records.data[entry.x];
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	float base_cull_sign = _ng_vsm_reverse_cull ? 1.0 : -1.0;
#else
	float base_cull_sign = raster.cull_sign;
#endif
	float effective_cull_sign = ((cull_flags & NEXTGEO_BUCKET_CULL_FRONT) != 0u) ? -base_cull_sign : base_cull_sign;
	mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
	bool flip_cluster = determinant(basis) * effective_cull_sign < 0.0;

	if (tid < tri_count) {
		uint ib0 = desc.y + tid * 3u + 0u;
		uint ib1 = desc.y + tid * 3u + 1u;
		uint ib2 = desc.y + tid * 3u + 2u;
		uint i0 = (nextgeo_pages.data[ib0 >> 2u] >> ((ib0 & 3u) * 8u)) & 0xFFu;
		uint i1 = (nextgeo_pages.data[ib1 >> 2u] >> ((ib1 & 3u) * 8u)) & 0xFFu;
		uint i2 = (nextgeo_pages.data[ib2 >> 2u] >> ((ib2 & 3u) * 8u)) & 0xFFu;
		if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count) {
			gl_PrimitiveTriangleIndicesEXT[tid] = uvec3(0u, 0u, 0u);
			vis_id_out[tid] = 0xFFFFFFFFu;
		} else {
			bool flip = no_cull ? (determinant(mat3(s_clip_pos[i0].xyw, s_clip_pos[i1].xyw, s_clip_pos[i2].xyw)) < 0.0) : flip_cluster;
			gl_PrimitiveTriangleIndicesEXT[tid] = flip ? uvec3(i0, i2, i1) : uvec3(i0, i1, i2);
			vis_id_out[tid] = (vis_cluster << 7u) | (tid & 0x7Fu);
		}
	}
}

#[fragment]

#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

#ifndef NEXTGEO_RASTER_DEPTH_ONLY
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif
#endif

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_HW_FRAGMENT

// clang-format off
#include "raster_material_inc.glsl"
// clang-format on

layout(location = 0) perprimitiveEXT flat in uint vis_id_in;

void main() {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	if (gl_HelperInvocation || vis_id_in == 0xFFFFFFFFu) {
		return;
	}
#else
	// Depth 0 is the far plane and the empty texel.
	if (!(gl_FragCoord.z > 0.0) || vis_id_in == 0xFFFFFFFFu) {
		return;
	}
#endif
	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(vis_id_in >> 7u, entry, desc, cull_flags) || !_ng_raster_load_triangle(entry, desc, vis_id_in & 0x7Fu)) {
		return;
	}
	_ng_raster_interpolate(gl_FragCoord.xy);
	_ng_raster_fragment(scene_data_block.data);
	if (!_ng_raster_covered) {
		return;
	}
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	_ng_raster_write(ivec2(gl_FragCoord.xy), _ng_vsm_depth_interp, vis_id_in);
#else
	_ng_raster_write(ivec2(gl_FragCoord.xy), gl_FragCoord.z, vis_id_in);
#endif
}
