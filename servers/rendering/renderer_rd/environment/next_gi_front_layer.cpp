/**************************************************************************/
/*  next_gi_front_layer.cpp                                               */
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

#include "next_gi_front_layer.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

// GodotNext: nextgi_front_layer_depth (#299).
uint32_t RendererRD::nextgi_front_layer_depth_usage() {
	return RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
}

void NextGIFrontLayer::set_owner(NextGI *p_owner) {
	owner = p_owner;
	if (owner != nullptr) {
		owner->set_front_layer(this);
	}
}

bool NextGIFrontLayer::ensure_targets(const Size2i &p_size) {
	if (framebuffer.is_valid() && size == p_size) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, false);
	if (gather != nullptr) {
		memdelete(gather);
		gather = nullptr;
	}
	if (framebuffer.is_valid()) {
		rd->free_rid(framebuffer);
		framebuffer = RID();
	}
	RID textures[4] = { depth, normal_roughness, reflection, opaque_depth };
	for (RID &texture : textures) {
		if (texture.is_valid()) {
			rd->free_rid(texture);
		}
	}
	opaque_depth = RID();

	RD::TextureFormat format;
	format.width = p_size.x;
	format.height = p_size.y;
	format.format = RD::DATA_FORMAT_D32_SFLOAT;
	format.usage_bits = nextgi_front_layer_depth_usage();
	depth = rd->texture_create(format, RD::TextureView());
	ERR_FAIL_COND_V(depth.is_null(), false);

	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	normal_roughness = rd->texture_create(format, RD::TextureView());
	ERR_FAIL_COND_V(normal_roughness.is_null(), false);

	format.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
	reflection = rd->texture_create(format, RD::TextureView());
	ERR_FAIL_COND_V(reflection.is_null(), false);
	// GodotNext: nextgi_front_layer_hit (#276). The gather allocates the front layer's hit
	// distances at the specular trace size, apart from the view's RB_TEX_SPECULAR_HIT.

	Vector<RID> attachments;
	attachments.push_back(normal_roughness);
	attachments.push_back(depth);
	framebuffer = rd->framebuffer_create(attachments);
	ERR_FAIL_COND_V(framebuffer.is_null(), false);
	size = p_size;
	return true;
}

bool NextGIFrontLayer::copy_opaque_depth(RID p_source, CopyEffects *p_copy_effects) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(p_copy_effects, false);
	ERR_FAIL_COND_V(p_source.is_null(), false);
	if (opaque_depth.is_valid() && rd->texture_get_format(opaque_depth).format != RD::DATA_FORMAT_R32_SFLOAT) {
		rd->free_rid(opaque_depth);
		opaque_depth = RID();
	}
	if (opaque_depth.is_null()) {
		RD::TextureFormat format;
		format.width = size.x;
		format.height = size.y;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		opaque_depth = rd->texture_create(format, RD::TextureView());
		ERR_FAIL_COND_V(opaque_depth.is_null(), false);
	}
	p_copy_effects->copy_to_fb_rect(p_source, FramebufferCacheRD::get_singleton()->get_cache(opaque_depth), Rect2i(0, 0, size.x, size.y));
	return opaque_depth.is_valid();
}

void NextGIFrontLayer::trace(RenderDataRD *p_render_data) {
	if (framebuffer.is_null()) {
		return;
	}
	if (gather == nullptr) {
		gather = memnew(NextGIGather);
		gather->init();
	}
	gather->set_front_layer_inputs(depth, normal_roughness, reflection);
	gather->gather(p_render_data, false);
	dispatch_count++;
}

void NextGIFrontLayer::free_resources() {
	if (gather != nullptr) {
		memdelete(gather);
		gather = nullptr;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		RID resources[5] = { framebuffer, depth, normal_roughness, reflection, opaque_depth };
		for (RID &resource : resources) {
			if (resource.is_valid()) {
				rd->free_rid(resource);
			}
		}
	}
	framebuffer = RID();
	depth = RID();
	normal_roughness = RID();
	reflection = RID();
	opaque_depth = RID();
	size = Size2i();
	if (owner != nullptr && NextGI::get_singleton() == owner) {
		owner->set_front_layer(nullptr);
	}
	owner = nullptr;
}

void NextGIFrontLayer::fill_info(Dictionary &r_info) const {
	r_info["front_layer_targets_created"] = framebuffer.is_valid();
	r_info["front_layer_shader_count"] = gather != nullptr ? int64_t(gather->get_shader_version_count()) : 0;
	r_info["front_layer_dispatch_count"] = int64_t(dispatch_count);
	r_info["front_layer_independent_history"] = gather != nullptr && gather->get_specular_history().is_valid();
	if (gather != nullptr) {
		Dictionary gather_info;
		gather->fill_info(gather_info);
		r_info["front_layer_traced"] = gather_info.get("reflection_rays", 0);
		r_info["front_layer_hits"] = gather_info.get("reflection_hits", 0);
		// GodotNext: nextgi_front_layer_hit (#276). The front layer's trace size and its own hit
		// distance target, which must match it.
		r_info["front_layer_trace_width"] = gather_info.get("reflection_trace_width", 0);
		r_info["front_layer_trace_height"] = gather_info.get("reflection_trace_height", 0);
		const Size2i hit_size = gather->get_front_hit_distance_size();
		r_info["front_layer_hit_distance_width"] = int64_t(hit_size.width);
		r_info["front_layer_hit_distance_height"] = int64_t(hit_size.height);
	} else {
		r_info["front_layer_traced"] = 0;
		r_info["front_layer_hits"] = 0;
		r_info["front_layer_trace_width"] = 0;
		r_info["front_layer_trace_height"] = 0;
		r_info["front_layer_hit_distance_width"] = 0;
		r_info["front_layer_hit_distance_height"] = 0;
	}
}

NextGIFrontLayer::~NextGIFrontLayer() {
	free_resources();
}
