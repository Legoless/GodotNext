/**************************************************************************/
/*  next_gi_rt_scene.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "next_gi_rt_scene.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf_stream.h"
#include "servers/rendering/rendering_server_globals.h"

#include <algorithm>

// GodotNext: nextgi_src_cards
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_instances.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"

namespace RendererRD {

int nextgi_rt_select_lod(const float *p_edge_lengths, uint32_t p_lod_count, float p_eps_rt) {
	int selected = -1;
	for (uint32_t i = 0; i < p_lod_count; i++) {
		if (p_edge_lengths[i] <= p_eps_rt) {
			selected = int(i);
		}
	}
	return selected;
}

bool nextgi_rt_decode_positions(uint64_t p_format, const Vector<uint8_t> &p_vertex_data, uint32_t p_vertex_count, const AABB &p_aabb, Vector<float> &r_positions) {
	r_positions.clear();
	ERR_FAIL_COND_V(!(p_format & RSE::ARRAY_FORMAT_VERTEX), false);
	ERR_FAIL_COND_V(p_format & RSE::ARRAY_FLAG_USE_2D_VERTICES, false);

	// Cross-check the position stride against the canonical offsets helper; positions sit at offset 0.
	// Without a RenderingServer (a doctest), derive the same stride directly.
	uint32_t vertex_element_size = 0;
	if (RenderingServer::get_singleton() != nullptr) {
		uint32_t offsets[RSE::ARRAY_MAX];
		uint32_t normal_element_size = 0;
		uint32_t attrib_element_size = 0;
		uint32_t skin_element_size = 0;
		// A dummy index length keeps the helper from rejecting indexed formats; only the vertex stride is read.
		RenderingServer::get_singleton()->mesh_surface_make_offsets_from_format(p_format, int(p_vertex_count), 3, offsets, vertex_element_size, normal_element_size, attrib_element_size, skin_element_size);
		ERR_FAIL_COND_V(offsets[RSE::ARRAY_VERTEX] != 0, false);
	} else {
		vertex_element_size = (p_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) ? sizeof(uint16_t) * 4 : sizeof(float) * 3;
	}
	ERR_FAIL_COND_V(uint64_t(vertex_element_size) * p_vertex_count > uint64_t(p_vertex_data.size()), false);

	r_positions.resize(int64_t(p_vertex_count) * 3);
	float *out = r_positions.ptrw();
	const uint8_t *src = p_vertex_data.ptr();
	if (p_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
		ERR_FAIL_COND_V(vertex_element_size != sizeof(uint16_t) * 4, false);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			const uint16_t *packed = reinterpret_cast<const uint16_t *>(src + uint64_t(i) * vertex_element_size);
			out[i * 3 + 0] = float(packed[0]) / 65535.0f * p_aabb.size.x + p_aabb.position.x;
			out[i * 3 + 1] = float(packed[1]) / 65535.0f * p_aabb.size.y + p_aabb.position.y;
			out[i * 3 + 2] = float(packed[2]) / 65535.0f * p_aabb.size.z + p_aabb.position.z;
		}
	} else {
		ERR_FAIL_COND_V(vertex_element_size != sizeof(float) * 3, false);
		memcpy(out, src, uint64_t(p_vertex_count) * sizeof(float) * 3);
	}
	return true;
}

void nextgi_rt_widen_indices(const Vector<uint8_t> &p_index_data, uint32_t p_index_count, bool p_is_16_bit, uint32_t p_vertex_count, Vector<uint32_t> &r_indices) {
	r_indices.clear();
	if (p_index_count == 0) {
		r_indices.resize(p_vertex_count);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			r_indices.write[i] = i;
		}
		return;
	}
	r_indices.resize(p_index_count);
	uint32_t *out = r_indices.ptrw();
	if (p_is_16_bit) {
		const uint16_t *src = reinterpret_cast<const uint16_t *>(p_index_data.ptr());
		for (uint32_t i = 0; i < p_index_count; i++) {
			out[i] = src[i];
		}
	} else {
		memcpy(out, p_index_data.ptr(), uint64_t(p_index_count) * sizeof(uint32_t));
	}
}

NextGIArenaAllocator::Allocation NextGIArenaAllocator::allocate(uint32_t p_size) {
	Allocation allocation;
	ERR_FAIL_COND_V(p_size == 0, allocation);
	const uint32_t aligned_size = (p_size + NEXTGI_RT_ALIGNMENT - 1) & ~uint32_t(NEXTGI_RT_ALIGNMENT - 1);
	ERR_FAIL_COND_V_MSG(aligned_size > chunk_size, allocation, "NextGI RT arena allocation is larger than one chunk.");

	for (uint32_t i = 0; i < free_ranges.size(); i++) {
		if (free_ranges[i].size >= aligned_size) {
			allocation.chunk = free_ranges[i].chunk;
			allocation.offset = free_ranges[i].offset;
			allocation.size = aligned_size;
			free_ranges[i].offset += aligned_size;
			free_ranges[i].size -= aligned_size;
			if (free_ranges[i].size == 0) {
				free_ranges.remove_at(i);
			}
			allocated_bytes += aligned_size;
			return allocation;
		}
	}

	allocation.chunk = chunk_count++;
	allocation.offset = 0;
	allocation.size = aligned_size;
	allocation.new_chunk = true;
	if (aligned_size < chunk_size) {
		FreeRange rest;
		rest.chunk = allocation.chunk;
		rest.offset = aligned_size;
		rest.size = chunk_size - aligned_size;
		free_ranges.push_back(rest);
	}
	allocated_bytes += aligned_size;
	return allocation;
}

void NextGIArenaAllocator::free_range(const Allocation &p_allocation) {
	if (!p_allocation.is_valid() || p_allocation.size == 0) {
		return;
	}
	allocated_bytes = allocated_bytes >= p_allocation.size ? allocated_bytes - p_allocation.size : 0;
	FreeRange freed;
	freed.chunk = p_allocation.chunk;
	freed.offset = p_allocation.offset;
	freed.size = p_allocation.size;

	// Coalesce with directly adjacent ranges of the same chunk.
	for (uint32_t i = 0; i < free_ranges.size();) {
		const FreeRange &range = free_ranges[i];
		if (range.chunk == freed.chunk && range.offset + range.size == freed.offset) {
			freed.offset = range.offset;
			freed.size += range.size;
			free_ranges.remove_at(i);
		} else if (range.chunk == freed.chunk && freed.offset + freed.size == range.offset) {
			freed.size += range.size;
			free_ranges.remove_at(i);
		} else {
			i++;
		}
	}
	free_ranges.push_back(freed);
}

void NextGIArenaAllocator::reset() {
	chunk_count = 0;
	allocated_bytes = 0;
	free_ranges.clear();
}

static _FORCE_INLINE_ uint32_t _nextgi_align16(uint32_t p_bytes) {
	return (p_bytes + NEXTGI_RT_ALIGNMENT - 1u) & ~uint32_t(NEXTGI_RT_ALIGNMENT - 1u);
}

bool nextgi_rt_plan_vgd_proxy(
		bool p_hwrt_tier_active,
		bool p_has_ray_query,
		uint32_t p_live_instances,
		const RenderingServerTypes::VirtualGeometryDesc &p_desc,
		uint32_t p_mesh_surface_count,
		const uint8_t *p_alpha_excluded_surfaces,
		const NextGIRTLegacySurfaceInput *p_legacy_surfaces,
		NextGIArenaAllocator &r_allocator,
		LocalVector<NextGIRTProxyPartUpload> &r_uploads,
		uint32_t &r_alpha_excluded_count) {
	r_uploads.clear();
	r_alpha_excluded_count = 0;
	if (!p_hwrt_tier_active || !p_has_ray_query || p_live_instances == 0) {
		return false;
	}

	const uint32_t part_count = uint32_t(p_desc.rt_proxy_parts.size() / 4);
	const uint32_t vgd_surface_count = part_count > 0 ? uint32_t(p_desc.rt_proxy_surface_ranges.size() / (part_count * 2)) : 0u;
	const uint32_t total_proxy_vertices = uint32_t(p_desc.rt_proxy_positions.size() / 3);
	const uint32_t total_proxy_indices = uint32_t(p_desc.rt_proxy_indices.size() / 4);
	const uint32_t total_proxy_triangles = total_proxy_indices / 3;
	const bool has_cards = !p_desc.card_descriptors.is_empty();
	const bool has_group_table = uint32_t(p_desc.rt_proxy_triangle_groups.size()) >= total_proxy_triangles * 2u;

	struct LegacySlice {
		uint32_t surface = 0;
		uint32_t first_triangle = 0;
		uint32_t triangle_count = 0;
	};
	LocalVector<LegacySlice> legacy_queue;
	for (uint32_t s = 0; s < p_mesh_surface_count; s++) {
		const bool is_alpha = p_alpha_excluded_surfaces != nullptr && p_alpha_excluded_surfaces[s] != 0;
		const bool is_virtual = s < 32u && ((p_desc.eligible_surface_mask & (1u << s)) != 0u);
		if (is_alpha) {
			bool had_tris = false;
			if (s < vgd_surface_count) {
				for (uint32_t p = 0; p < part_count; p++) {
					if (p_desc.rt_proxy_surface_ranges[(p * vgd_surface_count + s) * 2 + 1] >= 3) {
						had_tris = true;
						break;
					}
				}
			}
			if (!is_virtual && p_legacy_surfaces != nullptr && p_legacy_surfaces[s].index_count >= 3) {
				had_tris = true;
			}
			if (had_tris) {
				r_alpha_excluded_count++;
			}
			continue;
		}
		if (!is_virtual && p_legacy_surfaces != nullptr) {
			const NextGIRTLegacySurfaceInput &leg = p_legacy_surfaces[s];
			const uint32_t leg_tris = leg.index_count / 3;
			if (leg.positions != nullptr && leg.vertex_count > 0 && leg.indices != nullptr && leg_tris > 0) {
				LegacySlice slice;
				slice.surface = s;
				slice.first_triangle = 0;
				slice.triangle_count = leg_tris;
				legacy_queue.push_back(slice);
			}
		}
	}

	uint32_t legacy_head = 0;
	struct ProxyRange {
		uint32_t surface = 0;
		uint32_t first_triangle = 0;
		uint32_t triangle_count = 0;
		uint32_t group_rel = 0;
	};

	auto emit_part = [&](uint32_t p_v_off, uint32_t p_v_cnt, uint32_t p_i_off, uint32_t p_i_cnt, LocalVector<ProxyRange> &p_proxy_ranges, const LocalVector<LegacySlice> &p_legacy_slices) -> bool {
		uint32_t cursor = 0;
		uint32_t proxy_pos_rel = 0;
		uint32_t proxy_idx_rel = 0;
		if (!p_proxy_ranges.is_empty()) {
			proxy_pos_rel = 0;
			cursor = _nextgi_align16(p_v_cnt * sizeof(float) * 3u);
			proxy_idx_rel = cursor;
			cursor = _nextgi_align16(proxy_idx_rel + p_i_cnt * sizeof(uint32_t));
			if (has_cards && has_group_table) {
				for (ProxyRange &range : p_proxy_ranges) {
					range.group_rel = cursor;
					cursor = _nextgi_align16(cursor + range.triangle_count * sizeof(uint16_t));
				}
			}
		}

		struct LegacyLayout {
			uint32_t pos_rel = 0;
			uint32_t idx_rel = 0;
		};
		LocalVector<LegacyLayout> leg_layouts;
		leg_layouts.resize(p_legacy_slices.size());
		HashMap<uint32_t, uint32_t> surface_pos_rel;
		for (uint32_t l = 0; l < p_legacy_slices.size(); l++) {
			const LegacySlice &slice = p_legacy_slices[l];
			const NextGIRTLegacySurfaceInput &leg = p_legacy_surfaces[slice.surface];
			uint32_t pos_rel = 0;
			if (const uint32_t *existing = surface_pos_rel.getptr(slice.surface)) {
				pos_rel = *existing;
			} else {
				pos_rel = cursor;
				surface_pos_rel.insert(slice.surface, pos_rel);
				cursor = _nextgi_align16(pos_rel + leg.vertex_count * sizeof(float) * 3u);
			}
			const uint32_t idx_rel = cursor;
			cursor = _nextgi_align16(idx_rel + slice.triangle_count * 3u * sizeof(uint32_t));
			leg_layouts[l].pos_rel = pos_rel;
			leg_layouts[l].idx_rel = idx_rel;
		}

		if (cursor == 0) {
			return true;
		}
		const NextGIArenaAllocator::Allocation alloc = r_allocator.allocate(cursor);
		if (!alloc.is_valid()) {
			return false;
		}

		NextGIRTProxyPartUpload upload;
		upload.allocation = alloc;
		upload.payload.resize_initialized(cursor);
		uint8_t *dst = upload.payload.ptrw();

		uint32_t min_surf = UINT32_MAX;
		uint32_t max_surf = 0;
		if (!p_proxy_ranges.is_empty()) {
			memcpy(dst + proxy_pos_rel, p_desc.rt_proxy_positions.ptr() + uint64_t(p_v_off) * 3u, uint64_t(p_v_cnt) * sizeof(float) * 3u);
			memcpy(dst + proxy_idx_rel, p_desc.rt_proxy_indices.ptr() + uint64_t(p_i_off) * 4u, uint64_t(p_i_cnt) * sizeof(uint32_t));
			const uint32_t part_first_tri_global = p_i_off / 3u;
			for (const ProxyRange &range : p_proxy_ranges) {
				if (has_cards && has_group_table) {
					memcpy(dst + range.group_rel, p_desc.rt_proxy_triangle_groups.ptr() + uint64_t(part_first_tri_global + range.first_triangle) * sizeof(uint16_t), uint64_t(range.triangle_count) * sizeof(uint16_t));
				}
				NextGIRTPartGeometry geom;
				geom.surface = range.surface;
				geom.first_triangle = 0;
				geom.triangle_count = range.triangle_count;
				geom.is_proxy_surface = true;
				geom.has_stage2_cards = has_cards && has_group_table;
				geom.vertex_offset = alloc.offset + proxy_pos_rel;
				geom.vertex_count = p_v_cnt;
				geom.index_offset = alloc.offset + proxy_idx_rel + range.first_triangle * 3u * sizeof(uint32_t);
				geom.group_offset = geom.has_stage2_cards ? (alloc.offset + range.group_rel) : 0u;
				geom.eps = p_desc.rt_proxy_eps;
				upload.geometries.push_back(geom);
				upload.triangle_count += range.triangle_count;
				min_surf = MIN(min_surf, range.surface);
				max_surf = MAX(max_surf, range.surface);
			}
		}

		HashMap<uint32_t, bool> pos_copied;
		for (uint32_t l = 0; l < p_legacy_slices.size(); l++) {
			const LegacySlice &slice = p_legacy_slices[l];
			const NextGIRTLegacySurfaceInput &leg = p_legacy_surfaces[slice.surface];
			if (!pos_copied.has(slice.surface)) {
				memcpy(dst + leg_layouts[l].pos_rel, leg.positions, uint64_t(leg.vertex_count) * sizeof(float) * 3u);
				pos_copied.insert(slice.surface, true);
			}
			memcpy(dst + leg_layouts[l].idx_rel, leg.indices + uint64_t(slice.first_triangle) * 3u, uint64_t(slice.triangle_count) * 3u * sizeof(uint32_t));
			NextGIRTPartGeometry geom;
			geom.surface = slice.surface;
			geom.first_triangle = 0;
			geom.triangle_count = slice.triangle_count;
			geom.is_proxy_surface = true;
			geom.has_stage2_cards = false;
			geom.vertex_offset = alloc.offset + leg_layouts[l].pos_rel;
			geom.vertex_count = leg.vertex_count;
			geom.index_offset = alloc.offset + leg_layouts[l].idx_rel;
			geom.group_offset = 0u;
			geom.eps = leg.eps;
			upload.geometries.push_back(geom);
			upload.triangle_count += slice.triangle_count;
			min_surf = MIN(min_surf, slice.surface);
			max_surf = MAX(max_surf, slice.surface);
		}

		if (min_surf != UINT32_MAX) {
			upload.first_surface = min_surf;
			upload.surface_count = max_surf - min_surf + 1u;
		}
		r_uploads.push_back(upload);
		return true;
	};

	auto rollback = [&]() {
		for (const NextGIRTProxyPartUpload &u : r_uploads) {
			r_allocator.free_range(u.allocation);
		}
		r_uploads.clear();
		return false;
	};

	const uint32_t map_surfaces = MIN(vgd_surface_count, p_mesh_surface_count);
	for (uint32_t p = 0; p < part_count; p++) {
		const int32_t raw_v_off = p_desc.rt_proxy_parts[p * 4 + 0];
		const int32_t raw_v_cnt = p_desc.rt_proxy_parts[p * 4 + 1];
		const int32_t raw_i_off = p_desc.rt_proxy_parts[p * 4 + 2];
		const int32_t raw_i_cnt = p_desc.rt_proxy_parts[p * 4 + 3];
		if (raw_v_off < 0 || raw_v_cnt < 0 || raw_i_off < 0 || raw_i_cnt < 0) {
			return rollback();
		}
		const uint32_t v_off = uint32_t(raw_v_off);
		const uint32_t v_cnt = uint32_t(raw_v_cnt);
		const uint32_t i_off = uint32_t(raw_i_off);
		const uint32_t i_cnt = uint32_t(raw_i_cnt);
		if (uint64_t(v_off) + v_cnt > total_proxy_vertices || uint64_t(i_off) + i_cnt > total_proxy_indices || (i_cnt % 3u) != 0u) {
			return rollback();
		}

		LocalVector<ProxyRange> proxy_ranges;
		uint32_t part_tris = 0;
		for (uint32_t s = 0; s < map_surfaces; s++) {
			if (p_alpha_excluded_surfaces != nullptr && p_alpha_excluded_surfaces[s] != 0) {
				continue;
			}
			const int32_t raw_first = p_desc.rt_proxy_surface_ranges[(p * vgd_surface_count + s) * 2 + 0];
			const int32_t raw_cnt = p_desc.rt_proxy_surface_ranges[(p * vgd_surface_count + s) * 2 + 1];
			if (raw_first < 0 || raw_cnt <= 0) {
				continue;
			}
			const uint32_t first_idx = uint32_t(raw_first);
			const uint32_t idx_cnt = uint32_t(raw_cnt);
			if (uint64_t(first_idx) + idx_cnt > i_cnt || (first_idx % 3u) != 0u || (idx_cnt % 3u) != 0u) {
				return rollback();
			}
			ProxyRange range;
			range.surface = s;
			range.first_triangle = first_idx / 3u;
			range.triangle_count = idx_cnt / 3u;
			proxy_ranges.push_back(range);
			part_tris += range.triangle_count;
		}
		if (part_tris > NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
			return rollback();
		}

		LocalVector<LegacySlice> part_legacy;
		// Outside-mask surfaces append as extra geometries of the first part (#77 step 2),
		// bounded by NEXTGI_RT_MAX_TRIANGLES_PER_BLAS.
		if (r_uploads.is_empty()) {
			while (legacy_head < legacy_queue.size() && part_tris < NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
				LegacySlice &front = legacy_queue[legacy_head];
				// Keep a legacy surface whole if it fits a fresh part alone and would overflow part 0.
				if (part_tris > 0 && front.first_triangle == 0 && front.triangle_count <= NEXTGI_RT_MAX_TRIANGLES_PER_BLAS && part_tris + front.triangle_count > NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
					break;
				}
				const uint32_t take = MIN(front.triangle_count, uint32_t(NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) - part_tris);
				LegacySlice slice;
				slice.surface = front.surface;
				slice.first_triangle = front.first_triangle;
				slice.triangle_count = take;
				part_legacy.push_back(slice);
				part_tris += take;
				front.first_triangle += take;
				front.triangle_count -= take;
				if (front.triangle_count == 0) {
					legacy_head++;
				}
			}
		}

		if (proxy_ranges.is_empty() && part_legacy.is_empty()) {
			continue;
		}
		if (!emit_part(v_off, v_cnt, i_off, i_cnt, proxy_ranges, part_legacy)) {
			return rollback();
		}
	}

	// Emit any remaining outside-mask legacy slices as trailing <= 128k parts.
	while (legacy_head < legacy_queue.size()) {
		LocalVector<ProxyRange> empty_proxy;
		LocalVector<LegacySlice> part_legacy;
		uint32_t part_tris = 0;
		while (legacy_head < legacy_queue.size() && part_tris < NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
			LegacySlice &front = legacy_queue[legacy_head];
			if (part_tris > 0 && front.first_triangle == 0 && front.triangle_count <= NEXTGI_RT_MAX_TRIANGLES_PER_BLAS && part_tris + front.triangle_count > NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
				break;
			}
			const uint32_t take = MIN(front.triangle_count, uint32_t(NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) - part_tris);
			LegacySlice slice;
			slice.surface = front.surface;
			slice.first_triangle = front.first_triangle;
			slice.triangle_count = take;
			part_legacy.push_back(slice);
			part_tris += take;
			front.first_triangle += take;
			front.triangle_count -= take;
			if (front.triangle_count == 0) {
				legacy_head++;
			}
		}
		if (!emit_part(0, 0, 0, 0, empty_proxy, part_legacy)) {
			return rollback();
		}
	}

	return !r_uploads.is_empty();
}

void nextgi_rt_split_parts(const uint32_t *p_triangle_counts, const uint32_t *p_chunks, uint32_t p_surface_count, uint32_t p_max_triangles_per_part, LocalVector<NextGIRTPartPlan> &r_parts) {
	r_parts.clear();
	ERR_FAIL_COND(p_max_triangles_per_part == 0);

	NextGIRTPartPlan current;
	auto flush = [&]() {
		if (!current.geometries.is_empty()) {
			r_parts.push_back(current);
			current = NextGIRTPartPlan();
		}
	};

	for (uint32_t s = 0; s < p_surface_count; s++) {
		uint32_t remaining = p_triangle_counts[s];
		if (remaining == 0) {
			continue;
		}
		uint32_t first_triangle = 0;
		while (remaining > 0) {
			if (!current.geometries.is_empty() && (current.chunk != p_chunks[s] || current.triangle_count >= p_max_triangles_per_part)) {
				flush();
			}
			// Whole surfaces first: a surface that fits a fresh part whole is never split to fill one.
			if (!current.geometries.is_empty() && first_triangle == 0 && remaining <= p_max_triangles_per_part && current.triangle_count + remaining > p_max_triangles_per_part) {
				flush();
			}
			if (current.geometries.is_empty()) {
				current.chunk = p_chunks[s];
				current.first_surface = s;
				current.surface_count = 0;
			}
			const uint32_t take = MIN(remaining, p_max_triangles_per_part - current.triangle_count);
			NextGIRTPartGeometry geometry;
			geometry.surface = s;
			geometry.first_triangle = first_triangle;
			geometry.triangle_count = take;
			current.geometries.push_back(geometry);
			current.triangle_count += take;
			current.surface_count = s - current.first_surface + 1;
			first_triangle += take;
			remaining -= take;
			if (current.triangle_count >= p_max_triangles_per_part) {
				flush();
			}
		}
	}
	flush();
}

float nextgi_rt_angular_radius(float p_distance, float p_bounding_radius) {
	if (p_bounding_radius <= 0.0f) {
		return 0.0f;
	}
	if (p_distance <= 0.0f) {
		return Math::PI * 0.5f;
	}
	return Math::asin(MIN(1.0f, p_bounding_radius / p_distance));
}

void nextgi_rt_note_multimesh_change(NextGIMultiMeshChange &r_state, uint64_t p_frame) {
	if (r_state.last_change_frame == p_frame) {
		return;
	}
	if (p_frame == r_state.last_change_frame + 1 && r_state.change_streak > 0) {
		r_state.change_streak = MIN(uint8_t(r_state.change_streak + 1), uint8_t(255));
	} else {
		r_state.change_streak = 1;
	}
	r_state.last_change_frame = p_frame;
}

Transform3D nextgi_rt_element_transform(const float *p_rows_3x4) {
	Transform3D transform;
	if (p_rows_3x4 == nullptr) {
		return transform;
	}
	transform.basis.rows[0] = Vector3(p_rows_3x4[0], p_rows_3x4[1], p_rows_3x4[2]);
	transform.basis.rows[1] = Vector3(p_rows_3x4[4], p_rows_3x4[5], p_rows_3x4[6]);
	transform.basis.rows[2] = Vector3(p_rows_3x4[8], p_rows_3x4[9], p_rows_3x4[10]);
	transform.origin = Vector3(p_rows_3x4[3], p_rows_3x4[7], p_rows_3x4[11]);
	return transform;
}

Transform3D nextgi_rt_element_world(const Transform3D &p_instance, const float *p_rows_3x4) {
	return p_instance * nextgi_rt_element_transform(p_rows_3x4);
}

int64_t nextgi_rt_coverage_percent(const NextGIRTCoverage &p_coverage) {
	if (p_coverage.denominator == 0) {
		return 0;
	}
	return int64_t(p_coverage.numerator) * 100 / int64_t(p_coverage.denominator);
}

NextGIRTExclusion nextgi_rt_cull_instance(float p_distance, float p_bounding_radius, float p_scene_radius) {
	if (p_distance > p_scene_radius) {
		return NEXTGI_RT_NOT_IN_SCENE;
	}
	if (p_distance > NEXTGI_RT_ANGULAR_CULL_DISTANCE && nextgi_rt_angular_radius(p_distance, p_bounding_radius) < Math::deg_to_rad(0.5f)) {
		return NEXTGI_RT_NOT_IN_SCENE;
	}
	return NEXTGI_RT_INCLUDED;
}

void nextgi_rt_apply_instance_cap(float *p_angular, NextGIRTExclusion *p_reasons, uint32_t p_count, uint32_t p_cap) {
	LocalVector<uint32_t> included;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_reasons[i] == NEXTGI_RT_INCLUDED) {
			included.push_back(i);
		}
	}
	if (included.size() <= p_cap) {
		return;
	}
	struct Greater {
		const float *angular = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return angular[p_a] > angular[p_b]; }
	} greater;
	greater.angular = p_angular;
	std::nth_element(included.ptr(), included.ptr() + p_cap, included.ptr() + included.size(), greater);
	for (uint32_t i = p_cap; i < included.size(); i++) {
		p_reasons[included[i]] = NEXTGI_RT_INSTANCE_CAP;
	}
}

NextGIDirtyRegion nextgi_rt_dirty_region(NextGIDirtyKind p_kind) {
	NextGIDirtyRegion region;
	region.has_old = true;
	region.has_new = p_kind == NEXTGI_DIRTY_MOVED;
	return region;
}

bool nextgi_rt_surface_is_dynamic(uint32_t p_frame, const uint32_t p_update_frames[4]) {
	for (uint32_t i = 0; i < 4; i++) {
		if (p_update_frames[i] != p_frame - i) {
			return false;
		}
	}
	return true;
}

NextGISkinnedBuild nextgi_rt_skinned_build(uint32_t p_instance_id, uint32_t p_frame, uint32_t p_triangles, uint32_t p_triangles_so_far, uint32_t p_triangle_cap) {
	if (p_triangles_so_far + p_triangles > p_triangle_cap) {
		return NEXTGI_SKINNED_SKIP;
	}
	// Rebuild every 32 frames per instance, rotating across instances so the cost spreads.
	return (p_frame + p_instance_id) % 32u == 0u ? NEXTGI_SKINNED_REBUILD : NEXTGI_SKINNED_REFIT;
}

uint32_t nextgi_rt_skinned_triangle_budget(bool p_ada_class) {
	// Ada: 200k skinned triangles per frame; the mid-range class ships at 100k (unvalidated, §23).
	return p_ada_class ? 200000u : 100000u;
}

NextGIRTExclusion nextgi_rt_refit_or_exclude(uint32_t p_refit_triangles_so_far, uint32_t p_triangles, uint32_t p_refit_cap) {
	if (p_refit_triangles_so_far + p_triangles > p_refit_cap) {
		return NEXTGI_RT_DYNAMIC_EXCLUDED;
	}
	return NEXTGI_RT_INCLUDED;
}

uint32_t nextgi_rt_schedule_builds(const NextGIBuildItem *p_items, uint32_t p_count, uint32_t p_triangle_cap, uint32_t *r_accepted, uint32_t p_accepted_capacity, uint32_t &r_triangles) {
	LocalVector<uint32_t> order;
	order.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		order[i] = i;
	}
	struct Less {
		const NextGIBuildItem *items = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const {
			const NextGIBuildItem &a = items[p_a];
			const NextGIBuildItem &b = items[p_b];
			if (a.is_new != b.is_new) {
				return a.is_new;
			}
			return a.angular_radius > b.angular_radius;
		}
	} less;
	less.items = p_items;
	if (p_count > 1) {
		std::sort(order.ptr(), order.ptr() + p_count, less);
	}
	uint32_t accepted = 0;
	r_triangles = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		const NextGIBuildItem &item = p_items[order[i]];
		if (r_triangles + item.triangles > p_triangle_cap) {
			break;
		}
		if (accepted < p_accepted_capacity && r_accepted != nullptr) {
			r_accepted[accepted] = item.id;
		}
		accepted++;
		r_triangles += item.triangles;
	}
	return accepted;
}

bool nextgi_rt_evict_as_memory(const NextGIASMesh *p_meshes, uint32_t p_count, uint64_t p_budget, uint32_t p_absent_frames, uint32_t *r_evict, uint32_t &r_evict_count) {
	r_evict_count = 0;
	uint64_t total = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		total += p_meshes[i].bytes;
	}
	LocalVector<uint32_t> absent;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_meshes[i].frames_absent >= p_absent_frames) {
			absent.push_back(i);
		}
	}
	struct Older {
		const NextGIASMesh *meshes = nullptr;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return meshes[p_a].frames_absent > meshes[p_b].frames_absent; }
	} older;
	older.meshes = p_meshes;
	if (absent.size() > 1) {
		std::sort(absent.ptr(), absent.ptr() + absent.size(), older);
	}
	for (uint32_t index : absent) {
		if (total <= p_budget) {
			break;
		}
		r_evict[r_evict_count++] = p_meshes[index].id;
		total -= p_meshes[index].bytes;
	}
	return total > p_budget;
}

void nextgi_rt_patch_positions(Vector<float> &r_positions, uint32_t p_byte_offset, const uint8_t *p_data, uint32_t p_size) {
	const uint32_t pos_bytes = uint32_t(r_positions.size()) * sizeof(float);
	if (p_data == nullptr || p_size == 0 || p_byte_offset >= pos_bytes) {
		return;
	}
	const uint32_t count = MIN(p_size, pos_bytes - p_byte_offset);
	memcpy(reinterpret_cast<uint8_t *>(r_positions.ptrw()) + p_byte_offset, p_data, count);
}

// GodotNext: nextgi_alpha_policy (#61). Only effective-opaque surfaces enter the TLAS. Alpha-tested
// materials join it only with the `nextgi_rt_opaque` render mode (the BaseMaterial3D override);
// blended surfaces never do.
// GodotNext: ps_swrt_trace (#186, #188). Shared with the SRC's software-tier card rule.
bool nextgi_rt_material_is_alpha(RID p_material) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	if (p_material.is_null() || material_storage == nullptr) {
		return false;
	}
	return material_storage->material_get_nextgi_rt_class(p_material) != MaterialStorage::ShaderData::NEXTGI_RT_OPAQUE;
}

static bool _nextgi_rt_material_is_alpha(RID p_material) {
	return nextgi_rt_material_is_alpha(p_material);
}

struct NextGIRTScene::InstanceEncoder {
	NextgiRtInstancesShaderRD shader;
	RID version;
	RID pipelines[INSTANCE_LAYOUT_MAX] = { RID(), RID() };
};

void NextGIRTScene::init() {
	ERR_FAIL_COND(tlas.is_valid());
	ERR_FAIL_NULL(RD::get_singleton());
	ERR_FAIL_COND(!RD::get_singleton()->has_feature(RD::SUPPORTS_RAY_QUERY));
	tlas = RD::get_singleton()->tlas_create(NEXTGI_RT_TLAS_CAPACITY, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);

	// GodotNext: tlas_from_buffer
	instance_encoder = memnew(InstanceEncoder);
	Vector<String> defines;
	defines.push_back("\n#define INSTANCE_64\n");
	defines.push_back("\n#define INSTANCE_72\n");
	instance_encoder->shader.initialize(defines);
	instance_encoder->version = instance_encoder->shader.version_create();
	for (int i = 0; i < INSTANCE_LAYOUT_MAX; i++) {
		instance_encoder->pipelines[i] = RD::get_singleton()->compute_pipeline_create(instance_encoder->shader.version_get_shader(instance_encoder->version, i));
	}
}

void NextGIRTScene::free_far_field() {
	if (far_field != nullptr) {
		far_field->free();
		memdelete(far_field);
		far_field = nullptr;
	}
}

void NextGIRTScene::free() {
	free_far_field();
	total_record_count = 0;
	if (RD::get_singleton() == nullptr) {
		return;
	}
	if (instance_encoder != nullptr) {
		for (int i = 0; i < INSTANCE_LAYOUT_MAX; i++) {
			if (instance_encoder->pipelines[i].is_valid()) {
				RD::get_singleton()->free_rid(instance_encoder->pipelines[i]);
			}
		}
		if (instance_encoder->version.is_valid()) {
			instance_encoder->shader.version_free(instance_encoder->version);
		}
		memdelete(instance_encoder);
		instance_encoder = nullptr;
	}
	if (gpu_instance_buffer.is_valid()) {
		RD::get_singleton()->free_rid(gpu_instance_buffer);
		gpu_instance_buffer = RID();
	}
	if (gpu_count_dummy.is_valid()) {
		RD::get_singleton()->free_rid(gpu_count_dummy);
		gpu_count_dummy = RID();
	}
	gpu_instance_capacity = 0;
	// GodotNext: nextgi_skinned_actors (#59).
	for (const KeyValue<uint64_t, SkinnedPart> &entry : skinned_parts) {
		if (entry.value.blas.is_valid()) {
			RD::get_singleton()->free_rid(entry.value.blas);
		}
	}
	skinned_parts.clear();
	if (sdf_stream != nullptr) {
		sdf_stream->free_resources();
		memdelete(sdf_stream);
		sdf_stream = nullptr;
	}
	sdf_instances.clear();
	global_sdf.free_all();
	global_sdf_clipmaps.reset();
	if (tlas.is_valid()) {
		RD::get_singleton()->free_rid(tlas);
		tlas = RID();
	}
	if (instance_record_buffer.is_valid()) {
		RD::get_singleton()->free_rid(instance_record_buffer);
		instance_record_buffer = RID();
	}
	if (surface_record_buffer.is_valid()) {
		RD::get_singleton()->free_rid(surface_record_buffer);
		surface_record_buffer = RID();
	}
	instance_record_capacity = 0;
	surface_record_capacity = 0;
}

void NextGIRTScene::_ensure_record_buffers(uint32_t p_instance_count, uint32_t p_surface_count) {
	if (p_instance_count > instance_record_capacity) {
		if (instance_record_buffer.is_valid()) {
			RD::get_singleton()->free_rid(instance_record_buffer);
		}
		instance_record_capacity = MAX(p_instance_count, instance_record_capacity * 2);
		instance_record_buffer = RD::get_singleton()->storage_buffer_create(instance_record_capacity * sizeof(NextGIInstanceRecord));
	}
	if (p_surface_count > surface_record_capacity) {
		if (surface_record_buffer.is_valid()) {
			RD::get_singleton()->free_rid(surface_record_buffer);
		}
		surface_record_capacity = MAX(p_surface_count, surface_record_capacity * 2);
		surface_record_buffer = RD::get_singleton()->storage_buffer_create(surface_record_capacity * sizeof(NextGISurfaceRecord));
	}
}

void NextGIRTScene::_build_dirty_mesh_parts(RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return;
	}
	const bool dynamic = mesh_storage->mesh_nextgi_rt_is_dynamic(p_mesh);
	const LocalVector<MeshStorage::NextGIRTPart> *old_parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (old_parts != nullptr) {
		for (const MeshStorage::NextGIRTPart &old_part : *old_parts) {
			for (uint32_t i = 0; i < pending_compactions.size();) {
				if (pending_compactions[i].blas == old_part.blas) {
					pending_compactions.remove_at(i);
				} else {
					i++;
				}
			}
		}
	}
	mesh_storage->mesh_nextgi_rt_clear_parts(p_mesh);

	const uint32_t surface_count = uint32_t(mesh_storage->mesh_get_surface_count(p_mesh));
	LocalVector<uint32_t> triangle_counts;
	LocalVector<uint32_t> chunks;
	LocalVector<MeshStorage::NextGIRTSurfaceInfo> surface_infos;
	triangle_counts.resize(surface_count);
	chunks.resize(surface_count);
	surface_infos.resize(surface_count);
	for (uint32_t i = 0; i < surface_count; i++) {
		triangle_counts[i] = 0;
		chunks[i] = UINT32_MAX;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, i, surface_infos[i]) || !surface_infos[i].eligible) {
			continue;
		}
		if (_nextgi_rt_material_is_alpha(surface_infos[i].material)) {
			stats.coverage.alpha_excluded++;
			continue;
		}
		triangle_counts[i] = surface_infos[i].index_count / 3;
		chunks[i] = surface_infos[i].chunk;
	}

	LocalVector<NextGIRTPartPlan> plans;
	nextgi_rt_split_parts(triangle_counts.ptr(), chunks.ptr(), surface_count, NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, plans);

	LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	ERR_FAIL_NULL(parts);
	for (const NextGIRTPartPlan &plan : plans) {
		LocalVector<RD::AccelerationStructureGeometry> geometries;
		const RID chunk_buffer = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(plan.chunk);
		ERR_CONTINUE(chunk_buffer.is_null());
		for (const NextGIRTPartGeometry &part_geometry : plan.geometries) {
			const MeshStorage::NextGIRTSurfaceInfo &info = surface_infos[part_geometry.surface];
			RD::AccelerationStructureGeometry geometry;
			geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
			geometry.vertex_buffer = chunk_buffer;
			geometry.vertex_offset = info.vertex_offset;
			geometry.vertex_stride = sizeof(float) * 3;
			geometry.vertex_count = info.vertex_count;
			geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
			geometry.index_buffer = chunk_buffer;
			geometry.index_offset = info.index_offset + part_geometry.first_triangle * 3 * sizeof(uint32_t);
			geometry.index_count = part_geometry.triangle_count * 3;
			geometry.index_format = RD::INDEX_BUFFER_FORMAT_UINT32;
			geometries.push_back(geometry);
		}

		MeshStorage::NextGIRTPart part;
		part.first_surface = plan.first_surface;
		part.surface_count = plan.surface_count;
		part.geometries = plan.geometries;
		part.triangle_count = plan.triangle_count;
		// Static BLASes compact one frame later. A dynamic mesh is rebuilt once with ALLOW_UPDATE, then refit.
		BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
		if (dynamic) {
			flags = flags | RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		} else {
			flags = flags | RD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT;
		}
		part.blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), flags);
		if (part.blas.is_valid()) {
			RD::get_singleton()->blas_build(part.blas);
			part.uncompacted_bytes = RD::get_singleton()->acceleration_structure_get_size_bytes(part.blas);
			stats.blas_build_count++;
			stats.blas_build_triangles += plan.triangle_count;
			if (!dynamic) {
				PendingCompact pending;
				pending.blas = part.blas;
				pending.mesh = p_mesh;
				pending.part_index = uint32_t(parts->size());
				pending.triangles = plan.triangle_count;
				pending_compactions.push_back(pending);
			}
		}
		parts->push_back(part);
	}
	mesh_storage->mesh_nextgi_rt_set_allows_update(p_mesh, dynamic);
	mesh_storage->mesh_nextgi_rt_set_blas_dirty(p_mesh, false);
}

void NextGIRTScene::_build_proxy_part(RID p_mesh, uint32_t p_part_index) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh) || RD::get_singleton() == nullptr) {
		return;
	}
	LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr || p_part_index >= parts->size()) {
		return;
	}
	MeshStorage::NextGIRTPart &part = (*parts)[p_part_index];
	if (!part.is_proxy || part.blas.is_valid() || part.geometries.is_empty() || part.chunk == UINT32_MAX) {
		return;
	}
	const RID chunk_buffer = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(part.chunk);
	ERR_FAIL_COND(chunk_buffer.is_null());

	LocalVector<RD::AccelerationStructureGeometry> geometries;
	geometries.reserve(part.geometries.size());
	for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
		RD::AccelerationStructureGeometry geometry;
		geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
		geometry.vertex_buffer = chunk_buffer;
		geometry.vertex_offset = part_geometry.vertex_offset;
		geometry.vertex_stride = sizeof(float) * 3;
		geometry.vertex_count = part_geometry.vertex_count;
		geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		geometry.index_buffer = chunk_buffer;
		geometry.index_offset = part_geometry.index_offset;
		geometry.index_count = part_geometry.triangle_count * 3;
		geometry.index_format = RD::INDEX_BUFFER_FORMAT_UINT32;
		geometries.push_back(geometry);
	}

	const BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT | RD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT;
	part.blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), flags);
	if (part.blas.is_valid()) {
		RD::get_singleton()->blas_build(part.blas);
		part.uncompacted_bytes = RD::get_singleton()->acceleration_structure_get_size_bytes(part.blas);
		part.compacted = false;
		stats.blas_build_count++;
		stats.blas_build_triangles += part.triangle_count;
		PendingCompact pending;
		pending.blas = part.blas;
		pending.mesh = p_mesh;
		pending.part_index = p_part_index;
		pending.triangles = part.triangle_count;
		pending_compactions.push_back(pending);
	}

	bool all_built = true;
	for (const MeshStorage::NextGIRTPart &check : *parts) {
		if (!check.geometries.is_empty() && check.blas.is_null()) {
			all_built = false;
			break;
		}
	}
	if (all_built) {
		mesh_storage->mesh_nextgi_rt_set_allows_update(p_mesh, false);
		mesh_storage->mesh_nextgi_rt_set_blas_dirty(p_mesh, false);
	}
}

RID NextGIRTScene::build_far_proxy_mesh(RID p_mesh) {
	_build_dirty_mesh_parts(p_mesh);
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL_V(mesh_storage, RID());
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr || parts->size() != 1 || (*parts)[0].geometries.is_empty()) {
		return RID();
	}
	return (*parts)[0].blas;
}

bool NextGIRTScene::append_far_proxy(RID p_mesh, RenderGeometryInstance *p_instance, const Transform3D &p_transform, LocalVector<NextGIInstanceRecord> &r_records, LocalVector<NextGISurfaceRecord> &r_surfaces, RD::AccelerationStructureInstance &r_tlas_instance) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL_V(mesh_storage, false);
	ERR_FAIL_NULL_V(p_instance, false);
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr || parts->size() != 1 || r_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
		return false;
	}
	const MeshStorage::NextGIRTPart &part = (*parts)[0];
	if (part.geometries.is_empty() || part.blas.is_null()) {
		return false;
	}
	NextGIInstanceRecord record;
	for (int i = 0; i < 3; i++) {
		record.object_to_world[i * 4 + 0] = p_transform.basis.rows[i][0];
		record.object_to_world[i * 4 + 1] = p_transform.basis.rows[i][1];
		record.object_to_world[i * 4 + 2] = p_transform.basis.rows[i][2];
	}
	record.object_to_world[3] = p_transform.origin.x;
	record.object_to_world[7] = p_transform.origin.y;
	record.object_to_world[11] = p_transform.origin.z;
	record.surface_base = r_surfaces.size();
	record.surface_count = part.geometries.size();
	record.rt_flags = NEXTGI_RT_FLAG_FAR;
	record.card_set = NextGISRC::get_instance_card_set(p_instance);
	if (record.card_set == UINT32_MAX) {
		return false;
	}
	record.tlas_index = r_records.size();
	float eps_rt = 0.0f;
	for (const NextGIRTPartGeometry &geometry : part.geometries) {
		NextGISurfaceRecord surface;
		if (geometry.is_proxy_surface) {
			record.arena_chunk = part.chunk;
			eps_rt = MAX(eps_rt, geometry.eps);
			surface.vertex_offset = geometry.vertex_offset;
			surface.index_offset = geometry.index_offset;
			surface.triangle_count = geometry.triangle_count;
			surface.flags = geometry.has_stage2_cards ? (NEXTGI_RT_SURFACE_FLAG_STAGE2_CARDS | geometry.group_offset) : 0u;
		} else {
			MeshStorage::NextGIRTSurfaceInfo info;
			if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, geometry.surface, info)) {
				return false;
			}
			record.arena_chunk = info.chunk;
			eps_rt = MAX(eps_rt, info.eps);
			surface.vertex_offset = info.vertex_offset;
			surface.index_offset = info.index_offset + geometry.first_triangle * 3 * sizeof(uint32_t);
			surface.triangle_count = geometry.triangle_count;
		}
		r_surfaces.push_back(surface);
	}
	const Vector3 scale = p_transform.basis.get_scale_abs();
	record.origin_offset = nextgi_rt_origin_offset(eps_rt, MAX(scale.x, MAX(scale.y, scale.z)));
	if (NextGIRelight::get_singleton() != nullptr) {
		record.page_table = NextGIRelight::get_singleton()->assign_pages(p_instance, record);
	}
	r_records.push_back(record);
	r_tlas_instance.transform = p_transform;
	r_tlas_instance.id = record.tlas_index;
	r_tlas_instance.mask = 0xFF;
	r_tlas_instance.hit_sbt_range = 0;
	r_tlas_instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
	r_tlas_instance.blas = part.blas;
	return true;
}

void NextGIRTScene::_update_far_records(const ::RenderDataRD *p_render_data, LocalVector<NextGIInstanceRecord> &r_records, LocalVector<NextGISurfaceRecord> &r_surfaces) {
	if (!bool(GLOBAL_GET("rendering/global_illumination/nextgi/far_field"))) {
		if (far_field != nullptr) {
			far_field->free();
			memdelete(far_field);
			far_field = nullptr;
		}
		total_record_count = r_records.size();
		return;
	}
	if (far_field == nullptr) {
		far_field = memnew(NextGIFarField);
		far_field->init(this);
	}
	far_field->update(p_render_data, r_records, r_surfaces);
	total_record_count = r_records.size();
}

// GodotNext: nextgi_as_slots
// Both AS phases gate on the same v0 feed; either phase is a no-op without it.
static const LocalVector<RendererSceneRender::NextGIRTInstance> *_nextgi_rt_fed_instances(const ::RenderDataRD *p_render_data) {
	const RendererSceneRender::RenderSDFGIUpdateData *update_data = p_render_data->sdfgi_update_data;
	if (update_data == nullptr) {
		return nullptr;
	}
	return update_data->nextgi_rt_instances;
}

void NextGIRTScene::rebuild_tlas(const ::RenderDataRD *p_render_data) {
	// This phase runs first in the frame block, so it also clears the per-frame stats;
	// build_dirty_blas() then fills blas_build_count for the same frame.
	stats.fed_instance_count = 0;
	stats.instance_count = 0;
	stats.blas_build_count = 0;
	stats.blas_build_triangles = 0;
	stats.compaction_copies = 0;
	stats.refit_triangles = 0;
	// GodotNext: nextgi_skinned_actors (#59). Per-frame skinned counters.
	stats.skinned_instances = 0;
	stats.skinned_refit_triangles = 0;
	stats.skinned_build_triangles = 0;
	tlas_traceable = false;
	compaction_triangles_this_frame = 0;
	// GodotNext: ps_lumen_lite (#183). New instance records: relight pages claim them again.
	if (NextGIRelight::get_singleton() != nullptr) {
		NextGIRelight::get_singleton()->begin_record_pass();
	}
	ERR_FAIL_NULL(p_render_data);
	if (tlas.is_null()) {
		return;
	}
	const LocalVector<RendererSceneRender::NextGIRTInstance> *fed_instances = _nextgi_rt_fed_instances(p_render_data);
	if (fed_instances == nullptr || fed_instances->is_empty()) {
		LocalVector<NextGIInstanceRecord> records;
		LocalVector<NextGISurfaceRecord> surfaces;
		_update_far_records(p_render_data, records, surfaces);
		// GodotNext: ps_lumen_lite (#183)
		if (NextGIRelight::get_singleton() != nullptr) {
			NextGIRelight::get_singleton()->finish_record_pass(records);
		}
		_ensure_record_buffers(records.size(), surfaces.size());
		if (instance_record_buffer.is_valid() && !records.is_empty()) {
			RD::get_singleton()->buffer_update(instance_record_buffer, 0, records.size() * sizeof(NextGIInstanceRecord), records.ptr());
		}
		if (surface_record_buffer.is_valid() && !surfaces.is_empty()) {
			RD::get_singleton()->buffer_update(surface_record_buffer, 0, surfaces.size() * sizeof(NextGISurfaceRecord), surfaces.ptr());
		}
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	if (!mesh_storage->mesh_nextgi_rt_is_enabled()) {
		if (far_field != nullptr) {
			far_field->free();
			memdelete(far_field);
			far_field = nullptr;
		}
		total_record_count = 0;
		return;
	}
	stats.fed_instance_count = fed_instances->size();
	stats.coverage.alpha_excluded = 0;
	const RendererSceneRender::RenderSDFGIUpdateData *update_data = p_render_data->sdfgi_update_data;
	stats.coverage.not_in_scene = update_data != nullptr ? update_data->nextgi_rt_not_in_scene : 0;
	stats.coverage.instance_cap = update_data != nullptr ? update_data->nextgi_rt_instance_cap : 0;
	stats.coverage.dynamic_excluded = update_data != nullptr ? update_data->nextgi_rt_dynamic_excluded : 0;
	stats.coverage.denominator = update_data != nullptr ? update_data->nextgi_rt_denominator : 0;
	stats.coverage.numerator = 0;
	stats.coverage.blas_pending = 0;
	// GodotNext: ps_mesh_sdf_atlas (#185). One SDF instance per fed GI instance.
	sdf_instances.begin_frame();
	const uint64_t ticks_start = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;

	LocalVector<RD::AccelerationStructureInstance> tlas_instances;
	LocalVector<NextGIInstanceRecord> instance_records;
	LocalVector<NextGISurfaceRecord> surface_records;
	const Vector3 camera_origin = p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;

	for (const RendererSceneRender::NextGIRTInstance &fed : *fed_instances) {
		if (fed.gpu_source.is_valid()) {
			// A dynamic MultiMesh is one GPU source, encoded after this CPU pass (#57).
			continue;
		}
		RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
		if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
			continue;
		}
		geometry_instance->nextgi_record_index = UINT32_MAX;
		const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry_instance->data->base;
		if (!mesh_storage->owns_mesh(mesh)) {
			stats.coverage.blas_pending++;
			continue;
		}
		{
			const Transform3D sdf_transform = fed.has_rt_transform ? fed.rt_transform : geometry_instance->transform;
			const Vector3 sdf_scale = sdf_transform.basis.get_scale();
			const bool sdf_skinned = geometry_instance->mesh_instance.is_valid();
			// GodotNext: ps_lumen_lite (#183). Skinned meshes stay receivers. Both tracers skip them.
			if (sdf_skinned && !NextGI::lite_traces_skinned(NextGI::is_lumen_lite())) {
				stats.coverage.dynamic_excluded++;
				continue;
			}
			const bool sdf_instance_uniforms = geometry_instance->data->material_override.is_valid() || geometry_instance->data->material_overlay.is_valid();
			MeshStorage *sdf_mesh_storage = MeshStorage::get_singleton();
			if (sdf_mesh_storage != nullptr) {
				sdf_instances.add(sdf_mesh_storage->get_sdf_atlas(), mesh, sdf_transform, sdf_skinned, sdf_instance_uniforms, MAX(sdf_scale.x, MAX(sdf_scale.y, sdf_scale.z)), true, sdf_mesh_storage->mesh_nextgi_rt_is_dynamic(mesh));
			}
		}
		// GodotNext: nextgi_skinned_actors (#59). A skinned instance enters through its posed
		// slots; its bind-pose parts never enter the TLAS. Each slot holds the posed vertices
		// followed by the coarsest-LOD index copy, so one chunk serves the BLAS and the hit fetch.
		const RID skinned_instance = geometry_instance->mesh_instance;
		if (skinned_instance.is_valid()) {
			LocalVector<uint32_t> slot_surfaces;
			LocalVector<MeshStorage::NextGIRTSlotInfo> slots;
			uint32_t skinned_triangles = 0;
			const uint32_t skinned_surface_count = mesh_storage->mesh_get_surface_count(mesh);
			for (uint32_t surface_index = 0; surface_index < skinned_surface_count; surface_index++) {
				MeshStorage::NextGIRTSlotInfo slot;
				if (!mesh_storage->mesh_instance_get_rt_slot(skinned_instance, surface_index, slot)) {
					continue;
				}
				slot_surfaces.push_back(surface_index);
				slots.push_back(slot);
				skinned_triangles += slot.index_count / 3;
			}
			if (slots.is_empty()) {
				stats.coverage.dynamic_excluded++;
				continue;
			}
			const Transform3D instance_transform = fed.has_rt_transform ? fed.rt_transform : geometry_instance->transform;
			const Vector3 scale = instance_transform.basis.get_scale();
			const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
			// Distance drop-out beyond 30 m keeps crowds inside the per-frame refit budget (D17).
			const bool within_range = camera_origin.distance_squared_to(instance_transform.origin) <= NEXTGI_RT_ANGULAR_CULL_DISTANCE * NEXTGI_RT_ANGULAR_CULL_DISTANCE;
			const uint32_t skinned_budget = nextgi_rt_skinned_triangle_budget(true);
			const NextGISkinnedBuild build = within_range ? nextgi_rt_skinned_build(uint32_t(skinned_instance.get_id()), uint32_t(frame), skinned_triangles, stats.skinned_refit_triangles + stats.skinned_build_triangles, skinned_budget) : NEXTGI_SKINNED_SKIP;
			const uint32_t skinned_card_set = NextGISRC::get_instance_card_set(geometry_instance) != UINT32_MAX ? NextGISRC::get_instance_card_set(geometry_instance) : NextGISRC::get_mesh_card_set(mesh);
			const uint32_t first_record = instance_records.size();
			bool skinned_entered = false;
			if (build != NEXTGI_SKINNED_SKIP) {
				for (uint32_t k = 0; k < slots.size(); k++) {
					MeshStorage::NextGIRTSurfaceInfo info;
					if (!mesh_storage->mesh_nextgi_rt_get_surface_info(mesh, slot_surfaces[k], info)) {
						continue;
					}
					const MeshStorage::NextGIRTSlotInfo &slot = slots[k];
					const uint64_t key = (uint64_t(skinned_instance.get_id()) << 32) | slot_surfaces[k];
					SkinnedPart *part = skinned_parts.getptr(key);
					if (part == nullptr) {
						const RID chunk_buffer = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(slot.chunk);
						if (chunk_buffer.is_null()) {
							continue;
						}
						RD::AccelerationStructureGeometry geometry;
						geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
						geometry.vertex_buffer = chunk_buffer;
						geometry.vertex_offset = slot.vertex_offset;
						geometry.vertex_stride = sizeof(float) * 3;
						geometry.vertex_count = slot.vertex_count;
						geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
						geometry.index_buffer = chunk_buffer;
						geometry.index_offset = slot.index_offset;
						geometry.index_count = slot.index_count;
						geometry.index_format = RD::INDEX_BUFFER_FORMAT_UINT32;
						BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT | RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
						const RID blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(&geometry, 1), flags);
						if (!blas.is_valid()) {
							continue;
						}
						RD::get_singleton()->blas_build(blas);
						stats.skinned_build_triangles += slot.index_count / 3;
						SkinnedPart created;
						created.blas = blas;
						created.triangles = slot.index_count / 3;
						created.last_frame = frame;
						skinned_parts.insert(key, created);
						part = skinned_parts.getptr(key);
					} else if (build == NEXTGI_SKINNED_REBUILD) {
						RD::get_singleton()->blas_build(part->blas);
						stats.skinned_build_triangles += part->triangles;
						part->last_frame = frame;
					} else {
						RD::get_singleton()->blas_build(part->blas, RD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE);
						stats.skinned_refit_triangles += part->triangles;
						part->last_frame = frame;
					}
					if (part == nullptr || !part->blas.is_valid()) {
						continue;
					}
					NextGIInstanceRecord record;
					for (int row = 0; row < 3; row++) {
						record.object_to_world[row * 4 + 0] = instance_transform.basis.rows[row][0];
						record.object_to_world[row * 4 + 1] = instance_transform.basis.rows[row][1];
						record.object_to_world[row * 4 + 2] = instance_transform.basis.rows[row][2];
					}
					record.object_to_world[3] = instance_transform.origin.x;
					record.object_to_world[7] = instance_transform.origin.y;
					record.object_to_world[11] = instance_transform.origin.z;
					record.surface_base = surface_records.size();
					record.surface_count = 1;
					// Bind-pose cards shade the posed hit; without a card set the hit shades from the WRC.
					record.card_set = skinned_card_set;
					record.page_table = 0x80000000u;
					// GodotNext: ps_lumen_lite (#183). The pages remember this record's index, so it is set first.
					record.tlas_index = instance_records.size();
					if (skinned_card_set != UINT32_MAX && NextGIRelight::get_singleton() != nullptr) {
						record.page_table = NextGIRelight::get_singleton()->assign_pages(geometry_instance, record, fed.element_index);
					}
					record.rt_flags = NEXTGI_RT_FLAG_SKINNED;
					record.origin_offset = nextgi_rt_origin_offset(info.eps, max_scale);
					record.arena_chunk = slot.chunk;
					NextGISurfaceRecord surface_record;
					surface_record.vertex_offset = slot.vertex_offset;
					surface_record.index_offset = slot.index_offset;
					surface_record.triangle_count = slot.index_count / 3;
					// D6 samples the bind-pose cards at the posed hit's triangle and barycentrics.
					surface_record.flags = NEXTGI_RT_SURFACE_FLAG_SKINNED | ((slot.bind_vertex_offset / NEXTGI_RT_ALIGNMENT) << 8);
					surface_records.push_back(surface_record);
					instance_records.push_back(record);
					RD::AccelerationStructureInstance tlas_instance;
					tlas_instance.transform = instance_transform;
					tlas_instance.id = record.tlas_index;
					tlas_instance.mask = 0xFF;
					tlas_instance.hit_sbt_range = 0;
					tlas_instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					tlas_instance.blas = part->blas;
					tlas_instances.push_back(tlas_instance);
					skinned_entered = true;
				}
			}
			if (skinned_entered) {
				stats.skinned_instances++;
				stats.coverage.numerator++;
				if (geometry_instance->nextgi_record_index == UINT32_MAX && first_record < instance_records.size()) {
					geometry_instance->nextgi_record_index = first_record;
				}
			} else {
				stats.coverage.dynamic_excluded++;
			}
			continue;
		}

		// A dirty mesh is built in this frame's later "AS builds" phase and enters the
		// TLAS next frame; its parts are skipped here (nextgi_rt_part_enters_tlas).
		bool blas_dirty = mesh_storage->mesh_nextgi_rt_is_blas_dirty(mesh);
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
		if (parts == nullptr) {
			stats.coverage.blas_pending++;
			continue;
		}
		// A surface updated on each of the last 4 frames refits in this phase, up to the refit cap.
		if (blas_dirty && mesh_storage->mesh_nextgi_rt_is_dynamic(mesh) && mesh_storage->mesh_nextgi_rt_allows_update(mesh)) {
			uint32_t triangles = 0;
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				triangles += part.triangle_count;
			}
			if (nextgi_rt_refit_or_exclude(stats.refit_triangles, triangles, NEXTGI_RT_REFIT_TRIANGLE_CAP) != NEXTGI_RT_INCLUDED) {
				continue;
			}
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					RD::get_singleton()->blas_build(part.blas, RD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE);
				}
			}
			mesh_storage->mesh_nextgi_rt_set_blas_dirty(mesh, false);
			stats.refit_triangles += triangles;
			blas_dirty = false;
		}

		const Transform3D transform = fed.has_rt_transform ? fed.rt_transform : geometry_instance->transform;
		bool entered_tlas = false;
		uint32_t first_rt_fields[6] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX };
		const bool has_vgd_cards = RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(mesh) != nullptr && !RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(mesh)->card_descriptors.is_empty();
		const Vector3 scale = transform.basis.get_scale_abs();
		const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
		for (const MeshStorage::NextGIRTPart &part : *parts) {
			if (!nextgi_rt_part_enters_tlas(blas_dirty, part.blas.is_valid()) || part.geometries.is_empty()) {
				continue;
			}
			if (instance_records.size() >= NEXTGI_RT_TLAS_CAPACITY || instance_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
				break;
			}
			const uint32_t record_index = instance_records.size();

			NextGIInstanceRecord record;
			for (int i = 0; i < 3; i++) {
				record.object_to_world[i * 4 + 0] = transform.basis.rows[i][0];
				record.object_to_world[i * 4 + 1] = transform.basis.rows[i][1];
				record.object_to_world[i * 4 + 2] = transform.basis.rows[i][2];
			}
			record.object_to_world[3] = transform.origin.x;
			record.object_to_world[7] = transform.origin.y;
			record.object_to_world[11] = transform.origin.z;
			record.surface_base = surface_records.size();
			record.surface_count = part.geometries.size();
			record.rt_flags = NEXTGI_RT_FLAG_STATIC | (fed.lightmapped ? uint32_t(NEXTGI_RT_FLAG_LIGHTMAPPED) : 0);
			record.tlas_index = record_index;
			// GodotNext: nextgi_src_cards
			// Every part record of the mesh shares its SRC card set (§6.3); UINT32_MAX until built.
			record.card_set = NextGISRC::get_instance_card_set(geometry_instance);
			if (record.card_set == UINT32_MAX) {
				record.card_set = NextGISRC::get_mesh_card_set(mesh);
			}

			float eps_rt = 0.0f;
			uint32_t first_vertex_offset = 0;
			bool have_first_vertex_offset = false;
			for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
				NextGISurfaceRecord surface_record;
				if (part_geometry.is_proxy_surface) {
					record.arena_chunk = part.chunk;
					eps_rt = MAX(eps_rt, part_geometry.eps);
					surface_record.vertex_offset = part_geometry.vertex_offset;
					surface_record.index_offset = part_geometry.index_offset;
					surface_record.triangle_count = part_geometry.triangle_count;
					if (part_geometry.has_stage2_cards) {
						surface_record.flags = NEXTGI_RT_SURFACE_FLAG_STAGE2_CARDS | part_geometry.group_offset;
					} else if (has_vgd_cards) {
						surface_record.flags = NEXTGI_RT_SURFACE_FLAG_NO_CARDS;
					} else {
						surface_record.flags = 0;
					}
				} else {
					MeshStorage::NextGIRTSurfaceInfo info;
					if (!mesh_storage->mesh_nextgi_rt_get_surface_info(mesh, part_geometry.surface, info)) {
						continue;
					}
					record.arena_chunk = info.chunk;
					eps_rt = MAX(eps_rt, info.eps);
					surface_record.vertex_offset = info.vertex_offset;
					surface_record.index_offset = info.index_offset + part_geometry.first_triangle * 3 * sizeof(uint32_t);
					surface_record.triangle_count = part_geometry.triangle_count;
					surface_record.flags = 0;
				}
				if (!have_first_vertex_offset) {
					first_vertex_offset = surface_record.vertex_offset;
					have_first_vertex_offset = true;
				}
				surface_records.push_back(surface_record);
			}
			record.origin_offset = nextgi_rt_origin_offset(eps_rt, max_scale);
			if (NextGIRelight::get_singleton() != nullptr) {
				record.page_table = NextGIRelight::get_singleton()->assign_pages(geometry_instance, record, fed.element_index);
			}
			instance_records.push_back(record);
			if (RSG::rasterizer != nullptr) {
				mesh_last_tlas_frame[mesh] = RSG::rasterizer->get_frame_number();
			}

			RD::AccelerationStructureInstance tlas_instance;
			tlas_instance.transform = transform;
			tlas_instance.id = record_index;
			tlas_instance.mask = 0xFF;
			tlas_instance.hit_sbt_range = 0;
			tlas_instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
			tlas_instance.blas = part.blas;
			tlas_instances.push_back(tlas_instance);

			if (geometry_instance->nextgi_record_index == UINT32_MAX) {
				geometry_instance->nextgi_record_index = record_index;
			}
			if (!entered_tlas) {
				first_rt_fields[0] = record_index;
				first_rt_fields[1] = uint32_t(part.blas.get_local_index());
				first_rt_fields[2] = record.arena_chunk;
				first_rt_fields[3] = first_vertex_offset;
				first_rt_fields[4] = record.page_table;
				first_rt_fields[5] = record.card_set;
			}
			entered_tlas = true;
		}
		if (geometry_instance->nextgeo_record_index != UINT32_MAX && RSG::virtual_mesh_storage != nullptr && fed.element_index == 0) {
			RSG::virtual_mesh_storage->instance_record_set_rt(geometry_instance->nextgeo_record_index, first_rt_fields);
			RSG::virtual_mesh_storage->instance_record_set_rt_flags(geometry_instance->nextgeo_record_index, entered_tlas ? RendererVirtualMeshStorage::RECORD_RT_STATIC : 0u);
		}
		if (entered_tlas) {
			stats.coverage.numerator++;
		} else if (!blas_dirty && parts->is_empty() && mesh_storage->mesh_nextgi_get_proxy_alpha_excluded(mesh) > 0) {
			stats.coverage.alpha_excluded += mesh_storage->mesh_nextgi_get_proxy_alpha_excluded(mesh);
		} else {
			stats.coverage.blas_pending++;
		}
	}

	// GodotNext: tlas_from_buffer
	// Dynamic MultiMesh sources share the TLAS with the CPU-fed instances. Farthest sources are
	// dropped first when they do not fit the 100k cap.
	struct GpuSource {
		RID source;
		RID buffer;
		RID blas;
		RID count_buffer;
		uint32_t count = 0;
		uint32_t stride = 0;
		uint32_t element_offset = 0;
		float distance = 0.0f;
	};
	const Vector3 camera_pos = p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	uint32_t dynamic_budget = nextgi_rt_dynamic_budget(NEXTGI_RT_TLAS_CAPACITY, tlas_instances.size());
	LocalVector<GpuSource> gpu_candidates;
	for (const RendererSceneRender::NextGIRTInstance &fed : *fed_instances) {
		if (!fed.gpu_source.is_valid() || fed.gpu_count == 0 || fed.gpu_stride < 12) {
			continue;
		}
		const RID mesh = fed.mesh_override;
		if (mesh.is_null() || !mesh_storage->owns_mesh(mesh)) {
			stats.coverage.blas_pending += fed.gpu_count;
			continue;
		}
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
		if (parts == nullptr) {
			stats.coverage.blas_pending += fed.gpu_count;
			continue;
		}
		RID blas;
		for (const MeshStorage::NextGIRTPart &part : *parts) {
			if (part.blas.is_valid()) {
				blas = part.blas;
				break;
			}
		}
		if (blas.is_null()) {
			stats.coverage.blas_pending += fed.gpu_count;
			continue;
		}

		GpuSource source;
		source.source = fed.gpu_source;
		source.blas = blas;
		source.count = fed.gpu_count;
		source.stride = fed.gpu_stride;
		if (fed.gpu_particles) {
			ParticlesStorage *particles = ParticlesStorage::get_singleton();
			if (particles == nullptr) {
				continue;
			}
			source.buffer = particles->particles_get_instance_buffer(fed.gpu_source);
			source.element_offset = 0;
		} else {
			if (!mesh_storage->owns_multimesh(fed.gpu_source)) {
				continue;
			}
			source.buffer = mesh_storage->_multimesh_get_buffer_rd_rid(fed.gpu_source);
			uint32_t current_offset = 0;
			uint32_t previous_offset = 0;
			mesh_storage->_multimesh_get_motion_vectors_offsets(fed.gpu_source, current_offset, previous_offset);
			source.element_offset = current_offset;
			if (fed.gpu_indirect) {
				source.count_buffer = mesh_storage->_multimesh_get_command_buffer_rd_rid(fed.gpu_source);
			}
		}
		if (source.buffer.is_null()) {
			continue;
		}
		source.distance = (fed.has_rt_transform ? fed.rt_transform.origin : Vector3()).distance_to(camera_pos);
		gpu_candidates.push_back(source);
	}
	std::sort(gpu_candidates.ptr(), gpu_candidates.ptr() + gpu_candidates.size(), [](const GpuSource &p_a, const GpuSource &p_b) { return p_a.distance > p_b.distance; });

	LocalVector<GpuSource> gpu_sources;
	for (const GpuSource &source : gpu_candidates) {
		if (source.count > dynamic_budget) {
			stats.coverage.instance_cap += source.count;
			continue;
		}
		dynamic_budget -= source.count;
		gpu_sources.push_back(source);
	}

	// GodotNext: nextgi_skinned_actors (#59). Counts refits and rebuilds of this frame's
	// skinned BLAS work; the marker sits inside the "AS updates" slot (line 1a).
	if (stats.skinned_refit_triangles > 0 || stats.skinned_build_triangles > 0) {
		RENDER_TIMESTAMP("Skinned refit");
	}
	// A skinned part absent for the full drop-out window is freed; its slot geometry may be reused.
	{
		LocalVector<uint64_t> stale;
		for (const KeyValue<uint64_t, SkinnedPart> &entry : skinned_parts) {
			if (entry.value.last_frame + NEXTGI_RT_ABSENT_FRAMES < frame) {
				stale.push_back(entry.key);
			}
		}
		for (uint64_t key : stale) {
			SkinnedPart *part = skinned_parts.getptr(key);
			if (part != nullptr && part->blas.is_valid()) {
				RD::get_singleton()->free_rid(part->blas);
			}
			skinned_parts.erase(key);
		}
	}

	stats.instance_count = instance_records.size();
	if (OS::get_singleton() != nullptr) {
		stats.tlas_fill_ms = double(OS::get_singleton()->get_ticks_usec() - ticks_start) / 1000.0;
	}
	if (instance_records.is_empty() && gpu_sources.is_empty()) {
		_update_far_records(p_render_data, instance_records, surface_records);
		// GodotNext: ps_lumen_lite (#183)
		if (NextGIRelight::get_singleton() != nullptr) {
			NextGIRelight::get_singleton()->finish_record_pass(instance_records);
		}
		_ensure_record_buffers(instance_records.size(), surface_records.size());
		if (instance_record_buffer.is_valid() && !instance_records.is_empty()) {
			RD::get_singleton()->buffer_update(instance_record_buffer, 0, instance_records.size() * sizeof(NextGIInstanceRecord), instance_records.ptr());
		}
		if (surface_record_buffer.is_valid() && !surface_records.is_empty()) {
			RD::get_singleton()->buffer_update(surface_record_buffer, 0, surface_records.size() * sizeof(NextGISurfaceRecord), surface_records.ptr());
		}
		return;
	}
	_update_far_records(p_render_data, instance_records, surface_records);
	// GodotNext: ps_lumen_lite (#183). Records whose pages a later instance of this pass evicted.
	if (NextGIRelight::get_singleton() != nullptr) {
		NextGIRelight::get_singleton()->finish_record_pass(instance_records);
	}
	_ensure_record_buffers(instance_records.size(), surface_records.size());
	if (instance_record_buffer.is_valid()) {
		RD::get_singleton()->buffer_update(instance_record_buffer, 0, instance_records.size() * sizeof(NextGIInstanceRecord), instance_records.ptr());
	}
	if (surface_record_buffer.is_valid() && !surface_records.is_empty()) {
		RD::get_singleton()->buffer_update(surface_record_buffer, 0, surface_records.size() * sizeof(NextGISurfaceRecord), surface_records.ptr());
	}

	// No debug label here: the frame block's slot label names this work.
	RenderingDevice *rd = RD::get_singleton();
	bool built = false;
	if (!gpu_sources.is_empty() && _ensure_gpu_instance_buffer()) {
		const uint32_t record_size = rd->acceleration_structure_get_instance_size();
		PackedByteArray cpu_bytes;
		cpu_bytes.resize(tlas_instances.size() * record_size);
		bool cpu_ok = true;
		for (uint32_t i = 0; i < tlas_instances.size(); i++) {
			if (rd->acceleration_structure_instance_encode(tlas_instances[i], cpu_bytes.ptrw() + i * record_size) != OK) {
				cpu_ok = false;
				break;
			}
		}
		if (cpu_ok) {
			if (!cpu_bytes.is_empty()) {
				rd->buffer_update(gpu_instance_buffer, 0, cpu_bytes.size(), cpu_bytes.ptr());
			}
			LocalVector<RID> referenced_blases;
			referenced_blases.reserve(tlas_instances.size() + gpu_sources.size());
			for (const RD::AccelerationStructureInstance &instance : tlas_instances) {
				if (instance.blas.is_valid()) {
					referenced_blases.push_back(instance.blas);
				}
			}
			uint32_t cursor = tlas_instances.size();
			for (const GpuSource &source : gpu_sources) {
				const uint32_t encoded = encode_gpu_instances(source.buffer, source.stride, source.count, source.count, cursor, source.blas, cursor, 0u, camera_pos, NEXTGI_RT_ANGULAR_CULL_DISTANCE, Math::deg_to_rad(0.5f), source.element_offset, source.count_buffer);
				if (encoded == 0) {
					continue;
				}
				cursor += encoded;
				referenced_blases.push_back(source.blas);
				stats.coverage.numerator += encoded;
			}
			stats.instance_count += cursor - tlas_instances.size();
			if (cursor > 0) {
				built = rd->tlas_build_from_buffer(tlas, gpu_instance_buffer, 0, cursor, referenced_blases) == OK;
			}
		}
	} else {
		built = rd->tlas_build(tlas, Span<RD::AccelerationStructureInstance>(tlas_instances.ptr(), tlas_instances.size())) == OK;
	}
	if (built) {
		stats.tlas_build_count++;
		tlas_traceable = true;
	}
	sdf_instances.upload();
	// GodotNext: ps_mesh_sdf_atlas (#185). Request mips from camera distance;
	// the readback is applied one frame later with a 4 MiB budget.
	if (sdf_stream == nullptr) {
		sdf_stream = memnew(NextGISDFStream);
	}
	sdf_stream->poll(mesh_storage->get_sdf_atlas(), frame);
	if (mesh_storage->get_sdf_atlas().get_resident_bytes() > 0) {
		sdf_stream->dispatch(sdf_instances.get_buffer(), sdf_instances.get_count(), camera_origin);
	}
	update_global_sdf(p_render_data);
}

// GodotNext: nextgi_as_slots
void NextGIRTScene::build_dirty_blas(const ::RenderDataRD *p_render_data) {
	ERR_FAIL_NULL(p_render_data);
	if (tlas.is_null()) {
		return;
	}
	const LocalVector<RendererSceneRender::NextGIRTInstance> *fed_instances = _nextgi_rt_fed_instances(p_render_data);
	if (fed_instances == nullptr || fed_instances->is_empty()) {
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	if (!mesh_storage->mesh_nextgi_rt_is_enabled()) {
		return;
	}
	const uint64_t ticks_start = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
	const uint32_t spent_triangles = compaction_triangles_this_frame;

	// New BLASes first, then the largest angular radius. A mesh or proxy part that does not fit stays dirty.
	struct BuildTarget {
		RID mesh;
		uint32_t part_index = UINT32_MAX; // UINT32_MAX = legacy whole-mesh build; else single VGD proxy part.
	};
	HashMap<RID, float> max_angular;
	LocalVector<RID> dirty_meshes;
	for (const RendererSceneRender::NextGIRTInstance &fed : *fed_instances) {
		RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
		if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
			continue;
		}
		const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry_instance->data->base;
		if (!mesh_storage->owns_mesh(mesh)) {
			continue;
		}
		if (RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->mesh_has_virtual_geometry(mesh)) {
			mesh_storage->mesh_nextgi_sync_vgd_proxy(mesh);
		}
		if (!mesh_storage->mesh_nextgi_rt_is_blas_dirty(mesh)) {
			continue;
		}
		if (mesh_storage->mesh_nextgi_rt_is_dynamic(mesh) && mesh_storage->mesh_nextgi_rt_allows_update(mesh)) {
			// Line 1a already refit this mesh, or excluded it past the refit cap.
			continue;
		}
		if (float *existing = max_angular.getptr(mesh)) {
			*existing = MAX(*existing, fed.angular_radius);
		} else {
			max_angular.insert(mesh, fed.angular_radius);
			dirty_meshes.push_back(mesh);
		}
	}

	LocalVector<BuildTarget> targets;
	LocalVector<NextGIBuildItem> items;
	for (const RID &mesh : dirty_meshes) {
		const float angular = max_angular[mesh];
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
		const bool is_proxy_mesh = parts != nullptr && !parts->is_empty() && (*parts)[0].is_proxy;
		if (is_proxy_mesh) {
			bool any_pending_part = false;
			for (uint32_t p = 0; p < parts->size(); p++) {
				const MeshStorage::NextGIRTPart &part = (*parts)[p];
				if (part.geometries.is_empty() || part.blas.is_valid()) {
					continue;
				}
				any_pending_part = true;
				NextGIBuildItem item;
				item.id = targets.size();
				item.triangles = part.triangle_count;
				item.angular_radius = angular;
				item.is_new = true;
				BuildTarget target;
				target.mesh = mesh;
				target.part_index = p;
				targets.push_back(target);
				items.push_back(item);
			}
			if (!any_pending_part) {
				mesh_storage->mesh_nextgi_rt_set_blas_dirty(mesh, false);
			}
			continue;
		}

		NextGIBuildItem item;
		item.id = targets.size();
		item.triangles = _mesh_triangle_count(mesh);
		item.angular_radius = angular;
		item.is_new = parts == nullptr || parts->is_empty();
		if (!item.is_new) {
			item.is_new = true;
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					item.is_new = false;
					break;
				}
			}
		}
		BuildTarget target;
		target.mesh = mesh;
		target.part_index = UINT32_MAX;
		targets.push_back(target);
		items.push_back(item);
	}

	LocalVector<uint32_t> accepted_ids;
	accepted_ids.resize(items.size());
	uint32_t scheduled_triangles = 0;
	const uint32_t triangle_budget = spent_triangles >= uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP) ? 0 : uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP) - spent_triangles;
	const uint32_t accepted_count = nextgi_rt_schedule_builds(items.ptr(), items.size(), triangle_budget, accepted_ids.ptr(), accepted_ids.size(), scheduled_triangles);
	for (uint32_t i = 0; i < accepted_count; i++) {
		const BuildTarget &target = targets[accepted_ids[i]];
		if (target.part_index != UINT32_MAX) {
			_build_proxy_part(target.mesh, target.part_index);
		} else {
			_build_dirty_mesh_parts(target.mesh);
			// Rebuilding a BLAS that this frame's TLAS already references invalidates that TLAS.
			tlas_traceable = false;
		}
	}

	uint64_t frame = 0;
	if (RSG::rasterizer != nullptr) {
		frame = RSG::rasterizer->get_frame_number();
	}
	_apply_as_budget(frame);
	if (OS::get_singleton() != nullptr) {
		stats.scene_update_ms = double(OS::get_singleton()->get_ticks_usec() - ticks_start) / 1000.0;
	}
}

void NextGIRTScene::compact_after_traces() {
	if (tlas.is_null() || RD::get_singleton() == nullptr || pending_compactions.is_empty()) {
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	LocalVector<PendingCompact> deferred;
	for (const PendingCompact &pending : pending_compactions) {
		LocalVector<MeshStorage::NextGIRTPart> *parts = nullptr;
		if (pending.mesh.is_valid()) {
			parts = (mesh_storage != nullptr && mesh_storage->owns_mesh(pending.mesh)) ? mesh_storage->mesh_nextgi_rt_get_parts(pending.mesh) : nullptr;
			if (parts == nullptr || pending.part_index >= parts->size() || (*parts)[pending.part_index].blas != pending.blas) {
				continue;
			}
		}
		if (compaction_triangles_this_frame + pending.triangles > uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP)) {
			deferred.push_back(pending);
			continue;
		}
		if (RD::get_singleton()->blas_compact(pending.blas) == OK) {
			if (parts != nullptr && pending.part_index < parts->size()) {
				(*parts)[pending.part_index].compacted = true;
			}
			stats.compaction_copies++;
			compaction_triangles_this_frame += pending.triangles;
			// The copy completes on a later device frame and invalidates this TLAS.
			// Traces in this frame have already been recorded.
			tlas_traceable = false;
		}
	}
	pending_compactions = deferred;
}

uint32_t NextGIRTScene::_mesh_triangle_count(RID p_mesh) const {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return 0;
	}
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts != nullptr && !parts->is_empty()) {
		uint32_t triangles = 0;
		for (const MeshStorage::NextGIRTPart &part : *parts) {
			triangles += part.triangle_count;
		}
		if (triangles > 0) {
			return triangles;
		}
	}
	uint32_t triangles = 0;
	const uint32_t surface_count = uint32_t(mesh_storage->mesh_get_surface_count(p_mesh));
	for (uint32_t i = 0; i < surface_count; i++) {
		MeshStorage::NextGIRTSurfaceInfo info;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, i, info) || !info.eligible || _nextgi_rt_material_is_alpha(info.material)) {
			continue;
		}
		triangles += info.index_count / 3;
	}
	return triangles;
}

void NextGIRTScene::_apply_as_budget(uint64_t p_frame) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || RD::get_singleton() == nullptr) {
		return;
	}
	LocalVector<RID> ids;
	LocalVector<NextGIASMesh> meshes;
	LocalVector<RID> stale_meshes;
	for (const KeyValue<RID, uint64_t> &entry : mesh_last_tlas_frame) {
		if (!mesh_storage->owns_mesh(entry.key)) {
			stale_meshes.push_back(entry.key);
			continue;
		}
		NextGIASMesh mesh;
		mesh.id = ids.size();
		ids.push_back(entry.key);
		uint64_t bytes = 0;
		const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(entry.key);
		if (parts != nullptr) {
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					bytes += RD::get_singleton()->acceleration_structure_get_size_bytes(part.blas);
				}
			}
		}
		mesh.bytes = bytes;
		mesh.frames_absent = p_frame > entry.value ? uint32_t(p_frame - entry.value) : 0;
		meshes.push_back(mesh);
	}
	for (const RID &stale : stale_meshes) {
		mesh_last_tlas_frame.erase(stale);
	}
	LocalVector<uint32_t> evict;
	evict.resize(meshes.size());
	uint32_t evict_count = 0;
	const bool over = meshes.is_empty() ? false : nextgi_rt_evict_as_memory(meshes.ptr(), meshes.size(), as_budget_bytes, NEXTGI_RT_ABSENT_FRAMES, evict.ptr(), evict_count);
	for (uint32_t i = 0; i < evict_count; i++) {
		mesh_storage->mesh_nextgi_rt_clear_parts(ids[evict[i]]);
		mesh_last_tlas_frame.erase(ids[evict[i]]);
		tlas_traceable = false;
	}
	uint64_t total = 0;
	for (uint32_t i = 0; i < meshes.size(); i++) {
		bool evicted = false;
		for (uint32_t e = 0; e < evict_count; e++) {
			if (evict[e] == meshes[i].id) {
				evicted = true;
				break;
			}
		}
		if (!evicted) {
			total += meshes[i].bytes;
		}
	}
	stats.as_bytes = total;
	as_memory_exceeded = over;
}

bool NextGIRTScene::_ensure_gpu_instance_buffer() {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (gpu_instance_buffer.is_valid() && gpu_instance_capacity == NEXTGI_RT_TLAS_CAPACITY) {
		return true;
	}
	if (gpu_instance_buffer.is_valid()) {
		rd->free_rid(gpu_instance_buffer);
		gpu_instance_buffer = RID();
	}
	const uint32_t record_size = rd->acceleration_structure_get_instance_size();
	ERR_FAIL_COND_V(record_size == 0, false);
	gpu_instance_buffer = rd->storage_buffer_create(NEXTGI_RT_TLAS_CAPACITY * record_size, {}, 0, RenderingDevice::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	if (gpu_count_dummy.is_null()) {
		const uint32_t zero = 0;
		gpu_count_dummy = rd->storage_buffer_create(sizeof(uint32_t), Span<uint8_t>((uint8_t *)&zero, sizeof(zero)));
	}
	gpu_instance_capacity = gpu_instance_buffer.is_valid() ? NEXTGI_RT_TLAS_CAPACITY : 0;
	return gpu_instance_buffer.is_valid();
}

uint32_t NextGIRTScene::encode_gpu_instances(RID p_source_buffer, uint32_t p_source_stride_floats, uint32_t p_count, uint32_t p_visible_count, uint32_t p_dest_offset, RID p_blas, uint32_t p_id_base, uint32_t p_flags, const Vector3 &p_camera, float p_max_distance, float p_angle_limit, uint32_t p_source_element_offset, RID p_count_buffer) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, 0);
	ERR_FAIL_COND_V(p_count == 0 || p_source_buffer.is_null() || p_blas.is_null() || p_source_stride_floats < 12, 0);
	ERR_FAIL_COND_V(p_dest_offset + p_count > NEXTGI_RT_TLAS_CAPACITY, 0);
	ERR_FAIL_NULL_V(instance_encoder, 0);
	ERR_FAIL_COND_V(!_ensure_gpu_instance_buffer(), 0);

	const uint64_t blas_reference = rd->acceleration_structure_get_instance_reference(p_blas);
	ERR_FAIL_COND_V_MSG(blas_reference == 0, 0, "The BLAS has no GPU instance reference on this API.");

	const int layout = rd->acceleration_structure_get_instance_size() == 72 ? INSTANCE_LAYOUT_METAL_72 : INSTANCE_LAYOUT_VULKAN_64;
	RID shader = instance_encoder->shader.version_get_shader(instance_encoder->version, layout);

	const bool indirect = p_count_buffer.is_valid() && gpu_count_dummy.is_valid();
	RD::Uniform u_source(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_source_buffer);
	RD::Uniform u_destination(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, gpu_instance_buffer);
	RD::Uniform u_count(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, indirect ? p_count_buffer : gpu_count_dummy);
	RD::Uniform uniforms[3] = { u_source, u_destination, u_count };
	RID uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 3), shader, 0);
	ERR_FAIL_COND_V(uniform_set.is_null(), 0);

	uint32_t push_constants[24] = {};
	// counts: element count, source offset, destination offset, id base.
	push_constants[0] = p_count;
	push_constants[1] = p_source_element_offset;
	push_constants[2] = p_dest_offset;
	push_constants[3] = p_id_base;
	// layout: stride in floats, visible count, API flags, instance mask.
	push_constants[4] = p_source_stride_floats;
	push_constants[5] = p_visible_count;
	push_constants[6] = p_flags;
	push_constants[7] = 0xFFu;
	// camera: position and the distance limit.
	auto to_bits = [](float p_value) {
		uint32_t bits = 0;
		memcpy(&bits, &p_value, sizeof(bits));
		return bits;
	};
	push_constants[8] = to_bits(p_camera.x);
	push_constants[9] = to_bits(p_camera.y);
	push_constants[10] = to_bits(p_camera.z);
	push_constants[11] = to_bits(p_max_distance);
	// refs: BLAS reference low and high.
	push_constants[12] = uint32_t(blas_reference & 0xFFFFFFFFu);
	push_constants[13] = uint32_t(blas_reference >> 32u);
	// limits: angular radius limit in radians.
	push_constants[16] = to_bits(p_angle_limit);
	// extra: the element count comes from the count buffer (indirect MultiMesh).
	push_constants[20] = indirect ? 1u : 0u;

	const uint32_t groups = MAX((p_count + 63u) / 64u, 1u);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, instance_encoder->pipelines[layout]);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, push_constants, sizeof(push_constants));
	rd->compute_list_dispatch(list, groups, 1, 1);
	rd->compute_list_end();
	rd->free_rid(uniform_set);
	return p_count;
}

String NextGIRTScene::build_instances_shader_source(int p_layout) {
	NextgiRtInstancesShaderRD shader;
	Vector<String> defines;
	defines.push_back("\n#define INSTANCE_64\n");
	defines.push_back("\n#define INSTANCE_72\n");
	shader.initialize(defines);
	const RID version = shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = shader.version_build_variant_stage_sources(version, CLAMP(p_layout, 0, int(INSTANCE_LAYOUT_MAX) - 1));
	shader.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

} // namespace RendererRD

namespace RendererRD {

static Vector<NextGIGlobalSDF::Instance> nextgi_global_sdf_instances(const NextGISDFInstances &p_instances) {
	static LocalVector<MeshSDFData> fields;
	fields.clear();
	const uint32_t count = p_instances.get_count();
	fields.reserve(count);
	Vector<NextGIGlobalSDF::Instance> instances;
	instances.reserve(count);
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr) {
		return instances;
	}
	for (uint32_t index = 0; index < count; index++) {
		const RID mesh = p_instances.get_mesh(index);
		const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(mesh);
		if (pages == nullptr || pages->field.is_null()) {
			continue;
		}
		fields.push_back(pages->field->get_data());
		NextGIGlobalSDF::Instance instance;
		instance.transform = p_instances.get_transform(index);
		instance.field = &fields[fields.size() - 1];
		instance.dynamic = p_instances.is_dynamic(index);
		instance.two_sided = pages->field->is_two_sided();
		instances.push_back(instance);
	}
	return instances;
}

void NextGIRTScene::update_global_sdf(const ::RenderDataRD *p_render_data) {
	if (!bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/global_sdf"))) {
		return;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ERR_FAIL_NULL(mesh_storage);
	const uint64_t generation = mesh_storage->get_sdf_atlas().get_generation();
	// GodotNext: ps_swrt_trace (#186). The clipmap upkeep is shared with the SWRT tier.
	if (generation != global_sdf_generation) {
		global_sdf.free_all();
		global_sdf_clipmaps.reset();
		global_sdf_generation = generation;
	}
	const Vector<NextGIGlobalSDF::Instance> instances = nextgi_global_sdf_instances(sdf_instances);
	if (instances.is_empty()) {
		if (global_sdf_clipmaps.is_level_valid(0)) {
			global_sdf.free_all();
			global_sdf_clipmaps.reset();
		}
		return;
	}
	const Vector3 camera = p_render_data != nullptr && p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
	const float view_distance = p_render_data != nullptr ? RendererEnvironmentStorage::get_singleton()->environment_get_nextgi_scene_view_distance(p_render_data->environment) : 200.0f;
	const uint32_t levels = NextGIGlobalSDF::level_count(view_distance);
	if (p_render_data != nullptr && p_render_data->sdfgi_update_data != nullptr) {
		global_sdf_clipmaps.add_dirty_volumes(p_render_data->sdfgi_update_data->nextgi_dirty_volumes, p_render_data->sdfgi_update_data->nextgi_dirty_volume_count);
	}
	global_sdf_clipmaps.update(global_sdf, instances, camera, frame, levels);
	global_sdf_origin = global_sdf_clipmaps.get_origin(0);
}

Dictionary NextGIRTScene::get_global_sdf_debug() const {
	Dictionary info;
	info["global_sdf_checked"] = 0;
	info["global_sdf_max_error"] = 0.0;
	if (!global_sdf_clipmaps.is_level_valid(0)) {
		return info;
	}
	const Vector<NextGIGlobalSDF::Instance> instances = nextgi_global_sdf_instances(sdf_instances);
	PackedFloat32Array slice;
	PackedByteArray coverage;
	PackedByteArray step_counts;
	slice.resize(NextGIGlobalSDF::LEVEL_SIZE * NextGIGlobalSDF::LEVEL_SIZE);
	coverage.resize(slice.size());
	step_counts.resize(slice.size());
	int slice_y = 16;
	float slice_area = 0.0f;
	Vector3 slice_center;
	const AABB level_bounds(Vector3(global_sdf_origin) * NextGIGlobalSDF::level_voxel_size(0), Vector3(NextGIGlobalSDF::level_extent(0), NextGIGlobalSDF::level_extent(0), NextGIGlobalSDF::level_extent(0)));
	for (const NextGIGlobalSDF::Instance &instance : instances) {
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		const AABB overlap = instance.transform.xform(instance.field->bounds).intersection(level_bounds);
		const float area = overlap.size.x * overlap.size.z;
		if (overlap.size.y > 0.0f && area > slice_area) {
			slice_area = area;
			slice_center = instance.transform.xform(instance.field->bounds.get_center());
		}
	}
	if (slice_area > 0.0f) {
		slice_y = CLAMP(NextGIGlobalSDF::world_to_voxel(0, global_sdf_origin, slice_center).y, 0, int(NextGIGlobalSDF::LEVEL_SIZE) - 1);
	}
	slice.fill(65504.0f);
	float max_error = 0.0;
	int checked = 0;
	LocalVector<uint32_t> culled;
	culled.resize(instances.size());
	for (int brick_z = 0; brick_z < int(NextGIGlobalSDF::PAGE_SIZE); brick_z++) {
		for (int brick_x = 0; brick_x < int(NextGIGlobalSDF::PAGE_SIZE); brick_x++) {
			const Vector3i brick(brick_x, slice_y / int(NextGIGlobalSDF::BRICK_SIZE), brick_z);
			NextGIGlobalSDF::BrickSplit split;
			const uint32_t culled_count = NextGIGlobalSDF::cull_instances_for_brick(instances, 0, global_sdf_origin, brick, culled.ptr(), culled.size());
			if (!global_sdf.download_brick(0, brick, split)) {
				continue;
			}
			// GodotNext: ps_swrt_trace (#186). The cap's clearance (NextGIGlobalSDF::brick_cap()).
			const float clearance = NextGIGlobalSDF::brick_clearance(instances, 0, global_sdf_origin, brick);
			for (int local_z = 0; local_z < int(NextGIGlobalSDF::BRICK_SIZE); local_z++) {
				for (int local_x = 0; local_x < int(NextGIGlobalSDF::BRICK_SIZE); local_x++) {
					const int x = brick_x * int(NextGIGlobalSDF::BRICK_SIZE) + local_x;
					const int z = brick_z * int(NextGIGlobalSDF::BRICK_SIZE) + local_z;
					// GodotNext: ps_swrt_trace (#186). The brick-local row of slice_y, as the reference.
					const int local_y = slice_y % int(NextGIGlobalSDF::BRICK_SIZE);
					const int local_index = local_x + int(NextGIGlobalSDF::BRICK_SIZE) * local_y + local_z * int(NextGIGlobalSDF::BRICK_SIZE * NextGIGlobalSDF::BRICK_SIZE);
					const int index = x + z * int(NextGIGlobalSDF::LEVEL_SIZE);
					const float distance = MIN(split.static_distance[local_index], split.dynamic_distance[local_index]);
					slice.set(index, distance);
					coverage.set(index, NextGIGlobalSDF::get_coverage_bit(split, Vector3i(local_x, local_y, local_z)) ? 1 : 0);
					const Vector3 world = NextGIGlobalSDF::voxel_center(0, global_sdf_origin, Vector3i(x, slice_y, z));
					// GodotNext: ps_swrt_trace (#186). The CPU twin of the composite, cap included.
					const float reference = culled_count > 0 ? NextGIGlobalSDF::composite_distance(instances, culled.ptr(), culled_count, NextGIGlobalSDF::brick_cull_box(0, global_sdf_origin, brick), clearance, world) : 65504.0f;
					max_error = MAX(max_error, Math::abs(distance - reference));
					checked++;
				}
			}
		}
	}
	LocalVector<float> coarse;
	const bool coarse_valid = global_sdf.download_coarse(0, coarse);
	for (int z = 0; z < int(NextGIGlobalSDF::LEVEL_SIZE); z++) {
		for (int x = 0; x < int(NextGIGlobalSDF::LEVEL_SIZE); x++) {
			// GodotNext: global_sdf_coarse_bound (#257). Two cells in: the level's edge cells never step.
			const Vector3 start = NextGIGlobalSDF::voxel_center(0, global_sdf_origin, Vector3i(x, slice_y, 8));
			const uint32_t steps = coarse_valid ? NextGIGlobalSDF::trace_coarse_steps(coarse, 0, global_sdf_origin, start, Vector3(0.0f, 0.0f, 1.0f)) : 0;
			step_counts.set(x + z * int(NextGIGlobalSDF::LEVEL_SIZE), MIN(steps, 255u));
		}
	}
	uint32_t sign_mismatches = 0;
	uint32_t random_checked = 0;
	float random_max_error = 0.0;
	const bool random_valid = global_sdf.validate_reference(instances, 0, global_sdf_origin, 10000, random_max_error, sign_mismatches, random_checked);
	info["global_sdf_slice"] = slice;
	info["global_sdf_coverage"] = coverage;
	info["global_sdf_step_counts"] = step_counts;
	info["global_sdf_coarse_valid"] = coarse_valid;
	// GodotNext: global_sdf_coarse_bound (#257). The GPU bound against its CPU twin and against
	// the nearest instance at random points of level 0 (a quarter voxel of tolerance, as the
	// composite's overestimate check).
	uint32_t coarse_checked = 0;
	uint32_t coarse_overestimates = 0;
	uint32_t coarse_twin_mismatches = 0;
	const bool coarse_checks = global_sdf.validate_coarse(instances, 0, global_sdf_origin, 10000, 0.25f * NextGIGlobalSDF::level_voxel_size(0), coarse_checked, coarse_overestimates, coarse_twin_mismatches);
	info["global_sdf_coarse_checked"] = coarse_checks ? int64_t(coarse_checked) : int64_t(-1);
	info["global_sdf_coarse_overestimates"] = int64_t(coarse_overestimates);
	info["global_sdf_coarse_twin_mismatches"] = int64_t(coarse_twin_mismatches);
	info["global_sdf_slice_y"] = slice_y;
	info["global_sdf_origin"] = global_sdf_origin;
	info["global_sdf_instances"] = instances.size();
	info["global_sdf_checked"] = checked;
	info["global_sdf_max_error"] = max_error;
	info["global_sdf_random_valid"] = random_valid;
	info["global_sdf_random_checked"] = random_checked;
	info["global_sdf_random_max_error"] = random_max_error;
	info["global_sdf_sign_mismatches"] = sign_mismatches;
	info["global_sdf_used_bricks"] = global_sdf.get_used_bricks();
	uint32_t valid_levels = 0;
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		valid_levels += global_sdf_clipmaps.is_level_valid(level) ? 1 : 0;
	}
	info["global_sdf_levels"] = valid_levels;
	return info;
}

} // namespace RendererRD
