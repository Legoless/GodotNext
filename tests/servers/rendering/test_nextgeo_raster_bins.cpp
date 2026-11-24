/**************************************************************************/
/*  test_nextgeo_raster_bins.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgeo_raster_bins)

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3). The programmable raster bins' CPU side: the
// classification into bins, the material fallback order, the refcounted bin registry, the
// per-(instance, surface) raster-bin table, the per-bin compaction against its CPU reference, and the
// GLSL copies of the layouts.

#include "core/math/random_pcg.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_features_inc.glsl.gen.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_types.h"
#ifndef _3D_DISABLED
#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/material.h"
#endif

#include <limits>

using namespace RendererRD;

namespace TestNextGeoRasterBins {

using VMS = RendererVirtualMeshStorage;

String glsl_constant(const char *p_source, const String &p_name) {
	const String source = String::utf8(p_source);
	const String key = " " + p_name + " = ";
	const int at = source.find(key);
	if (at < 0) {
		return String();
	}
	const int begin = at + key.length();
	const int end = source.find(";", begin);
	return end < 0 ? String() : source.substr(begin, end - begin).strip_edges();
}

int64_t glsl_integer(const String &p_value) {
	return p_value.trim_suffix("u").to_int();
}

} // namespace TestNextGeoRasterBins

using namespace TestNextGeoRasterBins;

TEST_CASE("[NextGeo][RasterBins] Raster features come from the material's flags (N3)") {
	NextGeoRasterFacts facts;
	CHECK_EQ(nextgeo_raster_features(facts), 0u);

	facts.alpha_scissor = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_MASKED));
	facts = NextGeoRasterFacts();
	facts.alpha_hash = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_MASKED));
	facts = NextGeoRasterFacts();
	facts.discard = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_MASKED));
	facts = NextGeoRasterFacts();
	facts.writes_vertex = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_VERTEX_OFFSET));
	facts = NextGeoRasterFacts();
	facts.depth_offset = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_DEPTH_OFFSET));
	// GodotNext: nextgeo_displacement (#150).
	facts = NextGeoRasterFacts();
	facts.displaced = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_DISPLACED));
	facts = NextGeoRasterFacts();
	facts.cull_mode = RSE::CULL_MODE_DISABLED;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_TWO_SIDED));
	facts.cull_mode = RSE::CULL_MODE_FRONT;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_TWO_SIDED));

	facts.alpha_scissor = true;
	facts.writes_vertex = true;
	const uint32_t combined = nextgeo_raster_features(facts);
	CHECK_EQ(combined, uint32_t(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_TWO_SIDED));
	CHECK_EQ(nextgeo_raster_feature_names(combined), String("MASKED | VERTEX_OFFSET | TWO_SIDED"));
	CHECK_EQ(nextgeo_raster_feature_names(0u), String("FIXED"));
}

TEST_CASE("[NextGeo][RasterBins] Only a raster program moves a surface out of the fixed-function bin") {
	// The fixed-function bin: no feature, or TWO_SIDED alone (every bin flips facing from the cull bits).
	CHECK_EQ(VMS::resolve_raster_bin(0u, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_TWO_SIDED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_FALSE(nextgeo_raster_has_program(VMS::RASTER_FEATURE_TWO_SIDED));
	for (uint32_t feature : { uint32_t(VMS::RASTER_FEATURE_MASKED), uint32_t(VMS::RASTER_FEATURE_VERTEX_OFFSET), uint32_t(VMS::RASTER_FEATURE_DEPTH_OFFSET), uint32_t(VMS::RASTER_FEATURE_DISPLACED) }) {
		CHECK(nextgeo_raster_has_program(feature));
		CHECK_EQ(VMS::resolve_raster_bin(feature, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
		CHECK_EQ(VMS::resolve_raster_bin(feature | VMS::RASTER_FEATURE_TWO_SIDED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
		// Every visibility entry has its VSM entry (#217 iterates the same bins).
		CHECK_EQ(VMS::resolve_raster_bin(feature, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	}
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_VERTEX_OFFSET, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_DISPLACED | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	// GodotNext: spline_meshes (#161). SPLINE deforms in-place in fixed-function bins and combines cleanly with programmable bins.
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SPLINE, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SPLINE, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SPLINE | VMS::RASTER_FEATURE_TWO_SIDED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SPLINE | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SPLINE | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	// Fixed-function skinned surfaces keep the FIXED bin (in-raster skinning runs in fixed and programmable bins alike).
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SKINNED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SKINNED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SKINNED | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SKINNED | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	// Features later issues add have no entry yet: the surface keeps its legacy route.
	for (uint32_t feature : { uint32_t(VMS::RASTER_FEATURE_VOXEL), uint32_t(VMS::RASTER_FEATURE_ASSEMBLY) }) {
		CHECK_EQ(VMS::resolve_raster_bin(feature, VMS::RASTER_PASS_VISIBILITY), -1);
		CHECK_EQ(VMS::resolve_raster_bin(feature | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VISIBILITY), -1);
	}
	CHECK_EQ(VMS::resolve_raster_bin(1u << 20u, VMS::RASTER_PASS_VISIBILITY), -1);
	// Each visibility entry has a VSM entry with the same bin kind.
	for (uint32_t i = 0; i < VMS::RASTER_BIN_TABLE_SIZE; i++) {
		const VMS::RasterBinEntry &entry = VMS::RASTER_BIN_TABLE[i];
		const VMS::RasterPass other = entry.pass == VMS::RASTER_PASS_VISIBILITY ? VMS::RASTER_PASS_VSM : VMS::RASTER_PASS_VISIBILITY;
		const int32_t twin = VMS::find_raster_bin_entry(entry.features, other);
		REQUIRE_MESSAGE(twin >= 0, String(entry.name));
		CHECK_EQ(uint32_t(VMS::RASTER_BIN_TABLE[twin].bin), uint32_t(entry.bin));
	}
}

TEST_CASE("[NextGeo][RasterBins] The material fallback order keeps every default route") {
	NextGeoMaterialFallbackFacts facts;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NONE);

	// #80's reasons, unchanged for materials the raster target does not reject.
	facts.masked = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);
	facts = NextGeoMaterialFallbackFacts();
	facts.writes_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	facts = NextGeoMaterialFallbackFacts();
	facts.other_custom_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	facts = NextGeoMaterialFallbackFacts();
	facts.unsupported_derivative = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE);
	facts = NextGeoMaterialFallbackFacts();
	facts.masked = true;
	facts.writes_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);

	// Screen reads stay alpha, as in #80, unless the raster target rejects the material.
	facts = NextGeoMaterialFallbackFacts();
	facts.screen_reads = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);
	facts.masked = true;
	facts.raster_rejects = ShaderCompiler::RASTER_USES_SCREEN_TEXTURE;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);

	// Earlier reasons win over the raster target's rejects.
	facts = NextGeoMaterialFallbackFacts();
	facts.raster_rejects = ShaderCompiler::RASTER_USES_SCREEN_TEXTURE;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
	facts.transparent = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);
	facts.vertex_lighting = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_VERTEX_LIGHTING);
	facts.stencil = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY);

	// rendering/nextgeo/debug_programmable_raster admits masked and vertex-offset materials, and nothing else.
	facts = NextGeoMaterialFallbackFacts();
	facts.admit_programmable = true;
	facts.masked = true;
	facts.writes_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NONE);
	facts.raster_rejects = ShaderCompiler::RASTER_USES_FRAGCOORD;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
	facts.raster_rejects = 0u;
	facts.other_custom_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	facts.other_custom_vertex = false;
	facts.transparent = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);

	// The reason is appended to the fallback enum, so the debug view's codes and Q14 keep their values.
	CHECK_EQ(uint32_t(NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE), uint32_t(NEXTGEO_FALLBACK_NO_INT64_ATOMICS) + 1u);
	CHECK(uint32_t(NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE) <= 0xFu); // The fallback view packs 4 bits.
}

TEST_CASE("[NextGeo][RasterBins] The rejects follow N4, for materials with a raster program") {
	const uint32_t all_uses = ShaderCompiler::RASTER_USES_FRAGCOORD | ShaderCompiler::RASTER_USES_SCREEN_TEXTURE | ShaderCompiler::RASTER_USES_DEPTH_TEXTURE | ShaderCompiler::RASTER_USES_NORMAL_ROUGHNESS_TEXTURE | ShaderCompiler::RASTER_WRITES_DEPTH | ShaderCompiler::RASTER_USES_FUNCTION_DISCARD | ShaderCompiler::RASTER_USES_LIGHT_DISCARD;
	CHECK_EQ(nextgeo_raster_rejects(all_uses, VMS::RASTER_FEATURE_MASKED), all_uses);
	CHECK_EQ(nextgeo_raster_rejects(all_uses, VMS::RASTER_FEATURE_VERTEX_OFFSET), all_uses);
	// No raster program: no reject; #80's reasons apply.
	CHECK_EQ(nextgeo_raster_rejects(all_uses, 0u), 0u);
	CHECK_EQ(nextgeo_raster_rejects(all_uses, VMS::RASTER_FEATURE_TWO_SIDED), 0u);
	CHECK_EQ(nextgeo_raster_rejects(ShaderCompiler::RASTER_USES_FRAGCOORD, VMS::RASTER_FEATURE_VERTEX_OFFSET), uint32_t(ShaderCompiler::RASTER_USES_FRAGCOORD));
	CHECK_EQ(nextgeo_raster_rejects(ShaderCompiler::RASTER_USES_FUNCTION_DISCARD, VMS::RASTER_FEATURE_MASKED), uint32_t(ShaderCompiler::RASTER_USES_FUNCTION_DISCARD));
	CHECK_EQ(nextgeo_raster_rejects(ShaderCompiler::RASTER_USES_LIGHT_DISCARD, VMS::RASTER_FEATURE_MASKED), uint32_t(ShaderCompiler::RASTER_USES_LIGHT_DISCARD));
	CHECK_EQ(nextgeo_raster_reject_names(all_uses), String("FRAGCOORD, SCREEN_TEXTURE, DEPTH_TEXTURE, NORMAL_ROUGHNESS_TEXTURE, DEPTH, function discard, light() discard"));
}

TEST_CASE("[NextGeo][RasterBins] The registry keys bins by features, shader version and material, refcounted") {
	CHECK_EQ(VMS::RASTER_BIN_PROGRAMMABLE_MAX, 64u);
	CHECK_EQ(VMS::RASTER_BIN_LIMIT, 65u);

	VMS::RasterBinRegistry registry;
	VMS::RasterBinKey masked;
	masked.features = VMS::RASTER_FEATURE_MASKED;
	masked.shader_version = RID::from_uint64(1);
	masked.material = RID::from_uint64(100);

	// No program feature: the fixed-function bin, no reference.
	VMS::RasterBinKey fixed = masked;
	fixed.features = VMS::RASTER_FEATURE_TWO_SIDED;
	CHECK_EQ(registry.acquire(fixed), uint32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(registry.get_active_count(), 0u);

	const uint32_t bin = registry.acquire(masked);
	CHECK_EQ(bin, 1u);
	CHECK_EQ(registry.acquire(masked), bin);
	CHECK_EQ(registry.get_refcount(bin), 2u);
	CHECK_EQ(registry.get_active_count(), 1u);
	CHECK(registry.get_key(bin) == masked);

	// Another shader version, another material or another feature mask is another bin.
	VMS::RasterBinKey other_shader = masked;
	other_shader.shader_version = RID::from_uint64(2);
	VMS::RasterBinKey other_material = masked;
	other_material.material = RID::from_uint64(101);
	VMS::RasterBinKey other_features = masked;
	other_features.features |= VMS::RASTER_FEATURE_TWO_SIDED;
	const uint32_t b2 = registry.acquire(other_shader);
	const uint32_t b3 = registry.acquire(other_material);
	const uint32_t b4 = registry.acquire(other_features);
	CHECK_EQ(b2, 2u);
	CHECK_EQ(b3, 3u);
	CHECK_EQ(b4, 4u);
	CHECK_EQ(registry.get_active_count(), 4u);

	// Material change: the old (instance, surface) reference goes, the new one comes.
	registry.release(bin);
	CHECK_EQ(registry.get_refcount(bin), 1u);
	registry.release(bin);
	CHECK_EQ(registry.get_refcount(bin), 0u);
	CHECK_EQ(registry.get_active_count(), 3u);
	// A freed index is reused by the next new key; releasing a free or fixed bin changes nothing.
	registry.release(bin);
	registry.release(VMS::RASTER_BIN_FIXED);
	registry.release(VMS::RASTER_BIN_LIMIT + 5u);
	VMS::RasterBinKey fresh = masked;
	fresh.material = RID::from_uint64(200);
	CHECK_EQ(registry.acquire(fresh), bin);
	CHECK_EQ(registry.get_active_count(), 4u);

	// Every programmable bin (1..64) taken: the 65th returns the fixed-function answer, which keeps the surface on legacy raster.
	VMS::RasterBinRegistry full;
	for (uint32_t i = 0; i < VMS::RASTER_BIN_PROGRAMMABLE_MAX; i++) {
		VMS::RasterBinKey key = masked;
		key.material = RID::from_uint64(1000 + i);
		CHECK_EQ(full.acquire(key), i + 1u);
	}
	CHECK_EQ(full.get_active_count(), 64u);
	VMS::RasterBinKey overflow = masked;
	overflow.material = RID::from_uint64(5000);
	CHECK_EQ(full.acquire(overflow), uint32_t(VMS::RASTER_BIN_FIXED));
	// A key that already has a bin still gets it, including bin 64.
	VMS::RasterBinKey existing = masked;
	existing.material = RID::from_uint64(1000);
	CHECK_EQ(full.acquire(existing), 1u);
	VMS::RasterBinKey existing_64 = masked;
	existing_64.material = RID::from_uint64(1063);
	CHECK_EQ(full.acquire(existing_64), 64u);
}

TEST_CASE("[NextGeo][RasterBins] The raster-bin table packs one byte per surface, indexed like the bucket table") {
	const uint32_t records = 5;
	LocalVector<uint32_t> table;
	table.resize(records * NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD);
	memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
	RandomPCG rng(155);
	uint8_t expected[records][32] = {};
	for (uint32_t i = 0; i < 200; i++) {
		const uint32_t record = rng.rand() % records;
		const uint32_t surface = rng.rand() % 32u;
		const uint32_t bin = rng.rand() % VMS::RASTER_BIN_LIMIT;
		nextgeo_raster_bin_table_set(table.ptr(), record, surface, bin);
		expected[record][surface] = uint8_t(bin);
	}
	for (uint32_t record = 0; record < records; record++) {
		for (uint32_t surface = 0; surface < 32u; surface++) {
			CHECK_EQ(nextgeo_raster_bin_table_get(table.ptr(), record, surface), uint32_t(expected[record][surface]));
			// The GLSL form indexes by the record's bucket base (record * 32) plus the surface.
			const uint32_t bucket_base = record * 32u;
			CHECK_EQ(nextgeo_raster_bin_table_word(record, surface), (bucket_base + surface) >> 2u);
		}
	}
}

TEST_CASE("[NextGeo][RasterBins] Cull entries pack the visible-cluster index, kind and bin") {
	RandomPCG rng(1550);
	for (uint32_t i = 0; i < 1000; i++) {
		const uint32_t index = rng.rand() % (1u << NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS);
		const uint32_t bin = i == 0 ? VMS::RASTER_BIN_PROGRAMMABLE_MAX : (1u + rng.rand() % VMS::RASTER_BIN_PROGRAMMABLE_MAX);
		const uint32_t kind = rng.rand() % 2u;
		const uint32_t entry = nextgeo_raster_bin_pack_entry(index, bin, kind);
		CHECK_EQ(nextgeo_raster_bin_entry_index(entry), index);
		CHECK_EQ(nextgeo_raster_bin_entry_bin(entry), bin);
		CHECK_EQ(nextgeo_raster_bin_entry_kind(entry), kind);
	}
	// The visible-cluster list's capacity fits the index field.
	CHECK_EQ(uint32_t(1u << NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS), uint32_t(1u << 21u));
}

TEST_CASE("[NextGeo][RasterBins] Per-bin compaction gives each (bin, kind) its clusters, exactly once") {
	constexpr uint32_t SLOTS = VMS::RASTER_BIN_PROGRAMMABLE_MAX * 2u;
	RandomPCG rng(15500);
	for (uint32_t trial = 0; trial < 8; trial++) {
		// A phase's appended entries, in cull's (random) order.
		const uint32_t entry_count = trial == 0 ? 0u : 1u + rng.rand() % 5000u;
		LocalVector<uint32_t> entries;
		uint32_t counts[SLOTS] = {};
		for (uint32_t i = 0; i < entry_count; i++) {
			// Bins cluster on a few values (including bin 64), as a real frame's do; some bins stay empty.
			const uint32_t bin = i == 0 ? VMS::RASTER_BIN_PROGRAMMABLE_MAX : (1u + (rng.rand() % 7u) * (trial + 1u) % VMS::RASTER_BIN_PROGRAMMABLE_MAX);
			const uint32_t kind = rng.rand() % 2u;
			entries.push_back(nextgeo_raster_bin_pack_entry(i * 3u + 7u, bin, kind));
			counts[(bin - 1u) * 2u + kind]++;
		}
		uint32_t starts[SLOTS] = {};
		const uint32_t total = nextgeo_raster_bin_prefix(counts, starts);
		CHECK_EQ(total, entry_count);
		// Ranges are contiguous, in bin order (1..64) with the software range first.
		uint32_t expected_start = 0;
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			CHECK_EQ(starts[slot], expected_start);
			expected_start += counts[slot];
		}
		LocalVector<uint32_t> lists;
		lists.resize(MAX(entry_count, 1u));
		uint32_t cursors[SLOTS] = {};
		nextgeo_raster_bin_scatter(entries.ptr(), entry_count, starts, cursors, lists.ptr());
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			CHECK_EQ(cursors[slot], starts[slot] + counts[slot]);
		}
		// Every entry is in its own range, once.
		for (uint32_t i = 0; i < entry_count; i++) {
			const uint32_t entry = entries[i];
			const uint32_t slot = (nextgeo_raster_bin_entry_bin(entry) - 1u) * 2u + nextgeo_raster_bin_entry_kind(entry);
			uint32_t found = 0;
			for (uint32_t k = starts[slot]; k < starts[slot] + counts[slot]; k++) {
				found += lists[k] == nextgeo_raster_bin_entry_index(entry) ? 1u : 0u;
			}
			CHECK_EQ(found, 1u);
		}
		// Each software range dispatches enough workgroups, split as #162 splits its dispatch.
		for (uint32_t bin = 1; bin <= VMS::RASTER_BIN_PROGRAMMABLE_MAX; bin++) {
			const uint32_t sw_count = counts[(bin - 1u) * 2u];
			uint32_t x = 0;
			uint32_t y = 0;
			nextgeo_raster_bin_sw_groups(sw_count, 32768u, x, y);
			CHECK(x * y >= sw_count);
			CHECK(x <= 32768u);
			CHECK_EQ(sw_count == 0u, x == 0u || y == 0u);
		}
	}
	uint32_t x = 0;
	uint32_t y = 0;
	nextgeo_raster_bin_sw_groups(70000u, 32768u, x, y);
	CHECK_EQ(x, 32768u);
	CHECK_EQ(y, 3u);
}

TEST_CASE("[NextGeo][RasterBins] The bin layouts and their GLSL copies agree") {
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX")), int64_t(VMS::RASTER_BIN_PROGRAMMABLE_MAX));
	CHECK_EQ(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_LIMIT"), String("NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + 1u"));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_KIND_SW")), int64_t(NEXTGEO_RASTER_BIN_KIND_SW));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_KIND_HW")), int64_t(NEXTGEO_RASTER_BIN_KIND_HW));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD")), int64_t(NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS")), int64_t(NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT")), int64_t(NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT")), int64_t(NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_STATE_COUNTS")), int64_t(NEXTGEO_RASTER_BIN_STATE_COUNTS));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_STATE_CURSORS")), int64_t(NEXTGEO_RASTER_BIN_STATE_CURSORS));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_STATE_FILL")), int64_t(NEXTGEO_RASTER_BIN_STATE_FILL));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_STATE_APPENDED")), int64_t(NEXTGEO_RASTER_BIN_STATE_APPENDED));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ARGS_BINS")), int64_t(NEXTGEO_RASTER_BIN_ARGS_BINS));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ARGS_SCATTER")), int64_t(NEXTGEO_RASTER_BIN_ARGS_SCATTER));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ARGS_HW_MESH")), int64_t(NEXTGEO_RASTER_BIN_ARGS_HW_MESH));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_ARGS_WORDS")), int64_t(NEXTGEO_RASTER_BIN_ARGS_WORDS));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS")), int64_t(NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS));

	// State and indirect-argument buffers stay disjoint (no dual-usage hazard), and regions do not overlap:
	// 2 phases of 64 programmable bins (IDs 1..64) of 2 kinds, plus HW mesh-task indirect records (#154).
	CHECK_EQ(nextgeo_raster_bin_count_word(0u, 1u, 0u), NEXTGEO_RASTER_BIN_STATE_COUNTS);
	CHECK_EQ(nextgeo_raster_bin_count_word(1u, VMS::RASTER_BIN_PROGRAMMABLE_MAX, 1u) + 1u, NEXTGEO_RASTER_BIN_STATE_CURSORS);
	CHECK_EQ(nextgeo_raster_bin_cursor_word(1u, VMS::RASTER_BIN_PROGRAMMABLE_MAX, 1u) + 1u, NEXTGEO_RASTER_BIN_STATE_FILL);
	CHECK_EQ(nextgeo_raster_bin_fill_word(1u, VMS::RASTER_BIN_PROGRAMMABLE_MAX, 1u) + 1u, NEXTGEO_RASTER_BIN_STATE_APPENDED);
	CHECK(NEXTGEO_RASTER_BIN_STATE_APPENDED + 2u <= NEXTGEO_RASTER_BIN_STATE_WORDS);
	CHECK_EQ(nextgeo_raster_bin_args_word(0u, 1u, 0u), NEXTGEO_RASTER_BIN_ARGS_BINS);
	CHECK_EQ(nextgeo_raster_bin_args_word(1u, VMS::RASTER_BIN_PROGRAMMABLE_MAX, 1u) + 4u, NEXTGEO_RASTER_BIN_ARGS_SCATTER);
	CHECK_EQ(nextgeo_raster_bin_scatter_args_word(0u), NEXTGEO_RASTER_BIN_ARGS_SCATTER);
	CHECK_EQ(nextgeo_raster_bin_scatter_args_word(1u) + 4u, NEXTGEO_RASTER_BIN_ARGS_HW_MESH);
	CHECK_EQ(nextgeo_raster_bin_hw_mesh_args_word(0u, 1u), NEXTGEO_RASTER_BIN_ARGS_HW_MESH);
	CHECK_EQ(nextgeo_raster_bin_hw_mesh_args_word(1u, VMS::RASTER_BIN_PROGRAMMABLE_MAX) + 4u, NEXTGEO_RASTER_BIN_ARGS_WORDS);
	CHECK_EQ(nextgeo_raster_bin_hw_count_word(0u, 1u), 0u);
	CHECK_EQ(nextgeo_raster_bin_hw_count_word(1u, VMS::RASTER_BIN_PROGRAMMABLE_MAX) + 1u, NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS);
	// Indirect arguments sit on 16-byte boundaries.
	CHECK_EQ(nextgeo_raster_bin_args_word(0u, 5u, 1u) % 4u, 0u);
	CHECK_EQ(nextgeo_raster_bin_args_word(1u, 64u, 1u) % 4u, 0u);
	CHECK_EQ(nextgeo_raster_bin_scatter_args_word(0u) % 4u, 0u);
	CHECK_EQ(nextgeo_raster_bin_scatter_args_word(1u) % 4u, 0u);
	CHECK_EQ(nextgeo_raster_bin_hw_mesh_args_word(0u, 1u) % 4u, 0u);
	CHECK_EQ(nextgeo_raster_bin_hw_mesh_args_word(1u, 64u) % 4u, 0u);
	// The hardware draw keeps #162's per-cluster vertex count.
	CHECK_EQ(NEXTGEO_RASTER_VERTEX_COUNT, 384u);

	// GodotNext: nextgeo_masked_two_sided (#153). Feature group masks match virtual_mesh_storage.h.
	CHECK_EQ(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_PIXEL_PROGRAMMABLE_FEATURES"), String("NEXTGEO_RASTER_FEATURE_MASKED | NEXTGEO_RASTER_FEATURE_DEPTH_OFFSET"));
	CHECK_EQ(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_VERTEX_PROGRAMMABLE_FEATURES"), String("NEXTGEO_RASTER_FEATURE_VERTEX_OFFSET | NEXTGEO_RASTER_FEATURE_DISPLACED"));
	CHECK_EQ(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_PROGRAM_FEATURES"), String("NEXTGEO_RASTER_PIXEL_PROGRAMMABLE_FEATURES | NEXTGEO_RASTER_VERTEX_PROGRAMMABLE_FEATURES"));
	CHECK_EQ(uint32_t(VMS::RASTER_PIXEL_PROGRAMMABLE_FEATURES), uint32_t(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_DEPTH_OFFSET));
	CHECK_EQ(uint32_t(VMS::RASTER_VERTEX_PROGRAMMABLE_FEATURES), uint32_t(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_DISPLACED));
	CHECK_EQ(uint32_t(VMS::RASTER_PROGRAM_FEATURES), uint32_t(VMS::RASTER_PIXEL_PROGRAMMABLE_FEATURES | VMS::RASTER_VERTEX_PROGRAMMABLE_FEATURES));
}

TEST_CASE("[NextGeo][RasterBins] Default masked admission and programmable raster distance cutoff (#153)") {
	// admit_masked admits masked surfaces without opening vertex-offset / displacement.
	NextGeoMaterialFallbackFacts facts;
	facts.admit_masked = true;
	facts.masked = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NONE);
	facts.writes_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	facts.admit_programmable = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NONE);

	// Alpha blend, screen reads, and raster target rejects remain rejected even with admit_masked.
	facts = NextGeoMaterialFallbackFacts();
	facts.admit_masked = true;
	facts.transparent = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);
	facts.transparent = false;
	facts.masked = true;
	facts.raster_rejects = ShaderCompiler::RASTER_USES_FRAGCOORD;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);

	// Only pixel-programmable bins without vertex offset / displacement demote past programmable_raster_distance.
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(0u));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_TWO_SIDED));
	CHECK(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_MASKED));
	CHECK(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_TWO_SIDED));
	CHECK(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_DEPTH_OFFSET));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_VERTEX_OFFSET));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_DISPLACED));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_VERTEX_OFFSET));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_DISPLACED));

	// Camera-to-AABB distance and cutoff predicate match the GPU cull shaders.
	const Vector3 aabb_min(-1.0f, -1.0f, -1.0f);
	const Vector3 aabb_max(1.0f, 1.0f, 1.0f);
	CHECK_EQ(nextgeo_aabb_camera_distance(Vector3(0.0f, 0.25f, -0.5f), aabb_min, aabb_max), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_aabb_camera_distance(Vector3(1.0f, 0.0f, 0.0f), aabb_min, aabb_max), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_aabb_camera_distance(Vector3(4.0f, 0.0f, 0.0f), aabb_min, aabb_max), doctest::Approx(3.0f));
	CHECK_EQ(nextgeo_aabb_camera_distance(Vector3(4.0f, 5.0f, 0.0f), aabb_min, aabb_max), doctest::Approx(5.0f));

	const uint32_t masked_2s = VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_TWO_SIDED;
	// 0, negative, NaN, and Inf never demote (0 = always programmable).
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, 0.0f, masked_2s));
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, -5.0f, masked_2s));
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, std::numeric_limits<float>::quiet_NaN(), masked_2s));
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, std::numeric_limits<float>::infinity(), masked_2s));
	// Inside or exactly on the cutoff boundary stays programmable; strictly past demotes to FIXED.
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(3.5f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, masked_2s));
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(4.0f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, masked_2s));
	CHECK(nextgeo_programmable_raster_cutoff(Vector3(4.01f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, masked_2s));
	// Vertex-programmable features never demote past distance.
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(20.0f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_VERTEX_OFFSET));
	CHECK_FALSE(nextgeo_programmable_raster_cutoff(Vector3(20.0f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, VMS::RASTER_FEATURE_DISPLACED));

	// Instance record stores programmable_raster_distance in prev_world_aabb_min[3] and preserves it across updates/flush.
	class DummyVMS : public RendererVirtualMeshStorage {
	protected:
		virtual void _upload_records(uint32_t p_first, uint32_t p_count) override {}
		virtual void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {}
	};
	DummyVMS storage;
	const RID mesh_rid = RID::from_uint64(15301);
	const RID inst_rid = RID::from_uint64(15302);
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u;
	desc.ngm_path = "res://test_153.ngm";
	desc.dag_header.resize(8);
	desc.root_pages.push_back(Vector<uint8_t>());
	storage.mesh_set_virtual_geometry(mesh_rid, desc);

	const uint32_t slot = storage.instance_record_allocate(inst_rid, mesh_rid);
	REQUIRE_NE(slot, UINT32_MAX);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(0.0f));
	storage.instance_record_set_programmable_raster_distance(slot, 12.5f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(12.5f));
	const AABB unit_aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	storage.instance_record_set_transform(slot, Transform3D(Basis(), Vector3(2.0f, 0.0f, 0.0f)), unit_aabb, true, false, 1.0f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(12.5f));
	storage.flush();
	storage.begin_frame();
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(12.5f));
	storage.instance_record_set_transform(slot, Transform3D(Basis(), Vector3(5.0f, 0.0f, 0.0f)), unit_aabb, true, true, 1.0f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(12.5f));
	storage.flush();
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(12.5f));
	// Non-positive and non-finite inputs sanitize to 0.0.
	storage.instance_record_set_programmable_raster_distance(slot, -4.0f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(0.0f));
	storage.instance_record_set_programmable_raster_distance(slot, std::numeric_limits<float>::quiet_NaN());
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(0.0f));
	storage.instance_record_set_programmable_raster_distance(slot, std::numeric_limits<float>::infinity());
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_min[3], doctest::Approx(0.0f));
	storage.instance_record_free(slot);
}

#ifndef _3D_DISABLED
TEST_CASE("[NextGeo][RasterBins] GeometryInstance3D programmable_raster_distance property round-trips (#153)") {
	GeometryInstance3D *inst = memnew(GeometryInstance3D);
	CHECK_EQ(inst->get_programmable_raster_distance(), doctest::Approx(0.0f));
	inst->set_programmable_raster_distance(25.0f);
	CHECK_EQ(inst->get_programmable_raster_distance(), doctest::Approx(25.0f));
	inst->set_programmable_raster_distance(0.0f);
	CHECK_EQ(inst->get_programmable_raster_distance(), doctest::Approx(0.0f));
	ERR_PRINT_OFF;
	inst->set_programmable_raster_distance(-1.0f);
	ERR_PRINT_ON;
	CHECK_EQ(inst->get_programmable_raster_distance(), doctest::Approx(0.0f));
	memdelete(inst);
}

TEST_CASE("[NextGeo][RasterBins] Every BaseMaterial3D transparency and cull mode maps to expected feature bits and routes (#153)") {
	REQUIRE(RenderingServer::get_singleton() != nullptr);
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	ShaderCompiler compiler;
	ShaderCompiler::DefaultIdentifierActions defaults;
	defaults.renames["MODEL_MATRIX"] = "read_model_matrix";
	defaults.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
	defaults.renames["VIEW_MATRIX"] = "read_view_matrix";
	defaults.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
	defaults.renames["PROJECTION_MATRIX"] = "projection_matrix";
	defaults.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
	defaults.renames["MODELVIEW_MATRIX"] = "modelview";
	defaults.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
	defaults.renames["VERTEX"] = "vertex";
	defaults.renames["NORMAL"] = "normal_highp";
	defaults.renames["TANGENT"] = "tangent";
	defaults.renames["BINORMAL"] = "binormal";
	defaults.renames["POSITION"] = "position";
	defaults.renames["UV"] = "uv_interp";
	defaults.renames["UV2"] = "uv2_interp";
	defaults.renames["COLOR"] = "color_interp";
	defaults.renames["ALBEDO"] = "albedo_highp";
	defaults.renames["ALPHA"] = "alpha_highp";
	defaults.renames["METALLIC"] = "metallic_highp";
	defaults.renames["SPECULAR"] = "specular";
	defaults.renames["ROUGHNESS"] = "roughness_highp";
	defaults.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
	defaults.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
	defaults.renames["ALPHA_ANTIALIASING_EDGE"] = "alpha_antialiasing_edge";
	defaults.renames["ALPHA_TEXTURE_COORDINATE"] = "alpha_texture_coordinate";
	defaults.renames["FRONT_FACING"] = "gl_FrontFacing";
	defaults.renames["FRAGCOORD"] = "gl_FragCoord";
	defaults.renames["SCREEN_UV"] = "screen_uv";
	defaults.renames["TIME"] = "global_time";
	defaults.renames["VIEW"] = "view_highp";
	defaults.base_texture_binding_index = 1;
	defaults.texture_layout_set = 3;
	defaults.base_uniform_string = "material.";
	defaults.base_varying_index = 15;
	defaults.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	defaults.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	defaults.resolve_builtin_derivatives["VERTEX"] = Pair<String, String>("vertex_interp_ddx", "vertex_interp_ddy");
	defaults.resolve_builtin_derivatives["UV"] = Pair<String, String>("uv_interp_ddx", "uv_interp_ddy");
	defaults.resolve_builtin_derivatives["UV2"] = Pair<String, String>("uv2_interp_ddx", "uv2_interp_ddy");
	defaults.resolve_builtin_derivatives["COLOR"] = Pair<String, String>("color_interp_ddx", "color_interp_ddy");
	defaults.resolve_builtin_derivatives["VIEW"] = Pair<String, String>("view_ddx", "view_ddy");
	defaults.resolve_mutable_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	defaults.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	defaults.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	compiler.initialize(defaults);

	const BaseMaterial3D::Transparency transparencies[] = {
		BaseMaterial3D::TRANSPARENCY_DISABLED,
		BaseMaterial3D::TRANSPARENCY_ALPHA,
		BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR,
		BaseMaterial3D::TRANSPARENCY_ALPHA_HASH,
		BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS,
	};
	const BaseMaterial3D::CullMode culls[] = {
		BaseMaterial3D::CULL_BACK,
		BaseMaterial3D::CULL_FRONT,
		BaseMaterial3D::CULL_DISABLED,
	};
	const BaseMaterial3D::AlphaAntiAliasing alpha_aas[] = {
		BaseMaterial3D::ALPHA_ANTIALIASING_OFF,
		BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE,
		BaseMaterial3D::ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE,
	};

	for (BaseMaterial3D::Transparency transparency : transparencies) {
		for (BaseMaterial3D::CullMode cull : culls) {
			for (BaseMaterial3D::AlphaAntiAliasing alpha_aa : alpha_aas) {
				Ref<StandardMaterial3D> mat;
				mat.instantiate();
				mat->set_transparency(transparency);
				mat->set_cull_mode(cull);
				mat->set_alpha_antialiasing(alpha_aa);

				const String code = RenderingServer::get_singleton()->shader_get_code(mat->get_shader_rid());
				REQUIRE_FALSE(code.is_empty());

				bool uses_alpha = false;
				bool uses_alpha_scissor = false;
				bool uses_alpha_hash = false;
				bool uses_alpha_aa = false;
				bool uses_depth_prepass_alpha = false;
				bool uses_discard = false;
				int shader_cull = RSE::CULL_MODE_BACK;

				HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
				ShaderCompiler::IdentifierActions ia;
				ia.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
				ia.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
				ia.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
				ia.uniforms = &uniforms;
				ia.usage_flag_pointers["ALPHA"] = &uses_alpha;
				ia.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &uses_alpha_scissor;
				ia.usage_flag_pointers["ALPHA_HASH_SCALE"] = &uses_alpha_hash;
				ia.usage_flag_pointers["ALPHA_ANTIALIASING_EDGE"] = &uses_alpha_aa;
				ia.usage_flag_pointers["ALPHA_TEXTURE_COORDINATE"] = &uses_alpha_aa;
				ia.usage_flag_pointers["DISCARD"] = &uses_discard;
				ia.render_mode_flags["depth_prepass_alpha"] = &uses_depth_prepass_alpha;
				ia.render_mode_flags["alpha_to_coverage"] = &uses_alpha_aa;
				ia.render_mode_flags["alpha_to_coverage_and_one"] = &uses_alpha_aa;
				ia.render_mode_values["cull_back"] = Pair<int *, int>(&shader_cull, RSE::CULL_MODE_BACK);
				ia.render_mode_values["cull_front"] = Pair<int *, int>(&shader_cull, RSE::CULL_MODE_FRONT);
				ia.render_mode_values["cull_disabled"] = Pair<int *, int>(&shader_cull, RSE::CULL_MODE_DISABLED);
				ia.visibility_resolve = true;
				ia.raster_target = true;

				ShaderCompiler::GeneratedCode gen;
				const Error err = compiler.compile(RSE::SHADER_SPATIAL, code, &ia, "base_material_matrix.gdshader", gen);
				REQUIRE_EQ(err, OK);

				NextGeoRasterFacts rfacts;
				rfacts.alpha_scissor = uses_alpha_scissor;
				rfacts.alpha_hash = uses_alpha_hash;
				rfacts.discard = uses_discard;
				rfacts.cull_mode = RSE::CullMode(shader_cull);
				const uint32_t features = nextgeo_raster_features(rfacts);

				const bool is_masked = (transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR || transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_HASH);
				const bool is_two_sided = (cull == BaseMaterial3D::CULL_FRONT || cull == BaseMaterial3D::CULL_DISABLED);
				const bool is_alpha_fallback =
						transparency == BaseMaterial3D::TRANSPARENCY_ALPHA ||
						transparency == BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS ||
						(is_masked && alpha_aa != BaseMaterial3D::ALPHA_ANTIALIASING_OFF);

				uint32_t expected_features = 0u;
				if (is_masked) {
					expected_features |= VMS::RASTER_FEATURE_MASKED;
				}
				if (is_two_sided) {
					expected_features |= VMS::RASTER_FEATURE_TWO_SIDED;
				}
				CHECK_EQ(features, expected_features);

				// Check both visibility and VSM raster bin resolution.
				const int32_t expected_bin = is_masked ? int32_t(VMS::RASTER_BIN_PROGRAMMABLE) : int32_t(VMS::RASTER_BIN_FIXED);
				CHECK_EQ(VMS::resolve_raster_bin(features, VMS::RASTER_PASS_VISIBILITY), expected_bin);
				CHECK_EQ(VMS::resolve_raster_bin(features, VMS::RASTER_PASS_VSM), expected_bin);
				CHECK_EQ(nextgeo_raster_bin_can_demote_by_distance(features), is_masked);

				// Check runtime routing with default #153 masked admission (admit_masked = true, admit_programmable = false).
				NextGeoMaterialFallbackFacts ffacts;
				const bool uses_alpha_clip = uses_alpha_scissor || uses_alpha_hash;
				ffacts.transparent = (uses_alpha && !uses_alpha_clip) || uses_alpha_aa || uses_depth_prepass_alpha;
				ffacts.masked = (features & VMS::RASTER_FEATURE_MASKED) != 0u;
				ffacts.raster_rejects = nextgeo_raster_rejects(gen.raster_uses, features);
				ffacts.admit_masked = true;
				const NextGeoFallback expected_route = is_alpha_fallback ? NEXTGEO_FALLBACK_ALPHA : NEXTGEO_FALLBACK_NONE;
				CHECK_EQ(nextgeo_material_fallback(ffacts), expected_route);
			}
		}
	}
}
#endif // _3D_DISABLED
