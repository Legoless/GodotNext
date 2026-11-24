#[vertex]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11, GN-6, GN-12, N5).
// Bounded hardware stream rasterizer for diced sub-patches whose micro-triangles exceed the SW edge
// threshold or straddle the near plane. Shares the exact crack-free vertex evaluation, displacement(),
// winding, coverage, depth offset, and VIS64 / VSM pool atomicMax writes with tessellation_dice.glsl.

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_HW_VERTEX
#define NEXTGEO_TESSELLATION

// clang-format off
#include "raster_material_inc.glsl"
// clang-format on

layout(location = 0) out flat uint vis_id_out;

const uint RASTER_BIN_FLAG_NO_CULL = 1u << 0u;

void collapse_triangle() {
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
	vis_id_out = 0xFFFFFFFFu;
}

void main() {
	uint hw_idx = uint(gl_InstanceIndex);
	if (hw_idx >= bin_push.hw_bin_capacity) {
		collapse_triangle();
		return;
	}
	uint patch_slot = tess_hw_patches.data[bin_push.hw_bin_base + hw_idx];
	if (patch_slot >= bin_push.patch_capacity) {
		collapse_triangle();
		return;
	}
	NextGeoDicedPatchRecord patch_rec = diced_patches.data[patch_slot];
	uint m = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) - m * 3u;
	uint micro_count = patch_rec.tri_and_sub >> 16u;
	if (m >= micro_count) {
		collapse_triangle();
		return;
	}

	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(patch_rec.cluster_slot, entry, desc, cull_flags)) {
		collapse_triangle();
		return;
	}
	_NgTessSourceTri src;
	uint src_tri = patch_rec.tri_and_sub & 0xFFu;
	if (!_ng_tess_load_source_tri(desc, src_tri, src)) {
		collapse_triangle();
		return;
	}
	uint f0 = patch_rec.packed_factors & 0xFFu;
	uint f1 = (patch_rec.packed_factors >> 8u) & 0xFFu;
	uint f2 = (patch_rec.packed_factors >> 16u) & 0xFFu;
	NextGeoInstanceRecord record = nextgeo_records.data[entry.x];
	_ng_tess_eval_micro_triangle(entry.x, record, src, f0, f1, f2, patch_rec.micro_tri_base + m, patch_rec.disp_magnitude, patch_rec.disp_center);

	if ((bin_push.flags & RASTER_BIN_FLAG_NO_CULL) != 0u || (cull_flags & NEXTGEO_BUCKET_CULL_DISABLED) != 0u) {
		if (determinant(mat3(_ng_clip_c[0].xyw, _ng_clip_c[1].xyw, _ng_clip_c[2].xyw)) < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	} else {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		float base_cull_sign = _ng_vsm_reverse_cull ? 1.0 : -1.0;
#else
		float base_cull_sign = raster.cull_sign;
#endif
		float effective_cull_sign = ((cull_flags & NEXTGEO_BUCKET_CULL_FRONT) != 0u) ? -base_cull_sign : base_cull_sign;
		mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
		if (determinant(basis) * effective_cull_sign < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	}

	gl_Position = _ng_clip_c[corner];
	vis_id_out = nextgeo_vis_pack_patch_id(patch_slot, m);
}

#[fragment]

#version 450

#VERSION_DEFINES

#ifndef NEXTGEO_RASTER_DEPTH_ONLY
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif
#endif

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_HW_FRAGMENT
#define NEXTGEO_TESSELLATION

// clang-format off
#include "raster_material_inc.glsl"
// clang-format on

layout(location = 0) in flat uint vis_id_in;

void main() {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	if (gl_HelperInvocation || vis_id_in == 0xFFFFFFFFu) {
		return;
	}
#else
	if (!(gl_FragCoord.z > 0.0) || vis_id_in == 0xFFFFFFFFu) {
		return;
	}
#endif
	uint vis_cluster = vis_id_in >> 7u;
	if (!nextgeo_vis_is_patch(vis_cluster)) {
		return;
	}
	uint patch_slot = nextgeo_vis_patch_index(vis_cluster);
	if (patch_slot >= bin_push.patch_capacity) {
		return;
	}
	NextGeoDicedPatchRecord patch_rec = diced_patches.data[patch_slot];
	uint m = vis_id_in & 0x7Fu;
	if (m >= (patch_rec.tri_and_sub >> 16u)) {
		return;
	}

	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(patch_rec.cluster_slot, entry, desc, cull_flags)) {
		return;
	}
	_NgTessSourceTri src;
	uint src_tri = patch_rec.tri_and_sub & 0xFFu;
	if (!_ng_tess_load_source_tri(desc, src_tri, src)) {
		return;
	}
	uint f0 = patch_rec.packed_factors & 0xFFu;
	uint f1 = (patch_rec.packed_factors >> 8u) & 0xFFu;
	uint f2 = (patch_rec.packed_factors >> 16u) & 0xFFu;
	NextGeoInstanceRecord record = nextgeo_records.data[entry.x];
	_ng_tess_eval_micro_triangle(entry.x, record, src, f0, f1, f2, patch_rec.micro_tri_base + m, patch_rec.disp_magnitude, patch_rec.disp_center);

	_ng_raster_interpolate(gl_FragCoord.xy);
	_ng_raster_fragment(scene_data_block.data);
	if (!_ng_raster_covered) {
		return;
	}
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	if (_ng_vsm_map.kind != VSM_MAP_KIND_DIRECTIONAL && !(_ng_vsm_depth_interp >= _ng_vsm_map.near_w && _ng_vsm_depth_interp <= _ng_vsm_map.max_distance)) {
		return;
	}
	_ng_raster_write(ivec2(gl_FragCoord.xy), _ng_apply_depth_offset(scene_data_block.data, _ng_vsm_depth_interp), vis_id_in);
#else
	float _ng_depth = _ng_apply_depth_offset(scene_data_block.data, gl_FragCoord.z);
	if (!(_ng_depth > 0.0)) {
		return;
	}
	_ng_raster_write(ivec2(gl_FragCoord.xy), _ng_depth, vis_id_in);
#endif
}
