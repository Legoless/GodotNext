#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_gpu_cull (#76). One invocation per persistent instance
// record: flags/layer/frustum/previous-Hi-Z, then seed the level-0 node list
// or the phase-2 list.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
#ifndef ASSEMBLY_EXPAND
#include "nextgeo_cull_inc.glsl"
#endif
// clang-format on

#ifdef ASSEMBLY_EXPAND
// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 contract GN-7).
// GPU 2-level Nanite Assembly expansion (instance -> part -> sub-part).
// Reads source records in [0, push.source_count) and writes expanded leaf records
// into disjoint tail range [push.expanded_base, push.expanded_base + push.max_leaves).

layout(set = 0, binding = 0, std430) buffer restrict Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 1, std430) buffer restrict InstanceAttrs {
	NextGeoInstanceAttributes data[];
}
instance_attrs;

layout(set = 0, binding = 2, std430) buffer restrict readonly AssemblyHeaders {
	NextGeoAssemblyHeader data[];
}
assembly_headers;

layout(set = 0, binding = 3, std430) buffer restrict readonly AssemblyChildren {
	NextGeoAssemblyChild data[];
}
assembly_children;

layout(set = 0, binding = 4, std430) buffer restrict readonly AssemblyParts {
	NextGeoAssemblyPart data[];
}
assembly_parts;

layout(set = 0, binding = 5, std430) buffer restrict readonly BucketSurfaceMasks {
	NextGeoBucketSurfaceMask data[];
}
bucket_surface_masks;

layout(set = 0, binding = 6, std430) buffer restrict AssemblyCounter {
	uint next_slot;
	uint pad0;
	uint pad1;
	uint pad2;
}
assembly_counter;

layout(push_constant, std430) uniform AssemblyExpandPush {
	uint source_count;
	uint expanded_base;
	uint max_leaves;
	uint bucket_mask_count;
}
push;

void compose_3x4(vec4 a[3], vec4 b[3], out vec4 r[3]) {
	for (int row = 0; row < 3; row++) {
		vec3 ar = a[row].xyz;
		r[row].x = ar.x * b[0].x + ar.y * b[1].x + ar.z * b[2].x;
		r[row].y = ar.x * b[0].y + ar.y * b[1].y + ar.z * b[2].y;
		r[row].z = ar.x * b[0].z + ar.y * b[1].z + ar.z * b[2].z;
		r[row].w = ar.x * b[0].w + ar.y * b[1].w + ar.z * b[2].w + a[row].w;
	}
}

void transform_aabb(vec4 m[3], vec3 lmin, vec3 lmax, out vec3 wmin, out vec3 wmax) {
	vec3 center = 0.5 * (lmin + lmax);
	vec3 extent = 0.5 * (lmax - lmin);
	vec3 wc = vec3(
			dot(m[0].xyz, center) + m[0].w,
			dot(m[1].xyz, center) + m[1].w,
			dot(m[2].xyz, center) + m[2].w);
	vec3 we = vec3(
			dot(abs(m[0].xyz), extent),
			dot(abs(m[1].xyz), extent),
			dot(abs(m[2].xyz), extent));
	wmin = wc - we;
	wmax = wc + we;
}

void write_assembly_leaf(uint p_dst_slot, NextGeoInstanceRecord p_src, NextGeoInstanceAttributes p_src_attr, vec4 p_parent_cur[3], vec4 p_parent_prev[3], NextGeoAssemblyPart p_part) {
	if (p_part.part_virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	NextGeoInstanceRecord leaf = p_src;
	leaf.flags &= ~(NEXTGEO_RECORD_HAS_ASSEMBLY | NEXTGEO_RECORD_NON_UNIFORM_SCALE);
	leaf.flags |= NEXTGEO_RECORD_ASSEMBLY_PART;
	leaf.virtual_mesh = p_part.part_virtual_mesh;

	uint bslot = (p_src.bucket_base >> 5u) + p_part.unique_part_index;
	leaf.bucket_base = bslot * 32u;
	if (bslot < push.bucket_mask_count) {
		NextGeoBucketSurfaceMask sm = bucket_surface_masks.data[bslot];
		leaf.surface_mask = sm.surface_mask;
		leaf.pad0 = sm.shadow_surface_mask;
		leaf.pad1[0] = sm.cull_disabled_mask;
		leaf.pad1[1] = sm.cull_front_mask;
	} else {
		leaf.surface_mask = p_part.surface_mask;
		leaf.pad0 = p_part.surface_mask;
	}

	vec4 wcur[3];
	vec4 wprev[3];
	compose_3x4(p_parent_cur, p_part.transform, wcur);
	compose_3x4(p_parent_prev, p_part.transform, wprev);

	leaf.transform[0] = wcur[0];
	leaf.transform[1] = wcur[1];
	leaf.transform[2] = wcur[2];
	leaf.prev_transform[0] = wprev[0];
	leaf.prev_transform[1] = wprev[1];
	leaf.prev_transform[2] = wprev[2];

	vec3 c0 = vec3(wcur[0].x, wcur[1].x, wcur[2].x);
	vec3 c1 = vec3(wcur[0].y, wcur[1].y, wcur[2].y);
	vec3 c2 = vec3(wcur[0].z, wcur[1].z, wcur[2].z);
	float sx2 = dot(c0, c0);
	float sy2 = dot(c1, c1);
	float sz2 = dot(c2, c2);
	float max_s2 = max(sx2, max(sy2, sz2));
	float max_scale = sqrt(max_s2);
	if (max_s2 > 1e-12) {
		float eps = max_s2 * 1e-4;
		if (abs(sx2 - sy2) > eps || abs(sy2 - sz2) > eps || abs(sx2 - sz2) > eps) {
			leaf.flags |= NEXTGEO_RECORD_NON_UNIFORM_SCALE;
		}
	}
	if (max_scale <= 1e-12) {
		leaf.flags &= ~NEXTGEO_RECORD_VISIBLE;
	}

	vec3 wmin, wmax, pwmin, pwmax;
	transform_aabb(wcur, p_part.local_aabb_min.xyz, p_part.local_aabb_max.xyz, wmin, wmax);
	transform_aabb(wprev, p_part.local_aabb_min.xyz, p_part.local_aabb_max.xyz, pwmin, pwmax);
	leaf.world_aabb_min = vec4(wmin, max_scale);
	leaf.world_aabb_max = vec4(wmax, p_src.world_aabb_max.w);
	leaf.prev_world_aabb_min = vec4(pwmin, p_src.prev_world_aabb_min.w);
	leaf.prev_world_aabb_max = vec4(pwmax, p_src.prev_world_aabb_max.w);

	instances.data[p_dst_slot] = leaf;
	instance_attrs.data[p_dst_slot] = p_src_attr;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= push.source_count) {
		return;
	}
	NextGeoInstanceRecord src = instances.data[index];
	uint req = NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE | NEXTGEO_RECORD_HAS_ASSEMBLY;
	if ((src.flags & req) != req || (src.flags & NEXTGEO_RECORD_MULTIMESH_PARENT) != 0u || src.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	NextGeoAssemblyHeader hdr = assembly_headers.data[src.virtual_mesh];
	if (hdr.leaf_count == 0u) {
		return;
	}
	uint rel_base = atomicAdd(assembly_counter.next_slot, hdr.leaf_count);
	if (rel_base + hdr.leaf_count > push.max_leaves) {
		return;
	}
	NextGeoInstanceAttributes src_attr = instance_attrs.data[index];
	uint dst = push.expanded_base + rel_base;

	for (uint p = 0u; p < hdr.direct_part_count; p++) {
		NextGeoAssemblyPart part = assembly_parts.data[hdr.direct_part_offset + p];
		write_assembly_leaf(dst++, src, src_attr, src.transform, src.prev_transform, part);
	}
	for (uint c = 0u; c < hdr.child_count; c++) {
		NextGeoAssemblyChild child = assembly_children.data[hdr.child_offset + c];
		vec4 child_cur[3];
		vec4 child_prev[3];
		compose_3x4(src.transform, child.transform, child_cur);
		compose_3x4(src.prev_transform, child.transform, child_prev);
		for (uint p = 0u; p < child.part_count; p++) {
			NextGeoAssemblyPart part = assembly_parts.data[child.part_offset + p];
			write_assembly_leaf(dst++, src, src_attr, child_cur, child_prev, part);
		}
	}
}
#else

bool aabb_visible_planes(vec4 frustum[6], vec3 lo, vec3 hi) {
	for (uint plane = 0u; plane < 6u; plane++) {
		vec3 normal = frustum[plane].xyz;
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
		if (dot(normal, farthest) + frustum[plane].w < 0.0) {
			return false;
		}
	}
	return true;
}

layout(set = 0, binding = 8, std430) buffer restrict Phase2Nodes {
	uvec2 data[];
}
phase2_nodes;

layout(set = 0, binding = 9, std430) buffer restrict BVHArgs {
	uvec4 data[];
}
bvh_args;

layout(set = 0, binding = 10, std430) buffer restrict readonly InstanceAttrs {
	NextGeoInstanceAttributes data[];
}
instance_attrs;

bool multimesh_parent_visible_planes(vec4 frustum[6], NextGeoInstanceRecord p_record, NextGeoInstanceAttributes p_attr) {
	if ((p_record.flags & NEXTGEO_RECORD_MULTIMESH_ELEMENT) == 0u || p_attr.meta.x == 0xFFFFFFFFu) {
		return true;
	}
	NextGeoInstanceRecord parent = instances.data[p_attr.meta.x];
	if ((parent.flags & (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE)) != (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE)) {
		return false;
	}
	float parent_wpo = nextgeo_record_vertex_offset_bound(parent) * abs(parent.world_aabb_min.w);
	return aabb_visible_planes(frustum, parent.world_aabb_min.xyz - vec3(parent_wpo), parent.world_aabb_max.xyz + vec3(parent_wpo));
}

#ifdef SHADOW_VIEW
void main() {
	uint index = gl_GlobalInvocationID.x;
	uint view_idx = gl_GlobalInvocationID.y;
	NextGeoCullParamsData sc = shadow_culls.data[view_idx];
	uint count = sc.cull.y;
	if (index >= count) {
		return;
	}
	NextGeoInstanceRecord record = instances.data[index];
	if ((record.flags & (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE)) != (NEXTGEO_RECORD_VALID | NEXTGEO_RECORD_VISIBLE) ||
			(record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF ||
			(record.flags & (NEXTGEO_RECORD_HAS_INSTANCE_UNIFORMS | NEXTGEO_RECORD_MULTIMESH_PARENT | NEXTGEO_RECORD_HAS_ASSEMBLY)) != 0u ||
			(record.flags & (NEXTGEO_RECORD_HAS_SKELETON | NEXTGEO_RECORD_SKINNED_PALETTE)) == NEXTGEO_RECORD_HAS_SKELETON ||
			(record.gi_flags & NEXTGEO_GI_LIGHTMAPPED) != 0u ||
			record.virtual_mesh == 0xFFFFFFFFu ||
			record.pad0 == 0u ||
			(record.layer_mask & sc.cull.x) == 0u) {
		return;
	}
	NextGeoInstanceAttributes attr = instance_attrs.data[index];
	if (!multimesh_parent_visible_planes(sc.frustum, record, attr)) {
		return;
	}
	// GodotNext: nextgeo_vertex_offset (#164). Inflate instance AABB by vertex_offset_max * max_scale.
	float wpo_margin = nextgeo_record_vertex_offset_bound(record) * abs(record.world_aabb_min.w);
	if (!aabb_visible_planes(sc.frustum, record.world_aabb_min.xyz - vec3(wpo_margin), record.world_aabb_max.xyz + vec3(wpo_margin))) {
		return;
	}
	uint cap = sc.cull.z;
	uint slot = atomicAdd(shadow_counters.data[view_idx].phase1_count, 1u);
	if (slot >= cap) {
		atomicAdd(shadow_counters.data[view_idx].overflow, 1u);
		return;
	}
	atomicMax(bvh_args.data[view_idx * 9u].x, (slot + 64u) / 64u);
	nodes.data[view_idx * cap + slot] = uvec2(index, 0u);
}
#else
bool aabb_visible(vec3 lo, vec3 hi) {
	return aabb_visible_planes(cull.frustum, lo, hi);
}

bool distance_visible(NextGeoInstanceRecord p_record, NextGeoInstanceAttributes p_attr) {
	float vis_begin = uintBitsToFloat(p_attr.meta.z);
	float vis_end = uintBitsToFloat(p_attr.meta.w);
	if (vis_begin <= 0.0 && vis_end <= 0.0) {
		return true;
	}
	vec3 center = 0.5 * (p_record.world_aabb_min.xyz + p_record.world_aabb_max.xyz);
	float dist = length(center - cull.camera.xyz);
	if (vis_end > 0.0 && dist > vis_end) {
		return false;
	}
	if (vis_begin > 0.0 && dist < vis_begin) {
		return false;
	}
	return true;
}

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

#ifdef PHASE2
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= counters.phase2_count) {
		return;
	}
	uvec2 entry = phase2_nodes.data[index];
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u ||
			(record.flags & (NEXTGEO_RECORD_MULTIMESH_PARENT | NEXTGEO_RECORD_HAS_ASSEMBLY)) != 0u ||
			record.virtual_mesh == 0xFFFFFFFFu) {
		return;
	}
	NextGeoInstanceAttributes attr = instance_attrs.data[entry.x];
	if (!distance_visible(record, attr)) {
		return;
	}
	// GodotNext: nextgeo_vertex_offset (#164). Inflate instance AABB by vertex_offset_max * max_scale.
	float wpo_margin = nextgeo_record_vertex_offset_bound(record) * abs(record.world_aabb_min.w);
	vec4 w_min = vec4(record.world_aabb_min.xyz - vec3(wpo_margin), record.world_aabb_min.w);
	vec4 w_max = vec4(record.world_aabb_max.xyz + vec3(wpo_margin), record.world_aabb_max.w);
	if (!aabb_visible(w_min.xyz, w_max.xyz)) {
		return;
	}
	if (hiz_occluded(w_min, w_max, cull.view_proj)) {
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
			(record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_SHADOWS_ONLY ||
			(record.flags & (NEXTGEO_RECORD_HAS_INSTANCE_UNIFORMS | NEXTGEO_RECORD_MULTIMESH_PARENT | NEXTGEO_RECORD_HAS_ASSEMBLY)) != 0u ||
			(record.flags & (NEXTGEO_RECORD_HAS_SKELETON | NEXTGEO_RECORD_SKINNED_PALETTE)) == NEXTGEO_RECORD_HAS_SKELETON ||
			(record.gi_flags & NEXTGEO_GI_LIGHTMAPPED) != 0u ||
			record.virtual_mesh == 0xFFFFFFFFu ||
			record.surface_mask == 0u ||
			(record.layer_mask & cull.cull.x) == 0u) {
		return;
	}
	NextGeoInstanceAttributes attr = instance_attrs.data[index];
	// GodotNext: nextgeo_foliage_instancing (#151). Coarse MultiMesh AABB pre-test, then per-instance distance & frustum cull.
	if (!multimesh_parent_visible_planes(cull.frustum, record, attr)) {
		return;
	}
	if (!distance_visible(record, attr)) {
		return;
	}
	// GodotNext: nextgeo_vertex_offset (#164). Inflate instance AABB by vertex_offset_max * max_scale.
	float wpo_margin = nextgeo_record_vertex_offset_bound(record) * abs(record.world_aabb_min.w);
	if (!aabb_visible(record.world_aabb_min.xyz - vec3(wpo_margin), record.world_aabb_max.xyz + vec3(wpo_margin))) {
		return;
	}
	atomicAdd(counters.pad[0], 1u); // hiz_tested
	vec4 prev_min = vec4(record.prev_world_aabb_min.xyz - vec3(wpo_margin), record.prev_world_aabb_min.w);
	vec4 prev_max = vec4(record.prev_world_aabb_max.xyz + vec3(wpo_margin), record.prev_world_aabb_max.w);
	if (hiz_occluded(prev_min, prev_max, cull.prev_view_proj)) {
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
#endif
#endif
