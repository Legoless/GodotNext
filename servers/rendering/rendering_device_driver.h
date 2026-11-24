/**************************************************************************/
/*  rendering_device_driver.h                                             */
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

#pragma once

// ***********************************************************************************
// RenderingDeviceDriver - Design principles
// -----------------------------------------
// - Very little validation is done, and normally only in dev or debug builds.
// - Error reporting is generally simple: returning an id of 0 or a false boolean.
// - Certain enums/constants/structs follow Vulkan values/layout. That makes things easier for RDDVulkan (it asserts compatibility).
// - We allocate as little as possible in functions expected to be quick (a counterexample is loading/saving shaders) and use alloca() whenever suitable.
// - We try to back opaque ids with the native ones or memory addresses.
// - When using bookkeeping structures because the actual API id of a resource is not enough, we use a PagedAllocator.
// - Every struct has default initializers.
// - Using VectorView to take array-like arguments. Vector<uint8_t> is an exception (an indiom for "BLOB").
// - If a driver needs some higher-level information (the kind of info RenderingDevice keeps), it shall store a copy of what it needs.
//   There's no backwards communication from the driver to query data from RenderingDevice.
// ***********************************************************************************

#include "core/object/object.h"
#include "core/templates/paged_allocator.h"
#include "servers/rendering/rendering_context_driver.h"
#include "servers/rendering/rendering_device_commons.h"

class RenderingShaderContainer;
class RenderingShaderContainerFormat;

// These utilities help drivers avoid allocations.
#define ALLOCA(m_size) ((m_size != 0) ? alloca(m_size) : nullptr)
#define ALLOCA_ARRAY(m_type, m_count) ((m_type *)ALLOCA(sizeof(m_type) * (m_count)))
#define ALLOCA_SINGLE(m_type) ALLOCA_ARRAY(m_type, 1)

// This helps forwarding certain arrays to the API with confidence.
#define ARRAYS_COMPATIBLE(m_type_a, m_type_b) (sizeof(m_type_a) == sizeof(m_type_b) && alignof(m_type_a) == alignof(m_type_b))
// This is used when you also need to ensure structured types are compatible field-by-field.
// TODO: The fieldwise check is unimplemented, but still this one is useful, as a strong annotation about the needs.
#define ARRAYS_COMPATIBLE_FIELDWISE(m_type_a, m_type_b) ARRAYS_COMPATIBLE(m_type_a, m_type_b)
// Another utility, to make it easy to compare members of different enums, which is not fine with some compilers.
#define ENUM_MEMBERS_EQUAL(m_a, m_b) ((int64_t)m_a == (int64_t)m_b)

// This helps using a single paged allocator for many resource types.
template <typename... RESOURCE_TYPES>
struct VersatileResourceTemplate {
	static constexpr size_t RESOURCE_SIZES[] = { sizeof(RESOURCE_TYPES)... };
	static constexpr size_t MAX_RESOURCE_SIZE = Span<size_t>(RESOURCE_SIZES).max();
	uint8_t data[MAX_RESOURCE_SIZE];

	template <typename T>
	static T *allocate(PagedAllocator<VersatileResourceTemplate, true> &p_allocator) {
		T *obj = (T *)p_allocator.alloc();
		memnew_placement(obj, T);
		return obj;
	}

	template <typename T>
	static void free(PagedAllocator<VersatileResourceTemplate, true> &p_allocator, T *p_object) {
		p_object->~T();
		p_allocator.free((VersatileResourceTemplate *)p_object);
	}
};

class RenderingDeviceDriver : public RenderingDeviceCommons {
	GDSOFTCLASS(RenderingDeviceDriver, RenderingDeviceCommons);

public:
	struct ID {
		uint64_t id = 0;
		_ALWAYS_INLINE_ ID() = default;
		_ALWAYS_INLINE_ ID(uint64_t p_id) :
				id(p_id) {}
	};

#define DEFINE_ID(m_name) \
	struct m_name##ID : public ID { \
		_ALWAYS_INLINE_ explicit operator bool() const { \
			return id != 0; \
		} \
		_ALWAYS_INLINE_ m_name##ID &operator=(m_name##ID p_other) { \
			id = p_other.id; \
			return *this; \
		} \
		_ALWAYS_INLINE_ bool operator<(const m_name##ID &p_other) const { \
			return id < p_other.id; \
		} \
		_ALWAYS_INLINE_ bool operator==(const m_name##ID &p_other) const { \
			return id == p_other.id; \
		} \
		_ALWAYS_INLINE_ bool operator!=(const m_name##ID &p_other) const { \
			return id != p_other.id; \
		} \
		_ALWAYS_INLINE_ m_name##ID(const m_name##ID &p_other) : ID(p_other.id) {} \
		_ALWAYS_INLINE_ explicit m_name##ID(uint64_t p_int) : ID(p_int) {} \
		_ALWAYS_INLINE_ explicit m_name##ID(void *p_ptr) : ID((uint64_t)p_ptr) {} \
		_ALWAYS_INLINE_ m_name##ID() = default; \
	};

	// Id types declared before anything else to prevent cyclic dependencies between the different concerns.
	DEFINE_ID(Buffer);
	DEFINE_ID(Texture);
	DEFINE_ID(Sampler);
	DEFINE_ID(VertexFormat);
	DEFINE_ID(CommandQueue);
	DEFINE_ID(CommandQueueFamily);
	DEFINE_ID(CommandPool);
	DEFINE_ID(CommandBuffer);
	DEFINE_ID(SwapChain);
	DEFINE_ID(Framebuffer);
	DEFINE_ID(Shader);
	DEFINE_ID(UniformSet);
	DEFINE_ID(Pipeline);
	DEFINE_ID(RenderPass);
	DEFINE_ID(QueryPool);
	DEFINE_ID(Fence);
	DEFINE_ID(Semaphore);
	DEFINE_ID(AccelerationStructure);
	DEFINE_ID(RaytracingPipeline);
	// GodotNext: rd_bindless (#176)
	DEFINE_ID(BindlessTable);

public:
	/*****************/
	/**** GENERIC ****/
	/*****************/

	virtual Error initialize(uint32_t p_device_index, uint32_t p_frame_count) = 0;

	/****************/
	/**** MEMORY ****/
	/****************/

	enum MemoryAllocationType {
		MEMORY_ALLOCATION_TYPE_CPU, // For images, CPU allocation also means linear, GPU is tiling optimal.
		MEMORY_ALLOCATION_TYPE_GPU,
		MEMORY_ALLOCATION_TYPE_GPU_MAPPABLE, // Supported on UMA devices or discrete devices with ReBAR.
	};

	/*****************/
	/**** BUFFERS ****/
	/*****************/

	enum BufferUsageBits {
		BUFFER_USAGE_TRANSFER_FROM_BIT = (1 << 0),
		BUFFER_USAGE_TRANSFER_TO_BIT = (1 << 1),
		BUFFER_USAGE_TEXEL_BIT = (1 << 2),
		BUFFER_USAGE_UNIFORM_BIT = (1 << 4),
		BUFFER_USAGE_STORAGE_BIT = (1 << 5),
		BUFFER_USAGE_INDEX_BIT = (1 << 6),
		BUFFER_USAGE_VERTEX_BIT = (1 << 7),
		BUFFER_USAGE_INDIRECT_BIT = (1 << 8),
		BUFFER_USAGE_SHADER_BINDING_TABLE_BIT = (1 << 10),
		BUFFER_USAGE_DEVICE_ADDRESS_BIT = (1 << 17),
		BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT = (1 << 19),
		BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT = (1 << 20),
		// There are no Vulkan-equivalent. Try to use unused/unclaimed bits.
		BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT = (1 << 31),
	};

	enum {
		BUFFER_WHOLE_SIZE = ~0ULL
	};

	/** Allocates a new GPU buffer. Must be destroyed with buffer_free().
	 * @param p_size The size in bytes of the buffer.
	 * @param p_usage Usage flags.
	 * @param p_allocation_type See MemoryAllocationType.
	 * @param p_frames_drawn Used for debug checks when BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT is set.
	 * @return the buffer.
	 */
	virtual BufferID buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) = 0;
	// Only for a buffer with BUFFER_USAGE_TEXEL_BIT.
	virtual bool buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) = 0;
	virtual void buffer_free(BufferID p_buffer) = 0;
	virtual uint64_t buffer_get_allocation_size(BufferID p_buffer) = 0;
	virtual uint8_t *buffer_map(BufferID p_buffer) = 0;
	virtual void buffer_unmap(BufferID p_buffer) = 0;
	virtual uint8_t *buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) = 0;
	virtual uint64_t buffer_get_dynamic_offsets(Span<BufferID> p_buffers) = 0;
	virtual void buffer_flush(BufferID p_buffer) {}
	// Only for a buffer with BUFFER_USAGE_DEVICE_ADDRESS_BIT.
	virtual uint64_t buffer_get_device_address(BufferID p_buffer) = 0;

	/*****************/
	/**** TEXTURE ****/
	/*****************/

	struct TextureView {
		DataFormat format = DATA_FORMAT_MAX;
		TextureSwizzle swizzle_r = TEXTURE_SWIZZLE_R;
		TextureSwizzle swizzle_g = TEXTURE_SWIZZLE_G;
		TextureSwizzle swizzle_b = TEXTURE_SWIZZLE_B;
		TextureSwizzle swizzle_a = TEXTURE_SWIZZLE_A;
	};

	enum TextureLayout {
		TEXTURE_LAYOUT_UNDEFINED,
		TEXTURE_LAYOUT_GENERAL,
		TEXTURE_LAYOUT_STORAGE_OPTIMAL,
		TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		TEXTURE_LAYOUT_COPY_SRC_OPTIMAL,
		TEXTURE_LAYOUT_COPY_DST_OPTIMAL,
		TEXTURE_LAYOUT_RESOLVE_SRC_OPTIMAL,
		TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL,
		TEXTURE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL,
		TEXTURE_LAYOUT_FRAGMENT_DENSITY_MAP_ATTACHMENT_OPTIMAL,
		TEXTURE_LAYOUT_MAX
	};

	enum TextureAspect {
		TEXTURE_ASPECT_COLOR = 0,
		TEXTURE_ASPECT_DEPTH = 1,
		TEXTURE_ASPECT_STENCIL = 2,
		TEXTURE_ASPECT_MAX
	};

	enum TextureUsageMethod {
		TEXTURE_USAGE_VRS_FRAGMENT_SHADING_RATE_BIT = TEXTURE_USAGE_MAX_BIT << 1,
		TEXTURE_USAGE_VRS_FRAGMENT_DENSITY_MAP_BIT = TEXTURE_USAGE_MAX_BIT << 2,
	};

	enum TextureAspectBits {
		TEXTURE_ASPECT_COLOR_BIT = (1 << TEXTURE_ASPECT_COLOR),
		TEXTURE_ASPECT_DEPTH_BIT = (1 << TEXTURE_ASPECT_DEPTH),
		TEXTURE_ASPECT_STENCIL_BIT = (1 << TEXTURE_ASPECT_STENCIL),
	};

	struct TextureSubresource {
		TextureAspect aspect = TEXTURE_ASPECT_COLOR;
		uint32_t layer = 0;
		uint32_t mipmap = 0;
	};

	struct TextureSubresourceLayers {
		BitField<TextureAspectBits> aspect = {};
		uint32_t mipmap = 0;
		uint32_t base_layer = 0;
		uint32_t layer_count = 0;
	};

	struct TextureSubresourceRange {
		BitField<TextureAspectBits> aspect = {};
		uint32_t base_mipmap = 0;
		uint32_t mipmap_count = 0;
		uint32_t base_layer = 0;
		uint32_t layer_count = 0;
	};

	struct TextureCopyableLayout {
		uint64_t size = 0;
		uint64_t row_pitch = 0;
	};

	virtual TextureID texture_create(const TextureFormat &p_format, const TextureView &p_view) = 0;
	virtual TextureID texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) = 0;
	// texture_create_shared_*() can only use original, non-view textures as original. RenderingDevice is responsible for ensuring that.
	virtual TextureID texture_create_shared(TextureID p_original_texture, const TextureView &p_view) = 0;
	virtual TextureID texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) = 0;
	virtual void texture_free(TextureID p_texture) = 0;
	virtual uint64_t texture_get_allocation_size(TextureID p_texture) = 0;
	// Returns a texture layout for buffer <-> texture copies. If you are copying multiple texture subresources to/from the same buffer,
	// you are responsible for correctly aligning the start offset for every buffer region. See API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT.
	virtual void texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) = 0;
	// Returns the data of a texture layer for a CPU texture that was created with TEXTURE_USAGE_CPU_READ_BIT.
	virtual Vector<uint8_t> texture_get_data(TextureID p_texture, uint32_t p_layer) = 0;
	virtual BitField<TextureUsageBits> texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) = 0;
	virtual bool texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) = 0;

	/*****************/
	/**** SAMPLER ****/
	/*****************/

	virtual SamplerID sampler_create(const SamplerState &p_state) = 0;
	virtual void sampler_free(SamplerID p_sampler) = 0;
	virtual bool sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) = 0;

	/**********************/
	/**** VERTEX ARRAY ****/
	/**********************/

	virtual VertexFormatID vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) = 0;
	virtual void vertex_format_free(VertexFormatID p_vertex_format) = 0;

	/******************/
	/**** BARRIERS ****/
	/******************/

	enum PipelineStageBits {
		PIPELINE_STAGE_TOP_OF_PIPE_BIT = (1 << 0),
		PIPELINE_STAGE_DRAW_INDIRECT_BIT = (1 << 1),
		PIPELINE_STAGE_VERTEX_INPUT_BIT = (1 << 2),
		PIPELINE_STAGE_VERTEX_SHADER_BIT = (1 << 3),
		PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT = (1 << 4),
		PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT = (1 << 5),
		PIPELINE_STAGE_GEOMETRY_SHADER_BIT = (1 << 6),
		PIPELINE_STAGE_FRAGMENT_SHADER_BIT = (1 << 7),
		PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT = (1 << 8),
		PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT = (1 << 9),
		PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = (1 << 10),
		PIPELINE_STAGE_COMPUTE_SHADER_BIT = (1 << 11),
		PIPELINE_STAGE_COPY_BIT = (1 << 12),
		PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = (1 << 13),
		PIPELINE_STAGE_RESOLVE_BIT = (1 << 14),
		PIPELINE_STAGE_ALL_GRAPHICS_BIT = (1 << 15),
		PIPELINE_STAGE_ALL_COMMANDS_BIT = (1 << 16),
		PIPELINE_STAGE_CLEAR_STORAGE_BIT = (1 << 17),
		// GodotNext: rd_mesh_shaders (#178). The values of VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT and
		// VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT.
		PIPELINE_STAGE_TASK_SHADER_BIT = (1 << 19),
		PIPELINE_STAGE_MESH_SHADER_BIT = (1 << 20),
		PIPELINE_STAGE_RAY_TRACING_SHADER_BIT = (1 << 21),
		PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT = (1 << 22),
		PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT = (1 << 23),
		PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT = (1 << 25),
	};

	enum BarrierAccessBits {
		BARRIER_ACCESS_INDIRECT_COMMAND_READ_BIT = (1 << 0),
		BARRIER_ACCESS_INDEX_READ_BIT = (1 << 1),
		BARRIER_ACCESS_VERTEX_ATTRIBUTE_READ_BIT = (1 << 2),
		BARRIER_ACCESS_UNIFORM_READ_BIT = (1 << 3),
		BARRIER_ACCESS_INPUT_ATTACHMENT_READ_BIT = (1 << 4),
		BARRIER_ACCESS_SHADER_READ_BIT = (1 << 5),
		BARRIER_ACCESS_SHADER_WRITE_BIT = (1 << 6),
		BARRIER_ACCESS_COLOR_ATTACHMENT_READ_BIT = (1 << 7),
		BARRIER_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = (1 << 8),
		BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT = (1 << 9),
		BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT = (1 << 10),
		BARRIER_ACCESS_COPY_READ_BIT = (1 << 11),
		BARRIER_ACCESS_COPY_WRITE_BIT = (1 << 12),
		BARRIER_ACCESS_HOST_READ_BIT = (1 << 13),
		BARRIER_ACCESS_HOST_WRITE_BIT = (1 << 14),
		BARRIER_ACCESS_MEMORY_READ_BIT = (1 << 15),
		BARRIER_ACCESS_MEMORY_WRITE_BIT = (1 << 16),
		BARRIER_ACCESS_ACCELERATION_STRUCTURE_READ_BIT = (1 << 21),
		BARRIER_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT = (1 << 22),
		BARRIER_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT = (1 << 23),
		BARRIER_ACCESS_FRAGMENT_DENSITY_MAP_ATTACHMENT_READ_BIT = (1 << 24),
		BARRIER_ACCESS_RESOLVE_READ_BIT = (1 << 25),
		BARRIER_ACCESS_RESOLVE_WRITE_BIT = (1 << 26),
		BARRIER_ACCESS_STORAGE_CLEAR_BIT = (1 << 27),
	};

	// https://github.com/godotengine/godot/pull/110360 - "MemoryBarrier" conflicts with Windows header defines
	struct MemoryAccessBarrier {
		BitField<BarrierAccessBits> src_access = {};
		BitField<BarrierAccessBits> dst_access = {};
	};

	struct BufferBarrier {
		BufferID buffer;
		BitField<BarrierAccessBits> src_access = {};
		BitField<BarrierAccessBits> dst_access = {};
		uint64_t offset = 0;
		uint64_t size = 0;
	};

	struct TextureBarrier {
		TextureID texture;
		BitField<BarrierAccessBits> src_access = {};
		BitField<BarrierAccessBits> dst_access = {};
		TextureLayout prev_layout = TEXTURE_LAYOUT_UNDEFINED;
		TextureLayout next_layout = TEXTURE_LAYOUT_UNDEFINED;
		TextureSubresourceRange subresources;
	};

	struct AccelerationStructureBarrier {
		AccelerationStructureID acceleration_structure;
		BitField<BarrierAccessBits> src_access;
		BitField<BarrierAccessBits> dst_access;
		uint64_t offset = 0;
		uint64_t size = 0;
	};

	virtual void command_pipeline_barrier(
			CommandBufferID p_cmd_buffer,
			BitField<PipelineStageBits> p_src_stages,
			BitField<PipelineStageBits> p_dst_stages,
			VectorView<MemoryAccessBarrier> p_memory_barriers,
			VectorView<BufferBarrier> p_buffer_barriers,
			VectorView<TextureBarrier> p_texture_barriers,
			VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) = 0;

	/****************/
	/**** FENCES ****/
	/****************/

	virtual FenceID fence_create() = 0;
	virtual Error fence_wait(FenceID p_fence) = 0;
	virtual void fence_free(FenceID p_fence) = 0;

	/********************/
	/**** SEMAPHORES ****/
	/********************/

	virtual SemaphoreID semaphore_create() = 0;
	virtual void semaphore_free(SemaphoreID p_semaphore) = 0;

	/*************************/
	/**** COMMAND BUFFERS ****/
	/*************************/

	// ----- QUEUE FAMILY -----

	enum CommandQueueFamilyBits {
		COMMAND_QUEUE_FAMILY_GRAPHICS_BIT = 0x1,
		COMMAND_QUEUE_FAMILY_COMPUTE_BIT = 0x2,
		COMMAND_QUEUE_FAMILY_TRANSFER_BIT = 0x4
	};

	// The requested command queue family must support all specified bits or it'll fail to return a valid family otherwise. If a valid surface is specified, the queue must support presenting to it.
	// It is valid to specify no bits and a valid surface: in this case, the dedicated presentation queue family will be the preferred option.
	virtual CommandQueueFamilyID command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface = 0) = 0;

	// ----- QUEUE -----

	virtual CommandQueueID command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue = false) = 0;
	virtual Error command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) = 0;
	virtual void command_queue_free(CommandQueueID p_cmd_queue) = 0;
	// GodotNext: async_compute (#174). RenderingDevice calls this once, right after it creates the async
	// compute queue on a family other than the main one, and before it creates the resources the
	// queues share. The Vulkan driver then creates buffers and images shareable across the queue
	// families in use.
	virtual void command_queue_set_async_compute(CommandQueueID p_cmd_queue) {}

	// ----- POOL -----

	enum CommandBufferType {
		COMMAND_BUFFER_TYPE_PRIMARY,
		COMMAND_BUFFER_TYPE_SECONDARY,
	};

	virtual CommandPoolID command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) = 0;
	virtual bool command_pool_reset(CommandPoolID p_cmd_pool) = 0;
	virtual void command_pool_free(CommandPoolID p_cmd_pool) = 0;

	// ----- BUFFER -----

	virtual CommandBufferID command_buffer_create(CommandPoolID p_cmd_pool) = 0;
	virtual bool command_buffer_begin(CommandBufferID p_cmd_buffer) = 0;
	virtual bool command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) = 0;
	virtual void command_buffer_end(CommandBufferID p_cmd_buffer) = 0;
	virtual void command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) = 0;

	/********************/
	/**** SWAP CHAIN ****/
	/********************/

	// The swap chain won't be valid for use until it is resized at least once.
	virtual SwapChainID swap_chain_create(RenderingContextDriver::SurfaceID p_surface) = 0;

	// The swap chain must not be in use when a resize is requested. Wait until all rendering associated to the swap chain is finished before resizing it.
	virtual Error swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) = 0;

	// Acquire the framebuffer that can be used for drawing. This must be called only once every time a new frame will be rendered.
	virtual FramebufferID swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) = 0;

	// Retrieve the render pass that can be used to draw on the swap chain's framebuffers.
	virtual RenderPassID swap_chain_get_render_pass(SwapChainID p_swap_chain) = 0;

	// Retrieve the rotation in degrees to apply as a pre-transform. Usually 0 on PC. May be 0, 90, 180 & 270 on Android.
	virtual int swap_chain_get_pre_rotation_degrees(SwapChainID p_swap_chain) { return 0; }

	// Retrieve the format used by the swap chain's framebuffers.
	virtual DataFormat swap_chain_get_format(SwapChainID p_swap_chain) = 0;

	// Retrieve the color space used by the swap chain's framebuffers.
	virtual ColorSpace swap_chain_get_color_space(SwapChainID p_swap_chain) = 0;

	// Retrieve whether the swapchain supports our preferred HDR formats.
	virtual bool swap_chain_get_hdr_output_supported(SwapChainID p_swap_chain) = 0;

	// Tells the swapchain the max_fps so it can use the proper frame pacing.
	// Android uses this with Swappy library. Some implementations or platforms may ignore this hint.
	virtual void swap_chain_set_max_fps(SwapChainID p_swap_chain, int p_max_fps) {}

	// Wait until all rendering associated to the swap chain is finished before deleting it.
	virtual void swap_chain_free(SwapChainID p_swap_chain) = 0;

	/*********************/
	/**** FRAMEBUFFER ****/
	/*********************/

	virtual FramebufferID framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) = 0;
	virtual void framebuffer_free(FramebufferID p_framebuffer) = 0;

	/****************/
	/**** SHADER ****/
	/****************/

	struct ImmutableSampler {
		UniformType type = UNIFORM_TYPE_MAX;
		uint32_t binding = 0xffffffff; // Binding index as specified in shader.
		LocalVector<ID> ids;
	};

	// Creates a Pipeline State Object (PSO) out of the shader and all the input data it needs.
	// Immutable samplers can be embedded when creating the pipeline layout on the condition they remain valid and unchanged, so they don't need to be
	// specified when creating uniform sets PSO resource for binding.
	virtual ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) = 0;
	// Only meaningful if API_TRAIT_SHADER_CHANGE_INVALIDATION is SHADER_CHANGE_INVALIDATION_ALL_OR_NONE_ACCORDING_TO_LAYOUT_HASH.
	virtual uint32_t shader_get_layout_hash(ShaderID p_shader) { return 0; }
	// GodotNext: rd_rt_pipelines (#179). The ray payload of one stage of p_shader, in bytes, measured as
	// RDRTPipeline::get_spirv_payload_size() measures it (or RDRTPipeline::PAYLOAD_SIZE_UNKNOWN), so
	// RenderingDevice can check it against the payload size libraries declare. 0 without a payload, and
	// from drivers without pipeline libraries.
	virtual uint32_t shader_get_raytracing_payload_size(ShaderID p_shader, ShaderStage p_stage) const { return 0; }
	virtual void shader_free(ShaderID p_shader) = 0;
	virtual void shader_destroy_modules(ShaderID p_shader) = 0;

public:
	/*********************/
	/**** UNIFORM SET ****/
	/*********************/

	struct BoundUniform {
		UniformType type = UNIFORM_TYPE_MAX;
		uint32_t binding = 0xffffffff; // Binding index as specified in shader.
		LocalVector<ID> ids;
		// Flag to indicate  that this is an immutable sampler so it is skipped when creating uniform
		// sets, as it would be set previously when creating the pipeline layout.
		bool immutable_sampler = false;

		_FORCE_INLINE_ bool is_dynamic() const {
			return type == UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC || type == UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC;
		}
	};

	virtual UniformSetID uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) = 0;
	virtual void linear_uniform_set_pools_reset(int p_linear_pool_index) {}
	virtual void uniform_set_free(UniformSetID p_uniform_set) = 0;
	virtual bool uniform_sets_have_linear_pools() const { return false; }
	virtual uint32_t uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const = 0;

	// ----- COMMANDS -----

	virtual void command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) = 0;

	/************************/
	/**** BINDLESS TABLE ****/
	/************************/

	// GodotNext: rd_bindless (#176). A bindless table (plan §21.1 C4): a runtime array of textures,
	// samplers or read-only storage buffers. It is bound as the only binding of a uniform set, at
	// binding 0: uniform_set_create() gets one UNIFORM_TYPE_BINDLESS_TABLE uniform whose ID is the
	// table. bindless_table_write() writes one entry while the table may be bound in recorded or
	// submitted work; RenderingDevice never writes an entry that submitted work can still read. A driver
	// that reports SUPPORTS_BINDLESS overrides all four; the defaults are for the drivers that do not.
	virtual BindlessTableID bindless_table_create(UniformType p_type, uint32_t p_capacity) { return BindlessTableID(); }
	// p_resource is a TextureID, SamplerID or BufferID, as the table's type says.
	virtual void bindless_table_write(BindlessTableID p_table, uint32_t p_slot, ID p_resource) {}
	// The slot no longer holds a resource. Recorded and submitted work can still read its entry, so the entry
	// stays as written. A driver that keeps the table's resources resident keeps this one resident until the
	// commands recorded before the release are encoded (the next begin_segment()). A resource whose free
	// waited for the slot reaches the driver's free only later, once its frame comes around again.
	virtual void bindless_table_clear(BindlessTableID p_table, uint32_t p_slot) {}
	virtual void bindless_table_free(BindlessTableID p_table) {}
	// The GPU memory of the table's entries, in bytes.
	virtual uint64_t bindless_table_get_memory_usage(BindlessTableID p_table) const { return 0; }

	/******************/
	/**** TRANSFER ****/
	/******************/

	struct BufferCopyRegion {
		uint64_t src_offset = 0;
		uint64_t dst_offset = 0;
		uint64_t size = 0;
	};

	struct TextureCopyRegion {
		TextureSubresourceLayers src_subresources;
		Vector3i src_offset;
		TextureSubresourceLayers dst_subresources;
		Vector3i dst_offset;
		Vector3i size;
	};

	struct BufferTextureCopyRegion {
		uint64_t buffer_offset = 0;
		uint64_t row_pitch = 0;
		TextureSubresource texture_subresource;
		Vector3i texture_offset;
		Vector3i texture_region_size;
	};

	virtual void command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) = 0;
	virtual void command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) = 0;

	virtual void command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) = 0;
	virtual void command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) = 0;
	virtual void command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) = 0;
	virtual void command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) = 0;

	virtual void command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) = 0;
	virtual void command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) = 0;

	/******************/
	/**** PIPELINE ****/
	/******************/

	virtual void pipeline_free(PipelineID p_pipeline) = 0;

	// ----- BINDING -----

	virtual void command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) = 0;

	// ----- CACHE -----

	virtual bool pipeline_cache_create(const Vector<uint8_t> &p_data) = 0;
	virtual void pipeline_cache_free() = 0;
	virtual size_t pipeline_cache_query_size() = 0;
	virtual Vector<uint8_t> pipeline_cache_serialize() = 0;

	/*******************/
	/**** RENDERING ****/
	/*******************/

	// ----- SUBPASS -----

	enum AttachmentLoadOp {
		ATTACHMENT_LOAD_OP_LOAD = 0,
		ATTACHMENT_LOAD_OP_CLEAR = 1,
		ATTACHMENT_LOAD_OP_DONT_CARE = 2,
	};

	enum AttachmentStoreOp {
		ATTACHMENT_STORE_OP_STORE = 0,
		ATTACHMENT_STORE_OP_DONT_CARE = 1,
		// GodotNext: rd_readonly_depth (#301). No store access: preserves contents when no shader
		// writes the attachment (Vulkan VK_ATTACHMENT_STORE_OP_NONE).
		ATTACHMENT_STORE_OP_NONE = 2,
	};

	struct Attachment {
		DataFormat format = DATA_FORMAT_MAX;
		TextureSamples samples = TEXTURE_SAMPLES_MAX;
		AttachmentLoadOp load_op = ATTACHMENT_LOAD_OP_DONT_CARE;
		AttachmentStoreOp store_op = ATTACHMENT_STORE_OP_DONT_CARE;
		AttachmentLoadOp stencil_load_op = ATTACHMENT_LOAD_OP_DONT_CARE;
		AttachmentStoreOp stencil_store_op = ATTACHMENT_STORE_OP_DONT_CARE;
		TextureLayout initial_layout = TEXTURE_LAYOUT_UNDEFINED;
		TextureLayout final_layout = TEXTURE_LAYOUT_UNDEFINED;
	};

	struct AttachmentReference {
		static constexpr uint32_t UNUSED = 0xffffffff;
		uint32_t attachment = UNUSED;
		TextureLayout layout = TEXTURE_LAYOUT_UNDEFINED;
		BitField<TextureAspectBits> aspect = {};
	};

	struct Subpass {
		LocalVector<AttachmentReference> input_references;
		LocalVector<AttachmentReference> color_references;
		AttachmentReference depth_stencil_reference;
		AttachmentReference depth_resolve_reference;
		LocalVector<AttachmentReference> resolve_references;
		LocalVector<uint32_t> preserve_attachments;
		AttachmentReference fragment_shading_rate_reference;
		Size2i fragment_shading_rate_texel_size;
	};

	struct SubpassDependency {
		uint32_t src_subpass = 0xffffffff;
		uint32_t dst_subpass = 0xffffffff;
		BitField<PipelineStageBits> src_stages = {};
		BitField<PipelineStageBits> dst_stages = {};
		BitField<BarrierAccessBits> src_access = {};
		BitField<BarrierAccessBits> dst_access = {};
	};

	virtual RenderPassID render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) = 0;
	virtual void render_pass_free(RenderPassID p_render_pass) = 0;

	// ----- COMMANDS -----

	union RenderPassClearValue {
		Color color = {};
		struct {
			float depth;
			uint32_t stencil;
		};

		RenderPassClearValue() {}
	};

	struct AttachmentClear {
		BitField<TextureAspectBits> aspect = {};
		uint32_t color_attachment = 0xffffffff;
		RenderPassClearValue value;
	};

	// GodotNext: rd_clear_values (#275). p_clear_values holds one value per attachment of
	// p_render_pass, in its order (RenderingDeviceGraph::draw_list_driver_clear_values()).
	virtual void command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) = 0;
	virtual void command_end_render_pass(CommandBufferID p_cmd_buffer) = 0;
	virtual void command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) = 0;
	virtual void command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) = 0;
	virtual void command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) = 0;
	virtual void command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) = 0;

	// Binding.
	virtual void command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) = 0;
	virtual void command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) = 0;

	// Drawing.
	virtual void command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) = 0;
	virtual void command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) = 0;
	virtual void command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) = 0;
	virtual void command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) = 0;
	virtual void command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) = 0;
	virtual void command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) = 0;
	// GodotNext: rd_indirect_count (#177). Called for each indirect-count draw of a draw list, in order, before
	// the list's render pass (p_render_pass) begins, for drivers whose count draws need work that cannot run
	// inside a render pass (Metal clamps the records with a compute pass). The other parameters are the draw's.
	virtual void command_render_draw_indirect_count_prepare(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, bool p_indexed, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {}

	// GodotNext: rd_mesh_shaders (#178). Task and mesh shader draws, with a pipeline whose pre-raster stages are task
	// (optional) and mesh shaders bound. The indirect records are three uint32 group counts (x, y, z).
	virtual void command_render_draw_mesh_tasks(CommandBufferID p_cmd_buffer, uint32_t p_group_count_x, uint32_t p_group_count_y, uint32_t p_group_count_z) = 0;
	virtual void command_render_draw_mesh_tasks_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) = 0;
	virtual void command_render_draw_mesh_tasks_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) = 0;
	// Like command_render_draw_indirect_count_prepare(), for the indirect-count mesh task draws of a draw list. Both
	// hooks are called in the order the draws were recorded.
	virtual void command_render_draw_mesh_tasks_indirect_count_prepare(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {}

	// Buffer binding.
	virtual void command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) = 0;
	virtual void command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) = 0;

	// Dynamic state.
	virtual void command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) = 0;
	virtual void command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) = 0;

	// ----- PIPELINE -----

	virtual PipelineID render_pipeline_create(
			ShaderID p_shader,
			VertexFormatID p_vertex_format,
			RenderPrimitive p_render_primitive,
			PipelineRasterizationState p_rasterization_state,
			PipelineMultisampleState p_multisample_state,
			PipelineDepthStencilState p_depth_stencil_state,
			PipelineColorBlendState p_blend_state,
			VectorView<int32_t> p_color_attachments,
			BitField<PipelineDynamicStateFlags> p_dynamic_state,
			RenderPassID p_render_pass,
			uint32_t p_render_subpass,
			VectorView<PipelineSpecializationConstant> p_specialization_constants) = 0;

	/*****************/
	/**** COMPUTE ****/
	/*****************/

	// ----- COMMANDS -----

	virtual void command_begin_compute_pass(CommandBufferID p_cmd_buffer);
	virtual void command_end_compute_pass(CommandBufferID p_cmd_buffer);

	// Binding.
	virtual void command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) = 0;
	virtual void command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) = 0;

	// Dispatching.
	virtual void command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) = 0;
	virtual void command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) = 0;

	// ----- PIPELINE -----

	virtual PipelineID compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) = 0;

	/********************/
	/**** RAYTRACING ****/
	/********************/

	// ----- ACCELERATION STRUCTURE -----

	struct AccelerationStructureGeometry {
		BitField<AccelerationStructureGeometryFlagBits> flags = {};
		BufferID vertex_buffer;
		uint32_t vertex_offset = 0;
		uint32_t vertex_stride = 0;
		uint32_t vertex_count = 0;
		DataFormat vertex_format = DATA_FORMAT_MAX;
		BufferID index_buffer;
		uint32_t index_offset = 0;
		uint32_t index_count = 0;
		IndexBufferFormat index_format = {};
	};

	virtual AccelerationStructureID blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) = 0;
	// GodotNext: rq_substrate (#22). Whether blas_create() accepts p_format as a vertex format.
	virtual bool acceleration_structure_vertex_format_is_supported(DataFormat p_format) { return false; }

	struct AccelerationStructureInstance {
		Transform3D transform;
		uint32_t id = 0;
		uint8_t mask = 0;
		uint32_t hit_sbt_offset = 0;
		BitField<AccelerationStructureInstanceFlagBits> flags = {};
		AccelerationStructureID blas;
	};

	virtual AccelerationStructureID tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) = 0;
	virtual void acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) = 0;
	// GodotNext: tlas_from_buffer
	// Opaque reference a GPU-written instance record needs for this BLAS: the Vulkan device address,
	// Metal's resource ID or the D3D12 GPU virtual address (#118).
	virtual uint64_t acceleration_structure_get_instance_reference(AccelerationStructureID p_acceleration_structure) = 0;

	virtual void acceleration_structure_free(AccelerationStructureID p_acceleration_structure) = 0;
	// GodotNext: as_build_modes
	virtual uint32_t acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure, AccelerationStructureBuildMode p_mode) = 0;
	// GodotNext: metal_acceleration_structure
	// Bytes of the acceleration structure object itself. RenderingDevice adds them to
	// MEMORY_BUFFERS and MEMORY_ACCELERATION_STRUCTURES: Vulkan reports accelerationStructureSize
	// (#22), Metal the object's allocated size and D3D12 the prebuild or compacted size (#118).
	virtual uint64_t acceleration_structure_get_size_bytes(AccelerationStructureID p_acceleration_structure) { return 0; }

	// GodotNext: blas_compact
	// Drivers that cannot compact leave this false. RenderingDevice then returns
	// ERR_UNAVAILABLE and does not call the virtuals below.
	virtual bool acceleration_structure_compaction_supported() const { return false; }
	virtual QueryPoolID acceleration_structure_size_query_pool_create(uint32_t p_count) {
		ERR_FAIL_V_MSG(QueryPoolID(), "Acceleration structure compaction is not implemented by this driver.");
	}
	virtual void acceleration_structure_size_query_pool_free(QueryPoolID p_pool_id) {
		ERR_FAIL_MSG("Acceleration structure compaction is not implemented by this driver.");
	}
	virtual void acceleration_structure_size_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) {
		ERR_FAIL_MSG("Acceleration structure compaction is not implemented by this driver.");
	}
	virtual void command_acceleration_structure_write_compacted_size(CommandBufferID p_cmd_buffer, VectorView<AccelerationStructureID> p_acceleration_structures, QueryPoolID p_pool_id, uint32_t p_first) {
		ERR_FAIL_MSG("Acceleration structure compaction is not implemented by this driver.");
	}
	virtual AccelerationStructureID acceleration_structure_create_compacted(AccelerationStructureID p_src, uint64_t p_size) {
		ERR_FAIL_V_MSG(AccelerationStructureID(), "Acceleration structure compaction is not implemented by this driver.");
	}
	virtual void command_copy_acceleration_structure_compact(CommandBufferID p_cmd_buffer, AccelerationStructureID p_src, AccelerationStructureID p_dst) {
		ERR_FAIL_MSG("Acceleration structure compaction is not implemented by this driver.");
	}

	// ----- PIPELINE -----

	struct PipelineShader {
		ShaderID shader;
		VectorView<PipelineSpecializationConstant> specialization_constants;
		ShaderStage shader_stage = {};
	};

	struct HitGroup {
		uint32_t closest_hit_shader_index = UINT32_MAX;
		uint32_t any_hit_shader_index = UINT32_MAX;
		uint32_t intersection_shader_index = UINT32_MAX;
	};

	virtual RaytracingPipelineID raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader) = 0;
	virtual void raytracing_pipeline_free(RaytracingPipelineID p_pipeline) = 0;

	virtual bool raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID p_pipeline, uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes) = 0;

	// GodotNext: rd_rt_pipelines (#179). Pipeline libraries (plan §21.1 C5). A library is created like a
	// pipeline, but can be neither bound nor queried for handles; raytracing_pipeline_link() makes a
	// pipeline from libraries whose groups follow each other in p_libraries order. Every library and the
	// linked pipeline declare p_max_payload_size bytes of ray payload, the device's largest hit attribute
	// and the same recursion depth. Drivers without libraries keep these defaults: RenderingDevice checks
	// has_feature(SUPPORTS_RAYTRACING_PIPELINE_LIBRARY) and does not call them.
	virtual RaytracingPipelineID raytracing_pipeline_library_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, uint32_t p_max_payload_size, ShaderID p_layout_defining_shader) {
		ERR_FAIL_V_MSG(RaytracingPipelineID(), "Raytracing pipeline libraries are not implemented by this driver.");
	}
	virtual RaytracingPipelineID raytracing_pipeline_link(VectorView<RaytracingPipelineID> p_libraries, uint32_t p_max_trace_recursion_depth, uint32_t p_max_payload_size, ShaderID p_layout_defining_shader) {
		ERR_FAIL_V_MSG(RaytracingPipelineID(), "Raytracing pipeline libraries are not implemented by this driver.");
	}
	// GodotNext: d3d12_rt_pipelines (#121). The same link as raytracing_pipeline_link(), when its base is p_base, a
	// pipeline this driver linked from the first p_base_library_count entries of p_libraries: a driver that can grow an
	// existing pipeline (Direct3D 12's AddToStateObject) adds only the libraries after them. The default links all of
	// p_libraries again.
	virtual RaytracingPipelineID raytracing_pipeline_link_add(RaytracingPipelineID p_base, uint32_t p_base_library_count, VectorView<RaytracingPipelineID> p_libraries, uint32_t p_max_trace_recursion_depth, uint32_t p_max_payload_size, ShaderID p_layout_defining_shader) {
		return raytracing_pipeline_link(p_libraries, p_max_trace_recursion_depth, p_max_payload_size, p_layout_defining_shader);
	}
	// GodotNext: d3d12_rt_pipelines (#121). Rules of the driver's own that a pipeline or library of these shaders and hit
	// groups breaks, as one error that RenderingDevice fails with before it creates anything (Direct3D 12: the stages of
	// a hit group must declare the same ray payload and hit attributes). Empty when there are none, as by default.
	virtual String raytracing_pipeline_get_create_error(VectorView<PipelineShader> p_shaders, VectorView<HitGroup> p_hit_groups) { return String(); }
	// GodotNext: d3d12_rt_pipelines (#121). The bytes after each hit group's handle that the hit shaders of this pipeline
	// or library read whatever record_data the hit groups carry (Direct3D 12: the local root arguments of their shader
	// record). RenderingDevice makes every hit group's record at least this big, filled with zeros past its record_data.
	// 0 by default.
	virtual uint32_t raytracing_pipeline_get_hit_group_record_size(RaytracingPipelineID p_pipeline) { return 0; }

	// ----- COMMANDS -----

	// GodotNext: as_build_modes
	virtual void command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, uint64_t p_scratch_offset, AccelerationStructureBuildMode p_mode) = 0;
	virtual void command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, uint64_t p_scratch_offset, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count, AccelerationStructureBuildMode p_mode) = 0;
	virtual void command_bind_raytracing_pipeline(CommandBufferID p_cmd_buffer, RaytracingPipelineID p_pipeline) = 0;
	virtual void command_bind_raytracing_uniform_set(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) = 0;

	struct ShaderBindingTable {
		BufferID buffer;
		uint32_t offset = 0;
		uint32_t stride = 0;
		uint32_t size = 0;
	};

	virtual void command_trace_rays(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) = 0;
	// GodotNext: rd_rt_pipelines (#179). The launch size comes from three uint32 (width, height, depth) at
	// p_offset in p_indirect_buffer. Drivers without SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT keep this default.
	virtual void command_trace_rays_indirect(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, BufferID p_indirect_buffer, uint64_t p_offset) {
		ERR_FAIL_MSG("Indirect ray tracing is not implemented by this driver.");
	}

	/******************/
	/**** CALLBACK ****/
	/******************/

	typedef void (*DriverCallback)(RenderingDeviceDriver *p_driver, CommandBufferID p_command_buffer, void *p_userdata);

	/*****************/
	/**** QUERIES ****/
	/*****************/

	// ----- TIMESTAMP -----

	// Basic.
	virtual QueryPoolID timestamp_query_pool_create(uint32_t p_query_count) = 0;
	virtual void timestamp_query_pool_free(QueryPoolID p_pool_id) = 0;
	virtual void timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) = 0;
	virtual uint64_t timestamp_query_result_to_time(uint64_t p_result) = 0;

	// Commands.
	virtual void command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) = 0;
	virtual void command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) = 0;

	/****************/
	/**** LABELS ****/
	/****************/

	virtual void command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) = 0;
	virtual void command_end_label(CommandBufferID p_cmd_buffer) = 0;

	// Marks the start/end of a coherent batch of commands. Drivers may use this
	// to flush incidental encoder state (e.g. a still-open blit encoder used for
	// housekeeping copies) so subsequent commands start fresh. Default no-op.
	virtual void command_group_begin(CommandBufferID p_cmd_buffer) {}
	virtual void command_group_end(CommandBufferID p_cmd_buffer) {}
	// GodotNext: metal_level_fences (#186). p_cmd_buffer was just begun and continues p_from, which
	// is submitted before it on the same queue: the previous frame's last command buffer, or one the
	// graph ended in the middle of a frame. A driver that orders commands itself (Metal's level
	// fences) carries that order across. Default no-op.
	virtual void command_buffer_continue(CommandBufferID p_cmd_buffer, CommandBufferID p_from) {}

	/****************/
	/**** DEBUG *****/
	/****************/
	virtual void command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) = 0;

	/********************/
	/**** SUBMISSION ****/
	/********************/

	virtual void begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) = 0;
	virtual void end_segment() = 0;

	/**************/
	/**** MISC ****/
	/**************/

	enum ObjectType {
		OBJECT_TYPE_TEXTURE,
		OBJECT_TYPE_SAMPLER,
		OBJECT_TYPE_BUFFER,
		OBJECT_TYPE_SHADER,
		OBJECT_TYPE_UNIFORM_SET,
		OBJECT_TYPE_PIPELINE,
		OBJECT_TYPE_ACCELERATION_STRUCTURE,
		OBJECT_TYPE_RAYTRACING_PIPELINE,
	};

	struct MultiviewCapabilities {
		bool is_supported = false;
		bool geometry_shader_is_supported = false;
		bool tessellation_shader_is_supported = false;
		uint32_t max_view_count = 0;
		uint32_t max_instance_count = 0;
	};

	struct FragmentShadingRateCapabilities {
		Size2i min_texel_size;
		Size2i max_texel_size;
		Size2i max_fragment_size;
		bool pipeline_supported = false;
		bool primitive_supported = false;
		bool attachment_supported = false;
	};

	struct FragmentDensityMapCapabilities {
		Size2i min_texel_size;
		Size2i max_texel_size;
		Size2i offset_granularity;
		bool attachment_supported = false;
		bool dynamic_attachment_supported = false;
		bool non_subsampled_images_supported = false;
		bool invocations_supported = false;
		bool offset_supported = false;
	};

	enum ApiTrait {
		API_TRAIT_HONORS_PIPELINE_BARRIERS,
		API_TRAIT_SHADER_CHANGE_INVALIDATION,
		API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT,
		API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP,
		API_TRAIT_SECONDARY_VIEWPORT_SCISSOR,
		API_TRAIT_BUFFER_CLEARS_WITH_COPY_ENGINE,
		API_TRAIT_TEXTURE_CLEARS_WITH_COPY_ENGINE,
		API_TRAIT_USE_GENERAL_IN_COPY_QUEUES,
		API_TRAIT_BUFFERS_REQUIRE_TRANSITIONS,
		API_TRAIT_TEXTURES_REQUIRE_LAYOUT_TRANSITIONS,
		API_TRAIT_TEXTURE_OUTPUTS_REQUIRE_CLEARS,
		API_TRAIT_ACCELERATION_STRUCTURE_INSTANCE_SIZE,
		API_TRAIT_SHADER_GROUP_HANDLE_SIZE,
		API_TRAIT_SHADER_GROUP_BASE_ALIGNMENT,
		API_TRAIT_SHADER_GROUP_HANDLE_ALIGNMENT,
		// GodotNext: rd_readonly_depth (#301). True when ATTACHMENT_STORE_OP_NONE executes without a
		// depth/stencil store write (Vulkan VK_KHR/EXT_load_store_op_none, D3D12, Metal).
		API_TRAIT_SUPPORTS_ATTACHMENT_STORE_OP_NONE,
	};

	enum ShaderChangeInvalidation {
		SHADER_CHANGE_INVALIDATION_ALL_BOUND_UNIFORM_SETS,
		// What Vulkan does.
		SHADER_CHANGE_INVALIDATION_INCOMPATIBLE_SETS_PLUS_CASCADE,
		// What D3D12 does.
		SHADER_CHANGE_INVALIDATION_ALL_OR_NONE_ACCORDING_TO_LAYOUT_HASH,
	};

	enum DeviceFamily {
		DEVICE_UNKNOWN,
		DEVICE_OPENGL,
		DEVICE_VULKAN,
		DEVICE_DIRECTX,
		DEVICE_METAL,
	};

	struct Capabilities {
		DeviceFamily device_family = DEVICE_UNKNOWN;
		uint32_t version_major = 1;
		uint32_t version_minor = 0;
		// GodotNext: path_tracer (#167). The Apple GPU family on Metal (7 for Apple7, 8 for Apple8), 0 elsewhere.
		uint32_t apple_gpu_family = 0;
	};

	virtual void set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) = 0;
	virtual uint64_t get_resource_native_handle(DriverResource p_type, ID p_driver_id) = 0;
	// GodotNext: pu_core_rd_native_interop (#102). Vendor interop metadata. The base defaults return
	// no handle and ERR_UNAVAILABLE; the Vulkan driver fills them for NGX image-view resources.
	struct TextureNativeInfo {
		uint64_t image = 0;
		uint64_t view = 0;
		uint64_t format = 0;
		BitField<TextureAspectBits> aspect = {};
		uint32_t base_mipmap = 0;
		uint32_t mipmap_count = 0;
		uint32_t base_layer = 0;
		uint32_t layer_count = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};
	virtual uint64_t command_buffer_get_native_handle(CommandBufferID p_cmd_buffer);
	virtual Error texture_get_native_info(TextureID p_texture, TextureNativeInfo &r_info);
	virtual uint64_t get_total_memory_used() = 0;
	virtual uint64_t get_lazily_memory_used() = 0;
	virtual uint64_t limit_get(Limit p_limit) = 0;
	virtual uint64_t api_trait_get(ApiTrait p_trait);
	// GodotNext: metal_ray_query (#48, #171)
	// Pure Metal ray-query capability check. Always compiled, so the doctests run without a GPU.
	// msl_version uses major * 10000 + minor * 100, the same encoding as the Metal driver.
	// Below macOS 14, indirect-instance AS descriptors and MSL 3.1 ulong texture atomics are
	// unavailable, so reason is RAY_QUERY_REASON_METAL_OS_TOO_OLD.
	// On Apple8 (M2) with macOS >= 14, ray_query is true and hardware is false (software-traversed
	// BVH; NextGI reports NOT_HW_ACCELERATED unless opted in). Apple9+ sets hardware = true.
	struct MetalRayQueryCaps {
		bool ray_query = false;
		bool hardware = false;
		RayQueryUnsupportedReason reason = RAY_QUERY_REASON_NO_RAY_QUERY;
		const char *hardware_note = "";
	};

	static constexpr uint32_t METAL_RAY_QUERY_MIN_OS_MAJOR = 14;
	static constexpr uint32_t METAL_RAY_QUERY_MIN_MSL = 20400; // MSL 2.4.
	static constexpr uint32_t METAL_RAY_QUERY_APPLE8_FAMILY = 1008; // Apple8 (M2).
	static constexpr uint32_t METAL_RAY_QUERY_HARDWARE_FAMILY = 1009; // Apple9.
	static constexpr uint32_t METAL_ACCELERATION_STRUCTURE_INSTANCE_SIZE = 72;
	// GodotNext: metal_acceleration_structure
	// Cap accepted by tlas_create. ExtendedLimits is never set. 2^24 is above the
	// plan's 100,000 floor and is what limit_get reports on a ray-query device.
	static constexpr uint32_t METAL_MAX_ACCELERATION_STRUCTURE_INSTANCES = 1u << 24;

	static MetalRayQueryCaps metal_ray_query_caps(uint32_t p_apple_family, bool p_is_macos, uint32_t p_os_major, bool p_supports_raytracing, uint32_t p_msl_version, bool p_gpu_address, bool p_barriers, bool p_argument_buffers);

	// GodotNext: metal_apple8 (#171), rd_atomics64 (#175)
	// Pure Metal 64-bit integer atomic capability check. Apple9+ and Apple8 on Mac2 support
	// uint64 max/min on buffers with MSL 3.1+ (macOS 14+ on macOS), and on 2D R64_UINT images
	// when native image atomics are available. Metal has no 64-bit atomic arithmetic on any family.
	struct MetalInt64AtomicCaps {
		bool buffer_min_max = false;
		bool image_min_max = false;
		bool arith = false;
	};
	static constexpr uint32_t METAL_INT64_ATOMICS_MIN_MSL = 30100; // MSL 3.1.
	static MetalInt64AtomicCaps metal_int64_atomic_caps(uint32_t p_apple_family, bool p_is_mac2, bool p_is_macos, uint32_t p_os_major, uint32_t p_msl_version, bool p_native_image_atomics);

	// GodotNext: rq_substrate (#22), rt_core_check (#283)
	// Pure Vulkan ray-query capability check, always compiled so the doctests run without a GPU.
	// Ray query needs acceleration structures, the rayQuery feature and buffer device addresses;
	// hardware also needs the RT-core check to pass for the device's PCI IDs and type
	// (RenderingContextDriver::check_ray_query_hardware_device; p_integrated is
	// VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU). hardware_note says why, for the capability log.
	struct VulkanRayQueryCaps {
		bool ray_query = false;
		bool hardware = false;
		RayQueryUnsupportedReason reason = RAY_QUERY_REASON_NO_RAY_QUERY;
		const char *hardware_note = "";
	};
	static VulkanRayQueryCaps vulkan_ray_query_caps(bool p_acceleration_structure, bool p_ray_query, bool p_buffer_device_address, uint32_t p_vendor_id, uint32_t p_device_id, bool p_integrated);

	// GodotNext: d3d12_dxr (#118)
	// Pure D3D12 ray-query capability check, always compiled so the doctests run without a GPU.
	// Ray query (DXR 1.1 inline ray tracing) needs D3D12_OPTIONS5 RaytracingTier 1.1 or later and
	// shader model 6.5 (RayQuery in DXIL). RenderingDevice's acceleration-structure path also asks for
	// buffer device addresses (scratch ring, instance buffers, build inputs), which the D3D12 driver
	// gives from shader model 6.6, the same tightening as on Vulkan. The Mesa NIR library must compile
	// ray query (#123). Any failing check gives D3D12_NO_DXR, and `missing` names the first one.
	// Hardware also needs the RT-core check to pass for the adapter's PCI IDs (#283; p_integrated is
	// D3D12_FEATURE_DATA_ARCHITECTURE1::UMA). The tier rule alone turns WARP (a software adapter) into
	// CPU_DEVICE.
	struct D3D12RayQueryCaps {
		bool ray_query = false;
		bool hardware = false;
		RayQueryUnsupportedReason reason = RAY_QUERY_REASON_D3D12_NO_DXR;
		const char *missing = "";
		const char *hardware_note = "";
	};
	static constexpr uint32_t D3D12_RAY_QUERY_MIN_RAYTRACING_TIER = 11; // D3D12_RAYTRACING_TIER_1_1.
	static constexpr uint32_t D3D12_RAY_QUERY_MIN_SHADER_MODEL = 0x65; // D3D_SHADER_MODEL_6_5.
	static constexpr uint32_t D3D12_BUFFER_DEVICE_ADDRESS_MIN_SHADER_MODEL = 0x66; // D3D_SHADER_MODEL_6_6.
	static constexpr uint32_t D3D12_ACCELERATION_STRUCTURE_INSTANCE_SIZE = 64; // D3D12_RAYTRACING_INSTANCE_DESC.
	static D3D12RayQueryCaps d3d12_ray_query_caps(uint32_t p_raytracing_tier, uint32_t p_shader_model, bool p_ray_query_toolchain, uint32_t p_vendor_id, uint32_t p_device_id, bool p_integrated);
	// GodotNext: d3d12_rt_pipelines (#121). Pure D3D12 RT pipeline capability check. RT pipelines need what ray query
	// needs (p_ray_query: raytracing tier 1.1, the shader models and the Mesa NIR library, which compiles the
	// ray-tracing stages too), the same floor as the plan's DXR 1.1 path. Tier 1.1 also brings ExecuteIndirect's
	// DispatchRays (the indirect trace) and AddToStateObject; pipeline libraries (collections, linked with
	// AddToStateObject) also need ID3D12Device7 (p_device_7).
	struct D3D12RaytracingPipelineCaps {
		bool pipeline = false;
		bool library = false;
		bool trace_rays_indirect = false;
	};
	static D3D12RaytracingPipelineCaps d3d12_raytracing_pipeline_caps(bool p_ray_query, bool p_device_7);
	// The RD vertex formats a D3D12 triangle geometry accepts: DXR's tier 1.0 and 1.1 lists, as RD formats
	// with the same memory layout. BGR-ordered RD formats the D3D12 driver maps onto a listed DXGI format
	// (B8G8R8A8_SNORM, or A2R10G10B10_UNORM_PACK32 through an SRV swizzle a build never applies) are not
	// included: a build would read their components in the wrong order.
	static bool d3d12_acceleration_structure_vertex_format_supported(DataFormat p_format);

	// GodotNext: rd_mesh_shaders (#178)
	// Pure mesh-shader capability rules (SUPPORTS_MESH_SHADER and SUPPORTS_MESH_SHADER_INDIRECT), always compiled so
	// the doctests run without a GPU. indirect is never true without mesh.
	struct MeshShaderCaps {
		bool mesh = false;
		bool indirect = false;
	};
	// Metal: object and mesh functions from Apple7 with MSL 3.0 (macOS 13, iOS 16) and argument buffers, which the
	// driver's mesh pipelines bind; indirect mesh draws need Apple9 (plan §21.1.5). p_msl_version uses
	// major * 10000 + minor * 100, like metal_ray_query_caps(). The driver passes the device's values, the shader
	// baker an export profile's.
	static constexpr uint32_t METAL_MESH_SHADER_FAMILY = 1007; // Apple7.
	static constexpr uint32_t METAL_MESH_SHADER_INDIRECT_FAMILY = 1009; // Apple9.
	static constexpr uint32_t METAL_MESH_SHADER_MIN_MSL = 30000; // MSL 3.0.
	static MeshShaderCaps metal_mesh_shader_caps(uint32_t p_apple_family, uint32_t p_msl_version, bool p_argument_buffers);
	// D3D12: p_mesh_shader_tier is D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier (0, or 10 for tier 1), and
	// p_dxil_mesh_stages whether the shader container translates task and mesh stages to DXIL, which it does not
	// yet. r_log_line is the one line the driver prints about it when it checks the device.
	static constexpr uint32_t D3D12_MESH_SHADER_TIER_1 = 10;
	static MeshShaderCaps d3d12_mesh_shader_caps(uint32_t p_mesh_shader_tier, bool p_dxil_mesh_stages, String &r_log_line);

	// GodotNext: rd_mesh_shaders (#178), nextgeo_mesh_raster (#154)
	// The mesh shader limits a driver reports through limit_get(): task and mesh workgroup counts,
	// local workgroup invocation/axis limits, mesh output vertex/primitive caps, and task payload
	// byte size. Every field is 0 on a device without SUPPORTS_MESH_SHADER.
	struct MeshShaderLimits {
		uint64_t max_task_workgroup_count[3] = {};
		uint64_t max_task_workgroup_total_count = 0;
		uint64_t max_mesh_workgroup_count[3] = {};
		uint64_t max_mesh_workgroup_total_count = 0;
		// GodotNext: nextgeo_mesh_raster (#154, plan §21.1 C2, §21.4 N9).
		uint64_t max_task_workgroup_invocations = 0;
		uint64_t max_task_workgroup_size[3] = {};
		uint64_t max_mesh_workgroup_invocations = 0;
		uint64_t max_mesh_workgroup_size[3] = {};
		uint64_t max_mesh_output_vertices = 0;
		uint64_t max_mesh_output_primitives = 0;
		uint64_t max_task_payload_size = 0;
	};
	// Writes r_value and returns true when p_limit is one of the mesh shader limits.
	static bool mesh_shader_limit_get(Limit p_limit, const MeshShaderLimits &p_limits, uint64_t &r_value);
	// Metal Feature Set Tables and MSL 3.x specification: no limit on the threadgroups of an object shader grid
	// (reported as UINT32_MAX, the most a draw can ask for, as the driver reports its compute counts); threadgroups
	// per mesh shader grid 1024 on Apple7 and Apple8, 1,048,575 on Apple9 and 4,194,303 from Apple10; up to 1024
	// threads per object/mesh threadgroup; up to 256 vertices and 512 primitives per mesh; 16,384-byte task payload.
	static MeshShaderLimits metal_mesh_shader_limits(uint32_t p_apple_family, bool p_mesh_shader);
	// The Direct3D 12 mesh shader specification: each DispatchMesh() group count below 64k, their product at most
	// 2^22, at most 128 threads per amplification/mesh threadgroup, at most 256 output vertices and 256 output
	// primitives, and a 16,384-byte payload.
	static MeshShaderLimits d3d12_mesh_shader_limits(bool p_mesh_shader);

	// The acceleration-structure limits a driver reports through limit_get(). Every field is 0 on
	// a device without ray query and wherever the API does not report the limit.
	struct AccelerationStructureLimits {
		uint64_t max_instances = 0;
		uint64_t max_geometries = 0;
		uint64_t max_primitives = 0;
		uint64_t max_per_stage = 0;
		uint64_t min_scratch_offset_alignment = 0;
	};
	// Writes r_value and returns true when p_limit is one of the five AS limits.
	static bool acceleration_structure_limit_get(Limit p_limit, const AccelerationStructureLimits &p_limits, uint64_t &r_value);

	// NO_RAY_QUERY until a driver overrides it. Metal, Vulkan and D3D12 do.
	virtual RayQueryUnsupportedReason get_ray_query_unsupported_reason() const { return RAY_QUERY_REASON_NO_RAY_QUERY; }

	virtual bool has_feature(Features p_feature) = 0;
	virtual const MultiviewCapabilities &get_multiview_capabilities() = 0;
	virtual const FragmentShadingRateCapabilities &get_fragment_shading_rate_capabilities() = 0;
	virtual const FragmentDensityMapCapabilities &get_fragment_density_map_capabilities() = 0;
	virtual String get_api_name() const = 0;
	virtual String get_api_version() const = 0;
	// GodotNext: hwrt_safety_net
	// Default key is vendor, device name and API version. Vulkan overrides with vendorID:deviceID:driverVersion.
	static String format_driver_version_key(const String &p_vendor, const String &p_device_name, const String &p_api_version);
	static String format_vulkan_driver_version_key(uint32_t p_vendor_id, uint32_t p_device_id, uint32_t p_driver_version);
	// GodotNext: d3d12_dxr (#118). PCI IDs and the user-mode driver version DXGI reports
	// (IDXGIAdapter::CheckInterfaceSupport), as "D3D12:<vendor>:<device>:<a>.<b>.<c>.<d>".
	static String format_d3d12_driver_version_key(uint32_t p_vendor_id, uint32_t p_device_id, uint64_t p_driver_version);
	// GodotNext: linux_driver_minimums (#285)
	// UE 5.9's Linux Vulkan driver minimums (plan §26.1): NVIDIA's proprietary driver 570 and RADV 24.2.8.
	// Below them the Vulkan driver logs one warning on Linux and changes nothing else: no tier, no reason
	// (lead decision 2026-11-16). Pure, so the doctests run anywhere. p_driver_id and p_driver_info are
	// VkPhysicalDeviceDriverProperties::driverID and driverInfo: "580.178.04" on NVIDIA, "Mesa 24.2.8" (with
	// an optional suffix such as "-1ubuntu1" or "-devel") on RADV. Other drivers, and a version that does
	// not parse, give no warning.
	static constexpr uint32_t VULKAN_DRIVER_ID_MESA_RADV = 3; // VK_DRIVER_ID_MESA_RADV.
	static constexpr uint32_t VULKAN_DRIVER_ID_NVIDIA_PROPRIETARY = 4; // VK_DRIVER_ID_NVIDIA_PROPRIETARY.
	static constexpr uint32_t VULKAN_LINUX_NVIDIA_MIN_MAJOR = 570;
	static constexpr uint32_t VULKAN_LINUX_RADV_MIN_MAJOR = 24;
	static constexpr uint32_t VULKAN_LINUX_RADV_MIN_MINOR = 2;
	static constexpr uint32_t VULKAN_LINUX_RADV_MIN_PATCH = 8;
	struct VulkanDriverMinimumCheck {
		bool checked = false; // NVIDIA's proprietary driver or RADV, and its version parsed.
		bool below_minimum = false;
		String driver; // "NVIDIA" or "RADV"; empty for other drivers.
		String version; // As parsed: "580.178.04", "24.2.8".
		String minimum; // "570" or "24.2.8".
		String warning; // Empty unless below_minimum.
	};
	static VulkanDriverMinimumCheck vulkan_linux_driver_minimum_check(uint32_t p_driver_id, const String &p_driver_info);
	void set_driver_version_identity(const String &p_vendor, const String &p_device_name);
	virtual String get_driver_version_key() const;
	typedef void (*DeviceLostCallback)(void *);
	void set_device_lost_callback(DeviceLostCallback p_callback, void *p_userdata);
	void notify_device_lost();
	virtual String get_pipeline_cache_uuid() const = 0;
	virtual const Capabilities &get_capabilities() const = 0;
	virtual const RenderingShaderContainerFormat &get_shader_container_format() const = 0;

	virtual bool is_composite_alpha_supported(CommandQueueID p_queue) const { return false; }

	virtual DriverWorkarounds get_driver_workarounds() const { return DriverWorkarounds(); }

	/******************/

private:
	// GodotNext: hwrt_safety_net
	DeviceLostCallback device_lost_callback = nullptr;
	void *device_lost_userdata = nullptr;
	String driver_version_vendor;
	String driver_version_device;

public:
	virtual ~RenderingDeviceDriver();
};

using RDD = RenderingDeviceDriver;
