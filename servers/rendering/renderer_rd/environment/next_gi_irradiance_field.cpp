/**************************************************************************/
/*  next_gi_irradiance_field.cpp                                          */
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

#include "next_gi_irradiance_field.h"

#include "servers/rendering/renderer_rd/environment/gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_irradiance_field.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

using namespace RendererRD;

namespace {

struct LitePushGPU {
	uint32_t mode = 0;
	uint32_t probe_base = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};

static_assert(sizeof(NextGIIrradianceField::Params) == 208, "Lite params stay 208 bytes so the shader std430 layout matches.");
static_assert(sizeof(LitePushGPU) == 16, "Lite push constants stay 16 bytes.");
static_assert(NextGIIrradianceField::PROBE_COUNT == NextGI::LITE_PROBE_COUNT && NextGIIrradianceField::PROBE_VEC4S == NextGI::LITE_PROBE_VEC4S, "The Lite probe layout is shared with the software tier's probe trace.");

} // namespace

NextGIIrradianceField::Params NextGIIrradianceField::build_params(const Projection &p_cam_projection, const Transform3D &p_cam_transform, bool p_flip_y, const Vector2 &p_taa_jitter, const Size2i &p_gi_size, const Size2i &p_half_size, const Size2i &p_internal_size, bool p_depth_valid, bool p_normal_valid, bool p_hwrt) {
	Params params = {};
	// The depth buffer holds reverse-Z depth in [0, 1] drawn with this corrected projection, so
	// the shader unprojects vec4(uv * 2 - 1, depth, 1) with its inverse.
	const Projection view_proj = NextGIViewMath::view_projection(p_cam_projection, p_cam_transform, p_flip_y, p_taa_jitter);
	NextGIViewMath::store(view_proj.inverse(), params.inv_view_proj);
	const Vector3 camera = p_cam_transform.origin;
	params.camera[0] = camera.x;
	params.camera[1] = camera.y;
	params.camera[2] = camera.z;
	params.camera[3] = p_hwrt ? 1.0f : 0.0f;
	params.screen[0] = float(MAX(p_gi_size.x, 1));
	params.screen[1] = float(MAX(p_gi_size.y, 1));
	params.screen[2] = float(MAX(p_half_size.x, 1));
	params.screen[3] = float(MAX(p_half_size.y, 1));
	params.depth_info[0] = float(MAX(p_internal_size.x, 1));
	params.depth_info[1] = float(MAX(p_internal_size.y, 1));
	params.depth_info[2] = p_depth_valid ? 1.0f : 0.0f;
	params.depth_info[3] = p_normal_valid ? 1.0f : 0.0f;
	params.rules[0] = NextGI::LITE_INSIDE_BACKFACE;
	params.rules[1] = NextGI::LITE_PUSH_SPACING;
	params.rules[2] = NextGI::LITE_SSR_ROUGHNESS;
	params.rules[3] = NextGI::LITE_MIP2_ROUGHNESS;
	// The probes are the WRC's own level 0 and 1 probes, on the grid the WRC update snaps.
	for (uint32_t level = 0; level < NextGI::LITE_PROBE_LEVELS; level++) {
		const float spacing = nextgi_wrc_spacing(level);
		const Vector3 origin = nextgi_wrc_snap_origin(camera, spacing);
		params.level_origin[level][0] = origin.x;
		params.level_origin[level][1] = origin.y;
		params.level_origin[level][2] = origin.z;
		params.level_origin[level][3] = spacing;
	}
	NextGIViewMath::store_basis(p_cam_transform.basis, params.view_to_world);
	return params;
}

struct NextGIIrradianceField::GpuState {
	NextgiIrradianceFieldShaderRD shader;
	RID version;
	RID pipeline;
	RID sampler;
	RID dummy_2d;
	RID dummy_3d;
	RID params_buffer;
	bool ready = false;
};

NextGIIrradianceField::NextGIIrradianceField() {
}

NextGIIrradianceField::~NextGIIrradianceField() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		const RID rids[] = { probe_buffer, half_image };
		for (const RID &rid : rids) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
		if (gpu != nullptr) {
			const RID gpu_rids[] = { gpu->pipeline, gpu->sampler, gpu->dummy_2d, gpu->dummy_3d, gpu->params_buffer };
			for (const RID &rid : gpu_rids) {
				if (rid.is_valid()) {
					rd->free_rid(rid);
				}
			}
			if (gpu->version.is_valid()) {
				gpu->shader.version_free(gpu->version);
			}
		}
	}
	if (gpu != nullptr) {
		memdelete(gpu);
		gpu = nullptr;
	}
}

bool NextGIIrradianceField::ensure_probe_buffer() {
	if (probe_buffer.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	// Every probe starts not traced: validity 1 is above the inside threshold, so nothing reads it.
	Vector<float> cleared;
	cleared.resize(PROBE_COUNT * PROBE_VEC4S * 4);
	cleared.fill(0.0f);
	for (uint32_t probe = 0; probe < PROBE_COUNT; probe++) {
		cleared.write[probe * PROBE_VEC4S * 4 + 3] = 1.0f;
	}
	probe_buffer = rd->storage_buffer_create(cleared.size() * sizeof(float), Span<uint8_t>((const uint8_t *)cleared.ptr(), cleared.size() * sizeof(float)));
	return probe_buffer.is_valid();
}

uint32_t NextGIIrradianceField::_consume(uint32_t &r_cursor) {
	const uint32_t base = r_cursor;
	r_cursor = (r_cursor + PROBES_PER_FRAME) % PROBE_COUNT;
	return base;
}

uint32_t NextGIIrradianceField::consume_swrt_probe_base() {
	return _consume(swrt_cursor);
}

uint32_t NextGIIrradianceField::consume_hwrt_probe_base() {
	return _consume(hwrt_cursor);
}

bool NextGIIrradianceField::_ensure_gpu() {
	if (shader_failed) {
		return false;
	}
	if (gpu != nullptr && gpu->ready) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	if (gpu == nullptr) {
		gpu = memnew(GpuState);
	}
	if (!gpu->version.is_valid()) {
		gpu->shader.initialize(Vector<String>({ "" }));
		gpu->version = gpu->shader.version_create(false);
		if (gpu->version.is_valid()) {
			gpu->pipeline = rd->compute_pipeline_create(gpu->shader.version_get_shader(gpu->version, 0));
		}
	}
	if (!gpu->pipeline.is_valid()) {
		shader_failed = true;
		return false;
	}
	if (!gpu->sampler.is_valid()) {
		RD::SamplerState state;
		state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		gpu->sampler = rd->sampler_create(state);
	}
	if (!gpu->dummy_2d.is_valid()) {
		RD::TextureFormat format;
		format.width = 1;
		format.height = 1;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		gpu->dummy_2d = rd->texture_create(format, RD::TextureView());
		if (gpu->dummy_2d.is_valid()) {
			rd->texture_clear(gpu->dummy_2d, Color(0, 0, 0, 1), 0, 1, 0, 1);
		}
	}
	if (!gpu->dummy_3d.is_valid()) {
		RD::TextureFormat format;
		format.width = 1;
		format.height = 1;
		format.depth = 1;
		format.texture_type = RD::TEXTURE_TYPE_3D;
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		gpu->dummy_3d = rd->texture_create(format, RD::TextureView());
		if (gpu->dummy_3d.is_valid()) {
			rd->texture_clear(gpu->dummy_3d, Color(0, 0, 0, 1), 0, 1, 0, 1);
		}
	}
	if (!gpu->params_buffer.is_valid()) {
		gpu->params_buffer = rd->storage_buffer_create(sizeof(Params));
	}
	gpu->ready = gpu->sampler.is_valid() && gpu->dummy_2d.is_valid() && gpu->dummy_3d.is_valid() && gpu->params_buffer.is_valid();
	if (!gpu->ready) {
		shader_failed = true;
	}
	return gpu->ready;
}

void NextGIIrradianceField::integrate(RenderDataRD *p_render_data, bool p_hwrt, bool p_reflections_only) {
	if (p_reflections_only || p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	Ref<RenderSceneBuffersRD> buffers = p_render_data->render_buffers;
	if (buffers.is_null()) {
		return;
	}
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi == nullptr || !_ensure_gpu() || !ensure_probe_buffer()) {
		return;
	}
	const bool half_resolution = GI::get_singleton() != nullptr && GI::get_singleton()->is_half_resolution();
	next_gi->ensure_gi_buffers(buffers.ptr(), half_resolution);
	if (!buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT) || !buffers->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION)) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const Size2i gi_size = next_gi->get_gi_buffer_size(buffers->get_internal_size(), half_resolution);
	const Size2i wanted_half(MAX(gi_size.x / 2, 1), MAX(gi_size.y / 2, 1));
	if (!half_image.is_valid() || half_size != wanted_half) {
		if (half_image.is_valid()) {
			rd->free_rid(half_image);
			half_image = RID();
		}
		RD::TextureFormat format;
		format.width = wanted_half.x;
		format.height = wanted_half.y;
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		half_image = rd->texture_create(format, RD::TextureView());
		half_size = wanted_half;
		if (half_image.is_valid()) {
			rd->texture_clear(half_image, Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
	}
	if (!half_image.is_valid()) {
		return;
	}

	RID depth = gpu->dummy_2d;
	float depth_valid = 0.0f;
	if (buffers->get_depth_texture().is_valid()) {
		depth = buffers->get_depth_texture();
		depth_valid = 1.0f;
	}
	const StringName forward_scope("forward_clustered");
	const StringName normal_name("normal_roughness");
	RID normal = gpu->dummy_2d;
	float normal_valid = 0.0f;
	if (buffers->has_texture(forward_scope, normal_name)) {
		normal = buffers->get_texture(forward_scope, normal_name);
		normal_valid = 1.0f;
	}
	RID radiance = gpu->dummy_2d;
	RID visibility = gpu->dummy_2d;
	RID offsets = gpu->dummy_3d;
	RID mip2 = gpu->dummy_2d;
	RID mip4 = gpu->dummy_2d;
	bool wrc_bound = false;
	if (p_hwrt) {
		NextGIWRC *wrc = next_gi->get_wrc();
		if (wrc != nullptr && wrc->get_radiance().is_valid() && wrc->get_visibility().is_valid() && wrc->get_offsets().is_valid() && wrc->get_mip2().is_valid() && wrc->get_mip4().is_valid()) {
			radiance = wrc->get_radiance();
			visibility = wrc->get_visibility();
			offsets = wrc->get_offsets();
			mip2 = wrc->get_mip2();
			mip4 = wrc->get_mip4();
			wrc_bound = true;
		}
	}

	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const Params params = build_params(scene_data->cam_projection, scene_data->cam_transform, scene_data->flip_y, scene_data->taa_jitter, gi_size, wanted_half, buffers->get_internal_size(), depth_valid > 0.5f, normal_valid > 0.5f, wrc_bound);
	rd->buffer_update(gpu->params_buffer, 0, sizeof(params), &params);

	const RID ambient = buffers->get_texture_slice(RB_SCOPE_GI, RB_TEX_AMBIENT, 0, 0);
	const RID reflection = buffers->get_texture_slice(RB_SCOPE_GI, RB_TEX_REFLECTION, 0, 0);
	const RID shader = gpu->shader.version_get_shader(gpu->version, 0);
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, probe_buffer));
	{
		RD::Uniform depth_uniform;
		depth_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		depth_uniform.binding = 1;
		depth_uniform.append_id(gpu->sampler);
		depth_uniform.append_id(depth);
		uniforms.push_back(depth_uniform);
	}
	{
		RD::Uniform normal_uniform;
		normal_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		normal_uniform.binding = 2;
		normal_uniform.append_id(gpu->sampler);
		normal_uniform.append_id(normal);
		uniforms.push_back(normal_uniform);
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, half_image));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, ambient));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, reflection));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, gpu->params_buffer));
	{
		RD::Uniform radiance_uniform;
		radiance_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		radiance_uniform.binding = 7;
		radiance_uniform.append_id(gpu->sampler);
		radiance_uniform.append_id(radiance);
		uniforms.push_back(radiance_uniform);
	}
	{
		RD::Uniform visibility_uniform;
		visibility_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		visibility_uniform.binding = 8;
		visibility_uniform.append_id(gpu->sampler);
		visibility_uniform.append_id(visibility);
		uniforms.push_back(visibility_uniform);
	}
	{
		RD::Uniform offset_uniform;
		offset_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		offset_uniform.binding = 9;
		offset_uniform.append_id(gpu->sampler);
		offset_uniform.append_id(offsets);
		uniforms.push_back(offset_uniform);
	}
	{
		RD::Uniform mip_uniform;
		mip_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		mip_uniform.binding = 10;
		mip_uniform.append_id(gpu->sampler);
		mip_uniform.append_id(mip2);
		uniforms.push_back(mip_uniform);
	}
	{
		RD::Uniform mip_uniform;
		mip_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		mip_uniform.binding = 11;
		mip_uniform.append_id(gpu->sampler);
		mip_uniform.append_id(mip4);
		uniforms.push_back(mip_uniform);
	}
	const RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
	if (!uniform_set.is_valid()) {
		return;
	}
	const uint32_t half_groups_x = (uint32_t(wanted_half.x) + 7u) / 8u;
	const uint32_t half_groups_y = (uint32_t(wanted_half.y) + 7u) / 8u;
	const uint32_t gi_groups_x = (uint32_t(MAX(gi_size.x, 1)) + 7u) / 8u;
	const uint32_t gi_groups_y = (uint32_t(MAX(gi_size.y, 1)) + 7u) / 8u;
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, gpu->pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	if (wrc_bound) {
		LitePushGPU pack_push;
		pack_push.mode = 0;
		pack_push.probe_base = consume_hwrt_probe_base();
		rd->compute_list_set_push_constant(list, &pack_push, sizeof(pack_push));
		rd->compute_list_dispatch(list, PROBES_PER_FRAME, 1, 1);
		rd->compute_list_add_barrier(list);
		next_gi->note_lite_probe_traces(PROBES_PER_FRAME);
	}
	LitePushGPU interpolate_push;
	interpolate_push.mode = 1;
	rd->compute_list_set_push_constant(list, &interpolate_push, sizeof(interpolate_push));
	rd->compute_list_dispatch(list, half_groups_x, half_groups_y, 1);
	rd->compute_list_add_barrier(list);
	LitePushGPU upsample_push;
	upsample_push.mode = 2;
	rd->compute_list_set_push_constant(list, &upsample_push, sizeof(upsample_push));
	rd->compute_list_dispatch(list, gi_groups_x, gi_groups_y, 1);
	rd->compute_list_end();
	rd->free_rid(uniform_set);
}
