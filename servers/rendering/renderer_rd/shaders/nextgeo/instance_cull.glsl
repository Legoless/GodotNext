#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_gpu_cull (#76). One invocation per persistent instance
// record: flags/layer/frustum/previous-Hi-Z, then seed the level-0 node list
// or the phase-2 list.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "nextgeo_records_inc.glsl"
#include "nextgeo_cull_inc.glsl"

bool aabb_visible(vec3 lo, vec3 hi) {
	for (uint plane = 0u; plane < 6u; plane++) {
		vec3 normal = cull.frustum[plane].xyz;
		// Positive vertex test: the AABB is outside when its farthest corner
		// along the plane normal is behind the plane.
		vec3 farthest = lo;
		if (normal.x >= 0.0) {
			farthest.x = hi.x;
		}
		if (normal.y >= 0.0) {
			farthest.y = hi.y;
		}
		if (normal.z >= 0.0) {
			farthest.z = hi.z;
		}
		if (dot(normal, farthest) + cull.frustum[plane].w < 0.0) {
			return false;
		}
	}
	return true;
}

bool hiz_occluded(vec4 world_min, vec4 world_max) {
	vec3 lo = world_min.xyz;
	vec3 hi = world_max.xyz;
	vec4 clip_min = cull.prev_view_proj * vec4(lo, 1.0);
	vec4 clip_max = cull.prev_view_proj * vec4(hi, 1.0);
	if (clip_min.w <= 0.0 || clip_max.w <= 0.0) {
		return false; // Behind the previous camera; keep it for phase 1.
	}
	vec2 ndc_min = clip_min.xy / clip_min.w;
	vec2 ndc_max = clip_max.xy / clip_max.w;
	vec2 uv_min = ndc_min * 0.5 + 0.5;
	vec2 uv_max = ndc_max * 0.5 + 0.5;
	if (uv_max.x < 0.0 || uv_max.y < 0.0 || uv_min.x > 1.0 || uv_min.y > 1.0) {
		return false;
	}
	// Reverse-Z: the farthest corner is the minimum of the two depths.
	float farthest_depth = min(clip_min.z / clip_min.w, clip_max.z / clip_max.w);
	vec2 uv = clamp((uv_min + uv_max) * 0.5, vec2(0.0), vec2(1.0));
	vec2 size = abs(uv_max - uv_min) * cull.view.xy;
	float mip = max(log2(max(size.x, size.y)), 0.0);
	float occlusion = textureLod(hiz_prev, uv, mip).r;
	return farthest_depth < occlusion;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint count = cull.cull.y;
	if (index >= count) {
		return;
	}
	NextGeoInstanceRecord record = instances.data[index];
	if ((record.flags & (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE)) != (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE) ||
			(record.flags & (NEXTGEO_RECORD_HAS_SKELETON | NEXTGEO_RECORD_HAS_INSTANCE_UNIFORMS)) != 0u ||
			(record.layer_mask & cull.cull.x) == 0u) {
		return;
	}
	if (!aabb_visible(record.world_aabb_min.xyz, record.world_aabb_max.xyz)) {
		return;
	}
	if (hiz_occluded(record.prev_world_aabb_min, record.prev_world_aabb_max)) {
		uint slot = atomicAdd(counters.phase2_count, 1u);
		nodes.data[slot] = uvec2(index, record.virtual_mesh);
		return;
	}
	uint slot = atomicAdd(counters.phase1_count, 1u);
	nodes.data[slot] = uvec2(index, record.virtual_mesh);
}
