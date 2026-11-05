#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform image2D target_image;

layout(push_constant, std430) uniform Params {
	vec4 color;
}
params;

void main() {
	imageStore(target_image, ivec2(gl_GlobalInvocationID.xy), params.color);
}
