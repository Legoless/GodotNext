// GodotNext: path_tracer (#167). The product path tracer's paths (RendererRD::PathTracer). Not a standalone
// shader. The includer declares the D16 buffers (records, surfaces, chunk0..3, lights) and ref_params (the D16
// params plus the product word), includes nextgi_rt_inc.glsl and nextgi_reference_pt_inc.glsl, defines
// PATH_TRACER_TABLE(s) as the D16 table entry of surface record s and, with PATH_TRACER_HIT_GROUPS, the hit
// pipeline's payload and path_tracer_material (what NEXTGI_PT_MATERIAL() names).
//
// A path is the D16 estimator's (nextgi_pt_radiance_at): emission and next-event estimation at every vertex,
// the D16 BSDF sample, Russian roulette from bounce 4. The product adds the bounce limit, the cull rule, the
// per-pixel jitter, the firefly clamp and (PATH_TRACER_FLAG_PRODUCT) camera rays from the projection,
// decorrelated random numbers and black for camera rays that hit an instance without records. With no flag set
// (debug/path_tracer_reference_match) a pixel is traced exactly as the D16 reference traces it.

#ifndef PATH_TRACER_INC_GLSL
#define PATH_TRACER_INC_GLSL

// PathTracerGPUParams::flags (path_tracer.cpp).
#define PATH_TRACER_FLAG_CULL 1u
#define PATH_TRACER_FLAG_JITTER 2u
#define PATH_TRACER_FLAG_PRODUCT 4u
// PATH_TRACER_MAX_CULL_SKIPS (path_tracer.h).
#define PATH_TRACER_MAX_CULL_SKIPS 8u
// Past a culled hit the ray goes on from its own origin, with its tmin just past the hit: relative to the hit
// distance, so the step outlasts the float spacing of the hit's coordinates far from the origin too.
#define PATH_TRACER_SKIP_RELATIVE 1e-5
#define PATH_TRACER_SKIP_MIN 1e-6
// RSE::CullMode.
#define PATH_TRACER_CULL_FRONT 1u
#define PATH_TRACER_CULL_BACK 2u

struct PathTracerHit {
	vec3 origin; // The ray's origin.
	float t;
	vec3 position;
	uint surface; // The surface record: the D16 table entry.
	vec3 normal; // Geometric, on the side the ray came from.
	bool front; // The rasterizer's front face (path_tracer_hit_is_front() in path_tracer.h).
	float tmin; // The tmin of the trace that found the hit: past any culled hits.
};

// path_tracer_cull_skips_hit() in path_tracer.h.
bool path_tracer_cull_skips(uint p_cull_mode, bool p_front) {
	if (p_cull_mode == PATH_TRACER_CULL_BACK) {
		return !p_front;
	}
	if (p_cull_mode == PATH_TRACER_CULL_FRONT) {
		return p_front;
	}
	return false;
}

uint path_tracer_cull_mode(uint p_surface) {
	return uint(PATH_TRACER_TABLE(p_surface).specular_mode.z + 0.5);
}

// The hit's position, its geometric normal and its side. Godot's front faces wind clockwise as seen from the viewer,
// so (p1 - p0) x (p2 - p0) points into the front side; through the inverse transpose a mirrored instance keeps
// its sides, as the rasterizer reverses its cull mode for mirrored instances. The normal is computed as
// nextgi_pt_hit_surface() computes it (the D16 reference), turned toward the ray. False for a hit without records.
bool path_tracer_hit_surface(NextGIHit p_hit, vec3 p_origin, float p_tmin, vec3 p_dir, out PathTracerHit r_hit) {
	r_hit.origin = p_origin;
	r_hit.tmin = p_tmin;
	r_hit.t = p_hit.t;
	r_hit.position = vec3(0.0);
	r_hit.surface = 0u;
	r_hit.normal = vec3(0.0, 1.0, 0.0);
	r_hit.front = true;
	if (!p_hit.hit || p_hit.custom_index >= ref_params.counts.w) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[p_hit.custom_index];
	if (p_hit.geometry_index >= rec.surface_count) {
		return false;
	}
	uint surface_index = rec.surface_base + p_hit.geometry_index;
	if (surface_index >= ref_params.counts.z) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[surface_index];
	uint index_byte = surface.index_offset + p_hit.primitive * 12u;
	uint i0 = nextgi_pt_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_pt_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_pt_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_pt_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i0 * 12u);
	vec3 p1 = nextgi_pt_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i1 * 12u);
	vec3 p2 = nextgi_pt_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i2 * 12u);
	vec3 object_pos = p0 * (1.0 - p_hit.bary.x - p_hit.bary.y) + p1 * p_hit.bary.x + p2 * p_hit.bary.y;
	vec3 inward = nextgi_pt_mul_normal(rec, normalize(cross(p1 - p0, p2 - p0)));
	r_hit.front = dot(inward, p_dir) > 0.0;
	r_hit.normal = r_hit.front ? -inward : inward;
	r_hit.position = nextgi_pt_mul_point(rec, object_pos);
	r_hit.surface = surface_index;
	return true;
}

// The closest hit the cull rule keeps. A culled hit lets the ray go on past it: the same ray, its tmin just past
// the hit. r_unrecorded is true when the closest hit has no records (a GPU-written instance): the path ends there.
// Without culling this is the D16 reference's single nextgi_trace().
bool path_tracer_trace(vec3 p_origin, vec3 p_dir, float p_tmax, out PathTracerHit r_hit, out bool r_unrecorded) {
	r_unrecorded = false;
	float tmin = 0.0;
	bool cull = (ref_params.product.y & PATH_TRACER_FLAG_CULL) != 0u;
	for (uint i = 0u; i <= PATH_TRACER_MAX_CULL_SKIPS; i++) {
		// GodotNext: nextgi_shadow_policy (#145). GI's instances (mask bit 7), as the D16 reference traces them: none of
		// the shadow-only ones.
		NextGIHit hit = nextgi_trace_opaque(p_origin, tmin, p_dir, p_tmax, NEXTGI_RT_MASK_GI);
		if (!hit.hit) {
			return false;
		}
		if (!path_tracer_hit_surface(hit, p_origin, tmin, p_dir, r_hit)) {
			r_unrecorded = true;
			return false;
		}
		if (!cull || !path_tracer_cull_skips(path_tracer_cull_mode(r_hit.surface), r_hit.front)) {
			return true;
		}
		tmin = hit.t + max(hit.t * PATH_TRACER_SKIP_RELATIVE, PATH_TRACER_SKIP_MIN);
		if (tmin >= p_tmax) {
			return false;
		}
	}
	return false;
}

#ifdef PATH_TRACER_HIT_GROUPS
// The material at the hit, evaluated by its hit group: the same ray again, from the tmin that found the hit (past
// the culled ones), closest-hit only, up to just past the hit. Its cone is one pixel's (#130's texture level of
// detail; PathTracerGPURays). A fallback-group hit (no hit group of its own, no attribute stream, no record) keeps
// the D16 table entry, and so does a hit where the material's fragment() discards (GodotNext: nextgi_hit_discard
// (#304): its group reports it with the fallback flag). The hit group's normal (normal maps included) replaces the
// geometric one, on the side the ray came from.
void path_tracer_prepare_hit(PathTracerHit p_hit, vec3 p_dir, inout vec3 r_normal) {
	path_tracer_material = PATH_TRACER_TABLE(p_hit.surface);
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(ref_params.ray_dx.w), floatBitsToUint(ref_params.ray_dy.w), 0u, 0u);
	// GodotNext: nextgi_shadow_policy (#145). The instances the inline trace saw, with forced opacity: an instance whose
	// geometry is alpha-tested is non-opaque for shadow rays only, so no any-hit stage runs here.
	traceRayEXT(hit_tlas, gl_RayFlagsOpaqueEXT, NEXTGI_RT_MASK_GI, 0, 1, 0, p_hit.origin, p_hit.tmin, p_dir, p_hit.t * 1.0001 + 1e-4, 0);
	uint flags = payload.shading.z >> 24u;
	if ((flags & NEXTGI_HIT_PAYLOAD_HIT) == 0u || (flags & NEXTGI_HIT_PAYLOAD_FALLBACK) != 0u) {
		return;
	}
	vec4 albedo = unpackUnorm4x8(payload.shading.x);
	path_tracer_material.albedo_metal = vec4(albedo.rgb, float((payload.shading.z >> 8u) & 0xFFu) / 255.0);
	path_tracer_material.emission_roughness = vec4(nextgi_hit_unpack_r11g11b10(payload.shading.w), float(payload.shading.z & 0xFFu) / 255.0);
	vec3 shading = nextgi_hit_oct_decode(unpackSnorm2x16(payload.shading.y));
	if (dot(shading, r_normal) < 0.0) {
		shading = -shading;
	}
	r_normal = shading;
}
#else
void path_tracer_prepare_hit(PathTracerHit p_hit, vec3 p_dir, inout vec3 r_normal) {
}
#endif

// One path from the camera. The bounce limit counts the segments after the first hit, so 63 is the D16 hard stop.
// A camera ray that hits an instance without records (GPU-written instances, past the record count) sees it black
// in the product, as a bounce ray ends there; the D16 reference sees the background.
vec3 path_tracer_radiance(vec3 p_origin, vec3 p_dir, inout uint rng) {
	PathTracerHit hit;
	bool unrecorded;
	if (!path_tracer_trace(p_origin, p_dir, 1.0e6, hit, unrecorded)) {
		if (unrecorded && (ref_params.product.y & PATH_TRACER_FLAG_PRODUCT) != 0u) {
			return vec3(0.0);
		}
		return ref_params.sky_color.rgb;
	}
	vec3 normal = hit.normal;
	vec3 position = hit.position;
	uint material = hit.surface;
	path_tracer_prepare_hit(hit, p_dir, normal);
	vec3 view = -p_dir;
	vec3 accumulated = NEXTGI_PT_MATERIAL(material).emission_roughness.rgb;
	accumulated += nextgi_pt_next_event(position, normal, view, material);
	vec3 throughput = vec3(1.0);
	uint max_bounces = ref_params.product.x;
	for (uint bounce = 1u; bounce <= max_bounces; bounce++) {
		if (bounce >= NEXTGI_PT_ROULETTE_FROM) {
			float survival = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
			if (survival <= 0.0 || nextgi_pt_rnd(rng) > survival) {
				break;
			}
			throughput /= survival;
		}
		vec3 direction;
		float pdf = 0.0;
		vec3 weight = nextgi_pt_sample(material, normal, view, rng, direction, pdf);
		if (pdf <= 0.0 || weight.r != weight.r) {
			break;
		}
		throughput *= weight;
		vec3 origin = position + normal * NEXTGI_PT_ORIGIN_OFFSET;
		if (!path_tracer_trace(origin, direction, 1.0e6, hit, unrecorded)) {
			if (!unrecorded) {
				accumulated += throughput * ref_params.sky_color.rgb;
			}
			break;
		}
		normal = hit.normal;
		position = hit.position;
		material = hit.surface;
		path_tracer_prepare_hit(hit, direction, normal);
		view = -direction;
		accumulated += throughput * NEXTGI_PT_MATERIAL(material).emission_roughness.rgb;
		accumulated += throughput * nextgi_pt_next_event(position, normal, view, material);
	}
	return accumulated;
}

// UE's Max Path Intensity: a sample's exposed radiance is scaled down to the clamp (0 is off).
vec3 path_tracer_clamp(vec3 p_radiance) {
	float limit = uintBitsToFloat(ref_params.product.z);
	if (limit <= 0.0) {
		return p_radiance;
	}
	limit /= max(ref_params.exposure_clear.x, 1e-20);
	float peak = max(p_radiance.r, max(p_radiance.g, p_radiance.b));
	return peak > limit ? p_radiance * (limit / peak) : p_radiance;
}

// One sample of a pixel. The product's random numbers come from a hash of the pixel's own hash, the sample and the
// seed, so no two pixels share a stream within their samples; the D16 reference's (seed ^ pixel ^ sample, kept for
// the reference match) give every aligned run of 2^k pixels the same streams over their first 2^k samples, which
// shows as horizontal streaks in a progressive image. The sub-pixel position comes from its own hash.
vec3 path_tracer_sample_pixel(uvec2 p_pixel) {
	uint index = p_pixel.y * ref_params.dispatch.x + p_pixel.x;
	uint sample_index = ref_params.dispatch.z;
	bool product = (ref_params.product.y & PATH_TRACER_FLAG_PRODUCT) != 0u;
	uint rng;
	if (product) {
		rng = nextgi_pt_hash(nextgi_pt_hash(index * 0x27D4EB2Du + 0x165667B1u) ^ (sample_index * 0xC2B2AE35u) ^ (ref_params.counts.x * 0x61C88647u));
	} else {
		rng = nextgi_pt_hash(index ^ ref_params.counts.x ^ sample_index);
	}
	vec2 jitter = vec2(0.5);
	if ((ref_params.product.y & PATH_TRACER_FLAG_JITTER) != 0u) {
		uint bits = nextgi_pt_hash(nextgi_pt_hash(index * 0x9E3779B9u + 0x68E31DA4u) ^ (sample_index * 0x85EBCA6Bu) ^ ref_params.counts.x);
		jitter = vec2(float(bits & 0xFFFFu), float(bits >> 16u)) * (1.0 / 65536.0);
	}
	vec2 uv = (vec2(p_pixel) + jitter) / vec2(ref_params.dispatch.xy);
	vec2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;
	if (product) {
		// The ray through the near plane at this pixel (PathTracerGPURays): from the camera for a perspective
		// projection, along the view direction from the near plane for an orthogonal one.
		vec3 near_point = ref_params.ray_center.xyz + ref_params.ray_dx.xyz * ndc.x + ref_params.ray_dy.xyz * ndc.y;
		vec3 origin = ref_params.cam_origin_fov.xyz;
		vec3 direction = normalize(near_point - origin);
		if (ref_params.ray_center.w > 0.5) {
			origin = near_point;
			direction = normalize(ref_params.cam_forward_aspect.xyz);
		}
		return path_tracer_clamp(path_tracer_radiance(origin, direction, rng));
	}
	float tan_half = tan(ref_params.cam_origin_fov.w * 0.5);
	float aspect = ref_params.cam_forward_aspect.w;
	vec3 direction = normalize(ref_params.cam_right.xyz * (ndc.x * tan_half * aspect) + ref_params.cam_up.xyz * (ndc.y * tan_half) + ref_params.cam_forward_aspect.xyz);
	return path_tracer_clamp(path_tracer_radiance(ref_params.cam_origin_fov.xyz, direction, rng));
}

#endif // PATH_TRACER_INC_GLSL
