// GodotNext: nextgi_shadow_policy (#145, plan §21.3 contract M4). The shadow-ray include that ray-traced
// shadows (#141), MegaLights (#146) and the SRC relight's shadow rays (D14) share, so all of them apply one
// visibility policy (next_gi_shadow_policy.h has the CPU side and a model of this function).
//
// Not a standalone shader. The includer declares #version 460 and GL_EXT_ray_query, then defines, before the
// include:
//   NEXTGI_SHADOW_TLAS              its accelerationStructureEXT uniform;
//   NEXTGI_SHADOW_SET               the descriptor set of the two buffers below;
//   NEXTGI_SHADOW_INSTANCE_BINDING  the shadow instance buffer (one uvec2 per TLAS custom index);
//   NEXTGI_SHADOW_ALPHA_BINDING     the alpha table (one NextGIShadowAlpha per geometry of a non-opaque instance).
// Optional: NEXTGI_SHADOW_ALPHA_TEXTURES selects the bindless-texture alpha path (on devices with bindless
// tables). The includer then also defines NEXTGI_SHADOW_CHUNK0_BINDING to NEXTGI_SHADOW_CHUNK3_BINDING (the
// first four RT arena chunks), NEXTGI_SHADOW_SAMPLER_BINDING and NEXTGI_SHADOW_TEXTURE_SET (the bindless
// texture table, binding 0).
// Optional: NEXTGI_SHADOW_RETRACE_BINDING, the retrace records (NextGIShadowRetrace below). While their capacity
// is not 0, a light with an allocated mask bit leaves the candidates whose alpha comes from shader code to the
// RT-pipeline retrace: trace_shadow() sets nextgi_shadow_retrace when a ray met one and nothing else occluded
// it, and the caller appends the ray with nextgi_shadow_retrace_append(). Without them, and for lights without
// a bit (their layer check needs the candidate loop), those candidates shadow as opaque.
//
// The kernel rules of nextgi_rt_inc.glsl hold, except one: the policy needs a candidate loop (plan §21.3 M4)
// for alpha-tested casters and for lights whose caster mask has no TLAS mask bit. A light with an allocated
// bit and no alpha-tested caster on it takes the opaque fast path: gl_RayFlagsOpaqueEXT and one
// rayQueryProceedEXT, as before.

#ifndef NEXTGI_SHADOW_RAY_INC_GLSL
#define NEXTGI_SHADOW_RAY_INC_GLSL

// next_gi_shadow_policy.h
const uint NEXTGI_SHADOW_LAYER_MASK = 0xFFFFFu;
const uint NEXTGI_SHADOW_MODE_CULL_MASK = 0xFFu;
const uint NEXTGI_SHADOW_MODE_LOOP = 1u << 8u;
const uint NEXTGI_SHADOW_MODE_FALLBACK = 1u << 9u;
const uint NEXTGI_SHADOW_INSTANCE_ALPHA = 1u << 20u;
const uint NEXTGI_SHADOW_CLASS_ALPHA_TESTED = 1u;
const uint NEXTGI_SHADOW_ALPHA_CLASS_MASK = 0x3u;
const uint NEXTGI_SHADOW_ALPHA_TEXTURE = 1u << 2u;
const uint NEXTGI_SHADOW_ALPHA_RETRACE = 1u << 3u;
const uint NEXTGI_SHADOW_ALPHA_HASH = 1u << 4u;
const uint NEXTGI_SHADOW_ALPHA_CUT = 1u << 5u;
const uint NEXTGI_SHADOW_ALPHA_NEAREST = 1u << 6u;
const uint NEXTGI_SHADOW_ALPHA_CLAMP = 1u << 7u;
const uint NEXTGI_SHADOW_MAX_ALPHA_SAMPLES = 4u;

struct NextGIShadowAlpha {
	uvec4 info; // flags, bindless texture slot, attribute stream chunk, attribute stream offset.
	vec2 params; // threshold, albedo alpha.
	uvec2 index; // The geometry's triangles: arena chunk, byte offset.
	vec4 uv_transform; // UV1 scale (xy) and offset (zw).
};

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_INSTANCE_BINDING, std430) restrict readonly buffer NextGIShadowInstances {
	uvec2 data[];
}
nextgi_shadow_instances;

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_ALPHA_BINDING, std430) restrict readonly buffer NextGIShadowAlphaTable {
	NextGIShadowAlpha data[];
}
nextgi_shadow_alpha;

// Set by trace_shadow(): the ray met a candidate left to the retrace and nothing else occluded it.
bool nextgi_shadow_retrace = false;
// Alpha samples the last trace_shadow() took.
uint nextgi_shadow_alpha_samples = 0u;

#ifdef NEXTGI_SHADOW_RETRACE_BINDING
// next_gi_shadow_policy.h, NextGIShadowRetraceRecord: three uvec4 per record. info = (the caller's word, the
// light word, the caller's second word, the result the retrace writes: 1 occluded, 2 clear); then the origin and
// tmax, then the direction (as float bits). header = (records appended, which may pass the capacity; capacity).
layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_RETRACE_BINDING, std430) restrict buffer NextGIShadowRetrace {
	uvec4 header;
	uvec4 data[];
}
nextgi_shadow_retrace_records;

bool nextgi_shadow_retrace_enabled() {
	return nextgi_shadow_retrace_records.header.y != 0u;
}

// Appends a ray the retrace traces through the material any-hit stages. False when the records are full.
bool nextgi_shadow_retrace_append(vec3 p_origin, vec3 p_direction, float p_tmax, uint p_light_mask_mode, uint p_word, uint p_word2) {
	uint index = atomicAdd(nextgi_shadow_retrace_records.header.x, 1u);
	if (index >= nextgi_shadow_retrace_records.header.y) {
		return false;
	}
	nextgi_shadow_retrace_records.data[index * 3u] = uvec4(p_word, p_light_mask_mode, p_word2, 0u);
	nextgi_shadow_retrace_records.data[index * 3u + 1u] = uvec4(floatBitsToUint(p_origin), floatBitsToUint(p_tmax));
	nextgi_shadow_retrace_records.data[index * 3u + 2u] = uvec4(floatBitsToUint(p_direction), 0u);
	return true;
}
#endif

#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
#extension GL_EXT_nonuniform_qualifier : require

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_CHUNK0_BINDING, std430) restrict readonly buffer NextGIShadowChunk0 {
	uint data[];
}
nextgi_shadow_chunk0;

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_CHUNK1_BINDING, std430) restrict readonly buffer NextGIShadowChunk1 {
	uint data[];
}
nextgi_shadow_chunk1;

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_CHUNK2_BINDING, std430) restrict readonly buffer NextGIShadowChunk2 {
	uint data[];
}
nextgi_shadow_chunk2;

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_CHUNK3_BINDING, std430) restrict readonly buffer NextGIShadowChunk3 {
	uint data[];
}
nextgi_shadow_chunk3;

layout(set = NEXTGI_SHADOW_SET, binding = NEXTGI_SHADOW_SAMPLER_BINDING) uniform sampler nextgi_shadow_alpha_sampler;

layout(set = NEXTGI_SHADOW_TEXTURE_SET, binding = 0) uniform texture2D nextgi_shadow_alpha_textures[];

uint nextgi_shadow_chunk_uint(uint p_chunk, uint p_byte) {
	uint word = p_byte >> 2u;
	if (p_chunk == 0u) {
		return nextgi_shadow_chunk0.data[word];
	} else if (p_chunk == 1u) {
		return nextgi_shadow_chunk1.data[word];
	} else if (p_chunk == 2u) {
		return nextgi_shadow_chunk2.data[word];
	}
	return nextgi_shadow_chunk3.data[word];
}

// BaseMaterial3D's alpha at the hit: the albedo texture's alpha at UV1 (scale and offset applied, the texture's
// top mip) times the albedo color's alpha. The UV comes from the surface's attribute stream (#130's layout: a
// 32-byte header with the UV range, then 20 bytes per vertex with the UV at byte 8). The one sampler filters
// linearly and repeats: a nearest filter reads the texel's center, and a material with repeat disabled clamps the
// UV to the edge texels' centers (to the edge texels with a nearest filter), as its own sampler would.
float nextgi_shadow_alpha_sample(NextGIShadowAlpha p_entry, uint p_primitive, vec2 p_bary) {
	uint index_byte = p_entry.index.y + p_primitive * 12u;
	uint i0 = nextgi_shadow_chunk_uint(p_entry.index.x, index_byte);
	uint i1 = nextgi_shadow_chunk_uint(p_entry.index.x, index_byte + 4u);
	uint i2 = nextgi_shadow_chunk_uint(p_entry.index.x, index_byte + 8u);
	uint chunk = p_entry.info.z;
	uint base = p_entry.info.w;
	vec2 uv_min = vec2(uintBitsToFloat(nextgi_shadow_chunk_uint(chunk, base)), uintBitsToFloat(nextgi_shadow_chunk_uint(chunk, base + 4u)));
	vec2 uv_extent = vec2(uintBitsToFloat(nextgi_shadow_chunk_uint(chunk, base + 8u)), uintBitsToFloat(nextgi_shadow_chunk_uint(chunk, base + 12u)));
	vec2 uv0 = uv_min + unpackUnorm2x16(nextgi_shadow_chunk_uint(chunk, base + 32u + i0 * 20u + 8u)) * uv_extent;
	vec2 uv1 = uv_min + unpackUnorm2x16(nextgi_shadow_chunk_uint(chunk, base + 32u + i1 * 20u + 8u)) * uv_extent;
	vec2 uv2 = uv_min + unpackUnorm2x16(nextgi_shadow_chunk_uint(chunk, base + 32u + i2 * 20u + 8u)) * uv_extent;
	vec2 uv = uv0 * (1.0 - p_bary.x - p_bary.y) + uv1 * p_bary.x + uv2 * p_bary.y;
	uv = uv * p_entry.uv_transform.xy + p_entry.uv_transform.zw;
	if ((p_entry.info.x & (NEXTGI_SHADOW_ALPHA_NEAREST | NEXTGI_SHADOW_ALPHA_CLAMP)) != 0u) {
		vec2 size = vec2(max(textureSize(sampler2D(nextgi_shadow_alpha_textures[nonuniformEXT(p_entry.info.y)], nextgi_shadow_alpha_sampler), 0), ivec2(1)));
		bool clamped = (p_entry.info.x & NEXTGI_SHADOW_ALPHA_CLAMP) != 0u;
		if ((p_entry.info.x & NEXTGI_SHADOW_ALPHA_NEAREST) != 0u) {
			vec2 texel = floor(uv * size);
			if (clamped) {
				texel = clamp(texel, vec2(0.0), size - 1.0);
			}
			uv = (texel + 0.5) / size; // The texel's center; the repeating sampler wraps it.
		} else if (clamped) {
			uv = clamp(uv, 0.5 / size, 1.0 - 0.5 / size);
		}
	}
	return textureLod(sampler2D(nextgi_shadow_alpha_textures[nonuniformEXT(p_entry.info.y)], nextgi_shadow_alpha_sampler), uv, 0.0).a * p_entry.params.y;
}
#endif

// Whether the alpha test keeps the candidate's texel. p_entry is the geometry's alpha table entry.
bool nextgi_shadow_alpha_kept(NextGIShadowAlpha p_entry, uint p_primitive, vec2 p_bary) {
#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
	if ((p_entry.info.x & NEXTGI_SHADOW_ALPHA_TEXTURE) != 0u) {
		float alpha = nextgi_shadow_alpha_sample(p_entry, p_primitive, p_bary);
		float threshold = (p_entry.info.x & NEXTGI_SHADOW_ALPHA_HASH) != 0u ? 0.5 : p_entry.params.x;
		// BaseMaterial3D's scissor discards alpha below the threshold.
		return alpha >= threshold;
	}
#endif
	// A material whose alpha is a constant below its threshold casts nothing.
	return (p_entry.info.x & NEXTGI_SHADOW_ALPHA_CUT) == 0u;
}

// 1.0 when nothing between p_origin and p_origin + p_direction * p_tmax shadows a light whose word is
// p_light_mask_mode and whose caster mask is p_layer_mask, 0.0 otherwise. Translucent casters shadow as opaque
// (LQ6), so the result is a visibility, not a transmittance.
float trace_shadow(vec3 p_origin, vec3 p_direction, float p_tmax, uint p_light_mask_mode, uint p_layer_mask) {
	nextgi_shadow_retrace = false;
	nextgi_shadow_alpha_samples = 0u;
	uint cull = p_light_mask_mode & NEXTGI_SHADOW_MODE_CULL_MASK;
	if (cull == 0u || !(p_tmax > 0.0)) {
		return 1.0;
	}
	if ((p_light_mask_mode & NEXTGI_SHADOW_MODE_LOOP) == 0u) {
		// The opaque fast path: every instance with the light's bit is one of its casters.
		rayQueryEXT fast;
		rayQueryInitializeEXT(fast, NEXTGI_SHADOW_TLAS, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, cull, p_origin, 0.0, p_direction, p_tmax);
		rayQueryProceedEXT(fast);
		return rayQueryGetIntersectionTypeEXT(fast, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 0.0 : 1.0;
	}
	// The candidate loop. The fallback makes every hit a candidate and checks its layers; otherwise only the
	// alpha-tested (non-opaque) instances reach it.
	bool fallback = (p_light_mask_mode & NEXTGI_SHADOW_MODE_FALLBACK) != 0u;
	uint flags = gl_RayFlagsTerminateOnFirstHitEXT | (fallback ? gl_RayFlagsNoOpaqueEXT : gl_RayFlagsNoneEXT);
	uint layers = p_layer_mask & NEXTGI_SHADOW_LAYER_MASK;
	uint samples = 0u;
	bool deferred = false;
#ifdef NEXTGI_SHADOW_RETRACE_BINDING
	// The retrace traces with the light's cull mask only: an allocated bit is exactly the light's casters.
	bool defer = !fallback && nextgi_shadow_retrace_enabled();
#else
	bool defer = false;
#endif
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, NEXTGI_SHADOW_TLAS, flags, cull, p_origin, 0.0, p_direction, p_tmax);
	while (rayQueryProceedEXT(rq)) {
		if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) {
			continue;
		}
		uint custom_index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
		uvec2 word = nextgi_shadow_instances.data[custom_index];
		if ((word.x & layers) == 0u) {
			continue; // Not one of this light's casters (only the fallback meets these).
		}
		if ((word.x & NEXTGI_SHADOW_INSTANCE_ALPHA) != 0u) {
			uint geometry = uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
			NextGIShadowAlpha entry = nextgi_shadow_alpha.data[word.y + geometry];
			if ((entry.info.x & NEXTGI_SHADOW_ALPHA_CLASS_MASK) == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
				if (defer && (entry.info.x & NEXTGI_SHADOW_ALPHA_RETRACE) != 0u) {
					deferred = true;
					continue; // The RT-pipeline retrace evaluates the material.
				}
				if (samples < NEXTGI_SHADOW_MAX_ALPHA_SAMPLES) {
					samples++;
					uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
					vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
					if (!nextgi_shadow_alpha_kept(entry, primitive, bary)) {
						continue;
					}
				}
				// Past the sample cap, the remaining candidates shadow as opaque.
			}
		}
		rayQueryConfirmIntersectionEXT(rq);
	}
	nextgi_shadow_alpha_samples = samples;
	bool occluded = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
	nextgi_shadow_retrace = deferred && !occluded;
	return occluded ? 0.0 : 1.0;
}

#endif // NEXTGI_SHADOW_RAY_INC_GLSL
