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
// - front_face: the hit normal faces the ray. A two-sided coverage hit a containing instance
//   resolved (#186 step 6) uses that card's front face instead: the back of the card is
//   back-facing, so the WRC probe relocation fires on that side of the shell.
// The last trace's SWRTHit, origin and direction stay in the globals below for the hit shading.

#ifndef NEXTGI_SDF_HIT_INC_GLSL
#define NEXTGI_SDF_HIT_INC_GLSL

SWRTHit nextgi_sdf_last;
vec3 nextgi_sdf_origin = vec3(0.0);
// How far the next trace's origin sits past the surface its ray left: the screen march's
// pulled-back resume distance (#186 step 4), 0 for a ray that starts there. Set around one
// trace by nextgi_sdf_trace_noted().
float nextgi_sdf_resume = 0.0;
vec3 nextgi_sdf_direction = vec3(0.0, 0.0, 1.0);
// The surface a gather or reflection ray leaves, for its detail froxel cell.
vec3 nextgi_sdf_surface = vec3(0.0);
// Detail mode traces mesh SDFs first only for gather and reflection rays.
bool nextgi_sdf_detail_ray = false;

bool nextgi_trace(vec3 origin, vec3 direction, float tmax, out NextGIHit hit) {
	bool detail = nextgi_sdf_detail_ray && SWRT_P.trace.z > 0.5 && SWRT_P.extra.y > 0.5;
	SWRTHit traced = swrt_trace(origin, direction, tmax, detail, detail ? swrt_froxel_cell(nextgi_sdf_surface) : 0xffffffffu, nextgi_sdf_resume);
	nextgi_sdf_last = traced;
	nextgi_sdf_origin = origin;
	nextgi_sdf_direction = dot(direction, direction) > 1e-12 ? normalize(direction) : vec3(0.0, 0.0, 1.0);
	hit.hit = (traced.flags & SWRT_HIT_SURFACE) != 0u;
	hit.t = hit.hit ? traced.t : tmax;
	hit.custom_index = ((traced.flags & SWRT_HIT_DETAIL) != 0u && traced.instance != SWRT_NO_INSTANCE) ? traced.instance : 0xFFFFFFFFu;
	hit.geometry_index = 0u;
	hit.primitive = 0u;
	hit.bary = vec2(0.0);
	if ((traced.flags & SWRT_HIT_CARD) != 0u) {
		hit.front_face = (traced.flags & SWRT_HIT_BACKFACE) == 0u;
	} else {
		hit.front_face = dot(traced.normal, direction) <= 0.0;
	}
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

// A clean hit within two voxels of the surface the ray left (at that surface's level): a
// self-intersection candidate. Measured from that surface, not from a resumed trace origin.
bool nextgi_sdf_last_near() {
	vec3 surface_origin = nextgi_sdf_origin - nextgi_sdf_direction * nextgi_sdf_resume;
	return nextgi_sdf_last_clean() && nextgi_sdf_resume + nextgi_sdf_last.t < 2.0 * nextgi_sdf_voxel_at(surface_origin);
}

#endif // NEXTGI_SDF_HIT_INC_GLSL
