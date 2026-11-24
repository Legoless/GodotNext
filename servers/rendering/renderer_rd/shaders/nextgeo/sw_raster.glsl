#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N2). Built only on the Nanite device tier (N1): every
// variant uses 64-bit atomics or 64-bit image texels.
//   (none)        Software rasterizer, one 64-thread workgroup per software cluster: the threads pull
//                 and project the cluster's vertices into shared memory, then each rasterizes its
//                 triangles with the shared setup and writes atomicMax(vis64, depth << 32 | ID).
//   CLEAR_VIS64:  Clears the visibility buffer to 0 (empty) once per frame.
// VIS64_BUFFER selects the buffer form of the visibility buffer, for devices whose images lack 64-bit
// atomics; otherwise it is an R64_UINT image.

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#ifdef CLEAR_VIS64

#ifdef VIS64_BUFFER
layout(set = 0, binding = 0, std430) buffer restrict writeonly Vis64 {
	uint64_t data[];
}
vis64;
#else
layout(set = 0, binding = 0, r64ui) uniform restrict writeonly u64image2D vis64;
#endif

layout(push_constant, std430) uniform Params {
	uvec2 size;
	uvec2 pad;
}
params;

// Dispatched as (ceil(width / 64), height, 1).
void main() {
	uvec2 coord = uvec2(gl_GlobalInvocationID.x, gl_WorkGroupID.y);
	if (coord.x >= params.size.x || coord.y >= params.size.y) {
		return;
	}
#ifdef VIS64_BUFFER
	vis64.data[coord.y * params.size.x + coord.x] = uint64_t(0);
#else
	imageStore(vis64, ivec2(coord), u64vec4(uint64_t(0)));
#endif
}

#else

// clang-format off
#include "../spline_deform_inc.glsl"
#include "nextgeo_records_inc.glsl"
#include "raster_features_inc.glsl"
#include "sw_raster_setup_inc.glsl"
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

layout(set = 0, binding = 4, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

// The prefix of NextGeoCut's RasterParams that the vertex shader of visibility_raster.glsl reads.
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

layout(set = 0, binding = 7, std430) buffer restrict readonly RasterLists {
	uint data[];
}
raster_lists;

// NextGeoCut's CutCounters as words; NEXTGEO_COUNTER_* below are word indices.
layout(set = 0, binding = 8, std430) buffer restrict Counters {
	uint data[];
}
counters;

#ifdef VIS64_BUFFER
layout(set = 0, binding = 9, std430) buffer restrict Vis64 {
	uint64_t data[];
}
vis64;
#else
layout(set = 0, binding = 9, r64ui) uniform restrict u64image2D vis64;
#endif

// GodotNext: spline_meshes (#161).
layout(set = 0, binding = 10, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;

const uint NEXTGEO_COUNTER_RASTER_SW = 31u; // raster_sw[2]
const uint NEXTGEO_COUNTER_RASTER_SW_WRITES = 35u;

const uint SW_FLAG_NO_CULL = 1u << 0u; // The back-face debug view rasterizes both faces, as its HW pipeline does.
const uint SW_FLAG_STATS = 1u << 1u; // Count the samples the software rasterizer wrote.

layout(push_constant, std430) uniform Params {
	uint list_base; // First entry of the phase's region in raster_lists.
	uint phase;
	uint flags;
	uint pad;
	ivec2 size;
	uvec2 pad2;
}
params;

// Projected vertices: xy = framebuffer pixel, z = depth, w = 1 when the vertex is in front of the
// camera. NextGeo clusters hold at most 128 vertices; the vertex count is a byte.
shared vec4 s_vertices[256];
shared uint s_writes;

// The result of a 64-bit atomic max is never read: Metal's 64-bit max returns nothing (#175).
void vis64_max(ivec2 p_pixel, uint64_t p_value) {
#ifdef VIS64_BUFFER
	atomicMax(vis64.data[uint(p_pixel.y) * uint(params.size.x) + uint(p_pixel.x)], p_value);
#else
	imageAtomicMax(vis64, p_pixel, p_value);
#endif
}

void main() {
	uint local_cluster_index = gl_WorkGroupID.y * NEXTGEO_SW_DISPATCH_WIDTH + gl_WorkGroupID.x;
	// Every early return below depends only on the workgroup's cluster, so the whole workgroup takes it.
	if (local_cluster_index >= counters.data[NEXTGEO_COUNTER_RASTER_SW + params.phase]) {
		return;
	}
	uint vis_cluster = raster_lists.data[params.list_base + local_cluster_index];
	if (vis_cluster >= raster.visible_capacity || vis_cluster >= 0x1FFFFFFu) {
		return;
	}

	uvec2 entry = visible_clusters.data[vis_cluster];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || entry.y < mesh.cluster_offset) {
		return;
	}
	uint local_cluster = entry.y - mesh.cluster_offset;
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

	// The same rejections and winding as the vertex shader of visibility_raster.glsl.
	uint bucket_flags = 0u;
	if (raster.bucket_table_words > 0u) {
		uint bucket_slot = record.bucket_base + surface_id;
		if (bucket_slot >= raster.bucket_table_words) {
			return;
		}
		uint bucket_word = bucket_table.data[bucket_slot];
		if ((bucket_word & NEXTGEO_BUCKET_ID_MASK) == NEXTGEO_BUCKET_INVALID) {
			return;
		}
		bucket_flags = bucket_word;
	}
	int facing = NEXTGEO_SW_KEEP_BOTH;
	if ((params.flags & SW_FLAG_NO_CULL) == 0u && (bucket_flags & NEXTGEO_BUCKET_CULL_DISABLED) == 0u) {
		float effective_cull_sign = ((bucket_flags & NEXTGEO_BUCKET_CULL_FRONT) != 0u) ? -raster.cull_sign : raster.cull_sign;
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
	for (uint v = gl_LocalInvocationIndex; v < vert_count; v += 64u) {
		uint pos_word = desc.x + v * stride;
		vec3 local_pos = vec3(
				uintBitsToFloat(pages.data[pos_word + 0u]),
				uintBitsToFloat(pages.data[pos_word + 1u]),
				uintBitsToFloat(pages.data[pos_word + 2u]));
		if (has_spline) {
			local_pos = spline_deform_position(spline_pose, local_pos);
		}
		vec3 world_pos = vec3(
				dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
				dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
				dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
		vec4 clip = raster.proj * (raster.view * vec4(world_pos, 1.0));
		// Cluster cull routes only clusters in front of the near plane here; a vertex behind the
		// camera still never reaches the projection.
		s_vertices[v] = clip.w > 0.0 ? vec4(nextgeo_sw_clip_to_pixel(clip, params.size), 1.0) : vec4(0.0);
	}
	barrier();

	uint writes = 0u;
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
		NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(a.xy, b.xy, c.xy, vec3(a.z, b.z, c.z), params.size, facing);
		if (!tri.valid) {
			continue;
		}
		uint64_t id = uint64_t((vis_cluster << 7u) | (t & 0x7Fu));
		for (int y = tri.rect_min.y; y <= tri.rect_max.y; y++) {
			for (int x = tri.rect_min.x; x <= tri.rect_max.x; x++) {
				float depth;
				// Depth 0 is the far plane, where the hardware clips; it is also the empty texel.
				if (!nextgeo_sw_sample(tri, ivec2(x, y), depth) || !(depth > 0.0)) {
					continue;
				}
				vis64_max(ivec2(x, y), (uint64_t(floatBitsToUint(min(depth, 1.0))) << 32u) | id);
				writes++;
			}
		}
	}

	if ((params.flags & SW_FLAG_STATS) != 0u) {
		if (writes > 0u) {
			atomicAdd(s_writes, writes);
		}
		barrier();
		if (gl_LocalInvocationIndex == 0u && s_writes > 0u) {
			atomicAdd(counters.data[NEXTGEO_COUNTER_RASTER_SW_WRITES], s_writes);
		}
	}
}

#endif
