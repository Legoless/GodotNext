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

// `.ngm` v2, the paged sidecar for one virtualized mesh (plan §7.2). It is not a
// resource: nothing here is registered as a ResourceFormatLoader or Saver, and `ngm`
// is not a resource extension, so the editor file scan and the stock export loop
// ignore the files.
//
// Little-endian. Header, 128 bytes:
//   0  char[4] magic "NGM1"
//   4  u32     version = 2 (readers also take 1)
//   8  u32     header_size = 128
//  12  u32     flags (bit 0 = meshopt codec; the provisional codec is raw, so bit 0 is clear)
//  16  u32     page_size (builder budget, bytes; provisional value is NGM_PROVISIONAL_PAGE_SIZE)
//  20  u32     page_count
//  24  u32     root_page_count
//  28  u32     cluster_count
//  32  u32     group_count
//  36  u32     dag_depth
//  40  u32     vertex_layout (NextGeoVertexLayout bits)
//  44  u32     skin_influences: 0, or 4/8 when flags bit 1 (skinned) is set
//  48  u64     page_table_offset
//  56  u8[32]  source_sha256 (SHA-256 of the raw page payloads, in page order)
//  88  u8[40]  zeros
//
// Page table, 32 bytes per page, at page_table_offset:
//   u64 offset; u32 stored_size, raw_size, first_group, group_count, crc32, reserved;
// `crc32` is the ISO-HDLC CRC of the stored bytes. For the raw codec, stored_size == raw_size.
//
// Each page starts 16-byte aligned. Its payload (the bytes `NGMFile::read_page` returns) is:
//   u32 cluster_count, vertex_count, triangle_count, skin_bytes;
//   cluster_count records of 80 bytes (NextGeoCluster layout, page-local vertex and triangle offsets;
//     bytes 64..67 hold the tagged max_edge in v2 and the page_id in v1, see NGM_RECORD_MAX_EDGE_OFFSET);
//   vertices, vertex_count * stride floats, stride derived from vertex_layout;
//   triangles, triangle_count * 3 bytes;
//   skinning section, `skin_bytes` bytes (0 when the skinned flag is clear), per cluster:
//     u8 bone_count, u8 influences, u16 reserved;
//     bone_count times: u16 global bone, 6 f32 bind-space AABB (min xyz, max xyz);
//     vertex_count * influences u8 cluster-local bone indices;
//     vertex_count * influences unorm16 weights.
// The skinning section is per-page so every stored page (section included) stays within
// `page_size`. The section did not change the version: files without it are plain, and a
// reader that predates the section rejects a skinned file on its unknown flag.
// Bytes between a page's stored payload and the next 16-byte boundary are zero and are not
// part of the CRC. `page_size` budgets the whole stored page: the 16-byte header, every
// cluster record, the vertices and triangles, and that padding. `raw_size` is the payload
// without the padding, so `raw_size` is at most `page_size`.

// GodotNext: nextgeo_sw_raster (#162). Version 2 keeps the v1 layout except bytes 64..67 of every
// cluster record. v1 stored the record's page_id there, which is always the page that holds the record
// and which no reader uses. v2 stores the cluster's longest triangle edge in mesh units, `max_edge`, as
// an IEEE binary16 rounded up (bytes 64..65), then NGM_MAX_EDGE_TAG (bytes 66..67). The hybrid
// rasterizer's triangle-size switch reads it. The tag marks the value in the page itself, because root
// pages also travel inside VirtualGeometryData, without the file header; a v1 page_id would need more
// than 1.29 billion pages to look tagged. The runtime computes max_edge for an untagged (v1) page when
// it installs it (ngm_page_fill_max_edges).
static constexpr uint32_t NGM_VERSION = 2;
static constexpr uint32_t NGM_VERSION_MIN = 1;
static constexpr uint32_t NGM_CLUSTER_RECORD_SIZE = 80;
static constexpr uint32_t NGM_RECORD_MAX_EDGE_OFFSET = 64;
static constexpr uint16_t NGM_MAX_EDGE_TAG = 0x4D45;
static constexpr uint32_t NGM_HEADER_SIZE = 128;
static constexpr uint32_t NGM_PAGE_TABLE_ENTRY_SIZE = 32;
static constexpr uint32_t NGM_PAGE_ALIGN = 16;
static constexpr uint32_t NGM_FLAG_MESHOPT = 1u << 0;
// Pages carry the skinning section, and `skin_influences` is 4 or 8 (issue #159).
static constexpr uint32_t NGM_FLAG_SKINNED = 1u << 1;
// GodotNext: nextgeo_voxels (#165). Pages may carry an optional trailing sparse-brick voxel section.
static constexpr uint32_t NGM_FLAG_VOXELS = 1u << 2;
static constexpr uint32_t NGM_KNOWN_FLAGS = NGM_FLAG_MESHOPT | NGM_FLAG_SKINNED | NGM_FLAG_VOXELS;
static constexpr uint32_t NGM_VOXEL_SECTION_MAGIC = 0x5856474Eu; // 'NGVX'
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
	uint32_t skin_influences = 0; // 0, 4 or 8; non-zero iff NGM_FLAG_SKINNED is set.
	uint8_t source_sha256[32] = {};
};

// GodotNext: nextgeo_sw_raster (#162). The max_edge field of `.ngm` v2 cluster records.
// The smallest IEEE binary16 at or above p_value: 0 for zero and negative values, +infinity (0x7C00)
// above 65504 and for NaN, so a stored max_edge never understates an edge.
uint16_t ngm_half_ceil(double p_value);
float ngm_half_to_float(uint16_t p_half);
// The longest edge of p_triangle_count triangles (three u8 cluster-local indices each) over vertices
// whose first three floats are the position, p_stride floats apart. 0 with no valid triangle;
// +infinity when a position is not finite, so the cluster stays on the hardware rasterizer.
double ngm_cluster_max_edge(const uint8_t *p_vertices, uint32_t p_stride, uint32_t p_vertex_count, const uint8_t *p_triangles, uint32_t p_triangle_count);
// Record bytes 64..67 of a v2 cluster record: ngm_half_ceil(p_max_edge) in the low half and
// NGM_MAX_EDGE_TAG in the high half.
uint32_t ngm_max_edge_word(double p_max_edge);
// True when every cluster record of a page payload carries a tagged max_edge (a v2 page). False for a
// v1 page or a payload whose header and records do not fit p_size.
bool ngm_page_has_max_edges(const uint8_t *p_page, uint64_t p_size);
// Writes the tagged max_edge into every cluster record of a page payload from its own vertices and
// triangles (+infinity for a cluster whose ranges leave the page). p_stride is the vertex stride in
// floats. False, with the payload unchanged, when its header does not fit it.
bool ngm_page_fill_max_edges(uint8_t *p_page, uint64_t p_size, uint32_t p_stride);
// GodotNext: nextgeo_voxels (#165). Detects and validates the optional trailing sparse-brick voxel
// section inside an `.ngm` page payload. When present, sets `r_header_offset` (4-byte aligned byte
// offset of the voxel section header from page start) and `r_total_bytes` (bytes from the end of
// the skinning section to `p_size`, including 0..3 alignment pad bytes and the 8-byte trailer).
// Returns false with zeroed outputs for plain pages without voxel data.
bool ngm_page_voxel_section(const uint8_t *p_page, uint64_t p_size, uint32_t &r_header_offset, uint32_t &r_total_bytes);

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
	// Checks magic, version (1 or 2; anything else, or a meshopt/unknown flag, is not raw v1 or v2)
	// and that the page table and every page range sit inside the file.
	// Version or magic mismatch returns ERR_FILE_UNRECOGNIZED. A truncated or inconsistent
	// table returns ERR_FILE_CORRUPT. An encrypted pack entry returns ERR_UNAVAILABLE.
	Error open(const String &p_path);
	// Bounds or CRC failure returns ERR_FILE_CORRUPT and clears `r_raw`. Never crashes.
	Error read_page(uint32_t p_index, Vector<uint8_t> &r_raw) const;
	// Reads only the 16-byte page header and 80-byte cluster records of `p_index` (raw v1 or v2).
	Error read_page_cluster_records(uint32_t p_index, uint32_t &r_cluster_count, Vector<uint8_t> &r_records) const;

	bool is_open() const { return opened; }
	const NGMHeader &get_header() const { return header; }
	uint32_t get_page_count() const { return pages.size(); }
	uint32_t get_page_first_group(uint32_t p_index) const { return p_index < uint32_t(pages.size()) ? pages[p_index].first_group : 0u; }
	uint32_t get_page_group_count(uint32_t p_index) const { return p_index < uint32_t(pages.size()) ? pages[p_index].group_count : 0u; }
	uint32_t get_page_raw_size(uint32_t p_index) const { return p_index < uint32_t(pages.size()) ? pages[p_index].raw_size : 0u; }
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
