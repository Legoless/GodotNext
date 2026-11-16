/**************************************************************************/
/*  test_frame_generation.cpp                                             */
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

TEST_FORCE_LINK(test_frame_generation)

// GodotNext: pu_fg_present_path (#203), GU+-1 subset (plan §21.5.3): the reasons, a single active
// frame-generation provider, the main window only, and the vendor-neutral FrameGenerationExtension
// seam. No case needs a GPU: the test run has no RenderingDevice, so the split itself is checked in
// rendered runs (the testbed's frame_generation scene).

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "core/object/script_instance.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/frame_generation_extension.h"

namespace TestFrameGeneration {

using Reason = FrameGenerationExtension::FrameGenerationReason;
using Context = FrameGenerationRegistry::Context;

const char *PROVIDER_SETTING = "rendering/frame_generation/provider";
const char *MULTIPLIER_SETTING = "rendering/frame_generation/multiplier";
const char *DYNAMIC_SETTING = "rendering/frame_generation/dynamic";

// Counts the errors and warnings printed while it lives, and the ones that name a token.
struct LogCounter {
	ErrorHandlerList handler;
	int errors = 0;
	int warnings = 0;
	LocalVector<String> messages;

	static void _count(void *p_self, const char *p_func, const char *p_file, int p_line, const char *p_error, const char *p_errorexp, bool p_editor_notify, ErrorHandlerType p_type) {
		LogCounter *self = static_cast<LogCounter *>(p_self);
		if (p_type == ERR_HANDLER_WARNING) {
			self->warnings++;
		} else {
			self->errors++;
		}
		self->messages.push_back(String::utf8(p_error) + " " + String::utf8(p_errorexp));
	}

	int lines_naming(const String &p_token) const {
		int count = 0;
		for (const String &message : messages) {
			if (message.contains(p_token)) {
				count++;
			}
		}
		return count;
	}

	LogCounter() {
		handler.errfunc = _count;
		handler.userdata = this;
		add_error_handler(&handler);
	}
	~LogCounter() { remove_error_handler(&handler); }
};

struct MockProviderState {
	String name = "TestPassthrough";
	int reason = FrameGenerationExtension::REASON_OK;
	int available_calls = 0;
	int mode_calls = 0;
	int inputs_calls = 0;
	int reset_calls = 0;
	int present_calls = 0;
	Dictionary last_mode;
	Ref<FrameGenerationInputs> last_inputs;
	Object *last_present_inputs = nullptr;
	LocalVector<String> events;
};

// Answers the FrameGenerationExtension virtuals the way a script or GDExtension would, so every call
// goes through the GDVIRTUAL path.
class MockProviderScriptInstance : public ScriptInstance {
	MockProviderState *state = nullptr;

public:
	explicit MockProviderScriptInstance(MockProviderState *p_state) :
			state(p_state) {}

	bool set(const StringName &p_name, const Variant &p_value) override { return false; }
	bool get(const StringName &p_name, Variant &r_ret) const override { return false; }
	void get_property_list(List<PropertyInfo> *r_properties) const override {}
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const override {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return Variant::NIL;
	}
	void validate_property(PropertyInfo &p_property) const override {}
	bool property_can_revert(const StringName &p_name) const override { return false; }
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override { return false; }
	void get_method_list(List<MethodInfo> *r_list) const override {}
	bool has_method(const StringName &p_method) const override {
		return p_method == SNAME("_get_name") || p_method == SNAME("_is_available") || p_method == SNAME("_set_mode") ||
				p_method == SNAME("_on_frame_inputs") || p_method == SNAME("_on_present") || p_method == SNAME("_reset");
	}
	int get_method_argument_count(const StringName &p_method, bool *r_is_valid = nullptr) const override {
		if (r_is_valid) {
			*r_is_valid = has_method(p_method);
		}
		if (p_method == SNAME("_on_present")) {
			return 2;
		}
		return (p_method == SNAME("_set_mode") || p_method == SNAME("_on_frame_inputs")) ? 1 : 0;
	}
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override {
		r_error.error = Callable::CallError::CALL_OK;
		if (p_method == SNAME("_get_name")) {
			return state->name;
		}
		if (p_method == SNAME("_is_available")) {
			state->available_calls++;
			state->events.push_back("is_available");
			return state->reason;
		}
		if (p_method == SNAME("_set_mode") && p_argcount == 1) {
			state->mode_calls++;
			state->last_mode = *p_args[0];
			state->events.push_back("set_mode");
			return Variant();
		}
		if (p_method == SNAME("_on_frame_inputs") && p_argcount == 1) {
			state->inputs_calls++;
			state->last_inputs = *p_args[0];
			state->events.push_back("inputs");
			return Variant();
		}
		if (p_method == SNAME("_on_present") && p_argcount == 2) {
			state->present_calls++;
			state->last_present_inputs = *p_args[0];
			state->events.push_back(bool(*p_args[1]) ? "present_before" : "present_after");
			return Variant();
		}
		if (p_method == SNAME("_reset")) {
			state->reset_calls++;
			state->events.push_back("reset");
			return Variant();
		}
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	void notification(int p_notification, bool p_reversed = false) override {}
	Script *get_script() const override { return nullptr; }
	const Variant get_rpc_config() const override { return Variant(); }
	ScriptLanguage *get_language() override { return nullptr; }
};

// Test-only pass-through provider, answered through the GDVIRTUAL path like the testbed's
// passthrough_fg.gd.
class MockFrameGenerationExtension : public FrameGenerationExtension {
	GDSOFTCLASS(MockFrameGenerationExtension, FrameGenerationExtension);

public:
	MockProviderState state;

	explicit MockFrameGenerationExtension(const String &p_name = "TestPassthrough") {
		state.name = p_name;
		set_script_instance(memnew(MockProviderScriptInstance(&state)));
	}
};

// Restores the registry and the settings when a test ends.
struct FrameGenerationScope {
	Variant provider;
	Variant multiplier;
	Variant dynamic;

	FrameGenerationScope() {
		provider = GLOBAL_GET(PROVIDER_SETTING);
		multiplier = GLOBAL_GET(MULTIPLIER_SETTING);
		dynamic = GLOBAL_GET(DYNAMIC_SETTING);
		FrameGenerationRegistry::clear();
	}
	~FrameGenerationScope() {
		FrameGenerationRegistry::clear();
		ProjectSettings::get_singleton()->set_setting(PROVIDER_SETTING, provider);
		ProjectSettings::get_singleton()->set_setting(MULTIPLIER_SETTING, multiplier);
		ProjectSettings::get_singleton()->set_setting(DYNAMIC_SETTING, dynamic);
	}
};

// A running game's main window on a RenderingDevice renderer.
Context game_context(const String &p_provider, int p_multiplier = 2, bool p_dynamic = false) {
	Context context;
	context.provider = p_provider;
	context.multiplier = p_multiplier;
	context.dynamic = p_dynamic;
	context.editor = false;
	context.xr_main_window = false;
	context.rendering_device = true;
	return context;
}

Ref<MockFrameGenerationExtension> make_provider(const String &p_name = "TestPassthrough") {
	return Ref<MockFrameGenerationExtension>(memnew(MockFrameGenerationExtension(p_name)));
}

TEST_CASE("[FrameGeneration] Reasons are append-only, bound and named by their token") {
	CHECK_EQ(int(FrameGenerationExtension::REASON_OK), 0);
	CHECK_EQ(int(FrameGenerationExtension::REASON_API_UNSUPPORTED), 1);
	CHECK_EQ(int(FrameGenerationExtension::REASON_FG_GPU_UNSUPPORTED), 2);
	CHECK_EQ(int(FrameGenerationExtension::REASON_MFG_REQUIRES_RTX50), 3);
	CHECK_EQ(int(FrameGenerationExtension::REASON_NOT_NVIDIA_RTX), 4);
	CHECK_EQ(int(FrameGenerationExtension::REASON_NOT_MAIN_WINDOW), 5);
	CHECK_EQ(int(FrameGenerationExtension::REASON_PROVIDER_ALREADY_ACTIVE), 6);
	CHECK_EQ(int(FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE), 7);
	CHECK_EQ(int(FrameGenerationExtension::REASON_MAX), 8);

	const char *tokens[] = { "OK", "API_UNSUPPORTED", "FG_GPU_UNSUPPORTED", "MFG_REQUIRES_RTX50", "NOT_NVIDIA_RTX", "NOT_MAIN_WINDOW", "PROVIDER_ALREADY_ACTIVE", "PROVIDER_UNAVAILABLE" };
	REQUIRE(ClassDB::has_enum("FrameGenerationExtension", "FrameGenerationReason", true));
	for (int i = 0; i < FrameGenerationExtension::REASON_MAX; i++) {
		CHECK_EQ(FrameGenerationExtension::get_reason_token(Reason(i)), String(tokens[i]));
		// The status probe (#202) names the reason by its ClassDB constant.
		bool valid = false;
		CHECK_EQ(ClassDB::get_integer_constant("FrameGenerationExtension", StringName(String("REASON_") + tokens[i]), &valid), int64_t(i));
		CHECK_MESSAGE(valid, tokens[i]);
		CHECK_EQ(String(ClassDB::get_integer_constant_enum("FrameGenerationExtension", StringName(String("REASON_") + tokens[i]), true)), String("FrameGenerationReason"));
	}
	CHECK_EQ(FrameGenerationExtension::get_reason_token(FrameGenerationExtension::REASON_MAX), String("UNKNOWN"));
}

TEST_CASE("[FrameGeneration] Project settings: off by default, multiplier 2 to 6, dynamic off") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings->has_setting(PROVIDER_SETTING));
	REQUIRE(settings->has_setting(MULTIPLIER_SETTING));
	REQUIRE(settings->has_setting(DYNAMIC_SETTING));
	CHECK_EQ(String(settings->property_get_revert(PROVIDER_SETTING)), String());
	CHECK_EQ(int(settings->property_get_revert(MULTIPLIER_SETTING)), 2);
	CHECK_FALSE(bool(settings->property_get_revert(DYNAMIC_SETTING)));
	CHECK_EQ(FrameGenerationRegistry::MULTIPLIER_MIN, 2);
	CHECK_EQ(FrameGenerationRegistry::MULTIPLIER_MAX, 6);
	CHECK_EQ(FrameGenerationRegistry::clamp_multiplier(1), 2);
	CHECK_EQ(FrameGenerationRegistry::clamp_multiplier(4), 4);
	CHECK_EQ(FrameGenerationRegistry::clamp_multiplier(9), 6);
}

TEST_CASE("[FrameGeneration] Virtuals dispatch through the GDVIRTUAL path and fall back to defaults") {
	Ref<MockFrameGenerationExtension> ext = make_provider();
	CHECK_EQ(ext->get_provider_name(), "TestPassthrough");
	CHECK_EQ(ext->is_available(), FrameGenerationExtension::REASON_OK);
	ext->state.reason = FrameGenerationExtension::REASON_NOT_NVIDIA_RTX;
	CHECK_EQ(ext->is_available(), FrameGenerationExtension::REASON_NOT_NVIDIA_RTX);
	// A value outside the enum is PROVIDER_UNAVAILABLE.
	ext->state.reason = 99;
	CHECK_EQ(ext->is_available(), FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE);
	ext->state.reason = -1;
	CHECK_EQ(ext->is_available(), FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE);

	Dictionary mode;
	mode["multiplier"] = 3;
	ext->set_mode(mode);
	CHECK_EQ(int(ext->state.last_mode["multiplier"]), 3);
	Ref<FrameGenerationInputs> inputs;
	inputs.instantiate();
	ext->on_frame_inputs(inputs);
	CHECK(ext->state.last_inputs == inputs);
	ext->on_present(inputs.ptr(), true);
	ext->on_present(inputs.ptr(), false);
	CHECK(ext->state.last_present_inputs == inputs.ptr());
	ext->reset();
	CHECK_EQ(ext->state.mode_calls, 1);
	CHECK_EQ(ext->state.inputs_calls, 1);
	CHECK_EQ(ext->state.present_calls, 2);
	CHECK_EQ(ext->state.reset_calls, 1);

	// Without overrides: no name, PROVIDER_UNAVAILABLE, and the other calls do nothing.
	Ref<FrameGenerationExtension> plain;
	plain.instantiate();
	CHECK(plain->get_provider_name().is_empty());
	CHECK_EQ(plain->is_available(), FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE);
	plain->set_mode(mode);
	plain->on_frame_inputs(inputs);
	plain->on_present(inputs.ptr(), true);
	plain->reset();

	List<MethodInfo> virtuals;
	ClassDB::get_virtual_methods("FrameGenerationExtension", &virtuals, true);
	for (const char *method : { "_get_name", "_is_available", "_set_mode", "_on_frame_inputs", "_on_present", "_reset" }) {
		bool found = false;
		for (const MethodInfo &info : virtuals) {
			found = found || info.name == StringName(method);
		}
		CHECK_MESSAGE(found, method);
	}
}

TEST_CASE("[FrameGeneration] Inputs carry the HUD-less colour, the UI layer, depth, velocity and the camera") {
	for (const char *property : { "hudless_color", "ui_color", "depth", "velocity", "motion_vector_scale", "render_size", "output_size", "view_to_clip",
				 "clip_to_prev_clip", "camera_transform", "z_near", "z_far", "fovy", "aspect", "jitter", "reset", "frame_index", "swap_chain" }) {
		CHECK_MESSAGE(ClassDB::has_property("FrameGenerationInputs", property), property);
	}
	Ref<FrameGenerationInputs> inputs;
	inputs.instantiate();
	inputs->set_hudless_color(RID::from_uint64(21));
	inputs->set_ui_color(RID::from_uint64(22));
	inputs->set_render_size(Size2i(960, 540));
	inputs->set_output_size(Size2i(1920, 1080));
	inputs->set_reset(true);
	inputs->set_swap_chain(0x1234);
	CHECK(RID(inputs->get("hudless_color")) == RID::from_uint64(21));
	CHECK(RID(inputs->get("ui_color")) == RID::from_uint64(22));
	CHECK_EQ(Size2i(inputs->get("render_size")), Size2i(960, 540));
	CHECK_EQ(Size2i(inputs->get("output_size")), Size2i(1920, 1080));
	CHECK(bool(inputs->get("reset")));
	CHECK_EQ(int64_t(inputs->get("swap_chain")), int64_t(0x1234));

	// clip_to_prev_clip maps this frame's clip coordinates of a world point to the previous frame's.
	Projection projection;
	projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 4000.0);
	const Transform3D prev_camera(Basis(Vector3(0, 1, 0), 0.1), Vector3(0.0, 1.0, 5.0));
	const Transform3D camera(Basis(Vector3(0, 1, 0), 0.25), Vector3(0.5, 1.2, 4.0));
	// A camera that did not move: identity, up to float rounding of P * P^-1.
	const Projection still = FrameGenerationInputs::compute_clip_to_prev_clip(projection, camera, projection, camera);
	const Projection identity;
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			CHECK_MESSAGE(Math::is_equal_approx(still.columns[column][row], identity.columns[column][row], real_t(1e-4)), vformat("[%d][%d] = %f", column, row, still.columns[column][row]));
		}
	}
	const Projection moved = FrameGenerationInputs::compute_clip_to_prev_clip(projection, camera, projection, prev_camera);
	for (const Vector3 &world : { Vector3(0.0, 0.0, -3.0), Vector3(1.0, 2.0, -10.0), Vector3(-4.0, 0.5, -40.0) }) {
		const Vector3 view = camera.affine_inverse().xform(world);
		const Vector3 prev_view = prev_camera.affine_inverse().xform(world);
		const Vector4 clip = projection.xform(Vector4(view.x, view.y, view.z, 1.0));
		const Vector4 prev_clip = projection.xform(Vector4(prev_view.x, prev_view.y, prev_view.z, 1.0));
		const Vector4 mapped = moved.xform(clip);
		const Vector3 mapped_ndc = Vector3(mapped.x, mapped.y, mapped.z) / mapped.w;
		const Vector3 prev_ndc = Vector3(prev_clip.x, prev_clip.y, prev_clip.z) / prev_clip.w;
		CHECK_MESSAGE(mapped_ndc.is_equal_approx(prev_ndc), vformat("%s -> %s, expected %s", world, mapped_ndc, prev_ndc));
	}
}

TEST_CASE("[FrameGeneration] The documented depth correction gives the clip space of the textures") {
	// FrameGenerationInputs.view_to_clip and clip_to_prev_clip use Godot's projection convention (y up,
	// depth -1 at the near plane to 1 at the far plane). The class reference tells providers to apply
	// Projection.create_depth_correction(true), as the renderer does for a camera, to get the clip space
	// of the textures: y down, depth from 1 at the near plane to 0 at the far plane (reverse Z).
	const real_t z_near = 0.05;
	const real_t z_far = 4000.0;
	Projection projection;
	projection.set_perspective(70.0, 16.0 / 9.0, z_near, z_far);
	const Projection correction = Projection::create_depth_correction(true);
	const Projection corrected = correction * projection;
	const auto uv_depth = [&](const Projection &p_projection, const Vector3 &p_view) {
		const Vector4 clip = p_projection.xform(Vector4(p_view.x, p_view.y, p_view.z, 1.0));
		const Vector3 ndc = Vector3(clip.x, clip.y, clip.z) / clip.w;
		return Vector3(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5, ndc.z);
	};
	// The view center is the texture center; above it is a smaller v (y down); right is a larger u.
	CHECK(uv_depth(corrected, Vector3(0.0, 0.0, -10.0)).is_equal_approx(Vector3(0.5, 0.5, uv_depth(corrected, Vector3(0.0, 0.0, -10.0)).z)));
	CHECK(uv_depth(corrected, Vector3(0.0, 1.0, -10.0)).y < 0.5);
	CHECK(uv_depth(corrected, Vector3(1.0, 0.0, -10.0)).x > 0.5);
	// Reverse Z from 1 to 0, nearer is larger.
	CHECK(Math::is_equal_approx(uv_depth(corrected, Vector3(0.0, 0.0, -z_near)).z, real_t(1.0), real_t(1e-4)));
	CHECK(Math::is_equal_approx(uv_depth(corrected, Vector3(0.0, 0.0, -z_far)).z, real_t(0.0), real_t(1e-4)));
	CHECK(uv_depth(corrected, Vector3(0.0, 0.0, -2.0)).z > uv_depth(corrected, Vector3(0.0, 0.0, -20.0)).z);
	// Uncorrected, the same points are y up with depth -1 to 1.
	CHECK(uv_depth(projection, Vector3(0.0, 1.0, -10.0)).y > 0.5);
	CHECK(Math::is_equal_approx(uv_depth(projection, Vector3(0.0, 0.0, -z_near)).z, real_t(-1.0), real_t(1e-4)));

	// In the textures' clip space the reprojection is correction * clip_to_prev_clip * correction^-1.
	const Transform3D prev_camera(Basis(Vector3(0, 1, 0), 0.1), Vector3(0.0, 1.0, 5.0));
	const Transform3D camera(Basis(Vector3(0, 1, 0), 0.25), Vector3(0.5, 1.2, 4.0));
	const Projection reprojection = correction * FrameGenerationInputs::compute_clip_to_prev_clip(projection, camera, projection, prev_camera) * correction.inverse();
	for (const Vector3 &world : { Vector3(0.0, 0.0, -3.0), Vector3(1.0, 2.0, -10.0), Vector3(-4.0, 0.5, -40.0) }) {
		const Vector3 view = camera.affine_inverse().xform(world);
		const Vector3 prev_view = prev_camera.affine_inverse().xform(world);
		const Vector4 clip = corrected.xform(Vector4(view.x, view.y, view.z, 1.0));
		const Vector4 mapped = reprojection.xform(clip);
		const Vector3 mapped_uv_depth = Vector3(mapped.x / mapped.w * 0.5 + 0.5, mapped.y / mapped.w * 0.5 + 0.5, mapped.z / mapped.w);
		const Vector3 prev_uv_depth = uv_depth(corrected, prev_view);
		CHECK_MESSAGE(mapped_uv_depth.is_equal_approx(prev_uv_depth), vformat("%s -> %s, expected %s", world, mapped_uv_depth, prev_uv_depth));
	}
}

TEST_CASE("[FrameGeneration] Off: no provider, no fallback and no log line") {
	FrameGenerationScope scope;
	LogCounter log;
	FrameGenerationRegistry::update(game_context(""));
	CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_OK);
	CHECK_FALSE(FrameGenerationRegistry::is_active());
	CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 1);
	const Dictionary status = FrameGenerationRegistry::get_status();
	for (const char *key : { "provider", "requested", "active", "active_provider", "reason", "multiplier", "requested_multiplier", "dynamic", "present_hook" }) {
		CHECK_MESSAGE(status.has(key), key);
	}
	CHECK_EQ(String(status["provider"]), String());
	CHECK_FALSE(bool(status["requested"]));
	CHECK_FALSE(bool(status["active"]));
	CHECK_EQ(int(status["reason"]), 0);
	CHECK_EQ(int(status["multiplier"]), 1);
	CHECK_FALSE(bool(status["present_hook"]));

	// A registered provider stays unused while the setting is empty.
	Ref<MockFrameGenerationExtension> ext = make_provider();
	FrameGenerationRegistry::register_extension(ext);
	FrameGenerationRegistry::update(game_context(""));
	CHECK_FALSE(FrameGenerationRegistry::is_active());
	CHECK_EQ(ext->state.available_calls, 0);
	CHECK_EQ(ext->state.mode_calls, 0);
	CHECK_EQ(log.errors + log.warnings, 0);
}

TEST_CASE("[FrameGeneration] A provider that is not registered reports PROVIDER_UNAVAILABLE with one log line") {
	FrameGenerationScope scope;
	LogCounter log;
	ERR_PRINT_OFF;
	FrameGenerationRegistry::update(game_context("DLSS-G"));
	FrameGenerationRegistry::update(game_context("DLSS-G"));
	ERR_PRINT_ON;
	CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE);
	CHECK_FALSE(FrameGenerationRegistry::is_active());
	const Dictionary status = FrameGenerationRegistry::get_status();
	CHECK_EQ(String(status["provider"]), "DLSS-G");
	CHECK(bool(status["requested"]));
	CHECK_FALSE(bool(status["active"]));
	CHECK_EQ(String(status["active_provider"]), String());
	CHECK_EQ(int(status["reason"]), int(FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE));
	CHECK_EQ(int(status["multiplier"]), 1);
	CHECK_EQ(log.warnings, 1);
	CHECK_EQ(log.errors, 0);
	CHECK_EQ(log.lines_naming("PROVIDER_UNAVAILABLE"), 1);
}

TEST_CASE("[FrameGeneration] A registered provider activates, gets the mode first and runs at the requested multiplier") {
	FrameGenerationScope scope;
	LogCounter log;
	Ref<MockFrameGenerationExtension> ext = make_provider();
	FrameGenerationRegistry::register_extension(ext);
	FrameGenerationRegistry::update(game_context("TestPassthrough", 3, true));
	CHECK(FrameGenerationRegistry::is_active());
	CHECK(FrameGenerationRegistry::get_active_extension() == ext);
	CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_OK);
	CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 3);
	REQUIRE_EQ(ext->state.events.size(), 2u);
	CHECK_EQ(ext->state.events[0], "set_mode");
	CHECK_EQ(ext->state.events[1], "is_available");
	CHECK_EQ(int(ext->state.last_mode["multiplier"]), 3);
	CHECK(bool(ext->state.last_mode["dynamic"]));
	Dictionary status = FrameGenerationRegistry::get_status();
	CHECK(bool(status["active"]));
	CHECK_EQ(String(status["active_provider"]), "TestPassthrough");
	CHECK_EQ(int(status["multiplier"]), 3);
	CHECK_EQ(int(status["requested_multiplier"]), 3);
	CHECK(bool(status["dynamic"]));

	// Nothing changed: the provider is not asked again.
	FrameGenerationRegistry::update(game_context("TestPassthrough", 3, true));
	CHECK_EQ(ext->state.available_calls, 1);
	CHECK_EQ(ext->state.mode_calls, 1);

	// A new multiplier is clamped to 2-6 and sent again.
	FrameGenerationRegistry::update(game_context("TestPassthrough", 9, true));
	CHECK_EQ(ext->state.mode_calls, 2);
	CHECK_EQ(int(ext->state.last_mode["multiplier"]), 6);
	CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 6);

	// Off again: inactive, reason OK; nothing was logged on the way.
	FrameGenerationRegistry::update(game_context(""));
	CHECK_FALSE(FrameGenerationRegistry::is_active());
	CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_OK);
	CHECK_EQ(log.errors + log.warnings, 0);
}

TEST_CASE("[FrameGeneration] Each fallback reason gives its status and exactly one log line") {
	FrameGenerationScope scope;
	Ref<MockFrameGenerationExtension> ext = make_provider();
	FrameGenerationRegistry::register_extension(ext);
	const Reason provider_reasons[] = { FrameGenerationExtension::REASON_API_UNSUPPORTED, FrameGenerationExtension::REASON_FG_GPU_UNSUPPORTED, FrameGenerationExtension::REASON_NOT_NVIDIA_RTX,
		FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE };
	for (const Reason reason : provider_reasons) {
		const String token = FrameGenerationExtension::get_reason_token(reason);
		LogCounter log;
		ext->state.reason = reason;
		// Registering the same object again has its availability checked again.
		FrameGenerationRegistry::register_extension(ext);
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(game_context("TestPassthrough", 2));
		FrameGenerationRegistry::update(game_context("TestPassthrough", 2));
		ERR_PRINT_ON;
		CHECK_MESSAGE(FrameGenerationRegistry::get_reason() == reason, token);
		CHECK_MESSAGE(!FrameGenerationRegistry::is_active(), token);
		CHECK_MESSAGE(int(FrameGenerationRegistry::get_status()["multiplier"]) == 1, token);
		CHECK_MESSAGE(log.lines_naming(token) == 1, token);
		CHECK_MESSAGE(log.warnings == 1, token);
		CHECK_MESSAGE(log.errors == 0, token);
	}

	// MFG on a GPU without it: FG keeps running at 2x and says why, once.
	{
		LogCounter log;
		ext->state.reason = FrameGenerationExtension::REASON_MFG_REQUIRES_RTX50;
		FrameGenerationRegistry::register_extension(ext);
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(game_context("TestPassthrough", 4));
		FrameGenerationRegistry::update(game_context("TestPassthrough", 4));
		ERR_PRINT_ON;
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_MFG_REQUIRES_RTX50);
		CHECK(FrameGenerationRegistry::is_active());
		const Dictionary status = FrameGenerationRegistry::get_status();
		CHECK_EQ(int(status["multiplier"]), 2);
		CHECK_EQ(int(status["requested_multiplier"]), 4);
		CHECK_EQ(log.lines_naming("MFG_REQUIRES_RTX50"), 1);
		CHECK_EQ(log.warnings, 1);
	}

	// Back to OK, then the same fallback again: a new change logs a new line.
	{
		LogCounter log;
		ext->state.reason = FrameGenerationExtension::REASON_OK;
		FrameGenerationRegistry::register_extension(ext);
		FrameGenerationRegistry::update(game_context("TestPassthrough", 4));
		CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 4);
		ext->state.reason = FrameGenerationExtension::REASON_API_UNSUPPORTED;
		FrameGenerationRegistry::register_extension(ext);
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(game_context("TestPassthrough", 4));
		ERR_PRINT_ON;
		CHECK_EQ(log.lines_naming("API_UNSUPPORTED"), 1);
		CHECK_EQ(log.warnings, 1);
	}
}

TEST_CASE("[FrameGeneration] Main window of a game only: the editor, XR, secondary windows and stereo are rejected") {
	FrameGenerationScope scope;
	Ref<MockFrameGenerationExtension> ext = make_provider();
	FrameGenerationRegistry::register_extension(ext);

	Context editor = game_context("TestPassthrough");
	editor.editor = true;
	{
		LogCounter log;
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(editor);
		FrameGenerationRegistry::update(editor);
		ERR_PRINT_ON;
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_NOT_MAIN_WINDOW);
		CHECK_FALSE(FrameGenerationRegistry::is_active());
		CHECK_EQ(log.lines_naming("NOT_MAIN_WINDOW"), 1);
		// The provider is never asked in the editor.
		CHECK_EQ(ext->state.available_calls, 0);
		CHECK_EQ(ext->state.mode_calls, 0);
	}

	Context xr = game_context("TestPassthrough");
	xr.xr_main_window = true;
	{
		LogCounter log;
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(xr);
		ERR_PRINT_ON;
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_NOT_MAIN_WINDOW);
		CHECK_FALSE(FrameGenerationRegistry::is_active());
		CHECK_EQ(ext->state.available_calls, 0);
		// Same fallback as before: no second line.
		CHECK_EQ(log.lines_naming("NOT_MAIN_WINDOW"), 0);
	}

	// read_context() takes the editor hint from the engine.
	const bool was_editor = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	CHECK(FrameGenerationRegistry::read_context().editor);
	Engine::get_singleton()->set_editor_hint(false);
	CHECK_FALSE(FrameGenerationRegistry::read_context().editor);
	Engine::get_singleton()->set_editor_hint(was_editor);

	// Which viewports split their frame.
	CHECK(FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::MAIN_WINDOW_ID, false, 1, false));
	CHECK_FALSE(FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::WindowID(1), false, 1, false));
	CHECK_FALSE(FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::INVALID_WINDOW_ID, false, 1, false));
	CHECK_FALSE(FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::MAIN_WINDOW_ID, true, 1, false));
	CHECK_FALSE(FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::MAIN_WINDOW_ID, false, 2, false));
	CHECK_FALSE(FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::MAIN_WINDOW_ID, false, 1, true));
}

TEST_CASE("[FrameGeneration] Without a RenderingDevice the reason is API_UNSUPPORTED") {
	FrameGenerationScope scope;
	Ref<MockFrameGenerationExtension> ext = make_provider();
	FrameGenerationRegistry::register_extension(ext);
	Context compatibility = game_context("TestPassthrough");
	compatibility.rendering_device = false;
	LogCounter log;
	ERR_PRINT_OFF;
	FrameGenerationRegistry::update(compatibility);
	ERR_PRINT_ON;
	CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_API_UNSUPPORTED);
	CHECK_FALSE(FrameGenerationRegistry::is_active());
	CHECK_EQ(ext->state.available_calls, 0);
	CHECK_EQ(log.lines_naming("API_UNSUPPORTED"), 1);
	// This test run renders with the dummy rasterizer, which has no RenderingDevice.
	CHECK_FALSE(FrameGenerationRegistry::read_context().rendering_device);
}

TEST_CASE("[FrameGeneration] One active provider per process: a second activation fails with PROVIDER_ALREADY_ACTIVE and one error") {
	FrameGenerationScope scope;
	Ref<MockFrameGenerationExtension> first = make_provider("ProviderA");
	Ref<MockFrameGenerationExtension> second = make_provider("ProviderB");
	FrameGenerationRegistry::register_extension(first);
	FrameGenerationRegistry::register_extension(second);
	FrameGenerationRegistry::update(game_context("ProviderA"));
	REQUIRE(FrameGenerationRegistry::get_active_extension() == first);

	{
		LogCounter log;
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(game_context("ProviderB"));
		FrameGenerationRegistry::update(game_context("ProviderB"));
		ERR_PRINT_ON;
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_PROVIDER_ALREADY_ACTIVE);
		// The first provider keeps the process; the second is never asked.
		CHECK(FrameGenerationRegistry::get_active_extension() == first);
		CHECK_EQ(String(FrameGenerationRegistry::get_status()["active_provider"]), "ProviderA");
		CHECK_EQ(second->state.available_calls, 0);
		CHECK_EQ(log.errors, 1);
		CHECK_EQ(log.warnings, 0);
		CHECK_EQ(log.lines_naming("PROVIDER_ALREADY_ACTIVE"), 1);
	}

	// Back to the first provider: it runs again.
	FrameGenerationRegistry::update(game_context("ProviderA"));
	CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_OK);
	CHECK(FrameGenerationRegistry::get_active_extension() == first);

	// Another object under the active provider's name is refused with one error.
	{
		Ref<MockFrameGenerationExtension> impostor = make_provider("ProviderA");
		LogCounter log;
		ERR_PRINT_OFF;
		FrameGenerationRegistry::register_extension(impostor);
		ERR_PRINT_ON;
		CHECK_EQ(log.errors, 1);
		CHECK_EQ(log.lines_naming("PROVIDER_ALREADY_ACTIVE"), 1);
		FrameGenerationRegistry::update(game_context("ProviderA"));
		CHECK(FrameGenerationRegistry::get_active_extension() == first);
		CHECK_EQ(impostor->state.available_calls, 0);
		CHECK_EQ(impostor->get_reference_count(), 1);
	}

	// Unregistering the active provider turns frame generation off; another provider still cannot
	// activate until the engine restarts, but the same provider can register again.
	FrameGenerationRegistry::unregister_extension(first->get_instance_id());
	CHECK_FALSE(FrameGenerationRegistry::is_active());
	CHECK_EQ(first->get_reference_count(), 1);
	{
		LogCounter log;
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(game_context("ProviderB"));
		ERR_PRINT_ON;
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_PROVIDER_ALREADY_ACTIVE);
		CHECK_FALSE(FrameGenerationRegistry::is_active());
		CHECK_EQ(log.errors, 1);
	}
	Ref<MockFrameGenerationExtension> again = make_provider("ProviderA");
	FrameGenerationRegistry::register_extension(again);
	FrameGenerationRegistry::update(game_context("ProviderA"));
	CHECK(FrameGenerationRegistry::get_active_extension() == again);

	// clear() (engine exit) drops the providers and the process lock.
	FrameGenerationRegistry::clear();
	CHECK_EQ(again->get_reference_count(), 1);
	CHECK_EQ(second->get_reference_count(), 1);
	FrameGenerationRegistry::register_extension(second);
	FrameGenerationRegistry::update(game_context("ProviderB"));
	CHECK(FrameGenerationRegistry::get_active_extension() == second);
}

TEST_CASE("[FrameGeneration] Frame inputs and present calls reach the active provider only") {
	FrameGenerationScope scope;
	Ref<MockFrameGenerationExtension> ext = make_provider();
	FrameGenerationRegistry::register_extension(ext);
	Ref<FrameGenerationInputs> inputs = FrameGenerationRegistry::get_inputs();
	REQUIRE(inputs.is_valid());
	CHECK(FrameGenerationRegistry::get_inputs() == inputs);

	// Inactive: nothing is called.
	FrameGenerationRegistry::dispatch_frame_inputs();
	FrameGenerationRegistry::dispatch_present(0x10, 1, true);
	CHECK_EQ(ext->state.inputs_calls + ext->state.present_calls + ext->state.reset_calls, 0);

	FrameGenerationRegistry::update(game_context("TestPassthrough"));
	REQUIRE(FrameGenerationRegistry::is_active());
	ext->state.events.clear();

	// The first inputs after the provider becomes active carry a reset, even without a viewport reset.
	inputs->set_reset(false);
	inputs->set_frame_index(40);
	FrameGenerationRegistry::dispatch_frame_inputs();
	CHECK(inputs->get_reset());
	// A viewport reset runs _reset() right before _on_frame_inputs(); otherwise no reset.
	inputs->set_reset(true);
	inputs->set_frame_index(41);
	FrameGenerationRegistry::dispatch_frame_inputs();
	inputs->set_reset(false);
	inputs->set_frame_index(42);
	FrameGenerationRegistry::dispatch_frame_inputs();
	REQUIRE_EQ(ext->state.events.size(), 5u);
	CHECK_EQ(ext->state.events[0], "reset");
	CHECK_EQ(ext->state.events[1], "inputs");
	CHECK_EQ(ext->state.events[2], "reset");
	CHECK_EQ(ext->state.events[3], "inputs");
	CHECK_EQ(ext->state.events[4], "inputs");
	CHECK(ext->state.last_inputs == inputs);
	CHECK_EQ(inputs->get_swap_chain(), 0u);

	// One pair per present: the inputs object, with the swap chain and the frame index set.
	ext->state.events.clear();
	FrameGenerationRegistry::dispatch_present(0xABC0, 42, true);
	FrameGenerationRegistry::dispatch_present(0xABC0, 42, false);
	REQUIRE_EQ(ext->state.events.size(), 2u);
	CHECK_EQ(ext->state.events[0], "present_before");
	CHECK_EQ(ext->state.events[1], "present_after");
	CHECK(ext->state.last_present_inputs == inputs.ptr());
	CHECK_EQ(inputs->get_swap_chain(), 0xABC0u);
	CHECK_EQ(inputs->get_frame_index(), 42u);

	// The present hook exists only on D3D12, when the project asks for a provider at startup.
	CHECK_FALSE(FrameGenerationRegistry::has_present_hook());

	// A viewport that frees its split textures empties the inputs only when it filled them.
	inputs->set_hudless_color(RID::from_uint64(31));
	inputs->set_ui_color(RID::from_uint64(32));
	inputs->set_depth(RID::from_uint64(33));
	inputs->set_velocity(RID::from_uint64(34));
	FrameGenerationRegistry::forget_split_textures(RID::from_uint64(41));
	FrameGenerationRegistry::forget_split_textures(RID());
	CHECK(inputs->get_hudless_color() == RID::from_uint64(31));
	CHECK(inputs->get_ui_color() == RID::from_uint64(32));
	CHECK(inputs->get_depth() == RID::from_uint64(33));
	CHECK(inputs->get_velocity() == RID::from_uint64(34));
	FrameGenerationRegistry::forget_split_textures(RID::from_uint64(31));
	CHECK(inputs->get_hudless_color().is_null());
	CHECK(inputs->get_ui_color().is_null());
	CHECK(inputs->get_depth().is_null());
	CHECK(inputs->get_velocity().is_null());

	// A new object under the same name (a scene reloaded its provider) starts with a reset too.
	FrameGenerationRegistry::unregister_extension(ext->get_instance_id());
	Ref<MockFrameGenerationExtension> again = make_provider();
	FrameGenerationRegistry::register_extension(again);
	FrameGenerationRegistry::update(game_context("TestPassthrough"));
	REQUIRE(FrameGenerationRegistry::get_active_extension() == again);
	inputs->set_reset(false);
	FrameGenerationRegistry::dispatch_frame_inputs();
	REQUIRE_EQ(again->state.events.size(), 4u);
	CHECK_EQ(again->state.events[0], "set_mode");
	CHECK_EQ(again->state.events[1], "is_available");
	CHECK_EQ(again->state.events[2], "reset");
	CHECK_EQ(again->state.events[3], "inputs");
}

TEST_CASE("[FrameGeneration] RenderingServer API: status and registration") {
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	FrameGenerationScope scope;
	for (const char *method : { "get_frame_generation_status", "register_frame_generation_extension", "unregister_frame_generation_extension" }) {
		CHECK_MESSAGE(ClassDB::has_method("RenderingServer", method), method);
	}

	const RID camera = rs->camera_create();
	const RID viewport = rs->viewport_create();
	rs->viewport_attach_camera(viewport, camera);
	rs->viewport_set_size(viewport, 640, 360, 1);
	rs->viewport_set_active(viewport, true);
	rs->draw(false, 0.0);

	Dictionary status = rs->get_frame_generation_status();
	CHECK_EQ(int(status["reason"]), int(FrameGenerationExtension::REASON_OK));
	CHECK_FALSE(bool(status["active"]));
	CHECK_EQ(int(status["multiplier"]), 1);
	// An API smoke check only: without a RenderingDevice the split never runs. The testbed's
	// frame_generation scene checks the count on a renderer (0 when off or unregistered, 2 when on).
	REQUIRE(status.has("split_textures"));
	CHECK_EQ(int(status["split_textures"]), 0);

	// Requested and registered, but this run has no RenderingDevice: API_UNSUPPORTED, one line.
	Ref<MockFrameGenerationExtension> ext = make_provider();
	rs->register_frame_generation_extension(ext);
	ProjectSettings::get_singleton()->set_setting(PROVIDER_SETTING, "TestPassthrough");
	ProjectSettings::get_singleton()->set_setting(MULTIPLIER_SETTING, 3);
	{
		LogCounter log;
		ERR_PRINT_OFF;
		rs->draw(false, 0.0);
		status = rs->get_frame_generation_status();
		rs->draw(false, 0.0);
		ERR_PRINT_ON;
		CHECK_EQ(String(status["provider"]), "TestPassthrough");
		CHECK_EQ(int(status["requested_multiplier"]), 3);
		CHECK_EQ(int(status["reason"]), int(FrameGenerationExtension::REASON_API_UNSUPPORTED));
		CHECK_FALSE(bool(status["active"]));
		CHECK_EQ(log.lines_naming("API_UNSUPPORTED"), 1);
	}

	const int registered_refs = ext->get_reference_count();
	CHECK(registered_refs > 1);
	rs->unregister_frame_generation_extension(ext);
	CHECK_EQ(ext->get_reference_count(), 1);

	ProjectSettings::get_singleton()->set_setting(PROVIDER_SETTING, "");
	rs->draw(false, 0.0);
	status = rs->get_frame_generation_status();
	CHECK_EQ(int(status["reason"]), int(FrameGenerationExtension::REASON_OK));
	rs->free_rid(viewport);
	rs->free_rid(camera);
}

} // namespace TestFrameGeneration
