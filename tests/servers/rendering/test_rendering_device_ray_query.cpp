/**************************************************************************/
/*  test_rendering_device_ray_query.cpp                                   */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_rendering_device_ray_query)

// Ray-query-only RenderingDevice API (#17, plan D5 and §9 RT API items 1, 4 and 5).
// None of these cases needs a GPU: the validation rules are checked directly, and the entry points
// run on a RenderingDevice without a driver whose device capabilities are mocked.

#include "core/error/error_macros.h"
#include "servers/rendering/rendering_device_ray_query.h"

#ifdef RD_ENABLED
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#endif

namespace TestRenderingDeviceRayQuery {

// Records the errors printed while it is alive.
struct ErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	ErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~ErrorCapture() {
		remove_error_handler(&handler);
	}

	void clear() {
		count = 0;
		last_message = String();
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		ErrorCapture *self = static_cast<ErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

#ifdef RD_ENABLED
static uint64_t feature_bit(RenderingDevice::Features p_feature) {
	return uint64_t(1) << p_feature;
}

static const char *ERROR_NO_RT_SUPPORT = "The current rendering device has neither raytracing pipeline nor ray query support.";
#endif

} // namespace TestRenderingDeviceRayQuery

using namespace TestRenderingDeviceRayQuery;

TEST_CASE("[RenderingDevice][RayQuery] hit_sbt_range 0 marks a ray-query-only TLAS instance") {
	CHECK(RDRayQuery::is_ray_query_only_instance(true, 0));
	CHECK_FALSE(RDRayQuery::is_ray_query_only_instance(true, (int64_t(1) << 32) | 0));
	CHECK_FALSE(RDRayQuery::is_ray_query_only_instance(true, (int64_t(4) << 32) | 7));
	// A placeholder instance without a BLAS uses no hit shaders either way.
	CHECK_FALSE(RDRayQuery::is_ray_query_only_instance(false, 0));
}

TEST_CASE("[RenderingDevice][RayQuery] Build inputs need the device address and build input usage bits") {
	// The usage bits storage_buffer_create() sets with and without the two creation bits.
	const BitField<RDD::BufferUsageBits> storage = RDD::BUFFER_USAGE_TRANSFER_FROM_BIT | RDD::BUFFER_USAGE_TRANSFER_TO_BIT | RDD::BUFFER_USAGE_STORAGE_BIT;
	BitField<RDD::BufferUsageBits> address_only = storage;
	address_only.set_flag(RDD::BUFFER_USAGE_DEVICE_ADDRESS_BIT);
	BitField<RDD::BufferUsageBits> build_input_only = storage;
	build_input_only.set_flag(RDD::BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	BitField<RDD::BufferUsageBits> both = address_only;
	both.set_flag(RDD::BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);

	CHECK_FALSE(RDRayQuery::has_build_input_usage(storage));
	CHECK_FALSE(RDRayQuery::has_build_input_usage(address_only));
	CHECK_FALSE(RDRayQuery::has_build_input_usage(build_input_only));
	CHECK(RDRayQuery::has_build_input_usage(both));

	// The error names both creation bits and the failing buffer.
	const String error = RDRayQuery::get_build_input_usage_error("index", 2);
	CHECK(error.contains("BUFFER_CREATION_DEVICE_ADDRESS_BIT"));
	CHECK(error.contains("BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT"));
	CHECK(error.contains("index buffer of geometry 2"));
}

TEST_CASE("[RenderingDevice][RayQuery] Storage-buffer index sources accept 16-bit and 32-bit formats") {
	CHECK(RDRayQuery::is_valid_index_format(RDD::INDEX_BUFFER_FORMAT_UINT16));
	CHECK(RDRayQuery::is_valid_index_format(RDD::INDEX_BUFFER_FORMAT_UINT32));
	CHECK_FALSE(RDRayQuery::is_valid_index_format(RDD::IndexBufferFormat(RDD::INDEX_BUFFER_FORMAT_UINT32 + 1)));
	CHECK_FALSE(RDRayQuery::is_valid_index_format(RDD::IndexBufferFormat(-1)));
}

TEST_CASE("[RenderingDevice][RayQuery] Acceleration structure bind rules for compute and raytracing lists") {
	SUBCASE("A built TLAS with hit SBT ranges binds to both lists") {
		CHECK_EQ(RDRayQuery::get_bind_status(false, false, false), RDRayQuery::BIND_OK);
		CHECK_EQ(RDRayQuery::get_bind_status(false, false, true), RDRayQuery::BIND_OK);
	}
	SUBCASE("An unbuilt or invalidated acceleration structure binds to neither list") {
		CHECK_EQ(RDRayQuery::get_bind_status(true, false, false), RDRayQuery::BIND_INVALIDATED);
		CHECK_EQ(RDRayQuery::get_bind_status(true, false, true), RDRayQuery::BIND_INVALIDATED);
		CHECK_EQ(RDRayQuery::get_bind_status(true, true, false), RDRayQuery::BIND_INVALIDATED);
		CHECK_EQ(String(RDRayQuery::get_bind_error(RDRayQuery::BIND_INVALIDATED)), String("Acceleration structure either has not been built yet, or has been invalidated by an operation and needs to be rebuilt."));
	}
	SUBCASE("A zero-SBT TLAS binds to a compute list only") {
		CHECK_EQ(RDRayQuery::get_bind_status(false, true, false), RDRayQuery::BIND_OK);
		CHECK_EQ(RDRayQuery::get_bind_status(false, true, true), RDRayQuery::BIND_RAY_QUERY_ONLY);
		CHECK(String(RDRayQuery::get_bind_error(RDRayQuery::BIND_RAY_QUERY_ONLY)).begins_with("TLAS built with hit_sbt_range 0 is ray-query only"));
	}
	CHECK(String(RDRayQuery::get_bind_error(RDRayQuery::BIND_OK)).is_empty());
}

TEST_CASE("[RenderingDevice][RayQuery] Acceleration structure UPDATE is rejected before any driver command") {
	SUBCASE("BUILD is accepted for an unbuilt structure and for a changed instance count") {
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, false, true, false, 0, 0), RDRayQuery::UPDATE_OK);
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, true, false, true, 2, 4), RDRayQuery::UPDATE_OK);
	}
	SUBCASE("A mode other than BUILD or UPDATE is rejected first") {
		CHECK_EQ(RDRayQuery::get_update_status(RDD::AccelerationStructureBuildMode(2), false, true, true, 1, 2), RDRayQuery::UPDATE_INVALID_MODE);
	}
	SUBCASE("Missing ALLOW_UPDATE is reported before an unbuilt structure") {
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, false, true, false, 0, 0), RDRayQuery::UPDATE_MISSING_ALLOW_UPDATE);
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, false, false, true, 1, 2), RDRayQuery::UPDATE_MISSING_ALLOW_UPDATE);
	}
	SUBCASE("UPDATE before any BUILD is rejected, including when the instance count would also change") {
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, true, true, false, 0, 0), RDRayQuery::UPDATE_NOT_BUILT);
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, true, true, true, 0, 1), RDRayQuery::UPDATE_NOT_BUILT);
	}
	SUBCASE("A top-level UPDATE cannot change the instance count") {
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, true, false, true, 2, 1), RDRayQuery::UPDATE_INSTANCE_COUNT_CHANGED);
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, true, false, false, 2, 1), RDRayQuery::UPDATE_OK);
	}
	SUBCASE("A matching UPDATE is accepted") {
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, true, false, true, 4, 4), RDRayQuery::UPDATE_OK);
		CHECK_EQ(RDRayQuery::get_update_status(RDD::ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, true, false, false, 0, 0), RDRayQuery::UPDATE_OK);
	}
	CHECK(String(RDRayQuery::get_update_error(RDRayQuery::UPDATE_OK)).is_empty());
	CHECK(String(RDRayQuery::get_update_error(RDRayQuery::UPDATE_MISSING_ALLOW_UPDATE)).contains("ALLOW_UPDATE"));
	CHECK(String(RDRayQuery::get_update_error(RDRayQuery::UPDATE_NOT_BUILT)).contains("BUILD"));
	CHECK(String(RDRayQuery::get_update_error(RDRayQuery::UPDATE_INSTANCE_COUNT_CHANGED)).contains("instance count"));
	CHECK(String(RDRayQuery::get_update_error(RDRayQuery::UPDATE_INVALID_MODE)).contains("BUILD or UPDATE"));
}

#ifdef RD_ENABLED

TEST_CASE("[RenderingDevice][RayQuery] RT pipeline entry points fail cleanly without SUPPORTS_RAYTRACING_PIPELINE") {
	// A ray-query-only device: before #17 both calls went on to the driver, whose Vulkan
	// raytracing_pipeline_create called a null vkCreateRayTracingPipelinesKHR.
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY));
	REQUIRE(rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY));
	REQUIRE_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE));

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const RID pipeline = rd->raytracing_pipeline_create({}, {}, {}, 1);
	const int pipeline_errors = errors.count;
	const String pipeline_message = errors.last_message;
	errors.clear();
	const RID hit_sbt = rd->hit_sbt_create(RID(), 1);
	const int hit_sbt_errors = errors.count;
	const String hit_sbt_message = errors.last_message;
	errors.clear();
	const RenderingDevice::RaytracingListID raytracing_list = rd->raytracing_list_begin();
	const String raytracing_list_message = errors.last_message;
	ERR_PRINT_ON;

	CHECK_FALSE(pipeline.is_valid());
	CHECK_EQ(pipeline_errors, 1);
	CHECK_EQ(pipeline_message, String(RDRayQuery::ERROR_NO_RAYTRACING_PIPELINE));

	CHECK_FALSE(hit_sbt.is_valid());
	CHECK_EQ(hit_sbt_errors, 1);
	CHECK_EQ(hit_sbt_message, String(RDRayQuery::ERROR_NO_RAYTRACING_PIPELINE));

	CHECK_EQ(raytracing_list, RenderingDevice::RaytracingListID(RenderingDevice::INVALID_ID));
	CHECK_EQ(raytracing_list_message, String(RDRayQuery::ERROR_NO_RAYTRACING_PIPELINE));

	memdelete(rd);
}

TEST_CASE("[RenderingDevice][RayQuery] A device without ray tracing keeps the existing error on every entry point") {
	// The Metal and D3D12 drivers report neither feature today.
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	REQUIRE_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY));
	REQUIRE_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE));

	RenderingDevice::AccelerationStructureGeometry geometry;
	geometry.vertex_count = 3;

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const RID blas = rd->blas_create(Span<RenderingDevice::AccelerationStructureGeometry>(&geometry, 1), 0);
	const String blas_message = errors.last_message;
	const RID tlas = rd->tlas_create(2, 0);
	const String tlas_message = errors.last_message;
	const RID pipeline = rd->raytracing_pipeline_create({}, {}, {}, 1);
	const String pipeline_message = errors.last_message;
	const RID hit_sbt = rd->hit_sbt_create(RID(), 1);
	const String hit_sbt_message = errors.last_message;
	const int error_count = errors.count;
	ERR_PRINT_ON;

	CHECK_FALSE(blas.is_valid());
	CHECK_EQ(blas_message, String(ERROR_NO_RT_SUPPORT));
	CHECK_FALSE(tlas.is_valid());
	CHECK_EQ(tlas_message, String(ERROR_NO_RT_SUPPORT));
	CHECK_FALSE(pipeline.is_valid());
	CHECK_EQ(pipeline_message, String(ERROR_NO_RT_SUPPORT));
	CHECK_FALSE(hit_sbt.is_valid());
	CHECK_EQ(hit_sbt_message, String(ERROR_NO_RT_SUPPORT));
	CHECK_EQ(error_count, 4);

	memdelete(rd);
}

TEST_CASE("[RenderingDevice][RayQuery] blas_create rejects a vertex source that is not a vertex or storage buffer") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY));

	RenderingDevice::AccelerationStructureGeometry geometry;
	geometry.vertex_count = 3;
	geometry.vertex_stride = 12;
	geometry.vertex_format = RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const RID blas = rd->blas_create(Span<RenderingDevice::AccelerationStructureGeometry>(&geometry, 1), 0);
	ERR_PRINT_ON;

	CHECK_FALSE(blas.is_valid());
	CHECK_EQ(errors.count, 1);
	CHECK_EQ(errors.last_message, String("The vertex buffer of geometry 0 must be a vertex buffer or a storage buffer."));

	memdelete(rd);
}

TEST_CASE("[RenderingDevice][RayQuery] A feature mock reports exactly the mocked features") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE));
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY));
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE));
	memdelete(rd);
}

TEST_CASE("[RenderingDevice][RayQuery] RDAccelerationStructureGeometry exposes index_format, UINT32 by default") {
	CHECK_EQ(RenderingDevice::AccelerationStructureGeometry().index_format, RenderingDevice::INDEX_BUFFER_FORMAT_UINT32);

	Ref<RDAccelerationStructureGeometry> geometry;
	geometry.instantiate();
	CHECK_EQ(int(geometry->get("index_format")), int(RenderingDevice::INDEX_BUFFER_FORMAT_UINT32));
	geometry->set("index_format", RenderingDevice::INDEX_BUFFER_FORMAT_UINT16);
	CHECK_EQ(geometry->get_index_format(), RenderingDevice::INDEX_BUFFER_FORMAT_UINT16);
}

#endif // RD_ENABLED
