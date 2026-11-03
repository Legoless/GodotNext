/**************************************************************************/
/*  nextgeo_test_bridge.cpp                                               */
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

#include "nextgeo_test_bridge.h"

#include "core/object/class_db.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/lightmap_gi.h"

void NextGeoTestBridge::_bind_methods() {
	ClassDB::bind_static_method("NextGeoTestBridge", D_METHOD("bake_lightmap", "lightmap", "from_node", "image_data_path"), &NextGeoTestBridge::bake_lightmap);
}

int NextGeoTestBridge::bake_lightmap(Object *p_lightmap, Node *p_from_node, const String &p_image_data_path) {
	LightmapGI *lightmap = Object::cast_to<LightmapGI>(p_lightmap);
	if (lightmap == nullptr || p_from_node == nullptr) {
		return LightmapGI::BAKE_ERROR_NO_SCENE_ROOT;
	}
	// LightmapperRD::bake always reads EditorSettings (the OIDN executable path). The OIDN
	// branch then uses EditorPaths with no null check. A --script run creates neither
	// singleton, and entering the bake from there crashes. Refuse before any dereference.
	if (EditorSettings::get_singleton() == nullptr || EditorPaths::get_singleton() == nullptr) {
		ERR_PRINT("NextGeoTestBridge.bake_lightmap needs EditorSettings and EditorPaths. A --script run does not create them, so the bake was not started.");
		return LightmapGI::BAKE_ERROR_NO_LIGHTMAPPER;
	}
	return lightmap->bake(p_from_node, p_image_data_path);
}
