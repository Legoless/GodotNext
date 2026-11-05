/**************************************************************************/
/*  next_gi_sdf_stream.h                                                  */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_sdf_stream.glsl.gen.h"
#include "servers/rendering/renderer_rd/shader_rd.h"

// GodotNext: ps_mesh_sdf_atlas (#185)
// The GPU mip-request pass and its one-frame-later readback. dispatch()
// writes required_mip into the instance records, the async readback lands in
// _readback_complete(), and poll() applies the requests to the atlas with a
// 4 MiB per-frame budget.
class NextGISDFStream {
public:
	static constexpr uint64_t FRAME_BUDGET_BYTES = 4ull * 1024ull * 1024ull;

	void dispatch(RID p_instance_buffer, uint32_t p_count, const Vector3 &p_camera);
	void poll(NextGISDFAtlas &r_atlas, uint64_t p_frame);
	void free_resources();

private:
	bool _ensure_pipeline();
	void _readback_complete(const PackedByteArray &p_data);
	static void _readback_static(const PackedByteArray &p_data, uint64_t p_token);
	void _unregister_tokens();

	NextgiSdfStreamShaderRD shader;
	RID version;
	RID pipeline;
	RID uniform_buffer;
	RID uniform_set;
	bool pipeline_failed = false;

	Mutex mutex;
	Vector<uint8_t> readback_data;
	uint32_t readback_count = 0;
	bool readback_pending = false;
	bool readback_ready = false;
	LocalVector<uint64_t> pending_tokens;
};
