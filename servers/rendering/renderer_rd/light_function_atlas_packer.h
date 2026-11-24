/**************************************************************************/
/*  light_function_atlas_packer.h                                         */
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

#pragma once

#include "core/math/math_funcs.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

#include <cstdint>

namespace RendererRD {

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8, GM-11, M-9).
// Pure, device-free shelf-packer, static/animated dirty tracker, and CPU projection reference
// for the Light Function Atlas. Shared by the GPU LightFunctionAtlas and CPU doctests.

class LightFunctionAtlasPacker {
public:
	static constexpr uint32_t DEFAULT_ATLAS_SIZE = 2048u;
	static constexpr uint32_t MIN_ATLAS_SIZE = 256u;
	static constexpr uint32_t MAX_ATLAS_SIZE = 8192u;
	static constexpr uint32_t DEFAULT_TILE_SIZE = 256u;
	static constexpr uint32_t MIN_TILE_SIZE = 16u;
	static constexpr uint32_t ENTRY_NONE = 0xFFFFFFFFu;

	enum Format : uint32_t {
		FORMAT_R8 = 0,
		FORMAT_RGBA8 = 1,
	};

	enum Mode : uint32_t {
		MODE_DIRECTIONAL = 0,
		MODE_OMNI = 1,
		MODE_SPOT = 2,
		MODE_AREA = 3,
	};

	static constexpr uint32_t FLAG_MODE_MASK = 3u;
	static constexpr uint32_t FLAG_RGBA = 1u << 2u;

	struct TileRect {
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t size = 0;
		bool valid = false;

		// Half-texel inset UV mapping so bilinear filtering never bleeds across tile edges.
		// Maps local UV in [0, 1] -> atlas UV in [(x + 0.5)/N, (x + size - 0.5)/N].
		Vector4 to_inset_uv_rect(uint32_t p_atlas_size) const {
			if (!valid || size == 0 || p_atlas_size == 0) {
				return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
			}
			const float inv_atlas = 1.0f / float(p_atlas_size);
			const float inset_span = size > 1u ? float(size - 1u) : 0.0f;
			return Vector4(
					(float(x) + 0.5f) * inv_atlas,
					(float(y) + 0.5f) * inv_atlas,
					inset_span * inv_atlas,
					inset_span * inv_atlas);
		}
	};

	// Matches struct LightFunctionEntry in servers/rendering/renderer_rd/shaders/light_function_inc.glsl (std430, 80 bytes).
	struct EntryGPU {
		float tile_rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float world_to_light0[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		float world_to_light1[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
		float world_to_light2[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
		float scale_params[3] = { 1.0f, 1.0f, 1.0f };
		uint32_t flags = 0;
	};
	static_assert(sizeof(EntryGPU) == 80, "LightFunctionAtlasPacker::EntryGPU must match light_function_inc.glsl.");

	struct BakeJob {
		uint64_t light_id = 0;
		uint64_t material_id = 0;
		TileRect rect;
		bool is_animated = false;
	};

	struct Stats {
		uint32_t atlas_size = DEFAULT_ATLAS_SIZE;
		uint32_t current_tile_size = DEFAULT_TILE_SIZE;
		uint32_t active_tiles = 0;
		uint32_t animated_tiles = 0;
		uint32_t static_tiles = 0;
		uint32_t last_frame_bakes = 0;
		uint64_t total_bakes = 0;
		uint64_t static_bakes = 0;
		uint64_t animated_bakes = 0;
		uint64_t repacks = 0;
	};

private:
	struct Shelf {
		uint32_t y = 0;
		uint32_t height = 0;
		uint32_t cursor_x = 0;
	};

	struct LightRecord {
		uint64_t light_id = 0;
		uint64_t material_id = 0;
		uint64_t baked_material_serial = 0;
		uint64_t current_material_serial = 0;
		uint64_t last_baked_frame = UINT64_MAX;
		TileRect rect;
		bool is_animated = false;
		bool dirty = true;
	};

	uint32_t atlas_size = DEFAULT_ATLAS_SIZE;
	uint32_t current_tile_size = DEFAULT_TILE_SIZE;
	uint32_t next_shelf_y = 0;
	LocalVector<Shelf> shelves;
	HashMap<uint32_t, LocalVector<TileRect>> free_tiles;
	HashMap<uint64_t, LightRecord> records;
	LocalVector<uint64_t> insertion_order;
	Stats stats;

	static uint32_t _sanitize_atlas_size(uint32_t p_size) {
		uint32_t clamped = CLAMP(p_size, MIN_ATLAS_SIZE, MAX_ATLAS_SIZE);
		uint32_t p2 = MIN_ATLAS_SIZE;
		while (p2 < clamped && p2 < MAX_ATLAS_SIZE) {
			p2 <<= 1u;
		}
		return p2;
	}

	static uint32_t _preferred_tile_size(uint32_t p_atlas_size) {
		uint32_t pref = MIN(DEFAULT_TILE_SIZE, p_atlas_size / 4u);
		return MAX(pref, MIN_TILE_SIZE);
	}

	void _reset_packing_geometry(uint32_t p_tile_size) {
		shelves.clear();
		free_tiles.clear();
		next_shelf_y = 0;
		current_tile_size = CLAMP(p_tile_size, MIN_TILE_SIZE, atlas_size);
	}

	bool _try_place_tile(uint32_t p_size, TileRect &r_rect) {
		if (p_size == 0 || p_size > atlas_size) {
			return false;
		}
		LocalVector<TileRect> *bucket = free_tiles.getptr(p_size);
		if (bucket != nullptr && !bucket->is_empty()) {
			r_rect = (*bucket)[bucket->size() - 1];
			bucket->resize(bucket->size() - 1);
			r_rect.valid = true;
			return true;
		}
		for (uint32_t i = 0; i < shelves.size(); i++) {
			Shelf &shelf = shelves[i];
			if (shelf.height == p_size && shelf.cursor_x + p_size <= atlas_size) {
				r_rect.x = shelf.cursor_x;
				r_rect.y = shelf.y;
				r_rect.size = p_size;
				r_rect.valid = true;
				shelf.cursor_x += p_size;
				return true;
			}
		}
		if (next_shelf_y + p_size <= atlas_size) {
			Shelf shelf;
			shelf.y = next_shelf_y;
			shelf.height = p_size;
			shelf.cursor_x = p_size;
			shelves.push_back(shelf);
			next_shelf_y += p_size;
			r_rect.x = 0;
			r_rect.y = shelf.y;
			r_rect.size = p_size;
			r_rect.valid = true;
			return true;
		}
		return false;
	}

	bool _repack_all(uint32_t p_target_tile_size) {
		uint32_t try_size = CLAMP(p_target_tile_size, MIN_TILE_SIZE, atlas_size);
		while (try_size >= MIN_TILE_SIZE) {
			const uint32_t per_axis = atlas_size / try_size;
			const uint64_t capacity = uint64_t(per_axis) * uint64_t(per_axis);
			if (capacity >= uint64_t(records.size())) {
				break;
			}
			if (try_size == MIN_TILE_SIZE) {
				return false;
			}
			try_size >>= 1u;
		}

		_reset_packing_geometry(try_size);
		for (uint32_t i = 0; i < insertion_order.size(); i++) {
			LightRecord *rec = records.getptr(insertion_order[i]);
			if (rec == nullptr) {
				continue;
			}
			TileRect placed;
			if (!_try_place_tile(try_size, placed)) {
				rec->rect = TileRect();
				return false;
			}
			rec->rect = placed;
			rec->dirty = true;
			rec->last_baked_frame = UINT64_MAX;
		}
		stats.repacks++;
		stats.current_tile_size = current_tile_size;
		return true;
	}

	void _refresh_counts() {
		uint32_t anim = 0;
		uint32_t stat = 0;
		for (const KeyValue<uint64_t, LightRecord> &kv : records) {
			if (!kv.value.rect.valid) {
				continue;
			}
			if (kv.value.is_animated) {
				anim++;
			} else {
				stat++;
			}
		}
		stats.atlas_size = atlas_size;
		stats.current_tile_size = current_tile_size;
		stats.active_tiles = anim + stat;
		stats.animated_tiles = anim;
		stats.static_tiles = stat;
	}

public:
	explicit LightFunctionAtlasPacker(uint32_t p_atlas_size = DEFAULT_ATLAS_SIZE) {
		atlas_size = _sanitize_atlas_size(p_atlas_size);
		current_tile_size = _preferred_tile_size(atlas_size);
		stats.atlas_size = atlas_size;
		stats.current_tile_size = current_tile_size;
	}

	static uint32_t sanitize_atlas_size(uint32_t p_size) {
		return _sanitize_atlas_size(p_size);
	}

	uint32_t get_atlas_size() const { return atlas_size; }
	uint32_t get_current_tile_size() const { return current_tile_size; }
	uint32_t get_active_count() const { return records.size(); }
	const Stats &get_stats() const { return stats; }

	void set_atlas_size(uint32_t p_new_size) {
		const uint32_t sanitized = _sanitize_atlas_size(p_new_size);
		if (sanitized == atlas_size) {
			return;
		}
		atlas_size = sanitized;
		if (records.is_empty()) {
			_reset_packing_geometry(_preferred_tile_size(atlas_size));
		} else {
			_repack_all(_preferred_tile_size(atlas_size));
		}
		_refresh_counts();
	}

	void mark_all_dirty() {
		for (KeyValue<uint64_t, LightRecord> &kv : records) {
			kv.value.dirty = true;
			kv.value.last_baked_frame = UINT64_MAX;
		}
	}

	void clear() {
		records.clear();
		insertion_order.clear();
		_reset_packing_geometry(_preferred_tile_size(atlas_size));
		_refresh_counts();
	}

	// Register or update a light's material/animation/serial state.
	// Allocates an adaptive square tile on first registration, halving & repacking if full.
	bool register_or_update_light(uint64_t p_light_id, uint64_t p_material_id, bool p_is_animated, uint64_t p_material_serial, uint32_t p_fixed_tile_size = 0) {
		LightRecord *existing = records.getptr(p_light_id);
		if (existing != nullptr) {
			const bool changed = (existing->material_id != p_material_id) ||
					(existing->is_animated != p_is_animated) ||
					(existing->current_material_serial != p_material_serial);
			existing->material_id = p_material_id;
			existing->is_animated = p_is_animated;
			existing->current_material_serial = p_material_serial;
			if (changed) {
				existing->dirty = true;
			}
			_refresh_counts();
			return existing->rect.valid;
		}

		LightRecord rec;
		rec.light_id = p_light_id;
		rec.material_id = p_material_id;
		rec.is_animated = p_is_animated;
		rec.current_material_serial = p_material_serial;
		rec.baked_material_serial = UINT64_MAX;
		rec.dirty = true;

		const uint32_t req_size = p_fixed_tile_size > 0 ? CLAMP(p_fixed_tile_size, MIN_TILE_SIZE, atlas_size) : current_tile_size;
		TileRect placed;
		if (_try_place_tile(req_size, placed)) {
			rec.rect = placed;
			records.insert(p_light_id, rec);
			insertion_order.push_back(p_light_id);
			_refresh_counts();
			return true;
		}

		// Fixed-size callers do not repack implicitly; adaptive callers halve & repack.
		if (p_fixed_tile_size > 0) {
			return false;
		}

		records.insert(p_light_id, rec);
		insertion_order.push_back(p_light_id);
		const uint32_t halved = MAX(current_tile_size >> 1u, MIN_TILE_SIZE);
		if (!_repack_all(halved)) {
			records.erase(p_light_id);
			insertion_order.resize(insertion_order.size() - 1);
			_refresh_counts();
			return false;
		}
		_refresh_counts();
		return true;
	}

	bool unregister_light(uint64_t p_light_id) {
		LightRecord *existing = records.getptr(p_light_id);
		if (existing == nullptr) {
			return false;
		}
		if (existing->rect.valid) {
			TileRect freed = existing->rect;
			freed.valid = false;
			free_tiles[freed.size].push_back(freed);
		}
		records.erase(p_light_id);
		for (uint32_t i = 0; i < insertion_order.size(); i++) {
			if (insertion_order[i] == p_light_id) {
				insertion_order.remove_at(i);
				break;
			}
		}
		if (records.is_empty()) {
			_reset_packing_geometry(_preferred_tile_size(atlas_size));
		}
		_refresh_counts();
		return true;
	}

	bool has_light(uint64_t p_light_id) const {
		return records.has(p_light_id);
	}

	TileRect get_tile_rect(uint64_t p_light_id) const {
		const LightRecord *rec = records.getptr(p_light_id);
		return rec != nullptr ? rec->rect : TileRect();
	}

	uint64_t get_light_material(uint64_t p_light_id) const {
		const LightRecord *rec = records.getptr(p_light_id);
		return rec != nullptr ? rec->material_id : 0;
	}

	void get_registered_light_ids(LocalVector<uint64_t> &r_ids) const {
		r_ids = insertion_order;
	}

	// Collects the tiles that must be baked on p_frame_number:
	// - Static functions bake ONLY when dirty (creation, repack, material/shader/param change, atlas resize/format change).
	// - Animated (TIME-using) functions bake at most once per frame number (multiple views in one frame share the bake).
	uint32_t collect_bake_jobs(uint64_t p_frame_number, LocalVector<BakeJob> &r_jobs) {
		r_jobs.clear();
		uint32_t frame_bakes = 0;
		for (uint32_t i = 0; i < insertion_order.size(); i++) {
			LightRecord *rec = records.getptr(insertion_order[i]);
			if (rec == nullptr || !rec->rect.valid) {
				continue;
			}
			if (rec->baked_material_serial != rec->current_material_serial) {
				rec->dirty = true;
			}
			const bool needs_bake = rec->dirty || (rec->is_animated && rec->last_baked_frame != p_frame_number);
			if (!needs_bake) {
				continue;
			}
			BakeJob job;
			job.light_id = rec->light_id;
			job.material_id = rec->material_id;
			job.rect = rec->rect;
			job.is_animated = rec->is_animated;
			r_jobs.push_back(job);

			rec->dirty = false;
			rec->baked_material_serial = rec->current_material_serial;
			rec->last_baked_frame = p_frame_number;
			frame_bakes++;
			stats.total_bakes++;
			if (rec->is_animated) {
				stats.animated_bakes++;
			} else {
				stats.static_bakes++;
			}
		}
		stats.last_frame_bakes = frame_bakes;
		_refresh_counts();
		return frame_bakes;
	}

	// Builds the 80-byte GPU projection record shared by Forward+, Volumetric Fog, NextGI SRC, RT shadows, and MegaLights.
	static EntryGPU build_entry_gpu(const TileRect &p_rect, uint32_t p_atlas_size, Mode p_mode, const Transform3D &p_light_world_xform, const Vector3 &p_scale, float p_spot_angle_deg, bool p_is_rgba) {
		EntryGPU entry;
		const Vector4 uv_rect = p_rect.to_inset_uv_rect(p_atlas_size);
		entry.tile_rect[0] = uv_rect.x;
		entry.tile_rect[1] = uv_rect.y;
		entry.tile_rect[2] = uv_rect.z;
		entry.tile_rect[3] = uv_rect.w;

		// Use orthonormalized light transform so light scale never skews world->light projection.
		const Transform3D world_to_light = p_light_world_xform.orthonormalized().affine_inverse();
		entry.world_to_light0[0] = float(world_to_light.basis.rows[0][0]);
		entry.world_to_light0[1] = float(world_to_light.basis.rows[0][1]);
		entry.world_to_light0[2] = float(world_to_light.basis.rows[0][2]);
		entry.world_to_light0[3] = float(world_to_light.origin.x);

		entry.world_to_light1[0] = float(world_to_light.basis.rows[1][0]);
		entry.world_to_light1[1] = float(world_to_light.basis.rows[1][1]);
		entry.world_to_light1[2] = float(world_to_light.basis.rows[1][2]);
		entry.world_to_light1[3] = float(world_to_light.origin.y);

		entry.world_to_light2[0] = float(world_to_light.basis.rows[2][0]);
		entry.world_to_light2[1] = float(world_to_light.basis.rows[2][1]);
		entry.world_to_light2[2] = float(world_to_light.basis.rows[2][2]);
		entry.world_to_light2[3] = float(world_to_light.origin.z);

		const float sx = MAX(Math::abs(float(p_scale.x)), 1e-4f);
		const float sy = MAX(Math::abs(float(p_scale.y)), 1e-4f);
		const float sz = MAX(Math::abs(float(p_scale.z)), 1e-4f);
		if (p_mode == MODE_SPOT) {
			const float half_rad = Math::deg_to_rad(CLAMP(p_spot_angle_deg, 0.1f, 89.5f));
			const float cot_half = 1.0f / Math::tan(half_rad);
			entry.scale_params[0] = cot_half / sx;
			entry.scale_params[1] = cot_half / sy;
			entry.scale_params[2] = 1.0f / sz;
		} else {
			entry.scale_params[0] = 1.0f / sx;
			entry.scale_params[1] = 1.0f / sy;
			entry.scale_params[2] = 1.0f / sz;
		}
		entry.flags = (uint32_t(p_mode) & FLAG_MODE_MASK) | (p_is_rgba ? FLAG_RGBA : 0u);
		return entry;
	}

	// Pure CPU projection mirror of light_function_inc.glsl for unit tests.
	static Vector2 vec3_to_oct(const Vector3 &p_n) {
		const float l1 = Math::abs(p_n.x) + Math::abs(p_n.y) + Math::abs(p_n.z);
		if (l1 <= 1e-12f) {
			return Vector2(0.5f, 0.5f);
		}
		float nx = p_n.x / l1;
		float ny = p_n.y / l1;
		const float nz = p_n.z / l1;
		if (nz < 0.0f) {
			const float ox = (1.0f - Math::abs(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
			const float oy = (1.0f - Math::abs(nx)) * (ny >= 0.0f ? 1.0f : -1.0f);
			nx = ox;
			ny = oy;
		}
		return Vector2(nx * 0.5f + 0.5f, ny * 0.5f + 0.5f);
	}

	static bool project_local_uv(const EntryGPU &p_entry, const Vector3 &p_world_pos, Vector2 &r_local_uv) {
		const Vector3 local_pos(
				p_entry.world_to_light0[0] * p_world_pos.x + p_entry.world_to_light0[1] * p_world_pos.y + p_entry.world_to_light0[2] * p_world_pos.z + p_entry.world_to_light0[3],
				p_entry.world_to_light1[0] * p_world_pos.x + p_entry.world_to_light1[1] * p_world_pos.y + p_entry.world_to_light1[2] * p_world_pos.z + p_entry.world_to_light1[3],
				p_entry.world_to_light2[0] * p_world_pos.x + p_entry.world_to_light2[1] * p_world_pos.y + p_entry.world_to_light2[2] * p_world_pos.z + p_entry.world_to_light2[3]);

		const uint32_t mode = p_entry.flags & FLAG_MODE_MASK;
		if (mode == MODE_DIRECTIONAL || mode == MODE_AREA) {
			const float u = local_pos.x * p_entry.scale_params[0] + 0.5f;
			const float v = local_pos.y * p_entry.scale_params[1] + 0.5f;
			r_local_uv = Vector2(u - Math::floor(u), v - Math::floor(v));
			return true;
		}
		if (mode == MODE_OMNI) {
			const Vector3 scaled(local_pos.x * p_entry.scale_params[0], local_pos.y * p_entry.scale_params[1], local_pos.z * p_entry.scale_params[2]);
			const float len2 = scaled.length_squared();
			const Vector3 dir = len2 > 1e-12f ? (scaled / Math::sqrt(len2)) : Vector3(0.0f, 0.0f, -1.0f);
			r_local_uv = vec3_to_oct(dir);
			return true;
		}
		// MODE_SPOT: light shines along -Z.
		const float depth = -local_pos.z;
		if (depth <= 1e-5f) {
			r_local_uv = Vector2(0.0f, 0.0f);
			return false;
		}
		const float ndc_x = (local_pos.x / depth) * p_entry.scale_params[0];
		const float ndc_y = (local_pos.y / depth) * p_entry.scale_params[1];
		r_local_uv = Vector2(
				CLAMP(ndc_x * 0.5f + 0.5f, 0.0f, 1.0f),
				CLAMP(-ndc_y * 0.5f + 0.5f, 0.0f, 1.0f));
		return true;
	}

	static bool project_atlas_uv(const EntryGPU &p_entry, const Vector3 &p_world_pos, Vector2 &r_atlas_uv) {
		if (p_entry.tile_rect[2] <= 0.0f || p_entry.tile_rect[3] <= 0.0f) {
			return false;
		}
		Vector2 local_uv;
		if (!project_local_uv(p_entry, p_world_pos, local_uv)) {
			return false;
		}
		r_atlas_uv = Vector2(
				p_entry.tile_rect[0] + CLAMP(local_uv.x, 0.0f, 1.0f) * p_entry.tile_rect[2],
				p_entry.tile_rect[1] + CLAMP(local_uv.y, 0.0f, 1.0f) * p_entry.tile_rect[3]);
		return true;
	}
};

} // namespace RendererRD
