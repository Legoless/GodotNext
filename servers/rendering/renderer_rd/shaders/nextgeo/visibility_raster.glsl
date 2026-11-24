#[vertex]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_visibility_raster (#83, plan §6.1, §6.3, §7.4).
// Draw path (c): one indirect instanced draw per phase over the compacted
// visible-cluster list, pulling cluster vertices from the page arena.
// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N2). MODE_VIS64 is the hybrid
// raster's hardware path: an indirect-count draw over the phase's hardware list,
// no attachments, and a pixel shader that writes atomicMax(vis64, depth << 32 | ID).

// clang-format off
#include "../spline_deform_inc.glsl"
#include "nextgeo_records_inc.glsl"
// clang-format on

#ifdef MODE_SHADOW
#ifdef MODE_DUAL_PARABOLOID
layout(location = 0) out float dp_clip_out;
#endif
#else
layout(location = 0) out flat uint vis_id_out;
#ifndef MODE_VIS64
layout(location = 1) out flat uint instance_id_out;
#endif
#ifdef WITH_VELOCITY
layout(location = 2) out vec4 screen_position;
layout(location = 3) out vec4 prev_screen_position;
#endif
#endif

// MODE_VIS64: pad0 = the phase region's size in raster_lists, pad1 = the target width.
layout(push_constant, std430) uniform PushParams {
	uint list_base;
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

layout(set = 0, binding = 4, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

#ifdef MODE_SHADOW
struct ShadowRasterViewParams {
	mat4 proj;
	mat4 view;
	float z_far;
	float dp_side;
	float cull_sign;
	uint use_pancake;
	uint visible_capacity;
	uint pad0;
	uint pad1;
	uint pad2;
};

layout(set = 0, binding = 5, std430) buffer restrict readonly ShadowRasterParams {
	ShadowRasterViewParams data[];
}
shadow_raster;
#else
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
#endif

layout(set = 0, binding = 6, std430) buffer restrict readonly BucketTable {
	uint data[];
}
bucket_table;

#ifdef MODE_VIS64
// Hardware entries grow down from the top of the phase's region (raster_features_inc.glsl).
layout(set = 0, binding = 7, std430) buffer restrict readonly RasterLists {
	uint data[];
}
raster_lists;
#endif

// GodotNext: spline_meshes (#161).
layout(set = 0, binding = 9, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;

// GodotNext: nextgeo_skinned_runtime (#160). Skeleton palette arena, per-record skin table, cluster skin offsets.
layout(set = 0, binding = 10, std430) buffer restrict readonly NextGeoSkinPalette {
	vec4 data[];
}
nextgeo_skin_palette;

layout(set = 0, binding = 11, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

layout(set = 0, binding = 12, std430) buffer restrict readonly NextGeoClusterSkin {
	uint data[];
}
nextgeo_cluster_skin;

// clang-format off
#include "skinning_inc.glsl"
// clang-format on

void collapse_triangle() {
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
#ifdef MODE_SHADOW
#ifdef MODE_DUAL_PARABOLOID
	dp_clip_out = 1.0;
#endif
#else
	vis_id_out = 0xFFFFFFFFu;
#ifndef MODE_VIS64
	instance_id_out = 0xFFFFFFFFu;
#endif
#ifdef WITH_VELOCITY
	screen_position = vec4(0.0, 0.0, 0.0, 1.0);
	prev_screen_position = vec4(0.0, 0.0, 0.0, 1.0);
#endif
#endif
}

#ifdef MODE_SHADOW
vec4 project_shadow_pos(ShadowRasterViewParams sr, vec3 wp, out float r_dp_clip) {
	vec3 view_pos = (sr.view * vec4(wp, 1.0)).xyz;
#ifdef MODE_DUAL_PARABOLOID
	vec3 vtx = view_pos;
	vtx.z *= sr.dp_side;
	r_dp_clip = vtx.z;
	float dist = max(length(vtx), 1e-6);
	vtx /= dist;
	vtx.xy /= max(1.0 - vtx.z, 1e-6);
	vtx.z = (dist / max(sr.z_far, 1e-6)) * 2.0 - 1.0;
	return sr.proj * vec4(vtx, 1.0);
#else
	r_dp_clip = 0.0;
	vec4 clip = sr.proj * vec4(view_pos, 1.0);
	if (sr.use_pancake != 0u && clip.z >= 0.9999) {
		clip.z = 0.9999;
	}
	return clip;
#endif
}

void main() {
	uint view_idx = push_params.pad0;
	ShadowRasterViewParams sr = shadow_raster.data[view_idx];
	uint local_vis = uint(gl_InstanceIndex);
	uint t = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) - t * 3u;

	if (local_vis >= sr.visible_capacity) {
		collapse_triangle();
		return;
	}
	uint vis_cluster = push_params.list_base + local_vis;

	uvec2 entry = visible_clusters.data[vis_cluster];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || (record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF) {
		collapse_triangle();
		return;
	}

	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || entry.y < mesh.cluster_offset) {
		collapse_triangle();
		return;
	}
	uint local_cluster = entry.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		collapse_triangle();
		return;
	}

	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = desc.z & 0xFFu;
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	uint surface_id = desc.w & 0xFFFFu;
	if (t >= tri_count || stride < 3u || vert_count == 0u || surface_id >= 32u || (record.pad0 & (1u << surface_id)) == 0u) {
		collapse_triangle();
		return;
	}

	bool has_spline = (record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u;
	uint spline_idx = record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT;
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	uint cluster_table_idx = mesh.page_table_offset + local_cluster;
	bool cull_disabled = ((record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_DOUBLE_SIDED) || ((record.pad1[0] & (1u << surface_id)) != 0u);
	bool cull_front = (record.pad1[1] & (1u << surface_id)) != 0u;
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
				lp = nextgeo_skin_position(lp, entry.x, cluster_table_idx, vi, vert_count, false);
			}
			if (has_spline) {
				lp = spline_deform_position(spline_deforms.data[spline_idx].current, lp);
			}
			vec3 wp = vec3(
					dot(record.transform[0].xyz, lp) + record.transform[0].w,
					dot(record.transform[1].xyz, lp) + record.transform[1].w,
					dot(record.transform[2].xyz, lp) + record.transform[2].w);
			float dummy_dp = 0.0;
			tri_clip[c] = project_shadow_pos(sr, wp, dummy_dp);
		}
		if (determinant(mat3(tri_clip[0].xyw, tri_clip[1].xyw, tri_clip[2].xyw)) < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	} else {
		float effective_cull_sign = cull_front ? -sr.cull_sign : sr.cull_sign;
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
		local_pos = nextgeo_skin_position(local_pos, entry.x, cluster_table_idx, v_idx, vert_count, false);
	}
	if (has_spline) {
		local_pos = spline_deform_position(spline_deforms.data[spline_idx].current, local_pos);
	}

	vec3 world_pos = vec3(
			dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
			dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
			dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
	float dp_clip = 0.0;
	gl_Position = project_shadow_pos(sr, world_pos, dp_clip);
#ifdef MODE_DUAL_PARABOLOID
	dp_clip_out = dp_clip;
#endif
}
#else
void main() {
#ifdef MODE_VIS64
	uint vis_cluster = raster_lists.data[push_params.list_base + push_params.pad0 - 1u - uint(gl_InstanceIndex)];
#else
	uint vis_cluster = push_params.list_base + uint(gl_InstanceIndex);
#endif
	uint t = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) - t * 3u;

	if (vis_cluster >= raster.visible_capacity || vis_cluster >= 0x1FFFFFFu) {
		collapse_triangle();
		return;
	}

	uvec2 entry = visible_clusters.data[vis_cluster];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		collapse_triangle();
		return;
	}

	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || entry.y < mesh.cluster_offset) {
		collapse_triangle();
		return;
	}
	uint local_cluster = entry.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		collapse_triangle();
		return;
	}

	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = desc.z & 0xFFu;
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	uint surface_id = desc.w & 0xFFFFu;
	if (t >= tri_count || stride < 3u || vert_count == 0u || surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		collapse_triangle();
		return;
	}

	uint bucket_flags = 0u;
	if (raster.bucket_table_words > 0u) {
		uint bucket_slot = record.bucket_base + surface_id;
		if (bucket_slot >= raster.bucket_table_words) {
			collapse_triangle();
			return;
		}
		uint bucket_word = bucket_table.data[bucket_slot];
		if ((bucket_word & NEXTGEO_BUCKET_ID_MASK) == NEXTGEO_BUCKET_INVALID) {
			collapse_triangle();
			return;
		}
		bucket_flags = bucket_word;
	}

	bool has_spline = (record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u;
	uint spline_idx = record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT;
	bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
	uint cluster_table_idx = mesh.page_table_offset + local_cluster;
#ifndef DEBUG_BACKFACE
	if ((bucket_flags & NEXTGEO_BUCKET_CULL_DISABLED) != 0u) {
		// NEXTGEO_BUCKET_CULL_DISABLED: orient the triangle clockwise in clip space so both faces pass POLYGON_CULL_BACK.
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
				lp = nextgeo_skin_position(lp, entry.x, cluster_table_idx, vi, vert_count, false);
			}
			if (has_spline) {
				lp = spline_deform_position(spline_deforms.data[spline_idx].current, lp);
			}
			vec3 wp = vec3(
					dot(record.transform[0].xyz, lp) + record.transform[0].w,
					dot(record.transform[1].xyz, lp) + record.transform[1].w,
					dot(record.transform[2].xyz, lp) + record.transform[2].w);
			tri_clip[c] = raster.proj * (raster.view * vec4(wp, 1.0));
		}
		if (determinant(mat3(tri_clip[0].xyw, tri_clip[1].xyw, tri_clip[2].xyw)) < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	} else {
		float effective_cull_sign = ((bucket_flags & NEXTGEO_BUCKET_CULL_FRONT) != 0u) ? -raster.cull_sign : raster.cull_sign;
		mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
		if (determinant(basis) * effective_cull_sign < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	}
#endif

	uint idx_byte = desc.y + t * 3u + corner;
	uint v_idx = (pages.data[idx_byte >> 2u] >> ((idx_byte & 3u) * 8u)) & 0xFFu;
	if (v_idx >= vert_count) {
		collapse_triangle();
		return;
	}

	uint pos_word = desc.x + v_idx * stride;
	vec3 bind_pos = vec3(
			uintBitsToFloat(pages.data[pos_word + 0u]),
			uintBitsToFloat(pages.data[pos_word + 1u]),
			uintBitsToFloat(pages.data[pos_word + 2u]));
	vec3 local_pos = is_skinned ? nextgeo_skin_position(bind_pos, entry.x, cluster_table_idx, v_idx, vert_count, false) : bind_pos;
	if (has_spline) {
		local_pos = spline_deform_position(spline_deforms.data[spline_idx].current, local_pos);
	}

	vec3 world_pos = vec3(
			dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
			dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
			dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
	vec3 view_pos = (raster.view * vec4(world_pos, 1.0)).xyz;
	vec4 cur_clip = raster.proj * vec4(view_pos, 1.0);

	gl_Position = cur_clip;
	vis_id_out = (vis_cluster << 7u) | (t & 0x7Fu);
#ifndef MODE_VIS64
	instance_id_out = entry.x;
#endif

#ifdef WITH_VELOCITY
	vec3 prev_local_pos = is_skinned ? nextgeo_skin_position(bind_pos, entry.x, cluster_table_idx, v_idx, vert_count, true) : bind_pos;
	if (has_spline) {
		prev_local_pos = spline_deform_position(spline_deforms.data[spline_idx].previous, prev_local_pos);
	}
	vec3 prev_world_pos = vec3(
			dot(record.prev_transform[0].xyz, prev_local_pos) + record.prev_transform[0].w,
			dot(record.prev_transform[1].xyz, prev_local_pos) + record.prev_transform[1].w,
			dot(record.prev_transform[2].xyz, prev_local_pos) + record.prev_transform[2].w);
	vec3 prev_view_pos = (raster.prev_view * vec4(prev_world_pos, 1.0)).xyz;
	screen_position = cur_clip;
	prev_screen_position = raster.prev_proj * vec4(prev_view_pos, 1.0);
#endif
}
#endif

#[fragment]

#version 450

#VERSION_DEFINES

#ifdef MODE_VIS64
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif
#endif

#ifdef MODE_SHADOW
#ifdef MODE_DUAL_PARABOLOID
layout(location = 0) in float dp_clip_in;
#endif

void main() {
#ifdef MODE_DUAL_PARABOLOID
	if (dp_clip_in > 0.0) {
		discard;
	}
#endif
}
#elif defined(MODE_VIS64)
layout(location = 0) in flat uint vis_id_in;

layout(push_constant, std430) uniform PushParams {
	uint list_base;
	uint pad0;
	uint pad1; // The target width.
	uint pad2;
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
	if (!(gl_FragCoord.z > 0.0)) {
		return;
	}
	uint64_t value = (uint64_t(floatBitsToUint(min(gl_FragCoord.z, 1.0))) << 32u) | uint64_t(vis_id_in);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
#ifdef VIS64_BUFFER
	atomicMax(vis64.data[uint(pixel.y) * push_params.pad1 + uint(pixel.x)], value);
#else
	imageAtomicMax(vis64, pixel, value);
#endif
}
#else
layout(location = 0) in flat uint vis_id_in;
layout(location = 1) in flat uint instance_id_in;
#ifdef WITH_VELOCITY
layout(location = 2) in vec4 screen_position;
layout(location = 3) in vec4 prev_screen_position;
#endif

layout(location = 0) out uint visibility_out;
layout(location = 1) out uint instance_id_out;
#ifdef WITH_VELOCITY
layout(location = 2) out vec2 velocity_out;
#endif

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

void main() {
	visibility_out = vis_id_in;
	instance_id_out = instance_id_in;

#ifdef WITH_VELOCITY
	if (raster.reset_velocity != 0u || screen_position.w <= 1e-6 || prev_screen_position.w <= 1e-6) {
		velocity_out = vec2(0.0, 0.0);
	} else {
		vec2 position_clip = (screen_position.xy / screen_position.w) - raster.taa_jitter;
		vec2 prev_position_clip = (prev_screen_position.xy / prev_screen_position.w) - raster.prev_taa_jitter;
		vec2 mv = (prev_position_clip - position_clip) * vec2(0.5, 0.5);
		if (mv.x == -1.0 && mv.y == -1.0) {
			mv.x = -0.9990234375;
		}
		velocity_out = mv;
	}
#endif
}
#endif
