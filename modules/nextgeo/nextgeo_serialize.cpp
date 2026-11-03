/**************************************************************************/
/*  nextgeo_serialize.cpp                                                 */
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

#include "nextgeo_serialize.h"

#include "core/crypto/crypto_core.h"
#include "core/error/error_macros.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/mesh.h"

#include <cstring>

namespace {

struct ByteWriter {
	Vector<uint8_t> bytes;
	uint32_t cursor = 0;
	Error error = OK;

	void reserve(uint64_t p_size) {
		if (p_size > uint64_t(INT32_MAX)) {
			error = ERR_OUT_OF_MEMORY;
			return;
		}
		bytes.resize(int(p_size));
	}

	void u8(uint8_t p_value) {
		if (error != OK) {
			return;
		}
		if (cursor >= uint32_t(bytes.size())) {
			error = ERR_BUG;
			return;
		}
		bytes.write[cursor++] = p_value;
	}

	void u16(uint16_t p_value) {
		u8(uint8_t(p_value));
		u8(uint8_t(p_value >> 8));
	}

	void u32(uint32_t p_value) {
		u8(uint8_t(p_value));
		u8(uint8_t(p_value >> 8));
		u8(uint8_t(p_value >> 16));
		u8(uint8_t(p_value >> 24));
	}

	void i32(int32_t p_value) {
		u32(uint32_t(p_value));
	}

	void f32(float p_value) {
		uint32_t bits = 0;
		memcpy(&bits, &p_value, sizeof(bits));
		u32(bits);
	}
};

struct VertexLayout {
	uint32_t stride = 0;
	uint32_t position = 0;
	uint32_t normal = 0;
	uint32_t tangent = 0;
	uint32_t uv0 = 0;
	uint32_t uv2 = 0;
	uint32_t color = 0;
	bool has_normal = false;
	bool has_tangent = false;
	bool has_uv0 = false;
	bool has_uv2 = false;
	bool has_color = false;
};

Error decode_layout(uint32_t p_bits, uint32_t p_stride, VertexLayout &r_layout) {
	const uint32_t known = NEXTGEO_VERTEX_POSITION | NEXTGEO_VERTEX_NORMAL | NEXTGEO_VERTEX_TANGENT | NEXTGEO_VERTEX_UV0 | NEXTGEO_VERTEX_UV2 | NEXTGEO_VERTEX_COLOR;
	if ((p_bits & ~known) != 0 || (p_bits & NEXTGEO_VERTEX_POSITION) == 0) {
		return ERR_INVALID_DATA;
	}
	uint32_t cursor = 0;
	auto take = [&](uint32_t p_bit, uint32_t p_width, bool &r_present, uint32_t &r_offset) {
		r_present = (p_bits & p_bit) != 0;
		if (r_present) {
			r_offset = cursor;
			cursor += p_width;
		}
	};
	bool has_position = false;
	take(NEXTGEO_VERTEX_POSITION, 3, has_position, r_layout.position);
	if (!has_position) {
		return ERR_INVALID_DATA;
	}
	take(NEXTGEO_VERTEX_NORMAL, 3, r_layout.has_normal, r_layout.normal);
	take(NEXTGEO_VERTEX_TANGENT, 4, r_layout.has_tangent, r_layout.tangent);
	take(NEXTGEO_VERTEX_UV0, 2, r_layout.has_uv0, r_layout.uv0);
	take(NEXTGEO_VERTEX_UV2, 2, r_layout.has_uv2, r_layout.uv2);
	take(NEXTGEO_VERTEX_COLOR, 4, r_layout.has_color, r_layout.color);
	r_layout.stride = cursor;
	if (cursor != p_stride) {
		return ERR_INVALID_DATA;
	}
	return OK;
}

void write_cluster(ByteWriter &r_writer, const NextGeoCluster &p_cluster) {
	for (uint32_t i = 0; i < 4; i++) {
		r_writer.f32(p_cluster.bounds[i]);
	}
	for (uint32_t i = 0; i < 3; i++) {
		r_writer.u8(uint8_t(p_cluster.cone_axis[i]));
	}
	r_writer.u8(uint8_t(p_cluster.cone_cutoff));
	for (uint32_t i = 0; i < 4; i++) {
		r_writer.f32(p_cluster.group_lod[i]);
	}
	r_writer.f32(p_cluster.group_error);
	for (uint32_t i = 0; i < 4; i++) {
		r_writer.f32(p_cluster.parent_lod[i]);
	}
	r_writer.f32(p_cluster.parent_error);
	r_writer.i32(p_cluster.refined_group);
	r_writer.u32(p_cluster.page_id);
	r_writer.u32(p_cluster.vertex_offset);
	r_writer.u32(p_cluster.triangle_offset);
	r_writer.u8(p_cluster.vertex_count);
	r_writer.u8(p_cluster.triangle_count);
	r_writer.u16(p_cluster.surface_id);
}

Error page_ranges(const NextGeoBuildOutput &p_output, const NextGeoPage &p_page, uint32_t &r_vertices, uint32_t &r_triangles) {
	if (uint64_t(p_page.cluster_offset) + uint64_t(p_page.cluster_count) > uint64_t(p_output.clusters.size())) {
		return ERR_INVALID_DATA;
	}
	uint64_t vertices = 0;
	uint64_t triangles = 0;
	for (uint32_t i = 0; i < p_page.cluster_count; i++) {
		const NextGeoCluster &cluster = p_output.clusters[p_page.cluster_offset + i];
		vertices += cluster.vertex_count;
		triangles += cluster.triangle_count;
		const uint64_t vertex_begin = uint64_t(cluster.vertex_offset) * p_output.vertex_stride;
		const uint64_t vertex_floats = uint64_t(cluster.vertex_count) * p_output.vertex_stride;
		if (p_output.vertex_stride == 0 && cluster.vertex_count != 0) {
			return ERR_INVALID_DATA;
		}
		if (vertex_begin > p_output.vertices.size() || vertex_floats > p_output.vertices.size() - vertex_begin) {
			return ERR_INVALID_DATA;
		}
		const uint64_t triangle_begin = uint64_t(cluster.triangle_offset) * 3;
		const uint64_t triangle_bytes = uint64_t(cluster.triangle_count) * 3;
		if (triangle_begin > p_output.triangles.size() || triangle_bytes > p_output.triangles.size() - triangle_begin) {
			return ERR_INVALID_DATA;
		}
	}
	if (vertices > UINT32_MAX || triangles > UINT32_MAX) {
		return ERR_INVALID_DATA;
	}
	r_vertices = uint32_t(vertices);
	r_triangles = uint32_t(triangles);
	return OK;
}

} // namespace

Error nextgeo_write_pages(const NextGeoBuildOutput &p_output, Vector<Vector<uint8_t>> &r_pages) {
	r_pages.clear();
	VertexLayout layout;
	const Error layout_error = decode_layout(p_output.vertex_layout, p_output.vertex_stride, layout);
	if (layout_error != OK) {
		return layout_error;
	}

	r_pages.resize(p_output.pages.size());
	for (uint32_t page_index = 0; page_index < p_output.pages.size(); page_index++) {
		const NextGeoPage &page = p_output.pages[page_index];
		uint32_t vertex_count = 0;
		uint32_t triangle_count = 0;
		const Error range_error = page_ranges(p_output, page, vertex_count, triangle_count);
		if (range_error != OK) {
			r_pages.clear();
			return range_error;
		}

		const uint64_t payload = 16ull + uint64_t(page.cluster_count) * 80ull + uint64_t(vertex_count) * layout.stride * 4ull + uint64_t(triangle_count) * 3ull;
		ByteWriter writer;
		writer.reserve(payload);
		writer.u32(page.cluster_count);
		writer.u32(vertex_count);
		writer.u32(triangle_count);
		writer.u32(0);

		uint32_t local_vertex = 0;
		uint32_t local_triangle = 0;
		for (uint32_t i = 0; i < page.cluster_count; i++) {
			NextGeoCluster cluster = p_output.clusters[page.cluster_offset + i];
			cluster.vertex_offset = local_vertex;
			cluster.triangle_offset = local_triangle;
			write_cluster(writer, cluster);
			local_vertex += cluster.vertex_count;
			local_triangle += cluster.triangle_count;
		}

		for (uint32_t i = 0; i < page.cluster_count; i++) {
			const NextGeoCluster &cluster = p_output.clusters[page.cluster_offset + i];
			const float *src = p_output.vertices.ptr() + uint64_t(cluster.vertex_offset) * layout.stride;
			const uint32_t float_count = uint32_t(cluster.vertex_count) * layout.stride;
			for (uint32_t f = 0; f < float_count; f++) {
				writer.f32(src[f]);
			}
		}
		for (uint32_t i = 0; i < page.cluster_count; i++) {
			const NextGeoCluster &cluster = p_output.clusters[page.cluster_offset + i];
			const uint8_t *src = p_output.triangles.ptr() + uint64_t(cluster.triangle_offset) * 3;
			const uint32_t byte_count = uint32_t(cluster.triangle_count) * 3;
			for (uint32_t b = 0; b < byte_count; b++) {
				writer.u8(src[b]);
			}
		}

		if (writer.error != OK || writer.cursor != payload) {
			r_pages.clear();
			return writer.error != OK ? writer.error : ERR_BUG;
		}
		r_pages.write[page_index] = writer.bytes;
	}
	return OK;
}

Error nextgeo_fill_ngm_header(const NextGeoBuildOutput &p_output, const Vector<Vector<uint8_t>> &p_pages, NGMHeader &r_header) {
	r_header = NGMHeader();
	if (p_output.page_size == 0 || p_pages.size() != int(p_output.pages.size()) || p_output.root_page_count > uint32_t(p_pages.size())) {
		return ERR_INVALID_PARAMETER;
	}
	r_header.version = NGM_VERSION;
	r_header.flags = 0;
	r_header.page_size = p_output.page_size;
	r_header.page_count = uint32_t(p_pages.size());
	r_header.root_page_count = p_output.root_page_count;
	r_header.cluster_count = p_output.clusters.size();
	r_header.group_count = p_output.groups.size();
	r_header.dag_depth = p_output.dag_depth;
	r_header.vertex_layout = p_output.vertex_layout;

	CryptoCore::SHA256Context sha;
	const Error started = sha.start();
	if (started != OK) {
		return started;
	}
	for (int i = 0; i < p_pages.size(); i++) {
		if (p_pages[i].is_empty()) {
			continue;
		}
		const Error updated = sha.update(p_pages[i].ptr(), p_pages[i].size());
		if (updated != OK) {
			return updated;
		}
	}
	return sha.finish(r_header.source_sha256);
}

Error nextgeo_fill_vgd(const NextGeoBuildInput &p_input, const NextGeoBuildOutput &p_output, const Vector<Vector<uint8_t>> &p_pages, const Ref<VirtualGeometryData> &p_data) {
	ERR_FAIL_COND_V(p_data.is_null(), ERR_INVALID_PARAMETER);
	if (p_pages.size() != int(p_output.pages.size()) || p_output.root_page_count > uint32_t(p_pages.size())) {
		return ERR_INVALID_DATA;
	}
	VertexLayout layout;
	const Error layout_error = decode_layout(p_output.vertex_layout, p_output.vertex_stride, layout);
	if (layout_error != OK) {
		return layout_error;
	}

	const uint64_t dag_bytes = 8ull + uint64_t(p_output.groups.size()) * 60ull + uint64_t(p_output.bvh.size()) * 80ull;
	ByteWriter dag;
	dag.reserve(dag_bytes);
	dag.u32(p_output.groups.size());
	dag.u32(p_output.bvh.size());
	for (uint32_t i = 0; i < p_output.groups.size(); i++) {
		const NextGeoGroup &group = p_output.groups[i];
		dag.u32(group.cluster_offset);
		dag.u32(group.cluster_count);
		dag.u32(group.page_id);
		dag.u32(group.depth);
		for (uint32_t k = 0; k < 4; k++) {
			dag.f32(group.parent_lod[k]);
		}
		dag.f32(group.parent_error);
		for (uint32_t k = 0; k < 3; k++) {
			dag.f32(group.aabb_min[k]);
		}
		for (uint32_t k = 0; k < 3; k++) {
			dag.f32(group.aabb_max[k]);
		}
	}
	for (uint32_t i = 0; i < p_output.bvh.size(); i++) {
		const NextGeoBVHNode &node = p_output.bvh[i];
		for (uint32_t k = 0; k < 4; k++) {
			dag.f32(node.lod_sphere[k]);
		}
		dag.f32(node.max_parent_error);
		for (uint32_t k = 0; k < 3; k++) {
			dag.f32(node.aabb_min[k]);
		}
		for (uint32_t k = 0; k < 3; k++) {
			dag.f32(node.aabb_max[k]);
		}
		dag.u32(node.child_count);
		for (uint32_t k = 0; k < NEXTGEO_BVH_WIDTH; k++) {
			dag.u32(node.children[k]);
		}
	}
	if (dag.error != OK || dag.cursor != dag_bytes) {
		return dag.error != OK ? dag.error : ERR_BUG;
	}

	struct SurfaceBuild {
		PackedVector3Array vertices;
		PackedVector3Array normals;
		PackedFloat32Array tangents;
		PackedVector2Array uv0;
		PackedVector2Array uv2;
		PackedColorArray colors;
		PackedInt32Array indices;
	};
	Vector<SurfaceBuild> built;
	built.resize(p_input.surfaces.size());

	for (uint32_t page_index = 0; page_index < p_output.root_page_count; page_index++) {
		const NextGeoPage &page = p_output.pages[page_index];
		if (uint64_t(page.cluster_offset) + uint64_t(page.cluster_count) > uint64_t(p_output.clusters.size())) {
			return ERR_INVALID_DATA;
		}
		for (uint32_t i = 0; i < page.cluster_count; i++) {
			const NextGeoCluster &cluster = p_output.clusters[page.cluster_offset + i];
			if (cluster.surface_id >= uint32_t(built.size()) || cluster.page_id >= p_output.root_page_count) {
				return ERR_INVALID_DATA;
			}
			const uint64_t vertex_begin = uint64_t(cluster.vertex_offset) * layout.stride;
			const uint64_t vertex_floats = uint64_t(cluster.vertex_count) * layout.stride;
			if (vertex_begin > p_output.vertices.size() || vertex_floats > p_output.vertices.size() - vertex_begin) {
				return ERR_INVALID_DATA;
			}
			const uint64_t triangle_begin = uint64_t(cluster.triangle_offset) * 3;
			const uint64_t triangle_bytes = uint64_t(cluster.triangle_count) * 3;
			if (triangle_begin > p_output.triangles.size() || triangle_bytes > p_output.triangles.size() - triangle_begin) {
				return ERR_INVALID_DATA;
			}

			SurfaceBuild &surface = built.write[cluster.surface_id];
			const int base = surface.vertices.size();
			const float *src = p_output.vertices.ptr() + vertex_begin;
			for (uint32_t v = 0; v < cluster.vertex_count; v++) {
				const float *vertex = src + uint64_t(v) * layout.stride;
				surface.vertices.push_back(Vector3(vertex[layout.position], vertex[layout.position + 1], vertex[layout.position + 2]));
				if (layout.has_normal) {
					surface.normals.push_back(Vector3(vertex[layout.normal], vertex[layout.normal + 1], vertex[layout.normal + 2]));
				}
				if (layout.has_tangent) {
					for (uint32_t k = 0; k < 4; k++) {
						surface.tangents.push_back(vertex[layout.tangent + k]);
					}
				}
				if (layout.has_uv0) {
					surface.uv0.push_back(Vector2(vertex[layout.uv0], vertex[layout.uv0 + 1]));
				}
				if (layout.has_uv2) {
					surface.uv2.push_back(Vector2(vertex[layout.uv2], vertex[layout.uv2 + 1]));
				}
				if (layout.has_color) {
					surface.colors.push_back(Color(vertex[layout.color], vertex[layout.color + 1], vertex[layout.color + 2], vertex[layout.color + 3]));
				}
			}
			const uint8_t *tris = p_output.triangles.ptr() + triangle_begin;
			for (uint32_t t = 0; t < cluster.triangle_count; t++) {
				const uint8_t i0 = tris[t * 3];
				const uint8_t i1 = tris[t * 3 + 1];
				const uint8_t i2 = tris[t * 3 + 2];
				if (i0 >= cluster.vertex_count || i1 >= cluster.vertex_count || i2 >= cluster.vertex_count) {
					return ERR_INVALID_DATA;
				}
				surface.indices.push_back(base + int(i0));
				surface.indices.push_back(base + int(i1));
				surface.indices.push_back(base + int(i2));
			}
		}
	}

	TypedArray<Array> fallback;
	fallback.resize(built.size());
	PackedByteArray eligibility;
	eligibility.resize(p_input.surfaces.size());
	PackedInt32Array reasons;
	reasons.resize(p_input.surfaces.size());
	for (int i = 0; i < p_input.surfaces.size(); i++) {
		eligibility.write[i] = p_input.surfaces[i].eligible ? 1 : 0;
		reasons.write[i] = 0;
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		if (!built[i].vertices.is_empty()) {
			arrays[Mesh::ARRAY_VERTEX] = built[i].vertices;
		}
		if (!built[i].normals.is_empty()) {
			arrays[Mesh::ARRAY_NORMAL] = built[i].normals;
		}
		if (!built[i].tangents.is_empty()) {
			arrays[Mesh::ARRAY_TANGENT] = built[i].tangents;
		}
		if (!built[i].uv0.is_empty()) {
			arrays[Mesh::ARRAY_TEX_UV] = built[i].uv0;
		}
		if (!built[i].uv2.is_empty()) {
			arrays[Mesh::ARRAY_TEX_UV2] = built[i].uv2;
		}
		if (!built[i].colors.is_empty()) {
			arrays[Mesh::ARRAY_COLOR] = built[i].colors;
		}
		if (!built[i].indices.is_empty()) {
			arrays[Mesh::ARRAY_INDEX] = built[i].indices;
		}
		fallback[i] = arrays;
	}

	TypedArray<PackedByteArray> root_pages;
	root_pages.resize(p_output.root_page_count);
	for (uint32_t i = 0; i < p_output.root_page_count; i++) {
		root_pages[i] = p_pages[i];
	}

	int64_t source_bytes = 0;
	for (int i = 0; i < p_input.surfaces.size(); i++) {
		source_bytes += int64_t(p_input.surfaces[i].positions.size()) * 12;
		source_bytes += int64_t(p_input.surfaces[i].indices.size()) * 4;
	}

	p_data->set_format_version(VirtualGeometryData::FORMAT_VERSION);
	p_data->set_dag_header(dag.bytes);
	p_data->set_root_pages(root_pages);
	p_data->set_coarse_fallback(fallback);
	p_data->set_surface_eligibility(eligibility);
	p_data->set_surface_fallback_reasons(reasons);
	p_data->set_source_triangle_count(p_output.source_triangle_count);
	p_data->set_source_vertex_index_bytes(source_bytes);
	p_data->set_cluster_count(p_output.clusters.size());
	p_data->set_group_count(p_output.groups.size());
	p_data->set_dag_depth(p_output.dag_depth);
	p_data->set_page_count(p_pages.size());
	p_data->set_page_size(p_output.page_size);
	return OK;
}
