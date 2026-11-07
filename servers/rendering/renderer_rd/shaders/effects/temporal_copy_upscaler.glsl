#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(rgba16f, set = 0, binding = 1) uniform writeonly image2D destination_color;

layout(push_constant, std430) uniform Params {
	uvec2 resolution;
	uvec2 pad;
}
params;

void main() {
	if (any(greaterThanEqual(gl_GlobalInvocationID.xy, params.resolution))) {
		return;
	}
	vec2 uv = (vec2(gl_GlobalInvocationID.xy) + 0.5) / vec2(params.resolution);
	imageStore(destination_color, ivec2(gl_GlobalInvocationID.xy), textureLod(source_color, uv, 0.0));
}
