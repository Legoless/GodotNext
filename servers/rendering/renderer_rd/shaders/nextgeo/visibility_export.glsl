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
#include "nextgeo_records_inc.glsl"
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
	uint pad;
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
	if (depth_bits == 0u || vis_cluster >= raster.visible_capacity) {
		discard;
	}
	float depth = uintBitsToFloat(depth_bits);
	gl_FragDepth = depth;
	visibility_out = vis_id;
	uvec2 entry = visible_clusters.data[vis_cluster];
	instance_id_out = entry.x;

#ifdef WITH_VELOCITY
	// The same motion vector the v2.4 raster interpolates: this pixel's surface point reprojected with
	// the cluster's previous-frame instance transform and camera.
	vec2 ndc = (vec2(pixel) + 0.5) / vec2(raster.size) * 2.0 - 1.0;
	vec4 world_h = raster.inv_view_proj * vec4(ndc, depth, 1.0);
	NextGeoInstanceRecord record = instances.data[entry.x];
	velocity_out = vec2(0.0);
	if (raster.reset_velocity == 0u && abs(world_h.w) > 1e-20) {
		vec3 world_pos = world_h.xyz / world_h.w;
		// The instance transform's rows are record.transform; its inverse maps back to mesh space.
		mat3 rows_as_columns = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
		vec3 offset = world_pos - vec3(record.transform[0].w, record.transform[1].w, record.transform[2].w);
		vec3 local_pos = offset * inverse(rows_as_columns);
		vec3 prev_world_pos = vec3(
				dot(record.prev_transform[0].xyz, local_pos) + record.prev_transform[0].w,
				dot(record.prev_transform[1].xyz, local_pos) + record.prev_transform[1].w,
				dot(record.prev_transform[2].xyz, local_pos) + record.prev_transform[2].w);
		vec4 prev_clip = raster.prev_proj * (raster.prev_view * vec4(prev_world_pos, 1.0));
		if (prev_clip.w > 1e-6) {
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
