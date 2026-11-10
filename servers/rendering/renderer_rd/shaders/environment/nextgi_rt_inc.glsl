// NextGI ray-query include (D5). Not a standalone shader: the includer declares
// #version 460 and GL_EXT_ray_query, then defines NEXTGI_TLAS as its
// accelerationStructureEXT uniform, then includes this file.
//
// Rules for every includer:
// - Opaque-only traversal: gl_RayFlagsOpaqueEXT, one rayQueryProceedEXT, no candidate loop.
// - Workgroup shared memory stays at or under 8 KiB.
// - Do not fetch triangle vertex positions, do not build an acceleration structure
//   from a 64-bit address (OpConvertUToAccelerationStructureKHR), and do not read
//   a shader-binding-table record offset. SPIRV-Cross MSL cannot translate those.
//   The source lint that enforces this is documented in docs/godotnext/fleet.md.

#ifndef NEXTGI_RT_INC_GLSL
#define NEXTGI_RT_INC_GLSL

struct NextGIHit {
	float t;
	uint custom_index;
	uint geometry_index;
	uint primitive;
	vec2 bary;
	bool front_face;
	bool hit;
};

NextGIHit nextgi_trace_opaque(vec3 origin, float tmin, vec3 dir, float tmax, uint mask) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, NEXTGI_TLAS, gl_RayFlagsOpaqueEXT, mask, origin, tmin, dir, tmax);
	// Opaque flags commit the closest hit inside this single proceed. No candidate loop.
	rayQueryProceedEXT(rq);

	NextGIHit hit;
	uint committed = rayQueryGetIntersectionTypeEXT(rq, true);
	hit.hit = committed != gl_RayQueryCommittedIntersectionNoneEXT;
	hit.t = rayQueryGetIntersectionTEXT(rq, true);
	hit.custom_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
	hit.geometry_index = rayQueryGetIntersectionGeometryIndexEXT(rq, true);
	hit.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
	hit.bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
	hit.front_face = rayQueryGetIntersectionFrontFaceEXT(rq, true);
	return hit;
}

#ifdef MODE_FAR_FIELD
#define NEXTGI_NEAR_TRACE_RESTORE NEXTGI_TLAS
#undef NEXTGI_TLAS
#define NEXTGI_TLAS nextgi_tlas
NextGIHit nextgi_trace_near_opaque(vec3 origin, vec3 dir, float tmax) {
	return nextgi_trace_opaque(origin, 0.0, dir, tmax, 0xFFu);
}
#undef NEXTGI_TLAS
#define NEXTGI_TLAS NEXTGI_NEAR_TRACE_RESTORE
#undef NEXTGI_NEAR_TRACE_RESTORE
#endif

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

// Same query as nextgi_trace_opaque. The proceed stays in that one function.
bool nextgi_trace(vec3 origin, vec3 direction, float tmax, out NextGIHit hit) {
	hit = nextgi_trace_opaque(origin, 0.0, direction, tmax, 0xFFu);
	return hit.hit;
}

// GodotNext: ps_swrt_trace (#186). The WRC lookups carry no ray query, so the
// software tier includes them without this file.
#include "nextgi_wrc_sample_inc.glsl"

#endif // NEXTGI_RT_INC_GLSL
