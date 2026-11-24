// GodotNext: light_function_atlas (#138, plan §21.3 contract M8, GM-11, M-9).
// Reusable Light Function Atlas lookup helpers for Forward+, Volumetric Fog,
// NextGI SRC relight, ray-traced shadows (#141), and MegaLights (#142).

#ifndef LIGHT_FUNCTION_INC_GLSL
#define LIGHT_FUNCTION_INC_GLSL

#define LIGHT_FUNCTION_NONE 0xFFFFFFFFu
#define LIGHT_FUNCTION_MODE_DIRECTIONAL 0u
#define LIGHT_FUNCTION_MODE_OMNI 1u
#define LIGHT_FUNCTION_MODE_SPOT 2u
#define LIGHT_FUNCTION_MODE_AREA 3u
#define LIGHT_FUNCTION_FLAG_MODE_MASK 3u
#define LIGHT_FUNCTION_FLAG_RGBA (1u << 2u)

// std430, 80 bytes. Must match RendererRD::LightFunctionAtlasPacker::EntryGPU.
struct LightFunctionEntry {
	vec4 tile_rect; // xy = inset UV origin, zw = inset UV span (0 when inactive)
	vec4 world_to_light0; // row 0 of world->light affine transform
	vec4 world_to_light1; // row 1 of world->light affine transform
	vec4 world_to_light2; // row 2 of world->light affine transform
	vec3 scale_params; // directional/omni/area: 1/scale; spot: (cot_half/scale.x, cot_half/scale.y, 1/scale.z)
	uint flags; // bits 0..1: mode, bit 2: RGBA8 format
};

vec2 light_function_oct_wrap(vec2 p_v) {
	vec2 s = vec2(p_v.x >= 0.0 ? 1.0 : -1.0, p_v.y >= 0.0 ? 1.0 : -1.0);
	return (vec2(1.0) - abs(p_v.yx)) * s;
}

vec2 light_function_vec3_to_oct(vec3 p_n) {
	float l1 = abs(p_n.x) + abs(p_n.y) + abs(p_n.z);
	if (l1 <= 1e-12) {
		return vec2(0.5);
	}
	vec3 n = p_n / l1;
	n.xy = (n.z >= 0.0) ? n.xy : light_function_oct_wrap(n.xy);
	return n.xy * 0.5 + 0.5;
}

// Projects a world-space point into the light's atlas tile UV.
// Returns false when the point lies behind a spot light (caller yields vec3(0.0)).
bool light_function_project_uv(LightFunctionEntry p_entry, vec3 p_world_pos, out vec2 r_atlas_uv) {
	vec3 local_pos = vec3(
			dot(p_entry.world_to_light0.xyz, p_world_pos) + p_entry.world_to_light0.w,
			dot(p_entry.world_to_light1.xyz, p_world_pos) + p_entry.world_to_light1.w,
			dot(p_entry.world_to_light2.xyz, p_world_pos) + p_entry.world_to_light2.w);

	uint mode = p_entry.flags & LIGHT_FUNCTION_FLAG_MODE_MASK;
	vec2 uv;
	if (mode == LIGHT_FUNCTION_MODE_DIRECTIONAL || mode == LIGHT_FUNCTION_MODE_AREA) {
		uv = fract(local_pos.xy * p_entry.scale_params.xy + vec2(0.5));
	} else if (mode == LIGHT_FUNCTION_MODE_OMNI) {
		vec3 scaled = local_pos * p_entry.scale_params;
		float len2 = dot(scaled, scaled);
		vec3 dir = len2 > 1e-12 ? scaled * inversesqrt(len2) : vec3(0.0, 0.0, -1.0);
		uv = light_function_vec3_to_oct(dir);
	} else {
		// Spot light shines along -Z in light local space.
		float depth = -local_pos.z;
		if (depth <= 1e-5) {
			r_atlas_uv = vec2(0.0);
			return false;
		}
		vec2 ndc = (local_pos.xy / depth) * p_entry.scale_params.xy;
		uv = clamp(vec2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5), vec2(0.0), vec2(1.0));
	}

	r_atlas_uv = p_entry.tile_rect.xy + clamp(uv, vec2(0.0), vec2(1.0)) * p_entry.tile_rect.zw;
	return true;
}

vec3 light_function_mask(LightFunctionEntry p_entry, texture2D p_atlas, sampler p_sampler, vec3 p_world_pos) {
	if (p_entry.tile_rect.z <= 0.0 || p_entry.tile_rect.w <= 0.0) {
		return vec3(1.0);
	}
	vec2 atlas_uv;
	if (!light_function_project_uv(p_entry, p_world_pos, atlas_uv)) {
		return vec3(0.0);
	}
	vec4 texel = textureLod(sampler2D(p_atlas, p_sampler), atlas_uv, 0.0);
	return (p_entry.flags & LIGHT_FUNCTION_FLAG_RGBA) != 0u ? texel.rgb : texel.rrr;
}

vec3 light_function_mask_combined(LightFunctionEntry p_entry, sampler2D p_atlas, vec3 p_world_pos) {
	if (p_entry.tile_rect.z <= 0.0 || p_entry.tile_rect.w <= 0.0) {
		return vec3(1.0);
	}
	vec2 atlas_uv;
	if (!light_function_project_uv(p_entry, p_world_pos, atlas_uv)) {
		return vec3(0.0);
	}
	vec4 texel = textureLod(p_atlas, atlas_uv, 0.0);
	return (p_entry.flags & LIGHT_FUNCTION_FLAG_RGBA) != 0u ? texel.rgb : texel.rrr;
}

#endif // LIGHT_FUNCTION_INC_GLSL
