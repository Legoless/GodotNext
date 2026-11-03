/**************************************************************************/
/*  virtual_geometry_data.cpp                                             */
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

#include "virtual_geometry_data.h"

#include "core/object/class_db.h"

void VirtualGeometryData::set_format_version(int p_version) {
	format_version = p_version;
	emit_changed();
}

int VirtualGeometryData::get_format_version() const {
	return format_version;
}

void VirtualGeometryData::set_dag_header(const PackedByteArray &p_header) {
	dag_header = p_header;
	emit_changed();
}

PackedByteArray VirtualGeometryData::get_dag_header() const {
	return dag_header;
}

void VirtualGeometryData::set_root_pages(const TypedArray<PackedByteArray> &p_pages) {
	root_pages = p_pages;
	emit_changed();
}

TypedArray<PackedByteArray> VirtualGeometryData::get_root_pages() const {
	return root_pages;
}

void VirtualGeometryData::set_coarse_fallback(const TypedArray<Array> &p_fallback) {
	coarse_fallback = p_fallback;
	emit_changed();
}

TypedArray<Array> VirtualGeometryData::get_coarse_fallback() const {
	return coarse_fallback;
}

void VirtualGeometryData::set_surface_eligibility(const PackedByteArray &p_eligibility) {
	surface_eligibility = p_eligibility;
	emit_changed();
}

PackedByteArray VirtualGeometryData::get_surface_eligibility() const {
	return surface_eligibility;
}

void VirtualGeometryData::set_surface_fallback_reasons(const PackedInt32Array &p_reasons) {
	surface_fallback_reasons = p_reasons;
	emit_changed();
}

PackedInt32Array VirtualGeometryData::get_surface_fallback_reasons() const {
	return surface_fallback_reasons;
}

void VirtualGeometryData::set_rt_proxy_positions(const PackedFloat32Array &p_positions) {
	rt_proxy_positions = p_positions;
	emit_changed();
}

PackedFloat32Array VirtualGeometryData::get_rt_proxy_positions() const {
	return rt_proxy_positions;
}

void VirtualGeometryData::set_rt_proxy_indices(const PackedByteArray &p_indices) {
	rt_proxy_indices = p_indices;
	emit_changed();
}

PackedByteArray VirtualGeometryData::get_rt_proxy_indices() const {
	return rt_proxy_indices;
}

void VirtualGeometryData::set_rt_proxy_parts(const PackedInt32Array &p_parts) {
	rt_proxy_parts = p_parts;
	emit_changed();
}

PackedInt32Array VirtualGeometryData::get_rt_proxy_parts() const {
	return rt_proxy_parts;
}

void VirtualGeometryData::set_rt_proxy_surface_ranges(const PackedInt32Array &p_ranges) {
	rt_proxy_surface_ranges = p_ranges;
	emit_changed();
}

PackedInt32Array VirtualGeometryData::get_rt_proxy_surface_ranges() const {
	return rt_proxy_surface_ranges;
}

void VirtualGeometryData::set_rt_proxy_clusters(const PackedInt32Array &p_clusters) {
	rt_proxy_clusters = p_clusters;
	emit_changed();
}

PackedInt32Array VirtualGeometryData::get_rt_proxy_clusters() const {
	return rt_proxy_clusters;
}

void VirtualGeometryData::set_rt_proxy_eps(float p_eps) {
	rt_proxy_eps = p_eps;
	emit_changed();
}

float VirtualGeometryData::get_rt_proxy_eps() const {
	return rt_proxy_eps;
}

void VirtualGeometryData::set_rt_proxy_error(float p_error) {
	rt_proxy_error = p_error;
	emit_changed();
}

float VirtualGeometryData::get_rt_proxy_error() const {
	return rt_proxy_error;
}

void VirtualGeometryData::set_card_descriptors(const PackedByteArray &p_descriptors) {
	card_descriptors = p_descriptors;
	emit_changed();
}

PackedByteArray VirtualGeometryData::get_card_descriptors() const {
	return card_descriptors;
}

void VirtualGeometryData::set_ngm_path(const String &p_path) {
	ngm_path = p_path;
	emit_changed();
}

String VirtualGeometryData::get_ngm_path() const {
	return ngm_path;
}

void VirtualGeometryData::set_ngm_sha256(const PackedByteArray &p_sha256) {
	ngm_sha256 = p_sha256;
	emit_changed();
}

PackedByteArray VirtualGeometryData::get_ngm_sha256() const {
	return ngm_sha256;
}

void VirtualGeometryData::set_source_triangle_count(int64_t p_count) {
	source_triangle_count = p_count;
	emit_changed();
}

int64_t VirtualGeometryData::get_source_triangle_count() const {
	return source_triangle_count;
}

void VirtualGeometryData::set_source_vertex_index_bytes(int64_t p_bytes) {
	source_vertex_index_bytes = p_bytes;
	emit_changed();
}

int64_t VirtualGeometryData::get_source_vertex_index_bytes() const {
	return source_vertex_index_bytes;
}

void VirtualGeometryData::set_cluster_count(int64_t p_count) {
	cluster_count = p_count;
	emit_changed();
}

int64_t VirtualGeometryData::get_cluster_count() const {
	return cluster_count;
}

void VirtualGeometryData::set_group_count(int64_t p_count) {
	group_count = p_count;
	emit_changed();
}

int64_t VirtualGeometryData::get_group_count() const {
	return group_count;
}

void VirtualGeometryData::set_dag_depth(int64_t p_depth) {
	dag_depth = p_depth;
	emit_changed();
}

int64_t VirtualGeometryData::get_dag_depth() const {
	return dag_depth;
}

void VirtualGeometryData::set_page_count(int64_t p_count) {
	page_count = p_count;
	emit_changed();
}

int64_t VirtualGeometryData::get_page_count() const {
	return page_count;
}

void VirtualGeometryData::set_page_size(int64_t p_size) {
	page_size = p_size;
	emit_changed();
}

int64_t VirtualGeometryData::get_page_size() const {
	return page_size;
}

void VirtualGeometryData::set_ngm_page_table(uint32_t p_vertex_layout, const PackedInt32Array &p_page_groups) {
	ngm_vertex_layout = p_vertex_layout;
	ngm_page_groups = p_page_groups;
}

uint32_t VirtualGeometryData::get_ngm_vertex_layout() const {
	return ngm_vertex_layout;
}

PackedInt32Array VirtualGeometryData::get_ngm_page_groups() const {
	return ngm_page_groups;
}

void VirtualGeometryData::reset_state() {
	format_version = FORMAT_VERSION;
	dag_header.clear();
	root_pages.clear();
	coarse_fallback.clear();
	surface_eligibility.clear();
	surface_fallback_reasons.clear();
	rt_proxy_positions.clear();
	rt_proxy_indices.clear();
	rt_proxy_parts.clear();
	rt_proxy_surface_ranges.clear();
	rt_proxy_clusters.clear();
	rt_proxy_eps = 0.0f;
	rt_proxy_error = 0.0f;
	card_descriptors.clear();
	ngm_path.clear();
	ngm_sha256.clear();
	source_triangle_count = 0;
	source_vertex_index_bytes = 0;
	cluster_count = 0;
	group_count = 0;
	dag_depth = 0;
	page_count = 0;
	page_size = 0;
	ngm_vertex_layout = 0;
	ngm_page_groups.clear();
}

void VirtualGeometryData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_format_version", "version"), &VirtualGeometryData::set_format_version);
	ClassDB::bind_method(D_METHOD("get_format_version"), &VirtualGeometryData::get_format_version);
	ClassDB::bind_method(D_METHOD("set_dag_header", "header"), &VirtualGeometryData::set_dag_header);
	ClassDB::bind_method(D_METHOD("get_dag_header"), &VirtualGeometryData::get_dag_header);
	ClassDB::bind_method(D_METHOD("set_root_pages", "pages"), &VirtualGeometryData::set_root_pages);
	ClassDB::bind_method(D_METHOD("get_root_pages"), &VirtualGeometryData::get_root_pages);
	ClassDB::bind_method(D_METHOD("set_coarse_fallback", "fallback"), &VirtualGeometryData::set_coarse_fallback);
	ClassDB::bind_method(D_METHOD("get_coarse_fallback"), &VirtualGeometryData::get_coarse_fallback);
	ClassDB::bind_method(D_METHOD("set_surface_eligibility", "eligibility"), &VirtualGeometryData::set_surface_eligibility);
	ClassDB::bind_method(D_METHOD("get_surface_eligibility"), &VirtualGeometryData::get_surface_eligibility);
	ClassDB::bind_method(D_METHOD("set_surface_fallback_reasons", "reasons"), &VirtualGeometryData::set_surface_fallback_reasons);
	ClassDB::bind_method(D_METHOD("get_surface_fallback_reasons"), &VirtualGeometryData::get_surface_fallback_reasons);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_positions", "positions"), &VirtualGeometryData::set_rt_proxy_positions);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_positions"), &VirtualGeometryData::get_rt_proxy_positions);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_indices", "indices"), &VirtualGeometryData::set_rt_proxy_indices);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_indices"), &VirtualGeometryData::get_rt_proxy_indices);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_parts", "parts"), &VirtualGeometryData::set_rt_proxy_parts);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_parts"), &VirtualGeometryData::get_rt_proxy_parts);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_surface_ranges", "ranges"), &VirtualGeometryData::set_rt_proxy_surface_ranges);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_surface_ranges"), &VirtualGeometryData::get_rt_proxy_surface_ranges);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_clusters", "clusters"), &VirtualGeometryData::set_rt_proxy_clusters);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_clusters"), &VirtualGeometryData::get_rt_proxy_clusters);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_eps", "eps"), &VirtualGeometryData::set_rt_proxy_eps);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_eps"), &VirtualGeometryData::get_rt_proxy_eps);
	ClassDB::bind_method(D_METHOD("set_rt_proxy_error", "error"), &VirtualGeometryData::set_rt_proxy_error);
	ClassDB::bind_method(D_METHOD("get_rt_proxy_error"), &VirtualGeometryData::get_rt_proxy_error);
	ClassDB::bind_method(D_METHOD("set_card_descriptors", "descriptors"), &VirtualGeometryData::set_card_descriptors);
	ClassDB::bind_method(D_METHOD("get_card_descriptors"), &VirtualGeometryData::get_card_descriptors);
	ClassDB::bind_method(D_METHOD("set_ngm_path", "path"), &VirtualGeometryData::set_ngm_path);
	ClassDB::bind_method(D_METHOD("get_ngm_path"), &VirtualGeometryData::get_ngm_path);
	ClassDB::bind_method(D_METHOD("set_ngm_sha256", "sha256"), &VirtualGeometryData::set_ngm_sha256);
	ClassDB::bind_method(D_METHOD("get_ngm_sha256"), &VirtualGeometryData::get_ngm_sha256);
	ClassDB::bind_method(D_METHOD("set_source_triangle_count", "count"), &VirtualGeometryData::set_source_triangle_count);
	ClassDB::bind_method(D_METHOD("get_source_triangle_count"), &VirtualGeometryData::get_source_triangle_count);
	ClassDB::bind_method(D_METHOD("set_source_vertex_index_bytes", "bytes"), &VirtualGeometryData::set_source_vertex_index_bytes);
	ClassDB::bind_method(D_METHOD("get_source_vertex_index_bytes"), &VirtualGeometryData::get_source_vertex_index_bytes);
	ClassDB::bind_method(D_METHOD("set_cluster_count", "count"), &VirtualGeometryData::set_cluster_count);
	ClassDB::bind_method(D_METHOD("get_cluster_count"), &VirtualGeometryData::get_cluster_count);
	ClassDB::bind_method(D_METHOD("set_group_count", "count"), &VirtualGeometryData::set_group_count);
	ClassDB::bind_method(D_METHOD("get_group_count"), &VirtualGeometryData::get_group_count);
	ClassDB::bind_method(D_METHOD("set_dag_depth", "depth"), &VirtualGeometryData::set_dag_depth);
	ClassDB::bind_method(D_METHOD("get_dag_depth"), &VirtualGeometryData::get_dag_depth);
	ClassDB::bind_method(D_METHOD("set_page_count", "count"), &VirtualGeometryData::set_page_count);
	ClassDB::bind_method(D_METHOD("get_page_count"), &VirtualGeometryData::get_page_count);
	ClassDB::bind_method(D_METHOD("set_page_size", "size"), &VirtualGeometryData::set_page_size);
	ClassDB::bind_method(D_METHOD("get_page_size"), &VirtualGeometryData::get_page_size);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "format_version"), "set_format_version", "get_format_version");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "dag_header", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_dag_header", "get_dag_header");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "root_pages", PROPERTY_HINT_ARRAY_TYPE, "PackedByteArray", PROPERTY_USAGE_NO_EDITOR), "set_root_pages", "get_root_pages");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "coarse_fallback", PROPERTY_HINT_ARRAY_TYPE, "Array", PROPERTY_USAGE_NO_EDITOR), "set_coarse_fallback", "get_coarse_fallback");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "surface_eligibility", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_surface_eligibility", "get_surface_eligibility");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "surface_fallback_reasons", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_surface_fallback_reasons", "get_surface_fallback_reasons");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "rt_proxy_positions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_rt_proxy_positions", "get_rt_proxy_positions");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "rt_proxy_indices", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_rt_proxy_indices", "get_rt_proxy_indices");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "rt_proxy_parts", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_rt_proxy_parts", "get_rt_proxy_parts");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "rt_proxy_surface_ranges", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_rt_proxy_surface_ranges", "get_rt_proxy_surface_ranges");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "rt_proxy_clusters", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_rt_proxy_clusters", "get_rt_proxy_clusters");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rt_proxy_eps"), "set_rt_proxy_eps", "get_rt_proxy_eps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rt_proxy_error"), "set_rt_proxy_error", "get_rt_proxy_error");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "card_descriptors", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_card_descriptors", "get_card_descriptors");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "ngm_path"), "set_ngm_path", "get_ngm_path");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "ngm_sha256", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_ngm_sha256", "get_ngm_sha256");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_triangle_count"), "set_source_triangle_count", "get_source_triangle_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_vertex_index_bytes"), "set_source_vertex_index_bytes", "get_source_vertex_index_bytes");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cluster_count"), "set_cluster_count", "get_cluster_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "group_count"), "set_group_count", "get_group_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dag_depth"), "set_dag_depth", "get_dag_depth");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "page_count"), "set_page_count", "get_page_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "page_size"), "set_page_size", "get_page_size");
}
