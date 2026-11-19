/**************************************************************************/
/*  test_nextgeo_shading_bins.cpp                                         */
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

TEST_FORCE_LINK(test_nextgeo_shading_bins)

// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The material shading bins' CPU side: the
// 16+16-bit pixel packing, the folded indirect arguments and their tail guard, the prefix sum and scatter
// of the reference binning on CPU data, a CPU run of shading_binning.glsl's per-tile rank and prefix
// steps against that reference, the compute form's rejects and the setting, and the GLSL copies of the
// layouts.

#include "core/math/random_pcg.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_resolve.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_shading_bins.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/resolve_compute_inc.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/shading_bins_inc.glsl.gen.h"

using namespace RendererRD;

namespace TestNextGeoShadingBins {

String glsl_constant(const char *p_source, const String &p_name) {
	const String source = String::utf8(p_source);
	const String key = " " + p_name + " = ";
	const int at = source.find(key);
	if (at < 0) {
		return String();
	}
	const int begin = at + key.length();
	const int end = source.find(";", begin);
	return end < 0 ? String() : source.substr(begin, end - begin).strip_edges();
}

int64_t glsl_integer(const String &p_value) {
	if (p_value.begins_with("0x")) {
		return p_value.trim_suffix("u").substr(2).hex_to_int();
	}
	return p_value.trim_suffix("u").to_int();
}

// resolve_compute_inc.glsl's declaration of a storage image of p_format at set 2, p_binding.
String glsl_image_declaration(RD::DataFormat p_format, uint32_t p_binding, const char *p_name) {
	const char *qualifier = "";
	bool unsigned_int = false;
	switch (p_format) {
		case RD::DATA_FORMAT_R8G8B8A8_UNORM:
			qualifier = "rgba8";
			break;
		case RD::DATA_FORMAT_R8G8_UINT:
			qualifier = "rg8ui";
			unsigned_int = true;
			break;
		case RD::DATA_FORMAT_R32_UINT:
			qualifier = "r32ui";
			unsigned_int = true;
			break;
		case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
			qualifier = "rgba16f";
			break;
		case RD::DATA_FORMAT_R16G16_SFLOAT:
			qualifier = "rg16f";
			break;
		default:
			return String();
	}
	return vformat("layout(%s, set = 2, binding = %d) uniform restrict writeonly %s %s;", qualifier, p_binding, unsigned_int ? "uimage2D" : "image2D", p_name);
}

// A row-major image of pixel bins: NEXTGEO_SHADING_BIN_NONE or a bin below p_bin_count.
LocalVector<uint32_t> random_pixel_bins(uint32_t p_width, uint32_t p_height, uint32_t p_bin_count, uint64_t p_seed) {
	RandomPCG rng(p_seed);
	LocalVector<uint32_t> bins;
	bins.resize(p_width * p_height);
	for (uint32_t i = 0; i < bins.size(); i++) {
		const uint32_t pick = rng.rand() % (p_bin_count + 1u);
		bins[i] = pick == p_bin_count ? NEXTGEO_SHADING_BIN_NONE : pick;
	}
	return bins;
}

// shading_binning.glsl's PREFIX pass, one 64-invocation workgroup run step by step: an inclusive
// Hillis-Steele scan of 64 counts at a time in shared memory, plus the running total.
LocalVector<uint32_t> gpu_prefix(const LocalVector<uint32_t> &p_counts) {
	const uint32_t bin_count = p_counts.size();
	LocalVector<uint32_t> offsets;
	offsets.resize(bin_count);
	uint32_t running = 0u;
	for (uint32_t base = 0u; base < bin_count; base += 64u) {
		uint32_t scan[64];
		uint32_t count[64];
		for (uint32_t tid = 0u; tid < 64u; tid++) {
			count[tid] = base + tid < bin_count ? p_counts[base + tid] : 0u;
			scan[tid] = count[tid];
		}
		for (uint32_t offset = 1u; offset < 64u; offset <<= 1u) {
			uint32_t add[64];
			for (uint32_t tid = 0u; tid < 64u; tid++) {
				add[tid] = tid >= offset ? scan[tid - offset] : 0u;
			}
			for (uint32_t tid = 0u; tid < 64u; tid++) {
				scan[tid] += add[tid];
			}
		}
		for (uint32_t tid = 0u; tid < 64u; tid++) {
			if (base + tid < bin_count) {
				offsets[base + tid] = running + scan[tid] - count[tid];
			}
		}
		running += scan[63];
	}
	return offsets;
}

// shading_binning.glsl's COUNT, PREFIX and SCATTER passes on CPU data, with the tiles taking their runs
// in p_tile_order (on the GPU, the order of the atomics). Each pixel's rank, its tile's count of its bin
// and the first pixel of the bin in the tile are computed as the shader does.
void gpu_binning(const LocalVector<uint32_t> &p_pixel_bins, uint32_t p_width, uint32_t p_height, uint32_t p_bin_count, const LocalVector<uint32_t> &p_tile_order, LocalVector<uint32_t> &r_counts, LocalVector<uint32_t> &r_offsets, LocalVector<uint32_t> &r_pixels) {
	const uint32_t tiles_x = (p_width + 7u) / 8u;
	auto tile_bins = [&](uint32_t p_tile, uint32_t r_bins[64]) {
		const uint32_t tx = p_tile % tiles_x;
		const uint32_t ty = p_tile / tiles_x;
		for (uint32_t tid = 0u; tid < 64u; tid++) {
			const uint32_t x = tx * 8u + tid % 8u;
			const uint32_t y = ty * 8u + tid / 8u;
			r_bins[tid] = (x < p_width && y < p_height && p_pixel_bins[y * p_width + x] < p_bin_count) ? p_pixel_bins[y * p_width + x] : NEXTGEO_SHADING_BIN_NONE;
		}
	};
	auto rank_of = [](const uint32_t p_bins[64], uint32_t p_tid, uint32_t &r_rank, uint32_t &r_total, uint32_t &r_first) {
		r_rank = 0u;
		r_total = 0u;
		r_first = p_tid;
		if (p_bins[p_tid] == NEXTGEO_SHADING_BIN_NONE) {
			return;
		}
		for (uint32_t i = 0u; i < 64u; i++) {
			if (p_bins[i] == p_bins[p_tid]) {
				if (i < p_tid) {
					r_rank++;
					r_first = MIN(r_first, i);
				}
				r_total++;
			}
		}
	};

	r_counts.resize(p_bin_count);
	for (uint32_t b = 0u; b < p_bin_count; b++) {
		r_counts[b] = 0u;
	}
	for (const uint32_t tile : p_tile_order) {
		uint32_t bins[64];
		tile_bins(tile, bins);
		for (uint32_t tid = 0u; tid < 64u; tid++) {
			uint32_t rank, total, first;
			rank_of(bins, tid, rank, total, first);
			if (bins[tid] != NEXTGEO_SHADING_BIN_NONE && rank == 0u) {
				r_counts[bins[tid]] += total;
			}
		}
	}

	r_offsets = gpu_prefix(r_counts);
	uint32_t total_pixels = 0u;
	for (uint32_t b = 0u; b < p_bin_count; b++) {
		total_pixels += r_counts[b];
	}
	r_pixels.resize(total_pixels);
	LocalVector<uint32_t> cursors;
	cursors.resize(p_bin_count);
	for (uint32_t b = 0u; b < p_bin_count; b++) {
		cursors[b] = 0u;
	}
	for (const uint32_t tile : p_tile_order) {
		uint32_t bins[64];
		uint32_t runs[64] = {};
		tile_bins(tile, bins);
		for (uint32_t tid = 0u; tid < 64u; tid++) {
			uint32_t rank, total, first;
			rank_of(bins, tid, rank, total, first);
			if (bins[tid] != NEXTGEO_SHADING_BIN_NONE && rank == 0u) {
				runs[tid] = cursors[bins[tid]];
				cursors[bins[tid]] += total;
			}
		}
		for (uint32_t tid = 0u; tid < 64u; tid++) {
			uint32_t rank, total, first;
			rank_of(bins, tid, rank, total, first);
			if (bins[tid] != NEXTGEO_SHADING_BIN_NONE) {
				const uint32_t tx = tile % tiles_x;
				const uint32_t ty = tile / tiles_x;
				r_pixels[r_offsets[bins[tid]] + runs[first] + rank] = nextgeo_shading_pack_pixel(tx * 8u + tid % 8u, ty * 8u + tid / 8u);
			}
		}
	}
}

bool same(const LocalVector<uint32_t> &p_a, const LocalVector<uint32_t> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < p_a.size(); i++) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

// The pixels of bin p_bin in the list, as a set.
HashSet<uint32_t> bin_pixels(const LocalVector<uint32_t> &p_pixels, uint32_t p_offset, uint32_t p_count) {
	HashSet<uint32_t> out;
	for (uint32_t i = 0u; i < p_count; i++) {
		out.insert(p_pixels[p_offset + i]);
	}
	return out;
}

} // namespace TestNextGeoShadingBins

using namespace TestNextGeoShadingBins;

TEST_CASE("[NextGeo][ShadingBins] Pixel coordinates pack 16+16 bits and round-trip") {
	CHECK_EQ(nextgeo_shading_pack_pixel(0u, 0u), 0u);
	CHECK_EQ(nextgeo_shading_pack_pixel(1u, 0u), 1u);
	CHECK_EQ(nextgeo_shading_pack_pixel(0u, 1u), 0x10000u);
	CHECK_EQ(nextgeo_shading_pack_pixel(1234u, 4321u), 1234u | (4321u << 16u));
	CHECK_EQ(nextgeo_shading_pack_pixel(NEXTGEO_SHADING_MAX_EXTENT, NEXTGEO_SHADING_MAX_EXTENT), 0xFFFFFFFFu);
	// The largest pixel packs to the no-bin word, but the list never holds a word for "no pixel".
	const uint32_t samples[][2] = { { 0u, 0u }, { 7u, 9u }, { 3839u, 2159u }, { 7679u, 4319u }, { 65535u, 0u }, { 0u, 65535u } };
	for (const auto &sample : samples) {
		uint32_t x = 0u;
		uint32_t y = 0u;
		nextgeo_shading_unpack_pixel(nextgeo_shading_pack_pixel(sample[0], sample[1]), x, y);
		CHECK_EQ(x, sample[0]);
		CHECK_EQ(y, sample[1]);
	}
}

TEST_CASE("[NextGeo][ShadingBins] Indirect arguments are ceil(count / 64) workgroups, folded under the dispatch limit") {
	const struct {
		uint32_t count;
		uint32_t x, y;
	} cases[] = {
		{ 0u, 0u, 0u },
		{ 1u, 1u, 1u },
		{ 63u, 1u, 1u },
		{ 64u, 1u, 1u },
		{ 65u, 2u, 1u },
		{ 64u * NEXTGEO_SHADING_DISPATCH_WIDTH, NEXTGEO_SHADING_DISPATCH_WIDTH, 1u },
		// One workgroup past the width: two rows of half the width, one workgroup to spare.
		{ 64u * NEXTGEO_SHADING_DISPATCH_WIDTH + 1u, NEXTGEO_SHADING_DISPATCH_WIDTH / 2u + 1u, 2u },
		{ 1920u * 1080u, 32400u, 1u },
		// A full-screen bin at 2560x1440 needs 57600 workgroups, at 3840x2160 129600.
		{ 2560u * 1440u, 28800u, 2u },
		{ 3840u * 2160u, 32400u, 4u },
		// The largest bin: 65535x65535 pixels.
		{ 0xFFFFu * 0xFFFFu, 32768u, 2048u },
	};
	for (const auto &c : cases) {
		uint32_t args[3];
		nextgeo_shading_dispatch_args(c.count, args);
		CHECK_EQ(args[0], c.x);
		CHECK_EQ(args[1], c.y);
		CHECK_EQ(args[2], 1u);
		// Every dimension stays under the portable limit of 65535 workgroups.
		CHECK(args[0] <= NEXTGEO_SHADING_DISPATCH_WIDTH);
		CHECK(args[1] <= 65535u);
		// The dispatch covers every pixel of the bin, and runs fewer than one row's count (y) of workgroups
		// wholly past it.
		const uint64_t groups = (uint64_t(c.count) + NEXTGEO_SHADING_GROUP_SIZE - 1u) / NEXTGEO_SHADING_GROUP_SIZE;
		const uint64_t dispatched = uint64_t(args[0]) * uint64_t(args[1]);
		CHECK(dispatched * NEXTGEO_SHADING_GROUP_SIZE >= c.count);
		CHECK(dispatched - groups < MAX(uint64_t(args[1]), uint64_t(1u)));
	}
}

TEST_CASE("[NextGeo][ShadingBins] The slots of a folded dispatch take each bin pixel once; the tail shades nothing") {
	// Small bins: enumerate every invocation of the dispatch.
	const uint32_t counts[] = { 1u, 63u, 64u, 65u, 130u, 1000u };
	for (const uint32_t count : counts) {
		uint32_t args[3];
		nextgeo_shading_dispatch_args(count, args);
		CHECK_EQ(args[1], 1u);
		LocalVector<uint32_t> hits;
		hits.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			hits[i] = 0u;
		}
		uint32_t tail = 0u;
		for (uint32_t gy = 0u; gy < args[1]; gy++) {
			for (uint32_t gx = 0u; gx < args[0]; gx++) {
				for (uint32_t local = 0u; local < NEXTGEO_SHADING_GROUP_SIZE; local++) {
					const uint32_t slot = nextgeo_shading_slot(gx, gy, args[0], local);
					if (slot >= count) {
						tail++;
						continue;
					}
					hits[slot]++;
				}
			}
		}
		for (uint32_t i = 0; i < count; i++) {
			CHECK_EQ(hits[i], 1u);
		}
		CHECK_EQ(tail, args[0] * args[1] * NEXTGEO_SHADING_GROUP_SIZE - count);
	}
	// Folded bins: every row's slots follow the previous row's, each workgroup's slots are distinct and in
	// order, and the slots below the count are exactly the bin's pixels.
	const uint32_t folded[] = { 64u * NEXTGEO_SHADING_DISPATCH_WIDTH + 100u, 2560u * 1440u, 3840u * 2160u - 5u };
	for (const uint32_t count : folded) {
		uint32_t args[3];
		nextgeo_shading_dispatch_args(count, args);
		CHECK(args[1] > 1u);
		for (uint32_t gy = 1u; gy < args[1]; gy++) {
			CHECK_EQ(nextgeo_shading_slot(args[0] - 1u, gy - 1u, args[0], 63u) + 1u, nextgeo_shading_slot(0u, gy, args[0], 0u));
		}
		// The last pixel of the bin sits in the last row, and the dispatch's very last slot is at or past it.
		const uint32_t last_group = (count - 1u) / NEXTGEO_SHADING_GROUP_SIZE;
		CHECK_EQ(nextgeo_shading_slot(last_group % args[0], last_group / args[0], args[0], (count - 1u) % NEXTGEO_SHADING_GROUP_SIZE), count - 1u);
		CHECK_EQ(last_group / args[0], args[1] - 1u);
		CHECK(nextgeo_shading_slot(args[0] - 1u, args[1] - 1u, args[0], 63u) >= count - 1u);
		CHECK(uint64_t(args[0]) * args[1] * NEXTGEO_SHADING_GROUP_SIZE - count < uint64_t(args[1]) * NEXTGEO_SHADING_GROUP_SIZE);
	}
}

TEST_CASE("[NextGeo][ShadingBins] Prefix sum and scatter on CPU data: a worked example") {
	// 10x9 pixels: two 8x8 tiles across and two down, the right and bottom ones partial.
	const uint32_t width = 10u;
	const uint32_t height = 9u;
	LocalVector<uint32_t> pixel_bins;
	pixel_bins.resize(width * height);
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			// Bin 2 on the left half, bin 0 on row 8, no bin at x = 9, bin 1 elsewhere.
			uint32_t bin = x < 5u ? 2u : 1u;
			if (y == 8u) {
				bin = 0u;
			}
			if (x == 9u) {
				bin = NEXTGEO_SHADING_BIN_NONE;
			}
			pixel_bins[y * width + x] = bin;
		}
	}
	NextGeoShadingBinsReference ref;
	nextgeo_shading_bins_build(pixel_bins.ptr(), width, height, 4u, ref);
	// Bin 0: row 8, x 0..8 = 9 pixels. Bin 1: x 5..8, rows 0..7 = 32. Bin 2: x 0..4, rows 0..7 = 40. Bin 3: empty.
	CHECK_EQ(ref.counts[0], 9u);
	CHECK_EQ(ref.counts[1], 32u);
	CHECK_EQ(ref.counts[2], 40u);
	CHECK_EQ(ref.counts[3], 0u);
	CHECK_EQ(ref.offsets[0], 0u);
	CHECK_EQ(ref.offsets[1], 9u);
	CHECK_EQ(ref.offsets[2], 41u);
	CHECK_EQ(ref.offsets[3], 81u);
	CHECK_EQ(ref.pixels.size(), 81u);
	// Bin 0's pixels: the bottom-left tile's row (x 0..7), then the bottom-right tile's pixel (8, 8).
	for (uint32_t i = 0; i < 8u; i++) {
		CHECK_EQ(ref.pixels[ref.offsets[0] + i], nextgeo_shading_pack_pixel(i, 8u));
	}
	CHECK_EQ(ref.pixels[ref.offsets[0] + 8u], nextgeo_shading_pack_pixel(8u, 8u));
	// Bin 1: the top-left tile's x 5..7 rows 0..7 (24 pixels), then the top-right tile's x 8 (8 pixels).
	CHECK_EQ(ref.pixels[ref.offsets[1] + 0u], nextgeo_shading_pack_pixel(5u, 0u));
	CHECK_EQ(ref.pixels[ref.offsets[1] + 3u], nextgeo_shading_pack_pixel(5u, 1u));
	CHECK_EQ(ref.pixels[ref.offsets[1] + 23u], nextgeo_shading_pack_pixel(7u, 7u));
	CHECK_EQ(ref.pixels[ref.offsets[1] + 24u], nextgeo_shading_pack_pixel(8u, 0u));
	CHECK_EQ(ref.pixels[ref.offsets[1] + 31u], nextgeo_shading_pack_pixel(8u, 7u));
	// Arguments: ceil(9 / 64) = 1, ceil(32 / 64) = 1, ceil(40 / 64) = 1, an empty bin dispatches nothing.
	CHECK_EQ(ref.args[0 * NEXTGEO_SHADING_ARGS_WORDS + 0u], 1u);
	CHECK_EQ(ref.args[2 * NEXTGEO_SHADING_ARGS_WORDS + 0u], 1u);
	CHECK_EQ(ref.args[3 * NEXTGEO_SHADING_ARGS_WORDS + 0u], 0u);
	CHECK_EQ(ref.args[3 * NEXTGEO_SHADING_ARGS_WORDS + 2u], 1u);
}

TEST_CASE("[NextGeo][ShadingBins] Prefix sum and scatter on CPU data: random images keep every pixel exactly once") {
	const struct {
		uint32_t width, height, bins;
		uint64_t seed;
	} cases[] = {
		{ 1u, 1u, 1u, 1u },
		{ 37u, 21u, 5u, 2u },
		{ 64u, 64u, 1u, 3u },
		{ 131u, 67u, 63u, 4u },
		{ 96u, 40u, 64u, 5u },
		{ 50u, 50u, 65u, 6u },
		{ 200u, 120u, 200u, 7u },
	};
	for (const auto &c : cases) {
		const LocalVector<uint32_t> pixel_bins = random_pixel_bins(c.width, c.height, c.bins, c.seed);
		NextGeoShadingBinsReference ref;
		nextgeo_shading_bins_build(pixel_bins.ptr(), c.width, c.height, c.bins, ref);

		// Counts are the histogram; offsets its exclusive prefix sum; the list holds every binned pixel.
		LocalVector<uint32_t> histogram;
		histogram.resize(c.bins);
		for (uint32_t b = 0; b < c.bins; b++) {
			histogram[b] = 0u;
		}
		uint32_t binned = 0u;
		for (const uint32_t bin : pixel_bins) {
			if (bin != NEXTGEO_SHADING_BIN_NONE) {
				histogram[bin]++;
				binned++;
			}
		}
		uint32_t running = 0u;
		for (uint32_t b = 0; b < c.bins; b++) {
			CHECK_EQ(ref.counts[b], histogram[b]);
			CHECK_EQ(ref.offsets[b], running);
			running += histogram[b];
			uint32_t args[3];
			nextgeo_shading_dispatch_args(histogram[b], args);
			CHECK_EQ(ref.args[b * NEXTGEO_SHADING_ARGS_WORDS + 0u], args[0]);
			CHECK_EQ(ref.args[b * NEXTGEO_SHADING_ARGS_WORDS + 1u], args[1]);
		}
		CHECK_EQ(ref.pixels.size(), binned);

		// Each bin's range holds exactly its pixels, and each pixel once.
		HashSet<uint32_t> seen;
		bool ranges_ok = true;
		for (uint32_t b = 0; b < c.bins; b++) {
			for (uint32_t i = 0; i < ref.counts[b]; i++) {
				uint32_t x = 0u;
				uint32_t y = 0u;
				const uint32_t packed = ref.pixels[ref.offsets[b] + i];
				nextgeo_shading_unpack_pixel(packed, x, y);
				ranges_ok = ranges_ok && x < c.width && y < c.height && pixel_bins[y * c.width + x] == b && !seen.has(packed);
				seen.insert(packed);
			}
		}
		CHECK(ranges_ok);
		CHECK_EQ(uint32_t(seen.size()), binned);
	}
}

TEST_CASE("[NextGeo][ShadingBins] shading_binning.glsl's per-tile ranks and chunked prefix match the reference") {
	// The prefix pass alone, across the 64-bin chunk boundaries.
	const uint32_t bin_counts[] = { 0u, 1u, 63u, 64u, 65u, 127u, 128u, 129u, 300u };
	for (const uint32_t bin_count : bin_counts) {
		RandomPCG rng(bin_count + 11u);
		LocalVector<uint32_t> counts;
		counts.resize(bin_count);
		for (uint32_t b = 0; b < bin_count; b++) {
			counts[b] = rng.rand() % 5000u;
		}
		const LocalVector<uint32_t> offsets = gpu_prefix(counts);
		uint32_t running = 0u;
		bool prefix_ok = true;
		for (uint32_t b = 0; b < bin_count; b++) {
			prefix_ok = prefix_ok && offsets[b] == running;
			running += counts[b];
		}
		CHECK(prefix_ok);
	}

	// The whole binning, with the tiles in raster order (equal to the reference list) and in a shuffled
	// order (the GPU's atomic order: same counts and offsets, same pixels per bin, each tile's run intact).
	const uint32_t width = 77u;
	const uint32_t height = 45u;
	const uint32_t bins = 70u;
	const LocalVector<uint32_t> pixel_bins = random_pixel_bins(width, height, 9u, 12u);
	LocalVector<uint32_t> spread(pixel_bins);
	for (uint32_t i = 0; i < spread.size(); i++) {
		if (spread[i] != NEXTGEO_SHADING_BIN_NONE) {
			spread[i] = (spread[i] * 7u + i % 3u) % bins;
		}
	}
	NextGeoShadingBinsReference ref;
	nextgeo_shading_bins_build(spread.ptr(), width, height, bins, ref);

	const uint32_t tile_count = ((width + 7u) / 8u) * ((height + 7u) / 8u);
	LocalVector<uint32_t> raster_order;
	for (uint32_t t = 0; t < tile_count; t++) {
		raster_order.push_back(t);
	}
	LocalVector<uint32_t> counts, offsets, pixels;
	gpu_binning(spread, width, height, bins, raster_order, counts, offsets, pixels);
	CHECK(same(counts, ref.counts));
	CHECK(same(offsets, ref.offsets));
	CHECK(same(pixels, ref.pixels));

	LocalVector<uint32_t> shuffled(raster_order);
	RandomPCG rng(99u);
	for (uint32_t i = shuffled.size() - 1u; i > 0u; i--) {
		SWAP(shuffled[i], shuffled[rng.rand() % (i + 1u)]);
	}
	gpu_binning(spread, width, height, bins, shuffled, counts, offsets, pixels);
	CHECK(same(counts, ref.counts));
	CHECK(same(offsets, ref.offsets));
	CHECK_FALSE(same(pixels, ref.pixels));
	bool sets_ok = true;
	for (uint32_t b = 0; b < bins; b++) {
		sets_ok = sets_ok && bin_pixels(pixels, offsets[b], counts[b]).size() == counts[b];
		for (const uint32_t packed : bin_pixels(ref.pixels, ref.offsets[b], ref.counts[b])) {
			sets_ok = sets_ok && bin_pixels(pixels, offsets[b], counts[b]).has(packed);
		}
	}
	CHECK(sets_ok);
	// Within a bin, each tile's pixels are one run in raster order within the tile.
	bool runs_ok = true;
	for (uint32_t b = 0; b < bins; b++) {
		HashSet<uint32_t> finished_tiles;
		uint32_t current_tile = UINT32_MAX;
		uint32_t previous_local = 0u;
		for (uint32_t i = 0; i < counts[b]; i++) {
			uint32_t x = 0u;
			uint32_t y = 0u;
			nextgeo_shading_unpack_pixel(pixels[offsets[b] + i], x, y);
			const uint32_t tile = (y / 8u) * ((width + 7u) / 8u) + x / 8u;
			const uint32_t local = (y % 8u) * 8u + x % 8u;
			if (tile != current_tile) {
				runs_ok = runs_ok && !finished_tiles.has(tile);
				if (current_tile != UINT32_MAX) {
					finished_tiles.insert(current_tile);
				}
				current_tile = tile;
			} else {
				runs_ok = runs_ok && local > previous_local;
			}
			previous_local = local;
		}
	}
	CHECK(runs_ok);
}

TEST_CASE("[NextGeo][ShadingBins] The compute form rejects materials that can discard or write DEPTH") {
	NextGeoShadingFacts facts;
	CHECK_FALSE(nextgeo_shading_compute_rejects(facts));
	facts.discard = true;
	CHECK(nextgeo_shading_compute_rejects(facts));
	facts = NextGeoShadingFacts();
	facts.alpha_test = true;
	CHECK(nextgeo_shading_compute_rejects(facts));
	facts = NextGeoShadingFacts();
	facts.alpha_antialiasing = true;
	CHECK(nextgeo_shading_compute_rejects(facts));
	facts = NextGeoShadingFacts();
	facts.depth_prepass_alpha = true;
	CHECK(nextgeo_shading_compute_rejects(facts));
	facts = NextGeoShadingFacts();
	facts.writes_depth = true;
	CHECK(nextgeo_shading_compute_rejects(facts));
}

TEST_CASE("[NextGeo][ShadingBins] shading_path: auto keeps the tile quads during the build") {
	CHECK_FALSE(nextgeo_shading_path_wants_compute(NEXTGEO_SHADING_PATH_AUTO));
	CHECK(nextgeo_shading_path_wants_compute(NEXTGEO_SHADING_PATH_COMPUTE_BINS));
	CHECK_FALSE(nextgeo_shading_path_wants_compute(NEXTGEO_SHADING_PATH_TILE_QUADS));
	// The project setting's enum order.
	CHECK_EQ(int(NEXTGEO_SHADING_PATH_AUTO), 0);
	CHECK_EQ(int(NEXTGEO_SHADING_PATH_COMPUTE_BINS), 1);
	CHECK_EQ(int(NEXTGEO_SHADING_PATH_TILE_QUADS), 2);
	CHECK_EQ(String(nextgeo_shading_reason_name(NEXTGEO_SHADING_REASON_NONE)), "NONE");
	CHECK_EQ(String(nextgeo_shading_reason_name(NEXTGEO_SHADING_REASON_NO_STORAGE)), "NO_STORAGE");
	CHECK_EQ(String(nextgeo_shading_reason_name(NEXTGEO_SHADING_REASON_COMPUTE_REJECTED)), "COMPUTE_REJECTED");
}

TEST_CASE("[NextGeo][ShadingBins] The GLSL copies of the layouts match nextgeo_shading_bins.h and NextGeoResolve") {
	const char *bins = shading_bins_inc_shader_glsl;
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_BIN_NONE")), int64_t(NEXTGEO_SHADING_BIN_NONE));
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_GROUP_SIZE")), int64_t(NEXTGEO_SHADING_GROUP_SIZE));
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_DISPATCH_WIDTH")), int64_t(NEXTGEO_SHADING_DISPATCH_WIDTH));
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_BIN_STATE_WORDS")), int64_t(NEXTGEO_SHADING_BIN_STATE_WORDS));
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_BIN_STATE_COUNT")), int64_t(NEXTGEO_SHADING_BIN_STATE_COUNT));
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_BIN_STATE_OFFSET")), int64_t(NEXTGEO_SHADING_BIN_STATE_OFFSET));
	CHECK_EQ(glsl_integer(glsl_constant(bins, "NEXTGEO_SHADING_BIN_STATE_CURSOR")), int64_t(NEXTGEO_SHADING_BIN_STATE_CURSOR));

	// resolve_compute_inc.glsl: the bin state binding, the first image binding and the velocity flag.
	const String compute = String::utf8(resolve_compute_inc_shader_glsl);
	using Resolve = RendererSceneRenderImplementation::NextGeoResolve;
	CHECK(compute.contains(vformat("set = 2, binding = %d, std430) restrict readonly buffer NextGeoShadingBins", Resolve::COMPUTE_SHADING_BIN_STATE_BINDING)));
	CHECK(compute.contains(vformat("set = 2, binding = %d) uniform restrict writeonly image2D nextgeo_out_normal_roughness", Resolve::COMPUTE_IMAGE_BINDING_BASE)));
	CHECK(compute.contains(vformat("set = 2, binding = %d) uniform restrict writeonly image2D nextgeo_out_color", Resolve::COMPUTE_IMAGE_BINDING_BASE)));
	CHECK(compute.contains(vformat("set = 2, binding = %d) uniform restrict writeonly uimage2D nextgeo_out_instance_id", Resolve::COMPUTE_IMAGE_BINDING_BASE + Resolve::COMPUTE_IMAGE_COUNT - 1u)));
	CHECK_EQ(glsl_integer(glsl_constant(resolve_compute_inc_shader_glsl, "NEXTGEO_RESOLVE_COMPUTE_SKIP_VELOCITY").get_slicec('<', 0).strip_edges()), 1);
	CHECK_EQ(Resolve::COMPUTE_FLAG_SKIP_VELOCITY, 1u);

	// The image formats it declares, binding by binding, are the ones the targets are checked against.
	const char *stage_a_images[Resolve::COMPUTE_IMAGE_COUNT] = { "nextgeo_out_normal_roughness", "nextgeo_out_ml_albedo", "nextgeo_out_ml_material", "nextgeo_out_voxel_gi", "nextgeo_out_instance_id" };
	const char *stage_b_images[Resolve::COMPUTE_IMAGE_COUNT] = { "nextgeo_out_color", "nextgeo_out_specular", "nextgeo_out_velocity", nullptr, nullptr };
	for (uint32_t i = 0; i < Resolve::COMPUTE_IMAGE_COUNT; i++) {
		CHECK(compute.contains(glsl_image_declaration(Resolve::COMPUTE_STAGE_A_FORMATS[i], Resolve::COMPUTE_IMAGE_BINDING_BASE + i, stage_a_images[i])));
		if (stage_b_images[i] != nullptr) {
			CHECK(compute.contains(glsl_image_declaration(Resolve::COMPUTE_STAGE_B_FORMATS[i], Resolve::COMPUTE_IMAGE_BINDING_BASE + i, stage_b_images[i])));
		} else {
			CHECK_EQ(Resolve::COMPUTE_STAGE_B_FORMATS[i], RD::DATA_FORMAT_MAX);
		}
	}
}
