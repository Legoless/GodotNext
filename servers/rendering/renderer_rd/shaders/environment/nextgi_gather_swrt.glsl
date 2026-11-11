#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: ps_swrt_trace (#186). The software tier's sibling of nextgi_gather.glsl: the same
// gather, integrate, SRC feedback, coverage and reflect kernels (nextgi_gather_body_inc.glsl),
// with nextgi_trace() from the global-SDF trace (nextgi_sdf_hit_inc.glsl) instead of a ray query
// and the voxel-lighting hit shading. No ray query, so this ShaderRD compiles on every Forward+
// driver. Set 0 is the hardware gather's set 0 without the TLAS (0), the arena (9-13), the
// reference (23-25) and the far-field TLAS (39); set 1 holds the trace's resources (1-12, 21-26).

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF
#define SWRT_SET 1
#define SWRT_EXTERNAL_VIEW_INPUTS

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../half_inc.glsl"

// The RD header builder pastes an include once, at its first occurrence, so the includes the
// SDF trace leaves to its includer come first.
#include "nextgi_gather_wrc_bindings_inc.glsl"

#define NEXTGI_WRC
#include "nextgi_hit_inc.glsl"

// The instance records, cards and lights, without the ray-query include.
#include "nextgi_records_inc.glsl"

// The WRC lookups, against the gather's WRC bindings (26-28).
#include "nextgi_wrc_sample_inc.glsl"

// The Hi-Z screen trace on the gather's Hi-Z and last-frame bindings (30, 31).
#include "nextgi_view_inc.glsl"
#define NEXTGI_SCREEN_HIZ_TEX hiz_tex
#define NEXTGI_SCREEN_LAST_FRAME_TEX last_frame_tex
#include "nextgi_screen_trace_inc.glsl"

// The global-SDF trace in set 1.
#include "nextgi_sdf_trace_inc.glsl"

// nextgi_trace() for the shared kernels.
#include "nextgi_sdf_hit_inc.glsl"

// The gather's set-0 resources (bindings 1-8, 14-20).
#include "nextgi_gather_resources_inc.glsl"

// The SWRT-only counters, after a gather or reflection trace: SDF hits, the step-limit
// (exhausted) and near-origin ones among them, clean detail hits and the hit distance (cm).
void nextgi_sdf_note_gather(bool p_reflect) {
	if ((nextgi_sdf_last.flags & SWRT_HIT_SURFACE) == 0u) {
		return;
	}
	bool exhausted = (nextgi_sdf_last.flags & SWRT_HIT_EXHAUSTED) != 0u;
	bool near = nextgi_sdf_last_near();
	if (p_reflect) {
		atomicAdd(reflect_counters.sdf_hits, 1u);
		if (exhausted) {
			atomicAdd(reflect_counters.sdf_exhausted, 1u);
		}
		if (near) {
			atomicAdd(reflect_counters.sdf_near, 1u);
		}
		return;
	}
	atomicAdd(counters.sdf_hits, 1u);
	if (exhausted) {
		atomicAdd(counters.sdf_exhausted, 1u);
	}
	if (near) {
		atomicAdd(counters.sdf_near, 1u);
	}
	if ((nextgi_sdf_last.flags & SWRT_HIT_DETAIL) != 0u && nextgi_sdf_last.instance != SWRT_NO_INSTANCE) {
		atomicAdd(counters.sdf_detail_hits, 1u);
	}
	atomicAdd(counters.sdf_hit_cm_sum, uint(min(nextgi_sdf_last.t, 100.0) * 100.0));
}

// A gather or reflection ray: the detail froxel of its surface, the trace and its counters.
bool nextgi_sdf_trace_noted(vec3 p_origin, vec3 p_direction, float p_tmax, vec3 p_surface, bool p_reflect, out NextGIHit r_hit) {
	nextgi_sdf_surface = p_surface;
	nextgi_sdf_detail_ray = true;
	bool hit = nextgi_trace(p_origin, p_direction, p_tmax, r_hit);
	nextgi_sdf_detail_ray = false;
	nextgi_sdf_note_gather(p_reflect);
	return hit;
}

// The coverage counter (debug/coverage_counter): voxel lighting resolves the last hit.
bool nextgi_sdf_covered() {
	if (!(SWRT_P.flags.x > 0.5) || (nextgi_sdf_last.flags & SWRT_HIT_SURFACE) == 0u) {
		return false;
	}
	NextGIVoxelClipmap clipmap;
	clipmap.origin_size[0] = SWRT_P.clipmap[0];
	clipmap.origin_size[1] = SWRT_P.clipmap[1];
	clipmap.origin_size[2] = SWRT_P.clipmap[2];
	clipmap.origin_size[3] = SWRT_P.clipmap[3];
	float weight;
	nextgi_voxel_lighting_lookup(nextgi_sdf_origin + nextgi_sdf_direction * nextgi_sdf_last.t, nextgi_sdf_last.normal, vec3(0.0), clipmap, weight);
	return weight >= NEXTGI_VOXEL_WEIGHT_FALLBACK;
}

// The shared kernels.
#include "nextgi_gather_body_inc.glsl"
