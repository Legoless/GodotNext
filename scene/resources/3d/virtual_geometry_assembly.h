/**************************************************************************/
/*  virtual_geometry_assembly.h                                           */
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
#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/variant/typed_array.h"
#include "scene/resources/mesh.h"

// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 contract N10).
// Optional Nanite Assembly payload attached to VirtualGeometryData (Experimental, off by default).
// Holds virtualized part meshes with local transforms and child assemblies up to MAX_DEPTH (2) levels.
class VirtualGeometryAssembly : public Resource {
	GDCLASS(VirtualGeometryAssembly, Resource);

public:
	static constexpr int MAX_DEPTH = 2;

private:
	TypedArray<Mesh> parts;
	TypedArray<Transform3D> part_transforms;
	TypedArray<VirtualGeometryAssembly> child_assemblies;
	TypedArray<Transform3D> child_transforms;

protected:
	static void _bind_methods();
	void reset_state() override;

public:
	void set_parts(const TypedArray<Mesh> &p_parts);
	TypedArray<Mesh> get_parts() const;

	void set_part_transforms(const TypedArray<Transform3D> &p_transforms);
	TypedArray<Transform3D> get_part_transforms() const;

	void set_child_assemblies(const TypedArray<VirtualGeometryAssembly> &p_children);
	TypedArray<VirtualGeometryAssembly> get_child_assemblies() const;

	void set_child_transforms(const TypedArray<Transform3D> &p_transforms);
	TypedArray<Transform3D> get_child_transforms() const;

	void add_part(const Ref<Mesh> &p_mesh, const Transform3D &p_transform = Transform3D());
	Error add_child_assembly(const Ref<VirtualGeometryAssembly> &p_child, const Transform3D &p_transform = Transform3D());

	int get_part_count() const;
	int get_child_assembly_count() const;
	int get_depth() const;
	int get_total_leaf_part_count() const;
	AABB get_merged_aabb() const;
	void clear();
};
