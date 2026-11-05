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

namespace RendererRD {

// GodotNext: nextgeo_instance_records (#75)
// The RenderingDevice-backed record store. The CPU layout and scheduling live
// in RendererVirtualMeshStorage; this class owns the two storage buffers and
// uploads the dirty runs.
class VirtualMeshStorage : public RendererVirtualMeshStorage {
	RID record_buffer;
	RID mesh_record_buffer;
	uint32_t buffer_capacity = 0;
	uint32_t mesh_buffer_capacity = 0;

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override;
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override;

public:
	VirtualMeshStorage();
	virtual ~VirtualMeshStorage() override;

	RID get_record_buffer() const { return record_buffer; }
	RID get_mesh_record_buffer() const { return mesh_record_buffer; }

	void free_resources();
};

} // namespace RendererRD
