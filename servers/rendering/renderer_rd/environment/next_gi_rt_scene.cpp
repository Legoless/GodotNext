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
// GodotNext: nextgi_hit_groups (#130)
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
// GodotNext: nextgeo_rt_native (#156)
#include "servers/rendering/renderer_rd/environment/next_gi_rt_native.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_instances.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
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

// GodotNext: nextgi_hit_groups (#130)
uint32_t NextGIArenaAllocator::release_trailing_free_chunks() {
	bool released = true;
	while (chunk_count > 0 && released) {
		released = false;
		for (uint32_t i = 0; i < free_ranges.size(); i++) {
			const FreeRange &range = free_ranges[i];
			if (range.chunk == chunk_count - 1 && range.offset == 0 && range.size == chunk_size) {
				free_ranges.remove_at(i);
				chunk_count--;
				released = true;
				break;
			}
		}
	}
	return chunk_count;
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
		uint32_t &r_alpha_excluded_count,
		LocalVector<NextGIRTProxyPartUpload> *r_shadow_uploads) {
	r_uploads.clear();
	if (r_shadow_uploads != nullptr) {
		r_shadow_uploads->clear();
	}
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
	LocalVector<LegacySlice> shadow_legacy_queue;
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
				if (r_shadow_uploads != nullptr) {
					const NextGIRTLegacySurfaceInput &leg = p_legacy_surfaces[s];
					const uint32_t leg_tris = leg.index_count / 3;
					if (leg.positions != nullptr && leg.vertex_count > 0 && leg.indices != nullptr && leg_tris > 0) {
						LegacySlice slice;
						slice.surface = s;
						slice.first_triangle = 0;
						slice.triangle_count = leg_tris;
						shadow_legacy_queue.push_back(slice);
					}
				}
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
	uint32_t shadow_legacy_head = 0;
	struct ProxyRange {
		uint32_t surface = 0;
		uint32_t first_triangle = 0;
		uint32_t triangle_count = 0;
		uint32_t group_rel = 0;
	};

	auto emit_part = [&](uint32_t p_v_off, uint32_t p_v_cnt, uint32_t p_i_off, uint32_t p_i_cnt, LocalVector<ProxyRange> &p_proxy_ranges, const LocalVector<LegacySlice> &p_legacy_slices, bool p_with_cards, bool p_compact_proxy_indices, LocalVector<NextGIRTProxyPartUpload> &p_target) -> bool {
		uint32_t cursor = 0;
		uint32_t proxy_pos_rel = 0;
		uint32_t proxy_idx_rel = 0;
		uint32_t packed_proxy_indices = p_i_cnt;
		if (!p_proxy_ranges.is_empty()) {
			if (p_compact_proxy_indices) {
				packed_proxy_indices = 0;
				for (const ProxyRange &range : p_proxy_ranges) {
					packed_proxy_indices += range.triangle_count * 3u;
				}
			}
			proxy_pos_rel = 0;
			cursor = _nextgi_align16(p_v_cnt * sizeof(float) * 3u);
			proxy_idx_rel = cursor;
			cursor = _nextgi_align16(proxy_idx_rel + packed_proxy_indices * sizeof(uint32_t));
			if (p_with_cards && has_cards && has_group_table) {
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
			upload.proxy_vertex_offset = p_v_off;
			upload.proxy_vertex_count = p_v_cnt;
			upload.proxy_index_offset = p_i_off;
			upload.proxy_index_count = p_i_cnt;
			memcpy(dst + proxy_pos_rel, p_desc.rt_proxy_positions.ptr() + uint64_t(p_v_off) * 3u, uint64_t(p_v_cnt) * sizeof(float) * 3u);
			if (!p_compact_proxy_indices) {
				memcpy(dst + proxy_idx_rel, p_desc.rt_proxy_indices.ptr() + uint64_t(p_i_off) * 4u, uint64_t(p_i_cnt) * sizeof(uint32_t));
			}
			const uint32_t part_first_tri_global = p_i_off / 3u;
			uint32_t packed_tri_cursor = 0;
			for (const ProxyRange &range : p_proxy_ranges) {
				uint32_t range_idx_rel = proxy_idx_rel + range.first_triangle * 3u * sizeof(uint32_t);
				if (p_compact_proxy_indices) {
					range_idx_rel = proxy_idx_rel + packed_tri_cursor * 3u * sizeof(uint32_t);
					memcpy(dst + range_idx_rel, p_desc.rt_proxy_indices.ptr() + uint64_t(p_i_off + range.first_triangle * 3u) * 4u, uint64_t(range.triangle_count) * 3u * sizeof(uint32_t));
					packed_tri_cursor += range.triangle_count;
				}
				const bool write_cards = p_with_cards && has_cards && has_group_table;
				if (write_cards) {
					memcpy(dst + range.group_rel, p_desc.rt_proxy_triangle_groups.ptr() + uint64_t(part_first_tri_global + range.first_triangle) * sizeof(uint16_t), uint64_t(range.triangle_count) * sizeof(uint16_t));
				}
				NextGIRTPartGeometry geom;
				geom.surface = range.surface;
				geom.first_triangle = 0;
				geom.triangle_count = range.triangle_count;
				geom.is_proxy_surface = true;
				geom.is_legacy_surface = false;
				geom.has_stage2_cards = write_cards;
				geom.vertex_offset = alloc.offset + proxy_pos_rel;
				geom.vertex_count = p_v_cnt;
				geom.index_offset = alloc.offset + range_idx_rel;
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
			geom.is_legacy_surface = true;
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
		p_target.push_back(upload);
		return true;
	};

	auto rollback = [&]() {
		for (const NextGIRTProxyPartUpload &u : r_uploads) {
			r_allocator.free_range(u.allocation);
		}
		r_uploads.clear();
		if (r_shadow_uploads != nullptr) {
			for (const NextGIRTProxyPartUpload &u : *r_shadow_uploads) {
				r_allocator.free_range(u.allocation);
			}
			r_shadow_uploads->clear();
		}
		return false;
	};

	auto take_legacy_slices = [](LocalVector<LegacySlice> &r_queue, uint32_t &r_head, uint32_t &r_part_tris, LocalVector<LegacySlice> &r_out) {
		while (r_head < r_queue.size() && r_part_tris < NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
			LegacySlice &front = r_queue[r_head];
			// Keep a legacy surface whole if it fits a fresh part alone and would overflow the part.
			if (r_part_tris > 0 && front.first_triangle == 0 && front.triangle_count <= NEXTGI_RT_MAX_TRIANGLES_PER_BLAS && r_part_tris + front.triangle_count > NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
				break;
			}
			const uint32_t take = MIN(front.triangle_count, uint32_t(NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) - r_part_tris);
			LegacySlice slice;
			slice.surface = front.surface;
			slice.first_triangle = front.first_triangle;
			slice.triangle_count = take;
			r_out.push_back(slice);
			r_part_tris += take;
			front.first_triangle += take;
			front.triangle_count -= take;
			if (front.triangle_count == 0) {
				r_head++;
			}
		}
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
		LocalVector<ProxyRange> shadow_proxy_ranges;
		uint32_t part_tris = 0;
		uint32_t shadow_part_tris = 0;
		for (uint32_t s = 0; s < map_surfaces; s++) {
			const bool is_alpha = p_alpha_excluded_surfaces != nullptr && p_alpha_excluded_surfaces[s] != 0;
			if (is_alpha && r_shadow_uploads == nullptr) {
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
			if (is_alpha) {
				shadow_proxy_ranges.push_back(range);
				shadow_part_tris += range.triangle_count;
			} else {
				proxy_ranges.push_back(range);
				part_tris += range.triangle_count;
			}
		}
		if (part_tris > NEXTGI_RT_MAX_TRIANGLES_PER_BLAS || shadow_part_tris > NEXTGI_RT_MAX_TRIANGLES_PER_BLAS) {
			return rollback();
		}

		LocalVector<LegacySlice> part_legacy;
		// Outside-mask surfaces append as extra geometries of the first part (#77 step 2),
		// bounded by NEXTGI_RT_MAX_TRIANGLES_PER_BLAS.
		if (r_uploads.is_empty()) {
			take_legacy_slices(legacy_queue, legacy_head, part_tris, part_legacy);
		}

		if (!proxy_ranges.is_empty() || !part_legacy.is_empty()) {
			if (!emit_part(v_off, v_cnt, i_off, i_cnt, proxy_ranges, part_legacy, true, false, r_uploads)) {
				return rollback();
			}
		}

		// GodotNext: nextgi_nextgeo_alpha_shadows (#303). Alpha-tested and blended virtual surfaces stay
		// out of GI (D17) and emit shadow-only proxy parts so RT shadow rays still trace them.
		if (r_shadow_uploads != nullptr) {
			LocalVector<LegacySlice> shadow_part_legacy;
			if (r_shadow_uploads->is_empty()) {
				take_legacy_slices(shadow_legacy_queue, shadow_legacy_head, shadow_part_tris, shadow_part_legacy);
			}
			if (!shadow_proxy_ranges.is_empty() || !shadow_part_legacy.is_empty()) {
				if (!emit_part(v_off, v_cnt, i_off, i_cnt, shadow_proxy_ranges, shadow_part_legacy, false, true, *r_shadow_uploads)) {
					return rollback();
				}
			}
		}
	}

	// Emit any remaining outside-mask legacy slices as trailing <= 128k parts.
	while (legacy_head < legacy_queue.size()) {
		LocalVector<ProxyRange> empty_proxy;
		LocalVector<LegacySlice> part_legacy;
		uint32_t part_tris = 0;
		take_legacy_slices(legacy_queue, legacy_head, part_tris, part_legacy);
		if (!emit_part(0, 0, 0, 0, empty_proxy, part_legacy, true, false, r_uploads)) {
			return rollback();
		}
	}
	if (r_shadow_uploads != nullptr) {
		while (shadow_legacy_head < shadow_legacy_queue.size()) {
			LocalVector<ProxyRange> empty_proxy;
			LocalVector<LegacySlice> shadow_part_legacy;
			uint32_t shadow_part_tris = 0;
			take_legacy_slices(shadow_legacy_queue, shadow_legacy_head, shadow_part_tris, shadow_part_legacy);
			if (!emit_part(0, 0, 0, 0, empty_proxy, shadow_part_legacy, false, true, *r_shadow_uploads)) {
				return rollback();
			}
		}
	}

	return !r_uploads.is_empty() || (r_shadow_uploads != nullptr && !r_shadow_uploads->is_empty());
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

// GodotNext: nextgi_shadow_policy (#145). The material a shadow ray meets on a surface: the instance's override,
// its surface override, then the mesh's material, as the rasterizer picks it. GI keeps #61's rule (the mesh's
// material decides which surfaces it sees); shadow rays follow what the instance draws.
static RID _nextgi_shadow_effective_material(const RenderGeometryInstanceBase *p_instance, RID p_mesh, uint32_t p_surface) {
	if (p_instance != nullptr && p_instance->data != nullptr) {
		if (p_instance->data->material_override.is_valid()) {
			return p_instance->data->material_override;
		}
		if (int(p_surface) < p_instance->data->surface_materials.size() && p_instance->data->surface_materials[p_surface].is_valid()) {
			return p_instance->data->surface_materials[p_surface];
		}
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	return mesh_storage != nullptr && p_mesh.is_valid() ? mesh_storage->mesh_surface_get_material(p_mesh, int(p_surface)) : RID();
}

static NextGIShadowClass _nextgi_shadow_class(RID p_material) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	if (p_material.is_null() || material_storage == nullptr) {
		return NEXTGI_SHADOW_CLASS_OPAQUE;
	}
	switch (material_storage->material_get_nextgi_rt_class(p_material)) {
		case MaterialStorage::ShaderData::NEXTGI_RT_ALPHA_TESTED:
			return NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
		case MaterialStorage::ShaderData::NEXTGI_RT_BLENDED:
			return NEXTGI_SHADOW_CLASS_TRANSLUCENT;
		default:
			return NEXTGI_SHADOW_CLASS_OPAQUE;
	}
}

// GodotNext: nextgeo_rt_native (#156). Native-mode exclusions (plan §21.4 N13, #156 step 8). A
// material whose vertex code moves the surface (it writes VERTEX, POSITION, MODELVIEW_MATRIX or
// PROJECTION_MATRIX, or uses skip_vertex_transform) rasterizes away from the cut, so its instance
// keeps the proxy. Only the materials that shape the base surface count (the override or each
// surface's material): an overlay or next pass draws another layer over that surface and does not
// move the surface the TLAS carries.
static bool _nextgeo_rt_native_material_moves_vertices(RID p_material) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	if (p_material.is_null() || material_storage == nullptr) {
		return false;
	}
	MaterialStorage::ShaderData *shader_data = material_storage->material_get_shader_data(p_material);
	return shader_data != nullptr && shader_data->nextgeo_moves_vertices();
}

static bool _nextgeo_rt_native_vertex_offset(const RenderGeometryInstanceBase *p_instance, RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (p_instance == nullptr || p_instance->data == nullptr || mesh_storage == nullptr) {
		return false;
	}
	if (_nextgeo_rt_native_material_moves_vertices(p_instance->data->material_override)) {
		return true;
	}
	const int surface_count = mesh_storage->mesh_get_surface_count(p_mesh);
	for (int surface = 0; surface < surface_count; surface++) {
		const bool overridden = surface < p_instance->data->surface_materials.size() && p_instance->data->surface_materials[surface].is_valid();
		const RID material = overridden ? p_instance->data->surface_materials[surface] : mesh_storage->mesh_surface_get_material(p_mesh, surface);
		if (_nextgeo_rt_native_material_moves_vertices(material)) {
			return true;
		}
	}
	return false;
}

// The proxy also carries the mesh's legacy (non-virtual) surfaces; the native cut has none of them.
static bool _nextgeo_rt_native_legacy_surfaces(RID p_mesh, const LocalVector<MeshStorage::NextGIRTPart> &p_parts) {
	const RenderingServerTypes::VirtualGeometryDesc *desc = RSG::virtual_mesh_storage != nullptr ? RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(p_mesh) : nullptr;
	if (desc == nullptr) {
		return true;
	}
	for (const MeshStorage::NextGIRTPart &part : p_parts) {
		for (const NextGIRTPartGeometry &geometry : part.geometries) {
			if (geometry.surface >= 32u || (desc->eligible_surface_mask & (1u << geometry.surface)) == 0u) {
				return true;
			}
		}
	}
	return false;
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

bool NextGIRTScene::_skinned_part_blas_alive(const SkinnedPart &p_part) const {
	if (!p_part.blas.is_valid() || !p_part.chunk_buffer.is_valid()) {
		return false;
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	return mesh_storage != nullptr && p_part.chunk < mesh_storage->mesh_nextgi_rt_get_chunk_count() && mesh_storage->mesh_nextgi_rt_get_chunk_buffer(p_part.chunk) == p_part.chunk_buffer;
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
	// GodotNext: nextgeo_rt_native (#156)
	if (native != nullptr) {
		native->free();
		memdelete(native);
		native = nullptr;
	}
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
		if (_skinned_part_blas_alive(entry.value)) {
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
	// GodotNext: nextgi_shadow_policy (#145)
	if (shadow_instance_buffer.is_valid()) {
		RD::get_singleton()->free_rid(shadow_instance_buffer);
		shadow_instance_buffer = RID();
	}
	if (shadow_alpha_buffer.is_valid()) {
		RD::get_singleton()->free_rid(shadow_alpha_buffer);
		shadow_alpha_buffer = RID();
	}
	shadow_instance_capacity = 0;
	shadow_alpha_capacity = 0;
	shadow_allocator.reset();
	if (shadow_texture_table.is_valid()) {
		RD::get_singleton()->free_rid(shadow_texture_table);
		shadow_texture_table = RID();
	}
	if (shadow_dummy_buffer.is_valid()) {
		RD::get_singleton()->free_rid(shadow_dummy_buffer);
		shadow_dummy_buffer = RID();
	}
	shadow_texture_slots.clear();
	shadow_material_alpha.clear();
	shadow_streams_frame = 0;
	shadow_retrace_frame = 0;
	shadow_variant_frame = 0;
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

// GodotNext: nextgi_shadow_policy (#145)
void NextGIRTScene::_update_shadow_allocation(const ::RenderDataRD *p_render_data) {
	LocalVector<uint32_t> masks;
	const RendererSceneRender::RenderSDFGIUpdateData *feed = p_render_data != nullptr ? p_render_data->sdfgi_update_data : nullptr;
	LightStorage *light_storage = LightStorage::get_singleton();
	if (feed != nullptr && light_storage != nullptr) {
		// The lights whose shadows this frame traces: the SRC relight's shadowed lights.
		auto add = [&](RID p_light_instance, bool p_directional) {
			if (!light_storage->owns_light_instance(p_light_instance)) {
				return;
			}
			const RID light = light_storage->light_instance_get_base_light(p_light_instance);
			if (!light_storage->light_has_shadow(light) || light_storage->light_get_bake_mode(light) == RSE::LIGHT_BAKE_DISABLED) {
				return;
			}
			if (p_directional && light_storage->light_directional_get_sky_mode(light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
				return;
			}
			masks.push_back(light_storage->light_get_shadow_caster_mask(light));
		};
		if (feed->directional_lights != nullptr) {
			for (int i = 0; i < feed->directional_lights->size(); i++) {
				add(feed->directional_lights->get(i), true);
			}
		}
		for (uint32_t i = 0; i < feed->nextgi_dynamic_light_count; i++) {
			add(feed->nextgi_dynamic_light_instances[i], false);
		}
		for (uint32_t i = 0; i < feed->nextgi_static_light_count; i++) {
			add(feed->nextgi_static_light_instances[i], false);
		}
	}
	// GodotNext: nextgi_hit_lighting (#129). Reserve TLAS mask bit 1 for alpha-blended instances when hit lighting runs.
	shadow_allocator.update(masks.ptr(), masks.size(), hit_lighting_translucent_enabled ? uint32_t(NEXTGI_RT_MASK_TRANSLUCENT) : 0u);
}

void NextGIRTScene::_push_shadow_word(uint32_t p_layers, uint32_t p_alpha_base) {
	const bool alpha = p_alpha_base != UINT32_MAX;
	shadow_words.push_back((p_layers & NEXTGI_SHADOW_LAYER_MASK) | (alpha ? uint32_t(NEXTGI_SHADOW_INSTANCE_ALPHA) : 0u));
	shadow_words.push_back(alpha ? p_alpha_base : 0u);
}

uint32_t NextGIRTScene::_push_shadow_alpha(RenderGeometryInstance *p_instance, RID p_mesh, const LocalVector<NextGIRTPartGeometry> &p_geometries, uint32_t p_layers, bool p_retrace, uint32_t p_part_chunk, bool p_native_parts) {
	const RenderGeometryInstanceBase *instance = static_cast<const RenderGeometryInstanceBase *>(p_instance);
	bool alpha = false;
	for (const NextGIRTPartGeometry &geometry : p_geometries) {
		if (_nextgi_shadow_class(_nextgi_shadow_effective_material(instance, p_mesh, geometry.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
			alpha = true;
			break;
		}
	}
	if (!alpha || (p_layers & NEXTGI_SHADOW_LAYER_MASK) == 0) {
		return UINT32_MAX;
	}
	bool has_proxy_uvs = false;
	if (!p_native_parts && RSG::virtual_mesh_storage != nullptr && p_mesh.is_valid()) {
		const RenderingServerTypes::VirtualGeometryDesc *desc = RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(p_mesh);
		has_proxy_uvs = desc != nullptr && !desc->rt_proxy_uvs.is_empty();
	}
	const uint32_t base = shadow_alpha.size();
	for (const NextGIRTPartGeometry &geometry : p_geometries) {
		NextGIShadowAlphaEntry entry;
		const RID material = _nextgi_shadow_effective_material(instance, p_mesh, geometry.surface);
		entry.flags = _nextgi_shadow_class(material);
		if (entry.flags == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
			if (geometry.is_proxy_surface) {
				// GodotNext: nextgi_nextgeo_alpha_shadows (#303). VGD RT proxy parts (with rt_proxy_uvs) and
				// #156 native NextGeo parts carry their own #130 attribute stream and arena index offsets.
				const bool can_have_stream = !geometry.is_legacy_surface && (p_native_parts || has_proxy_uvs);
				const bool geom_retrace = p_retrace && can_have_stream && p_part_chunk < NEXTGI_HIT_ARENA_CHUNKS;
				_fill_shadow_alpha_entry(material, p_mesh, geometry.surface, 0, geom_retrace, entry, p_part_chunk, geometry.index_offset, true, geometry.attribute_chunk, geometry.attribute_offset, can_have_stream);
			} else {
				_fill_shadow_alpha_entry(material, p_mesh, geometry.surface, geometry.first_triangle, p_retrace, entry);
			}
		}
		shadow_alpha.push_back(entry);
	}
	return base;
}

uint32_t NextGIRTScene::_push_shadow_part(RenderGeometryInstance *p_instance, RID p_mesh, const LocalVector<NextGIRTPartGeometry> &p_geometries, uint32_t p_layers, uint32_t p_part_chunk, bool p_native_parts) {
	const uint32_t base = _push_shadow_alpha(p_instance, p_mesh, p_geometries, p_layers, true, p_part_chunk, p_native_parts);
	_push_shadow_word(p_layers, base);
	return base;
}

// GodotNext: nextgi_skinned_alpha_shadows (#302). Shadow word and optional alpha entry for a posed skinned slot.
uint32_t NextGIRTScene::_push_shadow_skinned(RenderGeometryInstance *p_instance, RID p_mesh, uint32_t p_surface, uint32_t p_slot_chunk, uint32_t p_slot_index_offset, uint32_t p_layers) {
	const RenderGeometryInstanceBase *instance = static_cast<const RenderGeometryInstanceBase *>(p_instance);
	const RID material = _nextgi_shadow_effective_material(instance, p_mesh, p_surface);
	const NextGIShadowClass shadow_class = _nextgi_shadow_class(material);
	uint32_t alpha_base = UINT32_MAX;
	if (shadow_class == NEXTGI_SHADOW_CLASS_ALPHA_TESTED && (p_layers & NEXTGI_SHADOW_LAYER_MASK) != 0) {
		NextGIShadowAlphaEntry entry;
		entry.flags = shadow_class;
		_fill_shadow_alpha_entry(material, p_mesh, p_surface, 0, p_slot_chunk < NEXTGI_HIT_ARENA_CHUNKS, entry, p_slot_chunk, p_slot_index_offset);
		alpha_base = shadow_alpha.size();
		shadow_alpha.push_back(entry);
	}
	_push_shadow_word(p_layers, alpha_base);
	return alpha_base;
}

void NextGIRTScene::_push_shadow_check_source(RenderGeometryInstance *p_instance, RID p_mesh, const LocalVector<NextGIRTPartGeometry> &p_geometries, const Transform3D &p_transform, uint32_t p_layers) {
	const RenderGeometryInstanceBase *instance = static_cast<const RenderGeometryInstanceBase *>(p_instance);
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	ShadowCheckSource check;
	check.transform = p_transform;
	check.world_bounds = mesh_storage != nullptr ? p_transform.xform(mesh_storage->mesh_get_aabb(p_mesh)) : AABB();
	check.layers = p_layers;
	check.mesh = p_mesh;
	for (const NextGIRTPartGeometry &geometry : p_geometries) {
		if (geometry.is_proxy_surface) {
			check.has_geometry = false;
			check.opaque_caster = false;
			continue;
		}
		if (_nextgi_shadow_class(_nextgi_shadow_effective_material(instance, p_mesh, geometry.surface)) != NEXTGI_SHADOW_CLASS_OPAQUE) {
			check.opaque_caster = false;
		}
		if (check.surfaces.find(geometry.surface) < 0) {
			check.surfaces.push_back(geometry.surface);
		}
	}
	shadow_check_sources.push_back(check);
}

static_assert(int(NEXTGI_SHADOW_TEXTURE_SLOTS) == int(NEXTGI_HIT_TABLE_SLOTS), "The material hit groups allocate their texture slots from the shadow rays' texture table.");

RID NextGIRTScene::ensure_shadow_texture_table() {
	RenderingDevice *rd = RD::get_singleton();
	if (shadow_texture_table.is_null() && !shadow_texture_table_failed && rd != nullptr && rd->has_feature(RD::SUPPORTS_BINDLESS)) {
		shadow_texture_table = rd->bindless_table_create(RD::UNIFORM_TYPE_TEXTURE, NEXTGI_SHADOW_TEXTURE_SLOTS);
		shadow_texture_table_failed = shadow_texture_table.is_null();
	}
	return shadow_texture_table;
}

uint64_t NextGIRTScene::append_shadow_alpha_uniforms(uint32_t p_chunk_binding, uint32_t p_sampler_binding, LocalVector<RD::Uniform> &r_uniforms) {
	RenderingDevice *rd = RD::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_COND_V(rd == nullptr || mesh_storage == nullptr || material_storage == nullptr, 0);
	if (shadow_dummy_buffer.is_null()) {
		shadow_dummy_buffer = rd->storage_buffer_create(16);
	}
	uint64_t key = hash_murmur3_one_64(shadow_dummy_buffer.get_id());
	for (uint32_t i = 0; i < NEXTGI_SHADOW_ALPHA_CHUNKS; i++) {
		RID chunk = i < mesh_storage->mesh_nextgi_rt_get_chunk_count() ? mesh_storage->mesh_nextgi_rt_get_chunk_buffer(i) : RID();
		if (chunk.is_null()) {
			chunk = shadow_dummy_buffer;
		}
		key = hash_murmur3_one_64(chunk.get_id(), key);
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, p_chunk_binding + i, chunk));
	}
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	key = hash_murmur3_one_64(sampler.get_id(), key);
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, p_sampler_binding, sampler));
	return key;
}

uint32_t NextGIRTScene::_shadow_texture_slot(RID p_rd_texture) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_rd_texture.is_null() || ensure_shadow_texture_table().is_null()) {
		return UINT32_MAX;
	}
	ShadowTextureSlot *slot = shadow_texture_slots.getptr(p_rd_texture);
	if (slot == nullptr) {
		const uint32_t allocated = rd->bindless_table_allocate(shadow_texture_table, p_rd_texture);
		if (allocated == RDBindless::INVALID_SLOT) {
			return UINT32_MAX; // The table is full: the caster shadows as opaque.
		}
		ShadowTextureSlot created;
		created.slot = allocated;
		shadow_texture_slots.insert(p_rd_texture, created);
		slot = shadow_texture_slots.getptr(p_rd_texture);
	}
	slot->frame = shadow_frame;
	return slot->slot;
}

// Slots no caster used this frame go back to the table (RenderingDevice reuses a slot only after the frame delay).
void NextGIRTScene::_release_shadow_texture_slots() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || shadow_texture_table.is_null()) {
		return;
	}
	LocalVector<RID> stale;
	for (const KeyValue<RID, ShadowTextureSlot> &entry : shadow_texture_slots) {
		if (entry.value.frame != shadow_frame) {
			stale.push_back(entry.key);
		}
	}
	for (const RID &texture : stale) {
		rd->bindless_table_release(shadow_texture_table, shadow_texture_slots[texture].slot);
		shadow_texture_slots.erase(texture);
	}
	// GodotNext: nextgi_shadow_policy (#145). Wake the hit groups' shared-table latch when shadow slots free up.
	if (!stale.is_empty()) {
		if (NextGIHitMaterials *hit_materials = NextGIHitMaterials::get_singleton()) {
			hit_materials->notify_shared_texture_slot_released();
		}
	}
}

// The bindless-texture path for BaseMaterial3D's alpha; any other alpha-tested material is left to the retrace.
void NextGIRTScene::_fill_shadow_alpha_entry(RID p_material, RID p_mesh, uint32_t p_surface, uint32_t p_first_triangle, bool p_retrace, NextGIShadowAlphaEntry &r_entry, uint32_t p_index_chunk, uint32_t p_index_offset, bool p_override_attributes, uint32_t p_attribute_chunk, uint32_t p_attribute_offset, bool p_can_have_stream) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (material_storage == nullptr || mesh_storage == nullptr || p_material.is_null()) {
		return;
	}
	using SceneShaderData = RendererSceneRenderImplementation::SceneShaderForwardClustered::ShaderData;
	MaterialStorage::ShaderData *shader_data = material_storage->material_get_shader_data(p_material);
	if (shader_data == nullptr) {
		stats.shadow_opaque_alpha_entries++;
		return;
	}
	const SceneShaderData *scene_shader = static_cast<const SceneShaderData *>(shader_data);
	// Read once per shader code: the serial changes with every code (a shader edit, or another shader).
	ShadowMaterialAlpha *known = shadow_material_alpha.getptr(p_material);
	if (known == nullptr || known->serial != scene_shader->nextgi_hit_serial) {
		ShadowMaterialAlpha read;
		read.serial = scene_shader->nextgi_hit_serial;
		read.texture_alpha = nextgi_shadow_code_has_texture_alpha(scene_shader->code, read.hash);
		read.uses_discard = scene_shader->uses_discard;
		read.sampler_flags = read.texture_alpha ? nextgi_shadow_code_albedo_sampler_flags(scene_shader->code) : 0u;
		shadow_material_alpha[p_material] = read;
		known = shadow_material_alpha.getptr(p_material);
	}
	known->frame = shadow_frame;
	// GodotNext: nextgi_shadow_dual_route (#307). A BaseMaterial3D-pattern shader that also discards keeps its
	// bindless texture-alpha entry AND sets NEXTGI_SHADOW_ALPHA_RETRACE when retrace is available and not refused,
	// so nextgi_shadow_ray_inc.glsl retraces at Full mode and falls back to the texture-alpha cut otherwise.
	const NextGI *next_gi = NextGI::get_singleton();
	const NextGIHitMode hwrt_mode = next_gi != nullptr ? NextGIHitMode(next_gi->get_hardware_ray_tracing_mode()) : NEXTGI_HIT_MODE_FULL;
	const NextGIShadowClass shadow_class = NextGIShadowClass(r_entry.flags & NEXTGI_SHADOW_ALPHA_CLASS_MASK);
	NextGIShadowAlphaRoute route = nextgi_shadow_alpha_route_for_mode(shadow_class, known->texture_alpha, known->uses_discard, hwrt_mode, p_retrace, known->serial != 0, known->verdict);
	if (route.poll_verdict) {
		const NextGIHitMaterials *hit_materials = NextGIHitMaterials::get_singleton();
		if (hit_materials != nullptr) {
			known->verdict = nextgi_shadow_retrace_verdict(known->verdict, hit_materials->get_shadow_retrace_verdict(known->serial));
		}
		route = nextgi_shadow_alpha_route_for_mode(shadow_class, known->texture_alpha, known->uses_discard, hwrt_mode, p_retrace, known->serial != 0, known->verdict);
	}
	if (route.use_retrace) {
		r_entry.flags |= NEXTGI_SHADOW_ALPHA_RETRACE;
		stats.shadow_retrace_entries++;
	}
	if (!route.use_texture_alpha) {
		if (!route.use_retrace) {
			stats.shadow_opaque_alpha_entries++;
		}
		return;
	}
	auto param = [&](const StringName &p_name) {
		const Variant value = material_storage->material_get_param(p_material, p_name);
		return value.get_type() != Variant::NIL ? value : shader_data->get_default_parameter(p_name);
	};
	const Variant threshold = param("alpha_scissor_threshold");
	const Variant albedo = param("albedo");
	const Variant uv_scale = param("uv1_scale");
	const Variant uv_offset = param("uv1_offset");
	const Variant texture = param("texture_albedo");
	r_entry.threshold = known->hash ? 0.5f : (threshold.get_type() == Variant::NIL ? 0.5f : float(threshold));
	r_entry.alpha_scale = albedo.get_type() == Variant::COLOR ? Color(albedo).a : 1.0f;
	const Vector3 scale = uv_scale.get_type() == Variant::VECTOR3 ? Vector3(uv_scale) : Vector3(1, 1, 1);
	const Vector3 offset = uv_offset.get_type() == Variant::VECTOR3 ? Vector3(uv_offset) : Vector3();
	r_entry.uv_scale[0] = scale.x;
	r_entry.uv_scale[1] = scale.y;
	r_entry.uv_offset[0] = offset.x;
	r_entry.uv_offset[1] = offset.y;
	const RID texture_rid = texture.get_type() == Variant::RID ? RID(texture) : RID();
	if (texture_rid.is_null()) {
		// No albedo texture: the alpha is the albedo color's, the same on every texel.
		if (r_entry.alpha_scale < r_entry.threshold) {
			r_entry.flags |= NEXTGI_SHADOW_ALPHA_CUT;
		}
		stats.shadow_cut_entries++;
		return;
	}
	// The bindless path reads the UV from the surface's attribute stream: NextGI keeps the streams while a caster
	// needs one. Until it can sample, the geometry shadows as opaque (unless retrace is already set).
	if (p_can_have_stream) {
		shadow_streams_wanted = true;
	}
	uint32_t attribute_chunk = UINT32_MAX;
	uint32_t attribute_offset = 0;
	uint32_t index_chunk = p_index_chunk;
	uint32_t index_offset = p_index_offset;
	if (p_override_attributes) {
		// GodotNext: nextgi_nextgeo_alpha_shadows (#303). VGD proxy and #156 native parts supply their own
		// #130 attribute stream and arena index offsets.
		attribute_chunk = p_attribute_chunk;
		attribute_offset = p_attribute_offset;
	} else {
		MeshStorage::NextGIRTSurfaceInfo info;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, p_surface, info)) {
			if (!route.use_retrace) {
				stats.shadow_opaque_alpha_entries++;
			}
			return;
		}
		attribute_chunk = info.attribute_chunk;
		attribute_offset = info.attribute_offset;
		// GodotNext: nextgi_skinned_alpha_shadows (#302). Posed skinned slots hold their indices in the slot chunk while
		// reusing the mesh surface's static attribute stream.
		if (index_chunk == UINT32_MAX) {
			index_chunk = info.chunk;
			index_offset = info.index_offset + p_first_triangle * 3 * sizeof(uint32_t);
		}
	}
	if (attribute_chunk == UINT32_MAX || index_chunk == UINT32_MAX) {
		if (!route.use_retrace) {
			stats.shadow_opaque_alpha_entries++;
		}
		return; // Opaque until the surface has its stream (unless retrace is active).
	}
	if (attribute_chunk >= NEXTGI_SHADOW_ALPHA_CHUNKS || index_chunk >= NEXTGI_SHADOW_ALPHA_CHUNKS) {
		if (!route.use_retrace) {
			stats.shadow_opaque_alpha_entries++;
		}
		return; // The shaders bind the first chunks only: opaque (unless retrace is active).
	}
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	const RID rd_texture = texture_storage != nullptr ? texture_storage->texture_get_rd_texture(texture_rid) : RID();
	const uint32_t slot = _shadow_texture_slot(rd_texture);
	if (slot == UINT32_MAX) {
		if (!route.use_retrace) {
			stats.shadow_opaque_alpha_entries++;
		}
		return;
	}
	r_entry.flags |= NEXTGI_SHADOW_ALPHA_TEXTURE | (known->hash ? uint32_t(NEXTGI_SHADOW_ALPHA_HASH) : 0u) | known->sampler_flags;
	r_entry.texture_slot = slot;
	r_entry.attribute_chunk = attribute_chunk;
	r_entry.attribute_offset = attribute_offset;
	r_entry.index_chunk = index_chunk;
	r_entry.index_offset = index_offset;
	stats.shadow_texture_entries++;
}

void NextGIRTScene::_upload_shadow_buffers() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	// Never empty: the shaders bind both buffers every frame.
	const uint32_t word_pairs = MAX(shadow_words.size() / 2, 1u);
	if (shadow_instance_buffer.is_null() || word_pairs > shadow_instance_capacity) {
		if (shadow_instance_buffer.is_valid()) {
			rd->free_rid(shadow_instance_buffer);
		}
		shadow_instance_capacity = MAX(word_pairs, shadow_instance_capacity * 2);
		shadow_instance_buffer = rd->storage_buffer_create(shadow_instance_capacity * 2 * sizeof(uint32_t));
	}
	if (!shadow_words.is_empty()) {
		rd->buffer_update(shadow_instance_buffer, 0, shadow_words.size() * sizeof(uint32_t), shadow_words.ptr());
	}
	const uint32_t entries = MAX(shadow_alpha.size(), 1u);
	if (shadow_alpha_buffer.is_null() || entries > shadow_alpha_capacity) {
		if (shadow_alpha_buffer.is_valid()) {
			rd->free_rid(shadow_alpha_buffer);
		}
		shadow_alpha_capacity = MAX(entries, shadow_alpha_capacity * 2);
		shadow_alpha_buffer = rd->storage_buffer_create(shadow_alpha_capacity * sizeof(NextGIShadowAlphaEntry));
	}
	if (!shadow_alpha.is_empty()) {
		rd->buffer_update(shadow_alpha_buffer, 0, shadow_alpha.size() * sizeof(NextGIShadowAlphaEntry), shadow_alpha.ptr());
	}
	_release_shadow_texture_slots();
	// What this phase's casters need, for the hold (the wants_*() accessors).
	if (shadow_streams_wanted) {
		shadow_streams_frame = shadow_frame;
	}
	if (stats.shadow_retrace_entries > 0) {
		shadow_retrace_frame = shadow_frame;
	}
	if (shadow_streams_wanted || stats.shadow_texture_entries > 0 || stats.shadow_retrace_entries > 0) {
		shadow_variant_frame = shadow_frame;
	}
	// Materials no TLAS phase used within the hold are forgotten.
	LocalVector<RID> stale;
	for (const KeyValue<RID, ShadowMaterialAlpha> &entry : shadow_material_alpha) {
		if (!_shadow_held(entry.value.frame)) {
			stale.push_back(entry.key);
		}
	}
	for (const RID &material : stale) {
		shadow_material_alpha.erase(material);
	}
}

void NextGIRTScene::_check_shadow_word(uint32_t p_custom_index_end) {
	if (shadow_words.size() != 2 * p_custom_index_end) {
		shadow_word_misaligned++;
		ERR_PRINT_ONCE("NextGI: the shadow instance words are out of step with the TLAS custom indices: shadow rays would read another instance's layers.");
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
	// GodotNext: nextgi_shadow_policy (#145). The surfaces #61 keeps out of the parts get shadow parts.
	LocalVector<uint32_t> shadow_triangle_counts;
	LocalVector<uint32_t> shadow_chunks;
	shadow_triangle_counts.resize(surface_count);
	shadow_chunks.resize(surface_count);
	for (uint32_t i = 0; i < surface_count; i++) {
		triangle_counts[i] = 0;
		chunks[i] = UINT32_MAX;
		shadow_triangle_counts[i] = 0;
		shadow_chunks[i] = UINT32_MAX;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, i, surface_infos[i]) || !surface_infos[i].eligible) {
			continue;
		}
		if (_nextgi_rt_material_is_alpha(surface_infos[i].material)) {
			stats.coverage.alpha_excluded++;
			shadow_triangle_counts[i] = surface_infos[i].index_count / 3;
			shadow_chunks[i] = surface_infos[i].chunk;
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

	// GodotNext: nextgi_shadow_policy (#145). Shadow parts: the same geometry layout, opaque geometry (the TLAS
	// instance flags decide what is a candidate), not compacted.
	LocalVector<NextGIRTPartPlan> shadow_plans;
	nextgi_rt_split_parts(shadow_triangle_counts.ptr(), shadow_chunks.ptr(), surface_count, NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, shadow_plans);
	LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(p_mesh);
	ERR_FAIL_NULL(shadow_parts);
	for (const NextGIRTPartPlan &plan : shadow_plans) {
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
		part.chunk = plan.chunk;
		BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
		if (dynamic) {
			flags = flags | RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		}
		part.blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), flags);
		if (part.blas.is_valid()) {
			RD::get_singleton()->blas_build(part.blas);
			part.uncompacted_bytes = RD::get_singleton()->acceleration_structure_get_size_bytes(part.blas);
			stats.blas_build_count++;
			stats.blas_build_triangles += plan.triangle_count;
		}
		shadow_parts->push_back(part);
	}
	mesh_storage->mesh_nextgi_rt_set_allows_update(p_mesh, dynamic);
	mesh_storage->mesh_nextgi_rt_set_blas_dirty(p_mesh, false);
}

void NextGIRTScene::_build_proxy_part(RID p_mesh, uint32_t p_part_index, bool p_is_shadow_part) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh) || RD::get_singleton() == nullptr) {
		return;
	}
	LocalVector<MeshStorage::NextGIRTPart> *parts = p_is_shadow_part ? mesh_storage->mesh_nextgi_rt_get_shadow_parts(p_mesh) : mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
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

	BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
	if (!p_is_shadow_part) {
		flags = flags | RD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT;
	}
	part.blas = RD::get_singleton()->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), flags);
	if (part.blas.is_valid()) {
		RD::get_singleton()->blas_build(part.blas);
		part.uncompacted_bytes = RD::get_singleton()->acceleration_structure_get_size_bytes(part.blas);
		part.compacted = false;
		stats.blas_build_count++;
		stats.blas_build_triangles += part.triangle_count;
		if (!p_is_shadow_part) {
			PendingCompact pending;
			pending.blas = part.blas;
			pending.mesh = p_mesh;
			pending.part_index = p_part_index;
			pending.triangles = part.triangle_count;
			pending_compactions.push_back(pending);
		}
	}

	bool all_built = true;
	if (const LocalVector<MeshStorage::NextGIRTPart> *gi_parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh)) {
		for (const MeshStorage::NextGIRTPart &check : *gi_parts) {
			if (!check.geometries.is_empty() && check.blas.is_null()) {
				all_built = false;
				break;
			}
		}
	}
	if (all_built) {
		if (const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(p_mesh)) {
			for (const MeshStorage::NextGIRTPart &check : *shadow_parts) {
				if (!check.geometries.is_empty() && check.blas.is_null()) {
					all_built = false;
					break;
				}
			}
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

// GodotNext: nextgi_src_page_levels (#247). The record waits unpaged until the pass's assignment.
void NextGIRTScene::_queue_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element) {
	if (NextGIRelight::get_singleton() == nullptr || p_instance == nullptr || r_record.card_set == UINT32_MAX) {
		if (r_record.card_set == UINT32_MAX) {
			r_record.page_table = UINT32_MAX;
		}
		return;
	}
	r_record.page_table = 0x80000000u;
	PendingPages pending;
	pending.instance = p_instance;
	pending.record = r_record.tlas_index;
	pending.element = p_element;
	pending_pages.push_back(pending);
}

// Each record's solid angle (NextGIRelight::record_solid_angle()) is seen from the nearest of the
// frame's NextGI view cameras (NextGI::get_page_cameras()), so a second view's surroundings keep
// their pages. Every existing set is touched first; then the records are assigned from the
// largest solid angle down.
void NextGIRTScene::_assign_pending_pages(const ::RenderDataRD *p_render_data, LocalVector<NextGIInstanceRecord> &r_records) {
	NextGIRelight *relight = NextGIRelight::get_singleton();
	if (relight == nullptr || pending_pages.is_empty()) {
		pending_pages.clear();
		return;
	}
	LocalVector<Vector3> cameras;
	if (NextGI::get_singleton() != nullptr) {
		NextGI::get_singleton()->get_page_cameras(cameras);
	}
	if (cameras.is_empty() && p_render_data != nullptr && p_render_data->scene_data != nullptr) {
		cameras.push_back(p_render_data->scene_data->cam_transform.origin);
	}
	HashMap<uint32_t, float> extents;
	for (PendingPages &pending : pending_pages) {
		if (pending.record >= r_records.size()) {
			pending.solid_angle = 0.0f;
			continue;
		}
		const NextGIInstanceRecord &record = r_records[pending.record];
		const float *extent = extents.getptr(record.card_set);
		if (extent == nullptr) {
			extent = &extents.insert(record.card_set, NextGIRelight::card_set_extent(record.card_set))->value;
		}
		pending.solid_angle = NextGIRelight::record_solid_angle(record, *extent, cameras);
	}
	for (const PendingPages &pending : pending_pages) {
		if (pending.record < r_records.size()) {
			relight->touch_pages(pending.instance, pending.element, r_records[pending.record].card_set, pending.solid_angle);
		}
	}
	struct ByDescendingSolidAngle {
		bool operator()(const PendingPages &p_a, const PendingPages &p_b) const {
			return p_a.solid_angle != p_b.solid_angle ? p_a.solid_angle > p_b.solid_angle : p_a.record < p_b.record;
		}
	};
	pending_pages.sort_custom<ByDescendingSolidAngle>();
	for (const PendingPages &pending : pending_pages) {
		if (pending.record < r_records.size()) {
			NextGIInstanceRecord &record = r_records[pending.record];
			record.page_table = relight->assign_pages(pending.instance, record, pending.element, pending.solid_angle);
		}
	}
	pending_pages.clear();
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
	_queue_pages(p_instance, record, UINT32_MAX); // GodotNext: nextgi_src_page_levels (#247)
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

// GodotNext: path_tracer (#167). The surface sources line up with the records (the far proxies, appended last,
// have none), and the hash covers what the path tracer's history depends on.
void NextGIRTScene::_finish_surface_sources(const LocalVector<NextGIInstanceRecord> &p_records, const LocalVector<NextGISurfaceRecord> &p_surfaces) {
	// Only while a path tracer wants them: the default path does not pay for the walk.
	if (!surface_sources_wanted) {
		return;
	}
	if (surface_sources.size() > p_surfaces.size()) {
		surface_sources.resize(p_surfaces.size());
	}
	while (surface_sources.size() < p_surfaces.size()) {
		surface_sources.push_back(SurfaceSource());
	}
	uint64_t hash = 1469598103934665603ull;
	auto mix = [&hash](const void *p_data, size_t p_size) {
		const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
		for (size_t i = 0; i < p_size; i++) {
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		}
	};
	for (const NextGIInstanceRecord &record : p_records) {
		mix(record.object_to_world, sizeof(record.object_to_world));
		const uint32_t words[3] = { record.surface_base, record.surface_count, record.arena_chunk };
		mix(words, sizeof(words));
	}
	for (const NextGISurfaceRecord &surface : p_surfaces) {
		const uint32_t words[3] = { surface.vertex_offset, surface.index_offset, surface.triangle_count };
		mix(words, sizeof(words));
	}
	records_hash = hash;
	surface_sources_valid = true;
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
	// GodotNext: nextgi_shadow_policy (#145)
	stats.shadow_only_instances = 0;
	stats.shadow_alpha_instances = 0;
	stats.shadow_unallocated_instances = 0;
	shadow_words.clear();
	shadow_alpha.clear();
	shadow_alpha_bits = 0;
	shadow_check_sources.clear();
	stats.shadow_texture_entries = 0;
	stats.shadow_cut_entries = 0;
	stats.shadow_retrace_entries = 0;
	stats.shadow_opaque_alpha_entries = 0;
	stats.gpu_written_instances = 0;
	shadow_streams_wanted = false;
	shadow_frame++;
	tlas_traceable = false;
	compaction_triangles_this_frame = 0;
	// GodotNext: ps_lumen_lite (#183). New instance records: relight pages claim them again.
	if (NextGIRelight::get_singleton() != nullptr) {
		NextGIRelight::get_singleton()->begin_record_pass();
	}
	pending_pages.clear(); // GodotNext: nextgi_src_page_levels (#247)
	// GodotNext: path_tracer (#167). This frame's sources come from this phase, or none.
	surface_sources.clear();
	surface_sources_valid = false;
	records_hash = 0;
	ERR_FAIL_NULL(p_render_data);
	if (tlas.is_null()) {
		return;
	}
	// GodotNext: nextgeo_rt_native (#156). Native Nanite RT exists only while the mode is native
	// (EXPERIMENTAL); the fallback mode, the default, creates nothing and takes no new path. Its
	// frame start runs before the early returns, so a switch back or the last instance's removal
	// releases the native sets even when no instance is fed.
	const bool native_mode = nextgeo_rt_mode_from_setting(GLOBAL_GET_CACHED(int, "rendering/nextgeo/ray_tracing_mode")) == NEXTGEO_RT_MODE_NATIVE;
	if (native_mode && native == nullptr) {
		native = memnew(NextGIRTNative);
	} else if (!native_mode && native != nullptr) {
		native->free();
		memdelete(native);
		native = nullptr;
	}
	if (native != nullptr) {
		native->begin_frame(p_render_data, RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0);
	}
	// GodotNext: nextgi_hit_groups (#130). While a consumer needs the hit groups, every surface record gets a hit
	// record at the same index and every TLAS instance a hit shader binding table range; otherwise nothing
	// changes and the ranges stay 0.
	NextGIHitMaterials *hit_materials = NextGIHitMaterials::get_singleton();
	const bool hit_records = hit_materials != nullptr && hit_materials->is_ready();
	if (hit_records) {
		hit_materials->begin_records();
	}
	// GodotNext: nextgi_shadow_policy (#145). The caster masks of this frame's shadowed lights decide the TLAS masks.
	_update_shadow_allocation(p_render_data);
	const NextGIShadowMaskAllocator::Allocation &shadow_allocation = shadow_allocator.get_allocation();
	const LocalVector<RendererSceneRender::NextGIRTInstance> *fed_instances = _nextgi_rt_fed_instances(p_render_data);
	if (fed_instances == nullptr || fed_instances->is_empty()) {
		_upload_shadow_buffers(); // GodotNext: nextgi_shadow_policy (#145)
		LocalVector<NextGIInstanceRecord> records;
		LocalVector<NextGISurfaceRecord> surfaces;
		_update_far_records(p_render_data, records, surfaces);
		_assign_pending_pages(p_render_data, records); // GodotNext: nextgi_src_page_levels (#247)
		// GodotNext: ps_lumen_lite (#183)
		if (NextGIRelight::get_singleton() != nullptr) {
			NextGIRelight::get_singleton()->finish_record_pass(records);
		}
		// GodotNext: nextgi_hit_groups (#130)
		if (hit_records) {
			hit_materials->finish_records(surfaces.size());
		}
		_finish_surface_sources(records, surfaces); // GodotNext: path_tracer (#167)
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
	// GodotNext: nextgi_hit_lighting (#129).
	translucent_instance_count = 0;

	// GodotNext: nextgi_shadow_policy (#145)
	struct DeferredShadowParts {
		RenderGeometryInstanceBase *instance = nullptr;
		RID mesh;
		Transform3D transform;
		uint32_t layers = 0;
		float max_scale = 1.0f;
		uint32_t element_index = UINT32_MAX;
		bool multimesh_element = false;
	};
	LocalVector<DeferredShadowParts> deferred_shadow_parts;
	// GodotNext: nextgi_skinned_alpha_shadows (#302). Shadow-only skinned surfaces (mesh material alpha-tested or
	// blended, excluded from GI per D17) join after every GI record, alongside deferred_shadow_parts.
	struct DeferredSkinnedShadowPart {
		RenderGeometryInstanceBase *instance = nullptr;
		RID mesh;
		uint32_t surface = 0;
		Transform3D transform;
		uint32_t layers = 0;
		float max_scale = 1.0f;
		float eps = 0.0f;
		MeshStorage::NextGIRTSlotInfo slot;
		RID blas;
		bool multimesh_element = false;
	};
	LocalVector<DeferredSkinnedShadowPart> deferred_skinned_shadow_parts;
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
		// GodotNext: nextgi_skinned_alpha_shadows (#302). Alpha-tested and translucent skinned
		// surfaces follow D17 / #145: shadow-only when excluded from GI by their mesh material,
		// non-opaque with alpha table entries and #130 hit groups when alpha-tested.
		const RID skinned_instance = geometry_instance->mesh_instance;
		if (skinned_instance.is_valid()) {
			struct SkinnedSlotEntry {
				uint32_t surface = 0;
				MeshStorage::NextGIRTSlotInfo slot;
				MeshStorage::NextGIRTSurfaceInfo info;
				NextGISkinnedShadowPolicy policy;
			};
			LocalVector<SkinnedSlotEntry> active_slots;
			uint32_t skinned_triangles = 0;
			uint32_t skinned_alpha_excluded = 0;
			const uint32_t skinned_surface_count = mesh_storage->mesh_get_surface_count(mesh);
			for (uint32_t surface_index = 0; surface_index < skinned_surface_count; surface_index++) {
				MeshStorage::NextGIRTSlotInfo slot;
				if (!mesh_storage->mesh_instance_get_rt_slot(skinned_instance, surface_index, slot)) {
					continue;
				}
				MeshStorage::NextGIRTSurfaceInfo info;
				if (!mesh_storage->mesh_nextgi_rt_get_surface_info(mesh, surface_index, info)) {
					continue;
				}
				const RID effective_material = _nextgi_shadow_effective_material(geometry_instance, mesh, surface_index);
				const NextGISkinnedShadowPolicy policy = nextgi_shadow_skinned_policy(_nextgi_rt_material_is_alpha(info.material), _nextgi_shadow_class(effective_material), fed.shadow_layers);
				if (!policy.gi_visible) {
					skinned_alpha_excluded++;
				}
				if (policy.skip) {
					continue;
				}
				SkinnedSlotEntry entry;
				entry.surface = surface_index;
				entry.slot = slot;
				entry.info = info;
				entry.policy = policy;
				active_slots.push_back(entry);
				skinned_triangles += slot.index_count / 3;
			}
			if (skinned_alpha_excluded > 0) {
				stats.coverage.alpha_excluded += skinned_alpha_excluded;
			}
			if (active_slots.is_empty()) {
				if (skinned_alpha_excluded == 0) {
					stats.coverage.dynamic_excluded++;
				}
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
			bool skinned_gi_entered = false;
			bool skinned_shadow_entered = false;
			if (build != NEXTGI_SKINNED_SKIP) {
				for (uint32_t k = 0; k < active_slots.size(); k++) {
					const SkinnedSlotEntry &slot_entry = active_slots[k];
					const MeshStorage::NextGIRTSlotInfo &slot = slot_entry.slot;
					const MeshStorage::NextGIRTSurfaceInfo &info = slot_entry.info;
					const uint64_t key = (uint64_t(skinned_instance.get_id()) << 32) | slot_entry.surface;
					SkinnedPart *part = skinned_parts.getptr(key);
					if (part != nullptr && !_skinned_part_blas_alive(*part)) {
						skinned_parts.erase(key);
						part = nullptr;
					}
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
						created.chunk_buffer = chunk_buffer;
						created.chunk = slot.chunk;
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
					if (slot_entry.policy.shadow_only) {
						DeferredSkinnedShadowPart deferred;
						deferred.instance = geometry_instance;
						deferred.mesh = mesh;
						deferred.surface = slot_entry.surface;
						deferred.transform = instance_transform;
						deferred.layers = fed.shadow_layers;
						deferred.max_scale = max_scale;
						deferred.eps = info.eps;
						deferred.slot = slot;
						deferred.blas = part->blas;
						deferred.multimesh_element = fed.element_index != UINT32_MAX;
						deferred_skinned_shadow_parts.push_back(deferred);
						skinned_shadow_entered = true;
						continue;
					}
					if (instance_records.size() >= NEXTGI_RT_TLAS_CAPACITY || instance_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
						break;
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
					if (skinned_card_set != UINT32_MAX) {
						_queue_pages(geometry_instance, record, fed.element_index); // GodotNext: nextgi_src_page_levels (#247)
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
					// GodotNext: path_tracer (#167)
					if (surface_sources_wanted) {
						surface_sources.push_back({ geometry_instance, mesh, slot_entry.surface });
					}
					instance_records.push_back(record);
					// GodotNext: nextgi_skinned_alpha_shadows (#302). GI sees the posed slot as before (with forced
					// opacity on GI rays); shadow rays follow the effective material (alpha-tested instance overrides
					// become FORCE_NO_OPAQUE with alpha entries and #130 hit groups, while opaque slots stay untouched).
					const uint32_t shadow_alpha_base = _push_shadow_skinned(geometry_instance, mesh, slot_entry.surface, slot.chunk, slot.index_offset, fed.shadow_layers);
					if (hit_records) {
						if (shadow_alpha_base != UINT32_MAX) {
							hit_materials->add_geometry(geometry_instance, mesh, slot_entry.surface, slot.chunk, slot.attribute_chunk != UINT32_MAX, slot.attribute_chunk, slot.attribute_offset, fed.element_index != UINT32_MAX);
						} else {
							hit_materials->add_fallback_surfaces(1);
						}
					}
					RD::AccelerationStructureInstance tlas_instance;
					tlas_instance.transform = instance_transform;
					tlas_instance.id = record.tlas_index;
					tlas_instance.mask = nextgi_shadow_instance_mask(shadow_allocation, fed.shadow_layers, true);
					stats.shadow_unallocated_instances += (tlas_instance.mask & NEXTGI_SHADOW_MASK_UNALLOCATED) != 0 ? 1 : 0;
					if (shadow_alpha_base != UINT32_MAX) {
						shadow_alpha_bits |= tlas_instance.mask;
						stats.shadow_alpha_instances++;
					}
					if (shadow_check_sources_enabled) {
						ShadowCheckSource check;
						check.transform = instance_transform;
						check.world_bounds = geometry_instance->transformed_aabb;
						check.layers = fed.shadow_layers;
						check.opaque_caster = false;
						check.has_geometry = false;
						shadow_check_sources.push_back(check);
					}
					tlas_instance.hit_sbt_range = hit_records ? hit_materials->end_instance(record.tlas_index, UINT32_MAX, false) : 0;
					tlas_instance.flags = shadow_alpha_base != UINT32_MAX ? RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
					tlas_instance.blas = part->blas;
					_check_shadow_word(tlas_instance.id + 1); // GodotNext: nextgi_shadow_policy (#145)
					tlas_instances.push_back(tlas_instance);
					skinned_gi_entered = true;
				}
			}
			if (skinned_gi_entered) {
				stats.skinned_instances++;
				stats.coverage.numerator++;
				if (geometry_instance->nextgi_record_index == UINT32_MAX && first_record < instance_records.size()) {
					geometry_instance->nextgi_record_index = first_record;
				}
			} else if (skinned_shadow_entered) {
				stats.skinned_instances++;
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
			// GodotNext: nextgi_shadow_policy (#145). The shadow parts refit with them.
			if (const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(mesh)) {
				for (const MeshStorage::NextGIRTPart &part : *shadow_parts) {
					triangles += part.triangle_count;
				}
			}
			if (nextgi_rt_refit_or_exclude(stats.refit_triangles, triangles, NEXTGI_RT_REFIT_TRIANGLE_CAP) != NEXTGI_RT_INCLUDED) {
				continue;
			}
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (part.blas.is_valid()) {
					RD::get_singleton()->blas_build(part.blas, RD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE);
				}
			}
			// GodotNext: nextgi_shadow_policy (#145)
			if (const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(mesh)) {
				for (const MeshStorage::NextGIRTPart &part : *shadow_parts) {
					if (part.blas.is_valid()) {
						RD::get_singleton()->blas_build(part.blas, RD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE);
					}
				}
			}
			mesh_storage->mesh_nextgi_rt_set_blas_dirty(mesh, false);
			stats.refit_triangles += triangles;
			blas_dirty = false;
		}

		const Transform3D transform = fed.has_rt_transform ? fed.rt_transform : geometry_instance->transform;
		// GodotNext: nextgeo_rt_native (#156). In native mode a NextGeo instance traces the BLAS of
		// its (mesh, distance band) bucket, its previous native BLAS, or else its proxy parts.
		const LocalVector<MeshStorage::NextGIRTPart> *tlas_parts = parts;
		bool native_parts = false;
		float native_eps = 0.0f;
		if (native != nullptr && !parts->is_empty() && (*parts)[0].is_proxy) {
			const NextGeoRTNativeExclusion exclusion = nextgeo_rt_native_exclusion(geometry_instance->mesh_instance.is_valid(), _nextgeo_rt_native_vertex_offset(geometry_instance, mesh), _nextgeo_rt_native_legacy_surfaces(mesh, *parts));
			const uint64_t instance_key = (uint64_t(uintptr_t(geometry_instance)) << 8u) ^ uint64_t(fed.element_index);
			const LocalVector<MeshStorage::NextGIRTPart> *selected = native->select(mesh, instance_key, transform.xform(mesh_storage->mesh_get_aabb(mesh)), transform, camera_origin, exclusion, native_eps);
			if (selected != nullptr) {
				tlas_parts = selected;
				native_parts = true;
			}
		}
		bool entered_tlas = false;
		uint32_t first_rt_fields[6] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX };
		const bool has_vgd_cards = RSG::virtual_mesh_storage != nullptr && RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(mesh) != nullptr && !RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(mesh)->card_descriptors.is_empty();
		const Vector3 scale = transform.basis.get_scale_abs();
		const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
		for (const MeshStorage::NextGIRTPart &part : *tlas_parts) {
			// GodotNext: nextgeo_rt_native (#156). A built native set is complete even while the
			// proxy is dirty.
			if (!nextgi_rt_part_enters_tlas(native_parts ? false : blas_dirty, part.blas.is_valid()) || part.geometries.is_empty()) {
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

			// GodotNext: nextgi_nextgeo_alpha_shadows (#303). Request #130 attribute streams lazily only when
			// a proxy/native NextGeo part actually carries an alpha-tested effective material; opaque NextGeo
			// proxies keep zero attribute stream overhead and use fallback hit records as before.
			bool part_has_alpha_proxy = false;
			if (part.is_proxy && (fed.shadow_layers & NEXTGI_SHADOW_LAYER_MASK) != 0) {
				for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
					if (part_geometry.is_proxy_surface && !part_geometry.is_legacy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(geometry_instance, mesh, part_geometry.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
						part_has_alpha_proxy = true;
						break;
					}
				}
			}
			if (part_has_alpha_proxy) {
				if (native != nullptr) {
					native->request_mesh_attributes(mesh);
				}
				if (!native_parts) {
					mesh_storage->mesh_nextgi_rt_request_proxy_attributes(mesh);
				}
			}

			float eps_rt = 0.0f;
			uint32_t first_vertex_offset = 0;
			bool have_first_vertex_offset = false;
			for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
				NextGISurfaceRecord surface_record;
				// GodotNext: nextgi_hit_groups (#130)
				uint32_t hit_attribute_chunk = UINT32_MAX;
				uint32_t hit_attribute_offset = 0;
				uint32_t hit_arena_chunk = UINT32_MAX;
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
					hit_attribute_chunk = part_geometry.attribute_chunk;
					hit_attribute_offset = part_geometry.attribute_offset;
					hit_arena_chunk = part.chunk;
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
					// GodotNext: nextgi_hit_groups (#130)
					hit_attribute_chunk = info.attribute_chunk;
					hit_attribute_offset = info.attribute_offset;
					hit_arena_chunk = info.chunk;
				}
				if (!have_first_vertex_offset) {
					first_vertex_offset = surface_record.vertex_offset;
					have_first_vertex_offset = true;
				}
				surface_records.push_back(surface_record);
				// GodotNext: path_tracer (#167)
				if (surface_sources_wanted) {
					surface_sources.push_back({ geometry_instance, mesh, part_geometry.surface });
				}
				// GodotNext: nextgi_hit_groups (#130) / nextgi_nextgeo_alpha_shadows (#303). Bind material hit groups
				// on alpha-tested NextGeo geometries that carry an attribute stream; opaque proxies keep fallback.
				if (hit_records) {
					if (part_geometry.is_proxy_surface) {
						const bool proxy_alpha = part_has_alpha_proxy && !part_geometry.is_legacy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(geometry_instance, mesh, part_geometry.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
						if (proxy_alpha && hit_attribute_chunk != UINT32_MAX) {
							hit_materials->add_geometry(geometry_instance, mesh, part_geometry.surface, hit_arena_chunk, true, hit_attribute_chunk, hit_attribute_offset, fed.element_index != UINT32_MAX);
						} else {
							hit_materials->add_fallback_surfaces(1);
						}
					} else {
						hit_materials->add_geometry(geometry_instance, mesh, part_geometry.surface, hit_arena_chunk, hit_attribute_chunk != UINT32_MAX, hit_attribute_chunk, hit_attribute_offset, fed.element_index != UINT32_MAX);
					}
				}
			}
			// GodotNext: nextgi_hit_lighting (#129). When hit lighting is active, an instance whose effective
			// material override turns every geometry in this part translucent enters the TLAS on mask bit 1
			// (NEXTGI_RT_MASK_TRANSLUCENT) instead of the opaque GI bit 7 so diffuse GI ignores it while
			// hit-lighting reflection rays can skip and tint through it.
			bool override_translucent = false;
			if (hit_lighting_translucent_enabled && !part.geometries.is_empty()) {
				override_translucent = true;
				for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
					if (part_geometry.is_proxy_surface || _nextgi_shadow_class(_nextgi_shadow_effective_material(geometry_instance, mesh, part_geometry.surface)) != NEXTGI_SHADOW_CLASS_TRANSLUCENT) {
						override_translucent = false;
						break;
					}
				}
			}
			if (override_translucent) {
				record.rt_flags |= NEXTGI_RT_FLAG_TRANSLUCENT;
				translucent_instance_count++;
			}
			// GodotNext: nextgeo_rt_native (#156). A native set's eps is its bucket's (select()).
			record.origin_offset = nextgi_rt_origin_offset(native_parts ? native_eps : eps_rt, max_scale);
			_queue_pages(geometry_instance, record, fed.element_index); // GodotNext: nextgi_src_page_levels (#247)
			instance_records.push_back(record);
			if (RSG::rasterizer != nullptr) {
				mesh_last_tlas_frame[mesh] = RSG::rasterizer->get_frame_number();
			}

			RD::AccelerationStructureInstance tlas_instance;
			tlas_instance.transform = transform;
			tlas_instance.id = record_index;
			// GodotNext: nextgi_shadow_policy (#145). GI sees the part as before. For shadow rays its geometries follow
			// the instance's materials: with an alpha-tested one the instance is non-opaque (GI rays force opacity), and
			// its geometries' alpha table entries tell the candidate loop which to test.
			const uint32_t shadow_alpha_base = _push_shadow_part(geometry_instance, mesh, part.geometries, fed.shadow_layers, part.chunk, native_parts);
			tlas_instance.mask = override_translucent
					? (nextgi_shadow_instance_mask(shadow_allocation, fed.shadow_layers, false) | uint32_t(NEXTGI_RT_MASK_TRANSLUCENT))
					: nextgi_shadow_instance_mask(shadow_allocation, fed.shadow_layers, true);
			stats.shadow_unallocated_instances += (tlas_instance.mask & NEXTGI_SHADOW_MASK_UNALLOCATED) != 0 ? 1 : 0;
			if (shadow_alpha_base != UINT32_MAX) {
				shadow_alpha_bits |= tlas_instance.mask;
				stats.shadow_alpha_instances++;
			}
			if (shadow_check_sources_enabled) {
				_push_shadow_check_source(geometry_instance, mesh, part.geometries, transform, fed.shadow_layers);
			}
			// GodotNext: nextgi_hit_groups (#130). A MultiMesh element shares the MultiMesh's card set: the check skips it.
			tlas_instance.hit_sbt_range = hit_records ? hit_materials->end_instance(record_index, record.card_set, fed.element_index == UINT32_MAX) : 0;
			tlas_instance.flags = shadow_alpha_base != UINT32_MAX ? RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
			tlas_instance.blas = part.blas;
			_check_shadow_word(tlas_instance.id + 1); // GodotNext: nextgi_shadow_policy (#145)
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
		// GodotNext: nextgi_shadow_policy (#145), nextgi_hit_lighting (#129). The instance's shadow parts join after
		// every GI record, so the GI records keep their indices. When hit lighting is active, translucent shadow parts
		// also enter even when they cast no shadows (mask bit 1, NEXTGI_RT_MASK_TRANSLUCENT).
		if (!blas_dirty) {
			const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(mesh);
			if (shadow_parts != nullptr && !shadow_parts->is_empty()) {
				bool admit_shadow_parts = (fed.shadow_layers & NEXTGI_SHADOW_LAYER_MASK) != 0;
				if (!admit_shadow_parts && hit_lighting_translucent_enabled) {
					for (const MeshStorage::NextGIRTPart &part : *shadow_parts) {
						for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
							if (!part_geometry.is_proxy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(geometry_instance, mesh, part_geometry.surface)) == NEXTGI_SHADOW_CLASS_TRANSLUCENT) {
								admit_shadow_parts = true;
								break;
							}
						}
						if (admit_shadow_parts) {
							break;
						}
					}
				}
				if (admit_shadow_parts) {
					DeferredShadowParts deferred;
					deferred.instance = geometry_instance;
					deferred.mesh = mesh;
					deferred.transform = transform;
					deferred.layers = fed.shadow_layers;
					deferred.max_scale = max_scale;
					deferred.element_index = fed.element_index;
					deferred.multimesh_element = fed.element_index != UINT32_MAX;
					deferred_shadow_parts.push_back(deferred);
				}
			}
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
		// GodotNext: nextgi_shadow_policy (#145). blas is the GI part's (invalid when the mesh has none: all its
		// surfaces are alpha-tested or blended); its geometries and the mesh's shadow parts get alpha entries.
		uint32_t shadow_layers = 0;
		AABB bounds;
		RenderGeometryInstance *instance = nullptr;
		RID mesh;
		uint32_t part_index = UINT32_MAX;
		bool shadow_parts = false;
	};
	const Vector3 camera_pos = p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	// GodotNext: nextgi_shadow_policy (#145). Only GI instances are in tlas_instances yet: shadow-only ones join after.
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
		uint32_t part_index = UINT32_MAX;
		for (uint32_t p = 0; p < parts->size(); p++) {
			if ((*parts)[p].blas.is_valid() && !(*parts)[p].geometries.is_empty()) {
				blas = (*parts)[p].blas;
				part_index = p;
				break;
			}
		}
		// GodotNext: nextgi_shadow_policy (#145). The elements also cast through the mesh's shadow parts.
		bool shadow_parts = false;
		if ((fed.shadow_layers & NEXTGI_SHADOW_LAYER_MASK) != 0) {
			if (const LocalVector<MeshStorage::NextGIRTPart> *list = mesh_storage->mesh_nextgi_rt_get_shadow_parts(mesh)) {
				for (const MeshStorage::NextGIRTPart &part : *list) {
					shadow_parts = shadow_parts || (part.blas.is_valid() && !part.geometries.is_empty());
				}
			}
		}
		if (blas.is_null()) {
			stats.coverage.blas_pending += fed.gpu_count;
			if (!shadow_parts) {
				continue;
			}
		}

		GpuSource source;
		source.source = fed.gpu_source;
		source.blas = blas;
		source.part_index = part_index;
		source.shadow_parts = shadow_parts;
		source.mesh = mesh;
		source.instance = fed.geometry_instance;
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
		// GodotNext: nextgi_shadow_policy (#145)
		source.shadow_layers = fed.shadow_layers;
		if (fed.geometry_instance != nullptr) {
			source.bounds = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance)->transformed_aabb;
		}
		gpu_candidates.push_back(source);
	}
	std::sort(gpu_candidates.ptr(), gpu_candidates.ptr() + gpu_candidates.size(), [](const GpuSource &p_a, const GpuSource &p_b) { return p_a.distance > p_b.distance; });

	LocalVector<GpuSource> gpu_sources;
	// GodotNext: nextgi_shadow_policy (#145). Sources whose mesh has shadow parts: encoded last, with what is left.
	LocalVector<GpuSource> gpu_shadow_sources;
	for (const GpuSource &source : gpu_candidates) {
		if (source.shadow_parts) {
			gpu_shadow_sources.push_back(source);
		}
		if (source.blas.is_null()) {
			continue;
		}
		if (source.count > dynamic_budget) {
			stats.coverage.instance_cap += source.count;
			continue;
		}
		dynamic_budget -= source.count;
		gpu_sources.push_back(source);
	}

	// GodotNext: nextgi_shadow_policy (#145). Shadow parts: TLAS instances only shadow rays see (no GI bit, D17).
	// They come after the GI instances and take only the capacity the GPU-written GI instances leave, so a
	// shadow-only surface never pushes an instance GI sees out of the TLAS.
	uint32_t gpu_reserved = 0;
	for (const GpuSource &source : gpu_sources) {
		gpu_reserved += source.count;
	}
	for (const DeferredShadowParts &deferred : deferred_shadow_parts) {
		const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(deferred.mesh);
		if (shadow_parts == nullptr) {
			continue;
		}
		for (const MeshStorage::NextGIRTPart &part : *shadow_parts) {
			if (!part.blas.is_valid() || part.geometries.is_empty()) {
				continue;
			}
			// GodotNext: nextgi_hit_lighting (#129). Alpha-blended surfaces admitted with mask bit 1 (no GI bit 7).
			bool has_translucent = false;
			if (hit_lighting_translucent_enabled) {
				for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
					if (!part_geometry.is_proxy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(deferred.instance, deferred.mesh, part_geometry.surface)) == NEXTGI_SHADOW_CLASS_TRANSLUCENT) {
						has_translucent = true;
						break;
					}
				}
			}
			uint32_t mask = nextgi_shadow_instance_mask(shadow_allocation, deferred.layers, false);
			if (has_translucent) {
				mask |= uint32_t(NEXTGI_RT_MASK_TRANSLUCENT);
			}
			if (mask == 0 || instance_records.size() + gpu_reserved >= NEXTGI_RT_TLAS_CAPACITY || instance_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
				continue;
			}
			const uint32_t record_index = instance_records.size();
			NextGIInstanceRecord record;
			for (int i = 0; i < 3; i++) {
				record.object_to_world[i * 4 + 0] = deferred.transform.basis.rows[i][0];
				record.object_to_world[i * 4 + 1] = deferred.transform.basis.rows[i][1];
				record.object_to_world[i * 4 + 2] = deferred.transform.basis.rows[i][2];
			}
			record.object_to_world[3] = deferred.transform.origin.x;
			record.object_to_world[7] = deferred.transform.origin.y;
			record.object_to_world[11] = deferred.transform.origin.z;
			record.surface_base = surface_records.size();
			record.surface_count = part.geometries.size();
			record.rt_flags = NEXTGI_RT_FLAG_STATIC | (has_translucent ? uint32_t(NEXTGI_RT_FLAG_TRANSLUCENT) : 0u);
			record.tlas_index = record_index;
			// No card set and no relight pages unless admitted for hit-lighting translucency: opaque GI never hits it.
			// GodotNext: nextgi_nextgeo_alpha_shadows (#303). Ensure proxy shadow parts carry #130 attribute
			// streams when an alpha-tested NextGeo surface needs them.
			bool shadow_proxy_has_alpha = false;
			if (part.is_proxy) {
				for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
					if (part_geometry.is_proxy_surface && !part_geometry.is_legacy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(deferred.instance, deferred.mesh, part_geometry.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
						shadow_proxy_has_alpha = true;
						break;
					}
				}
				if (shadow_proxy_has_alpha) {
					mesh_storage->mesh_nextgi_rt_request_proxy_attributes(deferred.mesh);
				}
			}
			if (has_translucent) {
				record.card_set = NextGISRC::get_instance_card_set(deferred.instance);
				if (record.card_set == UINT32_MAX) {
					record.card_set = NextGISRC::get_mesh_card_set(deferred.mesh);
				}
				translucent_instance_count++;
			}
			float eps_rt = 0.0f;
			for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
				NextGISurfaceRecord surface_record;
				uint32_t hit_arena_chunk = UINT32_MAX;
				uint32_t hit_attribute_chunk = UINT32_MAX;
				uint32_t hit_attribute_offset = 0;
				if (part_geometry.is_proxy_surface) {
					record.arena_chunk = part.chunk;
					eps_rt = MAX(eps_rt, part_geometry.eps);
					surface_record.vertex_offset = part_geometry.vertex_offset;
					surface_record.index_offset = part_geometry.index_offset;
					surface_record.triangle_count = part_geometry.triangle_count;
					surface_record.flags = NEXTGI_RT_SURFACE_FLAG_NO_CARDS;
					hit_arena_chunk = part.chunk;
					hit_attribute_chunk = part_geometry.attribute_chunk;
					hit_attribute_offset = part_geometry.attribute_offset;
				} else {
					MeshStorage::NextGIRTSurfaceInfo info;
					if (mesh_storage->mesh_nextgi_rt_get_surface_info(deferred.mesh, part_geometry.surface, info)) {
						record.arena_chunk = info.chunk;
						eps_rt = MAX(eps_rt, info.eps);
						surface_record.vertex_offset = info.vertex_offset;
						surface_record.index_offset = info.index_offset + part_geometry.first_triangle * 3 * sizeof(uint32_t);
						surface_record.triangle_count = part_geometry.triangle_count;
						surface_record.flags = has_translucent ? 0u : uint32_t(NEXTGI_RT_SURFACE_FLAG_NO_CARDS);
						hit_arena_chunk = info.chunk;
						hit_attribute_chunk = info.attribute_chunk;
						hit_attribute_offset = info.attribute_offset;
					}
				}
				surface_records.push_back(surface_record);
				// GodotNext: path_tracer (#167). The path tracer never reaches a shadow-only surface (no GI bit): its
				// source stays empty, in record order.
				if (surface_sources_wanted) {
					surface_sources.push_back(SurfaceSource());
				}
				if (hit_records) {
					if (part_geometry.is_proxy_surface) {
						const bool proxy_alpha = shadow_proxy_has_alpha && !part_geometry.is_legacy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(deferred.instance, deferred.mesh, part_geometry.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
						if (proxy_alpha && hit_attribute_chunk != UINT32_MAX) {
							hit_materials->add_geometry(deferred.instance, deferred.mesh, part_geometry.surface, hit_arena_chunk, true, hit_attribute_chunk, hit_attribute_offset, deferred.multimesh_element);
						} else {
							hit_materials->add_fallback_surfaces(1);
						}
					} else {
						hit_materials->add_geometry(deferred.instance, deferred.mesh, part_geometry.surface, hit_arena_chunk, hit_attribute_chunk != UINT32_MAX, hit_attribute_chunk, hit_attribute_offset, deferred.multimesh_element);
					}
				}
			}
			record.origin_offset = nextgi_rt_origin_offset(eps_rt, deferred.max_scale);
			if (has_translucent) {
				_queue_pages(deferred.instance, record, deferred.element_index);
			}
			instance_records.push_back(record);
			// The AS memory line tracks the mesh while its shadow parts are in the TLAS too (a mesh may have no other).
			if (RSG::rasterizer != nullptr) {
				mesh_last_tlas_frame[deferred.mesh] = RSG::rasterizer->get_frame_number();
			}
			const uint32_t shadow_alpha_base = _push_shadow_part(deferred.instance, deferred.mesh, part.geometries, deferred.layers, part.chunk, false);
			RD::AccelerationStructureInstance tlas_instance;
			tlas_instance.transform = deferred.transform;
			tlas_instance.id = record_index;
			tlas_instance.mask = mask;
			tlas_instance.hit_sbt_range = hit_records ? hit_materials->end_instance(record_index, UINT32_MAX, false) : 0;
			tlas_instance.flags = shadow_alpha_base != UINT32_MAX ? RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
			tlas_instance.blas = part.blas;
			_check_shadow_word(tlas_instance.id + 1);
			tlas_instances.push_back(tlas_instance);
			stats.shadow_only_instances++;
			stats.shadow_unallocated_instances += (mask & NEXTGI_SHADOW_MASK_UNALLOCATED) != 0 ? 1 : 0;
			if (shadow_alpha_base != UINT32_MAX) {
				shadow_alpha_bits |= mask;
				stats.shadow_alpha_instances++;
			}
			if (shadow_check_sources_enabled) {
				_push_shadow_check_source(deferred.instance, deferred.mesh, part.geometries, deferred.transform, deferred.layers);
			}
		}
	}

	// GodotNext: nextgi_skinned_alpha_shadows (#302). Shadow-only posed skinned slots (alpha-tested or translucent
	// mesh materials, excluded from GI per D17). They share the post-GI shadow-only budget so they never displace GI.
	for (const DeferredSkinnedShadowPart &deferred : deferred_skinned_shadow_parts) {
		const uint32_t mask = nextgi_shadow_instance_mask(shadow_allocation, deferred.layers, false);
		if (mask == 0 || !deferred.blas.is_valid() || instance_records.size() + gpu_reserved >= NEXTGI_RT_TLAS_CAPACITY || instance_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
			continue;
		}
		const uint32_t record_index = instance_records.size();
		NextGIInstanceRecord record;
		for (int row = 0; row < 3; row++) {
			record.object_to_world[row * 4 + 0] = deferred.transform.basis.rows[row][0];
			record.object_to_world[row * 4 + 1] = deferred.transform.basis.rows[row][1];
			record.object_to_world[row * 4 + 2] = deferred.transform.basis.rows[row][2];
		}
		record.object_to_world[3] = deferred.transform.origin.x;
		record.object_to_world[7] = deferred.transform.origin.y;
		record.object_to_world[11] = deferred.transform.origin.z;
		record.surface_base = surface_records.size();
		record.surface_count = 1;
		record.rt_flags = NEXTGI_RT_FLAG_SKINNED;
		record.tlas_index = record_index;
		record.arena_chunk = deferred.slot.chunk;
		record.origin_offset = nextgi_rt_origin_offset(deferred.eps, deferred.max_scale);
		// No card set and no relight pages: GI never hits it.
		NextGISurfaceRecord surface_record;
		surface_record.vertex_offset = deferred.slot.vertex_offset;
		surface_record.index_offset = deferred.slot.index_offset;
		surface_record.triangle_count = deferred.slot.index_count / 3;
		surface_record.flags = NEXTGI_RT_SURFACE_FLAG_SKINNED | NEXTGI_RT_SURFACE_FLAG_NO_CARDS | ((deferred.slot.bind_vertex_offset / NEXTGI_RT_ALIGNMENT) << 8);
		surface_records.push_back(surface_record);
		// GodotNext: path_tracer (#167). The path tracer never reaches a shadow-only surface (no GI bit).
		if (surface_sources_wanted) {
			surface_sources.push_back(SurfaceSource());
		}
		instance_records.push_back(record);
		const uint32_t shadow_alpha_base = _push_shadow_skinned(deferred.instance, deferred.mesh, deferred.surface, deferred.slot.chunk, deferred.slot.index_offset, deferred.layers);
		if (hit_records) {
			if (shadow_alpha_base != UINT32_MAX) {
				hit_materials->add_geometry(deferred.instance, deferred.mesh, deferred.surface, deferred.slot.chunk, deferred.slot.attribute_chunk != UINT32_MAX, deferred.slot.attribute_chunk, deferred.slot.attribute_offset, deferred.multimesh_element);
			} else {
				hit_materials->add_fallback_surfaces(1);
			}
		}
		RD::AccelerationStructureInstance tlas_instance;
		tlas_instance.transform = deferred.transform;
		tlas_instance.id = record_index;
		tlas_instance.mask = mask;
		tlas_instance.hit_sbt_range = hit_records ? hit_materials->end_instance(record_index, UINT32_MAX, false) : 0;
		tlas_instance.flags = shadow_alpha_base != UINT32_MAX ? RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
		tlas_instance.blas = deferred.blas;
		_check_shadow_word(tlas_instance.id + 1);
		tlas_instances.push_back(tlas_instance);
		stats.shadow_only_instances++;
		stats.shadow_unallocated_instances += (mask & NEXTGI_SHADOW_MASK_UNALLOCATED) != 0 ? 1 : 0;
		if (shadow_alpha_base != UINT32_MAX) {
			shadow_alpha_bits |= mask;
			stats.shadow_alpha_instances++;
		}
		if (shadow_check_sources_enabled) {
			ShadowCheckSource check;
			check.transform = deferred.transform;
			check.world_bounds = deferred.instance->transformed_aabb;
			check.layers = deferred.layers;
			check.opaque_caster = false;
			check.has_geometry = false;
			shadow_check_sources.push_back(check);
		}
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
			if (part != nullptr && _skinned_part_blas_alive(*part)) {
				RD::get_singleton()->free_rid(part->blas);
			}
			skinned_parts.erase(key);
		}
	}

	stats.instance_count = instance_records.size();
	if (OS::get_singleton() != nullptr) {
		stats.tlas_fill_ms = double(OS::get_singleton()->get_ticks_usec() - ticks_start) / 1000.0;
	}
	if (instance_records.is_empty() && gpu_sources.is_empty() && gpu_shadow_sources.is_empty()) {
		_upload_shadow_buffers(); // GodotNext: nextgi_shadow_policy (#145)
		_update_far_records(p_render_data, instance_records, surface_records);
		_assign_pending_pages(p_render_data, instance_records); // GodotNext: nextgi_src_page_levels (#247)
		// GodotNext: ps_lumen_lite (#183)
		if (NextGIRelight::get_singleton() != nullptr) {
			NextGIRelight::get_singleton()->finish_record_pass(instance_records);
		}
		// GodotNext: nextgi_hit_groups (#130)
		if (hit_records) {
			hit_materials->finish_records(surface_records.size());
		}
		_finish_surface_sources(instance_records, surface_records); // GodotNext: path_tracer (#167)
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
	_assign_pending_pages(p_render_data, instance_records); // GodotNext: nextgi_src_page_levels (#247)
	// GodotNext: ps_lumen_lite (#183). Records whose pages a later instance of this pass evicted.
	if (NextGIRelight::get_singleton() != nullptr) {
		NextGIRelight::get_singleton()->finish_record_pass(instance_records);
	}
	// GodotNext: nextgi_hit_groups (#130)
	if (hit_records) {
		hit_materials->finish_records(surface_records.size());
	}
	_finish_surface_sources(instance_records, surface_records); // GodotNext: path_tracer (#167)
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
	if ((!gpu_sources.is_empty() || !gpu_shadow_sources.is_empty()) && _ensure_gpu_instance_buffer()) {
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
			// GodotNext: nextgi_shadow_policy (#145). The API flag nextgi_rt_instances.glsl writes for a non-opaque
			// instance: Vulkan, Direct3D 12 and Metal use RD's bit value (each driver asserts it).
			const uint32_t no_opaque_flags = uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT);
			for (const GpuSource &source : gpu_sources) {
				// GodotNext: nextgi_shadow_policy (#145). Every element of a source shares its instance's layers and
				// materials: one set of alpha entries (sampled or constant: a GPU-written instance has no hit record
				// to retrace through), and a non-opaque instance when a geometry is alpha-tested (an alpha
				// material_override); GI rays force opacity.
				const uint32_t shadow_mask = nextgi_shadow_instance_mask(shadow_allocation, source.shadow_layers, true);
				const MeshStorage::NextGIRTPart *gi_part = nullptr;
				if (const LocalVector<MeshStorage::NextGIRTPart> *gi_parts = mesh_storage->mesh_nextgi_rt_get_parts(source.mesh)) {
					gi_part = source.part_index < gi_parts->size() ? &(*gi_parts)[source.part_index] : nullptr;
				}
				if (gi_part != nullptr && gi_part->is_proxy && source.instance != nullptr && (source.shadow_layers & NEXTGI_SHADOW_LAYER_MASK) != 0) {
					const RenderGeometryInstanceBase *inst_base = static_cast<const RenderGeometryInstanceBase *>(source.instance);
					for (const NextGIRTPartGeometry &geom : gi_part->geometries) {
						if (geom.is_proxy_surface && !geom.is_legacy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(inst_base, source.mesh, geom.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
							mesh_storage->mesh_nextgi_rt_request_proxy_attributes(source.mesh);
							break;
						}
					}
				}
				const uint32_t alpha_base = gi_part != nullptr && source.instance != nullptr ? _push_shadow_alpha(source.instance, source.mesh, gi_part->geometries, source.shadow_layers, false, gi_part->chunk, false) : UINT32_MAX;
				const uint32_t encoded = encode_gpu_instances(source.buffer, source.stride, source.count, source.count, cursor, source.blas, cursor, alpha_base != UINT32_MAX ? no_opaque_flags : 0u, camera_pos, NEXTGI_RT_ANGULAR_CULL_DISTANCE, Math::deg_to_rad(0.5f), source.element_offset, source.count_buffer, shadow_mask);
				if (encoded == 0) {
					shadow_alpha.resize(alpha_base != UINT32_MAX ? alpha_base : shadow_alpha.size());
					continue;
				}
				for (uint32_t element = 0; element < encoded; element++) {
					_push_shadow_word(source.shadow_layers, alpha_base);
				}
				if (alpha_base != UINT32_MAX) {
					shadow_alpha_bits |= shadow_mask;
					stats.shadow_alpha_instances += encoded;
				}
				stats.shadow_unallocated_instances += (shadow_mask & NEXTGI_SHADOW_MASK_UNALLOCATED) != 0 ? encoded : 0;
				_check_shadow_word(cursor + encoded);
				if (shadow_check_sources_enabled) {
					ShadowCheckSource check;
					check.world_bounds = source.bounds;
					check.layers = source.shadow_layers;
					check.opaque_caster = false;
					check.has_geometry = false;
					shadow_check_sources.push_back(check);
				}
				cursor += encoded;
				referenced_blases.push_back(source.blas);
				stats.coverage.numerator += encoded;
			}
			// GodotNext: nextgi_shadow_policy (#145). The elements' shadow parts (the surfaces #61 keeps out of GI):
			// instances only shadow rays see (no GI bit), after every instance GI sees and only in the capacity left,
			// so they never push a GI instance out. Their alpha entries are sampled or constant: a GPU-written
			// instance has no hit record, so its shader-code alpha shadows as opaque.
			for (const GpuSource &source : gpu_shadow_sources) {
				const uint32_t shadow_mask = nextgi_shadow_instance_mask(shadow_allocation, source.shadow_layers, false);
				const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(source.mesh);
				if (shadow_mask == 0 || shadow_parts == nullptr || source.instance == nullptr) {
					continue;
				}
				for (const MeshStorage::NextGIRTPart &part : *shadow_parts) {
					if (!part.blas.is_valid() || part.geometries.is_empty() || cursor + source.count > NEXTGI_RT_TLAS_CAPACITY) {
						continue;
					}
					if (part.is_proxy) {
						const RenderGeometryInstanceBase *inst_base = static_cast<const RenderGeometryInstanceBase *>(source.instance);
						for (const NextGIRTPartGeometry &geom : part.geometries) {
							if (geom.is_proxy_surface && !geom.is_legacy_surface && _nextgi_shadow_class(_nextgi_shadow_effective_material(inst_base, source.mesh, geom.surface)) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
								mesh_storage->mesh_nextgi_rt_request_proxy_attributes(source.mesh);
								break;
							}
						}
					}
					const uint32_t alpha_base = _push_shadow_alpha(source.instance, source.mesh, part.geometries, source.shadow_layers, false, part.chunk, false);
					const uint32_t encoded = encode_gpu_instances(source.buffer, source.stride, source.count, source.count, cursor, part.blas, cursor, alpha_base != UINT32_MAX ? no_opaque_flags : 0u, camera_pos, NEXTGI_RT_ANGULAR_CULL_DISTANCE, Math::deg_to_rad(0.5f), source.element_offset, source.count_buffer, shadow_mask);
					if (encoded == 0) {
						shadow_alpha.resize(alpha_base != UINT32_MAX ? alpha_base : shadow_alpha.size());
						continue;
					}
					for (uint32_t element = 0; element < encoded; element++) {
						_push_shadow_word(source.shadow_layers, alpha_base);
					}
					if (alpha_base != UINT32_MAX) {
						shadow_alpha_bits |= shadow_mask;
						stats.shadow_alpha_instances += encoded;
					}
					stats.shadow_only_instances += encoded;
					stats.shadow_unallocated_instances += (shadow_mask & NEXTGI_SHADOW_MASK_UNALLOCATED) != 0 ? encoded : 0;
					_check_shadow_word(cursor + encoded);
					if (shadow_check_sources_enabled) {
						ShadowCheckSource check;
						check.world_bounds = source.bounds;
						check.layers = source.shadow_layers;
						check.opaque_caster = false;
						check.has_geometry = false;
						shadow_check_sources.push_back(check);
					}
					// The AS memory line tracks the mesh while its shadow parts are in the TLAS (it may have no GI part).
					if (RSG::rasterizer != nullptr) {
						mesh_last_tlas_frame[source.mesh] = frame;
					}
					cursor += encoded;
					referenced_blases.push_back(part.blas);
				}
			}
			stats.instance_count += cursor - tlas_instances.size();
			stats.gpu_written_instances = cursor - tlas_instances.size(); // GodotNext: nextgi_shadow_policy (#145)
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
	_upload_shadow_buffers(); // GodotNext: nextgi_shadow_policy (#145)
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
		bool is_shadow_part = false;
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
		const LocalVector<MeshStorage::NextGIRTPart> *shadow_parts = mesh_storage->mesh_nextgi_rt_get_shadow_parts(mesh);
		const bool is_proxy_mesh = (parts != nullptr && !parts->is_empty() && (*parts)[0].is_proxy) || (shadow_parts != nullptr && !shadow_parts->is_empty() && (*shadow_parts)[0].is_proxy);
		if (is_proxy_mesh) {
			bool any_pending_part = false;
			auto enqueue_proxy_parts = [&](const LocalVector<MeshStorage::NextGIRTPart> *p_list, bool p_shadow) {
				if (p_list == nullptr) {
					return;
				}
				for (uint32_t p = 0; p < p_list->size(); p++) {
					const MeshStorage::NextGIRTPart &part = (*p_list)[p];
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
					target.is_shadow_part = p_shadow;
					targets.push_back(target);
					items.push_back(item);
				}
			};
			enqueue_proxy_parts(parts, false);
			enqueue_proxy_parts(shadow_parts, true);
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
			_build_proxy_part(target.mesh, target.part_index, target.is_shadow_part);
		} else {
			_build_dirty_mesh_parts(target.mesh);
			// Rebuilding a BLAS that this frame's TLAS already references invalidates that TLAS.
			tlas_traceable = false;
		}
	}
	// GodotNext: nextgeo_rt_native (#156). Native sets spend what line 1b left, at most the N7 cap.
	// They are new BLASes, so the TLAS built this frame stays traceable.
	if (native != nullptr) {
		native->build(nextgeo_rt_native_build_budget(triangle_budget > scheduled_triangles ? triangle_budget - scheduled_triangles : 0u));
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
	// GodotNext: nextgeo_rt_native (#156). Native compaction runs even with no proxy pending.
	if (tlas.is_null() || RD::get_singleton() == nullptr) {
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
	// GodotNext: nextgeo_rt_native (#156). The proxies go first; native compactions share the cap.
	if (native != nullptr && native->compact_after_traces(compaction_triangles_this_frame, uint32_t(NEXTGI_RT_BUILD_TRIANGLE_CAP))) {
		tlas_traceable = false;
	}
}

uint32_t NextGIRTScene::_mesh_triangle_count(RID p_mesh) const {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return 0;
	}
	// GodotNext: nextgi_shadow_policy (#145). _build_dirty_mesh_parts() builds the shadow parts (the surfaces #61 keeps
	// out of the GI parts) in the same call, so their triangles count too.
	uint32_t built = 0;
	const LocalVector<MeshStorage::NextGIRTPart> *part_lists[2] = { mesh_storage->mesh_nextgi_rt_get_parts(p_mesh), mesh_storage->mesh_nextgi_rt_get_shadow_parts(p_mesh) };
	for (const LocalVector<MeshStorage::NextGIRTPart> *parts : part_lists) {
		if (parts != nullptr) {
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				built += part.triangle_count;
			}
		}
	}
	if (built > 0) {
		return built;
	}
	uint32_t triangles = 0;
	const uint32_t surface_count = uint32_t(mesh_storage->mesh_get_surface_count(p_mesh));
	for (uint32_t i = 0; i < surface_count; i++) {
		MeshStorage::NextGIRTSurfaceInfo info;
		if (!mesh_storage->mesh_nextgi_rt_get_surface_info(p_mesh, i, info) || !info.eligible) {
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
		// GodotNext: nextgi_shadow_policy (#145). The shadow parts' BLASes count too; eviction clears both.
		const LocalVector<MeshStorage::NextGIRTPart> *part_lists[2] = { mesh_storage->mesh_nextgi_rt_get_parts(entry.key), mesh_storage->mesh_nextgi_rt_get_shadow_parts(entry.key) };
		for (const LocalVector<MeshStorage::NextGIRTPart> *parts : part_lists) {
			if (parts == nullptr) {
				continue;
			}
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
	// GodotNext: nextgeo_rt_native (#156). Native BLASes sit inside the AS memory line (MN4): the
	// proxies keep the line's eviction rule, and native mode's cap leaves them room (over it,
	// instances keep the proxy), so native sets never push the tier to AS_MEMORY.
	if (native != nullptr) {
		native->set_as_line(as_budget_bytes, total);
	}
	stats.as_bytes = total + (native != nullptr ? native->get_blas_bytes() : 0u);
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

uint32_t NextGIRTScene::encode_gpu_instances(RID p_source_buffer, uint32_t p_source_stride_floats, uint32_t p_count, uint32_t p_visible_count, uint32_t p_dest_offset, RID p_blas, uint32_t p_id_base, uint32_t p_flags, const Vector3 &p_camera, float p_max_distance, float p_angle_limit, uint32_t p_source_element_offset, RID p_count_buffer, uint32_t p_mask) {
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
	push_constants[7] = p_mask & 0xFFu; // GodotNext: nextgi_shadow_policy (#145)
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
