/**************************************************************************/
/*  rendering_device_rt_pipeline.cpp                                      */
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

#include "rendering_device_rt_pipeline.h"

#include "core/templates/local_vector.h"

#include <thirdparty/spirv-headers/include/spirv/unified1/spirv.h>

namespace RDRTPipeline {

uint32_t get_hit_group_stride(uint32_t p_handle_size, uint32_t p_handle_alignment, uint32_t p_record_size) {
	const uint32_t alignment = MAX(p_handle_alignment, 1u);
	const uint32_t size = p_handle_size + p_record_size;
	return ((size + alignment - 1) / alignment) * alignment;
}

void write_hit_group_records(uint8_t *r_table, uint32_t p_stride, uint32_t p_handle_size, uint32_t p_record_size, const uint32_t *p_hit_group_indices, uint32_t p_count, const uint8_t *p_records, uint32_t p_hit_group_count) {
	ERR_FAIL_COND(p_record_size > MAX_HIT_GROUP_RECORD_SIZE || p_handle_size + p_record_size > p_stride);
	for (uint32_t i = 0; i < p_count; i++) {
		uint8_t *entry = r_table + uint64_t(i) * p_stride;
		uint8_t *record = entry + p_handle_size;
		if (p_hit_group_indices[i] < p_hit_group_count) {
			memcpy(record, p_records + uint64_t(p_hit_group_indices[i]) * MAX_HIT_GROUP_RECORD_SIZE, p_record_size);
		} else {
			memset(record, 0, p_record_size);
		}
		memset(record + p_record_size, 0, p_stride - p_handle_size - p_record_size);
	}
}

String get_record_too_large_error(uint32_t p_hit_group_index, uint32_t p_size) {
	return vformat("Hit group %d has a %d-byte shader record; a record holds at most %d bytes.", p_hit_group_index, p_size, MAX_HIT_GROUP_RECORD_SIZE);
}

uint32_t get_spirv_reorder_usage(const uint8_t *p_spirv, uint64_t p_size) {
	const uint64_t header_words = 5;
	if (p_spirv == nullptr || p_size < header_words * sizeof(uint32_t) || p_size % sizeof(uint32_t) != 0) {
		return 0;
	}
	const uint64_t count = p_size / sizeof(uint32_t);
	auto word = [p_spirv](uint64_t p_index) -> uint32_t {
		uint32_t value;
		memcpy(&value, p_spirv + p_index * sizeof(uint32_t), sizeof(uint32_t));
		return value;
	};
	if (word(0) != SpvMagicNumber) {
		return 0;
	}

	// Capabilities come first in a module.
	uint32_t usage = 0;
	uint64_t i = header_words;
	while (i < count) {
		const uint32_t instruction = word(i);
		const uint32_t length = instruction >> SpvWordCountShift;
		if (length == 0 || i + length > count || (instruction & SpvOpCodeMask) != SpvOpCapability) {
			break;
		}
		if (length >= 2) {
			switch (word(i + 1)) {
				case SpvCapabilityShaderInvocationReorderEXT:
					usage |= REORDER_USAGE_EXT;
					break;
				case SpvCapabilityShaderInvocationReorderNV:
					usage |= REORDER_USAGE_NV;
					break;
				default:
					break;
			}
		}
		i += length;
	}
	return usage;
}

String get_missing_reorder_error(const String &p_shader_name, uint32_t p_missing_usage) {
	const String shader = p_shader_name.is_empty() ? String("A shader") : "Shader '" + p_shader_name + "'";
	PackedStringArray extensions;
	if (p_missing_usage & REORDER_USAGE_EXT) {
		extensions.push_back("GL_EXT_shader_invocation_reorder");
	}
	if (p_missing_usage & REORDER_USAGE_NV) {
		extensions.push_back("GL_NV_shader_invocation_reorder");
	}
	return shader + " uses shader execution reordering (" + String(", ").join(extensions) + "), which this device does not support (SUPPORTS_RAYTRACING_REORDER, LIMIT_RAYTRACING_REORDER_EXTENSIONS).";
}

namespace {

// The scalars of a type, for the payload size: how many, the largest scalar's size and the last scalar's
// size, in bytes. count is 0 for an empty struct; known is false for a type without a size.
struct PayloadScalars {
	bool known = true;
	uint64_t count = 0;
	uint32_t largest = 0;
	uint32_t last = 0;
};

class PayloadModule {
	const uint8_t *spirv = nullptr;
	uint64_t word_count = 0;
	// The word index of the instruction that defines each id, 0 when none does.
	LocalVector<uint64_t> definitions;

public:
	uint32_t word(uint64_t p_index) const {
		uint32_t value;
		memcpy(&value, spirv + p_index * sizeof(uint32_t), sizeof(uint32_t));
		return value;
	}

	uint64_t get_word_count() const { return word_count; }

	bool parse(const uint8_t *p_spirv, uint64_t p_size) {
		const uint64_t header_words = 5;
		if (p_spirv == nullptr || p_size < header_words * sizeof(uint32_t) || p_size % sizeof(uint32_t) != 0) {
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
		for (uint64_t i = header_words; i < word_count;) {
			const uint32_t instruction = word(i);
			const uint32_t length = instruction >> SpvWordCountShift;
			if (length == 0 || i + length > word_count) {
				return false;
			}
			uint32_t result = 0;
			switch (instruction & SpvOpCodeMask) {
				// Types: the result is the first operand.
				case SpvOpTypeBool:
				case SpvOpTypeInt:
				case SpvOpTypeFloat:
				case SpvOpTypeVector:
				case SpvOpTypeMatrix:
				case SpvOpTypeArray:
				case SpvOpTypeStruct:
				case SpvOpTypePointer:
					result = length >= 2 ? word(i + 1) : 0;
					break;
				// Constants: the result follows the result type.
				case SpvOpConstant:
				case SpvOpSpecConstant:
					result = length >= 3 ? word(i + 2) : 0;
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

	// The instruction that defines p_id: its word index, or 0.
	uint64_t definition(uint32_t p_id) const {
		return p_id < definitions.size() ? definitions[p_id] : 0;
	}

	PayloadScalars scalars(uint32_t p_type, uint32_t p_depth = 0) const {
		PayloadScalars unknown;
		unknown.known = false;
		const uint64_t at = definition(p_type);
		if (at == 0 || p_depth > 32) {
			return unknown;
		}
		const uint32_t length = word(at) >> SpvWordCountShift;
		PayloadScalars result;
		switch (word(at) & SpvOpCodeMask) {
			case SpvOpTypeBool:
				// Booleans have no size of their own; count them as 32 bits.
				result.count = 1;
				result.largest = 4;
				result.last = 4;
				return result;
			case SpvOpTypeInt:
			case SpvOpTypeFloat: {
				if (length < 3 || word(at + 2) % 8 != 0 || word(at + 2) == 0) {
					return unknown;
				}
				result.count = 1;
				result.largest = word(at + 2) / 8;
				result.last = result.largest;
				return result;
			}
			case SpvOpTypePointer:
				// A physical storage buffer address inside the payload.
				if (length < 3 || word(at + 2) != SpvStorageClassPhysicalStorageBuffer) {
					return unknown;
				}
				result.count = 1;
				result.largest = 8;
				result.last = 8;
				return result;
			case SpvOpTypeVector:
			case SpvOpTypeMatrix:
			case SpvOpTypeArray: {
				if (length < 4) {
					return unknown;
				}
				uint64_t elements = word(at + 3);
				if ((word(at) & SpvOpCodeMask) == SpvOpTypeArray) {
					// The length is the id of a constant; a specialization constant has no fixed value.
					const uint64_t constant = definition(word(at + 3));
					if (constant == 0 || (word(constant) & SpvOpCodeMask) != SpvOpConstant || (word(constant) >> SpvWordCountShift) < 4) {
						return unknown;
					}
					elements = word(constant + 3);
				}
				const PayloadScalars element = scalars(word(at + 2), p_depth + 1);
				if (!element.known) {
					return unknown;
				}
				// Saturates far above any payload limit instead of overflowing.
				const uint64_t limit = uint64_t(1) << 40;
				result.count = (element.count != 0 && elements > limit / element.count) ? limit : element.count * elements;
				result.largest = element.largest;
				result.last = elements > 0 ? element.last : 0;
				return result;
			}
			case SpvOpTypeStruct: {
				for (uint32_t m = 2; m < length; m++) {
					const PayloadScalars member = scalars(word(at + m), p_depth + 1);
					if (!member.known) {
						return unknown;
					}
					result.count = MIN(result.count + member.count, uint64_t(1) << 40);
					result.largest = MAX(result.largest, member.largest);
					if (member.count > 0) {
						result.last = member.last;
					}
				}
				return result;
			}
			default:
				return unknown;
		}
	}
};

} // namespace

uint32_t get_spirv_payload_size(const uint8_t *p_spirv, uint64_t p_size) {
	PayloadModule module;
	if (!module.parse(p_spirv, p_size)) {
		return 0;
	}

	uint64_t largest = 0;
	for (uint64_t i = 5; i < module.get_word_count();) {
		const uint32_t instruction = module.word(i);
		const uint32_t length = instruction >> SpvWordCountShift;
		if ((instruction & SpvOpCodeMask) == SpvOpVariable && length >= 4) {
			const uint32_t storage_class = module.word(i + 3);
			if (storage_class == SpvStorageClassRayPayloadKHR || storage_class == SpvStorageClassIncomingRayPayloadKHR) {
				// The variable's type is a pointer to the payload's type.
				const uint64_t pointer = module.definition(module.word(i + 1));
				if (pointer == 0 || (module.word(pointer) & SpvOpCodeMask) != SpvOpTypePointer || (module.word(pointer) >> SpvWordCountShift) < 4) {
					return PAYLOAD_SIZE_UNKNOWN;
				}
				const PayloadScalars payload = module.scalars(module.word(pointer + 3));
				if (!payload.known) {
					return PAYLOAD_SIZE_UNKNOWN;
				}
				// Every scalar starts at a multiple of the largest one's size; the last one ends the payload.
				const uint64_t size = payload.count == 0 ? 0 : (payload.count - 1) * payload.largest + payload.last;
				largest = MAX(largest, size);
			}
		}
		i += length;
	}
	return uint32_t(MIN(largest, uint64_t(PAYLOAD_SIZE_UNKNOWN - 1)));
}

String get_library_payload_error(const String &p_shader_name, RDD::ShaderStage p_stage, uint32_t p_size) {
	const String shader = p_shader_name.is_empty() ? String("A shader") : "Shader '" + p_shader_name + "'";
	String stage_name;
	switch (p_stage) {
		case RDD::SHADER_STAGE_RAYGEN:
			stage_name = "ray generation";
			break;
		case RDD::SHADER_STAGE_ANY_HIT:
			stage_name = "any-hit";
			break;
		case RDD::SHADER_STAGE_CLOSEST_HIT:
			stage_name = "closest-hit";
			break;
		case RDD::SHADER_STAGE_MISS:
			stage_name = "miss";
			break;
		case RDD::SHADER_STAGE_INTERSECTION:
			stage_name = "intersection";
			break;
		default:
			stage_name = itos(p_stage);
			break;
	}
	if (p_size == PAYLOAD_SIZE_UNKNOWN) {
		return vformat("%s declares a ray payload in its %s stage whose size is not fixed (an array sized by a specialization constant, or a type without a size); a raytracing pipeline library needs one of at most %d bytes.", shader, stage_name, LIBRARY_MAX_PAYLOAD_SIZE);
	}
	return vformat("%s declares a %d-byte ray payload in its %s stage; a raytracing pipeline library holds payloads of at most %d bytes.", shader, p_size, stage_name, LIBRARY_MAX_PAYLOAD_SIZE);
}

LinkStatus get_link_base_status(const LinkPart &p_base) {
	if (!p_base.library && !p_base.linked) {
		return LINK_BASE_NOT_LIBRARY;
	}
	if (p_base.raygen_count == 0) {
		return LINK_BASE_NO_RAYGEN;
	}
	return LINK_OK;
}

LinkStatus get_link_library_status(const LinkPart &p_base, const LinkPart &p_library) {
	if (!p_library.library) {
		return LINK_LIBRARY_NOT_LIBRARY;
	}
	if (p_library.raygen_count != 0 || p_library.miss_count != 0) {
		return LINK_LIBRARY_HAS_RAYGEN_OR_MISS;
	}
	if (p_library.max_trace_recursion_depth != p_base.max_trace_recursion_depth) {
		return LINK_RECURSION_MISMATCH;
	}
	if (p_library.push_constant_size != p_base.push_constant_size || p_library.set_formats != p_base.set_formats || p_library.immutable_sampler_hash != p_base.immutable_sampler_hash) {
		return LINK_LAYOUT_MISMATCH;
	}
	return LINK_OK;
}

const char *get_link_error(LinkStatus p_status) {
	switch (p_status) {
		case LINK_BASE_NOT_LIBRARY:
			return "The base of a link must be a pipeline created with RAYTRACING_PIPELINE_FLAG_LIBRARY, or one that raytracing_pipeline_link() returned.";
		case LINK_BASE_NO_RAYGEN:
			return "The base of a link must have at least one ray generation shader.";
		case LINK_LIBRARY_NOT_LIBRARY:
			return "Only pipelines created with RAYTRACING_PIPELINE_FLAG_LIBRARY can be linked to a base.";
		case LINK_LIBRARY_HAS_RAYGEN_OR_MISS:
			return "A library linked to a base adds hit groups only; its ray generation and miss shaders belong in the base.";
		case LINK_RECURSION_MISMATCH:
			return "A linked library must have the base's max_trace_recursion_depth.";
		case LINK_LAYOUT_MISMATCH:
			return "A linked library must have the base's layout: the same uniform set formats, immutable samplers and push constant size.";
		case LINK_DUPLICATE_LIBRARY:
			return "A library can be linked into a pipeline only once.";
		case LINK_OK:
		default:
			return "";
	}
}

IndirectStatus get_indirect_status(BitField<RDD::BufferUsageBits> p_usage, uint64_t p_offset, uint64_t p_buffer_size) {
	if (!p_usage.has_flag(RDD::BUFFER_USAGE_INDIRECT_BIT)) {
		return INDIRECT_NO_INDIRECT_USAGE;
	}
	if (!p_usage.has_flag(RDD::BUFFER_USAGE_DEVICE_ADDRESS_BIT)) {
		return INDIRECT_NO_DEVICE_ADDRESS;
	}
	if ((p_offset & 3) != 0) {
		return INDIRECT_MISALIGNED;
	}
	if (p_offset > p_buffer_size || p_buffer_size - p_offset < INDIRECT_COMMAND_SIZE) {
		return INDIRECT_OUT_OF_RANGE;
	}
	return INDIRECT_OK;
}

const char *get_indirect_error(IndirectStatus p_status) {
	switch (p_status) {
		case INDIRECT_NO_INDIRECT_USAGE:
			return "The indirect trace buffer must be a storage buffer created with STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT.";
		case INDIRECT_NO_DEVICE_ADDRESS:
			return "The indirect trace buffer must be created with BUFFER_CREATION_DEVICE_ADDRESS_BIT.";
		case INDIRECT_MISALIGNED:
			return "The indirect trace offset must be a multiple of 4.";
		case INDIRECT_OUT_OF_RANGE:
			return "The indirect trace command (width, height and depth, 12 bytes) must lie inside the buffer.";
		case INDIRECT_OK:
		default:
			return "";
	}
}

// GodotNext: d3d12_rt_pipelines (#121)

String get_stage_name(RDD::ShaderStage p_stage) {
	switch (p_stage) {
		case RDD::SHADER_STAGE_RAYGEN:
			return "ray generation";
		case RDD::SHADER_STAGE_ANY_HIT:
			return "any-hit";
		case RDD::SHADER_STAGE_CLOSEST_HIT:
			return "closest-hit";
		case RDD::SHADER_STAGE_MISS:
			return "miss";
		case RDD::SHADER_STAGE_INTERSECTION:
			return "intersection";
		default:
			return itos(p_stage);
	}
}

bool rewrite_spirv_shader_record_as_uniform_buffer(const uint32_t *p_spirv, uint64_t p_word_count, uint32_t p_set, uint32_t p_binding, Vector<uint32_t> &r_spirv) {
	r_spirv.clear();
	const uint64_t header_words = 5;
	if (p_spirv == nullptr || p_word_count < header_words || p_spirv[0] != SpvMagicNumber) {
		return false;
	}

	// The record's variable, and where the annotations start: the first instruction after the debug section.
	uint32_t record_id = 0;
	uint64_t annotations_start = 0;
	for (uint64_t i = header_words; i < p_word_count;) {
		const uint32_t length = p_spirv[i] >> SpvWordCountShift;
		if (length == 0 || i + length > p_word_count) {
			return false;
		}
		switch (SpvOp(p_spirv[i] & SpvOpCodeMask)) {
			case SpvOpNop:
			case SpvOpSourceContinued:
			case SpvOpSource:
			case SpvOpSourceExtension:
			case SpvOpName:
			case SpvOpMemberName:
			case SpvOpString:
			case SpvOpLine:
			case SpvOpNoLine:
			case SpvOpModuleProcessed:
			case SpvOpCapability:
			case SpvOpExtension:
			case SpvOpExtInstImport:
			case SpvOpMemoryModel:
			case SpvOpEntryPoint:
			case SpvOpExecutionMode:
			case SpvOpExecutionModeId:
				break;
			case SpvOpVariable:
				if (annotations_start == 0) {
					annotations_start = i;
				}
				if (length >= 4 && p_spirv[i + 3] == SpvStorageClassShaderRecordBufferKHR) {
					// GLSL allows one shaderRecordEXT block per stage.
					if (record_id != 0) {
						return false;
					}
					record_id = p_spirv[i + 2];
				}
				break;
			default:
				if (annotations_start == 0) {
					annotations_start = i;
				}
				break;
		}
		i += length;
	}
	if (record_id == 0 || annotations_start == 0) {
		return false;
	}

	// The block becomes a uniform buffer: the variable and every pointer type in the record's storage class move to
	// Uniform, and the variable gets the set and binding Mesa reads.
	const uint32_t decorations[] = {
		(4u << SpvWordCountShift) | SpvOpDecorate, record_id, SpvDecorationDescriptorSet, p_set,
		(4u << SpvWordCountShift) | SpvOpDecorate, record_id, SpvDecorationBinding, p_binding
	};
	r_spirv.resize(p_word_count + std_size(decorations));
	uint32_t *out = r_spirv.ptrw();
	memcpy(out, p_spirv, annotations_start * sizeof(uint32_t));
	memcpy(out + annotations_start, decorations, sizeof(decorations));
	memcpy(out + annotations_start + std_size(decorations), p_spirv + annotations_start, (p_word_count - annotations_start) * sizeof(uint32_t));
	for (uint64_t i = header_words; i < uint64_t(r_spirv.size());) {
		const uint32_t length = out[i] >> SpvWordCountShift;
		switch (SpvOp(out[i] & SpvOpCodeMask)) {
			case SpvOpTypePointer:
			case SpvOpTypeForwardPointer:
				if (length >= 3 && out[i + 2] == SpvStorageClassShaderRecordBufferKHR) {
					out[i + 2] = SpvStorageClassUniform;
				}
				break;
			case SpvOpVariable:
				if (length >= 4 && out[i + 3] == SpvStorageClassShaderRecordBufferKHR) {
					out[i + 3] = SpvStorageClassUniform;
				}
				break;
			default:
				break;
		}
		i += length;
	}
	return true;
}

String get_d3d12_stage_error(uint32_t p_shader_index, RDD::ShaderStage p_stage, bool p_has_stage_data, uint32_t p_record_size) {
	if (!p_has_stage_data) {
		return vformat("The %s stage of shader %d was compiled into a D3D12 shader container of an older format, without its ray-tracing data. Compile the shader again.", get_stage_name(p_stage), p_shader_index);
	}
	if (p_record_size == 0) {
		return String();
	}
	if (p_stage == RDD::SHADER_STAGE_RAYGEN || p_stage == RDD::SHADER_STAGE_MISS) {
		return vformat("A %s shader reads a shader record (shaderRecordEXT); only hit groups carry records.", get_stage_name(p_stage));
	}
	if (p_record_size > MAX_HIT_GROUP_RECORD_SIZE) {
		return vformat("A %s shader reads a %d-byte shader record (shaderRecordEXT); a record holds at most %d bytes.", get_stage_name(p_stage), p_record_size, MAX_HIT_GROUP_RECORD_SIZE);
	}
	return String();
}

namespace {

String describe_payload(const StageInterface &p_stage) {
	return p_stage.payload_declared ? vformat("a %d-byte ray payload", p_stage.payload_size) : vformat("no ray payload that it uses (it gets a stand-in of %d bytes)", p_stage.payload_size);
}

String describe_attributes(const StageInterface &p_stage) {
	return p_stage.attributes_declared ? vformat("%d bytes of hit attributes", p_stage.attribute_size) : vformat("no hit attributes that it uses (it gets a stand-in of %d bytes)", p_stage.attribute_size);
}

} // namespace

String get_hit_group_interface_error(uint32_t p_hit_group_index, const StageInterface *p_any_hit, const StageInterface *p_closest_hit, const StageInterface *p_intersection) {
	const String prefix = vformat("Hit group %d: ", p_hit_group_index);
	if (p_any_hit != nullptr && p_closest_hit != nullptr && p_any_hit->payload_size != p_closest_hit->payload_size) {
		return prefix + vformat("its any-hit shader declares %s and its closest-hit shader %s. Direct3D 12 passes both the same ray payload, so they must declare and use (read or write) the same rayPayloadInEXT type.", describe_payload(*p_any_hit), describe_payload(*p_closest_hit));
	}
	const StageInterface *stages[] = { p_any_hit, p_closest_hit };
	const char *stage_names[] = { "any-hit", "closest-hit" };
	for (uint32_t i = 0; i < std_size(stages); i++) {
		if (stages[i] == nullptr) {
			continue;
		}
		// An intersection stage that reports no attributes leaves the hit stages' attributes undefined, whatever they declare.
		if (p_intersection != nullptr && p_intersection->attributes_declared && stages[i]->attribute_size != p_intersection->attribute_size) {
			return prefix + vformat("its %s shader declares %s, and its intersection shader reports %d bytes. Direct3D 12 passes the reported hit attributes, so the %s shader must declare and read the intersection shader's hitAttributeEXT type.", stage_names[i], describe_attributes(*stages[i]), p_intersection->attribute_size, stage_names[i]);
		}
		if (p_intersection == nullptr && stages[i]->attribute_size != TRIANGLE_ATTRIBUTE_SIZE) {
			return prefix + vformat("its %s shader declares %s, but a triangle hit has %d bytes of hit attributes (two barycentrics). Direct3D 12 needs the hit shaders of a triangle hit group to declare and read hitAttributeEXT vec2.", stage_names[i], describe_attributes(*stages[i]), TRIANGLE_ATTRIBUTE_SIZE);
		}
	}
	return String();
}

} // namespace RDRTPipeline
