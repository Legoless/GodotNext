#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7, GM-6, budget M-8).
// Shared MegaLights froxel & D17 translucency volume pass (HWRT inline ray query + VSM coarse pages).
// Draws one stochastic sample set per froxel per frame (quality-preset configurable), traces visibility
// once via trace_shadow() (#145) or VSM coarse pages (#215) for VIRTUAL lights, and feeds BOTH the
// volumetric-fog in-scattering light volume AND the D17 translucency direct SH-L1 channel.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "ml_froxel_inc.glsl"

layout(set = 0, binding = 28) uniform accelerationStructureEXT ml_shadow_tlas;
#define NEXTGI_SHADOW_TLAS ml_shadow_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 29
#define NEXTGI_SHADOW_ALPHA_BINDING 30
#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_CHUNK0_BINDING 31
#define NEXTGI_SHADOW_CHUNK1_BINDING 32
#define NEXTGI_SHADOW_CHUNK2_BINDING 33
#define NEXTGI_SHADOW_CHUNK3_BINDING 34
#define NEXTGI_SHADOW_SAMPLER_BINDING 35
#define NEXTGI_SHADOW_TEXTURE_SET 1
#endif
#include "../environment/nextgi_shadow_ray_inc.glsl"

float ml_froxel_trace_visibility(vec3 p_world_pos, float p_linear_depth, uint p_light_type, MLFroxelLightEval p_ev, bool p_use_vsm, bool p_force_visible) {
	if (p_force_visible || p_ev.shadow_opacity <= 0.001) {
		return 1.0;
	}
	uvec4 policy = ml_light_shadow_policies.data[p_ev.policy_index];
	if ((policy.z & ML_POLICY_FLAG_CASTS_SHADOW) == 0u) {
		return 1.0;
	}
	if ((policy.z & ML_POLICY_FLAG_USE_VSM) != 0u) {
		if (!p_use_vsm || params.screen_frame_vsm.w == 0u || (p_ev.vsm_flags & VSM_LIGHT_FLAGS_SAMPLE) == 0u) {
			return 1.0;
		}
		float inv_fade = max(params.fog_z_params.w, 1.0);
		if (p_light_type == ML_LIGHT_TYPE_DIRECTIONAL) {
			return vsm_fog_shadow_coarse_directional(params.screen_frame_vsm.w, p_ev.vsm_map_base, p_ev.vsm_flags, p_world_pos, p_linear_depth, inv_fade);
		}
		return vsm_fog_shadow_coarse_local(params.screen_frame_vsm.w, p_ev.vsm_map_base, p_ev.vsm_flags, p_world_pos, inv_fade);
	}
	if (!(p_ev.ray_dist > 0.0)) {
		return 1.0;
	}
	return trace_shadow(p_world_pos, p_ev.dir_ws, p_ev.ray_dist, policy.x, policy.y);
}

void main() {
	ml_froxel_sample_main();
}
