// GodotNext: ps_swrt_trace (#186). Not a standalone shader.
//
// The hit record that nextgi_trace() returns on both tiers: the hardware tier fills it from a
// ray query (nextgi_rt_inc.glsl), the software tier from the SDF trace (nextgi_sdf_hit_inc.glsl).

#ifndef NEXTGI_HIT_INC_GLSL
#define NEXTGI_HIT_INC_GLSL

struct NextGIHit {
	float t;
	uint custom_index;
	uint geometry_index;
	uint primitive;
	vec2 bary;
	bool front_face;
	bool hit;
};

#endif // NEXTGI_HIT_INC_GLSL
