// GodotNext: ps_swrt_trace (#186). Not a standalone shader.
//
// Reflection-ray helpers both tiers use (#60 on hardware, #186 in software).

#ifndef NEXTGI_REFLECT_INC_GLSL
#define NEXTGI_REFLECT_INC_GLSL

uint nextgi_reflect_hash(uint p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16;
	return p_x;
}

// The prepass stores roughness in the normal-roughness alpha, folded above 0.5
// for dynamic instances (the velocity flag).
float nextgi_reflect_decode_roughness(float p_alpha) {
	float roughness = p_alpha;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	return roughness / (127.0 / 255.0);
}

vec3 nextgi_reflect_sample_ggx(vec3 p_normal, vec3 p_view, float p_roughness, vec2 p_u) {
	float alpha = max(p_roughness * p_roughness, 1e-3);
	vec3 up = abs(p_normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t1 = normalize(cross(up, p_normal));
	vec3 t2 = cross(p_normal, t1);
	vec3 v_local = vec3(dot(p_view, t1), dot(p_view, t2), dot(p_view, p_normal));
	float phi = 6.2831853 * p_u.x;
	float cos_theta = sqrt((1.0 - p_u.y) / (1.0 + (alpha * alpha - 1.0) * p_u.y));
	float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
	vec3 h_local = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 h = normalize(t1 * h_local.x + t2 * h_local.y + p_normal * h_local.z);
	return reflect(-p_view, h);
}

#endif // NEXTGI_REFLECT_INC_GLSL
