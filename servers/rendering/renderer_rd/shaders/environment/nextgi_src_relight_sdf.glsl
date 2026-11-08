#[compute]

#version 460

#VERSION_DEFINES

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF

// GodotNext: ps_swrt_trace (#186). Shadow rays that remain after the
// scenario-light masks march the global SDF.

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
	vec3 origin = params.camera.xyz;
	vec3 direction = normalize(params.light_dir.xyz);
	float tmax = min(params.trace.x, 32.0);
	vec3 normal = direction;
	if (params.screen.z > 0.5) {
		float depth = texelFetch(swrt_depth, ivec2(pixel), 0).r;
		if (depth > 0.0001) {
			vec3 hit = swrt_unproject(pixel);
			normal = swrt_geometric_normal(ivec2(pixel), hit);
			origin = hit + normal * params.trace.y;
			if (dot(params.light_dir.xyz, params.light_dir.xyz) > 1e-8) {
				direction = normalize(params.light_dir.xyz);
			}
		}
	}
	float hit_t;
	vec3 hit_normal;
	uint hit_flag;
	uint hit_instance;
	uint hit_steps;
	swrt_march(origin, direction, tmax, params.trace.z > 0.5, hit_t, hit_normal, hit_flag, hit_instance, hit_steps);
	atomicAdd(swrt_counters.shadow_rays, 1u);
	atomicAdd(swrt_counters.steps, hit_steps);
	if (hit_flag == 1u) {
		vec3 color = swrt_shade(origin + direction * hit_t, hit_normal, 0u, hit_instance, false);
		swrt_shade_out.data[ray_index] = nextgi_voxel_pack_r11g11b10(color);
		atomicAdd(swrt_counters.hits, 1u);
	}
}
