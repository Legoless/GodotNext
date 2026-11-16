#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3). Physical page allocation after the
// marking pass (vsm_mark_pages.glsl), one dispatch per mode, in this order:
// MODE_UPDATE: one thread per physical page. A page whose entry was cleared (a freed light, a new
//   pool) is free again (orphaned); a page whose entry is requested is kept and stamped with the
//   frame; a page no view requested for cache_frames frames, or whose clipmap slot scrolled to new
//   ground without a request, is dropped; the rest stay cached. Free and cached pages go into
//   buckets: 0 for free pages, then cache_frames - age, so the oldest cached pages come first.
// MODE_SCAN: one thread: the buckets' offsets and the free list's length.
// MODE_SCATTER: one thread per page: the free list, least recently used first.
// MODE_ASSIGN: one thread per request, run twice: coarse requests first (phase 0), then the rest.
//   A requested entry with a page keeps it (a scrolled or uncached one is re-rendered there, not
//   reallocated); a new request takes the next free-list page, clearing the entry of a cached page
//   it takes; past the end of the list the entry gets VSM_ENTRY_OVERFLOW.
// MODE_MAPS: one workgroup over the maps. The maps that requested the most pages (at least half the
//   largest map's count) take one more mip of overflow bias in a view with overflow (at most
//   VSM_OVERFLOW_MAX_BIAS, and once per frame); without overflow a map gives one mip back per
//   VSM_OVERFLOW_DECAY_FRAMES frames, counted by each frame's first marked view. Then each map's place
//   in the dirty list (directional maps first) and the indirect arguments.
// MODE_EMIT: one thread per request: the dirty list (map, mip and page, physical page, flags) and
//   the entry's request and dirty flags cleared.
// MODE_CLEAR: one workgroup per dirty page (dispatched indirectly): the layers about to be rendered
//   are cleared to the far plane (0 in reversed Z), so raster only has to add depth.
// The CPU model is RendererVirtualShadowMapStorage::allocate_model() and overflow_bias_update().

#if defined(MODE_MAPS)
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

layout(set = 0, binding = 0, std430) restrict buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;

#include "vsm_mark_inc.glsl"
#include "vsm_page_table_inc.glsl"

layout(set = 0, binding = 1, std430) restrict readonly buffer VSMMaps {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 2, std430) restrict buffer VSMMapState {
	uint data[];
}
map_state;

layout(set = 0, binding = 3, std430) restrict buffer VSMState {
	uint data[];
}
state;

layout(set = 0, binding = 4, std430) restrict readonly buffer VSMRequests {
	uint data[];
}
requests;

// The pool header: per physical page the owning entry plus one (0: free), the frame it was last
// requested in, and its bucket in this view's free list.
layout(set = 0, binding = 5, std430) restrict buffer VSMPages {
	uvec4 data[];
}
pages;

layout(set = 0, binding = 6, std430) restrict buffer VSMFreeList {
	uint data[];
}
free_list;

// Per dirty page: map, mip | page x << 8 | page y << 16, the physical page (x | y << 12), the
// entry's flags. Directional maps' pages first, each map's pages together.
layout(set = 0, binding = 7, std430) restrict buffer VSMDirty {
	uvec4 data[];
}
dirty;

#ifdef MODE_MAPS
// Only MODE_MAPS binds the indirect arguments: MODE_CLEAR reads them as its dispatch arguments, and a
// storage binding in the same pass would make the render graph track the wrong usage.
layout(set = 0, binding = 8, std430) restrict writeonly buffer VSMIndirect {
	uint data[];
}
indirect;
#endif

#ifdef MODE_CLEAR
layout(r32ui, set = 0, binding = 9) uniform restrict writeonly uimage2DArray pool;
#endif

layout(push_constant, std430) uniform Params {
	uint frame;
	uint cache_frames;
	uint table_maps;
	uint pool_pages;
	uint grid_width;
	uint request_capacity;
	uint phase;
	uint pad;
}
params;

// Used results of atomics: with D3D12 GPU-based validation, an atomic whose result is unused does not
// land (#277).
void vsm_sink(uint value) {
	if (value == 0xFFFFFFFFu) {
		state.data[VSM_STATE_SINK] = value;
	}
}

void vsm_count(uint counter) {
	vsm_sink(atomicAdd(state.data[counter], 1u));
}

uint vsm_map_state(uint region, uint map) {
	return region * params.table_maps + map;
}

uint vsm_request_count() {
	return min(state.data[VSM_STATE_REQUESTS], params.request_capacity);
}

#ifdef MODE_UPDATE

void main() {
	uint p = gl_GlobalInvocationID.x;
	if (p >= params.pool_pages) {
		return;
	}
	uvec4 page = pages.data[p];
	uint owner = page.x;
	uint bucket = VSM_BUCKET_NONE;
	if (owner != 0u) {
		uint index = owner - 1u;
		uint entry = index < params.table_maps * VSM_ENTRIES_PER_MAP ? vsm_page_table.entries[index] : 0u;
		uvec2 xy = uvec2(p % params.grid_width, p / params.grid_width);
		if ((entry & VSM_ENTRY_ALLOCATED) == 0u || any(notEqual(vsm_entry_page(entry), xy))) {
			owner = 0u;
			vsm_count(VSM_STATE_ORPHANED);
		} else if ((entry & VSM_ENTRY_REQUESTED) != 0u) {
			page.y = params.frame;
		} else {
			uint age = params.frame - page.y;
			if (age >= params.cache_frames || (entry & VSM_ENTRY_SCROLLED) != 0u) {
				vsm_page_table.entries[index] = 0u;
				owner = 0u;
				vsm_count(VSM_STATE_EVICTED);
			} else {
				vsm_count(VSM_STATE_CACHED);
				if (age != 0u) {
					bucket = params.cache_frames - age;
				}
			}
		}
	}
	if (owner == 0u) {
		bucket = 0u;
	}
	page.x = owner;
	page.z = bucket;
	pages.data[p] = page;
	if (bucket != VSM_BUCKET_NONE) {
		vsm_count(VSM_STATE_BUCKETS + bucket);
	}
}

#endif

#ifdef MODE_SCAN

void main() {
	if (gl_GlobalInvocationID.x != 0u) {
		return;
	}
	uint running = 0u;
	for (uint b = 0u; b < VSM_BUCKET_COUNT; b++) {
		state.data[VSM_STATE_BUCKET_OFFSETS + b] = running;
		running += state.data[VSM_STATE_BUCKETS + b];
	}
	state.data[VSM_STATE_AVAILABLE] = running;
}

#endif

#ifdef MODE_SCATTER

void main() {
	uint p = gl_GlobalInvocationID.x;
	if (p >= params.pool_pages) {
		return;
	}
	uint bucket = pages.data[p].z;
	if (bucket == VSM_BUCKET_NONE) {
		return;
	}
	uint slot = state.data[VSM_STATE_BUCKET_OFFSETS + bucket] + atomicAdd(state.data[VSM_STATE_BUCKET_CURSORS + bucket], 1u);
	free_list.data[slot] = p;
}

#endif

#ifdef MODE_ASSIGN

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= vsm_request_count()) {
		return;
	}
	uint index = requests.data[i];
	uint entry = vsm_page_table.entries[index];
	bool coarse = (entry & VSM_ENTRY_COARSE) != 0u;
	if (coarse != (params.phase == 0u)) {
		return;
	}
	uint map = index / VSM_ENTRIES_PER_MAP;
	if (coarse) {
		vsm_count(VSM_STATE_COARSE);
	}
	if ((entry & VSM_ENTRY_ALLOCATED) != 0u) {
		vsm_count(VSM_STATE_KEPT);
		if ((entry & (VSM_ENTRY_SCROLLED | VSM_ENTRY_UNCACHED)) != 0u) {
			entry |= VSM_ENTRY_DIRTY_MASK;
			vsm_page_table.entries[index] = entry;
		}
		if ((entry & VSM_ENTRY_DIRTY_MASK) != 0u) {
			vsm_sink(atomicAdd(map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_COUNT, map)], 1u));
		}
		return;
	}
	uint slot = atomicAdd(state.data[VSM_STATE_CURSOR], 1u);
	if (slot >= state.data[VSM_STATE_AVAILABLE]) {
		vsm_page_table.entries[index] = entry | VSM_ENTRY_OVERFLOW;
		vsm_count(VSM_STATE_OVERFLOW);
		return;
	}
	uint p = free_list.data[slot];
	uint previous = pages.data[p].x;
	if (previous != 0u) {
		vsm_page_table.entries[previous - 1u] = 0u;
		vsm_count(VSM_STATE_STOLEN);
	}
	uvec2 xy = uvec2(p % params.grid_width, p / params.grid_width);
	vsm_page_table.entries[index] = xy.x | (xy.y << VSM_ENTRY_PAGE_BITS) | VSM_ENTRY_ALLOCATED | VSM_ENTRY_REQUESTED | VSM_ENTRY_DIRTY_MASK | (entry & (VSM_ENTRY_COARSE | VSM_ENTRY_UNCACHED));
	pages.data[p] = uvec4(index + 1u, params.frame, VSM_BUCKET_NONE, 0u);
	vsm_count(VSM_STATE_NEW);
	vsm_sink(atomicAdd(map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_COUNT, map)], 1u));
}

#endif

#ifdef MODE_MAPS

shared uint shared_values[256];
shared bool shared_new_frame;

// RendererVirtualShadowMapStorage::overflow_bias_update().
uint vsm_bias_update(uint bias_state, uint map_requests, uint max_requests, uint overflow, bool new_frame) {
	uint bias = bias_state & 0xFFu;
	uint decay = (bias_state >> 8u) & 0xFFu;
	bool raised = !new_frame && (bias_state & VSM_OVERFLOW_BIAS_RAISED) != 0u;
	if (overflow > 0u) {
		decay = 0u;
		if (!raised && map_requests > 0u && map_requests * 2u >= max_requests) {
			bias = min(bias + 1u, VSM_OVERFLOW_MAX_BIAS);
			raised = true;
		}
	} else if (new_frame) {
		if (bias > 0u) {
			decay++;
			if (decay >= VSM_OVERFLOW_DECAY_FRAMES) {
				bias--;
				decay = 0u;
			}
		} else {
			decay = 0u;
		}
	}
	return bias | (decay << 8u) | (raised ? VSM_OVERFLOW_BIAS_RAISED : 0u);
}

// Inclusive prefix sum of shared_values over the workgroup; returns the thread's exclusive sum.
uint vsm_scan(uint value) {
	uint id = gl_LocalInvocationID.x;
	shared_values[id] = value;
	barrier();
	for (uint offset = 1u; offset < 256u; offset <<= 1u) {
		uint add = id >= offset ? shared_values[id - offset] : 0u;
		barrier();
		shared_values[id] += add;
		barrier();
	}
	return shared_values[id] - value;
}

void main() {
	uint id = gl_LocalInvocationID.x;
	uint maps = params.table_maps;
	uint per_thread = (maps + 255u) / 256u;
	uint begin = min(id * per_thread, maps);
	uint end = min(begin + per_thread, maps);
	// The first marked view of a frame: thread 0 reads the last bias frame here and writes it at the end.
	if (id == 0u) {
		shared_new_frame = state.data[VSM_STATE_BIAS_FRAME] != params.frame + 1u;
	}

	// The largest map's request count.
	uint local_max = 0u;
	for (uint m = begin; m < end; m++) {
		local_max = max(local_max, map_state.data[vsm_map_state(VSM_MAP_STATE_REQUESTS, m)]);
	}
	shared_values[id] = local_max;
	barrier();
	for (uint offset = 128u; offset > 0u; offset >>= 1u) {
		if (id < offset) {
			shared_values[id] = max(shared_values[id], shared_values[id + offset]);
		}
		barrier();
	}
	uint max_requests = shared_values[0];
	barrier();

	// Overflow bias.
	uint overflow = state.data[VSM_STATE_OVERFLOW];
	uint biased = 0u;
	for (uint m = begin; m < end; m++) {
		uint slot = vsm_map_state(VSM_MAP_STATE_BIAS, m);
		uint bias_state = vsm_bias_update(map_state.data[slot], map_state.data[vsm_map_state(VSM_MAP_STATE_REQUESTS, m)], max_requests, overflow, shared_new_frame);
		map_state.data[slot] = bias_state;
		biased += (bias_state & 0xFFu) != 0u ? 1u : 0u;
	}
	vsm_scan(biased);
	uint biased_total = shared_values[255];
	barrier();

	// Dirty list places: directional maps first, then the others.
	uint totals[2];
	uint before = 0u;
	for (uint pass = 0u; pass < 2u; pass++) {
		uint local_sum = 0u;
		for (uint m = begin; m < end; m++) {
			bool directional = vsm_maps.data[m].kind == VSM_MAP_KIND_DIRECTIONAL;
			if (directional == (pass == 0u)) {
				local_sum += map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_COUNT, m)];
			}
		}
		uint offset = before + vsm_scan(local_sum);
		totals[pass] = shared_values[255];
		barrier();
		for (uint m = begin; m < end; m++) {
			bool directional = vsm_maps.data[m].kind == VSM_MAP_KIND_DIRECTIONAL;
			if (directional == (pass == 0u)) {
				map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_OFFSET, m)] = offset;
				offset += map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_COUNT, m)];
			}
		}
		before += totals[pass];
	}

	if (id == 0u) {
		uint total = totals[0] + totals[1];
		state.data[VSM_STATE_DIRTY] = total;
		state.data[VSM_STATE_DIRTY_DIRECTIONAL] = totals[0];
		state.data[VSM_STATE_MAX_MAP_REQUESTS] = max_requests;
		state.data[VSM_STATE_BIASED_MAPS] = biased_total;
		state.data[VSM_STATE_TOTAL_OVERFLOW] += overflow;
		state.data[VSM_STATE_OVERFLOW_VIEWS] += overflow > 0u ? 1u : 0u;
		state.data[VSM_STATE_TOTAL_DROPPED] += state.data[VSM_STATE_REQUESTS_DROPPED];
		state.data[VSM_STATE_VIEWS] += 1u;
		state.data[VSM_STATE_BIAS_FRAME] = params.frame + 1u;
		indirect.data[VSM_INDIRECT_DIRECTIONAL] = totals[0];
		indirect.data[VSM_INDIRECT_DIRECTIONAL + 1u] = 1u;
		indirect.data[VSM_INDIRECT_DIRECTIONAL + 2u] = 1u;
		indirect.data[VSM_INDIRECT_LOCAL] = totals[1];
		indirect.data[VSM_INDIRECT_LOCAL + 1u] = 1u;
		indirect.data[VSM_INDIRECT_LOCAL + 2u] = 1u;
		indirect.data[VSM_INDIRECT_ALL] = total;
		indirect.data[VSM_INDIRECT_ALL + 1u] = 1u;
		indirect.data[VSM_INDIRECT_ALL + 2u] = 1u;
	}
}

#endif

#ifdef MODE_EMIT

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= vsm_request_count()) {
		return;
	}
	uint index = requests.data[i];
	uint entry = vsm_page_table.entries[index];
	if ((entry & VSM_ENTRY_ALLOCATED) != 0u && (entry & VSM_ENTRY_DIRTY_MASK) != 0u) {
		uint map = index / VSM_ENTRIES_PER_MAP;
		uint position = map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_OFFSET, map)] + atomicAdd(map_state.data[vsm_map_state(VSM_MAP_STATE_DIRTY_CURSOR, map)], 1u);
		uint local = index - map * VSM_ENTRIES_PER_MAP;
		uint mip = 0u;
		while (mip + 1u < VSM_MIP_COUNT && local >= vsm_mip_entry_offset(mip + 1u)) {
			mip++;
		}
		uint pages_per_side = VSM_PAGES_PER_SIDE >> mip;
		uint in_mip = local - vsm_mip_entry_offset(mip);
		uint page_x = in_mip % pages_per_side;
		uint page_y = in_mip / pages_per_side;
		if (position < params.pool_pages) {
			dirty.data[position] = uvec4(map, mip | (page_x << 8u) | (page_y << 16u), entry & ~VSM_ENTRY_FLAGS_MASK, entry & VSM_ENTRY_FLAGS_MASK);
		}
	}
	uint cleared = VSM_ENTRY_REQUESTED | VSM_ENTRY_COARSE | VSM_ENTRY_DIRTY_STATIC | VSM_ENTRY_DIRTY_DYNAMIC | VSM_ENTRY_SCROLLED;
	if ((entry & VSM_ENTRY_ALLOCATED) != 0u) {
		cleared |= VSM_ENTRY_OVERFLOW;
	}
	vsm_page_table.entries[index] = entry & ~cleared;
}

#endif

#ifdef MODE_CLEAR

void main() {
	uint record = gl_WorkGroupID.x;
	if (record >= state.data[VSM_STATE_DIRTY]) {
		return;
	}
	uvec4 page = dirty.data[record];
	ivec2 base = ivec2(vsm_entry_page(page.z) * VSM_PAGE_SIZE);
	bool static_layer = (page.w & VSM_ENTRY_DIRTY_STATIC) != 0u;
	bool dynamic_layer = (page.w & VSM_ENTRY_DIRTY_DYNAMIC) != 0u;
	for (uint y = 0u; y < VSM_PAGE_SIZE; y++) {
		for (uint x = gl_LocalInvocationID.x; x < VSM_PAGE_SIZE; x += 64u) {
			ivec2 texel = base + ivec2(x, y);
			if (static_layer) {
				imageStore(pool, ivec3(texel, 0), uvec4(0u));
			}
			if (dynamic_layer) {
				imageStore(pool, ivec3(texel, 1), uvec4(0u));
			}
		}
	}
}

#endif
