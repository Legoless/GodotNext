/**************************************************************************/
/*  rendering_device_mesh_shader.cpp                                      */
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

#include "rendering_device_mesh_shader.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

#include <cstring>

namespace RDMeshShader {

namespace {

struct TypeLayout {
	bool known = true;
	uint64_t size = 0;
	uint64_t align = 1;
};

static uint64_t align_up(uint64_t p_value, uint64_t p_align) {
	if (p_align <= 1) {
		return p_value;
	}
	const uint64_t rem = p_value % p_align;
	return rem == 0 ? p_value : p_value + (p_align - rem);
}

static uint64_t sat_mul(uint64_t p_a, uint64_t p_b) {
	constexpr uint64_t CAP = uint64_t(1) << 40;
	if (p_a == 0 || p_b == 0) {
		return 0;
	}
	if (p_a > CAP / p_b) {
		return CAP;
	}
	return p_a * p_b;
}

static uint64_t sat_add(uint64_t p_a, uint64_t p_b) {
	constexpr uint64_t CAP = uint64_t(1) << 40;
	if (p_a >= CAP || p_b >= CAP || p_a > CAP - p_b) {
		return CAP;
	}
	return p_a + p_b;
}

struct SpirvMeshModule {
	const uint8_t *spirv = nullptr;
	uint64_t word_count = 0;
	LocalVector<uint64_t> definitions;
	HashMap<uint32_t, uint32_t> array_strides;
	HashMap<uint64_t, uint32_t> member_offsets;

	uint32_t word(uint64_t p_index) const {
		uint32_t value = 0;
		memcpy(&value, spirv + p_index * sizeof(uint32_t), sizeof(uint32_t));
		return value;
	}

	uint64_t definition(uint32_t p_id) const {
		return p_id < definitions.size() ? definitions[p_id] : 0;
	}

	bool parse(const uint8_t *p_spirv, uint64_t p_size) {
		constexpr uint64_t HEADER_WORDS = 5;
		if (p_spirv == nullptr || p_size < HEADER_WORDS * sizeof(uint32_t) || (p_size % sizeof(uint32_t)) != 0) {
			return false;
		}
		spirv = p_spirv;
		word_count = p_size / sizeof(uint32_t);
		if (word(0) != SpvMagicNumber) {
			return false;
		}
		const uint32_t bound = word(3);
		if (bound == 0 || bound > word_count) {
			return false;
		}
		definitions.resize_initialized(bound);
		for (uint64_t i = HEADER_WORDS; i < word_count;) {
			const uint32_t instruction = word(i);
			const uint32_t length = instruction >> SpvWordCountShift;
			if (length == 0 || i + length > word_count) {
				return false;
			}
			const uint32_t opcode = instruction & SpvOpCodeMask;
			uint32_t result = 0;
			switch (opcode) {
				case SpvOpTypeBool:
				case SpvOpTypeInt:
				case SpvOpTypeFloat:
				case SpvOpTypeVector:
				case SpvOpTypeMatrix:
				case SpvOpTypeArray:
				case SpvOpTypeRuntimeArray:
				case SpvOpTypeStruct:
				case SpvOpTypePointer:
					result = length >= 2 ? word(i + 1) : 0;
					break;
				case SpvOpConstant:
				case SpvOpSpecConstant:
					result = length >= 3 ? word(i + 2) : 0;
					break;
				case SpvOpDecorate:
					if (length >= 4 && word(i + 2) == SpvDecorationArrayStride) {
						array_strides[word(i + 1)] = word(i + 3);
					}
					break;
				case SpvOpMemberDecorate:
					if (length >= 5 && word(i + 3) == SpvDecorationOffset) {
						const uint64_t key = (uint64_t(word(i + 1)) << 32) | uint64_t(word(i + 2));
						member_offsets[key] = word(i + 4);
					}
					break;
				default:
					break;
			}
			if (result != 0 && result < bound) {
				definitions[result] = i;
			}
			i += length;
		}
		return true;
	}

	bool constant_u32(uint32_t p_id, bool p_allow_spec_default, uint32_t &r_value) const {
		const uint64_t at = definition(p_id);
		if (at == 0) {
			return false;
		}
		const uint32_t instruction = word(at);
		const uint32_t length = instruction >> SpvWordCountShift;
		const uint32_t opcode = instruction & SpvOpCodeMask;
		if (length < 4) {
			return false;
		}
		if (opcode == SpvOpConstant || (p_allow_spec_default && opcode == SpvOpSpecConstant)) {
			r_value = word(at + 3);
			return true;
		}
		return false;
	}

	TypeLayout layout(uint32_t p_type, uint32_t p_depth = 0) const {
		TypeLayout unknown;
		unknown.known = false;
		const uint64_t at = definition(p_type);
		if (at == 0 || p_depth > 32) {
			return unknown;
		}
		const uint32_t instruction = word(at);
		const uint32_t length = instruction >> SpvWordCountShift;
		const uint32_t opcode = instruction & SpvOpCodeMask;
		TypeLayout result;
		switch (opcode) {
			case SpvOpTypeBool:
				result.size = 4;
				result.align = 4;
				return result;
			case SpvOpTypeInt:
			case SpvOpTypeFloat: {
				if (length < 3 || word(at + 2) == 0 || (word(at + 2) % 8) != 0) {
					return unknown;
				}
				const uint64_t bytes = word(at + 2) / 8;
				result.size = bytes;
				result.align = bytes;
				return result;
			}
			case SpvOpTypePointer: {
				if (length < 3 || word(at + 2) != SpvStorageClassPhysicalStorageBuffer) {
					return unknown;
				}
				result.size = 8;
				result.align = 8;
				return result;
			}
			case SpvOpTypeVector: {
				if (length < 4) {
					return unknown;
				}
				const TypeLayout elem = layout(word(at + 2), p_depth + 1);
				if (!elem.known) {
					return unknown;
				}
				const uint64_t count = word(at + 3);
				if (count == 0) {
					return unknown;
				}
				result.size = sat_mul(elem.size, count);
				result.align = sat_mul(elem.align, count == 3 ? 4 : count);
				return result;
			}
			case SpvOpTypeMatrix: {
				if (length < 4) {
					return unknown;
				}
				const TypeLayout col = layout(word(at + 2), p_depth + 1);
				if (!col.known) {
					return unknown;
				}
				const uint64_t cols = word(at + 3);
				const uint64_t stride = align_up(col.size, col.align);
				result.size = sat_mul(stride, cols);
				result.align = col.align;
				return result;
			}
			case SpvOpTypeArray: {
				if (length < 4) {
					return unknown;
				}
				uint32_t elements = 0;
				// An array sized by a specialization constant has no fixed size at compile time.
				if (!constant_u32(word(at + 3), false, elements)) {
					return unknown;
				}
				const TypeLayout elem = layout(word(at + 2), p_depth + 1);
				if (!elem.known) {
					return unknown;
				}
				const uint32_t *explicit_stride = array_strides.getptr(p_type);
				const uint64_t stride = explicit_stride != nullptr ? uint64_t(*explicit_stride) : align_up(elem.size, elem.align);
				result.size = sat_mul(stride, elements);
				result.align = elem.align;
				return result;
			}
			case SpvOpTypeStruct: {
				uint64_t offset = 0;
				uint64_t max_align = 1;
				uint64_t max_end = 0;
				bool any_explicit = false;
				for (uint32_t m = 2; m < length; m++) {
					const uint32_t member_index = m - 2;
					const TypeLayout member = layout(word(at + m), p_depth + 1);
					if (!member.known) {
						return unknown;
					}
					max_align = MAX(max_align, member.align);
					const uint64_t key = (uint64_t(p_type) << 32) | uint64_t(member_index);
					const uint32_t *explicit_offset = member_offsets.getptr(key);
					if (explicit_offset != nullptr) {
						any_explicit = true;
						max_end = MAX(max_end, sat_add(uint64_t(*explicit_offset), member.size));
					} else {
						offset = align_up(offset, member.align);
						offset = sat_add(offset, member.size);
						max_end = MAX(max_end, offset);
					}
				}
				result.size = align_up(any_explicit ? max_end : offset, max_align);
				result.align = max_align;
				return result;
			}
			default:
				return unknown;
		}
	}
};

static String format_shader_label(const String &p_shader_name) {
	return p_shader_name.is_empty() ? String("A shader") : vformat("Shader '%s'", p_shader_name);
}

static bool validate_stage_local_size(const String &p_shader_name, bool p_is_task, const uint32_t p_local_size[3], const RenderingDeviceDriver::MeshShaderLimits &p_limits, String &r_error) {
	const char *stage_name = p_is_task ? "task" : "mesh";
	const uint64_t *axis_limits = p_is_task ? p_limits.max_task_workgroup_size : p_limits.max_mesh_workgroup_size;
	const uint64_t inv_limit = p_is_task ? p_limits.max_task_workgroup_invocations : p_limits.max_mesh_workgroup_invocations;
	static const char *AXIS_CHARS[3] = { "X", "Y", "Z" };
	static const char *TASK_AXIS_LIMIT_NAMES[3] = { "LIMIT_MAX_TASK_WORKGROUP_SIZE_X", "LIMIT_MAX_TASK_WORKGROUP_SIZE_Y", "LIMIT_MAX_TASK_WORKGROUP_SIZE_Z" };
	static const char *MESH_AXIS_LIMIT_NAMES[3] = { "LIMIT_MAX_MESH_WORKGROUP_SIZE_X", "LIMIT_MAX_MESH_WORKGROUP_SIZE_Y", "LIMIT_MAX_MESH_WORKGROUP_SIZE_Z" };
	const char *const *axis_limit_names = p_is_task ? TASK_AXIS_LIMIT_NAMES : MESH_AXIS_LIMIT_NAMES;
	const char *inv_limit_name = p_is_task ? "LIMIT_MAX_TASK_WORKGROUP_INVOCATIONS" : "LIMIT_MAX_MESH_WORKGROUP_INVOCATIONS";

	for (uint32_t i = 0; i < 3; i++) {
		if (uint64_t(p_local_size[i]) > axis_limits[i]) {
			r_error = vformat("%s declares %s workgroup size %s = %d, which exceeds the device limit %s (%d).",
					format_shader_label(p_shader_name), stage_name, AXIS_CHARS[i], p_local_size[i], axis_limit_names[i], axis_limits[i]);
			return false;
		}
	}
	const uint64_t invocations = sat_mul(sat_mul(p_local_size[0], p_local_size[1]), p_local_size[2]);
	if (invocations > inv_limit) {
		r_error = vformat("%s declares %s workgroup size (%d, %d, %d) = %d invocations, which exceeds the device limit %s (%d).",
				format_shader_label(p_shader_name), stage_name, p_local_size[0], p_local_size[1], p_local_size[2], invocations, inv_limit_name, inv_limit);
		return false;
	}
	return true;
}

} // namespace

StageProperties inspect_spirv(const uint8_t *p_spirv, uint64_t p_size) {
	StageProperties props;
	SpirvMeshModule module;
	if (!module.parse(p_spirv, p_size)) {
		return props;
	}
	props.valid_spirv = true;

	for (uint64_t i = 5; i < module.word_count;) {
		const uint32_t instruction = module.word(i);
		const uint32_t length = instruction >> SpvWordCountShift;
		const uint32_t opcode = instruction & SpvOpCodeMask;
		if (opcode == SpvOpExecutionMode && length >= 3) {
			const uint32_t mode = module.word(i + 2);
			if (mode == SpvExecutionModeLocalSize && length >= 6) {
				props.local_size[0] = module.word(i + 3);
				props.local_size[1] = module.word(i + 4);
				props.local_size[2] = module.word(i + 5);
			} else if (mode == SpvExecutionModeOutputVertices && length >= 4) {
				props.output_vertices = module.word(i + 3);
			} else if (mode == SpvExecutionModeOutputPrimitivesEXT && length >= 4) {
				props.output_primitives = module.word(i + 3);
			}
		} else if (opcode == SpvOpExecutionModeId && length >= 6) {
			const uint32_t mode = module.word(i + 2);
			if (mode == SpvExecutionModeLocalSizeId) {
				uint32_t x = 1;
				uint32_t y = 1;
				uint32_t z = 1;
				if (module.constant_u32(module.word(i + 3), true, x)) {
					props.local_size[0] = x;
				}
				if (module.constant_u32(module.word(i + 4), true, y)) {
					props.local_size[1] = y;
				}
				if (module.constant_u32(module.word(i + 5), true, z)) {
					props.local_size[2] = z;
				}
			}
		} else if (opcode == SpvOpVariable && length >= 4) {
			const uint32_t storage_class = module.word(i + 3);
			if (storage_class == SpvStorageClassTaskPayloadWorkgroupEXT) {
				const uint64_t ptr_at = module.definition(module.word(i + 1));
				if (ptr_at == 0 || (module.word(ptr_at) & SpvOpCodeMask) != SpvOpTypePointer || (module.word(ptr_at) >> SpvWordCountShift) < 4) {
					props.task_payload_size = PAYLOAD_SIZE_UNKNOWN;
				} else if (props.task_payload_size != PAYLOAD_SIZE_UNKNOWN) {
					const TypeLayout tl = module.layout(module.word(ptr_at + 3));
					if (!tl.known) {
						props.task_payload_size = PAYLOAD_SIZE_UNKNOWN;
					} else {
						props.task_payload_size = MAX(props.task_payload_size, tl.size);
					}
				}
			}
		}
		i += length;
	}

	return props;
}

bool validate_shader(const String &p_shader_name, Span<RenderingDeviceCommons::ShaderStageSPIRVData> p_spirv, const RenderingDeviceDriver::MeshShaderLimits &p_limits, String &r_error) {
	r_error = String();
	for (const RenderingDeviceCommons::ShaderStageSPIRVData &stage : p_spirv) {
		if (stage.shader_stage != RenderingDeviceCommons::SHADER_STAGE_TASK && stage.shader_stage != RenderingDeviceCommons::SHADER_STAGE_MESH) {
			continue;
		}
		const bool is_task = stage.shader_stage == RenderingDeviceCommons::SHADER_STAGE_TASK;
		const StageProperties props = inspect_spirv(stage.spirv.ptr(), stage.spirv.size());
		if (!props.valid_spirv) {
			continue;
		}
		if (!validate_stage_local_size(p_shader_name, is_task, props.local_size, p_limits, r_error)) {
			return false;
		}
		if (!is_task) {
			if (uint64_t(props.output_vertices) > p_limits.max_mesh_output_vertices) {
				r_error = vformat("%s declares mesh shader max_vertices = %d, which exceeds the device limit LIMIT_MAX_MESH_OUTPUT_VERTICES (%d).",
						format_shader_label(p_shader_name), props.output_vertices, p_limits.max_mesh_output_vertices);
				return false;
			}
			if (uint64_t(props.output_primitives) > p_limits.max_mesh_output_primitives) {
				r_error = vformat("%s declares mesh shader max_primitives = %d, which exceeds the device limit LIMIT_MAX_MESH_OUTPUT_PRIMITIVES (%d).",
						format_shader_label(p_shader_name), props.output_primitives, p_limits.max_mesh_output_primitives);
				return false;
			}
		}
		if (props.task_payload_size == PAYLOAD_SIZE_UNKNOWN) {
			r_error = vformat("%s declares a task payload in its %s stage whose byte size is not fixed; it must fit within LIMIT_MAX_TASK_PAYLOAD_SIZE (%d bytes).",
					format_shader_label(p_shader_name), is_task ? "task" : "mesh", p_limits.max_task_payload_size);
			return false;
		}
		if (props.task_payload_size > p_limits.max_task_payload_size) {
			r_error = vformat("%s declares a %d-byte task payload in its %s stage, which exceeds the device limit LIMIT_MAX_TASK_PAYLOAD_SIZE (%d bytes).",
					format_shader_label(p_shader_name), props.task_payload_size, is_task ? "task" : "mesh", p_limits.max_task_payload_size);
			return false;
		}
	}
	return true;
}

bool validate_reflection_local_sizes(const String &p_shader_name, const RenderingDeviceCommons::ShaderReflection &p_reflection, const RenderingDeviceDriver::MeshShaderLimits &p_limits, String &r_error) {
	r_error = String();
	if (p_reflection.stages_bits.has_flag(RenderingDeviceCommons::SHADER_STAGE_TASK_BIT)) {
		if (!validate_stage_local_size(p_shader_name, true, p_reflection.task_local_size, p_limits, r_error)) {
			return false;
		}
	}
	if (p_reflection.stages_bits.has_flag(RenderingDeviceCommons::SHADER_STAGE_MESH_BIT)) {
		if (!validate_stage_local_size(p_shader_name, false, p_reflection.mesh_local_size, p_limits, r_error)) {
			return false;
		}
	}
	return true;
}

} // namespace RDMeshShader
