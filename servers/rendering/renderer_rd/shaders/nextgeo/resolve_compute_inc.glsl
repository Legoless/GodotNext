// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7)
// The compute form of the Stage A/B resolve (nextgeo/resolve_compute.glsl). The Forward+ fragment stage
// includes this in place of its outputs when NEXTGEO_RESOLVE_COMPUTE is defined: the outputs stay
// globals that fragment_shader() fills, and main() stores them with nextgeo_resolve_compute_store() at
// its bin pixel. The resolve set (set 2) gains the bin state and one storage image per output target;
// the targets are the tile quads' attachments, so the compute form adds no target. The image formats
// are the targets' formats (RenderBufferDataForwardClustered, RenderSceneBuffersRD): a target of another
// format keeps the tile quads (NextGeoResolve::compute_target_storable()).

#include "shading_bins_inc.glsl"

// Per bin: count, offset into the pixel list (set 2, binding 7), scatter cursor, unused.
layout(set = 2, binding = 9, std430) restrict readonly buffer NextGeoShadingBins {
	uint data[];
}
nextgeo_shading_bins;

// ResolvePush.tile_list_base carries these flags in the compute form.
const uint NEXTGEO_RESOLVE_COMPUTE_SKIP_VELOCITY = 1u << 0u; // The zero-velocity write mask of the tile quads.

#ifdef MODE_RENDER_DEPTH

// Stage A: the guides.
vec4 normal_roughness_output_buffer;
layout(rgba8, set = 2, binding = 10) uniform restrict writeonly image2D nextgeo_out_normal_roughness;
#ifdef MODE_MEGALIGHTS_INPUTS
vec4 ml_albedo_buffer;
vec4 ml_material_buffer;
layout(rgba8, set = 2, binding = 11) uniform restrict writeonly image2D nextgeo_out_ml_albedo;
layout(rgba8, set = 2, binding = 12) uniform restrict writeonly image2D nextgeo_out_ml_material;
#endif
#ifdef MODE_RENDER_VOXEL_GI
uvec2 voxel_gi_buffer;
layout(rg8ui, set = 2, binding = 13) uniform restrict writeonly uimage2D nextgeo_out_voxel_gi;
#endif
uint nextgeo_instance_id;
layout(r32ui, set = 2, binding = 14) uniform restrict writeonly uimage2D nextgeo_out_instance_id;
#ifdef MOTION_VECTORS
// GodotNext: nextgeo_vertex_offset (#164). Stage A velocity output for WPO buckets.
vec2 motion_vector;
layout(rg16f, set = 2, binding = 15) uniform restrict writeonly image2D nextgeo_out_velocity;
#endif

#else

// Stage B: the opaque colour.
#ifdef MODE_SEPARATE_SPECULAR
vec4 diffuse_buffer;
vec4 specular_buffer;
layout(rgba16f, set = 2, binding = 10) uniform restrict writeonly image2D nextgeo_out_color;
layout(rgba16f, set = 2, binding = 11) uniform restrict writeonly image2D nextgeo_out_specular;
#else
vec4 frag_color;
layout(rgba16f, set = 2, binding = 10) uniform restrict writeonly image2D nextgeo_out_color;
#endif
#ifdef MOTION_VECTORS
vec2 motion_vector;
layout(rg16f, set = 2, binding = 12) uniform restrict writeonly image2D nextgeo_out_velocity;
#endif

#endif // MODE_RENDER_DEPTH

void nextgeo_resolve_compute_store(ivec2 p_px) {
#ifdef MODE_RENDER_DEPTH
	imageStore(nextgeo_out_normal_roughness, p_px, normal_roughness_output_buffer);
#ifdef MODE_MEGALIGHTS_INPUTS
	imageStore(nextgeo_out_ml_albedo, p_px, ml_albedo_buffer);
	imageStore(nextgeo_out_ml_material, p_px, ml_material_buffer);
#endif
#ifdef MODE_RENDER_VOXEL_GI
	imageStore(nextgeo_out_voxel_gi, p_px, uvec4(voxel_gi_buffer, 0u, 0u));
#endif
	imageStore(nextgeo_out_instance_id, p_px, uvec4(nextgeo_instance_id, 0u, 0u, 0u));
#ifdef MOTION_VECTORS
	if ((resolve_push.tile_list_base & NEXTGEO_RESOLVE_COMPUTE_SKIP_VELOCITY) == 0u) {
		imageStore(nextgeo_out_velocity, p_px, vec4(motion_vector, 0.0, 0.0));
	}
#endif
#else
#ifdef MODE_SEPARATE_SPECULAR
	imageStore(nextgeo_out_color, p_px, diffuse_buffer);
	imageStore(nextgeo_out_specular, p_px, specular_buffer);
#else
	imageStore(nextgeo_out_color, p_px, frag_color);
#endif
#ifdef MOTION_VECTORS
	if ((resolve_push.tile_list_base & NEXTGEO_RESOLVE_COMPUTE_SKIP_VELOCITY) == 0u) {
		imageStore(nextgeo_out_velocity, p_px, vec4(motion_vector, 0.0, 0.0));
	}
#endif
#endif // MODE_RENDER_DEPTH
}
