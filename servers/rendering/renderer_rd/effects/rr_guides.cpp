/**************************************************************************/
/*  rr_guides.cpp                                                         */
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

#include "rr_guides.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererRD {

static bool rr_guides_requested = false;

bool RRGuides::is_requested() {
	return rr_guides_requested || is_dev_switch_on();
}

void RRGuides::set_requested(bool p_requested) {
	rr_guides_requested = p_requested;
}

bool RRGuides::is_dev_switch_on() {
#ifdef DEBUG_ENABLED
	static const bool dev_switch = OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("GODOT_DEV_RR_GUIDES") == "1";
	return dev_switch;
#else
	return false;
#endif
}

bool RRGuides::view_writes_guides(bool p_requested, bool p_reflection_probe, uint32_t p_view_count, bool p_msaa) {
	if (!p_requested || p_reflection_probe || p_view_count != 1) {
		return false;
	}
	if (p_msaa) {
		WARN_PRINT_ONCE("The DLSS Ray Reconstruction guides are not written for this view because MSAA is enabled.");
		return false;
	}
	return true;
}

void RRGuides::clear_guides(BitField<RD::DrawFlags> &r_flags, Vector<Color> &r_clear_colors, bool p_has_specular, bool p_has_velocity) {
	const uint32_t first = first_guide_color_index(p_has_specular, p_has_velocity);
	// The padding below would turn a pass's missing clear color into (0, 0, 0, 1) and hide the
	// error draw_list_begin() gives for it.
	for (uint32_t i = uint32_t(r_clear_colors.size()); i < first; i++) {
		ERR_FAIL_COND_MSG(r_flags.has_flag(BitField<RD::DrawFlags>(RD::DRAW_CLEAR_COLOR_0 << i)), vformat("Color attachment %d is cleared, but the pass gives no clear color for it.", i));
	}
	if (uint32_t(r_clear_colors.size()) < first + 2) {
		r_clear_colors.resize(first + 2);
	}
	r_clear_colors.write[first] = diffuse_albedo_clear_color();
	r_clear_colors.write[first + 1] = specular_albedo_clear_color();
	for (uint32_t i = first; i < first + 2; i++) {
		r_flags.clear_flag(BitField<RD::DrawFlags>(RD::DRAW_IGNORE_COLOR_0 << i));
		r_flags.set_flag(BitField<RD::DrawFlags>(RD::DRAW_CLEAR_COLOR_0 << i));
	}
}

String RRGuides::glsl_pack_prelude() {
	String prelude;
	prelude += "#define RR_ROUGHNESS_STORED_MAX " + itos(ROUGHNESS_STORED_MAX) + "\n";
	prelude += "#define RR_DYNAMIC_FLAG_THRESHOLD " + rtos(DYNAMIC_FLAG_THRESHOLD) + "\n";
	// The prepass stores encode24(view-space normal) * 0.5 + 0.5; encode24() only scales it.
	prelude += "#define RR_DECODE_NORMAL(e) ((e) * 2.0 - 1.0)\n";
	prelude += "#define RR_DECODE_ROUGHNESS(w) (((w) > RR_DYNAMIC_FLAG_THRESHOLD ? 1.0 - (w) : (w)) * (255.0 / float(RR_ROUGHNESS_STORED_MAX)))\n";
	return prelude;
}

Vector3 RRGuides::unpack_world_normal(const Basis &p_view_to_world, const Vector3 &p_encoded) {
	// RR_DECODE_NORMAL(), then the rotation and normalization of rr_guide_pack.glsl.
	const Vector3 normal = p_view_to_world.xform(p_encoded * 2.0 - Vector3(1.0, 1.0, 1.0));
	const real_t length_squared = normal.length_squared();
	return length_squared > 1e-12 ? Vector3(normal / Math::sqrt(length_squared)) : Vector3();
}

float RRGuides::unpack_roughness(float p_stored) {
	// RR_DECODE_ROUGHNESS().
	const float roughness = p_stored > DYNAMIC_FLAG_THRESHOLD ? 1.0f - p_stored : p_stored;
	return roughness * (255.0f / float(ROUGHNESS_STORED_MAX));
}

void RRGuides::pack(RID p_source_normal_roughness, RID p_diffuse_albedo, RID p_dest_normal_roughness, const Size2i &p_size, const Basis &p_view_to_world) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	PushConstant push_constant;
	for (int i = 0; i < 3; i++) {
		const Vector3 column = p_view_to_world.get_column(i);
		push_constant.view_to_world[i * 4 + 0] = column.x;
		push_constant.view_to_world[i * 4 + 1] = column.y;
		push_constant.view_to_world[i * 4 + 2] = column.z;
		push_constant.view_to_world[i * 4 + 3] = 0.0;
	}
	push_constant.size[0] = p_size.x;
	push_constant.size[1] = p_size.y;
	push_constant.pad[0] = 0;
	push_constant.pad[1] = 0;

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID pack_shader = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(pack_shader.is_null());

	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_source_normal_roughness }));
	RD::Uniform u_diffuse_albedo(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ default_sampler, p_diffuse_albedo }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 2, p_dest_normal_roughness);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(pack_shader, 0, u_normal_roughness, u_diffuse_albedo, u_dest), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}

RRGuides::RRGuides() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes, glsl_pack_prelude());
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

RRGuides::~RRGuides() {
	shader.version_free(shader_version);
}

} // namespace RendererRD
