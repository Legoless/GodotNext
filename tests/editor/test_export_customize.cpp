/**************************************************************************/
/*  test_export_customize.cpp                                             */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_export_customize)

#if defined(TOOLS_ENABLED) && !defined(_3D_DISABLED)

#include "editor/export/editor_export_platform_extension.h"
#include "editor/export/editor_export_plugin.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "tests/test_tools.h"

// _export_customize_object() is private. The test reaches it through this friend.
class ExportCustomizeTestAccess {
public:
	static bool customize_object(EditorExportPlatform *p_platform, Object *p_object, LocalVector<Ref<EditorExportPlugin>> &p_plugins) {
		return p_platform->_export_customize_object(p_object, p_plugins);
	}
};

namespace TestExportCustomize {

// Records every resource export customization offers it, and optionally replaces one.
class RecordingExportPlugin : public EditorExportPlugin {
public:
	Vector<Ref<Resource>> seen;
	Ref<Resource> replace_from;
	Ref<Resource> replace_to;

	virtual Ref<Resource> _customize_resource(const Ref<Resource> &p_resource, const String &p_path) override {
		seen.push_back(p_resource);
		if (replace_from.is_valid() && p_resource == replace_from) {
			return replace_to;
		}
		return Ref<Resource>();
	}
};

TEST_CASE("[SceneTree][ExportCustomize] Resource customization skips disabled GPUParticles3D draw passes") {
	Ref<EditorExportPlatform> platform = memnew(EditorExportPlatformExtension);
	Ref<RecordingExportPlugin> plugin = memnew(RecordingExportPlugin);
	LocalVector<Ref<EditorExportPlugin>> plugins;
	plugins.push_back(plugin);

	GPUParticles3D *particles = memnew(GPUParticles3D);
	REQUIRE(particles->get_draw_passes() == 1);
	Ref<QuadMesh> pass_mesh;
	pass_mesh.instantiate();
	particles->set_draw_pass_mesh(0, pass_mesh);

	SUBCASE("Only the active draw pass is read") {
		// draw_pass_2..4 stay in the property list with PROPERTY_USAGE_NONE; reading them is an error.
		ErrorDetector ed;
		CHECK_FALSE(ExportCustomizeTestAccess::customize_object(platform.ptr(), particles, plugins));
		CHECK_FALSE(ed.has_error);
		CHECK(plugin->seen.has(pass_mesh));
	}

	SUBCASE("Every active draw pass is read") {
		particles->set_draw_passes(2);
		Ref<BoxMesh> second_mesh;
		second_mesh.instantiate();
		particles->set_draw_pass_mesh(1, second_mesh);

		ErrorDetector ed;
		CHECK_FALSE(ExportCustomizeTestAccess::customize_object(platform.ptr(), particles, plugins));
		CHECK_FALSE(ed.has_error);
		CHECK(plugin->seen.has(pass_mesh));
		CHECK(plugin->seen.has(second_mesh));
	}

	SUBCASE("An active draw pass can still be replaced") {
		Ref<BoxMesh> replacement;
		replacement.instantiate();
		plugin->replace_from = pass_mesh;
		plugin->replace_to = replacement;

		ErrorDetector ed;
		CHECK(ExportCustomizeTestAccess::customize_object(platform.ptr(), particles, plugins));
		CHECK_FALSE(ed.has_error);
		CHECK(particles->get_draw_pass_mesh(0) == replacement);
	}

	memdelete(particles);
}

} // namespace TestExportCustomize

#endif // TOOLS_ENABLED && !_3D_DISABLED
