#[compute]

// GodotNext: path_tracer (#167). The product path tracer's inline backend (RendererRD::PathTracer): one sample
// per pixel into the view's RGBA32F history with inline ray query and the D16 table materials, where the #130
// hit groups do not run. MODE_COMPOSITE writes the history's mean into the HDR color.

#version 460
#extension GL_EXT_ray_query : require

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, set = 0, binding = 1) restrict uniform image2D accumulation;

#ifdef MODE_COMPOSITE

layout(rgba16f, set = 0, binding = 2) restrict writeonly uniform image2D color_image;

layout(push_constant, std430) uniform CompositeParams {
	uvec2 size;
	float exposure;
	float pad;
}
composite_params;

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= composite_params.size.x || pixel.y >= composite_params.size.y) {
		return;
	}
	vec4 acc = imageLoad(accumulation, ivec2(pixel));
	vec3 mean = acc.a > 0.0 ? acc.rgb / acc.a : vec3(0.0);
	imageStore(color_image, ivec2(pixel), vec4(mean * composite_params.exposure, 1.0));
}

#else

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

struct NextGIRefMaterial {
	vec4 albedo_metal;
	vec4 emission_roughness;
	vec4 specular_mode; // specular, mode, cull mode (RSE::CullMode), unused
};

layout(set = 0, binding = 3, std430) readonly buffer NextGIRefMaterials {
	NextGIRefMaterial data[];
}
materials;

layout(set = 0, binding = 4, std430) readonly buffer NextGIRefLights {
	vec4 data[];
}
lights;

layout(set = 0, binding = 5, std430) readonly buffer NextGIRefRecords {
	NextGIInstanceRecord data[];
}
records;

layout(set = 0, binding = 6, std430) readonly buffer NextGIRefSurfaces {
	NextGISurfaceRecord data[];
}
surfaces;

layout(set = 0, binding = 7, std430) readonly buffer NextGIRefChunk0 {
	uint data[];
}
chunk0;
layout(set = 0, binding = 8, std430) readonly buffer NextGIRefChunk1 {
	uint data[];
}
chunk1;
layout(set = 0, binding = 9, std430) readonly buffer NextGIRefChunk2 {
	uint data[];
}
chunk2;
layout(set = 0, binding = 10, std430) readonly buffer NextGIRefChunk3 {
	uint data[];
}
chunk3;

// PathTracerGPUParams (path_tracer.cpp): the D16 reference's params, then the product word, camera rays,
// and #169 sky, volumetric fog, legacy fog, thin-lens DOF, and motion blur parameters.
layout(set = 0, binding = 11, std430) readonly buffer PathTracerParams {
	vec4 cam_origin_fov;
	vec4 cam_forward_aspect;
	vec4 cam_up;
	vec4 cam_right;
	uvec4 dispatch; // width, height, sample_index, mode (0 full)
	uvec4 counts; // seed, light_count, surface_count, record_count
	vec4 sky_color;
	vec4 exposure_clear; // x exposure, y clear, z samples_per_pixel, w unused
	uvec4 product; // x max bounces, y flags, z firefly clamp (float bits), w fog volume count
	// The product's camera rays (PathTracerGPURays): the near-plane point at the view's center (w: 1 orthogonal) and
	// its change per normalized device x and y (w: the ray cone's width at its origin and its spread).
	vec4 ray_center;
	vec4 ray_dx;
	vec4 ray_dy;
	// GodotNext: path_tracer_lights_camera (#169).
	vec4 sky_params; // x radiance_scale, y border_pad, z border_scale, w unused
	vec4 sky_orient_row0; // xyz inverse sky orientation row 0, w bg_sky_enabled
	vec4 sky_orient_row1; // xyz inverse sky orientation row 1, w sky_cdf_dim
	vec4 sky_orient_row2; // xyz inverse sky orientation row 2, w sky_direct_scale
	vec4 vfog_params0; // x world_density, y anisotropy, z vfog_length, w vfog_sky_affect
	vec4 vfog_scattering; // rgb scattering albedo, w majorant_world
	vec4 vfog_emission; // rgb emission * energy, w fog_mode (0 off, 1 volumetric, 2 legacy)
	vec4 fog_legacy0; // rgb fog_light_color * energy, w fog_density
	vec4 fog_legacy1; // x fog_height, y fog_height_density, z fog_depth_begin, w fog_depth_end
	vec4 fog_legacy2; // x fog_depth_curve, y fog_sky_affect, z fog_depth_mode, w fog_sun_scatter
	vec4 dof_params; // x focus_distance, y aperture_radius, z near_plane_dist, w dof_enabled
	uvec4 motion_params; // x motion_slices (0 or 8), y shutter_bits, z sky_cdf_enabled, w pad
}
ref_params;

// GodotNext: path_tracer_denoise (#168). First-hit albedo and normal guides accumulated alongside radiance.
layout(rgba32f, set = 0, binding = 12) restrict uniform image2D guide_albedo;
layout(rgba32f, set = 0, binding = 13) restrict uniform image2D guide_normal;

// GodotNext: path_tracer_lights_camera (#169). Sky 2D CDF, FogVolumes, IES/area/projector atlases, sky octmap.
layout(set = 0, binding = 14, std430) readonly buffer PathTracerSkyCdf {
	float data[];
}
pt_sky_cdf;

layout(set = 0, binding = 15, std430) readonly buffer PathTracerFogVolumes {
	vec4 data[];
}
pt_fog_volumes;

layout(set = 0, binding = 16) uniform texture2DArray pt_ies_atlas;
layout(set = 0, binding = 17) uniform texture2D pt_area_atlas;
layout(set = 0, binding = 18) uniform texture2D pt_projector_atlas;
layout(set = 0, binding = 19) uniform texture2D pt_sky_octmap;
layout(set = 0, binding = 20) uniform sampler pt_linear_sampler;

#define PATH_TRACER_TABLE(s) materials.data[s]

#include "nextgi_reference_pt_inc.glsl"
#include "path_tracer_inc.glsl"

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= ref_params.dispatch.x || pixel.y >= ref_params.dispatch.y) {
		return;
	}
	vec3 first_albedo;
	vec3 first_normal;
	float first_hit;
	vec3 radiance = path_tracer_sample_pixel(pixel, first_albedo, first_normal, first_hit);
	bool clear = ref_params.exposure_clear.y > 0.5;
	vec4 previous = clear ? vec4(0.0) : imageLoad(accumulation, ivec2(pixel));
	imageStore(accumulation, ivec2(pixel), vec4(previous.rgb + radiance, previous.a + 1.0));
	vec4 prev_albedo = clear ? vec4(0.0) : imageLoad(guide_albedo, ivec2(pixel));
	imageStore(guide_albedo, ivec2(pixel), vec4(prev_albedo.rgb + first_albedo, prev_albedo.a + first_hit));
	vec4 prev_normal = clear ? vec4(0.0) : imageLoad(guide_normal, ivec2(pixel));
	imageStore(guide_normal, ivec2(pixel), vec4(prev_normal.rgb + first_normal, prev_normal.a + first_hit));
}

#endif
