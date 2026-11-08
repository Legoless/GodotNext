/**************************************************************************/
/*  temporal_upscaler.cpp                                                 */
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

#include "temporal_upscaler.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#ifdef DEV_ENABLED
#include "servers/rendering/renderer_rd/shaders/effects/temporal_copy_upscaler.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

class DevCopyTemporalUpscaler : public TemporalUpscaler {
	struct Context {
		Size2i internal_size;
		bool reset = false;
	};

	TemporalCopyUpscalerShaderRD copy_shader;
	RID shader_version;
	RID pipeline;
	HashMap<RenderSceneBuffersRD *, HashMap<uint32_t, Context>> contexts;

public:
	String get_display_name() const override { return "DEV Bilinear Copy"; }
	const Span<char> get_label() const override { return "DEV Bilinear Copy"; }
	bool supports_api(const String &p_api_name) const override { return p_api_name == "Vulkan" || p_api_name == "Metal"; }
	bool supports_multiview() const override { return true; }

	RSE::ViewportScaling3DReason ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) override {
		ERR_FAIL_NULL_V(p_render_buffers, RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);
		if (pipeline.is_null()) {
			return RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED;
		}
		Context &context = contexts[p_render_buffers][p_view];
		context.internal_size = p_render_buffers->get_max_internal_size();
		return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	}

	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) override {
		ERR_FAIL_COND(p_render_buffers.is_null());
		HashMap<uint32_t, Context> *view_contexts = contexts.getptr(p_render_buffers.ptr());
		if (view_contexts == nullptr || !view_contexts->has(p_view)) {
			latch_context_error(p_render_buffers.ptr(), RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);
			return;
		}
		Context &context = (*view_contexts)[p_view];
		context.reset = p_inputs.reset;

		RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
		UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
		ERR_FAIL_NULL(material_storage);
		ERR_FAIL_NULL(uniform_set_cache);
		RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		RID shader = copy_shader.version_get_shader(shader_version, 0);
		ERR_FAIL_COND(shader.is_null());
		RD::Uniform source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_inputs.color }));
		RD::Uniform destination(RD::UNIFORM_TYPE_IMAGE, 1, p_inputs.output);
		RID uniform_set = uniform_set_cache->get_cache(shader, 0, source, destination);
		const uint32_t push_constant[4] = { uint32_t(p_render_buffers->get_target_size().width), uint32_t(p_render_buffers->get_target_size().height), 0, 0 };

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, push_constant, sizeof(push_constant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_render_buffers->get_target_size().width, p_render_buffers->get_target_size().height, 1);
		RD::get_singleton()->compute_list_end();
	}

	void free_contexts(RenderSceneBuffersRD *p_render_buffers) override {
		contexts.erase(p_render_buffers);
	}

	void shutdown() override {
		contexts.clear();
		if (shader_version.is_valid()) {
			copy_shader.version_free(shader_version);
			shader_version = RID();
			pipeline = RID();
		}
	}

	DevCopyTemporalUpscaler() {
		Vector<String> variants;
		variants.push_back("");
		copy_shader.initialize(variants);
		shader_version = copy_shader.version_create();
		pipeline = RD::get_singleton()->compute_pipeline_create(copy_shader.version_get_shader(shader_version, 0));
	}
	~DevCopyTemporalUpscaler() { shutdown(); }
};

static DevCopyTemporalUpscaler *dev_copy_upscaler = nullptr;
#endif

TemporalUpscalerExtensionAdapter::TemporalUpscalerExtensionAdapter(const Ref<TemporalUpscalerExtension> &p_extension) :
		extension(p_extension) {
	display_name = extension.is_valid() ? extension->get_upscaler_name() : String();
	if (display_name.is_empty()) {
		display_name = "Custom Temporal Upscaler";
	}
	label_utf8 = display_name.utf8();
}

Vector2 TemporalUpscalerExtensionAdapter::get_scale_range() const {
	return extension.is_valid() ? extension->get_scale_range() : Vector2(0.25f, 2.0f);
}

bool TemporalUpscalerExtensionAdapter::supports_api(const String &p_api_name) const {
	(void)p_api_name;
	return extension.is_valid() && extension->is_available();
}

float TemporalUpscalerExtensionAdapter::mip_bias_offset() const {
	return extension.is_valid() ? extension->get_mip_bias_offset() : -0.5f;
}

uint32_t TemporalUpscalerExtensionAdapter::jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const {
	if (extension.is_valid()) {
		return extension->get_jitter_phase_count(p_target_size, p_render_size);
	}
	return nextgi_temporal_jitter_phase_count(p_target_size, p_render_size, true);
}

RSE::ViewportScaling3DReason TemporalUpscalerExtensionAdapter::ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) {
	ERR_FAIL_NULL_V(p_render_buffers, RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);
	if (p_view > 0 || extension.is_null() || !extension->is_available()) {
		return RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE;
	}
	contexts[p_render_buffers] |= (1u << p_view);
	return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
}

void TemporalUpscalerExtensionAdapter::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) {
	if (extension.is_null() || !extension->is_available()) {
		if (p_render_buffers.is_valid()) {
			latch_context_error(p_render_buffers.ptr(), RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);
		}
		return;
	}

	if (p_inputs.reset && p_view == 0) {
		extension->reset_history();
	}

	Ref<TemporalUpscalerInputs> ext_inputs;
	ext_inputs.instantiate();
	ext_inputs->set_color(p_inputs.color);
	ext_inputs->set_depth(p_inputs.depth);
	ext_inputs->set_velocity(p_inputs.velocity);
	ext_inputs->set_reactive(p_inputs.reactive);
	ext_inputs->set_output(p_inputs.output);
	ext_inputs->set_internal_size(p_inputs.internal_size);
	ext_inputs->set_max_internal_size(p_inputs.max_internal_size != Size2i() ? p_inputs.max_internal_size : p_inputs.internal_size);
	ext_inputs->set_output_size(p_inputs.output_size != Size2i() ? p_inputs.output_size : (p_render_buffers.is_valid() ? p_render_buffers->get_target_size() : Size2i()));
	ext_inputs->set_motion_vector_scale(Vector2(p_inputs.motion_vector_scale));
	ext_inputs->set_jitter(p_inputs.jitter);
	ext_inputs->set_world_to_view(p_inputs.world_to_view);
	ext_inputs->set_view_to_clip(p_inputs.view_to_clip);
	ext_inputs->set_z_near(p_inputs.z_near);
	ext_inputs->set_z_far(p_inputs.z_far);
	ext_inputs->set_fovy(p_inputs.fovy);
	ext_inputs->set_delta_time(p_inputs.delta_time);
	ext_inputs->set_reset(p_inputs.reset);
	ext_inputs->set_view_index(p_view);
	ext_inputs->set_render_scene_buffers(p_render_buffers);

	extension->process(ext_inputs);
}

void TemporalUpscalerExtensionAdapter::free_contexts(RenderSceneBuffersRD *p_render_buffers) {
	contexts.erase(p_render_buffers);
}

void TemporalUpscalerExtensionAdapter::shutdown() {
	contexts.clear();
}

TemporalUpscaler *TemporalUpscalerRegistry::create_extension_adapter(const Ref<TemporalUpscalerExtension> &p_extension) {
	return memnew(TemporalUpscalerExtensionAdapter(p_extension));
}

void TemporalUpscalerRegistry::destroy_extension_adapter(TemporalUpscaler *p_adapter) {
	if (p_adapter != nullptr) {
		p_adapter->shutdown();
		memdelete(p_adapter);
	}
}

void TemporalUpscalerRegistry::free_contexts(RenderSceneBuffersRD *p_render_buffers) {
	for (int mode = 0; mode < RSE::VIEWPORT_SCALING_3D_MODE_MAX; mode++) {
		// GodotNext: pt_temporal_upscaler_extension (#192). The CUSTOM slot
		// changes under the registry lock; free_extension_contexts() covers it.
		TemporalUpscaler *upscaler = mode == RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM ? nullptr : upscalers[mode];
		if (upscaler != nullptr) {
			upscaler->free_contexts(p_render_buffers);
			upscaler->clear_context_error(p_render_buffers);
		}
	}
	free_extension_contexts(p_render_buffers);
}

void TemporalUpscalerRegistry::shutdown() {
	clear_extensions();
	for (TemporalUpscaler *upscaler : upscalers) {
		if (upscaler != nullptr) {
			upscaler->shutdown();
		}
	}
#ifdef DEV_ENABLED
	if (dev_copy_upscaler != nullptr) {
		memdelete(dev_copy_upscaler);
		dev_copy_upscaler = nullptr;
	}
#endif
	clear();
}

void TemporalUpscalerRegistry::initialize_dev_copy_upscaler() {
#ifdef DEV_ENABLED
	if (OS::get_singleton()->get_environment("GODOT_DEV_COPY_UPSCALER") != "1") {
		return;
	}
	if (dev_copy_upscaler == nullptr) {
		dev_copy_upscaler = memnew(DevCopyTemporalUpscaler);
	}
	register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, dev_copy_upscaler);
#endif
}
