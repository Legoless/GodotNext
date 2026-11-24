// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10, GN-7, GN-12, N6, MN3).
// Shared GLSL layouts and helpers for sparse 4^3 Nanite Voxel bricks stored inside `.ngm` page payloads.
// Keep in sync with servers/rendering/renderer_rd/forward_clustered/nextgeo_voxels.h.

// clang-format off
#include "visibility_inc.glsl"
// clang-format on

const uint NEXTGEO_VOXEL_BRICK_DIM = 4u;
const uint NEXTGEO_VOXEL_BRICK_VOXELS = 64u;
const uint NEXTGEO_VOXEL_BRICK_HEADER_WORDS = 8u;
const uint NEXTGEO_VOXEL_DATA_WORDS = 2u;
const uint NEXTGEO_VOXEL_SECTION_MAGIC = 0x5856474Eu; // 'NGVX'
const uint NEXTGEO_VOXEL_BRICK_FLAG_UNIFORM_SURFACE = 1u << 24u;

uint nextgeo_voxel_rank(uint p_occ_lo, uint p_occ_hi, uint p_voxel_idx) {
	if (p_voxel_idx < 32u) {
		uint mask = (1u << p_voxel_idx) - 1u;
		return uint(bitCount(p_occ_lo & mask));
	}
	uint mask = (1u << (p_voxel_idx - 32u)) - 1u;
	return uint(bitCount(p_occ_lo)) + uint(bitCount(p_occ_hi & mask));
}

bool nextgeo_voxel_is_occupied(uint p_occ_lo, uint p_occ_hi, uint p_voxel_idx) {
	if (p_voxel_idx >= NEXTGEO_VOXEL_BRICK_VOXELS) {
		return false;
	}
	uint word = p_voxel_idx < 32u ? p_occ_lo : p_occ_hi;
	return ((word >> (p_voxel_idx & 31u)) & 1u) != 0u;
}

vec3 nextgeo_voxel_decode_oct16(uint p_oct16) {
	int qx = bitfieldExtract(int(p_oct16), 0, 8);
	int qy = bitfieldExtract(int(p_oct16), 8, 8);
	float ox = clamp(float(qx) / 127.0, -1.0, 1.0);
	float oy = clamp(float(qy) / 127.0, -1.0, 1.0);
	float oz = 1.0 - abs(ox) - abs(oy);
	if (oz < 0.0) {
		float rx = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
		float ry = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
		ox = rx;
		oy = ry;
	}
	vec3 n = vec3(ox, oy, oz);
	float len = length(n);
	return len > 1e-8 ? (n / len) : vec3(0.0, 1.0, 0.0);
}

// Frame-stable deterministic stochastic coverage hash (bit-exact with nextgeo_voxels.h).
bool nextgeo_voxel_coverage_pass(ivec2 p_pixel, uint p_record_idx, uint p_brick_word, uint p_voxel_idx, uint p_coverage_u8) {
	if (p_coverage_u8 >= 255u) {
		return true;
	}
	if (p_coverage_u8 == 0u) {
		return false;
	}
	uint h = uint(p_pixel.x) * 374761393u + uint(p_pixel.y) * 668265263u;
	h ^= p_record_idx * 2246822519u;
	h ^= p_brick_word * 3266489917u;
	h ^= p_voxel_idx * 1597334677u;
	h ^= h >> 13u;
	h *= 1274126177u;
	h ^= h >> 16u;
	return (h % 255u) < p_coverage_u8;
}

void nextgeo_voxel_tangent_basis(vec3 p_normal, out vec3 r_tangent, out vec3 r_binormal) {
	vec3 up = abs(p_normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	r_tangent = normalize(cross(up, p_normal));
	r_binormal = normalize(cross(p_normal, r_tangent));
}

uvec3 nextgeo_voxel_coord(uint p_voxel_idx) {
	return uvec3(p_voxel_idx & 3u, (p_voxel_idx >> 2u) & 3u, (p_voxel_idx >> 4u) & 3u);
}
