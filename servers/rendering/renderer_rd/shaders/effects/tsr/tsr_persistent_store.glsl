#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

// Clean-room TSR history resurrection (#198, docs/godotnext/tsr-sources.md): keeps the
// history just written as one persistent frame, at a fraction of its resolution.

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D history_color_curr_tex;
#ifdef MODE_HISTORY_RGBA16F
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D persistent_out;
#else
layout(r11f_g11f_b10f, set = 0, binding = 1) uniform restrict writeonly image2D persistent_out;
#endif

#define TSR_STORE_TAPS 4

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 store_size = imageSize(persistent_out);
	if (any(greaterThanEqual(px, store_size))) {
		return;
	}

	// Mean of a 4x4 grid of bilinear taps spread over the texel's footprint in the history,
	// weighted by 1 / (1 + luma) so a single bright sample does not dominate the frame.
	vec2 half_texel = 0.5 / vec2(params.history_size);
	vec3 sum = vec3(0.0);
	float w_sum = 0.0;
	for (int ty = 0; ty < TSR_STORE_TAPS; ty++) {
		for (int tx = 0; tx < TSR_STORE_TAPS; tx++) {
			vec2 uv = (vec2(px) + (vec2(float(tx), float(ty)) + 0.5) / float(TSR_STORE_TAPS)) / vec2(store_size);
			vec3 c = sanitize_hdr(textureLod(history_color_curr_tex, clamp(uv, half_texel, vec2(1.0) - half_texel), 0.0).rgb);
			float w = 1.0 / (1.0 + hdr_luma(c));
			sum += c * w;
			w_sum += w;
		}
	}

	imageStore(persistent_out, px, vec4(sum / max(w_sum, 1e-5), 1.0));
}
