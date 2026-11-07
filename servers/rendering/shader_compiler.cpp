/**************************************************************************/
/*  shader_compiler.cpp                                                   */
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

#include "shader_compiler.h"

#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/shader_types.h"

#define SL ShaderLanguage

static String _mktab(int p_level) {
	return String("\t").repeat(p_level);
}

static String _typestr(SL::DataType p_type) {
	String type = ShaderLanguage::get_datatype_name(p_type);
	if (!RS::get_singleton()->is_low_end() && ShaderLanguage::is_sampler_type(p_type)) {
		type = type.replace("sampler", "texture"); //we use textures instead of samplers in Vulkan GLSL
	}
	return type;
}

static int _get_datatype_alignment(SL::DataType p_type) {
	switch (p_type) {
		case SL::TYPE_VOID:
			return 0;
		case SL::TYPE_BOOL:
			return 4;
		case SL::TYPE_BVEC2:
			return 8;
		case SL::TYPE_BVEC3:
			return 16;
		case SL::TYPE_BVEC4:
			return 16;
		case SL::TYPE_INT:
			return 4;
		case SL::TYPE_IVEC2:
			return 8;
		case SL::TYPE_IVEC3:
			return 16;
		case SL::TYPE_IVEC4:
			return 16;
		case SL::TYPE_UINT:
			return 4;
		case SL::TYPE_UVEC2:
			return 8;
		case SL::TYPE_UVEC3:
			return 16;
		case SL::TYPE_UVEC4:
			return 16;
		case SL::TYPE_FLOAT:
			return 4;
		case SL::TYPE_VEC2:
			return 8;
		case SL::TYPE_VEC3:
			return 16;
		case SL::TYPE_VEC4:
			return 16;
		case SL::TYPE_MAT2:
			return 16;
		case SL::TYPE_MAT3:
			return 16;
		case SL::TYPE_MAT4:
			return 16;
		case SL::TYPE_SAMPLER2D:
			return 16;
		case SL::TYPE_ISAMPLER2D:
			return 16;
		case SL::TYPE_USAMPLER2D:
			return 16;
		case SL::TYPE_SAMPLER2DARRAY:
			return 16;
		case SL::TYPE_ISAMPLER2DARRAY:
			return 16;
		case SL::TYPE_USAMPLER2DARRAY:
			return 16;
		case SL::TYPE_SAMPLER3D:
			return 16;
		case SL::TYPE_ISAMPLER3D:
			return 16;
		case SL::TYPE_USAMPLER3D:
			return 16;
		case SL::TYPE_SAMPLERCUBE:
			return 16;
		case SL::TYPE_SAMPLERCUBEARRAY:
			return 16;
		case SL::TYPE_SAMPLEREXT:
			return 16;
		case SL::TYPE_STRUCT:
			return 0;
		case SL::TYPE_MAX: {
			ERR_FAIL_V(0);
		}
	}

	ERR_FAIL_V(0);
}

static String _interpstr(SL::DataInterpolation p_interp) {
	switch (p_interp) {
		case SL::INTERPOLATION_FLAT:
			return "flat ";
		case SL::INTERPOLATION_SMOOTH:
			return "";
		case SL::INTERPOLATION_DEFAULT:
			return "";
	}
	return "";
}

static String _prestr(SL::DataPrecision p_pres, bool p_force_highp = false) {
	switch (p_pres) {
		case SL::PRECISION_LOWP:
			return "lowp ";
		case SL::PRECISION_MEDIUMP:
			return "mediump ";
		case SL::PRECISION_HIGHP:
			return "highp ";
		case SL::PRECISION_DEFAULT:
			return p_force_highp ? "highp " : "";
	}
	return "";
}

static String _constr(bool p_is_const) {
	if (p_is_const) {
		return "const ";
	}
	return "";
}

static String _qualstr(SL::ArgumentQualifier p_qual) {
	switch (p_qual) {
		case SL::ARGUMENT_QUALIFIER_IN:
			return "";
		case SL::ARGUMENT_QUALIFIER_OUT:
			return "out ";
		case SL::ARGUMENT_QUALIFIER_INOUT:
			return "inout ";
	}
	return "";
}

static String _opstr(SL::Operator p_op) {
	return SL::get_operator_text(p_op);
}

static String _mkid(const String &p_id) {
	String id = "m_" + p_id.replace("__", "_dus_");
	return id.replace("__", "_dus_"); //doubleunderscore is reserved in glsl
}

static String f2sp0(float p_float) {
	String num = String::num_scientific(p_float);
	if (!num.contains_char('.') && !num.contains_char('e')) {
		num += ".0";
	}
	return num;
}

static String get_constant_text(SL::DataType p_type, const Vector<SL::Scalar> &p_values) {
	switch (p_type) {
		case SL::TYPE_BOOL:
			return p_values[0].boolean ? "true" : "false";
		case SL::TYPE_BVEC2:
		case SL::TYPE_BVEC3:
		case SL::TYPE_BVEC4: {
			String text = "bvec" + itos(p_type - SL::TYPE_BOOL + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += p_values[i].boolean ? "true" : "false";
			}
			text += ")";
			return text;
		}

		case SL::TYPE_INT:
			return itos(p_values[0].sint);
		case SL::TYPE_IVEC2:
		case SL::TYPE_IVEC3:
		case SL::TYPE_IVEC4: {
			String text = "ivec" + itos(p_type - SL::TYPE_INT + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += itos(p_values[i].sint);
			}
			text += ")";
			return text;

		} break;
		case SL::TYPE_UINT:
			return itos(p_values[0].uint) + "u";
		case SL::TYPE_UVEC2:
		case SL::TYPE_UVEC3:
		case SL::TYPE_UVEC4: {
			String text = "uvec" + itos(p_type - SL::TYPE_UINT + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += itos(p_values[i].uint) + "u";
			}
			text += ")";
			return text;
		} break;
		case SL::TYPE_FLOAT:
			return f2sp0(p_values[0].real);
		case SL::TYPE_VEC2:
		case SL::TYPE_VEC3:
		case SL::TYPE_VEC4: {
			String text = "vec" + itos(p_type - SL::TYPE_FLOAT + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += f2sp0(p_values[i].real);
			}
			text += ")";
			return text;

		} break;
		case SL::TYPE_MAT2:
		case SL::TYPE_MAT3:
		case SL::TYPE_MAT4: {
			String text = "mat" + itos(p_type - SL::TYPE_MAT2 + 2) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += f2sp0(p_values[i].real);
			}
			text += ")";
			return text;

		} break;
		default:
			ERR_FAIL_V(String());
	}
}

String ShaderCompiler::_get_sampler_name(ShaderLanguage::TextureFilter p_filter, ShaderLanguage::TextureRepeat p_repeat) {
	if (p_filter == ShaderLanguage::FILTER_DEFAULT) {
		ERR_FAIL_COND_V(actions.default_filter == ShaderLanguage::FILTER_DEFAULT, String());
		p_filter = actions.default_filter;
	}
	if (p_repeat == ShaderLanguage::REPEAT_DEFAULT) {
		ERR_FAIL_COND_V(actions.default_repeat == ShaderLanguage::REPEAT_DEFAULT, String());
		p_repeat = actions.default_repeat;
	}
	constexpr const char *name_mapping[] = {
		"SAMPLER_NEAREST_CLAMP",
		"SAMPLER_LINEAR_CLAMP",
		"SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP",
		"SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP",
		"SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP",
		"SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP",
		"SAMPLER_NEAREST_REPEAT",
		"SAMPLER_LINEAR_REPEAT",
		"SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT",
		"SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT",
		"SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT",
		"SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT"
	};
	return String(name_mapping[p_filter + (p_repeat == ShaderLanguage::REPEAT_ENABLE ? ShaderLanguage::FILTER_DEFAULT : 0)]);
}

void ShaderCompiler::_dump_function_deps(const SL::ShaderNode *p_node, const StringName &p_for_func, const HashMap<StringName, String> &p_func_code, String &r_to_add, HashSet<StringName> &added) {
	int fidx = -1;

	for (int i = 0; i < p_node->vfunctions.size(); i++) {
		if (p_node->vfunctions[i].name == p_for_func) {
			fidx = i;
			break;
		}
	}

	ERR_FAIL_COND(fidx == -1);

	Vector<StringName> uses_functions;

	for (const StringName &E : p_node->vfunctions[fidx].uses_function) {
		uses_functions.push_back(E);
	}
	uses_functions.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

	for (int k = 0; k < uses_functions.size(); k++) {
		if (added.has(uses_functions[k])) {
			continue; //was added already
		}

		_dump_function_deps(p_node, uses_functions[k], p_func_code, r_to_add, added);

		SL::FunctionNode *fnode = nullptr;

		for (int i = 0; i < p_node->vfunctions.size(); i++) {
			if (p_node->vfunctions[i].name == uses_functions[k]) {
				fnode = p_node->vfunctions[i].function;
				break;
			}
		}

		ERR_FAIL_NULL(fnode);

		r_to_add += "\n";

		String header;
		if (fnode->return_type == SL::TYPE_STRUCT) {
			header = _mkid(fnode->return_struct_name);
		} else {
			header = _typestr(fnode->return_type);
		}

		if (fnode->return_array_size > 0) {
			header += "[";
			header += itos(fnode->return_array_size);
			header += "]";
		}

		header += " ";
		header += _mkid(fnode->rname);
		header += "(";

		for (int i = 0; i < fnode->arguments.size(); i++) {
			if (i > 0) {
				header += ", ";
			}
			header += _constr(fnode->arguments[i].is_const);
			if (fnode->arguments[i].type == SL::TYPE_STRUCT) {
				header += _qualstr(fnode->arguments[i].qualifier) + _mkid(fnode->arguments[i].struct_name) + " " + _mkid(fnode->arguments[i].name);
			} else {
				header += _qualstr(fnode->arguments[i].qualifier) + _prestr(fnode->arguments[i].precision) + _typestr(fnode->arguments[i].type) + " " + _mkid(fnode->arguments[i].name);
			}
			if (fnode->arguments[i].array_size > 0) {
				header += "[";
				header += itos(fnode->arguments[i].array_size);
				header += "]";
			}
		}

		header += ")\n";
		r_to_add += header;
		r_to_add += p_func_code[uses_functions[k]];

		added.insert(uses_functions[k]);
	}
}

static String _get_global_shader_uniform_from_type_and_index(const String &p_buffer, const String &p_index, ShaderLanguage::DataType p_type, bool p_function_access) {
	const String open = p_function_access ? "(" : "[";
	const String close = p_function_access ? ")" : "]";

	const String v0 = p_buffer + open + p_index + close;

	switch (p_type) {
		case ShaderLanguage::TYPE_BOOL: {
			return "bool(floatBitsToUint(" + v0 + ".x))";
		}
		case ShaderLanguage::TYPE_BVEC2: {
			return "bvec2(floatBitsToUint(" + v0 + ".xy))";
		}
		case ShaderLanguage::TYPE_BVEC3: {
			return "bvec3(floatBitsToUint(" + v0 + ".xyz))";
		}
		case ShaderLanguage::TYPE_BVEC4: {
			return "bvec4(floatBitsToUint(" + v0 + ".xyzw))";
		}
		case ShaderLanguage::TYPE_INT: {
			return "floatBitsToInt(" + v0 + ".x)";
		}
		case ShaderLanguage::TYPE_IVEC2: {
			return "floatBitsToInt(" + v0 + ".xy)";
		}
		case ShaderLanguage::TYPE_IVEC3: {
			return "floatBitsToInt(" + v0 + ".xyz)";
		}
		case ShaderLanguage::TYPE_IVEC4: {
			return "floatBitsToInt(" + v0 + ".xyzw)";
		}
		case ShaderLanguage::TYPE_UINT: {
			return "floatBitsToUint(" + v0 + ".x)";
		}
		case ShaderLanguage::TYPE_UVEC2: {
			return "floatBitsToUint(" + v0 + ".xy)";
		}
		case ShaderLanguage::TYPE_UVEC3: {
			return "floatBitsToUint(" + v0 + ".xyz)";
		}
		case ShaderLanguage::TYPE_UVEC4: {
			return "floatBitsToUint(" + v0 + ".xyzw)";
		}
		case ShaderLanguage::TYPE_FLOAT: {
			return "(" + v0 + ".x)";
		}
		case ShaderLanguage::TYPE_VEC2: {
			return "(" + v0 + ".xy)";
		}
		case ShaderLanguage::TYPE_VEC3: {
			return "(" + v0 + ".xyz)";
		}
		case ShaderLanguage::TYPE_VEC4: {
			return "(" + v0 + ".xyzw)";
		}
		case ShaderLanguage::TYPE_MAT2: {
			const String v1 = p_buffer + open + p_index + "+1u" + close;
			return "mat2(" + v0 + ".xy," + v1 + ".xy)";
		}
		case ShaderLanguage::TYPE_MAT3: {
			const String v1 = p_buffer + open + p_index + "+1u" + close;
			const String v2 = p_buffer + open + p_index + "+2u" + close;

			return "mat3(" + v0 + ".xyz," + v1 + ".xyz," + v2 + ".xyz)";
		}
		case ShaderLanguage::TYPE_MAT4: {
			const String v1 = p_buffer + open + p_index + "+1u" + close;
			const String v2 = p_buffer + open + p_index + "+2u" + close;
			const String v3 = p_buffer + open + p_index + "+3u" + close;
			return "mat4(" + v0 + ".xyzw," + v1 + ".xyzw," + v2 + ".xyzw," + v3 + ".xyzw)";
		}
		default: {
			ERR_FAIL_V("void");
		}
	}
}

String ShaderCompiler::_dump_node_code(const SL::Node *p_node, int p_level, GeneratedCode &r_gen_code, IdentifierActions &p_actions, const DefaultIdentifierActions &p_default_actions, bool p_assigning, bool p_use_scope) {
	String code;

	switch (p_node->type) {
		case SL::Node::NODE_TYPE_SHADER: {
			SL::ShaderNode *pnode = (SL::ShaderNode *)p_node;

			// Render modes.

			for (int i = 0; i < pnode->render_modes.size(); i++) {
				if (p_default_actions.render_mode_defines.has(pnode->render_modes[i]) && !used_rmode_defines.has(pnode->render_modes[i])) {
					r_gen_code.defines.push_back(p_default_actions.render_mode_defines[pnode->render_modes[i]]);
					used_rmode_defines.insert(pnode->render_modes[i]);
				}

				if (p_actions.render_mode_flags.has(pnode->render_modes[i])) {
					*p_actions.render_mode_flags[pnode->render_modes[i]] = true;
				}

				if (p_actions.render_mode_values.has(pnode->render_modes[i])) {
					Pair<int *, int> &p = p_actions.render_mode_values[pnode->render_modes[i]];
					*p.first = p.second;
				}
			}

			// Stencil modes.

			for (int i = 0; i < pnode->stencil_modes.size(); i++) {
				if (p_default_actions.stencil_mode_defines.has(pnode->stencil_modes[i]) && !used_smode_defines.has(pnode->stencil_modes[i])) {
					r_gen_code.defines.push_back(p_default_actions.stencil_mode_defines[pnode->stencil_modes[i]]);
					used_smode_defines.insert(pnode->stencil_modes[i]);
				}

				if (p_actions.stencil_mode_values.has(pnode->stencil_modes[i])) {
					Pair<int *, int> &p = p_actions.stencil_mode_values[pnode->stencil_modes[i]];
					*p.first = p.second;
				}
			}

			// Stencil reference value.

			if (p_actions.stencil_reference && pnode->stencil_reference != -1) {
				*p_actions.stencil_reference = pnode->stencil_reference;
			}

			// structs

			for (int i = 0; i < pnode->vstructs.size(); i++) {
				SL::StructNode *st = pnode->vstructs[i].shader_struct;
				String struct_code;

				struct_code += "struct ";
				struct_code += _mkid(pnode->vstructs[i].name);
				struct_code += " ";
				struct_code += "{\n";
				for (SL::MemberNode *m : st->members) {
					if (m->datatype == SL::TYPE_STRUCT) {
						struct_code += _mkid(m->struct_name);
					} else {
						struct_code += _prestr(m->precision);
						struct_code += _typestr(m->datatype);
					}
					struct_code += " ";
					struct_code += _mkid(m->name);
					if (m->array_size > 0) {
						struct_code += "[";
						struct_code += itos(m->array_size);
						struct_code += "]";
					}
					struct_code += ";\n";
				}
				struct_code += "}";
				struct_code += ";\n";

				for (int j = 0; j < STAGE_MAX; j++) {
					r_gen_code.stage_globals[j] += struct_code;
				}
			}

			int max_texture_uniforms = 0;
			int max_uniforms = 0;

			for (const KeyValue<StringName, SL::ShaderNode::Uniform> &E : pnode->uniforms) {
				if (SL::is_sampler_type(E.value.type)) {
					if (E.value.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
						continue; // Don't create uniforms in the generated code for these.
					}
					max_texture_uniforms++;
				} else {
					if (E.value.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
						continue; // Instances are indexed directly, don't need index uniforms.
					}

					max_uniforms++;
				}
			}

			r_gen_code.texture_uniforms.resize(max_texture_uniforms);

			Vector<int> uniform_sizes;
			Vector<int> uniform_alignments;
			Vector<StringName> uniform_defines;
			uniform_sizes.resize(max_uniforms);
			uniform_alignments.resize(max_uniforms);
			uniform_defines.resize(max_uniforms);
			bool uses_uniforms = false;

			Vector<StringName> uniform_names;

			for (const KeyValue<StringName, SL::ShaderNode::Uniform> &E : pnode->uniforms) {
				uniform_names.push_back(E.key);
			}

			uniform_names.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

			for (int k = 0; k < uniform_names.size(); k++) {
				const StringName &uniform_name = uniform_names[k];
				const SL::ShaderNode::Uniform &uniform = pnode->uniforms[uniform_name];

				String ucode;

				if (uniform.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
					//insert, but don't generate any code.
					p_actions.uniforms->insert(uniform_name, uniform);
					continue; // Instances are indexed directly, don't need index uniforms.
				}

				if (uniform.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
					continue; // Don't create uniforms in the generated code for these.
				}

				if (SL::is_sampler_type(uniform.type)) {
					// Texture layouts are different for OpenGL GLSL and Vulkan GLSL
					if (!RS::get_singleton()->is_low_end()) {
						ucode = "layout(set = " + itos(actions.texture_layout_set) + ", binding = " + itos(actions.base_texture_binding_index + uniform.texture_binding) + ") ";
					}
					ucode += "uniform ";
				}

				bool is_buffer_global = !SL::is_sampler_type(uniform.type) && uniform.scope == SL::ShaderNode::Uniform::SCOPE_GLOBAL;

				if (is_buffer_global) {
					//this is an integer to index the global table
					ucode += _typestr(ShaderLanguage::TYPE_UINT);
				} else {
					ucode += _prestr(uniform.precision, ShaderLanguage::is_float_type(uniform.type));
					ucode += _typestr(uniform.type);
				}

				ucode += " " + _mkid(uniform_name);
				if (uniform.array_size > 0) {
					ucode += "[";
					ucode += itos(uniform.array_size);
					ucode += "]";
				}
				ucode += ";\n";
				if (SL::is_sampler_type(uniform.type)) {
					for (int j = 0; j < STAGE_MAX; j++) {
						r_gen_code.stage_globals[j] += ucode;
					}

					GeneratedCode::Texture texture;
					texture.name = uniform_name;
					texture.hint = uniform.hint;
					texture.type = uniform.type;
					texture.use_color = uniform.use_color;
					texture.filter = uniform.filter;
					texture.repeat = uniform.repeat;
					texture.global = uniform.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL;
					texture.array_size = uniform.array_size;
					if (texture.global) {
						r_gen_code.uses_global_textures = true;
					}

					r_gen_code.texture_uniforms.write[uniform.texture_order] = texture;
				} else {
					if (!uses_uniforms) {
						uses_uniforms = true;
					}
					uniform_defines.write[uniform.order] = ucode;
					if (is_buffer_global) {
						//globals are indices into the global table
						uniform_sizes.write[uniform.order] = ShaderLanguage::get_datatype_size(ShaderLanguage::TYPE_UINT);
						uniform_alignments.write[uniform.order] = _get_datatype_alignment(ShaderLanguage::TYPE_UINT);
					} else {
						// The following code enforces a 16-byte alignment of uniform arrays.
						if (uniform.array_size > 0) {
							int size = ShaderLanguage::get_datatype_size(uniform.type) * uniform.array_size;
							int m = (16 * uniform.array_size);
							if ((size % m) != 0) {
								size += m - (size % m);
							}
							uniform_sizes.write[uniform.order] = size;
							uniform_alignments.write[uniform.order] = 16;
						} else {
							uniform_sizes.write[uniform.order] = ShaderLanguage::get_datatype_size(uniform.type);
							uniform_alignments.write[uniform.order] = _get_datatype_alignment(uniform.type);
						}
					}
				}

				p_actions.uniforms->insert(uniform_name, uniform);
			}

			for (int i = 0; i < max_uniforms; i++) {
				r_gen_code.uniforms += uniform_defines[i];
			}

			// add up
			int offset = 0;
			for (int i = 0; i < uniform_sizes.size(); i++) {
				int align = offset % uniform_alignments[i];

				if (align != 0) {
					offset += uniform_alignments[i] - align;
				}

				r_gen_code.uniform_offsets.push_back(offset);

				offset += uniform_sizes[i];
			}

			r_gen_code.uniform_total_size = offset;

			if (r_gen_code.uniform_total_size % 16 != 0) { //UBO sizes must be multiples of 16
				r_gen_code.uniform_total_size += 16 - (r_gen_code.uniform_total_size % 16);
			}

			uint32_t index = p_default_actions.base_varying_index;

			List<Pair<StringName, SL::ShaderNode::Varying>> var_frag_to_light;

			Vector<StringName> varying_names;

			for (const KeyValue<StringName, SL::ShaderNode::Varying> &E : pnode->varyings) {
				varying_names.push_back(E.key);
			}

			varying_names.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

			String vert_varying_code;
			String frag_varying_code;

			for (int k = 0; k < varying_names.size(); k++) {
				const StringName &varying_name = varying_names[k];
				const SL::ShaderNode::Varying &varying = pnode->varyings[varying_name];

				if (varying.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT) {
					var_frag_to_light.push_back(Pair<StringName, SL::ShaderNode::Varying>(varying_name, varying));
					fragment_varyings.insert(varying_name);
					continue;
				}
				if (varying.type < SL::TYPE_INT) {
					continue; // Ignore boolean types to prevent crashing (if varying is just declared).
				}

				String vcode;
				String interp_mode = _interpstr(varying.interpolation);
				vcode += _prestr(varying.precision, ShaderLanguage::is_float_type(varying.type));
				vcode += _typestr(varying.type);
				vcode += " " + _mkid(varying_name);
				uint32_t inc = varying.get_size();

				if (varying.array_size > 0) {
					vcode += "[";
					vcode += itos(varying.array_size);
					vcode += "]";
				}

				vcode += ";\n";
				// GLSL ES 3.0 does not allow layout qualifiers for varyings
				if (!RS::get_singleton()->is_low_end()) {
					vert_varying_code += "layout(location=" + itos(index) + ") ";
					frag_varying_code += "layout(location=" + itos(index) + ") ";
				}
				vert_varying_code += interp_mode + "out " + vcode;
				frag_varying_code += interp_mode + "in " + vcode;

				index += inc;
			}

			if (!vert_varying_code.is_empty()) {
				if (p_actions.visibility_resolve) {
					r_gen_code.stage_globals[STAGE_VERTEX] += "#ifndef MODE_VISIBILITY_RESOLVE\n" + vert_varying_code + "#endif\n";
					r_gen_code.stage_globals[STAGE_FRAGMENT] += "#ifndef MODE_VISIBILITY_RESOLVE\n" + frag_varying_code + "#endif\n";
				} else {
					r_gen_code.stage_globals[STAGE_VERTEX] += vert_varying_code;
					r_gen_code.stage_globals[STAGE_FRAGMENT] += frag_varying_code;
				}
			}

			if (var_frag_to_light.size() > 0) {
				String gcode = "\n\nstruct {\n";
				for (const Pair<StringName, SL::ShaderNode::Varying> &E : var_frag_to_light) {
					gcode += "\t" + _prestr(E.second.precision) + _typestr(E.second.type) + " " + _mkid(E.first);
					if (E.second.array_size > 0) {
						gcode += "[";
						gcode += itos(E.second.array_size);
						gcode += "]";
					}
					gcode += ";\n";
				}
				gcode += "} frag_to_light;\n";
				r_gen_code.stage_globals[STAGE_FRAGMENT] += gcode;
			}

			for (int i = 0; i < pnode->vconstants.size(); i++) {
				const SL::ShaderNode::Constant &cnode = pnode->vconstants[i];
				String gcode;
				gcode += _constr(true);
				gcode += _prestr(cnode.precision, ShaderLanguage::is_float_type(cnode.type));
				if (cnode.type == SL::TYPE_STRUCT) {
					gcode += _mkid(cnode.struct_name);
				} else {
					gcode += _typestr(cnode.type);
				}
				gcode += " " + _mkid(String(cnode.name));
				if (cnode.array_size > 0) {
					gcode += "[";
					gcode += itos(cnode.array_size);
					gcode += "]";
				}
				gcode += "=";
				gcode += _dump_node_code(cnode.initializer, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				gcode += ";\n";
				for (int j = 0; j < STAGE_MAX; j++) {
					r_gen_code.stage_globals[j] += gcode;
				}
			}

			HashMap<StringName, String> function_code;

			//code for functions
			for (int i = 0; i < pnode->vfunctions.size(); i++) {
				SL::FunctionNode *fnode = pnode->vfunctions[i].function;
				function = fnode;
				current_func_name = fnode->name;
				function_code[fnode->name] = _dump_node_code(fnode->body, p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				function = nullptr;
			}

			//place functions in actual code

			HashSet<StringName> added_funcs_per_stage[STAGE_MAX];
			String stage_func_code[STAGE_MAX];

			for (int i = 0; i < pnode->vfunctions.size(); i++) {
				SL::FunctionNode *fnode = pnode->vfunctions[i].function;

				function = fnode;

				current_func_name = fnode->name;

				if (p_actions.entry_point_stages.has(fnode->name)) {
					Stage stage = p_actions.entry_point_stages[fnode->name];
					_dump_function_deps(pnode, fnode->name, function_code, stage_func_code[stage], added_funcs_per_stage[stage]);
					r_gen_code.code[fnode->name] = function_code[fnode->name];
				}

				function = nullptr;
			}

			for (int j = 0; j < STAGE_MAX; j++) {
				if (!stage_func_code[j].is_empty()) {
					if (p_actions.visibility_resolve && (j == STAGE_VERTEX || j == STAGE_FRAGMENT)) {
						r_gen_code.stage_globals[j] += "#ifndef MODE_VISIBILITY_RESOLVE\n" + stage_func_code[j] + "\n#endif\n";
					} else {
						r_gen_code.stage_globals[j] += stage_func_code[j];
					}
				}
			}

			//code+=dump_node_code(pnode->body,p_level);
		} break;
		case SL::Node::NODE_TYPE_STRUCT: {
		} break;
		case SL::Node::NODE_TYPE_FUNCTION: {
		} break;
		case SL::Node::NODE_TYPE_BLOCK: {
			SL::BlockNode *bnode = (SL::BlockNode *)p_node;

			//variables
			if (!bnode->single_statement) {
				code += _mktab(p_level - 1) + "{\n";
			}

			int i = 0;
			for (List<ShaderLanguage::Node *>::ConstIterator itr = bnode->statements.begin(); itr != bnode->statements.end(); ++itr, ++i) {
				String scode = _dump_node_code(*itr, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);

				if ((*itr)->type == SL::Node::NODE_TYPE_CONTROL_FLOW || bnode->single_statement) {
					code += scode; //use directly
					if (bnode->use_comma_between_statements && i + 1 < bnode->statements.size()) {
						code += ",";
					}
				} else {
					code += _mktab(p_level) + scode + ";\n";
				}
			}
			if (!bnode->single_statement) {
				code += _mktab(p_level - 1) + "}\n";
			}

		} break;
		case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
			SL::VariableDeclarationNode *vdnode = (SL::VariableDeclarationNode *)p_node;

			String declaration;
			declaration += _constr(vdnode->is_const);
			if (vdnode->datatype == SL::TYPE_STRUCT) {
				declaration += _mkid(vdnode->struct_name);
			} else {
				declaration += _prestr(vdnode->precision) + _typestr(vdnode->datatype);
			}
			declaration += " ";
			for (int i = 0; i < vdnode->declarations.size(); i++) {
				bool is_array = vdnode->declarations[i].size > 0;
				if (i > 0) {
					declaration += ",";
				}
				declaration += _mkid(vdnode->declarations[i].name);
				if (is_array) {
					declaration += "[";
					if (vdnode->declarations[i].size_expression != nullptr) {
						declaration += _dump_node_code(vdnode->declarations[i].size_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					} else {
						declaration += itos(vdnode->declarations[i].size);
					}
					declaration += "]";
				}

				if (!is_array || vdnode->declarations[i].single_expression) {
					if (!vdnode->declarations[i].initializer.is_empty()) {
						declaration += "=";
						declaration += _dump_node_code(vdnode->declarations[i].initializer[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					}
				} else {
					int size = vdnode->declarations[i].initializer.size();
					if (size > 0) {
						declaration += "=";
						if (vdnode->datatype == SL::TYPE_STRUCT) {
							declaration += _mkid(vdnode->struct_name);
						} else {
							declaration += _typestr(vdnode->datatype);
						}
						declaration += "[";
						declaration += itos(size);
						declaration += "]";
						declaration += "(";
						for (int j = 0; j < size; j++) {
							if (j > 0) {
								declaration += ",";
							}
							declaration += _dump_node_code(vdnode->declarations[i].initializer[j], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
						}
						declaration += ")";
					}
				}
			}

			code += declaration;
		} break;
		case SL::Node::NODE_TYPE_VARIABLE: {
			SL::VariableNode *vnode = (SL::VariableNode *)p_node;
			bool use_fragment_varying = false;

			if (!vnode->is_local && !(p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX)) {
				if (p_assigning) {
					if (shader->varyings.has(vnode->name)) {
						use_fragment_varying = true;
					}
				} else {
					if (fragment_varyings.has(vnode->name)) {
						use_fragment_varying = true;
					}
				}
			}

			if (p_assigning && p_actions.write_flag_pointers.has(vnode->name)) {
				*p_actions.write_flag_pointers[vnode->name] = true;
			}

			if (p_default_actions.usage_defines.has(vnode->name) && !used_name_defines.has(vnode->name)) {
				String define = p_default_actions.usage_defines[vnode->name];
				if (define.begins_with("@")) {
					define = p_default_actions.usage_defines[define.substr(1)];
				}
				r_gen_code.defines.push_back(define);
				used_name_defines.insert(vnode->name);
			}

			if (p_actions.usage_flag_pointers.has(vnode->name) && !used_flag_pointers.has(vnode->name)) {
				*p_actions.usage_flag_pointers[vnode->name] = true;
				used_flag_pointers.insert(vnode->name);
			}

			if (p_default_actions.renames.has(vnode->name)) {
				code = p_default_actions.renames[vnode->name];
			} else {
				bool param_found = false;
				if (function) {
					for (const SL::FunctionNode::Argument &argument : function->arguments) {
						if (argument.name == vnode->name) {
							param_found = true;
							break;
						}
					}
				}
				if (!param_found && shader->uniforms.has(vnode->name)) {
					//its a uniform!
					const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[vnode->name];
					if (u.is_texture()) {
						StringName name;
						if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
							name = "color_buffer";
							if (u.filter >= ShaderLanguage::FILTER_NEAREST_MIPMAP) {
								r_gen_code.uses_screen_texture_mipmaps = true;
							}
							r_gen_code.uses_screen_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
							name = "normal_roughness_buffer";
							r_gen_code.uses_normal_roughness_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
							name = "depth_buffer";
							r_gen_code.uses_depth_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE0) {
							name = "source0";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE1) {
							name = "source1";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE2) {
							name = "source2";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
							name = "source3";
						} else {
							name = _mkid(vnode->name); //texture, use as is
						}

						code = name;
					} else {
						//a scalar or vector
						if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
							code = actions.base_uniform_string + _mkid(vnode->name); //texture, use as is
							//global variable, this means the code points to an index to the global table
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
							//instance variable, index it as such
							code = "(" + p_default_actions.instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else {
							//regular uniform, index from UBO
							code = actions.base_uniform_string + _mkid(vnode->name);
						}
					}

				} else {
					if (use_fragment_varying) {
						code = "frag_to_light.";
					}
					code += _mkid(vnode->name); //its something else (local var most likely) use as is
				}
			}

			if (vnode->name == time_name) {
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX) {
					r_gen_code.uses_vertex_time = true;
				}
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_FRAGMENT) {
					r_gen_code.uses_fragment_time = true;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
			SL::ArrayConstructNode *acnode = (SL::ArrayConstructNode *)p_node;
			int sz = acnode->initializer.size();
			if (acnode->datatype == SL::TYPE_STRUCT) {
				code += _mkid(acnode->struct_name);
			} else {
				code += _typestr(acnode->datatype);
			}
			code += "[";
			code += itos(acnode->initializer.size());
			code += "]";
			code += "(";
			for (int i = 0; i < sz; i++) {
				code += _dump_node_code(acnode->initializer[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				if (i != sz - 1) {
					code += ", ";
				}
			}
			code += ")";
		} break;
		case SL::Node::NODE_TYPE_ARRAY: {
			SL::ArrayNode *anode = (SL::ArrayNode *)p_node;
			bool use_fragment_varying = false;

			if (!anode->is_local && !(p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX)) {
				if (anode->assign_expression != nullptr && shader->varyings.has(anode->name)) {
					use_fragment_varying = true;
				} else {
					if (p_assigning) {
						if (shader->varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					} else {
						if (fragment_varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					}
				}
			}

			if (p_assigning && p_actions.write_flag_pointers.has(anode->name)) {
				*p_actions.write_flag_pointers[anode->name] = true;
			}

			if (p_default_actions.usage_defines.has(anode->name) && !used_name_defines.has(anode->name)) {
				String define = p_default_actions.usage_defines[anode->name];
				if (define.begins_with("@")) {
					define = p_default_actions.usage_defines[define.substr(1)];
				}
				r_gen_code.defines.push_back(define);
				used_name_defines.insert(anode->name);
			}

			if (p_actions.usage_flag_pointers.has(anode->name) && !used_flag_pointers.has(anode->name)) {
				*p_actions.usage_flag_pointers[anode->name] = true;
				used_flag_pointers.insert(anode->name);
			}

			if (p_default_actions.renames.has(anode->name)) {
				code = p_default_actions.renames[anode->name];
			} else {
				bool param_found = false;
				if (function) {
					for (const SL::FunctionNode::Argument &argument : function->arguments) {
						if (argument.name == anode->name) {
							param_found = true;
							break;
						}
					}
				}
				if (!param_found && shader->uniforms.has(anode->name)) {
					//its a uniform!
					const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[anode->name];
					if (u.is_texture()) {
						code = _mkid(anode->name); //texture, use as is
					} else {
						//a scalar or vector
						if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
							code = actions.base_uniform_string + _mkid(anode->name); //texture, use as is
							//global variable, this means the code points to an index to the global table
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
							//instance variable, index it as such
							code = "(" + p_default_actions.instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else {
							//regular uniform, index from UBO
							code = actions.base_uniform_string + _mkid(anode->name);
						}
					}
				} else {
					if (use_fragment_varying) {
						code = "frag_to_light.";
					}
					code += _mkid(anode->name);
				}
			}

			if (anode->call_expression != nullptr) {
				code += ".";
				code += _dump_node_code(anode->call_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning, false);
			} else if (anode->index_expression != nullptr) {
				code += "[";
				code += _dump_node_code(anode->index_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += "]";
			} else if (anode->assign_expression != nullptr) {
				code += "=";
				code += _dump_node_code(anode->assign_expression, p_level, r_gen_code, p_actions, p_default_actions, true, false);
			}

			if (anode->name == time_name) {
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX) {
					r_gen_code.uses_vertex_time = true;
				}
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_FRAGMENT) {
					r_gen_code.uses_fragment_time = true;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_CONSTANT: {
			SL::ConstantNode *cnode = (SL::ConstantNode *)p_node;

			if (cnode->array_size == 0) {
				return get_constant_text(cnode->datatype, cnode->values);
			} else {
				if (cnode->get_datatype() == SL::TYPE_STRUCT) {
					code += _mkid(cnode->struct_name);
				} else {
					code += _typestr(cnode->datatype);
				}
				code += "[";
				code += itos(cnode->array_size);
				code += "]";
				code += "(";
				for (int i = 0; i < cnode->array_size; i++) {
					if (i > 0) {
						code += ",";
					} else {
						code += "";
					}
					code += _dump_node_code(cnode->array_declarations[0].initializer[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				}
				code += ")";
			}

		} break;
		case SL::Node::NODE_TYPE_OPERATOR: {
			SL::OperatorNode *onode = (SL::OperatorNode *)p_node;

			switch (onode->op) {
				case SL::OP_ASSIGN:
				case SL::OP_ASSIGN_ADD:
				case SL::OP_ASSIGN_SUB:
				case SL::OP_ASSIGN_MUL:
				case SL::OP_ASSIGN_DIV:
				case SL::OP_ASSIGN_SHIFT_LEFT:
				case SL::OP_ASSIGN_SHIFT_RIGHT:
				case SL::OP_ASSIGN_MOD:
				case SL::OP_ASSIGN_BIT_AND:
				case SL::OP_ASSIGN_BIT_OR:
				case SL::OP_ASSIGN_BIT_XOR:
					code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, true) + _opstr(onode->op) + _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					break;
				case SL::OP_BIT_INVERT:
				case SL::OP_NEGATE:
				case SL::OP_NOT:
				case SL::OP_DECREMENT:
				case SL::OP_INCREMENT: {
					const String node_code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);

					if (onode->op == SL::OP_NEGATE && node_code.begins_with("-")) { // To prevent writing unary minus twice.
						code = node_code;
					} else {
						code = _opstr(onode->op) + node_code;
					}

				} break;
				case SL::OP_POST_DECREMENT:
				case SL::OP_POST_INCREMENT:
					code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + _opstr(onode->op);
					break;
				case SL::OP_CALL:
				case SL::OP_STRUCT:
				case SL::OP_CONSTRUCT: {
					ERR_FAIL_COND_V(onode->arguments[0]->type != SL::Node::NODE_TYPE_VARIABLE, String());
					const SL::VariableNode *vnode = static_cast<const SL::VariableNode *>(onode->arguments[0]);
					const SL::FunctionNode *func = nullptr;
					const bool is_internal_func = internal_functions.has(vnode->name);

					if (!is_internal_func) {
						for (int i = 0; i < shader->vfunctions.size(); i++) {
							if (shader->vfunctions[i].name == vnode->name) {
								func = shader->vfunctions[i].function;
								break;
							}
						}
					}

					bool is_texture_func = false;
					bool is_screen_texture = false;
					bool is_radiance_texture = false;
					bool texture_func_no_uv = false;
					bool texture_func_returns_data = false;
					bool texture_func_simple = false;

					if (onode->op == SL::OP_STRUCT) {
						code += _mkid(vnode->name);
					} else if (onode->op == SL::OP_CONSTRUCT) {
						code += String(vnode->name);
					} else {
						if (p_actions.usage_flag_pointers.has(vnode->name) && !used_flag_pointers.has(vnode->name)) {
							*p_actions.usage_flag_pointers[vnode->name] = true;
							used_flag_pointers.insert(vnode->name);
						}

						if (is_internal_func) {
							code += vnode->name;
							is_texture_func = texture_functions.has(vnode->name);
							texture_func_no_uv = (vnode->name == "textureSize" || vnode->name == "textureQueryLevels");
							texture_func_returns_data = texture_func_no_uv || vnode->name == "textureQueryLod";
							texture_func_simple = vnode->name == "texture";
						} else if (p_default_actions.renames.has(vnode->name)) {
							code += p_default_actions.renames[vnode->name];
						} else {
							code += _mkid(vnode->rname);
						}
					}

					code += "(";

					// if color backbuffer, depth backbuffer or normal roughness texture is used,
					// we will add logic to automatically switch between
					// sampler2D and sampler2D array and vec2 UV and vec3 UV.
					bool multiview_uv_needed = false;
					bool is_normal_roughness_texture = false;

					for (int i = 1; i < onode->arguments.size(); i++) {
						if (i > 1) {
							code += ", ";
						}

						bool is_out_qualifier = false;
						if (is_internal_func) {
							is_out_qualifier = SL::is_builtin_func_out_parameter(vnode->name, i - 1);
						} else if (func != nullptr) {
							const SL::ArgumentQualifier qualifier = func->arguments[i - 1].qualifier;
							is_out_qualifier = qualifier == SL::ARGUMENT_QUALIFIER_OUT || qualifier == SL::ARGUMENT_QUALIFIER_INOUT;
						}

						if (is_out_qualifier) {
							StringName name;
							bool found = false;
							{
								const SL::Node *node = onode->arguments[i];

								bool done = false;
								do {
									switch (node->type) {
										case SL::Node::NODE_TYPE_VARIABLE: {
											name = static_cast<const SL::VariableNode *>(node)->name;
											done = true;
											found = true;
										} break;
										case SL::Node::NODE_TYPE_MEMBER: {
											node = static_cast<const SL::MemberNode *>(node)->owner;
										} break;
										default: {
											done = true;
										} break;
									}
								} while (!done);
							}

							if (found && p_actions.write_flag_pointers.has(name)) {
								*p_actions.write_flag_pointers[name] = true;
							}
						}

						String node_code = _dump_node_code(onode->arguments[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
						if (is_texture_func && i == 1) {
							// If we're doing a texture lookup we need to check our texture argument
							StringName texture_uniform;
							bool correct_texture_uniform = false;

							switch (onode->arguments[i]->type) {
								case SL::Node::NODE_TYPE_VARIABLE: {
									const SL::VariableNode *varnode = static_cast<const SL::VariableNode *>(onode->arguments[i]);
									texture_uniform = varnode->name;
									correct_texture_uniform = true;
								} break;
								case SL::Node::NODE_TYPE_ARRAY: {
									const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(onode->arguments[i]);
									texture_uniform = anode->name;
									correct_texture_uniform = true;
								} break;
								default:
									break;
							}

							if (correct_texture_uniform && !RS::get_singleton()->is_low_end()) {
								// Need to map from texture to sampler in order to sample when using Vulkan GLSL.
								String sampler_name;
								bool is_depth_texture = false;

								if (actions.custom_samplers.has(texture_uniform)) {
									sampler_name = actions.custom_samplers[texture_uniform];
								} else {
									if (shader->uniforms.has(texture_uniform)) {
										const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[texture_uniform];
										if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											is_screen_texture = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											is_depth_texture = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											is_normal_roughness_texture = true;
										}
										sampler_name = _get_sampler_name(u.filter, u.repeat);
									} else {
										bool found = false;

										for (int j = 0; j < function->arguments.size(); j++) {
											if (function->arguments[j].name == texture_uniform) {
												if (function->arguments[j].tex_builtin_check) {
													ERR_CONTINUE(!actions.custom_samplers.has(function->arguments[j].tex_builtin));
													sampler_name = actions.custom_samplers[function->arguments[j].tex_builtin];
													found = true;
													break;
												}
												if (function->arguments[j].tex_argument_check) {
													if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
														is_screen_texture = true;
													} else if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
														is_depth_texture = true;
													} else if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
														is_normal_roughness_texture = true;
													}
													sampler_name = _get_sampler_name(function->arguments[j].tex_argument_filter, function->arguments[j].tex_argument_repeat);
													found = true;
													break;
												}
											}
										}
										if (!found) {
											//function was most likely unused, so use anything (compiler will remove it anyway)
											sampler_name = _get_sampler_name(ShaderLanguage::FILTER_DEFAULT, ShaderLanguage::REPEAT_DEFAULT);
										}
									}
								}

								if (texture_uniform == SNAME("RADIANCE")) {
									is_radiance_texture = true;
								}

								String data_type_name = "";
								if (actions.check_multiview_samplers && (is_screen_texture || is_depth_texture || is_normal_roughness_texture)) {
									data_type_name = "multiviewSampler";
									multiview_uv_needed = true;
								} else if (is_radiance_texture) {
									// We need to use an explicit level of detail to avoid mip mapping artifacts caused by the octahedral discontinuity.
									if (texture_func_simple) {
										code = code.replace("texture(", "textureLod(");
									}
									data_type_name = "sampler2D";
								} else {
									data_type_name = ShaderLanguage::get_datatype_name(onode->arguments[i]->get_datatype());
								}

								code += data_type_name + "(" + node_code + ", " + sampler_name + ")";
							} else if (correct_texture_uniform && RS::get_singleton()->is_low_end()) {
								// Texture function on low end hardware (i.e. OpenGL).

								if (shader->uniforms.has(texture_uniform)) {
									const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[texture_uniform];
									if (actions.check_multiview_samplers) {
										if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											multiview_uv_needed = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											multiview_uv_needed = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											multiview_uv_needed = true;
										}
									}
									if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
										is_screen_texture = true;
									}
								}

								code += node_code;
							} else {
								code += node_code;
							}
						} else if (multiview_uv_needed && !texture_func_no_uv && i == 2) {
							// UV coordinate after using color, depth or normal roughness texture.
							node_code = "multiview_uv(" + node_code + ".xy)";

							code += node_code;
						} else if (is_radiance_texture && !texture_func_no_uv && i == 2) {
							node_code = "vec3_to_oct_with_border(" + node_code + ", params.border_size)";

							// Need an explicit level of detail if one isn't provided by the user.
							if (texture_func_simple && onode->arguments.size() == 3) {
								node_code += ", 0.0";
							}

							code += node_code;
						} else {
							code += node_code;
						}
					}
					code += ")";
					if (is_screen_texture && !texture_func_returns_data && actions.apply_luminance_multiplier) {
						if (RS::get_singleton()->is_low_end()) {
							code = "(" + code + " / vec4(vec3(scene_data_block.data.luminance_multiplier), 1.0))";
						} else {
							code = "(" + code + " * vec4(vec3(sc_luminance_multiplier()), 1.0))";
						}
					}
					if (is_normal_roughness_texture && !texture_func_returns_data) {
						code = "normal_roughness_compatibility(" + code + ")";
					}
				} break;
				case SL::OP_INDEX: {
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "[";
					code += _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "]";

				} break;
				case SL::OP_SELECT_IF: {
					code += "(";
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "?";
					code += _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += ":";
					code += _dump_node_code(onode->arguments[2], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += ")";

				} break;
				case SL::OP_EMPTY: {
					// Semicolon (or empty statement) - ignored.
				} break;

				default: {
					if (p_use_scope) {
						code += "(";
					}
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + " " + _opstr(onode->op) + " " + _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					if (p_use_scope) {
						code += ")";
					}
					break;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_CONTROL_FLOW: {
			SL::ControlFlowNode *cfnode = (SL::ControlFlowNode *)p_node;
			if (cfnode->flow_op == SL::FLOW_OP_IF) {
				code += _mktab(p_level) + "if (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				if (cfnode->blocks.size() == 2) {
					code += _mktab(p_level) + "else\n";
					code += _dump_node_code(cfnode->blocks[1], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				}
			} else if (cfnode->flow_op == SL::FLOW_OP_SWITCH) {
				code += _mktab(p_level) + "switch (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_CASE) {
				code += _mktab(p_level) + "case " + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ":\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_DEFAULT) {
				code += _mktab(p_level) + "default:\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_DO) {
				code += _mktab(p_level) + "do";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += _mktab(p_level) + "while (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ");";
			} else if (cfnode->flow_op == SL::FLOW_OP_WHILE) {
				code += _mktab(p_level) + "while (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_FOR) {
				String left = _dump_node_code(cfnode->blocks[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				String middle = _dump_node_code(cfnode->blocks[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				String right = _dump_node_code(cfnode->blocks[2], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += _mktab(p_level) + "for (" + left + ";" + middle + ";" + right + ")\n";
				code += _dump_node_code(cfnode->blocks[3], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);

			} else if (cfnode->flow_op == SL::FLOW_OP_RETURN) {
				if (cfnode->expressions.size()) {
					code = "return " + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ";";
				} else {
					code = "return;";
				}
			} else if (cfnode->flow_op == SL::FLOW_OP_DISCARD) {
				if (p_default_actions.usage_defines.has("DISCARD") && !used_name_defines.has("DISCARD")) {
					String define = p_default_actions.usage_defines["DISCARD"];
					if (define.begins_with("@")) {
						define = p_default_actions.usage_defines[define.substr(1)];
					}
					r_gen_code.defines.push_back(define);
					used_name_defines.insert("DISCARD");
				}

				if (p_actions.usage_flag_pointers.has("DISCARD") && !used_flag_pointers.has("DISCARD")) {
					*p_actions.usage_flag_pointers["DISCARD"] = true;
					used_flag_pointers.insert("DISCARD");
				}

				code = "discard;";
			} else if (cfnode->flow_op == SL::FLOW_OP_CONTINUE) {
				code = "continue;";
			} else if (cfnode->flow_op == SL::FLOW_OP_BREAK) {
				code = "break;";
			}

		} break;
		case SL::Node::NODE_TYPE_MEMBER: {
			SL::MemberNode *mnode = (SL::MemberNode *)p_node;
			String name;
			if (mnode->basetype == SL::TYPE_STRUCT) {
				name = _mkid(mnode->name);
			} else {
				name = mnode->name;
			}
			code = _dump_node_code(mnode->owner, p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + "." + name;
			if (mnode->index_expression != nullptr) {
				code += "[";
				code += _dump_node_code(mnode->index_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += "]";
			} else if (mnode->assign_expression != nullptr) {
				code += "=";
				code += _dump_node_code(mnode->assign_expression, p_level, r_gen_code, p_actions, p_default_actions, true, false);
			} else if (mnode->call_expression != nullptr) {
				code += ".";
				code += _dump_node_code(mnode->call_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning, false);
			}
		} break;
	}

	return code;
}

static bool _is_resolve_diff_type(SL::DataType p_type) {
	switch (p_type) {
		case SL::TYPE_FLOAT:
		case SL::TYPE_VEC2:
		case SL::TYPE_VEC3:
		case SL::TYPE_VEC4:
		case SL::TYPE_MAT2:
		case SL::TYPE_MAT3:
		case SL::TYPE_MAT4:
			return true;
		default:
			return false;
	}
}

static String _resolve_zero_expr(SL::DataType p_type) {
	switch (p_type) {
		case SL::TYPE_FLOAT:
			return "0.0";
		case SL::TYPE_VEC2:
			return "vec2(0.0)";
		case SL::TYPE_VEC3:
			return "vec3(0.0)";
		case SL::TYPE_VEC4:
			return "vec4(0.0)";
		case SL::TYPE_MAT2:
			return "mat2(0.0)";
		case SL::TYPE_MAT3:
			return "mat3(0.0)";
		case SL::TYPE_MAT4:
			return "mat4(0.0)";
		default:
			return "0.0";
	}
}

static String _resolve_ctor_arg_zero_expr(SL::DataType p_type) {
	switch (p_type) {
		case SL::TYPE_BOOL:
		case SL::TYPE_INT:
		case SL::TYPE_UINT:
		case SL::TYPE_FLOAT:
			return "0.0";
		case SL::TYPE_BVEC2:
		case SL::TYPE_IVEC2:
		case SL::TYPE_UVEC2:
		case SL::TYPE_VEC2:
			return "vec2(0.0)";
		case SL::TYPE_BVEC3:
		case SL::TYPE_IVEC3:
		case SL::TYPE_UVEC3:
		case SL::TYPE_VEC3:
			return "vec3(0.0)";
		case SL::TYPE_BVEC4:
		case SL::TYPE_IVEC4:
		case SL::TYPE_UVEC4:
		case SL::TYPE_VEC4:
			return "vec4(0.0)";
		case SL::TYPE_MAT2:
		case SL::TYPE_MAT3:
		case SL::TYPE_MAT4:
			return _resolve_zero_expr(p_type);
		default:
			return "0.0";
	}
}

static bool _is_resolve_matrix_type(SL::DataType p_type) {
	return p_type == SL::TYPE_MAT2 || p_type == SL::TYPE_MAT3 || p_type == SL::TYPE_MAT4;
}

static SL::DataType _resolve_sampler_grad_type(SL::DataType p_sampler_type) {
	switch (p_sampler_type) {
		case SL::TYPE_SAMPLER3D:
		case SL::TYPE_ISAMPLER3D:
		case SL::TYPE_USAMPLER3D:
		case SL::TYPE_SAMPLERCUBE:
		case SL::TYPE_SAMPLERCUBEARRAY:
			return SL::TYPE_VEC3;
		default:
			return SL::TYPE_VEC2;
	}
}

static void _collect_assigned_names(const SL::Node *p_node, HashSet<StringName> &r_assigned) {
	if (!p_node) {
		return;
	}
	switch (p_node->type) {
		case SL::Node::NODE_TYPE_BLOCK: {
			const SL::BlockNode *bnode = static_cast<const SL::BlockNode *>(p_node);
			for (const SL::Node *stmt : bnode->statements) {
				_collect_assigned_names(stmt, r_assigned);
			}
		} break;
		case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
			const SL::VariableDeclarationNode *vdnode = static_cast<const SL::VariableDeclarationNode *>(p_node);
			for (const SL::VariableDeclarationNode::Declaration &decl : vdnode->declarations) {
				for (const SL::Node *init : decl.initializer) {
					_collect_assigned_names(init, r_assigned);
				}
			}
		} break;
		case SL::Node::NODE_TYPE_OPERATOR: {
			const SL::OperatorNode *onode = static_cast<const SL::OperatorNode *>(p_node);
			switch (onode->op) {
				case SL::OP_ASSIGN:
				case SL::OP_ASSIGN_ADD:
				case SL::OP_ASSIGN_SUB:
				case SL::OP_ASSIGN_MUL:
				case SL::OP_ASSIGN_DIV:
				case SL::OP_ASSIGN_SHIFT_LEFT:
				case SL::OP_ASSIGN_SHIFT_RIGHT:
				case SL::OP_ASSIGN_MOD:
				case SL::OP_ASSIGN_BIT_AND:
				case SL::OP_ASSIGN_BIT_OR:
				case SL::OP_ASSIGN_BIT_XOR:
				case SL::OP_INCREMENT:
				case SL::OP_DECREMENT:
				case SL::OP_POST_INCREMENT:
				case SL::OP_POST_DECREMENT: {
					const SL::Node *lhs = onode->arguments[0];
					while (lhs && lhs->type == SL::Node::NODE_TYPE_MEMBER) {
						lhs = static_cast<const SL::MemberNode *>(lhs)->owner;
					}
					if (lhs && lhs->type == SL::Node::NODE_TYPE_VARIABLE) {
						r_assigned.insert(static_cast<const SL::VariableNode *>(lhs)->name);
					}
				} break;
				default:
					break;
			}
			for (const SL::Node *arg : onode->arguments) {
				_collect_assigned_names(arg, r_assigned);
			}
		} break;
		case SL::Node::NODE_TYPE_CONTROL_FLOW: {
			const SL::ControlFlowNode *cfnode = static_cast<const SL::ControlFlowNode *>(p_node);
			for (const SL::Node *expr : cfnode->expressions) {
				_collect_assigned_names(expr, r_assigned);
			}
			for (const SL::BlockNode *blk : cfnode->blocks) {
				_collect_assigned_names(blk, r_assigned);
			}
		} break;
		case SL::Node::NODE_TYPE_MEMBER: {
			const SL::MemberNode *mnode = static_cast<const SL::MemberNode *>(p_node);
			if (mnode->assign_expression) {
				const SL::Node *lhs = mnode->owner;
				while (lhs && lhs->type == SL::Node::NODE_TYPE_MEMBER) {
					lhs = static_cast<const SL::MemberNode *>(lhs)->owner;
				}
				if (lhs && lhs->type == SL::Node::NODE_TYPE_VARIABLE) {
					r_assigned.insert(static_cast<const SL::VariableNode *>(lhs)->name);
				}
				_collect_assigned_names(mnode->assign_expression, r_assigned);
			}
			_collect_assigned_names(mnode->owner, r_assigned);
			_collect_assigned_names(mnode->index_expression, r_assigned);
			_collect_assigned_names(mnode->call_expression, r_assigned);
		} break;
		case SL::Node::NODE_TYPE_ARRAY: {
			const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(p_node);
			if (anode->assign_expression) {
				r_assigned.insert(anode->name);
				_collect_assigned_names(anode->assign_expression, r_assigned);
			}
			_collect_assigned_names(anode->index_expression, r_assigned);
			_collect_assigned_names(anode->call_expression, r_assigned);
		} break;
		case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
			const SL::ArrayConstructNode *acnode = static_cast<const SL::ArrayConstructNode *>(p_node);
			for (const SL::Node *init : acnode->initializer) {
				_collect_assigned_names(init, r_assigned);
			}
		} break;
		default:
			break;
	}
}

void ShaderCompiler::_compile_visibility_resolve(GeneratedCode &r_gen_code, IdentifierActions &p_actions, const DefaultIdentifierActions &p_default_actions) {
	ERR_FAIL_NULL(shader);

	struct ResolveExpr {
		String val;
		String ddx;
		String ddy;
		SL::DataType type = SL::TYPE_VOID;
		int array_size = 0;
		bool exact = false;
		bool has_varying = false;
	};

	struct ResolveVarInfo {
		SL::DataType type = SL::TYPE_VOID;
		int array_size = 0;
		bool is_const = false;
		bool has_companion = false;
		String dx_name;
		String dy_name;
		bool exact = false;
		bool has_varying = false;
		int declared_cf_depth = 0;
	};

	// Sort vertex-to-fragment and fragment-to-light varyings deterministically.
	Vector<StringName> varying_names;
	for (const KeyValue<StringName, SL::ShaderNode::Varying> &E : shader->varyings) {
		varying_names.push_back(E.key);
	}
	varying_names.sort_custom<StringName::AlphCompare>();

	String resolve_frag_globals;
	String resolve_vertex_store;
	String resolve_interpolate;
	HashMap<StringName, bool> frag_to_light_exact;
	HashMap<StringName, bool> frag_to_light_varying;

	bool has_diff_frag_to_light = false;
	String frag_to_light_dx_struct = "struct {\n";
	String frag_to_light_dy_struct = "struct {\n";

	for (int k = 0; k < varying_names.size(); k++) {
		const StringName &varying_name = varying_names[k];
		const SL::ShaderNode::Varying &varying = shader->varyings[varying_name];
		if (varying.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT) {
			if (_is_resolve_diff_type(varying.type) && varying.array_size == 0) {
				has_diff_frag_to_light = true;
				String member = "\t" + _prestr(varying.precision) + _typestr(varying.type) + " " + _mkid(varying_name) + ";\n";
				frag_to_light_dx_struct += member;
				frag_to_light_dy_struct += member;
			}
			continue;
		}
		if (varying.type < SL::TYPE_INT) {
			continue;
		}

		const String id = _mkid(varying_name);
		const String type_decl = _prestr(varying.precision, SL::is_float_type(varying.type)) + _typestr(varying.type);
		const String arr_suffix = varying.array_size > 0 ? ("[" + itos(varying.array_size) + "]") : String();
		const bool is_smooth_diff = (varying.interpolation != SL::INTERPOLATION_FLAT) && _is_resolve_diff_type(varying.type);

		resolve_frag_globals += type_decl + " " + id + arr_suffix + ";\n";
		resolve_frag_globals += type_decl + " _ng_v0_" + id + arr_suffix + ";\n";
		resolve_frag_globals += type_decl + " _ng_v1_" + id + arr_suffix + ";\n";
		resolve_frag_globals += type_decl + " _ng_v2_" + id + arr_suffix + ";\n";
		if (is_smooth_diff) {
			resolve_frag_globals += type_decl + " _ng_dx_" + id + arr_suffix + ";\n";
			resolve_frag_globals += type_decl + " _ng_dy_" + id + arr_suffix + ";\n";
		}

		if (varying.array_size > 0) {
			for (int ai = 0; ai < varying.array_size; ai++) {
				const String idx = "[" + itos(ai) + "]";
				resolve_vertex_store += "\t\tif (_ng_corner == 0u) { _ng_v0_" + id + idx + " = " + id + idx + "; } else if (_ng_corner == 1u) { _ng_v1_" + id + idx + " = " + id + idx + "; } else { _ng_v2_" + id + idx + " = " + id + idx + "; }\n";
				if (is_smooth_diff) {
					resolve_interpolate += "\t" + id + idx + " = _ng_v0_" + id + idx + " * _ng_bary.x + _ng_v1_" + id + idx + " * _ng_bary.y + _ng_v2_" + id + idx + " * _ng_bary.z;\n";
					resolve_interpolate += "\t_ng_dx_" + id + idx + " = _ng_v0_" + id + idx + " * _ng_dbary_dx.x + _ng_v1_" + id + idx + " * _ng_dbary_dx.y + _ng_v2_" + id + idx + " * _ng_dbary_dx.z;\n";
					resolve_interpolate += "\t_ng_dy_" + id + idx + " = _ng_v0_" + id + idx + " * _ng_dbary_dy.x + _ng_v1_" + id + idx + " * _ng_dbary_dy.y + _ng_v2_" + id + idx + " * _ng_dbary_dy.z;\n";
				} else {
					resolve_interpolate += "\t" + id + idx + " = _ng_v0_" + id + idx + ";\n";
				}
			}
		} else {
			resolve_vertex_store += "\t\tif (_ng_corner == 0u) { _ng_v0_" + id + " = " + id + "; } else if (_ng_corner == 1u) { _ng_v1_" + id + " = " + id + "; } else { _ng_v2_" + id + " = " + id + "; }\n";
			if (is_smooth_diff) {
				resolve_interpolate += "\t" + id + " = _ng_v0_" + id + " * _ng_bary.x + _ng_v1_" + id + " * _ng_bary.y + _ng_v2_" + id + " * _ng_bary.z;\n";
				resolve_interpolate += "\t_ng_dx_" + id + " = _ng_v0_" + id + " * _ng_dbary_dx.x + _ng_v1_" + id + " * _ng_dbary_dx.y + _ng_v2_" + id + " * _ng_dbary_dx.z;\n";
				resolve_interpolate += "\t_ng_dy_" + id + " = _ng_v0_" + id + " * _ng_dbary_dy.x + _ng_v1_" + id + " * _ng_dbary_dy.y + _ng_v2_" + id + " * _ng_dbary_dy.z;\n";
			} else {
				resolve_interpolate += "\t" + id + " = _ng_v0_" + id + ";\n";
			}
		}
	}

	if (has_diff_frag_to_light) {
		frag_to_light_dx_struct += "} _ng_dx_frag_to_light;\n";
		frag_to_light_dy_struct += "} _ng_dy_frag_to_light;\n";
		resolve_frag_globals += frag_to_light_dx_struct;
		resolve_frag_globals += frag_to_light_dy_struct;
	}

	r_gen_code.code["resolve_vertex_store"] = resolve_vertex_store;
	r_gen_code.code["resolve_interpolate"] = resolve_interpolate;

	struct ResolveEmitter {
		ShaderCompiler *self = nullptr;
		const SL::ShaderNode *shader_node = nullptr;
		GeneratedCode *gen_code = nullptr;
		IdentifierActions *id_actions = nullptr;
		const DefaultIdentifierActions *def_actions = nullptr;
		const SL::FunctionNode *cur_func = nullptr;
		StringName cur_func_name;
		bool is_vertex_mode = false;
		bool is_entry_fragment = false;
		bool is_entry_light = false;
		int cf_depth = 0;
		Vector<HashMap<StringName, ResolveVarInfo>> scopes;
		HashMap<StringName, bool> *frag_to_light_exact_ptr = nullptr;
		HashMap<StringName, bool> *frag_to_light_varying_ptr = nullptr;

		ResolveVarInfo *find_var(const StringName &p_name) {
			for (int i = scopes.size() - 1; i >= 0; i--) {
				if (scopes[i].has(p_name)) {
					return &scopes.write[i][p_name];
				}
			}
			return nullptr;
		}

		bool get_lvalue_companions(const SL::Node *p_lhs, String &r_dx_lhs, String &r_dy_lhs, ResolveVarInfo **r_root_var) {
			if (!p_lhs) {
				return false;
			}
			if (p_lhs->type == SL::Node::NODE_TYPE_VARIABLE) {
				const SL::VariableNode *vn = static_cast<const SL::VariableNode *>(p_lhs);
				ResolveVarInfo *vi = find_var(vn->name);
				if (vi && vi->has_companion) {
					r_dx_lhs = vi->dx_name;
					r_dy_lhs = vi->dy_name;
					if (r_root_var) {
						*r_root_var = vi;
					}
					return true;
				}
				if (!vn->is_local && !is_vertex_mode && shader_node->varyings.has(vn->name)) {
					const SL::ShaderNode::Varying &var = shader_node->varyings[vn->name];
					if (var.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT && _is_resolve_diff_type(var.type) && var.array_size == 0) {
						r_dx_lhs = "_ng_dx_frag_to_light." + _mkid(vn->name);
						r_dy_lhs = "_ng_dy_frag_to_light." + _mkid(vn->name);
						if (r_root_var) {
							*r_root_var = nullptr;
						}
						return true;
					}
				}
				return false;
			}
			if (p_lhs->type == SL::Node::NODE_TYPE_MEMBER) {
				const SL::MemberNode *mn = static_cast<const SL::MemberNode *>(p_lhs);
				if (mn->basetype != SL::TYPE_STRUCT && mn->index_expression == nullptr && mn->call_expression == nullptr && mn->assign_expression == nullptr) {
					String owner_dx, owner_dy;
					if (get_lvalue_companions(mn->owner, owner_dx, owner_dy, r_root_var)) {
						r_dx_lhs = owner_dx + "." + String(mn->name);
						r_dy_lhs = owner_dy + "." + String(mn->name);
						return true;
					}
				}
			}
			return false;
		}

		void mark_lhs_assigned(const SL::Node *p_lhs, bool p_rhs_exact, bool p_rhs_varying) {
			const SL::Node *root = p_lhs;
			bool partial = false;
			while (root && root->type == SL::Node::NODE_TYPE_MEMBER) {
				partial = true;
				root = static_cast<const SL::MemberNode *>(root)->owner;
			}
			if (!root || root->type != SL::Node::NODE_TYPE_VARIABLE) {
				return;
			}
			const SL::VariableNode *vn = static_cast<const SL::VariableNode *>(root);
			ResolveVarInfo *vi = find_var(vn->name);
			if (vi) {
				if (cf_depth > vi->declared_cf_depth) {
					vi->exact = false;
				} else if (partial) {
					vi->exact = vi->exact && p_rhs_exact;
					vi->has_varying = vi->has_varying || p_rhs_varying;
				} else {
					vi->exact = p_rhs_exact;
					vi->has_varying = p_rhs_varying;
				}
			} else if (!vn->is_local && !is_vertex_mode && shader_node->varyings.has(vn->name)) {
				const SL::ShaderNode::Varying &var = shader_node->varyings[vn->name];
				if (var.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT && frag_to_light_exact_ptr) {
					if (cf_depth > 0) {
						(*frag_to_light_exact_ptr)[vn->name] = false;
					} else if (partial) {
						bool prev = frag_to_light_exact_ptr->has(vn->name) ? (*frag_to_light_exact_ptr)[vn->name] : true;
						(*frag_to_light_exact_ptr)[vn->name] = prev && p_rhs_exact;
						if (frag_to_light_varying_ptr) {
							bool prev_v = frag_to_light_varying_ptr->has(vn->name) ? (*frag_to_light_varying_ptr)[vn->name] : false;
							(*frag_to_light_varying_ptr)[vn->name] = prev_v || p_rhs_varying;
						}
					} else {
						(*frag_to_light_exact_ptr)[vn->name] = p_rhs_exact;
						if (frag_to_light_varying_ptr) {
							(*frag_to_light_varying_ptr)[vn->name] = p_rhs_varying;
						}
					}
				}
			}
		}

		ResolveExpr emit_node(const SL::Node *p_node, int p_level, bool p_assigning, bool p_use_scope = true) {
			ResolveExpr res;
			if (!p_node) {
				return res;
			}

			switch (p_node->type) {
				case SL::Node::NODE_TYPE_BLOCK: {
					const SL::BlockNode *bnode = static_cast<const SL::BlockNode *>(p_node);
					scopes.push_back(HashMap<StringName, ResolveVarInfo>());
					if (!bnode->single_statement) {
						res.val += _mktab(p_level - 1) + "{\n";
					}
					int i = 0;
					for (List<SL::Node *>::ConstIterator itr = bnode->statements.begin(); itr != bnode->statements.end(); ++itr, ++i) {
						ResolveExpr s = emit_node(*itr, p_level, p_assigning);
						if ((*itr)->type == SL::Node::NODE_TYPE_CONTROL_FLOW || bnode->single_statement) {
							res.val += s.val;
							if (bnode->use_comma_between_statements && i + 1 < bnode->statements.size()) {
								res.val += ",";
							}
						} else {
							res.val += _mktab(p_level) + s.val + ";\n";
						}
					}
					if (!bnode->single_statement) {
						res.val += _mktab(p_level - 1) + "}\n";
					}
					scopes.resize(scopes.size() - 1);
				} break;

				case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
					const SL::VariableDeclarationNode *vdnode = static_cast<const SL::VariableDeclarationNode *>(p_node);
					String declaration;
					declaration += _constr(vdnode->is_const);
					if (vdnode->datatype == SL::TYPE_STRUCT) {
						declaration += _mkid(vdnode->struct_name);
					} else {
						declaration += _prestr(vdnode->precision) + _typestr(vdnode->datatype);
					}
					declaration += " ";

					bool first_decl = true;
					for (int i = 0; i < vdnode->declarations.size(); i++) {
						const SL::VariableDeclarationNode::Declaration &d = vdnode->declarations[i];
						const bool is_array = d.size > 0;
						const String id = _mkid(d.name);
						const bool make_companion = !is_vertex_mode && !vdnode->is_const && !is_array && _is_resolve_diff_type(vdnode->datatype);

						ResolveExpr init_expr;
						bool has_single_init = (!is_array || d.single_expression) && !d.initializer.is_empty();
						if (has_single_init) {
							init_expr = emit_node(d.initializer[0], p_level, p_assigning);
						}

						if (make_companion) {
							String dx_id = "_ng_dx_" + id;
							String dy_id = "_ng_dy_" + id;
							String dx_init = has_single_init && !init_expr.ddx.is_empty() ? init_expr.ddx : _resolve_zero_expr(vdnode->datatype);
							String dy_init = has_single_init && !init_expr.ddy.is_empty() ? init_expr.ddy : _resolve_zero_expr(vdnode->datatype);
							if (!first_decl) {
								declaration += ",";
							}
							declaration += dx_id + "=" + dx_init + "," + dy_id + "=" + dy_init;
							first_decl = false;

							ResolveVarInfo vi;
							vi.type = vdnode->datatype;
							vi.array_size = 0;
							vi.is_const = false;
							vi.has_companion = true;
							vi.dx_name = dx_id;
							vi.dy_name = dy_id;
							vi.exact = has_single_init ? init_expr.exact : true;
							vi.has_varying = has_single_init ? init_expr.has_varying : false;
							vi.declared_cf_depth = cf_depth;
							scopes.write[scopes.size() - 1][d.name] = vi;
						} else {
							ResolveVarInfo vi;
							vi.type = vdnode->datatype;
							vi.array_size = d.size;
							vi.is_const = vdnode->is_const;
							vi.has_companion = false;
							vi.exact = vdnode->is_const && !is_array && _is_resolve_diff_type(vdnode->datatype) && (!has_single_init || init_expr.exact);
							vi.has_varying = vdnode->is_const && !is_array && has_single_init && init_expr.has_varying;
							if (vi.is_const && vi.has_varying && !init_expr.ddx.is_empty()) {
								vi.has_companion = true;
								vi.dx_name = "(" + init_expr.ddx + ")";
								vi.dy_name = "(" + init_expr.ddy + ")";
							}
							vi.declared_cf_depth = cf_depth;
							scopes.write[scopes.size() - 1][d.name] = vi;
						}

						if (!first_decl) {
							declaration += ",";
						}
						first_decl = false;

						declaration += id;
						if (is_array) {
							declaration += "[";
							if (d.size_expression != nullptr) {
								declaration += emit_node(d.size_expression, p_level, p_assigning).val;
							} else {
								declaration += itos(d.size);
							}
							declaration += "]";
						}

						if (!is_array || d.single_expression) {
							if (has_single_init) {
								declaration += "=" + init_expr.val;
							}
						} else {
							int sz = d.initializer.size();
							if (sz > 0) {
								declaration += "=";
								if (vdnode->datatype == SL::TYPE_STRUCT) {
									declaration += _mkid(vdnode->struct_name);
								} else {
									declaration += _typestr(vdnode->datatype);
								}
								declaration += "[" + itos(sz) + "](";
								for (int j = 0; j < sz; j++) {
									if (j > 0) {
										declaration += ",";
									}
									declaration += emit_node(d.initializer[j], p_level, p_assigning).val;
								}
								declaration += ")";
							}
						}
					}
					res.val = declaration;
				} break;

				case SL::Node::NODE_TYPE_VARIABLE: {
					const SL::VariableNode *vnode = static_cast<const SL::VariableNode *>(p_node);
					res.type = vnode->get_datatype();
					bool use_fragment_varying = false;

					if (!vnode->is_local && !is_vertex_mode) {
						if (p_assigning) {
							if (shader_node->varyings.has(vnode->name)) {
								use_fragment_varying = true;
							}
						} else {
							if (self->fragment_varyings.has(vnode->name)) {
								use_fragment_varying = true;
							}
						}
					}

					if (is_vertex_mode && def_actions->resolve_vertex_renames.has(vnode->name)) {
						res.val = def_actions->resolve_vertex_renames[vnode->name];
					} else if (!is_vertex_mode && def_actions->resolve_renames.has(vnode->name)) {
						res.val = def_actions->resolve_renames[vnode->name];
					} else if (def_actions->renames.has(vnode->name)) {
						res.val = def_actions->renames[vnode->name];
					} else {
						bool param_found = false;
						if (cur_func) {
							for (const SL::FunctionNode::Argument &argument : cur_func->arguments) {
								if (argument.name == vnode->name) {
									param_found = true;
									break;
								}
							}
						}
						if (!param_found && shader_node->uniforms.has(vnode->name)) {
							const SL::ShaderNode::Uniform &u = shader_node->uniforms[vnode->name];
							if (u.is_texture()) {
								if (u.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
									res.val = "color_buffer";
								} else if (u.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
									res.val = "normal_roughness_buffer";
								} else if (u.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
									res.val = "depth_buffer";
								} else if (u.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0) {
									res.val = "source0";
								} else if (u.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1) {
									res.val = "source1";
								} else if (u.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2) {
									res.val = "source2";
								} else if (u.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
									res.val = "source3";
								} else {
									res.val = _mkid(vnode->name);
								}
							} else {
								if (u.scope == SL::ShaderNode::Uniform::SCOPE_GLOBAL) {
									String idx = self->actions.base_uniform_string + _mkid(vnode->name);
									res.val = _get_global_shader_uniform_from_type_and_index(def_actions->global_buffer_array_variable, idx, u.type, def_actions->global_buffer_array_is_texture);
								} else if (u.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
									String idx = "(" + def_actions->instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
									res.val = _get_global_shader_uniform_from_type_and_index(def_actions->global_buffer_array_variable, idx, u.type, def_actions->global_buffer_array_is_texture);
								} else {
									res.val = self->actions.base_uniform_string + _mkid(vnode->name);
								}
							}
						} else {
							if (use_fragment_varying) {
								res.val = "frag_to_light.";
							}
							res.val += _mkid(vnode->name);
						}
					}

					if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
						ResolveVarInfo *vi = find_var(vnode->name);
						if (vi) {
							if (vi->has_companion) {
								res.ddx = vi->dx_name;
								res.ddy = vi->dy_name;
								res.exact = vi->exact;
								res.has_varying = vi->has_varying;
							} else {
								res.ddx = _resolve_zero_expr(res.type);
								res.ddy = _resolve_zero_expr(res.type);
								res.exact = vi->exact;
								res.has_varying = vi->has_varying;
							}
						} else if (!vnode->is_local && def_actions->resolve_builtin_derivatives.has(vnode->name)) {
							const Pair<String, String> &d = def_actions->resolve_builtin_derivatives[vnode->name];
							res.ddx = d.first;
							res.ddy = d.second;
							res.exact = true;
							res.has_varying = vnode->name != SNAME("POINT_COORD");
						} else if (!vnode->is_local && is_entry_fragment && def_actions->resolve_mutable_builtin_derivatives.has(vnode->name)) {
							const Pair<String, String> &d = def_actions->resolve_mutable_builtin_derivatives[vnode->name];
							res.ddx = d.first;
							res.ddy = d.second;
							res.exact = true;
							res.has_varying = true;
						} else if (!vnode->is_local && def_actions->resolve_uniform_builtins.has(vnode->name)) {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = true;
						} else if (!vnode->is_local && shader_node->varyings.has(vnode->name)) {
							const SL::ShaderNode::Varying &var = shader_node->varyings[vnode->name];
							if (var.array_size == 0) {
								if (var.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT) {
									res.ddx = "_ng_dx_frag_to_light." + _mkid(vnode->name);
									res.ddy = "_ng_dy_frag_to_light." + _mkid(vnode->name);
									res.exact = frag_to_light_exact_ptr && frag_to_light_exact_ptr->has(vnode->name) ? (*frag_to_light_exact_ptr)[vnode->name] : false;
									res.has_varying = frag_to_light_varying_ptr && frag_to_light_varying_ptr->has(vnode->name) ? (*frag_to_light_varying_ptr)[vnode->name] : false;
								} else if (var.interpolation == SL::INTERPOLATION_FLAT) {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = true;
								} else {
									res.ddx = "_ng_dx_" + _mkid(vnode->name);
									res.ddy = "_ng_dy_" + _mkid(vnode->name);
									res.exact = true;
									res.has_varying = true;
								}
							} else {
								res.ddx = _resolve_zero_expr(res.type);
								res.ddy = _resolve_zero_expr(res.type);
								res.exact = false;
							}
						} else if (!vnode->is_local && shader_node->uniforms.has(vnode->name)) {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = true;
						} else {
							bool is_global_const = false;
							for (int ci = 0; ci < shader_node->vconstants.size(); ci++) {
								if (shader_node->vconstants[ci].name == vnode->name) {
									is_global_const = true;
									break;
								}
							}
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = is_global_const;
						}
					}
				} break;

				case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
					const SL::ArrayConstructNode *acnode = static_cast<const SL::ArrayConstructNode *>(p_node);
					int sz = acnode->initializer.size();
					if (acnode->datatype == SL::TYPE_STRUCT) {
						res.val += _mkid(acnode->struct_name);
					} else {
						res.val += _typestr(acnode->datatype);
					}
					res.val += "[" + itos(sz) + "](";
					for (int i = 0; i < sz; i++) {
						res.val += emit_node(acnode->initializer[i], p_level, p_assigning).val;
						if (i != sz - 1) {
							res.val += ", ";
						}
					}
					res.val += ")";
					res.type = acnode->datatype;
					res.array_size = sz;
				} break;

				case SL::Node::NODE_TYPE_ARRAY: {
					const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(p_node);
					res.type = anode->get_datatype();
					bool use_fragment_varying = false;

					if (!anode->is_local && !is_vertex_mode) {
						if (anode->assign_expression != nullptr && shader_node->varyings.has(anode->name)) {
							use_fragment_varying = true;
						} else if (p_assigning) {
							if (shader_node->varyings.has(anode->name)) {
								use_fragment_varying = true;
							}
						} else if (self->fragment_varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					}

					String base_code;
					if (def_actions->renames.has(anode->name)) {
						base_code = def_actions->renames[anode->name];
					} else {
						bool param_found = false;
						if (cur_func) {
							for (const SL::FunctionNode::Argument &argument : cur_func->arguments) {
								if (argument.name == anode->name) {
									param_found = true;
									break;
								}
							}
						}
						if (!param_found && shader_node->uniforms.has(anode->name)) {
							const SL::ShaderNode::Uniform &u = shader_node->uniforms[anode->name];
							if (u.is_texture()) {
								base_code = _mkid(anode->name);
							} else if (u.scope == SL::ShaderNode::Uniform::SCOPE_GLOBAL) {
								String idx = self->actions.base_uniform_string + _mkid(anode->name);
								base_code = _get_global_shader_uniform_from_type_and_index(def_actions->global_buffer_array_variable, idx, u.type, def_actions->global_buffer_array_is_texture);
							} else if (u.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
								String idx = "(" + def_actions->instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
								base_code = _get_global_shader_uniform_from_type_and_index(def_actions->global_buffer_array_variable, idx, u.type, def_actions->global_buffer_array_is_texture);
							} else {
								base_code = self->actions.base_uniform_string + _mkid(anode->name);
							}
						} else {
							if (use_fragment_varying) {
								base_code = "frag_to_light.";
							}
							base_code += _mkid(anode->name);
						}
					}

					res.val = base_code;
					String idx_code;
					if (anode->call_expression != nullptr) {
						res.val += "." + emit_node(anode->call_expression, p_level, p_assigning, false).val;
					} else if (anode->index_expression != nullptr) {
						idx_code = emit_node(anode->index_expression, p_level, p_assigning).val;
						res.val += "[" + idx_code + "]";
					} else if (anode->assign_expression != nullptr) {
						res.val += "=" + emit_node(anode->assign_expression, p_level, true, false).val;
					}

					if (!is_vertex_mode && _is_resolve_diff_type(res.type) && anode->assign_expression == nullptr && anode->call_expression == nullptr) {
						if (!anode->is_local && anode->index_expression != nullptr && shader_node->varyings.has(anode->name)) {
							const SL::ShaderNode::Varying &var = shader_node->varyings[anode->name];
							if (var.stage != SL::ShaderNode::Varying::STAGE_FRAGMENT) {
								if (var.interpolation == SL::INTERPOLATION_FLAT) {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
								} else {
									res.ddx = "_ng_dx_" + _mkid(anode->name) + "[" + idx_code + "]";
									res.ddy = "_ng_dy_" + _mkid(anode->name) + "[" + idx_code + "]";
									res.has_varying = true;
								}
								res.exact = true;
							} else {
								res.ddx = _resolve_zero_expr(res.type);
								res.ddy = _resolve_zero_expr(res.type);
							}
						} else if (!anode->is_local && shader_node->uniforms.has(anode->name)) {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = true;
						} else {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = false;
						}
					}
				} break;

				case SL::Node::NODE_TYPE_CONSTANT: {
					const SL::ConstantNode *cnode = static_cast<const SL::ConstantNode *>(p_node);
					res.type = cnode->datatype;
					res.array_size = cnode->array_size;
					if (cnode->array_size == 0) {
						res.val = get_constant_text(cnode->datatype, cnode->values);
						if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = true;
						}
					} else {
						if (cnode->get_datatype() == SL::TYPE_STRUCT) {
							res.val += _mkid(cnode->struct_name);
						} else {
							res.val += _typestr(cnode->datatype);
						}
						res.val += "[" + itos(cnode->array_size) + "](";
						for (int i = 0; i < cnode->array_size; i++) {
							if (i > 0) {
								res.val += ",";
							}
							res.val += emit_node(cnode->array_declarations[0].initializer[i], p_level, p_assigning).val;
						}
						res.val += ")";
					}
				} break;

				case SL::Node::NODE_TYPE_OPERATOR: {
					const SL::OperatorNode *onode = static_cast<const SL::OperatorNode *>(p_node);
					res.type = onode->get_datatype();

					switch (onode->op) {
						case SL::OP_ASSIGN:
						case SL::OP_ASSIGN_ADD:
						case SL::OP_ASSIGN_SUB:
						case SL::OP_ASSIGN_MUL:
						case SL::OP_ASSIGN_DIV:
						case SL::OP_ASSIGN_SHIFT_LEFT:
						case SL::OP_ASSIGN_SHIFT_RIGHT:
						case SL::OP_ASSIGN_MOD:
						case SL::OP_ASSIGN_BIT_AND:
						case SL::OP_ASSIGN_BIT_OR:
						case SL::OP_ASSIGN_BIT_XOR: {
							ResolveExpr lhs = emit_node(onode->arguments[0], p_level, true);
							ResolveExpr rhs = emit_node(onode->arguments[1], p_level, p_assigning);
							String assign_expr = lhs.val + _opstr(onode->op) + rhs.val;

							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								String dx_lhs, dy_lhs;
								ResolveVarInfo *root_vi = nullptr;
								bool has_comp = get_lvalue_companions(onode->arguments[0], dx_lhs, dy_lhs, &root_vi);
								String new_dx = _resolve_zero_expr(res.type);
								String new_dy = _resolve_zero_expr(res.type);
								bool new_exact = false;
								bool new_varying = false;
								const String rhs_dx = rhs.ddx.is_empty() ? _resolve_zero_expr(rhs.type) : rhs.ddx;
								const String rhs_dy = rhs.ddy.is_empty() ? _resolve_zero_expr(rhs.type) : rhs.ddy;

								if (onode->op == SL::OP_ASSIGN) {
									new_dx = rhs_dx;
									new_dy = rhs_dy;
									new_exact = rhs.exact;
									new_varying = rhs.has_varying;
								} else if (has_comp && (onode->op == SL::OP_ASSIGN_ADD || onode->op == SL::OP_ASSIGN_SUB)) {
									String op = (onode->op == SL::OP_ASSIGN_ADD) ? " + " : " - ";
									new_dx = "(" + dx_lhs + op + rhs_dx + ")";
									new_dy = "(" + dy_lhs + op + rhs_dy + ")";
									new_exact = lhs.exact && rhs.exact;
									new_varying = lhs.has_varying || rhs.has_varying;
								} else if (has_comp && onode->op == SL::OP_ASSIGN_MUL) {
									new_dx = "(" + dx_lhs + " * " + rhs.val + " + " + lhs.val + " * " + rhs_dx + ")";
									new_dy = "(" + dy_lhs + " * " + rhs.val + " + " + lhs.val + " * " + rhs_dy + ")";
									new_exact = lhs.exact && rhs.exact;
									new_varying = lhs.has_varying || rhs.has_varying;
								} else if (has_comp && onode->op == SL::OP_ASSIGN_DIV && (!_is_resolve_matrix_type(res.type) || rhs.type == SL::TYPE_FLOAT)) {
									new_dx = "((" + dx_lhs + " * " + rhs.val + " - " + lhs.val + " * " + rhs_dx + ") / (" + rhs.val + " * " + rhs.val + "))";
									new_dy = "((" + dy_lhs + " * " + rhs.val + " - " + lhs.val + " * " + rhs_dy + ") / (" + rhs.val + " * " + rhs.val + "))";
									new_exact = lhs.exact && rhs.exact;
									new_varying = lhs.has_varying || rhs.has_varying;
								}

								mark_lhs_assigned(onode->arguments[0], new_exact, new_varying);
								if (has_comp) {
									res.val = "(" + dx_lhs + "=" + new_dx + ", " + dy_lhs + "=" + new_dy + ", " + assign_expr + ")";
									res.ddx = dx_lhs;
									res.ddy = dy_lhs;
									res.exact = new_exact && (!root_vi || cf_depth <= root_vi->declared_cf_depth);
									res.has_varying = new_varying;
								} else {
									res.val = assign_expr;
									res.ddx = new_dx;
									res.ddy = new_dy;
									res.exact = false;
								}
							} else {
								if (!is_vertex_mode) {
									mark_lhs_assigned(onode->arguments[0], false, false);
								}
								res.val = assign_expr;
							}
						} break;

						case SL::OP_BIT_INVERT:
						case SL::OP_NEGATE:
						case SL::OP_NOT:
						case SL::OP_DECREMENT:
						case SL::OP_INCREMENT: {
							ResolveExpr a = emit_node(onode->arguments[0], p_level, p_assigning);
							if (onode->op == SL::OP_NEGATE && a.val.begins_with("-")) {
								res.val = a.val;
							} else {
								res.val = _opstr(onode->op) + a.val;
							}
							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								if (onode->op == SL::OP_NEGATE) {
									res.ddx = "(-" + (a.ddx.is_empty() ? _resolve_zero_expr(res.type) : a.ddx) + ")";
									res.ddy = "(-" + (a.ddy.is_empty() ? _resolve_zero_expr(res.type) : a.ddy) + ")";
									res.exact = a.exact;
									res.has_varying = a.has_varying;
								} else {
									if (onode->op == SL::OP_INCREMENT || onode->op == SL::OP_DECREMENT) {
										mark_lhs_assigned(onode->arguments[0], a.exact, a.has_varying);
									}
									res.ddx = a.ddx.is_empty() ? _resolve_zero_expr(res.type) : a.ddx;
									res.ddy = a.ddy.is_empty() ? _resolve_zero_expr(res.type) : a.ddy;
									res.exact = a.exact && cf_depth == 0;
									res.has_varying = a.has_varying;
								}
							}
						} break;

						case SL::OP_POST_DECREMENT:
						case SL::OP_POST_INCREMENT: {
							ResolveExpr a = emit_node(onode->arguments[0], p_level, p_assigning);
							res.val = a.val + _opstr(onode->op);
							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								mark_lhs_assigned(onode->arguments[0], a.exact, a.has_varying);
								res.ddx = a.ddx.is_empty() ? _resolve_zero_expr(res.type) : a.ddx;
								res.ddy = a.ddy.is_empty() ? _resolve_zero_expr(res.type) : a.ddy;
								res.exact = a.exact && cf_depth == 0;
								res.has_varying = a.has_varying;
							}
						} break;

						case SL::OP_CALL:
						case SL::OP_STRUCT:
						case SL::OP_CONSTRUCT: {
							ERR_FAIL_COND_V(onode->arguments[0]->type != SL::Node::NODE_TYPE_VARIABLE, res);
							const SL::VariableNode *vnode = static_cast<const SL::VariableNode *>(onode->arguments[0]);
							const SL::FunctionNode *func = nullptr;
							const bool is_internal_func = self->internal_functions.has(vnode->name);

							if (!is_internal_func && onode->op == SL::OP_CALL) {
								for (int i = 0; i < shader_node->vfunctions.size(); i++) {
									if (shader_node->vfunctions[i].name == vnode->name) {
										func = shader_node->vfunctions[i].function;
										break;
									}
								}
							}

							// Evaluate arguments first.
							Vector<ResolveExpr> arg_exprs;
							arg_exprs.resize(onode->arguments.size());
							for (int i = 1; i < onode->arguments.size(); i++) {
								bool is_out_qual = false;
								if (onode->op == SL::OP_CALL) {
									if (is_internal_func) {
										is_out_qual = SL::is_builtin_func_out_parameter(vnode->name, i - 1);
									} else if (func != nullptr) {
										SL::ArgumentQualifier q = func->arguments[i - 1].qualifier;
										is_out_qual = q == SL::ARGUMENT_QUALIFIER_OUT || q == SL::ARGUMENT_QUALIFIER_INOUT;
									}
								}
								arg_exprs.write[i] = emit_node(onode->arguments[i], p_level, p_assigning);
								if (is_out_qual && !is_vertex_mode) {
									mark_lhs_assigned(onode->arguments[i], false, false);
								}
							}

							if (onode->op == SL::OP_CONSTRUCT) {
								res.val = String(vnode->name) + "(";
								for (int i = 1; i < onode->arguments.size(); i++) {
									if (i > 1) {
										res.val += ", ";
									}
									res.val += arg_exprs[i].val;
								}
								res.val += ")";

								if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
									bool all_exact = true;
									bool any_varying = false;
									bool all_zero = true;
									for (int i = 1; i < onode->arguments.size(); i++) {
										SL::DataType at = arg_exprs[i].type;
										if (_is_resolve_diff_type(at)) {
											all_exact = all_exact && arg_exprs[i].exact;
											any_varying = any_varying || arg_exprs[i].has_varying;
											String z = _resolve_zero_expr(at);
											if (arg_exprs[i].ddx != z || arg_exprs[i].ddy != z) {
												all_zero = false;
											}
										}
									}

									if (all_zero) {
										res.ddx = _resolve_zero_expr(res.type);
										res.ddy = _resolve_zero_expr(res.type);
									} else if (_is_resolve_matrix_type(res.type) && onode->arguments.size() == 2 && _is_resolve_matrix_type(arg_exprs[1].type) && res.type > arg_exprs[1].type) {
										// Expanding a smaller matrix to a larger matrix in GLSL sets diagonal 1s; zero those out in the derivative.
										String diag_fix = String(vnode->name) + "(1.0) - " + String(vnode->name) + "(" + _typestr(arg_exprs[1].type) + "(1.0))";
										res.ddx = "(" + String(vnode->name) + "(" + arg_exprs[1].ddx + ") - (" + diag_fix + "))";
										res.ddy = "(" + String(vnode->name) + "(" + arg_exprs[1].ddy + ") - (" + diag_fix + "))";
									} else {
										String dx_call = String(vnode->name) + "(";
										String dy_call = String(vnode->name) + "(";
										for (int i = 1; i < onode->arguments.size(); i++) {
											if (i > 1) {
												dx_call += ", ";
												dy_call += ", ";
											}
											SL::DataType at = arg_exprs[i].type;
											if (_is_resolve_diff_type(at) && !arg_exprs[i].ddx.is_empty()) {
												dx_call += arg_exprs[i].ddx;
												dy_call += arg_exprs[i].ddy;
											} else {
												String z = _resolve_ctor_arg_zero_expr(at);
												dx_call += z;
												dy_call += z;
											}
										}
										dx_call += ")";
										dy_call += ")";
										res.ddx = dx_call;
										res.ddy = dy_call;
									}
									res.exact = all_exact;
									res.has_varying = any_varying;
								}
								break;
							}

							if (onode->op == SL::OP_STRUCT) {
								res.val = _mkid(vnode->name) + "(";
								for (int i = 1; i < onode->arguments.size(); i++) {
									if (i > 1) {
										res.val += ", ";
									}
									res.val += arg_exprs[i].val;
								}
								res.val += ")";
								break;
							}

							// OP_CALL: check derivative built-ins in resolve fragment/light mode.
							if (!is_vertex_mode && is_internal_func) {
								const String fname = String(vnode->name);
								if (fname == "dFdx" || fname == "dFdxCoarse" || fname == "dFdxFine") {
									if (!arg_exprs[1].exact || !arg_exprs[1].has_varying) {
										gen_code->uses_unsupported_derivative = true;
									}
									res.val = arg_exprs[1].ddx.is_empty() ? _resolve_zero_expr(res.type) : arg_exprs[1].ddx;
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
									break;
								}
								if (fname == "dFdy" || fname == "dFdyCoarse" || fname == "dFdyFine") {
									if (!arg_exprs[1].exact || !arg_exprs[1].has_varying) {
										gen_code->uses_unsupported_derivative = true;
									}
									res.val = arg_exprs[1].ddy.is_empty() ? _resolve_zero_expr(res.type) : arg_exprs[1].ddy;
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
									break;
								}
								if (fname == "fwidth" || fname == "fwidthCoarse" || fname == "fwidthFine") {
									if (!arg_exprs[1].exact || !arg_exprs[1].has_varying) {
										gen_code->uses_unsupported_derivative = true;
									}
									String dx = arg_exprs[1].ddx.is_empty() ? _resolve_zero_expr(res.type) : arg_exprs[1].ddx;
									String dy = arg_exprs[1].ddy.is_empty() ? _resolve_zero_expr(res.type) : arg_exprs[1].ddy;
									res.val = "(abs(" + dx + ") + abs(" + dy + "))";
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
									break;
								}
								if (fname == "textureQueryLod") {
									gen_code->uses_unsupported_derivative = true;
									res.val = "vec2(0.0)";
									res.ddx = "vec2(0.0)";
									res.ddy = "vec2(0.0)";
									res.exact = false;
									break;
								}
							}

							// User-defined helper function call.
							if (!is_internal_func && func != nullptr) {
								String prefix = is_vertex_mode ? "_ng_rv_" : "_ng_rf_";
								res.val = prefix + _mkid(vnode->rname) + "(";
								bool first_arg = true;
								for (int i = 1; i < onode->arguments.size(); i++) {
									if (!first_arg) {
										res.val += ", ";
									}
									first_arg = false;
									res.val += arg_exprs[i].val;
									if (!is_vertex_mode) {
										const SL::FunctionNode::Argument &farg = func->arguments[i - 1];
										if (farg.qualifier == SL::ARGUMENT_QUALIFIER_IN && _is_resolve_diff_type(farg.type) && farg.array_size == 0) {
											String dx = arg_exprs[i].ddx.is_empty() ? _resolve_zero_expr(farg.type) : arg_exprs[i].ddx;
											String dy = arg_exprs[i].ddy.is_empty() ? _resolve_zero_expr(farg.type) : arg_exprs[i].ddy;
											res.val += ", " + dx + ", " + dy;
										}
									}
								}
								res.val += ")";
								if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
								}
								break;
							}

							// Built-in non-derivative functions (including texture sampling).
							const bool is_texture_func = is_internal_func && self->texture_functions.has(vnode->name);
							if (is_texture_func) {
								const String fname = String(vnode->name);
								const bool texture_func_no_uv = (fname == "textureSize" || fname == "textureQueryLevels");
								const bool texture_func_returns_data = texture_func_no_uv || fname == "textureQueryLod";
								const bool texture_func_simple = (fname == "texture");
								const bool texture_func_proj = (fname == "textureProj");

								bool is_screen_texture = false;
								bool is_depth_texture = false;
								bool is_normal_roughness_texture = false;
								bool is_radiance_texture = false;
								bool multiview_uv_needed = false;

								String sampler_arg_code = arg_exprs[1].val;
								SL::DataType sampler_dt = onode->arguments[1]->get_datatype();
								StringName texture_uniform;
								bool correct_texture_uniform = false;

								if (onode->arguments[1]->type == SL::Node::NODE_TYPE_VARIABLE) {
									texture_uniform = static_cast<const SL::VariableNode *>(onode->arguments[1])->name;
									correct_texture_uniform = true;
								} else if (onode->arguments[1]->type == SL::Node::NODE_TYPE_ARRAY) {
									texture_uniform = static_cast<const SL::ArrayNode *>(onode->arguments[1])->name;
									correct_texture_uniform = true;
								}

								if (correct_texture_uniform && !RS::get_singleton()->is_low_end()) {
									String sampler_name;
									if (self->actions.custom_samplers.has(texture_uniform)) {
										sampler_name = self->actions.custom_samplers[texture_uniform];
									} else if (shader_node->uniforms.has(texture_uniform)) {
										const SL::ShaderNode::Uniform &u = shader_node->uniforms[texture_uniform];
										if (u.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											is_screen_texture = true;
										} else if (u.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											is_depth_texture = true;
										} else if (u.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											is_normal_roughness_texture = true;
										}
										sampler_name = self->_get_sampler_name(u.filter, u.repeat);
									} else if (cur_func != nullptr) {
										bool found = false;
										for (int j = 0; j < cur_func->arguments.size(); j++) {
											if (cur_func->arguments[j].name == texture_uniform) {
												if (cur_func->arguments[j].tex_builtin_check) {
													if (self->actions.custom_samplers.has(cur_func->arguments[j].tex_builtin)) {
														sampler_name = self->actions.custom_samplers[cur_func->arguments[j].tex_builtin];
														found = true;
													}
													break;
												}
												if (cur_func->arguments[j].tex_argument_check) {
													if (cur_func->arguments[j].tex_hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
														is_screen_texture = true;
													} else if (cur_func->arguments[j].tex_hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
														is_depth_texture = true;
													} else if (cur_func->arguments[j].tex_hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
														is_normal_roughness_texture = true;
													}
													sampler_name = self->_get_sampler_name(cur_func->arguments[j].tex_argument_filter, cur_func->arguments[j].tex_argument_repeat);
													found = true;
													break;
												}
											}
										}
										if (!found) {
											sampler_name = self->_get_sampler_name(SL::FILTER_DEFAULT, SL::REPEAT_DEFAULT);
										}
									} else {
										sampler_name = self->_get_sampler_name(SL::FILTER_DEFAULT, SL::REPEAT_DEFAULT);
									}

									if (texture_uniform == SNAME("RADIANCE")) {
										is_radiance_texture = true;
									}

									String data_type_name;
									if (self->actions.check_multiview_samplers && (is_screen_texture || is_depth_texture || is_normal_roughness_texture)) {
										data_type_name = "multiviewSampler";
										multiview_uv_needed = true;
									} else if (is_radiance_texture) {
										data_type_name = "sampler2D";
									} else {
										data_type_name = SL::get_datatype_name(sampler_dt);
									}
									sampler_arg_code = data_type_name + "(" + sampler_arg_code + ", " + sampler_name + ")";
								}

								String uv_code;
								if (onode->arguments.size() >= 3) {
									uv_code = arg_exprs[2].val;
									if (multiview_uv_needed && !texture_func_no_uv) {
										uv_code = "multiview_uv(" + uv_code + ".xy)";
									} else if (is_radiance_texture && !texture_func_no_uv) {
										uv_code = "vec3_to_oct_with_border(" + uv_code + ", params.border_size)";
									}
								}

								String call_code;
								if (is_radiance_texture && texture_func_simple) {
									call_code = "textureLod(" + sampler_arg_code + ", " + uv_code;
									if (onode->arguments.size() == 3) {
										call_code += ", 0.0)";
									} else {
										call_code += ", " + arg_exprs[3].val + ")";
									}
								} else if ((texture_func_simple || texture_func_proj) && sampler_dt != SL::TYPE_SAMPLEREXT) {
									if (is_vertex_mode) {
										String lod_fn = texture_func_simple ? "textureLod" : "textureProjLod";
										String lod_val = (onode->arguments.size() >= 4) ? arg_exprs[3].val : "0.0";
										call_code = lod_fn + "(" + sampler_arg_code + ", " + uv_code + ", " + lod_val + ")";
									} else {
										SL::DataType grad_dt = _resolve_sampler_grad_type(sampler_dt);
										String gx = _resolve_zero_expr(grad_dt);
										String gy = _resolve_zero_expr(grad_dt);
										const ResolveExpr &uv_e = arg_exprs[2];
										const String uv_dx = uv_e.ddx.is_empty() ? _resolve_zero_expr(uv_e.type) : uv_e.ddx;
										const String uv_dy = uv_e.ddy.is_empty() ? _resolve_zero_expr(uv_e.type) : uv_e.ddy;

										if (texture_func_simple) {
											if (uv_e.type == grad_dt) {
												gx = uv_dx;
												gy = uv_dy;
											} else if (grad_dt == SL::TYPE_VEC2) {
												gx = "(" + uv_dx + ").xy";
												gy = "(" + uv_dy + ").xy";
											} else {
												gx = "(" + uv_dx + ").xyz";
												gy = "(" + uv_dy + ").xyz";
											}
										} else {
											// Projective coordinates: P.xy / q (2D) or P.xyz / q (3D), where q is last component of P.
											String coords_swiz = (grad_dt == SL::TYPE_VEC2) ? ".xy" : ".xyz";
											String q_swiz = (uv_e.type == SL::TYPE_VEC3) ? ".z" : ".w";
											String p_c = "(" + uv_e.val + ")" + coords_swiz;
											String p_q = "(" + uv_e.val + ")" + q_swiz;
											gx = "(((" + uv_dx + ")" + coords_swiz + " * " + p_q + " - " + p_c + " * (" + uv_dx + ")" + q_swiz + ") / (" + p_q + " * " + p_q + "))";
											gy = "(((" + uv_dy + ")" + coords_swiz + " * " + p_q + " - " + p_c + " * (" + uv_dy + ")" + q_swiz + ") / (" + p_q + " * " + p_q + "))";
										}

										if (onode->arguments.size() >= 4) {
											String bias_scale = "exp2(" + arg_exprs[3].val + ")";
											gx = "((" + gx + ") * " + bias_scale + ")";
											gy = "((" + gy + ") * " + bias_scale + ")";
										}

										String grad_fn = texture_func_simple ? "textureGrad" : "textureProjGrad";
										call_code = grad_fn + "(" + sampler_arg_code + ", " + uv_code + ", " + gx + ", " + gy + ")";
									}
								} else {
									call_code = fname + "(" + sampler_arg_code;
									for (int i = 2; i < onode->arguments.size(); i++) {
										call_code += ", " + ((i == 2) ? uv_code : arg_exprs[i].val);
									}
									call_code += ")";
								}

								if (is_screen_texture && !texture_func_returns_data && self->actions.apply_luminance_multiplier) {
									if (RS::get_singleton()->is_low_end()) {
										call_code = "(" + call_code + " / vec4(vec3(scene_data_block.data.luminance_multiplier), 1.0))";
									} else {
										call_code = "(" + call_code + " * vec4(vec3(sc_luminance_multiplier()), 1.0))";
									}
								}
								if (is_normal_roughness_texture && !texture_func_returns_data) {
									call_code = "normal_roughness_compatibility(" + call_code + ")";
								}

								res.val = call_code;
								if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
								}
								break;
							}

							// Standard builtin call (e.g. mix, sin, normalize, dot, etc.).
							String call_name = is_internal_func ? String(vnode->name) : (def_actions->renames.has(vnode->name) ? def_actions->renames[vnode->name] : _mkid(vnode->rname));
							res.val = call_name + "(";
							for (int i = 1; i < onode->arguments.size(); i++) {
								if (i > 1) {
									res.val += ", ";
								}
								res.val += arg_exprs[i].val;
							}
							res.val += ")";

							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								if (is_internal_func && vnode->name == SNAME("mix") && onode->arguments.size() == 4 && _is_resolve_diff_type(arg_exprs[1].type) && _is_resolve_diff_type(arg_exprs[2].type) && _is_resolve_diff_type(arg_exprs[3].type)) {
									const ResolveExpr &a = arg_exprs[1];
									const ResolveExpr &b = arg_exprs[2];
									const ResolveExpr &t = arg_exprs[3];
									String one = (t.type == SL::TYPE_FLOAT) ? "1.0" : (_typestr(t.type) + "(1.0)");
									res.ddx = "(" + a.ddx + " * (" + one + " - " + t.val + ") + " + b.ddx + " * " + t.val + " + (" + b.val + " - " + a.val + ") * " + t.ddx + ")";
									res.ddy = "(" + a.ddy + " * (" + one + " - " + t.val + ") + " + b.ddy + " * " + t.val + " + (" + b.val + " - " + a.val + ") * " + t.ddy + ")";
									res.exact = a.exact && b.exact && t.exact;
									res.has_varying = a.has_varying || b.has_varying || t.has_varying;
								} else {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
								}
							}
						} break;

						case SL::OP_INDEX: {
							ResolveExpr a = emit_node(onode->arguments[0], p_level, p_assigning);
							ResolveExpr idx = emit_node(onode->arguments[1], p_level, p_assigning);
							res.val = a.val + "[" + idx.val + "]";
							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								if (!a.ddx.is_empty() && _is_resolve_diff_type(a.type)) {
									res.ddx = "(" + a.ddx + ")[" + idx.val + "]";
									res.ddy = "(" + a.ddy + ")[" + idx.val + "]";
									res.exact = a.exact;
									res.has_varying = a.has_varying;
								} else {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
								}
							}
						} break;

						case SL::OP_SELECT_IF: {
							ResolveExpr cond = emit_node(onode->arguments[0], p_level, p_assigning);
							ResolveExpr t_branch = emit_node(onode->arguments[1], p_level, p_assigning);
							ResolveExpr f_branch = emit_node(onode->arguments[2], p_level, p_assigning);
							res.val = "(" + cond.val + "?" + t_branch.val + ":" + f_branch.val + ")";
							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								String tdx = t_branch.ddx.is_empty() ? _resolve_zero_expr(res.type) : t_branch.ddx;
								String fdx = f_branch.ddx.is_empty() ? _resolve_zero_expr(res.type) : f_branch.ddx;
								String tdy = t_branch.ddy.is_empty() ? _resolve_zero_expr(res.type) : t_branch.ddy;
								String fdy = f_branch.ddy.is_empty() ? _resolve_zero_expr(res.type) : f_branch.ddy;
								res.ddx = "(" + cond.val + "?" + tdx + ":" + fdx + ")";
								res.ddy = "(" + cond.val + "?" + tdy + ":" + fdy + ")";
								res.exact = false;
							}
						} break;

						case SL::OP_EMPTY: {
						} break;

						default: {
							ResolveExpr a = emit_node(onode->arguments[0], p_level, p_assigning);
							ResolveExpr b = emit_node(onode->arguments[1], p_level, p_assigning);
							if (p_use_scope) {
								res.val += "(";
							}
							res.val += a.val + " " + _opstr(onode->op) + " " + b.val;
							if (p_use_scope) {
								res.val += ")";
							}

							if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
								const String za = _resolve_zero_expr(a.type);
								const String zb = _resolve_zero_expr(b.type);
								const String adx = a.ddx.is_empty() ? za : a.ddx;
								const String ady = a.ddy.is_empty() ? za : a.ddy;
								const String bdx = b.ddx.is_empty() ? zb : b.ddx;
								const String bdy = b.ddy.is_empty() ? zb : b.ddy;
								const bool a_zero = (adx == za && ady == za);
								const bool b_zero = (bdx == zb && bdy == zb);

								if (onode->op == SL::OP_ADD || onode->op == SL::OP_SUB) {
									if (a_zero && b_zero) {
										res.ddx = _resolve_zero_expr(res.type);
										res.ddy = _resolve_zero_expr(res.type);
									} else {
										res.ddx = "(" + adx + " " + _opstr(onode->op) + " " + bdx + ")";
										res.ddy = "(" + ady + " " + _opstr(onode->op) + " " + bdy + ")";
									}
									res.exact = a.exact && b.exact;
									res.has_varying = a.has_varying || b.has_varying;
								} else if (onode->op == SL::OP_MUL) {
									if (a_zero && b_zero) {
										res.ddx = _resolve_zero_expr(res.type);
										res.ddy = _resolve_zero_expr(res.type);
									} else if (a_zero) {
										res.ddx = "(" + a.val + " * " + bdx + ")";
										res.ddy = "(" + a.val + " * " + bdy + ")";
									} else if (b_zero) {
										res.ddx = "(" + adx + " * " + b.val + ")";
										res.ddy = "(" + ady + " * " + b.val + ")";
									} else {
										res.ddx = "(" + adx + " * " + b.val + " + " + a.val + " * " + bdx + ")";
										res.ddy = "(" + ady + " * " + b.val + " + " + a.val + " * " + bdy + ")";
									}
									res.exact = a.exact && b.exact;
									res.has_varying = a.has_varying || b.has_varying;
								} else if (onode->op == SL::OP_DIV && (!_is_resolve_matrix_type(res.type) || b.type == SL::TYPE_FLOAT)) {
									if (a_zero && b_zero) {
										res.ddx = _resolve_zero_expr(res.type);
										res.ddy = _resolve_zero_expr(res.type);
									} else if (b_zero) {
										res.ddx = "(" + adx + " / " + b.val + ")";
										res.ddy = "(" + ady + " / " + b.val + ")";
									} else {
										res.ddx = "((" + adx + " * " + b.val + " - " + a.val + " * " + bdx + ") / (" + b.val + " * " + b.val + "))";
										res.ddy = "((" + ady + " * " + b.val + " - " + a.val + " * " + bdy + ") / (" + b.val + " * " + b.val + "))";
									}
									res.exact = a.exact && b.exact;
									res.has_varying = a.has_varying || b.has_varying;
								} else {
									res.ddx = _resolve_zero_expr(res.type);
									res.ddy = _resolve_zero_expr(res.type);
									res.exact = false;
								}
							}
						} break;
					}
				} break;

				case SL::Node::NODE_TYPE_CONTROL_FLOW: {
					const SL::ControlFlowNode *cfnode = static_cast<const SL::ControlFlowNode *>(p_node);
					if (cfnode->flow_op == SL::FLOW_OP_IF) {
						ResolveExpr cond = emit_node(cfnode->expressions[0], p_level, p_assigning);
						res.val += _mktab(p_level) + "if (" + cond.val + ")\n";
						cf_depth++;
						res.val += emit_node(cfnode->blocks[0], p_level + 1, p_assigning).val;
						if (cfnode->blocks.size() == 2) {
							res.val += _mktab(p_level) + "else\n";
							res.val += emit_node(cfnode->blocks[1], p_level + 1, p_assigning).val;
						}
						cf_depth--;
					} else if (cfnode->flow_op == SL::FLOW_OP_SWITCH) {
						ResolveExpr expr = emit_node(cfnode->expressions[0], p_level, p_assigning);
						res.val += _mktab(p_level) + "switch (" + expr.val + ")\n";
						cf_depth++;
						res.val += emit_node(cfnode->blocks[0], p_level + 1, p_assigning).val;
						cf_depth--;
					} else if (cfnode->flow_op == SL::FLOW_OP_CASE) {
						res.val += _mktab(p_level) + "case " + emit_node(cfnode->expressions[0], p_level, p_assigning).val + ":\n";
						res.val += emit_node(cfnode->blocks[0], p_level + 1, p_assigning).val;
					} else if (cfnode->flow_op == SL::FLOW_OP_DEFAULT) {
						res.val += _mktab(p_level) + "default:\n";
						res.val += emit_node(cfnode->blocks[0], p_level + 1, p_assigning).val;
					} else if (cfnode->flow_op == SL::FLOW_OP_DO) {
						res.val += _mktab(p_level) + "do";
						cf_depth++;
						res.val += emit_node(cfnode->blocks[0], p_level + 1, p_assigning).val;
						res.val += _mktab(p_level) + "while (" + emit_node(cfnode->expressions[0], p_level, p_assigning).val + ");";
						cf_depth--;
					} else if (cfnode->flow_op == SL::FLOW_OP_WHILE) {
						cf_depth++;
						res.val += _mktab(p_level) + "while (" + emit_node(cfnode->expressions[0], p_level, p_assigning).val + ")\n";
						res.val += emit_node(cfnode->blocks[0], p_level + 1, p_assigning).val;
						cf_depth--;
					} else if (cfnode->flow_op == SL::FLOW_OP_FOR) {
						scopes.push_back(HashMap<StringName, ResolveVarInfo>());
						String left = emit_node(cfnode->blocks[0], p_level, p_assigning).val;
						cf_depth++;
						String middle = emit_node(cfnode->blocks[1], p_level, p_assigning).val;
						String right = emit_node(cfnode->blocks[2], p_level, p_assigning).val;
						res.val += _mktab(p_level) + "for (" + left + ";" + middle + ";" + right + ")\n";
						res.val += emit_node(cfnode->blocks[3], p_level + 1, p_assigning).val;
						cf_depth--;
						scopes.resize(scopes.size() - 1);
					} else if (cfnode->flow_op == SL::FLOW_OP_RETURN) {
						if (cfnode->expressions.size()) {
							res.val = "return " + emit_node(cfnode->expressions[0], p_level, p_assigning).val + ";";
						} else {
							res.val = "return;";
						}
					} else if (cfnode->flow_op == SL::FLOW_OP_DISCARD) {
						res.val = "discard;";
					} else if (cfnode->flow_op == SL::FLOW_OP_CONTINUE) {
						res.val = "continue;";
					} else if (cfnode->flow_op == SL::FLOW_OP_BREAK) {
						res.val = "break;";
					}
				} break;

				case SL::Node::NODE_TYPE_MEMBER: {
					const SL::MemberNode *mnode = static_cast<const SL::MemberNode *>(p_node);
					res.type = mnode->get_datatype();
					String member_name = (mnode->basetype == SL::TYPE_STRUCT) ? _mkid(mnode->name) : String(mnode->name);
					ResolveExpr owner = emit_node(mnode->owner, p_level, p_assigning);
					res.val = owner.val + "." + member_name;

					if (mnode->index_expression != nullptr) {
						ResolveExpr idx = emit_node(mnode->index_expression, p_level, p_assigning);
						res.val += "[" + idx.val + "]";
						if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
						}
					} else if (mnode->assign_expression != nullptr) {
						ResolveExpr rhs = emit_node(mnode->assign_expression, p_level, true, false);
						String assign_code = res.val + "=" + rhs.val;
						if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
							String dx_lhs, dy_lhs;
							ResolveVarInfo *root_vi = nullptr;
							bool has_comp = get_lvalue_companions(mnode, dx_lhs, dy_lhs, &root_vi);
							String rhs_dx = rhs.ddx.is_empty() ? _resolve_zero_expr(res.type) : rhs.ddx;
							String rhs_dy = rhs.ddy.is_empty() ? _resolve_zero_expr(res.type) : rhs.ddy;
							mark_lhs_assigned(mnode, rhs.exact, rhs.has_varying);
							if (has_comp) {
								res.val = "(" + dx_lhs + "=" + rhs_dx + ", " + dy_lhs + "=" + rhs_dy + ", " + assign_code + ")";
								res.ddx = dx_lhs;
								res.ddy = dy_lhs;
								res.exact = rhs.exact && (!root_vi || cf_depth <= root_vi->declared_cf_depth);
								res.has_varying = rhs.has_varying;
							} else {
								res.val = assign_code;
								res.ddx = rhs_dx;
								res.ddy = rhs_dy;
								res.exact = false;
							}
						} else {
							if (!is_vertex_mode) {
								mark_lhs_assigned(mnode, false, false);
							}
							res.val = assign_code;
						}
					} else if (mnode->call_expression != nullptr) {
						res.val += "." + emit_node(mnode->call_expression, p_level, p_assigning, false).val;
					} else if (!is_vertex_mode && _is_resolve_diff_type(res.type)) {
						if (mnode->basetype != SL::TYPE_STRUCT && !owner.ddx.is_empty()) {
							const String zo = _resolve_zero_expr(owner.type);
							if (owner.ddx == zo && owner.ddy == zo) {
								res.ddx = _resolve_zero_expr(res.type);
								res.ddy = _resolve_zero_expr(res.type);
							} else {
								res.ddx = "(" + owner.ddx + ")." + member_name;
								res.ddy = "(" + owner.ddy + ")." + member_name;
							}
							res.exact = owner.exact;
							res.has_varying = owner.has_varying;
						} else {
							res.ddx = _resolve_zero_expr(res.type);
							res.ddy = _resolve_zero_expr(res.type);
							res.exact = false;
						}
					}
				} break;

				default:
					break;
			}

			return res;
		}
	};

	auto find_vfunc_index = [&](const StringName &p_name) -> int {
		for (int i = 0; i < shader->vfunctions.size(); i++) {
			if (shader->vfunctions[i].name == p_name) {
				return i;
			}
		}
		return -1;
	};

	// Helper to emit resolve-mode helper function dependencies recursively in deterministic order.
	struct HelperDepsEmitter {
		const SL::ShaderNode *shader_node = nullptr;
		ShaderCompiler *self = nullptr;
		GeneratedCode *gen_code = nullptr;
		IdentifierActions *id_actions = nullptr;
		const DefaultIdentifierActions *def_actions = nullptr;
		HashMap<StringName, bool> *frag_to_light_exact_ptr = nullptr;
		HashMap<StringName, bool> *frag_to_light_varying_ptr = nullptr;

		void emit_deps(const StringName &p_for_func, bool p_vertex_mode, String &r_out, HashSet<StringName> &r_added) {
			int fidx = -1;
			for (int i = 0; i < shader_node->vfunctions.size(); i++) {
				if (shader_node->vfunctions[i].name == p_for_func) {
					fidx = i;
					break;
				}
			}
			if (fidx == -1) {
				return;
			}

			Vector<StringName> uses_functions;
			for (const StringName &E : shader_node->vfunctions[fidx].uses_function) {
				uses_functions.push_back(E);
			}
			uses_functions.sort_custom<StringName::AlphCompare>();

			for (int k = 0; k < uses_functions.size(); k++) {
				const StringName &dep_name = uses_functions[k];
				if (r_added.has(dep_name)) {
					continue;
				}
				emit_deps(dep_name, p_vertex_mode, r_out, r_added);

				SL::FunctionNode *fnode = nullptr;
				for (int i = 0; i < shader_node->vfunctions.size(); i++) {
					if (shader_node->vfunctions[i].name == dep_name) {
						fnode = shader_node->vfunctions[i].function;
						break;
					}
				}
				ERR_FAIL_NULL(fnode);

				ResolveEmitter emitter;
				emitter.self = self;
				emitter.shader_node = shader_node;
				emitter.gen_code = gen_code;
				emitter.id_actions = id_actions;
				emitter.def_actions = def_actions;
				emitter.cur_func = fnode;
				emitter.cur_func_name = fnode->name;
				emitter.is_vertex_mode = p_vertex_mode;
				emitter.frag_to_light_exact_ptr = frag_to_light_exact_ptr;
				emitter.frag_to_light_varying_ptr = frag_to_light_varying_ptr;
				emitter.scopes.push_back(HashMap<StringName, ResolveVarInfo>());

				String header = "\n";
				if (fnode->return_type == SL::TYPE_STRUCT) {
					header += _mkid(fnode->return_struct_name);
				} else {
					header += _typestr(fnode->return_type);
				}
				if (fnode->return_array_size > 0) {
					header += "[" + itos(fnode->return_array_size) + "]";
				}
				header += " " + String(p_vertex_mode ? "_ng_rv_" : "_ng_rf_") + _mkid(fnode->rname) + "(";

				bool first_param = true;
				for (int i = 0; i < fnode->arguments.size(); i++) {
					const SL::FunctionNode::Argument &arg = fnode->arguments[i];
					if (!first_param) {
						header += ", ";
					}
					first_param = false;

					header += _constr(arg.is_const);
					if (arg.type == SL::TYPE_STRUCT) {
						header += _qualstr(arg.qualifier) + _mkid(arg.struct_name) + " " + _mkid(arg.name);
					} else {
						header += _qualstr(arg.qualifier) + _prestr(arg.precision) + _typestr(arg.type) + " " + _mkid(arg.name);
					}
					if (arg.array_size > 0) {
						header += "[" + itos(arg.array_size) + "]";
					}

					const bool has_comp = !p_vertex_mode && arg.qualifier == SL::ARGUMENT_QUALIFIER_IN && _is_resolve_diff_type(arg.type) && arg.array_size == 0;
					ResolveVarInfo vi;
					vi.type = arg.type;
					vi.array_size = arg.array_size;
					vi.is_const = arg.is_const;
					vi.has_companion = has_comp;
					vi.exact = false;
					vi.declared_cf_depth = 0;
					if (has_comp) {
						String id = _mkid(arg.name);
						vi.dx_name = "_ng_dx_" + id;
						vi.dy_name = "_ng_dy_" + id;
						header += ", " + _prestr(arg.precision) + _typestr(arg.type) + " " + vi.dx_name;
						header += ", " + _prestr(arg.precision) + _typestr(arg.type) + " " + vi.dy_name;
					}
					emitter.scopes.write[0][arg.name] = vi;
				}
				header += ")\n";
				r_out += header;
				r_out += emitter.emit_node(fnode->body, 2, false).val;
				r_added.insert(dep_name);
			}
		}
	};

	HelperDepsEmitter deps_emitter{ shader, this, &r_gen_code, &p_actions, &p_default_actions, &frag_to_light_exact, &frag_to_light_varying };
	HashSet<StringName> added_rv_funcs;
	HashSet<StringName> added_rf_funcs;

	// 1. vertex() -> code["resolve_vertex"] and _ng_rv_ helpers in fragment globals.
	int vertex_idx = find_vfunc_index(SNAME("vertex"));
	if (vertex_idx != -1) {
		deps_emitter.emit_deps(SNAME("vertex"), true, resolve_frag_globals, added_rv_funcs);
		SL::FunctionNode *vfunc = shader->vfunctions[vertex_idx].function;
		ResolveEmitter emitter;
		emitter.self = this;
		emitter.shader_node = shader;
		emitter.gen_code = &r_gen_code;
		emitter.id_actions = &p_actions;
		emitter.def_actions = &p_default_actions;
		emitter.cur_func = vfunc;
		emitter.cur_func_name = vfunc->name;
		emitter.is_vertex_mode = true;
		emitter.frag_to_light_exact_ptr = &frag_to_light_exact;
		emitter.frag_to_light_varying_ptr = &frag_to_light_varying;
		emitter.scopes.push_back(HashMap<StringName, ResolveVarInfo>());
		r_gen_code.code["resolve_vertex"] = emitter.emit_node(vfunc->body, 2, false).val;
	}

	// 2. fragment() -> wrap code["fragment"] with #ifndef MODE_VISIBILITY_RESOLVE ... #else ... #endif.
	int fragment_idx = find_vfunc_index(SNAME("fragment"));
	if (fragment_idx != -1) {
		deps_emitter.emit_deps(SNAME("fragment"), false, resolve_frag_globals, added_rf_funcs);
		SL::FunctionNode *ffunc = shader->vfunctions[fragment_idx].function;

		HashSet<StringName> assigned_in_frag;
		_collect_assigned_names(ffunc->body, assigned_in_frag);

		ResolveEmitter emitter;
		emitter.self = this;
		emitter.shader_node = shader;
		emitter.gen_code = &r_gen_code;
		emitter.id_actions = &p_actions;
		emitter.def_actions = &p_default_actions;
		emitter.cur_func = ffunc;
		emitter.cur_func_name = ffunc->name;
		emitter.is_vertex_mode = false;
		emitter.is_entry_fragment = true;
		emitter.frag_to_light_exact_ptr = &frag_to_light_exact;
		emitter.frag_to_light_varying_ptr = &frag_to_light_varying;
		emitter.scopes.push_back(HashMap<StringName, ResolveVarInfo>());

		String preamble;
		// Track mutable interpolated built-ins (e.g. NORMAL, TANGENT, BINORMAL) if modified in fragment().
		for (const KeyValue<StringName, Pair<String, String>> &E : p_default_actions.resolve_mutable_builtin_derivatives) {
			if (assigned_in_frag.has(E.key)) {
				SL::DataType bt = SL::TYPE_VEC3;
				String dx_id = "_ng_dx_b_" + String(E.key);
				String dy_id = "_ng_dy_b_" + String(E.key);
				preamble += "\t" + _typestr(bt) + " " + dx_id + " = " + E.value.first + ";\n";
				preamble += "\t" + _typestr(bt) + " " + dy_id + " = " + E.value.second + ";\n";

				ResolveVarInfo vi;
				vi.type = bt;
				vi.array_size = 0;
				vi.is_const = false;
				vi.has_companion = true;
				vi.dx_name = dx_id;
				vi.dy_name = dy_id;
				vi.exact = true;
				vi.has_varying = true;
				vi.declared_cf_depth = 0;
				emitter.scopes.write[0][E.key] = vi;
			}
		}

		// Initialize frag_to_light companion derivatives if present.
		for (int k = 0; k < varying_names.size(); k++) {
			const SL::ShaderNode::Varying &var = shader->varyings[varying_names[k]];
			if (var.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT && _is_resolve_diff_type(var.type) && var.array_size == 0) {
				String id = _mkid(varying_names[k]);
				String z = _resolve_zero_expr(var.type);
				preamble += "\t_ng_dx_frag_to_light." + id + " = " + z + ";\n";
				preamble += "\t_ng_dy_frag_to_light." + id + " = " + z + ";\n";
			}
		}

		String resolve_frag_body = preamble + emitter.emit_node(ffunc->body, 2, false).val;
		r_gen_code.code["resolve_fragment"] = resolve_frag_body;
		r_gen_code.code["fragment"] = "#ifndef MODE_VISIBILITY_RESOLVE\n" + r_gen_code.code["fragment"] + "#else\n" + resolve_frag_body + "#endif\n";
	}

	// 3. light() -> wrap code["light"] with #ifndef MODE_VISIBILITY_RESOLVE ... #else ... #endif.
	int light_idx = find_vfunc_index(SNAME("light"));
	if (light_idx != -1) {
		deps_emitter.emit_deps(SNAME("light"), false, resolve_frag_globals, added_rf_funcs);
		SL::FunctionNode *lfunc = shader->vfunctions[light_idx].function;
		ResolveEmitter emitter;
		emitter.self = this;
		emitter.shader_node = shader;
		emitter.gen_code = &r_gen_code;
		emitter.id_actions = &p_actions;
		emitter.def_actions = &p_default_actions;
		emitter.cur_func = lfunc;
		emitter.cur_func_name = lfunc->name;
		emitter.is_vertex_mode = false;
		emitter.is_entry_light = true;
		emitter.frag_to_light_exact_ptr = &frag_to_light_exact;
		emitter.frag_to_light_varying_ptr = &frag_to_light_varying;
		emitter.scopes.push_back(HashMap<StringName, ResolveVarInfo>());

		String resolve_light_body = emitter.emit_node(lfunc->body, 2, false).val;
		r_gen_code.code["resolve_light"] = resolve_light_body;
		r_gen_code.code["light"] = "#ifndef MODE_VISIBILITY_RESOLVE\n" + r_gen_code.code["light"] + "#else\n" + resolve_light_body + "#endif\n";
	}

	if (!resolve_frag_globals.is_empty()) {
		r_gen_code.stage_globals[STAGE_FRAGMENT] += "#ifdef MODE_VISIBILITY_RESOLVE\n" + resolve_frag_globals + "#endif\n";
	}
}

ShaderLanguage::DataType ShaderCompiler::_get_global_shader_uniform_type(const StringName &p_name) {
	RSE::GlobalShaderParameterType gvt = RSG::material_storage->global_shader_parameter_get_type(p_name);
	return (ShaderLanguage::DataType)RS::global_shader_uniform_type_get_shader_datatype(gvt);
}

Error ShaderCompiler::compile(RSE::ShaderMode p_mode, const String &p_code, IdentifierActions *p_actions, const String &p_path, GeneratedCode &r_gen_code) {
	SL::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(p_mode);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(p_mode);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(p_mode);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	info.global_shader_uniform_type_func = _get_global_shader_uniform_type;
	info.base_varying_index = actions.base_varying_index;

	Error err = parser.compile(p_code, info);

	if (err != OK) {
		Vector<ShaderLanguage::FilePosition> include_positions = parser.get_include_positions();

		String current;
		HashMap<String, Vector<String>> includes;
		includes[""] = Vector<String>();
		Vector<String> include_stack;
		Vector<String> shader_lines = p_code.split("\n");

		// Reconstruct the files.
		for (int i = 0; i < shader_lines.size(); i++) {
			String l = shader_lines[i];
			if (l.begins_with("@@>")) {
				String inc_path = l.replace_first("@@>", "");

				l = "#include \"" + inc_path + "\"";
				includes[current].append("#include \"" + inc_path + "\""); // Restore the include directive
				include_stack.push_back(current);
				current = inc_path;
				includes[inc_path] = Vector<String>();

			} else if (l.begins_with("@@<")) {
				if (include_stack.size()) {
					current = include_stack[include_stack.size() - 1];
					include_stack.resize(include_stack.size() - 1);
				}
			} else {
				includes[current].push_back(l);
			}
		}

		// Print the files.
		for (const KeyValue<String, Vector<String>> &E : includes) {
			int err_line = -1;
			for (const ShaderLanguage::FilePosition &include_position : include_positions) {
				if (include_position.file == E.key) {
					err_line = include_position.line;
				}
			}
			if (err_line < 0) {
				// Skip files that don't contain errors.
				continue;
			}

			if (E.key.is_empty()) {
				if (p_path == "") {
					print_line("--Main Shader--");
				} else {
					print_line("--" + p_path + "--");
				}
			} else {
				print_line("--" + E.key + "--");
			}
			const Vector<String> &V = E.value;
			for (int i = 0; i < V.size(); i++) {
				if (i == err_line - 1) {
					// Mark the error line to be visible without having to look at
					// the trace at the end.
					print_line(vformat("E%4d-> %s", i + 1, V[i]));
				} else if ((i == err_line - 3) || (i == err_line - 2) || (i == err_line) || (i == err_line + 1)) {
					// Print 4 lines around the error line.
					print_line(vformat("%5d | %s", i + 1, V[i]));
				}
			}
		}

		String file;
		int line;
		if (include_positions.size() > 1) {
			file = include_positions[include_positions.size() - 1].file;
			line = include_positions[include_positions.size() - 1].line;
		} else {
			file = p_path;
			line = parser.get_error_line();
		}

		_err_print_error(nullptr, file.utf8().get_data(), line, parser.get_error_text().utf8().get_data(), false, ERR_HANDLER_SHADER);
		return err;
	}

	r_gen_code.defines.clear();
	r_gen_code.code.clear();
	for (int i = 0; i < STAGE_MAX; i++) {
		r_gen_code.stage_globals[i] = String();
	}
	r_gen_code.uses_fragment_time = false;
	r_gen_code.uses_vertex_time = false;
	r_gen_code.uses_global_textures = false;
	r_gen_code.uses_screen_texture_mipmaps = false;
	r_gen_code.uses_screen_texture = false;
	r_gen_code.uses_depth_texture = false;
	r_gen_code.uses_normal_roughness_texture = false;
	r_gen_code.uses_unsupported_derivative = false;

	used_name_defines.clear();
	used_rmode_defines.clear();
	used_smode_defines.clear();
	used_flag_pointers.clear();
	fragment_varyings.clear();

	shader = parser.get_shader();
	function = nullptr;
	// Return value only relevant within nested calls.
	_ALLOW_DISCARD_ _dump_node_code(shader, 1, r_gen_code, *p_actions, actions, false);
	if (p_actions->visibility_resolve) {
		_compile_visibility_resolve(r_gen_code, *p_actions, actions);
	}

	return OK;
}

void ShaderCompiler::initialize(DefaultIdentifierActions p_actions) {
	actions = p_actions;

	time_name = "TIME";

	List<String> func_list;

	ShaderLanguage::get_builtin_funcs(&func_list);

	for (const String &E : func_list) {
		internal_functions.insert(E);
	}
	texture_functions.insert("texture");
	texture_functions.insert("textureProj");
	texture_functions.insert("textureLod");
	texture_functions.insert("textureProjLod");
	texture_functions.insert("textureGrad");
	texture_functions.insert("textureProjGrad");
	texture_functions.insert("textureGather");
	texture_functions.insert("textureSize");
	texture_functions.insert("textureQueryLod");
	texture_functions.insert("textureQueryLevels");
	texture_functions.insert("texelFetch");
}

ShaderCompiler::ShaderCompiler() {
}
