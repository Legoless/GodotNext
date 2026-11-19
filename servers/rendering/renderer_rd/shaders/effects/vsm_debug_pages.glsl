#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_settings_debug (#221, plan §21.3.2). Per-physical-page data for the VSM_CACHE and
// VSM_OVERDRAW debug draws (vsm_debug.glsl), from the view's dirty-page list and its non-Nanite
// instance-page pairs. Runs only while one of those draws is on; it writes nothing the renderer reads.
// page_info[p]: bit 0 the static layer was rendered this view, bit 1 the dynamic layer, bits 8 and up
// the pairs (caster draws) rasterized into the page. page_info[pool_pages] is the #277 sink.
// MODE_CLEAR: one thread per page. MODE_DIRTY: one thread per dirty record. MODE_PAIRS: a grid-stride loop
// over the pairs (the list holds up to 1 << 22 of them, more than 65,535 groups of 64 threads).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer AllocState {
	uint data[];
}
alloc_state;

layout(set = 0, binding = 1, std430) restrict readonly buffer DirtyList {
	uvec4 data[];
}
dirty;

layout(set = 0, binding = 2, std430) restrict readonly buffer Pairs {
	uvec4 data[];
}
pairs;

layout(set = 0, binding = 3, std430) restrict readonly buffer CullState {
	uint data[];
}
cull_state;

layout(set = 0, binding = 4, std430) restrict buffer PageInfo {
	uint data[];
}
page_info;

layout(push_constant, std430) uniform Params {
	uint pool_pages;
	uint grid_width;
	uint pair_capacity; // 0 without non-Nanite pairs this view.
	uint pair_stride; // MODE_PAIRS: the dispatched threads.
}
params;

uint page_index(uint phys_entry) {
	uint x = phys_entry & VSM_ENTRY_PAGE_MASK;
	uint y = (phys_entry >> VSM_ENTRY_PAGE_BITS) & VSM_ENTRY_PAGE_MASK;
	return y * params.grid_width + x;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
#ifdef MODE_CLEAR
	if (index <= params.pool_pages) {
		page_info.data[index] = 0u;
	}
#endif
#ifdef MODE_DIRTY
	uint count = min(alloc_state.data[VSM_STATE_DIRTY], params.pool_pages);
	if (index < count) {
		uvec4 record = dirty.data[index];
		uint page = page_index(record.z);
		if (page < params.pool_pages) {
			uint bits = ((record.w & VSM_ENTRY_DIRTY_STATIC) != 0u ? 1u : 0u) | ((record.w & VSM_ENTRY_DIRTY_DYNAMIC) != 0u ? 2u : 0u);
			// #277: every atomic's result is used.
			if (atomicOr(page_info.data[page], bits) == 0xFFFFFFFFu) {
				page_info.data[params.pool_pages] = 1u;
			}
		}
	}
#endif
#ifdef MODE_PAIRS
	uint count = min(cull_state.data[0], params.pair_capacity);
	// A pair's record indexes this view's dirty list.
	uint dirty_count = min(alloc_state.data[VSM_STATE_DIRTY], params.pool_pages);
	uint stride = max(params.pair_stride, 1u);
	for (uint i = index; i < count; i += stride) {
		uint record_index = pairs.data[i].z & 0x7FFFFFFFu;
		if (record_index < dirty_count) {
			uint page = page_index(dirty.data[record_index].z);
			if (page < params.pool_pages) {
				// #277: every atomic's result is used.
				if (atomicAdd(page_info.data[page], 256u) == 0xFFFFFFFFu) {
					page_info.data[params.pool_pages] = 1u;
				}
			}
		}
	}
#endif
}
