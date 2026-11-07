#[vertex]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_visibility_raster (#83, plan §6.1, §6.3, §7.4).
// Draw path (c): one indirect instanced draw per phase over the compacted
// visible-cluster list, pulling cluster vertices from the page arena.

// clang-format off
#include "nextgeo_records_inc.glsl"
// clang-format on

layout(location = 0) out flat uint vis_id_out;
layout(location = 1) out flat uint instance_id_out;
#ifdef WITH_VELOCITY
layout(location = 2) out vec4 screen_position;
layout(location = 3) out vec4 prev_screen_position;
#endif

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
	uint pad0;
}
raster;

void collapse_triangle() {
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
	vis_id_out = 0xFFFFFFFFu;
	instance_id_out = 0xFFFFFFFFu;
#ifdef WITH_VELOCITY
	screen_position = vec4(0.0, 0.0, 0.0, 1.0);
	prev_screen_position = vec4(0.0, 0.0, 0.0, 1.0);
#endif
}

void main() {
	uint vis_cluster = push_params.list_base + uint(gl_InstanceIndex);
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
	if (t >= tri_count || stride < 3u || vert_count == 0u) {
		collapse_triangle();
		return;
	}

#ifndef DEBUG_BACKFACE
	mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
	if (determinant(basis) * raster.cull_sign < 0.0 && corner != 0u) {
		corner = 3u - corner;
	}
#endif

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

	vec3 world_pos = vec3(
			dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
			dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
			dot(record.transform[2].xyz, local_pos) + record.transform[2].w);
	vec3 view_pos = (raster.view * vec4(world_pos, 1.0)).xyz;
	vec4 cur_clip = raster.proj * vec4(view_pos, 1.0);

	gl_Position = cur_clip;
	vis_id_out = (vis_cluster << 7u) | (t & 0x7Fu);
	instance_id_out = entry.x;

#ifdef WITH_VELOCITY
	vec3 prev_world_pos = vec3(
			dot(record.prev_transform[0].xyz, local_pos) + record.prev_transform[0].w,
			dot(record.prev_transform[1].xyz, local_pos) + record.prev_transform[1].w,
			dot(record.prev_transform[2].xyz, local_pos) + record.prev_transform[2].w);
	vec3 prev_view_pos = (raster.prev_view * vec4(prev_world_pos, 1.0)).xyz;
	screen_position = cur_clip;
	prev_screen_position = raster.prev_proj * vec4(prev_view_pos, 1.0);
#endif
}

#[fragment]

#version 450

#VERSION_DEFINES

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
	uint pad0;
}
raster;

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
