// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7)
// The material shading bins' constants and layouts. Copy of
// servers/rendering/renderer_rd/forward_clustered/nextgeo_shading_bins.h: change both in the same commit;
// test_nextgeo_shading_bins.cpp pins the values.

// The per-bucket shading-bin word of a bucket that keeps the tile quads.
const uint NEXTGEO_SHADING_BIN_NONE = 0xFFFFFFFFu;
// Pixels per shading workgroup, and the width the indirect dispatches fold at.
const uint NEXTGEO_SHADING_GROUP_SIZE = 64u;
const uint NEXTGEO_SHADING_DISPATCH_WIDTH = 32768u;
// The bin state buffer: per bin, uvec4 (count, offset, scatter cursor, unused).
const uint NEXTGEO_SHADING_BIN_STATE_WORDS = 4u;
const uint NEXTGEO_SHADING_BIN_STATE_COUNT = 0u;
const uint NEXTGEO_SHADING_BIN_STATE_OFFSET = 1u;
const uint NEXTGEO_SHADING_BIN_STATE_CURSOR = 2u;

uint nextgeo_shading_pack_pixel(uvec2 p_pixel) {
	return (p_pixel.x & 0xFFFFu) | ((p_pixel.y & 0xFFFFu) << 16u);
}

uvec2 nextgeo_shading_unpack_pixel(uint p_packed) {
	return uvec2(p_packed & 0xFFFFu, p_packed >> 16u);
}

// ceil(count / 64) workgroups, folded over y = ceil(groups / 32768) rows of x = ceil(groups / y); an empty
// bin dispatches nothing.
uvec4 nextgeo_shading_dispatch_args(uint p_count) {
	uint groups = (p_count + NEXTGEO_SHADING_GROUP_SIZE - 1u) / NEXTGEO_SHADING_GROUP_SIZE;
	uint rows = (groups + NEXTGEO_SHADING_DISPATCH_WIDTH - 1u) / NEXTGEO_SHADING_DISPATCH_WIDTH;
	return uvec4(rows == 0u ? 0u : (groups + rows - 1u) / rows, rows, 1u, 0u);
}

// The bin slot an invocation of a folded shading dispatch p_width workgroups wide takes.
uint nextgeo_shading_slot(uvec3 p_group, uint p_width, uint p_local) {
	return (p_group.y * p_width + p_group.x) * NEXTGEO_SHADING_GROUP_SIZE + p_local;
}
