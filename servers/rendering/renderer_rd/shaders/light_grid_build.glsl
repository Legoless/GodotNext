#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: light_grid (#139, plan §21.3 contract M2).
// Two-pass coarse (4x4x4 froxels) + fine packed light-grid build with prepass Hi-Z min/max depth cell culling,
// omni sphere / spot cone / rect barn-door frustum culling, and top-64 highest-intensity overflow handling.

#define TILE_SIZE 64u
#define Z_SLICES 32u
#define COARSE_GROUP 4u
#define MAX_LIGHTS_PER_CELL 64u

#define LIGHT_TYPE_OMNI 0u
#define LIGHT_TYPE_SPOT 1u
#define LIGHT_TYPE_AREA 2u

#define ENTRY_INDEX_MASK 0xFFFFu
#define ENTRY_TYPE_SHIFT 16u
#define ENTRY_TYPE_MASK 0x3u

#define BUILD_FLAG_ORTHOGONAL 1u
#define BUILD_FLAG_HAS_HIZ 2u
#define BUILD_FLAG_CULL_FRONT 4u

#define HIZ_FAR_TOLERANCE 1.0005
#define HIZ_NEAR_TOLERANCE 0.9995
#define SLICE_MARGIN_NEAR 0.9999
#define SLICE_MARGIN_FAR 1.0001
#define BARN_DOOR_OPEN_ANGLE_RAD 1.5706218 // deg_to_rad(89.99)
#define BARN_DOOR_MIN_LENGTH 1e-4

struct PackedLightData {
	vec4 pos_radius;
	vec4 dir_cos_spot;
	vec4 axis_x_hw;
	vec4 axis_y_hh;
	float barn_door_angle_rad;
	float barn_door_length;
	float intensity;
	uint packed_entry;
};

#ifdef MODE_HIZ_8X8

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform HiZPush {
	uvec2 screen_size;
	uvec2 mid_size;
}
hiz_push;

layout(set = 0, binding = 0) uniform texture2D depth_tex;
layout(set = 0, binding = 1) uniform sampler depth_sampler;
layout(set = 0, binding = 2, std430) buffer restrict writeonly HiZMid {
	vec2 data[];
}
hiz_mid;

shared float s_min[64];
shared float s_max[64];

void main() {
	uint lid = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;
	uvec2 pixel = gl_WorkGroupID.xy * 8u + gl_LocalInvocationID.xy;

	float d_min = 1.0;
	float d_max = 0.0;
	if (pixel.x < hiz_push.screen_size.x && pixel.y < hiz_push.screen_size.y) {
		float d = texelFetch(sampler2D(depth_tex, depth_sampler), ivec2(pixel), 0).r;
		d_min = d;
		d_max = d;
	}

	s_min[lid] = d_min;
	s_max[lid] = d_max;
	barrier();

	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (lid < stride) {
			s_min[lid] = min(s_min[lid], s_min[lid + stride]);
			s_max[lid] = max(s_max[lid], s_max[lid + stride]);
		}
		barrier();
	}

	if (lid == 0u && gl_WorkGroupID.x < hiz_push.mid_size.x && gl_WorkGroupID.y < hiz_push.mid_size.y) {
		uint idx = gl_WorkGroupID.y * hiz_push.mid_size.x + gl_WorkGroupID.x;
		hiz_mid.data[idx] = vec2(s_min[0], s_max[0]);
	}
}

#else // MODE_HIZ_TILE, MODE_COARSE, MODE_FINE

layout(set = 0, binding = 0, std140) uniform BuildParams {
	mat4 inv_projection;

	uvec2 screen_size;
	uvec2 mid_size;

	uvec3 grid_dims;
	uint flags;

	uvec3 coarse_dims;
	uint coarse_stride;

	float z_near;
	float z_far;
	float z_scale;
	float z_bias;

	uint light_count;
	uint omni_count;
	uint spot_count;
	uint area_count;
}
params;

#ifdef MODE_HIZ_TILE

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 1, std430) buffer restrict readonly HiZMid {
	vec2 data[];
}
hiz_mid;

layout(set = 0, binding = 2, std430) buffer restrict writeonly TileDepth {
	vec2 data[];
}
tile_depth;

shared float s_min[64];
shared float s_max[64];

float raw_depth_to_linear(float p_raw_z) {
	vec4 v = params.inv_projection * vec4(0.0, 0.0, clamp(p_raw_z, 0.0, 1.0), 1.0);
	return clamp(-v.z / v.w, params.z_near, params.z_far);
}

void main() {
	uvec2 tile = gl_WorkGroupID.xy;
	if (tile.x >= params.grid_dims.x || tile.y >= params.grid_dims.y) {
		return;
	}
	uint lid = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;

	if ((params.flags & BUILD_FLAG_HAS_HIZ) == 0u) {
		if (lid == 0u) {
			tile_depth.data[tile.y * params.grid_dims.x + tile.x] = vec2(params.z_near, params.z_far);
		}
		return;
	}

	uvec2 mid_coord = tile * 8u + gl_LocalInvocationID.xy;
	float d_min = 1.0;
	float d_max = 0.0;
	if (mid_coord.x < params.mid_size.x && mid_coord.y < params.mid_size.y) {
		vec2 v = hiz_mid.data[mid_coord.y * params.mid_size.x + mid_coord.x];
		d_min = v.x;
		d_max = v.y;
	}

	s_min[lid] = d_min;
	s_max[lid] = d_max;
	barrier();

	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (lid < stride) {
			s_min[lid] = min(s_min[lid], s_min[lid + stride]);
			s_max[lid] = max(s_max[lid], s_max[lid + stride]);
		}
		barrier();
	}

	if (lid == 0u) {
		float tile_min_raw = s_min[0];
		float tile_max_raw = s_max[0];
		bool has_opaque = tile_max_raw > 0.0;
		bool has_sky = tile_min_raw <= 0.0;
		bool cull_front = (params.flags & BUILD_FLAG_CULL_FRONT) != 0u;

		vec2 result;
		if (!has_opaque) {
			result = cull_front ? vec2(params.z_far * 2.0, 0.0) : vec2(params.z_near, params.z_far);
		} else {
			float max_lin = has_sky ? params.z_far : raw_depth_to_linear(tile_min_raw);
			float min_lin = cull_front ? raw_depth_to_linear(tile_max_raw) : params.z_near;
			min_lin = min(min_lin, max_lin);
			result = vec2(min_lin, max_lin);
		}
		tile_depth.data[tile.y * params.grid_dims.x + tile.x] = result;
	}
}

#else // MODE_COARSE or MODE_FINE

struct CellBounds {
	vec3 bmin;
	vec3 bmax;
	vec4 planes[4];
};

void slice_depth_bounds(uint p_slice_begin, uint p_slice_end, out float r_z0, out float r_z1) {
	float t0 = float(p_slice_begin) / float(Z_SLICES);
	float t1 = float(p_slice_end) / float(Z_SLICES);
	if ((params.flags & BUILD_FLAG_ORTHOGONAL) != 0u) {
		r_z0 = params.z_near + (params.z_far - params.z_near) * t0;
		r_z1 = params.z_near + (params.z_far - params.z_near) * t1;
	} else {
		float log2_ratio = log2(params.z_far / params.z_near);
		r_z0 = (p_slice_begin == 0u) ? params.z_near : params.z_near * exp2(t0 * log2_ratio);
		r_z1 = (p_slice_end >= Z_SLICES) ? params.z_far : params.z_near * exp2(t1 * log2_ratio);
	}
}

vec3 unproject_corner_at_depth(float p_ndc_x, float p_ndc_y, float p_depth) {
	vec4 p0 = params.inv_projection * vec4(p_ndc_x, p_ndc_y, 1.0, 1.0);
	vec4 p1 = params.inv_projection * vec4(p_ndc_x, p_ndc_y, 0.0, 1.0);
	vec3 v_near = p0.xyz / p0.w;
	vec3 v_far = p1.xyz / p1.w;
	float denom = v_near.z - v_far.z;
	float t = (abs(denom) > 1e-7) ? ((p_depth + v_near.z) / denom) : 0.0;
	return mix(v_near, v_far, t);
}

vec4 make_outward_plane(vec3 p_a, vec3 p_b, vec3 p_c, vec3 p_interior_center) {
	vec3 n = cross(p_b - p_a, p_c - p_a);
	float len = length(n);
	if (len <= 1e-12) {
		return vec4(0.0);
	}
	n /= len;
	float d = -dot(n, p_a);
	if (dot(n, p_interior_center) + d > 0.0) {
		n = -n;
		d = -d;
	}
	return vec4(n, d);
}

CellBounds compute_cell_bounds(uint p_px0, uint p_py0, uint p_px1, uint p_py1, float p_z0, float p_z1) {
	float inv_w = 1.0 / float(max(params.screen_size.x, 1u));
	float inv_h = 1.0 / float(max(params.screen_size.y, 1u));
	float x0 = float(min(p_px0, params.screen_size.x)) * inv_w * 2.0 - 1.0;
	float x1 = float(min(p_px1, params.screen_size.x)) * inv_w * 2.0 - 1.0;
	float y0 = float(min(p_py0, params.screen_size.y)) * inv_h * 2.0 - 1.0;
	float y1 = float(min(p_py1, params.screen_size.y)) * inv_h * 2.0 - 1.0;

	float z0 = p_z0 * SLICE_MARGIN_NEAR;
	float z1 = p_z1 * SLICE_MARGIN_FAR;

	vec3 c0 = unproject_corner_at_depth(x0, y0, z0);
	vec3 c1 = unproject_corner_at_depth(x1, y0, z0);
	vec3 c2 = unproject_corner_at_depth(x0, y1, z0);
	vec3 c3 = unproject_corner_at_depth(x1, y1, z0);
	vec3 c4 = unproject_corner_at_depth(x0, y0, z1);
	vec3 c5 = unproject_corner_at_depth(x1, y0, z1);
	vec3 c6 = unproject_corner_at_depth(x0, y1, z1);
	vec3 c7 = unproject_corner_at_depth(x1, y1, z1);

	vec3 bmin = min(min(min(c0, c1), min(c2, c3)), min(min(c4, c5), min(c6, c7)));
	vec3 bmax = max(max(max(c0, c1), max(c2, c3)), max(max(c4, c5), max(c6, c7)));
	vec3 center = (c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7) * 0.125;

	CellBounds bounds;
	bounds.bmin = bmin;
	bounds.bmax = bmax;
	bounds.planes[0] = make_outward_plane(c0, c2, c4, center);
	bounds.planes[1] = make_outward_plane(c1, c5, c3, center);
	bounds.planes[2] = make_outward_plane(c0, c4, c1, center);
	bounds.planes[3] = make_outward_plane(c2, c3, c6, center);
	return bounds;
}

bool sphere_intersects_cell(vec3 p_pos, float p_radius, CellBounds p_cell) {
	vec3 q = clamp(p_pos, p_cell.bmin, p_cell.bmax);
	vec3 d = p_pos - q;
	if (dot(d, d) > p_radius * p_radius) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if (dot(p_cell.planes[i].xyz, p_pos) + p_cell.planes[i].w > p_radius) {
			return false;
		}
	}
	return true;
}

bool light_intersects_cell(PackedLightData p_light, CellBounds p_cell, bool p_coarse_mode) {
	uint ltype = (p_light.packed_entry >> ENTRY_TYPE_SHIFT) & ENTRY_TYPE_MASK;
	vec3 pos = p_light.pos_radius.xyz;
	float radius = p_coarse_mode ? (p_light.pos_radius.w * 1.001 + 1e-4) : p_light.pos_radius.w;

	if (ltype == LIGHT_TYPE_OMNI) {
		return sphere_intersects_cell(pos, radius, p_cell);
	}

	vec3 center = (p_cell.bmin + p_cell.bmax) * 0.5;
	vec3 extents = (p_cell.bmax - p_cell.bmin) * 0.5;
	vec3 dir = p_light.dir_cos_spot.xyz;

	if (ltype == LIGHT_TYPE_SPOT) {
		if (!sphere_intersects_cell(pos, radius, p_cell)) {
			return false;
		}
		float cos_a = p_light.dir_cos_spot.w;
		if (cos_a <= 0.0) {
			return true;
		}
		vec3 rel = center - pos;
		float proj_r = dot(abs(dir), extents);
		float v1 = dot(rel, dir);
		if (v1 + proj_r < 0.0 || v1 - proj_r > radius) {
			return false;
		}
		float sin_a = p_light.axis_x_hw.x;
		vec3 ref_up = (abs(dir.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 cu = normalize(cross(dir, ref_up));
		vec3 cv = cross(cu, dir);

		vec3 sp0 = cu * cos_a - dir * sin_a;
		if (dot(rel, sp0) - dot(abs(sp0), extents) > (p_coarse_mode ? 1e-4 : 0.0)) {
			return false;
		}
		vec3 sp1 = -cu * cos_a - dir * sin_a;
		if (dot(rel, sp1) - dot(abs(sp1), extents) > (p_coarse_mode ? 1e-4 : 0.0)) {
			return false;
		}
		vec3 sp2 = cv * cos_a - dir * sin_a;
		if (dot(rel, sp2) - dot(abs(sp2), extents) > (p_coarse_mode ? 1e-4 : 0.0)) {
			return false;
		}
		vec3 sp3 = -cv * cos_a - dir * sin_a;
		if (dot(rel, sp3) - dot(abs(sp3), extents) > (p_coarse_mode ? 1e-4 : 0.0)) {
			return false;
		}

		if (p_coarse_mode) {
			// Coarse pass skips bounding-sphere refinement because a sub-box's bounding sphere
			// can extend outside its parent box's bounding sphere; all predicates above are monotone.
			return true;
		}

		float bs_r = length(extents);
		float v_len_sq = dot(rel, rel);
		float b = sqrt(max(v_len_sq - v1 * v1, 0.0));
		if (v1 * cos_a + b * sin_a < 0.0) {
			return v_len_sq <= bs_r * bs_r;
		}
		float signed_dist = b * cos_a - v1 * sin_a;
		return signed_dist <= bs_r;
	}

	// LIGHT_TYPE_AREA with barn-door frustum culling.
	vec3 ax = p_light.axis_x_hw.xyz;
	vec3 ay = p_light.axis_y_hh.xyz;
	float hw = p_light.axis_x_hw.w;
	float hh = p_light.axis_y_hh.w;

	for (int i = 0; i < 4; i++) {
		vec3 pn = p_cell.planes[i].xyz;
		float support = radius + abs(dot(pn, ax)) * hw + abs(dot(pn, ay)) * hh;
		if (dot(pn, pos) + p_cell.planes[i].w > support) {
			return false;
		}
	}

	vec3 rel = center - pos;
	float f_c = dot(rel, dir);
	float f_e = dot(abs(dir), extents);
	float f_min = f_c - f_e;
	float f_max = f_c + f_e;
	if (f_max < 0.0 || f_min > radius) {
		return false;
	}

	float x_c = dot(rel, ax);
	float x_e = dot(abs(ax), extents);
	float x_min = x_c - x_e;
	float x_max = x_c + x_e;

	float y_c = dot(rel, ay);
	float y_e = dot(abs(ay), extents);
	float y_min = y_c - y_e;
	float y_max = y_c + y_e;

	float dx = max(0.0, max(-hw - x_max, x_min - hw));
	float dy = max(0.0, max(-hh - y_max, y_min - hh));
	float df = max(0.0, f_min);
	if (dx * dx + dy * dy + df * df > radius * radius) {
		return false;
	}

	if (p_light.barn_door_angle_rad < BARN_DOOR_OPEN_ANGLE_RAD && p_light.barn_door_length > BARN_DOOR_MIN_LENGTH) {
		float angle = clamp(p_light.barn_door_angle_rad, 0.0, 1.5706963);
		float sin_a = sin(angle);
		float cos_a = cos(angle);
		float z_d = p_light.barn_door_length * cos_a;
		if (z_d > 1e-6) {
			float f_hi = f_max;
			float d_ext = p_light.barn_door_length * sin_a;

			if (p_coarse_mode) {
				// Strictly monotone light-local extent bound under box inclusion (F subset C):
				// rotated barn-door secant slopes vary with f_hi and can invert for corners at f < 0.
				float f_clamped = max(f_hi, 0.0);
				float ext_x;
				float ext_y;
				if (f_clamped <= z_d) {
					float tan_a = sin_a / cos_a;
					ext_x = hw + f_clamped * tan_a;
					ext_y = hh + f_clamped * tan_a;
				} else {
					ext_x = -hw + f_clamped * ((2.0 * hw + d_ext) / z_d);
					ext_y = -hh + f_clamped * ((2.0 * hh + d_ext) / z_d);
				}
				float eps = 1e-4;
				if (x_min > ext_x + eps || -x_max > ext_x + eps || y_min > ext_y + eps || -y_max > ext_y + eps) {
					return false;
				}
			} else {
				float slope_x;
				float rhs_x;
				float slope_y;
				float rhs_y;
				if (f_hi <= z_d) {
					float tan_a = sin_a / cos_a;
					slope_x = tan_a;
					rhs_x = hw;
					slope_y = tan_a;
					rhs_y = hh;
				} else if (f_min >= z_d) {
					slope_x = (2.0 * hw + d_ext) / z_d;
					rhs_x = -hw;
					slope_y = (2.0 * hh + d_ext) / z_d;
					rhs_y = -hh;
				} else {
					float x_at_fhi = -hw + f_hi * ((2.0 * hw + d_ext) / z_d);
					float y_at_fhi = -hh + f_hi * ((2.0 * hh + d_ext) / z_d);
					slope_x = (x_at_fhi - hw) / f_hi;
					rhs_x = hw;
					slope_y = (y_at_fhi - hh) / f_hi;
					rhs_y = hh;
				}

				vec3 n0 = ax - dir * slope_x;
				if (dot(rel, n0) - dot(abs(n0), extents) > rhs_x) {
					return false;
				}
				vec3 n1 = -ax - dir * slope_x;
				if (dot(rel, n1) - dot(abs(n1), extents) > rhs_x) {
					return false;
				}
				vec3 n2 = ay - dir * slope_y;
				if (dot(rel, n2) - dot(abs(n2), extents) > rhs_y) {
					return false;
				}
				vec3 n3 = -ay - dir * slope_y;
				if (dot(rel, n3) - dot(abs(n3), extents) > rhs_y) {
					return false;
				}
			}
		}
	}

	return true;
}

#ifdef MODE_COARSE

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 1, std430) buffer restrict readonly PackedLights {
	PackedLightData data[];
}
packed_lights;

layout(set = 0, binding = 2, std430) buffer restrict readonly TileDepth {
	vec2 data[];
}
tile_depth;

layout(set = 0, binding = 3, std430) buffer restrict writeonly CoarseCounts {
	uint data[];
}
coarse_counts;

layout(set = 0, binding = 4, std430) buffer restrict writeonly CoarseIndices {
	uint data[];
}
coarse_indices;

shared vec3 s_bmin;
shared vec3 s_bmax;
shared vec4 s_planes[4];
shared uint s_active;
shared uint s_count;

void main() {
	uvec3 ccoord = gl_WorkGroupID;
	if (any(greaterThanEqual(ccoord, params.coarse_dims))) {
		return;
	}
	uint tid = gl_LocalInvocationID.x;
	uint coarse_idx = (ccoord.z * params.coarse_dims.y + ccoord.y) * params.coarse_dims.x + ccoord.x;

	if (tid == 0u) {
		s_count = 0u;
		s_active = 0u;

		uint tx0 = ccoord.x * COARSE_GROUP;
		uint ty0 = ccoord.y * COARSE_GROUP;
		uint tz0 = ccoord.z * COARSE_GROUP;
		uint tx1 = min(tx0 + COARSE_GROUP, params.grid_dims.x);
		uint ty1 = min(ty0 + COARSE_GROUP, params.grid_dims.y);
		uint tz1 = min(tz0 + COARSE_GROUP, params.grid_dims.z);

		float cz0 = 0.0;
		float cz1 = 0.0;
		slice_depth_bounds(tz0, tz1, cz0, cz1);

		float min_tile_z = 3.402823466e+38;
		float max_tile_z = 0.0;
		for (uint ty = ty0; ty < ty1; ty++) {
			for (uint tx = tx0; tx < tx1; tx++) {
				vec2 td = tile_depth.data[ty * params.grid_dims.x + tx];
				if (td.y > 0.0) {
					min_tile_z = min(min_tile_z, td.x);
					max_tile_z = max(max_tile_z, td.y);
				}
			}
		}

		if (max_tile_z > 0.0 && !(cz0 > max_tile_z * HIZ_FAR_TOLERANCE || cz1 < min_tile_z * HIZ_NEAR_TOLERANCE)) {
			float z0 = max(cz0, min_tile_z * HIZ_NEAR_TOLERANCE);
			float z1 = min(cz1, max_tile_z * HIZ_FAR_TOLERANCE);
			if (z1 < z0) {
				z1 = z0;
			}
			uint px0 = tx0 * TILE_SIZE;
			uint py0 = ty0 * TILE_SIZE;
			uint px1 = min(tx1 * TILE_SIZE, params.screen_size.x);
			uint py1 = min(ty1 * TILE_SIZE, params.screen_size.y);

			CellBounds cb = compute_cell_bounds(px0, py0, px1, py1, z0, z1);
			s_bmin = cb.bmin;
			s_bmax = cb.bmax;
			s_planes[0] = cb.planes[0];
			s_planes[1] = cb.planes[1];
			s_planes[2] = cb.planes[2];
			s_planes[3] = cb.planes[3];
			s_active = 1u;
		}
	}

	barrier();

	if (s_active == 0u || params.light_count == 0u) {
		if (tid == 0u) {
			coarse_counts.data[coarse_idx] = 0u;
		}
		return;
	}

	CellBounds cb;
	cb.bmin = s_bmin;
	cb.bmax = s_bmax;
	cb.planes[0] = s_planes[0];
	cb.planes[1] = s_planes[1];
	cb.planes[2] = s_planes[2];
	cb.planes[3] = s_planes[3];

	uint coarse_base = coarse_idx * params.coarse_stride;
	for (uint li = tid; li < params.light_count; li += 128u) {
		PackedLightData L = packed_lights.data[li];
		if (light_intersects_cell(L, cb, true)) {
			uint slot = atomicAdd(s_count, 1u);
			if (slot < params.coarse_stride) {
				coarse_indices.data[coarse_base + slot] = li;
			}
		}
	}

	barrier();

	if (tid == 0u) {
		coarse_counts.data[coarse_idx] = s_count;
	}
}

#endif // MODE_COARSE

#ifdef MODE_FINE

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 1, std430) buffer restrict readonly PackedLights {
	PackedLightData data[];
}
packed_lights;

layout(set = 0, binding = 2, std430) buffer restrict readonly TileDepth {
	vec2 data[];
}
tile_depth;

layout(set = 0, binding = 3, std430) buffer restrict readonly CoarseCounts {
	uint data[];
}
coarse_counts;

layout(set = 0, binding = 4, std430) buffer restrict readonly CoarseIndices {
	uint data[];
}
coarse_indices;

layout(set = 0, binding = 5, std430) buffer restrict writeonly CellHeaders {
	uvec2 data[];
}
cell_headers;

layout(set = 0, binding = 6, std430) buffer restrict PackedIndices {
	uint data[];
}
packed_indices;

layout(set = 0, binding = 7, std430) buffer restrict GridStatsBuffer {
	uint overflow_cells;
	uint overflow_lights;
	uint total_assignments;
	uint max_cell_count;
	uint sink;
	uint pad0;
	uint pad1;
	uint pad2;
}
stats;

bool is_stronger(float p_int_a, uint p_li_a, float p_int_b, uint p_li_b) {
	return (p_int_a > p_int_b) || (p_int_a == p_int_b && p_li_a < p_li_b);
}

bool is_weaker(float p_int_a, uint p_li_a, float p_int_b, uint p_li_b) {
	return (p_int_a < p_int_b) || (p_int_a == p_int_b && p_li_a > p_li_b);
}

uint entry_to_global_index(uint p_entry) {
	uint ltype = (p_entry >> ENTRY_TYPE_SHIFT) & ENTRY_TYPE_MASK;
	uint idx = p_entry & ENTRY_INDEX_MASK;
	if (ltype == LIGHT_TYPE_OMNI) {
		return idx;
	}
	if (ltype == LIGHT_TYPE_SPOT) {
		return params.omni_count + idx;
	}
	return params.omni_count + params.spot_count + idx;
}

void main() {
	uvec3 cell = gl_WorkGroupID * COARSE_GROUP + gl_LocalInvocationID;
	if (any(greaterThanEqual(cell, params.grid_dims))) {
		return;
	}

	uint cell_idx = (cell.z * params.grid_dims.y + cell.y) * params.grid_dims.x + cell.x;
	uint base = cell_idx * MAX_LIGHTS_PER_CELL;

	uvec3 ccoord = gl_WorkGroupID;
	uint coarse_idx = (ccoord.z * params.coarse_dims.y + ccoord.y) * params.coarse_dims.x + ccoord.x;
	uint c_count = coarse_counts.data[coarse_idx];
	if (c_count == 0u || params.light_count == 0u) {
		cell_headers.data[cell_idx] = uvec2(base, 0u);
		return;
	}

	float slice_z0 = 0.0;
	float slice_z1 = 0.0;
	slice_depth_bounds(cell.z, cell.z + 1u, slice_z0, slice_z1);

	vec2 td = tile_depth.data[cell.y * params.grid_dims.x + cell.x];
	if (td.y <= 0.0 || slice_z0 > td.y * HIZ_FAR_TOLERANCE || slice_z1 < td.x * HIZ_NEAR_TOLERANCE) {
		cell_headers.data[cell_idx] = uvec2(base, 0u);
		return;
	}

	float z0 = max(slice_z0, td.x * HIZ_NEAR_TOLERANCE);
	float z1 = min(slice_z1, td.y * HIZ_FAR_TOLERANCE);
	if (z1 < z0) {
		z1 = z0;
	}

	uint px0 = cell.x * TILE_SIZE;
	uint py0 = cell.y * TILE_SIZE;
	uint px1 = min(px0 + TILE_SIZE, params.screen_size.x);
	uint py1 = min(py0 + TILE_SIZE, params.screen_size.y);
	CellBounds bounds = compute_cell_bounds(px0, py0, px1, py1, z0, z1);

	bool coarse_overflowed = c_count > params.coarse_stride;
	uint iter_count = coarse_overflowed ? params.light_count : c_count;
	uint coarse_base = coarse_idx * params.coarse_stride;

	uint count = 0u;
	uint overflow = 0u;
	float min_int = 3.402823466e+38;
	uint min_li = 0u;
	uint min_slot = 0u;

	for (uint k = 0u; k < iter_count; k++) {
		uint li = coarse_overflowed ? k : coarse_indices.data[coarse_base + k];
		PackedLightData L = packed_lights.data[li];
		if (!light_intersects_cell(L, bounds, false)) {
			continue;
		}
		if (count < MAX_LIGHTS_PER_CELL) {
			packed_indices.data[base + count] = L.packed_entry;
			if (count == 0u || is_weaker(L.intensity, li, min_int, min_li)) {
				min_int = L.intensity;
				min_li = li;
				min_slot = count;
			}
			count++;
		} else {
			overflow++;
			if (is_stronger(L.intensity, li, min_int, min_li)) {
				packed_indices.data[base + min_slot] = L.packed_entry;
				min_int = 3.402823466e+38;
				min_li = 0u;
				min_slot = 0u;
				for (uint s = 0u; s < MAX_LIGHTS_PER_CELL; s++) {
					uint s_li = entry_to_global_index(packed_indices.data[base + s]);
					float s_int = packed_lights.data[s_li].intensity;
					if (s == 0u || is_weaker(s_int, s_li, min_int, min_li)) {
						min_int = s_int;
						min_li = s_li;
						min_slot = s;
					}
				}
			}
		}
	}

	cell_headers.data[cell_idx] = uvec2(base, count);

	// GodotNext (#277): always consume atomic return values via an impossible-value sink for D3D12 GBV.
	if (overflow > 0u) {
		uint prev_c = atomicAdd(stats.overflow_cells, 1u);
		uint prev_l = atomicAdd(stats.overflow_lights, overflow);
		if ((prev_c & prev_l) == 0xFFFFFFFFu) {
			stats.sink = prev_c ^ prev_l;
		}
	}
	if (count > 0u) {
		uint prev_a = atomicAdd(stats.total_assignments, count);
		uint prev_m = atomicMax(stats.max_cell_count, count);
		if ((prev_a & prev_m) == 0xFFFFFFFFu) {
			stats.sink = prev_a ^ prev_m;
		}
	}
}

#endif // MODE_FINE

#endif // !MODE_HIZ_TILE

#endif // !MODE_HIZ_8X8
