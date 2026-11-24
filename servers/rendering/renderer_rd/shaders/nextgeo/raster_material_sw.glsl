#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N2, N4). The software form of the material raster
// target: one 64-thread workgroup per software cluster of a programmable raster bin. Each thread takes
// triangles of the cluster, runs vertex() at their corners, sets them up with #162's edge rules
// (sw_raster_setup_inc.glsl) and, for every covered pixel center, interpolates the varyings, runs the
// coverage subset of fragment() and writes atomicMax(vis64, depth << 32 | ID) for the samples it keeps.
// VIS64_BUFFER selects the buffer form of the visibility buffer.

#ifndef NEXTGEO_RASTER_DEPTH_ONLY
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif
#endif

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_SW

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "raster_material_inc.glsl"
#include "sw_raster_setup_inc.glsl"
// clang-format on

const uint RASTER_BIN_FLAG_NO_CULL = 1u << 0u; // The back-face debug view rasterizes both faces.

void main() {
	uint local_index = gl_WorkGroupID.y * NEXTGEO_SW_DISPATCH_WIDTH + gl_WorkGroupID.x;
	// Every early return below depends only on the workgroup's cluster.
	if (local_index >= raster_bin_state.data[nextgeo_raster_bin_count_word(bin_push.phase, bin_push.bin, NEXTGEO_RASTER_BIN_KIND_SW)]) {
		return;
	}
	uint start = raster_bin_state.data[nextgeo_raster_bin_cursor_word(bin_push.phase, bin_push.bin, NEXTGEO_RASTER_BIN_KIND_SW)];
	uint vis_cluster = raster_bin_lists.data[bin_push.list_base + start + local_index];

	uvec2 entry;
	uvec4 desc;
	uint cull_flags;
	if (!_ng_raster_cluster(vis_cluster, entry, desc, cull_flags)) {
		return;
	}
	NextGeoInstanceRecord record = nextgeo_records.data[entry.x];
	uint tri_count = (desc.z >> 16u) & 0xFFu;

	// The winding and culling of sw_raster.glsl (and vsm_raster_sw.glsl under NEXTGEO_RASTER_DEPTH_ONLY).
	int facing = NEXTGEO_SW_KEEP_BOTH;
	if ((bin_push.flags & RASTER_BIN_FLAG_NO_CULL) == 0u && (cull_flags & NEXTGEO_BUCKET_CULL_DISABLED) == 0u) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		float base_cull_sign = _ng_vsm_reverse_cull ? 1.0 : -1.0;
#else
		float base_cull_sign = raster.cull_sign;
#endif
		float effective_cull_sign = ((cull_flags & NEXTGEO_BUCKET_CULL_FRONT) != 0u) ? -base_cull_sign : base_cull_sign;
		mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
		facing = (determinant(basis) * effective_cull_sign < 0.0) ? NEXTGEO_SW_KEEP_NEGATIVE : NEXTGEO_SW_KEEP_POSITIVE;
	}

	for (uint t = gl_LocalInvocationIndex; t < tri_count; t += 64u) {
		if (!_ng_raster_load_triangle(entry, desc, t)) {
			continue;
		}
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		bool is_directional = _ng_vsm_map.kind == VSM_MAP_KIND_DIRECTIONAL;
		if (!is_directional && (!(_ng_clip_c[0].w > 0.0) || !(_ng_clip_c[1].w > 0.0) || !(_ng_clip_c[2].w > 0.0))) {
			continue;
		}
		ivec2 target_size = ivec2(int(VSM_PAGE_SIZE));
		vec3 a = nextgeo_sw_clip_to_pixel(_ng_clip_c[0], target_size);
		vec3 b = nextgeo_sw_clip_to_pixel(_ng_clip_c[1], target_size);
		vec3 c = nextgeo_sw_clip_to_pixel(_ng_clip_c[2], target_size);
		vec3 z_vals = is_directional
				? vec3(_ng_vsm_depth_c[0], _ng_vsm_depth_c[1], _ng_vsm_depth_c[2])
				: vec3(1.0 / _ng_clip_c[0].w, 1.0 / _ng_clip_c[1].w, 1.0 / _ng_clip_c[2].w);
		NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(a.xy, b.xy, c.xy, z_vals, target_size, facing);
		if (!tri.valid) {
			continue;
		}
		for (int y = tri.rect_min.y; y <= tri.rect_max.y; y++) {
			for (int x = tri.rect_min.x; x <= tri.rect_max.x; x++) {
				float z_interp;
				if (!nextgeo_sw_sample(tri, ivec2(x, y), z_interp)) {
					continue;
				}
				float depth = z_interp;
				if (!is_directional) {
					if (!(z_interp > 0.0)) {
						continue;
					}
					depth = 1.0 / z_interp;
					if (!(depth >= _ng_vsm_map.near_w && depth <= _ng_vsm_map.max_distance)) {
						continue;
					}
				}
				_ng_raster_interpolate(vec2(float(x) + 0.5, float(y) + 0.5));
				_ng_raster_fragment(scene_data_block.data);
				if (!_ng_raster_covered) {
					continue;
				}
				depth = _ng_apply_depth_offset(scene_data_block.data, depth);
				_ng_raster_write(ivec2(x, y), depth, 0u);
			}
		}
#else
		// Cluster cull routes only clusters in front of the near plane here, and vertex() may not move
		// a vertex behind the camera without leaving the software path's bounds.
		if (!(_ng_clip_c[0].w > 0.0) || !(_ng_clip_c[1].w > 0.0) || !(_ng_clip_c[2].w > 0.0)) {
			continue;
		}
		vec3 a = nextgeo_sw_clip_to_pixel(_ng_clip_c[0], bin_push.size);
		vec3 b = nextgeo_sw_clip_to_pixel(_ng_clip_c[1], bin_push.size);
		vec3 c = nextgeo_sw_clip_to_pixel(_ng_clip_c[2], bin_push.size);
		NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(a.xy, b.xy, c.xy, vec3(a.z, b.z, c.z), bin_push.size, facing);
		if (!tri.valid) {
			continue;
		}
		uint visibility_id = (vis_cluster << 7u) | (t & 0x7Fu);
		for (int y = tri.rect_min.y; y <= tri.rect_max.y; y++) {
			for (int x = tri.rect_min.x; x <= tri.rect_max.x; x++) {
				float depth;
				// Depth 0 is the far plane, where the hardware clips; it is also the empty texel.
				if (!nextgeo_sw_sample(tri, ivec2(x, y), depth) || !(depth > 0.0)) {
					continue;
				}
				_ng_raster_interpolate(vec2(float(x) + 0.5, float(y) + 0.5));
				_ng_raster_fragment(scene_data_block.data);
				if (!_ng_raster_covered) {
					continue;
				}
				depth = _ng_apply_depth_offset(scene_data_block.data, depth);
				if (!(depth > 0.0)) {
					continue;
				}
				_ng_raster_write(ivec2(x, y), depth, visibility_id);
			}
		}
#endif
	}
}
