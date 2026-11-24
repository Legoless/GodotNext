#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7, GM-6, budget M-8).
// Plain-compute (no ray-query dependency) sibling for the shared MegaLights froxel & D17 volume:
//   - MODE_SAMPLE_NO_RT: stochastic froxel/D17 light sampling with VSM coarse-page visibility (#215)
//     or unshadowed fallback on devices without hardware ray queries.
//   - MODE_RESAMPLE_D17: zero-ray trilinear projection from the shared fog-grid SH-L1 volume into
//     the D17 translucency volume (1/16 screen x 32 log slices), preventing double-sampling when
//     both volumetric fog and translucency are active.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#ifdef MODE_RESAMPLE_D17
#define ML_FROXEL_RESAMPLE_ONLY
#endif

#include "ml_froxel_inc.glsl"

#ifdef MODE_RESAMPLE_D17

layout(set = 0, binding = 1) uniform texture3D ml_src_sh_r;
layout(set = 0, binding = 2) uniform texture3D ml_src_sh_g;
layout(set = 0, binding = 3) uniform texture3D ml_src_sh_b;
layout(set = 0, binding = 4) uniform sampler ml_sampler_linear_clamp;

layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image3D ml_d17_out_r;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image3D ml_d17_out_g;
layout(rgba16f, set = 0, binding = 7) uniform restrict writeonly image3D ml_d17_out_b;

void main() {
	uvec3 d17_pos = gl_GlobalInvocationID.xyz;
	uvec3 d17_dims = params.d17_dims.xyz;
	if (d17_pos.x >= d17_dims.x || d17_pos.y >= d17_dims.y || d17_pos.z >= d17_dims.z) {
		return;
	}

	vec2 uv = (vec2(d17_pos.xy) + vec2(0.5)) / vec2(d17_dims.xy);
	float slice = (float(d17_pos.z) + 0.5) / float(d17_dims.z);
	float d17_near = params.d17_params.x;
	float d17_far = params.d17_params.y;
	float linear_depth = d17_near * pow(d17_far / d17_near, slice);

	// Map D17 (uv, linear_depth) directly into the shared volumetric-fog froxel grid UVW.
	float fog_end = max(params.fog_z_params.x, 1e-4);
	float detail_spread = max(params.fog_z_params.y, 1e-4);
	float z_ratio = clamp(linear_depth / fog_end, 0.0, 1.0);
	float fog_z = pow(z_ratio, 1.0 / detail_spread);
	vec3 fog_uvw = vec3(uv, fog_z);

	vec4 sh_r = textureLod(sampler3D(ml_src_sh_r, ml_sampler_linear_clamp), fog_uvw, 0.0);
	vec4 sh_g = textureLod(sampler3D(ml_src_sh_g, ml_sampler_linear_clamp), fog_uvw, 0.0);
	vec4 sh_b = textureLod(sampler3D(ml_src_sh_b, ml_sampler_linear_clamp), fog_uvw, 0.0);

	imageStore(ml_d17_out_r, ivec3(d17_pos), sh_r);
	imageStore(ml_d17_out_g, ivec3(d17_pos), sh_g);
	imageStore(ml_d17_out_b, ivec3(d17_pos), sh_b);
}

#else // MODE_SAMPLE_NO_RT

float ml_froxel_trace_visibility(vec3 p_world_pos, float p_linear_depth, uint p_light_type, MLFroxelLightEval p_ev, bool p_use_vsm, bool p_force_visible) {
	if (p_force_visible || p_ev.shadow_opacity <= 0.001) {
		return 1.0;
	}
	uvec4 policy = ml_light_shadow_policies.data[p_ev.policy_index];
	if ((policy.z & ML_POLICY_FLAG_CASTS_SHADOW) == 0u) {
		return 1.0;
	}
	if (p_use_vsm && params.screen_frame_vsm.w > 0u && (p_ev.vsm_flags & VSM_LIGHT_FLAGS_SAMPLE) != 0u) {
		float inv_fade = max(params.fog_z_params.w, 1.0);
		if (p_light_type == ML_LIGHT_TYPE_DIRECTIONAL) {
			return vsm_fog_shadow_coarse_directional(params.screen_frame_vsm.w, p_ev.vsm_map_base, p_ev.vsm_flags, p_world_pos, p_linear_depth, inv_fade);
		}
		return vsm_fog_shadow_coarse_local(params.screen_frame_vsm.w, p_ev.vsm_map_base, p_ev.vsm_flags, p_world_pos, inv_fade);
	}
	return 1.0;
}

void main() {
	ml_froxel_sample_main();
}

#endif // MODE_RESAMPLE_D17
