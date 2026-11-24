/**************************************************************************/
/*  scene_forward_lights_inc.glsl                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// Functions related to lighting

#extension GL_EXT_control_flow_attributes : require

#include "area_lights_inc.glsl"
#include "ies_inc.glsl"

// GodotNext: light_source_parity (#140, plan §21.3 contract M8, GM-11).
// Evaluates a light's IES photometric profile in light local space (-Z forward, +X right, +Y up).
float ies_attenuation(LightData p_light, vec3 p_dir_ls) {
#ifdef FORWARD_CLUSTERED_IES_ATLAS
	return ies_sample_atlas(p_light.ies_layer, p_light.ies_scale, p_dir_ls, ies_profile_atlas, SAMPLER_LINEAR_CLAMP);
#else
	return 1.0;
#endif
}

#if defined(FORWARD_CLUSTERED_IES_ATLAS) && !defined(USING_MOBILE_RENDERER) && !defined(SHADOWS_DISABLED)
half apply_local_contact_shadow(half p_shadow, uint p_sscs_index, float p_casting_intensity, float p_non_casting_intensity, vec2 p_screen_uv) {
	if (bool(implementation_data.ss_effects_flags & SCREEN_SPACE_EFFECTS_FLAGS_USE_SSCS) && p_sscs_index != 0xFFFFFFFFu) {
		bool casts_shadows = bool(instances.data[instance_index_interp].flags & INSTANCE_FLAGS_CASTS_SHADOWS);
		float cs_intensity = casts_shadows ? p_casting_intensity : p_non_casting_intensity;
		if (cs_intensity > 0.0) {
#ifdef USE_MULTIVIEW
			float sscs_layer = float(p_sscs_index * 2u + uint(ViewIndex));
#else
			float sscs_layer = float(p_sscs_index);
#endif
			float sscs_shadow = textureLod(sampler2DArray(sscs_buffer, SAMPLER_LINEAR_CLAMP), vec3(p_screen_uv, sscs_layer), 0.0).r;
			p_shadow = min(p_shadow, half(mix(1.0, sscs_shadow, cs_intensity)));
		}
	}
	return p_shadow;
}
#endif

// This annotation macro must be placed before any loops that rely on specialization constants as their upper bound.
// Drivers may choose to unroll these loops based on the possible range of the value that can be deduced from the
// spec constant, which can lead to their code generation taking a much longer time than desired.
#ifdef UBERSHADER
// Prefer to not unroll loops on the ubershader to reduce code size as much as possible.
#define SPEC_CONSTANT_LOOP_ANNOTATION [[dont_unroll]]
#else
// Don't make an explicit hint on specialized shaders.
#define SPEC_CONSTANT_LOOP_ANNOTATION
#endif

half D_GGX(half NoH, half roughness, hvec3 n, hvec3 h) {
	half a = NoH * roughness;
#ifdef EXPLICIT_FP16
	hvec3 NxH = cross(n, h);
	half k = roughness / (dot(NxH, NxH) + a * a);
#else
	float k = roughness / (1.0 - NoH * NoH + a * a);
#endif
	half d = k * k * half(1.0 / M_PI);
	return saturateHalf(d);
}

#ifdef LIGHT_TRANSMITTANCE_USED
#ifdef SSS_MODE_SKIN
hvec3 SSS_skin(half NdotL, half transmittance_depth, half transmittance_z, half transmittance_boost, hvec4 transmittance_color, hvec3 light_color) {
	half scale = half(8.25) / transmittance_depth;
	half d = scale * abs(transmittance_z);
	float dd = float(-d * d);
	hvec3 profile = hvec3(vec3(0.233, 0.455, 0.649) * exp(dd / 0.0064) +
			vec3(0.1, 0.336, 0.344) * exp(dd / 0.0484) +
			vec3(0.118, 0.198, 0.0) * exp(dd / 0.187) +
			vec3(0.113, 0.007, 0.007) * exp(dd / 0.567) +
			vec3(0.358, 0.004, 0.0) * exp(dd / 1.99) +
			vec3(0.078, 0.0, 0.0) * exp(dd / 7.41));

	return profile * transmittance_color.a * light_color * clamp(transmittance_boost - NdotL, half(0.0), half(1.0)) * half(1.0 / M_PI);
}
#else
hvec3 SSS(half NdotL, half transmittance_depth, half transmittance_z, half transmittance_boost, hvec4 transmittance_color, hvec3 light_color) {
	half scale = half(8.25) / transmittance_depth;
	half d = scale * abs(transmittance_z);
	half dd = -d * d;
	return exp(dd) * transmittance_color.rgb * transmittance_color.a * light_color * clamp(transmittance_boost - NdotL, half(0.0), half(1.0)) * half(1.0 / M_PI);
}
#endif
#endif

// From Earl Hammon, Jr. "PBR Diffuse Lighting for GGX+Smith Microsurfaces" https://www.gdcvault.com/play/1024478/PBR-Diffuse-Lighting-for-GGX
half V_GGX(half NdotL, half NdotV, half alpha) {
	half v = half(0.5) / mix(half(2.0) * NdotL * NdotV, NdotL + NdotV, alpha);
	return saturateHalf(v);
}

half D_GGX_anisotropic(half cos_theta_m, half alpha_x, half alpha_y, half cos_phi, half sin_phi) {
	half alpha2 = alpha_x * alpha_y;
	vec3 v = vec3(alpha_y * cos_phi, alpha_x * sin_phi, alpha2 * cos_theta_m);
	float v2 = dot(v, v);
	half w2 = half(float(alpha2) / v2);
	return alpha2 * w2 * w2 * half(1.0 / M_PI);
}

half V_GGX_anisotropic(half alpha_x, half alpha_y, half TdotV, half TdotL, half BdotV, half BdotL, half NdotV, half NdotL) {
	half Lambda_V = NdotL * length(hvec3(alpha_x * TdotV, alpha_y * BdotV, NdotV));
	half Lambda_L = NdotV * length(hvec3(alpha_x * TdotL, alpha_y * BdotL, NdotL));
	half v = half(0.5) / (Lambda_V + Lambda_L);
	return saturateHalf(v);
}

half SchlickFresnel(half u) {
	half m = half(1.0) - u;
	half m2 = m * m;
	return m2 * m2 * m; // pow(m,5)
}

hvec3 F0(half metallic, half specular, hvec3 albedo) {
	half dielectric = half(0.16) * specular * specular;
	// use albedo * metallic as colored specular reflectance at 0 angle for metallic materials;
	// see https://google.github.io/filament/Filament.md.html
	return mix(hvec3(dielectric), albedo, hvec3(metallic));
}

half V_Kelemen(half LdotH) {
	// Kelemen 2001, "A Microfacet Based Coupled Specular-Matte BRDF Model with Importance Sampling"
	return saturateHalf(half(0.25) / (LdotH * LdotH + half(1e-4)));
}

hvec3 f0_Clear_Coat_To_Surface(hvec3 f0) {
	// Approximation of iorTof0(f0ToIor(f0), 1.5)
	// This assumes that the clear coat layer has an IOR of 1.5
	// see https://github.com/google/filament/blob/837b2715a05f4656d4f524bce50d1b23ff8f84c9/shaders/src/surface_material.fs#L54-L62
	return clamp(f0 * (f0 * (half(0.941892) - half(0.263008) * f0) + half(0.346479)) - half(0.0285998), half(0.0), half(1.0));
}

void light_compute(hvec3 N, hvec3 L, hvec3 V, half A, hvec3 light_color, bool is_directional, half attenuation, hvec3 f0, half roughness, half metallic, half specular_amount, hvec3 albedo, inout half alpha, vec2 screen_uv, hvec3 energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
		hvec3 backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
		hvec4 transmittance_color,
		half transmittance_depth,
		half transmittance_boost,
		half transmittance_z,
#endif
#ifdef LIGHT_RIM_USED
		half rim, half rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
		half clearcoat, half clearcoat_roughness, hvec3 vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
		hvec3 B, hvec3 T, half anisotropy,
#endif
		inout hvec3 diffuse_light, inout hvec3 specular_light) {
#if defined(LIGHT_CODE_USED)
	// Light is written by the user shader.
	mat4 inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef USING_MOBILE_RENDERER
	uint instance_index = draw_call.instance_index;
#else
	uint instance_index = instance_index_interp;
#endif

	mat4 read_model_matrix = transpose(mat4(instances.data[instance_index].transform[0],
			instances.data[instance_index].transform[1],
			instances.data[instance_index].transform[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#undef projection_matrix
#define projection_matrix scene_data_block.data.projection_matrix
#undef inv_projection_matrix
#define inv_projection_matrix scene_data_block.data.inv_projection_matrix

	vec2 read_viewport_size = scene_data_block.data.viewport_size;

#ifdef LIGHT_BACKLIGHT_USED
	vec3 backlight_highp = vec3(backlight);
#endif
	float roughness_highp = float(roughness);
	float metallic_highp = float(metallic);
	vec3 albedo_highp = vec3(albedo);
	float alpha_highp = float(alpha);
	vec3 normal_highp = vec3(N);
	vec3 light_highp = vec3(L);
	vec3 view_highp = vec3(V);
	float specular_amount_highp = float(specular_amount);
	vec3 light_color_highp = vec3(light_color);
	float attenuation_highp = float(attenuation);
	vec3 diffuse_light_highp = vec3(diffuse_light);
	vec3 specular_light_highp = vec3(specular_light);
	bool is_area = false;
	vec3 area_diffuse = vec3(1.0);
	vec3 area_specular = vec3(1.0);
	vec3 area_diffuse_tex_color = vec3(1.0);
	vec3 area_specular_tex_color = vec3(1.0);

#CODE : LIGHT

	alpha = half(alpha_highp);
	diffuse_light = hvec3(diffuse_light_highp);
	specular_light = hvec3(specular_light_highp);
#else // !LIGHT_CODE_USED
	half NdotL = min(A + dot(N, L), half(1.0));
	half cNdotV = max(dot(N, V), half(1e-4));

#ifdef LIGHT_TRANSMITTANCE_USED
	{
#ifdef SSS_MODE_SKIN
		diffuse_light += SSS_skin(NdotL, transmittance_depth, transmittance_z, transmittance_boost, transmittance_color, light_color);
#else
		diffuse_light += SSS(NdotL, transmittance_depth, transmittance_z, transmittance_boost, transmittance_color, light_color);
#endif
	}
#endif //LIGHT_TRANSMITTANCE_USED

#if defined(LIGHT_RIM_USED)
	// Epsilon min to prevent pow(0, 0) singularity which results in undefined behavior.
	half rim_light = pow(max(half(1e-4), half(1.0) - cNdotV), max(half(0.0), (half(1.0) - roughness) * half(16.0)));
	diffuse_light += rim_light * rim * mix(hvec3(1.0), albedo, rim_tint) * light_color;
#endif

	half cc_attenuation = half(1.0);

	// We skip checking on attenuation on directional lights to avoid a branch that is not as beneficial for directional lights as the other ones.
	if (is_directional || attenuation > HALF_FLT_MIN) {
		half cNdotL = max(NdotL, half(0.0));
#if defined(DIFFUSE_BURLEY) || defined(SPECULAR_SCHLICK_GGX) || defined(LIGHT_CLEARCOAT_USED)
		hvec3 H = normalize(V + L);
		half cLdotH = clamp(A + dot(L, H), half(0.0), half(1.0));
#endif

#if defined(LIGHT_CLEARCOAT_USED)
		// Clearcoat ignores normal_map, use vertex normal instead
		half ccNdotL = clamp(A + dot(vertex_normal, L), half(0.0), half(1.0));
		half ccNdotH = clamp(A + dot(vertex_normal, H), half(0.0), half(1.0));
		half cLdotH5 = SchlickFresnel(cLdotH);

		half Dr = D_GGX(ccNdotH, half(mix(half(0.001), half(0.1), clearcoat_roughness)), vertex_normal, H);
		half Gr = V_Kelemen(cLdotH);
		half Fr = mix(half(0.04), half(1.0), cLdotH5) * clearcoat;
		cc_attenuation = half(1.0) - Fr;
		half clearcoat_specular_brdf_NL = Dr * Gr * Fr * ccNdotL;
		specular_light += clearcoat_specular_brdf_NL * light_color * attenuation * specular_amount;
#endif // LIGHT_CLEARCOAT_USED

		if (metallic < half(1.0)) {
			half diffuse_brdf_NL; // BRDF times N.L for calculating diffuse radiance

#if defined(DIFFUSE_LAMBERT_WRAP)
			// Energy conserving lambert wrap shader.
			// https://web.archive.org/web/20210228210901/http://blog.stevemcauley.com/2011/12/03/energy-conserving-wrapped-diffuse/
			half op_roughness = half(1.0) + roughness;
			diffuse_brdf_NL = max(half(0.0), (NdotL + roughness) / (op_roughness * op_roughness)) * half(1.0 / M_PI);
#elif defined(DIFFUSE_TOON)

			diffuse_brdf_NL = smoothstep(-roughness, max(roughness, half(0.01)), NdotL) * half(1.0 / M_PI);

#elif defined(DIFFUSE_BURLEY)
			{
				half FD90_minus_1 = half(2.0) * cLdotH * cLdotH * roughness - half(0.5);
				half FdV = half(1.0) + FD90_minus_1 * SchlickFresnel(cNdotV);
				half FdL = half(1.0) + FD90_minus_1 * SchlickFresnel(cNdotL);
				diffuse_brdf_NL = half(1.0 / M_PI) * FdV * FdL * cNdotL;
			}
#else
			// lambert
			diffuse_brdf_NL = cNdotL * half(1.0 / M_PI);
#endif

			diffuse_light += light_color * diffuse_brdf_NL * attenuation * cc_attenuation;

#if defined(LIGHT_BACKLIGHT_USED)
			diffuse_light += light_color * (hvec3(1.0 / M_PI) - diffuse_brdf_NL) * backlight * attenuation;
#endif
		}

		if (roughness > half(0.0)) {
#if defined(SPECULAR_SCHLICK_GGX)
			half cNdotH = clamp(A + dot(N, H), half(0.0), half(1.0));
#endif
			// Apply specular light.
			// FIXME: roughness == 0 should not disable specular light entirely
#if defined(SPECULAR_TOON)
			hvec3 R = normalize(-reflect(L, N));
			half RdotV = dot(R, V);
			half mid = half(1.0) - roughness;
			mid *= mid;
			half intensity = smoothstep(mid - roughness * half(0.5), mid + roughness * half(0.5), RdotV) * mid;
			diffuse_light += light_color * intensity * attenuation * specular_amount; // write to diffuse_light, as in toon shading you generally want no reflection

#elif defined(SPECULAR_DISABLED)
			// Do nothing.

#elif defined(SPECULAR_SCHLICK_GGX)
			// shlick+ggx as default
			half alpha_ggx = roughness * roughness;
#if defined(LIGHT_ANISOTROPY_USED)
			half aspect = sqrt(half(1.0) - anisotropy * half(0.9));
			half ax = alpha_ggx / aspect;
			half ay = alpha_ggx * aspect;
			half XdotH = dot(T, H);
			half YdotH = dot(B, H);
			half D = D_GGX_anisotropic(cNdotH, ax, ay, XdotH, YdotH);
			half G = V_GGX_anisotropic(ax, ay, dot(T, V), dot(T, L), dot(B, V), dot(B, L), cNdotV, cNdotL);
#else // LIGHT_ANISOTROPY_USED
			half D = D_GGX(cNdotH, alpha_ggx, N, H);
			half G = V_GGX(cNdotL, cNdotV, alpha_ggx);
#endif // LIGHT_ANISOTROPY_USED
	   // F
#if !defined(LIGHT_CLEARCOAT_USED)
			half cLdotH5 = SchlickFresnel(cLdotH);
#endif
			// Calculate Fresnel using specular occlusion term from Filament:
			// https://google.github.io/filament/Filament.html#lighting/occlusion/specularocclusion
			half f90 = clamp(dot(f0, hvec3(50.0 * 0.33)), metallic, half(1.0));
			hvec3 F = f0 + (f90 - f0) * cLdotH5;
			hvec3 specular_brdf_NL = energy_compensation * cNdotL * D * F * G;
			specular_light += specular_brdf_NL * light_color * attenuation * cc_attenuation * specular_amount;
#endif
		}

#ifdef USE_SHADOW_TO_OPACITY
		alpha = min(alpha, clamp(half(1.0 - attenuation), half(0.0), half(1.0)));
#endif
	}
#endif // LIGHT_CODE_USED
}

// GodotNext: nextgeo_shading_bins (#158). The pixel the shadow noise below is keyed on. NextGeo's compute
// shading bins (forward_clustered/scene_forward_clustered.glsl) define it as the bin pixel's center.
#ifndef NEXTGEO_FRAG_COORD
#define NEXTGEO_FRAG_COORD gl_FragCoord
#endif

#ifndef SHADOWS_DISABLED

// Interleaved Gradient Noise
// https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float quick_hash(vec2 pos) {
	const vec3 magic = vec3(0.06711056f, 0.00583715f, 52.9829189f);
	return fract(magic.z * fract(dot(pos, magic.xy)));
}

#ifdef MODE_VISIBILITY_RESOLVE
#define SHADOW_SAMPLE(m_tex, m_uvz) textureLod(sampler2DShadow(m_tex, shadow_sampler), (m_uvz), 0.0)
#else
#define SHADOW_SAMPLE(m_tex, m_uvz) textureProj(sampler2DShadow(m_tex, shadow_sampler), vec4((m_uvz), 1.0))
#endif

half sample_directional_pcf_shadow(texture2D shadow, vec2 shadow_pixel_size, vec4 coord, float taa_frame_count) {
	vec2 pos = coord.xy;
	float depth = coord.z;

	//if only one sample is taken, take it from the center
	if (sc_directional_soft_shadow_samples() == 0) {
		return half(SHADOW_SAMPLE(shadow, vec3(pos, depth)));
	}

	mat2 disk_rotation;
	{
		float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
		float sr = sin(r);
		float cr = cos(r);
		disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
	}

	float avg = 0.0;

	SPEC_CONSTANT_LOOP_ANNOTATION
	for (uint i = 0; i < sc_directional_soft_shadow_samples(); i++) {
		avg += SHADOW_SAMPLE(shadow, vec3(pos + shadow_pixel_size * (disk_rotation * scene_data_block.data.directional_soft_shadow_kernel[i].xy), depth));
	}

	return half(avg * (1.0 / float(sc_directional_soft_shadow_samples())));
}

half sample_pcf_shadow(texture2D shadow, vec2 shadow_pixel_size, vec3 coord, float taa_frame_count) {
	vec2 pos = coord.xy;
	float depth = coord.z;

	//if only one sample is taken, take it from the center
	if (sc_soft_shadow_samples() == 0) {
		return half(SHADOW_SAMPLE(shadow, vec3(pos, depth)));
	}

	mat2 disk_rotation;
	{
		float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
		float sr = sin(r);
		float cr = cos(r);
		disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
	}

	float avg = 0.0;

	SPEC_CONSTANT_LOOP_ANNOTATION
	for (uint i = 0; i < sc_soft_shadow_samples(); i++) {
		avg += SHADOW_SAMPLE(shadow, vec3(pos + shadow_pixel_size * (disk_rotation * scene_data_block.data.soft_shadow_kernel[i].xy), depth));
	}

	return half(avg * (1.0 / float(sc_soft_shadow_samples())));
}

half sample_omni_pcf_shadow(texture2D shadow, float blur_scale, vec2 coord, vec4 uv_rect, vec2 flip_offset, float depth, float taa_frame_count) {
	//if only one sample is taken, take it from the center
	if (sc_soft_shadow_samples() == 0) {
		vec2 pos = coord * 0.5 + 0.5;
		pos = uv_rect.xy + pos * uv_rect.zw;
		return half(SHADOW_SAMPLE(shadow, vec3(pos, depth)));
	}

	mat2 disk_rotation;
	{
		float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
		float sr = sin(r);
		float cr = cos(r);
		disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
	}

	float avg = 0.0;
	vec2 offset_scale = blur_scale * 2.0 * scene_data_block.data.shadow_atlas_pixel_size / uv_rect.zw;

	SPEC_CONSTANT_LOOP_ANNOTATION
	for (uint i = 0; i < sc_soft_shadow_samples(); i++) {
		vec2 offset = offset_scale * (disk_rotation * scene_data_block.data.soft_shadow_kernel[i].xy);
		vec2 sample_coord = coord + offset;

		float sample_coord_length_squared = dot(sample_coord, sample_coord);
		bool do_flip = sample_coord_length_squared > 1.0;

		if (do_flip) {
			float len = sqrt(sample_coord_length_squared);
			sample_coord = sample_coord * (2.0 / len - 1.0);
		}

		sample_coord = sample_coord * 0.5 + 0.5;
		sample_coord = uv_rect.xy + sample_coord * uv_rect.zw;

		if (do_flip) {
			sample_coord += flip_offset;
		}
		avg += SHADOW_SAMPLE(shadow, vec3(sample_coord, depth));
	}

	return half(avg * (1.0 / float(sc_soft_shadow_samples())));
}

half sample_directional_soft_shadow(texture2D shadow, vec3 pssm_coord, vec2 tex_scale, float taa_frame_count) {
	//find blocker
	float blocker_count = 0.0;
	float blocker_average = 0.0;

	mat2 disk_rotation;
	{
		float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
		float sr = sin(r);
		float cr = cos(r);
		disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
	}

	SPEC_CONSTANT_LOOP_ANNOTATION
	for (uint i = 0; i < sc_directional_penumbra_shadow_samples(); i++) {
		vec2 suv = pssm_coord.xy + (disk_rotation * scene_data_block.data.directional_penumbra_shadow_kernel[i].xy) * tex_scale;
		float d = textureLod(sampler2D(shadow, SAMPLER_LINEAR_CLAMP), suv, 0.0).r;
		if (d > pssm_coord.z) {
			blocker_average += d;
			blocker_count += 1.0;
		}
	}

	if (blocker_count > 0.0) {
		//blockers found, do soft shadow
		blocker_average /= blocker_count;
		float penumbra = (-pssm_coord.z + blocker_average) / (1.0 - blocker_average);
		tex_scale *= penumbra;

		float s = 0.0;

		SPEC_CONSTANT_LOOP_ANNOTATION
		for (uint i = 0; i < sc_directional_penumbra_shadow_samples(); i++) {
			vec2 suv = pssm_coord.xy + (disk_rotation * scene_data_block.data.directional_penumbra_shadow_kernel[i].xy) * tex_scale;
			s += SHADOW_SAMPLE(shadow, vec3(suv, pssm_coord.z));
		}

		return half(s / float(sc_directional_penumbra_shadow_samples()));

	} else {
		//no blockers found, so no shadow
		return half(1.0);
	}
}

#ifdef VSM_PROJECTION
// GodotNext: vsm_projection (#222). The light loop works in view space, virtual shadow maps in world space.
// SceneData's inv_view_matrix holds the camera transform's rows.
vec3 vsm_world_position(vec3 p_view) {
	vec4 p = vec4(p_view, 1.0);
	return vec3(dot(scene_data_block.data.inv_view_matrix[0], p), dot(scene_data_block.data.inv_view_matrix[1], p), dot(scene_data_block.data.inv_view_matrix[2], p));
}

vec3 vsm_world_vector(vec3 p_view) {
	vec4 v = vec4(p_view, 0.0);
	return vec3(dot(scene_data_block.data.inv_view_matrix[0], v), dot(scene_data_block.data.inv_view_matrix[1], v), dot(scene_data_block.data.inv_view_matrix[2], v));
}

vec3 vsm_world_normal(vec3 p_view) {
	return normalize(vsm_world_vector(p_view));
}

vec2 vsm_pixel_noise() {
	// NEXTGEO_FRAG_COORD (#158): gl_FragCoord, or the bin pixel's center in NextGeo's compute shading bins.
	return vsm_blue_noise_sample(uvec2(NEXTGEO_FRAG_COORD.xy), scene_data_block.data.taa_frame_count);
}

bool vsm_light_sampled(uint p_vsm_flags) {
	return sc_use_vsm() && (p_vsm_flags & VSM_LIGHT_FLAGS_SAMPLE) != 0u;
}

// GodotNext: df_shadows (#214, plan §21.3 contract S7). Distance-field shadow helpers and
// depth- and normal-aware 2x2 bilateral upsample from the half-res R8 mask array.
bool df_light_sampled(uint p_vsm_flags) {
	return sc_use_df_shadows() && (p_vsm_flags & VSM_LIGHT_FLAGS_DF_SAMPLE) != 0u;
}

bool df_light_full(uint p_vsm_flags) {
	return (p_vsm_flags & VSM_LIGHT_FLAGS_DF_FULL) != 0u;
}

uint df_light_layer(uint p_vsm_flags) {
	return (p_vsm_flags >> VSM_LIGHT_FLAGS_DF_LAYER_SHIFT) & VSM_LIGHT_FLAGS_DF_LAYER_MASK;
}

float df_directional_blend(float p_depth, float p_start, float p_distance) {
	if (!(p_start > 0.0) || !(p_distance > p_start)) {
		return 0.0;
	}
	float blend_lo = p_start * (1.0 - VSM_DF_BLEND_BAND_FRACTION);
	float u = clamp((p_depth - blend_lo) / max(p_start - blend_lo, 1e-4), 0.0, 1.0);
	return u * u * (3.0 - 2.0 * u);
}

float df_shadow_upsample(uint p_layer, float p_depth, vec3 p_view_normal) {
	ivec2 half_size = textureSize(sampler2D(df_shadow_guide, SAMPLER_NEAREST_CLAMP), 0);
	ivec2 max_coord = max(half_size - ivec2(1), ivec2(0));
	// df_shadow.glsl traces full-res pixel ivec2(hp * 2) whose frag_coord is hp * 2 + 0.5.
	vec2 h_pos = (NEXTGEO_FRAG_COORD.xy - vec2(0.5)) * 0.5;
	ivec2 base = ivec2(floor(h_pos));
	vec2 f = clamp(h_pos - vec2(base), vec2(0.0), vec2(1.0));
	float b[4];
	b[0] = (1.0 - f.x) * (1.0 - f.y);
	b[1] = f.x * (1.0 - f.y);
	b[2] = (1.0 - f.x) * f.y;
	b[3] = f.x * f.y;
	vec3 n_unit = normalize(p_view_normal);
	float inv_depth = 1.0 / max(p_depth, 0.05);
	float w_sum = 0.0;
	float v_sum = 0.0;
	float best_depth_err = 1e30;
	float fallback_mask = 1.0;
	bool any_valid = false;
	for (int k = 0; k < 4; k++) {
		ivec2 c = clamp(base + ivec2(k & 1, k >> 1), ivec2(0), max_coord);
		vec4 guide = texelFetch(sampler2D(df_shadow_guide, SAMPLER_NEAREST_CLAMP), c, 0);
		float td = guide.w;
		if (!(td > 0.0)) {
			continue;
		}
		any_valid = true;
		float m = texelFetch(sampler2DArray(df_shadow_mask, SAMPLER_NEAREST_CLAMP), ivec3(c, int(p_layer)), 0).r;
		float rel_depth = abs(td - p_depth) * inv_depth;
		if (rel_depth < best_depth_err) {
			best_depth_err = rel_depth;
			fallback_mask = m;
		}
		float w_depth = exp(-rel_depth * 32.0);
		float ndot = max(dot(n_unit, guide.xyz), 0.0);
		float ndot2 = ndot * ndot;
		float w = b[k] * w_depth * (ndot2 * ndot2);
		w_sum += w;
		v_sum += w * m;
	}
	if (!any_valid) {
		return 1.0;
	}
	return w_sum > 1e-5 ? clamp(v_sum / w_sum, 0.0, 1.0) : clamp(fallback_mask, 0.0, 1.0);
}
#endif

#ifdef RT_SHADOW_SAMPLING
// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Tile-sparse R8 RT shadow mask and hit-distance lookup.
#define RT_SHADOW_LIGHT_FLAG_SAMPLE (1u << 0u)
#define RT_SHADOW_TILE_NONE 0xFFFFFFFFu
#define RT_SHADOW_HIT_MISS 65504.0

bool rt_shadow_light_sampled(uint p_rt_flags) {
	return sc_use_rt_shadows() && (p_rt_flags & RT_SHADOW_LIGHT_FLAG_SAMPLE) != 0u;
}

uint rt_shadow_lookup_tile(uint p_base, uvec2 p_screen_px) {
	uvec2 st = p_screen_px >> 4u;
	uint pmin = rt_shadow_tile_lookup.data[p_base];
	uint pdim = rt_shadow_tile_lookup.data[p_base + 1u];
	uvec2 min_t = uvec2(pmin & 0xFFFFu, pmin >> 16u);
	uvec2 dim_t = uvec2(pdim & 0xFFFFu, pdim >> 16u);
	if (st.x < min_t.x || st.y < min_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uvec2 rel = st - min_t;
	if (rel.x >= dim_t.x || rel.y >= dim_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uint entry = rt_shadow_tile_lookup.data[p_base + 2u + rel.y * dim_t.x + rel.x];
	return entry > 0u ? (entry - 1u) : RT_SHADOW_TILE_NONE;
}

float rt_shadow_sample(uint p_base, uvec2 p_screen_px) {
	uint tile_id = rt_shadow_lookup_tile(p_base, p_screen_px);
	if (tile_id == RT_SHADOW_TILE_NONE) {
		return 1.0;
	}
	ivec2 atlas_px = ivec2(int((tile_id & 0xFFu) * 16u + (p_screen_px.x & 15u)), int((tile_id >> 8u) * 16u + (p_screen_px.y & 15u)));
	return texelFetch(sampler2D(rt_shadow_atlas, SAMPLER_NEAREST_CLAMP), atlas_px, 0).r;
}

float rt_shadow_sample_hit_dist(uint p_base, uvec2 p_screen_px) {
	uint tile_id = rt_shadow_lookup_tile(p_base, p_screen_px);
	if (tile_id == RT_SHADOW_TILE_NONE) {
		return -1.0;
	}
	ivec2 atlas_px = ivec2(int((tile_id & 0xFFu) * 16u + (p_screen_px.x & 15u)), int((tile_id >> 8u) * 16u + (p_screen_px.y & 15u)));
	float hd = texelFetch(sampler2D(rt_shadow_hit_dist, SAMPLER_NEAREST_CLAMP), atlas_px, 0).r;
	return hd < RT_SHADOW_HIT_MISS * 0.5 ? hd : -1.0;
}
#endif

#endif // SHADOWS_DISABLED

#ifdef LIGHT_FUNCTION_ATLAS
// GodotNext: light_function_atlas (#138). View-space to world-space conversion for Light Function Atlas lookups,
// available regardless of SHADOWS_DISABLED.
vec3 lf_world_position(vec3 p_view) {
	vec4 p = vec4(p_view, 1.0);
	return vec3(dot(scene_data_block.data.inv_view_matrix[0], p), dot(scene_data_block.data.inv_view_matrix[1], p), dot(scene_data_block.data.inv_view_matrix[2], p));
}
#endif

half get_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd; // nd^4
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2
	return half(nd * pow(max(distance, 0.0001), -decay));
}

void light_process_omni(uint idx, vec3 vertex, hvec3 eye_vec, hvec3 normal, vec3 vertex_ddx, vec3 vertex_ddy, hvec3 f0, half roughness, half metallic, float taa_frame_count, hvec3 albedo, inout half alpha, vec2 screen_uv, hvec3 energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
		hvec3 backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
		hvec4 transmittance_color,
		half transmittance_depth,
		half transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
		half rim, half rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
		half clearcoat, half clearcoat_roughness, hvec3 vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
		hvec3 binormal, hvec3 tangent, half anisotropy,
#endif
		inout hvec3 diffuse_light, inout hvec3 specular_light) {

	// Omni light attenuation.
	vec3 light_rel_vec = omni_lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	half omni_attenuation = get_omni_attenuation(light_length, omni_lights.data[idx].inv_radius, omni_lights.data[idx].attenuation);
	// GodotNext: light_source_parity (#140). IES profile attenuation in light local space.
	vec3 light_rel_vec_norm = light_rel_vec / max(light_length, 1e-6);
	float omni_ies = 1.0;
	if (omni_lights.data[idx].ies_layer != IES_LAYER_NONE) {
		vec3 lx = vec3(omni_lights.data[idx].area_width);
		vec3 ly = vec3(omni_lights.data[idx].area_height);
		vec3 lz = cross(lx, ly);
		vec3 dir_ls = vec3(dot(-light_rel_vec_norm, lx), dot(-light_rel_vec_norm, ly), dot(-light_rel_vec_norm, lz));
		omni_ies = ies_attenuation(omni_lights.data[idx], dir_ls);
		if (omni_ies <= 0.0) {
			return;
		}
	}

	// Compute size.
	half size = half(0.0);
	if (sc_use_light_soft_shadows() && omni_lights.data[idx].size > 0.0) {
		half t = half(omni_lights.data[idx].size / max(0.001, light_length));
		size = half(1.0) / sqrt(half(1.0) + t * t);
		size = max(half(1.0) - size, half(0.0));
	}

	half shadow = half(1.0);
#ifndef SHADOWS_DISABLED
	// Omni light shadow.
#ifdef VSM_PROJECTION
	// GodotNext: vsm_projection (#222), vsm_fog_translucency (#215), df_shadows (#214), rt_shadows (#141).
	// A light sampled through RT shadows, DF, or VSM has no atlas tile.
#ifdef RT_SHADOW_SAMPLING
	const bool rt_sampled = rt_shadow_light_sampled(omni_lights.data[idx].rt_shadow_flags);
#else
	const bool rt_sampled = false;
#endif
	const bool df_sampled = !rt_sampled && df_light_sampled(omni_lights.data[idx].vsm_flags);
	const bool vsm_sampled = !rt_sampled && !df_sampled && vsm_light_sampled(omni_lights.data[idx].vsm_flags);
#ifdef RT_SHADOW_SAMPLING
	if (rt_sampled && omni_attenuation > HALF_FLT_MIN && omni_lights.data[idx].shadow_opacity > 0.001) {
		float rt_shadow = rt_shadow_sample(omni_lights.data[idx].rt_shadow_tile_base, uvec2(NEXTGEO_FRAG_COORD.xy));
		shadow = mix(half(1.0), half(rt_shadow), half(omni_lights.data[idx].shadow_opacity));
	} else
#endif
	if (df_sampled && omni_attenuation > HALF_FLT_MIN && omni_lights.data[idx].shadow_opacity > 0.001) {
		float df_shadow = df_shadow_upsample(df_light_layer(omni_lights.data[idx].vsm_flags), -vertex.z, vec3(normal));
		shadow = mix(half(1.0), half(df_shadow), half(omni_lights.data[idx].shadow_opacity));
	} else if (vsm_sampled && omni_attenuation > HALF_FLT_MIN && omni_lights.data[idx].shadow_opacity > 0.001) {
		float vsm_shadow;
		if (sc_use_vsm_coarse()) {
			vsm_shadow = vsm_shadow_coarse_local(vsm_projection.data.counts.x, omni_lights.data[idx].vsm_map_base, omni_lights.data[idx].vsm_flags, vsm_world_position(vertex), vsm_world_normal(vec3(normal)), vsm_world_position(omni_lights.data[idx].position));
		} else {
			vsm_shadow = vsm_shadow_local(omni_lights.data[idx].vsm_map_base, omni_lights.data[idx].vsm_flags, vsm_world_position(vertex), vsm_world_normal(vec3(normal)), vsm_world_position(omni_lights.data[idx].position), omni_lights.data[idx].size, vec3(0.0), vec3(0.0), -vertex.z, vsm_pixel_noise());
		}
		shadow = mix(half(1.0), half(vsm_shadow), half(omni_lights.data[idx].shadow_opacity));
	}
	if (!rt_sampled && !vsm_sampled && !df_sampled && omni_attenuation > HALF_FLT_MIN && omni_lights.data[idx].shadow_opacity > 0.001) {
#else
	if (omni_attenuation > HALF_FLT_MIN && omni_lights.data[idx].shadow_opacity > 0.001) {
#endif
		// there is a shadowmap
		vec2 texel_size = scene_data_block.data.shadow_atlas_pixel_size;
		vec4 base_uv_rect = omni_lights.data[idx].atlas_rect;
		base_uv_rect.xy += texel_size;
		base_uv_rect.zw -= texel_size * 2.0;

		// Omni lights use direction.xy to store to store the offset between the two paraboloid regions
		vec2 flip_offset = omni_lights.data[idx].direction.xy;

		vec3 local_vert = (omni_lights.data[idx].shadow_matrix * vec4(vertex, 1.0)).xyz;

		float shadow_len = length(local_vert); //need to remember shadow len from here
		vec3 shadow_dir = normalize(local_vert);

		vec3 local_normal = normalize(mat3(omni_lights.data[idx].shadow_matrix) * vec3(normal));
		vec3 normal_bias = local_normal * omni_lights.data[idx].shadow_normal_bias * (1.0 - abs(dot(local_normal, shadow_dir)));

		if (sc_use_light_soft_shadows() && omni_lights.data[idx].soft_shadow_size > 0.0) {
			//soft shadow

			//find blocker

			float blocker_count = 0.0;
			float blocker_average = 0.0;

			mat2 disk_rotation;
			{
				float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
				float sr = sin(r);
				float cr = cos(r);
				disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
			}

			vec3 basis_normal = shadow_dir;
			vec3 v0 = abs(basis_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
			vec3 tangent = normalize(cross(v0, basis_normal));
			vec3 bitangent = normalize(cross(tangent, basis_normal));
			float z_norm = 1.0 - shadow_len * omni_lights.data[idx].inv_radius;

			tangent *= omni_lights.data[idx].soft_shadow_size * omni_lights.data[idx].soft_shadow_scale;
			bitangent *= omni_lights.data[idx].soft_shadow_size * omni_lights.data[idx].soft_shadow_scale;

			SPEC_CONSTANT_LOOP_ANNOTATION
			for (uint i = 0; i < sc_penumbra_shadow_samples(); i++) {
				vec2 disk = disk_rotation * scene_data_block.data.penumbra_shadow_kernel[i].xy;

				vec3 pos = local_vert + tangent * disk.x + bitangent * disk.y;

				pos = normalize(pos);

				vec4 uv_rect = base_uv_rect;

				if (pos.z >= 0.0) {
					uv_rect.xy += flip_offset;
				}

				pos.z = 1.0 + abs(pos.z);
				pos.xy /= pos.z;

				pos.xy = pos.xy * 0.5 + 0.5;
				pos.xy = uv_rect.xy + pos.xy * uv_rect.zw;

				float d = textureLod(sampler2D(shadow_atlas, SAMPLER_LINEAR_CLAMP), pos.xy, 0.0).r;
				if (d > z_norm) {
					blocker_average += d;
					blocker_count += 1.0;
				}
			}

			if (blocker_count > 0.0) {
				//blockers found, do soft shadow
				blocker_average /= blocker_count;
				float penumbra = (-z_norm + blocker_average) / (1.0 - blocker_average);
				tangent *= penumbra;
				bitangent *= penumbra;

				z_norm += omni_lights.data[idx].inv_radius * omni_lights.data[idx].shadow_bias;

				shadow = half(0.0);

				SPEC_CONSTANT_LOOP_ANNOTATION
				for (uint i = 0; i < sc_penumbra_shadow_samples(); i++) {
					vec2 disk = disk_rotation * scene_data_block.data.penumbra_shadow_kernel[i].xy;
					vec3 pos = local_vert + tangent * disk.x + bitangent * disk.y;

					pos = normalize(pos);
					pos = normalize(pos + normal_bias);

					vec4 uv_rect = base_uv_rect;

					if (pos.z >= 0.0) {
						uv_rect.xy += flip_offset;
					}

					pos.z = 1.0 + abs(pos.z);
					pos.xy /= pos.z;

					pos.xy = pos.xy * 0.5 + 0.5;
					pos.xy = uv_rect.xy + pos.xy * uv_rect.zw;
					shadow += half(SHADOW_SAMPLE(shadow_atlas, vec3(pos.xy, z_norm)));
				}

				shadow /= half(sc_penumbra_shadow_samples());
				shadow = mix(half(1.0), shadow, half(omni_lights.data[idx].shadow_opacity));

			} else {
				//no blockers found, so no shadow
				shadow = half(1.0);
			}
		} else {
			vec4 uv_rect = base_uv_rect;

			vec3 shadow_sample = normalize(shadow_dir + normal_bias);
			if (shadow_sample.z >= 0.0) {
				uv_rect.xy += flip_offset;
				flip_offset *= -1.0;
			}

			shadow_sample.z = 1.0 + abs(shadow_sample.z);
			vec2 pos = shadow_sample.xy / shadow_sample.z;
			float depth = shadow_len - omni_lights.data[idx].shadow_bias;
			depth *= omni_lights.data[idx].inv_radius;
			depth = 1.0 - depth;
			shadow = mix(half(1.0), sample_omni_pcf_shadow(shadow_atlas, omni_lights.data[idx].soft_shadow_scale / shadow_sample.z, pos, uv_rect, flip_offset, depth, taa_frame_count), half(omni_lights.data[idx].shadow_opacity));
		}
	}
#if defined(FORWARD_CLUSTERED_IES_ATLAS) && !defined(USING_MOBILE_RENDERER)
	// GodotNext: light_source_parity (#140). Per-light local contact shadows.
	shadow = apply_local_contact_shadow(shadow, omni_lights.data[idx].contact_shadow_sscs_index, omni_lights.data[idx].contact_shadow_casting_intensity, omni_lights.data[idx].contact_shadow_non_casting_intensity, screen_uv);
#endif
#endif

	vec3 color = omni_lights.data[idx].color * omni_ies;

#ifdef LIGHT_TRANSMITTANCE_USED
	half transmittance_z = transmittance_depth; //no transmittance by default
	transmittance_color.a *= omni_attenuation;
#ifndef SHADOWS_DISABLED
#ifdef VSM_PROJECTION
	// GodotNext: vsm_projection (#222), rt_shadows (#141).
#ifdef RT_SHADOW_SAMPLING
	if (rt_sampled && omni_lights.data[idx].shadow_opacity > 0.001) {
		float rt_thickness = rt_shadow_sample_hit_dist(omni_lights.data[idx].rt_shadow_tile_base, uvec2(NEXTGEO_FRAG_COORD.xy));
		if (rt_thickness >= 0.0) {
			transmittance_z = half(rt_thickness);
		}
	} else
#endif
	if (vsm_sampled && omni_lights.data[idx].shadow_opacity > 0.001) {
		float vsm_thickness = vsm_transmittance_local(omni_lights.data[idx].vsm_map_base, omni_lights.data[idx].vsm_flags, vsm_world_position(vertex - vec3(normal) * omni_lights.data[idx].transmittance_bias), -vertex.z);
		if (vsm_thickness >= 0.0) {
			transmittance_z = half(vsm_thickness);
		}
	}
	if (!rt_sampled && !vsm_sampled && !df_sampled && omni_lights.data[idx].shadow_opacity > 0.001) {
#else
	if (omni_lights.data[idx].shadow_opacity > 0.001) {
#endif
		// Redo shadowmapping, but shrink the model a bit to avoid artifacts.
		vec2 texel_size = scene_data_block.data.shadow_atlas_pixel_size;
		vec4 uv_rect = omni_lights.data[idx].atlas_rect;
		uv_rect.xy += texel_size;
		uv_rect.zw -= texel_size * 2.0;

		// Omni lights use direction.xy to store to store the offset between the two paraboloid regions
		vec2 flip_offset = omni_lights.data[idx].direction.xy;

		vec3 local_vert = (omni_lights.data[idx].shadow_matrix * vec4(vertex - normal * omni_lights.data[idx].transmittance_bias, 1.0)).xyz;

		float shadow_len = length(local_vert); //need to remember shadow len from here
		vec3 shadow_sample = normalize(local_vert);

		if (shadow_sample.z >= 0.0) {
			uv_rect.xy += flip_offset;
			flip_offset *= -1.0;
		}

		shadow_sample.z = 1.0 + abs(shadow_sample.z);
		vec2 pos = shadow_sample.xy / shadow_sample.z;
		float depth = shadow_len * omni_lights.data[idx].inv_radius;
		depth = 1.0 - depth;

		pos = pos * 0.5 + 0.5;
		pos = uv_rect.xy + pos * uv_rect.zw;
		float shadow_z = textureLod(sampler2D(shadow_atlas, SAMPLER_LINEAR_CLAMP), pos, 0.0).r;
		transmittance_z = half((depth - shadow_z) / omni_lights.data[idx].inv_radius);
	}
#endif // !SHADOWS_DISABLED
#endif // LIGHT_TRANSMITTANCE_USED

	if (sc_use_light_projector() && omni_lights.data[idx].projector_rect != vec4(0.0)) {
		vec3 local_v = (omni_lights.data[idx].shadow_matrix * vec4(vertex, 1.0)).xyz;
		local_v = normalize(local_v);

		vec4 atlas_rect = omni_lights.data[idx].projector_rect;

		if (local_v.z >= 0.0) {
			atlas_rect.y += atlas_rect.w;
		}

		local_v.z = 1.0 + abs(local_v.z);

		local_v.xy /= local_v.z;
		local_v.xy = local_v.xy * 0.5 + 0.5;
		vec2 proj_uv = local_v.xy * atlas_rect.zw;

		// Clamp the uv coordinates to prevent bleeding from neighboring decals.
		vec2 texel_size = 1.0 / vec2(textureSize(sampler2D(decal_atlas_srgb, light_projector_sampler), 0));
		proj_uv = clamp(proj_uv, texel_size * 0.5, atlas_rect.zw - texel_size * 0.5);

		if (sc_projector_use_mipmaps()) {
			vec2 proj_uv_ddx;
			vec2 proj_uv_ddy;
			{
				vec3 local_v_ddx = (omni_lights.data[idx].shadow_matrix * vec4(vertex + vertex_ddx, 1.0)).xyz;
				local_v_ddx = normalize(local_v_ddx);

				if (local_v_ddx.z >= 0.0) {
					local_v_ddx.z += 1.0;
				} else {
					local_v_ddx.z = 1.0 - local_v_ddx.z;
				}

				local_v_ddx.xy /= local_v_ddx.z;
				local_v_ddx.xy = local_v_ddx.xy * 0.5 + 0.5;

				proj_uv_ddx = local_v_ddx.xy * atlas_rect.zw - proj_uv;

				vec3 local_v_ddy = (omni_lights.data[idx].shadow_matrix * vec4(vertex + vertex_ddy, 1.0)).xyz;
				local_v_ddy = normalize(local_v_ddy);

				if (local_v_ddy.z >= 0.0) {
					local_v_ddy.z += 1.0;
				} else {
					local_v_ddy.z = 1.0 - local_v_ddy.z;
				}

				local_v_ddy.xy /= local_v_ddy.z;
				local_v_ddy.xy = local_v_ddy.xy * 0.5 + 0.5;

				proj_uv_ddy = local_v_ddy.xy * atlas_rect.zw - proj_uv;
			}

			vec4 proj = textureGrad(sampler2D(decal_atlas_srgb, light_projector_sampler), proj_uv + atlas_rect.xy, proj_uv_ddx, proj_uv_ddy);
			color *= proj.rgb * proj.a;
		} else {
			vec4 proj = textureLod(sampler2D(decal_atlas_srgb, light_projector_sampler), proj_uv + atlas_rect.xy, 0.0);
			color *= proj.rgb * proj.a;
		}
	}

#ifdef LIGHT_FUNCTION_ATLAS
	if (sc_use_light_function() && omni_lights.data[idx].light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(light_function_data.data[omni_lights.data[idx].light_function_index], light_function_atlas, SAMPLER_LINEAR_CLAMP, lf_world_position(vertex));
	}
#endif
	light_compute(normal, hvec3(light_rel_vec_norm), eye_vec, size, hvec3(color), false, omni_attenuation * shadow, f0, roughness, metallic, half(omni_lights.data[idx].specular_amount), albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
			backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
			transmittance_color,
			transmittance_depth,
			transmittance_boost,
			transmittance_z,
#endif
#ifdef LIGHT_RIM_USED
			rim * omni_attenuation, rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
			clearcoat, clearcoat_roughness, vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
			binormal, tangent, anisotropy,
#endif
			diffuse_light,
			specular_light);
}

vec2 normal_to_panorama(vec3 n) {
	n = normalize(n);
	vec2 panorama_coords = vec2(atan(n.x, n.z), acos(-n.y));

	if (panorama_coords.x < 0.0) {
		panorama_coords.x += M_PI * 2.0;
	}

	panorama_coords /= vec2(M_PI * 2.0, M_PI);
	return panorama_coords;
}

void light_process_spot(uint idx, vec3 vertex, hvec3 eye_vec, hvec3 normal, vec3 vertex_ddx, vec3 vertex_ddy, hvec3 f0, half roughness, half metallic, float taa_frame_count, hvec3 albedo, inout half alpha, vec2 screen_uv, hvec3 energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
		hvec3 backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
		hvec4 transmittance_color,
		half transmittance_depth,
		half transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
		half rim, half rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
		half clearcoat, half clearcoat_roughness, hvec3 vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
		hvec3 binormal, hvec3 tangent, half anisotropy,
#endif
		inout hvec3 diffuse_light,
		inout hvec3 specular_light) {

	// Spot light attenuation.
	vec3 light_rel_vec = spot_lights.data[idx].position - vertex;
	float light_length = length(light_rel_vec);
	hvec3 light_rel_vec_norm = hvec3(light_rel_vec / light_length);
	half spot_attenuation = get_omni_attenuation(light_length, spot_lights.data[idx].inv_radius, spot_lights.data[idx].attenuation);
	vec3 spot_dir = spot_lights.data[idx].direction;
	float cone_angle = spot_lights.data[idx].cone_angle;
	float scos = max(dot(-vec3(light_rel_vec_norm), spot_dir), cone_angle);

	// This conversion to a highp float is crucial to prevent light leaking due to precision errors.
	float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - cone_angle));
	spot_attenuation *= half(1.0 - pow(spot_rim, spot_lights.data[idx].cone_attenuation));
	// GodotNext: light_source_parity (#140). IES profile attenuation clipped to the spot cone.
	float spot_ies = 1.0;
	if (spot_lights.data[idx].ies_layer != IES_LAYER_NONE) {
		if (spot_attenuation <= HALF_FLT_MIN) {
			return;
		}
		vec3 lx = vec3(spot_lights.data[idx].area_width);
		vec3 ly = vec3(spot_lights.data[idx].area_height);
		vec3 lz = -spot_dir;
		vec3 dir_ls = vec3(dot(-vec3(light_rel_vec_norm), lx), dot(-vec3(light_rel_vec_norm), ly), dot(-vec3(light_rel_vec_norm), lz));
		spot_ies = ies_attenuation(spot_lights.data[idx], dir_ls);
		if (spot_ies <= 0.0) {
			return;
		}
	}

	// Compute size.
	half size = half(0.0);
	if (sc_use_light_soft_shadows() && spot_lights.data[idx].size > 0.0) {
		half t = half(spot_lights.data[idx].size / max(0.001, light_length));
		size = half(1.0) / sqrt(half(1.0) + t * t);
		size = max(half(1.0) - size, half(0.0));
	}

	half shadow = half(1.0);
#ifndef SHADOWS_DISABLED
	// Spot light shadow.
#ifdef VSM_PROJECTION
	// GodotNext: vsm_projection (#222), vsm_fog_translucency (#215), df_shadows (#214), rt_shadows (#141).
	// A light sampled through RT shadows, DF, or VSM has no atlas tile.
#ifdef RT_SHADOW_SAMPLING
	const bool rt_sampled = rt_shadow_light_sampled(spot_lights.data[idx].rt_shadow_flags);
#else
	const bool rt_sampled = false;
#endif
	const bool df_sampled = !rt_sampled && df_light_sampled(spot_lights.data[idx].vsm_flags);
	const bool vsm_sampled = !rt_sampled && !df_sampled && vsm_light_sampled(spot_lights.data[idx].vsm_flags);
#ifdef RT_SHADOW_SAMPLING
	if (rt_sampled && spot_attenuation > HALF_FLT_MIN && spot_lights.data[idx].shadow_opacity > 0.001) {
		float rt_shadow = rt_shadow_sample(spot_lights.data[idx].rt_shadow_tile_base, uvec2(NEXTGEO_FRAG_COORD.xy));
		shadow = mix(half(1.0), half(rt_shadow), half(spot_lights.data[idx].shadow_opacity));
	} else
#endif
	if (df_sampled && spot_attenuation > HALF_FLT_MIN && spot_lights.data[idx].shadow_opacity > 0.001) {
		float df_shadow = df_shadow_upsample(df_light_layer(spot_lights.data[idx].vsm_flags), -vertex.z, vec3(normal));
		shadow = mix(half(1.0), half(df_shadow), half(spot_lights.data[idx].shadow_opacity));
	} else if (vsm_sampled && spot_attenuation > HALF_FLT_MIN && spot_lights.data[idx].shadow_opacity > 0.001) {
		float vsm_shadow;
		if (sc_use_vsm_coarse()) {
			vsm_shadow = vsm_shadow_coarse_local(vsm_projection.data.counts.x, spot_lights.data[idx].vsm_map_base, spot_lights.data[idx].vsm_flags, vsm_world_position(vertex), vsm_world_normal(vec3(normal)), vsm_world_position(spot_lights.data[idx].position));
		} else {
			vsm_shadow = vsm_shadow_local(spot_lights.data[idx].vsm_map_base, spot_lights.data[idx].vsm_flags, vsm_world_position(vertex), vsm_world_normal(vec3(normal)), vsm_world_position(spot_lights.data[idx].position), spot_lights.data[idx].size, vec3(0.0), vec3(0.0), -vertex.z, vsm_pixel_noise());
		}
		shadow = mix(half(1.0), half(vsm_shadow), half(spot_lights.data[idx].shadow_opacity));
	}
	if (!rt_sampled && !vsm_sampled && !df_sampled && spot_attenuation > HALF_FLT_MIN && spot_lights.data[idx].shadow_opacity > 0.001) {
#else
	if (spot_attenuation > HALF_FLT_MIN && spot_lights.data[idx].shadow_opacity > 0.001) {
#endif
		vec3 normal_bias = vec3(normal) * light_length * spot_lights.data[idx].shadow_normal_bias * (1.0 - abs(dot(normal, light_rel_vec_norm)));

		//there is a shadowmap
		vec4 v = vec4(vertex + normal_bias, 1.0);

		vec4 splane = (spot_lights.data[idx].shadow_matrix * v);
		splane.z += spot_lights.data[idx].shadow_bias;
		splane /= splane.w;

		if (sc_use_light_soft_shadows() && spot_lights.data[idx].soft_shadow_size > 0.0) {
			//soft shadow

			//find blocker
			float z_norm = dot(spot_dir, -light_rel_vec) * spot_lights.data[idx].inv_radius;

			vec2 shadow_uv = splane.xy * spot_lights.data[idx].atlas_rect.zw + spot_lights.data[idx].atlas_rect.xy;

			float blocker_count = 0.0;
			float blocker_average = 0.0;

			mat2 disk_rotation;
			{
				float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
				float sr = sin(r);
				float cr = cos(r);
				disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
			}

			float uv_size = spot_lights.data[idx].soft_shadow_size * z_norm * spot_lights.data[idx].soft_shadow_scale;
			vec2 clamp_max = spot_lights.data[idx].atlas_rect.xy + spot_lights.data[idx].atlas_rect.zw;

			SPEC_CONSTANT_LOOP_ANNOTATION
			for (uint i = 0; i < sc_penumbra_shadow_samples(); i++) {
				vec2 suv = shadow_uv + (disk_rotation * scene_data_block.data.penumbra_shadow_kernel[i].xy) * uv_size;
				suv = clamp(suv, spot_lights.data[idx].atlas_rect.xy, clamp_max);
				float d = textureLod(sampler2D(shadow_atlas, SAMPLER_LINEAR_CLAMP), suv, 0.0).r;
				if (d > splane.z) {
					blocker_average += d;
					blocker_count += 1.0;
				}
			}

			if (blocker_count > 0.0) {
				//blockers found, do soft shadow
				blocker_average /= blocker_count;
				float penumbra = (-z_norm + blocker_average) / (1.0 - blocker_average);
				uv_size *= penumbra;

				shadow = half(0.0);

				SPEC_CONSTANT_LOOP_ANNOTATION
				for (uint i = 0; i < sc_penumbra_shadow_samples(); i++) {
					vec2 suv = shadow_uv + (disk_rotation * scene_data_block.data.penumbra_shadow_kernel[i].xy) * uv_size;
					suv = clamp(suv, spot_lights.data[idx].atlas_rect.xy, clamp_max);
					shadow += half(SHADOW_SAMPLE(shadow_atlas, vec3(suv, splane.z)));
				}

				shadow /= half(sc_penumbra_shadow_samples());
				shadow = mix(half(1.0), shadow, half(spot_lights.data[idx].shadow_opacity));

			} else {
				//no blockers found, so no shadow
				shadow = half(1.0);
			}
		} else {
			//hard shadow
			vec3 shadow_uv = vec3(splane.xy * spot_lights.data[idx].atlas_rect.zw + spot_lights.data[idx].atlas_rect.xy, splane.z);
			shadow = mix(half(1.0), sample_pcf_shadow(shadow_atlas, spot_lights.data[idx].soft_shadow_scale * scene_data_block.data.shadow_atlas_pixel_size, shadow_uv, taa_frame_count), half(spot_lights.data[idx].shadow_opacity));
		}
	}
#if defined(FORWARD_CLUSTERED_IES_ATLAS) && !defined(USING_MOBILE_RENDERER)
	// GodotNext: light_source_parity (#140). Per-light local contact shadows.
	shadow = apply_local_contact_shadow(shadow, spot_lights.data[idx].contact_shadow_sscs_index, spot_lights.data[idx].contact_shadow_casting_intensity, spot_lights.data[idx].contact_shadow_non_casting_intensity, screen_uv);
#endif
#endif // SHADOWS_DISABLED

	vec3 color = spot_lights.data[idx].color * spot_ies;

#ifdef LIGHT_TRANSMITTANCE_USED
	half transmittance_z = transmittance_depth;
	transmittance_color.a *= spot_attenuation;
#ifndef SHADOWS_DISABLED
#ifdef VSM_PROJECTION
	// GodotNext: vsm_projection (#222), rt_shadows (#141).
#ifdef RT_SHADOW_SAMPLING
	if (rt_sampled && spot_lights.data[idx].shadow_opacity > 0.001) {
		float rt_thickness = rt_shadow_sample_hit_dist(spot_lights.data[idx].rt_shadow_tile_base, uvec2(NEXTGEO_FRAG_COORD.xy));
		if (rt_thickness >= 0.0) {
			transmittance_z = half(rt_thickness);
		}
	} else
#endif
	if (vsm_sampled && spot_lights.data[idx].shadow_opacity > 0.001) {
		float vsm_thickness = vsm_transmittance_local(spot_lights.data[idx].vsm_map_base, spot_lights.data[idx].vsm_flags, vsm_world_position(vertex - vec3(normal) * spot_lights.data[idx].transmittance_bias), -vertex.z);
		if (vsm_thickness >= 0.0) {
			transmittance_z = half(vsm_thickness);
		}
	}
	if (!rt_sampled && !vsm_sampled && !df_sampled && spot_lights.data[idx].shadow_opacity > 0.001) {
#else
	if (spot_lights.data[idx].shadow_opacity > 0.001) {
#endif
		vec4 splane = (spot_lights.data[idx].shadow_matrix * vec4(vertex - vec3(normal) * spot_lights.data[idx].transmittance_bias, 1.0));
		splane /= splane.w;

		vec3 shadow_uv = vec3(splane.xy * spot_lights.data[idx].atlas_rect.zw + spot_lights.data[idx].atlas_rect.xy, splane.z);
		float shadow_z = textureLod(sampler2D(shadow_atlas, SAMPLER_LINEAR_CLAMP), shadow_uv.xy, 0.0).r;

		shadow_z = shadow_z * 2.0 - 1.0;
		float z_far = 1.0 / spot_lights.data[idx].inv_radius;
		float z_near = 0.01;
		shadow_z = 2.0 * z_near * z_far / (z_far + z_near - shadow_z * (z_far - z_near));

		//distance to light plane
		float z = dot(spot_dir, -light_rel_vec);
		transmittance_z = half(z - shadow_z);
	}
#endif // !SHADOWS_DISABLED
#endif // LIGHT_TRANSMITTANCE_USED

	if (sc_use_light_projector() && spot_lights.data[idx].projector_rect != vec4(0.0)) {
		vec4 splane = (spot_lights.data[idx].shadow_matrix * vec4(vertex, 1.0));
		splane /= splane.w;

		vec2 proj_uv = splane.xy * spot_lights.data[idx].projector_rect.zw;

		if (sc_projector_use_mipmaps()) {
			//ensure we have proper mipmaps
			vec4 splane_ddx = (spot_lights.data[idx].shadow_matrix * vec4(vertex + vertex_ddx, 1.0));
			splane_ddx /= splane_ddx.w;
			vec2 proj_uv_ddx = splane_ddx.xy * spot_lights.data[idx].projector_rect.zw - proj_uv;

			vec4 splane_ddy = (spot_lights.data[idx].shadow_matrix * vec4(vertex + vertex_ddy, 1.0));
			splane_ddy /= splane_ddy.w;
			vec2 proj_uv_ddy = splane_ddy.xy * spot_lights.data[idx].projector_rect.zw - proj_uv;

			vec4 proj = textureGrad(sampler2D(decal_atlas_srgb, light_projector_sampler), proj_uv + spot_lights.data[idx].projector_rect.xy, proj_uv_ddx, proj_uv_ddy);
			color *= proj.rgb * proj.a;
		} else {
			vec4 proj = textureLod(sampler2D(decal_atlas_srgb, light_projector_sampler), proj_uv + spot_lights.data[idx].projector_rect.xy, 0.0);
			color *= proj.rgb * proj.a;
		}
	}

#ifdef LIGHT_FUNCTION_ATLAS
	if (sc_use_light_function() && spot_lights.data[idx].light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(light_function_data.data[spot_lights.data[idx].light_function_index], light_function_atlas, SAMPLER_LINEAR_CLAMP, lf_world_position(vertex));
	}
#endif

	light_compute(normal, hvec3(light_rel_vec_norm), eye_vec, size, hvec3(color), false, spot_attenuation * shadow, f0, roughness, metallic, half(spot_lights.data[idx].specular_amount), albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
			backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
			transmittance_color,
			transmittance_depth,
			transmittance_boost,
			transmittance_z,
#endif
#ifdef LIGHT_RIM_USED
			rim * spot_attenuation, rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
			clearcoat, clearcoat_roughness, vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
			binormal, tangent, anisotropy,
#endif

			diffuse_light, specular_light);
}

// implementation of area lights with Linearly Transformed Cosines (LTC): https://eheitzresearch.wordpress.com/415-2/
void light_process_area(uint idx, vec3 vertex, hvec3 eye_vec, hvec3 normal, vec3 vertex_ddx, vec3 vertex_ddy, hvec3 f0, half roughness, half metallic, float taa_frame_count, hvec3 albedo, inout half alpha, vec2 screen_uv, hvec3 energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
		hvec3 backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
		hvec4 transmittance_color,
		half transmittance_depth,
		half transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
		half rim, half rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
		half clearcoat, half clearcoat_roughness, hvec3 vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
		hvec3 binormal, hvec3 tangent, half anisotropy,
#endif
		inout hvec3 diffuse_light, inout hvec3 specular_light) {
	half EPSILON = half(1e-7);
	hvec3 area_width = hvec3(area_lights.data[idx].area_width);
	hvec3 area_height = hvec3(area_lights.data[idx].area_height);

	if (dot(area_width, area_width) < EPSILON || dot(area_height, area_height) < EPSILON) { // area is 0
		return;
	}

	if (dot(area_lights.data[idx].direction, vertex - area_lights.data[idx].position) <= 0) {
		return; // vertex is behind light
	}

	half a_len = length(area_width);
	half b_len = length(area_height);
	half a_half_len = a_len / half(2.0);
	half b_half_len = b_len / half(2.0);
	hvec3 light_center = hvec3(area_lights.data[idx].position);
	hvec3 light_to_vert = hvec3(vertex) - light_center;
	hvec3 area_a_dir = normalize(area_width);
	hvec3 area_b_dir = normalize(area_height);
	hvec3 area_direction = hvec3(area_lights.data[idx].direction);
	hvec3 pos_local_to_light = hvec3(dot(light_to_vert, area_a_dir), dot(light_to_vert, area_b_dir), dot(light_to_vert, -area_direction));

	// GodotNext: light_source_parity (#140). Barn-door rect clipping (no-op when barn_door_angle >= 89.99 deg) & IES profile.
	vec2 barn_clip_min;
	vec2 barn_clip_max;
	if (!barn_door_clip_rect(vec3(pos_local_to_light), vec2(a_half_len, b_half_len), area_lights.data[idx].barn_door_angle_rad, area_lights.data[idx].barn_door_length, barn_clip_min, barn_clip_max)) {
		return;
	}
	float area_ies = 1.0;
	if (area_lights.data[idx].ies_layer != IES_LAYER_NONE) {
		area_ies = ies_attenuation(area_lights.data[idx], normalize(vec3(pos_local_to_light)));
		if (area_ies <= 0.0) {
			return;
		}
	}

	hvec3 closest_point_local_to_light = hvec3(clamp(float(pos_local_to_light.x), barn_clip_min.x, barn_clip_max.x), clamp(float(pos_local_to_light.y), barn_clip_min.y, barn_clip_max.y), 0.0);
	half dist = saturateHalf(length(closest_point_local_to_light - pos_local_to_light));

	half light_length = max(half(0.0), dist);
	half light_attenuation_raw = saturateHalf(get_omni_attenuation(light_length, area_lights.data[idx].inv_radius, area_lights.data[idx].attenuation));
	half light_attenuation_ltc = saturateHalf(light_attenuation_raw * light_length * light_length); // solid angle already decreases by inverse square, so attenuation power is 2.0 by default -> subtract 2.0
	half shadow = half(1.0);

#ifndef SHADOWS_DISABLED
	// Area light shadow.
#ifdef VSM_PROJECTION
	// GodotNext: vsm_projection (#222), vsm_fog_translucency (#215), df_shadows (#214), rt_shadows (#141).
	// A light sampled through RT shadows, DF, or VSM has no atlas tile.
#ifdef RT_SHADOW_SAMPLING
	const bool rt_sampled = rt_shadow_light_sampled(area_lights.data[idx].rt_shadow_flags);
#else
	const bool rt_sampled = false;
#endif
	const bool df_sampled = !rt_sampled && df_light_sampled(area_lights.data[idx].vsm_flags);
	const bool vsm_sampled = !rt_sampled && !df_sampled && vsm_light_sampled(area_lights.data[idx].vsm_flags);
#ifdef RT_SHADOW_SAMPLING
	if (rt_sampled && light_attenuation_raw > HALF_FLT_MIN && area_lights.data[idx].shadow_opacity > 0.001) {
		float rt_shadow = rt_shadow_sample(area_lights.data[idx].rt_shadow_tile_base, uvec2(NEXTGEO_FRAG_COORD.xy));
		shadow = mix(half(1.0), half(rt_shadow), half(area_lights.data[idx].shadow_opacity));
	} else
#endif
	if (df_sampled && light_attenuation_raw > HALF_FLT_MIN && area_lights.data[idx].shadow_opacity > 0.001) {
		float df_shadow = df_shadow_upsample(df_light_layer(area_lights.data[idx].vsm_flags), -vertex.z, vec3(normal));
		shadow = mix(half(1.0), half(df_shadow), half(area_lights.data[idx].shadow_opacity));
	} else if (vsm_sampled && light_attenuation_raw > HALF_FLT_MIN && area_lights.data[idx].shadow_opacity > 0.001) {
		float vsm_shadow;
		if (sc_use_vsm_coarse()) {
			vsm_shadow = vsm_shadow_coarse_local(vsm_projection.data.counts.x, area_lights.data[idx].vsm_map_base, area_lights.data[idx].vsm_flags, vsm_world_position(vertex), vsm_world_normal(vec3(normal)), vsm_world_position(area_lights.data[idx].position));
		} else {
			vsm_shadow = vsm_shadow_local(area_lights.data[idx].vsm_map_base, area_lights.data[idx].vsm_flags, vsm_world_position(vertex), vsm_world_normal(vec3(normal)), vsm_world_position(area_lights.data[idx].position), 0.0, vsm_world_vector(vec3(area_lights.data[idx].area_width)), vsm_world_vector(vec3(area_lights.data[idx].area_height)), -vertex.z, vsm_pixel_noise());
		}
		shadow = mix(half(1.0), half(vsm_shadow), half(area_lights.data[idx].shadow_opacity));
	}
	if (!rt_sampled && !vsm_sampled && !df_sampled && light_attenuation_raw > HALF_FLT_MIN && area_lights.data[idx].shadow_opacity > 0.001) {
#else
	if (light_attenuation_raw > HALF_FLT_MIN && area_lights.data[idx].shadow_opacity > 0.001) {
#endif
		// there is a shadowmap
		vec2 texel_size = scene_data_block.data.shadow_atlas_pixel_size;
		vec4 base_uv_rect = area_lights.data[idx].atlas_rect;
		base_uv_rect.xy += texel_size;
		base_uv_rect.zw -= texel_size * 2.0;

		vec3 local_vert = (area_lights.data[idx].shadow_matrix * vec4(vertex, 1.0)).xyz;

		float shadow_len = length(local_vert); //need to remember shadow len from here
		vec3 shadow_dir = normalize(local_vert);

		vec3 local_normal = normalize(mat3(area_lights.data[idx].shadow_matrix) * vec3(normal));
		vec3 normal_bias = local_normal * area_lights.data[idx].shadow_normal_bias * (1.0 - abs(dot(local_normal, shadow_dir)));

		float inv_center_range = area_lights.data[idx].cone_attenuation;

		if (sc_use_light_soft_shadows() && area_lights.data[idx].soft_shadow_size > 0.0) {
			//soft shadow

			//find blocker

			float blocker_count = 0.0;
			float blocker_average = 0.0;

			mat2 disk_rotation;
			{
				float r = quick_hash(NEXTGEO_FRAG_COORD.xy + vec2(taa_frame_count * 5.588238)) * 2.0 * M_PI; // GodotNext: nextgeo_shading_bins (#158)
				float sr = sin(r);
				float cr = cos(r);
				disk_rotation = mat2(vec2(cr, -sr), vec2(sr, cr));
			}

			vec3 basis_normal = shadow_dir;
			vec3 v0 = abs(basis_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
			vec3 tangent = normalize(cross(v0, basis_normal));
			vec3 bitangent = normalize(cross(tangent, basis_normal));
			float z_norm = 1.0 - shadow_len * inv_center_range;

			tangent *= area_lights.data[idx].soft_shadow_size * area_lights.data[idx].soft_shadow_scale;
			bitangent *= area_lights.data[idx].soft_shadow_size * area_lights.data[idx].soft_shadow_scale;

			SPEC_CONSTANT_LOOP_ANNOTATION
			for (uint i = 0; i < sc_penumbra_shadow_samples(); i++) {
				vec2 disk = disk_rotation * scene_data_block.data.penumbra_shadow_kernel[i].xy;

				vec3 pos = local_vert + tangent * disk.x + bitangent * disk.y;

				pos = normalize(pos);

				vec4 uv_rect = base_uv_rect;

				pos.z = 1.0 + abs(pos.z);
				pos.xy /= pos.z;

				pos.xy = pos.xy * 0.5 + 0.5;
				pos.xy = uv_rect.xy + pos.xy * uv_rect.zw;

				float d = textureLod(sampler2D(shadow_atlas, SAMPLER_LINEAR_CLAMP), pos.xy, 0.0).r;
				if (d > z_norm) {
					blocker_average += d;
					blocker_count += 1.0;
				}
			}

			if (blocker_count > 0.0) {
				//blockers found, do soft shadow
				blocker_average /= blocker_count;
				float penumbra = (-z_norm + blocker_average) / (1.0 - blocker_average);
				tangent *= penumbra;
				bitangent *= penumbra;

				z_norm += inv_center_range * area_lights.data[idx].shadow_bias;

				shadow = half(0.0);

				SPEC_CONSTANT_LOOP_ANNOTATION
				for (uint i = 0; i < sc_penumbra_shadow_samples(); i++) {
					vec2 disk = disk_rotation * scene_data_block.data.penumbra_shadow_kernel[i].xy;
					vec3 pos = local_vert + tangent * disk.x + bitangent * disk.y;

					pos = normalize(pos);
					pos = normalize(pos + normal_bias);

					vec4 uv_rect = base_uv_rect;

					pos.z = 1.0 + abs(pos.z);
					pos.xy /= pos.z;

					pos.xy = pos.xy * 0.5 + 0.5;
					pos.xy = uv_rect.xy + pos.xy * uv_rect.zw;
					shadow += half(SHADOW_SAMPLE(shadow_atlas, vec3(pos.xy, z_norm)));
				}

				shadow /= half(sc_penumbra_shadow_samples());
				shadow = mix(half(1.0), shadow, half(area_lights.data[idx].shadow_opacity));

			} else {
				//no blockers found, so no shadow
				shadow = half(1.0);
			}
		} else {
			vec4 uv_rect = base_uv_rect;

			vec3 shadow_sample = normalize(shadow_dir + normal_bias);

			shadow_sample.z = 1.0 + abs(shadow_sample.z);
			vec2 pos = shadow_sample.xy / shadow_sample.z;
			float depth = shadow_len - area_lights.data[idx].shadow_bias;
			depth *= inv_center_range;
			depth = 1.0 - depth;
			shadow = mix(half(1.0), sample_omni_pcf_shadow(shadow_atlas, area_lights.data[idx].soft_shadow_scale / shadow_sample.z, pos, uv_rect, vec2(0), depth, taa_frame_count), half(area_lights.data[idx].shadow_opacity));
		}
	}
#if defined(FORWARD_CLUSTERED_IES_ATLAS) && !defined(USING_MOBILE_RENDERER)
	// GodotNext: light_source_parity (#140). Per-light local contact shadows.
	shadow = apply_local_contact_shadow(shadow, area_lights.data[idx].contact_shadow_sscs_index, area_lights.data[idx].contact_shadow_casting_intensity, area_lights.data[idx].contact_shadow_non_casting_intensity, screen_uv);
#endif
#endif
	light_attenuation_ltc = light_attenuation_ltc * shadow;
	half light_attenuation = light_attenuation_raw * shadow;
	hvec3 color = hvec3(area_lights.data[idx].color * area_ies);
#ifdef LIGHT_FUNCTION_ATLAS
	if (sc_use_light_function() && area_lights.data[idx].light_function_index != LIGHT_FUNCTION_NONE) {
		color *= hvec3(light_function_mask(light_function_data.data[area_lights.data[idx].light_function_index], light_function_atlas, SAMPLER_LINEAR_CLAMP, lf_world_position(vertex)));
	}
#endif
	float max_mipmap = area_lights.data[idx].cone_angle;

	vec3 points[4];
	vec3 area_min_x = vec3(area_a_dir) * barn_clip_min.x;
	vec3 area_max_x = vec3(area_a_dir) * barn_clip_max.x;
	vec3 area_min_y = vec3(area_b_dir) * barn_clip_min.y;
	vec3 area_max_y = vec3(area_b_dir) * barn_clip_max.y;
	vec3 area_rel_center = area_lights.data[idx].position - vertex;
	points[0] = area_rel_center + area_min_x + area_min_y;
	points[1] = area_rel_center + area_max_x + area_min_y;
	points[2] = area_rel_center + area_max_x + area_max_y;
	points[3] = area_rel_center + area_min_x + area_max_y;
	vec4 area_tex_rect = barn_door_clip_texture_rect(area_lights.data[idx].projector_rect, vec2(a_half_len, b_half_len), barn_clip_min, barn_clip_max);

	float ltc_diffuse = 0.0;
	vec3 ltc_diffuse_tex_color = vec3(1.0);
	float ltc_specular = 0.0;
	vec3 ltc_specular_tex_color = vec3(1.0);
	vec2 ltc_fresnel = vec2(0.0);
	hvec3 fresnel_color = hvec3(0.0);
	ltc_evaluate(vec3(normal), vec3(eye_vec), mat3(1), points, area_tex_rect, max_mipmap, area_light_atlas, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP, ltc_diffuse, ltc_diffuse_tex_color);

#if !defined(SPECULAR_DISABLED) || (defined(LIGHT_CODE_USED) && defined(AREA_LIGHT_CODE_USED))
	ltc_evaluate_specular(vec3(normal), vec3(eye_vec), roughness, points, area_tex_rect, max_mipmap, area_light_atlas, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP, SAMPLER_LINEAR_CLAMP, ltc_lut1, ltc_lut2, ltc_specular, ltc_fresnel, ltc_specular_tex_color);
	half f90 = clamp(dot(f0, hvec3(50.0 * 0.33)), metallic, half(1.0));
	fresnel_color = f0 * max(half(ltc_fresnel.x), half(0.0)) + (f90 - f0) * max(half(ltc_fresnel.y), half(0.0));
#endif

#if defined(LIGHT_CODE_USED) && defined(AREA_LIGHT_CODE_USED)
	// Light is written by the user shader.
	mat4 inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef USING_MOBILE_RENDERER
	uint instance_index = draw_call.instance_index;
#else
	uint instance_index = instance_index_interp;
#endif

	mat4 read_model_matrix = transpose(mat4(instances.data[instance_index].transform[0],
			instances.data[instance_index].transform[1],
			instances.data[instance_index].transform[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#undef projection_matrix
#define projection_matrix scene_data_block.data.projection_matrix
#undef inv_projection_matrix
#define inv_projection_matrix scene_data_block.data.inv_projection_matrix

	vec2 read_viewport_size = scene_data_block.data.viewport_size;

#ifdef LIGHT_BACKLIGHT_USED
	vec3 backlight_highp = vec3(backlight);
#endif
	float roughness_highp = float(roughness);
	float metallic_highp = float(metallic);
	vec3 albedo_highp = vec3(albedo);
	float alpha_highp = float(alpha);
	vec3 normal_highp = vec3(normal);
	vec3 light_highp = (light_center - vertex) / light_length;
	vec3 view_highp = vec3(eye_vec);
	float specular_amount_highp = float(area_lights.data[idx].specular_amount);
	vec3 light_color_highp = vec3(color);
	float attenuation_highp = float(light_attenuation_ltc);
	vec3 diffuse_light_highp = vec3(diffuse_light);
	vec3 specular_light_highp = vec3(specular_light);
	bool is_directional = false;
	bool is_area = true;
	vec3 area_diffuse = ltc_diffuse * ltc_diffuse_tex_color;
	vec3 area_specular = ltc_specular * vec3(fresnel_color) * ltc_specular_tex_color;

#CODE : LIGHT

	alpha = half(alpha_highp);
	diffuse_light = hvec3(diffuse_light_highp);
	specular_light = hvec3(specular_light_highp);

#else
	half specular_amount = half(area_lights.data[idx].specular_amount);
	half area = a_len * b_len;
	half cc_attenuation = half(1.0);

#if defined(LIGHT_TRANSMITTANCE_USED) || defined(LIGHT_BACKLIGHT_USED) || defined(LIGHT_RIM_USED) || defined(DIFFUSE_TOON)
	hvec3 isotropic_light_color = hvec3(1.0); // independent of normal
	if (area_lights.data[idx].projector_rect != vec4(0.0)) {
		half lod = dist / sqrt(area);
		lod = log(half(2048.0) * lod) / log(half(3.0));

		hvec2 uv = (closest_point_local_to_light.xy + hvec2(a_half_len, b_half_len)) / hvec2(a_len, b_len);
		isotropic_light_color = hvec3(fetch_ltc_lod(vec2(hvec2(1.0) - uv), area_lights.data[idx].projector_rect, float(lod), max_mipmap, area_light_atlas, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP));
	}
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
	{
		half transmittance_z = transmittance_depth;
		transmittance_color.a *= light_attenuation_raw; // not including shadows or ltc falloff
#ifndef SHADOWS_DISABLED
#ifdef VSM_PROJECTION
		// GodotNext: vsm_projection (#222), rt_shadows (#141).
#ifdef RT_SHADOW_SAMPLING
		if (rt_sampled && area_lights.data[idx].shadow_opacity > 0.001) {
			float rt_thickness = rt_shadow_sample_hit_dist(area_lights.data[idx].rt_shadow_tile_base, uvec2(NEXTGEO_FRAG_COORD.xy));
			if (rt_thickness >= 0.0) {
				transmittance_z = half(rt_thickness);
			}
		} else
#endif
		if (vsm_sampled && area_lights.data[idx].shadow_opacity > 0.001) {
			float vsm_thickness = vsm_transmittance_local(area_lights.data[idx].vsm_map_base, area_lights.data[idx].vsm_flags, vsm_world_position(vertex - vec3(normal) * area_lights.data[idx].transmittance_bias), -vertex.z);
			if (vsm_thickness >= 0.0) {
				transmittance_z = half(vsm_thickness);
			}
		}
		if (!rt_sampled && !vsm_sampled && !df_sampled && area_lights.data[idx].shadow_opacity > 0.001) {
#else
		if (area_lights.data[idx].shadow_opacity > 0.001) {
#endif
			// Redo shadowmapping, but shrink the model a bit to avoid artifacts.
			vec2 texel_size = scene_data_block.data.shadow_atlas_pixel_size;
			vec4 uv_rect = area_lights.data[idx].atlas_rect;
			uv_rect.xy += texel_size;
			uv_rect.zw -= texel_size * 2.0;
			vec3 local_vert = (area_lights.data[idx].shadow_matrix * vec4(vertex - vec3(normal) * area_lights.data[idx].transmittance_bias, 1.0)).xyz;

			float shadow_len = length(local_vert); //need to remember shadow len from here
			vec3 shadow_sample = normalize(local_vert);

			shadow_sample.z = 1.0 + abs(shadow_sample.z);
			vec2 pos = shadow_sample.xy / shadow_sample.z;
			float inv_center_range = area_lights.data[idx].cone_attenuation;
			float depth = shadow_len * inv_center_range;
			depth = 1.0 - depth;

			pos = pos * 0.5 + 0.5;
			pos = uv_rect.xy + pos * uv_rect.zw;
			float shadow_z = textureLod(sampler2D(shadow_atlas, SAMPLER_LINEAR_CLAMP), pos, 0.0).r;
			transmittance_z = half((depth - shadow_z) / inv_center_range);
		}
#endif
		// transmission can't use ltc_diffuse_tex_color, because for backface pixels texture would have to be sampled for opposite normal direction.
#ifdef SSS_MODE_SKIN
		diffuse_light += SSS_skin(half(ltc_diffuse), transmittance_depth, transmittance_z, transmittance_boost, transmittance_color, color * isotropic_light_color * area);
#else
		diffuse_light += SSS(half(ltc_diffuse), transmittance_depth, transmittance_z, transmittance_boost, transmittance_color, color * isotropic_light_color * area);
#endif
	}
#endif //LIGHT_TRANSMITTANCE_USED

#if defined(LIGHT_BACKLIGHT_USED) || defined(LIGHT_RIM_USED) || defined(DIFFUSE_TOON) || defined(SPECULAR_TOON)
	float solid_angle = quad_solid_angle(points);
#endif

#if defined(LIGHT_CLEARCOAT_USED)
	vec3 cc_specular_tex_color = vec3(1.0);
	float cc_specular_ltc = 0.0;
	vec2 cc_fresnel;
	ltc_evaluate_specular(vec3(vertex_normal), vec3(eye_vec), sqrt(mix(0.001, 0.1, float(clearcoat_roughness))), points, area_tex_rect, max_mipmap, area_light_atlas, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP, SAMPLER_LINEAR_CLAMP, ltc_lut1, ltc_lut2, cc_specular_ltc, cc_fresnel, cc_specular_tex_color);
	half Fr = (half(0.04) * max(half(cc_fresnel.x), half(0.0)) + half(1.0 - 0.04) * max(half(cc_fresnel.y), half(0.0))) * clearcoat;
	cc_attenuation = half(1.0) - Fr;
	specular_light += half(cc_specular_ltc) * hvec3(cc_specular_tex_color) * Fr * color * light_attenuation_ltc * specular_amount;
#endif // LIGHT_CLEARCOAT_USED

	if (metallic < half(1.0)) {
#if defined(DIFFUSE_TOON)
		float backface_ltc_diffuse = 0.0;
		vec3 backface_ltc_tex_color_discard = vec3(1.0);
		ltc_evaluate(vec3(-normal), vec3(eye_vec), mat3(1), points, vec4(0.0), max_mipmap, area_light_atlas, SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP, backface_ltc_diffuse, backface_ltc_tex_color_discard);
		half NdotL = half((ltc_diffuse - backface_ltc_diffuse) / (max(solid_angle, 0.001) / M_PI));
		half diffuse_brdf_NL = smoothstep(-roughness, max(roughness, half(0.01)), NdotL) * half(1.0 / M_PI);
		diffuse_light += diffuse_brdf_NL * isotropic_light_color * color * area * light_attenuation * cc_attenuation;
#else
		diffuse_light += half(ltc_diffuse) * hvec3(ltc_diffuse_tex_color) * color * light_attenuation_ltc * cc_attenuation;
#endif // DIFFUSE_TOON

#if defined(LIGHT_BACKLIGHT_USED)
		// backlight can't use ltc_diffuse_tex_color, because for backface pixels texture would have to sampled for opposite normal direction.
		diffuse_light += color * max(half(solid_angle / M_PI) - half(ltc_diffuse), half(0.0)) * backlight * isotropic_light_color * light_attenuation_ltc;
#endif
	}

#if defined(LIGHT_RIM_USED) // same as for point lights
	half cNdotV = max(dot(normal, eye_vec), half(1e-4));
	half rim_light = pow(max(half(1e-4), half(1.0) - cNdotV), max(half(0.0), (half(1.0) - roughness) * half(16.0)));
	diffuse_light += rim_light * rim * mix(hvec3(1.0), albedo, rim_tint) * isotropic_light_color * color * half(solid_angle) * light_attenuation_ltc;
#endif

#if defined(SPECULAR_TOON)
	// If ltc_specular turns out to be not similar enough to RdotV, since its based on GGX, toon shading would need its own lookup-table.
	half mid = half(1.0) - roughness;
	mid *= mid;

	half RdotV = half(ltc_specular / (max(solid_angle, 0.001) / (M_PI)));
	half intensity = smoothstep(mid - roughness * half(0.5), mid + roughness * half(0.5), RdotV) * mid; // should we use specular tex color here?? or diffuse? or white?
	diffuse_light += intensity * hvec3(ltc_specular_tex_color) * color * area * light_attenuation * specular_amount; // write to diffuse_light, as in toon shading you generally want no reflection
#elif defined(SPECULAR_DISABLED)
	// do nothing
#else
	hvec3 spec = half(ltc_specular) * hvec3(ltc_specular_tex_color) * color * fresnel_color;
	specular_light += spec * specular_amount * light_attenuation_ltc * cc_attenuation;
#endif // SPECULAR_TOON

#endif // LIGHT_CODE_USED
}

#ifdef FORWARD_CLUSTERED_LIGHT_GRID

// GodotNext: light_grid (#139, plan §21.3 contract M2).
// Packed per-cell light-grid lookup and forward shading loop for non-MegaLights lights.
#define LIGHT_GRID_TILE_SHIFT 6u
#define LIGHT_GRID_FLAG_ENABLED 1u
#define LIGHT_GRID_FLAG_ORTHOGONAL 2u
#define LIGHT_GRID_TYPE_OMNI 0u
#define LIGHT_GRID_TYPE_SPOT 1u
#define LIGHT_GRID_TYPE_AREA 2u
#define LIGHT_GRID_ENTRY_INDEX_MASK 0xFFFFu
#define LIGHT_GRID_ENTRY_TYPE_SHIFT 16u
#define LIGHT_GRID_ENTRY_TYPE_MASK 3u
#define LIGHT_GRID_ENTRY_FLAG_MEGALIGHTS (1u << 18u)

bool light_grid_is_active() {
	return (implementation_data.light_grid_dims.w & LIGHT_GRID_FLAG_ENABLED) != 0u;
}

uint light_grid_cell_index(uvec2 p_screen_pixel, float p_linear_depth) {
	uvec2 tile = min(p_screen_pixel >> LIGHT_GRID_TILE_SHIFT, implementation_data.light_grid_dims.xy - uvec2(1u));
	float depth = max(p_linear_depth, implementation_data.light_grid_z_params.x);
	float z_f = ((implementation_data.light_grid_dims.w & LIGHT_GRID_FLAG_ORTHOGONAL) != 0u)
			? (depth * implementation_data.light_grid_z_params.z + implementation_data.light_grid_z_params.w)
			: (log(depth) * implementation_data.light_grid_z_params.z + implementation_data.light_grid_z_params.w);
	uint tz = uint(clamp(floor(z_f), 0.0, float(implementation_data.light_grid_dims.z - 1u)));
	return (tz * implementation_data.light_grid_dims.y + tile.y) * implementation_data.light_grid_dims.x + tile.x;
}

void light_grid_process_lights(bool p_skip_megalights, uvec2 p_screen_pixel, uint p_layer_mask, uint p_instance_flags, vec3 vertex, hvec3 eye_vec, hvec3 normal, vec3 vertex_ddx, vec3 vertex_ddy, hvec3 f0, half roughness, half metallic, float taa_frame_count, hvec3 albedo, inout half alpha, vec2 screen_uv, hvec3 energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
		hvec3 backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
		hvec4 transmittance_color,
		half transmittance_depth,
		half transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
		half rim, half rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
		half clearcoat, half clearcoat_roughness, hvec3 vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
		hvec3 binormal, hvec3 tangent, half anisotropy,
#endif
		inout hvec3 diffuse_light, inout hvec3 specular_light) {
	uint cell_idx = light_grid_cell_index(p_screen_pixel, -vertex.z);
	uvec2 cell_header = packed_light_grid_headers.data[cell_idx];
	uint offset = cell_header.x;
	uint count = cell_header.y;
	bool use_lightmap = bool(p_instance_flags & INSTANCE_FLAGS_USE_LIGHTMAP);

	for (uint k = 0u; k < count; k++) {
		uint entry = packed_light_grid_indices.data[offset + k];
		if (p_skip_megalights && (entry & LIGHT_GRID_ENTRY_FLAG_MEGALIGHTS) != 0u) {
			continue; // Handled by MegaLights on non-fallback opaque pixels.
		}
		uint ltype = (entry >> LIGHT_GRID_ENTRY_TYPE_SHIFT) & LIGHT_GRID_ENTRY_TYPE_MASK;
		uint light_index = entry & LIGHT_GRID_ENTRY_INDEX_MASK;

		if (ltype == LIGHT_GRID_TYPE_OMNI) {
			if (!bool(omni_lights.data[light_index].mask & p_layer_mask)) {
				continue;
			}
			if (omni_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && use_lightmap) {
				continue;
			}
			light_process_omni(light_index, vertex, eye_vec, normal, vertex_ddx, vertex_ddy, f0, roughness, metallic, taa_frame_count, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
					backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
					transmittance_color,
					transmittance_depth,
					transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
					rim,
					rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
					clearcoat, clearcoat_roughness, vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
					binormal, tangent, anisotropy,
#endif
					diffuse_light, specular_light);
		} else if (ltype == LIGHT_GRID_TYPE_SPOT) {
			if (!bool(spot_lights.data[light_index].mask & p_layer_mask)) {
				continue;
			}
			if (spot_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && use_lightmap) {
				continue;
			}
			light_process_spot(light_index, vertex, eye_vec, normal, vertex_ddx, vertex_ddy, f0, roughness, metallic, taa_frame_count, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
					backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
					transmittance_color,
					transmittance_depth,
					transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
					rim,
					rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
					clearcoat, clearcoat_roughness, vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
					binormal, tangent, anisotropy,
#endif
					diffuse_light, specular_light);
		} else if (ltype == LIGHT_GRID_TYPE_AREA && sc_cluster_has_area_light()) {
			if (!bool(area_lights.data[light_index].mask & p_layer_mask)) {
				continue;
			}
			if (area_lights.data[light_index].bake_mode == LIGHT_BAKE_STATIC && use_lightmap) {
				continue;
			}
			light_process_area(light_index, vertex, eye_vec, normal, vertex_ddx, vertex_ddy, f0, roughness, metallic, taa_frame_count, albedo, alpha, screen_uv, energy_compensation,
#ifdef LIGHT_BACKLIGHT_USED
					backlight,
#endif
#ifdef LIGHT_TRANSMITTANCE_USED
					transmittance_color,
					transmittance_depth,
					transmittance_boost,
#endif
#ifdef LIGHT_RIM_USED
					rim,
					rim_tint,
#endif
#ifdef LIGHT_CLEARCOAT_USED
					clearcoat, clearcoat_roughness, vertex_normal,
#endif
#ifdef LIGHT_ANISOTROPY_USED
					binormal, tangent, anisotropy,
#endif
					diffuse_light, specular_light);
		}
	}
}

#endif // FORWARD_CLUSTERED_LIGHT_GRID

void reflection_process(uint ref_index, vec3 vertex, hvec3 ref_vec, hvec3 normal, half roughness, hvec3 ambient_light,
#ifdef LIGHT_CLEARCOAT_USED
		hvec3 cc_ref_vec, half cc_roughness, inout hvec4 cc_reflection_accum,
#endif
		inout hvec4 ambient_accum, inout hvec4 reflection_accum) {
	vec3 box_extents = reflections.data[ref_index].box_extents;
	vec3 local_pos = (reflections.data[ref_index].local_matrix * vec4(vertex, 1.0)).xyz;

	if (any(greaterThan(abs(local_pos), box_extents))) { //out of the reflection box
		return;
	}

	half blend = half(1.0);
	if (reflections.data[ref_index].blend_distance != 0.0) {
		vec3 axis_blend_distance = min(vec3(reflections.data[ref_index].blend_distance), box_extents);
		vec3 blend_axes_highp = abs(local_pos) - box_extents + axis_blend_distance;
		hvec3 blend_axes = hvec3(blend_axes_highp / axis_blend_distance);
		blend_axes = clamp(half(1.0) - blend_axes, hvec3(0.0), hvec3(1.0));
		blend = pow(blend_axes.x * blend_axes.y * blend_axes.z, half(2.0));
	}

	vec2 border_size = scene_data_block.data.reflection_atlas_border_size;
	if (reflections.data[ref_index].intensity > 0.0 && reflection_accum.a < half(1.0)) { // compute reflection

		vec3 local_ref_vec = (reflections.data[ref_index].local_matrix * vec4(ref_vec, 0.0)).xyz;

		if (reflections.data[ref_index].box_project) { //box project

			vec3 nrdir = normalize(local_ref_vec);
			vec3 rbmax = (box_extents - local_pos) / nrdir;
			vec3 rbmin = (-box_extents - local_pos) / nrdir;

			vec3 rbminmax = mix(rbmin, rbmax, greaterThan(nrdir, vec3(0.0, 0.0, 0.0)));

			float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
			vec3 posonbox = local_pos + nrdir * fa;
			local_ref_vec = posonbox - reflections.data[ref_index].box_offset;
		}

		hvec4 reflection;
		half reflection_blend = max(half(0.0), blend - reflection_accum.a);

		float roughness_lod = sqrt(roughness) * MAX_ROUGHNESS_LOD;
		vec2 reflection_uv = vec3_to_oct_with_border(local_ref_vec, border_size);
		reflection.rgb = hvec3(textureLod(sampler2DArray(reflection_atlas, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(reflection_uv, reflections.data[ref_index].index), roughness_lod).rgb) * REFLECTION_MULTIPLIER;
		reflection.rgb *= half(reflections.data[ref_index].exposure_normalization);
		reflection.a = reflection_blend * half(reflections.data[ref_index].intensity);

		reflection.rgb *= reflection.a;

		reflection_accum += reflection;
	}

#ifdef LIGHT_CLEARCOAT_USED
	if (reflections.data[ref_index].intensity > 0.0 && cc_reflection_accum.a < half(1.0)) { // compute clearcoat reflection
		vec3 local_cc_ref_vec = (reflections.data[ref_index].local_matrix * vec4(cc_ref_vec, 0.0)).xyz;

		if (reflections.data[ref_index].box_project) { // Box project.

			vec3 nrdir = normalize(local_cc_ref_vec);
			vec3 rbmax = (box_extents - local_pos) / nrdir;
			vec3 rbmin = (-box_extents - local_pos) / nrdir;

			vec3 rbminmax = mix(rbmin, rbmax, greaterThan(nrdir, vec3(0.0, 0.0, 0.0)));

			float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
			vec3 posonbox = local_pos + nrdir * fa;
			local_cc_ref_vec = posonbox - reflections.data[ref_index].box_offset;
		}

		hvec4 cc_reflection;
		half cc_reflection_blend = max(half(0.0), blend - cc_reflection_accum.a);

		float cc_roughness_lod = sqrt(cc_roughness) * MAX_ROUGHNESS_LOD;
		vec2 cc_reflection_uv = vec3_to_oct_with_border(local_cc_ref_vec, border_size);
		cc_reflection.rgb = hvec3(textureLod(sampler2DArray(reflection_atlas, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(cc_reflection_uv, reflections.data[ref_index].index), cc_roughness_lod).rgb) * REFLECTION_MULTIPLIER;
		cc_reflection.rgb *= half(reflections.data[ref_index].exposure_normalization);
		cc_reflection.a = cc_reflection_blend * half(reflections.data[ref_index].intensity);

		cc_reflection.rgb *= cc_reflection.a;

		cc_reflection_accum += cc_reflection;
	}
#endif // LIGHT_CLEARCOAT_USED

	if (ambient_accum.a >= half(1.0)) {
		return;
	}

	switch (reflections.data[ref_index].ambient_mode) {
		case REFLECTION_AMBIENT_DISABLED: {
			//do nothing
		} break;
		case REFLECTION_AMBIENT_ENVIRONMENT: {
			vec3 local_amb_vec = (reflections.data[ref_index].local_matrix * vec4(normal, 0.0)).xyz;
			hvec4 ambient_out;
			half ambient_blend = max(half(0.0), blend - ambient_accum.a);

			float roughness_lod = MAX_ROUGHNESS_LOD;
			vec2 ambient_uv = vec3_to_oct_with_border(local_amb_vec, border_size);
			ambient_out.rgb = hvec3(textureLod(sampler2DArray(reflection_atlas, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), vec3(ambient_uv, reflections.data[ref_index].index), roughness_lod).rgb);
			ambient_out.rgb *= half(reflections.data[ref_index].exposure_normalization);
			ambient_out.a = ambient_blend * half(reflections.data[ref_index].intensity);
			ambient_out.rgb *= ambient_out.a;
			ambient_accum += ambient_out;
		} break;
		case REFLECTION_AMBIENT_COLOR: {
			hvec4 ambient_out;
			half ambient_blend = max(half(0.0), blend - ambient_accum.a);

			ambient_out.rgb = hvec3(reflections.data[ref_index].ambient);
			ambient_out.a = ambient_blend * half(reflections.data[ref_index].intensity);
			ambient_out.rgb *= ambient_out.a;
			ambient_accum += ambient_out;
		} break;
	}
}

half blur_shadow(half shadow) {
	return shadow;
#if 0
	//disabling for now, will investigate later
	float interp_shadow = shadow;
	if (gl_HelperInvocation) {
		interp_shadow = -4.0; // technically anything below -4 will do but just to make sure
	}

	uvec2 fc2 = uvec2(gl_FragCoord.xy);
	interp_shadow -= dFdx(interp_shadow) * (float(fc2.x & 1) - 0.5);
	interp_shadow -= dFdy(interp_shadow) * (float(fc2.y & 1) - 0.5);

	if (interp_shadow >= 0.0) {
		shadow = interp_shadow;
	}
	return shadow;
#endif
}
