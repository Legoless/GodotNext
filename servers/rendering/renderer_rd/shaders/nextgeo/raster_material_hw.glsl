#[vertex]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N2, N4). The hardware form of the material raster
// target: one draw per programmable raster bin over its hardware clusters, with no attachments, as
// visibility_raster.glsl's MODE_VIS64 draws the fixed-function bin. The vertex stage runs vertex() at
// the triangle's corners and keeps that path's winding; the pixel stage rebuilds the triangle's
// interpolants at the pixel center, runs the coverage subset of fragment() and writes
// atomicMax(vis64, depth << 32 | ID) for the samples it keeps. VIS64_BUFFER selects the buffer form of
// the visibility buffer.

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_HW_VERTEX

// clang-format off
#include "raster_material_inc.glsl"
// clang-format on

layout(location = 0) out flat uint vis_id_out;

const uint RASTER_BIN_FLAG_NO_CULL = 1u << 0u; // The back-face debug view rasterizes both faces.

void collapse_triangle() {
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
	vis_id_out = 0xFFFFFFFFu;
}

void main() {
	uint start = raster_bin_state.data[nextgeo_raster_bin_cursor_word(bin_push.phase, bin_push.bin, NEXTGEO_RASTER_BIN_KIND_HW)];
	uint vis_cluster = raster_bin_lists.data[bin_push.list_base + start + uint(gl_InstanceIndex)];
	uint t = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) - t * 3u;

	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(vis_cluster, entry, desc, cull_flags) || !_ng_raster_load_triangle(entry, desc, t)) {
		collapse_triangle();
		return;
	}

	// The winding of visibility_raster.glsl: a cull-disabled surface (or the back-face debug view) turns
	// every triangle to face the camera; otherwise the instance's handedness and the surface's cull bits
	// pick the kept face.
	if ((bin_push.flags & RASTER_BIN_FLAG_NO_CULL) != 0u || (cull_flags & NEXTGEO_BUCKET_CULL_DISABLED) != 0u) {
		if (determinant(mat3(_ng_clip_c[0].xyw, _ng_clip_c[1].xyw, _ng_clip_c[2].xyw)) < 0.0 && corner != 0u) {
			corner = 3u - corner;
		}
	} else {
		NextGeoInstanceRecord record = nextgeo_records.data[entry.x];
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
	vis_id_out = (vis_cluster << 7u) | (t & 0x7Fu);
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
	// Depth 0 is the far plane and the empty texel.
	if (!(gl_FragCoord.z > 0.0) || vis_id_in == 0xFFFFFFFFu) {
		return;
	}
#endif
	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(vis_id_in >> 7u, entry, desc, cull_flags) || !_ng_raster_load_triangle(entry, desc, vis_id_in & 0x7Fu)) {
		return;
	}
	_ng_raster_interpolate(gl_FragCoord.xy);
	_ng_raster_fragment(scene_data_block.data);
	if (!_ng_raster_covered) {
		return;
	}
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	_ng_raster_write(ivec2(gl_FragCoord.xy), _ng_apply_depth_offset(scene_data_block.data, _ng_vsm_depth_interp), vis_id_in);
#else
	float _ng_depth = _ng_apply_depth_offset(scene_data_block.data, gl_FragCoord.z);
	if (!(_ng_depth > 0.0)) {
		return;
	}
	_ng_raster_write(ivec2(gl_FragCoord.xy), _ng_depth, vis_id_in);
#endif
}
