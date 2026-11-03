/**************************************************************************/
/*  test_blas_compact.cpp                                                 */
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

TEST_FORCE_LINK(test_blas_compact)

// BLAS compaction (#38) without a GPU. The feature mock has a null driver, so these cases
// check that RenderingDevice returns before the driver's ERR_FAIL defaults. Invalid-RID
// parameter checks that require a live driver run in godotnext/testbed/tests/blas_compact.gd.

#include "core/error/error_macros.h"

#ifdef RD_ENABLED
#include "servers/rendering/rendering_device.h"
#endif

namespace TestBlasCompact {

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
static const char *ERROR_COMPACTION_UNSUPPORTED = "BLAS compaction is not supported by this rendering driver.";
static const char *ERROR_DRIVER_UNIMPLEMENTED = "Acceleration structure compaction is not implemented by this driver.";
#endif

} // namespace TestBlasCompact

using namespace TestBlasCompact;

#ifdef RD_ENABLED

TEST_CASE("[RenderingDevice][BLASCompact] compaction is unavailable without ray tracing") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	REQUIRE_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY));
	REQUIRE_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAYTRACING_PIPELINE));

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const Error err = rd->blas_compact(RID());
	ERR_PRINT_ON;

	CHECK_EQ(err, ERR_UNAVAILABLE);
	CHECK_EQ(errors.count, 1);
	CHECK_EQ(errors.last_message, String(ERROR_NO_RT_SUPPORT));
	CHECK_FALSE(errors.last_message.contains(ERROR_DRIVER_UNIMPLEMENTED));

	memdelete(rd);
}

TEST_CASE("[RenderingDevice][BLASCompact] a driverless ray-query device does not call the driver") {
	// The support check runs before the RID check, so an invalid RID on this mock is
	// ERR_UNAVAILABLE rather than ERR_INVALID_PARAMETER. The parameter checks need a driver
	// and are covered by the testbed fixture.
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_RAY_QUERY));
	REQUIRE(rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY));

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const Error err = rd->blas_compact(RID());
	ERR_PRINT_ON;

	CHECK_EQ(err, ERR_UNAVAILABLE);
	CHECK_EQ(errors.count, 1);
	CHECK_EQ(errors.last_message, String(ERROR_COMPACTION_UNSUPPORTED));
	CHECK_FALSE(errors.last_message.contains(ERROR_DRIVER_UNIMPLEMENTED));

	memdelete(rd);
}

TEST_CASE("[RenderingDevice][BLASCompact] invalid ids report no compacted structure") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);

	ErrorCapture errors;
	ERR_PRINT_OFF;
	const bool compacted = rd->blas_is_compacted(RID());
	const int compacted_errors = errors.count;
	errors.clear();
	const uint64_t size = rd->acceleration_structure_get_size_bytes(RID());
	const int size_errors = errors.count;
	const String size_message = errors.last_message;
	ERR_PRINT_ON;

	CHECK_FALSE(compacted);
	CHECK_EQ(compacted_errors, 0);
	CHECK_EQ(size, 0);
	CHECK_EQ(size_errors, 1);
	CHECK_EQ(size_message, String("Parameter \"acceleration_structure\" is null."));

	memdelete(rd);
}

#endif
