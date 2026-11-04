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

// Mirrors of the §6.3 records, the §8.3 cards and the D14 light (#31). std430, same layout as the C++ structs.
struct NextGIInstanceRecord {
	float object_to_world[12];
	uint surface_base;
	uint surface_count;
	uint card_set;
	uint page_table;
	uint rt_flags;
	float origin_offset;
	uint tlas_index;
	uint arena_chunk;
};

struct NextGICard {
	vec4 origin;
	vec4 axis_u;
	vec4 axis_v;
	vec4 axis_n;
	uvec4 atlas_rect;
};

struct NextGICardSet {
	uint first_card;
	uint card_count;
	float texel_size;
	float capture_scale;
};

struct NextGILight {
	vec4 color_energy;
	vec4 position_inv_range;
	vec4 direction_type;
	float attenuation;
	float cos_spot;
	float inv_spot_atten;
	uint flags;
};

// nextgi_shadow_ray is defined by the relight shader, not here. This include is pulled into the
// self-test, whose source is required to contain exactly one rayQueryProceedEXT.

struct NextGISurfaceRecord {
	uint vertex_offset;
	uint index_offset;
	uint triangle_count;
	uint flags;
};

// Same query as nextgi_trace_opaque. The proceed stays in that one function.
bool nextgi_trace(vec3 origin, vec3 direction, float tmax, out NextGIHit hit) {
	hit = nextgi_trace_opaque(origin, 0.0, direction, tmax, 0xFFu);
	return hit.hit;
}

// Set by nextgi_wrc_sample. False means the caller should use the sky.
bool nextgi_wrc_hit_cache = false;

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
	return accum / weight_sum;
}
#endif // NEXTGI_WRC

#endif // NEXTGI_RT_INC_GLSL
