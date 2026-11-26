/**************************************************************************/
/*  rendering_device_graph.cpp                                            */
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

#include "rendering_device_graph.h"

#define PRINT_RENDER_GRAPH 0
#define FORCE_FULL_ACCESS_BITS 0
#define PRINT_RESOURCE_TRACKER_TOTAL 0
#define PRINT_COMMAND_RECORDING 0

// Prints the total number of bytes used for draw lists in a frame.
#define PRINT_DRAW_LIST_STATS 0

RenderingDeviceGraph::RenderingDeviceGraph() {
	driver_honors_barriers = false;
	driver_buffer_clears_with_copy_engine = false;
	driver_texture_clears_with_copy_engine = false;
	driver_buffers_require_transitions = false;
	driver_textures_require_layout_transitions = false;
}

RenderingDeviceGraph::~RenderingDeviceGraph() {
}

String RenderingDeviceGraph::_usage_to_string(ResourceUsage p_usage) {
	switch (p_usage) {
		case RESOURCE_USAGE_NONE:
			return "None";
		case RESOURCE_USAGE_COPY_FROM:
			return "Copy From";
		case RESOURCE_USAGE_COPY_TO:
			return "Copy To";
		case RESOURCE_USAGE_RESOLVE_FROM:
			return "Resolve From";
		case RESOURCE_USAGE_RESOLVE_TO:
			return "Resolve To";
		case RESOURCE_USAGE_UNIFORM_BUFFER_READ:
			return "Uniform Buffer Read";
		case RESOURCE_USAGE_INDIRECT_BUFFER_READ:
			return "Indirect Buffer Read";
		case RESOURCE_USAGE_TEXTURE_BUFFER_READ:
			return "Texture Buffer Read";
		case RESOURCE_USAGE_TEXTURE_BUFFER_READ_WRITE:
			return "Texture Buffer Read Write";
		case RESOURCE_USAGE_STORAGE_BUFFER_READ:
			return "Storage Buffer Read";
		case RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE:
			return "Storage Buffer Read Write";
		case RESOURCE_USAGE_VERTEX_BUFFER_READ:
			return "Vertex Buffer Read";
		case RESOURCE_USAGE_INDEX_BUFFER_READ:
			return "Index Buffer Read";
		case RESOURCE_USAGE_TEXTURE_SAMPLE:
			return "Texture Sample";
		case RESOURCE_USAGE_STORAGE_IMAGE_READ:
			return "Storage Image Read";
		case RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE:
			return "Storage Image Read Write";
		case RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE:
			return "Attachment Color Read Write";
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE:
			return "Attachment Depth Stencil Read Write";
		// GodotNext: rd_readonly_depth (#301).
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ:
			return "Attachment Depth Stencil Read";
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_SHADING_RATE_READ:
			return "Attachment Fragment Shading Rate Read";
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_DENSITY_MAP_READ:
			return "Attachment Fragment Density Map Read";
		case RESOURCE_USAGE_GENERAL:
			return "General";
		case RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ:
			return "Acceleration Structure Read";
		case RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE:
			return "Acceleration Structure Read Write";
		default:
			ERR_FAIL_V_MSG("Invalid", vformat("Invalid resource usage %d.", p_usage));
	}
}

// GodotNext: rdg_cross_frame_waw (#262)
bool RenderingDeviceGraph::cross_frame_write_after_write(ResourceUsage p_last_usage, ResourceUsage p_new_usage, bool p_first_use_this_frame, bool p_is_buffer) {
	return p_is_buffer && p_first_use_this_frame && p_last_usage != RESOURCE_USAGE_NONE && _is_write_usage(p_last_usage) && _is_write_usage(p_new_usage);
}

bool RenderingDeviceGraph::_is_write_usage(ResourceUsage p_usage) {
	switch (p_usage) {
		case RESOURCE_USAGE_COPY_FROM:
		case RESOURCE_USAGE_RESOLVE_FROM:
		case RESOURCE_USAGE_UNIFORM_BUFFER_READ:
		case RESOURCE_USAGE_INDIRECT_BUFFER_READ:
		case RESOURCE_USAGE_TEXTURE_BUFFER_READ:
		case RESOURCE_USAGE_STORAGE_BUFFER_READ:
		case RESOURCE_USAGE_VERTEX_BUFFER_READ:
		case RESOURCE_USAGE_INDEX_BUFFER_READ:
		case RESOURCE_USAGE_TEXTURE_SAMPLE:
		case RESOURCE_USAGE_STORAGE_IMAGE_READ:
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_SHADING_RATE_READ:
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_DENSITY_MAP_READ:
		case RESOURCE_USAGE_ATTACHMENT_RASTERIZATION_RATE_MAP_READ:
		case RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ:
		// GodotNext: rd_readonly_depth (#301).
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ:
			return false;
		case RESOURCE_USAGE_COPY_TO:
		case RESOURCE_USAGE_RESOLVE_TO:
		case RESOURCE_USAGE_TEXTURE_BUFFER_READ_WRITE:
		case RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE:
		case RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE:
		case RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE:
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE:
		case RESOURCE_USAGE_GENERAL:
		case RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE:
			return true;
		default:
			DEV_ASSERT(false && "Invalid resource tracker usage.");
			return false;
	}
}

// GodotNext: rd_readonly_depth (#301).
bool RenderingDeviceGraph::_is_depth_stencil_texture(const ResourceTracker *p_tracker) {
	return p_tracker->texture_driver_id.id != 0 &&
			(p_tracker->texture_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_DEPTH_BIT) ||
					p_tracker->texture_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_STENCIL_BIT));
}

RDD::TextureLayout RenderingDeviceGraph::_usage_to_image_layout(ResourceUsage p_usage, bool p_is_depth_stencil) {
	switch (p_usage) {
		case RESOURCE_USAGE_COPY_FROM:
			return RDD::TEXTURE_LAYOUT_COPY_SRC_OPTIMAL;
		case RESOURCE_USAGE_COPY_TO:
			return RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL;
		case RESOURCE_USAGE_RESOLVE_FROM:
			return RDD::TEXTURE_LAYOUT_RESOLVE_SRC_OPTIMAL;
		case RESOURCE_USAGE_RESOLVE_TO:
			return RDD::TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL;
		case RESOURCE_USAGE_TEXTURE_SAMPLE:
			// GodotNext: rd_readonly_depth (#301). Depth/stencil textures sample in the shared
			// read-only depth layout so sampled reads and read-only depth attachments need no
			// layout transition between them.
			return p_is_depth_stencil ? RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : RDD::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case RESOURCE_USAGE_STORAGE_IMAGE_READ:
		case RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE:
			return RDD::TEXTURE_LAYOUT_STORAGE_OPTIMAL;
		case RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE:
			return RDD::TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE:
			return RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		// GodotNext: rd_readonly_depth (#301).
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ:
			return RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_SHADING_RATE_READ:
			return RDD::TEXTURE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL;
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_DENSITY_MAP_READ:
			return RDD::TEXTURE_LAYOUT_FRAGMENT_DENSITY_MAP_ATTACHMENT_OPTIMAL;
		case RESOURCE_USAGE_ATTACHMENT_RASTERIZATION_RATE_MAP_READ:
			// Rasterization rate map is not a real texture and it's readonly from shaders,
			// so it doesn't need a texture layout
			return RDD::TEXTURE_LAYOUT_UNDEFINED;
		case RESOURCE_USAGE_GENERAL:
			return RDD::TEXTURE_LAYOUT_GENERAL;
		case RESOURCE_USAGE_NONE:
			return RDD::TEXTURE_LAYOUT_UNDEFINED;
		default:
			DEV_ASSERT(false && "Invalid resource tracker usage or not an image usage.");
			return RDD::TEXTURE_LAYOUT_UNDEFINED;
	}
}

RDD::BarrierAccessBits RenderingDeviceGraph::_usage_to_access_bits(ResourceUsage p_usage) {
#if FORCE_FULL_ACCESS_BITS
	return RDD::BarrierAccessBits(RDD::BARRIER_ACCESS_MEMORY_READ_BIT | RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT);
#else
	switch (p_usage) {
		case RESOURCE_USAGE_NONE:
			return RDD::BarrierAccessBits(0);
		case RESOURCE_USAGE_COPY_FROM:
			return RDD::BARRIER_ACCESS_COPY_READ_BIT;
		case RESOURCE_USAGE_COPY_TO:
			return RDD::BARRIER_ACCESS_COPY_WRITE_BIT;
		case RESOURCE_USAGE_RESOLVE_FROM:
			return RDD::BARRIER_ACCESS_RESOLVE_READ_BIT;
		case RESOURCE_USAGE_RESOLVE_TO:
			return RDD::BARRIER_ACCESS_RESOLVE_WRITE_BIT;
		case RESOURCE_USAGE_UNIFORM_BUFFER_READ:
			return RDD::BARRIER_ACCESS_UNIFORM_READ_BIT;
		case RESOURCE_USAGE_INDIRECT_BUFFER_READ:
			return RDD::BARRIER_ACCESS_INDIRECT_COMMAND_READ_BIT;
		case RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ:
			return RDD::BARRIER_ACCESS_ACCELERATION_STRUCTURE_READ_BIT;
		case RESOURCE_USAGE_STORAGE_BUFFER_READ:
		case RESOURCE_USAGE_STORAGE_IMAGE_READ:
		case RESOURCE_USAGE_TEXTURE_BUFFER_READ:
		case RESOURCE_USAGE_TEXTURE_SAMPLE:
			return RDD::BARRIER_ACCESS_SHADER_READ_BIT;
		case RESOURCE_USAGE_TEXTURE_BUFFER_READ_WRITE:
		case RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE:
		case RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE:
			return RDD::BarrierAccessBits(RDD::BARRIER_ACCESS_SHADER_READ_BIT | RDD::BARRIER_ACCESS_SHADER_WRITE_BIT);
		case RESOURCE_USAGE_VERTEX_BUFFER_READ:
			return RDD::BARRIER_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
		case RESOURCE_USAGE_INDEX_BUFFER_READ:
			return RDD::BARRIER_ACCESS_INDEX_READ_BIT;
		case RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE:
			return RDD::BarrierAccessBits(RDD::BARRIER_ACCESS_COLOR_ATTACHMENT_READ_BIT | RDD::BARRIER_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE:
			return RDD::BarrierAccessBits(RDD::BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | RDD::BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
		// GodotNext: rd_readonly_depth (#301).
		case RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ:
			return RDD::BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_SHADING_RATE_READ:
			return RDD::BARRIER_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT;
		case RESOURCE_USAGE_ATTACHMENT_FRAGMENT_DENSITY_MAP_READ:
			return RDD::BARRIER_ACCESS_FRAGMENT_DENSITY_MAP_ATTACHMENT_READ_BIT;
		case RESOURCE_USAGE_ATTACHMENT_RASTERIZATION_RATE_MAP_READ:
			// Rasterization rate map is not a real texture and it's readonly from shaders,
			// so it doesn't need barrier access attributes
			return RDD::BarrierAccessBits(0);
		case RESOURCE_USAGE_GENERAL:
			return RDD::BarrierAccessBits(RDD::BARRIER_ACCESS_MEMORY_READ_BIT | RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT);
		case RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE:
			return RDD::BarrierAccessBits(RDD::BARRIER_ACCESS_ACCELERATION_STRUCTURE_READ_BIT | RDD::BARRIER_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT);
		default:
			DEV_ASSERT(false && "Invalid usage.");
			return RDD::BarrierAccessBits(0);
	}
#endif
}

bool RenderingDeviceGraph::_check_command_intersection(ResourceTracker *p_resource_tracker, int32_t p_previous_command_index, int32_t p_command_index) const {
	if (p_resource_tracker->usage != RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE && p_resource_tracker->usage != RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE) {
		// We don't check possible intersections for usages that aren't consecutive color or depth writes.
		return true;
	}

	const uint32_t previous_command_data_offset = command_data_offsets[p_previous_command_index];
	const uint32_t current_command_data_offset = command_data_offsets[p_command_index];
	const RecordedDrawListCommand &previous_draw_list_command = *reinterpret_cast<const RecordedDrawListCommand *>(&command_data[previous_command_data_offset]);
	const RecordedDrawListCommand &current_draw_list_command = *reinterpret_cast<const RecordedDrawListCommand *>(&command_data[current_command_data_offset]);
	if (previous_draw_list_command.type != RecordedCommand::TYPE_DRAW_LIST || current_draw_list_command.type != RecordedCommand::TYPE_DRAW_LIST) {
		// We don't check possible intersections if both commands aren't draw lists.
		return true;
	}

	// We check if the region used by both draw lists have an intersection.
	return previous_draw_list_command.region.intersects(current_draw_list_command.region);
}

bool RenderingDeviceGraph::_check_command_partial_coverage(ResourceTracker *p_resource_tracker, int32_t p_command_index) const {
	if (p_resource_tracker->usage != RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE && p_resource_tracker->usage != RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE) {
		// We don't check for partial coverage in usages that aren't attachment writes.
		return false;
	}

	const uint32_t command_data_offset = command_data_offsets[p_command_index];
	const RecordedDrawListCommand &draw_list_command = *reinterpret_cast<const RecordedDrawListCommand *>(&command_data[command_data_offset]);
	if (draw_list_command.type != RecordedCommand::TYPE_DRAW_LIST) {
		// We don't check for partial coverage on commands that aren't draw lists.
		return false;
	}

	Rect2i texture_region(Point2i(0, 0), p_resource_tracker->texture_size);
	return !draw_list_command.region.encloses(texture_region);
}

int32_t RenderingDeviceGraph::_add_to_command_list(int32_t p_command_index, int32_t p_list_index) {
	DEV_ASSERT(p_command_index < int32_t(command_count));
	DEV_ASSERT(p_list_index < int32_t(command_list_nodes.size()));

	int32_t next_index = int32_t(command_list_nodes.size());
	command_list_nodes.resize(next_index + 1);

	RecordedCommandListNode &new_node = command_list_nodes[next_index];
	new_node.command_index = p_command_index;
	new_node.next_list_index = p_list_index;
	return next_index;
}

void RenderingDeviceGraph::_add_adjacent_command(int32_t p_previous_command_index, int32_t p_command_index, RecordedCommand *r_command) {
	const uint32_t previous_command_data_offset = command_data_offsets[p_previous_command_index];
	RecordedCommand &previous_command = *reinterpret_cast<RecordedCommand *>(&command_data[previous_command_data_offset]);
	previous_command.adjacent_command_list_index = _add_to_command_list(p_command_index, previous_command.adjacent_command_list_index);
	previous_command.next_stages = previous_command.next_stages | r_command->self_stages;
	r_command->previous_stages = r_command->previous_stages | previous_command.self_stages;
}

int32_t RenderingDeviceGraph::_add_to_slice_read_list(int32_t p_command_index, Rect2i p_subresources, int32_t p_list_index) {
	DEV_ASSERT(p_command_index < int32_t(command_count));
	DEV_ASSERT(p_list_index < int32_t(read_slice_list_nodes.size()));

	int32_t next_index = int32_t(read_slice_list_nodes.size());
	read_slice_list_nodes.resize(next_index + 1);

	RecordedSliceListNode &new_node = read_slice_list_nodes[next_index];
	new_node.command_index = p_command_index;
	new_node.next_list_index = p_list_index;
	new_node.subresources = p_subresources;
	return next_index;
}

int32_t RenderingDeviceGraph::_add_to_write_list(int32_t p_command_index, Rect2i p_subresources, int32_t p_list_index, bool p_partial_coverage) {
	DEV_ASSERT(p_command_index < int32_t(command_count));
	DEV_ASSERT(p_list_index < int32_t(write_slice_list_nodes.size()));

	int32_t next_index = int32_t(write_slice_list_nodes.size());
	write_slice_list_nodes.resize(next_index + 1);

	RecordedSliceListNode &new_node = write_slice_list_nodes[next_index];
	new_node.command_index = p_command_index;
	new_node.next_list_index = p_list_index;
	new_node.subresources = p_subresources;
	new_node.partial_coverage = p_partial_coverage;
	return next_index;
}

int32_t RenderingDeviceGraph::_add_to_async_slice_list(int32_t p_command_index, Rect2i p_subresources, int32_t p_list_index) {
	DEV_ASSERT(p_command_index < int32_t(command_count));
	DEV_ASSERT(p_list_index < int32_t(async_slice_list_nodes.size()));

	int32_t next_index = int32_t(async_slice_list_nodes.size());
	async_slice_list_nodes.resize(next_index + 1);

	RecordedSliceListNode &new_node = async_slice_list_nodes[next_index];
	new_node.command_index = p_command_index;
	new_node.next_list_index = p_list_index;
	new_node.subresources = p_subresources;
	return next_index;
}

// Ensures all commands are 8-byte aligned.
#define GRAPH_ALIGN(x) (((x) + 7u) & 0xFFFFFFF8u)

RenderingDeviceGraph::RecordedCommand *RenderingDeviceGraph::_allocate_command(uint32_t p_command_size, int32_t &r_command_index) {
	uint32_t command_data_offset = command_data.size();
	command_data_offset = GRAPH_ALIGN(command_data_offset);
	command_data_offsets.push_back(command_data_offset);
	command_data.resize(command_data_offset + p_command_size);
	r_command_index = command_count++;
	RecordedCommand *new_command = reinterpret_cast<RecordedCommand *>(&command_data[command_data_offset]);
	*new_command = RecordedCommand();
	return new_command;
}

RenderingDeviceGraph::DrawListInstruction *RenderingDeviceGraph::_allocate_draw_list_instruction(uint32_t p_instruction_size) {
	uint32_t draw_list_data_offset = draw_instruction_list.data.size();
	draw_list_data_offset = GRAPH_ALIGN(draw_list_data_offset);
	draw_instruction_list.data.resize(draw_list_data_offset + p_instruction_size);
	return reinterpret_cast<DrawListInstruction *>(&draw_instruction_list.data[draw_list_data_offset]);
}

RenderingDeviceGraph::ComputeListInstruction *RenderingDeviceGraph::_allocate_compute_list_instruction(uint32_t p_instruction_size) {
	uint32_t compute_list_data_offset = compute_instruction_list.data.size();
	compute_list_data_offset = GRAPH_ALIGN(compute_list_data_offset);
	compute_instruction_list.data.resize(compute_list_data_offset + p_instruction_size);
	return reinterpret_cast<ComputeListInstruction *>(&compute_instruction_list.data[compute_list_data_offset]);
}

void RenderingDeviceGraph::_check_discardable_attachment_dependency(ResourceTracker *p_resource_tracker, int32_t p_previous_command_index, int32_t p_command_index) {
	if (!p_resource_tracker->is_discardable) {
		return;
	}

	// Check if the command is a a draw list that clears the attachment completely. If it is, we don't need to modify the previous draw list.
	uint32_t command_offset = command_data_offsets[p_command_index];
	RecordedDrawListCommand *draw_list_command = reinterpret_cast<RecordedDrawListCommand *>(&command_data[command_offset]);
	if (draw_list_command->type == RecordedCommand::TYPE_DRAW_LIST) {
		ResourceTracker **trackers = draw_list_command->trackers();
		for (uint32_t i = 0; i < draw_list_command->trackers_count; i++) {
			if (trackers[i] == p_resource_tracker && draw_list_command->load_ops()[i] == RDD::ATTACHMENT_LOAD_OP_CLEAR) {
				return;
			}
		}
	}

	// Check if the previous command is a draw list.
	uint32_t previous_command_offset = command_data_offsets[p_previous_command_index];
	RecordedDrawListCommand *previous_draw_list_command = reinterpret_cast<RecordedDrawListCommand *>(&command_data[previous_command_offset]);
	if (previous_draw_list_command->type != RecordedCommand::TYPE_DRAW_LIST) {
		return;
	}

	// Search for the tracker inside the draw list command and modify the store operation accordingly.
	ResourceTracker **trackers = previous_draw_list_command->trackers();
	for (uint32_t i = 0; i < previous_draw_list_command->trackers_count; i++) {
		if (trackers[i] == p_resource_tracker) {
			// GodotNext: rd_readonly_depth (#301). Never promote a read-only attachment's STORE_OP_NONE to STORE.
			if (previous_draw_list_command->store_ops()[i] != RDD::ATTACHMENT_STORE_OP_NONE) {
				previous_draw_list_command->store_ops()[i] = RDD::ATTACHMENT_STORE_OP_STORE;
			}
			return;
		}
	}
}

RenderingDeviceGraph::RaytracingListInstruction *RenderingDeviceGraph::_allocate_raytracing_list_instruction(uint32_t p_instruction_size) {
	uint32_t raytracing_list_data_offset = raytracing_instruction_list.data.size();
	raytracing_instruction_list.data.resize(raytracing_list_data_offset + p_instruction_size);
	return reinterpret_cast<RaytracingListInstruction *>(&raytracing_instruction_list.data[raytracing_list_data_offset]);
}

void RenderingDeviceGraph::_add_command_to_graph(ResourceTracker **p_resource_trackers, ResourceUsage *p_resource_usages, uint32_t p_resource_count, int32_t p_command_index, RecordedCommand *r_command) {
	// Assign the next stages derived from the stages the command requires first.
	r_command->next_stages = r_command->self_stages;

	if (command_label_index >= 0) {
		// If a label is active, tag the command with the label.
		r_command->label_index = command_label_index;
	}

	if (r_command->type == RecordedCommand::TYPE_CAPTURE_TIMESTAMP) {
		// All previous commands starting from the previous timestamp should be adjacent to this command.
		int32_t start_command_index = uint32_t(MAX(command_timestamp_index, 0));
		for (int32_t i = start_command_index; i < p_command_index; i++) {
			_add_adjacent_command(i, p_command_index, r_command);
		}

		// Make this command the new active timestamp command.
		command_timestamp_index = p_command_index;
	} else if (command_timestamp_index >= 0) {
		// Timestamp command should be adjacent to this command.
		_add_adjacent_command(command_timestamp_index, p_command_index, r_command);
	}

	if (command_synchronization_pending) {
		// All previous commands should be adjacent to this command.
		int32_t start_command_index = uint32_t(MAX(command_synchronization_index, 0));
		for (int32_t i = start_command_index; i < p_command_index; i++) {
			_add_adjacent_command(i, p_command_index, r_command);
		}

		command_synchronization_index = p_command_index;
		command_synchronization_pending = false;
	} else if (command_synchronization_index >= 0) {
		// Synchronization command should be adjacent to this command.
		_add_adjacent_command(command_synchronization_index, p_command_index, r_command);
	}

	for (uint32_t i = 0; i < p_resource_count; i++) {
		ResourceTracker *resource_tracker = p_resource_trackers[i];
		DEV_ASSERT(resource_tracker != nullptr);

		resource_tracker->reset_if_outdated(tracking_frame);

		resource_tracker->command_index = p_command_index;
		resource_tracker->usage_index = i;
	}

	for (uint32_t i = 0; i < p_resource_count; i++) {
		ResourceTracker *resource_tracker = p_resource_trackers[i];
		// GodotNext: async_compute (#174). The texture barriers this resource adds start here.
		const int32_t async_normalization_start = r_command->normalization_barrier_count;
		const int32_t async_transition_start = r_command->transition_barrier_count;

		// GodotNext: rd_bindless (#176). A use of a resident resource, or of a slice of one, can leave it
		// unready for the tables that hold it.
		ResourceTracker *bindless_tracker = resource_tracker->parent != nullptr ? resource_tracker->parent : resource_tracker;
		if (!bindless_tracker->bindless_residency.is_empty()) {
			_bindless_resource_used(bindless_tracker, p_resource_usages[i], p_command_index, r_command);
		}

		const RDD::TextureSubresourceRange &subresources = resource_tracker->texture_subresources;
		const Rect2i resource_tracker_rect(subresources.base_mipmap, subresources.base_layer, subresources.mipmap_count, subresources.layer_count);
		Rect2i search_tracker_rect = resource_tracker_rect;

		ResourceUsage new_resource_usage = p_resource_usages[i];
		bool write_usage = _is_write_usage(new_resource_usage);
		BitField<RDD::BarrierAccessBits> new_usage_access = _usage_to_access_bits(new_resource_usage);
		// GodotNext: rd_readonly_depth (#301). A draw list attaching depth read-only while also
		// binding a bindless texture table containing that dirty depth texture needs both read accesses.
		if (r_command->type == RecordedCommand::TYPE_DRAW_LIST &&
				!draw_instruction_list.bindless_readonly_depth_trackers.is_empty() &&
				draw_instruction_list.bindless_readonly_depth_trackers.has(resource_tracker)) {
			new_usage_access = new_usage_access | RDD::BARRIER_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | RDD::BARRIER_ACCESS_SHADER_READ_BIT;
		}
		bool is_resource_a_slice = resource_tracker->parent != nullptr;
		if (is_resource_a_slice) {
			// This resource depends on a parent resource.
			resource_tracker->parent->reset_if_outdated(tracking_frame);

			// Quit early if the parent is already in this command.
			if (resource_tracker->parent->command_index == p_command_index) {
				DEV_ASSERT(resource_tracker->parent->usage_index != UINT32_MAX);
				const ResourceUsage parent_usage = p_resource_usages[resource_tracker->parent->usage_index];
				// GodotNext: rd_readonly_depth (#301). A depth/stencil slice attached read-only in
				// a draw list that also samples the whole parent via a bindless texture table shares
				// the same read-only depth layout; merge both read accesses onto the parent.
				const bool shared_readonly_depth_pair = r_command->type == RecordedCommand::TYPE_DRAW_LIST &&
						_is_depth_stencil_texture(resource_tracker) &&
						((parent_usage == RESOURCE_USAGE_TEXTURE_SAMPLE && new_resource_usage == RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ) ||
								(parent_usage == RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ && new_resource_usage == RESOURCE_USAGE_TEXTURE_SAMPLE));
				if (shared_readonly_depth_pair) {
					if (!draw_instruction_list.bindless_readonly_depth_trackers.has(resource_tracker->parent)) {
						draw_instruction_list.bindless_readonly_depth_trackers.push_back(resource_tracker->parent);
					}
					continue;
				}

				ERR_FAIL_COND_MSG(parent_usage != new_resource_usage, "Using a full texture and its slices at the same time with different usages is not allowed.");

				continue;
			}

			if (resource_tracker->texture_slice_command_index != p_command_index) {
				// Indicate this slice has been used by this command.
				resource_tracker->texture_slice_command_index = p_command_index;
			}

			if (resource_tracker->parent->usage == RESOURCE_USAGE_NONE) {
				if (resource_tracker->parent->texture_driver_id.id != 0) {
					// If the resource is a texture, we transition it entirely to the layout determined by the first slice that uses it.
					_add_texture_barrier_to_command(resource_tracker->parent->texture_driver_id, RDD::BarrierAccessBits(0), new_usage_access, RDG::RESOURCE_USAGE_NONE, new_resource_usage, resource_tracker->parent->texture_subresources, command_normalization_barriers, r_command->normalization_barrier_index, r_command->normalization_barrier_count);
				}

				// If the parent hasn't been used yet, we assign the usage of the slice to the entire resource.
				resource_tracker->parent->usage = new_resource_usage;

				// Also assign the usage to the slice and consider it a write operation. Consider the parent's current usage access as its own.
				resource_tracker->usage = new_resource_usage;
				resource_tracker->usage_access = resource_tracker->parent->usage_access;
				resource_tracker->usage_stages = resource_tracker->parent->usage_stages; // GodotNext: rdg_access_stages (#267)
				write_usage = true;

				// Indicate the area that should be tracked is the entire resource.
				const RDD::TextureSubresourceRange &parent_subresources = resource_tracker->parent->texture_subresources;
				search_tracker_rect = Rect2i(parent_subresources.base_mipmap, parent_subresources.base_layer, parent_subresources.mipmap_count, parent_subresources.layer_count);
			} else if (resource_tracker->in_parent_dirty_list) {
				if (resource_tracker->parent->usage == new_resource_usage) {
					// The slice will be transitioned to the resource of the parent and can be deleted from the dirty list.
					ResourceTracker *previous_tracker = nullptr;
					ResourceTracker *current_tracker = resource_tracker->parent->dirty_shared_list;
					bool initialized_dirty_rect = false;
					while (current_tracker != nullptr) {
						current_tracker->reset_if_outdated(tracking_frame);

						if (current_tracker == resource_tracker) {
							current_tracker->in_parent_dirty_list = false;

							if (previous_tracker != nullptr) {
								previous_tracker->next_shared = current_tracker->next_shared;
							} else {
								resource_tracker->parent->dirty_shared_list = current_tracker->next_shared;
							}

							current_tracker = current_tracker->next_shared;
						} else {
							if (initialized_dirty_rect) {
								resource_tracker->parent->texture_slice_or_dirty_rect = resource_tracker->parent->texture_slice_or_dirty_rect.merge(current_tracker->texture_slice_or_dirty_rect);
							} else {
								resource_tracker->parent->texture_slice_or_dirty_rect = current_tracker->texture_slice_or_dirty_rect;
								initialized_dirty_rect = true;
							}

							previous_tracker = current_tracker;
							current_tracker = current_tracker->next_shared;
						}
					}
				}
			} else {
				if (resource_tracker->parent->dirty_shared_list != nullptr && resource_tracker->parent->texture_slice_or_dirty_rect.intersects(resource_tracker->texture_slice_or_dirty_rect)) {
					// There's an intersection with the current dirty area of the parent and the slice. We must verify if the intersection is against a slice
					// that was used in this command or not. Any slice we can find that wasn't used by this command must be reverted to the layout of the parent.
					ResourceTracker *previous_tracker = nullptr;
					ResourceTracker *current_tracker = resource_tracker->parent->dirty_shared_list;
					bool initialized_dirty_rect = false;
					while (current_tracker != nullptr) {
						current_tracker->reset_if_outdated(tracking_frame);

						if (current_tracker->texture_slice_or_dirty_rect.intersects(resource_tracker->texture_slice_or_dirty_rect)) {
							if (current_tracker->command_frame == tracking_frame && current_tracker->texture_slice_command_index == p_command_index) {
								ERR_FAIL_MSG("Texture slices that overlap can't be used in the same command.");
							} else {
								// Delete the slice from the dirty list and revert it to the usage of the parent.
								if (current_tracker->texture_driver_id.id != 0) {
									_add_texture_barrier_to_command(current_tracker->texture_driver_id, current_tracker->usage_access, new_usage_access, current_tracker->usage, resource_tracker->parent->usage, current_tracker->texture_subresources, command_normalization_barriers, r_command->normalization_barrier_index, r_command->normalization_barrier_count);
									r_command->previous_stages = r_command->previous_stages | current_tracker->usage_stages; // GodotNext: rdg_access_stages (#267)

									// Merge the area of the slice with the current tracking area of the command and indicate it's a write usage as well.
									search_tracker_rect = search_tracker_rect.merge(current_tracker->texture_slice_or_dirty_rect);
									write_usage = true;
								}

								current_tracker->in_parent_dirty_list = false;

								if (previous_tracker != nullptr) {
									previous_tracker->next_shared = current_tracker->next_shared;
								} else {
									resource_tracker->parent->dirty_shared_list = current_tracker->next_shared;
								}

								current_tracker = current_tracker->next_shared;
							}
						} else {
							// Recalculate the dirty rect of the parent so the deleted slices are excluded.
							if (initialized_dirty_rect) {
								resource_tracker->parent->texture_slice_or_dirty_rect = resource_tracker->parent->texture_slice_or_dirty_rect.merge(current_tracker->texture_slice_or_dirty_rect);
							} else {
								resource_tracker->parent->texture_slice_or_dirty_rect = current_tracker->texture_slice_or_dirty_rect;
								initialized_dirty_rect = true;
							}

							previous_tracker = current_tracker;
							current_tracker = current_tracker->next_shared;
						}
					}
				}

				// If it wasn't in the list, assume the usage is the same as the parent. Consider the parent's current usage access as its own.
				resource_tracker->usage = resource_tracker->parent->usage;
				resource_tracker->usage_access = resource_tracker->parent->usage_access;
				resource_tracker->usage_stages = resource_tracker->parent->usage_stages; // GodotNext: rdg_access_stages (#267)

				// GodotNext: rd_readonly_depth (#301). A depth/stencil slice reading in the shared
				// read-only depth layout while its parent is cleanly in that layout needs no layout
				// transition and must not enter dirty_shared_list.
				const bool slice_same_shared_read_only_depth = driver_textures_require_layout_transitions &&
						!write_usage &&
						_is_depth_stencil_texture(resource_tracker) &&
						resource_tracker->parent->dirty_shared_list == nullptr &&
						resource_tracker->parent->usage != RESOURCE_USAGE_NONE &&
						!_is_write_usage(resource_tracker->parent->usage) &&
						_usage_to_image_layout(resource_tracker->parent->usage, true) == RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
						_usage_to_image_layout(new_resource_usage, true) == RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

				if (resource_tracker->usage != new_resource_usage && !slice_same_shared_read_only_depth) {
					// Insert to the dirty list if the requested usage is different.
					resource_tracker->next_shared = resource_tracker->parent->dirty_shared_list;
					resource_tracker->parent->dirty_shared_list = resource_tracker;
					resource_tracker->in_parent_dirty_list = true;
					if (resource_tracker->parent->dirty_shared_list != nullptr) {
						resource_tracker->parent->texture_slice_or_dirty_rect = resource_tracker->parent->texture_slice_or_dirty_rect.merge(resource_tracker->texture_slice_or_dirty_rect);
					} else {
						resource_tracker->parent->texture_slice_or_dirty_rect = resource_tracker->texture_slice_or_dirty_rect;
					}
				}
			}
		} else {
			ResourceTracker *current_tracker = resource_tracker->dirty_shared_list;
			if (current_tracker != nullptr) {
				// Consider the usage as write if we must transition any of the slices.
				write_usage = true;
			}

			while (current_tracker != nullptr) {
				current_tracker->reset_if_outdated(tracking_frame);

				if (current_tracker->texture_driver_id.id != 0) {
					// Transition all slices to the layout of the parent resource.
					_add_texture_barrier_to_command(current_tracker->texture_driver_id, current_tracker->usage_access, new_usage_access, current_tracker->usage, resource_tracker->usage, current_tracker->texture_subresources, command_normalization_barriers, r_command->normalization_barrier_index, r_command->normalization_barrier_count);
					r_command->previous_stages = r_command->previous_stages | current_tracker->usage_stages; // GodotNext: rdg_access_stages (#267)
				}

				current_tracker->in_parent_dirty_list = false;
				current_tracker = current_tracker->next_shared;
			}

			resource_tracker->dirty_shared_list = nullptr;
		}

		// Use the resource's parent tracker directly for all search operations.
		bool resource_has_parent = resource_tracker->parent != nullptr;
		ResourceTracker *search_tracker = resource_has_parent ? resource_tracker->parent : resource_tracker;
		// GodotNext: rd_readonly_depth (#301). Whole depth/stencil textures (and clean slices
		// inheriting a parent already in the shared read-only depth layout) share
		// TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL across sampled reads and read-only depth
		// attachments. Record the transition into that layout in shared_read_transitions instead of
		// marking the first reader as write_usage, so concurrent readers across queues do not wait
		// on one another while still sharing visibility from the prior write.
		const bool is_depth_stencil = _is_depth_stencil_texture(resource_tracker);
		const RDD::TextureLayout prev_layout = resource_tracker->texture_driver_id.id != 0 ? _usage_to_image_layout(resource_tracker->usage, is_depth_stencil) : RDD::TEXTURE_LAYOUT_UNDEFINED;
		const RDD::TextureLayout new_layout = resource_tracker->texture_driver_id.id != 0 ? _usage_to_image_layout(new_resource_usage, is_depth_stencil) : RDD::TEXTURE_LAYOUT_UNDEFINED;
		const bool prev_is_shared_read_only_depth = prev_layout == RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL && !_is_write_usage(resource_tracker->usage) && resource_tracker->usage != RESOURCE_USAGE_NONE;
		const bool shared_read_only_depth_layout = driver_textures_require_layout_transitions && is_depth_stencil && !write_usage && new_layout == RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
				((!resource_has_parent && resource_tracker->dirty_shared_list == nullptr) || (resource_has_parent && !resource_tracker->in_parent_dirty_list && prev_is_shared_read_only_depth && search_tracker->dirty_shared_list == nullptr));
		const bool has_read_visibility = search_tracker->shared_read_transition_index >= 0 ||
				(((uint64_t(resource_tracker->usage_access) & uint64_t(new_usage_access)) == uint64_t(new_usage_access)) &&
						((uint64_t(resource_tracker->usage_stages) & uint64_t(r_command->self_stages)) == uint64_t(r_command->self_stages)));
		const bool same_shared_read_only_depth = shared_read_only_depth_layout && prev_is_shared_read_only_depth && has_read_visibility;
		bool different_usage = resource_tracker->usage != new_resource_usage;
		bool added_shared_read_transition = false;
		if (same_shared_read_only_depth) {
			// Already in the shared read-only depth layout with visibility for this access/stage
			// (or sharing this frame's entry transition): no new barrier, no write_usage, no
			// reader-to-reader edge. Accumulate read accesses and stages on the whole/parent tracker
			// and any active entry transition.
			search_tracker->usage_access = search_tracker->usage_access | new_usage_access;
			search_tracker->usage_stages = search_tracker->usage_stages | r_command->self_stages;
			if (resource_has_parent) {
				resource_tracker->usage_access = search_tracker->usage_access;
				resource_tracker->usage_stages = search_tracker->usage_stages;
			}
			if (search_tracker->shared_read_transition_index >= 0) {
				SharedReadLayoutTransition &transition = shared_read_transitions[search_tracker->shared_read_transition_index];
				transition.barrier.dst_access = transition.barrier.dst_access | new_usage_access;
				transition.dst_stages = transition.dst_stages | r_command->self_stages;
				transition.reader_commands.push_back(p_command_index);
			}
			search_tracker->current_frame_stages = search_tracker->current_frame_stages | r_command->self_stages;
			if (!search_tracker->previous_frame_stages.is_empty()) {
				r_command->previous_stages = r_command->previous_stages | search_tracker->previous_frame_stages;
				search_tracker->previous_frame_stages.clear();
			}
			if (!resource_has_parent) {
				resource_tracker->usage = new_resource_usage;
			}
		} else if (shared_read_only_depth_layout) {
			// First reader entering TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL after a write or
			// UNDEFINED, or a cross-frame reader expanding visibility to a new access/stage scope.
			r_command->previous_stages = r_command->previous_stages | search_tracker->usage_stages;
			if (driver_honors_barriers) {
				SharedReadLayoutTransition transition;
				transition.barrier.texture = search_tracker->texture_driver_id;
				if (prev_layout == RDD::TEXTURE_LAYOUT_UNDEFINED) {
					transition.barrier.src_access = RDD::BarrierAccessBits(0);
				} else if (search_tracker->usage_access.is_empty()) {
					transition.barrier.src_access = RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT;
				} else {
					transition.barrier.src_access = search_tracker->usage_access;
				}
				transition.barrier.dst_access = new_usage_access;
				transition.barrier.prev_layout = prev_layout;
				transition.barrier.next_layout = RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
				transition.barrier.subresources = search_tracker->texture_subresources;
				transition.src_stages = search_tracker->usage_stages | search_tracker->previous_frame_stages;
				transition.dst_stages = r_command->self_stages;
				transition.reader_commands.push_back(p_command_index);
				search_tracker->shared_read_transition_index = int32_t(shared_read_transitions.size());
				shared_read_transitions.push_back(transition);
				added_shared_read_transition = true;
			}
			if (prev_is_shared_read_only_depth) {
				search_tracker->usage_access = search_tracker->usage_access | new_usage_access;
				search_tracker->usage_stages = search_tracker->usage_stages | r_command->self_stages;
			} else {
				search_tracker->usage_access = new_usage_access;
				search_tracker->usage_stages = r_command->self_stages;
			}
			if (resource_has_parent) {
				resource_tracker->usage_access = search_tracker->usage_access;
				resource_tracker->usage_stages = search_tracker->usage_stages;
			}
			search_tracker->current_frame_stages = search_tracker->current_frame_stages | r_command->self_stages;
			if (!search_tracker->previous_frame_stages.is_empty()) {
				r_command->previous_stages = r_command->previous_stages | search_tracker->previous_frame_stages;
				search_tracker->previous_frame_stages.clear();
			}
			if (!resource_has_parent) {
				resource_tracker->usage = new_resource_usage;
			}
		} else {
			bool write_usage_after_write = (write_usage && search_tracker->write_command_or_list_index >= 0);
			// GodotNext: rdg_cross_frame_waw (#262). Or the last write was in an earlier frame.
			write_usage_after_write = write_usage_after_write || cross_frame_write_after_write(resource_tracker->usage, new_resource_usage, !search_tracker->previous_frame_stages.is_empty(), resource_tracker->buffer_driver_id.id != 0);
			const bool accumulate_depth_read = !write_usage && is_depth_stencil && prev_is_shared_read_only_depth && prev_layout == new_layout;
			const bool needs_shared_depth_visibility = accumulate_depth_read && !has_read_visibility;
			if (different_usage || write_usage_after_write || needs_shared_depth_visibility) {
				// A barrier must be pushed if the usage is different of it's a write usage and there was already a command that wrote to this resource previously.
				// GodotNext: rdg_access_stages (#267). The barrier waits on the access it names. That access
				// can be older than this frame and than the command this one depends on: a slice that is
				// first used after the parent's first use in a frame inherits the parent's access from an
				// earlier frame (a texture cleared in one frame and given its mips in the next).
				r_command->previous_stages = r_command->previous_stages | resource_tracker->usage_stages;
				if (resource_tracker->texture_driver_id.id != 0) {
					if (resource_tracker->usage_access.is_empty()) {
						// FIXME: If the tracker does not know the previous type of usage, assume the generic memory write one.
						// Tracking access bits across texture slices can be tricky, so this failsafe can be removed once that's improved.
						resource_tracker->usage_access = RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT;
					}

					_add_texture_barrier_to_command(resource_tracker->texture_driver_id, resource_tracker->usage_access, new_usage_access, resource_tracker->usage, new_resource_usage, resource_tracker->texture_subresources, command_transition_barriers, r_command->transition_barrier_index, r_command->transition_barrier_count);
				} else if (resource_tracker->buffer_driver_id.id != 0) {
#if USE_BUFFER_BARRIERS
					_add_buffer_barrier_to_command(resource_tracker->buffer_driver_id, resource_tracker->usage_access, new_usage_access, r_command->buffer_barrier_index, r_command->buffer_barrier_count);
#endif
					// Memory barriers are pushed regardless of buffer barriers being used or not.
					r_command->memory_barrier.src_access = r_command->memory_barrier.src_access | resource_tracker->usage_access;
					r_command->memory_barrier.dst_access = r_command->memory_barrier.dst_access | new_usage_access;
				} else if (resource_tracker->acceleration_structure_driver_id.id != 0) {
					// Make sure the acceleration structure has been built before accessing it from raytracing shaders.
					_add_acceleration_structure_barrier_to_command(resource_tracker->acceleration_structure_driver_id, resource_tracker->usage_access, new_usage_access, command_acceleration_structure_barriers, r_command->acceleration_structure_barrier_index, r_command->acceleration_structure_barrier_count);
					r_command->memory_barrier.src_access = r_command->memory_barrier.src_access | resource_tracker->usage_access;
					r_command->memory_barrier.dst_access = r_command->memory_barrier.dst_access | new_usage_access;
				} else {
					DEV_ASSERT(false && "Resource tracker does not contain a valid buffer or texture ID.");
				}
			}

			// Always update the access of the tracker according to the latest usage.
			if (accumulate_depth_read) {
				resource_tracker->usage_access = resource_tracker->usage_access | new_usage_access;
				resource_tracker->usage_stages = resource_tracker->usage_stages | r_command->self_stages;
			} else {
				resource_tracker->usage_access = new_usage_access;
				resource_tracker->usage_stages = r_command->self_stages; // GodotNext: rdg_access_stages (#267)
			}

			// Always accumulate the stages of the tracker with the commands that use it.
			search_tracker->current_frame_stages = search_tracker->current_frame_stages | r_command->self_stages;

			if (!search_tracker->previous_frame_stages.is_empty()) {
				// Add to the command the stages the tracker was used on in the previous frame.
				r_command->previous_stages = r_command->previous_stages | search_tracker->previous_frame_stages;
				search_tracker->previous_frame_stages.clear();
			}

			if (different_usage) {
				// Even if the usage of the resource isn't a write usage explicitly, a different usage implies a transition and it should therefore be considered a write.
				// However, this is not necessary if the driver does not consider different usages as different states (e.g. no image layouts).
				write_usage = write_usage || (driver_textures_require_layout_transitions && bool(resource_tracker->texture_driver_id)) || driver_buffers_require_transitions;
				resource_tracker->usage = new_resource_usage;
			}
		}

		if (write_usage) {
			// GodotNext: rd_readonly_depth (#301). A write closes any active shared read-only depth window.
			search_tracker->shared_read_transition_index = -1;
		}

		bool write_usage_has_partial_coverage = !different_usage && _check_command_partial_coverage(resource_tracker, p_command_index);
		if (search_tracker->write_command_or_list_index >= 0) {
			if (search_tracker->write_command_list_enabled) {
				// Make this command adjacent to any commands that wrote to this resource and intersect with the slice if it applies.
				// For buffers or textures that never use slices, this list will only be one element long at most.
				int32_t previous_write_list_index = -1;
				int32_t write_list_index = search_tracker->write_command_or_list_index;
				while (write_list_index >= 0) {
					const RecordedSliceListNode &write_list_node = write_slice_list_nodes[write_list_index];
					if (!resource_has_parent || search_tracker_rect.intersects(write_list_node.subresources)) {
						if (write_list_node.command_index == p_command_index) {
							ERR_FAIL_COND_MSG(!resource_has_parent, "Command can't have itself as a dependency.");
						} else if (!write_list_node.partial_coverage || _check_command_intersection(resource_tracker, write_list_node.command_index, p_command_index)) {
							_check_discardable_attachment_dependency(search_tracker, write_list_node.command_index, p_command_index);

							// Command is dependent on this command. Add this command to the adjacency list of the write command.
							_add_adjacent_command(write_list_node.command_index, p_command_index, r_command);

							if (resource_has_parent && write_usage && search_tracker_rect.encloses(write_list_node.subresources) && !write_usage_has_partial_coverage) {
								// Eliminate redundant writes from the list.
								if (previous_write_list_index >= 0) {
									RecordedSliceListNode &previous_list_node = write_slice_list_nodes[previous_write_list_index];
									previous_list_node.next_list_index = write_list_node.next_list_index;
								} else {
									search_tracker->write_command_or_list_index = write_list_node.next_list_index;
								}

								write_list_index = write_list_node.next_list_index;
								continue;
							}
						}
					}

					previous_write_list_index = write_list_index;
					write_list_index = write_list_node.next_list_index;
				}
			} else {
				// The index is just the latest command index that wrote to the resource.
				if (search_tracker->write_command_or_list_index == p_command_index) {
					ERR_FAIL_MSG("Command can't have itself as a dependency.");
				} else {
					_check_discardable_attachment_dependency(search_tracker, search_tracker->write_command_or_list_index, p_command_index);
					_add_adjacent_command(search_tracker->write_command_or_list_index, p_command_index, r_command);
				}
			}
		}

		if (write_usage) {
			bool use_write_list = resource_has_parent || write_usage_has_partial_coverage;
			if (use_write_list) {
				if (!search_tracker->write_command_list_enabled && search_tracker->write_command_or_list_index >= 0) {
					// Write command list was not being used but there was a write command recorded. Add a new node with the entire parent resource's subresources and the recorded command index to the list.
					const RDD::TextureSubresourceRange &tracker_subresources = search_tracker->texture_subresources;
					Rect2i tracker_rect(tracker_subresources.base_mipmap, tracker_subresources.base_layer, tracker_subresources.mipmap_count, tracker_subresources.layer_count);
					search_tracker->write_command_or_list_index = _add_to_write_list(search_tracker->write_command_or_list_index, tracker_rect, -1, false);
				}

				search_tracker->write_command_or_list_index = _add_to_write_list(p_command_index, search_tracker_rect, search_tracker->write_command_or_list_index, write_usage_has_partial_coverage);
				search_tracker->write_command_list_enabled = true;
			} else {
				search_tracker->write_command_or_list_index = p_command_index;
				search_tracker->write_command_list_enabled = false;
			}

			// We add this command to the adjacency list of all commands that were reading from the entire resource.
			int32_t read_full_command_list_index = search_tracker->read_full_command_list_index;
			while (read_full_command_list_index >= 0) {
				int32_t read_full_command_index = command_list_nodes[read_full_command_list_index].command_index;
				int32_t read_full_next_index = command_list_nodes[read_full_command_list_index].next_list_index;
				if (read_full_command_index == p_command_index) {
					if (!resource_has_parent) {
						// Only slices are allowed to be in different usages in the same command as they are guaranteed to have no overlap in the same command.
						ERR_FAIL_MSG("Command can't have itself as a dependency.");
					}
				} else {
					// Add this command to the adjacency list of each command that was reading this resource.
					_add_adjacent_command(read_full_command_index, p_command_index, r_command);
				}

				read_full_command_list_index = read_full_next_index;
			}

			if (!use_write_list) {
				// Clear the full list if this resource is not a slice.
				search_tracker->read_full_command_list_index = -1;
			}

			// We add this command to the adjacency list of all commands that were reading from resource slices.
			int32_t previous_slice_command_list_index = -1;
			int32_t read_slice_command_list_index = search_tracker->read_slice_command_list_index;
			while (read_slice_command_list_index >= 0) {
				const RecordedSliceListNode &read_list_node = read_slice_list_nodes[read_slice_command_list_index];
				if (!use_write_list || search_tracker_rect.encloses(read_list_node.subresources)) {
					if (previous_slice_command_list_index >= 0) {
						// Erase this element and connect the previous one to the next element.
						read_slice_list_nodes[previous_slice_command_list_index].next_list_index = read_list_node.next_list_index;
					} else {
						// Erase this element from the head of the list.
						DEV_ASSERT(search_tracker->read_slice_command_list_index == read_slice_command_list_index);
						search_tracker->read_slice_command_list_index = read_list_node.next_list_index;
					}

					// Advance to the next element.
					read_slice_command_list_index = read_list_node.next_list_index;
				} else {
					previous_slice_command_list_index = read_slice_command_list_index;
					read_slice_command_list_index = read_list_node.next_list_index;
				}

				if (!resource_has_parent || search_tracker_rect.intersects(read_list_node.subresources)) {
					// Add this command to the adjacency list of each command that was reading this resource.
					// We only add the dependency if there's an intersection between slices or this resource isn't a slice.
					_add_adjacent_command(read_list_node.command_index, p_command_index, r_command);
				}
			}
		} else if (resource_has_parent) {
			// We add a read dependency to the tracker to indicate this command reads from the resource slice.
			search_tracker->read_slice_command_list_index = _add_to_slice_read_list(p_command_index, resource_tracker_rect, search_tracker->read_slice_command_list_index);
		} else {
			// We add a read dependency to the tracker to indicate this command reads from the entire resource.
			search_tracker->read_full_command_list_index = _add_to_command_list(p_command_index, search_tracker->read_full_command_list_index);
		}

		// GodotNext: async_compute (#174) & px_rd_async_multiview (#300). A texture barrier the async
		// queue cannot run moves before the fork (BARRIER_SELECTION_ASYNC_FORK). That keeps the order
		// while its source access on the barrier's subresources is still on the main queue. A barrier
		// on one view's slice does not conflict when an earlier async command of the frame only used a
		// disjoint slice of the same parent texture; a barrier whose subresources intersect an earlier
		// async command's subresources (or a whole-resource async use) still falls back to main queue.
		if (r_command->queue == QUEUE_ASYNC_COMPUTE && driver_textures_require_layout_transitions) {
			const Rect2i parent_rect(search_tracker->texture_subresources.base_mipmap, search_tracker->texture_subresources.base_layer,
					search_tracker->texture_subresources.mipmap_count, search_tracker->texture_subresources.layer_count);
			// GodotNext: rd_readonly_depth (#301). A shared read-only depth transition covers the
			// parent's subresources and moves to the fork like any unsupported async texture barrier.
			const bool shared_read_conflict = added_shared_read_transition && _async_subresources_used_by_earlier_command(search_tracker, p_command_index, parent_rect);
			if (shared_read_conflict || _async_fork_texture_barriers_conflict(search_tracker, p_command_index, r_command, async_normalization_start, async_transition_start)) {
				async_fork_barrier_conflict = true;
			}
			if (resource_has_parent) {
				search_tracker->async_slice_command_list_index = _add_to_async_slice_list(p_command_index, search_tracker_rect, search_tracker->async_slice_command_list_index);
			} else {
				search_tracker->async_full_command_index = p_command_index;
			}
		}
	}
}

void RenderingDeviceGraph::_add_texture_barrier_to_command(RDD::TextureID p_texture_id, BitField<RDD::BarrierAccessBits> p_src_access, BitField<RDD::BarrierAccessBits> p_dst_access, ResourceUsage p_prev_usage, ResourceUsage p_next_usage, RDD::TextureSubresourceRange p_subresources, LocalVector<RDD::TextureBarrier> &r_barrier_vector, int32_t &r_barrier_index, int32_t &r_barrier_count) {
	if (!driver_honors_barriers) {
		return;
	}

	if (r_barrier_index < 0) {
		r_barrier_index = r_barrier_vector.size();
	}

	// GodotNext: rd_readonly_depth (#301). Derive depth/stencil aspect so sampled depth reads map
	// to TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL across whole textures and slices alike.
	const bool is_depth_stencil = p_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_DEPTH_BIT) || p_subresources.aspect.has_flag(RDD::TEXTURE_ASPECT_STENCIL_BIT);
	RDD::TextureBarrier texture_barrier;
	texture_barrier.texture = p_texture_id;
	texture_barrier.src_access = p_src_access;
	texture_barrier.dst_access = p_dst_access;
	texture_barrier.prev_layout = _usage_to_image_layout(p_prev_usage, is_depth_stencil);
	texture_barrier.next_layout = _usage_to_image_layout(p_next_usage, is_depth_stencil);
	texture_barrier.subresources = p_subresources;
	r_barrier_vector.push_back(texture_barrier);
	r_barrier_count++;
}

#if USE_BUFFER_BARRIERS
void RenderingDeviceGraph::_add_buffer_barrier_to_command(RDD::BufferID p_buffer_id, BitField<RDD::BarrierAccessBits> p_src_access, BitField<RDD::BarrierAccessBits> p_dst_access, int32_t &r_barrier_index, int32_t &r_barrier_count) {
	if (!driver_honors_barriers) {
		return;
	}

	if (r_barrier_index < 0) {
		r_barrier_index = command_buffer_barriers.size();
	}

	RDD::BufferBarrier buffer_barrier;
	buffer_barrier.buffer = p_buffer_id;
	buffer_barrier.src_access = p_src_access;
	buffer_barrier.dst_access = p_dst_access;
	buffer_barrier.offset = 0;
	buffer_barrier.size = RDD::BUFFER_WHOLE_SIZE;
	command_buffer_barriers.push_back(buffer_barrier);
	r_barrier_count++;
}
#endif

void RenderingDeviceGraph::_add_acceleration_structure_barrier_to_command(RDD::AccelerationStructureID p_acceleration_structure_id, BitField<RDD::BarrierAccessBits> p_src_access, BitField<RDD::BarrierAccessBits> p_dst_access, LocalVector<RDD::AccelerationStructureBarrier> &r_barrier_vector, int32_t &r_barrier_index, int32_t &r_barrier_count) {
	if (!driver_honors_barriers) {
		return;
	}

	if (r_barrier_index < 0) {
		r_barrier_index = r_barrier_vector.size();
	}

	RDD::AccelerationStructureBarrier accel_barrier;
	accel_barrier.acceleration_structure = p_acceleration_structure_id;
	accel_barrier.src_access = p_src_access;
	accel_barrier.dst_access = p_dst_access;
	accel_barrier.offset = 0;
	accel_barrier.size = RDD::BUFFER_WHOLE_SIZE;
	r_barrier_vector.push_back(accel_barrier);
	r_barrier_count++;
}

void RenderingDeviceGraph::_run_raytracing_list_command(RDD::CommandBufferID p_command_buffer, const uint8_t *p_instruction_data, uint32_t p_instruction_data_size) {
	uint32_t instruction_data_cursor = 0;
	while (instruction_data_cursor < p_instruction_data_size) {
		DEV_ASSERT((instruction_data_cursor + sizeof(RaytracingListInstruction)) <= p_instruction_data_size);

		const RaytracingListInstruction *instruction = reinterpret_cast<const RaytracingListInstruction *>(&p_instruction_data[instruction_data_cursor]);
		switch (instruction->type) {
			case RaytracingListInstruction::TYPE_BIND_PIPELINE: {
				const RaytracingListBindPipelineInstruction *bind_pipeline_instruction = reinterpret_cast<const RaytracingListBindPipelineInstruction *>(instruction);
				driver->command_bind_raytracing_pipeline(p_command_buffer, bind_pipeline_instruction->pipeline);
				instruction_data_cursor += sizeof(RaytracingListBindPipelineInstruction);
			} break;
			case RaytracingListInstruction::TYPE_BIND_UNIFORM_SET: {
				const RaytracingListBindUniformSetInstruction *bind_uniform_set_instruction = reinterpret_cast<const RaytracingListBindUniformSetInstruction *>(instruction);
				driver->command_bind_raytracing_uniform_set(p_command_buffer, bind_uniform_set_instruction->uniform_set, bind_uniform_set_instruction->shader, bind_uniform_set_instruction->set_index);
				instruction_data_cursor += sizeof(RaytracingListBindUniformSetInstruction);
			} break;
			case RaytracingListInstruction::TYPE_TRACE_RAYS: {
				const RaytracingListTraceRaysInstruction *trace_rays_instruction = reinterpret_cast<const RaytracingListTraceRaysInstruction *>(instruction);
				driver->command_trace_rays(p_command_buffer, trace_rays_instruction->raygen_sbt, trace_rays_instruction->miss_sbt, trace_rays_instruction->hit_sbt, trace_rays_instruction->width, trace_rays_instruction->height, trace_rays_instruction->depth);
				instruction_data_cursor += sizeof(RaytracingListTraceRaysInstruction);
			} break;
			// GodotNext: rd_rt_pipelines (#179)
			case RaytracingListInstruction::TYPE_TRACE_RAYS_INDIRECT: {
				const RaytracingListTraceRaysIndirectInstruction *trace_rays_indirect_instruction = reinterpret_cast<const RaytracingListTraceRaysIndirectInstruction *>(instruction);
				driver->command_trace_rays_indirect(p_command_buffer, trace_rays_indirect_instruction->raygen_sbt, trace_rays_indirect_instruction->miss_sbt, trace_rays_indirect_instruction->hit_sbt, trace_rays_indirect_instruction->buffer, trace_rays_indirect_instruction->offset);
				instruction_data_cursor += sizeof(RaytracingListTraceRaysIndirectInstruction);
			} break;
			case RaytracingListInstruction::TYPE_SET_PUSH_CONSTANT: {
				const RaytracingListSetPushConstantInstruction *set_push_constant_instruction = reinterpret_cast<const RaytracingListSetPushConstantInstruction *>(instruction);
				const VectorView push_constant_data_view(reinterpret_cast<const uint32_t *>(set_push_constant_instruction->data()), set_push_constant_instruction->size / sizeof(uint32_t));
				driver->command_bind_push_constants(p_command_buffer, set_push_constant_instruction->shader, 0, push_constant_data_view);
				instruction_data_cursor += sizeof(RaytracingListSetPushConstantInstruction);
				instruction_data_cursor += set_push_constant_instruction->size;
			} break;
			case RaytracingListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE: {
				const RaytracingListUniformSetPrepareForUseInstruction *uniform_set_prepare_for_use_instruction = reinterpret_cast<const RaytracingListUniformSetPrepareForUseInstruction *>(instruction);
				driver->command_uniform_set_prepare_for_use(p_command_buffer, uniform_set_prepare_for_use_instruction->uniform_set, uniform_set_prepare_for_use_instruction->shader, uniform_set_prepare_for_use_instruction->set_index);
				instruction_data_cursor += sizeof(RaytracingListUniformSetPrepareForUseInstruction);
			} break;
			default:
				DEV_ASSERT(false && "Unknown raytracing list instruction type.");
				return;
		}
	}
}

void RenderingDeviceGraph::_run_compute_list_command(RDD::CommandBufferID p_command_buffer, const uint8_t *p_instruction_data, uint32_t p_instruction_data_size) {
	uint32_t instruction_data_cursor = 0;
	while (instruction_data_cursor < p_instruction_data_size) {
		DEV_ASSERT((instruction_data_cursor + sizeof(ComputeListInstruction)) <= p_instruction_data_size);

		const ComputeListInstruction *instruction = reinterpret_cast<const ComputeListInstruction *>(&p_instruction_data[instruction_data_cursor]);
		switch (instruction->type) {
			case ComputeListInstruction::TYPE_BIND_PIPELINE: {
				const ComputeListBindPipelineInstruction *bind_pipeline_instruction = reinterpret_cast<const ComputeListBindPipelineInstruction *>(instruction);
				driver->command_bind_compute_pipeline(p_command_buffer, bind_pipeline_instruction->pipeline);
				instruction_data_cursor += sizeof(ComputeListBindPipelineInstruction);
			} break;
			case ComputeListInstruction::TYPE_BIND_UNIFORM_SETS: {
				const ComputeListBindUniformSetsInstruction *bind_uniform_sets_instruction = reinterpret_cast<const ComputeListBindUniformSetsInstruction *>(instruction);
				driver->command_bind_compute_uniform_sets(p_command_buffer, VectorView<RDD::UniformSetID>(bind_uniform_sets_instruction->uniform_set_ids(), bind_uniform_sets_instruction->set_count), bind_uniform_sets_instruction->shader, bind_uniform_sets_instruction->first_set_index, bind_uniform_sets_instruction->set_count, bind_uniform_sets_instruction->dynamic_offsets_mask);
				instruction_data_cursor += sizeof(ComputeListBindUniformSetsInstruction) + sizeof(RDD::UniformSetID) * bind_uniform_sets_instruction->set_count;
			} break;
			case ComputeListInstruction::TYPE_DISPATCH: {
				const ComputeListDispatchInstruction *dispatch_instruction = reinterpret_cast<const ComputeListDispatchInstruction *>(instruction);
				driver->command_compute_dispatch(p_command_buffer, dispatch_instruction->x_groups, dispatch_instruction->y_groups, dispatch_instruction->z_groups);
				instruction_data_cursor += sizeof(ComputeListDispatchInstruction);
			} break;
			case ComputeListInstruction::TYPE_DISPATCH_INDIRECT: {
				const ComputeListDispatchIndirectInstruction *dispatch_indirect_instruction = reinterpret_cast<const ComputeListDispatchIndirectInstruction *>(instruction);
				driver->command_compute_dispatch_indirect(p_command_buffer, dispatch_indirect_instruction->buffer, dispatch_indirect_instruction->offset);
				instruction_data_cursor += sizeof(ComputeListDispatchIndirectInstruction);
			} break;
			case ComputeListInstruction::TYPE_SET_PUSH_CONSTANT: {
				const ComputeListSetPushConstantInstruction *set_push_constant_instruction = reinterpret_cast<const ComputeListSetPushConstantInstruction *>(instruction);
				const VectorView push_constant_data_view(reinterpret_cast<const uint32_t *>(set_push_constant_instruction->data()), set_push_constant_instruction->size / sizeof(uint32_t));
				driver->command_bind_push_constants(p_command_buffer, set_push_constant_instruction->shader, 0, push_constant_data_view);
				instruction_data_cursor += sizeof(ComputeListSetPushConstantInstruction);
				instruction_data_cursor += set_push_constant_instruction->size;
			} break;
			case ComputeListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE: {
				const ComputeListUniformSetPrepareForUseInstruction *uniform_set_prepare_for_use_instruction = reinterpret_cast<const ComputeListUniformSetPrepareForUseInstruction *>(instruction);
				driver->command_uniform_set_prepare_for_use(p_command_buffer, uniform_set_prepare_for_use_instruction->uniform_set, uniform_set_prepare_for_use_instruction->shader, uniform_set_prepare_for_use_instruction->set_index);
				instruction_data_cursor += sizeof(ComputeListUniformSetPrepareForUseInstruction);
			} break;
			default:
				DEV_ASSERT(false && "Unknown compute list instruction type.");
				return;
		}

		instruction_data_cursor = GRAPH_ALIGN(instruction_data_cursor);
	}
}

void RenderingDeviceGraph::_get_draw_list_render_pass_and_framebuffer(const RecordedDrawListCommand *p_draw_list_command, RDD::RenderPassID &r_render_pass, RDD::FramebufferID &r_framebuffer) {
	// GodotNext: rd_readonly_depth (#301). Pack 4 bits per attachment (2 load, 2 store) into two
	// 64-bit words so ATTACHMENT_STORE_OP_NONE never collides with neighboring attachments.
	DEV_ASSERT(p_draw_list_command->trackers_count <= 32 && "Max number of attachments that can be encoded into the key.");

	// Build a unique key from the load and store ops for each attachment.
	const RDD::AttachmentLoadOp *load_ops = p_draw_list_command->load_ops();
	const RDD::AttachmentStoreOp *store_ops = p_draw_list_command->store_ops();
	FramebufferStorageKey key;
	for (uint32_t i = 0; i < p_draw_list_command->trackers_count; i++) {
		const uint64_t bits = uint64_t(load_ops[i]) | (uint64_t(store_ops[i]) << 2);
		if (i < 16) {
			key.lo |= bits << (i * 4);
		} else {
			key.hi |= bits << ((i - 16) * 4);
		}
	}

	// Check the storage map if the render pass and the framebuffer needs to be created.
	FramebufferCache *framebuffer_cache = p_draw_list_command->framebuffer_cache;
	HashMap<FramebufferStorageKey, FramebufferStorage, FramebufferStorageKey>::Iterator it = framebuffer_cache->storage_map.find(key);
	if (it == framebuffer_cache->storage_map.end()) {
		FramebufferStorage storage;
		VectorView<RDD::AttachmentLoadOp> load_ops_view(load_ops, p_draw_list_command->trackers_count);
		VectorView<RDD::AttachmentStoreOp> store_ops_view(store_ops, p_draw_list_command->trackers_count);
		storage.render_pass = render_pass_creation_function(driver, load_ops_view, store_ops_view, framebuffer_cache->render_pass_creation_user_data);
		ERR_FAIL_COND(!storage.render_pass);

		storage.framebuffer = driver->framebuffer_create(storage.render_pass, framebuffer_cache->textures, framebuffer_cache->width, framebuffer_cache->height);
		ERR_FAIL_COND(!storage.framebuffer);

		it = framebuffer_cache->storage_map.insert(key, storage);
	}

	r_render_pass = it->value.render_pass;
	r_framebuffer = it->value.framebuffer;
}

#if PRINT_DRAW_LIST_STATS
static uint32_t draw_list_total_size = 0;
#endif

void RenderingDeviceGraph::_run_draw_list_command(RDD::CommandBufferID p_command_buffer, const uint8_t *p_instruction_data, uint32_t p_instruction_data_size) {
#if PRINT_DRAW_LIST_STATS
	draw_list_total_size += p_instruction_data_size;
#endif

	uint32_t instruction_data_cursor = 0;
	while (instruction_data_cursor < p_instruction_data_size) {
		DEV_ASSERT((instruction_data_cursor + sizeof(DrawListInstruction)) <= p_instruction_data_size);

		const DrawListInstruction *instruction = reinterpret_cast<const DrawListInstruction *>(&p_instruction_data[instruction_data_cursor]);
		switch (instruction->type) {
			case DrawListInstruction::TYPE_BIND_INDEX_BUFFER: {
				const DrawListBindIndexBufferInstruction *bind_index_buffer_instruction = reinterpret_cast<const DrawListBindIndexBufferInstruction *>(instruction);
				driver->command_render_bind_index_buffer(p_command_buffer, bind_index_buffer_instruction->buffer, bind_index_buffer_instruction->format, bind_index_buffer_instruction->offset);
				instruction_data_cursor += sizeof(DrawListBindIndexBufferInstruction);
			} break;
			case DrawListInstruction::TYPE_BIND_PIPELINE: {
				const DrawListBindPipelineInstruction *bind_pipeline_instruction = reinterpret_cast<const DrawListBindPipelineInstruction *>(instruction);
				driver->command_bind_render_pipeline(p_command_buffer, bind_pipeline_instruction->pipeline);
				instruction_data_cursor += sizeof(DrawListBindPipelineInstruction);
			} break;
			case DrawListInstruction::TYPE_BIND_UNIFORM_SETS: {
				const DrawListBindUniformSetsInstruction *bind_uniform_sets_instruction = reinterpret_cast<const DrawListBindUniformSetsInstruction *>(instruction);
				driver->command_bind_render_uniform_sets(p_command_buffer, VectorView<RDD::UniformSetID>(bind_uniform_sets_instruction->uniform_set_ids(), bind_uniform_sets_instruction->set_count), bind_uniform_sets_instruction->shader, bind_uniform_sets_instruction->first_set_index, bind_uniform_sets_instruction->set_count, bind_uniform_sets_instruction->dynamic_offsets_mask);
				instruction_data_cursor += sizeof(DrawListBindUniformSetsInstruction) + sizeof(RDD::UniformSetID) * bind_uniform_sets_instruction->set_count;
			} break;
			case DrawListInstruction::TYPE_BIND_VERTEX_BUFFERS: {
				const DrawListBindVertexBuffersInstruction *bind_vertex_buffers_instruction = reinterpret_cast<const DrawListBindVertexBuffersInstruction *>(instruction);
				driver->command_render_bind_vertex_buffers(p_command_buffer, bind_vertex_buffers_instruction->vertex_buffers_count, bind_vertex_buffers_instruction->vertex_buffers(), bind_vertex_buffers_instruction->vertex_buffer_offsets(), bind_vertex_buffers_instruction->dynamic_offsets_mask);
				instruction_data_cursor += sizeof(DrawListBindVertexBuffersInstruction);
				instruction_data_cursor += sizeof(RDD::BufferID) * bind_vertex_buffers_instruction->vertex_buffers_count;
				instruction_data_cursor += sizeof(uint64_t) * bind_vertex_buffers_instruction->vertex_buffers_count;
			} break;
			case DrawListInstruction::TYPE_CLEAR_ATTACHMENTS: {
				const DrawListClearAttachmentsInstruction *clear_attachments_instruction = reinterpret_cast<const DrawListClearAttachmentsInstruction *>(instruction);
				const VectorView attachments_clear_view(clear_attachments_instruction->attachments_clear(), clear_attachments_instruction->attachments_clear_count);
				const VectorView attachments_clear_rect_view(clear_attachments_instruction->attachments_clear_rect(), clear_attachments_instruction->attachments_clear_rect_count);
				driver->command_render_clear_attachments(p_command_buffer, attachments_clear_view, attachments_clear_rect_view);
				instruction_data_cursor += sizeof(DrawListClearAttachmentsInstruction);
				instruction_data_cursor += sizeof(RDD::AttachmentClear) * clear_attachments_instruction->attachments_clear_count;
				instruction_data_cursor += sizeof(Rect2i) * clear_attachments_instruction->attachments_clear_rect_count;
			} break;
			case DrawListInstruction::TYPE_DRAW: {
				const DrawListDrawInstruction *draw_instruction = reinterpret_cast<const DrawListDrawInstruction *>(instruction);
				driver->command_render_draw(p_command_buffer, draw_instruction->vertex_count, draw_instruction->instance_count, 0, 0);
				instruction_data_cursor += sizeof(DrawListDrawInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDEXED: {
				const DrawListDrawIndexedInstruction *draw_indexed_instruction = reinterpret_cast<const DrawListDrawIndexedInstruction *>(instruction);
				driver->command_render_draw_indexed(p_command_buffer, draw_indexed_instruction->index_count, draw_indexed_instruction->instance_count, draw_indexed_instruction->first_index, 0, 0);
				instruction_data_cursor += sizeof(DrawListDrawIndexedInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDIRECT: {
				const DrawListDrawIndirectInstruction *draw_indirect_instruction = reinterpret_cast<const DrawListDrawIndirectInstruction *>(instruction);
				driver->command_render_draw_indirect(p_command_buffer, draw_indirect_instruction->buffer, draw_indirect_instruction->offset, draw_indirect_instruction->draw_count, draw_indirect_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT: {
				const DrawListDrawIndexedIndirectInstruction *draw_indexed_indirect_instruction = reinterpret_cast<const DrawListDrawIndexedIndirectInstruction *>(instruction);
				driver->command_render_draw_indexed_indirect(p_command_buffer, draw_indexed_indirect_instruction->buffer, draw_indexed_indirect_instruction->offset, draw_indexed_indirect_instruction->draw_count, draw_indexed_indirect_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndexedIndirectInstruction);
			} break;
			// GodotNext: rd_indirect_count (#177)
			case DrawListInstruction::TYPE_DRAW_INDIRECT_COUNT: {
				const DrawListDrawIndirectCountInstruction *draw_instruction = reinterpret_cast<const DrawListDrawIndirectCountInstruction *>(instruction);
				driver->command_render_draw_indirect_count(p_command_buffer, draw_instruction->buffer, draw_instruction->offset, draw_instruction->count_buffer, draw_instruction->count_buffer_offset, draw_instruction->max_draw_count, draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectCountInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT_COUNT: {
				const DrawListDrawIndirectCountInstruction *draw_instruction = reinterpret_cast<const DrawListDrawIndirectCountInstruction *>(instruction);
				driver->command_render_draw_indexed_indirect_count(p_command_buffer, draw_instruction->buffer, draw_instruction->offset, draw_instruction->count_buffer, draw_instruction->count_buffer_offset, draw_instruction->max_draw_count, draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectCountInstruction);
			} break;
			// GodotNext: rd_mesh_shaders (#178)
			case DrawListInstruction::TYPE_DRAW_MESH_TASKS: {
				const DrawListDrawMeshTasksInstruction *draw_instruction = reinterpret_cast<const DrawListDrawMeshTasksInstruction *>(instruction);
				driver->command_render_draw_mesh_tasks(p_command_buffer, draw_instruction->group_count[0], draw_instruction->group_count[1], draw_instruction->group_count[2]);
				instruction_data_cursor += sizeof(DrawListDrawMeshTasksInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT: {
				const DrawListDrawMeshTasksIndirectInstruction *draw_instruction = reinterpret_cast<const DrawListDrawMeshTasksIndirectInstruction *>(instruction);
				driver->command_render_draw_mesh_tasks_indirect(p_command_buffer, draw_instruction->buffer, draw_instruction->offset, draw_instruction->draw_count, draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawMeshTasksIndirectInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT_COUNT: {
				const DrawListDrawIndirectCountInstruction *draw_instruction = reinterpret_cast<const DrawListDrawIndirectCountInstruction *>(instruction);
				driver->command_render_draw_mesh_tasks_indirect_count(p_command_buffer, draw_instruction->buffer, draw_instruction->offset, draw_instruction->count_buffer, draw_instruction->count_buffer_offset, draw_instruction->max_draw_count, draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectCountInstruction);
			} break;
			case DrawListInstruction::TYPE_EXECUTE_COMMANDS: {
				const DrawListExecuteCommandsInstruction *execute_commands_instruction = reinterpret_cast<const DrawListExecuteCommandsInstruction *>(instruction);
				driver->command_buffer_execute_secondary(p_command_buffer, execute_commands_instruction->command_buffer);
				instruction_data_cursor += sizeof(DrawListExecuteCommandsInstruction);
			} break;
			case DrawListInstruction::TYPE_NEXT_SUBPASS: {
				const DrawListNextSubpassInstruction *next_subpass_instruction = reinterpret_cast<const DrawListNextSubpassInstruction *>(instruction);
				driver->command_next_render_subpass(p_command_buffer, next_subpass_instruction->command_buffer_type);
				instruction_data_cursor += sizeof(DrawListNextSubpassInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_BLEND_CONSTANTS: {
				const DrawListSetBlendConstantsInstruction *set_blend_constants_instruction = reinterpret_cast<const DrawListSetBlendConstantsInstruction *>(instruction);
				driver->command_render_set_blend_constants(p_command_buffer, set_blend_constants_instruction->color);
				instruction_data_cursor += sizeof(DrawListSetBlendConstantsInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_LINE_WIDTH: {
				const DrawListSetLineWidthInstruction *set_line_width_instruction = reinterpret_cast<const DrawListSetLineWidthInstruction *>(instruction);
				driver->command_render_set_line_width(p_command_buffer, set_line_width_instruction->width);
				instruction_data_cursor += sizeof(DrawListSetLineWidthInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_PUSH_CONSTANT: {
				const DrawListSetPushConstantInstruction *set_push_constant_instruction = reinterpret_cast<const DrawListSetPushConstantInstruction *>(instruction);
				const VectorView push_constant_data_view(reinterpret_cast<const uint32_t *>(set_push_constant_instruction->data()), set_push_constant_instruction->size / sizeof(uint32_t));
				driver->command_bind_push_constants(p_command_buffer, set_push_constant_instruction->shader, 0, push_constant_data_view);
				instruction_data_cursor += sizeof(DrawListSetPushConstantInstruction);
				instruction_data_cursor += set_push_constant_instruction->size;
			} break;
			case DrawListInstruction::TYPE_SET_SCISSOR: {
				const DrawListSetScissorInstruction *set_scissor_instruction = reinterpret_cast<const DrawListSetScissorInstruction *>(instruction);
				driver->command_render_set_scissor(p_command_buffer, set_scissor_instruction->rect);
				instruction_data_cursor += sizeof(DrawListSetScissorInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_VIEWPORT: {
				const DrawListSetViewportInstruction *set_viewport_instruction = reinterpret_cast<const DrawListSetViewportInstruction *>(instruction);
				driver->command_render_set_viewport(p_command_buffer, set_viewport_instruction->rect);
				instruction_data_cursor += sizeof(DrawListSetViewportInstruction);
			} break;
			case DrawListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE: {
				const DrawListUniformSetPrepareForUseInstruction *uniform_set_prepare_for_use_instruction = reinterpret_cast<const DrawListUniformSetPrepareForUseInstruction *>(instruction);
				driver->command_uniform_set_prepare_for_use(p_command_buffer, uniform_set_prepare_for_use_instruction->uniform_set, uniform_set_prepare_for_use_instruction->shader, uniform_set_prepare_for_use_instruction->set_index);
				instruction_data_cursor += sizeof(DrawListUniformSetPrepareForUseInstruction);
			} break;
			default:
				DEV_ASSERT(false && "Unknown draw list instruction type.");
				return;
		}

		instruction_data_cursor = GRAPH_ALIGN(instruction_data_cursor);
	}
}

// GodotNext: rd_clear_values (#275)
void RenderingDeviceGraph::draw_list_driver_clear_values(const FramebufferCache *p_framebuffer_cache, VectorView<RDD::RenderPassClearValue> p_slot_clear_values, LocalVector<RDD::RenderPassClearValue> &r_clear_values) {
	r_clear_values.clear();
	for (uint32_t i = 0; i < p_slot_clear_values.size(); i++) {
		if (p_framebuffer_cache != nullptr && (i >= p_framebuffer_cache->trackers.size() || p_framebuffer_cache->trackers[i] == nullptr)) {
			continue;
		}
		r_clear_values.push_back(p_slot_clear_values[i]);
	}
	// The driver framebuffer holds the slots that have a tracker (RenderingDevice::framebuffer_create_multipass()).
	DEV_ASSERT(p_framebuffer_cache == nullptr || r_clear_values.size() == p_framebuffer_cache->textures.size());
}

void RenderingDeviceGraph::_add_draw_list_begin(FramebufferCache *p_framebuffer_cache, RDD::RenderPassID p_render_pass, RDD::FramebufferID p_framebuffer, Rect2i p_region, VectorView<AttachmentOperation> p_attachment_operations, VectorView<RDD::RenderPassClearValue> p_attachment_clear_values, BitField<RDD::PipelineStageBits> p_stages, uint32_t p_breadcrumb, bool p_split_cmd_buffer) {
	DEV_ASSERT(p_attachment_operations.size() == p_attachment_clear_values.size());

	draw_instruction_list.clear();
	draw_instruction_list.indirect_count_offsets.clear(); // GodotNext: rd_indirect_count (#177)
	draw_instruction_list.bindless_readonly_depth_trackers.clear(); // GodotNext: rd_readonly_depth (#301)
	draw_instruction_list.index++;
	draw_instruction_list.framebuffer_cache = p_framebuffer_cache;
	draw_instruction_list.render_pass = p_render_pass;
	draw_instruction_list.framebuffer = p_framebuffer;
	draw_instruction_list.region = p_region;
	draw_instruction_list.stages = p_stages;
	draw_instruction_list.attachment_operations.resize(p_attachment_operations.size());

	for (uint32_t i = 0; i < p_attachment_operations.size(); i++) {
		draw_instruction_list.attachment_operations[i] = p_attachment_operations[i];
	}

	// GodotNext: rd_clear_values (#275)
	draw_list_driver_clear_values(p_framebuffer_cache, p_attachment_clear_values, draw_instruction_list.attachment_clear_values);

	draw_instruction_list.split_cmd_buffer = p_split_cmd_buffer;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	draw_instruction_list.breadcrumb = p_breadcrumb;
#endif

	workarounds_state.bound_any_draw_list_pipeline = false;
}

void RenderingDeviceGraph::_run_secondary_command_buffer_task(const SecondaryCommandBuffer *p_secondary) {
	driver->command_buffer_begin_secondary(p_secondary->command_buffer, p_secondary->render_pass, 0, p_secondary->framebuffer);
	_run_draw_list_command(p_secondary->command_buffer, p_secondary->instruction_data.ptr(), p_secondary->instruction_data.size());
	driver->command_buffer_end(p_secondary->command_buffer);
}

void RenderingDeviceGraph::_wait_for_secondary_command_buffer_tasks() {
	for (uint32_t i = 0; i < frames[frame].secondary_command_buffers_used; i++) {
		WorkerThreadPool::TaskID &task = frames[frame].secondary_command_buffers[i].task;
		if (task != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(task);
			task = WorkerThreadPool::INVALID_TASK_ID;
		}
	}
}

// GodotNext: async_compute (#174). p_async records into the async compute command buffer.
void RenderingDeviceGraph::_run_render_commands(int32_t p_level, const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool, int32_t &r_current_label_index, int32_t &r_current_label_level, bool p_async) {
	driver->command_group_begin(r_command_buffer);
	for (uint32_t i = 0; i < p_sorted_commands_count; i++) {
		const uint32_t command_index = p_sorted_commands[i].index;
		const uint32_t command_data_offset = command_data_offsets[command_index];
		const RecordedCommand *command = reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offset]);
		_run_label_command_change(r_command_buffer, command->label_index, p_level, false, true, &p_sorted_commands[i], p_sorted_commands_count - i, r_current_label_index, r_current_label_level);

		switch (command->type) {
			case RecordedCommand::TYPE_BOTTOM_LEVEL_ACCELERATION_STRUCTURE_BUILD: {
				const RecordedBottomLevelAccelerationStructureBuildCommand *blas_build_command = reinterpret_cast<const RecordedBottomLevelAccelerationStructureBuildCommand *>(command);
				// GodotNext: as_build_modes
				driver->command_build_blas(r_command_buffer, blas_build_command->acceleration_structure, blas_build_command->scratch_buffer, blas_build_command->scratch_offset, blas_build_command->mode);
			} break;
			case RecordedCommand::TYPE_TOP_LEVEL_ACCELERATION_STRUCTURE_BUILD: {
				const RecordedTopLevelAccelerationStructureBuildCommand *tlas_build_command = reinterpret_cast<const RecordedTopLevelAccelerationStructureBuildCommand *>(command);
				// GodotNext: as_build_modes
				driver->command_build_tlas(r_command_buffer, tlas_build_command->acceleration_structure, tlas_build_command->scratch_buffer, tlas_build_command->scratch_offset, tlas_build_command->instance_buffer, tlas_build_command->instance_offset, tlas_build_command->instance_count, tlas_build_command->mode);
			} break;
			case RecordedCommand::TYPE_BUFFER_CLEAR: {
				const RecordedBufferClearCommand *buffer_clear_command = reinterpret_cast<const RecordedBufferClearCommand *>(command);
				driver->command_clear_buffer(r_command_buffer, buffer_clear_command->buffer, buffer_clear_command->offset, buffer_clear_command->size);
			} break;
			case RecordedCommand::TYPE_BUFFER_COPY: {
				const RecordedBufferCopyCommand *buffer_copy_command = reinterpret_cast<const RecordedBufferCopyCommand *>(command);
				driver->command_copy_buffer(r_command_buffer, buffer_copy_command->source, buffer_copy_command->destination, buffer_copy_command->region);
			} break;
			case RecordedCommand::TYPE_BUFFER_GET_DATA: {
				const RecordedBufferGetDataCommand *buffer_get_data_command = reinterpret_cast<const RecordedBufferGetDataCommand *>(command);
				driver->command_copy_buffer(r_command_buffer, buffer_get_data_command->source, buffer_get_data_command->destination, buffer_get_data_command->region);
			} break;
			case RecordedCommand::TYPE_BUFFER_UPDATE: {
				const RecordedBufferUpdateCommand *buffer_update_command = reinterpret_cast<const RecordedBufferUpdateCommand *>(command);
				const RecordedBufferCopy *command_buffer_copies = buffer_update_command->buffer_copies();
				for (uint32_t j = 0; j < buffer_update_command->buffer_copies_count; j++) {
					driver->command_copy_buffer(r_command_buffer, command_buffer_copies[j].source, buffer_update_command->destination, command_buffer_copies[j].region);
				}
			} break;
			case RecordedCommand::TYPE_DRIVER_CALLBACK: {
				const RecordedDriverCallbackCommand *driver_callback_command = reinterpret_cast<const RecordedDriverCallbackCommand *>(command);
				driver_callback_command->callback(driver, r_command_buffer, driver_callback_command->userdata);
			} break;
			case RecordedCommand::TYPE_RAYTRACING_LIST: {
				const RecordedRaytracingListCommand *raytracing_list_command = reinterpret_cast<const RecordedRaytracingListCommand *>(command);
				_run_raytracing_list_command(r_command_buffer, raytracing_list_command->instruction_data(), raytracing_list_command->instruction_data_size);
			} break;
			case RecordedCommand::TYPE_COMPUTE_LIST: {
				// GodotNext: async_compute (#174). The async command buffer holds only compute lists,
				// so the compute-after-draw workaround applies to the main command buffer alone.
				if (!p_async && driver_workarounds.avoid_compute_after_draw && workarounds_state.draw_list_found) {
					// Avoid compute after draw workaround. Refer to the comment that enables this in the Vulkan driver for more information.
					workarounds_state.draw_list_found = false;

					// Create or reuse a command buffer and finish recording the current one.
					driver->command_buffer_end(r_command_buffer);
					// GodotNext: metal_level_fences (#186)
					const RDD::CommandBufferID continued_command_buffer = r_command_buffer;

					while (r_command_buffer_pool.buffers_used >= r_command_buffer_pool.buffers.size()) {
						RDD::CommandBufferID command_buffer = driver->command_buffer_create(r_command_buffer_pool.pool);
						RDD::SemaphoreID command_semaphore = driver->semaphore_create();
						r_command_buffer_pool.buffers.push_back(command_buffer);
						r_command_buffer_pool.semaphores.push_back(command_semaphore);
					}

					// Start recording on the next usable command buffer from the pool.
					uint32_t command_buffer_index = r_command_buffer_pool.buffers_used++;
					r_command_buffer = r_command_buffer_pool.buffers[command_buffer_index];
					driver->command_buffer_begin(r_command_buffer);
					driver->command_buffer_continue(r_command_buffer, continued_command_buffer); // GodotNext: metal_level_fences (#186)
				}

				const RecordedComputeListCommand *compute_list_command = reinterpret_cast<const RecordedComputeListCommand *>(command);
				driver->command_begin_compute_pass(r_command_buffer);
				_run_compute_list_command(r_command_buffer, compute_list_command->instruction_data(), compute_list_command->instruction_data_size);
				driver->command_end_compute_pass(r_command_buffer);
			} break;
			case RecordedCommand::TYPE_DRAW_LIST: {
				if (driver_workarounds.avoid_compute_after_draw) {
					// Indicate that a draw list was encountered for the workaround.
					workarounds_state.draw_list_found = true;
				}

				const RecordedDrawListCommand *draw_list_command = reinterpret_cast<const RecordedDrawListCommand *>(command);

				if (draw_list_command->split_cmd_buffer) {
					// Create or reuse a command buffer and finish recording the current one.
					driver->command_buffer_end(r_command_buffer);
					// GodotNext: metal_level_fences (#186)
					const RDD::CommandBufferID continued_command_buffer = r_command_buffer;

					while (r_command_buffer_pool.buffers_used >= r_command_buffer_pool.buffers.size()) {
						RDD::CommandBufferID command_buffer = driver->command_buffer_create(r_command_buffer_pool.pool);
						RDD::SemaphoreID command_semaphore = driver->semaphore_create();
						r_command_buffer_pool.buffers.push_back(command_buffer);
						r_command_buffer_pool.semaphores.push_back(command_semaphore);
					}

					// Start recording on the next usable command buffer from the pool.
					uint32_t command_buffer_index = r_command_buffer_pool.buffers_used++;
					r_command_buffer = r_command_buffer_pool.buffers[command_buffer_index];
					driver->command_buffer_begin(r_command_buffer);
					driver->command_buffer_continue(r_command_buffer, continued_command_buffer); // GodotNext: metal_level_fences (#186)
				}

				const VectorView clear_values(draw_list_command->clear_values(), draw_list_command->clear_values_count);
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
				driver->command_insert_breadcrumb(r_command_buffer, draw_list_command->breadcrumb);
#endif
				RDD::RenderPassID render_pass;
				RDD::FramebufferID framebuffer;
				if (draw_list_command->framebuffer_cache != nullptr) {
					_get_draw_list_render_pass_and_framebuffer(draw_list_command, render_pass, framebuffer);
				} else {
					render_pass = draw_list_command->render_pass;
					framebuffer = draw_list_command->framebuffer;
				}

				if (framebuffer && render_pass) {
					// GodotNext: rd_indirect_count (#177). Drivers that emulate the count (Metal) prepare
					// these draws with work that cannot run inside a render pass.
					for (uint32_t draw_index = 0; draw_index < draw_list_command->indirect_count_draws; draw_index++) {
						const DrawListDrawIndirectCountInstruction *draw_instruction = reinterpret_cast<const DrawListDrawIndirectCountInstruction *>(draw_list_command->instruction_data() + draw_list_command->indirect_count_offsets()[draw_index]);
						// GodotNext: rd_mesh_shaders (#178). Mesh task count draws have their own hook.
						if (draw_instruction->type == DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT_COUNT) {
							driver->command_render_draw_mesh_tasks_indirect_count_prepare(r_command_buffer, render_pass, draw_instruction->buffer, draw_instruction->offset, draw_instruction->count_buffer, draw_instruction->count_buffer_offset, draw_instruction->max_draw_count, draw_instruction->stride);
							continue;
						}
						driver->command_render_draw_indirect_count_prepare(r_command_buffer, render_pass, draw_instruction->type == DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT_COUNT, draw_instruction->buffer, draw_instruction->offset, draw_instruction->count_buffer, draw_instruction->count_buffer_offset, draw_instruction->max_draw_count, draw_instruction->stride);
					}
					driver->command_begin_render_pass(r_command_buffer, render_pass, framebuffer, draw_list_command->command_buffer_type, draw_list_command->region, clear_values);
					_run_draw_list_command(r_command_buffer, draw_list_command->instruction_data(), draw_list_command->instruction_data_size);
					driver->command_end_render_pass(r_command_buffer);
				}
			} break;
			case RecordedCommand::TYPE_TEXTURE_CLEAR_COLOR: {
				const RecordedTextureClearColorCommand *texture_clear_color_command = reinterpret_cast<const RecordedTextureClearColorCommand *>(command);
				driver->command_clear_color_texture(r_command_buffer, texture_clear_color_command->texture, RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, texture_clear_color_command->color, texture_clear_color_command->range);
			} break;
			case RecordedCommand::TYPE_TEXTURE_CLEAR_DEPTH_STENCIL: {
				const RecordedTextureClearDepthStencilCommand *texture_clear_depth_stencil_command = reinterpret_cast<const RecordedTextureClearDepthStencilCommand *>(command);
				driver->command_clear_depth_stencil_texture(r_command_buffer, texture_clear_depth_stencil_command->texture, RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, texture_clear_depth_stencil_command->depth, texture_clear_depth_stencil_command->stencil, texture_clear_depth_stencil_command->range);
			} break;
			case RecordedCommand::TYPE_TEXTURE_COPY: {
				const RecordedTextureCopyCommand *texture_copy_command = reinterpret_cast<const RecordedTextureCopyCommand *>(command);
				const VectorView<RDD::TextureCopyRegion> command_texture_copy_regions_view(texture_copy_command->texture_copy_regions(), texture_copy_command->texture_copy_regions_count);
				driver->command_copy_texture(r_command_buffer, texture_copy_command->from_texture, RDD::TEXTURE_LAYOUT_COPY_SRC_OPTIMAL, texture_copy_command->to_texture, RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, command_texture_copy_regions_view);
			} break;
			case RecordedCommand::TYPE_TEXTURE_GET_DATA: {
				const RecordedTextureGetDataCommand *texture_get_data_command = reinterpret_cast<const RecordedTextureGetDataCommand *>(command);
				const VectorView<RDD::BufferTextureCopyRegion> command_buffer_texture_copy_regions_view(texture_get_data_command->buffer_texture_copy_regions(), texture_get_data_command->buffer_texture_copy_regions_count);
				driver->command_copy_texture_to_buffer(r_command_buffer, texture_get_data_command->from_texture, RDD::TEXTURE_LAYOUT_COPY_SRC_OPTIMAL, texture_get_data_command->to_buffer, command_buffer_texture_copy_regions_view);
			} break;
			case RecordedCommand::TYPE_TEXTURE_RESOLVE: {
				const RecordedTextureResolveCommand *texture_resolve_command = reinterpret_cast<const RecordedTextureResolveCommand *>(command);
				driver->command_resolve_texture(r_command_buffer, texture_resolve_command->from_texture, RDD::TEXTURE_LAYOUT_RESOLVE_SRC_OPTIMAL, texture_resolve_command->src_layer, texture_resolve_command->src_mipmap, texture_resolve_command->to_texture, RDD::TEXTURE_LAYOUT_RESOLVE_DST_OPTIMAL, texture_resolve_command->dst_layer, texture_resolve_command->dst_mipmap);
			} break;
			case RecordedCommand::TYPE_TEXTURE_UPDATE: {
				const RecordedTextureUpdateCommand *texture_update_command = reinterpret_cast<const RecordedTextureUpdateCommand *>(command);
				const RecordedBufferToTextureCopy *command_buffer_to_texture_copies = texture_update_command->buffer_to_texture_copies();
				for (uint32_t j = 0; j < texture_update_command->buffer_to_texture_copies_count; j++) {
					driver->command_copy_buffer_to_texture(r_command_buffer, command_buffer_to_texture_copies[j].from_buffer, texture_update_command->to_texture, RDD::TEXTURE_LAYOUT_COPY_DST_OPTIMAL, command_buffer_to_texture_copies[j].region);
				}
			} break;
			case RecordedCommand::TYPE_CAPTURE_TIMESTAMP: {
				const RecordedCaptureTimestampCommand *texture_capture_timestamp_command = reinterpret_cast<const RecordedCaptureTimestampCommand *>(command);
				driver->command_timestamp_write(r_command_buffer, texture_capture_timestamp_command->pool, texture_capture_timestamp_command->index);
			} break;
			// GodotNext: blas_compact
			case RecordedCommand::TYPE_ACCELERATION_STRUCTURE_QUERY_SIZE: {
				const RecordedAccelerationStructureQuerySizeCommand *query_command = reinterpret_cast<const RecordedAccelerationStructureQuerySizeCommand *>(command);
				driver->command_acceleration_structure_write_compacted_size(r_command_buffer, VectorView<RDD::AccelerationStructureID>(&query_command->acceleration_structure, 1), query_command->pool, query_command->index);
			} break;
			case RecordedCommand::TYPE_ACCELERATION_STRUCTURE_COPY: {
				const RecordedAccelerationStructureCopyCommand *copy_command = reinterpret_cast<const RecordedAccelerationStructureCopyCommand *>(command);
				driver->command_copy_acceleration_structure_compact(r_command_buffer, copy_command->source, copy_command->destination);
			} break;
			// GodotNext: p6f_nv_rt (#90)
			case RecordedCommand::TYPE_CLAS_BUILD: {
				const RecordedCLASBuildCommand *clas_command = reinterpret_cast<const RecordedCLASBuildCommand *>(command);
				driver->command_build_clas_from_buffer(r_command_buffer, clas_command->params);
			} break;
			case RecordedCommand::TYPE_CLUSTER_BLAS_BUILD: {
				const RecordedClusterBLASBuildCommand *cblas_command = reinterpret_cast<const RecordedClusterBLASBuildCommand *>(command);
				driver->command_build_cluster_blas_from_buffer(r_command_buffer, cblas_command->params);
			} break;
			case RecordedCommand::TYPE_PTLAS_UPDATE: {
				const RecordedPTLASUpdateCommand *ptlas_command = reinterpret_cast<const RecordedPTLASUpdateCommand *>(command);
				driver->command_update_ptlas_from_buffer(r_command_buffer, ptlas_command->params);
			} break;
			case RecordedCommand::TYPE_MICROMAP_BUILD: {
				const RecordedMicromapBuildCommand *mm_command = reinterpret_cast<const RecordedMicromapBuildCommand *>(command);
				driver->command_build_micromap(r_command_buffer, mm_command->params);
			} break;
			default: {
				DEV_ASSERT(false && "Unknown recorded command type.");
				return;
			}
		}
	}
	driver->command_group_end(r_command_buffer);
}

void RenderingDeviceGraph::_run_label_command_change(RDD::CommandBufferID p_command_buffer, int32_t p_new_label_index, int32_t p_new_level, bool p_ignore_previous_value, bool p_use_label_for_empty, const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, int32_t &r_current_label_index, int32_t &r_current_label_level) {
	if (command_label_count == 0) {
		// Ignore any label operations if no labels were pushed.
		return;
	}

	if (p_ignore_previous_value || p_new_label_index != r_current_label_index || p_new_level != r_current_label_level) {
		if (!p_ignore_previous_value && (p_use_label_for_empty || r_current_label_index >= 0 || r_current_label_level >= 0)) {
			// End the current label.
			driver->command_end_label(p_command_buffer);
		}

		String label_name;
		Color label_color;
		if (p_new_label_index >= 0) {
			const char *label_chars = &command_label_chars[command_label_offsets[p_new_label_index]];
			label_name.append_utf8(label_chars);
			label_color = command_label_colors[p_new_label_index];
		} else if (p_use_label_for_empty) {
			label_name = "Command Graph";
			label_color = Color(1, 1, 1, 1);
		} else {
			return;
		}

		// Add the level to the name.
		label_name += " (L" + itos(p_new_level) + ")";

		if (p_sorted_commands != nullptr && p_sorted_commands_count > 0) {
			// Analyze the commands in the level that have the same label to detect what type of operations are performed.
			bool copy_commands = false;
			bool compute_commands = false;
			bool draw_commands = false;
			bool custom_commands = false;
			for (uint32_t i = 0; i < p_sorted_commands_count; i++) {
				const uint32_t command_index = p_sorted_commands[i].index;
				const uint32_t command_data_offset = command_data_offsets[command_index];
				const RecordedCommand *command = reinterpret_cast<RecordedCommand *>(&command_data[command_data_offset]);
				if (command->label_index != p_new_label_index) {
					break;
				}

				switch (command->type) {
					case RecordedCommand::TYPE_BUFFER_CLEAR:
					case RecordedCommand::TYPE_BUFFER_COPY:
					case RecordedCommand::TYPE_BUFFER_GET_DATA:
					case RecordedCommand::TYPE_BUFFER_UPDATE:
					case RecordedCommand::TYPE_TEXTURE_CLEAR_COLOR:
					case RecordedCommand::TYPE_TEXTURE_CLEAR_DEPTH_STENCIL:
					case RecordedCommand::TYPE_TEXTURE_COPY:
					case RecordedCommand::TYPE_TEXTURE_GET_DATA:
					case RecordedCommand::TYPE_TEXTURE_RESOLVE:
					case RecordedCommand::TYPE_TEXTURE_UPDATE: {
						copy_commands = true;
					} break;
					case RecordedCommand::TYPE_COMPUTE_LIST: {
						compute_commands = true;
					} break;
					case RecordedCommand::TYPE_DRAW_LIST: {
						draw_commands = true;
					} break;
					case RecordedCommand::TYPE_DRIVER_CALLBACK: {
						custom_commands = true;
					} break;
					// GodotNext: blas_compact
					case RecordedCommand::TYPE_ACCELERATION_STRUCTURE_QUERY_SIZE:
					case RecordedCommand::TYPE_ACCELERATION_STRUCTURE_COPY:
					// GodotNext: p6f_nv_rt (#90)
					case RecordedCommand::TYPE_CLAS_BUILD:
					case RecordedCommand::TYPE_CLUSTER_BLAS_BUILD:
					case RecordedCommand::TYPE_PTLAS_UPDATE:
					case RecordedCommand::TYPE_MICROMAP_BUILD: {
						custom_commands = true;
					} break;
					default: {
						// Ignore command.
					} break;
				}

				if (copy_commands && compute_commands && draw_commands && custom_commands) {
					// There's no more command types to find.
					break;
				}
			}

			if (copy_commands || compute_commands || draw_commands || custom_commands) {
				// Add the operations to the name.
				bool plus_after_copy = copy_commands && (compute_commands || draw_commands || custom_commands);
				bool plus_after_compute = compute_commands && (draw_commands || custom_commands);
				bool plus_after_draw = draw_commands && custom_commands;
				label_name += " (";
				label_name += copy_commands ? "Copy" : "";
				label_name += plus_after_copy ? "+" : "";
				label_name += compute_commands ? "Compute" : "";
				label_name += plus_after_compute ? "+" : "";
				label_name += draw_commands ? "Draw" : "";
				label_name += plus_after_draw ? "+" : "";
				label_name += custom_commands ? "Custom" : "";
				label_name += ")";
			}
		}

		// Start the new label.
		CharString label_name_utf8 = label_name.utf8();
		driver->command_begin_label(p_command_buffer, label_name_utf8.get_data(), label_color);

		r_current_label_index = p_new_label_index;
		r_current_label_level = p_new_level;
	}
}

void RenderingDeviceGraph::_boost_priority_for_render_commands(RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, uint32_t &r_boosted_priority) {
	if (p_sorted_commands_count == 0) {
		return;
	}

	const uint32_t boosted_priority_value = 0;
	if (r_boosted_priority > 0) {
		bool perform_sort = false;
		for (uint32_t j = 0; j < p_sorted_commands_count; j++) {
			if (p_sorted_commands[j].priority == r_boosted_priority) {
				p_sorted_commands[j].priority = boosted_priority_value;
				perform_sort = true;
			}
		}

		if (perform_sort) {
			SortArray<RecordedCommandSort> command_sorter;
			command_sorter.sort(p_sorted_commands, p_sorted_commands_count);
		}
	}

	if (p_sorted_commands[p_sorted_commands_count - 1].priority != boosted_priority_value) {
		r_boosted_priority = p_sorted_commands[p_sorted_commands_count - 1].priority;
	}
}

// GodotNext: async_compute (#174). Gathering the barriers moved to
// _gather_barriers_for_render_commands(), which the doctests call without a driver.
void RenderingDeviceGraph::_group_barriers_for_render_commands(RDD::CommandBufferID p_command_buffer, const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, bool p_full_memory_barrier, BarrierSelection p_selection) {
	if (!_gather_barriers_for_render_commands(p_sorted_commands, p_sorted_commands_count, p_full_memory_barrier, p_selection)) {
		// Commands don't require synchronization.
		return;
	}

	const bool is_memory_barrier_empty = barrier_group.memory_barrier.src_access.is_empty() && barrier_group.memory_barrier.dst_access.is_empty();
#if USE_BUFFER_BARRIERS
	const bool are_buffer_barriers_empty = barrier_group.buffer_barriers.is_empty();
#endif
	const bool are_acceleration_structure_barriers_empty = barrier_group.acceleration_structure_barriers.is_empty();
	const VectorView<RDD::MemoryAccessBarrier> memory_barriers = !is_memory_barrier_empty ? barrier_group.memory_barrier : VectorView<RDD::MemoryAccessBarrier>();
	const VectorView<RDD::TextureBarrier> texture_barriers = barrier_group.normalization_barriers.is_empty() ? barrier_group.transition_barriers : barrier_group.normalization_barriers;
#if USE_BUFFER_BARRIERS
	const VectorView<RDD::BufferBarrier> buffer_barriers = !are_buffer_barriers_empty ? barrier_group.buffer_barriers : VectorView<RDD::BufferBarrier>();
#else
	const VectorView<RDD::BufferBarrier> buffer_barriers = VectorView<RDD::BufferBarrier>();
#endif
	const VectorView<RDD::AccelerationStructureBarrier> acceleration_structure_barriers = !are_acceleration_structure_barriers_empty ? barrier_group.acceleration_structure_barriers : VectorView<RDD::AccelerationStructureBarrier>();

	driver->command_pipeline_barrier(p_command_buffer, barrier_group.src_stages, barrier_group.dst_stages, memory_barriers, buffer_barriers, texture_barriers, acceleration_structure_barriers);

	bool separate_texture_barriers = !barrier_group.normalization_barriers.is_empty() && !barrier_group.transition_barriers.is_empty();
	if (separate_texture_barriers) {
		driver->command_pipeline_barrier(p_command_buffer, barrier_group.src_stages, barrier_group.dst_stages, VectorView<RDD::MemoryAccessBarrier>(), VectorView<RDD::BufferBarrier>(), barrier_group.transition_barriers, VectorView<RDD::AccelerationStructureBarrier>());
	}
}

// GodotNext: async_compute (#174). Whether the async compute queue can run a texture barrier.
static bool _async_queue_supports_texture_barrier(const RDD::TextureBarrier &p_barrier) {
	return RenderingDeviceGraph::async_queue_supports_texture_layout(p_barrier.prev_layout) && RenderingDeviceGraph::async_queue_supports_texture_layout(p_barrier.next_layout);
}

bool RenderingDeviceGraph::_async_subresources_used_by_earlier_command(const ResourceTracker *p_search_tracker, int32_t p_command_index, const Rect2i &p_subresources) const {
	if (p_search_tracker->async_full_command_index >= 0 && p_search_tracker->async_full_command_index != p_command_index) {
		return true;
	}
	int32_t list_index = p_search_tracker->async_slice_command_list_index;
	while (list_index >= 0) {
		const RecordedSliceListNode &node = async_slice_list_nodes[list_index];
		if (node.command_index != p_command_index && node.subresources.intersects(p_subresources)) {
			return true;
		}
		list_index = node.next_list_index;
	}
	return false;
}

bool RenderingDeviceGraph::_async_fork_texture_barriers_conflict(const ResourceTracker *p_search_tracker, int32_t p_command_index, const RecordedCommand *p_command, int32_t p_normalization_start, int32_t p_transition_start) const {
	for (int32_t j = p_normalization_start; j < p_command->normalization_barrier_count; j++) {
		const RDD::TextureBarrier &barrier = command_normalization_barriers[p_command->normalization_barrier_index + j];
		if (!_async_queue_supports_texture_barrier(barrier)) {
			const Rect2i barrier_rect(barrier.subresources.base_mipmap, barrier.subresources.base_layer, barrier.subresources.mipmap_count, barrier.subresources.layer_count);
			if (_async_subresources_used_by_earlier_command(p_search_tracker, p_command_index, barrier_rect)) {
				return true;
			}
		}
	}
	for (int32_t j = p_transition_start; j < p_command->transition_barrier_count; j++) {
		const RDD::TextureBarrier &barrier = command_transition_barriers[p_command->transition_barrier_index + j];
		if (!_async_queue_supports_texture_barrier(barrier)) {
			const Rect2i barrier_rect(barrier.subresources.base_mipmap, barrier.subresources.base_layer, barrier.subresources.mipmap_count, barrier.subresources.layer_count);
			if (_async_subresources_used_by_earlier_command(p_search_tracker, p_command_index, barrier_rect)) {
				return true;
			}
		}
	}
	return false;
}

bool RenderingDeviceGraph::_gather_barriers_for_render_commands(const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count, bool p_full_memory_barrier, BarrierSelection p_selection) {
	if (!driver_honors_barriers) {
		return false;
	}

	// GodotNext: async_compute (#174). Without image layouts (Metal) the async queue runs every
	// barrier as recorded and nothing moves to the fork.
	const bool async_rules = p_selection != BARRIER_SELECTION_ALL && driver_textures_require_layout_transitions;
	if (p_selection == BARRIER_SELECTION_ASYNC_FORK && !async_rules) {
		return false;
	}

	barrier_group.clear();
	barrier_group.src_stages = RDD::PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	barrier_group.dst_stages = RDD::PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	for (uint32_t i = 0; i < p_sorted_commands_count; i++) {
		const uint32_t command_index = p_sorted_commands[i].index;
		const uint32_t command_data_offset = command_data_offsets[command_index];
		const RecordedCommand *command = reinterpret_cast<RecordedCommand *>(&command_data[command_data_offset]);

#if PRINT_COMMAND_RECORDING
		print_line(vformat("Grouping barriers for #%d", command_index));
#endif

		// GodotNext: async_compute (#174). The fork group takes only the async commands' texture
		// barriers the async queue cannot run, with the stages of the commands that have one. All of
		// those are valid on the main queue.
		if (p_selection == BARRIER_SELECTION_ASYNC_FORK) {
			if (p_sorted_commands[i].queue != QUEUE_ASYNC_COMPUTE) {
				continue;
			}
			bool fork_barrier_found = false;
			for (int32_t j = 0; j < command->normalization_barrier_count; j++) {
				const RDD::TextureBarrier &recorded_barrier = command_normalization_barriers[command->normalization_barrier_index + j];
				if (!_async_queue_supports_texture_barrier(recorded_barrier)) {
					barrier_group.normalization_barriers.push_back(recorded_barrier);
					fork_barrier_found = true;
				}
			}
			for (int32_t j = 0; j < command->transition_barrier_count; j++) {
				const RDD::TextureBarrier &recorded_barrier = command_transition_barriers[command->transition_barrier_index + j];
				if (!_async_queue_supports_texture_barrier(recorded_barrier)) {
					barrier_group.transition_barriers.push_back(recorded_barrier);
					fork_barrier_found = true;
				}
			}
			if (fork_barrier_found) {
				barrier_group.src_stages = barrier_group.src_stages | command->previous_stages;
				barrier_group.dst_stages = barrier_group.dst_stages | command->next_stages;
			}
			continue;
		}

		// Merge command's stage bits with the barrier group.
		barrier_group.src_stages = barrier_group.src_stages | command->previous_stages;
		barrier_group.dst_stages = barrier_group.dst_stages | command->next_stages;

		// Merge command's memory barrier bits with the barrier group.
		barrier_group.memory_barrier.src_access = barrier_group.memory_barrier.src_access | command->memory_barrier.src_access;
		barrier_group.memory_barrier.dst_access = barrier_group.memory_barrier.dst_access | command->memory_barrier.dst_access;

		// Gather texture barriers.
		for (int32_t j = 0; j < command->normalization_barrier_count; j++) {
			const RDD::TextureBarrier &recorded_barrier = command_normalization_barriers[command->normalization_barrier_index + j];
			// GodotNext: async_compute (#174). The main queue ran it before the fork.
			if (async_rules && !_async_queue_supports_texture_barrier(recorded_barrier)) {
				continue;
			}
			barrier_group.normalization_barriers.push_back(recorded_barrier);
#if PRINT_COMMAND_RECORDING
			print_line(vformat("Normalization Barrier #%d", barrier_group.normalization_barriers.size() - 1));
#endif
		}

		for (int32_t j = 0; j < command->transition_barrier_count; j++) {
			const RDD::TextureBarrier &recorded_barrier = command_transition_barriers[command->transition_barrier_index + j];
			// GodotNext: async_compute (#174). The main queue ran it before the fork.
			if (async_rules && !_async_queue_supports_texture_barrier(recorded_barrier)) {
				continue;
			}
			barrier_group.transition_barriers.push_back(recorded_barrier);
#if PRINT_COMMAND_RECORDING
			print_line(vformat("Transition Barrier #%d", barrier_group.transition_barriers.size() - 1));
#endif
		}

#if USE_BUFFER_BARRIERS
		// Gather buffer barriers.
		for (int32_t j = 0; j < command->buffer_barrier_count; j++) {
			const RDD::BufferBarrier &recorded_barrier = command_buffer_barriers[command->buffer_barrier_index + j];
			barrier_group.buffer_barriers.push_back(recorded_barrier);
		}
#endif

		// Gather acceleration structure barriers.
		for (int32_t j = 0; j < command->acceleration_structure_barrier_count; j++) {
			const RDD::AccelerationStructureBarrier &recorded_barrier = command_acceleration_structure_barriers[command->acceleration_structure_barrier_index + j];
			barrier_group.acceleration_structure_barriers.push_back(recorded_barrier);
		}
	}

	// GodotNext: rd_readonly_depth (#301). Emit shared read-only depth transitions at their resolved
	// execution point: before the fork on the main queue, or at the earliest main-queue reader's level.
	if (p_selection == BARRIER_SELECTION_ASYNC_FORK) {
		for (const SharedReadLayoutTransition &t : shared_read_transitions) {
			if (t.emit_at_fork) {
				barrier_group.transition_barriers.push_back(t.barrier);
				barrier_group.src_stages = barrier_group.src_stages | t.src_stages;
				barrier_group.dst_stages = barrier_group.dst_stages | t.dst_stages;
			}
		}
	} else if (p_selection == BARRIER_SELECTION_ALL && p_sorted_commands_count > 0) {
		const int32_t current_level = int32_t(p_sorted_commands[0].level);
		for (const SharedReadLayoutTransition &t : shared_read_transitions) {
			if (!t.emit_at_fork && t.emit_level == current_level) {
				barrier_group.transition_barriers.push_back(t.barrier);
				barrier_group.src_stages = barrier_group.src_stages | t.src_stages;
				barrier_group.dst_stages = barrier_group.dst_stages | t.dst_stages;
			}
		}
	}

	// GodotNext: async_compute (#174).
	if (p_selection == BARRIER_SELECTION_ASYNC_FORK && barrier_group.normalization_barriers.is_empty() && barrier_group.transition_barriers.is_empty()) {
		return false;
	}
	if (async_rules && p_selection == BARRIER_SELECTION_ASYNC_QUEUE) {
		barrier_group.src_stages = async_queue_stages(barrier_group.src_stages);
		barrier_group.dst_stages = async_queue_stages(barrier_group.dst_stages);
		barrier_group.memory_barrier.src_access = async_queue_access(barrier_group.memory_barrier.src_access);
		barrier_group.memory_barrier.dst_access = async_queue_access(barrier_group.memory_barrier.dst_access);
		for (RDD::TextureBarrier &barrier : barrier_group.normalization_barriers) {
			barrier.src_access = async_queue_access(barrier.src_access);
			barrier.dst_access = async_queue_access(barrier.dst_access);
		}
		for (RDD::TextureBarrier &barrier : barrier_group.transition_barriers) {
			barrier.src_access = async_queue_access(barrier.src_access);
			barrier.dst_access = async_queue_access(barrier.dst_access);
		}
#if USE_BUFFER_BARRIERS
		for (RDD::BufferBarrier &barrier : barrier_group.buffer_barriers) {
			barrier.src_access = async_queue_access(barrier.src_access);
			barrier.dst_access = async_queue_access(barrier.dst_access);
		}
#endif
		for (RDD::AccelerationStructureBarrier &barrier : barrier_group.acceleration_structure_barriers) {
			barrier.src_access = async_queue_access(barrier.src_access);
			barrier.dst_access = async_queue_access(barrier.dst_access);
		}
	}

	if (p_full_memory_barrier) {
		barrier_group.src_stages = RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT;
		barrier_group.dst_stages = RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT;
		barrier_group.memory_barrier.src_access = RDD::BARRIER_ACCESS_MEMORY_READ_BIT | RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT;
		barrier_group.memory_barrier.dst_access = RDD::BARRIER_ACCESS_MEMORY_READ_BIT | RDD::BARRIER_ACCESS_MEMORY_WRITE_BIT;
	}

	const bool is_memory_barrier_empty = barrier_group.memory_barrier.src_access.is_empty() && barrier_group.memory_barrier.dst_access.is_empty();
	const bool are_texture_barriers_empty = barrier_group.normalization_barriers.is_empty() && barrier_group.transition_barriers.is_empty();
#if USE_BUFFER_BARRIERS
	const bool are_buffer_barriers_empty = barrier_group.buffer_barriers.is_empty();
#else
	const bool are_buffer_barriers_empty = true;
#endif
	const bool are_acceleration_structure_barriers_empty = barrier_group.acceleration_structure_barriers.is_empty();
	return !(is_memory_barrier_empty && are_texture_barriers_empty && are_buffer_barriers_empty && are_acceleration_structure_barriers_empty);
}

void RenderingDeviceGraph::_print_render_commands(const RecordedCommandSort *p_sorted_commands, uint32_t p_sorted_commands_count) {
	for (uint32_t i = 0; i < p_sorted_commands_count; i++) {
		const uint32_t command_index = p_sorted_commands[i].index;
		const uint32_t command_level = p_sorted_commands[i].level;
		const uint32_t command_data_offset = command_data_offsets[command_index];
		const RecordedCommand *command = reinterpret_cast<RecordedCommand *>(&command_data[command_data_offset]);
		switch (command->type) {
			case RecordedCommand::TYPE_BUFFER_CLEAR: {
				const RecordedBufferClearCommand *buffer_clear_command = reinterpret_cast<const RecordedBufferClearCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "BUFFER CLEAR DESTINATION", itos(buffer_clear_command->buffer.id));
			} break;
			case RecordedCommand::TYPE_BUFFER_COPY: {
				const RecordedBufferCopyCommand *buffer_copy_command = reinterpret_cast<const RecordedBufferCopyCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "BUFFER COPY SOURCE", itos(buffer_copy_command->source.id), "DESTINATION", itos(buffer_copy_command->destination.id));
			} break;
			case RecordedCommand::TYPE_BUFFER_GET_DATA: {
				const RecordedBufferGetDataCommand *buffer_get_data_command = reinterpret_cast<const RecordedBufferGetDataCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "BUFFER GET DATA DESTINATION", itos(buffer_get_data_command->destination.id));
			} break;
			case RecordedCommand::TYPE_BUFFER_UPDATE: {
				const RecordedBufferUpdateCommand *buffer_update_command = reinterpret_cast<const RecordedBufferUpdateCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "BUFFER UPDATE DESTINATION", itos(buffer_update_command->destination.id), "COPIES", buffer_update_command->buffer_copies_count);
			} break;
			case RecordedCommand::TYPE_DRIVER_CALLBACK: {
				print_line(command_index, "LEVEL", command_level, "DRIVER CALLBACK");
			} break;
			case RecordedCommand::TYPE_COMPUTE_LIST: {
				const RecordedComputeListCommand *compute_list_command = reinterpret_cast<const RecordedComputeListCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "COMPUTE LIST SIZE", compute_list_command->instruction_data_size);
			} break;
			case RecordedCommand::TYPE_DRAW_LIST: {
				const RecordedDrawListCommand *draw_list_command = reinterpret_cast<const RecordedDrawListCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "DRAW LIST SIZE", draw_list_command->instruction_data_size);
			} break;
			case RecordedCommand::TYPE_TEXTURE_CLEAR_COLOR: {
				const RecordedTextureClearColorCommand *texture_clear_color_command = reinterpret_cast<const RecordedTextureClearColorCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "TEXTURE CLEAR COLOR", itos(texture_clear_color_command->texture.id), "COLOR", texture_clear_color_command->color);
			} break;
			case RecordedCommand::TYPE_TEXTURE_CLEAR_DEPTH_STENCIL: {
				const RecordedTextureClearDepthStencilCommand *texture_clear_depth_stencil_command = reinterpret_cast<const RecordedTextureClearDepthStencilCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "TEXTURE CLEAR DEPTH STENCIL", itos(texture_clear_depth_stencil_command->texture.id), "DEPTH", rtos(texture_clear_depth_stencil_command->depth), "STENCIL", itos(texture_clear_depth_stencil_command->stencil));
			} break;
			case RecordedCommand::TYPE_TEXTURE_COPY: {
				const RecordedTextureCopyCommand *texture_copy_command = reinterpret_cast<const RecordedTextureCopyCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "TEXTURE COPY FROM", itos(texture_copy_command->from_texture.id), "TO", itos(texture_copy_command->to_texture.id));
			} break;
			case RecordedCommand::TYPE_TEXTURE_GET_DATA: {
				print_line(command_index, "LEVEL", command_level, "TEXTURE GET DATA");
			} break;
			case RecordedCommand::TYPE_TEXTURE_RESOLVE: {
				const RecordedTextureResolveCommand *texture_resolve_command = reinterpret_cast<const RecordedTextureResolveCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "TEXTURE RESOLVE FROM", itos(texture_resolve_command->from_texture.id), "TO", itos(texture_resolve_command->to_texture.id));
			} break;
			case RecordedCommand::TYPE_TEXTURE_UPDATE: {
				const RecordedTextureUpdateCommand *texture_update_command = reinterpret_cast<const RecordedTextureUpdateCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "TEXTURE UPDATE TO", itos(texture_update_command->to_texture.id));
			} break;
			case RecordedCommand::TYPE_CAPTURE_TIMESTAMP: {
				const RecordedCaptureTimestampCommand *texture_capture_timestamp_command = reinterpret_cast<const RecordedCaptureTimestampCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "CAPTURE TIMESTAMP POOL", itos(texture_capture_timestamp_command->pool.id), "INDEX", texture_capture_timestamp_command->index);
			} break;
			// GodotNext: blas_compact
			case RecordedCommand::TYPE_ACCELERATION_STRUCTURE_QUERY_SIZE: {
				const RecordedAccelerationStructureQuerySizeCommand *query_command = reinterpret_cast<const RecordedAccelerationStructureQuerySizeCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "ACCELERATION STRUCTURE QUERY SIZE", itos(query_command->acceleration_structure.id), "INDEX", query_command->index);
			} break;
			case RecordedCommand::TYPE_ACCELERATION_STRUCTURE_COPY: {
				const RecordedAccelerationStructureCopyCommand *copy_command = reinterpret_cast<const RecordedAccelerationStructureCopyCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "ACCELERATION STRUCTURE COPY COMPACT SOURCE", itos(copy_command->source.id), "DESTINATION", itos(copy_command->destination.id));
			} break;
			// GodotNext: p6f_nv_rt (#90)
			case RecordedCommand::TYPE_CLAS_BUILD: {
				const RecordedCLASBuildCommand *clas_command = reinterpret_cast<const RecordedCLASBuildCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "CLAS BUILD MAX COUNT", clas_command->params.max_count);
			} break;
			case RecordedCommand::TYPE_CLUSTER_BLAS_BUILD: {
				const RecordedClusterBLASBuildCommand *cblas_command = reinterpret_cast<const RecordedClusterBLASBuildCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "CLUSTER BLAS BUILD MAX COUNT", cblas_command->params.max_blas_count);
			} break;
			case RecordedCommand::TYPE_PTLAS_UPDATE: {
				const RecordedPTLASUpdateCommand *ptlas_command = reinterpret_cast<const RecordedPTLASUpdateCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "PTLAS UPDATE", itos(ptlas_command->params.ptlas.id));
			} break;
			case RecordedCommand::TYPE_MICROMAP_BUILD: {
				const RecordedMicromapBuildCommand *mm_command = reinterpret_cast<const RecordedMicromapBuildCommand *>(command);
				print_line(command_index, "LEVEL", command_level, "MICROMAP BUILD", itos(mm_command->params.micromap.id));
			} break;
			default:
				DEV_ASSERT(false && "Unknown recorded command type.");
				return;
		}
	}
}

void RenderingDeviceGraph::_print_draw_list(const uint8_t *p_instruction_data, uint32_t p_instruction_data_size) {
	uint32_t instruction_data_cursor = 0;
	while (instruction_data_cursor < p_instruction_data_size) {
		DEV_ASSERT((instruction_data_cursor + sizeof(DrawListInstruction)) <= p_instruction_data_size);

		const DrawListInstruction *instruction = reinterpret_cast<const DrawListInstruction *>(&p_instruction_data[instruction_data_cursor]);
		switch (instruction->type) {
			case DrawListInstruction::TYPE_BIND_INDEX_BUFFER: {
				const DrawListBindIndexBufferInstruction *bind_index_buffer_instruction = reinterpret_cast<const DrawListBindIndexBufferInstruction *>(instruction);
				print_line("\tBIND INDEX BUFFER ID", itos(bind_index_buffer_instruction->buffer.id), "FORMAT", bind_index_buffer_instruction->format, "OFFSET", bind_index_buffer_instruction->offset);
				instruction_data_cursor += sizeof(DrawListBindIndexBufferInstruction);
			} break;
			case DrawListInstruction::TYPE_BIND_PIPELINE: {
				const DrawListBindPipelineInstruction *bind_pipeline_instruction = reinterpret_cast<const DrawListBindPipelineInstruction *>(instruction);
				print_line("\tBIND PIPELINE ID", itos(bind_pipeline_instruction->pipeline.id));
				instruction_data_cursor += sizeof(DrawListBindPipelineInstruction);
			} break;
			case DrawListInstruction::TYPE_BIND_UNIFORM_SETS: {
				const DrawListBindUniformSetsInstruction *bind_uniform_sets_instruction = reinterpret_cast<const DrawListBindUniformSetsInstruction *>(instruction);
				print_line("\tBIND UNIFORM SETS COUNT", bind_uniform_sets_instruction->set_count);
				for (uint32_t i = 0; i < bind_uniform_sets_instruction->set_count; i++) {
					print_line("\tBIND UNIFORM SET ID", itos(bind_uniform_sets_instruction->uniform_set_ids()[i].id), "START INDEX", bind_uniform_sets_instruction->first_set_index, "DYNAMIC_OFFSETS", bind_uniform_sets_instruction->dynamic_offsets_mask);
				}
				instruction_data_cursor += sizeof(DrawListBindUniformSetsInstruction) + sizeof(RDD::UniformSetID) * bind_uniform_sets_instruction->set_count;
			} break;
			case DrawListInstruction::TYPE_BIND_VERTEX_BUFFERS: {
				const DrawListBindVertexBuffersInstruction *bind_vertex_buffers_instruction = reinterpret_cast<const DrawListBindVertexBuffersInstruction *>(instruction);
				print_line("\tBIND VERTEX BUFFERS COUNT", bind_vertex_buffers_instruction->vertex_buffers_count);
				instruction_data_cursor += sizeof(DrawListBindVertexBuffersInstruction);
				instruction_data_cursor += sizeof(RDD::BufferID) * bind_vertex_buffers_instruction->vertex_buffers_count;
				instruction_data_cursor += sizeof(uint64_t) * bind_vertex_buffers_instruction->vertex_buffers_count;
			} break;
			case DrawListInstruction::TYPE_CLEAR_ATTACHMENTS: {
				const DrawListClearAttachmentsInstruction *clear_attachments_instruction = reinterpret_cast<const DrawListClearAttachmentsInstruction *>(instruction);
				print_line("\tATTACHMENTS CLEAR COUNT", clear_attachments_instruction->attachments_clear_count, "RECT COUNT", clear_attachments_instruction->attachments_clear_rect_count);
				instruction_data_cursor += sizeof(DrawListClearAttachmentsInstruction);
				instruction_data_cursor += sizeof(RDD::AttachmentClear) * clear_attachments_instruction->attachments_clear_count;
				instruction_data_cursor += sizeof(Rect2i) * clear_attachments_instruction->attachments_clear_rect_count;
			} break;
			case DrawListInstruction::TYPE_DRAW: {
				const DrawListDrawInstruction *draw_instruction = reinterpret_cast<const DrawListDrawInstruction *>(instruction);
				print_line("\tDRAW VERTICES", draw_instruction->vertex_count, "INSTANCES", draw_instruction->instance_count);
				instruction_data_cursor += sizeof(DrawListDrawInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDEXED: {
				const DrawListDrawIndexedInstruction *draw_indexed_instruction = reinterpret_cast<const DrawListDrawIndexedInstruction *>(instruction);
				print_line("\tDRAW INDICES", draw_indexed_instruction->index_count, "INSTANCES", draw_indexed_instruction->instance_count, "FIRST INDEX", draw_indexed_instruction->first_index);
				instruction_data_cursor += sizeof(DrawListDrawIndexedInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDIRECT: {
				const DrawListDrawIndirectInstruction *draw_indirect_instruction = reinterpret_cast<const DrawListDrawIndirectInstruction *>(instruction);
				print_line("\tDRAW INDIRECT BUFFER ID", itos(draw_indirect_instruction->buffer.id), "OFFSET", draw_indirect_instruction->offset, "DRAW COUNT", draw_indirect_instruction->draw_count, "STRIDE", draw_indirect_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT: {
				const DrawListDrawIndexedIndirectInstruction *draw_indexed_indirect_instruction = reinterpret_cast<const DrawListDrawIndexedIndirectInstruction *>(instruction);
				print_line("\tDRAW INDEXED INDIRECT BUFFER ID", itos(draw_indexed_indirect_instruction->buffer.id), "OFFSET", draw_indexed_indirect_instruction->offset, "DRAW COUNT", draw_indexed_indirect_instruction->draw_count, "STRIDE", draw_indexed_indirect_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndexedIndirectInstruction);
			} break;
			// GodotNext: rd_indirect_count (#177)
			case DrawListInstruction::TYPE_DRAW_INDIRECT_COUNT:
			case DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT_COUNT: {
				const DrawListDrawIndirectCountInstruction *draw_instruction = reinterpret_cast<const DrawListDrawIndirectCountInstruction *>(instruction);
				print_line(instruction->type == DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT_COUNT ? "\tDRAW INDEXED INDIRECT COUNT BUFFER ID" : "\tDRAW INDIRECT COUNT BUFFER ID", itos(draw_instruction->buffer.id), "OFFSET", draw_instruction->offset, "COUNT BUFFER ID", itos(draw_instruction->count_buffer.id), "COUNT OFFSET", draw_instruction->count_buffer_offset, "MAX DRAW COUNT", draw_instruction->max_draw_count, "STRIDE", draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectCountInstruction);
			} break;
			// GodotNext: rd_mesh_shaders (#178)
			case DrawListInstruction::TYPE_DRAW_MESH_TASKS: {
				const DrawListDrawMeshTasksInstruction *draw_instruction = reinterpret_cast<const DrawListDrawMeshTasksInstruction *>(instruction);
				print_line("\tDRAW MESH TASKS", draw_instruction->group_count[0], draw_instruction->group_count[1], draw_instruction->group_count[2]);
				instruction_data_cursor += sizeof(DrawListDrawMeshTasksInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT: {
				const DrawListDrawMeshTasksIndirectInstruction *draw_instruction = reinterpret_cast<const DrawListDrawMeshTasksIndirectInstruction *>(instruction);
				print_line("\tDRAW MESH TASKS INDIRECT BUFFER ID", itos(draw_instruction->buffer.id), "OFFSET", draw_instruction->offset, "DRAW COUNT", draw_instruction->draw_count, "STRIDE", draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawMeshTasksIndirectInstruction);
			} break;
			case DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT_COUNT: {
				const DrawListDrawIndirectCountInstruction *draw_instruction = reinterpret_cast<const DrawListDrawIndirectCountInstruction *>(instruction);
				print_line("\tDRAW MESH TASKS INDIRECT COUNT BUFFER ID", itos(draw_instruction->buffer.id), "OFFSET", draw_instruction->offset, "COUNT BUFFER ID", itos(draw_instruction->count_buffer.id), "COUNT OFFSET", draw_instruction->count_buffer_offset, "MAX DRAW COUNT", draw_instruction->max_draw_count, "STRIDE", draw_instruction->stride);
				instruction_data_cursor += sizeof(DrawListDrawIndirectCountInstruction);
			} break;
			case DrawListInstruction::TYPE_EXECUTE_COMMANDS: {
				print_line("\tEXECUTE COMMANDS");
				instruction_data_cursor += sizeof(DrawListExecuteCommandsInstruction);
			} break;
			case DrawListInstruction::TYPE_NEXT_SUBPASS: {
				print_line("\tNEXT SUBPASS");
				instruction_data_cursor += sizeof(DrawListNextSubpassInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_BLEND_CONSTANTS: {
				const DrawListSetBlendConstantsInstruction *set_blend_constants_instruction = reinterpret_cast<const DrawListSetBlendConstantsInstruction *>(instruction);
				print_line("\tSET BLEND CONSTANTS COLOR", set_blend_constants_instruction->color);
				instruction_data_cursor += sizeof(DrawListSetBlendConstantsInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_LINE_WIDTH: {
				const DrawListSetLineWidthInstruction *set_line_width_instruction = reinterpret_cast<const DrawListSetLineWidthInstruction *>(instruction);
				print_line("\tSET LINE WIDTH", set_line_width_instruction->width);
				instruction_data_cursor += sizeof(DrawListSetLineWidthInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_PUSH_CONSTANT: {
				const DrawListSetPushConstantInstruction *set_push_constant_instruction = reinterpret_cast<const DrawListSetPushConstantInstruction *>(instruction);
				print_line("\tSET PUSH CONSTANT SIZE", set_push_constant_instruction->size);
				instruction_data_cursor += sizeof(DrawListSetPushConstantInstruction);
				instruction_data_cursor += set_push_constant_instruction->size;
			} break;
			case DrawListInstruction::TYPE_SET_SCISSOR: {
				const DrawListSetScissorInstruction *set_scissor_instruction = reinterpret_cast<const DrawListSetScissorInstruction *>(instruction);
				print_line("\tSET SCISSOR", set_scissor_instruction->rect);
				instruction_data_cursor += sizeof(DrawListSetScissorInstruction);
			} break;
			case DrawListInstruction::TYPE_SET_VIEWPORT: {
				const DrawListSetViewportInstruction *set_viewport_instruction = reinterpret_cast<const DrawListSetViewportInstruction *>(instruction);
				print_line("\tSET VIEWPORT", set_viewport_instruction->rect);
				instruction_data_cursor += sizeof(DrawListSetViewportInstruction);
			} break;
			case DrawListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE: {
				const DrawListUniformSetPrepareForUseInstruction *uniform_set_prepare_for_use_instruction = reinterpret_cast<const DrawListUniformSetPrepareForUseInstruction *>(instruction);
				print_line("\tUNIFORM SET PREPARE FOR USE ID", itos(uniform_set_prepare_for_use_instruction->uniform_set.id), "SHADER ID", itos(uniform_set_prepare_for_use_instruction->shader.id), "INDEX", uniform_set_prepare_for_use_instruction->set_index);
				instruction_data_cursor += sizeof(DrawListUniformSetPrepareForUseInstruction);
			} break;
			default:
				DEV_ASSERT(false && "Unknown draw list instruction type.");
				return;
		}

		instruction_data_cursor = GRAPH_ALIGN(instruction_data_cursor);
	}
}

void RenderingDeviceGraph::_print_raytracing_list(const uint8_t *p_instruction_data, uint32_t p_instruction_data_size) {
	uint32_t instruction_data_cursor = 0;
	while (instruction_data_cursor < p_instruction_data_size) {
		DEV_ASSERT((instruction_data_cursor + sizeof(RaytracingListInstruction)) <= p_instruction_data_size);

		const RaytracingListInstruction *instruction = reinterpret_cast<const RaytracingListInstruction *>(&p_instruction_data[instruction_data_cursor]);
		switch (instruction->type) {
			case RaytracingListInstruction::TYPE_BIND_PIPELINE: {
				const RaytracingListBindPipelineInstruction *bind_pipeline_instruction = reinterpret_cast<const RaytracingListBindPipelineInstruction *>(instruction);
				print_line("\tBIND PIPELINE ID", itos(bind_pipeline_instruction->pipeline.id));
				instruction_data_cursor += sizeof(RaytracingListBindPipelineInstruction);
			} break;
			case RaytracingListInstruction::TYPE_BIND_UNIFORM_SET: {
				const RaytracingListBindUniformSetInstruction *bind_uniform_set_instruction = reinterpret_cast<const RaytracingListBindUniformSetInstruction *>(instruction);
				print_line("\tBIND UNIFORM SET ID", itos(bind_uniform_set_instruction->uniform_set.id), "SHADER ID", itos(bind_uniform_set_instruction->shader.id));
				instruction_data_cursor += sizeof(RaytracingListBindUniformSetInstruction);
			} break;
			case RaytracingListInstruction::TYPE_SET_PUSH_CONSTANT: {
				const RaytracingListSetPushConstantInstruction *set_push_constant_instruction = reinterpret_cast<const RaytracingListSetPushConstantInstruction *>(instruction);
				print_line("\tSET PUSH CONSTANT SIZE", set_push_constant_instruction->size);
				instruction_data_cursor += sizeof(RaytracingListSetPushConstantInstruction);
				instruction_data_cursor += set_push_constant_instruction->size;
			} break;
			case RaytracingListInstruction::TYPE_TRACE_RAYS: {
				const RaytracingListTraceRaysInstruction *trace_rays_instruction = reinterpret_cast<const RaytracingListTraceRaysInstruction *>(instruction);
				print_line("\tTRACE RAYS WIDTH", itos(trace_rays_instruction->width), "HEIGHT", itos(trace_rays_instruction->height));
				instruction_data_cursor += sizeof(RaytracingListTraceRaysInstruction);
			} break;
			// GodotNext: rd_rt_pipelines (#179)
			case RaytracingListInstruction::TYPE_TRACE_RAYS_INDIRECT: {
				const RaytracingListTraceRaysIndirectInstruction *trace_rays_indirect_instruction = reinterpret_cast<const RaytracingListTraceRaysIndirectInstruction *>(instruction);
				print_line("\tTRACE RAYS INDIRECT BUFFER ID", itos(trace_rays_indirect_instruction->buffer.id), "OFFSET", itos(trace_rays_indirect_instruction->offset));
				instruction_data_cursor += sizeof(RaytracingListTraceRaysIndirectInstruction);
			} break;
			case RaytracingListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE: {
				const RaytracingListUniformSetPrepareForUseInstruction *uniform_set_prepare_for_use_instruction = reinterpret_cast<const RaytracingListUniformSetPrepareForUseInstruction *>(instruction);
				print_line("\tUNIFORM SET PREPARE FOR USE ID", itos(uniform_set_prepare_for_use_instruction->uniform_set.id), "SHADER ID", itos(uniform_set_prepare_for_use_instruction->shader.id), "INDEX", itos(uniform_set_prepare_for_use_instruction->set_index));
				instruction_data_cursor += sizeof(RaytracingListUniformSetPrepareForUseInstruction);
			} break;
			default:
				DEV_ASSERT(false && "Unknown raytracing list instruction type.");
				return;
		}
	}
}

void RenderingDeviceGraph::_print_compute_list(const uint8_t *p_instruction_data, uint32_t p_instruction_data_size) {
	uint32_t instruction_data_cursor = 0;
	while (instruction_data_cursor < p_instruction_data_size) {
		DEV_ASSERT((instruction_data_cursor + sizeof(ComputeListInstruction)) <= p_instruction_data_size);

		const ComputeListInstruction *instruction = reinterpret_cast<const ComputeListInstruction *>(&p_instruction_data[instruction_data_cursor]);
		switch (instruction->type) {
			case ComputeListInstruction::TYPE_BIND_PIPELINE: {
				const ComputeListBindPipelineInstruction *bind_pipeline_instruction = reinterpret_cast<const ComputeListBindPipelineInstruction *>(instruction);
				print_line("\tBIND PIPELINE ID", itos(bind_pipeline_instruction->pipeline.id));
				instruction_data_cursor += sizeof(ComputeListBindPipelineInstruction);
			} break;
			case ComputeListInstruction::TYPE_BIND_UNIFORM_SETS: {
				const ComputeListBindUniformSetsInstruction *bind_uniform_sets_instruction = reinterpret_cast<const ComputeListBindUniformSetsInstruction *>(instruction);
				print_line("\tBIND UNIFORM SETS COUNT", bind_uniform_sets_instruction->set_count);
				for (uint32_t i = 0; i < bind_uniform_sets_instruction->set_count; i++) {
					print_line("\tBIND UNIFORM SET ID", itos(bind_uniform_sets_instruction->uniform_set_ids()[i].id), "START INDEX", bind_uniform_sets_instruction->first_set_index, "DYNAMIC_OFFSETS", bind_uniform_sets_instruction->dynamic_offsets_mask);
				}
				instruction_data_cursor += sizeof(ComputeListBindUniformSetsInstruction) + sizeof(RDD::UniformSetID) * bind_uniform_sets_instruction->set_count;
			} break;
			case ComputeListInstruction::TYPE_DISPATCH: {
				const ComputeListDispatchInstruction *dispatch_instruction = reinterpret_cast<const ComputeListDispatchInstruction *>(instruction);
				print_line("\tDISPATCH", dispatch_instruction->x_groups, dispatch_instruction->y_groups, dispatch_instruction->z_groups);
				instruction_data_cursor += sizeof(ComputeListDispatchInstruction);
			} break;
			case ComputeListInstruction::TYPE_DISPATCH_INDIRECT: {
				const ComputeListDispatchIndirectInstruction *dispatch_indirect_instruction = reinterpret_cast<const ComputeListDispatchIndirectInstruction *>(instruction);
				print_line("\tDISPATCH INDIRECT BUFFER ID", itos(dispatch_indirect_instruction->buffer.id), "OFFSET", dispatch_indirect_instruction->offset);
				instruction_data_cursor += sizeof(ComputeListDispatchIndirectInstruction);
			} break;
			case ComputeListInstruction::TYPE_SET_PUSH_CONSTANT: {
				const ComputeListSetPushConstantInstruction *set_push_constant_instruction = reinterpret_cast<const ComputeListSetPushConstantInstruction *>(instruction);
				print_line("\tSET PUSH CONSTANT SIZE", set_push_constant_instruction->size);
				instruction_data_cursor += sizeof(ComputeListSetPushConstantInstruction);
				instruction_data_cursor += set_push_constant_instruction->size;
			} break;
			case ComputeListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE: {
				const ComputeListUniformSetPrepareForUseInstruction *uniform_set_prepare_for_use_instruction = reinterpret_cast<const ComputeListUniformSetPrepareForUseInstruction *>(instruction);
				print_line("\tUNIFORM SET PREPARE FOR USE ID", itos(uniform_set_prepare_for_use_instruction->uniform_set.id), "SHADER ID", itos(uniform_set_prepare_for_use_instruction->shader.id), "INDEX", itos(uniform_set_prepare_for_use_instruction->set_index));
				instruction_data_cursor += sizeof(ComputeListUniformSetPrepareForUseInstruction);
			} break;
			default:
				DEV_ASSERT(false && "Unknown compute list instruction type.");
				return;
		}

		instruction_data_cursor = GRAPH_ALIGN(instruction_data_cursor);
	}
}

void RenderingDeviceGraph::initialize(RDD *p_driver, RenderPassCreationFunction p_render_pass_creation_function, uint32_t p_frame_count, RDD::CommandQueueFamilyID p_secondary_command_queue_family, uint32_t p_secondary_command_buffers_per_frame) {
	DEV_ASSERT(p_driver != nullptr);
	DEV_ASSERT(p_render_pass_creation_function != nullptr);
	DEV_ASSERT(p_frame_count > 0);

	driver = p_driver;
	driver_workarounds = p_driver->get_driver_workarounds();
	render_pass_creation_function = p_render_pass_creation_function;
	frames.resize(p_frame_count);

	for (uint32_t i = 0; i < p_frame_count; i++) {
		frames[i].secondary_command_buffers.resize(p_secondary_command_buffers_per_frame);

		for (uint32_t j = 0; j < p_secondary_command_buffers_per_frame; j++) {
			SecondaryCommandBuffer &secondary = frames[i].secondary_command_buffers[j];
			secondary.command_pool = driver->command_pool_create(p_secondary_command_queue_family, RDD::COMMAND_BUFFER_TYPE_SECONDARY);
			secondary.command_buffer = driver->command_buffer_create(secondary.command_pool);
			secondary.task = WorkerThreadPool::INVALID_TASK_ID;
		}
	}

	driver_honors_barriers = driver->api_trait_get(RDD::API_TRAIT_HONORS_PIPELINE_BARRIERS);
	driver_buffer_clears_with_copy_engine = driver->api_trait_get(RDD::API_TRAIT_BUFFER_CLEARS_WITH_COPY_ENGINE);
	driver_texture_clears_with_copy_engine = driver->api_trait_get(RDD::API_TRAIT_TEXTURE_CLEARS_WITH_COPY_ENGINE);
	driver_buffers_require_transitions = driver->api_trait_get(RDD::API_TRAIT_BUFFERS_REQUIRE_TRANSITIONS);
	driver_textures_require_layout_transitions = driver->api_trait_get(RDD::API_TRAIT_TEXTURES_REQUIRE_LAYOUT_TRANSITIONS);
}

void RenderingDeviceGraph::finalize() {
	if (!frames.is_empty()) {
		_wait_for_secondary_command_buffer_tasks();
	}

	for (Frame &f : frames) {
		for (SecondaryCommandBuffer &secondary : f.secondary_command_buffers) {
			if (secondary.command_pool.id != 0) {
				driver->command_pool_free(secondary.command_pool);
			}
		}
	}

	frames.clear();
}

void RenderingDeviceGraph::begin() {
	command_data.clear();
	command_data_offsets.clear();
	command_normalization_barriers.clear();
	command_transition_barriers.clear();
	command_buffer_barriers.clear();
	command_acceleration_structure_barriers.clear();
	// GodotNext: rd_readonly_depth (#301).
	shared_read_transitions.clear();
	command_label_chars.clear();
	command_label_colors.clear();
	command_label_offsets.clear();
	command_list_nodes.clear();
	read_slice_list_nodes.clear();
	write_slice_list_nodes.clear();
	// GodotNext: px_rd_async_multiview (#300).
	async_slice_list_nodes.clear();
	command_count = 0;
	// GodotNext: async_compute (#174).
	async_command_count = 0;
	async_fork_barrier_conflict = false;
	command_label_count = 0;
	command_timestamp_index = -1;
	command_synchronization_index = -1;
	command_synchronization_pending = false;
	command_label_index = -1;
	// GodotNext: rdg_access_stages (#267). A bare graph (the doctests) has no frames; begin() still
	// starts a new tracking frame for it.
	if (!frames.is_empty()) {
		frames[frame].secondary_command_buffers_used = 0;
	}
	draw_instruction_list.index = 0;
	compute_instruction_list.index = 0;
	tracking_frame++;

#ifdef DEV_ENABLED
	write_dependency_counters.clear();
#endif
}

void RenderingDeviceGraph::add_blas_build(RDD::AccelerationStructureID p_acceleration_structure, RDD::BufferID p_scratch_buffer, uint64_t p_scratch_offset, ResourceTracker *p_dst_tracker, VectorView<ResourceTracker *> p_src_trackers, RDD::AccelerationStructureBuildMode p_mode) {
	int32_t command_index;
	RecordedBottomLevelAccelerationStructureBuildCommand *command = static_cast<RecordedBottomLevelAccelerationStructureBuildCommand *>(_allocate_command(sizeof(RecordedBottomLevelAccelerationStructureBuildCommand), command_index));
	command->type = RecordedCommand::TYPE_BOTTOM_LEVEL_ACCELERATION_STRUCTURE_BUILD;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->acceleration_structure = p_acceleration_structure;
	command->scratch_buffer = p_scratch_buffer;
	command->scratch_offset = p_scratch_offset;
	// GodotNext: as_build_modes
	// UPDATE reads and writes the same acceleration structure. The destination usage below stays read-write.
	command->mode = p_mode;

	thread_local LocalVector<ResourceTracker *> trackers;
	thread_local LocalVector<ResourceUsage> usages;

	// Sources (deduplicated so a shared vertex/index buffer never self-depends under buffer-transition tracking) and destination.
	trackers.clear();
	usages.clear();
	trackers.reserve(p_src_trackers.size() + 1);
	usages.reserve(p_src_trackers.size() + 1);

	for (uint32_t i = 0; i < p_src_trackers.size(); ++i) {
		ResourceTracker *src_tracker = p_src_trackers[i];
		if (src_tracker != nullptr && !trackers.has(src_tracker)) {
			trackers.push_back(src_tracker);
			usages.push_back(RESOURCE_USAGE_STORAGE_BUFFER_READ);
		}
	}

	trackers.push_back(p_dst_tracker);
	usages.push_back(RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE);

	_add_command_to_graph(trackers.ptr(), usages.ptr(), usages.size(), command_index, command);
}

void RenderingDeviceGraph::add_tlas_build(RDD::AccelerationStructureID p_acceleration_structure, RDD::BufferID p_scratch_buffer, uint64_t p_scratch_offset, RDD::BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count, ResourceTracker *p_dst_tracker, VectorView<ResourceTracker *> p_src_trackers, RDD::AccelerationStructureBuildMode p_mode, ResourceTracker *p_instance_tracker) {
	int32_t command_index;
	RecordedTopLevelAccelerationStructureBuildCommand *command = static_cast<RecordedTopLevelAccelerationStructureBuildCommand *>(_allocate_command(sizeof(RecordedTopLevelAccelerationStructureBuildCommand), command_index));
	command->type = RecordedCommand::TYPE_TOP_LEVEL_ACCELERATION_STRUCTURE_BUILD;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->acceleration_structure = p_acceleration_structure;
	command->scratch_buffer = p_scratch_buffer;
	command->scratch_offset = p_scratch_offset;
	command->instance_buffer = p_instance_buffer;
	command->instance_offset = p_instance_offset;
	command->instance_count = p_instance_count;
	// GodotNext: as_build_modes
	// UPDATE reads and writes the same acceleration structure. The destination usage below stays read-write.
	command->mode = p_mode;

	thread_local LocalVector<ResourceTracker *> trackers;
	thread_local LocalVector<ResourceUsage> usages;

	// Sources and destination, plus the GPU-written instance buffer when there is one.
	uint32_t resource_count = p_src_trackers.size() + 1 + (p_instance_tracker != nullptr ? 1 : 0);
	trackers.resize(resource_count);
	usages.resize(resource_count);

	for (uint32_t i = 0; i < p_src_trackers.size(); ++i) {
		trackers[i] = p_src_trackers[i];
		usages[i] = RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ;
	}

	trackers[resource_count - 1] = p_dst_tracker;
	usages[resource_count - 1] = RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE;

	if (p_instance_tracker != nullptr) {
		trackers[resource_count - 2] = p_instance_tracker;
		usages[resource_count - 2] = RESOURCE_USAGE_STORAGE_BUFFER_READ;
	}

	_add_command_to_graph(trackers.ptr(), usages.ptr(), usages.size(), command_index, command);
}

// GodotNext: blas_compact
void RenderingDeviceGraph::add_acceleration_structure_query_size(RDD::AccelerationStructureID p_acceleration_structure, RDD::QueryPoolID p_pool, uint32_t p_index, ResourceTracker *p_tracker) {
	DEV_ASSERT(p_tracker != nullptr);

	int32_t command_index;
	RecordedAccelerationStructureQuerySizeCommand *command = static_cast<RecordedAccelerationStructureQuerySizeCommand *>(_allocate_command(sizeof(RecordedAccelerationStructureQuerySizeCommand), command_index));
	command->type = RecordedCommand::TYPE_ACCELERATION_STRUCTURE_QUERY_SIZE;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->acceleration_structure = p_acceleration_structure;
	command->pool = p_pool;
	command->index = p_index;

	ResourceUsage usage = RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ;
	_add_command_to_graph(&p_tracker, &usage, 1, command_index, command);
}

// GodotNext: blas_compact
void RenderingDeviceGraph::add_acceleration_structure_copy(RDD::AccelerationStructureID p_source, ResourceTracker *p_source_tracker, RDD::AccelerationStructureID p_destination, ResourceTracker *p_destination_tracker) {
	DEV_ASSERT(p_source_tracker != nullptr);
	DEV_ASSERT(p_destination_tracker != nullptr);

	int32_t command_index;
	RecordedAccelerationStructureCopyCommand *command = static_cast<RecordedAccelerationStructureCopyCommand *>(_allocate_command(sizeof(RecordedAccelerationStructureCopyCommand), command_index));
	command->type = RecordedCommand::TYPE_ACCELERATION_STRUCTURE_COPY;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->source = p_source;
	command->destination = p_destination;

	ResourceTracker *trackers[2] = { p_source_tracker, p_destination_tracker };
	ResourceUsage usages[2] = { RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ, RESOURCE_USAGE_ACCELERATION_STRUCTURE_READ_WRITE };
	_add_command_to_graph(trackers, usages, 2, command_index, command);
}

// GodotNext: p6f_nv_rt (#90)
void RenderingDeviceGraph::add_clas_build(const RDD::ClusterASBuildParams &p_params, VectorView<ResourceTracker *> p_read_trackers, VectorView<ResourceTracker *> p_write_trackers) {
	int32_t command_index;
	RecordedCLASBuildCommand *command = static_cast<RecordedCLASBuildCommand *>(_allocate_command(sizeof(RecordedCLASBuildCommand), command_index));
	command->type = RecordedCommand::TYPE_CLAS_BUILD;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->params = p_params;

	thread_local LocalVector<ResourceTracker *> trackers;
	thread_local LocalVector<ResourceUsage> usages;
	trackers.clear();
	usages.clear();
	for (uint32_t i = 0; i < p_write_trackers.size(); ++i) {
		ResourceTracker *t = p_write_trackers[i];
		if (t != nullptr && !trackers.has(t)) {
			trackers.push_back(t);
			usages.push_back(RESOURCE_USAGE_GENERAL);
		}
	}
	for (uint32_t i = 0; i < p_read_trackers.size(); ++i) {
		ResourceTracker *t = p_read_trackers[i];
		if (t != nullptr && !trackers.has(t)) {
			trackers.push_back(t);
			usages.push_back(RESOURCE_USAGE_STORAGE_BUFFER_READ);
		}
	}
	_add_command_to_graph(trackers.ptr(), usages.ptr(), usages.size(), command_index, command);
}

void RenderingDeviceGraph::add_cluster_blas_build(const RDD::ClusterBLASBuildParams &p_params, VectorView<ResourceTracker *> p_read_trackers, VectorView<ResourceTracker *> p_write_trackers) {
	int32_t command_index;
	RecordedClusterBLASBuildCommand *command = static_cast<RecordedClusterBLASBuildCommand *>(_allocate_command(sizeof(RecordedClusterBLASBuildCommand), command_index));
	command->type = RecordedCommand::TYPE_CLUSTER_BLAS_BUILD;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->params = p_params;

	thread_local LocalVector<ResourceTracker *> trackers;
	thread_local LocalVector<ResourceUsage> usages;
	trackers.clear();
	usages.clear();
	for (uint32_t i = 0; i < p_write_trackers.size(); ++i) {
		ResourceTracker *t = p_write_trackers[i];
		if (t != nullptr && !trackers.has(t)) {
			trackers.push_back(t);
			usages.push_back(RESOURCE_USAGE_GENERAL);
		}
	}
	for (uint32_t i = 0; i < p_read_trackers.size(); ++i) {
		ResourceTracker *t = p_read_trackers[i];
		if (t != nullptr && !trackers.has(t)) {
			trackers.push_back(t);
			usages.push_back(RESOURCE_USAGE_STORAGE_BUFFER_READ);
		}
	}
	_add_command_to_graph(trackers.ptr(), usages.ptr(), usages.size(), command_index, command);
}

void RenderingDeviceGraph::add_ptlas_update(const RDD::PartitionedTLASUpdateParams &p_params, ResourceTracker *p_ptlas_tracker, VectorView<ResourceTracker *> p_read_trackers) {
	DEV_ASSERT(p_ptlas_tracker != nullptr);

	int32_t command_index;
	RecordedPTLASUpdateCommand *command = static_cast<RecordedPTLASUpdateCommand *>(_allocate_command(sizeof(RecordedPTLASUpdateCommand), command_index));
	command->type = RecordedCommand::TYPE_PTLAS_UPDATE;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->params = p_params;

	thread_local LocalVector<ResourceTracker *> trackers;
	thread_local LocalVector<ResourceUsage> usages;
	trackers.clear();
	usages.clear();
	trackers.push_back(p_ptlas_tracker);
	usages.push_back(RESOURCE_USAGE_GENERAL);
	for (uint32_t i = 0; i < p_read_trackers.size(); ++i) {
		ResourceTracker *t = p_read_trackers[i];
		if (t != nullptr && !trackers.has(t)) {
			trackers.push_back(t);
			usages.push_back(RESOURCE_USAGE_STORAGE_BUFFER_READ);
		}
	}
	_add_command_to_graph(trackers.ptr(), usages.ptr(), usages.size(), command_index, command);
}

void RenderingDeviceGraph::add_micromap_build(const RDD::MicromapBuildParams &p_params, ResourceTracker *p_micromap_tracker, VectorView<ResourceTracker *> p_read_trackers) {
	DEV_ASSERT(p_micromap_tracker != nullptr);

	int32_t command_index;
	RecordedMicromapBuildCommand *command = static_cast<RecordedMicromapBuildCommand *>(_allocate_command(sizeof(RecordedMicromapBuildCommand), command_index));
	command->type = RecordedCommand::TYPE_MICROMAP_BUILD;
	command->self_stages = RDD::PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT;
	command->params = p_params;

	thread_local LocalVector<ResourceTracker *> trackers;
	thread_local LocalVector<ResourceUsage> usages;
	trackers.clear();
	usages.clear();
	trackers.push_back(p_micromap_tracker);
	usages.push_back(RESOURCE_USAGE_GENERAL);
	for (uint32_t i = 0; i < p_read_trackers.size(); ++i) {
		ResourceTracker *t = p_read_trackers[i];
		if (t != nullptr && !trackers.has(t)) {
			trackers.push_back(t);
			usages.push_back(RESOURCE_USAGE_STORAGE_BUFFER_READ);
		}
	}
	_add_command_to_graph(trackers.ptr(), usages.ptr(), usages.size(), command_index, command);
}

void RenderingDeviceGraph::add_buffer_clear(RDD::BufferID p_dst, ResourceTracker *p_dst_tracker, uint32_t p_offset, uint32_t p_size) {
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	RecordedBufferClearCommand *command = static_cast<RecordedBufferClearCommand *>(_allocate_command(sizeof(RecordedBufferClearCommand), command_index));
	command->type = RecordedCommand::TYPE_BUFFER_CLEAR;
	command->buffer = p_dst;
	command->offset = p_offset;
	command->size = p_size;

	ResourceUsage usage;
	if (driver_buffer_clears_with_copy_engine) {
		command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
		usage = RESOURCE_USAGE_COPY_TO;
	} else {
		// If the driver is uncapable of using the copy engine for clearing the buffer (e.g. D3D12), we must transition it to storage buffer read/write usage.
		command->self_stages = RDD::PIPELINE_STAGE_CLEAR_STORAGE_BIT;
		usage = RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE;
	}

	_add_command_to_graph(&p_dst_tracker, &usage, 1, command_index, command);
}

void RenderingDeviceGraph::add_buffer_copy(RDD::BufferID p_src, ResourceTracker *p_src_tracker, RDD::BufferID p_dst, ResourceTracker *p_dst_tracker, RDD::BufferCopyRegion p_region) {
	// Source tracker is allowed to be null as it could be a read-only buffer.
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	RecordedBufferCopyCommand *command = static_cast<RecordedBufferCopyCommand *>(_allocate_command(sizeof(RecordedBufferCopyCommand), command_index));
	command->type = RecordedCommand::TYPE_BUFFER_COPY;
	command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
	command->source = p_src;
	command->destination = p_dst;
	command->region = p_region;

	ResourceTracker *trackers[2] = { p_dst_tracker, p_src_tracker };
	ResourceUsage usages[2] = { RESOURCE_USAGE_COPY_TO, RESOURCE_USAGE_COPY_FROM };
	_add_command_to_graph(trackers, usages, p_src_tracker != nullptr ? 2 : 1, command_index, command);
}

void RenderingDeviceGraph::add_buffer_get_data(RDD::BufferID p_src, ResourceTracker *p_src_tracker, RDD::BufferID p_dst, RDD::BufferCopyRegion p_region) {
	// Source tracker is allowed to be null as it could be a read-only buffer.
	int32_t command_index;
	RecordedBufferGetDataCommand *command = static_cast<RecordedBufferGetDataCommand *>(_allocate_command(sizeof(RecordedBufferGetDataCommand), command_index));
	command->type = RecordedCommand::TYPE_BUFFER_GET_DATA;
	command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
	command->source = p_src;
	command->destination = p_dst;
	command->region = p_region;

	if (p_src_tracker != nullptr) {
		ResourceUsage usage = RESOURCE_USAGE_COPY_FROM;
		_add_command_to_graph(&p_src_tracker, &usage, 1, command_index, command);
	} else {
		_add_command_to_graph(nullptr, nullptr, 0, command_index, command);
	}
}

void RenderingDeviceGraph::add_buffer_update(RDD::BufferID p_dst, ResourceTracker *p_dst_tracker, VectorView<RecordedBufferCopy> p_buffer_copies) {
	DEV_ASSERT(p_dst_tracker != nullptr);

	size_t buffer_copies_size = p_buffer_copies.size() * sizeof(RecordedBufferCopy);
	uint64_t command_size = sizeof(RecordedBufferUpdateCommand) + buffer_copies_size;
	int32_t command_index;
	RecordedBufferUpdateCommand *command = static_cast<RecordedBufferUpdateCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_BUFFER_UPDATE;
	command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
	command->destination = p_dst;
	command->buffer_copies_count = p_buffer_copies.size();

	RecordedBufferCopy *buffer_copies = command->buffer_copies();
	for (uint32_t i = 0; i < command->buffer_copies_count; i++) {
		buffer_copies[i] = p_buffer_copies[i];
	}

	ResourceUsage buffer_usage = RESOURCE_USAGE_COPY_TO;
	_add_command_to_graph(&p_dst_tracker, &buffer_usage, 1, command_index, command);
}

void RenderingDeviceGraph::add_driver_callback(RDD::DriverCallback p_callback, void *p_userdata, VectorView<ResourceTracker *> p_trackers, VectorView<RenderingDeviceGraph::ResourceUsage> p_usages, BitField<RDD::PipelineStageBits> p_stages) {
	DEV_ASSERT(p_trackers.size() == p_usages.size());

	int32_t command_index;
	RecordedDriverCallbackCommand *command = static_cast<RecordedDriverCallbackCommand *>(_allocate_command(sizeof(RecordedDriverCallbackCommand), command_index));
	command->type = RecordedCommand::TYPE_DRIVER_CALLBACK;
	command->callback = p_callback;
	command->userdata = p_userdata;
	// GodotNext: pu_core_rd_callback_stages (#101). The callback's own stage seeds the barriers on
	// both sides, exactly like a compute or draw command.
	command->self_stages = p_stages;
	_add_command_to_graph((ResourceTracker **)p_trackers.ptr(), (ResourceUsage *)p_usages.ptr(), p_trackers.size(), command_index, command);
}

void RenderingDeviceGraph::command_get_stages(int32_t p_command_index, BitField<RDD::PipelineStageBits> &r_self, BitField<RDD::PipelineStageBits> &r_previous, BitField<RDD::PipelineStageBits> &r_next) const {
	DEV_ASSERT(p_command_index >= 0 && p_command_index < command_count);
	const RecordedCommand *command = reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offsets[p_command_index]]);
	r_self = command->self_stages;
	r_previous = command->previous_stages;
	r_next = command->next_stages;
}

// GodotNext: rd_clear_values (#275)
void RenderingDeviceGraph::draw_list_attachments_for_test(int32_t p_command_index, LocalVector<RDD::AttachmentLoadOp> &r_load_ops, LocalVector<RDD::RenderPassClearValue> &r_clear_values) const {
	r_load_ops.clear();
	r_clear_values.clear();
	ERR_FAIL_COND(p_command_index < 0 || uint32_t(p_command_index) >= command_count);
	const RecordedCommand *command = reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offsets[p_command_index]]);
	ERR_FAIL_COND(command->type != RecordedCommand::TYPE_DRAW_LIST);
	const RecordedDrawListCommand *draw_list_command = reinterpret_cast<const RecordedDrawListCommand *>(command);
	for (uint32_t i = 0; i < draw_list_command->trackers_count; i++) {
		r_load_ops.push_back(draw_list_command->load_ops()[i]);
	}
	for (uint32_t i = 0; i < draw_list_command->clear_values_count; i++) {
		r_clear_values.push_back(draw_list_command->clear_values()[i]);
	}
}

void RenderingDeviceGraph::add_raytracing_list_begin() {
	raytracing_instruction_list.clear();
	raytracing_instruction_list.index++;
}

void RenderingDeviceGraph::add_raytracing_list_bind_pipeline(RDD::RaytracingPipelineID p_pipeline) {
	RaytracingListBindPipelineInstruction *instruction = reinterpret_cast<RaytracingListBindPipelineInstruction *>(_allocate_raytracing_list_instruction(sizeof(RaytracingListBindPipelineInstruction)));
	instruction->type = RaytracingListInstruction::TYPE_BIND_PIPELINE;
	instruction->pipeline = p_pipeline;
	raytracing_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_RAY_TRACING_SHADER_BIT);
}

void RenderingDeviceGraph::add_raytracing_list_bind_uniform_set(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index) {
	RaytracingListBindUniformSetInstruction *instruction = reinterpret_cast<RaytracingListBindUniformSetInstruction *>(_allocate_raytracing_list_instruction(sizeof(RaytracingListBindUniformSetInstruction)));
	instruction->type = RaytracingListInstruction::TYPE_BIND_UNIFORM_SET;
	instruction->shader = p_shader;
	instruction->uniform_set = p_uniform_set;
	instruction->set_index = set_index;
}

void RenderingDeviceGraph::add_raytracing_list_set_push_constant(RDD::ShaderID p_shader, const void *p_data, uint32_t p_data_size) {
	uint32_t instruction_size = sizeof(RaytracingListSetPushConstantInstruction) + p_data_size;
	RaytracingListSetPushConstantInstruction *instruction = reinterpret_cast<RaytracingListSetPushConstantInstruction *>(_allocate_raytracing_list_instruction(instruction_size));
	instruction->type = RaytracingListInstruction::TYPE_SET_PUSH_CONSTANT;
	instruction->size = p_data_size;
	instruction->shader = p_shader;
	memcpy(instruction->data(), p_data, p_data_size);
}

void RenderingDeviceGraph::add_raytracing_list_trace_rays(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) {
	RaytracingListTraceRaysInstruction *instruction = reinterpret_cast<RaytracingListTraceRaysInstruction *>(_allocate_raytracing_list_instruction(sizeof(RaytracingListTraceRaysInstruction)));
	instruction->type = RaytracingListInstruction::TYPE_TRACE_RAYS;
	instruction->raygen_sbt = p_raygen_sbt;
	instruction->miss_sbt = p_miss_sbt;
	instruction->hit_sbt = p_hit_sbt;
	instruction->width = p_width;
	instruction->height = p_height;
	instruction->depth = p_depth;
}

// GodotNext: rd_rt_pipelines (#179)
bool RenderingDeviceGraph::add_raytracing_list_trace_rays_indirect(const RDD::ShaderBindingTable &p_raygen_sbt, const RDD::ShaderBindingTable &p_miss_sbt, const RDD::ShaderBindingTable &p_hit_sbt, RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint64_t p_offset) {
	// As in add_draw_list_draw_indirect_count() (#177): a tracker keeps the first usage it gets in a raytracing
	// list (add_raytracing_list_usage()), so a launch-size buffer this list already uses another way, such as
	// from a uniform set, would get no indirect-argument barrier.
	if (p_buffer_tracker != nullptr) {
		p_buffer_tracker->reset_if_outdated(tracking_frame);
		ERR_FAIL_COND_V_MSG(p_buffer_tracker->raytracing_list_index == raytracing_instruction_list.index && p_buffer_tracker->raytracing_list_usage != RESOURCE_USAGE_INDIRECT_BUFFER_READ, false,
				vformat("The launch-size buffer of an indirect trace is also used in this raytracing list (%s). A buffer has one usage per raytracing list, so it would not be synchronized for the indirect read; bind it in none of this raytracing list's uniform sets.", _usage_to_string(p_buffer_tracker->raytracing_list_usage)));
		// GodotNext: rd_bindless (#176). The same for a dirty resident of a bindless table the list binds (the
		// tables of this trace are bound by now), whose table read becomes a usage at the list's end.
		ERR_FAIL_COND_V_MSG(_bindless_dirty_in_list_tables(raytracing_instruction_list, p_buffer_tracker), false,
				"The launch-size buffer of an indirect trace is in a bindless table that this raytracing list binds, and the table has not read it since it was last written. A buffer has one usage per raytracing list, so the table's read would not be synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table.");
	}

	RaytracingListTraceRaysIndirectInstruction *instruction = reinterpret_cast<RaytracingListTraceRaysIndirectInstruction *>(_allocate_raytracing_list_instruction(sizeof(RaytracingListTraceRaysIndirectInstruction)));
	instruction->type = RaytracingListInstruction::TYPE_TRACE_RAYS_INDIRECT;
	instruction->raygen_sbt = p_raygen_sbt;
	instruction->miss_sbt = p_miss_sbt;
	instruction->hit_sbt = p_hit_sbt;
	instruction->buffer = p_buffer;
	instruction->offset = p_offset;
	raytracing_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);

	if (p_buffer_tracker != nullptr) {
		add_raytracing_list_usage(p_buffer_tracker, RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	}
	return true;
}

void RenderingDeviceGraph::add_raytracing_list_uniform_set_prepare_for_use(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index) {
	RaytracingListUniformSetPrepareForUseInstruction *instruction = reinterpret_cast<RaytracingListUniformSetPrepareForUseInstruction *>(_allocate_raytracing_list_instruction(sizeof(RaytracingListUniformSetPrepareForUseInstruction)));
	instruction->type = RaytracingListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE;
	instruction->shader = p_shader;
	instruction->uniform_set = p_uniform_set;
	instruction->set_index = set_index;
}

void RenderingDeviceGraph::add_raytracing_list_usage(ResourceTracker *p_tracker, ResourceUsage p_usage) {
	DEV_ASSERT(p_tracker != nullptr);

	p_tracker->reset_if_outdated(tracking_frame);

	if (p_tracker->raytracing_list_index != raytracing_instruction_list.index) {
		raytracing_instruction_list.command_trackers.push_back(p_tracker);
		raytracing_instruction_list.command_tracker_usages.push_back(p_usage);
		p_tracker->raytracing_list_index = raytracing_instruction_list.index;
		p_tracker->raytracing_list_usage = p_usage;
	}
#ifdef DEV_ENABLED
	else if (p_tracker->raytracing_list_usage != p_usage) {
		ERR_FAIL_MSG(vformat("Tracker can't have more than one type of usage in the same raytracing list. Raytracing list usage is %d and the requested usage is %d.", p_tracker->raytracing_list_usage, p_usage));
	}
#endif
}

void RenderingDeviceGraph::add_raytracing_list_usages(VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages) {
	DEV_ASSERT(p_trackers.size() == p_usages.size());

	for (uint32_t i = 0; i < p_trackers.size(); i++) {
		add_raytracing_list_usage(p_trackers[i], p_usages[i]);
	}
}

void RenderingDeviceGraph::add_raytracing_list_end() {
	int32_t command_index;
	uint32_t instruction_data_size = raytracing_instruction_list.data.size();
	uint32_t command_size = sizeof(RecordedRaytracingListCommand) + instruction_data_size;
	RecordedRaytracingListCommand *command = static_cast<RecordedRaytracingListCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_RAYTRACING_LIST;
	command->self_stages = raytracing_instruction_list.stages;
	command->instruction_data_size = instruction_data_size;
	memcpy(command->instruction_data(), raytracing_instruction_list.data.ptr(), instruction_data_size);
	// GodotNext: rd_bindless (#176), rd_rt_pipelines (#179).
	_bindless_raytracing_list_tables_add_usages(command_index);
	_add_command_to_graph(raytracing_instruction_list.command_trackers.ptr(), raytracing_instruction_list.command_tracker_usages.ptr(), raytracing_instruction_list.command_trackers.size(), command_index, command);
	_bindless_tables_bound(raytracing_instruction_list, command_index, command);
}

// GodotNext: async_compute (#174). p_queue tags the list for the async compute queue.
void RenderingDeviceGraph::add_compute_list_begin(RDD::BreadcrumbMarker p_phase, uint32_t p_breadcrumb_data, Queue p_queue) {
	compute_instruction_list.clear();
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	compute_instruction_list.breadcrumb = p_breadcrumb_data | (p_phase & ((1 << 16) - 1));
#endif
	// GodotNext: async_compute (#174).
	compute_instruction_list.queue = p_queue;
	compute_instruction_list.index++;
}

void RenderingDeviceGraph::add_compute_list_bind_pipeline(RDD::PipelineID p_pipeline) {
	ComputeListBindPipelineInstruction *instruction = reinterpret_cast<ComputeListBindPipelineInstruction *>(_allocate_compute_list_instruction(sizeof(ComputeListBindPipelineInstruction)));
	instruction->type = ComputeListInstruction::TYPE_BIND_PIPELINE;
	instruction->pipeline = p_pipeline;
	compute_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void RenderingDeviceGraph::add_compute_list_bind_uniform_set(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index) {
	add_compute_list_bind_uniform_sets(p_shader, VectorView(&p_uniform_set, 1), set_index, 1);
}

void RenderingDeviceGraph::add_compute_list_bind_uniform_sets(RDD::ShaderID p_shader, VectorView<RDD::UniformSetID> p_uniform_sets, uint32_t p_first_set_index, uint32_t p_set_count) {
	DEV_ASSERT(p_uniform_sets.size() >= p_set_count);

	uint32_t instruction_size = sizeof(ComputeListBindUniformSetsInstruction) + sizeof(RDD::UniformSetID) * p_set_count;
	ComputeListBindUniformSetsInstruction *instruction = reinterpret_cast<ComputeListBindUniformSetsInstruction *>(_allocate_compute_list_instruction(instruction_size));
	instruction->type = ComputeListInstruction::TYPE_BIND_UNIFORM_SETS;
	instruction->shader = p_shader;
	instruction->first_set_index = p_first_set_index;
	instruction->set_count = p_set_count;
	instruction->dynamic_offsets_mask = driver->uniform_sets_get_dynamic_offsets(p_uniform_sets, p_shader, p_first_set_index, p_set_count);

	RDD::UniformSetID *ids = instruction->uniform_set_ids();
	for (uint32_t i = 0; i < p_set_count; i++) {
		ids[i] = p_uniform_sets[i];
	}
}

void RenderingDeviceGraph::add_compute_list_dispatch(uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) {
	ComputeListDispatchInstruction *instruction = reinterpret_cast<ComputeListDispatchInstruction *>(_allocate_compute_list_instruction(sizeof(ComputeListDispatchInstruction)));
	instruction->type = ComputeListInstruction::TYPE_DISPATCH;
	instruction->x_groups = p_x_groups;
	instruction->y_groups = p_y_groups;
	instruction->z_groups = p_z_groups;
}

void RenderingDeviceGraph::add_compute_list_dispatch_indirect(RDD::BufferID p_buffer, uint32_t p_offset) {
	ComputeListDispatchIndirectInstruction *instruction = reinterpret_cast<ComputeListDispatchIndirectInstruction *>(_allocate_compute_list_instruction(sizeof(ComputeListDispatchIndirectInstruction)));
	instruction->type = ComputeListInstruction::TYPE_DISPATCH_INDIRECT;
	instruction->buffer = p_buffer;
	instruction->offset = p_offset;
	compute_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

void RenderingDeviceGraph::add_compute_list_set_push_constant(RDD::ShaderID p_shader, const void *p_data, uint32_t p_data_size) {
	uint32_t instruction_size = sizeof(ComputeListSetPushConstantInstruction) + p_data_size;
	ComputeListSetPushConstantInstruction *instruction = reinterpret_cast<ComputeListSetPushConstantInstruction *>(_allocate_compute_list_instruction(instruction_size));
	instruction->type = ComputeListInstruction::TYPE_SET_PUSH_CONSTANT;
	instruction->size = p_data_size;
	instruction->shader = p_shader;
	memcpy(instruction->data(), p_data, p_data_size);
}

void RenderingDeviceGraph::add_compute_list_uniform_set_prepare_for_use(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index) {
	ComputeListUniformSetPrepareForUseInstruction *instruction = reinterpret_cast<ComputeListUniformSetPrepareForUseInstruction *>(_allocate_compute_list_instruction(sizeof(ComputeListUniformSetPrepareForUseInstruction)));
	instruction->type = ComputeListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE;
	instruction->shader = p_shader;
	instruction->uniform_set = p_uniform_set;
	instruction->set_index = set_index;
}

void RenderingDeviceGraph::add_compute_list_usage(ResourceTracker *p_tracker, ResourceUsage p_usage) {
	DEV_ASSERT(p_tracker != nullptr);

	p_tracker->reset_if_outdated(tracking_frame);

	if (p_tracker->compute_list_index != compute_instruction_list.index) {
		compute_instruction_list.command_trackers.push_back(p_tracker);
		compute_instruction_list.command_tracker_usages.push_back(p_usage);
		p_tracker->compute_list_index = compute_instruction_list.index;
		p_tracker->compute_list_usage = p_usage;
	}
#ifdef DEV_ENABLED
	else if (p_tracker->compute_list_usage != p_usage) {
		ERR_FAIL_MSG(vformat("Tracker can't have more than one type of usage in the same compute list. Compute list usage is %s and the requested usage is %s.", _usage_to_string(p_tracker->compute_list_usage), _usage_to_string(p_usage)));
	}
#endif
}

void RenderingDeviceGraph::add_compute_list_usages(VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages) {
	DEV_ASSERT(p_trackers.size() == p_usages.size());

	for (uint32_t i = 0; i < p_trackers.size(); i++) {
		add_compute_list_usage(p_trackers[i], p_usages[i]);
	}
}

void RenderingDeviceGraph::add_compute_list_end() {
	int32_t command_index;
	uint32_t instruction_data_size = compute_instruction_list.data.size();
	uint32_t command_size = sizeof(RecordedComputeListCommand) + instruction_data_size;
	RecordedComputeListCommand *command = static_cast<RecordedComputeListCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_COMPUTE_LIST;
	command->self_stages = compute_instruction_list.stages;
	// GodotNext: async_compute (#174).
	command->queue = compute_instruction_list.queue;
	if (compute_instruction_list.queue == QUEUE_ASYNC_COMPUTE) {
		async_command_count++;
	}
	command->instruction_data_size = instruction_data_size;
	memcpy(command->instruction_data(), compute_instruction_list.data.ptr(), instruction_data_size);
	// GodotNext: rd_bindless (#176).
	_bindless_tables_add_usages(compute_instruction_list, &RenderingDeviceGraph::add_compute_list_usage);
	_add_command_to_graph(compute_instruction_list.command_trackers.ptr(), compute_instruction_list.command_tracker_usages.ptr(), compute_instruction_list.command_trackers.size(), command_index, command);
	_bindless_tables_bound(compute_instruction_list, command_index, command);
}

void RenderingDeviceGraph::add_draw_list_begin(FramebufferCache *p_framebuffer_cache, Rect2i p_region, VectorView<AttachmentOperation> p_attachment_operations, VectorView<RDD::RenderPassClearValue> p_attachment_clear_values, BitField<RDD::PipelineStageBits> p_stages, uint32_t p_breadcrumb, bool p_split_cmd_buffer) {
	_add_draw_list_begin(p_framebuffer_cache, RDD::RenderPassID(), RDD::FramebufferID(), p_region, p_attachment_operations, p_attachment_clear_values, p_stages, p_breadcrumb, p_split_cmd_buffer);
}

void RenderingDeviceGraph::add_draw_list_begin(RDD::RenderPassID p_render_pass, RDD::FramebufferID p_framebuffer, Rect2i p_region, VectorView<AttachmentOperation> p_attachment_operations, VectorView<RDD::RenderPassClearValue> p_attachment_clear_values, BitField<RDD::PipelineStageBits> p_stages, uint32_t p_breadcrumb, bool p_split_cmd_buffer) {
	_add_draw_list_begin(nullptr, p_render_pass, p_framebuffer, p_region, p_attachment_operations, p_attachment_clear_values, p_stages, p_breadcrumb, p_split_cmd_buffer);
}

void RenderingDeviceGraph::add_draw_list_bind_index_buffer(RDD::BufferID p_buffer, RDD::IndexBufferFormat p_format, uint32_t p_offset) {
	DrawListBindIndexBufferInstruction *instruction = reinterpret_cast<DrawListBindIndexBufferInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListBindIndexBufferInstruction)));
	instruction->type = DrawListInstruction::TYPE_BIND_INDEX_BUFFER;
	instruction->buffer = p_buffer;
	instruction->format = p_format;
	instruction->offset = p_offset;

	if (instruction->buffer.id != 0) {
		draw_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_VERTEX_INPUT_BIT);
	}
}

void RenderingDeviceGraph::add_draw_list_bind_pipeline(RDD::PipelineID p_pipeline, BitField<RDD::PipelineStageBits> p_pipeline_stage_bits) {
	DrawListBindPipelineInstruction *instruction = reinterpret_cast<DrawListBindPipelineInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListBindPipelineInstruction)));
	instruction->type = DrawListInstruction::TYPE_BIND_PIPELINE;
	instruction->pipeline = p_pipeline;
	draw_instruction_list.stages = draw_instruction_list.stages | p_pipeline_stage_bits;

	workarounds_state.bound_any_draw_list_pipeline = true;
}

void RenderingDeviceGraph::add_draw_list_bind_uniform_set(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index) {
	add_draw_list_bind_uniform_sets(p_shader, VectorView(&p_uniform_set, 1), set_index, 1);
}

void RenderingDeviceGraph::add_draw_list_bind_uniform_sets(RDD::ShaderID p_shader, VectorView<RDD::UniformSetID> p_uniform_sets, uint32_t p_first_index, uint32_t p_set_count) {
	DEV_ASSERT(p_uniform_sets.size() >= p_set_count);

	uint32_t instruction_size = sizeof(DrawListBindUniformSetsInstruction) + sizeof(RDD::UniformSetID) * p_set_count;
	DrawListBindUniformSetsInstruction *instruction = reinterpret_cast<DrawListBindUniformSetsInstruction *>(_allocate_draw_list_instruction(instruction_size));
	instruction->type = DrawListInstruction::TYPE_BIND_UNIFORM_SETS;
	instruction->shader = p_shader;
	instruction->first_set_index = p_first_index;
	instruction->set_count = p_set_count;
	instruction->dynamic_offsets_mask = driver->uniform_sets_get_dynamic_offsets(p_uniform_sets, p_shader, p_first_index, p_set_count);

	for (uint32_t i = 0; i < p_set_count; i++) {
		instruction->uniform_set_ids()[i] = p_uniform_sets[i];
	}
}

void RenderingDeviceGraph::add_draw_list_bind_vertex_buffers(Span<RDD::BufferID> p_vertex_buffers, Span<uint64_t> p_vertex_buffer_offsets) {
	DEV_ASSERT(p_vertex_buffers.size() == p_vertex_buffer_offsets.size());

	uint32_t instruction_size = sizeof(DrawListBindVertexBuffersInstruction) + sizeof(RDD::BufferID) * p_vertex_buffers.size() + sizeof(uint64_t) * p_vertex_buffer_offsets.size();
	DrawListBindVertexBuffersInstruction *instruction = reinterpret_cast<DrawListBindVertexBuffersInstruction *>(_allocate_draw_list_instruction(instruction_size));
	instruction->type = DrawListInstruction::TYPE_BIND_VERTEX_BUFFERS;
	instruction->vertex_buffers_count = p_vertex_buffers.size();
	instruction->dynamic_offsets_mask = driver->buffer_get_dynamic_offsets(p_vertex_buffers);

	RDD::BufferID *vertex_buffers = instruction->vertex_buffers();
	uint64_t *vertex_buffer_offsets = instruction->vertex_buffer_offsets();
	for (uint32_t i = 0; i < instruction->vertex_buffers_count; i++) {
		vertex_buffers[i] = p_vertex_buffers[i];
		vertex_buffer_offsets[i] = p_vertex_buffer_offsets[i];
	}

	if (instruction->vertex_buffers_count > 0) {
		draw_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_VERTEX_INPUT_BIT);
	}
}

void RenderingDeviceGraph::add_draw_list_clear_attachments(VectorView<RDD::AttachmentClear> p_attachments_clear, VectorView<Rect2i> p_attachments_clear_rect) {
	uint32_t instruction_size = sizeof(DrawListClearAttachmentsInstruction) + sizeof(RDD::AttachmentClear) * p_attachments_clear.size() + sizeof(Rect2i) * p_attachments_clear_rect.size();
	DrawListClearAttachmentsInstruction *instruction = reinterpret_cast<DrawListClearAttachmentsInstruction *>(_allocate_draw_list_instruction(instruction_size));
	instruction->type = DrawListInstruction::TYPE_CLEAR_ATTACHMENTS;
	instruction->attachments_clear_count = p_attachments_clear.size();
	instruction->attachments_clear_rect_count = p_attachments_clear_rect.size();

	RDD::AttachmentClear *attachments_clear = instruction->attachments_clear();
	Rect2i *attachments_clear_rect = instruction->attachments_clear_rect();
	for (uint32_t i = 0; i < instruction->attachments_clear_count; i++) {
		attachments_clear[i] = p_attachments_clear[i];
	}

	for (uint32_t i = 0; i < instruction->attachments_clear_rect_count; i++) {
		attachments_clear_rect[i] = p_attachments_clear_rect[i];
	}
}

void RenderingDeviceGraph::add_draw_list_draw(uint32_t p_vertex_count, uint32_t p_instance_count) {
	DrawListDrawInstruction *instruction = reinterpret_cast<DrawListDrawInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawInstruction)));
	instruction->type = DrawListInstruction::TYPE_DRAW;
	instruction->vertex_count = p_vertex_count;
	instruction->instance_count = p_instance_count;
}

void RenderingDeviceGraph::add_draw_list_draw_indexed(uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index) {
	DrawListDrawIndexedInstruction *instruction = reinterpret_cast<DrawListDrawIndexedInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawIndexedInstruction)));
	instruction->type = DrawListInstruction::TYPE_DRAW_INDEXED;
	instruction->index_count = p_index_count;
	instruction->instance_count = p_instance_count;
	instruction->first_index = p_first_index;
}

void RenderingDeviceGraph::add_draw_list_draw_indirect(RDD::BufferID p_buffer, uint32_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	DrawListDrawIndirectInstruction *instruction = reinterpret_cast<DrawListDrawIndirectInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawIndirectInstruction)));
	instruction->type = DrawListInstruction::TYPE_DRAW_INDIRECT;
	instruction->buffer = p_buffer;
	instruction->offset = p_offset;
	instruction->draw_count = p_draw_count;
	instruction->stride = p_stride;
	draw_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

void RenderingDeviceGraph::add_draw_list_draw_indexed_indirect(RDD::BufferID p_buffer, uint32_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	DrawListDrawIndexedIndirectInstruction *instruction = reinterpret_cast<DrawListDrawIndexedIndirectInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawIndexedIndirectInstruction)));
	instruction->type = DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT;
	instruction->buffer = p_buffer;
	instruction->offset = p_offset;
	instruction->draw_count = p_draw_count;
	instruction->stride = p_stride;
	draw_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

// GodotNext: rd_indirect_count (#177)
bool RenderingDeviceGraph::add_draw_list_draw_indirect_count(bool p_indexed, RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, RDD::BufferID p_count_buffer, ResourceTracker *p_count_buffer_tracker, uint32_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	// GodotNext: rd_mesh_shaders (#178). Shared with the mesh task count draws.
	return _add_draw_list_draw_indirect_count(p_indexed ? DrawListInstruction::TYPE_DRAW_INDEXED_INDIRECT_COUNT : DrawListInstruction::TYPE_DRAW_INDIRECT_COUNT, p_buffer, p_buffer_tracker, p_offset, p_count_buffer, p_count_buffer_tracker, p_count_buffer_offset, p_max_draw_count, p_stride);
}

// GodotNext: rd_mesh_shaders (#178). Moved out of add_draw_list_draw_indirect_count() (#177).
bool RenderingDeviceGraph::_draw_list_indirect_buffer_usable(ResourceTracker *p_tracker, const char *p_role, const char *p_draw) {
	// A tracker keeps the first usage it gets in a draw list (add_draw_list_usage()). A records or count buffer
	// this draw list already uses another way, such as from a uniform set, would get no indirect-argument barrier.
	if (p_tracker == nullptr) {
		return true;
	}
	p_tracker->reset_if_outdated(tracking_frame);
	ERR_FAIL_COND_V_MSG(p_tracker->draw_list_index == draw_instruction_list.index && p_tracker->draw_list_usage != RESOURCE_USAGE_INDIRECT_BUFFER_READ, false,
			vformat("The %s buffer of an %s is also used in this draw list (%s). A buffer has one usage per draw list, so it would not be synchronized for the indirect read; bind it in none of this draw list's uniform sets.", p_role, p_draw, _usage_to_string(p_tracker->draw_list_usage)));
	// GodotNext: rd_bindless (#176). The same for a dirty resident of a bindless table the draw list binds
	// (the tables of this draw are bound by now), whose table read becomes a usage at the list's end. A
	// resident that is not dirty needs no barrier for the table: the indirect read does not write it.
	ERR_FAIL_COND_V_MSG(_bindless_dirty_in_list_tables(draw_instruction_list, p_tracker), false,
			vformat("The %s buffer of an %s is in a bindless table that this draw list binds, and the table has not read it since it was last written. A buffer has one usage per draw list, so the table's read would not be synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table.", p_role, p_draw));
	return true;
}

// GodotNext: rd_mesh_shaders (#178). The body of #177's add_draw_list_draw_indirect_count(), for any count draw type.
bool RenderingDeviceGraph::_add_draw_list_draw_indirect_count(DrawListInstruction::Type p_type, RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, RDD::BufferID p_count_buffer, ResourceTracker *p_count_buffer_tracker, uint32_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	if (!_draw_list_indirect_buffer_usable(p_buffer_tracker, "indirect", "indirect count draw") || !_draw_list_indirect_buffer_usable(p_count_buffer_tracker, "count", "indirect count draw")) {
		return false;
	}

	DrawListDrawIndirectCountInstruction *instruction = reinterpret_cast<DrawListDrawIndirectCountInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawIndirectCountInstruction)));
	instruction->type = p_type;
	instruction->buffer = p_buffer;
	instruction->count_buffer = p_count_buffer;
	instruction->offset = p_offset;
	instruction->count_buffer_offset = p_count_buffer_offset;
	instruction->max_draw_count = p_max_draw_count;
	instruction->stride = p_stride;
	draw_instruction_list.indirect_count_offsets.push_back(uint32_t(reinterpret_cast<uint8_t *>(instruction) - draw_instruction_list.data.ptr()));
	draw_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);

	// The count is read like the records: as an indirect argument, before the draws.
	if (p_buffer_tracker != nullptr) {
		add_draw_list_usage(p_buffer_tracker, RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	}
	if (p_count_buffer_tracker != nullptr && p_count_buffer_tracker != p_buffer_tracker) {
		add_draw_list_usage(p_count_buffer_tracker, RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	}
	return true;
}

// GodotNext: rd_mesh_shaders (#178)
void RenderingDeviceGraph::add_draw_list_draw_mesh_tasks(uint32_t p_group_count_x, uint32_t p_group_count_y, uint32_t p_group_count_z) {
	DrawListDrawMeshTasksInstruction *instruction = reinterpret_cast<DrawListDrawMeshTasksInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawMeshTasksInstruction)));
	instruction->type = DrawListInstruction::TYPE_DRAW_MESH_TASKS;
	instruction->group_count[0] = p_group_count_x;
	instruction->group_count[1] = p_group_count_y;
	instruction->group_count[2] = p_group_count_z;
}

// GodotNext: rd_mesh_shaders (#178)
bool RenderingDeviceGraph::add_draw_list_draw_mesh_tasks_indirect(RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	if (!_draw_list_indirect_buffer_usable(p_buffer_tracker, "indirect", "indirect mesh task draw")) {
		return false;
	}

	DrawListDrawMeshTasksIndirectInstruction *instruction = reinterpret_cast<DrawListDrawMeshTasksIndirectInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListDrawMeshTasksIndirectInstruction)));
	instruction->type = DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT;
	instruction->buffer = p_buffer;
	instruction->offset = p_offset;
	instruction->draw_count = p_draw_count;
	instruction->stride = p_stride;
	draw_instruction_list.stages.set_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT);

	if (p_buffer_tracker != nullptr) {
		add_draw_list_usage(p_buffer_tracker, RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	}
	return true;
}

// GodotNext: rd_mesh_shaders (#178)
bool RenderingDeviceGraph::add_draw_list_draw_mesh_tasks_indirect_count(RDD::BufferID p_buffer, ResourceTracker *p_buffer_tracker, uint32_t p_offset, RDD::BufferID p_count_buffer, ResourceTracker *p_count_buffer_tracker, uint32_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	return _add_draw_list_draw_indirect_count(DrawListInstruction::TYPE_DRAW_MESH_TASKS_INDIRECT_COUNT, p_buffer, p_buffer_tracker, p_offset, p_count_buffer, p_count_buffer_tracker, p_count_buffer_offset, p_max_draw_count, p_stride);
}

void RenderingDeviceGraph::add_draw_list_execute_commands(RDD::CommandBufferID p_command_buffer) {
	DrawListExecuteCommandsInstruction *instruction = reinterpret_cast<DrawListExecuteCommandsInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListExecuteCommandsInstruction)));
	instruction->type = DrawListInstruction::TYPE_EXECUTE_COMMANDS;
	instruction->command_buffer = p_command_buffer;
}

void RenderingDeviceGraph::add_draw_list_next_subpass(RDD::CommandBufferType p_command_buffer_type) {
	DrawListNextSubpassInstruction *instruction = reinterpret_cast<DrawListNextSubpassInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListNextSubpassInstruction)));
	instruction->type = DrawListInstruction::TYPE_NEXT_SUBPASS;
	instruction->command_buffer_type = p_command_buffer_type;
}

void RenderingDeviceGraph::add_draw_list_set_blend_constants(const Color &p_color) {
	DrawListSetBlendConstantsInstruction *instruction = reinterpret_cast<DrawListSetBlendConstantsInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListSetBlendConstantsInstruction)));
	instruction->type = DrawListInstruction::TYPE_SET_BLEND_CONSTANTS;
	instruction->color = p_color;
}

void RenderingDeviceGraph::add_draw_list_set_line_width(float p_width) {
	DrawListSetLineWidthInstruction *instruction = reinterpret_cast<DrawListSetLineWidthInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListSetLineWidthInstruction)));
	instruction->type = DrawListInstruction::TYPE_SET_LINE_WIDTH;
	instruction->width = p_width;
}

void RenderingDeviceGraph::add_draw_list_set_push_constant(RDD::ShaderID p_shader, const void *p_data, uint32_t p_data_size) {
	uint32_t instruction_size = sizeof(DrawListSetPushConstantInstruction) + p_data_size;
	DrawListSetPushConstantInstruction *instruction = reinterpret_cast<DrawListSetPushConstantInstruction *>(_allocate_draw_list_instruction(instruction_size));
	instruction->type = DrawListInstruction::TYPE_SET_PUSH_CONSTANT;
	instruction->size = p_data_size;
	instruction->shader = p_shader;
	memcpy(instruction->data(), p_data, p_data_size);
}

void RenderingDeviceGraph::add_draw_list_set_scissor(Rect2i p_rect) {
	DrawListSetScissorInstruction *instruction = reinterpret_cast<DrawListSetScissorInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListSetScissorInstruction)));
	instruction->type = DrawListInstruction::TYPE_SET_SCISSOR;
	instruction->rect = p_rect;
}

void RenderingDeviceGraph::add_draw_list_set_viewport(Rect2i p_rect) {
	DrawListSetViewportInstruction *instruction = reinterpret_cast<DrawListSetViewportInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListSetViewportInstruction)));
	instruction->type = DrawListInstruction::TYPE_SET_VIEWPORT;
	instruction->rect = p_rect;
}

void RenderingDeviceGraph::add_draw_list_uniform_set_prepare_for_use(RDD::ShaderID p_shader, RDD::UniformSetID p_uniform_set, uint32_t set_index) {
	DrawListUniformSetPrepareForUseInstruction *instruction = reinterpret_cast<DrawListUniformSetPrepareForUseInstruction *>(_allocate_draw_list_instruction(sizeof(DrawListUniformSetPrepareForUseInstruction)));
	instruction->type = DrawListInstruction::TYPE_UNIFORM_SET_PREPARE_FOR_USE;
	instruction->shader = p_shader;
	instruction->uniform_set = p_uniform_set;
	instruction->set_index = set_index;
}

void RenderingDeviceGraph::add_draw_list_usage(ResourceTracker *p_tracker, ResourceUsage p_usage) {
	p_tracker->reset_if_outdated(tracking_frame);

	if (p_tracker->draw_list_index != draw_instruction_list.index) {
		draw_instruction_list.command_trackers.push_back(p_tracker);
		draw_instruction_list.command_tracker_usages.push_back(p_usage);
		p_tracker->draw_list_index = draw_instruction_list.index;
		p_tracker->draw_list_usage = p_usage;
	}
#ifdef DEV_ENABLED
	else if (p_tracker->draw_list_usage != p_usage) {
		ERR_FAIL_MSG(vformat("Tracker can't have more than one type of usage in the same draw list. Draw list usage is %s and the requested usage is %s.", _usage_to_string(p_tracker->draw_list_usage), _usage_to_string(p_usage)));
	}
#endif
}

void RenderingDeviceGraph::add_draw_list_usages(VectorView<ResourceTracker *> p_trackers, VectorView<ResourceUsage> p_usages) {
	DEV_ASSERT(p_trackers.size() == p_usages.size());

	for (uint32_t i = 0; i < p_trackers.size(); i++) {
		add_draw_list_usage(p_trackers[i], p_usages[i]);
	}
}

void RenderingDeviceGraph::add_draw_list_end() {
	FramebufferCache *framebuffer_cache = draw_instruction_list.framebuffer_cache;
	int32_t command_index;
	uint32_t clear_values_size = sizeof(RDD::RenderPassClearValue) * draw_instruction_list.attachment_clear_values.size();
	uint32_t trackers_count = framebuffer_cache != nullptr ? framebuffer_cache->trackers.size() : 0;
	uint32_t trackers_and_ops_size = (sizeof(ResourceTracker *) + sizeof(RDD::AttachmentLoadOp) + sizeof(RDD::AttachmentStoreOp)) * trackers_count;
	uint32_t instruction_data_size = draw_instruction_list.data.size();
	uint32_t command_size = sizeof(RecordedDrawListCommand) + clear_values_size + trackers_and_ops_size + instruction_data_size;
	// GodotNext: rd_indirect_count (#177). The offsets of the indirect-count draws follow the instructions.
	const uint32_t indirect_count_draws = draw_instruction_list.indirect_count_offsets.size();
	if (indirect_count_draws > 0) {
		command_size += RecordedDrawListCommand::indirect_count_offsets_start(instruction_data_size) - instruction_data_size + indirect_count_draws * sizeof(uint32_t);
	}
	RecordedDrawListCommand *command = static_cast<RecordedDrawListCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_DRAW_LIST;
	command->self_stages = draw_instruction_list.stages;
	command->framebuffer_cache = framebuffer_cache;
	command->render_pass = draw_instruction_list.render_pass;
	command->framebuffer = draw_instruction_list.framebuffer;
	command->instruction_data_size = instruction_data_size;
	command->command_buffer_type = RDD::COMMAND_BUFFER_TYPE_PRIMARY;
	command->region = draw_instruction_list.region;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	command->breadcrumb = draw_instruction_list.breadcrumb;
#endif
	command->split_cmd_buffer = draw_instruction_list.split_cmd_buffer;
	command->clear_values_count = draw_instruction_list.attachment_clear_values.size();
	command->trackers_count = trackers_count;

	RDD::AttachmentStoreOp attachment_store_op_dont_care = RDD::ATTACHMENT_STORE_OP_DONT_CARE;
	if (driver_workarounds.avoid_store_op_dont_care_in_draw_list_with_no_bound_pipeline && !workarounds_state.bound_any_draw_list_pipeline) {
		attachment_store_op_dont_care = RDD::ATTACHMENT_STORE_OP_STORE;
	}

	// Initialize the load and store operations to their default behaviors. The store behavior will be modified if a command depends on the result of this render pass.
	uint32_t attachment_op_count = draw_instruction_list.attachment_operations.size();
	ResourceTracker **trackers = command->trackers();
	RDD::AttachmentLoadOp *load_ops = command->load_ops();
	RDD::AttachmentStoreOp *store_ops = command->store_ops();
	for (uint32_t i = 0; i < command->trackers_count; i++) {
		ResourceTracker *resource_tracker = framebuffer_cache->trackers[i];
		if (resource_tracker != nullptr) {
			// GodotNext: rd_clear_values (#275), rd_readonly_depth (#301). The clear values are not per
			// slot (draw_list_driver_clear_values()). A read-only depth attachment always loads and
			// uses ATTACHMENT_STORE_OP_NONE so no store write races with concurrent readers.
			if (i < attachment_op_count && draw_instruction_list.attachment_operations[i] == ATTACHMENT_OPERATION_READ_ONLY) {
				load_ops[i] = RDD::ATTACHMENT_LOAD_OP_LOAD;
				store_ops[i] = RDD::ATTACHMENT_STORE_OP_NONE;
			} else {
				if (i < attachment_op_count && draw_instruction_list.attachment_operations[i] == ATTACHMENT_OPERATION_CLEAR) {
					load_ops[i] = RDD::ATTACHMENT_LOAD_OP_CLEAR;
				} else if (i < attachment_op_count && draw_instruction_list.attachment_operations[i] == ATTACHMENT_OPERATION_IGNORE) {
					load_ops[i] = RDD::ATTACHMENT_LOAD_OP_DONT_CARE;
				} else if (resource_tracker->is_discardable) {
					bool resource_has_parent = resource_tracker->parent != nullptr;
					ResourceTracker *search_tracker = resource_has_parent ? resource_tracker->parent : resource_tracker;
					search_tracker->reset_if_outdated(tracking_frame);
					bool resource_was_modified_this_frame = search_tracker->write_command_or_list_index >= 0;
					load_ops[i] = resource_was_modified_this_frame ? RDD::ATTACHMENT_LOAD_OP_LOAD : RDD::ATTACHMENT_LOAD_OP_DONT_CARE;
				} else {
					load_ops[i] = RDD::ATTACHMENT_LOAD_OP_LOAD;
				}

				store_ops[i] = resource_tracker->is_discardable ? attachment_store_op_dont_care : RDD::ATTACHMENT_STORE_OP_STORE;
			}
		} else {
			load_ops[i] = RDD::ATTACHMENT_LOAD_OP_DONT_CARE;
			store_ops[i] = RDD::ATTACHMENT_STORE_OP_DONT_CARE;
		}

		trackers[i] = resource_tracker;
	}

	RDD::RenderPassClearValue *clear_values = command->clear_values();
	for (uint32_t i = 0; i < command->clear_values_count; i++) {
		clear_values[i] = draw_instruction_list.attachment_clear_values[i];
	}

	memcpy(command->instruction_data(), draw_instruction_list.data.ptr(), instruction_data_size);
	// GodotNext: rd_indirect_count (#177)
	command->indirect_count_draws = indirect_count_draws;
	if (indirect_count_draws > 0) {
		memcpy(command->indirect_count_offsets(), draw_instruction_list.indirect_count_offsets.ptr(), indirect_count_draws * sizeof(uint32_t));
	}
	// GodotNext: rd_bindless (#176), rd_indirect_count (#177), rd_readonly_depth (#301).
	_bindless_draw_list_tables_add_usages(command_index, command);
	_add_command_to_graph(draw_instruction_list.command_trackers.ptr(), draw_instruction_list.command_tracker_usages.ptr(), draw_instruction_list.command_trackers.size(), command_index, command);
	draw_instruction_list.bindless_readonly_depth_trackers.clear();
	_bindless_tables_bound(draw_instruction_list, command_index, command);
}

void RenderingDeviceGraph::add_texture_clear_color(RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, const Color &p_color, const RDD::TextureSubresourceRange &p_range) {
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	RecordedTextureClearColorCommand *command = static_cast<RecordedTextureClearColorCommand *>(_allocate_command(sizeof(RecordedTextureClearColorCommand), command_index));
	command->type = RecordedCommand::TYPE_TEXTURE_CLEAR_COLOR;
	command->texture = p_dst;
	command->color = p_color;
	command->range = p_range;

	ResourceUsage usage;
	if (driver_texture_clears_with_copy_engine) {
		command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
		usage = RESOURCE_USAGE_COPY_TO;
	} else {
		// If the driver is uncapable of using the copy engine for clearing the image (e.g. D3D12), we must either transition the
		// resource to a render target or a storage image as that's the only two ways it can perform the operation.
		if (p_dst_tracker->texture_usage & RDD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT) {
			command->self_stages = RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			usage = RESOURCE_USAGE_ATTACHMENT_COLOR_READ_WRITE;
		} else {
			command->self_stages = RDD::PIPELINE_STAGE_CLEAR_STORAGE_BIT;
			usage = RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
		}
	}

	_add_command_to_graph(&p_dst_tracker, &usage, 1, command_index, command);
}

void RenderingDeviceGraph::add_texture_clear_depth_stencil(RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, float p_depth, uint8_t p_stencil, const RDD::TextureSubresourceRange &p_range) {
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	RecordedTextureClearDepthStencilCommand *command = static_cast<RecordedTextureClearDepthStencilCommand *>(_allocate_command(sizeof(RecordedTextureClearDepthStencilCommand), command_index));
	command->type = RecordedCommand::TYPE_TEXTURE_CLEAR_DEPTH_STENCIL;
	command->texture = p_dst;
	command->depth = p_depth;
	command->stencil = p_stencil;
	command->range = p_range;

	ResourceUsage usage;
	if (driver_texture_clears_with_copy_engine) {
		command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
		usage = RESOURCE_USAGE_COPY_TO;
	} else {
		// If the driver is uncapable of using the copy engine for clearing the image (e.g. D3D12), we must transition the
		// resource to a depth stencil as that's the only way it can perform the operation.
		command->self_stages = RDD::PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | RDD::PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		usage = RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ_WRITE;
	}

	_add_command_to_graph(&p_dst_tracker, &usage, 1, command_index, command);
}

void RenderingDeviceGraph::add_texture_copy(RDD::TextureID p_src, ResourceTracker *p_src_tracker, RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, VectorView<RDD::TextureCopyRegion> p_texture_copy_regions) {
	DEV_ASSERT(p_src_tracker != nullptr);
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	uint64_t command_size = sizeof(RecordedTextureCopyCommand) + p_texture_copy_regions.size() * sizeof(RDD::TextureCopyRegion);
	RecordedTextureCopyCommand *command = static_cast<RecordedTextureCopyCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_TEXTURE_COPY;
	command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
	command->from_texture = p_src;
	command->to_texture = p_dst;
	command->texture_copy_regions_count = p_texture_copy_regions.size();

	RDD::TextureCopyRegion *texture_copy_regions = command->texture_copy_regions();
	for (uint32_t i = 0; i < command->texture_copy_regions_count; i++) {
		texture_copy_regions[i] = p_texture_copy_regions[i];
	}

	ResourceTracker *trackers[2] = { p_dst_tracker, p_src_tracker };
	ResourceUsage usages[2] = { RESOURCE_USAGE_COPY_TO, RESOURCE_USAGE_COPY_FROM };
	_add_command_to_graph(trackers, usages, 2, command_index, command);
}

void RenderingDeviceGraph::add_texture_get_data(RDD::TextureID p_src, ResourceTracker *p_src_tracker, RDD::BufferID p_dst, VectorView<RDD::BufferTextureCopyRegion> p_buffer_texture_copy_regions, ResourceTracker *p_dst_tracker) {
	DEV_ASSERT(p_src_tracker != nullptr);

	int32_t command_index;
	uint64_t command_size = sizeof(RecordedTextureGetDataCommand) + p_buffer_texture_copy_regions.size() * sizeof(RDD::BufferTextureCopyRegion);
	RecordedTextureGetDataCommand *command = static_cast<RecordedTextureGetDataCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_TEXTURE_GET_DATA;
	command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
	command->from_texture = p_src;
	command->to_buffer = p_dst;
	command->buffer_texture_copy_regions_count = p_buffer_texture_copy_regions.size();

	RDD::BufferTextureCopyRegion *buffer_texture_copy_regions = command->buffer_texture_copy_regions();
	for (uint32_t i = 0; i < command->buffer_texture_copy_regions_count; i++) {
		buffer_texture_copy_regions[i] = p_buffer_texture_copy_regions[i];
	}

	if (p_dst_tracker != nullptr) {
		// Add the optional destination tracker if it was provided.
		ResourceTracker *trackers[2] = { p_dst_tracker, p_src_tracker };
		ResourceUsage usages[2] = { RESOURCE_USAGE_COPY_TO, RESOURCE_USAGE_COPY_FROM };
		_add_command_to_graph(trackers, usages, 2, command_index, command);
	} else {
		ResourceUsage usage = RESOURCE_USAGE_COPY_FROM;
		_add_command_to_graph(&p_src_tracker, &usage, 1, command_index, command);
	}
}

void RenderingDeviceGraph::add_texture_resolve(RDD::TextureID p_src, ResourceTracker *p_src_tracker, RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, uint32_t p_src_layer, uint32_t p_src_mipmap, uint32_t p_dst_layer, uint32_t p_dst_mipmap) {
	DEV_ASSERT(p_src_tracker != nullptr);
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	RecordedTextureResolveCommand *command = static_cast<RecordedTextureResolveCommand *>(_allocate_command(sizeof(RecordedTextureResolveCommand), command_index));
	command->type = RecordedCommand::TYPE_TEXTURE_RESOLVE;
	command->self_stages = RDD::PIPELINE_STAGE_RESOLVE_BIT;
	command->from_texture = p_src;
	command->to_texture = p_dst;
	command->src_layer = p_src_layer;
	command->src_mipmap = p_src_mipmap;
	command->dst_layer = p_dst_layer;
	command->dst_mipmap = p_dst_mipmap;

	ResourceTracker *trackers[2] = { p_dst_tracker, p_src_tracker };
	ResourceUsage usages[2] = { RESOURCE_USAGE_RESOLVE_TO, RESOURCE_USAGE_RESOLVE_FROM };
	_add_command_to_graph(trackers, usages, 2, command_index, command);
}

void RenderingDeviceGraph::add_texture_update(RDD::TextureID p_dst, ResourceTracker *p_dst_tracker, VectorView<RecordedBufferToTextureCopy> p_buffer_copies, VectorView<ResourceTracker *> p_buffer_trackers) {
	DEV_ASSERT(p_dst_tracker != nullptr);

	int32_t command_index;
	uint64_t command_size = sizeof(RecordedTextureUpdateCommand) + p_buffer_copies.size() * sizeof(RecordedBufferToTextureCopy);
	RecordedTextureUpdateCommand *command = static_cast<RecordedTextureUpdateCommand *>(_allocate_command(command_size, command_index));
	command->type = RecordedCommand::TYPE_TEXTURE_UPDATE;
	command->self_stages = RDD::PIPELINE_STAGE_COPY_BIT;
	command->to_texture = p_dst;
	command->buffer_to_texture_copies_count = p_buffer_copies.size();

	RecordedBufferToTextureCopy *buffer_to_texture_copies = command->buffer_to_texture_copies();
	for (uint32_t i = 0; i < command->buffer_to_texture_copies_count; i++) {
		buffer_to_texture_copies[i] = p_buffer_copies[i];
	}

	if (p_buffer_trackers.size() > 0) {
		// Add the optional buffer trackers if they were provided.
		thread_local LocalVector<ResourceTracker *> trackers;
		thread_local LocalVector<ResourceUsage> usages;
		trackers.clear();
		usages.clear();
		for (uint32_t i = 0; i < p_buffer_trackers.size(); i++) {
			trackers.push_back(p_buffer_trackers[i]);
			usages.push_back(RESOURCE_USAGE_COPY_FROM);
		}

		trackers.push_back(p_dst_tracker);
		usages.push_back(RESOURCE_USAGE_COPY_TO);

		_add_command_to_graph(trackers.ptr(), usages.ptr(), trackers.size(), command_index, command);
	} else {
		ResourceUsage usage = RESOURCE_USAGE_COPY_TO;
		_add_command_to_graph(&p_dst_tracker, &usage, 1, command_index, command);
	}
}

void RenderingDeviceGraph::add_capture_timestamp(RDD::QueryPoolID p_query_pool, uint32_t p_index) {
	int32_t command_index;
	RecordedCaptureTimestampCommand *command = static_cast<RecordedCaptureTimestampCommand *>(_allocate_command(sizeof(RecordedCaptureTimestampCommand), command_index));
	command->type = RecordedCommand::TYPE_CAPTURE_TIMESTAMP;
	command->self_stages = 0;
	command->pool = p_query_pool;
	command->index = p_index;
	_add_command_to_graph(nullptr, nullptr, 0, command_index, command);
}

void RenderingDeviceGraph::add_synchronization() {
	// Synchronization is only acknowledged if commands have been recorded on the graph already.
	if (command_count > 0) {
		command_synchronization_pending = true;
	}
}

void RenderingDeviceGraph::begin_label(const Span<char> &p_label_name, const Color &p_color) {
	uint32_t command_label_offset = command_label_chars.size();
	int command_label_size = p_label_name.size();
	command_label_chars.resize(command_label_offset + command_label_size + 1);
	memcpy(&command_label_chars[command_label_offset], p_label_name.ptr(), command_label_size);
	command_label_chars[command_label_offset + command_label_size] = '\0';
	command_label_colors.push_back(p_color);
	command_label_offsets.push_back(command_label_offset);
	command_label_index = command_label_count;
	command_label_count++;
}

void RenderingDeviceGraph::end_label() {
	command_label_index = -1;
}

// GodotNext: async_compute (#174). Levels and priorities of every recorded command, in recording
// index order: a command's level is one more than the deepest command it depends on.
void RenderingDeviceGraph::_compute_command_levels(LocalVector<RecordedCommandSort> &r_commands_sorted) const {
	thread_local LocalVector<int64_t> command_stack;
	thread_local LocalVector<int32_t> sorted_command_indices;
	thread_local LocalVector<uint32_t> command_degrees;
	int32_t adjacency_list_index = 0;
	int32_t command_index;

	// Count all the incoming connections to every node by traversing their adjacency list.
	command_degrees.resize(command_count);
	memset(command_degrees.ptr(), 0, sizeof(uint32_t) * command_degrees.size());
	for (uint32_t i = 0; i < command_count; i++) {
		const RecordedCommand &recorded_command = *reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offsets[i]]);
		adjacency_list_index = recorded_command.adjacent_command_list_index;
		while (adjacency_list_index >= 0) {
			const RecordedCommandListNode &command_list_node = command_list_nodes[adjacency_list_index];
			DEV_ASSERT((command_list_node.command_index != int32_t(i)) && "Command can't have itself as a dependency.");
			command_degrees[command_list_node.command_index] += 1;
			adjacency_list_index = command_list_node.next_list_index;
		}
	}

	// Push to the stack all nodes that have no incoming connections.
	command_stack.clear();
	for (uint32_t i = 0; i < command_count; i++) {
		if (command_degrees[i] == 0) {
			command_stack.push_back(i);
		}
	}

	sorted_command_indices.clear();
	while (!command_stack.is_empty()) {
		// Pop command from the stack.
		command_index = command_stack[command_stack.size() - 1];
		command_stack.resize(command_stack.size() - 1);

		// Add it to the sorted commands.
		sorted_command_indices.push_back(command_index);

		// Search for its adjacents and lower their degree for every visit. If the degree reaches zero, we push the command to the stack.
		const uint32_t command_data_offset = command_data_offsets[command_index];
		const RecordedCommand &recorded_command = *reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offset]);
		adjacency_list_index = recorded_command.adjacent_command_list_index;
		while (adjacency_list_index >= 0) {
			const RecordedCommandListNode &command_list_node = command_list_nodes[adjacency_list_index];
			uint32_t &command_degree = command_degrees[command_list_node.command_index];
			DEV_ASSERT(command_degree > 0);
			command_degree--;
			if (command_degree == 0) {
				command_stack.push_back(command_list_node.command_index);
			}

			adjacency_list_index = command_list_node.next_list_index;
		}
	}

	// Batch buffer, texture, draw lists and compute operations together.
	const uint32_t PriorityTable[] = {
		0, // TYPE_NONE
		6, // TYPE_BOTTOM_LEVEL_ACCELERATION_STRUCTURE_BUILD
		6, // TYPE_TOP_LEVEL_ACCELERATION_STRUCTURE_BUILD
		1, // TYPE_BUFFER_CLEAR
		1, // TYPE_BUFFER_COPY
		1, // TYPE_BUFFER_GET_DATA
		1, // TYPE_BUFFER_UPDATE
		4, // TYPE_COMPUTE_LIST
		7, // TYPE_RAYTRACING_LIST
		3, // TYPE_DRAW_LIST
		2, // TYPE_TEXTURE_CLEAR_COLOR
		2, // TYPE_TEXTURE_CLEAR_DEPTH_STENCIL
		2, // TYPE_TEXTURE_COPY
		2, // TYPE_TEXTURE_GET_DATA
		2, // TYPE_TEXTURE_RESOLVE
		2, // TYPE_TEXTURE_UPDATE
		2, // TYPE_CAPTURE_TIMESTAMP
		5, // TYPE_DRIVER_CALLBACK
		// GodotNext: blas_compact
		6, // TYPE_ACCELERATION_STRUCTURE_QUERY_SIZE
		6, // TYPE_ACCELERATION_STRUCTURE_COPY
		// GodotNext: p6f_nv_rt (#90)
		6, // TYPE_CLAS_BUILD
		6, // TYPE_CLUSTER_BLAS_BUILD
		6, // TYPE_PTLAS_UPDATE
		6, // TYPE_MICROMAP_BUILD
	};
	static_assert(std_size(PriorityTable) == RecordedCommand::TYPE_MAX, "PriorityTable must have one entry per RecordedCommand::Type");

	r_commands_sorted.clear();
	r_commands_sorted.resize(command_count);

	for (uint32_t i = 0; i < command_count; i++) {
		const int32_t sorted_command_index = sorted_command_indices[i];
		const uint32_t command_data_offset = command_data_offsets[sorted_command_index];
		const RecordedCommand recorded_command = *reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offset]);
		const uint32_t next_command_level = r_commands_sorted[sorted_command_index].level + 1;
		adjacency_list_index = recorded_command.adjacent_command_list_index;
		while (adjacency_list_index >= 0) {
			const RecordedCommandListNode &command_list_node = command_list_nodes[adjacency_list_index];
			uint32_t &adjacent_command_level = r_commands_sorted[command_list_node.command_index].level;
			if (adjacent_command_level < next_command_level) {
				adjacent_command_level = next_command_level;
			}

			adjacency_list_index = command_list_node.next_list_index;
		}

		r_commands_sorted[sorted_command_index].index = sorted_command_index;
		r_commands_sorted[sorted_command_index].priority = PriorityTable[recorded_command.type];
		// GodotNext: async_compute (#174). The planner clears this again when it falls back.
		r_commands_sorted[sorted_command_index].queue = recorded_command.queue;
	}
}

void RenderingDeviceGraph::end(bool p_reorder_commands, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer, CommandBufferPool &r_command_buffer_pool) {
	if (command_count == 0) {
		// No commands have been logged, do nothing.
		return;
	}

	thread_local LocalVector<RecordedCommandSort> commands_sorted;
	// GodotNext: async_compute (#174). The level computation moved to _compute_command_levels(), and
	// _plan_async_frame() decides whether the async lists run on the async queue; plan_for_test()
	// and emit_for_test() share both. Without reordering, everything runs on the main queue.
	AsyncPlan async_plan;
	if (p_reorder_commands) {
		_compute_command_levels(commands_sorted);
		async_plan = _plan_async_frame(commands_sorted, bool(r_command_buffer_pool.async_command_buffer));
		// GodotNext: rd_readonly_depth (#301).
		_resolve_shared_read_transitions(commands_sorted, async_plan);
	} else {
		commands_sorted.clear();
		commands_sorted.resize(command_count);

		for (uint32_t i = 0; i < command_count; i++) {
			commands_sorted[i].index = i;
			commands_sorted[i].level = i; // GodotNext: rd_readonly_depth (#301). Match sequential emit order.
		}
		// GodotNext: rd_readonly_depth (#301).
		_resolve_shared_read_transitions(commands_sorted, async_plan);
	}
	// GodotNext: async_compute (#174).
	r_command_buffer_pool.async_fallback = async_command_count > 0 && !async_plan.active;

	_wait_for_secondary_command_buffer_tasks();

	if (command_count > 0) {
		int32_t current_label_index = -1;
		int32_t current_label_level = -1;
		_run_label_command_change(r_command_buffer, -1, -1, true, true, nullptr, 0, current_label_index, current_label_level);

		if (driver_workarounds.avoid_compute_after_draw) {
			// Reset the state of the workaround.
			workarounds_state.draw_list_found = false;
		}

#if PRINT_DRAW_LIST_STATS
		draw_list_total_size = 0;
#endif

		if (p_reorder_commands) {
#if PRINT_RENDER_GRAPH
			print_line("BEFORE SORT");
			_print_render_commands(commands_sorted.ptr(), command_count);
#endif

			commands_sorted.sort();

#if PRINT_RENDER_GRAPH
			print_line("AFTER SORT");
			_print_render_commands(commands_sorted.ptr(), command_count);
#endif

#if PRINT_COMMAND_RECORDING
			print_line(vformat("Recording %d commands", command_count));
#endif

			uint32_t boosted_priority = 0;
			// GodotNext: async_compute (#174). An active plan splits the frame between the main and the
			// async compute queue (_run_async_frame()); otherwise the levels run as before.
			if (async_plan.active) {
				_run_async_frame(commands_sorted.ptr(), async_plan, p_full_barriers, r_command_buffer, r_command_buffer_pool, current_label_index, current_label_level, boosted_priority);
			} else {
				uint32_t current_level = commands_sorted[0].level;
				uint32_t current_level_start = 0;
				for (uint32_t i = 0; i < command_count; i++) {
					if (current_level != commands_sorted[i].level) {
						_run_level(current_level, &commands_sorted[current_level_start], i - current_level_start, p_full_barriers, r_command_buffer, r_command_buffer_pool, current_label_index, current_label_level, boosted_priority);
						current_level = commands_sorted[i].level;
						current_level_start = i;
					}
				}

				_run_level(current_level, &commands_sorted[current_level_start], command_count - current_level_start, p_full_barriers, r_command_buffer, r_command_buffer_pool, current_label_index, current_label_level, boosted_priority);
			}

#if PRINT_RENDER_GRAPH
			// GodotNext: async_compute (#174). The sort puts the deepest level last.
			print_line("COMMANDS", command_count, "LEVELS", commands_sorted[command_count - 1].level + 1);
#endif
		} else {
			for (uint32_t i = 0; i < command_count; i++) {
				_group_barriers_for_render_commands(r_command_buffer, &commands_sorted[i], 1, p_full_barriers);
				_run_render_commands(i, &commands_sorted[i], 1, r_command_buffer, r_command_buffer_pool, current_label_index, current_label_level);
			}
		}

		_run_label_command_change(r_command_buffer, -1, -1, false, false, nullptr, 0, current_label_index, current_label_level);

#if PRINT_DRAW_LIST_STATS
		print_line(vformat("Draw list %d bytes", draw_list_total_size));
#endif
#if PRINT_COMMAND_RECORDING
		print_line(vformat("Recorded %d commands", command_count));
#endif
	}

	// Advance the frame counter. It's not necessary to do this if no commands are recorded because that means no secondary command buffers were used.
	frame = (frame + 1) % frames.size();
}

#if PRINT_RESOURCE_TRACKER_TOTAL
static uint32_t resource_tracker_total = 0;
#endif

// GodotNext: rd_bindless (#176)
RenderingDeviceGraph::BindlessResidency *RenderingDeviceGraph::_bindless_find_residency(ResourceTracker *p_tracker, BindlessTableTracker *p_table) {
	for (BindlessResidency &residency : p_tracker->bindless_residency) {
		if (residency.table == p_table) {
			return &residency;
		}
	}
	return nullptr;
}

void RenderingDeviceGraph::_bindless_table_reset_if_outdated(BindlessTableTracker *p_table) const {
	if (p_table->command_frame != tracking_frame) {
		p_table->command_frame = tracking_frame;
		p_table->read_commands.clear();
		p_table->ready_commands.clear();
	}
}

void RenderingDeviceGraph::_bindless_resource_used(ResourceTracker *p_resident, ResourceUsage p_usage, int32_t p_command_index, RecordedCommand *r_command) {
	// A texture is ready while it is sampled (or, for a depth/stencil texture, attached read-only
	// in the same shared layout); any other usage changes its layout. A buffer is ready until it
	// is written.
	// GodotNext: rd_readonly_depth (#301).
	const bool is_texture = p_resident->texture_driver_id.id != 0;
	const bool leaves_unready = is_texture ? (p_usage != RESOURCE_USAGE_TEXTURE_SAMPLE && !(_is_depth_stencil_texture(p_resident) && p_usage == RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ)) : _is_write_usage(p_usage);
	if (!leaves_unready) {
		return;
	}

	for (BindlessResidency &residency : p_resident->bindless_residency) {
		BindlessTableTracker *table = residency.table;
		_bindless_table_reset_if_outdated(table);

		// Write after read: the commands that bound the table this frame read the resource in its old
		// state. The table's reads in earlier frames add their stages to the barrier.
		for (int32_t read_command : table->read_commands) {
			if (read_command != p_command_index) {
				_add_adjacent_command(read_command, p_command_index, r_command);
			}
		}
		r_command->previous_stages = r_command->previous_stages | table->read_stages;

		if (!residency.dirty) {
			residency.dirty = true;
			table->dirty.push_back(p_resident);
		}
		residency.dirty_frame = tracking_frame;
		residency.dirty_command_index = p_command_index;
	}
}

void RenderingDeviceGraph::_bindless_tables_add_usages(InstructionList &r_list, ListUsageFunction p_add_usage) {
	for (BindlessTableTracker *table : r_list.bindless_tables) {
		for (ResourceTracker *tracker : table->dirty) {
			(this->*p_add_usage)(tracker, table->read_usage);
		}
	}
}

// GodotNext: rd_indirect_count (#177)
bool RenderingDeviceGraph::_bindless_dirty_in_list_tables(const InstructionList &p_list, const ResourceTracker *p_tracker) {
	for (const BindlessResidency &residency : p_tracker->bindless_residency) {
		if (residency.dirty && p_list.bindless_tables.has(residency.table)) {
			return true;
		}
	}
	return false;
}

// GodotNext: rd_indirect_count (#177), rd_readonly_depth (#301). _bindless_tables_add_usages() for
// the draw list. A dirty resident the list already reads as indirect arguments keeps that usage and
// stays dirty. A dirty depth/stencil texture already attached read-only shares
// TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL with sampled reads, so SHADER_READ access is merged
// into its read-only depth transition instead of adding a conflicting duplicate usage.
void RenderingDeviceGraph::_bindless_draw_list_tables_add_usages(int32_t p_command_index, RecordedCommand *r_command) {
	BitField<RDD::PipelineStageBits> graphics_shader_stages = RDD::PIPELINE_STAGE_VERTEX_SHADER_BIT;
	graphics_shader_stages.set_flag(RDD::PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT);
	graphics_shader_stages.set_flag(RDD::PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT);
	graphics_shader_stages.set_flag(RDD::PIPELINE_STAGE_GEOMETRY_SHADER_BIT);
	graphics_shader_stages.set_flag(RDD::PIPELINE_STAGE_TASK_SHADER_BIT);
	graphics_shader_stages.set_flag(RDD::PIPELINE_STAGE_MESH_SHADER_BIT);
	graphics_shader_stages.set_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	for (BindlessTableTracker *table : draw_instruction_list.bindless_tables) {
		for (ResourceTracker *tracker : table->dirty) {
			tracker->reset_if_outdated(tracking_frame);
			if (tracker->draw_list_index == draw_instruction_list.index && tracker->draw_list_usage == RESOURCE_USAGE_INDIRECT_BUFFER_READ) {
				ERR_PRINT("A buffer in a bindless table that this draw list binds is also the indirect or count buffer of one of its indirect draws, and the table has not read it since it was last written. A buffer has one usage per draw list, so the table's reads in this draw list are not synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table.");
				BindlessResidency *residency = _bindless_find_residency(tracker, table);
				DEV_ASSERT(residency != nullptr);
				residency->dirty_frame = tracking_frame;
				residency->dirty_command_index = p_command_index;
				continue;
			}
			if (tracker->draw_list_index == draw_instruction_list.index &&
					tracker->draw_list_usage == RESOURCE_USAGE_ATTACHMENT_DEPTH_STENCIL_READ &&
					table->read_usage == RESOURCE_USAGE_TEXTURE_SAMPLE &&
					_is_depth_stencil_texture(tracker)) {
				if (!draw_instruction_list.bindless_readonly_depth_trackers.has(tracker)) {
					draw_instruction_list.bindless_readonly_depth_trackers.push_back(tracker);
				}
				if (r_command->self_stages.get_shared(graphics_shader_stages).is_empty()) {
					r_command->self_stages.set_flag(RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				}
				continue;
			}
			add_draw_list_usage(tracker, table->read_usage);
		}
	}
}

// GodotNext: rd_rt_pipelines (#179). _bindless_draw_list_tables_add_usages() for the raytracing list: a dirty
// resident the list already reads as the launch size of an indirect trace recorded before the table was bound
// keeps that usage and stays dirty, so the next command that binds the table gets the barrier.
void RenderingDeviceGraph::_bindless_raytracing_list_tables_add_usages(int32_t p_command_index) {
	for (BindlessTableTracker *table : raytracing_instruction_list.bindless_tables) {
		for (ResourceTracker *tracker : table->dirty) {
			tracker->reset_if_outdated(tracking_frame);
			if (tracker->raytracing_list_index == raytracing_instruction_list.index && tracker->raytracing_list_usage == RESOURCE_USAGE_INDIRECT_BUFFER_READ) {
				ERR_PRINT("A buffer in a bindless table that this raytracing list binds is also the launch-size buffer of one of its indirect traces, and the table has not read it since it was last written. A buffer has one usage per raytracing list, so the table's reads in this raytracing list are not synchronized; bind the table in an earlier list after the write, or keep the buffer out of the table.");
				BindlessResidency *residency = _bindless_find_residency(tracker, table);
				DEV_ASSERT(residency != nullptr);
				residency->dirty_frame = tracking_frame;
				residency->dirty_command_index = p_command_index;
				continue;
			}
			add_raytracing_list_usage(tracker, table->read_usage);
		}
	}
}

void RenderingDeviceGraph::_bindless_tables_bound(InstructionList &r_list, int32_t p_command_index, RecordedCommand *r_command) {
	for (BindlessTableTracker *table : r_list.bindless_tables) {
		_bindless_table_reset_if_outdated(table);

		// Read after write: the binds that made resources ready this frame come first, as this command
		// reads those resources without adding them as usages.
		for (int32_t ready_command : table->ready_commands) {
			if (ready_command != p_command_index) {
				_add_adjacent_command(ready_command, p_command_index, r_command);
			}
		}

		// The dirty resources were usages of this command, so they are ready now, except for those this
		// command made dirty itself.
		bool made_ready = false;
		uint32_t still_dirty = 0;
		for (uint32_t i = 0; i < table->dirty.size(); i++) {
			ResourceTracker *tracker = table->dirty[i];
			BindlessResidency *residency = _bindless_find_residency(tracker, table);
			DEV_ASSERT(residency != nullptr);
			if (residency->dirty_frame == tracking_frame && residency->dirty_command_index == p_command_index) {
				table->dirty[still_dirty++] = tracker;
			} else {
				residency->dirty = false;
				made_ready = true;
			}
		}
		table->dirty.resize(still_dirty);

		table->read_commands.push_back(p_command_index);
		if (made_ready) {
			table->ready_commands.push_back(p_command_index);
		}
		table->read_stages = table->read_stages | r_command->self_stages;
	}
}

static void _add_bindless_table_to_list(LocalVector<RenderingDeviceGraph::BindlessTableTracker *> &r_tables, RenderingDeviceGraph::BindlessTableTracker *p_table) {
	DEV_ASSERT(p_table != nullptr);
	if (!r_tables.has(p_table)) {
		r_tables.push_back(p_table);
	}
}

void RenderingDeviceGraph::add_compute_list_bindless_table(BindlessTableTracker *p_table) {
	_add_bindless_table_to_list(compute_instruction_list.bindless_tables, p_table);
}

void RenderingDeviceGraph::add_draw_list_bindless_table(BindlessTableTracker *p_table) {
	_add_bindless_table_to_list(draw_instruction_list.bindless_tables, p_table);
}

void RenderingDeviceGraph::add_raytracing_list_bindless_table(BindlessTableTracker *p_table) {
	_add_bindless_table_to_list(raytracing_instruction_list.bindless_tables, p_table);
}

RenderingDeviceGraph::BindlessTableTracker *RenderingDeviceGraph::bindless_table_tracker_create(ResourceUsage p_read_usage) {
	BindlessTableTracker *table = memnew(BindlessTableTracker);
	table->read_usage = p_read_usage;
	return table;
}

void RenderingDeviceGraph::bindless_table_tracker_free(BindlessTableTracker *p_table) {
	if (p_table == nullptr) {
		return;
	}

	// RenderingDevice releases every slot first.
	DEV_ASSERT(p_table->dirty.is_empty());
	memdelete(p_table);
}

void RenderingDeviceGraph::bindless_table_add_resident(BindlessTableTracker *p_table, ResourceTracker *p_tracker) {
	DEV_ASSERT(p_table != nullptr && p_tracker != nullptr);
	DEV_ASSERT(p_tracker->parent == nullptr);
	BindlessResidency *residency = _bindless_find_residency(p_tracker, p_table);
	if (residency == nullptr) {
		BindlessResidency new_residency;
		new_residency.table = p_table;
		new_residency.dirty = true;
		p_tracker->bindless_residency.push_back(new_residency);
		residency = &p_tracker->bindless_residency[p_tracker->bindless_residency.size() - 1];
		p_table->dirty.push_back(p_tracker);
	}
	residency->slots++;
}

void RenderingDeviceGraph::bindless_table_remove_resident(BindlessTableTracker *p_table, ResourceTracker *p_tracker) {
	DEV_ASSERT(p_table != nullptr && p_tracker != nullptr);
	for (uint32_t i = 0; i < p_tracker->bindless_residency.size(); i++) {
		BindlessResidency &residency = p_tracker->bindless_residency[i];
		if (residency.table != p_table) {
			continue;
		}

		DEV_ASSERT(residency.slots > 0);
		residency.slots--;
		if (residency.slots == 0) {
			if (residency.dirty) {
				p_table->dirty.erase_unordered(p_tracker);
			}
			p_tracker->bindless_residency.remove_at_unordered(i);
		}
		return;
	}
	DEV_ASSERT(false && "The resource is not resident in the bindless table.");
}

void RenderingDeviceGraph::command_barriers_for_test(int32_t p_command_index, LocalVector<RDD::TextureBarrier> &r_texture_barriers, LocalVector<RDD::BufferBarrier> &r_buffer_barriers, RDD::MemoryAccessBarrier &r_memory_barrier) const {
	ERR_FAIL_INDEX(p_command_index, int32_t(command_count));
	const RecordedCommand *command = reinterpret_cast<const RecordedCommand *>(&command_data[command_data_offsets[p_command_index]]);
	r_texture_barriers.clear();
	for (int32_t j = 0; j < command->normalization_barrier_count; j++) {
		r_texture_barriers.push_back(command_normalization_barriers[command->normalization_barrier_index + j]);
	}
	for (int32_t j = 0; j < command->transition_barrier_count; j++) {
		r_texture_barriers.push_back(command_transition_barriers[command->transition_barrier_index + j]);
	}
	r_buffer_barriers.clear();
#if USE_BUFFER_BARRIERS
	for (int32_t j = 0; j < command->buffer_barrier_count; j++) {
		r_buffer_barriers.push_back(command_buffer_barriers[command->buffer_barrier_index + j]);
	}
#endif
	r_memory_barrier = command->memory_barrier;
}

RenderingDeviceGraph::ResourceTracker *RenderingDeviceGraph::resource_tracker_create() {
#if PRINT_RESOURCE_TRACKER_TOTAL
	print_line("Resource trackers:", ++resource_tracker_total);
#endif
	return memnew(ResourceTracker);
}

void RenderingDeviceGraph::resource_tracker_free(ResourceTracker *p_tracker) {
	if (p_tracker == nullptr) {
		return;
	}

	// GodotNext: rd_bindless (#176). RenderingDevice defers the free of a resident resource.
	DEV_ASSERT(p_tracker->bindless_residency.is_empty());

	if (p_tracker->in_parent_dirty_list) {
		// Delete the tracker from the parent's dirty linked list.
		if (p_tracker->parent->dirty_shared_list == p_tracker) {
			p_tracker->parent->dirty_shared_list = p_tracker->next_shared;
		} else {
			ResourceTracker *node = p_tracker->parent->dirty_shared_list;
			while (node != nullptr) {
				if (node->next_shared == p_tracker) {
					node->next_shared = p_tracker->next_shared;
					node = nullptr;
				} else {
					node = node->next_shared;
				}
			}
		}
	}

	memdelete(p_tracker);

#if PRINT_RESOURCE_TRACKER_TOTAL
	print_line("Resource trackers:", --resource_tracker_total);
#endif
}

RenderingDeviceGraph::FramebufferCache *RenderingDeviceGraph::framebuffer_cache_create() {
	return memnew(FramebufferCache);
}

void RenderingDeviceGraph::framebuffer_cache_free(RDD *p_driver, FramebufferCache *p_cache) {
	DEV_ASSERT(p_driver != nullptr);

	if (p_cache == nullptr) {
		return;
	}

	// GodotNext: rd_readonly_depth (#301).
	for (KeyValue<FramebufferStorageKey, FramebufferStorage> &E : p_cache->storage_map) {
		p_driver->framebuffer_free(E.value.framebuffer);
		p_driver->render_pass_free(E.value.render_pass);
	}

	memdelete(p_cache);
}
