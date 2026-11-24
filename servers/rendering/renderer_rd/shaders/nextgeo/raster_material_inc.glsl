// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N4). The material raster target: what
// raster_material_sw.glsl (the software form, compute) and raster_material_hw.glsl (the hardware form,
// vertex and pixel) share. A programmable raster bin runs its material's vertex() at the three corners
// of each triangle with barycentric varyings, then the fragment() subset that decides coverage and
// depth offset: ALPHA, the alpha scissor and hash built-ins, discard and DEPTH_OFFSET (#149). The code comes from
// ShaderCompiler's visibility-resolve output (#80): RESOLVE_VERTEX, RESOLVE_VERTEX_STORE and
// RESOLVE_INTERPOLATE, with implicit-LOD sampling already rewritten to textureGrad with analytic
// derivatives, and RASTER_FRAGMENT, the fragment body whose discard is _NG_RASTER_DISCARD. The setup,
// interpolation and alpha tests below follow scene_forward_clustered.glsl's MODE_VISIBILITY_RESOLVE, so
// the coverage a bin decides is the coverage Stage A/B shades.
//
// The including template defines MODE_VISIBILITY_RESOLVE and one of NEXTGEO_RASTER_SW,
// NEXTGEO_RASTER_HW_VERTEX or NEXTGEO_RASTER_HW_FRAGMENT. Sets: 0 the NextGeo geometry, 1 the scene
// environment of the material code, 2 the bin lists and the visibility buffer, 3 the material.
// NEXTGEO_RASTER_DEPTH_ONLY (#217's VSM page raster) keeps vertex() and coverage and replaces the
// visibility write with nextgeo_raster_write_depth(), which that pass defines.

#define M_PI 3.14159265359
#define M_TAU 6.28318530718
#define MAX_VIEWS 2

#include "../half_inc.glsl"
#include "../scene_data_inc.glsl"
#include "../spline_deform_inc.glsl"
#include "nextgeo_records_inc.glsl"
#include "raster_features_inc.glsl"
#ifdef NEXTGEO_TESSELLATION
#include "tessellation_inc.glsl"
#endif
// GodotNext: vsm_nanite (#217). Depth-only VSM page raster shares the material raster target.
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
#include "../vsm_mark_inc.glsl"
#include "../vsm_raster_inc.glsl"
#endif

#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0
#define OUTPUT_IS_MULTIVIEW false
#define ViewIndex 0
#define sc_emulate_point_size false

#define INSTANCE_FLAGS_NON_UNIFORM_SCALE (1 << 4)
#define INSTANCE_FLAGS_FADE_SHIFT 24

/* Set 0: the NextGeo geometry, bound as the raster shaders bind it. */

layout(set = 0, binding = 0, std430) buffer restrict readonly NextGeoRecords {
	NextGeoInstanceRecord data[];
}
nextgeo_records;

layout(set = 0, binding = 1, std430) buffer restrict readonly NextGeoMeshes {
	NextGeoMeshRecord data[];
}
nextgeo_meshes;

layout(set = 0, binding = 2, std430) buffer restrict readonly NextGeoPages {
	uint data[];
}
nextgeo_pages;

layout(set = 0, binding = 3, std430) buffer restrict readonly NextGeoClusterTable {
	uvec4 data[];
}
nextgeo_cluster_table;

#ifdef NEXTGEO_RASTER_DEPTH_ONLY
// GodotNext: vsm_nanite (#217). (record_idx, global_cluster, dirty_idx, flags) pairs, per-map data and dirty pages.
layout(set = 0, binding = 4, std430) buffer restrict readonly NextGeoVSMClusters {
	uvec4 data[];
}
nextgeo_vsm_clusters;

layout(set = 0, binding = 5, std430) buffer restrict readonly VSMMapDataBuffer {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 6, std430) buffer restrict readonly VSMDirtyBuffer {
	uvec4 data[];
}
vsm_dirty;
#else
layout(set = 0, binding = 4, std430) buffer restrict readonly NextGeoVisibleClusters {
	uvec2 data[];
}
nextgeo_visible_clusters;

// The prefix of NextGeoCut's RasterParams that visibility_raster.glsl reads.
layout(set = 0, binding = 5, std430) buffer restrict readonly RasterParams {
	mat4 proj;
	mat4 view;
	mat4 prev_proj;
	mat4 prev_view;
	vec2 taa_jitter;
	vec2 prev_taa_jitter;
	uint visible_capacity;
	uint reset_velocity;
	float cull_sign;
	uint bucket_table_words;
}
raster;

layout(set = 0, binding = 6, std430) buffer restrict readonly NextGeoBucketTable {
	uint data[];
}
nextgeo_bucket_table;
#endif

// GodotNext: spline_meshes (#161).
layout(set = 0, binding = 7, std430) buffer restrict readonly NextGeoSplines {
	SplineDeformEntry data[];
}
nextgeo_spline_deforms;

// GodotNext: nextgeo_skinned_runtime (#160). Skeleton palette arena, per-record skin table, cluster skin offsets.
layout(set = 0, binding = 8, std430) buffer restrict readonly NextGeoSkinPalette {
	vec4 data[];
}
nextgeo_skin_palette;

layout(set = 0, binding = 9, std430) buffer restrict readonly NextGeoSkinInstances {
	uvec4 data[];
}
nextgeo_skin_instances;

layout(set = 0, binding = 10, std430) buffer restrict readonly NextGeoClusterSkin {
	uint data[];
}
nextgeo_cluster_skin;

#define NEXTGEO_SKIN_PAGES_DATA nextgeo_pages.data
// clang-format off
#include "skinning_inc.glsl"
// clang-format on

// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 N10). Per-record INSTANCE_COLOR, INSTANCE_CUSTOM, and INSTANCE_ID.
layout(set = 0, binding = 11, std430) buffer restrict readonly NextGeoInstanceAttrs {
	NextGeoInstanceAttributes data[];
}
nextgeo_instance_attrs;

/* Set 1: the scene environment the material code reads, as scene_forward_clustered_inc.glsl names it. */

layout(set = 1, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

// scene_forward_clustered_inc.glsl's InstanceData; the NextGeo resolve's per-record copy is bound here.
struct InstanceData {
	mat3x4 transform;
	vec4 compressed_aabb_position_pad;
	vec3 compressed_aabb_size_pad;
	uint material_feedback_index;
	vec4 uv_scale;
	uint flags;
	uint instance_uniforms_ofs;
	uint gi_offset;
	uint layer_mask;
	mat3x4 prev_transform;
	vec4 lightmap_uv_scale;
#ifdef USE_DOUBLE_PRECISION
	vec4 model_precision;
	vec4 prev_model_precision;
#endif
	uvec4 spline_deform_data;
};

layout(set = 1, binding = 1, std430) buffer restrict readonly InstanceDataBuffer {
	InstanceData data[];
}
instances;

layout(set = 1, binding = 2, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

layout(set = 1, binding = 3 + 0) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 1, binding = 3 + 1) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 1, binding = 3 + 2) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 3 + 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 3 + 4) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 3 + 5) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 3 + 6) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 1, binding = 3 + 7) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 1, binding = 3 + 8) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 3 + 9) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 3 + 10) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 1, binding = 3 + 11) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

/* Set 2: the bin's cluster lists and the visibility buffer. */

layout(set = 2, binding = 0, std430) buffer restrict readonly RasterBinLists {
	uint data[];
}
raster_bin_lists;

layout(set = 2, binding = 1, std430) buffer restrict readonly RasterBinState {
	uint data[];
}
raster_bin_state;

// Vertex stages may not write storage resources without vertexPipelineStoresAndAtomics.
#ifndef NEXTGEO_RASTER_HW_VERTEX
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
// GodotNext: vsm_nanite (#217). 32-bit depth-only VSM physical page pool and #277 sink buffer.
layout(set = 2, binding = 2, r32ui) uniform restrict uimage2DArray vsm_pool;
layout(set = 2, binding = 3, std430) buffer restrict VSMCounters {
	uint data[];
}
vsm_counters;
#elif defined(VIS64_BUFFER)
layout(set = 2, binding = 2, std430) buffer restrict Vis64 {
	uint64_t data[];
}
vis64;
// GodotNext: nextgeo_vertex_offset (#164). Main-view CutCounters for vertex_offset_clamped (word 44) and #277 sink (word 45).
layout(set = 2, binding = 3, std430) buffer restrict CutCountersBuf {
	uint data[];
}
cut_counters;
#else
layout(set = 2, binding = 2, r64ui) uniform restrict u64image2D vis64;
// GodotNext: nextgeo_vertex_offset (#164). Main-view CutCounters for vertex_offset_clamped (word 44) and #277 sink (word 45).
layout(set = 2, binding = 3, std430) buffer restrict CutCountersBuf {
	uint data[];
}
cut_counters;
#endif
#endif

// GodotNext: nextgeo_vertex_offset (#164). Flags carried in bin_push.flags (bit 0 = NO_CULL).
const uint NEXTGEO_RASTER_BIN_FLAG_VERTEX_OFFSET = 1u << 1u;
const uint NEXTGEO_RASTER_BIN_FLAG_COUNT_CLAMPED = 1u << 2u;

#ifdef NEXTGEO_TESSELLATION
// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11). Diced patch records and bounded HW stream.
// Vertex stages keep all storage buffers NonWritable (VUID-RuntimeSpirv-NonWritable-06341).
#ifdef NEXTGEO_RASTER_SW
layout(set = 2, binding = 4, std430) buffer restrict writeonly NextGeoDicedPatches {
	NextGeoDicedPatchRecord data[];
}
diced_patches;

layout(set = 2, binding = 5, std430) buffer restrict writeonly NextGeoTessHWPatches {
	uint data[];
}
tess_hw_patches;

layout(set = 2, binding = 6, std430) buffer restrict NextGeoTessStateBuf {
	uint patch_count;
	uint dicing_overflow;
	uint sw_micro_tris;
	uint hw_patches;
	uint hw_stream_overflow;
	uint coarsened_tris;
	uint diced_clusters;
	uint sink;
	uint hw_bin_alloc[64];
	uint pad[8];
}
tess_state;

layout(set = 2, binding = 7, std430) buffer restrict NextGeoTessHWArgs {
	uint data[]; // 64 * 4 words: VkDrawIndirectCommand { 384, instanceCount, 0, 0 }
}
tess_hw_args;
#else
layout(set = 2, binding = 4, std430) buffer restrict readonly NextGeoDicedPatches {
	NextGeoDicedPatchRecord data[];
}
diced_patches;

layout(set = 2, binding = 5, std430) buffer restrict readonly NextGeoTessHWPatches {
	uint data[];
}
tess_hw_patches;
#endif
#endif

layout(push_constant, std430) uniform RasterBinPush {
	uint list_base; // First entry of the phase's region in raster_bin_lists.
	uint phase;
	uint bin;
	uint flags;
	ivec2 size; // The visibility buffer's size.
	uvec2 pad; // pad.x = VSM cluster_capacity (under DEPTH_ONLY), pad.y = floatBitsToUint(vertex_offset_max) (#164).
#ifdef NEXTGEO_TESSELLATION
	float dicing_rate;
	float px_scale;
	float sw_max_edge;
	float z_near;
	float disp_magnitude;
	float disp_center;
	uint hw_bin_base;
	uint hw_bin_capacity;
	uint patch_capacity;
	uint tess_pad0;
	uint tess_pad1;
	uint tess_pad2;
#endif
}
bin_push;

/* The interpolants and corner attributes the resolve code reads and writes. */

vec3 vertex_interp = vec3(0.0);
vec3 vertex_interp_ddx = vec3(0.0);
vec3 vertex_interp_ddy = vec3(0.0);

vec3 normal_interp = vec3(0.0, 0.0, 1.0);
vec3 normal_interp_ddx = vec3(0.0);
vec3 normal_interp_ddy = vec3(0.0);

vec4 color_interp = vec4(1.0);
vec4 color_interp_ddx = vec4(0.0);
vec4 color_interp_ddy = vec4(0.0);

vec2 uv_interp = vec2(0.0);
vec2 uv_interp_ddx = vec2(0.0);
vec2 uv_interp_ddy = vec2(0.0);

vec2 uv2_interp = vec2(0.0);
vec2 uv2_interp_ddx = vec2(0.0);
vec2 uv2_interp_ddy = vec2(0.0);

vec3 tangent_interp = vec3(1.0, 0.0, 0.0);
vec3 tangent_interp_ddx = vec3(0.0);
vec3 tangent_interp_ddy = vec3(0.0);

vec3 binormal_interp = vec3(0.0, 1.0, 0.0);
vec3 binormal_interp_ddx = vec3(0.0);
vec3 binormal_interp_ddy = vec3(0.0);

vec3 view_ddx = vec3(0.0);
vec3 view_ddy = vec3(0.0);

vec4 _ng_fragcoord = vec4(0.0, 0.0, 0.0, 1.0);
vec4 _ng_fragcoord_ddx = vec4(1.0, 0.0, 0.0, 0.0);
vec4 _ng_fragcoord_ddy = vec4(0.0, 1.0, 0.0, 0.0);
bool _ng_front_facing = true;
bool _ng_skip_masked_discard = false;
bool _ng_skip_vertex_offset = false;
uint instance_index_interp = 0u;

int _ng_vertex_id = 0;
int _ng_instance_id = 0;
vec4 _ng_instance_color = vec4(1.0);
vec4 _ng_instance_custom = vec4(0.0);
mat3x4 _ng_model_xform = mat3x4(vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0, 1.0, 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0));
bool _ng_non_uniform_scale = false;
vec4 color_attrib = vec4(1.0);
vec2 uv_attrib = vec2(0.0);
vec2 uv2_attrib = vec2(0.0);
vec4 custom0_attrib = vec4(0.0);
vec4 custom1_attrib = vec4(0.0);
vec4 custom2_attrib = vec4(0.0);
vec4 custom3_attrib = vec4(0.0);
uvec4 bone_attrib = uvec4(0u);
vec4 weight_attrib = vec4(0.0);

#ifdef STREAMING_LOD_USED
float streaming_lod = 32.0;
#endif

#if defined(POINT_SIZE_USED) && defined(POINT_COORD_USED)
vec2 point_coord_interp = vec2(0.5);
#endif

// The triangle's corners after vertex().
vec4 _ng_clip_c[3];
vec3 _ng_vtx_c[3];
vec3 _ng_nrm_c[3];
vec3 _ng_tan_c[3];
vec3 _ng_bin_c[3];
vec2 _ng_uv_c[3];
vec2 _ng_uv2_c[3];
vec4 _ng_col_c[3];

#ifdef NEXTGEO_RASTER_DEPTH_ONLY
// GodotNext: vsm_nanite (#217). Active VSM page context for the current (cluster, page) pair.
VSMMapData _ng_vsm_map;
uint _ng_vsm_mip = 0u;
uvec2 _ng_vsm_slot = uvec2(0u);
ivec2 _ng_vsm_phys_origin = ivec2(0);
int _ng_vsm_layer = 0;
bool _ng_vsm_reverse_cull = false;
float _ng_vsm_depth_c[3];
float _ng_vsm_last_depth = 0.0;
float _ng_vsm_depth_interp = 0.0;
#endif

// Cleared by _NG_RASTER_DISCARD and by the alpha tests.
bool _ng_raster_covered = true;
// GodotNext: depth_offset (#149, plan §21.4 contract N6). Clamped view-space meters (>= 0.0) written by fragment().
float _ng_depth_offset = 0.0;

// The raster target's discard: a compute stage has none, and a pixel stage must still reach its
// atomic or skip it.
#define _NG_RASTER_DISCARD \
	do { \
		_ng_raster_covered = false; \
		return; \
	} while (false)

// Defines to keep compatibility with the scene shader's fragment stage.
#define projection_matrix scene_data.projection_matrix
#define inv_projection_matrix scene_data.inv_projection_matrix
#define global_time scene_data_block.data.time

#ifdef MATERIAL_UNIFORMS_USED
/* clang-format off */
layout(set = 3, binding = 0, std140) uniform MaterialUniforms {
#MATERIAL_UNIFORMS
} material;
/* clang-format on */
#endif

#GLOBALS

vec3 _ng_safe_normalize(vec3 v, vec3 fallback) {
	float l = length(v);
	return l > 1e-12 ? (v / l) : fallback;
}

vec3 _ng_d_normalize(vec3 u, vec3 n, vec3 du) {
	return (du - n * dot(n, du)) / max(length(u), 1e-8);
}

#ifdef USE_DOUBLE_PRECISION
vec3 _ng_quick_two_sum(vec3 a, vec3 b, out vec3 out_p) {
	vec3 s = a + b;
	out_p = b - (s - a);
	return s;
}

vec3 _ng_two_sum(vec3 a, vec3 b, out vec3 out_p) {
	vec3 s = a + b;
	vec3 v = s - a;
	out_p = (a - (s - v)) + (b - v);
	return s;
}

vec3 _ng_double_add_vec3(vec3 base_a, vec3 prec_a, vec3 base_b, vec3 prec_b, out vec3 out_precision) {
	vec3 s, t, se, te;
	s = _ng_two_sum(base_a, base_b, se);
	t = _ng_two_sum(prec_a, prec_b, te);
	se += t;
	s = _ng_quick_two_sum(s, se, se);
	se += te;
	s = _ng_quick_two_sum(s, se, out_precision);
	return s;
}
#endif

#include "../scene_forward_aa_inc.glsl"

#undef projection_matrix
#undef inv_projection_matrix
#undef global_time

// scene_forward_clustered.glsl's _ng_resolve_vertex_corner(): vertex() at one corner.
void _ng_resolve_vertex_corner(
		in vec3 vertex_input,
		in vec3 normal_input,
		in vec3 tangent_input,
		in vec3 binormal_input,
		in uint instance_index,
		in SceneData scene_data,
		in mat3x4 in_model_matrix,
#ifdef USE_DOUBLE_PRECISION
		in vec3 model_precision,
#endif
		out vec4 r_clip_pos) {
	vec4 instance_custom = _ng_instance_custom;
#if defined(COLOR_USED)
	color_interp = color_attrib * _ng_instance_color;
#endif

	mat4 inv_view_matrix = transpose(mat4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	// GodotNext: vsm_nanite (#217). The pair's map is the light camera, as in MODE_VSM.
	mat4 _ng_vsm_view_matrix;
	vsm_map_view(_ng_vsm_map, scene_data.main_cam_inv_view_matrix[3].xyz, _ng_vsm_view_matrix, inv_view_matrix);
#endif

	mat4 model_matrix = transpose(mat4(in_model_matrix[0],
			in_model_matrix[1],
			in_model_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef USE_DOUBLE_PRECISION
	vec3 view_precision = scene_data.inv_view_precision.xyz;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	view_precision = vec3(0.0);
#endif
#endif

	mat3 model_normal_matrix;
	if (_ng_non_uniform_scale || bool(instances.data[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(inverse(mat3(model_matrix)));
	} else {
		model_normal_matrix = mat3(model_matrix);
	}

	mat4 read_model_matrix = model_matrix;

	vec3 vertex = vertex_input;
	vec3 normal_highp = normal_input;
	vec3 tangent = tangent_input;
	vec3 binormal = binormal_input;

	uv_interp = uv_attrib;
	uv2_interp = uv2_attrib;

	vec4 position = vec4(1.0);
	mat4 projection_matrix = scene_data.projection_matrix;
	mat4 inv_projection_matrix = scene_data.inv_projection_matrix;
	vec3 eye_offset = vec3(0.0, 0.0, 0.0);
	float global_time = scene_data.time;

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	vertex = (model_matrix * vec4(vertex, 1.0)).xyz;
#ifdef NORMAL_USED
	normal_highp = model_normal_matrix * normal_highp;
#endif
#ifdef TANGENT_USED
	tangent = mat3(model_matrix) * tangent;
	binormal = mat3(model_matrix) * binormal;
#endif
#endif

	float z_clip_scale = 1.0;
	float roughness_highp = 1.0;
	float point_size = 1.0;

	mat4 read_view_matrix = transpose(mat4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	read_view_matrix = _ng_vsm_view_matrix;
#endif

#ifdef USE_DOUBLE_PRECISION
	mat4 modelview = read_view_matrix * model_matrix;
	vec3 model_origin = model_matrix[3].xyz;
	vec3 temp_precision;
	modelview[3].xyz = _ng_double_add_vec3(model_origin, model_precision, inv_view_matrix[3].xyz, view_precision, temp_precision);
	modelview[3].xyz = mat3(read_view_matrix) * modelview[3].xyz;
#else
	mat4 modelview = read_view_matrix * model_matrix;
#endif
	mat3 modelview_normal = mat3(read_view_matrix) * model_normal_matrix;
	vec2 read_viewport_size = scene_data.viewport_size;

	vec3 _ng_vertex_before_wpo = vertex;
	{
#CODE : RESOLVE_VERTEX
	}

	// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 contract N5). Skip WPO past
	// vertex_offset_disable_distance and clamp offsets exceeding Material.vertex_offset_max.
	if (_ng_skip_vertex_offset) {
		vertex = _ng_vertex_before_wpo;
	} else if ((bin_push.flags & NEXTGEO_RASTER_BIN_FLAG_VERTEX_OFFSET) != 0u) {
		vec3 _ng_wpo_delta = vertex - _ng_vertex_before_wpo;
#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
		float _ng_wpo_scale = max(length(model_matrix[0].xyz), max(length(model_matrix[1].xyz), length(model_matrix[2].xyz)));
		float _ng_wpo_limit = max(uintBitsToFloat(bin_push.pad.y), 0.0) * _ng_wpo_scale;
#else
		float _ng_wpo_limit = max(uintBitsToFloat(bin_push.pad.y), 0.0);
#endif
		float _ng_wpo_tol = max(_ng_wpo_limit * 1e-5, 1e-6);
		float _ng_wpo_thresh = _ng_wpo_limit + _ng_wpo_tol;
		float _ng_wpo_len_sq = dot(_ng_wpo_delta, _ng_wpo_delta);
		if (_ng_wpo_len_sq > _ng_wpo_thresh * _ng_wpo_thresh) {
			if (_ng_wpo_limit > 0.0) {
				vertex = _ng_vertex_before_wpo + _ng_wpo_delta * (_ng_wpo_limit * inversesqrt(_ng_wpo_len_sq));
			} else {
				vertex = _ng_vertex_before_wpo;
			}
#ifndef NEXTGEO_RASTER_HW_VERTEX
			if ((bin_push.flags & NEXTGEO_RASTER_BIN_FLAG_COUNT_CLAMPED) != 0u
#ifdef NEXTGEO_RASTER_HW_FRAGMENT
					&& !gl_HelperInvocation
#endif
			) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
				// #277: every atomic return value is used.
				if (atomicAdd(vsm_counters.data[28u], 1u) == 0xFFFFFFFFu) {
					vsm_counters.data[21u] = 1u;
				}
#else
				if (atomicAdd(cut_counters.data[44u], 1u) == 0xFFFFFFFFu) {
					cut_counters.data[45u] = 1u;
				}
#endif
			}
#endif
		}
	}

	vec3 normal = normal_highp;

#if !defined(SKIP_TRANSFORM_USED) && !defined(VERTEX_WORLD_COORDS_USED)
	vertex = (modelview * vec4(vertex, 1.0)).xyz;
#ifdef NORMAL_USED
	normal = modelview_normal * normal;
#endif
#ifdef TANGENT_USED
	tangent = mat3(modelview) * tangent;
	binormal = mat3(modelview) * binormal;
#endif
#endif

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	vertex = (read_view_matrix * vec4(vertex, 1.0)).xyz;
#ifdef NORMAL_USED
	normal = (read_view_matrix * vec4(normal, 0.0)).xyz;
#endif
#ifdef TANGENT_USED
	binormal = (read_view_matrix * vec4(binormal, 0.0)).xyz;
	tangent = (read_view_matrix * vec4(tangent, 0.0)).xyz;
#endif
#endif

	vertex_interp = vertex;
#ifdef NORMAL_USED
	normal_interp = _ng_safe_normalize(normal, vec3(0.0, 0.0, 1.0));
#endif
#ifdef TANGENT_USED
	tangent_interp = _ng_safe_normalize(tangent, vec3(1.0, 0.0, 0.0));
	binormal_interp = _ng_safe_normalize(binormal, vec3(0.0, 1.0, 0.0));
#endif

#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	// GodotNext: vsm_nanite (#217). Project world position after vertex() into the target VSM page.
	vec3 _ng_vsm_world = (inv_view_matrix * vec4(vertex_interp, 1.0)).xyz;
	r_clip_pos = vsm_page_clip_position(_ng_vsm_map, _ng_vsm_mip, _ng_vsm_slot, _ng_vsm_world, _ng_vsm_last_depth);
#elif defined(OVERRIDE_POSITION)
	r_clip_pos = position;
#else
	r_clip_pos = projection_matrix * vec4(vertex_interp, 1.0);
#endif
}

// GodotNext: nextgeo_displacement (#150, plan §21.4 N11). Evaluates optional spatial displacement()
// with edge-length derivatives for textureGrad() lowering (hooked by #163 dicing).
float _ng_eval_displacement(
		in vec3 _ng_disp_vertex,
		in vec3 _ng_disp_normal,
		in vec2 _ng_disp_uv,
		in vec2 _ng_disp_uv2,
		in vec4 _ng_disp_color,
		in vec4 _ng_disp_instance_custom,
		in vec3 _ng_disp_vertex_ddx,
		in vec3 _ng_disp_vertex_ddy,
		in vec3 _ng_disp_normal_ddx,
		in vec3 _ng_disp_normal_ddy,
		in vec2 _ng_disp_uv_ddx,
		in vec2 _ng_disp_uv_ddy,
		in vec2 _ng_disp_uv2_ddx,
		in vec2 _ng_disp_uv2_ddy,
		in vec4 _ng_disp_color_ddx,
		in vec4 _ng_disp_color_ddy,
		in SceneData scene_data) {
	float _ng_displacement = 0.0;
	float global_time = scene_data.time;
	{
#CODE : RESOLVE_DISPLACEMENT
	}
	return _ng_displacement;
}

#define projection_matrix scene_data.projection_matrix
#define inv_projection_matrix scene_data.inv_projection_matrix
#define global_time scene_data_block.data.time

// The cluster of a visible-cluster index, with the checks of sw_raster.glsl and visibility_raster.glsl.
// r_cull_flags: the surface's bucket-word cull bits (NEXTGEO_BUCKET_CULL_FRONT, NEXTGEO_BUCKET_CULL_DISABLED).
bool _ng_raster_cluster(uint p_vis_cluster, out uvec2 r_entry, out uvec4 r_desc, out uint r_cull_flags) {
	r_entry = uvec2(0u);
	r_desc = uvec4(0u);
	r_cull_flags = 0u;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	// bin_push.pad.x carries VSM cluster_capacity.
	if (p_vis_cluster >= bin_push.pad.x || p_vis_cluster >= 0x1FFFFFFu) {
		return false;
	}
	uvec4 pair = nextgeo_vsm_clusters.data[p_vis_cluster];
	r_entry = pair.xy;
	NextGeoInstanceRecord record = nextgeo_records.data[r_entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || (record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF) {
		return false;
	}
	NextGeoMeshRecord mesh = nextgeo_meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || r_entry.y < mesh.cluster_offset) {
		return false;
	}
	uint local_cluster = r_entry.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		return false;
	}
	r_desc = nextgeo_cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = r_desc.z & 0xFFu;
	uint vert_count = (r_desc.z >> 8u) & 0xFFu;
	uint surface_id = r_desc.w & 0xFFFFu;
	if (stride < 3u || vert_count == 0u || surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		return false;
	}
	if ((record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_DOUBLE_SIDED || (record.pad1[0] & (1u << surface_id)) != 0u) {
		r_cull_flags |= NEXTGEO_BUCKET_CULL_DISABLED;
	} else if ((record.pad1[1] & (1u << surface_id)) != 0u) {
		r_cull_flags |= NEXTGEO_BUCKET_CULL_FRONT;
	}
	uvec4 rec = vsm_dirty.data[pair.z];
	_ng_vsm_map = vsm_maps.data[rec.x];
	_ng_vsm_mip = rec.y & 0xFFu;
	_ng_vsm_slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	_ng_vsm_phys_origin = ivec2(vsm_raster_entry_page(rec.z) * VSM_PAGE_SIZE);
	_ng_vsm_layer = int(pair.w & 1u);
	_ng_vsm_reverse_cull = (pair.w & 2u) != 0u;
	return true;
#else
	if (p_vis_cluster >= raster.visible_capacity || p_vis_cluster >= 0x1FFFFFFu) {
		return false;
	}
	r_entry = nextgeo_visible_clusters.data[p_vis_cluster];
	NextGeoInstanceRecord record = nextgeo_records.data[r_entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		return false;
	}
	NextGeoMeshRecord mesh = nextgeo_meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || r_entry.y < mesh.cluster_offset) {
		return false;
	}
	uint local_cluster = r_entry.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		return false;
	}
	r_desc = nextgeo_cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = r_desc.z & 0xFFu;
	uint vert_count = (r_desc.z >> 8u) & 0xFFu;
	uint surface_id = r_desc.w & 0xFFFFu;
	if (stride < 3u || vert_count == 0u || surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		return false;
	}
	if (raster.bucket_table_words > 0u) {
		uint bucket_slot = record.bucket_base + surface_id;
		if (bucket_slot >= raster.bucket_table_words) {
			return false;
		}
		uint bucket_word = nextgeo_bucket_table.data[bucket_slot];
		if ((bucket_word & NEXTGEO_BUCKET_ID_MASK) == NEXTGEO_BUCKET_INVALID) {
			return false;
		}
		r_cull_flags = bucket_word & (NEXTGEO_BUCKET_CULL_DISABLED | NEXTGEO_BUCKET_CULL_FRONT);
	}
	return true;
#endif
}

#ifdef NEXTGEO_RASTER_HW_MESH
// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9). Evaluates vertex() once per cluster
// vertex in the mesh stage (1 thread per vertex, <= 128 vertices), matching _ng_raster_load_triangle().
bool _ng_raster_eval_vertex(uvec2 p_entry, uvec4 p_desc, uint p_v_idx, out vec4 r_clip_pos) {
	uint _ng_stride = p_desc.z & 0xFFu;
	uint _ng_vert_count = (p_desc.z >> 8u) & 0xFFu;
	uint _ng_layout = (p_desc.z >> 24u) & 0xFFu;
	if (p_v_idx >= _ng_vert_count) {
		r_clip_pos = vec4(0.0, 0.0, -2.0, 1.0);
		return false;
	}
	if (_ng_layout == 0u) {
		if (_ng_stride == 3u) {
			_ng_layout = 0x01u;
		} else if (_ng_stride == 5u) {
			_ng_layout = 0x09u;
		} else if (_ng_stride == 6u) {
			_ng_layout = 0x03u;
		} else if (_ng_stride == 7u) {
			_ng_layout = 0x21u;
		} else if (_ng_stride == 8u) {
			_ng_layout = 0x0Bu;
		} else if (_ng_stride == 10u) {
			_ng_layout = 0x07u;
		} else if (_ng_stride == 12u) {
			_ng_layout = 0x0Fu;
		} else if (_ng_stride == 14u) {
			_ng_layout = 0x1Fu;
		} else if (_ng_stride == 16u) {
			_ng_layout = 0x2Fu;
		} else if (_ng_stride >= 18u) {
			_ng_layout = 0x3Fu;
		}
	}

	NextGeoInstanceRecord _ng_record = nextgeo_records.data[p_entry.x];
	NextGeoInstanceAttributes _ng_attr = nextgeo_instance_attrs.data[p_entry.x];
	instance_index_interp = _ng_record.bucket_base >> 5u;
	_ng_instance_id = int(_ng_attr.meta.y);
	_ng_instance_color = ((_ng_record.flags & NEXTGEO_RECORD_HAS_INSTANCE_COLOR) != 0u) ? _ng_attr.color : vec4(1.0);
	_ng_instance_custom = ((_ng_record.flags & NEXTGEO_RECORD_HAS_INSTANCE_CUSTOM) != 0u) ? _ng_attr.custom_data : vec4(0.0);
	_ng_non_uniform_scale = (_ng_record.flags & NEXTGEO_RECORD_NON_UNIFORM_SCALE) != 0u;
	_ng_skip_masked_discard = false;
	_ng_skip_vertex_offset = false;
	{
		float _ng_prog_dist = _ng_record.prev_world_aabb_min.w;
		float _ng_wpo_dist = nextgeo_record_vertex_offset_disable_distance(_ng_record);
		if (_ng_prog_dist > 0.0 || _ng_wpo_dist > 0.0) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
			vec3 _ng_cam_pos = scene_data_block.data.main_cam_inv_view_matrix[3].xyz;
#else
			vec3 _ng_cam_pos = vec3(
					scene_data_block.data.inv_view_matrix[0].w,
					scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
#endif
			vec3 _ng_closest = clamp(_ng_cam_pos, _ng_record.world_aabb_min.xyz, _ng_record.world_aabb_max.xyz);
			float _ng_cam_dist = length(_ng_cam_pos - _ng_closest);
			if (_ng_prog_dist > 0.0 && _ng_cam_dist > _ng_prog_dist) {
				_ng_skip_masked_discard = true;
			}
			if (_ng_wpo_dist > 0.0 && _ng_cam_dist > _ng_wpo_dist) {
				_ng_skip_vertex_offset = true;
			}
		}
	}
	mat3x4 _ng_cur_xform = mat3x4(_ng_record.transform[0], _ng_record.transform[1], _ng_record.transform[2]);
	_ng_model_xform = _ng_cur_xform;

	uint _ng_w = p_desc.x + p_v_idx * _ng_stride;
	vec3 _ng_c_pos = vec3(
			uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
			uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
			uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]));
	_ng_w += 3u;

	vec3 _ng_c_nrm = vec3(0.0, 0.0, 1.0);
	if ((_ng_layout & 0x02u) != 0u) {
		_ng_c_nrm = vec3(
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]));
		_ng_w += 3u;
	}

	vec3 _ng_c_tan = vec3(1.0, 0.0, 0.0);
	vec3 _ng_c_bin = vec3(0.0, 1.0, 0.0);
	float _ng_tan_sign = 1.0;
	if ((_ng_layout & 0x04u) != 0u) {
		vec4 _ng_tan4 = vec4(
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 3u]));
		_ng_w += 4u;
		_ng_c_tan = _ng_tan4.xyz;
		_ng_tan_sign = _ng_tan4.w;
		_ng_c_bin = _ng_safe_normalize(cross(_ng_c_nrm, _ng_tan4.xyz) * _ng_tan_sign, vec3(0.0, 1.0, 0.0));
	}

	if ((_ng_record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u) {
		NextGeoMeshRecord _ng_sk_mesh = nextgeo_meshes.data[_ng_record.virtual_mesh];
		uint _ng_sk_cluster_idx = _ng_sk_mesh.page_table_offset + (p_entry.y - _ng_sk_mesh.cluster_offset);
		nextgeo_skin_vertex(_ng_c_pos, _ng_c_nrm, _ng_c_tan, p_entry.x, _ng_sk_cluster_idx, p_v_idx, _ng_vert_count, false);
		if ((_ng_layout & 0x04u) != 0u) {
			_ng_c_bin = _ng_safe_normalize(cross(_ng_c_nrm, _ng_c_tan) * _ng_tan_sign, vec3(0.0, 1.0, 0.0));
		}
	}

	vec2 _ng_c_uv = vec2(0.0);
	if ((_ng_layout & 0x08u) != 0u) {
		_ng_c_uv = vec2(
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]));
		_ng_w += 2u;
	}

	vec2 _ng_c_uv2 = vec2(0.0);
	if ((_ng_layout & 0x10u) != 0u) {
		_ng_c_uv2 = vec2(
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]));
		_ng_w += 2u;
	}

	vec4 _ng_c_col = vec4(1.0);
	if ((_ng_layout & 0x20u) != 0u) {
		_ng_c_col = vec4(
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 3u]));
		_ng_w += 4u;
	}

	_ng_vertex_id = int(p_v_idx);
	uv_attrib = _ng_c_uv;
	uv2_attrib = _ng_c_uv2;
	color_attrib = _ng_c_col;

	// GodotNext: spline_meshes (#161).
	if ((_ng_record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
		spline_deform_vertex_full(nextgeo_spline_deforms.data[_ng_record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current, _ng_c_pos, _ng_c_nrm, _ng_c_tan, _ng_c_bin);
	}

	_ng_resolve_vertex_corner(
			_ng_c_pos,
			_ng_c_nrm,
			_ng_c_tan,
			_ng_c_bin,
			instance_index_interp,
			scene_data_block.data,
			_ng_cur_xform,
#ifdef USE_DOUBLE_PRECISION
			_ng_record.model_precision.xyz,
#endif
			r_clip_pos);
	return true;
}
#else
// Loads triangle p_tri of the visible cluster and runs vertex() at its three corners: _ng_clip_c and
// the corner attributes, as the resolve does.
bool _ng_raster_load_triangle(uvec2 p_entry, uvec4 p_desc, uint p_tri) {
	uint _ng_stride = p_desc.z & 0xFFu;
	uint _ng_vert_count = (p_desc.z >> 8u) & 0xFFu;
	uint _ng_tri_count = (p_desc.z >> 16u) & 0xFFu;
	uint _ng_layout = (p_desc.z >> 24u) & 0xFFu;
	if (p_tri >= _ng_tri_count) {
		return false;
	}
	if (_ng_layout == 0u) {
		if (_ng_stride == 3u) {
			_ng_layout = 0x01u;
		} else if (_ng_stride == 5u) {
			_ng_layout = 0x09u;
		} else if (_ng_stride == 6u) {
			_ng_layout = 0x03u;
		} else if (_ng_stride == 7u) {
			_ng_layout = 0x21u;
		} else if (_ng_stride == 8u) {
			_ng_layout = 0x0Bu;
		} else if (_ng_stride == 10u) {
			_ng_layout = 0x07u;
		} else if (_ng_stride == 12u) {
			_ng_layout = 0x0Fu;
		} else if (_ng_stride == 14u) {
			_ng_layout = 0x1Fu;
		} else if (_ng_stride == 16u) {
			_ng_layout = 0x2Fu;
		} else if (_ng_stride >= 18u) {
			_ng_layout = 0x3Fu;
		}
	}

	NextGeoInstanceRecord _ng_record = nextgeo_records.data[p_entry.x];
	NextGeoInstanceAttributes _ng_attr = nextgeo_instance_attrs.data[p_entry.x];
	instance_index_interp = _ng_record.bucket_base >> 5u;
	_ng_instance_id = int(_ng_attr.meta.y);
	_ng_instance_color = ((_ng_record.flags & NEXTGEO_RECORD_HAS_INSTANCE_COLOR) != 0u) ? _ng_attr.color : vec4(1.0);
	_ng_instance_custom = ((_ng_record.flags & NEXTGEO_RECORD_HAS_INSTANCE_CUSTOM) != 0u) ? _ng_attr.custom_data : vec4(0.0);
	_ng_non_uniform_scale = (_ng_record.flags & NEXTGEO_RECORD_NON_UNIFORM_SCALE) != 0u;
	_ng_skip_masked_discard = false;
	_ng_skip_vertex_offset = false;
	{
		float _ng_prog_dist = _ng_record.prev_world_aabb_min.w;
		float _ng_wpo_dist = nextgeo_record_vertex_offset_disable_distance(_ng_record);
		if (_ng_prog_dist > 0.0 || _ng_wpo_dist > 0.0) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
			vec3 _ng_cam_pos = scene_data_block.data.main_cam_inv_view_matrix[3].xyz;
#else
			vec3 _ng_cam_pos = vec3(
					scene_data_block.data.inv_view_matrix[0].w,
					scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
#endif
			vec3 _ng_closest = clamp(_ng_cam_pos, _ng_record.world_aabb_min.xyz, _ng_record.world_aabb_max.xyz);
			float _ng_cam_dist = length(_ng_cam_pos - _ng_closest);
			if (_ng_prog_dist > 0.0 && _ng_cam_dist > _ng_prog_dist) {
				_ng_skip_masked_discard = true;
			}
			if (_ng_wpo_dist > 0.0 && _ng_cam_dist > _ng_wpo_dist) {
				_ng_skip_vertex_offset = true;
			}
		}
	}
	mat3x4 _ng_cur_xform = mat3x4(_ng_record.transform[0], _ng_record.transform[1], _ng_record.transform[2]);
	_ng_model_xform = _ng_cur_xform;

	for (uint _ng_corner = 0u; _ng_corner < 3u; _ng_corner++) {
		uint _ng_idx_byte = p_desc.y + p_tri * 3u + _ng_corner;
		uint _ng_v_idx = (nextgeo_pages.data[_ng_idx_byte >> 2u] >> ((_ng_idx_byte & 3u) * 8u)) & 0xFFu;
		if (_ng_v_idx >= _ng_vert_count) {
			return false;
		}
		uint _ng_w = p_desc.x + _ng_v_idx * _ng_stride;

		vec3 _ng_c_pos = vec3(
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
				uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]));
		_ng_w += 3u;

		vec3 _ng_c_nrm = vec3(0.0, 0.0, 1.0);
		if ((_ng_layout & 0x02u) != 0u) {
			_ng_c_nrm = vec3(
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]));
			_ng_w += 3u;
		}

		vec3 _ng_c_tan = vec3(1.0, 0.0, 0.0);
		vec3 _ng_c_bin = vec3(0.0, 1.0, 0.0);
		float _ng_tan_sign = 1.0;
		if ((_ng_layout & 0x04u) != 0u) {
			vec4 _ng_tan4 = vec4(
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 3u]));
			_ng_w += 4u;
			_ng_c_tan = _ng_tan4.xyz;
			_ng_tan_sign = _ng_tan4.w;
			_ng_c_bin = _ng_safe_normalize(cross(_ng_c_nrm, _ng_tan4.xyz) * _ng_tan_sign, vec3(0.0, 1.0, 0.0));
		}

		if ((_ng_record.flags & NEXTGEO_RECORD_SKINNED_PALETTE) != 0u) {
			NextGeoMeshRecord _ng_sk_mesh = nextgeo_meshes.data[_ng_record.virtual_mesh];
			uint _ng_sk_cluster_idx = _ng_sk_mesh.page_table_offset + (p_entry.y - _ng_sk_mesh.cluster_offset);
			nextgeo_skin_vertex(_ng_c_pos, _ng_c_nrm, _ng_c_tan, p_entry.x, _ng_sk_cluster_idx, _ng_v_idx, _ng_vert_count, false);
			if ((_ng_layout & 0x04u) != 0u) {
				_ng_c_bin = _ng_safe_normalize(cross(_ng_c_nrm, _ng_c_tan) * _ng_tan_sign, vec3(0.0, 1.0, 0.0));
			}
		}

		vec2 _ng_c_uv = vec2(0.0);
		if ((_ng_layout & 0x08u) != 0u) {
			_ng_c_uv = vec2(
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]));
			_ng_w += 2u;
		}

		vec2 _ng_c_uv2 = vec2(0.0);
		if ((_ng_layout & 0x10u) != 0u) {
			_ng_c_uv2 = vec2(
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]));
			_ng_w += 2u;
		}

		vec4 _ng_c_col = vec4(1.0);
		if ((_ng_layout & 0x20u) != 0u) {
			_ng_c_col = vec4(
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 1u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 2u]),
					uintBitsToFloat(nextgeo_pages.data[_ng_w + 3u]));
			_ng_w += 4u;
		}

		_ng_vertex_id = int(_ng_v_idx);
		uv_attrib = _ng_c_uv;
		uv2_attrib = _ng_c_uv2;
		color_attrib = _ng_c_col;

		// GodotNext: spline_meshes (#161).
		if ((_ng_record.flags & NEXTGEO_RECORD_HAS_SPLINE) != 0u) {
			spline_deform_vertex_full(nextgeo_spline_deforms.data[_ng_record.flags >> NEXTGEO_RECORD_SPLINE_INDEX_SHIFT].current, _ng_c_pos, _ng_c_nrm, _ng_c_tan, _ng_c_bin);
		}

		_ng_resolve_vertex_corner(
				_ng_c_pos,
				_ng_c_nrm,
				_ng_c_tan,
				_ng_c_bin,
				instance_index_interp,
				scene_data_block.data,
				_ng_cur_xform,
#ifdef USE_DOUBLE_PRECISION
				_ng_record.model_precision.xyz,
#endif
				_ng_clip_c[_ng_corner]);

		_ng_vtx_c[_ng_corner] = vertex_interp;
		_ng_nrm_c[_ng_corner] = normal_interp;
		_ng_tan_c[_ng_corner] = tangent_interp;
		_ng_bin_c[_ng_corner] = binormal_interp;
		_ng_uv_c[_ng_corner] = uv_interp;
		_ng_uv2_c[_ng_corner] = uv2_interp;
		_ng_col_c[_ng_corner] = color_interp;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		_ng_vsm_depth_c[_ng_corner] = _ng_vsm_last_depth;
#endif
		{
#CODE : RESOLVE_VERTEX_STORE
		}
	}
	return true;
}
#endif

#ifdef NEXTGEO_TESSELLATION
// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11). Cached source triangle attributes
// and deterministic displaced micro-triangle loader shared by tessellation_dice.glsl and tessellation_hw.glsl.
struct _NgTessSourceTri {
	vec3 pos[3];
	vec3 nrm[3];
	vec3 tan[3];
	vec3 bin[3];
	vec2 uv[3];
	vec2 uv2[3];
	vec4 col[3];
	uint v_idx[3];
};

bool _ng_tess_load_source_tri(uvec4 p_desc, uint p_tri, out _NgTessSourceTri r_tri) {
	uint _ng_stride = p_desc.z & 0xFFu;
	uint _ng_vert_count = (p_desc.z >> 8u) & 0xFFu;
	uint _ng_tri_count = (p_desc.z >> 16u) & 0xFFu;
	uint _ng_layout = (p_desc.z >> 24u) & 0xFFu;
	if (p_tri >= _ng_tri_count || _ng_stride < 3u || _ng_vert_count == 0u) {
		return false;
	}
	if (_ng_layout == 0u) {
		if (_ng_stride == 3u) {
			_ng_layout = 0x01u;
		} else if (_ng_stride == 5u) {
			_ng_layout = 0x09u;
		} else if (_ng_stride == 6u) {
			_ng_layout = 0x03u;
		} else if (_ng_stride == 7u) {
			_ng_layout = 0x21u;
		} else if (_ng_stride == 8u) {
			_ng_layout = 0x0Bu;
		} else if (_ng_stride == 10u) {
			_ng_layout = 0x07u;
		} else if (_ng_stride == 12u) {
			_ng_layout = 0x0Fu;
		} else if (_ng_stride == 14u) {
			_ng_layout = 0x1Fu;
		} else if (_ng_stride == 16u) {
			_ng_layout = 0x2Fu;
		} else if (_ng_stride >= 18u) {
			_ng_layout = 0x3Fu;
		}
	}

	for (uint c = 0u; c < 3u; c++) {
		uint idx_byte = p_desc.y + p_tri * 3u + c;
		uint v_idx = (nextgeo_pages.data[idx_byte >> 2u] >> ((idx_byte & 3u) * 8u)) & 0xFFu;
		if (v_idx >= _ng_vert_count) {
			return false;
		}
		r_tri.v_idx[c] = v_idx;
		uint w = p_desc.x + v_idx * _ng_stride;
		r_tri.pos[c] = vec3(
				uintBitsToFloat(nextgeo_pages.data[w + 0u]),
				uintBitsToFloat(nextgeo_pages.data[w + 1u]),
				uintBitsToFloat(nextgeo_pages.data[w + 2u]));
		w += 3u;

		vec3 nrm = vec3(0.0, 0.0, 1.0);
		if ((_ng_layout & 0x02u) != 0u) {
			nrm = vec3(
					uintBitsToFloat(nextgeo_pages.data[w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[w + 1u]),
					uintBitsToFloat(nextgeo_pages.data[w + 2u]));
			w += 3u;
		}
		r_tri.nrm[c] = nrm;

		vec3 tan_v = vec3(1.0, 0.0, 0.0);
		vec3 bin_v = vec3(0.0, 1.0, 0.0);
		if ((_ng_layout & 0x04u) != 0u) {
			vec4 tan4 = vec4(
					uintBitsToFloat(nextgeo_pages.data[w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[w + 1u]),
					uintBitsToFloat(nextgeo_pages.data[w + 2u]),
					uintBitsToFloat(nextgeo_pages.data[w + 3u]));
			w += 4u;
			tan_v = tan4.xyz;
			bin_v = _ng_safe_normalize(cross(nrm, tan4.xyz) * tan4.w, vec3(0.0, 1.0, 0.0));
		}
		r_tri.tan[c] = tan_v;
		r_tri.bin[c] = bin_v;

		vec2 uv_v = vec2(0.0);
		if ((_ng_layout & 0x08u) != 0u) {
			uv_v = vec2(
					uintBitsToFloat(nextgeo_pages.data[w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[w + 1u]));
			w += 2u;
		}
		r_tri.uv[c] = uv_v;

		vec2 uv2_v = vec2(0.0);
		if ((_ng_layout & 0x10u) != 0u) {
			uv2_v = vec2(
					uintBitsToFloat(nextgeo_pages.data[w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[w + 1u]));
			w += 2u;
		}
		r_tri.uv2[c] = uv2_v;

		vec4 col_v = vec4(1.0);
		if ((_ng_layout & 0x20u) != 0u) {
			col_v = vec4(
					uintBitsToFloat(nextgeo_pages.data[w + 0u]),
					uintBitsToFloat(nextgeo_pages.data[w + 1u]),
					uintBitsToFloat(nextgeo_pages.data[w + 2u]),
					uintBitsToFloat(nextgeo_pages.data[w + 3u]));
			w += 4u;
		}
		r_tri.col[c] = col_v;
	}
	return true;
}

void _ng_tess_eval_micro_triangle(
		in uint p_instance_index,
		in NextGeoInstanceRecord p_record,
		in _NgTessSourceTri p_src,
		in uint p_f0,
		in uint p_f1,
		in uint p_f2,
		in uint p_global_micro_tri,
		in float p_disp_mag,
		in float p_disp_center) {
	instance_index_interp = p_instance_index;
	_ng_instance_id = int(p_instance_index);
	mat3x4 cur_xform = mat3x4(p_record.transform[0], p_record.transform[1], p_record.transform[2]);
	uint n_in = nextgeo_tess_interior_factor(p_f0, p_f1, p_f2);

	NextGeoTessVertexCoord coords[3];
	nextgeo_tess_micro_tri_coords(p_f0, p_f1, p_f2, p_global_micro_tri, coords[0], coords[1], coords[2]);

	for (uint _ng_corner = 0u; _ng_corner < 3u; _ng_corner++) {
		vec3 d_pos;
		vec3 d_nrm;
		vec3 d_tan;
		vec3 d_bin;
		vec2 d_uv;
		vec2 d_uv2;
		vec4 d_col;
		vec3 pos_ddx;
		vec3 pos_ddy;
		vec3 nrm_ddx;
		vec3 nrm_ddy;
		vec2 uv_ddx;
		vec2 uv_ddy;
		vec2 uv2_ddx;
		vec2 uv2_ddy;
		vec4 col_ddx;
		vec4 col_ddy;
		nextgeo_tess_eval_vertex_attribs(
				p_src.pos, p_src.nrm, p_src.tan, p_src.bin, p_src.uv, p_src.uv2, p_src.col,
				coords[_ng_corner], n_in,
				d_pos, d_nrm, d_tan, d_bin, d_uv, d_uv2, d_col,
				pos_ddx, pos_ddy, nrm_ddx, nrm_ddy, uv_ddx, uv_ddy, uv2_ddx, uv2_ddy, col_ddx, col_ddy);

		vec3 unit_nrm = _ng_safe_normalize(d_nrm, vec3(0.0, 0.0, 1.0));
		vec3 unit_tan = _ng_safe_normalize(d_tan, vec3(1.0, 0.0, 0.0));
		vec3 unit_bin = _ng_safe_normalize(d_bin, vec3(0.0, 1.0, 0.0));
		if (p_disp_mag > 0.0) {
			float disp_val = _ng_eval_displacement(
					d_pos, unit_nrm, d_uv, d_uv2, d_col, vec4(0.0),
					pos_ddx, pos_ddy, nrm_ddx, nrm_ddy, uv_ddx, uv_ddy, uv2_ddx, uv2_ddy, col_ddx, col_ddy,
					scene_data_block.data);
			if (!isnan(disp_val) && !isinf(disp_val)) {
				d_pos += unit_nrm * ((disp_val - p_disp_center) * p_disp_mag);
			}
		}

		_ng_vertex_id = int(coords[_ng_corner].kind < 3u ? p_src.v_idx[coords[_ng_corner].kind] : p_src.v_idx[0]);
		uv_attrib = d_uv;
		uv2_attrib = d_uv2;
		color_attrib = d_col;

		_ng_resolve_vertex_corner(
				d_pos,
				unit_nrm,
				unit_tan,
				unit_bin,
				p_instance_index,
				scene_data_block.data,
				cur_xform,
#ifdef USE_DOUBLE_PRECISION
				p_record.model_precision.xyz,
#endif
				_ng_clip_c[_ng_corner]);

		_ng_vtx_c[_ng_corner] = vertex_interp;
		_ng_nrm_c[_ng_corner] = normal_interp;
		_ng_tan_c[_ng_corner] = tangent_interp;
		_ng_bin_c[_ng_corner] = binormal_interp;
		_ng_uv_c[_ng_corner] = uv_interp;
		_ng_uv2_c[_ng_corner] = uv2_interp;
		_ng_col_c[_ng_corner] = color_interp;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		_ng_vsm_depth_c[_ng_corner] = _ng_vsm_last_depth;
#endif
#ifndef NEXTGEO_RASTER_HW_VERTEX
		{
#CODE : RESOLVE_VERTEX_STORE
		}
#endif
	}
}

#ifdef NEXTGEO_RASTER_DEPTH_ONLY
vec3 _ng_tess_vsm_light_origin(VSMMapData md) {
	float len0 = length(md.row0.xyz);
	float inv_cot = len0 > 1.0e-20 ? (1.0 / len0) : 0.0;
	vec3 x_axis = md.row0.xyz * inv_cot;
	vec3 y_axis = md.row1.xyz * inv_cot;
	vec3 z_axis = -md.row3.xyz;
	vec3 t = vec3(md.row0.w * inv_cot, md.row1.w * inv_cot, -md.row3.w);
	return -(x_axis * t.x + y_axis * t.y + z_axis * t.z);
}
#endif
#endif // NEXTGEO_TESSELLATION

#ifndef NEXTGEO_RASTER_HW_VERTEX
// The interpolants at framebuffer position p_frag_xy (a pixel center), evaluated in homogeneous 2D
// clip space so near-plane-clipped hardware triangles with w <= 0 never divide by corner w or flip
// _ng_front_facing.
void _ng_raster_interpolate(vec2 p_frag_xy) {
	mat3 _ng_m = mat3(_ng_clip_c[0].xyw, _ng_clip_c[1].xyw, _ng_clip_c[2].xyw);
	vec3 _ng_u0 = cross(_ng_m[1], _ng_m[2]);
	vec3 _ng_u1 = cross(_ng_m[2], _ng_m[0]);
	vec3 _ng_u2 = cross(_ng_m[0], _ng_m[1]);
	float _ng_det = dot(_ng_m[0], _ng_u0);
	_ng_front_facing = (_ng_det > 0.0);

	float _ng_inv_det = abs(_ng_det) > 1e-20 ? (1.0 / _ng_det) : 0.0;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	vec2 _ng_pixel_size = vec2(1.0 / float(VSM_PAGE_SIZE));
#else
	vec2 _ng_pixel_size = scene_data_block.data.screen_pixel_size;
#endif
	vec3 _ng_p = vec3(p_frag_xy * _ng_pixel_size * 2.0 - 1.0, 1.0);
	vec3 _ng_q = vec3(dot(_ng_u0, _ng_p), dot(_ng_u1, _ng_p), dot(_ng_u2, _ng_p)) * _ng_inv_det;
	float _ng_inv_w_sum = _ng_q.x + _ng_q.y + _ng_q.z;
	float _ng_w_interp = abs(_ng_inv_w_sum) > 1e-20 ? (1.0 / _ng_inv_w_sum) : 1.0;
	vec3 _ng_bary = abs(_ng_inv_w_sum) > 1e-20 ? (_ng_q * _ng_w_interp) : vec3(1.0, 0.0, 0.0);

	vec3 _ng_dq_dx = vec3(_ng_u0.x, _ng_u1.x, _ng_u2.x) * (_ng_inv_det * 2.0 * _ng_pixel_size.x);
	vec3 _ng_dq_dy = vec3(_ng_u0.y, _ng_u1.y, _ng_u2.y) * (_ng_inv_det * 2.0 * _ng_pixel_size.y);
	vec3 _ng_dbary_dx = (_ng_dq_dx - _ng_bary * (_ng_dq_dx.x + _ng_dq_dx.y + _ng_dq_dx.z)) * _ng_w_interp;
	vec3 _ng_dbary_dy = (_ng_dq_dy - _ng_bary * (_ng_dq_dy.x + _ng_dq_dy.y + _ng_dq_dy.z)) * _ng_w_interp;

#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	_ng_vsm_depth_interp = _ng_vsm_depth_c[0] * _ng_bary.x + _ng_vsm_depth_c[1] * _ng_bary.y + _ng_vsm_depth_c[2] * _ng_bary.z;
#endif

	vec3 _ng_z_c = vec3(_ng_clip_c[0].z, _ng_clip_c[1].z, _ng_clip_c[2].z);
	_ng_fragcoord = vec4(p_frag_xy, dot(_ng_q, _ng_z_c), _ng_inv_w_sum);
	_ng_fragcoord_ddx = vec4(1.0, 0.0, dot(_ng_dq_dx, _ng_z_c), _ng_dq_dx.x + _ng_dq_dx.y + _ng_dq_dx.z);
	_ng_fragcoord_ddy = vec4(0.0, 1.0, dot(_ng_dq_dy, _ng_z_c), _ng_dq_dy.x + _ng_dq_dy.y + _ng_dq_dy.z);

	vertex_interp = _ng_vtx_c[0] * _ng_bary.x + _ng_vtx_c[1] * _ng_bary.y + _ng_vtx_c[2] * _ng_bary.z;
	vertex_interp_ddx = _ng_vtx_c[0] * _ng_dbary_dx.x + _ng_vtx_c[1] * _ng_dbary_dx.y + _ng_vtx_c[2] * _ng_dbary_dx.z;
	vertex_interp_ddy = _ng_vtx_c[0] * _ng_dbary_dy.x + _ng_vtx_c[1] * _ng_dbary_dy.y + _ng_vtx_c[2] * _ng_dbary_dy.z;

	normal_interp = _ng_nrm_c[0] * _ng_bary.x + _ng_nrm_c[1] * _ng_bary.y + _ng_nrm_c[2] * _ng_bary.z;
	normal_interp_ddx = _ng_nrm_c[0] * _ng_dbary_dx.x + _ng_nrm_c[1] * _ng_dbary_dx.y + _ng_nrm_c[2] * _ng_dbary_dx.z;
	normal_interp_ddy = _ng_nrm_c[0] * _ng_dbary_dy.x + _ng_nrm_c[1] * _ng_dbary_dy.y + _ng_nrm_c[2] * _ng_dbary_dy.z;

	tangent_interp = _ng_tan_c[0] * _ng_bary.x + _ng_tan_c[1] * _ng_bary.y + _ng_tan_c[2] * _ng_bary.z;
	tangent_interp_ddx = _ng_tan_c[0] * _ng_dbary_dx.x + _ng_tan_c[1] * _ng_dbary_dx.y + _ng_tan_c[2] * _ng_dbary_dx.z;
	tangent_interp_ddy = _ng_tan_c[0] * _ng_dbary_dy.x + _ng_tan_c[1] * _ng_dbary_dy.y + _ng_tan_c[2] * _ng_dbary_dy.z;

	binormal_interp = _ng_bin_c[0] * _ng_bary.x + _ng_bin_c[1] * _ng_bary.y + _ng_bin_c[2] * _ng_bary.z;
	binormal_interp_ddx = _ng_bin_c[0] * _ng_dbary_dx.x + _ng_bin_c[1] * _ng_dbary_dx.y + _ng_bin_c[2] * _ng_dbary_dx.z;
	binormal_interp_ddy = _ng_bin_c[0] * _ng_dbary_dy.x + _ng_bin_c[1] * _ng_dbary_dy.y + _ng_bin_c[2] * _ng_dbary_dy.z;

	uv_interp = _ng_uv_c[0] * _ng_bary.x + _ng_uv_c[1] * _ng_bary.y + _ng_uv_c[2] * _ng_bary.z;
	uv_interp_ddx = _ng_uv_c[0] * _ng_dbary_dx.x + _ng_uv_c[1] * _ng_dbary_dx.y + _ng_uv_c[2] * _ng_dbary_dx.z;
	uv_interp_ddy = _ng_uv_c[0] * _ng_dbary_dy.x + _ng_uv_c[1] * _ng_dbary_dy.y + _ng_uv_c[2] * _ng_dbary_dy.z;

	uv2_interp = _ng_uv2_c[0] * _ng_bary.x + _ng_uv2_c[1] * _ng_bary.y + _ng_uv2_c[2] * _ng_bary.z;
	uv2_interp_ddx = _ng_uv2_c[0] * _ng_dbary_dx.x + _ng_uv2_c[1] * _ng_dbary_dx.y + _ng_uv2_c[2] * _ng_dbary_dx.z;
	uv2_interp_ddy = _ng_uv2_c[0] * _ng_dbary_dy.x + _ng_uv2_c[1] * _ng_dbary_dy.y + _ng_uv2_c[2] * _ng_dbary_dy.z;

	color_interp = _ng_col_c[0] * _ng_bary.x + _ng_col_c[1] * _ng_bary.y + _ng_col_c[2] * _ng_bary.z;
	color_interp_ddx = _ng_col_c[0] * _ng_dbary_dx.x + _ng_col_c[1] * _ng_dbary_dx.y + _ng_col_c[2] * _ng_dbary_dx.z;
	color_interp_ddy = _ng_col_c[0] * _ng_dbary_dy.x + _ng_col_c[1] * _ng_dbary_dy.y + _ng_col_c[2] * _ng_dbary_dy.z;

	vec3 _ng_view_init = -_ng_safe_normalize(vertex_interp, vec3(0.0, 0.0, -1.0));
	view_ddx = _ng_d_normalize(-vertex_interp, _ng_view_init, -vertex_interp_ddx);
	view_ddy = _ng_d_normalize(-vertex_interp, _ng_view_init, -vertex_interp_ddy);

	{
#CODE : RESOLVE_INTERPOLATE
	}
}

// The fragment() subset that decides coverage, with scene_forward_clustered.glsl's fragment locals and
// its alpha scissor and hash tests. Leaves _ng_raster_covered false when the sample is not covered.
void _ng_raster_fragment(in SceneData scene_data) {
	_ng_raster_covered = true;
	_ng_depth_offset = 0.0;
	uint instance_index = instance_index_interp;

#ifdef PREMUL_ALPHA_USED
	float premul_alpha = 1.0;
#endif
	vec3 vertex = vertex_interp;
	vec3 eye_offset = vec3(0.0, 0.0, 0.0);
	vec3 view_highp = -normalize(vertex_interp);
	vec3 albedo_highp = vec3(1.0);
	vec3 backlight = vec3(0.0);
	vec4 transmittance_color = vec4(0.0, 0.0, 0.0, 1.0);
	float transmittance_depth = 0.0;
	float transmittance_boost = 0.0;
	float metallic_highp = 0.0;
	float specular = 0.5;
	vec3 emission = vec3(0.0);
	float roughness_highp = 1.0;
	float rim = 0.0;
	float rim_tint = 0.0;
	float clearcoat = 0.0;
	float clearcoat_roughness = 0.0;
	float anisotropy = 0.0;
	vec2 anisotropy_flow = vec2(1.0, 0.0);
	vec3 energy_compensation = vec3(1.0);
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
#if defined(CUSTOM_RADIANCE_USED)
	vec4 custom_radiance = vec4(0.0);
#endif
#if defined(CUSTOM_IRRADIANCE_USED)
	vec4 custom_irradiance = vec4(0.0);
#endif

	float ao = 1.0;
	float ao_light_affect = 0.0;

	float alpha_highp = float(instances.data[instance_index].flags >> INSTANCE_FLAGS_FADE_SHIFT) / float(255.0);

#ifdef TANGENT_USED
	vec3 binormal = binormal_interp;
	vec3 tangent = tangent_interp;
#else
	vec3 binormal = vec3(0.0);
	vec3 tangent = vec3(0.0);
#endif

#ifdef NORMAL_USED
	vec3 normal_highp = normal_interp;
#if defined(DO_SIDE_CHECK)
	if (!_ng_front_facing) {
		normal_highp = -normal_highp;
		normal_interp_ddx = -normal_interp_ddx;
		normal_interp_ddy = -normal_interp_ddy;
	}
#endif
#endif

#ifdef UV_USED
	vec2 uv = uv_interp;
#endif

#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	vec2 uv2 = uv2_interp;
#endif

#if defined(COLOR_USED)
	vec4 color = color_interp;
#endif

#if defined(NORMAL_MAP_USED)
	vec3 normal_map = vec3(0.5);
#endif

#if defined(BENT_NORMAL_MAP_USED)
	vec3 bent_normal_vector = vec3(0.5);
	vec3 bent_normal_map = vec3(0.5);
#endif

	float normal_map_depth = 1.0;

	vec2 screen_uv = _ng_fragcoord.xy * scene_data.screen_pixel_size;

	float sss_strength = 0.0;

#ifdef ALPHA_SCISSOR_USED
	float alpha_scissor_threshold = 1.0;
#endif

#ifdef ALPHA_HASH_USED
	float alpha_hash_scale = 1.0;
#endif

#ifdef ALPHA_ANTIALIASING_EDGE_USED
	float alpha_antialiasing_edge = 0.0;
	vec2 alpha_texture_coordinate = vec2(0.0, 0.0);
#endif

	mat4 inv_view_matrix = transpose(mat4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 read_model_matrix = transpose(mat4(_ng_model_xform[0],
			_ng_model_xform[1],
			_ng_model_xform[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

#ifdef LIGHT_VERTEX_USED
	vec3 light_vertex = vertex;
#endif

	mat3 model_normal_matrix;
	if (_ng_non_uniform_scale || bool(instances.data[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(inverse(mat3(read_model_matrix)));
	} else {
		model_normal_matrix = mat3(read_model_matrix);
	}

	mat4 read_view_matrix = transpose(mat4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec2 read_viewport_size = scene_data.viewport_size;
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	vsm_map_view(_ng_vsm_map, scene_data.main_cam_inv_view_matrix[3].xyz, read_view_matrix, inv_view_matrix);
	read_viewport_size = vec2(float(VSM_PAGE_SIZE));
#endif

#ifdef POINT_COORD_USED
	vec2 point_coord = vec2(0.5);
#endif

#ifdef DEPTH_OFFSET_USED
	float depth_offset = 0.0;
#endif

	if (!_ng_skip_masked_discard) {
		{
#CODE : RASTER_FRAGMENT
		}

		float alpha = alpha_highp;

#ifndef USE_SHADOW_TO_OPACITY
#ifdef ALPHA_SCISSOR_USED
		if (alpha < alpha_scissor_threshold) {
			_ng_raster_covered = false;
			return;
		}
#endif

#ifdef ALPHA_HASH_USED
		mat4 _ng_inv_model = inverse(read_model_matrix);
		vec3 object_pos = (_ng_inv_model * inv_view_matrix * vec4(vertex, 1.0)).xyz;
		mat3 _ng_view_to_object = mat3(_ng_inv_model) * mat3(inv_view_matrix);
		vec3 _ng_object_pos_ddx = _ng_view_to_object * vertex_interp_ddx;
		vec3 _ng_object_pos_ddy = _ng_view_to_object * vertex_interp_ddy;
		if (alpha < compute_alpha_hash_threshold_grad(object_pos, _ng_object_pos_ddx, _ng_object_pos_ddy, alpha_hash_scale)) {
			_ng_raster_covered = false;
			return;
		}
#endif
#endif
	}

#ifdef DEPTH_OFFSET_USED
	_ng_depth_offset = (depth_offset > 0.0) ? min(depth_offset, 1e15) : 0.0;
#endif
}

// GodotNext: depth_offset (#149, plan §21.4 contract N6). Applies the clamped view-space offset
// (>= 0.0 meters) in main-view reversed-Z clip depth or VSM linear light-space depth.
float _ng_apply_depth_offset(in SceneData scene_data, float p_base_depth) {
#ifdef DEPTH_OFFSET_USED
	if (_ng_depth_offset > 0.0) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
		return p_base_depth + _ng_depth_offset;
#else
		vec4 pdo_clip = projection_matrix * vec4(vertex_interp.xy, vertex_interp.z - _ng_depth_offset, 1.0);
		float pdo_depth = (abs(pdo_clip.w) > 1e-20) ? (pdo_clip.z / pdo_clip.w) : 0.0;
		return (pdo_depth > 0.0) ? min(pdo_depth, p_base_depth) : 0.0;
#endif
	}
#endif
	return p_base_depth;
}

#ifdef NEXTGEO_RASTER_DEPTH_ONLY
// GodotNext: vsm_nanite (#217). 32-bit reversed-Z depth atomic into the VSM physical page pool.
void nextgeo_raster_write_depth(ivec2 p_pixel, float p_depth) {
	if (any(lessThan(p_pixel, ivec2(0))) || any(greaterThanEqual(p_pixel, ivec2(int(VSM_PAGE_SIZE))))) {
		return;
	}
	uint packed_depth = vsm_depth_pack(p_depth);
	if (packed_depth != 0u) {
		ivec3 pool_coord = ivec3(_ng_vsm_phys_origin + p_pixel, _ng_vsm_layer);
		// #277: every atomic's result is used.
		if (imageAtomicMax(vsm_pool, pool_coord, packed_depth) == 0xFFFFFFFFu) {
			vsm_counters.data[21u] = 1u;
		}
	}
}
#endif

// The result of a 64-bit atomic max is never read: Metal's 64-bit max returns nothing (#175).
void _ng_raster_write(ivec2 p_pixel, float p_depth, uint p_visibility_id) {
#ifdef NEXTGEO_RASTER_DEPTH_ONLY
	nextgeo_raster_write_depth(p_pixel, p_depth);
#else
	uint64_t value = (uint64_t(floatBitsToUint(min(p_depth, 1.0))) << 32u) | uint64_t(p_visibility_id);
#ifdef VIS64_BUFFER
	atomicMax(vis64.data[uint(p_pixel.y) * uint(bin_push.size.x) + uint(p_pixel.x)], value);
#else
	imageAtomicMax(vis64, p_pixel, value);
#endif
#endif
}
#endif
