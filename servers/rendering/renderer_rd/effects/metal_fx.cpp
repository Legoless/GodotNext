/**************************************************************************/
/*  metal_fx.cpp                                                          */
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

#ifdef METAL_ENABLED

#include "metal_fx.h"

#include "drivers/metal/pixel_formats.h"
#include "drivers/metal/rendering_device_driver_metal3.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#include <MetalFX/MetalFX.hpp>

using namespace RendererRD;

static void defer_metal_release(CFTypeRef p_obj) {
	if (p_obj == nullptr) {
		return;
	}
	if (RD::get_singleton() != nullptr && RD::get_singleton()->get_device_driver() != nullptr) {
		RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
		rdd->defer_release(p_obj);
	} else {
		CFRelease(p_obj);
	}
}

#pragma mark - Spatial Scaler

MFXSpatialContext::~MFXSpatialContext() {
	if (scaler) {
		defer_metal_release(scaler);
		scaler = nullptr;
	}
}

MFXSpatialEffect::MFXSpatialEffect() {
}

MFXSpatialEffect::~MFXSpatialEffect() {
}

void MFXSpatialEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	obj->end();

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::SpatialScalerBase *scaler = p_userdata->scaler;
	scaler->setFence(obj->external_pass_fence());
	scaler->setColorTexture(src_texture);
	scaler->setOutputTexture(dst_texture);
	MTLFX::SpatialScaler *s = static_cast<MTLFX::SpatialScaler *>(scaler);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

void MFXSpatialEffect::ensure_context(Ref<RenderSceneBuffersRD> p_render_buffers) {
	p_render_buffers->ensure_mfx(this);
}

void MFXSpatialEffect::process(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_src, RID p_dst) {
	MFXSpatialContext *ctx = p_render_buffers->get_mfx_spatial_context();
	DEV_ASSERT(ctx); // this should have been done by the caller via ensure_context

	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_dst)),
			*ctx);
	RD::CallbackResource res[2] = {
		{ .rid = p_src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE }
	};
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXSpatialEffect::callback, userdata, VectorView<RD::CallbackResource>(res, 2));
}

MFXSpatialContext *MFXSpatialEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_SPATIAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::SpatialScalerDescriptor> desc = NS::TransferPtr(MTLFX::SpatialScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));
	desc->setColorProcessingMode(MTLFX::SpatialScalerColorProcessingModeLinear);

	MFXSpatialContext *context = memnew(MFXSpatialContext);
	context->scaler = desc->newSpatialScaler(dev);

	return context;
}

#ifdef METAL_MFXTEMPORAL_ENABLED

#pragma mark - Temporal Scaler

MFXTemporalContext::~MFXTemporalContext() {
	if (pad_src) {
		defer_metal_release(pad_src);
		pad_src = nullptr;
	}
	if (pad_depth) {
		defer_metal_release(pad_depth);
		pad_depth = nullptr;
	}
	if (pad_motion) {
		defer_metal_release(pad_motion);
		pad_motion = nullptr;
	}
	if (scaler) {
		defer_metal_release(scaler);
		scaler = nullptr;
	}
}

MFXTemporalEffect::MFXTemporalEffect() {}
MFXTemporalEffect::~MFXTemporalEffect() {}

MFXTemporalContext *MFXTemporalEffect::create_context(CreateParams p_params) const {
	DEV_ASSERT(RD::get_singleton()->has_feature(RD::SUPPORTS_METALFX_TEMPORAL));

	RenderingDeviceDriverMetal *rdd = (RenderingDeviceDriverMetal *)RD::get_singleton()->get_device_driver();
	PixelFormats &pf = rdd->get_pixel_formats();
	MTL::Device *dev = rdd->get_device();

	NS::SharedPtr<MTLFX::TemporalScalerDescriptor> desc = NS::TransferPtr(MTLFX::TemporalScalerDescriptor::alloc()->init());
	desc->setInputWidth((NS::UInteger)p_params.input_size.width);
	desc->setInputHeight((NS::UInteger)p_params.input_size.height);

	desc->setOutputWidth((NS::UInteger)p_params.output_size.width);
	desc->setOutputHeight((NS::UInteger)p_params.output_size.height);

	desc->setColorTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.input_format));
	desc->setDepthTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.depth_format));
	desc->setMotionTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.motion_format));
	desc->setAutoExposureEnabled(false);

	desc->setOutputTextureFormat((MTL::PixelFormat)pf.getMTLPixelFormat(p_params.output_format));

	const Vector2i min_in = (p_params.min_input_size != Vector2i()) ? p_params.min_input_size : p_params.input_size;
	if (p_params.dynamic_resolution) {
		const float hw_min_scale = MTLFX::TemporalScalerDescriptor::supportedInputContentMinScale(dev);
		const float hw_max_scale = MTLFX::TemporalScalerDescriptor::supportedInputContentMaxScale(dev);
		const float min_scale_x = float(p_params.output_size.width) / float(MAX(1, p_params.input_size.width));
		const float min_scale_y = float(p_params.output_size.height) / float(MAX(1, p_params.input_size.height));
		const float max_scale_x = float(p_params.output_size.width) / float(MAX(1, min_in.width));
		const float max_scale_y = float(p_params.output_size.height) / float(MAX(1, min_in.height));
		const float req_min = CLAMP(MIN(min_scale_x, min_scale_y) - 1e-4f, hw_min_scale, hw_max_scale);
		const float req_max = CLAMP(MAX(max_scale_x, max_scale_y) + 1e-4f, req_min, hw_max_scale);
		desc->setInputContentPropertiesEnabled(true);
		desc->setInputContentMinScale(req_min);
		desc->setInputContentMaxScale(req_max);
	}

	MFXTemporalContext *context = memnew(MFXTemporalContext);
	context->max_input_size = p_params.input_size;
	context->min_input_size = min_in;
	context->output_size = p_params.output_size;
	context->dynamic_resolution = p_params.dynamic_resolution;
	context->scaler = desc->newTemporalScaler(dev);
	if (context->scaler == nullptr) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "MetalFX failed to create MTLFXTemporalScaler.");
	}
	context->scaler->setMotionVectorScaleX(p_params.motion_vector_scale.x);
	context->scaler->setMotionVectorScaleY(p_params.motion_vector_scale.y);
	context->scaler->setDepthReversed(true); // Godot uses reverse Z per https://github.com/godotengine/godot/pull/88328

	return context;
}

void MFXTemporalEffect::process(RendererRD::MFXTemporalContext *p_ctx, RendererRD::MFXTemporalEffect::Params p_params) {
	ERR_FAIL_NULL(p_ctx);
	ERR_FAIL_NULL(p_ctx->scaler);
	CallbackArgs *userdata = args_allocator.alloc(
			this,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.src)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.depth)),
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.motion)),
			p_params.exposure.is_valid() ? RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.exposure)) : RDD::TextureID(),
			p_params.jitter_offset,
			p_params.content_size,
			RDD::TextureID(RD::get_singleton()->get_driver_resource(RDC::DRIVER_RESOURCE_TEXTURE, p_params.dst)),
			p_ctx,
			p_params.reset);
	RD::CallbackResource res[5] = {
		{ .rid = p_params.src, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.depth, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.motion, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ .rid = p_params.dst, .usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE },
		{ .rid = p_params.exposure, .usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
	};
	uint32_t res_count = p_params.exposure.is_valid() ? 5 : 4;
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)MFXTemporalEffect::callback, userdata, VectorView<RD::CallbackResource>(res, res_count));
}

void MFXTemporalEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	MDCommandBufferBase *obj = (MDCommandBufferBase *)(p_command_buffer.id);
	MTL3::MDCommandBuffer *cmd = (MTL3::MDCommandBuffer *)(p_command_buffer.id);

	MTL::Texture *src_texture = reinterpret_cast<MTL::Texture *>(p_userdata->src.id);
	MTL::Texture *depth = reinterpret_cast<MTL::Texture *>(p_userdata->depth.id);
	MTL::Texture *motion = reinterpret_cast<MTL::Texture *>(p_userdata->motion.id);
	MTL::Texture *exposure = reinterpret_cast<MTL::Texture *>(p_userdata->exposure.id);
	MTL::Texture *dst_texture = reinterpret_cast<MTL::Texture *>(p_userdata->dst.id);

	MTLFX::TemporalScalerBase *scaler = p_userdata->scaler;
	MFXTemporalContext *ctx = p_userdata->ctx;
	const NS::UInteger max_w = scaler->inputWidth();
	const NS::UInteger max_h = scaler->inputHeight();
	const NS::UInteger content_w = (p_userdata->content_size.width > 0) ? MIN((NS::UInteger)p_userdata->content_size.width, max_w) : MIN(src_texture->width(), max_w);
	const NS::UInteger content_h = (p_userdata->content_size.height > 0) ? MIN((NS::UInteger)p_userdata->content_size.height, max_h) : MIN(src_texture->height(), max_h);

	const bool needs_pad = (src_texture->width() != max_w || src_texture->height() != max_h ||
			depth->width() != max_w || depth->height() != max_h ||
			motion->width() != max_w || motion->height() != max_h);

	if (needs_pad && ctx != nullptr) {
		MTL::Device *dev = src_texture->device();
		auto ensure_pad_tex = [&](MTL::Texture *&r_pad, MTL::Texture *p_like, MTL::TextureUsage p_usage) {
			if (r_pad != nullptr && (r_pad->width() != max_w || r_pad->height() != max_h || r_pad->pixelFormat() != p_like->pixelFormat())) {
				defer_metal_release(r_pad);
				r_pad = nullptr;
			}
			if (r_pad == nullptr) {
				NS::SharedPtr<MTL::TextureDescriptor> td = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
				td->setTextureType(MTL::TextureType2D);
				td->setPixelFormat(p_like->pixelFormat());
				td->setWidth(max_w);
				td->setHeight(max_h);
				td->setMipmapLevelCount(1);
				td->setStorageMode(MTL::StorageModePrivate);
				td->setHazardTrackingMode(p_like->hazardTrackingMode());
				td->setUsage(p_usage);
				r_pad = dev->newTexture(td.get());
			}
		};

		ensure_pad_tex(ctx->pad_src, src_texture, scaler->colorTextureUsage());
		ensure_pad_tex(ctx->pad_depth, depth, scaler->depthTextureUsage());
		ensure_pad_tex(ctx->pad_motion, motion, scaler->motionTextureUsage());

		cmd->encode_texture_subregion_copy(src_texture, ctx->pad_src, (uint32_t)content_w, (uint32_t)content_h);
		cmd->encode_texture_subregion_copy(depth, ctx->pad_depth, (uint32_t)content_w, (uint32_t)content_h);
		cmd->encode_texture_subregion_copy(motion, ctx->pad_motion, (uint32_t)content_w, (uint32_t)content_h);

		obj->retain_resource(ctx->pad_src);
		obj->retain_resource(ctx->pad_depth);
		obj->retain_resource(ctx->pad_motion);

		src_texture = ctx->pad_src;
		depth = ctx->pad_depth;
		motion = ctx->pad_motion;
	}

	obj->end();

	if (ctx != nullptr && ctx->dynamic_resolution) {
		scaler->setInputContentWidth(content_w);
		scaler->setInputContentHeight(content_h);
	}
	scaler->setMotionVectorScaleX((float)content_w);
	scaler->setMotionVectorScaleY((float)content_h);
	scaler->setFence(obj->external_pass_fence());
	scaler->setReset(p_userdata->reset);
	scaler->setColorTexture(src_texture);
	scaler->setDepthTexture(depth);
	scaler->setMotionTexture(motion);
	scaler->setExposureTexture(exposure);
	scaler->setJitterOffsetX(p_userdata->jitter_offset.x);
	scaler->setJitterOffsetY(p_userdata->jitter_offset.y);
	scaler->setOutputTexture(dst_texture);
	MTLFX::TemporalScaler *s = static_cast<MTLFX::TemporalScaler *>(scaler);
	s->encodeToCommandBuffer(cmd->get_command_buffer());
	obj->retain_resource(scaler);

	CallbackArgs::free(&p_userdata);
}

#endif

#endif
