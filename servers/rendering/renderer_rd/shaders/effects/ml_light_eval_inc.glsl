// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, GM-1, GM-2).
// Shared MegaLights direct-light evaluation, M1 input unpacking, octahedral ray packing,
// spatio-temporal blue-noise sampling, and packed light-grid lookup helpers.
// Consumed by ml_generate_samples.glsl, ml_reference.glsl, and ready for #146/#135/#143.

#ifndef ML_LIGHT_EVAL_INC_GLSL
#define ML_LIGHT_EVAL_INC_GLSL

#include "../light_data_inc.glsl"
#define MODE_VISIBILITY_RESOLVE
#include "../area_lights_inc.glsl"
#include "../ies_inc.glsl"
#include "../light_function_inc.glsl"

#ifndef MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS
#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8
#endif

#define ML_LIGHT_TYPE_OMNI 0u
#define ML_LIGHT_TYPE_SPOT 1u
#define ML_LIGHT_TYPE_AREA 2u
#define ML_LIGHT_TYPE_DIRECTIONAL 3u

#define ML_SAMPLE_INDEX_MASK 0xFFFFu
#define ML_SAMPLE_TYPE_SHIFT 16u
#define ML_SAMPLE_TYPE_MASK 0x3u
#define ML_SAMPLE_INVALID 0xFFFFFFFFu

#define ML_MAX_SAMPLES_PER_PIXEL 16u

#define ML_GUIDING_TILE_SIZE 8u
#define ML_GUIDING_TILE_SHIFT 3u
#define ML_GUIDING_MAX_LIGHTS 32u
#define ML_GUIDING_HEADER_WORDS 4u
#define ML_GUIDING_TILE_WORDS 36u
#define ML_GUIDING_LIGHT_MASK 0x3FFFFu
#define ML_GUIDING_COUNT_SHIFT 18u

#define ML_LIGHT_GRID_TILE_SHIFT 6u
#define ML_LIGHT_GRID_FLAG_ENABLED 1u
#define ML_LIGHT_GRID_FLAG_ORTHOGONAL 2u

#define ML_FLAG_DIRECTIONAL_LIGHTS (1u << 0u)
#define ML_FLAG_ORTHOGONAL (1u << 1u)
#define ML_FLAG_HAS_GUIDING_HISTORY (1u << 2u)
#define ML_FLAG_USE_LIGHT_FUNCTION (1u << 3u)
#define ML_FLAG_FORCE_VISIBLE_ONE (1u << 4u)
#define ML_FLAG_USE_SOFT_SHADOWS (1u << 5u)

#define ML_MODEL_FALLBACK 3u
#define ML_MODEL_SSS 1u
#define ML_FLAG_TRANSMISSION (1u << 3u)

#define ML_HIT_DIST_MISS 65504.0
#define ML_DEMOD_ALBEDO_FLOOR 0.01
#define ML_DEMOD_SPECULAR_FLOOR 0.01

// M1 F0 packing constants matching MegaLightsInputs (megalights_inputs.h).
#define ML_LUMA_R 0.30
#define ML_LUMA_G 0.59
#define ML_LUMA_B 0.11
#define ML_CB_SCALE 1.78
#define ML_CR_SCALE 1.40
#define ML_CHROMA_LEVELS 14.0
#define ML_CHROMA_BIAS 7.0

// std140, 256 bytes. Must match RendererRD::MegaLights::ParamsUBO.
struct MLSceneParams {
	mat4 inv_projection;
	vec4 inv_view_rows[3];
	uvec4 screen_sample_size; // xy = full internal_size, zw = sample_grid_size
	uvec4 grid_dims_flags; // xyz = packed_light_grid_dims, w = packed_light_grid_flags
	vec4 grid_z_params; // x = z_near, y = z_far, z = z_scale, w = z_bias
	uvec4 light_counts; // x = omni, y = spot, z = area, w = directional
	uvec4 sample_config; // x = spp, y = downsample_mode (0=full,1=half), z = frame_index, w = flags
	uvec4 guiding_tile_info; // x = tiles_x, y = tiles_y, z = layer_group_count, w = ref_frame_index
	vec4 thresholds; // x = falloff_cull_threshold, y = guiding_floor, z = camera_far, w = unused
	uvec4 pad_extra;
	uvec4 pad_extra2;
};

#ifdef ML_DECLARE_COMMON_BINDINGS

layout(set = 0, binding = 0, std140) uniform MLParamsUBO {
	MLSceneParams params;
};

layout(set = 0, binding = 1) uniform texture2D ml_depth_tex;
layout(set = 0, binding = 2) uniform texture2D ml_normal_roughness_tex;
layout(set = 0, binding = 3) uniform texture2D ml_albedo_tex;
layout(set = 0, binding = 4) uniform texture2D ml_material_tex;
layout(set = 0, binding = 5) uniform texture2D ml_dfg_lut_tex;
layout(set = 0, binding = 6) uniform texture2D ml_ltc_lut1_tex;
layout(set = 0, binding = 7) uniform texture2D ml_ltc_lut2_tex;
layout(set = 0, binding = 8) uniform texture2D ml_area_light_atlas_tex;
layout(set = 0, binding = 9) uniform texture2DArray ml_ies_profile_atlas_tex;
layout(set = 0, binding = 10) uniform texture2D ml_light_function_atlas_tex;
layout(set = 0, binding = 11) uniform texture2DArray ml_stbn_tex;

layout(set = 0, binding = 12) uniform sampler ml_sampler_nearest_clamp;
layout(set = 0, binding = 13) uniform sampler ml_sampler_linear_clamp;
layout(set = 0, binding = 14) uniform sampler ml_sampler_linear_mips_clamp;

layout(set = 0, binding = 15, std430) restrict readonly buffer MLOmniLights {
	LightData data[];
}
ml_omni_lights;

layout(set = 0, binding = 16, std430) restrict readonly buffer MLSpotLights {
	LightData data[];
}
ml_spot_lights;

layout(set = 0, binding = 17, std430) restrict readonly buffer MLAreaLights {
	LightData data[];
}
ml_area_lights;

layout(set = 0, binding = 18, std140) uniform MLDirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
ml_directional_lights;

layout(set = 0, binding = 19, std430) restrict readonly buffer MLLightFunctionData {
	LightFunctionEntry data[];
}
ml_light_function_data;

#endif // ML_DECLARE_COMMON_BINDINGS

struct MLSurface {
	vec3 view_pos;
	vec3 world_pos;
	vec3 view_dir;
	vec3 normal;
	vec3 world_normal;
	vec3 diffuse_albedo;
	float direct_ao;
	vec3 f0;
	float roughness;
	vec3 energy_comp;
	vec3 spec_env_brdf;
	uint shading_model;
	uint model_flags;
	uint layer_mask;
};

struct MLLightEval {
	vec3 diffuse;
	vec3 specular;
	float attenuation;
	vec3 ray_dir_ws;
	float ray_dist;
	bool valid;
};

float ml_luminance(vec3 p_c) {
	return dot(p_c, vec3(0.2126, 0.7152, 0.0722));
}

// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5).
// Demodulation factors matching Forward+'s multiscatter split-sum EnvBRDF (rr_specular_albedo)
// and diffuse albedo with safe floors so black/metallic surfaces never divide by zero and
// remodulation by the same factors preserves exact direct energy.
vec3 ml_diffuse_demod_factor(vec3 p_diffuse_albedo) {
	return max(p_diffuse_albedo, vec3(ML_DEMOD_ALBEDO_FLOOR));
}

vec3 ml_specular_demod_factor(vec3 p_spec_env_brdf) {
	return max(p_spec_env_brdf, vec3(ML_DEMOD_SPECULAR_FLOOR));
}

vec3 ml_unpack_visibility_rgb(uint p_packed) {
	return vec3(
				   float(p_packed & 0xFFu),
				   float((p_packed >> 8u) & 0xFFu),
				   float((p_packed >> 16u) & 0xFFu)) *
			(1.0 / 255.0);
}

uint ml_pack_sample_light(uint p_type, uint p_index) {
	return (p_index & ML_SAMPLE_INDEX_MASK) | ((p_type & ML_SAMPLE_TYPE_MASK) << ML_SAMPLE_TYPE_SHIFT);
}

uint ml_unpack_sample_light_type(uint p_packed) {
	return (p_packed >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
}

uint ml_unpack_sample_light_index(uint p_packed) {
	return p_packed & ML_SAMPLE_INDEX_MASK;
}

vec2 ml_oct_encode(vec3 p_dir) {
	float l1 = abs(p_dir.x) + abs(p_dir.y) + abs(p_dir.z);
	if (!(l1 > 1e-12)) {
		return vec2(0.0);
	}
	vec3 n = p_dir / l1;
	if (n.z < 0.0) {
		vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (vec2(1.0) - abs(n.yx)) * s;
	}
	return clamp(n.xy, vec2(-1.0), vec2(1.0));
}

vec3 ml_oct_decode(vec2 p_e) {
	vec3 n = vec3(p_e.x, p_e.y, 1.0 - abs(p_e.x) - abs(p_e.y));
	if (n.z < 0.0) {
		vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (vec2(1.0) - abs(n.yx)) * s;
	}
	return normalize(n);
}

uint ml_pack_oct(vec3 p_dir) {
	return packSnorm2x16(ml_oct_encode(p_dir));
}

vec3 ml_unpack_oct(uint p_word) {
	return ml_oct_decode(unpackSnorm2x16(p_word));
}

uvec2 ml_downsample_offset(uint p_phase) {
	uint p = p_phase & 3u;
	if (p == 1u) {
		return uvec2(1u, 1u);
	}
	if (p == 2u) {
		return uvec2(1u, 0u);
	}
	if (p == 3u) {
		return uvec2(0u, 1u);
	}
	return uvec2(0u, 0u);
}

uvec2 ml_sample_to_full_pixel(uvec2 p_sample_px, uvec2 p_full_size, bool p_downsample_half, uint p_frame_index) {
	if (!p_downsample_half) {
		return min(p_sample_px, p_full_size - uvec2(1u));
	}
	uvec2 off = ml_downsample_offset(p_frame_index);
	return min(p_sample_px * 2u + off, p_full_size - uvec2(1u));
}

// Fast 32-bit PCG hash for sub-quantization dithering and decorrelating per-light STBN offsets.
uint ml_pcg_hash(uint p_v) {
	uint state = p_v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// Continuous uniform float in [0, 1) with full 24-bit mantissa precision.
float ml_hash_to_float(uint p_h) {
	return float(p_h >> 8u) * (1.0 / 16777216.0);
}

vec2 ml_concentric_disk(vec2 p_u) {
	vec2 o = p_u * 2.0 - vec2(1.0);
	if (abs(o.x) < 1e-7 && abs(o.y) < 1e-7) {
		return vec2(0.0);
	}
	float r;
	float theta;
	if (abs(o.x) > abs(o.y)) {
		r = o.x;
		theta = (M_PI * 0.25) * (o.y / o.x);
	} else {
		r = o.y;
		theta = (M_PI * 0.5) - (M_PI * 0.25) * (o.x / o.y);
	}
	return vec2(r * cos(theta), r * sin(theta));
}

void ml_orthonormal_basis(vec3 p_n, out vec3 r_t, out vec3 r_b) {
	vec3 up = abs(p_n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	r_t = normalize(cross(up, p_n));
	r_b = cross(p_n, r_t);
}

vec3 ml_view_to_world_pos(vec4 p_inv_view_rows[3], vec3 p_view_pos) {
	vec4 p = vec4(p_view_pos, 1.0);
	return vec3(dot(p_inv_view_rows[0], p), dot(p_inv_view_rows[1], p), dot(p_inv_view_rows[2], p));
}

vec3 ml_view_to_world_dir(vec4 p_inv_view_rows[3], vec3 p_view_dir) {
	return vec3(dot(p_inv_view_rows[0].xyz, p_view_dir), dot(p_inv_view_rows[1].xyz, p_view_dir), dot(p_inv_view_rows[2].xyz, p_view_dir));
}

vec3 ml_unpack_f0(float p_luma_unorm, float p_tint_unorm) {
	uint tint_byte = uint(clamp(round(p_tint_unorm * 255.0), 0.0, 255.0));
	float cb = (float((tint_byte >> 4u) & 0xFu) - ML_CHROMA_BIAS) / ML_CHROMA_LEVELS;
	float cr = (float(tint_byte & 0xFu) - ML_CHROMA_BIAS) / ML_CHROMA_LEVELS;
	vec3 f0;
	f0.b = p_luma_unorm + cb * ML_CB_SCALE;
	f0.r = p_luma_unorm + cr * ML_CR_SCALE;
	f0.g = (p_luma_unorm - ML_LUMA_R * f0.r - ML_LUMA_B * f0.b) / ML_LUMA_G;
	return clamp(f0, vec3(0.0), vec3(1.0));
}

void ml_unpack_normal_roughness(vec4 p_nr, out vec3 r_normal, out float r_roughness) {
	float roughness = p_nr.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	r_roughness = clamp(roughness / (127.0 / 255.0), 0.0, 1.0);
	vec3 n = p_nr.xyz * 2.0 - vec3(1.0);
	float len2 = dot(n, n);
	r_normal = len2 > 1e-12 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

float ml_get_omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd; // nd^4
	nd = max(1.0 - nd, 0.0);
	nd *= nd; // nd^2
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

float ml_schlick_fresnel(float p_u) {
	float m = 1.0 - p_u;
	float m2 = m * m;
	return m2 * m2 * m;
}

float ml_d_ggx(float p_noh, float p_alpha) {
	float a = p_noh * p_alpha;
	float k = p_alpha / max(1.0 - p_noh * p_noh + a * a, 1e-7);
	return min(k * k * (1.0 / M_PI), 65504.0);
}

float ml_v_ggx(float p_nol, float p_nov, float p_alpha) {
	float denom = mix(2.0 * p_nol * p_nov, p_nol + p_nov, p_alpha);
	return min(0.5 / max(denom, 1e-7), 65504.0);
}

// Forward+ matching Burley diffuse + Schlick-GGX specular evaluation using M1 inputs.
void ml_eval_brdf(MLSurface p_s, vec3 p_L, float p_size_A, vec3 p_light_color, float p_attenuation, float p_specular_amount, out vec3 r_diffuse, out vec3 r_specular) {
	r_diffuse = vec3(0.0);
	r_specular = vec3(0.0);

	float NdotL = min(p_size_A + dot(p_s.normal, p_L), 1.0);
	float cNdotL = max(NdotL, 0.0);
	if (!(cNdotL > 0.0) || !(p_attenuation > 0.0)) {
		return;
	}

	float cNdotV = max(dot(p_s.normal, p_s.view_dir), 1e-4);
	vec3 H_sum = p_s.view_dir + p_L;
	float H_len2 = dot(H_sum, H_sum);
	vec3 H = H_len2 > 1e-12 ? H_sum * inversesqrt(H_len2) : p_s.normal;
	float cLdotH = clamp(p_size_A + dot(p_L, H), 0.0, 1.0);

	// Burley diffuse (ml_albedo.rgb already carries ALBEDO * (1 - METALLIC)).
	float FD90_minus_1 = 2.0 * cLdotH * cLdotH * p_s.roughness - 0.5;
	float FdV = 1.0 + FD90_minus_1 * ml_schlick_fresnel(cNdotV);
	float FdL = 1.0 + FD90_minus_1 * ml_schlick_fresnel(cNdotL);
	float diffuse_brdf_NL = (1.0 / M_PI) * FdV * FdL * cNdotL;
	r_diffuse = p_s.diffuse_albedo * p_light_color * (diffuse_brdf_NL * p_attenuation * p_s.direct_ao);

	// Schlick-GGX specular with DFG multiscatter energy compensation.
	if (p_s.roughness > 0.0 && p_specular_amount > 0.0) {
		float cNdotH = clamp(p_size_A + dot(p_s.normal, H), 0.0, 1.0);
		float alpha_ggx = p_s.roughness * p_s.roughness;
		float D = ml_d_ggx(cNdotH, alpha_ggx);
		float G = ml_v_ggx(cNdotL, cNdotV, alpha_ggx);
		float cLdotH5 = ml_schlick_fresnel(cLdotH);
		float f90 = clamp(dot(p_s.f0, vec3(50.0 * 0.33)), 0.0, 1.0);
		vec3 F = p_s.f0 + (vec3(f90) - p_s.f0) * cLdotH5;
		vec3 specular_brdf_NL = p_s.energy_comp * (cNdotL * D * G) * F;
		r_specular = specular_brdf_NL * p_light_color * (p_attenuation * p_specular_amount * p_s.direct_ao);
	}
}

#ifdef ML_DECLARE_COMMON_BINDINGS

// 64x64x32 STBN lookup + exact continuous 24-bit sub-quantization dithering.
// Within each 32-frame epoch, neighboring pixels evaluating the same salt (light/reservoir)
// receive adjacent spatio-temporal blue-noise texels; across epochs (>32 frames), a hashed
// toroidal shift decorrelates repeats so multi-thousand-frame Monte Carlo integration is unbiased.
vec2 ml_stbn_sample_uv(uvec2 p_pixel, uint p_frame, uint p_salt) {
	uint epoch = p_frame >> 5u;
	uint h_off = ml_pcg_hash(p_salt ^ (epoch * 0x9E3779B9u));
	uvec2 xy = (p_pixel + uvec2(h_off & 63u, (h_off >> 6u) & 63u)) & uvec2(63u);
	uint t = (p_frame + ((h_off >> 12u) & 31u)) & 31u;
	vec2 stbn = texelFetch(sampler2DArray(ml_stbn_tex, ml_sampler_nearest_clamp), ivec3(int(xy.x), int(xy.y), int(t)), 0).rg;

	uint h_sub0 = ml_pcg_hash(p_pixel.x ^ (p_pixel.y * 0x9E3779B9u) ^ (p_frame * 0x85EBCA6Bu) ^ (p_salt * 0xC2B2AE35u));
	uint h_sub1 = ml_pcg_hash(h_sub0 ^ 0x27D4EB2Fu);
	vec2 sub = vec2(ml_hash_to_float(h_sub0), ml_hash_to_float(h_sub1));
	vec2 bin = floor(stbn * 255.0 + vec2(0.5));
	return (bin + sub) * (1.0 / 256.0);
}

float ml_stbn_sample_u(uvec2 p_pixel, uint p_frame, uint p_salt) {
	return ml_stbn_sample_uv(p_pixel, p_frame, p_salt).x;
}

// Spatially blue-noise, temporally frame-decorrelated uniform in [0, 1) for adaptive WRS light
// selection. Within a frame, all pixels share the same hashed toroidal shift and STBN slice so
// neighboring pixels retain full 64x64 blue-noise stratification; across frames, re-hashing the
// shift/slice eliminates temporal anti-correlation with frame (t-1)'s tile guiding table so the
// weighted estimator stays mathematically unbiased.
float ml_stbn_select_u(uvec2 p_pixel, uint p_frame, uint p_salt) {
	uint h_off = ml_pcg_hash(p_salt ^ (p_frame * 0x9E3779B9u));
	uvec2 xy = (p_pixel + uvec2(h_off & 63u, (h_off >> 6u) & 63u)) & uvec2(63u);
	uint t = (h_off >> 12u) & 31u;
	float stbn = texelFetch(sampler2DArray(ml_stbn_tex, ml_sampler_nearest_clamp), ivec3(int(xy.x), int(xy.y), int(t)), 0).r;

	uint h_sub = ml_pcg_hash(p_pixel.x ^ (p_pixel.y * 0x9E3779B9u) ^ (p_frame * 0x85EBCA6Bu) ^ (p_salt * 0xC2B2AE35u));
	float sub = ml_hash_to_float(h_sub);
	float bin = floor(stbn * 255.0 + 0.5);
	return (bin + sub) * (1.0 / 256.0);
}

uint ml_light_grid_cell_index(uvec2 p_screen_pixel, float p_linear_depth) {
	uvec2 tile = min(p_screen_pixel >> ML_LIGHT_GRID_TILE_SHIFT, params.grid_dims_flags.xy - uvec2(1u));
	float depth = max(p_linear_depth, params.grid_z_params.x);
	float z_f = ((params.grid_dims_flags.w & ML_LIGHT_GRID_FLAG_ORTHOGONAL) != 0u)
			? (depth * params.grid_z_params.z + params.grid_z_params.w)
			: (log(depth) * params.grid_z_params.z + params.grid_z_params.w);
	uint tz = uint(clamp(floor(z_f), 0.0, float(params.grid_dims_flags.z - 1u)));
	return (tz * params.grid_dims_flags.y + tile.y) * params.grid_dims_flags.x + tile.x;
}

bool ml_unpack_surface(uvec2 p_full_px, uint p_layer_mask, out MLSurface r_s) {
	float depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(p_full_px), 0).r;
	if (!(depth > 0.0)) {
		return false;
	}

	vec4 ml_mat = texelFetch(sampler2D(ml_material_tex, ml_sampler_nearest_clamp), ivec2(p_full_px), 0);
	uint model_byte = uint(clamp(round(ml_mat.b * 255.0), 0.0, 255.0));
	uint shading_model = model_byte & 3u;
	if (shading_model == ML_MODEL_FALLBACK) {
		return false;
	}

	vec4 ml_alb = texelFetch(sampler2D(ml_albedo_tex, ml_sampler_nearest_clamp), ivec2(p_full_px), 0);
	vec4 nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(p_full_px), 0);

	vec2 uv = (vec2(p_full_px) + vec2(0.5)) / vec2(params.screen_sample_size.xy);
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - vec2(1.0), depth, 1.0);
	vec3 view_pos = view_h.xyz / view_h.w;
	vec3 world_pos = ml_view_to_world_pos(params.inv_view_rows, view_pos);

	bool orthogonal = (params.sample_config.w & ML_FLAG_ORTHOGONAL) != 0u;
	vec3 view_dir = orthogonal ? vec3(0.0, 0.0, 1.0) : normalize(-view_pos);

	vec3 normal;
	float roughness;
	ml_unpack_normal_roughness(nr, normal, roughness);
	vec3 world_normal = ml_view_to_world_dir(params.inv_view_rows, normal);
	float wn_len2 = dot(world_normal, world_normal);
	world_normal = wn_len2 > 1e-12 ? world_normal * inversesqrt(wn_len2) : vec3(0.0, 1.0, 0.0);

	vec3 f0 = ml_unpack_f0(ml_mat.r, ml_mat.g);
	float cNdotV = max(dot(normal, view_dir), 1e-4);
	vec2 env_brdf = textureLod(sampler2D(ml_dfg_lut_tex, ml_sampler_linear_clamp), vec2(cNdotV, 1.0 - roughness), 0.0).rg;
	vec3 energy_comp = vec3(1.0) + f0 * (1.0 / max(env_brdf.y, 1e-4) - 1.0);
	float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), 0.0, 1.0);
	vec3 spec_env_brdf = energy_comp * ((vec3(f90) - f0) * env_brdf.x + f0 * env_brdf.y);

	r_s.view_pos = view_pos;
	r_s.world_pos = world_pos;
	r_s.view_dir = view_dir;
	r_s.normal = normal;
	r_s.world_normal = world_normal;
	r_s.diffuse_albedo = ml_alb.rgb;
	r_s.direct_ao = ml_alb.a;
	r_s.f0 = f0;
	r_s.roughness = roughness;
	r_s.energy_comp = energy_comp;
	r_s.spec_env_brdf = spec_env_brdf;
	r_s.shading_model = shading_model;
	r_s.model_flags = model_byte;
	r_s.layer_mask = p_layer_mask;
	return true;
}

uint ml_read_layer_group_index(uvec2 p_full_px) {
	vec4 ml_mat = texelFetch(sampler2D(ml_material_tex, ml_sampler_nearest_clamp), ivec2(p_full_px), 0);
	return uint(clamp(round(ml_mat.a * 255.0), 0.0, 255.0));
}

MLLightEval ml_invalid_eval() {
	MLLightEval ev;
	ev.diffuse = vec3(0.0);
	ev.specular = vec3(0.0);
	ev.attenuation = 0.0;
	ev.ray_dir_ws = vec3(0.0, 1.0, 0.0);
	ev.ray_dist = 0.0;
	ev.valid = false;
	return ev;
}

void ml_compute_world_ray(vec3 p_surface_ws, vec3 p_target_vs, out vec3 r_dir_ws, out float r_dist) {
	vec3 target_ws = ml_view_to_world_pos(params.inv_view_rows, p_target_vs);
	vec3 delta = target_ws - p_surface_ws;
	float dist = length(delta);
	r_dist = dist;
	r_dir_ws = dist > 1e-6 ? delta / dist : vec3(0.0, 1.0, 0.0);
}

// 1. Point / Omni light evaluation (unshadowed radiance + optional disk shadow ray).
MLLightEval ml_eval_omni(LightData p_light, MLSurface p_s, float p_falloff_threshold, bool p_soft_shadows, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	MLLightEval ev = ml_invalid_eval();
	if ((p_light.mask & p_s.layer_mask) == 0u) {
		return ev;
	}

	vec3 light_rel = p_light.position - p_s.view_pos;
	float dist = length(light_rel);
	float att = ml_get_omni_attenuation(dist, p_light.inv_radius, p_light.attenuation);
	if (!(att > 0.0) || att < p_falloff_threshold) {
		return ev;
	}

	vec3 L = light_rel / max(dist, 1e-6);
	float ies = 1.0;
	if (p_light.ies_layer != IES_LAYER_NONE) {
		vec3 lx = vec3(p_light.area_width);
		vec3 ly = vec3(p_light.area_height);
		vec3 lz = cross(lx, ly);
		vec3 dir_ls = vec3(dot(-L, lx), dot(-L, ly), dot(-L, lz));
		ies = ies_sample_atlas(p_light.ies_layer, p_light.ies_scale, dir_ls, ml_ies_profile_atlas_tex, ml_sampler_linear_clamp);
		if (!(ies > 0.0)) {
			return ev;
		}
	}

	vec3 color = p_light.color * ies;
	if (p_use_lf && p_light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[p_light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_s.world_pos);
	}

	float size_A = 0.0;
	if (p_soft_shadows && p_light.size > 0.0) {
		float t = p_light.size / max(0.001, dist);
		size_A = max(1.0 - 1.0 / sqrt(1.0 + t * t), 0.0);
	}

	ml_eval_brdf(p_s, L, size_A, color, att, p_light.specular_amount, ev.diffuse, ev.specular);
	ev.attenuation = att;
	ev.valid = true;

	if (p_compute_ray) {
		vec3 target_vs = p_light.position;
		if (p_light.size > 0.0) {
			vec3 t_vec;
			vec3 b_vec;
			ml_orthonormal_basis(L, t_vec, b_vec);
			vec2 disk = ml_concentric_disk(p_u_emitter) * p_light.size;
			target_vs += t_vec * disk.x + b_vec * disk.y;
		}
		ml_compute_world_ray(p_s.world_pos, target_vs, ev.ray_dir_ws, ev.ray_dist);
	}
	return ev;
}

// 2. Spot light evaluation (unshadowed radiance + cone + IES + light function + disk shadow ray).
MLLightEval ml_eval_spot(LightData p_light, MLSurface p_s, float p_falloff_threshold, bool p_soft_shadows, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	MLLightEval ev = ml_invalid_eval();
	if ((p_light.mask & p_s.layer_mask) == 0u) {
		return ev;
	}

	vec3 light_rel = p_light.position - p_s.view_pos;
	float dist = length(light_rel);
	vec3 L = light_rel / max(dist, 1e-6);

	float att = ml_get_omni_attenuation(dist, p_light.inv_radius, p_light.attenuation);
	float cone_angle = p_light.cone_angle;
	float scos = max(dot(-L, p_light.direction), cone_angle);
	float spot_rim = max(1e-4, (1.0 - scos) / max(1.0 - cone_angle, 1e-6));
	att *= max(1.0 - pow(spot_rim, p_light.cone_attenuation), 0.0);
	if (!(att > 0.0) || att < p_falloff_threshold) {
		return ev;
	}

	float ies = 1.0;
	if (p_light.ies_layer != IES_LAYER_NONE) {
		vec3 lx = vec3(p_light.area_width);
		vec3 ly = vec3(p_light.area_height);
		vec3 lz = -p_light.direction;
		vec3 dir_ls = vec3(dot(-L, lx), dot(-L, ly), dot(-L, lz));
		ies = ies_sample_atlas(p_light.ies_layer, p_light.ies_scale, dir_ls, ml_ies_profile_atlas_tex, ml_sampler_linear_clamp);
		if (!(ies > 0.0)) {
			return ev;
		}
	}

	vec3 color = p_light.color * ies;
	if (p_use_lf && p_light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[p_light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_s.world_pos);
	}

	float size_A = 0.0;
	if (p_soft_shadows && p_light.size > 0.0) {
		float t = p_light.size / max(0.001, dist);
		size_A = max(1.0 - 1.0 / sqrt(1.0 + t * t), 0.0);
	}

	ml_eval_brdf(p_s, L, size_A, color, att, p_light.specular_amount, ev.diffuse, ev.specular);
	ev.attenuation = att;
	ev.valid = true;

	if (p_compute_ray) {
		vec3 target_vs = p_light.position;
		if (p_light.size > 0.0) {
			vec3 t_vec;
			vec3 b_vec;
			ml_orthonormal_basis(L, t_vec, b_vec);
			vec2 disk = ml_concentric_disk(p_u_emitter) * p_light.size;
			target_vs += t_vec * disk.x + b_vec * disk.y;
		}
		ml_compute_world_ray(p_s.world_pos, target_vs, ev.ray_dir_ws, ev.ray_dist);
	}
	return ev;
}

// Shared rect/area geometry setup (barn-door clipping, falloff cull, IES, light function, LTC points).
bool ml_prepare_rect(LightData p_light, MLSurface p_s, float p_falloff_threshold, bool p_use_lf, out vec3 r_points[4], out vec2 r_half_size, out vec2 r_clip_min, out vec2 r_clip_max, out vec3 r_area_a_dir, out vec3 r_area_b_dir, out vec3 r_color, out float r_att_raw, out float r_att_ltc) {
	if ((p_light.mask & p_s.layer_mask) == 0u) {
		return false;
	}

	vec3 area_width = vec3(p_light.area_width);
	vec3 area_height = vec3(p_light.area_height);
	if (dot(area_width, area_width) < 1e-7 || dot(area_height, area_height) < 1e-7) {
		return false;
	}
	if (dot(p_light.direction, p_s.view_pos - p_light.position) <= 0.0) {
		return false;
	}

	float a_len = length(area_width);
	float b_len = length(area_height);
	r_half_size = vec2(a_len * 0.5, b_len * 0.5);
	r_area_a_dir = area_width / a_len;
	r_area_b_dir = area_height / b_len;
	vec3 light_to_vert = p_s.view_pos - p_light.position;
	vec3 pos_local = vec3(dot(light_to_vert, r_area_a_dir), dot(light_to_vert, r_area_b_dir), dot(light_to_vert, -p_light.direction));

	if (!barn_door_clip_rect(pos_local, r_half_size, p_light.barn_door_angle_rad, p_light.barn_door_length, r_clip_min, r_clip_max)) {
		return false;
	}

	vec3 closest_local = vec3(clamp(pos_local.x, r_clip_min.x, r_clip_max.x), clamp(pos_local.y, r_clip_min.y, r_clip_max.y), 0.0);
	float dist = max(length(closest_local - pos_local), 0.0);
	r_att_raw = min(ml_get_omni_attenuation(dist, p_light.inv_radius, p_light.attenuation), 65504.0);
	if (!(r_att_raw > 0.0) || r_att_raw < p_falloff_threshold) {
		return false;
	}
	r_att_ltc = min(r_att_raw * dist * dist, 65504.0);

	float area_ies = 1.0;
	if (p_light.ies_layer != IES_LAYER_NONE) {
		area_ies = ies_sample_atlas(p_light.ies_layer, p_light.ies_scale, normalize(pos_local), ml_ies_profile_atlas_tex, ml_sampler_linear_clamp);
		if (!(area_ies > 0.0)) {
			return false;
		}
	}

	r_color = p_light.color * area_ies;
	if (p_use_lf && p_light.light_function_index != LIGHT_FUNCTION_NONE) {
		r_color *= light_function_mask(ml_light_function_data.data[p_light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_s.world_pos);
	}

	vec3 area_rel_center = p_light.position - p_s.view_pos;
	vec3 min_x = r_area_a_dir * r_clip_min.x;
	vec3 max_x = r_area_a_dir * r_clip_max.x;
	vec3 min_y = r_area_b_dir * r_clip_min.y;
	vec3 max_y = r_area_b_dir * r_clip_max.y;
	r_points[0] = area_rel_center + min_x + min_y;
	r_points[1] = area_rel_center + max_x + min_y;
	r_points[2] = area_rel_center + max_x + max_y;
	r_points[3] = area_rel_center + min_x + max_y;
	return true;
}

// 3. Untextured Rect / Area light evaluation (barn doors + IES + light function + LTC).
MLLightEval ml_eval_rect(LightData p_light, MLSurface p_s, float p_falloff_threshold, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	MLLightEval ev = ml_invalid_eval();
	vec3 points[4];
	vec2 half_size;
	vec2 clip_min;
	vec2 clip_max;
	vec3 area_a_dir;
	vec3 area_b_dir;
	vec3 color;
	float att_raw;
	float att_ltc;
	if (!ml_prepare_rect(p_light, p_s, p_falloff_threshold, p_use_lf, points, half_size, clip_min, clip_max, area_a_dir, area_b_dir, color, att_raw, att_ltc)) {
		return ev;
	}

	float ltc_diffuse = 0.0;
	vec3 ltc_diffuse_tex = vec3(1.0);
	ltc_evaluate(p_s.normal, p_s.view_dir, mat3(1.0), points, vec4(0.0), 0.0, ml_area_light_atlas_tex, ml_sampler_linear_mips_clamp, ltc_diffuse, ltc_diffuse_tex);

	float ltc_specular = 0.0;
	vec3 ltc_specular_tex = vec3(1.0);
	vec2 ltc_fresnel = vec2(0.0);
	if (p_light.specular_amount > 0.0) {
		ltc_evaluate_specular(p_s.normal, p_s.view_dir, p_s.roughness, points, vec4(0.0), 0.0, ml_area_light_atlas_tex, ml_sampler_linear_mips_clamp, ml_sampler_linear_clamp, ml_ltc_lut1_tex, ml_ltc_lut2_tex, ltc_specular, ltc_fresnel, ltc_specular_tex);
	}

	float f90 = clamp(dot(p_s.f0, vec3(50.0 * 0.33)), 0.0, 1.0);
	vec3 fresnel_color = p_s.f0 * max(ltc_fresnel.x, 0.0) + (vec3(f90) - p_s.f0) * max(ltc_fresnel.y, 0.0);

	ev.diffuse = p_s.diffuse_albedo * color * (ltc_diffuse * att_ltc * p_s.direct_ao);
	ev.specular = color * fresnel_color * (ltc_specular * p_light.specular_amount * att_ltc * p_s.direct_ao);
	ev.attenuation = att_raw;
	ev.valid = true;

	if (p_compute_ray) {
		vec2 sample_xy = mix(clip_min, clip_max, p_u_emitter);
		vec3 target_vs = p_light.position + area_a_dir * sample_xy.x + area_b_dir * sample_xy.y;
		ml_compute_world_ray(p_s.world_pos, target_vs, ev.ray_dir_ws, ev.ray_dist);
	}
	return ev;
}

// 4. Textured Rect / Area light evaluation (barn-door-clipped area_light_atlas + IES + light function + LTC).
MLLightEval ml_eval_textured_rect(LightData p_light, MLSurface p_s, float p_falloff_threshold, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	MLLightEval ev = ml_invalid_eval();
	vec3 points[4];
	vec2 half_size;
	vec2 clip_min;
	vec2 clip_max;
	vec3 area_a_dir;
	vec3 area_b_dir;
	vec3 color;
	float att_raw;
	float att_ltc;
	if (!ml_prepare_rect(p_light, p_s, p_falloff_threshold, p_use_lf, points, half_size, clip_min, clip_max, area_a_dir, area_b_dir, color, att_raw, att_ltc)) {
		return ev;
	}

	vec4 tex_rect = barn_door_clip_texture_rect(p_light.projector_rect, half_size, clip_min, clip_max);
	float max_mipmap = p_light.cone_angle;

	float ltc_diffuse = 0.0;
	vec3 ltc_diffuse_tex = vec3(1.0);
	ltc_evaluate(p_s.normal, p_s.view_dir, mat3(1.0), points, tex_rect, max_mipmap, ml_area_light_atlas_tex, ml_sampler_linear_mips_clamp, ltc_diffuse, ltc_diffuse_tex);

	float ltc_specular = 0.0;
	vec3 ltc_specular_tex = vec3(1.0);
	vec2 ltc_fresnel = vec2(0.0);
	if (p_light.specular_amount > 0.0) {
		ltc_evaluate_specular(p_s.normal, p_s.view_dir, p_s.roughness, points, tex_rect, max_mipmap, ml_area_light_atlas_tex, ml_sampler_linear_mips_clamp, ml_sampler_linear_clamp, ml_ltc_lut1_tex, ml_ltc_lut2_tex, ltc_specular, ltc_fresnel, ltc_specular_tex);
	}

	float f90 = clamp(dot(p_s.f0, vec3(50.0 * 0.33)), 0.0, 1.0);
	vec3 fresnel_color = p_s.f0 * max(ltc_fresnel.x, 0.0) + (vec3(f90) - p_s.f0) * max(ltc_fresnel.y, 0.0);

	ev.diffuse = p_s.diffuse_albedo * ltc_diffuse_tex * color * (ltc_diffuse * att_ltc * p_s.direct_ao);
	ev.specular = ltc_specular_tex * color * fresnel_color * (ltc_specular * p_light.specular_amount * att_ltc * p_s.direct_ao);
	ev.attenuation = att_raw;
	ev.valid = true;

	if (p_compute_ray) {
		vec2 sample_xy = mix(clip_min, clip_max, p_u_emitter);
		vec3 target_vs = p_light.position + area_a_dir * sample_xy.x + area_b_dir * sample_xy.y;
		ml_compute_world_ray(p_s.world_pos, target_vs, ev.ray_dir_ws, ev.ray_dist);
	}
	return ev;
}

MLLightEval ml_eval_area(LightData p_light, MLSurface p_s, float p_falloff_threshold, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	if (p_light.projector_rect != vec4(0.0)) {
		return ml_eval_textured_rect(p_light, p_s, p_falloff_threshold, p_use_lf, p_u_emitter, p_compute_ray);
	}
	return ml_eval_rect(p_light, p_s, p_falloff_threshold, p_use_lf, p_u_emitter, p_compute_ray);
}

// 5. Directional light evaluation (opt-in via rendering/megalights/directional_lights).
MLLightEval ml_eval_directional(DirectionalLightData p_light, MLSurface p_s, bool p_soft_shadows, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	MLLightEval ev = ml_invalid_eval();
	if ((p_light.mask & p_s.layer_mask) == 0u) {
		return ev;
	}

	vec3 color = p_light.color * p_light.energy;
	if (p_use_lf && p_light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[p_light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_s.world_pos);
	}

	float size_A = p_soft_shadows ? p_light.size : 0.0;
	ml_eval_brdf(p_s, p_light.direction, size_A, color, 1.0, p_light.specular, ev.diffuse, ev.specular);
	ev.attenuation = 1.0;
	ev.valid = true;

	if (p_compute_ray) {
		vec3 dir_vs = p_light.direction;
		if (p_light.size > 0.0) {
			float s_clamped = clamp(p_light.size, 0.0, 0.99);
			float tan_half = sqrt(s_clamped / max(2.0 - s_clamped, 1e-4));
			vec3 t_vec;
			vec3 b_vec;
			ml_orthonormal_basis(dir_vs, t_vec, b_vec);
			vec2 disk = ml_concentric_disk(p_u_emitter) * tan_half;
			dir_vs = normalize(dir_vs + t_vec * disk.x + b_vec * disk.y);
		}
		vec3 dir_ws = ml_view_to_world_dir(params.inv_view_rows, dir_vs);
		float dlen2 = dot(dir_ws, dir_ws);
		ev.ray_dir_ws = dlen2 > 1e-12 ? dir_ws * inversesqrt(dlen2) : vec3(0.0, 1.0, 0.0);
		ev.ray_dist = max(params.thresholds.z, 1000.0);
	}
	return ev;
}

MLLightEval ml_eval_packed_light(uint p_light_packed, MLSurface p_s, float p_falloff_threshold, bool p_soft_shadows, bool p_use_lf, vec2 p_u_emitter, bool p_compute_ray) {
	uint ltype = ml_unpack_sample_light_type(p_light_packed);
	uint lidx = ml_unpack_sample_light_index(p_light_packed);
	if (ltype == ML_LIGHT_TYPE_OMNI && lidx < params.light_counts.x) {
		return ml_eval_omni(ml_omni_lights.data[lidx], p_s, p_falloff_threshold, p_soft_shadows, p_use_lf, p_u_emitter, p_compute_ray);
	}
	if (ltype == ML_LIGHT_TYPE_SPOT && lidx < params.light_counts.y) {
		return ml_eval_spot(ml_spot_lights.data[lidx], p_s, p_falloff_threshold, p_soft_shadows, p_use_lf, p_u_emitter, p_compute_ray);
	}
	if (ltype == ML_LIGHT_TYPE_AREA && lidx < params.light_counts.z) {
		return ml_eval_area(ml_area_lights.data[lidx], p_s, p_falloff_threshold, p_use_lf, p_u_emitter, p_compute_ray);
	}
	if (ltype == ML_LIGHT_TYPE_DIRECTIONAL && lidx < params.light_counts.w && lidx < MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS) {
		return ml_eval_directional(ml_directional_lights.data[lidx], p_s, p_soft_shadows, p_use_lf, p_u_emitter, p_compute_ray);
	}
	return ml_invalid_eval();
}

#endif // ML_DECLARE_COMMON_BINDINGS

#endif // ML_LIGHT_EVAL_INC_GLSL
