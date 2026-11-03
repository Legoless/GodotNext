/**************************************************************************/
/*  virtual_geometry_data.h                                               */
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

#include "core/io/resource.h"
#include "core/variant/typed_array.h"

// Sub-resource referenced by ArrayMesh (plan §7.2). It is not a Mesh subclass: the
// authored ArrayMesh RID and its surfaces stay in place, and any surface mutation
// detaches this resource before the next frame (§6.3).
//
// `.ngm` is a sidecar, not a resource, so this stores a path and a SHA-256 and no UID.
// The RT proxy fields are filled by the proxy builder; card descriptors stay empty until P5.
class VirtualGeometryData : public Resource {
	GDCLASS(VirtualGeometryData, Resource);

public:
	static constexpr int FORMAT_VERSION = 1;

private:
	int format_version = FORMAT_VERSION;
	PackedByteArray dag_header;
	TypedArray<PackedByteArray> root_pages;
	TypedArray<Array> coarse_fallback;
	PackedByteArray surface_eligibility;
	PackedInt32Array surface_fallback_reasons;
	PackedFloat32Array rt_proxy_positions;
	PackedByteArray rt_proxy_indices;
	PackedInt32Array rt_proxy_parts;
	PackedInt32Array rt_proxy_surface_ranges;
	PackedInt32Array rt_proxy_clusters;
	float rt_proxy_eps = 0.0f;
	float rt_proxy_error = 0.0f;
	PackedByteArray card_descriptors;
	String ngm_path;
	PackedByteArray ngm_sha256;
	int64_t source_triangle_count = 0;
	int64_t source_vertex_index_bytes = 0;
	int64_t cluster_count = 0;
	int64_t group_count = 0;
	int64_t dag_depth = 0;
	int64_t page_count = 0;
	int64_t page_size = 0;
	// Not saved and not bound. The importer hook stashes what NGMWriter needs beyond the
	// page payloads: vertex layout, and (first_group, group_count) pairs in page order.
	uint32_t ngm_vertex_layout = 0;
	PackedInt32Array ngm_page_groups;

protected:
	static void _bind_methods();
	void reset_state() override;

public:
	void set_format_version(int p_version);
	int get_format_version() const;

	// Groups, then BVH8 nodes. Little-endian; see nextgeo_fill_vgd().
	void set_dag_header(const PackedByteArray &p_header);
	PackedByteArray get_dag_header() const;

	void set_root_pages(const TypedArray<PackedByteArray> &p_pages);
	TypedArray<PackedByteArray> get_root_pages() const;

	// One Mesh::ARRAY_* array per source surface. The root cut of each eligible surface;
	// ineligible surfaces are empty arrays of the same length.
	void set_coarse_fallback(const TypedArray<Array> &p_fallback);
	TypedArray<Array> get_coarse_fallback() const;

	// One byte per source surface: 1 = virtualized, 0 = legacy.
	void set_surface_eligibility(const PackedByteArray &p_eligibility);
	PackedByteArray get_surface_eligibility() const;

	// One int per source surface. 0 when virtualized (and until import records a reason);
	// otherwise the import-time per-(instance, surface) Fallback value (§6.3).
	void set_surface_fallback_reasons(const PackedInt32Array &p_reasons);
	PackedInt32Array get_surface_fallback_reasons() const;

	// Float32 xyz, tightly packed, including in double-precision builds.
	void set_rt_proxy_positions(const PackedFloat32Array &p_positions);
	PackedFloat32Array get_rt_proxy_positions() const;

	// Little-endian uint32 indices, 4 bytes each.
	void set_rt_proxy_indices(const PackedByteArray &p_indices);
	PackedByteArray get_rt_proxy_indices() const;

	// Groups of 4: vertex_offset, vertex_count, index_offset, index_count. One group per BLAS part.
	void set_rt_proxy_parts(const PackedInt32Array &p_parts);
	PackedInt32Array get_rt_proxy_parts() const;

	// Groups of 2: first_index, index_count. One group per (part, surface), part-major,
	// relative to that part. Legacy surfaces are [0, 0].
	void set_rt_proxy_surface_ranges(const PackedInt32Array &p_ranges);
	PackedInt32Array get_rt_proxy_surface_ranges() const;

	void set_rt_proxy_clusters(const PackedInt32Array &p_clusters);
	PackedInt32Array get_rt_proxy_clusters() const;

	void set_rt_proxy_eps(float p_eps);
	float get_rt_proxy_eps() const;

	void set_rt_proxy_error(float p_error);
	float get_rt_proxy_error() const;

	void set_card_descriptors(const PackedByteArray &p_descriptors);
	PackedByteArray get_card_descriptors() const;

	void set_ngm_path(const String &p_path);
	String get_ngm_path() const;

	void set_ngm_sha256(const PackedByteArray &p_sha256);
	PackedByteArray get_ngm_sha256() const;

	void set_source_triangle_count(int64_t p_count);
	int64_t get_source_triangle_count() const;

	void set_source_vertex_index_bytes(int64_t p_bytes);
	int64_t get_source_vertex_index_bytes() const;

	void set_cluster_count(int64_t p_count);
	int64_t get_cluster_count() const;

	void set_group_count(int64_t p_count);
	int64_t get_group_count() const;

	void set_dag_depth(int64_t p_depth);
	int64_t get_dag_depth() const;

	void set_page_count(int64_t p_count);
	int64_t get_page_count() const;

	void set_page_size(int64_t p_size);
	int64_t get_page_size() const;

	// Transient `.ngm` write table. Cleared by reset_state. Not a property.
	void set_ngm_page_table(uint32_t p_vertex_layout, const PackedInt32Array &p_page_groups);
	uint32_t get_ngm_vertex_layout() const;
	PackedInt32Array get_ngm_page_groups() const;
};
