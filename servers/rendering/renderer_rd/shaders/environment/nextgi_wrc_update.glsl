#[compute]

#version 460

#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// World Radiance Cache update (#44, D15). 8x8, one probe per workgroup, one opaque proceed per ray.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"
// GodotNext: ps_swrt_trace (#186). The kernels live in nextgi_wrc_update_inc.glsl;
// nextgi_wrc_update_swrt.glsl is the software tier's sibling of this file.
#include "nextgi_wrc_update_inc.glsl"
