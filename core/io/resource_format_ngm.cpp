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

Error NGMWriter::write(const String &p_path, const NGMHeader &p_header, const Vector<NGMPage> &p_pages) {
	ERR_FAIL_COND_V_MSG(p_path.is_empty(), ERR_INVALID_PARAMETER, "NGM: path is empty.");
	ERR_FAIL_COND_V_MSG(p_header.version != NGM_VERSION, ERR_INVALID_PARAMETER, "NGM: writer only emits version 1.");
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
	if (header.version != NGM_VERSION) {
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
