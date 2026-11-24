// GodotNext: path_tracer (#167), path_tracer_denoise (#168), path_tracer_lights_camera (#169).
// The product path tracer's paths (RendererRD::PathTracer). Not a standalone shader.
// The includer declares the D16 buffers (records, surfaces, chunk0..3, lights), ref_params (PathTracerGPUParams),
// and the #169 bindings (pt_sky_cdf, pt_fog_volumes, pt_ies_atlas, pt_area_atlas, pt_projector_atlas,
// pt_sky_octmap, pt_linear_sampler), includes nextgi_rt_inc.glsl and nextgi_reference_pt_inc.glsl, defines
// PATH_TRACER_TABLE(s) as the D16 table entry of surface record s and, with PATH_TRACER_HIT_GROUPS, the hit
// pipeline's payload and path_tracer_material (what NEXTGI_PT_MATERIAL() names).
//
// With no product flag set (debug/path_tracer_reference_match) a pixel is traced exactly as the D16 reference
// traces it. With PATH_TRACER_FLAG_PRODUCT set:
// - All Godot light types (directional + angular diameter, omni + radius + projector, spot + cone + projector,
//   area/rect + barn doors + texture, and #140 IES profiles) with NEE + balance-heuristic MIS against BSDF sampling.
// - Sky radiance octmap importance sampling via 2D marginal/conditional CDF + MIS against BSDF sampling.
// - Homogeneous volumetric fog and FogVolume media via unbiased Woodcock delta tracking + ratio tracking + HG phase,
//   plus standard Environment.fog fallback when volumetric fog is disabled.
// - Thin-lens depth of field from CameraAttributesPractical/Physical.
// - Per-instance motion blur via 8-slice shutter-interpolated TLAS masks.

#ifndef PATH_TRACER_INC_GLSL
#define PATH_TRACER_INC_GLSL

#include "../ies_inc.glsl"
#include "../oct_inc.glsl"

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

// GodotNext: path_tracer_lights_camera (#169). Light types, flags, sky CDF layout, and delta tracking caps.
#define PT_LIGHT_DIRECTIONAL 0u
#define PT_LIGHT_OMNI 1u
#define PT_LIGHT_SPOT 2u
#define PT_LIGHT_AREA 3u

#define PT_LIGHT_FLAG_CAST_SHADOW 1u
#define PT_LIGHT_FLAG_HAS_PROJECTOR 2u
#define PT_LIGHT_FLAG_HAS_AREA_TEX 4u
#define PT_LIGHT_FLAG_NEGATIVE 8u
#define PT_LIGHT_FLAG_IES 16u

#define PT_SKY_CDF_DIM 128u
#define PT_SKY_CDF_COND_OFFSET 128u
#define PT_SKY_CDF_PDF_OFFSET (128u + 128u * 128u)

#define PT_MAX_DELTA_STEPS 64u
#define PT_MAX_RATIO_STEPS 32u

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

bool path_tracer_vec3_is_finite(vec3 p_v) {
	return all(greaterThanEqual(p_v, vec3(-1e30))) && all(lessThanEqual(p_v, vec3(1e30)));
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
bool path_tracer_trace(vec3 p_origin, vec3 p_dir, float p_tmax, uint p_ray_mask, out PathTracerHit r_hit, out bool r_unrecorded) {
	r_unrecorded = false;
	float tmin = 0.0;
	bool cull = (ref_params.product.y & PATH_TRACER_FLAG_CULL) != 0u;
	for (uint i = 0u; i <= PATH_TRACER_MAX_CULL_SKIPS; i++) {
		NextGIHit hit = nextgi_trace_opaque(p_origin, tmin, p_dir, p_tmax, p_ray_mask);
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

bool pt_shadow_occluded(vec3 p_origin, vec3 p_dir, float p_tmax, uint p_ray_mask) {
	if (p_tmax <= 1e-5) {
		return false;
	}
	NextGIHit hit = nextgi_trace_opaque(p_origin, 0.0, p_dir, p_tmax, p_ray_mask);
	return hit.hit;
}

#ifdef PATH_TRACER_HIT_GROUPS
void path_tracer_prepare_hit(PathTracerHit p_hit, vec3 p_dir, uint p_ray_mask, inout vec3 r_normal) {
	path_tracer_material = PATH_TRACER_TABLE(p_hit.surface);
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(ref_params.ray_dx.w), floatBitsToUint(ref_params.ray_dy.w), 0u, 0u);
	traceRayEXT(hit_tlas, gl_RayFlagsOpaqueEXT, p_ray_mask, 0, 1, 0, p_hit.origin, p_hit.tmin, p_dir, p_hit.t * 1.0001 + 1e-4, 0);
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
void path_tracer_prepare_hit(PathTracerHit p_hit, vec3 p_dir, uint p_ray_mask, inout vec3 r_normal) {
}
#endif

// === MIS, BSDF, AND SAMPLING HELPERS (#169) ===

float pt_mis_weight(float p_pdf_a, float p_pdf_b) {
	if (p_pdf_a <= 0.0) {
		return 0.0;
	}
	if (p_pdf_b <= 0.0) {
		return 1.0;
	}
	return p_pdf_a / (p_pdf_a + p_pdf_b);
}

float pt_bsdf_spec_prob(uint p_material) {
	vec3 reflected = nextgi_pt_diffuse_albedo(p_material);
	vec3 f0 = nextgi_pt_f0(p_material);
	float roughness = NEXTGI_PT_MATERIAL(p_material).emission_roughness.w;
	float spec_weight = max(f0.r, max(f0.g, f0.b));
	float diff_weight = max(reflected.r, max(reflected.g, reflected.b));
	if (roughness > 0.0 && spec_weight + diff_weight > 0.0) {
		return spec_weight / (spec_weight + diff_weight);
	}
	return 0.0;
}

float pt_bsdf_pdf(uint p_material, vec3 p_normal, vec3 p_view, vec3 p_light_dir) {
	float n_dot_l = dot(p_normal, p_light_dir);
	if (n_dot_l <= 0.0) {
		return 0.0;
	}
	float p_spec = pt_bsdf_spec_prob(p_material);
	float pdf_diff = n_dot_l * (1.0 / NEXTGI_PT_PI);
	float pdf_spec = p_spec > 0.0 ? nextgi_pt_ggx_pdf(p_material, p_normal, p_view, p_light_dir) : 0.0;
	return pdf_spec * p_spec + pdf_diff * (1.0 - p_spec);
}

vec3 pt_bsdf_eval_nl(uint p_material, vec3 p_normal, vec3 p_view, vec3 p_light_dir, float p_n_dot_l, float p_spec_amount) {
	vec3 diff = nextgi_pt_diffuse_albedo(p_material) * nextgi_pt_diffuse_nl(p_material, p_normal, p_view, p_light_dir, p_n_dot_l);
	vec3 spec = nextgi_pt_specular_nl(p_material, p_normal, p_view, p_light_dir, p_n_dot_l) * p_spec_amount;
	return diff + spec;
}

// Mixture-PDF BSDF sampling so BSDF bounce PDF matches pt_bsdf_pdf() identically for MIS.
vec3 pt_bsdf_sample_mis(uint p_material, vec3 p_normal, vec3 p_view, inout uint r_rng, out vec3 r_direction, out float r_pdf) {
	r_direction = p_normal;
	r_pdf = 0.0;
	float p_spec = pt_bsdf_spec_prob(p_material);
	vec2 random_uv = vec2(nextgi_pt_rnd(r_rng), nextgi_pt_rnd(r_rng));
	float lobe = nextgi_pt_rnd(r_rng);
	if (lobe < p_spec) {
		float roughness = NEXTGI_PT_MATERIAL(p_material).emission_roughness.w;
		float alpha = roughness * roughness;
		r_direction = nextgi_pt_ggx(p_normal, p_view, alpha, random_uv);
	} else {
		r_direction = nextgi_pt_cosine(p_normal, random_uv);
	}
	float n_dot_l = dot(p_normal, r_direction);
	if (n_dot_l <= 0.0) {
		return vec3(0.0);
	}
	r_pdf = pt_bsdf_pdf(p_material, p_normal, p_view, r_direction);
	if (r_pdf <= 1e-8) {
		return vec3(0.0);
	}
	return pt_bsdf_eval_nl(p_material, p_normal, p_view, r_direction, n_dot_l, 1.0) / r_pdf;
}

vec3 pt_sample_cone(vec3 p_axis, float p_cos_max, vec2 p_u) {
	float cos_theta = mix(1.0, p_cos_max, p_u.x);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * NEXTGI_PT_PI * p_u.y;
	vec3 t;
	vec3 b;
	nextgi_pt_basis(p_axis, t, b);
	return normalize(t * (sin_theta * cos(phi)) + b * (sin_theta * sin(phi)) + p_axis * cos_theta);
}

// Shirley-Chiu concentric disk sampling for thin-lens DOF.
vec2 pt_sample_concentric_disk(vec2 p_u) {
	vec2 offset = 2.0 * p_u - vec2(1.0);
	if (offset.x == 0.0 && offset.y == 0.0) {
		return vec2(0.0);
	}
	float r;
	float theta;
	if (abs(offset.x) > abs(offset.y)) {
		r = offset.x;
		theta = (NEXTGI_PT_PI * 0.25) * (offset.y / offset.x);
	} else {
		r = offset.y;
		theta = (NEXTGI_PT_PI * 0.5) - (NEXTGI_PT_PI * 0.25) * (offset.x / offset.y);
	}
	return r * vec2(cos(theta), sin(theta));
}

// === SKY OCTMAP & 2D CDF IMPORTANCE SAMPLING (#169) ===

vec3 pt_world_to_sky_dir(vec3 p_world_dir) {
	return normalize(vec3(
			dot(ref_params.sky_orient_row0.xyz, p_world_dir),
			dot(ref_params.sky_orient_row1.xyz, p_world_dir),
			dot(ref_params.sky_orient_row2.xyz, p_world_dir)));
}

vec3 pt_sky_to_world_dir(vec3 p_sky_dir) {
	return normalize(
			ref_params.sky_orient_row0.xyz * p_sky_dir.x +
			ref_params.sky_orient_row1.xyz * p_sky_dir.y +
			ref_params.sky_orient_row2.xyz * p_sky_dir.z);
}

float pt_sky_oct_jacobian(vec3 p_dir) {
	float l1 = abs(p_dir.x) + abs(p_dir.y) + abs(p_dir.z);
	return 4.0 * l1 * l1 * l1;
}

vec3 pt_eval_sky(vec3 p_world_dir) {
	if (ref_params.sky_orient_row0.w < 0.5) {
		return ref_params.sky_color.rgb;
	}
	vec3 sky_dir = pt_world_to_sky_dir(p_world_dir);
	vec2 oct_uv = vec3_to_oct_with_border(sky_dir, vec2(ref_params.sky_params.y, ref_params.sky_params.z));
	vec3 rgb = textureLod(sampler2D(pt_sky_octmap, pt_linear_sampler), oct_uv, 0.0).rgb * ref_params.sky_params.x;
	if (!path_tracer_vec3_is_finite(rgb)) {
		return vec3(0.0);
	}
	return max(rgb, vec3(0.0));
}

float pt_sky_pdf(vec3 p_world_dir) {
	if (ref_params.sky_orient_row0.w < 0.5 || ref_params.motion_params.z == 0u) {
		return 0.0;
	}
	vec3 sky_dir = pt_world_to_sky_dir(p_world_dir);
	vec2 uv = clamp(vec3_to_oct(sky_dir), vec2(0.0), vec2(0.999999));
	uint ix = min(uint(uv.x * float(PT_SKY_CDF_DIM)), PT_SKY_CDF_DIM - 1u);
	uint iy = min(uint(uv.y * float(PT_SKY_CDF_DIM)), PT_SKY_CDF_DIM - 1u);
	float p_uv = pt_sky_cdf.data[PT_SKY_CDF_PDF_OFFSET + iy * PT_SKY_CDF_DIM + ix];
	return p_uv / max(pt_sky_oct_jacobian(sky_dir), 1e-6);
}

void pt_sample_sky(vec2 p_u, out vec3 r_world_dir, out float r_pdf_omega) {
	uint lo = 0u;
	uint hi = PT_SKY_CDF_DIM - 1u;
	for (uint step = 0u; step < 7u; step++) {
		uint mid = (lo + hi) >> 1u;
		if (p_u.y > pt_sky_cdf.data[mid]) {
			lo = mid + 1u;
		} else {
			hi = mid;
		}
	}
	uint iy = min(lo, PT_SKY_CDF_DIM - 1u);
	float prev_y = iy > 0u ? pt_sky_cdf.data[iy - 1u] : 0.0;
	float curr_y = pt_sky_cdf.data[iy];
	float dy = clamp((p_u.y - prev_y) / max(curr_y - prev_y, 1e-8), 0.0, 1.0);

	uint cond_base = PT_SKY_CDF_COND_OFFSET + iy * PT_SKY_CDF_DIM;
	lo = 0u;
	hi = PT_SKY_CDF_DIM - 1u;
	for (uint step = 0u; step < 7u; step++) {
		uint mid = (lo + hi) >> 1u;
		if (p_u.x > pt_sky_cdf.data[cond_base + mid]) {
			lo = mid + 1u;
		} else {
			hi = mid;
		}
	}
	uint ix = min(lo, PT_SKY_CDF_DIM - 1u);
	float prev_x = ix > 0u ? pt_sky_cdf.data[cond_base + ix - 1u] : 0.0;
	float curr_x = pt_sky_cdf.data[cond_base + ix];
	float dx = clamp((p_u.x - prev_x) / max(curr_x - prev_x, 1e-8), 0.0, 1.0);

	vec2 uv = (vec2(float(ix) + dx, float(iy) + dy)) / float(PT_SKY_CDF_DIM);
	vec3 sky_dir = oct_to_vec3(uv * 2.0 - 1.0);
	r_world_dir = pt_sky_to_world_dir(sky_dir);
	float p_uv = pt_sky_cdf.data[PT_SKY_CDF_PDF_OFFSET + iy * PT_SKY_CDF_DIM + ix];
	r_pdf_omega = p_uv / max(pt_sky_oct_jacobian(sky_dir), 1e-6);
}

// === VOLUMETRIC FOG & FOG VOLUME DELTA/RATIO TRACKING (#169) ===

float pt_hg_phase(float p_cos_theta, float p_g) {
	float g = clamp(p_g, -0.95, 0.95);
	float g2 = g * g;
	float denom = max(1.0 + g2 - 2.0 * g * p_cos_theta, 1e-4);
	return (1.0 - g2) / (4.0 * NEXTGI_PT_PI * denom * sqrt(denom));
}

vec3 pt_sample_hg(vec3 p_dir, float p_g, vec2 p_u, out float r_pdf) {
	float g = clamp(p_g, -0.95, 0.95);
	float cos_theta;
	if (abs(g) < 1e-3) {
		cos_theta = 1.0 - 2.0 * p_u.x;
	} else {
		float sqr = (1.0 - g * g) / (1.0 - g + 2.0 * g * p_u.x);
		cos_theta = clamp((1.0 + g * g - sqr * sqr) / (2.0 * g), -1.0, 1.0);
	}
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * NEXTGI_PT_PI * p_u.y;
	vec3 t;
	vec3 b;
	nextgi_pt_basis(p_dir, t, b);
	r_pdf = pt_hg_phase(cos_theta, g);
	return normalize(t * (sin_theta * cos(phi)) + b * (sin_theta * sin(phi)) + p_dir * cos_theta);
}

// Analytic Godot FogVolume SDF matching volumetric_fog.glsl L180-212.
float pt_fog_volume_sdf(vec3 p_local, vec3 p_half_size, uint p_shape) {
	if (p_shape == 0u) {
		// Ellipsoid.
		vec3 hs = max(p_half_size, vec3(1e-4));
		float k0 = length(p_local / hs);
		float k1 = length(p_local / (hs * hs));
		return k1 > 1e-6 ? k0 * (k0 - 1.0) / k1 : -min(hs.x, min(hs.y, hs.z));
	} else if (p_shape == 1u) {
		// Cone.
		float inv_h = 1.0 / max(0.001, p_half_size.y);
		float r = 1.0 / max(0.001, min(p_half_size.x, p_half_size.z) * 0.5);
		float hyp = sqrt(r * r + inv_h * inv_h);
		vec2 c = vec2(r / hyp, inv_h / hyp);
		float q = length(p_local.xz);
		return max(dot(c, vec2(q, p_local.y - p_half_size.y)), -p_half_size.y - p_local.y);
	} else if (p_shape == 2u) {
		// Cylinder.
		vec2 d = abs(vec2(length(p_local.xz), p_local.y)) - vec2(min(p_half_size.x, p_half_size.z), p_half_size.y);
		return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
	} else if (p_shape == 3u) {
		// Box.
		vec3 q = abs(p_local) - p_half_size;
		return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
	}
	// Shape 4: World (infinite).
	return -1.0;
}

bool pt_ray_aabb_intersects(vec3 p_origin, vec3 p_dir, vec3 p_half_size, float p_tmax, out float r_t0, out float r_t1) {
	vec3 inv_d = 1.0 / mix(p_dir, vec3(1e-8), lessThan(abs(p_dir), vec3(1e-8)));
	vec3 tmin_v = (-p_half_size - p_origin) * inv_d;
	vec3 tmax_v = (p_half_size - p_origin) * inv_d;
	vec3 t1 = min(tmin_v, tmax_v);
	vec3 t2 = max(tmin_v, tmax_v);
	r_t0 = max(max(t1.x, t1.y), max(t1.z, 0.0));
	r_t1 = min(min(t2.x, t2.y), min(t2.z, p_tmax));
	return r_t1 > r_t0;
}

vec3 pt_fog_vol_to_local_point(uint p_base, vec3 p_world) {
	vec4 r0 = pt_fog_volumes.data[p_base + 0u];
	vec4 r1 = pt_fog_volumes.data[p_base + 1u];
	vec4 r2 = pt_fog_volumes.data[p_base + 2u];
	return vec3(dot(r0.xyz, p_world) + r0.w, dot(r1.xyz, p_world) + r1.w, dot(r2.xyz, p_world) + r2.w);
}

vec3 pt_fog_vol_to_local_dir(uint p_base, vec3 p_dir) {
	vec4 r0 = pt_fog_volumes.data[p_base + 0u];
	vec4 r1 = pt_fog_volumes.data[p_base + 1u];
	vec4 r2 = pt_fog_volumes.data[p_base + 2u];
	return vec3(dot(r0.xyz, p_dir), dot(r1.xyz, p_dir), dot(r2.xyz, p_dir));
}

// Evaluates volumetric fog extinction, scattering albedo, and emission at world point p_world.
void pt_eval_medium(vec3 p_world, out float r_sigma_t, out vec3 r_albedo, out vec3 r_emission) {
	float world_dist = length(p_world - ref_params.cam_origin_fov.xyz);
	float sigma_t = 0.0;
	vec3 scatter_sum = vec3(0.0);
	vec3 emission_sum = vec3(0.0);
	if (world_dist <= ref_params.vfog_params0.z) {
		float wd = ref_params.vfog_params0.x;
		sigma_t += wd;
		scatter_sum += wd * ref_params.vfog_scattering.rgb;
		emission_sum += ref_params.vfog_emission.rgb;
	}
	uint vol_count = ref_params.product.w;
	for (uint v = 0u; v < vol_count; v++) {
		uint base = v * 8u;
		vec4 half_shape = pt_fog_volumes.data[base + 3u];
		uint shape = uint(half_shape.w + 0.5);
		float sdf = -1.0;
		if (shape <= 3u) {
			vec3 lp = pt_fog_vol_to_local_point(base, p_world);
			sdf = pt_fog_volume_sdf(lp, half_shape.xyz, shape);
			if (sdf >= 0.0) {
				continue;
			}
		}
		vec4 albedo_den = pt_fog_volumes.data[base + 4u];
		vec4 emit_falloff = pt_fog_volumes.data[base + 5u];
		vec4 origin_edge = pt_fog_volumes.data[base + 6u];
		vec4 bounds_scale = pt_fog_volumes.data[base + 7u];

		float d = albedo_den.w;
		if (emit_falloff.w > 0.0) {
			d *= clamp(exp2(-emit_falloff.w * (p_world.y - origin_edge.y)), 0.0, 1.0);
		}
		if (shape <= 3u && origin_edge.w > 1e-5) {
			d *= pow(clamp(-sdf * bounds_scale.y, 0.0, 1.0), origin_edge.w);
		}
		sigma_t += d;
		if (d > 0.0) {
			scatter_sum += d * albedo_den.rgb;
			emission_sum += d * emit_falloff.rgb;
		}
	}
	r_sigma_t = max(sigma_t, 0.0);
	r_albedo = r_sigma_t > 1e-6 ? clamp(scatter_sum / r_sigma_t, vec3(0.0), vec3(1.0)) : vec3(0.0);
	r_emission = max(emission_sum, vec3(0.0));
}

// Computes strict upper-bound majorant extinction and active interval [r_t0, r_t1] along ray [0, p_tmax].
float pt_medium_majorant(vec3 p_origin, vec3 p_dir, float p_tmax, out float r_t0, out float r_t1) {
	float vfog_len = ref_params.vfog_params0.z;
	float maj = 0.0;
	r_t0 = p_tmax;
	r_t1 = 0.0;
	// World homogeneous fog sphere centered at camera with radius vfog_len.
	if (ref_params.vfog_params0.x > 1e-6 || dot(ref_params.vfog_emission.rgb, vec3(1.0)) > 1e-6) {
		vec3 oc = p_origin - ref_params.cam_origin_fov.xyz;
		float b = dot(oc, p_dir);
		float c = dot(oc, oc) - vfog_len * vfog_len;
		float disc = b * b - c;
		if (disc > 0.0) {
			float sd = sqrt(disc);
			float s0 = max(-b - sd, 0.0);
			float s1 = min(-b + sd, p_tmax);
			if (s1 > s0) {
				maj += max(ref_params.vfog_params0.x, 1e-4);
				r_t0 = min(r_t0, s0);
				r_t1 = max(r_t1, s1);
			}
		}
	}
	uint vol_count = ref_params.product.w;
	for (uint v = 0u; v < vol_count; v++) {
		uint base = v * 8u;
		vec4 bounds_scale = pt_fog_volumes.data[base + 7u];
		if (bounds_scale.x <= 1e-6) {
			continue;
		}
		vec4 half_shape = pt_fog_volumes.data[base + 3u];
		uint shape = uint(half_shape.w + 0.5);
		if (shape > 3u) {
			// World FogVolume spans the volumetric fog range.
			maj += bounds_scale.x;
			r_t0 = 0.0;
			r_t1 = max(r_t1, min(p_tmax, vfog_len));
		} else {
			vec3 lo = pt_fog_vol_to_local_point(base, p_origin);
			vec3 ld = pt_fog_vol_to_local_dir(base, p_dir);
			float vt0;
			float vt1;
			if (pt_ray_aabb_intersects(lo, ld, half_shape.xyz, p_tmax, vt0, vt1)) {
				maj += bounds_scale.x;
				r_t0 = min(r_t0, vt0);
				r_t1 = max(r_t1, vt1);
			}
		}
	}
	return maj;
}

// Unbiased volumetric transmittance along a shadow ray (analytic for homogeneous fog, ratio tracking with FogVolumes).
float pt_shadow_transmittance(vec3 p_origin, vec3 p_dir, float p_tmax, bool p_is_sky, inout uint r_rng) {
	uint fog_mode = uint(ref_params.vfog_emission.w + 0.5);
	if (fog_mode != 1u) {
		return 1.0;
	}
	float sky_affect = p_is_sky ? ref_params.vfog_params0.w : 1.0;
	if (sky_affect <= 1e-6) {
		return 1.0;
	}
	float t0;
	float t1;
	float maj = pt_medium_majorant(p_origin, p_dir, min(p_tmax, ref_params.vfog_params0.z * 2.0), t0, t1);
	if (maj <= 1e-6 || t1 <= t0) {
		return 1.0;
	}
	if (ref_params.product.w == 0u) {
		float tr = exp(-ref_params.vfog_params0.x * (t1 - t0));
		return mix(1.0, tr, sky_affect);
	}
	float tr = 1.0;
	float t = t0;
	for (uint s = 0u; s < PT_MAX_RATIO_STEPS; s++) {
		float dt = -log(max(1.0 - nextgi_pt_rnd(r_rng), 1e-7)) / maj;
		t += dt;
		if (t >= t1) {
			break;
		}
		float sigma_t;
		vec3 albedo;
		vec3 emit;
		pt_eval_medium(p_origin + p_dir * t, sigma_t, albedo, emit);
		tr *= max(0.0, 1.0 - sigma_t / maj);
		if (tr < 1e-4) {
			tr = 0.0;
			break;
		}
	}
	return mix(1.0, tr, sky_affect);
}

// Legacy Environment.fog fallback (when fog_enabled && !volumetric_fog_enabled).
vec4 pt_eval_legacy_fog(vec3 p_origin, vec3 p_dir, float p_dist, bool p_is_sky) {
	if (uint(ref_params.vfog_emission.w + 0.5) != 2u) {
		return vec4(0.0);
	}
	float fog_density = ref_params.fog_legacy0.w;
	float fog_amount = 0.0;
	if (ref_params.fog_legacy2.z > 0.5) {
		// Depth fog mode.
		float depth_begin = ref_params.fog_legacy1.z;
		float depth_end = max(ref_params.fog_legacy1.w, depth_begin + 1e-4);
		float norm_d = clamp((p_dist - depth_begin) / (depth_end - depth_begin), 0.0, 1.0);
		fog_amount = pow(norm_d, max(ref_params.fog_legacy2.x, 0.01)) * clamp(fog_density, 0.0, 1.0);
	} else {
		// Exponential + optional height fog.
		float dist = p_is_sky ? 1.0e4 : p_dist;
		fog_amount = 1.0 - exp(-min(dist * fog_density, 32.0));
		float h_density = ref_params.fog_legacy1.y;
		if (abs(h_density) > 1e-5 && !p_is_sky) {
			float y = p_origin.y + p_dir.y * p_dist;
			float h_factor = clamp(exp(-(y - ref_params.fog_legacy1.x) * abs(h_density)), 0.0, 1.0);
			fog_amount *= h_factor;
		}
	}
	if (p_is_sky) {
		fog_amount *= ref_params.fog_legacy2.y;
	}
	return vec4(ref_params.fog_legacy0.rgb, clamp(fog_amount, 0.0, 1.0));
}

// === ALL GODOT LIGHT TYPES: NEE + MIS + BSDF EMISSIVE HITS (#169) ===

vec3 pt_sample_projector_omni(vec4 p_rect, vec3 p_dir_ls) {
	vec3 lv = normalize(p_dir_ls);
	vec4 atlas_rect = p_rect;
	if (lv.z >= 0.0) {
		atlas_rect.y += atlas_rect.w;
	}
	lv.z = 1.0 + abs(lv.z);
	vec2 uv = (lv.xy / lv.z) * 0.5 + 0.5;
	vec2 tex_size = vec2(textureSize(sampler2D(pt_projector_atlas, pt_linear_sampler), 0));
	vec2 half_texel = 0.5 / max(tex_size, vec2(1.0));
	vec2 proj_uv = clamp(uv * atlas_rect.zw, half_texel, max(atlas_rect.zw - half_texel, half_texel));
	return textureLod(sampler2D(pt_projector_atlas, pt_linear_sampler), atlas_rect.xy + proj_uv, 0.0).rgb;
}

bool pt_sample_projector_spot(vec4 p_rect, vec3 p_to_point, vec3 p_right, vec3 p_up, vec3 p_spot_dir, float p_tan_half, out vec3 r_color) {
	float lz = dot(p_to_point, p_spot_dir);
	if (lz <= 1e-6) {
		r_color = vec3(0.0);
		return false;
	}
	float lx = dot(p_to_point, p_right);
	float ly = dot(p_to_point, p_up);
	float denom = lz * max(p_tan_half, 1e-4);
	vec2 ndc = vec2(lx, ly) / denom;
	if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) {
		r_color = vec3(0.0);
		return false;
	}
	vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
	vec2 tex_size = vec2(textureSize(sampler2D(pt_projector_atlas, pt_linear_sampler), 0));
	vec2 half_texel = 0.5 / max(tex_size, vec2(1.0));
	vec2 proj_uv = clamp(uv * p_rect.zw, half_texel, max(p_rect.zw - half_texel, half_texel));
	r_color = textureLod(sampler2D(pt_projector_atlas, pt_linear_sampler), p_rect.xy + proj_uv, 0.0).rgb;
	return true;
}

vec3 pt_sample_area_texture(vec4 p_rect, vec2 p_pos_xy, vec2 p_half_size) {
	vec2 uv_rect = clamp((p_pos_xy + p_half_size) / max(2.0 * p_half_size, vec2(1e-6)), vec2(0.0), vec2(1.0));
	vec2 uv_flip = vec2(1.0) - uv_rect;
	vec2 tex_size = vec2(textureSize(sampler2D(pt_area_atlas, pt_linear_sampler), 0));
	vec2 half_texel = 0.5 / max(tex_size, vec2(1.0));
	vec2 local_uv = clamp(uv_flip * p_rect.zw, half_texel, max(p_rect.zw - half_texel, half_texel));
	vec4 tex = textureLod(sampler2D(pt_area_atlas, pt_linear_sampler), p_rect.xy + local_uv, 0.0);
	return tex.rgb * tex.a;
}

// Evaluates NEE over all scenario lights + sky CDF at either a surface vertex (p_is_volume == false)
// or a volumetric scatter event (p_is_volume == true, using Henyey-Greenstein phase).
vec3 pt_next_event_product(vec3 p_origin, vec3 p_normal, vec3 p_view, uint p_material, bool p_is_volume, float p_phase_g, uint p_ray_mask, inout uint r_rng) {
	vec3 lit = vec3(0.0);
	vec3 shadow_origin = p_is_volume ? p_origin : (p_origin + p_normal * NEXTGI_PT_ORIGIN_OFFSET);
	uint light_count = ref_params.counts.y;

	for (uint i = 0u; i < light_count; i++) {
		uint base = i * 9u;
		vec4 v0 = lights.data[base + 0u];
		vec4 v1 = lights.data[base + 1u];
		vec4 v2 = lights.data[base + 2u];
		vec4 v3 = lights.data[base + 3u];
		vec4 v4 = lights.data[base + 4u];
		vec4 v5 = lights.data[base + 5u];
		vec4 v6 = lights.data[base + 6u];
		vec4 v7 = lights.data[base + 7u];
		vec4 v8 = lights.data[base + 8u];
		uvec3 u7 = floatBitsToUint(v7.xyz);
		uint ltype = u7.x;
		uint lflags = u7.y;
		uint ies_layer = u7.z;
		float inv_range = v7.w;
		float sign_mul = (lflags & PT_LIGHT_FLAG_NEGATIVE) != 0u ? -1.0 : 1.0;
		bool cast_shadow = (lflags & PT_LIGHT_FLAG_CAST_SHADOW) != 0u;
		float shadow_opacity = v8.x;

		vec3 light_dir = vec3(0.0);
		float shadow_dist = 0.0;
		vec3 radiance_weight = vec3(0.0); // L_e / pdf_light (or irradiance for delta lights)
		float pdf_light = 0.0;
		bool is_delta = true;
		bool is_sky_shadow = false;

		if (ltype == PT_LIGHT_DIRECTIONAL) {
			float ang_rad = v3.w;
			float cos_max = v0.w;
			if (ang_rad > 1e-4) {
				vec2 u = vec2(nextgi_pt_rnd(r_rng), nextgi_pt_rnd(r_rng));
				light_dir = pt_sample_cone(v0.xyz, cos_max, u);
				float omega = max(2.0 * NEXTGI_PT_PI * (1.0 - cos_max), 1e-8);
				pdf_light = 1.0 / omega;
				is_delta = false;
			} else {
				light_dir = v0.xyz;
				is_delta = true;
			}
			shadow_dist = 1.0e6;
			radiance_weight = v1.rgb * sign_mul;
			is_sky_shadow = true;
		} else if (ltype == PT_LIGHT_OMNI || ltype == PT_LIGHT_SPOT) {
			vec3 to_center = v0.xyz - shadow_origin;
			float dist_center = length(to_center);
			float radius = v3.w;
			if (dist_center <= 1e-6 || dist_center > v0.w + radius) {
				continue;
			}
			vec3 center_dir = to_center / dist_center;
			if (radius > 1e-4 && dist_center > radius * 1.001) {
				float sin2_max = clamp((radius * radius) / (dist_center * dist_center), 0.0, 0.999999);
				float cos_max = sqrt(1.0 - sin2_max);
				vec2 u = vec2(nextgi_pt_rnd(r_rng), nextgi_pt_rnd(r_rng));
				light_dir = pt_sample_cone(center_dir, cos_max, u);
				float cos_t = dot(light_dir, center_dir);
				float disc = max(0.0, radius * radius - dist_center * dist_center * max(0.0, 1.0 - cos_t * cos_t));
				shadow_dist = max(dist_center * cos_t - sqrt(disc), 1e-4);
				float omega = max(2.0 * NEXTGI_PT_PI * (1.0 - cos_max), 1e-8);
				pdf_light = 1.0 / omega;
				is_delta = false;
			} else {
				light_dir = center_dir;
				shadow_dist = dist_center;
				is_delta = true;
			}
			float atten = nextgi_pt_omni(dist_center, inv_range, v2.w);
			if (atten <= 0.0) {
				continue;
			}
			if (ltype == PT_LIGHT_SPOT) {
				float facing = dot(-light_dir, v2.xyz);
				if (facing <= v4.w) {
					continue;
				}
				float spot_rim = max(1e-4, (1.0 - facing) / max(1.0 - v4.w, 1e-4));
				atten *= max(0.0, 1.0 - pow(spot_rim, v5.x));
				if (atten <= 0.0) {
					continue;
				}
			}
			vec3 dir_ls = vec3(dot(-light_dir, v3.xyz), dot(-light_dir, v4.xyz), dot(-light_dir, -v2.xyz));
			float ies = (lflags & PT_LIGHT_FLAG_IES) != 0u ? ies_sample_atlas(ies_layer, v5.w, dir_ls, pt_ies_atlas, pt_linear_sampler) : 1.0;
			if (ies <= 0.0) {
				continue;
			}
			vec3 proj = vec3(1.0);
			if ((lflags & PT_LIGHT_FLAG_HAS_PROJECTOR) != 0u) {
				if (ltype == PT_LIGHT_SPOT) {
					if (!pt_sample_projector_spot(v6, -light_dir, v3.xyz, v4.xyz, v2.xyz, v8.z, proj)) {
						continue;
					}
				} else {
					proj = pt_sample_projector_omni(v6, dir_ls);
				}
			}
			radiance_weight = v1.rgb * proj * (sign_mul * atten * ies);
		} else if (ltype == PT_LIGHT_AREA) {
			vec2 half_size = vec2(v3.w, v4.w);
			vec3 rel_origin = shadow_origin - v0.xyz;
			vec3 p_local = vec3(dot(rel_origin, v3.xyz), dot(rel_origin, v4.xyz), dot(rel_origin, -v2.xyz));
			vec2 clip_min;
			vec2 clip_max;
			if (!barn_door_clip_rect(p_local, half_size, v5.y, v5.z, clip_min, clip_max)) {
				continue;
			}
			vec2 closest_xy = clamp(p_local.xy, -half_size, half_size);
			vec3 closest_world = v0.xyz + v3.xyz * closest_xy.x + v4.xyz * closest_xy.y;
			float dist_closest = max(length(closest_world - shadow_origin), 1e-4);
			if (dist_closest > v0.w) {
				continue;
			}
			float atten_ltc = nextgi_pt_omni(dist_closest, inv_range, v2.w) * dist_closest * dist_closest;
			if (atten_ltc <= 0.0) {
				continue;
			}
			vec2 u = vec2(nextgi_pt_rnd(r_rng), nextgi_pt_rnd(r_rng));
			vec2 sample_xy = mix(clip_min, clip_max, u);
			vec3 sample_pos = v0.xyz + v3.xyz * sample_xy.x + v4.xyz * sample_xy.y;
			vec3 to_light = sample_pos - shadow_origin;
			float dist = length(to_light);
			if (dist <= 1e-5) {
				continue;
			}
			light_dir = to_light / dist;
			float cos_light = dot(-light_dir, v2.xyz);
			if (cos_light <= 1e-6) {
				continue;
			}
			float clipped_area = max((clip_max.x - clip_min.x) * (clip_max.y - clip_min.y), 1e-8);
			pdf_light = (dist * dist) / (clipped_area * cos_light);
			is_delta = false;
			shadow_dist = dist;

			vec3 dir_ls = vec3(dot(-light_dir, v3.xyz), dot(-light_dir, v4.xyz), dot(-light_dir, -v2.xyz));
			float ies = (lflags & PT_LIGHT_FLAG_IES) != 0u ? ies_sample_atlas(ies_layer, v5.w, dir_ls, pt_ies_atlas, pt_linear_sampler) : 1.0;
			if (ies <= 0.0) {
				continue;
			}
			vec3 area_tex = vec3(1.0);
			if ((lflags & PT_LIGHT_FLAG_HAS_AREA_TEX) != 0u) {
				area_tex = pt_sample_area_texture(v6, sample_xy, half_size);
			}
			// Radiance L_e / pdf_light: matches Forward+ LTC irradiance 1:1.
			radiance_weight = (v1.rgb * area_tex * (sign_mul * atten_ltc * ies)) / max(pdf_light, 1e-8);
		} else {
			continue;
		}

		vec3 scatter_eval;
		float pdf_scatter;
		if (p_is_volume) {
			float phase = pt_hg_phase(dot(-p_view, light_dir), p_phase_g);
			scatter_eval = vec3(phase);
			pdf_scatter = phase;
		} else {
			float n_dot_l = dot(p_normal, light_dir);
			if (n_dot_l <= 0.0) {
				continue;
			}
			scatter_eval = pt_bsdf_eval_nl(p_material, p_normal, p_view, light_dir, n_dot_l, v1.w);
			pdf_scatter = pt_bsdf_pdf(p_material, p_normal, p_view, light_dir);
		}

		float vis = 1.0;
		if (cast_shadow && shadow_opacity > 1e-4) {
			float tmax = is_sky_shadow ? shadow_dist : max(shadow_dist - NEXTGI_PT_ORIGIN_OFFSET, 1e-4);
			if (pt_shadow_occluded(shadow_origin, light_dir, tmax, p_ray_mask)) {
				vis = 1.0 - clamp(shadow_opacity, 0.0, 1.0);
			}
		}
		if (vis <= 1e-6) {
			continue;
		}
		vis *= pt_shadow_transmittance(shadow_origin, light_dir, shadow_dist, is_sky_shadow, r_rng);
		if (vis <= 1e-6) {
			continue;
		}
		float mis_w = is_delta ? 1.0 : pt_mis_weight(pdf_light, pdf_scatter);
		lit += radiance_weight * scatter_eval * (vis * mis_w);
	}

	// Sky NEE via 2D marginal/conditional CDF + balance-heuristic MIS against BSDF/phase sampling.
	if (ref_params.sky_orient_row0.w > 0.5 && ref_params.motion_params.z != 0u) {
		vec2 u_sky = vec2(nextgi_pt_rnd(r_rng), nextgi_pt_rnd(r_rng));
		vec3 sky_dir;
		float pdf_sky;
		pt_sample_sky(u_sky, sky_dir, pdf_sky);
		if (pdf_sky > 1e-8) {
			bool valid_dir = p_is_volume || dot(p_normal, sky_dir) > 0.0;
			if (valid_dir && !pt_shadow_occluded(shadow_origin, sky_dir, 1.0e6, p_ray_mask)) {
				float tr = pt_shadow_transmittance(shadow_origin, sky_dir, 1.0e6, true, r_rng);
				if (tr > 1e-6) {
					vec3 sky_rad = pt_eval_sky(sky_dir);
					vec3 scatter_eval;
					float pdf_scatter;
					if (p_is_volume) {
						float phase = pt_hg_phase(dot(-p_view, sky_dir), p_phase_g);
						scatter_eval = vec3(phase);
						pdf_scatter = phase;
					} else {
						float n_dot_l = dot(p_normal, sky_dir);
						scatter_eval = pt_bsdf_eval_nl(p_material, p_normal, p_view, sky_dir, n_dot_l, 1.0);
						pdf_scatter = pt_bsdf_pdf(p_material, p_normal, p_view, sky_dir);
					}
					float mis_w = pt_mis_weight(pdf_sky, pdf_scatter);
					lit += (sky_rad * scatter_eval * (tr * mis_w)) / pdf_sky;
				}
			}
		}
	}

	return lit;
}

// MIS contribution when a BSDF or volume bounce ray (bounce >= 1) hits an extended light
// (directional sun disk on miss, omni/spot sphere radius, or AreaLight3D quad before p_tmax).
vec3 pt_eval_bsdf_light_hits(vec3 p_origin, vec3 p_dir, float p_tmax, bool p_hit_geom, float p_prev_pdf) {
	if (p_prev_pdf <= 1e-8) {
		return vec3(0.0);
	}
	vec3 contrib = vec3(0.0);
	uint light_count = ref_params.counts.y;
	for (uint i = 0u; i < light_count; i++) {
		uint base = i * 9u;
		vec4 v0 = lights.data[base + 0u];
		vec4 v1 = lights.data[base + 1u];
		vec4 v2 = lights.data[base + 2u];
		vec4 v3 = lights.data[base + 3u];
		vec4 v4 = lights.data[base + 4u];
		vec4 v5 = lights.data[base + 5u];
		vec4 v6 = lights.data[base + 6u];
		vec4 v7 = lights.data[base + 7u];
		vec4 v8 = lights.data[base + 8u];
		uvec3 u7 = floatBitsToUint(v7.xyz);
		uint ltype = u7.x;
		uint lflags = u7.y;
		uint ies_layer = u7.z;
		float inv_range = v7.w;
		float sign_mul = (lflags & PT_LIGHT_FLAG_NEGATIVE) != 0u ? -1.0 : 1.0;

		if (ltype == PT_LIGHT_DIRECTIONAL) {
			if (!p_hit_geom && v3.w > 1e-4 && dot(p_dir, v0.xyz) >= v0.w) {
				float omega = max(2.0 * NEXTGI_PT_PI * (1.0 - v0.w), 1e-8);
				float pdf_light = 1.0 / omega;
				float mis_w = pt_mis_weight(p_prev_pdf, pdf_light);
				contrib += (v1.rgb * sign_mul / omega) * mis_w;
			}
		} else if (ltype == PT_LIGHT_OMNI || ltype == PT_LIGHT_SPOT) {
			float radius = v3.w;
			if (radius <= 1e-4) {
				continue;
			}
			vec3 to_center = v0.xyz - p_origin;
			float dist_center = length(to_center);
			if (dist_center <= radius * 1.001 || dist_center > v0.w + radius) {
				continue;
			}
			float b = dot(to_center, p_dir);
			if (b <= 0.0) {
				continue;
			}
			float c = dist_center * dist_center - radius * radius;
			float disc = b * b - c;
			if (disc <= 0.0) {
				continue;
			}
			float t_hit = b - sqrt(disc);
			if (t_hit <= 1e-4 || (p_hit_geom && t_hit >= p_tmax)) {
				continue;
			}
			float atten = nextgi_pt_omni(dist_center, inv_range, v2.w);
			if (ltype == PT_LIGHT_SPOT) {
				float facing = dot(-p_dir, v2.xyz);
				if (facing <= v4.w) {
					continue;
				}
				float spot_rim = max(1e-4, (1.0 - facing) / max(1.0 - v4.w, 1e-4));
				atten *= max(0.0, 1.0 - pow(spot_rim, v5.x));
			}
			if (atten <= 0.0) {
				continue;
			}
			vec3 dir_ls = vec3(dot(-p_dir, v3.xyz), dot(-p_dir, v4.xyz), dot(-p_dir, -v2.xyz));
			float ies = (lflags & PT_LIGHT_FLAG_IES) != 0u ? ies_sample_atlas(ies_layer, v5.w, dir_ls, pt_ies_atlas, pt_linear_sampler) : 1.0;
			vec3 proj = vec3(1.0);
			if ((lflags & PT_LIGHT_FLAG_HAS_PROJECTOR) != 0u) {
				if (ltype == PT_LIGHT_SPOT) {
					if (!pt_sample_projector_spot(v6, -p_dir, v3.xyz, v4.xyz, v2.xyz, v8.z, proj)) {
						continue;
					}
				} else {
					proj = pt_sample_projector_omni(v6, dir_ls);
				}
			}
			float sin2_max = clamp((radius * radius) / (dist_center * dist_center), 0.0, 0.999999);
			float cos_max = sqrt(1.0 - sin2_max);
			float omega = max(2.0 * NEXTGI_PT_PI * (1.0 - cos_max), 1e-8);
			float pdf_light = 1.0 / omega;
			float mis_w = pt_mis_weight(p_prev_pdf, pdf_light);
			contrib += (v1.rgb * proj * (sign_mul * atten * ies) / omega) * mis_w;
		} else if (ltype == PT_LIGHT_AREA) {
			float cos_light = dot(-p_dir, v2.xyz);
			if (cos_light <= 1e-6) {
				continue;
			}
			float denom = dot(p_dir, v2.xyz);
			float t_hit = dot(v0.xyz - p_origin, v2.xyz) / denom;
			if (t_hit <= 1e-4 || (p_hit_geom && t_hit >= p_tmax)) {
				continue;
			}
			vec2 half_size = vec2(v3.w, v4.w);
			vec3 rel_origin = p_origin - v0.xyz;
			vec3 p_local = vec3(dot(rel_origin, v3.xyz), dot(rel_origin, v4.xyz), dot(rel_origin, -v2.xyz));
			vec2 clip_min;
			vec2 clip_max;
			if (!barn_door_clip_rect(p_local, half_size, v5.y, v5.z, clip_min, clip_max)) {
				continue;
			}
			vec3 hit_pt = p_origin + p_dir * t_hit - v0.xyz;
			vec2 hit_xy = vec2(dot(hit_pt, v3.xyz), dot(hit_pt, v4.xyz));
			if (hit_xy.x < clip_min.x || hit_xy.x > clip_max.x || hit_xy.y < clip_min.y || hit_xy.y > clip_max.y) {
				continue;
			}
			vec2 closest_xy = clamp(p_local.xy, -half_size, half_size);
			vec3 closest_world = v0.xyz + v3.xyz * closest_xy.x + v4.xyz * closest_xy.y;
			float dist_closest = max(length(closest_world - p_origin), 1e-4);
			if (dist_closest > v0.w) {
				continue;
			}
			float atten_ltc = nextgi_pt_omni(dist_closest, inv_range, v2.w) * dist_closest * dist_closest;
			if (atten_ltc <= 0.0) {
				continue;
			}
			vec3 dir_ls = vec3(dot(-p_dir, v3.xyz), dot(-p_dir, v4.xyz), dot(-p_dir, -v2.xyz));
			float ies = (lflags & PT_LIGHT_FLAG_IES) != 0u ? ies_sample_atlas(ies_layer, v5.w, dir_ls, pt_ies_atlas, pt_linear_sampler) : 1.0;
			vec3 area_tex = vec3(1.0);
			if ((lflags & PT_LIGHT_FLAG_HAS_AREA_TEX) != 0u) {
				area_tex = pt_sample_area_texture(v6, hit_xy, half_size);
			}
			float clipped_area = max((clip_max.x - clip_min.x) * (clip_max.y - clip_min.y), 1e-8);
			float pdf_light = (t_hit * t_hit) / (clipped_area * cos_light);
			float mis_w = pt_mis_weight(p_prev_pdf, pdf_light);
			contrib += (v1.rgb * area_tex * (sign_mul * atten_ltc * ies)) * mis_w;
		}
	}
	return contrib;
}

// === PATH INTEGRATION ===

// Legacy D16 reference-match path (used only when PATH_TRACER_FLAG_PRODUCT == 0 so reference_match stays bit-exact).
vec3 path_tracer_radiance_reference(vec3 p_origin, vec3 p_dir, inout uint rng, out vec3 r_albedo, out vec3 r_normal, out float r_hit) {
	r_albedo = vec3(0.0);
	r_normal = vec3(0.0);
	r_hit = 0.0;
	PathTracerHit hit;
	bool unrecorded;
	if (!path_tracer_trace(p_origin, p_dir, 1.0e6, NEXTGI_RT_MASK_GI, hit, unrecorded)) {
		return ref_params.sky_color.rgb;
	}
	vec3 normal = hit.normal;
	vec3 position = hit.position;
	uint material = hit.surface;
	path_tracer_prepare_hit(hit, p_dir, NEXTGI_RT_MASK_GI, normal);
	vec3 first_albedo = NEXTGI_PT_MATERIAL(material).albedo_metal.rgb;
	if (path_tracer_vec3_is_finite(first_albedo)) {
		r_albedo = clamp(first_albedo, vec3(0.0), vec3(1.0));
	}
	float normal_len = length(normal);
	if (path_tracer_vec3_is_finite(normal) && normal_len > 1e-6) {
		r_normal = normal / normal_len;
	}
	r_hit = 1.0;
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
		if (!path_tracer_trace(origin, direction, 1.0e6, NEXTGI_RT_MASK_GI, hit, unrecorded)) {
			if (!unrecorded) {
				accumulated += throughput * ref_params.sky_color.rgb;
			}
			break;
		}
		normal = hit.normal;
		position = hit.position;
		material = hit.surface;
		path_tracer_prepare_hit(hit, direction, NEXTGI_RT_MASK_GI, normal);
		view = -direction;
		accumulated += throughput * NEXTGI_PT_MATERIAL(material).emission_roughness.rgb;
		accumulated += throughput * nextgi_pt_next_event(position, normal, view, material);
	}
	return accumulated;
}

// Product path tracer (#169): all light types + NEE/MIS, sky CDF + MIS, volumetric fog/FogVolume delta tracking,
// legacy Environment.fog fallback, and shutter-masked ray traversal.
vec3 path_tracer_radiance_product(vec3 p_origin, vec3 p_dir, uint p_ray_mask, inout uint rng, out vec3 r_albedo, out vec3 r_normal, out float r_hit) {
	r_albedo = vec3(0.0);
	r_normal = vec3(0.0);
	r_hit = 0.0;

	vec3 origin = p_origin;
	vec3 direction = p_dir;
	vec3 throughput = vec3(1.0);
	vec3 accumulated = vec3(0.0);
	float prev_pdf = 0.0;
	uint max_bounces = ref_params.product.x;
	uint fog_mode = uint(ref_params.vfog_emission.w + 0.5);
	float phase_g = ref_params.vfog_params0.y;

	for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
		if (bounce >= NEXTGI_PT_ROULETTE_FROM) {
			float survival = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
			if (survival <= 0.0 || nextgi_pt_rnd(rng) > survival) {
				break;
			}
			throughput /= survival;
		}

		PathTracerHit hit;
		bool unrecorded;
		bool hit_geom = path_tracer_trace(origin, direction, 1.0e6, p_ray_mask, hit, unrecorded);
		float seg_tmax = hit_geom ? hit.t : 1.0e6;

		// Volumetric fog / FogVolume unbiased Woodcock delta tracking along [0, seg_tmax].
		bool scattered_in_volume = false;
		if (fog_mode == 1u) {
			float t0;
			float t1;
			float max_vol_dist = hit_geom ? seg_tmax : ref_params.vfog_params0.z * 2.0;
			float maj = pt_medium_majorant(origin, direction, max_vol_dist, t0, t1);
			if (maj > 1e-6 && t1 > t0) {
				// On sky miss, respect volumetric_fog_sky_affect by skipping world fog with probability (1 - sky_affect).
				bool track_vol = hit_geom || ref_params.product.w > 0u || (nextgi_pt_rnd(rng) < ref_params.vfog_params0.w);
				if (track_vol) {
					float t = t0;
					for (uint step = 0u; step < PT_MAX_DELTA_STEPS; step++) {
						float dt = -log(max(1.0 - nextgi_pt_rnd(rng), 1e-7)) / maj;
						t += dt;
						if (t >= t1) {
							break;
						}
						float sigma_t;
						vec3 vol_albedo;
						vec3 vol_emit;
						vec3 p_vol = origin + direction * t;
						pt_eval_medium(p_vol, sigma_t, vol_albedo, vol_emit);
						// Unbiased null-collision emission estimator along free-flight steps.
						accumulated += throughput * (vol_emit / maj);
						if (nextgi_pt_rnd(rng) * maj < sigma_t) {
							// Real scattering interaction in medium.
							if (bounce > 0u) {
								accumulated += throughput * pt_eval_bsdf_light_hits(origin, direction, t, true, prev_pdf);
							}
							throughput *= vol_albedo;
							if (max(throughput.r, max(throughput.g, throughput.b)) <= 1e-6) {
								return accumulated;
							}
							accumulated += throughput * pt_next_event_product(p_vol, vec3(0.0, 1.0, 0.0), -direction, 0u, true, phase_g, p_ray_mask, rng);
							vec3 new_dir = pt_sample_hg(direction, phase_g, vec2(nextgi_pt_rnd(rng), nextgi_pt_rnd(rng)), prev_pdf);
							origin = p_vol;
							direction = new_dir;
							scattered_in_volume = true;
							break;
						}
					}
				}
			}
		}

		if (scattered_in_volume) {
			continue;
		}

		// Account for MIS hits on extended lights before geometry (or sun disk on miss) for bounce >= 1.
		if (bounce > 0u) {
			accumulated += throughput * pt_eval_bsdf_light_hits(origin, direction, seg_tmax, hit_geom, prev_pdf);
		}

		if (!hit_geom) {
			if (!unrecorded) {
				vec3 sky_rad = pt_eval_sky(direction);
				float sky_mis = 1.0;
				if (bounce > 0u && ref_params.sky_orient_row0.w > 0.5 && ref_params.motion_params.z != 0u) {
					sky_mis = pt_mis_weight(prev_pdf, pt_sky_pdf(direction));
				}
				if (fog_mode == 2u) {
					vec4 lfog = pt_eval_legacy_fog(origin, direction, 1.0e4, true);
					sky_rad = mix(sky_rad, lfog.rgb, lfog.a);
				}
				accumulated += throughput * sky_rad * sky_mis;
			}
			break;
		}

		vec3 normal = hit.normal;
		vec3 position = hit.position;
		uint material = hit.surface;
		path_tracer_prepare_hit(hit, direction, p_ray_mask, normal);

		if (bounce == 0u) {
			vec3 first_albedo = NEXTGI_PT_MATERIAL(material).albedo_metal.rgb;
			if (path_tracer_vec3_is_finite(first_albedo)) {
				r_albedo = clamp(first_albedo, vec3(0.0), vec3(1.0));
			}
			float normal_len = length(normal);
			if (path_tracer_vec3_is_finite(normal) && normal_len > 1e-6) {
				r_normal = normal / normal_len;
			}
			r_hit = 1.0;
		}

		// Legacy Environment.fog blend on hit segments when volumetric fog is disabled.
		if (fog_mode == 2u) {
			vec4 lfog = pt_eval_legacy_fog(origin, direction, hit.t, false);
			accumulated += throughput * lfog.rgb * lfog.a;
			throughput *= (1.0 - lfog.a);
		}

		vec3 view = -direction;
		accumulated += throughput * NEXTGI_PT_MATERIAL(material).emission_roughness.rgb;
		accumulated += throughput * pt_next_event_product(position, normal, view, material, false, 0.0, p_ray_mask, rng);

		if (bounce == max_bounces) {
			break;
		}
		vec3 next_dir;
		float pdf = 0.0;
		vec3 weight = pt_bsdf_sample_mis(material, normal, view, rng, next_dir, pdf);
		if (pdf <= 1e-8 || !path_tracer_vec3_is_finite(weight)) {
			break;
		}
		throughput *= weight;
		prev_pdf = pdf;
		origin = position + normal * NEXTGI_PT_ORIGIN_OFFSET;
		direction = next_dir;
	}

	return accumulated;
}

// UE's Max Path Intensity: a sample's exposed radiance is scaled down to the clamp (0 is off).
vec3 path_tracer_clamp(vec3 p_radiance) {
	if (!path_tracer_vec3_is_finite(p_radiance)) {
		return vec3(0.0);
	}
	float limit = uintBitsToFloat(ref_params.product.z);
	if (limit <= 0.0) {
		return p_radiance;
	}
	limit /= max(ref_params.exposure_clear.x, 1e-20);
	float peak = max(p_radiance.r, max(p_radiance.g, p_radiance.b));
	return peak > limit ? p_radiance * (limit / peak) : p_radiance;
}

// One sample of a pixel.
vec3 path_tracer_sample_pixel(uvec2 p_pixel, out vec3 r_albedo, out vec3 r_normal, out float r_hit) {
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
		vec3 near_point = ref_params.ray_center.xyz + ref_params.ray_dx.xyz * ndc.x + ref_params.ray_dy.xyz * ndc.y;
		vec3 origin = ref_params.cam_origin_fov.xyz;
		vec3 direction = normalize(near_point - origin);
		if (ref_params.ray_center.w > 0.5) {
			origin = near_point;
			direction = normalize(ref_params.cam_forward_aspect.xyz);
		} else if (ref_params.dof_params.w > 0.5 && ref_params.dof_params.y > 1e-6) {
			// Thin-lens depth of field: perturb ray origin across concentric lens disk aimed at focal plane.
			vec2 lens_u = vec2(nextgi_pt_rnd(rng), nextgi_pt_rnd(rng));
			vec2 disk = pt_sample_concentric_disk(lens_u) * ref_params.dof_params.y;
			vec3 cam_fwd = normalize(ref_params.cam_forward_aspect.xyz);
			float cos_view = max(dot(direction, cam_fwd), 1e-4);
			vec3 focus_point = origin + direction * (ref_params.dof_params.x / cos_view);
			origin = origin + ref_params.cam_right.xyz * disk.x + ref_params.cam_up.xyz * disk.y;
			direction = normalize(focus_point - origin);
		}
		// Shutter slice mask for per-instance motion blur (8-slice masked TLAS when active).
		uint ray_mask = NEXTGI_RT_MASK_GI;
		uint motion_slices = ref_params.motion_params.x;
		if (motion_slices > 0u) {
			uint slice_bits = nextgi_pt_hash(nextgi_pt_hash(index * 0x45D9F3Bu + 0x119DE1F3u) ^ (sample_index * 0x27D4EB2Du) ^ ref_params.counts.x);
			uint slice = slice_bits % motion_slices;
			ray_mask = 1u << slice;
		}
		return path_tracer_clamp(path_tracer_radiance_product(origin, direction, ray_mask, rng, r_albedo, r_normal, r_hit));
	}
	float tan_half = tan(ref_params.cam_origin_fov.w * 0.5);
	float aspect = ref_params.cam_forward_aspect.w;
	vec3 direction = normalize(ref_params.cam_right.xyz * (ndc.x * tan_half * aspect) + ref_params.cam_up.xyz * (ndc.y * tan_half) + ref_params.cam_forward_aspect.xyz);
	return path_tracer_clamp(path_tracer_radiance_reference(ref_params.cam_origin_fov.xyz, direction, rng, r_albedo, r_normal, r_hit));
}

#endif // PATH_TRACER_INC_GLSL
