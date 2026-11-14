// GodotNext: ps_swrt_trace (#186, plan §21.2 L5). Not a standalone shader.
//
// The software ray tracer. Included by nextgi_gather_sdf.glsl (gather,
// reflection, Lite probe and detail froxel kernels) and by
// nextgi_src_relight_sdf.glsl (shadow rays). The includer defines
// EXPLICIT_FP16 and includes half_inc.glsl first.
//
// Trace order (L5): screen trace (Hi-Z, the hardware tier's) -> detail mesh
// SDFs for the first 2 m (software_ray_tracing_mode = detail only) -> global
// SDF -> WRC at the handoff distance or on a miss -> sky.
//
// Functions a consumer calls (NextGISWRTTrace documents the bindings):
//   SWRTHit swrt_trace(origin, direction, tmax, detail, froxel_cell[, resume])
//   bool swrt_shadow_ray(origin, direction, tmax)   // true when occluded
//   vec3 swrt_offset_origin(surface, normal)        // 1.5 voxels of the level
//   vec3 swrt_shade(hit, origin, direction, allow_wrc)
//   vec3 swrt_reconstruct(pixel, depth)             // nextgi_view_math
//
// GodotNext: ps_swrt_trace (#186). The production siblings of the hardware shaders
// (nextgi_gather_swrt.glsl, nextgi_wrc_update_swrt.glsl) bind the trace's resources in set
// SWRT_SET (default 0) and define SWRT_EXTERNAL_VIEW_INPUTS: their own set 0 declares the depth,
// screen, sky and WRC inputs, so this file leaves out its counters, its view bindings (13-20),
// the WRC and screen-trace includes and the functions that read them (swrt_miss_radiance,
// swrt_shade, swrt_reconstruct, swrt_surface_normal). Such an includer includes
// nextgi_wrc_sample_inc.glsl (and, when it traces the screen, nextgi_screen_trace_inc.glsl)
// before this file: the RD header builder pastes an include once, at its first occurrence.

#ifndef NEXTGI_SDF_TRACE_INC_GLSL
#define NEXTGI_SDF_TRACE_INC_GLSL

#ifndef SWRT_SET
#define SWRT_SET 0
#endif

#include "../oct_inc.glsl"
#include "nextgi_mesh_sdf_distance_inc.glsl"
#include "nextgi_view_inc.glsl"

// SWRTRayResult.flags and SWRTHit.flags. NextGISWRTTrace::HitFlags mirrors them.
#define SWRT_HIT_SURFACE 1u // Stopped on a surface (or on the step limit).
#define SWRT_HIT_EXHAUSTED 2u // The step limit ran out: a hit at the current t.
#define SWRT_HIT_SCREEN 4u // The Hi-Z screen trace hit first.
#define SWRT_HIT_DETAIL 8u // A mesh-SDF hit; the instance is valid.
#define SWRT_HIT_WRC 16u // Shaded from the WRC.
#define SWRT_HIT_SKY 32u // Shaded from the sky.
#define SWRT_HIT_COVERAGE 64u // The hit voxel is two-sided coverage.
#define SWRT_HIT_SKIPPED 128u // No ray: background pixel, or above the roughness gate.
#define SWRT_HIT_HANDOFF 256u // A gather ray reached the WRC handoff distance.
#define SWRT_HIT_VOXEL 512u // Shaded from voxel lighting.
#define SWRT_HIT_CARD 1024u // A two-sided instance resolved this coverage hit (#186 step 6).
#define SWRT_HIT_BACKFACE 2048u // The ray meets the back of that instance's card.

#define SWRT_NO_LEVEL 0xffffffffu
#define SWRT_NO_INSTANCE 0xffffffffu
#define SWRT_FAR 65504.0

struct SWRTInstance {
	vec4 inverse_row0;
	vec4 inverse_row1;
	vec4 inverse_row2;
	vec4 world_min; // xyz: world bounds of the mesh SDF.
	vec4 world_max;
	uvec4 flags; // x: 2 = two-sided, y: first mesh-info (mip 0), z: mip count, w: SRC card set or 0xffffffff.
	vec4 axis_scale; // NextGIGlobalSDF::instance_axis_scale().
};

// One mip of one mesh SDF (detail mode).
struct SWRTMeshInfo {
	vec4 bounds_voxel; // xyz: local bounds position, w: this mip's voxel size.
	ivec4 dimensions; // xyz: voxels.
	ivec4 brick_grid; // xyz: bricks.
	uvec4 offsets; // x: page offset, y: brick word offset, z: unused, w: band (float bits).
};

struct SWRTParams {
	mat4 inv_view_proj; // NextGIView: the depth texture's clip space to world.
	mat4 view_proj;
	vec4 camera; // xyz camera, w: active global-SDF levels.
	vec4 screen; // xy internal size, z: depth valid, w: Hi-Z max mip.
	vec4 trace; // x: gather tmax, y: max steps, z: detail mode, w: max roughness to trace.
	vec4 ambient; // rgb: ambient light, w: sky radiance texture valid.
	ivec4 origins[6]; // xyz: level origin in voxels, w: 1 when the level is resident, 2 when it is partial.
	vec4 voxel_size0;
	vec4 voxel_size1;
	ivec4 clipmap[4]; // Voxel-lighting clipmap (NextGIVoxelLighting::ClipmapLevelGPU).
	vec4 grid; // xy: ray grid cells, zw: screen pixels per cell.
	vec4 flags; // x: voxel lighting valid, y: screen trace valid, z: WRC valid, w: instance count.
	vec4 view_forward; // xyz: camera forward, w: two-sided instance count.
	uvec4 counts; // x: frame, y: shadow requests, z: lights, w: pool capacity in bricks.
	vec4 froxel; // xy: detail froxel cells, z: depth slices, w: reflection tmax.
	vec4 froxel_depth; // x: near, y: far (meters), z: detail instance count, w: unused.
	vec4 depth_coefficients; // NextGIViewMath::depth_coefficients() of the depth projection.
	vec4 extra; // x: normal-roughness valid, y: froxel grid valid, z: write shadow requests, w: fresh-level mask (#257).
	vec4 view_to_world[3]; // Camera basis columns: the normal-roughness buffer is in view space.
};

#ifndef SWRT_EXTERNAL_VIEW_INPUTS
layout(set = SWRT_SET, binding = 0, std430) buffer SWRTCounters {
	uint steps;
	uint gather_rays;
	uint gather_hits;
	uint reflect_rays;
	uint reflect_hits;
	uint shadow_rays;
	uint shadow_occluded;
	uint exhausted;
	uint screen_hits;
	uint wrc_samples;
	uint detail_hits;
	uint skipped;
	uint coarse_steps; // GodotNext: global_sdf_coarse_bound (#257)
}
swrt_counters;
#endif // !SWRT_EXTERNAL_VIEW_INPUTS

layout(set = SWRT_SET, binding = 1, std430) readonly buffer SWRTParamsBuffer {
	SWRTParams data;
}
swrt_params_buffer;

layout(set = SWRT_SET, binding = 2, std430) readonly buffer SWRTStaticPool {
	float16_t data[];
}
swrt_static_pool;

layout(set = SWRT_SET, binding = 3, std430) readonly buffer SWRTDynamicPool {
	float16_t data[];
}
swrt_dynamic_pool;

// 512 bits per slot in 16 words: word voxel / 32, bit voxel % 32 (#182 writer).
layout(set = SWRT_SET, binding = 4, std430) readonly buffer SWRTCoverage {
	uint data[];
}
swrt_coverage;

layout(set = SWRT_SET, binding = 5, std430) readonly buffer SWRTPage0 {
	int data[];
}
swrt_page0;
layout(set = SWRT_SET, binding = 6, std430) readonly buffer SWRTPage1 {
	int data[];
}
swrt_page1;
layout(set = SWRT_SET, binding = 7, std430) readonly buffer SWRTPage2 {
	int data[];
}
swrt_page2;
layout(set = SWRT_SET, binding = 8, std430) readonly buffer SWRTPage3 {
	int data[];
}
swrt_page3;
layout(set = SWRT_SET, binding = 9, std430) readonly buffer SWRTPage4 {
	int data[];
}
swrt_page4;
layout(set = SWRT_SET, binding = 10, std430) readonly buffer SWRTPage5 {
	int data[];
}
swrt_page5;

layout(set = SWRT_SET, binding = 11, std430) readonly buffer NextGIVoxelRadiance {
	uint data[];
}
voxel_radiance;

layout(set = SWRT_SET, binding = 12, std430) readonly buffer NextGIVoxelWeights {
	uint data[];
}
voxel_weights;

#include "nextgi_voxel_lighting_inc.glsl"

#ifndef SWRT_EXTERNAL_VIEW_INPUTS
layout(set = SWRT_SET, binding = 13) uniform sampler2D swrt_depth;
layout(set = SWRT_SET, binding = 14) uniform sampler2D swrt_normal_roughness;
layout(set = SWRT_SET, binding = 15) uniform sampler2D swrt_hiz;
layout(set = SWRT_SET, binding = 16) uniform sampler2D swrt_last_frame;
layout(set = SWRT_SET, binding = 17) uniform sampler2D swrt_sky;
layout(set = SWRT_SET, binding = 18) uniform sampler2D nextgi_wrc_radiance;
layout(set = SWRT_SET, binding = 19) uniform sampler2D nextgi_wrc_visibility;
layout(set = SWRT_SET, binding = 20, std430) restrict readonly buffer SWRTWRCParams {
	vec4 camera;
	vec4 sky;
	vec4 origin_spacing;
	uvec4 dispatch;
}
nextgi_wrc;
#endif // !SWRT_EXTERNAL_VIEW_INPUTS

layout(set = SWRT_SET, binding = 21, std430) readonly buffer SWRTInstances {
	SWRTInstance data[];
}
swrt_instances;

layout(set = SWRT_SET, binding = 22, std430) readonly buffer SWRTMeshes {
	SWRTMeshInfo data[];
}
swrt_meshes;

layout(set = SWRT_SET, binding = 23, std430) readonly buffer SWRTMeshPages {
	int data[];
}
swrt_mesh_pages;

layout(set = SWRT_SET, binding = 24, std430) readonly buffer SWRTMeshBricks {
	uint data[];
}
swrt_mesh_bricks;

// Detail froxel object grid: per cell a word, then up to 8 instance indices.
// The word holds SWRT_FROXEL_MARKED (the cell holds a ray-grid surface) and the
// number of instances the cull found, which can exceed the cap.
#define SWRT_FROXEL_CAP 8u
#define SWRT_FROXEL_MARKED 0x80000000u
#define SWRT_FROXEL_COUNT_MASK 0x7fffffffu
#define SWRT_DETAIL_METERS 2.0
layout(set = SWRT_SET, binding = 25, std430) buffer SWRTFroxel {
	uint data[];
}
swrt_froxel;

layout(set = SWRT_SET, binding = 26, std430) readonly buffer SWRTTwoSided {
	uint data[];
}
swrt_two_sided;

// GodotNext: global_sdf_coarse_bound (#257). Each global-SDF level's coarse bound
// (NextGIGlobalSDFGPU::get_coarse_buffer(), 32^3 floats per level). Binding 30: the set-0
// kernels use 27 to 29 for their own buffers.
layout(set = SWRT_SET, binding = 30, std430) readonly buffer SWRTCoarse {
	float data[];
}
swrt_coarse;
// Coarse steps this invocation's traces took (the verification kernel reports them).
uint swrt_coarse_steps = 0u;

#ifndef SWRT_EXTERNAL_VIEW_INPUTS
#define NEXTGI_WRC
#include "nextgi_wrc_sample_inc.glsl"
#define NEXTGI_SCREEN_HIZ_TEX swrt_hiz
#define NEXTGI_SCREEN_LAST_FRAME_TEX swrt_last_frame
#include "nextgi_screen_trace_inc.glsl"
#endif // !SWRT_EXTERNAL_VIEW_INPUTS

SWRTParams swrt_params() {
	return swrt_params_buffer.data;
}

// Hot paths read single fields instead of copying the whole struct.
#define SWRT_P swrt_params_buffer.data

// --- Global-SDF levels -------------------------------------------------------

uint swrt_active_levels() {
	return min(uint(max(SWRT_P.camera.w, 0.0)), 6u);
}

float swrt_level_voxel(uint level) {
	if (level < 4u) {
		return SWRT_P.voxel_size0[level];
	}
	return SWRT_P.voxel_size1[min(level - 4u, 1u)];
}

int swrt_page_slot(uint level, uint index) {
	if (level == 0u) {
		return swrt_page0.data[index];
	}
	if (level == 1u) {
		return swrt_page1.data[index];
	}
	if (level == 2u) {
		return swrt_page2.data[index];
	}
	if (level == 3u) {
		return swrt_page3.data[index];
	}
	if (level == 4u) {
		return swrt_page4.data[index];
	}
	return swrt_page5.data[index];
}

// NextGIGlobalSDFGPU::PAGE_UNALLOCATED and PAGE_MISSING.
#define SWRT_PAGE_UNALLOCATED -1
#define SWRT_PAGE_MISSING -2

// A partial level (the pool ran out at the level floor) marks the bricks that
// hold geometry but got no slot. True when the trilinear corners or the normal
// taps at p (voxels floor(f) - 1 to floor(f) + 2) reach one of them.
bool swrt_level_missing(uint level, vec3 p) {
	ivec3 base = ivec3(floor(p / swrt_level_voxel(level) - vec3(SWRT_P.origins[level].xyz) - vec3(0.5)));
	ivec3 lo = clamp(base - ivec3(1), ivec3(0), ivec3(127)) >> 3;
	ivec3 hi = clamp(base + ivec3(2), ivec3(0), ivec3(127)) >> 3;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				if (swrt_page_slot(level, uint(x + 16 * (y + 16 * z))) == SWRT_PAGE_MISSING) {
					return true;
				}
			}
		}
	}
	return false;
}

// The finest resident level whose clipmap holds p two voxels inside its edge,
// so the trilinear corners and the normal taps stay in the level. Loops the
// levels from 0 (#186 step 2). Where a partial level misses a brick, the next
// level holds p (the levels are built coarsest first).
uint swrt_level_at(vec3 p) {
	uint levels = swrt_active_levels();
	for (uint level = 0u; level < levels; level++) {
		ivec4 origin = SWRT_P.origins[level];
		if (origin.w == 0) {
			continue;
		}
		vec3 local = p / swrt_level_voxel(level) - vec3(origin.xyz);
		if (all(greaterThanEqual(local, vec3(2.0))) && all(lessThan(local, vec3(126.0)))) {
			if (origin.w == 2 && swrt_level_missing(level, p)) {
				continue;
			}
			return level;
		}
	}
	return SWRT_NO_LEVEL;
}

// One stored voxel. False for a voxel outside the level or in an unallocated brick.
bool swrt_fetch(uint level, ivec3 voxel, out float value, out bool coverage) {
	value = SWRT_FAR;
	coverage = false;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return false;
	}
	ivec3 brick = voxel >> 3;
	int slot = swrt_page_slot(level, uint(brick.x + 16 * (brick.y + 16 * brick.z)));
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

// Trilinear distance between the voxel centers. present counts the corners
// that exist; a missing corner reads one voxel past the largest present one,
// so it never pulls the surface closer. coverage is the nearest voxel's bit.
float swrt_sample(uint level, vec3 p, out uint present, out bool coverage) {
	float voxel_size = swrt_level_voxel(level);
	vec3 f = p / voxel_size - vec3(SWRT_P.origins[level].xyz) - vec3(0.5);
	ivec3 base = ivec3(floor(f));
	vec3 w = f - vec3(base);
	float values[8];
	bool have[8];
	present = 0u;
	float largest = -SWRT_FAR;
	for (uint i = 0u; i < 8u; i++) {
		bool unused;
		have[i] = swrt_fetch(level, base + ivec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u), values[i], unused);
		if (have[i]) {
			present++;
			largest = max(largest, values[i]);
		}
	}
	float nearest;
	swrt_fetch(level, ivec3(floor(f + vec3(0.5))), nearest, coverage);
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

// Distance along the ray to the exit of the 8^3 brick that holds p.
float swrt_brick_exit(uint level, vec3 p, vec3 direction) {
	float voxel_size = swrt_level_voxel(level);
	float brick_size = 8.0 * voxel_size;
	vec3 local = p - vec3(SWRT_P.origins[level].xyz) * voxel_size;
	vec3 brick_min = floor(local / brick_size) * brick_size;
	vec3 brick_max = brick_min + vec3(brick_size);
	vec3 safe = vec3(abs(direction.x) > 1e-6 ? direction.x : 1e-6, abs(direction.y) > 1e-6 ? direction.y : 1e-6, abs(direction.z) > 1e-6 ? direction.z : 1e-6);
	vec3 exits = (mix(brick_min, brick_max, greaterThan(safe, vec3(0.0))) - local) / safe;
	return max(min(exits.x, min(exits.y, exits.z)), 0.0);
}

// True when the brick that holds p at this level is resident in the page
// table but unallocated: no instance's mesh SDF comes within a voxel of it. A
// missing brick of a partial level is not empty.
bool swrt_brick_empty(uint level, vec3 p) {
	ivec4 origin = SWRT_P.origins[level];
	if (origin.w == 0) {
		return false;
	}
	ivec3 voxel = ivec3(floor(p / swrt_level_voxel(level))) - origin.xyz;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return false;
	}
	ivec3 brick = voxel >> 3;
	return swrt_page_slot(level, uint(brick.x + 16 * (brick.y + 16 * brick.z))) == SWRT_PAGE_UNALLOCATED;
}

// GodotNext: global_sdf_coarse_bound (#257). The coarse bound of the cell (4^3 voxels) that holds
// p: a lower bound of the fine sample anywhere in the cell and of the distance to the nearest
// brick that may hold a surface (nextgi_global_sdf_coarse.glsl). 0, so no step, outside the
// level or with the coarse skip off.
float swrt_coarse_bound(uint level, vec3 p) {
	// Bits 0-5: levels with no dirty volume pending, while the coarse skip is on
	// (NextGISWRTTrace's nextgi_swrt_coarse_levels()).
	if ((uint(SWRT_P.extra.w + 0.5) & (1u << level)) == 0u) {
		return 0.0;
	}
	ivec3 voxel = ivec3(floor(p / swrt_level_voxel(level))) - SWRT_P.origins[level].xyz;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return 0.0;
	}
	ivec3 cell = voxel >> 2;
	return swrt_coarse.data[level * 32768u + uint(cell.x + 32 * (cell.y + 32 * cell.z))];
}

// Gradient from six taps one voxel out. A tap without all eight corners is
// missing: the axis falls back to a one-sided difference with the center, and
// an axis with both taps missing contributes nothing.
vec3 swrt_normal(uint level, vec3 p, float center, vec3 fallback) {
	float voxel_size = swrt_level_voxel(level);
	vec3 gradient = vec3(0.0);
	for (uint axis = 0u; axis < 3u; axis++) {
		vec3 offset = vec3(0.0);
		offset[axis] = voxel_size;
		uint plus_present;
		uint minus_present;
		bool unused;
		float plus = swrt_sample(level, p + offset, plus_present, unused);
		float minus = swrt_sample(level, p - offset, minus_present, unused);
		bool has_plus = plus_present == 8u;
		bool has_minus = minus_present == 8u;
		if (has_plus && has_minus) {
			gradient[axis] = plus - minus;
		} else if (has_plus) {
			gradient[axis] = 2.0 * (plus - center);
		} else if (has_minus) {
			gradient[axis] = 2.0 * (center - minus);
		}
	}
	if (dot(gradient, gradient) <= 1e-12) {
		return fallback;
	}
	return normalize(gradient);
}

// Self-intersection offset: 1.5 voxels of the finest level that holds the surface (#186 step 6).
float swrt_self_offset(vec3 surface) {
	uint level = swrt_level_at(surface);
	if (level == SWRT_NO_LEVEL) {
		level = max(swrt_active_levels(), 1u) - 1u;
	}
	return 1.5 * swrt_level_voxel(level);
}

vec3 swrt_offset_origin(vec3 surface, vec3 normal) {
	return surface + normal * swrt_self_offset(surface);
}

float swrt_instance_distance(uint index, vec3 world, out float voxel_size);

// GodotNext: ps_swrt_trace (#186 step 6, plan §21.2.6). A two-sided coverage voxel is a hit
// from either side of the shell, as UE traces two-sided distance fields from either side. The
// instance whose shell was hit resolves it: hit_instance from the detail march, else the
// two-sided instance whose bounds, grown by pad (the hit band), contain p and whose mesh SDF is
// nearest there. When that mesh is a flat card (its own bounds are thinner than two of its
// finest voxels along one local axis and no other: a quad, a leaf card), the card normal is that
// axis taken to world space. front is false when the ray meets the card from behind that normal, so the
// WRC probe relocation fires there as it does on a cull_disabled triangle's back face. outward
// is the card normal turned toward the ray, the shading normal. Any other two-sided mesh, and a
// coverage voxel no two-sided instance contains, keeps the gradient normal, which faces the ray.
bool swrt_two_sided_outward(vec3 p, vec3 direction, uint hit_instance, float pad, out vec3 outward, out bool front) {
	outward = vec3(0.0);
	front = true;
	uint best_index = hit_instance;
	if (best_index == SWRT_NO_INSTANCE) {
		uint count = uint(max(SWRT_P.view_forward.w, 0.0));
		float best = 1e30;
		for (uint i = 0u; i < count; i++) {
			uint index = swrt_two_sided.data[i];
			SWRTInstance instance = swrt_instances.data[index];
			if (any(lessThan(p, instance.world_min.xyz - vec3(pad))) || any(greaterThan(p, instance.world_max.xyz + vec3(pad)))) {
				continue;
			}
			float unused;
			float distance = abs(swrt_instance_distance(index, p, unused));
			if (distance < best) {
				best = distance;
				best_index = index;
			}
		}
		if (best_index == SWRT_NO_INSTANCE) {
			return false;
		}
	}
	SWRTInstance card_instance = swrt_instances.data[best_index];
	SWRTMeshInfo finest = swrt_meshes.data[card_instance.flags.y];
	vec3 finest_max = finest.bounds_voxel.xyz + vec3(max(finest.dimensions.xyz, ivec3(1))) * finest.bounds_voxel.w;
	vec3 surface_min;
	vec3 surface_max;
	nextgi_mesh_sdf_surface_bounds(finest.bounds_voxel.xyz, finest_max, uintBitsToFloat(finest.offsets.w), surface_min, surface_max);
	vec3 extent = surface_max - surface_min;
	uint axis = extent.x <= extent.y && extent.x <= extent.z ? 0u : (extent.y <= extent.z ? 1u : 2u);
	float next_extent = axis == 0u ? min(extent.y, extent.z) : (axis == 1u ? min(extent.x, extent.z) : min(extent.x, extent.y));
	float flat_limit = 2.0 * finest.bounds_voxel.w;
	if (extent[axis] > flat_limit || next_extent <= flat_limit) {
		return false; // Not a flat card: too thick, or thin along two axes.
	}
	// A local normal goes to world space with the transpose of the world-to-local basis, so the
	// card normal is row axis of the inverse (normalized, which also covers non-uniform scale).
	vec3 card = axis == 0u ? card_instance.inverse_row0.xyz : (axis == 1u ? card_instance.inverse_row1.xyz : card_instance.inverse_row2.xyz);
	if (!(dot(card, card) > 1e-12)) {
		return false;
	}
	card = normalize(card);
	front = dot(card, direction) <= 0.0;
	outward = front ? card : -card;
	return true;
}

void swrt_cover_two_sided(vec3 p, vec3 direction, uint hit_instance, float pad, inout vec3 normal, inout uint flags) {
	vec3 outward;
	bool front;
	if (!swrt_two_sided_outward(p, direction, hit_instance, pad, outward, front)) {
		return;
	}
	normal = outward;
	flags |= SWRT_HIT_CARD;
	if (!front) {
		flags |= SWRT_HIT_BACKFACE;
	}
}

struct SWRTHit {
	float t;
	vec3 normal;
	uint flags;
	uint instance;
	uint steps;
	uint level;
};

SWRTHit swrt_no_hit(uint steps) {
	SWRTHit hit;
	hit.t = -1.0;
	hit.normal = vec3(0.0);
	hit.flags = 0u;
	hit.instance = SWRT_NO_INSTANCE;
	hit.steps = steps;
	hit.level = SWRT_NO_LEVEL;
	return hit;
}

// Sphere-traces the global SDF from t_start. Unallocated bricks are skipped
// to their exit (no surface is within a voxel of them). The step limit
// reports a hit at the current t (#186 step 2).
// A ray that starts on the surface it leaves has not cleared that surface until
// a sample lies outside the band (an origin offset into a concave corner starts
// inside one, and a coarser level's band is wider than the offset a finer level
// gave the origin). cleared is the largest distance sampled so far: while it is
// below the band of the sample's level, a sample in that band is still the
// surface the ray left, and only a penetration (a negative distance) is a hit.
// A ray in flight (in_flight: continued past the detail range, or resumed after
// the screen march, #186 step 4) starts cleared: a band it starts in belongs to
// a surface in its way.
SWRTHit swrt_trace_global(vec3 origin, vec3 direction, float t_start, float tmax, uint steps, bool in_flight) {
	uint max_steps = uint(max(SWRT_P.trace.y, 1.0));
	float t = t_start;
	float cleared = in_flight ? SWRT_FAR : 0.0;
	while (steps < max_steps && t < tmax) {
		vec3 p = origin + direction * t;
		uint level = swrt_level_at(p);
		if (level == SWRT_NO_LEVEL) {
			return swrt_no_hit(steps); // Left every clipmap.
		}
		steps++;
		float voxel_size = swrt_level_voxel(level);
		// GodotNext: global_sdf_coarse_bound (#257). The coarse bound never exceeds the fine sample
		// in its cell nor the distance to a brick that may hold a surface. In an unallocated brick,
		// a bound of at least a cell puts every voxel the sample would read in empty bricks, so
		// the sample is skipped: the step is the empty-space skip below without it. A bound only
		// lengthens that skip, so no step is shorter than without the coarse level.
		float bound = swrt_coarse_bound(level, p);
		bool coarse_empty = bound >= 4.0 * voxel_size && swrt_brick_empty(level, p);
		uint present = 0u;
		bool coverage = false;
		float distance = SWRT_FAR;
		if (!coarse_empty) {
			distance = swrt_sample(level, p, present, coverage);
		}
		if (present == 0u) {
			// Empty space: skip to the exit of the largest empty brick around p,
			// climbing the coarser levels while their brick is empty too.
			float skip = max(swrt_brick_exit(level, p, direction), bound);
			uint levels = swrt_active_levels();
			for (uint coarse = level + 1u; coarse < levels; coarse++) {
				// GodotNext: global_sdf_coarse_bound (#257). Only through a level with no dirty
				// volume pending (bits 8-13): a level that has not applied a move yet may call
				// empty a brick a finer level has filled.
				if ((uint(SWRT_P.extra.w + 0.5) & (256u << coarse)) == 0u || !swrt_brick_empty(coarse, p)) {
					break;
				}
				skip = max(skip, max(swrt_brick_exit(coarse, p, direction), swrt_coarse_bound(coarse, p)));
			}
			if (coarse_empty) {
				swrt_coarse_steps++;
			}
			// No surface is within a voxel of an unallocated brick.
			cleared = max(cleared, voxel_size);
			t += skip + 0.01 * voxel_size;
			continue;
		}
		// An unsigned two-sided shell is one voxel thick; a signed surface is its zero crossing.
		float band = (coverage ? 1.0 : 0.5) * voxel_size;
		cleared = max(cleared, distance);
		if (cleared < band && (coverage || distance >= 0.0)) {
			t += max(band - distance, 0.25 * voxel_size);
			continue;
		}
		if (distance < band) {
			vec3 normal = swrt_normal(level, p, distance, -direction);
			uint flags = SWRT_HIT_SURFACE | (coverage ? SWRT_HIT_COVERAGE : 0u);
			if (coverage) {
				swrt_cover_two_sided(p, direction, SWRT_NO_INSTANCE, band, normal, flags);
			}
			SWRTHit hit;
			hit.t = t;
			hit.normal = normal;
			hit.flags = flags;
			hit.instance = SWRT_NO_INSTANCE;
			hit.steps = steps;
			hit.level = level;
			return hit;
		}
		t += distance;
	}
	if (t < tmax) {
		SWRTHit hit;
		hit.t = t;
		hit.normal = -direction;
		hit.flags = SWRT_HIT_SURFACE | SWRT_HIT_EXHAUSTED;
		hit.instance = SWRT_NO_INSTANCE;
		hit.steps = steps;
		hit.level = swrt_level_at(origin + direction * t);
		return hit;
	}
	return swrt_no_hit(steps);
}

// A ray from t_start: at its origin when t_start is 0, in flight otherwise.
SWRTHit swrt_trace_global(vec3 origin, vec3 direction, float t_start, float tmax, uint steps) {
	return swrt_trace_global(origin, direction, t_start, tmax, steps, t_start > 0.0);
}

// --- Detail mode: mesh SDFs for the first 2 m --------------------------------

float swrt_mesh_voxel(SWRTMeshInfo mesh, ivec3 voxel) {
	ivec3 dimensions = max(mesh.dimensions.xyz, ivec3(1));
	ivec3 clamped = clamp(voxel, ivec3(0), dimensions - ivec3(1));
	ivec3 brick_grid = max(mesh.brick_grid.xyz, ivec3(1));
	ivec3 brick = clamped / 8;
	ivec3 local = clamped % 8;
	int entry = swrt_mesh_pages.data[mesh.offsets.x + uint(brick.x + brick_grid.x * (brick.y + brick_grid.y * brick.z))];
	float band = uintBitsToFloat(mesh.offsets.w);
	if (entry == -1) {
		return band;
	}
	if (entry == -2) {
		return -band;
	}
	uint voxel_index = uint(local.x + 8 * (local.y + 8 * local.z));
	uint packed = swrt_mesh_bricks.data[mesh.offsets.y + uint(entry) * 128u + voxel_index / 4u];
	uint encoded = (packed >> ((voxel_index % 4u) * 8u)) & 255u;
	return (float(encoded) / 255.0 * 2.0 - 1.0) * band;
}

// One mip's trilinear sample in local units. Outside the bounds it clamps;
// nextgi_mesh_sdf_world_distance() extends it.
float swrt_mesh_sample(SWRTMeshInfo mesh, vec3 local) {
	float voxel_size = mesh.bounds_voxel.w;
	ivec3 dimensions = max(mesh.dimensions.xyz, ivec3(1));
	vec3 f = (local - mesh.bounds_voxel.xyz) / voxel_size - vec3(0.5);
	f = clamp(f, vec3(0.0), vec3(dimensions - ivec3(1)));
	ivec3 base = ivec3(floor(f));
	vec3 w = f - vec3(base);
	float x00 = mix(swrt_mesh_voxel(mesh, base), swrt_mesh_voxel(mesh, base + ivec3(1, 0, 0)), w.x);
	float x10 = mix(swrt_mesh_voxel(mesh, base + ivec3(0, 1, 0)), swrt_mesh_voxel(mesh, base + ivec3(1, 1, 0)), w.x);
	float x01 = mix(swrt_mesh_voxel(mesh, base + ivec3(0, 0, 1)), swrt_mesh_voxel(mesh, base + ivec3(1, 0, 1)), w.x);
	float x11 = mix(swrt_mesh_voxel(mesh, base + ivec3(0, 1, 1)), swrt_mesh_voxel(mesh, base + ivec3(1, 1, 1)), w.x);
	return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z);
}

vec3 swrt_instance_local(SWRTInstance instance, vec3 world) {
	return vec3(
			dot(instance.inverse_row0.xyz, world) + instance.inverse_row0.w,
			dot(instance.inverse_row1.xyz, world) + instance.inverse_row1.w,
			dot(instance.inverse_row2.xyz, world) + instance.inverse_row2.w);
}

// Mip stepping: the coarsest mip steps through empty space; near a surface
// the march refines to the finest mip. Returns a lower bound in world meters
// under any instance scale (nextgi_mesh_sdf_distance_inc.glsl, shared with the
// global SDF composite); voxel_size is the sampled mip's voxel in world meters
// at the smallest singular value.
float swrt_instance_distance(uint index, vec3 world, out float voxel_size) {
	SWRTInstance instance = swrt_instances.data[index];
	vec3 local = swrt_instance_local(instance, world);
	float min_scale = nextgi_mesh_sdf_min_scale(instance.axis_scale);
	uint first = instance.flags.y;
	uint mips = max(instance.flags.z, 1u);
	// The mesh's own bounds come from mip 0: a coarser mip pads by a wider band.
	SWRTMeshInfo finest = swrt_meshes.data[first];
	vec3 finest_max = finest.bounds_voxel.xyz + vec3(max(finest.dimensions.xyz, ivec3(1))) * finest.bounds_voxel.w;
	vec3 surface_min;
	vec3 surface_max;
	nextgi_mesh_sdf_surface_bounds(finest.bounds_voxel.xyz, finest_max, uintBitsToFloat(finest.offsets.w), surface_min, surface_max);
	float distance = SWRT_FAR;
	voxel_size = 0.1;
	for (int mip = int(mips) - 1; mip >= 0; mip--) {
		SWRTMeshInfo mesh = swrt_meshes.data[first + uint(mip)];
		vec3 bounds_max = mesh.bounds_voxel.xyz + vec3(max(mesh.dimensions.xyz, ivec3(1))) * mesh.bounds_voxel.w;
		distance = nextgi_mesh_sdf_world_distance(local, mesh.bounds_voxel.xyz, bounds_max, surface_min, surface_max, swrt_mesh_sample(mesh, local), instance.axis_scale);
		voxel_size = mesh.bounds_voxel.w * min_scale;
		if (abs(distance) > 2.0 * voxel_size) {
			break;
		}
	}
	return distance;
}

vec3 swrt_instance_normal(uint index, vec3 world, float voxel_size, vec3 fallback) {
	vec3 gradient = vec3(0.0);
	for (uint axis = 0u; axis < 3u; axis++) {
		vec3 offset = vec3(0.0);
		offset[axis] = voxel_size;
		float unused;
		gradient[axis] = swrt_instance_distance(index, world + offset, unused) - swrt_instance_distance(index, world - offset, unused);
	}
	return dot(gradient, gradient) > 1e-12 ? normalize(gradient) : fallback;
}

// The number of instances the cull found for a froxel cell (it can exceed
// SWRT_FROXEL_CAP; only the first SWRT_FROXEL_CAP are listed).
uint swrt_froxel_found(uint froxel_cell) {
	if (froxel_cell == 0xffffffffu) {
		return 0u;
	}
	return swrt_froxel.data[froxel_cell * (SWRT_FROXEL_CAP + 1u)] & SWRT_FROXEL_COUNT_MASK;
}

// The detail range of a ray: mesh SDFs of the ray's froxel cell for the first
// 2 m, then the global trace continues from there. A cell that lists nothing
// leaves the whole ray to the global SDF from t = 0. A cell whose cull found
// more instances than it lists (the cap) cannot vouch for the detail range:
// the global SDF also traces it from t = 0, and a global hit more than one
// voxel of its level before the detail hit is geometry the list left out and
// wins. A global hit that ran out of steps does not override a detail hit.
// resume is how far origin sits past the surface the ray left (the screen
// march's resume distance, #186 step 4; 0 for a ray that starts there). The
// cell lists the instances near that surface, so the detail range ends 2 m
// from it, not from origin, and a resumed ray is in flight (no escape rule).
SWRTHit swrt_trace_detail(vec3 origin, vec3 direction, float tmax, uint froxel_cell, float resume) {
	bool in_flight = resume > 0.0;
	uint found_instances = swrt_froxel_found(froxel_cell);
	float limit = min(tmax, SWRT_DETAIL_METERS - resume);
	if (found_instances == 0u || !(limit > 0.0)) {
		return swrt_trace_global(origin, direction, 0.0, tmax, 0u, in_flight);
	}
	uint count = min(found_instances, SWRT_FROXEL_CAP);
	bool overflow = found_instances > SWRT_FROXEL_CAP;
	uint max_steps = uint(max(SWRT_P.trace.y, 1.0));
	uint steps = 0u;
	float t = 0.0;
	bool first = !in_flight;
	bool escaping = false;
	bool detail_found = false;
	SWRTHit detail_hit = swrt_no_hit(0u);
	while (steps < max_steps && t < limit) {
		vec3 p = origin + direction * t;
		steps++;
		float best = SWRT_FAR;
		float best_voxel = 0.1;
		uint best_instance = SWRT_NO_INSTANCE;
		for (uint i = 0u; i < count; i++) {
			uint index = swrt_froxel.data[froxel_cell * (SWRT_FROXEL_CAP + 1u) + 1u + i];
			float voxel_size;
			float distance = swrt_instance_distance(index, p, voxel_size);
			if (distance < best) {
				best = distance;
				best_voxel = voxel_size;
				best_instance = index;
			}
		}
		bool two_sided = best_instance != SWRT_NO_INSTANCE && (swrt_instances.data[best_instance].flags.x & 2u) != 0u;
		float band = (two_sided ? 1.0 : 0.5) * best_voxel;
		// The same escape rule as the global march: a mesh SDF coarser than the
		// origin offset must not report the surface the ray starts on.
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
		if (best < band) {
			vec3 normal = swrt_instance_normal(best_instance, p, best_voxel, -direction);
			uint flags = SWRT_HIT_SURFACE | SWRT_HIT_DETAIL | (two_sided ? SWRT_HIT_COVERAGE : 0u);
			if (two_sided) {
				swrt_cover_two_sided(p, direction, best_instance, 0.0, normal, flags);
			}
			detail_hit.t = t;
			detail_hit.normal = normal;
			detail_hit.flags = flags;
			detail_hit.instance = best_instance;
			detail_hit.steps = steps;
			detail_hit.level = SWRT_NO_LEVEL;
			detail_found = true;
			break;
		}
		t += max(best, 0.25 * best_voxel);
	}
	if (overflow) {
		// The global SDF covers the detail range too (see above).
		float global_tmax = tmax;
		if (detail_found) {
			uint level = swrt_level_at(origin + direction * detail_hit.t);
			global_tmax = detail_hit.t - (level == SWRT_NO_LEVEL ? 0.0 : swrt_level_voxel(level));
		}
		SWRTHit global_hit = swrt_trace_global(origin, direction, 0.0, global_tmax, 0u, in_flight);
		if (!detail_found || ((global_hit.flags & SWRT_HIT_SURFACE) != 0u && (global_hit.flags & SWRT_HIT_EXHAUSTED) == 0u)) {
			global_hit.steps += steps;
			return global_hit;
		}
		detail_hit.steps = steps + global_hit.steps;
		return detail_hit;
	}
	if (detail_found) {
		return detail_hit;
	}
	if (t < limit && steps >= max_steps) {
		SWRTHit hit;
		hit.t = t;
		hit.normal = -direction;
		hit.flags = SWRT_HIT_SURFACE | SWRT_HIT_EXHAUSTED | SWRT_HIT_DETAIL;
		hit.instance = SWRT_NO_INSTANCE;
		hit.steps = steps;
		hit.level = SWRT_NO_LEVEL;
		return hit;
	}
	// Every instance near the cell is listed: the global trace continues past the detail range.
	return swrt_trace_global(origin, direction, min(t, limit), tmax, steps, true);
}

// resume is how far origin sits past the surface the ray left: 0 for a ray that starts on it,
// the screen march's resume distance for one continued after it (#186 step 4).
SWRTHit swrt_trace(vec3 origin, vec3 direction, float tmax, bool detail, uint froxel_cell, float resume) {
	if (!(tmax > 0.0) || !(dot(direction, direction) > 1e-12)) {
		return swrt_no_hit(0u);
	}
	vec3 d = normalize(direction);
	if (detail) {
		return swrt_trace_detail(origin, d, tmax, froxel_cell, resume);
	}
	return swrt_trace_global(origin, d, 0.0, tmax, 0u, resume > 0.0);
}

SWRTHit swrt_trace(vec3 origin, vec3 direction, float tmax, bool detail, uint froxel_cell) {
	return swrt_trace(origin, direction, tmax, detail, froxel_cell, 0.0);
}

// The software twin of nextgi_shadow_ray (nextgi_src_relight.glsl): true when
// something is in the way within tmax. The caller offsets the origin.
bool swrt_shadow_ray(vec3 origin, vec3 direction, float tmax) {
	SWRTHit hit = swrt_trace_global(origin, normalize(direction), 0.0, tmax, 0u);
	return (hit.flags & SWRT_HIT_SURFACE) != 0u;
}

// --- Shading -----------------------------------------------------------------

#ifndef SWRT_EXTERNAL_VIEW_INPUTS
vec3 swrt_miss_radiance(vec3 direction) {
	SWRTParams params = swrt_params();
	if (params.ambient.w > 0.5) {
		return textureLod(swrt_sky, vec3_to_oct(normalize(direction)), 0.0).rgb;
	}
	return params.ambient.rgb;
}

// Radiance along a traced ray. A surface hit reads voxel lighting; an
// uncovered hit and a miss read the WRC, then the sky. A detail hit keeps its
// instance in SWRTHit.instance (SWRTInstance.flags.w is its SRC card set) for
// the D6 card lookup, which needs SRC pages on this tier.
vec3 swrt_shade(inout SWRTHit hit, vec3 origin, vec3 direction, bool allow_wrc, float tmax) {
	SWRTParams params = swrt_params();
	bool wrc_valid = allow_wrc && params.flags.z > 0.5;
	if ((hit.flags & SWRT_HIT_SURFACE) != 0u) {
		vec3 world = origin + direction * hit.t;
		vec3 fallback = params.ambient.rgb;
		if (wrc_valid) {
			vec3 cached = nextgi_wrc_sample(world, -direction);
			if (nextgi_wrc_hit_cache) {
				fallback = cached;
			}
		}
		if (params.flags.x > 0.5) {
			NextGIVoxelClipmap clipmap;
			clipmap.origin_size[0] = params.clipmap[0];
			clipmap.origin_size[1] = params.clipmap[1];
			clipmap.origin_size[2] = params.clipmap[2];
			clipmap.origin_size[3] = params.clipmap[3];
			float weight;
			vec3 color = nextgi_voxel_lighting_lookup(world, hit.normal, fallback, clipmap, weight);
			if (weight >= NEXTGI_VOXEL_WEIGHT_FALLBACK) {
				hit.flags |= SWRT_HIT_VOXEL;
			}
			return max(color, vec3(0.0));
		}
		return max(fallback, vec3(0.0));
	}
	if (wrc_valid) {
		vec3 cached = nextgi_wrc_sample(origin + direction * max(tmax, 0.0), direction);
		if (nextgi_wrc_hit_cache) {
			hit.flags |= SWRT_HIT_WRC;
			return max(cached, vec3(0.0));
		}
	}
	hit.flags |= SWRT_HIT_SKY;
	return max(swrt_miss_radiance(direction), vec3(0.0));
}
#endif // !SWRT_EXTERNAL_VIEW_INPUTS

// The hardware tier's handoff distance, 1.5x the WRC probe spacing at p
// (nextgi_wrc_handoff), whether or not the WRC holds data: the screen trace
// covers this range on both tiers.
float swrt_screen_range(vec3 p) {
	float distance = length(p - SWRT_P.camera.xyz);
	float spacing = distance > 32.0 ? 8.0 : (distance > 16.0 ? 4.0 : (distance > 8.0 ? 2.0 : 1.0));
	return 1.5 * spacing;
}

// --- View ----------------------------------------------------------------------

#ifndef SWRT_EXTERNAL_VIEW_INPUTS
vec3 swrt_reconstruct(ivec2 pixel, float depth) {
	vec2 screen = max(swrt_params().screen.xy, vec2(1.0));
	return nextgi_view_reconstruct(swrt_params().inv_view_proj, (vec2(pixel) + vec2(0.5)) / screen, depth);
}

// The prepass normal when the normal-roughness buffer is bound, else the
// depth-derived geometric normal facing the camera.
vec3 swrt_surface_normal(ivec2 pixel, vec3 world, bool has_normal_roughness) {
	if (has_normal_roughness) {
		vec3 encoded = texelFetch(swrt_normal_roughness, pixel, 0).xyz;
		if (dot(encoded, encoded) > 1e-6) {
			mat3 view_to_world = mat3(SWRT_P.view_to_world[0].xyz, SWRT_P.view_to_world[1].xyz, SWRT_P.view_to_world[2].xyz);
			return nextgi_view_world_normal(view_to_world, encoded);
		}
	}
	ivec2 size = max(ivec2(swrt_params().screen.xy), ivec2(1));
	ivec2 px = clamp(pixel + ivec2(1, 0), ivec2(0), size - ivec2(1));
	ivec2 py = clamp(pixel + ivec2(0, 1), ivec2(0), size - ivec2(1));
	vec3 wx = swrt_reconstruct(px, texelFetch(swrt_depth, px, 0).r) - world;
	vec3 wy = swrt_reconstruct(py, texelFetch(swrt_depth, py, 0).r) - world;
	if (px.x == pixel.x) {
		wx = world - swrt_reconstruct(pixel - ivec2(1, 0), texelFetch(swrt_depth, pixel - ivec2(1, 0), 0).r);
	}
	if (py.y == pixel.y) {
		wy = world - swrt_reconstruct(pixel - ivec2(0, 1), texelFetch(swrt_depth, pixel - ivec2(0, 1), 0).r);
	}
	vec3 normal = cross(wx, wy);
	vec3 to_camera = swrt_params().camera.xyz - world;
	if (dot(normal, normal) <= 1e-12) {
		return normalize(to_camera);
	}
	normal = normalize(normal);
	return dot(normal, to_camera) < 0.0 ? -normal : normal;
}
#endif // !SWRT_EXTERNAL_VIEW_INPUTS

// --- Detail froxel grid ---------------------------------------------------------

// The froxel of a world position: screen tiles by depth slices (exponential
// between froxel_depth.x and froxel_depth.y meters).
uint swrt_froxel_cell(vec3 world) {
	SWRTParams params = swrt_params();
	uvec3 cells = uvec3(params.froxel.xyz);
	if (any(equal(cells, uvec3(0u)))) {
		return 0xffffffffu;
	}
	vec3 screen = nextgi_view_project(params.view_proj, world);
	if (screen.z < 0.0 || any(lessThan(screen.xy, vec2(0.0))) || any(greaterThanEqual(screen.xy, vec2(1.0)))) {
		return 0xffffffffu;
	}
	float linear = nextgi_view_linear_depth(params.depth_coefficients, screen.z);
	float near = max(params.froxel_depth.x, 1e-3);
	float far = max(params.froxel_depth.y, near * 1.001);
	float slice = log(max(linear, near) / near) / log(far / near);
	if (slice >= 1.0) {
		return 0xffffffffu;
	}
	uvec3 cell = min(uvec3(vec3(screen.xy, slice) * vec3(cells)), cells - uvec3(1u));
	return cell.x + cells.x * (cell.y + cells.y * cell.z);
}

#endif // NEXTGI_SDF_TRACE_INC_GLSL
