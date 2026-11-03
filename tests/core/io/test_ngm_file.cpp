/**************************************************************************/
/*  test_ngm_file.cpp                                                     */
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

TEST_FORCE_LINK(test_ngm_file)

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_pack.h"
#include "core/io/pck_packer.h"
#include "core/io/resource_format_ngm.h"
#include "tests/test_utils.h"

namespace TestNGMFile {

struct PoolCleanup {
	Vector<String> packed_paths;
	Vector<String> files;
	~PoolCleanup() {
		PackedData *packed = PackedData::get_singleton();
		if (packed != nullptr) {
			for (const String &path : packed_paths) {
				packed->remove_path(path);
			}
		}
		for (const String &path : files) {
			DirAccess::remove_absolute(path);
		}
		NGMHandlePool::get_singleton().clear();
	}
};

void store_u32(Vector<uint8_t> &r_bytes, int p_offset, uint32_t p_value) {
	r_bytes.write[p_offset] = uint8_t(p_value);
	r_bytes.write[p_offset + 1] = uint8_t(p_value >> 8);
	r_bytes.write[p_offset + 2] = uint8_t(p_value >> 16);
	r_bytes.write[p_offset + 3] = uint8_t(p_value >> 24);
}

uint32_t load_u32(const Vector<uint8_t> &p_bytes, int p_offset) {
	return uint32_t(p_bytes[p_offset]) | (uint32_t(p_bytes[p_offset + 1]) << 8) | (uint32_t(p_bytes[p_offset + 2]) << 16) | (uint32_t(p_bytes[p_offset + 3]) << 24);
}

void store_u64(Vector<uint8_t> &r_bytes, int p_offset, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		r_bytes.write[p_offset + i] = uint8_t(p_value >> (i * 8));
	}
}

Error write_sample(const String &p_path, const Vector<uint8_t> &p_payload, uint32_t p_flags = 0) {
	NGMHeader header;
	header.version = NGM_VERSION;
	header.flags = p_flags;
	header.page_size = NGM_PROVISIONAL_PAGE_SIZE;
	header.page_count = 1;
	header.root_page_count = 1;
	header.cluster_count = 1;
	header.group_count = 1;
	header.dag_depth = 1;
	header.vertex_layout = 0;

	CryptoCore::SHA256Context sha;
	Error err = sha.start();
	if (err != OK) {
		return err;
	}
	if (!p_payload.is_empty()) {
		err = sha.update(p_payload.ptr(), p_payload.size());
		if (err != OK) {
			return err;
		}
	}
	err = sha.finish(header.source_sha256);
	if (err != OK) {
		return err;
	}

	NGMPage page;
	page.bytes = p_payload;
	page.first_group = 0;
	page.group_count = 1;
	Vector<NGMPage> pages;
	pages.push_back(page);
	return NGMWriter::write(p_path, header, pages);
}

Vector<uint8_t> sample_payload() {
	Vector<uint8_t> payload;
	payload.resize(16);
	for (int i = 0; i < payload.size(); i++) {
		payload.write[i] = uint8_t(0xA0 + i);
	}
	return payload;
}

TEST_CASE("[NextGeo][NGM] Round trip, determinism and rejection") {
	PoolCleanup cleanup;
	NGMHandlePool::get_singleton().clear();

	const Vector<uint8_t> payload = sample_payload();
	const String path_a = TestUtils::get_temp_path("ngm72_round_a.ngm");
	const String path_b = TestUtils::get_temp_path("ngm72_round_b.ngm");
	cleanup.files.push_back(path_a);
	cleanup.files.push_back(path_b);
	cleanup.files.push_back(path_a + ".tmp");
	cleanup.files.push_back(path_b + ".tmp");

	REQUIRE(write_sample(path_a, payload) == OK);
	REQUIRE(write_sample(path_b, payload) == OK);

	const Vector<uint8_t> bytes_a = FileAccess::get_file_as_bytes(path_a);
	const Vector<uint8_t> bytes_b = FileAccess::get_file_as_bytes(path_b);
	CHECK(bytes_a.size() == 176);
	CHECK(bytes_a == bytes_b);
	const String sha_a = FileAccess::get_sha256(path_a);
	const String sha_b = FileAccess::get_sha256(path_b);
	CHECK(sha_a == sha_b);
	CHECK(sha_a.length() == 64);

	CHECK(load_u32(bytes_a, 4) == 1);
	CHECK(load_u32(bytes_a, 8) == 128);
	CHECK(load_u32(bytes_a, 12) == 0);
	CHECK(load_u32(bytes_a, 16) == NGM_PROVISIONAL_PAGE_SIZE);

	Ref<NGMFile> file;
	file.instantiate();
	REQUIRE(file->open(path_a) == OK);
	CHECK(file->get_page_count() == 1);
	CHECK(file->get_header().page_size == NGM_PROVISIONAL_PAGE_SIZE);
	CHECK(file->get_header().flags == 0);
	Vector<uint8_t> read_back;
	REQUIRE(file->read_page(0, read_back) == OK);
	CHECK(read_back == payload);
	CHECK(file->read_page(1, read_back) == ERR_FILE_CORRUPT);

	const String empty_path = TestUtils::get_temp_path("ngm72_empty.ngm");
	cleanup.files.push_back(empty_path);
	NGMHeader empty_header;
	empty_header.version = NGM_VERSION;
	empty_header.page_size = NGM_PROVISIONAL_PAGE_SIZE;
	Vector<NGMPage> no_pages;
	REQUIRE(NGMWriter::write(empty_path, empty_header, no_pages) == OK);
	CHECK(FileAccess::get_file_as_bytes(empty_path).size() == 128);
	Ref<NGMFile> empty_file;
	empty_file.instantiate();
	REQUIRE(empty_file->open(empty_path) == OK);
	CHECK(empty_file->get_page_count() == 0);

	ERR_PRINT_OFF;
	NGMHeader rejected = empty_header;
	rejected.flags = NGM_FLAG_MESHOPT;
	CHECK(NGMWriter::write(empty_path, rejected, no_pages) == ERR_UNAVAILABLE);
	rejected.flags = 2;
	CHECK(NGMWriter::write(empty_path, rejected, no_pages) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

TEST_CASE("[NextGeo][NGM] Corrupt, truncated and version-mismatched files") {
	PoolCleanup cleanup;
	NGMHandlePool::get_singleton().clear();

	const String path = TestUtils::get_temp_path("ngm72_corrupt.ngm");
	cleanup.files.push_back(path);
	const Vector<uint8_t> payload = sample_payload();
	REQUIRE(write_sample(path, payload) == OK);
	const Vector<uint8_t> original = FileAccess::get_file_as_bytes(path);

	const int file_size = int(original.size());
	const int cuts[] = { 0, 3, 4, 8, 32, 127, 128, 129, file_size / 2, file_size - 1 };
	for (int length : cuts) {
		const String cut_path = TestUtils::get_temp_path("ngm72_cut_" + itos(length) + ".ngm");
		cleanup.files.push_back(cut_path);
		Ref<FileAccess> out = FileAccess::open(cut_path, FileAccess::WRITE);
		REQUIRE(out.is_valid());
		if (length > 0) {
			REQUIRE(out->store_buffer(original.ptr(), length));
		}
		out.unref();
		Ref<NGMFile> file;
		file.instantiate();
		const Error opened = file->open(cut_path);
		CHECK(opened != OK);
	}

	auto write_patched = [&](const String &p_name, const Vector<uint8_t> &p_bytes) {
		const String patched_path = TestUtils::get_temp_path(p_name);
		cleanup.files.push_back(patched_path);
		Ref<FileAccess> out = FileAccess::open(patched_path, FileAccess::WRITE);
		REQUIRE(out.is_valid());
		REQUIRE(out->store_buffer(p_bytes.ptr(), p_bytes.size()));
		return patched_path;
	};

	Vector<uint8_t> bad_magic = original;
	bad_magic.write[0] = 'X';
	{
		Ref<NGMFile> file;
		file.instantiate();
		CHECK(file->open(write_patched("ngm72_bad_magic.ngm", bad_magic)) == ERR_FILE_UNRECOGNIZED);
	}

	Vector<uint8_t> bad_version = original;
	store_u32(bad_version, 4, 2);
	{
		Ref<NGMFile> file;
		file.instantiate();
		CHECK(file->open(write_patched("ngm72_bad_version.ngm", bad_version)) == ERR_FILE_UNRECOGNIZED);
	}

	Vector<uint8_t> meshopt = original;
	store_u32(meshopt, 12, NGM_FLAG_MESHOPT);
	{
		Ref<NGMFile> file;
		file.instantiate();
		CHECK(file->open(write_patched("ngm72_meshopt.ngm", meshopt)) == ERR_UNAVAILABLE);
	}

	Vector<uint8_t> unknown_flag = original;
	store_u32(unknown_flag, 12, 2);
	{
		Ref<NGMFile> file;
		file.instantiate();
		CHECK(file->open(write_patched("ngm72_unknown_flag.ngm", unknown_flag)) == ERR_FILE_UNRECOGNIZED);
	}

	Vector<uint8_t> bad_crc = original;
	bad_crc.write[160] ^= 0xFF;
	{
		Ref<NGMFile> file;
		file.instantiate();
		REQUIRE(file->open(write_patched("ngm72_bad_crc.ngm", bad_crc)) == OK);
		Vector<uint8_t> ignored;
		CHECK(file->read_page(0, ignored) == ERR_FILE_CORRUPT);
	}

	// A 16-byte-aligned page offset past EOF used to wrap `file_size - offset` and skip the range check.
	Vector<uint8_t> past_eof = original;
	store_u64(past_eof, int(NGM_HEADER_SIZE), 0x00100000);
	{
		Ref<NGMFile> file;
		file.instantiate();
		CHECK(file->open(write_patched("ngm72_offset_past_eof.ngm", past_eof)) == ERR_FILE_CORRUPT);
	}
}

TEST_CASE("[NextGeo][NGM] Handle pool stays at 16 and a pack uses one") {
	PoolCleanup cleanup;
	NGMHandlePool::get_singleton().clear();

	const Vector<uint8_t> payload = sample_payload();
	DirAccess::make_dir_recursive_absolute(TestUtils::get_temp_path("ngm72_pool"));
	Vector<Ref<NGMFile>> live;
	live.resize(1000);
	for (int i = 0; i < live.size(); i++) {
		const String path = TestUtils::get_temp_path("ngm72_pool/file_" + itos(i) + ".ngm");
		cleanup.files.push_back(path);
		REQUIRE(write_sample(path, payload) == OK);
		live.write[i].instantiate();
		REQUIRE(live[i]->open(path) == OK);
		CHECK(NGMHandlePool::get_singleton().get_handle_count() <= NGM_MAX_HANDLES);
	}
	CHECK(NGMHandlePool::get_singleton().get_handle_count() == NGM_MAX_HANDLES);
	Vector<uint8_t> page;
	REQUIRE(live[999]->read_page(0, page) == OK);
	CHECK(page == payload);
	live.clear();

	NGMHandlePool::get_singleton().clear();
	Vector<Vector<uint8_t>> packed_payloads;
	Vector<String> packed_files;
	for (int i = 0; i < 3; i++) {
		Vector<uint8_t> distinct = payload;
		distinct.write[0] = uint8_t(i + 1);
		const String loose = TestUtils::get_temp_path("ngm72_pack_src_" + itos(i) + ".ngm");
		cleanup.files.push_back(loose);
		REQUIRE(write_sample(loose, distinct) == OK);
		packed_payloads.push_back(distinct);
		packed_files.push_back(loose);
	}

	const String pck_path = TestUtils::get_temp_path("ngm72_three.pck");
	cleanup.files.push_back(pck_path);
	PCKPacker packer;
	REQUIRE(packer.pck_start(pck_path) == OK);
	const String res_paths[3] = { "res://ngm72_a.ngm", "res://ngm72_b.ngm", "res://ngm72_c.ngm" };
	for (int i = 0; i < 3; i++) {
		REQUIRE(packer.add_file(res_paths[i], packed_files[i]) == OK);
		cleanup.packed_paths.push_back(res_paths[i]);
	}
	REQUIRE(packer.flush() == OK);
	REQUIRE(PackedData::get_singleton()->add_pack(pck_path, true, 0) == OK);

	String pack_path;
	uint64_t offset = 0;
	uint64_t size = 0;
	bool encrypted = true;
	bool is_pck = false;
	REQUIRE(PackedData::get_singleton()->get_file_location(res_paths[0], pack_path, offset, size, encrypted, is_pck));
	CHECK(is_pck);
	CHECK_FALSE(encrypted);
	CHECK(pack_path == pck_path);
	CHECK(offset != 0);
	CHECK(size == uint64_t(FileAccess::get_file_as_bytes(packed_files[0]).size()));

	Vector<Ref<NGMFile>> packed_live;
	packed_live.resize(3);
	for (int i = 0; i < 3; i++) {
		packed_live.write[i].instantiate();
		REQUIRE(packed_live[i]->open(res_paths[i]) == OK);
		Vector<uint8_t> read_back;
		REQUIRE(packed_live[i]->read_page(0, read_back) == OK);
		CHECK(read_back == packed_payloads[i]);
	}
	CHECK(NGMHandlePool::get_singleton().get_handle_count() == 1);

	NGMHandlePool::get_singleton().clear();
	const String enc_loose = TestUtils::get_temp_path("ngm72_enc_src.ngm");
	const String enc_pck = TestUtils::get_temp_path("ngm72_enc.pck");
	cleanup.files.push_back(enc_loose);
	cleanup.files.push_back(enc_pck);
	REQUIRE(write_sample(enc_loose, payload) == OK);
	const String enc_res = "res://ngm72_encrypted.ngm";
	cleanup.packed_paths.push_back(enc_res);
	PCKPacker enc_packer;
	REQUIRE(enc_packer.pck_start(enc_pck) == OK);
	REQUIRE(enc_packer.add_file(enc_res, enc_loose, true) == OK);
	REQUIRE(enc_packer.flush() == OK);
	REQUIRE(PackedData::get_singleton()->add_pack(enc_pck, true, 0) == OK);

	const int handles_before = NGMHandlePool::get_singleton().get_handle_count();
	Ref<NGMFile> encrypted_file;
	encrypted_file.instantiate();
	CHECK(encrypted_file->open(enc_res) == ERR_UNAVAILABLE);
	CHECK(NGMHandlePool::get_singleton().get_handle_count() == handles_before);
}

} // namespace TestNGMFile
