/**************************************************************************/
/*  next_gi_shadow_policy.h                                               */
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

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/typedefs.h"

namespace RendererRD {

// GodotNext: nextgi_shadow_policy (#145, plan §21.3 contract M4). The RT visibility policy that ray-traced
// shadows (#141), MegaLights (#146) and the SRC relight's shadow rays (D14) share through
// nextgi_shadow_ray_inc.glsl. Everything here is device-free, so the doctests run it without a GPU.
//
// Godot has 20 render layers and a 32-bit caster mask per light; a TLAS instance has an 8-bit mask. The bits:
// - 0-5: the 6 most frequent distinct caster masks of the shadowed lights (masked to the 20 layers), reassigned
//   only after the new choice has held for 60 frames. Every shadow caster whose layers meet allocated mask i
//   carries bit i, whatever its material: a light with an allocated mask traces with cull mask (1 << i) and sees
//   exactly its casters, so it needs no layer check.
// - 6: shadow casters whose layers meet none of the allocated masks.
// - 7: the instances GI rays see. GI rays trace with cull mask 0x80, so GI sees the same instances as before
//   (#61's rule), and shadow-only instances (alpha-tested and translucent surfaces, which D17 keeps out of GI)
//   never reach a GI ray.
// A light whose mask has no bit traces bits 0-6 with every hit a candidate and checks each candidate's 20-bit
// layer word (the fallback). Alpha-tested casters are non-opaque instances: their candidates sample the
// material's alpha (at most 4 samples per ray, then opaque). Translucent casters shadow as opaque, as UE 5.9's
// hardware ray tracing feature table says ("The translucent shadow is treated as opaque", lead question LQ6):
// they are opaque instances without the GI bit.

enum {
	NEXTGI_SHADOW_LAYER_MASK = 0xFFFFF, // Godot's 20 render layers.
	NEXTGI_SHADOW_DYNAMIC_BITS = 6,
	NEXTGI_SHADOW_MASK_DYNAMIC = 0x3F,
	NEXTGI_SHADOW_MASK_UNALLOCATED = 1 << 6,
	NEXTGI_SHADOW_MASK_GI = 1 << 7,
	NEXTGI_SHADOW_MASK_CASTERS = NEXTGI_SHADOW_MASK_DYNAMIC | NEXTGI_SHADOW_MASK_UNALLOCATED,
	NEXTGI_SHADOW_HYSTERESIS_FRAMES = 60,
	NEXTGI_SHADOW_MAX_ALPHA_SAMPLES = 4,
	// The bindless-texture alpha path: the slots of NextGI's texture table (within RDBindless::MIN_SLOTS), which the
	// material hit groups share (NEXTGI_HIT_TABLE_SLOTS: Direct3D 12 holds two texture or storage-buffer tables at a
	// time), and the RT arena chunks the shaders bind (a caster whose triangles or stream sit in a later chunk
	// shadows as opaque).
	NEXTGI_SHADOW_TEXTURE_SLOTS = 16384,
	NEXTGI_SHADOW_ALPHA_CHUNKS = 4,
	// Rays per frame and consumer the RT-pipeline retrace takes (the SRC relight keeps them inside its R2 ray
	// budget too). A ray that finds the records full keeps its last answer (the relight) or shadows as opaque.
	NEXTGI_SHADOW_RETRACE_MAX_RECORDS = 16384,
};

// The light word (trace_shadow()'s light_mask_mode): bits 0-7 the cull mask; LOOP traces with a candidate loop
// (some alpha-tested caster carries a bit of the cull mask, or the light falls back); FALLBACK makes every hit a
// candidate (gl_RayFlagsNoOpaqueEXT) and checks its layer word. A word of 0 traces nothing: the light's caster
// mask covers no layer, so nothing shadows it.
enum : uint32_t {
	NEXTGI_SHADOW_MODE_CULL_MASK = 0xFFu,
	NEXTGI_SHADOW_MODE_LOOP = 1u << 8,
	NEXTGI_SHADOW_MODE_FALLBACK = 1u << 9,
};

// What a surface is to a shadow ray, from its effective material (the instance's override, its surface
// override, then the mesh's material).
enum NextGIShadowClass : uint32_t {
	NEXTGI_SHADOW_CLASS_OPAQUE = 0,
	NEXTGI_SHADOW_CLASS_ALPHA_TESTED = 1, // Alpha scissor or hash without nextgi_rt_opaque.
	NEXTGI_SHADOW_CLASS_TRANSLUCENT = 2, // Alpha blended: an opaque shadow (LQ6).
};

// The per-instance word of the shadow instance buffer (one uvec2 per TLAS custom index):
// x: bits 0-19 the layers it shadows with (0 when it casts no shadow), bit 20 ALPHA (the instance is non-opaque:
//    its geometries have entries in the alpha table); y: its first alpha table entry (geometry g is y + g).
enum : uint32_t {
	NEXTGI_SHADOW_INSTANCE_ALPHA = 1u << 20,
};

// The alpha table entry of a geometry of a non-opaque instance. flags: bits 0-1 the NextGIShadowClass, bit 2
// TEXTURE (the bindless-texture path can evaluate it: texture_slot, the threshold and the UV transform are valid
// and the surface has an attribute stream), bit 3 RETRACE (its alpha comes from shader code: the RT-pipeline
// retrace evaluates it), bit 4 HASH (alpha hash, evaluated as a scissor at 0.5), bit 5 CUT (BaseMaterial3D
// without an albedo texture whose albedo alpha is below the threshold: it casts nothing), bits 6 and 7 the albedo
// texture's sampler as the material declares it: NEAREST (a nearest filter) and CLAMP (repeat disabled). The
// shaders emulate both with the one linear repeating sampler they bind, at the top mip.
// An alpha-tested geometry with none of TEXTURE, RETRACE and CUT shadows as opaque.
enum : uint32_t {
	NEXTGI_SHADOW_ALPHA_CLASS_MASK = 0x3u,
	NEXTGI_SHADOW_ALPHA_TEXTURE = 1u << 2,
	NEXTGI_SHADOW_ALPHA_RETRACE = 1u << 3,
	NEXTGI_SHADOW_ALPHA_HASH = 1u << 4,
	NEXTGI_SHADOW_ALPHA_CUT = 1u << 5,
	NEXTGI_SHADOW_ALPHA_NEAREST = 1u << 6,
	NEXTGI_SHADOW_ALPHA_CLAMP = 1u << 7,
};

// Whether the material retrace can decide a caster whose alpha comes from shader code: its shader has a material
// hit group with an any-hit stage (#130; GodotNext: nextgi_hit_discard (#304): alpha tests and discard in
// fragment()). Hit groups refuse some shaders (discard in a helper function, derivatives, screen reads, ...,
// nextgi_hit_classify()); their hits take the fallback group, which has no any-hit stage, so a retrace through them
// always reports the ray occluded. The RT scene keeps the verdict per shader code (ShaderData::nextgi_hit_serial)
// while the material stays in the scene, also after the hit groups are freed, so a refused shader does not ask
// for the hit groups again: its casters shadow as opaque without a retrace.
enum NextGIShadowRetraceVerdict : uint8_t {
	NEXTGI_SHADOW_VERDICT_UNKNOWN, // The hit groups have not compiled this code yet: the retrace is tried.
	NEXTGI_SHADOW_VERDICT_RETRACE, // A hit group with an any-hit stage.
	NEXTGI_SHADOW_VERDICT_OPAQUE, // No hit group or no any-hit stage: opaque, no retrace.
};

// The verdict to keep: what the hit groups report now, or the kept one while they report nothing (they are not
// running, or the shader still compiles).
_FORCE_INLINE_ NextGIShadowRetraceVerdict nextgi_shadow_retrace_verdict(NextGIShadowRetraceVerdict p_kept, NextGIShadowRetraceVerdict p_reported) {
	return p_reported != NEXTGI_SHADOW_VERDICT_UNKNOWN ? p_reported : p_kept;
}

// GodotNext: nextgi_shadow_dual_route (#307). Pure routing decision for one shadow-casting geometry's alpha
// evaluation:
// - use_texture_alpha: fill the bindless texture-alpha (or constant-alpha cut) fields on the alpha entry.
// - poll_verdict: whether the caller may poll NextGIHitMaterials for the shader's retrace verdict (false when
//   the Hardware Ray Tracing Mode cannot retrace, so Simplified Material / Inline never latch OPAQUE).
// - use_retrace: set NEXTGI_SHADOW_ALPHA_RETRACE so Full-mode allocated lights defer to the RT-pipeline any-hit
//   stage.
// When a BaseMaterial3D-pattern shader also uses `discard`, both use_texture_alpha and use_retrace are true when
// retrace is available and not refused: nextgi_shadow_ray_inc.glsl retraces at Full mode and falls back to the
// texture-alpha cut (instead of opaque) wherever retrace cannot run (Simplified Material, Inline, GPU-written
// instances, fallback lights, or shaders refused by the hit groups such as helper-function discard per #304).
struct NextGIShadowAlphaRoute {
	bool use_texture_alpha = false;
	bool poll_verdict = false;
	bool use_retrace = false;
};

NextGIShadowAlphaRoute nextgi_shadow_alpha_route(
		NextGIShadowClass p_shadow_class,
		bool p_code_has_texture_alpha,
		bool p_uses_discard,
		bool p_retrace_mode_available,
		bool p_instance_retrace,
		bool p_has_shader_serial,
		NextGIShadowRetraceVerdict p_verdict);

// The SRC relight's retrace turn (nextgi_src_relight_inc.glsl). On frame f a mask texel traces the page's shadowed
// lights in a pair, slots (2f, 2f + 1) mod n, and appends at most one retrace record, for the first light of the
// pair that needs one. The pairs repeat every n / 2 frames for an even n and every n frames for an odd n; which
// light of a pair goes first flips on every visit of the pair, so each light goes first within two visits. (The
// frame's parity alone would never flip a pair whose visits all fall on frames of one parity: with 4 or 8 lights,
// some lights would never get a record.) Returns 0 when the pair's first slot goes first, 1 when its second does.
_FORCE_INLINE_ uint32_t nextgi_shadow_retrace_turn(uint32_t p_frame, uint32_t p_shadowed_count) {
	const uint32_t period = (p_shadowed_count & 1u) != 0u ? p_shadowed_count : p_shadowed_count / 2u;
	return (p_frame / MAX(period, 1u)) & 1u;
}

// What the bindless-texture path needs from an alpha-tested material: BaseMaterial3D's generated alpha
// (ALPHA *= albedo.a * albedo_tex.a, the albedo texture at UV1 with its scale and offset, a scissor or hash).
// Any other alpha (a ShaderMaterial, triplanar or point UVs, MSDF, vertex color in the albedo, height mapping)
// comes from shader code: valid is false and the RT-pipeline retrace evaluates the material.
struct NextGIShadowTextureAlpha {
	bool valid = false;
	bool hash = false;
	bool has_texture = false;
	float threshold = 0.5f;
	float alpha_scale = 1.0f;
	float uv_scale[2] = { 1.0f, 1.0f };
	float uv_offset[2] = { 0.0f, 0.0f };
};

// Reads p_code (the material shader's source) for BaseMaterial3D's alpha. The parameters come from the caller.
bool nextgi_shadow_code_has_texture_alpha(const String &p_code, bool &r_hash);
// The NEXTGI_SHADOW_ALPHA_NEAREST and NEXTGI_SHADOW_ALPHA_CLAMP flags of BaseMaterial3D's albedo texture uniform
// (its filter_* and repeat_* hints) in p_code.
uint32_t nextgi_shadow_code_albedo_sampler_flags(const String &p_code);

// std430, 48 bytes (nextgi_shadow_ray_inc.glsl's NextGIShadowAlpha).
struct NextGIShadowAlphaEntry {
	uint32_t flags = NEXTGI_SHADOW_CLASS_OPAQUE;
	uint32_t texture_slot = 0; // Bindless texture table slot of the albedo texture.
	uint32_t attribute_chunk = UINT32_MAX; // The surface's attribute stream (#130's layout).
	uint32_t attribute_offset = 0;
	float threshold = 0.5f;
	float alpha_scale = 1.0f; // The albedo color's alpha.
	uint32_t index_chunk = 0; // The geometry's triangles (its surface record's index range).
	uint32_t index_offset = 0;
	float uv_scale[2] = { 1.0f, 1.0f };
	float uv_offset[2] = { 0.0f, 0.0f };
};
static_assert(sizeof(NextGIShadowAlphaEntry) == 48, "NextGIShadowAlphaEntry is three vec4 (nextgi_shadow_ray_inc.glsl).");

// The retrace records (nextgi_shadow_ray_inc.glsl's NextGIShadowRetrace): a 16-byte header (records appended,
// capacity), then one record per ray. The consumer writes word and word2; the retrace's ray
// generation stage (nextgi_hit_base.glsl, NextGIHitMaterials::trace_shadow_retrace()) writes result.
enum : uint32_t {
	NEXTGI_SHADOW_RETRACE_PENDING = 0,
	NEXTGI_SHADOW_RETRACE_OCCLUDED = 1,
	NEXTGI_SHADOW_RETRACE_CLEAR = 2,
};

struct NextGIShadowRetraceRecord {
	uint32_t word = 0;
	uint32_t light_mode = 0;
	uint32_t word2 = 0;
	uint32_t result = NEXTGI_SHADOW_RETRACE_PENDING;
	float origin[3] = {};
	float tmax = 0.0f;
	float direction[3] = {};
	uint32_t pad = 0;
};
static_assert(sizeof(NextGIShadowRetraceRecord) == 48, "NextGIShadowRetraceRecord is three uvec4 (nextgi_shadow_ray_inc.glsl).");

_FORCE_INLINE_ uint32_t nextgi_shadow_retrace_buffer_size(uint32_t p_capacity) {
	return 16 + p_capacity * uint32_t(sizeof(NextGIShadowRetraceRecord));
}

// One caster mask's share of the shadowed lights.
struct NextGIShadowMaskCount {
	uint32_t mask = 0;
	uint32_t count = 0;
};

// The 6 most frequent distinct non-zero masks (layers only), most frequent first, ties by the lower mask.
void nextgi_shadow_top_masks(const uint32_t *p_caster_masks, uint32_t p_count, LocalVector<NextGIShadowMaskCount> &r_top);

class NextGIShadowMaskAllocator {
public:
	struct Allocation {
		uint32_t masks[NEXTGI_SHADOW_DYNAMIC_BITS] = {};
		uint32_t used = 0; // Bit i is set when masks[i] is allocated.
		uint64_t generation = 0; // Bumped on every change.

		int find(uint32_t p_layers) const;
		uint32_t count() const;
		bool same_set(const uint32_t *p_masks, uint32_t p_count) const;
	};

private:
	Allocation allocation;
	uint32_t pending[NEXTGI_SHADOW_DYNAMIC_BITS] = {};
	uint32_t pending_count = 0;
	uint32_t stable_frames = 0;
	bool has_allocation = false;

	void _adopt(const uint32_t *p_masks, uint32_t p_count);

public:
	// Once per frame, before the TLAS phase, with the caster masks of every shadowed light. The first non-empty
	// choice applies at once (nothing to keep stable yet). After that, a different choice applies only once it
	// has been the choice on NEXTGI_SHADOW_HYSTERESIS_FRAMES consecutive frames; masks in both keep their bits.
	// Returns true when the allocation changed.
	bool update(const uint32_t *p_caster_masks, uint32_t p_count);
	const Allocation &get_allocation() const { return allocation; }
	uint32_t get_stable_frames() const { return stable_frames; }
	uint32_t get_pending_count() const { return pending_count; }
	void reset();
};

// The 8-bit TLAS mask of an instance. p_shadow_layers is 0 when the instance casts no shadow; p_gi_visible
// keeps it in GI rays.
uint32_t nextgi_shadow_instance_mask(const NextGIShadowMaskAllocator::Allocation &p_allocation, uint32_t p_shadow_layers, bool p_gi_visible);

// The light word of a light with caster mask p_caster_mask. p_alpha_bits is the OR of the masks of this frame's
// non-opaque (alpha-tested) instances: an allocated light without one of them takes the opaque fast path.
uint32_t nextgi_shadow_light_mode(const NextGIShadowMaskAllocator::Allocation &p_allocation, uint32_t p_caster_mask, uint32_t p_alpha_bits);

// GodotNext: nextgi_skinned_alpha_shadows (#302). How a posed skinned surface enters the TLAS:
// - gi_visible: true unless the mesh surface's own material is alpha-tested or blended (#61 / D17).
// - shadow_only: !gi_visible and casts_shadow (deferred after GI instances, no GI mask bit).
// - skip: !gi_visible and !casts_shadow (neither GI nor shadow rays can see it).
// - alpha_tested / force_no_opaque: casts_shadow and the effective material is alpha-tested.
struct NextGISkinnedShadowPolicy {
	bool gi_visible = false;
	bool shadow_only = false;
	bool skip = false;
	bool alpha_tested = false;
	bool force_no_opaque = false;
};

NextGISkinnedShadowPolicy nextgi_shadow_skinned_policy(bool p_mesh_material_is_alpha, NextGIShadowClass p_effective_shadow_class, uint32_t p_shadow_layers);

// A CPU model of trace_shadow() for the doctests. One entry per triangle the ray crosses inside its interval,
// in the order traversal reports them.
struct NextGIShadowModelHit {
	uint32_t mask = 0; // The instance's TLAS mask.
	uint32_t layers = 0; // Its shadow layer word.
	bool opaque_instance = true; // False for an alpha (non-opaque) instance.
	NextGIShadowClass geometry_class = NEXTGI_SHADOW_CLASS_OPAQUE;
	bool alpha_kept = true; // An alpha-tested geometry: the sample keeps the texel.
	bool retrace = false; // An alpha-tested geometry whose alpha comes from shader code (NEXTGI_SHADOW_ALPHA_RETRACE).
	// GodotNext: nextgi_shadow_dual_route (#307). For dual-route entries (both RETRACE and TEXTURE/CUT),
	// texture_alpha is true and texture_alpha_kept is the inline texture-alpha decision used when retrace
	// cannot run (Simplified Material, Inline, GPU-written, fallback light, or refused shader).
	bool texture_alpha = false;
	bool texture_alpha_kept = true;
};

struct NextGIShadowModelResult {
	bool occluded = false; // trace_shadow() returns 0.
	bool deferred = false; // nextgi_shadow_retrace: the caller appends the ray to the retrace records.
	bool retraced_occluded = false; // With deferred, the retrace's answer: the material any-hit stages decide.
	uint32_t alpha_samples = 0;
};

// trace_shadow() on the CPU. p_retrace: the retrace records have a capacity this frame. Without the retrace, a
// candidate whose alpha comes from shader code shadows as opaque (it still counts as an alpha sample).
NextGIShadowModelResult nextgi_shadow_model_trace(uint32_t p_mode, uint32_t p_caster_mask, const NextGIShadowModelHit *p_hits, uint32_t p_count, bool p_retrace);
// True when trace_shadow() reports the ray occluded, without the retrace.
bool nextgi_shadow_model_occluded(uint32_t p_mode, uint32_t p_caster_mask, const NextGIShadowModelHit *p_hits, uint32_t p_count);
// True when the raster path's caster rule occludes it: a hit of an instance whose layers meet the light's caster
// mask (renderer_scene_cull's shadow pass), whose texel the alpha test keeps.
bool nextgi_shadow_reference_occluded(uint32_t p_caster_mask, const NextGIShadowModelHit *p_hits, uint32_t p_count);

// GM-4's caster-mask matrix (plan §21.3.3): rays the GPU traces through trace_shadow() and the occlusion the raster
// caster rule gives them, computed on the CPU from the scene's triangles. A ray counts only where the answer is
// unambiguous: every triangle it crosses or passes is at least a margin away from the ray's interval ends and
// from its own edges, so float differences between the CPU and the GPU traversal cannot flip it.
struct NextGIShadowCheckInstance {
	Transform3D transform;
	AABB world_bounds;
	uint32_t layers = 0; // Shadow layer word (0: casts no shadow).
	uint32_t vertex_count = 0;
	const float *positions = nullptr; // Object space, 3 floats per vertex.
	uint32_t index_count = 0;
	const uint32_t *indices = nullptr;
	bool opaque_caster = true; // False for alpha-tested surfaces: rays that touch them are left out.
};

struct NextGIShadowCheckRay {
	float origin[3] = {};
	float tmax = 0.0f;
	float direction[3] = {};
	uint32_t light = 0; // Index into the check's caster masks.
};
static_assert(sizeof(NextGIShadowCheckRay) == 32, "NextGIShadowCheckRay is two vec4 (nextgi_shadow_check.glsl).");

enum {
	NEXTGI_SHADOW_CHECK_LIGHTS = 8,
	NEXTGI_SHADOW_CHECK_RAYS = 65536,
};

// The 8 caster masks of the matrix: all layers, single layers, and two multi-layer masks.
void nextgi_shadow_check_masks(uint32_t r_masks[NEXTGI_SHADOW_CHECK_LIGHTS]);

// Builds up to p_ray_count rays (NEXTGI_SHADOW_CHECK_RAYS) aimed at the single-layer casters, cycling through the
// lights and the layers, and their expected occlusion (1 occluded). Rays are drawn from a fixed seed and
// redrawn while ambiguous. Returns the number built; fewer when the scene has no single-layer opaque caster.
uint32_t nextgi_shadow_check_build(const NextGIShadowCheckInstance *p_instances, uint32_t p_instance_count, const uint32_t p_masks[NEXTGI_SHADOW_CHECK_LIGHTS], uint32_t p_ray_count, uint32_t p_seed, LocalVector<NextGIShadowCheckRay> &r_rays, LocalVector<uint8_t> &r_expected);

// The ray's occlusion by one instance's triangles: 1 occluded, 0 clear, -1 ambiguous (a crossing within the
// margin of an edge or of the interval ends).
int nextgi_shadow_check_instance_hit(const NextGIShadowCheckInstance &p_instance, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax);

} // namespace RendererRD
