#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8, GM-11, M-9).
// Compute shader template that bakes one square tile of a `shader_type light_function;`
// shader into the Light Function Atlas (R8_UNORM default, or RGBA8_UNORM).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define M_PI 3.14159265359

#include "samplers_inc.glsl"

layout(set = 0, binding = 0, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

#ifdef LIGHT_FUNCTION_RGBA8
layout(rgba8, set = 1, binding = 0) uniform restrict writeonly image2D atlas_image;
#else
layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D atlas_image;
#endif

layout(push_constant, std430) uniform Params {
	uvec2 tile_origin;
	uint tile_size;
	float time;
}
params;

#ifdef MATERIAL_UNIFORMS_USED
/* clang-format off */
layout(set = 2, binding = 0, std140) uniform MaterialUniforms {
#MATERIAL_UNIFORMS
} material;
/* clang-format on */
#endif

#GLOBALS

void main() {
	uvec2 local_px = gl_GlobalInvocationID.xy;
	if (local_px.x >= params.tile_size || local_px.y >= params.tile_size) {
		return;
	}

	// Map texel centers [0 .. tile_size - 1] to exact [0, 1] endpoints so the half-texel inset
	// lookup in light_function_inc.glsl reproduces exact boundary values without tile bleeding.
	vec2 light_uv = params.tile_size > 1u ? (vec2(local_px) / float(params.tile_size - 1u)) : vec2(0.5);
	vec3 light_mask = vec3(1.0);

	{
#CODE : LIGHT_FUNCTION
	}

	light_mask = clamp(light_mask, vec3(0.0), vec3(1.0));
	ivec2 dst = ivec2(params.tile_origin + local_px);
#ifdef LIGHT_FUNCTION_RGBA8
	imageStore(atlas_image, dst, vec4(light_mask, 1.0));
#else
	float lum = dot(light_mask, vec3(0.2126, 0.7152, 0.0722));
	imageStore(atlas_image, dst, vec4(lum, 0.0, 0.0, 1.0));
#endif
}
