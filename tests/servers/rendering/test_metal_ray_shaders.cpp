/**************************************************************************/
/*  test_metal_ray_shaders.cpp                                            */
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

TEST_FORCE_LINK(test_metal_ray_shaders)

#include "core/config/project_settings.h"
#include "servers/rendering/rendering_device_commons.h"
#include "servers/rendering/rendering_device_driver.h"
#include "servers/rendering/rendering_device_rt_pipeline.h"
#include "tests/servers/rendering/metal_msl_test_bridge.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif

#include <cstddef>

namespace TestMetalRayShaders {

TEST_CASE("[MetalRayShaders] Project setting rendering/raytracing/metal_ray_shaders exists and defaults to false") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	REQUIRE(ps->has_setting("rendering/raytracing/metal_ray_shaders"));
	const Variant val = ps->get_setting("rendering/raytracing/metal_ray_shaders");
	CHECK_EQ(val.get_type(), Variant::BOOL);
	CHECK_FALSE(bool(val));
}

TEST_CASE("[MetalRayShaders] Enablement gating across setting, Apple family, macOS version, and ray query") {
	using RDD = RenderingDeviceDriver;
	constexpr uint32_t APPLE7 = 1007;
	constexpr uint32_t APPLE8 = 1008;
	constexpr uint32_t APPLE9 = 1009;
	constexpr uint32_t APPLE10 = 1010;
	constexpr uint32_t MACOS_15_0 = 150000;
	constexpr uint32_t MACOS_26_3 = 260300;
	constexpr uint32_t MACOS_26_4 = 260400;
	constexpr uint32_t MACOS_27_0 = 270000;

	SUBCASE("Setting off disables RT pipelines with SETTING_OFF even on Apple9+ macOS 26.4+") {
		const RDD::MetalRaytracingPipelineCaps caps = RDD::metal_raytracing_pipeline_caps(false, APPLE9, true, MACOS_26_4, true);
		CHECK_FALSE(caps.pipeline);
		CHECK_FALSE(caps.library);
		CHECK_FALSE(caps.trace_rays_indirect);
		CHECK_EQ(caps.reason, RDD::METAL_RT_PIPELINE_REASON_SETTING_OFF);
		CHECK_EQ(String(RDD::metal_raytracing_pipeline_reason_name(caps.reason)), "SETTING_OFF");
		CHECK(RDD::metal_raytracing_pipeline_log_line(caps).contains("SETTING_OFF"));

		// Setting off takes precedence on older hardware/OS as well.
		const RDD::MetalRaytracingPipelineCaps caps_old = RDD::metal_raytracing_pipeline_caps(false, APPLE8, true, MACOS_15_0, true);
		CHECK_FALSE(caps_old.pipeline);
		CHECK_EQ(caps_old.reason, RDD::METAL_RT_PIPELINE_REASON_SETTING_OFF);
	}

	SUBCASE("Setting on with Apple7 or Apple8 disables RT pipelines with APPLE_FAMILY_TOO_OLD") {
		const RDD::MetalRaytracingPipelineCaps caps8 = RDD::metal_raytracing_pipeline_caps(true, APPLE8, true, MACOS_26_4, true);
		CHECK_FALSE(caps8.pipeline);
		CHECK_FALSE(caps8.library);
		CHECK_FALSE(caps8.trace_rays_indirect);
		CHECK_EQ(caps8.reason, RDD::METAL_RT_PIPELINE_REASON_APPLE_FAMILY_TOO_OLD);
		CHECK_EQ(String(RDD::metal_raytracing_pipeline_reason_name(caps8.reason)), "APPLE_FAMILY_TOO_OLD");
		CHECK(RDD::metal_raytracing_pipeline_log_line(caps8).contains("APPLE_FAMILY_TOO_OLD"));

		const RDD::MetalRaytracingPipelineCaps caps7 = RDD::metal_raytracing_pipeline_caps(true, APPLE7, true, MACOS_26_4, false);
		CHECK_FALSE(caps7.pipeline);
		CHECK_EQ(caps7.reason, RDD::METAL_RT_PIPELINE_REASON_APPLE_FAMILY_TOO_OLD);

		// Non-macOS Apple9 is also gated out.
		const RDD::MetalRaytracingPipelineCaps caps_ios = RDD::metal_raytracing_pipeline_caps(true, APPLE9, false, MACOS_26_4, true);
		CHECK_FALSE(caps_ios.pipeline);
		CHECK_EQ(caps_ios.reason, RDD::METAL_RT_PIPELINE_REASON_APPLE_FAMILY_TOO_OLD);
	}

	SUBCASE("Setting on with Apple9+ and macOS < 26.4 disables RT pipelines with METAL_OS_TOO_OLD") {
		const RDD::MetalRaytracingPipelineCaps caps15 = RDD::metal_raytracing_pipeline_caps(true, APPLE9, true, MACOS_15_0, true);
		CHECK_FALSE(caps15.pipeline);
		CHECK_FALSE(caps15.library);
		CHECK_FALSE(caps15.trace_rays_indirect);
		CHECK_EQ(caps15.reason, RDD::METAL_RT_PIPELINE_REASON_METAL_OS_TOO_OLD);
		CHECK_EQ(String(RDD::metal_raytracing_pipeline_reason_name(caps15.reason)), "METAL_OS_TOO_OLD");
		CHECK(RDD::metal_raytracing_pipeline_log_line(caps15).contains("METAL_OS_TOO_OLD"));

		const RDD::MetalRaytracingPipelineCaps caps263 = RDD::metal_raytracing_pipeline_caps(true, APPLE9, true, MACOS_26_3, true);
		CHECK_FALSE(caps263.pipeline);
		CHECK_EQ(caps263.reason, RDD::METAL_RT_PIPELINE_REASON_METAL_OS_TOO_OLD);

		const RDD::MetalRaytracingPipelineCaps caps10_263 = RDD::metal_raytracing_pipeline_caps(true, APPLE10, true, 260399, true);
		CHECK_FALSE(caps10_263.pipeline);
		CHECK_EQ(caps10_263.reason, RDD::METAL_RT_PIPELINE_REASON_METAL_OS_TOO_OLD);
	}

	SUBCASE("Setting on with Apple9+ and macOS >= 26.4 requires ray query substrate") {
		const RDD::MetalRaytracingPipelineCaps caps_no_rq = RDD::metal_raytracing_pipeline_caps(true, APPLE9, true, MACOS_26_4, false);
		CHECK_FALSE(caps_no_rq.pipeline);
		CHECK_EQ(caps_no_rq.reason, RDD::METAL_RT_PIPELINE_REASON_NO_RAY_QUERY);
		CHECK(RDD::metal_raytracing_pipeline_log_line(caps_no_rq).contains("NO_RAY_QUERY"));
	}

	SUBCASE("Setting on with Apple9+ and macOS >= 26.4 and ray query enables RT pipelines, libraries, and indirect trace") {
		const RDD::MetalRaytracingPipelineCaps caps9 = RDD::metal_raytracing_pipeline_caps(true, APPLE9, true, MACOS_26_4, true);
		CHECK(caps9.pipeline);
		CHECK(caps9.library);
		CHECK(caps9.trace_rays_indirect);
		CHECK_EQ(caps9.reason, RDD::METAL_RT_PIPELINE_SUPPORTED);
		CHECK_EQ(String(RDD::metal_raytracing_pipeline_reason_name(caps9.reason)), "SUPPORTED");
		CHECK(RDD::metal_raytracing_pipeline_log_line(caps9).contains("enabled"));

		const RDD::MetalRaytracingPipelineCaps caps10 = RDD::metal_raytracing_pipeline_caps(true, APPLE10, true, MACOS_27_0, true);
		CHECK(caps10.pipeline);
		CHECK(caps10.library);
		CHECK(caps10.trace_rays_indirect);
		CHECK_EQ(caps10.reason, RDD::METAL_RT_PIPELINE_SUPPORTED);
	}
}

TEST_CASE("[MetalRayShaders] SBT handle, params struct layout, raygen index, and threadgroup dispatch math") {
	using RDD = RenderingDeviceDriver;

	CHECK_EQ(sizeof(RDD::MetalRTShaderGroupHandle), RDD::METAL_RT_SHADER_GROUP_HANDLE_SIZE);
	CHECK_EQ(sizeof(RDD::MetalRTSBTParams), 208u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, launch_size_x), 0u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, handle_size), 12u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, miss_sbt_address), 16u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, hit_sbt_address), 32u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, indirect_launch_address), 48u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, instance_sbt_offsets_address), 56u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, const_sets), 64u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, device_sets), 128u);
	CHECK_EQ(offsetof(RDD::MetalRTSBTParams, push_constants_address), 192u);

	CHECK_EQ(RDD::metal_rt_raygen_index_from_sbt(0, 64), 0u);
	CHECK_EQ(RDD::metal_rt_raygen_index_from_sbt(128, 64), 2u);
	CHECK_EQ(RDD::metal_rt_raygen_index_from_sbt(64, 0), 0u);

	uint32_t gx = 0;
	uint32_t gy = 0;
	uint32_t gz = 0;
	RDD::metal_rt_compute_threadgroups(256, 256, 1, 8, 8, 1, gx, gy, gz);
	CHECK_EQ(gx, 32u);
	CHECK_EQ(gy, 32u);
	CHECK_EQ(gz, 1u);

	RDD::metal_rt_compute_threadgroups(257, 9, 3, 8, 8, 1, gx, gy, gz);
	CHECK_EQ(gx, 33u);
	CHECK_EQ(gy, 2u);
	CHECK_EQ(gz, 3u);

	RDD::metal_rt_compute_threadgroups(0, 16, 1, 8, 8, 1, gx, gy, gz);
	CHECK_EQ(gx, 0u);
	CHECK_EQ(gy, 2u);
	CHECK_EQ(gz, 1u);

	CHECK_EQ(RDRTPipeline::get_hit_group_stride(RDD::METAL_RT_SHADER_GROUP_HANDLE_SIZE, RDD::METAL_RT_SHADER_GROUP_HANDLE_ALIGNMENT, 0), 16u);
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(RDD::METAL_RT_SHADER_GROUP_HANDLE_SIZE, RDD::METAL_RT_SHADER_GROUP_HANDLE_ALIGNMENT, 8), 32u);
	CHECK_EQ(RDRTPipeline::get_hit_group_stride(RDD::METAL_RT_SHADER_GROUP_HANDLE_SIZE, RDD::METAL_RT_SHADER_GROUP_HANDLE_ALIGNMENT, 32), 48u);

	// SBT-slot-indexed intersection function resolution (#134: 256+ materials > 18 hit groups).
	constexpr uint32_t HIT_GROUPS = 18;
	constexpr uint32_t SBT_SLOTS = 268;
	constexpr uint32_t STRIDE = 32; // 16-byte handle + 16-byte shader record.
	Vector<uint8_t> sbt;
	sbt.resize_initialized(SBT_SLOTS * STRIDE);
	for (uint32_t s = 0; s < SBT_SLOTS; s++) {
		RDD::MetalRTShaderGroupHandle h;
		h.group_type = RDD::MetalRTShaderGroupHandle::TYPE_HIT;
		h.visible_function_index = s % HIT_GROUPS;
		// Slots 200..255 use any-hit group 5; slot 260 uses opaque (INVALID_FUNCTION_INDEX).
		h.intersection_function_index = (s >= 200 && s < 256) ? 5u : RDD::MetalRTShaderGroupHandle::INVALID_FUNCTION_INDEX;
		memcpy(sbt.ptrw() + s * STRIDE, &h, sizeof(h));
	}
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, 0, HIT_GROUPS), RDD::MetalRTShaderGroupHandle::INVALID_FUNCTION_INDEX);
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, 200, HIT_GROUPS), 5u);
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, 255, HIT_GROUPS), 5u);
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, 260, HIT_GROUPS), RDD::MetalRTShaderGroupHandle::INVALID_FUNCTION_INDEX);
	// Out-of-bounds slot and non-zero offset checks.
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, SBT_SLOTS, HIT_GROUPS), RDD::MetalRTShaderGroupHandle::INVALID_FUNCTION_INDEX);
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 200 * STRIDE, STRIDE, 0, HIT_GROUPS), 5u);
	// Non-hit handle type and out-of-range hit-group index resolve to invalid (opaque fallback).
	RDD::MetalRTShaderGroupHandle bad_type;
	bad_type.group_type = RDD::MetalRTShaderGroupHandle::TYPE_MISS;
	bad_type.intersection_function_index = 2u;
	memcpy(sbt.ptrw() + 10 * STRIDE, &bad_type, sizeof(bad_type));
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, 10, HIT_GROUPS), RDD::MetalRTShaderGroupHandle::INVALID_FUNCTION_INDEX);
	RDD::MetalRTShaderGroupHandle bad_idx;
	bad_idx.group_type = RDD::MetalRTShaderGroupHandle::TYPE_HIT;
	bad_idx.intersection_function_index = HIT_GROUPS;
	memcpy(sbt.ptrw() + 11 * STRIDE, &bad_idx, sizeof(bad_idx));
	CHECK_EQ(RDD::metal_rt_hit_sbt_intersection_function_index(sbt.ptr(), sbt.size(), 0, STRIDE, 11, HIT_GROUPS), RDD::MetalRTShaderGroupHandle::INVALID_FUNCTION_INDEX);
}

#ifdef MODULE_GLSLANG_ENABLED

static Vector<MetalMSLTestBridge::ResourceBinding> _build_rt_test_bindings() {
	Vector<MetalMSLTestBridge::ResourceBinding> bindings;

	// Set 0: binding 0 = accelerationStructureEXT tlas, binding 1 = Rays SSBO, binding 2 = Results SSBO.
	{
		MetalMSLTestBridge::ResourceBinding b;
		b.basetype = MetalMSLTestBridge::BASE_TYPE_ACCELERATION_STRUCTURE;
		b.desc_set = 0;
		b.binding = 0;
		b.count = 1;
		b.msl_buffer = 0;
		bindings.push_back(b);
	}
	{
		MetalMSLTestBridge::ResourceBinding b;
		b.basetype = MetalMSLTestBridge::BASE_TYPE_VOID;
		b.desc_set = 0;
		b.binding = 1;
		b.count = 1;
		b.msl_buffer = 1;
		bindings.push_back(b);
	}
	{
		MetalMSLTestBridge::ResourceBinding b;
		b.basetype = MetalMSLTestBridge::BASE_TYPE_VOID;
		b.desc_set = 0;
		b.binding = 2;
		b.count = 1;
		b.msl_buffer = 2;
		bindings.push_back(b);
	}
	// Set 1: binding 0 = bindless runtime array of texture2D alpha_textures[].
	{
		MetalMSLTestBridge::ResourceBinding b;
		b.basetype = MetalMSLTestBridge::BASE_TYPE_IMAGE;
		b.desc_set = 1;
		b.binding = 0;
		b.count = 0; // Runtime array.
		b.msl_texture = 0;
		bindings.push_back(b);
	}

	return bindings;
}

static String _compile_glsl_stage_to_msl(RenderingDeviceCommons::ShaderStage p_stage, const String &p_glsl, uint32_t *r_payload_size = nullptr) {
	String glsl_error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			p_stage,
			p_glsl,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_6,
			&glsl_error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed for RT stage:\n" + glsl_error).utf8().get_data());
		return String();
	}
	REQUIRE_EQ(spirv.size() % 4, 0);

	if (r_payload_size != nullptr) {
		*r_payload_size = RDRTPipeline::get_spirv_payload_size(spirv.ptr(), uint64_t(spirv.size()));
	}

	MetalMSLTestBridge::CompileOptions opts;
	opts.msl_major = 3;
	opts.msl_minor = 2;
	opts.argument_buffers = true;
	opts.device_address_space_sets = 1u << 1; // Set 1 is a bindless table (device address space).
	opts.rt_visible_function_table_index = RenderingDeviceDriver::METAL_RT_VISIBLE_FUNCTION_TABLE_BUFFER_INDEX;
	opts.rt_intersection_function_table_index = RenderingDeviceDriver::METAL_RT_INTERSECTION_FUNCTION_TABLE_BUFFER_INDEX;
	opts.rt_sbt_params_buffer_index = RenderingDeviceDriver::METAL_RT_SBT_PARAMS_BUFFER_INDEX;
	opts.has_push_constants = true;
	opts.push_constant_buffer_index = 2;

	String msl;
	String msl_error;
	const bool ok = MetalMSLTestBridge::compile_spirv_to_msl(
			reinterpret_cast<const uint32_t *>(spirv.ptr()),
			size_t(spirv.size() / 4),
			p_stage,
			opts,
			_build_rt_test_bindings(),
			msl,
			msl_error);
	if (!ok) {
		FAIL_CHECK(("SPIRV-Cross MSL compile failed:\n" + msl_error).utf8().get_data());
		return String();
	}
	return msl;
}

TEST_CASE("[MetalRayShaders] SPIRV-Cross compiles RayGeneration, Miss, AnyHit, and ClosestHit shaders to MSL") {
	const String common = R"GLSL(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) restrict readonly buffer Rays {
	vec4 data[];
} rays;
layout(set = 0, binding = 2, std430) restrict writeonly buffer Results {
	uvec4 data[];
} results;
layout(set = 1, binding = 0) uniform texture2D alpha_textures[];

layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint pad0;
	uint pad1;
} params;

uint alpha_at(uint slot, vec2 bary) {
	uvec2 texel = min(uvec2(bary * 16.0), uvec2(15u));
	return uint(texelFetch(alpha_textures[nonuniformEXT(slot)], ivec2(texel), 0).a * 255.0 + 0.5);
}
)GLSL";

	const String raygen_glsl = common + R"GLSL(
layout(location = 0) rayPayloadEXT uvec4 payload;

void main() {
	uint index = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
	vec4 origin = rays.data[index * 2u];
	vec4 direction = rays.data[index * 2u + 1u];
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0, origin.xyz, origin.w, direction.xyz, direction.w, 0);
	results.data[index] = payload;
}
)GLSL";

	const String miss_glsl = common + R"GLSL(
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	payload = uvec4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u);
}
)GLSL";

	const String record_glsl = R"GLSL(
layout(shaderRecordEXT, std430) buffer HitRecord {
	uint material;
	uint slot;
} record;
hitAttributeEXT vec2 bary;
)GLSL";

	const String any_hit_glsl = common + record_glsl + R"GLSL(
layout(constant_id = 0) const uint ALPHA_CUTOFF = 128u;

void main() {
	if (alpha_at(record.slot, bary) < ALPHA_CUTOFF) {
		ignoreIntersectionEXT;
	}
}
)GLSL";

	const String any_hit_terminate_glsl = common + record_glsl + R"GLSL(
void main() {
	if (alpha_at(record.slot, bary) >= 200u) {
		terminateRayEXT;
	}
}
)GLSL";

	const String closest_hit_glsl = common + record_glsl + R"GLSL(
layout(location = 0) rayPayloadInEXT uvec4 payload;

void main() {
	uint front = gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT ? 1u : 0u;
	payload = uvec4(uint(gl_InstanceID), uint(gl_PrimitiveID), floatBitsToUint(gl_HitTEXT), front | (record.material << 1u) | (record.slot << 8u) | (alpha_at(record.slot, bary) << 24u));
}
)GLSL";

	SUBCASE("RayGenerationKHR emits compute kernel with intersector, IFT, VFT, and thread payload pointer") {
		uint32_t payload_size = 0;
		const String msl = _compile_glsl_stage_to_msl(RenderingDeviceCommons::SHADER_STAGE_RAYGEN, raygen_glsl, &payload_size);
		REQUIRE_FALSE(msl.is_empty());
		CHECK_EQ(payload_size, 16u);
		CHECK(msl.contains("#include <metal_raytracing>"));
		CHECK(msl.contains("using namespace metal::raytracing;"));
		CHECK(msl.contains("kernel void main0("));
		CHECK(msl.contains("visible_function_table<spvRTVisibleFn> spvRTVisibleTable [[buffer(16)]]"));
		CHECK(msl.contains("intersection_function_table<triangle_data, instancing, world_space_data> spvRTIntersectionTable [[buffer(17)]]"));
		CHECK(msl.contains("constant spvRTSBTParams& spvRTParams [[buffer(18)]]"));
		CHECK(msl.contains("uint3 spvLaunchID [[thread_position_in_grid]]"));
		CHECK(msl.contains("if (any(spvLaunchID >= spvLaunchSize))"));
		CHECK(msl.contains("uint4 payload = {};"));
		CHECK(msl.contains("intersector<triangle_data, instancing, world_space_data> spvIntersector;"));
		CHECK(msl.contains("spvIntersector.intersect(spvRay,"));
		CHECK(msl.contains("spvRTIntersectionTable)"));
		CHECK(msl.contains("spvRTVisibleTable[spvMissHandle->visible_function_index](&payload, spvHitInfo, nullptr, spvRTParams);"));
		CHECK(msl.contains("spvRTVisibleTable[spvHitHandle->visible_function_index](&payload, spvHitInfo, spvHitEntry + spvRTParams.handle_size, spvRTParams);"));
	}

	SUBCASE("MissKHR emits canonical [[visible]] function with thread reference payload") {
		uint32_t payload_size = 0;
		const String msl = _compile_glsl_stage_to_msl(RenderingDeviceCommons::SHADER_STAGE_MISS, miss_glsl, &payload_size);
		REQUIRE_FALSE(msl.is_empty());
		CHECK_EQ(payload_size, 16u);
		CHECK(msl.contains("[[visible]] void main0(thread void* spvPayloadPtr, const thread spvRTHitInfo& spvHitInfo, const device void* spvRecordPtr, const constant spvRTSBTParams& spvRTParams)"));
		CHECK(msl.contains("thread uint4& payload = *static_cast<thread uint4*>(spvPayloadPtr);"));
		CHECK_FALSE(msl.contains("[[buffer("));
	}

	SUBCASE("AnyHitKHR emits triangle intersection function returning false on ignore and true on accept") {
		const String msl = _compile_glsl_stage_to_msl(RenderingDeviceCommons::SHADER_STAGE_ANY_HIT, any_hit_glsl);
		REQUIRE_FALSE(msl.is_empty());
		CHECK(msl.contains("[[intersection(triangle, triangle_data, instancing, world_space_data)]] bool main0("));
		CHECK(msl.contains("constant spvRTSBTParams& spvRTParams [[buffer(18)]]"));
		CHECK(msl.contains("float2 spvBary [[barycentric_coord]]"));
		CHECK(msl.contains("uint spvPrimitiveId [[primitive_id]]"));
		CHECK(msl.contains("uint spvInstanceId [[instance_id]]"));
		CHECK(msl.contains("uint spvUserInstanceId [[user_instance_id]]"));
		CHECK(msl.contains("bool spvFrontFacing [[front_facing]]"));
		CHECK(msl.contains("const device HitRecord& record = *static_cast<const device HitRecord*>(spvRecordPtr);"));
		CHECK(msl.contains("[[function_constant(0)]]"));
		CHECK(msl.contains("return false;"));
		CHECK(msl.contains("return true;"));
		CHECK_FALSE(msl.contains("ignoreIntersectionEXT"));

		const String msl_term = _compile_glsl_stage_to_msl(RenderingDeviceCommons::SHADER_STAGE_ANY_HIT, any_hit_terminate_glsl);
		REQUIRE_FALSE(msl_term.is_empty());
		CHECK(msl_term.contains("return true;"));
		CHECK_FALSE(msl_term.contains("terminateRayEXT"));
	}

	SUBCASE("ClosestHitKHR emits canonical [[visible]] function with payload reference, hit record, builtins, and bindless table") {
		uint32_t payload_size = 0;
		const String msl = _compile_glsl_stage_to_msl(RenderingDeviceCommons::SHADER_STAGE_CLOSEST_HIT, closest_hit_glsl, &payload_size);
		REQUIRE_FALSE(msl.is_empty());
		CHECK_EQ(payload_size, 16u);
		CHECK(msl.contains("[[visible]] void main0(thread void* spvPayloadPtr, const thread spvRTHitInfo& spvHitInfo, const device void* spvRecordPtr, const constant spvRTSBTParams& spvRTParams)"));
		CHECK(msl.contains("thread uint4& payload = *static_cast<thread uint4*>(spvPayloadPtr);"));
		CHECK(msl.contains("const device HitRecord& record = *static_cast<const device HitRecord*>(spvRecordPtr);"));
		CHECK(msl.contains("float2 bary = spvHitInfo.barycentrics;"));
		CHECK(msl.contains("spvHitInfo.hit_kind"));
		CHECK(msl.contains("spvHitInfo.instance_id"));
		CHECK(msl.contains("spvHitInfo.primitive_id"));
		CHECK(msl.contains("spvHitInfo.ray_tmax"));
		CHECK(msl.contains("spvRTParams.device_sets[1]"));
		CHECK_FALSE(msl.contains("[[buffer("));
	}

	SUBCASE("RayGenerationKHR forwards RT tables, SBT params, and launch builtins to helper functions calling traceRayEXT") {
		const String helper_raygen_glsl = common + R"GLSL(
layout(location = 0) rayPayloadEXT uvec4 payload;

void inner_trace(vec3 origin, vec3 dir) {
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 1, 0, origin, 0.001, dir, 50.0, 0);
}

void outer_helper() {
	uint idx = gl_LaunchIDEXT.x + gl_LaunchSizeEXT.x * gl_LaunchIDEXT.y;
	inner_trace(vec3(float(idx), 0.0, 0.0), vec3(0.0, 0.0, -1.0));
}

void main() {
	payload = uvec4(0u);
	outer_helper();
	results.data[gl_LaunchIDEXT.x] = payload;
}
)GLSL";
		uint32_t payload_size = 0;
		const String msl = _compile_glsl_stage_to_msl(RenderingDeviceCommons::SHADER_STAGE_RAYGEN, helper_raygen_glsl, &payload_size);
		REQUIRE_FALSE(msl.is_empty());
		CHECK_EQ(payload_size, 16u);
		CHECK_FALSE(msl.contains("unsupported-built-in-type"));
		CHECK(msl.contains("visible_function_table<spvRTVisibleFn> spvRTVisibleTable, intersection_function_table<triangle_data, instancing, world_space_data> spvRTIntersectionTable, constant spvRTSBTParams& spvRTParams, thread const uint3& spvRTLaunchID, thread const uint3& spvRTLaunchSize"));
		CHECK(msl.contains("spvHitInfo.launch_id = spvRTLaunchID;"));
		CHECK(msl.contains("spvHitInfo.launch_size = spvRTLaunchSize;"));
		CHECK(msl.contains("spvRTParams.instance_sbt_offsets[spvHit.instance_id * 18u + 14u]"));
	}
}

#endif // MODULE_GLSLANG_ENABLED

} // namespace TestMetalRayShaders
