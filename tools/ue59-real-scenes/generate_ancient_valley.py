#!/usr/bin/env python3
"""Generate a clean-room, multi-million-triangle desert canyon & ancient monolith ruins
glTF 2.0 showcase scene ("Ancient Valley") with 2K procedural PBR textures for side-by-side
UE 5.9 (Nanite + Lumen HWRT + VSM) vs. GodotNext (NextGeo + NextGI HWRT + VSM) comparison."""
import json
import math
import os
import struct
import sys
from pathlib import Path

for dist_dir in ("/usr/lib/python3/dist-packages", "/usr/local/lib/python3.12/dist-packages"):
    if os.path.isdir(dist_dir) and dist_dir not in sys.path:
        sys.path.append(dist_dir)

import numpy as np
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = Path(os.environ.get("GODOTNEXT_ROOT") or SCRIPT_DIR.parents[1])
TOOLS_DIR = Path(os.environ.get("GODOTNEXT_TOOLS_DIR") or REPO_ROOT / "local/godotnext-tools")
OUT_DIR = TOOLS_DIR / "ue59-real-scenes/ancient_valley"
TEX_DIR = OUT_DIR / "textures"


# ---------------------------------------------------------------------------
# Fast vectorized tileable value/fBM/ridge noise in NumPy
# ---------------------------------------------------------------------------
def _tileable_noise_2d(res: int, freq: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    grid = rng.random((freq, freq), dtype=np.float32)
    u = np.linspace(0.0, float(freq), res, endpoint=False, dtype=np.float32)
    i0 = np.floor(u).astype(np.int32) % freq
    i1 = (i0 + 1) % freq
    f = u - np.floor(u)
    s = f * f * (3.0 - 2.0 * f)
    g00 = grid[i0[:, None], i0[None, :]]
    g10 = grid[i1[:, None], i0[None, :]]
    g01 = grid[i0[:, None], i1[None, :]]
    g11 = grid[i1[:, None], i1[None, :]]
    nx0 = g00 * (1.0 - s[:, None]) + g10 * s[:, None]
    nx1 = g01 * (1.0 - s[:, None]) + g11 * s[:, None]
    return nx0 * (1.0 - s[None, :]) + nx1 * s[None, :]


def fbm_tileable(res: int, base_freq: int, octaves: int, seed: int, ridge: bool = False) -> np.ndarray:
    acc = np.zeros((res, res), dtype=np.float32)
    amp = 0.5
    norm = 0.0
    freq = base_freq
    for o in range(octaves):
        n = _tileable_noise_2d(res, min(freq, res // 2), seed + o * 97)
        if ridge:
            n = 1.0 - np.abs(n * 2.0 - 1.0)
            n = n * n
        acc += amp * n
        norm += amp
        amp *= 0.5
        freq *= 2
    return acc / max(norm, 1e-6)


def height_to_normal_png(height: np.ndarray, strength: float, path: Path):
    gx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * strength
    gy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * strength
    nx = -gx
    ny = -gy
    nz = np.ones_like(height)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.empty((height.shape[0], height.shape[1], 3), dtype=np.uint8)
    rgb[:, :, 0] = np.clip(np.round((nx * inv * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
    rgb[:, :, 1] = np.clip(np.round((ny * inv * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
    rgb[:, :, 2] = np.clip(np.round((nz * inv * 0.5 + 0.5) * 255.0), 128, 255).astype(np.uint8)
    Image.fromarray(rgb, mode="RGB").save(path, compress_level=3)


def save_rgb(arr01: np.ndarray, path: Path):
    u8 = np.clip(np.round(np.clip(arr01, 0.0, 1.0) * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(u8, mode="RGB").save(path, compress_level=3)


def save_mr(rough01: np.ndarray, metal01: np.ndarray, ao01: np.ndarray, path: Path):
    u8 = np.empty((rough01.shape[0], rough01.shape[1], 3), dtype=np.uint8)
    u8[:, :, 0] = np.clip(np.round(np.clip(ao01, 0.0, 1.0) * 255.0), 0, 255).astype(np.uint8)
    u8[:, :, 1] = np.clip(np.round(np.clip(rough01, 0.04, 1.0) * 255.0), 0, 255).astype(np.uint8)
    u8[:, :, 2] = np.clip(np.round(np.clip(metal01, 0.0, 1.0) * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(u8, mode="RGB").save(path, compress_level=3)


def generate_textures(res: int = 1024):
    TEX_DIR.mkdir(parents=True, exist_ok=True)
    y = np.linspace(0.0, 1.0, res, endpoint=False, dtype=np.float32)[:, None]
    x = np.linspace(0.0, 1.0, res, endpoint=False, dtype=np.float32)[None, :]

    # 1. Stratified Navajo red/ochre canyon sandstone
    n1 = fbm_tileable(res, 6, 6, 101)
    n2 = fbm_tileable(res, 16, 5, 202, ridge=True)
    warp = (n1 - 0.5) * 0.08
    strata = (
        0.5
        + 0.24 * np.sin((y + warp) * 2.0 * math.pi * 7.0)
        + 0.16 * np.sin((y + warp * 1.4) * 2.0 * math.pi * 19.0 + 1.1)
        + 0.10 * np.sin((y + warp * 0.7) * 2.0 * math.pi * 41.0 + 2.3)
    )
    strata = np.clip(strata, 0.0, 1.0)
    h_cliff = 0.55 * strata + 0.30 * n2 + 0.15 * n1
    c_rust = np.array([0.52, 0.21, 0.12], dtype=np.float32)
    c_ochre = np.array([0.72, 0.42, 0.24], dtype=np.float32)
    c_buff = np.array([0.78, 0.58, 0.38], dtype=np.float32)
    c_dark = np.array([0.30, 0.14, 0.09], dtype=np.float32)
    alb = c_rust * (1.0 - strata[:, :, None]) + c_ochre * strata[:, :, None]
    alb = alb * (1.0 - 0.35 * (n1[:, :, None] ** 2)) + c_buff * (0.35 * (n1[:, :, None] ** 2))
    crevice = np.clip((0.35 - n2) * 2.2, 0.0, 0.55)[:, :, None]
    alb = alb * (1.0 - crevice) + c_dark * crevice
    save_rgb(alb, TEX_DIR / "sandstone_albedo.png")
    height_to_normal_png(h_cliff, 14.0, TEX_DIR / "sandstone_normal.png")
    save_mr(0.76 + 0.20 * (1.0 - n2), np.zeros_like(h_cliff), 0.72 + 0.28 * h_cliff, TEX_DIR / "sandstone_mr.png")

    # 2. Sun-baked canyon sand & gravel wash
    ns1 = fbm_tileable(res, 8, 6, 303)
    ns2 = fbm_tileable(res, 32, 4, 404, ridge=True)
    ripple = 0.5 + 0.5 * np.sin((x * 18.0 + y * 5.0 + ns1 * 2.4) * 2.0 * math.pi)
    h_sand = 0.35 * ripple + 0.45 * ns1 + 0.20 * ns2
    c_s1 = np.array([0.68, 0.48, 0.31], dtype=np.float32)
    c_s2 = np.array([0.50, 0.31, 0.19], dtype=np.float32)
    c_peb = np.array([0.36, 0.25, 0.18], dtype=np.float32)
    pebble = (ns2 > 0.72).astype(np.float32) * 0.45
    alb_s = c_s2 * (1.0 - h_sand[:, :, None]) + c_s1 * h_sand[:, :, None]
    alb_s = alb_s * (1.0 - pebble[:, :, None]) + c_peb * pebble[:, :, None]
    save_rgb(alb_s, TEX_DIR / "sand_albedo.png")
    height_to_normal_png(h_sand, 9.0, TEX_DIR / "sand_normal.png")
    save_mr(0.82 + 0.15 * (1.0 - ns2), np.zeros_like(h_sand), 0.80 + 0.20 * h_sand, TEX_DIR / "sand_mr.png")

    # 3. Carved ancient dark volcanic basalt temple stone (ashlar blocks + sand dust in joints)
    nb1 = fbm_tileable(res, 8, 6, 505)
    nb2 = fbm_tileable(res, 24, 4, 606, ridge=True)
    # Ashlar block mortar joints
    row = np.floor(y * 8.0)
    col_u = (x * 4.0 + 0.5 * (row % 2.0)) % 1.0
    row_v = (y * 8.0) % 1.0
    edge_u = np.minimum(col_u, 1.0 - col_u)
    edge_v = np.minimum(row_v, 1.0 - row_v)
    joint = np.clip(1.0 - np.minimum(edge_u * 26.0, edge_v * 18.0), 0.0, 1.0)
    # Subtle carved geometric band relief
    band = ((np.abs(row_v - 0.5) < 0.14) & ((np.floor(x * 32.0) % 2.0) == 0.0)).astype(np.float32) * 0.12
    h_basalt = 0.55 * nb1 + 0.25 * nb2 + band - 0.45 * joint
    c_bdark = np.array([0.16, 0.15, 0.15], dtype=np.float32)
    c_bmid = np.array([0.27, 0.25, 0.23], dtype=np.float32)
    c_dust = np.array([0.56, 0.37, 0.24], dtype=np.float32)
    alb_b = c_bdark * (1.0 - nb1[:, :, None]) + c_bmid * nb1[:, :, None]
    dust_mask = np.clip(joint * 0.75 + (1.0 - nb2) * 0.25, 0.0, 0.75)[:, :, None]
    alb_b = alb_b * (1.0 - dust_mask) + c_dust * dust_mask
    save_rgb(alb_b, TEX_DIR / "basalt_albedo.png")
    height_to_normal_png(h_basalt, 16.0, TEX_DIR / "basalt_normal.png")
    save_mr(0.55 + 0.35 * np.clip(joint + (1.0 - nb1) * 0.5, 0.0, 1.0), np.full_like(h_basalt, 0.05), 0.65 + 0.35 * (1.0 - joint), TEX_DIR / "basalt_mr.png")

    # 4. Weathered boulder rock
    nr1 = fbm_tileable(res, 6, 6, 707)
    nr2 = fbm_tileable(res, 18, 5, 808, ridge=True)
    h_rock = 0.45 * nr1 + 0.55 * nr2
    c_r1 = np.array([0.46, 0.25, 0.16], dtype=np.float32)
    c_r2 = np.array([0.65, 0.44, 0.28], dtype=np.float32)
    c_r3 = np.array([0.28, 0.17, 0.12], dtype=np.float32)
    alb_r = c_r1 * (1.0 - h_rock[:, :, None]) + c_r2 * h_rock[:, :, None]
    crack = np.clip((0.32 - nr2) * 3.0, 0.0, 0.65)[:, :, None]
    alb_r = alb_r * (1.0 - crack) + c_r3 * crack
    save_rgb(alb_r, TEX_DIR / "rock_albedo.png")
    height_to_normal_png(h_rock, 16.0, TEX_DIR / "rock_normal.png")
    save_mr(0.70 + 0.25 * (1.0 - nr2), np.zeros_like(h_rock), 0.65 + 0.35 * nr2, TEX_DIR / "rock_mr.png")

    # 5. Aged ceremonial bronze/gold trim
    ng = fbm_tileable(res, 10, 5, 909)
    c_g1 = np.array([0.74, 0.56, 0.26], dtype=np.float32)
    c_g2 = np.array([0.36, 0.29, 0.18], dtype=np.float32)
    alb_g = c_g2 * (1.0 - ng[:, :, None]) + c_g1 * ng[:, :, None]
    save_rgb(alb_g, TEX_DIR / "bronze_albedo.png")
    height_to_normal_png(ng, 6.0, TEX_DIR / "bronze_normal.png")
    save_mr(0.28 + 0.30 * (1.0 - ng), 0.72 + 0.20 * ng, 0.8 + 0.2 * ng, TEX_DIR / "bronze_mr.png")


# ---------------------------------------------------------------------------
# Fast 3D noise for geometry displacement
# ---------------------------------------------------------------------------
def trig_fbm3(x: np.ndarray, y: np.ndarray, z: np.ndarray, octaves: int = 5) -> np.ndarray:
    """Fast smooth deterministic multi-octave 3D noise using irrational wave vectors."""
    dirs = [
        (0.173, 0.291, 0.137, 0.4),
        (-0.241, 0.183, 0.311, 1.7),
        (0.389, -0.277, 0.229, 2.9),
        (-0.523, -0.419, 0.613, 4.1),
        (0.877, 0.733, -0.941, 0.8),
        (-1.431, 1.193, 1.327, 2.2),
        (2.311, -1.973, 2.111, 3.5),
    ]
    out = np.zeros_like(x, dtype=np.float32)
    amp = 0.5
    norm = 0.0
    for i in range(min(octaves, len(dirs))):
        kx, ky, kz, ph = dirs[i]
        v = np.sin(x * kx + y * ky + z * kz + ph) * np.cos(x * kz - y * kx + z * ky - ph * 0.7)
        out += amp * v
        norm += amp
        amp *= 0.52
    return out / max(norm, 1e-6)


def compute_smooth_normals(pos: np.ndarray, idx: np.ndarray) -> np.ndarray:
    p0 = pos[idx[:, 0]]
    p1 = pos[idx[:, 1]]
    p2 = pos[idx[:, 2]]
    fn = np.cross(p1 - p0, p2 - p0)
    nrm = np.zeros_like(pos, dtype=np.float32)
    np.add.at(nrm, idx[:, 0], fn)
    np.add.at(nrm, idx[:, 1], fn)
    np.add.at(nrm, idx[:, 2], fn)
    lens = np.linalg.norm(nrm, axis=1, keepdims=True)
    nrm /= np.maximum(lens, 1e-8)
    return nrm


def grid_indices(ny: int, nx: int, flip: bool = False) -> np.ndarray:
    j = np.arange(ny - 1, dtype=np.uint32)[:, None]
    i = np.arange(nx - 1, dtype=np.uint32)[None, :]
    v00 = j * nx + i
    v10 = v00 + 1
    v01 = v00 + nx
    v11 = v01 + 1
    if not flip:
        t1 = np.stack([v00, v01, v10], axis=-1)
        t2 = np.stack([v10, v01, v11], axis=-1)
    else:
        t1 = np.stack([v00, v10, v01], axis=-1)
        t2 = np.stack([v10, v11, v01], axis=-1)
    return np.concatenate([t1.reshape(-1, 3), t2.reshape(-1, 3)], axis=0)


# ---------------------------------------------------------------------------
# Hero Geometry Builders
# ---------------------------------------------------------------------------
def build_canyon_terrain() -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """High-density canyon floor + rising valley shoulders (520k tris)."""
    nx, nz = 512, 512
    xs = np.linspace(-62.0, 62.0, nx, dtype=np.float32)
    zs = np.linspace(-95.0, 75.0, nz, dtype=np.float32)
    xx, zz = np.meshgrid(xs, zs)

    # Winding canyon centerline x_c(z)
    xc = 4.2 * np.sin(zz * 0.042 + 0.4) + 2.0 * np.cos(zz * 0.085)
    # Narrow canyon at the ancient gate (z ~ -28)
    gate_pinch = np.exp(-((zz + 28.0) / 22.0) ** 2)
    half_width = 16.5 - 4.5 * gate_pinch + 2.0 * np.sin(zz * 0.06)

    dx = np.abs(xx - xc)
    # Smooth dry canyon floor + gentle terraced rock benches outside the wash
    floor_h = (
        0.55 * np.sin(zz * 0.03)
        + 0.35 * trig_fbm3(xx * 0.25, np.zeros_like(xx), zz * 0.25, 4)
        + 0.08 * np.sin(xx * 1.4 + zz * 0.6)
    )
    # Flatten ceremonial approach near portal plaza (z in [-48, -6], |x - xc| < 12)
    plaza_w = np.exp(-((zz + 28.0) / 18.0) ** 2) * np.exp(-((xx - xc) / 12.0) ** 2)
    floor_h = floor_h * (1.0 - 0.85 * plaza_w) + 0.0 * (0.85 * plaza_w)

    # Steep canyon wall rise outside half_width with stratified terraces
    t = np.clip((dx - half_width) / 26.0, 0.0, 1.5)
    base_wall = 34.0 * np.power(t, 1.35)
    # Add geological strata ledges & craggy erosion
    disp = trig_fbm3(xx * 0.14, base_wall * 0.12, zz * 0.14, 6)
    ridge = 1.0 - np.abs(trig_fbm3(xx * 0.22, base_wall * 0.18, zz * 0.22, 4))
    strata_step = 0.85 * np.sin(base_wall * 0.85 + disp * 2.2)
    wall_h = base_wall + (t > 0.01).astype(np.float32) * np.clip(t * 3.0, 0.0, 1.0) * (3.8 * disp + 2.6 * ridge + strata_step)

    # Carve dramatic deep canyon cliff walls higher at sides
    yy = floor_h + wall_h
    pos = np.stack([xx, yy, zz], axis=-1).reshape(-1, 3).astype(np.float32)
    uv = np.stack([xx * 0.09, zz * 0.09], axis=-1).reshape(-1, 2).astype(np.float32)
    idx = grid_indices(nz, nx, flip=False)
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


def build_cliff_massif(side: int, z_start: float, z_end: float, seed: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Overhanging stratified cliff wall with eroded cave/grotto alcove (~260k tris each)."""
    nu, nv = 360, 360
    zs = np.linspace(z_start, z_end, nu, dtype=np.float32)
    ys = np.linspace(-1.5, 42.0, nv, dtype=np.float32)
    zz, yy = np.meshgrid(zs, ys)  # (nv, nu)

    xc = 4.2 * np.sin(zz * 0.042 + 0.4) + 2.0 * np.cos(zz * 0.085)
    gate_pinch = np.exp(-((zz + 28.0) / 22.0) ** 2)
    base_offset = 17.5 - 4.0 * gate_pinch

    # Vertical strata ledges + dramatic overhanging lip + grotto recess on left side (side == -1)
    n_macro = trig_fbm3(zz * 0.11 + seed * 3.1, yy * 0.15, np.full_like(zz, side * 5.0), 6)
    n_detail = trig_fbm3(zz * 0.45 + seed * 7.3, yy * 0.55, np.full_like(zz, side * 11.0), 5)
    strata = 1.6 * np.sin(yy * 0.72 + n_macro * 2.5) + 0.65 * np.sin(yy * 1.95 + n_detail * 1.4)

    # Overhang grotto pocket around z in [-24, 4], y in [0, 12] on the left cliff (side == -1)
    grotto = 0.0
    if side < 0:
        gz = np.exp(-((zz + 10.0) / 14.0) ** 2)
        gy = np.exp(-((yy - 5.2) / 5.5) ** 2)
        overhang_cap = np.exp(-((yy - 14.5) / 4.0) ** 2) * gz
        grotto = 8.5 * gz * gy - 4.2 * overhang_cap

    # Top mesa roll-back so cliff tops smoothly into plateau
    top_roll = np.clip((yy - 32.0) / 10.0, 0.0, 1.0) ** 2 * 14.0
    x_dist = base_offset + 2.8 * n_macro + 0.9 * n_detail + strata + grotto + top_roll
    xx = xc + side * x_dist

    pos = np.stack([xx, yy, zz], axis=-1).reshape(-1, 3).astype(np.float32)
    uv = np.stack([zz * 0.06, yy * 0.06], axis=-1).reshape(-1, 2).astype(np.float32)
    # Face inward toward canyon center
    idx = grid_indices(nv, nu, flip=(side < 0))
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


def build_distant_mesa() -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Monumental distant desert mesa butte framing the horizon behind the portal (~130k tris)."""
    nu, nv = 256, 256
    thetas = np.linspace(0.0, 2.0 * math.pi, nu, endpoint=False, dtype=np.float32)
    vs = np.linspace(0.0, 1.0, nv, dtype=np.float32)
    th, vv = np.meshgrid(thetas, vs)
    # Flat-topped stepped butte profile
    r_base = 52.0 * (1.0 - 0.58 * np.power(vv, 0.75))
    r_base = np.where(vv > 0.88, r_base * (1.0 - (vv - 0.88) / 0.12), r_base)
    cx = r_base * np.cos(th)
    cz = r_base * np.sin(th)
    yy = np.minimum(vv / 0.88, 1.0) * 62.0 - 2.0
    disp = trig_fbm3(cx * 0.07, yy * 0.10, cz * 0.07, 5)
    strata = 2.2 * np.sin(yy * 0.45 + disp * 2.0)
    scale = 1.0 + (0.18 * disp + 0.04 * strata) * (vv < 0.88)
    xx = cx * scale + 6.0
    zz = cz * scale - 135.0
    pos = np.stack([xx, yy, zz], axis=-1).reshape(-1, 3).astype(np.float32)
    uv = np.stack([th / (2.0 * math.pi) * 8.0, yy * 0.05], axis=-1).reshape(-1, 2).astype(np.float32)
    idx = grid_indices(nv, nu, flip=False)
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


def build_displaced_box(sx: float, sy: float, sz: float, subdiv: int = 24, noise_amp: float = 0.08, seed: int = 1, bevel: float = 0.05) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Subdivided, micro-weathered carved architectural stone block (~7k-30k tris)."""
    all_pos, all_uv, all_idx = [], [], []
    v_off = 0
    faces = [
        # (u_axis, v_axis, n_axis, sign)
        (0, 1, 2, 1.0),
        (0, 1, 2, -1.0),
        (2, 1, 0, 1.0),
        (2, 1, 0, -1.0),
        (0, 2, 1, 1.0),
        (0, 2, 1, -1.0),
    ]
    half = np.array([sx * 0.5, sy * 0.5, sz * 0.5], dtype=np.float32)
    t = np.linspace(-1.0, 1.0, subdiv, dtype=np.float32)
    uu, vv = np.meshgrid(t, t)
    for ua, va, na, sgn in faces:
        p = np.zeros((subdiv, subdiv, 3), dtype=np.float32)
        p[:, :, ua] = uu * half[ua]
        p[:, :, va] = vv * half[va]
        p[:, :, na] = sgn * half[na]
        # Subtle bevel near edges
        eu = np.maximum(np.abs(uu) - (1.0 - bevel), 0.0) / max(bevel, 1e-4)
        ev = np.maximum(np.abs(vv) - (1.0 - bevel), 0.0) / max(bevel, 1e-4)
        p[:, :, na] -= sgn * (eu * eu + ev * ev) * min(sx, sy, sz) * 0.025
        # Weathering chips & surface relief
        if noise_amp > 0.0:
            d = trig_fbm3(p[:, :, 0] * 1.1 + seed * 3.7, p[:, :, 1] * 1.1, p[:, :, 2] * 1.1, 4)
            p[:, :, na] += sgn * d * noise_amp
        uv_f = np.stack([(p[:, :, ua] + half[ua]) * 0.25, (p[:, :, va] + half[va]) * 0.25], axis=-1)
        # Ensure outward-facing winding
        e1 = np.zeros(3); e1[ua] = 1.0
        e2 = np.zeros(3); e2[va] = 1.0
        en = np.zeros(3); en[na] = sgn
        flip = float(np.dot(np.cross(e1, e2), en)) < 0.0
        idx_f = grid_indices(subdiv, subdiv, flip=flip) + v_off
        all_pos.append(p.reshape(-1, 3))
        all_uv.append(uv_f.reshape(-1, 2))
        all_idx.append(idx_f)
        v_off += subdiv * subdiv
    pos = np.concatenate(all_pos, axis=0)
    uv = np.concatenate(all_uv, axis=0)
    idx = np.concatenate(all_idx, axis=0)
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


def build_sculpted_boulder(rx: float, ry: float, rz: float, seed: int, nu: int = 96, nv: int = 64) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """High-poly faceted eroded desert sandstone boulder (~12k tris)."""
    th = np.linspace(0.0, 2.0 * math.pi, nu, dtype=np.float32)
    ph = np.linspace(0.02, math.pi - 0.02, nv, dtype=np.float32)
    tt, pp = np.meshgrid(th, ph)
    dx = np.sin(pp) * np.cos(tt)
    dy = np.cos(pp)
    dz = np.sin(pp) * np.sin(tt)
    n1 = trig_fbm3(dx * 2.4 + seed * 5.1, dy * 2.4 + seed * 2.3, dz * 2.4, 5)
    n2 = 1.0 - np.abs(trig_fbm3(dx * 5.2 + seed * 1.7, dy * 5.2, dz * 5.2, 4))
    strata = 0.08 * np.sin(dy * 9.0 + n1 * 2.0)
    r = 1.0 + 0.28 * n1 + 0.14 * (n2 - 0.5) + strata
    # Slightly flatten underside so boulders sit naturally in sand
    y_flat = np.where(dy < -0.35, -0.35 + (dy + 0.35) * 0.35, dy)
    pos = np.stack([dx * r * rx, y_flat * r * ry, dz * r * rz], axis=-1).reshape(-1, 3).astype(np.float32)
    uv = np.stack([tt / (2.0 * math.pi) * 2.0, pp / math.pi * 2.0], axis=-1).reshape(-1, 2).astype(np.float32)
    idx = grid_indices(nv, nu, flip=False)
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


def build_monolith_ring(R: float = 4.2, r: float = 0.65, nu: int = 96, nv: int = 40) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Sculpted ancient stone/bronze portal ring standing inside the sanctuary."""
    u = np.linspace(0.0, 2.0 * math.pi, nu, dtype=np.float32)
    v = np.linspace(0.0, 2.0 * math.pi, nv, dtype=np.float32)
    uu, vv = np.meshgrid(u, v)
    # Octagonal/fluted cross-section modulation
    r_mod = r * (1.0 + 0.08 * np.cos(vv * 8.0) + 0.05 * np.cos(uu * 16.0))
    xx = (R + r_mod * np.cos(vv)) * np.cos(uu)
    yy = (R + r_mod * np.cos(vv)) * np.sin(uu)
    zz = r_mod * np.sin(vv)
    pos = np.stack([xx, yy, zz], axis=-1).reshape(-1, 3).astype(np.float32)
    uv = np.stack([uu / (2.0 * math.pi) * 6.0, vv / (2.0 * math.pi) * 2.0], axis=-1).reshape(-1, 2).astype(np.float32)
    idx = grid_indices(nv, nu, flip=True)
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


def build_fluted_pillar(radius: float, height: float, nu: int = 80, nv: int = 96, seed: int = 11) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Tapered weathered ancient fluted column (~15k tris)."""
    u = np.linspace(0.0, 2.0 * math.pi, nu, dtype=np.float32)
    y = np.linspace(0.0, height, nv, dtype=np.float32)
    uu, yy = np.meshgrid(u, y)
    taper = 1.0 - 0.18 * (yy / height)
    flute = 1.0 - 0.07 * np.maximum(np.cos(uu * 12.0), 0.0)
    band = 0.04 * np.exp(-((yy % 3.0 - 1.5) / 0.18) ** 2)
    wear = 0.04 * trig_fbm3(np.cos(uu) * 2.5 + seed, yy * 0.7, np.sin(uu) * 2.5, 4)
    rr = radius * taper * (flute + band + wear)
    xx = rr * np.cos(uu)
    zz = rr * np.sin(uu)
    pos = np.stack([xx, yy, zz], axis=-1).reshape(-1, 3).astype(np.float32)
    uv = np.stack([uu / (2.0 * math.pi) * 2.0, yy * 0.25], axis=-1).reshape(-1, 2).astype(np.float32)
    idx = grid_indices(nv, nu, flip=False)
    nrm = compute_smooth_normals(pos, idx)
    return pos, nrm, uv, idx


# ---------------------------------------------------------------------------
# glTF 2.0 Binary Assembly
# ---------------------------------------------------------------------------
class GLTFBuilder:
    def __init__(self):
        self.buf = bytearray()
        self.buffer_views = []
        self.accessors = []
        self.meshes = []
        self.nodes = []
        self.images = []
        self.textures = []
        self.materials = []

    def _append_bytes(self, raw: bytes, target: int) -> int:
        while len(self.buf) % 4 != 0:
            self.buf.append(0)
        off = len(self.buf)
        self.buf.extend(raw)
        bv_idx = len(self.buffer_views)
        self.buffer_views.append({
            "buffer": 0,
            "byteOffset": off,
            "byteLength": len(raw),
            "target": target,
        })
        return bv_idx

    def add_mesh(self, name: str, pos: np.ndarray, nrm: np.ndarray, uv: np.ndarray, idx: np.ndarray, mat_idx: int) -> int:
        pos = np.ascontiguousarray(pos, dtype=np.float32)
        nrm = np.ascontiguousarray(nrm, dtype=np.float32)
        uv = np.ascontiguousarray(uv, dtype=np.float32)
        idx = np.ascontiguousarray(idx.reshape(-1), dtype=np.uint32)

        bv_p = self._append_bytes(pos.tobytes(), 34962)
        bv_n = self._append_bytes(nrm.tobytes(), 34962)
        bv_t = self._append_bytes(uv.tobytes(), 34962)
        bv_i = self._append_bytes(idx.tobytes(), 34963)

        acc_p = len(self.accessors)
        self.accessors.append({
            "bufferView": bv_p, "componentType": 5126, "count": len(pos), "type": "VEC3",
            "min": [float(v) for v in pos.min(axis=0)],
            "max": [float(v) for v in pos.max(axis=0)],
        })
        acc_n = len(self.accessors)
        self.accessors.append({"bufferView": bv_n, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        acc_t = len(self.accessors)
        self.accessors.append({"bufferView": bv_t, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        acc_i = len(self.accessors)
        self.accessors.append({"bufferView": bv_i, "componentType": 5125, "count": len(idx), "type": "SCALAR"})

        m_idx = len(self.meshes)
        self.meshes.append({
            "name": name,
            "primitives": [{
                "attributes": {"POSITION": acc_p, "NORMAL": acc_n, "TEXCOORD_0": acc_t},
                "indices": acc_i,
                "material": mat_idx,
            }],
        })
        return m_idx

    def add_texture(self, rel_uri: str) -> int:
        img_i = len(self.images)
        self.images.append({"uri": rel_uri, "mimeType": "image/png"})
        tex_i = len(self.textures)
        self.textures.append({"source": img_i, "sampler": 0})
        return tex_i

    def add_pbr_material(self, name: str, alb: str, nrm: str, mr: str, emissive=None, double_sided: bool = False) -> int:
        m = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": self.add_texture(alb)},
                "metallicRoughnessTexture": {"index": self.add_texture(mr)},
                "metallicFactor": 1.0,
                "roughnessFactor": 1.0,
            },
            "normalTexture": {"index": self.add_texture(nrm)},
            "doubleSided": double_sided,
        }
        if emissive is not None:
            m["emissiveFactor"] = list(emissive)
        idx = len(self.materials)
        self.materials.append(m)
        return idx

    def add_node(self, name: str, mesh_idx: int, translation=(0, 0, 0), rotation=(0, 0, 0, 1), scale=(1, 1, 1)):
        n = {"name": name, "mesh": mesh_idx}
        if translation != (0, 0, 0):
            n["translation"] = [float(x) for x in translation]
        if rotation != (0, 0, 0, 1):
            n["rotation"] = [float(x) for x in rotation]
        if scale != (1, 1, 1):
            n["scale"] = [float(x) for x in scale]
        self.nodes.append(n)

    def write(self, out_dir: Path):
        bin_path = out_dir / "ancient_valley.bin"
        bin_path.write_bytes(self.buf)
        doc = {
            "asset": {"version": "2.0", "generator": "GodotNext AncientValley Clean-Room Generator"},
            "scene": 0,
            "scenes": [{"name": "AncientValley", "nodes": list(range(len(self.nodes)))}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "textures": self.textures,
            "images": self.images,
            "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
            "accessors": self.accessors,
            "bufferViews": self.buffer_views,
            "buffers": [{"uri": "ancient_valley.bin", "byteLength": len(self.buf)}],
        }
        gltf_path = out_dir / "ancient_valley.gltf"
        gltf_path.write_text(json.dumps(doc, separators=(",", ":")))
        return gltf_path


def euler_xyz_to_quat(rx: float, ry: float, rz: float):
    cx, sx = math.cos(rx * 0.5), math.sin(rx * 0.5)
    cy, sy = math.cos(ry * 0.5), math.sin(ry * 0.5)
    cz, sz = math.cos(rz * 0.5), math.sin(rz * 0.5)
    return (
        sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz,
        cx * cy * sz + sx * sy * cz,
        cx * cy * cz - sx * sy * sz,
    )


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print("Generating 1024x1024 procedural PBR texture sets...", flush=True)
    generate_textures(1024)

    gb = GLTFBuilder()
    mat_cliff = gb.add_pbr_material("SandstoneCliff", "textures/sandstone_albedo.png", "textures/sandstone_normal.png", "textures/sandstone_mr.png", double_sided=True)
    mat_sand = gb.add_pbr_material("CanyonSand", "textures/sand_albedo.png", "textures/sand_normal.png", "textures/sand_mr.png")
    mat_basalt = gb.add_pbr_material("AncientBasalt", "textures/basalt_albedo.png", "textures/basalt_normal.png", "textures/basalt_mr.png")
    mat_rock = gb.add_pbr_material("WeatheredRock", "textures/rock_albedo.png", "textures/rock_normal.png", "textures/rock_mr.png")
    mat_bronze = gb.add_pbr_material("AncientBronze", "textures/bronze_albedo.png", "textures/bronze_normal.png", "textures/bronze_mr.png")
    mat_amber_glow = len(gb.materials)
    gb.materials.append({
        "name": "AncientSanctumGlow",
        "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.78, 0.38, 1.0], "metallicFactor": 0.1, "roughnessFactor": 0.25},
        "emissiveFactor": [1.0, 0.65, 0.22],
    })

    total_unique_tris = 0
    total_inst_tris = 0

    def reg_mesh(name, geom, mat):
        nonlocal total_unique_tris
        pos, nrm, uv, idx = geom
        total_unique_tris += len(idx)
        return gb.add_mesh(name, pos, nrm, uv, idx, mat), len(idx)

    # 1. Canyon Terrain & Cliffs (~1.7M tris)
    print("Building canyon terrain, stratified cliffs, and distant mesa...", flush=True)
    m_floor, t_floor = reg_mesh("CanyonTerrain", build_canyon_terrain(), mat_sand)
    gb.add_node("CanyonTerrain", m_floor)
    total_inst_tris += t_floor

    cliff_specs = [
        ("CliffLeftSouth", -1, -5.0, 68.0, 11),
        ("CliffLeftNorthGrotto", -1, -88.0, -3.0, 23),
        ("CliffRightSouth", 1, -5.0, 68.0, 37),
        ("CliffRightNorth", 1, -88.0, -3.0, 53),
    ]
    for cname, side, zs, ze, seed in cliff_specs:
        mc, tc = reg_mesh(cname, build_cliff_massif(side, zs, ze, seed), mat_cliff)
        gb.add_node(cname, mc)
        total_inst_tris += tc

    m_mesa, t_mesa = reg_mesh("DistantMesaButte", build_distant_mesa(), mat_cliff)
    gb.add_node("DistantMesaButte", m_mesa)
    total_inst_tris += t_mesa

    # 2. Monumental Ancient Gate & Stepped Temple Sanctuary (centered near x_c(-28), z=-28)
    print("Building monumental ancient monolith gateway & temple sanctuary...", flush=True)
    xc_gate = 4.2 * math.sin(-28.0 * 0.042 + 0.4) + 2.0 * math.cos(-28.0 * 0.085)

    # Stepped ceremonial plaza tiers
    for tier in range(5):
        w = 24.0 - tier * 2.2
        d = 26.0 - tier * 3.0
        h = 0.72
        m_step, t_step = reg_mesh(f"PlazaTier_{tier}", build_displaced_box(w, h, d, subdiv=20, noise_amp=0.04, seed=100 + tier), mat_basalt)
        gb.add_node(f"PlazaTier_{tier}", m_step, translation=(xc_gate, tier * h + h * 0.5 - 0.1, -28.0 - tier * 1.1))
        total_inst_tris += t_step

    # Twin Colossal Monolith Gate Pylons
    m_pylon, t_pylon = reg_mesh("GatePylon", build_displaced_box(5.6, 22.0, 6.2, subdiv=36, noise_amp=0.14, seed=210, bevel=0.08), mat_basalt)
    for sgn, label in ((-1, "Left"), (1, "Right")):
        gb.add_node(f"GatePylon_{label}", m_pylon, translation=(xc_gate + sgn * 8.2, 11.2, -28.0))
        total_inst_tris += t_pylon

    # Monumental Lintel Beam & Crown Cap bridging the pylons
    m_lintel, t_lintel = reg_mesh("GateLintel", build_displaced_box(23.5, 3.6, 5.2, subdiv=32, noise_amp=0.12, seed=220, bevel=0.06), mat_basalt)
    gb.add_node("GateLintelMain", m_lintel, translation=(xc_gate, 21.8, -28.0))
    total_inst_tris += t_lintel

    m_crown, t_crown = reg_mesh("GateCrown", build_displaced_box(16.0, 2.2, 3.6, subdiv=24, noise_amp=0.08, seed=225), mat_bronze)
    gb.add_node("GateCrownBronze", m_crown, translation=(xc_gate, 24.4, -28.0))
    total_inst_tris += t_crown

    # Sanctuary Colonnade (Fluted Ancient Columns along approach & inner temple)
    m_pillar, t_pillar = reg_mesh("FlutedPillar", build_fluted_pillar(0.95, 11.5, nu=80, nv=88, seed=310), mat_basalt)
    m_cap, t_cap = reg_mesh("PillarCap", build_displaced_box(2.4, 0.9, 2.4, subdiv=14, noise_amp=0.04, seed=315), mat_bronze)
    for zi, zpos in enumerate([-8.0, -16.0, -37.0, -45.0, -53.0]):
        xc_z = 4.2 * math.sin(zpos * 0.042 + 0.4) + 2.0 * math.cos(zpos * 0.085)
        for sgn in (-1, 1):
            px = xc_z + sgn * (9.2 if zpos > -25 else 8.4)
            py = 0.0 if zpos > -20 else 3.1
            gb.add_node(f"Col_{zi}_{sgn}", m_pillar, translation=(px, py, zpos))
            gb.add_node(f"Cap_{zi}_{sgn}", m_cap, translation=(px, py + 11.6, zpos))
            total_inst_tris += t_pillar + t_cap

    # Central Ancient Portal Ring + Sanctum Glowing Core
    m_ring, t_ring = reg_mesh("AncientPortalRing", build_monolith_ring(4.8, 0.72, nu=112, nv=48), mat_bronze)
    gb.add_node("AncientPortalRing", m_ring, translation=(xc_gate, 10.2, -41.0))
    total_inst_tris += t_ring

    m_core, t_core = reg_mesh("SanctumCore", build_monolith_ring(3.6, 0.22, nu=80, nv=24), mat_amber_glow)
    gb.add_node("SanctumCoreGlow", m_core, translation=(xc_gate, 10.2, -41.0))
    total_inst_tris += t_core

    # Fallen broken stone columns & lintel ruins along the canyon approach
    m_fallen, t_fallen = reg_mesh("FallenPillar", build_fluted_pillar(1.05, 6.2, nu=64, nv=56, seed=330), mat_basalt)
    fallen_specs = [
        (xc_gate - 5.2, 0.7, 6.5, (1.42, 0.35, 0.2)),
        (xc_gate + 6.8, 0.6, -2.5, (1.50, -0.55, 0.1)),
        (xc_gate - 8.5, 0.8, -14.0, (1.48, 0.85, -0.15)),
        (xc_gate + 4.5, 0.5, 18.0, (1.52, -0.25, 0.05)),
    ]
    for fi, (fx, fy, fz, frot) in enumerate(fallen_specs):
        gb.add_node(f"FallenPillar_{fi}", m_fallen, translation=(fx, fy, fz), rotation=euler_xyz_to_quat(*frot))
        total_inst_tris += t_fallen

    # 3. Eroded Sandstone Boulders & Cliff Talus Piles (~1M instanced tris)
    print("Scattering sculpted canyon boulders & talus scree...", flush=True)
    boulder_meshes = []
    for bi in range(8):
        mb, tb = reg_mesh(f"BoulderType_{bi}", build_sculpted_boulder(2.4 + (bi % 3) * 0.7, 1.5 + (bi % 2) * 0.5, 2.1 + ((bi + 1) % 3) * 0.6, seed=400 + bi * 19, nu=96, nv=64), mat_rock)
        boulder_meshes.append((mb, tb))

    rng = np.random.default_rng(20261126)
    for bi in range(84):
        side = -1 if (bi % 2 == 0) else 1
        zpos = float(rng.uniform(-68.0, 52.0))
        xc_z = 4.2 * math.sin(zpos * 0.042 + 0.4) + 2.0 * math.cos(zpos * 0.085)
        gate_pinch = math.exp(-((zpos + 28.0) / 22.0) ** 2)
        hw = 15.0 - 4.0 * gate_pinch
        dist = hw - float(rng.uniform(0.5, 4.8))
        # Keep center sightline clear
        if abs(dist) < 5.5:
            dist = 6.0 + float(rng.uniform(0.0, 3.0))
        xpos = xc_z + side * dist
        sc = float(rng.uniform(0.55, 2.35))
        ypos = float(rng.uniform(-0.2, 0.6)) + max(0.0, (dist - hw + 2.0) * 0.45)
        rot = euler_xyz_to_quat(float(rng.uniform(0, 6.28)), float(rng.uniform(0, 6.28)), float(rng.uniform(0, 6.28)))
        mb, tb = boulder_meshes[bi % len(boulder_meshes)]
        gb.add_node(f"Boulder_{bi:02d}", mb, translation=(xpos, ypos, zpos), rotation=rot, scale=(sc, sc * float(rng.uniform(0.75, 1.25)), sc))
        total_inst_tris += tb

    out_gltf = gb.write(OUT_DIR)
    meta = {
        "scene": "ancient_valley",
        "gltf": str(out_gltf),
        "unique_tris": total_unique_tris,
        "instanced_tris": total_inst_tris,
        "meshes": len(gb.meshes),
        "nodes": len(gb.nodes),
        "materials": len(gb.materials),
        "xc_gate": xc_gate,
    }
    (OUT_DIR / "meta.json").write_text(json.dumps(meta, indent=2))
    print(f"Wrote {out_gltf}: unique_tris={total_unique_tris:,} instanced_tris={total_inst_tris:,} meshes={len(gb.meshes)} nodes={len(gb.nodes)}", flush=True)


if __name__ == "__main__":
    main()
