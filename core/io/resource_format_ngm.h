/**************************************************************************/
/*  resource_format_ngm.h                                                 */
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

#pragma once

#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/vector.h"

// `.ngm` v1, the paged sidecar for one virtualized mesh (plan §7.2). It is not a
// resource: nothing here is registered as a ResourceFormatLoader or Saver, and `ngm`
// is not a resource extension, so the editor file scan and the stock export loop
// ignore the files.
//
// Little-endian. Header, 128 bytes:
//   0  char[4] magic "NGM1"
//   4  u32     version = 1
//   8  u32     header_size = 128
//  12  u32     flags (bit 0 = meshopt codec; the provisional codec is raw, so bit 0 is clear)
//  16  u32     page_size (builder budget, bytes; provisional value is NGM_PROVISIONAL_PAGE_SIZE)
//  20  u32     page_count
//  24  u32     root_page_count
//  28  u32     cluster_count
//  32  u32     group_count
//  36  u32     dag_depth
//  40  u32     vertex_layout (NextGeoVertexLayout bits)
//  44  u32     reserved = 0
//  48  u64     page_table_offset
//  56  u8[32]  source_sha256 (SHA-256 of the raw page payloads, in page order)
//  88  u8[40]  zeros
//
// Page table, 32 bytes per page, at page_table_offset:
//   u64 offset; u32 stored_size, raw_size, first_group, group_count, crc32, reserved;
// `crc32` is the ISO-HDLC CRC of the stored bytes. For the raw codec, stored_size == raw_size.
//
// Each page starts 16-byte aligned. Its payload (the bytes `NGMFile::read_page` returns) is:
//   u32 cluster_count, vertex_count, triangle_count, reserved;
//   cluster_count records of 80 bytes (NextGeoCluster layout, page-local vertex and triangle offsets);
//   vertices, vertex_count * stride floats, stride derived from vertex_layout;
//   triangles, triangle_count * 3 bytes.
// Bytes between a page's stored payload and the next 16-byte boundary are zero and are not
// part of the CRC. `page_size` budgets the whole stored page: the 16-byte header, every
// cluster record, the vertices and triangles, and that padding. `raw_size` is the payload
// without the padding, so `raw_size` is at most `page_size`.

static constexpr uint32_t NGM_VERSION = 1;
static constexpr uint32_t NGM_HEADER_SIZE = 128;
static constexpr uint32_t NGM_PAGE_TABLE_ENTRY_SIZE = 32;
static constexpr uint32_t NGM_PAGE_ALIGN = 16;
static constexpr uint32_t NGM_FLAG_MESHOPT = 1u << 0;
// Provisional until G4 measures 64 KiB against 128 KiB (plan §18.2, §22.10). Not a format limit:
// the header stores whatever budget the build used.
static constexpr uint32_t NGM_PROVISIONAL_PAGE_SIZE = 64 * 1024;
static constexpr int NGM_MAX_HANDLES = 16;

struct NGMHeader {
	uint32_t version = NGM_VERSION;
	uint32_t flags = 0;
	uint32_t page_size = 0;
	uint32_t page_count = 0;
	uint32_t root_page_count = 0;
	uint32_t cluster_count = 0;
	uint32_t group_count = 0;
	uint32_t dag_depth = 0;
	uint32_t vertex_layout = 0;
	uint8_t source_sha256[32] = {};
};

struct NGMPage {
	Vector<uint8_t> bytes;
	uint32_t first_group = 0;
	uint32_t group_count = 0;
};

class NGMWriter {
public:
	// Writes `p_path + ".tmp"` and renames it over `p_path`. The same header and pages
	// produce the same bytes. The provisional codec is raw: flags bit 0 returns ERR_UNAVAILABLE.
	static Error write(const String &p_path, const NGMHeader &p_header, const Vector<NGMPage> &p_pages);
};

// One open `.ngm`, either a loose file or a range inside a pack. The file handle lives in
// NGMHandlePool, not here, so a caller can keep many NGMFile objects and still only hold
// NGM_MAX_HANDLES descriptors.
class NGMFile : public RefCounted {
	struct PageEntry {
		uint64_t offset = 0;
		uint32_t stored_size = 0;
		uint32_t raw_size = 0;
		uint32_t first_group = 0;
		uint32_t group_count = 0;
		uint32_t crc = 0;
	};

	bool opened = false;
	String key;
	String open_path;
	uint64_t base_offset = 0;
	uint64_t file_size = 0;
	NGMHeader header;
	Vector<PageEntry> pages;

public:
	// Checks magic, version (anything other than 1, or a meshopt/unknown flag, is not v1 raw)
	// and that the page table and every page range sit inside the file.
	// Version or magic mismatch returns ERR_FILE_UNRECOGNIZED. A truncated or inconsistent
	// table returns ERR_FILE_CORRUPT. An encrypted pack entry returns ERR_UNAVAILABLE.
	Error open(const String &p_path);
	// Bounds or CRC failure returns ERR_FILE_CORRUPT and clears `r_raw`. Never crashes.
	Error read_page(uint32_t p_index, Vector<uint8_t> &r_raw) const;

	bool is_open() const { return opened; }
	const NGMHeader &get_header() const { return header; }
	uint32_t get_page_count() const { return pages.size(); }
};

// LRU of at most NGM_MAX_HANDLES file handles. A PCK is one key (the physical pack path);
// reads seek to the packed file's offset plus the page offset. ZIP and directory entries,
// and loose files, are opened with FileAccess::open and keyed by that path.
class NGMHandlePool {
	struct Handle {
		String key;
		Ref<FileAccess> file;
	};

	static NGMHandlePool &singleton();

public:
	static NGMHandlePool &get_singleton() { return singleton(); }

	int get_handle_count() const;
	void clear();

private:
	friend class NGMFile;

	Ref<FileAccess> _acquire(const String &p_key, const String &p_open_path, Error &r_error);
	Error get_length(const String &p_key, const String &p_open_path, uint64_t &r_length);
	Error read_at(const String &p_key, const String &p_open_path, uint64_t p_position, uint64_t p_length, Vector<uint8_t> &r_out);

	mutable Mutex mutex;
	List<Handle> order;
	HashMap<String, List<Handle>::Element *> by_key;
};
