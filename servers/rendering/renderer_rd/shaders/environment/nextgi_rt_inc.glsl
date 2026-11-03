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

#endif
