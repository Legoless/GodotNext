#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: ps_swrt_trace (#186, #188). The software tier's sibling of nextgi_src_relight.glsl:
// the same SRC world-space relight kernel (nextgi_src_relight_inc.glsl, #31, D14, §2.1 line 2b),
// whose remaining shadow rays march the global SDF (#186 step 5) instead of a ray query. No ray
// query, so this ShaderRD compiles on every Forward+ driver. Set 0 is the hardware relight's
// set 0 without the TLAS (0); set 1 holds the trace's resources (1-12, 21-26).

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

// The software twin of the hardware relight's opaque shadow ray: true when something is in the
// way within tmax. A ray that runs out of steps reports a hit, so it counts as occluded.
bool nextgi_shadow_ray(vec3 origin, vec3 direction, float tmax) {
	return swrt_shadow_ray(origin, direction, tmax);
}

// The shared kernel.
#include "nextgi_src_relight_inc.glsl"
