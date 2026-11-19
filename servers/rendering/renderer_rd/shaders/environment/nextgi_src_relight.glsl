#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// SRC world-space relight (#31, D14, §2.1 line 2b). One 8x8 workgroup.
// GodotNext: nextgi_shadow_policy (#145). nextgi_shadow_ray below traces through the shared shadow-ray include,
// so the relight applies the lights' caster masks and the alpha-tested casters as RT shadows and MegaLights do.
// A light with an allocated mask bit and no alpha-tested caster on it keeps the opaque single-proceed query.
// No shared memory. No position fetch.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

#define NEXTGI_SHADOW_TLAS nextgi_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 20
#define NEXTGI_SHADOW_ALPHA_BINDING 21
// The alpha variant (NextGIRelight::get_shadow_alpha_variant_defines()), on devices with bindless tables while an
// alpha-tested caster needs it: alpha-tested casters sample their albedo texture's alpha, and the rays that meet a
// caster whose alpha comes from shader code go to the RT-pipeline retrace, which writes their mask bit after this
// dispatch (nextgi_src_relight_retrace.glsl).
#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_CHUNK0_BINDING 23
#define NEXTGI_SHADOW_CHUNK1_BINDING 24
#define NEXTGI_SHADOW_CHUNK2_BINDING 25
#define NEXTGI_SHADOW_CHUNK3_BINDING 26
#define NEXTGI_SHADOW_SAMPLER_BINDING 27
#define NEXTGI_SHADOW_RETRACE_BINDING 28
#define NEXTGI_SHADOW_TEXTURE_SET 1
#endif
#include "nextgi_shadow_ray_inc.glsl"

// Per light: the light word and the caster mask (NextGIRelight). The far-field dispatch binds words that trace
// every far proxy as opaque, as before.
layout(set = 0, binding = 22, std430) restrict readonly buffer NextGILightShadows {
	uvec2 data[];
}
light_shadows;

// True when something shadows the light along the ray.
bool nextgi_shadow_ray(vec3 origin, vec3 direction, float tmax, uint light_index) {
	uvec2 policy = light_shadows.data[light_index];
	return trace_shadow(origin, direction, tmax, policy.x, policy.y) < 0.5;
}

#include "nextgi_src_relight_inc.glsl"
