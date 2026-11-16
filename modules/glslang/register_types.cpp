/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "shader_compile.h"

#include "core/config/engine.h"

#ifdef D3D12_ENABLED
#include "core/os/os.h"
#endif

GODOT_GCC_WARNING_PUSH_AND_IGNORE("-Wshadow")

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

GODOT_GCC_WARNING_POP

// GodotNext: rd_mesh_shaders (#178). Whether a line of the source enables GL_EXT_mesh_shader: an #extension directive
// with require, enable or warn. A comment or a "disable" that names it does not.
static bool _source_enables_mesh_shader_extension(const String &p_source_code) {
	if (!p_source_code.contains("GL_EXT_mesh_shader")) {
		return false;
	}
	for (const String &line : p_source_code.split("\n")) {
		String directive = line.strip_edges();
		if (!directive.begins_with("#")) {
			continue;
		}
		directive = directive.substr(1).strip_edges();
		if (!directive.begins_with("extension")) {
			continue;
		}
		directive = directive.substr(int(sizeof("extension")) - 1).strip_edges();
		if (!directive.begins_with("GL_EXT_mesh_shader")) {
			continue;
		}
		directive = directive.substr(int(sizeof("GL_EXT_mesh_shader")) - 1).strip_edges();
		if (!directive.begins_with(":")) {
			continue;
		}
		directive = directive.substr(1).strip_edges();
		if (directive.begins_with("require") || directive.begins_with("enable") || directive.begins_with("warn")) {
			return true;
		}
	}
	return false;
}

Vector<uint8_t> compile_glslang_shader(RenderingDeviceCommons::ShaderStage p_stage, const String &p_source_code, RenderingDeviceCommons::ShaderLanguageVersion p_language_version, RenderingDeviceCommons::ShaderSpirvVersion p_spirv_version, String *r_error) {
	Vector<uint8_t> ret;
	// GodotNext: rd_mesh_shaders (#178). Sized by the initializer, so a stage without an entry fails to compile.
	static const EShLanguage stages[] = {
		EShLangVertex,
		EShLangFragment,
		EShLangTessControl,
		EShLangTessEvaluation,
		EShLangCompute,
		EShLangRayGen,
		EShLangAnyHit,
		EShLangClosestHit,
		EShLangMiss,
		EShLangIntersect,
		// GL_EXT_mesh_shader needs SPIR-V 1.4, which every container format asks for these two stages.
		EShLangTask,
		EShLangMesh,
	};
	static_assert(std_size(stages) == RenderingDeviceCommons::SHADER_STAGE_MAX);
	ERR_FAIL_INDEX_V(p_stage, RenderingDeviceCommons::SHADER_STAGE_MAX, ret);

	// GodotNext: rd_mesh_shaders (#178). GL_EXT_mesh_shader needs SPIR-V 1.4, and glslang refuses lower targets. The
	// fragment shader of a mesh pipeline enables it to read per-primitive values (perprimitiveEXT), while container
	// formats ask for SPIR-V 1.3 for every fragment shader (Vulkan 1.1 devices). A device that runs mesh shaders
	// takes SPIR-V 1.4, so a stage that enables the extension is compiled for it.
	if (p_spirv_version < RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_4 && _source_enables_mesh_shader_extension(p_source_code)) {
		p_spirv_version = RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_4;
	}

	int ClientInputSemanticsVersion = 100; // maps to, say, #define VULKAN 100

	// The enum values can be converted directly.
	glslang::EShTargetClientVersion ClientVersion = (glslang::EShTargetClientVersion)p_language_version;
	glslang::EShTargetLanguageVersion TargetVersion = (glslang::EShTargetLanguageVersion)p_spirv_version;

	glslang::TShader shader(stages[p_stage]);
	CharString cs = p_source_code.utf8();
	const char *cs_strings = cs.get_data();
	std::string preamble = "";

	shader.setStrings(&cs_strings, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, stages[p_stage], glslang::EShClientVulkan, ClientInputSemanticsVersion);
	shader.setEnvClient(glslang::EShClientVulkan, ClientVersion);
	shader.setEnvTarget(glslang::EShTargetSpv, TargetVersion);

	if (!preamble.empty()) {
		shader.setPreamble(preamble.c_str());
	}

	bool generate_spirv_debug_info = Engine::get_singleton()->is_generate_spirv_debug_info_enabled();
#ifdef D3D12_ENABLED
	if (OS::get_singleton()->get_current_rendering_driver_name() == "d3d12") {
		// SPIRV to DXIL conversion does not support debug info.
		generate_spirv_debug_info = false;
	}
#endif

	EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
	if (generate_spirv_debug_info) {
		messages = (EShMessages)(messages | EShMsgDebugInfo);
	}
	const int DefaultVersion = 100;

	//parse
	if (!shader.parse(GetDefaultResources(), DefaultVersion, false, messages)) {
		if (r_error) {
			(*r_error) = "Failed parse:\n";
			(*r_error) += shader.getInfoLog();
			(*r_error) += "\n";
			(*r_error) += shader.getInfoDebugLog();
		}
		return ret;
	}

	//link
	glslang::TProgram program;
	program.addShader(&shader);

	if (!program.link(messages)) {
		if (r_error) {
			(*r_error) = "Failed link:\n";
			(*r_error) += program.getInfoLog();
			(*r_error) += "\n";
			(*r_error) += program.getInfoDebugLog();
		}

		return ret;
	}

	std::vector<uint32_t> SpirV;
	spv::SpvBuildLogger logger;
	glslang::SpvOptions spvOptions;

	if (generate_spirv_debug_info) {
		spvOptions.generateDebugInfo = true;
		spvOptions.emitNonSemanticShaderDebugInfo = true;
		spvOptions.emitNonSemanticShaderDebugSource = true;
	}

	glslang::GlslangToSpv(*program.getIntermediate(stages[p_stage]), SpirV, &logger, &spvOptions);

	ret.resize(SpirV.size() * sizeof(uint32_t));
	{
		uint8_t *w = ret.ptrw();
		memcpy(w, &SpirV[0], SpirV.size() * sizeof(uint32_t));
	}

	return ret;
}

void initialize_glslang_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_CORE) {
		return;
	}

	// Initialize in case it's not initialized. This is done once per thread
	// and it's safe to call multiple times.
	glslang::InitializeProcess();
}

void uninitialize_glslang_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_CORE) {
		return;
	}

	glslang::FinalizeProcess();
}
