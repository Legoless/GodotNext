// GodotNext: vsm_fog_translucency (#215, plan §21.3 contract S6, GV-7). Coarse-page Virtual Shadow Map
// lookups for volumetric fog froxels and forward transparent surfaces: one depth tap per light on the
// coarse pages marked by #219, with no SMRT. A sample inside a light map's valid domain whose coarse page
// is not resident returns unshadowed (1.0) and increments the atomic missing-coarse-page counter buffer.
//
// RendererVirtualShadowMapStorage::fog_shadow_coarse_directional(), fog_shadow_coarse_local(),
// shadow_coarse_directional() and shadow_coarse_local() are the CPU models; keep them in step.
//
// Include after vsm_mark_inc.glsl, vsm_page_table_inc.glsl and vsm_raster_inc.glsl. The including shader
// declares vsm_page_table, vsm_maps (VSMMapData data[]), vsm_pool_tex (utexture2DArray), vsm_coarse_miss
// (uint data[]), and defines VSM_COARSE_NEAREST_SAMPLER to a NEAREST clamp sampler.

// The nearest depth of a pool texel: the larger packed value of the static and dynamic layers.
float vsm_pool_depth(uvec2 p_texel) {
	uint static_depth = texelFetch(usampler2DArray(vsm_pool_tex, VSM_COARSE_NEAREST_SAMPLER), ivec3(ivec2(p_texel), 0), 0).r;
	uint dynamic_depth = texelFetch(usampler2DArray(vsm_pool_tex, VSM_COARSE_NEAREST_SAMPLER), ivec3(ivec2(p_texel), 1), 0).r;
	return vsm_depth_unpack(max(static_depth, dynamic_depth));
}

// Atomically records a missing coarse page (both cumulative total and per-view counters).
// Uses a sink read of the atomic return value so D3D12 GBV preserves the writes (#277).
void vsm_record_coarse_miss(uint p_total_index, uint p_view_index) {
#ifndef VSM_NO_COARSE_MISS_ATOMICS
	uint prev_total = atomicAdd(vsm_coarse_miss.data[p_total_index], 1u);
	uint prev_view = atomicAdd(vsm_coarse_miss.data[p_view_index], 1u);
	if (prev_total == 0xFFFFFFFFu || prev_view == 0xFFFFFFFFu) {
		vsm_coarse_miss.data[VSM_COARSE_MISS_SINK] = prev_total | prev_view;
	}
#endif
}

// RendererVirtualShadowMapStorage::clipmap_coarse_sample_level(): mips VSM_CLIPMAP_RESIDENT_MIP and
// coarser in the finest level whose resident window holds p_texel0, or the coarsest in-window mip
// (< VSM_CLIPMAP_RESIDENT_MIP) in the last level's far strip. r_in_domain is true when at least one
// clipmap level's mip 0 window holds p_texel0.
VSMLookup vsm_clipmap_coarse_sample(uint p_table_maps, uint p_first, uint p_count, ivec2 p_texel0, out uint r_level, out bool r_in_domain) {
	r_level = 0u;
	r_in_domain = false;
	for (uint level = 0u; level < p_count; level++) {
		uint map = p_first + level;
		if (map >= p_table_maps) {
			break;
		}
		ivec2 origin = vsm_maps.data[map].origin;
		ivec2 level_texel = p_texel0 >> int(level);
		if (vsm_clipmap_in_window(origin, level_texel, 0u)) {
			r_in_domain = true;
		}
		VSMLookup result = vsm_lookup_clipmap(p_table_maps, map, origin, level_texel, VSM_CLIPMAP_RESIDENT_MIP);
		if (result.valid) {
			r_level = level;
			return result;
		}
	}
	if (p_count > 0u && p_first + p_count - 1u < p_table_maps) {
		uint last = p_count - 1u;
		uint map = p_first + last;
		ivec2 origin = vsm_maps.data[map].origin;
		ivec2 level_texel = p_texel0 >> int(last);
		if (vsm_clipmap_in_window(origin, level_texel, 0u) && !vsm_clipmap_in_window(origin, level_texel, VSM_CLIPMAP_RESIDENT_MIP)) {
			r_in_domain = true;
			uint resident = 0u;
			while (resident + 1u < VSM_CLIPMAP_RESIDENT_MIP && vsm_clipmap_in_window(origin, level_texel, resident + 1u)) {
				resident++;
			}
			r_level = last;
			// resident + 1 is outside the window, so vsm_lookup_clipmap() stops after resident.
			return vsm_lookup_clipmap(p_table_maps, map, origin, level_texel, resident);
		}
	}
	return vsm_lookup_miss();
}

// RendererVirtualShadowMapStorage::face_projection_select(): the face of a spot, omni or area light that
// holds p_world (the one with the largest w where faces overlap), or VSM_MAP_NONE.
uint vsm_face_select(uint p_table_maps, uint p_map_base, uint p_light_flags, vec3 p_world, out float r_w) {
	uint count = (p_light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint best = VSM_MAP_NONE;
	r_w = 0.0;
	for (uint face = 0u; face < count; face++) {
		uint map = vsm_light_map(p_map_base, p_light_flags, face);
		if (map >= p_table_maps || vsm_maps.data[map].kind == VSM_MAP_KIND_NONE || vsm_maps.data[map].kind == VSM_MAP_KIND_DIRECTIONAL) {
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

// The face of a local light's point: p_map when it holds p_point, else (omni and area) the face with the
// largest w, or VSM_MAP_NONE.
uint vsm_face_select_hint(uint p_table_maps, uint p_map_base, uint p_light_flags, uint p_map, vec3 p_point, out float r_x, out float r_y, out float r_w) {
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
	uint map = vsm_face_select(p_table_maps, p_map_base, p_light_flags, p_point, w);
	if (map != VSM_MAP_NONE) {
		VSMMapData other = vsm_maps.data[map];
		r_x = vsm_row(other.row0, p_point);
		r_y = vsm_row(other.row1, p_point);
		r_w = w;
	}
	return map;
}

// Volumetric fog directional coarse lookup (RendererVirtualShadowMapStorage::fog_shadow_coarse_directional()):
// one tap on coarse pages, with Godot's exponential depth fade (p_inv_fog_fade).
float vsm_fog_shadow_coarse_directional(uint p_table_maps, uint p_map_base, uint p_light_flags, vec3 p_world, float p_view_depth, float p_inv_fog_fade) {
	uint count = (p_light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint first = vsm_light_map(p_map_base, p_light_flags, 0u);
	if (first >= p_table_maps || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
		return 1.0;
	}
	VSMMapData level0 = vsm_maps.data[first];
	if (p_view_depth > level0.max_distance) {
		return 1.0;
	}
	uint found_level;
	bool in_domain;
	VSMLookup result = vsm_clipmap_coarse_sample(p_table_maps, first, count, vsm_clipmap_texel0(level0, p_world), found_level, in_domain);
	if (!result.valid) {
		if (in_domain) {
			vsm_record_coarse_miss(VSM_COARSE_MISS_FOG_TOTAL, VSM_COARSE_MISS_FOG_VIEW);
		}
		return 1.0;
	}
	float stored = vsm_pool_depth(result.texel);
	if (!(stored < 1.0e30)) {
		return 1.0;
	}
	float receiver_depth = vsm_row(level0.row2, p_world);
	return exp(min(0.0, stored - receiver_depth) * p_inv_fog_fade);
}

// Volumetric fog positional coarse lookup (RendererVirtualShadowMapStorage::fog_shadow_coarse_local()):
// one tap on coarse pages of a spot, omni or area light, with exponential depth fade.
float vsm_fog_shadow_coarse_local(uint p_table_maps, uint p_map_base, uint p_light_flags, vec3 p_world, float p_inv_fog_fade) {
	float w;
	uint map = vsm_face_select(p_table_maps, p_map_base, p_light_flags, p_world, w);
	if (map == VSM_MAP_NONE) {
		return 1.0;
	}
	VSMMapData face = vsm_maps.data[map];
	vec2 uv = vec2(vsm_row(face.row0, p_world) / w * 0.5 + 0.5, vsm_row(face.row1, p_world) / w * 0.5 + 0.5);
	VSMLookup result = vsm_lookup(p_table_maps, map, uv, VSM_COARSE_FIRST_MIP);
	if (!result.valid) {
		vsm_record_coarse_miss(VSM_COARSE_MISS_FOG_TOTAL, VSM_COARSE_MISS_FOG_VIEW);
		return 1.0;
	}
	float stored = vsm_pool_depth(result.texel);
	if (!(stored < 1.0e30)) {
		return 1.0;
	}
	return exp(min(0.0, stored - w) * p_inv_fog_fade);
}

// Forward transparent directional coarse lookup (RendererVirtualShadowMapStorage::shadow_coarse_directional()):
// one depth tap on coarse pages, normal and depth biased using the resolved resident coarse mip.
float vsm_shadow_coarse_directional(uint p_table_maps, uint p_map_base, uint p_light_flags, vec3 p_world, vec3 p_normal, float p_view_depth) {
	uint count = (p_light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint first = vsm_light_map(p_map_base, p_light_flags, 0u);
	if (first >= p_table_maps || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
		return 1.0;
	}
	VSMMapData level0 = vsm_maps.data[first];
	if (p_view_depth > level0.max_distance) {
		return 1.0;
	}
	uint found_level;
	bool in_domain;
	VSMLookup initial = vsm_clipmap_coarse_sample(p_table_maps, first, count, vsm_clipmap_texel0(level0, p_world), found_level, in_domain);
	if (!initial.valid) {
		if (in_domain) {
			vsm_record_coarse_miss(VSM_COARSE_MISS_TRANSLUCENT_TOTAL, VSM_COARSE_MISS_TRANSLUCENT_VIEW);
		}
		return 1.0;
	}
	float texel = exp2(float(int(found_level + initial.mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	vec3 toward = -level0.row2.xyz;
	float n_dot_l = clamp(dot(p_normal, toward), -1.0, 1.0);
	float offset = texel * VSM_SMRT_NORMAL_BIAS_TEXELS * sqrt(max(1.0 - n_dot_l * n_dot_l, 0.0));
	vec3 receiver = p_world + p_normal * offset;
	uint sample_level;
	bool sample_in_domain;
	VSMLookup sample_lookup = vsm_clipmap_coarse_sample(p_table_maps, first, count, vsm_clipmap_texel0(level0, receiver), sample_level, sample_in_domain);
	if (!sample_lookup.valid) {
		sample_lookup = initial;
		sample_level = found_level;
	}
	float sample_texel = exp2(float(int(sample_level + sample_lookup.mip) + VSM_CLIPMAP_TEXEL0_EXPONENT));
	float receiver_depth = vsm_row(level0.row2, receiver);
	return vsm_pool_depth(sample_lookup.texel) < receiver_depth - sample_texel * VSM_SMRT_DEPTH_BIAS_TEXELS ? 0.0 : 1.0;
}

// Forward transparent positional coarse lookup (RendererVirtualShadowMapStorage::shadow_coarse_local()):
// one depth tap on coarse pages of a spot, omni or area light, biased using the resolved coarse mip.
float vsm_shadow_coarse_local(uint p_table_maps, uint p_map_base, uint p_light_flags, vec3 p_world, vec3 p_normal, vec3 p_light_position) {
	float w;
	uint map = vsm_face_select(p_table_maps, p_map_base, p_light_flags, p_world, w);
	if (map == VSM_MAP_NONE) {
		return 1.0;
	}
	VSMMapData face = vsm_maps.data[map];
	vec2 uv = vec2(vsm_row(face.row0, p_world) / w * 0.5 + 0.5, vsm_row(face.row1, p_world) / w * 0.5 + 0.5);
	VSMLookup initial = vsm_lookup(p_table_maps, map, uv, VSM_COARSE_FIRST_MIP);
	if (!initial.valid) {
		vsm_record_coarse_miss(VSM_COARSE_MISS_TRANSLUCENT_TOTAL, VSM_COARSE_MISS_TRANSLUCENT_VIEW);
		return 1.0;
	}
	vec3 to_light = p_light_position - p_world;
	float light_distance = length(to_light);
	if (!(light_distance > 0.0)) {
		return 1.0;
	}
	float texel = face.texel_scale * w * float(1u << initial.mip);
	vec3 light_dir = to_light / light_distance;
	float n_dot_l = clamp(dot(p_normal, light_dir), -1.0, 1.0);
	float offset = texel * VSM_SMRT_NORMAL_BIAS_TEXELS * sqrt(max(1.0 - n_dot_l * n_dot_l, 0.0));
	vec3 receiver = p_world + p_normal * offset;
	float rx;
	float ry;
	float rw;
	uint receiver_map = vsm_face_select_hint(p_table_maps, p_map_base, p_light_flags, map, receiver, rx, ry, rw);
	VSMLookup sample_lookup = vsm_lookup_miss();
	if (receiver_map != VSM_MAP_NONE) {
		vec2 sample_uv = vec2(rx / rw * 0.5 + 0.5, ry / rw * 0.5 + 0.5);
		sample_lookup = vsm_lookup(p_table_maps, receiver_map, sample_uv, VSM_COARSE_FIRST_MIP);
	}
	if (receiver_map == VSM_MAP_NONE || !sample_lookup.valid) {
		sample_lookup = initial;
		receiver_map = map;
		rw = w;
	}
	float sample_texel = vsm_maps.data[receiver_map].texel_scale * rw * float(1u << sample_lookup.mip);
	return vsm_pool_depth(sample_lookup.texel) < rw - sample_texel * VSM_SMRT_DEPTH_BIAS_TEXELS ? 0.0 : 1.0;
}
