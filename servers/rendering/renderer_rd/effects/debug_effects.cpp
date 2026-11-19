/**************************************************************************/
/*  debug_effects.cpp                                                     */
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

#include "debug_effects.h"

#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
// GodotNext: vsm_settings_debug (#221).
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

using namespace RendererRD;

DebugEffects::DebugEffects() {
	{
		// Shadow Frustum debug shader
		Vector<String> modes;
		modes.push_back("");

		shadow_frustum.shader.initialize(modes);
		shadow_frustum.shader_version = shadow_frustum.shader.version_create();

		RD::PipelineRasterizationState raster_state = RD::PipelineRasterizationState();
		shadow_frustum.pipelines[SFP_TRANSPARENT].setup(shadow_frustum.shader.version_get_shader(shadow_frustum.shader_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_blend(), 0);

		raster_state.wireframe = true;
		shadow_frustum.pipelines[SFP_WIREFRAME].setup(shadow_frustum.shader.version_get_shader(shadow_frustum.shader_version, 0), RD::RENDER_PRIMITIVE_LINES, raster_state, RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);
	}

	{
		// Motion Vectors debug shader.
		Vector<String> modes;
		modes.push_back("");

		motion_vectors.shader.initialize(modes);
		motion_vectors.shader_version = motion_vectors.shader.version_create();

		motion_vectors.pipeline.setup(motion_vectors.shader.version_get_shader(motion_vectors.shader_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_blend(), 0);
	}

	{
		// GodotNext: nextgeo_exclusive_dispatch (#79). NextGeo RT proxy depth-difference overlay.
		Vector<String> modes;
		modes.push_back("");

		nextgeo_rt_proxy.shader.initialize(modes);
		nextgeo_rt_proxy.shader_version = nextgeo_rt_proxy.shader.version_create();

		nextgeo_rt_proxy.pipeline.setup(nextgeo_rt_proxy.shader.version_get_shader(nextgeo_rt_proxy.shader_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_blend(), 0);
	}
}

void DebugEffects::_create_frustum_arrays() {
	if (frustum.vertex_buffer.is_null()) {
		// Create vertex buffer, but don't put data in it yet
		frustum.vertex_buffer = RD::get_singleton()->vertex_buffer_create(8 * sizeof(float) * 3, Vector<uint8_t>());

		Vector<RD::VertexAttribute> attributes;
		Vector<RID> buffers;
		RD::VertexAttribute vd;

		vd.location = 0;
		vd.stride = sizeof(float) * 3;
		vd.format = RD::DATA_FORMAT_R32G32B32_SFLOAT;

		attributes.push_back(vd);
		buffers.push_back(frustum.vertex_buffer);

		frustum.vertex_format = RD::get_singleton()->vertex_format_create(attributes);
		frustum.vertex_array = RD::get_singleton()->vertex_array_create(8, frustum.vertex_format, buffers);
	}

	if (frustum.index_buffer.is_null()) {
		uint16_t indices[6 * 2 * 3] = {
			// Far
			0, 1, 2, // FLT, FLB, FRT
			1, 3, 2, // FLB, FRB, FRT
			// Near
			4, 6, 5, // NLT, NRT, NLB
			6, 7, 5, // NRT, NRB, NLB
			// Left
			0, 4, 1, // FLT, NLT, FLB
			4, 5, 1, // NLT, NLB, FLB
			// Right
			6, 2, 7, // NRT, FRT, NRB
			2, 3, 7, // FRT, FRB, NRB
			// Top
			0, 2, 4, // FLT, FRT, NLT
			2, 6, 4, // FRT, NRT, NLT
			// Bottom
			5, 7, 1, // NLB, NRB, FLB,
			7, 3, 1 // NRB, FRB, FLB
		};

		// Create our index_array
		PackedByteArray data;
		data.resize(6 * 2 * 3 * 2);
		{
			uint8_t *w = data.ptrw();
			uint16_t *p16 = (uint16_t *)w;
			for (int i = 0; i < 6 * 2 * 3; i++) {
				*p16 = indices[i];
				p16++;
			}
		}

		frustum.index_buffer = RD::get_singleton()->index_buffer_create(6 * 2 * 3, RenderingDevice::INDEX_BUFFER_FORMAT_UINT16, data);
		frustum.index_array = RD::get_singleton()->index_array_create(frustum.index_buffer, 0, 6 * 2 * 3);
	}

	if (frustum.lines_buffer.is_null()) {
		uint16_t indices[12 * 2] = {
			0, 1, // FLT - FLB
			1, 3, // FLB - FRB
			3, 2, // FRB - FRT
			2, 0, // FRT - FLT

			4, 6, // NLT - NRT
			6, 7, // NRT - NRB
			7, 5, // NRB - NLB
			5, 4, // NLB - NLT

			0, 4, // FLT - NLT
			1, 5, // FLB - NLB
			2, 6, // FRT - NRT
			3, 7 // FRB - NRB
		};

		// Create our lines_array
		PackedByteArray data;
		data.resize(12 * 2 * 2);
		{
			uint8_t *w = data.ptrw();
			uint16_t *p16 = (uint16_t *)w;
			for (int i = 0; i < 12 * 2; i++) {
				*p16 = indices[i];
				p16++;
			}
		}

		frustum.lines_buffer = RD::get_singleton()->index_buffer_create(12 * 2, RenderingDevice::INDEX_BUFFER_FORMAT_UINT16, data);
		frustum.lines_array = RD::get_singleton()->index_array_create(frustum.lines_buffer, 0, 12 * 2);
	}
}

DebugEffects::~DebugEffects() {
	shadow_frustum.shader.version_free(shadow_frustum.shader_version);

	// Destroy vertex buffer and array.
	if (frustum.vertex_buffer.is_valid()) {
		RD::get_singleton()->free_rid(frustum.vertex_buffer); // Array gets freed as dependency.
	}

	// Destroy index buffer and array,
	if (frustum.index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(frustum.index_buffer); // Array gets freed as dependency.
	}

	// Destroy lines buffer and array.
	if (frustum.lines_buffer.is_valid()) {
		RD::get_singleton()->free_rid(frustum.lines_buffer); // Array gets freed as dependency.
	}

	motion_vectors.shader.version_free(motion_vectors.shader_version);
	nextgeo_rt_proxy.shader.version_free(nextgeo_rt_proxy.shader_version);

	// GodotNext: vsm_settings_debug (#221).
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (vsm_debug.params_buffer.is_valid()) {
			rd->free_rid(vsm_debug.params_buffer);
		}
		if (vsm_debug.page_info_buffer.is_valid()) {
			rd->free_rid(vsm_debug.page_info_buffer);
		}
		if (vsm_debug.empty_buffer.is_valid()) {
			rd->free_rid(vsm_debug.empty_buffer);
		}
	}
	if (vsm_debug.shader_version.is_valid()) {
		vsm_debug.shader.version_free(vsm_debug.shader_version);
	}
	if (vsm_debug.pages_version.is_valid()) {
		vsm_debug.pages_shader.version_free(vsm_debug.pages_version);
	}
}

void DebugEffects::draw_shadow_frustum(RID p_light, const Projection &p_cam_projection, const Transform3D &p_cam_transform, RID p_dest_fb, const Rect2 p_rect) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	RID base = light_storage->light_instance_get_base_light(p_light);
	ERR_FAIL_COND(light_storage->light_get_type(base) != RSE::LIGHT_DIRECTIONAL);

	// Make sure our buffers and arrays exist.
	_create_frustum_arrays();

	// Setup a points buffer for our view frustum.
	PackedByteArray points;
	points.resize(8 * sizeof(float) * 3);

	// Get info about our splits.
	RSE::LightDirectionalShadowMode shadow_mode = light_storage->light_directional_get_shadow_mode(base);
	bool overlap = light_storage->light_directional_get_blend_splits(base);
	int splits = 1;
	if (shadow_mode == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS) {
		splits = 4;
	} else if (shadow_mode == RSE::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
		splits = 2;
	}

	// Setup our camera info (this is mostly a duplicate of the logic found in RendererSceneCull::_light_instance_setup_directional_shadow).
	bool is_orthogonal = p_cam_projection.is_orthogonal();
	real_t aspect = p_cam_projection.get_aspect();
	real_t fov = 0.0;
	Vector2 vp_he;
	if (is_orthogonal) {
		vp_he = p_cam_projection.get_viewport_half_extents();
	} else {
		fov = p_cam_projection.get_fov(); //this is actually yfov, because set aspect tries to keep it
	}
	real_t min_distance = p_cam_projection.get_z_near();
	real_t max_distance = p_cam_projection.get_z_far();
	real_t shadow_max = RSG::light_storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
	if (shadow_max > 0 && !is_orthogonal) {
		max_distance = MIN(shadow_max, max_distance);
	}

	// Make sure we've not got bad info coming in.
	max_distance = MAX(max_distance, min_distance + 0.001);
	min_distance = MIN(min_distance, max_distance);
	real_t range = max_distance - min_distance;

	real_t distances[5];
	distances[0] = min_distance;
	for (int i = 0; i < splits; i++) {
		distances[i + 1] = min_distance + RSG::light_storage->light_get_param(base, RSE::LightParam(RSE::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET + i)) * range;
	};
	distances[splits] = max_distance;

	Color colors[4] = {
		Color(1.0, 0.0, 0.0, 0.1),
		Color(0.0, 1.0, 0.0, 0.1),
		Color(0.0, 0.0, 1.0, 0.1),
		Color(1.0, 1.0, 0.0, 0.1),
	};

	for (int split = 0; split < splits; split++) {
		// Load frustum points into vertex buffer.
		uint8_t *w = points.ptrw();
		Vector3 *vw = (Vector3 *)w;

		Projection projection;

		if (is_orthogonal) {
			projection.set_orthogonal(vp_he.y * 2.0, aspect, distances[(split == 0 || !overlap) ? split : split - 1], distances[split + 1], false);
		} else {
			projection.set_perspective(fov, aspect, distances[(split == 0 || !overlap) ? split : split - 1], distances[split + 1], true);
		}

		bool res = projection.get_endpoints(p_cam_transform, vw);
		ERR_CONTINUE(!res);

		RD::get_singleton()->buffer_update(frustum.vertex_buffer, 0, 8 * sizeof(float) * 3, w);

		// Get our light projection info.
		Projection light_projection = light_storage->light_instance_get_shadow_camera(p_light, split);
		Transform3D light_transform = light_storage->light_instance_get_shadow_transform(p_light, split);
		Rect2 atlas_rect_norm = light_storage->light_instance_get_directional_shadow_atlas_rect(p_light, split);

		if (!is_orthogonal) {
			light_transform.orthogonalize();
		}

		// Setup our push constant.
		ShadowFrustumPushConstant push_constant;
		MaterialStorage::store_camera(light_projection * Projection(light_transform.inverse()), push_constant.mvp);
		push_constant.color[0] = colors[split].r;
		push_constant.color[1] = colors[split].g;
		push_constant.color[2] = colors[split].b;
		push_constant.color[3] = colors[split].a;

		// Adjust our rect to our atlas position.
		Rect2 rect = p_rect;
		rect.position.x += atlas_rect_norm.position.x * rect.size.x;
		rect.position.y += atlas_rect_norm.position.y * rect.size.y;
		rect.size.x *= atlas_rect_norm.size.x;
		rect.size.y *= atlas_rect_norm.size.y;

		// And draw our frustum.
		RD::FramebufferFormatID fb_format_id = RD::get_singleton()->framebuffer_get_format(p_dest_fb);

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dest_fb, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0, rect);

		RID pipeline = shadow_frustum.pipelines[SFP_TRANSPARENT].get_render_pipeline(frustum.vertex_format, fb_format_id);
		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline);
		RD::get_singleton()->draw_list_bind_vertex_array(draw_list, frustum.vertex_array);
		RD::get_singleton()->draw_list_bind_index_array(draw_list, frustum.index_array);
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(ShadowFrustumPushConstant));
		RD::get_singleton()->draw_list_draw(draw_list, true);

		pipeline = shadow_frustum.pipelines[SFP_WIREFRAME].get_render_pipeline(frustum.vertex_format, fb_format_id);
		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline);
		RD::get_singleton()->draw_list_bind_vertex_array(draw_list, frustum.vertex_array);
		RD::get_singleton()->draw_list_bind_index_array(draw_list, frustum.lines_array);
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(ShadowFrustumPushConstant));
		RD::get_singleton()->draw_list_draw(draw_list, true);

		RD::get_singleton()->draw_list_end();

		if (split < (splits - 1) && splits > 1) {
			// Also draw it in the last split so we get a proper overview of the whole view frustum...

			// Get our light projection info.
			light_projection = light_storage->light_instance_get_shadow_camera(p_light, (splits - 1));
			light_transform = light_storage->light_instance_get_shadow_transform(p_light, (splits - 1));
			atlas_rect_norm = light_storage->light_instance_get_directional_shadow_atlas_rect(p_light, (splits - 1));

			if (!is_orthogonal) {
				light_transform.orthogonalize();
			}

			// Update our push constant.
			MaterialStorage::store_camera(light_projection * Projection(light_transform.inverse()), push_constant.mvp);
			push_constant.color[0] = colors[split].r;
			push_constant.color[1] = colors[split].g;
			push_constant.color[2] = colors[split].b;
			push_constant.color[3] = colors[split].a;

			// Adjust our rect to our atlas position.
			rect = p_rect;
			rect.position.x += atlas_rect_norm.position.x * rect.size.x;
			rect.position.y += atlas_rect_norm.position.y * rect.size.y;
			rect.size.x *= atlas_rect_norm.size.x;
			rect.size.y *= atlas_rect_norm.size.y;

			draw_list = RD::get_singleton()->draw_list_begin(p_dest_fb, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0, rect);

			pipeline = shadow_frustum.pipelines[SFP_TRANSPARENT].get_render_pipeline(frustum.vertex_format, fb_format_id);
			RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline);
			RD::get_singleton()->draw_list_bind_vertex_array(draw_list, frustum.vertex_array);
			RD::get_singleton()->draw_list_bind_index_array(draw_list, frustum.index_array);
			RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(ShadowFrustumPushConstant));
			RD::get_singleton()->draw_list_draw(draw_list, true);

			RD::get_singleton()->draw_list_end();
		}
	}
}

void DebugEffects::draw_motion_vectors(RID p_velocity, RID p_depth, RID p_dest_fb, const Projection &p_current_projection, const Transform3D &p_current_transform, const Projection &p_previous_projection, const Transform3D &p_previous_transform, Size2i p_resolution) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_source_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_velocity }));
	RD::Uniform u_source_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ default_sampler, p_depth }));

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dest_fb);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, motion_vectors.pipeline.get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_dest_fb), false, RD::get_singleton()->draw_list_get_current_pass()));

	Projection correction;
	correction.set_depth_correction(true, true, false);
	Projection reprojection = (correction * p_previous_projection) * p_previous_transform.affine_inverse() * p_current_transform * (correction * p_current_projection).inverse();
	RendererRD::MaterialStorage::store_camera(reprojection, motion_vectors.push_constant.reprojection_matrix);

	motion_vectors.push_constant.resolution[0] = p_resolution.width;
	motion_vectors.push_constant.resolution[1] = p_resolution.height;
	motion_vectors.push_constant.force_derive_from_depth = false;

	RID shader = motion_vectors.shader.version_get_shader(motion_vectors.shader_version, 0);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_source_velocity, u_source_depth), 0);
	RD::get_singleton()->draw_list_set_push_constant(draw_list, &motion_vectors.push_constant, sizeof(MotionVectorsPushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);

#ifdef DRAW_DERIVATION_FROM_DEPTH_ON_TOP
	motion_vectors.push_constant.force_derive_from_depth = true;

	RD::get_singleton()->draw_list_set_push_constant(draw_list, &motion_vectors.push_constant, sizeof(MotionVectorsPushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
#endif

	RD::get_singleton()->draw_list_end();
}

void DebugEffects::draw_nextgeo_rt_proxy(RID p_raster_depth, RID p_proxy_depth, RID p_dest_fb, const Projection &p_projection, Size2i p_internal_size, Size2i p_output_size) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	ERR_FAIL_COND(p_raster_depth.is_null() || p_proxy_depth.is_null() || p_dest_fb.is_null());

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_raster(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_raster_depth }));
	RD::Uniform u_proxy(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ default_sampler, p_proxy_depth }));

	NextGeoRTProxyPushConstant push;
	push.internal_size[0] = float(MAX(p_internal_size.width, 1));
	push.internal_size[1] = float(MAX(p_internal_size.height, 1));
	push.output_size[0] = float(MAX(p_output_size.width, 1));
	push.output_size[1] = float(MAX(p_output_size.height, 1));
	push.z_near = float(MAX(p_projection.get_z_near(), 1e-4));
	push.z_far = float(MAX(p_projection.get_z_far(), push.z_near + 1e-3f));
	push.is_orthogonal = p_projection.is_orthogonal() ? 1u : 0u;
	push.pad0 = 0u;

	RID shader = nextgeo_rt_proxy.shader.version_get_shader(nextgeo_rt_proxy.shader_version, 0);
	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dest_fb);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, nextgeo_rt_proxy.pipeline.get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_dest_fb), false, RD::get_singleton()->draw_list_get_current_pass()));
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_raster, u_proxy), 0);
	RD::get_singleton()->draw_list_set_push_constant(draw_list, &push, sizeof(NextGeoRTProxyPushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
	RD::get_singleton()->draw_list_end();
}

// GodotNext: vsm_settings_debug (#221, plan §21.3.2). The six virtual shadow map debug draws.

namespace {

// DebugParams in vsm_debug.glsl (std430), followed by the lights.
struct VSMDebugParamsHeader {
	float inv_projection[4][4];
	float view_to_world[3][4];
	uint32_t sizes[4];
	float footprint[4];
	int32_t bias[4];
	uint32_t misc[4];
	uint32_t misc2[4];
};
static_assert(sizeof(VSMDebugParamsHeader) == 192, "VSMDebugParamsHeader matches DebugParams in vsm_debug.glsl.");

} // namespace

bool DebugEffects::_ensure_vsm_debug() {
	if (vsm_debug.ready) {
		return true;
	}
	if (vsm_debug.failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	vsm_debug.failed = true; // Until every part exists.
	const String defines = RendererVirtualShadowMapStorage::get_glsl_defines();

	Vector<String> modes;
	modes.push_back("");
	vsm_debug.shader.initialize(modes, defines);
	vsm_debug.shader_version = vsm_debug.shader.version_create();
	const RID shader = vsm_debug.shader.version_get_shader(vsm_debug.shader_version, 0);
	ERR_FAIL_COND_V_MSG(shader.is_null(), false, "The virtual shadow map debug shader did not compile; the VSM debug draws stay off.");
	vsm_debug.pipeline.setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_blend(), 0);

	Vector<String> page_modes;
	page_modes.push_back("\n#define MODE_CLEAR\n");
	page_modes.push_back("\n#define MODE_DIRTY\n");
	page_modes.push_back("\n#define MODE_PAIRS\n");
	vsm_debug.pages_shader.initialize(page_modes, defines);
	vsm_debug.pages_version = vsm_debug.pages_shader.version_create();
	for (int mode = 0; mode < VSM_DEBUG_PAGES_MAX; mode++) {
		const RID pages_shader = vsm_debug.pages_shader.version_get_shader(vsm_debug.pages_version, mode);
		ERR_FAIL_COND_V_MSG(pages_shader.is_null(), false, "The virtual shadow map debug page shader did not compile; the VSM debug draws stay off.");
		vsm_debug.pages_pipelines[mode] = rd->compute_pipeline_create(pages_shader);
	}

	vsm_debug.params_buffer = rd->storage_buffer_create(sizeof(VSMDebugParamsHeader) + VSM_DEBUG_MAX_LIGHTS * sizeof(VSMDebugLight));
	rd->set_resource_name(vsm_debug.params_buffer, "VSM Debug Params");
	vsm_debug.params_lights = VSM_DEBUG_MAX_LIGHTS;
	vsm_debug.empty_buffer = rd->storage_buffer_create(16);
	rd->set_resource_name(vsm_debug.empty_buffer, "VSM Debug Empty");
	vsm_debug.failed = false;
	vsm_debug.ready = true;
	return true;
}

void DebugEffects::draw_vsm(const VSMDebugInput &p_input) {
	RenderingDevice *rd = RD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_NULL(material_storage);
	ERR_FAIL_NULL(uniform_set_cache);
	if (p_input.depth_texture.is_null() || p_input.page_table.is_null() || p_input.map_data.is_null() || p_input.pool.is_null() || p_input.dest_fb.is_null() || p_input.table_maps == 0 || p_input.pool_pages == 0) {
		return;
	}
	if (!_ensure_vsm_debug()) {
		return;
	}

	// The pages this view rendered and the caster draws into each (VSM_CACHE and VSM_OVERDRAW).
	const bool page_draw = (p_input.mode == 3 || p_input.mode == 4) && p_input.alloc_state.is_valid() && p_input.dirty.is_valid();
	if (page_draw) {
		if (vsm_debug.page_info_buffer.is_null() || vsm_debug.page_info_pages < p_input.pool_pages) {
			if (vsm_debug.page_info_buffer.is_valid()) {
				rd->free_rid(vsm_debug.page_info_buffer);
			}
			vsm_debug.page_info_buffer = rd->storage_buffer_create((p_input.pool_pages + 1) * uint32_t(sizeof(uint32_t)));
			rd->set_resource_name(vsm_debug.page_info_buffer, "VSM Debug Page Info");
			vsm_debug.page_info_pages = p_input.pool_pages;
		}
		const bool has_pairs = p_input.pairs.is_valid() && p_input.cull_state.is_valid() && p_input.pair_capacity > 0;
		VSMDebugPagesPushConstant push = {};
		push.pool_pages = p_input.pool_pages;
		push.grid_width = p_input.grid_width;
		push.pair_capacity = has_pairs ? p_input.pair_capacity : 0u;
		// A grid-stride loop: the pair list holds up to 1 << 22 pairs, more than 65,535 groups of 64.
		const uint32_t pair_groups = RendererVirtualShadowMapStorage::grid_stride_groups(push.pair_capacity, 64u, p_input.max_dispatch_groups);
		push.pair_stride = pair_groups * 64u;
		const RID pages_shader = vsm_debug.pages_shader.version_get_shader(vsm_debug.pages_version, VSM_DEBUG_PAGES_CLEAR);
		RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_input.alloc_state);
		RD::Uniform u_dirty(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_input.dirty);
		RD::Uniform u_pairs(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, has_pairs ? p_input.pairs : vsm_debug.empty_buffer);
		RD::Uniform u_cull(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, has_pairs ? p_input.cull_state : vsm_debug.empty_buffer);
		RD::Uniform u_info(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_debug.page_info_buffer);
		const RID set = uniform_set_cache->get_cache(pages_shader, 0, u_state, u_dirty, u_pairs, u_cull, u_info);

		rd->draw_command_begin_label("VSM Debug Pages");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, vsm_debug.pages_pipelines[VSM_DEBUG_PAGES_CLEAR]);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch_threads(list, p_input.pool_pages + 1, 1, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, vsm_debug.pages_pipelines[VSM_DEBUG_PAGES_DIRTY]);
		rd->compute_list_bind_uniform_set(list, set, 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch_threads(list, p_input.pool_pages, 1, 1);
		if (has_pairs) {
			rd->compute_list_add_barrier(list);
			rd->compute_list_bind_compute_pipeline(list, vsm_debug.pages_pipelines[VSM_DEBUG_PAGES_PAIRS]);
			rd->compute_list_bind_uniform_set(list, set, 0);
			rd->compute_list_set_push_constant(list, &push, sizeof(push));
			rd->compute_list_dispatch(list, pair_groups, 1, 1);
		}
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// The parameters and the lights.
	const uint32_t light_count = MIN(p_input.lights.size(), VSM_DEBUG_MAX_LIGHTS);
	Vector<uint8_t> data;
	data.resize(sizeof(VSMDebugParamsHeader) + MAX(light_count, 1u) * sizeof(VSMDebugLight));
	memset(data.ptrw(), 0, data.size());
	VSMDebugParamsHeader *header = reinterpret_cast<VSMDebugParamsHeader *>(data.ptrw());
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			header->inv_projection[row][column] = float(p_input.inv_projection.columns[column][row]);
		}
	}
	for (int row = 0; row < 3; row++) {
		header->view_to_world[row][0] = float(p_input.cam_transform.basis.rows[row].x);
		header->view_to_world[row][1] = float(p_input.cam_transform.basis.rows[row].y);
		header->view_to_world[row][2] = float(p_input.cam_transform.basis.rows[row].z);
		header->view_to_world[row][3] = float(p_input.cam_transform.origin[row]);
	}
	header->sizes[0] = uint32_t(MAX(p_input.internal_size.x, 1));
	header->sizes[1] = uint32_t(MAX(p_input.internal_size.y, 1));
	header->sizes[2] = uint32_t(MAX(p_input.output_size.x, 1));
	header->sizes[3] = uint32_t(MAX(p_input.output_size.y, 1));
	header->footprint[0] = p_input.pixel_scale;
	header->footprint[1] = p_input.pixel_bias;
	header->bias[0] = p_input.lod_bias_directional;
	header->bias[1] = p_input.lod_bias_local;
	header->misc[0] = p_input.table_maps;
	header->misc[1] = light_count;
	header->misc[2] = p_input.mode;
	header->misc[3] = MAX(p_input.grid_width, 1u);
	header->misc2[0] = p_input.pool_pages;
	header->misc2[1] = page_draw ? 1u : 0u;
	if (light_count > 0) {
		memcpy(data.ptrw() + sizeof(VSMDebugParamsHeader), p_input.lights.ptr(), light_count * sizeof(VSMDebugLight));
	}
	rd->buffer_update(vsm_debug.params_buffer, 0, data.size(), data.ptr());

	const RID shader = vsm_debug.shader.version_get_shader(vsm_debug.shader_version, 0);
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_input.depth_texture }));
	RD::Uniform u_table(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_input.page_table);
	RD::Uniform u_maps(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_input.map_data);
	RD::Uniform u_pool(RD::UNIFORM_TYPE_IMAGE, 3, p_input.pool);
	RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, vsm_debug.params_buffer);
	RD::Uniform u_info(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, vsm_debug.page_info_buffer.is_valid() ? vsm_debug.page_info_buffer : vsm_debug.empty_buffer);

	rd->draw_command_begin_label("VSM Debug Draw");
	RD::DrawListID draw_list = rd->draw_list_begin(p_input.dest_fb);
	rd->draw_list_bind_render_pipeline(draw_list, vsm_debug.pipeline.get_render_pipeline(RD::INVALID_ID, rd->framebuffer_get_format(p_input.dest_fb), false, rd->draw_list_get_current_pass()));
	rd->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_depth, u_table, u_maps, u_pool, u_params, u_info), 0);
	rd->draw_list_draw(draw_list, false, 1u, 3u);
	rd->draw_list_end();
	rd->draw_command_end_label();
}
