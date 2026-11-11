// GodotNext: ps_swrt_trace (#186). Not a standalone shader: the D6 card lookup at an
// instance-local position and normal, from a page table and a card set. Voxel lighting
// (nextgi_voxel_lighting.glsl) shades its stored hits with it, and the software gather
// (nextgi_gather_swrt.glsl) its detail (mesh-SDF) hits, which know their instance.
//
// The includer declares the resources and names them, and declares PAGE_EVICTED_BIT and
// CARD_DEPTH_TEXELS:
//   NEXTGI_D6_CARDS, NEXTGI_D6_CARD_SETS, NEXTGI_D6_PAGE_RECTS  storage buffers (.data[])
//   NEXTGI_D6_CARD_T, NEXTGI_D6_CARD_SET_T                      their element types
//   NEXTGI_D6_MATERIAL_DEPTH, NEXTGI_D6_RADIANCE                the SRC depth and radiance atlases
//   NEXTGI_D6_CARD_COUNT, NEXTGI_D6_CARD_SET_COUNT, NEXTGI_D6_PAGE_RECT_COUNT  their entry counts
// Page rects are (origin.xy, size.xy) packed as two uints.

#ifndef NEXTGI_SRC_D6_LOCAL_INC_GLSL
#define NEXTGI_SRC_D6_LOCAL_INC_GLSL

// D6. Returns false when no card qualifies; then the hit falls back to the WRC.
bool resample_src(vec3 position, vec3 normal, uint page_table, uint card_set_index, out vec3 radiance, out float projection) {
	radiance = vec3(0.0);
	projection = 0.0;
	if (page_table == 0xffffffffu || (page_table & PAGE_EVICTED_BIT) != 0u || card_set_index >= NEXTGI_D6_CARD_SET_COUNT) {
		return false;
	}
	NEXTGI_D6_CARD_SET_T card_set = NEXTGI_D6_CARD_SETS.data[card_set_index];
	if (card_set.first_card >= NEXTGI_D6_CARD_COUNT || card_set.card_count > NEXTGI_D6_CARD_COUNT - card_set.first_card) {
		return false;
	}
	float best_align = -2.0;
	bool found = false;
	for (uint i = 0u; i < card_set.card_count; i++) {
		uint page_index = page_table + i;
		if (page_index >= NEXTGI_D6_PAGE_RECT_COUNT) {
			continue;
		}
		NEXTGI_D6_CARD_T card = NEXTGI_D6_CARDS.data[card_set.first_card + i];
		vec3 rel = position - card.origin.xyz;
		float u = dot(rel, card.axis_u.xyz);
		float v = dot(rel, card.axis_v.xyz);
		if (abs(u) > card.axis_u.w || abs(v) > card.axis_v.w) {
			continue;
		}
		float align = dot(normal, card.axis_n.xyz);
		if (align <= best_align) {
			continue;
		}
		float depth = -dot(rel, card.axis_n.xyz);
		vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
		vec2 texel = vec2((u / card.axis_u.w) * 0.5 + 0.5, (v / card.axis_v.w) * 0.5 + 0.5) * card_size;
		ivec2 material = ivec2(card.atlas_rect.xy) + ivec2(clamp(texel, vec2(0.0), card_size - 1.0));
		material = clamp(material, ivec2(0), textureSize(NEXTGI_D6_MATERIAL_DEPTH, 0) - ivec2(1));
		float card_depth = texelFetch(NEXTGI_D6_MATERIAL_DEPTH, material, 0).r;
		// A card texel is the larger of the set's texel size and the rect's own footprint, as in
		// nextgi_src_lookup's Stage-2 path. Its regular path uses the texel size alone; the two
		// differ only for a rect the atlas downscaled, where this one is looser.
		float tolerance = CARD_DEPTH_TEXELS * max(card_set.texel_size, max(2.0 * card.axis_u.w / card_size.x, 2.0 * card.axis_v.w / card_size.y));
		if (!(card_depth > 0.0 && abs(card_depth - depth) <= tolerance)) {
			continue;
		}
		uvec2 page = NEXTGI_D6_PAGE_RECTS.data[page_index];
		uvec2 page_origin = uvec2(page.x & 0xffffu, page.x >> 16u);
		uvec2 page_size = max(uvec2(page.y & 0xffffu, page.y >> 16u), uvec2(1u));
		ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - vec2(1.0)));
		radiance_coord = clamp(radiance_coord, ivec2(0), textureSize(NEXTGI_D6_RADIANCE, 0) - ivec2(1));
		radiance = texelFetch(NEXTGI_D6_RADIANCE, radiance_coord, 0).rgb;
		best_align = align;
		found = true;
	}
	projection = clamp(best_align, 0.0, 1.0);
	return found;
}

#endif // NEXTGI_SRC_D6_LOCAL_INC_GLSL
