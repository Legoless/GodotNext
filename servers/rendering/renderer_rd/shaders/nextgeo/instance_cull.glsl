#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_gpu_cull (#76). One invocation per persistent instance
// record: flags/layer/frustum/previous-Hi-Z, then seed the level-0 node list
// or the phase-2 list.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
#include "nextgeo_cull_inc.glsl"
// clang-format on

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

layout(set = 0, binding = 8, std430) buffer restrict Phase2Nodes {
	uvec2 data[];
}
phase2_nodes;

bool hiz_occluded(vec4 world_min, vec4 world_max, mat4 proj_mat) {
	if ((cull.cull.w & 1u) == 0u) {
		return false; // Occlusion off (the CPU parity reference).
	}
	vec3 lo = world_min.xyz;
	vec3 hi = world_max.xyz;
	vec2 uv_min = vec2(2.0);
	vec2 uv_max = vec2(-1.0);
	// Reverse-Z: the nearest corner has the largest depth; the whole box is
	// hidden only when even its nearest point is behind the stored surface.
	float nearest_depth = 0.0;
	bool any_front = false;
	for (int corner = 0; corner < 8; corner++) {
		vec3 world = vec3((corner & 1) != 0 ? hi.x : lo.x, (corner & 2) != 0 ? hi.y : lo.y, (corner & 4) != 0 ? hi.z : lo.z);
		vec4 clip = proj_mat * vec4(world, 1.0);
		if (clip.w <= 0.0) {
			continue;
		}
		any_front = true;
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		uv_min = min(uv_min, uv);
		uv_max = max(uv_max, uv);
		nearest_depth = max(nearest_depth, clip.z / clip.w);
	}
	if (!any_front) {
		atomicAdd(counters.cut_lo, 1u); // hiz_behind diagnostic
		return false;
	}
	// Only cull when the whole projected box is on screen; an off-screen part
	// could be visible and must keep the instance.
	if (uv_min.x < 0.0 || uv_min.y < 0.0 || uv_max.x > 1.0 || uv_max.y > 1.0) {
		atomicAdd(counters.cut_hi, 1u); // hiz_oob diagnostic
		return false;
	}
	// Level 0 is half the internal resolution; pick the mip whose texel spans
	// the box and take the farthest (minimum) of four taps, which keeps the
	// test conservative (a false miss costs clusters, a false hit drops the box).
	vec2 size = (uv_max - uv_min) * cull.view.xy * 0.5;
	float mip = max(floor(log2(max(max(size.x, size.y), 1.0))), 0.0);
	float occlusion = 1.0;
	for (int tap = 0; tap < 4; tap++) {
		vec2 uv = vec2((tap & 1) != 0 ? uv_max.x : uv_min.x, (tap & 2) != 0 ? uv_max.y : uv_min.y);
		occlusion = min(occlusion, textureLod(hiz_prev, uv, mip).r);
	}
	return nearest_depth < occlusion;
}

layout(set = 0, binding = 9, std430) buffer restrict BVHArgs {
	uvec4 data[];
}
bvh_args;

#ifdef PHASE2
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= counters.phase2_count) {
		return;
	}
	uvec2 entry = phase2_nodes.data[index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	if (!aabb_visible(record.world_aabb_min.xyz, record.world_aabb_max.xyz)) {
		return;
	}
	if (hiz_occluded(record.world_aabb_min, record.world_aabb_max, cull.view_proj)) {
		return;
	}
	atomicAdd(counters.phase1_count, 1u);
	uint slot = atomicAdd(counters.phase2_visible_count, 1u);
	atomicMax(bvh_args.data[9].x, (slot + 64u) / 64u);
	nodes.data[slot] = uvec2(entry.x, 0u);
}
#else
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint count = cull.cull.y;
	if (index >= count) {
		return;
	}
	NextGeoInstanceRecord record = instances.data[index];
	if ((record.flags & (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE)) != (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE) ||
			(record.flags & (NEXTGEO_RECORD_HAS_SKELETON | NEXTGEO_RECORD_HAS_INSTANCE_UNIFORMS)) != 0u ||
			(record.gi_flags & NEXTGEO_GI_LIGHTMAPPED) != 0u ||
			record.virtual_mesh == 0xFFFFFFFFu ||
			record.surface_mask == 0u ||
			(record.layer_mask & cull.cull.x) == 0u) {
		return;
	}
	if (!aabb_visible(record.world_aabb_min.xyz, record.world_aabb_max.xyz)) {
		return;
	}
	atomicAdd(counters.pad[0], 1u); // hiz_tested
	if (hiz_occluded(record.prev_world_aabb_min, record.prev_world_aabb_max, cull.prev_view_proj)) {
		atomicAdd(counters.pad[1], 1u); // hiz_hit
		uint slot = atomicAdd(counters.phase2_count, 1u);
		phase2_nodes.data[slot] = uvec2(index, record.virtual_mesh);
		return;
	}
	uint slot = atomicAdd(counters.phase1_count, 1u);
	atomicMax(bvh_args.data[0].x, (slot + 64u) / 64u);
	// Level 0 always starts at the mesh's BVH root (node 0).
	nodes.data[slot] = uvec2(index, 0u);
}
#endif
