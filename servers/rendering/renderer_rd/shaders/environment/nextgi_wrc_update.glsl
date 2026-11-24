#[compute]

#version 460

#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// World Radiance Cache update (#44, D15). 8x8, one probe per workgroup, one opaque proceed per ray.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

// GodotNext: nextgi_world_cache (#294). Sparse world-space lighting cache replacing SRC on HWRT.
#ifdef MODE_WORLD_CACHE
#include "nextgi_world_cache_common_inc.glsl"

layout(set = 0, binding = 36, std430) restrict readonly buffer WRCWCKeys {
	uint data[];
}
wc_keys;

layout(set = 0, binding = 37, std430) restrict readonly buffer WRCWCRadiance {
	vec4 data[];
}
wc_radiance;

bool nextgi_wrc_wc_lookup(vec3 p_world, vec3 p_normal, vec3 p_camera_pos, out vec3 r_radiance) {
	r_radiance = vec3(0.0);
	float dist = length(p_world - p_camera_pos);
	uint base_level = nextgi_wc_level_for_distance(dist);
	uint face = nextgi_wc_normal_face(p_normal);
	for (uint l = 0u; l < 2u; l++) {
		uint level = base_level + l;
		if (level >= NEXTGI_WC_LEVELS) {
			break;
		}
		ivec3 cell = nextgi_wc_quantize(p_world, level);
		uint key = nextgi_wc_make_key(cell, level, face);
		uint base_slot = nextgi_wc_hash_bucket(cell, level, face) * NEXTGI_WC_SLOTS_PER_BUCKET;
		for (uint w = 0u; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
			if (wc_keys.data[base_slot + w] == key) {
				vec4 rad = wc_radiance.data[base_slot + w];
				if (rad.a > 0.0) {
					r_radiance = rad.rgb;
					return true;
				}
				return false;
			}
		}
	}
	return false;
}
#endif

// GodotNext: ps_swrt_trace (#186). The kernels live in nextgi_wrc_update_inc.glsl;
// nextgi_wrc_update_swrt.glsl is the software tier's sibling of this file.
#include "nextgi_wrc_update_inc.glsl"
