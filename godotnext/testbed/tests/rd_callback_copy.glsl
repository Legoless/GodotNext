#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(rgba16f, set = 0, binding = 1) uniform image2D dest_image;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	imageStore(dest_image, pixel, texture(source_texture, vec2(pixel) + 0.5));
}
