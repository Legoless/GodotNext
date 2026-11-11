#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// SRC world-space relight (#31, D14, §2.1 line 2b). One 8x8 workgroup.
// nextgi_shadow_ray below uses gl_RayFlagsOpaqueEXT and one rayQueryProceedEXT.
// No shared memory. No position fetch.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

// Opaque shadow ray. True when something is in the way. One proceed, no candidate loop.
bool nextgi_shadow_ray(vec3 origin, vec3 direction, float tmax) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, NEXTGI_TLAS, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, origin, 0.0, direction, tmax);
	rayQueryProceedEXT(rq);
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

#include "nextgi_src_relight_inc.glsl"
