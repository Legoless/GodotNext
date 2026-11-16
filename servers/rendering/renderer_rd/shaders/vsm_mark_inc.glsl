// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3). Where a world position lands in a
// virtual shadow map, and the mip a request takes. Not a standalone shader: include it after
// vsm_page_table_inc.glsl (the VSM_* defines come from
// RendererVirtualShadowMapStorage::get_glsl_defines()). The CPU models are
// RendererVirtualShadowMapStorage::MapData and its row_dot(), face_texel(), face_mip(),
// clipmap_texel0(), clipmap_in_window(), clipmap_page(), clipmap_level_mip(), light_reaches() and
// light_reaches_cell();
// they use the same operation order, so keep both in step.
//
// One VSMMapData record per page-table map. Rows are (x, y, z, constant) coefficients of the world
// position.
// - Directional clipmap level: rows 0 and 1 give the absolute mip 0 virtual texel of the level (a
//   power-of-two scale of the light plane's X and Y), row 2 the depth along the light, row 3 is
//   (0, 0, 0, 1). origin is the level's window origin in absolute mip 0 pages.
// - Spot, omni and area face: rows 0 to 3 are a perspective clip transform. The map's uv is
//   (x / w, y / w) * 0.5 + 0.5 with no Y flip; a point is in the face when w > near_w, |x| <= w and
//   |y| <= w; texel_scale * w is the face's mip 0 texel in meters at the point.

struct VSMMapData {
	vec4 row0;
	vec4 row1;
	vec4 row2;
	vec4 row3;
	ivec2 origin;
	uint kind; // VSM_MAP_KIND_*, or VSM_MAP_KIND_NONE for a map no light owns.
	float texel_scale; // Clipmap: the level's mip 0 texel in meters; face: per meter of w.
	float near_w;
	float max_distance; // Clipmap: no view pixel deeper than this requests the light.
	uint index; // The clipmap level or the face.
	uint pad;
};

// ((r.x * p.x + r.y * p.y) + r.z * p.z) + r.w, in that order.
float vsm_row(vec4 r, vec3 p) {
	float sum = r.x * p.x;
	sum = sum + r.y * p.y;
	sum = sum + r.z * p.z;
	return sum + r.w;
}

// floor(log2(value)) from the exponent bits; -127 for zero and subnormals.
int vsm_float_exponent(float value) {
	int exponent = int((floatBitsToUint(value) >> 23u) & 0xFFu);
	return exponent == 0 ? -127 : exponent - 127;
}

// The coarsest face mip whose texel, texel0 * 2^mip, is not larger than the footprint (mip 0 when
// even mip 0's texel is larger).
uint vsm_face_mip(float texel0, float footprint) {
	uint mip = 0u;
	while (mip + 1u < VSM_MIP_COUNT && texel0 * float(1u << (mip + 1u)) <= footprint) {
		mip++;
	}
	return mip;
}

uint vsm_biased_mip(int mip, int bias) {
	return uint(clamp(mip + bias, 0, int(VSM_MIP_COUNT) - 1));
}

// A face's virtual texel of world, and w. False when the point is not in the face.
bool vsm_face_texel(VSMMapData map_data, vec3 world, out float r_w, out uvec2 r_texel) {
	float x = vsm_row(map_data.row0, world);
	float y = vsm_row(map_data.row1, world);
	float w = vsm_row(map_data.row3, world);
	r_w = w;
	r_texel = uvec2(0u);
	if (!(w > map_data.near_w) || abs(x) > w || abs(y) > w) {
		return false;
	}
	float u = x / w * 0.5 + 0.5;
	float v = y / w * 0.5 + 0.5;
	r_texel = min(uvec2(clamp(vec2(u, v), vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
	return true;
}

// The absolute mip 0 texel of world in a directional light's level 0; level l's is this >> l
// (arithmetic: floor division). Clamped to +-2^30 texels.
ivec2 vsm_clipmap_texel0(VSMMapData level0, vec3 world) {
	vec2 t = floor(vec2(vsm_row(level0.row0, world), vsm_row(level0.row1, world)));
	t = clamp(t, vec2(-1073741824.0), vec2(1073741824.0));
	return ivec2(t);
}

// Whether vsm_lookup_clipmap() reads a level texel at mip: inside the level and the mip's window.
bool vsm_clipmap_in_window(ivec2 origin, ivec2 level_texel, uint mip) {
	ivec2 rel = level_texel - origin * int(VSM_PAGE_SIZE);
	if (any(lessThan(rel, ivec2(0))) || any(greaterThanEqual(rel, ivec2(int(VSM_VIRTUAL_SIZE))))) {
		return false;
	}
	uvec2 offset = (uvec2(origin) & uvec2((1u << mip) - 1u)) + (uvec2(rel) >> VSM_PAGE_SIZE_SHIFT);
	return all(lessThan(offset, uvec2(VSM_PAGES_PER_SIDE)));
}

// The page slot of a level texel at a mip (the map is addressed toroidally).
uvec2 vsm_clipmap_page(ivec2 level_texel, uint mip) {
	uvec2 wrapped = uvec2(level_texel) & uvec2(VSM_VIRTUAL_SIZE - 1u);
	return (wrapped >> mip) >> VSM_PAGE_SIZE_SHIFT;
}

// level + mip of a clipmap request: a level l mip m texel is 2^(l + m + VSM_CLIPMAP_TEXEL0_EXPONENT) m,
// the largest power of two that is not larger than the footprint.
int vsm_clipmap_level_mip(float footprint, int bias) {
	return vsm_float_exponent(footprint) - VSM_CLIPMAP_TEXEL0_EXPONENT + bias;
}

// Whether a local light (LightData, view space) reaches a point; rel is the point minus the light's
// position. Omni and spot: inv_radius is 1 / range; area: 1 / (range + half the diagonal), and only
// the front hemisphere. Spot: also inside the cone (cos_angle is the cosine of the spot angle).
bool vsm_light_reaches(uint kind, vec3 rel, float inv_radius, vec3 direction, float cos_angle) {
	float d2 = rel.x * rel.x;
	d2 = d2 + rel.y * rel.y;
	d2 = d2 + rel.z * rel.z;
	if (!((d2 * inv_radius) * inv_radius < 1.0)) {
		return false;
	}
	if (kind == VSM_MAP_KIND_OMNI) {
		return true;
	}
	float c = rel.x * direction.x;
	c = c + rel.y * direction.y;
	c = c + rel.z * direction.z;
	if (kind == VSM_MAP_KIND_AREA) {
		return c > 0.0;
	}
	float limit = (cos_angle * cos_angle) * d2;
	if (cos_angle >= 0.0) {
		return c > 0.0 && c * c > limit;
	}
	return c > 0.0 || c * c < limit;
}

// Whether a local light reaches the fog cells around a froxel grid vertex (rel is the vertex minus the
// light's position; cell_size the cells' diagonal): within its range plus one diagonal, with no cone or
// hemisphere test, since a point of the cells may lie in the cone while the vertex does not.
// r_all_faces: the light is within one diagonal, so the cells around the vertex may meet every face.
bool vsm_light_reaches_cell(vec3 rel, float inv_radius, float cell_size, out bool r_all_faces) {
	float d2 = rel.x * rel.x;
	d2 = d2 + rel.y * rel.y;
	d2 = d2 + rel.z * rel.z;
	r_all_faces = d2 < cell_size * cell_size;
	float reach = 1.0 + cell_size * inv_radius;
	return (d2 * inv_radius) * inv_radius < reach * reach;
}
