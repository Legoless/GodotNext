#[vertex]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N2). The visibility export: a full-screen pass after
// each phase of the hybrid raster that turns the 64-bit visibility buffer back into the v2.4 targets,
// so Hi-Z, Stage A/B and the upscalers keep their inputs (§7.4). Per covered texel it writes
// gl_FragDepth, the R32 visibility ID, the instance ID and, with WITH_VELOCITY, the velocity from the
// cluster's previous-frame transform. The depth test (GREATER_OR_EQUAL against the depth pre-pass)
// keeps legacy geometry in front, as the v2.4 depth attachment did. Built only on the Nanite device
// tier (N1).

void main() {
	vec2 base = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(base * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifndef VIS64_BUFFER
#extension GL_EXT_shader_image_int64 : require
#endif

// clang-format off
#include "../spline_deform_inc.glsl"
#include "nextgeo_records_inc.glsl"
#include "tessellation_inc.glsl"
#include "voxel_inc.glsl"
// clang-format on

layout(location = 0) out uint visibility_out;
layout(location = 1) out uint instance_id_out;
#ifdef WITH_VELOCITY
layout(location = 2) out vec2 velocity_out;
#endif

layout(set = 0, binding = 0, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 1, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

// NextGeoCut's RasterParams.
layout(set = 0, binding = 2, std430) buffer restrict readonly RasterParams {
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
	mat4 inv_view_proj;
	uvec2 size;
	uint raster_region;
	uint patch_capacity; // GodotNext: nextgeo_tessellation (#163)
	uint voxel_brick_capacity; // GodotNext: nextgeo_voxels (#165)
	uint pad_v0;
	uint pad_v1;
	uint pad_v2;
}
raster;

#ifdef VIS64_BUFFER
layout(set = 0, binding = 3, std430) buffer restrict readonly Vis64 {
	uint64_t data[];
}
vis64;
#else
layout(set = 0, binding = 3, r64ui) uniform restrict readonly u64image2D vis64;
#endif

// GodotNext: nextgeo_tessellation (#163). Compact diced patch records (dummy buffer when tessellation is off).
layout(set = 0, binding = 4, std430) buffer restrict readonly NextGeoDicedPatches {
	NextGeoDicedPatchRecord data[];
}
diced_patches;

#ifdef WITH_VELOCITY
// GodotNext: nextgeo_skinned_runtime (#160) + spline_meshes (#161). Bindings 5..11 reconstruct exact
// current/previous triangle motion vectors for skinned and spline-deformed virtual geometry instances.
layout(set = 0, binding = 5, std430) buffer restrict readonly Meshes {
	NextGeoMeshRecord data[];
}
meshes;

layout(set = 0, binding = 6, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;

layout(set = 0, binding = 7, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;

layout(set = 0, binding = 8, std430) buffer restrict readonly NextGeoSkinPalette {
	vec4 data[];
}
nextgeo_skin_palette;

layout(set = 0, binding = 9, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

layout(set = 0, binding = 10, std430) buffer restrict readonly NextGeoClusterSkin {
	uint data[];
}
nextgeo_cluster_skin;

layout(set = 0, binding = 11, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
spline_deforms;

// GodotNext: nextgeo_foliage_wind (#152). Bindings 12..13 hold wind params and per-mesh wind skeletons (14..15 reserved).
layout(set = 0, binding = 12, std430) buffer restrict readonly NextGeoWindParams {
	vec4 direction_strength;
	vec4 time_gust_flags;
	uvec4 meta;
}
nextgeo_wind_params;

layout(set = 0, binding = 13, std430) buffer restrict readonly NextGeoWindMeshes {
	NextGeoWindMesh data[];
}
nextgeo_wind_meshes;

#define NEXTGEO_HAS_WIND_BUFFERS
// clang-format off
#include "skinning_inc.glsl"
// clang-format on
#endif

// GodotNext: nextgeo_voxels (#165). Visible sparse 4^3 voxel brick records (binding 14 avoids 12..13 reserved for #152).
layout(set = 0, binding = 14, std430) buffer restrict readonly NextGeoVisibleVoxelBricks {
	uvec4 data[];
}
visible_voxel_bricks;

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
#ifdef VIS64_BUFFER
	uint64_t texel = vis64.data[uint(pixel.y) * raster.size.x + uint(pixel.x)];
#else
	uint64_t texel = imageLoad(vis64, pixel).x;
#endif
	uint depth_bits = uint(texel >> 32u);
	uint vis_id = uint(texel & uint64_t(0xFFFFFFFFu));
	uint vis_cluster = vis_id >> 7u;
	// A zero depth word is an empty texel: the R32 targets keep their 0xFFFFFFFF clear.
	if (depth_bits == 0u) {
		discard;
	}
	bool is_voxel = nextgeo_vis_is_voxel(vis_cluster);
	uvec2 entry = uvec2(0xFFFFFFFFu, 0u);
	if (is_voxel) {
		uint brick_idx = nextgeo_vis_voxel_brick_index(vis_cluster);
		if (brick_idx >= raster.voxel_brick_capacity) {
			discard;
		}
		entry.x = visible_voxel_bricks.data[brick_idx].x;
		if (entry.x == 0xFFFFFFFFu) {
			discard;
		}
	} else {
		uint cluster_slot = vis_cluster;
		if (nextgeo_vis_is_patch(vis_cluster)) {
			uint patch_idx = nextgeo_vis_patch_index(vis_cluster);
			if (patch_idx >= raster.patch_capacity) {
				discard;
			}
			cluster_slot = diced_patches.data[patch_idx].cluster_slot;
		} else if (!nextgeo_vis_is_normal(vis_cluster)) {
			discard;
		}
		if (cluster_slot >= raster.visible_capacity) {
			discard;
		}
		entry = visible_clusters.data[cluster_slot];
	}
	float depth = uintBitsToFloat(depth_bits);
	gl_FragDepth = depth;
	visibility_out = vis_id;
	instance_id_out = entry.x;

#ifdef WITH_VELOCITY
	// The same motion vector the v2.4 raster interpolates: this pixel's surface point reprojected with
	// the cluster's previous-frame instance transform and camera (and previous-frame bone palette for skinned).
	vec2 ndc = (vec2(pixel) + 0.5) / vec2(raster.size) * 2.0 - 1.0;
	NextGeoInstanceRecord record = instances.data[entry.x];
	velocity_out = vec2(0.0);
	if (raster.reset_velocity == 0u) {
		bool computed = false;
		vec4 prev_clip = vec4(0.0);
		bool is_skinned = (record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u;
		bool is_wind = !is_skinned && nextgeo_wind_mesh_active(record.virtual_mesh);
		bool has_spline = (record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u;
		if (nextgeo_vis_is_normal(vis_cluster) && (is_skinned || is_wind || has_spline) && record.virtual_mesh != 0xFFFFFFFFu) {
			NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
			if (mesh.page_count > 0u && entry.y >= mesh.cluster_offset) {
				uint local_cluster = entry.y - mesh.cluster_offset;
				uint tri_idx = vis_id & 0x7Fu;
				if (local_cluster < mesh.cluster_count) {
					uint cluster_table_idx = mesh.page_table_offset + local_cluster;
					uvec4 desc = cluster_table.data[cluster_table_idx];
					uint stride = desc.z & 0xFFu;
					uint vert_count = (desc.z >> 8u) & 0xFFu;
					uint tri_count = (desc.z >> 16u) & 0xFFu;
					if (tri_idx < tri_count && stride >= 3u && vert_count > 0u) {
						SplineDeformPose cur_pose;
						SplineDeformPose prev_pose;
						if (has_spline) {
							uint spline_idx = record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT;
							cur_pose = spline_deforms.data[spline_idx].current;
							prev_pose = spline_deforms.data[spline_idx].previous;
						}
						vec4 cur_clip_c[3];
						vec4 prev_clip_c[3];
						bool tri_ok = true;
						for (uint c = 0u; c < 3u; c++) {
							uint ib = desc.y + tri_idx * 3u + c;
							uint vi = (pages.data[ib >> 2u] >> ((ib & 3u) * 8u)) & 0xFFu;
							if (vi >= vert_count) {
								tri_ok = false;
								break;
							}
							uint pw = desc.x + vi * stride;
							vec3 bind_p = vec3(
									uintBitsToFloat(pages.data[pw + 0u]),
									uintBitsToFloat(pages.data[pw + 1u]),
									uintBitsToFloat(pages.data[pw + 2u]));
							vec3 cur_lp = bind_p;
							vec3 prev_lp = bind_p;
							if (is_skinned) {
								cur_lp = nextgeo_skin_position(bind_p, entry.x, cluster_table_idx, vi, vert_count, false);
								prev_lp = nextgeo_skin_position(bind_p, entry.x, cluster_table_idx, vi, vert_count, true);
							} else if (is_wind) {
								cur_lp = nextgeo_wind_position(bind_p, record.virtual_mesh, record.transform, cluster_table_idx, vi, vert_count, false);
								prev_lp = nextgeo_wind_position(bind_p, record.virtual_mesh, record.prev_transform, cluster_table_idx, vi, vert_count, true);
							}
							if (has_spline) {
								cur_lp = spline_deform_position(cur_pose, cur_lp);
								prev_lp = spline_deform_position(prev_pose, prev_lp);
							}
							vec3 cur_wp = vec3(
									dot(record.transform[0].xyz, cur_lp) + record.transform[0].w,
									dot(record.transform[1].xyz, cur_lp) + record.transform[1].w,
									dot(record.transform[2].xyz, cur_lp) + record.transform[2].w);
							vec3 prev_wp = vec3(
									dot(record.prev_transform[0].xyz, prev_lp) + record.prev_transform[0].w,
									dot(record.prev_transform[1].xyz, prev_lp) + record.prev_transform[1].w,
									dot(record.prev_transform[2].xyz, prev_lp) + record.prev_transform[2].w);
							cur_clip_c[c] = raster.proj * (raster.view * vec4(cur_wp, 1.0));
							prev_clip_c[c] = raster.prev_proj * (raster.prev_view * vec4(prev_wp, 1.0));
						}
						if (tri_ok) {
							mat3 m = mat3(cur_clip_c[0].xyw, cur_clip_c[1].xyw, cur_clip_c[2].xyw);
							vec3 u0 = cross(m[1], m[2]);
							vec3 u1 = cross(m[2], m[0]);
							vec3 u2 = cross(m[0], m[1]);
							float det = dot(m[0], u0);
							float inv_det = abs(det) > 1e-20 ? (1.0 / det) : 0.0;
							vec3 p = vec3(ndc, 1.0);
							vec3 q = vec3(dot(u0, p), dot(u1, p), dot(u2, p)) * inv_det;
							float inv_w_sum = q.x + q.y + q.z;
							vec3 bary = abs(inv_w_sum) > 1e-20 ? (q / inv_w_sum) : vec3(1.0, 0.0, 0.0);
							prev_clip = prev_clip_c[0] * bary.x + prev_clip_c[1] * bary.y + prev_clip_c[2] * bary.z;
							computed = true;
						}
					}
				}
			}
		}
		if (!computed) {
			vec4 world_h = raster.inv_view_proj * vec4(ndc, depth, 1.0);
			if (abs(world_h.w) > 1e-20) {
				vec3 world_pos = world_h.xyz / world_h.w;
				// The instance transform's rows are record.transform; its inverse maps back to mesh space.
				mat3 rows_as_columns = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
				vec3 offset = world_pos - vec3(record.transform[0].w, record.transform[1].w, record.transform[2].w);
				vec3 local_pos = offset * inverse(rows_as_columns);
				vec3 prev_world_pos = vec3(
						dot(record.prev_transform[0].xyz, local_pos) + record.prev_transform[0].w,
						dot(record.prev_transform[1].xyz, local_pos) + record.prev_transform[1].w,
						dot(record.prev_transform[2].xyz, local_pos) + record.prev_transform[2].w);
				prev_clip = raster.prev_proj * (raster.prev_view * vec4(prev_world_pos, 1.0));
				computed = true;
			}
		}
		if (computed && prev_clip.w > 1e-6) {
			vec2 position_clip = ndc - raster.taa_jitter;
			vec2 prev_position_clip = (prev_clip.xy / prev_clip.w) - raster.prev_taa_jitter;
			vec2 mv = (prev_position_clip - position_clip) * vec2(0.5, 0.5);
			if (mv.x == -1.0 && mv.y == -1.0) {
				mv.x = -0.9990234375;
			}
			velocity_out = mv;
		}
	}
#endif
}
