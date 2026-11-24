#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 contract N5, GN-12).
// Opt-in compute evaluation of a spatial material's vertex() over a virtual mesh's RT proxy
// vertices so the per-instance proxy BLAS refit tracks world-position-offset animation.

#define MODE_VISIBILITY_RESOLVE
#define NEXTGEO_RASTER_HW_VERTEX

#define M_PI 3.14159265359
#define M_TAU 6.28318530718
#define MAX_VIEWS 2

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "../half_inc.glsl"
#include "../scene_data_inc.glsl"

#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0
#define OUTPUT_IS_MULTIVIEW false
#define ViewIndex 0
#define sc_emulate_point_size false

#define INSTANCE_FLAGS_NON_UNIFORM_SCALE (1 << 4)
#define INSTANCE_FLAGS_FADE_SHIFT 24

/* Set 0: RT proxy bind-pose input (vec4(pos.xyz, uv.x), vec4(nrm.xyz, uv.y)) and displaced xyz output. */

struct RTBindPoseVertex {
	vec4 pos_uvx;
	vec4 nrm_uvy;
};

layout(set = 0, binding = 0, std430) buffer restrict readonly RTBindPoseBuffer {
	RTBindPoseVertex data[];
}
rt_bind_pose;

layout(set = 0, binding = 1, std430) buffer restrict writeonly RTDisplacedVertexBuffer {
	float data[];
}
rt_out_positions;

/* Set 1: Scene environment matching scene_forward_clustered_inc.glsl / raster_material_inc.glsl. */

layout(set = 1, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

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
};

layout(set = 1, binding = 1, std430) buffer restrict readonly InstanceDataBuffer {
	InstanceData data[];
}
instances;

layout(set = 1, binding = 2, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

/* Set 2: Default samplers. */

layout(set = 2, binding = 0) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 2, binding = 1) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 2, binding = 2) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 2, binding = 3) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 2, binding = 4) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 2, binding = 5) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 2, binding = 6) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 2, binding = 7) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 2, binding = 8) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 2, binding = 9) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 2, binding = 10) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 2, binding = 11) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;

layout(push_constant, std430) uniform RTVertexOffsetPush {
	mat4 model_matrix;
	uint bind_pose_base_vertex;
	uint out_float_offset;
	uint vertex_count;
	uint instance_flags;
	float vertex_offset_max;
	float max_scale;
	uint instance_id;
	uint pad0;
}
rt_push;

/* Globals referenced by ShaderCompiler's resolve/raster output. */

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
uint instance_index_interp = 0u;

int _ng_vertex_id = 0;
int _ng_instance_id = 0;
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

bool _ng_raster_covered = true;

#define _NG_RASTER_DISCARD \
	do { \
		_ng_raster_covered = false; \
		return; \
	} while (false)

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

#include "../scene_forward_aa_inc.glsl"

#undef projection_matrix
#undef inv_projection_matrix
#undef global_time

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= rt_push.vertex_count) {
		return;
	}

	RTBindPoseVertex bind_v = rt_bind_pose.data[rt_push.bind_pose_base_vertex + idx];
	vec3 local_pos = bind_v.pos_uvx.xyz;
	vec3 local_nrm = bind_v.nrm_uvy.xyz;
	vec2 local_uv = vec2(bind_v.pos_uvx.w, bind_v.nrm_uvy.w);

	SceneData scene_data = scene_data_block.data;
	mat4 inv_view_matrix = transpose(mat4(scene_data.inv_view_matrix[0],
			scene_data.inv_view_matrix[1],
			scene_data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 read_view_matrix = transpose(mat4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

	mat4 model_matrix = rt_push.model_matrix;
	mat4 read_model_matrix = model_matrix;
	mat3 model_normal_matrix;
	if ((rt_push.instance_flags & uint(INSTANCE_FLAGS_NON_UNIFORM_SCALE)) != 0u) {
		model_normal_matrix = transpose(inverse(mat3(model_matrix)));
	} else {
		model_normal_matrix = mat3(model_matrix);
	}
	mat4 modelview = read_view_matrix * model_matrix;
	mat3 modelview_normal = mat3(read_view_matrix) * model_normal_matrix;

	vec3 vertex = local_pos;
	vec3 normal_highp = local_nrm;
	vec3 tangent = vec3(1.0, 0.0, 0.0);
	vec3 binormal = vec3(0.0, 1.0, 0.0);
	vec4 position = vec4(1.0);
	vec4 instance_custom = vec4(0.0);
	float z_clip_scale = 1.0;
	float roughness_highp = 1.0;
	float point_size = 1.0;
	vec3 eye_offset = vec3(0.0);
	vec2 read_viewport_size = scene_data.viewport_size;
	mat4 projection_matrix = scene_data.projection_matrix;
	mat4 inv_projection_matrix = scene_data.inv_projection_matrix;
	float global_time = scene_data.time;

	_ng_vertex_id = int(idx);
	_ng_instance_id = int(rt_push.instance_id);
	instance_index_interp = 0u;
	uv_attrib = local_uv;
	uv2_attrib = vec2(0.0);
	color_attrib = vec4(1.0);
	uv_interp = local_uv;
	uv2_interp = vec2(0.0);
	color_interp = vec4(1.0);

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	vertex = (model_matrix * vec4(vertex, 1.0)).xyz;
#ifdef NORMAL_USED
	normal_highp = model_normal_matrix * normal_highp;
#endif
#endif

	vec3 vertex_before_wpo = vertex;
	{
#CODE : RESOLVE_VERTEX
	}

	vec3 delta = vertex - vertex_before_wpo;
#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	float wpo_limit = max(rt_push.vertex_offset_max, 0.0) * max(rt_push.max_scale, 0.0);
#else
	float wpo_limit = max(rt_push.vertex_offset_max, 0.0);
#endif
	float wpo_tol = max(wpo_limit * 1e-5, 1e-6);
	float wpo_thresh = wpo_limit + wpo_tol;
	float delta_len_sq = dot(delta, delta);
	if (delta_len_sq > wpo_thresh * wpo_thresh) {
		if (wpo_limit > 0.0) {
			vertex = vertex_before_wpo + delta * (wpo_limit * inversesqrt(delta_len_sq));
		} else {
			vertex = vertex_before_wpo;
		}
	}

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	vertex = (inverse(model_matrix) * vec4(vertex, 1.0)).xyz;
#endif

	if (any(isnan(vertex)) || any(isinf(vertex))) {
		vertex = local_pos;
	}

	uint out_base = rt_push.out_float_offset + idx * 3u;
	rt_out_positions.data[out_base + 0u] = vertex.x;
	rt_out_positions.data[out_base + 1u] = vertex.y;
	rt_out_positions.data[out_base + 2u] = vertex.z;
}
