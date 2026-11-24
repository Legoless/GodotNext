#define M_PI 3.14159265359
#define M_TAU 6.28318530718
#define ROUGHNESS_MAX_LOD 5

#define MAX_VOXEL_GI_INSTANCES 8
#define MAX_VIEWS 2

#extension GL_KHR_shader_subgroup_ballot : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable

#include "../cluster_data_inc.glsl"
#include "../decal_data_inc.glsl"
#include "../oct_inc.glsl"
#include "../scene_data_inc.glsl"

#ifdef MODE_VISIBILITY_RESOLVE
#include "../nextgeo/nextgeo_records_inc.glsl"
#endif

// GodotNext: vsm_non_nanite (#218) / vsm_projection (#222). In every variant (the VSM_* defines reach them
// all): the VSM raster variant draws pages and the light loop samples them. The include builder inlines a
// file once per stage, so these stay outside any variant branch.
#include "../vsm_mark_inc.glsl"
#include "../vsm_raster_inc.glsl"

#if !defined(MODE_RENDER_DEPTH) || defined(MODE_RENDER_MATERIAL) || defined(MODE_RENDER_SDF) || defined(MODE_RENDER_NORMAL_ROUGHNESS) || defined(MODE_RENDER_VOXEL_GI) || defined(TANGENT_USED) || defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED)
#ifndef NORMAL_USED
#define NORMAL_USED
#endif
#endif

#if !defined(TANGENT_USED) && (defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED))
#define TANGENT_USED
#endif

#ifdef MODE_VISIBILITY_RESOLVE
layout(push_constant, std430) uniform ResolvePush {
	uint bucket_id;
	uint material_feedback_index;
	uint tile_list_base;
	float cull_sign;
}
resolve_push;
#else
layout(push_constant, std430) uniform DrawCall {
	uint instance_index;
	uint uv_offset;
	uint multimesh_motion_vectors_current_offset;
	uint multimesh_motion_vectors_previous_offset;
#ifdef UBERSHADER
	uint sc_packed_0;
	uint sc_packed_1;
	uint sc_packed_2;
	uint uc_packed_0;
#endif
}
draw_call;
#endif

/* Specialization Constants */

#ifdef UBERSHADER

#define POLYGON_CULL_DISABLED 0
#define POLYGON_CULL_FRONT 1
#define POLYGON_CULL_BACK 2

// Pull the constants from the draw call's push constants.
uint sc_packed_0() {
	return draw_call.sc_packed_0;
}

uint sc_packed_1() {
	return draw_call.sc_packed_1;
}

uint uc_cull_mode() {
	return (draw_call.uc_packed_0 >> 0) & 3U;
}

#else

// Pull the constants from the pipeline's specialization constants.
layout(constant_id = 0) const uint pso_sc_packed_0 = 0;
layout(constant_id = 1) const uint pso_sc_packed_1 = 0;

uint sc_packed_0() {
	return pso_sc_packed_0;
}

uint sc_packed_1() {
	return pso_sc_packed_1;
}

#endif

bool sc_use_forward_gi() {
	return ((sc_packed_0() >> 0) & 1U) != 0;
}

bool sc_use_light_projector() {
	return ((sc_packed_0() >> 1) & 1U) != 0;
}

bool sc_use_light_soft_shadows() {
	return ((sc_packed_0() >> 2) & 1U) != 0;
}

bool sc_use_directional_soft_shadows() {
	return ((sc_packed_0() >> 3) & 1U) != 0;
}

bool sc_decal_use_mipmaps() {
	return ((sc_packed_0() >> 4) & 1U) != 0;
}

bool sc_projector_use_mipmaps() {
	return ((sc_packed_0() >> 5) & 1U) != 0;
}

bool sc_use_depth_fog() {
	return ((sc_packed_0() >> 6) & 1U) != 0;
}

bool sc_use_lightmap_bicubic_filter() {
	return ((sc_packed_0() >> 7) & 1U) != 0;
}

uint sc_soft_shadow_samples() {
	return (sc_packed_0() >> 8) & 63U;
}

uint sc_penumbra_shadow_samples() {
	return (sc_packed_0() >> 14) & 63U;
}

uint sc_directional_soft_shadow_samples() {
	return (sc_packed_0() >> 20) & 63U;
}

uint sc_directional_penumbra_shadow_samples() {
	return (sc_packed_0() >> 26) & 63U;
}

bool sc_multimesh() {
	return ((sc_packed_1() >> 0) & 1U) != 0;
}

bool sc_multimesh_format_2d() {
	return ((sc_packed_1() >> 1) & 1U) != 0;
}

bool sc_multimesh_has_color() {
	return ((sc_packed_1() >> 2) & 1U) != 0;
}

bool sc_multimesh_has_custom_data() {
	return ((sc_packed_1() >> 3) & 1U) != 0;
}

bool sc_fog_use_legacy_blending() {
	return ((sc_packed_1() >> 4) & 1U) != 0;
}

bool sc_cluster_has_area_light() {
	return ((sc_packed_1() >> 5) & 1U) != 0;
}

bool sc_use_lightmap_specular() {
	return ((sc_packed_1() >> 6) & 1U) != 0;
}

bool sc_material_feedback() {
	return ((sc_packed_1() >> 7) & 1U) != 0;
}

// GodotNext: avboit (#295). RendererRD::AVBOIT::CompositeMode: 0 outside the AVBOIT composite.
uint sc_avboit_composite() {
	return (sc_packed_1() >> 8) & 7U;
}

// GodotNext: vsm_projection (#222). The view's light loop samples virtual shadow maps for lights whose
// vsm_flags carry VSM_LIGHT_FLAGS_SAMPLE.
bool sc_use_vsm() {
	return ((sc_packed_1() >> 11) & 1U) != 0;
}

// GodotNext: vsm_fog_translucency (#215). Forward transparent pass uses one coarse-page VSM tap (no SMRT)
// when rendering/lights_and_shadows/virtual_shadows/translucent_quality == 0.
bool sc_use_vsm_coarse() {
	return ((sc_packed_1() >> 12) & 1U) != 0;
}

// GodotNext: df_shadows (#214). The view's light loop samples the half-res R8 DF shadow mask array for
// lights whose vsm_flags carry VSM_LIGHT_FLAGS_DF_SAMPLE.
bool sc_use_df_shadows() {
	return ((sc_packed_1() >> 13) & 1U) != 0;
}

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8). The pass samples the Light Function Atlas
// for lights whose light_function_index != LIGHT_FUNCTION_NONE (off on forward transparents by default).
bool sc_use_light_function() {
	return ((sc_packed_1() >> 14) & 1U) != 0;
}

float sc_luminance_multiplier() {
	// Not used in clustered renderer but we share some code with the mobile renderer that requires this.
	return 1.0;
}

layout(constant_id = 2) const bool sc_emulate_point_size = false;

// GodotNext: vsm_non_nanite (#218). INSTANCE_INDEX is the pair's MultiMesh or particle instance;
// the VSM variant never emulates point sizes.
#ifdef MODE_VSM

int vsm_sub_instance = 0;
#define VERTEX_INDEX gl_VertexIndex
#define INSTANCE_INDEX vsm_sub_instance

#elif defined(POINT_SIZE_USED)

#define VERTEX_INDEX (sc_emulate_point_size ? gl_InstanceIndex : gl_VertexIndex)
#define INSTANCE_INDEX (sc_emulate_point_size ? (gl_VertexIndex / 6) : gl_InstanceIndex)

#else

#define VERTEX_INDEX gl_VertexIndex
#define INSTANCE_INDEX gl_InstanceIndex

#endif

#define REFLECTION_MULTIPLIER 1.0

#define SDFGI_MAX_CASCADES 8

/* Set 0: Base Pass (never changes) */

#include "../light_data_inc.glsl"

layout(set = 0, binding = 2) uniform sampler shadow_sampler;

// GodotNext: nextgi_alpha_policy (#61).
#define INSTANCE_FLAGS_USE_NEXTGI_TRANSLUCENCY (1 << 0)
#define INSTANCE_FLAGS_DYNAMIC (1 << 3)
#define INSTANCE_FLAGS_NON_UNIFORM_SCALE (1 << 4)
#define INSTANCE_FLAGS_USE_GI_BUFFERS (1 << 5)
#define INSTANCE_FLAGS_USE_SDFGI (1 << 6)
#define INSTANCE_FLAGS_USE_LIGHTMAP_CAPTURE (1 << 7)
#define INSTANCE_FLAGS_USE_LIGHTMAP (1 << 8)
#define INSTANCE_FLAGS_USE_SH_LIGHTMAP (1 << 9)
#define INSTANCE_FLAGS_USE_VOXEL_GI (1 << 10)
#define INSTANCE_FLAGS_PARTICLES (1 << 11)
#define INSTANCE_FLAGS_PARTICLE_TRAIL_SHIFT 16
#define INSTANCE_FLAGS_FADE_SHIFT 24
//3 bits of stride
#define INSTANCE_FLAGS_PARTICLE_TRAIL_MASK 0xFF

#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSAO (1 << 0)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSIL (1 << 1)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSR (1 << 2)
#define SCREEN_SPACE_EFFECTS_FLAGS_RESOLVE_SSR (1 << 3)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_SSCS (1 << 4)
#define SCREEN_SPACE_EFFECTS_FLAGS_USE_NEXTGI (1 << 5)
#define SCREEN_SPACE_EFFECTS_FLAGS_NEXTGI_ROUGH_REFLECTION (1 << 6)
// GodotNext: ps_lumen_lite (#183)
#define SCREEN_SPACE_EFFECTS_FLAGS_NEXTGI_LITE (1 << 7)

layout(set = 0, binding = 3, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 4, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 5, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;

layout(set = 0, binding = 6, std430) restrict readonly buffer ReflectionProbeData {
	ReflectionData data[];
}
reflections;

layout(set = 0, binding = 7, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;

#define LIGHTMAP_FLAG_USE_DIRECTION 1
#define LIGHTMAP_FLAG_USE_SPECULAR_DIRECTION 2

#define LIGHTMAP_SHADOWMASK_MODE_NONE 0
#define LIGHTMAP_SHADOWMASK_MODE_REPLACE 1
#define LIGHTMAP_SHADOWMASK_MODE_OVERLAY 2
#define LIGHTMAP_SHADOWMASK_MODE_ONLY 3

struct Lightmap {
	mat3x4 normal_xform_and_specular_intensity; // "normal_xform" is the 3x3 matrix. "specular_intensity" is the 4th row of the 1st column.
	vec2 light_texture_size;
	float exposure_normalization;
	uint flags;
};

layout(set = 0, binding = 8, std140) restrict readonly buffer Lightmaps {
	Lightmap data[];
}
lightmaps;

struct LightmapCapture {
	vec4 sh[9];
};

layout(set = 0, binding = 9, std140) restrict readonly buffer LightmapCaptures {
	LightmapCapture data[];
}
lightmap_captures;

layout(set = 0, binding = 10) uniform texture2D decal_atlas;
layout(set = 0, binding = 11) uniform texture2D decal_atlas_srgb;

layout(set = 0, binding = 12, std430) restrict readonly buffer Decals {
	DecalData data[];
}
decals;

layout(set = 0, binding = 13, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

struct SDFVoxelGICascadeData {
	vec3 position;
	float to_probe;
	ivec3 probe_world_offset;
	float to_cell; // 1/bounds * grid_size
	vec3 pad;
	float exposure_normalization;
};

layout(set = 0, binding = 14, std140) uniform SDFGI {
	vec3 grid_size;
	uint max_cascades;

	bool use_occlusion;
	int probe_axis_size;
	float probe_to_uvw;
	float normal_bias;

	vec3 lightprobe_tex_pixel_size;
	float energy;

	vec3 lightprobe_uv_offset;
	float y_mult;

	vec3 occlusion_clamp;
	uint pad3;

	vec3 occlusion_renormalize;
	uint pad4;

	vec3 cascade_probe_size;
	uint pad5;

	SDFVoxelGICascadeData cascades[SDFGI_MAX_CASCADES];
}
sdfgi;

layout(set = 0, binding = 15) uniform sampler DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;

layout(set = 0, binding = 16) uniform texture2D best_fit_normal_texture;

layout(set = 0, binding = 17) uniform texture2D dfg;

layout(set = 0, binding = 18) uniform texture2D ltc_lut1;

layout(set = 0, binding = 19) uniform texture2D ltc_lut2;

layout(set = 0, binding = 20) uniform texture2D area_light_atlas;
/* Set 1: Render Pass (changes per render pass) */

layout(set = 1, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

struct ImplementationData {
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;

	uint ss_effects_flags;
	float ssao_light_affect;
	float ssao_ao_affect;
	uint pad1;

	mat4 sdf_to_bounds;

	ivec3 sdf_offset;
	uint pad2;

	ivec3 sdf_size;
	bool gi_upscale_for_msaa;

	bool volumetric_fog_enabled;
	float volumetric_fog_inv_length;
	float volumetric_fog_detail_spread;
	uint volumetric_fog_pad;

	// GodotNext: light_grid (#139, plan §21.3 contract M2). Packed froxel light grid parameters.
	uvec4 light_grid_dims; // x: grid_x, y: grid_y, z: z_slices (32), w: flags (bit 0: enabled, bit 1: orthogonal)
	vec4 light_grid_z_params; // x: z_near, y: z_far, z: z_scale, w: z_bias
};

layout(set = 1, binding = 1, std140) uniform ImplementationDataBlock {
	ImplementationData data;
}
implementation_data_block;

#define implementation_data implementation_data_block.data

struct InstanceData {
	mat3x4 transform;
	vec4 compressed_aabb_position_pad; // .xyz is the compressed AABB. GodotNext: megalights_inputs (#136) stores the layer-group index in .w.
	vec3 compressed_aabb_size_pad; // Only .xyz is used. .w is padding.
	uint material_feedback_index; // Index into the material feedback buffer.
	vec4 uv_scale;
	uint flags;
	uint instance_uniforms_ofs; //base offset in global buffer for instance variables
	uint gi_offset; //GI information when using lightmapping (VCT or lightmap index)
	uint layer_mask;
	mat3x4 prev_transform;
	vec4 lightmap_uv_scale;
#ifdef USE_DOUBLE_PRECISION
	vec4 model_precision;
	vec4 prev_model_precision;
#endif
};

#ifdef MODE_VISIBILITY_RESOLVE
layout(set = 1, binding = 2, std430) buffer restrict readonly LegacyInstanceDataBuffer {
	InstanceData data[];
}
_ng_unused_legacy_instances;
#else
layout(set = 1, binding = 2, std430) buffer restrict readonly InstanceDataBuffer {
	InstanceData data[];
}
instances;
#endif

#ifdef USE_RADIANCE_OCTMAP_ARRAY

layout(set = 1, binding = 3) uniform texture2DArray radiance_octmap;

#else

layout(set = 1, binding = 3) uniform texture2D radiance_octmap;

#endif

layout(set = 1, binding = 4) uniform texture2DArray reflection_atlas;

layout(set = 1, binding = 5) uniform texture2D shadow_atlas;

layout(set = 1, binding = 6) uniform texture2D directional_shadow_atlas;

layout(set = 1, binding = 7) uniform texture2DArray lightmap_textures[MAX_LIGHTMAP_TEXTURES * 2];

layout(set = 1, binding = 8) uniform texture3D voxel_gi_textures[MAX_VOXEL_GI_INSTANCES];

layout(set = 1, binding = 9, std430) buffer restrict readonly ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 1, binding = 10) uniform sampler decal_sampler;

layout(set = 1, binding = 11) uniform sampler light_projector_sampler;

layout(set = 1, binding = 12 + 0) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 1, binding = 12 + 1) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 1, binding = 12 + 2) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 12 + 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 12 + 4) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 12 + 5) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 12 + 6) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 1, binding = 12 + 7) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 1, binding = 12 + 8) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 12 + 9) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 12 + 10) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 1, binding = 12 + 11) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

#ifdef MODE_RENDER_SDF

layout(r16ui, set = 1, binding = 24) uniform restrict writeonly uimage3D albedo_volume_grid;
layout(r32ui, set = 1, binding = 25) uniform restrict writeonly uimage3D emission_grid;
layout(r32ui, set = 1, binding = 26) uniform restrict writeonly uimage3D emission_aniso_grid;
layout(r32ui, set = 1, binding = 27) uniform restrict uimage3D geom_facing_grid;

//still need to be present for shaders that use it, so remap them to something
#define depth_buffer shadow_atlas
#define color_buffer shadow_atlas
#define normal_roughness_buffer shadow_atlas

#define multiviewSampler sampler2D
#elif defined(MODE_VSM)

// GodotNext: vsm_non_nanite (#218). Per-page depth raster bindings in Set 1.
layout(r32ui, set = 1, binding = 24) uniform restrict uimage2DArray vsm_pool;
layout(set = 1, binding = 25, std430) buffer restrict readonly VSMGroupOffsets {
	uint data[];
}
vsm_group_offsets;
layout(set = 1, binding = 26, std430) buffer restrict readonly VSMPairs {
	uvec4 data[];
}
vsm_pairs;
layout(set = 1, binding = 27, std430) buffer restrict readonly VSMDirty {
	uvec4 data[];
}
vsm_dirty;
layout(set = 1, binding = 28, std430) buffer restrict readonly VSMMaps {
	VSMMapData data[];
}
vsm_maps;
layout(set = 1, binding = 29, std430) buffer restrict VSMPairTexels {
	uint data[]; // 0: the #277 sink; 1 + 2 * pair: its fragments; 2 + 2 * pair: its vertices outside its cull box.
}
vsm_pair_texels;
layout(set = 1, binding = 30, std430) buffer restrict readonly VSMPairBoxes {
	vec4 data[]; // A raster check's cull box of each pair: min, max.
}
vsm_pair_boxes;

#define depth_buffer shadow_atlas
#define color_buffer shadow_atlas
#define normal_roughness_buffer shadow_atlas

#define multiviewSampler sampler2D
#else

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 24) uniform texture2DArray depth_buffer;
layout(set = 1, binding = 25) uniform texture2DArray color_buffer;
layout(set = 1, binding = 26) uniform texture2DArray normal_roughness_buffer;
layout(set = 1, binding = 27) uniform texture2DArray ao_buffer;
layout(set = 1, binding = 28) uniform texture2DArray ambient_buffer;
layout(set = 1, binding = 29) uniform texture2DArray reflection_buffer;
#define multiviewSampler sampler2DArray
#else // USE_MULTIVIEW
layout(set = 1, binding = 24) uniform texture2D depth_buffer;
layout(set = 1, binding = 25) uniform texture2D color_buffer;
layout(set = 1, binding = 26) uniform texture2D normal_roughness_buffer;
layout(set = 1, binding = 27) uniform texture2D ao_buffer;
layout(set = 1, binding = 28) uniform texture2D ambient_buffer;
layout(set = 1, binding = 29) uniform texture2D reflection_buffer;
#define multiviewSampler sampler2D
#endif
layout(set = 1, binding = 30) uniform texture2DArray sdfgi_lightprobe_texture;
layout(set = 1, binding = 31) uniform texture3D sdfgi_occlusion_cascades;

struct VoxelGIData {
	mat4 xform; // 64 - 64

	vec3 bounds; // 12 - 76
	float dynamic_range; // 4 - 80

	float bias; // 4 - 84
	float normal_bias; // 4 - 88
	bool blend_ambient; // 4 - 92
	uint mipmaps; // 4 - 96

	vec3 pad; // 12 - 108
	float exposure_normalization; // 4 - 112
};

layout(set = 1, binding = 32, std140) uniform VoxelGIs {
	VoxelGIData data[MAX_VOXEL_GI_INSTANCES];
}
voxel_gi_instances;

layout(set = 1, binding = 33) uniform texture3D volumetric_fog_texture;

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 34) uniform texture2DArray ssil_buffer;
layout(set = 1, binding = 35) uniform texture2DArray ssr_buffer;
layout(set = 1, binding = 36) uniform texture2DArray ssr_mip_level_buffer;
layout(set = 1, binding = 37) uniform texture2DArray sscs_buffer;
#else
layout(set = 1, binding = 34) uniform texture2D ssil_buffer;
layout(set = 1, binding = 35) uniform texture2D ssr_buffer;
layout(set = 1, binding = 36) uniform texture2D ssr_mip_level_buffer;
layout(set = 1, binding = 37) uniform texture2DArray sscs_buffer;
#endif // USE_MULTIVIEW

// GodotNext: vsm_projection (#222, plan §21.3 contract S6). What the light loop reads to sample virtual
// shadow maps (RenderForwardClustered::_setup_render_pass_uniform_set(); defaults while VSM is off).
layout(set = 1, binding = 51) uniform utexture2DArray vsm_pool_tex;
layout(set = 1, binding = 52, std430) buffer restrict readonly VSMPageTable {
	uint entries[];
}
vsm_page_table;
layout(set = 1, binding = 53, std430) buffer restrict readonly VSMMaps {
	VSMMapData data[];
}
vsm_maps;
layout(set = 1, binding = 54, std430) buffer restrict readonly VSMMapState {
	uint data[];
}
vsm_map_state;

// RenderForwardClustered::VSMProjectionParams.
struct VSMProjectionParams {
	vec4 footprint; // x, y: a pixel's footprint in meters is view depth * x + y (the marking pass's rule).
	ivec4 bias; // x: directional LOD bias, y: local-light LOD bias (mips).
	uvec4 counts; // x: the page table's map count, y: rays for local lights, z: rays for directional lights, w: steps.
	uvec4 misc; // x: Prefiltered Distant on; y, z, w: unused.
};

layout(set = 1, binding = 55, std430) buffer restrict readonly VSMProjection {
	VSMProjectionParams data;
}
vsm_projection;
layout(set = 1, binding = 56) uniform texture2D vsm_blue_noise;
// GodotNext: vsm_fog_translucency (#215). Atomic missing-coarse-page counters for forward translucent coarse lookups.
#ifdef VSM_NO_COARSE_MISS_ATOMICS
layout(set = 1, binding = 57, std430) buffer restrict readonly VSMCoarseMiss {
	uint data[];
}
vsm_coarse_miss;
#else
layout(set = 1, binding = 57, std430) buffer restrict VSMCoarseMiss {
	uint data[];
}
vsm_coarse_miss;
#endif
// GodotNext: df_shadows (#214, plan §21.3 contract S7). Half-res R8 DF shadow mask array (1 layer per DF light,
// max 8) and its matching half-res view-space normal + linear-depth guide written by df_shadow.glsl.
layout(set = 1, binding = 58) uniform texture2DArray df_shadow_mask;
layout(set = 1, binding = 59) uniform texture2D df_shadow_guide;

// GodotNext: light_grid (#139, plan §21.3 contract M2). Packed per-cell light list buffers
// (bound to a 16-byte default dummy buffer when the packed light grid is inactive).
layout(set = 1, binding = 60, std430) buffer restrict readonly PackedLightGridHeaders {
	uvec2 data[];
}
packed_light_grid_headers;
layout(set = 1, binding = 61, std430) buffer restrict readonly PackedLightGridIndices {
	uint data[];
}
packed_light_grid_indices;

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8). Bindings 62 and 63 hold the
// Light Function Atlas texture and per-view LightFunctionEntry SSBO (binding 64 stays free for #140 IES).
#include "../light_function_inc.glsl"
layout(set = 1, binding = 62) uniform texture2D light_function_atlas;
layout(set = 1, binding = 63, std430) buffer restrict readonly LightFunctionBuffer {
	LightFunctionEntry data[];
}
light_function_data;

#define FORWARD_CLUSTERED_LIGHT_GRID

#if !defined(MODE_RENDER_DEPTH) && !defined(MODE_UNSHADED)
#define LIGHT_FUNCTION_ATLAS
#define VSM_PROJECTION
#define VSM_COARSE_NEAREST_SAMPLER SAMPLER_NEAREST_CLAMP
#include "../vsm_page_table_inc.glsl"
#include "../vsm_coarse_inc.glsl"
#include "../vsm_smrt_inc.glsl"
#endif

#endif

vec4 normal_roughness_compatibility(vec4 p_normal_roughness) {
	float roughness = p_normal_roughness.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);
	return vec4(normalize(p_normal_roughness.xyz * 2.0 - 1.0) * 0.5 + 0.5, roughness);
}

// https://google.github.io/filament/Filament.html#toc5.3.4.7
// Note: We have an inverted Y-axis in the DFG; therefore, the roughness value must be inverted.
vec2 prefiltered_dfg(float lod, float NoV) {
	return textureLod(sampler2D(dfg, SAMPLER_LINEAR_CLAMP), vec2(NoV, 1.0 - lod), 0.0).rg;
}

// Compute multiscatter compensation
// https://google.github.io/filament/Filament.html#listing_energycompensationimpl
vec3 get_energy_compensation(vec3 f0, float env) {
	return 1.0 + f0 * (1.0 / env - 1.0);
}

#ifdef TEXTURE_STREAMING
// Texture streaming material feedback buffer access
layout(set = 1, binding = 38, std430) buffer restrict MaterialFeedbackBuffer {
	uint data[];
}
material_feedback;
#endif

// GodotNext: nextgi_alpha_policy (#61). WRC SH clipmap for the translucency cut fallback.
layout(set = 1, binding = 39) uniform texture3D nextgi_wrc_sh_r;
layout(set = 1, binding = 40) uniform texture3D nextgi_wrc_sh_g;
layout(set = 1, binding = 41) uniform texture3D nextgi_wrc_sh_b;
layout(set = 1, binding = 42, std430) restrict readonly buffer NextGIWRCForward {
	vec4 camera;
	vec4 sky;
	vec4 origin_spacing;
	uvec4 dispatch;
	uint page_rect_count;
	uint fresh_levels; // GodotNext: nextgi_wrc_shift (#254). Unused here.
	uint pad1;
	uint pad2;
	mat4 inv_view_proj;
	vec4 volume; // x width, y height, z near, w far.
	// GodotNext: nextgi_view_math. The same layout as NextGIWRCParams in
	// nextgi_wrc_update.glsl and NextGIWRCParamsGPU (a doctest compares them).
	vec4 depth_coefficients;
	mat4 previous_view_proj;
	vec4 history;
}
nextgi_wrc_fwd;
// GodotNext: nextgi_translucency_volume (#61).
layout(set = 1, binding = 43) uniform texture3D nextgi_translucency_r;
layout(set = 1, binding = 44) uniform texture3D nextgi_translucency_g;
layout(set = 1, binding = 45) uniform texture3D nextgi_translucency_b;
layout(set = 1, binding = 46) uniform sampler2D nextgi_front_layer_opaque_depth;
layout(set = 1, binding = 47) uniform sampler2D nextgi_front_layer_depth;
layout(set = 1, binding = 48) uniform sampler2D nextgi_front_layer_reflection;

// GodotNext: avboit (#295, plan §21.2.1). The AVBOIT occlusion grid: one cell per
// RendererRD::AVBOIT::TILE_SIZE pixels square. avboit_bounds holds the nearest (.r) and farthest (.a)
// view depth of the occluding translucent surfaces in the cell, avboit_occlusion the optical depth of
// AVBOIT_SLICES slices, spaced logarithmically between them, four per layer. Outside the AVBOIT passes
// both are black, which reads as an empty cell. RendererRD::AVBOIT has the same math on the CPU.
layout(set = 1, binding = 49) uniform texture2D avboit_bounds;
layout(set = 1, binding = 50) uniform texture2DArray avboit_occlusion;

#define AVBOIT_SLICES 16u
#define AVBOIT_MAX_ALPHA 0.999
#define AVBOIT_MIN_DEPTH 1e-4
// The largest half float: the bounds keep no farther depth.
#define AVBOIT_MAX_DEPTH 65504.0
// Past the cell's farthest depth by this factor (more than its half-float rounding), a surface is
// behind every occluder the cell recorded.
#define AVBOIT_FAR_MARGIN 1.002
// The composite's depth at a cell's center and the pre-passes' differ by rounding, so a surface on a
// slice boundary can land in the next slice in one of them: its own slice is every slice within this
// relative distance of its depth.
#define AVBOIT_SLICE_TOLERANCE 2.5e-4

float avboit_optical_depth(float p_alpha) {
	return -log(1.0 - clamp(p_alpha, 0.0, AVBOIT_MAX_ALPHA));
}

// The view depth of window depth p_window_depth at window position p_frag_coord, in a view
// p_viewport_size pixels in size, kept to the bounds' half-float range. Both pre-passes and the
// composite classify by it, so a surface's depth at a cell's center is the same number in each.
float avboit_view_depth(mat4 p_inv_projection, vec2 p_frag_coord, vec2 p_viewport_size, float p_window_depth) {
	vec2 ndc = p_frag_coord / p_viewport_size * 2.0 - 1.0;
	vec4 view_position = p_inv_projection * vec4(ndc, clamp(p_window_depth, 0.0, 1.0), 1.0);
	return clamp(-view_position.z / view_position.w, AVBOIT_MIN_DEPTH, AVBOIT_MAX_DEPTH);
}

uint avboit_slice(float p_depth, float p_near, float p_far) {
	if (!(p_far > p_near * 1.0001)) {
		return 0u;
	}
	float t = log(max(p_depth, p_near) / p_near) / log(p_far / p_near);
	return uint(clamp(t * float(AVBOIT_SLICES), 0.0, float(AVBOIT_SLICES - 1u)));
}

// 1 in the lane of p_slice when it lies in layer p_layer.
vec4 avboit_slice_lanes(uint p_slice, uint p_layer) {
	return vec4(equal(uvec4(p_slice), uvec4(p_layer * 4u) + uvec4(0u, 1u, 2u, 3u)));
}

// The transmittance in front of a surface whose depth at the center of grid cell p_cell is p_depth
// (where the pre-passes sampled it): the optical depth of the slices in front of its own, and of the
// others in its own slice (the slices within AVBOIT_SLICE_TOLERANCE of its depth). p_self_optical_depth
// is its own share when the pre-passes record it (0 otherwise); it comes off its own slice only, so a
// surface the cell's center missed is not let through the slices in front of it.
// A surface nearer than every occluder in the cell (an additive surface in front of glass, or a
// layer's edge where the cell's center saw only the layers behind it) has nothing in front of it; one
// behind every occluder the cell recorded has them all in front of it.
float avboit_transmittance_front(ivec2 p_cell, float p_depth, float p_self_optical_depth) {
	vec4 bounds = texelFetch(sampler2D(avboit_bounds, SAMPLER_NEAREST_CLAMP), p_cell, 0);
	if (!(bounds.a > 0.0) || p_depth < bounds.r) {
		return 1.0;
	}
	uint own_first = avboit_slice(p_depth * (1.0 - AVBOIT_SLICE_TOLERANCE), bounds.r, bounds.a);
	uint own_last = avboit_slice(p_depth * (1.0 + AVBOIT_SLICE_TOLERANCE), bounds.r, bounds.a);
	float in_front = 0.0;
	float same = 0.0;
	for (uint layer = 0u; layer < AVBOIT_SLICES / 4u; layer++) {
		vec4 slices = texelFetch(sampler2DArray(avboit_occlusion, SAMPLER_NEAREST_CLAMP), ivec3(p_cell, int(layer)), 0);
		uvec4 index = uvec4(layer * 4u) + uvec4(0u, 1u, 2u, 3u);
		vec4 front = vec4(lessThan(index, uvec4(own_first)));
		in_front += dot(slices, front);
		same += dot(slices, vec4(lessThanEqual(index, uvec4(own_last))) - front);
	}
	if (p_depth > bounds.a * AVBOIT_FAR_MARGIN) {
		return exp(-(in_front + same));
	}
	return exp(-(in_front + max(same - p_self_optical_depth, 0.0)));
}

vec3 nextgi_translucency_volume(vec2 p_screen_uv, float p_linear_depth, vec3 p_world_direction) {
	const float near = nextgi_wrc_fwd.volume.z;
	const float far = nextgi_wrc_fwd.volume.w;
	float slice = clamp(log(max(p_linear_depth, near) / near) / log(far / near), 0.0, 1.0);
	vec3 coord = vec3(p_screen_uv, slice);
#ifdef MODE_VISIBILITY_RESOLVE
	vec4 red = textureLod(sampler3D(nextgi_translucency_r, SAMPLER_LINEAR_CLAMP), coord, 0.0);
	vec4 green = textureLod(sampler3D(nextgi_translucency_g, SAMPLER_LINEAR_CLAMP), coord, 0.0);
	vec4 blue = textureLod(sampler3D(nextgi_translucency_b, SAMPLER_LINEAR_CLAMP), coord, 0.0);
#else
	vec4 red = texture(sampler3D(nextgi_translucency_r, SAMPLER_LINEAR_CLAMP), coord);
	vec4 green = texture(sampler3D(nextgi_translucency_g, SAMPLER_LINEAR_CLAMP), coord);
	vec4 blue = texture(sampler3D(nextgi_translucency_b, SAMPLER_LINEAR_CLAMP), coord);
#endif
	vec3 direction = normalize(p_world_direction);
	vec3 irradiance = vec3(red.r, green.r, blue.r);
	irradiance += direction.x * vec3(red.g, green.g, blue.g);
	irradiance += direction.y * vec3(red.b, green.b, blue.b);
	irradiance += direction.z * vec3(red.a, green.a, blue.a);
	return max(irradiance, vec3(0.0));
}

vec3 nextgi_wrc_translucency_sh(vec3 p_world) {
	float distance = length(p_world - nextgi_wrc_fwd.camera.xyz);
	float level = clamp(floor(log2(max(distance / 8.0, 1.0))), 0.0, 3.0);
	float spacing = nextgi_wrc_fwd.origin_spacing.w * exp2(level);
	vec3 level_origin = floor(nextgi_wrc_fwd.origin_spacing.xyz / spacing) * spacing;
	vec3 rel = (p_world - level_origin) / spacing + 7.5;
	ivec3 cell = clamp(ivec3(round(rel)), ivec3(0), ivec3(15));
	cell.z += int(level) * 16;
	vec3 sh;
	sh.r = texelFetch(sampler3D(nextgi_wrc_sh_r, SAMPLER_LINEAR_CLAMP), cell, 0).r;
	sh.g = texelFetch(sampler3D(nextgi_wrc_sh_g, SAMPLER_LINEAR_CLAMP), cell, 0).r;
	sh.b = texelFetch(sampler3D(nextgi_wrc_sh_b, SAMPLER_LINEAR_CLAMP), cell, 0).r;
	return sh;
}

/* Set 2 Skeleton & Instancing (can change per item) */

#ifdef MODE_VISIBILITY_RESOLVE

layout(set = 2, binding = 0) uniform utexture2D nextgeo_visibility_tex;

layout(set = 2, binding = 1, std430) restrict readonly buffer NextGeoVisibleClusters {
	uvec2 data[];
}
nextgeo_visible_clusters;

layout(set = 2, binding = 2, std430) restrict readonly buffer NextGeoRecords {
	NextGeoInstanceRecord data[];
}
nextgeo_records;

layout(set = 2, binding = 3, std430) restrict readonly buffer NextGeoMeshes {
	NextGeoMeshRecord data[];
}
nextgeo_meshes;

layout(set = 2, binding = 4, std430) restrict readonly buffer NextGeoClusterTable {
	uvec4 data[];
}
nextgeo_cluster_table;

layout(set = 2, binding = 5, std430) restrict readonly buffer NextGeoPages {
	uint data[];
}
nextgeo_pages;

layout(set = 2, binding = 6, std430) restrict readonly buffer NextGeoBucketTable {
	uint data[];
}
nextgeo_bucket_table;

layout(set = 2, binding = 7, std430) restrict readonly buffer NextGeoTileList {
	uint data[];
}
nextgeo_tile_list;

layout(set = 2, binding = 8, std430) restrict readonly buffer InstanceDataBuffer {
	InstanceData data[];
}
instances;

#else

layout(set = 2, binding = 0, std430) restrict readonly buffer Transforms {
	vec4 data[];
}
transforms;

#endif

/* Set 3 User Material */
