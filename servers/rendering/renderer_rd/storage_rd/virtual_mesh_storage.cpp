/**************************************************************************/
/*  virtual_mesh_storage.cpp                                              */
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

#include "virtual_mesh_storage.h"

#include "servers/rendering/rendering_device.h"

namespace RendererRD {

VirtualMeshStorage::VirtualMeshStorage() {
}

VirtualMeshStorage::~VirtualMeshStorage() {
	free_resources();
}

void VirtualMeshStorage::free_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (record_buffer.is_valid()) {
		rd->free_rid(record_buffer);
		record_buffer = RID();
	}
	if (mesh_record_buffer.is_valid()) {
		rd->free_rid(mesh_record_buffer);
		mesh_record_buffer = RID();
	}
	buffer_capacity = 0;
	mesh_buffer_capacity = 0;
}

void VirtualMeshStorage::_upload_records(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0) {
		return;
	}
	if (record_buffer.is_null() || buffer_capacity < record_capacity) {
		if (record_buffer.is_valid()) {
			rd->free_rid(record_buffer);
		}
		buffer_capacity = record_capacity;
		record_buffer = rd->storage_buffer_create(buffer_capacity * sizeof(Record));
	}
	ERR_FAIL_COND(record_buffer.is_null());
	ERR_FAIL_COND(p_first + p_count > record_capacity);
	rd->buffer_update(record_buffer, p_first * sizeof(Record), p_count * sizeof(Record), records.ptr() + p_first);
}

void VirtualMeshStorage::_upload_mesh_records(uint32_t p_first, uint32_t p_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || p_count == 0) {
		return;
	}
	const uint32_t required = uint32_t(mesh_records.size());
	if (mesh_record_buffer.is_null() || mesh_buffer_capacity < required) {
		if (mesh_record_buffer.is_valid()) {
			rd->free_rid(mesh_record_buffer);
		}
		mesh_buffer_capacity = required;
		mesh_record_buffer = rd->storage_buffer_create(mesh_buffer_capacity * sizeof(MeshRecord));
	}
	ERR_FAIL_COND(mesh_record_buffer.is_null());
	ERR_FAIL_COND(p_first + p_count > mesh_buffer_capacity);
	rd->buffer_update(mesh_record_buffer, p_first * sizeof(MeshRecord), p_count * sizeof(MeshRecord), mesh_records.ptr() + p_first);
}

} // namespace RendererRD
