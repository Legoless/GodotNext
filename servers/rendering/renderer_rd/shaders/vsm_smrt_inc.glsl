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

uint vsm_projection_overflow_bias(uint p_map) {
	return vsm_map_state.data[VSM_MAP_STATE_BIAS * vsm_projection.data.counts.x + p_map] & 0xFFu;
}

// The nearest depth of a pool texel: the larger packed value of the static and dynamic layers.
float vsm_pool_depth(uvec2 p_texel) {
	uint static_depth = texelFetch(usampler2DArray(vsm_pool_tex, SAMPLER_NEAREST_CLAMP), ivec3(ivec2(p_texel), 0), 0).r;
	uint dynamic_depth = texelFetch(usampler2DArray(vsm_pool_tex, SAMPLER_NEAREST_CLAMP), ivec3(ivec2(p_texel), 1), 0).r;
	return vsm_depth_unpack(max(static_depth, dynamic_depth));
}

// RendererVirtualShadowMapStorage::clipmap_projection_select(): the marking pass's fine request.
bool vsm_clipmap_projection_select(uint p_first, uint p_count, ivec2 p_texel0, float p_footprint, out uint r_level, out uint r_mip) {
	uint table_maps = vsm_projection.data.counts.x;
	// GodotNext: vsm_settings_debug (#221). Plus the light's moving bias, held by level 0, as the marking pass.
	int map_bias = p_first < table_maps ? vsm_maps.data[p_first].lod_bias : 0;
	int level_mip = vsm_clipmap_level_mip(p_footprint, vsm_projection.data.bias.x + map_bias);
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

// _smrt_clipmap_occluded(): whether level 0 texel position p_t lies behind the stored depth.
bool vsm_smrt_clipmap_occluded(uint p_first, uint p_count, vec2 p_t, float p_depth, uint p_level, uint p_mip) {
	ivec2 texel0 = ivec2(clamp(floor(p_t), vec2(-1073741824.0), vec2(1073741824.0)));
	uint level;
	VSMLookup result = vsm_clipmap_sample_level(p_first, p_count, texel0, p_level, p_mip, level);
	if (!result.valid) {
		return false;
	}
	float texel = exp2(float(int(level + result.mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	return vsm_pool_depth(result.texel) < p_depth - texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
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
			sum += stored;
			sum_squares += stored * stored;
			stored_count++;
		}
	}
	if (stored_count == 0u) {
		return 1.0;
	}
	float coverage = float(stored_count) / float(VSM_PREFILTERED_DISTANT_TAPS * VSM_PREFILTERED_DISTANT_TAPS);
	float mean = sum / float(stored_count);
	float min_variance = texel * VSM_PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS * texel * VSM_PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS;
	float variance = max(sum_squares / float(stored_count) - mean * mean, min_variance);
	float d = p_depth - texel * VSM_SMRT_DEPTH_BIAS_TEXELS - mean;
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
	float footprint = p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y;
	uint level;
	uint mip;
	if (!vsm_clipmap_projection_select(first, count, vsm_clipmap_texel0(level0, p_world), footprint, level, mip)) {
		return 1.0;
	}
	float texel = exp2(float(int(level + mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	// Row 2 is minus the light's Z axis: the light shines along it, and the light lies along -row 2.
	vec3 toward = -level0.row2.xyz;
	float n_dot_l = clamp(dot(p_normal, toward), -1.0, 1.0);
	float offset = texel * VSM_SMRT_NORMAL_BIAS_TEXELS * sqrt(max(1.0 - n_dot_l * n_dot_l, 0.0));
	vec3 receiver = p_world + p_normal * offset;
	vec2 t = vec2(vsm_row(level0.row0, receiver), vsm_row(level0.row1, receiver));
	float depth = vsm_row(level0.row2, receiver);
	float texels_per_meter = exp2(float(-VSM_CLIPMAP_TEXEL0_EXPONENT));

	if (vsm_projection.data.misc.x != 0u && count > VSM_PREFILTERED_DISTANT_LEVELS && level + VSM_PREFILTERED_DISTANT_LEVELS >= count) {
		return vsm_prefiltered_distant(first, count, t, depth, level, mip);
	}
	uint rays = min(vsm_projection.data.counts.z, VSM_SMRT_MAX_RAYS);
	if (rays == 0u || !(p_tan_half_angle > 0.0)) {
		// One 2 x 2 filtered tap on the sampled level and mip.
		float scale = exp2(float(level + mip));
		vec2 grid = t / scale - vec2(0.5);
		vec2 base = floor(grid);
		vec2 f = grid - base;
		float lit = 0.0;
		for (uint corner = 0u; corner < 4u; corner++) {
			vec2 corner_offset = vec2(float(corner & 1u), float(corner >> 1u));
			vec2 sample_t = (base + corner_offset + vec2(0.5)) * scale;
			float weight = ((corner & 1u) != 0u ? f.x : 1.0 - f.x) * ((corner >> 1u) != 0u ? f.y : 1.0 - f.y);
			lit += vsm_smrt_clipmap_occluded(first, count, sample_t, depth, level, mip) ? 0.0 : weight;
		}
		return lit;
	}

	uint steps = clamp(vsm_projection.data.counts.w, 1u, VSM_SMRT_MAX_STEPS);
	float first_height = texel * VSM_SMRT_FIRST_STEP_TEXELS;
	float level_extent = VSM_CLIPMAP_LEVEL0_EXTENT * exp2(float(level));
	uint lit = 0u;
	for (uint ray = 0u; ray < rays; ray++) {
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
		for (uint step_index = 0u; step_index < steps && !occluded; step_index++) {
			float height = vsm_smrt_step(step_index, steps, jitter, first_height, last_height);
			vec2 sample_t = t + slope * (height * texels_per_meter);
			occluded = vsm_smrt_clipmap_occluded(first, count, sample_t, depth - height, level, mip);
		}
		lit += occluded ? 0u : 1u;
	}
	return float(lit) / float(rays);
}

// RendererVirtualShadowMapStorage::face_projection_select(): the face that holds p_world, the one with the
// largest w where faces overlap.
uint vsm_face_projection_select(uint p_map_base, uint p_light_flags, vec3 p_world, out float r_w) {
	uint table_maps = vsm_projection.data.counts.x;
	uint count = (p_light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint best = VSM_MAP_NONE;
	r_w = 0.0;
	for (uint face = 0u; face < count; face++) {
		uint map = vsm_light_map(p_map_base, p_light_flags, face);
		if (map >= table_maps || vsm_maps.data[map].kind == VSM_MAP_KIND_NONE || vsm_maps.data[map].kind == VSM_MAP_KIND_DIRECTIONAL) {
			continue;
		}
		float w;
		uvec2 texel;
		if (vsm_face_texel(vsm_maps.data[map], p_world, w, texel) && (best == VSM_MAP_NONE || w > r_w)) {
			best = map;
			r_w = w;
		}
	}
	return best;
}

// _smrt_face(): the face of a sample point (p_map when it holds the point, else for omni and area lights
// the face with the largest w), or VSM_MAP_NONE.
uint vsm_smrt_face(uint p_map_base, uint p_light_flags, uint p_map, vec3 p_point, out float r_x, out float r_y, out float r_w) {
	VSMMapData data = vsm_maps.data[p_map];
	r_x = vsm_row(data.row0, p_point);
	r_y = vsm_row(data.row1, p_point);
	r_w = vsm_row(data.row3, p_point);
	if (r_w > data.near_w && abs(r_x) <= r_w && abs(r_y) <= r_w) {
		return p_map;
	}
	if ((p_light_flags & VSM_LIGHT_FLAGS_KIND_MASK) == VSM_MAP_KIND_SPOT) {
		return VSM_MAP_NONE;
	}
	float w;
	uint map = vsm_face_projection_select(p_map_base, p_light_flags, p_point, w);
	if (map != VSM_MAP_NONE) {
		VSMMapData other = vsm_maps.data[map];
		r_x = vsm_row(other.row0, p_point);
		r_y = vsm_row(other.row1, p_point);
		r_w = w;
	}
	return map;
}

// _smrt_face_occluded(): whether a face point (depth w) lies behind the stored depth.
bool vsm_smrt_face_occluded(uint p_map, float p_x, float p_y, float p_w, uint p_mip) {
	vec2 uv = vec2(p_x / p_w * 0.5 + 0.5, p_y / p_w * 0.5 + 0.5);
	VSMLookup result = vsm_lookup(vsm_projection.data.counts.x, p_map, uv, p_mip);
	if (!result.valid) {
		return false;
	}
	float texel = vsm_maps.data[p_map].texel_scale * p_w * float(1u << result.mip);
	return vsm_pool_depth(result.texel) < p_w - texel * VSM_SMRT_DEPTH_BIAS_TEXELS;
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
	// GodotNext: vsm_settings_debug (#221). Plus the light's moving bias, as the marking pass.
	int bias = vsm_projection.data.bias.y + face.lod_bias + int(vsm_projection_overflow_bias(map));
	uint mip = vsm_biased_mip(int(vsm_face_mip(w * face.texel_scale, footprint)), bias);
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
	uint rays = min(vsm_projection.data.counts.y, VSM_SMRT_MAX_RAYS);
	if (rays == 0u || (!area && !(p_radius > 0.0))) {
		// One 2 x 2 filtered tap at the receiver's mip.
		float mip_size = float(VSM_VIRTUAL_SIZE >> mip);
		vec2 grid = vec2(x / rw * 0.5 + 0.5, y / rw * 0.5 + 0.5) * mip_size - vec2(0.5);
		vec2 base = floor(grid);
		vec2 f = grid - base;
		float lit = 0.0;
		for (uint corner = 0u; corner < 4u; corner++) {
			vec2 uv = (base + vec2(float(corner & 1u), float(corner >> 1u)) + vec2(0.5)) / mip_size;
			float weight = ((corner & 1u) != 0u ? f.x : 1.0 - f.x) * ((corner >> 1u) != 0u ? f.y : 1.0 - f.y);
			lit += vsm_smrt_face_occluded(receiver_map, (uv.x * 2.0 - 1.0) * rw, (uv.y * 2.0 - 1.0) * rw, rw, mip) ? 0.0 : weight;
		}
		return lit;
	}

	// A frame across the direction to the light, for points of a spot or omni light's disk.
	vec3 up = abs(light_dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(light_dir, up));
	vec3 bitangent = cross(light_dir, tangent);
	// The angle one texel of the receiver's mip spans, seen from the light.
	float texel_angle = face.texel_scale * float(1u << mip);
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
			// s * offset / ((1 - s) * distance): it reaches VSM_SMRT_MAX_TEXELS texels at s = c / (1 + c).
			float c = VSM_SMRT_MAX_TEXELS * texel_angle * light_distance / offset_length;
			last = min(c / (1.0 + c), VSM_SMRT_LOCAL_MAX_REACH);
		}
		last = max(last, first);
		float jitter = vsm_smrt_jitter(ray, p_noise);
		bool occluded = false;
		for (uint step_index = 0u; step_index < steps && !occluded; step_index++) {
			vec3 point = receiver + ray_vector * vsm_smrt_step(step_index, steps, jitter, first, last);
			float px;
			float py;
			float pw;
			uint point_map = vsm_smrt_face(p_map_base, p_light_flags, receiver_map, point, px, py, pw);
			if (point_map != VSM_MAP_NONE) {
				occluded = vsm_smrt_face_occluded(point_map, px, py, pw, mip);
			}
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
	uint level;
	uint mip;
	if (!vsm_clipmap_projection_select(first, count, texel0, p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y, level, mip)) {
		return -1.0;
	}
	uint found_level;
	VSMLookup result = vsm_clipmap_sample_level(first, count, texel0, level, mip, found_level);
	if (!result.valid) {
		return -1.0;
	}
	float stored = vsm_pool_depth(result.texel);
	return stored < 1.0e30 ? vsm_row(level0.row2, p_world) - stored : -1.0;
}

// As vsm_transmittance_directional(), for spot, omni and area lights (the distance along the face axis).
float vsm_transmittance_local(uint p_map_base, uint p_light_flags, vec3 p_world, float p_view_depth) {
	float w;
	uint map = vsm_face_projection_select(p_map_base, p_light_flags, p_world, w);
	if (map == VSM_MAP_NONE) {
		return -1.0;
	}
	VSMMapData face = vsm_maps.data[map];
	float footprint = p_view_depth * vsm_projection.data.footprint.x + vsm_projection.data.footprint.y;
	uint mip = vsm_biased_mip(int(vsm_face_mip(w * face.texel_scale, footprint)), vsm_projection.data.bias.y + face.lod_bias + int(vsm_projection_overflow_bias(map)));
	vec2 uv = vec2(vsm_row(face.row0, p_world) / w * 0.5 + 0.5, vsm_row(face.row1, p_world) / w * 0.5 + 0.5);
	VSMLookup result = vsm_lookup(vsm_projection.data.counts.x, map, uv, mip);
	if (!result.valid) {
		return -1.0;
	}
	float stored = vsm_pool_depth(result.texel);
	return stored < 1.0e30 ? w - stored : -1.0;
}
