#[raygen]

#version 460
#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// GodotNext: path_tracer (#167). The product path tracer's hit-group backend (RendererRD::PathTracer): the second
// ray generation stage of the NextGI hit pipeline's base library (NextGIHitMaterials). Each path vertex is found
// with inline ray query over the shared TLAS, as the inline backend finds it, and its material is evaluated by the
// geometry's #130 hit group (path_tracer_prepare_hit()). One sample per pixel into the view's history.

#include "nextgi_hit_layout_inc.glsl"

layout(set = 2, binding = 0, std430) restrict readonly buffer HitMaterialParams {
	uint data[];
}
hit_material_params[];

// Keeps the parameter table a run-time array (nextgi_hit_texture_table_probe()).
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].data[0];
}

layout(location = 0) rayPayloadEXT NextGIHitPayload payload;

#define NEXTGI_TLAS hit_tlas
#include "nextgi_rt_inc.glsl"

// The estimator's names for the hit layout's bindings.
#define ref_params hit_pt_params
#define lights hit_pt_lights
#define records hit_records
#define surfaces hit_surface_records
#define chunk0 hit_chunk0
#define chunk1 hit_chunk1
#define chunk2 hit_chunk2
#define chunk3 hit_chunk3
#define PATH_TRACER_TABLE(s) hit_pt_materials.data[s]

// The material of the current path vertex, as its hit group evaluated it (or its D16 table entry).
NextGIPTMaterial path_tracer_material;
#define NEXTGI_PT_MATERIAL(m) path_tracer_material

#define PATH_TRACER_HIT_GROUPS
#include "nextgi_reference_pt_inc.glsl"
#include "path_tracer_inc.glsl"

void main() {
	uvec2 pixel = gl_LaunchIDEXT.xy;
	if (pixel.x >= ref_params.dispatch.x || pixel.y >= ref_params.dispatch.y) {
		return;
	}
	vec3 first_albedo;
	vec3 first_normal;
	float first_hit;
	vec3 radiance = path_tracer_sample_pixel(pixel, first_albedo, first_normal, first_hit);
	bool clear = ref_params.exposure_clear.y > 0.5;
	vec4 previous = clear ? vec4(0.0) : imageLoad(hit_pt_accumulation, ivec2(pixel));
	imageStore(hit_pt_accumulation, ivec2(pixel), vec4(previous.rgb + radiance, previous.a + 1.0));
	vec4 prev_albedo = clear ? vec4(0.0) : imageLoad(hit_pt_guide_albedo, ivec2(pixel));
	imageStore(hit_pt_guide_albedo, ivec2(pixel), vec4(prev_albedo.rgb + first_albedo, prev_albedo.a + first_hit));
	vec4 prev_normal = clear ? vec4(0.0) : imageLoad(hit_pt_guide_normal, ivec2(pixel));
	imageStore(hit_pt_guide_normal, ivec2(pixel), vec4(prev_normal.rgb + first_normal, prev_normal.a + first_hit));
}
