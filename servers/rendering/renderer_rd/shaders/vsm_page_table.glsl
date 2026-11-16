#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_page_pool (#220). Page-table passes of the Virtual Shadow Map core.
// MODE_SCROLL: a directional clipmap level moved; set VSM_ENTRY_SCROLLED on every allocated entry
// of the map in a column or row that wrapped onto new ground (the CPU model is
// RendererVirtualShadowMapStorage::clipmap_apply_scroll()).
// MODE_LOOKUP_CHECK: runs vsm_lookup(), vsm_light_map() and vsm_lookup_clipmap() for a list of
// queries, for the self-check that compares them with the CPU models.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;

#include "vsm_page_table_inc.glsl"

#ifdef MODE_SCROLL

layout(push_constant, std430) uniform Params {
	uint map;
	// Per mip: column start in bits 0-7, column count in 8-15, row start in 16-23, row count in 24-31.
	uint mip_ranges[VSM_MIP_COUNT];
}
params;

bool in_wrapped_range(uint slot, uint start, uint count, uint mask) {
	return ((slot - start) & mask) < count;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= VSM_ENTRIES_PER_MAP) {
		return;
	}
	uint mip = 0u;
	while (mip + 1u < VSM_MIP_COUNT && index >= vsm_mip_entry_offset(mip + 1u)) {
		mip++;
	}
	uint pages = VSM_PAGES_PER_SIDE >> mip;
	uint local = index - vsm_mip_entry_offset(mip);
	uint x = local % pages;
	uint y = local / pages;
	uint ranges = params.mip_ranges[mip];
	uint mask = pages - 1u;
	bool hit = in_wrapped_range(x, ranges & 0xFFu, (ranges >> 8u) & 0xFFu, mask) || in_wrapped_range(y, (ranges >> 16u) & 0xFFu, (ranges >> 24u) & 0xFFu, mask);
	if (!hit) {
		return;
	}
	uint slot = params.map * VSM_ENTRIES_PER_MAP + index;
	uint entry = vsm_page_table.entries[slot];
	if ((entry & VSM_ENTRY_ALLOCATED) != 0u) {
		vsm_page_table.entries[slot] = entry | VSM_ENTRY_SCROLLED;
	}
}

#endif

#ifdef MODE_LOOKUP_CHECK

// Three uvec4 per query.
// 0: the map, or the light's vsm_map_base (kinds 1 and 2); the uv's two float bit patterns (kinds 0
//    and 1) or the absolute mip 0 texel (kind 2); the mip in bits 0-7 and the kind in bits 8-15.
// 1: the light's vsm_flags, the map index (kinds 1 and 2), and the level origin (kind 2).
// 2: the page table's map count the lookup is given, then padding.
// Kind 0 calls vsm_lookup(map), kind 1 vsm_lookup(vsm_light_map(base, flags, index)) and kind 2
// vsm_lookup_clipmap(vsm_light_map(base, flags, index), origin, texel).
layout(set = 0, binding = 1, std430) restrict readonly buffer Queries {
	uvec4 data[];
}
queries;

// One result per query: texel x, texel y, mip with the valid bit in bit 31, entry.
layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	uvec4 data[];
}
results;

layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.count) {
		return;
	}
	uvec4 query = queries.data[index * 3u];
	uvec4 light = queries.data[index * 3u + 1u];
	uint table_maps = queries.data[index * 3u + 2u].x;
	uint mip = query.w & 0xFFu;
	uint kind = (query.w >> 8u) & 0xFFu;
	VSMLookup lookup;
	if (kind == 2u) {
		lookup = vsm_lookup_clipmap(table_maps, vsm_light_map(query.x, light.x, light.y), ivec2(light.zw), ivec2(query.yz), mip);
	} else {
		uint map = kind == 1u ? vsm_light_map(query.x, light.x, light.y) : query.x;
		lookup = vsm_lookup(table_maps, map, uintBitsToFloat(query.yz), mip);
	}
	results.data[index] = uvec4(lookup.texel, lookup.mip | (lookup.valid ? 0x80000000u : 0u), lookup.entry);
}

#endif
