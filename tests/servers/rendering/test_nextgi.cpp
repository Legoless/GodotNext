/**************************************************************************/
/*  test_nextgi.cpp                                                       */
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

TEST_FORCE_LINK(test_nextgi)

// NextGI doctests live in this file. Append new cases here.
// Editor-only cases stay inside TOOLS_ENABLED so later issues can add runtime cases below.

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/string/translation_server.h"
#include "editor/editor_node.h"
#include "editor/scene/3d/nextgi_editor_plugin.h"
#include "tests/test_utils.h"

namespace TestNextGI {

static void restore_setting(const String &p_name, bool p_existed, const Variant &p_previous) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (p_existed) {
		ps->set_setting(p_name, p_previous);
	} else if (ps->has_setting(p_name)) {
		ps->clear(p_name);
	}
}

TEST_CASE("[NextGI][Editor] New projects map driver.windows to vulkan") {
	const HashMap<String, Variant> settings = EditorNode::get_initial_settings();
	const Variant *driver = settings.getptr("rendering/rendering_device/driver.windows");
	REQUIRE(driver != nullptr);
	CHECK_EQ(String(*driver), "vulkan");
}

TEST_CASE("[NextGI][Editor] Vulkan switch offer truth table") {
	CHECK(NextGIEditorPlugin::should_offer_vulkan_switch(false, true, "d3d12", false));

	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(false, true, "d3d12", true));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(false, true, "vulkan", false));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(true, true, "d3d12", false));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(false, false, "d3d12", false));
}

TEST_CASE("[NextGI][Editor] apply_vulkan_switch writes vulkan and keeps the D3D12 fallback") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String driver_setting = "rendering/rendering_device/driver.windows";
	const String fallback_setting = "rendering/rendering_device/fallback_to_d3d12";
	const String features_setting = "application/config/features";

	const bool had_driver = ps->has_setting(driver_setting);
	const bool had_fallback = ps->has_setting(fallback_setting);
	const bool had_features = ps->has_setting(features_setting);
	const Variant previous_driver = ps->get_setting(driver_setting);
	const Variant previous_fallback = ps->get_setting(fallback_setting);
	const Variant previous_features = ps->get_setting(features_setting);
	String &resource_path = TestProjectSettingsInternalsAccessor::resource_path();
	const String previous_resource_path = resource_path;

	const String temp_path = TestUtils::get_temp_path("nextgi_vulkan_switch");
	const Error mkdir_error = DirAccess::make_dir_recursive_absolute(temp_path);
	REQUIRE_EQ(mkdir_error, Error::OK);

	resource_path = temp_path;
	ps->set_setting(driver_setting, "d3d12");
	ps->set_setting(fallback_setting, false);

	NextGIEditorPlugin::apply_vulkan_switch();

	CHECK_EQ(String(ps->get_setting(driver_setting)), "vulkan");
	CHECK_EQ(bool(ps->get_setting(fallback_setting)), true);
	CHECK(FileAccess::exists(temp_path.path_join("project.godot")));

	restore_setting(driver_setting, had_driver, previous_driver);
	restore_setting(fallback_setting, had_fallback, previous_fallback);
	restore_setting(features_setting, had_features, previous_features);
	resource_path = previous_resource_path;
	DirAccess::remove_absolute(temp_path.path_join("project.godot"));
}

TEST_CASE("[NextGI][Editor] Vulkan switch dialog names the HDR and DComp cost") {
	TranslationServer::get_singleton()->set_locale("en");

	const String title = NextGIEditorPlugin::get_vulkan_switch_dialog_title();
	const String text = NextGIEditorPlugin::get_vulkan_switch_dialog_text();

	CHECK_EQ(title, "Use Vulkan on Windows for hardware RT");
	CHECK(text.contains("SDFGI"));
	CHECK(text.contains("Direct3D 12 fallback"));
	CHECK(text.contains("HDR output"));
	CHECK(text.contains("DComp transparency"));
	CHECK(text.contains("restart"));
}

} // namespace TestNextGI

#endif // TOOLS_ENABLED
