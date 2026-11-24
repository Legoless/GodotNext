/**************************************************************************/
/*  nextgeo_raster_bins.h                                                 */
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
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/typedefs.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4).
//
// The CPU side of the programmable raster bins: a material's raster features, what its raster target
// rejects, the material-level NextGeo fallback reason, the per-(instance, surface) raster-bin table and
// the per-bin compaction of cluster cull's output. shaders/nextgeo/raster_features_inc.glsl,
// cluster_cull.glsl and raster_bin_compact.glsl use the same layouts and formulas;
// test_nextgeo_raster_bins.cpp pins them.
namespace RendererRD {

// What a material's raster features come from (N3): ShaderData's usage and write flags.
struct NextGeoRasterFacts {
	bool alpha_scissor = false; // ALPHA_SCISSOR_THRESHOLD
	bool alpha_hash = false; // ALPHA_HASH_SCALE
	bool discard = false;
	bool writes_vertex = false; // VERTEX written in vertex()
	bool depth_offset = false; // DEPTH_OFFSET (#149)
	// GodotNext: nextgeo_displacement (#150, plan §21.4 N11). True when Material.displacement_magnitude > 0.
	bool displaced = false;
	// GodotNext: nextgeo_skinned_runtime (#160, plan §21.4 N8). True on skinned virtual mesh instances.
	bool skinned = false;
	RSE::CullMode cull_mode = RSE::CULL_MODE_BACK;
};

inline uint32_t nextgeo_raster_features(const NextGeoRasterFacts &p_facts) {
	uint32_t features = 0u;
	if (p_facts.alpha_scissor || p_facts.alpha_hash || p_facts.discard) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_MASKED;
	}
	if (p_facts.writes_vertex) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET;
	}
	if (p_facts.depth_offset) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_DEPTH_OFFSET;
	}
	// GodotNext: nextgeo_displacement (#150).
	if (p_facts.displaced) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED;
	}
	// GodotNext: nextgeo_skinned_runtime (#160).
	if (p_facts.skinned) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_SKINNED;
	}
	if (p_facts.cull_mode != RSE::CULL_MODE_BACK) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_TWO_SIDED;
	}
	return features;
}

// True when the material runs its raster program (vertex() and the coverage subset of fragment()) in
// a programmable bin. TWO_SIDED alone stays in the fixed-function bin: every bin flips facing from the
// surface's cull bits, as #162's FIXED bin does.
inline bool nextgeo_raster_has_program(uint32_t p_features) {
	return (p_features & RendererVirtualMeshStorage::RASTER_PROGRAM_FEATURES) != 0u;
}

// N4: what the raster target rejects, for a material with a raster program: FRAGCOORD, the screen, depth
// and normal-roughness textures, the absolute DEPTH, and discard in a called helper or light(). p_uses
// is ShaderCompiler::GeneratedCode::raster_uses. A material without a raster program runs no material
// code in a bin; it keeps its #80 reason (screen reads are alpha, an absolute DEPTH is a custom shader,
// and the resolve emulates FRAGCOORD).
inline uint32_t nextgeo_raster_rejects(uint32_t p_uses, uint32_t p_features) {
	if (!nextgeo_raster_has_program(p_features)) {
		return 0u;
	}
	return p_uses & (ShaderCompiler::RASTER_USES_FRAGCOORD | ShaderCompiler::RASTER_USES_SCREEN_TEXTURE | ShaderCompiler::RASTER_USES_DEPTH_TEXTURE | ShaderCompiler::RASTER_USES_NORMAL_ROUGHNESS_TEXTURE | ShaderCompiler::RASTER_WRITES_DEPTH | ShaderCompiler::RASTER_USES_FUNCTION_DISCARD | ShaderCompiler::RASTER_USES_LIGHT_DISCARD);
}

inline String nextgeo_raster_reject_names(uint32_t p_rejects) {
	static const struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{ ShaderCompiler::RASTER_USES_FRAGCOORD, "FRAGCOORD" },
		{ ShaderCompiler::RASTER_USES_SCREEN_TEXTURE, "SCREEN_TEXTURE" },
		{ ShaderCompiler::RASTER_USES_DEPTH_TEXTURE, "DEPTH_TEXTURE" },
		{ ShaderCompiler::RASTER_USES_NORMAL_ROUGHNESS_TEXTURE, "NORMAL_ROUGHNESS_TEXTURE" },
		{ ShaderCompiler::RASTER_WRITES_DEPTH, "DEPTH" },
		{ ShaderCompiler::RASTER_USES_FUNCTION_DISCARD, "function discard" },
		{ ShaderCompiler::RASTER_USES_LIGHT_DISCARD, "light() discard" },
	};
	String out;
	for (const auto &entry : names) {
		if ((p_rejects & entry.bit) != 0u) {
			out += out.is_empty() ? String(entry.name) : (String(", ") + entry.name);
		}
	}
	return out;
}

inline String nextgeo_raster_feature_names(uint32_t p_features) {
	static const char *names[] = { "MASKED", "VERTEX_OFFSET", "DEPTH_OFFSET", "TWO_SIDED", "SKINNED", "DISPLACED", "SPLINE", "VOXEL", "ASSEMBLY" };
	String out;
	for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((p_features & (1u << i)) != 0u) {
			out += out.is_empty() ? String(names[i]) : (String(" | ") + names[i]);
		}
	}
	return out.is_empty() ? String("FIXED") : out;
}

// The material-level NextGeo fallback reason (#80's order, plus #155's raster-target rejects, which come
// only from materials with a raster program). The first matching reason wins; a material the raster
// target does not reject gets #80's reason.
struct NextGeoMaterialFallbackFacts {
	bool stencil = false;
	bool vertex_lighting = false;
	// Blended or depth-less: ShaderData::uses_alpha_pass() without its screen-reading term, and alpha
	// antialiasing.
	bool transparent = false;
	bool screen_reads = false; // The screen, depth or normal-roughness texture (uses_alpha_pass()'s other term).
	bool masked = false; // RASTER_FEATURE_MASKED
	bool depth_offset = false; // RASTER_FEATURE_DEPTH_OFFSET (#149)
	bool writes_vertex = false; // RASTER_FEATURE_VERTEX_OFFSET
	// POSITION, MODELVIEW_MATRIX or PROJECTION_MATRIX writes, skip_vertex_transform, Z_CLIP_SCALE,
	// POINT_SIZE, particle trails, wireframe, the absolute DEPTH.
	bool other_custom_vertex = false;
	bool unsupported_derivative = false;
	uint32_t raster_rejects = 0u; // nextgeo_raster_rejects()
	// GodotNext: nextgeo_masked_two_sided (#153) & depth_offset (#149). Admits pixel-programmable
	// materials (masked: alpha scissor, alpha hash, discard; depth offset: DEPTH_OFFSET) into
	// programmable raster bins on the Nanite device tier by default.
	bool admit_masked = false;
	bool admit_depth_offset = false;
	// GodotNext: nextgeo_vertex_offset (#164). Admits vertex-offset (VERTEX write) materials into
	// programmable raster bins on the Nanite device tier by default.
	bool admit_vertex_offset = false;
	// rendering/nextgeo/debug_programmable_raster: admits programmable raster materials (such as
	// displaced materials) when enabled.
	bool admit_programmable = false;
};

inline NextGeoFallback nextgeo_material_fallback(const NextGeoMaterialFallbackFacts &p_facts) {
	if (p_facts.stencil) {
		return NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY;
	}
	if (p_facts.vertex_lighting) {
		return NEXTGEO_FALLBACK_VERTEX_LIGHTING;
	}
	if (p_facts.transparent) {
		return NEXTGEO_FALLBACK_ALPHA;
	}
	if (p_facts.raster_rejects != 0u) {
		return NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE;
	}
	if (p_facts.screen_reads || (p_facts.masked && !(p_facts.admit_masked || p_facts.admit_programmable))) {
		return NEXTGEO_FALLBACK_ALPHA;
	}
	if (p_facts.other_custom_vertex || (p_facts.depth_offset && !(p_facts.admit_depth_offset || p_facts.admit_programmable)) || (p_facts.writes_vertex && !(p_facts.admit_vertex_offset || p_facts.admit_programmable))) {
		return NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER;
	}
	if (p_facts.unsupported_derivative) {
		return NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE;
	}
	return NEXTGEO_FALLBACK_NONE;
}

// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5). Resolves Material.vertex_offset_max (-1.0 = inherit
// rendering/nextgeo/vertex_offset_max_default), inflates bounds by vertex_offset_max * instance_max_scale,
// and clamps offsets exceeding the bound.
static constexpr float NEXTGEO_VERTEX_OFFSET_MAX_DEFAULT = 0.5f;
static constexpr float NEXTGEO_VERTEX_OFFSET_MAX_INHERIT = -1.0f;

inline float nextgeo_resolve_vertex_offset_max(float p_material_value, float p_project_default) {
	if (Math::is_finite(p_material_value) && p_material_value >= 0.0f) {
		return p_material_value;
	}
	return (Math::is_finite(p_project_default) && p_project_default >= 0.0f) ? p_project_default : 0.0f;
}

inline float nextgeo_effective_vertex_offset_bound(bool p_writes_vertex, float p_material_value, float p_project_default) {
	if (!p_writes_vertex) {
		return 0.0f;
	}
	return nextgeo_resolve_vertex_offset_max(p_material_value, p_project_default);
}

inline float nextgeo_inflate_vertex_offset_world_margin(float p_vertex_offset_max, float p_instance_max_scale) {
	if (!Math::is_finite(p_vertex_offset_max) || !(p_vertex_offset_max > 0.0f) || !Math::is_finite(p_instance_max_scale)) {
		return 0.0f;
	}
	return p_vertex_offset_max * Math::abs(p_instance_max_scale);
}

// Clamps p_delta to length <= p_max_offset. Returns true if clamped.
inline bool nextgeo_clamp_vertex_offset(Vector3 &r_delta, float p_max_offset) {
	const float limit = (Math::is_finite(p_max_offset) && p_max_offset > 0.0f) ? p_max_offset : 0.0f;
	const real_t len = r_delta.length();
	if (len > real_t(limit)) {
		r_delta = (limit > 0.0f && len > real_t(0.0)) ? (r_delta * (real_t(limit) / len)) : Vector3();
		return true;
	}
	return false;
}

// GodotNext: nextgeo_masked_two_sided (#153, plan §21.4.1). True when a programmable bin only runs
// pixel-programmable features (masked, depth offset) and can fall back to the fixed-function bin past
// GeometryInstance3D.programmable_raster_distance; vertex-programmable bins (vertex offset, displaced)
// keep their programmable bin.
inline bool nextgeo_raster_bin_can_demote_by_distance(uint32_t p_features) {
	return (p_features & RendererVirtualMeshStorage::RASTER_PIXEL_PROGRAMMABLE_FEATURES) != 0u &&
			(p_features & RendererVirtualMeshStorage::RASTER_VERTEX_PROGRAMMABLE_FEATURES) == 0u;
}

// GodotNext: nextgeo_vertex_offset (#164). True when a programmable bin runs vertex offset (without
// non-demotable displacement) and can demote to FIXED past vertex_offset_disable_distance (combined
// with programmable_raster_distance when also pixel-programmable).
inline bool nextgeo_raster_bin_can_demote_vertex_offset(uint32_t p_features) {
	return (p_features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u &&
			(p_features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) == 0u;
}

// Distance from p_camera_pos to the closest point on the world AABB [p_aabb_min, p_aabb_max].
// Matches cluster_cull.glsl and scene_forward_clustered.glsl.
inline float nextgeo_aabb_camera_distance(const Vector3 &p_camera_pos, const Vector3 &p_aabb_min, const Vector3 &p_aabb_max) {
	const Vector3 closest(
			CLAMP(p_camera_pos.x, p_aabb_min.x, p_aabb_max.x),
			CLAMP(p_camera_pos.y, p_aabb_min.y, p_aabb_max.y),
			CLAMP(p_camera_pos.z, p_aabb_min.z, p_aabb_max.z));
	return float((p_camera_pos - closest).length());
}

// CPU reference for cluster_cull.glsl's programmable_raster_distance cutoff: returns true when the
// instance's clusters should be routed to NEXTGEO_RASTER_BIN_FIXED instead of their programmable bin.
// p_distance <= 0 (or non-finite) means always programmable.
inline bool nextgeo_programmable_raster_cutoff(const Vector3 &p_camera_pos, const Vector3 &p_aabb_min, const Vector3 &p_aabb_max, float p_distance, uint32_t p_bin_features) {
	if (!Math::is_finite(p_distance) || !(p_distance > 0.0f) || !nextgeo_raster_bin_can_demote_by_distance(p_bin_features)) {
		return false;
	}
	return nextgeo_aabb_camera_distance(p_camera_pos, p_aabb_min, p_aabb_max) > p_distance;
}

// GodotNext: nextgeo_vertex_offset (#164). True when WPO is disabled at this camera position (for both
// pure VERTEX_OFFSET demotion to FIXED and combined MASKED | VERTEX_OFFSET skipping vertex() offset).
inline bool nextgeo_vertex_offset_disabled_at_distance(const Vector3 &p_camera_pos, const Vector3 &p_aabb_min, const Vector3 &p_aabb_max, float p_disable_distance) {
	if (!Math::is_finite(p_disable_distance) || !(p_disable_distance > 0.0f)) {
		return false;
	}
	return nextgeo_aabb_camera_distance(p_camera_pos, p_aabb_min, p_aabb_max) > p_disable_distance;
}

// Combined CPU reference for cluster_cull.glsl (main & VSM): returns true when a programmable bin
// demotes completely to NEXTGEO_RASTER_BIN_FIXED given both cutoff distances.
inline bool nextgeo_raster_bin_demotes_to_fixed(const Vector3 &p_camera_pos, const Vector3 &p_aabb_min, const Vector3 &p_aabb_max, float p_programmable_distance, float p_vertex_offset_disable_distance, uint32_t p_bin_features) {
	if ((p_bin_features & RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) != 0u) {
		return false;
	}
	const bool has_pixel = (p_bin_features & RendererVirtualMeshStorage::RASTER_PIXEL_PROGRAMMABLE_FEATURES) != 0u;
	const bool has_wpo = (p_bin_features & RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET) != 0u;
	if (!has_pixel && !has_wpo) {
		return false;
	}
	const float dist = nextgeo_aabb_camera_distance(p_camera_pos, p_aabb_min, p_aabb_max);
	const bool pixel_ok = !has_pixel || (Math::is_finite(p_programmable_distance) && p_programmable_distance > 0.0f && dist > p_programmable_distance);
	const bool wpo_ok = !has_wpo || (Math::is_finite(p_vertex_offset_disable_distance) && p_vertex_offset_disable_distance > 0.0f && dist > p_vertex_offset_disable_distance);
	return pixel_ok && wpo_ok;
}

// GodotNext: nextgeo_displacement (#150). When all programmable raster bins are occupied, a surface
// keeps the material's default route: materials with a legacy default fallback return to legacy, while
// displaced-only materials (default fallback NONE) stay on the virtual fixed-function bin (bin 0),
// preserving the exclusive-dispatch invariant.
inline void nextgeo_raster_bin_apply_exhausted(NextGeoSurfaceRoute &r_route, NextGeoFallback p_default_fallback) {
	if (p_default_fallback != NEXTGEO_FALLBACK_NONE) {
		r_route.virtual_geometry = false;
		r_route.reason = p_default_fallback;
	}
}

// The per-(instance, surface) raster-bin table: one byte per surface, four surfaces per word, indexed
// like the bucket table (record * 32 + surface). 0 is the fixed-function bin.
static constexpr uint32_t NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD = 8u;

inline uint32_t nextgeo_raster_bin_table_word(uint32_t p_record, uint32_t p_surface) {
	return p_record * NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD + (p_surface >> 2u);
}

inline uint32_t nextgeo_raster_bin_table_shift(uint32_t p_surface) {
	return (p_surface & 3u) * 8u;
}

inline uint32_t nextgeo_raster_bin_table_get(const uint32_t *p_table, uint32_t p_record, uint32_t p_surface) {
	return (p_table[nextgeo_raster_bin_table_word(p_record, p_surface)] >> nextgeo_raster_bin_table_shift(p_surface)) & 0xFFu;
}

inline void nextgeo_raster_bin_table_set(uint32_t *r_table, uint32_t p_record, uint32_t p_surface, uint32_t p_bin) {
	uint32_t &word = r_table[nextgeo_raster_bin_table_word(p_record, p_surface)];
	const uint32_t shift = nextgeo_raster_bin_table_shift(p_surface);
	word = (word & ~(0xFFu << shift)) | ((p_bin & 0xFFu) << shift);
}

// Per-bin compaction (N3, issue step 5). Cluster cull appends one packed entry per programmable-bin
// cluster to its phase's region and counts it per (phase, bin, kind); a prefix pass gives every
// (bin, kind) a contiguous range of the region, in bin order with the software range first; a scatter
// pass moves the entries into their ranges. The fixed-function bin keeps #162's lists.
static constexpr uint32_t NEXTGEO_RASTER_BIN_KIND_SW = 0u;
static constexpr uint32_t NEXTGEO_RASTER_BIN_KIND_HW = 1u;
static constexpr uint32_t NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS = 21u; // NextGeoCut::CLUSTER_CAPACITY = 2^21.
static constexpr uint32_t NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT = 21u;
static constexpr uint32_t NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT = 22u;

inline uint32_t nextgeo_raster_bin_pack_entry(uint32_t p_visible_index, uint32_t p_bin, uint32_t p_kind) {
	return (p_visible_index & ((1u << NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS) - 1u)) | ((p_kind & 1u) << NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT) | (((p_bin - 1u) & 0x3Fu) << NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT);
}

inline uint32_t nextgeo_raster_bin_entry_index(uint32_t p_entry) {
	return p_entry & ((1u << NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS) - 1u);
}

inline uint32_t nextgeo_raster_bin_entry_kind(uint32_t p_entry) {
	return (p_entry >> NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT) & 1u;
}

inline uint32_t nextgeo_raster_bin_entry_bin(uint32_t p_entry) {
	return ((p_entry >> NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT) & 0x3Fu) + 1u;
}

// Word layouts of the bin state buffer and the dedicated indirect-arguments buffer
// (raster_features_inc.glsl has the GLSL copies). Keeping indirect commands in raster_bin_args (written
// only by PREFIX, read as indirect arguments by SCATTER, SW and HW) avoids dual-usage tracker hazards.
static constexpr uint32_t NEXTGEO_RASTER_BIN_STATE_COUNTS = 0u; // [phase][bin - 1][kind] cluster counts
static constexpr uint32_t NEXTGEO_RASTER_BIN_STATE_CURSORS = 256u; // [phase][bin - 1][kind] range starts in the phase's region
static constexpr uint32_t NEXTGEO_RASTER_BIN_STATE_FILL = 512u; // [phase][bin - 1][kind] scatter fill counters
static constexpr uint32_t NEXTGEO_RASTER_BIN_STATE_APPENDED = 768u; // [phase] appended entries
static constexpr uint32_t NEXTGEO_RASTER_BIN_STATE_WORDS = 772u;

static constexpr uint32_t NEXTGEO_RASTER_BIN_ARGS_BINS = 0u; // [phase][bin - 1] SW uvec4 dispatch, HW uvec4 draw
static constexpr uint32_t NEXTGEO_RASTER_BIN_ARGS_SCATTER = 1024u; // [phase] uvec4 scatter dispatch
// GodotNext: nextgeo_mesh_raster (#154). [phase][bin - 1] HW mesh-task uvec4 indirect dispatch.
static constexpr uint32_t NEXTGEO_RASTER_BIN_ARGS_HW_MESH = 1032u;
static constexpr uint32_t NEXTGEO_RASTER_BIN_ARGS_WORDS = 1544u;
static constexpr uint32_t NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS = 128u; // [phase][bin - 1] indirect draw count (0 or 1)

inline uint32_t nextgeo_raster_bin_count_word(uint32_t p_phase, uint32_t p_bin, uint32_t p_kind) {
	return NEXTGEO_RASTER_BIN_STATE_COUNTS + (p_phase * RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 2u + p_kind;
}

inline uint32_t nextgeo_raster_bin_cursor_word(uint32_t p_phase, uint32_t p_bin, uint32_t p_kind) {
	return NEXTGEO_RASTER_BIN_STATE_CURSORS + (p_phase * RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 2u + p_kind;
}

inline uint32_t nextgeo_raster_bin_fill_word(uint32_t p_phase, uint32_t p_bin, uint32_t p_kind) {
	return NEXTGEO_RASTER_BIN_STATE_FILL + (p_phase * RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 2u + p_kind;
}

inline uint32_t nextgeo_raster_bin_args_word(uint32_t p_phase, uint32_t p_bin, uint32_t p_kind) {
	return NEXTGEO_RASTER_BIN_ARGS_BINS + (p_phase * RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 8u + p_kind * 4u;
}

inline uint32_t nextgeo_raster_bin_scatter_args_word(uint32_t p_phase) {
	return NEXTGEO_RASTER_BIN_ARGS_SCATTER + p_phase * 4u;
}

inline uint32_t nextgeo_raster_bin_hw_mesh_args_word(uint32_t p_phase, uint32_t p_bin) {
	return NEXTGEO_RASTER_BIN_ARGS_HW_MESH + (p_phase * RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 4u;
}

inline uint32_t nextgeo_raster_bin_hw_count_word(uint32_t p_phase, uint32_t p_bin) {
	return p_phase * RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u);
}

// The prefix pass for one phase: p_counts[(bin - 1) * 2 + kind] in, the range starts out. Returns the total.
inline uint32_t nextgeo_raster_bin_prefix(const uint32_t *p_counts, uint32_t *r_starts) {
	uint32_t total = 0u;
	for (uint32_t i = 0; i < RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX * 2u; i++) {
		r_starts[i] = total;
		total += p_counts[i];
	}
	return total;
}

// A software range's dispatch: x and y workgroups, split as #162 splits its software dispatch.
inline void nextgeo_raster_bin_sw_groups(uint32_t p_count, uint32_t p_dispatch_width, uint32_t &r_x, uint32_t &r_y) {
	r_x = MIN(p_count, p_dispatch_width);
	r_y = p_count == 0u ? 0u : (p_count - 1u) / p_dispatch_width + 1u;
}

// The scatter pass on the CPU: places every appended entry of a phase into its (bin, kind) range.
// p_starts comes from nextgeo_raster_bin_prefix(). Entries keep their append order within a range here;
// the GPU's atomic cursors may order them differently, which does not change any bin's set.
inline void nextgeo_raster_bin_scatter(const uint32_t *p_entries, uint32_t p_entry_count, const uint32_t *p_starts, uint32_t *r_cursors, uint32_t *r_lists) {
	for (uint32_t i = 0; i < RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE_MAX * 2u; i++) {
		r_cursors[i] = p_starts[i];
	}
	for (uint32_t i = 0; i < p_entry_count; i++) {
		const uint32_t entry = p_entries[i];
		const uint32_t slot = (nextgeo_raster_bin_entry_bin(entry) - 1u) * 2u + nextgeo_raster_bin_entry_kind(entry);
		r_lists[r_cursors[slot]++] = nextgeo_raster_bin_entry_index(entry);
	}
}

} // namespace RendererRD
