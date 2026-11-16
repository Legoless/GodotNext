// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N2)
// Software triangle setup and edge rules, shared by the visibility software rasterizer
// (sw_raster.glsl) and the VSM page rasterizer (#217). GLSL copy of nextgeo_sw_setup_triangle() and
// nextgeo_sw_sample() in servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h, which
// test_nextgeo_sw_raster.cpp checks against an independent reference rasterizer: change both in the
// same commit.
//
// Vertices snap to 1 / 2^bits pixel: 8 bits for triangles whose bounding box spans at most 64 pixels,
// one bit less per doubling down to 1 bit, so every edge function stays inside a 32-bit integer.
// Pixel (x, y) is sampled at its center. A sample is covered when all three edge functions are
// positive, or zero on a top or left edge (framebuffer y points down).

const int NEXTGEO_SW_SUBPIXEL_BITS = 8;
const int NEXTGEO_SW_FULL_PRECISION_EXTENT_PX = 64;
const int NEXTGEO_SW_MAX_EXTENT_PX = NEXTGEO_SW_FULL_PRECISION_EXTENT_PX << (NEXTGEO_SW_SUBPIXEL_BITS - 1);

// Which faces survive (RendererRD::NextGeoSwFacing).
const int NEXTGEO_SW_KEEP_POSITIVE = 1;
const int NEXTGEO_SW_KEEP_NEGATIVE = -1;
const int NEXTGEO_SW_KEEP_BOTH = 0;

struct NextGeoSwTriangle {
	ivec2 rect_min; // Inclusive pixel rectangle, clamped to the target.
	ivec2 rect_max;
	ivec2 v0; // Fixed point, relative to rect_min << bits, wound so the area is positive.
	ivec2 v1;
	ivec2 v2;
	vec3 z;
	ivec3 bias; // Edges 0->1, 1->2, 2->0: 0 for a top or left edge, -1 otherwise.
	int area;
	int bits;
	bool valid;
};

int nextgeo_sw_edge(ivec2 p_a, ivec2 p_b, ivec2 p_p) {
	return (p_b.x - p_a.x) * (p_p.y - p_a.y) - (p_b.y - p_a.y) * (p_p.x - p_a.x);
}

int nextgeo_sw_top_left_bias(ivec2 p_a, ivec2 p_b) {
	ivec2 d = p_b - p_a;
	return (d.y < 0 || (d.y == 0 && d.x > 0)) ? 0 : -1;
}

// p_px*: framebuffer pixel coordinates, p_z: depth in [0, 1] (reversed Z).
NextGeoSwTriangle nextgeo_sw_setup_triangle(vec2 p_px0, vec2 p_px1, vec2 p_px2, vec3 p_z, ivec2 p_size, int p_facing) {
	NextGeoSwTriangle tri;
	tri.rect_min = ivec2(0);
	tri.rect_max = ivec2(-1);
	tri.v0 = ivec2(0);
	tri.v1 = ivec2(0);
	tri.v2 = ivec2(0);
	tri.z = vec3(0.0);
	tri.bias = ivec3(0);
	tri.area = 0;
	tri.bits = NEXTGEO_SW_SUBPIXEL_BITS;
	tri.valid = false;

	if (any(isnan(vec3(p_px0.x, p_px1.x, p_px2.x))) || any(isnan(vec3(p_px0.y, p_px1.y, p_px2.y))) || any(isinf(vec3(p_px0.x, p_px1.x, p_px2.x))) || any(isinf(vec3(p_px0.y, p_px1.y, p_px2.y)))) {
		return tri;
	}
	vec2 fmin = min(p_px0, min(p_px1, p_px2));
	vec2 fmax = max(p_px0, max(p_px1, p_px2));
	// Reject triangles entirely outside the target before any integer conversion.
	if (!(fmax.x >= 0.0 && fmax.y >= 0.0 && fmin.x <= float(p_size.x) && fmin.y <= float(p_size.y))) {
		return tri;
	}
	float extent = max(fmax.x - fmin.x, fmax.y - fmin.y);
	if (!(extent <= float(NEXTGEO_SW_MAX_EXTENT_PX))) {
		return tri;
	}
	int bits = NEXTGEO_SW_SUBPIXEL_BITS;
	while (bits > 1 && extent > float(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX << (NEXTGEO_SW_SUBPIXEL_BITS - bits))) {
		bits--;
	}
	float scale = float(1 << bits);
	ivec2 a0 = ivec2(roundEven(p_px0 * scale));
	ivec2 a1 = ivec2(roundEven(p_px1 * scale));
	ivec2 a2 = ivec2(roundEven(p_px2 * scale));
	int half_px = (1 << bits) >> 1;
	ivec2 smin = min(a0, min(a1, a2));
	ivec2 smax = max(a0, max(a1, a2));
	// Pixel centers sit at (p << bits) + half; ceil and floor keep only the centers inside the box.
	tri.rect_min = max(-((ivec2(half_px) - smin) >> bits), ivec2(0));
	tri.rect_max = min((smax - ivec2(half_px)) >> bits, p_size - ivec2(1));
	if (tri.rect_min.x > tri.rect_max.x || tri.rect_min.y > tri.rect_max.y) {
		return tri;
	}
	ivec2 origin = tri.rect_min << bits;
	ivec2 x0 = a0 - origin;
	ivec2 x1 = a1 - origin;
	ivec2 x2 = a2 - origin;
	int area = nextgeo_sw_edge(x0, x1, x2);
	if (area == 0 || (p_facing == NEXTGEO_SW_KEEP_POSITIVE && area < 0) || (p_facing == NEXTGEO_SW_KEEP_NEGATIVE && area > 0)) {
		return tri;
	}
	tri.v0 = x0;
	tri.z = p_z;
	if (area < 0) {
		tri.v1 = x2;
		tri.v2 = x1;
		tri.z = p_z.xzy;
		area = -area;
	} else {
		tri.v1 = x1;
		tri.v2 = x2;
	}
	tri.bias = ivec3(nextgeo_sw_top_left_bias(tri.v0, tri.v1), nextgeo_sw_top_left_bias(tri.v1, tri.v2), nextgeo_sw_top_left_bias(tri.v2, tri.v0));
	tri.area = area;
	tri.bits = bits;
	tri.valid = true;
	return tri;
}

// Samples pixel p_pixel (inside tri.rect_min..rect_max). On coverage, r_depth is the depth plane at the
// pixel center, interpolated from the integer edge functions as the hardware interpolates z.
bool nextgeo_sw_sample(NextGeoSwTriangle p_tri, ivec2 p_pixel, out float r_depth) {
	r_depth = 0.0;
	int half_px = (1 << p_tri.bits) >> 1;
	ivec2 p = ((p_pixel - p_tri.rect_min) << p_tri.bits) + ivec2(half_px);
	int e0 = nextgeo_sw_edge(p_tri.v1, p_tri.v2, p);
	int e1 = nextgeo_sw_edge(p_tri.v2, p_tri.v0, p);
	int e2 = nextgeo_sw_edge(p_tri.v0, p_tri.v1, p);
	if (e0 + p_tri.bias.y < 0 || e1 + p_tri.bias.z < 0 || e2 + p_tri.bias.x < 0) {
		return false;
	}
	float inv_area = 1.0 / float(p_tri.area);
	r_depth = p_tri.z.x + (float(e1) * (p_tri.z.y - p_tri.z.x) + float(e2) * (p_tri.z.z - p_tri.z.x)) * inv_area;
	return true;
}

// Framebuffer pixel coordinates and depth of a clip-space position, as the viewport transform maps them.
vec3 nextgeo_sw_clip_to_pixel(vec4 p_clip, ivec2 p_size) {
	float inv_w = 1.0 / p_clip.w;
	return vec3((p_clip.xy * inv_w * 0.5 + 0.5) * vec2(p_size), p_clip.z * inv_w);
}
