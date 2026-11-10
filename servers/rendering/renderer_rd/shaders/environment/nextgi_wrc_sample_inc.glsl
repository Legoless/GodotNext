// GodotNext: ps_swrt_trace (#186). Not a standalone shader.
//
// WRC (D15) handoff and probe lookups, shared by the hardware gather and the
// software trace. With NEXTGI_WRC defined, the includer declares the WRC
// params buffer as nextgi_wrc (camera, sky, origin_spacing, ...) and the
// nextgi_wrc_radiance and nextgi_wrc_visibility samplers.

#ifndef NEXTGI_WRC_SAMPLE_INC_GLSL
#define NEXTGI_WRC_SAMPLE_INC_GLSL

// Set by nextgi_wrc_sample. False means the caller should use the sky.
bool nextgi_wrc_hit_cache = false;
// Chebyshev weight of a cache hit, kept below 1 so a screen or SRC hit stays stronger.
float nextgi_wrc_confidence = 0.0;

#ifdef NEXTGI_WRC
// Diffuse handoff distance. Negative means the cache is not valid and the caller keeps its own tmax.
float nextgi_wrc_handoff(vec3 origin) {
	if (nextgi_wrc.camera.w < 0.5) {
		return -1.0;
	}
	float dist = length(origin - nextgi_wrc.camera.xyz);
	float spacing = 1.0;
	if (dist > 8.0) {
		spacing = 2.0;
	}
	if (dist > 16.0) {
		spacing = 4.0;
	}
	if (dist > 32.0) {
		spacing = 8.0;
	}
	return 1.5 * spacing;
}

vec2 nextgi_wrc_oct_uv(vec3 direction) {
	vec3 d = normalize(direction);
	float sum = abs(d.x) + abs(d.y) + abs(d.z);
	vec2 oct = d.xy / max(sum, 0.0001);
	if (d.z < 0.0) {
		vec2 flipped = (vec2(1.0) - abs(oct.yx)) * sign(oct.xy);
		oct = flipped;
	}
	return oct * 0.5 + 0.5;
}

// Trilinear sample of the 8 probes around p, weighted by Chebyshev visibility from the moments.
vec3 nextgi_wrc_sample(vec3 p, vec3 dir) {
	nextgi_wrc_hit_cache = false;
	nextgi_wrc_confidence = 0.0;
	if (nextgi_wrc.camera.w < 0.5) {
		return vec3(0.0);
	}
	float dist = length(p - nextgi_wrc.camera.xyz);
	float spacing = 1.0;
	uint level = 0u;
	if (dist > 8.0) {
		spacing = 2.0;
		level = 1u;
	}
	if (dist > 16.0) {
		spacing = 4.0;
		level = 2u;
	}
	if (dist > 32.0) {
		spacing = 8.0;
		level = 3u;
	}
	vec3 level_origin = floor(nextgi_wrc.origin_spacing.xyz / spacing) * spacing;
	vec3 local = (p - level_origin) / spacing + vec3(7.5);
	vec3 base = floor(local);
	vec3 frac = local - base;
	vec2 oct = nextgi_wrc_oct_uv(dir);
	vec3 accum = vec3(0.0);
	float weight_sum = 0.0;
	for (uint i = 0u; i < 8u; i++) {
		ivec3 offset = ivec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
		ivec3 cell = ivec3(base) + offset;
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(16)))) {
			continue;
		}
		uint probe = uint(cell.x) + uint(cell.y) * 16u + uint(cell.z) * 256u + level * 4096u;
		uvec2 tile = uvec2(probe % 128u, probe / 128u);
		vec2 uv = (vec2(tile) * 10.0 + vec2(1.0) + oct * 8.0 + vec2(0.5)) / vec2(1280.0);
		vec2 vis_uv = (vec2(tile) * 8.0 + oct * 7.0 + vec2(0.5)) / vec2(1024.0);
		vec3 radiance = textureLod(nextgi_wrc_radiance, uv, 0.0).rgb;
		vec2 moments = textureLod(nextgi_wrc_visibility, vis_uv, 0.0).rg;
		float variance = max(moments.g - moments.r * moments.r, 0.0);
		float chebyshev = variance / (variance + 0.05);
		vec3 trilinear = mix(vec3(1.0) - frac, frac, vec3(offset));
		float weight = trilinear.x * trilinear.y * trilinear.z * max(chebyshev, 0.05);
		accum += radiance * weight;
		weight_sum += weight;
	}
	if (weight_sum <= 0.0) {
		return vec3(0.0);
	}
	nextgi_wrc_hit_cache = true;
	nextgi_wrc_confidence = min(weight_sum / 8.0, 0.99);
	return accum / weight_sum;
}
#endif // NEXTGI_WRC

#endif // NEXTGI_WRC_SAMPLE_INC_GLSL
