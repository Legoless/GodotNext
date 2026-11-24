#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: df_shadows (#214, plan §21.3 contract S7, GV-10).
// Half-resolution distance-field shadow mask pass: one cone trace per pixel per DF light
// through the PS global SDF (#182, up to 10 levels covering 2+ km), refined with per-mesh
// SDFs (#185, via NextGISWRTTrace's 2 m detail froxel grid) within 2 m of the receiver.
// Outputs an R8 mask texture array (1 layer per DF light, max 8 per view) and a matching
// half-res RGBA16F guide (view-space unit normal + linear depth) for the depth- and
// normal-aware 2x2 bilateral upsample in the forward light loop.

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF
#define SWRT_SET 1
#define SWRT_EXTERNAL_VIEW_INPUTS

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "half_inc.glsl"
#include "environment/nextgi_sdf_trace_inc.glsl"

#define DF_LIGHT_DIRECTIONAL 0u
#define DF_LIGHT_OMNI 1u
#define DF_LIGHT_SPOT 2u
#define DF_LIGHT_AREA 3u
#define DF_MAX_LEVELS 10u
#define DF_MIN_CONE_TAN 0.004

struct DFParams {
	uvec4 sizes; // x: full_w, y: full_h, z: half_w, w: half_h.
	uvec4 counts; // x: light_count, y: max_steps, z: total_levels (up to 10), w: flags (bit 0: has_normal_roughness, bit 1: detail_enabled).
	uvec4 fresh; // x: coarse-skip fresh mask (bits 0-9), y: empty-climb/stale fresh mask (bits 0-9), z: unused, w: unused.
	ivec4 extra_origins[4]; // Global SDF levels 6..9 origins (xyz) and status (w: 0 invalid, 1 resident, 2 partial).
	vec4 extra_voxel_size; // Global SDF levels 6..9 voxel sizes in meters.
	vec4 world_to_view[3]; // Camera inverse basis rows (world normal -> view normal fallback when prepass normal is missing).
};

struct DFLight {
	vec4 pos_range; // xyz: world position (local); w: range (local) or max trace distance (directional).
	vec4 dir_radius; // xyz: world direction (directional: toward light; spot/area: shine dir); w: source radius or directional tan(half angle).
	vec4 handoff_spot; // x: directional handoff start (0 if full DF), y: directional handoff end, z: cos(spot angle), w: unused.
	uvec4 meta; // x: DF_LIGHT_*, y: full_df (0/1), z: unused, w: unused.
};

layout(set = 0, binding = 0) uniform sampler2D df_depth_tex;
layout(set = 0, binding = 1) uniform sampler2D df_normal_roughness_tex;

layout(set = 0, binding = 2, std430) restrict readonly buffer DFParamsBuffer {
	DFParams data;
}
df_params;

layout(set = 0, binding = 3, std430) restrict readonly buffer DFLightsBuffer {
	DFLight data[];
}
df_lights;

layout(set = 0, binding = 4, r8) uniform restrict writeonly image2DArray df_mask_out;
layout(set = 0, binding = 5, rgba16f) uniform restrict writeonly image2D df_guide_out;

layout(set = 0, binding = 6, std430) restrict readonly buffer DFPage6 {
	int data[];
}
df_page6;
layout(set = 0, binding = 7, std430) restrict readonly buffer DFPage7 {
	int data[];
}
df_page7;
layout(set = 0, binding = 8, std430) restrict readonly buffer DFPage8 {
	int data[];
}
df_page8;
layout(set = 0, binding = 9, std430) restrict readonly buffer DFPage9 {
	int data[];
}
df_page9;

#define DF_P df_params.data

uint df_total_levels() {
	return min(DF_P.counts.z, DF_MAX_LEVELS);
}

ivec4 df_level_origin(uint level) {
	if (level < 6u) {
		return SWRT_P.origins[level];
	}
	return DF_P.extra_origins[min(level - 6u, 3u)];
}

float df_level_voxel(uint level) {
	if (level < 6u) {
		return swrt_level_voxel(level);
	}
	return DF_P.extra_voxel_size[min(level - 6u, 3u)];
}

int df_page_slot(uint level, uint index) {
	if (level < 6u) {
		return swrt_page_slot(level, index);
	}
	if (level == 6u) {
		return df_page6.data[index];
	}
	if (level == 7u) {
		return df_page7.data[index];
	}
	if (level == 8u) {
		return df_page8.data[index];
	}
	return df_page9.data[index];
}

bool df_level_missing(uint level, vec3 p) {
	ivec3 base = ivec3(floor(p / df_level_voxel(level) - vec3(df_level_origin(level).xyz) - vec3(0.5)));
	ivec3 lo = clamp(base - ivec3(1), ivec3(0), ivec3(127)) >> 3;
	ivec3 hi = clamp(base + ivec3(2), ivec3(0), ivec3(127)) >> 3;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				if (df_page_slot(level, uint(x + 16 * (y + 16 * z))) == SWRT_PAGE_MISSING) {
					return true;
				}
			}
		}
	}
	return false;
}

uint df_level_at(vec3 p) {
	uint levels = df_total_levels();
	for (uint level = 0u; level < levels; level++) {
		ivec4 origin = df_level_origin(level);
		if (origin.w == 0) {
			continue;
		}
		vec3 local = p / df_level_voxel(level) - vec3(origin.xyz);
		if (all(greaterThanEqual(local, vec3(2.0))) && all(lessThan(local, vec3(126.0)))) {
			if (origin.w == 2 && df_level_missing(level, p)) {
				continue;
			}
			return level;
		}
	}
	return SWRT_NO_LEVEL;
}

bool df_fetch(uint level, ivec3 voxel, out float value, out bool coverage) {
	value = SWRT_FAR;
	coverage = false;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return false;
	}
	ivec3 brick = voxel >> 3;
	int slot = df_page_slot(level, uint(brick.x + 16 * (brick.y + 16 * brick.z)));
	if (slot < 0 || uint(slot) >= SWRT_P.counts.w) {
		return false;
	}
	ivec3 local = voxel & ivec3(7);
	uint voxel_index = uint(local.x + 8 * (local.y + 8 * local.z));
	uint pool_index = uint(slot) * 512u + voxel_index;
	value = min(float(swrt_static_pool.data[pool_index]), float(swrt_dynamic_pool.data[pool_index]));
	coverage = (swrt_coverage.data[uint(slot) * 16u + voxel_index / 32u] & (1u << (voxel_index & 31u))) != 0u;
	return value < 60000.0;
}

float df_sample(uint level, vec3 p, out uint present, out bool coverage) {
	float voxel_size = df_level_voxel(level);
	vec3 f = p / voxel_size - vec3(df_level_origin(level).xyz) - vec3(0.5);
	ivec3 base = ivec3(floor(f));
	vec3 w = f - vec3(base);
	float values[8];
	bool have[8];
	present = 0u;
	float largest = -SWRT_FAR;
	for (uint i = 0u; i < 8u; i++) {
		bool unused;
		have[i] = df_fetch(level, base + ivec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u), values[i], unused);
		if (have[i]) {
			present++;
			largest = max(largest, values[i]);
		}
	}
	float nearest;
	df_fetch(level, ivec3(floor(f + vec3(0.5))), nearest, coverage);
	if (present == 0u) {
		return SWRT_FAR;
	}
	float fill = max(largest, 0.0) + voxel_size;
	for (uint i = 0u; i < 8u; i++) {
		if (!have[i]) {
			values[i] = fill;
		}
	}
	float x00 = mix(values[0], values[1], w.x);
	float x10 = mix(values[2], values[3], w.x);
	float x01 = mix(values[4], values[5], w.x);
	float x11 = mix(values[6], values[7], w.x);
	return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

float df_brick_exit(uint level, vec3 p, vec3 direction) {
	float voxel_size = df_level_voxel(level);
	float brick_size = 8.0 * voxel_size;
	vec3 local = p - vec3(df_level_origin(level).xyz) * voxel_size;
	vec3 brick_min = floor(local / brick_size) * brick_size;
	vec3 brick_max = brick_min + vec3(brick_size);
	vec3 safe = vec3(abs(direction.x) > 1e-6 ? direction.x : 1e-6, abs(direction.y) > 1e-6 ? direction.y : 1e-6, abs(direction.z) > 1e-6 ? direction.z : 1e-6);
	vec3 exits = (mix(brick_min, brick_max, greaterThan(safe, vec3(0.0))) - local) / safe;
	return max(min(exits.x, min(exits.y, exits.z)), 0.0);
}

bool df_brick_empty(uint level, vec3 p) {
	ivec4 origin = df_level_origin(level);
	if (origin.w == 0) {
		return false;
	}
	ivec3 voxel = ivec3(floor(p / df_level_voxel(level))) - origin.xyz;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return false;
	}
	ivec3 brick = voxel >> 3;
	return df_page_slot(level, uint(brick.x + 16 * (brick.y + 16 * brick.z))) == SWRT_PAGE_UNALLOCATED;
}

float df_coarse_bound(uint level, vec3 p) {
	if ((DF_P.fresh.x & (1u << level)) == 0u) {
		return 0.0;
	}
	ivec3 voxel = ivec3(floor(p / df_level_voxel(level))) - df_level_origin(level).xyz;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return 0.0;
	}
	ivec3 cell = voxel >> 2;
	return swrt_coarse.data[level * 32768u + uint(cell.x + 32 * (cell.y + 32 * cell.z))];
}

float df_missing_exit(uint level, vec3 p, vec3 direction) {
	float voxel_size = df_level_voxel(level);
	vec3 local = p / voxel_size - vec3(df_level_origin(level).xyz);
	ivec3 base = ivec3(floor(local - vec3(0.5)));
	ivec3 lo = clamp(base - ivec3(1), ivec3(0), ivec3(127)) >> 3;
	ivec3 hi = clamp(base + ivec3(2), ivec3(0), ivec3(127)) >> 3;
	vec3 safe = vec3(abs(direction.x) > 1e-6 ? direction.x : 1e-6, abs(direction.y) > 1e-6 ? direction.y : 1e-6, abs(direction.z) > 1e-6 ? direction.z : 1e-6);
	float reach = 0.0;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				if (df_page_slot(level, uint(x + 16 * (y + 16 * z))) != SWRT_PAGE_MISSING) {
					continue;
				}
				vec3 held_min = vec3(ivec3(x, y, z) * 8) - vec3(1.5);
				vec3 exits = (mix(held_min, held_min + vec3(11.0), greaterThan(safe, vec3(0.0))) - local) / safe;
				reach = max(reach, min(exits.x, min(exits.y, exits.z)) * voxel_size);
			}
		}
	}
	return reach;
}

float df_stale_limit(uint level, vec3 p, vec3 direction) {
	uint fresh_climb = DF_P.fresh.y;
	vec3 safe = vec3(abs(direction.x) > 1e-6 ? direction.x : 1e-6, abs(direction.y) > 1e-6 ? direction.y : 1e-6, abs(direction.z) > 1e-6 ? direction.z : 1e-6);
	float limit = SWRT_FAR;
	for (uint finer = 0u; finer < level; finer++) {
		if ((fresh_climb & (1u << finer)) == 0u) {
			continue;
		}
		float voxel_size = df_level_voxel(finer);
		vec3 origin = vec3(df_level_origin(finer).xyz);
		vec3 region_min = (origin + vec3(2.0)) * voxel_size;
		vec3 region_max = (origin + vec3(126.0)) * voxel_size;
		vec3 t0 = (region_min - p) / safe;
		vec3 t1 = (region_max - p) / safe;
		vec3 slab_in = min(t0, t1);
		vec3 slab_out = max(t0, t1);
		float enter = max(slab_in.x, max(slab_in.y, slab_in.z));
		float leave = min(slab_out.x, min(slab_out.y, slab_out.z));
		if (!(leave > max(enter, 0.0))) {
			continue;
		}
		float reach = enter > 0.0 ? enter : df_missing_exit(finer, p, direction);
		limit = min(limit, reach + 0.01 * voxel_size);
	}
	return limit;
}

float df_self_offset(vec3 surface) {
	uint level = df_level_at(surface);
	if (level == SWRT_NO_LEVEL) {
		level = max(df_total_levels(), 1u) - 1u;
	}
	return 1.5 * df_level_voxel(level);
}

// One-sided smoothstep soft-shadow cone visibility step. Matches
// RendererVirtualShadowMapStorage::df_cone_visibility_step().
float df_cone_vis(float surface_d, float t, float cone_tan, float min_radius) {
	if (surface_d <= 0.0) {
		return 0.0;
	}
	float cone_r = max(cone_tan * t, max(min_radius, 1e-4));
	float u = clamp(surface_d / cone_r, 0.0, 1.0);
	return u * u * (3.0 - 2.0 * u);
}

float df_cone_trace_global(vec3 surface, vec3 world_normal, vec3 origin, vec3 direction, float t_start, float tmax, float cone_tan, uint steps, uint max_steps, bool in_flight, float vis) {
	float t = t_start;
	float cleared = in_flight ? SWRT_FAR : 0.0;
	uint levels = df_total_levels();
	while (steps < max_steps && t < tmax) {
		vec3 p = origin + direction * t;
		uint level = df_level_at(p);
		if (level == SWRT_NO_LEVEL) {
			break;
		}
		steps++;
		float voxel_size = df_level_voxel(level);
		float limit = level > 0u && (DF_P.fresh.y & (1u << level)) == 0u ? df_stale_limit(level, p, direction) : SWRT_FAR;
		float bound = df_coarse_bound(level, p);
		bool coarse_empty = bound >= 4.0 * voxel_size && df_brick_empty(level, p);
		uint present = 0u;
		bool coverage = false;
		float distance = SWRT_FAR;
		if (!coarse_empty) {
			distance = df_sample(level, p, present, coverage);
		}
		if (present == 0u) {
			float skip = max(df_brick_exit(level, p, direction), bound);
			for (uint coarse = level + 1u; coarse < levels; coarse++) {
				if ((DF_P.fresh.y & (1u << coarse)) == 0u || !df_brick_empty(coarse, p)) {
					break;
				}
				skip = max(skip, max(df_brick_exit(coarse, p, direction), df_coarse_bound(coarse, p)));
			}
			cleared = max(cleared, voxel_size);
			t += min(skip + 0.01 * voxel_size, limit);
			continue;
		}
		float band = (coverage ? 1.0 : 0.5) * voxel_size;
		cleared = max(cleared, distance);
		if (cleared < band && (coverage || distance >= 0.0)) {
			t += min(max(band - distance, 0.25 * voxel_size), limit);
			continue;
		}
		float surface_d = distance - band;
		if (surface_d <= 0.0) {
			return 0.0;
		}
		float h_plane = dot(p - surface, world_normal);
		if (t > 0.0 && h_plane >= 2.0 * band) {
			vis = min(vis, df_cone_vis(surface_d, max(t, 0.5 * voxel_size), cone_tan, band));
			if (vis <= 0.005) {
				return 0.0;
			}
		}
		t += min(max(distance, 0.25 * voxel_size), limit);
	}
	return clamp(vis, 0.0, 1.0);
}

float df_cone_trace(vec3 surface, vec3 world_normal, vec3 direction, float tmax, float cone_tan, bool use_detail, uint froxel_cell) {
	if (!(tmax > 0.0) || df_total_levels() == 0u) {
		return 1.0;
	}
	float ndotl = dot(world_normal, direction);
	if (ndotl <= -0.05) {
		return 0.0;
	}
	// Keep the cone inside the receiver's upper hemisphere so an unobstructed flat plane never self-shadows.
	float eff_cone_tan = clamp(cone_tan, DF_MIN_CONE_TAN, max(ndotl * 0.5, DF_MIN_CONE_TAN));
	uint max_steps = max(DF_P.counts.y, 1u);
	float global_offset = df_self_offset(surface);
	vec3 global_origin = surface + world_normal * global_offset;
	uint found_instances = use_detail ? swrt_froxel_found(froxel_cell) : 0u;
	float detail_limit = min(tmax, SWRT_DETAIL_METERS);
	if (found_instances == 0u || !(detail_limit > 0.0)) {
		return df_cone_trace_global(surface, world_normal, global_origin, direction, 0.0, tmax, eff_cone_tan, 0u, max_steps, false, 1.0);
	}

	uint count = min(found_instances, SWRT_FROXEL_CAP);
	bool overflow = found_instances > SWRT_FROXEL_CAP;
	// Detail mesh SDFs have finer voxels than coarse global SDF levels, so cap the detail origin offset.
	float detail_offset = min(global_offset, 0.15);
	vec3 detail_origin = surface + world_normal * detail_offset;
	uint steps = 0u;
	float t = 0.0;
	bool first = true;
	bool escaping = false;
	float vis = 1.0;
	while (steps < max_steps && t < detail_limit) {
		vec3 p = detail_origin + direction * t;
		steps++;
		float best = SWRT_FAR;
		float best_voxel = 0.1;
		uint best_instance = SWRT_NO_INSTANCE;
		for (uint i = 0u; i < count; i++) {
			uint index = swrt_froxel.data[froxel_cell * (SWRT_FROXEL_CAP + 1u) + 1u + i];
			float voxel_size;
			float dist = swrt_instance_distance(index, p, voxel_size);
			if (dist < best) {
				best = dist;
				best_voxel = voxel_size;
				best_instance = index;
			}
		}
		bool two_sided = best_instance != SWRT_NO_INSTANCE && (swrt_instances.data[best_instance].flags.x & 2u) != 0u;
		float band = (two_sided ? 1.0 : 0.5) * best_voxel;
		if (first) {
			escaping = best < band;
			first = false;
		}
		if (escaping) {
			if (best >= band) {
				escaping = false;
			} else if (two_sided || best >= 0.0) {
				t += max(band - best, 0.25 * best_voxel);
				continue;
			}
		}
		float surface_d = best - band;
		if (surface_d <= 0.0) {
			return 0.0;
		}
		float h_plane = dot(p - surface, world_normal);
		if (t > 0.0 && h_plane >= 2.0 * band) {
			vis = min(vis, df_cone_vis(surface_d, max(t, 0.5 * best_voxel), eff_cone_tan, band));
			if (vis <= 0.005) {
				return 0.0;
			}
		}
		t += max(best, 0.25 * best_voxel);
	}

	if (overflow) {
		// Froxel list exceeded cap: also trace the global SDF from t = 0 so unlisted casters contribute.
		return df_cone_trace_global(surface, world_normal, global_origin, direction, 0.0, tmax, eff_cone_tan, 0u, max_steps, false, vis);
	}
	if (t >= tmax) {
		return clamp(vis, 0.0, 1.0);
	}
	// Continue past the 2 m detail range in the global SDF.
	return df_cone_trace_global(surface, world_normal, detail_origin, direction, min(t, detail_limit), tmax, eff_cone_tan, steps, max_steps, true, vis);
}

vec3 df_reconstruct(ivec2 pixel, float depth) {
	vec2 full_size = max(vec2(DF_P.sizes.xy), vec2(1.0));
	return nextgi_view_reconstruct(SWRT_P.inv_view_proj, (vec2(pixel) + vec2(0.5)) / full_size, depth);
}

void df_surface_normals(ivec2 pixel, vec3 world, out vec3 view_normal, out vec3 world_normal) {
	mat3 view_to_world = mat3(SWRT_P.view_to_world[0].xyz, SWRT_P.view_to_world[1].xyz, SWRT_P.view_to_world[2].xyz);
	if ((DF_P.counts.w & 1u) != 0u) {
		vec3 encoded = texelFetch(df_normal_roughness_tex, pixel, 0).xyz;
		if (dot(encoded, encoded) > 1e-6) {
			vec3 vn = encoded * 2.0 - 1.0;
			if (dot(vn, vn) > 1e-12) {
				view_normal = normalize(vn);
				vec3 wn = view_to_world * view_normal;
				world_normal = dot(wn, wn) > 1e-12 ? normalize(wn) : vec3(0.0, 1.0, 0.0);
				return;
			}
		}
	}
	ivec2 full_size = max(ivec2(DF_P.sizes.xy), ivec2(1));
	ivec2 px = clamp(pixel + ivec2(1, 0), ivec2(0), full_size - ivec2(1));
	ivec2 py = clamp(pixel + ivec2(0, 1), ivec2(0), full_size - ivec2(1));
	vec3 wx = df_reconstruct(px, texelFetch(df_depth_tex, px, 0).r) - world;
	vec3 wy = df_reconstruct(py, texelFetch(df_depth_tex, py, 0).r) - world;
	if (px.x == pixel.x) {
		wx = world - df_reconstruct(max(pixel - ivec2(1, 0), ivec2(0)), texelFetch(df_depth_tex, max(pixel - ivec2(1, 0), ivec2(0)), 0).r);
	}
	if (py.y == pixel.y) {
		wy = world - df_reconstruct(max(pixel - ivec2(0, 1), ivec2(0)), texelFetch(df_depth_tex, max(pixel - ivec2(0, 1), ivec2(0)), 0).r);
	}
	vec3 wn = cross(wx, wy);
	vec3 to_cam = SWRT_P.camera.xyz - world;
	if (dot(wn, wn) <= 1e-12) {
		wn = normalize(to_cam);
	} else {
		wn = normalize(wn);
		if (dot(wn, to_cam) < 0.0) {
			wn = -wn;
		}
	}
	world_normal = wn;
	vec3 vn = vec3(dot(DF_P.world_to_view[0].xyz, wn), dot(DF_P.world_to_view[1].xyz, wn), dot(DF_P.world_to_view[2].xyz, wn));
	view_normal = dot(vn, vn) > 1e-12 ? normalize(vn) : vec3(0.0, 0.0, 1.0);
}

void main() {
	ivec2 hp = ivec2(gl_GlobalInvocationID.xy);
	uint layer = gl_GlobalInvocationID.z;
	if (hp.x >= int(DF_P.sizes.z) || hp.y >= int(DF_P.sizes.w) || layer >= DF_P.counts.x) {
		return;
	}
	ivec2 full_size = max(ivec2(DF_P.sizes.xy), ivec2(1));
	ivec2 fp = min(hp * 2, full_size - ivec2(1));
	float depth = texelFetch(df_depth_tex, fp, 0).r;
	if (nextgi_view_is_background(depth)) {
		if (layer == 0u) {
			imageStore(df_guide_out, hp, vec4(0.0, 0.0, 1.0, 0.0));
		}
		imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(1.0, 0.0, 0.0, 0.0));
		return;
	}

	float linear_depth = nextgi_view_linear_depth(SWRT_P.depth_coefficients, depth);
	vec3 surface = df_reconstruct(fp, depth);
	vec3 view_normal;
	vec3 world_normal;
	df_surface_normals(fp, surface, view_normal, world_normal);
	if (layer == 0u) {
		imageStore(df_guide_out, hp, vec4(view_normal, linear_depth));
	}

	DFLight light = df_lights.data[layer];
	uint light_type = light.meta.x;
	bool full_df = light.meta.y != 0u;
	vec3 trace_dir = vec3(0.0, 1.0, 0.0);
	float tmax = 0.0;
	float cone_tan = DF_MIN_CONE_TAN;

	if (light_type == DF_LIGHT_DIRECTIONAL) {
		if (!full_df) {
			float start = light.handoff_spot.x;
			float end = light.handoff_spot.y;
			float blend_lo = start * (1.0 - 0.1);
			// Margin keeps 2x2 upsample boundary taps traced across the start and end edges.
			float margin = max(start * 0.02, 1.0);
			if (linear_depth < blend_lo - margin || linear_depth > end + margin) {
				imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(1.0, 0.0, 0.0, 0.0));
				return;
			}
		} else if (light.handoff_spot.y > 0.0 && linear_depth > light.handoff_spot.y * 1.02 + 1.0) {
			imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(1.0, 0.0, 0.0, 0.0));
			return;
		}
		trace_dir = normalize(light.dir_radius.xyz);
		tmax = min(max(light.pos_range.w, 1.0), 3200.0);
		cone_tan = max(light.dir_radius.w, DF_MIN_CONE_TAN);
	} else {
		vec3 to_light = light.pos_range.xyz - surface;
		float dist = length(to_light);
		if (!(dist > 1e-4) || dist >= light.pos_range.w) {
			imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(1.0, 0.0, 0.0, 0.0));
			return;
		}
		trace_dir = to_light / dist;
		if (light_type == DF_LIGHT_SPOT) {
			float cos_angle = dot(-trace_dir, light.dir_radius.xyz);
			if (cos_angle < light.handoff_spot.z - 0.05) {
				imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(1.0, 0.0, 0.0, 0.0));
				return;
			}
		} else if (light_type == DF_LIGHT_AREA) {
			if (dot(-trace_dir, light.dir_radius.xyz) <= 0.0) {
				imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(1.0, 0.0, 0.0, 0.0));
				return;
			}
		}
		float source_radius = max(light.dir_radius.w, 0.0);
		tmax = max(dist - max(source_radius, 0.05), 0.0);
		cone_tan = max(source_radius / max(dist, 0.05), DF_MIN_CONE_TAN);
	}

	bool use_detail = (DF_P.counts.w & 2u) != 0u;
	uint froxel = use_detail ? swrt_froxel_cell(surface) : 0xffffffffu;
	float vis = df_cone_trace(surface, world_normal, trace_dir, tmax, cone_tan, use_detail, froxel);
	imageStore(df_mask_out, ivec3(hp, int(layer)), vec4(vis, 0.0, 0.0, 0.0));
}
