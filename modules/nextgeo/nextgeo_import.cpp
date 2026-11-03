/**************************************************************************/
/*  nextgeo_import.cpp                                                    */
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

#include "nextgeo_import.h"

#include "nextgeo_dag_builder.h"
#include "nextgeo_serialize.h"

#include "core/io/resource_format_ngm.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/mesh.h"

// The ray-tracing proxy (plan §7.1, `eps_rt = min(max_extent/256, rt_max_error)`) is not
// built here. `nextgeo_build_rt_proxy()` does not exist yet, and neither does the
// `rt_max_error` project setting. RT proxy fields stay at their empty defaults. Card
// descriptors stay empty until P5.

static bool _arrays_are_triangles(const Array &p_arrays) {
	if (p_arrays.size() != Mesh::ARRAY_MAX) {
		return false;
	}
	if (p_arrays[Mesh::ARRAY_VERTEX].get_type() == Variant::NIL) {
		return false;
	}
	const PackedVector3Array vertices = p_arrays[Mesh::ARRAY_VERTEX];
	if (vertices.is_empty()) {
		return false;
	}
	if (p_arrays[Mesh::ARRAY_INDEX].get_type() == Variant::NIL) {
		return vertices.size() % 3 == 0;
	}
	const PackedInt32Array indices = p_arrays[Mesh::ARRAY_INDEX];
	if (indices.is_empty()) {
		return vertices.size() % 3 == 0;
	}
	return indices.size() % 3 == 0;
}

static Vector<Vector3> _as_vec3(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return Vector<Vector3>();
	}
	return p_value;
}

static Vector<Vector2> _as_vec2(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return Vector<Vector2>();
	}
	return p_value;
}

static Vector<float> _as_floats(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return Vector<float>();
	}
	return p_value;
}

static Vector<Color> _as_colors(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return Vector<Color>();
	}
	return p_value;
}

static Vector<int32_t> _as_indices(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return Vector<int32_t>();
	}
	return p_value;
}

static void _copy_triangle_surface(const Array &p_arrays, NextGeoSurfaceInput &r_surface) {
	r_surface.positions = _as_vec3(p_arrays[Mesh::ARRAY_VERTEX]);
	r_surface.normals = _as_vec3(p_arrays[Mesh::ARRAY_NORMAL]);
	r_surface.tangents = _as_floats(p_arrays[Mesh::ARRAY_TANGENT]);
	r_surface.uv0 = _as_vec2(p_arrays[Mesh::ARRAY_TEX_UV]);
	r_surface.uv2 = _as_vec2(p_arrays[Mesh::ARRAY_TEX_UV2]);
	r_surface.colors = _as_colors(p_arrays[Mesh::ARRAY_COLOR]);
	r_surface.indices = _as_indices(p_arrays[Mesh::ARRAY_INDEX]);

	const int count = r_surface.positions.size();
	if (r_surface.normals.size() != count) {
		r_surface.normals.clear();
	}
	if (r_surface.tangents.size() != count * 4) {
		r_surface.tangents.clear();
	}
	if (r_surface.uv0.size() != count) {
		r_surface.uv0.clear();
	}
	if (r_surface.uv2.size() != count) {
		r_surface.uv2.clear();
	}
	if (r_surface.colors.size() != count) {
		r_surface.colors.clear();
	}
}

Error nextgeo_import_build(const ImporterMesh *p_mesh, const PackedByteArray &p_eligible, Ref<VirtualGeometryData> &r_vgd, Vector<Vector<uint8_t>> &r_pages) {
	r_vgd.unref();
	r_pages.clear();
	ERR_FAIL_NULL_V(p_mesh, ERR_INVALID_PARAMETER);

	NextGeoBuildInput input;
	input.page_size = NGM_PROVISIONAL_PAGE_SIZE;
	input.use_threads = true;

	bool any_eligible = false;
	for (int i = 0; i < p_mesh->surfaces.size(); i++) {
		const ImporterMesh::Surface &src = p_mesh->surfaces[i];
		const bool want_eligible = i < p_eligible.size() && p_eligible[i] == 1;
		NextGeoSurfaceInput surface;
		// Non-triangles and broken index lists stay in the surface list so eligibility
		// lines up with the ImporterMesh, but they are empty so the builder does not
		// reject the whole mesh. Ineligible triangles are passed through so borders lock.
		if (src.primitive == Mesh::PRIMITIVE_TRIANGLES && _arrays_are_triangles(src.arrays)) {
			_copy_triangle_surface(src.arrays, surface);
			surface.eligible = want_eligible;
			if (want_eligible) {
				any_eligible = true;
			}
		}
		input.surfaces.push_back(surface);
	}
	if (!any_eligible) {
		return ERR_INVALID_DATA;
	}

	NextGeoBuildOutput output;
	Error err = nextgeo_build_dag(input, output);
	if (err != OK) {
		return err;
	}
	err = nextgeo_write_pages(output, r_pages);
	if (err != OK) {
		r_pages.clear();
		return err;
	}

	r_vgd.instantiate();
	err = nextgeo_fill_vgd(input, output, r_pages, r_vgd);
	if (err != OK) {
		r_vgd.unref();
		r_pages.clear();
		return err;
	}

	// Transient write table. Not a saved property: NGMWriter needs the vertex layout and
	// each page's group range, and the hook's signature only returns payloads.
	PackedInt32Array groups;
	groups.resize(output.pages.size() * 2);
	for (uint32_t i = 0; i < output.pages.size(); i++) {
		groups.write[i * 2] = int32_t(output.pages[i].group_offset);
		groups.write[i * 2 + 1] = int32_t(output.pages[i].group_count);
	}
	r_vgd->set_ngm_page_table(output.vertex_layout, groups);
	return OK;
}
