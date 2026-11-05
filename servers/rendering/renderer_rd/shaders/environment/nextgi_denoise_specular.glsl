#[compute]

#version 450

// GodotNext: nextgi_denoise_specular (#54). Joint-bilateral upsample of the specular trace from
// its trace resolution into RB_TEX_REFLECTION at the GI buffer resolution.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D raw_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 3) uniform image2D output_image;

layout(push_constant, std430) uniform Params {
	ivec2 trace_size;
	ivec2 screen_size;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	if (depth <= 0.0) {
		imageStore(output_image, pixel, vec4(0.0));
		return;
	}
	vec3 normal = normalize(textureLod(normal_roughness_tex, uv, 0.0).xyz * 2.0 - 1.0);
	ivec2 base = ivec2(uv * vec2(params.trace_size) - 0.5);

	vec4 accum = vec4(0.0);
	float weight_sum = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 trace_pixel = clamp(base + ivec2(x, y), ivec2(0), params.trace_size - 1);
			vec4 sample_value = texelFetch(raw_texture, trace_pixel, 0);
			vec2 trace_uv = (vec2(trace_pixel) + 0.5) / vec2(params.trace_size);
			float sample_depth = textureLod(depth_tex, trace_uv, 0.0).r;
			vec3 sample_normal = normalize(textureLod(normal_roughness_tex, trace_uv, 0.0).xyz * 2.0 - 1.0);
			float depth_weight = 1.0 / (1.0 + abs(sample_depth - depth) * 100.0);
			float normal_weight = pow(max(dot(normal, sample_normal), 0.0), 8.0);
			float weight = depth_weight * normal_weight;
			accum += sample_value * weight;
			weight_sum += weight;
		}
	}
	imageStore(output_image, pixel, weight_sum > 0.0 ? accum / weight_sum : vec4(0.0));
}
