/**************************************************************************/
/*  mesh_distance_field.cpp                                               */
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

#include "mesh_distance_field.h"

#include "core/object/class_db.h"

void MeshDistanceField::set_format_version(int p_version) {
	format_version = p_version;
	emit_changed();
}

int MeshDistanceField::get_format_version() const {
	return format_version;
}

void MeshDistanceField::set_bounds(const AABB &p_bounds) {
	bounds = p_bounds;
	emit_changed();
}

AABB MeshDistanceField::get_bounds() const {
	return bounds;
}

void MeshDistanceField::set_voxel_size(float p_voxel_size) {
	voxel_size = p_voxel_size;
	emit_changed();
}

float MeshDistanceField::get_voxel_size() const {
	return voxel_size;
}

void MeshDistanceField::set_flags(int p_flags) {
	flags = p_flags;
	emit_changed();
}

int MeshDistanceField::get_flags() const {
	return flags;
}

bool MeshDistanceField::is_two_sided() const {
	return (flags & FLAG_TWO_SIDED) != 0;
}

void MeshDistanceField::set_mip_dimensions(const PackedInt32Array &p_dimensions) {
	mip_dimensions = p_dimensions;
	emit_changed();
}

PackedInt32Array MeshDistanceField::get_mip_dimensions() const {
	return mip_dimensions;
}

void MeshDistanceField::set_mip_brick_grid_dimensions(const PackedInt32Array &p_dimensions) {
	mip_brick_grid_dimensions = p_dimensions;
	emit_changed();
}

PackedInt32Array MeshDistanceField::get_mip_brick_grid_dimensions() const {
	return mip_brick_grid_dimensions;
}

void MeshDistanceField::set_mip_bands(const PackedFloat32Array &p_bands) {
	mip_bands = p_bands;
	emit_changed();
}

PackedFloat32Array MeshDistanceField::get_mip_bands() const {
	return mip_bands;
}

void MeshDistanceField::set_page_tables(const TypedArray<PackedInt32Array> &p_tables) {
	page_tables = p_tables;
	emit_changed();
}

TypedArray<PackedInt32Array> MeshDistanceField::get_page_tables() const {
	return page_tables;
}

void MeshDistanceField::set_brick_data(const TypedArray<PackedByteArray> &p_data) {
	brick_data = p_data;
	emit_changed();
}

TypedArray<PackedByteArray> MeshDistanceField::get_brick_data() const {
	return brick_data;
}

void MeshDistanceField::set_data(const MeshSDFData &p_data) {
	format_version = FORMAT_VERSION;
	bounds = p_data.bounds;
	voxel_size = p_data.voxel_size;
	flags = int(p_data.flags);
	mip_dimensions.clear();
	mip_brick_grid_dimensions.clear();
	mip_bands.clear();
	page_tables.clear();
	brick_data.clear();
	for (int i = 0; i < p_data.mip_dimensions.size(); i++) {
		mip_dimensions.push_back(p_data.mip_dimensions[i].x);
		mip_dimensions.push_back(p_data.mip_dimensions[i].y);
		mip_dimensions.push_back(p_data.mip_dimensions[i].z);
		mip_brick_grid_dimensions.push_back(p_data.mip_brick_grid_dimensions[i].x);
		mip_brick_grid_dimensions.push_back(p_data.mip_brick_grid_dimensions[i].y);
		mip_brick_grid_dimensions.push_back(p_data.mip_brick_grid_dimensions[i].z);
		mip_bands.push_back(p_data.mip_bands[i]);
		page_tables.push_back(p_data.page_tables[i]);
		brick_data.push_back(p_data.brick_data[i]);
	}
	emit_changed();
}

MeshSDFData MeshDistanceField::get_data() const {
	MeshSDFData data;
	data.bounds = bounds;
	data.voxel_size = voxel_size;
	data.flags = uint32_t(flags);
	int mip_count = get_mip_count();
	for (int i = 0; i < mip_count; i++) {
		data.mip_dimensions.push_back(Vector3i(mip_dimensions[i * 3], mip_dimensions[i * 3 + 1], mip_dimensions[i * 3 + 2]));
		data.mip_brick_grid_dimensions.push_back(Vector3i(mip_brick_grid_dimensions[i * 3], mip_brick_grid_dimensions[i * 3 + 1], mip_brick_grid_dimensions[i * 3 + 2]));
		data.mip_bands.push_back(mip_bands[i]);
		data.page_tables.push_back(page_tables[i]);
		data.brick_data.push_back(brick_data[i]);
	}
	return data;
}

int MeshDistanceField::get_mip_count() const {
	return mip_dimensions.size() / 3;
}

int MeshDistanceField::get_brick_count(int p_mip) const {
	ERR_FAIL_INDEX_V(p_mip, get_mip_count(), 0);
	PackedByteArray bricks = brick_data[p_mip];
	return int(bricks.size() / MeshSDFData::BRICK_VOXELS);
}

int64_t MeshDistanceField::get_data_size() const {
	return get_data().get_data_size();
}

float MeshDistanceField::get_voxel_distance(int p_mip, const Vector3i &p_voxel) const {
	return get_data().get_voxel_distance(p_mip, p_voxel);
}

float MeshDistanceField::sample_distance(const Vector3 &p_position, int p_mip) const {
	return get_data().sample_distance(p_position, p_mip);
}

void MeshDistanceField::reset_state() {
	format_version = FORMAT_VERSION;
	bounds = AABB();
	voxel_size = 0.0f;
	flags = 0;
	mip_dimensions.clear();
	mip_brick_grid_dimensions.clear();
	mip_bands.clear();
	page_tables.clear();
	brick_data.clear();
}

void MeshDistanceField::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_format_version", "version"), &MeshDistanceField::set_format_version);
	ClassDB::bind_method(D_METHOD("get_format_version"), &MeshDistanceField::get_format_version);
	ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &MeshDistanceField::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &MeshDistanceField::get_bounds);
	ClassDB::bind_method(D_METHOD("set_voxel_size", "voxel_size"), &MeshDistanceField::set_voxel_size);
	ClassDB::bind_method(D_METHOD("get_voxel_size"), &MeshDistanceField::get_voxel_size);
	ClassDB::bind_method(D_METHOD("set_flags", "flags"), &MeshDistanceField::set_flags);
	ClassDB::bind_method(D_METHOD("get_flags"), &MeshDistanceField::get_flags);
	ClassDB::bind_method(D_METHOD("is_two_sided"), &MeshDistanceField::is_two_sided);
	ClassDB::bind_method(D_METHOD("set_mip_dimensions", "dimensions"), &MeshDistanceField::set_mip_dimensions);
	ClassDB::bind_method(D_METHOD("get_mip_dimensions"), &MeshDistanceField::get_mip_dimensions);
	ClassDB::bind_method(D_METHOD("set_mip_brick_grid_dimensions", "dimensions"), &MeshDistanceField::set_mip_brick_grid_dimensions);
	ClassDB::bind_method(D_METHOD("get_mip_brick_grid_dimensions"), &MeshDistanceField::get_mip_brick_grid_dimensions);
	ClassDB::bind_method(D_METHOD("set_mip_bands", "bands"), &MeshDistanceField::set_mip_bands);
	ClassDB::bind_method(D_METHOD("get_mip_bands"), &MeshDistanceField::get_mip_bands);
	ClassDB::bind_method(D_METHOD("set_page_tables", "tables"), &MeshDistanceField::set_page_tables);
	ClassDB::bind_method(D_METHOD("get_page_tables"), &MeshDistanceField::get_page_tables);
	ClassDB::bind_method(D_METHOD("set_brick_data", "data"), &MeshDistanceField::set_brick_data);
	ClassDB::bind_method(D_METHOD("get_brick_data"), &MeshDistanceField::get_brick_data);
	ClassDB::bind_method(D_METHOD("get_mip_count"), &MeshDistanceField::get_mip_count);
	ClassDB::bind_method(D_METHOD("get_brick_count", "mip"), &MeshDistanceField::get_brick_count);
	ClassDB::bind_method(D_METHOD("get_data_size"), &MeshDistanceField::get_data_size);
	ClassDB::bind_method(D_METHOD("get_voxel_distance", "mip", "voxel"), &MeshDistanceField::get_voxel_distance);
	ClassDB::bind_method(D_METHOD("sample_distance", "position", "mip"), &MeshDistanceField::sample_distance, DEFVAL(0));

	ADD_PROPERTY(PropertyInfo(Variant::INT, "format_version"), "set_format_version", "get_format_version");
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "bounds", PROPERTY_HINT_NO_NODEPATH, "suffix:m"), "set_bounds", "get_bounds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_size", PROPERTY_HINT_NONE, "suffix:m"), "set_voxel_size", "get_voxel_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flags"), "set_flags", "get_flags");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "mip_dimensions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_mip_dimensions", "get_mip_dimensions");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "mip_brick_grid_dimensions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_mip_brick_grid_dimensions", "get_mip_brick_grid_dimensions");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "mip_bands", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_mip_bands", "get_mip_bands");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "page_tables", PROPERTY_HINT_ARRAY_TYPE, "PackedInt32Array", PROPERTY_USAGE_NO_EDITOR), "set_page_tables", "get_page_tables");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "brick_data", PROPERTY_HINT_ARRAY_TYPE, "PackedByteArray", PROPERTY_USAGE_NO_EDITOR), "set_brick_data", "get_brick_data");

	BIND_ENUM_CONSTANT(FLAG_TWO_SIDED);
	BIND_ENUM_CONSTANT(FLAG_UNSIGNED);
}
