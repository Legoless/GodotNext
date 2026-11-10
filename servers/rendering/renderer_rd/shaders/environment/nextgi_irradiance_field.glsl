#[compute]

#version 450

// GodotNext: ps_lumen_lite (#183). Irradiance-field gather for Lumen Lite (plan §21.2 L6).
// Mode 0 packs WRC levels 0-1 into the probe buffer (hardware tier).
// Mode 1 interpolates irradiance at half resolution. Mode 2 upsamples it into the GI buffers
// and writes the rough specular from the WRC mips.
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

// NextGIIrradianceField::Params. The doctest "[NextGI][Lite] Params reconstruct world positions"
// checks inv_view_proj and view_to_world against the shared view helper (nextgi_view_inc.glsl).
struct LiteParams {
	mat4 inv_view_proj;
	vec4 camera; // xyz; w = 1 when the WRC textures are bound.
	vec4 screen; // GI size, half size.
	vec4 depth_info; // Internal size, depth valid, normal valid.
	vec4 rules; // Inside back-face fraction, push spacing, SSR roughness, mip-2 roughness.
	vec4 level_origin[2]; // WRC level grid origin (xyz) and spacing (w).
	vec4 view_to_world[3]; // Camera basis columns (xyz): view-space normals to world space.
};

// Four vec4 per probe: L0 radiance and validity (w above rules.x: inside geometry or never
// traced), then L1 radiance along x, y and z. Radiance in direction d is L0 + L1 . d.
layout(set = 0, binding = 0, std430) buffer LiteProbes {
	vec4 data[32768];
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
layout(set = 0, binding = 11) uniform sampler2D wrc_mip4;

shared vec3 pack_l0[64];
shared vec3 pack_l1x[64];
shared vec3 pack_l1y[64];
shared vec3 pack_l1z[64];

LiteParams lite_params() {
	return params_buffer.data;
}

vec3 lite_unproject(ivec2 pixel, float depth, vec2 internal_size) {
	LiteParams params = lite_params();
	vec2 uv = (vec2(pixel) + vec2(0.5)) / max(internal_size, vec2(1.0));
	return nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
}

// The WRC's octahedral layout (nextgi_wrc_oct_dir), continuous: [0, 1]^2 over the tile.
vec2 lite_oct_uv(vec3 direction) {
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
	return clamp(n.xy * 0.5 + 0.5, vec2(0.0), vec2(1.0));
}

vec3 lite_oct_dir(ivec2 texel) {
	vec2 uv = (vec2(texel) + 0.5) / 8.0 * 2.0 - 1.0;
	vec3 direction = vec3(uv.x, uv.y, 1.0 - abs(uv.x) - abs(uv.y));
	if (direction.z < 0.0) {
		float x = (1.0 - abs(direction.y)) * sign(direction.x);
		float y = (1.0 - abs(direction.x)) * sign(direction.y);
		direction.x = x;
		direction.y = y;
	}
	return normalize(direction);
}

uvec2 lite_tile(uint probe) {
	return uvec2(probe % 128u, probe / 128u);
}

vec3 lite_probe_position(uint level, ivec3 cell) {
	vec4 origin = lite_params().level_origin[level];
	return origin.xyz + (vec3(cell) - vec3(7.5)) * origin.w;
}

// Cosine-weighted irradiance over pi from L0/L1 radiance: L0 + 2/3 L1 . n.
vec3 lite_probe_irradiance(uint probe, vec3 normal) {
	vec3 l0 = probes.data[probe * 4u].rgb;
	vec3 l1 = probes.data[probe * 4u + 1u].rgb * normal.x + probes.data[probe * 4u + 2u].rgb * normal.y + probes.data[probe * 4u + 3u].rgb * normal.z;
	return max(l0 + (2.0 / 3.0) * l1, vec3(0.0));
}

vec3 lite_probe_radiance(uint probe, vec3 direction) {
	vec3 l0 = probes.data[probe * 4u].rgb;
	vec3 l1 = probes.data[probe * 4u + 1u].rgb * direction.x + probes.data[probe * 4u + 2u].rgb * direction.y + probes.data[probe * 4u + 3u].rgb * direction.z;
	return max(l0 + l1, vec3(0.0));
}

// Bilinear over a probe's side x side octahedral mip tile (WRC mip 4 or mip 2).
vec3 lite_wrc_mip(sampler2D mip, uint probe, vec3 direction, int side, int stride) {
	vec2 f = lite_oct_uv(direction) * float(side) - 0.5;
	vec2 base_f = clamp(floor(f), vec2(0.0), vec2(float(side - 1)));
	vec2 t = clamp(f - base_f, vec2(0.0), vec2(1.0));
	ivec2 base = ivec2(base_f);
	ivec2 next = min(base + ivec2(1), ivec2(side - 1));
	ivec2 origin = ivec2(lite_tile(probe)) * stride + ivec2(1, 1);
	vec3 c00 = texelFetch(mip, origin + base, 0).rgb;
	vec3 c10 = texelFetch(mip, origin + ivec2(next.x, base.y), 0).rgb;
	vec3 c01 = texelFetch(mip, origin + ivec2(base.x, next.y), 0).rgb;
	vec3 c11 = texelFetch(mip, origin + next, 0).rgb;
	return mix(mix(c00, c10, t.x), mix(c01, c11, t.x), t.y);
}

void lite_pack() {
	uint local = gl_LocalInvocationIndex;
	uint probe = (pc.probe_base + gl_WorkGroupID.x) % 8192u;
	ivec2 local_texel = ivec2(int(local % 8u), int(local / 8u));
	ivec2 texel = ivec2(lite_tile(probe)) * 10 + ivec2(1, 1) + local_texel;
	vec3 radiance = texelFetch(wrc_radiance, texel, 0).rgb;
	vec3 direction = lite_oct_dir(local_texel);
	pack_l0[local] = radiance;
	pack_l1x[local] = radiance * direction.x;
	pack_l1y[local] = radiance * direction.y;
	pack_l1z[local] = radiance * direction.z;
	barrier();
	if (local == 0u) {
		vec3 l0 = vec3(0.0);
		vec3 l1x = vec3(0.0);
		vec3 l1y = vec3(0.0);
		vec3 l1z = vec3(0.0);
		for (uint i = 0u; i < 64u; i++) {
			l0 += pack_l0[i];
			l1x += pack_l1x[i];
			l1y += pack_l1y[i];
			l1z += pack_l1z[i];
		}
		// Least-squares L1 over the sphere: 3 E[L d].
		l0 *= 1.0 / 64.0;
		l1x *= 3.0 / 64.0;
		l1y *= 3.0 / 64.0;
		l1z *= 3.0 / 64.0;
		uint in_level = probe % 4096u;
		uint level = probe / 4096u;
		ivec3 cell = ivec3(int(in_level % 16u), int((in_level / 16u) % 16u), int(in_level / 256u + level * 16u));
		// The WRC writes w = 1 for a traced probe and 2 for one inside geometry; 0 is never traced.
		float offset_w = texelFetch(wrc_offsets, cell, 0).a;
		float invalid = (offset_w < 0.5 || offset_w > 1.5) ? 1.0 : 0.0;
		probes.data[probe * 4u] = vec4(l0, invalid);
		probes.data[probe * 4u + 1u] = vec4(l1x, 0.0);
		probes.data[probe * 4u + 2u] = vec4(l1y, 0.0);
		probes.data[probe * 4u + 3u] = vec4(l1z, 0.0);
	}
}

// The 8 probes around world on the finest level whose grid contains it, with trilinear
// weights times probe validity and, on the hardware tier, the depth-moment visibility.
// False when world is outside level 1 (no probe covers it).
bool lite_probe_weights(vec3 world, vec3 normal, out uint corner_probe[8], out float corner_weight[8]) {
	LiteParams params = lite_params();
	int level = -1;
	vec3 grid = vec3(0.0);
	for (int candidate = 0; candidate < 2; candidate++) {
		vec4 origin = params.level_origin[candidate];
		vec3 g = (world - origin.xyz) / origin.w + vec3(7.5);
		if (all(greaterThanEqual(g, vec3(0.0))) && all(lessThanEqual(g, vec3(15.0)))) {
			level = candidate;
			grid = g;
			break;
		}
	}
	for (uint corner = 0u; corner < 8u; corner++) {
		corner_probe[corner] = 0u;
		corner_weight[corner] = 0.0;
	}
	if (level < 0) {
		return false;
	}
	float spacing = params.level_origin[level].w;
	vec3 base_f = min(floor(grid), vec3(14.0));
	vec3 frac = grid - base_f;
	ivec3 base = ivec3(base_f);
	for (uint corner = 0u; corner < 8u; corner++) {
		ivec3 delta = ivec3(int(corner & 1u), int((corner >> 1u) & 1u), int((corner >> 2u) & 1u));
		ivec3 cell = base + delta;
		uint probe = uint(level) * 4096u + uint(cell.x) + uint(cell.y) * 16u + uint(cell.z) * 256u;
		vec4 probe_l0 = probes.data[probe * 4u];
		if (probe_l0.a > params.rules.x) {
			continue;
		}
		vec3 probe_pos = lite_probe_position(uint(level), cell);
		// A probe behind the visible surface is pushed out along the prepass normal (D15 rule)
		// before the back-face term weighs it.
		vec3 weighed_pos = probe_pos;
		if (dot(probe_pos - world, normal) < 0.0) {
			weighed_pos += normal * (params.rules.y * spacing);
		}
		vec3 to_probe = weighed_pos - world;
		float facing = dot(to_probe, to_probe) > 1e-8 ? dot(normalize(to_probe), normal) : 1.0;
		float weight = (delta.x == 0 ? (1.0 - frac.x) : frac.x);
		weight *= delta.y == 0 ? (1.0 - frac.y) : frac.y;
		weight *= delta.z == 0 ? (1.0 - frac.z) : frac.z;
		weight *= (facing + 1.0) * (facing + 1.0) * 0.25 + 0.2;
		float visibility = clamp(1.0 - probe_l0.a, 0.0, 1.0);
		if (params.camera.w > 0.5) {
			// The WRC traced the probe from its relocated position. The receiver is lifted off its
			// surface so the surface's own hit does not occlude it.
			ivec3 wrc_cell = ivec3(cell.xy, cell.z + level * 16);
			vec3 traced_pos = probe_pos + texelFetch(wrc_offsets, wrc_cell, 0).xyz;
			vec3 to_pixel = world + normal * (0.1 * spacing) - traced_pos;
			float hit_distance = length(to_pixel);
			vec3 direction = hit_distance > 1e-4 ? to_pixel / hit_distance : normal;
			ivec2 visibility_texel = ivec2(lite_tile(probe)) * 8 + ivec2(clamp(lite_oct_uv(direction) * 8.0, vec2(0.0), vec2(7.0)));
			vec2 moments = texelFetch(wrc_visibility, visibility_texel, 0).rg;
			if (moments.x > 0.0 && hit_distance > moments.x) {
				float variance = max(moments.y - moments.x * moments.x, 0.01 * spacing * spacing);
				float delta_t = hit_distance - moments.x;
				visibility = variance / (variance + delta_t * delta_t);
			}
		}
		corner_probe[corner] = probe;
		corner_weight[corner] = weight * visibility;
	}
	return true;
}

// The prepass writes view-space normals (scene_forward_clustered.glsl encodes modelview_normal),
// while the probes, the WRC and the reflection vector are in world space.
vec3 lite_decode_normal(vec3 encoded) {
	LiteParams params = lite_params();
	return nextgi_view_world_normal(mat3(params.view_to_world[0].xyz, params.view_to_world[1].xyz, params.view_to_world[2].xyz), encoded);
}

vec3 lite_pixel_normal(ivec2 depth_pixel, vec3 world) {
	LiteParams params = lite_params();
	if (params.depth_info.w > 0.5) {
		vec3 encoded = texelFetch(normal_roughness_tex, depth_pixel, 0).rgb;
		if (dot(encoded, encoded) > 1e-8) {
			return lite_decode_normal(encoded);
		}
	}
	vec3 to_camera = params.camera.xyz - world;
	return dot(to_camera, to_camera) > 1e-8 ? normalize(to_camera) : vec3(0.0, 1.0, 0.0);
}

void lite_interpolate() {
	LiteParams params = lite_params();
	ivec2 half_pixel = ivec2(gl_GlobalInvocationID.xy);
	vec2 half_size = max(params.screen.zw, vec2(1.0));
	if (half_pixel.x >= int(half_size.x) || half_pixel.y >= int(half_size.y)) {
		return;
	}
	vec2 internal_size = max(params.depth_info.xy, vec2(1.0));
	ivec2 depth_pixel = ivec2(clamp((vec2(half_pixel) + vec2(0.5)) / half_size * internal_size, vec2(0.0), internal_size - vec2(1.0)));
	float depth = params.depth_info.z > 0.5 ? texelFetch(depth_tex, depth_pixel, 0).r : 0.0;
	if (params.depth_info.z < 0.5 || depth <= 0.0) {
		imageStore(half_image, half_pixel, vec4(0.0));
		return;
	}
	vec3 world = lite_unproject(depth_pixel, depth, internal_size);
	vec3 normal = lite_pixel_normal(depth_pixel, world);
	uint corner_probe[8];
	float corner_weight[8];
	vec3 color = vec3(0.0);
	float weight_sum = 0.0;
	if (lite_probe_weights(world, normal, corner_probe, corner_weight)) {
		for (uint corner = 0u; corner < 8u; corner++) {
			if (corner_weight[corner] <= 0.0) {
				continue;
			}
			color += lite_probe_irradiance(corner_probe[corner], normal) * corner_weight[corner];
			weight_sum += corner_weight[corner];
		}
	}
	imageStore(half_image, half_pixel, weight_sum > 0.0 ? vec4(color / weight_sum, 1.0) : vec4(0.0));
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
	if (params.depth_info.z < 0.5 || depth <= 0.0) {
		imageStore(ambient_image, pixel, vec4(0.0));
		imageStore(reflection_image, pixel, vec4(0.0));
		return;
	}
	vec3 world = lite_unproject(depth_pixel, depth, internal_size);
	vec3 normal = lite_pixel_normal(depth_pixel, world);
	float roughness = 0.0;
	if (params.depth_info.w > 0.5) {
		vec4 encoded = texelFetch(normal_roughness_tex, depth_pixel, 0);
		if (dot(encoded.rgb, encoded.rgb) > 1e-8) {
			float rough_w = encoded.a > 0.5 ? 1.0 - encoded.a : encoded.a;
			roughness = rough_w * (255.0 / 127.0);
		}
	}
	vec2 half_size = max(params.screen.zw, vec2(1.0));
	ivec2 half_center = ivec2(clamp((vec2(pixel) + vec2(0.5)) / gi_size * half_size, vec2(0.0), half_size - vec2(1.0)));
	vec3 accum = vec3(0.0);
	float weight_sum = 0.0;
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
					sample_normal = lite_decode_normal(sample_encoded);
				}
			}
			float depth_weight = exp(-abs(sample_depth - depth) * 32.0);
			float normal_weight = clamp(dot(normal, sample_normal), 0.0, 1.0);
			float weight = exp(-abs(float(x)) - abs(float(y))) * depth_weight * normal_weight * max(sample_color.a, 0.0);
			accum += sample_color.rgb * weight;
			weight_sum += weight;
		}
	}
	imageStore(ambient_image, pixel, weight_sum > 0.0 ? vec4(accum / weight_sum, 1.0) : vec4(0.0));

	// Specular: no reflection rays. At or below the SSR roughness the forward pass keeps its own
	// specular (alpha 0, SCREEN_SPACE_EFFECTS_FLAGS_NEXTGI_LITE). Rougher surfaces read the WRC
	// mips of the surrounding probes at the reflection vector (software tier: the probes' L1).
	vec4 reflection = vec4(0.0);
	uint corner_probe[8];
	float corner_weight[8];
	if (roughness > params.rules.z && lite_probe_weights(world, normal, corner_probe, corner_weight)) {
		vec3 reflected = reflect(normalize(world - params.camera.xyz), normal);
		float mip2_mix = clamp((roughness - params.rules.z) / max(params.rules.w - params.rules.z, 1e-4), 0.0, 1.0);
		vec3 specular = vec3(0.0);
		float specular_weight = 0.0;
		for (uint corner = 0u; corner < 8u; corner++) {
			if (corner_weight[corner] <= 0.0) {
				continue;
			}
			uint probe = corner_probe[corner];
			vec3 radiance;
			if (params.camera.w > 0.5) {
				radiance = mix(lite_wrc_mip(wrc_mip4, probe, reflected, 4, 6), lite_wrc_mip(wrc_mip2, probe, reflected, 2, 4), mip2_mix);
			} else {
				radiance = lite_probe_radiance(probe, reflected);
			}
			specular += radiance * corner_weight[corner];
			specular_weight += corner_weight[corner];
		}
		if (specular_weight > 0.0) {
			reflection = vec4(specular / specular_weight, 1.0);
		}
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
