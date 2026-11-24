/**************************************************************************/
/*  resource_format_ngm.cpp                                               */
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

#include "resource_format_ngm.h"

#include "core/io/dir_access.h"
#include "core/io/file_access_pack.h"

#include <zlib.h>

#include <cmath>
#include <cstring>

namespace {

// ISO-HDLC CRC-32, the same value zlib's crc32 returns. Empty input hashes to 0.
uint32_t ngm_crc32(const uint8_t *p_data, uint64_t p_size) {
	const uLong sum = crc32_z(0L, p_size == 0 ? nullptr : p_data, size_t(p_size));
	return uint32_t(sum);
}

uint32_t load_u32(const uint8_t *p_bytes) {
	return uint32_t(p_bytes[0]) | (uint32_t(p_bytes[1]) << 8) | (uint32_t(p_bytes[2]) << 16) | (uint32_t(p_bytes[3]) << 24);
}

uint64_t load_u64(const uint8_t *p_bytes) {
	return uint64_t(load_u32(p_bytes)) | (uint64_t(load_u32(p_bytes + 4)) << 32);
}

bool store_u32(const Ref<FileAccess> &p_file, uint32_t p_value) {
	uint8_t bytes[4] = { uint8_t(p_value), uint8_t(p_value >> 8), uint8_t(p_value >> 16), uint8_t(p_value >> 24) };
	return p_file->store_buffer(bytes, 4);
}

bool store_u64(const Ref<FileAccess> &p_file, uint64_t p_value) {
	return store_u32(p_file, uint32_t(p_value)) && store_u32(p_file, uint32_t(p_value >> 32));
}

Error discard_temp(const String &p_temp, Error p_error) {
	DirAccess::remove_absolute(p_temp);
	return p_error;
}

} // namespace

NGMHandlePool &NGMHandlePool::singleton() {
	static NGMHandlePool pool;
	return pool;
}

int NGMHandlePool::get_handle_count() const {
	MutexLock lock(mutex);
	return order.size();
}

void NGMHandlePool::clear() {
	MutexLock lock(mutex);
	by_key.clear();
	order.clear();
}

Ref<FileAccess> NGMHandlePool::_acquire(const String &p_key, const String &p_open_path, Error &r_error) {
	HashMap<String, List<Handle>::Element *>::Iterator found = by_key.find(p_key);
	if (found) {
		order.move_to_front(found->value);
		r_error = OK;
		return found->value->get().file;
	}

	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_open_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		r_error = open_error != OK ? open_error : ERR_FILE_CANT_OPEN;
		return Ref<FileAccess>();
	}

	while (order.size() >= NGM_MAX_HANDLES) {
		List<Handle>::Element *oldest = order.back();
		const String old_key = oldest->get().key;
		by_key.erase(old_key);
		order.erase(oldest);
	}

	Handle handle;
	handle.key = p_key;
	handle.file = file;
	List<Handle>::Element *element = order.push_front(handle);
	by_key.insert(p_key, element);
	r_error = OK;
	return file;
}

Error NGMHandlePool::get_length(const String &p_key, const String &p_open_path, uint64_t &r_length) {
	MutexLock lock(mutex);
	Error open_error = OK;
	Ref<FileAccess> file = _acquire(p_key, p_open_path, open_error);
	if (file.is_null()) {
		return open_error;
	}
	r_length = file->get_length();
	return OK;
}

Error NGMHandlePool::read_at(const String &p_key, const String &p_open_path, uint64_t p_position, uint64_t p_length, Vector<uint8_t> &r_out) {
	r_out.clear();
	if (p_length > uint64_t(INT32_MAX) || (p_length > 0 && p_position > UINT64_MAX - p_length)) {
		return ERR_FILE_CORRUPT;
	}

	MutexLock lock(mutex);
	Error open_error = OK;
	Ref<FileAccess> file = _acquire(p_key, p_open_path, open_error);
	if (file.is_null()) {
		return open_error;
	}
	if (p_position + p_length > file->get_length()) {
		return ERR_FILE_CORRUPT;
	}
	if (p_length == 0) {
		return OK;
	}

	file->seek(p_position);
	r_out.resize(int(p_length));
	const uint64_t got = file->get_buffer(r_out.ptrw(), p_length);
	if (got != p_length) {
		r_out.clear();
		return ERR_FILE_CORRUPT;
	}
	return OK;
}

// GodotNext: nextgeo_sw_raster (#162). The max_edge field of `.ngm` v2 cluster records.
uint16_t ngm_half_ceil(double p_value) {
	if (std::isnan(p_value) || p_value > 65504.0) {
		return 0x7C00u;
	}
	if (p_value <= 0.0) {
		return 0u;
	}
	int exponent = 0;
	std::frexp(p_value, &exponent);
	exponent -= 1; // p_value = m * 2^exponent with m in [1, 2).
	if (exponent < -14) {
		// Subnormal: units of 2^-24. 1024 units is the smallest normal value, whose bits are 0x0400.
		return uint16_t(std::ceil(std::ldexp(p_value, 24)));
	}
	const double mantissa = std::ceil((std::ldexp(p_value, -exponent) - 1.0) * 1024.0);
	// A mantissa of 1024 carries into the exponent, which is the next binade's first value.
	const uint32_t bits = (uint32_t(exponent + 15) << 10) + uint32_t(mantissa);
	return bits >= 0x7C00u ? uint16_t(0x7C00u) : uint16_t(bits);
}

float ngm_half_to_float(uint16_t p_half) {
	const uint32_t exponent = (p_half >> 10) & 0x1Fu;
	const uint32_t mantissa = p_half & 0x3FFu;
	const float sign = (p_half & 0x8000u) != 0 ? -1.0f : 1.0f;
	if (exponent == 0) {
		return sign * float(std::ldexp(double(mantissa), -24));
	}
	if (exponent == 31) {
		return mantissa != 0 ? NAN : sign * INFINITY;
	}
	return sign * float(std::ldexp(double(1024u + mantissa), int(exponent) - 25));
}

double ngm_cluster_max_edge(const uint8_t *p_vertices, uint32_t p_stride, uint32_t p_vertex_count, const uint8_t *p_triangles, uint32_t p_triangle_count) {
	double longest = 0.0;
	if (p_stride < 3) {
		return longest;
	}
	for (uint32_t t = 0; t < p_triangle_count; t++) {
		float corners[3][3];
		bool valid = true;
		for (uint32_t c = 0; c < 3; c++) {
			const uint32_t index = p_triangles[t * 3 + c];
			if (index >= p_vertex_count) {
				valid = false;
				break;
			}
			memcpy(corners[c], p_vertices + uint64_t(index) * p_stride * sizeof(float), sizeof(corners[c]));
		}
		if (!valid) {
			continue;
		}
		for (uint32_t e = 0; e < 3; e++) {
			const float *a = corners[e];
			const float *b = corners[(e + 1) % 3];
			const double dx = double(b[0]) - double(a[0]);
			const double dy = double(b[1]) - double(a[1]);
			const double dz = double(b[2]) - double(a[2]);
			const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
			// A position that is not finite makes the edge unbounded, so the cluster stays on the
			// hardware path.
			longest = std::isfinite(length) ? MAX(longest, length) : INFINITY;
		}
	}
	return longest;
}

uint32_t ngm_max_edge_word(double p_max_edge) {
	return uint32_t(ngm_half_ceil(p_max_edge)) | (uint32_t(NGM_MAX_EDGE_TAG) << 16);
}

bool ngm_page_has_max_edges(const uint8_t *p_page, uint64_t p_size) {
	if (p_page == nullptr || p_size < 16u) {
		return false;
	}
	const uint32_t cluster_count = load_u32(p_page);
	if (16ull + uint64_t(cluster_count) * NGM_CLUSTER_RECORD_SIZE > p_size) {
		return false;
	}
	for (uint32_t i = 0; i < cluster_count; i++) {
		const uint8_t *record = p_page + 16u + uint64_t(i) * NGM_CLUSTER_RECORD_SIZE;
		if ((load_u32(record + NGM_RECORD_MAX_EDGE_OFFSET) >> 16) != NGM_MAX_EDGE_TAG) {
			return false;
		}
	}
	return true;
}

bool ngm_page_fill_max_edges(uint8_t *p_page, uint64_t p_size, uint32_t p_stride) {
	if (p_page == nullptr || p_size < 16u || p_stride < 3u) {
		return false;
	}
	const uint32_t cluster_count = load_u32(p_page);
	const uint32_t vertex_count = load_u32(p_page + 4);
	const uint32_t triangle_count = load_u32(p_page + 8);
	const uint64_t vertex_begin = 16ull + uint64_t(cluster_count) * NGM_CLUSTER_RECORD_SIZE;
	const uint64_t triangle_begin = vertex_begin + uint64_t(vertex_count) * p_stride * sizeof(float);
	if (triangle_begin + uint64_t(triangle_count) * 3ull > p_size) {
		return false;
	}
	for (uint32_t i = 0; i < cluster_count; i++) {
		uint8_t *record = p_page + 16u + uint64_t(i) * NGM_CLUSTER_RECORD_SIZE;
		const uint32_t first_vertex = load_u32(record + 68);
		const uint32_t first_triangle = load_u32(record + 72);
		const uint32_t cluster_vertices = record[76];
		const uint32_t cluster_triangles = record[77];
		// A cluster whose ranges leave the page gets +infinity, which keeps it on the hardware path.
		double longest = INFINITY;
		if (uint64_t(first_vertex) + cluster_vertices <= vertex_count && uint64_t(first_triangle) + cluster_triangles <= triangle_count) {
			longest = ngm_cluster_max_edge(p_page + vertex_begin + uint64_t(first_vertex) * p_stride * sizeof(float), p_stride, cluster_vertices, p_page + triangle_begin + uint64_t(first_triangle) * 3ull, cluster_triangles);
		}
		const uint32_t word = ngm_max_edge_word(longest);
		for (uint32_t b = 0; b < 4; b++) {
			record[NGM_RECORD_MAX_EDGE_OFFSET + b] = uint8_t((word >> (8u * b)) & 0xFFu);
		}
	}
	return true;
}

bool ngm_page_voxel_section(const uint8_t *p_page, uint64_t p_size, uint32_t &r_header_offset, uint32_t &r_total_bytes) {
	r_header_offset = 0u;
	r_total_bytes = 0u;
	// Minimum section: 16-byte page header + 16-byte voxel header + 8-byte trailer = 40 bytes.
	if (p_page == nullptr || p_size < 40ull) {
		return false;
	}
	const uint32_t tail_bytes = load_u32(p_page + p_size - 8ull);
	const uint32_t tail_magic = load_u32(p_page + p_size - 4ull);
	if (tail_magic != NGM_VOXEL_SECTION_MAGIC || tail_bytes < 24u || uint64_t(tail_bytes) > p_size - 16ull) {
		return false;
	}
	const uint64_t base_end = p_size - uint64_t(tail_bytes);
	const uint64_t hdr_off = (base_end + 3ull) & ~3ull;
	if (hdr_off + 24ull > p_size || hdr_off > uint64_t(UINT32_MAX)) {
		return false;
	}
	if (load_u32(p_page + hdr_off) != NGM_VOXEL_SECTION_MAGIC) {
		return false;
	}
	const uint32_t group_count = load_u32(p_page + hdr_off + 8ull);
	const uint32_t total_bricks = load_u32(p_page + hdr_off + 12ull);
	const uint64_t dir_bytes = uint64_t(group_count) * 16ull;
	const uint64_t brick_area_start = hdr_off + 16ull + dir_bytes;
	const uint64_t trailer_start = p_size - 8ull;
	if (brick_area_start > trailer_start) {
		return false;
	}
	const uint64_t section_words = (trailer_start - hdr_off) >> 2u;
	if (((trailer_start - hdr_off) & 3ull) != 0ull) {
		return false;
	}
	uint64_t counted_bricks = 0ull;
	for (uint32_t g = 0; g < group_count; g++) {
		const uint8_t *dir = p_page + hdr_off + 16ull + uint64_t(g) * 16ull;
		const uint32_t brick_rel_word = load_u32(dir + 0u);
		const uint32_t brick_count = load_u32(dir + 4u);
		if (brick_count == 0u) {
			continue;
		}
		counted_bricks += brick_count;
		if (counted_bricks > uint64_t(total_bricks)) {
			return false;
		}
		uint64_t cursor = uint64_t(brick_rel_word);
		const uint64_t min_rel_word = (16ull + dir_bytes) >> 2u;
		if (cursor < min_rel_word || cursor >= section_words) {
			return false;
		}
		for (uint32_t b = 0; b < brick_count; b++) {
			if (cursor + 8ull > section_words) {
				return false;
			}
			const uint32_t meta = load_u32(p_page + hdr_off + (cursor + 6ull) * 4ull);
			const uint32_t occ_count = meta & 0xFFu;
			if (occ_count == 0u || occ_count > 64u) {
				return false;
			}
			cursor += 8ull + uint64_t(occ_count) * 2ull;
			if (cursor > section_words) {
				return false;
			}
		}
	}
	if (counted_bricks != uint64_t(total_bricks)) {
		return false;
	}
	r_header_offset = uint32_t(hdr_off);
	r_total_bytes = tail_bytes;
	return true;
}

Error NGMWriter::write(const String &p_path, const NGMHeader &p_header, const Vector<NGMPage> &p_pages) {
	ERR_FAIL_COND_V_MSG(p_path.is_empty(), ERR_INVALID_PARAMETER, "NGM: path is empty.");
	ERR_FAIL_COND_V_MSG(p_header.version != NGM_VERSION, ERR_INVALID_PARAMETER, "NGM: writer only emits version 2.");
	ERR_FAIL_COND_V_MSG(p_header.page_size == 0, ERR_INVALID_PARAMETER, "NGM: page_size is required.");
	ERR_FAIL_COND_V_MSG(p_header.page_count != uint32_t(p_pages.size()), ERR_INVALID_PARAMETER, "NGM: page_count does not match the page list.");
	ERR_FAIL_COND_V_MSG(p_header.root_page_count > p_header.page_count, ERR_INVALID_PARAMETER, "NGM: root_page_count exceeds page_count.");
	if ((p_header.flags & ~NGM_KNOWN_FLAGS) != 0) {
		ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, "NGM: unknown flag bits.");
	}
	const bool skinned = (p_header.flags & NGM_FLAG_SKINNED) != 0;
	ERR_FAIL_COND_V_MSG(skinned != (p_header.skin_influences == 4 || p_header.skin_influences == 8), ERR_INVALID_PARAMETER, "NGM: the skinned flag requires 4 or 8 skin influences, and skin influences require the flag.");
	if ((p_header.flags & NGM_FLAG_MESHOPT) != 0) {
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "NGM: the provisional codec is raw; meshopt pages are not written.");
	}

	Vector<uint64_t> offsets;
	offsets.resize(p_pages.size());
	Vector<uint32_t> checksums;
	checksums.resize(p_pages.size());

	uint64_t cursor = uint64_t(NGM_HEADER_SIZE) + uint64_t(p_pages.size()) * NGM_PAGE_TABLE_ENTRY_SIZE;
	for (int i = 0; i < p_pages.size(); i++) {
		const NGMPage &page = p_pages[i];
		const uint64_t stored = uint64_t(page.bytes.size());
		if (uint64_t(page.first_group) + uint64_t(page.group_count) > uint64_t(p_header.group_count)) {
			ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, "NGM: a page's group range is outside the header.");
		}
		if (cursor > UINT64_MAX - stored) {
			ERR_FAIL_V_MSG(ERR_OUT_OF_MEMORY, "NGM: page offsets overflow.");
		}
		offsets.write[i] = cursor;
		checksums.write[i] = ngm_crc32(page.bytes.size() == 0 ? nullptr : page.bytes.ptr(), stored);
		cursor += stored;
		const uint64_t pad = (NGM_PAGE_ALIGN - (stored % NGM_PAGE_ALIGN)) % NGM_PAGE_ALIGN;
		if (cursor > UINT64_MAX - pad) {
			ERR_FAIL_V_MSG(ERR_OUT_OF_MEMORY, "NGM: page padding overflows.");
		}
		cursor += pad;
	}

	const String temp_path = p_path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temp_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_CREATE;
	}

	const uint8_t magic[4] = { 'N', 'G', 'M', '1' };
	bool stored = file->store_buffer(magic, 4);
	stored = stored && store_u32(file, p_header.version);
	stored = stored && store_u32(file, NGM_HEADER_SIZE);
	stored = stored && store_u32(file, p_header.flags);
	stored = stored && store_u32(file, p_header.page_size);
	stored = stored && store_u32(file, p_header.page_count);
	stored = stored && store_u32(file, p_header.root_page_count);
	stored = stored && store_u32(file, p_header.cluster_count);
	stored = stored && store_u32(file, p_header.group_count);
	stored = stored && store_u32(file, p_header.dag_depth);
	stored = stored && store_u32(file, p_header.vertex_layout);
	stored = stored && store_u32(file, p_header.skin_influences);
	stored = stored && store_u64(file, NGM_HEADER_SIZE); // Page table follows the header.
	stored = stored && file->store_buffer(p_header.source_sha256, 32);
	uint8_t padding[40] = {};
	stored = stored && file->store_buffer(padding, 40);

	for (int i = 0; i < p_pages.size() && stored; i++) {
		const NGMPage &page = p_pages[i];
		stored = stored && store_u64(file, offsets[i]);
		stored = stored && store_u32(file, uint32_t(page.bytes.size()));
		stored = stored && store_u32(file, uint32_t(page.bytes.size())); // Raw codec: stored == raw.
		stored = stored && store_u32(file, page.first_group);
		stored = stored && store_u32(file, page.group_count);
		stored = stored && store_u32(file, checksums[i]);
		stored = stored && store_u32(file, 0);
	}

	uint8_t zeros[NGM_PAGE_ALIGN] = {};
	for (int i = 0; i < p_pages.size() && stored; i++) {
		const NGMPage &page = p_pages[i];
		if (page.bytes.size() > 0) {
			stored = stored && file->store_buffer(page.bytes.ptr(), page.bytes.size());
		}
		const uint32_t pad = (NGM_PAGE_ALIGN - (uint32_t(page.bytes.size()) % NGM_PAGE_ALIGN)) % NGM_PAGE_ALIGN;
		if (pad > 0) {
			stored = stored && file->store_buffer(zeros, pad);
		}
	}

	file->flush();
	file.unref();
	if (!stored) {
		return discard_temp(temp_path, ERR_CANT_CREATE);
	}

	const Error renamed = DirAccess::rename_absolute(temp_path, p_path);
	if (renamed != OK) {
		return discard_temp(temp_path, renamed);
	}
	return OK;
}

Error NGMFile::open(const String &p_path) {
	opened = false;
	pages.clear();
	header = NGMHeader();
	key.clear();
	open_path.clear();
	base_offset = 0;
	file_size = 0;

	ERR_FAIL_COND_V_MSG(p_path.is_empty(), ERR_INVALID_PARAMETER, "NGM: path is empty.");

	bool size_known = false;
	PackedData *packed = PackedData::get_singleton();
	if (packed != nullptr) {
		String pack;
		uint64_t offset = 0;
		uint64_t packed_size = 0;
		bool encrypted = false;
		bool is_pck = false;
		if (packed->get_file_location(p_path, pack, offset, packed_size, encrypted, is_pck)) {
			if (encrypted) {
				return ERR_UNAVAILABLE;
			}
			if (is_pck) {
				key = pack;
				open_path = pack;
				base_offset = offset;
				file_size = packed_size;
				size_known = true;
			} else {
				// ZIP and directory sources are not seekable by pack offset.
				key = p_path;
				open_path = p_path;
			}
		}
	}
	if (key.is_empty()) {
		key = p_path;
		open_path = p_path;
	}

	NGMHandlePool &pool = NGMHandlePool::get_singleton();
	uint64_t physical_length = 0;
	const Error length_error = pool.get_length(key, open_path, physical_length);
	if (length_error != OK) {
		return length_error;
	}
	if (!size_known) {
		file_size = physical_length;
		base_offset = 0;
	} else if (base_offset > physical_length || file_size > physical_length - base_offset) {
		return ERR_FILE_CORRUPT;
	}

	if (file_size < 4) {
		return ERR_FILE_CORRUPT;
	}

	const uint64_t prefix_length = file_size < NGM_HEADER_SIZE ? file_size : uint64_t(NGM_HEADER_SIZE);
	Vector<uint8_t> prefix;
	const Error prefix_error = pool.read_at(key, open_path, base_offset, prefix_length, prefix);
	if (prefix_error != OK) {
		return prefix_error;
	}
	if (prefix.size() < 4) {
		return ERR_FILE_CORRUPT;
	}
	const uint8_t expected_magic[4] = { 'N', 'G', 'M', '1' };
	if (memcmp(prefix.ptr(), expected_magic, 4) != 0) {
		return ERR_FILE_UNRECOGNIZED;
	}
	if (prefix.size() < 8) {
		return ERR_FILE_CORRUPT;
	}
	header.version = load_u32(prefix.ptr() + 4);
	if (header.version < NGM_VERSION_MIN || header.version > NGM_VERSION) {
		return ERR_FILE_UNRECOGNIZED;
	}
	if (prefix.size() < NGM_HEADER_SIZE) {
		return ERR_FILE_CORRUPT;
	}

	const uint8_t *bytes = prefix.ptr();
	const uint32_t header_size = load_u32(bytes + 8);
	if (header_size != NGM_HEADER_SIZE) {
		return ERR_FILE_CORRUPT;
	}
	header.flags = load_u32(bytes + 12);
	if ((header.flags & ~NGM_KNOWN_FLAGS) != 0) {
		return ERR_FILE_UNRECOGNIZED;
	}
	if ((header.flags & NGM_FLAG_MESHOPT) != 0) {
		return ERR_UNAVAILABLE;
	}
	header.page_size = load_u32(bytes + 16);
	header.page_count = load_u32(bytes + 20);
	header.root_page_count = load_u32(bytes + 24);
	header.cluster_count = load_u32(bytes + 28);
	header.group_count = load_u32(bytes + 32);
	header.dag_depth = load_u32(bytes + 36);
	header.vertex_layout = load_u32(bytes + 40);
	header.skin_influences = load_u32(bytes + 44);
	const bool skinned = (header.flags & NGM_FLAG_SKINNED) != 0;
	if (skinned != (header.skin_influences == 4 || header.skin_influences == 8)) {
		return ERR_FILE_CORRUPT;
	}
	if (header.page_size == 0 || header.root_page_count > header.page_count) {
		return ERR_FILE_CORRUPT;
	}
	const uint64_t table_offset = load_u64(bytes + 48);
	memcpy(header.source_sha256, bytes + 56, 32);
	for (uint32_t i = 88; i < NGM_HEADER_SIZE; i++) {
		if (bytes[i] != 0) {
			return ERR_FILE_CORRUPT;
		}
	}

	if (table_offset != NGM_HEADER_SIZE || table_offset > file_size) {
		return ERR_FILE_CORRUPT;
	}
	const uint64_t table_bytes = uint64_t(header.page_count) * NGM_PAGE_TABLE_ENTRY_SIZE;
	if (header.page_count != 0 && table_bytes / NGM_PAGE_TABLE_ENTRY_SIZE != header.page_count) {
		return ERR_FILE_CORRUPT;
	}
	if (table_bytes > file_size - table_offset) {
		return ERR_FILE_CORRUPT;
	}

	Vector<uint8_t> table;
	if (table_bytes > 0) {
		const Error table_error = pool.read_at(key, open_path, base_offset + table_offset, table_bytes, table);
		if (table_error != OK) {
			return table_error;
		}
	}

	const uint64_t table_end = table_offset + table_bytes;
	uint64_t previous_end = table_end;
	if (header.page_count > uint32_t(INT32_MAX)) {
		return ERR_FILE_CORRUPT;
	}
	pages.resize(header.page_count);
	for (uint32_t i = 0; i < header.page_count; i++) {
		const uint8_t *entry = table.ptr() + i * NGM_PAGE_TABLE_ENTRY_SIZE;
		PageEntry page;
		page.offset = load_u64(entry);
		page.stored_size = load_u32(entry + 8);
		page.raw_size = load_u32(entry + 12);
		page.first_group = load_u32(entry + 16);
		page.group_count = load_u32(entry + 20);
		page.crc = load_u32(entry + 24);
		if (load_u32(entry + 28) != 0 || page.stored_size != page.raw_size) {
			pages.clear();
			return ERR_FILE_CORRUPT;
		}
		if ((page.offset % NGM_PAGE_ALIGN) != 0 || page.offset < table_end || page.offset < previous_end) {
			pages.clear();
			return ERR_FILE_CORRUPT;
		}
		if (page.offset > file_size || uint64_t(page.stored_size) > file_size - page.offset) {
			pages.clear();
			return ERR_FILE_CORRUPT;
		}
		if (uint64_t(page.first_group) + uint64_t(page.group_count) > uint64_t(header.group_count)) {
			pages.clear();
			return ERR_FILE_CORRUPT;
		}
		previous_end = page.offset + page.stored_size;
		pages.write[i] = page;
	}

	auto align_up = [](uint64_t p_value, bool &r_overflow) {
		const uint64_t mask = NGM_PAGE_ALIGN - 1;
		if (p_value > UINT64_MAX - mask) {
			r_overflow = true;
			return uint64_t(0);
		}
		return (p_value + mask) & ~mask;
	};
	bool overflow = false;
	uint64_t expected = align_up(table_end, overflow);
	for (uint32_t i = 0; i < header.page_count && !overflow; i++) {
		const PageEntry &page = pages[i];
		if (page.offset != expected) {
			pages.clear();
			return ERR_FILE_CORRUPT;
		}
		const uint64_t payload_end = page.offset + page.stored_size;
		expected = align_up(payload_end, overflow);
		if (!overflow && expected > payload_end) {
			Vector<uint8_t> pad;
			const Error pad_error = pool.read_at(key, open_path, base_offset + payload_end, expected - payload_end, pad);
			if (pad_error != OK) {
				pages.clear();
				return pad_error;
			}
			for (int b = 0; b < pad.size(); b++) {
				if (pad[b] != 0) {
					pages.clear();
					return ERR_FILE_CORRUPT;
				}
			}
		}
	}
	if (overflow || file_size != expected) {
		pages.clear();
		return ERR_FILE_CORRUPT;
	}

	opened = true;
	return OK;
}

Error NGMFile::read_page(uint32_t p_index, Vector<uint8_t> &r_raw) const {
	r_raw.clear();
	if (!opened) {
		return ERR_UNCONFIGURED;
	}
	if (p_index >= uint32_t(pages.size())) {
		return ERR_FILE_CORRUPT;
	}
	const PageEntry &page = pages[p_index];
	if (page.offset > UINT64_MAX - base_offset) {
		return ERR_FILE_CORRUPT;
	}
	Vector<uint8_t> stored;
	const Error read_error = NGMHandlePool::get_singleton().read_at(key, open_path, base_offset + page.offset, page.stored_size, stored);
	if (read_error != OK) {
		return read_error;
	}
	if (ngm_crc32(stored.size() == 0 ? nullptr : stored.ptr(), stored.size()) != page.crc) {
		return ERR_FILE_CORRUPT;
	}
	r_raw = stored;
	return OK;
}

Error NGMFile::read_page_cluster_records(uint32_t p_index, uint32_t &r_cluster_count, Vector<uint8_t> &r_records) const {
	r_cluster_count = 0;
	r_records.clear();
	if (!opened) {
		return ERR_UNCONFIGURED;
	}
	if (p_index >= uint32_t(pages.size())) {
		return ERR_FILE_CORRUPT;
	}
	const PageEntry &page = pages[p_index];
	if (page.stored_size < 16u || page.offset > UINT64_MAX - base_offset) {
		return ERR_FILE_CORRUPT;
	}
	Vector<uint8_t> page_hdr;
	const Error hdr_error = NGMHandlePool::get_singleton().read_at(key, open_path, base_offset + page.offset, 16u, page_hdr);
	if (hdr_error != OK) {
		return hdr_error;
	}
	uint32_t cluster_count = 0;
	memcpy(&cluster_count, page_hdr.ptr(), sizeof(uint32_t));
	const uint64_t record_bytes = uint64_t(cluster_count) * 80ull;
	if (16ull + record_bytes > uint64_t(page.stored_size)) {
		return ERR_FILE_CORRUPT;
	}
	if (record_bytes > 0) {
		const Error rec_error = NGMHandlePool::get_singleton().read_at(key, open_path, base_offset + page.offset + 16ull, record_bytes, r_records);
		if (rec_error != OK) {
			r_records.clear();
			return rec_error;
		}
	}
	r_cluster_count = cluster_count;
	return OK;
}
