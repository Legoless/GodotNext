#[compute]
#version 460
#extension GL_EXT_ray_query : require

// RD ray-query smoke kernel (#18): opaque traversal, one rayQueryProceedEXT,
// committed getters only (the D5 rules in nextgi_rt_inc.glsl).

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

struct SmokeRay {
	vec4 origin_tmin;
	vec4 dir_tmax;
};

layout(set = 0, binding = 1, std430) restrict readonly buffer Rays {
	SmokeRay data[];
}
rays;

// Q17 record, 32 bytes, shared with harness/metrics (godotnext_metrics.rays).
struct Result {
	float t;
	uint custom_index;
	uint primitive;
	float u;
	float v;
	uint front_face;
	uint committed;
	uint candidate_count;
};

layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	Result data[];
}
results;

layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	SmokeRay smoke_ray = rays.data[i];
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, smoke_ray.origin_tmin.xyz, smoke_ray.origin_tmin.w, smoke_ray.dir_tmax.xyz, smoke_ray.dir_tmax.w);
	rayQueryProceedEXT(rq);

	Result result;
	result.t = -1.0;
	result.custom_index = 0u;
	result.primitive = 0u;
	result.u = 0.0;
	result.v = 0.0;
	result.front_face = 0u;
	result.committed = 0u;
	result.candidate_count = 0u;
	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
		result.t = rayQueryGetIntersectionTEXT(rq, true);
		result.custom_index = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
		result.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
		result.u = bary.x;
		result.v = bary.y;
		result.front_face = rayQueryGetIntersectionFrontFaceEXT(rq, true) ? 1u : 0u;
		result.committed = 1u;
		result.candidate_count = 1u;
	}
	results.data[i] = result;
}
