/**************************************************************************/
/*  test_editor_help_shutdown.cpp                                         */
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

TEST_FORCE_LINK(test_editor_help_shutdown)

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/templates/safe_refcount.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/project_manager/project_manager.h"
#include "tests/test_tools.h"
#include "tests/test_utils.h"

// EditorHelp private state and deferred helpers are reached through this friend.
class EditorHelpTestAccess {
public:
	static void set_doc_for_test() {
		EditorHelp::doc_cleaned.clear();
		if (!EditorHelp::doc) {
			EditorHelp::doc = memnew(DocTools);
		}
	}

	static void set_script_docs_loaded_for_test(bool p_loaded) {
		EditorHelp::_script_docs_loaded.set_to(p_loaded);
	}

	static DocTools *get_raw_doc() {
		return EditorHelp::doc;
	}

	static bool is_doc_cleaned() {
		return EditorHelp::doc_cleaned.is_set();
	}

	static bool is_worker_thread_started() {
		return EditorHelp::worker_thread.is_started();
	}

	static bool is_loader_thread_started() {
		return EditorHelp::loader_thread.is_started();
	}

	static void queue_gen_extensions_docs() {
		callable_mp_static(&EditorHelp::_gen_extensions_docs).call_deferred();
	}

	static void queue_regenerate_doc_deferred(bool p_use_script_cache) {
		callable_mp_static(&EditorHelp::_regenerate_doc_deferred).call_deferred(p_use_script_cache);
	}

	static void call_regenerate_doc_deferred(bool p_use_script_cache) {
		EditorHelp::_regenerate_doc_deferred(p_use_script_cache);
	}

	static void call_reload_scripts_documentation(EditorFileSystemDirectory *p_dir) {
		EditorHelp::_reload_scripts_documentation(p_dir);
	}

	static void start_loader_thread(Thread::Callback p_callback, void *p_user) {
		EditorHelp::loader_thread.start(p_callback, p_user);
	}
};

namespace TestEditorHelpShutdown {

struct ScopedProjectLoaded {
	bool prev = false;

	explicit ScopedProjectLoaded(bool p_loaded) {
		prev = TestProjectSettingsInternalsAccessor::project_loaded();
		TestProjectSettingsInternalsAccessor::project_loaded() = p_loaded;
	}

	~ScopedProjectLoaded() {
		TestProjectSettingsInternalsAccessor::project_loaded() = prev;
	}
};

static void _dummy_loader_thread(void *p_userdata) {
	SafeFlag *ran = static_cast<SafeFlag *>(p_userdata);
	ran->set();
}

TEST_CASE("[EditorHelp] Deferred callbacks flushed after cleanup_doc are safe no-ops") {
	REQUIRE(EditorNode::get_singleton() == nullptr);
	REQUIRE(ProjectManager::get_singleton() == nullptr);
	REQUIRE(EditorFileSystem::get_singleton() == nullptr);
	REQUIRE(ProjectSettings::get_singleton() != nullptr);

	// Mirror Main::cleanup() flushing MessageQueue while ProjectSettings still reports a loaded project.
	ScopedProjectLoaded loaded_guard(true);
	REQUIRE(ProjectSettings::get_singleton()->is_project_loaded());

	EditorHelpTestAccess::set_doc_for_test();
	REQUIRE(EditorHelpTestAccess::get_raw_doc() != nullptr);

	// Queue every static deferred callback a background doc thread can post before EditorNode teardown.
	callable_mp_static(&EditorHelp::load_script_doc_cache).call_deferred();
	EditorHelpTestAccess::queue_gen_extensions_docs();
	EditorHelpTestAccess::queue_regenerate_doc_deferred(true);
	callable_mp_static(&EditorHelp::regenerate_script_doc_cache).call_deferred();

	// Mirror EditorNode::~EditorNode() calling EditorHelp::cleanup_doc() before Main::cleanup() flushes MessageQueue.
	EditorHelp::cleanup_doc();
	CHECK(EditorHelpTestAccess::is_doc_cleaned());
	CHECK(EditorHelpTestAccess::get_raw_doc() == nullptr);

	ErrorDetector ed;
	MessageQueue::get_singleton()->flush();

	CHECK_FALSE(ed.has_error);
	CHECK(EditorHelpTestAccess::get_raw_doc() == nullptr);
	CHECK_FALSE(EditorHelpTestAccess::is_worker_thread_started());
	CHECK_FALSE(EditorHelpTestAccess::is_loader_thread_started());
}

TEST_CASE("[EditorHelp] Direct shutdown-race calls without EditorNode do not error or spawn threads") {
	REQUIRE(EditorNode::get_singleton() == nullptr);
	REQUIRE(ProjectManager::get_singleton() == nullptr);
	REQUIRE(EditorFileSystem::get_singleton() == nullptr);
	REQUIRE(ProjectSettings::get_singleton() != nullptr);

	ScopedProjectLoaded loaded_guard(true);
	REQUIRE(ProjectSettings::get_singleton()->is_project_loaded());

	EditorHelpTestAccess::set_doc_for_test();

	ErrorDetector ed;
	EditorHelp::load_script_doc_cache();
	EditorHelp::regenerate_script_doc_cache();
	EditorHelpTestAccess::call_regenerate_doc_deferred(true);
	EditorHelpTestAccess::call_reload_scripts_documentation(nullptr);
	EditorHelp::save_script_doc_cache();

	CHECK_FALSE(ed.has_error);
	CHECK_FALSE(EditorHelpTestAccess::is_worker_thread_started());
	CHECK_FALSE(EditorHelpTestAccess::is_loader_thread_started());

	EditorHelp::cleanup_doc();
	CHECK(EditorHelpTestAccess::get_raw_doc() == nullptr);

	// Exercise save_script_doc_cache()'s `!doc` guard when `_script_docs_loaded` is set.
	EditorHelpTestAccess::set_script_docs_loaded_for_test(true);
	EditorHelp::save_script_doc_cache();
	EditorHelpTestAccess::set_script_docs_loaded_for_test(false);
	CHECK_FALSE(ed.has_error);
}

TEST_CASE("[EditorHelp] cleanup_doc joins an active loader_thread") {
	EditorHelpTestAccess::set_doc_for_test();

	SafeFlag ran(false);
	EditorHelpTestAccess::start_loader_thread(_dummy_loader_thread, &ran);
	REQUIRE(EditorHelpTestAccess::is_loader_thread_started());

	EditorHelp::cleanup_doc();

	CHECK(ran.is_set());
	CHECK_FALSE(EditorHelpTestAccess::is_loader_thread_started());
	CHECK_FALSE(EditorHelpTestAccess::is_worker_thread_started());
	CHECK(EditorHelpTestAccess::get_raw_doc() == nullptr);
}

TEST_CASE("[EditorFileSystem] Destructor clears singleton pointer") {
	REQUIRE(EditorFileSystem::get_singleton() == nullptr);

	ResourceLoaderImport prev_import = ResourceLoader::import;
	ResourceFormatImporterLoadOnStartup prev_load_on_startup = ResourceImporter::load_on_startup;

	EditorFileSystem *efs = memnew(EditorFileSystem);
	CHECK(EditorFileSystem::get_singleton() == efs);

	memdelete(efs);
	CHECK(EditorFileSystem::get_singleton() == nullptr);

	ResourceLoader::import = prev_import;
	ResourceImporter::load_on_startup = prev_load_on_startup;
}

} // namespace TestEditorHelpShutdown

#endif // TOOLS_ENABLED
