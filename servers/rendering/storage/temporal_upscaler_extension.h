/**************************************************************************/
/*  temporal_upscaler_extension.h                                         */
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

#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/object/gdvirtual.gen.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "servers/rendering/storage/render_scene_buffers.h"

class TemporalUpscalerInputs : public RefCounted {
	GDCLASS(TemporalUpscalerInputs, RefCounted);

	RID color;
	RID depth;
	RID velocity;
	RID reactive;
	RID output;
	Size2i internal_size;
	Size2i max_internal_size;
	Size2i output_size;
	Vector2 motion_vector_scale;
	Vector2 jitter;
	Transform3D world_to_view;
	Projection view_to_clip;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	float delta_time = 0.0f;
	bool reset = false;
	uint32_t view_index = 0;
	Ref<RenderSceneBuffers> render_scene_buffers;

protected:
	static void _bind_methods();

public:
	RID get_color() const { return color; }
	void set_color(RID p_color) { color = p_color; }

	RID get_depth() const { return depth; }
	void set_depth(RID p_depth) { depth = p_depth; }

	RID get_velocity() const { return velocity; }
	void set_velocity(RID p_velocity) { velocity = p_velocity; }

	RID get_reactive() const { return reactive; }
	void set_reactive(RID p_reactive) { reactive = p_reactive; }

	RID get_output() const { return output; }
	void set_output(RID p_output) { output = p_output; }

	Size2i get_internal_size() const { return internal_size; }
	void set_internal_size(const Size2i &p_internal_size) { internal_size = p_internal_size; }

	Size2i get_max_internal_size() const { return max_internal_size; }
	void set_max_internal_size(const Size2i &p_max_internal_size) { max_internal_size = p_max_internal_size; }

	Size2i get_output_size() const { return output_size; }
	void set_output_size(const Size2i &p_output_size) { output_size = p_output_size; }

	Vector2 get_motion_vector_scale() const { return motion_vector_scale; }
	void set_motion_vector_scale(const Vector2 &p_motion_vector_scale) { motion_vector_scale = p_motion_vector_scale; }

	Vector2 get_jitter() const { return jitter; }
	void set_jitter(const Vector2 &p_jitter) { jitter = p_jitter; }

	Transform3D get_world_to_view() const { return world_to_view; }
	void set_world_to_view(const Transform3D &p_world_to_view) { world_to_view = p_world_to_view; }

	Projection get_view_to_clip() const { return view_to_clip; }
	void set_view_to_clip(const Projection &p_view_to_clip) { view_to_clip = p_view_to_clip; }

	float get_z_near() const { return z_near; }
	void set_z_near(float p_z_near) { z_near = p_z_near; }

	float get_z_far() const { return z_far; }
	void set_z_far(float p_z_far) { z_far = p_z_far; }

	float get_fovy() const { return fovy; }
	void set_fovy(float p_fovy) { fovy = p_fovy; }

	float get_delta_time() const { return delta_time; }
	void set_delta_time(float p_delta_time) { delta_time = p_delta_time; }

	bool get_reset() const { return reset; }
	void set_reset(bool p_reset) { reset = p_reset; }

	uint32_t get_view_index() const { return view_index; }
	void set_view_index(uint32_t p_view_index) { view_index = p_view_index; }

	// Identifies the viewport, for per-(viewport, view) history (§6.3).
	Ref<RenderSceneBuffers> get_render_scene_buffers() const { return render_scene_buffers; }
	void set_render_scene_buffers(const Ref<RenderSceneBuffers> &p_render_scene_buffers) { render_scene_buffers = p_render_scene_buffers; }
};

class TemporalUpscalerExtension : public RefCounted {
	GDCLASS(TemporalUpscalerExtension, RefCounted);

protected:
	static void _bind_methods();

	GDVIRTUAL0RC(String, _get_name)
	GDVIRTUAL0RC(bool, _is_available)
	GDVIRTUAL0RC(Vector2, _get_scale_range)
	GDVIRTUAL0RC(float, _get_mip_bias_offset)
	GDVIRTUAL2RC(int, _get_jitter_phase_count, Vector2i, Vector2i)
	GDVIRTUAL1(_process, Ref<TemporalUpscalerInputs>)
	GDVIRTUAL0(_reset_history)

public:
	String get_upscaler_name() const;
	bool is_available() const;
	Vector2 get_scale_range() const;
	float get_mip_bias_offset() const;
	uint32_t get_jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const;
	void process(const Ref<TemporalUpscalerInputs> &p_inputs);
	void reset_history();
};
