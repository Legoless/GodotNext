/**************************************************************************/
/*  metal3_objects.cpp                                                    */
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

/**************************************************************************/
/*                                                                        */
/* Portions of this code were derived from MoltenVK.                      */
/*                                                                        */
/* Copyright (c) 2015-2023 The Brenwill Workshop Ltd.                     */
/* (http://www.brenwill.com)                                              */
/*                                                                        */
/* Licensed under the Apache License, Version 2.0 (the "License");        */
/* you may not use this file except in compliance with the License.       */
/* You may obtain a copy of the License at                                */
/*                                                                        */
/*     http://www.apache.org/licenses/LICENSE-2.0                         */
/*                                                                        */
/* Unless required by applicable law or agreed to in writing, software    */
/* distributed under the License is distributed on an "AS IS" BASIS,      */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or        */
/* implied. See the License for the specific language governing           */
/* permissions and limitations under the License.                         */
/**************************************************************************/

#include "metal3_objects.h"

// GodotNext: metal_shader_validation
#include "drivers/metal/metal_shader_validation.h"
#include "drivers/metal/metal_utils.h"
#include "drivers/metal/pixel_formats.h"
#include "drivers/metal/rendering_device_driver_metal3.h"
#include "drivers/metal/rendering_shader_container_metal.h"

#include <algorithm>

// GodotNext: metal_shader_validation
static_assert(METAL_RESOURCE_USAGE_READ == static_cast<uint32_t>(MTL::ResourceUsageRead), "MTLResourceUsageRead changed");
static_assert(METAL_RESOURCE_USAGE_WRITE == static_cast<uint32_t>(MTL::ResourceUsageWrite), "MTLResourceUsageWrite changed");

using namespace MTL3;

MDCommandBuffer::MDCommandBuffer(MTL::CommandQueue *p_queue, ::RenderingDeviceDriverMetal *p_device_driver) :
		_scratch(p_device_driver->get_allocator()), queue(p_queue) {
	device_driver = p_device_driver;
	type = MDCommandBufferStateType::None;
	sync_mode = device_driver->sync_mode;
	if (sync_mode != RDM::SyncMode::HazardTracking) {
		_create_level_fences(p_device_driver->get_device());
	}
}

void MDCommandBuffer::_encode_residency(MTL::RenderCommandEncoder *p_enc) {
	if (sync_mode == RDM::SyncMode::HazardTracking) {
		return; // ResourceTracker handles residency + hazards.
	}
	MetalAllocator *alloc = device_driver->get_allocator();
	if (alloc->get_heap_generation() != _resident_heaps_generation) {
		_resident_heaps.clear();
		_resident_heaps_generation = alloc->get_heaps(_resident_heaps);
	}
	p_enc->useHeaps(_resident_heaps.ptr(), _resident_heaps.size(), MTL::RenderStageVertex | MTL::RenderStageFragment);
	device_driver->encode_imported_resources(p_enc);
}

void MDCommandBuffer::_encode_residency(MTL::ComputeCommandEncoder *p_enc) {
	if (sync_mode == RDM::SyncMode::HazardTracking) {
		return;
	}
	MetalAllocator *alloc = device_driver->get_allocator();
	if (alloc->get_heap_generation() != _resident_heaps_generation) {
		_resident_heaps.clear();
		_resident_heaps_generation = alloc->get_heaps(_resident_heaps);
	}
	p_enc->useHeaps(_resident_heaps.ptr(), _resident_heaps.size());
	device_driver->encode_imported_resources(p_enc);
}

// Both render stages on both sides. Narrower masks (update after fragment, wait
// before vertex) would allow more overlap, but the graph does not tell us which
// stage of a pass reads or writes, so use the conservative mask.
static constexpr MTL::RenderStages RENDER_FENCE_STAGES = MTL::RenderStageVertex | MTL::RenderStageFragment;
// GodotNext: rd_mesh_shaders (#178). Added to an encoder's residency and fences only once it binds a mesh pipeline
// (_encode_mesh_stages()), so passes without mesh shaders keep the masks above.
static constexpr MTL::RenderStages MESH_FENCE_STAGES = MTL::RenderStageObject | MTL::RenderStageMesh;

// GodotNext: rd_mesh_shaders (#178). Returns the fence waited on, which a mesh pipeline bound later in the encoder
// waits on too.
MTL::Fence *MDCommandBuffer::_fence_wait(MTL::RenderCommandEncoder *p_enc) {
	MTL::Fence *fence = _fence_to_wait();
	if (p_enc && fence) {
		p_enc->waitForFence(fence, RENDER_FENCE_STAGES);
	}
	return fence;
}

// GodotNext: rd_mesh_shaders (#178). The object and mesh stages read resources and wait on the encoder's fence like
// the vertex and fragment stages, from the first mesh pipeline bound on the encoder.
void MDCommandBuffer::_encode_mesh_stages() {
	MTL::RenderCommandEncoder *enc = render.encoder.get();
	if (enc == nullptr || render.mesh_stages_encoded) {
		return;
	}
	render.mesh_stages_encoded = true;
	if (sync_mode != RDM::SyncMode::HazardTracking) {
		// _encode_residency() filled _resident_heaps when the encoder was created.
		enc->useHeaps(_resident_heaps.ptr(), _resident_heaps.size(), MESH_FENCE_STAGES);
		device_driver->encode_imported_resources(enc, MESH_FENCE_STAGES);
	}
	if (render.encoder_wait_fence != nullptr) {
		enc->waitForFence(render.encoder_wait_fence, MESH_FENCE_STAGES);
	}
}

void MDCommandBuffer::_fence_wait(MTL::ComputeCommandEncoder *p_enc) {
	MTL::Fence *fence = _fence_to_wait();
	if (p_enc && fence) {
		p_enc->waitForFence(fence);
	}
}

void MDCommandBuffer::_fence_wait(MTL::BlitCommandEncoder *p_enc) {
	MTL::Fence *fence = _fence_to_wait();
	if (p_enc && fence) {
		p_enc->waitForFence(fence);
	}
}

void MDCommandBuffer::_fence_update(MTL::RenderCommandEncoder *p_enc, bool p_mesh_stages) {
	if (!p_enc) {
		return;
	}
	if (MTL::Fence *fence = _fence_to_update()) {
		// GodotNext: rd_mesh_shaders (#178). After the object and mesh stages too when the encoder ran them.
		p_enc->updateFence(fence, p_mesh_stages ? (RENDER_FENCE_STAGES | MESH_FENCE_STAGES) : RENDER_FENCE_STAGES);
	}
}

void MDCommandBuffer::_fence_update(MTL::ComputeCommandEncoder *p_enc) {
	if (!p_enc) {
		return;
	}
	if (MTL::Fence *fence = _fence_to_update()) {
		p_enc->updateFence(fence);
	}
}

void MDCommandBuffer::_fence_update(MTL::BlitCommandEncoder *p_enc) {
	if (!p_enc) {
		return;
	}
	if (MTL::Fence *fence = _fence_to_update()) {
		p_enc->updateFence(fence);
	}
}

void MDCommandBuffer::begin_label(const char *p_label_name, const Color &p_color) {
	NS::SharedPtr<NS::String> s = NS::TransferPtr(NS::String::alloc()->init(p_label_name, NS::UTF8StringEncoding));
	switch (type) {
		case MDCommandBufferStateType::None:
			command_buffer()->pushDebugGroup(s.get());
			break;
		case MDCommandBufferStateType::Render:
			render.encoder->pushDebugGroup(s.get());
			break;
		case MDCommandBufferStateType::InlineRender:
			inline_render.encoder->pushDebugGroup(s.get());
			break;
		case MDCommandBufferStateType::Compute:
			compute.encoder->pushDebugGroup(s.get());
			break;
		case MDCommandBufferStateType::Blit:
			blit.encoder->pushDebugGroup(s.get());
			break;
		// GodotNext: metal_acceleration_structure
		case MDCommandBufferStateType::AccelerationStructure:
			as.encoder->pushDebugGroup(s.get());
			break;
	}
	label_stack.push_back({ type, false });
}

void MDCommandBuffer::end_label() {
	if (label_stack.is_empty()) {
		return;
	}
	LabelStackEntry entry = label_stack[label_stack.size() - 1];
	label_stack.remove_at(label_stack.size() - 1);
	if (entry.stale) {
		return;
	}
	switch (entry.type) {
		case MDCommandBufferStateType::None:
			command_buffer()->popDebugGroup();
			break;
		case MDCommandBufferStateType::Render:
			render.encoder->popDebugGroup();
			break;
		case MDCommandBufferStateType::InlineRender:
			inline_render.encoder->popDebugGroup();
			break;
		case MDCommandBufferStateType::Compute:
			compute.encoder->popDebugGroup();
			break;
		case MDCommandBufferStateType::Blit:
			blit.encoder->popDebugGroup();
			break;
		// GodotNext: metal_acceleration_structure
		case MDCommandBufferStateType::AccelerationStructure:
			as.encoder->popDebugGroup();
			break;
	}
}

void MDCommandBuffer::_pop_active_encoder_labels() {
	if (type == MDCommandBufferStateType::None) {
		return;
	}
	for (int64_t i = (int64_t)label_stack.size() - 1; i >= 0; i--) {
		LabelStackEntry &entry = label_stack[i];
		if (entry.stale || entry.type != type) {
			continue;
		}
		switch (type) {
			case MDCommandBufferStateType::Render:
				render.encoder->popDebugGroup();
				break;
			case MDCommandBufferStateType::InlineRender:
				inline_render.encoder->popDebugGroup();
				break;
			case MDCommandBufferStateType::Compute:
				compute.encoder->popDebugGroup();
				break;
			case MDCommandBufferStateType::Blit:
				blit.encoder->popDebugGroup();
				break;
			// GodotNext: metal_acceleration_structure
			case MDCommandBufferStateType::AccelerationStructure:
				as.encoder->popDebugGroup();
				break;
			case MDCommandBufferStateType::None:
				break;
		}
		entry.stale = true;
	}
}

void MDCommandBuffer::_begin() {
	DEV_ASSERT(!commandBuffer && !state_begin);
	state_begin = true;
	binding_cache.clear();
	_scratch.reset();
	inline_render.reset();
	release_resources();
	// GodotNext: metal_acceleration_structure
	_free_as_staging();
	// GodotNext: rd_indirect_count (#177)
	_indirect_count_clamps.clear();
	_indirect_count_clamps_used = 0;
	_indirect_count_clamp_fenced = false;
	// GodotNext: metal_ray_shaders (#172)
	raytracing.reset();
}

MDCommandBuffer::Alloc MDCommandBuffer::allocate_arg_buffer(uint32_t p_size) {
	return _scratch.allocate(p_size);
}

void MDCommandBuffer::_end() {
	switch (type) {
		case MDCommandBufferStateType::None:
			return;
		case MDCommandBufferStateType::Render:
			return render_end_pass();
		case MDCommandBufferStateType::InlineRender:
			return _end_inline_render();
		case MDCommandBufferStateType::Compute:
			return _end_compute_dispatch();
		case MDCommandBufferStateType::Blit:
			return _end_blit();
		// GodotNext: metal_acceleration_structure
		case MDCommandBufferStateType::AccelerationStructure:
			return _end_as();
	}
}

void MDCommandBuffer::_commit() {
	end();
	if (commandBuffer) {
		commandBuffer->commit();
		commandBuffer.reset();
	}
	state_begin = false;
}

MTL::CommandBuffer *MDCommandBuffer::command_buffer() {
	DEV_ASSERT(state_begin);
	if (!commandBuffer) {
		commandBuffer = NS::RetainPtr(queue->commandBuffer());
	}
	return commandBuffer.get();
}

void MDCommandBuffer::pipeline_barrier(BitField<RDD::PipelineStageBits> p_src_stages,
		BitField<RDD::PipelineStageBits> p_dst_stages,
		VectorView<RDD::MemoryAccessBarrier> p_memory_barriers,
		VectorView<RDD::BufferBarrier> p_buffer_barriers,
		VectorView<RDD::TextureBarrier> p_texture_barriers,
		VectorView<RDD::AccelerationStructureBarrier> p_acceleration_structure_barriers) {
	// Nothing to do. Ordering between levels is carried by the level fences,
	// which are driven by command_group_begin/command_group_end. The only live
	// caller for Metal is the render graph's _group_barriers_for_render_commands;
	// the transfer-worker calls in rendering_device.cpp are gated on
	// API_TRAIT_TEXTURES_REQUIRE_LAYOUT_TRANSITIONS, which Metal returns false
	// for, and RenderingDevice exposes no public barrier API.
	// GodotNext: metal_acceleration_structure
	DEV_ASSERT(!(render.encoder || compute.encoder || blit.encoder || inline_render.encoder || as.encoder));
}

#pragma mark - Timestamp

// metal-cpp does not expose MTLCounterDontSample or MTLCounterErrorValue.
static constexpr NS::UInteger COUNTER_DONT_SAMPLE = NS::UIntegerMax;
static constexpr uint64_t COUNTER_ERROR_VALUE = UINT64_MAX;

QueryPool *QueryPool::create(MTL::Device *p_device, uint32_t p_count) {
	MTL::CounterSet *counter_set = nullptr;
	NS::Array *counter_sets = p_device->counterSets();
	for (NS::UInteger i = 0; i < counter_sets->count(); i++) {
		MTL::CounterSet *cs = counter_sets->object<MTL::CounterSet>(i);
		if (cs->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
			counter_set = cs;
			break;
		}
	}
	if (counter_set == nullptr) {
		return nullptr;
	}

	// Metal limits a single MTLCounterSampleBuffer to 32768 bytes (4096 8-byte timestamp samples).
	// Clamp requested counts so projects configured with larger Vulkan/D3D12 pools (such as
	// max_timestamp_query_elements=65535) keep GPU timestamps active on Metal.
	constexpr uint32_t MAX_METAL_TIMESTAMP_SAMPLES = 32768u / uint32_t(sizeof(MTL::CounterResultTimestamp));
	const uint32_t sample_count = MIN(MAX(p_count, 1u), MAX_METAL_TIMESTAMP_SAMPLES);

	NS::SharedPtr<MTL::CounterSampleBufferDescriptor> desc = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
	desc->setCounterSet(counter_set);
	// Shared storage makes the samples resolvable on the CPU, so no blit pass is
	// needed to convert them.
	desc->setStorageMode(MTL::StorageModeShared);
	desc->setSampleCount(sample_count);

	NS::Error *error = nullptr;
	NS::SharedPtr<MTL::CounterSampleBuffer> sample_buffer = NS::TransferPtr(p_device->newCounterSampleBuffer(desc.get(), &error));
	ERR_FAIL_COND_V_MSG(error != nullptr, nullptr, vformat("Failed to create counter sample buffer: %s", String(error->localizedDescription()->utf8String())));

	QueryPool *pool = memnew(QueryPool);
	pool->sample_buffer = sample_buffer;
	pool->dummy_buffer = NS::TransferPtr(p_device->newBuffer(1, MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked));
	pool->device = p_device;
	p_device->sampleTimestamps(&pool->cpu_base, &pool->gpu_base);
	pool->count = sample_count;
	return pool;
}

double QueryPool::_resolve_timestamp_to_nano() {
	if (timestamp_to_nano != 0.0) {
		return timestamp_to_nano;
	}

	MTL::Timestamp cpu_now = 0;
	MTL::Timestamp gpu_now = 0;
	device->sampleTimestamps(&cpu_now, &gpu_now);
	if (gpu_now <= gpu_base || cpu_now <= cpu_base) {
		return 0.0; // Too soon to correlate; try again on the next readback.
	}
	timestamp_to_nano = (double)(cpu_now - cpu_base) / (double)(gpu_now - gpu_base);
	return timestamp_to_nano;
}

void QueryPool::get_results(uint32_t p_count, uint64_t *r_results) {
	double to_nano = _resolve_timestamp_to_nano();
	if (to_nano == 0.0) {
		memset(r_results, 0, p_count * sizeof(uint64_t));
		return;
	}

	const uint32_t resolve_count = MIN(p_count, count);
	NS::Data *data = sample_buffer->resolveCounterRange(NS::Range::Make(0, resolve_count));
	ERR_FAIL_NULL(data);
	const MTL::CounterResultTimestamp *src = (const MTL::CounterResultTimestamp *)data->bytes();
	uint64_t last = 0;
	for (uint32_t i = 0; i < resolve_count; i++) {
		// A slot the GPU never wrote resolves to the error value; hold the previous
		// reading so the profiler reports a zero length span rather than garbage.
		last = src[i].timestamp == COUNTER_ERROR_VALUE ? last : uint64_t((double)src[i].timestamp * to_nano);
		r_results[i] = last;
	}
	for (uint32_t i = resolve_count; i < p_count; i++) {
		r_results[i] = last;
	}
}

void MDCommandBuffer::timestamp_write(QueryPool *p_pool, uint32_t p_index) {
	if (p_index >= p_pool->get_count()) {
		return;
	}
	end();

	NS::SharedPtr<MTL::BlitPassDescriptor> desc = NS::TransferPtr(MTL::BlitPassDescriptor::alloc()->init());
	MTL::BlitPassSampleBufferAttachmentDescriptor *sba = desc->sampleBufferAttachments()->object(0);
	sba->setSampleBuffer(p_pool->get_sample_buffer());
	sba->setStartOfEncoderSampleIndex(p_index);
	sba->setEndOfEncoderSampleIndex(COUNTER_DONT_SAMPLE);

	// A self contained encoder: the state machine stays in the None state, so this
	// must not go through _ensure_blit_encoder(), which cannot take a descriptor.
	MTL::BlitCommandEncoder *enc = command_buffer()->blitCommandEncoder(desc.get());
#ifdef DEV_ENABLED
	enc->setLabel(NS::String::string(vformat("Timestamp %03d", p_index).utf8().get_data(), NS::UTF8StringEncoding));
#endif
	_fence_wait(enc);
	// An encoder with no commands may be elided, which would drop the sample.
	enc->fillBuffer(p_pool->get_dummy_buffer(), NS::Range::Make(0, 1), 0);
	_fence_update(enc);
	enc->endEncoding();
}

void MDCommandBuffer::bind_pipeline(RDD::PipelineID p_pipeline) {
	MDPipeline *p = (MDPipeline *)(p_pipeline.id);

	// End the current encoder if the new pipeline requires a new encoder type.
	if (type == MDCommandBufferStateType::InlineRender) {
		_end_inline_render();
	} else if (type == MDCommandBufferStateType::Blit) {
		_end_blit();
	} else if (type == MDCommandBufferStateType::AccelerationStructure) {
		// GodotNext: metal_acceleration_structure
		_end_as();
	}

	if (p->type == MDPipelineType::Render) {
		DEV_ASSERT(type == MDCommandBufferStateType::Render);
		MDRenderPipeline *rp = (MDRenderPipeline *)p;

		if (!render.encoder) {
			// This error would happen if the render pass failed.
			ERR_FAIL_NULL_MSG(render.desc.get(), "Render pass descriptor is null.");

			// This condition occurs when there are no attachments when calling render_next_subpass()
			// and is due to the SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS flag.
			render.desc->setDefaultRasterSampleCount(static_cast<NS::UInteger>(rp->sample_count));

			render.encoder = NS::RetainPtr(command_buffer()->renderCommandEncoder(render.desc.get()));
			render.clear_arg_buffer_cache();
			render.encoder_raster = MDRenderPipeline::RasterState();
			_encode_residency(render.encoder.get());
			// GodotNext: rd_mesh_shaders (#178)
			render.encoder_wait_fence = _fence_wait(render.encoder.get());
			render.mesh_stages_encoded = false;
		}

		if (render.pipeline != rp) {
			if (render.pipeline && render.pipeline->shader != rp->shader) {
				// Push constants belong to the previous shader; RD requires them to be supplied again.
				render.dirty.clear_flag(RenderState::DIRTY_PUSH);
				push_constant_binding = MetalPushConstantBinding();
			}
			render.dirty.set_flag((RenderState::DirtyFlag)(RenderState::DIRTY_PIPELINE | RenderState::DIRTY_RASTER));
			// Mark all uniforms as dirty, as variants of a shader pipeline may have a different entry point ABI,
			// due to setting force_active_argument_buffer_resources = true for spirv_cross::CompilerMSL::Options.
			//
			// binding_cache is kept, so unchanged bindings are not reissued. Slots written outside the
			// cache (push constants, view mask) are invalidated at the write site in _render_set_dirty_state.
			render.mark_uniforms_dirty();
			render.pipeline = rp;
			render.raster_state = rp->raster_state;
		}
		// GodotNext: rd_mesh_shaders (#178)
		if (rp->shader->is_mesh()) {
			_encode_mesh_stages();
		}
	} else if (p->type == MDPipelineType::Compute) {
		DEV_ASSERT(type == MDCommandBufferStateType::Compute);

		MDComputePipeline *cp = (MDComputePipeline *)p;
		if (compute.pipeline != cp) {
			if (compute.pipeline && compute.pipeline->shader != cp->shader) {
				// Push constants belong to the previous shader; RD requires them to be supplied again.
				compute.dirty.clear_flag(ComputeState::DIRTY_PUSH);
				push_constant_binding = MetalPushConstantBinding();
			}
			compute.dirty.set_flag(ComputeState::DIRTY_PIPELINE);
			binding_cache.clear();
			compute.mark_uniforms_dirty();
			compute.pipeline = cp;
		}
	}
}

void MDCommandBuffer::mark_push_constants_dirty() {
	switch (type) {
		case MDCommandBufferStateType::Render:
			render.dirty.set_flag(RenderState::DirtyFlag::DIRTY_PUSH);
			break;
		case MDCommandBufferStateType::Compute:
			compute.dirty.set_flag(ComputeState::DirtyFlag::DIRTY_PUSH);
			break;
		default:
			break;
	}
}

MTL::BlitCommandEncoder *MDCommandBuffer::_ensure_blit_encoder() {
	switch (type) {
		case MDCommandBufferStateType::None:
			break;
		case MDCommandBufferStateType::Render:
			render_end_pass();
			break;
		case MDCommandBufferStateType::InlineRender:
			_end_inline_render();
			break;
		case MDCommandBufferStateType::Compute:
			_end_compute_dispatch();
			break;
		case MDCommandBufferStateType::Blit:
			return blit.encoder.get();
		// GodotNext: metal_acceleration_structure
		case MDCommandBufferStateType::AccelerationStructure:
			_end_as();
			break;
	}

	type = MDCommandBufferStateType::Blit;
	blit.encoder = NS::RetainPtr(command_buffer()->blitCommandEncoder());
	_fence_wait(blit.encoder.get());

	return blit.encoder.get();
}

void MDCommandBuffer::resolve_texture(RDD::TextureID p_src_texture, RDD::TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, RDD::TextureID p_dst_texture, RDD::TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) {
	MTL::Texture *src_tex = rid::get<RDM::TextureInfo>(p_src_texture)->texture.get();
	MTL::Texture *dst_tex = rid::get<RDM::TextureInfo>(p_dst_texture)->texture.get();

	NS::SharedPtr<MTL::RenderPassDescriptor> mtlRPD = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
	MTL::RenderPassColorAttachmentDescriptor *mtlColorAttDesc = mtlRPD->colorAttachments()->object(0);
	mtlColorAttDesc->setLoadAction(MTL::LoadActionLoad);
	mtlColorAttDesc->setStoreAction(MTL::StoreActionMultisampleResolve);

	mtlColorAttDesc->setTexture(src_tex);
	mtlColorAttDesc->setResolveTexture(dst_tex);
	mtlColorAttDesc->setLevel(p_src_mipmap);
	mtlColorAttDesc->setSlice(p_src_layer);
	mtlColorAttDesc->setResolveLevel(p_dst_mipmap);
	mtlColorAttDesc->setResolveSlice(p_dst_layer);
	MTL::RenderCommandEncoder *enc = get_new_render_encoder_with_descriptor(mtlRPD.get());
	enc->setLabel(MTLSTR("Resolve Image"));
	_set_inline_render_encoder(enc);
}

void MDCommandBuffer::clear_color_texture(RDD::TextureID p_texture, RDD::TextureLayout p_texture_layout, const Color &p_color, const RDD::TextureSubresourceRange &p_subresources) {
	MTL::Texture *src_tex = rid::get<RDM::TextureInfo>(p_texture)->texture.get();

	if (src_tex->parentTexture()) {
		// Clear via the parent texture rather than the view.
		src_tex = src_tex->parentTexture();
	}

	PixelFormats &pf = device_driver->get_pixel_formats();

	if (pf.isDepthFormat(src_tex->pixelFormat()) || pf.isStencilFormat(src_tex->pixelFormat())) {
		ERR_FAIL_MSG("invalid: depth or stencil texture format");
	}

	NS::SharedPtr<MTL::RenderPassDescriptor> desc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());

	if (p_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_COLOR_BIT)) {
		MTL::RenderPassColorAttachmentDescriptor *caDesc = desc->colorAttachments()->object(0);
		caDesc->setTexture(src_tex);
		caDesc->setLoadAction(MTL::LoadActionClear);
		caDesc->setStoreAction(MTL::StoreActionStore);
		caDesc->setClearColor(MTL::ClearColor(p_color.r, p_color.g, p_color.b, p_color.a));

		// Extract the mipmap levels that are to be updated.
		uint32_t mipLvlStart = p_subresources.base_mipmap;
		uint32_t mipLvlCnt = p_subresources.mipmap_count;
		uint32_t mipLvlEnd = mipLvlStart + mipLvlCnt;

		NS::UInteger levelCount = src_tex->mipmapLevelCount();

		// Extract the cube or array layers (slices) that are to be updated.
		bool is3D = src_tex->textureType() == MTL::TextureType3D;
		uint32_t layerStart = is3D ? 0 : p_subresources.base_layer;
		uint32_t layerCnt = p_subresources.layer_count;
		uint32_t layerEnd = layerStart + layerCnt;

		const MetalFeatures &features = device_driver->get_device_properties().features;

		// Iterate across mipmap levels and layers, and perform an empty render to clear each.
		for (uint32_t mipLvl = mipLvlStart; mipLvl < mipLvlEnd; mipLvl++) {
			ERR_FAIL_INDEX_MSG(mipLvl, levelCount, "mip level out of range");

			caDesc->setLevel(mipLvl);

			// If a 3D image, we need to get the depth for each level.
			if (is3D) {
				layerCnt = mipmapLevelSizeFromTexture(src_tex, mipLvl).depth;
				layerEnd = layerStart + layerCnt;
			}

			if ((features.layeredRendering && src_tex->sampleCount() == 1) || features.multisampleLayeredRendering) {
				// We can clear all layers at once.
				if (is3D) {
					caDesc->setDepthPlane(layerStart);
				} else {
					caDesc->setSlice(layerStart);
				}
				desc->setRenderTargetArrayLength(layerCnt);
				MTL::RenderCommandEncoder *enc = get_new_render_encoder_with_descriptor(desc.get());
				enc->setLabel(MTLSTR("Clear Image"));
				if (mipLvl + 1 == mipLvlEnd) {
					_set_inline_render_encoder(enc);
				} else {
					_fence_update(enc);
					enc->endEncoding();
				}
			} else {
				for (uint32_t layer = layerStart; layer < layerEnd; layer++) {
					if (is3D) {
						caDesc->setDepthPlane(layer);
					} else {
						caDesc->setSlice(layer);
					}
					MTL::RenderCommandEncoder *enc = get_new_render_encoder_with_descriptor(desc.get());
					enc->setLabel(MTLSTR("Clear Image"));
					if (mipLvl + 1 == mipLvlEnd && layer + 1 == layerEnd) {
						_set_inline_render_encoder(enc);
					} else {
						_fence_update(enc);
						enc->endEncoding();
					}
				}
			}
		}
	}
}

void MDCommandBuffer::clear_buffer(RDD::BufferID p_buffer, uint64_t p_offset, uint64_t p_size) {
	MTL::BlitCommandEncoder *blit_enc = _ensure_blit_encoder();
	const RDM::BufferInfo *buffer = (const RDM::BufferInfo *)p_buffer.id;

	blit_enc->fillBuffer(buffer->buffer.get(), NS::Range(p_offset, p_size), 0);
}

void MDCommandBuffer::clear_depth_stencil_texture(RDD::TextureID p_texture, RDD::TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const RDD::TextureSubresourceRange &p_subresources) {
	MTL::Texture *src_tex = rid::get<RDM::TextureInfo>(p_texture)->texture.get();

	if (src_tex->parentTexture()) {
		// Clear via the parent texture rather than the view.
		src_tex = src_tex->parentTexture();
	}

	PixelFormats &pf = device_driver->get_pixel_formats();

	bool is_depth_format = pf.isDepthFormat(src_tex->pixelFormat());
	bool is_stencil_format = pf.isStencilFormat(src_tex->pixelFormat());

	if (!is_depth_format && !is_stencil_format) {
		ERR_FAIL_MSG("invalid: color texture format");
	}

	bool clear_depth = is_depth_format && p_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_DEPTH_BIT);
	bool clear_stencil = is_stencil_format && p_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_STENCIL_BIT);

	if (clear_depth || clear_stencil) {
		NS::SharedPtr<MTL::RenderPassDescriptor> desc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());

		MTL::RenderPassDepthAttachmentDescriptor *daDesc = desc->depthAttachment();
		if (clear_depth) {
			daDesc->setTexture(src_tex);
			daDesc->setLoadAction(MTL::LoadActionClear);
			daDesc->setStoreAction(MTL::StoreActionStore);
			daDesc->setClearDepth(p_depth);
		}

		MTL::RenderPassStencilAttachmentDescriptor *saDesc = desc->stencilAttachment();
		if (clear_stencil) {
			saDesc->setTexture(src_tex);
			saDesc->setLoadAction(MTL::LoadActionClear);
			saDesc->setStoreAction(MTL::StoreActionStore);
			saDesc->setClearStencil(p_stencil);
		}

		// Extract the mipmap levels that are to be updated.
		uint32_t mipLvlStart = p_subresources.base_mipmap;
		uint32_t mipLvlCnt = p_subresources.mipmap_count;
		uint32_t mipLvlEnd = mipLvlStart + mipLvlCnt;

		uint32_t levelCount = src_tex->mipmapLevelCount();

		// Extract the cube or array layers (slices) that are to be updated.
		bool is3D = src_tex->textureType() == MTL::TextureType3D;
		uint32_t layerStart = is3D ? 0 : p_subresources.base_layer;
		uint32_t layerCnt = p_subresources.layer_count;
		uint32_t layerEnd = layerStart + layerCnt;

		const MetalFeatures &features = device_driver->get_device_properties().features;

		// Iterate across mipmap levels and layers, and perform and empty render to clear each.
		for (uint32_t mipLvl = mipLvlStart; mipLvl < mipLvlEnd; mipLvl++) {
			ERR_FAIL_INDEX_MSG(mipLvl, levelCount, "mip level out of range");

			if (clear_depth) {
				daDesc->setLevel(mipLvl);
			}
			if (clear_stencil) {
				saDesc->setLevel(mipLvl);
			}

			// If a 3D image, we need to get the depth for each level.
			if (is3D) {
				layerCnt = mipmapLevelSizeFromTexture(src_tex, mipLvl).depth;
				layerEnd = layerStart + layerCnt;
			}

			if ((features.layeredRendering && src_tex->sampleCount() == 1) || features.multisampleLayeredRendering) {
				// We can clear all layers at once.
				if (is3D) {
					if (clear_depth) {
						daDesc->setDepthPlane(layerStart);
					}
					if (clear_stencil) {
						saDesc->setDepthPlane(layerStart);
					}
				} else {
					if (clear_depth) {
						daDesc->setSlice(layerStart);
					}
					if (clear_stencil) {
						saDesc->setSlice(layerStart);
					}
				}
				desc->setRenderTargetArrayLength(layerCnt);
				MTL::RenderCommandEncoder *enc = get_new_render_encoder_with_descriptor(desc.get());
				enc->setLabel(MTLSTR("Clear Image"));
				if (mipLvl + 1 == mipLvlEnd) {
					_set_inline_render_encoder(enc);
				} else {
					_fence_update(enc);
					enc->endEncoding();
				}
			} else {
				for (uint32_t layer = layerStart; layer < layerEnd; layer++) {
					if (is3D) {
						if (clear_depth) {
							daDesc->setDepthPlane(layer);
						}
						if (clear_stencil) {
							saDesc->setDepthPlane(layer);
						}
					} else {
						if (clear_depth) {
							daDesc->setSlice(layer);
						}
						if (clear_stencil) {
							saDesc->setSlice(layer);
						}
					}
					MTL::RenderCommandEncoder *enc = get_new_render_encoder_with_descriptor(desc.get());
					enc->setLabel(MTLSTR("Clear Image"));
					if (mipLvl + 1 == mipLvlEnd && layer + 1 == layerEnd) {
						_set_inline_render_encoder(enc);
					} else {
						_fence_update(enc);
						enc->endEncoding();
					}
				}
			}
		}
	}
}

void MDCommandBuffer::copy_buffer(RDD::BufferID p_src_buffer, RDD::BufferID p_dst_buffer, VectorView<RDD::BufferCopyRegion> p_regions) {
	const RDM::BufferInfo *src = (const RDM::BufferInfo *)p_src_buffer.id;
	const RDM::BufferInfo *dst = (const RDM::BufferInfo *)p_dst_buffer.id;

	device_driver->sbt_shadow_copy(src, dst, p_regions);

	MTL::BlitCommandEncoder *enc = _ensure_blit_encoder();

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		RDD::BufferCopyRegion region = p_regions[i];
		enc->copyFromBuffer(src->buffer.get(), region.src_offset,
				dst->buffer.get(), region.dst_offset, region.size);
	}
}

void MDCommandBuffer::encode_texture_subregion_copy(MTL::Texture *p_src, MTL::Texture *p_dst, uint32_t p_width, uint32_t p_height) {
	MTL::BlitCommandEncoder *enc = _ensure_blit_encoder();
	enc->copyFromTexture(p_src, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size::Make(p_width, p_height, 1), p_dst, 0, 0, MTL::Origin::Make(0, 0, 0));
}

void MDCommandBuffer::copy_texture(RDD::TextureID p_src_texture, RDD::TextureID p_dst_texture, VectorView<RDD::TextureCopyRegion> p_regions) {
	MTL::Texture *src = rid::get<RDM::TextureInfo>(p_src_texture)->texture.get();
	MTL::Texture *dst = rid::get<RDM::TextureInfo>(p_dst_texture)->texture.get();

	MTL::BlitCommandEncoder *enc = _ensure_blit_encoder();
	PixelFormats &pf = device_driver->get_pixel_formats();

	MTL::PixelFormat src_fmt = src->pixelFormat();
	bool src_is_compressed = pf.getFormatType(src_fmt) == MTLFormatType::Compressed;
	MTL::PixelFormat dst_fmt = dst->pixelFormat();
	bool dst_is_compressed = pf.getFormatType(dst_fmt) == MTLFormatType::Compressed;

	// Validate copy.
	if (src->sampleCount() != dst->sampleCount() || pf.getBytesPerBlock(src_fmt) != pf.getBytesPerBlock(dst_fmt)) {
		ERR_FAIL_MSG("Cannot copy between incompatible pixel formats, such as formats of different pixel sizes, or between images with different sample counts.");
	}

	// If source and destination have different formats and at least one is compressed, a temporary buffer is required.
	bool need_tmp_buffer = (src_fmt != dst_fmt) && (src_is_compressed || dst_is_compressed);
	if (need_tmp_buffer) {
		ERR_FAIL_MSG("not implemented: copy with intermediate buffer");
	}

	NS::SharedPtr<MTL::Texture> src_view;
	if (src_fmt != dst_fmt) {
		src_view = NS::TransferPtr(src->newTextureView(dst_fmt));
		src = src_view.get();
	}

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		RDD::TextureCopyRegion region = p_regions[i];

		MTL::Size extent = MTLSizeFromVector3i(region.size);

		// If copies can be performed using direct texture-texture copying, do so.
		uint32_t src_level = region.src_subresources.mipmap;
		uint32_t src_base_layer = region.src_subresources.base_layer;
		MTL::Size src_extent = mipmapLevelSizeFromTexture(src, src_level);
		uint32_t dst_level = region.dst_subresources.mipmap;
		uint32_t dst_base_layer = region.dst_subresources.base_layer;
		MTL::Size dst_extent = mipmapLevelSizeFromTexture(dst, dst_level);

		// All layers may be copied at once, if the extent completely covers both images.
		if (src_extent == extent && dst_extent == extent) {
			enc->copyFromTexture(src, src_base_layer, src_level,
					dst, dst_base_layer, dst_level,
					region.src_subresources.layer_count, 1);
		} else {
			MTL::Origin src_origin = MTLOriginFromVector3i(region.src_offset);
			MTL::Size src_size = clampMTLSize(extent, src_origin, src_extent);
			uint32_t layer_count = 0;
			if ((src->textureType() == MTL::TextureType3D) != (dst->textureType() == MTL::TextureType3D)) {
				// In the case, the number of layers to copy is in extent.depth. Use that value,
				// then clamp the depth, so we don't try to copy more than Metal will allow.
				layer_count = extent.depth;
				src_size.depth = 1;
			} else {
				layer_count = region.src_subresources.layer_count;
			}
			MTL::Origin dst_origin = MTLOriginFromVector3i(region.dst_offset);

			for (uint32_t layer = 0; layer < layer_count; layer++) {
				// We can copy between a 3D and a 2D image easily. Just copy between
				// one slice of the 2D image and one plane of the 3D image at a time.
				if ((src->textureType() == MTL::TextureType3D) == (dst->textureType() == MTL::TextureType3D)) {
					enc->copyFromTexture(src, src_base_layer + layer, src_level, src_origin, src_size,
							dst, dst_base_layer + layer, dst_level, dst_origin);
				} else if (src->textureType() == MTL::TextureType3D) {
					enc->copyFromTexture(src, src_base_layer, src_level,
							MTL::Origin(src_origin.x, src_origin.y, src_origin.z + layer), src_size,
							dst, dst_base_layer + layer, dst_level, dst_origin);
				} else {
					DEV_ASSERT(dst->textureType() == MTL::TextureType3D);
					enc->copyFromTexture(src, src_base_layer + layer, src_level, src_origin, src_size,
							dst, dst_base_layer, dst_level,
							MTL::Origin(dst_origin.x, dst_origin.y, dst_origin.z + layer));
				}
			}
		}
	}
}

void MDCommandBuffer::copy_buffer_to_texture(RDD::BufferID p_src_buffer, RDD::TextureID p_dst_texture, VectorView<RDD::BufferTextureCopyRegion> p_regions) {
	_copy_texture_buffer(CopySource::Buffer, p_dst_texture, p_src_buffer, p_regions);
}

void MDCommandBuffer::copy_texture_to_buffer(RDD::TextureID p_src_texture, RDD::BufferID p_dst_buffer, VectorView<RDD::BufferTextureCopyRegion> p_regions) {
	_copy_texture_buffer(CopySource::Texture, p_src_texture, p_dst_buffer, p_regions);
}

void MDCommandBuffer::_copy_texture_buffer(CopySource p_source,
		RDD::TextureID p_texture,
		RDD::BufferID p_buffer,
		VectorView<RDD::BufferTextureCopyRegion> p_regions) {
	const RDM::BufferInfo *buffer = (const RDM::BufferInfo *)p_buffer.id;
	MTL::Texture *texture = rid::get<RDM::TextureInfo>(p_texture)->texture.get();

	MTL::BlitCommandEncoder *enc = _ensure_blit_encoder();

	PixelFormats &pf = device_driver->get_pixel_formats();
	MTL::PixelFormat mtlPixFmt = texture->pixelFormat();

	MTL::BlitOption options = MTL::BlitOptionNone;
	if (pf.isPVRTCFormat(mtlPixFmt)) {
		options |= MTL::BlitOptionRowLinearPVRTC;
	}

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		RDD::BufferTextureCopyRegion region = p_regions[i];

		uint32_t mip_level = region.texture_subresource.mipmap;
		MTL::Origin txt_origin = MTL::Origin(region.texture_offset.x, region.texture_offset.y, region.texture_offset.z);
		MTL::Size src_extent = mipmapLevelSizeFromTexture(texture, mip_level);
		MTL::Size txt_size = clampMTLSize(MTL::Size(region.texture_region_size.x, region.texture_region_size.y, region.texture_region_size.z),
				txt_origin,
				src_extent);

		uint32_t buffImgWd = region.texture_region_size.x;
		uint32_t buffImgHt = region.texture_region_size.y;

		NS::UInteger bytesPerRow = pf.getBytesPerRow(mtlPixFmt, buffImgWd);
		NS::UInteger bytesPerImg = pf.getBytesPerLayer(mtlPixFmt, bytesPerRow, buffImgHt);

		MTL::BlitOption blit_options = options;

		if (pf.isDepthFormat(mtlPixFmt) && pf.isStencilFormat(mtlPixFmt)) {
			// Don't reduce depths of 32-bit depth/stencil formats.
			if (region.texture_subresource.aspect == RDD::TEXTURE_ASPECT_DEPTH) {
				if (pf.getBytesPerTexel(mtlPixFmt) != 4) {
					bytesPerRow -= buffImgWd;
					bytesPerImg -= buffImgWd * buffImgHt;
				}
				blit_options |= MTL::BlitOptionDepthFromDepthStencil;
			} else if (region.texture_subresource.aspect == RDD::TEXTURE_ASPECT_STENCIL) {
				// The stencil component is always 1 byte per pixel.
				bytesPerRow = buffImgWd;
				bytesPerImg = buffImgWd * buffImgHt;
				blit_options |= MTL::BlitOptionStencilFromDepthStencil;
			}
		}

		if (!isArrayTexture(texture->textureType())) {
			bytesPerImg = 0;
		}

		if (p_source == CopySource::Buffer) {
			enc->copyFromBuffer(buffer->buffer.get(), region.buffer_offset,
					bytesPerRow, bytesPerImg, txt_size,
					texture, region.texture_subresource.layer, mip_level, txt_origin, blit_options);
		} else {
			enc->copyFromTexture(texture, region.texture_subresource.layer, mip_level,
					txt_origin, txt_size,
					buffer->buffer.get(), region.buffer_offset,
					bytesPerRow, bytesPerImg, blit_options);
		}
	}
}

MTL::RenderCommandEncoder *MDCommandBuffer::get_new_render_encoder_with_descriptor(MTL::RenderPassDescriptor *p_desc) {
	switch (type) {
		case MDCommandBufferStateType::None:
			break;
		case MDCommandBufferStateType::Render:
			render_end_pass();
			break;
		case MDCommandBufferStateType::InlineRender:
			_end_inline_render();
			break;
		case MDCommandBufferStateType::Compute:
			_end_compute_dispatch();
			break;
		case MDCommandBufferStateType::Blit:
			_end_blit();
			break;
		// GodotNext: metal_acceleration_structure
		case MDCommandBufferStateType::AccelerationStructure:
			_end_as();
			break;
	}

	MTL::RenderCommandEncoder *enc = command_buffer()->renderCommandEncoder(p_desc);
	_encode_residency(enc);
	_fence_wait(enc);
	return enc;
}

#pragma mark - Render Commands

void MDCommandBuffer::render_bind_uniform_sets(VectorView<RDD::UniformSetID> p_uniform_sets, RDD::ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);

	if (uint32_t new_size = p_first_set_index + p_set_count; render.uniform_sets.size() < new_size) {
		uint32_t s = render.uniform_sets.size();
		render.uniform_sets.resize(new_size);
		// Set intermediate values to null.
		std::fill(&render.uniform_sets[s], render.uniform_sets.end().operator->(), nullptr);
	}

	const MDShader *shader = (const MDShader *)p_shader.id;
	DynamicOffsetLayout layout = shader->dynamic_offset_layout;

	// Clear bits for sets being bound, then OR new values.
	for (uint32_t i = 0; i < p_set_count && render.dynamic_offsets != 0; i++) {
		uint32_t set_index = p_first_set_index + i;
		uint32_t count = layout.get_count(set_index);
		if (count > 0) {
			uint32_t shift = layout.get_offset_index_shift(set_index);
			uint32_t mask = ((1u << (count * 4u)) - 1u) << shift;
			render.dynamic_offsets &= ~mask; // Clear this set's bits
		}
	}
	render.dynamic_offsets |= p_dynamic_offsets;

	for (size_t i = 0; i < p_set_count; ++i) {
		MDUniformSet *set = (MDUniformSet *)(p_uniform_sets[i].id);

		uint32_t index = p_first_set_index + i;
		if (render.uniform_sets[index] != set || layout.get_count(index) > 0) {
			render.dirty.set_flag(RenderState::DIRTY_UNIFORMS);
			render.uniform_set_mask |= 1ULL << index;
			render.uniform_sets[index] = set;
		}
	}
}

void MDCommandBuffer::render_clear_attachments(VectorView<RDD::AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);

	const MDSubpass &subpass = render.get_subpass();

	uint32_t vertex_count = p_rects.size() * 6 * subpass.view_count;
	simd::float4 *vertices = ALLOCA_ARRAY(simd::float4, vertex_count);
	simd::float4 clear_colors[ClearAttKey::ATTACHMENT_COUNT];

	Size2i size = render.frameBuffer->size;
	Rect2i render_area = render.clip_to_render_area({ { 0, 0 }, size });
	size = Size2i(render_area.position.x + render_area.size.width, render_area.position.y + render_area.size.height);
	_populate_vertices(vertices, size, p_rects);

	ClearAttKey key;
	key.sample_count = render.pass->get_sample_count();
	if (subpass.view_count > 1) {
		key.enable_layered_rendering();
	}

	// GodotNext: rd_clear_values (#275). Populate pixel formats for every active attachment in the
	// subpass so Metal's render pipeline descriptor matches the framebuffer even on attachments that
	// are loaded rather than cleared (those keep MTL::ColorWriteMaskNone via clear_mask).
	for (uint32_t c = 0; c < subpass.color_references.size(); c++) {
		uint32_t att_idx = subpass.color_references[c].attachment;
		if (att_idx != RDD::AttachmentReference::UNUSED) {
			key.set_color_format(c, render.pass->attachments[att_idx].format);
		}
	}
	if (subpass.depth_stencil_reference.attachment != RDD::AttachmentReference::UNUSED) {
		const MDAttachment &ds_att = render.pass->attachments[subpass.depth_stencil_reference.attachment];
		if (ds_att.type & MDAttachmentType::Depth) {
			key.set_depth_format(ds_att.format);
		}
		if (ds_att.type & MDAttachmentType::Stencil) {
			key.set_stencil_format(ds_att.format);
		}
	}

	float depth_value = 0;
	uint32_t stencil_value = 0;

	for (uint32_t i = 0; i < p_attachment_clears.size(); i++) {
		const RDD::AttachmentClear &attClear = p_attachment_clears[i];
		uint32_t attachment_index;
		if (attClear.aspect.has_flag(RDD::TEXTURE_ASPECT_COLOR_BIT)) {
			// GodotNext: rd_clear_values (#275). color_attachment is the subpass color slot, as on
			// Vulkan. The clear pipeline writes that output, where render_next_subpass() binds the
			// slot's render pass attachment.
			ERR_CONTINUE(attClear.color_attachment >= subpass.color_references.size());
			attachment_index = subpass.color_references[attClear.color_attachment].attachment;
			ERR_CONTINUE(attachment_index == RDD::AttachmentReference::UNUSED);
		} else {
			attachment_index = subpass.depth_stencil_reference.attachment;
		}

		const MDAttachment &mda = render.pass->attachments[attachment_index];
		if (attClear.aspect.has_flag(RDD::TEXTURE_ASPECT_COLOR_BIT)) {
			key.set_color_format(attClear.color_attachment, mda.format);
			key.enable_color_clear(attClear.color_attachment);

			clear_colors[attClear.color_attachment] = simd_make_float4(
					attClear.value.color.r,
					attClear.value.color.g,
					attClear.value.color.b,
					attClear.value.color.a);
		}

		if (attClear.aspect.has_flag(RDD::TEXTURE_ASPECT_DEPTH_BIT)) {
			key.set_depth_format(mda.format);
			key.enable_depth_clear();
			depth_value = attClear.value.depth;
		}

		if (attClear.aspect.has_flag(RDD::TEXTURE_ASPECT_STENCIL_BIT)) {
			key.set_stencil_format(mda.format);
			key.enable_stencil_clear();
			stencil_value = attClear.value.stencil;
		}
	}
	clear_colors[ClearAttKey::DEPTH_INDEX] = simd_make_float4(depth_value);

	MTL::RenderCommandEncoder *enc = render.encoder.get();

	MDResourceCache &cache = device_driver->get_resource_cache();

	enc->pushDebugGroup(MTLSTR("ClearAttachments"));
	enc->setRenderPipelineState(cache.get_clear_render_pipeline_state(key, nullptr));

	MDRenderPipeline::RasterState clear_state = render.encoder_raster;
	clear_state.depth_stencil = cache.get_depth_stencil_state(key.is_depth_clear_enabled(), key.is_stencil_clear_enabled());
	clear_state.cull_mode = MTL::CullModeNone;
	clear_state.fill_mode = MTL::TriangleFillModeFill;
	clear_state.depth_bias = MDRenderPipeline::RasterState::DepthBias();
	clear_state.stencil.enabled = true;
	clear_state.stencil.front_reference = stencil_value;
	clear_state.stencil.back_reference = stencil_value;
	clear_state.apply(enc, render.encoder_raster);

	enc->setViewport(MTL::Viewport{ 0, 0, (double)size.width, (double)size.height, 0.0, 1.0 });
	enc->setScissorRect(MTL::ScissorRect{ 0, 0, (NS::UInteger)size.width, (NS::UInteger)size.height });

	enc->setVertexBytes(clear_colors, sizeof(clear_colors), 0);
	if (key.has_color_clear()) {
		enc->setFragmentBytes(clear_colors, sizeof(clear_colors), 0);
	}
	enc->setVertexBytes(vertices, vertex_count * sizeof(vertices[0]), device_driver->get_metal_buffer_index_for_vertex_attribute_binding(VERT_CONTENT_BUFFER_INDEX));

	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, vertex_count);
	enc->popDebugGroup();

	render.dirty.set_flag((RenderState::DirtyFlag)(RenderState::DIRTY_PIPELINE | RenderState::DIRTY_RASTER));
	binding_cache.clear();
	render.clear_arg_buffer_cache();
	render.mark_uniforms_dirty({ 0 }); // Mark index 0 dirty, if there is already a binding for index 0.
	render.mark_viewport_dirty();
	render.mark_scissors_dirty();
	render.mark_vertex_dirty();
}

void MDCommandBuffer::_render_set_dirty_state() {
	_render_bind_uniform_sets();

	if (render.dirty.has_flag(RenderState::DIRTY_PUSH)) {
		uint32_t slot = push_constant_binding.get_binding();
		if (push_constant_binding.has(RDD::SHADER_STAGE_VERTEX)) {
			render.encoder->setVertexBytes(push_constant_data, push_constant_data_len, slot);
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_VERTEX].invalidate_buffer(slot);
		}
		if (push_constant_binding.has(RDD::SHADER_STAGE_FRAGMENT)) {
			render.encoder->setFragmentBytes(push_constant_data, push_constant_data_len, slot);
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_FRAGMENT].invalidate_buffer(slot);
		}
		// GodotNext: rd_mesh_shaders (#178)
		if (push_constant_binding.has(RDD::SHADER_STAGE_TASK)) {
			render.encoder->setObjectBytes(push_constant_data, push_constant_data_len, slot);
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_OBJECT].invalidate_buffer(slot);
		}
		if (push_constant_binding.has(RDD::SHADER_STAGE_MESH)) {
			render.encoder->setMeshBytes(push_constant_data, push_constant_data_len, slot);
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_MESH].invalidate_buffer(slot);
		}
		binding_cache.invalidate_buffer(slot);
	}

	const MDSubpass &subpass = render.get_subpass();
	if (subpass.view_count > 1) {
		uint32_t view_range[2] = { 0, subpass.view_count };
		render.encoder->setVertexBytes(view_range, sizeof(view_range), VIEW_MASK_BUFFER_INDEX);
		render.encoder->setFragmentBytes(view_range, sizeof(view_range), VIEW_MASK_BUFFER_INDEX);
		binding_cache.invalidate_buffer(VIEW_MASK_BUFFER_INDEX);
	}

	if (render.dirty.has_flag(RenderState::DIRTY_PIPELINE)) {
		render.encoder->setRenderPipelineState(render.pipeline->state.get());
	}

	if (render.dirty.has_flag(RenderState::DIRTY_VIEWPORT)) {
		render.encoder->setViewports(reinterpret_cast<const MTL::Viewport *>(render.viewports.ptr()), render.viewports.size());
	}

	if (render.dirty.has_flag(RenderState::DIRTY_RASTER)) {
		if (render.raster_state.blend.enabled && render.blend_constants.has_value()) {
			render.raster_state.blend.set_color(*render.blend_constants);
		}
		render.raster_state.apply(render.encoder.get(), render.encoder_raster);
	}

	if (render.dirty.has_flag(RenderState::DIRTY_SCISSOR) && !render.scissors.is_empty()) {
		size_t len = render.scissors.size();
		MTL::ScissorRect *rects = ALLOCA_ARRAY(MTL::ScissorRect, len);
		for (size_t i = 0; i < len; i++) {
			rects[i] = render.clip_to_render_area(render.scissors[i]);
		}
		render.encoder->setScissorRects(rects, len);
	}

	if (render.dirty.has_flag(RenderState::DIRTY_VERTEX)) {
		uint32_t p_binding_count = render.vertex_buffers.size();
		if (p_binding_count > 0) {
			uint32_t first = device_driver->get_metal_buffer_index_for_vertex_attribute_binding(p_binding_count - 1);
			render.encoder->setVertexBuffers(render.vertex_buffers.ptr(), render.vertex_offsets.ptr(), NS::Range(first, p_binding_count));
		}
	}

	// GodotNext: metal_shader_validation
	// useHeaps marks heap resources read-only. Argument-buffer resources still
	// need useResource with the usage the shader performs, or validation
	// reports invalid stores and non-resident textures.
	render.resource_tracker.encode(render.encoder.get());

	render.dirty.clear();
}

void ResourceTracker::merge_from(const ::ResourceUsageMap &p_from) {
	for (const KeyValue<StageResourceUsage, ::ResourceVector> &keyval : p_from) {
		ResourceVector *resources = _current.getptr(keyval.key);
		if (resources == nullptr) {
			resources = &_current.insert(keyval.key, ResourceVector())->value;
		}
		resources->reserve(resources->size() + keyval.value.size());

		MTL::Resource *const *keyval_ptr = (MTL::Resource *const *)(void *)keyval.value.ptr();

		// Helper to check if a resource needs to be added based on previous usage.
		auto should_add_resource = [this, usage = keyval.key](MTL::Resource *res) -> bool {
			ResourceUsageEntry *existing = _previous.getptr(res);
			if (existing == nullptr) {
				_previous.insert(res, usage);
				return true;
			}
			if (existing->usage != usage) {
				existing->usage |= usage;
				return true;
			}
			return false;
		};

		// 2-way merge of sorted resource lists.
		uint32_t i = 0, j = 0;
		while (i < resources->size() && j < keyval.value.size()) {
			MTL::Resource *current_res = resources->ptr()[i];
			MTL::Resource *new_res = keyval_ptr[j];

			if (current_res < new_res) {
				i++;
			} else if (current_res > new_res) {
				if (should_add_resource(new_res)) {
					resources->insert(i, new_res);
					i++;
				}
				j++;
			} else {
				i++;
				j++;
			}
		}

		// Append any remaining resources from the input.
		for (; j < keyval.value.size(); j++) {
			if (should_add_resource(keyval_ptr[j])) {
				resources->push_back(keyval_ptr[j]);
			}
		}
	}
}

void ResourceTracker::encode(MTL::RenderCommandEncoder *p_enc) {
	for (const KeyValue<StageResourceUsage, ResourceVector> &keyval : _current) {
		if (keyval.value.is_empty()) {
			continue;
		}

		MTL::ResourceUsage vert_usage = (MTL::ResourceUsage)resource_usage_for_stage(keyval.key, RDD::ShaderStage::SHADER_STAGE_VERTEX);
		MTL::ResourceUsage frag_usage = (MTL::ResourceUsage)resource_usage_for_stage(keyval.key, RDD::ShaderStage::SHADER_STAGE_FRAGMENT);
		// GodotNext: metal_shader_validation
		vert_usage = (MTL::ResourceUsage)metal_shader_validation_usage((uint32_t)vert_usage);
		frag_usage = (MTL::ResourceUsage)metal_shader_validation_usage((uint32_t)frag_usage);
		const MTL::Resource **resources = (const MTL::Resource **)(void *)keyval.value.ptr();
		NS::UInteger count = keyval.value.size();
		if (vert_usage == frag_usage) {
			p_enc->useResources(resources, count, vert_usage, MTL::RenderStageVertex | MTL::RenderStageFragment);
		} else {
			if (vert_usage != 0) {
				p_enc->useResources(resources, count, vert_usage, MTL::RenderStageVertex);
			}
			if (frag_usage != 0) {
				p_enc->useResources(resources, count, frag_usage, MTL::RenderStageFragment);
			}
		}
		// GodotNext: rd_mesh_shaders (#178). Resources of the object and mesh functions (only mesh pipelines have
		// these usages).
		MTL::ResourceUsage object_usage = (MTL::ResourceUsage)metal_shader_validation_usage((uint32_t)resource_usage_for_stage(keyval.key, RDD::ShaderStage::SHADER_STAGE_TASK));
		MTL::ResourceUsage mesh_usage = (MTL::ResourceUsage)metal_shader_validation_usage((uint32_t)resource_usage_for_stage(keyval.key, RDD::ShaderStage::SHADER_STAGE_MESH));
		if (object_usage != 0) {
			p_enc->useResources(resources, count, object_usage, MTL::RenderStageObject);
		}
		if (mesh_usage != 0) {
			p_enc->useResources(resources, count, mesh_usage, MTL::RenderStageMesh);
		}
	}

	// Keep the keys for now and clear the vectors to reduce churn.
	for (KeyValue<StageResourceUsage, ResourceVector> &v : _current) {
		v.value.clear();
	}
}

void ResourceTracker::encode(MTL::ComputeCommandEncoder *p_enc) {
	for (const KeyValue<StageResourceUsage, ResourceVector> &keyval : _current) {
		if (keyval.value.is_empty()) {
			continue;
		}
		MTL::ResourceUsage usage = (MTL::ResourceUsage)resource_usage_for_stage(keyval.key, RDD::ShaderStage::SHADER_STAGE_COMPUTE);
		// GodotNext: metal_shader_validation
		usage = (MTL::ResourceUsage)metal_shader_validation_usage((uint32_t)usage);
		if (usage != 0) {
			const MTL::Resource **resources = (const MTL::Resource **)(void *)keyval.value.ptr();
			p_enc->useResources(resources, keyval.value.size(), usage);
		}
	}

	// Keep the keys for now and clear the vectors to reduce churn.
	for (KeyValue<StageResourceUsage, ResourceVector> &v : _current) {
		v.value.clear();
	}
}

void ResourceTracker::reset() {
	// Keep the keys for now, as they are likely to be used repeatedly.
	for (KeyValue<MTL::Resource *, ResourceUsageEntry> &v : _previous) {
		if (v.value.usage == ResourceUnused) {
			v.value.unused++;
			if (v.value.unused >= RESOURCE_UNUSED_CLEANUP_COUNT) {
				_scratch.push_back(v.key);
			}
		} else {
			v.value = ResourceUnused;
			v.value.unused = 0;
		}
	}

	// Clear up resources that weren't used for the last pass.
	for (MTL::Resource *res : _scratch) {
		_previous.erase(res);
	}
	_scratch.clear();
}

void MDCommandBuffer::_render_bind_uniform_sets() {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	if (!render.dirty.has_flag(RenderState::DIRTY_UNIFORMS)) {
		return;
	}

	render.dirty.clear_flag(RenderState::DIRTY_UNIFORMS);
	uint64_t set_uniforms = render.uniform_set_mask;
	render.uniform_set_mask = 0;

	MDRenderShader *shader = render.pipeline->shader;
	const uint32_t dynamic_offsets = render.dynamic_offsets;

	while (set_uniforms != 0) {
		// Find the index of the next set bit.
		uint32_t index = (uint32_t)__builtin_ctzll(set_uniforms);
		// Clear the set bit.
		set_uniforms &= (set_uniforms - 1);
		MDUniformSet *set = render.uniform_sets[index];
		if (set == nullptr || index >= (uint32_t)shader->sets.size()) {
			continue;
		}
		if (shader->uses_argument_buffers) {
			_bind_uniforms_argument_buffers(set, shader, index, dynamic_offsets);
		} else {
			DirectEncoder de(render.encoder.get(), binding_cache, DirectEncoder::RENDER);
			_bind_uniforms_direct(set, shader, de, index, dynamic_offsets);
		}
	}
}

void MDCommandBuffer::render_begin_pass(RDD::RenderPassID p_render_pass, RDD::FramebufferID p_frameBuffer, RDD::CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RDD::RenderPassClearValue> p_clear_values) {
	DEV_ASSERT(command_buffer());
	end();

	MDRenderPass *pass = (MDRenderPass *)(p_render_pass.id);
	MDFrameBuffer *fb = (MDFrameBuffer *)(p_frameBuffer.id);

	type = MDCommandBufferStateType::Render;
	render.pass = pass;
	render.current_subpass = nullptr;
	render.render_area = p_rect;
	render.clear_values.resize(p_clear_values.size());
	for (uint32_t i = 0; i < p_clear_values.size(); i++) {
		render.clear_values[i] = p_clear_values[i];
	}
	render.is_rendering_entire_area = (p_rect.position == Point2i(0, 0)) && p_rect.size == fb->size;
	render.frameBuffer = fb;
	// GodotNext: rd_indirect_count (#177). The pass reads the clamped records written just before it.
	if (_indirect_count_clamp_fenced) {
		_fence_wait_current_level = true;
		_indirect_count_clamp_fenced = false;
	}
	render_next_subpass();
}

void MDCommandBuffer::render_next_subpass() {
	DEV_ASSERT(command_buffer());

	MDSubpass *prev_subpass = render.current_subpass;
	if (prev_subpass != nullptr) {
		_end_render_pass();
		// The closed encoder updated F_cur, and this subpass depends on it.
		_fence_wait_current_level = true;
	}

	render.current_subpass = (prev_subpass == nullptr)
			? &render.pass->subpasses[0]
			: prev_subpass + 1;

	const MDFrameBuffer &fb = *render.frameBuffer;
	const MDRenderPass &pass = *render.pass;
	const MDSubpass &subpass = render.get_subpass();

	NS::SharedPtr<MTL::RenderPassDescriptor> desc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());

	if (subpass.view_count > 1) {
		desc->setRenderTargetArrayLength(subpass.view_count);
	}

	PixelFormats &pf = device_driver->get_pixel_formats();

	uint32_t attachmentCount = 0;
	for (uint32_t i = 0; i < subpass.color_references.size(); i++) {
		uint32_t idx = subpass.color_references[i].attachment;
		if (idx == RDD::AttachmentReference::UNUSED) {
			continue;
		}

		attachmentCount += 1;
		MTL::RenderPassColorAttachmentDescriptor *ca = desc->colorAttachments()->object(i);

		uint32_t resolveIdx = subpass.resolve_references.is_empty() ? RDD::AttachmentReference::UNUSED : subpass.resolve_references[i].attachment;
		bool has_resolve = resolveIdx != RDD::AttachmentReference::UNUSED;
		bool can_resolve = true;
		if (resolveIdx != RDD::AttachmentReference::UNUSED) {
			MTL::Texture *resolve_tex = fb.get_texture(resolveIdx);
			can_resolve = flags::all(pf.getCapabilities(resolve_tex->pixelFormat()), kMTLFmtCapsResolve);
			if (can_resolve) {
				ca->setResolveTexture(resolve_tex);
			} else {
				CRASH_NOW_MSG("unimplemented: using a texture format that is not supported for resolve");
			}
		}

		const MDAttachment &attachment = pass.attachments[idx];

		MTL::Texture *tex = fb.get_texture(idx);
		ERR_FAIL_NULL_MSG(tex, "Frame buffer color texture is null.");

		if ((attachment.type & MDAttachmentType::Color)) {
			if (attachment.configureDescriptor(ca, pf, subpass, tex, render.is_rendering_entire_area, has_resolve, can_resolve, false)) {
				Color clearColor = render.clear_values[idx].color;
				ca->setClearColor(MTL::ClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
			}
		}
	}

	if (subpass.depth_stencil_reference.attachment != RDD::AttachmentReference::UNUSED) {
		attachmentCount += 1;
		uint32_t idx = subpass.depth_stencil_reference.attachment;
		const MDAttachment &attachment = pass.attachments[idx];
		MTL::Texture *tex = fb.get_texture(idx);
		ERR_FAIL_NULL_MSG(tex, "Frame buffer depth / stencil texture is null.");
		if (attachment.type & MDAttachmentType::Depth) {
			MTL::RenderPassDepthAttachmentDescriptor *da = desc->depthAttachment();

			uint32_t resolveIdx = subpass.depth_resolve_reference.attachment;
			bool has_resolve = resolveIdx != RDD::AttachmentReference::UNUSED;
			bool can_resolve = true;
			if (has_resolve) {
				MTL::Texture *resolve_tex = fb.get_texture(resolveIdx);
				can_resolve = flags::all(pf.getCapabilities(resolve_tex->pixelFormat()), kMTLFmtCapsResolve);
				if (can_resolve) {
					da->setResolveTexture(resolve_tex);
				} else {
					CRASH_NOW_MSG("unimplemented: using a texture format that is not supported for resolve");
				}
			}

			if (attachment.configureDescriptor(da, pf, subpass, tex, render.is_rendering_entire_area, has_resolve, can_resolve, false)) {
				da->setClearDepth(render.clear_values[idx].depth);
			}
		}

		if (attachment.type & MDAttachmentType::Stencil) {
			MTL::RenderPassStencilAttachmentDescriptor *sa = desc->stencilAttachment();
			if (attachment.configureDescriptor(sa, pf, subpass, tex, render.is_rendering_entire_area, false, false, true)) {
				sa->setClearStencil(render.clear_values[idx].stencil);
			}
		}
	}

	desc->setRasterizationRateMap(fb.rasterization_rate_map);
	desc->setRenderTargetWidth(MAX((NS::UInteger)MIN(render.render_area.position.x + render.render_area.size.width, fb.size.width), 1u));
	desc->setRenderTargetHeight(MAX((NS::UInteger)MIN(render.render_area.position.y + render.render_area.size.height, fb.size.height), 1u));

	if (attachmentCount == 0) {
		// If there are no attachments, delay the creation of the encoder,
		// so we can use a matching sample count for the pipeline, by setting
		// the defaultRasterSampleCount from the pipeline's sample count.
		render.desc = desc;
	} else {
		render.encoder = NS::RetainPtr(command_buffer()->renderCommandEncoder(desc.get()));
		render.clear_arg_buffer_cache();
		render.encoder_raster = MDRenderPipeline::RasterState();
		_encode_residency(render.encoder.get());
		// GodotNext: rd_mesh_shaders (#178)
		render.encoder_wait_fence = _fence_wait(render.encoder.get());
		render.mesh_stages_encoded = false;

		if (!render.is_rendering_entire_area) {
			_render_clear_render_area();
		}
		// With a new encoder, all state is dirty.
		render.dirty.set_flag(RenderState::DIRTY_ALL);
	}
}

void MDCommandBuffer::render_draw(uint32_t p_vertex_count,
		uint32_t p_instance_count,
		uint32_t p_base_vertex,
		uint32_t p_first_instance) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");

	_render_set_dirty_state();

	const MDSubpass &subpass = render.get_subpass();
	if (subpass.view_count > 1) {
		p_instance_count *= subpass.view_count;
	}

	DEV_ASSERT(render.dirty == 0);

	MTL::RenderCommandEncoder *enc = render.encoder.get();
	enc->drawPrimitives(render.pipeline->raster_state.render_primitive, p_base_vertex, p_vertex_count, p_instance_count, p_first_instance);
}

void MDCommandBuffer::render_bind_vertex_buffers(uint32_t p_binding_count, const RDD::BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);

	render.vertex_buffers.resize(p_binding_count);
	render.vertex_offsets.resize(p_binding_count);

	// Are the existing buffer bindings the same?
	bool same = true;

	// Reverse the buffers, as their bindings are assigned in descending order.
	for (uint32_t i = 0; i < p_binding_count; i += 1) {
		const RenderingDeviceDriverMetal::BufferInfo *buf_info = (const RenderingDeviceDriverMetal::BufferInfo *)p_buffers[p_binding_count - i - 1].id;

		NS::UInteger dynamic_offset = 0;
		if (buf_info->is_dynamic()) {
			const MetalBufferDynamicInfo *dyn_buf = (const MetalBufferDynamicInfo *)buf_info;
			uint64_t frame_idx = p_dynamic_offsets & 0x3;
			p_dynamic_offsets >>= 2;
			dynamic_offset = frame_idx * dyn_buf->size_bytes;
		}
		if (render.vertex_buffers[i] != buf_info->buffer.get()) {
			render.vertex_buffers[i] = buf_info->buffer.get();
			same = false;
		}

		render.vertex_offsets[i] = dynamic_offset + p_offsets[p_binding_count - i - 1];
	}

	if (render.encoder) {
		uint32_t first = device_driver->get_metal_buffer_index_for_vertex_attribute_binding(p_binding_count - 1);
		if (same) {
			NS::UInteger *offset_ptr = render.vertex_offsets.ptr();
			for (uint32_t i = first; i < first + p_binding_count; i++) {
				render.encoder->setVertexBufferOffset(*offset_ptr, i);
				offset_ptr++;
			}
		} else {
			render.encoder->setVertexBuffers(render.vertex_buffers.ptr(), render.vertex_offsets.ptr(), NS::Range(first, p_binding_count));
		}
		render.dirty.clear_flag(RenderState::DIRTY_VERTEX);
	} else {
		render.dirty.set_flag(RenderState::DIRTY_VERTEX);
	}
}

void MDCommandBuffer::render_bind_index_buffer(RDD::BufferID p_buffer, RDD::IndexBufferFormat p_format, uint64_t p_offset) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);

	const RenderingDeviceDriverMetal::BufferInfo *buffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_buffer.id;

	render.index_buffer = buffer->buffer.get();
	render.index_type = p_format == RDD::IndexBufferFormat::INDEX_BUFFER_FORMAT_UINT16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
	render.index_offset = p_offset;
}

void MDCommandBuffer::render_draw_indexed(uint32_t p_index_count,
		uint32_t p_instance_count,
		uint32_t p_first_index,
		int32_t p_vertex_offset,
		uint32_t p_first_instance) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");

	_render_set_dirty_state();

	const MDSubpass &subpass = render.get_subpass();
	if (subpass.view_count > 1) {
		p_instance_count *= subpass.view_count;
	}

	MTL::RenderCommandEncoder *enc = render.encoder.get();

	uint32_t index_offset = render.index_offset;
	index_offset += p_first_index * (render.index_type == MTL::IndexTypeUInt16 ? sizeof(uint16_t) : sizeof(uint32_t));

	enc->drawIndexedPrimitives(render.pipeline->raster_state.render_primitive, p_index_count, render.index_type, render.index_buffer, index_offset, p_instance_count, p_vertex_offset, p_first_instance);
}

void MDCommandBuffer::render_draw_indexed_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");

	_render_set_dirty_state();

	MTL::RenderCommandEncoder *enc = render.encoder.get();

	const RenderingDeviceDriverMetal::BufferInfo *indirect_buffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_indirect_buffer.id;
	NS::UInteger indirect_offset = p_offset;

	for (uint32_t i = 0; i < p_draw_count; i++) {
		enc->drawIndexedPrimitives(render.pipeline->raster_state.render_primitive, render.index_type, render.index_buffer, 0, indirect_buffer->buffer.get(), indirect_offset);
		indirect_offset += p_stride;
	}
}

void MDCommandBuffer::render_draw_indexed_indirect_count(RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	// GodotNext: rd_indirect_count (#177). The records and the count were read by the prepare step.
	_render_draw_indirect_count(true, p_max_draw_count);
}

// GodotNext: rd_indirect_count (#177)
void MDCommandBuffer::render_draw_indirect_count_prepare(RDD::RenderPassID p_render_pass, bool p_indexed, RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	// Every prepare adds one entry, so the draws find theirs by order.
	IndirectCountClamp clamp;
	clamp.max_draw_count = p_max_draw_count;
	clamp.indexed = p_indexed;

	const uint32_t words = p_indexed ? 5 : 4;
	static_assert(sizeof(MTL::DrawIndexedPrimitivesIndirectArguments) == 5 * sizeof(uint32_t));
	static_assert(sizeof(MTL::DrawPrimitivesIndirectArguments) == 4 * sizeof(uint32_t));
	// SPIRV-Cross derives gl_ViewIndex from the instance, so a multiview draw runs instanceCount x view_count
	// instances, as in render_draw(). Every subpass of a pass has the pass's view count (render_pass_create()).
	const MDRenderPass *pass = (const MDRenderPass *)(p_render_pass.id);
	const uint32_t view_count = (pass != nullptr && !pass->subpasses.is_empty()) ? MAX(pass->subpasses[0].view_count, 1u) : 1u;

	// GodotNext: rd_mesh_shaders (#178). The clamp pass moved to _prepare_indirect_count_clamp(); word 1 is
	// instanceCount.
	_prepare_indirect_count_clamp(clamp, words, 1, view_count, p_indirect_buffer, p_offset, p_count_buffer, p_count_buffer_offset, p_stride);
}

// GodotNext: rd_mesh_shaders (#178). The body of #177's render_draw_indirect_count_prepare(), for any record layout.
void MDCommandBuffer::_prepare_indirect_count_clamp(IndirectCountClamp p_clamp, uint32_t p_words, uint32_t p_count_word, uint32_t p_view_count, RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_stride) {
	IndirectCountClamp &clamp = p_clamp;
	const uint32_t p_max_draw_count = clamp.max_draw_count;
	const uint32_t words = p_words;
	const uint32_t view_count = p_view_count;
	const uint32_t stride = p_stride == 0 ? words * sizeof(uint32_t) : p_stride;

	// Null after one error when the kernel cannot be created; the draw is then skipped.
	MTL::ComputePipelineState *pipeline = device_driver->get_resource_cache().get_indirect_count_clamp_pipeline_state();
	if (p_max_draw_count > 0 && pipeline != nullptr) {
		MetalBuffer clamped = device_driver->get_allocator()->new_buffer(NS::UInteger(p_max_draw_count) * words * sizeof(uint32_t), MTL::ResourceStorageModePrivate);
		if (clamped.buffer) {
			// Freed with the command buffer's other deferred buffers, once it has run.
			defer_acceleration_structure_resource(clamped);
			clamp.buffer = clamped.buffer.get();

			const RenderingDeviceDriverMetal::BufferInfo *indirect_buffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_indirect_buffer.id;
			const RenderingDeviceDriverMetal::BufferInfo *count_buffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_count_buffer.id;
			struct {
				uint32_t max_draw_count;
				uint32_t stride;
				uint32_t words;
				uint32_t view_count;
				uint32_t count_word; // GodotNext: rd_mesh_shaders (#178)
			} params = { p_max_draw_count, stride, words, view_count, p_count_word };

			// A self-contained encoder before the render pass, as in timestamp_write(). It waits on what wrote the
			// records and the count, and the render pass (or the next clamp) waits on it.
			end();
			MTL::ComputeCommandEncoder *enc = command_buffer()->computeCommandEncoder(MTL::DispatchTypeConcurrent);
#ifdef DEV_ENABLED
			enc->setLabel(MTLSTR("Indirect Count Clamp"));
#endif
			if (_indirect_count_clamp_fenced) {
				_fence_wait_current_level = true;
			}
			_fence_wait(enc);
			_encode_residency(enc);
			enc->setComputePipelineState(pipeline);
			enc->setBuffer(indirect_buffer->buffer.get(), p_offset, 0);
			enc->setBuffer(count_buffer->buffer.get(), p_count_buffer_offset, 1);
			enc->setBuffer(clamp.buffer, 0, 2);
			enc->setBytes(&params, sizeof(params), 3);
			const NS::UInteger group = MIN(NS::UInteger(64), pipeline->maxTotalThreadsPerThreadgroup());
			enc->dispatchThreads(MTL::Size(p_max_draw_count, 1, 1), MTL::Size(group, 1, 1));
			_fence_update(enc);
			enc->endEncoding();
			_indirect_count_clamp_fenced = true;
		} else {
			ERR_PRINT("Cannot allocate the buffer for an indirect count clamp.");
		}
	}

	_indirect_count_clamps.push_back(clamp);
}

// GodotNext: rd_indirect_count (#177)
void MDCommandBuffer::_render_draw_indirect_count(bool p_indexed, uint32_t p_max_draw_count) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_COND_MSG(_indirect_count_clamps_used >= _indirect_count_clamps.size(), "An indirect count draw was not prepared before its render pass.");
	const IndirectCountClamp clamp = _indirect_count_clamps[_indirect_count_clamps_used++];
	DEV_ASSERT(!clamp.mesh_tasks && clamp.indexed == p_indexed && clamp.max_draw_count == p_max_draw_count); // GodotNext: rd_mesh_shaders (#178)
	if (clamp.buffer == nullptr) {
		return; // The prepare failed and said why.
	}
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");

	_render_set_dirty_state();

	MTL::RenderCommandEncoder *enc = render.encoder.get();
	const NS::UInteger record_size = p_indexed ? sizeof(MTL::DrawIndexedPrimitivesIndirectArguments) : sizeof(MTL::DrawPrimitivesIndirectArguments);
	NS::UInteger indirect_offset = 0;
	for (uint32_t i = 0; i < p_max_draw_count; i++) {
		if (p_indexed) {
			enc->drawIndexedPrimitives(render.pipeline->raster_state.render_primitive, render.index_type, render.index_buffer, render.index_offset, clamp.buffer, indirect_offset);
		} else {
			enc->drawPrimitives(render.pipeline->raster_state.render_primitive, clamp.buffer, indirect_offset);
		}
		indirect_offset += record_size;
	}
}

// GodotNext: rd_mesh_shaders (#178)
void MDCommandBuffer::render_draw_mesh_tasks_indirect_count_prepare(RDD::RenderPassID p_render_pass, RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	IndirectCountClamp clamp;
	clamp.max_draw_count = p_max_draw_count;
	clamp.mesh_tasks = true;
	static_assert(sizeof(MTL::DispatchThreadgroupsIndirectArguments) == 3 * sizeof(uint32_t));
	// Word 0 is threadgroupsPerGrid.x. Mesh pipelines take no view count multiplication.
	_prepare_indirect_count_clamp(clamp, 3, 0, 1, p_indirect_buffer, p_offset, p_count_buffer, p_count_buffer_offset, p_stride);
}

// GodotNext: rd_mesh_shaders (#178)
void MDCommandBuffer::render_draw_mesh_tasks(uint32_t p_group_count_x, uint32_t p_group_count_y, uint32_t p_group_count_z) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");
	const MDRenderShader *shader = render.pipeline->shader;
	ERR_FAIL_COND_MSG(!shader->is_mesh(), "The bound pipeline has no mesh function.");

	_render_set_dirty_state();

	render.encoder->drawMeshThreadgroups(MTL::Size(p_group_count_x, p_group_count_y, p_group_count_z), shader->task_local, shader->mesh_local);
}

// GodotNext: rd_mesh_shaders (#178)
void MDCommandBuffer::render_draw_mesh_tasks_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");
	const MDRenderShader *shader = render.pipeline->shader;
	ERR_FAIL_COND_MSG(!shader->is_mesh(), "The bound pipeline has no mesh function.");

	_render_set_dirty_state();

	MTL::RenderCommandEncoder *enc = render.encoder.get();
	const RenderingDeviceDriverMetal::BufferInfo *indirect_buffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_indirect_buffer.id;
	const NS::UInteger stride = p_stride == 0 ? sizeof(MTL::DispatchThreadgroupsIndirectArguments) : p_stride;
	NS::UInteger indirect_offset = p_offset;
	for (uint32_t i = 0; i < p_draw_count; i++) {
		enc->drawMeshThreadgroups(indirect_buffer->buffer.get(), indirect_offset, shader->task_local, shader->mesh_local);
		indirect_offset += stride;
	}
}

// GodotNext: rd_mesh_shaders (#178). The records and the count were read by the prepare step.
void MDCommandBuffer::render_draw_mesh_tasks_indirect_count(RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_COND_MSG(_indirect_count_clamps_used >= _indirect_count_clamps.size(), "An indirect count draw was not prepared before its render pass.");
	const IndirectCountClamp clamp = _indirect_count_clamps[_indirect_count_clamps_used++];
	DEV_ASSERT(clamp.mesh_tasks && clamp.max_draw_count == p_max_draw_count);
	if (clamp.buffer == nullptr) {
		return; // The prepare failed and said why.
	}
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");
	const MDRenderShader *shader = render.pipeline->shader;
	ERR_FAIL_COND_MSG(!shader->is_mesh(), "The bound pipeline has no mesh function.");

	_render_set_dirty_state();

	MTL::RenderCommandEncoder *enc = render.encoder.get();
	NS::UInteger indirect_offset = 0;
	for (uint32_t i = 0; i < p_max_draw_count; i++) {
		enc->drawMeshThreadgroups(clamp.buffer, indirect_offset, shader->task_local, shader->mesh_local);
		indirect_offset += sizeof(MTL::DispatchThreadgroupsIndirectArguments);
	}
}

void MDCommandBuffer::render_draw_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);
	ERR_FAIL_NULL_MSG(render.pipeline, "No pipeline set for render command buffer.");

	_render_set_dirty_state();

	MTL::RenderCommandEncoder *enc = render.encoder.get();

	const RenderingDeviceDriverMetal::BufferInfo *indirect_buffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_indirect_buffer.id;
	NS::UInteger indirect_offset = p_offset;

	for (uint32_t i = 0; i < p_draw_count; i++) {
		enc->drawPrimitives(render.pipeline->raster_state.render_primitive, indirect_buffer->buffer.get(), indirect_offset);
		indirect_offset += p_stride;
	}
}

void MDCommandBuffer::render_draw_indirect_count(RDD::BufferID p_indirect_buffer, uint64_t p_offset, RDD::BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	// GodotNext: rd_indirect_count (#177)
	_render_draw_indirect_count(false, p_max_draw_count);
}

void MDCommandBuffer::render_end_pass() {
	DEV_ASSERT(type == MDCommandBufferStateType::Render);

	_pop_active_encoder_labels();
	_fence_update(render.encoder.get(), render.mesh_stages_encoded); // GodotNext: rd_mesh_shaders (#178)
	render.end_encoding();
	render.reset();
	reset();

	// GodotNext: rd_indirect_count (#177). The render graph prepares each pass's count draws right before it.
	DEV_ASSERT(_indirect_count_clamps_used == _indirect_count_clamps.size());
	_indirect_count_clamps.clear();
	_indirect_count_clamps_used = 0;
}

#pragma mark - RenderState

void MDCommandBuffer::RenderState::reset() {
	clear_arg_buffer_cache();
	encoder_raster = MDRenderPipeline::RasterState();
	pass = nullptr;
	frameBuffer = nullptr;
	pipeline = nullptr;
	current_subpass = nullptr;
	render_area = {};
	is_rendering_entire_area = false;
	desc.reset();
	encoder.reset();
	index_buffer = nullptr;
	index_type = MTL::IndexTypeUInt16;
	dirty = DIRTY_NONE;
	uniform_sets.clear();
	dynamic_offsets = 0;
	uniform_set_mask = 0;
	clear_values.clear();
	viewports.clear();
	scissors.clear();
	blend_constants.reset();
	bzero(vertex_buffers.ptr(), sizeof(MTL::Buffer *) * vertex_buffers.size());
	vertex_buffers.clear();
	bzero(vertex_offsets.ptr(), sizeof(NS::UInteger) * vertex_offsets.size());
	vertex_offsets.clear();
	resource_tracker.reset();
	// GodotNext: rd_mesh_shaders (#178)
	encoder_wait_fence = nullptr;
	mesh_stages_encoded = false;
}

void MDCommandBuffer::RenderState::end_encoding() {
	if (!encoder) {
		return;
	}

	encoder->endEncoding();
	encoder.reset();
}

#pragma mark - ComputeState

void MDCommandBuffer::ComputeState::end_encoding() {
	if (!encoder) {
		return;
	}

	encoder->endEncoding();
	encoder.reset();
}

#pragma mark - Compute

void MDCommandBuffer::compute_begin_pass() {
	ERR_FAIL_COND_MSG(type == MDCommandBufferStateType::Render, "Compute pass cannot begin while a render pass is active.");

	if (type == MDCommandBufferStateType::InlineRender) {
		_end_inline_render();
	} else if (type == MDCommandBufferStateType::Blit) {
		_end_blit();
	} else if (type == MDCommandBufferStateType::AccelerationStructure) {
		// GodotNext: metal_acceleration_structure
		_end_as();
	}

	type = MDCommandBufferStateType::Compute;
	if (!compute.encoder) {
		compute.encoder = NS::RetainPtr(command_buffer()->computeCommandEncoder(MTL::DispatchTypeConcurrent));
		_encode_residency(compute.encoder.get());
		_fence_wait(compute.encoder.get());
	}
}

void MDCommandBuffer::compute_end_pass() {
	if (type == MDCommandBufferStateType::Compute) {
		_end_compute_dispatch();
	}
}

void MDCommandBuffer::_compute_set_dirty_state() {
	if (compute.dirty.has_flag(ComputeState::DIRTY_PIPELINE)) {
		if (!compute.encoder) {
			compute.encoder = NS::RetainPtr(command_buffer()->computeCommandEncoder(MTL::DispatchTypeConcurrent));
			_encode_residency(compute.encoder.get());
			_fence_wait(compute.encoder.get());
		}
		compute.encoder->setComputePipelineState(compute.pipeline->state.get());
	}

	_compute_bind_uniform_sets();

	if (compute.dirty.has_flag(ComputeState::DIRTY_PUSH)) {
		if (push_constant_binding.has(RDD::SHADER_STAGE_COMPUTE)) {
			uint32_t slot = push_constant_binding.get_binding();
			compute.encoder->setBytes(push_constant_data, push_constant_data_len, slot);
			binding_cache.invalidate_buffer(slot);
		}
	}

	// GodotNext: metal_shader_validation
	// Same as the render path: declare argument-buffer usage before the dispatch.
	compute.resource_tracker.encode(compute.encoder.get());

	compute.dirty.clear();
}

void MDCommandBuffer::_compute_bind_uniform_sets() {
	DEV_ASSERT(type == MDCommandBufferStateType::Compute);
	if (!compute.dirty.has_flag(ComputeState::DIRTY_UNIFORMS)) {
		return;
	}

	compute.dirty.clear_flag(ComputeState::DIRTY_UNIFORMS);
	uint64_t set_uniforms = compute.uniform_set_mask;
	compute.uniform_set_mask = 0;

	MDComputeShader *shader = compute.pipeline->shader;
	const uint32_t dynamic_offsets = compute.dynamic_offsets;

	while (set_uniforms != 0) {
		// Find the index of the next set bit.
		uint32_t index = (uint32_t)__builtin_ctzll(set_uniforms);
		// Clear the set bit.
		set_uniforms &= (set_uniforms - 1);
		MDUniformSet *set = compute.uniform_sets[index];
		if (set == nullptr || index >= (uint32_t)shader->sets.size()) {
			continue;
		}
		if (shader->uses_argument_buffers) {
			_bind_uniforms_argument_buffers_compute(set, shader, index, dynamic_offsets);
		} else {
			DirectEncoder de(compute.encoder.get(), binding_cache, DirectEncoder::COMPUTE);
			_bind_uniforms_direct(set, shader, de, index, dynamic_offsets);
		}
	}
}

void MDCommandBuffer::ComputeState::reset() {
	pipeline = nullptr;
	encoder.reset();
	dirty = DIRTY_NONE;
	uniform_sets.clear();
	dynamic_offsets = 0;
	uniform_set_mask = 0;
	resource_tracker.reset();
}

void MDCommandBuffer::compute_bind_uniform_sets(VectorView<RDD::UniformSetID> p_uniform_sets, RDD::ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	DEV_ASSERT(type == MDCommandBufferStateType::Compute);

	if (uint32_t new_size = p_first_set_index + p_set_count; compute.uniform_sets.size() < new_size) {
		uint32_t s = compute.uniform_sets.size();
		compute.uniform_sets.resize(new_size);
		// Set intermediate values to null.
		std::fill(&compute.uniform_sets[s], compute.uniform_sets.end().operator->(), nullptr);
	}

	const MDShader *shader = (const MDShader *)p_shader.id;
	DynamicOffsetLayout layout = shader->dynamic_offset_layout;

	// Clear bits for sets being bound, then OR new values.
	for (uint32_t i = 0; i < p_set_count && compute.dynamic_offsets != 0; i++) {
		uint32_t set_index = p_first_set_index + i;
		uint32_t count = layout.get_count(set_index);
		if (count > 0) {
			uint32_t shift = layout.get_offset_index_shift(set_index);
			uint32_t mask = ((1u << (count * 4u)) - 1u) << shift;
			compute.dynamic_offsets &= ~mask; // Clear this set's bits
		}
	}
	compute.dynamic_offsets |= p_dynamic_offsets;

	for (size_t i = 0; i < p_set_count; ++i) {
		MDUniformSet *set = (MDUniformSet *)(p_uniform_sets[i].id);

		uint32_t index = p_first_set_index + i;
		if (compute.uniform_sets[index] != set || layout.get_count(index) > 0) {
			compute.dirty.set_flag(ComputeState::DIRTY_UNIFORMS);
			compute.uniform_set_mask |= 1ULL << index;
			compute.uniform_sets[index] = set;
		}
	}
}

void MDCommandBuffer::compute_dispatch(uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) {
	DEV_ASSERT(type == MDCommandBufferStateType::Compute);

	_compute_set_dirty_state();

	MTL::Size size = MTL::Size(p_x_groups, p_y_groups, p_z_groups);

	MTL::ComputeCommandEncoder *enc = compute.encoder.get();
	enc->dispatchThreadgroups(size, compute.pipeline->compute_state.local);
}

void MDCommandBuffer::compute_dispatch_indirect(RDD::BufferID p_indirect_buffer, uint64_t p_offset) {
	DEV_ASSERT(type == MDCommandBufferStateType::Compute);

	_compute_set_dirty_state();

	const RenderingDeviceDriverMetal::BufferInfo *indirectBuffer = (const RenderingDeviceDriverMetal::BufferInfo *)p_indirect_buffer.id;

	MTL::ComputeCommandEncoder *enc = compute.encoder.get();
	enc->dispatchThreadgroups(indirectBuffer->buffer.get(), p_offset, compute.pipeline->compute_state.local);
}

#pragma mark - Raytracing Commands

// GodotNext: metal_ray_shaders (#172)
void MDCommandBuffer::raytracing_bind_pipeline(RDD::RaytracingPipelineID p_pipeline) {
	MDRaytracingPipeline *pipeline = reinterpret_cast<MDRaytracingPipeline *>(p_pipeline.id);
	ERR_FAIL_NULL(pipeline);
	ERR_FAIL_COND(pipeline->is_library);
	if (type == MDCommandBufferStateType::Render) {
		render_end_pass();
	} else if (type == MDCommandBufferStateType::InlineRender) {
		_end_inline_render();
	} else if (type == MDCommandBufferStateType::Blit) {
		_end_blit();
	} else if (type == MDCommandBufferStateType::AccelerationStructure) {
		_end_as();
	}
	type = MDCommandBufferStateType::Compute;
	if (raytracing.pipeline != pipeline) {
		if (raytracing.pipeline != nullptr && raytracing.pipeline->layout_shader != pipeline->layout_shader) {
			push_constant_binding = MetalPushConstantBinding();
			push_constant_data_len = 0;
		}
		raytracing.pipeline = pipeline;
	}
}

void MDCommandBuffer::raytracing_bind_uniform_set(RDD::UniformSetID p_uniform_set, RDD::ShaderID p_shader, uint32_t p_set_index) {
	if (raytracing.uniform_sets.size() <= p_set_index) {
		uint32_t old_size = raytracing.uniform_sets.size();
		raytracing.uniform_sets.resize(p_set_index + 1);
		for (uint32_t i = old_size; i < raytracing.uniform_sets.size(); i++) {
			raytracing.uniform_sets[i] = nullptr;
		}
	}
	raytracing.uniform_sets[p_set_index] = reinterpret_cast<MDUniformSet *>(p_uniform_set.id);
}

GODOT_CLANG_WARNING_PUSH_AND_IGNORE("-Wunguarded-availability-new")

void MDCommandBuffer::_raytracing_encode_trace(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth, RDD::BufferID p_indirect_buffer, uint64_t p_indirect_offset) {
	ERR_FAIL_NULL(raytracing.pipeline);
	ERR_FAIL_COND(raytracing.pipeline->raygen_entries.is_empty());

	const bool is_indirect = p_indirect_buffer.id != 0;
	if (!is_indirect && (p_width == 0 || p_height == 0 || p_depth == 0)) {
		return;
	}

	const uint32_t raygen_index = RDD::metal_rt_raygen_index_from_sbt(p_raygen_sbt.offset, p_raygen_sbt.stride);
	ERR_FAIL_UNSIGNED_INDEX(raygen_index, raytracing.pipeline->raygen_entries.size());
	MDRaytracingPipeline::RaygenEntry &entry = raytracing.pipeline->raygen_entries[raygen_index];
	ERR_FAIL_NULL(entry.state.get());
	ERR_FAIL_NULL(entry.visible_table.get());
	ERR_FAIL_NULL(entry.intersection_table.get());

	// Ensure a compute command encoder is active, closing any non-compute encoder first.
	if (type == MDCommandBufferStateType::Render) {
		render_end_pass();
	} else if (type == MDCommandBufferStateType::InlineRender) {
		_end_inline_render();
	} else if (type == MDCommandBufferStateType::Blit) {
		_end_blit();
	} else if (type == MDCommandBufferStateType::AccelerationStructure) {
		_end_as();
	}
	type = MDCommandBufferStateType::Compute;
	if (!compute.encoder) {
		compute.encoder = NS::RetainPtr(command_buffer()->computeCommandEncoder(MTL::DispatchTypeConcurrent));
		_encode_residency(compute.encoder.get());
		_fence_wait(compute.encoder.get());
	}
	MTL::ComputeCommandEncoder *enc = compute.encoder.get();

	RDD::MetalRTSBTParams params = {};
	params.launch_size_x = p_width;
	params.launch_size_y = p_height;
	params.launch_size_z = p_depth;
	params.handle_size = RDD::METAL_RT_SHADER_GROUP_HANDLE_SIZE;

	if (p_miss_sbt.buffer.id != 0) {
		const RDM::BufferInfo *miss_info = reinterpret_cast<const RDM::BufferInfo *>(p_miss_sbt.buffer.id);
		if (miss_info != nullptr && miss_info->buffer.get() != nullptr) {
			params.miss_sbt_address = miss_info->buffer->gpuAddress() + p_miss_sbt.offset;
			params.miss_sbt_stride = p_miss_sbt.stride;
			params.miss_sbt_count = p_miss_sbt.stride > 0 ? p_miss_sbt.size / p_miss_sbt.stride : 0;
			enc->useResource(miss_info->buffer.get(), MTL::ResourceUsageRead);
		}
	}

	MTL::IntersectionFunctionTable *active_ift = entry.intersection_table.get();
	if (p_hit_sbt.buffer.id != 0) {
		const RDM::BufferInfo *hit_info = reinterpret_cast<const RDM::BufferInfo *>(p_hit_sbt.buffer.id);
		if (hit_info != nullptr && hit_info->buffer.get() != nullptr) {
			params.hit_sbt_address = hit_info->buffer->gpuAddress() + p_hit_sbt.offset;
			params.hit_sbt_stride = p_hit_sbt.stride;
			params.hit_sbt_count = p_hit_sbt.stride > 0 ? p_hit_sbt.size / p_hit_sbt.stride : 0;
			enc->useResource(hit_info->buffer.get(), MTL::ResourceUsageRead);

			// Build or reuse an SBT-slot-indexed MTL::IntersectionFunctionTable so TLAS instances
			// with intersectionFunctionTableOffset == hit_sbt_offset invoke the right any-hit
			// function even when SBT slots outnumber pipeline hit groups (e.g. 256 materials).
			if (params.hit_sbt_count > 0 && params.hit_sbt_stride >= RDD::METAL_RT_SHADER_GROUP_HANDLE_SIZE) {
				uint64_t sbt_version = 0;
				const bool has_shadow = device_driver->sbt_shadow_get_version(hit_info, sbt_version);
				const bool is_shared = !has_shadow && hit_info->buffer->storageMode() == MTL::StorageModeShared && hit_info->buffer->contents() != nullptr;
				if (has_shadow || is_shared) {
					const bool cache_hit = has_shadow &&
							entry.sbt_intersection_table &&
							entry.sbt_ift_buffer == hit_info &&
							entry.sbt_ift_offset == p_hit_sbt.offset &&
							entry.sbt_ift_stride == params.hit_sbt_stride &&
							entry.sbt_ift_count == params.hit_sbt_count &&
							entry.sbt_ift_version == sbt_version;
					if (!cache_hit) {
						LocalVector<uint8_t> shadow_bytes;
						const uint8_t *sbt_ptr = nullptr;
						uint64_t sbt_size = 0;
						if (has_shadow && device_driver->sbt_shadow_copy_out(hit_info, shadow_bytes, sbt_version)) {
							sbt_ptr = shadow_bytes.ptr();
							sbt_size = shadow_bytes.size();
						} else if (is_shared) {
							sbt_ptr = reinterpret_cast<const uint8_t *>(hit_info->buffer->contents());
							sbt_size = hit_info->buffer->length();
						}
						if (sbt_ptr != nullptr) {
							NS::SharedPtr<MTL::IntersectionFunctionTableDescriptor> ift_desc = NS::TransferPtr(MTL::IntersectionFunctionTableDescriptor::alloc()->init());
							ift_desc->setFunctionCount(params.hit_sbt_count);
							NS::SharedPtr<MTL::IntersectionFunctionTable> sbt_ift = NS::TransferPtr(entry.state->newIntersectionFunctionTable(ift_desc.get()));
							if (sbt_ift) {
								const uint32_t hit_group_count = entry.hit_anyhit_handles.size();
								for (uint32_t slot = 0; slot < params.hit_sbt_count; slot++) {
									const uint32_t ift_idx = RDD::metal_rt_hit_sbt_intersection_function_index(sbt_ptr, sbt_size, p_hit_sbt.offset, params.hit_sbt_stride, slot, hit_group_count);
									if (ift_idx < hit_group_count && entry.hit_anyhit_handles[ift_idx]) {
										sbt_ift->setFunction(entry.hit_anyhit_handles[ift_idx].get(), slot);
									} else {
										sbt_ift->setOpaqueTriangleIntersectionFunction(entry.opaque_sig, slot);
									}
								}
								if (entry.sbt_intersection_table) {
									entry.sbt_intersection_table->retain();
									device_driver->defer_release(reinterpret_cast<CFTypeRef>(entry.sbt_intersection_table.get()));
								}
								entry.sbt_intersection_table = std::move(sbt_ift);
								entry.sbt_ift_buffer = has_shadow ? hit_info : nullptr;
								entry.sbt_ift_offset = p_hit_sbt.offset;
								entry.sbt_ift_stride = params.hit_sbt_stride;
								entry.sbt_ift_count = params.hit_sbt_count;
								entry.sbt_ift_version = sbt_version;
							}
						}
					}
					if (entry.sbt_intersection_table) {
						active_ift = entry.sbt_intersection_table.get();
					}
				}
			}
		}
	}

	MTL::Buffer *indirect_mtl_buffer = nullptr;
	if (is_indirect) {
		const RDM::BufferInfo *ind_info = reinterpret_cast<const RDM::BufferInfo *>(p_indirect_buffer.id);
		ERR_FAIL_NULL(ind_info);
		indirect_mtl_buffer = ind_info->buffer.get();
		ERR_FAIL_NULL(indirect_mtl_buffer);
		params.indirect_launch_address = indirect_mtl_buffer->gpuAddress() + p_indirect_offset;
		enc->useResource(indirect_mtl_buffer, MTL::ResourceUsageRead);
	}

	MDShader *layout_shader = raytracing.pipeline->layout_shader;
	const uint32_t max_sets = MIN((uint32_t)raytracing.uniform_sets.size(), 8u);
	for (uint32_t set_index = 0; set_index < max_sets; set_index++) {
		MDUniformSet *set = raytracing.uniform_sets[set_index];
		if (set == nullptr) {
			continue;
		}
		MTL::Buffer *arg_buf = nullptr;
		if (set->bindless_table != nullptr) {
			raytracing.resource_tracker.merge_from(set->bindless_table->get_usage_to_resources());
			arg_buf = set->bindless_table->buffer.get();
		} else {
			raytracing.resource_tracker.merge_from(set->usage_to_resources);
			arg_buf = set->arg_buffer.buffer.get();
			// GodotNext: metal_ray_shaders (#172). Bind the TLAS instance descriptor buffer so
			// OpTraceRayKHR and AnyHit read each instance's hit_sbt_offset rather than userID.
			if (params.instance_sbt_offsets_address == 0) {
				for (uint32_t i = 0; i < set->uniforms.size(); i++) {
					const RDD::BoundUniform &uniform = set->uniforms[i];
					if (uniform.type == RDD::UNIFORM_TYPE_ACCELERATION_STRUCTURE && !uniform.ids.is_empty()) {
						const RDM::AccelerationStructureInfo *as_info = reinterpret_cast<const RDM::AccelerationStructureInfo *>(uniform.ids[0].id);
						if (as_info != nullptr && as_info->top_level && as_info->last_instance_buffer.get() != nullptr) {
							params.instance_sbt_offsets_address = as_info->last_instance_buffer->gpuAddress() + as_info->last_instance_offset;
							enc->useResource(as_info->last_instance_buffer.get(), MTL::ResourceUsageRead);
							break;
						}
					}
				}
			}
			if (sync_mode != RDM::SyncMode::HazardTracking && layout_shader != nullptr && set_index < (uint32_t)layout_shader->sets.size()) {
				const UniformSet &shader_set = layout_shader->sets[set_index];
				const uint32_t uniform_count = MIN((uint32_t)set->uniforms.size(), (uint32_t)shader_set.uniforms.size());
				for (uint32_t i = 0; i < uniform_count; i++) {
					const UniformInfo &ui = shader_set.uniforms[i];
					const RDD::BoundUniform &uniform = set->uniforms[i];
					if ((ui.usage & MTL::ResourceUsageWrite) == 0 || uniform.ids.is_empty()) {
						continue;
					}
					if (uniform.type == RDD::UNIFORM_TYPE_STORAGE_BUFFER) {
						const RDM::BufferInfo *info = reinterpret_cast<const RDM::BufferInfo *>(uniform.ids[0].id);
						if (info != nullptr && info->buffer.get() != nullptr) {
							enc->useResource(info->buffer.get(), ui.usage);
						}
					} else if (uniform.type == RDD::UNIFORM_TYPE_IMAGE) {
						for (uint32_t j = 0; j < uniform.ids.size(); j++) {
							const RDM::TextureInfo *tinfo = reinterpret_cast<const RDM::TextureInfo *>(uniform.ids[j].id);
							if (tinfo != nullptr && tinfo->texture.get() != nullptr) {
								enc->useResource(tinfo->texture.get(), ui.usage);
							}
						}
					}
				}
			}
		}
		if (arg_buf != nullptr) {
			const uint64_t gpu_addr = arg_buf->gpuAddress();
			params.const_sets[set_index] = gpu_addr;
			params.device_sets[set_index] = gpu_addr;
			enc->setBuffer(arg_buf, 0, set_index);
			active_ift->setBuffer(arg_buf, 0, set_index);
			enc->useResource(arg_buf, MTL::ResourceUsageRead);
		}
	}

	MDRingBuffer::Allocation pc_alloc;
	if (push_constant_data_len > 0) {
		pc_alloc = allocate_arg_buffer(push_constant_data_len);
		memcpy(pc_alloc.ptr, push_constant_data, push_constant_data_len);
		params.push_constants_address = pc_alloc.gpu_address;
		enc->useResource(pc_alloc.buffer, MTL::ResourceUsageRead);
		if (push_constant_binding.is_valid()) {
			const uint32_t pc_slot = push_constant_binding.get_binding();
			enc->setBuffer(pc_alloc.buffer, pc_alloc.offset, pc_slot);
			active_ift->setBuffer(pc_alloc.buffer, pc_alloc.offset, pc_slot);
		}
	}

	MDRingBuffer::Allocation params_alloc = allocate_arg_buffer(sizeof(RDD::MetalRTSBTParams));
	memcpy(params_alloc.ptr, &params, sizeof(RDD::MetalRTSBTParams));
	enc->useResource(params_alloc.buffer, MTL::ResourceUsageRead);

	active_ift->setBuffer(params_alloc.buffer, params_alloc.offset, RDD::METAL_RT_SBT_PARAMS_BUFFER_INDEX);

	raytracing.resource_tracker.encode(enc);

	const MTL::Size threads_per_tg(RDD::METAL_RT_THREADGROUP_WIDTH, RDD::METAL_RT_THREADGROUP_HEIGHT, 1);

	if (is_indirect) {
		MTL::ComputePipelineState *convert_pso = device_driver->get_resource_cache().get_rt_indirect_dispatch_pipeline_state();
		ERR_FAIL_NULL(convert_pso);
		MDRingBuffer::Allocation tg_alloc = allocate_arg_buffer(sizeof(MTL::DispatchThreadgroupsIndirectArguments));
		enc->useResource(tg_alloc.buffer, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
		enc->setComputePipelineState(convert_pso);
		enc->setBuffer(indirect_mtl_buffer, p_indirect_offset, 0);
		enc->setBuffer(tg_alloc.buffer, tg_alloc.offset, 1);
		enc->dispatchThreadgroups(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
		enc->memoryBarrier(MTL::BarrierScopeBuffers);

		// Restore RT bindings after the 1-thread converter dispatch.
		for (uint32_t set_index = 0; set_index < max_sets; set_index++) {
			MDUniformSet *set = raytracing.uniform_sets[set_index];
			if (set == nullptr) {
				continue;
			}
			MTL::Buffer *arg_buf = set->bindless_table != nullptr ? set->bindless_table->buffer.get() : set->arg_buffer.buffer.get();
			if (arg_buf != nullptr) {
				enc->setBuffer(arg_buf, 0, set_index);
			}
		}
		if (pc_alloc.is_valid() && push_constant_binding.is_valid()) {
			enc->setBuffer(pc_alloc.buffer, pc_alloc.offset, push_constant_binding.get_binding());
		}
		enc->setComputePipelineState(entry.state.get());
		enc->setVisibleFunctionTable(entry.visible_table.get(), RDD::METAL_RT_VISIBLE_FUNCTION_TABLE_BUFFER_INDEX);
		enc->setIntersectionFunctionTable(active_ift, RDD::METAL_RT_INTERSECTION_FUNCTION_TABLE_BUFFER_INDEX);
		enc->setBuffer(params_alloc.buffer, params_alloc.offset, RDD::METAL_RT_SBT_PARAMS_BUFFER_INDEX);
		enc->useResource(entry.visible_table.get(), MTL::ResourceUsageRead);
		enc->useResource(active_ift, MTL::ResourceUsageRead);
		enc->dispatchThreadgroups(tg_alloc.buffer, tg_alloc.offset, threads_per_tg);
	} else {
		uint32_t gx = 0;
		uint32_t gy = 0;
		uint32_t gz = 0;
		RDD::metal_rt_compute_threadgroups(p_width, p_height, p_depth, RDD::METAL_RT_THREADGROUP_WIDTH, RDD::METAL_RT_THREADGROUP_HEIGHT, 1, gx, gy, gz);
		enc->setComputePipelineState(entry.state.get());
		enc->setVisibleFunctionTable(entry.visible_table.get(), RDD::METAL_RT_VISIBLE_FUNCTION_TABLE_BUFFER_INDEX);
		enc->setIntersectionFunctionTable(active_ift, RDD::METAL_RT_INTERSECTION_FUNCTION_TABLE_BUFFER_INDEX);
		enc->setBuffer(params_alloc.buffer, params_alloc.offset, RDD::METAL_RT_SBT_PARAMS_BUFFER_INDEX);
		enc->useResource(entry.visible_table.get(), MTL::ResourceUsageRead);
		enc->useResource(active_ift, MTL::ResourceUsageRead);
		enc->dispatchThreadgroups(MTL::Size(gx, gy, gz), threads_per_tg);
	}

	// Invalidate compute binding cache since RT trace overwrote compute encoder bindings.
	binding_cache.clear();
	compute.dirty.set_flag(ComputeState::DIRTY_ALL);
}

GODOT_CLANG_WARNING_POP

void MDCommandBuffer::raytracing_trace_rays(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) {
	_raytracing_encode_trace(p_raygen_sbt, p_miss_sbt, p_hit_sbt, p_width, p_height, p_depth, RDD::BufferID(), 0);
}

void MDCommandBuffer::raytracing_trace_rays_indirect(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, RDD::BufferID p_indirect_buffer, uint64_t p_offset) {
	_raytracing_encode_trace(p_raygen_sbt, p_miss_sbt, p_hit_sbt, 0, 0, 0, p_indirect_buffer, p_offset);
}

void MDCommandBuffer::reset() {
	push_constant_binding = MetalPushConstantBinding();
	push_constant_data_len = 0;
	type = MDCommandBufferStateType::None;
	binding_cache.clear();
}

void MDCommandBuffer::_end_compute_dispatch() {
	DEV_ASSERT(type == MDCommandBufferStateType::Compute);

	_pop_active_encoder_labels();
	_fence_update(compute.encoder.get());
	compute.end_encoding();
	compute.reset();
	reset();
}

void MDCommandBuffer::_set_inline_render_encoder(MTL::RenderCommandEncoder *p_encoder) {
	DEV_ASSERT(p_encoder != nullptr);
	DEV_ASSERT(!render.encoder);
	DEV_ASSERT(!inline_render.encoder);

	type = MDCommandBufferStateType::InlineRender;
	inline_render.encoder = NS::RetainPtr(p_encoder);
}

void MDCommandBuffer::_end_inline_render() {
	DEV_ASSERT(type == MDCommandBufferStateType::InlineRender);

	_pop_active_encoder_labels();
	_fence_update(inline_render.encoder.get());
	inline_render.encoder->endEncoding();
	inline_render.reset();
	reset();
}

void MDCommandBuffer::_end_blit() {
	DEV_ASSERT(type == MDCommandBufferStateType::Blit);

	_pop_active_encoder_labels();
	_fence_update(blit.encoder.get());
	blit.encoder->endEncoding();
	blit.reset();
	reset();
}

// GodotNext: metal_acceleration_structure
MDCommandBuffer::~MDCommandBuffer() {
	_free_as_staging();
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::_free_as_staging() {
	if (device_driver != nullptr && device_driver->get_allocator() != nullptr) {
		MetalAllocator *allocator = device_driver->get_allocator();
		for (MetalBuffer &buffer : _as_staging) {
			allocator->free_buffer(buffer);
		}
	}
	_as_staging.clear();
	_as_descriptors.clear();
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::defer_acceleration_structure_resource(MetalBuffer p_buffer) {
	_as_staging.push_back(p_buffer);
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::defer_acceleration_structure_descriptor(NS::SharedPtr<MTL::AccelerationStructureDescriptor> p_descriptor) {
	_as_descriptors.push_back(p_descriptor);
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::_encode_residency(MTL::AccelerationStructureCommandEncoder *p_enc) {
	if (sync_mode == RDM::SyncMode::HazardTracking || p_enc == nullptr) {
		return;
	}
	MetalAllocator *alloc = device_driver->get_allocator();
	if (alloc->get_heap_generation() != _resident_heaps_generation) {
		_resident_heaps.clear();
		_resident_heaps_generation = alloc->get_heaps(_resident_heaps);
	}
	if (_resident_heaps.is_empty()) {
		return;
	}
	p_enc->useHeaps(_resident_heaps.ptr(), _resident_heaps.size());
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::_fence_wait(MTL::AccelerationStructureCommandEncoder *p_enc) {
	if (!p_enc) {
		return;
	}
	if (MTL::Fence *fence = _fence_to_wait()) {
		p_enc->waitForFence(fence);
	}
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::_fence_update(MTL::AccelerationStructureCommandEncoder *p_enc) {
	if (!p_enc) {
		return;
	}
	if (MTL::Fence *fence = _fence_to_update()) {
		p_enc->updateFence(fence);
	}
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::_end_as() {
	DEV_ASSERT(type == MDCommandBufferStateType::AccelerationStructure);

	_pop_active_encoder_labels();
	_fence_update(as.encoder.get());
	as.encoder->endEncoding();
	as.reset();
	reset();
}

// GodotNext: metal_acceleration_structure
MTL::AccelerationStructureCommandEncoder *MDCommandBuffer::_ensure_as_encoder(bool p_top_level) {
	const AccelerationStructureBuildKind kind = p_top_level ? AccelerationStructureBuildKind::TLAS : AccelerationStructureBuildKind::BLAS;
	if (type == MDCommandBufferStateType::AccelerationStructure) {
		if (as.kind == kind) {
			_encode_residency(as.encoder.get());
			return as.encoder.get();
		}
		// The render graph ends the encoder at the level boundary. Reaching this
		// in a dev build means a BLAS and a TLAS were encoded in one level.
		DEV_ASSERT(as.kind == kind);
		_end_as();
	}

	switch (type) {
		case MDCommandBufferStateType::None:
			break;
		case MDCommandBufferStateType::Render:
			render_end_pass();
			break;
		case MDCommandBufferStateType::InlineRender:
			_end_inline_render();
			break;
		case MDCommandBufferStateType::Compute:
			_end_compute_dispatch();
			break;
		case MDCommandBufferStateType::Blit:
			_end_blit();
			break;
		case MDCommandBufferStateType::AccelerationStructure:
			break;
	}

	MTL::AccelerationStructureCommandEncoder *enc = command_buffer()->accelerationStructureCommandEncoder();
	ERR_FAIL_NULL_V(enc, nullptr);
	type = MDCommandBufferStateType::AccelerationStructure;
	as.kind = kind;
	as.encoder = NS::RetainPtr(enc);
	_encode_residency(enc);
	_fence_wait(enc);
	return enc;
}

// GodotNext: metal_acceleration_structure
void MDCommandBuffer::encode_acceleration_structure_build(MTL::AccelerationStructure *p_structure, MTL::AccelerationStructureDescriptor *p_descriptor, MTL::Buffer *p_scratch, NS::UInteger p_scratch_offset, bool p_top_level) {
	MTL::AccelerationStructureCommandEncoder *enc = _ensure_as_encoder(p_top_level);
	ERR_FAIL_NULL(enc);
	enc->buildAccelerationStructure(p_structure, p_descriptor, p_scratch, p_scratch_offset);
}

// GodotNext: as_build_modes
// In-place refit: source and destination are the same acceleration structure. Bottom-level refit
// passes VertexData; without it Metal ignores the vertex buffers and copies the previously
// encoded vertices. Top-level refit uses the no-options overload so the instance descriptor is read.
void MDCommandBuffer::encode_acceleration_structure_refit(MTL::AccelerationStructure *p_source, MTL::AccelerationStructureDescriptor *p_descriptor, MTL::AccelerationStructure *p_destination, MTL::Buffer *p_scratch, NS::UInteger p_scratch_offset, bool p_top_level, bool p_vertex_data) {
	MTL::AccelerationStructureCommandEncoder *enc = _ensure_as_encoder(p_top_level);
	ERR_FAIL_NULL(enc);
	if (p_vertex_data) {
		enc->refitAccelerationStructure(p_source, p_descriptor, p_destination, p_scratch, p_scratch_offset, MTL::AccelerationStructureRefitOptionVertexData);
	} else {
		enc->refitAccelerationStructure(p_source, p_descriptor, p_destination, p_scratch, p_scratch_offset);
	}
}

// GodotNext: blas_compact
MTL::AccelerationStructureCommandEncoder *MDCommandBuffer::_ensure_as_encoder_any() {
	// Compaction is neither a BLAS build nor a TLAS build. Reuse whichever
	// acceleration-structure encoder is already open so a mixed level does not
	// hit the kind assert in _ensure_as_encoder. The render graph still closes
	// the encoder between a compact copy and a TLAS build.
	if (type == MDCommandBufferStateType::AccelerationStructure && as.encoder) {
		_encode_residency(as.encoder.get());
		return as.encoder.get();
	}
	return _ensure_as_encoder(false);
}

// GodotNext: blas_compact
void MDCommandBuffer::encode_acceleration_structure_write_compacted_size(MTL::AccelerationStructure *p_structure, MTL::Buffer *p_buffer, NS::UInteger p_offset) {
	MTL::AccelerationStructureCommandEncoder *enc = _ensure_as_encoder_any();
	ERR_FAIL_NULL(enc);
	ERR_FAIL_NULL(p_structure);
	ERR_FAIL_NULL(p_buffer);
	// useHeaps marks heap resources read-only for buffer writes. The size buffer
	// is a heap allocation, so the encoder has to declare the write explicitly.
	_encode_residency(enc);
	enc->useResource(p_buffer, MTL::ResourceUsageWrite);
	enc->writeCompactedAccelerationStructureSize(p_structure, p_buffer, p_offset, MTL::DataTypeULong);
}

// GodotNext: blas_compact
void MDCommandBuffer::encode_acceleration_structure_copy_compact(MTL::AccelerationStructure *p_src, MTL::AccelerationStructure *p_dst) {
	MTL::AccelerationStructureCommandEncoder *enc = _ensure_as_encoder_any();
	ERR_FAIL_NULL(enc);
	ERR_FAIL_NULL(p_src);
	ERR_FAIL_NULL(p_dst);
	_encode_residency(enc);
	enc->copyAndCompactAccelerationStructure(p_src, p_dst);
}

// GodotNext: metal_acceleration_structure
bool MDCommandBuffer::write_instance_count(uint32_t p_count, MTL::Buffer *&r_buffer, NS::UInteger &r_offset) {
	Alloc count_alloc = allocate_arg_buffer(sizeof(uint32_t));
	if (!count_alloc.is_valid()) {
		return false;
	}
	*(uint32_t *)count_alloc.ptr = p_count;
	r_buffer = count_alloc.buffer;
	r_offset = count_alloc.offset;
	return true;
}

MDComputeShader::MDComputeShader(CharString p_name,
		Vector<UniformSet> p_sets,
		bool p_uses_argument_buffers,
		std::shared_ptr<MDLibrary> p_kernel) :
		MDShader(p_name, p_sets, p_uses_argument_buffers), kernel(std::move(p_kernel)) {
}

MDRenderShader::MDRenderShader(CharString p_name,
		Vector<UniformSet> p_sets,
		bool p_needs_view_mask_buffer,
		bool p_uses_argument_buffers,
		std::shared_ptr<MDLibrary> p_vert, std::shared_ptr<MDLibrary> p_frag) :
		MDShader(p_name, p_sets, p_uses_argument_buffers),
		needs_view_mask_buffer(p_needs_view_mask_buffer),
		vert(std::move(p_vert)),
		frag(std::move(p_frag)) {
}

void DirectEncoder::set(MTL::Texture **p_textures, NS::Range p_range) {
	if (cache.update(p_range, p_textures)) {
		switch (mode) {
			case RENDER: {
				MTL::RenderCommandEncoder *enc = static_cast<MTL::RenderCommandEncoder *>(encoder);
				enc->setVertexTextures(p_textures, p_range);
				enc->setFragmentTextures(p_textures, p_range);
			} break;
			case COMPUTE: {
				MTL::ComputeCommandEncoder *enc = static_cast<MTL::ComputeCommandEncoder *>(encoder);
				enc->setTextures(p_textures, p_range);
			} break;
		}
	}
}

void DirectEncoder::set(MTL::Buffer **p_buffers, const NS::UInteger *p_offsets, NS::Range p_range) {
	if (cache.update(p_range, p_buffers, p_offsets)) {
		switch (mode) {
			case RENDER: {
				MTL::RenderCommandEncoder *enc = static_cast<MTL::RenderCommandEncoder *>(encoder);
				enc->setVertexBuffers(p_buffers, p_offsets, p_range);
				enc->setFragmentBuffers(p_buffers, p_offsets, p_range);
			} break;
			case COMPUTE: {
				MTL::ComputeCommandEncoder *enc = static_cast<MTL::ComputeCommandEncoder *>(encoder);
				enc->setBuffers(p_buffers, p_offsets, p_range);
			} break;
		}
	}
}

// GodotNext: metal_ray_query
void DirectEncoder::set(MTL::AccelerationStructure *p_acceleration_structure, uint32_t p_index) {
	// Acceleration structures share the buffer index, so a later buffer bind must not see a stale slot.
	cache.invalidate_buffer(p_index);
	switch (mode) {
		case RENDER: {
			MTL::RenderCommandEncoder *enc = static_cast<MTL::RenderCommandEncoder *>(encoder);
			enc->setVertexAccelerationStructure(p_acceleration_structure, p_index);
			enc->setFragmentAccelerationStructure(p_acceleration_structure, p_index);
		} break;
		case COMPUTE: {
			MTL::ComputeCommandEncoder *enc = static_cast<MTL::ComputeCommandEncoder *>(encoder);
			enc->setAccelerationStructure(p_acceleration_structure, p_index);
		} break;
	}
}

void DirectEncoder::set(MTL::Buffer *p_buffer, NS::UInteger p_offset, uint32_t p_index) {
	if (cache.update(p_buffer, p_offset, p_index)) {
		switch (mode) {
			case RENDER: {
				MTL::RenderCommandEncoder *enc = static_cast<MTL::RenderCommandEncoder *>(encoder);
				enc->setVertexBuffer(p_buffer, p_offset, p_index);
				enc->setFragmentBuffer(p_buffer, p_offset, p_index);
			} break;
			case COMPUTE: {
				MTL::ComputeCommandEncoder *enc = static_cast<MTL::ComputeCommandEncoder *>(encoder);
				enc->setBuffer(p_buffer, p_offset, p_index);
			} break;
		}
	}
}

void DirectEncoder::set(MTL::SamplerState **p_samplers, NS::Range p_range) {
	if (cache.update(p_range, p_samplers)) {
		switch (mode) {
			case RENDER: {
				MTL::RenderCommandEncoder *enc = static_cast<MTL::RenderCommandEncoder *>(encoder);
				enc->setVertexSamplerStates(p_samplers, p_range);
				enc->setFragmentSamplerStates(p_samplers, p_range);
			} break;
			case COMPUTE: {
				MTL::ComputeCommandEncoder *enc = static_cast<MTL::ComputeCommandEncoder *>(encoder);
				enc->setSamplerStates(p_samplers, p_range);
			} break;
		}
	}
}

GODOT_CLANG_WARNING_PUSH_AND_IGNORE("-Wunguarded-availability-new")

void MDCommandBuffer::_set_arg_buffer(MTL::RenderCommandEncoder *p_enc, const UniformSet &p_shader_set, MTL::Buffer *p_buffer, uint32_t p_offset, uint32_t p_set_index) {
	if (p_shader_set.active_stages.has_flag(RDD::SHADER_STAGE_VERTEX_BIT) &&
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_VERTEX].update(p_buffer, p_offset, p_set_index)) {
		p_enc->setVertexBuffer(p_buffer, p_offset, p_set_index);
	}
	if (p_shader_set.active_stages.has_flag(RDD::SHADER_STAGE_FRAGMENT_BIT) &&
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_FRAGMENT].update(p_buffer, p_offset, p_set_index)) {
		p_enc->setFragmentBuffer(p_buffer, p_offset, p_set_index);
	}
	// GodotNext: rd_mesh_shaders (#178)
	if (p_shader_set.active_stages.has_flag(RDD::SHADER_STAGE_TASK_BIT) &&
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_OBJECT].update(p_buffer, p_offset, p_set_index)) {
		p_enc->setObjectBuffer(p_buffer, p_offset, p_set_index);
	}
	if (p_shader_set.active_stages.has_flag(RDD::SHADER_STAGE_MESH_BIT) &&
			render.arg_buffer_cache[RenderState::ARG_BUFFER_STAGE_MESH].update(p_buffer, p_offset, p_set_index)) {
		p_enc->setMeshBuffer(p_buffer, p_offset, p_set_index);
	}
}

void MDCommandBuffer::_bind_uniforms_argument_buffers(MDUniformSet *p_set, MDShader *p_shader, uint32_t p_set_index, uint32_t p_dynamic_offsets) {
	DEV_ASSERT(p_shader->uses_argument_buffers);
	DEV_ASSERT(render.encoder.get() != nullptr);

	MTL::RenderCommandEncoder *enc = render.encoder.get();

	// GodotNext: rd_bindless (#176). The table is the set's argument buffer, and its resources become
	// resident with the set's.
	if (p_set->bindless_table != nullptr) {
		render.resource_tracker.merge_from(p_set->bindless_table->get_usage_to_resources());
		_set_arg_buffer(enc, p_shader->sets[p_set_index], p_set->bindless_table->buffer.get(), 0, p_set_index);
		return;
	}

	render.resource_tracker.merge_from(p_set->usage_to_resources);

	const UniformSet &shader_set = p_shader->sets[p_set_index];

	// Check if this set has dynamic uniforms.
	if (!shader_set.dynamic_uniforms.is_empty()) {
		// Allocate from the ring buffer.
		uint32_t buffer_size = p_set->arg_buffer_data.size();
		MDRingBuffer::Allocation alloc = allocate_arg_buffer(buffer_size);

		// Copy the base argument buffer data.
		memcpy(alloc.ptr, p_set->arg_buffer_data.ptr(), buffer_size);

		// Update dynamic buffer GPU addresses.
		uint64_t *ptr = (uint64_t *)alloc.ptr;
		DynamicOffsetLayout layout = p_shader->dynamic_offset_layout;
		uint32_t dynamic_index = 0;

		for (uint32_t i : shader_set.dynamic_uniforms) {
			const RDD::BoundUniform &uniform = p_set->uniforms[i];
			const UniformInfo &ui = shader_set.uniforms[i];
			const UniformInfo::Indexes &idx = ui.arg_buffer;

			uint32_t shift = layout.get_offset_index_shift(p_set_index, dynamic_index);
			dynamic_index++;
			uint32_t frame_idx = (p_dynamic_offsets >> shift) & 0xf;

			const MetalBufferDynamicInfo *buf_info = (const MetalBufferDynamicInfo *)uniform.ids[0].id;
			uint64_t gpu_address = buf_info->buffer.get()->gpuAddress() + frame_idx * buf_info->size_bytes;
			*(uint64_t *)(ptr + idx.buffer) = gpu_address;
		}

		_set_arg_buffer(enc, shader_set, alloc.buffer, alloc.offset, p_set_index);
	} else {
		_set_arg_buffer(enc, shader_set, p_set->arg_buffer.buffer.get(), 0, p_set_index);
	}
}

void MDCommandBuffer::_bind_uniforms_direct(MDUniformSet *p_set, MDShader *p_shader, DirectEncoder p_enc, uint32_t p_set_index, uint32_t p_dynamic_offsets) {
	DEV_ASSERT(!p_shader->uses_argument_buffers);

	const UniformSet &set = p_shader->sets[p_set_index];
	DynamicOffsetLayout layout = p_shader->dynamic_offset_layout;
	uint32_t dynamic_index = 0;

	for (uint32_t i = 0; i < MIN(p_set->uniforms.size(), set.uniforms.size()); i++) {
		const RDD::BoundUniform &uniform = p_set->uniforms[i];
		const UniformInfo &ui = set.uniforms[i];
		const UniformInfo::Indexes &indexes = ui.slot;

		uint32_t frame_idx;
		if (uniform.is_dynamic()) {
			uint32_t shift = layout.get_offset_index_shift(p_set_index, dynamic_index);
			dynamic_index++;
			frame_idx = (p_dynamic_offsets >> shift) & 0xf;
		} else {
			frame_idx = 0;
		}

		switch (uniform.type) {
			case RDD::UNIFORM_TYPE_SAMPLER: {
				size_t count = uniform.ids.size();
				MTL::SamplerState **objects = ALLOCA_ARRAY(MTL::SamplerState *, count);
				for (size_t j = 0; j < count; j += 1) {
					objects[j] = rid::get<MTL::SamplerState>(uniform.ids[j]);
				}
				NS::Range sampler_range = { indexes.sampler, count };
				p_enc.set(objects, sampler_range);
			} break;
			case RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
				size_t count = uniform.ids.size() / 2;
				MTL::Texture **textures = ALLOCA_ARRAY(MTL::Texture *, count);
				MTL::SamplerState **samplers = ALLOCA_ARRAY(MTL::SamplerState *, count);
				for (uint32_t j = 0; j < count; j += 1) {
					samplers[j] = rid::get<MTL::SamplerState>(uniform.ids[j * 2 + 0]);
					textures[j] = rid::get<RDM::TextureInfo>(uniform.ids[j * 2 + 1])->texture.get();
				}
				NS::Range sampler_range = { indexes.sampler, count };
				NS::Range texture_range = { indexes.texture, count };
				p_enc.set(samplers, sampler_range);
				p_enc.set(textures, texture_range);
			} break;
			case RDD::UNIFORM_TYPE_TEXTURE: {
				size_t count = uniform.ids.size();
				MTL::Texture **objects = ALLOCA_ARRAY(MTL::Texture *, count);
				for (size_t j = 0; j < count; j += 1) {
					objects[j] = rid::get<RDM::TextureInfo>(uniform.ids[j])->texture.get();
				}
				NS::Range texture_range = { indexes.texture, count };
				p_enc.set(objects, texture_range);
			} break;
			case RDD::UNIFORM_TYPE_IMAGE: {
				size_t count = uniform.ids.size();
				MTL::Texture **objects = ALLOCA_ARRAY(MTL::Texture *, count);
				for (size_t j = 0; j < count; j += 1) {
					objects[j] = rid::get<RDM::TextureInfo>(uniform.ids[j])->texture.get();
				}
				NS::Range texture_range = { indexes.texture, count };
				p_enc.set(objects, texture_range);

				if (indexes.buffer != UINT32_MAX) {
					// Emulated atomic image access.
					MTL::Buffer **bufs = ALLOCA_ARRAY(MTL::Buffer *, count);
					for (size_t j = 0; j < count; j += 1) {
						MTL::Texture *obj = objects[j];
						MTL::Texture *tex = obj->parentTexture() ? obj->parentTexture() : obj;
						bufs[j] = tex->buffer();
					}
					NS::UInteger *offs = ALLOCA_ARRAY(NS::UInteger, count);
					bzero(offs, sizeof(NS::UInteger) * count);
					NS::Range buffer_range = { indexes.buffer, count };
					p_enc.set(bufs, offs, buffer_range);
				}
			} break;
			case RDD::UNIFORM_TYPE_TEXTURE_BUFFER: {
				ERR_PRINT("not implemented: UNIFORM_TYPE_TEXTURE_BUFFER");
			} break;
			case RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER: {
				ERR_PRINT("not implemented: UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER");
			} break;
			case RDD::UNIFORM_TYPE_IMAGE_BUFFER: {
				CRASH_NOW_MSG("not implemented: UNIFORM_TYPE_IMAGE_BUFFER");
			} break;
			case RDD::UNIFORM_TYPE_UNIFORM_BUFFER:
			case RDD::UNIFORM_TYPE_STORAGE_BUFFER: {
				const RDM::BufferInfo *buf_info = (const RDM::BufferInfo *)uniform.ids[0].id;
				p_enc.set(buf_info->buffer.get(), 0, indexes.buffer);
			} break;
			case RDD::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case RDD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
				const MetalBufferDynamicInfo *buf_info = (const MetalBufferDynamicInfo *)uniform.ids[0].id;
				p_enc.set(buf_info->buffer.get(), frame_idx * buf_info->size_bytes, indexes.buffer);
			} break;
			case RDD::UNIFORM_TYPE_INPUT_ATTACHMENT: {
				size_t count = uniform.ids.size();
				MTL::Texture **objects = ALLOCA_ARRAY(MTL::Texture *, count);
				for (size_t j = 0; j < count; j += 1) {
					objects[j] = rid::get<RDM::TextureInfo>(uniform.ids[j])->texture.get();
				}
				NS::Range texture_range = { indexes.texture, count };
				p_enc.set(objects, texture_range);
			} break;
			// GodotNext: metal_ray_query
			case RDD::UNIFORM_TYPE_ACCELERATION_STRUCTURE: {
				const RDM::AccelerationStructureInfo *as_info = (const RDM::AccelerationStructureInfo *)uniform.ids[0].id;
				p_enc.set(as_info->as.get(), indexes.buffer);
			} break;
			default: {
				DEV_ASSERT(false);
			}
		}
	}
}

void MDCommandBuffer::_bind_uniforms_argument_buffers_compute(MDUniformSet *p_set, MDShader *p_shader, uint32_t p_set_index, uint32_t p_dynamic_offsets) {
	DEV_ASSERT(p_shader->uses_argument_buffers);
	DEV_ASSERT(compute.encoder);

	MTL::ComputeCommandEncoder *enc = compute.encoder.get();

	// GodotNext: rd_bindless (#176). The table is the set's argument buffer.
	if (p_set->bindless_table != nullptr) {
		compute.resource_tracker.merge_from(p_set->bindless_table->get_usage_to_resources());
		enc->setBuffer(p_set->bindless_table->buffer.get(), 0, p_set_index);
		return;
	}

	compute.resource_tracker.merge_from(p_set->usage_to_resources);

	const UniformSet &shader_set = p_shader->sets[p_set_index];
	if (!shader_set.active_stages.has_flag(RDD::SHADER_STAGE_COMPUTE_BIT)) {
		return;
	}

	// GodotNext: metal_acceleration_structure
	// useHeaps makes heap resources resident for read-only argument-buffer access.
	// A storage buffer the shader writes needs useResource, or shader validation
	// reports the store as Read Only. Hazard tracking already encodes this usage.
	if (sync_mode != RDM::SyncMode::HazardTracking) {
		const uint32_t uniform_count = MIN((uint32_t)p_set->uniforms.size(), (uint32_t)shader_set.uniforms.size());
		for (uint32_t i = 0; i < uniform_count; i++) {
			const UniformInfo &ui = shader_set.uniforms[i];
			const RDD::BoundUniform &uniform = p_set->uniforms[i];
			if ((ui.usage & MTL::ResourceUsageWrite) == 0 || uniform.ids.is_empty()) {
				continue;
			}
			MTL::Buffer *buffer = nullptr;
			if (uniform.type == RDD::UNIFORM_TYPE_STORAGE_BUFFER) {
				const RDM::BufferInfo *info = (const RDM::BufferInfo *)uniform.ids[0].id;
				buffer = info != nullptr ? info->buffer.get() : nullptr;
			} else if (uniform.type == RDD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC) {
				const MetalBufferDynamicInfo *info = (const MetalBufferDynamicInfo *)uniform.ids[0].id;
				buffer = info != nullptr ? info->buffer.get() : nullptr;
			}
			if (buffer != nullptr) {
				enc->useResource(buffer, ui.usage);
			}
		}
	}

	// Check if this set has dynamic uniforms.
	if (!shader_set.dynamic_uniforms.is_empty()) {
		// Allocate from the ring buffer.
		uint32_t buffer_size = p_set->arg_buffer_data.size();
		MDRingBuffer::Allocation alloc = allocate_arg_buffer(buffer_size);

		// Copy the base argument buffer data.
		memcpy(alloc.ptr, p_set->arg_buffer_data.ptr(), buffer_size);

		// Update dynamic buffer GPU addresses.
		uint64_t *ptr = (uint64_t *)alloc.ptr;
		DynamicOffsetLayout layout = p_shader->dynamic_offset_layout;
		uint32_t dynamic_index = 0;

		for (uint32_t i : shader_set.dynamic_uniforms) {
			const RDD::BoundUniform &uniform = p_set->uniforms[i];
			const UniformInfo &ui = shader_set.uniforms[i];
			const UniformInfo::Indexes &idx = ui.arg_buffer;

			uint32_t shift = layout.get_offset_index_shift(p_set_index, dynamic_index);
			dynamic_index++;
			uint32_t frame_idx = (p_dynamic_offsets >> shift) & 0xf;

			const MetalBufferDynamicInfo *buf_info = (const MetalBufferDynamicInfo *)uniform.ids[0].id;
			uint64_t gpu_address = buf_info->buffer.get()->gpuAddress() + frame_idx * buf_info->size_bytes;
			*(uint64_t *)(ptr + idx.buffer) = gpu_address;
		}

		enc->setBuffer(alloc.buffer, alloc.offset, p_set_index);
	} else {
		enc->setBuffer(p_set->arg_buffer.buffer.get(), 0, p_set_index);
	}
}

GODOT_CLANG_WARNING_POP
