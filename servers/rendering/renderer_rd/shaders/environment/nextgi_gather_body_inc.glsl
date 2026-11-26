// GodotNext: ps_swrt_trace (#186). Not a standalone shader: the gather, integrate, SRC feedback,
// coverage and reflect kernels, shared by the hardware gather and the software sibling. Only
// the trace (nextgi_trace) and the hit shading differ between the tiers.

#ifndef NEXTGI_GATHER_BODY_INC_GLSL
#define NEXTGI_GATHER_BODY_INC_GLSL

#include "../oct_inc.glsl"

// D6 helpers. They stay in this shader: the RD header builder pastes an include once, and these
// functions need the buffers declared above. nextgi_trace stays in nextgi_rt_inc.glsl.
// GodotNext: ps_swrt_trace (#186). The arena and the D6 triangle lookup are the hardware tier's.
#ifndef NEXTGI_TRACE_SDF
uint nextgi_chunk_uint(uint chunk, uint byte_offset) {
	uint word = byte_offset >> 2u;
	if (chunk == 0u) {
		return chunk0.data[word];
	}
	if (chunk == 1u) {
		return chunk1.data[word];
	}
	if (chunk == 2u) {
		return chunk2.data[word];
	}
	return chunk3.data[word];
}

vec3 nextgi_chunk_vec3(uint chunk, uint byte_offset) {
	return vec3(uintBitsToFloat(nextgi_chunk_uint(chunk, byte_offset)), uintBitsToFloat(nextgi_chunk_uint(chunk, byte_offset + 4u)), uintBitsToFloat(nextgi_chunk_uint(chunk, byte_offset + 8u)));
}

// Arena float3 positions and uint32 indices. This is not a ray-query position fetch.
bool nextgi_hit_object_pos_normal(NextGIHit hit, out vec3 object_pos, out vec3 face_normal) {
	if (!hit.hit || hit.custom_index >= params.record_count) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (hit.geometry_index >= rec.surface_count) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	uint index_byte = surface.index_offset + hit.primitive * 12u;
	uint i0 = nextgi_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i0 * 12u);
	vec3 p1 = nextgi_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i1 * 12u);
	vec3 p2 = nextgi_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i2 * 12u);
	object_pos = p0 * (1.0 - hit.bary.x - hit.bary.y) + p1 * hit.bary.x + p2 * hit.bary.y;
	face_normal = normalize(cross(p1 - p0, p2 - p0));
	if (!hit.front_face) {
		face_normal = -face_normal;
	}
	return true;
}

// GodotNext: nextgi_skinned_actors (#59). D6 samples a skinned surface's bind-pose cards at
// the posed hit's triangle and barycentrics; bit 0 of flags marks the posed slot and bits
// 8-31 hold the bind-pose vertex offset in 16-byte units.
bool nextgi_hit_bind_pose_pos_normal(NextGIHit hit, out vec3 object_pos, out vec3 face_normal) {
	if (!hit.hit || hit.custom_index >= params.record_count) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (hit.geometry_index >= rec.surface_count) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	uint vertex_base = surface.vertex_offset;
	if ((surface.flags & 1u) != 0u) {
		vertex_base = (surface.flags >> 8u) * 16u;
	}
	uint index_byte = surface.index_offset + hit.primitive * 12u;
	uint i0 = nextgi_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_chunk_vec3(rec.arena_chunk, vertex_base + i0 * 12u);
	vec3 p1 = nextgi_chunk_vec3(rec.arena_chunk, vertex_base + i1 * 12u);
	vec3 p2 = nextgi_chunk_vec3(rec.arena_chunk, vertex_base + i2 * 12u);
	object_pos = p0 * (1.0 - hit.bary.x - hit.bary.y) + p1 * hit.bary.x + p2 * hit.bary.y;
	face_normal = normalize(cross(p1 - p0, p2 - p0));
	if (!hit.front_face) {
		face_normal = -face_normal;
	}
	return true;
}

// D6. uncovered is set when only the best-axis card, without the depth test, qualified.
bool nextgi_src_lookup(NextGIHit hit, out vec3 radiance, out bool uncovered) {
	radiance = vec3(0.0);
	uncovered = false;
	vec3 object_pos;
	vec3 face_normal;
	if (!nextgi_hit_bind_pose_pos_normal(hit, object_pos, face_normal)) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (rec.card_set == 0xFFFFFFFFu || rec.page_table == 0xFFFFFFFFu) {
		return false;
	}
	// Bit 31 marks a page that was evicted. D6 treats that as uncovered until it is resident again.
	if ((rec.page_table & 0x80000000u) != 0u) {
		uncovered = true;
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	if ((surface.flags & 4u) != 0u) {
		// GodotNext: nextgi_nextgeo (#77). Outside-mask surface on a Stage-2 mesh has no cards; fall back to WRC.
		return false;
	}
	NextGICardSet card_set = card_sets.data[rec.card_set];
	if ((surface.flags & 2u) != 0u) {
		// GodotNext: nextgi_nextgeo (#77). Stage-2 cards keyed by proxy-cut cluster group + dominant axis.
		uint group_byte = (surface.flags & ~15u) + hit.primitive * 2u;
		uint word = nextgi_chunk_uint(rec.arena_chunk, group_byte & ~3u);
		uint hit_group = ((group_byte & 2u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
		uint target_tag = hit_group + 1u;
		uint lo = 0u;
		uint hi = card_set.card_count;
		for (uint bstep = 0u; bstep < 20u && lo < hi; bstep++) {
			uint mid = (lo + hi) >> 1u;
			uint mid_tag = uint(cards.data[card_set.first_card + mid].origin.w + 0.5);
			if (mid_tag < target_tag) {
				lo = mid + 1u;
			} else {
				hi = mid;
			}
		}
		float best_align = 0.0;
		vec3 best = vec3(0.0);
		bool found = false;
		for (uint k = 0u; k < 6u && (lo + k) < card_set.card_count; k++) {
			uint i = lo + k;
			NextGICard card = cards.data[card_set.first_card + i];
			if (uint(card.origin.w + 0.5) != target_tag) {
				break;
			}
			float align = dot(face_normal, card.axis_n.xyz);
			if (align <= best_align) {
				continue;
			}
			vec3 rel = object_pos - card.origin.xyz;
			float u = dot(rel, card.axis_u.xyz);
			float v = dot(rel, card.axis_v.xyz);
			if (abs(u) > card.axis_u.w + 1e-4 || abs(v) > card.axis_v.w + 1e-4) {
				continue;
			}
			float depth = -dot(rel, card.axis_n.xyz);
			vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
			vec2 texel = vec2((u / card.axis_u.w) * 0.5 + 0.5, (v / card.axis_v.w) * 0.5 + 0.5) * card_size;
			ivec2 material = ivec2(card.atlas_rect.xy) + ivec2(clamp(texel, vec2(0.0), card_size - 1.0));
			float card_depth = texelFetch(material_depth_tex, material, 0).r;
			float depth_tol = 2.0 * max(card_set.texel_size, max(2.0 * card.axis_u.w / card_size.x, 2.0 * card.axis_v.w / card_size.y));
			if (!(card_depth > 0.0 && abs(card_depth - depth) <= depth_tol)) {
				continue;
			}
			uint page_index = rec.page_table + i;
			if (page_index >= params.page_rect_count) {
				continue;
			}
			uvec2 page = page_rects.data[page_index];
			uvec2 page_origin = uvec2(page.x & 0xFFFFu, page.x >> 16u);
			uvec2 page_size = max(uvec2(page.y & 0xFFFFu, page.y >> 16u), uvec2(1u));
			ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - 1.0));
			best_align = align;
			best = texelFetch(radiance_tex, radiance_coord, 0).rgb;
			found = true;
		}
		if (found) {
			radiance = best;
			uncovered = false;
			return true;
		}
		return false;
	}
	float best_align = -2.0;
	float fallback_align = -2.0;
	vec3 best = vec3(0.0);
	vec3 fallback = vec3(0.0);
	bool found = false;
	bool found_fallback = false;
	for (uint i = 0u; i < card_set.card_count; i++) {
		NextGICard card = cards.data[card_set.first_card + i];
		vec3 rel = object_pos - card.origin.xyz;
		float u = dot(rel, card.axis_u.xyz);
		float v = dot(rel, card.axis_v.xyz);
		float depth = -dot(rel, card.axis_n.xyz);
		if (abs(u) > card.axis_u.w + 1e-4 || abs(v) > card.axis_v.w + 1e-4) {
			continue;
		}
		float align = dot(face_normal, card.axis_n.xyz);
		vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
		vec2 texel = vec2((u / card.axis_u.w) * 0.5 + 0.5, (v / card.axis_v.w) * 0.5 + 0.5) * card_size;
		ivec2 material = ivec2(card.atlas_rect.xy) + ivec2(clamp(texel, vec2(0.0), card_size - 1.0));
		float card_depth = texelFetch(material_depth_tex, material, 0).r;
		uint page_index = rec.page_table + i;
		if (page_index >= params.page_rect_count) {
			continue;
		}
		uvec2 page = page_rects.data[page_index];
		uvec2 page_origin = uvec2(page.x & 0xFFFFu, page.x >> 16u);
		uvec2 page_size = max(uvec2(page.y & 0xFFFFu, page.y >> 16u), uvec2(1u));
		ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - 1.0));
		vec3 sample_radiance = texelFetch(radiance_tex, radiance_coord, 0).rgb;
		if (align > fallback_align) {
			fallback_align = align;
			fallback = sample_radiance;
			found_fallback = true;
		}
		float depth_tol = 2.0 * max(card_set.texel_size, max(2.0 * card.axis_u.w / card_size.x, 2.0 * card.axis_v.w / card_size.y));
		if (card_depth > 0.0 && abs(card_depth - depth) <= depth_tol && align > best_align) {
			best_align = align;
			best = sample_radiance;
			found = true;
		}
	}
	if (found) {
		radiance = best;
		uncovered = false;
		return true;
	}
	return false;
}
#endif // !NEXTGI_TRACE_SDF

const float PI = 3.14159265;

float nextgi_radical_inverse(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec3 nextgi_unproject(vec2 pixel, float depth) {
	vec2 uv = (pixel + 0.5) / params.screen_jitter.xy;
	return nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
}

vec3 nextgi_geometric_normal(ivec2 pixel, float depth, vec3 world) {
	ivec2 size = ivec2(params.screen_jitter.xy);
	ivec2 px_p = clamp(pixel + ivec2(1, 0), ivec2(0), size - 1);
	ivec2 px_m = clamp(pixel - ivec2(1, 0), ivec2(0), size - 1);
	ivec2 py_p = clamp(pixel + ivec2(0, 1), ivec2(0), size - 1);
	ivec2 py_m = clamp(pixel - ivec2(0, 1), ivec2(0), size - 1);
	float dx_p = texelFetch(depth_tex, px_p, 0).r;
	float dx_m = texelFetch(depth_tex, px_m, 0).r;
	float dy_p = texelFetch(depth_tex, py_p, 0).r;
	float dy_m = texelFetch(depth_tex, py_m, 0).r;
	bool use_xm = (dx_m > 0.0) && (dx_p <= 0.0 || abs(dx_m - depth) < abs(dx_p - depth));
	bool use_ym = (dy_m > 0.0) && (dy_p <= 0.0 || abs(dy_m - depth) < abs(dy_p - depth));
	vec3 tx = use_xm ? (world - nextgi_unproject(vec2(px_m), dx_m)) : (nextgi_unproject(vec2(px_p), dx_p) - world);
	vec3 ty = use_ym ? (world - nextgi_unproject(vec2(py_m), dy_m)) : (nextgi_unproject(vec2(py_p), dy_p) - world);
	vec3 cross_n = cross(tx, ty);
	vec3 to_cam = params.camera_pos.xyz - world;
	vec3 normal = dot(cross_n, cross_n) > 1e-12 ? normalize(cross_n) : (dot(to_cam, to_cam) > 1e-12 ? normalize(to_cam) : vec3(0.0, 1.0, 0.0));
	if (dot(normal, to_cam) < 0.0) {
		normal = -normal;
	}
	return normal;
}

vec3 nextgi_surface_normal(ivec2 pixel, float depth, vec3 world) {
	if (textureSize(normal_roughness_tex, 0) == ivec2(params.screen_jitter.xy)) {
		vec3 encoded = texelFetch(normal_roughness_tex, pixel, 0).xyz;
		vec3 n = nextgi_view_world_normal(mat3(params.view_to_world[0].xyz, params.view_to_world[1].xyz, params.view_to_world[2].xyz), encoded);
		if (dot(n, n) > 1e-6) {
			return n;
		}
	}
	return nextgi_geometric_normal(pixel, depth, world);
}

vec3 nextgi_orient(vec3 normal, vec3 local_dir) {
	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	return normalize(tangent * local_dir.x + bitangent * local_dir.y + normal * local_dir.z);
}

vec3 nextgi_miss_radiance(vec3 direction) {
	if (params.sky_enabled != 0u) {
		return textureLod(sky_tex, vec3_to_oct(direction), 0.0).rgb * params.ambient_color.a;
	}
	return params.ambient_color.rgb;
}

#ifdef NEXTGI_TRACE_SDF
// GodotNext: ps_swrt_trace (#186). The software tier's hit shading (nextgi_sdf_hit_inc.glsl keeps
// the trace's origin and normal). A detail (mesh-SDF) hit knows its instance, whose record (the
// SWRT record pass: record i is SWRT instance i) holds its page table and card set: D6 in the
// instance's local space first (#186 step 3). Then voxel lighting, then the WRC at the hit, then
// the miss radiance: the hardware tier's D6-miss chain.
vec3 nextgi_shade_hit(NextGIHit hit, vec3 ray_dir, inout uint uncovered_count, out float confidence) {
	confidence = 0.0;
	vec3 world = nextgi_sdf_origin + ray_dir * hit.t;
	vec3 normal = nextgi_sdf_last.normal;
	if (hit.custom_index < params.record_count) {
		NextGIInstanceRecord rec = records.data[hit.custom_index];
		// GodotNext: swrt_detail_refine (#250). The detail march stops anywhere in the surface band
		// (half a mesh-SDF voxel, 5 cm at 10 voxels per meter), but D6 accepts a card texel only
		// within CARD_DEPTH_TEXELS card texels of its captured depth: millimeters on a small prop.
		// Two steps along the hit normal by the instance's own distance bring the lookup to the
		// zero crossing; each is clamped to the hit mip's voxel. Voxel lighting, which shares the
		// lookup, stores hits already at the crossing.
		vec3 surface = world;
		vec3 step_normal = dot(normal, normal) > 1e-12 ? normalize(normal) : -ray_dir;
		for (uint refine = 0u; refine < 2u; refine++) {
			float detail_voxel;
			float crossing = swrt_instance_distance(hit.custom_index, surface, detail_voxel);
			surface -= step_normal * clamp(crossing, -detail_voxel, detail_voxel);
		}
		vec3 local = swrt_instance_local(swrt_instances.data[hit.custom_index], surface);
		// A normal goes to object space through the transpose of the object-to-world basis.
		vec3 local_normal = vec3(
				rec.object_to_world[0] * normal.x + rec.object_to_world[4] * normal.y + rec.object_to_world[8] * normal.z,
				rec.object_to_world[1] * normal.x + rec.object_to_world[5] * normal.y + rec.object_to_world[9] * normal.z,
				rec.object_to_world[2] * normal.x + rec.object_to_world[6] * normal.y + rec.object_to_world[10] * normal.z);
		vec3 resolved;
		float projection;
		if (dot(local_normal, local_normal) > 1e-12 && resample_src(local, normalize(local_normal), rec.page_table, rec.card_set, resolved, projection)) {
			confidence = 1.0;
			nextgi_sdf_note_d6();
			return max(resolved, vec3(0.0));
		}
	}
	vec3 cached = nextgi_wrc_sample(world, -ray_dir);
	bool wrc_hit = nextgi_wrc_hit_cache;
	float wrc_confidence = nextgi_wrc_confidence;
	if (SWRT_P.flags.x > 0.5) {
		NextGIVoxelClipmap clipmap;
		clipmap.origin_size[0] = SWRT_P.clipmap[0];
		clipmap.origin_size[1] = SWRT_P.clipmap[1];
		clipmap.origin_size[2] = SWRT_P.clipmap[2];
		clipmap.origin_size[3] = SWRT_P.clipmap[3];
		float weight;
		vec3 voxel = nextgi_voxel_lighting_lookup(world, normal, cached, clipmap, weight);
		if (weight >= NEXTGI_VOXEL_WEIGHT_FALLBACK) {
			confidence = 1.0;
			return max(voxel, vec3(0.0));
		}
	}
	if (wrc_hit) {
		confidence = wrc_confidence;
		uncovered_count++;
		return cached;
	}
	// A ray that hit solid geometry must never leak open-sky radiance through the hit surface.
	return vec3(0.0);
}
#else
vec3 nextgi_shade_hit(NextGIHit hit, vec3 ray_dir, inout uint uncovered_count, out float confidence) {
	confidence = 0.0;
#ifdef MODE_HIT_RADIANCE_REFERENCE
	// 16 estimator samples at the hit. Misses stay on the sky path in the caller.
	vec3 object_pos;
	vec3 face_normal;
	if (!nextgi_hit_object_pos_normal(hit, object_pos, face_normal) || hit.custom_index >= params.record_count) {
		return nextgi_miss_radiance(ray_dir);
	}
	confidence = 1.0;
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	vec3 world = nextgi_pt_mul_point(rec, object_pos);
	vec3 face = nextgi_pt_mul_normal(rec, face_normal);
	vec3 accum = vec3(0.0);
	uint rng = nextgi_pt_hash(hit.custom_index ^ params.frame ^ ref_params.counts.x);
	for (uint s = 0u; s < 16u; s++) {
		accum += nextgi_pt_radiance(world + face * rec.origin_offset, -ray_dir, rng);
	}
	return accum / 16.0;
#elif defined(MODE_WORLD_CACHE)
	// GodotNext: nextgi_world_cache (#294). Sample the sparse world-space cache at the hit instead of SRC.
	vec3 object_pos;
	vec3 face_normal;
	if (nextgi_hit_object_pos_normal(hit, object_pos, face_normal) && hit.custom_index < params.record_count) {
		NextGIInstanceRecord rec = records.data[hit.custom_index];
		vec3 world = vec3(
				dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), object_pos) + rec.object_to_world[3],
				dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), object_pos) + rec.object_to_world[7],
				dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), object_pos) + rec.object_to_world[11]);
		vec3 world_normal = normalize(nextgi_record_normal_to_world(rec, face_normal));
		if (dot(world_normal, -ray_dir) < 0.0) {
			world_normal = -world_normal;
		}
		vec3 wc_rad;
		if (nextgi_wc_lookup(world, world_normal, params.camera_pos.xyz, wc_rad)) {
			confidence = 1.0;
			return wc_rad;
		}
		vec3 cached = nextgi_wrc_sample(world, -ray_dir);
		if (nextgi_wrc_hit_cache) {
			confidence = nextgi_wrc_confidence;
			uncovered_count++;
			return cached;
		}
	}
	return vec3(0.0);
#else
	vec3 radiance;
	bool uncovered = false;
	if (!nextgi_src_lookup(hit, radiance, uncovered)) {
		vec3 missed_pos;
		vec3 missed_normal;
		if (nextgi_hit_object_pos_normal(hit, missed_pos, missed_normal) && hit.custom_index < params.record_count) {
			NextGIInstanceRecord rec = records.data[hit.custom_index];
			vec3 world = vec3(
					dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), missed_pos) + rec.object_to_world[3],
					dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), missed_pos) + rec.object_to_world[7],
					dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), missed_pos) + rec.object_to_world[11]);
			vec3 cached = nextgi_wrc_sample(world, -ray_dir);
			if (nextgi_wrc_hit_cache) {
				confidence = nextgi_wrc_confidence;
				uncovered_count++;
				return cached;
			}
		}
		// A ray that hit solid geometry must never fall back to open-sky radiance.
		return vec3(0.0);
	}
	confidence = 1.0;
	if (uncovered) {
		uncovered_count++;
	}
	return radiance;
#endif
}
#endif // NEXTGI_TRACE_SDF

// GodotNext: nextgi_view_math. The Hi-Z march is shared with the software tier (#186).
#ifndef NEXTGI_SCREEN_HIZ_TEX
#define NEXTGI_SCREEN_HIZ_TEX hiz_tex
#define NEXTGI_SCREEN_LAST_FRAME_TEX last_frame_tex
#endif
#include "nextgi_screen_trace_inc.glsl"

bool nextgi_screen_trace(vec3 origin, vec3 direction, float tmax, out vec3 color, out vec3 march_end) {
	color = vec3(0.0);
	march_end = origin;
	if (params.pad0 == 0u) {
		return false;
	}
	return nextgi_screen_trace_hiz(params.view_proj, params.inv_view_proj, params.camera_pos.xyz, params.screen_jitter.xy, float(params.pad1), origin, direction, tmax, color, march_end);
}

// GodotNext: ps_swrt_trace (#186 step 4). A screen miss that left the screen or passed
// behind a surface resumes at march_end (the last sample before it), pulled back toward
// the origin, on both tiers. Any other miss leaves march_end on the origin, so the world
// trace starts there. On the software tier the pull-back is at least one voxel of the
// SDF level at march_end.
float nextgi_screen_resume_tier(vec3 origin, vec3 direction, float marched, float world_tmax, out vec3 trace_origin, out float trace_tmax) {
	float clearance = 0.0;
#ifdef NEXTGI_TRACE_SDF
	clearance = nextgi_sdf_voxel_at(origin + direction * marched);
#endif
	return nextgi_screen_resume(origin, direction, marched, world_tmax, clearance, trace_origin, trace_tmax);
}

#ifdef MODE_ANCHOR
// Pre-pass (1 thread per probe, HWRT only): computes the per-probe origin_offset via one anchor ray
// and stores it in confidence_data (-1.0 for background probes) so the 1-thread-per-ray gather never
// repeats the anchor ray 16x/64x per probe.
void nextgi_anchor_main() {
#ifndef NEXTGI_TRACE_SDF
	uvec2 probe = gl_GlobalInvocationID.xy;
	if (probe.x >= params.probes_x || probe.y >= params.probes_y) {
		return;
	}
	uint probe_idx = probe.y * params.probes_x + probe.x;
	ivec2 ipixel = clamp(ivec2(probe) * 16 + ivec2(8), ivec2(0), ivec2(params.screen_jitter.xy) - ivec2(1));
	float depth = texelFetch(depth_tex, ipixel, 0).r;
	if (depth <= 0.0) {
		confidence_data.data[probe_idx] = -1.0;
		return;
	}
	vec3 world = nextgi_unproject(vec2(ipixel), depth);
	vec3 to_camera = normalize(params.camera_pos.xyz - world);
	float anchor_t = length(params.camera_pos.xyz - world);
	NextGIHit anchor;
	float origin_offset = params.max_origin_offset;
	if (nextgi_trace(params.camera_pos.xyz, -to_camera, anchor_t + 0.05, anchor)) {
		origin_offset = records.data[anchor.custom_index].origin_offset;
	}
	confidence_data.data[probe_idx] = origin_offset;
#endif
}
#endif // MODE_ANCHOR

#ifdef MODE_CLAMP
// Post-pass (1 thread per probe, HWRT and SWRT): performs the exact per-probe 3-sigma firefly clamp
// (#54) over the raw ray samples stored in probe_image, writes the probe distance to .a and publishes
// the probe's mean confidence into confidence_data.
void nextgi_clamp_main() {
	uvec2 probe = gl_GlobalInvocationID.xy;
	if (probe.x >= params.probes_x || probe.y >= params.probes_y) {
		return;
	}
	uint probe_idx = probe.y * params.probes_x + probe.x;
	ivec2 ipixel = clamp(ivec2(probe) * 16 + ivec2(8), ivec2(0), ivec2(params.screen_jitter.xy) - ivec2(1));
	float depth = texelFetch(depth_tex, ipixel, 0).r;
	if (depth <= 0.0) {
		confidence_data.data[probe_idx] = 0.0;
		return;
	}
	vec3 world = nextgi_unproject(vec2(ipixel), depth);
	float probe_dist = length(world - params.camera_pos.xyz);

	const vec3 luminance_weights = vec3(0.2126, 0.7152, 0.0722);
	const uint rstride = max(params.ray_stride, 1u);
	const uint rcount = max(params.ray_count, 1u);
	const ivec2 base_texel = ivec2(probe) * int(rstride);

	float lum_sum = 0.0;
	float confidence_sum = 0.0;
	uint non_hl_count = 0u;
	for (uint i = 0u; i < rcount; i++) {
		ivec2 texel = base_texel + ivec2(int(i % rstride), int(i / rstride));
		vec4 sample_val = imageLoad(probe_image, texel);
		bool is_hl = sample_val.a < -0.5;
		confidence_sum += is_hl ? 1.0 : max(sample_val.a, 0.0);
		if (!is_hl) {
			lum_sum += dot(sample_val.rgb, luminance_weights);
			non_hl_count++;
		}
	}
	const float mean_lum = non_hl_count > 0u ? (lum_sum / float(non_hl_count)) : 0.0;
	float variance = 0.0;
	for (uint i = 0u; i < rcount; i++) {
		ivec2 texel = base_texel + ivec2(int(i % rstride), int(i / rstride));
		vec4 sample_val = imageLoad(probe_image, texel);
		if (!(sample_val.a < -0.5)) {
			const float delta = dot(sample_val.rgb, luminance_weights) - mean_lum;
			variance += delta * delta;
		}
	}
	const float clamp_limit = non_hl_count > 0u ? (mean_lum + 3.0 * sqrt(variance / float(non_hl_count))) : 1e9;

	float removed = 0.0;
	for (uint i = 0u; i < rcount; i++) {
		ivec2 texel = base_texel + ivec2(int(i % rstride), int(i / rstride));
		vec3 radiance = imageLoad(probe_image, texel).rgb;
		const float luminance = dot(radiance, luminance_weights);
		if (luminance > clamp_limit && luminance > 0.0) {
			radiance *= clamp_limit / luminance;
			removed += luminance - clamp_limit;
		}
		imageStore(probe_image, texel, vec4(radiance, probe_dist));
	}
	const uint texels = rstride * rstride;
	for (uint t = rcount; t < texels; t++) {
		uint ray_idx = t % rcount;
		ivec2 src_texel = base_texel + ivec2(int(ray_idx % rstride), int(ray_idx / rstride));
		ivec2 dst_texel = base_texel + ivec2(int(t % rstride), int(t / rstride));
		imageStore(probe_image, dst_texel, imageLoad(probe_image, src_texel));
	}
	atomicAdd(clamp_totals.removed, uint(removed * 4.0));
	atomicAdd(clamp_totals.total, uint(lum_sum * 4.0));
	confidence_data.data[probe_idx] = confidence_sum / float(rcount);
}
#endif // MODE_CLAMP

void nextgi_gather_main() {
	// 1 thread per probe ray texel (16x/64x higher GPU occupancy than 1 thread per probe, zero
	// vec3[64] register spill, zero shared memory). MODE_ANCHOR computes per-probe origin_offset
	// on HWRT; MODE_CLAMP finishes the 3-sigma firefly clamp and confidence reduction.
	const uint rstride = max(params.ray_stride, 1u);
	const uvec2 gid = gl_GlobalInvocationID.xy;
	const uvec2 probe = gid / rstride;
	const uvec2 local = gid - probe * rstride;
	if (probe.x >= params.probes_x || probe.y >= params.probes_y) {
		return;
	}
	const uint i = local.y * rstride + local.x;
	if (i >= params.ray_count) {
		return;
	}
	const uint probe_idx = probe.y * params.probes_x + probe.x;
#ifndef NEXTGI_TRACE_SDF
	float origin_offset = confidence_data.data[probe_idx];
	if (origin_offset < 0.0) {
		return;
	}
#endif
	ivec2 ipixel = clamp(ivec2(probe) * 16 + ivec2(8), ivec2(0), ivec2(params.screen_jitter.xy) - ivec2(1));
	float depth = texelFetch(depth_tex, ipixel, 0).r;
	if (depth <= 0.0) {
		return;
	}
	vec3 world = nextgi_unproject(vec2(ipixel), depth);
	vec3 normal = nextgi_surface_normal(ipixel, depth, world);
#ifdef NEXTGI_TRACE_SDF
	// GodotNext: ps_swrt_trace (#186 step 6). 1.5 voxels of the level that holds the surface.
	vec3 origin = swrt_offset_origin(world, normal);
#else
	vec3 origin = world + normal * origin_offset;
#endif
#ifdef MODE_HIT_LIGHTING_GI
	float probe_depth = length(world - params.camera_pos.xyz);
#endif
	float angle = float(params.frame) * 2.3999632;
	if (params.sampling_mode == 0u) {
		angle = nextgi_blue_noise_sample(ivec2(probe), params.frame).x * 6.2831853;
	}
	uint uncovered_count = 0u;
	uint ix = local.x;
	uint iy = local.y;
	// Exact stratified polar cosine-weighted hemisphere (Malley's method):
	// E[r^2] = 0.5 exactly for every ray_stride, so mean(L_i) estimates E / PI directly.
	float r2 = (float(iy) + 0.5) / float(rstride);
	float disk_r = sqrt(r2);
	float disk_phi = ((float(ix) + 0.5) / float(rstride)) * (2.0 * PI) + angle;
	vec3 local_dir = vec3(disk_r * cos(disk_phi), disk_r * sin(disk_phi), sqrt(max(1.0 - r2, 0.0)));
	vec3 dir = nextgi_orient(normal, local_dir);
	NextGIHit hit;
	vec3 radiance;
	float ray_confidence = 0.0;
	bool queued_hl = false;
	atomicAdd(counters.rays, 1u);
	float handoff = nextgi_wrc_handoff(origin);
#ifdef MODE_HIT_LIGHTING_GI
	float gi_hl_cutoff = handoff > 0.0 ? min(1.5 * handoff, params.rt_radius) : params.rt_radius;
	float tmax = gi_hl_cutoff;
#else
	float tmax = handoff > 0.0 ? handoff : params.rt_radius;
#endif
	vec3 screen_color;
	vec3 march_end;
	bool screen_hit = nextgi_screen_trace(origin, dir, tmax, screen_color, march_end);
	float traveled = length(march_end - origin);
	vec3 trace_origin = origin;
	float trace_tmax = tmax;
	// GodotNext: ps_swrt_trace (#186 step 4). Both tiers resume after the screen march
	// with the pull-back. A hit keeps the screen color and does not continue.
	float resume = 0.0;
	if (!screen_hit) {
		resume = nextgi_screen_resume_tier(origin, dir, traveled, tmax, trace_origin, trace_tmax);
	}
	if (screen_hit) {
		radiance = screen_color;
		ray_confidence = 1.0;
		atomicAdd(counters.screen_hits, 1u);
#ifdef NEXTGI_TRACE_SDF
	} else if (nextgi_sdf_trace_noted(trace_origin, dir, trace_tmax, world, false, resume, hit)) {
#else
	} else if (nextgi_trace(trace_origin, dir, trace_tmax, hit)) {
#endif
#ifdef MODE_HIT_LIGHTING_GI
		// GodotNext: nextgi_hit_lighting_gi (#128). Hits before 1.5x WRC handoff write compact hit
		// records into the shared hit-lighting buffer with keys in [0x8000..0xFFFF].
		if ((resume + hit.t) < gi_hl_cutoff) {
			uint slot = atomicAdd(gi_hl_counters.data.marked_hits, 1u);
			if (slot < params.hit_record_max_rays) {
				uint material_key = NEXTGI_HL_KEY_GI_FALLBACK;
				if (hit.custom_index < params.hit_record_count) {
					NextGIInstanceRecord inst = records.data[hit.custom_index];
					if (hit.geometry_index < inst.surface_count) {
						uint surf_idx = inst.surface_base + hit.geometry_index;
						NextGIHitSurfaceGI hs = gi_hit_surfaces.data[surf_idx];
						if (hs.material_id < params.hit_material_count) {
							NextGIHitMaterialGI hm = gi_hit_materials.data[hs.material_id];
							material_key = nextgi_hit_light_gi_material_key(hm.shader_slot, hs.material_id);
						}
					}
				}
				ivec2 texel = ivec2(gid);
				uint flags = (hit.front_face ? NEXTGI_HL_RECORD_FLAG_FRONT_FACE : 0u) | NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI;
				float cone_width = max(probe_depth * 16.0 / max(params.screen_jitter.y, 1.0), 0.01);
				float cone_spread = 0.18;
				NextGIHitLightRecord rec;
				rec.origin_t = vec4(trace_origin, hit.t);
				rec.id_pixel = uvec4(
						(hit.custom_index & 0xFFFFFFu) | flags,
						(hit.geometry_index & 0xFFFFu) | ((material_key & 0xFFFFu) << 16u),
						hit.primitive,
						(uint(texel.x) & 0xFFFFu) | ((uint(texel.y) & 0xFFFFu) << 16u));
				rec.packed_geom = uvec4(
						packHalf2x16(hit.bary),
						packSnorm2x16(nextgi_hl_oct_encode(dir)),
						packHalf2x16(vec2(cone_width, cone_spread)),
						floatBitsToUint(probe_depth));
				gi_hit_records_out.data[slot] = rec;
				uint gi_prev = atomicAdd(gi_hl_counters.data.gi_marked_hits, 1u);
				if (gi_prev == 0xFFFFFFFFu) {
					gi_hl_counters.data.sink = gi_prev;
				}
				radiance = vec3(0.0);
				ray_confidence = 1.0;
				queued_hl = true;
			} else if (slot == 0xFFFFFFFFu) {
				gi_hl_counters.data.sink = slot;
			}
		}
		if (!queued_hl) {
			radiance = nextgi_shade_hit(hit, dir, uncovered_count, ray_confidence);
		}
#else
		radiance = nextgi_shade_hit(hit, dir, uncovered_count, ray_confidence);
#endif
	} else {
		radiance = nextgi_wrc_sample(trace_origin + dir * trace_tmax, dir);
		if (!nextgi_wrc_hit_cache) {
			radiance = nextgi_miss_radiance(dir);
			ray_confidence = 1.0;
			atomicAdd(counters.sky_samples, 1u);
		} else {
			ray_confidence = nextgi_wrc_confidence;
			atomicAdd(counters.wrc_samples, 1u);
			uncovered_count++;
		}
		atomicAdd(counters.misses, 1u);
		uint lum = uint(clamp(dot(radiance, vec3(0.2126, 0.7152, 0.0722)) * 64.0, 0.0, 4095.0));
		atomicAdd(counters.miss_radiance_bits, lum);
	}
	if (uncovered_count > 0u) {
		atomicAdd(counters.uncovered, uncovered_count);
	}
	imageStore(probe_image, ivec2(gid), vec4(radiance, queued_hl ? -1.0 : ray_confidence));
}

void nextgi_integrate_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 ambient_size = imageSize(ambient_image);
	if (pixel.x >= ambient_size.x || pixel.y >= ambient_size.y) {
		return;
	}
	vec2 screen = max(params.screen_jitter.xy, vec2(1.0));
	vec2 full = (vec2(pixel) + 0.5) * screen / vec2(max(ambient_size, ivec2(1))) - 0.5;
	ivec2 ipixel = ivec2(clamp(full, vec2(0.0), screen - 1.0));
	float depth = texelFetch(depth_tex, ipixel, 0).r;
	if (depth <= 0.0) {
		imageStore(ambient_image, pixel, vec4(0.0));
		return;
	}
	vec3 world = nextgi_unproject(full, depth);
	vec3 normal = nextgi_surface_normal(ipixel, depth, world);
	float view_dist = max(length(world - params.camera_pos.xyz), 0.05);
	ivec2 screen_i = ivec2(screen);
	vec2 probe_f = (full - 8.0) / 16.0;
	ivec2 base = ivec2(floor(probe_f));
	vec2 fract_p = probe_f - vec2(base);
	vec3 accum = vec3(0.0);
	float weight = 0.0;
	float conf_accum = 0.0;
	vec3 fallback_accum = vec3(0.0);
	float fallback_weight = 0.0;
	float fallback_conf = 0.0;
	for (int oy = 0; oy <= 1; oy++) {
		for (int ox = 0; ox <= 1; ox++) {
			ivec2 id = base + ivec2(ox, oy);
			if (id.x < 0 || id.y < 0 || id.x >= int(params.probes_x) || id.y >= int(params.probes_y)) {
				continue;
			}
			float bilinear = (ox == 0 ? 1.0 - fract_p.x : fract_p.x) * (oy == 0 ? 1.0 - fract_p.y : fract_p.y);
			vec4 probe = imageLoad(probe_image, id * int(params.ray_stride));
			if (probe.a <= 0.0) {
				continue;
			}
			ivec2 p_anchor = clamp(id * 16 + ivec2(8), ivec2(0), screen_i - ivec2(1));
			float p_depth = texelFetch(depth_tex, p_anchor, 0).r;
			if (p_depth <= 0.0) {
				continue;
			}
			vec3 p_world = nextgi_unproject(vec2(p_anchor), p_depth);
			vec3 p_normal = nextgi_surface_normal(p_anchor, p_depth, p_world);
			float plane_dist = abs(dot(p_world - world, normal));
			float depth_weight = 1.0 / (1.0 + (plane_dist / (0.05 * view_dist)) + abs(view_dist - probe.a) * 0.5);
			float normal_weight = pow(max(dot(normal, p_normal), 0.0), 4.0);
			vec3 irradiance = vec3(0.0);
			for (uint i = 0u; i < params.ray_stride * params.ray_stride; i++) {
				irradiance += imageLoad(probe_image, id * int(params.ray_stride) + ivec2(i % params.ray_stride, i / params.ray_stride)).rgb;
			}
			irradiance /= float(params.ray_stride * params.ray_stride);
			float probe_confidence = confidence_data.data[uint(id.y) * params.probes_x + uint(id.x)];
			float w = bilinear * depth_weight * normal_weight;
			accum += irradiance * w;
			conf_accum += probe_confidence * w;
			weight += w;
			float fw = bilinear * depth_weight;
			fallback_accum += irradiance * fw;
			fallback_conf += probe_confidence * fw;
			fallback_weight += fw;
		}
	}
	vec3 out_irr = weight > 1e-5 ? (accum / weight) : (fallback_weight > 0.0 ? (fallback_accum / fallback_weight) : vec3(0.0));
	float out_confidence = weight > 1e-5 ? (conf_accum / weight) : (fallback_weight > 0.0 ? (fallback_conf / fallback_weight) : 0.0);
	// Cosine-weighted ray mean is already E / PI, matching scene_forward_clustered.glsl (ambient_light *= albedo.rgb)
	// and nextgi_irradiance_field.glsl (Lumen Lite).
	imageStore(ambient_image, pixel, vec4(out_irr, out_confidence));
}

void nextgi_feedback_main() {
	uint linear = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * params.feedback_stride;
	if (linear >= params.feedback_count) {
		return;
	}
	uvec4 block = feedback_pages.data[linear];
	uint record_index = block.y;
	if (record_index >= params.record_count) {
		return;
	}
	NextGIInstanceRecord rec = records.data[record_index];
	NextGICard card = cards.data[block.z];
	vec3 card_n = normalize(card.axis_n.xyz);
	// GodotNext: nextgi_card_normal (#251). The columns of the basis (its transpose) turned a
	// rotated instance's card hemisphere the wrong way.
	vec3 normal = normalize(nextgi_record_normal_to_world(rec, card_n));
	// Place each 8x8 feedback block at its actual position on the card (instead of card.origin.xyz)
	// so multi-room slabs (e.g. Floor/Ceiling spanning a shared wall) trace from inside each room.
	vec2 block_uv = block.w != 0u ? vec2(float(block.w & 0xFFFFu), float(block.w >> 16u)) / 65535.0 : vec2(0.5);
	float xu = (block_uv.x * 2.0 - 1.0) * card.axis_u.w;
	float yv = (block_uv.y * 2.0 - 1.0) * card.axis_v.w;
	float block_depth = 0.0;
	if (rec.card_set != 0xFFFFFFFFu) {
		NextGICardSet card_set = card_sets.data[rec.card_set];
		vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
		ivec2 mat_coord = ivec2(card.atlas_rect.xy) + ivec2(clamp(block_uv * card_size, vec2(0.0), card_size - 1.0));
		float d = texelFetch(material_depth_tex, mat_coord, 0).r;
		if (d > 0.0) {
			block_depth = d * card_set.capture_scale;
		}
	}
	vec3 block_obj = card.origin.xyz + card.axis_u.xyz * xu + card.axis_v.xyz * yv - card.axis_n.xyz * block_depth;
#ifdef NEXTGI_TRACE_SDF
	// GodotNext: ps_swrt_trace (#186 step 6). The block's world origin, offset 1.5 voxels of its level.
	vec3 card_origin = vec3(
			dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), block_obj) + rec.object_to_world[3],
			dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), block_obj) + rec.object_to_world[7],
			dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), block_obj) + rec.object_to_world[11]);
	vec3 origin = swrt_offset_origin(card_origin, normal);
#else
	vec3 object_origin = block_obj + card.axis_n.xyz * rec.origin_offset;
	vec3 origin = vec3(
			dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), object_origin) + rec.object_to_world[3],
			dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), object_origin) + rec.object_to_world[7],
			dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), object_origin) + rec.object_to_world[11]);
#endif
	vec3 accum = vec3(0.0);
	uint uncovered_count = 0u;
	uint traced = 0u;
	uint escaped = 0u;
	for (uint i = 0u; i < 16u; i++) {
		vec2 uv = vec2(float(i) / 16.0, nextgi_radical_inverse(i + params.frame));
		float radius = sqrt(uv.x);
		float phi = uv.y * 2.0 * PI + float(params.frame) * 0.1;
		vec3 local_dir = vec3(radius * cos(phi), radius * sin(phi), sqrt(max(1.0 - uv.x, 0.0)));
		vec3 dir = nextgi_orient(normal, local_dir);
		NextGIHit hit;
		vec3 radiance = nextgi_miss_radiance(dir);
		float handoff = nextgi_wrc_handoff(origin);
		float tmax = handoff > 0.0 ? handoff : params.rt_radius;
		if ((rec.rt_flags & 8u) != 0u) {
			radiance = nextgi_miss_radiance(dir);
		} else if (nextgi_trace(origin, dir, tmax, hit)) {
			traced++;
			float ignored_confidence = 0.0;
			radiance = nextgi_shade_hit(hit, dir, uncovered_count, ignored_confidence);
		} else {
			traced++;
			escaped++;
			radiance = nextgi_wrc_sample(origin + dir * tmax, dir);
			if (!nextgi_wrc_hit_cache) {
				radiance = nextgi_miss_radiance(dir);
			} else {
				uncovered_count++;
			}
		}
		accum += radiance;
	}
	imageStore(indirect_image, ivec2(block.x & 0xFFFFu, block.x >> 16u), vec4(accum / 16.0 * PI, 1.0));
	// GodotNext: nextgi_card_normal (#251). Feedback rays and those that left the scene unhit.
	atomicAdd(counters.feedback_rays, traced);
	atomicAdd(counters.feedback_escapes, escaped);
}

void nextgi_coverage_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.screen_jitter.xy) / 2;
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}
	ivec2 full = pixel * 2;
	float depth = texelFetch(depth_tex, full, 0).r;
	if (depth <= 0.0) {
		return;
	}
	vec3 world = nextgi_unproject(vec2(full), depth);
	vec3 normal = nextgi_surface_normal(full, depth, world);
	vec3 dir = normal;
	NextGIHit hit;
#ifdef NEXTGI_TRACE_SDF
	// GodotNext: ps_swrt_trace (#186). A global-SDF hit is covered when voxel lighting resolves it.
	if (!nextgi_trace(swrt_offset_origin(world, normal), dir, params.rt_radius, hit) || hit.t > 10.0) {
		return;
	}
	atomicAdd(counters.coverage_rays, 1u);
	if (nextgi_sdf_covered()) {
#else
	if (!nextgi_trace(world + normal * params.max_origin_offset, dir, params.rt_radius, hit) || hit.t > 10.0) {
		return;
	}
	atomicAdd(counters.coverage_rays, 1u);
	vec3 radiance;
	bool uncovered = false;
	if (nextgi_src_lookup(hit, radiance, uncovered) && !uncovered) {
#endif
		atomicAdd(counters.coverage_hits, 1u);
	} else {
		// A WRC sample, or an SRC miss, is uncovered for Q2.
		atomicAdd(counters.uncovered, 1u);
	}
}

// GodotNext: nextgi_reflect (#60). One GGX VNDF reflection ray per trace pixel.
// GodotNext: ps_swrt_trace (#186). The hash and the GGX sample are shared with the software tier.
#include "nextgi_reflect_inc.glsl"

// GodotNext: nextgi_reflection_behaviours (#132). Mirrors the forward fog
// formula for a hit at p_distance/p_world; sky misses keep their radiance.
vec3 nextgi_reflection_fog(vec3 p_radiance, vec3 p_world, float p_distance) {
	if (params.fog_enabled == 0u || p_distance >= 65504.0) {
		return p_radiance;
	}
	float amount;
	if (params.fog_depth_mode != 0u) {
		float fog_z = smoothstep(params.fog_depth_begin, params.fog_depth_end, p_distance);
		amount = pow(fog_z, params.fog_depth_curve) * params.fog_density;
	} else {
		amount = 1.0 - exp(min(0.0, -p_distance * params.fog_density));
	}
	if (abs(params.fog_height_density) >= 0.0001) {
		float y_dist = p_world.y - params.fog_height;
		amount = max(1.0 - exp(min(0.0, y_dist * params.fog_height_density)), amount);
	}
	return mix(p_radiance, params.fog_light_color.rgb * params.fog_light_color.a, clamp(amount, 0.0, 1.0));
}

void nextgi_reflect_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 full_size = ivec2(params.screen_jitter.xy);
	// GodotNext: nextgi_reflect_divisor (#274). The trace size is the output's: NextGIGather sizes the
	// trace textures and the dispatch with nextgi_reflect_trace_size() from reflection_divisor.
	ivec2 size = imageSize(reflection_image);
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
	ivec2 full = ivec2(clamp(uv * vec2(full_size), vec2(0.0), vec2(full_size) - 1.0));
	float depth = texelFetch(depth_tex, full, 0).r;
	if (depth <= 0.0 || params.reflect_enabled == 0u) {
		imageStore(reflection_image, pixel, vec4(0.0));
		imageStore(specular_hit_image, pixel, vec4(65504.0));
		return;
	}
	atomicAdd(reflect_counters.traced, 1u);
	vec4 guide = texelFetch(normal_roughness_tex, full, 0);
	float roughness = nextgi_reflect_decode_roughness(guide.a);
	vec3 normal = nextgi_view_world_normal(mat3(params.view_to_world[0].xyz, params.view_to_world[1].xyz, params.view_to_world[2].xyz), guide.xyz);
	vec3 world = nextgi_unproject(vec2(full), depth);
	vec3 view_dir = normalize(params.camera_pos.xyz - world);
#ifdef NEXTGI_TRACE_SDF
	vec3 origin = swrt_offset_origin(world, normal);
#else
	vec3 origin = world + normal * params.max_origin_offset;
#endif

	uint hash = nextgi_reflect_hash(uint(uv.x * 65535.0) ^ (uint(uv.y * 65535.0) << 16) ^ params.frame);
	vec2 seed = vec2(float(hash & 0xFFFFu), float(hash >> 16)) / 65535.0;
	if (params.sampling_mode == 0u) {
		seed = nextgi_blue_noise_sample(pixel, params.frame);
	}

	vec3 radiance;
	float confidence = 0.0;
	float hit_distance = 65504.0;
	// GodotNext: nextgi_reflect_divisor (#274). A traced ray without a hit (it ends at the WRC
	// handoff or at rt_radius) writes the miss distance; its radiance and fog stay as they were.
	bool missed = false;
	if (roughness <= params.max_trace_roughness) {
		vec3 direction = nextgi_reflect_sample_ggx(normal, view_dir, roughness, seed);
		if (dot(direction, normal) <= 0.0) {
			direction = reflect(-view_dir, normal);
		}
#ifdef NEXTGI_TRACE_SDF
		// GodotNext: ps_swrt_trace (#186 step 4). Reflections trace to rt_scene_radius without the
		// WRC handoff; the screen trace covers the hardware tier's screen range.
		float world_tmax = params.rt_radius;
		float screen_tmax = min(params.rt_radius, swrt_screen_range(origin));
#else
		float handoff = nextgi_wrc_handoff(origin);
		float world_tmax = handoff > 0.0 ? handoff : params.rt_radius;
		float screen_tmax = world_tmax;
#endif
		vec3 screen_color;
		vec3 march_end;
		bool screen_hit = nextgi_screen_trace(origin, direction, screen_tmax, screen_color, march_end);
		if (params.front_layer != 0u) {
			screen_hit = false;
			march_end = origin;
		}
		float traveled = length(march_end - origin);
		vec3 trace_origin = origin;
		float trace_tmax = world_tmax;
		float resume = 0.0;
		if (!screen_hit) {
			resume = nextgi_screen_resume_tier(origin, direction, traveled, world_tmax, trace_origin, trace_tmax);
		}
		NextGIHit hit;
#ifdef MODE_FAR_FIELD
		bool visibility_only = false;
#endif
		uint uncovered_count = 0u;
		if (screen_hit) {
			radiance = screen_color;
			confidence = 1.0;
			hit_distance = traveled;
			atomicAdd(reflect_counters.hits, 1u);
		} else {
			bool hit_result;
#ifdef MODE_FAR_FIELD
			hit = nextgi_trace_near_opaque(trace_origin, direction, trace_tmax);
			hit_result = hit.hit;
			if (!hit_result) {
				visibility_only = params.far_occlusion_only != 0u;
				hit_result = nextgi_trace(origin + direction * params.rt_radius, direction, max(params.far_distance - params.rt_radius, 0.0), hit);
				if (hit_result) {
					hit_distance = params.rt_radius + hit.t;
				}
			} else {
				hit_distance = resume + hit.t;
			}
#elif defined(NEXTGI_TRACE_SDF)
			hit_result = nextgi_sdf_trace_noted(trace_origin, direction, trace_tmax, world, true, resume, hit);
			hit_distance = resume + hit.t;
#else
			hit_result = nextgi_trace(trace_origin, direction, trace_tmax, hit);
			hit_distance = resume + hit.t;
#endif
			missed = !hit_result;
			if (hit_result) {
				atomicAdd(reflect_counters.hits, 1u);
#ifdef MODE_FAR_FIELD
				if (params.far_occlusion_only != 0u && (records.data[hit.custom_index].rt_flags & 8u) != 0u) {
					radiance = vec3(0.0);
					confidence = 1.0;
				} else {
					radiance = nextgi_shade_hit(hit, direction, uncovered_count, confidence);
				}
#else
				radiance = nextgi_shade_hit(hit, direction, uncovered_count, confidence);
#endif
			} else {
#ifdef MODE_FAR_FIELD
				if (params.far_occlusion_only != 0u) {
					radiance = vec3(1.0);
					confidence = 1.0;
				} else {
					radiance = nextgi_miss_radiance(direction);
				}
#else
				radiance = nextgi_wrc_sample(trace_origin + direction * trace_tmax, direction);
				if (!nextgi_wrc_hit_cache) {
					radiance = nextgi_miss_radiance(direction);
				} else {
					confidence = nextgi_wrc_confidence;
				}
#endif
				atomicAdd(reflect_counters.misses, 1u);
			}
		}
#ifdef MODE_FAR_FIELD
		if (!visibility_only) {
			radiance = nextgi_reflection_fog(radiance, origin + direction * hit_distance, hit_distance);
		}
#else
		radiance = nextgi_reflection_fog(radiance, origin + direction * hit_distance, hit_distance);
#endif
		if (missed) {
			hit_distance = 65504.0;
		}
	} else {
		vec3 direction = reflect(-view_dir, normal);
		radiance = nextgi_wrc_sample(origin, direction);
		confidence = nextgi_wrc_confidence;
	}
	imageStore(reflection_image, pixel, vec4(radiance, clamp(confidence, 0.0, 1.0)));
	imageStore(specular_hit_image, pixel, vec4(hit_distance));
	if (params.reflect_debug != 0u) {
		float scaled = hit_distance >= 65504.0 ? 1.0 : clamp(log2(1.0 + hit_distance) / 8.0, 0.0, 1.0);
		imageStore(specular_debug_image, pixel, vec4(vec3(scaled), 1.0));
	}
}

void main() {
#ifdef MODE_INTEGRATE
	nextgi_integrate_main();
#elif defined(MODE_SRC_FEEDBACK)
	nextgi_feedback_main();
#elif defined(MODE_COVERAGE)
	nextgi_coverage_main();
#elif defined(MODE_REFLECT) || defined(MODE_FAR_FIELD)
	nextgi_reflect_main();
#elif defined(MODE_CLAMP)
	nextgi_clamp_main();
#elif defined(MODE_ANCHOR)
	nextgi_anchor_main();
#else
	nextgi_gather_main();
#endif
}

#endif // NEXTGI_GATHER_BODY_INC_GLSL
