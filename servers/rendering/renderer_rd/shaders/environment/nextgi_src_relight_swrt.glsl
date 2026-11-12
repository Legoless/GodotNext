#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: ps_swrt_trace (#186, #188). The software tier's sibling of nextgi_src_relight.glsl:
// the same SRC world-space relight kernel (nextgi_src_relight_inc.glsl, #31, D14, §2.1 line 2b)
// without a ray query, so this ShaderRD compiles on every Forward+ driver. Shadow rays toward a
// positional light march the mesh SDFs of the instances inside that light's range (#186 step 5,
// plan §21.2 L5 and §21.2.6, as UE traces off-screen shadows against mesh SDFs); directional rays
// and positional rays the candidates cannot vouch for march the global SDF. Set 0 is the hardware
// relight's set 0 with the per-light candidates in place of the TLAS (0); set 1 holds the trace's
// resources (1-12, 21-26).

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF
#define SWRT_SET 1
#define SWRT_EXTERNAL_VIEW_INPUTS

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../half_inc.glsl"

// The hit record, without the ray-query include.
#include "nextgi_hit_inc.glsl"

// The instance records, cards and lights.
#include "nextgi_records_inc.glsl"

// The global-SDF trace in set 1.
#include "nextgi_sdf_trace_inc.glsl"

// Per light of the relight's light buffer: data[2 * light] is the offset of its candidate list in
// data[], data[2 * light + 1] its length, or SWRT_LIGHT_CANDIDATES_OVERFLOW when more instances
// overlap the light's range than the list holds (NextGIRelight::build_light_candidates).
layout(set = 0, binding = 0, std430) readonly buffer SWRTLightCandidates {
	uint data[];
}
light_candidates;

#define SWRT_LIGHT_CANDIDATES_OVERFLOW 0xffffffffu
// Candidates one ray marches: those whose mesh-SDF bounds the ray's segment enters, nearest first.
// A ray that enters more falls back to the global SDF. Start value, the detail froxel cap.
#define SWRT_SHADOW_MESH_CAP 8u
// Steps of the mesh march. A ray that closes on a surface shrinks its step to a quarter of a mesh
// voxel; 192 steps cover the ceiling fixture's longest floor ray (about 14 m). A ray that runs out
// of steps continues in the global SDF from where it stopped. Start value.
#define SWRT_SHADOW_MESH_STEPS 192u

// The software twin of the hardware relight's opaque shadow ray: true when something is in the
// way within tmax. A ray that runs out of steps reports a hit, so it counts as occluded.
bool nextgi_shadow_ray(vec3 origin, vec3 direction, float tmax) {
	return swrt_shadow_ray(origin, direction, tmax);
}

// Enter t of the segment [0, tmax] against an AABB, or -1 when it misses.
float swrt_segment_aabb_enter(vec3 origin, vec3 direction, float tmax, vec3 bmin, vec3 bmax) {
	float t0 = 0.0;
	float t1 = tmax;
	for (int axis = 0; axis < 3; axis++) {
		float component = direction[axis];
		if (abs(component) <= 1e-8) {
			if (origin[axis] < bmin[axis] || origin[axis] > bmax[axis]) {
				return -1.0;
			}
			continue;
		}
		float inv = 1.0 / component;
		float near_t = (bmin[axis] - origin[axis]) * inv;
		float far_t = (bmax[axis] - origin[axis]) * inv;
		t0 = max(t0, min(near_t, far_t));
		t1 = min(t1, max(near_t, far_t));
		if (t0 > t1) {
			return -1.0;
		}
	}
	return t0;
}

// Marches the mesh SDFs of light light_index's candidates whose bounds the segment enters. Returns
// 1 when one occludes, 0 when none does over the whole segment, and -1 when the candidates cannot
// vouch for the ray (the light's list overflowed, the segment enters more than
// SWRT_SHADOW_MESH_CAP of them, or the march ran out of steps; r_from is then where the global SDF
// continues). Every instance that occludes a point inside the light's range overlaps that range,
// and a relit texel lies inside it, so a list that did not overflow holds every possible occluder.
//
// A signed mesh occludes within a quarter voxel of its surface, and only inside the solid
// (distance <= 0) for the last half voxel before the light: a light 5 cm from the surface it
// hangs on lies inside that surface's half-voxel band when the mesh is coarse (a 20 m plate at
// 128 voxels has 15.6 cm voxels), but it is outside the solid. A two-sided shell has no inside,
// so it occludes within its band, from either side (#186 step 6, plan §21.2.6).
int swrt_shadow_mesh_list(vec3 origin, vec3 direction, float tmax, uint light_index, out float r_from) {
	r_from = 0.0;
	uint offset = light_candidates.data[2u * light_index];
	uint count = light_candidates.data[2u * light_index + 1u];
	if (count == SWRT_LIGHT_CANDIDATES_OVERFLOW) {
		return -1;
	}
	uint selected[SWRT_SHADOW_MESH_CAP];
	float selected_enter[SWRT_SHADOW_MESH_CAP];
	uint selected_count = 0u;
	for (uint i = 0u; i < count; i++) {
		uint index = light_candidates.data[offset + i];
		SWRTInstance instance = swrt_instances.data[index];
		float enter = swrt_segment_aabb_enter(origin, direction, tmax, instance.world_min.xyz, instance.world_max.xyz);
		if (enter < 0.0) {
			continue;
		}
		if (selected_count == SWRT_SHADOW_MESH_CAP) {
			return -1;
		}
		uint slot = selected_count;
		while (slot > 0u && enter < selected_enter[slot - 1u]) {
			selected[slot] = selected[slot - 1u];
			selected_enter[slot] = selected_enter[slot - 1u];
			slot--;
		}
		selected[slot] = index;
		selected_enter[slot] = enter;
		selected_count++;
	}
	if (selected_count == 0u) {
		return 0;
	}
	// The march starts where the first candidate's bounds begin. The escape rule below applies only
	// when that is the origin: a ray that starts on a candidate's bounds face is not leaving it.
	float t = selected_enter[0];
	bool first = t <= 0.0;
	bool escaping = false;
	for (uint steps = 0u; steps < SWRT_SHADOW_MESH_STEPS; steps++) {
		if (t >= tmax) {
			return 0;
		}
		vec3 p = origin + direction * t;
		float best = SWRT_FAR;
		float best_voxel = 0.1;
		bool two_sided = false;
		for (uint i = 0u; i < selected_count; i++) {
			if (selected_enter[i] > t + best) {
				break;
			}
			float voxel_size;
			float distance = swrt_instance_distance(selected[i], p, voxel_size);
			if (distance < best) {
				best = distance;
				best_voxel = voxel_size;
				two_sided = (swrt_instances.data[selected[i]].flags.x & 2u) != 0u;
			}
		}
		float band = (two_sided ? 1.0 : 0.5) * best_voxel;
		// The origin's own surface is not a hit until a sample clears its band (the global march's
		// escape rule).
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
		// A signed mesh occludes within a quarter voxel of its surface, which keeps a ray that only
		// clips a rounded corner occluded; in the last half voxel before the light it occludes only
		// inside the solid, so the surface the light hangs on does not shadow it.
		float hit_band = (tmax - t) > 0.5 * best_voxel ? 0.25 * best_voxel : 0.0;
		if (two_sided ? best < band : best <= hit_band) {
			return 1;
		}
		t += max(best, 0.25 * best_voxel);
	}
	r_from = t;
	return -1;
}

// A shadow ray toward light light_index of type light_type (0 = directional).
bool nextgi_shadow_ray_to_light(vec3 origin, vec3 direction, float tmax, uint light_type, uint light_index) {
	if (light_type == 0u || !(tmax > 0.0)) {
		return swrt_shadow_ray(origin, direction, tmax);
	}
	float from_t;
	int mesh = swrt_shadow_mesh_list(origin, direction, tmax, light_index, from_t);
	if (mesh >= 0) {
		return mesh == 1;
	}
	SWRTHit hit = swrt_trace_global(origin, direction, min(from_t, tmax), tmax, 0u);
	return (hit.flags & SWRT_HIT_SURFACE) != 0u;
}

// The shared kernel.
#include "nextgi_src_relight_inc.glsl"
