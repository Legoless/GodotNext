#!/usr/bin/env python3
"""Compute visual diff metrics (FLIP, MAE, luminance) + side-by-side PNG sheets +
interactive HTML comparison viewer (wipe slider, side-by-side, blink, FLIP heatmap)
for UE 5.9 (Nanite + Lumen HWRT + VSM) vs. GodotNext (NextGeo + NextGI HWRT + VSM)
across all 5 scenes x 3 camera angles, and copy it to $GODOTNEXT_DROP_DIR/godotnext-ue59-comparison-v3/."""
import csv
import json
import math
import os
import shutil
import statistics
import sys
from pathlib import Path

for dist_dir in ("/usr/lib/python3/dist-packages", "/usr/local/lib/python3.12/dist-packages"):
    if os.path.isdir(dist_dir) and dist_dir not in sys.path:
        sys.path.append(dist_dir)

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = Path(os.environ.get("GODOTNEXT_ROOT") or SCRIPT_DIR.parents[1])
TOOLS_DIR = Path(os.environ.get("GODOTNEXT_TOOLS_DIR") or REPO_ROOT / "local/godotnext-tools")
sys.path.insert(0, str(REPO_ROOT / "godotnext/testbed/harness/metrics"))

import numpy as np
from PIL import Image, ImageDraw, ImageFont
import flip_evaluator
from godotnext_metrics.ue_bar import run_statistic
from godotnext_metrics.ue_csv import read_csv_profile, group_ms

REAL_ROOT = TOOLS_DIR / "ue59-real-scenes"
COMP_ROOT = REAL_ROOT / "comparison_v3"
# The page folder is replaced on every build, so GODOTNEXT_DROP_DIR names the folder it goes in.
DROP_DIR = Path(os.environ.get("GODOTNEXT_DROP_DIR") or REPO_ROOT / "local/comparison-page") / "godotnext-ue59-comparison-v3"

UE_GROUPS = {
    "whole_frame_gpu": ["GPUTime"],
    "lumen_total": [
        "GPU/RayTracingScene",
        "GPU/LumenSceneUpdate",
        "GPU/UpdateLumenSceneBuffers",
        "GPU/LumenSceneLighting",
        "GPU/LumenScreenProbeGather",
        "GPU/LumenReflections",
    ],
    "nanite_visbuffer": ["GPU/NaniteVisBuffer"],
    "nanite_basepass": ["GPU/NaniteBasePass"],
    "vsm_total": ["GPU/ShadowDepths", "GPU/ShadowProjection"],
}


def load_rgb_u8(path: Path) -> np.ndarray:
    im = Image.open(path).convert("RGB")
    if im.size != (1920, 1080):
        im = im.resize((1920, 1080), Image.Resampling.LANCZOS)
    return np.asarray(im, dtype=np.uint8)


def read_gn_csv_metrics(csv_path: Path) -> dict:
    if not csv_path.is_file():
        return {}
    cols = {}
    with open(csv_path, "r", newline="") as fh:
        r = csv.DictReader(fh)
        for name in r.fieldnames or []:
            cols[name] = []
        for row in r:
            for name in r.fieldnames or []:
                v = row.get(name, "")
                cols[name].append(float(v) if v not in ("", None) else None)
    n = len(cols.get("frame", []))
    if n == 0:
        return {}

    def group_p50(select_fn):
        pcols = [c for c in cols.keys() if c not in ("frame", "wall_ms") and select_fn(c)]
        if not pcols:
            return None
        totals = []
        for i in range(n):
            vals = [cols[c][i] for c in pcols]
            if not all(v is None for v in vals):
                totals.append(sum(v or 0.0 for v in vals))
        return round(statistics.median(totals), 3) if totals else None

    return {
        "whole_frame_ms": group_p50(lambda c: True),
        "nextgi_ms": group_p50(lambda c: (c.startswith("nextgi_") and c not in ("nextgi_2_ms", "nextgi_as_builds_ms")) or c == "skinned_refit_ms"),
        "nextgeo_cull_raster_ms": group_p50(lambda c: c in (
            "nextgeo_instance_cull_ms", "nextgeo_bvh_ms", "nextgeo_cluster_cull_ms",
            "nextgeo_raster_p1_ms", "nextgeo_hiz_ms", "nextgeo_phase2_cull_ms", "nextgeo_raster_p2_ms",
        )),
        "nextgeo_resolve_ms": group_p50(lambda c: c in ("nextgeo_stage_a_ms", "nextgeo_stage_b_ms")),
        "vsm_ms": group_p50(lambda c: c.startswith("vsm_") or "virtual_shadow" in c or c in ("nextgeo_shadow_cull_ms", "nextgeo_shadow_raster_ms")),
    }


def read_ue_csv_metrics(csv_path: Path, warmup: int = 120) -> dict:
    if not csv_path.is_file() or csv_path.stat().st_size < 500:
        return {}
    try:
        prof = read_csv_profile(csv_path)
    except Exception:
        return {}
    out = {}
    mapping = [
        ("whole_frame_ms", "whole_frame_gpu"),
        ("lumen_ms", "lumen_total"),
        ("nanite_vis_ms", "nanite_visbuffer"),
        ("nanite_basepass_ms", "nanite_basepass"),
        ("vsm_ms", "vsm_total"),
    ]
    for k, grp in mapping:
        avail = [c for c in UE_GROUPS[grp] if prof.has_column(c)]
        if not avail:
            out[k] = None
            continue
        try:
            # Take last 60 frames (post-warmup)
            n_total = len(prof.column(avail[0]))
            start = min(warmup, max(0, n_total - 60))
            count = max(1, n_total - start)
            series = group_ms(prof, avail, start, count)
            clean = [v for v in series if v is not None and not math.isnan(v)]
            out[k] = round(statistics.median(clean), 3) if clean else None
        except Exception:
            out[k] = None
    return out


def make_side_by_side(ue_u8: np.ndarray, gn_u8: np.ndarray, title: str, subtitle: str, out_path: Path):
    h, w, _ = ue_u8.shape
    tw, th = 960, 540
    im_ue = Image.fromarray(ue_u8).resize((tw, th), Image.Resampling.LANCZOS)
    im_gn = Image.fromarray(gn_u8).resize((tw, th), Image.Resampling.LANCZOS)
    header_h = 54
    gap = 6
    canvas = Image.new("RGB", (tw * 2 + gap, th + header_h), (18, 22, 30))
    canvas.paste(im_ue, (0, header_h))
    canvas.paste(im_gn, (tw + gap, header_h))
    draw = ImageDraw.Draw(canvas)
    try:
        font_b = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 18)
        font_s = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14)
    except Exception:
        font_b = ImageFont.load_default()
        font_s = ImageFont.load_default()
    draw.text((16, 8), title, fill=(240, 244, 250), font=font_b)
    draw.text((16, 31), subtitle, fill=(165, 178, 198), font=font_s)
    # Corner labels
    draw.rectangle([12, header_h + 12, 330, header_h + 40], fill=(12, 16, 24))
    draw.text((22, header_h + 17), "Unreal Engine 5.9 (Nanite + Lumen + VSM)", fill=(120, 200, 255), font=font_s)
    draw.rectangle([tw + gap + 12, header_h + 12, tw + gap + 335, header_h + 40], fill=(12, 16, 24))
    draw.text((tw + gap + 22, header_h + 17), "GodotNext (NextGeo + NextGI + VSM)", fill=(110, 235, 165), font=font_s)
    canvas.save(out_path, optimize=True)


def compute_visual_pair(ue_png: Path, gn_png: Path, diff_png: Path, sbs_png: Path, title: str, subtitle: str) -> dict:
    ue_u8 = load_rgb_u8(ue_png)
    gn_u8 = load_rgb_u8(gn_png)
    ue_f = ue_u8.astype(np.float32) / 255.0
    gn_f = gn_u8.astype(np.float32) / 255.0

    # FLIP scalar + magma map
    err_scalar, mean_flip, _ = flip_evaluator.evaluate(ue_f, gn_f, "LDR", inputsRGB=True, applyMagma=False, computeMeanError=True)
    err_magma, _, _ = flip_evaluator.evaluate(ue_f, gn_f, "LDR", inputsRGB=True, applyMagma=True, computeMeanError=False)
    magma_u8 = np.clip(np.round(np.asarray(err_magma, dtype=np.float32) * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(magma_u8, mode="RGB").save(diff_png, optimize=True)

    scalar_flat = np.asarray(err_scalar, dtype=np.float64).reshape(-1)
    flip_p95 = float(np.percentile(scalar_flat, 95.0))
    mae = float(np.mean(np.abs(ue_f - gn_f)))
    mse = float(np.mean((ue_f - gn_f) ** 2))
    psnr = float(10.0 * math.log10(1.0 / max(mse, 1e-10)))
    # Mean sRGB luminance
    ue_lum = float(np.mean(0.2126 * ue_f[:, :, 0] + 0.7152 * ue_f[:, :, 1] + 0.0722 * ue_f[:, :, 2]))
    gn_lum = float(np.mean(0.2126 * gn_f[:, :, 0] + 0.7152 * gn_f[:, :, 1] + 0.0722 * gn_f[:, :, 2]))

    make_side_by_side(ue_u8, gn_u8, title, f"{subtitle}  |  FLIP mean {float(mean_flip):.3f}  |  MAE {mae:.3f}  |  PSNR {psnr:.1f} dB", sbs_png)
    return {
        "flip_mean": round(float(mean_flip), 4),
        "flip_p95": round(flip_p95, 4),
        "mae": round(mae, 4),
        "psnr_db": round(psnr, 2),
        "ue_mean_lum": round(ue_lum, 4),
        "gn_mean_lum": round(gn_lum, 4),
    }


HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>Unreal Engine 5.9 vs. GodotNext — Visual &amp; Performance Comparison</title>
<style>
  :root {
    --bg: #0c0f16; --panel: #141925; --panel2: #1b2233; --border: #2a344a;
    --text: #e8eef8; --muted: #93a3bd; --ue: #58b4ff; --gn: #4ce59b; --warn: #f6c453;
  }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Inter, sans-serif; background: var(--bg); color: var(--text); }
  header { padding: 20px 28px; border-bottom: 1px solid var(--border); background: linear-gradient(180deg, #131926, #0c0f16); display: flex; flex-wrap: wrap; align-items: baseline; justify-content: space-between; gap: 12px; }
  header h1 { margin: 0; font-size: 20px; font-weight: 700; letter-spacing: -0.02em; }
  header .sub { color: var(--muted); font-size: 13px; }
  .badge { display: inline-block; padding: 3px 9px; border-radius: 999px; font-size: 11px; font-weight: 600; }
  .badge-ue { background: rgba(88,180,255,0.14); color: var(--ue); border: 1px solid rgba(88,180,255,0.35); }
  .badge-gn { background: rgba(76,229,155,0.14); color: var(--gn); border: 1px solid rgba(76,229,155,0.35); }
  .wrap { max-width: 1520px; margin: 0 auto; padding: 18px 24px 48px; }
  .tabs { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 14px; }
  .tab { background: var(--panel); color: var(--muted); border: 1px solid var(--border); padding: 9px 14px; border-radius: 8px; cursor: pointer; font-size: 13px; font-weight: 600; transition: all 0.15s; }
  .tab:hover { color: var(--text); border-color: #425275; }
  .tab.active { background: #1f2b42; color: #fff; border-color: var(--ue); box-shadow: 0 0 0 1px rgba(88,180,255,0.25); }
  .scene-head { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; gap: 12px; background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 14px 18px; margin-bottom: 14px; }
  .scene-head h2 { margin: 0 0 4px; font-size: 17px; }
  .scene-head p { margin: 0; color: var(--muted); font-size: 13px; }
  .controls { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }
  .seg { display: inline-flex; background: #0e131e; border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
  .seg button { background: transparent; border: 0; color: var(--muted); padding: 7px 12px; font-size: 12px; font-weight: 600; cursor: pointer; }
  .seg button.active { background: #24324d; color: #fff; }
  .viewer-box { position: relative; background: #07090d; border: 1px solid var(--border); border-radius: 10px; overflow: hidden; user-select: none; }
  .slider-stage { position: relative; width: 100%; aspect-ratio: 16 / 9; cursor: ew-resize; }
  .slider-stage img { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: contain; display: block; pointer-events: none; }
  .clip-layer { position: absolute; inset: 0; overflow: hidden; }
  .divider { position: absolute; top: 0; bottom: 0; width: 2px; background: #fff; box-shadow: 0 0 10px rgba(0,0,0,0.85); pointer-events: none; }
  .handle { position: absolute; top: 50%; transform: translate(-50%, -50%); width: 34px; height: 34px; border-radius: 50%; background: rgba(16,22,34,0.92); border: 2px solid #fff; color: #fff; display: flex; align-items: center; justify-content: center; font-size: 13px; font-weight: 700; box-shadow: 0 2px 10px rgba(0,0,0,0.7); }
  .corner-tag { position: absolute; top: 12px; padding: 5px 10px; border-radius: 6px; font-size: 12px; font-weight: 700; background: rgba(10,14,22,0.82); backdrop-filter: blur(4px); pointer-events: none; }
  .tag-left { left: 12px; color: var(--ue); border: 1px solid rgba(88,180,255,0.4); }
  .tag-right { right: 12px; color: var(--gn); border: 1px solid rgba(76,229,155,0.4); }
  .sbs-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; padding: 6px; }
  .sbs-cell { position: relative; }
  .sbs-cell img { width: 100%; display: block; border-radius: 6px; }
  .single-stage { position: relative; width: 100%; aspect-ratio: 16 / 9; }
  .single-stage img { width: 100%; height: 100%; object-fit: contain; display: block; }
  .metrics-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 12px; margin-top: 14px; }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 14px 16px; }
  .card h3 { margin: 0 0 10px; font-size: 13px; text-transform: uppercase; letter-spacing: 0.05em; color: var(--muted); }
  .kv { display: flex; justify-content: space-between; padding: 5px 0; border-bottom: 1px solid rgba(255,255,255,0.05); font-size: 13px; }
  .kv:last-child { border-bottom: 0; }
  .kv strong { font-variant-numeric: tabular-nums; }
  .ue-c { color: var(--ue); } .gn-c { color: var(--gn); } .warn-c { color: var(--warn); }
  .bar-row { margin: 7px 0; }
  .bar-lbl { display: flex; justify-content: space-between; font-size: 12px; color: var(--muted); margin-bottom: 3px; }
  .bar-track { height: 7px; background: #0c1018; border-radius: 4px; overflow: hidden; display: flex; gap: 2px; }
  .bar-ue { background: var(--ue); height: 100%; border-radius: 3px; }
  .bar-gn { background: var(--gn); height: 100%; border-radius: 3px; }
  .thumbs { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-top: 14px; }
  .thumb { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; cursor: pointer; transition: border-color 0.15s; }
  .thumb.active { border-color: var(--gn); box-shadow: 0 0 0 1px rgba(76,229,155,0.35); }
  .thumb img { width: 100%; aspect-ratio: 32 / 9; object-fit: cover; display: block; }
  .thumb .cap { padding: 8px 10px; font-size: 12px; display: flex; justify-content: space-between; align-items: center; }
  .hint { color: var(--muted); font-size: 12px; margin-top: 8px; }
  @media (max-width: 900px) { .sbs-grid, .thumbs { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<header>
  <div>
    <h1>Unreal Engine 5.9 vs. GodotNext — Side-by-Side Comparison</h1>
    <div class="sub">NVIDIA GeForce RTX 5090 &middot; Linux Vulkan SM6 &middot; 1920&times;1080 &middot; Matched Cameras, Sun Vector, Sky &amp; ACES Tonemapping</div>
  </div>
  <div style="display:flex; gap:8px;">
    <span class="badge badge-ue">UE 5.9: Nanite + Lumen HWRT High + VSM</span>
    <span class="badge badge-gn">GodotNext: NextGeo + NextGI HWRT High + VSM</span>
  </div>
</header>
<div class="wrap">
  <div id="sceneTabs" class="tabs"></div>
  <div class="scene-head">
    <div>
      <h2 id="sceneTitle"></h2>
      <p id="sceneDesc"></p>
    </div>
    <div class="controls">
      <div class="seg" id="camSeg"></div>
      <div class="seg" id="modeSeg">
        <button data-mode="slider" class="active">Wipe Slider</button>
        <button data-mode="sbs">Side-by-Side</button>
        <button data-mode="ue">UE 5.9 Only</button>
        <button data-mode="gn">GodotNext Only</button>
        <button data-mode="diff">FLIP Diff</button>
      </div>
    </div>
  </div>
  <div id="viewerBox" class="viewer-box"></div>
  <div class="hint">Tip: Drag across the Wipe Slider (or use keyboard <code>1-5</code> for scenes, <code>A/B/C</code> for angles, <code>Space</code> to blink UE 5.9 / GodotNext).</div>
  <div id="thumbs" class="thumbs"></div>
  <div id="metricsGrid" class="metrics-grid"></div>
</div>

<script>
const DATA = __COMPARISON_DATA_JSON__;

let curScene = 0;
let curCam = 0;
let curMode = 'slider';
let splitPct = 50;
let blinkShowGN = false;

function fmtMs(v) { return (v === null || v === undefined) ? '—' : v.toFixed(2) + ' ms'; }
function fmtRatio(g, u) {
  if (g == null || u == null || u <= 0) return '—';
  return (g / u).toFixed(2) + '×';
}

function renderTabs() {
  const el = document.getElementById('sceneTabs');
  el.innerHTML = '';
  DATA.scenes.forEach((s, idx) => {
    const b = document.createElement('button');
    b.className = 'tab' + (idx === curScene ? ' active' : '');
    b.textContent = `${idx + 1}. ${s.short_title}`;
    b.onclick = () => { curScene = idx; curCam = 0; renderAll(); };
    el.appendChild(b);
  });
}

function renderCamSeg(scene) {
  const el = document.getElementById('camSeg');
  el.innerHTML = '';
  scene.cameras.forEach((c, idx) => {
    const b = document.createElement('button');
    b.className = idx === curCam ? 'active' : '';
    b.textContent = c.label;
    b.onclick = () => { curCam = idx; renderAll(); };
    el.appendChild(b);
  });
}

function renderViewer(cam) {
  const box = document.getElementById('viewerBox');
  if (curMode === 'slider') {
    box.innerHTML = `
      <div class="slider-stage" id="sliderStage">
        <img src="${cam.gn_img}" alt="GodotNext" />
        <div class="clip-layer" id="clipLayer" style="clip-path: inset(0 ${100 - splitPct}% 0 0);">
          <img src="${cam.ue_img}" alt="Unreal Engine 5.9" />
        </div>
        <div class="divider" id="divider" style="left:${splitPct}%;">
          <div class="handle">&#8596;</div>
        </div>
        <div class="corner-tag tag-left">UE 5.9 &middot; Nanite + Lumen HWRT</div>
        <div class="corner-tag tag-right">GodotNext &middot; NextGeo + NextGI HWRT</div>
      </div>`;
    const stage = document.getElementById('sliderStage');
    let dragging = false;
    const update = (clientX) => {
      const r = stage.getBoundingClientRect();
      splitPct = Math.max(0, Math.min(100, ((clientX - r.left) / r.width) * 100));
      document.getElementById('clipLayer').style.clipPath = `inset(0 ${100 - splitPct}% 0 0)`;
      document.getElementById('divider').style.left = `${splitPct}%`;
    };
    stage.onpointerdown = (e) => { dragging = true; stage.setPointerCapture(e.pointerId); update(e.clientX); };
    stage.onpointermove = (e) => { if (dragging) update(e.clientX); };
    stage.onpointerup = () => { dragging = false; };
  } else if (curMode === 'sbs') {
    box.innerHTML = `
      <div class="sbs-grid">
        <div class="sbs-cell">
          <img src="${cam.ue_img}" alt="UE 5.9" />
          <div class="corner-tag tag-left">UE 5.9 &middot; Nanite + Lumen HWRT</div>
        </div>
        <div class="sbs-cell">
          <img src="${cam.gn_img}" alt="GodotNext" />
          <div class="corner-tag tag-right">GodotNext &middot; NextGeo + NextGI HWRT</div>
        </div>
      </div>`;
  } else if (curMode === 'ue' || curMode === 'gn') {
    const isUE = curMode === 'ue';
    box.innerHTML = `
      <div class="single-stage">
        <img src="${isUE ? cam.ue_img : cam.gn_img}" alt="${isUE ? 'UE 5.9' : 'GodotNext'}" />
        <div class="corner-tag ${isUE ? 'tag-left' : 'tag-right'}">${isUE ? 'UE 5.9 &middot; Nanite + Lumen HWRT' : 'GodotNext &middot; NextGeo + NextGI HWRT'}</div>
      </div>`;
  } else if (curMode === 'diff') {
    box.innerHTML = `
      <div class="single-stage">
        <img src="${cam.diff_img}" alt="FLIP Perceptual Diff" />
        <div class="corner-tag tag-left">FLIP Perceptual Error Map (Magma: black=0, yellow=1.0) &middot; Mean ${cam.visual.flip_mean.toFixed(3)}</div>
      </div>`;
  }
}

function renderThumbs(scene) {
  const el = document.getElementById('thumbs');
  el.innerHTML = '';
  scene.cameras.forEach((c, idx) => {
    const d = document.createElement('div');
    d.className = 'thumb' + (idx === curCam ? ' active' : '');
    d.innerHTML = `<img src="${c.sbs_img}" alt="${c.label}" /><div class="cap"><strong>${c.label}</strong><span class="muted">FLIP ${c.visual.flip_mean.toFixed(3)}</span></div>`;
    d.onclick = () => { curCam = idx; renderAll(); };
    el.appendChild(d);
  });
}

function renderMetrics(scene, cam) {
  const g = cam.gn_timing || {};
  const u = cam.ue_timing || {};
  const v = cam.visual || {};
  const maxBar = Math.max(g.whole_frame_ms || 1, u.whole_frame_ms || 1, 1);
  const pct = (x) => Math.min(100, Math.max(1, ((x || 0) / maxBar) * 100)).toFixed(1);

  document.getElementById('metricsGrid').innerHTML = `
    <div class="card">
      <h3>Visual Comparison Metrics (${cam.label})</h3>
      <div class="kv"><span>Perceptual FLIP (Mean)</span><strong>${v.flip_mean != null ? v.flip_mean.toFixed(4) : '—'}</strong></div>
      <div class="kv"><span>Perceptual FLIP (p95)</span><strong>${v.flip_p95 != null ? v.flip_p95.toFixed(4) : '—'}</strong></div>
      <div class="kv"><span>RGB MAE / PSNR</span><strong>${v.mae != null ? v.mae.toFixed(4) : '—'} / ${v.psnr_db != null ? v.psnr_db.toFixed(1) + ' dB' : '—'}</strong></div>
      <div class="kv"><span>Mean sRGB Luminance (<span class="ue-c">UE</span> / <span class="gn-c">GN</span>)</span><strong>${v.ue_mean_lum != null ? v.ue_mean_lum.toFixed(3) : '—'} / ${v.gn_mean_lum != null ? v.gn_mean_lum.toFixed(3) : '—'}</strong></div>
    </div>
    <div class="card">
      <h3>GPU Frame Timing (1080p p50, RTX 5090)</h3>
      <div class="bar-row">
        <div class="bar-lbl"><span>Whole Frame GPU (<span class="ue-c">UE ${fmtMs(u.whole_frame_ms)}</span> vs <span class="gn-c">GN ${fmtMs(g.whole_frame_ms)}</span>)</span><strong>${fmtRatio(g.whole_frame_ms, u.whole_frame_ms)}</strong></div>
        <div class="bar-track"><div class="bar-ue" style="width:${pct(u.whole_frame_ms)}%"></div></div>
        <div class="bar-track" style="margin-top:2px;"><div class="bar-gn" style="width:${pct(g.whole_frame_ms)}%"></div></div>
      </div>
      <div class="kv"><span>GI (<span class="ue-c">Lumen</span> / <span class="gn-c">NextGI</span>)</span><strong><span class="ue-c">${fmtMs(u.lumen_ms)}</span> / <span class="gn-c">${fmtMs(g.nextgi_ms)}</span> (${fmtRatio(g.nextgi_ms, u.lumen_ms)})</strong></div>
      <div class="kv"><span>Virtual Geo (<span class="ue-c">Nanite Vis</span> / <span class="gn-c">NextGeo Cull+Rast</span>)</span><strong><span class="ue-c">${fmtMs(u.nanite_vis_ms)}</span> / <span class="gn-c">${fmtMs(g.nextgeo_cull_raster_ms)}</span> (${fmtRatio(g.nextgeo_cull_raster_ms, u.nanite_vis_ms)})</strong></div>
      <div class="kv"><span>Shadows (<span class="ue-c">UE VSM</span> / <span class="gn-c">GN VSM</span>)</span><strong><span class="ue-c">${fmtMs(u.vsm_ms)}</span> / <span class="gn-c">${fmtMs(g.vsm_ms)}</span> (${fmtRatio(g.vsm_ms, u.vsm_ms)})</strong></div>
    </div>
    <div class="card">
      <h3>Scene &amp; Geometry Summary</h3>
      <div class="kv"><span>Geometry Source</span><strong>${scene.source_label}</strong></div>
      <div class="kv"><span>Triangle Count</span><strong>${scene.tris_label}</strong></div>
      <div class="kv"><span>Virtual Geometry</span><strong>${scene.vg_label}</strong></div>
      <div class="kv"><span>Notes</span><strong style="text-align:right;font-weight:500;color:var(--muted);">${scene.notes}</strong></div>
    </div>`;
}

function renderAll() {
  renderTabs();
  const scene = DATA.scenes[curScene];
  const cam = scene.cameras[curCam];
  document.getElementById('sceneTitle').textContent = scene.title;
  document.getElementById('sceneDesc').textContent = scene.description;
  renderCamSeg(scene);
  document.querySelectorAll('#modeSeg button').forEach(b => {
    b.classList.toggle('active', b.dataset.mode === curMode);
    b.onclick = () => { curMode = b.dataset.mode; renderAll(); };
  });
  renderViewer(cam);
  renderThumbs(scene);
  renderMetrics(scene, cam);
}

window.addEventListener('keydown', (e) => {
  if (e.key >= '1' && e.key <= String(DATA.scenes.length)) { curScene = Number(e.key) - 1; curCam = 0; renderAll(); }
  else if (e.key.toLowerCase() === 'a') { curCam = 0; renderAll(); }
  else if (e.key.toLowerCase() === 'b' && DATA.scenes[curScene].cameras.length > 1) { curCam = 1; renderAll(); }
  else if (e.key.toLowerCase() === 'c' && DATA.scenes[curScene].cameras.length > 2) { curCam = 2; renderAll(); }
  else if (e.code === 'Space') { e.preventDefault(); blinkShowGN = !blinkShowGN; curMode = blinkShowGN ? 'gn' : 'ue'; renderAll(); }
});

renderAll();
</script>
</body>
</html>
"""


def load_specs(name: str) -> dict:
    """Read a scene spec file kept next to this script. Its paths are relative to TOOLS_DIR; absolute ones are used as they are."""
    specs = json.loads((SCRIPT_DIR / name).read_text())
    for sc in specs["scenes"]:
        for key in ("ue_project", "ue_sky_hdri"):
            if sc.get(key):
                sc[key] = str(TOOLS_DIR / sc[key])
    return specs


def main():
    specs = load_specs("scene_specs.json")
    viewer_dir = COMP_ROOT / "viewer"
    img_dir = viewer_dir / "images"
    img_dir.mkdir(parents=True, exist_ok=True)

    out_scenes = []
    for sc in specs["scenes"]:
        sid = sc["id"]
        s_out = COMP_ROOT / sid
        gn_sum_p = s_out / "godotnext_summary.json"
        gn_sum = json.loads(gn_sum_p.read_text()) if gn_sum_p.is_file() else {}
        ms = gn_sum.get("mesh_stats", {})

        cam_entries = []
        for cam in sc["cameras"]:
            cid = cam["id"]
            ue_png = s_out / f"{cid}_ue59.png"
            gn_png = s_out / f"{cid}_godotnext.png"
            if not ue_png.is_file() or not gn_png.is_file():
                print(f"WARN missing pair for {sid}/{cid}: ue={ue_png.is_file()} gn={gn_png.is_file()}")
                continue
            ue_rel = f"images/{sid}_{cid}_ue59.png"
            gn_rel = f"images/{sid}_{cid}_godotnext.png"
            diff_rel = f"images/{sid}_{cid}_flip.png"
            sbs_rel = f"images/{sid}_{cid}_sbs.png"
            shutil.copy2(ue_png, viewer_dir / ue_rel)
            shutil.copy2(gn_png, viewer_dir / gn_rel)
            vis = compute_visual_pair(
                ue_png, gn_png,
                viewer_dir / diff_rel,
                viewer_dir / sbs_rel,
                f"{sc['short_title']} — {cam['label']}",
                f"1920x1080 Vulkan RTX 5090",
            )
            gn_t = read_gn_csv_metrics(s_out / f"{cid}_timing.csv")
            ue_t = read_ue_csv_metrics(s_out / f"{cid}_ue_profile.csv")
            cam_entries.append({
                "id": cid,
                "label": cam["label"],
                "ue_img": ue_rel,
                "gn_img": gn_rel,
                "diff_img": diff_rel,
                "sbs_img": sbs_rel,
                "visual": vis,
                "gn_timing": gn_t,
                "ue_timing": ue_t,
            })
            print(f"  {sid}/{cid}: FLIP={vis['flip_mean']:.4f} MAE={vis['mae']:.4f} lum(UE/GN)={vis['ue_mean_lum']:.3f}/{vis['gn_mean_lum']:.3f} GPU(UE/GN)={ue_t.get('whole_frame_ms')}/{gn_t.get('whole_frame_ms')} ms")

        if cam_entries:
            tris = ms.get("tris", sc.get("approx_tris", 0))
            out_scenes.append({
                "id": sid,
                "short_title": sc["short_title"],
                "title": sc["title"],
                "description": sc["description"],
                "source_label": sc["source_label"],
                "tris_label": f"{tris:,} tris" if tris else sc.get("tris_label", "—"),
                "vg_label": f"{ms.get('vg_meshes', '?')}/{ms.get('meshes', '?')} NextGeo VG meshes" if ms else "NextGeo VG + UE Nanite",
                "notes": sc.get("notes", ""),
                "cameras": cam_entries,
            })

    payload = {"scenes": out_scenes}
    (viewer_dir / "metrics.json").write_text(json.dumps(payload, indent=2))
    html = HTML_TEMPLATE.replace("__COMPARISON_DATA_JSON__", json.dumps(payload))
    (viewer_dir / "index.html").write_text(html)

    # Copy cleanly to $GODOTNEXT_DROP_DIR/godotnext-ue59-comparison-v3/
    if DROP_DIR.exists():
        shutil.rmtree(DROP_DIR)
    shutil.copytree(viewer_dir, DROP_DIR)
    print(f"Built viewer with {len(out_scenes)} scenes ({sum(len(s['cameras']) for s in out_scenes)} pairs) -> {DROP_DIR}")


if __name__ == "__main__":
    main()
