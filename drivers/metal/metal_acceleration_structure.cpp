/**************************************************************************/
/*  metal_acceleration_structure.cpp                                      */
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

// Primitive BLAS, indirect-instance TLAS, and the build commands for the Metal driver.

#include "drivers/metal/metal3_objects.h"
#include "drivers/metal/pixel_formats.h"

static_assert(sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor) == RenderingDeviceDriver::METAL_ACCELERATION_STRUCTURE_INSTANCE_SIZE);
static_assert((int)MTL::VertexFormatFloat3 == (int)MTL::AttributeFormatFloat3);
static_assert((uint32_t)MTL::AccelerationStructureInstanceOptionDisableTriangleCulling == (uint32_t)RenderingDeviceDriver::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
static_assert((uint32_t)MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise == (uint32_t)RenderingDeviceDriver::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT);
static_assert((uint32_t)MTL::AccelerationStructureInstanceOptionOpaque == (uint32_t)RenderingDeviceDriver::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT);
static_assert((uint32_t)MTL::AccelerationStructureInstanceOptionNonOpaque == (uint32_t)RenderingDeviceDriver::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT);

static MTL::AccelerationStructureUsage _metal_as_usage(BitField<RenderingDeviceDriver::AccelerationStructureFlagBits> p_flags) {
	// ExtendedLimits is never set. SPIRV-Cross emits intersection_query<instancing, triangle_data>
	// without the extended_limits tag, and a structure built with that usage would not be traversable.
	// ALLOW_COMPACTION has no Metal usage bit; compaction is a later issue.
	NS::UInteger usage = MTL::AccelerationStructureUsageNone;
	if (p_flags.has_flag(RenderingDeviceDriver::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT)) {
		usage |= MTL::AccelerationStructureUsageRefit;
	}
	if (p_flags.has_flag(RenderingDeviceDriver::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT)) {
		usage |= MTL::AccelerationStructureUsagePreferFastBuild;
	}
	if (p_flags.has_flag(RenderingDeviceDriver::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT)) {
		usage |= MTL::AccelerationStructureUsagePreferFastIntersection;
	}
	if (p_flags.has_flag(RenderingDeviceDriver::ACCELERATION_STRUCTURE_LOW_MEMORY_BIT)) {
		usage |= MTL::AccelerationStructureUsageMinimizeMemory;
	}
	return (MTL::AccelerationStructureUsage)usage;
}

static uint64_t _metal_as_reported_size(MTL::AccelerationStructure *p_as, NS::UInteger p_queried_size) {
	const NS::UInteger allocated = p_as->allocatedSize();
	return allocated != 0 ? allocated : (uint64_t)p_queried_size;
}

static uint64_t _metal_as_scratch_size(NS::UInteger p_scratch_size) {
	// A zero scratch size cannot back a buffer. A larger buffer is a valid scratch.
	return p_scratch_size != 0 ? (uint64_t)p_scratch_size : 256;
}

RDD::AccelerationStructureID RenderingDeviceDriverMetal::blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) {
	ERR_FAIL_COND_V_MSG(!has_feature(SUPPORTS_RAY_QUERY), AccelerationStructureID(), "Ray query is not supported by the Metal driver.");
	ERR_FAIL_COND_V_MSG(p_geometries.size() == 0, AccelerationStructureID(), "Acceleration structure has no geometries.");

	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> desc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	ERR_FAIL_COND_V_MSG(!desc, AccelerationStructureID(), "Failed to create a primitive acceleration structure descriptor.");
	desc->setUsage(_metal_as_usage(p_flags));

	LocalVector<NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor>> geometries;
	LocalVector<NS::Object *> geometry_objects;
	geometries.reserve(p_geometries.size());
	geometry_objects.reserve(p_geometries.size());

	for (uint32_t i = 0; i < p_geometries.size(); i++) {
		const AccelerationStructureGeometry &geometry = p_geometries[i];
		ERR_FAIL_COND_V_MSG(!geometry.vertex_buffer, AccelerationStructureID(), vformat("Acceleration structure geometry %d is missing a vertex buffer.", i));
		ERR_FAIL_COND_V_MSG(geometry.vertex_stride == 0, AccelerationStructureID(), vformat("Acceleration structure geometry %d has a zero vertex stride.", i));

		const MTL::VertexFormat vertex_format = pixel_formats->getMTLVertexFormat(geometry.vertex_format);
		ERR_FAIL_COND_V_MSG(vertex_format == MTL::VertexFormatInvalid, AccelerationStructureID(), vformat("Acceleration structure geometry %d has an unsupported vertex format.", i));

		BufferInfo *vertices = (BufferInfo *)geometry.vertex_buffer.id;
		ERR_FAIL_NULL_V_MSG(vertices->buffer.get(), AccelerationStructureID(), vformat("Acceleration structure geometry %d vertex buffer is empty.", i));

		NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geom = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
		ERR_FAIL_COND_V_MSG(!geom, AccelerationStructureID(), "Failed to create a triangle geometry descriptor.");
		geom->setVertexBuffer(vertices->buffer.get());
		geom->setVertexBufferOffset(geometry.vertex_offset);
		geom->setVertexStride(geometry.vertex_stride);
		geom->setVertexFormat((MTL::AttributeFormat)vertex_format);
		geom->setOpaque(geometry.flags.has_flag(ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT));
		geom->setAllowDuplicateIntersectionFunctionInvocation(!geometry.flags.has_flag(ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT));

		if (geometry.index_buffer) {
			ERR_FAIL_COND_V_MSG(geometry.index_count < 3 || (geometry.index_count % 3) != 0, AccelerationStructureID(), vformat("Acceleration structure geometry %d index count is not a multiple of 3.", i));
			BufferInfo *indices = (BufferInfo *)geometry.index_buffer.id;
			ERR_FAIL_NULL_V_MSG(indices->buffer.get(), AccelerationStructureID(), vformat("Acceleration structure geometry %d index buffer is empty.", i));
			geom->setIndexBuffer(indices->buffer.get());
			geom->setIndexBufferOffset(geometry.index_offset);
			geom->setIndexType(geometry.index_format == INDEX_BUFFER_FORMAT_UINT16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32);
			geom->setTriangleCount(geometry.index_count / 3);
		} else {
			ERR_FAIL_COND_V_MSG(geometry.vertex_count < 3 || (geometry.vertex_count % 3) != 0, AccelerationStructureID(), vformat("Acceleration structure geometry %d vertex count is not a multiple of 3.", i));
			geom->setTriangleCount(geometry.vertex_count / 3);
		}

		geometry_objects.push_back(geom.get());
		geometries.push_back(geom);
	}

	NS::SharedPtr<NS::Array> geometry_array = NS::TransferPtr(NS::Array::array(geometry_objects.ptr(), geometry_objects.size())->retain());
	ERR_FAIL_COND_V_MSG(!geometry_array, AccelerationStructureID(), "Failed to create the geometry descriptor array.");
	desc->setGeometryDescriptors(geometry_array.get());

	const MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(desc.get());
	ERR_FAIL_COND_V_MSG(sizes.accelerationStructureSize == 0, AccelerationStructureID(), "Metal reported an empty bottom-level acceleration structure.");

	MetalAccelerationStructure created = allocator->new_acceleration_structure(sizes.accelerationStructureSize);
	ERR_FAIL_COND_V_MSG(!created.structure, AccelerationStructureID(), "Failed to allocate a bottom-level acceleration structure.");

	AccelerationStructureInfo *info = VersatileResource::allocate<AccelerationStructureInfo>(resources_allocator);
	info->as = created.structure;
	info->allocation = created.allocation;
	created.allocation.invalidate();
	info->descriptor = desc;
	info->size_bytes = _metal_as_reported_size(info->as.get(), sizes.accelerationStructureSize);
	info->scratch_size = _metal_as_scratch_size(sizes.buildScratchBufferSize);
	info->top_level = false;
	return AccelerationStructureID(info);
}

RDD::AccelerationStructureID RenderingDeviceDriverMetal::tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) {
	ERR_FAIL_COND_V_MSG(!has_feature(SUPPORTS_RAY_QUERY), AccelerationStructureID(), "Ray query is not supported by the Metal driver.");
	ERR_FAIL_COND_V_MSG(p_max_instance_count == 0, AccelerationStructureID(), "Acceleration structure instance count must be greater than 0.");
	ERR_FAIL_COND_V_MSG(p_max_instance_count > METAL_MAX_ACCELERATION_STRUCTURE_INSTANCES, AccelerationStructureID(), "Acceleration structure instance count exceeds the device limit.");

	NS::SharedPtr<MTL::IndirectInstanceAccelerationStructureDescriptor> desc = NS::TransferPtr(MTL::IndirectInstanceAccelerationStructureDescriptor::alloc()->init());
	ERR_FAIL_COND_V_MSG(!desc, AccelerationStructureID(), "Failed to create an instance acceleration structure descriptor.");
	desc->setUsage(_metal_as_usage(p_flags));
	desc->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
	desc->setInstanceDescriptorStride(METAL_ACCELERATION_STRUCTURE_INSTANCE_SIZE);
	desc->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutColumnMajor);
	desc->setMaxInstanceCount(p_max_instance_count);

	const MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(desc.get());
	ERR_FAIL_COND_V_MSG(sizes.accelerationStructureSize == 0, AccelerationStructureID(), "Metal reported an empty top-level acceleration structure.");

	MetalAccelerationStructure created = allocator->new_acceleration_structure(sizes.accelerationStructureSize);
	ERR_FAIL_COND_V_MSG(!created.structure, AccelerationStructureID(), "Failed to allocate a top-level acceleration structure.");

	AccelerationStructureInfo *info = VersatileResource::allocate<AccelerationStructureInfo>(resources_allocator);
	info->as = created.structure;
	info->allocation = created.allocation;
	created.allocation.invalidate();
	info->descriptor = desc;
	info->size_bytes = _metal_as_reported_size(info->as.get(), sizes.accelerationStructureSize);
	info->scratch_size = _metal_as_scratch_size(sizes.buildScratchBufferSize);
	info->max_instance_count = p_max_instance_count;
	info->top_level = true;
	return AccelerationStructureID(info);
}

void RenderingDeviceDriverMetal::acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) {
	ERR_FAIL_NULL(r_driver_instance);

	MTL::IndirectAccelerationStructureInstanceDescriptor desc = {};
	const Transform3D &xform = p_instance.transform;
	for (int column = 0; column < 3; column++) {
		const Vector3 basis_column = xform.basis.get_column(column);
		desc.transformationMatrix.columns[column] = MTL::PackedFloat3(basis_column.x, basis_column.y, basis_column.z);
	}
	desc.transformationMatrix.columns[3] = MTL::PackedFloat3(xform.origin.x, xform.origin.y, xform.origin.z);

	// Metal's default front face is clockwise. Vulkan's, and therefore Godot's, is counter-clockwise.
	// The instance option forces counter-clockwise unless FLIP_FACING asks for the Metal default.
	// The numeric values match, so this is not a blind cast of the Godot bitfield.
	uint32_t options = 0;
	if (p_instance.flags.has_flag(ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT)) {
		options |= MTL::AccelerationStructureInstanceOptionDisableTriangleCulling;
	}
	if (!p_instance.flags.has_flag(ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT)) {
		options |= MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise;
	}
	if (p_instance.flags.has_flag(ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT)) {
		options |= MTL::AccelerationStructureInstanceOptionOpaque;
	}
	if (p_instance.flags.has_flag(ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT)) {
		options |= MTL::AccelerationStructureInstanceOptionNonOpaque;
	}
	desc.options = (MTL::AccelerationStructureInstanceOptions)options;
	desc.mask = p_instance.mask;
	desc.intersectionFunctionTableOffset = p_instance.hit_sbt_offset;
	desc.userID = p_instance.id;

	if (p_instance.blas) {
		const AccelerationStructureInfo *blas = (const AccelerationStructureInfo *)p_instance.blas.id;
		ERR_FAIL_NULL(blas);
		ERR_FAIL_NULL(blas->as.get());
		desc.accelerationStructureID = blas->as->gpuResourceID();
	} else {
		ERR_FAIL_NULL(dummy_blas.get());
		desc.accelerationStructureID = dummy_blas->gpuResourceID();
		desc.mask = 0;
	}

	memcpy(r_driver_instance, &desc, sizeof(desc));
}

void RenderingDeviceDriverMetal::acceleration_structure_free(AccelerationStructureID p_acceleration_structure) {
	if (!p_acceleration_structure) {
		return;
	}
	AccelerationStructureInfo *info = (AccelerationStructureInfo *)p_acceleration_structure.id;
	ERR_FAIL_NULL(info);

	info->as.reset();
	MetalAccelerationStructure resource;
	resource.allocation = info->allocation;
	info->allocation.invalidate();
	allocator->free_acceleration_structure(resource);
	VersatileResource::free(resources_allocator, info);
}

uint32_t RenderingDeviceDriverMetal::acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure) {
	const AccelerationStructureInfo *info = (const AccelerationStructureInfo *)p_acceleration_structure.id;
	ERR_FAIL_NULL_V(info, 0);
	ERR_FAIL_COND_V_MSG(info->scratch_size > UINT32_MAX, UINT32_MAX, "Acceleration structure scratch size does not fit in 32 bits.");
	return (uint32_t)info->scratch_size;
}

uint64_t RenderingDeviceDriverMetal::acceleration_structure_get_size_bytes(AccelerationStructureID p_acceleration_structure) {
	const AccelerationStructureInfo *info = (const AccelerationStructureInfo *)p_acceleration_structure.id;
	ERR_FAIL_NULL_V(info, 0);
	return info->size_bytes;
}

void RenderingDeviceDriverMetal::command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer) {
	AccelerationStructureInfo *info = (AccelerationStructureInfo *)p_acceleration_structure.id;
	BufferInfo *scratch = (BufferInfo *)p_scratch_buffer.id;
	ERR_FAIL_NULL(info);
	ERR_FAIL_NULL(scratch);
	ERR_FAIL_COND_MSG(info->top_level, "command_build_blas was given a top-level acceleration structure.");
	ERR_FAIL_NULL(info->as.get());
	ERR_FAIL_NULL(info->descriptor.get());
	ERR_FAIL_NULL(scratch->buffer.get());

	MTL3::MDCommandBuffer *cb = (MTL3::MDCommandBuffer *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb);
	cb->encode_acceleration_structure_build(info->as.get(), info->descriptor.get(), scratch->buffer.get(), false);
}

void RenderingDeviceDriverMetal::command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count) {
	AccelerationStructureInfo *info = (AccelerationStructureInfo *)p_acceleration_structure.id;
	BufferInfo *scratch = (BufferInfo *)p_scratch_buffer.id;
	BufferInfo *instances = (BufferInfo *)p_instance_buffer.id;
	ERR_FAIL_NULL(info);
	ERR_FAIL_NULL(scratch);
	ERR_FAIL_NULL(instances);
	ERR_FAIL_COND_MSG(!info->top_level, "command_build_tlas was given a bottom-level acceleration structure.");
	ERR_FAIL_COND_MSG(p_instance_count == 0, "Top-level acceleration structure build has no instances.");
	ERR_FAIL_COND_MSG(p_instance_count > info->max_instance_count, "Top-level acceleration structure build exceeds its instance capacity.");
	ERR_FAIL_NULL(info->as.get());
	ERR_FAIL_NULL(info->descriptor.get());
	ERR_FAIL_NULL(scratch->buffer.get());
	ERR_FAIL_NULL(instances->buffer.get());

	MTL3::MDCommandBuffer *cb = (MTL3::MDCommandBuffer *)p_cmd_buffer.id;
	ERR_FAIL_NULL(cb);

	// The instance-count slot is a 4-byte ring allocation. Allocate it before the encoder
	// opens so its heap is included in useHeaps.
	MTL::Buffer *count_buffer = nullptr;
	NS::UInteger count_offset = 0;
	ERR_FAIL_COND_MSG(!cb->write_instance_count(p_instance_count, count_buffer, count_offset), "Failed to allocate the acceleration structure instance count.");

	MTL::Buffer *instance_buffer = instances->buffer.get();
	NS::UInteger instance_offset = p_instance_offset;
	// instanceDescriptorBufferOffset must be a multiple of 64. The 72-byte instance
	// stride is 64-aligned only when the instance count so far is a multiple of 8.
	if ((p_instance_offset % 64) != 0) {
		const uint64_t instance_bytes = (uint64_t)p_instance_count * METAL_ACCELERATION_STRUCTURE_INSTANCE_SIZE;
		ERR_FAIL_COND_MSG(instance_bytes > UINT32_MAX, "Acceleration structure instance staging buffer is too large.");
		ERR_FAIL_NULL_MSG(instance_buffer->contents(), "Acceleration structure instance buffer is not CPU-visible.");
		MetalBuffer staging = allocator->new_buffer((NS::UInteger)instance_bytes, MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
		ERR_FAIL_COND_MSG(!staging.buffer, "Failed to allocate an aligned acceleration structure instance buffer.");
		memcpy(staging.buffer->contents(), (const uint8_t *)instance_buffer->contents() + p_instance_offset, (size_t)instance_bytes);
		instance_buffer = staging.buffer.get();
		instance_offset = 0;
		cb->defer_acceleration_structure_resource(staging);
	}

	auto *stored = static_cast<MTL::IndirectInstanceAccelerationStructureDescriptor *>(info->descriptor.get());
	NS::SharedPtr<MTL::IndirectInstanceAccelerationStructureDescriptor> build_desc = NS::TransferPtr(stored->copy());
	ERR_FAIL_COND_MSG(!build_desc, "Failed to copy the instance acceleration structure descriptor.");
	build_desc->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
	build_desc->setInstanceDescriptorStride(METAL_ACCELERATION_STRUCTURE_INSTANCE_SIZE);
	build_desc->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutColumnMajor);
	build_desc->setInstanceDescriptorBuffer(instance_buffer);
	build_desc->setInstanceDescriptorBufferOffset(instance_offset);
	build_desc->setInstanceCountBuffer(count_buffer);
	build_desc->setInstanceCountBufferOffset(count_offset);
	cb->defer_acceleration_structure_descriptor(build_desc);
	cb->encode_acceleration_structure_build(info->as.get(), build_desc.get(), scratch->buffer.get(), true);
}

bool RenderingDeviceDriverMetal::_create_dummy_blas() {
	ERR_FAIL_NULL_V(allocator, false);
	ERR_FAIL_NULL_V(device, false);

	const float positions[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	dummy_blas_vertices = allocator->new_buffer(sizeof(positions), MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
	if (!dummy_blas_vertices.buffer) {
		ERR_PRINT("Metal dummy BLAS vertex buffer allocation failed.");
		return false;
	}
	dummy_blas_vertices.buffer->setLabel(MTLSTR("GodotNext Dummy BLAS Vertices"));
	memcpy(dummy_blas_vertices.buffer->contents(), positions, sizeof(positions));

	NS::SharedPtr<MTL::AccelerationStructureTriangleGeometryDescriptor> geom = NS::TransferPtr(MTL::AccelerationStructureTriangleGeometryDescriptor::alloc()->init());
	NS::SharedPtr<MTL::PrimitiveAccelerationStructureDescriptor> desc = NS::TransferPtr(MTL::PrimitiveAccelerationStructureDescriptor::alloc()->init());
	if (!geom || !desc) {
		ERR_PRINT("Metal dummy BLAS descriptor allocation failed.");
		_destroy_dummy_blas();
		return false;
	}
	geom->setVertexBuffer(dummy_blas_vertices.buffer.get());
	geom->setVertexStride(sizeof(float) * 3);
	geom->setVertexFormat(MTL::AttributeFormatFloat3);
	geom->setTriangleCount(1);
	geom->setOpaque(true);
	geom->setAllowDuplicateIntersectionFunctionInvocation(true);
	NS::Object *geom_object = geom.get();
	NS::SharedPtr<NS::Array> geometry_array = NS::TransferPtr(NS::Array::array(&geom_object, 1)->retain());
	desc->setGeometryDescriptors(geometry_array.get());
	desc->setUsage(MTL::AccelerationStructureUsageNone);

	const MTL::AccelerationStructureSizes sizes = device->accelerationStructureSizes(desc.get());
	if (sizes.accelerationStructureSize == 0) {
		ERR_PRINT("Metal dummy BLAS size query returned an empty structure.");
		_destroy_dummy_blas();
		return false;
	}

	MetalAccelerationStructure created = allocator->new_acceleration_structure(sizes.accelerationStructureSize);
	if (!created.structure) {
		ERR_PRINT("Metal dummy BLAS allocation failed.");
		_destroy_dummy_blas();
		return false;
	}
	dummy_blas = created.structure;
	dummy_blas_allocation = created.allocation;
	created.allocation.invalidate();
	dummy_blas->setLabel(MTLSTR("GodotNext Dummy BLAS"));

	const NS::UInteger scratch_size = (NS::UInteger)_metal_as_scratch_size(sizes.buildScratchBufferSize);
	MetalBuffer scratch = allocator->new_buffer(scratch_size, MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked);
	if (!scratch.buffer) {
		ERR_PRINT("Metal dummy BLAS scratch allocation failed.");
		_destroy_dummy_blas();
		return false;
	}

	MTL::CommandQueue *queue = get_command_queue();
	NS::SharedPtr<MTL::CommandBuffer> command_buffer = queue != nullptr ? NS::RetainPtr(queue->commandBuffer()) : NS::SharedPtr<MTL::CommandBuffer>();
	NS::SharedPtr<MTL::AccelerationStructureCommandEncoder> enc = command_buffer ? NS::RetainPtr(command_buffer->accelerationStructureCommandEncoder()) : NS::SharedPtr<MTL::AccelerationStructureCommandEncoder>();
	if (!enc) {
		ERR_PRINT("Metal dummy BLAS command encoder allocation failed.");
		allocator->free_buffer(scratch);
		_destroy_dummy_blas();
		return false;
	}

	LocalVector<MTL::Heap *> heaps;
	allocator->get_heaps(heaps);
	if (!heaps.is_empty()) {
		enc->useHeaps(heaps.ptr(), heaps.size());
	}
	enc->buildAccelerationStructure(dummy_blas.get(), desc.get(), scratch.buffer.get(), 0);
	enc->endEncoding();
	command_buffer->commit();
	command_buffer->waitUntilCompleted();
	allocator->free_buffer(scratch);

	if (command_buffer->status() == MTL::CommandBufferStatusError) {
		NS::Error *error = command_buffer->error();
		if (error != nullptr && error->localizedDescription() != nullptr) {
			ERR_PRINT(vformat("Metal dummy BLAS build failed: %s", String(error->localizedDescription()->utf8String())));
		} else {
			ERR_PRINT("Metal dummy BLAS build failed.");
		}
		_destroy_dummy_blas();
		return false;
	}

	as_objects_ready = true;
	print_verbose(vformat("Metal: dummy BLAS allocatedSize=%d queriedSize=%d scratch=%d.", (int64_t)dummy_blas->allocatedSize(), (int64_t)sizes.accelerationStructureSize, (int64_t)sizes.buildScratchBufferSize));
	return true;
}

void RenderingDeviceDriverMetal::_destroy_dummy_blas() {
	dummy_blas.reset();
	as_objects_ready = false;
	if (allocator == nullptr) {
		return;
	}
	MetalAccelerationStructure resource;
	resource.allocation = dummy_blas_allocation;
	dummy_blas_allocation.invalidate();
	allocator->free_acceleration_structure(resource);
	allocator->free_buffer(dummy_blas_vertices);
}
