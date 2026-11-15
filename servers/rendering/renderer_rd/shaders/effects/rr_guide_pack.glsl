#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: rr_guides (#205). Packs the DLSS-RR normal and roughness guide from the
// prepass normal-roughness buffer: the world-space unit normal in rgb and the roughness
// in a. Pixels where the opaque passes drew no surface (the diffuse albedo guide's alpha
// is 0: the background, and transparent surfaces the depth prepass drew) are written as 0.
// RR_DECODE_NORMAL() and RR_DECODE_ROUGHNESS() come from RRGuides::glsl_pack_prelude().

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 1) uniform sampler2D diffuse_albedo;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D dest_normal_roughness;

layout(push_constant, std430) uniform Params {
	// The camera basis, one column per vec4.
	vec4 view_to_world[3];
	ivec2 size;
	uint pad[2];
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	// The opaque passes write alpha 1 to the albedo guides; the clear leaves alpha 0.
	if (!(texelFetch(diffuse_albedo, pos, 0).a > 0.5)) {
		imageStore(dest_normal_roughness, pos, vec4(0.0));
		return;
	}

	vec4 normal_roughness = texelFetch(source_normal_roughness, pos, 0);

	vec3 normal = mat3(params.view_to_world[0].xyz, params.view_to_world[1].xyz, params.view_to_world[2].xyz) * RR_DECODE_NORMAL(normal_roughness.xyz);
	float length_squared = dot(normal, normal);
	normal = length_squared > 1e-12 ? normal * inversesqrt(length_squared) : vec3(0.0);

	imageStore(dest_normal_roughness, pos, vec4(normal, RR_DECODE_ROUGHNESS(normal_roughness.w)));
}
