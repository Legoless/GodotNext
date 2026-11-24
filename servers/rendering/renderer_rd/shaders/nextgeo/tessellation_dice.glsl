#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11, GN-6, GN-12, N5).
// Runtime displacement dicing and software rasterizer (1 64-thread workgroup per displaced cluster).
// Evaluates crack-free canonical edge factors, splits triangles into sub-patches of <= 128 micro-triangles
// occupying visibility cluster IDs [NEXTGEO_VIS_PATCH_BASE .. NEXTGEO_VIS_VOXEL_BASE - 1], evaluates user
// displacement() via #150's compiler hook, SW-rasterizes small micro-triangles directly into VIS64 (or VSM
// vsm_pool under NEXTGEO_RASTER_DEPTH_ONLY) in the same workgroup, and routes patches with micro-triangles
// exceeding the SW edge threshold (or straddling the near plane) to the bounded HW patch stream.

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
#define NEXTGEO_TESSELLATION

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "raster_material_inc.glsl"
#include "sw_raster_setup_inc.glsl"
// clang-format on

const uint RASTER_BIN_FLAG_NO_CULL = 1u << 0u;
const uint RASTER_BIN_FLAG_ORTHO = 1u << 3u;

shared uint s_tri_factors[128];
shared uint s_tri_total_micro[128];
shared uint s_tri_patch_base[128];
shared uint s_tri_patch_count[128];
shared uint s_coop_tris[128];
shared uint s_coop_count;
shared uint s_patch_needs_hw;
shared uint s_patch_hw_fallback;
shared uint s_patch_vsm_slot;

// #277: D3D12 GBV requires every 32-bit atomic's return value to be used.
void _ng_tess_sink(uint p_val) {
	if (p_val == 0xFFFFFFFFu) {
		tess_state.sink = 1u;
	}
}

void _ng_tess_sw_raster_current(int p_facing, uint p_visibility_id) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	bool is_directional = _ng_vsm_map.kind == VSM_MAP_KIND_DIRECTIONAL;
	if (!is_directional && (!(_ng_clip_c[0].w > 0.0) || !(_ng_clip_c[1].w > 0.0) || !(_ng_clip_c[2].w > 0.0))) {
		return;
	}
	ivec2 target_size = ivec2(int(VSM_PAGE_SIZE));
	vec3 a = nextgeo_sw_clip_to_pixel(_ng_clip_c[0], target_size);
	vec3 b = nextgeo_sw_clip_to_pixel(_ng_clip_c[1], target_size);
	vec3 c = nextgeo_sw_clip_to_pixel(_ng_clip_c[2], target_size);
	vec3 z_vals = is_directional
			? vec3(_ng_vsm_depth_c[0], _ng_vsm_depth_c[1], _ng_vsm_depth_c[2])
			: vec3(1.0 / _ng_clip_c[0].w, 1.0 / _ng_clip_c[1].w, 1.0 / _ng_clip_c[2].w);
	NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(a.xy, b.xy, c.xy, z_vals, target_size, p_facing);
	if (!tri.valid) {
		return;
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
	if (!(_ng_clip_c[0].w > 0.0) || !(_ng_clip_c[1].w > 0.0) || !(_ng_clip_c[2].w > 0.0)) {
		return;
	}
	vec3 a = nextgeo_sw_clip_to_pixel(_ng_clip_c[0], bin_push.size);
	vec3 b = nextgeo_sw_clip_to_pixel(_ng_clip_c[1], bin_push.size);
	vec3 c = nextgeo_sw_clip_to_pixel(_ng_clip_c[2], bin_push.size);
	NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(a.xy, b.xy, c.xy, vec3(a.z, b.z, c.z), bin_push.size, p_facing);
	if (!tri.valid) {
		return;
	}
	for (int y = tri.rect_min.y; y <= tri.rect_max.y; y++) {
		for (int x = tri.rect_min.x; x <= tri.rect_max.x; x++) {
			float depth;
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
			_ng_raster_write(ivec2(x, y), depth, p_visibility_id);
		}
	}
#endif
}

bool _ng_tess_micro_needs_hw(float p_sw_threshold) {
	if (!(p_sw_threshold > 0.0)) {
		return true;
	}
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	bool is_directional = _ng_vsm_map.kind == VSM_MAP_KIND_DIRECTIONAL;
	if (!is_directional && (!(_ng_clip_c[0].w > 0.0) || !(_ng_clip_c[1].w > 0.0) || !(_ng_clip_c[2].w > 0.0))) {
		return true;
	}
	ivec2 target_size = ivec2(int(VSM_PAGE_SIZE));
#else
	if (!(_ng_clip_c[0].w > 0.0) || !(_ng_clip_c[1].w > 0.0) || !(_ng_clip_c[2].w > 0.0)) {
		return true;
	}
	ivec2 target_size = bin_push.size;
#endif
	vec3 a = nextgeo_sw_clip_to_pixel(_ng_clip_c[0], target_size);
	vec3 b = nextgeo_sw_clip_to_pixel(_ng_clip_c[1], target_size);
	vec3 c = nextgeo_sw_clip_to_pixel(_ng_clip_c[2], target_size);
	float e0 = length(b.xy - a.xy);
	float e1 = length(c.xy - b.xy);
	float e2 = length(a.xy - c.xy);
	float max_e = max(e0, max(e1, e2));
	float max_sw = min(p_sw_threshold, 64.0);
	return !(max_e <= max_sw);
}

bool _ng_tess_emit_hw_patch(uint p_patch_slot, uint p_bin_slot) {
	if (bin_push.hw_bin_capacity == 0u) {
		_ng_tess_sink(atomicAdd(tess_state.hw_stream_overflow, 1u));
		_ng_tess_sink(atomicAdd(tess_state.dicing_overflow, 1u));
		return false;
	}
	uint hw_local = atomicAdd(tess_state.hw_bin_alloc[p_bin_slot], 1u);
	if (hw_local < bin_push.hw_bin_capacity) {
		tess_hw_patches.data[bin_push.hw_bin_base + hw_local] = p_patch_slot;
		_ng_tess_sink(atomicMax(tess_hw_args.data[p_bin_slot * 4u + 1u], hw_local + 1u));
		_ng_tess_sink(atomicAdd(tess_state.hw_patches, 1u));
		return true;
	}
	_ng_tess_sink(atomicAdd(tess_state.hw_stream_overflow, 1u));
	_ng_tess_sink(atomicAdd(tess_state.dicing_overflow, 1u));
	return false;
}

void main() {
	uint local_index = gl_WorkGroupID.y * NEXTGEO_SW_DISPATCH_WIDTH + gl_WorkGroupID.x;
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
	uint tri_count = min((desc.z >> 16u) & 0xFFu, 128u);
	if (tri_count == 0u) {
		return;
	}

	uint tid = gl_LocalInvocationIndex;
	if (tid == 0u) {
		s_coop_count = 0u;
		_ng_tess_sink(atomicAdd(tess_state.diced_clusters, 1u));
	}
	barrier();

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

	mat3x4 cur_xform = mat3x4(record.transform[0], record.transform[1], record.transform[2]);
	uint bin_slot = (bin_push.flags >> 16u) & 0x3Fu;
	uint patch_cap = bin_push.patch_capacity;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	bool vsm_is_dir = _ng_vsm_map.kind == VSM_MAP_KIND_DIRECTIONAL;
	float vsm_page_texel = _ng_vsm_map.texel_scale * float(1u << _ng_vsm_mip);
	vec3 vsm_light_pos = vsm_is_dir ? vec3(0.0) : _ng_tess_vsm_light_origin(_ng_vsm_map);
#else
	bool is_ortho = (bin_push.flags & RASTER_BIN_FLAG_ORTHO) != 0u;
	vec3 cam_pos = vec3(
			scene_data_block.data.inv_view_matrix[0].w,
			scene_data_block.data.inv_view_matrix[1].w,
			scene_data_block.data.inv_view_matrix[2].w);
#endif

	// Phase A: compute crack-free canonical edge factors and allocate sub-patch slots per source triangle.
	for (uint t = tid; t < tri_count; t += 64u) {
		_NgTessSourceTri src;
		if (!_ng_tess_load_source_tri(desc, t, src)) {
			s_tri_total_micro[t] = 0u;
			s_tri_patch_count[t] = 0u;
			s_tri_patch_base[t] = 0xFFFFFFFFu;
			s_tri_factors[t] = nextgeo_tess_pack_factors(1u, 1u, 1u, 0u);
			continue;
		}
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		uint f0 = nextgeo_tess_vsm_edge_factor(src.pos[0], src.pos[1], cur_xform, vsm_light_pos, vsm_page_texel, _ng_vsm_map.near_w, vsm_is_dir, bin_push.dicing_rate);
		uint f1 = nextgeo_tess_vsm_edge_factor(src.pos[1], src.pos[2], cur_xform, vsm_light_pos, vsm_page_texel, _ng_vsm_map.near_w, vsm_is_dir, bin_push.dicing_rate);
		uint f2 = nextgeo_tess_vsm_edge_factor(src.pos[2], src.pos[0], cur_xform, vsm_light_pos, vsm_page_texel, _ng_vsm_map.near_w, vsm_is_dir, bin_push.dicing_rate);
		uint total_micro = nextgeo_tess_micro_tri_count(f0, f1, f2);
		uint needed = nextgeo_tess_patch_count(f0, f1, f2);
		uint patch_base = 0u;
		s_tri_factors[t] = nextgeo_tess_pack_factors(f0, f1, f2, 0u);
		s_tri_total_micro[t] = total_micro;
		s_tri_patch_count[t] = needed;
		s_tri_patch_base[t] = patch_base;
#else
		uint f0 = nextgeo_tess_edge_factor(src.pos[0], src.pos[1], cur_xform, cam_pos, bin_push.px_scale, bin_push.z_near, is_ortho, bin_push.dicing_rate);
		uint f1 = nextgeo_tess_edge_factor(src.pos[1], src.pos[2], cur_xform, cam_pos, bin_push.px_scale, bin_push.z_near, is_ortho, bin_push.dicing_rate);
		uint f2 = nextgeo_tess_edge_factor(src.pos[2], src.pos[0], cur_xform, cam_pos, bin_push.px_scale, bin_push.z_near, is_ortho, bin_push.dicing_rate);
		uint needed = nextgeo_tess_patch_count(f0, f1, f2);
		uint patch_base = 0xFFFFFFFFu;
		uint alloc_patches = 0u;

		if (patch_cap > 0u) {
			uint base = atomicAdd(tess_state.patch_count, needed);
			if (base + needed <= patch_cap) {
				patch_base = base;
				alloc_patches = needed;
			} else {
				_ng_tess_sink(atomicAdd(tess_state.dicing_overflow, 1u));
				if (base < patch_cap) {
					uint avail = patch_cap - base;
					uint steps = 0u;
					if (nextgeo_tess_coarsen_for_budget(f0, f1, f2, avail, steps)) {
						if (steps > 0u) {
							_ng_tess_sink(atomicAdd(tess_state.coarsened_tris, 1u));
						}
						patch_base = base;
						alloc_patches = nextgeo_tess_patch_count(f0, f1, f2);
					}
				}
			}
		} else {
			_ng_tess_sink(atomicAdd(tess_state.dicing_overflow, 1u));
		}

		uint total_micro = alloc_patches > 0u ? nextgeo_tess_micro_tri_count(f0, f1, f2) : 0u;
		s_tri_factors[t] = nextgeo_tess_pack_factors(f0, f1, f2, 0u);
		s_tri_total_micro[t] = total_micro;
		s_tri_patch_count[t] = alloc_patches;
		s_tri_patch_base[t] = patch_base;
#endif
		if (total_micro > 1u) {
			uint cidx = atomicAdd(s_coop_count, 1u);
			if (cidx < 128u) {
				s_coop_tris[cidx] = t;
			}
		}
	}
	barrier();

	// Phase B1: single-triangle (undiced f0=f1=f2=1) and overflow-fallback triangles run fully parallel across threads.
	for (uint t = tid; t < tri_count; t += 64u) {
		uint total_micro = s_tri_total_micro[t];
		if (total_micro > 1u) {
			continue;
		}
		if (total_micro == 0u) {
#ifndef NEXTGEO_RASTER_DEPTH_ONLY
			// Hard patch-buffer overflow fallback: rasterize the base cluster triangle safely with its normal cluster ID.
			if (_ng_raster_load_triangle(entry, desc, t)) {
				_ng_tess_sw_raster_current(facing, (vis_cluster << 7u) | (t & 0x7Fu));
			}
#endif
			continue;
		}
		_NgTessSourceTri src;
		if (!_ng_tess_load_source_tri(desc, t, src)) {
			continue;
		}
		_ng_tess_eval_micro_triangle(entry.x, record, src, 1u, 1u, 1u, 0u, bin_push.disp_magnitude, bin_push.disp_center);
		bool needs_hw = _ng_tess_micro_needs_hw(bin_push.sw_max_edge);
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		if (!needs_hw) {
			_ng_tess_sw_raster_current(facing, 0u);
			_ng_tess_sink(atomicAdd(tess_state.sw_micro_tris, 1u));
		} else {
			uint vslot = patch_cap > 0u ? atomicAdd(tess_state.patch_count, 1u) : patch_cap;
			if (vslot < patch_cap) {
				NextGeoDicedPatchRecord rec;
				rec.cluster_slot = vis_cluster;
				rec.tri_and_sub = nextgeo_tess_pack_tri_and_sub(t, 0u, 1u);
				rec.packed_factors = nextgeo_tess_pack_factors(1u, 1u, 1u, bin_push.bin);
				rec.micro_tri_base = 0u;
				rec.disp_magnitude = bin_push.disp_magnitude;
				rec.disp_center = bin_push.disp_center;
				rec.pad0 = bin_push.phase;
				rec.pad1 = 0u;
				diced_patches.data[vslot] = rec;
				if (!_ng_tess_emit_hw_patch(vslot, bin_slot)) {
					_ng_tess_sw_raster_current(facing, 0u);
				}
			} else {
				_ng_tess_sink(atomicAdd(tess_state.dicing_overflow, 1u));
				_ng_tess_sw_raster_current(facing, 0u);
			}
		}
#else
		uint patch_slot = s_tri_patch_base[t];
		NextGeoDicedPatchRecord rec;
		rec.cluster_slot = vis_cluster;
		rec.tri_and_sub = nextgeo_tess_pack_tri_and_sub(t, 0u, 1u);
		rec.packed_factors = nextgeo_tess_pack_factors(1u, 1u, 1u, bin_push.bin);
		rec.micro_tri_base = 0u;
		rec.disp_magnitude = bin_push.disp_magnitude;
		rec.disp_center = bin_push.disp_center;
		rec.pad0 = bin_push.phase;
		rec.pad1 = 0u;
		diced_patches.data[patch_slot] = rec;
		if (!needs_hw || !_ng_tess_emit_hw_patch(patch_slot, bin_slot)) {
			_ng_tess_sw_raster_current(facing, nextgeo_vis_pack_patch_id(patch_slot, 0u));
			_ng_tess_sink(atomicAdd(tess_state.sw_micro_tris, 1u));
		}
#endif
	}

	// Phase B2: subdivided triangles (total_micro > 1) execute workgroup-cooperatively across all 64 threads.
	uint coop_n = min(s_coop_count, 128u);
	for (uint ci = 0u; ci < coop_n; ci++) {
		uint t = s_coop_tris[ci];
		uint packed_f = s_tri_factors[t];
		uint f0 = packed_f & 0xFFu;
		uint f1 = (packed_f >> 8u) & 0xFFu;
		uint f2 = (packed_f >> 16u) & 0xFFu;
		uint total_micro = s_tri_total_micro[t];
		uint patch_count = s_tri_patch_count[t];
		uint patch_base = s_tri_patch_base[t];

		_NgTessSourceTri src;
		bool src_ok = _ng_tess_load_source_tri(desc, t, src);
		if (!src_ok) {
			continue;
		}

		for (uint sp = 0u; sp < patch_count; sp++) {
			uint micro_base = sp * NEXTGEO_TESS_MAX_PATCH_TRIANGLES;
			uint micro_count = min(NEXTGEO_TESS_MAX_PATCH_TRIANGLES, total_micro - micro_base);
			if (tid == 0u) {
				s_patch_needs_hw = 0u;
				s_patch_hw_fallback = 0u;
				s_patch_vsm_slot = 0xFFFFFFFFu;
			}
			barrier();

			// First check whether any micro-triangle in this sub-patch requires the bounded HW stream.
			bool any_hw = false;
			for (uint m = tid; m < micro_count; m += 64u) {
				_ng_tess_eval_micro_triangle(entry.x, record, src, f0, f1, f2, micro_base + m, bin_push.disp_magnitude, bin_push.disp_center);
				if (_ng_tess_micro_needs_hw(bin_push.sw_max_edge)) {
					any_hw = true;
				}
			}
			if (any_hw) {
				_ng_tess_sink(atomicOr(s_patch_needs_hw, 1u));
			}
			barrier();

			bool use_hw = (s_patch_needs_hw != 0u);
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
			if (use_hw && tid == 0u) {
				uint vslot = patch_cap > 0u ? atomicAdd(tess_state.patch_count, 1u) : patch_cap;
				if (vslot < patch_cap) {
					NextGeoDicedPatchRecord rec;
					rec.cluster_slot = vis_cluster;
					rec.tri_and_sub = nextgeo_tess_pack_tri_and_sub(t, sp, micro_count);
					rec.packed_factors = nextgeo_tess_pack_factors(f0, f1, f2, bin_push.bin);
					rec.micro_tri_base = micro_base;
					rec.disp_magnitude = bin_push.disp_magnitude;
					rec.disp_center = bin_push.disp_center;
					rec.pad0 = bin_push.phase;
					rec.pad1 = 0u;
					diced_patches.data[vslot] = rec;
					s_patch_vsm_slot = vslot;
					if (!_ng_tess_emit_hw_patch(vslot, bin_slot)) {
						s_patch_hw_fallback = 1u;
					}
				} else {
					_ng_tess_sink(atomicAdd(tess_state.dicing_overflow, 1u));
					s_patch_hw_fallback = 1u;
				}
			}
			barrier();
			if (!use_hw || s_patch_hw_fallback != 0u) {
				for (uint m = tid; m < micro_count; m += 64u) {
					_ng_tess_eval_micro_triangle(entry.x, record, src, f0, f1, f2, micro_base + m, bin_push.disp_magnitude, bin_push.disp_center);
					_ng_tess_sw_raster_current(facing, 0u);
					_ng_tess_sink(atomicAdd(tess_state.sw_micro_tris, 1u));
				}
			}
#else
			uint patch_slot = patch_base + sp;
			if (tid == 0u) {
				NextGeoDicedPatchRecord rec;
				rec.cluster_slot = vis_cluster;
				rec.tri_and_sub = nextgeo_tess_pack_tri_and_sub(t, sp, micro_count);
				rec.packed_factors = nextgeo_tess_pack_factors(f0, f1, f2, bin_push.bin);
				rec.micro_tri_base = micro_base;
				rec.disp_magnitude = bin_push.disp_magnitude;
				rec.disp_center = bin_push.disp_center;
				rec.pad0 = bin_push.phase;
				rec.pad1 = 0u;
				diced_patches.data[patch_slot] = rec;
				if (use_hw) {
					if (!_ng_tess_emit_hw_patch(patch_slot, bin_slot)) {
						s_patch_hw_fallback = 1u;
					}
				}
			}
			barrier();
			if (!use_hw || s_patch_hw_fallback != 0u) {
				for (uint m = tid; m < micro_count; m += 64u) {
					_ng_tess_eval_micro_triangle(entry.x, record, src, f0, f1, f2, micro_base + m, bin_push.disp_magnitude, bin_push.disp_center);
					_ng_tess_sw_raster_current(facing, nextgeo_vis_pack_patch_id(patch_slot, m));
					_ng_tess_sink(atomicAdd(tess_state.sw_micro_tris, 1u));
				}
			}
#endif
			barrier();
		}
	}
}
