// GodotNext: nextgeo_instance_records (#75, plan §6.3, §9)
// std430 copy of RendererVirtualMeshStorage::Record (224 B, 256 B under
// REAL_T_IS_DOUBLE) and MeshRecord (32 B). Keep every offset in sync with
// servers/rendering/storage/virtual_mesh_storage.h.

struct NextGeoInstanceRecord {
	vec4 transform[3]; // 0, 16, 32
	vec4 prev_transform[3]; // 48, 64, 80
	vec4 world_aabb_min; // 96, w = max scale
	vec4 world_aabb_max; // 112, w = lod_bias
	vec4 prev_world_aabb_min; // 128
	vec4 prev_world_aabb_max; // 144
	uint virtual_mesh; // 160
	uint page_table_offset; // 164
	uint surface_mask; // 168
	uint bucket_base; // 172
	uint layer_mask; // 176
	uint flags; // 180
	uint gi_flags; // 184
	uint pad0; // 188
	uint tlas_instance_index; // 192
	uint blas_handle; // 196
	uint rt_arena_chunk; // 200
	uint rt_arena_offset; // 204
	uint src_page_table; // 208
	uint card_set; // 212
	uint pad1[2]; // 216: [0] = per-surface cull_disabled_mask, [1] = per-surface cull_front_mask (#82)
#if defined(REAL_T_IS_DOUBLE) || defined(USE_DOUBLE_PRECISION)
	vec4 model_precision; // 224
	vec4 prev_model_precision; // 240
#endif
};

struct NextGeoMeshRecord {
	uint bvh_node_offset; // 0
	uint bvh_level_count; // 4
	uint cluster_offset; // 8
	uint cluster_count; // 12
	uint group_offset; // 16
	uint page_table_offset; // 20
	uint page_count; // 24
	uint surface_count; // 28
};

// NextGeoInstanceRecord::flags.
const uint NEXTGEO_RECORD_VALID = 1u << 0;
const uint NEXTGEO_RECORD_VISIBLE = 1u << 1;
const uint NEXTGEO_RECORD_CAST_SHADOW_MASK = 3u << 2;
const uint NEXTGEO_RECORD_CAST_SHADOW_OFF = 0u << 2;
const uint NEXTGEO_RECORD_CAST_SHADOW_ON = 1u << 2;
const uint NEXTGEO_RECORD_CAST_SHADOW_DOUBLE_SIDED = 2u << 2;
const uint NEXTGEO_RECORD_CAST_SHADOW_SHADOWS_ONLY = 3u << 2;
const uint NEXTGEO_RECORD_MOVED = 1u << 4;
const uint NEXTGEO_RECORD_TELEPORTED = 1u << 5;
const uint NEXTGEO_RECORD_HAS_SKELETON = 1u << 6;
const uint NEXTGEO_RECORD_HAS_INSTANCE_UNIFORMS = 1u << 7;
const uint NEXTGEO_RECORD_RT_STATIC = 1u << 8;
const uint NEXTGEO_RECORD_RT_SKINNED = 1u << 9;
const uint NEXTGEO_RECORD_RT_ALPHA_AS_OPAQUE = 1u << 10;
const uint NEXTGEO_RECORD_RT_FAR = 1u << 11;

// NextGeoInstanceRecord::gi_flags.
const uint NEXTGEO_GI_MODE_MASK = 3u << 0;
const uint NEXTGEO_GI_MODE_NONE = 0u << 0;
const uint NEXTGEO_GI_MODE_STATIC = 1u << 0;
const uint NEXTGEO_GI_MODE_DYNAMIC = 2u << 0;
const uint NEXTGEO_GI_LIGHTMAPPED = 1u << 2;

// Per-(record, surface_id) bucket table entry bits (#80).
const uint NEXTGEO_BUCKET_ID_MASK = 0x0FFFFFFFu;
const uint NEXTGEO_BUCKET_INVALID = 0x0FFFFFFFu;
const uint NEXTGEO_BUCKET_CULL_FRONT = 1u << 28u;
const uint NEXTGEO_BUCKET_CULL_DISABLED = 1u << 29u;
