/**************************************************************************/
/*  test_nextgeo_foliage_wind.cpp                                         */
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

TEST_FORCE_LINK(test_nextgeo_foliage_wind)

#include "core/config/project_settings.h"
#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_skinning.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#ifndef _3D_DISABLED
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/3d/virtual_geometry_wind.h"
#include "scene/resources/3d/world_3d.h"
#endif

#include <cmath>
#include <cstring>

namespace {

using VMS = RendererVirtualMeshStorage;

class MockWindVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	uint32_t palette_resize_calls = 0;
	uint32_t palette_upload_calls = 0;
	uint32_t wind_params_uploads = 0;
	uint32_t wind_meshes_uploads = 0;
	WindParamsGPU last_wind_params = {};
	LocalVector<WindMeshGPU> gpu_wind_meshes;

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	bool _resize_skin_palette(uint32_t p_bytes) override {
		(void)p_bytes;
		palette_resize_calls++;
		return true;
	}
	void _upload_skin_palette(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		(void)p_size;
		palette_upload_calls++;
	}
	void _upload_wind_params(const WindParamsGPU &p_params) override {
		last_wind_params = p_params;
		wind_params_uploads++;
	}
	void _upload_wind_meshes(uint32_t p_first, uint32_t p_count) override {
		if (p_first + p_count > gpu_wind_meshes.size()) {
			gpu_wind_meshes.resize(p_first + p_count);
		}
		for (uint32_t i = 0; i < p_count; i++) {
			const WindMeshGPU *wm = get_wind_mesh(p_first + i);
			if (wm != nullptr) {
				gpu_wind_meshes[p_first + i] = *wm;
			}
		}
		wind_meshes_uploads++;
	}
	void _reset_wind() override {
		gpu_wind_meshes.clear();
		last_wind_params = WindParamsGPU();
	}
};

#ifndef _3D_DISABLED
static PackedVector3Array _make_tree_positions() {
	PackedVector3Array pts;
	// Trunk cylinder from y = 0..3.
	for (int ring = 0; ring <= 12; ring++) {
		const float y = float(ring) * 0.25f;
		for (int s = 0; s < 8; s++) {
			const float a = float(s) * float(Math::TAU) / 8.0f;
			pts.push_back(Vector3(Math::cos(a) * 0.2f, y, Math::sin(a) * 0.2f));
		}
	}
	// Canopy crown spread across 8 radial sectors from y = 2.5..6.0.
	for (int b = 0; b < 8; b++) {
		const float base_ang = float(b) * float(Math::TAU) / 8.0f;
		for (int k = 0; k < 10; k++) {
			const float t = float(k) / 9.0f;
			const float r = 0.5f + t * 2.0f;
			const float y = 2.5f + t * 3.5f + 0.15f * Math::sin(float(b) + t * 3.0f);
			const float ang = base_ang + (t - 0.5f) * 0.25f;
			pts.push_back(Vector3(Math::cos(ang) * r, y, Math::sin(ang) * r));
		}
	}
	return pts;
}
#endif

static void _extract_frustum_planes(const Projection &p_vp, float r_planes[6][4]) {
	Vector4 rows[4];
	for (int col = 0; col < 4; col++) {
		rows[0][col] = p_vp.columns[col][0];
		rows[1][col] = p_vp.columns[col][1];
		rows[2][col] = p_vp.columns[col][2];
		rows[3][col] = p_vp.columns[col][3];
	}
	const Vector4 planes[6] = {
		rows[3] + rows[0],
		rows[3] - rows[0],
		rows[3] + rows[1],
		rows[3] - rows[1],
		rows[2],
		rows[3] - rows[2],
	};
	for (int i = 0; i < 6; i++) {
		const float len = Vector3(planes[i].x, planes[i].y, planes[i].z).length();
		const float inv = len > 0.0f ? 1.0f / len : 0.0f;
		r_planes[i][0] = float(planes[i].x) * inv;
		r_planes[i][1] = float(planes[i].y) * inv;
		r_planes[i][2] = float(planes[i].z) * inv;
		r_planes[i][3] = float(planes[i].w) * inv;
	}
}

static RenderingServerTypes::VirtualGeometryDesc _make_wind_vgd_desc(uint32_t p_bones, float p_max_sway) {
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.page_count = 1;
	desc.page_size = 65536;
	desc.eligible_surface_mask = 1u;
	desc.skin_influences = 4u;
	desc.wind_max_sway = p_max_sway;
	desc.wind_base_height = 0.0f;
	desc.wind_top_height = 6.0f;
	const uint32_t count = MIN(p_bones, uint32_t(VMS::WIND_MAX_BONES));
	desc.wind_bones.resize(count);
	for (uint32_t b = 0; b < count; b++) {
		RenderingServerTypes::VirtualGeometryDesc::WindBoneDesc &bd = desc.wind_bones.write[b];
		if (b == 0) {
			bd.rest_position = Vector3(0.0f, 0.0f, 0.0f);
			bd.axis = Vector3(0.0f, 1.0f, 0.0f);
			bd.stiffness = 0.72f;
			bd.sway_phase = 0.0f;
			bd.max_sway = p_max_sway * 0.55f;
			bd.lever_length = 6.0f;
		} else {
			const float ang = float(b - 1) * float(Math::TAU) / float(MAX(count - 1, 1u));
			bd.rest_position = Vector3(Math::cos(ang) * 0.4f, 2.5f, Math::sin(ang) * 0.4f);
			bd.axis = Vector3(Math::cos(ang) * 0.8f, 0.6f, Math::sin(ang) * 0.8f).normalized();
			bd.stiffness = 0.35f;
			bd.sway_phase = ang;
			bd.max_sway = p_max_sway;
			bd.lever_length = 2.5f;
		}
	}
	return desc;
}

} // namespace

TEST_CASE("[NextGeo][FoliageWind] Project setting defaults to false") {
	REQUIRE(ProjectSettings::get_singleton() != nullptr);
	CHECK_FALSE(bool(GLOBAL_GET("rendering/nextgeo/experimental/foliage_wind")));
	CHECK_FALSE(RendererVirtualMeshStorage::is_foliage_wind_enabled());
}

#ifndef _3D_DISABLED
TEST_CASE("[NextGeo][FoliageWind] Procedural wind skeleton generation from bind-pose geometry") {
	const PackedVector3Array positions = _make_tree_positions();
	Ref<VirtualGeometryWind> wind;
	wind.instantiate();

	REQUIRE(wind->generate_from_geometry(positions, Vector3(0, 1, 0), 0.6f) == OK);
	const int bone_count = wind->get_bone_count();
	CHECK(bone_count >= 2);
	CHECK(bone_count <= VirtualGeometryWind::MAX_BONES);
	CHECK(bone_count - 1 <= VirtualGeometryWind::MAX_BRANCH_BONES);
	CHECK(Math::is_equal_approx(wind->get_max_sway(), 0.6f));
	CHECK(wind->get_top_height() > wind->get_base_height());

	const PackedVector3Array rest = wind->get_bone_rest_positions();
	const PackedVector3Array axes = wind->get_bone_axes();
	const PackedFloat32Array stiff = wind->get_bone_stiffness();
	const PackedFloat32Array sways = wind->get_bone_max_sway();
	const PackedFloat32Array levers = wind->get_bone_lever_lengths();
	REQUIRE(rest.size() == bone_count);
	REQUIRE(axes.size() == bone_count);
	REQUIRE(stiff.size() == bone_count);
	REQUIRE(sways.size() == bone_count);
	REQUIRE(levers.size() == bone_count);

	for (int b = 0; b < bone_count; b++) {
		CHECK(Math::is_equal_approx(float(axes[b].length()), 1.0f, 1e-3f));
		CHECK(stiff[b] >= 0.0f);
		CHECK(stiff[b] <= 1.0f);
		CHECK(sways[b] > 0.0f);
		CHECK(sways[b] <= wind->get_max_sway() + 1e-5f);
		CHECK(levers[b] >= 0.25f);
	}

	const Dictionary skin = wind->compute_vertex_skinning(positions);
	const PackedInt32Array bones = skin["bones"];
	const PackedFloat32Array weights = skin["weights"];
	REQUIRE(bones.size() == positions.size() * VirtualGeometryWind::SKIN_INFLUENCES);
	REQUIRE(weights.size() == positions.size() * VirtualGeometryWind::SKIN_INFLUENCES);

	bool any_branch_weight = false;
	for (int i = 0; i < positions.size(); i++) {
		float wsum = 0.0f;
		for (int k = 0; k < VirtualGeometryWind::SKIN_INFLUENCES; k++) {
			const int bi = bones[i * 4 + k];
			const float w = weights[i * 4 + k];
			CHECK(bi >= 0);
			CHECK(bi < bone_count);
			CHECK(w >= 0.0f);
			if (bi > 0 && w > 0.05f) {
				any_branch_weight = true;
			}
			wsum += w;
		}
		CHECK(Math::is_equal_approx(wsum, 1.0f, 1e-4f));
	}
	CHECK(any_branch_weight);

	// Base vertex at y == 0 should be strongly anchored to trunk bone 0.
	float base_trunk_w = 0.0f;
	for (int k = 0; k < 4; k++) {
		if (bones[k] == 0) {
			base_trunk_w += weights[k];
		}
	}
	CHECK(base_trunk_w > 0.8f);
}

TEST_CASE("[NextGeo][FoliageWind] Procedural wind skeleton generation from glTF skeleton clamps to <= 8 branches") {
	const PackedVector3Array positions = _make_tree_positions();
	// Author a 14-bone glTF-style tree skeleton (root + 2 trunk bones + 11 branch bones).
	constexpr int SRC_BONES = 14;
	PackedVector3Array src_rest;
	PackedInt32Array src_parents;
	src_rest.resize(SRC_BONES);
	src_parents.resize(SRC_BONES);
	src_rest.write[0] = Vector3(0.0f, 0.0f, 0.0f);
	src_parents.write[0] = -1;
	src_rest.write[1] = Vector3(0.0f, 1.5f, 0.0f);
	src_parents.write[1] = 0;
	src_rest.write[2] = Vector3(0.0f, 3.0f, 0.0f);
	src_parents.write[2] = 1;
	for (int b = 3; b < SRC_BONES; b++) {
		const float ang = float(b - 3) * float(Math::TAU) / float(SRC_BONES - 3);
		src_rest.write[b] = Vector3(Math::cos(ang) * 1.1f, 3.2f + float(b % 3) * 0.6f, Math::sin(ang) * 1.1f);
		src_parents.write[b] = 2;
	}

	PackedInt32Array src_bones;
	PackedFloat32Array src_weights;
	src_bones.resize(positions.size() * 4);
	src_weights.resize(positions.size() * 4);
	for (int i = 0; i < positions.size(); i++) {
		const Vector3 p = positions[i];
		int nearest = 0;
		float best_d2 = FLT_MAX;
		for (int b = 0; b < SRC_BONES; b++) {
			const float d2 = float(p.distance_squared_to(src_rest[b]));
			if (d2 < best_d2) {
				best_d2 = d2;
				nearest = b;
			}
		}
		src_bones.write[i * 4 + 0] = nearest;
		src_bones.write[i * 4 + 1] = src_parents[nearest] >= 0 ? src_parents[nearest] : 0;
		src_bones.write[i * 4 + 2] = 0;
		src_bones.write[i * 4 + 3] = 0;
		src_weights.write[i * 4 + 0] = 0.75f;
		src_weights.write[i * 4 + 1] = 0.25f;
		src_weights.write[i * 4 + 2] = 0.0f;
		src_weights.write[i * 4 + 3] = 0.0f;
	}

	Ref<VirtualGeometryWind> wind;
	wind.instantiate();
	PackedInt32Array remap;
	REQUIRE(wind->generate_from_skeleton(src_rest, src_parents, positions, src_bones, src_weights, 4, remap, 0.75f) == OK);
	CHECK(wind->get_bone_count() == VirtualGeometryWind::MAX_BONES);
	CHECK(wind->get_bone_count() - 1 <= VirtualGeometryWind::MAX_BRANCH_BONES);
	REQUIRE(remap.size() == SRC_BONES);
	CHECK(remap[0] == 0);
	for (int b = 0; b < SRC_BONES; b++) {
		CHECK(remap[b] >= 0);
		CHECK(remap[b] < wind->get_bone_count());
	}

	PackedInt32Array dst_bones;
	PackedFloat32Array dst_weights;
	VirtualGeometryWind::remap_vertex_skinning(src_bones, src_weights, 4, remap, wind->get_bone_count(), dst_bones, dst_weights);
	REQUIRE(dst_bones.size() == positions.size() * 4);
	REQUIRE(dst_weights.size() == positions.size() * 4);
	for (int i = 0; i < positions.size(); i++) {
		float sum = 0.0f;
		for (int k = 0; k < 4; k++) {
			CHECK(dst_bones[i * 4 + k] >= 0);
			CHECK(dst_bones[i * 4 + k] < wind->get_bone_count());
			sum += dst_weights[i * 4 + k];
		}
		CHECK(Math::is_equal_approx(sum, 1.0f, 1e-4f));
	}

	// Verify VirtualGeometryData attaches the wind resource cleanly.
	Ref<VirtualGeometryData> vgd;
	vgd.instantiate();
	vgd->set_wind(wind);
	CHECK(vgd->get_wind() == wind);
}

TEST_CASE("[SceneTree][NextGeo][FoliageWind] World3D wind properties and RenderingServer scenario_set_wind") {
	Ref<World3D> world;
	world.instantiate();
	CHECK(world->get_wind_direction().is_equal_approx(Vector3(1.0f, 0.0f, 0.0f)));
	CHECK(Math::is_equal_approx(world->get_wind_strength(), 0.0f));
	CHECK(Math::is_equal_approx(world->get_wind_gust_frequency(), 1.0f));

	world->set_wind_direction(Vector3(0.0f, 0.0f, 2.0f));
	world->set_wind_strength(2.5f);
	world->set_wind_gust_frequency(1.75f);
	CHECK(world->get_wind_direction().is_equal_approx(Vector3(0.0f, 0.0f, 2.0f)));
	CHECK(Math::is_equal_approx(world->get_wind_strength(), 2.5f));
	CHECK(Math::is_equal_approx(world->get_wind_gust_frequency(), 1.75f));

	// Clamping edge cases.
	world->set_wind_strength(-3.0f);
	CHECK(Math::is_equal_approx(world->get_wind_strength(), 0.0f));
	world->set_wind_gust_frequency(-1.0f);
	CHECK(Math::is_equal_approx(world->get_wind_gust_frequency(), 0.0f));

	// Direct RenderingServer call works cleanly.
	if (RenderingServer::get_singleton() != nullptr && world->get_scenario().is_valid()) {
		RenderingServer::get_singleton()->scenario_set_wind(world->get_scenario(), Vector3(1.0f, 0.0f, 1.0f), 1.5f, 2.0f);
	}
}
#endif

TEST_CASE("[NextGeo][FoliageWind] Zero per-instance skin palette arena memory for 100+ procedural wind instances") {
	MockWindVirtualMeshStorage storage;

	const RID mesh_rid = RID::from_uint64(101);
	const RenderingServerTypes::VirtualGeometryDesc desc = _make_wind_vgd_desc(7, 0.5f);
	storage.mesh_set_virtual_geometry(mesh_rid, desc);
	REQUIRE(storage.mesh_has_virtual_geometry(mesh_rid));
	const VMS::MeshInfo *minfo = storage.get_meshes().getptr(mesh_rid);
	REQUIRE(minfo != nullptr);
	const uint32_t mesh_idx = minfo->mesh_index;
	REQUIRE(mesh_idx != UINT32_MAX);
	CHECK(storage.mesh_has_wind(mesh_rid));
	CHECK(storage.mesh_index_has_wind(mesh_idx));
	CHECK(Math::is_equal_approx(storage.mesh_get_wind_max_sway(mesh_rid), 0.5f));
	CHECK(Math::is_equal_approx(storage.mesh_index_get_wind_max_sway(mesh_idx), 0.5f));

	// Allocate 64 standalone instances (including 8 that also pass a skeleton RID) + 128 MultiMesh instances = 192 total.
	LocalVector<uint32_t> records;
	for (uint32_t i = 0; i < 64; i++) {
		const uint32_t rec = storage.instance_record_allocate(RID::from_uint64(1000 + i), mesh_rid);
		REQUIRE(rec != UINT32_MAX);
		storage.instance_record_set_transform(rec, Transform3D(Basis(), Vector3(float(i) * 2.0f, 0.0f, 0.0f)), AABB(Vector3(-1, 0, -1), Vector3(2, 6, 2)), true);
		storage.instance_record_set_state(rec, 0u);
		if (i < 8) {
			// Even if a Skeleton3D RID is provided on a wind asset, wind instances must allocate 0 palette bytes.
			storage.instance_record_set_skeleton(rec, RID::from_uint64(9000 + i));
		}
		records.push_back(rec);
	}

	const uint32_t mm_parent = storage.instance_record_allocate(RID::from_uint64(5000), mesh_rid);
	REQUIRE(mm_parent != UINT32_MAX);
	storage.instance_record_set_transform(mm_parent, Transform3D(), AABB(Vector3(-50, 0, -50), Vector3(100, 10, 100)), true);
	storage.instance_record_set_state(mm_parent, VMS::RECORD_MULTIMESH_PARENT);
	const uint32_t mm_base = storage.multimesh_range_attach(mm_parent, RID::from_uint64(5001), mesh_rid, 128);
	REQUIRE(mm_base != UINT32_MAX);

	storage.set_wind_params(Vector3(1.0f, 0.0f, 0.0f), 1.8f, 1.25f, 3.5f, 3.4833f, true);
	storage.flush();

	// Critical acceptance invariant: 0 bytes of per-instance skin palette arena memory!
	CHECK(storage.get_palette_arena_used_bones() == 0u);
	CHECK(storage.get_palette_arena_bone_capacity() == 0u);
	CHECK(storage.palette_resize_calls == 0u);
	CHECK(storage.palette_upload_calls == 0u);
	CHECK(storage.get_skin_palette_buffer().is_null());

	// Per-mesh wind descriptor and per-scenario wind params uploaded cleanly.
	CHECK(storage.wind_params_uploads > 0u);
	CHECK(storage.wind_meshes_uploads > 0u);
	REQUIRE(mesh_idx < storage.gpu_wind_meshes.size());
	CHECK(storage.gpu_wind_meshes[mesh_idx].bone_count == 7u);
	CHECK(Math::is_equal_approx(storage.gpu_wind_meshes[mesh_idx].max_sway, 0.5f));
	CHECK(storage.last_wind_params.mesh_count == storage.gpu_wind_meshes.size());
	CHECK((storage.last_wind_params.flags & VMS::WIND_PARAM_FLAG_ENABLED) != 0u);

	for (uint32_t rec : records) {
		CHECK((storage.get_record(rec).flags & VMS::RECORD_SKINNED_PALETTE) == 0u);
	}

	storage.multimesh_range_detach(mm_parent);
	storage.instance_record_free(mm_parent);
	for (uint32_t rec : records) {
		storage.instance_record_free(rec);
	}
	storage.mesh_clear_virtual_geometry(mesh_rid, 0u);
}

TEST_CASE("[NextGeo][FoliageWind] Procedural bone evaluation, sway bound, velocity delta, and spatial phase") {
	MockWindVirtualMeshStorage storage;
	const RID mesh_rid = RID::from_uint64(202);
	const float declared_max_sway = 0.55f;
	storage.mesh_set_virtual_geometry(mesh_rid, _make_wind_vgd_desc(5, declared_max_sway));
	const VMS::MeshInfo *minfo = storage.get_meshes().getptr(mesh_rid);
	REQUIRE(minfo != nullptr);
	const uint32_t mesh_idx = minfo->mesh_index;
	REQUIRE(mesh_idx != UINT32_MAX);
	storage.set_wind_params(Vector3(1.0f, 0.0f, 0.0f), 2.0f, 1.5f, 1.25f, 1.2333f, true);
	storage.flush();

	const VMS::WindMeshGPU *wm_ptr = storage.get_wind_mesh(mesh_idx);
	REQUIRE(wm_ptr != nullptr);
	const VMS::WindMeshGPU &wm = *wm_ptr;
	const VMS::WindParamsGPU params_on = storage.last_wind_params;
	VMS::WindParamsGPU params_off = params_on;
	params_off.flags = 0u;
	VMS::WindParamsGPU params_zero = params_on;
	params_zero.strength = 0.0f;

	const float xform_a[3][4] = {
		{ 1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f },
	};
	const float xform_b[3][4] = {
		{ 1.0f, 0.0f, 0.0f, 7.5f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, -4.2f },
	};

	const Vector3 canopy_pt(1.2f, 4.8f, 0.3f);
	const Vector3 bind_nrm(0.0f, 0.0f, 1.0f);
	const Vector3 bind_tan(1.0f, 0.0f, 0.0f);
	// Build a valid #159 cluster skinning block for 1 vertex influenced by wind bones {0, 1, 2} with weights {0.35, 0.45, 0.20}.
	Vector<uint8_t> skin_block;
	const uint32_t sb_bones = 3u;
	const uint32_t sb_inf = 4u;
	const uint32_t sb_verts = 1u;
	skin_block.resize(4u + sb_bones * 26u + sb_verts * sb_inf + sb_verts * sb_inf * 2u);
	memset(skin_block.ptrw(), 0, skin_block.size());
	skin_block.write[0] = uint8_t(sb_bones);
	skin_block.write[1] = uint8_t(sb_inf);
	for (uint32_t b = 0; b < sb_bones; b++) {
		const uint16_t gb = uint16_t(b);
		memcpy(skin_block.ptrw() + 4u + b * 26u, &gb, sizeof(uint16_t));
	}
	const uint32_t idx_off = 4u + sb_bones * 26u;
	skin_block.write[idx_off + 0] = 0u;
	skin_block.write[idx_off + 1] = 1u;
	skin_block.write[idx_off + 2] = 2u;
	skin_block.write[idx_off + 3] = 0u;
	const uint32_t w_off = idx_off + sb_verts * sb_inf;
	const uint16_t w_u16[4] = { 22937u, 29491u, 13107u, 0u };
	memcpy(skin_block.ptrw() + w_off, w_u16, sizeof(w_u16));

	// Disabled or strength == 0 -> exact identity (0 displacement).
	Vector3 p_off = canopy_pt, n_off = bind_nrm, t_off = bind_tan;
	CHECK_FALSE(RendererRD::nextgeo_wind_eval_vertex_lbs_raw(skin_block.ptr(), skin_block.size(), 0u, sb_verts, 0u, params_off, wm, xform_a, false, p_off, &n_off, &t_off));
	CHECK(p_off.is_equal_approx(canopy_pt));

	Vector3 p_zero = canopy_pt, n_zero = bind_nrm, t_zero = bind_tan;
	CHECK_FALSE(RendererRD::nextgeo_wind_eval_vertex_lbs_raw(skin_block.ptr(), skin_block.size(), 0u, sb_verts, 0u, params_zero, wm, xform_a, false, p_zero, &n_zero, &t_zero));
	CHECK(p_zero.is_equal_approx(canopy_pt));

	// Enabled -> non-zero displacement, distinct prev vs cur for velocity, and distinct spatial phase across instances.
	Vector3 p_cur_a = canopy_pt, n_cur_a = bind_nrm, t_cur_a = bind_tan;
	CHECK(RendererRD::nextgeo_wind_eval_vertex_lbs_raw(skin_block.ptr(), skin_block.size(), 0u, sb_verts, 0u, params_on, wm, xform_a, false, p_cur_a, &n_cur_a, &t_cur_a));
	CHECK(p_cur_a.distance_to(canopy_pt) > 0.01f);
	CHECK(Math::is_equal_approx(float(n_cur_a.length()), 1.0f, 1e-4f));
	CHECK(Math::is_equal_approx(float(t_cur_a.length()), 1.0f, 1e-4f));

	Vector3 p_prev_a = canopy_pt, n_prev_a = bind_nrm, t_prev_a = bind_tan;
	CHECK(RendererRD::nextgeo_wind_eval_vertex_lbs_raw(skin_block.ptr(), skin_block.size(), 0u, sb_verts, 0u, params_on, wm, xform_a, true, p_prev_a, &n_prev_a, &t_prev_a));
	CHECK(p_cur_a.distance_to(p_prev_a) > 1e-4f);

	Vector3 p_cur_b = canopy_pt, n_cur_b = bind_nrm, t_cur_b = bind_tan;
	CHECK(RendererRD::nextgeo_wind_eval_vertex_lbs_raw(skin_block.ptr(), skin_block.size(), 0u, sb_verts, 0u, params_on, wm, xform_b, false, p_cur_b, &n_cur_b, &t_cur_b));
	CHECK(p_cur_a.distance_to(p_cur_b) > 1e-4f);

	// Extreme wind strength still respects conservative max_sway bound for vertices within lever length.
	VMS::WindParamsGPU params_gale = params_on;
	params_gale.strength = 1000.0f;
	for (uint32_t b = 0; b < wm.bone_count; b++) {
		const Vector3 pivot(wm.bones[b].rest_pos[0], wm.bones[b].rest_pos[1], wm.bones[b].rest_pos[2]);
		const Vector3 axis(wm.bones[b].axis[0], wm.bones[b].axis[1], wm.bones[b].axis[2]);
		const float lever = wm.bones[b].lever_length;
		const Vector3 tip = pivot + axis.normalized() * lever;
		float rows[12];
		RendererRD::nextgeo_wind_eval_bone_matrix(params_gale, wm, b, xform_a, false, rows);
		const Vector3 moved(
				rows[0] * float(tip.x) + rows[1] * float(tip.y) + rows[2] * float(tip.z) + rows[3],
				rows[4] * float(tip.x) + rows[5] * float(tip.y) + rows[6] * float(tip.z) + rows[7],
				rows[8] * float(tip.x) + rows[9] * float(tip.y) + rows[10] * float(tip.z) + rows[11]);
		CHECK(moved.distance_to(tip) <= declared_max_sway + 1e-3f);
		// Pivot itself does not move under pure rotation about pivot.
		const Vector3 moved_pivot(
				rows[0] * float(pivot.x) + rows[1] * float(pivot.y) + rows[2] * float(pivot.z) + rows[3],
				rows[4] * float(pivot.x) + rows[5] * float(pivot.y) + rows[6] * float(pivot.z) + rows[7],
				rows[8] * float(pivot.x) + rows[9] * float(pivot.y) + rows[10] * float(pivot.z) + rows[11]);
		CHECK(moved_pivot.is_equal_approx(pivot));
	}

	storage.mesh_clear_virtual_geometry(mesh_rid, 0u);
}

TEST_CASE("[NextGeo][FoliageWind] Cull bounds inflation and VSM dynamic layer routing") {
	// Camera at origin looking down -Z with 90 deg FOV -> left frustum plane is x = -z.
	const Projection proj = Projection::create_perspective(90.0, 1.0, 0.1, 100.0);
	float frustum[6][4];
	_extract_frustum_planes(proj, frustum);

	VMS::Record rec = {};
	rec.flags = VMS::RECORD_VALID | VMS::RECORD_VISIBLE;
	rec.virtual_mesh = 0u;
	rec.layer_mask = 1u;
	rec.surface_mask = 1u;
	rec.transform[0][0] = 1.0f;
	rec.transform[1][1] = 1.0f;
	rec.transform[2][2] = 1.0f;
	// At z = -10.5 (farthest corner of box), frustum left edge is x = -10.5. Place a unit-width box at
	// x in [-11.85, -10.85], which is 0.35m outside the frustum without wind inflation, but inside with wind_max_sway = 0.60m!
	rec.world_aabb_min[0] = -11.85f;
	rec.world_aabb_min[1] = -0.5f;
	rec.world_aabb_min[2] = -10.5f;
	rec.world_aabb_min[3] = 1.0f; // scale
	rec.world_aabb_max[0] = -10.85f;
	rec.world_aabb_max[1] = 0.5f;
	rec.world_aabb_max[2] = -9.5f;
	rec.world_aabb_max[3] = 0.0f;

	VMS::InstanceAttributes attr = {};
	CHECK_FALSE(VMS::cull_instance_cpu(rec, attr, nullptr, frustum, Vector3(), 1u, false, 0.0f));
	CHECK(VMS::cull_instance_cpu(rec, attr, nullptr, frustum, Vector3(), 1u, false, 0.6f));

	// Helper AABB inflation scales by instance scale.
	const AABB base_aabb(Vector3(-1, 0, -1), Vector3(2, 4, 2));
	const AABB inflated = RendererRD::nextgeo_wind_inflate_aabb(base_aabb, 0.5f, 2.0f);
	CHECK(inflated.position.is_equal_approx(Vector3(-2, -1, -2)));
	CHECK(inflated.size.is_equal_approx(Vector3(4, 6, 4)));

	// VSM dynamic cache classification & layer routing.
	RendererVirtualShadowMapStorage::CacheClassificationInput in;
	in.mode = RSE::SHADOW_CACHE_INVALIDATION_AUTO;
	in.has_foliage_wind = false;
	const uint32_t static_mask = RendererVirtualShadowMapStorage::classify_shadow_cache_dynamic(in);
	CHECK(static_mask == 0u);
	CHECK((static_mask & RendererVirtualShadowMapStorage::CACHE_DYNAMIC_FOLIAGE_WIND) == 0u);

	in.has_foliage_wind = true;
	const uint32_t wind_mask = RendererVirtualShadowMapStorage::classify_shadow_cache_dynamic(in);
	CHECK(wind_mask != 0u);
	CHECK((wind_mask & RendererVirtualShadowMapStorage::CACHE_DYNAMIC_FOLIAGE_WIND) != 0u);

	CHECK(RendererRD::nextgeo_vsm_instance_layer(VMS::RECORD_VALID, false, RendererRD::NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK, false, false) == 0u);
	CHECK(RendererRD::nextgeo_vsm_instance_layer(VMS::RECORD_VALID, false, RendererRD::NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK, false, true) == 1u);
	CHECK(RendererRD::nextgeo_vsm_instance_layer(VMS::RECORD_VALID | VMS::RECORD_VSM_CACHE_STATIC, false, RendererRD::NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK, false, true) == 0u);
}
