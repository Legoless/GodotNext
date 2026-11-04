#[compute]

#version 460

#VERSION_DEFINES

// NextGI diffuse denoiser (#53, D8). One 8x8 workgroup over the screen-probe grid.
// MODE_PROBE_SPATIAL is the 3x3 probe-space filter: depth, normal and hit-distance weights.
// The temporal pass (MODE_PROBE_TEMPORAL) and the per-pixel stabilizer (MODE_STABILIZE) land next.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D probe_radiance;
layout(rgba16f, set = 0, binding = 1) uniform image2D probe_radiance_out;
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 4) uniform sampler2D probe_hit_tex;

layout(push_constant, std430) uniform Params {
	mat4 inv_projection;
	ivec2 probe_size;
	ivec2 screen_size;
	float depth_threshold;
	float normal_power;
	uint probe_temporal_max_history;
	uint stabilizer_enabled;
}
params;

// Reverse-Z depth to view space, matching the engine's other RD compute passes.
vec3 nextgi_view_position(vec2 p_uv, float p_depth) {
	vec4 view = params.inv_projection * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return view.xyz / view.w;
}

#ifdef MODE_PROBE_SPATIAL

const ivec2 NEXTGI_DENOISE_KERNEL[9] = {
	ivec2(-1, -1), ivec2(0, -1), ivec2(1, -1),
	ivec2(-1, 0), ivec2(0, 0), ivec2(1, 0),
	ivec2(-1, 1), ivec2(0, 1), ivec2(1, 1)
};

void main() {
	ivec2 probe = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(probe, params.probe_size))) {
		return;
	}

	vec2 uv = (vec2(probe) + 0.5) / vec2(params.probe_size);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	vec3 normal = normalize(textureLod(normal_roughness_tex, uv, 0.0).xyz * 2.0 - 1.0);
	float hit = max(textureLod(probe_hit_tex, uv, 0.0).r, 1e-3);
	vec3 view_pos = nextgi_view_position(uv, depth);
	float probe_depth = max(abs(view_pos.z), 1e-4);

	vec4 sum = vec4(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 9; i++) {
		ivec2 q = probe + NEXTGI_DENOISE_KERNEL[i];
		if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, params.probe_size))) {
			continue;
		}

		vec2 q_uv = (vec2(q) + 0.5) / vec2(params.probe_size);
		float q_depth = textureLod(depth_tex, q_uv, 0.0).r;
		vec3 q_normal = normalize(textureLod(normal_roughness_tex, q_uv, 0.0).xyz * 2.0 - 1.0);
		float q_hit = max(textureLod(probe_hit_tex, q_uv, 0.0).r, 1e-3);
		vec3 q_pos = nextgi_view_position(q_uv, q_depth);

		float plane_distance = abs(dot(q_pos - view_pos, normal));
		float w_depth = exp(-plane_distance / (params.depth_threshold * probe_depth));
		float w_normal = pow(max(dot(normal, q_normal), 0.0), params.normal_power);
		float w_hit = exp(-abs(log(q_hit / hit)));
		float w = w_depth * w_normal * w_hit;

		sum += textureLod(probe_radiance, q_uv, 0.0) * w;
		weight_sum += w;
	}

	vec4 filtered = weight_sum > 0.0 ? sum / weight_sum : textureLod(probe_radiance, uv, 0.0);
	imageStore(probe_radiance_out, probe, filtered);
}

#endif // MODE_PROBE_SPATIAL
