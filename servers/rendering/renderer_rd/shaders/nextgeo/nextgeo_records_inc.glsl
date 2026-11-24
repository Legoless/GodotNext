// GodotNext: nextgeo_instance_records (#75, plan §6.3, §9)
// std430 copy of RendererVirtualMeshStorage::Record (224 B, 256 B under
// REAL_T_IS_DOUBLE) and MeshRecord (32 B). Keep every offset in sync with
// servers/rendering/storage/virtual_mesh_storage.h.

struct NextGeoInstanceRecord {
	vec4 transform[3]; // 0, 16, 32
	vec4 prev_transform[3]; // 48, 64, 80
	vec4 world_aabb_min; // 96, w = max scale
	vec4 world_aabb_max; // 112, w = lod_bias
	vec4 prev_world_aabb_min; // 128, w = programmable_raster_distance (#153; 0 = always programmable)
	vec4 prev_world_aabb_max; // 144, w = displacement_bound (#150)
	uint virtual_mesh; // 160
	uint page_table_offset; // 164: floatBitsToUint(vertex_offset_disable_distance) (#164; 0 = never disable)
	uint surface_mask; // 168
	uint bucket_base; // 172
	uint layer_mask; // 176
	uint flags; // 180
	uint gi_flags; // 184: [15:0] GI flags, [31:16] conservative float16 vertex_offset_bound (#164)
	uint pad0; // 188: fixed-function shadow surface_mask (#155)
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
// GodotNext: vsm_caching (#212). Per-instance VSM cache behavior and resolved dynamic layer flag.
const uint NEXTGEO_RECORD_VSM_CACHE_AUTO = 0u << 12;
const uint NEXTGEO_RECORD_VSM_CACHE_ALWAYS = 1u << 12;
const uint NEXTGEO_RECORD_VSM_CACHE_RIGID = 2u << 12;
const uint NEXTGEO_RECORD_VSM_CACHE_STATIC = 3u << 12;
const uint NEXTGEO_RECORD_VSM_CACHE_MODE_MASK = 3u << 12;
const uint NEXTGEO_RECORD_VSM_DYNAMIC = 1u << 14;
// GodotNext: nextgeo_vertex_offset (#164). Opt-in RT proxy vertex() evaluation and BLAS refit.
const uint NEXTGEO_RECORD_RT_EVALUATE_VERTEX_OFFSET = 1u << 15;
// GodotNext: nextgeo_skinned_runtime (#160). Instance has a valid #159 skinned virtual mesh + 3D skeleton palette entry.
const uint NEXTGEO_RECORD_SKINNED_PALETTE = 1u << 16;
// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 N10, GN-7, GN-12).
const uint NEXTGEO_RECORD_MULTIMESH_PARENT = 1u << 17;
const uint NEXTGEO_RECORD_MULTIMESH_ELEMENT = 1u << 18;
const uint NEXTGEO_RECORD_NON_UNIFORM_SCALE = 1u << 19;
const uint NEXTGEO_RECORD_HAS_INSTANCE_COLOR = 1u << 20;
const uint NEXTGEO_RECORD_HAS_INSTANCE_CUSTOM = 1u << 21;
const uint NEXTGEO_RECORD_HAS_ASSEMBLY = 1u << 22;
const uint NEXTGEO_RECORD_ASSEMBLY_PART = 1u << 23;
// GodotNext: spline_meshes (#161). Bit 24 marks an active spline record; bits 25..31 hold the 7-bit spline slot index.
const uint NEXTGEO_RECORD_HAS_SPLINE = 1u << 24;
const uint NEXTGEO_RECORD_SPLINE_INDEX_SHIFT = 25u;

// GodotNext: nextgeo_foliage_instancing (#151). Per-record sideband attributes (48 B std430), parallel to NextGeoInstanceRecord.
struct NextGeoInstanceAttributes {
	vec4 color; // 0
	vec4 custom_data; // 16
	uvec4 meta; // 32: x = parent_record_idx (0xFFFFFFFF if none), y = element_index (INSTANCE_ID), z = visibility_range_begin bits, w = visibility_range_end bits
};

// Per-bucket-slot surface and cull masks (16 B std430), indexed by (bucket_base >> 5).
struct NextGeoBucketSurfaceMask {
	uint surface_mask;
	uint shadow_surface_mask;
	uint cull_disabled_mask;
	uint cull_front_mask;
};

// GPU Nanite Assembly descriptors (std430, <= 2 levels: instance -> part -> sub-part).
struct NextGeoAssemblyHeader {
	uint direct_part_offset;
	uint direct_part_count;
	uint child_offset;
	uint child_count;
	uint leaf_count;
	uint pad0;
	uint pad1;
	uint pad2;
}; // 32 B

struct NextGeoAssemblyChild {
	vec4 transform[3];
	uint part_offset;
	uint part_count;
	uint pad0;
	uint pad1;
}; // 64 B

struct NextGeoAssemblyPart {
	vec4 transform[3];
	vec4 local_aabb_min;
	vec4 local_aabb_max;
	uint part_virtual_mesh;
	uint unique_part_index;
	uint surface_mask;
	uint pad0;
}; // 96 B

// GodotNext: nextgeo_foliage_wind (#152, plan §21.4 contract N10).
// Shared std430 structs matching RendererVirtualMeshStorage::WindBoneGPU (48 B)
// and WindMeshGPU (448 B). Evaluated procedurally in shader with 0 per-instance palette bytes.
const uint NEXTGEO_WIND_MAX_BONES = 9u;
const uint NEXTGEO_WIND_PARAM_FLAG_ENABLED = 1u << 0;

struct NextGeoWindBone {
	vec4 rest_pos_lever; // xyz = rest pivot (local), w = lever_length (>= 0.25)
	vec4 axis_stiffness; // xyz = normalized rest axis (local), w = stiffness [0..1]
	vec4 sway_phase_branch; // x = max_sway, y = sway_phase (rad), z = branch_weight (0=trunk, 1=branch), w = pad
}; // 48 B

struct NextGeoWindMesh {
	uvec4 header; // x = bone_count (0..9), y = floatBitsToUint(max_sway), z = base_height, w = top_height
	NextGeoWindBone bones[9];
}; // 448 B

// NextGeoInstanceRecord::gi_flags.
const uint NEXTGEO_GI_MODE_MASK = 3u << 0;
const uint NEXTGEO_GI_MODE_NONE = 0u << 0;
const uint NEXTGEO_GI_MODE_STATIC = 1u << 0;
const uint NEXTGEO_GI_MODE_DYNAMIC = 2u << 0;
const uint NEXTGEO_GI_LIGHTMAPPED = 1u << 2;

// GodotNext: nextgeo_vertex_offset (#164). Unpack conservative float16 WPO bound and disable distance.
float nextgeo_record_vertex_offset_bound(NextGeoInstanceRecord p_record) {
	return max(unpackHalf2x16(p_record.gi_flags >> 16u).x, 0.0);
}

float nextgeo_record_vertex_offset_disable_distance(NextGeoInstanceRecord p_record) {
	return max(uintBitsToFloat(p_record.page_table_offset), 0.0);
}

// Per-(record, surface_id) bucket table entry bits (#80).
const uint NEXTGEO_BUCKET_ID_MASK = 0x0FFFFFFFu;
const uint NEXTGEO_BUCKET_INVALID = 0x0FFFFFFFu;
const uint NEXTGEO_BUCKET_CULL_FRONT = 1u << 28u;
const uint NEXTGEO_BUCKET_CULL_DISABLED = 1u << 29u;

// GodotNext: nextgeo_tessellation (#163) & nextgeo_voxels (#165) reserved 25-bit visibility cluster ID partition.
#include "visibility_inc.glsl"
