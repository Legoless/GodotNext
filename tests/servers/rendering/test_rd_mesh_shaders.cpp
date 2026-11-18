/**************************************************************************/
/*  test_rd_mesh_shaders.cpp                                              */
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

TEST_FORCE_LINK(test_rd_mesh_shaders)

// Task and mesh shader stages through RenderingDevice (#178, plan §21.1, GR-3) without a GPU: the appended stages
// and features, the named errors of a device without them, the render graph's mesh draws and their barrier stages,
// the per-driver capability rules (Metal from Apple7 with indirect draws from Apple9; Direct3D 12 with mesh shader
// tier 1 and the GodotNext Mesa NIR library 25.3.1-gn4 or later, with one log line), and ShaderRD's mesh setup with
// the shader baker's rule.
// godotnext/testbed/tests/rd_parity/meshlets.gd runs the draws.

#ifdef RD_ENABLED

#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_device_graph.h"
#include "servers/rendering/rendering_shader_container.h"

#include "modules/modules_enabled.gen.h" // For glslang.

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif
#ifdef TOOLS_ENABLED
#include "editor/export/shader_baker/shader_baker_export_plugin.h"
#endif
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#endif
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_shader_container_d3d12.h"
#endif

namespace TestRDMeshShaders {

using RDC = RenderingDeviceCommons;

// Records the errors printed while it is alive.
struct MeshErrorCapture {
	ErrorHandlerList handler;
	int count = 0;
	String last_message;

	MeshErrorCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}

	~MeshErrorCapture() {
		remove_error_handler(&handler);
	}

	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		MeshErrorCapture *self = static_cast<MeshErrorCapture *>(p_self);
		self->count++;
		self->last_message = String::utf8((p_message != nullptr && *p_message != 0) ? p_message : p_error);
	}
};

uint64_t feature_bit(RenderingDevice::Features p_feature) {
	return uint64_t(1) << p_feature;
}

const char *SHADER_RD_TASK = R"(#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

layout(local_size_x = 32) in;

void main() {
	EmitMeshTasksEXT(MESH_GROUPS, 1, 1);
}
)";

const char *SHADER_RD_MESH = R"(#version 460
#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

layout(local_size_x = 32) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

void main() {
	SetMeshOutputsEXT(0, 0);
}
)";

const char *SHADER_RD_FRAGMENT = R"(#version 450

#VERSION_DEFINES

layout(location = 0) out vec4 color;

void main() {
	color = vec4(1.0);
}
)";

// A ShaderRD set up the way glsl_builders.py sets up a file with #[task], #[mesh] and #[fragment] sections.
class MeshTestShaderRD : public ShaderRD {
public:
	explicit MeshTestShaderRD(bool p_task) {
		setup_mesh(p_task ? SHADER_RD_TASK : nullptr, SHADER_RD_MESH, SHADER_RD_FRAGMENT, "MeshTestShaderRD");
	}
};

} // namespace TestRDMeshShaders

using namespace TestRDMeshShaders;

TEST_CASE("[RD][MeshShader] The stages, stage bits and features are appended to the bound enums") {
	// After SHADER_STAGE_INTERSECTION (9) and SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT (26), so no bound value moves
	// (plan §21.1 C2).
	CHECK_EQ(int(RenderingDevice::SHADER_STAGE_INTERSECTION), 9);
	CHECK_EQ(int(RenderingDevice::SHADER_STAGE_TASK), 10);
	CHECK_EQ(int(RenderingDevice::SHADER_STAGE_MESH), 11);
	CHECK_EQ(int(RenderingDevice::SHADER_STAGE_MAX), 12);
	CHECK_EQ(int(RenderingDevice::SHADER_STAGE_TASK_BIT), 1 << 10);
	CHECK_EQ(int(RenderingDevice::SHADER_STAGE_MESH_BIT), 1 << 11);
	CHECK_EQ(int(RenderingDevice::SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT), 26);
	CHECK_EQ(int(RenderingDevice::SUPPORTS_MESH_SHADER), 27);
	CHECK_EQ(int(RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT), 28);
	CHECK_EQ(String(RDC::SHADER_STAGE_NAMES[RDC::SHADER_STAGE_TASK]), String("Task"));
	CHECK_EQ(String(RDC::SHADER_STAGE_NAMES[RDC::SHADER_STAGE_MESH]), String("Mesh"));
	// The render graph's barrier stages are Vulkan's VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT and _MESH_.
	CHECK_EQ(int(RDD::PIPELINE_STAGE_TASK_SHADER_BIT), 0x00080000);
	CHECK_EQ(int(RDD::PIPELINE_STAGE_MESH_SHADER_BIT), 0x00100000);

	bool valid = false;
	CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", "SHADER_STAGE_TASK", &valid), 10);
	CHECK(valid);
	CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", "SHADER_STAGE_MESH_BIT", &valid), 1 << 11);
	CHECK(valid);
	CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", "SUPPORTS_MESH_SHADER", &valid), 27);
	CHECK(valid);
	CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", "SUPPORTS_MESH_SHADER_INDIRECT", &valid), 28);
	CHECK(valid);
	CHECK(ClassDB::has_method("RenderingDevice", "draw_list_draw_mesh_tasks"));
	CHECK(ClassDB::has_method("RenderingDevice", "draw_list_draw_mesh_tasks_indirect"));
	CHECK(ClassDB::has_method("RenderingDevice", "draw_list_draw_mesh_tasks_indirect_count"));
	CHECK(ClassDB::has_property("RDShaderSource", "source_task"));
	CHECK(ClassDB::has_property("RDShaderSource", "source_mesh"));
	CHECK(ClassDB::has_property("RDShaderSPIRV", "bytecode_task"));
	CHECK(ClassDB::has_property("RDShaderSPIRV", "bytecode_mesh"));
	CHECK(ClassDB::has_property("RDShaderSPIRV", "compile_error_task"));
	CHECK(ClassDB::has_property("RDShaderSPIRV", "compile_error_mesh"));

	// The properties reach the task and mesh slots.
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set("source_mesh", "mesh code");
	CHECK_EQ(source->get_stage_source(RDC::SHADER_STAGE_MESH), String("mesh code"));
	CHECK(source->get_stage_source(RDC::SHADER_STAGE_TASK).is_empty());

	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_MESH_SHADER));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT));
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_MESH_SHADER));
	CHECK(rd->has_feature(RenderingDevice::SUPPORTS_MESH_SHADER));
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT));
	memdelete(rd);
}

TEST_CASE("[RD][MeshShader] RDShaderFile reads #[task] and #[mesh] sections") {
	Ref<RDShaderFile> file;
	file.instantiate();
	const String text = "#[task]\n#version 460\nvoid main() {}\n#[mesh]\n#version 460\nvoid main() {}\n#[fragment]\n#version 450\nvoid main() {}\n";
	// The sections parse; whether they compile depends on the glslang module, so only the base error is checked.
	file->parse_versions_from_text(text);
	CHECK(file->get_base_error().is_empty());
	const String twice = "#[mesh]\n#version 460\nvoid main() {}\n#[mesh]\n#version 460\nvoid main() {}\n";
	file->parse_versions_from_text(twice);
	CHECK_EQ(file->get_base_error(), String("Invalid shader file, stage appears twice: mesh"));
}

TEST_CASE("[RD][MeshShader] A device without SUPPORTS_MESH_SHADER refuses task and mesh shaders with one named error") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
	MeshErrorCapture errors;

	// The check comes before the SPIR-V is read or the driver is reached (this device has none).
	Vector<uint8_t> module;
	module.resize(32);
	module.fill(0);
	Vector<RDC::ShaderStageSPIRVData> stages;
	for (RDC::ShaderStage stage : { RDC::SHADER_STAGE_MESH, RDC::SHADER_STAGE_FRAGMENT }) {
		RDC::ShaderStageSPIRVData data;
		data.shader_stage = stage;
		data.spirv = module;
		stages.push_back(data);
	}

	SUBCASE("shader_compile_binary_from_spirv (the ShaderRD path)") {
		ERR_PRINT_OFF;
		const Vector<uint8_t> binary = rd->shader_compile_binary_from_spirv(stages, "mesh_raster");
		ERR_PRINT_ON;
		CHECK(binary.is_empty());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("Shader 'mesh_raster' has task or mesh shader stages, which this device does not support. Missing RenderingDevice feature: SUPPORTS_MESH_SHADER."));
	}
	SUBCASE("shader_create_from_spirv, a task stage alone, without a name") {
		Vector<RDC::ShaderStageSPIRVData> task_stages;
		RDC::ShaderStageSPIRVData task;
		task.shader_stage = RDC::SHADER_STAGE_TASK;
		task.spirv = module;
		task_stages.push_back(task);
		ERR_PRINT_OFF;
		const RID shader = rd->shader_create_from_spirv(task_stages);
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("A shader has task or mesh shader stages, which this device does not support. Missing RenderingDevice feature: SUPPORTS_MESH_SHADER."));
	}
	SUBCASE("The bound method reads RDShaderSPIRV.bytecode_mesh") {
		Ref<RDShaderSPIRV> spirv;
		spirv.instantiate();
		spirv->set("bytecode_mesh", module);
		spirv->set_stage_bytecode(RDC::SHADER_STAGE_FRAGMENT, module);
		ERR_PRINT_OFF;
		const RID shader = rd->call("shader_create_from_spirv", spirv, "bound");
		ERR_PRINT_ON;
		CHECK_FALSE(shader.is_valid());
		CHECK_EQ(errors.count, 1);
		CHECK(errors.last_message.begins_with("Shader 'bound' has task or mesh shader stages"));
	}
	memdelete(rd);
}

TEST_CASE("[RD][MeshShader] Mesh task draws on a device without the features give one named error") {
	RenderingDevice *rd = memnew(RenderingDevice);
	MeshErrorCapture errors;

	SUBCASE("Direct, without SUPPORTS_MESH_SHADER") {
		rd->set_feature_mock(true, 0);
		ERR_PRINT_OFF;
		rd->draw_list_draw_mesh_tasks(0, 4, 1, 1);
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("This device does not support mesh shaders (RenderingDevice feature SUPPORTS_MESH_SHADER)."));
	}
	SUBCASE("Indirect, with mesh shaders but without SUPPORTS_MESH_SHADER_INDIRECT (Apple7 and Apple8)") {
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_MESH_SHADER) | feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
		ERR_PRINT_OFF;
		rd->draw_list_draw_mesh_tasks_indirect(0, RID(), 0, 1, 0);
		rd->draw_list_draw_mesh_tasks_indirect_count(0, RID(), 0, RID(), 0, 8, 0);
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 2);
		CHECK_EQ(errors.last_message, String("This device does not support indirect mesh task draws (RenderingDevice feature SUPPORTS_MESH_SHADER_INDIRECT)."));
	}
	SUBCASE("Indirect count, without SUPPORTS_DRAW_INDIRECT_COUNT") {
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_MESH_SHADER) | feature_bit(RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT));
		ERR_PRINT_OFF;
		rd->draw_list_draw_mesh_tasks_indirect_count(0, RID(), 0, RID(), 0, 8, 0);
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 1);
		CHECK_EQ(errors.last_message, String("This device does not support indirect draws with a GPU count (RenderingDevice feature SUPPORTS_DRAW_INDIRECT_COUNT)."));
	}
	SUBCASE("With the features, the draw list is checked next") {
		rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_MESH_SHADER) | feature_bit(RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT) | feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
		ERR_PRINT_OFF;
		rd->draw_list_draw_mesh_tasks(0, 4, 1, 1);
		rd->draw_list_draw_mesh_tasks_indirect(0, RID(), 0, 1, 0);
		rd->draw_list_draw_mesh_tasks_indirect_count(0, RID(), 0, RID(), 0, 8, 0);
		ERR_PRINT_ON;
		CHECK_EQ(errors.count, 3);
		CHECK_FALSE(errors.last_message.contains("SUPPORTS_"));
	}
	memdelete(rd);
}

TEST_CASE("[RD][MeshShader] The render graph records mesh task draws, their indirect reads and the mesh stages") {
	RDG graph;
	RDG::ResourceTracker *meshlets = RDG::resource_tracker_create();
	RDG::ResourceTracker *records = RDG::resource_tracker_create();
	RDG::ResourceTracker *count = RDG::resource_tracker_create();
	meshlets->buffer_driver_id = RDD::BufferID(21);
	records->buffer_driver_id = RDD::BufferID(22);
	count->buffer_driver_id = RDD::BufferID(23);

	// A compute pass writes the meshlet data the task shader reads; command 0.
	graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, RDG::QUEUE_MAIN);
	graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	graph.add_compute_list_usage(meshlets, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	graph.add_compute_list_dispatch(1, 1, 1);
	graph.add_compute_list_end();

	// A compute pass writes the records and the count; command 1.
	graph.add_compute_list_begin(RDD::BreadcrumbMarker::NONE, 0, RDG::QUEUE_MAIN);
	graph.add_compute_list_bind_pipeline(RDD::PipelineID());
	graph.add_compute_list_usage(records, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	graph.add_compute_list_usage(count, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ_WRITE);
	graph.add_compute_list_dispatch(1, 1, 1);
	graph.add_compute_list_end();

	// The draw list with a mesh pipeline (the stage bits RenderingDevice gives a task, mesh and fragment shader);
	// command 2.
	const BitField<RDD::PipelineStageBits> mesh_pipeline_stages = RDD::PIPELINE_STAGE_TASK_SHADER_BIT | RDD::PIPELINE_STAGE_MESH_SHADER_BIT | RDD::PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(), VectorView<RDD::RenderPassClearValue>(), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	graph.add_draw_list_bind_pipeline(RDD::PipelineID(), mesh_pipeline_stages);
	graph.add_draw_list_usage(meshlets, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	graph.add_draw_list_draw_mesh_tasks(4, 2, 1);
	CHECK(graph.add_draw_list_draw_mesh_tasks_indirect(RDD::BufferID(22), records, 16, 3, 32));
	CHECK(graph.add_draw_list_draw_mesh_tasks_indirect_count(RDD::BufferID(22), records, 0, RDD::BufferID(23), count, 4, 64, 0));
	graph.add_draw_list_end();

	CHECK_EQ(meshlets->usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	CHECK_EQ(records->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);
	CHECK_EQ(count->usage, RDG::RESOURCE_USAGE_INDIRECT_BUFFER_READ);

	BitField<RDD::PipelineStageBits> self_stages;
	BitField<RDD::PipelineStageBits> previous_stages;
	BitField<RDD::PipelineStageBits> next_stages;
	graph.command_get_stages(2, self_stages, previous_stages, next_stages);
	CHECK(self_stages.has_flag(RDD::PIPELINE_STAGE_TASK_SHADER_BIT));
	CHECK(self_stages.has_flag(RDD::PIPELINE_STAGE_MESH_SHADER_BIT));
	CHECK(self_stages.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));
	CHECK(previous_stages.has_flag(RDD::PIPELINE_STAGE_COMPUTE_SHADER_BIT));
	// The barriers after both compute passes wait in the task and mesh stages (and the draw-indirect stage), so a
	// buffer only the task or mesh shader reads is synchronized.
	for (int32_t compute_command : { 0, 1 }) {
		graph.command_get_stages(compute_command, self_stages, previous_stages, next_stages);
		CHECK(next_stages.has_flag(RDD::PIPELINE_STAGE_TASK_SHADER_BIT));
		CHECK(next_stages.has_flag(RDD::PIPELINE_STAGE_MESH_SHADER_BIT));
		CHECK(next_stages.has_flag(RDD::PIPELINE_STAGE_DRAW_INDIRECT_BIT));
	}

	// The count draw goes through the drivers' count preparation, marked as a mesh task draw.
	const LocalVector<RDG::IndirectCountDraw> draws = graph.indirect_count_draws_for_test(2);
	REQUIRE_EQ(draws.size(), 1u);
	CHECK(draws[0].mesh_tasks);
	CHECK_FALSE(draws[0].indexed);
	CHECK_EQ(draws[0].buffer.id, 22u);
	CHECK_EQ(draws[0].count_buffer.id, 23u);
	CHECK_EQ(draws[0].count_buffer_offset, 4u);
	CHECK_EQ(draws[0].max_draw_count, 64u);

	RDG::resource_tracker_free(meshlets);
	RDG::resource_tracker_free(records);
	RDG::resource_tracker_free(count);
}

TEST_CASE("[RD][MeshShader] An indirect mesh task draw whose buffer the draw list also uses another way gives one named error") {
	RDG graph;
	RDG::ResourceTracker *records = RDG::resource_tracker_create();
	RDG::ResourceTracker *count = RDG::resource_tracker_create();
	records->buffer_driver_id = RDD::BufferID(31);
	count->buffer_driver_id = RDD::BufferID(32);
	MeshErrorCapture errors;

	graph.add_draw_list_begin(RDD::RenderPassID(), RDD::FramebufferID(), Rect2i(0, 0, 64, 64), VectorView<RDG::AttachmentOperation>(), VectorView<RDD::RenderPassClearValue>(), RDD::PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	graph.add_draw_list_usage(records, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	graph.add_draw_list_usage(count, RDG::RESOURCE_USAGE_UNIFORM_BUFFER_READ);
	ERR_PRINT_OFF;
	CHECK_FALSE(graph.add_draw_list_draw_mesh_tasks_indirect(RDD::BufferID(31), records, 0, 1, 0));
	ERR_PRINT_ON;
	CHECK_EQ(errors.count, 1);
	CHECK_EQ(errors.last_message, String("The indirect buffer of an indirect mesh task draw is also used in this draw list (Storage Buffer Read). A buffer has one usage per draw list, so it would not be synchronized for the indirect read; bind it in none of this draw list's uniform sets."));

	// The count variant shares #177's check and wording.
	RDG::ResourceTracker *other_records = RDG::resource_tracker_create();
	ERR_PRINT_OFF;
	CHECK_FALSE(graph.add_draw_list_draw_mesh_tasks_indirect_count(RDD::BufferID(33), other_records, 0, RDD::BufferID(32), count, 0, 4, 0));
	ERR_PRINT_ON;
	CHECK_EQ(errors.count, 2);
	CHECK(errors.last_message.begins_with("The count buffer of an indirect count draw is also used in this draw list (Uniform Buffer Read)."));
	graph.add_draw_list_end();

	// Nothing was recorded for the failed draws.
	CHECK_EQ(records->usage, RDG::RESOURCE_USAGE_STORAGE_BUFFER_READ);
	CHECK_EQ(other_records->usage, RDG::RESOURCE_USAGE_NONE);

	RDG::resource_tracker_free(records);
	RDG::resource_tracker_free(count);
	RDG::resource_tracker_free(other_records);
}

TEST_CASE("[RD][MeshShader] Metal: mesh shaders from Apple7 with MSL 3.0 and argument buffers, indirect mesh draws from Apple9") {
	const uint32_t msl_3_0 = RenderingDeviceDriver::METAL_MESH_SHADER_MIN_MSL;
	CHECK_EQ(msl_3_0, 30000u);
	// Mocked Apple8 (M2): mesh shaders as direct draws only (plan §21.1.5).
	const RenderingDeviceDriver::MeshShaderCaps apple8 = RenderingDeviceDriver::metal_mesh_shader_caps(1008, 30100, true);
	CHECK(apple8.mesh);
	CHECK_FALSE(apple8.indirect);
	const RenderingDeviceDriver::MeshShaderCaps apple7 = RenderingDeviceDriver::metal_mesh_shader_caps(1007, msl_3_0, true);
	CHECK(apple7.mesh);
	CHECK_FALSE(apple7.indirect);
	for (uint32_t family : { 1009u, 1010u, 1011u }) {
		const RenderingDeviceDriver::MeshShaderCaps caps = RenderingDeviceDriver::metal_mesh_shader_caps(family, 30200, true);
		CHECK(caps.mesh);
		CHECK(caps.indirect);
	}
	const RenderingDeviceDriver::MeshShaderCaps apple6 = RenderingDeviceDriver::metal_mesh_shader_caps(1006, 30200, true);
	CHECK_FALSE(apple6.mesh);
	CHECK_FALSE(apple6.indirect);
	// Below MSL 3.0 (macOS 12, iOS 15) there are no object or mesh functions, on any GPU.
	const RenderingDeviceDriver::MeshShaderCaps old_os = RenderingDeviceDriver::metal_mesh_shader_caps(1010, 20400, true);
	CHECK_FALSE(old_os.mesh);
	CHECK_FALSE(old_os.indirect);
	// The mesh pipelines bind through argument buffers: none without them, whatever the GPU and MSL version. An iOS 15
	// export profile (Apple7, MSL 2.4, no argument buffers) is refused on both counts.
	const RenderingDeviceDriver::MeshShaderCaps no_argument_buffers = RenderingDeviceDriver::metal_mesh_shader_caps(1010, 30200, false);
	CHECK_FALSE(no_argument_buffers.mesh);
	CHECK_FALSE(no_argument_buffers.indirect);
	CHECK_FALSE(RenderingDeviceDriver::metal_mesh_shader_caps(1007, 20400, false).mesh);
}

TEST_CASE("[RD][MeshShader] The mesh task group-count limits: bound, mapped, and 0 without mesh shaders") {
	const RenderingDevice::Limit limits[] = {
		RenderingDevice::LIMIT_MAX_TASK_WORKGROUP_COUNT_X,
		RenderingDevice::LIMIT_MAX_TASK_WORKGROUP_COUNT_Y,
		RenderingDevice::LIMIT_MAX_TASK_WORKGROUP_COUNT_Z,
		RenderingDevice::LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT,
		RenderingDevice::LIMIT_MAX_MESH_WORKGROUP_COUNT_X,
		RenderingDevice::LIMIT_MAX_MESH_WORKGROUP_COUNT_Y,
		RenderingDevice::LIMIT_MAX_MESH_WORKGROUP_COUNT_Z,
		RenderingDevice::LIMIT_MAX_MESH_WORKGROUP_TOTAL_COUNT,
	};
	// Appended after the last limit, so no bound value moves.
	CHECK_EQ(int(limits[0]), int(RenderingDevice::LIMIT_RAYTRACING_REORDER_EXTENSIONS) + 1);
	const char *names[] = {
		"LIMIT_MAX_TASK_WORKGROUP_COUNT_X",
		"LIMIT_MAX_TASK_WORKGROUP_COUNT_Y",
		"LIMIT_MAX_TASK_WORKGROUP_COUNT_Z",
		"LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT",
		"LIMIT_MAX_MESH_WORKGROUP_COUNT_X",
		"LIMIT_MAX_MESH_WORKGROUP_COUNT_Y",
		"LIMIT_MAX_MESH_WORKGROUP_COUNT_Z",
		"LIMIT_MAX_MESH_WORKGROUP_TOTAL_COUNT",
	};
	RenderingDeviceDriver::MeshShaderLimits mapped;
	for (uint32_t i = 0; i < 3; i++) {
		mapped.max_task_workgroup_count[i] = 10 + i;
		mapped.max_mesh_workgroup_count[i] = 20 + i;
	}
	mapped.max_task_workgroup_total_count = 13;
	mapped.max_mesh_workgroup_total_count = 23;
	const uint64_t expected[] = { 10, 11, 12, 13, 20, 21, 22, 23 };
	for (uint32_t i = 0; i < std_size(limits); i++) {
		CHECK_EQ(int(limits[i]), int(limits[0]) + int(i));
		bool valid = false;
		CHECK_EQ(ClassDB::get_integer_constant("RenderingDevice", names[i], &valid), int64_t(limits[i]));
		CHECK_MESSAGE(valid, names[i]);
		uint64_t value = 0;
		CHECK(RenderingDeviceDriver::mesh_shader_limit_get(limits[i], mapped, value));
		CHECK_EQ(value, expected[i]);
		// Without mesh shaders every driver reports 0.
		CHECK(RenderingDeviceDriver::mesh_shader_limit_get(limits[i], RenderingDeviceDriver::metal_mesh_shader_limits(1010, false), value));
		CHECK_EQ(value, 0u);
		CHECK(RenderingDeviceDriver::mesh_shader_limit_get(limits[i], RenderingDeviceDriver::d3d12_mesh_shader_limits(false), value));
		CHECK_EQ(value, 0u);
	}
	uint64_t other = 7;
	CHECK_FALSE(RenderingDeviceDriver::mesh_shader_limit_get(RenderingDevice::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X, mapped, other));
	CHECK_EQ(other, 7u);

	// Direct3D 12 (mesh shader specification): each count below 64k, at most 2^22 in all, for both stages.
	const RenderingDeviceDriver::MeshShaderLimits d3d12 = RenderingDeviceDriver::d3d12_mesh_shader_limits(true);
	for (uint32_t i = 0; i < 3; i++) {
		CHECK_EQ(d3d12.max_task_workgroup_count[i], 65535u);
		CHECK_EQ(d3d12.max_mesh_workgroup_count[i], 65535u);
	}
	CHECK_EQ(d3d12.max_task_workgroup_total_count, uint64_t(1) << 22);
	CHECK_EQ(d3d12.max_mesh_workgroup_total_count, uint64_t(1) << 22);

	// Metal (Metal Feature Set Tables): no limit on the object grid; the mesh grid holds 1024 threadgroups on Apple7 and
	// Apple8, 1,048,575 on Apple9 and 4,194,303 from Apple10.
	const uint64_t mesh_grids[][2] = { { 1007, 1024 }, { 1008, 1024 }, { 1009, 1048575 }, { 1010, 4194303 }, { 1011, 4194303 } };
	for (const uint64_t (&family_grid)[2] : mesh_grids) {
		const RenderingDeviceDriver::MeshShaderLimits metal = RenderingDeviceDriver::metal_mesh_shader_limits(uint32_t(family_grid[0]), true);
		for (uint32_t i = 0; i < 3; i++) {
			CHECK_EQ(metal.max_task_workgroup_count[i], uint64_t(UINT32_MAX));
			CHECK_EQ(metal.max_mesh_workgroup_count[i], family_grid[1]);
		}
		CHECK_EQ(metal.max_task_workgroup_total_count, uint64_t(UINT32_MAX));
		CHECK_EQ(metal.max_mesh_workgroup_total_count, family_grid[1]);
	}
}

TEST_CASE("[RD][MeshShader] Direct3D 12: mesh shaders follow the device tier and the linked Mesa NIR library, with exactly one log line") {
	String line;
	const RenderingDeviceDriver::MeshShaderCaps tier_1 = RenderingDeviceDriver::d3d12_mesh_shader_caps(RenderingDeviceDriver::D3D12_MESH_SHADER_TIER_1, false, line);
	CHECK_FALSE(tier_1.mesh);
	CHECK_FALSE(tier_1.indirect);
	CHECK_EQ(line, String("D3D12: Mesh shaders are off: the linked Mesa NIR library does not translate task and mesh shaders to DXIL (the GodotNext library 25.3.1-gn4 or later does; mesh shader tier 1)."));
	CHECK_EQ(line.split("\n").size(), 1);

	const RenderingDeviceDriver::MeshShaderCaps no_tier = RenderingDeviceDriver::d3d12_mesh_shader_caps(0, false, line);
	CHECK_FALSE(no_tier.mesh);
	CHECK_EQ(line, String("D3D12: Mesh shaders are off: the device has no mesh shader support (mesh shader tier none)."));

	// With a library that translates the stages (25.3.1-gn4 or later, #122), a tier-1 device reports both features.
	const RenderingDeviceDriver::MeshShaderCaps translated = RenderingDeviceDriver::d3d12_mesh_shader_caps(RenderingDeviceDriver::D3D12_MESH_SHADER_TIER_1, true, line);
	CHECK(translated.mesh);
	CHECK(translated.indirect);
	CHECK_EQ(line.split("\n").size(), 1);
	CHECK_FALSE(RenderingDeviceDriver::d3d12_mesh_shader_caps(0, true, line).mesh);
}

TEST_CASE("[RD][MeshShader] ShaderRD's mesh setup expands task, mesh and fragment stages") {
	for (bool task : { true, false }) {
		MeshTestShaderRD shader(task);
		CHECK(shader.requires_mesh_shader());
		CHECK_FALSE(shader.requires_ray_query());
		shader.initialize({ "\n#define MESH_GROUPS 3\n" });
		const RID version = shader.version_create(false);
		REQUIRE(version.is_valid());
		const Vector<String> sources = shader.version_build_variant_stage_sources(version, 0);
		REQUIRE_EQ(sources.size(), int(RDC::SHADER_STAGE_MAX));
		CHECK(sources[RDC::SHADER_STAGE_VERTEX].is_empty());
		CHECK(sources[RDC::SHADER_STAGE_COMPUTE].is_empty());
		CHECK_EQ(sources[RDC::SHADER_STAGE_TASK].is_empty(), !task);
		CHECK(sources[RDC::SHADER_STAGE_MESH].contains("SetMeshOutputsEXT"));
		CHECK(sources[RDC::SHADER_STAGE_FRAGMENT].contains("color = vec4(1.0);"));
		if (task) {
			CHECK(sources[RDC::SHADER_STAGE_TASK].contains("#define MESH_GROUPS 3"));
		}
#ifdef MODULE_GLSLANG_ENABLED
		// The expanded stages compile at the SPIR-V version the container formats ask for (1.4).
		for (RDC::ShaderStage stage : { RDC::SHADER_STAGE_TASK, RDC::SHADER_STAGE_MESH }) {
			if (sources[stage].is_empty()) {
				continue;
			}
			String error;
			const Vector<uint8_t> spirv = compile_glslang_shader(stage, sources[stage], RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_4, &error);
			CHECK_MESSAGE(!spirv.is_empty(), error);
		}
#endif
		CHECK(shader.version_free(version));
	}

	// The vertex setup is not a mesh shader.
	class VertexTestShaderRD : public ShaderRD {
	public:
		VertexTestShaderRD() {
			setup(SHADER_RD_FRAGMENT, SHADER_RD_FRAGMENT, nullptr, "VertexTestShaderRD");
		}
	};
	CHECK_FALSE(VertexTestShaderRD().requires_mesh_shader());
}

#ifdef TOOLS_ENABLED
TEST_CASE("[RD][MeshShader] The shader baker bakes mesh-shader versions only for formats with task and mesh stages") {
	// Mesh shader versions need the format; the others bake everywhere, as the ray-query rule had them.
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_version(false, false, true, false));
	CHECK(ShaderBakerExportPlugin::should_bake_version(false, false, true, true));
	CHECK(ShaderBakerExportPlugin::should_bake_version(false, false, false, false));
	CHECK(ShaderBakerExportPlugin::should_bake_version(false, true));
	// Both rules apply together.
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_version(true, false, true, true));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_version(true, true, true, false));
	CHECK(ShaderBakerExportPlugin::should_bake_version(true, true, true, true));

#ifdef VULKAN_ENABLED
	CHECK(RenderingShaderContainerFormatVulkan().supports_mesh_shaders());
#endif
#ifdef D3D12_ENABLED
	// Only libraries that write the format-6 containers (25.3.1-gn4 or later) translate the stages, so baked mesh
	// shaders never meet a library that cannot run them.
	CHECK_EQ(RenderingShaderContainerFormatD3D12().supports_mesh_shaders(), RenderingShaderContainerD3D12::get_library_format_version() >= RenderingShaderContainerD3D12::FORMAT_VERSION_NON_UNIFORM_RESOURCE_INDEX);
#endif
}
#endif // TOOLS_ENABLED

#endif // RD_ENABLED
