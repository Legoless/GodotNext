/**************************************************************************/
/*  denoise.cpp                                                           */
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

#include "denoise.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

NextGIDenoise::NextGIDenoise() {
	Vector<String> shader_modes;
	shader_modes.push_back("\n#define MODE_PROBE_SPATIAL\n");
	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

NextGIDenoise::~NextGIDenoise() {
	RD::get_singleton()->free_rid(pipeline);
	shader.version_free(shader_version);
}

void NextGIDenoise::process_diffuse(const DiffuseParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.probe_radiance.is_valid(), "NextGIDenoise: probe radiance is invalid.");
	ERR_FAIL_COND_MSG(!p_params.probe_radiance_out.is_valid(), "NextGIDenoise: probe radiance output is invalid.");
	ERR_FAIL_COND_MSG(!p_params.depth.is_valid(), "NextGIDenoise: depth is invalid.");
	ERR_FAIL_COND_MSG(!p_params.normal_roughness.is_valid(), "NextGIDenoise: normal-roughness guide is invalid.");
	ERR_FAIL_COND_MSG(!p_params.probe_hit_distance.is_valid(), "NextGIDenoise: probe hit distance is invalid.");
	ERR_FAIL_COND_MSG(p_params.probe_radiance == p_params.probe_radiance_out, "NextGIDenoise: the spatial pass cannot read and write the same probe texture.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	DiffusePushConstant push_constant;
	for (int i = 0; i < 16; i++) {
		push_constant.inv_projection[i] = p_params.inv_projection.columns[i / 4][i % 4];
	}
	push_constant.probe_size[0] = p_params.probe_size.width;
	push_constant.probe_size[1] = p_params.probe_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.depth_threshold = p_params.depth_threshold;
	push_constant.normal_power = p_params.normal_power;
	push_constant.probe_temporal_max_history = p_params.probe_temporal_max_history;
	push_constant.stabilizer_enabled = p_params.stabilizer_enabled ? 1u : 0u;

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID denoise_shader = shader.version_get_shader(shader_version, 0);

	RD::Uniform u_probe_radiance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_params.probe_radiance }));
	RD::Uniform u_probe_radiance_out(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_params.probe_radiance_out }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ default_sampler, p_params.depth }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ default_sampler, p_params.normal_roughness }));
	RD::Uniform u_probe_hit(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ default_sampler, p_params.probe_hit_distance }));

	RID uniform_set = uniform_set_cache->get_cache(denoise_shader, 0, u_probe_radiance, u_probe_radiance_out, u_depth, u_normal_roughness, u_probe_hit);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the diffuse uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DiffusePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.probe_size.width, p_params.probe_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

PackedByteArray NextGIDenoise::select_reset_probes(const Vector<Rect2i> &p_dirty_regions, const Size2i &p_probe_size) {
	const int width = MAX(p_probe_size.width, 0);
	const int height = MAX(p_probe_size.height, 0);

	PackedByteArray mask;
	mask.resize(width * height);
	mask.fill(0);
	if (width == 0 || height == 0) {
		return mask;
	}

	const Rect2i bounds(Point2i(), Size2i(width, height));
	uint8_t *mask_data = mask.ptrw();
	for (const Rect2i &region : p_dirty_regions) {
		const Rect2i clipped = region.intersection(bounds);
		for (int y = clipped.position.y; y < clipped.position.y + clipped.size.height; y++) {
			for (int x = clipped.position.x; x < clipped.position.x + clipped.size.x; x++) {
				mask_data[y * width + x] = 1;
			}
		}
	}
	return mask;
}
