#[compute]

#version 460

#VERSION_DEFINES

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF

// GodotNext: ps_swrt_trace (#186). SDF shadow rays for the SRC relight on the
// software tier (#186 step 5): the relight's remaining shadow rays, after its
// masks and shadow-map reuse, march the global SDF toward their light.
//
// One request per receiver and light. The direction and length follow the
// relight's rule (nextgi_src_relight.glsl): a directional light (type 0) is
// traced along its reverse direction for rt_radius meters, a positional
// light up to its position. NextGISWRTTrace::trace_shadow_rays() dispatches
// this; requests and lights use the NextGISWRTTrace::ShadowRequest and
// NextGILight layouts.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../half_inc.glsl"
#include "nextgi_sdf_trace_inc.glsl"

struct SWRTShadowRequest {
	vec4 position_offset; // xyz: receiver, w: origin offset along the normal (<= 0: 1.5 voxels of its level).
	vec4 normal_light; // xyz: receiver normal, w: light index (negative: no ray).
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

layout(set = 0, binding = 27, std430) readonly buffer SWRTShadowRequests {
	SWRTShadowRequest data[];
}
swrt_shadow_requests;

// Per request: 1 lit, 0 occluded, 2 no ray.
layout(set = 0, binding = 28, std430) buffer SWRTShadowResults {
	uint data[];
}
swrt_shadow_results;

layout(set = 0, binding = 29, std430) readonly buffer SWRTLights {
	NextGILight data[];
}
swrt_lights;

layout(push_constant, std430) uniform SWRTShadowPush {
	uint request_count;
	uint light_count;
	float rt_radius;
	uint pad;
}
shadow_push;

void main() {
	uint index = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
	if (index >= shadow_push.request_count) {
		return;
	}
	SWRTShadowRequest request = swrt_shadow_requests.data[index];
	int light_index = int(request.normal_light.w);
	if (request.normal_light.w < 0.0 || uint(light_index) >= shadow_push.light_count) {
		swrt_shadow_results.data[index] = 2u;
		return;
	}
	NextGILight light = swrt_lights.data[uint(light_index)];
	vec3 receiver = request.position_offset.xyz;
	vec3 normal = request.normal_light.xyz;
	normal = dot(normal, normal) > 1e-8 ? normalize(normal) : vec3(0.0, 1.0, 0.0);
	vec3 to_light;
	float tmax;
	uint light_type = uint(light.direction_type.w + 0.5);
	if (light_type == 0u) {
		to_light = -normalize(light.direction_type.xyz);
		tmax = shadow_push.rt_radius;
	} else {
		to_light = light.position_inv_range.xyz - receiver;
		tmax = length(to_light);
		to_light = tmax > 0.0 ? to_light / tmax : vec3(0.0, 1.0, 0.0);
	}
	float offset = request.position_offset.w > 0.0 ? request.position_offset.w : swrt_self_offset(receiver);
	vec3 origin = receiver + normal * offset;
	atomicAdd(swrt_counters.shadow_rays, 1u);
	bool occluded = swrt_shadow_ray(origin, to_light, max(tmax - offset, 0.0));
	if (occluded) {
		atomicAdd(swrt_counters.shadow_occluded, 1u);
	}
	swrt_shadow_results.data[index] = occluded ? 0u : 1u;
}
