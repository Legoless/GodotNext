/**************************************************************************/
/*  test_nextgi_hit_materials.cpp                                         */
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

// GodotNext: nextgi_hit_groups (#130). The device-free parts of the material hit groups: the gate, the
// RT-arena attribute stream, the eligibility rules, the hit shader binding table keys, the check's jobs and
// the ShaderCompiler's hit mode.

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgi_hit_materials)

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_types.h"

using namespace RendererRD;

namespace TestNextGIHitMaterials {

// What NextGI::_update_hit_materials() decides each frame: the availability from the consumers and the
// device, then the stream from the availability and the arena.
static bool _stream_for(uint32_t p_consumers, bool p_hwrt, bool p_pipeline, bool p_library, bool p_bindless, bool p_init_failed, bool p_rt_arena) {
	return nextgi_hit_stream_needed(nextgi_hit_availability(p_consumers, p_hwrt, p_pipeline, p_library, p_bindless, p_init_failed), p_rt_arena);
}

TEST_CASE("[NextGIHit] The attribute stream exists only while the hit groups run, in the RT arena") {
	// Hit lighting off (no consumer): no stream, with or without the arena.
	CHECK_FALSE(_stream_for(0, true, true, true, true, false, true));
	CHECK_FALSE(_stream_for(0, true, true, true, true, false, false));
	// Every consumer, on a device that runs the hit groups, with the arena.
	for (uint32_t consumer : { NEXTGI_HIT_CONSUMER_HIT_LIGHTING, NEXTGI_HIT_CONSUMER_TRANSLUCENCY, NEXTGI_HIT_CONSUMER_FAR_FIELD, NEXTGI_HIT_CONSUMER_CHECK }) {
		CHECK(_stream_for(consumer, true, true, true, true, false, true));
		CHECK_FALSE(_stream_for(consumer, true, true, true, true, false, false));
	}
	// A consumer, but hit groups that cannot run: nothing reads a stream, so none is allocated.
	CHECK_FALSE(_stream_for(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, false, true, true, true, false, true));
	CHECK_FALSE(_stream_for(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, false, false, true, false, true));
	CHECK_FALSE(_stream_for(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, true, false, true, false, true));
	CHECK_FALSE(_stream_for(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, true, true, false, false, true));
	CHECK_FALSE(_stream_for(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, true, true, true, true, true));

	// Surface stream allocation and free in the RT arena, as MeshStorage drives it across consumer off -> on -> off.
	NextGIArenaAllocator allocator(512);
	const NextGIArenaAllocator::Allocation geometry = allocator.allocate(448);
	REQUIRE(geometry.is_valid());
	const uint64_t geometry_bytes = allocator.get_allocated_bytes();
	uint64_t attribute_bytes = 0;
	uint32_t attr_chunk = UINT32_MAX;
	uint32_t attr_offset = 0;
	uint32_t attr_size = 0;
	Vector<uint8_t> attr_cpu;

	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = Vector<Vector3>({ Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(1, 1, 0) });
	arrays[RSE::ARRAY_TEX_UV] = Vector<Vector2>({ Vector2(0, 0), Vector2(4, 0), Vector2(0, 4), Vector2(4, 4) });

	// Consumer off: no stream is allocated.
	if (_stream_for(0, true, true, true, true, false, true)) {
		nextgi_hit_allocate_surface_stream(arrays, 4, allocator, attribute_bytes, attr_chunk, attr_offset, attr_size, attr_cpu);
	}
	CHECK_EQ(attr_chunk, UINT32_MAX);
	CHECK_EQ(attribute_bytes, 0u);
	CHECK_EQ(allocator.get_allocated_bytes(), geometry_bytes);

	// Consumer on: the surface stream allocates in the arena (opening chunk 1 here) and reports its bytes.
	REQUIRE(_stream_for(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, true, true, true, false, true));
	REQUIRE(nextgi_hit_allocate_surface_stream(arrays, 4, allocator, attribute_bytes, attr_chunk, attr_offset, attr_size, attr_cpu));
	CHECK_NE(attr_chunk, UINT32_MAX);
	CHECK(attribute_bytes >= nextgi_hit_stream_size(4));
	CHECK_EQ(allocator.get_allocated_bytes(), geometry_bytes + attribute_bytes);
	CHECK_EQ(allocator.get_chunk_count(), 2u);
	// Allocating again on an already-allocated surface is a no-op.
	CHECK_FALSE(nextgi_hit_allocate_surface_stream(arrays, 4, allocator, attribute_bytes, attr_chunk, attr_offset, attr_size, attr_cpu));

	// Consumer off again: the stream and its trailing chunk are released, restoring the geometry-only arena.
	nextgi_hit_free_surface_stream(allocator, attribute_bytes, attr_chunk, attr_offset, attr_size, attr_cpu);
	CHECK_EQ(allocator.release_trailing_free_chunks(), 1u);
	CHECK_EQ(attr_chunk, UINT32_MAX);
	CHECK_EQ(attribute_bytes, 0u);
	CHECK(attr_cpu.is_empty());
	CHECK_EQ(allocator.get_allocated_bytes(), geometry_bytes);
}

TEST_CASE("[NextGIHit] Availability names the first missing requirement") {
	CHECK_EQ(nextgi_hit_availability(0, true, true, true, true), NEXTGI_HIT_NO_CONSUMER);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, false, true, true, true), NEXTGI_HIT_NOT_HWRT);
	// Metal reports no RT pipelines until #172: the hits fall back.
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, false, false, true), NEXTGI_HIT_NO_RAYTRACING_PIPELINE);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, true, false, true), NEXTGI_HIT_NO_PIPELINE_LIBRARY);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_HIT_LIGHTING, true, true, true, false), NEXTGI_HIT_NO_BINDLESS);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_CHECK, true, true, true, true), NEXTGI_HIT_AVAILABLE);
	// A failed setup is reported, not AVAILABLE, until the consumers go away; a missing requirement comes first.
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_CHECK, true, true, true, true, true), NEXTGI_HIT_INIT_FAILED);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_CHECK, false, true, true, true, true), NEXTGI_HIT_NOT_HWRT);
	CHECK_EQ(nextgi_hit_availability(0, true, true, true, true, true), NEXTGI_HIT_NO_CONSUMER);
	CHECK_EQ(String(nextgi_hit_availability_name(NEXTGI_HIT_INIT_FAILED)), "INIT_FAILED");
	CHECK_EQ(String(nextgi_hit_availability_name(NEXTGI_HIT_NO_RAYTRACING_PIPELINE)), "NO_RAYTRACING_PIPELINE");
	for (int i = 0; i < NEXTGI_HIT_AVAILABILITY_MAX; i++) {
		CHECK_NE(String(nextgi_hit_availability_name(NextGIHitAvailability(i))), "UNKNOWN");
	}
}

static NextGIHitStreamHeader _header(const Vector2 &p_uv_min, const Vector2 &p_uv_extent, const Vector2 &p_uv2_min, const Vector2 &p_uv2_extent) {
	NextGIHitStreamHeader header;
	header.uv_min[0] = p_uv_min.x;
	header.uv_min[1] = p_uv_min.y;
	header.uv_extent[0] = p_uv_extent.x;
	header.uv_extent[1] = p_uv_extent.y;
	header.uv2_min[0] = p_uv2_min.x;
	header.uv2_min[1] = p_uv2_min.y;
	header.uv2_extent[0] = p_uv2_extent.x;
	header.uv2_extent[1] = p_uv2_extent.y;
	return header;
}

TEST_CASE("[NextGIHit] A stream vertex decodes to its normal, tangent, binormal sign, UVs and color") {
	// Tiled UVs over 16 units: unorm16 over the range keeps 16 / 65535 of a unit, where a half float near 16
	// keeps only 1/64.
	const NextGIHitStreamHeader header = _header(Vector2(0, 0), Vector2(16, 16), Vector2(-2, -2), Vector2(4, 6));
	const Vector3 normals[] = { Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(1, 2, 3).normalized(), Vector3(-0.3f, 0.9f, -0.2f).normalized(), Vector3(0, -1, 0) };
	const Vector3 tangents[] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(3, -1, 0.5f).normalized(), Vector3(0.1f, 0.2f, -0.97f).normalized(), Vector3(-1, 0, 0) };
	for (int i = 0; i < 5; i++) {
		for (float sign : { 1.0f, -1.0f }) {
			NextGIHitAttributes v;
			nextgi_hit_pack_vertex(header, normals[i], tangents[i], sign, Vector2(15.3f, 7.71f), Vector2(-1.5f, 3.0f), Color(0.2f, 0.4f, 0.6f, 1.0f), v);
			Vector3 n;
			Vector3 t;
			float s = 0.0f;
			nextgi_hit_unpack_tbn(v, n, t, s);
			CHECK(n.distance_to(normals[i]) < 1e-3f);
			CHECK(t.distance_to(tangents[i]) < 1e-3f);
			CHECK_EQ(s, sign);
			const Vector2 uv = nextgi_hit_unpack_uv0(header, v);
			const Vector2 uv2 = nextgi_hit_unpack_uv1(header, v);
			CHECK(Math::abs(uv.x - 15.3f) <= 16.0f / 65535.0f);
			CHECK(Math::abs(uv.y - 7.71f) <= 16.0f / 65535.0f);
			CHECK(Math::abs(uv2.x + 1.5f) <= 4.0f / 65535.0f);
			CHECK(Math::abs(uv2.y - 3.0f) <= 6.0f / 65535.0f);
			CHECK_EQ(v.color[0], 51);
			CHECK_EQ(v.color[1], 102);
			CHECK_EQ(v.color[2], 153);
			CHECK_EQ(v.color[3], 255);
		}
	}
}

// Godot's uncompressed normal block (RenderingServer::_surface_set_data()): unorm16 octahedral normal, then
// the tangent with the binormal sign folded into y.
static void _godot_normal_block(const Vector3 &p_normal, const Vector3 &p_tangent, float p_sign, uint16_t r_words[4]) {
	const Vector2 n = p_normal.octahedron_encode();
	const Vector2 t = p_tangent.octahedron_tangent_encode(p_sign);
	r_words[0] = uint16_t(CLAMP(n.x * 65535, 0, 65535));
	r_words[1] = uint16_t(CLAMP(n.y * 65535, 0, 65535));
	r_words[2] = uint16_t(CLAMP(t.x * 65535, 0, 65535));
	r_words[3] = uint16_t(CLAMP(t.y * 65535, 0, 65535));
}

TEST_CASE("[NextGIHit] The uncompressed vertex buffer's normal block maps one to one onto the stream") {
	const Vector3 normal = Vector3(0.3f, -0.5f, 0.8f).normalized();
	const Vector3 tangent = Vector3(0.9f, 0.4f, -0.1f).normalized();
	for (float sign : { 1.0f, -1.0f }) {
		uint16_t words[4];
		_godot_normal_block(normal, tangent, sign, words);
		NextGIHitAttributes packed;
		nextgi_hit_pack_vertex(NextGIHitStreamHeader(), normal, tangent, sign, Vector2(), Vector2(), Color(1, 1, 1, 1), packed);
		// Converting the buffer's words gives the stream's components within one step.
		CHECK(Math::abs(int(nextgi_hit_unorm16_to_snorm16(words[0])) - int(packed.normal[0])) <= 1);
		CHECK(Math::abs(int(nextgi_hit_unorm16_to_snorm16(words[1])) - int(packed.normal[1])) <= 1);
		CHECK(Math::abs(int(nextgi_hit_unorm16_to_snorm16(words[2])) - int(packed.tangent[0])) <= 1);
		CHECK(Math::abs(int(nextgi_hit_unorm16_to_snorm16(words[3])) - int(packed.tangent[1])) <= 1);
		CHECK_EQ(packed.tangent[1] < 0, sign < 0.0f);
	}
	// The snorm16 ends.
	CHECK_EQ(nextgi_hit_unorm16_to_snorm16(0), -32767);
	CHECK_EQ(nextgi_hit_unorm16_to_snorm16(65535), 32767);
	CHECK_EQ(nextgi_hit_snorm16_to_float(-32768), -1.0f);
}

TEST_CASE("[NextGIHit] Packing arrays fills the defaults and refuses mismatched sizes") {
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	Vector<Vector3> positions = { Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0) };
	arrays[RSE::ARRAY_VERTEX] = positions;
	Vector<uint8_t> stream;
	REQUIRE(nextgi_hit_pack_arrays(arrays, 3, stream));
	REQUIRE_EQ(stream.size(), NEXTGI_HIT_ATTRIBUTE_HEADER + 3 * NEXTGI_HIT_ATTRIBUTE_STRIDE);
	CHECK_EQ(nextgi_hit_stream_size(3), uint32_t(stream.size()));
	const NextGIHitAttributes *v = reinterpret_cast<const NextGIHitAttributes *>(stream.ptr() + NEXTGI_HIT_ATTRIBUTE_HEADER);
	Vector3 n;
	Vector3 t;
	float s = 0.0f;
	nextgi_hit_unpack_tbn(v[2], n, t, s);
	CHECK(n.distance_to(Vector3(0, 0, 1)) < 1e-3f);
	CHECK(t.distance_to(Vector3(1, 0, 0)) < 1e-3f);
	CHECK_EQ(s, 1.0f);
	CHECK_EQ(v[2].color[0], 255);
	CHECK_EQ(v[2].color[3], 255);
	CHECK_EQ(v[2].uv[0], 0);

	// The header holds each UV set's range, and every vertex decodes back into it.
	Vector<Vector2> tiled = { Vector2(-1, 2), Vector2(7, 2.5f), Vector2(3, 10) };
	arrays[RSE::ARRAY_TEX_UV] = tiled;
	REQUIRE(nextgi_hit_pack_arrays(arrays, 3, stream));
	const NextGIHitStreamHeader *header = reinterpret_cast<const NextGIHitStreamHeader *>(stream.ptr());
	CHECK_EQ(header->uv_min[0], -1.0f);
	CHECK_EQ(header->uv_min[1], 2.0f);
	CHECK_EQ(header->uv_extent[0], 8.0f);
	CHECK_EQ(header->uv_extent[1], 8.0f);
	CHECK_EQ(header->uv2_extent[0], 0.0f);
	const NextGIHitAttributes *tiled_v = reinterpret_cast<const NextGIHitAttributes *>(stream.ptr() + NEXTGI_HIT_ATTRIBUTE_HEADER);
	for (int i = 0; i < 3; i++) {
		CHECK(nextgi_hit_unpack_uv0(*header, tiled_v[i]).distance_to(tiled[i]) <= 8.0f / 65535.0f);
	}

	Vector<Vector2> uvs = { Vector2(0, 0), Vector2(1, 0) }; // One short.
	arrays[RSE::ARRAY_TEX_UV] = uvs;
	CHECK_FALSE(nextgi_hit_pack_arrays(arrays, 3, stream));
	CHECK(stream.is_empty());
	CHECK_FALSE(nextgi_hit_pack_arrays(Array(), 3, stream));
}

TEST_CASE("[NextGIHit] The source layout follows mesh_surface_make_offsets_from_format and skips compressed buffers") {
	const uint64_t format = RSE::ARRAY_FORMAT_VERTEX | RSE::ARRAY_FORMAT_NORMAL | RSE::ARRAY_FORMAT_TANGENT | RSE::ARRAY_FORMAT_COLOR | RSE::ARRAY_FORMAT_TEX_UV | RSE::ARRAY_FORMAT_TEX_UV2;
	const NextGIHitSourceLayout layout = nextgi_hit_source_layout(format, 10);
	CHECK(layout.valid);
	CHECK_EQ(layout.normal_block_offset, 120u);
	CHECK_EQ(layout.normal_stride, 8u);
	CHECK_EQ(layout.normal_offset, 0);
	CHECK_EQ(layout.tangent_offset, 4);
	CHECK_EQ(layout.attribute_stride, 20u);
	CHECK_EQ(layout.color_offset, 0);
	CHECK_EQ(layout.uv_offset, 4);
	CHECK_EQ(layout.uv2_offset, 12);

	const NextGIHitSourceLayout normals_only = nextgi_hit_source_layout(RSE::ARRAY_FORMAT_VERTEX | RSE::ARRAY_FORMAT_NORMAL | RSE::ARRAY_FORMAT_TEX_UV, 4);
	CHECK_EQ(normals_only.normal_stride, 4u);
	CHECK_EQ(normals_only.tangent_offset, -1);
	CHECK_EQ(normals_only.color_offset, -1);
	CHECK_EQ(normals_only.uv_offset, 0);
	CHECK_EQ(normals_only.attribute_stride, 8u);

	// A custom array widens the attribute element after the UVs.
	const uint64_t custom = RSE::ARRAY_FORMAT_VERTEX | RSE::ARRAY_FORMAT_TEX_UV | RSE::ARRAY_FORMAT_CUSTOM0 | (uint64_t(RSE::ARRAY_CUSTOM_RGBA_FLOAT) << RSE::ARRAY_FORMAT_CUSTOM0_SHIFT);
	CHECK_EQ(nextgi_hit_source_layout(custom, 4).attribute_stride, 24u);

	CHECK_FALSE(nextgi_hit_source_layout(format | RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES, 10).valid);
	CHECK_FALSE(nextgi_hit_source_layout(format | RSE::ARRAY_FLAG_USE_2D_VERTICES, 10).valid);
}

TEST_CASE("[NextGIHit] Region updates patch only the components they cover") {
	const uint32_t vertex_count = 4;
	const uint64_t format = RSE::ARRAY_FORMAT_VERTEX | RSE::ARRAY_FORMAT_NORMAL | RSE::ARRAY_FORMAT_TANGENT | RSE::ARRAY_FORMAT_COLOR | RSE::ARRAY_FORMAT_TEX_UV | RSE::ARRAY_FORMAT_TEX_UV2;
	const NextGIHitSourceLayout layout = nextgi_hit_source_layout(format, vertex_count);
	REQUIRE(layout.valid);

	// Start from four identical vertices but for UVs spanning [0, 1].
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = Vector<Vector3>({ Vector3(), Vector3(), Vector3(), Vector3() });
	arrays[RSE::ARRAY_NORMAL] = Vector<Vector3>({ Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1) });
	arrays[RSE::ARRAY_TANGENT] = Vector<float>({ 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1 });
	arrays[RSE::ARRAY_COLOR] = Vector<Color>({ Color(0, 0, 0, 1), Color(0, 0, 0, 1), Color(0, 0, 0, 1), Color(0, 0, 0, 1) });
	arrays[RSE::ARRAY_TEX_UV] = Vector<Vector2>({ Vector2(0, 0), Vector2(1, 0), Vector2(0, 1), Vector2(1, 1) });
	arrays[RSE::ARRAY_TEX_UV2] = Vector<Vector2>({ Vector2(0, 0), Vector2(1, 0), Vector2(0, 1), Vector2(1, 1) });
	Vector<uint8_t> stream;
	REQUIRE(nextgi_hit_pack_arrays(arrays, vertex_count, stream));
	const Vector<uint8_t> original = stream;
	// Read through the stream's current buffer: the copy above makes the next write copy it.
	auto header = [&]() -> const NextGIHitStreamHeader & {
		return *reinterpret_cast<const NextGIHitStreamHeader *>(stream.ptr());
	};
	auto vertex = [&](uint32_t p_index) -> const NextGIHitAttributes & {
		return reinterpret_cast<const NextGIHitAttributes *>(stream.ptr() + NEXTGI_HIT_ATTRIBUTE_HEADER)[p_index];
	};
	const float quantum = 1.0f / 65535.0f;

	// A positions-only update changes nothing.
	uint32_t begin = 0;
	uint32_t end = 0;
	Vector<uint8_t> positions;
	positions.resize(vertex_count * 12);
	CHECK_FALSE(nextgi_hit_patch_stream(layout, vertex_count, false, 0, positions.ptr(), positions.size(), stream.ptrw(), begin, end));
	CHECK(stream == original);

	// Vertices 1 and 2 get a new normal and tangent (the whole 8-byte elements).
	const Vector3 normal = Vector3(1, 1, 0).normalized();
	const Vector3 tangent = Vector3(0, 0, -1);
	uint16_t words[8];
	_godot_normal_block(normal, tangent, -1.0f, words);
	_godot_normal_block(normal, tangent, -1.0f, words + 4);
	const uint32_t offset = layout.normal_block_offset + 1 * layout.normal_stride;
	REQUIRE(nextgi_hit_patch_stream(layout, vertex_count, false, offset, reinterpret_cast<const uint8_t *>(words), sizeof(words), stream.ptrw(), begin, end));
	CHECK_EQ(begin, uint32_t(NEXTGI_HIT_ATTRIBUTE_HEADER + 1 * NEXTGI_HIT_ATTRIBUTE_STRIDE));
	CHECK_EQ(end, uint32_t(NEXTGI_HIT_ATTRIBUTE_HEADER + 3 * NEXTGI_HIT_ATTRIBUTE_STRIDE));
	for (uint32_t i = 0; i < vertex_count; i++) {
		Vector3 n;
		Vector3 t;
		float s = 0.0f;
		nextgi_hit_unpack_tbn(vertex(i), n, t, s);
		const bool patched = i == 1 || i == 2;
		CHECK(n.distance_to(patched ? normal : Vector3(0, 0, 1)) < 1e-3f);
		CHECK(t.distance_to(patched ? tangent : Vector3(1, 0, 0)) < 1e-3f);
		CHECK_EQ(s, patched ? -1.0f : 1.0f);
		// The update leaves the attribute components alone.
		CHECK_EQ(vertex(i).color[0], 0);
	}

	// An attribute update inside the UV range that covers vertex 3's UV but only half of vertex 2's: vertex 2
	// keeps its uv.x.
	const float uv_values[3] = { 0.5f, 0.25f, 0.125f }; // Vertex 2's uv.y, then vertex 3's uv.
	const uint32_t attribute_offset = 2 * layout.attribute_stride + layout.uv_offset + 4;
	uint8_t attribute_bytes[20 + 4] = {};
	memcpy(attribute_bytes, &uv_values[0], 4);
	// Vertex 2's uv2 and vertex 3's color stay zero in the update; vertex 3's uv starts 16 bytes in.
	memcpy(attribute_bytes + 16, &uv_values[1], 4);
	memcpy(attribute_bytes + 20, &uv_values[2], 4);
	REQUIRE(nextgi_hit_patch_stream(layout, vertex_count, true, attribute_offset, attribute_bytes, sizeof(attribute_bytes), stream.ptrw(), begin, end));
	CHECK_EQ(begin, uint32_t(NEXTGI_HIT_ATTRIBUTE_HEADER + 2 * NEXTGI_HIT_ATTRIBUTE_STRIDE));
	CHECK_EQ(end, uint32_t(NEXTGI_HIT_ATTRIBUTE_HEADER + 4 * NEXTGI_HIT_ATTRIBUTE_STRIDE));
	CHECK(nextgi_hit_unpack_uv0(header(), vertex(2)).distance_to(Vector2(0.0f, 0.5f)) <= quantum);
	CHECK(nextgi_hit_unpack_uv0(header(), vertex(3)).distance_to(Vector2(0.25f, 0.125f)) <= quantum);
	CHECK(nextgi_hit_unpack_uv1(header(), vertex(2)).distance_to(Vector2(0.0f, 0.0f)) <= quantum); // Covered: zero.
	CHECK(nextgi_hit_unpack_uv1(header(), vertex(3)).distance_to(Vector2(1.0f, 1.0f)) <= quantum); // Not covered.
	CHECK_EQ(vertex(3).color[3], 0); // Covered, and zero in the update.
	CHECK_EQ(vertex(1).color[3], 255);

	// A UV past the range widens it: every vertex is requantized and the whole stream uploads.
	const float far_uv[2] = { 4.0f, -3.0f };
	const uint32_t far_offset = 0 * layout.attribute_stride + layout.uv_offset;
	REQUIRE(nextgi_hit_patch_stream(layout, vertex_count, true, far_offset, reinterpret_cast<const uint8_t *>(far_uv), sizeof(far_uv), stream.ptrw(), begin, end));
	CHECK_EQ(begin, 0u);
	CHECK_EQ(end, nextgi_hit_stream_size(vertex_count));
	CHECK_EQ(header().uv_min[0], 0.0f);
	CHECK_EQ(header().uv_extent[0], 4.0f);
	CHECK_EQ(header().uv_min[1], -3.0f);
	CHECK_EQ(header().uv_extent[1], 4.0f);
	CHECK(nextgi_hit_unpack_uv0(header(), vertex(0)).distance_to(Vector2(4.0f, -3.0f)) <= 4.0f * quantum);
	CHECK(nextgi_hit_unpack_uv0(header(), vertex(1)).distance_to(Vector2(1.0f, 0.0f)) <= 5.0f * quantum);
	CHECK(nextgi_hit_unpack_uv0(header(), vertex(3)).distance_to(Vector2(0.25f, 0.125f)) <= 5.0f * quantum);
}

TEST_CASE("[NextGIHit] R11G11B10 keeps positive emission within its precision") {
	const Color values[] = { Color(0, 0, 0), Color(1, 0.5f, 0.25f), Color(12.0f, 3.5f, 100.0f), Color(-1, 2, 0) };
	for (const Color &c : values) {
		const Color decoded = nextgi_hit_unpack_r11g11b10(nextgi_hit_pack_r11g11b10(c));
		CHECK(Math::abs(decoded.r - MAX(c.r, 0.0f)) <= MAX(c.r, 0.0f) / 32.0f + 1e-6f);
		CHECK(Math::abs(decoded.g - MAX(c.g, 0.0f)) <= MAX(c.g, 0.0f) / 32.0f + 1e-6f);
		CHECK(Math::abs(decoded.b - MAX(c.b, 0.0f)) <= MAX(c.b, 0.0f) / 16.0f + 1e-6f);
	}
	CHECK_EQ(nextgi_hit_pack_r11g11b10(Color(0, 0, 0)), 0u);
}

static NextGIHitShaderSource _source(const String &p_fragment) {
	NextGIHitShaderSource source;
	source.fragment_code = p_fragment;
	return source;
}

TEST_CASE("[NextGIHit] Ineligible materials are classified, and only by whole identifiers") {
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = vec3(1.0);")), NEXTGI_HIT_ELIGIBLE);
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = vec3(screen_uv, 0.0);")), NEXTGI_HIT_INELIGIBLE_SCREEN_READ);
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = gl_FragCoord.xyz;")), NEXTGI_HIT_INELIGIBLE_SCREEN_READ);
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = vec3(dFdx(uv_interp.x));")), NEXTGI_HIT_INELIGIBLE_DERIVATIVE);
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = vec3(fwidth(uv_interp.x));")), NEXTGI_HIT_INELIGIBLE_DERIVATIVE);
	CHECK_EQ(nextgi_hit_classify(_source("if (uv_interp.x > 0.5) { discard; }")), NEXTGI_HIT_INELIGIBLE_DISCARD);
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = vec3(gl_FrontFacing ? 1.0 : 0.0);")), NEXTGI_HIT_INELIGIBLE_BUILTIN);
	CHECK_EQ(nextgi_hit_classify(_source("albedo_highp = vec3(float(INSTANCE_INDEX));")), NEXTGI_HIT_INELIGIBLE_BUILTIN);
	// Identifiers that only contain a token are fine.
	CHECK_EQ(nextgi_hit_classify(_source("float m_my_screen_uv_scale = 1.0; float m_dFdx_like = 2.0;")), NEXTGI_HIT_ELIGIBLE);

	NextGIHitShaderSource screen = _source("");
	screen.screen_read = true;
	CHECK_EQ(nextgi_hit_classify(screen), NEXTGI_HIT_INELIGIBLE_SCREEN_READ);
	NextGIHitShaderSource depth = _source("");
	depth.writes_depth = true;
	CHECK_EQ(nextgi_hit_classify(depth), NEXTGI_HIT_INELIGIBLE_DEPTH_WRITE);
	NextGIHitShaderSource position = _source("");
	position.writes_position = true;
	CHECK_EQ(nextgi_hit_classify(position), NEXTGI_HIT_INELIGIBLE_POSITION_WRITE);
	NextGIHitShaderSource instance = _source("");
	instance.instance_uniforms = true;
	CHECK_EQ(nextgi_hit_classify(instance), NEXTGI_HIT_INELIGIBLE_INSTANCE_UNIFORM);

	NextGIHitShaderSource textures = _source("");
	ShaderCompiler::GeneratedCode::Texture texture;
	texture.name = "albedo_tex";
	texture.type = ShaderLanguage::TYPE_SAMPLER2D;
	textures.textures.push_back(texture);
	CHECK_EQ(nextgi_hit_classify(textures), NEXTGI_HIT_ELIGIBLE);
	textures.textures.write[0].type = ShaderLanguage::TYPE_SAMPLERCUBE;
	CHECK_EQ(nextgi_hit_classify(textures), NEXTGI_HIT_INELIGIBLE_TEXTURE_TYPE);
	textures.textures.write[0].type = ShaderLanguage::TYPE_SAMPLER2D;
	textures.textures.write[0].array_size = 4;
	CHECK_EQ(nextgi_hit_classify(textures), NEXTGI_HIT_INELIGIBLE_TEXTURE_TYPE);
	textures.textures.write[0].array_size = 0;
	textures.textures.write[0].global = true;
	CHECK_EQ(nextgi_hit_classify(textures), NEXTGI_HIT_INELIGIBLE_TEXTURE_TYPE);

	for (int i = 0; i < NEXTGI_HIT_INELIGIBLE_MAX; i++) {
		CHECK_NE(String(nextgi_hit_ineligible_name(NextGIHitIneligible(i))), "UNKNOWN");
	}
}

TEST_CASE("[NextGIHit] MultiMesh elements fall back only for the per-element inputs their MultiMesh stores") {
	CHECK_EQ(nextgi_hit_element_inputs(_source("albedo_highp = vec3(1.0);")), 0u);
	CHECK_EQ(nextgi_hit_element_inputs(_source("albedo_highp = color_interp.rgb;")), uint32_t(NEXTGI_HIT_READS_COLOR));
	NextGIHitShaderSource custom = _source("roughness_highp = m_v_custom;");
	custom.vertex_code = "m_v_custom = instance_custom.x;";
	CHECK_EQ(nextgi_hit_element_inputs(custom), uint32_t(NEXTGI_HIT_READS_INSTANCE_CUSTOM));
	// A function both entry points use counts too; an identifier that only contains the name does not.
	NextGIHitShaderSource helper = _source("albedo_highp = m_tint();");
	helper.globals = "vec3 m_tint() { return color_interp.rgb; }";
	CHECK_EQ(nextgi_hit_element_inputs(helper), uint32_t(NEXTGI_HIT_READS_COLOR));
	CHECK_EQ(nextgi_hit_element_inputs(_source("float m_color_interp_scale = 1.0;")), 0u);

	const uint32_t both = NEXTGI_HIT_READS_COLOR | NEXTGI_HIT_READS_INSTANCE_CUSTOM;
	CHECK_FALSE(nextgi_hit_element_needs_fallback(both, false, false)); // The MultiMesh stores neither.
	CHECK(nextgi_hit_element_needs_fallback(NEXTGI_HIT_READS_COLOR, true, false));
	CHECK_FALSE(nextgi_hit_element_needs_fallback(NEXTGI_HIT_READS_COLOR, false, true));
	CHECK(nextgi_hit_element_needs_fallback(NEXTGI_HIT_READS_INSTANCE_CUSTOM, false, true));
	CHECK_FALSE(nextgi_hit_element_needs_fallback(NEXTGI_HIT_READS_INSTANCE_CUSTOM, true, false));
	CHECK_FALSE(nextgi_hit_element_needs_fallback(0, true, true)); // The material reads neither.
}

TEST_CASE("[NextGIHit] Dead groups are relinked away past the threshold, never during a link") {
	CHECK_FALSE(nextgi_hit_should_relink(0, false));
	CHECK_FALSE(nextgi_hit_should_relink(NEXTGI_HIT_RELINK_DEAD_GROUPS - 1, false));
	CHECK(nextgi_hit_should_relink(NEXTGI_HIT_RELINK_DEAD_GROUPS, false));
	CHECK(nextgi_hit_should_relink(NEXTGI_HIT_RELINK_DEAD_GROUPS * 4, false));
	CHECK_FALSE(nextgi_hit_should_relink(NEXTGI_HIT_RELINK_DEAD_GROUPS * 4, true));
}

TEST_CASE("[NextGIHit] A full bindless table is tried again once a released slot is reusable") {
	const uint32_t delay = 3;
	SUBCASE("A slot released just before the failure wakes the table") {
		// The parameter buffer's resize releases its slot and allocates again in one frame.
		NextGIHitTableLatch latch;
		CHECK(latch.may_allocate(10));
		latch.released(10, delay);
		latch.allocated(false, 10, delay);
		CHECK_FALSE(latch.may_allocate(10));
		CHECK_FALSE(latch.may_allocate(10 + delay));
		CHECK(latch.may_allocate(10 + delay + 1));
		latch.allocated(true, 10 + delay + 1, delay);
		CHECK(latch.may_allocate(10 + delay + 1));
	}
	SUBCASE("Without a release the table stays shut") {
		NextGIHitTableLatch latch;
		latch.allocated(false, 10, delay);
		CHECK_FALSE(latch.may_allocate(1000));
		latch.released(1000, delay);
		CHECK_FALSE(latch.may_allocate(1000 + delay));
		CHECK(latch.may_allocate(1000 + delay + 1));
	}
	SUBCASE("A release every frame does not keep the table shut") {
		NextGIHitTableLatch latch;
		latch.allocated(false, 10, delay);
		for (uint64_t frame = 11; frame < 11 + delay; frame++) {
			latch.released(frame, delay);
			CHECK_FALSE(latch.may_allocate(frame));
		}
		latch.released(11 + delay, delay);
		CHECK(latch.may_allocate(11 + delay + 1));
	}
	SUBCASE("A retry that fails again waits for the releases it could not see yet") {
		NextGIHitTableLatch latch;
		latch.allocated(false, 10, delay);
		latch.released(12, delay);
		const uint64_t retry = 12 + delay + 1;
		CHECK(latch.may_allocate(retry));
		latch.allocated(false, retry, delay);
		// The release at frame 12 was reusable by the retry: nothing else is pending.
		CHECK_FALSE(latch.may_allocate(retry + 100));
		latch.released(retry, delay);
		latch.allocated(false, retry, delay);
		CHECK(latch.may_allocate(retry + delay + 1));
	}
}

TEST_CASE("[NextGIHit] The arena drops the trailing chunks the stream left wholly free") {
	NextGIArenaAllocator allocator(1024);
	const NextGIArenaAllocator::Allocation geometry = allocator.allocate(600);
	const NextGIArenaAllocator::Allocation stream_a = allocator.allocate(600); // Opens chunk 1.
	const NextGIArenaAllocator::Allocation stream_b = allocator.allocate(300); // Fits chunk 0's rest.
	REQUIRE(geometry.is_valid());
	REQUIRE(stream_a.is_valid());
	REQUIRE(stream_b.is_valid());
	CHECK_EQ(stream_a.chunk, 1u);
	CHECK_EQ(stream_b.chunk, 0u);
	CHECK_EQ(allocator.get_chunk_count(), 2u);
	// Chunk 1 still holds a stream: nothing is dropped.
	CHECK_EQ(allocator.release_trailing_free_chunks(), 2u);
	allocator.free_range(stream_b);
	allocator.free_range(stream_a);
	// Chunk 1 is wholly free and last: dropped. Chunk 0 keeps the geometry.
	CHECK_EQ(allocator.release_trailing_free_chunks(), 1u);
	CHECK_EQ(allocator.get_chunk_count(), 1u);
	CHECK_EQ(allocator.get_allocated_bytes(), uint64_t(geometry.size));
	// The next allocation past chunk 0's rest opens chunk 1 again.
	const NextGIArenaAllocator::Allocation again = allocator.allocate(600);
	CHECK_EQ(again.chunk, 1u);
	CHECK(again.new_chunk);
}

TEST_CASE("[NextGIHit] Each material texture is a define over its bindless slot") {
	NextGIHitShaderSource source;
	ShaderCompiler::GeneratedCode::Texture texture;
	texture.name = "texture_albedo";
	source.textures.push_back(texture);
	texture.name = "texture_normal";
	source.textures.push_back(texture);
	const String defines = nextgi_hit_texture_defines(source);
	CHECK(defines.contains("#define m_texture_albedo hit_textures[nonuniformEXT(hit_texture_slots.data[hit_texture_base + 0u])]\n"));
	CHECK(defines.contains("#define m_texture_normal hit_textures[nonuniformEXT(hit_texture_slots.data[hit_texture_base + 1u])]\n"));
}

TEST_CASE("[NextGIHit] Hit shader binding table ranges are keyed by their group list") {
	const uint32_t a[] = { 1, 2, 3 };
	const uint32_t b[] = { 1, 2, 3 };
	const uint32_t c[] = { 1, 3, 2 };
	const uint32_t d[] = { 1, 2 };
	CHECK_EQ(nextgi_hit_group_list_key(a, 3), nextgi_hit_group_list_key(b, 3));
	CHECK_NE(nextgi_hit_group_list_key(a, 3), nextgi_hit_group_list_key(c, 3));
	CHECK_NE(nextgi_hit_group_list_key(a, 3), nextgi_hit_group_list_key(d, 2));
}

TEST_CASE("[NextGIHit] The check's jobs cover each card's texels in order, within the sample cap") {
	const NextGIHitCheckCard cards[] = {
		{ 4, 10, 8, 4 }, // 32 samples.
		{ 5, 11, 0, 4 }, // Empty: skipped.
		{ 6, 12, 16, 16 }, // 256 samples: past a cap of 100, skipped.
		{ 7, 13, 2, 3 }, // 6 samples.
	};
	LocalVector<uint32_t> jobs;
	const uint32_t samples = nextgi_hit_plan_check_jobs(cards, 4, 100, jobs);
	CHECK_EQ(samples, 38u);
	REQUIRE_EQ(jobs.size(), 8u);
	CHECK_EQ(jobs[0], 4u);
	CHECK_EQ(jobs[1], 10u);
	CHECK_EQ(jobs[2], 0u);
	CHECK_EQ(jobs[3], 8u | (4u << 16));
	CHECK_EQ(jobs[4], 7u);
	CHECK_EQ(jobs[5], 13u);
	CHECK_EQ(jobs[6], 32u);
	CHECK_EQ(jobs[7], 2u | (3u << 16));

	CHECK_EQ(nextgi_hit_plan_check_jobs(cards, 4, 1000, jobs), 294u);
	CHECK_EQ(jobs.size(), 12u);
	CHECK_EQ(nextgi_hit_check_counter_name(NEXTGI_HIT_CHECK_ALL), String("all"));
	for (int i = 0; i < NEXTGI_HIT_CHECK_COUNTER_MAX; i++) {
		CHECK_NE(String(nextgi_hit_check_counter_name(NextGIHitCheckCounter(i))), "unknown");
	}
	CHECK_EQ(nextgi_hit_check_counter_name(NEXTGI_HIT_CHECK_PROBE_GPU_FALLBACK), String("probe_gpu_fallback"));
}

static void _init_spatial_compiler(ShaderCompiler &r_compiler) {
	ShaderCompiler::DefaultIdentifierActions actions;
	actions.renames["VERTEX"] = "vertex";
	actions.renames["NORMAL"] = "normal_highp";
	actions.renames["UV"] = "uv_interp";
	actions.renames["COLOR"] = "color_interp";
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ROUGHNESS"] = "roughness_highp";
	actions.renames["TIME"] = "global_time";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["FRAGCOORD"] = "#define FRAGCOORD_USED\n";
	actions.base_texture_binding_index = 1;
	actions.texture_layout_set = 2;
	actions.base_uniform_string = "material.";
	actions.base_varying_index = 15;
	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	r_compiler.initialize(actions);
}

static Error _compile_spatial(const String &p_code, bool p_hit, ShaderCompiler::GeneratedCode &r_gen) {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	ShaderCompiler compiler;
	_init_spatial_compiler(compiler);
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	actions.uniforms = &uniforms;
	actions.nextgi_hit = p_hit;
	return compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, "test_hit.gdshader", r_gen);
}

TEST_CASE("[NextGIHit][ShaderCompiler] The hit mode makes one module with cone-LOD texture reads and leaves the raster output alone") {
	const String code = R"(
shader_type spatial;
uniform sampler2D albedo_tex;
uniform vec4 tint;
varying vec2 v_detail;

float shared_scale(float x) {
	return x * 2.0;
}

void vertex() {
	v_detail = UV * shared_scale(0.5);
}

void fragment() {
	vec4 a = texture(albedo_tex, UV);
	vec4 b = texture(albedo_tex, v_detail, 1.5);
	vec4 c = textureLod(albedo_tex, UV, 2.0);
	ALBEDO = (a.rgb + b.rgb + c.rgb) * tint.rgb * shared_scale(1.0);
}
)";

	ShaderCompiler::GeneratedCode hit;
	REQUIRE_EQ(_compile_spatial(code, true, hit), OK);
	const String &globals = hit.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	// No texture declaration: the hit template defines the texture over the bindless table.
	CHECK_FALSE(globals.contains("uniform sampler2D"));
	CHECK_FALSE(globals.contains("uniform texture2D"));
	CHECK_EQ(hit.texture_uniforms.size(), 1);
	// The varying is a plain global, declared once.
	CHECK_MESSAGE(globals.contains("highp vec2 m_v_detail;"), globals);
	CHECK_FALSE(globals.contains("out highp vec2 m_v_detail"));
	CHECK_FALSE(globals.contains("in highp vec2 m_v_detail"));
	// A function both entry points use is in the fragment globals once, and nowhere else.
	CHECK_EQ(globals.count("float m_shared_scale("), 1);
	CHECK_FALSE(hit.stage_globals[ShaderCompiler::STAGE_VERTEX].contains("m_shared_scale"));
	// texture() samples at the cone's LOD; a bias adds to it; an explicit LOD stays.
	const String &fragment = hit.code["fragment"];
	CHECK_FALSE(fragment.contains("texture(sampler2D("));
	// The LOD function takes the texture: a sampler constructor may appear only where it is used.
	CHECK_MESSAGE(fragment.contains("textureLod(sampler2D(m_albedo_tex, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv_interp, nextgi_hit_texture_lod(m_albedo_tex))"), fragment);
	CHECK_MESSAGE(fragment.contains("nextgi_hit_texture_lod(m_albedo_tex) + ("), fragment);
	CHECK_FALSE(fragment.contains("nextgi_hit_texture_lod(sampler2D("));
	CHECK(fragment.contains("textureLod(sampler2D(m_albedo_tex, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv_interp, 2.0)"));
	CHECK(hit.uniforms.contains("m_tint"));

	ShaderCompiler::GeneratedCode raster;
	REQUIRE_EQ(_compile_spatial(code, false, raster), OK);
	const String &raster_fragment_globals = raster.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	const String &raster_vertex_globals = raster.stage_globals[ShaderCompiler::STAGE_VERTEX];
	CHECK_MESSAGE(raster_fragment_globals.contains("layout(set = 2, binding = 1) uniform"), raster_fragment_globals);
	CHECK_MESSAGE(raster_fragment_globals.contains("texture2D m_albedo_tex;"), raster_fragment_globals);
	CHECK_MESSAGE(raster_fragment_globals.contains("in highp vec2 m_v_detail;"), raster_fragment_globals);
	CHECK_MESSAGE(raster_vertex_globals.contains("out highp vec2 m_v_detail;"), raster_vertex_globals);
	CHECK(raster.stage_globals[ShaderCompiler::STAGE_VERTEX].contains("m_shared_scale"));
	CHECK(raster.code["fragment"].contains("texture(sampler2D(m_albedo_tex, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv_interp)"));
	CHECK_FALSE(raster.code["fragment"].contains("nextgi_hit_texture_lod"));
	CHECK_EQ(raster.uniforms, hit.uniforms);
	CHECK_EQ(raster.uniform_total_size, hit.uniform_total_size);
}

TEST_CASE("[NextGIHit][ShaderCompiler] The hit mode rewrites vertex() and textureProj() reads and leaves light()-only functions out") {
	const String code = R"(
shader_type spatial;
uniform sampler2D tint_map;
uniform sampler2D proj_map;

float light_only_helper(float x) {
	return dFdx(x);
}

void vertex() {
	COLOR = texture(tint_map, UV);
}

void fragment() {
	vec4 a = textureProj(proj_map, vec3(UV, 1.0));
	vec4 b = textureProj(proj_map, vec4(UV, 0.0, 2.0), 0.5);
	ALBEDO = COLOR.rgb * (a.rgb + b.rgb);
}

void light() {
	DIFFUSE_LIGHT += vec3(light_only_helper(FRAGCOORD.x));
}
)";

	ShaderCompiler::GeneratedCode hit;
	REQUIRE_EQ(_compile_spatial(code, true, hit), OK);
	// vertex()'s texture() reads at the template's LOD too (level 0 while vertex() runs at a hit).
	const String &vertex = hit.code["vertex"];
	CHECK_MESSAGE(vertex.contains("textureLod(sampler2D(m_tint_map, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv_interp, nextgi_hit_texture_lod(m_tint_map))"), vertex);
	// textureProj() becomes textureProjLod() at the cone's LOD, and its bias adds to it.
	const String &fragment = hit.code["fragment"];
	CHECK_FALSE(fragment.contains("textureProj(sampler2D("));
	CHECK_MESSAGE(fragment.contains("textureProjLod(sampler2D(m_proj_map, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), vec3(uv_interp, 1.0), nextgi_hit_texture_lod(m_proj_map))"), fragment);
	CHECK_MESSAGE(fragment.contains("nextgi_hit_texture_lod(m_proj_map) + (0.5)"), fragment);
	// light() is noted, and neither its body nor the derivative in the function only it uses pollutes the hit module.
	CHECK(hit.code.has("light"));
	CHECK(hit.code["light"].is_empty());
	CHECK_FALSE(hit.defines.has("#define FRAGCOORD_USED\n"));
	const String &globals = hit.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	CHECK_FALSE(globals.contains("m_light_only_helper"));
	NextGIHitShaderSource source;
	source.globals = globals;
	source.vertex_code = vertex;
	source.fragment_code = fragment;
	CHECK_EQ(nextgi_hit_classify(source), NEXTGI_HIT_ELIGIBLE);
	CHECK_EQ(nextgi_hit_element_inputs(source), uint32_t(NEXTGI_HIT_READS_COLOR));

	// The raster output keeps textureProj() and light() with its helper.
	ShaderCompiler::GeneratedCode raster;
	REQUIRE_EQ(_compile_spatial(code, false, raster), OK);
	CHECK(raster.code["fragment"].contains("textureProj(sampler2D(m_proj_map, SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), vec3(uv_interp, 1.0))"));
	CHECK_FALSE(raster.code["light"].is_empty());
	CHECK(raster.stage_globals[ShaderCompiler::STAGE_FRAGMENT].contains("m_light_only_helper"));
}

} // namespace TestNextGIHitMaterials
