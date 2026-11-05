/**************************************************************************/
/*  virtual_mesh_storage.h                                                */
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

#include "servers/rendering/storage/virtual_mesh_storage.h"

namespace RendererDummy {

// GodotNext: nextgeo_instance_records (#75)
// Headless/dummy record store: the CPU contract stays testable, uploads are
// counted but never reach a device.
class VirtualMeshStorage : public RendererVirtualMeshStorage {
	int record_upload_calls = 0;

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		record_upload_calls++;
		(void)p_first;
		(void)p_count;
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	bool _resize_dag(uint32_t p_bytes) override {
		dag_bytes = p_bytes;
		return true;
	}
	void _upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		(void)p_size;
	}

	uint32_t dag_bytes = 0;

public:
	int get_record_upload_calls() const { return record_upload_calls; }
	uint32_t get_dag_bytes() const { return dag_bytes; }
};

} // namespace RendererDummy
