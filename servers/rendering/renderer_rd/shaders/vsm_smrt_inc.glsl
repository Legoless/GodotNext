/**************************************************************************/
/*  vsm_smrt_inc.glsl                                                     */
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

// GodotNext: vsm_projection (#222, plan §21.3 contract S6). Virtual Shadow Map projection with Shadow Map
// Ray Tracing (SMRT) for the Forward+ light loop: vsm_shadow_directional() and vsm_shadow_local() replace
// the shadow atlas samplers for lights whose LightData vsm_flags carry VSM_LIGHT_FLAGS_SAMPLE.
//
// R rays leave the receiver toward points of the light (a directional light's disk of its angular size, a
// spot or omni light's disk of radius LIGHT_PARAM_SIZE across the direction to the light, an area light's
// rectangle). Each ray is marched in S steps through the map at the receiver's mip, from a first step
// VSM_SMRT_FIRST_STEP_TEXELS texels above the receiver to the point where it has moved VSM_SMRT_MAX_TEXELS
// texels across the map (geometric spacing, jittered per ray). A step behind the stored depth occludes its
// ray; the shadow factor is the share of rays that reach the light. R = 0, or a light without size, takes
// one 2 x 2 filtered tap. The receiver reads the page the marking pass requested for it (the same level,
// face and mip) and falls back to coarser mips and levels; a sample with no page at all is lit. With
// Prefiltered Distant on (VSMProjectionParams misc.x), receivers in a directional light's last
// VSM_PREFILTERED_DISTANT_LEVELS levels take vsm_prefiltered_distant() instead.
//
// RendererVirtualShadowMapStorage::shadow_directional() and shadow_local() are the CPU models; keep them in
// step. Include after vsm_mark_inc.glsl, vsm_page_table_inc.glsl and vsm_raster_inc.glsl. The including
// shader declares vsm_page_table, vsm_maps (VSMMapData data[]), vsm_map_state (uint data[]), vsm_projection
// (VSMProjectionParams data), vsm_pool_tex (utexture2DArray), vsm_blue_noise (texture2D) and the
// SAMPLER_NEAREST_CLAMP sampler.

// #54's blue-noise tile (nextgi_blue_noise_sample()), offset by the R2 sequence of the frame.
vec2 vsm_blue_noise_sample(uvec2 p_pixel, float p_frame) {
	vec2 offset = fract(vec2(float(uint(p_frame))) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (ivec2(p_pixel) + ivec2(offset)) & ivec2(63);
	return texelFetch(sampler2D(vsm_blue_noise, SAMPLER_NEAREST_CLAMP), coord, 0).rg;
}

// RendererVirtualShadowMapStorage::smrt_disk().
vec2 vsm_smrt_disk(uint p_ray, uint p_rays, vec2 p_noise) {
	float count = float(max(p_rays, 1u));
	float radius = sqrt(min((float(p_ray) + p_noise.x) / count, 1.0));
	float turn = float(p_ray) * 0.618034 + p_noise.y;
	turn = turn - floor(turn);
	float angle = 6.2831853 * turn;
	return vec2(cos(angle) * radius, sin(angle) * radius);
}

// RendererVirtualShadowMapStorage::smrt_square().
vec2 vsm_smrt_square(uint p_ray, uint p_rays, vec2 p_noise) {
	float count = float(max(p_rays, 1u));
	float u = (float(p_ray) + p_noise.x) / count;
	u = u - floor(u);
	float v = float(p_ray) * 0.618034 + p_noise.y;
	v = v - floor(v);
	return vec2(u - 0.5, v - 0.5);
}

// RendererVirtualShadowMapStorage::smrt_jitter().
float vsm_smrt_jitter(uint p_ray, vec2 p_noise) {
	float jitter = (p_noise.x + p_noise.y) * 0.5 + float(p_ray) * 0.754878;
	return jitter - floor(jitter);
}

// RendererVirtualShadowMapStorage::smrt_step().
float vsm_smrt_step(uint p_step, uint p_steps, float p_jitter, float p_first, float p_last) {
	float t = (float(p_step) + p_jitter) / float(max(p_steps, 1u));
	if (!(p_last > p_first) || !(p_first > 0.0)) {
		return p_first;
	}
	return p_first * pow(p_last / p_first, t);
}

// RendererVirtualShadowMapStorage::smrt_step_local().
float vsm_smrt_step_local(uint p_step, uint p_steps, float p_jitter, float p_first, float p_last) {
	float t = (float(p_step) + p_jitter) / float(max(p_steps, 1u));
	if (!(p_last > p_first) || !(p_first > 0.0)) {
		return p_first;
	}
	return p_first + (p_last - p_first) * (t * t);
}

uint vsm_projection_overflow_bias(uint p_map) {
	return vsm_map_state.data[VSM_MAP_STATE_BIAS * vsm_projection.data.counts.x + p_map] & 0xFFu;
}

// RendererVirtualShadowMapStorage::clipmap_projection_select(): the marking pass's fine request.
bool vsm_clipmap_projection_select(uint p_first, uint p_count, uint p_light_flags, ivec2 p_texel0, float p_footprint, out uint r_level, out uint r_mip) {
	uint table_maps = vsm_projection.data.counts.x;
	// GodotNext: vsm_settings_debug (#221) / vsm_local_lights (#216). Plus the light's moving bias (level 0)
	// and per-light resolution bias, as the marking pass.
	int map_bias = p_first < table_maps ? vsm_maps.data[p_first].lod_bias : 0;
	int res_int_bias;
	float res_scale;
	vsm_resolution_bias_parts(p_light_flags, res_int_bias, res_scale);
	int level_mip = vsm_clipmap_level_mip(p_footprint * res_scale, vsm_projection.data.bias.x + map_bias + res_int_bias);
	r_level = 0u;
	r_mip = 0u;
	for (uint level = 0u; level < p_count; level++) {
		uint map = p_first + level;
		if (map >= table_maps) {
			break;
		}
		ivec2 origin = vsm_maps.data[map].origin;
		ivec2 level_texel = p_texel0 >> int(level);
		if (!vsm_clipmap_in_window(origin, level_texel, 0u)) {
			continue;
		}
		uint mip = min(vsm_biased_mip(level_mip - int(level), 0) + vsm_projection_overflow_bias(map), VSM_MIP_COUNT - 1u);
		if (level + 1u == p_count) {
			while (mip > 0u && !vsm_clipmap_in_window(origin, level_texel, mip)) {
				mip--;
			}
		} else if (!vsm_clipmap_in_window(origin, level_texel, mip)) {
			continue;
		}
		r_level = level;
		r_mip = mip;
		return true;
	}
	return false;
}

// RendererVirtualShadowMapStorage::clipmap_sample_level().
VSMLookup vsm_clipmap_sample_level(uint p_first, uint p_count, ivec2 p_texel0, uint p_level, uint p_mip, out uint r_level) {
	uint table_maps = vsm_projection.data.counts.x;
	r_level = p_level;
	for (uint level = p_level; level < p_count; level++) {
		uint map = p_first + level;
		if (map >= table_maps) {
			break;
		}
		uint mip = uint(max(int(p_mip) - int(level - p_level), 0));
		VSMLookup result = vsm_lookup_clipmap(table_maps, map, vsm_maps.data[map].origin, p_texel0 >> int(level), mip);
		if (result.valid) {
			r_level = level;
			return result;
		}
	}
	return vsm_lookup_miss();
}

bool vsm_smrt_clipmap_depth(uint p_first, uint p_count, vec2 p_t, uint p_level, uint p_mip, out float r_stored, out float r_texel) {
	ivec2 texel0 = ivec2(clamp(floor(p_t), vec2(-1073741824.0), vec2(1073741824.0)));
	uint level;
	VSMLookup result = vsm_clipmap_sample_level(p_first, p_count, texel0, p_level, p_mip, level);
	if (!result.valid) {
		return false;
	}
	r_texel = exp2(float(int(level + result.mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	r_stored = vsm_pool_depth(result.texel);
	return true;
}

// _smrt_clipmap_occluded(): whether level 0 texel position p_t lies behind the stored depth.
bool vsm_smrt_clipmap_occluded(uint p_first, uint p_count, vec2 p_t, float p_depth, uint p_level, uint p_mip) {
	float stored;
	float texel;
	if (!vsm_smrt_clipmap_depth(p_first, p_count, p_t, p_level, p_mip, stored, texel)) {
		return false;
	}
	return stored < p_depth - texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
}

// Prefiltered Distant (RendererVirtualShadowMapStorage::prefiltered_distant()): a moments filter over the
// VSM_PREFILTERED_DISTANT_TAPS^2 texels around level 0 texel position p_t at p_level and p_mip.
float vsm_prefiltered_distant(uint p_first, uint p_count, vec2 p_t, float p_depth, uint p_level, uint p_mip) {
	float scale = exp2(float(p_level + p_mip));
	float texel = exp2(float(int(p_level + p_mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	vec2 grid = p_t / scale - vec2(0.5);
	float half_block = float(VSM_PREFILTERED_DISTANT_TAPS / 2u - 1u);
	vec2 base = floor(grid) - vec2(half_block);
	float sum = 0.0;
	float sum_squares = 0.0;
	uint stored_count = 0u;
	for (uint y = 0u; y < VSM_PREFILTERED_DISTANT_TAPS; y++) {
		for (uint x = 0u; x < VSM_PREFILTERED_DISTANT_TAPS; x++) {
			vec2 sample_t = (base + vec2(float(x) + 0.5, float(y) + 0.5)) * scale;
			ivec2 texel0 = ivec2(clamp(floor(sample_t), vec2(-1073741824.0), vec2(1073741824.0)));
			uint found_level;
			VSMLookup result = vsm_clipmap_sample_level(p_first, p_count, texel0, p_level, p_mip, found_level);
			if (!result.valid) {
				continue;
			}
			float stored = vsm_pool_depth(result.texel);
			if (!(stored < 1.0e30)) {
				continue;
			}
			// Center around p_depth before squaring to avoid float32 cancellation on far clipmap levels.
			float delta = stored - p_depth;
			sum += delta;
			sum_squares += delta * delta;
			stored_count++;
		}
	}
	if (stored_count == 0u) {
		return 1.0;
	}
	float coverage = float(stored_count) / float(VSM_PREFILTERED_DISTANT_TAPS * VSM_PREFILTERED_DISTANT_TAPS);
	float mean_delta = sum / float(stored_count);
	float min_variance = texel * VSM_PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS * texel * VSM_PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS;
	float variance = max(sum_squares / float(stored_count) - mean_delta * mean_delta, min_variance);
	float d = -texel * VSM_SMRT_DEPTH_BIAS_TEXELS - mean_delta;
	float lit = 1.0;
	if (d > 0.0) {
		lit = variance / (variance + d * d);
		lit = clamp((lit - VSM_PREFILTERED_DISTANT_BLEED) / (1.0 - VSM_PREFILTERED_DISTANT_BLEED), 0.0, 1.0);
	}
	return (1.0 - coverage) + coverage * lit;
}

// The shadow of a directional light at a receiver (RendererVirtualShadowMapStorage::shadow_directional()):
// 1 lit, 0 shadowed. p_world and p_normal in world space; p_tan_half_angle is tan of half the light's angular
// diameter; p_view_depth the receiver's view depth (its pixel footprint picks the level and mip).
float vsm_shadow_directional(uint p_map_base, uint p_light_flags, vec3 p_world, vec3 p_normal, float p_tan_half_angle, float p_view_depth, vec2 p_noise) {
	uint table_maps = vsm_projection.data.counts.x;
	uint count = (p_light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint first = vsm_light_map(p_map_base, p_light_flags, 0u);
	if (first >= table_maps || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
		return 1.0;
	}
	VSMMapData level0 = vsm_maps.data[first];
	ivec2 initial_texel0 = vsm_clipmap_texel0(level0, p_world);
	float footprint = p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y;
	uint level;
	uint mip;
	if (!vsm_clipmap_projection_select(first, count, p_light_flags, initial_texel0, footprint, level, mip)) {
		return 1.0;
	}
	// GodotNext: vsm_fog_translucency (#215). Resolve the resident page first so normal bias, PCF spacing
	// and SMRT reach scale to the actual resident mip (e.g. coarse pages under translucent SMRT or overflow).
	uint resident_level;
	VSMLookup probe = vsm_clipmap_sample_level(first, count, initial_texel0, level, mip, resident_level);
	if (!probe.valid) {
		return 1.0;
	}
	level = resident_level;
	mip = probe.mip;
	float texel = exp2(float(int(level + mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	// Row 2 is minus the light's Z axis: the light shines along it, and the light lies along -row 2.
	vec3 toward = -level0.row2.xyz;
	float n_dot_l = clamp(dot(p_normal, toward), -1.0, 1.0);
	float offset = texel * VSM_SMRT_NORMAL_BIAS_TEXELS * sqrt(max(1.0 - n_dot_l * n_dot_l, 0.0));
	vec3 receiver = p_world + p_normal * offset;
	vec2 t = vec2(vsm_row(level0.row0, receiver), vsm_row(level0.row1, receiver));
	float depth = vsm_row(level0.row2, receiver);
	float texels_per_meter = exp2(float(-VSM_CLIPMAP_TEXEL0_EXPONENT));
	float scale = exp2(float(level + mip));
	// Receiver tangent-plane depth slope per meter along the clipmap's X and Y axes.
	vec3 lx = level0.row0.xyz * level0.texel_scale;
	vec3 ly = level0.row1.xyz * level0.texel_scale;
	float inv_ndl = n_dot_l > 0.15 ? (1.0 / n_dot_l) : 0.0;
	vec2 rp_slope = clamp(vec2(dot(p_normal, lx), dot(p_normal, ly)) * inv_ndl, vec2(-2.0), vec2(2.0));

	if (vsm_projection.data.misc.x != 0u && count > VSM_PREFILTERED_DISTANT_LEVELS && level + VSM_PREFILTERED_DISTANT_LEVELS >= count) {
		return vsm_prefiltered_distant(first, count, t, depth, level, mip);
	}
	uint rays = min(vsm_projection.data.counts.z, VSM_SMRT_MAX_RAYS);
	if (rays == 0u || !(p_tan_half_angle > 0.0)) {
		// One 2 x 2 filtered tap on the sampled level and mip with uphill receiver-plane depth projection.
		vec2 grid = t / scale - vec2(0.5);
		vec2 base = floor(grid);
		vec2 f = grid - base;
		float lit = 0.0;
		for (uint corner = 0u; corner < 4u; corner++) {
			vec2 corner_offset = vec2(float(corner & 1u), float(corner >> 1u));
			vec2 sample_t = (base + corner_offset + vec2(0.5)) * scale;
			vec2 delta_tx = (sample_t - t) / scale;
			float corner_depth = depth + min(dot(rp_slope, delta_tx), 0.0) * texel;
			float weight = ((corner & 1u) != 0u ? f.x : 1.0 - f.x) * ((corner >> 1u) != 0u ? f.y : 1.0 - f.y);
			float stored;
			float sample_texel;
			bool occluded = vsm_smrt_clipmap_depth(first, count, sample_t, level, mip, stored, sample_texel) && (stored < corner_depth - sample_texel * VSM_SMRT_DEPTH_BIAS_TEXELS);
			lit += occluded ? 0.0 : weight;
		}
		return lit;
	}

	uint steps = clamp(vsm_projection.data.counts.w, 1u, VSM_SMRT_MAX_STEPS);
	float first_height = texel * VSM_SMRT_FIRST_STEP_TEXELS;
	float level_extent = VSM_CLIPMAP_LEVEL0_EXTENT * exp2(float(level));
	float sub_scale = clamp((n_dot_l - 0.50) * 3.0, 0.0, 1.0);
	vec2 sub_noise = fract(p_noise + vec2(0.381966, 0.754878));
	uint lit = 0u;
	for (uint ray = 0u; ray < rays; ray++) {
		// Stratified sub-texel origin jitter on the receiver's tangent plane gives stochastic bilinear
		// anti-aliasing even for near-ground contact shadows whose penumbra is narrower than one texel.
		// Clamp tangent-plane depth shift to <= 0 (uphill only) so curved or bumpy NextGeo geometry
		// never projects behind its own depth buffer surface.
		vec2 sub_tx = vsm_smrt_square(ray, rays, sub_noise) * sub_scale;
		vec2 ray_t = t + sub_tx * scale;
		float ray_depth = depth + min(dot(rp_slope, sub_tx), 0.0) * texel;
		float init_stored;
		float init_texel;
		bool prev_behind = false;
		if (vsm_smrt_clipmap_depth(first, count, ray_t, level, mip, init_stored, init_texel)) {
			float init_scale = init_texel * texels_per_meter;
			vec2 init_center = (floor(ray_t / init_scale) + vec2(0.5)) * init_scale;
			vec2 init_delta_tx = (init_center - ray_t) / init_scale;
			float init_depth = ray_depth + min(dot(rp_slope, init_delta_tx), 0.0) * init_texel;
			prev_behind = init_stored < init_depth - init_texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
		}
		// The ray's slope across the light: a point of the light's disk, in tangent units.
		vec2 slope = vsm_smrt_disk(ray, rays, p_noise) * p_tan_half_angle;
		float slope_length = length(slope);
		float last_height = level_extent;
		if (slope_length > 0.0) {
			last_height = min(VSM_SMRT_MAX_TEXELS * texel / slope_length, level_extent);
		}
		last_height = max(last_height, first_height);
		float jitter = vsm_smrt_jitter(ray, p_noise);
		bool occluded = false;
		float z_prev = ray_depth;
		for (uint step_index = 0u; step_index < steps && !occluded; step_index++) {
			float height = vsm_smrt_step(step_index, steps, jitter, first_height, last_height);
			float z_curr = ray_depth - height;
			vec2 sample_t = ray_t + slope * (height * texels_per_meter);
			float stored;
			float sample_texel;
			if (vsm_smrt_clipmap_depth(first, count, sample_t, level, mip, stored, sample_texel)) {
				float bias = sample_texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
				float step_dz = max(z_prev - z_curr, 0.0);
				float thickness = max(step_dz * 0.5, sample_texel * 2.0);
				bool behind = stored < z_curr - bias;
				if ((behind && (z_curr - stored <= thickness)) || (prev_behind && !behind && (stored < z_prev - bias)) || (step_index + 1u == steps && behind)) {
					occluded = true;
				}
				prev_behind = behind;
			} else {
				prev_behind = false;
			}
			z_prev = z_curr;
		}
		lit += occluded ? 0u : 1u;
	}
	return float(lit) / float(rays);
}

// RendererVirtualShadowMapStorage::face_projection_select(): the face that holds p_world, the one with the
// largest w where faces overlap.
uint vsm_face_projection_select(uint p_map_base, uint p_light_flags, vec3 p_world, out float r_w) {
	return vsm_face_select(vsm_projection.data.counts.x, p_map_base, p_light_flags, p_world, r_w);
}

// _smrt_face(): the face of a sample point (p_map when it holds the point, else for omni and area lights
// the face with the largest w), or VSM_MAP_NONE.
uint vsm_smrt_face(uint p_map_base, uint p_light_flags, uint p_map, vec3 p_point, out float r_x, out float r_y, out float r_w) {
	return vsm_face_select_hint(vsm_projection.data.counts.x, p_map_base, p_light_flags, p_map, p_point, r_x, r_y, r_w);
}

bool vsm_smrt_face_depth(uint p_map, float p_x, float p_y, float p_w, uint p_mip, out float r_stored, out float r_texel) {
	vec2 uv = vec2(p_x / p_w * 0.5 + 0.5, p_y / p_w * 0.5 + 0.5);
	VSMLookup result = vsm_lookup(vsm_projection.data.counts.x, p_map, uv, p_mip);
	if (!result.valid) {
		return false;
	}
	r_texel = vsm_maps.data[p_map].texel_scale * p_w * float(1u << result.mip);
	r_stored = vsm_pool_depth(result.texel);
	return true;
}

// _smrt_face_occluded(): whether a face point (depth w) lies behind the stored depth.
bool vsm_smrt_face_occluded(uint p_map, float p_x, float p_y, float p_w, uint p_mip) {
	float stored;
	float texel;
	if (!vsm_smrt_face_depth(p_map, p_x, p_y, p_w, p_mip, stored, texel)) {
		return false;
	}
	return stored < p_w - texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
}

// The shadow of a spot, omni or area light at a receiver (RendererVirtualShadowMapStorage::shadow_local()):
// 1 lit, 0 shadowed. World space; p_radius is a spot or omni light's radius (LIGHT_PARAM_SIZE); an area
// light passes its two edge vectors (otherwise zero).
float vsm_shadow_local(uint p_map_base, uint p_light_flags, vec3 p_world, vec3 p_normal, vec3 p_light_position, float p_radius, vec3 p_area_a, vec3 p_area_b, float p_view_depth, vec2 p_noise) {
	float w;
	uint map = vsm_face_projection_select(p_map_base, p_light_flags, p_world, w);
	if (map == VSM_MAP_NONE) {
		return 1.0;
	}
	VSMMapData face = vsm_maps.data[map];
	float footprint = p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y;
	// GodotNext: vsm_settings_debug (#221) / vsm_local_lights (#216). Plus moving bias, per-light resolution
	// bias and distant-light coarse-only policy, as the marking pass.
	int res_int_bias;
	float res_scale;
	vsm_resolution_bias_parts(p_light_flags, res_int_bias, res_scale);
	int bias = vsm_projection.data.bias.y + face.lod_bias + res_int_bias + int(vsm_projection_overflow_bias(map));
	uint mip = vsm_biased_mip(int(vsm_face_mip(w * face.texel_scale, footprint * res_scale)), bias);
	if ((p_light_flags & VSM_LIGHT_FLAGS_DISTANT_COARSE) != 0u && mip < VSM_COARSE_FIRST_MIP) {
		mip = VSM_COARSE_FIRST_MIP;
	}
	// GodotNext: vsm_fog_translucency (#215). Resolve the resident mip first so normal bias, PCF spacing
	// and SMRT reach scale to the actual resident mip (e.g. coarse pages under translucent SMRT or overflow).
	vec2 initial_uv = vec2(vsm_row(face.row0, p_world) / w * 0.5 + 0.5, vsm_row(face.row1, p_world) / w * 0.5 + 0.5);
	VSMLookup probe = vsm_lookup(vsm_projection.data.counts.x, map, initial_uv, mip);
	if (!probe.valid) {
		return 1.0;
	}
	mip = probe.mip;
	float texel = face.texel_scale * w * float(1u << mip);
	vec3 to_light = p_light_position - p_world;
	float light_distance = length(to_light);
	if (!(light_distance > 0.0)) {
		return 1.0;
	}
	vec3 light_dir = to_light / light_distance;
	float n_dot_l = clamp(dot(p_normal, light_dir), -1.0, 1.0);
	float offset = texel * VSM_SMRT_NORMAL_BIAS_TEXELS * sqrt(max(1.0 - n_dot_l * n_dot_l, 0.0));
	vec3 receiver = p_world + p_normal * offset;
	bool area = dot(p_area_a, p_area_a) > 0.0 && dot(p_area_b, p_area_b) > 0.0;

	float x;
	float y;
	float rw;
	uint receiver_map = vsm_smrt_face(p_map_base, p_light_flags, map, receiver, x, y, rw);
	if (receiver_map == VSM_MAP_NONE) {
		return 1.0;
	}
	VSMMapData rmd = vsm_maps.data[receiver_map];
	float inv_cot = 1.0 / max(length(rmd.row0.xyz), 1.0e-20);
	vec3 fx = rmd.row0.xyz * inv_cot;
	vec3 fy = rmd.row1.xyz * inv_cot;
	float f_ndl = dot(p_normal, p_light_position - receiver) / max(rw, 1.0e-20);
	float inv_f_ndl = f_ndl > 0.15 ? (1.0 / f_ndl) : 0.0;
	vec2 f_slope = clamp(vec2(dot(p_normal, fx), dot(p_normal, fy)) * inv_f_ndl, vec2(-2.0), vec2(2.0));
	uint rays = min(vsm_projection.data.counts.y, VSM_SMRT_MAX_RAYS);
	if (rays == 0u || (!area && !(p_radius > 0.0))) {
		// One 2 x 2 filtered tap at the receiver's mip with uphill receiver-plane depth projection.
		float mip_size = float(VSM_VIRTUAL_SIZE >> mip);
		vec2 receiver_tx = vec2(x / rw * 0.5 + 0.5, y / rw * 0.5 + 0.5) * mip_size;
		vec2 grid = receiver_tx - vec2(0.5);
		vec2 base = floor(grid);
		vec2 f = grid - base;
		float lit = 0.0;
		for (uint corner = 0u; corner < 4u; corner++) {
			vec2 sample_tx = base + vec2(float(corner & 1u), float(corner >> 1u)) + vec2(0.5);
			vec2 uv = sample_tx / mip_size;
			vec2 delta_tx = sample_tx - receiver_tx;
			float corner_w = max(rw + min(dot(f_slope, delta_tx), 0.0) * texel, rmd.near_w);
			float weight = ((corner & 1u) != 0u ? f.x : 1.0 - f.x) * ((corner >> 1u) != 0u ? f.y : 1.0 - f.y);
			float stored;
			float sample_texel;
			bool occluded = vsm_smrt_face_depth(receiver_map, (uv.x * 2.0 - 1.0) * corner_w, (uv.y * 2.0 - 1.0) * corner_w, corner_w, mip, stored, sample_texel) && (stored < corner_w - sample_texel * VSM_SMRT_DEPTH_BIAS_TEXELS);
			lit += occluded ? 0.0 : weight;
		}
		return lit;
	}

	// A frame across the direction to the light, for points of a spot or omni light's disk.
	vec3 up = abs(light_dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(light_dir, up));
	vec3 bitangent = cross(light_dir, tangent);
	float init_stored;
	float init_texel;
	bool init_behind = false;
	if (vsm_smrt_face_depth(receiver_map, x, y, rw, mip, init_stored, init_texel)) {
		float init_mip_size = max(rmd.texel_scale * rw * float(VSM_VIRTUAL_SIZE) / max(init_texel, 1.0e-20), 1.0);
		vec2 ray_tx = vec2(x / rw * 0.5 + 0.5, y / rw * 0.5 + 0.5) * init_mip_size;
		vec2 delta_tx = (floor(ray_tx) + vec2(0.5)) - ray_tx;
		float init_w = max(rw + min(dot(f_slope, delta_tx), 0.0) * init_texel, rmd.near_w);
		init_behind = init_stored < init_w - init_texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
	}
	uint steps = clamp(vsm_projection.data.counts.w, 1u, VSM_SMRT_MAX_STEPS);
	uint lit = 0u;
	for (uint ray = 0u; ray < rays; ray++) {
		vec3 light_offset;
		if (area) {
			vec2 square = vsm_smrt_square(ray, rays, p_noise);
			light_offset = p_area_a * square.x + p_area_b * square.y;
		} else {
			vec2 disk = vsm_smrt_disk(ray, rays, p_noise) * p_radius;
			light_offset = tangent * disk.x + bitangent * disk.y;
		}
		vec3 ray_vector = p_light_position + light_offset - receiver;
		float ray_length = length(ray_vector);
		if (!(ray_length > 0.0)) {
			lit++;
			continue;
		}
		float first = min(texel * VSM_SMRT_FIRST_STEP_TEXELS / ray_length, VSM_SMRT_LOCAL_MAX_REACH);
		float offset_length = length(light_offset);
		float last = VSM_SMRT_LOCAL_MAX_REACH;
		if (offset_length > 0.0) {
			// The angle between the sample and the receiver seen from the light is about
			// s * offset / ((1 - s) * distance): it reaches VSM_SMRT_LOCAL_MAX_TAN_ANGLE at s = c / (1 + c).
			float c = VSM_SMRT_LOCAL_MAX_TAN_ANGLE * light_distance / offset_length;
			last = min(c / (1.0 + c), VSM_SMRT_LOCAL_MAX_REACH);
		}
		last = max(last, first);
		float jitter = vsm_smrt_jitter(ray, p_noise);
		bool occluded = false;
		vec3 prev_point = receiver;
		uint prev_map = receiver_map;
		float prev_w = rw;
		bool prev_behind = init_behind;
		for (uint step_index = 0u; step_index < steps && !occluded; step_index++) {
			vec3 point = receiver + ray_vector * vsm_smrt_step_local(step_index, steps, jitter, first, last);
			float px;
			float py;
			float pw;
			uint point_map = vsm_smrt_face(p_map_base, p_light_flags, receiver_map, point, px, py, pw);
			if (point_map != VSM_MAP_NONE) {
				float stored;
				float sample_texel;
				if (vsm_smrt_face_depth(point_map, px, py, pw, mip, stored, sample_texel)) {
					float z_prev = (point_map == prev_map) ? prev_w : vsm_row(vsm_maps.data[point_map].row3, prev_point);
					float bias = sample_texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
					float step_dz = max(z_prev - pw, 0.0);
					float thickness = max(step_dz * 0.5, sample_texel * 2.0);
					bool behind = stored < pw - bias;
					occluded = (behind && (pw - stored <= thickness)) || (prev_behind && !behind && (stored < z_prev - bias)) || (step_index + 1u == steps && behind);
					prev_behind = behind;
				} else {
					prev_behind = false;
				}
				prev_map = point_map;
				prev_w = pw;
			} else {
				prev_behind = false;
			}
			prev_point = point;
		}
		lit += occluded ? 0u : 1u;
	}
	return float(lit) / float(rays);
}

// The distance from the first occluder toward the light to the receiver along the light, for
// transmittance, or -1 without a mapped page (the caller keeps its default). Directional lights.
float vsm_transmittance_directional(uint p_map_base, uint p_light_flags, vec3 p_world, float p_view_depth) {
	uint table_maps = vsm_projection.data.counts.x;
	uint count = (p_light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint first = vsm_light_map(p_map_base, p_light_flags, 0u);
	if (first >= table_maps || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
		return -1.0;
	}
	VSMMapData level0 = vsm_maps.data[first];
	ivec2 texel0 = vsm_clipmap_texel0(level0, p_world);
	VSMLookup result;
	if (sc_use_vsm_coarse()) {
		if (p_view_depth > level0.max_distance) {
			return -1.0;
		}
		uint found_level;
		bool in_domain;
		result = vsm_clipmap_coarse_sample(table_maps, first, count, texel0, found_level, in_domain);
	} else {
		uint level;
		uint mip;
		if (!vsm_clipmap_projection_select(first, count, p_light_flags, texel0, p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y, level, mip)) {
			return -1.0;
		}
		uint found_level;
		result = vsm_clipmap_sample_level(first, count, texel0, level, mip, found_level);
	}
	if (!result.valid) {
		return -1.0;
	}
	float stored = vsm_pool_depth(result.texel);
	// GodotNext: vsm_local_lights (#216). Return abs(thickness) >= 0 so small negative front-face deltas do
	// not collide with the -1.0 unmapped sentinel (SSS() uses abs(transmittance_z) anyway).
	return stored < 1.0e30 ? abs(vsm_row(level0.row2, p_world) - stored) : -1.0;
}

// As vsm_transmittance_directional(), for spot, omni and area lights.
float vsm_transmittance_local(uint p_map_base, uint p_light_flags, vec3 p_world, float p_view_depth) {
	float w;
	uint map = vsm_face_projection_select(p_map_base, p_light_flags, p_world, w);
	if (map == VSM_MAP_NONE) {
		return -1.0;
	}
	VSMMapData face = vsm_maps.data[map];
	uint mip = VSM_COARSE_FIRST_MIP;
	if (!sc_use_vsm_coarse()) {
		float footprint = p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y;
		int res_int_bias;
		float res_scale;
		vsm_resolution_bias_parts(p_light_flags, res_int_bias, res_scale);
		mip = vsm_biased_mip(int(vsm_face_mip(w * face.texel_scale, footprint * res_scale)), vsm_projection.data.bias.y + face.lod_bias + res_int_bias + int(vsm_projection_overflow_bias(map)));
		if ((p_light_flags & VSM_LIGHT_FLAGS_DISTANT_COARSE) != 0u && mip < VSM_COARSE_FIRST_MIP) {
			mip = VSM_COARSE_FIRST_MIP;
		}
	}
	float x_ndc = vsm_row(face.row0, p_world) / w;
	float y_ndc = vsm_row(face.row1, p_world) / w;
	vec2 uv = vec2(x_ndc * 0.5 + 0.5, y_ndc * 0.5 + 0.5);
	VSMLookup result = vsm_lookup(vsm_projection.data.counts.x, map, uv, mip);
	if (!result.valid) {
		return -1.0;
	}
	float stored = vsm_pool_depth(result.texel);
	if (!(stored < 1.0e30)) {
		return -1.0;
	}
	float thickness = abs(w - stored);
	if (face.kind != VSM_MAP_KIND_SPOT) {
		// GodotNext: vsm_local_lights (#216). Omni and area cube faces store axial depth along the face
		// normal; convert to Euclidean radial thickness so SSS matches Godot's dual-paraboloid atlas.
		thickness *= sqrt(1.0 + x_ndc * x_ndc + y_ndc * y_ndc);
	}
	return thickness;
}
