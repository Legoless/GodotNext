#[compute]

#version 460

#VERSION_DEFINES

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF

// GodotNext: ps_swrt_trace (#186). Software-trace kernels. RD_GLSL variants
// supply one of NEXTGI_SWRT_GATHER, NEXTGI_SWRT_REFLECT,
// NEXTGI_SWRT_FROXEL_MARK or NEXTGI_SWRT_FROXEL_CULL
// through #VERSION_DEFINES. No ray query: this ShaderRD compiles on every
// Forward+ driver.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../half_inc.glsl"
#include "nextgi_reflect_inc.glsl"
#include "nextgi_sdf_trace_inc.glsl"

// One traced ray of the gather or reflection grid. NextGISWRTTrace::RayResult mirrors it.
struct SWRTRayResult {
	vec4 surface_offset; // xyz: the depth-reconstructed surface, w: the origin offset along surface_normal.
	vec4 surface_normal; // xyz: the normal the ray left from, w: roughness (reflections).
	vec4 direction_tmax; // xyz: direction, w: tmax.
	vec4 hit_normal_t; // xyz: hit normal, w: hit t (negative: no surface hit).
	vec4 radiance_flags; // rgb: radiance, w: SWRT_HIT_* flags (uint bits).
};

layout(set = 0, binding = 27, std430) buffer SWRTRayResults {
	SWRTRayResult data[];
}
swrt_rays;

struct SWRTShadowRequest {
	vec4 position_offset;
	vec4 normal_light;
};

// With extra.z set, the gather writes one shadow request per cell toward light 0
// (NextGISWRTTrace's verification readback feeds the shadow kernel with them).
layout(set = 0, binding = 29, std430) buffer SWRTShadowRequestsOut {
	SWRTShadowRequest data[];
}
swrt_shadow_requests_out;

#if defined(NEXTGI_SWRT_FROXEL_MARK) || defined(NEXTGI_SWRT_FROXEL_CULL)

ivec2 swrt_grid_pixel(uvec2 cell) {
	SWRTParams params = swrt_params();
	vec2 screen = max(params.screen.xy, vec2(1.0));
	return ivec2(clamp((vec2(cell) + vec2(0.5)) * params.grid.zw, vec2(0.0), screen - vec2(1.0)));
}

#ifdef NEXTGI_SWRT_FROXEL_MARK
// Marks the froxels that hold a ray-grid surface (the screen-probe positions).
void main() {
	uvec2 cell = gl_GlobalInvocationID.xy;
	SWRTParams params = swrt_params();
	if (cell.x >= uint(params.grid.x) || cell.y >= uint(params.grid.y) || params.screen.z < 0.5) {
		return;
	}
	ivec2 pixel = swrt_grid_pixel(cell);
	float depth = texelFetch(swrt_depth, pixel, 0).r;
	if (nextgi_view_is_background(depth)) {
		return;
	}
	uint froxel = swrt_froxel_cell(swrt_reconstruct(pixel, depth));
	if (froxel != 0xffffffffu) {
		atomicOr(swrt_froxel.data[froxel * (SWRT_FROXEL_CAP + 1u)], SWRT_FROXEL_MARKED);
	}
}
#else
// Lists, per marked froxel, the instances whose mesh SDF comes within the 2 m
// detail range of the froxel: a bounds test, then a mesh-SDF sample at the
// froxel center against its radius plus the range. x: instance, y: froxel.
// The count word keeps counting past SWRT_FROXEL_CAP, so the trace knows the
// list is incomplete (swrt_trace_detail).
vec3 swrt_froxel_point(uvec3 cell, vec3 offset) {
	SWRTParams params = swrt_params();
	vec3 cells = params.froxel.xyz;
	vec3 f = (vec3(cell) + offset) / cells;
	float near = max(params.froxel_depth.x, 1e-3);
	float far = max(params.froxel_depth.y, near * 1.001);
	float linear = near * pow(far / near, f.z);
	return nextgi_view_reconstruct(params.inv_view_proj, f.xy, nextgi_view_depth_from_linear(params.depth_coefficients, linear));
}

void main() {
	SWRTParams params = swrt_params();
	uint instance_index = gl_GlobalInvocationID.x;
	uint froxel = gl_GlobalInvocationID.y + gl_GlobalInvocationID.z * gl_NumWorkGroups.y * 8u;
	uvec3 cells = uvec3(params.froxel.xyz);
	uint froxel_count = cells.x * cells.y * cells.z;
	if (instance_index >= uint(params.froxel_depth.z) || froxel >= froxel_count) {
		return;
	}
	uint base = froxel * (SWRT_FROXEL_CAP + 1u);
	if ((swrt_froxel.data[base] & SWRT_FROXEL_MARKED) == 0u) {
		return;
	}
	uvec3 cell = uvec3(froxel % cells.x, (froxel / cells.x) % cells.y, froxel / (cells.x * cells.y));
	vec3 lo = vec3(1e30);
	vec3 hi = vec3(-1e30);
	float offset = 0.0;
	for (uint corner = 0u; corner < 8u; corner++) {
		vec3 p = swrt_froxel_point(cell, vec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u));
		lo = min(lo, p);
		hi = max(hi, p);
		offset = max(offset, swrt_self_offset(p));
	}
	vec3 center = (lo + hi) * 0.5;
	// A ray leaves its surface in the cell, offset along the normal, and its detail range is 2 m.
	float radius = length(hi - lo) * 0.5 + offset + SWRT_DETAIL_METERS;
	SWRTInstance instance = swrt_instances.data[instance_index];
	vec3 closest = clamp(center, instance.world_min.xyz, instance.world_max.xyz);
	if (length(closest - center) > radius) {
		return;
	}
	float voxel_size;
	float distance = swrt_instance_distance(instance_index, center, voxel_size);
	// The mesh SDF clamps to its band outside its bounds, so the sample is a lower bound there.
	if (distance > radius) {
		return;
	}
	uint slot = atomicAdd(swrt_froxel.data[base], 1u) & SWRT_FROXEL_COUNT_MASK;
	if (slot < SWRT_FROXEL_CAP) {
		swrt_froxel.data[base + 1u + slot] = instance_index;
	}
}
#endif

#endif

#if defined(NEXTGI_SWRT_GATHER) || defined(NEXTGI_SWRT_REFLECT)
void swrt_write(uint index, vec3 surface, float offset, vec3 surface_normal, float roughness, vec3 direction, float tmax, SWRTHit hit, vec3 radiance) {
	SWRTRayResult result;
	result.surface_offset = vec4(surface, offset);
	result.surface_normal = vec4(surface_normal, roughness);
	result.direction_tmax = vec4(direction, tmax);
	result.hit_normal_t = vec4(hit.normal, (hit.flags & SWRT_HIT_SURFACE) != 0u ? hit.t : -1.0);
	result.radiance_flags = vec4(radiance, uintBitsToFloat(hit.flags));
	swrt_rays.data[index] = result;
}

void main() {
	uvec2 cell = gl_GlobalInvocationID.xy;
	SWRTParams params = swrt_params();
	if (cell.x >= uint(params.grid.x) || cell.y >= uint(params.grid.y)) {
		return;
	}
	uint ray_index = cell.x + uint(params.grid.x) * cell.y;
	vec2 screen = max(params.screen.xy, vec2(1.0));
	ivec2 pixel = ivec2(clamp((vec2(cell) + vec2(0.5)) * params.grid.zw, vec2(0.0), screen - vec2(1.0)));
	SWRTHit skipped = swrt_no_hit(0u);
	skipped.flags = SWRT_HIT_SKIPPED;
	float depth = params.screen.z > 0.5 ? texelFetch(swrt_depth, pixel, 0).r : 0.0;
	if (nextgi_view_is_background(depth)) {
#ifdef NEXTGI_SWRT_GATHER
		if (params.extra.z > 0.5) {
			swrt_shadow_requests_out.data[ray_index].normal_light = vec4(0.0, 0.0, 0.0, -1.0);
		}
#endif
		swrt_write(ray_index, vec3(0.0), 0.0, vec3(0.0), 1.0, vec3(0.0), 0.0, skipped, vec3(0.0));
		atomicAdd(swrt_counters.skipped, 1u);
		return;
	}
	vec3 surface = swrt_reconstruct(pixel, depth);
	bool has_normal_roughness = params.extra.x > 0.5;
	vec3 normal = swrt_surface_normal(pixel, surface, has_normal_roughness);
	float offset = swrt_self_offset(surface);
	vec3 origin = surface + normal * offset;
#ifdef NEXTGI_SWRT_GATHER
	if (params.extra.z > 0.5) {
		swrt_shadow_requests_out.data[ray_index].position_offset = vec4(surface, offset);
		swrt_shadow_requests_out.data[ray_index].normal_light = vec4(normal, 0.0);
	}
#endif
	vec3 view_dir = normalize(params.camera.xyz - surface);
	uint hash = nextgi_reflect_hash(uint(pixel.x) ^ (uint(pixel.y) << 16u) ^ (params.counts.x * 0x9e3779b9u));
	vec2 u = vec2(float(hash & 0xFFFFu), float(hash >> 16u)) / 65535.0;

#ifdef NEXTGI_SWRT_REFLECT
	// Reflections: GGX around the prepass normal, only up to max_roughness_to_trace,
	// with no WRC handoff (#186 step 4).
	float roughness = has_normal_roughness ? nextgi_reflect_decode_roughness(texelFetch(swrt_normal_roughness, pixel, 0).a) : 1.0;
	if (!has_normal_roughness || roughness > params.trace.w) {
		swrt_write(ray_index, surface, offset, normal, roughness, vec3(0.0), 0.0, skipped, vec3(0.0));
		atomicAdd(swrt_counters.skipped, 1u);
		return;
	}
	vec3 direction = nextgi_reflect_sample_ggx(normal, view_dir, roughness, u);
	if (dot(direction, normal) <= 0.0) {
		direction = reflect(-view_dir, normal);
	}
	float tmax = params.froxel.w;
	atomicAdd(swrt_counters.reflect_rays, 1u);
#else
	// Gather: a cosine-weighted hemisphere ray, cut at the WRC handoff (1.5x probe spacing) when the WRC is valid.
	float roughness = 1.0;
	float radius = sqrt(u.x);
	float theta = 6.2831853 * u.y;
	vec3 helper = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(helper, normal));
	vec3 bitangent = cross(normal, tangent);
	vec3 direction = normalize(tangent * (radius * cos(theta)) + bitangent * (radius * sin(theta)) + normal * sqrt(max(1.0 - u.x, 0.0)));
	float tmax = params.trace.x;
	bool handoff = false;
	if (params.flags.z > 0.5) {
		float wrc_handoff = nextgi_wrc_handoff(origin);
		if (wrc_handoff > 0.0 && wrc_handoff < tmax) {
			tmax = wrc_handoff;
			handoff = true;
		}
	}
	atomicAdd(swrt_counters.gather_rays, 1u);
#endif

	// Screen trace first, over the hardware tier's screen range (the handoff
	// distance, 1.5x the WRC probe spacing at the origin). On a screen miss that
	// stopped, the SDF trace resumes at the last sample before the stop, pulled
	// back as on the hardware tier (#186 step 4).
	vec3 trace_origin = origin;
	float trace_tmax = tmax;
	float resume = 0.0;
	if (params.flags.y > 0.5) {
		vec3 screen_color;
		vec3 march_end;
		float screen_tmax = min(tmax, swrt_screen_range(origin));
		if (nextgi_screen_trace_hiz(params.view_proj, params.inv_view_proj, params.camera.xyz, params.screen.xy, params.screen.w, origin, direction, screen_tmax, screen_color, march_end)) {
			SWRTHit screen_hit = swrt_no_hit(0u);
			screen_hit.t = length(march_end - origin);
			screen_hit.normal = -direction;
			screen_hit.flags = SWRT_HIT_SURFACE | SWRT_HIT_SCREEN;
			swrt_write(ray_index, surface, offset, normal, roughness, direction, tmax, screen_hit, screen_color);
			atomicAdd(swrt_counters.screen_hits, 1u);
#ifdef NEXTGI_SWRT_REFLECT
			atomicAdd(swrt_counters.reflect_hits, 1u);
#else
			atomicAdd(swrt_counters.gather_hits, 1u);
#endif
			return;
		}
		float marched = length(march_end - origin);
		uint level = swrt_level_at(march_end);
		if (level == SWRT_NO_LEVEL) {
			level = max(swrt_active_levels(), 1u) - 1u;
		}
		resume = nextgi_screen_resume(origin, direction, marched, tmax, swrt_level_voxel(level), trace_origin, trace_tmax);
	}
	bool detail = params.trace.z > 0.5 && params.extra.y > 0.5;
	uint froxel = detail ? swrt_froxel_cell(surface) : 0xffffffffu;
	SWRTHit hit = swrt_trace(trace_origin, direction, trace_tmax, detail, froxel, resume);
	atomicAdd(swrt_counters.steps, hit.steps);
	atomicAdd(swrt_counters.coarse_steps, swrt_coarse_steps); // GodotNext: global_sdf_coarse_bound (#257)
	vec3 radiance = swrt_shade(hit, trace_origin, direction, true, trace_tmax);
	if ((hit.flags & SWRT_HIT_SURFACE) != 0u) {
#ifdef NEXTGI_SWRT_REFLECT
		atomicAdd(swrt_counters.reflect_hits, 1u);
#else
		atomicAdd(swrt_counters.gather_hits, 1u);
#endif
		if ((hit.flags & SWRT_HIT_EXHAUSTED) != 0u) {
			atomicAdd(swrt_counters.exhausted, 1u);
		}
		if ((hit.flags & SWRT_HIT_DETAIL) != 0u) {
			atomicAdd(swrt_counters.detail_hits, 1u);
		}
	} else {
#ifndef NEXTGI_SWRT_REFLECT
		if (handoff && !((hit.flags & SWRT_HIT_SURFACE) != 0u)) {
			hit.flags |= SWRT_HIT_HANDOFF;
		}
#endif
		if ((hit.flags & SWRT_HIT_WRC) != 0u) {
			atomicAdd(swrt_counters.wrc_samples, 1u);
		}
	}
	if ((hit.flags & SWRT_HIT_SURFACE) != 0u && hit.t >= 0.0) {
		hit.t += resume;
	}
	swrt_write(ray_index, surface, offset, normal, roughness, direction, tmax, hit, radiance);
}
#endif
