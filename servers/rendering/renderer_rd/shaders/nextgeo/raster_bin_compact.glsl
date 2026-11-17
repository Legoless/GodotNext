#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, issue step 5). The per-bin compaction of cluster
// cull's programmable-bin entries, dispatched only when a programmable bin exists:
//   PREFIX:  one thread. The prefix sum over the phase's (bin, kind) counts gives each range its start
//            in the phase's region, in bin order with the software range first; it writes every bin's
//            software dispatch and hardware draw arguments and the scatter dispatch.
//   (none):  SCATTER, dispatched indirectly, one thread per appended entry: moves the entry's
//            visible-cluster index into its (bin, kind) range.
// nextgeo_raster_bin_prefix() and nextgeo_raster_bin_scatter() in nextgeo_raster_bins.h are the CPU
// reference.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "raster_features_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std430) buffer restrict RasterBinState {
	uint data[];
}
state;

layout(set = 0, binding = 1, std430) buffer restrict readonly RasterBinAppend {
	uint data[];
}
append_list;

layout(set = 0, binding = 2, std430) buffer restrict writeonly RasterBinLists {
	uint data[];
}
bin_lists;

layout(push_constant, std430) uniform Params {
	uint phase;
	uint list_base; // First entry of the phase's region in the append and bin lists.
	uint region_size;
	uint vertex_count; // Hardware draw vertices per cluster (NEXTGEO_RASTER_VERTEX_COUNT).
}
params;

#ifdef PREFIX

layout(set = 0, binding = 3, std430) buffer restrict writeonly RasterBinArgs {
	uint data[];
}
bin_args;

void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	uint total = 0u;
	for (uint bin = 1u; bin <= NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX; bin++) {
		for (uint kind = 0u; kind < 2u; kind++) {
			uint count = state.data[nextgeo_raster_bin_count_word(params.phase, bin, kind)];
			// The region never holds more entries than cull appended; clamp a count past it.
			count = min(count, params.region_size - min(total, params.region_size));
			state.data[nextgeo_raster_bin_count_word(params.phase, bin, kind)] = count;
			state.data[nextgeo_raster_bin_cursor_word(params.phase, bin, kind)] = total;
			state.data[nextgeo_raster_bin_fill_word(params.phase, bin, kind)] = 0u;
			uint args = nextgeo_raster_bin_args_word(params.phase, bin, kind);
			if (kind == NEXTGEO_RASTER_BIN_KIND_SW) {
				// Split as #162's software dispatch: x * y workgroups cover count clusters.
				bin_args.data[args + 0u] = min(count, NEXTGEO_SW_DISPATCH_WIDTH);
				bin_args.data[args + 1u] = count == 0u ? 0u : (count - 1u) / NEXTGEO_SW_DISPATCH_WIDTH + 1u;
				bin_args.data[args + 2u] = 1u;
				bin_args.data[args + 3u] = 0u;
			} else {
				// VkDrawIndirectCommand: vertices, instances (clusters), first vertex, first instance.
				bin_args.data[args + 0u] = params.vertex_count;
				bin_args.data[args + 1u] = count;
				bin_args.data[args + 2u] = 0u;
				bin_args.data[args + 3u] = 0u;
			}
			total += count;
		}
	}
	uint appended = min(state.data[NEXTGEO_RASTER_BIN_STATE_APPENDED + params.phase], params.region_size);
	uint scatter = nextgeo_raster_bin_scatter_args_word(params.phase);
	bin_args.data[scatter + 0u] = (appended + 63u) / 64u;
	bin_args.data[scatter + 1u] = 1u;
	bin_args.data[scatter + 2u] = 1u;
	bin_args.data[scatter + 3u] = 0u;
}

#else

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint appended = min(state.data[NEXTGEO_RASTER_BIN_STATE_APPENDED + params.phase], params.region_size);
	if (index >= appended) {
		return;
	}
	uint entry = append_list.data[params.list_base + index];
	uint bin = nextgeo_raster_bin_entry_bin(entry);
	uint kind = (entry >> NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT) & 1u;
	uint count = state.data[nextgeo_raster_bin_count_word(params.phase, bin, kind)];
	uint fill = atomicAdd(state.data[nextgeo_raster_bin_fill_word(params.phase, bin, kind)], 1u);
	if (fill >= count) {
		return;
	}
	uint slot = state.data[nextgeo_raster_bin_cursor_word(params.phase, bin, kind)] + fill;
	if (slot < params.region_size) {
		bin_lists.data[params.list_base + slot] = entry & ((1u << NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS) - 1u);
	}
}

#endif
