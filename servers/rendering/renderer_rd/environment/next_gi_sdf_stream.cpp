/**************************************************************************/
/*  next_gi_sdf_stream.cpp                                                */
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

#include "next_gi_sdf_stream.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_device.h"

namespace {
struct SDFStreamPushConstant {
	float camera[4];
	uint32_t count;
	uint32_t pad[3];
};
} // namespace

// Async readback callbacks need a plain Callable; the stream is not an Object,
// so tokens map callbacks back to it on the render thread.
static Mutex s_sdf_stream_registry_mutex;
static HashMap<uint64_t, NextGISDFStream *> s_sdf_stream_registry;
static uint64_t s_sdf_stream_next_token = 1;

bool NextGISDFStream::_ensure_pipeline() {
	if (pipeline_failed) {
		return false;
	}
	if (version.is_valid() && pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	Vector<String> defines;
	defines.push_back("");
	shader.initialize(defines);
	version = shader.version_create();
	if (!version.is_valid()) {
		pipeline_failed = true;
		return false;
	}
	pipeline = rd->compute_pipeline_create(shader.version_get_shader(version, 0));
	if (!pipeline.is_valid()) {
		shader.version_free(version);
		version = RID();
		pipeline_failed = true;
		return false;
	}
	return true;
}

void NextGISDFStream::dispatch(RID p_instance_buffer, uint32_t p_count, const Vector3 &p_camera) {
	if (p_count == 0 || p_instance_buffer.is_null()) {
		return;
	}
	{
		MutexLock lock(mutex);
		if (readback_pending) {
			return;
		}
	}
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || !_ensure_pipeline()) {
		return;
	}
	if (uniform_buffer != p_instance_buffer) {
		if (uniform_set.is_valid()) {
			rd->free_rid(uniform_set);
			uniform_set = RID();
		}
		RD::Uniform uniform;
		uniform.binding = 0;
		uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		uniform.append_id(p_instance_buffer);
		uniform_set = rd->uniform_set_create(Vector<RD::Uniform>({ uniform }), shader.version_get_shader(version, 0), 0);
		uniform_buffer = p_instance_buffer;
	}
	if (!uniform_set.is_valid()) {
		return;
	}

	SDFStreamPushConstant push_constant;
	push_constant.camera[0] = p_camera.x;
	push_constant.camera[1] = p_camera.y;
	push_constant.camera[2] = p_camera.z;
	push_constant.camera[3] = 0.0f;
	push_constant.count = p_count;
	push_constant.pad[0] = 0;
	push_constant.pad[1] = 0;
	push_constant.pad[2] = 0;

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(SDFStreamPushConstant));
	rd->compute_list_dispatch(compute_list, (p_count + 63) / 64, 1, 1);
	rd->compute_list_end();

	uint64_t token = 0;
	{
		MutexLock lock(mutex);
		readback_pending = true;
		readback_ready = false;
		readback_count = p_count;
		MutexLock registry_lock(s_sdf_stream_registry_mutex);
		token = s_sdf_stream_next_token++;
		s_sdf_stream_registry.insert(token, this);
		pending_tokens.push_back(token);
	}
	rd->buffer_get_data_async(p_instance_buffer, callable_mp_static(&NextGISDFStream::_readback_static).bind(token), 0, p_count * NextGISDFInstances::RECORD_BYTES);
}

void NextGISDFStream::_readback_static(const PackedByteArray &p_data, uint64_t p_token) {
	NextGISDFStream *stream = nullptr;
	{
		MutexLock registry_lock(s_sdf_stream_registry_mutex);
		NextGISDFStream **found = s_sdf_stream_registry.getptr(p_token);
		if (found != nullptr) {
			stream = *found;
			s_sdf_stream_registry.erase(p_token);
		}
	}
	if (stream != nullptr) {
		stream->_readback_complete(p_data);
	}
}

void NextGISDFStream::_readback_complete(const PackedByteArray &p_data) {
	MutexLock lock(mutex);
	readback_data = p_data;
	readback_pending = false;
	readback_ready = true;
}

void NextGISDFStream::poll(NextGISDFAtlas &r_atlas, uint64_t p_frame) {
	Vector<uint8_t> data;
	uint32_t count = 0;
	{
		MutexLock lock(mutex);
		if (!readback_ready) {
			return;
		}
		data = readback_data;
		count = readback_count;
		readback_ready = false;
	}
	const uint32_t record_bytes = NextGISDFInstances::RECORD_BYTES;
	if (count == 0 || data.size() < count * record_bytes) {
		return;
	}
	LocalVector<uint32_t> ids;
	LocalVector<uint32_t> mips;
	ids.resize(count);
	mips.resize(count);
	for (uint32_t index = 0; index < count; index++) {
		const uint8_t *record = data.ptr() + index * record_bytes;
		uint32_t sdf_id;
		uint32_t required_mip;
		memcpy(&sdf_id, record + 64, sizeof(uint32_t));
		memcpy(&required_mip, record + 76, sizeof(uint32_t));
		ids[index] = sdf_id;
		mips[index] = required_mip;
	}
	r_atlas.stream_mips(ids, mips, p_frame, FRAME_BUDGET_BYTES);
}

void NextGISDFStream::free_resources() {
	_unregister_tokens();
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (uniform_set.is_valid()) {
		rd->free_rid(uniform_set);
		uniform_set = RID();
	}
	uniform_buffer = RID();
	if (pipeline.is_valid()) {
		rd->free_rid(pipeline);
		pipeline = RID();
	}
	if (version.is_valid()) {
		shader.version_free(version);
		version = RID();
	}
}

void NextGISDFStream::_unregister_tokens() {
	MutexLock registry_lock(s_sdf_stream_registry_mutex);
	for (const uint64_t &token : pending_tokens) {
		s_sdf_stream_registry.erase(token);
	}
	pending_tokens.clear();
}
