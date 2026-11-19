/**************************************************************************/
/*  nextgeo_shading_bins.h                                                */
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

#include "core/templates/local_vector.h"
#include "core/typedefs.h"

// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7, GN-11, N3).
//
// The CPU side of the material shading bins. After visibility export, shading_binning.glsl reads the
// R32 visibility ID of every pixel, finds the pixel's bucket in the bucket table and the bucket's
// shading bin in the per-bucket bin words, counts the bin's pixels per 8x8 tile in shared memory and then globally, takes an
// exclusive prefix sum over the bins and scatters packed 16+16-bit pixel coordinates into one list,
// so each bin's pixels are contiguous and each tile's pixels of a bin stay together. Each bin then
// shades through one indirect dispatch of its material's Stage A or Stage B resolve compiled as compute
// (nextgeo/resolve_compute.glsl), 64 pixels per workgroup. shaders/nextgeo/shading_bins_inc.glsl holds
// the same constants and layouts; test_nextgeo_shading_bins.cpp pins them and checks the reference
// below. The v2.4 8x8 tile quads (resolve_classify.glsl) stay for the materials the compute form
// rejects, for views whose targets it cannot write, and as the A/B baseline.
namespace RendererRD {

// rendering/nextgeo/shading_path. Auto keeps tile quads through the build; GN-11 decides compute bins per
// GPU class in PQ (§22) and the choice is recorded in Appendix A.
enum NextGeoShadingPath : uint32_t {
	NEXTGEO_SHADING_PATH_AUTO,
	NEXTGEO_SHADING_PATH_COMPUTE_BINS,
	NEXTGEO_SHADING_PATH_TILE_QUADS,
	NEXTGEO_SHADING_PATH_MAX,
};

inline bool nextgeo_shading_path_wants_compute(NextGeoShadingPath p_path) {
	return p_path == NEXTGEO_SHADING_PATH_COMPUTE_BINS;
}

// Why a stage, or a material within it, keeps the tile quads.
enum NextGeoShadingReason : uint32_t {
	NEXTGEO_SHADING_REASON_NONE, // Shaded by compute bins.
	NEXTGEO_SHADING_REASON_SETTING, // shading_path is tile_quads, or auto during the build.
	NEXTGEO_SHADING_REASON_NO_STORAGE, // An output target of the stage cannot be written with imageStore.
	NEXTGEO_SHADING_REASON_VRS, // Variable-rate shading: compute shades every pixel.
	NEXTGEO_SHADING_REASON_COMPUTE_REJECTED, // The material's compute form is rejected (see below).
	NEXTGEO_SHADING_REASON_MAX,
};

inline const char *nextgeo_shading_reason_name(NextGeoShadingReason p_reason) {
	switch (p_reason) {
		case NEXTGEO_SHADING_REASON_NONE:
			return "NONE";
		case NEXTGEO_SHADING_REASON_SETTING:
			return "SETTING";
		case NEXTGEO_SHADING_REASON_NO_STORAGE:
			return "NO_STORAGE";
		case NEXTGEO_SHADING_REASON_VRS:
			return "VRS";
		case NEXTGEO_SHADING_REASON_COMPUTE_REJECTED:
			return "COMPUTE_REJECTED";
		default:
			return "UNKNOWN";
	}
}

// What decides whether a material has a compute form: ShaderData's usage flags.
struct NextGeoShadingFacts {
	bool discard = false; // discard in fragment(), light() or a function they call
	bool alpha_test = false; // ALPHA_SCISSOR_THRESHOLD or ALPHA_HASH_SCALE
	bool alpha_antialiasing = false; // ALPHA_ANTIALIASING_EDGE or ALPHA_TEXTURE_COORDINATE
	bool depth_prepass_alpha = false; // render_mode depth_prepass_alpha
	bool writes_depth = false; // DEPTH (gl_FragDepth); such materials already keep the legacy path
};

// A compute shader has no discard and no depth output, so the compute form rejects every material whose
// Stage A/B code can discard a pixel or writes DEPTH; those keep the tile quads in the same frame. The
// visibility buffer has already decided a masked material's coverage (#155's programmable raster), so the
// tile quads only repeat the same test.
inline bool nextgeo_shading_compute_rejects(const NextGeoShadingFacts &p_facts) {
	return p_facts.discard || p_facts.alpha_test || p_facts.alpha_antialiasing || p_facts.depth_prepass_alpha || p_facts.writes_depth;
}

// One shading-bin word per bucket, made by NextGeoResolve::bin() each frame: the bucket's own index when
// its material shades in compute, or NEXTGEO_SHADING_BIN_NONE (tile quads). A pixel's bucket comes from
// the bucket table the tile quads already read, so the binning adds no per-(instance, surface) table.
// The bin key is the bucket key: (material, shader version, shading specialization). A material's
// parameters bind per dispatch, so a bin holds one material, as #155's programmable raster bins do,
// and bin i is bucket i.
static constexpr uint32_t NEXTGEO_SHADING_BIN_NONE = 0xFFFFFFFFu;
// Pixels per shading workgroup, and the width the indirect dispatches fold at (the portable limit of a
// dispatch dimension is 65535 workgroups; a full-screen bin at 3840x2160 needs 129600).
static constexpr uint32_t NEXTGEO_SHADING_GROUP_SIZE = 64u;
static constexpr uint32_t NEXTGEO_SHADING_DISPATCH_WIDTH = 32768u;
static constexpr uint32_t NEXTGEO_SHADING_TILE_SIZE = 8u;
// The bin state buffer: per bin, uvec4 (count, offset, scatter cursor, unused). The bin-count buffer
// #157's stats and visualization read.
static constexpr uint32_t NEXTGEO_SHADING_BIN_STATE_WORDS = 4u;
static constexpr uint32_t NEXTGEO_SHADING_BIN_STATE_COUNT = 0u;
static constexpr uint32_t NEXTGEO_SHADING_BIN_STATE_OFFSET = 1u;
static constexpr uint32_t NEXTGEO_SHADING_BIN_STATE_CURSOR = 2u;
// The indirect-arguments buffer: per bin, uvec4 (x, y, z, unused) workgroups.
static constexpr uint32_t NEXTGEO_SHADING_ARGS_WORDS = 4u;
// Pixel coordinates are packed 16+16 bits, so the internal resolution is capped at 65535 per axis.
static constexpr uint32_t NEXTGEO_SHADING_MAX_EXTENT = 0xFFFFu;

inline uint32_t nextgeo_shading_pack_pixel(uint32_t p_x, uint32_t p_y) {
	return (p_x & 0xFFFFu) | ((p_y & 0xFFFFu) << 16u);
}

inline void nextgeo_shading_unpack_pixel(uint32_t p_packed, uint32_t &r_x, uint32_t &r_y) {
	r_x = p_packed & 0xFFFFu;
	r_y = p_packed >> 16u;
}

// The workgroups of a bin of p_count pixels: ceil(count / 64), folded over y = ceil(groups / 32768) rows
// of x = ceil(groups / y), so a folded dispatch runs fewer than y workgroups past the bin. An empty bin
// dispatches nothing: (0, 0, 1).
inline void nextgeo_shading_dispatch_args(uint32_t p_count, uint32_t r_args[3]) {
	const uint32_t groups = (p_count + NEXTGEO_SHADING_GROUP_SIZE - 1u) / NEXTGEO_SHADING_GROUP_SIZE;
	const uint32_t rows = (groups + NEXTGEO_SHADING_DISPATCH_WIDTH - 1u) / NEXTGEO_SHADING_DISPATCH_WIDTH;
	r_args[0] = rows == 0u ? 0u : (groups + rows - 1u) / rows;
	r_args[1] = rows;
	r_args[2] = 1u;
}

// The bin slot an invocation shades, in a dispatch p_width (its x) workgroups wide; a slot at or past the
// bin's count shades nothing (the tail of the last workgroup, and the folded dispatch's last row). The
// compute form takes p_width from nextgeo_shading_dispatch_args() of the bin's count.
inline uint32_t nextgeo_shading_slot(uint32_t p_group_x, uint32_t p_group_y, uint32_t p_width, uint32_t p_local) {
	return (p_group_y * p_width + p_group_x) * NEXTGEO_SHADING_GROUP_SIZE + p_local;
}

// The CPU reference of shading_binning.glsl: from each pixel's bin (row-major, NEXTGEO_SHADING_BIN_NONE
// for pixels no bin shades), each bin's count, exclusive prefix offset and indirect arguments, and the
// packed pixel list. Tiles are visited in raster order and a tile's pixels of a bin in raster order
// within the tile; on the GPU tiles take their places in a bin in atomic order, so only each tile's run
// and the set of pixels per bin are fixed.
struct NextGeoShadingBinsReference {
	LocalVector<uint32_t> counts;
	LocalVector<uint32_t> offsets;
	LocalVector<uint32_t> args; // NEXTGEO_SHADING_ARGS_WORDS per bin
	LocalVector<uint32_t> pixels;
};

inline void nextgeo_shading_bins_build(const uint32_t *p_pixel_bins, uint32_t p_width, uint32_t p_height, uint32_t p_bin_count, NextGeoShadingBinsReference &r_bins) {
	r_bins.counts.resize(p_bin_count);
	r_bins.offsets.resize(p_bin_count);
	r_bins.args.resize(p_bin_count * NEXTGEO_SHADING_ARGS_WORDS);
	for (uint32_t b = 0; b < p_bin_count; b++) {
		r_bins.counts[b] = 0u;
	}

	// Count (per tile, then globally).
	for (uint32_t i = 0; i < p_width * p_height; i++) {
		const uint32_t bin = p_pixel_bins[i];
		if (bin < p_bin_count) {
			r_bins.counts[bin]++;
		}
	}

	// Exclusive prefix sum and the indirect arguments.
	uint32_t running = 0u;
	for (uint32_t b = 0; b < p_bin_count; b++) {
		r_bins.offsets[b] = running;
		running += r_bins.counts[b];
		uint32_t dispatch[3];
		nextgeo_shading_dispatch_args(r_bins.counts[b], dispatch);
		r_bins.args[b * NEXTGEO_SHADING_ARGS_WORDS + 0u] = dispatch[0];
		r_bins.args[b * NEXTGEO_SHADING_ARGS_WORDS + 1u] = dispatch[1];
		r_bins.args[b * NEXTGEO_SHADING_ARGS_WORDS + 2u] = dispatch[2];
		r_bins.args[b * NEXTGEO_SHADING_ARGS_WORDS + 3u] = 0u;
	}

	// Scatter: tile by tile, each tile's pixels of a bin as one run.
	r_bins.pixels.resize(running);
	LocalVector<uint32_t> cursors;
	cursors.resize(p_bin_count);
	for (uint32_t b = 0; b < p_bin_count; b++) {
		cursors[b] = 0u;
	}
	const uint32_t tiles_x = (p_width + NEXTGEO_SHADING_TILE_SIZE - 1u) / NEXTGEO_SHADING_TILE_SIZE;
	const uint32_t tiles_y = (p_height + NEXTGEO_SHADING_TILE_SIZE - 1u) / NEXTGEO_SHADING_TILE_SIZE;
	for (uint32_t ty = 0; ty < tiles_y; ty++) {
		for (uint32_t tx = 0; tx < tiles_x; tx++) {
			for (uint32_t ly = 0; ly < NEXTGEO_SHADING_TILE_SIZE; ly++) {
				for (uint32_t lx = 0; lx < NEXTGEO_SHADING_TILE_SIZE; lx++) {
					const uint32_t x = tx * NEXTGEO_SHADING_TILE_SIZE + lx;
					const uint32_t y = ty * NEXTGEO_SHADING_TILE_SIZE + ly;
					if (x >= p_width || y >= p_height) {
						continue;
					}
					const uint32_t bin = p_pixel_bins[y * p_width + x];
					if (bin >= p_bin_count) {
						continue;
					}
					r_bins.pixels[r_bins.offsets[bin] + cursors[bin]] = nextgeo_shading_pack_pixel(x, y);
					cursors[bin]++;
				}
			}
		}
	}
}

} // namespace RendererRD
