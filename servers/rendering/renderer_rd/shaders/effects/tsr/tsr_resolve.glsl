#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D resolved_history_tex;
#if defined(MODE_OUTPUT_RGBA16F)
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D upscaled_out;
#elif defined(MODE_OUTPUT_RGB10A2)
layout(rgb10_a2, set = 0, binding = 1) uniform restrict writeonly image2D upscaled_out;
#else
layout(r11f_g11f_b10f, set = 0, binding = 1) uniform restrict writeonly image2D upscaled_out;
#endif

void main() {
	ivec2 opx = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(opx, params.output_size))) {
		return;
	}

	if (params.history_size == params.output_size) {
		vec3 col = sanitize_hdr(texelFetch(resolved_history_tex, opx, 0).rgb);
		imageStore(upscaled_out, opx, vec4(col, 1.0));
		return;
	}

	if (params.history_size == params.output_size * 2) {
		// Exact 200% history -> 100% output Karis-weighted 2x2 downsample.
		ivec2 base_h = opx * 2;
		vec3 c00 = sanitize_hdr(texelFetch(resolved_history_tex, base_h + ivec2(0, 0), 0).rgb);
		vec3 c10 = sanitize_hdr(texelFetch(resolved_history_tex, base_h + ivec2(1, 0), 0).rgb);
		vec3 c01 = sanitize_hdr(texelFetch(resolved_history_tex, base_h + ivec2(0, 1), 0).rgb);
		vec3 c11 = sanitize_hdr(texelFetch(resolved_history_tex, base_h + ivec2(1, 1), 0).rgb);

		float w00 = 1.0 / (1.0 + hdr_luma(c00));
		float w10 = 1.0 / (1.0 + hdr_luma(c10));
		float w01 = 1.0 / (1.0 + hdr_luma(c01));
		float w11 = 1.0 / (1.0 + hdr_luma(c11));

		vec3 col = sanitize_hdr((c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11) / max(w00 + w10 + w01 + w11, 1e-5));
		imageStore(upscaled_out, opx, vec4(col, 1.0));
		return;
	}

	// Fallback for clamped non-integer history ratios (e.g. if 2x target exceeded 16384).
	vec2 inv_out = 1.0 / vec2(params.output_size);
	vec2 uv_center = (vec2(opx) + 0.5) * inv_out;
	vec2 quarter_out = 0.25 * inv_out;

	vec3 s0 = sanitize_hdr(textureLod(resolved_history_tex, uv_center + vec2(-quarter_out.x, -quarter_out.y), 0.0).rgb);
	vec3 s1 = sanitize_hdr(textureLod(resolved_history_tex, uv_center + vec2(quarter_out.x, -quarter_out.y), 0.0).rgb);
	vec3 s2 = sanitize_hdr(textureLod(resolved_history_tex, uv_center + vec2(-quarter_out.x, quarter_out.y), 0.0).rgb);
	vec3 s3 = sanitize_hdr(textureLod(resolved_history_tex, uv_center + vec2(quarter_out.x, quarter_out.y), 0.0).rgb);

	float ws0 = 1.0 / (1.0 + hdr_luma(s0));
	float ws1 = 1.0 / (1.0 + hdr_luma(s1));
	float ws2 = 1.0 / (1.0 + hdr_luma(s2));
	float ws3 = 1.0 / (1.0 + hdr_luma(s3));

	vec3 out_col = sanitize_hdr((s0 * ws0 + s1 * ws1 + s2 * ws2 + s3 * ws3) / max(ws0 + ws1 + ws2 + ws3, 1e-5));
	imageStore(upscaled_out, opx, vec4(out_col, 1.0));
}
