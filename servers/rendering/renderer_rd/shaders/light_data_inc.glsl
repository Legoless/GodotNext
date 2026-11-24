/**************************************************************************/
/*  light_data_inc.glsl                                                   */
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

#define LIGHT_BAKE_DISABLED 0
#define LIGHT_BAKE_STATIC 1
#define LIGHT_BAKE_DYNAMIC 2

struct LightData { //this structure needs to be as packed as possible
	vec3 position;
	float inv_radius;

	vec3 direction;
	float size;

	vec3 color;
	float attenuation;

	mediump vec3 area_width;
	float cone_attenuation; // area lights: 1 / (range + diagonal/2)

	mediump vec3 area_height;
	float cone_angle; // area lights: max mipmaps

	float specular_amount;
	float shadow_opacity;
	float pad[2];

	vec4 atlas_rect; // rect in the shadow atlas
	mat4 shadow_matrix;
	float shadow_bias;
	float shadow_normal_bias;
	float transmittance_bias;
	float soft_shadow_size; // for spot, it's the size in uv coordinates of the light, for omni it's the span angle
	float soft_shadow_scale; // scales the shadow kernel for blurrier shadows
	uint mask;
	float volumetric_fog_energy;
	uint bake_mode;
	vec4 projector_rect; //projector rect in srgb decal atlas
	// GodotNext: vsm_page_pool (#220) and light_function_atlas (#138).
	// vsm_map_base is the light's first virtual shadow map or VSM_MAP_NONE (0xFFFFFFFF);
	// light_function_index indexes the LightFunctionEntry SSBO or LIGHT_FUNCTION_NONE (0xFFFFFFFF).
	uint vsm_map_base;
	uint vsm_flags;
	uint light_function_index;
	uint vsm_pad1;
	// GodotNext: light_source_parity (#140). IES profile, barn-door shading, and per-light contact shadows.
	uint ies_layer;
	float ies_scale;
	float barn_door_angle_rad;
	float barn_door_length;
	float contact_shadow_length;
	uint contact_shadow_sscs_index;
	float contact_shadow_casting_intensity;
	float contact_shadow_non_casting_intensity;
	// GodotNext: rt_shadows (#141). Tile-sparse lookup base (UINT32_MAX when unused) and packed screen-tile rect + flags.
	uint rt_shadow_tile_base;
	uint rt_shadow_flags;
	uint rt_shadow_pad0;
	uint rt_shadow_pad1;
};

#define REFLECTION_AMBIENT_DISABLED 0
#define REFLECTION_AMBIENT_ENVIRONMENT 1
#define REFLECTION_AMBIENT_COLOR 2

struct ReflectionData {
	vec3 box_extents;
	float index;
	vec3 box_offset;
	uint mask;
	vec3 ambient; // ambient color
	float intensity;
	float blend_distance;
	bool exterior;
	bool box_project;
	uint ambient_mode;
	float exposure_normalization;
	float pad0;
	float pad1;
	float pad2;
	//0-8 is intensity,8-9 is ambient, mode
	mat4 local_matrix; // up to here for spot and omni, rest is for directional
	// notes: for ambientblend, use distance to edge to blend between already existing global environment
};

struct DirectionalLightData {
	vec3 direction;
	float energy; // needs to be highp to avoid NaNs being created with high energy values (i.e. when using physical light units and over-exposing the image)
	vec3 color;
	float size;
	float specular;
	uint mask;
	float softshadow_angle;
	float soft_shadow_scale;
	bool blend_splits;
	float shadow_opacity;
	float fade_from;
	float fade_to;
	uint sscs_index;
	uint light_function_index; // GodotNext: light_function_atlas (#138). Index into LightFunctionEntry SSBO or LIGHT_FUNCTION_NONE.
	uint bake_mode;
	float volumetric_fog_energy;
	vec4 shadow_bias;
	vec4 shadow_normal_bias;
	vec4 shadow_transmittance_bias;
	vec4 shadow_z_range;
	vec4 shadow_range_begin;
	vec4 shadow_split_offsets;
	mat4 shadow_matrix1;
	mat4 shadow_matrix2;
	mat4 shadow_matrix3;
	mat4 shadow_matrix4;
	vec2 uv_scale1;
	vec2 uv_scale2;
	vec2 uv_scale3;
	vec2 uv_scale4;
	// GodotNext: vsm_page_pool (#220) and df_shadows (#214). As LightData's, plus directional DF handoff distances.
	uint vsm_map_base;
	uint vsm_flags;
	float df_shadow_start;
	float df_shadow_distance;
	// GodotNext: light_source_parity (#140). Per-light contact shadow controls.
	float contact_shadow_length;
	uint contact_shadow_flags;
	float contact_shadow_casting_intensity;
	float contact_shadow_non_casting_intensity;
	// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Standalone RT shadow tile lookup base and flags.
	uint rt_shadow_tile_base;
	uint rt_shadow_flags;
	uint rt_shadow_pad0;
	uint rt_shadow_pad1;
};
