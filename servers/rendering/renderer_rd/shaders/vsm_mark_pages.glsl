/**************************************************************************/
/*  vsm_mark_pages.glsl                                                   */
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

#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3). The page marking pass: every page a
// view will sample is flagged VSM_ENTRY_REQUESTED in the page table, and the first request of an
// entry appends it to the request list that vsm_allocate_pages.glsl works through.
// MODE_PIXEL: one thread per pixel of the depth prepass. Each directional light with maps, and each
//   omni, spot and area light of the pixel's cluster (the bits light_process_omni() and friends
//   read) that reaches the pixel, requests the page whose mip gives at most one shadow texel per
//   pixel footprint, plus the configured LOD bias and the map's overflow bias.
// MODE_FROXEL: one thread per vertex of the volumetric fog froxel grid: the lights the fog samples
//   request their coarsest VSM_COARSE_MIPS mips (VSM_ENTRY_COARSE). A local light counts when its
//   range reaches within one cell diagonal of the vertex, as the fog samples anywhere in its cells.
// MODE_BOX: one workgroup per transparent instance's world box: every light that reaches it
//   requests the coarse pages the box's corners span.
// MODE_RESIDENT: one thread per light of the view, dispatched before the other modes with a barrier
//   after it: every light with maps requests its always-resident pages (the root page of each face,
//   or mips VSM_CLIPMAP_RESIDENT_MIP and coarser of each clipmap level). They take the request list's
//   first VSM_STATE_RESIDENT entries, which allocation serves before any other request.
// The CPU models are RendererVirtualShadowMapStorage::mark_local_point(), mark_directional_point(),
// mark_local_box(), mark_directional_box() and mark_resident(); the GPU check compares their results.

#ifdef MODE_PIXEL
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif
#ifdef MODE_FROXEL
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
#endif
#if defined(MODE_BOX) || defined(MODE_RESIDENT)
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

layout(set = 0, binding = 0, std430) restrict buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;

#include "servers/rendering/renderer_rd/shaders/light_data_inc.glsl"
#include "servers/rendering/renderer_rd/shaders/vsm_mark_inc.glsl"
#include "servers/rendering/renderer_rd/shaders/vsm_page_table_inc.glsl"
#include "servers/rendering/renderer_rd/shaders/vsm_state_inc.glsl"

layout(set = 0, binding = 1, std430) restrict readonly buffer VSMMaps {
	VSMMapData data[];
}
vsm_maps;

// VSM_MAP_STATE_* regions of VSMViewParams' table map count each.
layout(set = 0, binding = 2, std430) restrict buffer VSMMapState {
	uint data[];
}
map_state;

layout(set = 0, binding = 3, std430) restrict buffer VSMState {
	uint data[];
}
state;

layout(set = 0, binding = 4, std430) restrict writeonly buffer VSMRequests {
	uint data[];
}
requests;

layout(set = 0, binding = 5, std430) restrict readonly buffer VSMView {
	VSMViewParams data;
}
view;

layout(set = 0, binding = 6, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 0, binding = 7, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 8, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 9, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;

#ifdef MODE_PIXEL
layout(set = 0, binding = 10) uniform sampler2D depth_buffer;

// The depth bits each pixel read, for the GPU check (VSMViewParams.screen.z set).
layout(set = 0, binding = 11, std430) restrict writeonly buffer CaptureDepth {
	uint data[];
}
capture_depth;

// GodotNext: vsm_caching (#212). Screen pixel count per physical page for deferred static invalidation
// ordering (written only when view.data.misc.w > 0).
layout(set = 0, binding = 13, std430) restrict buffer VSMPageCoverage {
	uint data[];
}
page_coverage;
#endif

#ifdef MODE_BOX
// Two vec4 per box: the world-space minimum and maximum.
layout(set = 0, binding = 12, std430) restrict readonly buffer Boxes {
	vec4 data[];
}
boxes;
#endif

// Used results of atomics: with D3D12 GPU-based validation, an atomic whose result is unused does not
// land (#277).
void vsm_sink(uint value) {
	if (value == 0xFFFFFFFFu) {
		state.data[VSM_STATE_SINK] = value;
	}
}

// Flags an entry requested. The first request of an entry in this view appends it to the request
// list and counts it for its map; when the list is full the request is dropped and its flags undone.
void vsm_request(uint map, uint mip, uvec2 page, bool coarse) {
	uint index = vsm_entry_index(map, mip, page);
	uint flags = VSM_ENTRY_REQUESTED | (coarse ? VSM_ENTRY_COARSE : 0u);
	uint previous = atomicOr(vsm_page_table.entries[index], flags);
#ifdef MODE_PIXEL
	// GodotNext: vsm_caching (#212). Count screen pixels covering already-allocated DIRTY_STATIC pages
	// when deferred_invalidation_budget > 0 so allocation prioritizes highest screen coverage first.
	if (view.data.misc.w > 0u && (previous & (VSM_ENTRY_ALLOCATED | VSM_ENTRY_DIRTY_STATIC)) == (VSM_ENTRY_ALLOCATED | VSM_ENTRY_DIRTY_STATIC)) {
		uvec2 xy = vsm_entry_page(previous);
		uint p = xy.y * uint(view.data.bias.z) + xy.x;
		if (p < uint(view.data.bias.w)) {
			vsm_sink(atomicAdd(page_coverage.data[p], 1u));
		}
	}
#endif
	if ((previous & VSM_ENTRY_REQUESTED) != 0u) {
		return;
	}
	uint slot = atomicAdd(state.data[VSM_STATE_REQUESTS], 1u);
	if (slot < view.data.screen.w) {
		requests.data[slot] = index;
		vsm_sink(atomicAdd(map_state.data[VSM_MAP_STATE_REQUESTS * view.data.misc.x + map], 1u));
#ifdef MODE_RESIDENT
		// Nothing else is marked yet: the resident requests are the list's first entries.
		vsm_sink(atomicAdd(state.data[VSM_STATE_RESIDENT], 1u));
#endif
	} else {
		vsm_sink(atomicAnd(vsm_page_table.entries[index], ~(VSM_ENTRY_REQUESTED | VSM_ENTRY_COARSE)));
		vsm_sink(atomicAdd(state.data[VSM_STATE_REQUESTS_DROPPED], 1u));
	}
}

uint vsm_overflow_bias(uint map) {
	return map_state.data[VSM_MAP_STATE_BIAS * view.data.misc.x + map] & 0xFFu;
}

vec3 vsm_view_to_world(vec3 view_pos) {
	return vec3(vsm_row(view.data.view_to_world[0], view_pos), vsm_row(view.data.view_to_world[1], view_pos), vsm_row(view.data.view_to_world[2], view_pos));
}

// A local light's requests at a point (RendererVirtualShadowMapStorage::mark_local_point()).
void vsm_mark_local(uint map_base, uint light_flags, vec3 world, float footprint, bool coarse, bool all_faces) {
	uint count = (light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	for (uint face = 0u; face < count; face++) {
		uint map = vsm_light_map(map_base, light_flags, face);
		if (map >= view.data.misc.x) {
			continue;
		}
		VSMMapData map_data = vsm_maps.data[map];
		if (map_data.kind == VSM_MAP_KIND_NONE || map_data.kind == VSM_MAP_KIND_DIRECTIONAL) {
			continue;
		}
		float w;
		uvec2 texel;
		bool inside = vsm_face_texel(map_data, world, w, texel);
		if (coarse) {
			if (!inside && !all_faces) {
				continue;
			}
			for (uint mip = VSM_COARSE_FIRST_MIP; mip < VSM_MIP_COUNT; mip++) {
				if (inside) {
					vsm_request(map, mip, (texel >> mip) >> VSM_PAGE_SIZE_SHIFT, true);
				} else {
					uint pages = VSM_PAGES_PER_SIDE >> mip;
					for (uint y = 0u; y < pages; y++) {
						for (uint x = 0u; x < pages; x++) {
							vsm_request(map, mip, uvec2(x, y), true);
						}
					}
				}
			}
			continue;
		}
		if (!inside) {
			continue;
		}
		// GodotNext: vsm_local_lights (#216). Per-light resolution bias and distant-light coarse-only policy.
		int res_int_bias;
		float res_scale;
		vsm_resolution_bias_parts(light_flags, res_int_bias, res_scale);
		bool distant_coarse = (light_flags & VSM_LIGHT_FLAGS_DISTANT_COARSE) != 0u;
		int bias = view.data.bias.y + map_data.lod_bias + res_int_bias + int(vsm_overflow_bias(map));
		uint mip = vsm_biased_mip(int(vsm_face_mip(w * map_data.texel_scale, footprint * res_scale)), bias);
		if (distant_coarse && mip < VSM_COARSE_FIRST_MIP) {
			mip = VSM_COARSE_FIRST_MIP;
		}
		vsm_request(map, mip, (texel >> mip) >> VSM_PAGE_SIZE_SHIFT, distant_coarse);
	}
}

// A directional light's request at a point (RendererVirtualShadowMapStorage::mark_directional_point()).
void vsm_mark_directional(uint map_base, uint light_flags, vec3 world, float footprint, bool coarse) {
	uint count = (light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint first = vsm_light_map(map_base, light_flags, 0u);
	if (first >= view.data.misc.x || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
		return;
	}
	ivec2 texel0 = vsm_clipmap_texel0(vsm_maps.data[first], world);
	if (coarse) {
		for (uint mip = VSM_CLIPMAP_RESIDENT_MIP; mip < VSM_MIP_COUNT; mip++) {
			for (uint level = 0u; level < count; level++) {
				uint map = first + level;
				if (map >= view.data.misc.x) {
					break;
				}
				ivec2 level_texel = texel0 >> int(level);
				if (vsm_clipmap_in_window(vsm_maps.data[map].origin, level_texel, mip)) {
					vsm_request(map, mip, vsm_clipmap_page(level_texel, mip), true);
					break;
				}
			}
		}
		// The last level's far edge: outside mip VSM_CLIPMAP_RESIDENT_MIP's window, with no coarser level.
		uint last_map = first + count - 1u;
		if (count > 0u && last_map < view.data.misc.x) {
			ivec2 origin = vsm_maps.data[last_map].origin;
			ivec2 level_texel = texel0 >> int(count - 1u);
			if (vsm_clipmap_in_window(origin, level_texel, 0u) && !vsm_clipmap_in_window(origin, level_texel, VSM_CLIPMAP_RESIDENT_MIP)) {
				uint resident = 0u;
				while (resident + 1u < VSM_CLIPMAP_RESIDENT_MIP && vsm_clipmap_in_window(origin, level_texel, resident + 1u)) {
					resident++;
				}
				vsm_request(last_map, resident, vsm_clipmap_page(level_texel, resident), true);
			}
		}
		return;
	}
	int res_int_bias;
	float res_scale;
	vsm_resolution_bias_parts(light_flags, res_int_bias, res_scale);
	int level_mip = vsm_clipmap_level_mip(footprint * res_scale, view.data.bias.x + vsm_maps.data[first].lod_bias + res_int_bias);
	for (uint level = 0u; level < count; level++) {
		uint map = first + level;
		if (map >= view.data.misc.x) {
			break;
		}
		ivec2 origin = vsm_maps.data[map].origin;
		ivec2 level_texel = texel0 >> int(level);
		if (!vsm_clipmap_in_window(origin, level_texel, 0u)) {
			continue;
		}
		uint mip = min(vsm_biased_mip(level_mip - int(level), 0) + vsm_overflow_bias(map), VSM_MIP_COUNT - 1u);
		if (level + 1u == count) {
			while (mip > 0u && !vsm_clipmap_in_window(origin, level_texel, mip)) {
				mip--;
			}
		} else if (!vsm_clipmap_in_window(origin, level_texel, mip)) {
			continue;
		}
		// The last level's far edge: outside its resident mips' window, and no coarser level holds it.
		// The page at the coarsest mip whose window holds the point is requested ahead of the fine
		// requests, so the sample always finds a page.
		bool far_resident = level + 1u == count && !vsm_clipmap_in_window(origin, level_texel, VSM_CLIPMAP_RESIDENT_MIP);
		uint resident = mip;
		if (far_resident) {
			while (resident + 1u < VSM_CLIPMAP_RESIDENT_MIP && vsm_clipmap_in_window(origin, level_texel, resident + 1u)) {
				resident++;
			}
		}
		vsm_request(map, mip, vsm_clipmap_page(level_texel, mip), far_resident && resident == mip);
		if (far_resident && resident != mip) {
			vsm_request(map, resident, vsm_clipmap_page(level_texel, resident), true);
		}
		return;
	}
}

// A light's always-resident requests (RendererVirtualShadowMapStorage::mark_resident()): the root page
// of each spot, omni and area face, or every page of mips VSM_CLIPMAP_RESIDENT_MIP and coarser of each
// clipmap level.
void vsm_mark_resident(uint map_base, uint light_flags) {
	uint count = (light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	if ((light_flags & VSM_LIGHT_FLAGS_KIND_MASK) == VSM_MAP_KIND_DIRECTIONAL) {
		uint first = vsm_light_map(map_base, light_flags, 0u);
		if (first >= view.data.misc.x || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
			return;
		}
		for (uint level = 0u; level < count; level++) {
			uint map = first + level;
			if (map >= view.data.misc.x) {
				break;
			}
			for (uint mip = VSM_CLIPMAP_RESIDENT_MIP; mip < VSM_MIP_COUNT; mip++) {
				uint pages = VSM_PAGES_PER_SIDE >> mip;
				for (uint y = 0u; y < pages; y++) {
					for (uint x = 0u; x < pages; x++) {
						vsm_request(map, mip, uvec2(x, y), true);
					}
				}
			}
		}
		return;
	}
	for (uint face = 0u; face < count; face++) {
		uint map = vsm_light_map(map_base, light_flags, face);
		if (map >= view.data.misc.x) {
			continue;
		}
		uint kind = vsm_maps.data[map].kind;
		if (kind == VSM_MAP_KIND_NONE || kind == VSM_MAP_KIND_DIRECTIONAL) {
			continue;
		}
		vsm_request(map, VSM_MIP_COUNT - 1u, uvec2(0u), true);
	}
}

#if defined(MODE_PIXEL) || defined(MODE_FROXEL)

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;

	item_from = item_min >> 5;
	item_to = (item_max == 0) ? 0 : ((item_max - 1) >> 5) + 1; //side effect of how it is stored, as item_max 0 means no elements
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}

// The lights of one type (0 omni, 1 spot, 2 area) in a cluster cell that reach the point.
void vsm_mark_cluster_type(uint type, uint cluster_offset, uint cluster_z, vec3 view_pos, vec3 world, float footprint, bool coarse, float cell_size) {
	uint type_offset = cluster_offset + view.data.cluster.z * type;
	uint item_min;
	uint item_max;
	uint item_from;
	uint item_to;
	cluster_get_item_range(type_offset + view.data.cluster.w + cluster_z, item_min, item_max, item_from, item_to);
	for (uint i = item_from; i < item_to; i++) {
		uint mask = cluster_buffer.data[type_offset + i];
		mask &= cluster_get_range_clip_mask(i, item_min, item_max);
		while (mask != 0u) {
			uint bit = findMSB(mask);
			mask &= ~(1u << bit);
			uint light_index = 32u * i + bit;
			LightData light;
			uint kind;
			float inv_radius;
			if (type == 0u) {
				light = omni_lights.data[light_index];
				kind = VSM_MAP_KIND_OMNI;
				inv_radius = light.inv_radius;
			} else if (type == 1u) {
				light = spot_lights.data[light_index];
				kind = VSM_MAP_KIND_SPOT;
				inv_radius = light.inv_radius;
			} else {
				light = area_lights.data[light_index];
				kind = VSM_MAP_KIND_AREA;
				inv_radius = light.cone_attenuation;
			}
			if (light.vsm_map_base == VSM_MAP_NONE || (coarse && !(light.volumetric_fog_energy > 0.0))) {
				continue;
			}
			vec3 rel = view_pos - light.position;
			// A fog vertex within a cell of the light: the cells around it may meet every face.
			bool all_faces = false;
			if (coarse) {
				if (!vsm_light_reaches_cell(rel, inv_radius, cell_size, all_faces)) {
					continue;
				}
			} else if (!vsm_light_reaches(kind, rel, inv_radius, light.direction, light.cone_angle)) {
				continue;
			}
			vsm_mark_local(light.vsm_map_base, light.vsm_flags, world, footprint, coarse, all_faces);
		}
	}
}

void vsm_mark_point(uvec2 screen_pos, vec3 view_pos, bool coarse, float cell_size) {
	float view_depth = -view_pos.z;
	float footprint = view_depth * view.data.footprint.x + view.data.footprint.y;
	vec3 world = vsm_view_to_world(view_pos);
	for (uint i = 0u; i < view.data.counts.x; i++) {
		uvec4 light = view.data.directional[i];
		if (view_depth > uintBitsToFloat(light.z) || (coarse && !(uintBitsToFloat(light.w) > 0.0))) {
			continue;
		}
		vsm_mark_directional(light.x, light.y, world, footprint, coarse);
	}
	uvec2 cluster_pos = screen_pos >> view.data.cluster.x;
	uint cluster_offset = (view.data.cluster.y * cluster_pos.y + cluster_pos.x) * (view.data.cluster.w + 32u);
	uint cluster_z = uint(clamp((view_depth / view.data.footprint.z) * 32.0, 0.0, 31.0));
	for (uint type = 0u; type < 3u; type++) {
		vsm_mark_cluster_type(type, cluster_offset, cluster_z, view_pos, world, footprint, coarse, cell_size);
	}
}

#endif

#ifdef MODE_PIXEL

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	uvec2 size = view.data.screen.xy;
	if (any(greaterThanEqual(pixel, size))) {
		return;
	}
	float depth = texelFetch(depth_buffer, ivec2(pixel), 0).r;
	if (view.data.screen.z != 0u) {
		capture_depth.data[pixel.y * size.x + pixel.x] = floatBitsToUint(depth);
	}
	if (!(depth > 0.0)) {
		return; // Reverse Z: the sky.
	}
	vec2 ndc = vec2((float(pixel.x) + 0.5) / float(size.x), (float(pixel.y) + 0.5) / float(size.y)) * 2.0 - 1.0;
	vec3 clip = vec3(ndc, depth);
	float w = vsm_row(view.data.inv_projection[3], clip);
	vec3 view_pos = vec3(vsm_row(view.data.inv_projection[0], clip), vsm_row(view.data.inv_projection[1], clip), vsm_row(view.data.inv_projection[2], clip)) / w;
	vsm_mark_point(pixel, view_pos, false, 0.0);
}

#endif

#ifdef MODE_FROXEL

// The view-space position of a froxel grid vertex, as volumetric_fog_process.glsl places froxels.
vec3 vsm_froxel_vertex(uvec3 vertex) {
	vec3 unit = vec3(vertex) / vec3(view.data.fog_size.xyz);
	unit.z = pow(unit.z, view.data.fog_a.y);
	vec3 view_pos;
	view_pos.xy = (unit.xy * 2.0 - 1.0) * mix(view.data.fog_a.zw, view.data.fog_b.xy, vec2(unit.z));
	view_pos.z = -view.data.fog_a.x * unit.z;
	view_pos.y = -view_pos.y;
	return view_pos;
}

void main() {
	uvec3 vertex = gl_GlobalInvocationID.xyz;
	if (any(greaterThan(vertex, view.data.fog_size.xyz))) {
		return;
	}
	vec3 view_pos = vsm_froxel_vertex(vertex);
	// The cell's diagonal toward the next vertex: a light this close may sit inside the cell.
	vec3 next = vsm_froxel_vertex(vertex + uvec3(1u));
	vec3 diagonal = next - view_pos;
	float cell_size = sqrt(dot(diagonal, diagonal));
	vec2 unit = vec2(vertex.xy) / vec2(view.data.fog_size.xy);
	uvec2 screen_pos = min(uvec2(unit * vec2(view.data.screen.xy)), view.data.screen.xy - uvec2(1u));
	vsm_mark_point(screen_pos, view_pos, true, cell_size);
}

#endif

#ifdef MODE_BOX

void main() {
	uint box = gl_WorkGroupID.x;
	if (box >= view.data.misc.y) {
		return;
	}
	vec3 box_min = boxes.data[box * 2u].xyz;
	vec3 box_max = boxes.data[box * 2u + 1u].xyz;
	uint total = view.data.counts.x + view.data.counts.y + view.data.counts.z + view.data.counts.w;
	for (uint l = gl_LocalInvocationID.x; l < total; l += 64u) {
		if (l < view.data.counts.x) {
			uvec4 light = view.data.directional[l];
			uint count = (light.y >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
			uint first = vsm_light_map(light.x, light.y, 0u);
			if (first >= view.data.misc.x || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
				continue;
			}
			ivec2 texel_min = ivec2(0x7FFFFFFF);
			ivec2 texel_max = ivec2(-0x7FFFFFFF - 1);
			for (uint c = 0u; c < 8u; c++) {
				vec3 corner = vec3((c & 1u) != 0u ? box_max.x : box_min.x, (c & 2u) != 0u ? box_max.y : box_min.y, (c & 4u) != 0u ? box_max.z : box_min.z);
				ivec2 texel = vsm_clipmap_texel0(vsm_maps.data[first], corner);
				texel_min = min(texel_min, texel);
				texel_max = max(texel_max, texel);
			}
			for (uint level = 0u; level < count; level++) {
				uint map = first + level;
				if (map >= view.data.misc.x) {
					break;
				}
				ivec2 origin = vsm_maps.data[map].origin;
				ivec2 level_min = texel_min >> int(level);
				ivec2 level_max = texel_max >> int(level);
				uint first_mip = level + 1u == count ? 0u : VSM_CLIPMAP_RESIDENT_MIP;
				for (uint mip = first_mip; mip < VSM_MIP_COUNT; mip++) {
					// The level texels the lookup reads at this mip: [origin * page size, mip window end).
					ivec2 window_start = ((origin >> int(mip)) << int(mip)) * int(VSM_PAGE_SIZE);
					ivec2 lo = max(level_min, origin * int(VSM_PAGE_SIZE));
					ivec2 hi = min(level_max, window_start + ivec2(int(VSM_VIRTUAL_SIZE) - 1));
					if (any(greaterThan(lo, hi))) {
						continue;
					}
					int shift = int(mip + VSM_PAGE_SIZE_SHIFT);
					uint mask = (VSM_PAGES_PER_SIDE >> mip) - 1u;
					// In the last level below VSM_CLIPMAP_RESIDENT_MIP, only pages outside mip + 1's window.
					ivec2 next_end = (origin >> int(mip + 1u)) * 2 + ivec2(int(VSM_PAGES_PER_SIDE >> mip));
					for (int y = lo.y >> shift; y <= (hi.y >> shift); y++) {
						for (int x = lo.x >> shift; x <= (hi.x >> shift); x++) {
							if (mip >= VSM_CLIPMAP_RESIDENT_MIP || x >= next_end.x || y >= next_end.y) {
								vsm_request(map, mip, uvec2(uint(x), uint(y)) & uvec2(mask), true);
							}
						}
					}
				}
			}
			continue;
		}
		uint local = l - view.data.counts.x;
		LightData light;
		float inv_radius;
		if (local < view.data.counts.y) {
			light = omni_lights.data[local];
			inv_radius = light.inv_radius;
		} else if (local < view.data.counts.y + view.data.counts.z) {
			light = spot_lights.data[local - view.data.counts.y];
			inv_radius = light.inv_radius;
		} else {
			light = area_lights.data[local - view.data.counts.y - view.data.counts.z];
			inv_radius = light.cone_attenuation;
		}
		if (light.vsm_map_base == VSM_MAP_NONE || !(inv_radius > 0.0)) {
			continue;
		}
		// The light's sphere must touch the box.
		vec3 light_world = vsm_view_to_world(light.position);
		float radius = 1.0 / inv_radius;
		float d2 = 0.0;
		for (int axis = 0; axis < 3; axis++) {
			float excess = max(max(box_min[axis] - light_world[axis], light_world[axis] - box_max[axis]), 0.0);
			d2 = d2 + excess * excess;
		}
		if (d2 > radius * radius) {
			continue;
		}
		uint count = (light.vsm_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
		for (uint face = 0u; face < count; face++) {
			uint map = vsm_light_map(light.vsm_map_base, light.vsm_flags, face);
			if (map >= view.data.misc.x) {
				continue;
			}
			VSMMapData map_data = vsm_maps.data[map];
			if (map_data.kind == VSM_MAP_KIND_NONE || map_data.kind == VSM_MAP_KIND_DIRECTIONAL) {
				continue;
			}
			uint outside[5] = uint[](0u, 0u, 0u, 0u, 0u);
			vec2 uv_min = vec2(1.0);
			vec2 uv_max = vec2(0.0);
			bool cut = false;
			for (uint c = 0u; c < 8u; c++) {
				vec3 corner = vec3((c & 1u) != 0u ? box_max.x : box_min.x, (c & 2u) != 0u ? box_max.y : box_min.y, (c & 4u) != 0u ? box_max.z : box_min.z);
				float x = vsm_row(map_data.row0, corner);
				float y = vsm_row(map_data.row1, corner);
				float w = vsm_row(map_data.row3, corner);
				outside[0] += (w - x < 0.0) ? 1u : 0u;
				outside[1] += (w + x < 0.0) ? 1u : 0u;
				outside[2] += (w - y < 0.0) ? 1u : 0u;
				outside[3] += (w + y < 0.0) ? 1u : 0u;
				outside[4] += (w - map_data.near_w < 0.0) ? 1u : 0u;
				if (!(w > map_data.near_w)) {
					cut = true;
					continue;
				}
				vec2 uv = vec2(x / w * 0.5 + 0.5, y / w * 0.5 + 0.5);
				uv_min = min(uv_min, uv);
				uv_max = max(uv_max, uv);
			}
			if (outside[0] == 8u || outside[1] == 8u || outside[2] == 8u || outside[3] == 8u || outside[4] == 8u) {
				continue;
			}
			uvec2 t0 = uvec2(0u);
			uvec2 t1 = uvec2(VSM_VIRTUAL_SIZE - 1u);
			if (!cut) {
				t0 = min(uvec2(clamp(uv_min, vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
				t1 = min(uvec2(clamp(uv_max, vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
			}
			for (uint mip = VSM_COARSE_FIRST_MIP; mip < VSM_MIP_COUNT; mip++) {
				uint shift = mip + VSM_PAGE_SIZE_SHIFT;
				for (uint y = t0.y >> shift; y <= (t1.y >> shift); y++) {
					for (uint x = t0.x >> shift; x <= (t1.x >> shift); x++) {
						vsm_request(map, mip, uvec2(x, y), true);
					}
				}
			}
		}
	}
}

#endif

#ifdef MODE_RESIDENT

void main() {
	uint l = gl_GlobalInvocationID.x;
	if (l < view.data.counts.x) {
		uvec4 light = view.data.directional[l];
		vsm_mark_resident(light.x, light.y);
		return;
	}
	uint local = l - view.data.counts.x;
	uint map_base;
	uint light_flags;
	if (local < view.data.counts.y) {
		map_base = omni_lights.data[local].vsm_map_base;
		light_flags = omni_lights.data[local].vsm_flags;
	} else if (local < view.data.counts.y + view.data.counts.z) {
		map_base = spot_lights.data[local - view.data.counts.y].vsm_map_base;
		light_flags = spot_lights.data[local - view.data.counts.y].vsm_flags;
	} else if (local < view.data.counts.y + view.data.counts.z + view.data.counts.w) {
		map_base = area_lights.data[local - view.data.counts.y - view.data.counts.z].vsm_map_base;
		light_flags = area_lights.data[local - view.data.counts.y - view.data.counts.z].vsm_flags;
	} else {
		return;
	}
	if (map_base != VSM_MAP_NONE) {
		vsm_mark_resident(map_base, light_flags);
	}
}

#endif
