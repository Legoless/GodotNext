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

#ifdef DEV_ENABLED
#include "servers/rendering/renderer_rd/shaders/effects/temporal_copy_upscaler.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
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
		context.internal_size = p_render_buffers->get_internal_size();
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

void TemporalUpscalerRegistry::free_contexts(RenderSceneBuffersRD *p_render_buffers) {
	for (TemporalUpscaler *upscaler : upscalers) {
		if (upscaler != nullptr) {
			upscaler->free_contexts(p_render_buffers);
			upscaler->clear_context_error(p_render_buffers);
		}
	}
}

void TemporalUpscalerRegistry::shutdown() {
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
