#[raygen]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_groups (#130). The base library of the NextGI hit pipeline: the check's ray
// generation stage, the miss stage and the fallback hit group (group 0). The check traces each covered texel
// of every captured SRC card from the card plane back into the TLAS, once with the instances' forced
// opacity (closest-hit) and once with gl_RayFlagsNoOpaqueEXT so alpha-tested groups run their any-hit, and
// compares the payloads with the material atlas the raster capture wrote (MODE_RENDER_MATERIAL). A third
// trace leaves the texel the other way, into what stands in front of the card, and counts the hits on
// GPU-written TLAS instances, which have no record and must come back from the fallback group.

#include "nextgi_hit_layout_inc.glsl"

layout(set = 2, binding = 0, std430) restrict readonly buffer HitMaterialParams {
	uint data[];
}
hit_material_params[];

// Keeps the parameter table a run-time array (nextgi_hit_texture_table_probe()).
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].data[0];
}

layout(location = 0) rayPayloadEXT NextGIHitPayload payload;

void count(uint p_counter) {
	atomicAdd(hit_check_counters.data[p_counter], 1u);
}

bool byte_close(uint p_a, uint p_b) {
	return abs(int(p_a) - int(p_b)) <= 1;
}

void main() {
	uint sample_index = gl_LaunchIDEXT.x;
	if (sample_index >= hit_params.sample_count || hit_params.job_count == 0u) {
		return;
	}
	// The last job whose first sample is at or before this one.
	uint lo = 0u;
	uint hi = hit_params.job_count;
	while (hi - lo > 1u) {
		uint mid = (lo + hi) >> 1u;
		if (hit_check_jobs.data[mid].z <= sample_index) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	uvec4 job = hit_check_jobs.data[lo];
	uint width = job.w & 0xFFFFu;
	uint height = job.w >> 16u;
	uint local_index = sample_index - job.z;
	if (width == 0u || local_index >= width * height) {
		return;
	}
	uvec2 texel = uvec2(local_index % width, local_index / width);
	NextGICard card = hit_check_cards.data[job.y];
	ivec2 atlas_texel = ivec2(card.atlas_rect.xy + texel);
	float depth = texelFetch(hit_check_depth, atlas_texel, 0).r;
	if (depth <= 0.0) {
		return; // Not covered by the capture.
	}

	// nextgi_src.h's card frame: the texel's center on the card plane, traced along -axis_n.
	NextGIInstanceRecord rec = hit_records.data[job.x];
	mat4 model = transpose(mat4(
			vec4(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2], rec.object_to_world[3]),
			vec4(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6], rec.object_to_world[7]),
			vec4(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10], rec.object_to_world[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
	float x = ((float(texel.x) + 0.5) / float(width) * 2.0 - 1.0) * card.axis_u.w;
	float y = ((float(texel.y) + 0.5) / float(height) * 2.0 - 1.0) * card.axis_v.w;
	vec3 origin = (model * vec4(card.origin.xyz + x * card.axis_u.xyz + y * card.axis_v.xyz, 1.0)).xyz;
	vec3 direction = mat3(model) * -card.axis_n.xyz;
	float scale = length(direction);
	direction /= scale;
	float t_max = (card.axis_n.w + 2.0 * card.axis_u.w / float(width)) * scale;
	// An orthographic capture: the cone keeps the width of one card texel.
	float cone_width = 2.0 * card.axis_u.w / float(width) * scale;

	vec4 atlas_albedo = texelFetch(hit_check_albedo, atlas_texel, 0);
	vec4 atlas_normal = texelFetch(hit_check_normal, atlas_texel, 0);
	vec4 atlas_orm = texelFetch(hit_check_orm, atlas_texel, 0);
	uvec4 atlas_albedo_bytes = uvec4(round(clamp(atlas_albedo, vec4(0.0), vec4(1.0)) * 255.0));
	count(NEXTGI_HIT_CHECK_SAMPLES_COUNTER);

	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(cone_width), floatBitsToUint(0.0), 0u, 0u);
	traceRayEXT(hit_tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, origin, 0.0, direction, t_max, 0);
	uint flags = payload.shading.z >> 24u;
	bool any_hit_group = false;
	if ((flags & NEXTGI_HIT_PAYLOAD_HIT) != 0u) {
		count(NEXTGI_HIT_CHECK_HITS_COUNTER);
		uint material_id = payload.hit.y;
		if (material_id < hit_params.material_count) {
			atomicAdd(hit_check_counters.data[NEXTGI_HIT_CHECK_MATERIALS_BASE + material_id], 1u);
		}
		bool fallback = (flags & NEXTGI_HIT_PAYLOAD_FALLBACK) != 0u;
		any_hit_group = (flags & NEXTGI_HIT_PAYLOAD_ANY_HIT_GROUP) != 0u;
		if (fallback) {
			// The fallback group writes constants: only material groups are compared.
			count(NEXTGI_HIT_CHECK_FALLBACK_COUNTER);
		} else {
			uvec4 albedo_bytes = (uvec4(payload.shading.x) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu;
			bool albedo_ok = byte_close(albedo_bytes.r, atlas_albedo_bytes.r) && byte_close(albedo_bytes.g, atlas_albedo_bytes.g) && byte_close(albedo_bytes.b, atlas_albedo_bytes.b);
			bool opacity_ok = byte_close(albedo_bytes.a, atlas_albedo_bytes.a);

			// The capture-view normal: the object-space normal in the card frame (nextgi_src.h).
			vec3 world_normal = nextgi_hit_oct_decode(unpackSnorm2x16(payload.shading.y));
			vec3 object_normal = normalize(transpose(mat3(model)) * world_normal);
			vec3 view_normal = vec3(dot(object_normal, card.axis_u.xyz), dot(object_normal, card.axis_v.xyz), dot(object_normal, card.axis_n.xyz));
			vec3 atlas_view_normal = normalize(atlas_normal.rgb * 2.0 - 1.0);
			// 1/255 per component of the G-buffer's [0, 1] mapping (n * 0.5 + 0.5), which is 2/255 in [-1, 1].
			bool normal_ok = all(lessThanEqual(abs(view_normal - atlas_view_normal), vec3(2.0 / 255.0)));

			uint roughness = payload.shading.z & 0xFFu;
			uint metallic = (payload.shading.z >> 8u) & 0xFFu;
			bool roughness_ok = byte_close(roughness, uint(round(clamp(atlas_orm.g, 0.0, 1.0) * 255.0)));
			bool metallic_ok = byte_close(metallic, uint(round(clamp(atlas_orm.b, 0.0, 1.0) * 255.0)));

			// The record of the group the table chose must name the shader of the geometry's material.
			bool record_ok = material_id < hit_params.material_count && payload.hit.z == hit_materials.data[material_id].shader_slot;

			if (albedo_ok) {
				count(NEXTGI_HIT_CHECK_ALBEDO_COUNTER);
			}
			if (normal_ok) {
				count(NEXTGI_HIT_CHECK_NORMAL_COUNTER);
			}
			if (roughness_ok) {
				count(NEXTGI_HIT_CHECK_ROUGHNESS_COUNTER);
			}
			if (metallic_ok) {
				count(NEXTGI_HIT_CHECK_METALLIC_COUNTER);
			}
			if (opacity_ok) {
				count(NEXTGI_HIT_CHECK_OPACITY_COUNTER);
			}
			if (record_ok) {
				count(NEXTGI_HIT_CHECK_RECORD_COUNTER);
			}
			if (albedo_ok && normal_ok && roughness_ok && metallic_ok && opacity_ok && record_ok) {
				count(NEXTGI_HIT_CHECK_ALL_COUNTER);
			}
		}
	}

	// Again without the forced opacity: an alpha-tested group's any-hit ignores what the scissor cuts, so the
	// ray hits exactly where the capture kept the texel.
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(cone_width), floatBitsToUint(0.0), 0u, 0u);
	traceRayEXT(hit_tlas, gl_RayFlagsNoOpaqueEXT, 0xFF, 0, 1, 0, origin, 0.0, direction, t_max, 0);
	bool hit_again = ((payload.shading.z >> 24u) & NEXTGI_HIT_PAYLOAD_HIT) != 0u;
	count(NEXTGI_HIT_CHECK_ANY_HIT_SAMPLES_COUNTER);
	if (hit_again == (atlas_albedo_bytes.a >= 128u)) {
		count(NEXTGI_HIT_CHECK_ANY_HIT_AGREE_COUNTER);
	}
	if (any_hit_group) {
		count(NEXTGI_HIT_CHECK_ALPHA_TESTED_COUNTER);
		if (atlas_albedo_bytes.a < 128u) {
			count(NEXTGI_HIT_CHECK_ALPHA_CUT_COUNTER);
		}
	}

	// The probe: up to 4 units out of the card's face.
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(cone_width), floatBitsToUint(0.0), 0u, 0u);
	traceRayEXT(hit_tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, origin, 0.01, -direction, 4.0, 0);
	uint probe_flags = payload.shading.z >> 24u;
	if ((probe_flags & NEXTGI_HIT_PAYLOAD_HIT) != 0u) {
		count(NEXTGI_HIT_CHECK_PROBE_HITS_COUNTER);
		if (payload.hit.w >= hit_params.record_count) {
			count(NEXTGI_HIT_CHECK_PROBE_GPU_INSTANCES_COUNTER);
			if ((probe_flags & NEXTGI_HIT_PAYLOAD_FALLBACK) != 0u) {
				count(NEXTGI_HIT_CHECK_PROBE_GPU_FALLBACK_COUNTER);
			}
		}
	}
}

#[miss]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_groups (#130). A miss writes no hit flag.

#include "nextgi_hit_layout_inc.glsl"

layout(set = 2, binding = 0, std430) restrict readonly buffer HitMaterialParams {
	uint data[];
}
hit_material_params[];

// Keeps the parameter table a run-time array (nextgi_hit_texture_table_probe()).
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].data[0];
}

layout(location = 0) rayPayloadInEXT NextGIHitPayload payload;

void main() {
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(-1.0), 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

#[closest_hit]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_groups (#130). The fallback group (group 0): the hits of geometries whose material has
// no hit group of its own yet (compiling), is ineligible, or that have no attribute stream, and of GPU-written
// TLAS instances (no record). It writes surface-cache-like constants with the geometric normal facing the ray
// and the fallback flag, so the caller shades the hit from the SRC as the inline path does.

#include "nextgi_hit_layout_inc.glsl"

layout(set = 2, binding = 0, std430) restrict readonly buffer HitMaterialParams {
	uint data[];
}
hit_material_params[];

// Keeps the parameter table a run-time array (nextgi_hit_texture_table_probe()).
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].data[0];
}

layout(shaderRecordEXT, std430) buffer NextGIHitRecord {
	uint shader_slot;
	uint flags;
	uint pad0;
	uint pad1;
}
hit_record;

hitAttributeEXT vec2 hit_bary;

layout(location = 0) rayPayloadInEXT NextGIHitPayload payload;

void main() {
	NextGIInstanceRecord rec;
	if (!nextgi_hit_fetch_record(uint(gl_InstanceCustomIndexEXT), uint(gl_GeometryIndexEXT), rec)) {
		payload = nextgi_hit_unrecorded_payload(gl_WorldRayDirectionEXT, gl_HitTEXT, hit_record.shader_slot, uint(gl_InstanceCustomIndexEXT));
		return;
	}
	uint surface_index = rec.surface_base + gl_GeometryIndexEXT;
	NextGISurfaceRecord surf = hit_surface_records.data[surface_index];
	uint index_byte = surf.index_offset + uint(gl_PrimitiveID) * 12u;
	uint i0 = nextgi_hit_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_hit_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_hit_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_hit_chunk_vec3(rec.arena_chunk, surf.vertex_offset + i0 * 12u);
	vec3 p1 = nextgi_hit_chunk_vec3(rec.arena_chunk, surf.vertex_offset + i1 * 12u);
	vec3 p2 = nextgi_hit_chunk_vec3(rec.arena_chunk, surf.vertex_offset + i2 * 12u);
	// The front face as gl_FrontFacing decides it: the winding of the world-space corners (clockwise as seen
	// from the viewer, so (p2 - p0) x (p1 - p0) points out of it; a mirrored transform reverses both).
	mat3 object_to_world = mat3(gl_ObjectToWorldEXT);
	vec3 normal = normalize(cross(object_to_world * (p2 - p0), object_to_world * (p1 - p0)));
	bool front_facing = dot(normal, gl_WorldRayDirectionEXT) <= 0.0;
	if (!front_facing) {
		normal = -normal;
	}
	uint flags = NEXTGI_HIT_PAYLOAD_HIT | NEXTGI_HIT_PAYLOAD_FALLBACK | (front_facing ? NEXTGI_HIT_PAYLOAD_FRONT_FACE : 0u);
	payload.shading = uvec4(packUnorm4x8(vec4(0.5, 0.5, 0.5, 1.0)), packSnorm2x16(nextgi_hit_oct_encode(normal)), 255u | (255u << 16u) | (flags << 24u), 0u);
	payload.hit = uvec4(floatBitsToUint(gl_HitTEXT), hit_surfaces.data[surface_index].material_id, hit_record.shader_slot, uint(gl_InstanceCustomIndexEXT));
}
