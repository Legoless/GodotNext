#[compute]

#version 450

// GodotNext: ps_lumen_lite (#183). Irradiance-field gather for Lumen Lite.
// Mode 0 packs WRC levels 0-1 into the probe buffer (hardware tier).
// Mode 1 interpolates at half resolution. Mode 2 upsamples into the GI buffers.
// No ray query and no SSBO length: this shader stays valid on the software tier.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// GodotNext: nextgi_view_math. One reconstruction for every NextGI pass.
#include "nextgi_view_inc.glsl"

layout(push_constant, std430) uniform LitePush {
	uint mode;
	uint probe_base;
	uint pad0;
	uint pad1;
}
pc;

struct LiteParams {
	mat4 inv_view_proj;
	vec4 camera;
	vec4 screen;
	vec4 depth_info;
	vec4 rules;
};

layout(set = 0, binding = 0, std430) buffer LiteProbes {
	vec4 data[8192];
}
probes;

layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 3) uniform image2D half_image;
layout(rgba16f, set = 0, binding = 4) uniform image2D ambient_image;
layout(rgba16f, set = 0, binding = 5) uniform image2D reflection_image;

layout(set = 0, binding = 6, std430) readonly buffer LiteParamsBuffer {
	LiteParams data;
}
params_buffer;

layout(set = 0, binding = 7) uniform sampler2D wrc_radiance;
layout(set = 0, binding = 8) uniform sampler2D wrc_visibility;
layout(set = 0, binding = 9) uniform sampler3D wrc_offsets;
layout(set = 0, binding = 10) uniform sampler2D wrc_mip2;

shared vec3 pack_samples[64];

LiteParams lite_params() {
	return params_buffer.data;
}

vec3 lite_unproject(ivec2 pixel, float depth, vec2 internal_size) {
	LiteParams params = lite_params();
	vec2 uv = (vec2(pixel) + vec2(0.5)) / max(internal_size, vec2(1.0));
	return nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
}

ivec2 lite_oct_texel(vec3 direction) {
	vec3 n = direction;
	float manhattan = abs(n.x) + abs(n.y) + abs(n.z);
	if (manhattan > 1e-6) {
		n /= manhattan;
	}
	if (n.z < 0.0) {
		float x = (1.0 - abs(n.y)) * sign(n.x);
		float y = (1.0 - abs(n.x)) * sign(n.y);
		n.x = x;
		n.y = y;
	}
	vec2 uv = n.xy * 0.5 + 0.5;
	return ivec2(clamp(uv * 8.0, vec2(0.0), vec2(7.0)));
}

vec3 lite_probe_position(uint probe, vec3 camera) {
	uint level = min(probe / 4096u, 1u);
	uint in_level = probe % 4096u;
	float spacing = level == 0u ? 1.0 : 2.0;
	vec3 cell = vec3(float(in_level % 16u), float((in_level / 16u) % 16u), float(in_level / 256u));
	vec3 origin = floor(camera / spacing) * spacing;
	return origin + (cell - vec3(7.5)) * spacing;
}

void lite_pack() {
	LiteParams params = lite_params();
	uint local = gl_LocalInvocationIndex;
	uint probe = (pc.probe_base + gl_WorkGroupID.x) % 8192u;
	ivec2 local_texel = ivec2(int(local % 8u), int(local / 8u));
	uvec2 tile = uvec2(probe % 128u, probe / 128u);
	ivec2 texel = ivec2(tile) * 10 + ivec2(1, 1) + local_texel;
	pack_samples[local] = texelFetch(wrc_radiance, texel, 0).rgb;
	barrier();
	if (local == 0u) {
		vec3 average = vec3(0.0);
		for (uint i = 0u; i < 64u; i++) {
			average += pack_samples[i];
		}
		average *= 1.0 / 64.0;
		uint in_level = probe % 4096u;
		uint level = probe / 4096u;
		ivec3 cell = ivec3(int(in_level % 16u), int((in_level / 16u) % 16u), int(in_level / 256u + level * 16u));
		float offset_w = texelFetch(wrc_offsets, cell, 0).a;
		float alpha = offset_w > 1.5 ? 1.0 : 0.0;
		probes.data[probe] = vec4(average, alpha);
	}
}

void lite_interpolate() {
	LiteParams params = lite_params();
	ivec2 half_pixel = ivec2(gl_GlobalInvocationID.xy);
	vec2 half_size = max(params.screen.zw, vec2(1.0));
	if (half_pixel.x >= int(half_size.x) || half_pixel.y >= int(half_size.y)) {
		return;
	}
	vec2 internal_size = max(params.depth_info.xy, vec2(1.0));
	vec2 gi_size = max(params.screen.xy, vec2(1.0));
	vec2 gi_pixel = (vec2(half_pixel) + vec2(0.5)) / half_size * gi_size;
	ivec2 depth_pixel = ivec2(clamp(gi_pixel / gi_size * internal_size, vec2(0.0), internal_size - vec2(1.0)));
	float depth = params.depth_info.z > 0.5 ? texelFetch(depth_tex, depth_pixel, 0).r : 0.0;
	if (params.depth_info.z < 0.5 || depth <= 0.0001) {
		imageStore(half_image, half_pixel, vec4(0.0));
		return;
	}
	vec3 world = lite_unproject(depth_pixel, depth, internal_size);
	vec3 normal = normalize(params.camera.xyz - world);
	bool have_normal = false;
	if (params.depth_info.w > 0.5) {
		vec3 encoded = texelFetch(normal_roughness_tex, depth_pixel, 0).rgb;
		if (dot(encoded, encoded) > 1e-8) {
			normal = normalize(encoded * 2.0 - 1.0);
			have_normal = true;
		}
	}
	if (!have_normal && dot(normal, normal) <= 1e-8) {
		normal = vec3(0.0, 1.0, 0.0);
	}
	float camera_distance = length(world - params.camera.xyz);
	uint level = camera_distance < 8.0 ? 0u : 1u;
	float spacing = level == 0u ? 1.0 : 2.0;
	vec3 origin = floor(params.camera.xyz / spacing) * spacing;
	vec3 grid = (world - origin) / spacing + vec3(7.5);
	vec3 base_f = floor(grid);
	vec3 frac = grid - base_f;
	ivec3 base = ivec3(base_f);
	vec3 color = vec3(0.0);
	float weight_sum = 0.0;
	bool pushed = false;
	for (uint corner = 0u; corner < 8u; corner++) {
		ivec3 delta = ivec3(int(corner & 1u), int((corner >> 1u) & 1u), int((corner >> 2u) & 1u));
		ivec3 cell = base + delta;
		if (any(lessThan(cell, ivec3(0))) || any(greaterThan(cell, ivec3(15)))) {
			continue;
		}
		uint probe = level * 4096u + uint(cell.x) + uint(cell.y) * 16u + uint(cell.z) * 256u;
		vec4 probe_data = probes.data[probe];
		if (probe_data.a > params.rules.x) {
			continue;
		}
		vec3 probe_pos = origin + (vec3(cell) - vec3(7.5)) * spacing;
		bool behind = dot(probe_pos - world, normal) < 0.0;
		if (behind && dot(normal, normal) > 1e-8) {
			probe_pos += normalize(normal) * (params.rules.y * spacing);
			pushed = true;
		}
		float weight = (delta.x == 0 ? (1.0 - frac.x) : frac.x);
		weight *= delta.y == 0 ? (1.0 - frac.y) : frac.y;
		weight *= delta.z == 0 ? (1.0 - frac.z) : frac.z;
		float visibility = clamp(1.0 - probe_data.a, 0.0, 1.0);
		if (params.camera.w > 0.5) {
			vec3 to_pixel = world - probe_pos;
			float hit_distance = length(to_pixel);
			vec3 direction = hit_distance > 1e-4 ? to_pixel / hit_distance : vec3(0.0, 1.0, 0.0);
			uvec2 tile = uvec2(probe % 128u, probe / 128u);
			ivec2 visibility_texel = ivec2(tile) * 8 + lite_oct_texel(direction);
			vec2 moments = texelFetch(wrc_visibility, visibility_texel, 0).rg;
			if (moments.x == 0.0 && moments.y == 0.0) {
				visibility = 1.0;
			} else if (hit_distance <= moments.x) {
				visibility = 1.0;
			} else {
				float variance = max(moments.y - moments.x * moments.x, 0.0);
				float delta_t = hit_distance - moments.x;
				visibility = variance / (variance + delta_t * delta_t);
			}
		}
		weight *= visibility;
		color += probe_data.rgb * weight;
		weight_sum += weight;
	}
	vec3 out_color = weight_sum > 0.0 ? color / weight_sum : vec3(0.0);
	float out_alpha = weight_sum > 0.0 ? (pushed ? 0.5 : 1.0) : 0.0;
	imageStore(half_image, half_pixel, vec4(out_color, out_alpha));
}

void lite_upsample() {
	LiteParams params = lite_params();
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	vec2 gi_size = max(params.screen.xy, vec2(1.0));
	if (pixel.x >= int(gi_size.x) || pixel.y >= int(gi_size.y)) {
		return;
	}
	vec2 internal_size = max(params.depth_info.xy, vec2(1.0));
	ivec2 depth_pixel = ivec2(clamp((vec2(pixel) + vec2(0.5)) / gi_size * internal_size, vec2(0.0), internal_size - vec2(1.0)));
	float depth = params.depth_info.z > 0.5 ? texelFetch(depth_tex, depth_pixel, 0).r : 0.0;
	if (params.depth_info.z < 0.5 || depth <= 0.0001) {
		imageStore(ambient_image, pixel, vec4(0.0));
		imageStore(reflection_image, pixel, vec4(0.0));
		return;
	}
	vec3 world = lite_unproject(depth_pixel, depth, internal_size);
	vec4 encoded = params.depth_info.w > 0.5 ? texelFetch(normal_roughness_tex, depth_pixel, 0) : vec4(0.0);
	vec3 normal = encoded.rgb * 2.0 - 1.0;
	float roughness = 0.0;
	if (dot(encoded.rgb, encoded.rgb) > 1e-8) {
		normal = normalize(normal);
		float rough_w = encoded.a > 0.5 ? 1.0 - encoded.a : encoded.a;
		roughness = rough_w * (255.0 / 127.0);
	} else {
		normal = normalize(params.camera.xyz - world);
	}
	vec2 half_size = max(params.screen.zw, vec2(1.0));
	ivec2 half_center = ivec2(clamp((vec2(pixel) + vec2(0.5)) / gi_size * half_size, vec2(0.0), half_size - vec2(1.0)));
	float center_depth = depth;
	vec3 accum = vec3(0.0);
	float weight_sum = 0.0;
	float validity = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 sample_pixel = clamp(half_center + ivec2(x, y), ivec2(0), ivec2(half_size) - ivec2(1));
			vec4 sample_color = imageLoad(half_image, sample_pixel);
			ivec2 sample_depth_pixel = ivec2(clamp((vec2(sample_pixel) + vec2(0.5)) / half_size * internal_size, vec2(0.0), internal_size - vec2(1.0)));
			float sample_depth = texelFetch(depth_tex, sample_depth_pixel, 0).r;
			vec3 sample_normal = normal;
			if (params.depth_info.w > 0.5) {
				vec3 sample_encoded = texelFetch(normal_roughness_tex, sample_depth_pixel, 0).rgb;
				if (dot(sample_encoded, sample_encoded) > 1e-8) {
					sample_normal = normalize(sample_encoded * 2.0 - 1.0);
				}
			}
			float depth_weight = exp(-abs(sample_depth - center_depth) * 32.0);
			float normal_weight = clamp(dot(normal, sample_normal), 0.0, 1.0);
			float weight = exp(-abs(float(x)) - abs(float(y))) * depth_weight * normal_weight * max(sample_color.a, 0.0);
			accum += sample_color.rgb * weight;
			weight_sum += weight;
			validity = max(validity, sample_color.a);
		}
	}
	vec3 irradiance = weight_sum > 0.0 ? accum / weight_sum : vec3(0.0);
	float ambient_alpha = validity > 0.0 ? 1.0 : 0.0;
	imageStore(ambient_image, pixel, vec4(irradiance, ambient_alpha));
	vec4 reflection = vec4(0.0);
	if (roughness > params.rules.z) {
		uint level = 1u;
		float spacing = 2.0;
		vec3 origin = floor(params.camera.xyz / spacing) * spacing;
		vec3 local = (world - origin) / spacing + vec3(7.5);
		ivec3 cell = ivec3(clamp(round(local), vec3(0.0), vec3(15.0)));
		uint probe = level * 4096u + uint(cell.x) + uint(cell.y) * 16u + uint(cell.z) * 256u;
		vec3 coarse = probes.data[probe].rgb;
		if (params.camera.w > 0.5) {
			uvec2 tile = uvec2(probe % 128u, probe / 128u);
			ivec2 mip_texel = ivec2(tile) * 4 + ivec2(1, 1);
			vec3 mip_color = texelFetch(wrc_mip2, mip_texel, 0).rgb;
			if (dot(mip_color, mip_color) > 0.0) {
				coarse = mip_color;
			}
		}
		reflection = vec4(coarse, 1.0);
	}
	imageStore(reflection_image, pixel, reflection);
}

void main() {
	if (pc.mode == 0u) {
		lite_pack();
	} else if (pc.mode == 1u) {
		lite_interpolate();
	} else {
		lite_upsample();
	}
}
