// GodotNext: ps_swrt_trace (#186). Not a standalone shader: the gather's WRC, confidence, Hi-Z,
// reflection, clamp and blue-noise bindings (set 0, 26-38), shared by the hardware gather
// (nextgi_gather.glsl) and the software sibling (nextgi_gather_swrt.glsl).

#ifndef NEXTGI_GATHER_WRC_BINDINGS_INC_GLSL
#define NEXTGI_GATHER_WRC_BINDINGS_INC_GLSL

layout(set = 0, binding = 26) uniform sampler2D nextgi_wrc_radiance;
layout(set = 0, binding = 27) uniform sampler2D nextgi_wrc_visibility;
layout(set = 0, binding = 29, std430) restrict buffer NextGIGatherConfidence {
	float data[];
}
confidence_data;
layout(set = 0, binding = 30) uniform sampler2D hiz_tex;
layout(set = 0, binding = 31) uniform sampler2D last_frame_tex;
// GodotNext: nextgi_reflect (#60). The reflect variant reuses the gather binding set and writes
// the specular radiance plus its confidence into RB_TEX_REFLECTION.
layout(set = 0, binding = 32) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 33) uniform image2D reflection_image;
// GodotNext: nextgi_firefly_clamp (#54). Clamped-energy counter (Q16): removed and total sample
// luminance in quarter units, summed with 32-bit integer atomics (order independent).
// GodotNext: nextgi_blue_noise (#54). 64x64 RG8 void-and-cluster tile.
layout(set = 0, binding = 38) uniform sampler2D blue_noise_tex;
layout(set = 0, binding = 37, std430) buffer NextGIClampTotals {
	uint removed;
	uint total;
}
clamp_totals;
layout(r16f, set = 0, binding = 35) uniform image2D specular_hit_image;
layout(rgba16f, set = 0, binding = 36) uniform image2D specular_debug_image;
layout(set = 0, binding = 34, std430) buffer NextGIReflectCounters {
	uint traced;
	uint hits;
	uint misses;
	uint pad;
}
reflect_counters;
layout(set = 0, binding = 28, std430) restrict readonly buffer NextGIGatherWRCParams {
	vec4 camera;
	vec4 sky;
	vec4 origin_spacing;
	uvec4 dispatch;
	uint page_rect_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
nextgi_wrc;

#endif // NEXTGI_GATHER_WRC_BINDINGS_INC_GLSL
