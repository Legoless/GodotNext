#!/usr/bin/env python3
"""Generate GodotNext blue-noise tables (#54 plan §8.6, #142 plan §21.3 M3).

Void-and-cluster ordering (Ulichney) on periodic grids:
1. 64x64 RG8 spatial blue-noise tile for NextGI (#54):
   servers/rendering/renderer_rd/environment/nextgi_blue_noise.h
2. 64x64x32 RG8 spatio-temporal blue-noise volume for MegaLights (#142):
   servers/rendering/renderer_rd/effects/megalights_stbn.h

Original implementation; no third-party noise textures (clean-room rule, plan §19).

    python3 godotnext/tools/blue_noise/generate.py [--spatial-only | --stbn-only]
"""

import argparse
import math
import os
import random

SIZE = 64
SIGMA = 1.5
SEEDS = (1, 2)

STBN_WIDTH = 64
STBN_HEIGHT = 64
STBN_DEPTH = 32
STBN_SIGMA_SPATIAL = 1.5
STBN_SIGMA_TEMPORAL = 1.0
STBN_SEEDS = (14201, 14202)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "..", "servers", "rendering", "renderer_rd", "environment", "nextgi_blue_noise.h")
OUT_STBN = os.path.join(HERE, "..", "..", "..", "servers", "rendering", "renderer_rd", "effects", "megalights_stbn.h")


def _license_header(filename: str) -> list[str]:
    pad = " " * max(1, 69 - len(filename))
    return [
        "/**************************************************************************/",
        f"/*  {filename}{pad}*/",
        "/**************************************************************************/",
        "/*                         This file is part of:                          */",
        "/*                             GODOT ENGINE                               */",
        "/*                        https://godotengine.org                         */",
        "/**************************************************************************/",
        "/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */",
        "/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */",
        "/*                                                                        */",
        "/* Permission is hereby granted, free of charge, to any person obtaining  */",
        "/* a copy of this software and associated documentation files (the        */",
        '/* "Software"), to deal in the Software without restriction, including    */',
        "/* without limitation the rights to use, copy, modify, merge, publish,    */",
        "/* distribute, sublicense, and/or sell copies of the Software, and to     */",
        "/* permit persons to whom the Software is furnished to do so, subject to  */",
        "/* the following conditions:                                              */",
        "/*                                                                        */",
        "/* The above copyright notice and this permission notice shall be         */",
        "/* included in all copies or substantial portions of the Software.        */",
        "/*                                                                        */",
        '/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */',
        "/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */",
        "/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */",
        "/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */",
        "/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */",
        "/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */",
        "/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */",
        "/**************************************************************************/",
        "",
    ]


def kernel():
    import numpy as np

    coords = np.arange(SIZE, dtype=np.float64)
    dx = np.abs(coords[:, None] - coords[None, :])
    dx = np.minimum(dx, SIZE - dx)
    dy = dx.T
    return np.exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA))


def rank_tile(seed: int):
    import numpy as np

    rng = np.random.default_rng(seed)
    field = np.zeros((SIZE, SIZE), dtype=np.float64)
    ranks = np.full((SIZE, SIZE), -1, dtype=np.int32)
    kernel_weights = kernel()
    for step in range(SIZE * SIZE):
        # Largest void: the lowest energy on the periodic grid, ties broken by the rng.
        noise = rng.random((SIZE, SIZE))
        score = field + noise * 1e-6
        y, x = np.unravel_index(np.argmin(score), score.shape)
        ranks[y, x] = step
        # Add this point's periodic Gaussian contribution.
        field += np.roll(kernel_weights, (y, x), axis=(0, 1))
    return ranks


def rank_stbn_volume(seed: int) -> list[list[list[int]]]:
    """Per-slice stratified 3D periodic void-and-cluster (64x64x32), pure stdlib Python.

    Assigns ranks 0..(W*H - 1) across all D slices simultaneously while accumulating a
    separable periodic 3D Gaussian energy kernel (spatial sigma=1.5, temporal sigma=1.0).
    Uses per-row minimum tracking so every void lookup is O(H) instead of O(W*H).
    Guarantees:
      - Every 2D slice t has an exact uniform marginal distribution (every rank once).
      - Every 2D slice t has strong spatial blue-noise suppression of low frequencies.
      - Every pixel (x, y) across t=0..31 has strong temporal blue-noise anti-correlation.
    """
    rng = random.Random(seed)
    w, h, d = STBN_WIDTH, STBN_HEIGHT, STBN_DEPTH
    inf = float("inf")

    # Compact truncated periodic 3D Gaussian kernel (support radius covers >= 3 sigma).
    rx, ry, rt = 4, 4, 3
    inv_2ss = 1.0 / (2.0 * STBN_SIGMA_SPATIAL * STBN_SIGMA_SPATIAL)
    inv_2st = 1.0 / (2.0 * STBN_SIGMA_TEMPORAL * STBN_SIGMA_TEMPORAL)
    gt = [math.exp(-(dt * dt) * inv_2st) for dt in range(-rt, rt + 1)]
    gy = [math.exp(-(dy * dy) * inv_2ss) for dy in range(-ry, ry + 1)]
    gx = [math.exp(-(dx * dx) * inv_2ss) for dx in range(-rx, rx + 1)]
    k_rows = [[gt_v * gy_v * gx_v for gx_v in gx] for gt_v in gt for gy_v in gy]

    # Precompute periodic neighbor lists.
    t_off = [tuple((t + dt) % d for dt in range(-rt, rt + 1)) for t in range(d)]
    y_off = [tuple((y + dy) % h for dy in range(-ry, ry + 1)) for y in range(h)]
    x_off = [tuple((x + dx) % w for dx in range(-rx, rx + 1)) for x in range(w)]
    num_t = 2 * rt + 1
    num_y = 2 * ry + 1
    num_x = 2 * rx + 1

    # Energy field: field[t][y][x]; occupied cells are set to inf.
    field = [[[rng.random() * 1e-6 for _ in range(w)] for _ in range(h)] for _ in range(d)]
    ranks = [[[-1] * w for _ in range(h)] for _ in range(d)]

    # Per-row cached (min_val, min_x) so finding the slice minimum checks only 64 rows.
    row_min = [[0.0] * h for _ in range(d)]
    row_arg = [[0] * h for _ in range(d)]
    for t in range(d):
        ft = field[t]
        rm = row_min[t]
        ra = row_arg[t]
        for y in range(h):
            row = ft[y]
            mv = min(row)
            rm[y] = mv
            ra[y] = row.index(mv)

    slice_order = list(range(d))
    for rank in range(w * h):
        rng.shuffle(slice_order)
        for t in slice_order:
            rm = row_min[t]
            best_v = min(rm)
            best_y = rm.index(best_v)
            best_x = row_arg[t][best_y]

            ranks[t][best_y][best_x] = rank
            field[t][best_y][best_x] = inf

            # Scatter 3D periodic Gaussian kernel around (t, best_y, best_x).
            ts = t_off[t]
            ys = y_off[best_y]
            xs = x_off[best_x]
            k_idx = 0
            for it in range(num_t):
                nt = ts[it]
                ft = field[nt]
                nt_rm = row_min[nt]
                nt_ra = row_arg[nt]
                for iy in range(num_y):
                    ny = ys[iy]
                    row = ft[ny]
                    kr = k_rows[k_idx]
                    k_idx += 1
                    cur_arg = nt_ra[ny]
                    recompute = False
                    for ix in range(num_x):
                        nx = xs[ix]
                        v = row[nx]
                        if v < inf:
                            row[nx] = v + kr[ix]
                        if nx == cur_arg:
                            recompute = True
                    if recompute or (nt == t and ny == best_y):
                        mv = min(row)
                        nt_rm[ny] = mv
                        nt_ra[ny] = row.index(mv)

    return ranks


def write_spatial() -> None:
    import numpy as np

    tiles = []
    for seed in SEEDS:
        ranks = rank_tile(seed)
        values = np.floor(ranks * 256.0 / float(SIZE * SIZE)).astype(np.uint8)
        tiles.append(values)
    data = np.stack(tiles, axis=-1).reshape(-1)  # 64*64*2, channel-interleaved.

    lines = _license_header("nextgi_blue_noise.h")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "core/typedefs.h"')
    lines.append("")
    lines.append("// Generated by godotnext/tools/blue_noise/generate.py; do not edit by hand.")
    lines.append("// 64x64, RG8: two independent void-and-cluster orderings (sigma 1.5).")
    lines.append(f"static const uint8_t nextgi_blue_noise_tile[{data.size}] = {{")
    for i in range(0, data.size, 16):
        chunk = ", ".join(str(int(v)) for v in data[i : i + 16])
        lines.append("\t" + chunk + ",")
    lines.append("};")
    lines.append("")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote", os.path.relpath(OUT))


def write_stbn() -> None:
    w, h, d = STBN_WIDTH, STBN_HEIGHT, STBN_DEPTH
    scale = 256.0 / float(w * h)
    channels = []
    for seed in STBN_SEEDS:
        ranks = rank_stbn_volume(seed)
        vals = [[[int(ranks[t][y][x] * scale) for x in range(w)] for y in range(h)] for t in range(d)]
        channels.append(vals)

    # Sanity-check statistical properties before writing.
    n_vox = float(d * h * w)
    for ch in range(2):
        c = channels[ch]
        var = 0.0
        sx = 0.0
        sy = 0.0
        st = 0.0
        for t in range(d):
            ct = c[t]
            ct1 = c[(t + 1) % d]
            for y in range(h):
                row = ct[y]
                row_y1 = ct[(y + 1) % h]
                row_t1 = ct1[y]
                for x in range(w):
                    v = row[x] / 255.0 - 0.5
                    var += v * v
                    sx += v * (row[(x + 1) % w] / 255.0 - 0.5)
                    sy += v * (row_y1[x] / 255.0 - 0.5)
                    st += v * (row_t1[x] / 255.0 - 0.5)
        per_px_means = [
            sum(c[t][y][x] for t in range(d)) / (255.0 * d) for y in range(h) for x in range(w)
        ]
        mean_avg = sum(per_px_means) / len(per_px_means)
        mean_std = math.sqrt(sum((m - mean_avg) ** 2 for m in per_px_means) / len(per_px_means))
        print(
            f"STBN ch{ch}: spatial_r=({sx / var:.4f}, {sy / var:.4f}), "
            f"temporal_r={st / var:.4f}, temporal_mean_std={mean_std:.4f} (n={int(n_vox)})"
        )

    # Flatten (32, 64, 64, 2) -> slice-major, row-major, RG8 interleaved.
    data = []
    c0, c1 = channels[0], channels[1]
    for t in range(d):
        c0t, c1t = c0[t], c1[t]
        for y in range(h):
            r0, r1 = c0t[y], c1t[y]
            for x in range(w):
                data.append(r0[x])
                data.append(r1[x])

    lines = _license_header("megalights_stbn.h")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "core/typedefs.h"')
    lines.append("")
    lines.append("// Generated by godotnext/tools/blue_noise/generate.py; do not edit by hand.")
    lines.append("// 64x64x32, RG8: two independent spatio-temporal void-and-cluster tables")
    lines.append("// (spatial sigma 1.5, temporal sigma 1.0, exact per-slice uniform marginal).")
    lines.append(f"static constexpr uint32_t MEGALIGHTS_STBN_WIDTH = {w}u;")
    lines.append(f"static constexpr uint32_t MEGALIGHTS_STBN_HEIGHT = {h}u;")
    lines.append(f"static constexpr uint32_t MEGALIGHTS_STBN_DEPTH = {d}u;")
    lines.append("static constexpr uint32_t MEGALIGHTS_STBN_CHANNELS = 2u;")
    lines.append(f"static const uint8_t megalights_stbn_data[{len(data)}] = {{")
    for i in range(0, len(data), 32):
        chunk = ",".join(str(v) for v in data[i : i + 32])
        lines.append("\t" + chunk + ",")
    lines.append("};")
    lines.append("")

    with open(OUT_STBN, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote", os.path.relpath(OUT_STBN))


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate GodotNext blue-noise headers.")
    parser.add_argument("--spatial-only", action="store_true", help="Generate only nextgi_blue_noise.h")
    parser.add_argument("--stbn-only", action="store_true", help="Generate only megalights_stbn.h")
    args = parser.parse_args()

    if not args.stbn_only:
        write_spatial()
    if not args.spatial_only:
        write_stbn()


if __name__ == "__main__":
    main()
