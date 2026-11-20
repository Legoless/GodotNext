/**************************************************************************/
/*  test_material_texture_cache.cpp                                       */
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

// GodotNext: nextgeo_texture_cache_refresh (#305). Device-free doctests for MaterialStorage::TextureCacheIndex,
// MaterialData::texture_cache refreshes after streaming texture swaps (texture_replace_compatible),
// get_parameter_uniforms() consumers (#155 raster bins, #158 shading bins), and #130/#145 bindless residency
// release interactions.

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_material_texture_cache)

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/rendering_device_bindless.h"

namespace TestMaterialTextureCache {

struct StubMaterialData : public RendererRD::MaterialStorage::MaterialData {
	virtual void set_render_priority(int p_priority) override {}
	virtual void set_next_pass(RID p_pass) override {}
	virtual bool update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) override {
		return false;
	}
};

static RID make_rid(uint64_t p_id) {
	return RID::from_uint64(p_id);
}

static ShaderCompiler::GeneratedCode::Texture make_tex_uniform(const StringName &p_name, int p_array_size = 0) {
	ShaderCompiler::GeneratedCode::Texture tex;
	tex.name = p_name;
	tex.array_size = p_array_size;
	return tex;
}

TEST_CASE("[MaterialStorage][TextureCache] Streaming swap refreshes texture_cache and #155/#158 parameter uniforms") {
	RendererRD::MaterialStorage::TextureCacheIndex index;
	StubMaterialData mat_a;
	StubMaterialData mat_b;

	const RID rd_albedo_old = make_rid(1001);
	const RID rd_albedo_new = make_rid(1002);
	const RID rd_normal_old = make_rid(1010);
	const RID rd_normal_new = make_rid(1011);
	const RID rd_arr_0 = make_rid(1020);
	const RID rd_arr_1_old = make_rid(1021);
	const RID rd_arr_1_new = make_rid(1022);

	// Material A has a scalar albedo texture, a normal map, and a 2-element texture array (reusing albedo).
	Vector<RID> mat_a_textures = { rd_albedo_old, rd_normal_old, rd_arr_0, rd_arr_1_old };
	// Material B shares rd_albedo_old in two slots and uses rd_normal_old.
	Vector<RID> mat_b_textures = { rd_albedo_old, rd_albedo_old };

	index.set_material_textures(&mat_a, mat_a_textures);
	index.set_material_textures(&mat_b, mat_b_textures);

	CHECK_EQ(index.get_tracked_texture_count(), 4u);
	CHECK_EQ(index.get_material_count_for_texture(rd_albedo_old), 2u);
	CHECK_EQ(index.get_material_count_for_texture(rd_normal_old), 1u);

	// Swap rd_albedo_old -> rd_albedo_new (as TextureStorage::texture_replace_compatible does).
	LocalVector<RendererRD::MaterialStorage::MaterialData *> affected;
	const uint32_t updated = index.replace_texture(rd_albedo_old, rd_albedo_new, &affected);
	CHECK_EQ(updated, 2u);
	CHECK_EQ(affected.size(), 2u);
	CHECK_EQ(index.get_material_count_for_texture(rd_albedo_old), 0u);
	CHECK_EQ(index.get_material_count_for_texture(rd_albedo_new), 2u);

	// Both materials' texture_cache vectors now hold rd_albedo_new with zero references to rd_albedo_old.
	CHECK_EQ(mat_a.get_texture_cache()[0], rd_albedo_new);
	CHECK_EQ(mat_a.get_texture_cache()[1], rd_normal_old);
	CHECK_EQ(mat_b.get_texture_cache()[0], rd_albedo_new);
	CHECK_EQ(mat_b.get_texture_cache()[1], rd_albedo_new);

	// Swap an array element texture too (rd_arr_1_old -> rd_arr_1_new).
	affected.clear();
	CHECK_EQ(index.replace_texture(rd_arr_1_old, rd_arr_1_new, &affected), 1u);
	CHECK_EQ(affected.size(), 1u);
	CHECK_EQ(affected[0], &mat_a);

	// Verify #155 raster bins and #158 shading bins consumer path (get_parameter_uniforms).
	Vector<ShaderCompiler::GeneratedCode::Texture> layout;
	layout.push_back(make_tex_uniform("texture_albedo", 0));
	layout.push_back(make_tex_uniform("texture_normal", 0));
	layout.push_back(make_tex_uniform("texture_layers", 2));

	Vector<RD::Uniform> uniforms;
	CHECK(mat_a.get_parameter_uniforms(uniforms, layout, 0, true));
	REQUIRE_EQ(uniforms.size(), 3);
	CHECK_EQ(uniforms[0].binding, 1);
	CHECK_EQ(uniforms[0].get_id_count(), 1u);
	CHECK_EQ(uniforms[0].get_id(0), rd_albedo_new);
	CHECK_EQ(uniforms[1].binding, 2);
	CHECK_EQ(uniforms[1].get_id(0), rd_normal_old);
	CHECK_EQ(uniforms[2].binding, 3);
	CHECK_EQ(uniforms[2].get_id_count(), 2u);
	CHECK_EQ(uniforms[2].get_id(0), rd_arr_0);
	CHECK_EQ(uniforms[2].get_id(1), rd_arr_1_new);

	// Swap normal map and verify chained replacement updates binding 2 immediately.
	CHECK_EQ(index.replace_texture(rd_normal_old, rd_normal_new), 1u);
	uniforms.clear();
	CHECK(mat_a.get_parameter_uniforms(uniforms, layout, 0, true));
	REQUIRE_EQ(uniforms.size(), 3);
	CHECK_EQ(uniforms[1].get_id(0), rd_normal_new);

	// Unregistering cleans up all index entries cleanly.
	index.unregister_material(&mat_a, mat_a.get_texture_cache());
	index.unregister_material(&mat_b, mat_b.get_texture_cache());
	CHECK_EQ(index.get_tracked_texture_count(), 0u);
}

TEST_CASE("[MaterialStorage][TextureCache] Chained multi-swaps, no-ops, shrink-to-zero, and bounds safety") {
	RendererRD::MaterialStorage::TextureCacheIndex index;
	StubMaterialData mat;

	const RID rd_mip0 = make_rid(2000);
	const RID rd_mip1 = make_rid(2001);
	const RID rd_mip2 = make_rid(2002);
	const RID rd_mip3 = make_rid(2003);
	const RID rd_unused = make_rid(2999);

	index.set_material_textures(&mat, Vector<RID>({ rd_mip0 }));

	// No-op calls: null old RID, same RID, or untracked RID.
	CHECK_EQ(index.replace_texture(RID(), rd_mip1), 0u);
	CHECK_EQ(index.replace_texture(rd_mip0, rd_mip0), 0u);
	CHECK_EQ(index.replace_texture(rd_unused, rd_mip1), 0u);
	CHECK_EQ(mat.get_texture_cache()[0], rd_mip0);

	// Three back-to-back streaming swaps before a frame renders.
	CHECK_EQ(index.replace_texture(rd_mip0, rd_mip1), 1u);
	CHECK_EQ(index.replace_texture(rd_mip1, rd_mip2), 1u);
	CHECK_EQ(index.replace_texture(rd_mip2, rd_mip3), 1u);
	CHECK_EQ(mat.get_texture_cache()[0], rd_mip3);
	CHECK_EQ(index.get_tracked_texture_count(), 1u);
	CHECK_EQ(index.get_material_count_for_texture(rd_mip3), 1u);

	// Clearing to RID() (texture_free / non-compatible texture_replace) drops the stale RID immediately,
	// and get_parameter_uniforms() reports incomplete (false) instead of silently hiding missing slots.
	LocalVector<RendererRD::MaterialStorage::MaterialData *> affected;
	CHECK_EQ(index.replace_texture(rd_mip3, RID(), &affected), 1u);
	CHECK_EQ(affected.size(), 1u);
	CHECK_EQ(affected[0], &mat);
	CHECK(mat.get_texture_cache()[0].is_null());
	CHECK_EQ(index.get_tracked_texture_count(), 0u);

	Vector<ShaderCompiler::GeneratedCode::Texture> layout;
	layout.push_back(make_tex_uniform("tex_a", 0));
	layout.push_back(make_tex_uniform("tex_arr", 2));
	Vector<RD::Uniform> uniforms;
	CHECK_FALSE(mat.get_parameter_uniforms(uniforms, layout, 0, true));
	REQUIRE_EQ(uniforms.size(), 2);
	CHECK_EQ(uniforms[0].get_id_count(), 0u);
	CHECK_EQ(uniforms[1].get_id_count(), 0u);

	// Partially cleared texture array (slot 0 cleared, slot 1 still valid) also reports incomplete.
	index.set_material_textures(&mat, Vector<RID>({ rd_mip1, rd_mip2, rd_mip3 }));
	CHECK_EQ(index.replace_texture(rd_mip2, RID()), 1u);
	uniforms.clear();
	CHECK_FALSE(mat.get_parameter_uniforms(uniforms, layout, 0, true));
	REQUIRE_EQ(uniforms.size(), 2);
	CHECK_EQ(uniforms[0].get_id_count(), 1u);
	CHECK_EQ(uniforms[0].get_id(0), rd_mip1);
	CHECK_EQ(uniforms[1].get_id_count(), 1u);
	CHECK_EQ(uniforms[1].get_id(0), rd_mip3);

	// Shrinking texture_cache to empty leaves get_parameter_uniforms() bounds-safe and reports incomplete.
	index.set_material_textures(&mat, Vector<RID>());
	CHECK(mat.get_texture_cache().is_empty());
	uniforms.clear();
	CHECK_FALSE(mat.get_parameter_uniforms(uniforms, layout, 0, true));
	REQUIRE_EQ(uniforms.size(), 2);
	CHECK_EQ(uniforms[0].get_id_count(), 0u);
	CHECK_EQ(uniforms[1].get_id_count(), 0u);
}

TEST_CASE("[MaterialStorage][TextureCache] #130 hit-group cache diff releases bindless residency after streaming swap") {
	RendererRD::MaterialStorage::TextureCacheIndex index;
	RDBindless::Residency residency;
	StubMaterialData mat;

	// An sRGB albedo view (owned by its linear texture) plus a separate unchanging normal map.
	const RID old_linear = make_rid(3001);
	const RID old_srgb = make_rid(3002);
	const RID new_linear = make_rid(3003);
	const RID new_srgb = make_rid(3004);
	const RID normal_tex = make_rid(3010);

	index.set_material_textures(&mat, Vector<RID>({ old_srgb, normal_tex }));

	// Frame N: #130 pins old_srgb (+ its owner old_linear) and normal_tex; #145 also pins old_linear.
	Vector<RID> hit_material_textures = mat.get_texture_cache();
	residency.add(old_srgb);
	residency.add(old_linear); // pinned_owner of old_srgb in #130
	residency.add(normal_tex);
	residency.add(old_linear); // #145 shadow alpha slot

	// Streaming swap runs between frames: RD::free_rid defers both old_srgb and old_linear while pinned.
	CHECK(residency.defer_free(old_srgb));
	CHECK(residency.defer_free(old_linear));
	CHECK(residency.is_free_pending(old_srgb));
	CHECK(residency.is_free_pending(old_linear));

	// #305 refreshes MaterialData::texture_cache in place.
	CHECK_EQ(index.replace_texture(old_srgb, new_srgb), 1u);

	// Frame N+1: #130 compares its cached texture list against MaterialData::get_texture_cache().
	const Vector<RID> &current = mat.get_texture_cache();
	CHECK(hit_material_textures != current);
	CHECK_EQ(current[0], new_srgb);
	CHECK_EQ(current[1], normal_tex);

	// Acquire new slots before releasing old slots: normal_tex stays resident (refs 1->2->1), old_srgb frees.
	residency.add(new_srgb);
	residency.add(new_linear);
	residency.add(normal_tex);
	CHECK_EQ(residency.get_slot_count(normal_tex), 2u);

	CHECK(residency.remove(old_srgb)); // Last slot on old_srgb -> deferred free completes!
	CHECK_FALSE(residency.remove(old_linear)); // Still held by #145 shadow alpha slot.
	CHECK_FALSE(residency.remove(normal_tex)); // Kept alive by the material.
	CHECK_EQ(residency.get_slot_count(normal_tex), 1u);

	// #145 updates to new_linear and releases old_linear -> deferred free of old_linear completes!
	residency.add(new_linear);
	CHECK(residency.remove(old_linear));
	CHECK_FALSE(residency.is_resident(old_srgb));
	CHECK_FALSE(residency.is_resident(old_linear));
	CHECK(residency.is_resident(new_srgb));
	CHECK_EQ(residency.get_slot_count(new_linear), 2u);

	index.unregister_material(&mat, mat.get_texture_cache());
}

} // namespace TestMaterialTextureCache
