// GodotNext: light_source_parity (#140, plan §21.3 contract M8, GM-11).
// Reusable IES photometric profile sampling and AreaLight3D barn-door rect clipping helpers.
// Shared across Forward+, volumetric fog, NextGI SRC relight, and ready for MegaLights (#142) / path tracer (#169).

#ifndef IES_INC_GLSL
#define IES_INC_GLSL

#define IES_LAYER_NONE 0xFFFFFFFFu
#define IES_TABLE_WIDTH 256.0
#define IES_TABLE_HEIGHT 64.0
#define IES_PI 3.14159265359
#define IES_TAU 6.28318530718

#define BARN_DOOR_OPEN_ANGLE_RAD 1.5706218 // 89.99 deg
#define BARN_DOOR_MIN_LENGTH 1e-4

// Converts a unit vector in light local space (forward = -Z, right = +X, up = +Y)
// to half-texel-centered UV coordinates in the 256 x 64 R16F IES profile table.
vec2 ies_direction_to_uv(vec3 p_dir_ls) {
	float cos_theta = clamp(-p_dir_ls.z, -1.0, 1.0);
	float theta = acos(cos_theta);
	float phi = atan(p_dir_ls.y, p_dir_ls.x);
	if (phi < 0.0) {
		phi += IES_TAU;
	}
	float u = (theta / IES_PI) * ((IES_TABLE_WIDTH - 1.0) / IES_TABLE_WIDTH) + (0.5 / IES_TABLE_WIDTH);
	float v = (phi / IES_TAU) * ((IES_TABLE_HEIGHT - 1.0) / IES_TABLE_HEIGHT) + (0.5 / IES_TABLE_HEIGHT);
	return vec2(u, v);
}

float ies_sample_atlas(uint p_layer, float p_scale, vec3 p_dir_ls, texture2DArray p_atlas, sampler p_sampler) {
	if (p_layer == IES_LAYER_NONE) {
		return 1.0;
	}
	vec2 uv = ies_direction_to_uv(p_dir_ls);
	float candela_norm = textureLod(sampler2DArray(p_atlas, p_sampler), vec3(uv, float(p_layer)), 0.0).r;
	return max(candela_norm, 0.0) * p_scale;
}

// Clips an AreaLight3D emitter rectangle [-p_half_size, +p_half_size] by the four barn-door planes
// as seen from shading point p_local in light local space (forward emission is -Z, so p_local.z < 0
// is in front of the emitter). Matches LightSourceParity::barn_door_clip_rect() and #139 culling.
// Returns false when the emitter is completely occluded by the doors.
bool barn_door_clip_rect(vec3 p_local, vec2 p_half_size, float p_angle_rad, float p_length, out vec2 r_min, out vec2 r_max) {
	r_min = -p_half_size;
	r_max = p_half_size;

	float f = -p_local.z;
	if (f <= 0.0) {
		return false;
	}
	if (p_angle_rad >= BARN_DOOR_OPEN_ANGLE_RAD || p_length <= BARN_DOOR_MIN_LENGTH) {
		return true;
	}

	float clamped_angle = clamp(p_angle_rad, 0.0, 1.5706963);
	float sin_a = sin(clamped_angle);
	float cos_a = cos(clamped_angle);
	float z_d = p_length * cos_a;
	if (z_d <= 1e-6) {
		return true;
	}

	if (f <= z_d) {
		float tan_a = sin_a / cos_a;
		vec2 lim = p_half_size + vec2(f * tan_a);
		return abs(p_local.x) <= lim.x && abs(p_local.y) <= lim.y;
	}

	float inv_dz = 1.0 / (f - z_d);
	vec2 d_tip = p_half_size + vec2(p_length * sin_a);

	vec2 c_min = max(-p_half_size, (-f * d_tip - z_d * p_local.xy) * inv_dz);
	vec2 c_max = min(p_half_size, (f * d_tip - z_d * p_local.xy) * inv_dz);
	if (c_min.x >= c_max.x || c_min.y >= c_max.y) {
		return false;
	}

	r_min = c_min;
	r_max = c_max;
	return true;
}

// Adjusts an AreaLight3D's texture atlas rect for a barn-door clipped sub-rectangle,
// matching fetch_ltc_lod's (1.0 - uv) convention in area_lights_inc.glsl.
vec4 barn_door_clip_texture_rect(vec4 p_tex_rect, vec2 p_half_size, vec2 p_clip_min, vec2 p_clip_max) {
	if (p_tex_rect == vec4(0.0) || p_half_size.x <= 1e-6 || p_half_size.y <= 1e-6) {
		return p_tex_rect;
	}
	vec2 inv_size = vec2(0.5) / p_half_size;
	vec2 uv_lo = clamp((p_clip_min + p_half_size) * inv_size, vec2(0.0), vec2(1.0));
	vec2 uv_hi = clamp((p_clip_max + p_half_size) * inv_size, vec2(0.0), vec2(1.0));
	return vec4(
			p_tex_rect.x + (1.0 - uv_hi.x) * p_tex_rect.z,
			p_tex_rect.y + (1.0 - uv_hi.y) * p_tex_rect.w,
			(uv_hi.x - uv_lo.x) * p_tex_rect.z,
			(uv_hi.y - uv_lo.y) * p_tex_rect.w);
}

#endif // IES_INC_GLSL
