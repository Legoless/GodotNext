/**************************************************************************/
/*  test_nextgi_swrt_trace.cpp                                            */
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

TEST_FORCE_LINK(test_nextgi_swrt_trace)

#include "core/math/face3.h"
#include "core/math/mesh_sdf_builder.h"
#include "core/math/random_number_generator.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_clipmaps.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_gpu.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

#include "modules/modules_enabled.gen.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight_sdf.glsl.gen.h"
#ifdef MODULE_GLSLANG_ENABLED
#include "servers/rendering/rendering_device_commons.h"

#include "modules/glslang/shader_compile.h"
#endif
#endif

namespace {

MeshSDFData _box_sdf(const Vector3 &p_size) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	MeshSDFSettings settings;
	settings.voxels_per_meter = 8.0f;
	settings.max_voxels_per_axis = 32;
	settings.prefer_static_raycaster = false;
	MeshSDFData data;
	const Error err = MeshSDFBuilder::build(box->get_faces(), settings, data);
	CHECK(err == OK);
	CHECK(data.is_valid());
	return data;
}

// Signed distance from a world point to the image of the unit cube [-0.5, 0.5]^3
// under a transform whose basis columns are orthogonal (a rotated, scaled box).
float _oriented_box_distance(const Transform3D &p_transform, const Vector3 &p_world) {
	const Vector3 offset = p_world - p_transform.origin;
	Vector3 q;
	for (int axis = 0; axis < 3; axis++) {
		const Vector3 column = p_transform.basis.get_column(axis);
		const float length = column.length();
		q[axis] = Math::abs(offset.dot(column / length)) - 0.5f * length;
	}
	const float outside = Vector3(MAX(q.x, 0.0f), MAX(q.y, 0.0f), MAX(q.z, 0.0f)).length();
	return outside + MIN(MAX(q.x, MAX(q.y, q.z)), 0.0f);
}

// The distance from a world point to the image of the unit cube [-0.5, 0.5]^3 under any
// transform (a parallelepiped when the basis is sheared): 0 inside, else the nearest of its
// twelve triangles.
float _parallelepiped_distance(const Transform3D &p_transform, const Vector3 &p_world) {
	const Vector3 local = p_transform.affine_inverse().xform(p_world);
	if (MAX(Math::abs(local.x), MAX(Math::abs(local.y), Math::abs(local.z))) <= 0.5f) {
		return 0.0f;
	}
	Vector3 corners[8];
	for (int corner = 0; corner < 8; corner++) {
		corners[corner] = p_transform.xform(Vector3((corner & 1) ? 0.5f : -0.5f, (corner & 2) ? 0.5f : -0.5f, (corner & 4) ? 0.5f : -0.5f));
	}
	// The six faces, each a quad of the corners with one bit fixed.
	const int faces[6][4] = { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } };
	float nearest = 1e30f;
	for (const auto &face : faces) {
		const Face3 first(corners[face[0]], corners[face[1]], corners[face[2]]);
		const Face3 second(corners[face[0]], corners[face[2]], corners[face[3]]);
		nearest = MIN(nearest, first.get_closest_point_to(p_world).distance_to(p_world));
		nearest = MIN(nearest, second.get_closest_point_to(p_world).distance_to(p_world));
	}
	return nearest;
}

// The world error one 8-bit step of a mesh SDF can make along a local axis.
float _quantum(const MeshSDFData &p_field, float p_axis_scale) {
	return 2.0f * p_field.mip_bands[0] / 255.0f * p_axis_scale;
}

// The renderer's prepass setup: Godot's default perspective camera.
RenderSceneDataRD *_scene_data(bool p_flip_y, const Vector2 &p_jitter) {
	RenderSceneDataRD *scene_data = memnew(RenderSceneDataRD);
	scene_data->cam_projection.set_perspective(75.0f, 16.0f / 9.0f, 0.05f, 4000.0f);
	scene_data->cam_transform = Transform3D(Basis::from_euler(Vector3(-0.3f, 0.7f, 0.0f)), Vector3(1.0f, 2.0f, 8.0f));
	scene_data->flip_y = p_flip_y;
	scene_data->taa_jitter = p_jitter;
	scene_data->prev_cam_projection = scene_data->cam_projection;
	scene_data->prev_cam_transform = Transform3D(Basis::from_euler(Vector3(-0.25f, 0.65f, 0.0f)), Vector3(1.2f, 2.0f, 7.9f));
	scene_data->prev_taa_jitter = -p_jitter;
	return scene_data;
}

} // namespace

#ifdef RD_ENABLED
TEST_CASE("[NextGI][SWRT] Software trace shaders do not require ray query") {
	CHECK_FALSE(NextgiGatherSdfShaderRD().requires_ray_query());
	CHECK_FALSE(NextgiSrcRelightSdfShaderRD().requires_ray_query());
	CHECK(NextgiGatherShaderRD().requires_ray_query());
}
#endif

TEST_CASE("[NextGI][View] Depth reconstruction inverts the prepass projection") {
	// What the depth prepass stores for a world point, with the projection the
	// renderer builds for the scene UBO (RenderSceneDataRD::get_cam_projection),
	// reconstructed with the matrix every NextGI pass now uploads.
	for (int flip = 0; flip < 2; flip++) {
		RenderSceneDataRD *scene_data = _scene_data(flip == 1, Vector2(0.4f / 1920.0f, -0.3f / 1080.0f));
		const Projection prepass = scene_data->get_cam_projection() * Projection(scene_data->cam_transform.affine_inverse());
		const Projection view_projection = NextGIView::view_projection(*scene_data);
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				CHECK(Math::is_equal_approx(view_projection.columns[column][row], prepass.columns[column][row]));
			}
		}
		const Projection inv_view_projection = NextGIView::inv_view_projection(*scene_data);
		const Vector3 forward = -scene_data->cam_transform.basis.get_column(2);
		const Vector3 right = scene_data->cam_transform.basis.get_column(0);
		const Vector3 up = scene_data->cam_transform.basis.get_column(1);
		const float distances[] = { 0.2f, 0.5f, 2.0f, 8.0f, 20.0f, 100.0f, 500.0f };
		for (const float distance : distances) {
			const Vector3 world = scene_data->cam_transform.origin + forward * distance + right * (0.3f * distance) - up * (0.2f * distance);
			const Vector4 clip = prepass.xform(Vector4(world.x, world.y, world.z, 1.0f));
			const Vector2 uv(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f);
			const float depth = clip.z / clip.w;
			// Reverse Z with a [0, 1] remap: in front of the camera, never the cleared 0.
			CHECK(depth > 0.0f);
			CHECK(depth < 1.0f);
			CHECK_FALSE(NextGIViewMath::is_background(depth));
			const Vector3 reconstructed = NextGIViewMath::reconstruct(inv_view_projection, uv, depth);
			CHECK(reconstructed.distance_to(world) <= 1.0e-3f * distance + 1.0e-4f);
			const Vector3 projected = NextGIViewMath::project(view_projection, world);
			CHECK(Math::is_equal_approx(projected.x, uv.x));
			CHECK(Math::is_equal_approx(projected.y, uv.y));
			CHECK(Math::is_equal_approx(projected.z, depth));
			if (distance == 8.0f) {
				// The convention the passes used before: the raw OpenGL-style projection
				// fed depth * 2 - 1. Every surface lands on the near plane.
				const Projection raw = (scene_data->cam_projection * Projection(scene_data->cam_transform.affine_inverse())).inverse();
				const Vector4 old_clip(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
				const Vector4 old_world = raw.xform(old_clip);
				const Vector3 old_position = Vector3(old_world.x, old_world.y, old_world.z) / old_world.w;
				CHECK(old_position.distance_to(scene_data->cam_transform.origin) < 0.1f);
				CHECK(old_position.distance_to(world) > 7.0f);
			}
		}
		// The previous view uses the previous camera and jitter, as the scene UBO's previous projection does.
		Projection prev_correction;
		prev_correction.set_depth_correction(scene_data->flip_y);
		prev_correction.add_jitter_offset(scene_data->prev_taa_jitter);
		const Projection prev_prepass = prev_correction * scene_data->prev_cam_projection * Projection(scene_data->prev_cam_transform.affine_inverse());
		const Projection prev_view_projection = NextGIView::prev_view_projection(*scene_data);
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				CHECK(Math::is_equal_approx(prev_view_projection.columns[column][row], prev_prepass.columns[column][row]));
			}
		}
		// View space (the diffuse denoiser's spatial filter) uses the same depth projection.
		const Vector3 view_point(0.5f, -0.25f, -6.0f);
		const Vector4 view_clip = scene_data->get_cam_projection().xform(Vector4(view_point.x, view_point.y, view_point.z, 1.0f));
		const Vector3 view_reconstructed = NextGIViewMath::reconstruct(NextGIView::inv_projection(*scene_data), Vector2(view_clip.x / view_clip.w * 0.5f + 0.5f, view_clip.y / view_clip.w * 0.5f + 0.5f), view_clip.z / view_clip.w);
		CHECK(view_reconstructed.distance_to(view_point) < 1.0e-3f);
		memdelete(scene_data);
	}
}

TEST_CASE("[NextGI][View] The hardware tier's gather and WRC upload the prepass's matrices") {
	// The HWRT gather (its diffuse and reflect kernels) and the WRC froxels reconstruct from the
	// depth texture, so they must upload the projection the prepass drew with, not the raw
	// OpenGL-style cam_projection (which puts every surface on the near plane).
	for (int flip = 0; flip < 2; flip++) {
		RenderSceneDataRD *scene_data = _scene_data(flip == 1, Vector2(0.4f / 1920.0f, -0.3f / 1080.0f));
		const Projection prepass = scene_data->get_cam_projection() * Projection(scene_data->cam_transform.affine_inverse());
		const RendererRD::NextGIGather::ViewMatrices gather = RendererRD::NextGIGather::view_matrices(*scene_data);
		const RendererRD::NextGIWRC::ViewParams wrc = RendererRD::NextGIWRC::view_params(*scene_data);
		const Transform3D world_to_view = scene_data->cam_transform.affine_inverse();
		const Vector3 forward = -scene_data->cam_transform.basis.get_column(2);
		const Vector3 right = scene_data->cam_transform.basis.get_column(0);
		const Vector3 up = scene_data->cam_transform.basis.get_column(1);
		const float distances[] = { 0.5f, 8.0f, 120.0f };
		for (const float distance : distances) {
			const Vector3 world = scene_data->cam_transform.origin + forward * distance + right * (0.2f * distance) + up * (0.1f * distance);
			const Vector4 clip = prepass.xform(Vector4(world.x, world.y, world.z, 1.0f));
			const Vector2 uv(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f);
			const float depth = clip.z / clip.w;
			const float tolerance = 1.0e-3f * distance + 1.0e-4f;
			CHECK(NextGIViewMath::reconstruct(gather.inv_view_proj, uv, depth).distance_to(world) <= tolerance);
			CHECK(NextGIViewMath::reconstruct(wrc.inv_view_proj, uv, depth).distance_to(world) <= tolerance);
			// The WRC history projects into the same uv and depth.
			const Vector3 projected = NextGIViewMath::project(wrc.view_proj, world);
			CHECK(Vector2(projected.x, projected.y).distance_to(uv) <= 1.0e-4f);
			CHECK(Math::is_equal_approx(projected.z, depth, 1.0e-5f));
			// The froxels step in linear depth (view-space -z) and store the depth texture's value.
			const float linear = -world_to_view.xform(world).z;
			CHECK(Math::is_equal_approx(NextGIViewMath::depth_from_linear(wrc.depth_coefficients, linear), depth, 1.0e-5f));
			// The gather's denoiser works in view space.
			const Vector3 view_point = world_to_view.xform(world);
			CHECK(NextGIViewMath::reconstruct(gather.inv_projection, uv, depth).distance_to(view_point) <= tolerance);
		}
		// The gather's history reprojects with the previous camera and jitter.
		Projection prev_correction;
		prev_correction.set_depth_correction(scene_data->flip_y);
		prev_correction.add_jitter_offset(scene_data->prev_taa_jitter);
		const Projection prev_prepass = prev_correction * scene_data->prev_cam_projection * Projection(scene_data->prev_cam_transform.affine_inverse());
		const Vector3 point = scene_data->cam_transform.origin + forward * 6.0f;
		const Vector3 previous = NextGIViewMath::project(gather.prev_view_proj, point);
		const Vector4 prev_clip = prev_prepass.xform(Vector4(point.x, point.y, point.z, 1.0f));
		CHECK(Vector2(previous.x, previous.y).distance_to(Vector2(prev_clip.x / prev_clip.w * 0.5f + 0.5f, prev_clip.y / prev_clip.w * 0.5f + 0.5f)) <= 1.0e-4f);
		CHECK(Math::is_equal_approx(previous.z, prev_clip.z / prev_clip.w, 1.0e-5f));
		// The prepass stores view-space normals; the reflect kernel turns them into world space.
		const Vector3 world_normal = Vector3(0.3f, 0.8f, -0.52f).normalized();
		const Vector3 view_normal = world_to_view.basis.xform(world_normal);
		const Vector3 encoded = view_normal * 0.5f + Vector3(0.5f, 0.5f, 0.5f);
		CHECK(NextGIViewMath::world_normal(gather.view_to_world, encoded).distance_to(world_normal) <= 1.0e-5f);
		memdelete(scene_data);
	}
}

#if defined(RD_ENABLED) && defined(MODULE_GLSLANG_ENABLED)
// The Offset decorations of the members of the struct that declares p_member, by member name.
static HashMap<String, uint32_t> _spirv_struct_offsets(const Vector<uint8_t> &p_spirv, const String &p_member) {
	const int count = p_spirv.size() / 4;
	auto word = [&](int p_index) {
		const uint8_t *bytes = p_spirv.ptr() + p_index * 4;
		return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
	};
	HashMap<uint64_t, String> names;
	HashMap<uint64_t, uint32_t> offsets;
	uint32_t owner = UINT32_MAX;
	for (int index = 5; index < count;) {
		const uint32_t word_count = word(index) >> 16;
		const uint32_t opcode = word(index) & 0xffffu;
		if (word_count == 0 || index + int(word_count) > count) {
			break;
		}
		const uint64_t key = word_count >= 3 ? (uint64_t(word(index + 1)) << 32) | word(index + 2) : 0;
		if (opcode == 6 && word_count >= 4) { // OpMemberName: struct, member, literal string.
			const String name = String::utf8((const char *)(p_spirv.ptr() + (index + 3) * 4));
			names.insert(key, name);
			if (name == p_member) {
				owner = word(index + 1);
			}
		} else if (opcode == 72 && word_count >= 5 && word(index + 3) == 35) { // OpMemberDecorate ... Offset.
			offsets.insert(key, word(index + 4));
		}
		index += int(word_count);
	}
	HashMap<String, uint32_t> result;
	for (const KeyValue<uint64_t, String> &name : names) {
		if (uint32_t(name.key >> 32) == owner && offsets.has(name.key)) {
			result.insert(name.value, offsets[name.key]);
		}
	}
	return result;
}

TEST_CASE("[NextGI][View] The WRC update shader reads its params where the renderer writes them") {
	// NextGIWRCParamsGPU (next_gi_wrc.cpp) pins these std430 offsets with static_asserts. The
	// shader dropped depth_coefficients once, and read previous_view_proj and history 16 bytes off.
	const String source = RendererRD::NextGIWRC::build_update_shader_source();
	REQUIRE_FALSE(source.is_empty());
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(RenderingDeviceCommons::SHADER_STAGE_COMPUTE, source, RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3, &error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed to compile the WRC update shader:\n" + error).utf8().get_data());
	}
	REQUIRE_GE(spirv.size(), 20);
	const HashMap<String, uint32_t> offsets = _spirv_struct_offsets(spirv, "previous_view_proj");
	auto offset_of = [&offsets](const String &p_name) {
		const uint32_t *offset = offsets.getptr(p_name);
		return offset != nullptr ? *offset : UINT32_MAX;
	};
	CHECK_EQ(offset_of("inv_view_proj"), 80u);
	CHECK_EQ(offset_of("volume"), 144u);
	CHECK_EQ(offset_of("depth_coefficients"), 160u);
	CHECK_EQ(offset_of("previous_view_proj"), 176u);
	CHECK_EQ(offset_of("history"), 240u);
	// The froxels turn linear depth into the depth texture's value and back with them.
	CHECK(source.contains("nextgi_view_depth_from_linear(params.depth_coefficients,"));
	CHECK(source.contains("nextgi_view_linear_depth(params.depth_coefficients,"));
}
#endif

TEST_CASE("[NextGI][View] Linear depth slices round-trip through the stored depth") {
	RenderSceneDataRD *scene_data = _scene_data(true, Vector2());
	const Projection depth_projection = NextGIView::depth_projection(*scene_data);
	const Vector4 coefficients = NextGIViewMath::depth_coefficients(depth_projection);
	const float distances[] = { 0.05f, 0.5f, 3.0f, 64.0f, 1000.0f };
	for (const float distance : distances) {
		const Vector4 clip = depth_projection.xform(Vector4(0.1f, 0.2f, -distance, 1.0f));
		const float depth = clip.z / clip.w;
		CHECK(Math::is_equal_approx(NextGIViewMath::depth_from_linear(coefficients, distance), depth, 1.0e-6f));
		CHECK(Math::is_equal_approx(NextGIViewMath::linear_depth(coefficients, depth), distance, 1.0e-3f * distance));
	}
	// Reverse Z: nearer is larger.
	CHECK(NextGIViewMath::depth_from_linear(coefficients, 1.0f) > NextGIViewMath::depth_from_linear(coefficients, 2.0f));
	memdelete(scene_data);
}

TEST_CASE("[NextGI][GlobalSDF] Coverage words read the CPU coverage layout") {
	// One bit per voxel, 16 words per slot: a bit set through the CPU BrickSplit
	// layout is the word and bit the trace and the compositing shader use, and
	// no other voxel's bit, also in the next slot.
	for (uint32_t voxel = 0; voxel < NextGIGlobalSDF::BRICK_VOXELS; voxel++) {
		NextGIGlobalSDF::BrickSplit split;
		NextGIGlobalSDF::set_coverage_bit(split, Vector3i(int(voxel % 8), int((voxel / 8) % 8), int(voxel / 64)), true);
		uint32_t words[2 * NextGIGlobalSDFGPU::COVERAGE_BYTES / 4] = {};
		memcpy(words + NextGIGlobalSDFGPU::COVERAGE_BYTES / 4, split.coverage, NextGIGlobalSDFGPU::COVERAGE_BYTES);
		const uint32_t word = NextGIGlobalSDFGPU::coverage_word(1, voxel);
		CHECK(word < 2 * NextGIGlobalSDFGPU::COVERAGE_BYTES / 4);
		CHECK(word >= NextGIGlobalSDFGPU::COVERAGE_BYTES / 4);
		CHECK_EQ(words[word], NextGIGlobalSDFGPU::coverage_bit(voxel));
		uint32_t set_bits = 0;
		for (const uint32_t value : words) {
			for (uint32_t bit = 0; bit < 32; bit++) {
				set_bits += (value >> bit) & 1u;
			}
		}
		CHECK_EQ(set_bits, 1u);
	}
}

TEST_CASE("[NextGI][GlobalSDF] Distances past a mesh SDF's bounds keep growing") {
	// The mesh SDF stores a narrow band and clamps outside its bounds. The global
	// field extends it with a continuous lower bound, so coarse levels do not read
	// "near a surface" a voxel or more away from every mesh.
	const MeshSDFData box = _box_sdf(Vector3(1.0f, 1.0f, 1.0f));
	NextGIGlobalSDF::Instance instance;
	instance.field = &box;
	instance.transform = Transform3D(Basis(), Vector3(2.0f, 0.0f, -1.0f));
	const float band = box.mip_bands[0];
	const float distances[] = { 0.8f, 1.5f, 3.0f, 6.0f };
	float previous = 0.0f;
	for (const float distance : distances) {
		// Straight out of the +x face: the true distance is `distance`.
		const float sampled = NextGIGlobalSDF::sample_instance(instance, instance.transform.origin + Vector3(0.5f + distance, 0.0f, 0.0f));
		CHECK(sampled <= distance + 0.02f);
		CHECK(sampled >= distance - band - 0.02f);
		CHECK(sampled > previous);
		previous = sampled;
	}
	CHECK(previous > 4.0f * band);
	// Continuous across the bounds: just inside and just outside agree.
	const Vector3 local_edge = box.bounds.position + box.bounds.size * Vector3(1.0f, 0.5f, 0.5f);
	const float inner = NextGIGlobalSDF::sample_instance(instance, instance.transform.xform(local_edge - Vector3(1.0e-3f, 0.0f, 0.0f)));
	const float outer = NextGIGlobalSDF::sample_instance(instance, instance.transform.xform(local_edge + Vector3(1.0e-3f, 0.0f, 0.0f)));
	CHECK(Math::abs(outer - inner) < 0.01f);
}

TEST_CASE("[NextGI][GlobalSDF] A brick never reads past an instance it does not list") {
	// A brick composites only the instances whose bounds overlap its cull box. Past their
	// bounds their distances keep growing, so a voxel far from them read their distance even
	// where an unlisted instance just outside the cull box was nearer (the composite check
	// found 0.90 m stored beside a pillar 0.64 m away), and rays stepped into it.
	const MeshSDFData box = _box_sdf(Vector3(1.0f, 1.0f, 1.0f));
	Vector<NextGIGlobalSDF::Instance> instances;
	NextGIGlobalSDF::Instance below; // Its bounds reach into the cull box from below.
	below.field = &box;
	below.transform.origin = Vector3(0.4f, -0.9f, 0.4f);
	instances.push_back(below);
	NextGIGlobalSDF::Instance beside; // Its bounds stop just past the cull box on +x.
	beside.field = &box;
	beside.transform.origin = Vector3(2.0f, 0.4f, 0.4f);
	instances.push_back(beside);

	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i brick(8, 8, 8);
	const AABB cull_box = NextGIGlobalSDF::brick_cull_box(0, origin, brick);
	CHECK(cull_box.is_equal_approx(AABB(Vector3(-0.1f, -0.1f, -0.1f), Vector3(1.0f, 1.0f, 1.0f))));
	uint32_t culled[2] = {};
	const uint32_t culled_count = NextGIGlobalSDF::cull_instances_for_brick(instances, 0, origin, brick, culled, 2);
	REQUIRE_EQ(culled_count, 1u);
	REQUIRE_EQ(culled[0], 0u);
	// The cap's clearance is the gap to the unlisted instance's bounds, here on +x only.
	const AABB beside_bounds = beside.transform.xform(box.bounds);
	const float clearance = NextGIGlobalSDF::brick_clearance(instances, 0, origin, brick);
	CHECK(clearance > 0.0f);
	CHECK(clearance < NextGIGlobalSDF::clearance_radius(0));
	CHECK(Math::is_equal_approx(clearance, beside_bounds.position.x - cull_box.get_end().x));
	NextGIGlobalSDF::BrickSplit split;
	NextGIGlobalSDF::compose_brick_split(instances, culled, culled_count, 0, origin, brick, split);

	const float voxel = NextGIGlobalSDF::level_voxel_size(0);
	float worst_listed_excess = 0.0f;
	for (uint32_t index = 0; index < NextGIGlobalSDF::BRICK_VOXELS; index++) {
		const Vector3i local(int(index % 8), int((index / 8) % 8), int(index / 64));
		const Vector3 world = NextGIGlobalSDF::voxel_center(0, origin, brick * 8 + local);
		const float stored = MIN(split.static_distance[index], split.dynamic_distance[index]);
		const float nearest = NextGIGlobalSDF::reference_distance(instances, world);
		const float listed = NextGIGlobalSDF::sample_instance(instances[0], world);
		worst_listed_excess = MAX(worst_listed_excess, listed - nearest);
		// Never past the nearest instance, listed or not.
		CHECK(stored <= nearest + 0.25f * voxel);
		// The CPU twin the GPU composite is validated against.
		CHECK(Math::is_equal_approx(stored, NextGIGlobalSDF::composite_distance(instances, culled, culled_count, cull_box, clearance, world)));
		// The cap is at least 1.5 voxels at a voxel center, so it never makes a surface (the hit
		// band is half a voxel, a voxel for two-sided coverage).
		if (nearest >= 1.5f * voxel) {
			CHECK(stored >= 1.5f * voxel - 1.0e-4f);
		}
	}
	// The listed instance alone reads well past the unlisted one somewhere in the brick.
	CHECK(worst_listed_excess > 0.3f);
}

TEST_CASE("[NextGI][GlobalSDF] A brick beside open space keeps its listed distances") {
	// The cap once held every voxel to 1.5-4.5 voxels of its level, wherever the nearest
	// unlisted instance was. Rays near a large surface then crept through its resident bricks
	// about 3 voxels a step and ran out of the 64-step budget after 25-50 m; a ray out of
	// steps reports a hit, so the low sun cast false shadows on the open floor. With nothing
	// else within the clearance radius, the cap stays above the floor's own distance.
	const MeshSDFData slab = _box_sdf(Vector3(6.0f, 0.2f, 6.0f));
	Vector<NextGIGlobalSDF::Instance> instances;
	NextGIGlobalSDF::Instance ground;
	ground.field = &slab;
	ground.transform.origin = Vector3(0.4f, -0.1f, 0.4f); // Its top is the brick's bottom face.
	instances.push_back(ground);

	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i brick(8, 8, 8);
	const float voxel = NextGIGlobalSDF::level_voxel_size(0);
	uint32_t culled[1] = {};
	const uint32_t culled_count = NextGIGlobalSDF::cull_instances_for_brick(instances, 0, origin, brick, culled, 1);
	REQUIRE_EQ(culled_count, 1u);
	const float clearance = NextGIGlobalSDF::brick_clearance(instances, 0, origin, brick);
	CHECK(Math::is_equal_approx(clearance, NextGIGlobalSDF::clearance_radius(0)));
	CHECK(clearance >= 7.0f * voxel - 1.0e-4f);
	NextGIGlobalSDF::BrickSplit split;
	NextGIGlobalSDF::compose_brick_split(instances, culled, culled_count, 0, origin, brick, split);
	uint32_t past_old_cap = 0;
	for (uint32_t index = 0; index < NextGIGlobalSDF::BRICK_VOXELS; index++) {
		const Vector3i local(int(index % 8), int((index / 8) % 8), int(index / 64));
		const Vector3 world = NextGIGlobalSDF::voxel_center(0, origin, brick * 8 + local);
		const float listed = NextGIGlobalSDF::sample_instance(instances[0], world);
		const float stored = MIN(split.static_distance[index], split.dynamic_distance[index]);
		// Every voxel steps the floor's full distance.
		CHECK(Math::is_equal_approx(stored, listed));
		// Where the cull box alone capped it (1.5 voxels at its faces, 4.5 at its center).
		past_old_cap += listed > NextGIGlobalSDF::brick_cap(NextGIGlobalSDF::brick_cull_box(0, origin, brick), 0.0f, world) + 1.0e-4f ? 1 : 0;
	}
	CHECK(past_old_cap > 100u);
}

TEST_CASE("[NextGI][GlobalSDF] Non-uniformly scaled instances read world distances") {
	// A scaled unit box is the common way to build floors and walls. The largest axis
	// scale overestimated distances (0.3 m above a 40 x 0.2 x 40 floor read about 28 m,
	// so rays tunneled), and the smallest one alone reads a stretched box's ends meters
	// too close. With the oriented-bounds bound (plan L4) a box reads its true distance.
	const MeshSDFData box = _box_sdf(Vector3(1.0f, 1.0f, 1.0f));
	const Basis bases[] = {
		Basis().scaled(Vector3(40.0f, 0.2f, 40.0f)),
		Basis().scaled(Vector3(0.3f, 2.0f, 4.0f)),
		Basis::from_euler(Vector3(0.3f, 0.8f, -0.2f)) * Basis().scaled(Vector3(6.0f, 0.25f, 1.5f)),
	};
	for (const Basis &basis : bases) {
		NextGIGlobalSDF::Instance instance;
		instance.field = &box;
		instance.transform = Transform3D(basis, Vector3(1.0f, 2.0f, -3.0f));
		const Vector4 axis_scale = NextGIGlobalSDF::instance_axis_scale(basis);
		const float smallest = axis_scale.w;
		CHECK(Math::is_equal_approx(smallest, MIN(axis_scale.x, MIN(axis_scale.y, axis_scale.z))));
		const float tolerance = 0.01f + 2.0f * _quantum(box, smallest);
		// Straight out of every face, inside and past the mesh SDF's bounds: the true distance.
		const float heights[] = { 0.05f, 0.3f, 1.0f, 2.5f };
		for (int axis = 0; axis < 3; axis++) {
			const Vector3 normal = basis.get_column(axis).normalized();
			const Vector3 face = instance.transform.origin + basis.get_column(axis) * 0.5f;
			for (const float height : heights) {
				const float sampled = NextGIGlobalSDF::sample_instance(instance, face + normal * height);
				CHECK(Math::abs(sampled - height) <= tolerance);
			}
			// Inside, half way to the center: negative.
			CHECK(NextGIGlobalSDF::sample_instance(instance, instance.transform.origin + basis.get_column(axis) * 0.25f) < 0.0f);
		}
		// Around the box, near and far: never farther than the true distance, and outside the
		// box (its oriented bounds) the true distance.
		RandomNumberGenerator rng;
		rng.set_seed(186);
		uint32_t outside = 0;
		for (int sample = 0; sample < 400; sample++) {
			const Vector3 local(rng.randf_range(-1.5f, 1.5f), rng.randf_range(-1.5f, 1.5f), rng.randf_range(-1.5f, 1.5f));
			const Vector3 world = instance.transform.xform(local);
			const float truth = _oriented_box_distance(instance.transform, world);
			const float sampled = NextGIGlobalSDF::sample_instance(instance, world);
			if (truth <= 0.0f) {
				CHECK(sampled <= tolerance);
				continue;
			}
			outside++;
			CHECK(Math::abs(sampled - truth) <= tolerance);
		}
		CHECK(outside > 200);
	}
}

TEST_CASE("[NextGI][GlobalSDF] Non-uniformly scaled curved meshes keep a lower bound") {
	// An ellipsoid from a scaled sphere: inside its bounds the distance may read closer
	// than it is, never farther (brute force over points on the ellipsoid).
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(0.5f);
	sphere->set_height(1.0f);
	MeshSDFSettings settings;
	settings.voxels_per_meter = 16.0f;
	settings.max_voxels_per_axis = 32;
	settings.prefer_static_raycaster = false;
	MeshSDFData field;
	REQUIRE(MeshSDFBuilder::build(sphere->get_faces(), settings, field) == OK);
	NextGIGlobalSDF::Instance instance;
	instance.field = &field;
	instance.transform = Transform3D(Basis::from_euler(Vector3(0.0f, 0.5f, 0.0f)) * Basis().scaled(Vector3(3.0f, 1.0f, 0.5f)), Vector3(0.0f, 1.0f, 0.0f));
	LocalVector<Vector3> surface;
	for (int u = 0; u <= 120; u++) {
		for (int v = 0; v <= 60; v++) {
			const float azimuth = float(u) / 120.0f * Math::TAU;
			const float polar = float(v) / 60.0f * Math::PI;
			surface.push_back(instance.transform.xform(0.5f * Vector3(Math::sin(polar) * Math::cos(azimuth), Math::cos(polar), Math::sin(polar) * Math::sin(azimuth))));
		}
	}
	RandomNumberGenerator rng;
	rng.set_seed(188);
	uint32_t checked = 0;
	for (int sample = 0; sample < 150; sample++) {
		const Vector3 local(rng.randf_range(-1.2f, 1.2f), rng.randf_range(-1.2f, 1.2f), rng.randf_range(-1.2f, 1.2f));
		if (local.length() <= 0.55f) {
			continue;
		}
		const Vector3 world = instance.transform.xform(local);
		float truth = 1e30f;
		for (const Vector3 &point : surface) {
			truth = MIN(truth, world.distance_to(point));
		}
		checked++;
		// The sphere's faceting and the field's voxels: a few centimeters.
		CHECK(NextGIGlobalSDF::sample_instance(instance, world) <= truth + 0.04f);
	}
	CHECK(checked > 100);
}

TEST_CASE("[NextGI][GlobalSDF] Instance axis scales, sheared bases included") {
	const Basis rotated_scaled = Basis::from_euler(Vector3(0.4f, -1.1f, 0.25f)) * Basis().scaled(Vector3(2.0f, 0.5f, 3.0f));
	const Vector4 axis_scale = NextGIGlobalSDF::instance_axis_scale(rotated_scaled);
	CHECK(axis_scale.is_equal_approx(Vector4(2.0f, 0.5f, 3.0f, 0.5f)));
	// A sheared basis: w is its smallest singular value, negative to mark it sheared. The
	// brute-force minimum of |B v| over unit vectors finds the same value.
	Basis sheared;
	sheared.set_column(0, Vector3(1.0f, 0.0f, 0.0f));
	sheared.set_column(1, Vector3(0.8f, 1.0f, 0.0f));
	sheared.set_column(2, Vector3(0.0f, 0.3f, 2.0f));
	const Vector4 sheared_scale = NextGIGlobalSDF::instance_axis_scale(sheared);
	REQUIRE(sheared_scale.w < 0.0f);
	// xyz: the distance between each local axis's face planes per local unit, one over the
	// length of that axis's row of the inverse basis (the row is the face normal over it).
	const Basis inverse = sheared.inverse();
	for (int axis = 0; axis < 3; axis++) {
		CHECK(Math::is_equal_approx(sheared_scale[axis], 1.0f / inverse.rows[axis].length()));
		CHECK(sheared_scale[axis] >= -sheared_scale.w);
	}
	float smallest = 1e30f;
	const int count = 20000;
	for (int index = 0; index < count; index++) {
		// Fibonacci sphere.
		const float y = 1.0f - 2.0f * (float(index) + 0.5f) / float(count);
		const float radius = Math::sqrt(MAX(1.0f - y * y, 0.0f));
		const float angle = float(index) * 2.39996323f;
		smallest = MIN(smallest, sheared.xform(Vector3(Math::cos(angle) * radius, y, Math::sin(angle) * radius)).length());
	}
	CHECK(-sheared_scale.w <= smallest + 1.0e-4f);
	CHECK(-sheared_scale.w >= smallest - 0.02f);

	// A sheared instance's distance stays a lower bound of the distance to its
	// parallelepiped (brute force over points on its six faces).
	const MeshSDFData box = _box_sdf(Vector3(1.0f, 1.0f, 1.0f));
	NextGIGlobalSDF::Instance instance;
	instance.field = &box;
	instance.transform = Transform3D(sheared, Vector3(0.0f, 1.0f, 0.0f));
	LocalVector<Vector3> surface;
	for (int axis = 0; axis < 3; axis++) {
		for (int side = 0; side < 2; side++) {
			for (int u = 0; u <= 40; u++) {
				for (int v = 0; v <= 40; v++) {
					Vector3 local;
					local[axis] = side == 0 ? -0.5f : 0.5f;
					local[(axis + 1) % 3] = float(u) / 40.0f - 0.5f;
					local[(axis + 2) % 3] = float(v) / 40.0f - 0.5f;
					surface.push_back(instance.transform.xform(local));
				}
			}
		}
	}
	RandomNumberGenerator rng;
	rng.set_seed(187);
	for (int sample = 0; sample < 60; sample++) {
		const Vector3 local(rng.randf_range(-1.6f, 1.6f), rng.randf_range(-1.6f, 1.6f), rng.randf_range(-1.6f, 1.6f));
		if (MAX(Math::abs(local.x), MAX(Math::abs(local.y), Math::abs(local.z))) <= 0.5f) {
			continue;
		}
		const Vector3 world = instance.transform.xform(local);
		float truth = 1e30f;
		for (const Vector3 &point : surface) {
			truth = MIN(truth, world.distance_to(point));
		}
		CHECK(NextGIGlobalSDF::sample_instance(instance, world) <= truth + 0.05f);
	}
}

TEST_CASE("[NextGI][GlobalSDF] A sheared instance reads close to its true distance") {
	// A rotated node under a non-uniformly scaled parent has a sheared basis: here a unit box
	// scaled (7, 1.5, 0.3), turned 30 degrees about Y, under a parent scaled (1.5, 1, 1). Its
	// column lengths are 9.74, 1.5 and 0.34 and its smallest singular value 0.32. That value on
	// every axis read a median of 7% of the true distance outside the box, so the trace stopped
	// in the air around it. Each axis's face planes bound the distance to the box (plan L4's
	// oriented bounds) without that collapse.
	const MeshSDFData box = _box_sdf(Vector3(1.0f, 1.0f, 1.0f));
	const Basis basis = Basis().scaled(Vector3(1.5f, 1.0f, 1.0f)) * Basis::from_euler(Vector3(0.0f, Math::PI / 6.0f, 0.0f)) * Basis().scaled(Vector3(7.0f, 1.5f, 0.3f));
	NextGIGlobalSDF::Instance instance;
	instance.field = &box;
	instance.transform = Transform3D(basis, Vector3(0.0f, 0.75f, -4.2f));
	const Vector4 axis_scale = NextGIGlobalSDF::instance_axis_scale(basis);
	REQUIRE(axis_scale.w < 0.0f);
	const float tolerance = 0.01f + 2.0f * _quantum(box, -axis_scale.w);
	// Straight out of the middle of each face, inside and past the mesh SDF's bounds: the
	// nearest point is the face's center, so the face plane's distance is the true distance.
	const Basis inverse = basis.inverse();
	const float heights[] = { 0.05f, 0.3f, 1.0f, 2.5f };
	for (int axis = 0; axis < 3; axis++) {
		for (int side = -1; side <= 1; side += 2) {
			Vector3 center_local;
			center_local[axis] = 0.5f * float(side);
			const Vector3 normal = inverse.rows[axis].normalized() * float(side);
			for (const float height : heights) {
				const Vector3 world = instance.transform.xform(center_local) + normal * height;
				CHECK(_parallelepiped_distance(instance.transform, world) == doctest::Approx(height).epsilon(0.001));
				CHECK(Math::abs(NextGIGlobalSDF::sample_instance(instance, world) - height) <= tolerance);
			}
		}
	}
	// Around the box, near and far: never farther than the true distance, and most of it.
	RandomNumberGenerator rng;
	rng.set_seed(186);
	LocalVector<float> ratios;
	for (int sample = 0; sample < 600; sample++) {
		const Vector3 local(rng.randf_range(-1.5f, 1.5f), rng.randf_range(-1.5f, 1.5f), rng.randf_range(-1.5f, 1.5f));
		const Vector3 world = instance.transform.xform(local);
		const float truth = _parallelepiped_distance(instance.transform, world);
		const float sampled = NextGIGlobalSDF::sample_instance(instance, world);
		CHECK(sampled <= truth + tolerance);
		if (truth > 0.05f) {
			ratios.push_back(sampled / truth);
		}
	}
	REQUIRE(ratios.size() > 300u);
	ratios.sort();
	CHECK(ratios[ratios.size() / 2] >= 0.85f);
}

TEST_CASE("[NextGI][SWRT] Microbenchmark fixture segments are lit or cross a box core") {
	// The microbenchmark checks its trace against these. Checked here by sampling along each
	// segment against the boxes' analytic distance, not by the generator's own box tests.
	LocalVector<AABB> boxes;
	boxes.push_back(AABB(Vector3(-12.0f, -0.2f, -12.0f), Vector3(24.0f, 0.2f, 24.0f)));
	boxes.push_back(AABB(Vector3(11.9f, 0.0f, -12.0f), Vector3(0.2f, 6.0f, 24.0f)));
	boxes.push_back(AABB(Vector3(-0.5f, 0.0f, -0.5f), Vector3(1.0f, 1.0f, 1.0f)));
	LocalVector<NextGISWRTTrace::ShadowRequest> requests;
	LocalVector<RendererRD::NextGILight> lights;
	LocalVector<uint8_t> occluded;
	NextGISWRTTrace::fixture_segments(boxes, 32, requests, lights, occluded);
	REQUIRE_EQ(requests.size(), 64u);
	REQUIRE_EQ(lights.size(), 64u);
	REQUIRE_EQ(occluded.size(), 64u);
	auto distance_to_boxes = [&](const Vector3 &p_point) {
		float nearest = 1e30f;
		for (const AABB &box : boxes) {
			const Vector3 center = box.get_center();
			nearest = MIN(nearest, _oriented_box_distance(Transform3D(Basis().scaled(box.size), center), p_point));
		}
		return nearest;
	};
	uint32_t occluded_count = 0;
	for (uint32_t index = 0; index < requests.size(); index++) {
		const Vector3 from(requests[index].position_offset[0], requests[index].position_offset[1], requests[index].position_offset[2]);
		CHECK_EQ(int(requests[index].normal_light[3]), int(index));
		CHECK(Math::is_equal_approx(lights[index].direction_type[3], 1.0f));
		const Vector3 to(lights[index].position_inv_range[0], lights[index].position_inv_range[1], lights[index].position_inv_range[2]);
		CHECK(distance_to_boxes(from) >= NextGISWRTTrace::FIXTURE_CLEARANCE);
		CHECK(distance_to_boxes(to) >= NextGISWRTTrace::FIXTURE_CLEARANCE);
		float nearest = 1e30f;
		for (int step = 0; step <= 400; step++) {
			nearest = MIN(nearest, distance_to_boxes(from.lerp(to, float(step) / 400.0f)));
		}
		if (occluded[index]) {
			occluded_count++;
			CHECK(nearest <= -NextGISWRTTrace::FIXTURE_CORE_INSET + 0.05f);
		} else {
			CHECK(nearest >= NextGISWRTTrace::FIXTURE_CLEARANCE - 0.05f);
		}
	}
	CHECK_EQ(occluded_count, 32u);
}

TEST_CASE("[NextGI][SWRT] Software views trace once per single-view render") {
	CHECK(RendererRD::NextGI::view_traces_swrt(RendererRD::NextGI::TIER_SOFTWARE, false, true, false, 1));
	// XR: depth and normal-roughness are arrays the trace does not bind.
	CHECK_FALSE(RendererRD::NextGI::view_traces_swrt(RendererRD::NextGI::TIER_SOFTWARE, false, true, false, 2));
	CHECK_FALSE(RendererRD::NextGI::view_traces_swrt(RendererRD::NextGI::TIER_HWRT, false, true, false, 1));
	CHECK_FALSE(RendererRD::NextGI::view_traces_swrt(RendererRD::NextGI::TIER_SOFTWARE, true, true, false, 1));
}

TEST_CASE("[NextGI][SWRT] SWRT_REDUCED ends when the tier leaves SOFTWARE") {
	RendererRD::NextGI next_gi;
	RendererRD::NextGI::TierInputs software;
	software.project_enabled = true;
	software.ray_query = false;
	software.driver_reason = RendererRD::NextGI::NO_RAY_QUERY;
	RendererRD::NextGI::TierInputs hardware = software;
	hardware.ray_query = true;
	hardware.ray_query_hw = true;
	hardware.driver_reason = RendererRD::NextGI::TIER_REASON_NONE;
	String line;
	REQUIRE(next_gi.update_tier(software, RendererRD::NextGI::QUALITY_HIGH, line));
	next_gi.note_swrt_reduced(4);
	CHECK(next_gi.get_tier_result().swrt_reduced);
	REQUIRE(next_gi.update_tier(software, RendererRD::NextGI::QUALITY_HIGH, line));
	CHECK(line.contains("SWRT_REDUCED"));
	// The software tracer goes with the tier; the hardware line does not carry its flag.
	REQUIRE(next_gi.update_tier(hardware, RendererRD::NextGI::QUALITY_HIGH, line));
	CHECK(line.begins_with("NextGI: tier=HWRT"));
	CHECK_FALSE(line.contains("SWRT_REDUCED"));
	CHECK_FALSE(next_gi.get_tier_result().swrt_reduced);
	// Back on SWRT the new tracer starts with every level.
	REQUIRE(next_gi.update_tier(software, RendererRD::NextGI::QUALITY_HIGH, line));
	CHECK_FALSE(line.contains("SWRT_REDUCED"));
}

TEST_CASE("[NextGI][GlobalSDF] Scrolling one level keeps every other level's slots") {
	NextGIGlobalSDFGPU gpu(6);
	float voxels[NextGIGlobalSDF::BRICK_VOXELS] = {};
	REQUIRE(gpu.upload_brick(0, Vector3i(0, 0, 0), voxels));
	REQUIRE(gpu.upload_brick(0, Vector3i(5, 5, 5), voxels));
	REQUIRE(gpu.upload_brick(1, Vector3i(2, 2, 2), voxels));
	REQUIRE(gpu.upload_brick(2, Vector3i(3, 3, 3), voxels));
	const int32_t slot_1 = gpu.get_slot(1, Vector3i(2, 2, 2));
	const int32_t slot_2 = gpu.get_slot(2, Vector3i(3, 3, 3));
	const int32_t kept = gpu.get_slot(0, Vector3i(5, 5, 5));
	// Level 0 scrolls one brick on +x: brick 0 leaves, brick 5 shifts to 4.
	gpu.scroll_level(0, Vector3i(1, 0, 0));
	CHECK_EQ(gpu.get_used_bricks(), 3u);
	CHECK_EQ(gpu.get_slot(0, Vector3i(4, 5, 5)), kept);
	CHECK_EQ(gpu.get_slot(1, Vector3i(2, 2, 2)), slot_1);
	CHECK_EQ(gpu.get_slot(2, Vector3i(3, 3, 3)), slot_2);
	// The three free slots stay distinct from the resident ones.
	for (int index = 0; index < 3; index++) {
		REQUIRE(gpu.upload_brick(3, Vector3i(index, 0, 0), voxels));
		const int32_t slot = gpu.get_slot(3, Vector3i(index, 0, 0));
		CHECK(slot != kept);
		CHECK(slot != slot_1);
		CHECK(slot != slot_2);
	}
	CHECK_EQ(gpu.get_used_bricks(), 6u);
	CHECK_FALSE(gpu.upload_brick(3, Vector3i(9, 0, 0), voxels));
}

TEST_CASE("[NextGI][GlobalSDF] Instance bins match the per-brick cull") {
	const MeshSDFData rock = _box_sdf(Vector3(1.0f, 0.6f, 1.4f));
	const MeshSDFData slab = _box_sdf(Vector3(6.0f, 0.2f, 6.0f));
	RandomNumberGenerator rng;
	rng.set_seed(186);
	Vector<NextGIGlobalSDF::Instance> instances;
	for (int index = 0; index < 120; index++) {
		NextGIGlobalSDF::Instance instance;
		instance.field = index % 7 == 0 ? &slab : &rock;
		instance.transform = Transform3D(Basis::from_euler(Vector3(0.0f, rng.randf_range(0.0f, 6.28f), 0.0f)).scaled(Vector3(1.0f, 1.0f, 1.0f) * rng.randf_range(0.5f, 2.0f)), Vector3(rng.randf_range(-40.0f, 40.0f), rng.randf_range(-2.0f, 6.0f), rng.randf_range(-40.0f, 40.0f)));
		instances.push_back(instance);
	}
	LocalVector<uint32_t> culled;
	culled.resize(instances.size());
	for (uint32_t level = 0; level < 3; level++) {
		const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(3.0f, 1.0f, -2.0f), level);
		LocalVector<uint32_t> offsets;
		LocalVector<uint32_t> indices;
		NextGIGlobalSDF::bin_instances(instances, level, origin, offsets, indices);
		REQUIRE_EQ(offsets.size(), NextGIGlobalSDF::BRICKS_PER_LEVEL + 1);
		NextGIGlobalSDF::ClearanceIndex clearance_index;
		NextGIGlobalSDF::build_clearance_index(instances, level, origin, offsets, indices, clearance_index);
		uint32_t occupied = 0;
		uint32_t near_unlisted = 0;
		uint32_t near_outside = 0;
		for (uint32_t brick = 0; brick < NextGIGlobalSDF::BRICKS_PER_LEVEL; brick++) {
			const Vector3i coordinate(int(brick % 16), int((brick / 16) % 16), int(brick / 256));
			const uint32_t count = NextGIGlobalSDF::cull_instances_for_brick(instances, level, origin, coordinate, culled.ptr(), culled.size());
			CHECK_EQ(offsets[brick + 1] - offsets[brick], count);
			for (uint32_t i = 0; i < count && offsets[brick] + i < indices.size(); i++) {
				CHECK_EQ(indices[offsets[brick] + i], culled[i]);
			}
			occupied += count > 0 ? 1 : 0;
			// GodotNext: ps_swrt_trace (#186). The composite's clearance searches only the bins
			// around the brick (update_bricks()); its CPU twin searches every instance.
			const float binned = NextGIGlobalSDF::brick_clearance(instances, level, origin, coordinate, &clearance_index);
			const float every = NextGIGlobalSDF::brick_clearance(instances, level, origin, coordinate);
			CHECK_EQ(binned, every);
			const bool inner = coordinate.x > 0 && coordinate.y > 0 && coordinate.z > 0 && coordinate.x < 15 && coordinate.y < 15 && coordinate.z < 15;
			const bool limited = every < NextGIGlobalSDF::clearance_radius(level);
			near_unlisted += inner && limited ? 1 : 0;
			near_outside += !inner && limited ? 1 : 0;
		}
		CHECK(occupied > 0);
		// Bricks whose clearance an unlisted instance sets, inside the level and on its shell
		// (where instances reaching out of the level count too), so every search path is used.
		CHECK(near_unlisted > 20);
		CHECK(near_outside > 20);
		CHECK(clearance_index.outside.size() > 0);
	}
}

TEST_CASE("[NextGI][GlobalSDF] Instance bins keep bounds that end at a cull box face") {
	// Bounds that end within float rounding of a brick's cull box face: the per-brick cull
	// keeps the instance, and the bins must too. The bins' brick range once floored past the
	// brick, so the GPU composite missed a pillar its CPU twin listed and, with the cap's
	// clearance, read 0.25 m past it.
	const MeshSDFData pillar = _box_sdf(Vector3(0.6f, 2.4f, 0.6f));
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(0.3f, 3.2f, 4.2f), 0);
	Vector<NextGIGlobalSDF::Instance> instances;
	instances.resize(1);
	instances.write[0].field = &pillar;
	LocalVector<uint32_t> offsets;
	LocalVector<uint32_t> indices;
	uint32_t touching = 0;
	for (int axis = 0; axis < 3; axis++) {
		for (int brick = 1; brick < 15; brick++) {
			Vector3i coordinate(7, 5, 3);
			coordinate[axis] = brick;
			const AABB cull_box = NextGIGlobalSDF::brick_cull_box(0, origin, coordinate);
			// Put the bounds' low face on the cull box's high face, then step it by single ulps.
			Transform3D transform;
			transform.origin = NextGIGlobalSDF::voxel_center(0, origin, coordinate * 8 + Vector3i(4, 4, 4)) - pillar.bounds.get_center();
			transform.origin[axis] = cull_box.get_end()[axis] - pillar.bounds.position[axis];
			for (int step = 0; step < 12; step++) {
				instances.write[0].transform = transform;
				const AABB bounds = transform.xform(pillar.bounds);
				if (bounds.intersects(cull_box) && bounds.position[axis] > cull_box.get_end()[axis] - 1.0e-4f) {
					touching++;
					NextGIGlobalSDF::bin_instances(instances, 0, origin, offsets, indices);
					const uint32_t index = uint32_t(coordinate.x) + 16u * (uint32_t(coordinate.y) + 16u * uint32_t(coordinate.z));
					CHECK_EQ(offsets[index + 1] - offsets[index], 1u);
				}
				transform.origin[axis] = std::nextafter(transform.origin[axis], -1.0e30f);
			}
		}
	}
	CHECK(touching > 50u);
}

TEST_CASE("[NextGI][SWRT] Clipmaps build every brick once, then only scroll edges and dirty volumes") {
	NextGIGlobalSDFClipmaps clipmaps;
	NextGIGlobalSDFClipmaps::Plan plan;
	const Vector3 camera(0.05f, 1.0f, 0.05f);
	clipmaps.plan_level(0, camera, 0, plan);
	CHECK(plan.update);
	CHECK(plan.full);
	CHECK_EQ(plan.bricks.size(), NextGIGlobalSDF::BRICKS_PER_LEVEL);
	clipmaps.commit_level(0, plan, true);
	CHECK(clipmaps.is_level_valid(0));

	// A still camera rebuilds nothing.
	clipmaps.plan_level(0, camera, 1, plan);
	CHECK(plan.update);
	CHECK_FALSE(plan.full);
	CHECK_EQ(plan.bricks.size(), 0u);
	clipmaps.commit_level(0, plan, true);

	// One brick of camera travel on +x shifts the table and rebuilds only the new slab.
	const float brick = NextGIGlobalSDF::level_voxel_size(0) * float(NextGIGlobalSDF::BRICK_SIZE);
	const Vector3 moved = camera + Vector3(brick, 0.0f, 0.0f);
	clipmaps.plan_level(0, moved, 2, plan);
	CHECK_EQ(plan.scroll, Vector3i(1, 0, 0));
	CHECK_EQ(plan.bricks.size(), NextGIGlobalSDF::PAGE_SIZE * NextGIGlobalSDF::PAGE_SIZE);
	for (const Vector3i &edge : plan.bricks) {
		CHECK_EQ(edge.x, int(NextGIGlobalSDF::PAGE_SIZE) - 1);
	}
	clipmaps.commit_level(0, plan, true);
	CHECK_EQ(clipmaps.get_origin(0), NextGIGlobalSDF::scrolled_origin(moved, 0));

	// A dirty volume rebuilds the bricks around it, grown by the mesh-SDF band.
	const AABB rock(Vector3(1.0f, 0.5f, 1.0f), Vector3(0.4f, 0.4f, 0.4f));
	clipmaps.add_dirty_volumes(&rock, 1);
	clipmaps.plan_level(0, moved, 3, plan);
	CHECK_FALSE(plan.full);
	CHECK(plan.bricks.size() > 0);
	const Vector3i origin = clipmaps.get_origin(0);
	const AABB grown = rock.grow(NextGIGlobalSDFClipmaps::dirty_margin(0));
	for (const Vector3i &dirty : plan.bricks) {
		const AABB brick_bounds(Vector3(origin + dirty * int(NextGIGlobalSDF::BRICK_SIZE)) * NextGIGlobalSDF::level_voxel_size(0), Vector3(brick, brick, brick));
		CHECK(brick_bounds.intersects(grown.grow(1.0e-3f)));
	}
	// The brick that holds the rock is among them.
	const Vector3i rock_brick = NextGIGlobalSDF::world_to_voxel(0, origin, rock.get_center()) / int(NextGIGlobalSDF::BRICK_SIZE);
	bool found = false;
	for (const Vector3i &dirty : plan.bricks) {
		found = found || dirty == rock_brick;
	}
	CHECK(found);
	clipmaps.commit_level(0, plan, true);
	CHECK_EQ(clipmaps.get_pending_dirty(0), 0u);

	// Level 1 updates every second frame; a dirty volume waits for it.
	clipmaps.plan_level(1, moved, 0, plan);
	clipmaps.commit_level(1, plan, true);
	clipmaps.add_dirty_volumes(&rock, 1);
	clipmaps.plan_level(1, moved, 5, plan);
	CHECK_FALSE(plan.update);
	clipmaps.commit_level(1, plan, true);
	CHECK_EQ(clipmaps.get_pending_dirty(1), 1u);
	clipmaps.plan_level(1, moved, 6, plan);
	CHECK(plan.update);
	CHECK(plan.bricks.size() > 0);
	clipmaps.commit_level(1, plan, true);
	CHECK_EQ(clipmaps.get_pending_dirty(1), 0u);

	// A failed rebuild and a reset start the level over in full.
	clipmaps.plan_level(0, moved, 8, plan);
	clipmaps.commit_level(0, plan, false);
	CHECK_FALSE(clipmaps.is_level_valid(0));
	clipmaps.plan_level(0, moved, 9, plan);
	CHECK(plan.full);
	clipmaps.commit_level(0, plan, true);
	clipmaps.reset();
	clipmaps.plan_level(0, moved, 10, plan);
	CHECK(plan.full);
}

TEST_CASE("[NextGI][SWRT] An instance moved beside a brick it does not list rebuilds the brick's cap") {
	// A brick's cap depends on the instances within its clearance radius, past its cull box.
	// When one moves there, the dirty volume must reach the brick, or the brick keeps a cap
	// from before and reads past the instance (rays would step through it).
	const MeshSDFData slab = _box_sdf(Vector3(6.0f, 0.2f, 6.0f));
	const MeshSDFData rock = _box_sdf(Vector3(1.0f, 0.6f, 1.4f));
	const uint32_t level = 2; // 0.4 m voxels: the clearance radius (2.8 m) passes the 1 m band margin.
	const float voxel = NextGIGlobalSDF::level_voxel_size(level);
	const Vector3 camera;
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(camera, level);
	const Vector3i brick(8, 8, 8); // World [0, 3.2]^3, on the slab.

	Vector<NextGIGlobalSDF::Instance> before;
	NextGIGlobalSDF::Instance ground;
	ground.field = &slab;
	ground.transform.origin = Vector3(1.6f, -0.1f, 1.6f);
	before.push_back(ground);
	NextGIGlobalSDF::Instance stone;
	stone.field = &rock;
	stone.dynamic = true;
	stone.transform.origin = Vector3(18.0f, 3.0f, 18.0f);
	before.push_back(stone);
	Vector<NextGIGlobalSDF::Instance> after = before;
	// Beside the brick's top on +x: its bounds stay 1.2 m past the cull box.
	after.write[1].transform.origin = Vector3(5.8f, 3.0f, 1.6f);
	const AABB cull_box = NextGIGlobalSDF::brick_cull_box(level, origin, brick);
	const AABB moved = after[1].transform.xform(rock.bounds);
	REQUIRE_FALSE(moved.intersects(cull_box));
	REQUIRE(NextGIGlobalSDF::brick_clearance(after, level, origin, brick) < NextGIGlobalSDF::brick_clearance(before, level, origin, brick));

	// The level as built before the move: one composite per resident brick.
	HashMap<uint32_t, NextGIGlobalSDF::BrickSplit> stored;
	LocalVector<uint32_t> culled;
	culled.resize(2);
	auto compose = [&](const Vector<NextGIGlobalSDF::Instance> &p_instances, const Vector3i &p_brick) {
		const uint32_t key = uint32_t(p_brick.x) + 16u * (uint32_t(p_brick.y) + 16u * uint32_t(p_brick.z));
		const uint32_t count = NextGIGlobalSDF::cull_instances_for_brick(p_instances, level, origin, p_brick, culled.ptr(), culled.size());
		if (count == 0) {
			stored.erase(key);
			return;
		}
		NextGIGlobalSDF::BrickSplit split;
		NextGIGlobalSDF::compose_brick_split(p_instances, culled.ptr(), count, level, origin, p_brick, split);
		stored[key] = split;
	};
	NextGIGlobalSDFClipmaps clipmaps;
	NextGIGlobalSDFClipmaps::Plan plan;
	clipmaps.plan_level(level, camera, 0, plan);
	REQUIRE(plan.full);
	for (const Vector3i &rebuilt : plan.bricks) {
		compose(before, rebuilt);
	}
	clipmaps.commit_level(level, plan, true);
	REQUIRE(stored.has(8u + 16u * (8u + 16u * 8u)));

	// The move dirties the rock's old and new mesh-SDF bounds, as the SWRT feed does.
	const AABB volumes[2] = { before[1].transform.xform(rock.bounds), moved };
	clipmaps.add_dirty_volumes(volumes, 2);
	clipmaps.plan_level(level, camera, 4, plan);
	REQUIRE(plan.update);
	REQUIRE_FALSE(plan.full);
	for (const Vector3i &rebuilt : plan.bricks) {
		compose(after, rebuilt);
	}
	clipmaps.commit_level(level, plan, true);

	// No resident voxel reads past the moved rock (or anything else).
	uint32_t checked = 0;
	uint32_t overestimates = 0;
	for (const KeyValue<uint32_t, NextGIGlobalSDF::BrickSplit> &entry : stored) {
		const Vector3i coordinate(int(entry.key % 16), int((entry.key / 16) % 16), int(entry.key / 256));
		for (uint32_t index = 0; index < NextGIGlobalSDF::BRICK_VOXELS; index++) {
			const Vector3i local(int(index % 8), int((index / 8) % 8), int(index / 64));
			const Vector3 world = NextGIGlobalSDF::voxel_center(level, origin, coordinate * 8 + local);
			const float value = MIN(entry.value.static_distance[index], entry.value.dynamic_distance[index]);
			overestimates += value > NextGIGlobalSDF::reference_distance(after, world) + 0.25f * voxel ? 1 : 0;
			checked++;
		}
	}
	CHECK(checked > 0u);
	CHECK_EQ(overestimates, 0u);
}

TEST_CASE("[NextGI][SWRT] A brick a scroll keeps holds the composite of its new place") {
	// A scroll shifts the resident bricks and rebuilds only the new edge, so a brick's
	// composite may depend only on its place in the world. The cap's clearance once stopped
	// at the level's edge: a brick that scrolled in kept the bare cull-box cap after it
	// moved inward, and the whole level lost the clearance as the camera travelled.
	const MeshSDFData slab = _box_sdf(Vector3(14.0f, 0.2f, 14.0f));
	Vector<NextGIGlobalSDF::Instance> instances;
	NextGIGlobalSDF::Instance ground;
	ground.field = &slab;
	ground.transform.origin = Vector3(0.0f, -0.1f, 0.0f);
	instances.push_back(ground);
	const float brick = NextGIGlobalSDF::level_voxel_size(0) * float(NextGIGlobalSDF::BRICK_SIZE);
	Vector3 camera(0.05f, 0.4f, 0.05f);
	NextGIGlobalSDFClipmaps clipmaps;
	NextGIGlobalSDFClipmaps::Plan plan;
	clipmaps.plan_level(0, camera, 0, plan);
	REQUIRE(plan.full);
	clipmaps.commit_level(0, plan, true);

	// One brick of travel on +x builds the new slab at x = 15, the floor's row among it.
	camera.x += brick;
	clipmaps.plan_level(0, camera, 1, plan);
	REQUIRE_EQ(plan.scroll, Vector3i(1, 0, 0));
	clipmaps.commit_level(0, plan, true);
	const Vector3i entered_origin = clipmaps.get_origin(0);
	const Vector3i entered(15, 8, 8);
	bool rebuilt_entered = false;
	for (const Vector3i &rebuilt : plan.bricks) {
		rebuilt_entered = rebuilt_entered || rebuilt == entered;
	}
	REQUIRE(rebuilt_entered);
	uint32_t culled[1] = {};
	REQUIRE_EQ(NextGIGlobalSDF::cull_instances_for_brick(instances, 0, entered_origin, entered, culled, 1), 1u);
	NextGIGlobalSDF::BrickSplit stored;
	NextGIGlobalSDF::compose_brick_split(instances, culled, 1, 0, entered_origin, entered, stored);
	const Vector3i world_brick = entered_origin / int(NextGIGlobalSDF::BRICK_SIZE) + entered;

	// Three more bricks of travel: the brick is shifted, never rebuilt.
	for (uint64_t frame = 2; frame <= 4; frame++) {
		camera.x += brick;
		clipmaps.plan_level(0, camera, frame, plan);
		REQUIRE_EQ(plan.scroll, Vector3i(1, 0, 0));
		const Vector3i kept = world_brick - plan.origin / int(NextGIGlobalSDF::BRICK_SIZE);
		for (const Vector3i &rebuilt : plan.bricks) {
			CHECK(rebuilt != kept);
		}
		clipmaps.commit_level(0, plan, true);
	}
	const Vector3i origin = clipmaps.get_origin(0);
	const Vector3i now = world_brick - origin / int(NextGIGlobalSDF::BRICK_SIZE);
	REQUIRE_EQ(now, Vector3i(12, 8, 8));
	CHECK_EQ(NextGIGlobalSDF::brick_clearance(instances, 0, origin, now), NextGIGlobalSDF::brick_clearance(instances, 0, entered_origin, entered));
	REQUIRE_EQ(NextGIGlobalSDF::cull_instances_for_brick(instances, 0, origin, now, culled, 1), 1u);
	NextGIGlobalSDF::BrickSplit fresh;
	NextGIGlobalSDF::compose_brick_split(instances, culled, 1, 0, origin, now, fresh);
	uint32_t differ = 0;
	for (uint32_t index = 0; index < NextGIGlobalSDF::BRICK_VOXELS; index++) {
		differ += Math::is_equal_approx(stored.static_distance[index], fresh.static_distance[index]) ? 0 : 1;
	}
	CHECK_EQ(differ, 0u);
}

TEST_CASE("[NextGI][GlobalSDF] A full level build plans the bricks nearest the camera first") {
	// A level that runs out of pool slots keeps the bricks it allocated first: the ones near
	// the camera (the level's center), not a corner of the level.
	NextGIGlobalSDFClipmaps clipmaps;
	NextGIGlobalSDFClipmaps::Plan plan;
	clipmaps.plan_level(1, Vector3(3.0f, 1.0f, -2.0f), 0, plan);
	REQUIRE(plan.full);
	REQUIRE_EQ(plan.bricks.size(), NextGIGlobalSDF::BRICKS_PER_LEVEL);
	const int page = int(NextGIGlobalSDF::PAGE_SIZE);
	// Twice the offset of a brick's center from the page's center, squared.
	auto distance = [](const Vector3i &p_brick) {
		const int size = int(NextGIGlobalSDF::PAGE_SIZE);
		return Vector3i(2 * p_brick.x + 1 - size, 2 * p_brick.y + 1 - size, 2 * p_brick.z + 1 - size).length_squared();
	};
	LocalVector<uint8_t> seen;
	seen.resize(NextGIGlobalSDF::BRICKS_PER_LEVEL);
	memset(seen.ptr(), 0, seen.size());
	uint32_t unique = 0;
	uint32_t out_of_order = 0;
	for (uint32_t index = 0; index < plan.bricks.size(); index++) {
		const Vector3i &brick = plan.bricks[index];
		const uint32_t key = uint32_t(brick.x + page * (brick.y + page * brick.z));
		unique += seen[key] == 0 ? 1 : 0;
		seen[key] = 1;
		out_of_order += index > 0 && distance(plan.bricks[index - 1]) > distance(brick) ? 1 : 0;
	}
	CHECK_EQ(unique, NextGIGlobalSDF::BRICKS_PER_LEVEL);
	CHECK_EQ(out_of_order, 0u);
	// One of the eight bricks around the center comes first, a corner last.
	CHECK_EQ(distance(plan.bricks[0]), 3);
	CHECK_EQ(distance(plan.bricks[plan.bricks.size() - 1]), 3 * (page - 1) * (page - 1));
}

TEST_CASE("[NextGI][GlobalSDF] A partial level marks its missing bricks until they get a slot") {
	// A brick that holds geometry but got no pool slot is PAGE_MISSING, not empty: the trace
	// reads a coarser level there (swrt_level_missing() in nextgi_sdf_trace_inc.glsl).
	NextGIGlobalSDFGPU gpu(2);
	float voxels[NextGIGlobalSDF::BRICK_VOXELS] = {};
	REQUIRE(gpu.upload_brick(0, Vector3i(1, 1, 1), voxels));
	gpu.mark_missing(0, Vector3i(1, 1, 1)); // A brick with a slot keeps it.
	CHECK(gpu.has_brick(0, Vector3i(1, 1, 1)));
	gpu.mark_missing(0, Vector3i(2, 1, 1));
	gpu.mark_missing(0, Vector3i(3, 1, 1));
	gpu.mark_missing(0, Vector3i(3, 1, 1));
	CHECK_EQ(gpu.get_slot(0, Vector3i(2, 1, 1)), NextGIGlobalSDFGPU::PAGE_MISSING);
	CHECK_FALSE(gpu.has_brick(0, Vector3i(2, 1, 1)));
	CHECK_EQ(gpu.get_missing_bricks(0), 2u);
	CHECK_EQ(gpu.get_missing_bricks(1), 0u);
	// A scroll moves them with the level; it never turns them into empty space.
	gpu.scroll_level(0, Vector3i(1, 0, 0));
	CHECK(gpu.has_brick(0, Vector3i(0, 1, 1)));
	CHECK_EQ(gpu.get_slot(0, Vector3i(1, 1, 1)), NextGIGlobalSDFGPU::PAGE_MISSING);
	CHECK_EQ(gpu.get_slot(0, Vector3i(2, 1, 1)), NextGIGlobalSDFGPU::PAGE_MISSING);
	CHECK_EQ(gpu.get_slot(0, Vector3i(3, 1, 1)), NextGIGlobalSDFGPU::PAGE_UNALLOCATED);
	CHECK_EQ(gpu.get_missing_bricks(0), 2u);
	// A brick that gets a slot is no longer missing; one that cannot stays missing.
	REQUIRE(gpu.upload_brick(0, Vector3i(1, 1, 1), voxels));
	CHECK_EQ(gpu.get_missing_bricks(0), 1u);
	CHECK_FALSE(gpu.upload_brick(0, Vector3i(2, 1, 1), voxels));
	CHECK_EQ(gpu.get_slot(0, Vector3i(2, 1, 1)), NextGIGlobalSDFGPU::PAGE_MISSING);
	// A scroll that moves a missing brick out of the level drops it.
	gpu.scroll_level(0, Vector3i(3, 0, 0));
	CHECK_EQ(gpu.get_missing_bricks(0), 0u);
	gpu.mark_missing(0, Vector3i(5, 5, 5));
	CHECK_EQ(gpu.get_missing_bricks(0), 1u);
	// A full rebuild starts over.
	gpu.release_level(0);
	CHECK_EQ(gpu.get_missing_bricks(0), 0u);
	CHECK_EQ(gpu.get_slot(0, Vector3i(5, 5, 5)), NextGIGlobalSDFGPU::PAGE_UNALLOCATED);
	CHECK_EQ(gpu.get_used_bricks(), 0u);
}

TEST_CASE("[NextGI][SWRT] SWRT_REDUCED drops the top level and frees its slots") {
	NextGIGlobalSDFGPU gpu(NextGISWRTTrace::pool_bricks_for_levels(NextGIGlobalSDF::BASE_LEVELS));
	NextGIGlobalSDFClipmaps clipmaps;
	float voxels[NextGIGlobalSDF::BRICK_VOXELS] = {};
	for (uint32_t level = 0; level < NextGIGlobalSDF::BASE_LEVELS; level++) {
		for (int brick = 0; brick < 10; brick++) {
			REQUIRE(gpu.upload_brick(level, Vector3i(brick, 0, 0), voxels));
		}
		NextGIGlobalSDFClipmaps::Plan plan;
		clipmaps.plan_level(level, Vector3(), 0, plan);
		clipmaps.commit_level(level, plan, true);
	}
	CHECK_EQ(gpu.get_used_bricks(), 60u);
	uint32_t levels = NextGIGlobalSDF::BASE_LEVELS;
	levels = NextGIGlobalSDFClipmaps::drop_top_level(gpu, clipmaps, levels, NextGISWRTTrace::MIN_LEVELS);
	CHECK_EQ(levels, 5u);
	CHECK_EQ(gpu.get_used_bricks(), 50u);
	CHECK_FALSE(gpu.has_brick(5, Vector3i(0, 0, 0)));
	CHECK(gpu.has_brick(4, Vector3i(0, 0, 0)));
	CHECK_FALSE(clipmaps.is_level_valid(5));
	CHECK(clipmaps.is_level_valid(4));
	levels = NextGIGlobalSDFClipmaps::drop_top_level(gpu, clipmaps, levels, NextGISWRTTrace::MIN_LEVELS);
	levels = NextGIGlobalSDFClipmaps::drop_top_level(gpu, clipmaps, levels, NextGISWRTTrace::MIN_LEVELS);
	CHECK_EQ(levels, NextGISWRTTrace::MIN_LEVELS);
	CHECK_EQ(gpu.get_used_bricks(), 30u);
	// The floor holds (#187 step 2: min 3 levels).
	CHECK_EQ(NextGIGlobalSDFClipmaps::drop_top_level(gpu, clipmaps, levels, NextGISWRTTrace::MIN_LEVELS), NextGISWRTTrace::MIN_LEVELS);
	CHECK_EQ(gpu.get_used_bricks(), 30u);

	// A retried allocation sizes the pool for the levels it keeps.
	CHECK_EQ(NextGISWRTTrace::pool_bricks_for_levels(NextGIGlobalSDF::BASE_LEVELS), NextGIGlobalSDFGPU::DEFAULT_POOL_BRICKS);
	CHECK(NextGISWRTTrace::pool_bricks_for_levels(5) < NextGISWRTTrace::pool_bricks_for_levels(6));
	CHECK(NextGISWRTTrace::pool_bricks_for_levels(3) < NextGISWRTTrace::pool_bricks_for_levels(5));
	CHECK_EQ(NextGISWRTTrace::pool_bricks_for_levels(3), NextGIGlobalSDFGPU::DEFAULT_POOL_BRICKS / NextGIGlobalSDF::BASE_LEVELS * 3);
}

TEST_CASE("[NextGI][SWRT] Ray and froxel grids cover the view") {
	CHECK_EQ(NextGISWRTTrace::ray_grid(Vector2i(1920, 1080)), Vector2i(64, 64));
	CHECK_EQ(NextGISWRTTrace::ray_grid(Vector2i(32, 16)), Vector2i(32, 16));
	CHECK_EQ(NextGISWRTTrace::ray_grid(Vector2i(0, 0)), Vector2i(1, 1));
	const Vector3i froxels = NextGISWRTTrace::froxel_cells(Vector2i(1920, 1080));
	CHECK_EQ(froxels, Vector3i(30, 17, int(NextGISWRTTrace::FROXEL_SLICES)));
	CHECK_EQ(NextGISWRTTrace::froxel_cells(Vector2i(1, 1)), Vector3i(1, 1, int(NextGISWRTTrace::FROXEL_SLICES)));
}
