#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// GodotNext: nextgi_shadow_policy (#145). GM-4's caster-mask matrix (plan §21.3.3): traces the check's rays
// (next_gi_shadow_policy.h, nextgi_shadow_check_build()) through trace_shadow() with the lights' words of this
// frame, and counts how many agree with the occlusion the raster caster rule gives them on the CPU. Run by
// rendering/global_illumination/nextgi/debug/shadow_ray_check.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT shadow_tlas;
#define NEXTGI_SHADOW_TLAS shadow_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 1
#define NEXTGI_SHADOW_ALPHA_BINDING 2
#include "nextgi_shadow_ray_inc.glsl"

// Two vec4 per ray: origin and tmax, then direction and the light index (as bits).
layout(set = 0, binding = 3, std430) restrict readonly buffer CheckRays {
	vec4 data[];
}
check_rays;

// The expected occlusion, one byte per ray.
layout(set = 0, binding = 4, std430) restrict readonly buffer CheckExpected {
	uint data[];
}
check_expected;

// Per light: the word and the caster mask.
layout(set = 0, binding = 5, std430) restrict readonly buffer CheckLights {
	uvec2 data[];
}
check_lights;

// next_gi_shadow_check.h, NextGIShadowCheckCounter.
layout(set = 0, binding = 6, std430) restrict buffer CheckCounters {
	uint data[];
}
check_counters;

layout(push_constant, std430) uniform Params {
	uint ray_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

const uint COUNTER_RAYS = 0u;
const uint COUNTER_AGREE = 1u;
const uint COUNTER_DISAGREE = 2u;
const uint COUNTER_EXPECTED_OCCLUDED = 3u;
const uint COUNTER_TRACED_OCCLUDED = 4u;
const uint COUNTER_PATH_RAYS = 5u; // + path (fast, loop, fallback)
const uint COUNTER_PATH_DISAGREE = 8u; // + path
const uint COUNTER_ALPHA_SAMPLES = 11u;
const uint COUNTER_FIRST_DISAGREEMENT = 12u;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.ray_count) {
		return;
	}
	vec4 origin_tmax = check_rays.data[index * 2u];
	vec4 direction_light = check_rays.data[index * 2u + 1u];
	uvec2 policy = check_lights.data[floatBitsToUint(direction_light.w)];
	bool occluded = trace_shadow(origin_tmax.xyz, direction_light.xyz, origin_tmax.w, policy.x, policy.y) < 0.5;
	bool expected = ((check_expected.data[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu) != 0u;
	uint path = (policy.x & NEXTGI_SHADOW_MODE_FALLBACK) != 0u ? 2u : ((policy.x & NEXTGI_SHADOW_MODE_LOOP) != 0u ? 1u : 0u);
	atomicAdd(check_counters.data[COUNTER_RAYS], 1u);
	atomicAdd(check_counters.data[COUNTER_PATH_RAYS + path], 1u);
	if (expected) {
		atomicAdd(check_counters.data[COUNTER_EXPECTED_OCCLUDED], 1u);
	}
	if (occluded) {
		atomicAdd(check_counters.data[COUNTER_TRACED_OCCLUDED], 1u);
	}
	if (nextgi_shadow_alpha_samples > 0u) {
		atomicAdd(check_counters.data[COUNTER_ALPHA_SAMPLES], nextgi_shadow_alpha_samples);
	}
	if (occluded == expected) {
		atomicAdd(check_counters.data[COUNTER_AGREE], 1u);
	} else {
		atomicAdd(check_counters.data[COUNTER_DISAGREE], 1u);
		atomicAdd(check_counters.data[COUNTER_PATH_DISAGREE + path], 1u);
		atomicMin(check_counters.data[COUNTER_FIRST_DISAGREEMENT], index);
	}
}
