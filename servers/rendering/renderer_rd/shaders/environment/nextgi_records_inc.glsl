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
