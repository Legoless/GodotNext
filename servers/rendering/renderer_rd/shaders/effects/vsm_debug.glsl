#[vertex]

#version 450

#VERSION_DEFINES

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

// GodotNext: vsm_settings_debug (#221, plan §21.3.2). The six virtual shadow map debug draws
// (Viewport.DEBUG_DRAW_VSM_*), drawn over the final image. Each pixel's world position comes from the
// view's depth; it looks up the pages the projection would read, as the mark pass chose them
// (vsm_mark_pages.glsl) and the CPU model's lookup() and clipmap_sample() fall back.
// DEBUG_MASK: the raw visibility, one depth comparison per light with maps (not #222's filtered shadow).
// DEBUG_MIP: the mip of the first light's page. DEBUG_PAGES: a color per virtual page address.
// DEBUG_CACHE / DEBUG_OVERDRAW: what the view rendered into the first light's page (vsm_debug_pages.glsl).
// DEBUG_CLIPMAP: the clipmap level of the first directional light's page.

#define DEBUG_MASK 0u
#define DEBUG_MIP 1u
#define DEBUG_PAGES 2u
#define DEBUG_CACHE 3u
#define DEBUG_OVERDRAW 4u
#define DEBUG_CLIPMAP 5u

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;

layout(set = 0, binding = 1, std430) restrict readonly buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;

#include "../vsm_mark_inc.glsl"
#include "../vsm_page_table_inc.glsl"
#include "../vsm_raster_inc.glsl"

layout(set = 0, binding = 2, std430) restrict readonly buffer VSMMaps {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 3, r32ui) uniform restrict readonly uimage2DArray vsm_pool;

struct DebugLight {
	uvec4 info; // Map base, light flags, map kind, 1 / reach (float bits).
	vec4 position; // World position; w: the spot cone's cosine.
	vec4 direction; // World direction the light shines along.
};

layout(set = 0, binding = 4, std430) restrict readonly buffer DebugParams {
	vec4 inv_projection[4]; // Rows: depth-texture clip space to view space.
	vec4 view_to_world[3]; // Rows of the camera transform.
	uvec4 sizes; // xy: the depth buffer's size, zw: the output's size.
	vec4 footprint; // x, y: a pixel's footprint in meters is view depth * x + y.
	ivec4 bias; // x: directional LOD bias, y: local-light LOD bias.
	uvec4 misc; // x: the page table's map count, y: lights, z: the draw, w: the pool grid's width.
	uvec4 misc2; // x: pool pages, y: 1 when page_info holds this view's pages.
	DebugLight lights[];
}
params;

layout(set = 0, binding = 5, std430) restrict readonly buffer PageInfo {
	uint data[];
}
page_info;

layout(location = 0) out vec4 frag_color;

struct DebugSample {
	uint map;
	uint mip;
	uint level;
	uvec2 phys_texel;
	uvec2 page; // The virtual page slot at the sample's mip.
	float receiver_depth;
	float texel_size; // Meters.
};

#define REACH_NONE 0u
#define REACH_UNMAPPED 1u
#define REACH_SAMPLED 2u

vec3 to_world(vec3 view_pos) {
	return vec3(vsm_row(params.view_to_world[0], view_pos), vsm_row(params.view_to_world[1], view_pos), vsm_row(params.view_to_world[2], view_pos));
}

uint sample_directional(DebugLight light, vec3 world, float view_depth, float footprint, out DebugSample r_sample) {
	r_sample.map = VSM_MAP_NONE;
	r_sample.mip = VSM_MIP_COUNT;
	r_sample.level = 0u;
	r_sample.phys_texel = uvec2(0u);
	r_sample.page = uvec2(0u);
	r_sample.receiver_depth = 0.0;
	r_sample.texel_size = 0.0;
	uint table_maps = params.misc.x;
	uint count = (light.info.y >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	uint first = vsm_light_map(light.info.x, light.info.y, 0u);
	if (first >= table_maps || vsm_maps.data[first].kind != VSM_MAP_KIND_DIRECTIONAL) {
		return REACH_NONE;
	}
	VSMMapData level0 = vsm_maps.data[first];
	if (view_depth > level0.max_distance) {
		return REACH_NONE;
	}
	ivec2 texel0 = vsm_clipmap_texel0(level0, world);
	int level_mip = vsm_clipmap_level_mip(footprint, params.bias.x + level0.lod_bias);
	uint start_level = count;
	for (uint level = 0u; level < count; level++) {
		uint map = first + level;
		if (map >= table_maps) {
			break;
		}
		if (vsm_clipmap_in_window(vsm_maps.data[map].origin, texel0 >> int(level), 0u)) {
			start_level = level;
			break;
		}
	}
	if (start_level == count) {
		return REACH_NONE;
	}
	uint start_mip = uint(clamp(level_mip - int(start_level), 0, int(VSM_MIP_COUNT) - 1));
	for (uint level = start_level; level < count; level++) {
		uint map = first + level;
		if (map >= table_maps) {
			break;
		}
		uint mip = uint(max(int(start_mip) - int(level - start_level), 0));
		VSMMapData map_data = vsm_maps.data[map];
		ivec2 level_texel = texel0 >> int(level);
		VSMLookup lookup = vsm_lookup_clipmap(table_maps, map, map_data.origin, level_texel, mip);
		if (lookup.valid) {
			r_sample.map = map;
			r_sample.mip = lookup.mip;
			r_sample.level = level;
			r_sample.phys_texel = lookup.texel;
			r_sample.page = vsm_clipmap_page(level_texel, lookup.mip);
			r_sample.receiver_depth = vsm_row(map_data.row2, world);
			r_sample.texel_size = map_data.texel_scale * float(1u << lookup.mip);
			return REACH_SAMPLED;
		}
	}
	return REACH_UNMAPPED;
}

uint sample_local(DebugLight light, vec3 world, float footprint, out DebugSample r_sample) {
	r_sample.map = VSM_MAP_NONE;
	r_sample.mip = VSM_MIP_COUNT;
	r_sample.level = 0u;
	r_sample.phys_texel = uvec2(0u);
	r_sample.page = uvec2(0u);
	r_sample.receiver_depth = 0.0;
	r_sample.texel_size = 0.0;
	if (!vsm_light_reaches(light.info.z, world - light.position.xyz, uintBitsToFloat(light.info.w), light.direction.xyz, light.position.w)) {
		return REACH_NONE;
	}
	uint table_maps = params.misc.x;
	uint count = (light.info.y >> VSM_LIGHT_FLAGS_COUNT_SHIFT) & VSM_LIGHT_FLAGS_COUNT_MASK;
	for (uint face = 0u; face < count; face++) {
		uint map = vsm_light_map(light.info.x, light.info.y, face);
		if (map >= table_maps) {
			continue;
		}
		VSMMapData map_data = vsm_maps.data[map];
		if (map_data.kind == VSM_MAP_KIND_NONE || map_data.kind == VSM_MAP_KIND_DIRECTIONAL) {
			continue;
		}
		float w;
		uvec2 texel;
		if (!vsm_face_texel(map_data, world, w, texel)) {
			continue;
		}
		uint mip = vsm_biased_mip(int(vsm_face_mip(w * map_data.texel_scale, footprint)), params.bias.y + map_data.lod_bias);
		vec2 uv = (vec2(texel) + 0.5) / float(VSM_VIRTUAL_SIZE);
		VSMLookup lookup = vsm_lookup(table_maps, map, uv, mip);
		if (!lookup.valid) {
			return REACH_UNMAPPED;
		}
		r_sample.map = map;
		r_sample.mip = lookup.mip;
		r_sample.phys_texel = lookup.texel;
		r_sample.page = (texel >> lookup.mip) >> VSM_PAGE_SIZE_SHIFT;
		r_sample.receiver_depth = w;
		r_sample.texel_size = map_data.texel_scale * w * float(1u << lookup.mip);
		return REACH_SAMPLED;
	}
	return REACH_NONE;
}

uint sample_light(uint index, vec3 world, float view_depth, float footprint, out DebugSample r_sample) {
	DebugLight light = params.lights[index];
	if (light.info.z == VSM_MAP_KIND_DIRECTIONAL) {
		return sample_directional(light, world, view_depth, footprint, r_sample);
	}
	return sample_local(light, world, footprint, r_sample);
}

// The nearest caster of both layers at the sample's texel; +INF for an empty texel.
bool is_shadowed(DebugSample p_sample) {
	uint static_depth = imageLoad(vsm_pool, ivec3(ivec2(p_sample.phys_texel), 0)).r;
	uint dynamic_depth = imageLoad(vsm_pool, ivec3(ivec2(p_sample.phys_texel), 1)).r;
	// Larger packed values are nearer.
	float caster = vsm_depth_unpack(max(static_depth, dynamic_depth));
	float bias = max(2.0 * p_sample.texel_size, 0.01);
	return caster < p_sample.receiver_depth - bias;
}

uint hash_uint(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

vec3 hue_color(float h) {
	vec3 k = clamp(abs(fract(vec3(h) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
	return k;
}

vec3 mip_color(uint mip) {
	const vec3 colors[7] = vec3[](vec3(0.9, 0.1, 0.1), vec3(0.95, 0.5, 0.1), vec3(0.95, 0.9, 0.1), vec3(0.2, 0.8, 0.2), vec3(0.1, 0.8, 0.85), vec3(0.15, 0.3, 0.95), vec3(0.6, 0.2, 0.85));
	return colors[min(mip, 6u)];
}

vec3 heat_ramp(float t) {
	t = clamp(t, 0.0, 1.0);
	if (t < 0.5) {
		return mix(vec3(0.05, 0.1, 0.45), vec3(0.2, 0.8, 0.2), t * 2.0);
	}
	return mix(vec3(0.95, 0.85, 0.1), vec3(0.95, 0.1, 0.1), (t - 0.5) * 2.0);
}

uint page_info_of(DebugSample p_sample) {
	if (params.misc2.y == 0u) {
		return 0u;
	}
	uvec2 page = p_sample.phys_texel >> VSM_PAGE_SIZE_SHIFT;
	uint index = page.y * params.misc.w + page.x;
	return index < params.misc2.x ? page_info.data[index] : 0u;
}

const vec4 COLOR_UNMAPPED = vec4(1.0, 0.0, 1.0, 0.85);
const vec4 COLOR_NONE = vec4(0.0);

void main() {
	uvec2 internal_size = max(params.sizes.xy, uvec2(1u));
	uvec2 output_size = max(params.sizes.zw, uvec2(1u));
	vec2 uv = gl_FragCoord.xy / vec2(output_size);
	uvec2 pixel = min(uvec2(uv * vec2(internal_size)), internal_size - uvec2(1u));
	float depth = texelFetch(depth_buffer, ivec2(pixel), 0).r;
	uint mode = params.misc.z;
	if (!(depth > 0.0)) {
		// Reverse Z: the sky.
		frag_color = mode == DEBUG_MASK ? vec4(0.25, 0.25, 0.3, 1.0) : COLOR_NONE;
		return;
	}
	vec2 ndc = (vec2(pixel) + 0.5) / vec2(internal_size) * 2.0 - 1.0;
	vec3 clip = vec3(ndc, depth);
	float w = vsm_row(params.inv_projection[3], clip);
	vec3 view_pos = vec3(vsm_row(params.inv_projection[0], clip), vsm_row(params.inv_projection[1], clip), vsm_row(params.inv_projection[2], clip)) / w;
	vec3 world = to_world(view_pos);
	float view_depth = -view_pos.z;
	float footprint = view_depth * params.footprint.x + params.footprint.y;
	uint light_count = params.misc.y;

	if (mode == DEBUG_MASK) {
		uint reached = 0u;
		uint lit = 0u;
		bool unmapped = false;
		for (uint i = 0u; i < light_count; i++) {
			DebugSample s;
			uint reach = sample_light(i, world, view_depth, footprint, s);
			if (reach == REACH_UNMAPPED) {
				unmapped = true;
			} else if (reach == REACH_SAMPLED) {
				reached++;
				lit += is_shadowed(s) ? 0u : 1u;
			}
		}
		if (reached == 0u) {
			frag_color = unmapped ? COLOR_UNMAPPED : vec4(0.5, 0.5, 0.5, 1.0);
			return;
		}
		frag_color = vec4(vec3(float(lit) / float(reached)), 1.0);
		return;
	}

	// The other draws show the first light that reaches the pixel; DEBUG_CLIPMAP only directional lights.
	DebugSample s;
	uint reach = REACH_NONE;
	for (uint i = 0u; i < light_count && reach == REACH_NONE; i++) {
		if (mode == DEBUG_CLIPMAP && params.lights[i].info.z != VSM_MAP_KIND_DIRECTIONAL) {
			continue;
		}
		reach = sample_light(i, world, view_depth, footprint, s);
	}
	if (reach == REACH_NONE) {
		frag_color = mode == DEBUG_CLIPMAP ? vec4(0.5, 0.5, 0.5, 0.85) : COLOR_NONE;
		return;
	}
	if (reach == REACH_UNMAPPED) {
		frag_color = COLOR_UNMAPPED;
		return;
	}
	vec3 color;
	if (mode == DEBUG_MIP) {
		color = mip_color(s.mip);
	} else if (mode == DEBUG_PAGES) {
		uint h = hash_uint(s.map * 0x9E3779B9u ^ hash_uint((s.mip << 16u) ^ (s.page.y << 8u) ^ s.page.x));
		color = hue_color(float(h & 0xFFFFu) / 65536.0) * (0.55 + 0.45 * float((h >> 16u) & 0xFFu) / 255.0);
	} else if (mode == DEBUG_CACHE) {
		uint info = page_info_of(s);
		if ((info & 1u) != 0u) {
			color = vec3(0.95, 0.15, 0.1); // Both layers, or the static layer, rendered this view.
		} else if ((info & 2u) != 0u) {
			color = vec3(0.15, 0.3, 0.95); // Only the dynamic layer rendered: the static one is cached.
		} else {
			color = vec3(0.15, 0.85, 0.2); // Cached.
		}
	} else if (mode == DEBUG_OVERDRAW) {
		uint draws = page_info_of(s) >> 8u;
		color = draws == 0u ? vec3(0.05, 0.05, 0.2) : heat_ramp(float(draws) / 16.0);
	} else {
		color = hue_color(float(s.level) / 8.0) * (0.6 + 0.4 * float(s.level & 1u));
	}
	// Page edges, so the page grid shows.
	uvec2 local = s.phys_texel & uvec2(VSM_PAGE_SIZE - 1u);
	if (mode != DEBUG_CLIPMAP && (any(equal(local, uvec2(0u))) || any(equal(local, uvec2(VSM_PAGE_SIZE - 1u))))) {
		color *= 0.4;
	}
	frag_color = vec4(color, 0.85);
}
