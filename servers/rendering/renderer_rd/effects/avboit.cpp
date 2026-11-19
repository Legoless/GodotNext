/**************************************************************************/
/*  avboit.cpp                                                            */
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

#include "avboit.h"

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererRD {

AVBOIT::SurfaceClass AVBOIT::surface_class(int p_blend_mode, bool p_shadow_to_opacity) {
	switch (p_blend_mode) {
		case MaterialStorage::ShaderData::BLEND_MODE_MIX:
		case MaterialStorage::ShaderData::BLEND_MODE_PREMULTIPLIED_ALPHA:
		case MaterialStorage::ShaderData::BLEND_MODE_ALPHA_TO_COVERAGE:
			// shadow_to_opacity replaces the alpha with the light the surface catches, which only the
			// color pass computes: the pre-passes would record the material's ALPHA instead.
			return p_shadow_to_opacity ? SURFACE_UNRECORDED : SURFACE_OCCLUDING;
		case MaterialStorage::ShaderData::BLEND_MODE_ADD:
		case MaterialStorage::ShaderData::BLEND_MODE_SUB:
			return SURFACE_UNRECORDED;
		default:
			return SURFACE_REMAINDER;
	}
}

bool AVBOIT::pass_draws(Pass p_pass, int p_blend_mode, bool p_shadow_to_opacity) {
	const SurfaceClass surface = surface_class(p_blend_mode, p_shadow_to_opacity);
	switch (p_pass) {
		case PASS_NONE:
			return true;
		case PASS_BOUNDS:
		case PASS_OCCLUSION:
			return surface == SURFACE_OCCLUDING;
		case PASS_COMPOSITE:
			return surface != SURFACE_REMAINDER;
		case PASS_REMAINDER:
			return surface == SURFACE_REMAINDER;
	}
	return false;
}

AVBOIT::CompositeMode AVBOIT::composite_mode(int p_blend_mode, bool p_shadow_to_opacity) {
	switch (p_blend_mode) {
		case MaterialStorage::ShaderData::BLEND_MODE_MIX:
		case MaterialStorage::ShaderData::BLEND_MODE_ALPHA_TO_COVERAGE:
			return p_shadow_to_opacity ? COMPOSITE_MIX_UNRECORDED : COMPOSITE_MIX;
		case MaterialStorage::ShaderData::BLEND_MODE_PREMULTIPLIED_ALPHA:
			return p_shadow_to_opacity ? COMPOSITE_PREMULTIPLIED_UNRECORDED : COMPOSITE_PREMULTIPLIED;
		case MaterialStorage::ShaderData::BLEND_MODE_ADD:
			return COMPOSITE_ADD;
		case MaterialStorage::ShaderData::BLEND_MODE_SUB:
			return COMPOSITE_SUBTRACT;
		default:
			return COMPOSITE_OFF;
	}
}

Size2i AVBOIT::grid_size(const Size2i &p_internal_size) {
	return Size2i(MAX((p_internal_size.x + TILE_SIZE - 1) / TILE_SIZE, 1), MAX((p_internal_size.y + TILE_SIZE - 1) / TILE_SIZE, 1));
}

Vector2i AVBOIT::cell_of_pixel(const Vector2 &p_frag_coord, const Size2i &p_internal_size, const Size2i &p_grid_size) {
	// GLSL: ivec2(gl_FragCoord.xy * scene_data.screen_pixel_size * vec2(grid)). The low-resolution
	// pre-passes cover the same screen with p_grid_size pixels, so a cell is what they wrote there.
	const int x = int(p_frag_coord.x / float(p_internal_size.x) * float(p_grid_size.x));
	const int y = int(p_frag_coord.y / float(p_internal_size.y) * float(p_grid_size.y));
	return Vector2i(CLAMP(x, 0, p_grid_size.x - 1), CLAMP(y, 0, p_grid_size.y - 1));
}

Vector2 AVBOIT::cell_center(const Vector2i &p_cell, const Size2i &p_internal_size, const Size2i &p_grid_size) {
	// GLSL: (vec2(cell) + 0.5) / grid / scene_data.screen_pixel_size, the window position of the grid
	// pixel center the low-resolution pre-passes sampled.
	return Vector2((float(p_cell.x) + 0.5f) / float(p_grid_size.x) * float(p_internal_size.x), (float(p_cell.y) + 0.5f) / float(p_grid_size.y) * float(p_internal_size.y));
}

float AVBOIT::view_depth(const Projection &p_inv_projection, const Vector2 &p_ndc, float p_window_depth) {
	// GLSL: avboit_view_depth().
	const Vector4 view_position = p_inv_projection.xform(Vector4(p_ndc.x, p_ndc.y, CLAMP(p_window_depth, 0.0f, 1.0f), 1.0f));
	return grid_depth(-view_position.z / view_position.w);
}

float AVBOIT::grid_depth(float p_view_depth) {
	return CLAMP(p_view_depth, MIN_DEPTH, MAX_DEPTH);
}

float AVBOIT::optical_depth(float p_alpha) {
	return -Math::log(1.0f - CLAMP(p_alpha, 0.0f, MAX_ALPHA));
}

uint32_t AVBOIT::slice(float p_depth, float p_near, float p_far) {
	if (!(p_far > p_near * 1.0001f)) {
		return 0;
	}
	const float t = Math::log(MAX(p_depth, p_near) / p_near) / Math::log(p_far / p_near);
	return uint32_t(CLAMP(t * float(SLICES), 0.0f, float(SLICES - 1)));
}

float AVBOIT::transmittance_front(const float *p_slices, float p_near, float p_far, float p_depth, float p_self_optical_depth) {
	// An empty cell, or a surface nearer than every occluder at the cell's center: nothing is in front.
	if (!(p_far > 0.0f) || p_depth < p_near) {
		return 1.0f;
	}
	// Its own slice: every slice within SLICE_TOLERANCE of its depth, as the pre-passes may have put
	// its sample on the other side of a slice boundary.
	const uint32_t own_first = slice(p_depth * (1.0f - SLICE_TOLERANCE), p_near, p_far);
	const uint32_t own_last = slice(p_depth * (1.0f + SLICE_TOLERANCE), p_near, p_far);
	float in_front = 0.0f;
	for (uint32_t i = 0; i < own_first; i++) {
		in_front += p_slices[i];
	}
	float same = 0.0f;
	for (uint32_t i = own_first; i <= own_last; i++) {
		same += p_slices[i];
	}
	// Behind every occluder the cell recorded: its own sample is not among them, all are in front.
	if (p_depth > p_far * FAR_MARGIN) {
		return Math::exp(-(in_front + same));
	}
	// The others in its own slice dim it; its own share comes off that slice only, so a surface the
	// cell's center missed (nothing of its own there) is not let through the slices in front of it.
	return Math::exp(-(in_front + MAX(same - p_self_optical_depth, 0.0f)));
}

RD::PipelineColorBlendState AVBOIT::bounds_blend_state() {
	// Min of the depths in .rgb, max in .a.
	RD::PipelineColorBlendState::Attachment attachment;
	attachment.enable_blend = true;
	attachment.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.color_blend_op = RD::BLEND_OP_MINIMUM;
	attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.alpha_blend_op = RD::BLEND_OP_MAXIMUM;
	RD::PipelineColorBlendState state;
	state.attachments.push_back(attachment);
	return state;
}

RD::PipelineColorBlendState AVBOIT::occlusion_blend_state() {
	RD::PipelineColorBlendState::Attachment attachment;
	attachment.enable_blend = true;
	attachment.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.color_blend_op = RD::BLEND_OP_ADD;
	attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.alpha_blend_op = RD::BLEND_OP_ADD;
	RD::PipelineColorBlendState state;
	for (uint32_t i = 0; i < LAYERS; i++) {
		state.attachments.push_back(attachment);
	}
	return state;
}

RD::PipelineColorBlendState::Attachment AVBOIT::composite_blend_attachment() {
	// Color: a plain sum of the weighted contributions. Alpha: a + dst * (1 - a), which leaves
	// 1 - prod(1 - a) whatever the order.
	RD::PipelineColorBlendState::Attachment attachment;
	attachment.enable_blend = true;
	attachment.src_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_color_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.color_blend_op = RD::BLEND_OP_ADD;
	attachment.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	attachment.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	attachment.alpha_blend_op = RD::BLEND_OP_ADD;
	return attachment;
}

::AVBOITPolicy::DeviceFeatures AVBOIT::query_device_features(RenderingDevice *p_device) {
	::AVBOITPolicy::DeviceFeatures features;
	if (p_device == nullptr) {
		return features;
	}
	// The format query has no blend bit. Blending (add, min, max) is required of an RGBA16F color
	// attachment by Vulkan's required format support, by D3D12 and by Metal, so attachment and
	// sampling support stand for it.
	features.rgba16f_attachment = p_device->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT);
	features.rgba16f_storage = p_device->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_STORAGE_BIT);
	features.max_color_attachments = uint32_t(MIN(p_device->limit_get(RD::LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS), uint64_t(UINT32_MAX)));
	return features;
}

bool AVBOIT::ensure_targets(RenderSceneBuffersRD *p_buffers, RID p_depth, Targets &r_targets) {
	ERR_FAIL_NULL_V(p_buffers, false);
	ERR_FAIL_COND_V(p_depth.is_null(), false);
	const Size2i grid = grid_size(p_buffers->get_internal_size());
	const uint32_t usage = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	if (!p_buffers->has_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_BOUNDS)) {
		p_buffers->create_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_BOUNDS, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, grid, 1);
	}
	if (!p_buffers->has_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_OCCLUSION)) {
		p_buffers->create_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_OCCLUSION, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, grid, LAYERS);
	}
	if (!p_buffers->has_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_ACCUM)) {
		p_buffers->create_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_ACCUM, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, Size2i(), 1);
	}
	r_targets.grid = grid;
	r_targets.bounds = p_buffers->get_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_BOUNDS);
	r_targets.occlusion = p_buffers->get_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_OCCLUSION);
	r_targets.accum = p_buffers->get_texture(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_ACCUM);
	ERR_FAIL_COND_V(r_targets.bounds.is_null() || r_targets.occlusion.is_null() || r_targets.accum.is_null(), false);

	FramebufferCacheRD *framebuffers = FramebufferCacheRD::get_singleton();
	r_targets.bounds_framebuffer = framebuffers->get_cache(r_targets.bounds);
	Vector<RID> layers;
	for (uint32_t i = 0; i < LAYERS; i++) {
		layers.push_back(p_buffers->get_texture_slice(RB_SCOPE_AVBOIT, RB_TEX_AVBOIT_OCCLUSION, i, 0));
	}
	r_targets.occlusion_framebuffer = framebuffers->get_cache(layers[0], layers[1], layers[2], layers[3]);
	r_targets.composite_framebuffer = framebuffers->get_cache(r_targets.accum, p_depth);
	return r_targets.bounds_framebuffer.is_valid() && r_targets.occlusion_framebuffer.is_valid() && r_targets.composite_framebuffer.is_valid();
}

bool AVBOIT::_ensure_shader() {
	if (pipeline.is_valid()) {
		return true;
	}
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	ERR_FAIL_COND_V(shader_version.is_null(), false);
	const RID resolve_shader = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND_V(resolve_shader.is_null(), false);
	pipeline = RD::get_singleton()->compute_pipeline_create(resolve_shader);
	return pipeline.is_valid();
}

void AVBOIT::resolve(RID p_accum, RID p_color, const Rect2i &p_rect) {
	ERR_FAIL_COND(p_rect.size.x <= 0 || p_rect.size.y <= 0);
	if (!_ensure_shader()) {
		return;
	}
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	struct PushConstant {
		int32_t offset[2];
		int32_t size[2];
	} push_constant;
	push_constant.offset[0] = p_rect.position.x;
	push_constant.offset[1] = p_rect.position.y;
	push_constant.size[0] = p_rect.size.x;
	push_constant.size[1] = p_rect.size.y;

	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID resolve_shader = shader.version_get_shader(shader_version, 0);
	RD::Uniform u_accum(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_accum }));
	RD::Uniform u_color(RD::UNIFORM_TYPE_IMAGE, 1, p_color);
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(resolve_shader, 0, u_accum, u_color), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_rect.size.x, p_rect.size.y, 1);
	RD::get_singleton()->compute_list_end();
}

AVBOIT::~AVBOIT() {
	if (shader_version.is_valid()) {
		// The pipeline is freed with the shader version.
		shader.version_free(shader_version);
	}
}

} // namespace RendererRD
