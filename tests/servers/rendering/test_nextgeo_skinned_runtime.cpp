/**************************************************************************/
/*  test_nextgeo_skinned_runtime.cpp                                      */
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

TEST_FORCE_LINK(test_nextgeo_skinned_runtime)

#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/nextgeo_cull.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_skinning.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/nextgeo_records_inc.glsl.gen.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <cmath>
#include <cstring>

namespace {

class SkinnedTestVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	Vector<uint8_t> gpu_palette;
	Vector<uint8_t> gpu_skin_instances;
	Vector<uint8_t> gpu_cluster_skin;
	Vector<uint8_t> gpu_pages;
	uint32_t palette_resize_calls = 0;
	uint32_t palette_upload_calls = 0;
	uint32_t skin_instance_resize_calls = 0;
	uint32_t skin_instance_upload_calls = 0;
	uint32_t cluster_skin_resize_calls = 0;
	uint32_t cluster_skin_upload_calls = 0;

	void read_palette_bone(uint32_t p_vec4_offset, float r_rows[12]) const {
		const uint32_t byte_off = p_vec4_offset * 16u;
		REQUIRE(byte_off + 48u <= uint32_t(gpu_palette.size()));
		memcpy(r_rows, gpu_palette.ptr() + byte_off, 48u);
	}

	SkinInstanceInfo read_gpu_skin_instance(uint32_t p_record) const {
		SkinInstanceInfo out;
		const uint32_t byte_off = p_record * sizeof(SkinInstanceInfo);
		REQUIRE(byte_off + sizeof(SkinInstanceInfo) <= uint32_t(gpu_skin_instances.size()));
		memcpy(&out, gpu_skin_instances.ptr() + byte_off, sizeof(SkinInstanceInfo));
		return out;
	}

	uint32_t read_gpu_cluster_skin(uint32_t p_cluster) const {
		uint32_t out = UINT32_MAX;
		const uint32_t byte_off = p_cluster * sizeof(uint32_t);
		REQUIRE(byte_off + sizeof(uint32_t) <= uint32_t(gpu_cluster_skin.size()));
		memcpy(&out, gpu_cluster_skin.ptr() + byte_off, sizeof(uint32_t));
		return out;
	}

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	bool _resize_dag(uint32_t p_bytes) override {
		(void)p_bytes;
		return true;
	}
	bool _resize_pages(uint32_t p_bytes) override {
		gpu_pages.resize(p_bytes);
		return true;
	}
	void _upload_pages(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		REQUIRE(p_offset + p_size <= uint32_t(gpu_pages.size()));
		memcpy(gpu_pages.ptrw() + p_offset, p_data, p_size);
	}
	bool _resize_skin_palette(uint32_t p_bytes) override {
		palette_resize_calls++;
		const uint32_t old_size = uint32_t(gpu_palette.size());
		gpu_palette.resize(p_bytes);
		if (p_bytes > old_size) {
			memset(gpu_palette.ptrw() + old_size, 0, p_bytes - old_size);
		}
		return true;
	}
	void _upload_skin_palette(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		palette_upload_calls++;
		REQUIRE(p_offset + p_size <= uint32_t(gpu_palette.size()));
		memcpy(gpu_palette.ptrw() + p_offset, p_data, p_size);
	}
	void _reset_skin_palette() override {
		gpu_palette.clear();
	}
	bool _resize_skin_instances(uint32_t p_bytes) override {
		skin_instance_resize_calls++;
		const uint32_t old_size = uint32_t(gpu_skin_instances.size());
		gpu_skin_instances.resize(p_bytes);
		if (p_bytes > old_size) {
			memset(gpu_skin_instances.ptrw() + old_size, 0, p_bytes - old_size);
		}
		return true;
	}
	void _upload_skin_instances(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		skin_instance_upload_calls++;
		REQUIRE(p_offset + p_size <= uint32_t(gpu_skin_instances.size()));
		memcpy(gpu_skin_instances.ptrw() + p_offset, p_data, p_size);
	}
	void _reset_skin_instances() override {
		gpu_skin_instances.clear();
	}
	bool _resize_cluster_skin(uint32_t p_bytes) override {
		cluster_skin_resize_calls++;
		const uint32_t old_size = uint32_t(gpu_cluster_skin.size());
		gpu_cluster_skin.resize(p_bytes);
		if (p_bytes > old_size) {
			memset(gpu_cluster_skin.ptrw() + old_size, 0xFF, p_bytes - old_size);
		}
		return true;
	}
	void _upload_cluster_skin(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		cluster_skin_upload_calls++;
		REQUIRE(p_offset + p_size <= uint32_t(gpu_cluster_skin.size()));
		memcpy(gpu_cluster_skin.ptrw() + p_offset, p_data, p_size);
	}
	void _reset_cluster_skin() override {
		gpu_cluster_skin.clear();
	}
};

static void _append_u8(Vector<uint8_t> &r_out, uint8_t p_v) {
	r_out.push_back(p_v);
}

static void _append_u16(Vector<uint8_t> &r_out, uint16_t p_v) {
	const uint8_t *raw = reinterpret_cast<const uint8_t *>(&p_v);
	r_out.push_back(raw[0]);
	r_out.push_back(raw[1]);
}

static void _append_u32(Vector<uint8_t> &r_out, uint32_t p_v) {
	const uint8_t *raw = reinterpret_cast<const uint8_t *>(&p_v);
	for (int i = 0; i < 4; i++) {
		r_out.push_back(raw[i]);
	}
}

static void _append_f32(Vector<uint8_t> &r_out, float p_v) {
	uint32_t bits = 0;
	memcpy(&bits, &p_v, sizeof(uint32_t));
	_append_u32(r_out, bits);
}

static void _fill_bone_xform(const Transform3D &p_xform, float r_rows[12]) {
	r_rows[0] = float(p_xform.basis.rows[0][0]);
	r_rows[1] = float(p_xform.basis.rows[0][1]);
	r_rows[2] = float(p_xform.basis.rows[0][2]);
	r_rows[3] = float(p_xform.origin.x);
	r_rows[4] = float(p_xform.basis.rows[1][0]);
	r_rows[5] = float(p_xform.basis.rows[1][1]);
	r_rows[6] = float(p_xform.basis.rows[1][2]);
	r_rows[7] = float(p_xform.origin.y);
	r_rows[8] = float(p_xform.basis.rows[2][0]);
	r_rows[9] = float(p_xform.basis.rows[2][1]);
	r_rows[10] = float(p_xform.basis.rows[2][2]);
	r_rows[11] = float(p_xform.origin.z);
}

static Vector<float> _make_palette(const Vector<Transform3D> &p_bones) {
	Vector<float> pal;
	pal.resize(p_bones.size() * 12);
	for (int i = 0; i < p_bones.size(); i++) {
		_fill_bone_xform(p_bones[i], pal.ptrw() + i * 12);
	}
	return pal;
}

static Vector<uint8_t> _make_single_cluster_dag() {
	Vector<uint8_t> dag;
	_append_u32(dag, 1); // group count
	_append_u32(dag, 1); // BVH node count
	// Group 0: 1 cluster at offset 0, page 0, depth 0.
	_append_u32(dag, 0);
	_append_u32(dag, 1);
	_append_u32(dag, 0);
	_append_u32(dag, 0);
	for (int i = 0; i < 4; i++) {
		_append_f32(dag, 0.0f);
	}
	_append_f32(dag, 1.0f);
	for (int i = 0; i < 3; i++) {
		_append_f32(dag, -1.0f);
	}
	for (int i = 0; i < 3; i++) {
		_append_f32(dag, 1.0f);
	}
	// Node 0 -> leaf group 0.
	for (int i = 0; i < 4; i++) {
		_append_f32(dag, 0.0f);
	}
	_append_f32(dag, 1.0f);
	for (int i = 0; i < 3; i++) {
		_append_f32(dag, -1.0f);
	}
	for (int i = 0; i < 3; i++) {
		_append_f32(dag, 1.0f);
	}
	_append_u32(dag, 1);
	_append_u32(dag, 0x80000000u);
	for (int c = 1; c < 8; c++) {
		_append_u32(dag, 0xFFFFFFFFu);
	}
	return dag;
}

// Builds a minimal #159 skinned page containing 1 cluster with 3 vertices (position-only,
// stride = 3 floats), 1 triangle, and a valid skinning block (with p_influences = 4 or 8).
// Returns the page bytes and writes the page-relative byte offset of the skinning block into r_skin_rel_offset.
static Vector<uint8_t> _make_skinned_page(
		uint8_t p_influences,
		const Vector<RendererRD::NextGeoClusterSkinBone> &p_bones,
		const Vector<uint8_t> &p_vert_local_bones,
		const Vector<uint16_t> &p_vert_weights,
		uint32_t &r_skin_rel_offset) {
	constexpr uint32_t VERT_COUNT = 3;
	constexpr uint32_t TRI_COUNT = 1;
	constexpr uint32_t STRIDE_FLOATS = 3; // Position-only vertex_layout = 1.
	const uint32_t vert_off = 16u + 80u;
	const uint32_t vert_bytes = VERT_COUNT * STRIDE_FLOATS * 4u;
	const uint32_t idx_off = vert_off + vert_bytes;
	const uint32_t idx_bytes = TRI_COUNT * 3u;
	const uint32_t skin_off = idx_off + idx_bytes;
	r_skin_rel_offset = skin_off;

	Vector<uint8_t> skin_block;
	_append_u8(skin_block, uint8_t(p_bones.size()));
	_append_u8(skin_block, p_influences);
	_append_u16(skin_block, 0);
	for (int i = 0; i < p_bones.size(); i++) {
		_append_u16(skin_block, p_bones[i].global_bone);
		_append_f32(skin_block, float(p_bones[i].aabb_min.x));
		_append_f32(skin_block, float(p_bones[i].aabb_min.y));
		_append_f32(skin_block, float(p_bones[i].aabb_min.z));
		_append_f32(skin_block, float(p_bones[i].aabb_max.x));
		_append_f32(skin_block, float(p_bones[i].aabb_max.y));
		_append_f32(skin_block, float(p_bones[i].aabb_max.z));
	}
	for (int i = 0; i < p_vert_local_bones.size(); i++) {
		_append_u8(skin_block, p_vert_local_bones[i]);
	}
	for (int i = 0; i < p_vert_weights.size(); i++) {
		_append_u16(skin_block, p_vert_weights[i]);
	}

	const uint32_t skin_bytes = uint32_t(skin_block.size());
	const uint32_t total_size = skin_off + skin_bytes;
	Vector<uint8_t> page;
	page.resize(total_size);
	memset(page.ptrw(), 0, total_size);

	// #159 Page header (16 bytes): page_clusters, page_vertices, page_triangles, skin_bytes.
	uint32_t hdr[4] = { 1u, VERT_COUNT, TRI_COUNT, skin_bytes };
	memcpy(page.ptrw(), hdr, sizeof(hdr));

	// Cluster record (80 bytes at byte 16): vertex_offset (+68), triangle_offset (+72), vertex_count (+76), triangle_count (+77), surface_id (+78).
	uint8_t *rec = page.ptrw() + 16u;
	rec[76] = uint8_t(VERT_COUNT);
	rec[77] = uint8_t(TRI_COUNT);

	// Triangle indices (0, 1, 2).
	page.write[idx_off + 0] = 0;
	page.write[idx_off + 1] = 1;
	page.write[idx_off + 2] = 2;

	// Skinning block at an intentionally unaligned offset (idx_off + 3) to test unaligned reads.
	memcpy(page.ptrw() + skin_off, skin_block.ptr(), skin_bytes);
	return page;
}

static String _glsl_constant(const char *p_source, const String &p_name) {
	const String src = String::utf8(p_source);
	const Vector<String> lines = src.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (!line.begins_with("const ")) {
			continue;
		}
		const int eq = line.find_char('=');
		if (eq < 0) {
			continue;
		}
		const String lhs = line.substr(0, eq).strip_edges();
		if (lhs.ends_with(" " + p_name)) {
			String rhs = line.substr(eq + 1).strip_edges();
			const int semi = rhs.find_char(';');
			if (semi >= 0) {
				rhs = rhs.substr(0, semi).strip_edges();
			}
			return rhs;
		}
	}
	return String();
}

} // namespace

TEST_CASE("[NextGeo][Skinned] Skeleton palette arena packing, prev-frame copy, resize, and recycling (#160)") {
	SkinnedTestVirtualMeshStorage storage;
	using VMS = RendererVirtualMeshStorage;

	// Lazy allocation: with no skinned virtual instances, GPU skin buffers remain untouched.
	CHECK_FALSE(storage.has_active_skinned_instances());
	CHECK_EQ(storage.get_palette_arena_bone_capacity(), 0u);
	CHECK_EQ(storage.palette_resize_calls, 0u);
	CHECK_EQ(storage.skin_instance_resize_calls, 0u);

	const RID mesh_rid = RID::from_uint64(101);
	const RID inst_a = RID::from_uint64(201);
	const RID inst_b = RID::from_uint64(202);
	const RID skel_a = RID::from_uint64(301);
	const RID skel_b = RID::from_uint64(302);

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u;
	desc.vertex_layout = 1u;
	desc.skin_influences = 4u;
	desc.ngm_path = "res://skinned_unit.ngm";
	desc.dag_header = _make_single_cluster_dag();
	Vector<RendererRD::NextGeoClusterSkinBone> page_bones;
	page_bones.push_back({ 0, Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f) });
	Vector<uint8_t> lb = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	Vector<uint16_t> lw = { 65535, 0, 0, 0, 65535, 0, 0, 0, 65535, 0, 0, 0 };
	uint32_t skin_rel_off = 0;
	desc.root_pages.push_back(_make_skinned_page(4, page_bones, lb, lw, skin_rel_off));
	storage.mesh_set_virtual_geometry(mesh_rid, desc);
	CHECK(storage.mesh_is_skinned_virtual_geometry(mesh_rid));

	const uint32_t rec_a = storage.instance_record_allocate(inst_a, mesh_rid);
	storage.instance_record_set_state(rec_a, VMS::RECORD_VISIBLE | VMS::RECORD_HAS_SKELETON);
	storage.instance_record_set_skeleton(rec_a, skel_a);

	// Before skeleton data arrives, RECORD_SKINNED_PALETTE is not set yet.
	CHECK_EQ(storage.get_record(rec_a).flags & VMS::RECORD_SKINNED_PALETTE, 0u);

	auto bits_to_float = [](uint32_t p_bits) {
		float f = 0.0f;
		memcpy(&f, &p_bits, sizeof(float));
		return f;
	};

	// Frame 1: initial pose for skel_a (2 bones). Initial upload must seed BOTH cur and prev so frame 0 has zero velocity spike.
	storage.begin_frame();
	Vector<Transform3D> pose_a0;
	pose_a0.push_back(Transform3D(Basis(), Vector3(1.0f, 0.0f, 0.0f)));
	pose_a0.push_back(Transform3D(Basis::from_scale(Vector3(2.0f, 1.0f, 1.0f)), Vector3(0.0f, 2.0f, 0.0f)));
	const Vector<float> pal_a0 = _make_palette(pose_a0);
	storage.skeleton_update_palette(skel_a, pal_a0.ptr(), 2u);
	storage.flush();

	CHECK(storage.has_active_skinned_instances());
	CHECK_NE(storage.get_record(rec_a).flags & VMS::RECORD_SKINNED_PALETTE, 0u);
	const VMS::SkinInstanceInfo info_a = storage.get_skin_instance_info(rec_a);
	CHECK_EQ(info_a.bone_count, 2u);
	CHECK_EQ(info_a.prev_palette_offset, info_a.current_palette_offset + 2u * 3u);
	CHECK(Math::is_equal_approx(bits_to_float(info_a.max_bone_scale_bits), 2.0f, 1e-4f));

	const VMS::SkinInstanceInfo gpu_info_a = storage.read_gpu_skin_instance(rec_a);
	CHECK_EQ(gpu_info_a.current_palette_offset, info_a.current_palette_offset);
	CHECK_EQ(gpu_info_a.prev_palette_offset, info_a.prev_palette_offset);
	CHECK_EQ(gpu_info_a.bone_count, 2u);

	float cur_b0[12] = {};
	float prev_b0[12] = {};
	storage.read_palette_bone(info_a.current_palette_offset, cur_b0);
	storage.read_palette_bone(info_a.prev_palette_offset, prev_b0);
	CHECK(Math::is_equal_approx(cur_b0[3], 1.0f));
	CHECK(Math::is_equal_approx(prev_b0[3], 1.0f)); // Seeded equal to initial pose.

	// Verify page cluster skin offset was indexed on install into gpu_cluster_skin.
	const VMS::MeshRecord mr = storage.get_mesh_record(mesh_rid);
	const uint32_t gpu_skin_off = storage.read_gpu_cluster_skin(mr.cluster_offset);
	CHECK_NE(gpu_skin_off, UINT32_MAX);
	CHECK_EQ(gpu_skin_off % storage.get_page_slot_size_bytes(), skin_rel_off);

	// Frame 2: animate skel_a twice in the same frame; prev must stay pose_a0 (snapshot before first dirty update), cur must be pose_a2.
	storage.begin_frame();
	Vector<Transform3D> pose_a1;
	pose_a1.push_back(Transform3D(Basis(), Vector3(3.0f, 0.0f, 0.0f)));
	pose_a1.push_back(Transform3D(Basis(), Vector3(0.0f, 4.0f, 0.0f)));
	const Vector<float> pal_a1 = _make_palette(pose_a1);
	storage.skeleton_update_palette(skel_a, pal_a1.ptr(), 2u);

	Vector<Transform3D> pose_a2;
	pose_a2.push_back(Transform3D(Basis(), Vector3(5.0f, 0.0f, 0.0f)));
	pose_a2.push_back(Transform3D(Basis::from_scale(Vector3(3.0f, 3.0f, 3.0f)), Vector3(0.0f, 6.0f, 0.0f)));
	const Vector<float> pal_a2 = _make_palette(pose_a2);
	storage.skeleton_update_palette(skel_a, pal_a2.ptr(), 2u);
	storage.flush();

	storage.read_palette_bone(info_a.current_palette_offset, cur_b0);
	storage.read_palette_bone(info_a.prev_palette_offset, prev_b0);
	CHECK(Math::is_equal_approx(cur_b0[3], 5.0f));
	CHECK(Math::is_equal_approx(prev_b0[3], 1.0f)); // Preserved frame 1 pose across two updates in frame 2.
	CHECK(Math::is_equal_approx(bits_to_float(storage.get_skin_instance_info(rec_a).max_bone_scale_bits), 3.0f, 1e-4f));

	// Frame 3: static frame (no skeleton update). Steady-state flush must sync prev = cur so velocity drops to zero.
	storage.begin_frame();
	storage.flush();
	storage.read_palette_bone(info_a.current_palette_offset, cur_b0);
	storage.read_palette_bone(info_a.prev_palette_offset, prev_b0);
	CHECK(Math::is_equal_approx(cur_b0[3], 5.0f));
	CHECK(Math::is_equal_approx(prev_b0[3], 5.0f));

	// Frame 4: add a large skeleton skel_b (150 bones -> 300 > initial 256 bone capacity, forcing resize) and verify skel_a data survives resize.
	storage.begin_frame();
	const uint32_t rec_b = storage.instance_record_allocate(inst_b, mesh_rid);
	storage.instance_record_set_state(rec_b, VMS::RECORD_VISIBLE | VMS::RECORD_HAS_SKELETON);
	storage.instance_record_set_skeleton(rec_b, skel_b);
	Vector<Transform3D> pose_b;
	pose_b.resize(150);
	for (int i = 0; i < 150; i++) {
		pose_b.write[i] = Transform3D(Basis(), Vector3(float(i), 0.0f, 0.0f));
	}
	const Vector<float> pal_b = _make_palette(pose_b);
	storage.skeleton_update_palette(skel_b, pal_b.ptr(), 150u);
	storage.flush();

	CHECK_GE(storage.get_palette_arena_bone_capacity(), 304u); // (2 + 150) * 2 = 304 bones.
	CHECK_GE(storage.palette_resize_calls, 2u);
	// skel_a's bone data must still be intact after arena resize.
	storage.read_palette_bone(info_a.current_palette_offset, cur_b0);
	CHECK(Math::is_equal_approx(cur_b0[3], 5.0f));

	// Free skel_a and allocate another 2-bone skeleton skel_c -> must recycle skel_a's exact offset (0) without growing used bones.
	const uint32_t used_before_free = storage.get_palette_arena_used_bones();
	storage.instance_record_free(rec_a);
	storage.skeleton_free_notify(skel_a);
	CHECK_GE(storage.get_palette_arena_free_block_count(), 1u);

	const RID inst_c = RID::from_uint64(203);
	const RID skel_c = RID::from_uint64(303);
	storage.begin_frame();
	const uint32_t rec_c = storage.instance_record_allocate(inst_c, mesh_rid);
	storage.instance_record_set_state(rec_c, VMS::RECORD_VISIBLE | VMS::RECORD_HAS_SKELETON);
	storage.instance_record_set_skeleton(rec_c, skel_c);
	storage.skeleton_update_palette(skel_c, pal_a0.ptr(), 2u);
	storage.flush();

	const VMS::SkinInstanceInfo info_c = storage.get_skin_instance_info(rec_c);
	CHECK_EQ(info_c.current_palette_offset, info_a.current_palette_offset); // Recycled slot 0.
	CHECK_EQ(storage.get_palette_arena_used_bones(), used_before_free);

	// Free both remaining skeletons -> free blocks coalesce cleanly back into 1 block.
	storage.instance_record_free(rec_b);
	storage.instance_record_free(rec_c);
	storage.skeleton_free_notify(skel_b);
	storage.skeleton_free_notify(skel_c);
	CHECK_EQ(storage.get_palette_arena_free_block_count(), 1u);
}

TEST_CASE("[NextGeo][Skinned] Bone-bound union AABB and max-bone-scale LOD error vs brute-force LBS (4 and 8 weights) (#160)") {
	using namespace RendererRD;

	// 8 bones with varied rotations, translations, non-uniform scales, and a mirrored negative scale.
	Vector<Transform3D> bones;
	bones.push_back(Transform3D(Basis(Vector3(0, 0, 1), 0.7f), Vector3(1.5f, -0.5f, 0.25f)));
	bones.push_back(Transform3D(Basis(Vector3(0, 1, 0), -1.1f).scaled(Vector3(1.8f, 0.6f, 1.2f)), Vector3(-2.0f, 3.0f, 1.0f)));
	bones.push_back(Transform3D(Basis(Vector3(1, 0, 0), 1.4f).scaled(Vector3(-1.5f, 1.5f, 1.5f)), Vector3(0.4f, 1.2f, -2.5f)));
	bones.push_back(Transform3D(Basis(Vector3(1, 1, 0).normalized(), 0.9f).scaled(Vector3(2.75f, 0.9f, 1.1f)), Vector3(4.0f, -2.0f, 0.5f)));
	bones.push_back(Transform3D(Basis(Vector3(0, 1, 1).normalized(), -0.6f), Vector3(-1.0f, -3.5f, 2.0f)));
	bones.push_back(Transform3D(Basis(Vector3(1, 0, 1).normalized(), 2.1f).scaled(Vector3(0.5f, 2.2f, 0.7f)), Vector3(2.2f, 2.2f, -1.0f)));
	bones.push_back(Transform3D(Basis(Vector3(0, 0, 1), -2.4f).scaled(Vector3(1.3f, 1.3f, 1.3f)), Vector3(-3.0f, 0.5f, -1.5f)));
	bones.push_back(Transform3D(Basis(Vector3(1, 1, 1).normalized(), 1.7f).scaled(Vector3(1.0f, 0.4f, 1.9f)), Vector3(0.0f, 4.5f, 3.0f)));
	const Vector<float> palette = _make_palette(bones);

	// Verify max bone scale across the palette is >= the largest axis scale (2.75 on bone 3),
	// and exact on a pure diagonal scale matrix.
	const float pal_max_scale = nextgeo_palette_max_bone_scale(palette.ptr(), uint32_t(bones.size()));
	CHECK_GE(pal_max_scale, 2.75f - 1e-4f);
	CHECK_GE(nextgeo_bone_matrix_scale(palette.ptr() + 3 * 12), 2.75f - 1e-3f);
	float diag_rows[12] = {};
	_fill_bone_xform(Transform3D(Basis::from_scale(Vector3(2.75f, 0.9f, 1.1f)), Vector3()), diag_rows);
	CHECK(Math::is_equal_approx(nextgeo_bone_matrix_scale(diag_rows), 2.75f, 1e-4f));

	// Test BOTH 4-influence and 8-influence clusters built into raw #159 page bytes.
	const uint8_t influence_modes[2] = { 4, 8 };
	for (uint8_t infl : influence_modes) {
		// 3 vertices with diverse positions and convex normalized weights over 4 or 8 bones.
		const Vector3 rest_pos[3] = {
			Vector3(-0.8f, 0.2f, 0.6f),
			Vector3(0.7f, -0.9f, -0.3f),
			Vector3(0.3f, 1.1f, -0.7f),
		};

		const uint32_t active_bones = infl == 4 ? 4u : 8u;
		Vector<NextGeoClusterSkinBone> cluster_bones;
		cluster_bones.resize(active_bones);
		for (uint32_t b = 0; b < active_bones; b++) {
			cluster_bones.write[b].global_bone = uint16_t(b);
			cluster_bones.write[b].aabb_min = Vector3(1e9f, 1e9f, 1e9f);
			cluster_bones.write[b].aabb_max = Vector3(-1e9f, -1e9f, -1e9f);
		}

		Vector<uint8_t> local_indices;
		Vector<uint16_t> weights_u16;
		for (uint32_t v = 0; v < 3; v++) {
			uint32_t rem = 65535u;
			for (uint32_t k = 0; k < infl; k++) {
				const uint8_t lb = uint8_t((k + v) % active_bones);
				local_indices.push_back(lb);
				const uint16_t w = (k + 1u == infl) ? uint16_t(rem) : uint16_t(65535u / (infl + (k == 0 ? 0 : 1)));
				rem -= w;
				weights_u16.push_back(w);
				if (w > 0u) {
					cluster_bones.write[lb].aabb_min = cluster_bones[lb].aabb_min.min(rest_pos[v]);
					cluster_bones.write[lb].aabb_max = cluster_bones[lb].aabb_max.max(rest_pos[v]);
				}
			}
		}

		uint32_t skin_rel_off = 0;
		const Vector<uint8_t> page = _make_skinned_page(infl, cluster_bones, local_indices, weights_u16, skin_rel_off);

		Vector3 raw_min, raw_max;
		float raw_scale = 0.0f;
		REQUIRE(nextgeo_skin_cluster_bounds_raw(page.ptr(), uint32_t(page.size()), skin_rel_off, palette.ptr(), uint32_t(bones.size()), raw_min, raw_max, raw_scale));

		Vector3 struct_min, struct_max;
		float struct_scale = 0.0f;
		REQUIRE(nextgeo_skin_cluster_bounds_from_bones(cluster_bones.ptr(), cluster_bones.size(), palette.ptr(), uint32_t(bones.size()), struct_min, struct_max, struct_scale));

		// Raw page parser and struct reference must agree identically.
		CHECK(raw_min.is_equal_approx(struct_min));
		CHECK(raw_max.is_equal_approx(struct_max));
		CHECK(Math::is_equal_approx(raw_scale, struct_scale, 1e-5f));
		// Cluster contains bone 3 (scale 2.75), so LOD error scale factor must be >= 2.75.
		CHECK_GE(raw_scale, 2.75f - 1e-3f);

		// Brute-force skin every vertex and verify zero culled-but-visible bound violations!
		constexpr float EPS = 1e-4f;
		for (uint32_t v = 0; v < 3; v++) {
			Vector3 skinned_pos = rest_pos[v];
			Vector3 skinned_norm = Vector3(0.0f, 1.0f, 0.0f);
			Vector3 skinned_tang = Vector3(1.0f, 0.0f, 0.0f);
			REQUIRE(nextgeo_skin_vertex_lbs_raw(page.ptr(), uint32_t(page.size()), skin_rel_off, 3u, v, palette.ptr(), uint32_t(bones.size()), skinned_pos, &skinned_norm, &skinned_tang));

			// Compute direct brute-force reference LBS from Transform3D.
			Vector3 ref_pos;
			float w_sum = 0.0f;
			for (uint32_t k = 0; k < infl; k++) {
				const float w = float(weights_u16[v * infl + k]) * (1.0f / 65535.0f);
				const uint8_t lb = local_indices[v * infl + k];
				ref_pos += bones[cluster_bones[lb].global_bone].xform(rest_pos[v]) * w;
				w_sum += w;
			}
			ref_pos /= w_sum;

			CHECK(skinned_pos.is_equal_approx(ref_pos));
			CHECK(Math::is_equal_approx(float(skinned_norm.length()), 1.0f, 1e-4f));
			CHECK(Math::is_equal_approx(float(skinned_tang.length()), 1.0f, 1e-4f));

			// Skinned vertex MUST lie inside the union bone-bound AABB (0 culled-but-visible clusters).
			CHECK_GE(skinned_pos.x, raw_min.x - EPS);
			CHECK_GE(skinned_pos.y, raw_min.y - EPS);
			CHECK_GE(skinned_pos.z, raw_min.z - EPS);
			CHECK_LE(skinned_pos.x, raw_max.x + EPS);
			CHECK_LE(skinned_pos.y, raw_max.y + EPS);
			CHECK_LE(skinned_pos.z, raw_max.z + EPS);
		}
	}
}

TEST_CASE("[NextGeo][Skinned] Route admission, RASTER_FEATURE_SKINNED bins, VSM dynamic layer, and GLSL sync (#160)") {
	using VMS = RendererVirtualMeshStorage;
	using namespace RendererRD;

	// Surface route admission: skinned VGD + skeleton + no blend shapes -> virtual!
	NextGeoSurfaceFacts facts;
	facts.instance_mesh = true;
	facts.mesh_has_virtual_geometry = true;
	facts.mesh_skinned_virtual_geometry = true;
	facts.surface_in_mask = true;
	facts.draw_ready = true;
	facts.skeleton = true;
	facts.blend_shapes = false;
	CHECK(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_NONE);

	// Skinned VGD with blend shapes must still fall back cleanly.
	NextGeoSurfaceFacts with_bs = facts;
	with_bs.blend_shapes = true;
	CHECK_FALSE(nextgeo_resolve_surface_route(with_bs).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(with_bs).reason, NEXTGEO_FALLBACK_SKINNING);

	// Unskinned VGD with a skeleton attached must still fall back cleanly.
	NextGeoSurfaceFacts unskinned_vgd = facts;
	unskinned_vgd.mesh_skinned_virtual_geometry = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(unskinned_vgd).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(unskinned_vgd).reason, NEXTGEO_FALLBACK_SKINNING);

	// Raster feature mask & bin table across visibility and VSM passes.
	NextGeoRasterFacts rf;
	rf.skinned = true;
	CHECK_EQ(nextgeo_raster_features(rf), uint32_t(VMS::RASTER_FEATURE_SKINNED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SKINNED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_SKINNED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_FIXED));

	rf.alpha_scissor = true;
	CHECK_EQ(nextgeo_raster_features(rf), uint32_t(VMS::RASTER_FEATURE_SKINNED | VMS::RASTER_FEATURE_MASKED));
	CHECK_EQ(VMS::resolve_raster_bin(nextgeo_raster_features(rf), VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(nextgeo_raster_features(rf), VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));

	rf.alpha_scissor = false;
	rf.writes_vertex = true;
	CHECK_EQ(VMS::resolve_raster_bin(nextgeo_raster_features(rf), VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(nextgeo_raster_features(rf), VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));

	// Instance cull predicates (main + shadow): admit RECORD_HAS_SKELETON only when RECORD_SKINNED_PALETTE is set.
	const uint32_t base = VMS::RECORD_VALID | VMS::RECORD_VISIBLE;
	CHECK_FALSE(nextgeo_instance_cullable(base | VMS::RECORD_HAS_SKELETON, 1u, 1u));
	CHECK(nextgeo_instance_cullable(base | VMS::RECORD_HAS_SKELETON | VMS::RECORD_SKINNED_PALETTE, 1u, 1u));
	CHECK_FALSE(nextgeo_shadow_instance_cullable(base | VMS::RECORD_CAST_SHADOW_ON | VMS::RECORD_HAS_SKELETON, 1u, 1u));
	CHECK(nextgeo_shadow_instance_cullable(base | VMS::RECORD_CAST_SHADOW_ON | VMS::RECORD_HAS_SKELETON | VMS::RECORD_SKINNED_PALETTE, 1u, 1u));

	// VSM dynamic layer classification (#212 / #217): skinned NextGeo instances route dynamic when hook is active.
	const uint32_t hook = NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK;
	CHECK_EQ(nextgeo_vsm_instance_layer(base | VMS::RECORD_HAS_SKELETON | VMS::RECORD_SKINNED_PALETTE, false, 0u, false), 0u);
	CHECK_EQ(nextgeo_vsm_instance_layer(base | VMS::RECORD_HAS_SKELETON | VMS::RECORD_SKINNED_PALETTE, false, hook, false), 1u);
	CHECK_EQ(nextgeo_vsm_instance_layer(base | VMS::RECORD_SKINNED_PALETTE, false, hook, false), 1u);

	// GLSL header sync: NEXTGEO_RECORD_SKINNED_PALETTE matches RECORD_SKINNED_PALETTE (1u << 16).
	const String glsl_flag = _glsl_constant(nextgeo_records_inc_shader_glsl, "NEXTGEO_RECORD_SKINNED_PALETTE");
	CHECK_EQ(glsl_flag, String("1u << 16"));
	CHECK_EQ(uint32_t(VMS::RECORD_SKINNED_PALETTE), 1u << 16);
}

TEST_CASE("[NextGeo][Skinned] Off-screen RT-range proxy refit scheduling (#160)") {
	using namespace RendererRD;

	const Vector3 cam_origin(0.0f, 1.5f, 0.0f);
	const float rt_range = NEXTGI_RT_ANGULAR_CULL_DISTANCE; // 30m #59 refit range.
	const float scene_radius = 100.0f;

	// Case 1: Skinned instance directly BEHIND the camera (frustum-culled at +Z when camera looks -Z)
	// at 6m distance -> MUST be eligible for NextGI RT selection and within RT refit range!
	const Vector3 behind_near(0.0f, 1.5f, 6.0f);
	CHECK(nextgi_rt_instance_eligible(true, true, true, true));
	CHECK_EQ(nextgi_rt_cull_instance(float(cam_origin.distance_to(behind_near)), 1.0f, scene_radius), NEXTGI_RT_INCLUDED);
	CHECK(nextgeo_skinned_within_rt_refit_range(cam_origin, behind_near, rt_range));

	// Case 2: On exact boundary (30.0m behind camera) -> included.
	const Vector3 behind_boundary(0.0f, 1.5f, rt_range);
	CHECK(nextgeo_skinned_within_rt_refit_range(cam_origin, behind_boundary, rt_range));

	// Case 3: Beyond RT refit range (35.0m behind camera) -> skipped to stay inside D17 budget.
	const Vector3 behind_far(0.0f, 1.5f, rt_range + 5.0f);
	CHECK_FALSE(nextgeo_skinned_within_rt_refit_range(cam_origin, behind_far, rt_range));

	// Case 4: Non-positive or NaN range -> false.
	CHECK_FALSE(nextgeo_skinned_within_rt_refit_range(cam_origin, behind_near, 0.0f));
	CHECK_FALSE(nextgeo_skinned_within_rt_refit_range(cam_origin, behind_near, -10.0f));
	CHECK_FALSE(nextgeo_skinned_within_rt_refit_range(cam_origin, behind_near, NAN));
}
