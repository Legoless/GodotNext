#[compute]

#version 450

// GodotNext: vsm_non_nanite (#218, plan §21.3 contract S4, budget V3). Per-page GPU instance culling
// and indirect-count argument build for non-NextGeo shadow casters (regular meshes, Skeleton3D/blend
// shapes, MultiMesh 3D/2D, GPUParticles3D, alpha-tested and VERTEX-animated geometry).
//
// RendererVirtualShadowMapStorage::cull_box_dirty_pages() and sub_instance_box() are the CPU models;
// keep their arithmetic and operation order in step.

#VERSION_DEFINES

#ifdef MODE_SCAN
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

// The page table: a pair the full list cannot hold marks its page to be drawn again.
layout(set = 0, binding = 14, std430) restrict buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;

#include "vsm_mark_inc.glsl"
#include "vsm_page_table_inc.glsl"
#include "vsm_raster_inc.glsl"

#define VSM_CULL_STATE_TOTAL_PAIRS 0u
#define VSM_CULL_STATE_DROPPED_PAIRS 1u
#define VSM_CULL_STATE_SMALL_CULLED 2u
#define VSM_CULL_STATE_SINK 3u
#define VSM_CULL_STATE_REDRAW_MARKS 4u // Pairs the full list could not hold whose page was marked to be drawn again.
#define VSM_CULL_STATE_WRITE_MISMATCH 5u // Pairs the write pass emitted that its count pass did not, or the reverse.
// GodotNext: df_shadows (#214). Non-Nanite directional casters dropped beyond distance_field_shadow_start.
#define VSM_CULL_STATE_DF_FAR_CULLED 6u

#define VSM_CULL_FLAG_CAPTURE 1u // A raster check: write each pair's cull box.
#define VSM_SUB_FLAG_VIEW_FACING 1u // The shadow material writes MODELVIEW_MATRIX (billboards).

struct VSMBoxCullRecord {
	uvec4 meta0; // x: element_index, y: group_index, z: map_base, w: light_flags.
	uvec4 meta1; // x: is_dynamic (0/1), y: caster_id, z: 0, w: 0.
	vec4 box_min; // xyz: world AABB min, w: directional DF handoff start (>0 drops far non-Nanite casters, #214).
	vec4 box_max; // xyz: world AABB max.
};

struct VSMSubCullRecord {
	uvec4 meta0; // x: element_index, y: group_index, z: map_base, w: light_flags.
	uvec4 meta1; // x: is_dynamic (0/1), y: caster_id, z: sub_count, w: current_offset.
	uvec4 meta2; // x: stride_vec4, y: format_2d (0/1), z: trail_steps, w: VSM_SUB_FLAG_*.
	vec4 local_min; // xyz: local mesh AABB min (Mesh.custom_aabb when set), w: extra_cull_margin.
	vec4 local_max; // xyz: local mesh AABB max, w: the largest distance of a local AABB corner from the origin.
	vec4 model_row0;
	vec4 model_row1;
	vec4 model_row2;
	vec4 df_params; // GodotNext: df_shadows (#214). x: directional DF handoff start (>0 drops far non-Nanite casters).
};

layout(push_constant, std430) uniform Params {
	uint table_maps;
	uint pool_pages;
	uint group_count;
	uint pair_capacity;
	uint box_record_count;
	uint sub_record_index;
	float min_caster_texels;
	uint flags; // VSM_CULL_FLAG_*.
	vec4 camera_pos_unused; // GodotNext: df_shadows (#214). xyz: camera world position for directional DF handoff cull.
}
params;

layout(set = 0, binding = 0, std430) restrict readonly buffer VSMMapDataBuffer {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 1, std430) restrict readonly buffer VSMMapStateBuffer {
	uint data[];
}
vsm_map_state;

layout(set = 0, binding = 2, std430) restrict readonly buffer VSMDirtyBuffer {
	uvec4 data[];
}
vsm_dirty;

layout(set = 0, binding = 3, std430) restrict readonly buffer VSMAllocStateBuffer {
	uint data[];
}
vsm_alloc_state;

layout(set = 0, binding = 4, std430) restrict readonly buffer BoxRecords {
	VSMBoxCullRecord data[];
}
box_records;

layout(set = 0, binding = 5, std430) restrict readonly buffer SubRecords {
	VSMSubCullRecord data[];
}
sub_records;

layout(set = 0, binding = 6, std430) restrict readonly buffer GroupDrawMeta {
	uvec2 data[]; // x: index_or_vertex_count, y: indexed_flag.
}
group_draw_meta;

layout(set = 0, binding = 7, std430) restrict buffer GroupCounts {
	uint data[];
}
group_counts;

layout(set = 0, binding = 8, std430) restrict buffer GroupOffsets {
	uint data[];
}
group_offsets;

layout(set = 0, binding = 9, std430) restrict buffer GroupCursors {
	uint data[];
}
group_cursors;

layout(set = 0, binding = 10, std430) restrict writeonly buffer Pairs {
	uvec4 data[];
}
pairs;

layout(set = 0, binding = 11, std430) restrict buffer IndirectCommands {
	uint data[];
}
indirect_commands;

layout(set = 0, binding = 12, std430) restrict writeonly buffer IndirectCounts {
	uint data[];
}
indirect_counts;

layout(set = 0, binding = 13, std430) restrict buffer CullState {
	uint data[];
}
cull_state;

layout(set = 0, binding = 15, std430) restrict writeonly buffer PairBoxes {
	vec4 data[]; // Each pair's cull box (min, max), only while a raster check captures.
}
pair_boxes;

#if defined(MODE_COUNT_INSTANCES) || defined(MODE_WRITE_INSTANCES)
layout(set = 1, binding = 0, std430) restrict readonly buffer SubTransforms {
	vec4 data[];
}
sub_transforms;
#endif

// #277: D3D12 GPU-based validation flags discarded atomic return values.
void vsm_cull_sink(uint value) {
	if (value == 0xFFFFFFFFu) {
		cull_state.data[VSM_CULL_STATE_SINK] = 1u;
	}
}

uint vsm_map_state_idx(uint region, uint map) {
	return region * params.table_maps + map;
}

// Writes one pair into its group's range of the list. A pair the list cannot hold (the list is full, or
// the write pass found more pairs than the count pass) is not drawn; its page keeps its allocation but
// is marked scrolled, so the next view that requests it draws it again in full instead of caching it
// without this caster.
void vsm_write_pair(uint group_index, uint group_base, uint group_cap, uint element_index, uint sub_index, uint d, uvec4 rec, uint is_dynamic, uint caster_id, vec3 box_min, vec3 box_max) {
	uint local_slot = atomicAdd(group_cursors.data[group_index], 1u);
	if (local_slot < group_cap) {
		uint pair = group_base + local_slot;
		pairs.data[pair] = uvec4(element_index, sub_index, d | (is_dynamic << 31u), caster_id);
		if ((params.flags & VSM_CULL_FLAG_CAPTURE) != 0u) {
			pair_boxes.data[pair * 2u + 0u] = vec4(box_min, 0.0);
			pair_boxes.data[pair * 2u + 1u] = vec4(box_max, 0.0);
		}
		return;
	}
	uint map = rec.x;
	if (map >= params.table_maps) {
		return;
	}
	uint mip = rec.y & 0xFFu;
	uvec2 page = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	vsm_cull_sink(atomicOr(vsm_page_table.entries[vsm_entry_index(map, mip, page)], VSM_ENTRY_SCROLLED));
	vsm_cull_sink(atomicAdd(cull_state.data[VSM_CULL_STATE_REDRAW_MARKS], 1u));
}

void vsm_emit_or_count_box(uint element_index, uint sub_index, uint group_index, uint map_base, uint light_flags, uint is_dynamic, uint caster_id, vec3 box_min, vec3 box_max, float df_handoff_start, bool write_pairs) {
	uint dirty_limit = min(vsm_alloc_state.data[VSM_STATE_DIRTY], params.pool_pages);
	if (dirty_limit == 0u || map_base == VSM_MAP_NONE) {
		return;
	}
	if (!(box_max.x >= box_min.x && box_max.y >= box_min.y && box_max.z >= box_min.z)) {
		return;
	}
	// GodotNext: df_shadows (#214, plan §21.3 contract S7). Drop non-Nanite casters from directional maps
	// beyond distance_field_shadow_start when DF far-shadow handoff is active.
	if ((light_flags & VSM_LIGHT_FLAGS_KIND_MASK) == VSM_MAP_KIND_DIRECTIONAL && df_handoff_start > 0.0) {
		vec3 cam = params.camera_pos_unused.xyz;
		vec3 closest = clamp(cam, box_min, box_max);
		vec3 delta = closest - cam;
		if (dot(delta, delta) > df_handoff_start * df_handoff_start) {
			if (!write_pairs) {
				vsm_cull_sink(atomicAdd(cull_state.data[VSM_CULL_STATE_DF_FAR_CULLED], 1u));
			}
			return;
		}
	}
	float dx = box_max.x - box_min.x;
	float dy = box_max.y - box_min.y;
	float dz = box_max.z - box_min.z;
	float box_extent = max(max(dx, dy), dz);
	uint req_dirty = is_dynamic != 0u ? VSM_ENTRY_DIRTY_DYNAMIC : VSM_ENTRY_DIRTY_STATIC;
	uint map_count = (light_flags >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint group_base = write_pairs ? group_offsets.data[group_index] : 0u;
	uint group_cap = write_pairs ? min(group_counts.data[group_index], group_base < params.pair_capacity ? (params.pair_capacity - group_base) : 0u) : 0u;

	uint matched = 0u;
	uint small_culled = 0u;

	for (uint m_idx = 0u; m_idx < map_count; m_idx++) {
		uint map = vsm_raster_light_map(map_base, light_flags, m_idx);
		if (map >= params.table_maps) {
			continue;
		}
		uint d_offset = vsm_map_state.data[vsm_map_state_idx(VSM_MAP_STATE_DIRTY_OFFSET, map)];
		uint d_count = vsm_map_state.data[vsm_map_state_idx(VSM_MAP_STATE_DIRTY_COUNT, map)];
		if (d_count == 0u || d_offset >= dirty_limit) {
			continue;
		}
		uint d_end = min(d_offset + d_count, dirty_limit);
		VSMMapData md = vsm_maps.data[map];
		if (md.kind == VSM_MAP_KIND_NONE) {
			continue;
		}

		if (md.kind == VSM_MAP_KIND_DIRECTIONAL) {
			float min_tx = 1.0e30;
			float max_tx = -1.0e30;
			float min_ty = 1.0e30;
			float max_ty = -1.0e30;
			for (uint c = 0u; c < 8u; c++) {
				vec3 cp = vec3((c & 1u) != 0u ? box_max.x : box_min.x, (c & 2u) != 0u ? box_max.y : box_min.y, (c & 4u) != 0u ? box_max.z : box_min.z);
				float tx = vsm_row(md.row0, cp);
				float ty = vsm_row(md.row1, cp);
				if (c == 0u) {
					min_tx = tx;
					max_tx = tx;
					min_ty = ty;
					max_ty = ty;
				} else {
					min_tx = min(min_tx, tx);
					max_tx = max(max_tx, tx);
					min_ty = min(min_ty, ty);
					max_ty = max(max_ty, ty);
				}
			}
			min_tx = clamp(min_tx, -1073741824.0, 1073741824.0);
			max_tx = clamp(max_tx, -1073741824.0, 1073741824.0);
			min_ty = clamp(min_ty, -1073741824.0, 1073741824.0);
			max_ty = clamp(max_ty, -1073741824.0, 1073741824.0);
			ivec2 t_min = ivec2(floor(vec2(min_tx, min_ty)));
			ivec2 t_max = ivec2(floor(vec2(max_tx, max_ty)));
			ivec2 win_min = md.origin * int(VSM_PAGE_SIZE);
			ivec2 win_max = (md.origin + ivec2(int(VSM_PAGES_PER_SIDE))) * int(VSM_PAGE_SIZE) - ivec2(1);
			if (t_max.x < win_min.x || t_min.x > win_max.x || t_max.y < win_min.y || t_min.y > win_max.y) {
				continue;
			}
			for (uint d = d_offset; d < d_end; d++) {
				uvec4 rec = vsm_dirty.data[d];
				if ((rec.w & req_dirty) == 0u) {
					continue;
				}
				uint mip = rec.y & 0xFFu;
				if (mip >= VSM_MIP_COUNT) {
					continue;
				}
				uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
				uint mask = (VSM_PAGES_PER_SIDE >> mip) - 1u;
				ivec2 mip_origin = md.origin >> int(mip);
				ivec2 abs_p = mip_origin + ivec2((slot - uvec2(mip_origin)) & uvec2(mask));
				int page_texels = int(VSM_PAGE_SIZE << mip);
				ivec2 p_min = abs_p * page_texels;
				ivec2 p_max = p_min + ivec2(page_texels - 1);
				if (t_max.x < p_min.x || t_min.x > p_max.x || t_max.y < p_min.y || t_min.y > p_max.y) {
					continue;
				}
				float mip_texel = md.texel_scale * float(1u << mip);
				if (params.min_caster_texels > 0.0 && box_extent < params.min_caster_texels * mip_texel) {
					small_culled++;
					continue;
				}
				if (write_pairs) {
					vsm_write_pair(group_index, group_base, group_cap, element_index, sub_index, d, rec, is_dynamic, caster_id, box_min, box_max);
				} else {
					matched++;
				}
			}
		} else {
			float cx[8];
			float cy[8];
			float cw[8];
			uint out_near = 0u;
			uint out_far = 0u;
			uint out_left = 0u;
			uint out_right = 0u;
			uint out_bottom = 0u;
			uint out_top = 0u;
			float min_w = 0.0;
			for (uint c = 0u; c < 8u; c++) {
				vec3 cp = vec3((c & 1u) != 0u ? box_max.x : box_min.x, (c & 2u) != 0u ? box_max.y : box_min.y, (c & 4u) != 0u ? box_max.z : box_min.z);
				float x = vsm_row(md.row0, cp);
				float y = vsm_row(md.row1, cp);
				float w = vsm_row(md.row3, cp);
				cx[c] = x;
				cy[c] = y;
				cw[c] = w;
				min_w = c == 0u ? w : min(min_w, w);
				out_near += w < md.near_w ? 1u : 0u;
				out_far += w > md.max_distance ? 1u : 0u;
				out_left += x < -w ? 1u : 0u;
				out_right += x > w ? 1u : 0u;
				out_bottom += y < -w ? 1u : 0u;
				out_top += y > w ? 1u : 0u;
			}
			if (out_near == 8u || out_far == 8u || out_left == 8u || out_right == 8u || out_bottom == 8u || out_top == 8u) {
				continue;
			}
			uvec2 t_min = uvec2(0u);
			uvec2 t_max = uvec2(VSM_VIRTUAL_SIZE - 1u);
			if (min_w > md.near_w) {
				float min_u = 0.0;
				float max_u = 0.0;
				float min_v = 0.0;
				float max_v = 0.0;
				for (uint c = 0u; c < 8u; c++) {
					float u = (cx[c] / cw[c]) * 0.5 + 0.5;
					float v = (cy[c] / cw[c]) * 0.5 + 0.5;
					if (c == 0u) {
						min_u = u;
						max_u = u;
						min_v = v;
						max_v = v;
					} else {
						min_u = min(min_u, u);
						max_u = max(max_u, u);
						min_v = min(min_v, v);
						max_v = max(max_v, v);
					}
				}
				if (max_u < 0.0 || min_u > 1.0 || max_v < 0.0 || min_v > 1.0) {
					continue;
				}
				t_min = min(uvec2(clamp(vec2(min_u, min_v), vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
				t_max = min(uvec2(clamp(vec2(max_u, max_v), vec2(0.0), vec2(1.0)) * float(VSM_VIRTUAL_SIZE)), uvec2(VSM_VIRTUAL_SIZE - 1u));
			}
			float closest_w = max(min_w, md.near_w);
			for (uint d = d_offset; d < d_end; d++) {
				uvec4 rec = vsm_dirty.data[d];
				if ((rec.w & req_dirty) == 0u) {
					continue;
				}
				uint mip = rec.y & 0xFFu;
				if (mip >= VSM_MIP_COUNT) {
					continue;
				}
				uvec2 page = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
				uvec2 bp_min = (t_min >> mip) >> VSM_PAGE_SIZE_SHIFT;
				uvec2 bp_max = (t_max >> mip) >> VSM_PAGE_SIZE_SHIFT;
				if (page.x < bp_min.x || page.x > bp_max.x || page.y < bp_min.y || page.y > bp_max.y) {
					continue;
				}
				float mip_texel = (md.texel_scale * closest_w) * float(1u << mip);
				if (params.min_caster_texels > 0.0 && box_extent < params.min_caster_texels * mip_texel) {
					small_culled++;
					continue;
				}
				if (write_pairs) {
					vsm_write_pair(group_index, group_base, group_cap, element_index, sub_index, d, rec, is_dynamic, caster_id, box_min, box_max);
				} else {
					matched++;
				}
			}
		}
	}

	if (!write_pairs) {
		if (matched > 0u) {
			vsm_cull_sink(atomicAdd(group_counts.data[group_index], matched));
		}
		if (small_culled > 0u) {
			vsm_cull_sink(atomicAdd(cull_state.data[VSM_CULL_STATE_SMALL_CULLED], small_culled));
		}
	}
}

#if defined(MODE_COUNT_INSTANCES) || defined(MODE_WRITE_INSTANCES)
bool vsm_sub_finite(vec4 r) {
	return (r.x == r.x) && abs(r.x) < 1.0e30 && (r.y == r.y) && abs(r.y) < 1.0e30 && (r.z == r.z) && abs(r.z) < 1.0e30 && (r.w == r.w) && abs(r.w) < 1.0e30;
}

bool vsm_sub_instance_box(VSMSubCullRecord rec, uint sub_index, out vec3 r_min, out vec3 r_max) {
	r_min = vec3(1.0e30);
	r_max = vec3(-1.0e30);
	uint trail_steps = rec.meta2.z;
	uint stride_vec4 = rec.meta2.x;
	bool format_2d = rec.meta2.y != 0u;
	uint steps = max(trail_steps, 1u);
	if (trail_steps <= 1u && stride_vec4 == 0u) {
		return false;
	}
	vec3 lmin = rec.local_min.xyz;
	vec3 lmax = rec.local_max.xyz;
	bool view_facing = (rec.meta2.w & VSM_SUB_FLAG_VIEW_FACING) != 0u;
	bool any_valid = false;
	for (uint seg = 0u; seg < steps; seg++) {
		uint ofs = trail_steps > 1u ? ((trail_steps * sub_index + seg) * 5u) : (stride_vec4 * (sub_index + rec.meta1.w));
		vec4 r0 = sub_transforms.data[ofs + 0u];
		vec4 r1 = sub_transforms.data[ofs + 1u];
		vec4 r2 = (trail_steps <= 1u && format_2d) ? vec4(0.0, 0.0, 1.0, 0.0) : sub_transforms.data[ofs + 2u];
		if (!vsm_sub_finite(r0) || !vsm_sub_finite(r1) || !vsm_sub_finite(r2)) {
			continue;
		}
		float basis_sq = ((r0.x * r0.x + r0.y * r0.y) + r0.z * r0.z) + ((r1.x * r1.x + r1.y * r1.y) + r1.z * r1.z) + ((r2.x * r2.x + r2.y * r2.y) + r2.z * r2.z);
		if (!(basis_sq > 0.0)) {
			continue;
		}
		for (uint c = 0u; c < 8u; c++) {
			vec3 lp = vec3((c & 1u) != 0u ? lmax.x : lmin.x, (c & 2u) != 0u ? lmax.y : lmin.y, (c & 4u) != 0u ? lmax.z : lmin.z);
			vec3 sp = vec3(vsm_row(r0, lp), vsm_row(r1, lp), vsm_row(r2, lp));
			vec3 wp = vec3(vsm_row(rec.model_row0, sp), vsm_row(rec.model_row1, sp), vsm_row(rec.model_row2, sp));
			if (!any_valid) {
				r_min = wp;
				r_max = wp;
				any_valid = true;
			} else {
				r_min = min(r_min, wp);
				r_max = max(r_max, wp);
			}
		}
		if (view_facing) {
			// A billboard keeps the instance's world origin (MODEL_MATRIX[3]) and turns the mesh to face the
			// camera, scaled by 1 or, with Keep Scale, by the lengths of MODEL_MATRIX's axes: the mesh stays
			// within the largest corner distance times max(1, the longest axis) of that origin.
			vec3 origin = vec3(vsm_row(rec.model_row0, vec3(r0.w, r1.w, r2.w)), vsm_row(rec.model_row1, vec3(r0.w, r1.w, r2.w)), vsm_row(rec.model_row2, vec3(r0.w, r1.w, r2.w)));
			vec3 axis_len_sq = vec3(0.0);
			for (uint k = 0u; k < 3u; k++) {
				vec3 inst_axis = vec3(r0[k], r1[k], r2[k]);
				vec3 world_axis = vec3(dot(rec.model_row0.xyz, inst_axis), dot(rec.model_row1.xyz, inst_axis), dot(rec.model_row2.xyz, inst_axis));
				axis_len_sq[k] = dot(world_axis, world_axis);
			}
			float scale = max(1.0, sqrt(max(max(axis_len_sq.x, axis_len_sq.y), axis_len_sq.z)));
			float reach = rec.local_max.w * scale;
			r_min = min(r_min, origin - vec3(reach));
			r_max = max(r_max, origin + vec3(reach));
		}
	}
	if (!any_valid) {
		return false;
	}
	float margin = max(rec.local_min.w, 0.0);
	r_min -= vec3(margin);
	r_max += vec3(margin);
	return true;
}
#endif

#ifdef MODE_CLEAR
void main() {
	uint g = gl_GlobalInvocationID.x;
	if (g < params.group_count) {
		group_counts.data[g] = 0u;
		group_cursors.data[g] = 0u;
	}
	if (g == 0u) {
		cull_state.data[VSM_CULL_STATE_TOTAL_PAIRS] = 0u;
		cull_state.data[VSM_CULL_STATE_DROPPED_PAIRS] = 0u;
		cull_state.data[VSM_CULL_STATE_SMALL_CULLED] = 0u;
		cull_state.data[VSM_CULL_STATE_SINK] = 0u;
		cull_state.data[VSM_CULL_STATE_REDRAW_MARKS] = 0u;
		cull_state.data[VSM_CULL_STATE_WRITE_MISMATCH] = 0u;
		cull_state.data[VSM_CULL_STATE_DF_FAR_CULLED] = 0u;
	}
}
#endif

#ifdef MODE_COUNT_BOXES
void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.box_record_count) {
		return;
	}
	VSMBoxCullRecord rec = box_records.data[i];
	vsm_emit_or_count_box(rec.meta0.x, 0u, rec.meta0.y, rec.meta0.z, rec.meta0.w, rec.meta1.x, rec.meta1.y, rec.box_min.xyz, rec.box_max.xyz, rec.box_min.w, false);
}
#endif

#ifdef MODE_COUNT_INSTANCES
void main() {
	VSMSubCullRecord rec = sub_records.data[params.sub_record_index];
	uint sub = gl_GlobalInvocationID.x;
	if (sub >= rec.meta1.z) {
		return;
	}
	vec3 bmin;
	vec3 bmax;
	if (!vsm_sub_instance_box(rec, sub, bmin, bmax)) {
		return;
	}
	vsm_emit_or_count_box(rec.meta0.x, sub, rec.meta0.y, rec.meta0.z, rec.meta0.w, rec.meta1.x, rec.meta1.y, bmin, bmax, rec.df_params.x, false);
}
#endif

#ifdef MODE_SCAN
shared uint shared_scan[256];

void main() {
	uint id = gl_LocalInvocationID.x;
	uint running = 0u;
	for (uint base = 0u; base < params.group_count; base += 256u) {
		uint g = base + id;
		uint raw = g < params.group_count ? group_counts.data[g] : 0u;
		shared_scan[id] = raw;
		barrier();
		for (uint step = 1u; step < 256u; step <<= 1u) {
			uint add = id >= step ? shared_scan[id - step] : 0u;
			barrier();
			shared_scan[id] += add;
			barrier();
		}
		uint excl = running + (shared_scan[id] - raw);
		if (g < params.group_count) {
			uint clamped_offset = min(excl, params.pair_capacity);
			uint avail = excl < params.pair_capacity ? (params.pair_capacity - excl) : 0u;
			uint clamped_count = min(raw, avail);
			group_offsets.data[g] = clamped_offset;
			uvec2 meta = group_draw_meta.data[g];
			uint cmd = g * 5u;
			indirect_commands.data[cmd + 0u] = meta.x;
			indirect_commands.data[cmd + 1u] = clamped_count;
			indirect_commands.data[cmd + 2u] = 0u;
			indirect_commands.data[cmd + 3u] = 0u;
			indirect_commands.data[cmd + 4u] = 0u;
			indirect_counts.data[g] = clamped_count > 0u ? 1u : 0u;
		}
		running += shared_scan[255];
		barrier();
	}
	if (id == 0u) {
		cull_state.data[VSM_CULL_STATE_TOTAL_PAIRS] = running;
		cull_state.data[VSM_CULL_STATE_DROPPED_PAIRS] = running > params.pair_capacity ? (running - params.pair_capacity) : 0u;
	}
}
#endif

#ifdef MODE_FIXUP
// After the write pass: each group draws only the pairs its write pass wrote. The count and write passes
// run the same cull in separate pipelines; if a compiler made them disagree, the draw would otherwise read
// pairs left over from an earlier view.
void main() {
	uint g = gl_GlobalInvocationID.x;
	if (g >= params.group_count) {
		return;
	}
	uint counted = group_counts.data[g];
	uint written = group_cursors.data[g];
	uint drawn = min(indirect_commands.data[g * 5u + 1u], written);
	indirect_commands.data[g * 5u + 1u] = drawn;
	indirect_counts.data[g] = drawn > 0u ? 1u : 0u;
	if (written != counted) {
		vsm_cull_sink(atomicAdd(cull_state.data[VSM_CULL_STATE_WRITE_MISMATCH], written > counted ? written - counted : counted - written));
	}
}
#endif

#ifdef MODE_WRITE_BOXES
void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.box_record_count) {
		return;
	}
	VSMBoxCullRecord rec = box_records.data[i];
	vsm_emit_or_count_box(rec.meta0.x, 0u, rec.meta0.y, rec.meta0.z, rec.meta0.w, rec.meta1.x, rec.meta1.y, rec.box_min.xyz, rec.box_max.xyz, rec.box_min.w, true);
}
#endif

#ifdef MODE_WRITE_INSTANCES
void main() {
	VSMSubCullRecord rec = sub_records.data[params.sub_record_index];
	uint sub = gl_GlobalInvocationID.x;
	if (sub >= rec.meta1.z) {
		return;
	}
	vec3 bmin;
	vec3 bmax;
	if (!vsm_sub_instance_box(rec, sub, bmin, bmax)) {
		return;
	}
	vsm_emit_or_count_box(rec.meta0.x, sub, rec.meta0.y, rec.meta0.z, rec.meta0.w, rec.meta1.x, rec.meta1.y, bmin, bmax, rec.df_params.x, true);
}
#endif
