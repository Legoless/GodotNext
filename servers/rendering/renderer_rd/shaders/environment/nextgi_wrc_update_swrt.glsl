#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: ps_swrt_trace (#186). The software tier's sibling of nextgi_wrc_update.glsl (plan
// §21.2 L9b: the WRC traces with the active tier): the same probe trace, SH projection and
// translucency kernels (nextgi_wrc_update_inc.glsl), with nextgi_trace() from the global-SDF trace
// and the probe-ray hits shaded from voxel lighting. No ray query. Set 0 is the hardware update's
// set 0 without the TLAS (0) and the arena and D6 inputs (9-17); set 1 holds the trace's
// resources (1-12, 21-26).

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF
#define SWRT_SET 1
#define SWRT_EXTERNAL_VIEW_INPUTS

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../half_inc.glsl"

// The hit record and the records, without the ray-query include.
#include "nextgi_hit_inc.glsl"

// The instance records, cards and lights.
#include "nextgi_records_inc.glsl"

// The WRC lookup globals (the update writes the cache, it does not sample it).
#include "nextgi_wrc_sample_inc.glsl"

// The global-SDF trace in set 1.
#include "nextgi_sdf_trace_inc.glsl"

// nextgi_trace() for the shared kernels.
#include "nextgi_sdf_hit_inc.glsl"

// The probe ray's hit, shaded from voxel lighting. False below the fallback weight: the caller
// keeps the probe's previous radiance, as the hardware tier does on a D6 miss.
bool nextgi_src_lookup(NextGIHit hit, out vec3 radiance, out bool uncovered) {
	radiance = vec3(0.0);
	uncovered = false;
	if (!hit.hit || !(SWRT_P.flags.x > 0.5)) {
		return false;
	}
	NextGIVoxelClipmap clipmap;
	clipmap.origin_size[0] = SWRT_P.clipmap[0];
	clipmap.origin_size[1] = SWRT_P.clipmap[1];
	clipmap.origin_size[2] = SWRT_P.clipmap[2];
	clipmap.origin_size[3] = SWRT_P.clipmap[3];
	float weight;
	vec3 voxel = nextgi_voxel_lighting_lookup(nextgi_sdf_origin + nextgi_sdf_direction * hit.t, nextgi_sdf_last.normal, vec3(0.0), clipmap, weight);
	if (!(weight >= NEXTGI_VOXEL_WEIGHT_FALLBACK)) {
		return false;
	}
	radiance = max(voxel, vec3(0.0));
	return true;
}

// The shared kernels.
#include "nextgi_wrc_update_inc.glsl"
