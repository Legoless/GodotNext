#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgi_denoise_specular (#54). SVGF/ReLAX-class specular filter:
// MODE_TEMPORAL reprojects with the prepass velocity or the previous view-projection (virtual
// motion), clamps the history to the current 3x3 mean +- 1.5 sigma and caps its length by
// roughness; MODE_SPATIAL is one a-trous pass (3x3, edge-stopping); the default variant is the
// joint-bilateral upsample into RB_TEX_REFLECTION.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// GodotNext: nextgi_view_math. One reconstruction for every NextGI pass.
#include "nextgi_view_inc.glsl"

#if defined(MODE_TEMPORAL)

layout(set = 0, binding = 0) uniform sampler2D raw_texture;
layout(set = 0, binding = 1) uniform sampler2D history_texture;
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 4) uniform sampler2D velocity_tex;
layout(set = 0, binding = 5) uniform sampler2D hit_distance_tex;
layout(rgba16f, set = 0, binding = 6) uniform image2D output_image;
layout(rgba16f, set = 0, binding = 7) uniform image2D history_length_image;
layout(rgba16f, set = 0, binding = 9) uniform image2D debug_variance_image;
layout(rgba16f, set = 0, binding = 10) uniform image2D debug_rejected_image;
layout(set = 0, binding = 8, std430) readonly buffer NextGISpecularParams {
	mat4 prev_view_proj;
	mat4 inv_view_proj;
	vec4 camera_pos;
	vec4 prev_camera_pos;
}
params;

layout(push_constant, std430) uniform Push {
	ivec2 trace_size;
	ivec2 screen_size;
	float depth_tolerance;
	float normal_tolerance;
	uint velocity_enabled;
	uint history_cap_min;
	uint debug_views;
	uint pad;
}
push;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= push.trace_size.x || pixel.y >= push.trace_size.y) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(push.trace_size);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	if (depth <= 0.0) {
		imageStore(output_image, pixel, vec4(0.0));
		imageStore(history_length_image, pixel, vec4(0.0));
		return;
	}
	vec4 guide = textureLod(normal_roughness_tex, uv, 0.0);
	float roughness = guide.a;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);

	// World position, then reproject: velocity for dynamic instances, the previous view-projection
	// for static geometry (virtual motion at the hit distance comes from the depth).
	vec3 world = nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
	vec2 prev_uv = uv;
	if (push.velocity_enabled != 0u && guide.a > 0.5) {
		// Dynamic instances follow the prepass velocity; smooth surfaces blend toward the virtual
		// motion of the hit point, which the depth and hit distance define.
		vec2 motion = textureLod(velocity_tex, uv, 0.0).xy;
		vec2 surface_uv = uv - motion;
		const float hit_distance = textureLod(hit_distance_tex, uv, 0.0).r;
		vec3 view_dir = normalize(world - params.camera_pos.xyz);
		vec3 virtual_uv_depth = nextgi_view_project(params.prev_view_proj, world + view_dir * hit_distance);
		vec2 virtual_uv = virtual_uv_depth.z >= 0.0 ? virtual_uv_depth.xy : surface_uv;
		prev_uv = mix(surface_uv, virtual_uv, smoothstep(0.4, 0.0, roughness));
	} else {
		vec3 prev_uv_depth = nextgi_view_project(params.prev_view_proj, world);
		if (prev_uv_depth.z >= 0.0) {
			prev_uv = prev_uv_depth.xy;
		}
	}
	const bool valid = all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));

	vec4 current = texelFetch(raw_texture, pixel, 0);
	vec4 history = valid ? textureLod(history_texture, prev_uv, 0.0) : vec4(0.0);
	ivec2 prev_pixel = clamp(ivec2(prev_uv * vec2(push.trace_size)), ivec2(0), push.trace_size - 1);
	uint history_length = 0u;
	if (valid) {
		history_length = uint(clamp(imageLoad(history_length_image, prev_pixel).r * 255.0, 0.0, 255.0));
	}

	// Clamp the history to the current 3x3 neighbourhood's mean +- 1.5 sigma.
	vec3 mean = vec3(0.0);
	vec3 mean_sq = vec3(0.0);
	float count = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 p = clamp(pixel + ivec2(x, y), ivec2(0), push.trace_size - 1);
			vec3 c = texelFetch(raw_texture, p, 0).rgb;
			mean += c;
			mean_sq += c * c;
			count += 1.0;
		}
	}
	mean /= count;
	vec3 variance = max(mean_sq / count - mean * mean, vec3(0.0));
	vec3 sigma = sqrt(variance);
	vec3 clamped = clamp(history.rgb, mean - 1.5 * sigma, mean + 1.5 * sigma);

	// History caps: 8 frames at roughness 0 to 32 at 0.4, interpolated.
	const uint cap_max = uint(mix(8.0, 32.0, clamp(roughness / 0.4, 0.0, 1.0)));
	history_length = valid ? min(history_length + 1u, max(cap_max, push.history_cap_min)) : 1u;
	const float blend = 1.0 / float(max(history_length, 1u));
	vec3 result = mix(clamped, current.rgb, blend);
	if (push.debug_views != 0u) {
		imageStore(debug_variance_image, pixel, vec4(sigma, 1.0));
		imageStore(debug_rejected_image, pixel, vec4(valid ? 0.0 : 1.0, 0.0, 0.0, 1.0));
	}
	imageStore(output_image, pixel, vec4(result, current.a));
	imageStore(history_length_image, pixel, vec4(float(min(history_length, 255u)) / 255.0, 0.0, 0.0, 1.0));
}

#elif defined(MODE_SPATIAL)

layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 3) uniform image2D output_image;

layout(push_constant, std430) uniform Push {
	ivec2 trace_size;
	ivec2 screen_size;
	int step;
	float normal_power;
}
push;

float nextgi_luma(vec3 p_color) {
	return dot(p_color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= push.trace_size.x || pixel.y >= push.trace_size.y) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(push.trace_size);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	vec3 normal = normalize(textureLod(normal_roughness_tex, uv, 0.0).xyz * 2.0 - 1.0);
	vec4 center = texelFetch(source_texture, pixel, 0);
	float center_luma = nextgi_luma(center.rgb);

	vec3 accum = center.rgb;
	float weight_sum = 1.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 p = pixel + ivec2(x, y) * push.step;
			if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, push.trace_size))) {
				continue;
			}
			vec4 sample_value = texelFetch(source_texture, p, 0);
			vec2 sample_uv = (vec2(p) + 0.5) / vec2(push.trace_size);
			float sample_depth = textureLod(depth_tex, sample_uv, 0.0).r;
			vec3 sample_normal = normalize(textureLod(normal_roughness_tex, sample_uv, 0.0).xyz * 2.0 - 1.0);
			float depth_weight = 1.0 / (1.0 + abs(sample_depth - depth) * 100.0);
			float normal_weight = pow(max(dot(normal, sample_normal), 0.0), push.normal_power);
			float luma_weight = exp(-abs(nextgi_luma(sample_value.rgb) - center_luma) * 4.0);
			float weight = depth_weight * normal_weight * luma_weight;
			accum += sample_value.rgb * weight;
			weight_sum += weight;
		}
	}
	imageStore(output_image, pixel, vec4(accum / weight_sum, center.a));
}

#else

layout(set = 0, binding = 0) uniform sampler2D raw_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 3) uniform image2D output_image;

layout(push_constant, std430) uniform Push {
	ivec2 trace_size;
	ivec2 screen_size;
}
push;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= push.screen_size.x || pixel.y >= push.screen_size.y) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(push.screen_size);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	if (depth <= 0.0) {
		imageStore(output_image, pixel, vec4(0.0));
		return;
	}
	vec3 normal = normalize(textureLod(normal_roughness_tex, uv, 0.0).xyz * 2.0 - 1.0);
	ivec2 base = ivec2(uv * vec2(push.trace_size) - 0.5);

	vec4 accum = vec4(0.0);
	float weight_sum = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 trace_pixel = clamp(base + ivec2(x, y), ivec2(0), push.trace_size - 1);
			vec4 sample_value = texelFetch(raw_texture, trace_pixel, 0);
			vec2 trace_uv = (vec2(trace_pixel) + 0.5) / vec2(push.trace_size);
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

#endif
