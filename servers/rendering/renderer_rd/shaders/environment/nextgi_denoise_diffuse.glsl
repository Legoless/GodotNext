#[compute]

#version 460

#VERSION_DEFINES

// NextGI diffuse denoiser (#53, D8). One 8x8 workgroup over the gather's probe-texel grid.
//
// The gather stores 16 rays per 16x16 screen tile, four texels per axis, and the probe image's
// alpha carries the anchor's distance from the camera. The spatial and temporal modes walk the
// probe grid (one step per tile), keeping the ray index inside the tile, so a texel only ever
// mixes the same ray direction of neighboring probes.
//
// MODE_PROBE_SPATIAL: 3x3 probe-space filter with depth, normal and hit-distance weights.
// MODE_PROBE_TEMPORAL: per-probe reprojection with the prepass velocity, history rejection and a
// history cap, resetting inside the dirty regions.
// MODE_STABILIZE: per-pixel, non-accumulating stabilizer after the upsample.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_PROBE_SPATIAL

layout(set = 0, binding = 0) uniform sampler2D probe_radiance;
layout(rgba16f, set = 0, binding = 1) uniform image2D probe_radiance_out;
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_tex;

layout(push_constant, std430) uniform Params {
	mat4 inv_projection;
	ivec2 probe_size; // Ray-texel grid, four texels per axis per probe.
	ivec2 screen_size;
	float depth_threshold;
	float normal_power;
	uint probe_stride; // Ray texels per probe axis (4).
	uint probe_extent; // Screen pixels per probe axis (16).
}
params;

vec3 nextgi_denoise_view_position(vec2 p_uv, float p_depth) {
	vec4 view = params.inv_projection * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return view.xyz / view.w;
}

// Screen pixel whose guide describes a probe. Matches the gather's anchor for the same tile.
vec2 nextgi_denoise_probe_uv(ivec2 p_probe) {
	ivec2 anchor = min(p_probe * int(params.probe_extent) + int(params.probe_extent) / 2, params.screen_size - ivec2(1));
	return (vec2(anchor) + 0.5) / vec2(params.screen_size);
}

const ivec2 NEXTGI_DENOISE_KERNEL[9] = {
	ivec2(-1, -1), ivec2(0, -1), ivec2(1, -1),
	ivec2(-1, 0), ivec2(0, 0), ivec2(1, 0),
	ivec2(-1, 1), ivec2(0, 1), ivec2(1, 1)
};

void main() {
	ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(texel, params.probe_size))) {
		return;
	}

	const int stride = int(params.probe_stride);
	const ivec2 probe = texel / stride;
	const ivec2 ray = texel - probe * stride;
	const ivec2 probe_count = params.probe_size / stride;

	vec2 uv = nextgi_denoise_probe_uv(probe);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	vec3 normal = normalize(textureLod(normal_roughness_tex, uv, 0.0).xyz * 2.0 - 1.0);
	vec3 view_pos = nextgi_denoise_view_position(uv, depth);
	float probe_depth = max(abs(view_pos.z), 1e-4);

	vec4 center = textureLod(probe_radiance, (vec2(texel) + 0.5) / vec2(params.probe_size), 0.0);
	float hit = max(center.a, 1e-3);

	vec4 sum = vec4(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 9; i++) {
		ivec2 q_probe = probe + NEXTGI_DENOISE_KERNEL[i];
		if (any(lessThan(q_probe, ivec2(0))) || any(greaterThanEqual(q_probe, probe_count))) {
			continue;
		}

		ivec2 q_texel = q_probe * stride + ray;
		vec2 q_uv = nextgi_denoise_probe_uv(q_probe);
		float q_depth = textureLod(depth_tex, q_uv, 0.0).r;
		vec3 q_normal = normalize(textureLod(normal_roughness_tex, q_uv, 0.0).xyz * 2.0 - 1.0);
		vec3 q_pos = nextgi_denoise_view_position(q_uv, q_depth);
		vec4 q_radiance = textureLod(probe_radiance, (vec2(q_texel) + 0.5) / vec2(params.probe_size), 0.0);

		float plane_distance = abs(dot(q_pos - view_pos, normal));
		float w_depth = exp(-plane_distance / (params.depth_threshold * probe_depth));
		float w_normal = pow(max(dot(normal, q_normal), 0.0), params.normal_power);
		float w_hit = exp(-abs(log(max(q_radiance.a, 1e-3) / hit)));
		float w = w_depth * w_normal * w_hit;

		sum += q_radiance * w;
		weight_sum += w;
	}

	vec4 filtered = weight_sum > 0.0 ? sum / weight_sum : center;
	imageStore(probe_radiance_out, texel, filtered);
}

#elif defined(MODE_PROBE_TEMPORAL)

layout(set = 0, binding = 0) uniform sampler2D probe_radiance;
layout(rgba16f, set = 0, binding = 1) uniform image2D probe_radiance_out;
layout(set = 0, binding = 2) uniform sampler2D probe_history;
layout(set = 0, binding = 3) uniform sampler2D depth_tex;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 5) uniform sampler2D velocity_tex;
layout(set = 0, binding = 6, std430) restrict readonly buffer NextGIDenoiseResetMask {
	uint data[];
}
reset_mask;
layout(r8ui, set = 0, binding = 7) uniform uimage2D history_length_image;
layout(rgba16f, set = 0, binding = 9) uniform image2D debug_variance_image;
layout(rgba16f, set = 0, binding = 10) uniform image2D debug_history_image;
layout(rgba16f, set = 0, binding = 11) uniform image2D debug_rejected_image;

layout(set = 0, binding = 8, std430) restrict readonly buffer NextGIDenoiseTemporalParams {
	mat4 prev_view_proj;
	mat4 inv_projection;
	vec4 camera_pos;
	vec4 prev_camera_pos;
}
temporal;

layout(push_constant, std430) uniform Params {
	ivec2 probe_size;
	ivec2 screen_size;
	float depth_tolerance;
	float normal_tolerance;
	uint history_cap;
	uint velocity_enabled;
	uint probe_stride;
	uint probe_extent;
	uint debug_views;
}
params;

vec3 nextgi_denoise_view_position(vec2 p_uv, float p_depth) {
	vec4 view = temporal.inv_projection * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return view.xyz / view.w;
}

vec2 nextgi_denoise_probe_uv(ivec2 p_probe) {
	ivec2 anchor = min(p_probe * int(params.probe_extent) + int(params.probe_extent) / 2, params.screen_size - ivec2(1));
	return (vec2(anchor) + 0.5) / vec2(params.screen_size);
}

void main() {
	ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(texel, params.probe_size))) {
		return;
	}

	const int stride = int(params.probe_stride);
	const ivec2 probe = texel / stride;
	const ivec2 ray = texel - probe * stride;
	const ivec2 probe_count = params.probe_size / stride;
	const uint probe_index = uint(probe.y) * uint(probe_count.x) + uint(probe.x);

	vec2 uv = nextgi_denoise_probe_uv(probe);
	float depth = textureLod(depth_tex, uv, 0.0).r;
	vec3 normal = normalize(textureLod(normal_roughness_tex, uv, 0.0).xyz * 2.0 - 1.0);
	vec3 world = nextgi_denoise_view_position(uv, depth);
	float distance_to_camera = length(world - temporal.camera_pos.xyz);

	vec4 current = textureLod(probe_radiance, (vec2(texel) + 0.5) / vec2(params.probe_size), 0.0);

	// Reproject: dynamic texels follow the prepass velocity, static ones the previous view-projection.
	bool have_prev = false;
	ivec2 prev_probe = probe;
	ivec2 anchor = min(probe * int(params.probe_extent) + int(params.probe_extent) / 2, params.screen_size - ivec2(1));
	if (params.velocity_enabled != 0u && textureLod(normal_roughness_tex, uv, 0.0).a > 0.5) {
		vec2 motion = textureLod(velocity_tex, uv, 0.0).xy;
		prev_probe = ivec2((vec2(anchor) + motion * vec2(params.screen_size)) / float(params.probe_extent));
		have_prev = true;
	} else {
		vec4 prev_clip = temporal.prev_view_proj * vec4(world, 1.0);
		if (prev_clip.w != 0.0) {
			vec2 prev_ndc = prev_clip.xy / prev_clip.w;
			vec2 prev_pixel = (prev_ndc * 0.5 + 0.5) * vec2(params.screen_size);
			prev_probe = ivec2(prev_pixel / float(params.probe_extent));
			have_prev = true;
		}
	}

	bool reset = reset_mask.data[probe_index] != 0u;
	if (!have_prev || prev_probe.x < 0 || prev_probe.y < 0 || prev_probe.x >= probe_count.x || prev_probe.y >= probe_count.y) {
		reset = true;
		prev_probe = probe;
	}

	ivec2 prev_texel = prev_probe * stride + ray;
	vec4 history = textureLod(probe_history, (vec2(prev_texel) + 0.5) / vec2(params.probe_size), 0.0);

	// Depth rejection: the history's stored distance is from the camera that wrote it.
	float previous_distance = length(world - temporal.prev_camera_pos.xyz);
	if (history.a <= 0.0 || abs(previous_distance - history.a) > params.depth_tolerance * history.a) {
		reset = true;
	}

	// Normal rejection: compare the guide of the reprojected tile against this one.
	vec2 prev_uv = nextgi_denoise_probe_uv(prev_probe);
	vec3 prev_normal = normalize(textureLod(normal_roughness_tex, prev_uv, 0.0).xyz * 2.0 - 1.0);
	if (dot(normal, prev_normal) < params.normal_tolerance) {
		reset = true;
	}

	// The length is stored per probe, written by the first ray only. A static camera can read the
	// value it is writing in the same dispatch, which at worst advances the length by one.
	uint history_length = 0u;
	if (!reset) {
		history_length = min(imageLoad(history_length_image, prev_probe).r + 1u, params.history_cap);
	}

	// Clamp the history to the current 3x3 neighbourhood's mean +/- 1.5 sigma.
	vec3 mean = vec3(0.0);
	vec3 mean_sq = vec3(0.0);
	float count = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 q_probe = probe + ivec2(x, y);
			if (any(lessThan(q_probe, ivec2(0))) || any(greaterThanEqual(q_probe, probe_count))) {
				continue;
			}
			vec3 sample_rgb = textureLod(probe_radiance, (vec2(q_probe * stride + ray) + 0.5) / vec2(params.probe_size), 0.0).rgb;
			mean += sample_rgb;
			mean_sq += sample_rgb * sample_rgb;
			count += 1.0;
		}
	}
	vec3 out_rgb = current.rgb;
	vec3 debug_sigma = vec3(0.0);
	if (count > 0.0) {
		mean /= count;
		vec3 variance = max(mean_sq / count - mean * mean, vec3(0.0));
		debug_sigma = sqrt(variance);
		vec3 clamped = clamp(history.rgb, mean - 1.5 * debug_sigma, mean + 1.5 * debug_sigma);
		// With a history cap of 0 the pass is a pass-through, for the DLSS-RR bypass hook (#206).
		float weight = params.history_cap == 0u ? 1.0 : 1.0 / float(history_length + 1u);
		out_rgb = reset ? current.rgb : mix(clamped, current.rgb, weight);
	}

	// Optional debug views (#53): variance, history length and rejection. Values are written per
	// texel so the debug copy can show the full trace resolution.
	if (params.debug_views != 0u) {
		imageStore(debug_variance_image, texel, vec4(debug_sigma, 1.0));
		imageStore(debug_history_image, texel, vec4(float(history_length) / 255.0, 0.0, 0.0, 1.0));
		imageStore(debug_rejected_image, texel, vec4(reset ? 1.0 : 0.0, 0.0, 0.0, 1.0));
	}

	imageStore(probe_radiance_out, texel, vec4(out_rgb, distance_to_camera));
	if (ray.x == 0 && ray.y == 0) {
		imageStore(history_length_image, probe, uvec4(min(history_length, 255u)));
	}
}

#elif defined(MODE_STABILIZE)

layout(set = 0, binding = 0) uniform sampler2D current_tex;
layout(set = 0, binding = 1) uniform sampler2D previous_tex;
layout(rgba16f, set = 0, binding = 2) uniform image2D output_image;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	float blend_weight; // Fixed and at most 0.5: this pass never accumulates.
	uint stabilizer_enabled;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 current = textureLod(current_tex, uv, 0.0);
	vec4 result = current;
	if (params.stabilizer_enabled != 0u) {
		// Clamp the previous output to the current 3x3 min/max, then blend with a fixed weight.
		vec3 min_rgb = current.rgb;
		vec3 max_rgb = current.rgb;
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - ivec2(1));
				vec3 c = textureLod(current_tex, (vec2(q) + 0.5) / vec2(params.screen_size), 0.0).rgb;
				min_rgb = min(min_rgb, c);
				max_rgb = max(max_rgb, c);
			}
		}
		vec3 previous = clamp(textureLod(previous_tex, uv, 0.0).rgb, min_rgb, max_rgb);
		result = vec4(mix(previous, current.rgb, clamp(params.blend_weight, 0.0, 0.5)), current.a);
	}
	imageStore(output_image, pixel, result);
}

#endif // MODE_STABILIZE
