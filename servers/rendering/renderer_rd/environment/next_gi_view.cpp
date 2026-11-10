/**************************************************************************/
/*  next_gi_view.cpp                                                      */
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

#include "next_gi_view.h"

#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

// GodotNext: nextgi_view_math

Projection NextGIView::depth_projection(const RenderSceneDataRD &p_scene_data) {
	return NextGIViewMath::depth_projection(p_scene_data.cam_projection, p_scene_data.flip_y, p_scene_data.taa_jitter);
}

Projection NextGIView::view_projection(const RenderSceneDataRD &p_scene_data) {
	return NextGIViewMath::view_projection(p_scene_data.cam_projection, p_scene_data.cam_transform, p_scene_data.flip_y, p_scene_data.taa_jitter);
}

Projection NextGIView::inv_view_projection(const RenderSceneDataRD &p_scene_data) {
	return view_projection(p_scene_data).inverse();
}

Projection NextGIView::prev_view_projection(const RenderSceneDataRD &p_scene_data) {
	// The history textures share this frame's orientation, so the current flip_y applies.
	return NextGIViewMath::view_projection(p_scene_data.prev_cam_projection, p_scene_data.prev_cam_transform, p_scene_data.flip_y, p_scene_data.prev_taa_jitter);
}

Projection NextGIView::inv_projection(const RenderSceneDataRD &p_scene_data) {
	return depth_projection(p_scene_data).inverse();
}
