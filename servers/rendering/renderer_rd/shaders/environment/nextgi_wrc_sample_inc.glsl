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
		vec2 s = vec2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
		vec2 flipped = (vec2(1.0) - abs(oct.yx)) * s;
		oct = flipped;
	}
	return oct * 0.5 + 0.5;
}

// Trilinear sample of the 8 probes around p, weighted by exact wall visibility (HWRT) and
// Chebyshev moments from the probe visibility atlas.
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
	float max_vis = 0.0;
	bool any_probe_in_bounds = false;
	for (uint i = 0u; i < 8u; i++) {
		ivec3 offset = ivec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
		ivec3 cell = ivec3(base) + offset;
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(16)))) {
			continue;
		}
		any_probe_in_bounds = true;
		uint probe = uint(cell.x) + uint(cell.y) * 16u + uint(cell.z) * 256u + level * 4096u;
		uvec2 tile = uvec2(probe % 128u, probe / 128u);
		vec3 probe_pos = level_origin + (vec3(cell) - vec3(7.5)) * spacing;
		vec3 to_p = p - probe_pos;
		float dist_to_p = length(to_p);
		float chebyshev = 1.0;
		if (dist_to_p > 1e-4) {
#ifdef NEXTGI_TLAS
			// Exact wall-visibility test from p toward the grid probe (reuses D5 nextgi_trace_opaque).
			// Prevents probes across thin dividing walls or outside rooms/courtyards from leaking.
			NextGIHit probe_vis_hit;
			if (dist_to_p > 0.02 && nextgi_trace(p, -to_p / dist_to_p, dist_to_p - 0.01, probe_vis_hit)) {
				chebyshev = 0.0;
			}
#endif
			if (chebyshev > 0.0) {
				vec2 vis_oct = nextgi_wrc_oct_uv(to_p / dist_to_p);
				ivec2 vis_local = clamp(ivec2(vis_oct * 8.0), ivec2(0), ivec2(7));
				vec2 moments = texelFetch(nextgi_wrc_visibility, ivec2(tile) * 8 + vis_local, 0).rg;
				float depth_bias = 0.05 * spacing;
				if (moments.r > 0.0 && dist_to_p > moments.r + depth_bias) {
					float delta = dist_to_p - (moments.r + depth_bias);
					float variance = max(moments.g - moments.r * moments.r, 1e-4);
					float p_vis = variance / (variance + delta * delta);
					chebyshev = p_vis * p_vis;
				}
			}
		}
		if (chebyshev <= 0.0) {
			continue;
		}
		// Probe 8x8 octahedral interior spans [tile*10 + 1, tile*10 + 9); oct in [0, 1] maps texel centers directly.
		vec2 uv = (vec2(tile) * 10.0 + vec2(1.0) + oct * 8.0) / vec2(1280.0);
		vec3 radiance = textureLod(nextgi_wrc_radiance, uv, 0.0).rgb;
		max_vis = max(max_vis, chebyshev);
		vec3 trilinear = mix(vec3(1.0) - frac, frac, vec3(offset));
		float weight = trilinear.x * trilinear.y * trilinear.z * chebyshev;
		accum += radiance * weight;
		weight_sum += weight;
	}
	if (!any_probe_in_bounds) {
		return vec3(0.0);
	}
	if (weight_sum <= 0.0) {
		// All surrounding probes are occluded behind walls: treat as enclosed dark cache hit, not open sky.
		nextgi_wrc_hit_cache = true;
		nextgi_wrc_confidence = 0.99;
		return vec3(0.0);
	}
	nextgi_wrc_hit_cache = true;
	nextgi_wrc_confidence = min(max_vis, 0.99);
	return accum / weight_sum;
}
#endif // NEXTGI_WRC

#endif // NEXTGI_WRC_SAMPLE_INC_GLSL
