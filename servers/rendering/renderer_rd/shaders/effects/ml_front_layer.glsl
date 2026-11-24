#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7).
// High-quality front-layer translucency guide preparation pass (plain compute, no ray query).
// Synthesizes neutral demodulated M1 inputs (albedo + material) from the transparent front-layer
// depth and normal-roughness targets (#61) so the frontmost transparent layer reuses the exact
// MegaLights sampling, visibility, shading, and denoising kernels with a separate temporal history.
// Pixels without a front-layer transparent surface are marked ML_MODEL_FALLBACK (3) so all
// downstream MegaLights kernels skip them automatically.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform MLFrontLayerPush {
	uvec2 size;
	uint pad0;
	uint pad1;
}
push_const;

layout(set = 0, binding = 0) uniform texture2D fl_depth_tex;
layout(set = 0, binding = 1) uniform texture2D fl_normal_roughness_tex;
layout(set = 0, binding = 2) uniform sampler fl_sampler_nearest;

layout(rgba8, set = 0, binding = 3) uniform restrict writeonly image2D fl_albedo_out;
layout(rgba8, set = 0, binding = 4) uniform restrict writeonly image2D fl_material_out;

void main() {
	uvec2 px = gl_GlobalInvocationID.xy;
	if (px.x >= push_const.size.x || px.y >= push_const.size.y) {
		return;
	}

	float depth = texelFetch(sampler2D(fl_depth_tex, fl_sampler_nearest), ivec2(px), 0).r;
	// Neutral chroma (Cb=0, Cr=0 -> 7*16 + 7 = 119 / 255) for dielectric F0 = 0.04.
	const float neutral_tint = 119.0 / 255.0;
	if (depth > 0.0) {
		// Unit diffuse albedo (demodulated irradiance) + direct_ao = 1.0.
		imageStore(fl_albedo_out, ivec2(px), vec4(1.0, 1.0, 1.0, 1.0));
		// luma = 0.04 (neutral dielectric glass), neutral tint, model = ML_MODEL_STANDARD (0),
		// layer_group = 255 (1.0 -> matches all light layers via 0xFFFFFFFF fallback).
		imageStore(fl_material_out, ivec2(px), vec4(0.04, neutral_tint, 0.0, 1.0));
	} else {
		imageStore(fl_albedo_out, ivec2(px), vec4(0.0, 0.0, 0.0, 0.0));
		// model = ML_MODEL_FALLBACK (3) -> skipped by generate_samples, shade, and denoise.
		imageStore(fl_material_out, ivec2(px), vec4(0.0, neutral_tint, 3.0 / 255.0, 1.0));
	}
}
