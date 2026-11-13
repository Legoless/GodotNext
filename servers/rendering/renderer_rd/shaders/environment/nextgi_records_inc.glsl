// GodotNext: ps_swrt_trace (#186). Not a standalone shader, and free of ray-query tokens, so
// the software tier's siblings include it too.

#ifndef NEXTGI_RECORDS_INC_GLSL
#define NEXTGI_RECORDS_INC_GLSL

// Mirrors of the §6.3 records, the §8.3 cards and the D14 light (#31). std430, same layout as the C++ structs.
struct NextGIInstanceRecord {
	float object_to_world[12];
	uint surface_base;
	uint surface_count;
	uint card_set;
	uint page_table;
	uint rt_flags;
	float origin_offset;
	uint tlas_index;
	uint arena_chunk;
};

struct NextGICard {
	vec4 origin;
	vec4 axis_u;
	vec4 axis_v;
	vec4 axis_n;
	uvec4 atlas_rect;
};

struct NextGICardSet {
	uint first_card;
	uint card_count;
	float texel_size;
	float capture_scale;
};

struct NextGILight {
	vec4 color_energy;
	vec4 position_inv_range;
	vec4 direction_type;
	float attenuation;
	float cos_spot;
	float inv_spot_atten;
	uint flags;
};

// nextgi_shadow_ray is defined by the relight shader, not here. This include is pulled into the
// self-test, whose source is required to contain exactly one rayQueryProceedEXT.

struct NextGISurfaceRecord {
	uint vertex_offset;
	uint index_offset;
	uint triangle_count;
	uint flags;
};

#endif // NEXTGI_RECORDS_INC_GLSL

// GodotNext: nextgi_card_normal (#251). An object-space normal to world space through the
// inverse transpose of the record's row-major object-to-world basis, so it is exact for
// rotation, mirroring and non-uniform scale. With rows r0, r1 and r2, the rows of that inverse
// transpose are (r1 x r2, r2 x r0, r0 x r1) divided by the determinant; only its sign matters
// before the caller normalizes.
vec3 nextgi_record_normal_to_world(NextGIInstanceRecord rec, vec3 n) {
	vec3 r0 = vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]);
	vec3 r1 = vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]);
	vec3 r2 = vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]);
	vec3 world = vec3(dot(cross(r1, r2), n), dot(cross(r2, r0), n), dot(cross(r0, r1), n));
	return dot(r0, cross(r1, r2)) < 0.0 ? -world : world;
}
