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
#include "nextgeo_rt_proxy.h"
#include "nextgeo_serialize.h"

#include "core/config/project_settings.h"
#include "core/io/resource_format_ngm.h"
#include "core/math/aabb.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/3d/virtual_geometry_wind.h"
#include "scene/resources/mesh.h"

// Stage-2 card descriptors and the fixed RT proxy are filled below.

// Bounds of every source surface, ineligible and non-triangle surfaces included.
// eps_rt uses this AABB, so a large legacy surface still bounds the proxy error.
static bool _mesh_aabb(const ImporterMesh *p_mesh, AABB &r_aabb) {
	bool has_point = false;
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		const Array arrays = p_mesh->get_surface_arrays(i);
		if (arrays.size() != Mesh::ARRAY_MAX || arrays[Mesh::ARRAY_VERTEX].get_type() == Variant::NIL) {
			continue;
		}
		const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		for (int v = 0; v < vertices.size(); v++) {
			if (!has_point) {
				r_aabb = AABB(vertices[v], Vector3());
				has_point = true;
			} else {
				r_aabb.expand_to(vertices[v]);
			}
		}
	}
	return has_point;
}

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

static void _copy_triangle_surface(const Array &p_arrays, uint64_t p_flags, NextGeoSurfaceInput &r_surface) {
	r_surface.positions = _as_vec3(p_arrays[Mesh::ARRAY_VERTEX]);
	r_surface.normals = _as_vec3(p_arrays[Mesh::ARRAY_NORMAL]);
	r_surface.tangents = _as_floats(p_arrays[Mesh::ARRAY_TANGENT]);
	r_surface.uv0 = _as_vec2(p_arrays[Mesh::ARRAY_TEX_UV]);
	r_surface.uv2 = _as_vec2(p_arrays[Mesh::ARRAY_TEX_UV2]);
	r_surface.colors = _as_colors(p_arrays[Mesh::ARRAY_COLOR]);
	// Skinned surfaces (issue #159): both the 4-weight layout and
	// ARRAY_FLAG_USE_8_BONE_WEIGHTS. The builder validates the array sizes.
	r_surface.bones = _as_indices(p_arrays[Mesh::ARRAY_BONES]);
	r_surface.weights = _as_floats(p_arrays[Mesh::ARRAY_WEIGHTS]);
	if (!r_surface.bones.is_empty() || !r_surface.weights.is_empty()) {
		r_surface.skin_influences = (p_flags & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS) != 0 ? 8 : 4;
	}
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

Error nextgeo_import_build(const ImporterMesh *p_mesh, const PackedByteArray &p_eligible, bool p_preserve_area, bool p_wind, const PackedVector3Array &p_wind_bone_rest_positions, const PackedInt32Array &p_wind_bone_parents, Ref<VirtualGeometryData> &r_vgd, Vector<Vector<uint8_t>> &r_pages) {
	r_vgd.unref();
	r_pages.clear();
	ERR_FAIL_NULL_V(p_mesh, ERR_INVALID_PARAMETER);

	NextGeoBuildInput input;
	input.page_size = NGM_PROVISIONAL_PAGE_SIZE;
	input.use_threads = true;
	input.preserve_area = p_preserve_area;

	bool any_eligible = false;
	for (int i = 0; i < p_mesh->surfaces.size(); i++) {
		const ImporterMesh::Surface &src = p_mesh->surfaces[i];
		const bool want_eligible = i < p_eligible.size() && p_eligible[i] == 1;
		NextGeoSurfaceInput surface;
		// Non-triangles and broken index lists stay in the surface list so eligibility
		// lines up with the ImporterMesh, but they are empty so the builder does not
		// reject the whole mesh. Ineligible triangles are passed through so borders lock.
		if (src.primitive == Mesh::PRIMITIVE_TRIANGLES && _arrays_are_triangles(src.arrays)) {
			_copy_triangle_surface(src.arrays, src.flags, surface);
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

	// GodotNext: nextgeo_foliage_wind (#152). Generate procedural wind skeleton (1 trunk + <= 8 branches)
	// and write #159 4-influence skinning data so `.ngm` clusters carry vertex wind bone weights.
	Ref<VirtualGeometryWind> wind_resource;
	if (p_wind) {
		PackedVector3Array all_positions;
		PackedInt32Array all_src_bones;
		PackedFloat32Array all_src_weights;
		int src_inf = 0;
		bool all_have_src_skin = true;
		for (int i = 0; i < input.surfaces.size(); i++) {
			const NextGeoSurfaceInput &s = input.surfaces[i];
			if (!s.eligible || s.positions.is_empty()) {
				continue;
			}
			const int vcount = s.positions.size();
			const int prev_size = all_positions.size();
			all_positions.resize(prev_size + vcount);
			for (int v = 0; v < vcount; v++) {
				all_positions.write[prev_size + v] = s.positions[v];
			}
			if (s.skin_influences != 4u && s.skin_influences != 8u) {
				all_have_src_skin = false;
			} else if (src_inf == 0) {
				src_inf = int(s.skin_influences);
			} else if (uint32_t(src_inf) != s.skin_influences) {
				all_have_src_skin = false;
			}
		}
		if (!all_positions.is_empty()) {
			wind_resource.instantiate();
			PackedInt32Array remap_table;
			bool used_skeleton = false;
			if (all_have_src_skin && src_inf > 0 && !p_wind_bone_rest_positions.is_empty() && p_wind_bone_parents.size() == p_wind_bone_rest_positions.size()) {
				for (int i = 0; i < input.surfaces.size(); i++) {
					const NextGeoSurfaceInput &s = input.surfaces[i];
					if (!s.eligible || s.positions.is_empty()) {
						continue;
					}
					const int prev_n = all_src_bones.size();
					const int add_n = s.positions.size() * src_inf;
					if (s.bones.size() == add_n && s.weights.size() == add_n) {
						all_src_bones.resize(prev_n + add_n);
						all_src_weights.resize(prev_n + add_n);
						for (int k = 0; k < add_n; k++) {
							all_src_bones.write[prev_n + k] = s.bones[k];
							all_src_weights.write[prev_n + k] = s.weights[k];
						}
					} else {
						all_have_src_skin = false;
						break;
					}
				}
				if (all_have_src_skin && wind_resource->generate_from_skeleton(p_wind_bone_rest_positions, p_wind_bone_parents, all_positions, all_src_bones, all_src_weights, src_inf, remap_table) == OK) {
					used_skeleton = true;
				}
			}
			if (!used_skeleton) {
				const Error wind_err = wind_resource->generate_from_geometry(all_positions);
				if (wind_err != OK) {
					return wind_err;
				}
			}
			// Populate #159 4-influence skinning weights on every surface with vertices (including ineligible
			// border-lock surfaces so nextgeo_build_dag sees a consistent skin_influences across surfaces).
			const int wind_bones = wind_resource->get_bone_count();
			for (int i = 0; i < input.surfaces.size(); i++) {
				NextGeoSurfaceInput &s = input.surfaces.write[i];
				if (s.positions.is_empty()) {
					continue;
				}
				PackedInt32Array dst_bones;
				PackedFloat32Array dst_weights;
				if (used_skeleton && (s.skin_influences == 4u || s.skin_influences == 8u) && s.bones.size() == s.positions.size() * int(s.skin_influences) && s.weights.size() == s.positions.size() * int(s.skin_influences)) {
					PackedInt32Array src_b;
					PackedFloat32Array src_w;
					src_b.resize(s.bones.size());
					src_w.resize(s.weights.size());
					for (int k = 0; k < s.bones.size(); k++) {
						src_b.write[k] = s.bones[k];
						src_w.write[k] = s.weights[k];
					}
					VirtualGeometryWind::remap_vertex_skinning(src_b, src_w, s.skin_influences, remap_table, wind_bones, dst_bones, dst_weights);
				} else {
					PackedVector3Array surf_pos;
					surf_pos.resize(s.positions.size());
					for (int v = 0; v < s.positions.size(); v++) {
						surf_pos.write[v] = s.positions[v];
					}
					wind_resource->compute_vertex_skinning_arrays(surf_pos, dst_bones, dst_weights);
				}
				s.bones.resize(dst_bones.size());
				s.weights.resize(dst_weights.size());
				for (int k = 0; k < dst_bones.size(); k++) {
					s.bones.write[k] = dst_bones[k];
					s.weights.write[k] = dst_weights[k];
				}
				s.skin_influences = VirtualGeometryWind::SKIN_INFLUENCES;
			}
		}
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
	if (wind_resource.is_valid()) {
		r_vgd->set_wind(wind_resource);
	}

	// Fixed RT proxy (plan §7.1): the error-only DAG cut at
	// eps_rt = min(max_extent / 256, rt_max_error). A changed project setting
	// applies on the next import. Ineligible surfaces get [0, 0] ranges.
	AABB mesh_aabb;
	if (!_mesh_aabb(p_mesh, mesh_aabb)) {
		r_vgd.unref();
		r_pages.clear();
		return ERR_INVALID_DATA;
	}
	const float rt_max_error = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_max_error"));
	NextGeoRTProxy proxy;
	err = nextgeo_build_rt_proxy(output, mesh_aabb, rt_max_error, uint32_t(p_mesh->get_surface_count()), proxy);
	if (err != OK) {
		r_vgd.unref();
		r_pages.clear();
		return err;
	}
	err = nextgeo_fill_vgd_rt_proxy(proxy, r_vgd);
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
