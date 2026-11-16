// GodotNext: vsm_page_pool (#220, plan §21.3 contract S2). The Virtual Shadow Map page-table
// lookup. The VSM_* defines come from RendererVirtualShadowMapStorage::get_glsl_defines(), and
// the including shader declares the page-table buffer as `vsm_page_table` with a `uint entries[]`
// member. RendererVirtualShadowMapStorage::lookup(), lookup_clipmap() and light_map() are the CPU
// models of vsm_lookup(), vsm_lookup_clipmap() and vsm_light_map(); keep them in step. The lookups
// take the table's map count from the caller, which gets it with the buffer
// (RendererVirtualShadowMapStorage::get_table_capacity()).
//
// Each map owns VSM_ENTRIES_PER_MAP consecutive entries: the mip chain from mip 0
// (VSM_PAGES_PER_SIDE^2 entries) down to one entry, row by row. An entry holds the physical page's
// x in bits 0-11, its y in bits 12-23 and the VSM_ENTRY_* flags in bits 24-31.

struct VSMLookup {
	uvec2 texel; // The physical texel in the pool, when valid.
	uint mip; // The mip the entry came from, or VSM_MIP_COUNT.
	uint entry; // The entry, or 0.
	bool valid;
};

uint vsm_mip_entry_offset(uint mip) {
	return ((1u << (2u * VSM_MIP_COUNT)) - (1u << (2u * (VSM_MIP_COUNT - mip)))) / 3u;
}

uint vsm_entry_index(uint map, uint mip, uvec2 page) {
	return map * VSM_ENTRIES_PER_MAP + vsm_mip_entry_offset(mip) + page.y * (VSM_PAGES_PER_SIDE >> mip) + page.x;
}

uvec2 vsm_entry_page(uint entry) {
	return uvec2(entry & VSM_ENTRY_PAGE_MASK, (entry >> VSM_ENTRY_PAGE_BITS) & VSM_ENTRY_PAGE_MASK);
}

// The map of a light's clipmap level or face, from LightData's vsm_map_base and vsm_flags: VSM_MAP_NONE
// when the light has no maps or index is not below its map count. Never add the index to
// vsm_map_base directly: VSM_MAP_NONE + 1 wraps to map 0, another light's map.
// RendererVirtualShadowMapStorage::light_map() is the CPU model.
uint vsm_light_map(uint map_base, uint light_flags, uint index) {
	uint count = (light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	return (map_base == VSM_MAP_NONE || index >= count) ? VSM_MAP_NONE : map_base + index;
}

VSMLookup vsm_lookup_miss() {
	VSMLookup result;
	result.texel = uvec2(0u);
	result.mip = VSM_MIP_COUNT;
	result.entry = 0u;
	result.valid = false;
	return result;
}

// Looks up uv (the map's virtual space, clamped to [0, 1]) at mip (clamped to the last mip) in a
// spot, omni or area map, falling back to coarser mips until an entry is allocated. table_maps is
// the page table's map count (RendererVirtualShadowMapStorage::get_table_capacity()): a map that is
// VSM_MAP_NONE or not below it misses without reading the table. Not for directional clipmap
// levels: their mips do not cover the same ground, so use vsm_lookup_clipmap().
VSMLookup vsm_lookup(uint table_maps, uint map, vec2 uv, uint mip) {
	VSMLookup result = vsm_lookup_miss();
	if (map >= table_maps) {
		return result;
	}

	// Integer virtual texels first, so that every mip and the CPU model agree exactly.
	uvec2 texel = min(uvec2(clamp(uv, vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
	for (uint m = min(mip, VSM_MIP_COUNT - 1u); m < VSM_MIP_COUNT; m++) {
		uvec2 mip_texel = texel >> m;
		uint entry = vsm_page_table.entries[vsm_entry_index(map, m, mip_texel >> VSM_PAGE_SIZE_SHIFT)];
		if ((entry & VSM_ENTRY_ALLOCATED) != 0u) {
			result.texel = (vsm_entry_page(entry) << VSM_PAGE_SIZE_SHIFT) | (mip_texel & uvec2(VSM_PAGE_SIZE - 1u));
			result.mip = m;
			result.entry = entry;
			result.valid = true;
			break;
		}
	}
	return result;
}

// Looks up a directional clipmap level. level_origin is the absolute mip 0 page where the level's
// window starts, and texel the absolute mip 0 virtual texel; both may be negative. The map is
// addressed toroidally: the slot is the absolute texel modulo VSM_VIRTUAL_SIZE. At mip m the
// window starts at floor(level_origin / 2^m) and holds VSM_PAGES_PER_SIDE >> m pages, so when the
// origin is not a multiple of 2^m the level's far edge lies outside it and its slot holds the near
// edge. A texel outside the level misses, and the fallback stops at the first mip whose page is
// outside that mip's window (every coarser mip's is too): the caller then tries the next clipmap
// level. RendererVirtualShadowMapStorage::lookup_clipmap() is the CPU model.
VSMLookup vsm_lookup_clipmap(uint table_maps, uint map, ivec2 level_origin, ivec2 texel, uint mip) {
	VSMLookup result = vsm_lookup_miss();
	if (map >= table_maps) {
		return result;
	}
	ivec2 rel = texel - level_origin * int(VSM_PAGE_SIZE);
	if (any(lessThan(rel, ivec2(0))) || any(greaterThanEqual(rel, ivec2(int(VSM_VIRTUAL_SIZE))))) {
		return result;
	}
	uvec2 window_page = uvec2(rel) >> VSM_PAGE_SIZE_SHIFT;
	uvec2 wrapped = uvec2(texel) & uvec2(VSM_VIRTUAL_SIZE - 1u);
	for (uint m = min(mip, VSM_MIP_COUNT - 1u); m < VSM_MIP_COUNT; m++) {
		// The page at mip m is floor(origin / 2^m) + (((origin mod 2^m) + window_page) >> m): inside
		// the mip's window while (origin mod 2^m) + window_page < VSM_PAGES_PER_SIDE.
		uvec2 offset = (uvec2(level_origin) & uvec2((1u << m) - 1u)) + window_page;
		if (any(greaterThanEqual(offset, uvec2(VSM_PAGES_PER_SIDE)))) {
			break;
		}
		uvec2 mip_texel = wrapped >> m;
		uint entry = vsm_page_table.entries[vsm_entry_index(map, m, mip_texel >> VSM_PAGE_SIZE_SHIFT)];
		if ((entry & VSM_ENTRY_ALLOCATED) != 0u) {
			result.texel = (vsm_entry_page(entry) << VSM_PAGE_SIZE_SHIFT) | (mip_texel & uvec2(VSM_PAGE_SIZE - 1u));
			result.mip = m;
			result.entry = entry;
			result.valid = true;
			break;
		}
	}
	return result;
}
