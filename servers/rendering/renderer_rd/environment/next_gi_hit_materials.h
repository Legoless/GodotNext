/**************************************************************************/
/*  next_gi_hit_materials.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"
#include "servers/rendering/shader_compiler.h"

class RenderDataRD;
class RenderGeometryInstance;

namespace RendererRD {

class NextGIArenaAllocator;
class NextGIRTScene;
class NextGISRC;

// GodotNext: nextgi_hit_groups (#130, plan §21.2 L7). Material hit groups: each Godot spatial material
// shader compiles to a closest-hit stage (and an any-hit stage when it alpha-tests or discards, #304),
// linked through #179's pipeline libraries into one RT pipeline whose hit shader binding table maps every
// geometry of the NextGI TLAS to its material shader's group. Textures are read through #176's bindless
// tables, and the hit shaders read the vertex attributes from an attribute stream in the RT arena.
// Everything above NextGIHitMaterials is device-free so doctests run it without a GPU.
//
// Nothing here runs unless a consumer asks for it and the hit groups can run (nextgi_hit_availability()):
// otherwise there is no stream in the arena, no table, no pipeline, and every TLAS instance keeps
// hit_sbt_range 0.
//
// The contract for callers (#129 hit lighting, #145's material-evaluation retrace, translucency, far field):
// - GI rays trace the instances with mask bit 7 (next_gi_shadow_policy.h) with gl_RayFlagsOpaqueEXT, so they
//   run closest-hit stages only: alpha-tested materials (only those traced as opaque enter the GI parts, #61)
//   are hit as opaque there, as BaseMaterial3D's nextgi_rt_treat_as_opaque says. GodotNext:
//   nextgi_shadow_policy (#145): an instance with an alpha-tested geometry is
//   ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT for shadow rays (every other instance is
//   FORCE_OPAQUE_BIT), and the shadow-only instances lack bit 7. A trace with gl_RayFlagsNoOpaqueEXT overrides
//   the instance flag, and the any-hit stage of an alpha-tested material's group then runs its alpha scissor or
//   hash test and ignores what it cuts (GodotNext: nextgi_hit_discard (#304): and where fragment() discards); so
//   does a trace without opacity flags on a FORCE_NO_OPAQUE instance (the retrace, trace_shadow_retrace()).
// - HitParams.record_count must be get_record_count(): instances past it are written by the GPU (particles,
//   dynamic MultiMesh), have no record and come back from the fallback group without one.
// - Trace only on frames where has_current_records() is true: the records and shader binding table ranges
//   are rebuilt each TLAS phase, and a relink replaces the hit shader binding table.

enum {
	// Bytes per vertex of the attribute stream: oct normal and oct tangent with the binormal sign
	// (2x snorm16 each), UV and UV2 (2x unorm16 each, over the surface's UV range) and color (RGBA8).
	NEXTGI_HIT_ATTRIBUTE_STRIDE = 20,
	// The stream starts with the surface's UV ranges (NextGIHitStreamHeader), then the vertices.
	NEXTGI_HIT_ATTRIBUTE_HEADER = 32,
	// The payload the hit shaders write: two uvec4 (material, then hit).
	NEXTGI_HIT_PAYLOAD_SIZE = 32,
	// The shader record of every hit group (shader slot, flags and two padding words).
	NEXTGI_HIT_RECORD_SIZE = 16,
	// The fallback range covers a part with up to RS::MAX_MESH_SURFACES geometries. It is the first range of
	// the table, so a GPU-written TLAS instance (SBT offset 0) lands on the fallback group too.
	NEXTGI_HIT_FALLBACK_RANGE = 256,
	// Slots of each bindless table (textures, material parameter buffers). Within RDBindless::MIN_SLOTS.
	NEXTGI_HIT_TABLE_SLOTS = 16384,
	// The check's counters, and the per-material sample counts that follow them.
	NEXTGI_HIT_CHECK_COUNTERS = 32,
	NEXTGI_HIT_CHECK_MAX_SAMPLES = 1 << 22,
	NEXTGI_HIT_CHECK_MAX_MATERIALS = 4096,
	// Arena chunks the hit stages bind (nextgi_hit_layout_inc.glsl). Geometry whose triangles or stream sit in
	// a later chunk has no stream (fallback group, counted).
	NEXTGI_HIT_ARENA_CHUNKS = 4,
	// Dead linked groups (of shaders no material uses any more) that start a relink without them.
	NEXTGI_HIT_RELINK_DEAD_GROUPS = 16,
	// HitParams.flags: the caller compares with the SRC material capture, whose gl_FrontFacing is inverted (the
	// card batch renders without the camera's Y flip), so the material stages decide the side as it does. Only
	// the check sets it.
	NEXTGI_HIT_PARAMS_CAPTURE_FACING = 1,
	// GodotNext: nextgi_shadow_policy (#145). The ray generation stage traces the shadow retrace records
	// (trace_shadow_retrace()).
	NEXTGI_HIT_PARAMS_SHADOW_RETRACE = 2,
};

// Who needs the attribute stream and the hit groups. Hit lighting (#129) sets NEXTGI_HIT_CONSUMER_HIT_LIGHTING
// while ray_lighting_mode is not surface_cache; translucency and far-field consumers set their bits. The
// debug/hit_material_check project setting is the check's consumer.
// A consumer traces through the hit groups only while NextGI::get_hit_availability_for() returns AVAILABLE for
// its bit (then NextGI::get_hit_materials() holds them), and keeps its inline path otherwise.
enum NextGIHitConsumer : uint32_t {
	NEXTGI_HIT_CONSUMER_HIT_LIGHTING = 1u << 0,
	NEXTGI_HIT_CONSUMER_TRANSLUCENCY = 1u << 1,
	NEXTGI_HIT_CONSUMER_FAR_FIELD = 1u << 2,
	NEXTGI_HIT_CONSUMER_CHECK = 1u << 3,
	// GodotNext: nextgi_hwrt_mode (#286). #145's material-evaluation retrace (any-hit groups of materials whose
	// alpha comes from shader code), which #146's MegaLights retrace shares. GodotNext: nextgi_shadow_policy
	// (#145): the shadow rays ask for it while a caster's alpha comes from shader code; at Full only, as the other
	// modes compile no material group (their shader-alpha casters shadow as opaque).
	NEXTGI_HIT_CONSUMER_MATERIAL_RETRACE = 1u << 4,
	// GodotNext: path_tracer (#167). A viewport path traces (RendererRD::PathTracer). It shades hits with the
	// material's own hit group, so it runs on them at Full only; otherwise it traces with its INLINE backend.
	NEXTGI_HIT_CONSUMER_PATH_TRACER = 1u << 5,
};

// GodotNext: path_tracer (#167). The base library's ray generation stages: the check's, then the path tracer's.
enum {
	NEXTGI_HIT_RAYGEN_CHECK = 0,
	NEXTGI_HIT_RAYGEN_PATH_TRACER = 1,
};

// GodotNext: path_tracer (#167), path_tracer_denoise (#168). One path tracer sample per pixel through the hit
// pipeline: the view's RGBA32F history, first-hit albedo and normal guides, the params (PathTracerGPUParams) and
// the D16 light and material tables (path_tracer_hit.glsl).
struct NextGIHitPathTrace {
	RID accumulation;
	RID guide_albedo;
	RID guide_normal;
	RID params;
	RID lights;
	RID materials;
	uint32_t width = 0;
	uint32_t height = 0;
	float time = 0.0f;
};

// GodotNext: nextgi_hwrt_mode (#286, plan §21.1). rendering/global_illumination/nextgi/hardware_ray_tracing_mode,
// UE 5.9's per-platform Hardware Ray Tracing Mode for GodotNext's RT pipelines. UE's fourth mode, Disabled, is
// rendering/global_illumination/nextgi/hardware_ray_tracing = Off. Read once, when NextGI starts.
enum NextGIHitMode {
	// The default. Every consumer runs on the material hit groups, as before the setting existed.
	NEXTGI_HIT_MODE_FULL,
	// The hit pipeline is the base library alone: its fallback group is the one simplified hit shader every
	// geometry uses. No material shader compiles a hit group, and no attribute stream is kept for the hit groups.
	// Only the consumers in NEXTGI_HIT_SIMPLIFIED_CONSUMERS run; the others report
	// NEXTGI_HIT_HWRT_MODE_SIMPLIFIED_MATERIAL and keep their inline path.
	NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL,
	// No hit pipeline at all: no material hit group, no base library and no attribute stream for the hit groups.
	// Every consumer reports NEXTGI_HIT_HWRT_MODE_INLINE and keeps its inline path.
	// GodotNext: nextgi_shadow_policy (#145). In every mode the shadow rays' bindless texture-alpha path, an inline
	// ray query, keeps the streams it reads its UVs from (NextGI::_wants_shadow_streams()).
	NEXTGI_HIT_MODE_INLINE,
	NEXTGI_HIT_MODE_MAX,
};

// Out-of-range values read as Full, the mode that changes nothing.
NextGIHitMode nextgi_hit_mode_from_setting(int p_setting);
const char *nextgi_hit_mode_name(NextGIHitMode p_mode);

// The consumers that can run on the simplified hit shader. The check then traces through the fallback group
// alone. Hit lighting (#129), material retrace (#145), translucency, far field and the path tracer (#167) need the
// material's own group.
inline constexpr uint32_t NEXTGI_HIT_SIMPLIFIED_CONSUMERS = NEXTGI_HIT_CONSUMER_CHECK;

// The consumers p_mode lets run: all of them at Full, the simplified ones at Simplified Material, none at Inline.
uint32_t nextgi_hit_mode_consumers(uint32_t p_consumers, NextGIHitMode p_mode);
// Whether material shaders compile hit groups (and the attribute stream they read exists): Full only.
_FORCE_INLINE_ bool nextgi_hit_mode_material_groups(NextGIHitMode p_mode) {
	return p_mode == NEXTGI_HIT_MODE_FULL;
}

// Why the hit groups do not run although a consumer asked for them. Hit lighting falls back to the surface
// cache in every case but AVAILABLE (plan §21.2 L7, HIT_LIGHTING_UNSUPPORTED).
enum NextGIHitAvailability {
	NEXTGI_HIT_AVAILABLE,
	NEXTGI_HIT_NO_CONSUMER,
	NEXTGI_HIT_NOT_HWRT,
	NEXTGI_HIT_NO_RAYTRACING_PIPELINE,
	NEXTGI_HIT_NO_PIPELINE_LIBRARY,
	NEXTGI_HIT_NO_BINDLESS,
	NEXTGI_HIT_INIT_FAILED, // The base library, its link, the hit shader binding table or the tables failed.
	// GodotNext: nextgi_hwrt_mode (#286). The Hardware Ray Tracing Mode lets none of the consumers run.
	NEXTGI_HIT_HWRT_MODE_INLINE,
	NEXTGI_HIT_HWRT_MODE_SIMPLIFIED_MATERIAL,
	// GodotNext: path_tracer (#167). The previous hit groups were freed within the device's frame delay, and their
	// bindless tables are freed only at its end: Direct3D 12 holds two texture or storage-buffer tables at a time,
	// so new hit groups wait for them (a view that turns path tracing off and on again within a few frames).
	NEXTGI_HIT_RELEASING,
	NEXTGI_HIT_AVAILABILITY_MAX,
};

// What the settings alone decide, known without a frame block: NO_CONSUMER, the mode's reason when it lets none
// of the consumers run, or AVAILABLE when the tier and the device decide.
NextGIHitAvailability nextgi_hit_setting_availability(uint32_t p_consumers, NextGIHitMode p_mode);
// p_init_failed: NextGIHitMaterials failed to initialize since the consumers were last off (latched, so it is
// not created again every frame). The mode comes right after the consumers, as a project setting, before the
// tier and the device (as the tier rule puts SETTING_OFF before the device's reasons).
// GodotNext: path_tracer (#167). p_releasing: nextgi_hit_tables_releasing() for the last free, after the latched
// failure.
NextGIHitAvailability nextgi_hit_availability(uint32_t p_consumers, bool p_hwrt_tier, bool p_raytracing_pipeline, bool p_pipeline_library, bool p_bindless, bool p_init_failed = false, NextGIHitMode p_mode = NEXTGI_HIT_MODE_FULL, bool p_releasing = false);
// One consumer's reason (p_consumer is one NextGIHitConsumer bit): the mode's reason first; then, for a consumer
// that asks (in p_consumers), p_last_frame, what the last frame block found, so it is AVAILABLE only while the hit
// groups exist; for one that does not ask, p_on_its_own, what it would get on this tier and device if it asked.
NextGIHitAvailability nextgi_hit_consumer_availability(uint32_t p_consumer, uint32_t p_consumers, NextGIHitMode p_mode, NextGIHitAvailability p_last_frame, NextGIHitAvailability p_on_its_own);
// GodotNext: path_tracer (#167). Whether the hit groups freed on frame p_freed_frame (UINT64_MAX: never) still hold
// their bindless tables on frame p_frame: the device frees them p_frame_delay frames after the free.
_FORCE_INLINE_ bool nextgi_hit_tables_releasing(uint64_t p_freed_frame, uint64_t p_frame, uint32_t p_frame_delay) {
	return p_freed_frame != UINT64_MAX && p_frame <= p_freed_frame + p_frame_delay;
}
const char *nextgi_hit_availability_name(NextGIHitAvailability p_availability);

// The stream lives in the RT arena and only the material hit groups read it, so it exists only while they run.
// GodotNext: nextgi_shadow_policy (#145). NextGI also keeps it, in every mode, while the shadow rays' texture-alpha
// path needs it; this is the hit groups' part of that decision.
_FORCE_INLINE_ bool nextgi_hit_stream_needed(NextGIHitAvailability p_availability, bool p_rt_arena, NextGIHitMode p_mode = NEXTGI_HIT_MODE_FULL) {
	return p_availability == NEXTGI_HIT_AVAILABLE && p_rt_arena && nextgi_hit_mode_material_groups(p_mode);
}

// The head of a surface's attribute stream: each UV set's minimum and extent. The vertices' UVs are unorm16
// over that range, as Godot's compressed vertex format stores them (16 bits over the surface's range, where a
// half float keeps 11 bits and loses texels on tiled UVs).
struct NextGIHitStreamHeader {
	float uv_min[2] = { 0.0f, 0.0f };
	float uv_extent[2] = { 0.0f, 0.0f };
	float uv2_min[2] = { 0.0f, 0.0f };
	float uv2_extent[2] = { 0.0f, 0.0f };
};
static_assert(sizeof(NextGIHitStreamHeader) == NEXTGI_HIT_ATTRIBUTE_HEADER, "The stream header is 32 bytes (nextgi_hit_material_inc.glsl).");

// One vertex of the attribute stream, as the hit shaders read it.
struct NextGIHitAttributes {
	int16_t normal[2]; // Octahedral, [-1, 1].
	int16_t tangent[2]; // Octahedral, Godot's encoding: |y| * 2 - 1 is the coordinate and sign(y) the binormal sign.
	uint16_t uv[2]; // Unorm16 over the header's UV range.
	uint16_t uv2[2];
	uint8_t color[4];
};
static_assert(sizeof(NextGIHitAttributes) == NEXTGI_HIT_ATTRIBUTE_STRIDE, "The attribute stream is 20 bytes per vertex (plan §21.2 L7).");

// Whether NextGIHitMaterials::add_geometry() looks up a geometry's material, the only path on which a material
// shader compiles a hit group: with material groups (Full), for geometry with an attribute stream in the chunks the
// hit stages bind. Otherwise the geometry uses the fallback group.
bool nextgi_hit_geometry_uses_material(bool p_material_groups, bool p_has_stream, uint32_t p_attribute_chunk, uint32_t p_arena_chunk);

// The bytes of a stream of p_vertex_count vertices.
_FORCE_INLINE_ uint32_t nextgi_hit_stream_size(uint32_t p_vertex_count) {
	return NEXTGI_HIT_ATTRIBUTE_HEADER + p_vertex_count * NEXTGI_HIT_ATTRIBUTE_STRIDE;
}

int16_t nextgi_hit_snorm16(float p_value);
float nextgi_hit_snorm16_to_float(int16_t p_value);
// A [0, 1] unorm16 octahedral component of Godot's uncompressed vertex buffer, as the stream's snorm16.
int16_t nextgi_hit_unorm16_to_snorm16(uint16_t p_value);
// Godot's octahedral decode of a [-1, 1] pair (oct_to_vec3 in oct_inc.glsl).
Vector3 nextgi_hit_oct_decode(const Vector2 &p_oct);

// A UV in [p_min, p_min + p_extent] as unorm16, and back (as unpackUnorm2x16 decodes it).
uint16_t nextgi_hit_pack_uv(float p_value, float p_min, float p_extent);
float nextgi_hit_unpack_uv(uint16_t p_value, float p_min, float p_extent);
void nextgi_hit_pack_vertex(const NextGIHitStreamHeader &p_header, const Vector3 &p_normal, const Vector3 &p_tangent, float p_binormal_sign, const Vector2 &p_uv, const Vector2 &p_uv2, const Color &p_color, NextGIHitAttributes &r_vertex);
// The decoded normal, tangent and binormal sign of a stream vertex, as the hit shaders decode them.
void nextgi_hit_unpack_tbn(const NextGIHitAttributes &p_vertex, Vector3 &r_normal, Vector3 &r_tangent, float &r_binormal_sign);
Vector2 nextgi_hit_unpack_uv0(const NextGIHitStreamHeader &p_header, const NextGIHitAttributes &p_vertex);
Vector2 nextgi_hit_unpack_uv1(const NextGIHitStreamHeader &p_header, const NextGIHitAttributes &p_vertex);

// Packs the decoded arrays of a surface (RenderingServer::mesh_create_arrays_from_surface_data()): the header
// with the UV ranges, then the vertices. Missing arrays take the defaults the raster path uses: normal +Z,
// tangent +X with a positive sign, UVs 0, color white. Returns false when the arrays do not hold
// p_vertex_count vertices.
bool nextgi_hit_pack_arrays(const Array &p_arrays, uint32_t p_vertex_count, Vector<uint8_t> &r_stream);

// Where the stream's inputs sit in an uncompressed surface's vertex and attribute buffers.
struct NextGIHitSourceLayout {
	bool valid = false; // False for compressed or 2D surfaces: their region updates leave the stream stale.
	uint32_t normal_block_offset = 0; // Bytes into the vertex buffer: vertex_count * position stride.
	uint32_t normal_stride = 0; // 8 with tangents, 4 with normals only, 0 with neither.
	int32_t normal_offset = -1;
	int32_t tangent_offset = -1;
	uint32_t attribute_stride = 0;
	int32_t color_offset = -1;
	int32_t uv_offset = -1;
	int32_t uv2_offset = -1;
};

NextGIHitSourceLayout nextgi_hit_source_layout(uint64_t p_format, uint32_t p_vertex_count);

// Applies a mesh_surface_update_vertex_region() (p_attribute_region false) or
// mesh_surface_update_attribute_region() (true) to the stream copy r_stream (header and p_vertex_count
// vertices). Only the components the update covers in full change. A UV outside its set's range widens the
// range and requantizes that set on every vertex. Returns false when it changes nothing; otherwise
// [r_byte_begin, r_byte_end) of the stream is what to upload.
bool nextgi_hit_patch_stream(const NextGIHitSourceLayout &p_layout, uint32_t p_vertex_count, bool p_attribute_region, uint32_t p_offset, const uint8_t *p_data, uint32_t p_size, uint8_t *r_stream, uint32_t &r_byte_begin, uint32_t &r_byte_end);

// Allocates and frees one surface's attribute stream in the RT arena, updating the stream byte total
// MeshStorage reports. Nothing is allocated unless a consumer runs the hit groups (nextgi_hit_stream_needed()).
bool nextgi_hit_allocate_surface_stream(const Array &p_arrays, uint32_t p_vertex_count, NextGIArenaAllocator &r_allocator, uint64_t &r_attribute_bytes, uint32_t &r_chunk, uint32_t &r_offset, uint32_t &r_size, Vector<uint8_t> &r_cpu);
void nextgi_hit_free_surface_stream(NextGIArenaAllocator &r_allocator, uint64_t &r_attribute_bytes, uint32_t &r_chunk, uint32_t &r_offset, uint32_t &r_size, Vector<uint8_t> &r_cpu);

// R11G11B10 float packing of the payload's emission (positive, truncated), and its decode.
uint32_t nextgi_hit_pack_r11g11b10(const Color &p_color);
Color nextgi_hit_unpack_r11g11b10(uint32_t p_packed);

// Why a material shader has no hit group of its own and its hits use the fallback group (plan §7.4's
// resolve-ineligible rows, read for hits).
enum NextGIHitIneligible {
	NEXTGI_HIT_ELIGIBLE,
	NEXTGI_HIT_INELIGIBLE_SCREEN_READ, // Screen, depth or normal-roughness textures, SCREEN_UV, FRAGCOORD.
	NEXTGI_HIT_INELIGIBLE_DERIVATIVE, // dFdx, dFdy, fwidth or textureQueryLod: no derivatives at a hit.
	// GodotNext: nextgi_hit_discard (#304). discard in a helper function: the hit stages leave only fragment() itself
	// early (discard in fragment() is NEXTGI_HIT_DISCARD, ignoreIntersectionEXT in the any-hit stage).
	NEXTGI_HIT_INELIGIBLE_DISCARD,
	NEXTGI_HIT_INELIGIBLE_DEPTH_WRITE,
	NEXTGI_HIT_INELIGIBLE_POSITION_WRITE, // POSITION, MODELVIEW_MATRIX or PROJECTION_MATRIX writes.
	NEXTGI_HIT_INELIGIBLE_TEXTURE_TYPE, // Not a 2D texture, a texture array or a global texture.
	NEXTGI_HIT_INELIGIBLE_INSTANCE_UNIFORM, // Per-instance uniforms are not in the hit records.
	NEXTGI_HIT_INELIGIBLE_BUILTIN, // A built-in the hit has no value for (front facing, vertex ID, bones, ...).
	NEXTGI_HIT_INELIGIBLE_RENDER_TARGET, // A viewport texture: bindless tables are read-only.
	NEXTGI_HIT_INELIGIBLE_COMPILE_FAILED,
	NEXTGI_HIT_INELIGIBLE_NOT_SPATIAL, // No Forward+ spatial shader behind the material.
	NEXTGI_HIT_INELIGIBLE_TABLE_FULL, // A bindless table had no slot for the parameter buffer or a texture.
	NEXTGI_HIT_INELIGIBLE_MAX,
};

const char *nextgi_hit_ineligible_name(NextGIHitIneligible p_reason);

// The hit variant of a spatial shader: the ShaderCompiler output of the same code the raster variants
// compile, generated in hit mode (ShaderCompiler::IdentifierActions::nextgi_hit).
struct NextGIHitShaderSource {
	Vector<String> defines;
	String uniforms; // Members of the material parameter block (std140, as the uniform buffer).
	String globals; // Structs, varyings as plain globals, constants and every function vertex() and fragment() use
	// (not the functions only light() uses: light() never runs at a hit).
	String vertex_code;
	String fragment_code;
	Vector<ShaderCompiler::GeneratedCode::Texture> textures; // In texture order, as MaterialData's texture cache.
	uint32_t ubo_size = 0;
	bool has_light = false; // A custom light(): hits skip it and count it.
	bool alpha_scissor = false;
	bool alpha_hash = false;
	bool screen_read = false;
	bool uses_discard = false;
	bool writes_depth = false;
	bool writes_position = false;
	bool instance_uniforms = false;
};

// The reason a hit source is ineligible, or NEXTGI_HIT_ELIGIBLE. Scans the generated code for built-ins the
// hit template does not provide.
NextGIHitIneligible nextgi_hit_classify(const NextGIHitShaderSource &p_source);

// GodotNext: nextgi_hit_discard (#304). Whether an eligible hit source's group gets an any-hit stage: an alpha
// scissor or hash test, or discard in fragment() (the any-hit stage ignores the intersection where it discards).
_FORCE_INLINE_ bool nextgi_hit_source_has_any_hit(const NextGIHitShaderSource &p_source) {
	return p_source.alpha_scissor || p_source.alpha_hash || p_source.uses_discard;
}

// GodotNext: nextgi_hit_discard (#304). NextGIHitMaterials::get_shadow_retrace_verdict() for one shader:
// OPAQUE without material groups (Simplified Material, #286), UNKNOWN before a material with the shader was seen or
// while it compiles, RETRACE for an eligible shader whose group has an any-hit stage, OPAQUE for any other.
NextGIShadowRetraceVerdict nextgi_hit_shadow_retrace_verdict(bool p_material_groups, bool p_seen, bool p_compiling, bool p_eligible, bool p_any_hit);

// The per-element MultiMesh inputs a hit source reads: the hit records do not carry an element's color or
// custom data, so a MultiMesh element whose material reads one its MultiMesh stores uses the fallback group.
enum {
	NEXTGI_HIT_READS_COLOR = 1u << 0, // COLOR, which the rasterizer multiplies by the element's color.
	NEXTGI_HIT_READS_INSTANCE_CUSTOM = 1u << 1,
};
uint32_t nextgi_hit_element_inputs(const NextGIHitShaderSource &p_source);
bool nextgi_hit_element_needs_fallback(uint32_t p_inputs, bool p_element_colors, bool p_element_custom_data);

// Whether to link the live libraries again without the dead ones (no link may be running then).
_FORCE_INLINE_ bool nextgi_hit_should_relink(uint32_t p_dead_linked_groups, bool p_link_running) {
	return !p_link_running && p_dead_linked_groups >= NEXTGI_HIT_RELINK_DEAD_GROUPS;
}

// The full-table latch of a bindless table the hit groups allocate from. A failed allocation logs, so after one
// the table is tried again only once a slot released from it can be reused: RenderingDevice reuses a released slot
// after the frame delay. The retry frame never moves later, so steady releases cannot keep it shut.
struct NextGIHitTableLatch {
	static constexpr uint64_t NONE = UINT64_MAX;
	bool full = false;
	uint64_t retry_frame = NONE;
	uint64_t last_release_frame = NONE;

	bool may_allocate(uint64_t p_frame) const { return !full || p_frame >= retry_frame; }
	void allocated(bool p_success, uint64_t p_frame, uint32_t p_frame_delay) {
		full = !p_success;
		if (p_success) {
			retry_frame = NONE;
			return;
		}
		// A slot released within the frame delay before this failure was not reusable yet: try again once it is.
		const bool pending = last_release_frame != NONE && last_release_frame + p_frame_delay + 1 > p_frame;
		retry_frame = pending ? last_release_frame + p_frame_delay + 1 : NONE;
	}
	void released(uint64_t p_frame, uint32_t p_frame_delay) {
		last_release_frame = p_frame;
		if (full) {
			retry_frame = MIN(retry_frame, p_frame + p_frame_delay + 1);
		}
	}
};
// The texture defines the hit template reads the material's textures through: texture k of the material is
// slot hit_texture_slots[hit_texture_base + k] of the bindless texture table.
String nextgi_hit_texture_defines(const NextGIHitShaderSource &p_source);

// The key of a hit shader binding table range: the group index of each geometry, in order.
uint64_t nextgi_hit_group_list_key(const uint32_t *p_groups, uint32_t p_count);

// One card of the check: the samples are its covered texels, read from the SRC material atlas.
struct NextGIHitCheckCard {
	uint32_t record = 0; // NextGIInstanceRecord index.
	uint32_t card = 0; // Index into the SRC card buffer.
	uint32_t width = 0; // Atlas rect size in texels.
	uint32_t height = 0;
};

// The check's jobs, one per card: (record, card, first sample, width | height << 16), in sample order. Cards
// that would take the sample count past p_max_samples are left out. Returns the sample count.
uint32_t nextgi_hit_plan_check_jobs(const NextGIHitCheckCard *p_cards, uint32_t p_count, uint32_t p_max_samples, LocalVector<uint32_t> &r_jobs);

// The check counters (nextgi_hit_check.glsl writes them with atomics).
enum NextGIHitCheckCounter {
	NEXTGI_HIT_CHECK_SAMPLES, // Covered atlas texels traced.
	NEXTGI_HIT_CHECK_HITS, // Of them, rays that hit.
	NEXTGI_HIT_CHECK_FALLBACK, // Hits shaded by the fallback group.
	NEXTGI_HIT_CHECK_ALBEDO, // Hits whose albedo matches the atlas within 1/255 per channel.
	NEXTGI_HIT_CHECK_NORMAL, // ... normal within 1/255 per [0, 1]-mapped component.
	NEXTGI_HIT_CHECK_ROUGHNESS,
	NEXTGI_HIT_CHECK_METALLIC,
	NEXTGI_HIT_CHECK_OPACITY, // The closest-hit opacity matches the atlas alpha (0 or 1 after the scissor).
	NEXTGI_HIT_CHECK_RECORD, // The group's shader record names the shader of the geometry's material.
	NEXTGI_HIT_CHECK_ALL, // Albedo, normal, roughness, metallic, opacity and record all match.
	NEXTGI_HIT_CHECK_ANY_HIT_SAMPLES, // Samples traced again without forced opacity (any-hit runs).
	NEXTGI_HIT_CHECK_ANY_HIT_AGREE, // ... whose hit or miss matches the atlas alpha.
	NEXTGI_HIT_CHECK_ALPHA_TESTED, // Samples on materials with an any-hit stage.
	NEXTGI_HIT_CHECK_ALPHA_CUT, // ... whose atlas alpha is 0 (the any-hit must ignore them).
	// A third trace leaves each texel the other way, into what stands in front of the card.
	NEXTGI_HIT_CHECK_PROBE_HITS,
	NEXTGI_HIT_CHECK_PROBE_GPU_INSTANCES, // ... on GPU-written TLAS instances (custom index past the records).
	NEXTGI_HIT_CHECK_PROBE_GPU_FALLBACK, // ... of them, shaded by the fallback group (all must be).
	// GodotNext: nextgi_hit_discard (#304). Texels the capture did not cover, traced too: those whose forced-opaque hit
	// on the card's own instance reports that fragment() discarded there (the closest-hit stage's discard branch ran).
	NEXTGI_HIT_CHECK_DISCARD_CUT,
	NEXTGI_HIT_CHECK_DISCARD_IGNORED, // ... of them, missed without forced opacity (the any-hit stage ignored them).
	NEXTGI_HIT_CHECK_DISCARD_COVERED, // Covered texels whose hit reports a discard (the capture kept them).
	NEXTGI_HIT_CHECK_COUNTER_MAX,
};
static_assert(int(NEXTGI_HIT_CHECK_COUNTER_MAX) <= int(NEXTGI_HIT_CHECK_COUNTERS), "The check counters fit their block.");

const char *nextgi_hit_check_counter_name(NextGIHitCheckCounter p_counter);

// The owner of the hit groups, the bindless tables and the per-frame hit records. Owned by NextGI, created
// only under the HWRT tier and only while a consumer asks for it.
class NextGIHitMaterials {
public:
	struct Counters {
		uint32_t shaders = 0; // Material shaders some material uses (not the dead ones).
		uint32_t groups_linked = 0;
		uint32_t groups_pending = 0; // Compiling, or compiled and waiting for a link.
		uint32_t ineligible = 0; // Shaders and materials whose hits use the fallback group.
		uint32_t ineligible_reasons[NEXTGI_HIT_INELIGIBLE_MAX] = {};
		uint32_t light_skipped = 0; // Shaders with a custom light(), which hits skip.
		uint32_t materials = 0;
		uint32_t geometries = 0; // Geometries of the last TLAS phase.
		uint32_t fallback_geometries = 0; // ... whose group is the fallback group.
		uint32_t no_stream_geometries = 0; // ... without an attribute stream (proxies, skinned slots, chunk 4 on).
		uint32_t element_data_geometries = 0; // MultiMesh elements whose material reads their color or custom data.
		uint32_t groups_dead = 0; // Linked groups of shaders no material uses, until a relink drops them.
		uint64_t relinks = 0;
		uint32_t sbt_ranges = 0;
		uint64_t links = 0;
		double group_add_ms_last = 0.0; // Off-thread: hit GLSL generation, stage compile and library creation.
		double group_add_ms_max = 0.0;
		double link_ms_last = 0.0; // Off-thread: the link that adds new libraries.
		double link_ms_max = 0.0;
		uint64_t check_runs = 0;
		uint32_t check[NEXTGI_HIT_CHECK_COUNTERS] = {};
		uint32_t check_materials = 0; // Materials the last check sampled.
		uint32_t check_pending_shaders = 0; // Shaders not ready when the check last ran.
		uint64_t shadow_retrace_dispatches = 0; // GodotNext: nextgi_shadow_policy (#145)
	};

private:
	struct HitShader;
	struct LinkJob;
	struct Material;
	struct Internal;
	Internal *internal = nullptr;

	static NextGIHitMaterials *singleton;

	bool _initialize();
	void _poll_tasks();
	void _finish_link(LinkJob *p_job);
	void _start_link();
	void _start_relink();
	void _release_dead_shaders();
	void _free_shader(HitShader *p_shader);
	void _set_material_shader(Material &r_material, HitShader *p_shader);
	// Off the render thread: compiles a hit shader's stages and creates its library; links new libraries.
	static void _compile_task(void *p_userdata);
	static void _link_task(void *p_userdata);
	HitShader *_get_shader(void *p_shader_data, uint64_t p_serial);
	Material *_get_material(RID p_material, uint64_t p_frame);
	void _release_material_slots(Material &r_material);
	void _upload_material_tables();
	uint32_t _texture_slot(RID p_texture);
	uint32_t _retry_texture_slot(RID p_texture);
	void _release_texture_slot(RID p_texture);
	// Bindless slots through each table's NextGIHitTableLatch.
	uint32_t _allocate_slot(bool p_texture_table, RID p_resource);
	void _release_slot(bool p_texture_table, uint32_t p_slot);

public:
	static NextGIHitMaterials *get_singleton() { return singleton; }

	// Start of the frame, before the TLAS phase: polls the compile and link tasks and swaps in a newly
	// linked pipeline. False when the hit groups could not be set up (no hit records are written then).
	bool begin_frame(uint64_t p_frame);
	bool is_ready() const;

	// The TLAS phase (NextGIRTScene::rebuild_tlas). Every surface record gets one hit surface record, in the
	// same order: add_geometry() for the geometries of a TLAS instance, then end_instance() for its range.
	// add_fallback_surfaces() covers surface records whose geometry has no material hit group (skinned
	// slots, far proxies). finish_records() uploads the records of the frame. p_arena_chunk is the chunk
	// of the geometry's triangles (the record's arena_chunk); p_multimesh_element marks a MultiMesh element.
	// A MultiMesh element's instance gives the check no cards (end_instance(), p_check_cards false): its
	// card set is the MultiMesh's, not the element's.
	void begin_records();
	void add_geometry(RenderGeometryInstance *p_instance, RID p_mesh, uint32_t p_surface, uint32_t p_arena_chunk, bool p_has_stream, uint32_t p_attribute_chunk, uint32_t p_attribute_offset, bool p_multimesh_element);
	void add_fallback_surfaces(uint32_t p_count);
	int64_t end_instance(uint32_t p_record_index, uint32_t p_card_set, bool p_check_cards);
	int64_t get_fallback_range() const;
	void finish_records(uint32_t p_surface_record_count);
	// The CPU-fed TLAS instances of the last TLAS phase: HitParams.record_count.
	uint32_t get_record_count() const;
	// Whether this frame's TLAS phase wrote the records (traces need them, see the header comment).
	bool has_current_records() const;

	// The debug check (debug/hit_material_check): traces the covered texels of every captured SRC card back
	// into the TLAS through the hit pipeline and compares the payloads with the material atlas.
	void run_check(NextGIRTScene *p_rt_scene, NextGISRC *p_src, float p_time);

	// GodotNext: path_tracer (#167). The path tracer's ray generation stage is in the base library with material
	// groups (Full; a device that could not build it has the hit groups without it, and Simplified Material leaves
	// it out, #286). It traces on frames with current records, on this frame's
	// TLAS, and the pipeline serial changes on every link and relink (the path tracer restarts its history).
	bool has_path_tracer_raygen() const;
	bool can_trace_path_tracer() const;
	uint64_t get_pipeline_serial() const;
	bool trace_path_tracer(NextGIRTScene *p_rt_scene, const NextGIHitPathTrace &p_trace);

	// GodotNext: nextgi_shadow_policy (#145). The shadow rays' material retrace: traces every record of
	// p_records (next_gi_shadow_policy.h, NextGIShadowRetraceRecord, up to p_capacity) through the hit groups with
	// the record's cull mask and writes its result. Only on frames where has_current_records() is true; returns
	// false when it traced nothing.
	bool trace_shadow_retrace(NextGIRTScene *p_rt_scene, RID p_records, uint32_t p_capacity, float p_time);
	// Whether the retrace can evaluate the alpha of the shader code with this serial (ShaderData::nextgi_hit_serial):
	// RETRACE once its hit group with an any-hit stage exists, OPAQUE when the shader is refused or has no any-hit
	// stage (or no material group compiles, Simplified Material), UNKNOWN while it compiles or before a material
	// with it was seen (nextgi_hit_shadow_retrace_verdict()). GodotNext: nextgi_hit_discard (#304): a shader that
	// discards in fragment() has an any-hit stage, so its casters are retraced.
	NextGIShadowRetraceVerdict get_shadow_retrace_verdict(uint64_t p_serial) const;
	// Wake the shared bindless texture-table latch when NextGIRTScene releases slots from the shared table.
	void notify_shared_texture_slot_released();

	const Counters &get_counters() const;
	void fill_counters(Dictionary &r_counters) const;

	// p_material_groups false is the Simplified Material mode (#286): the base library alone, whose fallback group
	// shades every hit; no material hit group compiles.
	// GodotNext: nextgi_shadow_policy (#145). p_texture_table: a bindless texture table to allocate the texture slots
	// from instead of a table of its own (NextGIRTScene's, which the shadow rays' alpha path shares: Direct3D 12 holds
	// two texture or storage-buffer tables at a time). It must outlive this object, which does not free it. The
	// check's, the path tracer's (#167) and the retrace's traces all bind it as set 1.
	explicit NextGIHitMaterials(bool p_material_groups = true, RID p_texture_table = RID());
	~NextGIHitMaterials();
};

} // namespace RendererRD
