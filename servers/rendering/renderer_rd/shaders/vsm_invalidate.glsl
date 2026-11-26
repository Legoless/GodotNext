#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_caching (#212, plan §21.3 contract S5). Per-page VSM cache invalidation pass,
// dispatched before vsm_mark_pages.glsl and vsm_allocate_pages.glsl:
// MODE_MAPS: one workgroup per light map that moved, rotated or changed properties. Marks every
//   allocated entry of the map VSM_ENTRY_DIRTY_STATIC (and VSM_ENTRY_DIRTY_DYNAMIC when the page
//   had dynamic depth on a previous frame).
// MODE_BOXES: one workgroup per moved or dynamic caster's swept world AABB (previous U current).
//   Projects the box into every active map whose shadow caster mask matches the box's layer mask
//   and sets the box's dirty flags (VSM_ENTRY_DIRTY_STATIC and/or VSM_ENTRY_DIRTY_DYNAMIC) on
//   overlapping allocated pages across all mips.
// CPU models: RendererVirtualShadowMapStorage::invalidate_box_model() and invalidate_map_model().

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

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

layout(set = 0, binding = 2, std430) restrict readonly buffer VSMMapState {
	uint data[];
}
map_state;

layout(set = 0, binding = 3, std430) restrict buffer VSMState {
	uint data[];
}
state;

layout(set = 0, binding = 4, std430) restrict buffer VSMPages {
	uvec4 data[];
}
pages;

#ifdef MODE_BOXES
struct VSMInvalidationBox {
	vec3 min_pos;
	uint layer_mask;
	vec3 max_pos;
	uint dirty_flags;
};

layout(set = 0, binding = 5, std430) restrict readonly buffer VSMInvalidationBoxes {
	VSMInvalidationBox data[];
}
invalidation_boxes;
#endif

#ifdef MODE_MAPS
// x = map, y = dirty_flags, z = dynamic_if_used (0 or 1), w = pad.
layout(set = 0, binding = 5, std430) restrict readonly buffer VSMInvalidationMaps {
	uvec4 data[];
}
invalidation_maps;
#endif

layout(push_constant, std430) uniform Params {
	uint count;
	uint table_maps;
	uint pool_pages;
	uint grid_width;
}
params;

// Used results of atomics (#277 D3D12 GBV compliance).
void vsm_sink(uint value) {
	if (value == 0xFFFFFFFFu) {
		state.data[VSM_STATE_SINK] = value;
	}
}

void vsm_invalidate_entry(uint index, uint dirty_flags, bool dynamic_if_used) {
	uint entry = vsm_page_table.entries[index];
	if ((entry & VSM_ENTRY_ALLOCATED) == 0u) {
		return;
	}
	uvec2 xy = vsm_entry_page(entry);
	uint p = xy.y * params.grid_width + xy.x;
	uint flags = dirty_flags;
	if (dynamic_if_used && p < params.pool_pages) {
		if ((pages.data[p].w & (VSM_PAGE_FLAG_HAS_DYNAMIC_PREV | VSM_PAGE_FLAG_HAS_DYNAMIC_CURR)) != 0u) {
			flags |= VSM_ENTRY_DIRTY_DYNAMIC;
		}
	}
	if (flags == 0u) {
		return;
	}
	uint prev = atomicOr(vsm_page_table.entries[index], flags);
	if ((flags & VSM_ENTRY_DIRTY_STATIC) != 0u && (prev & VSM_ENTRY_DIRTY_STATIC) == 0u) {
		vsm_sink(atomicAdd(state.data[VSM_STATE_INVALIDATED_STATIC], 1u));
	}
	if ((flags & VSM_ENTRY_DIRTY_DYNAMIC) != 0u) {
		if ((prev & VSM_ENTRY_DIRTY_DYNAMIC) == 0u) {
			vsm_sink(atomicAdd(state.data[VSM_STATE_INVALIDATED_DYNAMIC], 1u));
		}
		if (!dynamic_if_used && p < params.pool_pages) {
			vsm_sink(atomicOr(pages.data[p].w, VSM_PAGE_FLAG_HAS_DYNAMIC_CURR));
		}
	}
}

#ifdef MODE_BOXES

// Exact operation order matching RendererVirtualShadowMapStorage::row_dot() without FMA contraction.
float vsm_invalidate_row(vec4 r, vec3 p) {
	precise float sum = r.x * p.x;
	sum = sum + r.y * p.y;
	sum = sum + r.z * p.z;
	return sum + r.w;
}

void main() {
	uint box_idx = gl_WorkGroupID.x;
	if (box_idx >= params.count) {
		return;
	}
	VSMInvalidationBox box = invalidation_boxes.data[box_idx];
	uint dirty_flags = box.dirty_flags & VSM_ENTRY_DIRTY_MASK;
	if (dirty_flags == 0u) {
		return;
	}
	vec3 bmin = box.min_pos;
	vec3 bmax = box.max_pos;
	if (!(bmax.x >= bmin.x && bmax.y >= bmin.y && bmax.z >= bmin.z)) {
		return;
	}
	for (uint m = 0u; m < params.table_maps; m++) {
		VSMMapData md = vsm_maps.data[m];
		if (md.kind == VSM_MAP_KIND_NONE) {
			continue;
		}
		uint cull_mask = map_state.data[VSM_MAP_STATE_CULL_MASK * params.table_maps + m];
		if ((cull_mask & box.layer_mask) == 0u) {
			continue;
		}
		if (md.kind == VSM_MAP_KIND_DIRECTIONAL) {
			ivec2 level_min = ivec2(0x7FFFFFFF);
			ivec2 level_max = ivec2(-0x7FFFFFFF - 1);
			for (uint c = 0u; c < 8u; c++) {
				vec3 corner = vec3(
						(c & 1u) != 0u ? bmax.x : bmin.x,
						(c & 2u) != 0u ? bmax.y : bmin.y,
						(c & 4u) != 0u ? bmax.z : bmin.z);
				vec2 t = floor(vec2(vsm_invalidate_row(md.row0, corner), vsm_invalidate_row(md.row1, corner)));
				t = clamp(t, vec2(-1073741824.0), vec2(1073741824.0));
				ivec2 texel = ivec2(t);
				level_min = min(level_min, texel);
				level_max = max(level_max, texel);
			}
			ivec2 origin = md.origin;
			for (uint mip = 0u; mip < VSM_MIP_COUNT; mip++) {
				ivec2 window_start = ((origin >> int(mip)) << int(mip)) * int(VSM_PAGE_SIZE);
				ivec2 lo = max(level_min, origin * int(VSM_PAGE_SIZE));
				ivec2 hi = min(level_max, window_start + ivec2(int(VSM_VIRTUAL_SIZE) - 1));
				if (any(greaterThan(lo, hi))) {
					continue;
				}
				int shift = int(mip + VSM_PAGE_SIZE_SHIFT);
				ivec2 p0 = lo >> shift;
				ivec2 p1 = hi >> shift;
				uint nx = uint(p1.x - p0.x + 1);
				uint ny = uint(p1.y - p0.y + 1);
				uint total = nx * ny;
				uint mask = (VSM_PAGES_PER_SIDE >> mip) - 1u;
				for (uint k = gl_LocalInvocationID.x; k < total; k += 64u) {
					int px = p0.x + int(k % nx);
					int py = p0.y + int(k / nx);
					uint idx = vsm_entry_index(m, mip, uvec2(uint(px), uint(py)) & uvec2(mask));
					vsm_invalidate_entry(idx, dirty_flags, false);
				}
			}
			continue;
		}

		// Local face (spot, omni, area): 6-plane frustum test + near-plane straddle fallback.
		uint outside[6] = uint[](0u, 0u, 0u, 0u, 0u, 0u);
		vec2 uv_min = vec2(1.0);
		vec2 uv_max = vec2(0.0);
		bool cut = false;
		for (uint c = 0u; c < 8u; c++) {
			vec3 corner = vec3(
					(c & 1u) != 0u ? bmax.x : bmin.x,
					(c & 2u) != 0u ? bmax.y : bmin.y,
					(c & 4u) != 0u ? bmax.z : bmin.z);
			precise float x = vsm_invalidate_row(md.row0, corner);
			precise float y = vsm_invalidate_row(md.row1, corner);
			precise float w = vsm_invalidate_row(md.row3, corner);
			outside[0] += (w - x < 0.0) ? 1u : 0u;
			outside[1] += (w + x < 0.0) ? 1u : 0u;
			outside[2] += (w - y < 0.0) ? 1u : 0u;
			outside[3] += (w + y < 0.0) ? 1u : 0u;
			outside[4] += (w - md.near_w < 0.0) ? 1u : 0u;
			outside[5] += (w > md.max_distance) ? 1u : 0u;
			if (!(w > md.near_w)) {
				cut = true;
				continue;
			}
			precise float u = (x / w) * 0.5 + 0.5;
			precise float v = (y / w) * 0.5 + 0.5;
			vec2 uv = vec2(u, v);
			uv_min = min(uv_min, uv);
			uv_max = max(uv_max, uv);
		}
		if (outside[0] == 8u || outside[1] == 8u || outside[2] == 8u || outside[3] == 8u || outside[4] == 8u || outside[5] == 8u) {
			continue;
		}
		uvec2 t0 = uvec2(0u);
		uvec2 t1 = uvec2(VSM_VIRTUAL_SIZE - 1u);
		if (!cut) {
			t0 = min(uvec2(clamp(uv_min, vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
			t1 = min(uvec2(clamp(uv_max, vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
		}
		for (uint mip = 0u; mip < VSM_MIP_COUNT; mip++) {
			uint shift = mip + VSM_PAGE_SIZE_SHIFT;
			uvec2 p0 = t0 >> shift;
			uvec2 p1 = t1 >> shift;
			uint nx = p1.x - p0.x + 1u;
			uint ny = p1.y - p0.y + 1u;
			uint total = nx * ny;
			for (uint k = gl_LocalInvocationID.x; k < total; k += 64u) {
				uvec2 page = uvec2(p0.x + (k % nx), p0.y + (k / nx));
				vsm_invalidate_entry(vsm_entry_index(m, mip, page), dirty_flags, false);
			}
		}
	}
}

#endif

#ifdef MODE_MAPS

void main() {
	uint item = gl_WorkGroupID.x;
	if (item >= params.count) {
		return;
	}
	uvec4 rec = invalidation_maps.data[item];
	uint m = rec.x;
	uint flags = rec.y & VSM_ENTRY_DIRTY_MASK;
	bool dyn_if_used = rec.z != 0u;
	if (m >= params.table_maps) {
		return;
	}
	uint base = m * VSM_ENTRIES_PER_MAP;
	for (uint local = gl_LocalInvocationID.x; local < VSM_ENTRIES_PER_MAP; local += 64u) {
		vsm_invalidate_entry(base + local, flags, dyn_if_used);
	}
}

#endif
