#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// Startup 64-ray HWRT self-test (#36). One workgroup, one ray per thread.
// The host compares the records with the analytic table. Same traversal rules
// as nextgi_rt_inc.glsl: opaque only, one rayQueryProceedEXT.

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas

struct NextGIRay {
	vec4 origin_tmin;
	vec4 dir_tmax;
};

layout(set = 0, binding = 1, std430) restrict readonly buffer NextGIRays {
	NextGIRay data[];
}
nextgi_rays;

// 32 bytes per ray. uint stands in for the bools in NextGIHit so the std430 layout stays explicit.
struct NextGIRayResult {
	float t;
	uint custom_index;
	uint geometry_index;
	uint primitive;
	vec2 bary;
	uint front_face;
	uint hit;
};

layout(set = 0, binding = 2, std430) restrict writeonly buffer NextGIRayResults {
	NextGIRayResult data[];
}
nextgi_results;

#include "nextgi_rt_inc.glsl"

void main() {
	uint index = gl_GlobalInvocationID.x;
	NextGIRay ray = nextgi_rays.data[index];
	NextGIHit hit = nextgi_trace_opaque(ray.origin_tmin.xyz, ray.origin_tmin.w, ray.dir_tmax.xyz, ray.dir_tmax.w, 0xFFu);

	NextGIRayResult result;
	result.t = hit.t;
	result.custom_index = hit.custom_index;
	result.geometry_index = hit.geometry_index;
	result.primitive = hit.primitive;
	result.bary = hit.bary;
	result.front_face = hit.front_face ? 1u : 0u;
	result.hit = hit.hit ? 1u : 0u;
	nextgi_results.data[index] = result;
}
