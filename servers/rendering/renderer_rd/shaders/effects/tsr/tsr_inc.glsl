// Clean-room TSR common helpers and push constants (docs/godotnext/tsr-sources.md).

#define TSR_GROUP_SIZE 8

#define TSR_FLAG_RESET (1u << 0)
#define TSR_FLAG_ORTHOGONAL (1u << 1)
#define TSR_FLAG_REJECTION_AA (1u << 2)

#define TSR_MAX_HDR 64000.0
#define TSR_SKY_DEPTH 65000.0

layout(push_constant, std430) uniform Params {
	highp mat4 reprojection;
	ivec2 internal_size;
	ivec2 history_size;
	ivec2 output_size;
	vec2 jitter_pixels;
	float z_near;
	float z_far;
	uint flags;
	float history_scale;
}
params;

vec3 sanitize_hdr(vec3 c) {
	bvec3 bad = bvec3(isnan(c.x) || isinf(c.x), isnan(c.y) || isinf(c.y), isnan(c.z) || isinf(c.z));
	vec3 safe = mix(c, vec3(0.0), bad);
	return clamp(safe, vec3(0.0), vec3(TSR_MAX_HDR));
}

vec2 sanitize_vec2(vec2 v) {
	bvec2 bad = bvec2(isnan(v.x) || isinf(v.x), isnan(v.y) || isinf(v.y));
	return mix(v, vec2(0.0), bad);
}

// Godot RD reverse-Z: raw_d = 1.0 at z_near, 0.0 at z_far / sky.
float linearize_depth(float raw_d) {
	if (raw_d <= 1e-7) {
		return TSR_SKY_DEPTH;
	}
	if ((params.flags & TSR_FLAG_ORTHOGONAL) != 0u) {
		float z_ortho = params.z_near + (1.0 - clamp(raw_d, 0.0, 1.0)) * (params.z_far - params.z_near);
		return clamp(z_ortho, 0.0, TSR_MAX_HDR);
	}
	float denom = max(params.z_near + clamp(raw_d, 0.0, 1.0) * (params.z_far - params.z_near), 1e-6);
	return clamp((params.z_near * params.z_far) / denom, 0.0, TSR_MAX_HDR);
}

float hdr_luma(vec3 rgb) {
	return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

// Karis-compressed YCoCg: maps linear HDR RGB into bounded perceptual [Y, Co, Cg]
// so neighborhood statistics and variance clipping remain stable on bright highlights.
vec3 rgb_to_perceptual_ycocg(vec3 rgb) {
	vec3 c = sanitize_hdr(rgb);
	float y = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
	float co = 0.5 * c.r - 0.5 * c.b;
	float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
	float w = 1.0 / (1.0 + max(y, 0.0));
	return vec3(y, co, cg) * w;
}

vec3 perceptual_ycocg_to_rgb(vec3 ycocg) {
	float y_p = clamp(ycocg.x, 0.0, 0.999984);
	float inv_w = 1.0 / max(1.0 - y_p, 1.0 / (1.0 + TSR_MAX_HDR));
	float y = y_p * inv_w;
	float co = ycocg.y * inv_w;
	float cg = ycocg.z * inv_w;
	float tmp = y - cg;
	vec3 rgb = vec3(tmp + co, y + cg, tmp - co);
	return sanitize_hdr(rgb);
}

vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 history_sample) {
	vec3 center = 0.5 * (aabb_max + aabb_min);
	vec3 extents = 0.5 * (aabb_max - aabb_min) + vec3(1e-5);
	vec3 offset = history_sample - center;
	vec3 ts = abs(offset / extents);
	float max_t = max(ts.x, max(ts.y, ts.z));
	if (max_t > 1.0) {
		return center + offset / max_t;
	}
	return history_sample;
}

// 5-tap Catmull-Rom bicubic sampler (corners omitted) clamped to local tap bounds
// to prevent negative lobes / ringing on high-contrast HDR edges.
vec3 sample_catmull_rom_5tap(sampler2D tex, vec2 uv, vec2 tex_size) {
	vec2 sample_pos = uv * tex_size;
	vec2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
	vec2 f = sample_pos - tex_pos1;

	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);

	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / max(w12, vec2(1e-5));

	vec2 inv_size = 1.0 / tex_size;
	vec2 tc0 = (tex_pos1 - 1.0) * inv_size;
	vec2 tc3 = (tex_pos1 + 2.0) * inv_size;
	vec2 tc12 = (tex_pos1 + offset12) * inv_size;

	vec3 s_top = sanitize_hdr(textureLod(tex, vec2(tc12.x, tc0.y), 0.0).rgb);
	vec3 s_left = sanitize_hdr(textureLod(tex, vec2(tc0.x, tc12.y), 0.0).rgb);
	vec3 s_center = sanitize_hdr(textureLod(tex, vec2(tc12.x, tc12.y), 0.0).rgb);
	vec3 s_right = sanitize_hdr(textureLod(tex, vec2(tc3.x, tc12.y), 0.0).rgb);
	vec3 s_bottom = sanitize_hdr(textureLod(tex, vec2(tc12.x, tc3.y), 0.0).rgb);

	float cw_top = w12.x * w0.y;
	float cw_left = w0.x * w12.y;
	float cw_center = w12.x * w12.y;
	float cw_right = w3.x * w12.y;
	float cw_bottom = w12.x * w3.y;

	float total_w = cw_top + cw_left + cw_center + cw_right + cw_bottom;
	vec3 result = (s_top * cw_top + s_left * cw_left + s_center * cw_center + s_right * cw_right + s_bottom * cw_bottom) / max(total_w, 1e-5);

	vec3 tap_min = min(s_center, min(min(s_top, s_bottom), min(s_left, s_right)));
	vec3 tap_max = max(s_center, max(max(s_top, s_bottom), max(s_left, s_right)));
	return clamp(result, tap_min, tap_max);
}
