/**************************************************************************/
/*  virtual_geometry_assembly.cpp                                         */
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

#include "virtual_geometry_assembly.h"

#include "core/object/class_db.h"

void VirtualGeometryAssembly::set_parts(const TypedArray<Mesh> &p_parts) {
	parts = p_parts;
	emit_changed();
}

TypedArray<Mesh> VirtualGeometryAssembly::get_parts() const {
	return parts;
}

void VirtualGeometryAssembly::set_part_transforms(const TypedArray<Transform3D> &p_transforms) {
	part_transforms = p_transforms;
	emit_changed();
}

TypedArray<Transform3D> VirtualGeometryAssembly::get_part_transforms() const {
	return part_transforms;
}

void VirtualGeometryAssembly::set_child_assemblies(const TypedArray<VirtualGeometryAssembly> &p_children) {
	TypedArray<VirtualGeometryAssembly> sanitized;
	for (int i = 0; i < p_children.size(); i++) {
		Ref<VirtualGeometryAssembly> child = p_children[i];
		if (child.is_null() || child.ptr() == this) {
			continue;
		}
		if (child->get_child_assembly_count() > 0) {
			WARN_PRINT("VirtualGeometryAssembly supports at most 2 hierarchy levels; grandchildren beyond level 2 are ignored.");
		}
		sanitized.push_back(child);
	}
	child_assemblies = sanitized;
	emit_changed();
}

TypedArray<VirtualGeometryAssembly> VirtualGeometryAssembly::get_child_assemblies() const {
	return child_assemblies;
}

void VirtualGeometryAssembly::set_child_transforms(const TypedArray<Transform3D> &p_transforms) {
	child_transforms = p_transforms;
	emit_changed();
}

TypedArray<Transform3D> VirtualGeometryAssembly::get_child_transforms() const {
	return child_transforms;
}

void VirtualGeometryAssembly::add_part(const Ref<Mesh> &p_mesh, const Transform3D &p_transform) {
	ERR_FAIL_COND_MSG(p_mesh.is_null(), "VirtualGeometryAssembly::add_part requires a valid Mesh.");
	while (part_transforms.size() < parts.size()) {
		part_transforms.push_back(Transform3D());
	}
	parts.push_back(p_mesh);
	part_transforms.push_back(p_transform);
	emit_changed();
}

Error VirtualGeometryAssembly::add_child_assembly(const Ref<VirtualGeometryAssembly> &p_child, const Transform3D &p_transform) {
	ERR_FAIL_COND_V_MSG(p_child.is_null(), ERR_INVALID_PARAMETER, "VirtualGeometryAssembly::add_child_assembly requires a valid child assembly.");
	ERR_FAIL_COND_V_MSG(p_child.ptr() == this, ERR_INVALID_PARAMETER, "VirtualGeometryAssembly cannot reference itself as a child assembly.");
	ERR_FAIL_COND_V_MSG(p_child->get_child_assembly_count() > 0, ERR_INVALID_PARAMETER, "VirtualGeometryAssembly supports at most 2 hierarchy levels (instance -> child assembly -> sub-part).");
	while (child_transforms.size() < child_assemblies.size()) {
		child_transforms.push_back(Transform3D());
	}
	child_assemblies.push_back(p_child);
	child_transforms.push_back(p_transform);
	emit_changed();
	return OK;
}

int VirtualGeometryAssembly::get_part_count() const {
	return parts.size();
}

int VirtualGeometryAssembly::get_child_assembly_count() const {
	return child_assemblies.size();
}

int VirtualGeometryAssembly::get_depth() const {
	for (int i = 0; i < child_assemblies.size(); i++) {
		Ref<VirtualGeometryAssembly> child = child_assemblies[i];
		if (child.is_valid() && child->get_part_count() > 0) {
			return 2;
		}
	}
	return parts.is_empty() ? 0 : 1;
}

int VirtualGeometryAssembly::get_total_leaf_part_count() const {
	int total = 0;
	for (int i = 0; i < parts.size(); i++) {
		Ref<Mesh> part = parts[i];
		if (part.is_valid()) {
			total++;
		}
	}
	for (int i = 0; i < child_assemblies.size(); i++) {
		Ref<VirtualGeometryAssembly> child = child_assemblies[i];
		if (child.is_null() || child.ptr() == this) {
			continue;
		}
		const TypedArray<Mesh> sub_parts = child->get_parts();
		for (int j = 0; j < sub_parts.size(); j++) {
			Ref<Mesh> sub_mesh = sub_parts[j];
			if (sub_mesh.is_valid()) {
				total++;
			}
		}
	}
	return total;
}

AABB VirtualGeometryAssembly::get_merged_aabb() const {
	AABB merged;
	bool has_any = false;
	auto merge_mesh = [&](const Ref<Mesh> &p_mesh, const Transform3D &p_xform) {
		if (p_mesh.is_null()) {
			return;
		}
		const AABB local = p_mesh->get_aabb();
		const AABB xformed = p_xform.xform(local);
		if (!has_any) {
			merged = xformed;
			has_any = true;
		} else {
			merged.merge_with(xformed);
		}
	};

	for (int i = 0; i < parts.size(); i++) {
		Ref<Mesh> part = parts[i];
		const Transform3D xf = i < part_transforms.size() ? Transform3D(part_transforms[i]) : Transform3D();
		merge_mesh(part, xf);
	}
	for (int i = 0; i < child_assemblies.size(); i++) {
		Ref<VirtualGeometryAssembly> child = child_assemblies[i];
		if (child.is_null() || child.ptr() == this) {
			continue;
		}
		const Transform3D child_xf = i < child_transforms.size() ? Transform3D(child_transforms[i]) : Transform3D();
		const TypedArray<Mesh> sub_parts = child->get_parts();
		const TypedArray<Transform3D> sub_xfs = child->get_part_transforms();
		for (int j = 0; j < sub_parts.size(); j++) {
			Ref<Mesh> sub_mesh = sub_parts[j];
			const Transform3D sub_xf = j < sub_xfs.size() ? Transform3D(sub_xfs[j]) : Transform3D();
			merge_mesh(sub_mesh, child_xf * sub_xf);
		}
	}
	return merged;
}

void VirtualGeometryAssembly::clear() {
	parts.clear();
	part_transforms.clear();
	child_assemblies.clear();
	child_transforms.clear();
	emit_changed();
}

void VirtualGeometryAssembly::reset_state() {
	parts.clear();
	part_transforms.clear();
	child_assemblies.clear();
	child_transforms.clear();
}

void VirtualGeometryAssembly::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_parts", "parts"), &VirtualGeometryAssembly::set_parts);
	ClassDB::bind_method(D_METHOD("get_parts"), &VirtualGeometryAssembly::get_parts);
	ClassDB::bind_method(D_METHOD("set_part_transforms", "transforms"), &VirtualGeometryAssembly::set_part_transforms);
	ClassDB::bind_method(D_METHOD("get_part_transforms"), &VirtualGeometryAssembly::get_part_transforms);
	ClassDB::bind_method(D_METHOD("set_child_assemblies", "children"), &VirtualGeometryAssembly::set_child_assemblies);
	ClassDB::bind_method(D_METHOD("get_child_assemblies"), &VirtualGeometryAssembly::get_child_assemblies);
	ClassDB::bind_method(D_METHOD("set_child_transforms", "transforms"), &VirtualGeometryAssembly::set_child_transforms);
	ClassDB::bind_method(D_METHOD("get_child_transforms"), &VirtualGeometryAssembly::get_child_transforms);
	ClassDB::bind_method(D_METHOD("add_part", "mesh", "transform"), &VirtualGeometryAssembly::add_part, DEFVAL(Transform3D()));
	ClassDB::bind_method(D_METHOD("add_child_assembly", "child", "transform"), &VirtualGeometryAssembly::add_child_assembly, DEFVAL(Transform3D()));
	ClassDB::bind_method(D_METHOD("get_part_count"), &VirtualGeometryAssembly::get_part_count);
	ClassDB::bind_method(D_METHOD("get_child_assembly_count"), &VirtualGeometryAssembly::get_child_assembly_count);
	ClassDB::bind_method(D_METHOD("get_depth"), &VirtualGeometryAssembly::get_depth);
	ClassDB::bind_method(D_METHOD("get_total_leaf_part_count"), &VirtualGeometryAssembly::get_total_leaf_part_count);
	ClassDB::bind_method(D_METHOD("get_merged_aabb"), &VirtualGeometryAssembly::get_merged_aabb);
	ClassDB::bind_method(D_METHOD("clear"), &VirtualGeometryAssembly::clear);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "parts", PROPERTY_HINT_ARRAY_TYPE, "Mesh"), "set_parts", "get_parts");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "part_transforms", PROPERTY_HINT_ARRAY_TYPE, "Transform3D"), "set_part_transforms", "get_part_transforms");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "child_assemblies", PROPERTY_HINT_ARRAY_TYPE, "VirtualGeometryAssembly"), "set_child_assemblies", "get_child_assemblies");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "child_transforms", PROPERTY_HINT_ARRAY_TYPE, "Transform3D"), "set_child_transforms", "get_child_transforms");

	BIND_CONSTANT(MAX_DEPTH);
}
