#[compute]

// Reference path tracer scene path (#27). One sample per pixel. The estimator is
// nextgi_reference_pt_inc.glsl. MODE_COMPOSITE writes the mean into the HDR color.

#version 460
#extension GL_EXT_ray_query : require

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

layout(rgba32f, set = 0, binding = 1) restrict uniform image2D accumulation;
layout(rgba16f, set = 0, binding = 2) restrict uniform image2D color_image;
// GodotNext: nextgi_reference_reflections (#60 step 6). Guides for the reflection-only mode:
// rgb = distance sum, a unused; the composite writes mean distance and hit fraction.
layout(rgba32f, set = 0, binding = 12) restrict uniform image2D reflection_guides;
layout(rgba16f, set = 0, binding = 13) restrict uniform image2D reflection_distance;

struct NextGIRefMaterial {
	vec4 albedo_metal;
	vec4 emission_roughness;
	vec4 specular_mode;
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

layout(set = 0, binding = 11, std430) readonly buffer NextGIRefParamsBuffer {
	vec4 cam_origin_fov;
	vec4 cam_forward_aspect;
	vec4 cam_up;
	vec4 cam_right;
	uvec4 dispatch; // width, height, sample_index, direct_only
	uvec4 counts; // seed, light_count, surface_count, record_count
	vec4 sky_color;
	vec4 exposure_clear; // x exposure, y clear
}
ref_params;

#include "nextgi_reference_pt_inc.glsl"

vec3 nextgi_ref_camera_dir(uvec2 pixel) {
	vec2 uv = (vec2(pixel) + 0.5) / vec2(ref_params.dispatch.xy);
	vec2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;
	float tan_half = tan(ref_params.cam_origin_fov.w * 0.5);
	float aspect = ref_params.cam_forward_aspect.w;
	return normalize(ref_params.cam_right.xyz * (ndc.x * tan_half * aspect) + ref_params.cam_up.xyz * (ndc.y * tan_half) + ref_params.cam_forward_aspect.xyz);
}

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= ref_params.dispatch.x || pixel.y >= ref_params.dispatch.y) {
		return;
	}
#ifdef MODE_COMPOSITE
	vec4 acc = imageLoad(accumulation, ivec2(pixel));
	vec3 mean = acc.a > 0.0 ? acc.rgb / acc.a : vec3(0.0);
	imageStore(color_image, ivec2(pixel), vec4(mean * ref_params.exposure_clear.x, 1.0));
	vec4 guide = imageLoad(reflection_guides, ivec2(pixel));
	const float guide_samples = guide.z;
	imageStore(reflection_distance, ivec2(pixel), vec4(guide_samples > 0.0 ? guide.x / guide_samples : 0.0, guide_samples > 0.0 ? guide.y / guide_samples : 0.0, 0.0, 1.0));
#else
	uint index = pixel.y * ref_params.dispatch.x + pixel.x;
	vec3 origin = ref_params.cam_origin_fov.xyz;
	vec3 direction = nextgi_ref_camera_dir(pixel);
	uint spp = max(uint(ref_params.exposure_clear.z), 1u);
	vec3 sample_sum = vec3(0.0);
	vec3 guide_sum = vec3(0.0);
	for (uint s = 0u; s < spp; s++) {
		uint rng = nextgi_pt_hash(index ^ ref_params.counts.x ^ (ref_params.dispatch.z + s));
		vec3 sample_radiance = ref_params.sky_color.rgb;
		vec3 guide_sample = vec3(0.0);
		NextGIHit hit;
		if (nextgi_trace(origin, direction, 1.0e6, hit)) {
			vec3 position;
			vec3 normal;
			uint material;
			if (nextgi_pt_hit_surface(hit, position, normal, material)) {
				if (ref_params.dispatch.w == 2u) {
					float hit_distance = 0.0;
					float hit_fraction = 0.0;
					sample_radiance = nextgi_pt_reflection(position, normal, -direction, material, rng, hit_distance, hit_fraction);
					guide_sample = vec3(hit_distance, hit_fraction, 1.0);
				} else {
					sample_radiance = nextgi_pt_radiance_at(position, normal, -direction, material, rng);
				}
			}
		}
		sample_sum += sample_radiance;
		guide_sum += guide_sample;
	}
	vec4 previous = ref_params.exposure_clear.y > 0.5 ? vec4(0.0) : imageLoad(accumulation, ivec2(pixel));
	imageStore(accumulation, ivec2(pixel), vec4(previous.rgb + sample_sum, previous.a + float(spp)));
	vec4 previous_guides = ref_params.exposure_clear.y > 0.5 ? vec4(0.0) : imageLoad(reflection_guides, ivec2(pixel));
	imageStore(reflection_guides, ivec2(pixel), previous_guides + vec4(guide_sum, 0.0));
#endif
}
