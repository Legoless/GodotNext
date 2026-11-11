/**************************************************************************/
/*  rendering_device_graph_async.cpp                                      */
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

// GodotNext async compute queue (#174): how RenderingDeviceGraph splits a frame between the main
// and the async compute queue, and the order RenderingDevice submits the result in. end() and
// RenderingDevice::execute_chained_cmds() call these; the doctests in
// tests/servers/rendering/test_rd_async_compute.cpp drive the same code without a driver.

#include "servers/rendering/rendering_device_graph.h"

// Decides whether this frame's async compute lists can run on the async queue with one fork and
// one join. Main commands that feed async commands must all run before fork_level, and main
// commands that consume async results must all run from join_level on. When fork_level <
// join_level both hold, because the main commands in between have no edge to or from an async
// command.
RenderingDeviceGraph::AsyncPlan RenderingDeviceGraph::_plan_async_partition(LocalVector<RecordedCommandSort> &r_commands_sorted) const {
	AsyncPlan plan;
	uint32_t max_level = 0;
	for (const RecordedCommandSort &command_sort : r_commands_sorted) {
		max_level = MAX(max_level, command_sort.level);
	}
	plan.level_count = r_commands_sorted.is_empty() ? 0 : max_level + 1;

	int64_t max_producer_level = -1;
	uint32_t min_consumer_level = plan.level_count;
	for (uint32_t i = 0; i < command_count; i++) {
		const RecordedCommand &recorded_command = *reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offsets[i]]);
		if (recorded_command.queue == QUEUE_ASYNC_COMPUTE) {
			plan.async_count++;
		} else {
			plan.main_count++;
		}

		int32_t adjacency_list_index = recorded_command.adjacent_command_list_index;
		while (adjacency_list_index >= 0) {
			const RecordedCommandListNode &command_list_node = command_list_nodes[adjacency_list_index];
			const RecordedCommand &next_command = *reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offsets[command_list_node.command_index]]);
			if (recorded_command.queue == QUEUE_MAIN && next_command.queue == QUEUE_ASYNC_COMPUTE) {
				max_producer_level = MAX(max_producer_level, int64_t(r_commands_sorted[i].level));
			} else if (recorded_command.queue == QUEUE_ASYNC_COMPUTE && next_command.queue == QUEUE_MAIN) {
				min_consumer_level = MIN(min_consumer_level, r_commands_sorted[command_list_node.command_index].level);
			}
			adjacency_list_index = command_list_node.next_list_index;
		}
	}

	plan.fork_level = uint32_t(max_producer_level + 1);
	plan.join_level = min_consumer_level;
	plan.active = plan.async_count > 0 && plan.fork_level < plan.join_level;
	return plan;
}

// The plan end() uses. A frame without async lists has nothing to plan. A frame whose async lists
// cannot run on the async queue (no async command buffer, or a producer and a consumer that
// overlap) puts every command back on the main queue, which gives exactly the order end() used
// before async compute existed. level_count is filled only when the frame recorded async lists.
RenderingDeviceGraph::AsyncPlan RenderingDeviceGraph::_plan_async_frame(LocalVector<RecordedCommandSort> &r_commands_sorted, bool p_async_queue_available) const {
	if (async_command_count == 0) {
		// Every queue key is already QUEUE_MAIN.
		AsyncPlan plan;
		plan.main_count = command_count;
		return plan;
	}

	AsyncPlan plan = _plan_async_partition(r_commands_sorted);
	plan.active = plan.active && p_async_queue_available;
	if (!plan.active) {
		for (RecordedCommandSort &command_sort : r_commands_sorted) {
			command_sort.queue = QUEUE_MAIN;
		}
	}
	return plan;
}

// Ends the main command buffer and continues on the next one from the pool, so RenderingDevice can
// submit the async work between the two. While end() records with labels, exactly one label is
// open on the main command buffer; it is closed here and a generic one reopened, so every command
// buffer balances its debug groups.
void RenderingDeviceGraph::_split_main_command_buffer(RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level) {
	if (command_label_count > 0) {
		driver->command_end_label(r_command_buffer);
	}
	driver->command_buffer_end(r_command_buffer);
	// GodotNext: metal_level_fences (#186). The next main command buffer continues this one.
	const RDD::CommandBufferID continued_command_buffer = r_command_buffer;

	while (r_command_buffer_pool.buffers_used >= r_command_buffer_pool.buffers.size()) {
		RDD::CommandBufferID command_buffer = driver->command_buffer_create(r_command_buffer_pool.pool);
		RDD::SemaphoreID command_semaphore = driver->semaphore_create();
		r_command_buffer_pool.buffers.push_back(command_buffer);
		r_command_buffer_pool.semaphores.push_back(command_semaphore);
	}

	uint32_t command_buffer_index = r_command_buffer_pool.buffers_used++;
	r_command_buffer = r_command_buffer_pool.buffers[command_buffer_index];
	driver->command_buffer_begin(r_command_buffer);
	driver->command_buffer_continue(r_command_buffer, continued_command_buffer);
	r_current_label_index = -1;
	r_current_label_level = -1;
	_run_label_command_change(r_command_buffer, -1, -1, true, true, nullptr, 0, r_current_label_index, r_current_label_level);
}

// Boosts, groups the barriers of and records one level's main commands, as end() did inline.
void RenderingDeviceGraph::_run_level(uint32_t p_level, RecordedCommandSort *p_level_commands, uint32_t p_level_command_count, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level, uint32_t &r_boosted_priority) {
	_boost_priority_for_render_commands(p_level_commands, p_level_command_count, r_boosted_priority);
	_group_barriers_for_render_commands(r_command_buffer, p_level_commands, p_level_command_count, p_full_barriers);
	_run_render_commands(p_level, p_level_commands, p_level_command_count, r_command_buffer, r_command_buffer_pool, r_current_label_index, r_current_label_level);
}

// Walks a sorted frame with an active plan. Each level's async commands sort after its main
// commands. The main ones keep today's order on the main command buffer, which is split once before
// fork_level (the command buffer that ends there is the one the async work waits for) and once
// before join_level (the command buffer that begins there waits for the async work). The async
// ones go level by level to the async command buffer. The indices are read from the pool at the
// split, so a split the main commands make themselves (the swap chain pass) keeps them right.
// T provides split_main(), which must advance r_command_buffer_pool.buffers_used by one, and
// run_main() and run_async(), which record a level's commands.
template <typename T>
void RenderingDeviceGraph::_walk_async_frame(RecordedCommandSort *p_commands_sorted, uint32_t p_command_count, const AsyncPlan &p_plan, CommandBufferPool &r_command_buffer_pool, T &r_emitter) {
	bool forked = false;
	bool joined = false;
	uint32_t level_start = 0;
	while (level_start < p_command_count) {
		const uint32_t level = p_commands_sorted[level_start].level;
		uint32_t level_end = level_start;
		uint32_t async_start = UINT32_MAX;
		while (level_end < p_command_count && p_commands_sorted[level_end].level == level) {
			if (async_start == UINT32_MAX && p_commands_sorted[level_end].queue == QUEUE_ASYNC_COMPUTE) {
				async_start = level_end;
			}
			level_end++;
		}
		if (async_start == UINT32_MAX) {
			async_start = level_end;
		}

		if (!forked && level >= p_plan.fork_level) {
			r_command_buffer_pool.async_fork_index = int32_t(r_command_buffer_pool.buffers_used);
			r_emitter.split_main();
			forked = true;
		}
		if (!joined && level >= p_plan.join_level) {
			r_emitter.split_main();
			r_command_buffer_pool.async_join_index = int32_t(r_command_buffer_pool.buffers_used);
			joined = true;
		}

		if (async_start > level_start) {
			r_emitter.run_main(level, &p_commands_sorted[level_start], async_start - level_start);
		}
		if (level_end > async_start) {
			r_emitter.run_async(level, &p_commands_sorted[async_start], level_end - async_start);
		}

		level_start = level_end;
	}

	if (!forked) {
		// An active plan has fork_level < join_level <= level_count, so the loop always forks; keep
		// the submission valid anyway.
		r_command_buffer_pool.async_fork_index = int32_t(r_command_buffer_pool.buffers_used);
		r_emitter.split_main();
	}
	if (!joined) {
		// Nothing on the main queue consumes the async work: join in a trailing command buffer, so
		// the frame fence still covers it.
		r_emitter.split_main();
		r_command_buffer_pool.async_join_index = int32_t(r_command_buffer_pool.buffers_used);
	}
}

void RenderingDeviceGraph::_run_async_frame(RecordedCommandSort *p_commands_sorted, const AsyncPlan &p_plan, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level, uint32_t &r_boosted_priority) {
	struct Emitter {
		RenderingDeviceGraph &graph;
		bool full_barriers;
		RDD::CommandBufferID &command_buffer;
		CommandBufferPool &pool;
		int32_t &label_index;
		int32_t &label_level;
		uint32_t &boosted_priority;
		RDD::CommandBufferID async_command_buffer;
		int32_t async_label_index = -1;
		int32_t async_label_level = -1;
		bool async_begun = false;

		void split_main() {
			graph._split_main_command_buffer(command_buffer, pool, label_index, label_level);
		}

		void run_main(uint32_t p_level, RecordedCommandSort *p_commands, uint32_t p_count) {
			graph._run_level(p_level, p_commands, p_count, full_barriers, command_buffer, pool, label_index, label_level, boosted_priority);
		}

		void run_async(uint32_t p_level, RecordedCommandSort *p_commands, uint32_t p_count) {
			if (!async_begun) {
				graph.driver->command_buffer_begin(async_command_buffer);
				graph._run_label_command_change(async_command_buffer, -1, -1, true, true, nullptr, 0, async_label_index, async_label_level);
				async_begun = true;
			}
			graph._group_barriers_for_render_commands(async_command_buffer, p_commands, p_count, full_barriers);
			graph._run_render_commands(p_level, p_commands, p_count, async_command_buffer, pool, async_label_index, async_label_level, true);
		}
	};

	Emitter emitter = { *this, p_full_barriers, r_command_buffer, r_command_buffer_pool, r_current_label_index, r_current_label_level, r_boosted_priority, r_command_buffer_pool.async_command_buffer };
	_walk_async_frame(p_commands_sorted, command_count, p_plan, r_command_buffer_pool, emitter);

	if (command_label_count > 0 && r_current_label_index < 0 && r_current_label_level < 0) {
		// Only the generic label reopened by the last split is open, which the closing call in end()
		// does not end.
		driver->command_end_label(r_command_buffer);
	}

	if (emitter.async_begun) {
		_run_label_command_change(emitter.async_command_buffer, -1, -1, false, false, nullptr, 0, emitter.async_label_index, emitter.async_label_level);
		driver->command_buffer_end(emitter.async_command_buffer);
	} else {
		// The plan had async commands, so this cannot happen; never leave a fork without work.
		DEV_ASSERT(false && "Async plan without async commands.");
		r_command_buffer_pool.async_fork_index = -1;
		r_command_buffer_pool.async_join_index = -1;
	}
}

void RenderingDeviceGraph::plan_for_test(LocalVector<int32_t> &r_order, AsyncPlan &r_plan, bool p_async_queue_available) const {
	LocalVector<RecordedCommandSort> commands_sorted;
	_compute_command_levels(commands_sorted);
	r_plan = _plan_async_frame(commands_sorted, p_async_queue_available);
	commands_sorted.sort();
	r_order.clear();
	for (const RecordedCommandSort &command_sort : commands_sorted) {
		r_order.push_back(command_sort.index);
	}
}

void RenderingDeviceGraph::emit_for_test(AsyncPlan &r_plan, AsyncEmission &r_emission, uint32_t p_split_main_level) const {
	LocalVector<RecordedCommandSort> commands_sorted;
	_compute_command_levels(commands_sorted);
	r_plan = _plan_async_frame(commands_sorted, true);
	commands_sorted.sort();

	r_emission = AsyncEmission();
	CommandBufferPool pool;
	if (r_plan.active) {
		struct Recorder {
			CommandBufferPool &pool;
			AsyncEmission &emission;
			uint32_t split_main_level;

			void record(AsyncEmission::StepType p_type, uint32_t p_level, uint32_t p_buffer, const RecordedCommandSort *p_commands, uint32_t p_count) {
				AsyncEmission::Step step;
				step.type = p_type;
				step.level = p_level;
				step.buffer = p_buffer;
				for (uint32_t i = 0; i < p_count; i++) {
					step.commands.push_back(p_commands[i].index);
				}
				emission.steps.push_back(step);
			}

			void split_main() {
				pool.buffers_used++;
				record(AsyncEmission::STEP_SPLIT, 0, pool.buffers_used, nullptr, 0);
			}

			void run_main(uint32_t p_level, RecordedCommandSort *p_commands, uint32_t p_count) {
				if (p_level == split_main_level) {
					split_main();
				}
				record(AsyncEmission::STEP_MAIN, p_level, pool.buffers_used, p_commands, p_count);
			}

			void run_async(uint32_t p_level, RecordedCommandSort *p_commands, uint32_t p_count) {
				record(AsyncEmission::STEP_ASYNC, p_level, 0, p_commands, p_count);
			}
		};

		Recorder recorder = { pool, r_emission, p_split_main_level };
		_walk_async_frame(commands_sorted.ptr(), command_count, r_plan, pool, recorder);
	}
	r_emission.fork_index = pool.async_fork_index;
	r_emission.join_index = pool.async_join_index;
	r_emission.buffers_used = pool.buffers_used;
}

void RenderingDeviceGraph::submit_chained_command_buffers(const ChainedSubmission &p_submission, ChainedSubmitFunction p_submit, void *p_userdata) {
	const CommandBufferPool &pool = *p_submission.pool;
	const uint32_t command_buffer_count = p_submission.command_buffer_count;
	const int32_t async_fork_index = p_submission.async_fork_index;
	const int32_t async_join_index = p_submission.async_join_index;
	const bool async_submit = async_fork_index >= 0;
	DEV_ASSERT(!async_submit || (async_fork_index < async_join_index && async_join_index < int32_t(command_buffer_count)));

	// Instead of having just one command; we have potentially many (which had to be split due to an
	// Adreno workaround on mobile, only if the workaround is active). Thus we must execute all of them
	// and chain them together via semaphores as dependent executions.
	thread_local LocalVector<RDD::SemaphoreID> wait_semaphores;
	wait_semaphores.clear();
	for (uint32_t i = 0; i < p_submission.frame_wait_semaphores.size(); i++) {
		wait_semaphores.push_back(p_submission.frame_wait_semaphores[i]);
	}

	for (uint32_t i = 0; i < command_buffer_count; i++) {
		RDD::CommandBufferID command_buffer;
		RDD::SemaphoreID signal_semaphore;
		RDD::FenceID signal_fence;
		VectorView<RDD::SwapChainID> swap_chains;
		if (i > 0) {
			command_buffer = pool.buffers[i - 1];
		} else {
			command_buffer = p_submission.frame_command_buffer;
		}

		if (i == (command_buffer_count - 1)) {
			// This is the last command buffer, it should signal the semaphore & fence, and present.
			signal_semaphore = p_submission.last_signal_semaphore;
			signal_fence = p_submission.last_signal_fence;
			swap_chains = p_submission.last_swap_chains;
		} else {
			// Semaphores always need to be signaled if it's not the last command buffer.
			signal_semaphore = pool.semaphores[i];
		}

		// The join command buffer also waits for the async work. The async work was submitted with
		// the fork command buffer, which comes first, so the signal is submitted before the wait.
		if (async_submit && int32_t(i) == async_join_index) {
			wait_semaphores.push_back(p_submission.async_join_semaphore);
		}

		if (async_submit && int32_t(i) == async_fork_index) {
			// The fork command buffer signals the next main command buffer and the async work, which
			// then runs on the async queue and signals the join command buffer.
			const RDD::SemaphoreID fork_signal_semaphores[2] = { signal_semaphore, p_submission.async_fork_semaphore };
			p_submit(p_userdata, p_submission.main_queue, wait_semaphores, command_buffer, VectorView<RDD::SemaphoreID>(fork_signal_semaphores, 2), signal_fence, swap_chains);
			p_submit(p_userdata, p_submission.async_queue, p_submission.async_fork_semaphore, p_submission.async_command_buffer, p_submission.async_join_semaphore, RDD::FenceID(), VectorView<RDD::SwapChainID>());
		} else {
			p_submit(p_userdata, p_submission.main_queue, wait_semaphores, command_buffer, signal_semaphore ? signal_semaphore : VectorView<RDD::SemaphoreID>(), signal_fence, swap_chains);
		}

		// Make the next command buffer wait on the semaphore signaled by this one.
		wait_semaphores.resize(1);
		wait_semaphores[0] = signal_semaphore;
	}
}
