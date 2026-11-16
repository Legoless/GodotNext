/**************************************************************************/
/*  next_gi_hit_materials.cpp                                             */
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

#include "next_gi_hit_materials.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_hit_base.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_hit_material.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"

namespace RendererRD {

/* Device-free helpers. */

NextGIHitAvailability nextgi_hit_availability(uint32_t p_consumers, bool p_hwrt_tier, bool p_raytracing_pipeline, bool p_pipeline_library, bool p_bindless, bool p_init_failed) {
	if (p_consumers == 0) {
		return NEXTGI_HIT_NO_CONSUMER;
	}
	if (!p_hwrt_tier) {
		return NEXTGI_HIT_NOT_HWRT;
	}
	if (!p_raytracing_pipeline) {
		return NEXTGI_HIT_NO_RAYTRACING_PIPELINE;
	}
	if (!p_pipeline_library) {
		return NEXTGI_HIT_NO_PIPELINE_LIBRARY;
	}
	if (!p_bindless) {
		return NEXTGI_HIT_NO_BINDLESS;
	}
	if (p_init_failed) {
		return NEXTGI_HIT_INIT_FAILED;
	}
	return NEXTGI_HIT_AVAILABLE;
}

const char *nextgi_hit_availability_name(NextGIHitAvailability p_availability) {
	switch (p_availability) {
		case NEXTGI_HIT_AVAILABLE:
			return "AVAILABLE";
		case NEXTGI_HIT_NO_CONSUMER:
			return "NO_CONSUMER";
		case NEXTGI_HIT_NOT_HWRT:
			return "NOT_HWRT";
		case NEXTGI_HIT_NO_RAYTRACING_PIPELINE:
			return "NO_RAYTRACING_PIPELINE";
		case NEXTGI_HIT_NO_PIPELINE_LIBRARY:
			return "NO_PIPELINE_LIBRARY";
		case NEXTGI_HIT_NO_BINDLESS:
			return "NO_BINDLESS";
		case NEXTGI_HIT_INIT_FAILED:
			return "INIT_FAILED";
		case NEXTGI_HIT_AVAILABILITY_MAX:
			break;
	}
	return "UNKNOWN";
}

int16_t nextgi_hit_snorm16(float p_value) {
	const float clamped = CLAMP(p_value, -1.0f, 1.0f);
	return int16_t(Math::round(clamped * 32767.0f));
}

float nextgi_hit_snorm16_to_float(int16_t p_value) {
	// As unpackSnorm2x16: -32768 clamps to -1.
	return MAX(float(p_value) / 32767.0f, -1.0f);
}

int16_t nextgi_hit_unorm16_to_snorm16(uint16_t p_value) {
	return nextgi_hit_snorm16(float(p_value) / 65535.0f * 2.0f - 1.0f);
}

Vector3 nextgi_hit_oct_decode(const Vector2 &p_oct) {
	Vector3 v(p_oct.x, p_oct.y, 1.0f - Math::abs(p_oct.x) - Math::abs(p_oct.y));
	const float t = MAX(-v.z, 0.0f);
	v.x += t * (v.x > 0.0f ? -1.0f : (v.x < 0.0f ? 1.0f : 0.0f));
	v.y += t * (v.y > 0.0f ? -1.0f : (v.y < 0.0f ? 1.0f : 0.0f));
	return v.normalized();
}

// The tangent's y keeps its sign through the snorm16 conversion: 0 would lose the binormal sign.
static int16_t _nextgi_hit_signed_tangent_y(float p_value) {
	int16_t y = nextgi_hit_snorm16(p_value);
	if (y == 0) {
		y = p_value < 0.0f ? -1 : 1;
	}
	return y;
}

uint16_t nextgi_hit_pack_uv(float p_value, float p_min, float p_extent) {
	if (!(p_extent > 0.0f)) {
		return 0;
	}
	const float unit = CLAMP((p_value - p_min) / p_extent, 0.0f, 1.0f);
	return uint16_t(Math::round(unit * 65535.0f));
}

float nextgi_hit_unpack_uv(uint16_t p_value, float p_min, float p_extent) {
	return p_min + float(p_value) / 65535.0f * p_extent;
}

Vector2 nextgi_hit_unpack_uv0(const NextGIHitStreamHeader &p_header, const NextGIHitAttributes &p_vertex) {
	return Vector2(nextgi_hit_unpack_uv(p_vertex.uv[0], p_header.uv_min[0], p_header.uv_extent[0]), nextgi_hit_unpack_uv(p_vertex.uv[1], p_header.uv_min[1], p_header.uv_extent[1]));
}

Vector2 nextgi_hit_unpack_uv1(const NextGIHitStreamHeader &p_header, const NextGIHitAttributes &p_vertex) {
	return Vector2(nextgi_hit_unpack_uv(p_vertex.uv2[0], p_header.uv2_min[0], p_header.uv2_extent[0]), nextgi_hit_unpack_uv(p_vertex.uv2[1], p_header.uv2_min[1], p_header.uv2_extent[1]));
}

void nextgi_hit_pack_vertex(const NextGIHitStreamHeader &p_header, const Vector3 &p_normal, const Vector3 &p_tangent, float p_binormal_sign, const Vector2 &p_uv, const Vector2 &p_uv2, const Color &p_color, NextGIHitAttributes &r_vertex) {
	// Godot's octahedral encodings ([0, 1]), as the uncompressed vertex buffer stores them, mapped to [-1, 1].
	const Vector3 normal = p_normal.length_squared() > 0.0f ? p_normal.normalized() : Vector3(0, 0, 1);
	const Vector2 n = normal.octahedron_encode();
	r_vertex.normal[0] = nextgi_hit_snorm16(n.x * 2.0f - 1.0f);
	r_vertex.normal[1] = nextgi_hit_snorm16(n.y * 2.0f - 1.0f);
	const Vector3 tangent = p_tangent.length_squared() > 0.0f ? p_tangent.normalized() : Vector3(1, 0, 0);
	const Vector2 t = tangent.octahedron_tangent_encode(p_binormal_sign);
	r_vertex.tangent[0] = nextgi_hit_snorm16(t.x * 2.0f - 1.0f);
	r_vertex.tangent[1] = _nextgi_hit_signed_tangent_y(t.y * 2.0f - 1.0f);
	r_vertex.uv[0] = nextgi_hit_pack_uv(p_uv.x, p_header.uv_min[0], p_header.uv_extent[0]);
	r_vertex.uv[1] = nextgi_hit_pack_uv(p_uv.y, p_header.uv_min[1], p_header.uv_extent[1]);
	r_vertex.uv2[0] = nextgi_hit_pack_uv(p_uv2.x, p_header.uv2_min[0], p_header.uv2_extent[0]);
	r_vertex.uv2[1] = nextgi_hit_pack_uv(p_uv2.y, p_header.uv2_min[1], p_header.uv2_extent[1]);
	r_vertex.color[0] = uint8_t(Math::round(CLAMP(p_color.r, 0.0f, 1.0f) * 255.0f));
	r_vertex.color[1] = uint8_t(Math::round(CLAMP(p_color.g, 0.0f, 1.0f) * 255.0f));
	r_vertex.color[2] = uint8_t(Math::round(CLAMP(p_color.b, 0.0f, 1.0f) * 255.0f));
	r_vertex.color[3] = uint8_t(Math::round(CLAMP(p_color.a, 0.0f, 1.0f) * 255.0f));
}

void nextgi_hit_unpack_tbn(const NextGIHitAttributes &p_vertex, Vector3 &r_normal, Vector3 &r_tangent, float &r_binormal_sign) {
	r_normal = nextgi_hit_oct_decode(Vector2(nextgi_hit_snorm16_to_float(p_vertex.normal[0]), nextgi_hit_snorm16_to_float(p_vertex.normal[1])));
	const float ty = nextgi_hit_snorm16_to_float(p_vertex.tangent[1]);
	r_tangent = nextgi_hit_oct_decode(Vector2(nextgi_hit_snorm16_to_float(p_vertex.tangent[0]), Math::abs(ty) * 2.0f - 1.0f));
	r_binormal_sign = ty < 0.0f ? -1.0f : 1.0f;
}

bool nextgi_hit_pack_arrays(const Array &p_arrays, uint32_t p_vertex_count, Vector<uint8_t> &r_stream) {
	r_stream.clear();
	if (p_arrays.size() != RSE::ARRAY_MAX || p_vertex_count == 0) {
		return false;
	}
	const Vector<Vector3> normals = p_arrays[RSE::ARRAY_NORMAL];
	const Vector<float> tangents = p_arrays[RSE::ARRAY_TANGENT];
	const Vector<Vector2> uvs = p_arrays[RSE::ARRAY_TEX_UV];
	const Vector<Vector2> uv2s = p_arrays[RSE::ARRAY_TEX_UV2];
	const Vector<Color> colors = p_arrays[RSE::ARRAY_COLOR];
	if ((!normals.is_empty() && uint32_t(normals.size()) != p_vertex_count) || (!tangents.is_empty() && uint32_t(tangents.size()) != p_vertex_count * 4) || (!uvs.is_empty() && uint32_t(uvs.size()) != p_vertex_count) || (!uv2s.is_empty() && uint32_t(uv2s.size()) != p_vertex_count) || (!colors.is_empty() && uint32_t(colors.size()) != p_vertex_count)) {
		return false;
	}
	NextGIHitStreamHeader header;
	auto range = [](const Vector<Vector2> &p_uvs, float *r_min, float *r_extent) {
		if (p_uvs.is_empty()) {
			return;
		}
		Vector2 lo = p_uvs[0];
		Vector2 hi = p_uvs[0];
		for (const Vector2 &uv : p_uvs) {
			lo = lo.min(uv);
			hi = hi.max(uv);
		}
		r_min[0] = lo.x;
		r_min[1] = lo.y;
		r_extent[0] = hi.x - lo.x;
		r_extent[1] = hi.y - lo.y;
	};
	range(uvs, header.uv_min, header.uv_extent);
	range(uv2s, header.uv2_min, header.uv2_extent);
	r_stream.resize(nextgi_hit_stream_size(p_vertex_count));
	memcpy(r_stream.ptrw(), &header, sizeof(header));
	NextGIHitAttributes *out = reinterpret_cast<NextGIHitAttributes *>(r_stream.ptrw() + NEXTGI_HIT_ATTRIBUTE_HEADER);
	for (uint32_t i = 0; i < p_vertex_count; i++) {
		const Vector3 normal = normals.is_empty() ? Vector3(0, 0, 1) : normals[i];
		const Vector3 tangent = tangents.is_empty() ? Vector3(1, 0, 0) : Vector3(tangents[i * 4 + 0], tangents[i * 4 + 1], tangents[i * 4 + 2]);
		const float sign = tangents.is_empty() ? 1.0f : (tangents[i * 4 + 3] < 0.0f ? -1.0f : 1.0f);
		nextgi_hit_pack_vertex(header, normal, tangent, sign, uvs.is_empty() ? Vector2() : uvs[i], uv2s.is_empty() ? Vector2() : uv2s[i], colors.is_empty() ? Color(1, 1, 1, 1) : colors[i], out[i]);
	}
	return true;
}

NextGIHitSourceLayout nextgi_hit_source_layout(uint64_t p_format, uint32_t p_vertex_count) {
	NextGIHitSourceLayout layout;
	if ((p_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) || (p_format & RSE::ARRAY_FLAG_USE_2D_VERTICES) || !(p_format & RSE::ARRAY_FORMAT_VERTEX)) {
		return layout;
	}
	layout.valid = true;
	// As RenderingServer::mesh_surface_make_offsets_from_format(): float positions, then the normal block.
	layout.normal_block_offset = p_vertex_count * uint32_t(sizeof(float) * 3);
	if (p_format & RSE::ARRAY_FORMAT_NORMAL) {
		layout.normal_offset = int32_t(layout.normal_stride);
		layout.normal_stride += 4;
	}
	if (p_format & RSE::ARRAY_FORMAT_TANGENT) {
		layout.tangent_offset = int32_t(layout.normal_stride);
		layout.normal_stride += 4;
	}
	if (p_format & RSE::ARRAY_FORMAT_COLOR) {
		layout.color_offset = int32_t(layout.attribute_stride);
		layout.attribute_stride += 4;
	}
	if (p_format & RSE::ARRAY_FORMAT_TEX_UV) {
		layout.uv_offset = int32_t(layout.attribute_stride);
		layout.attribute_stride += 8;
	}
	if (p_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		layout.uv2_offset = int32_t(layout.attribute_stride);
		layout.attribute_stride += 8;
	}
	for (int i = 0; i < RSE::ARRAY_CUSTOM_COUNT; i++) {
		if (!(p_format & (1ULL << (RSE::ARRAY_CUSTOM0 + i)))) {
			continue;
		}
		const uint64_t custom = (p_format >> (RSE::ARRAY_FORMAT_CUSTOM_BASE + RSE::ARRAY_FORMAT_CUSTOM_BITS * i)) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
		switch (custom) {
			case RSE::ARRAY_CUSTOM_RGBA_HALF:
			case RSE::ARRAY_CUSTOM_RG_FLOAT:
				layout.attribute_stride += 8;
				break;
			case RSE::ARRAY_CUSTOM_RGB_FLOAT:
				layout.attribute_stride += 12;
				break;
			case RSE::ARRAY_CUSTOM_RGBA_FLOAT:
				layout.attribute_stride += 16;
				break;
			default:
				layout.attribute_stride += 4;
				break;
		}
	}
	return layout;
}

// Whether [p_start, p_start + p_size) lies inside the update [p_offset, p_offset + p_count).
static _FORCE_INLINE_ bool _nextgi_hit_covered(uint64_t p_start, uint32_t p_size, uint64_t p_offset, uint32_t p_count) {
	return p_start >= p_offset && p_start + p_size <= p_offset + p_count;
}

// Requantizes one UV set of every vertex to a range that also holds p_new_min and p_new_max.
static void _nextgi_hit_widen_uv_range(uint8_t *r_stream, uint32_t p_vertex_count, int p_set, int p_axis, float p_new_min, float p_new_max) {
	NextGIHitStreamHeader &header = *reinterpret_cast<NextGIHitStreamHeader *>(r_stream);
	NextGIHitAttributes *vertices = reinterpret_cast<NextGIHitAttributes *>(r_stream + NEXTGI_HIT_ATTRIBUTE_HEADER);
	float &min = p_set == 0 ? header.uv_min[p_axis] : header.uv2_min[p_axis];
	float &extent = p_set == 0 ? header.uv_extent[p_axis] : header.uv2_extent[p_axis];
	const float old_min = min;
	const float old_extent = extent;
	const float new_min = MIN(old_min, p_new_min);
	const float new_extent = MAX(old_min + old_extent, p_new_max) - new_min;
	for (uint32_t v = 0; v < p_vertex_count; v++) {
		uint16_t &value = p_set == 0 ? vertices[v].uv[p_axis] : vertices[v].uv2[p_axis];
		value = nextgi_hit_pack_uv(nextgi_hit_unpack_uv(value, old_min, old_extent), new_min, new_extent);
	}
	min = new_min;
	extent = new_extent;
}

bool nextgi_hit_patch_stream(const NextGIHitSourceLayout &p_layout, uint32_t p_vertex_count, bool p_attribute_region, uint32_t p_offset, const uint8_t *p_data, uint32_t p_size, uint8_t *r_stream, uint32_t &r_byte_begin, uint32_t &r_byte_end) {
	if (!p_layout.valid || p_data == nullptr || p_size == 0 || r_stream == nullptr || p_vertex_count == 0) {
		return false;
	}
	const uint32_t stride = p_attribute_region ? p_layout.attribute_stride : p_layout.normal_stride;
	if (stride == 0) {
		return false;
	}
	const uint64_t block = p_attribute_region ? 0 : p_layout.normal_block_offset;
	const uint64_t region_end = uint64_t(p_offset) + p_size;
	if (region_end <= block) {
		return false; // Positions only.
	}
	const uint64_t start = MAX(uint64_t(p_offset), block) - block;
	const uint32_t first = uint32_t(MIN(start / stride, uint64_t(p_vertex_count)));
	const uint32_t last = uint32_t(MIN((region_end - block - 1) / stride, uint64_t(p_vertex_count) - 1));
	auto read_u16 = [&](uint64_t p_at) {
		uint16_t value;
		memcpy(&value, p_data + (p_at - p_offset), sizeof(uint16_t));
		return value;
	};
	auto read_f32 = [&](uint64_t p_at) {
		float value;
		memcpy(&value, p_data + (p_at - p_offset), sizeof(float));
		return value;
	};
	const NextGIHitStreamHeader &header = *reinterpret_cast<const NextGIHitStreamHeader *>(r_stream);
	NextGIHitAttributes *vertices = reinterpret_cast<NextGIHitAttributes *>(r_stream + NEXTGI_HIT_ATTRIBUTE_HEADER);

	// A UV the update brings outside its set's range widens the range first (every vertex of that set).
	bool widened = false;
	if (p_attribute_region) {
		for (int set = 0; set < 2; set++) {
			const int32_t offset = set == 0 ? p_layout.uv_offset : p_layout.uv2_offset;
			if (offset < 0) {
				continue;
			}
			for (int axis = 0; axis < 2; axis++) {
				float lo = 0.0f;
				float hi = 0.0f;
				bool any = false;
				for (uint32_t v = first; v <= last && v < p_vertex_count; v++) {
					const uint64_t at = uint64_t(v) * stride + offset + axis * 4;
					if (_nextgi_hit_covered(at, 4, p_offset, p_size)) {
						const float value = read_f32(at);
						lo = any ? MIN(lo, value) : value;
						hi = any ? MAX(hi, value) : value;
						any = true;
					}
				}
				const float min = set == 0 ? header.uv_min[axis] : header.uv2_min[axis];
				const float extent = set == 0 ? header.uv_extent[axis] : header.uv2_extent[axis];
				if (any && (lo < min || hi > min + extent)) {
					_nextgi_hit_widen_uv_range(r_stream, p_vertex_count, set, axis, lo, hi);
					widened = true;
				}
			}
		}
	}

	bool changed = false;
	uint32_t changed_first = UINT32_MAX;
	uint32_t changed_last = 0;
	for (uint32_t v = first; v <= last && v < p_vertex_count; v++) {
		NextGIHitAttributes &out = vertices[v];
		const uint64_t element = block + uint64_t(v) * stride;
		bool vertex_changed = false;
		// One source component at a time: the stream's components map one to one.
		if (!p_attribute_region) {
			if (p_layout.normal_offset >= 0) {
				for (int c = 0; c < 2; c++) {
					const uint64_t at = element + p_layout.normal_offset + c * 2;
					if (_nextgi_hit_covered(at, 2, p_offset, p_size)) {
						out.normal[c] = nextgi_hit_unorm16_to_snorm16(read_u16(at));
						vertex_changed = true;
					}
				}
			}
			if (p_layout.tangent_offset >= 0) {
				for (int c = 0; c < 2; c++) {
					const uint64_t at = element + p_layout.tangent_offset + c * 2;
					if (_nextgi_hit_covered(at, 2, p_offset, p_size)) {
						const uint16_t value = read_u16(at);
						out.tangent[c] = c == 1 ? _nextgi_hit_signed_tangent_y(float(value) / 65535.0f * 2.0f - 1.0f) : nextgi_hit_unorm16_to_snorm16(value);
						vertex_changed = true;
					}
				}
			}
		} else {
			if (p_layout.color_offset >= 0) {
				for (int c = 0; c < 4; c++) {
					const uint64_t at = element + p_layout.color_offset + c;
					if (_nextgi_hit_covered(at, 1, p_offset, p_size)) {
						out.color[c] = p_data[at - p_offset];
						vertex_changed = true;
					}
				}
			}
			if (p_layout.uv_offset >= 0) {
				for (int c = 0; c < 2; c++) {
					const uint64_t at = element + p_layout.uv_offset + c * 4;
					if (_nextgi_hit_covered(at, 4, p_offset, p_size)) {
						out.uv[c] = nextgi_hit_pack_uv(read_f32(at), header.uv_min[c], header.uv_extent[c]);
						vertex_changed = true;
					}
				}
			}
			if (p_layout.uv2_offset >= 0) {
				for (int c = 0; c < 2; c++) {
					const uint64_t at = element + p_layout.uv2_offset + c * 4;
					if (_nextgi_hit_covered(at, 4, p_offset, p_size)) {
						out.uv2[c] = nextgi_hit_pack_uv(read_f32(at), header.uv2_min[c], header.uv2_extent[c]);
						vertex_changed = true;
					}
				}
			}
		}
		if (vertex_changed) {
			changed = true;
			changed_first = MIN(changed_first, v);
			changed_last = MAX(changed_last, v);
		}
	}
	if (widened) {
		// The header and every vertex changed.
		r_byte_begin = 0;
		r_byte_end = nextgi_hit_stream_size(p_vertex_count);
		return true;
	}
	if (!changed) {
		return false;
	}
	r_byte_begin = NEXTGI_HIT_ATTRIBUTE_HEADER + changed_first * NEXTGI_HIT_ATTRIBUTE_STRIDE;
	r_byte_end = NEXTGI_HIT_ATTRIBUTE_HEADER + (changed_last + 1) * NEXTGI_HIT_ATTRIBUTE_STRIDE;
	return true;
}

uint32_t nextgi_hit_pack_r11g11b10(const Color &p_color) {
	auto bits = [](float p_value, uint32_t p_shift) {
		const float clamped = CLAMP(p_value, 0.0f, 65000.0f);
		return uint32_t(Math::make_half_float(clamped) & 0x7FFFu) >> p_shift;
	};
	return bits(p_color.r, 4) | (bits(p_color.g, 4) << 11) | (bits(p_color.b, 5) << 22);
}

Color nextgi_hit_unpack_r11g11b10(uint32_t p_packed) {
	const float r = Math::half_to_float(uint16_t((p_packed & 0x7FFu) << 4));
	const float g = Math::half_to_float(uint16_t(((p_packed >> 11) & 0x7FFu) << 4));
	const float b = Math::half_to_float(uint16_t(((p_packed >> 22) & 0x3FFu) << 5));
	return Color(r, g, b, 1.0f);
}

const char *nextgi_hit_ineligible_name(NextGIHitIneligible p_reason) {
	switch (p_reason) {
		case NEXTGI_HIT_ELIGIBLE:
			return "ELIGIBLE";
		case NEXTGI_HIT_INELIGIBLE_SCREEN_READ:
			return "SCREEN_READ";
		case NEXTGI_HIT_INELIGIBLE_DERIVATIVE:
			return "DERIVATIVE";
		case NEXTGI_HIT_INELIGIBLE_DISCARD:
			return "DISCARD";
		case NEXTGI_HIT_INELIGIBLE_DEPTH_WRITE:
			return "DEPTH_WRITE";
		case NEXTGI_HIT_INELIGIBLE_POSITION_WRITE:
			return "POSITION_WRITE";
		case NEXTGI_HIT_INELIGIBLE_TEXTURE_TYPE:
			return "TEXTURE_TYPE";
		case NEXTGI_HIT_INELIGIBLE_INSTANCE_UNIFORM:
			return "INSTANCE_UNIFORM";
		case NEXTGI_HIT_INELIGIBLE_BUILTIN:
			return "BUILTIN";
		case NEXTGI_HIT_INELIGIBLE_RENDER_TARGET:
			return "RENDER_TARGET";
		case NEXTGI_HIT_INELIGIBLE_COMPILE_FAILED:
			return "COMPILE_FAILED";
		case NEXTGI_HIT_INELIGIBLE_NOT_SPATIAL:
			return "NOT_SPATIAL";
		case NEXTGI_HIT_INELIGIBLE_TABLE_FULL:
			return "TABLE_FULL";
		case NEXTGI_HIT_INELIGIBLE_MAX:
			break;
	}
	return "UNKNOWN";
}

// Whether p_code uses p_token as a whole identifier.
static bool _nextgi_hit_uses_token(const String &p_code, const String &p_token) {
	int from = 0;
	while (true) {
		const int at = p_code.find(p_token, from);
		if (at < 0) {
			return false;
		}
		const int end = at + p_token.length();
		const bool start_ok = at == 0 || !(is_ascii_identifier_char(p_code[at - 1]));
		const bool end_ok = end >= p_code.length() || !(is_ascii_identifier_char(p_code[end]));
		if (start_ok && end_ok) {
			return true;
		}
		from = at + 1;
	}
}

NextGIHitIneligible nextgi_hit_classify(const NextGIHitShaderSource &p_source) {
	const String code = p_source.globals + "\n" + p_source.vertex_code + "\n" + p_source.fragment_code;
	// The scene_forward_clustered names (SceneShaderForwardClustered's renames) that read the screen.
	static const char *screen_tokens[] = { "screen_uv", "gl_FragCoord", "color_buffer", "depth_buffer", "normal_roughness_buffer", "multiview_uv", "multiviewSampler" };
	if (p_source.screen_read) {
		return NEXTGI_HIT_INELIGIBLE_SCREEN_READ;
	}
	for (const char *token : screen_tokens) {
		if (_nextgi_hit_uses_token(code, token)) {
			return NEXTGI_HIT_INELIGIBLE_SCREEN_READ;
		}
	}
	static const char *derivative_tokens[] = { "dFdx", "dFdy", "fwidth", "dFdxCoarse", "dFdyCoarse", "fwidthCoarse", "dFdxFine", "dFdyFine", "fwidthFine", "textureQueryLod" };
	for (const char *token : derivative_tokens) {
		if (_nextgi_hit_uses_token(code, token)) {
			return NEXTGI_HIT_INELIGIBLE_DERIVATIVE;
		}
	}
	if (p_source.uses_discard || _nextgi_hit_uses_token(code, "discard")) {
		return NEXTGI_HIT_INELIGIBLE_DISCARD;
	}
	if (p_source.writes_depth || _nextgi_hit_uses_token(code, "gl_FragDepth")) {
		return NEXTGI_HIT_INELIGIBLE_DEPTH_WRITE;
	}
	if (p_source.writes_position) {
		return NEXTGI_HIT_INELIGIBLE_POSITION_WRITE;
	}
	for (const ShaderCompiler::GeneratedCode::Texture &texture : p_source.textures) {
		if (texture.type != ShaderLanguage::TYPE_SAMPLER2D || texture.array_size > 0 || texture.global) {
			return NEXTGI_HIT_INELIGIBLE_TEXTURE_TYPE;
		}
	}
	if (p_source.instance_uniforms) {
		return NEXTGI_HIT_INELIGIBLE_INSTANCE_UNIFORM;
	}
	// Built-ins the hit template has no value for: per-draw data, bones and custom attributes, the scene's
	// view data and the multiview index.
	static const char *builtin_tokens[] = { "gl_FrontFacing", "gl_PointCoord", "point_coord", "INSTANCE_INDEX", "VERTEX_INDEX", "instances", "instance_index_interp", "scene_data", "scene_data_block", "bone_attrib", "weight_attrib", "custom0_attrib", "custom1_attrib", "custom2_attrib", "custom3_attrib", "sc_luminance_multiplier", "frag_to_light", "gl_ViewIndex" };
	for (const char *token : builtin_tokens) {
		if (_nextgi_hit_uses_token(code, token)) {
			return NEXTGI_HIT_INELIGIBLE_BUILTIN;
		}
	}
	return NEXTGI_HIT_ELIGIBLE;
}

uint32_t nextgi_hit_element_inputs(const NextGIHitShaderSource &p_source) {
	const String code = p_source.globals + "\n" + p_source.vertex_code + "\n" + p_source.fragment_code;
	uint32_t inputs = 0;
	// SceneShaderForwardClustered's renames of COLOR and INSTANCE_CUSTOM.
	if (_nextgi_hit_uses_token(code, "color_interp")) {
		inputs |= NEXTGI_HIT_READS_COLOR;
	}
	if (_nextgi_hit_uses_token(code, "instance_custom")) {
		inputs |= NEXTGI_HIT_READS_INSTANCE_CUSTOM;
	}
	return inputs;
}

bool nextgi_hit_element_needs_fallback(uint32_t p_inputs, bool p_element_colors, bool p_element_custom_data) {
	return (p_element_colors && (p_inputs & NEXTGI_HIT_READS_COLOR)) || (p_element_custom_data && (p_inputs & NEXTGI_HIT_READS_INSTANCE_CUSTOM));
}

String nextgi_hit_texture_defines(const NextGIHitShaderSource &p_source) {
	String defines;
	for (int i = 0; i < p_source.textures.size(); i++) {
		defines += vformat("#define m_%s hit_textures[nonuniformEXT(hit_texture_slots.data[hit_texture_base + %du])]\n", String(p_source.textures[i].name), i);
	}
	return defines;
}

uint64_t nextgi_hit_group_list_key(const uint32_t *p_groups, uint32_t p_count) {
	uint64_t key = hash_murmur3_one_64(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		key = hash_murmur3_one_64(p_groups[i], key);
	}
	return hash_fmix32(uint32_t(key)) | (uint64_t(hash_fmix32(uint32_t(key >> 32))) << 32);
}

uint32_t nextgi_hit_plan_check_jobs(const NextGIHitCheckCard *p_cards, uint32_t p_count, uint32_t p_max_samples, LocalVector<uint32_t> &r_jobs) {
	r_jobs.clear();
	uint32_t samples = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		const NextGIHitCheckCard &card = p_cards[i];
		if (card.width == 0 || card.height == 0 || card.width > 0xFFFF || card.height > 0xFFFF) {
			continue;
		}
		const uint64_t texels = uint64_t(card.width) * card.height;
		if (uint64_t(samples) + texels > p_max_samples) {
			continue;
		}
		r_jobs.push_back(card.record);
		r_jobs.push_back(card.card);
		r_jobs.push_back(samples);
		r_jobs.push_back(card.width | (card.height << 16));
		samples += uint32_t(texels);
	}
	return samples;
}

const char *nextgi_hit_check_counter_name(NextGIHitCheckCounter p_counter) {
	switch (p_counter) {
		case NEXTGI_HIT_CHECK_SAMPLES:
			return "samples";
		case NEXTGI_HIT_CHECK_HITS:
			return "hits";
		case NEXTGI_HIT_CHECK_FALLBACK:
			return "fallback";
		case NEXTGI_HIT_CHECK_ALBEDO:
			return "albedo";
		case NEXTGI_HIT_CHECK_NORMAL:
			return "normal";
		case NEXTGI_HIT_CHECK_ROUGHNESS:
			return "roughness";
		case NEXTGI_HIT_CHECK_METALLIC:
			return "metallic";
		case NEXTGI_HIT_CHECK_OPACITY:
			return "opacity";
		case NEXTGI_HIT_CHECK_RECORD:
			return "record";
		case NEXTGI_HIT_CHECK_ALL:
			return "all";
		case NEXTGI_HIT_CHECK_ANY_HIT_SAMPLES:
			return "any_hit_samples";
		case NEXTGI_HIT_CHECK_ANY_HIT_AGREE:
			return "any_hit_agree";
		case NEXTGI_HIT_CHECK_ALPHA_TESTED:
			return "alpha_tested";
		case NEXTGI_HIT_CHECK_ALPHA_CUT:
			return "alpha_cut";
		case NEXTGI_HIT_CHECK_PROBE_HITS:
			return "probe_hits";
		case NEXTGI_HIT_CHECK_PROBE_GPU_INSTANCES:
			return "probe_gpu_instances";
		case NEXTGI_HIT_CHECK_PROBE_GPU_FALLBACK:
			return "probe_gpu_fallback";
		case NEXTGI_HIT_CHECK_COUNTER_MAX:
			break;
	}
	return "unknown";
}

/* NextGIHitMaterials */

NextGIHitMaterials *NextGIHitMaterials::singleton = nullptr;

struct NextGIHitMaterials::HitShader {
	enum State {
		STATE_COMPILING, // The task generates the hit GLSL, compiles the stages and creates the library.
		STATE_READY, // The library exists; waiting for a link.
		STATE_LINKING,
		STATE_LINKED,
		STATE_INELIGIBLE,
	};
	uint64_t serial = 0;
	uint32_t slot = 0; // The shader record's shader_slot.
	uint32_t refs = 0; // Materials that use this shader. A shader no material uses is dead.
	State state = STATE_COMPILING;
	// The code and path the serial belongs to: the task compiles these, never the live ShaderData.
	String code;
	String path;
	// Written by the compile task; read only once its state has left STATE_COMPILING.
	NextGIHitIneligible reason = NEXTGI_HIT_ELIGIBLE;
	bool any_hit = false;
	bool has_light = false;
	uint32_t inputs = 0; // NEXTGI_HIT_READS_* bits.
	RID library;
	double add_ms = 0.0;
	NextgiHitMaterialShaderRD *shader_rd = nullptr;
	RID version;
	uint32_t group = 0; // 0 is the fallback group until the link.
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
};

struct NextGIHitMaterials::LinkJob {
	RID base;
	LocalVector<RID> libraries;
	LocalVector<HitShader *> shaders;
	// A relink: the base library and only the live libraries, into a new hit shader binding table.
	bool relink = false;
	RID result;
	double ms = 0.0;
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
};

struct NextGIHitMaterials::Material {
	uint32_t index = UINT32_MAX;
	uint64_t serial = 0;
	HitShader *shader = nullptr;
	NextGIHitIneligible reason = NEXTGI_HIT_ELIGIBLE;
	RID params_buffer;
	uint32_t params_size = 0;
	uint32_t params_slot = RDBindless::INVALID_SLOT;
	Vector<uint8_t> params;
	Vector<RID> textures;
	LocalVector<uint32_t> texture_slots;
	uint64_t last_frame = 0;
};

struct NextGIHitMaterials::Internal {
	NextgiHitBaseShaderRD base_shader;
	RID base_version;
	RID base_shader_rid;
	NextgiHitMaterialShaderRD material_shader;

	RID base_library;
	RID linked;
	uint32_t linked_libraries = 0;
	RID hit_sbt;
	int64_t fallback_range = 0;

	RID texture_table;
	RID params_table;
	RID texture_table_set;
	RID params_table_set;
	RID dummy_buffer;

	HashMap<uint64_t, HitShader *> shaders;
	uint32_t next_shader_slot = 0;
	LinkJob *link_job = nullptr;

	HashMap<RID, Material> materials;
	LocalVector<uint32_t> free_material_indices;
	uint32_t material_index_count = 0;
	struct TextureSlot {
		uint32_t slot = RDBindless::INVALID_SLOT;
		uint32_t refs = 0;
	};
	struct TableState {
		bool full = false; // The last allocation failed.
		bool released_since_full = false;
		uint64_t released_frame = 0;
	};
	TableState texture_table_state;
	TableState params_table_state;
	HashMap<RID, TextureSlot> texture_slots;
	bool tables_dirty = true;
	RID material_table_buffer;
	uint32_t material_table_capacity = 0;
	RID texture_slot_buffer;
	uint32_t texture_slot_capacity = 0;

	struct SurfaceRecord {
		uint32_t attribute_chunk = UINT32_MAX;
		uint32_t attribute_offset = 0;
		uint32_t material = UINT32_MAX;
		uint32_t flags = 0;
	};
	static_assert(sizeof(SurfaceRecord) == 16, "NextGIHitSurface is 16 bytes (nextgi_hit_layout_inc.glsl).");
	struct MaterialRecord {
		uint32_t params_slot = 0;
		uint32_t texture_base = 0;
		uint32_t texture_count = 0;
		uint32_t shader_slot = UINT32_MAX;
	};
	static_assert(sizeof(MaterialRecord) == 16, "NextGIHitMaterial is 16 bytes (nextgi_hit_layout_inc.glsl).");
	LocalVector<SurfaceRecord> surfaces;
	LocalVector<uint32_t> instance_groups;
	struct Range {
		int64_t range = 0;
		uint64_t frame = 0;
	};
	HashMap<uint64_t, Range> ranges;
	struct InstanceCards {
		uint32_t record = 0;
		uint32_t card_set = UINT32_MAX;
	};
	LocalVector<InstanceCards> instance_cards;
	RID hit_surface_buffer;
	uint32_t hit_surface_capacity = 0;

	uint64_t frame = 0;
	uint64_t last_check_frame = 0;
	// The CPU-fed TLAS instances of the last TLAS phase, and the frame that phase ran in.
	uint32_t record_count = 0;
	uint64_t records_frame = UINT64_MAX;
	bool relink_failed = false; // A relink failed: dead groups stay until the hit groups are recreated.
	bool ready = false;
	Counters counters;
};

void NextGIHitMaterials::_compile_task(void *p_userdata) {
	HitShader *shader = static_cast<HitShader *>(p_userdata);
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	// The hit GLSL comes from the shader compiler as ShaderData::set_code() runs it on resource loading threads
	// (under the compiler's mutex), then the stages compile and the library is created.
	using SceneShaderData = RendererSceneRenderImplementation::SceneShaderForwardClustered::ShaderData;
	NextGIHitShaderSource source;
	if (!SceneShaderData::build_nextgi_hit_source(shader->code, shader->path, source)) {
		shader->reason = NEXTGI_HIT_INELIGIBLE_COMPILE_FAILED;
	} else {
		shader->has_light = source.has_light;
		shader->inputs = nextgi_hit_element_inputs(source);
		shader->reason = nextgi_hit_classify(source);
	}
	if (shader->reason == NEXTGI_HIT_ELIGIBLE) {
		shader->any_hit = source.alpha_scissor || source.alpha_hash;
		HashMap<String, String> code;
		code["vertex"] = source.vertex_code;
		code["fragment"] = source.fragment_code;
		const String globals = nextgi_hit_texture_defines(source) + source.globals;
		shader->shader_rd->version_set_raytracing_code(shader->version, code, source.uniforms, String(), globals, globals, String(), String(), source.defines);
		const RID rd_shader = shader->shader_rd->version_get_shader(shader->version, 0);
		if (rd_shader.is_valid()) {
			RD::HitGroup group;
			group.closest_hit_shader.shader = rd_shader;
			if (shader->any_hit) {
				group.any_hit_shader.shader = rd_shader;
			}
			const uint32_t record[4] = { shader->slot, shader->any_hit ? 1u : 0u, 0u, 0u };
			group.record_data.resize(sizeof(record));
			memcpy(group.record_data.ptrw(), record, sizeof(record));
			shader->library = RD::get_singleton()->raytracing_pipeline_create(Span<RD::PipelineShader>(), Span<RD::PipelineShader>(), Span<RD::HitGroup>(&group, 1), 1, RD::RAYTRACING_PIPELINE_FLAG_LIBRARY);
		}
		if (shader->library.is_null()) {
			shader->reason = NEXTGI_HIT_INELIGIBLE_COMPILE_FAILED;
		}
	}
	shader->add_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
}

void NextGIHitMaterials::_link_task(void *p_userdata) {
	LinkJob *job = static_cast<LinkJob *>(p_userdata);
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	job->result = RD::get_singleton()->raytracing_pipeline_link(job->base, Span<RID>(job->libraries.ptr(), job->libraries.size()));
	job->ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
}

bool NextGIHitMaterials::_initialize() {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	Internal &in = *internal;

	in.base_shader.initialize({ "" });
	in.base_version = in.base_shader.version_create(false);
	in.base_shader_rid = in.base_shader.version_get_shader(in.base_version, 0);
	ERR_FAIL_COND_V_MSG(in.base_shader_rid.is_null(), false, "NextGI hit groups: the base hit shader did not compile.");
	in.material_shader.initialize({ "" });

	RD::PipelineShader raygen;
	raygen.shader = in.base_shader_rid;
	RD::PipelineShader miss;
	miss.shader = in.base_shader_rid;
	RD::HitGroup fallback;
	fallback.closest_hit_shader.shader = in.base_shader_rid;
	const uint32_t record[4] = { UINT32_MAX, 0u, 0u, 0u };
	fallback.record_data.resize(sizeof(record));
	memcpy(fallback.record_data.ptrw(), record, sizeof(record));
	in.base_library = rd->raytracing_pipeline_create(Span<RD::PipelineShader>(&raygen, 1), Span<RD::PipelineShader>(&miss, 1), Span<RD::HitGroup>(&fallback, 1), 1, RD::RAYTRACING_PIPELINE_FLAG_LIBRARY);
	ERR_FAIL_COND_V(in.base_library.is_null(), false);
	in.linked = rd->raytracing_pipeline_link(in.base_library, Span<RID>());
	ERR_FAIL_COND_V(in.linked.is_null(), false);
	in.hit_sbt = rd->hit_sbt_create(in.linked, 1024);
	ERR_FAIL_COND_V(in.hit_sbt.is_null(), false);
	// The first range starts at 0, where GPU-written TLAS instances land.
	in.fallback_range = rd->hit_sbt_range_alloc(in.hit_sbt, NEXTGI_HIT_FALLBACK_RANGE);
	ERR_FAIL_COND_V(in.fallback_range == 0, false);
	LocalVector<uint32_t> zeros;
	zeros.resize_initialized(NEXTGI_HIT_FALLBACK_RANGE);
	rd->hit_sbt_range_update(in.hit_sbt, in.fallback_range, 0, Span<uint32_t>(zeros.ptr(), zeros.size()));

	in.texture_table = rd->bindless_table_create(RD::UNIFORM_TYPE_TEXTURE, NEXTGI_HIT_TABLE_SLOTS);
	in.params_table = rd->bindless_table_create(RD::UNIFORM_TYPE_STORAGE_BUFFER, NEXTGI_HIT_TABLE_SLOTS);
	ERR_FAIL_COND_V(in.texture_table.is_null() || in.params_table.is_null(), false);
	{
		RD::Uniform u(RD::UNIFORM_TYPE_BINDLESS_TABLE, 0, in.texture_table);
		in.texture_table_set = rd->uniform_set_create(u, in.base_shader_rid, 1);
	}
	{
		RD::Uniform u(RD::UNIFORM_TYPE_BINDLESS_TABLE, 0, in.params_table);
		in.params_table_set = rd->uniform_set_create(u, in.base_shader_rid, 2);
	}
	ERR_FAIL_COND_V(in.texture_table_set.is_null() || in.params_table_set.is_null(), false);
	in.dummy_buffer = rd->storage_buffer_create(16);
	return true;
}

NextGIHitMaterials::HitShader *NextGIHitMaterials::_get_shader(void *p_shader_data, uint64_t p_serial) {
	Internal &in = *internal;
	HitShader **existing = in.shaders.getptr(p_serial);
	if (existing != nullptr) {
		return *existing;
	}
	using SceneShaderData = RendererSceneRenderImplementation::SceneShaderForwardClustered::ShaderData;
	const SceneShaderData *shader_data = static_cast<const SceneShaderData *>(p_shader_data);
	HitShader *shader = memnew(HitShader);
	shader->serial = p_serial;
	shader->slot = in.next_shader_slot++;
	shader->shader_rd = &in.material_shader;
	shader->code = shader_data->code;
	shader->path = shader_data->path;
	shader->version = in.material_shader.version_create(false);
	shader->state = HitShader::STATE_COMPILING;
	in.shaders.insert(p_serial, shader);
	// Off the render thread: the hit GLSL, the stages and the library (group_add_ms).
	shader->task = WorkerThreadPool::get_singleton()->add_native_task(&NextGIHitMaterials::_compile_task, shader, false, "NextGI hit group");
	return shader;
}

void NextGIHitMaterials::_set_material_shader(Material &r_material, HitShader *p_shader) {
	if (r_material.shader == p_shader) {
		return;
	}
	if (r_material.shader != nullptr && r_material.shader->refs > 0) {
		r_material.shader->refs--;
	}
	r_material.shader = p_shader;
	if (p_shader != nullptr) {
		p_shader->refs++;
	}
}

uint32_t NextGIHitMaterials::_texture_slot(RID p_texture) {
	Internal &in = *internal;
	Internal::TextureSlot *slot = in.texture_slots.getptr(p_texture);
	if (slot != nullptr) {
		slot->refs++;
		return slot->slot;
	}
	Internal::TextureSlot created;
	created.slot = _allocate_slot(true, p_texture);
	created.refs = 1;
	in.texture_slots.insert(p_texture, created);
	return created.slot;
}

// A texture whose slot could not be allocated (the table was full) tries again, keeping its references.
uint32_t NextGIHitMaterials::_retry_texture_slot(RID p_texture) {
	Internal &in = *internal;
	Internal::TextureSlot *slot = in.texture_slots.getptr(p_texture);
	if (slot == nullptr) {
		return RDBindless::INVALID_SLOT;
	}
	if (slot->slot == RDBindless::INVALID_SLOT) {
		slot->slot = _allocate_slot(true, p_texture);
	}
	return slot->slot;
}

void NextGIHitMaterials::_release_texture_slot(RID p_texture) {
	Internal &in = *internal;
	Internal::TextureSlot *slot = in.texture_slots.getptr(p_texture);
	if (slot == nullptr) {
		return;
	}
	if (--slot->refs == 0) {
		_release_slot(true, slot->slot);
		in.texture_slots.erase(p_texture);
	}
}

uint32_t NextGIHitMaterials::_allocate_slot(bool p_texture_table, RID p_resource) {
	Internal &in = *internal;
	Internal::TableState &state = p_texture_table ? in.texture_table_state : in.params_table_state;
	if (state.full) {
		// A released slot is reusable once the frames that may still read it are done.
		const uint64_t reusable = state.released_frame + RD::get_singleton()->get_frame_delay() + 1;
		if (!state.released_since_full || in.frame < reusable) {
			return RDBindless::INVALID_SLOT;
		}
	}
	const uint32_t slot = RD::get_singleton()->bindless_table_allocate(p_texture_table ? in.texture_table : in.params_table, p_resource);
	state.full = slot == RDBindless::INVALID_SLOT;
	state.released_since_full = false;
	return slot;
}

void NextGIHitMaterials::_release_slot(bool p_texture_table, uint32_t p_slot) {
	Internal &in = *internal;
	if (p_slot == RDBindless::INVALID_SLOT) {
		return;
	}
	RD::get_singleton()->bindless_table_release(p_texture_table ? in.texture_table : in.params_table, p_slot);
	Internal::TableState &state = p_texture_table ? in.texture_table_state : in.params_table_state;
	state.released_since_full = true;
	state.released_frame = in.frame;
}

void NextGIHitMaterials::_release_material_slots(Material &r_material) {
	Internal &in = *internal;
	for (const RID &texture : r_material.textures) {
		_release_texture_slot(texture);
	}
	r_material.textures.clear();
	r_material.texture_slots.clear();
	_release_slot(false, r_material.params_slot);
	r_material.params_slot = RDBindless::INVALID_SLOT;
	if (r_material.params_buffer.is_valid()) {
		RD::get_singleton()->free_rid(r_material.params_buffer);
		r_material.params_buffer = RID();
	}
	r_material.params.clear();
	r_material.params_size = 0;
}

// The material's parameter buffer and texture slots follow what update_parameters_uniform_set() last wrote.
NextGIHitMaterials::Material *NextGIHitMaterials::_get_material(RID p_material, uint64_t p_frame) {
	Internal &in = *internal;
	RenderingDevice *rd = RD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	Material *material = in.materials.getptr(p_material);
	if (material == nullptr) {
		Material created;
		if (!in.free_material_indices.is_empty()) {
			created.index = in.free_material_indices[in.free_material_indices.size() - 1];
			in.free_material_indices.resize(in.free_material_indices.size() - 1);
		} else {
			created.index = in.material_index_count++;
		}
		in.materials.insert(p_material, created);
		material = in.materials.getptr(p_material);
		in.tables_dirty = true;
	}
	if (material->last_frame == p_frame && p_frame != 0) {
		return material;
	}
	material->last_frame = p_frame;

	using SceneMaterialData = RendererSceneRenderImplementation::SceneShaderForwardClustered::MaterialData;
	MaterialStorage::MaterialData *base_data = material_storage != nullptr ? material_storage->material_get_data(p_material, MaterialStorage::SHADER_TYPE_3D) : nullptr;
	SceneMaterialData *data = static_cast<SceneMaterialData *>(base_data);
	if (data == nullptr || data->shader_data == nullptr || data->shader_data->nextgi_hit_serial == 0) {
		if (material->reason != NEXTGI_HIT_INELIGIBLE_NOT_SPATIAL) {
			_release_material_slots(*material);
			in.tables_dirty = true;
		}
		material->reason = NEXTGI_HIT_INELIGIBLE_NOT_SPATIAL;
		_set_material_shader(*material, nullptr);
		material->serial = 0;
		return material;
	}
	if (material->serial != data->shader_data->nextgi_hit_serial) {
		material->serial = data->shader_data->nextgi_hit_serial;
		_set_material_shader(*material, _get_shader(data->shader_data, material->serial));
		in.tables_dirty = true;
	}
	if (!data->render_target_cache.is_empty()) {
		if (material->reason != NEXTGI_HIT_INELIGIBLE_RENDER_TARGET) {
			_release_material_slots(*material);
			in.tables_dirty = true;
		}
		material->reason = NEXTGI_HIT_INELIGIBLE_RENDER_TARGET;
		return material;
	}
	// The shader's reason is known once its task is done. Until then its geometries use the fallback group,
	// and the material takes no slots (the shader may still be refused).
	const HitShader *shader = material->shader;
	if (shader != nullptr && shader->state == HitShader::STATE_COMPILING) {
		material->reason = NEXTGI_HIT_ELIGIBLE;
		return material;
	}
	material->reason = shader == nullptr ? NEXTGI_HIT_INELIGIBLE_NOT_SPATIAL : (shader->state == HitShader::STATE_INELIGIBLE ? shader->reason : NEXTGI_HIT_ELIGIBLE);
	if (material->reason != NEXTGI_HIT_ELIGIBLE) {
		// Refused (also after the material took its slots for an earlier code): no slots.
		if (material->params_buffer.is_valid() || !material->textures.is_empty()) {
			_release_material_slots(*material);
			in.tables_dirty = true;
		}
		return material;
	}

	// Parameters: a storage buffer with the uniform buffer's std140 bytes.
	const Vector<uint8_t> &ubo = data->get_ubo_data(true);
	const uint32_t params_size = MAX(uint32_t(ubo.size()), 16u);
	if (material->params_size != params_size || material->params_buffer.is_null()) {
		_release_slot(false, material->params_slot);
		material->params_slot = RDBindless::INVALID_SLOT;
		if (material->params_buffer.is_valid()) {
			rd->free_rid(material->params_buffer);
		}
		material->params_buffer = rd->storage_buffer_create(params_size);
		material->params_size = params_size;
		material->params.clear();
		in.tables_dirty = true;
	}
	if (material->params_slot == RDBindless::INVALID_SLOT && material->params_buffer.is_valid()) {
		// After the table was full, again once a slot released from it is reusable (_allocate_slot()).
		material->params_slot = _allocate_slot(false, material->params_buffer);
		if (material->params_slot != RDBindless::INVALID_SLOT) {
			in.tables_dirty = true;
		}
	}
	if (material->params != ubo && material->params_buffer.is_valid()) {
		material->params = ubo;
		if (!ubo.is_empty()) {
			rd->buffer_update(material->params_buffer, 0, uint32_t(ubo.size()), ubo.ptr());
		}
	}

	// Textures: the RD textures of the uniform set, in texture order, each in the bindless texture table.
	// update_textures() resolves a missing texture to a default one, so the cache holds no null RID.
	const Vector<RID> &textures = data->get_texture_cache();
	if (material->textures != textures) {
		for (const RID &texture : material->textures) {
			_release_texture_slot(texture);
		}
		material->textures = textures;
		material->texture_slots.clear();
		for (const RID &texture : textures) {
			material->texture_slots.push_back(texture.is_valid() ? _texture_slot(texture) : RDBindless::INVALID_SLOT);
		}
		in.tables_dirty = true;
	}
	bool slots_full = material->params_slot == RDBindless::INVALID_SLOT;
	for (uint32_t i = 0; i < material->texture_slots.size(); i++) {
		const RID &texture = material->textures[i];
		if (material->texture_slots[i] == RDBindless::INVALID_SLOT && texture.is_valid()) {
			material->texture_slots[i] = _retry_texture_slot(texture);
			if (material->texture_slots[i] == RDBindless::INVALID_SLOT) {
				slots_full = true;
			} else {
				in.tables_dirty = true;
			}
		}
	}
	if (slots_full) {
		// No parameter or texture slot: the shader would index past its table. The fallback group shades it.
		material->reason = NEXTGI_HIT_INELIGIBLE_TABLE_FULL;
	}
	return material;
}

void NextGIHitMaterials::_upload_material_tables() {
	Internal &in = *internal;
	if (!in.tables_dirty) {
		return;
	}
	in.tables_dirty = false;
	RenderingDevice *rd = RD::get_singleton();
	LocalVector<Internal::MaterialRecord> records;
	records.resize(MAX(in.material_index_count, 1u));
	LocalVector<uint32_t> slots;
	for (const KeyValue<RID, Material> &E : in.materials) {
		const Material &material = E.value;
		Internal::MaterialRecord &record = records[material.index];
		record.params_slot = material.params_slot;
		record.texture_base = slots.size();
		record.texture_count = material.texture_slots.size();
		record.shader_slot = material.shader != nullptr ? material.shader->slot : UINT32_MAX;
		for (uint32_t slot : material.texture_slots) {
			slots.push_back(slot == RDBindless::INVALID_SLOT ? 0u : slot);
		}
	}
	if (slots.is_empty()) {
		slots.push_back(0);
	}
	const uint32_t record_bytes = records.size() * sizeof(Internal::MaterialRecord);
	if (in.material_table_buffer.is_null() || in.material_table_capacity < record_bytes) {
		if (in.material_table_buffer.is_valid()) {
			rd->free_rid(in.material_table_buffer);
		}
		in.material_table_capacity = MAX(record_bytes * 2, 256u);
		in.material_table_buffer = rd->storage_buffer_create(in.material_table_capacity);
	}
	rd->buffer_update(in.material_table_buffer, 0, record_bytes, records.ptr());
	const uint32_t slot_bytes = slots.size() * sizeof(uint32_t);
	if (in.texture_slot_buffer.is_null() || in.texture_slot_capacity < slot_bytes) {
		if (in.texture_slot_buffer.is_valid()) {
			rd->free_rid(in.texture_slot_buffer);
		}
		in.texture_slot_capacity = MAX(slot_bytes * 2, 256u);
		in.texture_slot_buffer = rd->storage_buffer_create(in.texture_slot_capacity);
	}
	rd->buffer_update(in.texture_slot_buffer, 0, slot_bytes, slots.ptr());
}

void NextGIHitMaterials::_poll_tasks() {
	Internal &in = *internal;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		HitShader *shader = E.value;
		if (shader->task == WorkerThreadPool::INVALID_TASK_ID || !pool->is_task_completed(shader->task)) {
			continue;
		}
		pool->wait_for_task_completion(shader->task);
		shader->task = WorkerThreadPool::INVALID_TASK_ID;
		in.counters.group_add_ms_last = shader->add_ms;
		in.counters.group_add_ms_max = MAX(in.counters.group_add_ms_max, shader->add_ms);
		if (shader->reason == NEXTGI_HIT_ELIGIBLE && shader->library.is_valid()) {
			shader->state = HitShader::STATE_READY;
		} else {
			shader->state = HitShader::STATE_INELIGIBLE;
			in.tables_dirty = true;
			// Once per serial: the shader and why its hits use the fallback group.
			print_verbose(vformat("NextGI hit groups: \"%s\" has no hit group (%s); its hits use the fallback group.", shader->path.is_empty() ? String("(no path)") : shader->path, nextgi_hit_ineligible_name(shader->reason)));
		}
	}
	LinkJob *job = in.link_job;
	if (job != nullptr && pool->is_task_completed(job->task)) {
		pool->wait_for_task_completion(job->task);
		in.link_job = nullptr;
		in.counters.link_ms_last = job->ms;
		in.counters.link_ms_max = MAX(in.counters.link_ms_max, job->ms);
		_finish_link(job);
		memdelete(job);
	}
}

void NextGIHitMaterials::_finish_link(LinkJob *p_job) {
	Internal &in = *internal;
	RenderingDevice *rd = RD::get_singleton();
	if (p_job->relink) {
		RID hit_sbt;
		int64_t fallback_range = 0;
		if (p_job->result.is_valid()) {
			hit_sbt = rd->hit_sbt_create(p_job->result, 1024);
		}
		if (hit_sbt.is_valid()) {
			// The first range of the new table starts at 0, where GPU-written TLAS instances land.
			fallback_range = rd->hit_sbt_range_alloc(hit_sbt, NEXTGI_HIT_FALLBACK_RANGE);
		}
		if (fallback_range == 0) {
			if (hit_sbt.is_valid()) {
				rd->free_rid(hit_sbt);
			}
			if (p_job->result.is_valid()) {
				rd->free_rid(p_job->result);
			}
			// The live groups keep their places in the current pipeline; the dead ones stay until the hit groups
			// are created again.
			for (HitShader *shader : p_job->shaders) {
				if (shader->state == HitShader::STATE_LINKING) {
					shader->state = HitShader::STATE_READY;
				}
			}
			in.relink_failed = true;
			return;
		}
		LocalVector<uint32_t> zeros;
		zeros.resize_initialized(NEXTGI_HIT_FALLBACK_RANGE);
		rd->hit_sbt_range_update(hit_sbt, fallback_range, 0, Span<uint32_t>(zeros.ptr(), zeros.size()));
		// The old table and pipeline go first: the pipeline depends on the dead libraries freed below.
		rd->free_rid(in.hit_sbt);
		rd->free_rid(in.linked);
		in.hit_sbt = hit_sbt;
		in.fallback_range = fallback_range;
		in.linked = p_job->result;
		in.ranges.clear();
		HashSet<HitShader *> relinked;
		for (uint32_t i = 0; i < p_job->shaders.size(); i++) {
			p_job->shaders[i]->group = 1 + i;
			p_job->shaders[i]->state = HitShader::STATE_LINKED;
			relinked.insert(p_job->shaders[i]);
		}
		in.linked_libraries = p_job->shaders.size();
		// Linked shaders the relink left out: dead ones are freed; one a material took up again meanwhile links
		// again.
		LocalVector<uint64_t> freed;
		for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
			HitShader *shader = E.value;
			if (shader->state != HitShader::STATE_LINKED || relinked.has(shader)) {
				continue;
			}
			if (shader->refs == 0) {
				freed.push_back(E.key);
			} else {
				shader->state = HitShader::STATE_READY;
				shader->group = 0;
			}
		}
		for (uint64_t key : freed) {
			_free_shader(in.shaders[key]);
			in.shaders.erase(key);
		}
		in.counters.links++;
		in.counters.relinks++;
		return;
	}
	if (p_job->result.is_valid() && rd->hit_sbt_set_pipeline(in.hit_sbt, p_job->result) == OK) {
		// The groups of a linked pipeline are the base's (the fallback, group 0) and then each library's.
		for (uint32_t i = 0; i < p_job->shaders.size(); i++) {
			p_job->shaders[i]->group = 1 + in.linked_libraries + i;
			p_job->shaders[i]->state = HitShader::STATE_LINKED;
		}
		in.linked_libraries += p_job->shaders.size();
		rd->free_rid(in.linked);
		in.linked = p_job->result;
		in.counters.links++;
	} else {
		if (p_job->result.is_valid()) {
			rd->free_rid(p_job->result);
		}
		for (HitShader *shader : p_job->shaders) {
			shader->state = HitShader::STATE_INELIGIBLE;
			shader->reason = NEXTGI_HIT_INELIGIBLE_COMPILE_FAILED;
		}
		in.tables_dirty = true;
	}
}

void NextGIHitMaterials::_start_link() {
	Internal &in = *internal;
	if (in.link_job != nullptr) {
		return;
	}
	LinkJob *job = nullptr;
	for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		HitShader *shader = E.value;
		if (shader->state != HitShader::STATE_READY || shader->refs == 0) {
			continue;
		}
		if (job == nullptr) {
			job = memnew(LinkJob);
			job->base = in.linked;
		}
		shader->state = HitShader::STATE_LINKING;
		job->libraries.push_back(shader->library);
		job->shaders.push_back(shader);
	}
	if (job == nullptr) {
		return;
	}
	in.link_job = job;
	// Off the render thread: a link adds only the new libraries to the previous pipeline (#179).
	job->task = WorkerThreadPool::get_singleton()->add_native_task(&NextGIHitMaterials::_link_task, job, false, "NextGI hit link");
}

// The base library and every live library into a new pipeline, off the render thread. The live linked groups
// keep shading from the current pipeline until _finish_link() swaps in the new one and its table.
void NextGIHitMaterials::_start_relink() {
	Internal &in = *internal;
	LinkJob *job = memnew(LinkJob);
	job->base = in.base_library;
	job->relink = true;
	for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		HitShader *shader = E.value;
		if (shader->refs == 0 || (shader->state != HitShader::STATE_LINKED && shader->state != HitShader::STATE_READY)) {
			continue;
		}
		if (shader->state == HitShader::STATE_READY) {
			shader->state = HitShader::STATE_LINKING;
		}
		job->libraries.push_back(shader->library);
		job->shaders.push_back(shader);
	}
	in.link_job = job;
	job->task = WorkerThreadPool::get_singleton()->add_native_task(&NextGIHitMaterials::_link_task, job, false, "NextGI hit relink");
}

// Library first: it depends on the version's shader, and linked pipelines depend on it.
void NextGIHitMaterials::_free_shader(HitShader *p_shader) {
	Internal &in = *internal;
	if (p_shader->library.is_valid()) {
		RD::get_singleton()->free_rid(p_shader->library);
		p_shader->library = RID();
	}
	if (p_shader->version.is_valid()) {
		in.material_shader.version_free(p_shader->version);
		p_shader->version = RID();
	}
	memdelete(p_shader);
}

// Shaders no material uses any more (a code change gives the shader a new serial, or its materials went
// away): those outside the pipeline are freed now; linked ones wait for a relink without them.
void NextGIHitMaterials::_release_dead_shaders() {
	Internal &in = *internal;
	LocalVector<uint64_t> freed;
	uint32_t dead_linked = 0;
	for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		const HitShader *shader = E.value;
		if (shader->refs > 0) {
			continue;
		}
		if (shader->state == HitShader::STATE_READY || shader->state == HitShader::STATE_INELIGIBLE) {
			freed.push_back(E.key);
		} else if (shader->state == HitShader::STATE_LINKED) {
			dead_linked++;
		}
		// STATE_COMPILING and STATE_LINKING: a task still uses the shader.
	}
	for (uint64_t key : freed) {
		_free_shader(in.shaders[key]);
		in.shaders.erase(key);
	}
	in.counters.groups_dead = dead_linked;
	if (!in.relink_failed && nextgi_hit_should_relink(dead_linked, in.link_job != nullptr)) {
		_start_relink();
	}
}

bool NextGIHitMaterials::begin_frame(uint64_t p_frame) {
	Internal &in = *internal;
	in.frame = p_frame;
	if (!in.ready) {
		return false;
	}
	_poll_tasks();

	// Materials no hit record used for a while give their slots, index and shader reference back.
	LocalVector<RID> stale;
	for (const KeyValue<RID, Material> &E : in.materials) {
		if (E.value.last_frame + 120 < p_frame) {
			stale.push_back(E.key);
		}
	}
	for (const RID &rid : stale) {
		Material *material = in.materials.getptr(rid);
		_release_material_slots(*material);
		_set_material_shader(*material, nullptr);
		in.free_material_indices.push_back(material->index);
		in.materials.erase(rid);
		in.tables_dirty = true;
	}

	_release_dead_shaders();
	_start_link();

	// Only live shaders count: a dead one is waiting to be freed.
	Counters &c = in.counters;
	c.shaders = 0;
	c.groups_linked = 0;
	c.groups_pending = 0;
	c.light_skipped = 0;
	c.ineligible = 0;
	for (uint32_t i = 0; i < NEXTGI_HIT_INELIGIBLE_MAX; i++) {
		c.ineligible_reasons[i] = 0;
	}
	for (const KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		const HitShader *shader = E.value;
		if (shader->refs == 0) {
			continue;
		}
		c.shaders++;
		if (shader->state == HitShader::STATE_LINKED) {
			c.groups_linked++;
		} else if (shader->state == HitShader::STATE_INELIGIBLE) {
			c.ineligible++;
			c.ineligible_reasons[shader->reason]++;
		} else {
			c.groups_pending++;
		}
		if (shader->state != HitShader::STATE_COMPILING && shader->has_light) {
			c.light_skipped++;
		}
	}
	c.materials = in.materials.size();
	for (const KeyValue<RID, Material> &E : in.materials) {
		// Shader-level reasons are counted with the shaders; these are the material's own.
		if (E.value.reason == NEXTGI_HIT_INELIGIBLE_RENDER_TARGET || E.value.reason == NEXTGI_HIT_INELIGIBLE_NOT_SPATIAL || E.value.reason == NEXTGI_HIT_INELIGIBLE_TABLE_FULL) {
			c.ineligible++;
			c.ineligible_reasons[E.value.reason]++;
		}
	}
	return true;
}

bool NextGIHitMaterials::is_ready() const {
	return internal->ready;
}

void NextGIHitMaterials::begin_records() {
	Internal &in = *internal;
	in.surfaces.clear();
	in.instance_groups.clear();
	in.instance_cards.clear();
	in.record_count = 0;
	in.counters.geometries = 0;
	in.counters.fallback_geometries = 0;
	in.counters.no_stream_geometries = 0;
	in.counters.element_data_geometries = 0;
}

void NextGIHitMaterials::add_geometry(RenderGeometryInstance *p_instance, RID p_mesh, uint32_t p_surface, uint32_t p_arena_chunk, bool p_has_stream, uint32_t p_attribute_chunk, uint32_t p_attribute_offset, bool p_multimesh_element) {
	Internal &in = *internal;
	RenderGeometryInstanceBase *instance = static_cast<RenderGeometryInstanceBase *>(p_instance);
	RID material_rid;
	if (instance != nullptr && instance->data != nullptr) {
		if (instance->data->material_override.is_valid()) {
			material_rid = instance->data->material_override;
		} else if (int(p_surface) < instance->data->surface_materials.size() && instance->data->surface_materials[p_surface].is_valid()) {
			material_rid = instance->data->surface_materials[p_surface];
		}
	}
	if (material_rid.is_null() && MeshStorage::get_singleton() != nullptr && p_mesh.is_valid()) {
		material_rid = MeshStorage::get_singleton()->mesh_surface_get_material(p_mesh, int(p_surface));
	}
	if (material_rid.is_null()) {
		// No material anywhere: the rasterizer draws the scene shader's default material.
		material_rid = RendererSceneRenderImplementation::SceneShaderForwardClustered::get_default_material_for_nextgi();
	}

	Internal::SurfaceRecord record;
	uint32_t group = 0;
	// The hit stages bind the first NEXTGI_HIT_ARENA_CHUNKS chunks: geometry past them has no stream.
	if (p_has_stream && p_attribute_chunk < NEXTGI_HIT_ARENA_CHUNKS && p_arena_chunk < NEXTGI_HIT_ARENA_CHUNKS) {
		record.attribute_chunk = p_attribute_chunk;
		record.attribute_offset = p_attribute_offset;
		record.flags = 1u;
		const Material *material = material_rid.is_valid() ? _get_material(material_rid, in.frame) : nullptr;
		if (material != nullptr && material->reason == NEXTGI_HIT_ELIGIBLE) {
			record.material = material->index;
			if (material->shader != nullptr && material->shader->state == HitShader::STATE_LINKED) {
				group = material->shader->group;
				// The records carry no MultiMesh element color or custom data: an element whose material reads one
				// its MultiMesh stores uses the fallback group.
				MeshStorage *mesh_storage = MeshStorage::get_singleton();
				if (p_multimesh_element && instance != nullptr && instance->data != nullptr && instance->data->base_type == RSE::INSTANCE_MULTIMESH && mesh_storage != nullptr && mesh_storage->owns_multimesh(instance->data->base)) {
					const RID multimesh = instance->data->base;
					if (nextgi_hit_element_needs_fallback(material->shader->inputs, mesh_storage->multimesh_uses_colors(multimesh), mesh_storage->multimesh_uses_custom_data(multimesh))) {
						group = 0;
						in.counters.element_data_geometries++;
					}
				}
			}
		}
	} else {
		in.counters.no_stream_geometries++;
	}
	in.surfaces.push_back(record);
	in.instance_groups.push_back(group);
	in.counters.geometries++;
	if (group == 0) {
		in.counters.fallback_geometries++;
	}
}

void NextGIHitMaterials::add_fallback_surfaces(uint32_t p_count) {
	Internal &in = *internal;
	for (uint32_t i = 0; i < p_count; i++) {
		in.surfaces.push_back(Internal::SurfaceRecord());
		in.instance_groups.push_back(0);
		in.counters.geometries++;
		in.counters.fallback_geometries++;
		in.counters.no_stream_geometries++;
	}
}

int64_t NextGIHitMaterials::get_fallback_range() const {
	return internal->fallback_range;
}

int64_t NextGIHitMaterials::end_instance(uint32_t p_record_index, uint32_t p_card_set, bool p_check_cards) {
	Internal &in = *internal;
	in.record_count = MAX(in.record_count, p_record_index + 1);
	if (p_card_set != UINT32_MAX && p_check_cards) {
		Internal::InstanceCards cards;
		cards.record = p_record_index;
		cards.card_set = p_card_set;
		in.instance_cards.push_back(cards);
	}
	bool all_fallback = true;
	for (uint32_t group : in.instance_groups) {
		if (group != 0) {
			all_fallback = false;
			break;
		}
	}
	if (all_fallback || in.instance_groups.size() > NEXTGI_HIT_FALLBACK_RANGE) {
		in.instance_groups.clear();
		return in.fallback_range;
	}
	const uint64_t key = nextgi_hit_group_list_key(in.instance_groups.ptr(), in.instance_groups.size());
	Internal::Range *range = in.ranges.getptr(key);
	if (range == nullptr) {
		RenderingDevice *rd = RD::get_singleton();
		Internal::Range created;
		created.range = rd->hit_sbt_range_alloc(in.hit_sbt, in.instance_groups.size());
		if (created.range == 0) {
			in.instance_groups.clear();
			return in.fallback_range;
		}
		rd->hit_sbt_range_update(in.hit_sbt, created.range, 0, Span<uint32_t>(in.instance_groups.ptr(), in.instance_groups.size()));
		in.ranges.insert(key, created);
		range = in.ranges.getptr(key);
	}
	range->frame = in.frame;
	in.instance_groups.clear();
	return range->range;
}

void NextGIHitMaterials::finish_records(uint32_t p_surface_record_count) {
	Internal &in = *internal;
	RenderingDevice *rd = RD::get_singleton();
	in.instance_groups.clear();
	// Surface records with no hit record of their own (far-field proxies) take the fallback record.
	while (in.surfaces.size() < p_surface_record_count) {
		in.surfaces.push_back(Internal::SurfaceRecord());
	}
	if (in.surfaces.size() > p_surface_record_count) {
		in.surfaces.resize(p_surface_record_count);
	}
	if (in.surfaces.is_empty()) {
		in.surfaces.push_back(Internal::SurfaceRecord());
	}
	const uint32_t bytes = in.surfaces.size() * sizeof(Internal::SurfaceRecord);
	if (in.hit_surface_buffer.is_null() || in.hit_surface_capacity < bytes) {
		if (in.hit_surface_buffer.is_valid()) {
			rd->free_rid(in.hit_surface_buffer);
		}
		in.hit_surface_capacity = MAX(bytes * 2, 1024u);
		in.hit_surface_buffer = rd->storage_buffer_create(in.hit_surface_capacity);
	}
	rd->buffer_update(in.hit_surface_buffer, 0, bytes, in.surfaces.ptr());

	// Ranges no TLAS instance used this frame go back to the table.
	LocalVector<uint64_t> stale;
	for (const KeyValue<uint64_t, Internal::Range> &E : in.ranges) {
		if (E.value.frame != in.frame) {
			stale.push_back(E.key);
		}
	}
	for (uint64_t key : stale) {
		rd->hit_sbt_range_free(in.hit_sbt, in.ranges[key].range);
		in.ranges.erase(key);
	}
	in.counters.sbt_ranges = in.ranges.size() + 1;
	_upload_material_tables();
	in.records_frame = in.frame;
}

uint32_t NextGIHitMaterials::get_record_count() const {
	return internal->record_count;
}

bool NextGIHitMaterials::has_current_records() const {
	return internal->ready && internal->records_frame == internal->frame;
}

void NextGIHitMaterials::run_check(NextGIRTScene *p_rt_scene, NextGISRC *p_src, float p_time) {
	Internal &in = *internal;
	if (!has_current_records() || p_rt_scene == nullptr || p_src == nullptr || !p_rt_scene->is_tlas_traceable()) {
		return;
	}
	// Every live shader compiled and linked (or ineligible), and every card captured.
	uint32_t pending = 0;
	for (const KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		if (E.value->refs > 0 && E.value->state != HitShader::STATE_LINKED && E.value->state != HitShader::STATE_INELIGIBLE) {
			pending++;
		}
	}
	in.counters.check_pending_shaders = pending;
	const NextGISRC::Stats &src_stats = p_src->get_stats();
	if (pending > 0 || in.link_job != nullptr || src_stats.card_count == 0 || src_stats.captured_card_count < src_stats.card_count || in.instance_cards.is_empty()) {
		return;
	}
	if (in.counters.check_runs > 0 && in.frame < in.last_check_frame + 30) {
		return;
	}

	LocalVector<NextGIHitCheckCard> cards;
	for (const Internal::InstanceCards &instance : in.instance_cards) {
		NextGICardSet set;
		if (!p_src->copy_card_set(instance.card_set, set)) {
			continue;
		}
		for (uint32_t i = 0; i < set.card_count; i++) {
			NextGICard card;
			if (!p_src->copy_card(set.first_card + i, card)) {
				continue;
			}
			NextGIHitCheckCard check_card;
			check_card.record = instance.record;
			check_card.card = set.first_card + i;
			check_card.width = card.atlas_rect[2];
			check_card.height = card.atlas_rect[3];
			cards.push_back(check_card);
		}
	}
	LocalVector<uint32_t> jobs;
	const uint32_t samples = nextgi_hit_plan_check_jobs(cards.ptr(), cards.size(), NEXTGI_HIT_CHECK_MAX_SAMPLES, jobs);
	if (samples == 0) {
		return;
	}

	RenderingDevice *rd = RD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	const uint32_t material_count = MIN(in.material_index_count, uint32_t(NEXTGI_HIT_CHECK_MAX_MATERIALS));
	Vector<uint8_t> zeroed;
	zeroed.resize_initialized((NEXTGI_HIT_CHECK_COUNTERS + MAX(material_count, 1u)) * sizeof(uint32_t));
	const RID counter_buffer = rd->storage_buffer_create(zeroed.size(), zeroed);
	const RID job_buffer = rd->storage_buffer_create(jobs.size() * sizeof(uint32_t), Span<uint8_t>(reinterpret_cast<const uint8_t *>(jobs.ptr()), jobs.size() * sizeof(uint32_t)));

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_rt_scene->get_tlas()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_rt_scene->get_instance_record_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_rt_scene->get_surface_record_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, in.hit_surface_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, in.material_table_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, in.texture_slot_buffer));
	for (uint32_t i = 0; i < 4; i++) {
		RID chunk = mesh_storage != nullptr && i < mesh_storage->mesh_nextgi_rt_get_chunk_count() ? mesh_storage->mesh_nextgi_rt_get_chunk_buffer(i) : RID();
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, chunk.is_valid() ? chunk : in.dummy_buffer));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, material_storage->global_shader_uniforms_get_storage_buffer()));
	material_storage->samplers_rd_get_default().append_uniforms(uniforms, 11);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 23, p_src->get_atlas_texture(NextGISRC::ATLAS_ALBEDO)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 24, p_src->get_atlas_texture(NextGISRC::ATLAS_NORMAL)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 25, p_src->get_atlas_texture(NextGISRC::ATLAS_ORM)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 26, p_src->get_atlas_texture(NextGISRC::ATLAS_DEPTH)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, p_src->get_card_buffer()));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, job_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, counter_buffer));
	const RID scene_set = rd->uniform_set_create(uniforms, in.base_shader_rid, 0);

	struct PushConstant {
		float time;
		uint32_t record_count;
		uint32_t job_count;
		uint32_t sample_count;
		uint32_t material_count;
		uint32_t flags;
		uint32_t pad0;
		uint32_t pad1;
	};
	static_assert(sizeof(PushConstant) == 32, "HitParams is 32 bytes (nextgi_hit_layout_inc.glsl).");
	PushConstant push = {};
	push.time = p_time;
	// The CPU-fed instances only: GPU-written ones (custom index past them) have no record.
	push.record_count = in.record_count;
	push.job_count = jobs.size() / 4;
	push.sample_count = samples;
	push.material_count = material_count;
	push.flags = NEXTGI_HIT_PARAMS_CAPTURE_FACING;

	if (scene_set.is_valid()) {
		rd->draw_command_begin_label("NextGI Hit Check");
		RD::RaytracingListID list = rd->raytracing_list_begin();
		rd->raytracing_list_bind_raytracing_pipeline(list, in.linked);
		rd->raytracing_list_bind_uniform_set(list, scene_set, 0);
		rd->raytracing_list_bind_uniform_set(list, in.texture_table_set, 1);
		rd->raytracing_list_bind_uniform_set(list, in.params_table_set, 2);
		rd->raytracing_list_set_push_constant(list, &push, sizeof(push));
		rd->raytracing_list_trace_rays(list, 0, in.hit_sbt, samples, 1, 1);
		rd->raytracing_list_end();
		rd->draw_command_end_label();

		const Vector<uint8_t> result = rd->buffer_get_data(counter_buffer);
		if (result.size() >= int64_t(NEXTGI_HIT_CHECK_COUNTERS * sizeof(uint32_t))) {
			const uint32_t *values = reinterpret_cast<const uint32_t *>(result.ptr());
			for (uint32_t i = 0; i < NEXTGI_HIT_CHECK_COUNTERS; i++) {
				in.counters.check[i] = values[i];
			}
			uint32_t sampled = 0;
			for (uint32_t i = 0; i < material_count && NEXTGI_HIT_CHECK_COUNTERS + i < uint32_t(result.size() / sizeof(uint32_t)); i++) {
				if (values[NEXTGI_HIT_CHECK_COUNTERS + i] > 0) {
					sampled++;
				}
			}
			in.counters.check_materials = sampled;
			in.counters.check_runs++;
			in.last_check_frame = in.frame;
		}
		rd->free_rid(scene_set);
	}
	rd->free_rid(job_buffer);
	rd->free_rid(counter_buffer);
}

const NextGIHitMaterials::Counters &NextGIHitMaterials::get_counters() const {
	return internal->counters;
}

void NextGIHitMaterials::fill_counters(Dictionary &r_counters) const {
	const Counters &c = internal->counters;
	r_counters["hit_shaders"] = int64_t(c.shaders);
	r_counters["hit_groups_linked"] = int64_t(c.groups_linked);
	r_counters["hit_groups_pending"] = int64_t(c.groups_pending);
	r_counters["hit_ineligible"] = int64_t(c.ineligible);
	for (uint32_t i = 1; i < NEXTGI_HIT_INELIGIBLE_MAX; i++) {
		r_counters[String("hit_ineligible_") + String(nextgi_hit_ineligible_name(NextGIHitIneligible(i))).to_lower()] = int64_t(c.ineligible_reasons[i]);
	}
	r_counters["hit_light_skipped"] = int64_t(c.light_skipped);
	r_counters["hit_materials"] = int64_t(c.materials);
	r_counters["hit_geometries"] = int64_t(c.geometries);
	r_counters["hit_fallback_geometries"] = int64_t(c.fallback_geometries);
	r_counters["hit_no_stream_geometries"] = int64_t(c.no_stream_geometries);
	r_counters["hit_element_data_geometries"] = int64_t(c.element_data_geometries);
	r_counters["hit_groups_dead"] = int64_t(c.groups_dead);
	r_counters["hit_relinks"] = int64_t(c.relinks);
	r_counters["hit_sbt_ranges"] = int64_t(c.sbt_ranges);
	r_counters["hit_links"] = int64_t(c.links);
	r_counters["hit_group_add_ms_last"] = c.group_add_ms_last;
	r_counters["hit_group_add_ms_max"] = c.group_add_ms_max;
	r_counters["hit_link_ms_last"] = c.link_ms_last;
	r_counters["hit_link_ms_max"] = c.link_ms_max;
	r_counters["hit_check_runs"] = int64_t(c.check_runs);
	for (uint32_t i = 0; i < NEXTGI_HIT_CHECK_COUNTER_MAX; i++) {
		r_counters[String("hit_check_") + nextgi_hit_check_counter_name(NextGIHitCheckCounter(i))] = int64_t(c.check[i]);
	}
	r_counters["hit_check_materials"] = int64_t(c.check_materials);
	r_counters["hit_check_pending_shaders"] = int64_t(c.check_pending_shaders);
}

NextGIHitMaterials::NextGIHitMaterials() {
	internal = memnew(Internal);
	if (singleton == nullptr) {
		singleton = this;
	}
	internal->ready = _initialize();
}

NextGIHitMaterials::~NextGIHitMaterials() {
	Internal &in = *internal;
	RenderingDevice *rd = RD::get_singleton();
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (in.link_job != nullptr) {
		pool->wait_for_task_completion(in.link_job->task);
		if (in.link_job->result.is_valid() && rd != nullptr) {
			rd->free_rid(in.link_job->result);
		}
		memdelete(in.link_job);
		in.link_job = nullptr;
	}
	for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		if (E.value->task != WorkerThreadPool::INVALID_TASK_ID) {
			pool->wait_for_task_completion(E.value->task);
			E.value->task = WorkerThreadPool::INVALID_TASK_ID;
		}
	}
	if (rd != nullptr) {
		for (KeyValue<RID, Material> &E : in.materials) {
			_release_material_slots(E.value);
		}
		in.materials.clear();
		// The table first: it depends on the linked pipeline, which depends on the libraries.
		if (in.hit_sbt.is_valid()) {
			rd->free_rid(in.hit_sbt);
		}
		if (in.linked.is_valid()) {
			rd->free_rid(in.linked);
		}
		for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
			if (E.value->library.is_valid()) {
				rd->free_rid(E.value->library);
			}
		}
		if (in.base_library.is_valid()) {
			rd->free_rid(in.base_library);
		}
		const RID buffers[] = { in.texture_table_set, in.params_table_set, in.texture_table, in.params_table, in.material_table_buffer, in.texture_slot_buffer, in.hit_surface_buffer, in.dummy_buffer };
		for (const RID &rid : buffers) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
	}
	for (KeyValue<uint64_t, HitShader *> &E : in.shaders) {
		if (E.value->version.is_valid()) {
			in.material_shader.version_free(E.value->version);
		}
		memdelete(E.value);
	}
	in.shaders.clear();
	if (in.base_version.is_valid()) {
		in.base_shader.version_free(in.base_version);
	}
	memdelete(internal);
	internal = nullptr;
	if (singleton == this) {
		singleton = nullptr;
	}
}

} // namespace RendererRD
