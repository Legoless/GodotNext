/**************************************************************************/
/*  rendering_device_driver_vulkan.h                                      */
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

#include "core/templates/hash_map.h"
#include "core/templates/paged_allocator.h"
#include "core/templates/rb_map.h"
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#include "drivers/vulkan/vulkan_extension_provider.h" // GodotNext: pu_vk_extension_providers (#105).
#include "servers/rendering/rendering_device_driver.h"

#ifdef DEBUG_ENABLED
#ifndef _MSC_VER
#define _DEBUG
#endif
#endif
#include <thirdparty/re-spirv/re-spirv.h>
#include <thirdparty/vulkan/vk_mem_alloc.h>

#include <drivers/vulkan/godot_vulkan.h>

class FileAccess;

// Design principles:
// - Vulkan structs are zero-initialized and fields not requiring a non-zero value are omitted (except in cases where expresivity reasons apply).
class RenderingDeviceDriverVulkan : public RenderingDeviceDriver {
	/*****************/
	/**** GENERIC ****/
	/*****************/

	struct CommandQueue;
	struct SwapChain;
	struct CommandBufferInfo;
	struct RenderPassInfo;
	struct Framebuffer;

	struct Queue {
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t virtual_count = 0;
		BinaryMutex submit_mutex;
	};

	struct SubgroupCapabilities {
		uint32_t size = 0;
		uint32_t min_size = 0;
		uint32_t max_size = 0;
		VkShaderStageFlags supported_stages = 0;
		VkSubgroupFeatureFlags supported_operations = 0;
		VkBool32 quad_operations_in_all_stages = false;
		bool size_control_is_supported = false;

		uint32_t supported_stages_flags_rd() const;
		String supported_stages_desc() const;
		uint32_t supported_operations_flags_rd() const;
		String supported_operations_desc() const;
	};

	struct ShaderCapabilities {
		bool shader_float16_is_supported = false;
		bool shader_int8_is_supported = false;
	};

	struct StorageBufferCapabilities {
		bool storage_buffer_16_bit_access_is_supported = false;
		bool uniform_and_storage_buffer_16_bit_access_is_supported = false;
		bool storage_push_constant_16_is_supported = false;
		bool storage_input_output_16 = false;
	};

	struct AccelerationStructureCapabilities {
		bool acceleration_structure_support = false;
		uint32_t min_acceleration_structure_scratch_offset_alignment = 0;
	};

	// GodotNext: rd_atomics64 (#175)
	struct Int64AtomicsCapabilities {
		bool buffer_supported = false; // shaderBufferInt64Atomics.
		bool shared_supported = false; // shaderSharedInt64Atomics: part of SUPPORTS_INT64_ATOMIC_ARITH.
		bool image_supported = false; // shaderImageInt64Atomics.
		bool r64_storage_atomic_supported = false; // VK_FORMAT_R64_UINT has STORAGE_IMAGE_ATOMIC with optimal tiling.
	};

	// GodotNext: rd_bindless (#176). Descriptor indexing (VK_EXT_descriptor_indexing, core in Vulkan 1.2),
	// as bindless tables use it.
	struct BindlessCapabilities {
		bool runtime_descriptor_array = false;
		bool sampled_image_non_uniform_indexing = false; // Covers samplers too.
		bool storage_buffer_non_uniform_indexing = false;
		bool partially_bound = false;
		bool update_unused_while_pending = false;
		bool sampled_image_update_after_bind = false; // Covers samplers too.
		bool storage_buffer_update_after_bind = false;
		bool variable_descriptor_count = false;
		// The capacity limits: half of the update-after-bind descriptor limits, so a shader can bind two
		// full tables of a type beside its ordinary uniform sets, capped at MAX_SLOTS (MAX_SAMPLER_SLOTS).
		uint32_t max_textures = 0;
		uint32_t max_samplers = 0;
		uint32_t max_storage_buffers = 0;
		// The descriptor sizes VK_EXT_descriptor_buffer reports, for the memory a table reports; 0 when the
		// device does not expose the extension.
		uint32_t sampled_image_descriptor_size = 0;
		uint32_t sampler_descriptor_size = 0;
		uint32_t storage_buffer_descriptor_size = 0;

		static constexpr uint32_t MAX_SLOTS = 1 << 18;
		static constexpr uint32_t MAX_SAMPLER_SLOTS = 4096;
		// The size a table reports per entry when the device does not give its descriptor sizes: an upper
		// bound of the descriptor sizes desktop drivers report.
		static constexpr uint32_t FALLBACK_DESCRIPTOR_SIZE = 64;

		bool features_supported() const {
			return runtime_descriptor_array && sampled_image_non_uniform_indexing && storage_buffer_non_uniform_indexing && partially_bound && update_unused_while_pending && sampled_image_update_after_bind && storage_buffer_update_after_bind && variable_descriptor_count;
		}
	};

	struct RaytracingCapabilities {
		bool raytracing_pipeline_support = false;
		uint32_t shader_group_handle_size = 0;
		uint32_t shader_group_handle_alignment = 0;
		uint32_t shader_group_handle_size_aligned = 0;
		uint32_t shader_group_base_alignment = 0;
		bool validation = false;
		// GodotNext: rd_rt_pipelines (#179)
		bool library_support = false; // VK_KHR_pipeline_library.
		bool trace_rays_indirect_support = false; // rayTracingPipelineTraceRaysIndirect.
		uint32_t max_ray_hit_attribute_size = 0;
		bool reorder_ext = false; // VK_EXT_ray_tracing_invocation_reorder's rayTracingInvocationReorder.
		bool reorder_nv = false; // VK_NV_ray_tracing_invocation_reorder's rayTracingInvocationReorder.
	};

	struct DeviceFunctions {
		PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
		PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
		PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
		PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
		PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
		PFN_vkCreateRenderPass2KHR CreateRenderPass2KHR = nullptr;
		PFN_vkCmdEndRenderPass2KHR EndRenderPass2KHR = nullptr;

		// Debug marker extensions.
		PFN_vkCmdDebugMarkerBeginEXT CmdDebugMarkerBeginEXT = nullptr;
		PFN_vkCmdDebugMarkerEndEXT CmdDebugMarkerEndEXT = nullptr;
		PFN_vkCmdDebugMarkerInsertEXT CmdDebugMarkerInsertEXT = nullptr;
		PFN_vkDebugMarkerSetObjectNameEXT DebugMarkerSetObjectNameEXT = nullptr;

		// Debug device fault.
		PFN_vkGetDeviceFaultInfoEXT GetDeviceFaultInfoEXT = nullptr;

		// Raytracing extensions.
		PFN_vkCreateAccelerationStructureKHR CreateAccelerationStructureKHR = nullptr;
		PFN_vkDestroyAccelerationStructureKHR DestroyAccelerationStructureKHR = nullptr;
		PFN_vkGetAccelerationStructureBuildSizesKHR GetAccelerationStructureBuildSizesKHR = nullptr;
		PFN_vkCmdBuildAccelerationStructuresKHR CmdBuildAccelerationStructuresKHR = nullptr;
		// GodotNext: rq_substrate (#22)
		PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddressKHR = nullptr;
		PFN_vkCmdWriteAccelerationStructuresPropertiesKHR CmdWriteAccelerationStructuresPropertiesKHR = nullptr;
		PFN_vkCmdCopyAccelerationStructureKHR CmdCopyAccelerationStructureKHR = nullptr;
		PFN_vkCreateRayTracingPipelinesKHR CreateRaytracingPipelinesKHR = nullptr;
		PFN_vkGetRayTracingShaderGroupHandlesKHR GetRayTracingShaderGroupHandlesKHR = nullptr;
		PFN_vkCmdTraceRaysKHR CmdTraceRaysKHR = nullptr;
		// GodotNext: rd_rt_pipelines (#179)
		PFN_vkCmdTraceRaysIndirectKHR CmdTraceRaysIndirectKHR = nullptr;

		// GodotNext: rd_indirect_count (#177). VK_KHR_draw_indirect_count.
		PFN_vkCmdDrawIndirectCountKHR CmdDrawIndirectCountKHR = nullptr;
		PFN_vkCmdDrawIndexedIndirectCountKHR CmdDrawIndexedIndirectCountKHR = nullptr;

		// GodotNext: rd_mesh_shaders (#178). VK_EXT_mesh_shader.
		PFN_vkCmdDrawMeshTasksEXT CmdDrawMeshTasksEXT = nullptr;
		PFN_vkCmdDrawMeshTasksIndirectEXT CmdDrawMeshTasksIndirectEXT = nullptr;
		PFN_vkCmdDrawMeshTasksIndirectCountEXT CmdDrawMeshTasksIndirectCountEXT = nullptr;
	};
	// Debug marker extensions.
	VkDebugReportObjectTypeEXT _convert_to_debug_report_objectType(VkObjectType p_object_type);

	VkDevice vk_device = VK_NULL_HANDLE;
	RenderingContextDriverVulkan *context_driver = nullptr;
	RenderingContextDriver::Device context_device = {};
	uint32_t frame_count = 1;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties physical_device_properties = {};
	VkPhysicalDeviceFeatures physical_device_features = {};
	VkPhysicalDeviceFeatures requested_device_features = {};
	HashMap<CharString, bool> requested_device_extensions;
	HashSet<CharString> enabled_device_extension_names;
	TightLocalVector<TightLocalVector<Queue>> queue_families;
	TightLocalVector<VkQueueFamilyProperties> queue_family_properties;
	// GodotNext: async_compute (#174). Empty until RenderingDevice creates the async compute queue
	// (command_queue_set_async_compute()). From then on, buffers and images are created
	// VK_SHARING_MODE_CONCURRENT across every family listed here: each family that has a queue.
	bool async_compute_queue_created = false;
	TightLocalVector<uint32_t> resource_sharing_queue_families;
	void _update_resource_sharing_queue_families();
	RDD::Capabilities device_capabilities;
	SubgroupCapabilities subgroup_capabilities;
	MultiviewCapabilities multiview_capabilities;
	FragmentShadingRateCapabilities fsr_capabilities;
	FragmentDensityMapCapabilities fdm_capabilities;
	ShaderCapabilities shader_capabilities;
	StorageBufferCapabilities storage_buffer_capabilities;
	RenderingShaderContainerFormatVulkan shader_container_format;
	bool buffer_device_address_support = false;
	bool vulkan_memory_model_support = false;
	bool vulkan_memory_model_device_scope_support = false;
	AccelerationStructureCapabilities acceleration_structure_capabilities;
	bool ray_query_support = false;
	// GodotNext: rq_substrate (#22). Set once the device's features and properties are known.
	VulkanRayQueryCaps ray_query_caps;
	AccelerationStructureLimits acceleration_structure_limits;
	RaytracingCapabilities raytracing_capabilities;
	Int64AtomicsCapabilities int64_atomics_capabilities; // GodotNext: rd_atomics64 (#175).
	BindlessCapabilities bindless_capabilities; // GodotNext: rd_bindless (#176).
	// GodotNext: rd_bindless (#176). Found, not enabled: the table memory line reads its descriptor sizes.
	bool descriptor_buffer_available = false;
	// GodotNext: rd_indirect_count (#177). shaderDrawParameters (gl_DrawID, gl_BaseInstance, gl_BaseVertex).
	bool shader_draw_parameters_support = false;
	// GodotNext: rd_mesh_shaders (#178). The taskShader and meshShader features of VK_EXT_mesh_shader; device
	// creation enables both, and nothing else of the extension, when the device has both.
	bool mesh_shader_support = false;
	MeshShaderLimits mesh_shader_limits; // GodotNext: rd_mesh_shaders (#178). From VkPhysicalDeviceMeshShaderPropertiesEXT.
	bool pipeline_cache_control_support = false;
	bool device_fault_support = false;
	bool framebuffer_depth_resolve = false;
#if defined(VK_TRACK_DEVICE_MEMORY)
	bool device_memory_report_support = false;
#endif
	bool device_local_host_visible_host_coherent_memory_support = false;
#if defined(SWAPPY_FRAME_PACING_ENABLED)
	// Swappy frame pacer for Android.
	bool swappy_frame_pacer_enable = false;
	uint8_t swappy_mode = 2; // See default value for display/window/frame_pacing/android/swappy_mode.
#endif
	DeviceFunctions device_functions;

	// GodotNext: pu_vk_extension_providers (#105). The device extensions each registered provider
	// asked for, and whether the providers were told about this device.
	VulkanExtensionProviders::DeviceRequests extension_provider_requests;
	bool extension_providers_notified = false;

	// GodotNext: pu_vk_extension_providers (#105). The created-handle registry: every Vulkan handle
	// this driver creates is in it from when its create call returns until its destroy call has
	// returned, so the validation messages raised inside those calls are attributed. It also holds
	// the swap chains' images and the device memory VMA allocates. One gap: VMA reports a memory
	// block it frees just before its vkFreeMemory call, so a message raised inside that call that
	// names only the block is not attributed. Debug builds only (never in template_release), and
	// filled only with validation layers on. Not in it: the device, its queues and command buffers,
	// which the providers' own Vulkan calls share, and the image of a texture made by
	// texture_create_from_extension().
#ifdef DEBUG_ENABLED
	VulkanCreatedHandles created_handles;
	bool created_handles_tracked = false;
	bool created_handles_check = false;
	SafeNumeric<uint32_t> created_handles_checked_images;
	SafeNumeric<uint32_t> created_handles_checked_views;
	SafeNumeric<uint32_t> created_handles_check_missing;

	template <typename T>
	_FORCE_INLINE_ void _track_handle(T p_handle) {
		if (created_handles_tracked) {
			created_handles.add((uint64_t)p_handle);
		}
	}
	template <typename T>
	_FORCE_INLINE_ void _untrack_handle(T p_handle) {
		if (created_handles_tracked) {
			created_handles.remove((uint64_t)p_handle);
		}
	}
#else
	template <typename T>
	_FORCE_INLINE_ void _track_handle(T p_handle) {}
	template <typename T>
	_FORCE_INLINE_ void _untrack_handle(T p_handle) {}
#endif

	struct PendingFlushes {
		LocalVector<VmaAllocation> allocations;
		LocalVector<VkDeviceSize> offsets;
		LocalVector<VkDeviceSize> sizes;
	};

	PendingFlushes pending_flushes;

	struct PipelineStatistics {
		Ref<FileAccess> file_access;
		Mutex file_access_mutex;
	};

	PipelineStatistics pipeline_statistics;
	DriverWorkarounds driver_workarounds;
	BinaryMutex *powervr_rogue_shader_crash_workaround_mutex = nullptr;

	void _register_requested_device_extension(const CharString &p_extension_name, bool p_required);
	Error _initialize_device_extensions();
	void _check_driver_workarounds(const VkPhysicalDeviceProperties &p_device_properties, const VkPhysicalDeviceDriverPropertiesKHR *p_driver_properties);
	void _get_device_properties();
	Error _check_device_features();
	Error _check_device_capabilities();
	void _choose_vrs_capabilities();
	Error _add_queue_create_info(LocalVector<VkDeviceQueueCreateInfo> &r_queue_create_info);
	Error _initialize_device(const LocalVector<VkDeviceQueueCreateInfo> &p_queue_create_info);
	Error _initialize_allocator();
	Error _initialize_pipeline_cache();
	VkResult _create_render_pass(VkDevice p_device, const VkRenderPassCreateInfo2 *p_create_info, const VkAllocationCallbacks *p_allocator, VkRenderPass *p_render_pass);
	bool _release_image_semaphore(CommandQueue *p_command_queue, uint32_t p_semaphore_index, bool p_release_on_swap_chain);
	bool _recreate_image_semaphore(CommandQueue *p_command_queue, uint32_t p_semaphore_index, bool p_release_on_swap_chain);
	void _set_object_name(VkObjectType p_object_type, uint64_t p_object_handle, String p_object_name);

public:
	Error initialize(uint32_t p_device_index, uint32_t p_frame_count) override final;

private:
	/****************/
	/**** MEMORY ****/
	/****************/

	VmaAllocator allocator = nullptr;
	HashMap<uint32_t, VmaPool> small_allocs_pools;

	VmaPool _find_or_create_small_allocs_pool(uint32_t p_mem_type_index);

private:
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// It's a circular buffer.
	BufferID breadcrumb_buffer;
	uint32_t breadcrumb_offset = 0u;
	uint32_t breadcrumb_id = 0u;
#endif

public:
	/*****************/
	/**** BUFFERS ****/
	/*****************/
	struct BufferInfo {
		VkBuffer vk_buffer = VK_NULL_HANDLE;
		struct {
			VmaAllocation handle = nullptr;
			uint64_t size = UINT64_MAX;
		} allocation;
		uint64_t size = 0;
		VkBufferView vk_view = VK_NULL_HANDLE; // For texel buffers.
		// If dynamic buffer, then its range is [0; RenderingDeviceDriverVulkan::frame_count)
		// else it's UINT32_MAX.
		uint32_t frame_idx = UINT32_MAX;

		bool is_dynamic() const { return frame_idx != UINT32_MAX; }
	};

	struct BufferDynamicInfo : BufferInfo {
		uint8_t *persistent_ptr = nullptr;
#ifdef DEBUG_ENABLED
		// For tracking that a persistent buffer isn't mapped twice in the same frame.
		uint64_t last_frame_mapped = 0;
#endif
	};

	virtual BufferID buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) override final;
	virtual bool buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) override final;
	virtual void buffer_free(BufferID p_buffer) override final;
	virtual uint64_t buffer_get_allocation_size(BufferID p_buffer) override final;
	virtual uint8_t *buffer_map(BufferID p_buffer) override final;
	virtual void buffer_unmap(BufferID p_buffer) override final;
	virtual uint8_t *buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) override final;
	virtual uint64_t buffer_get_dynamic_offsets(Span<BufferID> p_buffers) override final;
	virtual void buffer_flush(BufferID p_buffer) override final;
	virtual uint64_t buffer_get_device_address(BufferID p_buffer) override final;

	/*****************/
	/**** TEXTURE ****/
	/*****************/

	struct TextureInfo {
		VkImage vk_image = VK_NULL_HANDLE;
		VkImageView vk_view = VK_NULL_HANDLE;
		DataFormat rd_format = DATA_FORMAT_MAX;
		VkImageCreateInfo vk_create_info = {};
		VkImageViewCreateInfo vk_view_create_info = {};
		struct {
			VmaAllocation handle = nullptr;
			VmaAllocationInfo info = {};
		} allocation; // All 0/null if just a view.
		bool is_subsampled = false;
#ifdef DEBUG_ENABLED
		bool created_from_extension = false;
		bool transient = false;
#endif
	};

	VkSampleCountFlagBits _ensure_supported_sample_count(TextureSamples p_requested_sample_count);
#ifdef DEBUG_ENABLED
	// GodotNext: pu_vk_extension_providers (#105).
	void _check_texture_handles(const TextureInfo *p_tex_info);
#endif

public:
	virtual TextureID texture_create(const TextureFormat &p_format, const TextureView &p_view) override final;
	virtual TextureID texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) override final;
	virtual TextureID texture_create_shared(TextureID p_original_texture, const TextureView &p_view) override final;
	virtual TextureID texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) override final;
	virtual void texture_free(TextureID p_texture) override final;
	virtual uint64_t texture_get_allocation_size(TextureID p_texture) override final;
	virtual void texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) override final;
	virtual Vector<uint8_t> texture_get_data(TextureID p_texture, uint32_t p_layer) override final;
	virtual BitField<TextureUsageBits> texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) override final;
	virtual bool texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) override final;

	/*****************/
	/**** SAMPLER ****/
	/*****************/
public:
	virtual SamplerID sampler_create(const SamplerState &p_state) final override;
	virtual void sampler_free(SamplerID p_sampler) final override;
	virtual bool sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) override final;

	/**********************/
	/**** VERTEX ARRAY ****/
	/**********************/
private:
	struct VertexFormatInfo {
		TightLocalVector<VkVertexInputBindingDescription> vk_bindings;
		TightLocalVector<VkVertexInputAttributeDescription> vk_attributes;
		VkPipelineVertexInputStateCreateInfo vk_create_info = {};
	};

public:
	virtual VertexFormatID vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) override final;
	virtual void vertex_format_free(VertexFormatID p_vertex_format) override final;

	/******************/
	/**** BARRIERS ****/
	/******************/

	virtual void command_pipeline_barrier(
			CommandBufferID p_cmd_buffer,
			BitField<PipelineStageBits> p_src_stages,
			BitField<PipelineStageBits> p_dst_stages,
			VectorView<MemoryAccessBarrier> p_memory_barriers,
			VectorView<BufferBarrier> p_buffer_barriers,
			VectorView<TextureBarrier> p_texture_barriers,
			VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) override final;

	/****************/
	/**** FENCES ****/
	/****************/

private:
	struct Fence {
		VkFence vk_fence = VK_NULL_HANDLE;
		CommandQueue *queue_signaled_from = nullptr;
	};

public:
	virtual FenceID fence_create() override final;
	virtual Error fence_wait(FenceID p_fence) override final;
	virtual void fence_free(FenceID p_fence) override final;

	/********************/
	/**** SEMAPHORES ****/
	/********************/

	virtual SemaphoreID semaphore_create() override final;
	virtual void semaphore_free(SemaphoreID p_semaphore) override final;

	/******************/
	/**** COMMANDS ****/
	/******************/

	// ----- QUEUE FAMILY -----

	virtual CommandQueueFamilyID command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface = 0) override final;

	// ----- QUEUE -----
private:
	struct CommandQueue {
		LocalVector<VkSemaphore> image_semaphores;
		LocalVector<SwapChain *> image_semaphores_swap_chains;
		LocalVector<uint32_t> pending_semaphores_for_execute;
		LocalVector<uint32_t> pending_semaphores_for_fence;
		LocalVector<uint32_t> free_image_semaphores;
		LocalVector<Pair<Fence *, uint32_t>> image_semaphores_for_fences;
		uint32_t queue_family = 0;
		uint32_t queue_index = 0;
	};

public:
	virtual CommandQueueID command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue = false) override final;
	virtual Error command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) override final;
	virtual void command_queue_free(CommandQueueID p_cmd_queue) override final;
	// GodotNext: async_compute (#174).
	virtual void command_queue_set_async_compute(CommandQueueID p_cmd_queue) override final;

private:
	// ----- POOL -----

	struct CommandPool {
		VkCommandPool vk_command_pool = VK_NULL_HANDLE;
		CommandBufferType buffer_type = COMMAND_BUFFER_TYPE_PRIMARY;
		LocalVector<CommandBufferInfo *> command_buffers_created;
	};

public:
	virtual CommandPoolID command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) override final;
	virtual bool command_pool_reset(CommandPoolID p_cmd_pool) override final;
	virtual void command_pool_free(CommandPoolID p_cmd_pool) override final;

private:
	// ----- BUFFER -----

	struct CommandBufferInfo {
		VkCommandBuffer vk_command_buffer = VK_NULL_HANDLE;
		Framebuffer *active_framebuffer = nullptr;
		RenderPassInfo *active_render_pass = nullptr;
	};

public:
	virtual CommandBufferID command_buffer_create(CommandPoolID p_cmd_pool) override final;
	virtual bool command_buffer_begin(CommandBufferID p_cmd_buffer) override final;
	virtual bool command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) override final;
	virtual void command_buffer_end(CommandBufferID p_cmd_buffer) override final;
	virtual void command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) override final;

	/********************/
	/**** SWAP CHAIN ****/
	/********************/

private:
	struct SwapChain {
		VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;
		RenderingContextDriver::SurfaceID surface = RenderingContextDriver::SurfaceID();
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		RDD::ColorSpace rdd_color_space = RDD::COLOR_SPACE_REC709_NONLINEAR_SRGB;
		TightLocalVector<VkImage> images;
		TightLocalVector<VkImageView> image_views;
		TightLocalVector<VkSemaphore> present_semaphores;
		TightLocalVector<FramebufferID> framebuffers;
		LocalVector<CommandQueue *> command_queues_acquired;
		LocalVector<uint32_t> command_queues_acquired_semaphores;
		RenderPassID render_pass;
		int pre_transform_rotation_degrees = 0;
		uint32_t image_index = 0;
#ifdef ANDROID_ENABLED
		uint64_t refresh_duration = 0;
#endif
	};

	bool _determine_swap_chain_format(RenderingContextDriver::SurfaceID p_surface, VkFormat &r_format, VkColorSpaceKHR &r_color_space, RDD::ColorSpace &r_rdd_color_space);
	void _swap_chain_release(SwapChain *p_swap_chain);

public:
	virtual SwapChainID swap_chain_create(RenderingContextDriver::SurfaceID p_surface) override final;
	virtual Error swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) override final;
	virtual FramebufferID swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) override final;
	virtual RenderPassID swap_chain_get_render_pass(SwapChainID p_swap_chain) override final;
	virtual int swap_chain_get_pre_rotation_degrees(SwapChainID p_swap_chain) override final;
	virtual DataFormat swap_chain_get_format(SwapChainID p_swap_chain) override final;
	virtual ColorSpace swap_chain_get_color_space(SwapChainID p_swap_chain) override final;
	virtual bool swap_chain_get_hdr_output_supported(SwapChainID p_swap_chain) override final;
	virtual void swap_chain_set_max_fps(SwapChainID p_swap_chain, int p_max_fps) override final;
	virtual void swap_chain_free(SwapChainID p_swap_chain) override final;

private:
	/*********************/
	/**** FRAMEBUFFER ****/
	/*********************/

	struct Framebuffer {
		VkFramebuffer vk_framebuffer = VK_NULL_HANDLE;

		// Only filled in if the framebuffer uses a fragment density map with offsets. Unused otherwise.
		uint32_t fragment_density_map_offsets_layers = 0;

		// Only filled in by a framebuffer created by a swap chain. Unused otherwise.
		VkImage swap_chain_image = VK_NULL_HANDLE;
		VkImageSubresourceRange swap_chain_image_subresource_range = {};
		bool swap_chain_acquired = false;
	};

public:
	virtual FramebufferID framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) override final;
	virtual void framebuffer_free(FramebufferID p_framebuffer) override final;

	/****************/
	/**** SHADER ****/
	/****************/
private:
	struct ShaderInfo {
		String name;
		VkShaderStageFlags vk_push_constant_stages = 0;
		TightLocalVector<VkPipelineShaderStageCreateInfo> vk_stages_create_info;
		TightLocalVector<VkDescriptorSetLayout> vk_descriptor_set_layouts;
		TightLocalVector<respv::Shader> respv_stage_shaders;
		TightLocalVector<Vector<uint8_t>> spirv_stage_bytes;
		TightLocalVector<uint64_t> original_stage_size;
		VkPipelineLayout vk_pipeline_layout = VK_NULL_HANDLE;
		// GodotNext: rd_rt_pipelines (#179). Each ray tracing stage's payload (RDRTPipeline::get_spirv_payload_size()).
		uint32_t raytracing_payload_sizes[SHADER_STAGE_MAX] = {};
	};

public:
	virtual ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) override final;
	virtual void shader_free(ShaderID p_shader) override final;

	virtual void shader_destroy_modules(ShaderID p_shader) override final;
	// GodotNext: rd_rt_pipelines (#179)
	virtual uint32_t shader_get_raytracing_payload_size(ShaderID p_shader, ShaderStage p_stage) const override final;
	/*********************/
	/**** UNIFORM SET ****/
	/*********************/

	// Descriptor sets require allocation from a pool.
	// The documentation on how to use pools properly
	// is scarce, and the documentation is strange.
	//
	// Basically, you can mix and match pools as you
	// like, but you'll run into fragmentation issues.
	// Because of this, the recommended approach is to
	// create a pool for every descriptor set type, as
	// this prevents fragmentation.
	//
	// This is implemented here as a having a list of
	// pools (each can contain up to 64 sets) for each
	// set layout. The amount of sets for each type
	// is used as the key.

private:
	static const uint32_t MAX_UNIFORM_POOL_ELEMENT = 65535;

	struct DescriptorSetPoolKey {
		uint16_t uniform_type[UNIFORM_TYPE_MAX] = {};

		bool operator<(const DescriptorSetPoolKey &p_other) const {
			return memcmp(uniform_type, p_other.uniform_type, sizeof(uniform_type)) < 0;
		}
	};

	using DescriptorSetPools = RBMap<DescriptorSetPoolKey, HashMap<VkDescriptorPool, uint32_t>>;
	DescriptorSetPools descriptor_set_pools;
	uint32_t max_descriptor_sets_per_pool = 0;

	HashMap<int, DescriptorSetPools> linear_descriptor_set_pools;
	bool linear_descriptor_pools_enabled = true;
	VkDescriptorPool _descriptor_set_pool_create(const DescriptorSetPoolKey &p_key, bool p_linear_pool);
	void _descriptor_set_pool_unreference(DescriptorSetPools::Iterator p_pool_sets_it, VkDescriptorPool p_vk_descriptor_pool, int p_linear_pool_index);

	// Global flag to toggle usage of immutable sampler when creating pipeline layouts.
	// It cannot change after creating the PSOs, since we need to skipping samplers when creating uniform sets.
	bool immutable_samplers_enabled = true;

	struct UniformSetInfo {
		VkDescriptorSet vk_descriptor_set = VK_NULL_HANDLE;
		VkDescriptorPool vk_descriptor_pool = VK_NULL_HANDLE;
		VkDescriptorPool vk_linear_descriptor_pool = VK_NULL_HANDLE;
		DescriptorSetPools::Iterator pool_sets_it;
		TightLocalVector<const BufferInfo *, uint32_t> dynamic_buffers;
		// GodotNext: rd_bindless (#176). The set is a bindless table's descriptor set, which the table owns.
		bool bindless_table = false;
	};

	bool adreno_5xx_empty_descriptor_set_layout_workaround = false;

public:
	virtual UniformSetID uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) override final;
	virtual void linear_uniform_set_pools_reset(int p_linear_pool_index) override final;
	virtual void uniform_set_free(UniformSetID p_uniform_set) override final;
	virtual bool uniform_sets_have_linear_pools() const override final;
	virtual uint32_t uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const override final;

	// ----- COMMANDS -----

	virtual void command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override final;

	/************************/
	/**** BINDLESS TABLE ****/
	/************************/

	// GodotNext: rd_bindless (#176). A table is one UPDATE_AFTER_BIND descriptor set, from a pool of its
	// own, with one partially bound, variable-count runtime array at binding 0. Shaders lay out the set
	// the same way (_bindless_set_layout_create()), so the table's set is compatible with them.
private:
	struct BindlessTableInfo {
		UniformType type = UNIFORM_TYPE_MAX;
		uint32_t capacity = 0;
		VkDescriptorSetLayout vk_layout = VK_NULL_HANDLE;
		VkDescriptorPool vk_pool = VK_NULL_HANDLE;
		VkDescriptorSet vk_set = VK_NULL_HANDLE;
	};

	static VkDescriptorType _bindless_descriptor_type(UniformType p_type);
	uint32_t _bindless_max_capacity(UniformType p_type) const;
	VkDescriptorSetLayout _bindless_set_layout_create(UniformType p_type);

public:
	virtual BindlessTableID bindless_table_create(UniformType p_type, uint32_t p_capacity) override final;
	virtual void bindless_table_write(BindlessTableID p_table, uint32_t p_slot, ID p_resource) override final;
	virtual void bindless_table_free(BindlessTableID p_table) override final;
	virtual uint64_t bindless_table_get_memory_usage(BindlessTableID p_table) const override final;

	/******************/
	/**** TRANSFER ****/
	/******************/

	virtual void command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) override final;
	virtual void command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) override final;

	virtual void command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) override final;
	virtual void command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) override final;
	virtual void command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) override final;
	virtual void command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) override final;

	virtual void command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) override final;
	virtual void command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) override final;

	/******************/
	/**** PIPELINE ****/
	/******************/
private:
	struct PipelineCacheHeader {
		uint32_t magic = 0;
		uint32_t data_size = 0;
		uint64_t data_hash = 0;
		uint32_t vendor_id = 0;
		uint32_t device_id = 0;
		uint32_t driver_version = 0;
		uint8_t uuid[VK_UUID_SIZE] = {};
		uint8_t driver_abi = 0;
	};

	struct PipelineCache {
		String file_path;
		size_t current_size = 0;
		Vector<uint8_t> buffer; // Header then data.
		VkPipelineCache vk_cache = VK_NULL_HANDLE;
	};

	static int caching_instance_count;
	PipelineCache pipelines_cache;
	String pipeline_cache_id;
	HashMap<uint64_t, bool> has_comp_alpha;

public:
	virtual void pipeline_free(PipelineID p_pipeline) override final;

	// ----- BINDING -----

	virtual void command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) override final;

	// ----- CACHE -----

	virtual bool pipeline_cache_create(const Vector<uint8_t> &p_data) override final;
	virtual void pipeline_cache_free() override final;
	virtual size_t pipeline_cache_query_size() override final;
	virtual Vector<uint8_t> pipeline_cache_serialize() override final;

	/*******************/
	/**** RENDERING ****/
	/*******************/

private:
	// ----- SUBPASS -----

	struct RenderPassInfo {
		VkRenderPass vk_render_pass = VK_NULL_HANDLE;
		bool uses_fragment_density_map = false;
	};

public:
	virtual RenderPassID render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) override final;
	virtual void render_pass_free(RenderPassID p_render_pass) override final;

	// ----- COMMANDS -----

	virtual void command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) override final;
	virtual void command_end_render_pass(CommandBufferID p_cmd_buffer) override final;
	virtual void command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) override final;
	virtual void command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) override final;
	virtual void command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) override final;
	virtual void command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) override final;

	// Binding.
	virtual void command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override final;
	virtual void command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override final;

	// Drawing.
	virtual void command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) override final;
	virtual void command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) override final;
	virtual void command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override final;
	virtual void command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override final;
	virtual void command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override final;
	virtual void command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override final;
	// GodotNext: rd_mesh_shaders (#178)
	virtual void command_render_draw_mesh_tasks(CommandBufferID p_cmd_buffer, uint32_t p_group_count_x, uint32_t p_group_count_y, uint32_t p_group_count_z) override final;
	virtual void command_render_draw_mesh_tasks_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override final;
	virtual void command_render_draw_mesh_tasks_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override final;

	// Buffer binding.
	virtual void command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) override final;
	virtual void command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) override final;

	// Dynamic state.
	virtual void command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) override final;
	virtual void command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) override final;

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
			VectorView<PipelineSpecializationConstant> p_specialization_constants) override final;

	/*****************/
	/**** COMPUTE ****/
	/*****************/

	// ----- COMMANDS -----

	// Binding.
	virtual void command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override final;
	virtual void command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override final;

	// Dispatching.
	virtual void command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) override final;
	virtual void command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) override final;

	// ----- PIPELINE -----

	virtual PipelineID compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) override final;

	/********************/
	/**** RAYTRACING ****/
	/********************/

	// ----- ACCELERATION STRUCTURE -----

	struct AccelerationStructureInfo {
		VkAccelerationStructureKHR vk_acceleration_structure = VK_NULL_HANDLE;
		// Buffer used for the structure
		RDD::BufferID buffer;
		// GodotNext: rq_substrate (#22). vkGetAccelerationStructureDeviceAddressKHR, the address TLAS
		// instances reference, and accelerationStructureSize, the bytes RenderingDevice counts.
		VkDeviceAddress device_address = 0;
		uint64_t size_bytes = 0;

		// Alignment of the scratch buffer for building the structure
		uint32_t scratch_alignment;
		// Size of the scratch buffer for building the structure
		uint32_t scratch_size;
		// GodotNext: rq_substrate (#22). Size of the scratch for an UPDATE (refit), alignment included.
		uint32_t update_scratch_size = 0;

		// Required for building
		TightLocalVector<VkAccelerationStructureGeometryKHR> geometries;
		VkAccelerationStructureBuildGeometryInfoKHR build_info;
		TightLocalVector<VkAccelerationStructureBuildRangeInfoKHR> range_infos;
	};

	virtual AccelerationStructureID blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) override final;
	virtual bool acceleration_structure_vertex_format_is_supported(DataFormat p_format) override final;
	virtual AccelerationStructureID tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) override final;
	virtual void acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) override final;
	virtual uint64_t acceleration_structure_get_instance_reference(AccelerationStructureID p_acceleration_structure) override final;
	// GodotNext: rq_substrate (#22)
	virtual uint64_t acceleration_structure_get_size_bytes(AccelerationStructureID p_acceleration_structure) override final;
	virtual bool acceleration_structure_compaction_supported() const override final;
	virtual QueryPoolID acceleration_structure_size_query_pool_create(uint32_t p_count) override final;
	virtual void acceleration_structure_size_query_pool_free(QueryPoolID p_pool_id) override final;
	virtual void acceleration_structure_size_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) override final;
	virtual void command_acceleration_structure_write_compacted_size(CommandBufferID p_cmd_buffer, VectorView<AccelerationStructureID> p_acceleration_structures, QueryPoolID p_pool_id, uint32_t p_first) override final;
	virtual AccelerationStructureID acceleration_structure_create_compacted(AccelerationStructureID p_src, uint64_t p_size) override final;
	virtual void command_copy_acceleration_structure_compact(CommandBufferID p_cmd_buffer, AccelerationStructureID p_src, AccelerationStructureID p_dst) override final;
	virtual void acceleration_structure_free(AccelerationStructureID p_acceleration_structure) override final;
	// GodotNext: as_build_modes
	virtual uint32_t acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure, AccelerationStructureBuildMode p_mode) override final;

private:
	void _acceleration_structure_create(VkAccelerationStructureTypeKHR p_type, VkAccelerationStructureBuildSizesInfoKHR p_size_info, AccelerationStructureInfo *r_accel_info);

private:
	VkStridedDeviceAddressRegionKHR _sbt_to_vk_strided_device_address_region(const ShaderBindingTable &p_sbt);

public:
	// ----- COMMANDS -----
	// GodotNext: as_build_modes
	virtual void command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, uint64_t p_scratch_offset, AccelerationStructureBuildMode p_mode) override final;
	virtual void command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, uint64_t p_scratch_offset, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count, AccelerationStructureBuildMode p_mode) override final;
	virtual void command_bind_raytracing_pipeline(CommandBufferID p_cmd_buffer, RaytracingPipelineID p_pipeline) override final;
	virtual void command_bind_raytracing_uniform_set(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override final;
	virtual void command_trace_rays(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) override final;
	// GodotNext: rd_rt_pipelines (#179)
	virtual void command_trace_rays_indirect(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, BufferID p_indirect_buffer, uint64_t p_offset) override final;

private:
	// GodotNext: rd_rt_pipelines (#179). raytracing_pipeline_create() and raytracing_pipeline_library_create(),
	// with the pipeline flags and the library interface (null for a pipeline that uses no libraries).
	RaytracingPipelineID _raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader, VkPipelineCreateFlags p_flags, const VkRayTracingPipelineInterfaceCreateInfoKHR *p_library_interface);

public:
	// ----- PIPELINE -----
	virtual RaytracingPipelineID raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader) override final;
	virtual void raytracing_pipeline_free(RaytracingPipelineID p_pipeline) override final;

	virtual bool raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID p_pipeline, uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes) override final;

	// GodotNext: rd_rt_pipelines (#179)
	virtual RaytracingPipelineID raytracing_pipeline_library_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, uint32_t p_max_payload_size, ShaderID p_layout_defining_shader) override final;
	virtual RaytracingPipelineID raytracing_pipeline_link(VectorView<RaytracingPipelineID> p_libraries, uint32_t p_max_trace_recursion_depth, uint32_t p_max_payload_size, ShaderID p_layout_defining_shader) override final;

	/*****************/
	/**** QUERIES ****/
	/*****************/

	// ----- TIMESTAMP -----

	// Basic.
	virtual QueryPoolID timestamp_query_pool_create(uint32_t p_query_count) override final;
	virtual void timestamp_query_pool_free(QueryPoolID p_pool_id) override final;
	virtual void timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) override final;
	virtual uint64_t timestamp_query_result_to_time(uint64_t p_result) override final;

	// Commands.
	virtual void command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) override final;
	virtual void command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) override final;

	/****************/
	/**** LABELS ****/
	/****************/

	virtual void command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) override final;
	virtual void command_end_label(CommandBufferID p_cmd_buffer) override final;

	/****************/
	/**** DEBUG *****/
	/****************/
	virtual void command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) override final;
	void print_lost_device_info();
	void on_device_lost() const;
	static String get_vulkan_result(VkResult err);

	/********************/
	/**** SUBMISSION ****/
	/********************/

	virtual void begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) override final;
	virtual void end_segment() override final;

	/**************/
	/**** MISC ****/
	/**************/

	virtual void set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) override final;
	virtual uint64_t get_resource_native_handle(DriverResource p_type, ID p_driver_id) override final;
	// GodotNext: pu_core_rd_native_interop (#102).
	virtual uint64_t command_buffer_get_native_handle(CommandBufferID p_cmd_buffer) override final;
	virtual Error texture_get_native_info(TextureID p_texture, TextureNativeInfo &r_info) override final;
	static void texture_native_info_from_create_infos(const VkImageCreateInfo &p_image_info, const VkImageViewCreateInfo &p_view_info, VkImageView p_view, TextureNativeInfo &r_info);
	virtual uint64_t get_total_memory_used() override final;
	virtual uint64_t get_lazily_memory_used() override final;
	virtual uint64_t limit_get(Limit p_limit) override final;
	virtual uint64_t api_trait_get(ApiTrait p_trait) override final;
	virtual bool has_feature(Features p_feature) override final;
	virtual RayQueryUnsupportedReason get_ray_query_unsupported_reason() const override final { return ray_query_caps.reason; }
	virtual const MultiviewCapabilities &get_multiview_capabilities() override final;
	virtual const FragmentShadingRateCapabilities &get_fragment_shading_rate_capabilities() override final;
	virtual const FragmentDensityMapCapabilities &get_fragment_density_map_capabilities() override final;
	virtual String get_api_name() const override final;
	virtual String get_api_version() const override final;
	// GodotNext: hwrt_safety_net
	virtual String get_driver_version_key() const override final;
	virtual String get_pipeline_cache_uuid() const override final;
	virtual const Capabilities &get_capabilities() const override final;
	virtual const RenderingShaderContainerFormat &get_shader_container_format() const override final;

	virtual bool is_composite_alpha_supported(CommandQueueID p_queue) const override final;

	virtual DriverWorkarounds get_driver_workarounds() const override final;

private:
	/*********************/
	/**** BOOKKEEPING ****/
	/*********************/

	using VersatileResource = VersatileResourceTemplate<
			BufferInfo,
			TextureInfo,
			VertexFormatInfo,
			ShaderInfo,
			UniformSetInfo,
			RenderPassInfo,
			CommandBufferInfo>;
	PagedAllocator<VersatileResource, true> resources_allocator;

	/******************/

public:
	RenderingDeviceDriverVulkan(RenderingContextDriverVulkan *p_context_driver);
	virtual ~RenderingDeviceDriverVulkan();
};

using VKC = RenderingContextDriverVulkan;
