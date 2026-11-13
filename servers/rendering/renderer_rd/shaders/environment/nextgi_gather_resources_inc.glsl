// GodotNext: ps_swrt_trace (#186). Not a standalone shader: the gather's set-0 resources
// (bindings 1-25), its params, counters and the reference structs, shared by the hardware gather
// and the software sibling. The includer includes nextgi_hit_inc.glsl and
// nextgi_records_inc.glsl first.

#ifndef NEXTGI_GATHER_RESOURCES_INC_GLSL
#define NEXTGI_GATHER_RESOURCES_INC_GLSL

// GodotNext: nextgi_view_math. One reconstruction for every NextGI pass.
#include "nextgi_view_inc.glsl"

// GodotNext: nextgi_blue_noise (#54). R2-rotated 64x64 tile lookup, wrapping by bitmask.
vec2 nextgi_blue_noise_sample(ivec2 p_pixel, uint p_frame) {
	vec2 offset = fract(vec2(p_frame) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (p_pixel + ivec2(offset)) & ivec2(63);
	return texelFetch(blue_noise_tex, coord, 0).rg;
}

layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D radiance_tex;
layout(set = 0, binding = 3) uniform sampler2D material_depth_tex;
layout(set = 0, binding = 4) uniform sampler2D sky_tex;
// Binding 5 stays free: the gather only writes the SRC indirect texture (binding 17). Sampling it in
// the same set would leave it in a read-only layout on Vulkan while the store runs.

layout(set = 0, binding = 6, std430) restrict readonly buffer NextGIGatherCards {
	NextGICard data[];
}
cards;

layout(set = 0, binding = 7, std430) restrict readonly buffer NextGIGatherCardSets {
	NextGICardSet data[];
}
card_sets;

layout(set = 0, binding = 8, std430) restrict readonly buffer NextGIGatherRecords {
	NextGIInstanceRecord data[];
}
records;

// GodotNext: ps_swrt_trace (#186). The triangle records and the arena are the hardware tier's.
#ifndef NEXTGI_TRACE_SDF
layout(set = 0, binding = 9, std430) restrict readonly buffer NextGIGatherSurfaces {
	NextGISurfaceRecord data[];
}
surfaces;

layout(set = 0, binding = 10, std430) restrict readonly buffer NextGIChunk0 {
	uint data[];
}
chunk0;
layout(set = 0, binding = 11, std430) restrict readonly buffer NextGIChunk1 {
	uint data[];
}
chunk1;
layout(set = 0, binding = 12, std430) restrict readonly buffer NextGIChunk2 {
	uint data[];
}
chunk2;
layout(set = 0, binding = 13, std430) restrict readonly buffer NextGIChunk3 {
	uint data[];
}
chunk3;
#endif // !NEXTGI_TRACE_SDF

layout(set = 0, binding = 14, std430) restrict readonly buffer NextGIPageRects {
	uvec2 data[];
}
page_rects;

layout(rgba16f, set = 0, binding = 15) uniform image2D probe_image;
layout(rgba16f, set = 0, binding = 16) uniform image2D ambient_image;
layout(rgba16f, set = 0, binding = 17) uniform image2D indirect_image;

layout(set = 0, binding = 18, std430) restrict readonly buffer NextGIGatherParams {
	mat4 inv_view_proj;
	mat4 prev_view_proj;
	vec4 camera_pos;
	vec4 prev_camera_pos;
	vec4 ambient_color;
	vec4 screen_jitter; // xy screen size, zw hammersley jitter in the tile
	uint frame;
	uint probes_x;
	uint probes_y;
	float rt_radius;
	float max_origin_offset;
	uint sky_enabled;
	uint feedback_count;
	uint feedback_stride;
	uint record_count;
	uint velocity_enabled;
	uint page_rect_count;
	uint pad0;
	uint pad1;
#ifdef NEXTGI_TRACE_SDF
	// GodotNext: ps_swrt_trace (#186). The SRC card-set and card counts the detail hits' D6
	// lookup checks its indices against (pad2 and pad4 in the hardware shader).
	uint card_set_count;
#else
	uint pad2;
#endif
	float max_trace_roughness;
	float reflect_divisor;
	uint reflect_enabled;
	uint reflect_debug;
	uint sampling_mode;
#ifdef NEXTGI_TRACE_SDF
	uint card_count;
#else
	uint pad4;
#endif
	uint ray_stride;
	uint ray_count;
	// GodotNext: nextgi_reflection_behaviours (#132). Fog at reflection hits.
	uint fog_enabled;
	uint fog_depth_mode;
	float fog_density;
	float fog_height;
	float fog_height_density;
	float fog_depth_begin;
	float fog_depth_end;
	float fog_depth_curve;
	uint fog_pad0; // GodotNext: nextgi_view_math. Explicit std430 padding, as in GatherParamsGPU.
	uint fog_pad1;
	vec4 fog_light_color;
	float far_distance;
	uint far_occlusion_only;
	uint front_layer;
	uint front_pad0;
	vec4 view_to_world[3]; // GodotNext: nextgi_view_math. The normal-roughness buffer is in view space.
}
params;

layout(set = 0, binding = 19, std430) buffer NextGIGatherCounters {
	uint rays;
	uint misses;
	uint history_resets;
	uint uncovered;
	uint coverage_hits;
	uint coverage_rays;
	uint miss_radiance_bits; // packed as a scaled luminance
	uint wrc_samples;
	uint sky_samples;
	uint screen_hits;
	// GodotNext: ps_swrt_trace (#186). The software tier's SDF hits, the step-limit and
	// near-origin ones among them, detail hits, D6-resolved detail hits and the hit distance sum
	// (cm). Only the software sibling writes them.
	uint sdf_hits;
	uint sdf_exhausted;
	uint sdf_near;
	uint sdf_detail_hits;
	uint sdf_d6_resolved;
	uint sdf_hit_cm_sum;
	// GodotNext: nextgi_card_normal (#251). SRC feedback rays traced, and those that hit nothing.
	uint feedback_rays;
	uint feedback_escapes;
}
counters;

layout(set = 0, binding = 20, std430) restrict readonly buffer NextGIFeedbackPages {
	uvec4 data[]; // x = indirect texel (x | y << 16), y = record, z = card
}
feedback_pages;

// GodotNext: ps_swrt_trace (#186). The hit-radiance reference is the hardware tier's.
#ifndef NEXTGI_TRACE_SDF
struct NextGIRefMaterial {
	vec4 albedo_metal;
	vec4 emission_roughness;
	vec4 specular_mode;
};

layout(set = 0, binding = 23, std430) restrict readonly buffer NextGIGatherRefMaterials {
	NextGIRefMaterial data[];
}
materials;

layout(set = 0, binding = 24, std430) restrict readonly buffer NextGIGatherRefLights {
	vec4 data[];
}
lights;

layout(set = 0, binding = 25, std430) restrict readonly buffer NextGIGatherRefParams {
	vec4 cam_origin_fov;
	vec4 cam_forward_aspect;
	vec4 cam_up;
	vec4 cam_right;
	uvec4 dispatch;
	uvec4 counts;
	vec4 sky_color;
	vec4 exposure_clear;
}
ref_params;
#endif // !NEXTGI_TRACE_SDF

#endif // NEXTGI_GATHER_RESOURCES_INC_GLSL
