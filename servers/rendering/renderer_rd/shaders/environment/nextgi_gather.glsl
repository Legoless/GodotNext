#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// HWRT diffuse gather (#25). 8x8, no workgroup memory. One opaque proceed per query,
// via nextgi_trace. Arena positions are read from the chunk storage buffers
// (chunk index + byte offset). SPIRV-Cross rejects physical buffer addresses.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
// GodotNext: ps_swrt_trace (#186). The shared parts live in includes; nextgi_gather_swrt.glsl
// is the software tier's sibling of this file.
#include "nextgi_gather_wrc_bindings_inc.glsl"
#ifdef MODE_FAR_FIELD
layout(set = 0, binding = 39) uniform accelerationStructureEXT far_tlas;
#define NEXTGI_TLAS far_tlas
#else
#define NEXTGI_TLAS nextgi_tlas
#endif
#define NEXTGI_WRC
#include "nextgi_rt_inc.glsl"
// Bindings 1-25 after the ray-query include, which declares the records they hold.
#include "nextgi_gather_resources_inc.glsl"

#ifdef MODE_HIT_RADIANCE_REFERENCE
#include "nextgi_reference_pt_inc.glsl"
#endif

// GodotNext: nextgi_hit_lighting_gi (#128). Epic-only diffuse GI hit marking: writes hits before
// 1.5x WRC handoff into the shared hit-lighting record buffer using the disjoint GI key range.
#ifdef MODE_HIT_LIGHTING_GI
#include "nextgi_hit_lighting_common_inc.glsl"

struct NextGIHitSurfaceGI {
	uint attribute_chunk;
	uint attribute_offset;
	uint material_id;
	uint flags;
};

struct NextGIHitMaterialGI {
	uint params_slot;
	uint texture_base;
	uint texture_count;
	uint shader_slot;
};

layout(set = 0, binding = 40, std430) restrict readonly buffer GatherHitSurfaces {
	NextGIHitSurfaceGI data[];
}
gi_hit_surfaces;

layout(set = 0, binding = 41, std430) restrict readonly buffer GatherHitMaterials {
	NextGIHitMaterialGI data[];
}
gi_hit_materials;

layout(set = 0, binding = 42, std430) restrict buffer GatherHitRecordsOut {
	NextGIHitLightRecord data[];
}
gi_hit_records_out;

layout(set = 0, binding = 43, std430) restrict buffer GatherHitCounters {
	NextGIHitLightCounters data;
}
gi_hl_counters;
#endif

#include "nextgi_gather_body_inc.glsl"
