/**************************************************************************/
/*  test_nextgeo_dynamic_wind.cpp                                         */
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

TEST_FORCE_LINK(test_nextgeo_dynamic_wind)

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_dynamic_wind.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#ifndef _3D_DISABLED
#include "scene/resources/3d/virtual_geometry_wind.h"
#include "scene/resources/3d/world_3d.h"
#endif

#include <cmath>
#include <cstring>

namespace {

using VMS = RendererVirtualMeshStorage;

class MockDynamicWindVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	uint32_t palette_resize_calls = 0;
	uint32_t palette_upload_calls = 0;
	uint32_t wind_params_uploads = 0;
	uint32_t wind_meshes_uploads = 0;
	bool mock_device_tier_supported = true;
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
	bool _query_device_dynamic_wind_tier() const override {
		return mock_device_tier_supported;
	}
};

struct ScopedDynamicWindSettings {
	Variant orig_dw;
	Variant orig_mode;
	Variant orig_fw;

	ScopedDynamicWindSettings() {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (ps != nullptr) {
			orig_dw = ps->get_setting("rendering/nextgeo/experimental/dynamic_wind");
			orig_mode = ps->get_setting("rendering/nextgeo/experimental/dynamic_wind_mode");
			orig_fw = ps->get_setting("rendering/nextgeo/experimental/foliage_wind");
		}
	}
	~ScopedDynamicWindSettings() {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (ps != nullptr) {
			ps->set_setting("rendering/nextgeo/experimental/dynamic_wind", orig_dw);
			ps->set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", orig_mode);
			ps->set_setting("rendering/nextgeo/experimental/foliage_wind", orig_fw);
		}
	}
};

static RenderingServerTypes::VirtualGeometryDesc _make_dynamic_wind_vgd_desc(uint32_t p_bones, float p_max_sway) {
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.page_count = 1;
	desc.page_size = 65536;
	desc.eligible_surface_mask = 1u;
	desc.skin_influences = 4u;
	desc.wind_max_sway = p_max_sway;
	desc.wind_base_height = 0.0f;
	desc.wind_top_height = 5.0f;
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
			bd.lever_length = 5.0f;
			bd.mass = 2.4f;
			bd.wind_catch = 0.75f;
		} else {
			const float ang = float(b - 1) * float(Math::TAU) / float(MAX(count - 1, 1u));
			bd.rest_position = Vector3(Math::cos(ang) * 0.4f, 2.2f, Math::sin(ang) * 0.4f);
			bd.axis = Vector3(Math::cos(ang) * 0.8f, 0.6f, Math::sin(ang) * 0.8f).normalized();
			bd.stiffness = 0.35f;
			bd.sway_phase = ang;
			bd.max_sway = p_max_sway;
			bd.lever_length = 2.2f;
			bd.mass = 0.8f;
			bd.wind_catch = 1.6f;
		}
	}
	return desc;
}

static Vector3 _transform_point_3x4(const float p_rows[12], const Vector3 &p_pt) {
	return Vector3(
			p_rows[0] * float(p_pt.x) + p_rows[1] * float(p_pt.y) + p_rows[2] * float(p_pt.z) + p_rows[3],
			p_rows[4] * float(p_pt.x) + p_rows[5] * float(p_pt.y) + p_rows[6] * float(p_pt.z) + p_rows[7],
			p_rows[8] * float(p_pt.x) + p_rows[9] * float(p_pt.y) + p_rows[10] * float(p_pt.z) + p_rows[11]);
}

#ifndef _3D_DISABLED
static void _make_triangulated_foliage_mesh(PackedVector3Array &r_positions, PackedInt32Array &r_indices) {
	r_positions.clear();
	r_indices.clear();

	// Narrow cylindrical trunk (y = 0..3, radius = 0.18).
	constexpr int TRUNK_RINGS = 8;
	constexpr int TRUNK_SEGS = 8;
	for (int r = 0; r <= TRUNK_RINGS; r++) {
		const float y = float(r) * (3.0f / float(TRUNK_RINGS));
		for (int s = 0; s < TRUNK_SEGS; s++) {
			const float a = float(s) * float(Math::TAU) / float(TRUNK_SEGS);
			r_positions.push_back(Vector3(Math::cos(a) * 0.18f, y, Math::sin(a) * 0.18f));
		}
	}
	for (int r = 0; r < TRUNK_RINGS; r++) {
		for (int s = 0; s < TRUNK_SEGS; s++) {
			const int s_next = (s + 1) % TRUNK_SEGS;
			const int i0 = r * TRUNK_SEGS + s;
			const int i1 = r * TRUNK_SEGS + s_next;
			const int i2 = (r + 1) * TRUNK_SEGS + s;
			const int i3 = (r + 1) * TRUNK_SEGS + s_next;
			r_indices.push_back(i0);
			r_indices.push_back(i2);
			r_indices.push_back(i1);
			r_indices.push_back(i1);
			r_indices.push_back(i2);
			r_indices.push_back(i3);
		}
	}

	// Four asymmetric foliage cards at distinct radial quadrants around y = 3..5.5.
	// Sector 0 (+X) has a large broad leaf sail; Sector 2 (-X) has a much smaller twig card.
	const float card_scales[4] = { 2.4f, 1.3f, 0.45f, 1.1f };
	for (int q = 0; q < 4; q++) {
		const float ang = float(q) * float(Math::TAU) / 4.0f;
		const Vector3 radial(Math::cos(ang), 0.0f, Math::sin(ang));
		const Vector3 side(-Math::sin(ang), 0.0f, Math::cos(ang));
		const float sc = card_scales[q];
		const Vector3 center = radial * (0.9f + 0.5f * sc) + Vector3(0.0f, 3.6f + 0.2f * float(q % 2), 0.0f);
		const int base = r_positions.size();
		r_positions.push_back(center - side * (0.8f * sc) - Vector3(0.0f, 0.7f * sc, 0.0f));
		r_positions.push_back(center + side * (0.8f * sc) - Vector3(0.0f, 0.7f * sc, 0.0f));
		r_positions.push_back(center + side * (0.8f * sc) + Vector3(0.0f, 0.7f * sc, 0.0f));
		r_positions.push_back(center - side * (0.8f * sc) + Vector3(0.0f, 0.7f * sc, 0.0f));
		r_indices.push_back(base + 0);
		r_indices.push_back(base + 1);
		r_indices.push_back(base + 2);
		r_indices.push_back(base + 0);
		r_indices.push_back(base + 2);
		r_indices.push_back(base + 3);
	}
}
#endif

} // namespace

TEST_CASE("[NextGeo][DynamicWind] Experimental project settings default to false and Force Based mode") {
	REQUIRE(ProjectSettings::get_singleton() != nullptr);
	CHECK_FALSE(bool(GLOBAL_GET("rendering/nextgeo/experimental/dynamic_wind")));
	CHECK(int(GLOBAL_GET("rendering/nextgeo/experimental/dynamic_wind_mode")) == 0);
	CHECK_FALSE(RendererVirtualMeshStorage::is_dynamic_wind_enabled());
	CHECK(RendererVirtualMeshStorage::get_dynamic_wind_mode() == RendererVirtualMeshStorage::DYNAMIC_WIND_MODE_FORCE_BASED);

	// Verify enum metadata is registered for Force Based / Parametric selection.
	const Dictionary hint_info = ProjectSettings::get_singleton()->get_custom_property_info().has("rendering/nextgeo/experimental/dynamic_wind_mode")
			? Dictionary(ProjectSettings::get_singleton()->get_custom_property_info()["rendering/nextgeo/experimental/dynamic_wind_mode"])
			: Dictionary();
	if (!hint_info.is_empty()) {
		CHECK(String(hint_info.get("hint_string", "")) == "Force Based,Parametric");
	}
}

TEST_CASE("[NextGeo][DynamicWind] Hardware and feature floor evaluation and one-time session logging") {
	using RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR;
	using RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_DISABLED;
	using RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_FOLIAGE_UNAVAILABLE;
	using RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_NEXTGEO_UNAVAILABLE;
	using RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_NONE;
	using RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_SKINNED_UNAVAILABLE;

	const RendererRD::NextGeoDynamicWindFloor f_off = RendererRD::nextgeo_dynamic_wind_evaluate_floor(false, true, true, true, true);
	CHECK_FALSE(f_off.supported);
	CHECK(f_off.reason == NEXTGEO_DYNAMIC_WIND_REASON_DISABLED);
	CHECK(strcmp(RendererRD::nextgeo_dynamic_wind_reason_name(f_off.reason), "DISABLED") == 0);

	const RendererRD::NextGeoDynamicWindFloor f_ng = RendererRD::nextgeo_dynamic_wind_evaluate_floor(true, false, true, true, true);
	CHECK_FALSE(f_ng.supported);
	CHECK(f_ng.reason == NEXTGEO_DYNAMIC_WIND_REASON_NEXTGEO_UNAVAILABLE);
	CHECK(strcmp(RendererRD::nextgeo_dynamic_wind_reason_name(f_ng.reason), "NEXTGEO_UNAVAILABLE") == 0);

	const RendererRD::NextGeoDynamicWindFloor f_tier = RendererRD::nextgeo_dynamic_wind_evaluate_floor(true, true, false, true, true);
	CHECK_FALSE(f_tier.supported);
	CHECK(f_tier.reason == NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR);
	CHECK(strcmp(RendererRD::nextgeo_dynamic_wind_reason_name(f_tier.reason), "BELOW_TIER_FLOOR") == 0);

	const RendererRD::NextGeoDynamicWindFloor f_skin = RendererRD::nextgeo_dynamic_wind_evaluate_floor(true, true, true, false, true);
	CHECK_FALSE(f_skin.supported);
	CHECK(f_skin.reason == NEXTGEO_DYNAMIC_WIND_REASON_SKINNED_UNAVAILABLE);
	CHECK(strcmp(RendererRD::nextgeo_dynamic_wind_reason_name(f_skin.reason), "SKINNED_UNAVAILABLE") == 0);

	const RendererRD::NextGeoDynamicWindFloor f_fol = RendererRD::nextgeo_dynamic_wind_evaluate_floor(true, true, true, true, false);
	CHECK_FALSE(f_fol.supported);
	CHECK(f_fol.reason == NEXTGEO_DYNAMIC_WIND_REASON_FOLIAGE_UNAVAILABLE);
	CHECK(strcmp(RendererRD::nextgeo_dynamic_wind_reason_name(f_fol.reason), "FOLIAGE_UNAVAILABLE") == 0);

	const RendererRD::NextGeoDynamicWindFloor f_ok = RendererRD::nextgeo_dynamic_wind_evaluate_floor(true, true, true, true, true);
	CHECK(f_ok.supported);
	CHECK(f_ok.reason == NEXTGEO_DYNAMIC_WIND_REASON_NONE);
	CHECK(strcmp(RendererRD::nextgeo_dynamic_wind_reason_name(f_ok.reason), "NONE") == 0);

	// One-time reason logging per session.
	RendererRD::NextGeoDynamicWindFloorLogger logger;
	CHECK_FALSE(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_NONE, false));
	CHECK_FALSE(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_DISABLED, false));
	CHECK(logger.get_log_count() == 0u);

	CHECK(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR, false));
	CHECK(logger.get_log_count() == 1u);
	CHECK(logger.has_logged(NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR));
	CHECK(logger.get_last_reason() == NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR);

	// Repeated calls with the same reason do NOT re-log.
	CHECK_FALSE(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR, false));
	CHECK_FALSE(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR, false));
	CHECK(logger.get_log_count() == 1u);

	// A distinct floor reason logs once more.
	CHECK(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_SKINNED_UNAVAILABLE, false));
	CHECK(logger.get_log_count() == 2u);
	CHECK_FALSE(logger.record_and_log_once(NEXTGEO_DYNAMIC_WIND_REASON_SKINNED_UNAVAILABLE, false));
	CHECK(logger.get_log_count() == 2u);

	// Below tier floor: even with dynamic_wind setting enabled, plants do not move and 0 slots/palette bones allocate.
	ScopedDynamicWindSettings scoped;
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/dynamic_wind", true);
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 0);
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/foliage_wind", false);

	MockDynamicWindVirtualMeshStorage storage;
	storage.mock_device_tier_supported = false;
	storage.set_dynamic_wind_floor_state(false, uint32_t(NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR));

	const RID mesh_rid = RID::from_uint64(301);
	storage.mesh_set_virtual_geometry(mesh_rid, _make_dynamic_wind_vgd_desc(5, 0.5f));
	const uint32_t rec = storage.instance_record_allocate(RID::from_uint64(302), mesh_rid);
	REQUIRE(rec != UINT32_MAX);
	storage.instance_record_set_transform(rec, Transform3D(), AABB(Vector3(-1, 0, -1), Vector3(2, 5, 2)), true);

	CHECK_FALSE(storage.is_dynamic_wind_floor_supported());
	CHECK_FALSE(storage.is_dynamic_wind_active());
	CHECK_FALSE(storage.is_dynamic_wind_force_based_active());
	CHECK_FALSE(storage.is_dynamic_wind_parametric_active());
	CHECK_FALSE(storage.is_any_wind_active());

	storage.update_dynamic_wind(Vector3(1.0f, 0.0f, 0.0f), 4.0f, 1.5f, 1.0f, 1.0f / 60.0f, nullptr, 0u, storage.is_dynamic_wind_force_based_active());
	storage.flush();

	const VMS::DynamicWindStats stats = storage.get_dynamic_wind_stats();
	CHECK(stats.setting_enabled);
	CHECK_FALSE(stats.floor_supported);
	CHECK(stats.floor_reason == uint32_t(NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR));
	CHECK(stats.active_slots == 0u);
	CHECK(stats.shared_slots == 0u);
	CHECK(stats.promoted_instances == 0u);
	CHECK(storage.get_palette_arena_used_bones() == 0u);
	CHECK(storage.palette_resize_calls == 0u);

	storage.instance_record_free(rec);
	storage.mesh_clear_virtual_geometry(mesh_rid, 0u);
}

#ifndef _3D_DISABLED
TEST_CASE("[NextGeo][DynamicWind] Import-time per-bone mass and wind catch computation from bind-pose geometry") {
	PackedVector3Array positions;
	PackedInt32Array indices;
	_make_triangulated_foliage_mesh(positions, indices);

	Ref<VirtualGeometryWind> wind;
	wind.instantiate();
	REQUIRE(wind->generate_from_geometry(positions, Vector3(0.0f, 1.0f, 0.0f), 0.6f) == OK);
	const int bone_count = wind->get_bone_count();
	REQUIRE(bone_count >= 5);

	// Point-cloud fallback populated during generate_from_geometry produces valid positive physics.
	const PackedFloat32Array pc_masses = wind->get_bone_masses();
	const PackedFloat32Array pc_catches = wind->get_bone_wind_catches();
	REQUIRE(pc_masses.size() == bone_count);
	REQUIRE(pc_catches.size() == bone_count);
	for (int b = 0; b < bone_count; b++) {
		CHECK(std::isfinite(pc_masses[b]));
		CHECK(std::isfinite(pc_catches[b]));
		CHECK(pc_masses[b] >= 0.05f);
		CHECK(pc_catches[b] >= 0.01f);
	}

	// Triangle-area pass computes physical mass and aerodynamic wind catch per bone.
	const Dictionary skin = wind->compute_vertex_skinning(positions);
	const PackedInt32Array bones = skin["bones"];
	const PackedFloat32Array weights = skin["weights"];
	wind->compute_bone_physics_from_geometry(positions, indices, bones, weights, VirtualGeometryWind::SKIN_INFLUENCES);

	const PackedFloat32Array masses = wind->get_bone_masses();
	const PackedFloat32Array catches = wind->get_bone_wind_catches();
	REQUIRE(masses.size() == bone_count);
	REQUIRE(catches.size() == bone_count);
	for (int b = 0; b < bone_count; b++) {
		CHECK(std::isfinite(masses[b]));
		CHECK(std::isfinite(catches[b]));
		CHECK(masses[b] >= 0.05f);
		CHECK(catches[b] >= 0.01f);
	}

	// Identify the branch bone closest to +X (broad 2.4x sail) vs -X (small 0.45x twig).
	const PackedVector3Array axes = wind->get_bone_axes();
	int broad_branch = -1;
	int small_branch = -1;
	float best_pos_x = -FLT_MAX;
	float best_neg_x = -FLT_MAX;
	for (int b = 1; b < bone_count; b++) {
		if (axes[b].x > best_pos_x) {
			best_pos_x = float(axes[b].x);
			broad_branch = b;
		}
		if (-axes[b].x > best_neg_x) {
			best_neg_x = float(-axes[b].x);
			small_branch = b;
		}
	}
	REQUIRE(broad_branch > 0);
	REQUIRE(small_branch > 0);
	REQUIRE(broad_branch != small_branch);
	CHECK(catches[broad_branch] > catches[small_branch] * 1.5f);

	// Backward-compatible add_bone defaults mass=1.0, wind_catch=1.0 and accepts explicit overrides.
	Ref<VirtualGeometryWind> manual;
	manual.instantiate();
	REQUIRE(manual->add_bone(Vector3(0, 0, 0), Vector3(0, 1, 0)) == OK);
	REQUIRE(manual->add_bone(Vector3(0, 2, 0), Vector3(1, 0.5, 0), 0.3f, 0.5f, 0.4f, 1.8f, 3.2f, 2.5f) == OK);
	REQUIRE(manual->get_bone_masses().size() == 2);
	REQUIRE(manual->get_bone_wind_catches().size() == 2);
	CHECK(Math::is_equal_approx(manual->get_bone_masses()[0], 1.0f));
	CHECK(Math::is_equal_approx(manual->get_bone_wind_catches()[0], 1.0f));
	CHECK(Math::is_equal_approx(manual->get_bone_masses()[1], 3.2f));
	CHECK(Math::is_equal_approx(manual->get_bone_wind_catches()[1], 2.5f));
}
#endif

TEST_CASE("[NextGeo][DynamicWind] Spring and flutter physics, mass/wind-catch response, local forces, and max_sway clamp") {
	VMS::WindBoneGPU bone;
	bone.rest_pos[0] = 0.0f;
	bone.rest_pos[1] = 2.0f;
	bone.rest_pos[2] = 0.0f;
	bone.lever_length = 2.0f;
	bone.axis[0] = 0.0f;
	bone.axis[1] = 1.0f;
	bone.axis[2] = 0.0f;
	bone.stiffness = 0.35f;
	bone.max_sway = 0.5f;
	bone.sway_phase = 0.4f;
	bone.branch_weight = 1.0f;
	bone.mass = 1.0f;
	bone.wind_catch = 1.0f;

	VMS::DynamicWindSlotDescGPU slot;
	slot.mesh_index = 0u;
	slot.bone_count = 1u;
	slot.palette_bone_offset = 0u;
	slot.flags = VMS::DYNAMIC_WIND_SLOT_FLAG_ACTIVE;
	slot.phase_offset = 0.25f;

	const Vector3 pivot(0.0f, 2.0f, 0.0f);
	const Vector3 tip(0.0f, 4.0f, 0.0f);

	// 1. Zero wind produces exact identity.
	VMS::DynamicWindBoneStateGPU st_zero;
	float cur_z[12], prev_z[12];
	RendererRD::nextgeo_dynamic_wind_step_bone(bone, slot, Vector3(1, 0, 0), 0.0f, 1.0f, 0.5f, 1.0f / 60.0f, nullptr, 0u, st_zero, cur_z, prev_z);
	CHECK(_transform_point_3x4(cur_z, tip).is_equal_approx(tip));
	CHECK(_transform_point_3x4(prev_z, tip).is_equal_approx(tip));

	// 2. Active global wind drives slow sway + flutter with non-zero cur-vs-prev velocity delta while keeping pivot fixed.
	VMS::DynamicWindBoneStateGPU st_wind;
	float cur_w[12], prev_w[12];
	for (int f = 0; f < 30; f++) {
		RendererRD::nextgeo_dynamic_wind_step_bone(bone, slot, Vector3(1, 0, 0), 3.0f, 1.5f, float(f) / 60.0f, 1.0f / 60.0f, nullptr, 0u, st_wind, cur_w, prev_w);
	}
	const Vector3 moved_cur = _transform_point_3x4(cur_w, tip);
	const Vector3 moved_prev = _transform_point_3x4(prev_w, tip);
	CHECK(moved_cur.distance_to(tip) > 0.02f);
	CHECK(moved_cur.distance_to(moved_prev) > 1.0e-5f);
	CHECK(_transform_point_3x4(cur_w, pivot).is_equal_approx(pivot));

	// 3. Higher wind_catch yields larger sway deflection than lower wind_catch.
	VMS::WindBoneGPU bone_low_catch = bone;
	bone_low_catch.wind_catch = 0.4f;
	VMS::WindBoneGPU bone_high_catch = bone;
	bone_high_catch.wind_catch = 2.4f;
	VMS::DynamicWindBoneStateGPU st_lc, st_hc;
	float cur_lc[12], prev_lc[12], cur_hc[12], prev_hc[12];
	for (int f = 0; f < 45; f++) {
		RendererRD::nextgeo_dynamic_wind_step_bone(bone_low_catch, slot, Vector3(1, 0, 0), 2.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, nullptr, 0u, st_lc, cur_lc, prev_lc);
		RendererRD::nextgeo_dynamic_wind_step_bone(bone_high_catch, slot, Vector3(1, 0, 0), 2.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, nullptr, 0u, st_hc, cur_hc, prev_hc);
	}
	CHECK(_transform_point_3x4(cur_hc, tip).distance_to(tip) > _transform_point_3x4(cur_lc, tip).distance_to(tip) * 1.4f);

	// 4. Lighter mass responds faster to an initial wind impulse than heavy mass.
	VMS::WindBoneGPU bone_light = bone;
	bone_light.mass = 0.4f;
	VMS::WindBoneGPU bone_heavy = bone;
	bone_heavy.mass = 4.0f;
	VMS::DynamicWindBoneStateGPU st_light, st_heavy;
	float cur_lt[12], prev_lt[12], cur_hv[12], prev_hv[12];
	for (int f = 0; f < 8; f++) {
		RendererRD::nextgeo_dynamic_wind_step_bone(bone_light, slot, Vector3(1, 0, 0), 3.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, nullptr, 0u, st_light, cur_lt, prev_lt);
		RendererRD::nextgeo_dynamic_wind_step_bone(bone_heavy, slot, Vector3(1, 0, 0), 3.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, nullptr, 0u, st_heavy, cur_hv, prev_hv);
	}
	CHECK(_transform_point_3x4(cur_lt, tip).distance_to(tip) > _transform_point_3x4(cur_hv, tip).distance_to(tip) * 1.5f);

	// 5. Local wind forces deflect promoted slots (directional and radial) and are ignored by shared slots.
	VMS::WindForceGPU dir_force;
	dir_force.position_radius[0] = 0.0f;
	dir_force.position_radius[1] = 2.5f;
	dir_force.position_radius[2] = 0.0f;
	dir_force.position_radius[3] = 4.0f;
	dir_force.direction_strength[0] = 0.0f;
	dir_force.direction_strength[1] = 0.0f;
	dir_force.direction_strength[2] = 1.0f;
	dir_force.direction_strength[3] = 8.0f;
	dir_force.falloff_flags[0] = 1.0f;
	dir_force.falloff_flags[1] = 0.0f; // directional

	VMS::DynamicWindSlotDescGPU promoted_slot = slot;
	promoted_slot.flags = VMS::DYNAMIC_WIND_SLOT_FLAG_ACTIVE | VMS::DYNAMIC_WIND_SLOT_FLAG_PROMOTED;
	VMS::DynamicWindBoneStateGPU st_shared_f, st_promo_f;
	float cur_sf[12], prev_sf[12], cur_pf[12], prev_pf[12];
	for (int f = 0; f < 20; f++) {
		RendererRD::nextgeo_dynamic_wind_step_bone(bone, slot, Vector3(1, 0, 0), 0.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, &dir_force, 1u, st_shared_f, cur_sf, prev_sf);
		RendererRD::nextgeo_dynamic_wind_step_bone(bone, promoted_slot, Vector3(1, 0, 0), 0.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, &dir_force, 1u, st_promo_f, cur_pf, prev_pf);
	}
	CHECK(_transform_point_3x4(cur_sf, tip).is_equal_approx(tip));
	const Vector3 promo_tip = _transform_point_3x4(cur_pf, tip);
	CHECK(promo_tip.z > 0.05f);

	// Radial force positioned at (-1, 3, 0) pushes the bone tip toward +X.
	VMS::WindForceGPU rad_force;
	rad_force.position_radius[0] = -1.0f;
	rad_force.position_radius[1] = 3.0f;
	rad_force.position_radius[2] = 0.0f;
	rad_force.position_radius[3] = 4.0f;
	rad_force.direction_strength[3] = 10.0f;
	rad_force.falloff_flags[0] = 1.0f;
	rad_force.falloff_flags[1] = 1.0f; // radial
	VMS::DynamicWindBoneStateGPU st_rad;
	float cur_rad[12], prev_rad[12];
	for (int f = 0; f < 20; f++) {
		RendererRD::nextgeo_dynamic_wind_step_bone(bone, promoted_slot, Vector3(1, 0, 0), 0.0f, 1.0f, float(f) / 60.0f, 1.0f / 60.0f, &rad_force, 1u, st_rad, cur_rad, prev_rad);
	}
	CHECK(_transform_point_3x4(cur_rad, tip).x > 0.05f);

	// 6. Extreme gale + extreme local force never exceeds declared max_sway.
	VMS::WindForceGPU gale_force = dir_force;
	gale_force.direction_strength[3] = 500.0f;
	VMS::DynamicWindBoneStateGPU st_gale;
	float cur_g[12], prev_g[12];
	for (int f = 0; f < 40; f++) {
		RendererRD::nextgeo_dynamic_wind_step_bone(bone, promoted_slot, Vector3(1, 0, 0), 1000.0f, 4.0f, float(f) / 60.0f, 1.0f / 60.0f, &gale_force, 1u, st_gale, cur_g, prev_g);
		CHECK(_transform_point_3x4(cur_g, tip).distance_to(tip) <= bone.max_sway + 1.0e-3f);
	}
}

TEST_CASE("[NextGeo][DynamicWind] Shared simulation pooling, local force promotion/demotion, and parametric mode switching") {
	ScopedDynamicWindSettings scoped;
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/dynamic_wind", true);
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 0);
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/foliage_wind", false);

	MockDynamicWindVirtualMeshStorage storage;
	storage.mock_device_tier_supported = true;
	storage.set_dynamic_wind_floor_state(true, uint32_t(RendererRD::NEXTGEO_DYNAMIC_WIND_REASON_NONE));

	CHECK(storage.is_dynamic_wind_active());
	CHECK(storage.is_dynamic_wind_force_based_active());
	CHECK_FALSE(storage.is_dynamic_wind_parametric_active());
	CHECK(storage.is_any_wind_active());

	const RID mesh_rid = RID::from_uint64(401);
	constexpr uint32_t BONE_COUNT = 7u;
	storage.mesh_set_virtual_geometry(mesh_rid, _make_dynamic_wind_vgd_desc(BONE_COUNT, 0.6f));
	REQUIRE(storage.mesh_has_wind(mesh_rid));

	// Allocate 64 standalone foliage instances spaced 6m apart along X.
	constexpr uint32_t INSTANCE_COUNT = 64u;
	LocalVector<uint32_t> records;
	records.reserve(INSTANCE_COUNT);
	for (uint32_t i = 0; i < INSTANCE_COUNT; i++) {
		const uint32_t rec = storage.instance_record_allocate(RID::from_uint64(2000 + i), mesh_rid);
		REQUIRE(rec != UINT32_MAX);
		const float px = float(i) * 6.0f;
		storage.instance_record_set_transform(
				rec,
				Transform3D(Basis(), Vector3(px, 0.0f, 0.0f)),
				AABB(Vector3(px - 1.0f, 0.0f, -1.0f), Vector3(2.0f, 5.0f, 2.0f)),
				true);
		storage.instance_record_set_state(rec, VMS::RECORD_VISIBLE);
		records.push_back(rec);
	}

	// Step several frames with global wind only -> 64 instances must share <= 4 simulation slots.
	float sim_time = 0.0f;
	constexpr float DT = 1.0f / 60.0f;
	for (int f = 0; f < 10; f++) {
		sim_time += DT;
		storage.update_dynamic_wind(Vector3(1.0f, 0.0f, 0.0f), 2.5f, 1.5f, sim_time, DT, nullptr, 0u, true);
		storage.flush();
	}

	VMS::DynamicWindStats stats = storage.get_dynamic_wind_stats();
	CHECK(stats.shared_slots > 0u);
	CHECK(stats.shared_slots <= VMS::DYNAMIC_WIND_SHARED_SLOTS_PER_MESH);
	CHECK(stats.promoted_instances == 0u);
	CHECK(stats.active_slots == stats.shared_slots);
	// Palette arena stores only the shared slots (cur + prev), not 64 per-instance copies!
	CHECK(storage.get_palette_arena_used_bones() == stats.shared_slots * BONE_COUNT * 2u);
	CHECK(stats.palette_bones == stats.shared_slots * BONE_COUNT * 2u);

	for (uint32_t rec : records) {
		CHECK((storage.get_record(rec).flags & VMS::RECORD_SKINNED_PALETTE) != 0u);
		CHECK_FALSE(storage.is_record_dynamic_wind_promoted(rec));
		uint32_t bc = 0u;
		CHECK(storage.get_record_dynamic_wind_palette_data(rec, false, bc) != nullptr);
		CHECK(bc == BONE_COUNT);
	}

	// Capture instance 10's current bone pose right before promotion to verify warm-start continuity.
	constexpr uint32_t TARGET_IDX = 10u;
	constexpr uint32_t SIBLING_IDX = 14u; // Same shared variant (10 % 4 == 14 % 4)
	uint32_t pre_bc = 0u;
	const float *pre_cur = storage.get_record_dynamic_wind_palette_data(records[TARGET_IDX], false, pre_bc);
	REQUIRE(pre_cur != nullptr);
	REQUIRE(pre_bc == BONE_COUNT);
	float pre_bone1[12];
	memcpy(pre_bone1, pre_cur + 12u, sizeof(pre_bone1));

	// Add a local wind force overlapping ONLY instance 10 (at x = 60.0, radius = 2.5m; neighbors are 6m away).
	VMS::WindForceGPU local_force;
	local_force.position_radius[0] = float(TARGET_IDX) * 6.0f;
	local_force.position_radius[1] = 2.2f;
	local_force.position_radius[2] = 0.0f;
	local_force.position_radius[3] = 2.5f;
	local_force.direction_strength[0] = 0.0f;
	local_force.direction_strength[1] = 0.0f;
	local_force.direction_strength[2] = 1.0f;
	local_force.direction_strength[3] = 15.0f;
	local_force.falloff_flags[0] = 1.0f;
	local_force.falloff_flags[1] = 0.0f;

	sim_time += DT;
	storage.update_dynamic_wind(Vector3(1.0f, 0.0f, 0.0f), 2.5f, 1.5f, sim_time, DT, &local_force, 1u, true);
	storage.flush();

	stats = storage.get_dynamic_wind_stats();
	CHECK(stats.promoted_instances == 1u);
	CHECK(stats.local_force_count == 1u);
	CHECK(storage.is_record_dynamic_wind_promoted(records[TARGET_IDX]));
	for (uint32_t i = 0; i < INSTANCE_COUNT; i++) {
		if (i != TARGET_IDX) {
			CHECK_FALSE(storage.is_record_dynamic_wind_promoted(records[i]));
		}
	}

	// Verify warm-start descriptor flag and seamless previous-frame pose continuity on promotion frame.
	const uint32_t promo_slot = storage.get_record_dynamic_wind_slot(records[TARGET_IDX]);
	REQUIRE(promo_slot < storage.get_dynamic_wind_slot_descs().size());
	const VMS::DynamicWindSlotDescGPU &promo_desc = storage.get_dynamic_wind_slot_descs()[promo_slot];
	CHECK((promo_desc.flags & VMS::DYNAMIC_WIND_SLOT_FLAG_PROMOTED) != 0u);
	CHECK((promo_desc.flags & VMS::DYNAMIC_WIND_SLOT_FLAG_WARM_START) != 0u);
	CHECK(promo_desc.warm_start_slot >= 0);

	uint32_t post_bc = 0u;
	const float *promo_prev = storage.get_record_dynamic_wind_palette_data(records[TARGET_IDX], true, post_bc);
	REQUIRE(promo_prev != nullptr);
	for (int k = 0; k < 12; k++) {
		CHECK(Math::is_equal_approx(promo_prev[12u + k], pre_bone1[k], 1.0e-4f));
	}

	// Advance 15 frames under the local +Z force and verify target deflects differently from untouched sibling.
	for (int f = 0; f < 15; f++) {
		sim_time += DT;
		storage.update_dynamic_wind(Vector3(1.0f, 0.0f, 0.0f), 2.5f, 1.5f, sim_time, DT, &local_force, 1u, true);
		storage.flush();
	}
	uint32_t bc_t = 0u, bc_s = 0u;
	const float *pal_t = storage.get_record_dynamic_wind_palette_data(records[TARGET_IDX], false, bc_t);
	const float *pal_s = storage.get_record_dynamic_wind_palette_data(records[SIBLING_IDX], false, bc_s);
	REQUIRE(pal_t != nullptr);
	REQUIRE(pal_s != nullptr);
	const Vector3 sample_tip(0.0f, 4.5f, 0.0f);
	const Vector3 pt_t = _transform_point_3x4(pal_t + 12u, sample_tip);
	const Vector3 pt_s = _transform_point_3x4(pal_s + 12u, sample_tip);
	CHECK(pt_t.distance_to(pt_s) > 0.05f);

	// Remove local wind force and step until promoted instance settles and demotes back to shared pool.
	for (uint32_t f = 0; f < VMS::DYNAMIC_WIND_SETTLE_MAX_FRAMES + 5u; f++) {
		sim_time += DT;
		storage.update_dynamic_wind(Vector3(1.0f, 0.0f, 0.0f), 2.5f, 1.5f, sim_time, DT, nullptr, 0u, true);
		storage.flush();
		if (storage.get_dynamic_wind_stats().promoted_instances == 0u) {
			break;
		}
	}
	stats = storage.get_dynamic_wind_stats();
	CHECK(stats.promoted_instances == 0u);
	CHECK_FALSE(storage.is_record_dynamic_wind_promoted(records[TARGET_IDX]));
	CHECK(stats.palette_bones == stats.shared_slots * BONE_COUNT * 2u);
	// Palette free-list retains the high-water mark so subsequent promotions reuse the freed slice without growing.
	const uint32_t hwm_after_demote = storage.get_palette_arena_used_bones();
	CHECK(hwm_after_demote == (stats.shared_slots + 1u) * BONE_COUNT * 2u);

	// Switch to Parametric mode -> dynamic wind stays active, force-based turns off, and palette memory drops to 0!
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/dynamic_wind_mode", 1);
	CHECK(storage.is_dynamic_wind_active());
	CHECK_FALSE(storage.is_dynamic_wind_force_based_active());
	CHECK(storage.is_dynamic_wind_parametric_active());
	CHECK(storage.is_any_wind_active());

	sim_time += DT;
	storage.update_dynamic_wind(Vector3(1.0f, 0.0f, 0.0f), 2.5f, 1.5f, sim_time, DT, nullptr, 0u, storage.is_dynamic_wind_force_based_active());
	storage.flush();

	stats = storage.get_dynamic_wind_stats();
	CHECK(stats.mode == VMS::DYNAMIC_WIND_MODE_PARAMETRIC);
	CHECK(stats.active_slots == 0u);
	CHECK(stats.shared_slots == 0u);
	CHECK(stats.promoted_instances == 0u);
	CHECK(stats.palette_bones == 0u);
	CHECK(storage.get_palette_arena_used_bones() == 0u);
	CHECK(storage.get_palette_arena_bone_capacity() == 0u);
	for (uint32_t rec : records) {
		CHECK((storage.get_record(rec).flags & VMS::RECORD_SKINNED_PALETTE) == 0u);
	}

	for (uint32_t rec : records) {
		storage.instance_record_free(rec);
	}
	storage.mesh_clear_virtual_geometry(mesh_rid, 0u);
}

#ifndef _3D_DISABLED
TEST_CASE("[SceneTree][NextGeo][DynamicWind] World3D and RenderingServer local wind force API") {
	Ref<World3D> world;
	world.instantiate();
	REQUIRE(world.is_valid());
	if (RenderingServer::get_singleton() != nullptr && world->get_scenario().is_valid()) {
		const int f1 = world->add_wind_force(Vector3(1.0f, 2.0f, 3.0f), 5.0f, Vector3(1.0f, 0.0f, 0.0f), 4.0f, 1.5f);
		const int f2 = world->add_wind_force(Vector3(-2.0f, 1.0f, 0.0f), 3.0f, Vector3(), 6.0f, 1.0f);
		CHECK(f1 > 0);
		CHECK(f2 > 0);
		CHECK(f1 != f2);

		world->update_wind_force(f1, Vector3(2.0f, 2.0f, 3.0f), 6.0f, Vector3(0.0f, 0.0f, 1.0f), 5.0f, 2.0f);
		world->remove_wind_force(f1);
		world->clear_wind_forces();

		const int rs_id = RenderingServer::get_singleton()->scenario_add_wind_force(world->get_scenario(), Vector3(0, 1, 0), 4.0f, Vector3(0, 0, 1), 3.0f, 1.0f);
		CHECK(rs_id > 0);
		RenderingServer::get_singleton()->scenario_update_wind_force(world->get_scenario(), rs_id, Vector3(0, 2, 0), 4.5f, Vector3(1, 0, 0), 3.5f, 1.2f);
		RenderingServer::get_singleton()->scenario_remove_wind_force(world->get_scenario(), rs_id);
		RenderingServer::get_singleton()->scenario_clear_wind_forces(world->get_scenario());
	}
}
#endif
