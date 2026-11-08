#[compute]

#version 460

#VERSION_DEFINES

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF

// GodotNext: ps_swrt_trace (#186). Gather and reflection variants of the
// software trace. RD_GLSL variants supply NEXTGI_SWRT_GATHER or
// NEXTGI_SWRT_REFLECT through #VERSION_DEFINES. Reflections skip the WRC handoff.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../half_inc.glsl"
#include "nextgi_sdf_trace_inc.glsl"

void main() {
	uvec2 grid = gl_GlobalInvocationID.xy;
	SWRTParams params = swrt_params();
	if (grid.x >= uint(params.grid.x) || grid.y >= uint(params.grid.y)) {
		return;
	}
	uint ray_index = grid.x + uint(params.grid.x) * grid.y;
	vec2 screen = max(params.screen.xy, vec2(1.0));
	vec2 pixel = floor((vec2(grid) + vec2(0.5)) / max(params.grid.xy, vec2(1.0)) * screen);
	pixel = clamp(pixel, vec2(0.0), screen - vec2(1.0));
	vec2 uv = (pixel + vec2(0.5)) / screen;
	vec4 far_h = params.inv_view_proj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec3 far_world = far_h.xyz / max(abs(far_h.w), 1e-6) * sign(far_h.w);
	vec3 origin = params.camera.xyz;
	vec3 direction = far_world - origin;
	if (dot(direction, direction) <= 1e-8) {
		direction = vec3(0.0, 0.0, -1.0);
	}
	direction = normalize(direction);
	float tmax = params.trace.x;
	vec3 normal = direction;
	bool from_surface = false;
	if (params.screen.z > 0.5) {
		float depth = texelFetch(swrt_depth, ivec2(pixel), 0).r;
		if (depth > 0.0001) {
			vec3 hit = swrt_unproject(pixel);
			normal = swrt_geometric_normal(ivec2(pixel), hit);
			origin = hit + normal * params.trace.y;
			direction = swrt_hemisphere(normal, ray_index);
			from_surface = true;
#ifndef NEXTGI_SWRT_REFLECT
			tmax = min(tmax, swrt_handoff(hit));
#endif
		}
	}
	if (!from_surface) {
		tmax = min(tmax, 8.0);
	}
	float screen_t = swrt_screen_trace(origin, direction, pixel, tmax);
	bool allow_wrc = true;
#ifdef NEXTGI_SWRT_REFLECT
	allow_wrc = false;
#endif
	if (screen_t > 0.0 && screen_t <= tmax) {
		vec3 color = swrt_shade(origin + direction * screen_t, normal, 0u, 0xffffffffu, allow_wrc);
		swrt_shade_out.data[ray_index] = nextgi_voxel_pack_r11g11b10(color);
		atomicAdd(swrt_counters.steps, 1u);
		atomicAdd(swrt_counters.hits, 1u);
		return;
	}
	float hit_t;
	vec3 hit_normal;
	uint hit_flag;
	uint hit_instance;
	uint hit_steps;
	bool detail = params.trace.z > 0.5;
	swrt_march(origin, direction, tmax, detail, hit_t, hit_normal, hit_flag, hit_instance, hit_steps);
	atomicAdd(swrt_counters.steps, hit_steps);
	uint detail_hit = detail && hit_instance != 0xffffffffu ? 1u : 0u;
	vec3 color = hit_flag == 1u ? swrt_shade(origin + direction * hit_t, hit_normal, detail_hit, hit_instance, allow_wrc) : swrt_shade(origin, direction, 0u, 0xffffffffu, allow_wrc);
	swrt_shade_out.data[ray_index] = nextgi_voxel_pack_r11g11b10(color);
	if (hit_flag == 1u) {
		atomicAdd(swrt_counters.hits, 1u);
	} else {
		atomicAdd(swrt_counters.misses, 1u);
	}
}
