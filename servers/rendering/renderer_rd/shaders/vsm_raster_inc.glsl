// GodotNext: vsm_non_nanite (#218, plan §21.3 contract S4). Shared depth encoding and virtual-page
// clip projection for VSM rasterization and sampling. Include after vsm_mark_inc.glsl (the VSM_*
// defines come from RendererVirtualShadowMapStorage::get_glsl_defines()). Keep vsm_depth_pack() and
// vsm_depth_unpack() in step with RendererVirtualShadowMapStorage::depth_pack() and depth_unpack().

uvec2 vsm_raster_entry_page(uint entry) {
	return uvec2(entry & VSM_ENTRY_PAGE_MASK, (entry >> VSM_ENTRY_PAGE_BITS) & VSM_ENTRY_PAGE_MASK);
}

uint vsm_raster_light_map(uint map_base, uint light_flags, uint index) {
	uint count = (light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	return (map_base == VSM_MAP_NONE || index >= count) ? VSM_MAP_NONE : map_base + index;
}

// Reversed-Z order-preserving float encoding into (0, 0xFFFFFFFF): smaller (nearer) depth produces a
// strictly larger uint so imageAtomicMax keeps the nearest caster over cleared empty texels (0), while
// 0xFFFFFFFF stays reserved for #277 atomic sinks.
uint vsm_depth_pack(float depth) {
	if (!(depth == depth)) {
		return 0u;
	}
	float neg = clamp(-depth, -1.0e30, 1.0e30);
	if (neg == 0.0) {
		neg = 0.0;
	}
	uint bits = floatBitsToUint(neg);
	return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

float vsm_depth_unpack(uint packed_depth) {
	if (packed_depth == 0u) {
		return uintBitsToFloat(0x7F800000u);
	}
	uint bits = (packed_depth & 0x80000000u) != 0u ? (packed_depth & 0x7FFFFFFFu) : ~packed_depth;
	float neg = uintBitsToFloat(bits);
	return neg == 0.0 ? 0.0 : -neg;
}

// The light's camera for one map, from the map's rows: r_view takes world to view and r_inv_view view to
// world, as the light's camera gives VIEW_MATRIX and INV_VIEW_MATRIX in the shadow atlas, so vertex code
// that builds on the camera (billboards) faces the light. A face's camera is face_camera(): its rows are
// a symmetric perspective clip transform, so rows 0 and 1 are the camera's X and Y rows times the same
// cotangent and row 3 is minus its Z row. A clipmap level looks along the light (rows 0 and 1 are the
// light's X and Y axes times the level's scale, row 2 minus its Z axis) from camera_position, the
// clipmaps' center. RendererVirtualShadowMapStorage::map_view() is the CPU model.
void vsm_map_view(VSMMapData map_data, vec3 camera_position, out mat4 r_view, out mat4 r_inv_view) {
	vec3 x_axis;
	vec3 y_axis;
	vec3 z_axis;
	vec3 origin;
	if (map_data.kind == VSM_MAP_KIND_DIRECTIONAL) {
		x_axis = normalize(map_data.row0.xyz);
		y_axis = normalize(map_data.row1.xyz);
		z_axis = -map_data.row2.xyz;
		origin = camera_position;
	} else {
		float inv_cot = 1.0 / length(map_data.row0.xyz);
		x_axis = map_data.row0.xyz * inv_cot;
		y_axis = map_data.row1.xyz * inv_cot;
		z_axis = -map_data.row3.xyz;
		vec3 t = vec3(map_data.row0.w * inv_cot, map_data.row1.w * inv_cot, -map_data.row3.w);
		origin = -(x_axis * t.x + y_axis * t.y + z_axis * t.z);
	}
	r_inv_view = mat4(vec4(x_axis, 0.0), vec4(y_axis, 0.0), vec4(z_axis, 0.0), vec4(origin, 1.0));
	r_view = mat4(vec4(x_axis.x, y_axis.x, z_axis.x, 0.0),
			vec4(x_axis.y, y_axis.y, z_axis.y, 0.0),
			vec4(x_axis.z, y_axis.z, z_axis.z, 0.0),
			vec4(-dot(x_axis, origin), -dot(y_axis, origin), -dot(z_axis, origin), 1.0));
}

// Projects world into the [[-1, 1]^2 x [0, 1]] clip volume of one virtual page (mip, slot) of map_data
// for a VSM_PAGE_SIZE x VSM_PAGE_SIZE viewport, and outputs linear light-space depth r_depth.
vec4 vsm_page_clip_position(VSMMapData map_data, uint mip, uvec2 slot, vec3 world, out float r_depth) {
	if (map_data.kind == VSM_MAP_KIND_DIRECTIONAL) {
		uint mask = (VSM_PAGES_PER_SIDE >> mip) - 1u;
		ivec2 mip_origin = map_data.origin >> int(mip);
		ivec2 abs_mip_page = mip_origin + ivec2((slot - uvec2(mip_origin)) & uvec2(mask));
		float page_texels = float(VSM_PAGE_SIZE << mip);
		vec2 t = vec2(vsm_row(map_data.row0, world), vsm_row(map_data.row1, world));
		vec2 ndc = ((t - vec2(abs_mip_page) * page_texels) / page_texels) * 2.0 - 1.0;
		r_depth = vsm_row(map_data.row2, world);
		return vec4(ndc, 0.5, 1.0);
	}
	float x = vsm_row(map_data.row0, world);
	float y = vsm_row(map_data.row1, world);
	float w = vsm_row(map_data.row3, world);
	float n = float(VSM_PAGES_PER_SIDE >> mip);
	float xc = x * n + w * (n - 2.0 * float(slot.x) - 1.0);
	float yc = y * n + w * (n - 2.0 * float(slot.y) - 1.0);
	float k = map_data.max_distance / max(map_data.max_distance - map_data.near_w, 1.0e-5);
	float zc = (w - map_data.near_w) * k;
	r_depth = w;
	return vec4(xc, yc, zc, w);
}
