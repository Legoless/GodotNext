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

#ifdef NEXTGI_SWRT_LITE_PROBE
// GodotNext: ps_lumen_lite (#183). One workgroup traces one WRC probe. detail_hit stays 0
// so swrt_shade does not read extra.z as a card count. Binding 27 exists only on this variant.

layout(push_constant, std430) uniform LiteProbePush {
	uint probe_base;
	uint pad0;
	uint pad1;
	uint pad2;
}
lite_probe_push;

layout(set = 0, binding = 27, std430) buffer LiteProbeOut {
	vec4 data[8192];
}
lite_probe_out;

shared vec3 lite_radiance_samples[64];
shared uint lite_backfaces;

void main() {
	uint local = gl_LocalInvocationIndex;
	if (local == 0u) {
		lite_backfaces = 0u;
	}
	barrier();
	uint probe = (lite_probe_push.probe_base + gl_WorkGroupID.x) % 8192u;
	uint level = min(probe / 4096u, 1u);
	float spacing = level == 0u ? 1.0 : 2.0;
	uint in_level = probe % 4096u;
	vec3 cell = vec3(float(in_level % 16u), float((in_level / 16u) % 16u), float(in_level / 256u));
	SWRTParams params = swrt_params();
	vec3 camera = params.camera.xyz;
	vec3 origin_snap = floor(camera / spacing) * spacing;
	vec3 probe_pos = origin_snap + (cell - vec3(7.5)) * spacing;
	ivec2 texel = ivec2(int(local % 8u), int(local / 8u));
	vec2 uv = (vec2(texel) + vec2(0.5)) / 8.0 * 2.0 - 1.0;
	vec3 direction = vec3(uv.x, uv.y, 1.0 - abs(uv.x) - abs(uv.y));
	if (direction.z < 0.0) {
		float flipped_x = (1.0 - abs(direction.y)) * sign(direction.x);
		float flipped_y = (1.0 - abs(direction.x)) * sign(direction.y);
		direction.x = flipped_x;
		direction.y = flipped_y;
	}
	direction = normalize(direction);
	float hit_t;
	vec3 hit_normal;
	uint hit_flag;
	uint hit_instance;
	uint hit_steps;
	swrt_march(probe_pos, direction, 32.0, false, hit_t, hit_normal, hit_flag, hit_instance, hit_steps);
	bool back_face = hit_flag == 1u && dot(hit_normal, direction) > 0.0 && hit_t < 0.5 * spacing;
	if (back_face) {
		atomicAdd(lite_backfaces, 1u);
	}
	vec3 shaded = swrt_shade(hit_flag == 1u ? probe_pos + direction * hit_t : probe_pos, hit_flag == 1u ? hit_normal : direction, 0u, 0xffffffffu, true);
	lite_radiance_samples[local] = shaded;
	barrier();
	if (local == 0u) {
		vec3 average = vec3(0.0);
		for (uint i = 0u; i < 64u; i++) {
			average += lite_radiance_samples[i];
		}
		average *= 1.0 / 64.0;
		lite_probe_out.data[probe] = vec4(average, float(lite_backfaces) / 64.0);
	}
}
#endif

#ifndef NEXTGI_SWRT_LITE_PROBE
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
#endif
