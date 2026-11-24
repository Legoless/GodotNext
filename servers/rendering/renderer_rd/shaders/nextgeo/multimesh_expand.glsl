#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 contract N10, GN-12).
// Expands an eligible 3D non-indirect MultiMesh GPU storage buffer into persistent
// per-element NextGeoInstanceRecord and NextGeoInstanceAttributes slots.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std430) buffer restrict Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 1, std430) buffer restrict InstanceAttrs {
	NextGeoInstanceAttributes data[];
}
instance_attrs;

layout(set = 0, binding = 2, std430) buffer restrict readonly MultiMeshBuffer {
	float data[];
}
multimesh_buf;

layout(set = 0, binding = 3, std430) buffer restrict readonly ExpandParams {
	uint range_base;
	uint range_count;
	uint visible_count;
	uint parent_record;
	uint stride;
	uint color_offset;
	uint custom_offset;
	uint cur_offset;
	uint prev_offset;
	uint flags; // bit 0 = has_color, bit 1 = has_custom, bit 2 = set_prev_equal_cur
	uint pad0;
	uint pad1;
	vec4 local_aabb_min;
	vec4 local_aabb_max;
}
params;

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

void main() {
	uint elem = gl_GlobalInvocationID.x;
	if (elem >= params.range_count) {
		return;
	}

	uint dst_slot = params.range_base + elem;
	NextGeoInstanceRecord parent = instances.data[params.parent_record];
	NextGeoInstanceAttributes parent_attr = instance_attrs.data[params.parent_record];

	NextGeoInstanceRecord out_rec = parent;
	out_rec.flags &= ~(NEXTGEO_RECORD_MULTIMESH_PARENT | NEXTGEO_RECORD_NON_UNIFORM_SCALE | NEXTGEO_RECORD_HAS_INSTANCE_COLOR | NEXTGEO_RECORD_HAS_INSTANCE_CUSTOM);
	out_rec.flags |= NEXTGEO_RECORD_MULTIMESH_ELEMENT;

	bool has_color = (params.flags & 1u) != 0u;
	bool has_custom = (params.flags & 2u) != 0u;
	bool set_prev_equal_cur = (params.flags & 4u) != 0u;

	if (has_color) {
		out_rec.flags |= NEXTGEO_RECORD_HAS_INSTANCE_COLOR;
	}
	if (has_custom) {
		out_rec.flags |= NEXTGEO_RECORD_HAS_INSTANCE_CUSTOM;
	}

	uint cur_base = (params.cur_offset + elem) * params.stride;
	uint prev_base = (params.prev_offset + elem) * params.stride;

	vec4 elem_cur[3];
	elem_cur[0] = vec4(multimesh_buf.data[cur_base + 0u], multimesh_buf.data[cur_base + 1u], multimesh_buf.data[cur_base + 2u], multimesh_buf.data[cur_base + 3u]);
	elem_cur[1] = vec4(multimesh_buf.data[cur_base + 4u], multimesh_buf.data[cur_base + 5u], multimesh_buf.data[cur_base + 6u], multimesh_buf.data[cur_base + 7u]);
	elem_cur[2] = vec4(multimesh_buf.data[cur_base + 8u], multimesh_buf.data[cur_base + 9u], multimesh_buf.data[cur_base + 10u], multimesh_buf.data[cur_base + 11u]);

	vec4 world_cur[3];
	compose_3x4(parent.transform, elem_cur, world_cur);

	vec4 world_prev[3];
	if (set_prev_equal_cur) {
		world_prev[0] = world_cur[0];
		world_prev[1] = world_cur[1];
		world_prev[2] = world_cur[2];
	} else {
		vec4 elem_prev[3];
		elem_prev[0] = vec4(multimesh_buf.data[prev_base + 0u], multimesh_buf.data[prev_base + 1u], multimesh_buf.data[prev_base + 2u], multimesh_buf.data[prev_base + 3u]);
		elem_prev[1] = vec4(multimesh_buf.data[prev_base + 4u], multimesh_buf.data[prev_base + 5u], multimesh_buf.data[prev_base + 6u], multimesh_buf.data[prev_base + 7u]);
		elem_prev[2] = vec4(multimesh_buf.data[prev_base + 8u], multimesh_buf.data[prev_base + 9u], multimesh_buf.data[prev_base + 10u], multimesh_buf.data[prev_base + 11u]);
		compose_3x4(parent.prev_transform, elem_prev, world_prev);
	}

	out_rec.transform[0] = world_cur[0];
	out_rec.transform[1] = world_cur[1];
	out_rec.transform[2] = world_cur[2];
	out_rec.prev_transform[0] = world_prev[0];
	out_rec.prev_transform[1] = world_prev[1];
	out_rec.prev_transform[2] = world_prev[2];

	vec3 c0 = vec3(world_cur[0].x, world_cur[1].x, world_cur[2].x);
	vec3 c1 = vec3(world_cur[0].y, world_cur[1].y, world_cur[2].y);
	vec3 c2 = vec3(world_cur[0].z, world_cur[1].z, world_cur[2].z);
	float sx2 = dot(c0, c0);
	float sy2 = dot(c1, c1);
	float sz2 = dot(c2, c2);
	float max_s2 = max(sx2, max(sy2, sz2));
	float max_scale = sqrt(max_s2);
	if (max_s2 > 1e-12) {
		float eps = max_s2 * 1e-4;
		if (abs(sx2 - sy2) > eps || abs(sy2 - sz2) > eps || abs(sx2 - sz2) > eps) {
			out_rec.flags |= NEXTGEO_RECORD_NON_UNIFORM_SCALE;
		}
	}

	if (elem >= params.visible_count || max_scale <= 1e-12) {
		out_rec.flags &= ~NEXTGEO_RECORD_VISIBLE;
	}

	vec3 wmin, wmax, pwmin, pwmax;
	transform_aabb(world_cur, params.local_aabb_min.xyz, params.local_aabb_max.xyz, wmin, wmax);
	transform_aabb(world_prev, params.local_aabb_min.xyz, params.local_aabb_max.xyz, pwmin, pwmax);

	out_rec.world_aabb_min = vec4(wmin, max_scale);
	out_rec.world_aabb_max = vec4(wmax, parent.world_aabb_max.w);
	out_rec.prev_world_aabb_min = vec4(pwmin, parent.prev_world_aabb_min.w);
	out_rec.prev_world_aabb_max = vec4(pwmax, parent.prev_world_aabb_max.w);

	NextGeoInstanceAttributes out_attr;
	out_attr.color = vec4(1.0);
	out_attr.custom_data = vec4(0.0);
	if (has_color) {
		uint cb = cur_base + params.color_offset;
		out_attr.color = vec4(multimesh_buf.data[cb + 0u], multimesh_buf.data[cb + 1u], multimesh_buf.data[cb + 2u], multimesh_buf.data[cb + 3u]);
	}
	if (has_custom) {
		uint db = cur_base + params.custom_offset;
		out_attr.custom_data = vec4(multimesh_buf.data[db + 0u], multimesh_buf.data[db + 1u], multimesh_buf.data[db + 2u], multimesh_buf.data[db + 3u]);
	}
	out_attr.meta = uvec4(params.parent_record, elem, parent_attr.meta.z, parent_attr.meta.w);

	instances.data[dst_slot] = out_rec;
	instance_attrs.data[dst_slot] = out_attr;
}
