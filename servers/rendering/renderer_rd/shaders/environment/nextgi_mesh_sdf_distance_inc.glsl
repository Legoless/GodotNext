// GodotNext: ps_swrt_trace (#186, plan §21.2 L4). Not a standalone shader.
//
// The world distance of one mesh-SDF instance, from a sample in mesh-local
// units. Shared by the global SDF composite (nextgi_global_sdf.glsl) and the
// SWRT detail trace (nextgi_sdf_trace_inc.glsl). The CPU twin is
// NextGIGlobalSDF::mesh_world_distance(); NextGIGlobalSDF::instance_axis_scale()
// computes axis_scale.
//
// axis_scale is the world length of one local unit along each local axis: the
// basis column lengths, or, for a sheared basis, its smallest singular value on
// every axis. A local distance times the largest axis scale overestimates the
// world distance of a non-uniformly scaled instance (rays tunnel through
// stretched boxes). Every term below is a lower bound instead:
// - The local distance times the smallest axis scale (the field's Lipschitz
//   bound in world space).
// - Outside the SDF bounds, the clamp offset in world length joins that bound
//   at the clamped point: sqrt(e^2 + d(q)^2). The world projection onto the
//   (oriented) bounds is the clamp, so the sum of squares still holds.
// - Plan L4: "Non-uniform scale is bounded by the distance to the oriented
//   bounds." The mesh lies inside surface_min..surface_max (the SDF bounds less
//   the band the builder pads them by), so the world distance to that oriented
//   box bounds the distance to the mesh. It is exact for a box, the common way
//   to build stretched floors and walls.
// The result is the largest of them. Inside its bounds a non-box mesh under a
// non-uniform scale reads closer than it is along its stretched axes, never
// farther.

#ifndef NEXTGI_MESH_SDF_DISTANCE_INC_GLSL
#define NEXTGI_MESH_SDF_DISTANCE_INC_GLSL

float nextgi_mesh_sdf_min_scale(vec3 axis_scale) {
	return max(min(axis_scale.x, min(axis_scale.y, axis_scale.z)), 1e-6);
}

float nextgi_mesh_sdf_world_distance(vec3 local, vec3 bounds_min, vec3 bounds_max, vec3 surface_min, vec3 surface_max, float sampled, vec3 axis_scale) {
	float distance = sampled * nextgi_mesh_sdf_min_scale(axis_scale);
	vec3 excess = (local - clamp(local, bounds_min, bounds_max)) * axis_scale;
	float excess_squared = dot(excess, excess);
	if (excess_squared > 0.0) {
		float boundary = max(distance, 0.0);
		distance = sqrt(excess_squared + boundary * boundary);
	}
	vec3 outside = (local - clamp(local, surface_min, surface_max)) * axis_scale;
	float oriented = length(outside);
	if (oriented > 0.0) {
		distance = max(distance, oriented);
	}
	return distance;
}

// The mesh's own bounds: the SDF bounds less the mip-0 band the builder pads
// them by, never inverted.
void nextgi_mesh_sdf_surface_bounds(vec3 bounds_min, vec3 bounds_max, float band, out vec3 r_surface_min, out vec3 r_surface_max) {
	vec3 center = (bounds_min + bounds_max) * 0.5;
	r_surface_min = min(bounds_min + vec3(band), center);
	r_surface_max = max(bounds_max - vec3(band), center);
}

#endif // NEXTGI_MESH_SDF_DISTANCE_INC_GLSL
