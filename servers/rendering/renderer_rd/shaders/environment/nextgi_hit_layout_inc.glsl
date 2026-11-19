// GodotNext: nextgi_hit_groups (#130). Not a standalone shader.
//
// The one layout every stage of the NextGI hit pipeline declares: the base library's ray generation, miss
// and fallback closest-hit stages and every material's closest-hit and any-hit stages. Pipeline libraries
// are linked only when their layouts are identical (#179), so every stage declares every binding here,
// also the ones it does not read. Set 2, the material parameter table, is declared by each stage with its
// own block (the descriptor layout is the same).

#ifndef NEXTGI_HIT_LAYOUT_INC_GLSL
#define NEXTGI_HIT_LAYOUT_INC_GLSL

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

#include "nextgi_records_inc.glsl"

// NextGIHitMaterials' records (next_gi_hit_materials.cpp), std430.
// One per NextGISurfaceRecord, at the same index: where the geometry's attribute stream is and which
// material shades it.
struct NextGIHitSurface {
	uint attribute_chunk;
	uint attribute_offset; // Bytes in the chunk.
	uint material_id; // Index into hit_materials, or 0xFFFFFFFF. Not "material": the hit template defines that name.
	uint flags; // Bit 0: the geometry has an attribute stream.
};

// One per material: its parameter buffer's slot in the set 2 table and its textures' slots.
struct NextGIHitMaterial {
	uint params_slot;
	uint texture_base; // First entry in hit_texture_slots.
	uint texture_count;
	uint shader_slot; // The material shader's hit group record names this slot.
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT hit_tlas;

layout(set = 0, binding = 1, std430) restrict readonly buffer HitInstanceRecords {
	NextGIInstanceRecord data[];
}
hit_records;

layout(set = 0, binding = 2, std430) restrict readonly buffer HitSurfaceRecords {
	NextGISurfaceRecord data[];
}
hit_surface_records;

layout(set = 0, binding = 3, std430) restrict readonly buffer HitSurfaces {
	NextGIHitSurface data[];
}
hit_surfaces;

layout(set = 0, binding = 4, std430) restrict readonly buffer HitMaterials {
	NextGIHitMaterial data[];
}
hit_materials;

layout(set = 0, binding = 5, std430) restrict readonly buffer HitTextureSlots {
	uint data[];
}
hit_texture_slots;

layout(set = 0, binding = 6, std430) restrict readonly buffer HitChunk0 {
	uint data[];
}
hit_chunk0;

layout(set = 0, binding = 7, std430) restrict readonly buffer HitChunk1 {
	uint data[];
}
hit_chunk1;

layout(set = 0, binding = 8, std430) restrict readonly buffer HitChunk2 {
	uint data[];
}
hit_chunk2;

layout(set = 0, binding = 9, std430) restrict readonly buffer HitChunk3 {
	uint data[];
}
hit_chunk3;

layout(set = 0, binding = 10, std430) restrict readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;

#define SAMPLERS_BINDING_FIRST_INDEX 11
#include "../samplers_inc.glsl"

// The check (nextgi_hit_base.glsl's ray generation stage): the SRC material atlas, its cards, the jobs and
// the counters.
layout(set = 0, binding = 23) uniform texture2D hit_check_albedo;
layout(set = 0, binding = 24) uniform texture2D hit_check_normal;
layout(set = 0, binding = 25) uniform texture2D hit_check_orm;
layout(set = 0, binding = 26) uniform texture2D hit_check_depth;

layout(set = 0, binding = 27, std430) restrict readonly buffer HitCheckCards {
	NextGICard data[];
}
hit_check_cards;

layout(set = 0, binding = 28, std430) restrict readonly buffer HitCheckJobs {
	uvec4 data[];
}
hit_check_jobs;

layout(set = 0, binding = 29, std430) restrict buffer HitCheckCounters {
	uint data[];
}
hit_check_counters;

// GodotNext: path_tracer (#167). The path tracer's ray generation stage (path_tracer_hit.glsl): the view's history,
// its params (PathTracerGPUParams in path_tracer.cpp), the D16 light table and the D16 material table, one entry
// per surface record. The check binds stand-ins, as the path tracer does for the check's bindings.
struct NextGIPTMaterial {
	vec4 albedo_metal;
	vec4 emission_roughness;
	vec4 specular_mode; // specular, mode, cull mode (RSE::CullMode), unused
};

layout(rgba32f, set = 0, binding = 30) restrict uniform image2D hit_pt_accumulation;

layout(set = 0, binding = 31, std430) restrict readonly buffer HitPTParams {
	vec4 cam_origin_fov;
	vec4 cam_forward_aspect;
	vec4 cam_up;
	vec4 cam_right;
	uvec4 dispatch; // width, height, sample_index, mode (0 full)
	uvec4 counts; // seed, light_count, surface_count, record_count
	vec4 sky_color;
	vec4 exposure_clear; // x exposure, y clear
	uvec4 product; // x max bounces, y flags, z firefly clamp (float bits)
	// The product's camera rays (PathTracerGPURays): the near-plane point at the view's center (w: 1 orthogonal) and
	// its change per normalized device x and y (w: the ray cone's width at its origin and its spread).
	vec4 ray_center;
	vec4 ray_dx;
	vec4 ray_dy;
}
hit_pt_params;

layout(set = 0, binding = 32, std430) restrict readonly buffer HitPTLights {
	vec4 data[];
}
hit_pt_lights;

layout(set = 0, binding = 33, std430) restrict readonly buffer HitPTMaterials {
	NextGIPTMaterial data[];
}
hit_pt_materials;

layout(set = 1, binding = 0) uniform texture2D hit_textures[];

// Indexes the texture table with a run-time index. glslang sizes an unsized array that no code indexes
// that way to one element, which is not a bindless table: this keeps it a run-time array in every stage,
// whether the stage reads it or not.
vec4 nextgi_hit_texture_table_probe(uint p_slot) {
	return texelFetch(hit_textures[nonuniformEXT(p_slot)], ivec2(0), 0);
}

layout(push_constant, std430) uniform HitParams {
	float time;
	// The CPU-fed TLAS instances (NextGIHitMaterials::get_record_count()): an instance with a custom index at or
	// past it was written by the GPU (particles, dynamic MultiMesh) and has no record.
	uint record_count;
	uint job_count;
	uint sample_count;
	uint material_count;
	uint flags; // NEXTGI_HIT_PARAMS_* bits.
	uint pad0;
	uint pad1;
}
hit_params;

// The caller compares with the surface cache's material capture (the check). The capture renders its cards
// without the camera's Y flip, so its gl_FrontFacing is inverted; the material stages then decide the side
// the same way. Consumers that shade hits never set it.
#define NEXTGI_HIT_PARAMS_CAPTURE_FACING 1u

// The payload (32 bytes): shading = (albedo RGBA8 with the opacity in alpha, octahedral normal
// 2x snorm16, roughness | metallic << 8 | ao << 16 | flags << 24, emission R11G11B10F); hit = (t, material
// index, the group record's shader slot, instance custom index). The caller passes the ray cone in hit.xy
// (width at the origin, spread angle per unit length), which the hit shaders read before they write. No
// member is called "material": the material stages define that name for the material code.
struct NextGIHitPayload {
	uvec4 shading;
	uvec4 hit;
};

#define NEXTGI_HIT_PAYLOAD_HIT 1u
#define NEXTGI_HIT_PAYLOAD_FALLBACK 2u
// The ray hit the triangle's front face as the rasterizer's gl_FrontFacing decides it: by the winding of the
// transformed corners (Godot's front faces wind clockwise as seen from the viewer).
#define NEXTGI_HIT_PAYLOAD_FRONT_FACE 4u
// Bit 3: the group has an any-hit stage.
#define NEXTGI_HIT_PAYLOAD_ANY_HIT_GROUP 8u

// NextGIHitCheckCounter (next_gi_hit_materials.h); the per-material sample counts follow the counters.
#define NEXTGI_HIT_CHECK_SAMPLES_COUNTER 0u
#define NEXTGI_HIT_CHECK_HITS_COUNTER 1u
#define NEXTGI_HIT_CHECK_FALLBACK_COUNTER 2u
#define NEXTGI_HIT_CHECK_ALBEDO_COUNTER 3u
#define NEXTGI_HIT_CHECK_NORMAL_COUNTER 4u
#define NEXTGI_HIT_CHECK_ROUGHNESS_COUNTER 5u
#define NEXTGI_HIT_CHECK_METALLIC_COUNTER 6u
#define NEXTGI_HIT_CHECK_OPACITY_COUNTER 7u
#define NEXTGI_HIT_CHECK_RECORD_COUNTER 8u
#define NEXTGI_HIT_CHECK_ALL_COUNTER 9u
#define NEXTGI_HIT_CHECK_ANY_HIT_SAMPLES_COUNTER 10u
#define NEXTGI_HIT_CHECK_ANY_HIT_AGREE_COUNTER 11u
#define NEXTGI_HIT_CHECK_ALPHA_TESTED_COUNTER 12u
#define NEXTGI_HIT_CHECK_ALPHA_CUT_COUNTER 13u
#define NEXTGI_HIT_CHECK_PROBE_HITS_COUNTER 14u
#define NEXTGI_HIT_CHECK_PROBE_GPU_INSTANCES_COUNTER 15u
#define NEXTGI_HIT_CHECK_PROBE_GPU_FALLBACK_COUNTER 16u
#define NEXTGI_HIT_CHECK_MATERIALS_BASE 32u

// The arena chunks bound below (NEXTGI_HIT_ARENA_CHUNKS).
#define NEXTGI_HIT_ARENA_CHUNKS 4u

uint nextgi_hit_chunk_uint(uint chunk, uint byte_offset) {
	uint word = byte_offset >> 2u;
	if (chunk == 0u) {
		return hit_chunk0.data[word];
	}
	if (chunk == 1u) {
		return hit_chunk1.data[word];
	}
	if (chunk == 2u) {
		return hit_chunk2.data[word];
	}
	return hit_chunk3.data[word];
}

vec3 nextgi_hit_chunk_vec3(uint chunk, uint byte_offset) {
	return vec3(uintBitsToFloat(nextgi_hit_chunk_uint(chunk, byte_offset)), uintBitsToFloat(nextgi_hit_chunk_uint(chunk, byte_offset + 4u)), uintBitsToFloat(nextgi_hit_chunk_uint(chunk, byte_offset + 8u)));
}

vec2 nextgi_hit_oct_encode(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 e = n.xy;
	if (n.z < 0.0) {
		e = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	}
	return e;
}

vec3 nextgi_hit_oct_decode(vec2 e) {
	vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

// Positive floats as R11G11B10F: the half float's bits, truncated to 6 and 5 mantissa bits.
uint nextgi_hit_pack_r11g11b10(vec3 c) {
	c = clamp(c, vec3(0.0), vec3(65000.0));
	uint r = packHalf2x16(vec2(c.r, 0.0)) & 0x7FFFu;
	uint g = packHalf2x16(vec2(c.g, 0.0)) & 0x7FFFu;
	uint b = packHalf2x16(vec2(c.b, 0.0)) & 0x7FFFu;
	return (r >> 4u) | ((g >> 4u) << 11u) | ((b >> 5u) << 22u);
}

vec3 nextgi_hit_unpack_r11g11b10(uint p) {
	float r = unpackHalf2x16((p & 0x7FFu) << 4u).x;
	float g = unpackHalf2x16(((p >> 11u) & 0x7FFu) << 4u).x;
	float b = unpackHalf2x16(((p >> 22u) & 0x3FFu) << 5u).x;
	return vec3(r, g, b);
}

// Whether a hit has records to read: its instance was fed from the CPU, its geometry is one of the record's
// surfaces and its triangles sit in a bound arena chunk. GPU-written instances have none.
bool nextgi_hit_fetch_record(uint p_custom_index, uint p_geometry_index, out NextGIInstanceRecord r_record) {
	if (p_custom_index >= hit_params.record_count) {
		return false;
	}
	r_record = hit_records.data[p_custom_index];
	return p_geometry_index < r_record.surface_count && r_record.arena_chunk < NEXTGI_HIT_ARENA_CHUNKS;
}

// The payload of a hit without records: a fallback hit facing the ray, with no material.
NextGIHitPayload nextgi_hit_unrecorded_payload(vec3 p_ray_dir, float p_t, uint p_shader_slot, uint p_custom_index) {
	NextGIHitPayload p;
	uint flags = NEXTGI_HIT_PAYLOAD_HIT | NEXTGI_HIT_PAYLOAD_FALLBACK | NEXTGI_HIT_PAYLOAD_FRONT_FACE;
	p.shading = uvec4(packUnorm4x8(vec4(0.5, 0.5, 0.5, 1.0)), packSnorm2x16(nextgi_hit_oct_encode(-p_ray_dir)), 255u | (255u << 16u) | (flags << 24u), 0u);
	p.hit = uvec4(floatBitsToUint(p_t), 0xFFFFFFFFu, p_shader_slot, p_custom_index);
	return p;
}

#endif // NEXTGI_HIT_LAYOUT_INC_GLSL
