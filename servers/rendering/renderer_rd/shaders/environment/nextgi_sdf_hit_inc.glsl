// GodotNext: ps_swrt_trace (#186). Not a standalone shader.
//
// The software tier's nextgi_trace(): the SDF trace (nextgi_sdf_trace_inc.glsl) behind the
// interface the shared kernels (nextgi_gather_body_inc.glsl, nextgi_wrc_update_inc.glsl) call on
// both tiers. Included by the software siblings only, after nextgi_hit_inc.glsl and
// nextgi_sdf_trace_inc.glsl.
//
// NextGIHit from an SDF hit:
// - hit: the trace stopped on a surface, step-limit hits included (#186 step 2);
// - custom_index: the instance of a detail (mesh-SDF) hit, else 0xFFFFFFFF (a global-SDF hit);
// - front_face: the hit normal faces the ray. An accepted two-sided coverage hit carries its
//   outward normal, so it is front-facing (the SWRT two-sided rule, #186 step 6).
// The last trace's SWRTHit, origin and direction stay in the globals below for the hit shading.

#ifndef NEXTGI_SDF_HIT_INC_GLSL
#define NEXTGI_SDF_HIT_INC_GLSL

SWRTHit nextgi_sdf_last;
vec3 nextgi_sdf_origin = vec3(0.0);
vec3 nextgi_sdf_direction = vec3(0.0, 0.0, 1.0);
// The surface a gather or reflection ray leaves, for its detail froxel cell.
vec3 nextgi_sdf_surface = vec3(0.0);
// Detail mode traces mesh SDFs first only for gather and reflection rays.
bool nextgi_sdf_detail_ray = false;

bool nextgi_trace(vec3 origin, vec3 direction, float tmax, out NextGIHit hit) {
	bool detail = nextgi_sdf_detail_ray && SWRT_P.trace.z > 0.5 && SWRT_P.extra.y > 0.5;
	SWRTHit traced = swrt_trace(origin, direction, tmax, detail, detail ? swrt_froxel_cell(nextgi_sdf_surface) : 0xffffffffu);
	nextgi_sdf_last = traced;
	nextgi_sdf_origin = origin;
	nextgi_sdf_direction = dot(direction, direction) > 1e-12 ? normalize(direction) : vec3(0.0, 0.0, 1.0);
	hit.hit = (traced.flags & SWRT_HIT_SURFACE) != 0u;
	hit.t = hit.hit ? traced.t : tmax;
	hit.custom_index = ((traced.flags & SWRT_HIT_DETAIL) != 0u && traced.instance != SWRT_NO_INSTANCE) ? traced.instance : 0xFFFFFFFFu;
	hit.geometry_index = 0u;
	hit.primitive = 0u;
	hit.bary = vec2(0.0);
	hit.front_face = dot(traced.normal, direction) <= 0.0;
	return hit.hit;
}

// The voxel size of the global-SDF level that holds p (the coarsest active level outside them).
float nextgi_sdf_voxel_at(vec3 p) {
	uint level = swrt_level_at(p);
	if (level == SWRT_NO_LEVEL) {
		level = max(swrt_active_levels(), 1u) - 1u;
	}
	return swrt_level_voxel(level);
}

// The last trace stopped on a surface, without running out of steps.
bool nextgi_sdf_last_clean() {
	return (nextgi_sdf_last.flags & SWRT_HIT_SURFACE) != 0u && (nextgi_sdf_last.flags & SWRT_HIT_EXHAUSTED) == 0u;
}

// A clean hit within two voxels of the origin's level: a self-intersection candidate.
bool nextgi_sdf_last_near() {
	return nextgi_sdf_last_clean() && nextgi_sdf_last.t < 2.0 * nextgi_sdf_voxel_at(nextgi_sdf_origin);
}

#endif // NEXTGI_SDF_HIT_INC_GLSL
