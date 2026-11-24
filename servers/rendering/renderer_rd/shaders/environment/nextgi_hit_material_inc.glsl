// GodotNext: nextgi_hit_groups (#130). Not a standalone shader: the body of the closest-hit
// (NEXTGI_HIT_CLOSEST) and any-hit (NEXTGI_HIT_ANY) stages of a Godot spatial material, filled with the
// material's ShaderCompiler output (nextgi_hit mode). It runs the material's vertex() at the triangle's
// three corners and interpolates the built-in outputs with the hit's barycentrics, as the rasterizer does
// (custom varyings come from one more run at the hit point), then runs fragment() in a view space that is
// world space moved to the ray origin, so VIEW is the reverse ray direction and NORMAL is a world normal.
// Implicit-LOD texture() calls sample at the ray cone's LOD (Akenine-Moller et al. 2021). light() is
// never run at a hit.

#include "nextgi_hit_layout_inc.glsl"

#ifndef NORMAL_USED
#define NORMAL_USED
#endif
#if !defined(TANGENT_USED) && (defined(NORMAL_MAP_USED) || defined(BENT_NORMAL_MAP_USED) || defined(LIGHT_ANISOTROPY_USED))
#define TANGENT_USED
#endif

// The material's uniform buffer bytes (std140), then a member past them that only the probe below names.
layout(set = 2, binding = 0, std140) restrict readonly buffer HitMaterialParams {
#MATERIAL_UNIFORMS
	uint nextgi_hit_params_tail;
}
hit_material_params[];

// Keeps the parameter table a run-time array (nextgi_hit_texture_table_probe()). Never called.
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].nextgi_hit_params_tail;
}

// Not readonly: glslang then decorates the record NonWritable, which spirv-val rejects for the shader
// record storage class.
layout(shaderRecordEXT, std430) buffer NextGIHitRecord {
	uint shader_slot;
	uint flags; // Bit 0: the group has an any-hit stage.
	uint pad0;
	uint pad1;
}
hit_record;

hitAttributeEXT vec2 hit_bary;

layout(location = 0) rayPayloadInEXT NextGIHitPayload payload;

// The material's parameter buffer and texture slots, set from the hit's material before any material code.
uint hit_params_slot;
uint hit_texture_base;
// log2 of the cone footprint over the triangle's UV density; nextgi_hit_texture_lod() adds the texture size.
// While vertex() runs it is far below 0, so its texture() reads sample level 0 as the rasterizer's vertex
// stage does; the cone's level is set only for fragment().
float hit_lod_base;
#define NEXTGI_HIT_VERTEX_LOD_BASE -1.0e4

#define material hit_material_params[nonuniformEXT(hit_params_slot)]
#define SHADER_IS_SRGB false
#define SHADER_SPACE_FAR 0.0
#define OUTPUT_IS_MULTIVIEW false
#define ViewIndex 0

// The LOD of the ray cone's footprint on a texture (ShaderCompiler's nextgi_hit mode passes the texture
// of each texture() call).
float nextgi_hit_texture_lod_for_size(vec2 p_size) {
	return hit_lod_base + 0.5 * log2(max(p_size.x * p_size.y, 1.0));
}

float nextgi_hit_texture_lod(texture2D t) {
	return nextgi_hit_texture_lod_for_size(vec2(textureSize(t, 0)));
}

float nextgi_hit_texture_lod(itexture2D t) {
	return nextgi_hit_texture_lod_for_size(vec2(textureSize(t, 0)));
}

float nextgi_hit_texture_lod(utexture2D t) {
	return nextgi_hit_texture_lod_for_size(vec2(textureSize(t, 0)));
}

float nextgi_hit_texture_lod(texture2DArray t) {
	return nextgi_hit_texture_lod_for_size(vec2(textureSize(t, 0).xy));
}

float nextgi_hit_texture_lod(texture3D t) {
	return nextgi_hit_texture_lod_for_size(vec2(textureSize(t, 0).xy));
}

float nextgi_hit_texture_lod(textureCube t) {
	return nextgi_hit_texture_lod_for_size(vec2(textureSize(t, 0)));
}

// The scene_forward_clustered names the material code reads and writes.
vec3 vertex;
vec3 normal_highp;
vec3 tangent;
vec3 binormal;
vec2 uv_interp;
vec2 uv2_interp;
vec4 color_interp;
vec4 position;
float point_size;
float z_clip_scale;
vec4 instance_custom;
float roughness_highp;
float global_time;
mat4 read_model_matrix;
mat3 model_normal_matrix;
mat4 read_view_matrix;
mat4 inv_view_matrix;
mat4 projection_matrix;
mat4 inv_projection_matrix;
mat4 modelview;
mat3 modelview_normal;
vec3 eye_offset;
vec2 read_viewport_size;

vec3 view_highp;
vec3 albedo_highp;
float alpha_highp;
float metallic_highp;
float specular;
vec3 emission;
float rim;
float rim_tint;
float clearcoat;
float clearcoat_roughness;
float anisotropy;
vec2 anisotropy_flow;
float sss_strength;
vec4 transmittance_color;
float transmittance_depth;
float transmittance_boost;
vec3 backlight;
float ao;
float ao_light_affect;
vec3 normal_map;
float normal_map_depth;
vec3 bent_normal_map;
float alpha_scissor_threshold;
float alpha_hash_scale;
float alpha_antialiasing_edge;
vec2 alpha_texture_coordinate;
float premul_alpha;
vec4 fog;
vec4 custom_radiance;
vec4 custom_irradiance;
vec3 light_vertex;
float streaming_lod;
float depth_offset;
vec3 energy_compensation;

#ifdef DISCARD_USED
// GodotNext: nextgi_hit_discard (#304). discard in fragment(), which ShaderCompiler's hit mode writes as
// NEXTGI_HIT_DISCARD: fragment() ends there and the hit is marked discarded. The any-hit stage then ignores the
// intersection, as the rasterizer drops the fragment. The closest-hit stage (a forced-opaque trace) reports the hit
// discarded and with the fallback flag (NEXTGI_HIT_PAYLOAD_DISCARDED): fragment() wrote only what came before the
// discard. A discard in a helper function stays discard, and nextgi_hit_classify() refuses the shader.
bool nextgi_hit_discarded;
#define NEXTGI_HIT_DISCARD \
	do { \
		nextgi_hit_discarded = true; \
		return; \
	} while (false)
#endif

#GLOBALS

void nextgi_hit_vertex_code() {
#CODE : VERTEX
}

void nextgi_hit_fragment_code() {
#CODE : FRAGMENT
}

struct NextGIHitVertex {
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	vec2 uv;
	vec2 uv2;
	vec4 color;
};

// The stream's header: each UV set's minimum and extent (NextGIHitStreamHeader).
struct NextGIHitUVRanges {
	vec2 uv_min;
	vec2 uv_extent;
	vec2 uv2_min;
	vec2 uv2_extent;
};

NextGIHitUVRanges nextgi_hit_fetch_uv_ranges(NextGIHitSurface p_surface) {
	uint base = p_surface.attribute_offset;
	uint chunk = p_surface.attribute_chunk;
	NextGIHitUVRanges r;
	r.uv_min = vec2(uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base)), uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 4u)));
	r.uv_extent = vec2(uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 8u)), uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 12u)));
	r.uv2_min = vec2(uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 16u)), uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 20u)));
	r.uv2_extent = vec2(uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 24u)), uintBitsToFloat(nextgi_hit_chunk_uint(chunk, base + 28u)));
	return r;
}

// One vertex of the attribute stream, after the 32-byte header: oct normal, oct tangent with the binormal
// sign in y (Godot's uncompressed encoding), UV and UV2 (unorm16 over the header's ranges) and color (RGBA8).
NextGIHitVertex nextgi_hit_fetch_vertex(NextGIHitSurface p_surface, NextGIHitUVRanges p_ranges, uint p_index) {
	uint base = p_surface.attribute_offset + 32u + p_index * 20u;
	uint chunk = p_surface.attribute_chunk;
	NextGIHitVertex v;
	v.normal = nextgi_hit_oct_decode(unpackSnorm2x16(nextgi_hit_chunk_uint(chunk, base)));
	vec2 t = unpackSnorm2x16(nextgi_hit_chunk_uint(chunk, base + 4u));
	v.tangent = nextgi_hit_oct_decode(vec2(t.x, abs(t.y) * 2.0 - 1.0));
	float binormal_sign = t.y < 0.0 ? -1.0 : 1.0;
	v.binormal = normalize(cross(v.normal, v.tangent) * binormal_sign);
	v.uv = p_ranges.uv_min + unpackUnorm2x16(nextgi_hit_chunk_uint(chunk, base + 8u)) * p_ranges.uv_extent;
	v.uv2 = p_ranges.uv2_min + unpackUnorm2x16(nextgi_hit_chunk_uint(chunk, base + 12u)) * p_ranges.uv2_extent;
	v.color = unpackUnorm4x8(nextgi_hit_chunk_uint(chunk, base + 16u));
	return v;
}

struct NextGIHitCorner {
	vec3 vertex;
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	vec2 uv;
	vec2 uv2;
	vec4 color;
};

// vertex() for one point, with the raster path's transforms around it.
NextGIHitCorner nextgi_hit_run_vertex(vec3 p_position, NextGIHitVertex p_vertex) {
	vertex = p_position;
	normal_highp = p_vertex.normal;
	tangent = p_vertex.tangent;
	binormal = p_vertex.binormal;
	uv_interp = p_vertex.uv;
	uv2_interp = p_vertex.uv2;
	color_interp = p_vertex.color;
	position = vec4(1.0);
	point_size = 1.0;
	z_clip_scale = 1.0;
	instance_custom = vec4(0.0);
	roughness_highp = 1.0;

#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	vertex = (read_model_matrix * vec4(vertex, 1.0)).xyz;
	normal_highp = model_normal_matrix * normal_highp;
	tangent = mat3(read_model_matrix) * tangent;
	binormal = mat3(read_model_matrix) * binormal;
#endif

	nextgi_hit_vertex_code();

	vec3 normal = normal_highp;
#if !defined(SKIP_TRANSFORM_USED) && !defined(VERTEX_WORLD_COORDS_USED)
	vertex = (modelview * vec4(vertex, 1.0)).xyz;
	normal = modelview_normal * normal;
	tangent = mat3(modelview) * tangent;
	binormal = mat3(modelview) * binormal;
#endif
#if !defined(SKIP_TRANSFORM_USED) && defined(VERTEX_WORLD_COORDS_USED)
	vertex = (read_view_matrix * vec4(vertex, 1.0)).xyz;
	normal = (read_view_matrix * vec4(normal, 0.0)).xyz;
	binormal = (read_view_matrix * vec4(binormal, 0.0)).xyz;
	tangent = (read_view_matrix * vec4(tangent, 0.0)).xyz;
#endif

	NextGIHitCorner c;
	c.vertex = vertex;
	c.normal = normalize(normal);
	c.tangent = normalize(tangent);
	c.binormal = normalize(binormal);
	c.uv = uv_interp;
	c.uv2 = uv2_interp;
	c.color = color_interp;
	return c;
}

#ifdef ALPHA_HASH_USED
float nextgi_hit_hash_2d(vec2 p) {
	return fract(1.0e4 * sin(17.0 * p.x + 0.1 * p.y) * (0.1 + abs(sin(13.0 * p.y + p.x))));
}

float nextgi_hit_hash_3d(vec3 p) {
	return nextgi_hit_hash_2d(vec2(nextgi_hit_hash_2d(p.xy), p.z));
}

// scene_forward_aa_inc.glsl's alpha hash threshold, with the ray cone's footprint (object units) in place of
// the screen derivatives a hit does not have.
float nextgi_hit_alpha_hash_threshold(vec3 p_pos, float p_hash_scale, float p_footprint) {
	float pix_scale = 1.0 / (p_hash_scale * max(p_footprint, 1e-6));
	vec2 pix_scales = vec2(exp2(floor(log2(pix_scale))), exp2(ceil(log2(pix_scale))));
	vec2 a_thresh = vec2(nextgi_hit_hash_3d(floor(pix_scales.x * p_pos)), nextgi_hit_hash_3d(floor(pix_scales.y * p_pos)));
	float lerp_factor = fract(log2(pix_scale));
	float a_interp = (1.0 - lerp_factor) * a_thresh.x + lerp_factor * a_thresh.y;
	float min_lerp = min(lerp_factor, 1.0 - lerp_factor);
	vec3 cases = vec3(a_interp * a_interp / (2.0 * min_lerp * (1.0 - min_lerp)),
			(a_interp - 0.5 * min_lerp) / (1.0 - min_lerp),
			1.0 - ((1.0 - a_interp) * (1.0 - a_interp) / (2.0 * min_lerp * (1.0 - min_lerp))));
	float threshold = (a_interp < (1.0 - min_lerp)) ? ((a_interp < min_lerp) ? cases.x : cases.y) : cases.z;
	return clamp(threshold, 0.00001, 1.0);
}
#endif // ALPHA_HASH_USED

void main() {
	// Only CPU-fed instances reach a material group, and a hit without records never touches hit_surfaces.
	NextGIInstanceRecord rec;
	if (!nextgi_hit_fetch_record(uint(gl_InstanceCustomIndexEXT), uint(gl_GeometryIndexEXT), rec)) {
#ifdef NEXTGI_HIT_CLOSEST
		payload = nextgi_hit_unrecorded_payload(gl_WorldRayDirectionEXT, gl_HitTEXT, hit_record.shader_slot, uint(gl_InstanceCustomIndexEXT));
#endif
		return;
	}
	uint surface_index = rec.surface_base + gl_GeometryIndexEXT;
	NextGIHitSurface hit_surface = hit_surfaces.data[surface_index];
	if ((hit_surface.flags & 1u) == 0u || hit_surface.material_id == 0xFFFFFFFFu || hit_surface.attribute_chunk >= NEXTGI_HIT_ARENA_CHUNKS) {
#ifdef NEXTGI_HIT_CLOSEST
		payload = nextgi_hit_unrecorded_payload(gl_WorldRayDirectionEXT, gl_HitTEXT, hit_record.shader_slot, uint(gl_InstanceCustomIndexEXT));
#endif
		return;
	}
	NextGISurfaceRecord surf = hit_surface_records.data[surface_index];
	uint material_index = hit_surface.material_id;
	NextGIHitMaterial hit_material = hit_materials.data[material_index];
	hit_params_slot = hit_material.params_slot;
	hit_texture_base = hit_material.texture_base;
	global_time = hit_params.time;

	uint index_byte = surf.index_offset + uint(gl_PrimitiveID) * 12u;
	uvec3 idx = uvec3(nextgi_hit_chunk_uint(rec.arena_chunk, index_byte), nextgi_hit_chunk_uint(rec.arena_chunk, index_byte + 4u), nextgi_hit_chunk_uint(rec.arena_chunk, index_byte + 8u));
	vec3 p0 = nextgi_hit_chunk_vec3(rec.arena_chunk, surf.vertex_offset + idx.x * 12u);
	vec3 p1 = nextgi_hit_chunk_vec3(rec.arena_chunk, surf.vertex_offset + idx.y * 12u);
	vec3 p2 = nextgi_hit_chunk_vec3(rec.arena_chunk, surf.vertex_offset + idx.z * 12u);
	NextGIHitUVRanges uv_ranges = nextgi_hit_fetch_uv_ranges(hit_surface);
	NextGIHitVertex a0 = nextgi_hit_fetch_vertex(hit_surface, uv_ranges, idx.x);
	NextGIHitVertex a1 = nextgi_hit_fetch_vertex(hit_surface, uv_ranges, idx.y);
	NextGIHitVertex a2 = nextgi_hit_fetch_vertex(hit_surface, uv_ranges, idx.z);
	vec3 b = vec3(1.0 - hit_bary.x - hit_bary.y, hit_bary.x, hit_bary.y);

	read_model_matrix = transpose(mat4(
			vec4(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2], rec.object_to_world[3]),
			vec4(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6], rec.object_to_world[7]),
			vec4(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10], rec.object_to_world[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
	model_normal_matrix = transpose(inverse(mat3(read_model_matrix)));
	vec3 ray_origin = gl_WorldRayOriginEXT;
	vec3 ray_dir = gl_WorldRayDirectionEXT;
	read_view_matrix = mat4(1.0);
	read_view_matrix[3] = vec4(-ray_origin, 1.0);
	inv_view_matrix = mat4(1.0);
	inv_view_matrix[3] = vec4(ray_origin, 1.0);
	projection_matrix = mat4(1.0);
	inv_projection_matrix = mat4(1.0);
	modelview = read_view_matrix * read_model_matrix;
	modelview_normal = mat3(read_view_matrix) * model_normal_matrix;
	eye_offset = vec3(0.0);
	read_viewport_size = vec2(1.0);
	hit_lod_base = NEXTGI_HIT_VERTEX_LOD_BASE;

	// vertex() at the corners for the interpolated built-ins, then at the hit point for custom varyings.
	NextGIHitCorner c0 = nextgi_hit_run_vertex(p0, a0);
	NextGIHitCorner c1 = nextgi_hit_run_vertex(p1, a1);
	NextGIHitCorner c2 = nextgi_hit_run_vertex(p2, a2);
	NextGIHitVertex ah;
	ah.normal = normalize(a0.normal * b.x + a1.normal * b.y + a2.normal * b.z);
	ah.tangent = normalize(a0.tangent * b.x + a1.tangent * b.y + a2.tangent * b.z);
	ah.binormal = normalize(a0.binormal * b.x + a1.binormal * b.y + a2.binormal * b.z);
	ah.uv = a0.uv * b.x + a1.uv * b.y + a2.uv * b.z;
	ah.uv2 = a0.uv2 * b.x + a1.uv2 * b.y + a2.uv2 * b.z;
	ah.color = a0.color * b.x + a1.color * b.y + a2.color * b.z;
	nextgi_hit_run_vertex(p0 * b.x + p1 * b.y + p2 * b.z, ah);

	vec3 vertex_interp = c0.vertex * b.x + c1.vertex * b.y + c2.vertex * b.z;
	vec3 normal_interp = c0.normal * b.x + c1.normal * b.y + c2.normal * b.z;

	// The ray cone at the hit (the caller's width and spread in hit.xy), over the triangle's UV density.
	float cone_width = uintBitsToFloat(payload.hit.x) + gl_HitTEXT * uintBitsToFloat(payload.hit.y);
	vec3 edge_normal = cross(c1.vertex - c0.vertex, c2.vertex - c0.vertex);
	float world_area = length(edge_normal);
	vec2 duv1 = c1.uv - c0.uv;
	vec2 duv2 = c2.uv - c0.uv;
	float uv_area = abs(duv1.x * duv2.y - duv1.y * duv2.x);
	float cos_theta = max(abs(dot(edge_normal / max(world_area, 1e-20), ray_dir)), 1e-3);
	hit_lod_base = 0.5 * log2(max(uv_area, 1e-20) / max(world_area, 1e-20)) + log2(max(cone_width, 1e-20) / cos_theta);

	vertex = vertex_interp;
	view_highp = -normalize(vertex_interp);
	uv_interp = c0.uv * b.x + c1.uv * b.y + c2.uv * b.z;
	uv2_interp = c0.uv2 * b.x + c1.uv2 * b.y + c2.uv2 * b.z;
	color_interp = c0.color * b.x + c1.color * b.y + c2.color * b.z;
	tangent = c0.tangent * b.x + c1.tangent * b.y + c2.tangent * b.z;
	binormal = c0.binormal * b.x + c1.binormal * b.y + c2.binormal * b.z;
	normal_highp = normal_interp;
	// gl_FrontFacing: the winding of the corners vertex() placed (world-oriented view space), as the rasterizer
	// sees it. Godot's front faces wind clockwise as seen from the viewer, so (c2 - c0) x (c1 - c0) points out
	// of the front face; a mirrored transform reverses it, as it reverses the rasterized winding.
	bool front_facing = dot(cross(c2.vertex - c0.vertex, c1.vertex - c0.vertex), ray_dir) <= 0.0;
	if ((hit_params.flags & NEXTGI_HIT_PARAMS_CAPTURE_FACING) != 0u) {
		front_facing = !front_facing;
	}
#ifdef DO_SIDE_CHECK
	if (!front_facing) {
		normal_highp = -normal_highp;
	}
#endif

	albedo_highp = vec3(1.0);
	alpha_highp = 1.0;
	metallic_highp = 0.0;
	specular = 0.5;
	emission = vec3(0.0);
	roughness_highp = 1.0;
	rim = 0.0;
	rim_tint = 0.0;
	clearcoat = 0.0;
	clearcoat_roughness = 0.0;
	anisotropy = 0.0;
	anisotropy_flow = vec2(1.0, 0.0);
	sss_strength = 0.0;
	transmittance_color = vec4(0.0, 0.0, 0.0, 1.0);
	transmittance_depth = 0.0;
	transmittance_boost = 0.0;
	depth_offset = 0.0;
	backlight = vec3(0.0);
	ao = 1.0;
	ao_light_affect = 0.0;
	normal_map = vec3(0.5);
	normal_map_depth = 1.0;
	bent_normal_map = vec3(0.5);
	alpha_scissor_threshold = 1.0;
	alpha_hash_scale = 1.0;
	alpha_antialiasing_edge = 0.0;
	alpha_texture_coordinate = vec2(0.0);
	premul_alpha = 1.0;
	fog = vec4(0.0, 0.0, 0.0, 1.0);
	custom_radiance = vec4(0.0);
	custom_irradiance = vec4(0.0);
	light_vertex = vertex;
	streaming_lod = 0.0;
	energy_compensation = vec3(1.0);
#ifdef DISCARD_USED
	nextgi_hit_discarded = false;
#endif

	nextgi_hit_fragment_code();

	float alpha = alpha_highp;
#ifdef ALPHA_SCISSOR_USED
	alpha = alpha < alpha_scissor_threshold ? 0.0 : 1.0;
#endif
#ifdef ALPHA_HASH_USED
	vec3 object_pos = p0 * b.x + p1 * b.y + p2 * b.z;
	float object_scale = max(length(read_model_matrix[0].xyz), 1e-6);
	alpha = alpha < nextgi_hit_alpha_hash_threshold(object_pos, alpha_hash_scale, cone_width / object_scale) ? 0.0 : 1.0;
#endif

#ifdef NEXTGI_HIT_ANY
#ifdef DISCARD_USED
	// GodotNext: nextgi_hit_discard (#304)
	if (nextgi_hit_discarded) {
		ignoreIntersectionEXT;
	}
#endif
	// GodotNext: nextgi_hit_discard (#304). Only an alpha test cuts by alpha: a group that has the any-hit stage
	// for discard alone keeps the hits its alpha would blend.
#if defined(ALPHA_SCISSOR_USED) || defined(ALPHA_HASH_USED)
	if (alpha < 0.5) {
		ignoreIntersectionEXT;
	}
#endif
#else
#ifdef DISCARD_USED
	// GodotNext: nextgi_hit_discard (#304). A forced-opaque trace (GI) runs no any-hit stage, so it meets the texels
	// fragment() discards. fragment() stopped at the discard, so the material outputs below it were never written:
	// the hit is cut (opacity 0) and carries the fallback flag, so the caller shades it as a fallback-group hit (a
	// refused shader's hit, as before #304) instead of with the template's defaults.
	if (nextgi_hit_discarded) {
		alpha = 0.0;
	}
#endif
	vec3 normal = normal_highp;
#if defined(NORMAL_MAP_USED)
	normal_map.xy = normal_map.xy * 2.0 - 1.0;
	normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy)));
	normal = normalize(mix(normal, tangent * normal_map.x + binormal * normal_map.y + normal * normal_map.z, normal_map_depth));
#else
	normal = normalize(normal);
#endif

	uint flags = NEXTGI_HIT_PAYLOAD_HIT | (front_facing ? NEXTGI_HIT_PAYLOAD_FRONT_FACE : 0u) | ((hit_record.flags & 1u) != 0u ? NEXTGI_HIT_PAYLOAD_ANY_HIT_GROUP : 0u);
#ifdef DISCARD_USED
	if (nextgi_hit_discarded) {
		flags |= NEXTGI_HIT_PAYLOAD_FALLBACK | NEXTGI_HIT_PAYLOAD_DISCARDED; // GodotNext: nextgi_hit_discard (#304)
	}
#endif
	payload.shading.x = packUnorm4x8(vec4(clamp(albedo_highp, vec3(0.0), vec3(1.0)), clamp(alpha, 0.0, 1.0)));
	payload.shading.y = packSnorm2x16(nextgi_hit_oct_encode(normal));
	payload.shading.z = uint(round(clamp(roughness_highp, 0.0, 1.0) * 255.0)) | (uint(round(clamp(metallic_highp, 0.0, 1.0) * 255.0)) << 8u) | (uint(round(clamp(ao, 0.0, 1.0) * 255.0)) << 16u) | (flags << 24u);
	payload.shading.w = nextgi_hit_pack_r11g11b10(emission);
	payload.hit = uvec4(floatBitsToUint(gl_HitTEXT), material_index, hit_record.shader_slot, uint(gl_InstanceCustomIndexEXT));
#endif
}
