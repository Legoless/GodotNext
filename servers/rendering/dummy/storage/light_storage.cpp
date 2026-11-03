/**************************************************************************/
/*  light_storage.cpp                                                     */
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

#include "light_storage.h"

using namespace RendererDummy;

LightStorage *LightStorage::singleton = nullptr;

LightStorage *LightStorage::get_singleton() {
	return singleton;
}

LightStorage::LightStorage() {
	singleton = this;
}

LightStorage::~LightStorage() {
	singleton = nullptr;
}

bool LightStorage::free(RID p_rid) {
	if (owns_lightmap(p_rid)) {
		lightmap_free(p_rid);
		return true;
	} else if (owns_lightmap_instance(p_rid)) {
		lightmap_instance_free(p_rid);
		return true;
	}

	return false;
}

/* LIGHTMAP API */

RID LightStorage::lightmap_allocate() {
	return lightmap_owner.allocate_rid();
}

void LightStorage::lightmap_initialize(RID p_lightmap) {
	lightmap_owner.initialize_rid(p_lightmap, Lightmap());
}

void LightStorage::lightmap_free(RID p_rid) {
	lightmap_set_textures(p_rid, RID(), false);
	lightmap_owner.free(p_rid);
}

// GodotNext: dummy_lightmap_probe_data
// The dummy renderer stores no GPU state, but LightmapGIData reads its probe
// data back from the rendering server when it is saved. Headless runs (exports
// and other tools) use this renderer, so it keeps the data the resource set.

void LightStorage::lightmap_set_probe_bounds(RID p_lightmap, const AABB &p_bounds) {
	Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL(lightmap);
	lightmap->bounds = p_bounds;
}

void LightStorage::lightmap_set_probe_interior(RID p_lightmap, bool p_interior) {
	Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL(lightmap);
	lightmap->interior = p_interior;
}

void LightStorage::lightmap_set_probe_capture_data(RID p_lightmap, const PackedVector3Array &p_points, const PackedColorArray &p_point_sh, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree) {
	Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL(lightmap);

	if (p_points.size()) {
		ERR_FAIL_COND(p_points.size() * 9 != p_point_sh.size());
		ERR_FAIL_COND((p_tetrahedra.size() % 4) != 0);
		ERR_FAIL_COND((p_bsp_tree.size() % 6) != 0);
	}

	lightmap->points = p_points;
	lightmap->point_sh = p_point_sh;
	lightmap->tetrahedra = p_tetrahedra;
	lightmap->bsp_tree = p_bsp_tree;
}

PackedVector3Array LightStorage::lightmap_get_probe_capture_points(RID p_lightmap) const {
	const Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL_V(lightmap, PackedVector3Array());
	return lightmap->points;
}

PackedColorArray LightStorage::lightmap_get_probe_capture_sh(RID p_lightmap) const {
	const Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL_V(lightmap, PackedColorArray());
	return lightmap->point_sh;
}

PackedInt32Array LightStorage::lightmap_get_probe_capture_tetrahedra(RID p_lightmap) const {
	const Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL_V(lightmap, PackedInt32Array());
	return lightmap->tetrahedra;
}

PackedInt32Array LightStorage::lightmap_get_probe_capture_bsp_tree(RID p_lightmap) const {
	const Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL_V(lightmap, PackedInt32Array());
	return lightmap->bsp_tree;
}

AABB LightStorage::lightmap_get_aabb(RID p_lightmap) const {
	const Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL_V(lightmap, AABB());
	return lightmap->bounds;
}

bool LightStorage::lightmap_is_interior(RID p_lightmap) const {
	const Lightmap *lightmap = lightmap_owner.get_or_null(p_lightmap);
	ERR_FAIL_NULL_V(lightmap, false);
	return lightmap->interior;
}

/* LIGHTMAP INSTANCE */

RID LightStorage::lightmap_instance_create(RID p_lightmap) {
	LightmapInstance li;
	li.lightmap = p_lightmap;
	return lightmap_instance_owner.make_rid(li);
}

void LightStorage::lightmap_instance_free(RID p_lightmap) {
	lightmap_instance_owner.free(p_lightmap);
}
