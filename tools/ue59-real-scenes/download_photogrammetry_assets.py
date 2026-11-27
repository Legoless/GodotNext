#!/usr/bin/env python3
"""Download real CC0 Poly Haven 4K photogrammetry models, 4K PBR textures, and 8K HDRIs."""
import concurrent.futures
import json
import os
import urllib.request
from pathlib import Path

ROOT = Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/photogrammetry_sources")
HDR = {"User-Agent": "GodotNextCompare/1.0"}

MODELS_4K = [
    # Red desert Namaqualand photogrammetry scans
    "namaqualand_cliff_01",
    "namaqualand_cliff_02",
    "namaqualand_boulders_01",
    "namaqualand_boulder_02",
    "namaqualand_boulder_03",
    "namaqualand_boulder_04",
    "namaqualand_boulder_05",
    "namaqualand_boulder_06",
    "namaqualand_rocks_01",
    "namaqualand_stones_01",
    "wild_rooibos_bush",
    "dead_quiver_branch_01",
    "dead_quiver_branch_02",
    "dead_quiver_trunk",
    "bark_debris_01",
    # Massive cliff & jagged mountain photogrammetry scans + ancient ruins
    "mountainside",
    "coastal_cliff_02",
    "coastal_cliff_04",
    "rock_face_01",
    "rock_face_02",
    "modular_fort_01",
    "stone_fire_pit",
]

TEXTURES_4K = [
    "terrain_red_01",
    "red_laterite_soil_stones",
    "sandy_gravel_02",
    "red_sandstone_wall",
    "cliff_side",
    "brown_leather",
    "fabric_leather_02",
    "rough_linen",
    "hessian_230",
    "large_sandstone_blocks_01",
]

HDRIS_8K = [
    "belfast_sunset_puresky",
    "drakensberg_solitary_mountain_puresky",
]


def get_json(url: str) -> dict:
    req = urllib.request.Request(url, headers=HDR)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def download_file(url: str, dst: Path) -> str:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_file() and dst.stat().st_size > 100:
        return f"cached {dst.name}"
    tmp = dst.with_suffix(dst.suffix + ".part")
    req = urllib.request.Request(url, headers=HDR)
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 18)
            if not chunk:
                break
            f.write(chunk)
    tmp.replace(dst)
    return f"ok {dst.name} ({dst.stat().st_size / (1024*1024):.1f} MB)"


def collect_tasks() -> list[tuple[str, Path]]:
    tasks = []

    # 1. Models (4k glTF + included bin & textures; fallback to 2k if 4k missing)
    for m in MODELS_4K:
        data = get_json(f"https://api.polyhaven.com/files/{m}")
        gltf_block = data.get("gltf", {})
        res = "4k" if "4k" in gltf_block else ("2k" if "2k" in gltf_block else next(iter(gltf_block.keys())))
        entry = gltf_block[res]["gltf"]
        mdir = ROOT / "models" / m
        tasks.append((entry["url"], mdir / f"{m}_{res}.gltf"))
        for rel_path, inc in entry.get("include", {}).items():
            tasks.append((inc["url"], mdir / rel_path))

    # 2. PBR Textures (4k Diffuse, Rough, nor_gl, disp, AO)
    for t in TEXTURES_4K:
        data = get_json(f"https://api.polyhaven.com/files/{t}")
        tdir = ROOT / "textures" / t
        for chan in ("Diffuse", "nor_gl", "Rough", "AO", "Displacement"):
            ch_block = data.get(chan, {})
            if not ch_block:
                continue
            res = "4k" if "4k" in ch_block else ("2k" if "2k" in ch_block else next(iter(ch_block.keys())))
            fmts = ch_block[res]
            fmt = "jpg" if (chan == "Diffuse" and "jpg" in fmts) else ("png" if "png" in fmts else ("jpg" if "jpg" in fmts else next(iter(fmts.keys()))))
            url = fmts[fmt]["url"]
            ext = Path(url.split("?")[0]).suffix or f".{fmt}"
            tasks.append((url, tdir / f"{t}_{chan}_{res}{ext}"))

    # 3. 8K HDRIs
    for h in HDRIS_8K:
        data = get_json(f"https://api.polyhaven.com/files/{h}")
        hblock = data["hdri"]
        res = "8k" if "8k" in hblock else "4k"
        url = hblock[res]["hdr"]["url"]
        tasks.append((url, ROOT / "hdris" / f"{h}_{res}.hdr"))

    return tasks


def main():
    print("Querying Poly Haven file manifests...", flush=True)
    tasks = collect_tasks()
    print(f"Downloading {len(tasks)} files with 12 threads...", flush=True)
    ok = 0
    fail = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as ex:
        futs = {ex.submit(download_file, u, p): (u, p) for u, p in tasks}
        for f in concurrent.futures.as_completed(futs):
            u, p = futs[f]
            try:
                msg = f.result()
                ok += 1
                if ok % 15 == 0 or not msg.startswith("cached"):
                    print(f"  [{ok}/{len(tasks)}] {msg}", flush=True)
            except Exception as e:
                fail += 1
                print(f"  FAIL {p}: {e}", flush=True)
    print(f"Done: ok={ok} fail={fail}", flush=True)
    if fail:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
