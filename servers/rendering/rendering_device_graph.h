/**************************************************************************/
/*  rendering_device_graph.h                                              */
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

#include "core/object/worker_thread_pool.h"
#include "servers/rendering/rendering_device_commons.h"
#include "servers/rendering/rendering_device_driver.h"

#ifdef DEV_ENABLED
#include "core/templates/rb_map.h"
#endif

// Buffer barriers have not shown any significant improvement or shown to be
// even detrimental to performance. However, there are currently some known
// cases where using them can solve problems that using singular memory
// barriers does not, probably due to driver issues (see comment on PR #84976
// https://github.com/godotengine/godot/pull/84976#issuecomment-1878566830).

#define USE_BUFFER_BARRIERS 1

class RenderingDeviceGraph {
public:
	struct RaytracingListInstruction {
		enum Type {
			TYPE_NONE,
			TYPE_BIND_PIPELINE,
			TYPE_BIND_UNIFORM_SET,
			TYPE_SET_PUSH_CONSTANT,
			TYPE_TRACE_RAYS,
			TYPE_UNIFORM_SET_PREPARE_FOR_USE,
			// GodotNext: rd_rt_pipelines (#179)
			TYPE_TRACE_RAYS_INDIRECT,
		};

		Type type = TYPE_NONE;
	};

	struct ComputeListInstruction {
		enum Type {
			TYPE_NONE,
			TYPE_BIND_PIPELINE,
			TYPE_BIND_UNIFORM_SETS,
			TYPE_DISPATCH,
			TYPE_DISPATCH_INDIRECT,
			TYPE_SET_PUSH_CONSTANT,
			TYPE_UNIFORM_SET_PREPARE_FOR_USE
		};

		Type type = TYPE_NONE;
	};

	struct DrawListInstruction {
		enum Type {
			TYPE_NONE,
			TYPE_BIND_INDEX_BUFFER,
			TYPE_BIND_PIPELINE,
			TYPE_BIND_UNIFORM_SETS,
			TYPE_BIND_VERTEX_BUFFERS,
			TYPE_CLEAR_ATTACHMENTS,
			TYPE_DRAW,
			TYPE_DRAW_INDEXED,
			TYPE_DRAW_INDIRECT,
			TYPE_DRAW_INDEXED_INDIRECT,
			TYPE_EXECUTE_COMMANDS,
			TYPE_NEXT_SUBPASS,
			TYPE_SET_BLEND_CONSTANTS,
			TYPE_SET_LINE_WIDTH,
			TYPE_SET_PUSH_CONSTANT,
			TYPE_SET_SCISSOR,
			TYPE_SET_VIEWPORT,
			TYPE_UNIFORM_SET_PREPARE_FOR_USE,
			// GodotNext: rd_indirect_count (#177)
			TYPE_DRAW_INDIRECT_COUNT,
			TYPE_DRAW_INDEXED_INDIRECT_COUNT,
			// GodotNext: rd_mesh_shaders (#178)
			TYPE_DRAW_MESH_TASKS,
			TYPE_DRAW_MESH_TASKS_INDIRECT,
			TYPE_DRAW_MESH_TASKS_INDIRECT_COUNT,
		};

		Type type = TYPE_NONE;
	};

	// GodotNext: async_compute (#174). The queue a recorded command runs on.
	enum Queue : uint8_t {
		QUEUE_MAIN,
		QUEUE_ASYNC_COMPUTE,
	};

	struct RecordedCommand {
		enum Type {
			TYPE_NONE,
			TYPE_BOTTOM_LEVEL_ACCELERATION_STRUCTURE_BUILD,
			TYPE_TOP_LEVEL_ACCELERATION_STRUCTURE_BUILD,
			TYPE_BUFFER_CLEAR,
			TYPE_BUFFER_COPY,
			TYPE_BUFFER_GET_DATA,
			TYPE_BUFFER_UPDATE,
			TYPE_COMPUTE_LIST,
			TYPE_RAYTRACING_LIST,
			TYPE_DRAW_LIST,
			TYPE_TEXTURE_CLEAR_COLOR,
			TYPE_TEXTURE_CLEAR_DEPTH_STENCIL,
			TYPE_TEXTURE_COPY,
			TYPE_TEXTURE_GET_DATA,
			TYPE_TEXTURE_RESOLVE,
			TYPE_TEXTURE_UPDATE,
			TYPE_CAPTURE_TIMESTAMP,
			TYPE_DRIVER_CALLBACK,
			// GodotNext: blas_compact
			TYPE_ACCELERATION_STRUCTURE_QUERY_SIZE,
			TYPE_ACCELERATION_STRUCTURE_COPY,
			TYPE_MAX
		};

		Type type = TYPE_NONE;
		int32_t adjacent_command_list_index = -1;
		RDD::MemoryAccessBarrier memory_barrier;
		int32_t normalization_barrier_index = -1;
		int normalization_barrier_count = 0;
		int32_t transition_barrier_index = -1;
		int32_t transition_barrier_count = 0;
#if USE_BUFFER_BARRIERS
		int32_t buffer_barrier_index = -1;
		int32_t buffer_barrier_count = 0;
#endif
		int32_t acceleration_structure_barrier_index = -1;
		int32_t acceleration_structure_barrier_count = 0;
		int32_t label_index = -1;
		// GodotNext: async_compute (#174).
		uint8_t queue = QUEUE_MAIN;
		BitField<RDD::PipelineStageBits> previous_stages = {};
		BitField<RDD::PipelineStageBits> next_stages = {};
		BitField<RDD::PipelineStageBits> self_stages = {};
	};

	struct RecordedBufferCopy {
		RDD::BufferID source;
		RDD::BufferCopyRegion region;
	};

	struct RecordedBufferToTextureCopy {
		RDD::BufferID from_buffer;
		RDD::BufferTextureCopyRegion region;
	};

	enum ResourceUsage {
		RESOURCE_USAGE_NONE,
		RESOURCE_USAGE_COPY_FROM,
		RESOURCE_USAGE_COPY_TO,
		RESOURCE_USAGE_RESOLVE_FROM,
		RESOURCE_USAGE_RESOLVE_TO,
		RESOURCE_USAGE_UNIFORM_BUFFER_READ,
		RESOURCE_USAGE_INDIRECT_BUFFER_READ,
		RESOURCE_USAGE_TEXTURE_BUFFER_READ,
		RESOURCE_USAGE_TEXTURE_BUFFER_READ_WRITE,
		RESOURCE_USAGE_STORAGE_BUFFER_READ,
		RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE,
		RESOURCE_USAGE_VERTEX_BUFFER_READ,
		RESOURCE_USAGE_INDEX_BUFFER_READ,
		RESOURCE_USAGE_TEXTURE_SAMPLE,
		RESOURCE_USAGE_STORAGE_IMAGE_READ,
		RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE,
		RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE,
		RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE,
		RESOURCE_USAGE_ATTACHMENT_FRAGMENT_SHADING_RATE_READ,
		RESOURCE_USAGE_ATTACHMENT_FRAGMENT_DENSITY_MAP_READ,
		RESOURCE_USAGE_ATTACHMENT_RASTERIZATION_RATE_MAP_READ,
		RESOURCE_USAGE_GENERAL,
		RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ,
		RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE,
		// GodotNext: rd_readonly_depth (#301). Read-only depth/stencil attachment (depth test without
		// depth or stencil writes), sharing TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL with
		// sampled reads of depth/stencil textures.
		RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ,
		RESOURCE_USAGE_MAX
	};

	// GodotNext: rd_bindless (#176).
	struct BindlessTableTracker;

	// GodotNext: rd_bindless (#176). A bindless table whose slots hold a resource.
	struct BindlessResidency {
		BindlessTableTracker *table = nullptr;
		// How many of the table's slots hold the resource.
		uint32_t slots = 0;
		// On the table's dirty list: the table has not read the resource since a command left it unready.
		bool dirty = false;
		// The command that made it dirty last. When that command also binds the table, the resource stays
		// dirty after it.
		int64_t dirty_frame = -1;
		int32_t dirty_command_index = -1;
	};

	struct ResourceTracker {
		uint32_t reference_count = 0;
		int64_t command_frame = -1;
		int32_t command_index = -1;
		uint32_t usage_index = UINT32_MAX;
		BitField<RDD::PipelineStageBits> previous_frame_stages = {};
		BitField<RDD::PipelineStageBits> current_frame_stages = {};
		int32_t read_full_command_list_index = -1;
		int32_t read_slice_command_list_index = -1;
		int32_t write_command_or_list_index = -1;
		int32_t draw_list_index = -1;
		ResourceUsage draw_list_usage = RESOURCE_USAGE_NONE;
		int32_t compute_list_index = -1;
		int32_t raytracing_list_index = -1;
		ResourceUsage compute_list_usage = RESOURCE_USAGE_NONE;
		ResourceUsage raytracing_list_usage = RESOURCE_USAGE_NONE;
		ResourceUsage usage = RESOURCE_USAGE_NONE;
		BitField<RDD::BarrierAccessBits> usage_access = {};
		// GodotNext: rdg_access_stages (#267). The stages of the command that made usage_access, kept
		// with it across frames and copied with it to slices, so a barrier that waits on that access
		// also waits on its stages.
		BitField<RDD::PipelineStageBits> usage_stages = {};
		RDD::BufferID buffer_driver_id;
		RDD::TextureID texture_driver_id;
		RDD::AccelerationStructureID acceleration_structure_driver_id;
		RDD::TextureSubresourceRange texture_subresources;
		Size2i texture_size;
		uint32_t texture_usage = 0;
		int32_t texture_slice_command_index = -1;
		// GodotNext: async_compute (#174) & px_rd_async_multiview (#300). Async compute lists of the
		// current tracking frame that used this resource (whole resource or per-slice rects). A slice
		// records on its parent; both reset each frame in reset_if_outdated().
		int32_t async_full_command_index = -1;
		int32_t async_slice_command_list_index = -1;
		// GodotNext: rd_readonly_depth (#301). Active entry in shared_read_transitions for this frame,
		// or -1 when the resource has not entered the shared read-only depth layout since its last write.
		int32_t shared_read_transition_index = -1;
		ResourceTracker *parent = nullptr;
		ResourceTracker *dirty_shared_list = nullptr;
		ResourceTracker *next_shared = nullptr;
		Rect2i texture_slice_or_dirty_rect;
		bool in_parent_dirty_list = false;
		bool write_command_list_enabled = false;
		bool is_discardable = false;
		// GodotNext: rd_bindless (#176). The bindless tables whose slots hold this resource. Only whole
		// resources are resident: a texture slice is not, but a command that uses one dirties its parent.
		LocalVector<BindlessResidency> bindless_residency;

		_FORCE_INLINE_ void reset_if_outdated(int64_t new_command_frame) {
			if (new_command_frame != command_frame) {
				command_frame = new_command_frame;
				command_index = -1;
				usage_index = UINT32_MAX;
				previous_frame_stages = current_frame_stages;
				current_frame_stages.clear();
				read_full_command_list_index = -1;
				read_slice_command_list_index = -1;
				write_command_or_list_index = -1;
				draw_list_index = -1;
				compute_list_index = -1;
				raytracing_list_index = -1;
				texture_slice_command_index = -1;
				write_command_list_enabled = false;
				// GodotNext: px_rd_async_multiview (#300).
				async_full_command_index = -1;
				async_slice_command_list_index = -1;
				// GodotNext: rd_readonly_depth (#301).
				shared_read_transition_index = -1;
			}
		}
	};

	// GodotNext: rd_bindless (#176). A bindless table as the graph sees it. A command that binds the table
	// reads every resource its slots hold, but only the dirty ones become usages of the command, so the
	// command gets barriers for those alone and not for the whole table. A resident resource becomes dirty
	// when a command uses it in a way that leaves it unready for the table's read: any write, and for a
	// texture any usage but sampling, as that changes its layout. The table orders its own reads against
	// those commands: a command that dirties a resident resource waits for the commands that bound the
	// table, and a command that binds the table waits for the binds that made dirty resources ready.
	struct BindlessTableTracker {
		// RESOURCE_USAGE_TEXTURE_SAMPLE for texture tables, RESOURCE_USAGE_STORAGE_BUFFER_READ for
		// storage-buffer tables. Sampler tables have no resources to track.
		ResourceUsage read_usage = RESOURCE_USAGE_NONE;
		// Resident resources the table has not read since a command left them unready, each once.
		LocalVector<ResourceTracker *> dirty;
		// The commands of tracking frame command_frame that bound the table, and those of them that
		// read dirty resources (made them ready).
		int64_t command_frame = -1;
		LocalVector<int32_t> read_commands;
		LocalVector<int32_t> ready_commands;
		// Every stage that has bound the table, in any frame. A command that dirties a resident resource
		// waits for these stages, as the reads before it can be from an earlier frame.
		BitField<RDD::PipelineStageBits> read_stages = {};
	};

	typedef RDD::RenderPassID (*RenderPassCreationFunction)(RenderingDeviceDriver *p_driver, VectorView<RDD::AttachmentLoadOp> p_load_ops, VectorView<RDD::AttachmentStoreOp> p_store_ops, void *p_user_data);

	struct FramebufferStorage {
		RDD::FramebufferID framebuffer;
		RDD::RenderPassID render_pass;
	};

	// GodotNext: rd_readonly_depth (#301). Load and store ops each have 3 values (2 bits each);
	// packing 4 bits per attachment across two 64-bit words supports up to 32 attachments.
	struct FramebufferStorageKey {
		uint64_t lo = 0;
		uint64_t hi = 0;

		bool operator==(const FramebufferStorageKey &p_other) const {
			return lo == p_other.lo && hi == p_other.hi;
		}

		static uint32_t hash(const FramebufferStorageKey &p_key) {
			uint32_t h = hash_murmur3_one_64(p_key.lo);
			return hash_fmix32(hash_murmur3_one_64(p_key.hi, h));
		}
	};

	struct FramebufferCache {
		uint32_t width = 0;
		uint32_t height = 0;
		LocalVector<RDD::TextureID> textures;
		LocalVector<ResourceTracker *> trackers;
		HashMap<FramebufferStorageKey, FramebufferStorage, FramebufferStorageKey> storage_map;
		void *render_pass_creation_user_data = nullptr;
	};

	struct CommandBufferPool {
		// Provided by RenderingDevice.
		RDD::CommandPoolID pool;

		// Created internally by RenderingDeviceGraph.
		LocalVector<RDD::CommandBufferID> buffers;
		LocalVector<RDD::SemaphoreID> semaphores;
		uint32_t buffers_used = 0;

		// GodotNext: async_compute (#174). Provided by RenderingDevice while async compute is
		// active; null otherwise. end() records the frame's async work into it, and the indices
		// (0 = the frame command buffer, k = buffers[k - 1]) of the main command buffer the async
		// work waits for and of the one that waits for it. -1 when nothing ran async.
		RDD::CommandBufferID async_command_buffer;
		int32_t async_fork_index = -1;
		int32_t async_join_index = -1;
		// Set by end() when async lists were recorded but had to run on the main queue.
		bool async_fallback = false;
	};

	// GodotNext: async_compute (#174). How end() splits a frame between the main and the async
	// compute queue: async work starts after the main levels before fork_level and must finish
	// before the main levels from join_level on. Inactive means everything runs on the main queue.
	struct AsyncPlan {
		bool active = false;
		uint32_t fork_level = 0;
		uint32_t join_level = 0;
		uint32_t level_count = 0;
		uint32_t main_count = 0;
		uint32_t async_count = 0;
	};

	// GodotNext: async_compute (#174). What end() records for a frame with an active async plan,
	// for the doctests. Buffer indices count as in CommandBufferPool (0 = the frame command
	// buffer, k = buffers[k - 1]).
	struct AsyncEmission {
		enum StepType : uint8_t {
			STEP_SPLIT, // The main command buffer ends and `buffer` begins.
			STEP_MAIN, // Main commands of `level`, recorded into main command buffer `buffer`.
			STEP_ASYNC, // Async commands of `level`, recorded into the async command buffer.
		};

		struct Step {
			StepType type = STEP_SPLIT;
			uint32_t level = 0;
			uint32_t buffer = 0;
			LocalVector<int32_t> commands;
		};

		LocalVector<Step> steps;
		int32_t fork_index = -1;
		int32_t join_index = -1;
		uint32_t buffers_used = 0;
		// The main command buffer that records the BARRIER_SELECTION_ASYNC_FORK barriers, just before
		// the fork split; -1 when the frame did not fork.
		int32_t fork_barrier_buffer = -1;
	};

	// GodotNext: async_compute (#174). Everything RenderingDevice::execute_chained_cmds() submits
	// for a frame, as driver IDs, so submit_chained_command_buffers() can order the submissions
	// without a driver.
	struct ChainedSubmission {
		RDD::CommandQueueID main_queue;
		RDD::CommandQueueID async_queue;
		// The frame command buffer (index 0), then pool->buffers, each signaling pool->semaphores.
		RDD::CommandBufferID frame_command_buffer;
		const CommandBufferPool *pool = nullptr;
		uint32_t command_buffer_count = 1;
		// The first command buffer waits on these.
		VectorView<RDD::SemaphoreID> frame_wait_semaphores;
		// The last command buffer signals these and presents the swap chains.
		RDD::SemaphoreID last_signal_semaphore;
		RDD::FenceID last_signal_fence;
		VectorView<RDD::SwapChainID> last_swap_chains;
		// From CommandBufferPool; -1 when nothing runs async.
		int32_t async_fork_index = -1;
		int32_t async_join_index = -1;
		RDD::CommandBufferID async_command_buffer;
		RDD::SemaphoreID async_fork_semaphore;
		RDD::SemaphoreID async_join_semaphore;
	};

	// GodotNext: async_compute (#174). Which barriers of a level's commands a barrier group takes.
	// On drivers with image layouts (Vulkan, D3D12) a compute-only queue cannot run every barrier:
	// it supports only compute and copy stages and accesses, and D3D12 does not allow render-target,
	// depth or resolve layouts on a compute command list. The barriers of async commands therefore
	// split in two. Each barrier runs whole on one queue; the fork semaphore orders the main-queue
	// work before it. Metal, which has no layouts, keeps every barrier as recorded.
	enum BarrierSelection : uint8_t {
		// Every barrier as recorded, on the main queue.
		BARRIER_SELECTION_ALL,
		// What the async compute queue runs: the stages and accesses it supports, without the texture
		// barriers of BARRIER_SELECTION_ASYNC_FORK.
		BARRIER_SELECTION_ASYNC_QUEUE,
		// The async commands' texture barriers from or to a layout the async queue cannot use. Their
		// source access is on the main queue, before the fork, so the main queue runs them at the end
		// of the fork command buffer.
		BARRIER_SELECTION_ASYNC_FORK,
	};

	// GodotNext: async_compute (#174). The barriers a selection takes from the async commands of a
	// frame, for the doctests.
	struct AsyncBarriers {
		BitField<RDD::PipelineStageBits> src_stages = {};
		BitField<RDD::PipelineStageBits> dst_stages = {};
		RDD::MemoryAccessBarrier memory_barrier;
		LocalVector<RDD::TextureBarrier> texture_barriers;
		LocalVector<RDD::BufferBarrier> buffer_barriers;
	};

	// GodotNext: async_compute (#174). Submits one command buffer: RenderingDevice passes the
	// driver, the doctests a recorder.
	typedef void (*ChainedSubmitFunction)(void *p_userdata, RDD::CommandQueueID p_queue, VectorView<RDD::SemaphoreID> p_wait_semaphores, RDD::CommandBufferID p_command_buffer, VectorView<RDD::SemaphoreID> p_signal_semaphores, RDD::FenceID p_signal_fence, VectorView<RDD::SwapChainID> p_swap_chains);

	struct WorkaroundsState {
		bool draw_list_found = false;
		bool bound_any_draw_list_pipeline = false;
	};

	// GodotNext: rdg_cross_frame_waw (#262). A buffer's first use in a frame needs a barrier with
	// its last access when both write and the last use was in an earlier frame: the tracker keeps
	// its usage across frames but not its write index, so the same usage written twice (two copies
	// of a uniform buffer with no read between, as when a frame updates the canvas state and draws
	// no canvas item) would otherwise get no barrier.
	static bool cross_frame_write_after_write(ResourceUsage p_last_usage, ResourceUsage p_new_usage, bool p_first_use_this_frame, bool p_is_buffer);

	// GodotNext: async_compute (#174). What a compute-only queue supports in a barrier, on drivers with
	// image layouts. async_queue_stages() drops graphics stages and, when it drops any, adds the
	// compute stage: the dropped stages belong to main-queue work that the fork semaphore already
	// waited for, and the compute stage chains the barrier to that wait. async_queue_access() drops
	// graphics accesses.
	static bool async_queue_supports_texture_layout(RDD::TextureLayout p_layout);
	static BitField<RDD::PipelineStageBits> async_queue_stages(BitField<RDD::PipelineStageBits> p_stages);
	static BitField<RDD::BarrierAccessBits> async_queue_access(BitField<RDD::BarrierAccessBits> p_access);

	// GodotNext: rd_clear_values (#275). The clear values a draw list passes to
	// RDD::command_begin_render_pass(): one per attachment of the driver's render pass and
	// framebuffer, in their order. RenderingDevice gives one per framebuffer slot, but it leaves the
	// unused slots (null trackers in p_framebuffer_cache) out of the render pass and the framebuffer
	// it creates in the driver, so their values are dropped. Without a framebuffer cache (a screen's
	// render pass) every value is kept.
	static void draw_list_driver_clear_values(const FramebufferCache *p_framebuffer_cache, VectorView<RDD::RenderPassClearValue> p_slot_clear_values, LocalVector<RDD::RenderPassClearValue> &r_clear_values);

	enum AttachmentOperation {
		// Loads or ignores if the attachment is discardable.
		ATTACHMENT_OPERATION_DEFAULT,
		// Clear the attachment to a value.
		ATTACHMENT_OPERATION_CLEAR,
		// Ignore any contents from the attachment.
		ATTACHMENT_OPERATION_IGNORE,
		// GodotNext: rd_readonly_depth (#301). Attach depth/stencil read-only (load, store op none).
		ATTACHMENT_OPERATION_READ_ONLY,
	};

private:
	struct InstructionList {
		LocalVector<uint8_t> data;
		LocalVector<ResourceTracker *> command_trackers;
		LocalVector<ResourceUsage> command_tracker_usages;
		// GodotNext: rd_bindless (#176). The bindless tables the list binds, each once.
		LocalVector<BindlessTableTracker *> bindless_tables;
		BitField<RDD::PipelineStageBits> stages = {};
		int32_t index = 0;

		void clear() {
			data.clear();
			command_trackers.clear();
			command_tracker_usages.clear();
			bindless_tables.clear();
			stages.clear();
		}
	};

	struct ComputeInstructionList : InstructionList {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		uint32_t breadcrumb;
#endif
		// GodotNext: async_compute (#174).
		Queue queue = QUEUE_MAIN;
	};

	struct RaytracingInstructionList : InstructionList {
		// No extra contents.
	};

	struct DrawInstructionList : InstructionList {
		FramebufferCache *framebuffer_cache = nullptr;
		RDD::RenderPassID render_pass;
		RDD::FramebufferID framebuffer;
		Rect2i region;
		LocalVector<AttachmentOperation> attachment_operations;
		// GodotNext: rd_clear_values (#275). One per driver attachment
		// (draw_list_driver_clear_values()); attachment_operations has one per framebuffer slot.
		LocalVector<RDD::RenderPassClearValue> attachment_clear_values;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		uint32_t breadcrumb;
#endif
		bool split_cmd_buffer = false;
		// GodotNext: rd_indirect_count (#177). Where the indirect-count draws sit in data.
		LocalVector<uint32_t> indirect_count_offsets;
		// GodotNext: rd_readonly_depth (#301). Depth/stencil trackers attached read-only in this
		// draw list that are also dirty in a bound bindless texture table, so _add_command_to_graph()
		// merges SHADER_READ access into their read-only depth transition without a duplicate usage.
		LocalVector<ResourceTracker *> bindless_readonly_depth_trackers;
	};

	struct RecordedCommandSort {
		uint32_t level = 0;
		uint32_t priority = 0;
		int32_t index = -1;
		// GodotNext: async_compute (#174). Nonzero only while an async plan is active, so async
		// commands sort to the end of their level.
		uint8_t queue = QUEUE_MAIN;

		RecordedCommandSort() = default;

		bool operator<(const RecordedCommandSort &p_other) const {
			if (level < p_other.level) {
				return true;
			} else if (level > p_other.level) {
				return false;
			}

			// GodotNext: async_compute (#174). Within a level, async commands sort after the main ones.
			if (queue < p_other.queue) {
				return true;
			} else if (queue > p_other.queue) {
				return false;
			}

			if (priority < p_other.priority) {
				return true;
			} else if (priority > p_other.priority) {
				return false;
			}

			return index < p_other.index;
		}
	};

	struct RecordedCommandListNode {
		int32_t command_index = -1;
		int32_t next_list_index = -1;
	};

	struct RecordedSliceListNode {
		int32_t command_index = -1;
		int32_t next_list_index = -1;
		Rect2i subresources;
		bool partial_coverage = false;
	};

	struct RecordedBottomLevelAccelerationStructureBuildCommand : RecordedCommand {
		RDD::AccelerationStructureID acceleration_structure;
		RDD::BufferID scratch_buffer;
		// GodotNext: rq_substrate (#22). The build's region of the frame's scratch ring.
		uint64_t scratch_offset = 0;
		// GodotNext: as_build_modes
		// UPDATE refits this structure in place, so playback passes the mode to the driver.
		RDD::AccelerationStructureBuildMode mode = RDD::ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
	};

	struct RecordedTopLevelAccelerationStructureBuildCommand : RecordedCommand {
		RDD::AccelerationStructureID acceleration_structure;
		RDD::BufferID scratch_buffer;
		// GodotNext: rq_substrate (#22). The build's region of the frame's scratch ring.
		uint64_t scratch_offset = 0;
		RDD::BufferID instance_buffer;
		uint32_t instance_offset = 0;
		uint32_t instance_count = 0;
		// GodotNext: as_build_modes
		RDD::AccelerationStructureBuildMode mode = RDD::ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
	};

	struct RecordedBufferClearCommand : RecordedCommand {
		RDD::BufferID buffer;
		uint32_t offset = 0;
		uint32_t size = 0;
	};

	struct RecordedBufferCopyCommand : RecordedCommand {
		RDD::BufferID source;
		RDD::BufferID destination;
		RDD::BufferCopyRegion region;
	};

	struct RecordedBufferGetDataCommand : RecordedCommand {
		RDD::BufferID source;
		RDD::BufferID destination;
		RDD::BufferCopyRegion region;
	};

	struct RecordedBufferUpdateCommand : RecordedCommand {
		RDD::BufferID destination;
		uint32_t buffer_copies_count = 0;

		_FORCE_INLINE_ RecordedBufferCopy *buffer_copies() {
			return reinterpret_cast<RecordedBufferCopy *>(&this[1]);
		}

		_FORCE_INLINE_ const RecordedBufferCopy *buffer_copies() const {
			return reinterpret_cast<const RecordedBufferCopy *>(&this[1]);
		}
	};

	struct RecordedDriverCallbackCommand : RecordedCommand {
		RDD::DriverCallback callback;
		void *userdata = nullptr;
	};

	// GodotNext: blas_compact
	struct RecordedAccelerationStructureQuerySizeCommand : RecordedCommand {
		RDD::AccelerationStructureID acceleration_structure;
		RDD::QueryPoolID pool;
		uint32_t index = 0;
	};

	// GodotNext: blas_compact
	struct RecordedAccelerationStructureCopyCommand : RecordedCommand {
		RDD::AccelerationStructureID source;
		RDD::AccelerationStructureID destination;
	};

	struct RecordedRaytracingListCommand : RecordedCommand {
		uint32_t instruction_data_size = 0;

		_FORCE_INLINE_ uint8_t *instruction_data() {
			return reinterpret_cast<uint8_t *>(&this[1]);
		}

		_FORCE_INLINE_ const uint8_t *instruction_data() const {
			return reinterpret_cast<const uint8_t *>(&this[1]);
		}
	};

	struct RecordedComputeListCommand : RecordedCommand {
		uint32_t instruction_data_size = 0;
		uint32_t breadcrumb = 0;

		_FORCE_INLINE_ uint8_t *instruction_data() {
			return reinterpret_cast<uint8_t *>(&this[1]);
		}

		_FORCE_INLINE_ const uint8_t *instruction_data() const {
			return reinterpret_cast<const uint8_t *>(&this[1]);
		}
	};

	struct RecordedDrawListCommand : RecordedCommand {
		FramebufferCache *framebuffer_cache = nullptr;
		RDD::FramebufferID framebuffer;
		RDD::RenderPassID render_pass;
		uint32_t instruction_data_size = 0;
		RDD::CommandBufferType command_buffer_type;
		Rect2i region;
		uint32_t clear_values_count = 0;
		uint32_t trackers_count = 0;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		uint32_t breadcrumb = 0;
#endif
		bool split_cmd_buffer = false;

		_FORCE_INLINE_ RDD::RenderPassClearValue *clear_values() {
			return reinterpret_cast<RDD::RenderPassClearValue *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::RenderPassClearValue *clear_values() const {
			return reinterpret_cast<const RDD::RenderPassClearValue *>(&this[1]);
		}

		_FORCE_INLINE_ ResourceTracker **trackers() {
			return reinterpret_cast<ResourceTracker **>(&clear_values()[clear_values_count]);
		}

		_FORCE_INLINE_ ResourceTracker *const *trackers() const {
			return reinterpret_cast<ResourceTracker *const *>(&clear_values()[clear_values_count]);
		}

		_FORCE_INLINE_ RDD::AttachmentLoadOp *load_ops() {
			return reinterpret_cast<RDD::AttachmentLoadOp *>(&trackers()[trackers_count]);
		}

		_FORCE_INLINE_ const RDD::AttachmentLoadOp *load_ops() const {
			return reinterpret_cast<const RDD::AttachmentLoadOp *>(&trackers()[trackers_count]);
		}

		_FORCE_INLINE_ RDD::AttachmentStoreOp *store_ops() {
			return reinterpret_cast<RDD::AttachmentStoreOp *>(&load_ops()[trackers_count]);
		}

		_FORCE_INLINE_ const RDD::AttachmentStoreOp *store_ops() const {
			return reinterpret_cast<const RDD::AttachmentStoreOp *>(&load_ops()[trackers_count]);
		}

		_FORCE_INLINE_ uint8_t *instruction_data() {
			return reinterpret_cast<uint8_t *>(&store_ops()[trackers_count]);
		}

		_FORCE_INLINE_ const uint8_t *instruction_data() const {
			return reinterpret_cast<const uint8_t *>(&store_ops()[trackers_count]);
		}

		// GodotNext: rd_indirect_count (#177). The offsets of the indirect-count draws in instruction_data(),
		// after it at the next 8-byte boundary, so drivers can prepare them before the render pass begins.
		uint32_t indirect_count_draws = 0;

		_FORCE_INLINE_ static uint32_t indirect_count_offsets_start(uint32_t p_instruction_data_size) {
			return (p_instruction_data_size + 7u) & ~7u;
		}

		_FORCE_INLINE_ uint32_t *indirect_count_offsets() {
			return reinterpret_cast<uint32_t *>(instruction_data() + indirect_count_offsets_start(instruction_data_size));
		}

		_FORCE_INLINE_ const uint32_t *indirect_count_offsets() const {
			return reinterpret_cast<const uint32_t *>(instruction_data() + indirect_count_offsets_start(instruction_data_size));
		}
	};

	struct RecordedTextureClearColorCommand : RecordedCommand {
		RDD::TextureID texture;
		RDD::TextureSubresourceRange range;
		Color color;
	};

	struct RecordedTextureClearDepthStencilCommand : RecordedCommand {
		RDD::TextureID texture;
		RDD::TextureSubresourceRange range;
		float depth;
		uint8_t stencil;
	};

	struct RecordedTextureCopyCommand : RecordedCommand {
		RDD::TextureID from_texture;
		RDD::TextureID to_texture;
		uint32_t texture_copy_regions_count = 0;

		_FORCE_INLINE_ RDD::TextureCopyRegion *texture_copy_regions() {
			return reinterpret_cast<RDD::TextureCopyRegion *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::TextureCopyRegion *texture_copy_regions() const {
			return reinterpret_cast<const RDD::TextureCopyRegion *>(&this[1]);
		}
	};

	struct RecordedTextureGetDataCommand : RecordedCommand {
		RDD::TextureID from_texture;
		RDD::BufferID to_buffer;
		uint32_t buffer_texture_copy_regions_count = 0;

		_FORCE_INLINE_ RDD::BufferTextureCopyRegion *buffer_texture_copy_regions() {
			return reinterpret_cast<RDD::BufferTextureCopyRegion *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::BufferTextureCopyRegion *buffer_texture_copy_regions() const {
			return reinterpret_cast<const RDD::BufferTextureCopyRegion *>(&this[1]);
		}
	};

	struct RecordedTextureResolveCommand : RecordedCommand {
		RDD::TextureID from_texture;
		RDD::TextureID to_texture;
		uint32_t src_layer = 0;
		uint32_t src_mipmap = 0;
		uint32_t dst_layer = 0;
		uint32_t dst_mipmap = 0;
	};

	struct RecordedTextureUpdateCommand : RecordedCommand {
		RDD::TextureID to_texture;
		uint32_t buffer_to_texture_copies_count = 0;

		_FORCE_INLINE_ RecordedBufferToTextureCopy *buffer_to_texture_copies() {
			return reinterpret_cast<RecordedBufferToTextureCopy *>(&this[1]);
		}

		_FORCE_INLINE_ const RecordedBufferToTextureCopy *buffer_to_texture_copies() const {
			return reinterpret_cast<const RecordedBufferToTextureCopy *>(&this[1]);
		}
	};

	struct RecordedCaptureTimestampCommand : RecordedCommand {
		RDD::QueryPoolID pool;
		uint32_t index = 0;
	};

	struct DrawListBindIndexBufferInstruction : DrawListInstruction {
		RDD::BufferID buffer;
		RenderingDeviceCommons::IndexBufferFormat format;
		uint32_t offset = 0;
	};

	struct DrawListBindPipelineInstruction : DrawListInstruction {
		RDD::PipelineID pipeline;
	};

	struct DrawListBindUniformSetsInstruction : DrawListInstruction {
		RDD::ShaderID shader;
		uint32_t first_set_index = 0;
		uint32_t set_count = 0;
		uint32_t dynamic_offsets_mask = 0u;

		_FORCE_INLINE_ RDD::UniformSetID *uniform_set_ids() {
			return reinterpret_cast<RDD::UniformSetID *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::UniformSetID *uniform_set_ids() const {
			return reinterpret_cast<const RDD::UniformSetID *>(&this[1]);
		}
	};

	struct DrawListBindVertexBuffersInstruction : DrawListInstruction {
		uint32_t vertex_buffers_count = 0;
		uint64_t dynamic_offsets_mask = 0;

		_FORCE_INLINE_ RDD::BufferID *vertex_buffers() {
			return reinterpret_cast<RDD::BufferID *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::BufferID *vertex_buffers() const {
			return reinterpret_cast<const RDD::BufferID *>(&this[1]);
		}

		_FORCE_INLINE_ uint64_t *vertex_buffer_offsets() {
			return reinterpret_cast<uint64_t *>(&vertex_buffers()[vertex_buffers_count]);
		}

		_FORCE_INLINE_ const uint64_t *vertex_buffer_offsets() const {
			return reinterpret_cast<const uint64_t *>(&vertex_buffers()[vertex_buffers_count]);
		}
	};

	struct DrawListClearAttachmentsInstruction : DrawListInstruction {
		uint32_t attachments_clear_count = 0;
		uint32_t attachments_clear_rect_count = 0;

		_FORCE_INLINE_ RDD::AttachmentClear *attachments_clear() {
			return reinterpret_cast<RDD::AttachmentClear *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::AttachmentClear *attachments_clear() const {
			return reinterpret_cast<const RDD::AttachmentClear *>(&this[1]);
		}

		_FORCE_INLINE_ Rect2i *attachments_clear_rect() {
			return reinterpret_cast<Rect2i *>(&attachments_clear()[attachments_clear_count]);
		}

		_FORCE_INLINE_ const Rect2i *attachments_clear_rect() const {
			return reinterpret_cast<const Rect2i *>(&attachments_clear()[attachments_clear_count]);
		}
	};

	struct DrawListDrawInstruction : DrawListInstruction {
		uint32_t vertex_count = 0;
		uint32_t instance_count = 0;
	};

	struct DrawListDrawIndexedInstruction : DrawListInstruction {
		uint32_t index_count = 0;
		uint32_t instance_count = 0;
		uint32_t first_index = 0;
	};

	struct DrawListDrawIndirectInstruction : DrawListInstruction {
		RDD::BufferID buffer;
		uint32_t offset = 0;
		uint32_t draw_count = 0;
		uint32_t stride = 0;
	};

	struct DrawListDrawIndexedIndirectInstruction : DrawListInstruction {
		RDD::BufferID buffer;
		uint32_t offset = 0;
		uint32_t draw_count = 0;
		uint32_t stride = 0;
	};

	// GodotNext: rd_mesh_shaders (#178)
	struct DrawListDrawMeshTasksInstruction : DrawListInstruction {
		uint32_t group_count[3] = {};
	};

	// GodotNext: rd_mesh_shaders (#178). Three uint32 group counts per record.
	struct DrawListDrawMeshTasksIndirectInstruction : DrawListInstruction {
		RDD::BufferID buffer;
		uint32_t offset = 0;
		uint32_t draw_count = 0;
		uint32_t stride = 0;
	};

	// GodotNext: rd_indirect_count (#177). TYPE_DRAW_INDIRECT_COUNT and TYPE_DRAW_INDEXED_INDIRECT_COUNT.
	// GodotNext: rd_mesh_shaders (#178). And TYPE_DRAW_MESH_TASKS_INDIRECT_COUNT.
	struct DrawListDrawIndirectCountInstruction : DrawListInstruction {
		RDD::BufferID buffer;
		RDD::BufferID count_buffer;
		uint32_t offset = 0;
		uint32_t count_buffer_offset = 0;
		uint32_t max_draw_count = 0;
		uint32_t stride = 0;
	};

	struct DrawListEndRenderPassInstruction : DrawListInstruction {
		// No contents.
	};

	struct DrawListExecuteCommandsInstruction : DrawListInstruction {
		RDD::CommandBufferID command_buffer;
	};

	struct DrawListSetPushConstantInstruction : DrawListInstruction {
		uint32_t size = 0;
		RDD::ShaderID shader;

		_FORCE_INLINE_ uint8_t *data() {
			return reinterpret_cast<uint8_t *>(&this[1]);
		}

		_FORCE_INLINE_ const uint8_t *data() const {
			return reinterpret_cast<const uint8_t *>(&this[1]);
		}
	};

	struct DrawListNextSubpassInstruction : DrawListInstruction {
		RDD::CommandBufferType command_buffer_type;
	};

	struct DrawListSetBlendConstantsInstruction : DrawListInstruction {
		Color color;
	};

	struct DrawListSetLineWidthInstruction : DrawListInstruction {
		float width;
	};

	struct DrawListSetScissorInstruction : DrawListInstruction {
		Rect2i rect;
	};

	struct DrawListSetViewportInstruction : DrawListInstruction {
		Rect2i rect;
	};

	struct DrawListUniformSetPrepareForUseInstruction : DrawListInstruction {
		RDD::UniformSetID uniform_set;
		RDD::ShaderID shader;
		uint32_t set_index = 0;
	};

	struct RaytracingListBuildAccelerationStructureInstruction : RaytracingListInstruction {
		RDD::AccelerationStructureID acceleration_structure;
		RDD::AccelerationStructureType acceleration_structure_type;
	};

	struct RaytracingListBindPipelineInstruction : RaytracingListInstruction {
		RDD::RaytracingPipelineID pipeline;
	};

	struct RaytracingListBindUniformSetInstruction : RaytracingListInstruction {
		RDD::UniformSetID uniform_set;
		RDD::ShaderID shader;
		uint32_t set_index = 0;
	};

	struct RaytracingListSetPushConstantInstruction : RaytracingListInstruction {
		uint32_t size = 0;
		RDD::ShaderID shader;

		_FORCE_INLINE_ uint8_t *data() {
			return reinterpret_cast<uint8_t *>(&this[1]);
		}

		_FORCE_INLINE_ const uint8_t *data() const {
			return reinterpret_cast<const uint8_t *>(&this[1]);
		}
	};

	struct RaytracingListTraceRaysInstruction : RaytracingListInstruction {
		RDD::ShaderBindingTable raygen_sbt;
		RDD::ShaderBindingTable miss_sbt;
		RDD::ShaderBindingTable hit_sbt;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depth = 0;
	};

	struct RaytracingListUniformSetPrepareForUseInstruction : RaytracingListInstruction {
		RDD::UniformSetID uniform_set;
		RDD::ShaderID shader;
		uint32_t set_index = 0;
	};

	// GodotNext: rd_rt_pipelines (#179)
	struct RaytracingListTraceRaysIndirectInstruction : RaytracingListInstruction {
		RDD::ShaderBindingTable raygen_sbt;
		RDD::ShaderBindingTable miss_sbt;
		RDD::ShaderBindingTable hit_sbt;
		RDD::BufferID buffer;
		uint64_t offset = 0;
	};

	struct ComputeListBindPipelineInstruction : ComputeListInstruction {
		RDD::PipelineID pipeline;
	};

	struct ComputeListBindUniformSetsInstruction : ComputeListInstruction {
		RDD::ShaderID shader;
		uint32_t first_set_index = 0;
		uint32_t set_count = 0;
		uint32_t dynamic_offsets_mask = 0u;

		_FORCE_INLINE_ RDD::UniformSetID *uniform_set_ids() {
			return reinterpret_cast<RDD::UniformSetID *>(&this[1]);
		}

		_FORCE_INLINE_ const RDD::UniformSetID *uniform_set_ids() const {
			return reinterpret_cast<const RDD::UniformSetID *>(&this[1]);
		}
	};

	struct ComputeListDispatchInstruction : ComputeListInstruction {
		uint32_t x_groups = 0;
		uint32_t y_groups = 0;
		uint32_t z_groups = 0;
	};

	struct ComputeListDispatchIndirectInstruction : ComputeListInstruction {
		RDD::BufferID buffer;
		uint32_t offset = 0;
	};

	struct ComputeListSetPushConstantInstruction : ComputeListInstruction {
		uint32_t size = 0;
		RDD::ShaderID shader;

		_FORCE_INLINE_ uint8_t *data() {
			return reinterpret_cast<uint8_t *>(&this[1]);
		}

		_FORCE_INLINE_ const uint8_t *data() const {
			return reinterpret_cast<const uint8_t *>(&this[1]);
		}
	};

	struct ComputeListUniformSetPrepareForUseInstruction : ComputeListInstruction {
		RDD::UniformSetID uniform_set;
		RDD::ShaderID shader;
		uint32_t set_index = 0;
	};

	struct BarrierGroup {
		BitField<RDD::PipelineStageBits> src_stages = {};
		BitField<RDD::PipelineStageBits> dst_stages = {};
		RDD::MemoryAccessBarrier memory_barrier;
		LocalVector<RDD::TextureBarrier> normalization_barriers;
		LocalVector<RDD::TextureBarrier> transition_barriers;
#if USE_BUFFER_BARRIERS
		LocalVector<RDD::BufferBarrier> buffer_barriers;
#endif
		LocalVector<RDD::AccelerationStructureBarrier> acceleration_structure_barriers;

		void clear() {
			src_stages.clear();
			dst_stages.clear();
			memory_barrier.src_access.clear();
			memory_barrier.dst_access.clear();
			normalization_barriers.clear();
			transition_barriers.clear();
#if USE_BUFFER_BARRIERS
			buffer_barriers.clear();
#endif
			acceleration_structure_barriers.clear();
		}
	};

	struct SecondaryCommandBuffer {
		LocalVector<uint8_t> instruction_data;
		RDD::CommandBufferID command_buffer;
		RDD::CommandPoolID command_pool;
		RDD::RenderPassID render_pass;
		RDD::FramebufferID framebuffer;
		WorkerThreadPool::TaskID task;
	};

	struct Frame {
		TightLocalVector<SecondaryCommandBuffer> secondary_command_buffers;
		uint32_t secondary_command_buffers_used = 0;
	};

	RDD *driver = nullptr;
	RDD::DriverWorkarounds driver_workarounds;
	RenderPassCreationFunction render_pass_creation_function = nullptr;
	int64_t tracking_frame = 0;
	LocalVector<uint8_t> command_data;
	LocalVector<uint32_t> command_data_offsets;
	LocalVector<RDD::TextureBarrier> command_normalization_barriers;
	LocalVector<RDD::TextureBarrier> command_transition_barriers;
	LocalVector<RDD::BufferBarrier> command_buffer_barriers;
	LocalVector<RDD::AccelerationStructureBarrier> command_acceleration_structure_barriers;
	// GodotNext: rd_readonly_depth (#301). Depth/stencil texture transitions into the shared
	// read-only layout (TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) that serve concurrent
	// readers across queues without creating a reader-to-reader dependency.
	struct SharedReadLayoutTransition {
		RDD::TextureBarrier barrier;
		BitField<RDD::PipelineStageBits> src_stages = {};
		BitField<RDD::PipelineStageBits> dst_stages = {};
		LocalVector<int32_t> reader_commands;
		int32_t emit_level = -1;
		bool emit_at_fork = false;
	};
	LocalVector<SharedReadLayoutTransition> shared_read_transitions;
	LocalVector<char> command_label_chars;
	LocalVector<Color> command_label_colors;
	LocalVector<uint32_t> command_label_offsets;
	int32_t command_label_index = -1;
	DrawInstructionList draw_instruction_list;
	ComputeInstructionList compute_instruction_list;
	RaytracingInstructionList raytracing_instruction_list;
	uint32_t command_count = 0;
	// GodotNext: async_compute (#174). Compute lists recorded for the async queue this frame.
	uint32_t async_command_count = 0;
	// GodotNext: async_compute (#174). An async command has a BARRIER_SELECTION_ASYNC_FORK barrier on
	// a resource an earlier async command of the frame already used, so its source access may be on
	// the async queue and it cannot move before the fork. The frame then runs on the main queue.
	bool async_fork_barrier_conflict = false;
	uint32_t command_label_count = 0;
	LocalVector<RecordedCommandListNode> command_list_nodes;
	LocalVector<RecordedSliceListNode> read_slice_list_nodes;
	LocalVector<RecordedSliceListNode> write_slice_list_nodes;
	// GodotNext: px_rd_async_multiview (#300). Per-frame async slice uses for slice-aware fork barriers.
	LocalVector<RecordedSliceListNode> async_slice_list_nodes;
	int32_t command_timestamp_index = -1;
	int32_t command_synchronization_index = -1;
	bool command_synchronization_pending = false;
	BarrierGroup barrier_group;
	bool driver_honors_barriers : 1;
	bool driver_buffer_clears_with_copy_engine : 1;
	bool driver_texture_clears_with_copy_engine : 1;
	bool driver_buffers_require_transitions : 1;
	bool driver_textures_require_layout_transitions : 1;
	WorkaroundsState workarounds_state;
	TightLocalVector<Frame> frames;
	uint32_t frame = 0;

#ifdef DEV_ENABLED
	RBMap<ResourceTracker *, uint32_t> write_dependency_counters;
#endif

	static String _usage_to_string(ResourceUsage p_usage);
	// GodotNext: rd_mesh_shaders (#178). The indirect-count draw recording that #177's draws and the mesh task draws
	// share, and its check that the current draw list uses p_tracker for nothing else (p_draw names the draw in the
	// error).
	bool _draw_list_indirect_buffer_usable(ResourceTracker *p_tracker, const char *p_role, const char *p_draw);
	bool _add_draw_list_draw_indirect_count(DrawListInstruction::Type p_type, RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, RDD::BufferID p_count_buffer, ResourceTracker *p_count_buffer_tracker, uint32_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride);
	static bool _is_write_usage(ResourceUsage p_usage);
	// GodotNext: rd_readonly_depth (#301).
	static bool _is_depth_stencil_texture(const ResourceTracker *p_tracker);
	static RDD::TextureLayout _usage_to_image_layout(ResourceUsage p_usage, bool p_is_depth_stencil = false);
	static RDD::BarrierAccessBits _usage_to_access_bits(ResourceUsage p_usage);
	bool _check_command_intersection(ResourceTracker *p_resource_tracker, int32_t p_previous_command_index, int32_t p_command_index) const;
	bool _check_command_partial_coverage(ResourceTracker *p_resource_tracker, int32_t p_command_index) const;
	int32_t _add_to_command_list(int32_t p_command_index, int32_t p_list_index);
	void _add_adjacent_command(int32_t p_previous_command_index, int32_t p_command_index, RecordedCommand *r_command);
	int32_t _add_to_slice_read_list(int32_t p_command_index, Rect2i p_subresources, int32_t p_list_index);
	int32_t _add_to_write_list(int32_t p_command_index, Rect2i p_subresources, int32_t p_list_index, bool p_partial_coverage);
	int32_t _add_to_async_slice_list(int32_t p_command_index, Rect2i p_subresources, int32_t p_list_index);
	RecordedCommand *_allocate_command(uint32_t p_command_size, int32_t &r_command_index);
	DrawListInstruction *_allocate_draw_list_instruction(uint32_t p_instruction_size);
	ComputeListInstruction *_allocate_compute_list_instruction(uint32_t p_instruction_size);
	void _check_discardable_attachment_dependency(ResourceTracker *p_resource_tracker, int32_t p_previous_command_index, int32_t p_command_index);
	RaytracingListInstruction *_allocate_raytracing_list_instruction(uint32_t p_instruction_size);
	void _add_command_to_graph(ResourceTracker **p_resource_trackers, ResourceUsage *p_resource_usages, uint32_t p_resource_count, int32_t p_command_index, RecordedCommand *r_command);
	void _add_texture_barrier_to_command(RDD::TextureID p_texture_id, BitField<RDD::BarrierAccessBits> p_src_access, BitField<RDD::BarrierAccessBits> p_dst_access, ResourceUsage p_prev_usage, ResourceUsage p_next_usage, RDD::TextureSubresourceRange p_subresources, LocalVector<RDD::TextureBarrier> &r_barrier_vector, int32_t &r_barrier_index, int32_t &r_barrier_count);
#if USE_BUFFER_BARRIERS
	void _add_buffer_barrier_to_command(RDD::BufferID p_buffer_id, BitField<RDD::BarrierAccessBits> p_src_access, BitField<RDD::BarrierAccessBits> p_dst_access, int32_t &r_barrier_index, int32_t &r_barrier_count);
#endif
	void _add_acceleration_structure_barrier_to_command(RDD::AccelerationStructureID p_acceleration_structure_id, BitField<RDD::BarrierAccessBits> p_src_access, BitField<RDD::BarrierAccessBits> p_dst_access, LocalVector<RDD::AccelerationStructureBarrier> &r_barrier_vector, int32_t &r_barrier_index, int32_t &r_barrier_count);
	void _run_compute_list_command(RDD::CommandBufferID p_command_buffer, const uint8_t *p_instruction_data, uint32_t p_instruction_data_size);
	void _get_draw_list_render_pass_and_framebuffer(const RecordedDrawListCommand *p_draw_list_command, RDD::RenderPassID &r_render_pass, RDD::FramebufferID &r_framebuffer);
	void _run_raytracing_list_command(RDD::CommandBufferID p_command_buffer, const uint8_t *p_instruction_data, uint32_t p_instruction_data_size);
	void _run_draw_list_command(RDD::CommandBufferID p_command_buffer, const uint8_t *p_instruction_data, uint32_t p_instruction_data_size);
	void _add_draw_list_begin(FramebufferCache *p_framebuffer_cache, RDD::RenderPassID p_render_pass, RDD::FramebufferID p_framebuffer, Rect2i p_region, VectorView<AttachmentOperation> p_attachment_operations, VectorView<RDD::RenderPassClearValue> p_attachment_clear_values, BitField<RDD::PipelineStageBits> p_stages, uint32_t p_breadcrumb, bool p_split_cmd_buffer);
	void _run_secondary_command_buffer_task(const SecondaryCommandBuffer *p_secondary);
	void _wait_for_secondary_command_buffer_tasks();
	// GodotNext: async_compute (#174). p_async records into the async compute command buffer. The
	// helpers after it are in rendering_device_graph_async.cpp, except _compute_command_levels().
	void _run_render_commands(int32_t p_level, const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level, bool p_async = false);
	void _compute_command_levels(LocalVector<RecordedCommandSort> &r_commands_sorted) const;
	AsyncPlan _plan_async_partition(LocalVector<RecordedCommandSort> &r_commands_sorted) const;
	AsyncPlan _plan_async_frame(LocalVector<RecordedCommandSort> &r_commands_sorted, bool p_async_queue_available) const;
	void _split_main_command_buffer(RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level);
	void _run_level(uint32_t p_level, RecordedCommandSort *p_level_commands, uint32_t p_level_command_count, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level, uint32_t &r_boosted_priority);
	template <typename T>
	static void _walk_async_frame(RecordedCommandSort *p_commands_sorted, uint32_t p_command_count, const AsyncPlan &p_plan, CommandBufferPool &r_command_buffer_pool, T &r_emitter);
	void _run_async_frame(RecordedCommandSort *p_commands_sorted, const AsyncPlan &p_plan, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level, uint32_t &r_boosted_priority);
	void _run_label_command_change(RDD::CommandBufferID p_command_buffer, int32_t p_new_label_index, int32_t p_new_level, bool p_ignore_previous_value, bool p_use_label_for_empty, const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, int32_t &r_current_label_index, int32_t &r_current_label_level);
	void _boost_priority_for_render_commands(RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, uint32_t &r_boosted_priority);
	// GodotNext: async_compute (#174). p_selection picks the barriers for the async queue or the fork;
	// _gather_barriers_for_render_commands() fills barrier_group and returns false when it is empty.
	void _group_barriers_for_render_commands(RDD::CommandBufferID p_command_buffer, const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, bool p_full_memory_barrier, BarrierSelection p_selection = BARRIER_SELECTION_ALL);
	bool _gather_barriers_for_render_commands(const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, bool p_full_memory_barrier, BarrierSelection p_selection);
	bool _async_subresources_used_by_earlier_command(const ResourceTracker *p_search_tracker, int32_t p_command_index, const Rect2i &p_subresources) const;
	bool _async_fork_texture_barriers_conflict(const ResourceTracker *p_search_tracker, int32_t p_command_index, const RecordedCommand *p_command, int32_t p_normalization_start, int32_t p_transition_start) const;
	// GodotNext: rd_readonly_depth (#301). Places each shared read-only depth transition at the
	// fork (main queue, before fork_level) or at the earliest main-queue reader's level.
	void _resolve_shared_read_transitions(const LocalVector<RecordedCommandSort> &p_commands_sorted, const AsyncPlan &p_plan);
	// GodotNext: rd_bindless (#176).
	typedef void (RenderingDeviceGraph::*ListUsageFunction)(ResourceTracker *p_tracker, ResourceUsage p_usage);
	static BindlessResidency *_bindless_find_residency(ResourceTracker *p_tracker, BindlessTableTracker *p_table);
	void _bindless_table_reset_if_outdated(BindlessTableTracker *p_table) const;
	void _bindless_resource_used(ResourceTracker *p_resident, ResourceUsage p_usage, int32_t p_command_index, RecordedCommand *r_command);
	void _bindless_tables_add_usages(InstructionList &r_list, ListUsageFunction p_add_usage);
	void _bindless_tables_bound(InstructionList &r_list, int32_t p_command_index, RecordedCommand *r_command);
	// GodotNext: rd_indirect_count (#177), rd_readonly_depth (#301).
	static bool _bindless_dirty_in_list_tables(const InstructionList &p_list, const ResourceTracker *p_tracker);
	void _bindless_draw_list_tables_add_usages(int32_t p_command_index, RecordedCommand *r_command);
	// GodotNext: rd_rt_pipelines (#179). The same for the raytracing list and its indirect traces.
	void _bindless_raytracing_list_tables_add_usages(int32_t p_command_index);
	void _print_render_commands(const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count);
	void _print_draw_list(const uint8_t *p_instruction_data, uint32_t p_instruction_data_size);
	void _print_compute_list(const uint8_t *p_instruction_data, uint32_t p_instruction_data_size);
	void _print_raytracing_list(const uint8_t *p_instruction_data, uint32_t p_instruction_data_size);

public:
	RenderingDeviceGraph();
	~RenderingDeviceGraph();
	void initialize(RDD *p_driver, RenderPassCreationFunction p_render_pass_creation_function, uint32_t p_frame_count, RDD::CommandQueueFamilyID p_secondary_command_queue_family, uint32_t p_secondary_command_buffers_per_frame);
	void finalize();
	void begin();
	// GodotNext: pu_core_rd_callback_stages (#101). C++-only read of the stages a recorded command
	// declared and the stages it inherited from its neighbors. Add-time data, no driver call.
	void command_get_stages(int32_t p_command_index, BitField<RDD::PipelineStageBits> &r_self, BitField<RDD::PipelineStageBits> &r_previous, BitField<RDD::PipelineStageBits> &r_next) const;
	// GodotNext: as_build_modes
	void add_blas_build(RDD::AccelerationStructureID p_blas, RDD::BufferID p_scratch_buffer, uint64_t p_scratch_offset, ResourceTracker *p_dst_tracker, VectorView<ResourceTracker *> p_src_trackers, RDD::AccelerationStructureBuildMode p_mode);
	// GodotNext: tlas_from_buffer — p_instance_tracker is the GPU-written instance buffer, null on the CPU path.
	void add_tlas_build(RDD::AccelerationStructureID p_tlas, RDD::BufferID p_scratch_buffer, uint64_t p_scratch_offset, RDD::BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count, ResourceTracker *p_dst_tracker, VectorView<ResourceTracker *> p_src_trackers, RDD::AccelerationStructureBuildMode p_mode, ResourceTracker *p_instance_tracker = nullptr);
	// GodotNext: blas_compact
	void add_acceleration_structure_query_size(RDD::AccelerationStructureID p_acceleration_structure, RDD::QueryPoolID p_pool, uint32_t p_index, ResourceTracker *p_tracker);
	void add_acceleration_structure_copy(RDD::AccelerationStructureID p_source, ResourceTracker *p_source_tracker, RDD::AccelerationStructureID p_destination, ResourceTracker *p_destination_tracker);
	void add_buffer_clear(RDD::BufferID p_dst, ResourceTracker *p_dst_tracker, uint32_t p_offset, uint32_t p_size);
	void add_buffer_copy(RDD::BufferID p_src, ResourceTracker *p_src_tracker, RDD::BufferID p_dst, ResourceTracker *p_dst_tracker, RDD::BufferCopyRegion p_region);
	void add_buffer_get_data(RDD::BufferID p_src, ResourceTracker *p_src_tracker, RDD::BufferID p_dst, RDD::BufferCopyRegion p_region);
	void add_buffer_update(RDD::BufferID p_dst, ResourceTracker *p_dst_tracker, VectorView<RecordedBufferCopy> p_buffer_copies);
	void add_driver_callback(RDD::DriverCallback p_callback, void *p_userdata, VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages, BitField<RDD::PipelineStageBits> p_stages);
	void add_raytracing_list_begin();
	void add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID p_pipeline);
	void add_raytracing_list_bind_uniform_set(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index);
	void add_raytracing_list_set_push_constant(RDD::ShaderID p_shader, const void *p_data, uint32_t p_data_size);
	void add_raytracing_list_trace_rays(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth);
	// GodotNext: rd_rt_pipelines (#179). The list also runs at the indirect-command stage, which reads
	// the launch size, and gets the buffer's RESOURCE_USAGE_INDIRECT_BUFFER_READ usage. Like #177's count
	// draws, it records nothing and returns false, with one error, when the list already uses the buffer
	// another way or a bindless table the list binds has not read it since it was last written.
	bool add_raytracing_list_trace_rays_indirect(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint64_t p_offset);
	void add_raytracing_list_uniform_set_prepare_for_use(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index);
	void add_raytracing_list_usage(ResourceTracker *p_tracker, ResourceUsage p_usage);
	void add_raytracing_list_usages(VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages);
	void add_raytracing_list_end();
	// GodotNext: async_compute (#174). p_queue tags the list for the async compute queue.
	void add_compute_list_begin(RDD::BreadcrumbMarker p_phase = RDD::BreadcrumbMarker::NONE, uint32_t p_breadcrumb_data = 0, Queue p_queue = QUEUE_MAIN);
	void add_compute_list_bind_pipeline(RDD::PipelineID p_pipeline);
	void add_compute_list_bind_uniform_set(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index);
	void add_compute_list_bind_uniform_sets(RDD::ShaderID p_shader, VectorView<RDD::UniformSetID> p_uniform_set, uint32_t p_first_set_index, uint32_t p_set_count);
	void add_compute_list_dispatch(uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups);
	void add_compute_list_dispatch_indirect(RDD::BufferID p_buffer, uint32_t p_offset);
	void add_compute_list_set_push_constant(RDD::ShaderID p_shader, const void *p_data, uint32_t p_data_size);
	void add_compute_list_uniform_set_prepare_for_use(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index);
	void add_compute_list_usage(ResourceTracker *p_tracker, ResourceUsage p_usage);
	void add_compute_list_usages(VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages);
	void add_compute_list_end();
	void add_draw_list_begin(FramebufferCache *p_framebuffer_cache, Rect2i p_region, VectorView<AttachmentOperation> p_attachment_operations, VectorView<RDD::RenderPassClearValue> p_attachment_clear_values, BitField<RDD::PipelineStageBits> p_stages, uint32_t p_breadcrumb = 0, bool p_split_cmd_buffer = false);
	void add_draw_list_begin(RDD::RenderPassID p_render_pass, RDD::FramebufferID p_framebuffer, Rect2i p_region, VectorView<AttachmentOperation> p_attachment_operations, VectorView<RDD::RenderPassClearValue> p_attachment_clear_values, BitField<RDD::PipelineStageBits> p_stages, uint32_t p_breadcrumb = 0, bool p_split_cmd_buffer = false);
	void add_draw_list_bind_index_buffer(RDD::BufferID p_buffer, RDD::IndexBufferFormat p_format, uint32_t p_offset);
	void add_draw_list_bind_pipeline(RDD::PipelineID p_pipeline, BitField<RDD::PipelineStageBits> p_pipeline_stage_bits);
	void add_draw_list_bind_uniform_set(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index);
	void add_draw_list_bind_uniform_sets(RDD::ShaderID p_shader, VectorView<RDD::UniformSetID> p_uniform_set, uint32_t p_first_index, uint32_t p_set_count);
	void add_draw_list_bind_vertex_buffers(Span<RDD::BufferID> p_vertex_buffers, Span<uint64_t> p_vertex_buffer_offsets);
	void add_draw_list_clear_attachments(VectorView<RDD::AttachmentClear> p_attachments_clear, VectorView<Rect2i> p_attachments_clear_rect);
	void add_draw_list_draw(uint32_t p_vertex_count, uint32_t p_instance_count);
	void add_draw_list_draw_indexed(uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index);
	void add_draw_list_draw_indirect(RDD::BufferID p_buffer, uint32_t p_offset, uint32_t p_draw_count, uint32_t p_stride);
	void add_draw_list_draw_indexed_indirect(RDD::BufferID p_buffer, uint32_t p_offset, uint32_t p_draw_count, uint32_t p_stride);
	// GodotNext: rd_indirect_count (#177). Also records the indirect read of both buffers (trackers may be null).
	// Fails with an error, and records nothing, when this draw list already uses either buffer another way, or
	// binds a bindless table that has not read either buffer since it was last written.
	bool add_draw_list_draw_indirect_count(bool p_indexed, RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, RDD::BufferID p_count_buffer, ResourceTracker *p_count_buffer_tracker, uint32_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride);
	// GodotNext: rd_mesh_shaders (#178). The indirect variants also record the indirect read of their buffers
	// (trackers may be null) and fail like add_draw_list_draw_indirect_count() when this draw list already uses one
	// of them another way. The count variant goes through the same driver preparation as the count draws.
	void add_draw_list_draw_mesh_tasks(uint32_t p_group_count_x, uint32_t p_group_count_y, uint32_t p_group_count_z);
	bool add_draw_list_draw_mesh_tasks_indirect(RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, uint32_t p_draw_count, uint32_t p_stride);
	bool add_draw_list_draw_mesh_tasks_indirect_count(RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, RDD::BufferID p_count_buffer, ResourceTracker *p_count_buffer_tracker, uint32_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride);
	void add_draw_list_execute_commands(RDD::CommandBufferID p_command_buffer);
	void add_draw_list_next_subpass(RDD::CommandBufferType p_command_buffer_type);
	void add_draw_list_set_blend_constants(const Color &p_color);
	void add_draw_list_set_line_width(float p_width);
	void add_draw_list_set_push_constant(RDD::ShaderID p_shader, const void *p_data, uint32_t p_data_size);
	void add_draw_list_set_scissor(Rect2i p_rect);
	void add_draw_list_set_viewport(Rect2i p_rect);
	void add_draw_list_uniform_set_prepare_for_use(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index);
	void add_draw_list_usage(ResourceTracker *p_tracker, ResourceUsage p_usage);
	void add_draw_list_usages(VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages);
	void add_draw_list_end();
	void add_texture_clear_color(RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, const Color &p_color, const RDD::TextureSubresourceRange &p_range);
	void add_texture_clear_depth_stencil(RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, float p_depth, uint8_t p_stencil, const RDD::TextureSubresourceRange &p_range);
	void add_texture_copy(RDD::TextureID p_src, ResourceTracker *p_src_tracker, RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, VectorView<RDD::TextureCopyRegion> p_texture_copy_regions);
	void add_texture_get_data(RDD::TextureID p_src, ResourceTracker *p_src_tracker, RDD::BufferID p_dst, VectorView<RDD::BufferTextureCopyRegion> p_buffer_texture_copy_regions, ResourceTracker *p_dst_tracker = nullptr);
	void add_texture_resolve(RDD::TextureID p_src, ResourceTracker *p_src_tracker, RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, uint32_t p_src_layer, uint32_t p_src_mipmap, uint32_t p_dst_layer, uint32_t p_dst_mipmap);
	void add_texture_update(RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, VectorView<RecordedBufferToTextureCopy> p_buffer_copies, VectorView<ResourceTracker *> p_buffer_trackers = VectorView<ResourceTracker *>());
	void add_capture_timestamp(RDD::QueryPoolID p_query_pool, uint32_t p_index);
	void add_synchronization();
	void begin_label(const Span<char> &p_label_name, const Color &p_color);
	void end_label();
	void end(bool p_reorder_commands, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool);
	// GodotNext: async_compute (#174). C++-only and driver-free, for the doctests.
	// plan_for_test(): the reordered command order and the async plan end() would use for the
	// commands recorded so far. p_async_queue_available is false when the device has no async
	// queue, which makes end() fall back to the main queue.
	void plan_for_test(LocalVector<int32_t> &r_order, AsyncPlan &r_plan, bool p_async_queue_available = true) const;
	// emit_for_test(): plans as end() does with an async command buffer and, when the plan is active,
	// walks the frame the way end() records it, from an empty command buffer pool. The main commands
	// of p_split_main_level begin a new command buffer first, as a draw list with split_cmd_buffer
	// (the swap chain pass) does.
	void emit_for_test(AsyncPlan &r_plan, AsyncEmission &r_emission, uint32_t p_split_main_level = UINT32_MAX) const;
	// set_driver_traits_for_test(): the driver traits initialize() reads, so a driverless graph records
	// barriers. async_barriers_for_test(): plans as end() does with an async command buffer and, when
	// the plan is active, gathers p_selection's barriers from every async command of the frame.
	void set_driver_traits_for_test(bool p_honors_barriers, bool p_textures_require_layout_transitions, bool p_buffers_require_transitions = false);
	// GodotNext: rd_indirect_count (#177). The indirect-count draws of recorded command p_command_index (a draw
	// list), as the driver is asked to prepare them before its render pass begins.
	struct IndirectCountDraw {
		bool indexed = false;
		bool mesh_tasks = false; // GodotNext: rd_mesh_shaders (#178)
		RDD::BufferID buffer;
		RDD::BufferID count_buffer;
		uint32_t offset = 0;
		uint32_t count_buffer_offset = 0;
		uint32_t max_draw_count = 0;
		uint32_t stride = 0;
	};
	LocalVector<IndirectCountDraw> indirect_count_draws_for_test(int32_t p_command_index) const;
	void async_barriers_for_test(BarrierSelection p_selection, AsyncPlan &r_plan, AsyncBarriers &r_barriers);
	// GodotNext: rd_readonly_depth (#301). C++-only and driver-free, for the doctests: barriers
	// emitted on the main queue at p_level (after async planning) and a draw list's store ops.
	void main_level_barriers_for_test(uint32_t p_level, bool p_async_queue_available, AsyncPlan &r_plan, AsyncBarriers &r_barriers);
	void draw_list_store_ops_for_test(int32_t p_command_index, LocalVector<RDD::AttachmentStoreOp> &r_store_ops) const;
	// GodotNext: rd_clear_values (#275). C++-only and driver-free, for the doctests: what a recorded
	// draw list passes to the driver, the load ops of its framebuffer slots and its clear values.
	void draw_list_attachments_for_test(int32_t p_command_index, LocalVector<RDD::AttachmentLoadOp> &r_load_ops, LocalVector<RDD::RenderPassClearValue> &r_clear_values) const;
	// submit_chained_command_buffers(): the frame's queue submissions in order. The fork command
	// buffer signals the async work, which runs on the async queue, and the join command buffer
	// waits for it: one wait on each side of the async queue.
	static void submit_chained_command_buffers(const ChainedSubmission &p_submission, ChainedSubmitFunction p_submit, void *p_userdata);
	// GodotNext: rd_bindless (#176). C++-only and driver-free, for the doctests: the barriers recorded for
	// one command.
	void command_barriers_for_test(int32_t p_command_index, LocalVector<RDD::TextureBarrier> &r_texture_barriers, LocalVector<RDD::BufferBarrier> &r_buffer_barriers, RDD::MemoryAccessBarrier &r_memory_barrier) const;
	// GodotNext: rd_bindless (#176). Bindless tables (see BindlessTableTracker). A resource that enters a
	// table is dirty, as the table has not read it in its current state. add_*_list_bindless_table()
	// records that the list binds the table; the list's end adds the dirty resources as its usages.
	static BindlessTableTracker *bindless_table_tracker_create(ResourceUsage p_read_usage);
	static void bindless_table_tracker_free(BindlessTableTracker *p_table);
	static void bindless_table_add_resident(BindlessTableTracker *p_table, ResourceTracker *p_tracker);
	static void bindless_table_remove_resident(BindlessTableTracker *p_table, ResourceTracker *p_tracker);
	void add_compute_list_bindless_table(BindlessTableTracker *p_table);
	void add_draw_list_bindless_table(BindlessTableTracker *p_table);
	void add_raytracing_list_bindless_table(BindlessTableTracker *p_table);
	static ResourceTracker *resource_tracker_create();
	static void resource_tracker_free(ResourceTracker *p_tracker);
	static FramebufferCache *framebuffer_cache_create();
	static void framebuffer_cache_free(RDD *p_driver, FramebufferCache *p_cache);
};

using RDG = RenderingDeviceGraph;
